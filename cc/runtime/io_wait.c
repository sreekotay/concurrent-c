#include "io_wait.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__) || defined(__DragonFly__)
#include <sys/event.h>
#include <sys/time.h>
#define CC_IO_WAIT_HAS_KQUEUE 1
#else
#define CC_IO_WAIT_HAS_KQUEUE 0
#endif

#include "fiber_internal.h"
#include "wait_select_internal.h"

typedef struct cc_io_waiter {
    struct cc_io_waiter* next;
    struct cc_io_waiter* prev;
    int fd;
    short events;
    void* fiber;
    uint64_t wait_ticket;
    _Atomic int ready;
    _Atomic int cancelled;
    _Atomic int refs;
    _Atomic int backend_registered;
    int linked;
    void* select_group;
    size_t select_index;
} cc_io_waiter;

#if CC_IO_WAIT_HAS_KQUEUE
typedef struct cc_io_kqueue_slot {
    struct cc_io_kqueue_slot* next;
    int fd;
    short events;
    void* fiber;
    uint64_t wait_ticket;
    _Atomic int ready;
    _Atomic int active;
    _Atomic int armed;
    _Atomic int persistent;
    void* select_group;
    size_t select_index;
} cc_io_kqueue_slot;
#endif

struct cc__io_owned_watcher {
    int fd;
#if CC_IO_WAIT_HAS_KQUEUE
    cc_io_kqueue_slot* read_slot;
    cc_io_kqueue_slot* write_slot;
#endif
};

typedef struct {
    pthread_mutex_t mu;
    pthread_once_t once;
    pthread_t thread;
    int wake_pipe[2];
    int kqfd;
    int init_err;
    cc_io_waiter* head;
#if CC_IO_WAIT_HAS_KQUEUE
    cc_io_kqueue_slot* kq_slots;
#endif
} cc_io_wait_state;

static cc_io_wait_state g_cc_io_wait_state = {
    .mu = PTHREAD_MUTEX_INITIALIZER,
    .once = PTHREAD_ONCE_INIT,
    .wake_pipe = {-1, -1},
    .kqfd = -1,
    .init_err = 0,
    .head = NULL,
#if CC_IO_WAIT_HAS_KQUEUE
    .kq_slots = NULL,
#endif
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
    _Atomic uint64_t kq_slot_new;
    _Atomic uint64_t kq_slot_reuse;
    _Atomic uint64_t kq_slot_busy_fallback;
    _Atomic uint64_t kq_arm_calls;
    _Atomic uint64_t kq_arm_errors;
    _Atomic uint64_t kq_disarm_calls;
    _Atomic uint64_t kq_suspend_calls;
    _Atomic uint64_t kq_ready_before_suspend;
} cc_io_wait_stats;

static cc_io_wait_stats g_cc_io_wait_stats = {
    .enabled = -1,
    .init = 0,
    .atexit_registered = 0,
};

static int cc__io_wait_stats_enabled(void) {
    int mode = atomic_load_explicit(&g_cc_io_wait_stats.enabled, memory_order_acquire);
    if (mode >= 0) return mode;
    mode = (getenv("CC_IO_WAIT_STATS") || getenv("CC_NET_WATCH_STATS")) ? 1 : 0;
    int expected = -1;
    (void)atomic_compare_exchange_strong_explicit(&g_cc_io_wait_stats.enabled,
                                                  &expected,
                                                  mode,
                                                  memory_order_release,
                                                  memory_order_acquire);
    return atomic_load_explicit(&g_cc_io_wait_stats.enabled, memory_order_acquire);
}

static void cc__io_wait_stats_dump(void) {
    if (!cc__io_wait_stats_enabled()) return;
    fprintf(stderr,
            "\n[cc:io_wait] stats: waits=%llu adds=%llu removes=%llu notify=%llu "
            "poll_loops=%llu wake_pipe_hits=%llu ready_slots=%llu current=%llu max=%llu "
            "kq_slot_new=%llu kq_slot_reuse=%llu kq_slot_busy_fallback=%llu "
            "kq_arm_calls=%llu kq_arm_errors=%llu kq_disarm_calls=%llu "
            "kq_suspend_calls=%llu kq_ready_before_suspend=%llu\n",
            (unsigned long long)atomic_load_explicit(&g_cc_io_wait_stats.wait_async_calls, memory_order_relaxed),
            (unsigned long long)atomic_load_explicit(&g_cc_io_wait_stats.waiter_adds, memory_order_relaxed),
            (unsigned long long)atomic_load_explicit(&g_cc_io_wait_stats.waiter_removes, memory_order_relaxed),
            (unsigned long long)atomic_load_explicit(&g_cc_io_wait_stats.wake_notifications, memory_order_relaxed),
            (unsigned long long)atomic_load_explicit(&g_cc_io_wait_stats.poll_loops, memory_order_relaxed),
            (unsigned long long)atomic_load_explicit(&g_cc_io_wait_stats.poll_wake_pipe_hits, memory_order_relaxed),
            (unsigned long long)atomic_load_explicit(&g_cc_io_wait_stats.poll_ready_slots, memory_order_relaxed),
            (unsigned long long)atomic_load_explicit(&g_cc_io_wait_stats.current_waiters, memory_order_relaxed),
            (unsigned long long)atomic_load_explicit(&g_cc_io_wait_stats.max_waiters, memory_order_relaxed),
            (unsigned long long)atomic_load_explicit(&g_cc_io_wait_stats.kq_slot_new, memory_order_relaxed),
            (unsigned long long)atomic_load_explicit(&g_cc_io_wait_stats.kq_slot_reuse, memory_order_relaxed),
            (unsigned long long)atomic_load_explicit(&g_cc_io_wait_stats.kq_slot_busy_fallback, memory_order_relaxed),
            (unsigned long long)atomic_load_explicit(&g_cc_io_wait_stats.kq_arm_calls, memory_order_relaxed),
            (unsigned long long)atomic_load_explicit(&g_cc_io_wait_stats.kq_arm_errors, memory_order_relaxed),
            (unsigned long long)atomic_load_explicit(&g_cc_io_wait_stats.kq_disarm_calls, memory_order_relaxed),
            (unsigned long long)atomic_load_explicit(&g_cc_io_wait_stats.kq_suspend_calls, memory_order_relaxed),
            (unsigned long long)atomic_load_explicit(&g_cc_io_wait_stats.kq_ready_before_suspend, memory_order_relaxed));
}

