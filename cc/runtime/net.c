/*
 * Concurrent-C Networking Runtime
 *
 * POSIX socket implementation.
 * Async variants require runtime scheduler integration.
 */

#include <ccc/std/net.cch>
#include <ccc/cc_channel.cch>

#undef cc_socket_read
#undef cc_socket_peer_addr
#undef cc_socket_local_addr
#undef cc_udp_recv_from
#undef cc_dns_lookup
#undef cc_ip_addr_to_string

#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>

#include "fiber_internal.h"
#include "fiber_sched_boundary.h"
#include "io_wait.h"
#include "channel_wait_internal.h"
#include "wait_select_internal.h"

#define CC_NET_FLAG_NONBLOCK 0x01

static cc__io_owned_watcher* cc__net_ensure_socket_watcher(CCSocket* sock) {
    if (!sock || sock->fd < 0) return NULL;
    if (sock->watcher) return (cc__io_owned_watcher*)sock->watcher;
    cc__io_owned_watcher* watcher = cc__io_watcher_create(sock->fd);
    if (watcher) {
        sock->watcher = watcher;
    }
    return watcher;
}

static cc__io_owned_watcher* cc__net_ensure_listener_watcher(CCListener* ln) {
    if (!ln || ln->fd < 0) return NULL;
    if (ln->watcher) return (cc__io_owned_watcher*)ln->watcher;
    cc__io_owned_watcher* watcher = cc__io_watcher_create(ln->fd);
    if (watcher) {
        ln->watcher = watcher;
    }
    return watcher;
}

/* ============================================================================
 * Helpers
 * ============================================================================ */

static CCNetError errno_to_net_error(int err) {
    switch (err) {
        case ECONNREFUSED: return CC_NET_CONNECTION_REFUSED;
        case ECONNRESET:   return CC_NET_CONNECTION_RESET;
        case EPIPE:        return CC_NET_CONNECTION_RESET;
        case ETIMEDOUT:    return CC_NET_TIMED_OUT;
        case EHOSTUNREACH: return CC_NET_HOST_UNREACHABLE;
        case ENETUNREACH:  return CC_NET_NETWORK_UNREACHABLE;
        case EADDRINUSE:   return CC_NET_ADDRESS_IN_USE;
        case EADDRNOTAVAIL: return CC_NET_ADDRESS_NOT_AVAILABLE;
        default:           return CC_NET_OTHER;
    }
}

/* Map OS errno into the socket byte-I/O Result domain (CCIoError). */
static CCIoError errno_to_io_error(int err) {
    if (err == EAGAIN || err == EWOULDBLOCK) {
        return cc_io_error_os(CC_IO_BUSY, err);
    }
    if (err == ETIMEDOUT) {
        return cc_io_error_os(CC_IO_BUSY, err);
    }
    if (err == ECONNRESET || err == EPIPE) {
        return cc_io_error_os(CC_IO_CONNECTION_CLOSED, err);
    }
    CCIoError mapped = cc_net_to_io_error(errno_to_net_error(err));
    if (mapped.os_code == 0 && err != 0) {
        mapped.os_code = err;
    }
    return mapped;
}

static int cc__net_set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return errno;
    if (flags & O_NONBLOCK) return 0;
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) return errno;
    return 0;
}

static void cc__net_set_cloexec_best_effort(int fd) {
    int flags = fcntl(fd, F_GETFD, 0);
    if (flags < 0) return;
    (void)fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
}

static void cc__net_disable_sigpipe_best_effort(int fd) {
#ifdef SO_NOSIGPIPE
    int opt = 1;
    (void)setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &opt, sizeof(opt));
#else
    (void)fd;
#endif
}

/* Linux has no SO_NOSIGPIPE; SIGPIPE must be suppressed per-send with
 * MSG_NOSIGNAL instead (a peer that closes mid-stream would otherwise
 * kill the whole process on the second write after its FIN/RST).  send()
 * with these flags is equivalent to write() for connected sockets. */
#ifdef MSG_NOSIGNAL
#define CC__NET_SEND_FLAGS MSG_NOSIGNAL
#else
#define CC__NET_SEND_FLAGS 0
#endif

static int cc__net_prepare_fiber_fd(int fd, uint8_t* flags) {
    if (!cc__fiber_in_context()) return 0;
    if (flags && (*flags & CC_NET_FLAG_NONBLOCK)) return 0;
    int err = cc__net_set_nonblocking(fd);
    if (err == 0 && flags) *flags |= CC_NET_FLAG_NONBLOCK;
    return err;
}

static int cc__net_trace_read_enabled(void) {
    static _Atomic int cached = -1;
    int value = atomic_load_explicit(&cached, memory_order_relaxed);
    if (value >= 0) return value;
    const char* env = getenv("CC_NET_TRACE_READ");
    int enabled = (env && env[0] && !(env[0] == '0' && env[1] == '\0')) ? 1 : 0;
    int expected = -1;
    (void)atomic_compare_exchange_strong_explicit(&cached, &expected, enabled,
                                                  memory_order_relaxed,
                                                  memory_order_relaxed);
    return atomic_load_explicit(&cached, memory_order_relaxed);
}

