/*
 * Concurrent-C TLS Runtime — BearSSL client/server.
 *
 * Server: load PEM cert chain + RSA/EC key, wrap an accepted socket,
 * handshake (blocking I/O on the fd for the handshake), then app
 * read/write via the engine with non-blocking-friendly pumps.
 */
#include <ccc/std/tls.cch>
#include <ccc/std/net.cch>

#undef cc_tls_connect
#undef cc_tls_connect_addr
#undef cc_tls_accept
#undef cc_tls_read
#undef cc_tls_write
#undef cc_tls_shutdown
#undef cc_tls_close
#undef cc_tls_load_cert_chain
#undef cc_tls_load_private_key
#undef cc_tls_load_trust_anchors

#ifdef CC_HAS_BEARSSL
#include <bearssl.h>
#endif

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/select.h>

#define CC__TLS_F_SERVER  0x01
#define CC__TLS_F_READY   0x02
#define CC__TLS_F_CLOSED  0x04

#ifdef CC_HAS_BEARSSL

/* ---- process-wide server materials (cert chain + key) ---- */

typedef struct CC__TlsPemBlob {
    unsigned char *data;
    size_t len;
} CC__TlsPemBlob;

typedef struct CC__TlsServerMats {
    int ready;
    br_x509_certificate *chain;
    size_t chain_len;
    int key_type; /* BR_KEYTYPE_RSA or BR_KEYTYPE_EC */
    br_rsa_private_key rsa;
    br_ec_private_key ec;
    /* Owned key component buffers */
    unsigned char *key_bufs[8];
    size_t nkey_bufs;
    unsigned char *chain_bufs[16];
    size_t nchain_bufs;
} CC__TlsServerMats;

static CC__TlsServerMats cc__tls_server_mats;

typedef struct CC__TlsCtx {
    int is_server;
    union {
        br_ssl_client_context client;
        br_ssl_server_context server;
    } u;
    br_sslio_context ioc;
    br_ssl_engine_context *eng;
} CC__TlsCtx;

/* Low-level fd I/O for br_sslio (blocking or ready fd). */
static int cc__tls_fd_read(void *ctx, unsigned char *buf, size_t len) {
    int fd = *(int *)ctx;
    for (;;) {
        ssize_t r = read(fd, buf, len);
        if (r < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                errno = EAGAIN;
                return -1;
            }
            return -1;
        }
        return (int)r;
    }
}

static int cc__tls_fd_write(void *ctx, const unsigned char *buf, size_t len) {
    int fd = *(int *)ctx;
    for (;;) {
        ssize_t r = write(fd, buf, len);
        if (r < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                errno = EAGAIN;
                return -1;
            }
            return -1;
        }
        return (int)r;
    }
}

static int cc__tls_set_blocking(int fd, int block) {
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl < 0) return -1;
    if (block) fl &= ~O_NONBLOCK;
    else fl |= O_NONBLOCK;
    return fcntl(fd, F_SETFL, fl);
}

static unsigned char *cc__tls_read_file(const char *path, size_t *out_len) {
    FILE *f;
    unsigned char *buf = NULL;
    size_t cap = 0, n = 0;
    *out_len = 0;
    f = fopen(path, "rb");
    if (!f) return NULL;
    for (;;) {
        unsigned char tmp[4096];
        size_t r = fread(tmp, 1, sizeof(tmp), f);
        if (r == 0) break;
        if (n + r + 1 > cap) {
            size_t ncap = cap ? cap * 2 : 8192;
            unsigned char *nb;
            while (ncap < n + r + 1) ncap *= 2;
            nb = (unsigned char *)realloc(buf, ncap);
            if (!nb) {
                free(buf);
                fclose(f);
                return NULL;
            }
            buf = nb;
            cap = ncap;
        }
        memcpy(buf + n, tmp, r);
        n += r;
    }
    fclose(f);
    if (!buf) {
        buf = (unsigned char *)malloc(1);
        if (!buf) return NULL;
    }
    buf[n] = 0;
    *out_len = n;
    return buf;
}