static void cc__io_wait_stats_init(void) {
    if (!cc__io_wait_stats_enabled()) return;
    if (atomic_exchange_explicit(&g_cc_io_wait_stats.init, 1, memory_order_acq_rel)) return;
    if (!atomic_exchange_explicit(&g_cc_io_wait_stats.atexit_registered, 1, memory_order_acq_rel)) {
        atexit(cc__io_wait_stats_dump);
    }
}

static void cc__io_wait_stats_inc_current_waiters(void) {
    uint64_t cur = atomic_fetch_add_explicit(&g_cc_io_wait_stats.current_waiters, 1, memory_order_relaxed) + 1;
    uint64_t max = atomic_load_explicit(&g_cc_io_wait_stats.max_waiters, memory_order_relaxed);
    while (cur > max &&
           !atomic_compare_exchange_weak_explicit(&g_cc_io_wait_stats.max_waiters,
                                                  &max,
                                                  cur,
                                                  memory_order_relaxed,
                                                  memory_order_relaxed)) {
    }
}

static void cc__io_wait_stats_dec_current_waiters(void) {
    atomic_fetch_sub_explicit(&g_cc_io_wait_stats.current_waiters, 1, memory_order_relaxed);
}

static int cc__io_wait_force_direct(void) {
    static int mode = -1;
    if (mode >= 0) return mode;
    const char* env = getenv("CC_IO_WAIT_DIRECT");
    mode = (env && env[0] && !(env[0] == '0' && env[1] == '\0')) ? 1 : 0;
    return mode;
}

static int cc__io_wait_trace_enabled(void) {
    static int mode = -1;
    if (mode >= 0) return mode;
    const char* env = getenv("CC_IO_WAIT_TRACE");
    mode = (env && env[0] && !(env[0] == '0' && env[1] == '\0')) ? 1 : 0;
    return mode;
}

static int cc__io_wait_notify_on_remove(void) {
    static int mode = -1;
    if (mode >= 0) return mode;
    const char* env = getenv("CC_IO_WAIT_NOTIFY_ON_REMOVE");
    mode = (env && env[0] && !(env[0] == '0' && env[1] == '\0')) ? 1 : 0;
    return mode;
}

static int cc__io_wait_poll_timeout_ms(void) {
    static int mode = -2;
    if (mode != -2) return mode;
    const char* env = getenv("CC_IO_WAIT_POLL_TIMEOUT_MS");
    if (!env || !env[0]) {
        mode = -1;
        return mode;
    }
    mode = atoi(env);
    return mode;
}

static int cc__io_wait_use_kqueue(void) {
#if CC_IO_WAIT_HAS_KQUEUE
    static int mode = -1;
    if (mode >= 0) return mode;
    const char* env = getenv("CC_IO_WAIT_KQUEUE");
    mode = (!env || !env[0] || !(env[0] == '0' && env[1] == '\0')) ? 1 : 0;
    return mode;
#else
    return 0;
#endif
}

static void cc__io_wait_set_cloexec_best_effort(int fd) {
    int flags = fcntl(fd, F_GETFD, 0);
    if (flags < 0) return;
    (void)fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
}

static void cc__io_wait_trace(const char* action, const cc_io_waiter* waiter, short revents) {
    if (!cc__io_wait_trace_enabled() || !waiter) return;
    fprintf(stderr,
            "[cc:io_wait] %s fd=%d events=0x%x revents=0x%x fiber=%p ticket=%llu ready=%d cancelled=%d linked=%d\n",
            action,
            waiter->fd,
            (unsigned short)waiter->events,
            (unsigned short)revents,
            waiter->fiber,
            (unsigned long long)waiter->wait_ticket,
            atomic_load_explicit((const _Atomic int*)&waiter->ready, memory_order_relaxed),
            atomic_load_explicit((const _Atomic int*)&waiter->cancelled, memory_order_relaxed),
            waiter->linked);
}

static void cc__io_waiter_addref(cc_io_waiter* waiter) {
    if (!waiter) return;
    atomic_fetch_add_explicit(&waiter->refs, 1, memory_order_relaxed);
}

static void cc__io_waiter_release(cc_io_waiter* waiter) {
    if (!waiter) return;
    if (atomic_fetch_sub_explicit(&waiter->refs, 1, memory_order_acq_rel) == 1) {
        free(waiter);
    }
}

static void cc__io_wait_select_handle_clear(cc__io_wait_select_handle* handle) {
    if (!handle) return;
    handle->kind = 0;
    handle->ptr = NULL;
}

static void cc__io_waiter_notify(void) {
    if (g_cc_io_wait_state.wake_pipe[1] < 0) return;
    if (cc__io_wait_stats_enabled()) {
        cc__io_wait_stats_init();
        atomic_fetch_add_explicit(&g_cc_io_wait_stats.wake_notifications, 1, memory_order_relaxed);
    }
    unsigned char byte = 0;
    ssize_t rc;
    do {
        rc = write(g_cc_io_wait_state.wake_pipe[1], &byte, 1);
    } while (rc < 0 && errno == EINTR);
}

