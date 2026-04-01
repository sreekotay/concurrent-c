/*
 * Concurrent-C Networking Runtime
 *
 * POSIX socket implementation.
 * Async variants require runtime scheduler integration.
 */

#include <ccc/std/net.cch>

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

typedef struct cc_net_waiter {
    struct cc_net_waiter* next;
    struct cc_net_waiter* prev;
    int fd;
    short events;
    void* fiber;
    uint64_t wait_ticket;
    _Atomic int ready;
    _Atomic int cancelled;
    _Atomic int refs;
    int linked;
} cc_net_waiter;

typedef struct {
    pthread_mutex_t mu;
    pthread_once_t once;
    pthread_t thread;
    int wake_pipe[2];
    int init_err;
    cc_net_waiter* head;
} cc_net_wait_state;

static cc_net_wait_state g_cc_net_wait_state = {
    .mu = PTHREAD_MUTEX_INITIALIZER,
    .once = PTHREAD_ONCE_INIT,
    .wake_pipe = {-1, -1},
    .init_err = 0,
    .head = NULL,
};

typedef struct {
    _Atomic int enabled;
    _Atomic int init;
    _Atomic int atexit_registered;
    _Atomic uint64_t wait_async_calls;
    _Atomic uint64_t waiter_adds;
    _Atomic uint64_t waiter_removes;
    _Atomic uint64_t wake_notifications;
    _Atomic uint64_t poll_loops;
    _Atomic uint64_t poll_wake_pipe_hits;
    _Atomic uint64_t poll_ready_slots;
    _Atomic uint64_t current_waiters;
    _Atomic uint64_t max_waiters;
} cc_net_watch_stats;

static cc_net_watch_stats g_cc_net_watch_stats = {
    .enabled = -1,
    .init = 0,
    .atexit_registered = 0,
};

/* One background watcher owns the kernel readiness wait so scheduler workers
 * stay imperative and fiber-focused: socket ops keep local retry loops, fibers
 * park on "would block", and no worker is pinned in poll() or spinning on
 * EAGAIN while external I/O progress is pending. */

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

static int cc__net_wait_ready(int fd, short events) {
    struct pollfd pfd = {.fd = fd, .events = events};
    while (1) {
        int rc = poll(&pfd, 1, -1);
        if (rc > 0) {
            if (pfd.revents & POLLNVAL) return EBADF;
            if (pfd.revents & (POLLERR | POLLHUP)) return EIO;
            return 0;
        }
        if (rc == 0) continue;
        if (errno == EINTR) continue;
        return errno;
    }
}

static int cc__net_prepare_fiber_fd(int fd) {
    if (!cc__fiber_in_context()) return 0;
    return cc__net_set_nonblocking(fd);
}

static int cc__net_watch_stats_enabled(void) {
    int mode = atomic_load_explicit(&g_cc_net_watch_stats.enabled, memory_order_acquire);
    if (mode >= 0) return mode;
    mode = getenv("CC_NET_WATCH_STATS") ? 1 : 0;
    int expected = -1;
    (void)atomic_compare_exchange_strong_explicit(&g_cc_net_watch_stats.enabled,
                                                  &expected,
                                                  mode,
                                                  memory_order_release,
                                                  memory_order_acquire);
    return atomic_load_explicit(&g_cc_net_watch_stats.enabled, memory_order_acquire);
}

static void cc__net_watch_stats_dump(void) {
    if (!cc__net_watch_stats_enabled()) return;
    fprintf(stderr,
            "\n[cc:net] watcher stats: waits=%llu adds=%llu removes=%llu notify=%llu "
            "poll_loops=%llu wake_pipe_hits=%llu ready_slots=%llu current=%llu max=%llu\n",
            (unsigned long long)atomic_load_explicit(&g_cc_net_watch_stats.wait_async_calls, memory_order_relaxed),
            (unsigned long long)atomic_load_explicit(&g_cc_net_watch_stats.waiter_adds, memory_order_relaxed),
            (unsigned long long)atomic_load_explicit(&g_cc_net_watch_stats.waiter_removes, memory_order_relaxed),
            (unsigned long long)atomic_load_explicit(&g_cc_net_watch_stats.wake_notifications, memory_order_relaxed),
            (unsigned long long)atomic_load_explicit(&g_cc_net_watch_stats.poll_loops, memory_order_relaxed),
            (unsigned long long)atomic_load_explicit(&g_cc_net_watch_stats.poll_wake_pipe_hits, memory_order_relaxed),
            (unsigned long long)atomic_load_explicit(&g_cc_net_watch_stats.poll_ready_slots, memory_order_relaxed),
            (unsigned long long)atomic_load_explicit(&g_cc_net_watch_stats.current_waiters, memory_order_relaxed),
            (unsigned long long)atomic_load_explicit(&g_cc_net_watch_stats.max_waiters, memory_order_relaxed));
}

