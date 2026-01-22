/*
 * Concurrent-C Networking Runtime
 *
 * POSIX socket implementation.
 * Async variants require runtime scheduler integration.
 */

#include <ccc/std/net.cch>

#include <errno.h>
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

/* ============================================================================
 * Helpers
 * ============================================================================ */

static CCNetError errno_to_net_error(int err) {
    switch (err) {
        case ECONNREFUSED: return CC_NET_CONNECTION_REFUSED;
        case ECONNRESET:   return CC_NET_CONNECTION_RESET;
        case ETIMEDOUT:    return CC_NET_TIMED_OUT;
        case EHOSTUNREACH: return CC_NET_HOST_UNREACHABLE;
        case ENETUNREACH:  return CC_NET_NETWORK_UNREACHABLE;
        case EADDRINUSE:   return CC_NET_ADDRESS_IN_USE;
        case EADDRNOTAVAIL: return CC_NET_ADDRESS_NOT_AVAILABLE;
        default:           return CC_NET_OTHER;
    }
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
    CCSocket sock = {.fd = -1, .flags = 0};
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

    sock.fd = fd;
    return sock;
}

/* ============================================================================
 * TCP Server
 * ============================================================================ */

CCListener cc_tcp_listen(const char* addr, size_t addr_len, CCNetError* out_err) {
    CCListener ln = {.fd = -1, .flags = 0};
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
    return ln;
}

CCSocket cc_listener_accept(CCListener* ln, CCNetError* out_err) {
    CCSocket sock = {.fd = -1, .flags = 0};
    *out_err = CC_NET_OK;

    struct sockaddr_storage client_addr;
    socklen_t client_len = sizeof(client_addr);

    int fd = accept(ln->fd, (struct sockaddr*)&client_addr, &client_len);
    if (fd < 0) {
        *out_err = errno_to_net_error(errno);
        return sock;
    }

    sock.fd = fd;
    return sock;
}

void cc_listener_close(CCListener* ln) {
    if (ln->fd >= 0) {
        close(ln->fd);
        ln->fd = -1;
    }
}

/* ============================================================================
 * Socket I/O
 * ============================================================================ */

CCSlice cc_socket_read(CCSocket* sock, CCArena* arena, size_t max_bytes, CCNetError* out_err) {
    CCSlice result = {0};
    *out_err = CC_NET_OK;

    char* buf = cc_arena_alloc(arena, max_bytes, 1);
    if (!buf) {
        *out_err = CC_NET_OTHER;
        return result;
    }

    ssize_t n = read(sock->fd, buf, max_bytes);
    if (n < 0) {
        *out_err = errno_to_net_error(errno);
        return result;
    }
    if (n == 0) {
        *out_err = CC_NET_CONNECTION_CLOSED;
        return result;
    }

    result.ptr = buf;
    result.len = (size_t)n;
    return result;
}

size_t cc_socket_write(CCSocket* sock, const char* data, size_t len, CCNetError* out_err) {
    *out_err = CC_NET_OK;

    ssize_t n = write(sock->fd, data, len);
    if (n < 0) {
        *out_err = errno_to_net_error(errno);
        return 0;
    }

    return (size_t)n;
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