static void cc__io_waiter_drain_wake_pipe(void) {
    if (g_cc_io_wait_state.wake_pipe[0] < 0) return;
    char buf[128];
    while (1) {
        ssize_t n = read(g_cc_io_wait_state.wake_pipe[0], buf, sizeof(buf));
        if (n > 0) continue;
        if (n < 0 && errno == EINTR) continue;
        break;
    }
}

#if CC_IO_WAIT_HAS_KQUEUE
static short cc__io_wait_kevent_to_revents(const struct kevent* ev) {
    short revents = 0;
    if (!ev) return 0;
    if (ev->filter == EVFILT_READ) revents |= POLLIN;
    if (ev->filter == EVFILT_WRITE) revents |= POLLOUT;
    if (ev->flags & EV_EOF) revents |= POLLHUP;
    if (ev->flags & EV_ERROR) {
        revents |= ((intptr_t)ev->data == EBADF) ? POLLNVAL : POLLERR;
    }
    return revents;
}

static cc_io_kqueue_slot* cc__io_wait_kqueue_find_slot_locked(int fd, short events) {
    for (cc_io_kqueue_slot* slot = g_cc_io_wait_state.kq_slots; slot; slot = slot->next) {
        if (slot->fd == fd && slot->events == events) return slot;
    }
    return NULL;
}

static cc_io_kqueue_slot* cc__io_wait_kqueue_acquire_slot(int fd, short events, void* fiber, uint64_t wait_ticket) {
    pthread_mutex_lock(&g_cc_io_wait_state.mu);
    cc_io_kqueue_slot* slot = cc__io_wait_kqueue_find_slot_locked(fd, events);
    if (!slot) {
        slot = (cc_io_kqueue_slot*)calloc(1, sizeof(*slot));
        if (slot) {
            if (cc__io_wait_stats_enabled()) {
                cc__io_wait_stats_init();
                atomic_fetch_add_explicit(&g_cc_io_wait_stats.kq_slot_new, 1, memory_order_relaxed);
            }
            slot->fd = fd;
            slot->events = events;
            slot->next = g_cc_io_wait_state.kq_slots;
            g_cc_io_wait_state.kq_slots = slot;
        }
    } else if (cc__io_wait_stats_enabled()) {
        cc__io_wait_stats_init();
        atomic_fetch_add_explicit(&g_cc_io_wait_stats.kq_slot_reuse, 1, memory_order_relaxed);
    }
    if (slot && atomic_load_explicit(&slot->active, memory_order_acquire)) {
        if (cc__io_wait_stats_enabled()) {
            cc__io_wait_stats_init();
            atomic_fetch_add_explicit(&g_cc_io_wait_stats.kq_slot_busy_fallback, 1, memory_order_relaxed);
        }
        slot = NULL;
    }
    if (slot) {
        slot->fiber = fiber;
        slot->wait_ticket = wait_ticket;
        atomic_store_explicit(&slot->ready, 0, memory_order_relaxed);
        atomic_store_explicit(&slot->active, 1, memory_order_release);
    }
    pthread_mutex_unlock(&g_cc_io_wait_state.mu);
    return slot;
}

static cc_io_kqueue_slot* cc__io_wait_kqueue_bind_cached_slot(cc_io_kqueue_slot** cached_slot,
                                                              int fd,
                                                              short events,
                                                              void* fiber,
                                                              uint64_t wait_ticket,
                                                              int preserve_ready) {
    cc_io_kqueue_slot* slot = cached_slot ? *cached_slot : NULL;
    if (!slot) {
        slot = cc__io_wait_kqueue_acquire_slot(fd, events, fiber, wait_ticket);
        if (slot && cached_slot) {
            *cached_slot = slot;
        }
        return slot;
    }
    if (atomic_load_explicit(&slot->active, memory_order_acquire)) {
        if (cc__io_wait_stats_enabled()) {
            cc__io_wait_stats_init();
            atomic_fetch_add_explicit(&g_cc_io_wait_stats.kq_slot_busy_fallback, 1, memory_order_relaxed);
        }
        return NULL;
    }
    slot->fiber = fiber;
    slot->wait_ticket = wait_ticket;
    if (!preserve_ready) {
        atomic_store_explicit(&slot->ready, 0, memory_order_relaxed);
    }
    atomic_store_explicit(&slot->active, 1, memory_order_release);
    return slot;
}

static void cc__io_wait_kqueue_forget_fd(int fd) {
    pthread_mutex_lock(&g_cc_io_wait_state.mu);
    cc_io_kqueue_slot** cur = &g_cc_io_wait_state.kq_slots;
    while (*cur) {
        cc_io_kqueue_slot* slot = *cur;
        if (slot->fd == fd && !atomic_load_explicit(&slot->active, memory_order_acquire)) {
            *cur = slot->next;
            free(slot);
            continue;
        }
        cur = &slot->next;
    }
    pthread_mutex_unlock(&g_cc_io_wait_state.mu);
}

static int cc__io_wait_kqueue_apply(struct kevent* changes, int nchanges) {
    while (1) {
        int rc = kevent(g_cc_io_wait_state.kqfd, changes, nchanges, NULL, 0, NULL);
        if (rc >= 0) return 0;
        if (errno == EINTR) continue;
        return errno;
    }
}

static int cc__io_wait_kqueue_arm(cc_io_kqueue_slot* slot) {
    if (!slot) return EINVAL;
    if (cc__io_wait_stats_enabled()) {
        cc__io_wait_stats_init();
        atomic_fetch_add_explicit(&g_cc_io_wait_stats.kq_arm_calls, 1, memory_order_relaxed);
    }
    struct kevent evs[2];
    int nchanges = 0;
    if (slot->events & POLLIN) {
        EV_SET(&evs[nchanges++], slot->fd, EVFILT_READ, EV_ADD | EV_ONESHOT, 0, 0, slot);
    }
    if (slot->events & POLLOUT) {
        EV_SET(&evs[nchanges++], slot->fd, EVFILT_WRITE, EV_ADD | EV_ONESHOT, 0, 0, slot);
    }
    if (nchanges == 0) return EINVAL;
    atomic_store_explicit(&slot->armed, 1, memory_order_release);
    int err = cc__io_wait_kqueue_apply(evs, nchanges);
    if (err != 0) {
        if (cc__io_wait_stats_enabled()) {
            cc__io_wait_stats_init();
            atomic_fetch_add_explicit(&g_cc_io_wait_stats.kq_arm_errors, 1, memory_order_relaxed);
        }
        atomic_store_explicit(&slot->armed, 0, memory_order_release);
    }
    return err;
}

