/*
 * Concurrent-C TLS Runtime
 *
 * BearSSL integration for TLS client/server.
 * 
 * BearSSL characteristics that make it ideal for CC:
 * - No dynamic allocation (caller provides all buffers)
 * - Explicit state machine (no callbacks for I/O)
 * - Small code size (~30KB for full TLS 1.2)
 * - MIT license
 */

#include "std/tls.cch"
#include "std/net.cch"

/* BearSSL includes */
#ifdef CC_HAS_BEARSSL
#include <bearssl.h>
#endif

#include <string.h>
#include <stdlib.h>

/* ============================================================================
 * Internal Helpers
 * ============================================================================ */

#ifdef CC_HAS_BEARSSL

/* Low-level I/O: read from socket into BearSSL engine */
static int br_low_read(void* ctx, unsigned char* buf, size_t len) {
    CCSocket* sock = (CCSocket*)ctx;
    CCNetError err;
    CCSlice data = cc_socket_read(sock, NULL, len, &err);  /* TODO: need temp arena */
    if (err != CC_NET_OK) {
        return -1;
    }
    if (data.len == 0) {
        return 0;  /* EOF */
    }
    memcpy(buf, data.ptr, data.len);
    return (int)data.len;
}

/* Low-level I/O: write from BearSSL engine to socket */
static int br_low_write(void* ctx, const unsigned char* buf, size_t len) {
    CCSocket* sock = (CCSocket*)ctx;
    CCNetError err;
    size_t written = cc_socket_write(sock, (const char*)buf, len, &err);
    if (err != CC_NET_OK) {
        return -1;
    }
    return (int)written;
}

#endif /* CC_HAS_BEARSSL */

/* ============================================================================
 * TLS Client
 * ============================================================================ */

CCTlsConn cc_tls_connect(CCSocket sock, CCTlsClientConfig cfg,
                          void* iobuf, size_t iobuf_len,
                          CCArena* info_arena, CCNetError* out_err) {
    CCTlsConn conn = {0};
    *out_err = CC_NET_OK;

#ifdef CC_HAS_BEARSSL
    /* Allocate BearSSL context */
    br_ssl_client_context* cc = malloc(sizeof(br_ssl_client_context));
    if (!cc) {
        *out_err = CC_NET_OTHER;
        return conn;
    }

    /* Initialize client context with default profiles */
    br_ssl_client_init_full(cc, NULL, NULL, 0);  /* TODO: load trust anchors */

    /* Set I/O buffer */
    br_ssl_engine_set_buffer(&cc->eng, iobuf, iobuf_len, 1);  /* bidirectional */

    /* Set SNI hostname */
    if (cfg.sni_hostname && cfg.sni_hostname_len > 0) {
        char sni_buf[256];
        size_t sni_len = cfg.sni_hostname_len < sizeof(sni_buf) - 1 
                         ? cfg.sni_hostname_len : sizeof(sni_buf) - 1;
        memcpy(sni_buf, cfg.sni_hostname, sni_len);
        sni_buf[sni_len] = '\0';
        br_ssl_client_reset(cc, sni_buf, 0);
    } else {
        br_ssl_client_reset(cc, NULL, 0);
    }

    /* Run handshake */
    /* TODO: This needs to be async-aware; for now, blocking */
    while (br_ssl_engine_current_state(&cc->eng) == BR_SSL_HANDSHAKE) {
        /* ... run engine state machine ... */
        /* This is a simplification; real impl needs proper I/O loop */
    }

    if (br_ssl_engine_current_state(&cc->eng) == BR_SSL_CLOSED) {
        int err = br_ssl_engine_last_error(&cc->eng);
        *out_err = CC_NET_TLS_HANDSHAKE_FAILED;
        free(cc);
        return conn;
    }

    conn.ctx = cc;
    conn.iobuf = iobuf;
    conn.iobuf_len = iobuf_len;
    conn.underlying = sock;
    conn.info_arena = info_arena;
    conn.flags = 1;  /* client mode */

#else
    /* No BearSSL: return error */
    (void)sock;
    (void)cfg;
    (void)iobuf;
    (void)iobuf_len;
    (void)info_arena;
    *out_err = CC_NET_TLS_HANDSHAKE_FAILED;
#endif

    return conn;
}

CCTlsConn cc_tls_connect_addr(const char* addr, size_t addr_len,
                               CCTlsClientConfig cfg,
                               CCArena* conn_arena, CCNetError* out_err) {
    CCTlsConn conn = {0};
    *out_err = CC_NET_OK;

    /* Connect TCP first */
    CCSocket sock = cc_tcp_connect(addr, addr_len, out_err);
    if (*out_err != CC_NET_OK) {
        return conn;
    }

    /* Allocate I/O buffer from arena */
    void* iobuf = cc_arena_alloc(conn_arena, CC_TLS_IOBUF_SIZE);
    if (!iobuf) {
        cc_socket_close(&sock);
        *out_err = CC_NET_OTHER;
        return conn;
    }

    /* Perform TLS handshake */
    conn = cc_tls_connect(sock, cfg, iobuf, CC_TLS_IOBUF_SIZE, conn_arena, out_err);
    if (*out_err != CC_NET_OK) {
        cc_socket_close(&sock);
    }

    return conn;
}

/* ============================================================================
 * TLS Server
 * ============================================================================ */