static void cc__net_trace_read(const char* action, int fd, ssize_t n, int err) {
    if (!cc__net_trace_read_enabled()) return;
    fprintf(stderr, "[cc:net:read] %s fd=%d n=%zd err=%d fiber=%p\n",
            action, fd, n, err, cc__fiber_current());
}

/* Parse "host:port" into sockaddr.
 * Handles IPv4, IPv6 [::]:port, and hostname resolution. */
static int parse_addr(const char* addr, size_t addr_len,
                      struct sockaddr_storage* out_sa, socklen_t* out_sa_len,
                      CCNetError* out_err) {
    /* Null-terminate for getaddrinfo */
    char buf[256];
    if (addr_len >= sizeof(buf)) {
        *out_err = CC_NET_INVALID_ADDRESS;
        return -1;
    }
    memcpy(buf, addr, addr_len);
    buf[addr_len] = '\0';

    /* Find last colon for port */
    char* port_sep = NULL;
    char* host_start = buf;

    if (buf[0] == '[') {
        /* IPv6 literal: [::1]:port */
        host_start = buf + 1;
        char* bracket = strchr(host_start, ']');
        if (!bracket) {
            *out_err = CC_NET_INVALID_ADDRESS;
            return -1;
        }
        *bracket = '\0';
        if (bracket[1] == ':') {
            port_sep = bracket + 2;
        } else if (bracket[1] == '\0') {
            port_sep = NULL;  /* No port */
        } else {
            *out_err = CC_NET_INVALID_ADDRESS;
            return -1;
        }
    } else {
        /* IPv4 or hostname: find last colon */
        port_sep = strrchr(buf, ':');
        if (port_sep) {
            *port_sep = '\0';
            port_sep++;
        }
    }

    /* Parse port */
    int port = 0;
    if (port_sep && *port_sep) {
        port = atoi(port_sep);
        if (port <= 0 || port > 65535) {
            *out_err = CC_NET_INVALID_ADDRESS;
            return -1;
        }
    }

    /* Resolve hostname */
    struct addrinfo hints = {0};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo* result = NULL;
    int gai_err = getaddrinfo(host_start, NULL, &hints, &result);
    if (gai_err != 0) {
        *out_err = CC_NET_DNS_FAILURE;
        return -1;
    }

    /* Copy first result */
    memcpy(out_sa, result->ai_addr, result->ai_addrlen);
    *out_sa_len = result->ai_addrlen;

    /* Set port */
    if (out_sa->ss_family == AF_INET) {
        ((struct sockaddr_in*)out_sa)->sin_port = htons(port);
    } else if (out_sa->ss_family == AF_INET6) {
        ((struct sockaddr_in6*)out_sa)->sin6_port = htons(port);
    }

    freeaddrinfo(result);
    *out_err = CC_NET_OK;
    return 0;
}

/* ============================================================================
 * TCP Client
 * ============================================================================ */

CCResult_CCSocket_CCNetError cc_tcp_connect(const char* addr, size_t addr_len) {
    CCSocket sock = {.fd = -1, .flags = 0, .watcher = NULL};
    CCNetError err = CC_NET_OK;

    struct sockaddr_storage sa;
    socklen_t sa_len;
    if (parse_addr(addr, addr_len, &sa, &sa_len, &err) < 0) {
        return cc_err_CCResult_CCSocket_CCNetError(err);
    }

    int fd = socket(sa.ss_family, SOCK_STREAM, 0);
    if (fd < 0) {
        return cc_err_CCResult_CCSocket_CCNetError(errno_to_net_error(errno));
    }

    if (connect(fd, (struct sockaddr*)&sa, sa_len) < 0) {
        err = errno_to_net_error(errno);
        close(fd);
        return cc_err_CCResult_CCSocket_CCNetError(err);
    }

    cc__net_disable_sigpipe_best_effort(fd);
    sock.fd = fd;
    return cc_ok_CCResult_CCSocket_CCNetError(sock);
}

/* ============================================================================
 * TCP Server
 * ============================================================================ */

