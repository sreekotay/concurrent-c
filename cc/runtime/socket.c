/*
 * Concurrent-C socket-bound readiness signal runtime.
 */

#include <ccc/std/net.cch>

#include <poll.h>
#include <pthread.h>
#include <string.h>

#include "fiber_internal.h"
#include "fiber_sched_boundary.h"
#include "io_wait.h"
#include "wait_select_internal.h"

static int cc__net_prepare_fiber_fd(int fd, uint8_t* flags);
static cc__io_owned_watcher* cc__net_ensure_socket_watcher(CCSocket* sock);

typedef struct cc__socket_signal_impl {
    CCSocket* socket;
    _Atomic uint64_t epoch;
    pthread_mutex_t wait_mu;
    void* waiter_fiber;
    uint64_t waiter_ticket;
    uint64_t waiter_seen_epoch;
    void* wait_group;
    size_t wait_index;
    /* wait_armed is written under wait_mu but READ lock-free by
     * cc_socket_signal_signal so a busy producer can skip wait_mu entirely
     * when no fiber is parked on this signal.  See the Dekker comment in
     * cc_socket_signal_signal and cc__socket_signal_arm for the pairing. */
    _Atomic int wait_armed;
    int initialized;
} cc__socket_signal_impl;

_Static_assert(sizeof(CCSocketSignal) >= sizeof(cc__socket_signal_impl),
               "CCSocketSignal storage is too small for runtime impl");
_Static_assert(_Alignof(CCSocketSignal) >= _Alignof(cc__socket_signal_impl),
               "CCSocketSignal alignment is too small for runtime impl");

static cc__socket_signal_impl* cc__socket_signal_impl_ptr(CCSocketSignal* sig) {
    return (cc__socket_signal_impl*)sig;
}

static const cc__socket_signal_impl* cc__socket_signal_impl_const(const CCSocketSignal* sig) {
    return (const cc__socket_signal_impl*)sig;
}

typedef struct cc__socket_signal_wait_ctx {
    CCSocketSignal* sig;
    cc__wait_select_group* group;
    uint64_t wait_ticket;
    uint64_t seen_epoch;
    size_t select_index;
} cc__socket_signal_wait_ctx;

typedef struct cc__ready_socket_wait_ctx {
    cc__io_owned_watcher* watcher;
    cc__wait_select_group* group;
    uint64_t wait_ticket;
    short events;
    size_t select_index;
    cc__io_wait_select_handle handle;
} cc__ready_socket_wait_ctx;

static uint64_t cc__socket_signal_current(CCSocketSignal* sig) {
    if (!sig) return 0;
    return atomic_load_explicit(&cc__socket_signal_impl_const(sig)->epoch, memory_order_acquire);
}

void cc_socket_create_signal(CCSocket* sock, CCSocketSignal* out_sig) {
    cc_socket_signal_init(out_sig, sock);
}

void cc_socket_signal_init(CCSocketSignal* sig, CCSocket* sock) {
    if (!sig) return;
    cc__socket_signal_impl* impl = cc__socket_signal_impl_ptr(sig);
    memset(impl, 0, sizeof(*impl));
    impl->socket = sock;
    atomic_store_explicit(&impl->epoch, 0, memory_order_relaxed);
    pthread_mutex_init(&impl->wait_mu, NULL);
    impl->initialized = 1;
}

void cc_socket_signal_free(CCSocketSignal* sig) {
    if (!sig) return;
    cc__socket_signal_impl* impl = cc__socket_signal_impl_ptr(sig);
    if (!impl->initialized) return;
    pthread_mutex_destroy(&impl->wait_mu);
    memset(impl, 0, sizeof(*impl));
}