static int cc__io_wait_kqueue_arm_persistent_read(cc_io_kqueue_slot* slot) {
    if (!slot) return EINVAL;
    if (atomic_load_explicit(&slot->armed, memory_order_acquire)) {
        return 0;
    }
    if (cc__io_wait_stats_enabled()) {
        cc__io_wait_stats_init();
        atomic_fetch_add_explicit(&g_cc_io_wait_stats.kq_arm_calls, 1, memory_order_relaxed);
    }
    struct kevent ev;
    EV_SET(&ev, slot->fd, EVFILT_READ, EV_ADD | EV_CLEAR, 0, 0, slot);
    atomic_store_explicit(&slot->armed, 1, memory_order_release);
    atomic_store_explicit(&slot->persistent, 1, memory_order_release);
    int err = cc__io_wait_kqueue_apply(&ev, 1);
    if (err != 0) {
        if (cc__io_wait_stats_enabled()) {
            cc__io_wait_stats_init();
            atomic_fetch_add_explicit(&g_cc_io_wait_stats.kq_arm_errors, 1, memory_order_relaxed);
        }
        atomic_store_explicit(&slot->persistent, 0, memory_order_release);
        atomic_store_explicit(&slot->armed, 0, memory_order_release);
    }
    return err;
}

static void cc__io_wait_kqueue_disarm(cc_io_kqueue_slot* slot) {
    if (!slot) return;
    if (!atomic_exchange_explicit(&slot->armed, 0, memory_order_acq_rel)) return;
    if (cc__io_wait_stats_enabled()) {
        cc__io_wait_stats_init();
        atomic_fetch_add_explicit(&g_cc_io_wait_stats.kq_disarm_calls, 1, memory_order_relaxed);
    }
    struct kevent evs[2];
    int nchanges = 0;
    if (slot->events & POLLIN) {
        EV_SET(&evs[nchanges++], slot->fd, EVFILT_READ, EV_DELETE, 0, 0, NULL);
    }
    if (slot->events & POLLOUT) {
        EV_SET(&evs[nchanges++], slot->fd, EVFILT_WRITE, EV_DELETE, 0, 0, NULL);
    }
    int err = cc__io_wait_kqueue_apply(evs, nchanges);
    atomic_store_explicit(&slot->persistent, 0, memory_order_release);
    (void)err;
}

static void* cc__io_waiter_main_kqueue(void* arg) {
    (void)arg;
    struct kevent events[128];
    while (1) {
        struct timespec timeout;
        struct timespec* timeout_ptr = NULL;
        int timeout_ms = cc__io_wait_poll_timeout_ms();
        if (timeout_ms >= 0) {
            timeout.tv_sec = timeout_ms / 1000;
            timeout.tv_nsec = (timeout_ms % 1000) * 1000000L;
            timeout_ptr = &timeout;
        }
        int rc;
        while (1) {
            rc = kevent(g_cc_io_wait_state.kqfd, NULL, 0, events, (int)(sizeof(events) / sizeof(events[0])), timeout_ptr);
            if (rc >= 0) break;
            if (errno == EINTR) continue;
            rc = -1;
            break;
        }
        if (cc__io_wait_stats_enabled()) {
            cc__io_wait_stats_init();
            atomic_fetch_add_explicit(&g_cc_io_wait_stats.poll_loops, 1, memory_order_relaxed);
        }
        if (rc <= 0) continue;
        for (int i = 0; i < rc; ++i) {
            cc_io_kqueue_slot* slot = (cc_io_kqueue_slot*)events[i].udata;
            if (!slot) continue;
            short revents = cc__io_wait_kevent_to_revents(&events[i]);
            if (cc__io_wait_stats_enabled()) {
                atomic_fetch_add_explicit(&g_cc_io_wait_stats.poll_ready_slots, 1, memory_order_relaxed);
            }
            if (atomic_load_explicit(&slot->persistent, memory_order_acquire)) {
                if (revents & (slot->events | POLLERR | POLLHUP | POLLNVAL)) {
                    int was_ready = atomic_exchange_explicit(&slot->ready, 1, memory_order_acq_rel);
                    void* fiber = slot->fiber;
                    if (atomic_load_explicit(&slot->active, memory_order_acquire) &&
                        fiber &&
                        cc__fiber_wait_ticket_matches(fiber, slot->wait_ticket) &&
                        was_ready == 0 &&
                        cc__wait_select_try_win(slot->select_group, slot->select_index)) {
                        cc__fiber_unpark(fiber);
                    }
                }
                continue;
            }
            if (atomic_exchange_explicit(&slot->armed, 0, memory_order_acq_rel) == 0) {
                continue;
            }
            if (atomic_load_explicit(&slot->active, memory_order_acquire) &&
                (revents & (slot->events | POLLERR | POLLHUP | POLLNVAL)) &&
                (!slot->fiber || cc__fiber_wait_ticket_matches(slot->fiber, slot->wait_ticket)) &&
                atomic_exchange_explicit(&slot->ready, 1, memory_order_acq_rel) == 0 &&
                cc__wait_select_try_win(slot->select_group, slot->select_index)) {
                cc__fiber_unpark(slot->fiber);
            }
        }
    }
    return NULL;
}
#endif