CCResult_CCListener_CCNetError cc_tcp_listen(CCSlice addr) {
    CCListener ln = {.fd = -1, .flags = 0, .watcher = NULL};
    CCNetError err = CC_NET_OK;

    struct sockaddr_storage sa;
    socklen_t sa_len;
    if (!addr.ptr) {
        return cc_err_CCResult_CCListener_CCNetError(CC_NET_INVALID_ADDRESS);
    }
    if (parse_addr((const char*)addr.ptr, addr.len, &sa, &sa_len, &err) < 0) {
        return cc_err_CCResult_CCListener_CCNetError(err);
    }

    int fd = socket(sa.ss_family, SOCK_STREAM, 0);
    if (fd < 0) {
        return cc_err_CCResult_CCListener_CCNetError(errno_to_net_error(errno));
    }

    /* Allow address reuse */
    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    if (bind(fd, (struct sockaddr*)&sa, sa_len) < 0) {
        err = errno_to_net_error(errno);
        close(fd);
        return cc_err_CCResult_CCListener_CCNetError(err);
    }

    /* Kernel SYN queue, not dest occupancy. The kernel clamps to its own
     * somaxconn (128 on stock Darwin; 4096 on Linux 5.4+), so asking for
     * the platform maximum never asks for less than it will grant. */
    if (listen(fd, SOMAXCONN) < 0) {
        err = errno_to_net_error(errno);
        close(fd);
        return cc_err_CCResult_CCListener_CCNetError(err);
    }

    ln.fd = fd;
    ln.watcher = cc__io_watcher_create(fd);
    return cc_ok_CCResult_CCListener_CCNetError(ln);
}

/* Listener close is two-phase (see net.cch). `state` packs the closing bit
 * with a count of fibers inside accept; whoever observes closing with the
 * count at zero owns the teardown, so the fd/watcher are freed exactly once
 * and never under a live accepter. */
#define CC_LN_CLOSING 1
#define CC_LN_ONE     2

static void cc__listener_teardown(CCListener* ln) {
    if (ln->watcher) {
        cc__io_watcher_destroy((cc__io_owned_watcher*)ln->watcher);
        ln->watcher = NULL;
    } else if (ln->fd >= 0) {
        cc__io_wait_forget_fd(ln->fd);
    }
    if (ln->fd >= 0) {
        close(ln->fd);
        ln->fd = -1;
    }
}

/* Returns 0 when admitted, nonzero when the listener is closing. */
static int cc__listener_enter(CCListener* ln) {
    int cur = atomic_load_explicit(&ln->state, memory_order_acquire);
    while (1) {
        if (cur & CC_LN_CLOSING) return 1;
        if (atomic_compare_exchange_weak_explicit(&ln->state, &cur, cur + CC_LN_ONE,
                                                  memory_order_acq_rel, memory_order_acquire))
            return 0;
    }
}

static void cc__listener_leave(CCListener* ln) {
    int prev = atomic_fetch_sub_explicit(&ln->state, CC_LN_ONE, memory_order_acq_rel);
    if ((prev & CC_LN_CLOSING) && (prev - CC_LN_ONE) == CC_LN_CLOSING) {
        cc__listener_teardown(ln);
    }
}

static CCResult_CCSocket_CCNetError cc__listener_accept_inner(CCListener* ln, int fiber_ctx);

CCResult_CCSocket_CCNetError cc_listener_accept(CCListener* ln) {
    if (!ln) return cc_err_CCResult_CCSocket_CCNetError(CC_NET_OTHER);
    if (cc__listener_enter(ln) != 0) {
        return cc_err_CCResult_CCSocket_CCNetError(CC_NET_CONNECTION_CLOSED);
    }
    if (ln->fd < 0) {
        cc__listener_leave(ln);
        return cc_err_CCResult_CCSocket_CCNetError(CC_NET_OTHER);
    }
    CCResult_CCSocket_CCNetError r = cc__listener_accept_inner(ln, cc__fiber_in_context());
    /* A close that raced our last wait is reported as CLOSED, not as the
     * EBADF/ECANCELED it surfaced as. */
    if (cc_is_err(r) &&
        (atomic_load_explicit(&ln->state, memory_order_acquire) & CC_LN_CLOSING)) {
        r = cc_err_CCResult_CCSocket_CCNetError(CC_NET_CONNECTION_CLOSED);
    }
    cc__listener_leave(ln);
    return r;
}