static int cc__socket_signal_notify_waiter(CCSocketSignal* sig) {
    if (!sig) return 0;
    cc__socket_signal_impl* impl = cc__socket_signal_impl_ptr(sig);
    if (!impl->initialized) return 0;
    void* fiber = NULL;
    cc__wait_select_group* group = NULL;
    int should_unpark = 0;

    pthread_mutex_lock(&impl->wait_mu);
    if (atomic_load_explicit(&impl->wait_armed, memory_order_relaxed)) {
        uint64_t cur_epoch = atomic_load_explicit(&impl->epoch, memory_order_acquire);
        if (impl->waiter_fiber &&
            cur_epoch != impl->waiter_seen_epoch &&
            cc__fiber_wait_ticket_matches(impl->waiter_fiber, impl->waiter_ticket)) {
            group = (cc__wait_select_group*)impl->wait_group;
            if (cc__wait_select_try_win(group, impl->wait_index)) {
                atomic_fetch_add_explicit(&group->signaled, 1, memory_order_release);
                fiber = impl->waiter_fiber;
                atomic_store_explicit(&impl->wait_armed, 0, memory_order_release);
                impl->waiter_fiber = NULL;
                impl->wait_group = NULL;
                should_unpark = 1;
            }
        }
    }
    pthread_mutex_unlock(&impl->wait_mu);

    if (should_unpark) {
        cc__fiber_unpark(fiber);
        return 1;
    }
    return 0;
}

void cc_socket_signal_signal(CCSocketSignal* sig) {
    if (!sig) return;
    cc__socket_signal_impl* impl = cc__socket_signal_impl_ptr(sig);
    if (!impl->initialized) return;

    /* Bump epoch with seq_cst ordering so it Dekker-pairs with arm()'s
     * "set wait_armed=1 ; seq_cst fence ; reload epoch" sequence.  If
     * this load of wait_armed observes 0, then the arming fiber's store
     * of wait_armed=1 has not happened-before us, and by the seq_cst
     * total order the armer's subsequent epoch reload is guaranteed to
     * see our bump and self-wake in the recheck branch of arm().  This
     * lets a busy producer (e.g. a reply channel feeding a client fiber
     * that is already spinning on the next request) skip the wait_mu
     * lock/unlock pair on every send when the consumer is already
     * running — which is the steady state under load.
     *
     * Before this fast-path, every successful send on a signal-bearing
     * channel took wait_mu via cc__socket_signal_notify_waiter, which
     * showed up as ~175 leaf samples of pthread_mutex_{lock,unlock} on
     * high-throughput fan-in server profiles — pure overhead because
     * wait_armed was 0 on the vast majority of sends. */
    atomic_fetch_add_explicit(&impl->epoch, 1, memory_order_seq_cst);
    if (atomic_load_explicit(&impl->wait_armed, memory_order_acquire) == 0) {
        return;
    }
    (void)cc__socket_signal_notify_waiter(sig);
}

static int cc__socket_signal_arm(CCSocketSignal* sig,
                                 void* fiber,
                                 uint64_t wait_ticket,
                                 uint64_t seen_epoch,
                                 cc__wait_select_group* group,
                                 size_t select_index) {
    if (!sig || !group) return 0;
    cc__socket_signal_impl* impl = cc__socket_signal_impl_ptr(sig);
    if (!impl->initialized) return 0;

    pthread_mutex_lock(&impl->wait_mu);
    uint64_t cur_epoch = atomic_load_explicit(&impl->epoch, memory_order_acquire);
    if (cur_epoch != seen_epoch) {
        pthread_mutex_unlock(&impl->wait_mu);
        return 1;
    }

    impl->waiter_fiber = fiber;
    impl->waiter_ticket = wait_ticket;
    impl->waiter_seen_epoch = seen_epoch;
    impl->wait_group = group;
    impl->wait_index = select_index;
    /* Publish wait_armed=1 with release so a concurrent producer in
     * cc_socket_signal_signal sees it when loaded with acquire.  The
     * following seq_cst fence + epoch reload is the Dekker pair: if
     * the producer's seq_cst epoch bump happens-before our load here,
     * we observe the new epoch and self-wake.  If it happens-after,
     * the producer's acquire-load of wait_armed is guaranteed to
     * observe our store above (by seq_cst total order) and take the
     * mutex-based notify path. */
    atomic_store_explicit(&impl->wait_armed, 1, memory_order_release);
    atomic_thread_fence(memory_order_seq_cst);

    cur_epoch = atomic_load_explicit(&impl->epoch, memory_order_acquire);
    if (cur_epoch != seen_epoch) {
        if (cc__wait_select_try_win(group, select_index)) {
            atomic_fetch_add_explicit(&group->signaled, 1, memory_order_release);
        }
        atomic_store_explicit(&impl->wait_armed, 0, memory_order_release);
        impl->waiter_fiber = NULL;
        impl->wait_group = NULL;
        pthread_mutex_unlock(&impl->wait_mu);
        return 1;
    }

    pthread_mutex_unlock(&impl->wait_mu);
    return 0;
}