typedef struct {
    unsigned char *data;
    size_t len;
    size_t cap;
} CC__TlsBuf;

static void cc__tls_buf_append(void *ctx, const void *data, size_t len) {
    CC__TlsBuf *b = (CC__TlsBuf *)ctx;
    if (b->len + len > b->cap) {
        size_t ncap = b->cap ? b->cap * 2 : 256;
        unsigned char *nb;
        while (ncap < b->len + len) ncap *= 2;
        nb = (unsigned char *)realloc(b->data, ncap);
        if (!nb) return;
        b->data = nb;
        b->cap = ncap;
    }
    memcpy(b->data + b->len, data, len);
    b->len += len;
}

static int cc__tls_pem_push_certs(const unsigned char *src, size_t len,
                                  CC__TlsServerMats *m) {
    br_pem_decoder_context pc;
    CC__TlsBuf bv = {0};
    int inobj = 0;
    int extra_nl = 1;
    const unsigned char *buf = src;
    size_t left = len;
    char name[128];

    br_pem_decoder_init(&pc);
    name[0] = 0;
    while (left > 0 || extra_nl) {
        size_t tlen;
        if (left == 0 && extra_nl) {
            extra_nl = 0;
            buf = (const unsigned char *)"\n";
            left = 1;
        }
        tlen = br_pem_decoder_push(&pc, buf, left);
        buf += tlen;
        left -= tlen;
        switch (br_pem_decoder_event(&pc)) {
        case BR_PEM_BEGIN_OBJ: {
            const char *n = br_pem_decoder_name(&pc);
            size_t nl = n ? strlen(n) : 0;
            if (nl >= sizeof(name)) nl = sizeof(name) - 1;
            memcpy(name, n ? n : "", nl);
            name[nl] = 0;
            bv.len = 0;
            br_pem_decoder_setdest(&pc, cc__tls_buf_append, &bv);
            inobj = 1;
            break;
        }
        case BR_PEM_END_OBJ:
            if (inobj && (strcmp(name, "CERTIFICATE") == 0
                          || strcmp(name, "X509 CERTIFICATE") == 0)) {
                unsigned char *copy;
                br_x509_certificate *neu;
                if (m->chain_len >= 16) {
                    free(bv.data);
                    return -1;
                }
                copy = (unsigned char *)malloc(bv.len ? bv.len : 1);
                if (!copy) {
                    free(bv.data);
                    return -1;
                }
                if (bv.len) memcpy(copy, bv.data, bv.len);
                m->chain_bufs[m->nchain_bufs++] = copy;
                neu = (br_x509_certificate *)realloc(
                    m->chain, (m->chain_len + 1) * sizeof(*m->chain));
                if (!neu) {
                    free(bv.data);
                    return -1;
                }
                m->chain = neu;
                m->chain[m->chain_len].data = copy;
                m->chain[m->chain_len].data_len = bv.len;
                m->chain_len++;
            }
            inobj = 0;
            bv.len = 0;
            break;
        case BR_PEM_ERROR:
            free(bv.data);
            return -1;
        default:
            break;
        }
    }
    free(bv.data);
    return m->chain_len > 0 ? 0 : -1;
}