static void cc__net_watch_stats_init(void) {
    if (!cc__net_watch_stats_enabled()) return;
    if (atomic_exchange_explicit(&g_cc_net_watch_stats.init, 1, memory_order_acq_rel)) return;
    if (!atomic_exchange_explicit(&g_cc_net_watch_stats.atexit_registered, 1, memory_order_acq_rel)) {
        atexit(cc__net_watch_stats_dump);
    }
}

static void cc__net_watch_stats_inc_current_waiters(void) {
    uint64_t cur = atomic_fetch_add_explicit(&g_cc_net_watch_stats.current_waiters, 1, memory_order_relaxed) + 1;
    uint64_t max = atomic_load_explicit(&g_cc_net_watch_stats.max_waiters, memory_order_relaxed);
    while (cur > max &&
           !atomic_compare_exchange_weak_explicit(&g_cc_net_watch_stats.max_waiters,
                                                  &max,
                                                  cur,
                                                  memory_order_relaxed,
                                                  memory_order_relaxed)) {
    }
}

static void cc__net_watch_stats_dec_current_waiters(void) {
    atomic_fetch_sub_explicit(&g_cc_net_watch_stats.current_waiters, 1, memory_order_relaxed);
}

static void cc__net_waiter_addref(cc_net_waiter* waiter) {
    if (!waiter) return;
    atomic_fetch_add_explicit(&waiter->refs, 1, memory_order_relaxed);
}

static void cc__net_waiter_release(cc_net_waiter* waiter) {
    if (!waiter) return;
    if (atomic_fetch_sub_explicit(&waiter->refs, 1, memory_order_acq_rel) == 1) {
        free(waiter);
    }
}

static void cc__net_waiter_notify(void) {
    if (g_cc_net_wait_state.wake_pipe[1] < 0) return;
    if (cc__net_watch_stats_enabled()) {
        cc__net_watch_stats_init();
        atomic_fetch_add_explicit(&g_cc_net_watch_stats.wake_notifications, 1, memory_order_relaxed);
    }
    unsigned char byte = 0;
    ssize_t rc;
    do {
        rc = write(g_cc_net_wait_state.wake_pipe[1], &byte, 1);
    } while (rc < 0 && errno == EINTR);
}

static void cc__net_waiter_drain_wake_pipe(void) {
    if (g_cc_net_wait_state.wake_pipe[0] < 0) return;
    char buf[128];
    while (1) {
        ssize_t n = read(g_cc_net_wait_state.wake_pipe[0], buf, sizeof(buf));
        if (n > 0) continue;
        if (n < 0 && errno == EINTR) continue;
        break;
    }
}