static CCResult_CCSocket_CCNetError cc__listener_accept_inner(CCListener* ln, int fiber_ctx) {
    CCSocket sock = {.fd = -1, .flags = 0, .watcher = NULL};

    struct sockaddr_storage client_addr;
    socklen_t client_len = sizeof(client_addr);
    int prep_err = cc__net_prepare_fiber_fd(ln->fd, &ln->flags);
    if (prep_err != 0) {
        return cc_err_CCResult_CCSocket_CCNetError(errno_to_net_error(prep_err));
    }

    while (1) {
        if (atomic_load_explicit(&ln->state, memory_order_acquire) & CC_LN_CLOSING) {
            return cc_err_CCResult_CCSocket_CCNetError(CC_NET_CONNECTION_CLOSED);
        }
        /* A blocking accept outside fiber context is still waiting on
         * outside-world progress, so classify just this wait site as external. */
        if (!fiber_ctx) cc_external_wait_enter();
        int fd = accept(ln->fd, (struct sockaddr*)&client_addr, &client_len);
        if (!fiber_ctx) cc_external_wait_leave();
        if (fd >= 0) {
            int fd_err = cc__net_set_nonblocking(fd);
            if (fd_err != 0) {
                close(fd);
                return cc_err_CCResult_CCSocket_CCNetError(errno_to_net_error(fd_err));
            }
            cc__net_set_cloexec_best_effort(fd);
            cc__net_disable_sigpipe_best_effort(fd);
            sock.fd = fd;
            sock.flags |= CC_NET_FLAG_NONBLOCK;
            return cc_ok_CCResult_CCSocket_CCNetError(sock);
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            cc__io_owned_watcher* watcher = cc__net_ensure_listener_watcher(ln);
            int wait_err;
            if (watcher) {
                wait_err = cc__io_watcher_wait(watcher, POLLIN);
            } else {
                /* No watcher (allocation failed): nothing for close to wake,
                 * so poll in slices and re-read the closing bit ourselves. */
                struct timespec slice;
                clock_gettime(CLOCK_REALTIME, &slice);
                slice.tv_nsec += 50 * 1000000L;
                if (slice.tv_nsec >= 1000000000L) { slice.tv_sec++; slice.tv_nsec -= 1000000000L; }
                wait_err = cc__io_wait_fd_deadline(ln->fd, POLLIN, &slice);
                if (wait_err == ETIMEDOUT) wait_err = 0;
            }
            if (wait_err == ECANCELED ||
                (atomic_load_explicit(&ln->state, memory_order_acquire) & CC_LN_CLOSING)) {
                return cc_err_CCResult_CCSocket_CCNetError(CC_NET_CONNECTION_CLOSED);
            }
            if (wait_err != 0) {
                return cc_err_CCResult_CCSocket_CCNetError(errno_to_net_error(wait_err));
            }
            client_len = sizeof(client_addr);
            continue;
        }
        return cc_err_CCResult_CCSocket_CCNetError(errno_to_net_error(errno));
    }
}

void cc_listener_serve(CCListener* ln, CCNursery n, CCClosure1 on_conn) {
    if (!ln || !n.p) {
        cc_closure1_drop(on_conn);
        return;
    }
    /* Borrow-call: cc_closure1_call is single-shot (drops env). An accept
     * loop must invoke on_conn many times, then drop once at the end. */
    while (!cc_nursery_is_cancelled(n)) {
        CCResult_CCSocket_CCNetError ar = cc_listener_accept(ln);
        CCSocket client;
        if (cc_is_err(ar)) break;
        client = cc_value(ar);
        if (on_conn.fn)
            (void)on_conn.fn(on_conn.env, (intptr_t)&client);
    }
    cc_closure1_drop(on_conn);
}

void cc_listener_close(CCListener* ln) {
    if (!ln) return;
    int prev = atomic_fetch_or_explicit(&ln->state, CC_LN_CLOSING, memory_order_acq_rel);
    if ((prev & ~CC_LN_CLOSING) == 0) {
        /* Nobody inside accept: this call owns the teardown. Also the path
         * a second close / the @destroy hook takes — teardown is a no-op
         * once fd == -1 and watcher == NULL. */
        cc__listener_teardown(ln);
        return;
    }
    /* Accepters are inside. Wake them; the last one out tears down. The
     * watcher is only ever created by an accepter, which is admitted, so
     * it cannot appear or vanish under us here. */
    if (ln->watcher) {
        cc__io_watcher_cancel_waiters((cc__io_owned_watcher*)ln->watcher);
    }
}

/* ============================================================================
 * Socket I/O (Result-primary, EOF model B)
 * ============================================================================ */

CCResult_bool_CCIoError cc_socket_read_into(CCSocket* sock, char* buf, size_t max_bytes, size_t* out) {
    return cc_socket_read_into_deadline(sock, buf, max_bytes, out, NULL);
}