static int cc__tls_pem_push_key(const unsigned char *src, size_t len,
                                CC__TlsServerMats *m) {
    br_pem_decoder_context pc;
    CC__TlsBuf bv = {0};
    int inobj = 0;
    int extra_nl = 1;
    const unsigned char *buf = src;
    size_t left = len;
    char name[128];
    br_skey_decoder_context dc;
    int err;

    br_pem_decoder_init(&pc);
    name[0] = 0;
    while (left > 0 || extra_nl) {
        size_t tlen;
        if (left == 0 && extra_nl) {
            extra_nl = 0;
            buf = (const unsigned char *)"\n";
            left = 1;
        }
        tlen = br_pem_decoder_push(&pc, buf, left);
        buf += tlen;
        left -= tlen;
        switch (br_pem_decoder_event(&pc)) {
        case BR_PEM_BEGIN_OBJ: {
            const char *n = br_pem_decoder_name(&pc);
            size_t nl = n ? strlen(n) : 0;
            if (nl >= sizeof(name)) nl = sizeof(name) - 1;
            memcpy(name, n ? n : "", nl);
            name[nl] = 0;
            bv.len = 0;
            br_pem_decoder_setdest(&pc, cc__tls_buf_append, &bv);
            inobj = 1;
            break;
        }
        case BR_PEM_END_OBJ:
            if (inobj && (strcmp(name, "RSA PRIVATE KEY") == 0
                          || strcmp(name, "EC PRIVATE KEY") == 0
                          || strcmp(name, "PRIVATE KEY") == 0)) {
                br_skey_decoder_init(&dc);
                br_skey_decoder_push(&dc, bv.data, bv.len);
                err = br_skey_decoder_last_error(&dc);
                if (err) {
                    free(bv.data);
                    return -1;
                }
                m->key_type = br_skey_decoder_key_type(&dc);
                if (m->key_type == BR_KEYTYPE_RSA) {
                    const br_rsa_private_key *sk = br_skey_decoder_get_rsa(&dc);
                    unsigned char *p, *q, *dp, *dq, *iq;
                    p = (unsigned char *)malloc(sk->plen);
                    q = (unsigned char *)malloc(sk->qlen);
                    dp = (unsigned char *)malloc(sk->dplen);
                    dq = (unsigned char *)malloc(sk->dqlen);
                    iq = (unsigned char *)malloc(sk->iqlen);
                    if (!p || !q || !dp || !dq || !iq) {
                        free(p); free(q); free(dp); free(dq); free(iq);
                        free(bv.data);
                        return -1;
                    }
                    memcpy(p, sk->p, sk->plen);
                    memcpy(q, sk->q, sk->qlen);
                    memcpy(dp, sk->dp, sk->dplen);
                    memcpy(dq, sk->dq, sk->dqlen);
                    memcpy(iq, sk->iq, sk->iqlen);
                    m->rsa = *sk;
                    m->rsa.p = p;
                    m->rsa.q = q;
                    m->rsa.dp = dp;
                    m->rsa.dq = dq;
                    m->rsa.iq = iq;
                    m->key_bufs[m->nkey_bufs++] = p;
                    m->key_bufs[m->nkey_bufs++] = q;
                    m->key_bufs[m->nkey_bufs++] = dp;
                    m->key_bufs[m->nkey_bufs++] = dq;
                    m->key_bufs[m->nkey_bufs++] = iq;
                } else if (m->key_type == BR_KEYTYPE_EC) {
                    const br_ec_private_key *sk = br_skey_decoder_get_ec(&dc);
                    unsigned char *x = (unsigned char *)malloc(sk->xlen);
                    if (!x) {
                        free(bv.data);
                        return -1;
                    }
                    memcpy(x, sk->x, sk->xlen);
                    m->ec = *sk;
                    m->ec.x = x;
                    m->key_bufs[m->nkey_bufs++] = x;
                } else {
                    free(bv.data);
                    return -1;
                }
                free(bv.data);
                return 0;
            }
            inobj = 0;
            bv.len = 0;
            break;
        case BR_PEM_ERROR:
            free(bv.data);
            return -1;
        default:
            break;
        }
    }
    free(bv.data);
    return -1;
}

static void cc__tls_mats_clear(CC__TlsServerMats *m) {
    size_t i;
    if (!m) return;
    for (i = 0; i < m->nchain_bufs; i++) free(m->chain_bufs[i]);
    for (i = 0; i < m->nkey_bufs; i++) free(m->key_bufs[i]);
    free(m->chain);
    memset(m, 0, sizeof(*m));
}