static void* cc__io_waiter_main_poll(void* arg) {
    (void)arg;
    struct pollfd* pfds = NULL;
    cc_io_waiter** waiters = NULL;
    size_t capacity = 0;
    while (1) {
        size_t count = 0;
        size_t idx = 1;

        pthread_mutex_lock(&g_cc_io_wait_state.mu);
        for (cc_io_waiter* w = g_cc_io_wait_state.head; w; w = w->next) count++;
        size_t needed = count + 1;
        if (needed > capacity) {
            size_t new_capacity = capacity ? capacity : 16;
            while (new_capacity < needed) new_capacity *= 2;
            struct pollfd* new_pfds = (struct pollfd*)malloc(sizeof(*new_pfds) * new_capacity);
            cc_io_waiter** new_waiters =
                (cc_io_waiter**)malloc(sizeof(*new_waiters) * (new_capacity > 0 ? (new_capacity - 1) : 1));
            if (!new_pfds || !new_waiters) {
                pthread_mutex_unlock(&g_cc_io_wait_state.mu);
                free(new_pfds);
                free(new_waiters);
                usleep(1000);
                continue;
            }
            if (pfds) memcpy(new_pfds, pfds, sizeof(*new_pfds) * capacity);
            if (waiters && capacity > 1) memcpy(new_waiters, waiters, sizeof(*new_waiters) * (capacity - 1));
            free(pfds);
            free(waiters);
            pfds = new_pfds;
            waiters = new_waiters;
            capacity = new_capacity;
        }
        pfds[0].fd = g_cc_io_wait_state.wake_pipe[0];
        pfds[0].events = POLLIN;
        pfds[0].revents = 0;
        for (cc_io_waiter* w = g_cc_io_wait_state.head; w; w = w->next) {
            cc__io_waiter_addref(w);
            waiters[idx - 1] = w;
            pfds[idx].fd = w->fd;
            pfds[idx].events = w->events;
            pfds[idx].revents = 0;
            idx++;
        }
        pthread_mutex_unlock(&g_cc_io_wait_state.mu);

        while (1) {
            int rc = poll(pfds, idx, cc__io_wait_poll_timeout_ms());
            if (rc >= 0) break;
            if (errno == EINTR) continue;
            break;
        }
        if (cc__io_wait_stats_enabled()) {
            cc__io_wait_stats_init();
            atomic_fetch_add_explicit(&g_cc_io_wait_stats.poll_loops, 1, memory_order_relaxed);
        }

        if (pfds[0].revents & POLLIN) {
            if (cc__io_wait_stats_enabled()) {
                atomic_fetch_add_explicit(&g_cc_io_wait_stats.poll_wake_pipe_hits, 1, memory_order_relaxed);
            }
            cc__io_waiter_drain_wake_pipe();
        }

        for (size_t i = 1; i < idx; ++i) {
            cc_io_waiter* w = waiters[i - 1];
            short revents = pfds[i].revents;
            if (!w || revents == 0) continue;
            cc__io_wait_trace("poll_hit", w, revents);
            if (cc__io_wait_stats_enabled()) {
                atomic_fetch_add_explicit(&g_cc_io_wait_stats.poll_ready_slots, 1, memory_order_relaxed);
            }
            if (atomic_load_explicit(&w->cancelled, memory_order_acquire)) continue;
            if (!(revents & (w->events | POLLERR | POLLHUP | POLLNVAL))) continue;
            if (w->fiber && !cc__fiber_wait_ticket_matches(w->fiber, w->wait_ticket)) continue;
            if (atomic_exchange_explicit(&w->ready, 1, memory_order_acq_rel) == 0 &&
                cc__wait_select_try_win(w->select_group, w->select_index)) {
                cc__io_wait_trace("unpark", w, revents);
                cc__fiber_unpark(w->fiber);
            }
        }

        for (size_t i = 0; i + 1 < idx; ++i) {
            cc__io_waiter_release(waiters[i]);
        }
    }
    free(waiters);
    free(pfds);
    return NULL;
}

static void cc__io_wait_init_once(void) {
    if (cc__io_wait_use_kqueue()) {
#if CC_IO_WAIT_HAS_KQUEUE
        g_cc_io_wait_state.kqfd = kqueue();
        if (g_cc_io_wait_state.kqfd < 0) {
            g_cc_io_wait_state.init_err = errno ? errno : EIO;
            return;
        }
        cc__io_wait_set_cloexec_best_effort(g_cc_io_wait_state.kqfd);
        int err = pthread_create(&g_cc_io_wait_state.thread, NULL, cc__io_waiter_main_kqueue, NULL);
        if (err != 0) {
            g_cc_io_wait_state.init_err = err;
            close(g_cc_io_wait_state.kqfd);
            g_cc_io_wait_state.kqfd = -1;
            return;
        }
        pthread_detach(g_cc_io_wait_state.thread);
        return;
#endif
    }
    if (pipe(g_cc_io_wait_state.wake_pipe) != 0) {
        g_cc_io_wait_state.init_err = errno;
        g_cc_io_wait_state.wake_pipe[0] = -1;
        g_cc_io_wait_state.wake_pipe[1] = -1;
        return;
    }
    int flags0 = fcntl(g_cc_io_wait_state.wake_pipe[0], F_GETFL, 0);
    if (flags0 < 0 || fcntl(g_cc_io_wait_state.wake_pipe[0], F_SETFL, flags0 | O_NONBLOCK) < 0) {
        g_cc_io_wait_state.init_err = errno ? errno : EIO;
        close(g_cc_io_wait_state.wake_pipe[0]);
        close(g_cc_io_wait_state.wake_pipe[1]);
        g_cc_io_wait_state.wake_pipe[0] = -1;
        g_cc_io_wait_state.wake_pipe[1] = -1;
        return;
    }
    int flags1 = fcntl(g_cc_io_wait_state.wake_pipe[1], F_GETFL, 0);
    if (flags1 < 0 || fcntl(g_cc_io_wait_state.wake_pipe[1], F_SETFL, flags1 | O_NONBLOCK) < 0) {
        g_cc_io_wait_state.init_err = errno ? errno : EIO;
        close(g_cc_io_wait_state.wake_pipe[0]);
        close(g_cc_io_wait_state.wake_pipe[1]);
        g_cc_io_wait_state.wake_pipe[0] = -1;
        g_cc_io_wait_state.wake_pipe[1] = -1;
        return;
    }
    int err = pthread_create(&g_cc_io_wait_state.thread, NULL, cc__io_waiter_main_poll, NULL);
    if (err != 0) {
        g_cc_io_wait_state.init_err = err;
        close(g_cc_io_wait_state.wake_pipe[0]);
        close(g_cc_io_wait_state.wake_pipe[1]);
        g_cc_io_wait_state.wake_pipe[0] = -1;
        g_cc_io_wait_state.wake_pipe[1] = -1;
        return;
    }
    pthread_detach(g_cc_io_wait_state.thread);
}

