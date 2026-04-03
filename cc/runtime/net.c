/*
 * Concurrent-C Networking Runtime
 *
 * POSIX socket implementation.
 * Async variants require runtime scheduler integration.
 */

#include <ccc/std/net.cch>
#include <ccc/cc_channel.cch>

#include <errno.h>
#include <poll.h>
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

static int cc__net_prepare_fiber_fd(int fd, uint8_t* flags) {
    if (!cc__fiber_in_context()) return 0;
    if (flags && (*flags & CC_NET_FLAG_NONBLOCK)) return 0;
    int err = cc__net_set_nonblocking(fd);
    if (err == 0 && flags) *flags |= CC_NET_FLAG_NONBLOCK;
    return err;
}

CCSocketOrRecvWaitKind cc_socket_wait_readable_or_recv(CCSocket* sock,
                                                       CCChanRx rx,
                                                       void* out_value,
                                                       size_t value_size,
                                                       CCNetError* out_err) {
    if (out_err) *out_err = CC_NET_OK;
    if (!sock || sock->fd < 0 || !rx.raw || !out_value || value_size == 0) {
        if (out_err) *out_err = CC_NET_OTHER;
        return CC_SOCKET_OR_RECV_WAIT_ERROR;
    }

    int prep_err = cc__net_prepare_fiber_fd(sock->fd, &sock->flags);
    if (prep_err != 0) {
        if (out_err) *out_err = errno_to_net_error(prep_err);
        return CC_SOCKET_OR_RECV_WAIT_ERROR;
    }

    int rc = cc_chan_try_recv(rx.raw, out_value, value_size);
    if (rc == 0) return CC_SOCKET_OR_RECV_WAIT_RECV;
    if (rc == EPIPE) return CC_SOCKET_OR_RECV_WAIT_RECV_CLOSED;
    if (rc != EAGAIN) {
        if (out_err) *out_err = CC_NET_OTHER;
        return CC_SOCKET_OR_RECV_WAIT_ERROR;
    }

    cc__io_owned_watcher* watcher = cc__net_ensure_socket_watcher(sock);
    if (!watcher) {
        int wait_err = cc__io_wait_fd(sock->fd, POLLIN);
        if (wait_err != 0) {
            if (out_err) *out_err = errno_to_net_error(wait_err);
            return CC_SOCKET_OR_RECV_WAIT_ERROR;
        }
        return CC_SOCKET_OR_RECV_WAIT_SOCKET;
    }

    cc__wait_select_group group = {
        .fiber = (cc__fiber*)cc__fiber_current(),
        .signaled = 0,
        .selected_index = -1,
    };
    cc__fiber_wait_node recv_node = {0};
    int recv_pub = cc__chan_publish_recv_wait_select(rx.raw, &recv_node, out_value, &group, 0);
    if (recv_pub == CC__CHAN_WAIT_DATA) return CC_SOCKET_OR_RECV_WAIT_RECV;
    if (recv_pub == CC__CHAN_WAIT_CLOSE) return CC_SOCKET_OR_RECV_WAIT_RECV_CLOSED;
    if (recv_pub == CC__CHAN_WAIT_SIGNAL) {
        rc = cc_chan_try_recv(rx.raw, out_value, value_size);
        if (rc == 0) return CC_SOCKET_OR_RECV_WAIT_RECV;
        if (rc == EPIPE) return CC_SOCKET_OR_RECV_WAIT_RECV_CLOSED;
    }

    cc__io_wait_select_handle io_handle = {0};
    int io_pub = cc__io_wait_select_publish(watcher, POLLIN, &group, 1, &io_handle);
    if (io_pub != 0) {
        (void)cc__chan_finish_recv_wait_select(rx.raw, &recv_node);
        if (out_err) *out_err = errno_to_net_error(io_pub);
        return CC_SOCKET_OR_RECV_WAIT_ERROR;
    }

    if (atomic_load_explicit(&group.signaled, memory_order_acquire) == 0) {
        cc_sched_wait_on_flag(&group.signaled, 0, "socket_or_recv_wait");
    }

    int selected = atomic_load_explicit(&group.selected_index, memory_order_acquire);
    cc__io_wait_select_finish(&io_handle);
    int notify = cc__chan_finish_recv_wait_select(rx.raw, &recv_node);

    if (selected == 0 || notify == CC_CHAN_NOTIFY_DATA || notify == CC_CHAN_NOTIFY_SIGNAL) {
        if (notify == CC_CHAN_NOTIFY_SIGNAL) {
            rc = cc_chan_try_recv(rx.raw, out_value, value_size);
            if (rc == 0) return CC_SOCKET_OR_RECV_WAIT_RECV;
            if (rc == EPIPE) return CC_SOCKET_OR_RECV_WAIT_RECV_CLOSED;
            if (out_err) *out_err = CC_NET_OTHER;
            return CC_SOCKET_OR_RECV_WAIT_ERROR;
        }
        if (notify == CC_CHAN_NOTIFY_DATA) return CC_SOCKET_OR_RECV_WAIT_RECV;
        if (selected == 0) return CC_SOCKET_OR_RECV_WAIT_RECV;
    }
    if (notify == CC_CHAN_NOTIFY_CLOSE) return CC_SOCKET_OR_RECV_WAIT_RECV_CLOSED;
    return CC_SOCKET_OR_RECV_WAIT_SOCKET;
}