/* Load cert+key paths into process mats. Returns 0 on success. */
int cc_tls_server_load(const char *cert_path, const char *key_path) {
    unsigned char *cbuf = NULL, *kbuf = NULL;
    size_t clen = 0, klen = 0;
    cc__tls_mats_clear(&cc__tls_server_mats);
    if (!cert_path || !key_path) return -1;
    cbuf = cc__tls_read_file(cert_path, &clen);
    kbuf = cc__tls_read_file(key_path, &klen);
    if (!cbuf || !kbuf) {
        free(cbuf);
        free(kbuf);
        return -1;
    }
    if (cc__tls_pem_push_certs(cbuf, clen, &cc__tls_server_mats) != 0) {
        free(cbuf);
        free(kbuf);
        cc__tls_mats_clear(&cc__tls_server_mats);
        return -1;
    }
    if (cc__tls_pem_push_key(kbuf, klen, &cc__tls_server_mats) != 0) {
        free(cbuf);
        free(kbuf);
        cc__tls_mats_clear(&cc__tls_server_mats);
        return -1;
    }
    free(cbuf);
    free(kbuf);
    cc__tls_server_mats.ready = 1;
    return 0;
}

void cc_tls_server_unload(void) {
    cc__tls_mats_clear(&cc__tls_server_mats);
}

static br_ssl_engine_context *cc__tls_eng(CCTlsConn *conn) {
    CC__TlsCtx *c;
    if (!conn || !conn->ctx) return NULL;
    c = (CC__TlsCtx *)conn->ctx;
    return c->eng;
}

/* Pump record layer once. Returns 1 if progress, 0 if would-block, -1 error. */
static int cc__tls_pump_rec(CCTlsConn *conn) {
    br_ssl_engine_context *eng = cc__tls_eng(conn);
    unsigned st;
    int fd;
    if (!eng) return -1;
    fd = conn->underlying.fd;
    st = br_ssl_engine_current_state(eng);
    if (st & BR_SSL_CLOSED) {
        int err = br_ssl_engine_last_error(eng);
        return err == BR_ERR_OK ? 0 : -1;
    }
    if (st & BR_SSL_SENDREC) {
        size_t len;
        unsigned char *buf = br_ssl_engine_sendrec_buf(eng, &len);
        int w;
        if (!buf || !len) return 0;
        w = cc__tls_fd_write(&fd, buf, len);
        if (w < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
            return -1;
        }
        if (w == 0) return -1;
        br_ssl_engine_sendrec_ack(eng, (size_t)w);
        return 1;
    }
    if (st & BR_SSL_RECVREC) {
        size_t len;
        unsigned char *buf = br_ssl_engine_recvrec_buf(eng, &len);
        int r;
        if (!buf || !len) return 0;
        r = cc__tls_fd_read(&fd, buf, len);
        if (r < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
            return -1;
        }
        if (r == 0) {
            br_ssl_engine_close(eng);
            return -1;
        }
        br_ssl_engine_recvrec_ack(eng, (size_t)r);
        return 1;
    }
    return 0;
}