CCResult_bool_CCIoError cc_socket_read_into_deadline(CCSocket* sock,
                                                    char* buf,
                                                    size_t max_bytes,
                                                    size_t* out,
                                                    const CCDeadline* deadline) {
    if (out) *out = 0;
    if (!sock || (!buf && max_bytes > 0) || !out) {
        return cc_err_CCResult_bool_CCIoError(cc_io_from_errno(EINVAL));
    }

    int prep_err = cc__net_prepare_fiber_fd(sock->fd, &sock->flags);
    if (prep_err != 0) {
        return cc_err_CCResult_bool_CCIoError(errno_to_io_error(prep_err));
    }
    struct timespec ts;
    const struct timespec* abs_deadline = cc_deadline_as_timespec(deadline, &ts);

    while (1) {
        ssize_t n = read(sock->fd, buf, max_bytes);
        if (n > 0) {
            cc__net_trace_read("read_ok", sock->fd, n, 0);
            *out = (size_t)n;
            return cc_ok_CCResult_bool_CCIoError(true);
        }
        if (n == 0) {
            /* Clean peer close (FIN) — Ok(false), not an error. */
            cc__net_trace_read("read_eof", sock->fd, n, 0);
            *out = 0;
            return cc_ok_CCResult_bool_CCIoError(false);
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            cc__net_trace_read("wait_begin", sock->fd, n, errno);
            cc__io_owned_watcher* watcher = cc__net_ensure_socket_watcher(sock);
            int wait_err = watcher ? cc__io_watcher_wait_deadline(watcher, POLLIN, abs_deadline)
                                   : cc__io_wait_fd_deadline(sock->fd, POLLIN, abs_deadline);
            cc__net_trace_read("wait_end", sock->fd, n, wait_err);
            if (wait_err != 0) {
                return cc_err_CCResult_bool_CCIoError(errno_to_io_error(wait_err));
            }
            continue;
        }
        cc__net_trace_read("read_err", sock->fd, n, errno);
        return cc_err_CCResult_bool_CCIoError(errno_to_io_error(errno));
    }
}

CCResult_bool_CCIoError cc_socket_try_read_into(CCSocket* sock,
                                               char* buf,
                                               size_t max_bytes,
                                               size_t* out) {
    if (out) *out = 0;
    if (!sock || (!buf && max_bytes > 0) || !out) {
        return cc_err_CCResult_bool_CCIoError(cc_io_from_errno(EINVAL));
    }

    int prep_err = cc__net_prepare_fiber_fd(sock->fd, &sock->flags);
    if (prep_err != 0) {
        return cc_err_CCResult_bool_CCIoError(errno_to_io_error(prep_err));
    }

    ssize_t n = read(sock->fd, buf, max_bytes);
    if (n > 0) {
        cc__net_trace_read("try_read_ok", sock->fd, n, 0);
        *out = (size_t)n;
        return cc_ok_CCResult_bool_CCIoError(true);
    }
    if (n == 0) {
        /* Clean peer close (FIN) — distinct from would-block. */
        cc__net_trace_read("try_read_eof", sock->fd, n, 0);
        *out = 0;
        return cc_ok_CCResult_bool_CCIoError(false);
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
        cc__net_trace_read("try_read_would_block", sock->fd, n, errno);
        return cc_err_CCResult_bool_CCIoError(cc_io_error_os(CC_IO_BUSY, errno));
    }
    cc__net_trace_read("try_read_err", sock->fd, n, errno);
    return cc_err_CCResult_bool_CCIoError(errno_to_io_error(errno));
}

CCResult_bool_CCIoError cc_socket_read(CCSocket* sock, CCArena arena, size_t max_bytes, CCSlice* out) {
    if (out) *out = (CCSlice){0};
    if (!sock || !cc_arena_is_live(arena) || !out) {
        return cc_err_CCResult_bool_CCIoError(cc_io_from_errno(EINVAL));
    }
    if (max_bytes == 0) {
        *out = (CCSlice){0};
        return cc_ok_CCResult_bool_CCIoError(false);
    }

    char* buf = cc_arena_alloc(arena, max_bytes, 1);
    if (!buf) {
        return cc_err_CCResult_bool_CCIoError(cc_io_error_os(CC_IO_OUT_OF_MEMORY, ENOMEM));
    }

    size_t n = 0;
    CCResult_bool_CCIoError status = cc_socket_read_into(sock, buf, max_bytes, &n);
    if (cc_is_err(status)) return status;
    if (!cc_value(status)) {
        *out = (CCSlice){0};
        return cc_ok_CCResult_bool_CCIoError(false);
    }
    out->ptr = buf;
    out->len = n;
    return cc_ok_CCResult_bool_CCIoError(true);
}

CCResult_size_t_CCIoError cc_socket_write(CCSocket* sock, const char* data, size_t len) {
    return cc_socket_write_deadline(sock, data, len, NULL);
}