static int cc__io_wait_ensure_running(void) {
    pthread_once(&g_cc_io_wait_state.once, cc__io_wait_init_once);
    return g_cc_io_wait_state.init_err;
}

cc__io_owned_watcher* cc__io_watcher_create(int fd) {
    if (fd < 0) return NULL;
    cc__io_owned_watcher* watcher = (cc__io_owned_watcher*)calloc(1, sizeof(*watcher));
    if (!watcher) return NULL;
    watcher->fd = fd;
    return watcher;
}

void cc__io_watcher_destroy(cc__io_owned_watcher* watcher) {
    if (!watcher) return;
#if CC_IO_WAIT_HAS_KQUEUE
    if (watcher->read_slot && atomic_load_explicit(&watcher->read_slot->armed, memory_order_acquire)) {
        cc__io_wait_kqueue_disarm(watcher->read_slot);
    }
    if (watcher->write_slot && atomic_load_explicit(&watcher->write_slot->armed, memory_order_acquire)) {
        cc__io_wait_kqueue_disarm(watcher->write_slot);
    }
#endif
    if (watcher->fd >= 0) {
        cc__io_wait_forget_fd(watcher->fd);
    }
    free(watcher);
}

void cc__io_wait_forget_fd(int fd) {
    if (fd < 0) return;
#if CC_IO_WAIT_HAS_KQUEUE
    if (cc__io_wait_use_kqueue() && g_cc_io_wait_state.kqfd >= 0) {
        cc__io_wait_kqueue_forget_fd(fd);
    }
#else
    (void)fd;
#endif
}

int cc__io_watcher_wait(cc__io_owned_watcher* watcher, short events) {
    if (!watcher || watcher->fd < 0) return EINVAL;
    if (!cc__fiber_in_context()) {
        return cc__io_wait_ready(watcher->fd, events);
    }
    if (cc__io_wait_force_direct()) {
        return cc__io_wait_ready(watcher->fd, events);
    }

    int init_err = cc__io_wait_ensure_running();
    if (init_err != 0) return cc__io_wait_ready(watcher->fd, events);
    if (cc__io_wait_stats_enabled()) {
        cc__io_wait_stats_init();
        atomic_fetch_add_explicit(&g_cc_io_wait_stats.wait_async_calls, 1, memory_order_relaxed);
    }

    if (cc__io_wait_use_kqueue()) {
#if CC_IO_WAIT_HAS_KQUEUE
        void* fiber = cc__fiber_current();
        uint64_t wait_ticket = cc__fiber_publish_wait_ticket(fiber);
        cc_io_kqueue_slot** cached_slot = NULL;
        int persistent_read = 0;
        if (events == POLLIN) cached_slot = &watcher->read_slot;
        else if (events == POLLOUT) cached_slot = &watcher->write_slot;
        if (events == POLLIN) persistent_read = 1;
        cc_io_kqueue_slot* slot = cc__io_wait_kqueue_bind_cached_slot(cached_slot, watcher->fd, events, fiber, wait_ticket, persistent_read);
        if (!slot) {
            return cc__io_wait_fd(watcher->fd, events);
        }
        if (cc__io_wait_stats_enabled()) {
            atomic_fetch_add_explicit(&g_cc_io_wait_stats.waiter_adds, 1, memory_order_relaxed);
            cc__io_wait_stats_inc_current_waiters();
        }
        int arm_err = persistent_read ? cc__io_wait_kqueue_arm_persistent_read(slot)
                                      : cc__io_wait_kqueue_arm(slot);
        if (arm_err != 0) {
            atomic_store_explicit(&slot->active, 0, memory_order_release);
            if (cc__io_wait_stats_enabled()) {
                atomic_fetch_add_explicit(&g_cc_io_wait_stats.waiter_removes, 1, memory_order_relaxed);
                cc__io_wait_stats_dec_current_waiters();
            }
            return arm_err;
        }
        if (persistent_read && atomic_exchange_explicit(&slot->ready, 0, memory_order_acq_rel) != 0) {
            atomic_store_explicit(&slot->active, 0, memory_order_release);
            slot->fiber = NULL;
            if (cc__io_wait_stats_enabled()) {
                atomic_fetch_add_explicit(&g_cc_io_wait_stats.waiter_removes, 1, memory_order_relaxed);
                cc__io_wait_stats_dec_current_waiters();
                atomic_fetch_add_explicit(&g_cc_io_wait_stats.kq_ready_before_suspend, 1, memory_order_relaxed);
            }
            return 0;
        }
        if (cc__io_wait_stats_enabled()) {
            cc__io_wait_stats_init();
            atomic_fetch_add_explicit(&g_cc_io_wait_stats.kq_suspend_calls, 1, memory_order_relaxed);
            if (atomic_load_explicit(&slot->ready, memory_order_acquire)) {
                atomic_fetch_add_explicit(&g_cc_io_wait_stats.kq_ready_before_suspend, 1, memory_order_relaxed);
            }
        }
        cc__fiber_set_park_obj(slot);
        CC_FIBER_SUSPEND_UNTIL_READY(&slot->ready, 0, "io_ready");
        cc__fiber_set_park_obj(NULL);
        if (persistent_read) {
            (void)atomic_exchange_explicit(&slot->ready, 0, memory_order_acq_rel);
        }
        atomic_store_explicit(&slot->active, 0, memory_order_release);
        slot->fiber = NULL;
        if (cc__io_wait_stats_enabled()) {
            atomic_fetch_add_explicit(&g_cc_io_wait_stats.waiter_removes, 1, memory_order_relaxed);
            cc__io_wait_stats_dec_current_waiters();
        }
        if (!persistent_read && atomic_load_explicit(&slot->armed, memory_order_acquire)) {
            cc__io_wait_kqueue_disarm(slot);
        }
        return 0;
#endif
    }

    return cc__io_wait_fd(watcher->fd, events);
}