static void cc__socket_signal_disarm(CCSocketSignal* sig, uint64_t wait_ticket) {
    if (!sig) return;
    cc__socket_signal_impl* impl = cc__socket_signal_impl_ptr(sig);
    if (!impl->initialized) return;
    pthread_mutex_lock(&impl->wait_mu);
    if (atomic_load_explicit(&impl->wait_armed, memory_order_relaxed) &&
        impl->waiter_ticket == wait_ticket) {
        atomic_store_explicit(&impl->wait_armed, 0, memory_order_release);
        impl->waiter_fiber = NULL;
        impl->wait_group = NULL;
    }
    pthread_mutex_unlock(&impl->wait_mu);
}

static bool cc__socket_signal_try_complete(void* waitable, CCSchedFiber* fiber, void* io) {
    (void)waitable;
    (void)fiber;
    cc__socket_signal_wait_ctx* ctx = (cc__socket_signal_wait_ctx*)io;
    if (!ctx || !ctx->sig) return false;
    return cc__socket_signal_current(ctx->sig) != ctx->seen_epoch;
}

static bool cc__socket_signal_publish_wait(void* waitable, CCSchedFiber* fiber, void* io) {
    (void)waitable;
    cc__socket_signal_wait_ctx* ctx = (cc__socket_signal_wait_ctx*)io;
    if (!ctx || !ctx->sig || !ctx->group) return false;
    (void)cc__socket_signal_arm(ctx->sig,
                                fiber,
                                ctx->wait_ticket,
                                ctx->seen_epoch,
                                ctx->group,
                                ctx->select_index);
    return true;
}

static void cc__socket_signal_unpublish_wait(void* waitable, CCSchedFiber* fiber) {
    (void)fiber;
    cc__socket_signal_wait_ctx* ctx = (cc__socket_signal_wait_ctx*)waitable;
    if (!ctx || !ctx->sig) return;
    cc__socket_signal_disarm(ctx->sig, ctx->wait_ticket);
}

static const cc_sched_waitable_ops cc__socket_signal_wait_ops = {
    .try_complete = cc__socket_signal_try_complete,
    .publish = cc__socket_signal_publish_wait,
    .unpublish = cc__socket_signal_unpublish_wait,
};

static bool cc__ready_socket_publish_wait(void* waitable, CCSchedFiber* fiber, void* io) {
    (void)waitable;
    (void)fiber;
    cc__ready_socket_wait_ctx* ctx = (cc__ready_socket_wait_ctx*)io;
    if (!ctx || !ctx->watcher || !ctx->group) return false;
    int rc = cc__io_wait_select_publish(ctx->watcher,
                                        ctx->events,
                                        ctx->wait_ticket,
                                        ctx->group,
                                        ctx->select_index,
                                        &ctx->handle);
    return rc == 0;
}

static void cc__ready_socket_unpublish_wait(void* waitable, CCSchedFiber* fiber) {
    (void)waitable;
    (void)fiber;
    cc__ready_socket_wait_ctx* ctx = (cc__ready_socket_wait_ctx*)waitable;
    if (!ctx) return;
    cc__io_wait_select_finish(&ctx->handle);
}