CCResult_size_t_CCIoError cc_socket_write_deadline(CCSocket* sock,
                                                  const char* data,
                                                  size_t len,
                                                  const CCDeadline* deadline) {
    if (!sock || (!data && len > 0)) {
        return cc_err_CCResult_size_t_CCIoError(cc_io_from_errno(EINVAL));
    }
    if (len == 0) {
        return cc_ok_CCResult_size_t_CCIoError(0);
    }

    int prep_err = cc__net_prepare_fiber_fd(sock->fd, &sock->flags);
    if (prep_err != 0) {
        return cc_err_CCResult_size_t_CCIoError(errno_to_io_error(prep_err));
    }
    struct timespec ts;
    const struct timespec* abs_deadline = cc_deadline_as_timespec(deadline, &ts);

    while (1) {
        ssize_t n = send(sock->fd, data, len, CC__NET_SEND_FLAGS);
        int nobufs;
        if (n >= 0) {
            return cc_ok_CCResult_size_t_CCIoError((size_t)n);
        }
        /* ENOBUFS (BSD / Darwin): the kernel could not get an mbuf for this
         * send. The socket is still writable, so it is a transient like
         * EAGAIN, not a peer condition. Retry after the writable wait (which
         * honors the deadline) plus a short park, since the fd reports
         * POLLOUT immediately and a bare retry would spin on the pool.
         *
         * OPEN — BACKPRESSURE POLICY, NOT SETTLED. This keeps every sender
         * alive and lets them all thrash the pool; at c=1000 x 10MB on
         * loopback that halves aggregate bytes versus a server that drops
         * a third of its clients on the first ENOBUFS and serves the rest
         * fast (the pre-retry runtime did exactly that, by accident). The
         * 1ms park is a first cut (sysmon expiry is ~250us; yield-first
         * measured worse). Bounding how many fibers are in send() at once
         * — here, or in the caller — is the real fix. See TODO: ENOBUFS. */
        nobufs = errno == ENOBUFS;
        if (nobufs || errno == EAGAIN || errno == EWOULDBLOCK) {
            cc__io_owned_watcher* watcher = cc__net_ensure_socket_watcher(sock);
            int wait_err = watcher ? cc__io_watcher_wait_deadline(watcher, POLLOUT, abs_deadline)
                                   : cc__io_wait_fd_deadline(sock->fd, POLLOUT, abs_deadline);
            if (wait_err != 0) {
                return cc_err_CCResult_size_t_CCIoError(errno_to_io_error(wait_err));
            }
            if (nobufs) {
                /* Park the fiber, not the worker (cc_sleep_ms would
                 * nanosleep the thread). Sysmon expires the deadline. */
                struct timespec until;
                clock_gettime(CLOCK_REALTIME, &until);
                until.tv_nsec += 1000000L;
                if (until.tv_nsec >= 1000000000L) {
                    until.tv_nsec -= 1000000000L;
                    until.tv_sec += 1;
                }
                if (abs_deadline &&
                    (abs_deadline->tv_sec < until.tv_sec ||
                     (abs_deadline->tv_sec == until.tv_sec &&
                      abs_deadline->tv_nsec < until.tv_nsec)))
                    until = *abs_deadline;
                if (cc__fiber_in_context())
                    (void)CC_FIBER_PARK_IF_UNTIL(NULL, 0, &until, "send_enobufs");
                else
                    cc_sleep_ms(1); /* a thread: there is no one to park */
            }
            continue;
        }
        return cc_err_CCResult_size_t_CCIoError(errno_to_io_error(errno));
    }
}

void cc_socket_shutdown(CCSocket* sock, CCShutdownMode mode, CCNetError* out_err) {
    *out_err = CC_NET_OK;

    int how;
    switch (mode) {
        case CC_SHUTDOWN_READ:  how = SHUT_RD; break;
        case CC_SHUTDOWN_WRITE: how = SHUT_WR; break;
        case CC_SHUTDOWN_BOTH:  how = SHUT_RDWR; break;
        default: how = SHUT_RDWR; break;
    }

    if (shutdown(sock->fd, how) < 0) {
        *out_err = errno_to_net_error(errno);
    }
}

int cc_socket_set_nodelay(CCSocket* sock, int on) {
    int v;
    if (!sock || sock->fd < 0) return -1;
    v = on ? 1 : 0;
    return setsockopt(sock->fd, IPPROTO_TCP, TCP_NODELAY, &v, sizeof(v));
}

void cc_socket_close(CCSocket* sock) {
    if (sock->fd >= 0) {
        if (sock->watcher) {
            cc__io_watcher_destroy((cc__io_owned_watcher*)sock->watcher);
            sock->watcher = NULL;
        } else {
            cc__io_wait_forget_fd(sock->fd);
        }
        close(sock->fd);
        sock->fd = -1;
    }
}

CCSlice cc_socket_peer_addr(CCSocket* sock, CCArena arena, CCNetError* out_err) {
    CCSlice result = {0};
    *out_err = CC_NET_OK;

    struct sockaddr_storage sa;
    socklen_t sa_len = sizeof(sa);

    if (getpeername(sock->fd, (struct sockaddr*)&sa, &sa_len) < 0) {
        *out_err = errno_to_net_error(errno);
        return result;
    }

    char buf[64];
    if (sa.ss_family == AF_INET) {
        struct sockaddr_in* sin = (struct sockaddr_in*)&sa;
        inet_ntop(AF_INET, &sin->sin_addr, buf, sizeof(buf));
        snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), ":%d", ntohs(sin->sin_port));
    } else if (sa.ss_family == AF_INET6) {
        struct sockaddr_in6* sin6 = (struct sockaddr_in6*)&sa;
        buf[0] = '[';
        inet_ntop(AF_INET6, &sin6->sin6_addr, buf + 1, sizeof(buf) - 1);
        snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), "]:%d", ntohs(sin6->sin6_port));
    } else {
        *out_err = CC_NET_OTHER;
        return result;
    }

    size_t len = strlen(buf);
    char* copy = cc_arena_alloc(arena, len, 1);
    if (!copy) {
        *out_err = CC_NET_OTHER;
        return result;
    }
    memcpy(copy, buf, len);

    result.ptr = copy;
    result.len = len;
    return result;
}