int cc__io_wait_select_publish(cc__io_owned_watcher* watcher,
                               short events,
                               cc__wait_select_group* group,
                               size_t select_index,
                               cc__io_wait_select_handle* out_handle) {
    if (!watcher || watcher->fd < 0 || !out_handle) return EINVAL;
    cc__io_wait_select_handle_clear(out_handle);
    if (!cc__fiber_in_context()) return cc__io_wait_ready(watcher->fd, events);
    if (cc__io_wait_force_direct()) return cc__io_wait_ready(watcher->fd, events);

    int init_err = cc__io_wait_ensure_running();
    if (init_err != 0) return init_err;

    if (cc__io_wait_use_kqueue()) {
#if CC_IO_WAIT_HAS_KQUEUE
        void* fiber = cc__fiber_current();
        uint64_t wait_ticket = cc__fiber_publish_wait_ticket(fiber);
        cc_io_kqueue_slot** cached_slot = NULL;
        int persistent_read = 0;
        if (events == POLLIN) cached_slot = &watcher->read_slot;
        else if (events == POLLOUT) cached_slot = &watcher->write_slot;
        if (events == POLLIN) persistent_read = 1;
        cc_io_kqueue_slot* slot = cc__io_wait_kqueue_bind_cached_slot(cached_slot, watcher->fd, events, fiber, wait_ticket, persistent_read);
        if (!slot) return EAGAIN;
        slot->select_group = group;
        slot->select_index = select_index;
        int arm_err = persistent_read ? cc__io_wait_kqueue_arm_persistent_read(slot)
                                      : cc__io_wait_kqueue_arm(slot);
        if (arm_err != 0) {
            atomic_store_explicit(&slot->active, 0, memory_order_release);
            slot->fiber = NULL;
            slot->select_group = NULL;
            slot->select_index = 0;
            return arm_err;
        }
        if (atomic_load_explicit(&slot->ready, memory_order_acquire) &&
            cc__wait_select_try_win(group, select_index)) {
            atomic_fetch_add_explicit(&group->signaled, 1, memory_order_release);
        }
        out_handle->kind = 1;
        out_handle->ptr = slot;
        return 0;
#endif
    }

    cc_io_waiter* waiter = (cc_io_waiter*)calloc(1, sizeof(*waiter));
    if (!waiter) return EAGAIN;
    waiter->fd = watcher->fd;
    waiter->events = events;
    waiter->fiber = cc__fiber_current();
    waiter->wait_ticket = cc__fiber_publish_wait_ticket(waiter->fiber);
    waiter->select_group = group;
    waiter->select_index = select_index;
    atomic_store_explicit(&waiter->ready, 0, memory_order_relaxed);
    atomic_store_explicit(&waiter->cancelled, 0, memory_order_relaxed);
    atomic_store_explicit(&waiter->refs, 1, memory_order_relaxed);
    atomic_store_explicit(&waiter->backend_registered, 0, memory_order_relaxed);
    pthread_mutex_lock(&g_cc_io_wait_state.mu);
    waiter->next = g_cc_io_wait_state.head;
    if (waiter->next) waiter->next->prev = waiter;
    g_cc_io_wait_state.head = waiter;
    waiter->linked = 1;
    pthread_mutex_unlock(&g_cc_io_wait_state.mu);
    cc__io_waiter_notify();
    out_handle->kind = 2;
    out_handle->ptr = waiter;
    return 0;
}

void cc__io_wait_select_finish(cc__io_wait_select_handle* handle) {
    if (!handle || handle->kind == 0 || !handle->ptr) return;
#if CC_IO_WAIT_HAS_KQUEUE
    if (handle->kind == 1) {
        cc_io_kqueue_slot* slot = (cc_io_kqueue_slot*)handle->ptr;
        if (atomic_load_explicit(&slot->persistent, memory_order_acquire)) {
            (void)atomic_exchange_explicit(&slot->ready, 0, memory_order_acq_rel);
        }
        atomic_store_explicit(&slot->active, 0, memory_order_release);
        slot->fiber = NULL;
        slot->select_group = NULL;
        slot->select_index = 0;
        if (!atomic_load_explicit(&slot->persistent, memory_order_acquire) &&
            atomic_load_explicit(&slot->armed, memory_order_acquire)) {
            cc__io_wait_kqueue_disarm(slot);
        }
        cc__io_wait_select_handle_clear(handle);
        return;
    }
#endif
    if (handle->kind == 2) {
        cc_io_waiter* waiter = (cc_io_waiter*)handle->ptr;
        atomic_store_explicit(&waiter->cancelled, 1, memory_order_release);
        pthread_mutex_lock(&g_cc_io_wait_state.mu);
        if (waiter->linked) {
            if (waiter->prev) waiter->prev->next = waiter->next;
            else g_cc_io_wait_state.head = waiter->next;
            if (waiter->next) waiter->next->prev = waiter->prev;
            waiter->linked = 0;
        }
        pthread_mutex_unlock(&g_cc_io_wait_state.mu);
        cc__io_waiter_release(waiter);
        cc__io_wait_select_handle_clear(handle);
    }
}