CCTlsConn cc_tls_accept(CCSocket sock, CCTlsServerConfig cfg,
                         void* iobuf, size_t iobuf_len,
                         CCArena* info_arena, CCNetError* out_err) {
    CCTlsConn conn = {0};
    *out_err = CC_NET_OK;

#ifdef CC_HAS_BEARSSL
    /* TODO: Implement server-side TLS */
    /* Similar to client but uses br_ssl_server_context */
    (void)sock;
    (void)cfg;
    (void)iobuf;
    (void)iobuf_len;
    (void)info_arena;
    *out_err = CC_NET_TLS_HANDSHAKE_FAILED;
#else
    (void)sock;
    (void)cfg;
    (void)iobuf;
    (void)iobuf_len;
    (void)info_arena;
    *out_err = CC_NET_TLS_HANDSHAKE_FAILED;
#endif

    return conn;
}

/* ============================================================================
 * TLS I/O
 * ============================================================================ */

CCSlice cc_tls_read(CCTlsConn* conn, CCArena* arena, size_t max_bytes, CCNetError* out_err) {
    CCSlice result = {0};
    *out_err = CC_NET_OK;

#ifdef CC_HAS_BEARSSL
    br_ssl_client_context* cc = (br_ssl_client_context*)conn->ctx;
    if (!cc) {
        *out_err = CC_NET_CONNECTION_CLOSED;
        return result;
    }

    /* Get application data from engine */
    size_t avail;
    unsigned char* buf = br_ssl_engine_recvapp_buf(&cc->eng, &avail);
    if (!buf || avail == 0) {
        /* Need to run engine to get more data */
        /* TODO: proper async I/O loop */
        *out_err = CC_NET_CONNECTION_CLOSED;
        return result;
    }

    size_t to_read = avail < max_bytes ? avail : max_bytes;
    char* out = cc_arena_alloc(arena, to_read);
    if (!out) {
        *out_err = CC_NET_OTHER;
        return result;
    }

    memcpy(out, buf, to_read);
    br_ssl_engine_recvapp_ack(&cc->eng, to_read);

    result.ptr = out;
    result.len = to_read;
#else
    (void)conn;
    (void)arena;
    (void)max_bytes;
    *out_err = CC_NET_TLS_HANDSHAKE_FAILED;
#endif

    return result;
}

size_t cc_tls_write(CCTlsConn* conn, const char* data, size_t len, CCNetError* out_err) {
    *out_err = CC_NET_OK;

#ifdef CC_HAS_BEARSSL
    br_ssl_client_context* cc = (br_ssl_client_context*)conn->ctx;
    if (!cc) {
        *out_err = CC_NET_CONNECTION_CLOSED;
        return 0;
    }

    /* Get send buffer from engine */
    size_t avail;
    unsigned char* buf = br_ssl_engine_sendapp_buf(&cc->eng, &avail);
    if (!buf || avail == 0) {
        /* Buffer full, need to flush */
        /* TODO: proper async I/O loop */
        *out_err = CC_NET_OTHER;
        return 0;
    }

    size_t to_write = len < avail ? len : avail;
    memcpy(buf, data, to_write);
    br_ssl_engine_sendapp_ack(&cc->eng, to_write);
    br_ssl_engine_flush(&cc->eng, 0);

    /* TODO: actually send data to socket */

    return to_write;
#else
    (void)conn;
    (void)data;
    (void)len;
    *out_err = CC_NET_TLS_HANDSHAKE_FAILED;
    return 0;
#endif
}

void cc_tls_shutdown(CCTlsConn* conn, CCShutdownMode mode, CCNetError* out_err) {
    *out_err = CC_NET_OK;

#ifdef CC_HAS_BEARSSL
    br_ssl_client_context* cc = (br_ssl_client_context*)conn->ctx;
    if (cc) {
        br_ssl_engine_close(&cc->eng);
        /* TODO: send close_notify and wait for response */
    }
#endif

    /* Shutdown underlying socket */
    cc_socket_shutdown(&conn->underlying, mode, out_err);
}

void cc_tls_close(CCTlsConn* conn) {
#ifdef CC_HAS_BEARSSL
    if (conn->ctx) {
        free(conn->ctx);
        conn->ctx = NULL;
    }
#endif
    cc_socket_close(&conn->underlying);
}

const CCTlsInfo* cc_tls_info(const CCTlsConn* conn) {
    /* TODO: Extract TLS info from BearSSL context */
    (void)conn;
    return NULL;
}

/* ============================================================================
 * Certificate Loading (stubs)
 * ============================================================================ */

CCTlsCertChain* cc_tls_load_cert_chain(CCArena* arena, const char* path, size_t path_len, CCNetError* out_err) {
    /* TODO: Implement PEM parsing */
    (void)arena;
    (void)path;
    (void)path_len;
    *out_err = CC_NET_OTHER;
    return NULL;
}

CCTlsPrivateKey* cc_tls_load_private_key(CCArena* arena, const char* path, size_t path_len, CCNetError* out_err) {
    /* TODO: Implement PEM parsing */
    (void)arena;
    (void)path;
    (void)path_len;
    *out_err = CC_NET_OTHER;
    return NULL;
}

CCTlsTrustAnchors* cc_tls_load_trust_anchors(CCArena* arena, const char* path, size_t path_len, CCNetError* out_err) {
    /* TODO: Implement PEM parsing for CA certs */
    (void)arena;
    (void)path;
    (void)path_len;
    *out_err = CC_NET_OTHER;
    return NULL;
}