CCSlice cc_socket_local_addr(CCSocket* sock, CCArena arena, CCNetError* out_err) {
    CCSlice result = {0};
    *out_err = CC_NET_OK;

    struct sockaddr_storage sa;
    socklen_t sa_len = sizeof(sa);

    if (getsockname(sock->fd, (struct sockaddr*)&sa, &sa_len) < 0) {
        *out_err = errno_to_net_error(errno);
        return result;
    }

    char buf[64];
    if (sa.ss_family == AF_INET) {
        struct sockaddr_in* sin = (struct sockaddr_in*)&sa;
        inet_ntop(AF_INET, &sin->sin_addr, buf, sizeof(buf));
        snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), ":%d", ntohs(sin->sin_port));
    } else if (sa.ss_family == AF_INET6) {
        struct sockaddr_in6* sin6 = (struct sockaddr_in6*)&sa;
        buf[0] = '[';
        inet_ntop(AF_INET6, &sin6->sin6_addr, buf + 1, sizeof(buf) - 1);
        snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), "]:%d", ntohs(sin6->sin6_port));
    } else {
        *out_err = CC_NET_OTHER;
        return result;
    }

    size_t len = strlen(buf);
    char* copy = cc_arena_alloc(arena, len, 1);
    if (!copy) {
        *out_err = CC_NET_OTHER;
        return result;
    }
    memcpy(copy, buf, len);

    result.ptr = copy;
    result.len = len;
    return result;
}

/* ============================================================================
 * UDP — synchronous bind / send_to / recv_from over SOCK_DGRAM sockets.
 * ============================================================================ */

CCUdpSocket cc_udp_bind(const char* addr, size_t addr_len, CCNetError* out_err) {
    CCUdpSocket sock = {.fd = -1, .flags = 0};
    *out_err = CC_NET_OK;

    struct sockaddr_storage sa;
    socklen_t sa_len;
    if (parse_addr(addr, addr_len, &sa, &sa_len, out_err) < 0) {
        return sock;
    }

    int fd = socket(sa.ss_family, SOCK_DGRAM, 0);
    if (fd < 0) {
        *out_err = errno_to_net_error(errno);
        return sock;
    }

    if (bind(fd, (struct sockaddr*)&sa, sa_len) < 0) {
        *out_err = errno_to_net_error(errno);
        close(fd);
        return sock;
    }

    sock.fd = fd;
    return sock;
}

size_t cc_udp_send_to(CCUdpSocket* sock, const char* data, size_t len,
                      const char* addr, size_t addr_len, CCNetError* out_err) {
    *out_err = CC_NET_OK;

    struct sockaddr_storage sa;
    socklen_t sa_len;
    if (parse_addr(addr, addr_len, &sa, &sa_len, out_err) < 0) {
        return 0;
    }

    ssize_t n = sendto(sock->fd, data, len, CC__NET_SEND_FLAGS, (struct sockaddr*)&sa, sa_len);
    if (n < 0) {
        *out_err = errno_to_net_error(errno);
        return 0;
    }

    return (size_t)n;
}

CCUdpPacket cc_udp_recv_from(CCUdpSocket* sock, CCArena arena, size_t max_bytes, CCNetError* out_err) {
    CCUdpPacket pkt = {0};
    *out_err = CC_NET_OK;

    char* buf = cc_arena_alloc(arena, max_bytes, 1);
    if (!buf) {
        *out_err = CC_NET_OTHER;
        return pkt;
    }

    struct sockaddr_storage sa;
    socklen_t sa_len = sizeof(sa);

    ssize_t n = recvfrom(sock->fd, buf, max_bytes, 0, (struct sockaddr*)&sa, &sa_len);
    if (n < 0) {
        *out_err = errno_to_net_error(errno);
        return pkt;
    }

    pkt.data.ptr = buf;
    pkt.data.len = (size_t)n;

    /* Format sender address */
    char addr_buf[64];
    if (sa.ss_family == AF_INET) {
        struct sockaddr_in* sin = (struct sockaddr_in*)&sa;
        inet_ntop(AF_INET, &sin->sin_addr, addr_buf, sizeof(addr_buf));
        snprintf(addr_buf + strlen(addr_buf), sizeof(addr_buf) - strlen(addr_buf), ":%d", ntohs(sin->sin_port));
    } else if (sa.ss_family == AF_INET6) {
        struct sockaddr_in6* sin6 = (struct sockaddr_in6*)&sa;
        addr_buf[0] = '[';
        inet_ntop(AF_INET6, &sin6->sin6_addr, addr_buf + 1, sizeof(addr_buf) - 1);
        snprintf(addr_buf + strlen(addr_buf), sizeof(addr_buf) - strlen(addr_buf), "]:%d", ntohs(sin6->sin6_port));
    }

    size_t addr_len = strlen(addr_buf);
    char* addr_copy = cc_arena_alloc(arena, addr_len, 1);
    if (addr_copy) {
        memcpy(addr_copy, addr_buf, addr_len);
        pkt.from_addr.ptr = addr_copy;
        pkt.from_addr.len = addr_len;
    }

    return pkt;
}