int cc__io_wait_ready(int fd, short events) {
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

int cc__io_wait_fd(int fd, short events) {
    if (!cc__fiber_in_context()) {
        return cc__io_wait_ready(fd, events);
    }
    if (cc__io_wait_force_direct()) {
        return cc__io_wait_ready(fd, events);
    }

    int init_err = cc__io_wait_ensure_running();
    if (init_err != 0) return cc__io_wait_ready(fd, events);
    if (cc__io_wait_stats_enabled()) {
        cc__io_wait_stats_init();
        atomic_fetch_add_explicit(&g_cc_io_wait_stats.wait_async_calls, 1, memory_order_relaxed);
    }

    if (cc__io_wait_use_kqueue()) {
#if CC_IO_WAIT_HAS_KQUEUE
        void* fiber = cc__fiber_current();
        uint64_t wait_ticket = cc__fiber_publish_wait_ticket(fiber);
        cc_io_kqueue_slot* slot = cc__io_wait_kqueue_acquire_slot(fd, events, fiber, wait_ticket);
        if (!slot) {
            return cc__io_wait_ready(fd, events);
        }
        if (cc__io_wait_stats_enabled()) {
            atomic_fetch_add_explicit(&g_cc_io_wait_stats.waiter_adds, 1, memory_order_relaxed);
            cc__io_wait_stats_inc_current_waiters();
        }
        int arm_err = cc__io_wait_kqueue_arm(slot);
        if (arm_err != 0) {
            atomic_store_explicit(&slot->active, 0, memory_order_release);
            if (cc__io_wait_stats_enabled()) {
                atomic_fetch_add_explicit(&g_cc_io_wait_stats.waiter_removes, 1, memory_order_relaxed);
                cc__io_wait_stats_dec_current_waiters();
            }
            return arm_err;
        }
        if (cc__io_wait_stats_enabled()) {
            cc__io_wait_stats_init();
            atomic_fetch_add_explicit(&g_cc_io_wait_stats.kq_suspend_calls, 1, memory_order_relaxed);
            if (atomic_load_explicit(&slot->ready, memory_order_acquire)) {
                atomic_fetch_add_explicit(&g_cc_io_wait_stats.kq_ready_before_suspend, 1, memory_order_relaxed);
            }
        }
        cc__fiber_set_park_obj(slot);
        CC_FIBER_SUSPEND_UNTIL_READY(&slot->ready, 0, "io_ready");
        cc__fiber_set_park_obj(NULL);
        atomic_store_explicit(&slot->active, 0, memory_order_release);
        slot->fiber = NULL;
        if (cc__io_wait_stats_enabled()) {
            atomic_fetch_add_explicit(&g_cc_io_wait_stats.waiter_removes, 1, memory_order_relaxed);
            cc__io_wait_stats_dec_current_waiters();
        }
        if (atomic_load_explicit(&slot->armed, memory_order_acquire)) {
            cc__io_wait_kqueue_disarm(slot);
        }
        return 0;
#endif
    }

    cc_io_waiter* waiter = (cc_io_waiter*)calloc(1, sizeof(*waiter));
    if (!waiter) return cc__io_wait_ready(fd, events);

    waiter->fd = fd;
    waiter->events = events;
    waiter->fiber = cc__fiber_current();
    waiter->wait_ticket = cc__fiber_publish_wait_ticket(waiter->fiber);
    atomic_store_explicit(&waiter->ready, 0, memory_order_relaxed);
    atomic_store_explicit(&waiter->cancelled, 0, memory_order_relaxed);
    atomic_store_explicit(&waiter->refs, 1, memory_order_relaxed);
    atomic_store_explicit(&waiter->backend_registered, 0, memory_order_relaxed);

    pthread_mutex_lock(&g_cc_io_wait_state.mu);
    waiter->next = g_cc_io_wait_state.head;
    if (waiter->next) waiter->next->prev = waiter;
    g_cc_io_wait_state.head = waiter;
    waiter->linked = 1;
    pthread_mutex_unlock(&g_cc_io_wait_state.mu);
    if (cc__io_wait_stats_enabled()) {
        atomic_fetch_add_explicit(&g_cc_io_wait_stats.waiter_adds, 1, memory_order_relaxed);
        cc__io_wait_stats_inc_current_waiters();
    }
    cc__io_wait_trace("add", waiter, 0);
    cc__io_waiter_notify();

    cc__fiber_set_park_obj(waiter);
    CC_FIBER_SUSPEND_UNTIL_READY(&waiter->ready, 0, "io_ready");
    cc__fiber_set_park_obj(NULL);

    atomic_store_explicit(&waiter->cancelled, 1, memory_order_release);
    pthread_mutex_lock(&g_cc_io_wait_state.mu);
    if (waiter->linked) {
        if (waiter->prev) waiter->prev->next = waiter->next;
        else g_cc_io_wait_state.head = waiter->next;
        if (waiter->next) waiter->next->prev = waiter->prev;
        waiter->linked = 0;
    }
    pthread_mutex_unlock(&g_cc_io_wait_state.mu);
    if (cc__io_wait_stats_enabled()) {
        atomic_fetch_add_explicit(&g_cc_io_wait_stats.waiter_removes, 1, memory_order_relaxed);
        cc__io_wait_stats_dec_current_waiters();
    }
    cc__io_wait_trace("remove", waiter, 0);
    if (cc__io_wait_notify_on_remove()) {
        cc__io_waiter_notify();
    }
    cc__io_waiter_release(waiter);
    return 0;
}