static void* cc__net_waiter_main(void* arg) {
    (void)arg;
    while (1) {
        struct pollfd* pfds = NULL;
        cc_net_waiter** waiters = NULL;
        size_t count = 0;
        size_t idx = 1;

        pthread_mutex_lock(&g_cc_net_wait_state.mu);
        for (cc_net_waiter* w = g_cc_net_wait_state.head; w; w = w->next) count++;
        pfds = (struct pollfd*)calloc(count + 1, sizeof(*pfds));
        waiters = (cc_net_waiter**)calloc(count ? count : 1, sizeof(*waiters));
        if (!pfds || !waiters) {
            pthread_mutex_unlock(&g_cc_net_wait_state.mu);
            free(pfds);
            free(waiters);
            usleep(1000);
            continue;
        }
        pfds[0].fd = g_cc_net_wait_state.wake_pipe[0];
        pfds[0].events = POLLIN;
        for (cc_net_waiter* w = g_cc_net_wait_state.head; w; w = w->next) {
            cc__net_waiter_addref(w);
            waiters[idx - 1] = w;
            pfds[idx].fd = w->fd;
            pfds[idx].events = w->events;
            idx++;
        }
        pthread_mutex_unlock(&g_cc_net_wait_state.mu);

        while (1) {
            int rc = poll(pfds, idx, -1);
            if (rc >= 0) break;
            if (errno == EINTR) continue;
            break;
        }
        if (cc__net_watch_stats_enabled()) {
            cc__net_watch_stats_init();
            atomic_fetch_add_explicit(&g_cc_net_watch_stats.poll_loops, 1, memory_order_relaxed);
        }

        if (pfds[0].revents & POLLIN) {
            if (cc__net_watch_stats_enabled()) {
                atomic_fetch_add_explicit(&g_cc_net_watch_stats.poll_wake_pipe_hits, 1, memory_order_relaxed);
            }
            cc__net_waiter_drain_wake_pipe();
        }

        for (size_t i = 1; i < idx; ++i) {
            cc_net_waiter* w = waiters[i - 1];
            short revents = pfds[i].revents;
            if (!w || revents == 0) continue;
            if (cc__net_watch_stats_enabled()) {
                atomic_fetch_add_explicit(&g_cc_net_watch_stats.poll_ready_slots, 1, memory_order_relaxed);
            }
            if (atomic_load_explicit(&w->cancelled, memory_order_acquire)) continue;
            if (!(revents & (w->events | POLLERR | POLLHUP | POLLNVAL))) continue;
            if (w->fiber && !cc__fiber_wait_ticket_matches(w->fiber, w->wait_ticket)) continue;
            if (atomic_exchange_explicit(&w->ready, 1, memory_order_acq_rel) == 0) {
                cc__fiber_unpark(w->fiber);
            }
        }

        for (size_t i = 0; i + 1 < idx; ++i) {
            cc__net_waiter_release(waiters[i]);
        }
        free(waiters);
        free(pfds);
    }
    return NULL;
}

static void cc__net_waiter_init_once(void) {
    if (pipe(g_cc_net_wait_state.wake_pipe) != 0) {
        g_cc_net_wait_state.init_err = errno;
        g_cc_net_wait_state.wake_pipe[0] = -1;
        g_cc_net_wait_state.wake_pipe[1] = -1;
        return;
    }
    if (cc__net_set_nonblocking(g_cc_net_wait_state.wake_pipe[0]) != 0 ||
        cc__net_set_nonblocking(g_cc_net_wait_state.wake_pipe[1]) != 0) {
        g_cc_net_wait_state.init_err = errno ? errno : EIO;
        close(g_cc_net_wait_state.wake_pipe[0]);
        close(g_cc_net_wait_state.wake_pipe[1]);
        g_cc_net_wait_state.wake_pipe[0] = -1;
        g_cc_net_wait_state.wake_pipe[1] = -1;
        return;
    }
    int err = pthread_create(&g_cc_net_wait_state.thread, NULL, cc__net_waiter_main, NULL);
    if (err != 0) {
        g_cc_net_wait_state.init_err = err;
        close(g_cc_net_wait_state.wake_pipe[0]);
        close(g_cc_net_wait_state.wake_pipe[1]);
        g_cc_net_wait_state.wake_pipe[0] = -1;
        g_cc_net_wait_state.wake_pipe[1] = -1;
        return;
    }
    pthread_detach(g_cc_net_wait_state.thread);
}

static int cc__net_waiter_ensure_running(void) {
    pthread_once(&g_cc_net_wait_state.once, cc__net_waiter_init_once);
    return g_cc_net_wait_state.init_err;
}