static int cc__tls_handshake(CCTlsConn *conn) {
    br_ssl_engine_context *eng = cc__tls_eng(conn);
    int spins = 0;
    if (!eng) return -1;
    for (;;) {
        unsigned st = br_ssl_engine_current_state(eng);
        if (st & BR_SSL_CLOSED) {
            return br_ssl_engine_last_error(eng) == BR_ERR_OK ? 0 : -1;
        }
        /* Application can send/recv ⇒ handshake done. */
        if ((st & BR_SSL_SENDAPP) || (st & BR_SSL_RECVAPP)) return 0;
        if ((st & (BR_SSL_SENDREC | BR_SSL_RECVREC)) == 0) {
            /* Idle mid-handshake — need peer data. */
            if (++spins > 100000) return -1;
            {
                int p = cc__tls_pump_rec(conn);
                if (p < 0) return -1;
                if (p == 0) {
                    /* Blocking wait: briefly poll for readability. */
                    fd_set rf, wf;
                    struct timeval tv;
                    int fd = conn->underlying.fd;
                    FD_ZERO(&rf);
                    FD_ZERO(&wf);
                    FD_SET(fd, &rf);
                    FD_SET(fd, &wf);
                    tv.tv_sec = 5;
                    tv.tv_usec = 0;
                    if (select(fd + 1, &rf, &wf, NULL, &tv) <= 0) return -1;
                }
            }
            continue;
        }
        {
            int p = cc__tls_pump_rec(conn);
            if (p < 0) return -1;
            if (p == 0) {
                fd_set rf, wf;
                struct timeval tv;
                int fd = conn->underlying.fd;
                unsigned st2 = br_ssl_engine_current_state(eng);
                FD_ZERO(&rf);
                FD_ZERO(&wf);
                if (st2 & BR_SSL_RECVREC) FD_SET(fd, &rf);
                if (st2 & BR_SSL_SENDREC) FD_SET(fd, &wf);
                tv.tv_sec = 5;
                tv.tv_usec = 0;
                if (select(fd + 1, &rf, &wf, NULL, &tv) <= 0) return -1;
            }
        }
        spins = 0;
    }
}

#endif /* CC_HAS_BEARSSL */

/* ============================================================================ */

CCTlsConn cc_tls_connect(CCSocket sock, CCTlsClientConfig cfg,
                         void *iobuf, size_t iobuf_len,
                         CCArena info_arena, CCNetError *out_err) {
    CCTlsConn conn = {0};
    *out_err = CC_NET_OK;
    (void)info_arena;
#ifdef CC_HAS_BEARSSL
    {
        CC__TlsCtx *ctx;
        char sni[256];
        if (!iobuf || iobuf_len < CC_TLS_IOBUF_SIZE) {
            *out_err = CC_NET_OTHER;
            return conn;
        }
        ctx = (CC__TlsCtx *)calloc(1, sizeof(*ctx));
        if (!ctx) {
            *out_err = CC_NET_OTHER;
            return conn;
        }
        ctx->is_server = 0;
        ctx->eng = &ctx->u.client.eng;
        br_ssl_client_init_full(&ctx->u.client, NULL, NULL, 0);
        br_ssl_engine_set_buffer(ctx->eng, iobuf, iobuf_len, 1);
        sni[0] = 0;
        if (cfg.sni_hostname && cfg.sni_hostname_len > 0) {
            size_t n = cfg.sni_hostname_len < sizeof(sni) - 1
                           ? cfg.sni_hostname_len
                           : sizeof(sni) - 1;
            memcpy(sni, cfg.sni_hostname, n);
            sni[n] = 0;
        }
        if (!br_ssl_client_reset(&ctx->u.client, sni[0] ? sni : NULL, 0)) {
            free(ctx);
            *out_err = CC_NET_TLS_HANDSHAKE_FAILED;
            return conn;
        }
        conn.ctx = ctx;
        conn.iobuf = iobuf;
        conn.iobuf_len = iobuf_len;
        conn.underlying = sock;
        conn.flags = 0;
        cc__tls_set_blocking(sock.fd, 1);
        if (cc__tls_handshake(&conn) != 0) {
            free(ctx);
            conn.ctx = NULL;
            *out_err = CC_NET_TLS_HANDSHAKE_FAILED;
            return conn;
        }
        cc__tls_set_blocking(sock.fd, 0);
        conn.flags |= CC__TLS_F_READY;
        return conn;
    }
#else
    (void)sock;
    (void)cfg;
    (void)iobuf;
    (void)iobuf_len;
    *out_err = CC_NET_TLS_HANDSHAKE_FAILED;
    return conn;
#endif
}