void cc_udp_close(CCUdpSocket* sock) {
    if (sock->fd >= 0) {
        close(sock->fd);
        sock->fd = -1;
    }
}

/* ============================================================================
 * DNS
 * ============================================================================ */

CCSlice cc_dns_lookup(CCArena arena, const char* hostname, size_t hostname_len, CCNetError* out_err) {
    CCSlice result = {0};
    *out_err = CC_NET_OK;

    /* Null-terminate */
    char buf[256];
    if (hostname_len >= sizeof(buf)) {
        *out_err = CC_NET_INVALID_ADDRESS;
        return result;
    }
    memcpy(buf, hostname, hostname_len);
    buf[hostname_len] = '\0';

    struct addrinfo hints = {0};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo* res = NULL;
    int err = getaddrinfo(buf, NULL, &hints, &res);
    if (err != 0) {
        *out_err = CC_NET_DNS_FAILURE;
        return result;
    }

    /* Count results */
    size_t count = 0;
    for (struct addrinfo* p = res; p; p = p->ai_next) {
        if (p->ai_family == AF_INET || p->ai_family == AF_INET6) {
            count++;
        }
    }

    if (count == 0) {
        freeaddrinfo(res);
        *out_err = CC_NET_DNS_FAILURE;
        return result;
    }

    /* Allocate array */
    CCIpAddr* addrs = cc_arena_alloc(arena, count * sizeof(CCIpAddr), _Alignof(CCIpAddr));
    if (!addrs) {
        freeaddrinfo(res);
        *out_err = CC_NET_OTHER;
        return result;
    }

    /* Copy addresses */
    size_t i = 0;
    for (struct addrinfo* p = res; p && i < count; p = p->ai_next) {
        if (p->ai_family == AF_INET) {
            struct sockaddr_in* sin = (struct sockaddr_in*)p->ai_addr;
            addrs[i].family = 4;
            memcpy(addrs[i].addr.v4, &sin->sin_addr, 4);
            i++;
        } else if (p->ai_family == AF_INET6) {
            struct sockaddr_in6* sin6 = (struct sockaddr_in6*)p->ai_addr;
            addrs[i].family = 6;
            memcpy(addrs[i].addr.v6, &sin6->sin6_addr, 16);
            i++;
        }
    }

    freeaddrinfo(res);

    result.ptr = (char*)addrs;
    result.len = count;  /* Note: len is count of CCIpAddr, not bytes */
    return result;
}

CCSlice cc_ip_addr_to_string(CCIpAddr* addr, CCArena arena) {
    CCSlice result = {0};

    char buf[64];
    if (addr->family == 4) {
        inet_ntop(AF_INET, addr->addr.v4, buf, sizeof(buf));
    } else if (addr->family == 6) {
        inet_ntop(AF_INET6, addr->addr.v6, buf, sizeof(buf));
    } else {
        return result;
    }

    size_t len = strlen(buf);
    char* copy = cc_arena_alloc(arena, len, 1);
    if (!copy) return result;
    memcpy(copy, buf, len);

    result.ptr = copy;
    result.len = len;
    return result;
}

CCIpAddr cc_ip_parse(const char* s, size_t len, CCNetError* out_err) {
    CCIpAddr addr = {0};
    *out_err = CC_NET_OK;

    char buf[64];
    if (len >= sizeof(buf)) {
        *out_err = CC_NET_INVALID_ADDRESS;
        return addr;
    }
    memcpy(buf, s, len);
    buf[len] = '\0';

    /* Try IPv4 first */
    struct in_addr in4;
    if (inet_pton(AF_INET, buf, &in4) == 1) {
        addr.family = 4;
        memcpy(addr.addr.v4, &in4, 4);
        return addr;
    }

    /* Try IPv6 */
    struct in6_addr in6;
    if (inet_pton(AF_INET6, buf, &in6) == 1) {
        addr.family = 6;
        memcpy(addr.addr.v6, &in6, 16);
        return addr;
    }

    *out_err = CC_NET_INVALID_ADDRESS;
    return addr;
}