static int cc__net_wait_async(int fd, short events) {
    int init_err = cc__net_waiter_ensure_running();
    if (init_err != 0) return cc__net_wait_ready(fd, events);
    if (cc__net_watch_stats_enabled()) {
        cc__net_watch_stats_init();
        atomic_fetch_add_explicit(&g_cc_net_watch_stats.wait_async_calls, 1, memory_order_relaxed);
    }

    cc_net_waiter* waiter = (cc_net_waiter*)calloc(1, sizeof(*waiter));
    if (!waiter) return cc__net_wait_ready(fd, events);

    waiter->fd = fd;
    waiter->events = events;
    waiter->fiber = cc__fiber_current();
    waiter->wait_ticket = cc__fiber_publish_wait_ticket(waiter->fiber);
    atomic_store_explicit(&waiter->ready, 0, memory_order_relaxed);
    atomic_store_explicit(&waiter->cancelled, 0, memory_order_relaxed);
    atomic_store_explicit(&waiter->refs, 1, memory_order_relaxed);

    pthread_mutex_lock(&g_cc_net_wait_state.mu);
    waiter->next = g_cc_net_wait_state.head;
    if (waiter->next) waiter->next->prev = waiter;
    g_cc_net_wait_state.head = waiter;
    waiter->linked = 1;
    pthread_mutex_unlock(&g_cc_net_wait_state.mu);
    if (cc__net_watch_stats_enabled()) {
        atomic_fetch_add_explicit(&g_cc_net_watch_stats.waiter_adds, 1, memory_order_relaxed);
        cc__net_watch_stats_inc_current_waiters();
    }
    cc__net_waiter_notify();

    cc__fiber_set_park_obj(waiter);
    CC_FIBER_SUSPEND_UNTIL_READY(&waiter->ready, 0, "io_ready");
    cc__fiber_set_park_obj(NULL);

    atomic_store_explicit(&waiter->cancelled, 1, memory_order_release);
    pthread_mutex_lock(&g_cc_net_wait_state.mu);
    if (waiter->linked) {
        if (waiter->prev) waiter->prev->next = waiter->next;
        else g_cc_net_wait_state.head = waiter->next;
        if (waiter->next) waiter->next->prev = waiter->prev;
        waiter->linked = 0;
    }
    pthread_mutex_unlock(&g_cc_net_wait_state.mu);
    if (cc__net_watch_stats_enabled()) {
        atomic_fetch_add_explicit(&g_cc_net_watch_stats.waiter_removes, 1, memory_order_relaxed);
        cc__net_watch_stats_dec_current_waiters();
    }
    cc__net_waiter_notify();
    cc__net_waiter_release(waiter);
    return 0;
}

static int cc__net_wait_would_block(int fd, short events) {
    if (cc__fiber_in_context()) {
        return cc__net_wait_async(fd, events);
    }
    return cc__net_wait_ready(fd, events);
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
    int prep_err = cc__net_prepare_fiber_fd(ln->fd);
    if (prep_err != 0) {
        *out_err = errno_to_net_error(prep_err);
        return sock;
    }

    while (1) {
        int fd = accept(ln->fd, (struct sockaddr*)&client_addr, &client_len);
        if (fd >= 0) {
            sock.fd = fd;
            return sock;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            int wait_err = cc__net_wait_would_block(ln->fd, POLLIN);
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

    int prep_err = cc__net_prepare_fiber_fd(sock->fd);
    if (prep_err != 0) {
        *out_err = errno_to_net_error(prep_err);
        return result;
    }

    while (1) {
        ssize_t n = read(sock->fd, buf, max_bytes);
        if (n > 0) {
            result.ptr = buf;
            result.len = (size_t)n;
            return result;
        }
        if (n == 0) {
            *out_err = CC_NET_CONNECTION_CLOSED;
            return result;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            int wait_err = cc__net_wait_would_block(sock->fd, POLLIN);
            if (wait_err != 0) {
                *out_err = errno_to_net_error(wait_err);
                return result;
            }
            continue;
        }
        *out_err = errno_to_net_error(errno);
        return result;
    }
}

size_t cc_socket_write(CCSocket* sock, const char* data, size_t len, CCNetError* out_err) {
    *out_err = CC_NET_OK;
    int prep_err = cc__net_prepare_fiber_fd(sock->fd);
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
            int wait_err = cc__net_wait_would_block(sock->fd, POLLOUT);
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