CCTlsConn cc_tls_connect_addr(const char *addr, size_t addr_len,
                              CCTlsClientConfig cfg, CCArena conn_arena,
                              CCNetError *out_err) {
    CCSocket sock = {0};
    void *iobuf;
    CCTlsConn conn = {0};
    CCResult_CCSocket_CCNetError sr;
    *out_err = CC_NET_OK;
    sr = cc_tcp_connect(addr, addr_len);
    if (!sr.ok) {
        *out_err = sr.u.error;
        return conn;
    }
    sock = sr.u.value;
    iobuf = cc_arena_alloc(conn_arena, CC_TLS_IOBUF_SIZE, 1);
    if (!iobuf) {
        cc_socket_close(&sock);
        *out_err = CC_NET_OTHER;
        return conn;
    }
    conn = cc_tls_connect(sock, cfg, iobuf, CC_TLS_IOBUF_SIZE, conn_arena,
                          out_err);
    if (*out_err != CC_NET_OK) cc_socket_close(&sock);
    return conn;
}

CCTlsConn cc_tls_accept(CCSocket sock, CCTlsServerConfig cfg, void *iobuf,
                        size_t iobuf_len, CCArena info_arena,
                        CCNetError *out_err) {
    CCTlsConn conn = {0};
    *out_err = CC_NET_OK;
    (void)info_arena;
#ifdef CC_HAS_BEARSSL
    {
        CC__TlsCtx *ctx;
        CC__TlsServerMats *m = &cc__tls_server_mats;
        /* Load from cfg paths if mats not yet ready. */
        if (!m->ready) {
            char cert[1024], key[1024];
            size_t cl = cfg.cert_path_len, kl = cfg.key_path_len;
            if (!cfg.cert_path || !cfg.key_path || cl == 0 || kl == 0
                || cl >= sizeof(cert) || kl >= sizeof(key)) {
                *out_err = CC_NET_TLS_CERTIFICATE_ERROR;
                return conn;
            }
            memcpy(cert, cfg.cert_path, cl);
            cert[cl] = 0;
            memcpy(key, cfg.key_path, kl);
            key[kl] = 0;
            if (cc_tls_server_load(cert, key) != 0) {
                *out_err = CC_NET_TLS_CERTIFICATE_ERROR;
                return conn;
            }
        }
        if (!iobuf || iobuf_len < CC_TLS_IOBUF_SIZE) {
            *out_err = CC_NET_OTHER;
            return conn;
        }
        ctx = (CC__TlsCtx *)calloc(1, sizeof(*ctx));
        if (!ctx) {
            *out_err = CC_NET_OTHER;
            return conn;
        }
        ctx->is_server = 1;
        ctx->eng = &ctx->u.server.eng;
        if (m->key_type == BR_KEYTYPE_RSA) {
            br_ssl_server_init_full_rsa(&ctx->u.server, m->chain, m->chain_len,
                                        &m->rsa);
        } else if (m->key_type == BR_KEYTYPE_EC) {
            br_ssl_server_init_full_ec(&ctx->u.server, m->chain, m->chain_len,
                                       BR_KEYTYPE_EC, &m->ec);
        } else {
            free(ctx);
            *out_err = CC_NET_TLS_CERTIFICATE_ERROR;
            return conn;
        }
        br_ssl_engine_set_buffer(ctx->eng, iobuf, iobuf_len, 1);
        if (!br_ssl_server_reset(&ctx->u.server)) {
            free(ctx);
            *out_err = CC_NET_TLS_HANDSHAKE_FAILED;
            return conn;
        }
        conn.ctx = ctx;
        conn.iobuf = iobuf;
        conn.iobuf_len = iobuf_len;
        conn.underlying = sock;
        conn.flags = CC__TLS_F_SERVER;
        /* Handshake with blocking I/O, then restore nonblock. */
        cc__tls_set_blocking(sock.fd, 1);
        if (cc__tls_handshake(&conn) != 0) {
            free(ctx);
            conn.ctx = NULL;
            *out_err = CC_NET_TLS_HANDSHAKE_FAILED;
            return conn;
        }
        cc__tls_set_blocking(sock.fd, 0);
        conn.flags |= CC__TLS_F_READY;
        return conn;
    }
#else
    (void)sock;
    (void)cfg;
    (void)iobuf;
    (void)iobuf_len;
    *out_err = CC_NET_TLS_HANDSHAKE_FAILED;
    return conn;
#endif
}