static int cc__net_trace_read_enabled(void) {
    static int mode = -1;
    if (mode >= 0) return mode;
    const char* env = getenv("CC_NET_TRACE_READ");
    mode = (env && env[0] && !(env[0] == '0' && env[1] == '\0')) ? 1 : 0;
    return mode;
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

CCSocket cc_tcp_connect(const char* addr, size_t addr_len, CCNetError* out_err) {
    CCSocket sock = {.fd = -1, .flags = 0, .watcher = NULL};
    *out_err = CC_NET_OK;

    struct sockaddr_storage sa;
    socklen_t sa_len;
    if (parse_addr(addr, addr_len, &sa, &sa_len, out_err) < 0) {
        return sock;
    }

    int fd = socket(sa.ss_family, SOCK_STREAM, 0);
    if (fd < 0) {
        *out_err = errno_to_net_error(errno);
        return sock;
    }

    if (connect(fd, (struct sockaddr*)&sa, sa_len) < 0) {
        *out_err = errno_to_net_error(errno);
        close(fd);
        return sock;
    }

    cc__net_disable_sigpipe_best_effort(fd);
    sock.fd = fd;
    return sock;
}

/* ============================================================================
 * TCP Server
 * ============================================================================ */

CCListener cc_tcp_listen(const char* addr, size_t addr_len, CCNetError* out_err) {
    CCListener ln = {.fd = -1, .flags = 0, .watcher = NULL};
    *out_err = CC_NET_OK;

    struct sockaddr_storage sa;
    socklen_t sa_len;
    if (parse_addr(addr, addr_len, &sa, &sa_len, out_err) < 0) {
        return ln;
    }

    int fd = socket(sa.ss_family, SOCK_STREAM, 0);
    if (fd < 0) {
        *out_err = errno_to_net_error(errno);
        return ln;
    }

    /* Allow address reuse */
    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    if (bind(fd, (struct sockaddr*)&sa, sa_len) < 0) {
        *out_err = errno_to_net_error(errno);
        close(fd);
        return ln;
    }

    if (listen(fd, 128) < 0) {
        *out_err = errno_to_net_error(errno);
        close(fd);
        return ln;
    }

    ln.fd = fd;
    ln.watcher = cc__io_watcher_create(fd);
    return ln;
}

CCSocket cc_listener_accept(CCListener* ln, CCNetError* out_err) {
    CCSocket sock = {.fd = -1, .flags = 0, .watcher = NULL};
    *out_err = CC_NET_OK;

    struct sockaddr_storage client_addr;
    socklen_t client_len = sizeof(client_addr);
    int prep_err = cc__net_prepare_fiber_fd(ln->fd, &ln->flags);
    if (prep_err != 0) {
        *out_err = errno_to_net_error(prep_err);
        return sock;
    }

    while (1) {
        int fd = accept(ln->fd, (struct sockaddr*)&client_addr, &client_len);
        if (fd >= 0) {
            int fd_err = cc__net_set_nonblocking(fd);
            if (fd_err != 0) {
                *out_err = errno_to_net_error(fd_err);
                close(fd);
                return sock;
            }
            cc__net_set_cloexec_best_effort(fd);
            cc__net_disable_sigpipe_best_effort(fd);
            sock.fd = fd;
            sock.flags |= CC_NET_FLAG_NONBLOCK;
            return sock;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            cc__io_owned_watcher* watcher = cc__net_ensure_listener_watcher(ln);
            int wait_err = watcher ? cc__io_watcher_wait(watcher, POLLIN)
                                   : cc__io_wait_fd(ln->fd, POLLIN);
            if (wait_err != 0) {
                *out_err = errno_to_net_error(wait_err);
                return sock;
            }
            client_len = sizeof(client_addr);
            continue;
        }
        *out_err = errno_to_net_error(errno);
        return sock;
    }
}

void cc_listener_close(CCListener* ln) {
    if (ln->fd >= 0) {
        if (ln->watcher) {
            cc__io_watcher_destroy((cc__io_owned_watcher*)ln->watcher);
            ln->watcher = NULL;
        } else {
            cc__io_wait_forget_fd(ln->fd);
        }
        close(ln->fd);
        ln->fd = -1;
    }
}

/* ============================================================================
 * Socket I/O
 * ============================================================================ */

size_t cc_socket_read_into(CCSocket* sock, char* buf, size_t max_bytes, CCNetError* out_err) {
    *out_err = CC_NET_OK;
    if (!buf && max_bytes > 0) {
        *out_err = CC_NET_OTHER;
        return 0;
    }

    int prep_err = cc__net_prepare_fiber_fd(sock->fd, &sock->flags);
    if (prep_err != 0) {
        *out_err = errno_to_net_error(prep_err);
        return 0;
    }

    while (1) {
        ssize_t n = read(sock->fd, buf, max_bytes);
        if (n > 0) {
            cc__net_trace_read("read_ok", sock->fd, n, 0);
            return (size_t)n;
        }
        if (n == 0) {
            cc__net_trace_read("read_eof", sock->fd, n, 0);
            *out_err = CC_NET_CONNECTION_CLOSED;
            return 0;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            cc__net_trace_read("wait_begin", sock->fd, n, errno);
            cc__io_owned_watcher* watcher = cc__net_ensure_socket_watcher(sock);
            int wait_err = watcher ? cc__io_watcher_wait(watcher, POLLIN)
                                   : cc__io_wait_fd(sock->fd, POLLIN);
            cc__net_trace_read("wait_end", sock->fd, n, wait_err);
            if (wait_err != 0) {
                *out_err = errno_to_net_error(wait_err);
                return 0;
            }
            continue;
        }
        cc__net_trace_read("read_err", sock->fd, n, errno);
        *out_err = errno_to_net_error(errno);
        return 0;
    }
}

CCSlice cc_socket_read(CCSocket* sock, CCArena* arena, size_t max_bytes, CCNetError* out_err) {
    CCSlice result = {0};
    *out_err = CC_NET_OK;

    char* buf = cc_arena_alloc(arena, max_bytes, 1);
    if (!buf) {
        *out_err = CC_NET_OTHER;
        return result;
    }

    size_t n = cc_socket_read_into(sock, buf, max_bytes, out_err);
    if (*out_err == CC_NET_OK && n > 0) {
        result.ptr = buf;
        result.len = n;
    }
    return result;
}

size_t cc_socket_write(CCSocket* sock, const char* data, size_t len, CCNetError* out_err) {
    *out_err = CC_NET_OK;
    int prep_err = cc__net_prepare_fiber_fd(sock->fd, &sock->flags);
    if (prep_err != 0) {
        *out_err = errno_to_net_error(prep_err);
        return 0;
    }

    while (1) {
        ssize_t n = write(sock->fd, data, len);
        if (n >= 0) {
            return (size_t)n;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            cc__io_owned_watcher* watcher = cc__net_ensure_socket_watcher(sock);
            int wait_err = watcher ? cc__io_watcher_wait(watcher, POLLOUT)
                                   : cc__io_wait_fd(sock->fd, POLLOUT);
            if (wait_err != 0) {
                *out_err = errno_to_net_error(wait_err);
                return 0;
            }
            continue;
        }
        *out_err = errno_to_net_error(errno);
        return 0;
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

CCSlice cc_socket_peer_addr(CCSocket* sock, CCArena* arena, CCNetError* out_err) {
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

CCSlice cc_socket_local_addr(CCSocket* sock, CCArena* arena, CCNetError* out_err) {
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
 * UDP (stubs for now)
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

    ssize_t n = sendto(sock->fd, data, len, 0, (struct sockaddr*)&sa, sa_len);
    if (n < 0) {
        *out_err = errno_to_net_error(errno);
        return 0;
    }

    return (size_t)n;
}

CCUdpPacket cc_udp_recv_from(CCUdpSocket* sock, CCArena* arena, size_t max_bytes, CCNetError* out_err) {
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

CCSlice cc_dns_lookup(CCArena* arena, const char* hostname, size_t hostname_len, CCNetError* out_err) {
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

CCSlice cc_ip_addr_to_string(CCIpAddr* addr, CCArena* arena) {
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