static const cc_sched_waitable_ops cc__ready_socket_wait_ops = {
    .publish = cc__ready_socket_publish_wait,
    .unpublish = cc__ready_socket_unpublish_wait,
};

void cc__socket_wait_dump_diag(void) {
    /* Diagnostic hook used by sched_v2 sysmon. No-op until socket wait
     * diagnostics are reintroduced on the current wait path. */
}

static CCResult_bool_CCIoError cc__socket_signal_wait_with_epoch(CCSocketSignal* sig, uint64_t seen_epoch) {
    if (!sig) {
        return CCRes_err(bool, CCIoError, cc_io_from_errno(EINVAL));
    }
    cc__socket_signal_impl* impl = cc__socket_signal_impl_ptr(sig);
    if (!impl->initialized || !impl->socket || impl->socket->fd < 0) {
        return CCRes_err(bool, CCIoError, cc_io_from_errno(EINVAL));
    }
    CCSocket* sock = impl->socket;

    int prep_err = cc__net_prepare_fiber_fd(sock->fd, &sock->flags);
    if (prep_err != 0) {
        return CCRes_err(bool, CCIoError, cc_io_from_errno(prep_err));
    }

    if (cc__socket_signal_current(sig) != seen_epoch) {
        return CCRes_ok(bool, CCIoError, false);
    }

    cc__io_owned_watcher* watcher = cc__net_ensure_socket_watcher(sock);
    if (!watcher) {
        return CCRes_err(bool, CCIoError, cc_io_from_errno(ENOMEM));
    }

    cc__wait_select_group group = {
        .fiber = (cc__fiber*)cc__fiber_current(),
        .signaled = 0,
        .selected_index = -1,
    };
    uint64_t wait_ticket = cc__fiber_publish_wait_ticket(cc__fiber_current());
    cc__socket_signal_wait_ctx signal_ctx = {
        .sig = sig,
        .group = &group,
        .wait_ticket = wait_ticket,
        .seen_epoch = seen_epoch,
        .select_index = 0,
    };
    cc__ready_socket_wait_ctx socket_ctx = {
        .watcher = watcher,
        .group = &group,
        .wait_ticket = wait_ticket,
        .events = POLLIN,
        .select_index = 1,
        .handle = {0},
    };
    cc_sched_wait_case cases[2] = {
        {.waitable = &signal_ctx, .io = &signal_ctx, .ops = &cc__socket_signal_wait_ops},
        {.waitable = &socket_ctx, .io = &socket_ctx, .ops = &cc__ready_socket_wait_ops},
    };
    size_t selected_index = (size_t)-1;
    cc_sched_wait_result wait_rc =
        cc_sched_fiber_wait_many(cases,
                                 2,
                                 &group.signaled,
                                 &group.selected_index,
                                 &selected_index);
    if (wait_rc == CC_SCHED_WAIT_CLOSED) {
        return CCRes_err(bool, CCIoError, cc_io_error(CC_IO_OTHER));
    }

    if (selected_index == 0 || cc__socket_signal_current(sig) != seen_epoch) {
        return CCRes_ok(bool, CCIoError, false);
    }
    return CCRes_ok(bool, CCIoError, true);
}

uint64_t cc_socket_signal_snapshot(CCSocketSignal* sig) {
    return cc__socket_signal_current(sig);
}

CCResult_bool_CCIoError cc_socket_signal_wait(CCSocketSignal* sig) {
    uint64_t seen_epoch = cc__socket_signal_current(sig);
    return cc__socket_signal_wait_with_epoch(sig, seen_epoch);
}

CCResult_bool_CCIoError cc_socket_signal_wait_since(CCSocketSignal* sig, uint64_t seen_epoch) {
    return cc__socket_signal_wait_with_epoch(sig, seen_epoch);
}