CCSlice cc_tls_read(CCTlsConn *conn, CCArena arena, size_t max_bytes,
                    CCNetError *out_err) {
    CCSlice result = {0};
    *out_err = CC_NET_OK;
#ifdef CC_HAS_BEARSSL
    {
        br_ssl_engine_context *eng;
        int idle = 0;
        if (!conn || !(conn->flags & CC__TLS_F_READY)) {
            *out_err = CC_NET_CONNECTION_CLOSED;
            return result;
        }
        eng = cc__tls_eng(conn);
        for (;;) {
            unsigned st = br_ssl_engine_current_state(eng);
            size_t avail;
            unsigned char *buf;
            size_t to_read;
            char *out;
            if (st & BR_SSL_CLOSED) {
                *out_err = CC_NET_CONNECTION_CLOSED;
                return result;
            }
            if (st & BR_SSL_RECVAPP) {
                buf = br_ssl_engine_recvapp_buf(eng, &avail);
                if (!buf || !avail) break;
                to_read = avail < max_bytes ? avail : max_bytes;
                out = (char *)cc_arena_alloc(arena, to_read, 1);
                if (!out) {
                    *out_err = CC_NET_OTHER;
                    return result;
                }
                memcpy(out, buf, to_read);
                br_ssl_engine_recvapp_ack(eng, to_read);
                result.ptr = out;
                result.len = to_read;
                return result;
            }
            {
                int p = cc__tls_pump_rec(conn);
                if (p < 0) {
                    *out_err = CC_NET_CONNECTION_CLOSED;
                    return result;
                }
                if (p == 0) {
                    if (++idle > 2) {
                        /* Would block — empty Ok slice, caller polls. */
                        return result;
                    }
                } else {
                    idle = 0;
                }
            }
        }
        return result;
    }
#else
    (void)conn;
    (void)arena;
    (void)max_bytes;
    *out_err = CC_NET_TLS_HANDSHAKE_FAILED;
    return result;
#endif
}

size_t cc_tls_write(CCTlsConn *conn, const char *data, size_t len,
                    CCNetError *out_err) {
    *out_err = CC_NET_OK;
#ifdef CC_HAS_BEARSSL
    {
        br_ssl_engine_context *eng;
        size_t sent = 0;
        int idle = 0;
        if (!conn || !(conn->flags & CC__TLS_F_READY) || !data) {
            *out_err = CC_NET_CONNECTION_CLOSED;
            return 0;
        }
        eng = cc__tls_eng(conn);
        while (sent < len) {
            unsigned st = br_ssl_engine_current_state(eng);
            size_t avail;
            unsigned char *buf;
            size_t chunk;
            if (st & BR_SSL_CLOSED) {
                *out_err = CC_NET_CONNECTION_CLOSED;
                return sent;
            }
            if (st & BR_SSL_SENDAPP) {
                buf = br_ssl_engine_sendapp_buf(eng, &avail);
                if (buf && avail) {
                    chunk = (len - sent) < avail ? (len - sent) : avail;
                    memcpy(buf, data + sent, chunk);
                    br_ssl_engine_sendapp_ack(eng, chunk);
                    br_ssl_engine_flush(eng, 0);
                    sent += chunk;
                    idle = 0;
                    continue;
                }
            }
            {
                int p = cc__tls_pump_rec(conn);
                if (p < 0) {
                    *out_err = CC_NET_CONNECTION_CLOSED;
                    return sent;
                }
                if (p == 0) {
                    /* Nonblocking socket: wait briefly for SENDREC/RECVREC. */
                    fd_set rf, wf;
                    struct timeval tv;
                    int fd = conn->underlying.fd;
                    FD_ZERO(&rf);
                    FD_ZERO(&wf);
                    FD_SET(fd, &rf);
                    FD_SET(fd, &wf);
                    tv.tv_sec = 5;
                    tv.tv_usec = 0;
                    if (select(fd + 1, &rf, &wf, NULL, &tv) <= 0) {
                        if (++idle > 64) {
                            *out_err = CC_NET_TIMED_OUT;
                            return sent;
                        }
                    } else {
                        idle = 0;
                    }
                } else {
                    idle = 0;
                }
            }
        }
        /* Flush remaining records. */
        idle = 0;
        for (;;) {
            unsigned st = br_ssl_engine_current_state(eng);
            if (!(st & BR_SSL_SENDREC)) break;
            {
                int p = cc__tls_pump_rec(conn);
                if (p < 0) {
                    *out_err = CC_NET_CONNECTION_CLOSED;
                    return sent;
                }
                if (p == 0) {
                    fd_set rf, wf;
                    struct timeval tv;
                    int fd = conn->underlying.fd;
                    FD_ZERO(&rf);
                    FD_ZERO(&wf);
                    FD_SET(fd, &wf);
                    tv.tv_sec = 5;
                    tv.tv_usec = 0;
                    if (select(fd + 1, NULL, &wf, NULL, &tv) <= 0) {
                        if (++idle > 64) break;
                    } else {
                        idle = 0;
                    }
                } else {
                    idle = 0;
                }
            }
        }
        return sent;
    }
#else
    (void)conn;
    (void)data;
    (void)len;
    *out_err = CC_NET_TLS_HANDSHAKE_FAILED;
    return 0;
#endif
}

void cc_tls_shutdown(CCTlsConn *conn, CCShutdownMode mode,
                     CCNetError *out_err) {
    *out_err = CC_NET_OK;
#ifdef CC_HAS_BEARSSL
    if (conn && conn->ctx) {
        br_ssl_engine_context *eng = cc__tls_eng(conn);
        if (eng) {
            br_ssl_engine_close(eng);
            (void)cc__tls_pump_rec(conn);
        }
    }
#endif
    if (conn) cc_socket_shutdown(&conn->underlying, mode, out_err);
}

void cc_tls_close(CCTlsConn *conn) {
    if (!conn) return;
#ifdef CC_HAS_BEARSSL
    if (conn->ctx) {
        free(conn->ctx);
        conn->ctx = NULL;
    }
#endif
    cc_socket_close(&conn->underlying);
    conn->flags |= CC__TLS_F_CLOSED;
}

const CCTlsInfo *cc_tls_info(const CCTlsConn *conn) {
    (void)conn;
    return NULL;
}

CCTlsCertChain *cc_tls_load_cert_chain(CCArena arena, const char *path,
                                       size_t path_len, CCNetError *out_err) {
    (void)arena;
    (void)path;
    (void)path_len;
    *out_err = CC_NET_OTHER;
    return NULL;
}

CCTlsPrivateKey *cc_tls_load_private_key(CCArena arena, const char *path,
                                         size_t path_len, CCNetError *out_err) {
    (void)arena;
    (void)path;
    (void)path_len;
    *out_err = CC_NET_OTHER;
    return NULL;
}

CCTlsTrustAnchors *cc_tls_load_trust_anchors(CCArena arena, const char *path,
                                             size_t path_len,
                                             CCNetError *out_err) {
    (void)arena;
    (void)path;
    (void)path_len;
    *out_err = CC_NET_OTHER;
    return NULL;
}
