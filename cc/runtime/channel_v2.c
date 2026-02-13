/*
 * Channel V2: Performance-first rewrite.
 * See spec/concurrent-c-channel.md for the full specification.
 *
 * Design: Two-tier send/recv (fast path + slow path), 3-value notification
 * (WAITING/WOKEN/DATA), direct handoff for both buffered and unbuffered,
 * simplified select with CAS-based winner selection.
 *
 * Keeps: liblfds MPMC queue, fast_path_ok branding, lfqueue_count,
 * lfqueue_inflight, all 47 exported functions, same public API.
 */

#include <ccc/cc_channel.cch>

/* liblfds lock-free data structures */
#include "../../third_party/liblfds/liblfds7.1.1/liblfds711/inc/liblfds711.h"

/* Include liblfds source files for bounded MPMC queue */
#include "../../third_party/liblfds/liblfds7.1.1/liblfds711/src/lfds711_misc/lfds711_misc_globals.c"
#include "../../third_party/liblfds/liblfds7.1.1/liblfds711/src/lfds711_misc/lfds711_misc_internal_backoff_init.c"
#include "../../third_party/liblfds/liblfds7.1.1/liblfds711/src/lfds711_queue_bounded_manyproducer_manyconsumer/lfds711_queue_bounded_manyproducer_manyconsumer_init.c"
#include "../../third_party/liblfds/liblfds7.1.1/liblfds711/src/lfds711_queue_bounded_manyproducer_manyconsumer/lfds711_queue_bounded_manyproducer_manyconsumer_cleanup.c"
#include "../../third_party/liblfds/liblfds7.1.1/liblfds711/src/lfds711_queue_bounded_manyproducer_manyconsumer/lfds711_queue_bounded_manyproducer_manyconsumer_enqueue.c"
#include "../../third_party/liblfds/liblfds7.1.1/liblfds711/src/lfds711_queue_bounded_manyproducer_manyconsumer/lfds711_queue_bounded_manyproducer_manyconsumer_dequeue.c"
#include <ccc/cc_sched.cch>
#include <ccc/cc_nursery.cch>
#include <ccc/cc_exec.cch>
#include <ccc/cc_async_runtime.cch>
#include <ccc/cc_slice.cch>
#include <ccc/std/async_io.cch>
#include <ccc/std/future.cch>

#include <dlfcn.h>
#include <errno.h>
#include <inttypes.h>
#include <stddef.h>
#include <pthread.h>
#include <sched.h>
#include <unistd.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdio.h>
#include <stdatomic.h>
#include <stdint.h>

#include "fiber_internal.h"
#include "fiber_sched_boundary.h"

/* ============================================================================
 * Notification values (spec §Notification values)
 * ============================================================================ */
#define NOTIFY_WAITING  0   /* Not yet notified (initial state) */
#define NOTIFY_WOKEN    1   /* Woken — retry the operation */
#define NOTIFY_DATA     2   /* Direct handoff — data written to node->data */
#define NOTIFY_SIGNAL   3   /* Wake signal — retry buffer operation */

/* ============================================================================
 * TLS / externs
 * ============================================================================ */
extern __thread CCNursery* cc__tls_current_nursery;
__thread CCDeadline* cc__tls_current_deadline = NULL;

CCDeadline* cc_current_deadline(void) { return cc__tls_current_deadline; }

CCDeadline* cc_deadline_push(CCDeadline* d) {
    CCDeadline* prev = cc__tls_current_deadline;
    cc__tls_current_deadline = d;
    return prev;
}

void cc_deadline_pop(CCDeadline* prev) { cc__tls_current_deadline = prev; }

void cc_cancel_current(void) {
    if (cc__tls_current_deadline) cc__tls_current_deadline->cancelled = 1;
}

bool cc_is_cancelled_current(void) {
    return cc__tls_current_deadline && cc__tls_current_deadline->cancelled;
}

/* ============================================================================
 * Debug channel registry (for stall diagnosis)
 * ============================================================================ */
#define DBG_CHAN_REGISTRY_MAX 64
static CCChan* g_dbg_chan_registry[DBG_CHAN_REGISTRY_MAX];
static _Atomic int g_dbg_chan_count = 0;

static void chan_debug_maybe_start_dumper(void);  /* forward decl */
static void dbg_chan_register(CCChan* ch) {
    int idx = atomic_fetch_add_explicit(&g_dbg_chan_count, 1, memory_order_relaxed);
    if (idx < DBG_CHAN_REGISTRY_MAX) g_dbg_chan_registry[idx] = ch;
    chan_debug_maybe_start_dumper();
}

void cc__chan_debug_dump_global(void) {
    int n = atomic_load_explicit(&g_dbg_chan_count, memory_order_relaxed);
    if (n > DBG_CHAN_REGISTRY_MAX) n = DBG_CHAN_REGISTRY_MAX;
    fprintf(stderr, "Channel stats (%d channels, v2 registry):\n", n);
    for (int i = 0; i < n; i++) {
        CCChan* ch = g_dbg_chan_registry[i];
        if (!ch) continue;
        fprintf(stderr, "  ch[%d]=%p\n", i, (void*)ch);
    }
}

/* Periodic background dumper for channel stats (runs in a dedicated thread) */
static void* chan_debug_dumper(void* arg) {
    (void)arg;
    FILE* f = fopen("/tmp/chan_stats.log", "w");
    if (!f) return NULL;
    setbuf(f, NULL);  /* unbuffered */
    for (;;) {
        usleep(500000);  /* 0.5s */
        int n = atomic_load_explicit(&g_dbg_chan_count, memory_order_relaxed);
        if (n == 0) continue;
        if (n > DBG_CHAN_REGISTRY_MAX) n = DBG_CHAN_REGISTRY_MAX;
        fprintf(f, "Channel stats (%d channels, v2 registry):\n", n);
        for (int i = 0; i < n; i++) {
            CCChan* ch = g_dbg_chan_registry[i];
            if (!ch) continue;
            fprintf(f, "  ch[%d]=%p\n", i, (void*)ch);
        }
    }
    return NULL;
}

static void chan_debug_maybe_start_dumper(void) {
#ifdef CC_DEBUG_CHAN_STATS
    static _Atomic int started = 0;
    if (atomic_exchange(&started, 1)) return;
    pthread_t t;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_create(&t, &attr, chan_debug_dumper, NULL);
    pthread_attr_destroy(&attr);
#endif
}

/* ============================================================================
 * Yield interval for fast-path fairness
 * ============================================================================ */
#define CC_LF_YIELD_INTERVAL 16
static __thread unsigned int cc__tls_lf_ops = 0;
static inline void cc__chan_maybe_yield(void) {
    if (++cc__tls_lf_ops >= CC_LF_YIELD_INTERVAL) {
        cc__tls_lf_ops = 0;
        if (cc__fiber_in_context()) cc__fiber_yield_global();
    }
}

/* ============================================================================
 * Debug counters (gated behind CC_CHAN_DEBUG=1)
 * ============================================================================ */
#ifdef CC_CHAN_DEBUG
static _Atomic uint64_t g_dbg_send_fast = 0;
static _Atomic uint64_t g_dbg_send_slow = 0;
static _Atomic uint64_t g_dbg_recv_fast = 0;
static _Atomic uint64_t g_dbg_recv_slow = 0;
static _Atomic uint64_t g_dbg_handoff_send = 0;
static _Atomic uint64_t g_dbg_handoff_recv = 0;
static _Atomic uint64_t g_dbg_close_calls = 0;
#define DBG_INC(c) atomic_fetch_add_explicit(&(c), 1, memory_order_relaxed)
#else
#define DBG_INC(c) ((void)0)
#endif

static int cc__chan_dbg_enabled(void) {
    static int v = -1;
    if (v == -1) { const char* e = getenv("CC_CHAN_DEBUG"); v = (e && e[0] == '1') ? 1 : 0; }
    return v;
}

static _Atomic int g_chan_edge_wake_mode = -1; /* -1 unknown, 0 off, 1 on */
static inline int cc__chan_edge_wake_enabled(void) {
    int cached = atomic_load_explicit(&g_chan_edge_wake_mode, memory_order_relaxed);
    if (cached < 0) {
        const char* env = getenv("CC_CHAN_STEADY_EDGE_WAKE");
        int enabled = !(env && env[0] == '0');
        int expected = -1;
        (void)atomic_compare_exchange_strong_explicit(&g_chan_edge_wake_mode,
                                                      &expected,
                                                      enabled,
                                                      memory_order_relaxed,
                                                      memory_order_relaxed);
        cached = atomic_load_explicit(&g_chan_edge_wake_mode, memory_order_relaxed);
    }
    return cached;
}

/* ============================================================================
 * Timing (gated behind CC_CHANNEL_TIMING=1)
 * ============================================================================ */
static int channel_timing_enabled(void) {
    static int v = -1;
    if (v == -1) { const char* e = getenv("CC_CHANNEL_TIMING"); v = (e && e[0] == '1') ? 1 : 0; }
    return v;
}

/* ============================================================================
 * Wake batch (TLS, flush = N unpark calls)
 * ============================================================================ */
#define WAKE_BATCH_SIZE 16

typedef struct {
    cc__fiber* fibers[WAKE_BATCH_SIZE];
    size_t count;
} wake_batch_t;

static __thread wake_batch_t tls_wake_batch = {{NULL}, 0};
static __thread int tls_chan_lock_depth = 0;
static __thread int tls_wake_batch_deferred = 0;
static _Atomic uint64_t g_wake_batch_deferred = 0;
static _Atomic uint64_t g_wake_batch_defer_calls = 0;
static _Atomic uint64_t g_wake_batch_defer_nonempty = 0;
static _Atomic uint64_t g_wake_batch_flush_calls = 0;
static _Atomic uint64_t g_wake_batch_flush_nonempty = 0;
static _Atomic uint64_t g_wake_batch_flush_empty = 0;
static __thread CCChan* tls_chan_last_lock = NULL;
#define CHAN_LOCK_STACK_MAX 16
static __thread CCChan* tls_chan_lock_stack[CHAN_LOCK_STACK_MAX];
static __thread void* tls_chan_lock_ra0[CHAN_LOCK_STACK_MAX];
static __thread void* tls_chan_lock_ra1[CHAN_LOCK_STACK_MAX];

static int wake_batch_defer_enabled(void) {
    static int enabled = -1;
    if (enabled < 0) {
        const char* v = getenv("CC_CHAN_WAKE_DEFER");
        enabled = (v && v[0] == '0') ? 0 : 1;
    }
    return enabled;
}

static int wake_batch_guard_enabled(void) {
    static int enabled = -1;
    if (enabled < 0) {
        const char* v = getenv("CC_CHAN_WAKE_GUARD");
        enabled = (v && v[0] == '0') ? 0 : 1;
    }
    return enabled;
}

static inline void wake_batch_add(cc__fiber* f) {
    if (!f) return;
    wake_batch_t* b = &tls_wake_batch;
    if (b->count >= WAKE_BATCH_SIZE) {
        for (size_t i = 0; i < b->count; i++) {
            if (b->fibers[i]) { cc_sched_fiber_wake((CCSchedFiber*)b->fibers[i]); b->fibers[i] = NULL; }
        }
        b->count = 0;
    }
    b->fibers[b->count++] = f;
}

static inline void wake_batch_flush_now(void) {
    wake_batch_t* b = &tls_wake_batch;
    atomic_fetch_add_explicit(&g_wake_batch_flush_calls, 1, memory_order_relaxed);
    if (b->count > 0) {
        atomic_fetch_add_explicit(&g_wake_batch_flush_nonempty, 1, memory_order_relaxed);
    } else {
        atomic_fetch_add_explicit(&g_wake_batch_flush_empty, 1, memory_order_relaxed);
    }
    for (size_t i = 0; i < b->count; i++) {
        if (b->fibers[i]) { cc_sched_fiber_wake((CCSchedFiber*)b->fibers[i]); b->fibers[i] = NULL; }
    }
    b->count = 0;
}

static inline void wake_batch_flush(void) {
    /* Stabilization mode: flush immediately to avoid deferred-wake ordering bugs
     * while validating v2 lock/park semantics. */
    wake_batch_flush_now();
}

typedef struct cc__chan_wait_flag_ctx {
    _Atomic int* flag;
    int expected;
} cc__chan_wait_flag_ctx;

static bool cc__chan_wait_flag_try_complete(void* waitable, CCSchedFiber* fiber, void* io) {
    (void)fiber;
    (void)io;
    cc__chan_wait_flag_ctx* ctx = (cc__chan_wait_flag_ctx*)waitable;
    return atomic_load_explicit(ctx->flag, memory_order_acquire) != ctx->expected;
}

static bool cc__chan_wait_flag_publish(void* waitable, CCSchedFiber* fiber, void* io) {
    (void)waitable;
    (void)fiber;
    (void)io;
    return true;
}

static void cc__chan_wait_flag_unpublish(void* waitable, CCSchedFiber* fiber) {
    (void)waitable;
    (void)fiber;
}

static inline void cc__chan_wait_flag(_Atomic int* flag, int expected) {
    if (atomic_load_explicit(flag, memory_order_acquire) != expected) {
        return;
    }
    cc__chan_wait_flag_ctx ctx = {
        .flag = flag,
        .expected = expected,
    };
    const cc_sched_waitable_ops ops = {
        .try_complete = cc__chan_wait_flag_try_complete,
        .publish = cc__chan_wait_flag_publish,
        .unpublish = cc__chan_wait_flag_unpublish,
    };
    (void)cc_sched_fiber_wait(&ctx, NULL, &ops);
}

static inline void cc_chan_lock(CCChan* ch);
static inline void cc_chan_unlock(CCChan* ch);

int cc__chan_tls_lock_depth(void) { return tls_chan_lock_depth; }
void* cc__chan_tls_last_lock(void) { return (void*)tls_chan_last_lock; }
void cc__chan_flush_deferred(void) {
    if (tls_chan_lock_depth == 0 && tls_wake_batch_deferred) {
        tls_wake_batch_deferred = 0;
        wake_batch_flush_now();
    }
}
size_t cc__chan_lock_stack(void** chans, void** ra0s, void** ra1s, size_t max) {
    size_t depth = (tls_chan_lock_depth < CHAN_LOCK_STACK_MAX)
                       ? (size_t)tls_chan_lock_depth
                       : (size_t)CHAN_LOCK_STACK_MAX;
    if (depth > max) depth = max;
    for (size_t i = 0; i < depth; i++) {
        if (chans) chans[i] = tls_chan_lock_stack[i];
        if (ra0s) ra0s[i] = tls_chan_lock_ra0[i];
        if (ra1s) ra1s[i] = tls_chan_lock_ra1[i];
    }
    return depth;
}
void cc__chan_wake_batch_stats(uint64_t* defer_calls,
                               uint64_t* defer_nonempty,
                               uint64_t* flush_calls,
                               uint64_t* flush_nonempty,
                               uint64_t* flush_empty) {
    if (defer_calls) {
        *defer_calls = atomic_load_explicit(&g_wake_batch_defer_calls, memory_order_relaxed);
    }
    if (defer_nonempty) {
        *defer_nonempty = atomic_load_explicit(&g_wake_batch_defer_nonempty, memory_order_relaxed);
    }
    if (flush_calls) {
        *flush_calls = atomic_load_explicit(&g_wake_batch_flush_calls, memory_order_relaxed);
    }
    if (flush_nonempty) {
        *flush_nonempty = atomic_load_explicit(&g_wake_batch_flush_nonempty, memory_order_relaxed);
    }
    if (flush_empty) {
        *flush_empty = atomic_load_explicit(&g_wake_batch_flush_empty, memory_order_relaxed);
    }
}

static pthread_mutex_t g_chan_broadcast_mu;

static inline int cc__pthread_mutex_lock(pthread_mutex_t* mu) {
    return pthread_mutex_lock(mu);
}
static inline int cc__pthread_mutex_unlock(pthread_mutex_t* mu) {
    return pthread_mutex_unlock(mu);
}
static inline int cc__pthread_cond_wait(pthread_cond_t* cv, pthread_mutex_t* mu) {
    return pthread_cond_wait(cv, mu);
}
static inline int cc__pthread_cond_timedwait(pthread_cond_t* cv, pthread_mutex_t* mu, const struct timespec* ts) {
    return pthread_cond_timedwait(cv, mu, ts);
}

static inline void cc_chan_lock_mu(pthread_mutex_t* mu) {
    cc__pthread_mutex_lock(mu);
    if (mu != &g_chan_broadcast_mu) {
        tls_chan_lock_depth++;
    }
}

static inline void cc_chan_unlock_mu(pthread_mutex_t* mu) {
    cc__pthread_mutex_unlock(mu);
    if (mu != &g_chan_broadcast_mu) {
        if (tls_chan_lock_depth > 0) tls_chan_lock_depth--;
        if (tls_chan_lock_depth == 0 && tls_wake_batch_deferred) {
            tls_wake_batch_deferred = 0;
            wake_batch_flush_now();
        }
    }
}

static inline int cc_chan_cond_wait_mu(pthread_cond_t* cv, pthread_mutex_t* mu) {
    if (mu == &g_chan_broadcast_mu) {
        return cc__pthread_cond_wait(cv, mu);
    }
    if (tls_chan_lock_depth > 0) tls_chan_lock_depth--;
    int rc = cc__pthread_cond_wait(cv, mu);
    tls_chan_lock_depth++;
    return rc;
}
static inline int cc_chan_cond_timedwait_mu(pthread_cond_t* cv, pthread_mutex_t* mu, const struct timespec* ts) {
    if (mu == &g_chan_broadcast_mu) {
        return cc__pthread_cond_timedwait(cv, mu, ts);
    }
    if (tls_chan_lock_depth > 0) tls_chan_lock_depth--;
    int rc = cc__pthread_cond_timedwait(cv, mu, ts);
    tls_chan_lock_depth++;
    return rc;
}

#define pthread_mutex_lock(mu) cc_chan_lock_mu(mu)
#define pthread_mutex_unlock(mu) cc_chan_unlock_mu(mu)
#define pthread_cond_wait(cv, mu) cc_chan_cond_wait_mu(cv, mu)
#define pthread_cond_timedwait(cv, mu, ts) cc_chan_cond_timedwait_mu(cv, mu, ts)

/* ============================================================================
 * Global broadcast condvar for select/match (non-fiber fallback)
 * ============================================================================ */
static pthread_mutex_t g_chan_broadcast_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_chan_broadcast_cv = PTHREAD_COND_INITIALIZER;
static _Atomic int g_select_waiters = 0;

static void cc__chan_signal_activity(CCChan* ch) {
    (void)ch;
    if (atomic_load_explicit(&g_select_waiters, memory_order_relaxed) > 0) {
        pthread_mutex_lock(&g_chan_broadcast_mu);
        pthread_cond_broadcast(&g_chan_broadcast_cv);
        pthread_mutex_unlock(&g_chan_broadcast_mu);
    }
}

void cc_chan_wait_any_activity_timeout(int timeout_us) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_nsec += timeout_us * 1000;
    if (ts.tv_nsec >= 1000000000) { ts.tv_sec += ts.tv_nsec / 1000000000; ts.tv_nsec %= 1000000000; }
    atomic_fetch_add_explicit(&g_select_waiters, 1, memory_order_relaxed);
    pthread_mutex_lock(&g_chan_broadcast_mu);
    pthread_cond_timedwait(&g_chan_broadcast_cv, &g_chan_broadcast_mu, &ts);
    pthread_mutex_unlock(&g_chan_broadcast_mu);
    atomic_fetch_sub_explicit(&g_select_waiters, 1, memory_order_relaxed);
}

/* ============================================================================
 * Select wait group
 * ============================================================================ */
typedef struct cc__select_wait_group {
    cc__fiber* fiber;
    _Atomic int signaled;
    _Atomic int selected_index;
} cc__select_wait_group;

/* ============================================================================
 * CCChan struct (spec §Channel state)
 * ============================================================================ */
struct CCChan {
    /* Ring buffer (liblfds) */
    size_t cap;
    size_t elem_size;
    void* buf;
    int use_lockfree;
    size_t lfqueue_cap;
    struct lfds711_queue_bmm_state lfqueue_state;
    struct lfds711_queue_bmm_element* lfqueue_elements;
    _Atomic int lfqueue_count;
    _Atomic int lfqueue_inflight;

    /* Branded fast path */
    int fast_path_ok;

    /* Channel state */
    int closed;
    int tx_error_code;
    int rx_error_closed;
    int rx_error_code;
    CCChanMode mode;
    int allow_take;
    int is_sync;
    CCChanTopology topology;

    /* Synchronization */
    pthread_mutex_t mu;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;

    /* Fiber waiter lists (fiber-only; OS threads use condvars) */
    cc__fiber_wait_node* send_waiters_head;
    cc__fiber_wait_node* send_waiters_tail;
    cc__fiber_wait_node* recv_waiters_head;
    cc__fiber_wait_node* recv_waiters_tail;
    _Atomic int has_send_waiters;
    _Atomic int has_recv_waiters;

    /* Unbuffered (rendezvous) support: cap==0 */
    /* (no extra fields needed — handled entirely through waiter lists) */

    /* Mutex fallback ring buffer (cap==0, or non-lockfree buffered) */
    size_t count;
    size_t head;
    size_t tail;

    /* Ordered channel (task channel) support */
    int is_ordered;

    /* Owned channel (pool) support */
    int is_owned;
    CCClosure0 on_create;
    CCClosure1 on_destroy;
    CCClosure1 on_reset;
    size_t items_created;
    size_t max_items;

    /* Autoclose */
    CCNursery* autoclose_owner;
    int warned_autoclose_block;

    /* Fairness */
    _Atomic int recv_fairness_ctr;
    _Atomic size_t slot_counter;

    /* Generation counter: bumped by every fast-path mutation (enqueue,
     * dequeue) and every mu-path wake/close.  Slow-path parkers snapshot
     * gen under mu, then FIBER_PARK_IF(&gen, snapshot) — if any fast-path
     * op lands between unlock and park, worker_commit_park sees gen !=
     * snapshot and aborts the park.  Single field, single concept. */
    _Atomic int gen;

    /* Debug counters (always present, negligible cost) */
    _Atomic int dbg_sends;
    _Atomic int dbg_recvs;
    int dbg_id;           /* channel creation order for identification */
};

static inline void cc_chan_lock(CCChan* ch) {
    pthread_mutex_lock(&ch->mu);
    tls_chan_last_lock = ch;
    if (tls_chan_lock_depth < CHAN_LOCK_STACK_MAX) {
        tls_chan_lock_stack[tls_chan_lock_depth] = ch;
        tls_chan_lock_ra0[tls_chan_lock_depth] = __builtin_return_address(0);
        tls_chan_lock_ra1[tls_chan_lock_depth] = __builtin_return_address(1);
    }
    tls_chan_lock_depth++;
}

static inline void cc_chan_unlock(CCChan* ch) {
    pthread_mutex_unlock(&ch->mu);
    if (tls_chan_lock_depth > 0) {
        int idx = tls_chan_lock_depth - 1;
        if (idx < CHAN_LOCK_STACK_MAX) {
            tls_chan_lock_stack[idx] = NULL;
            tls_chan_lock_ra0[idx] = NULL;
            tls_chan_lock_ra1[idx] = NULL;
        }
        tls_chan_lock_depth--;
    }
    if (tls_chan_lock_depth == 0 && tls_wake_batch_deferred) {
        tls_wake_batch_deferred = 0;
        wake_batch_flush_now();
        tls_chan_last_lock = NULL;
    }
}

void cc__chan_set_autoclose_owner(CCChan* ch, CCNursery* owner) {
    if (!ch) return;
    cc_chan_lock(ch);
    ch->autoclose_owner = owner;
    ch->warned_autoclose_block = 0;
    cc_chan_unlock(ch);
}

void cc__chan_debug_dump_chan(void* ch_ptr) {
    if (!cc__chan_dbg_enabled() || !ch_ptr) return;
    CCChan* ch = (CCChan*)ch_ptr;
    int locked = pthread_mutex_trylock(&ch->mu) == 0;
    int count = atomic_load_explicit(&ch->lfqueue_count, memory_order_relaxed);
    int inflight = atomic_load_explicit(&ch->lfqueue_inflight, memory_order_relaxed);
    fprintf(stderr,
            "  [chan_v2 %p] cap=%zu elem=%zu closed=%d rx_err=%d lf=%d lfcap=%zu count=%d inflight=%d send_waiters=%p recv_waiters=%p%s\n",
            (void*)ch,
            ch->cap,
            ch->elem_size,
            ch->closed,
            ch->rx_error_closed,
            ch->use_lockfree,
            ch->lfqueue_cap,
            count,
            inflight,
            (void*)ch->send_waiters_head,
            (void*)ch->recv_waiters_head,
            locked ? "" : " (lock busy)");
    if (locked) pthread_mutex_unlock(&ch->mu);
}

/* ============================================================================
 * Forward declarations
 * ============================================================================ */
static CCChan* cc_chan_create_internal(size_t capacity, CCChanMode mode, bool allow_take, bool is_sync, CCChanTopology topology);
static int chan_send_slow(CCChan* ch, const void* value, size_t value_size, const struct timespec* deadline);
static int chan_recv_slow(CCChan* ch, void* out_value, size_t value_size, const struct timespec* deadline);

/* ============================================================================
 * Waiter list helpers (all under ch->mu)
 * ============================================================================ */
static inline void list_append(cc__fiber_wait_node** head, cc__fiber_wait_node** tail, cc__fiber_wait_node* node) {
    node->next = NULL;
    node->prev = *tail;
    if (*tail) (*tail)->next = node;
    else *head = node;
    *tail = node;
    node->in_wait_list = 1;
}

static inline void list_remove(cc__fiber_wait_node** head, cc__fiber_wait_node** tail, cc__fiber_wait_node* node) {
    if (!node->in_wait_list) return;
    if (node->prev) node->prev->next = node->next;
    else *head = node->next;
    if (node->next) node->next->prev = node->prev;
    else *tail = node->prev;
    node->next = node->prev = NULL;
    node->in_wait_list = 0;
}

static inline void add_send_waiter(CCChan* ch, cc__fiber_wait_node* node) {
    list_append(&ch->send_waiters_head, &ch->send_waiters_tail, node);
    atomic_store_explicit(&ch->has_send_waiters, 1, memory_order_seq_cst);
}

static inline void add_recv_waiter(CCChan* ch, cc__fiber_wait_node* node) {
    list_append(&ch->recv_waiters_head, &ch->recv_waiters_tail, node);
    atomic_store_explicit(&ch->has_recv_waiters, 1, memory_order_seq_cst);
}

static inline void remove_send_waiter(CCChan* ch, cc__fiber_wait_node* node) {
    list_remove(&ch->send_waiters_head, &ch->send_waiters_tail, node);
    if (!ch->send_waiters_head) {
        atomic_store_explicit(&ch->has_send_waiters, 0, memory_order_seq_cst);
    }
}

static inline void remove_recv_waiter(CCChan* ch, cc__fiber_wait_node* node) {
    list_remove(&ch->recv_waiters_head, &ch->recv_waiters_tail, node);
    if (!ch->recv_waiters_head) {
        atomic_store_explicit(&ch->has_recv_waiters, 0, memory_order_seq_cst);
    }
}

/* Select CAS: try to claim this node's select case as the winner.
 * Returns 1 if won (or not a select node), 0 if another case already won. */
static inline int select_try_win(cc__fiber_wait_node* node) {
    if (!node->is_select || !node->select_group) return 1;
    cc__select_wait_group* g = (cc__select_wait_group*)node->select_group;
    int sel = atomic_load_explicit(&g->selected_index, memory_order_acquire);
    if (sel == (int)node->select_index) return 1;  /* already won */
    if (sel != -1) return 0;  /* another case won */
    int expected = -1;
    return atomic_compare_exchange_strong_explicit(&g->selected_index, &expected,
                                                    (int)node->select_index,
                                                    memory_order_acq_rel,
                                                    memory_order_acquire);
}

/* Pop a recv waiter (must hold ch->mu). Skips already-notified and lost-select nodes. */
static cc__fiber_wait_node* pop_recv_waiter(CCChan* ch) {
    while (ch->recv_waiters_head) {
        cc__fiber_wait_node* node = ch->recv_waiters_head;
        /* Skip already-notified nodes */
        if (atomic_load_explicit(&node->notified, memory_order_acquire) != NOTIFY_WAITING) {
            remove_recv_waiter(ch, node);
            continue;
        }
        /* For select nodes, try to win the CAS */
        if (!select_try_win(node)) {
            remove_recv_waiter(ch, node);
            continue;
        }
        remove_recv_waiter(ch, node);
        return node;
    }
    return NULL;
}

/* Pop a send waiter (must hold ch->mu). Skips already-notified and lost-select nodes. */
static cc__fiber_wait_node* pop_send_waiter(CCChan* ch) {
    while (ch->send_waiters_head) {
        cc__fiber_wait_node* node = ch->send_waiters_head;
        if (atomic_load_explicit(&node->notified, memory_order_acquire) != NOTIFY_WAITING) {
            remove_send_waiter(ch, node);
            continue;
        }
        if (!select_try_win(node)) {
            remove_send_waiter(ch, node);
            continue;
        }
        remove_send_waiter(ch, node);
        return node;
    }
    return NULL;
}

/* unpark_or_signal: wake a fiber or signal a condvar (Invariant 5) */
static inline void unpark_or_signal(cc__fiber_wait_node* node, pthread_cond_t* cv) {
    if (node->fiber) {
        wake_batch_add(node->fiber);
    } else {
        pthread_cond_signal(cv);
    }
}

/* Signal one recv waiter (must hold ch->mu), v1-style (keep node in list). */
static void wake_one_recv_waiter(CCChan* ch) {
    if (!ch || !ch->recv_waiters_head) return;
    for (cc__fiber_wait_node* node = ch->recv_waiters_head; node; node = node->next) {
        if (atomic_load_explicit(&node->notified, memory_order_acquire) != NOTIFY_WAITING) continue;
        if (!select_try_win(node)) continue;
        atomic_store_explicit(&node->notified, NOTIFY_SIGNAL, memory_order_release);
        atomic_fetch_add_explicit(&ch->gen, 1, memory_order_release);
        if (node->is_select && node->select_group) {
            cc__select_wait_group* g = (cc__select_wait_group*)node->select_group;
            atomic_fetch_add_explicit(&g->signaled, 1, memory_order_release);
        }
        unpark_or_signal(node, &ch->not_empty);
        return;
    }
}

/* Signal one send waiter (must hold ch->mu), v1-style (keep node in list). */
static void wake_one_send_waiter(CCChan* ch) {
    if (!ch || !ch->send_waiters_head) return;
    for (cc__fiber_wait_node* node = ch->send_waiters_head; node; node = node->next) {
        if (atomic_load_explicit(&node->notified, memory_order_acquire) != NOTIFY_WAITING) continue;
        if (!select_try_win(node)) continue;
        atomic_store_explicit(&node->notified, NOTIFY_SIGNAL, memory_order_release);
        atomic_fetch_add_explicit(&ch->gen, 1, memory_order_release);
        if (node->is_select && node->select_group) {
            cc__select_wait_group* g = (cc__select_wait_group*)node->select_group;
            atomic_fetch_add_explicit(&g->signaled, 1, memory_order_release);
        }
        unpark_or_signal(node, &ch->not_full);
        return;
    }
}

/* ============================================================================
 * Lock-free enqueue/dequeue helpers
 * ============================================================================ */
static inline int lf_enqueue(CCChan* ch, const void* value, int* old_count_out) {
    void* queue_val = NULL;
    memcpy(&queue_val, value, ch->elem_size);
    if (!lfds711_queue_bmm_enqueue(&ch->lfqueue_state, NULL, queue_val)) return EAGAIN;
    int old_count = atomic_fetch_add_explicit(&ch->lfqueue_count, 1, memory_order_release);
    if (old_count_out) *old_count_out = old_count;
    return 0;
}

static inline int lf_dequeue(CCChan* ch, void* out_value, int* old_count_out) {
    void *key, *val;
    if (!lfds711_queue_bmm_dequeue(&ch->lfqueue_state, &key, &val)) return EAGAIN;
    int old_count = atomic_fetch_sub_explicit(&ch->lfqueue_count, 1, memory_order_release);
    if (old_count_out) *old_count_out = old_count;
    memcpy(out_value, &val, ch->elem_size);
    return 0;
}

/* Minimal fast-path variants.
 * Keep lfqueue_count updates so edge-triggered wake can avoid unnecessary lock/wake work. */
static inline int lf_enqueue_minimal(CCChan* ch, const void* value, int* old_count_out) {
    void* queue_val = NULL;
    memcpy(&queue_val, value, ch->elem_size);
    if (!lfds711_queue_bmm_enqueue(&ch->lfqueue_state, NULL, queue_val)) return EAGAIN;
    int old_count = atomic_fetch_add_explicit(&ch->lfqueue_count, 1, memory_order_release);
    if (old_count_out) *old_count_out = old_count;
    return 0;
}

static inline int lf_dequeue_minimal(CCChan* ch, void* out_value, int* old_count_out) {
    void *key, *val;
    if (!lfds711_queue_bmm_dequeue(&ch->lfqueue_state, &key, &val)) return EAGAIN;
    int old_count = atomic_fetch_sub_explicit(&ch->lfqueue_count, 1, memory_order_release);
    if (old_count_out) *old_count_out = old_count;
    memcpy(out_value, &val, ch->elem_size);
    return 0;
}

static inline int should_wake_recv(CCChan* ch, int old_count) {
    if (!atomic_load_explicit(&ch->has_recv_waiters, memory_order_seq_cst)) return 0;
    if (cc__chan_edge_wake_enabled() && old_count != 0) return 0;
    return 1;
}

static inline int should_wake_send(CCChan* ch, int old_count) {
    if (!atomic_load_explicit(&ch->has_send_waiters, memory_order_seq_cst)) return 0;
    if (cc__chan_edge_wake_enabled() && old_count != (int)ch->cap) return 0;
    return 1;
}

/* ============================================================================
 * Utility
 * ============================================================================ */
static inline size_t next_power_of_2(size_t n) {
    if (n == 0) return 1;
    n--;
    n |= n >> 1; n |= n >> 2; n |= n >> 4; n |= n >> 8; n |= n >> 16;
#if SIZE_MAX > 0xFFFFFFFF
    n |= n >> 32;
#endif
    return n + 1;
}

static inline void channel_store_slot(void* dst, const void* src, size_t sz) {
    memcpy(dst, src, sz);
}

static inline void channel_load_slot(const void* src, void* dst, size_t sz) {
    memcpy(dst, src, sz);
}

/* ============================================================================
 * Channel create / init / free (spec §Channel state)
 * ============================================================================ */
static int cc_chan_ensure_buf(CCChan* ch, size_t elem_size) {
    if (ch->elem_size == 0) {
        ch->elem_size = elem_size;
        if (ch->use_lockfree && ch->cap > 0) {
            ch->buf = malloc(ch->lfqueue_cap * elem_size);
        } else {
            size_t slots = (ch->cap == 0) ? 1 : ch->cap;
            ch->buf = malloc(slots * elem_size);
        }
        if (!ch->buf) return ENOMEM;
        ch->fast_path_ok = (ch->use_lockfree && ch->cap > 0 && ch->buf &&
                            elem_size <= sizeof(void*) &&
                            !ch->is_owned && !ch->is_ordered && !ch->is_sync);
        return 0;
    }
    if (ch->elem_size != elem_size) return EINVAL;
    return 0;
}

int cc_chan_init_elem(CCChan* ch, size_t elem_size) {
    if (!ch || elem_size == 0) return EINVAL;
    return cc_chan_ensure_buf(ch, elem_size);
}

static CCChan* cc_chan_create_internal(size_t capacity, CCChanMode mode, bool allow_take, bool is_sync, CCChanTopology topology) {
    CCChan* ch = (CCChan*)calloc(1, sizeof(CCChan));
    if (!ch) return NULL;
    ch->cap = capacity;
    ch->mode = mode;
    ch->allow_take = allow_take ? 1 : 0;
    ch->is_sync = is_sync ? 1 : 0;
    ch->topology = topology;
    pthread_mutex_init(&ch->mu, NULL);
    pthread_cond_init(&ch->not_empty, NULL);
    pthread_cond_init(&ch->not_full, NULL);

    if (capacity > 1) {
        const char* disable_lf = getenv("CC_CHAN_NO_LOCKFREE");
        if (disable_lf && disable_lf[0] == '1') { dbg_chan_register(ch); return ch; }
        size_t lfcap = next_power_of_2(capacity);
        ch->lfqueue_cap = lfcap;
        size_t alloc_size = sizeof(struct lfds711_queue_bmm_element) * lfcap;
        size_t align = LFDS711_PAL_ATOMIC_ISOLATION_IN_BYTES;
        alloc_size = ((alloc_size + align - 1) / align) * align;
        ch->lfqueue_elements = (struct lfds711_queue_bmm_element*)aligned_alloc(align, alloc_size);
        if (ch->lfqueue_elements) {
            lfds711_queue_bmm_init_valid_on_current_logical_core(&ch->lfqueue_state, ch->lfqueue_elements, lfcap, NULL);
            ch->use_lockfree = 1;
        }
    }
    dbg_chan_register(ch);
    return ch;
}

CCChan* cc_chan_create(size_t capacity) {
    return cc_chan_create_internal(capacity, CC_CHAN_MODE_BLOCK, true, false, CC_CHAN_TOPO_DEFAULT);
}

CCChan* cc_chan_create_mode(size_t capacity, CCChanMode mode) {
    return cc_chan_create_internal(capacity, mode, true, false, CC_CHAN_TOPO_DEFAULT);
}

CCChan* cc_chan_create_mode_take(size_t capacity, CCChanMode mode, bool allow_send_take) {
    return cc_chan_create_internal(capacity, mode, allow_send_take, false, CC_CHAN_TOPO_DEFAULT);
}

CCChan* cc_chan_create_sync(size_t capacity, CCChanMode mode, bool allow_send_take) {
    return cc_chan_create_internal(capacity, mode, allow_send_take, true, CC_CHAN_TOPO_DEFAULT);
}

int cc_chan_pair_create(size_t capacity, CCChanMode mode, bool allow_send_take, size_t elem_size, CCChanTx* out_tx, CCChanRx* out_rx) {
    return cc_chan_pair_create_ex(capacity, mode, allow_send_take, elem_size, false, out_tx, out_rx);
}

int cc_chan_pair_create_ex(size_t capacity, CCChanMode mode, bool allow_send_take, size_t elem_size, bool is_sync, CCChanTx* out_tx, CCChanRx* out_rx) {
    return cc_chan_pair_create_full(capacity, mode, allow_send_take, elem_size, is_sync, CC_CHAN_TOPO_DEFAULT, out_tx, out_rx);
}

int cc_chan_pair_create_full(size_t capacity, CCChanMode mode, bool allow_send_take, size_t elem_size, bool is_sync, int topology, CCChanTx* out_tx, CCChanRx* out_rx) {
    if (!out_tx || !out_rx) return EINVAL;
    out_tx->raw = NULL; out_rx->raw = NULL;
    CCChan* ch = cc_chan_create_internal(capacity, mode, allow_send_take, is_sync, (CCChanTopology)topology);
    if (!ch) return ENOMEM;
    if (elem_size != 0) {
        int e = cc_chan_init_elem(ch, elem_size);
        if (e != 0) { cc_chan_free(ch); return e; }
    }
    out_tx->raw = ch; out_rx->raw = ch;
    return 0;
}

CCChan* cc_chan_pair_create_returning(size_t capacity, CCChanMode mode, bool allow_send_take, size_t elem_size, bool is_sync, int topology, bool is_ordered, CCChanTx* out_tx, CCChanRx* out_rx) {
    if (!out_tx || !out_rx) return NULL;
    out_tx->raw = NULL; out_rx->raw = NULL;
    CCChan* ch = cc_chan_create_internal(capacity, mode, allow_send_take, is_sync, (CCChanTopology)topology);
    if (!ch) return NULL;
    ch->is_ordered = is_ordered ? 1 : 0;
    if (elem_size != 0) {
        int e = cc_chan_init_elem(ch, elem_size);
        if (e != 0) { cc_chan_free(ch); return NULL; }
    }
    out_tx->raw = ch; out_rx->raw = ch;
    return ch;
}

int cc_chan_is_ordered(CCChan* ch) { return ch ? ch->is_ordered : 0; }

CCChan* cc_chan_create_owned(size_t capacity, size_t elem_size, CCClosure0 on_create, CCClosure1 on_destroy, CCClosure1 on_reset) {
    if (capacity == 0) return NULL;
    CCChan* ch = cc_chan_create_internal(capacity, CC_CHAN_MODE_BLOCK, false, true, CC_CHAN_TOPO_DEFAULT);
    if (!ch) return NULL;
    int err = cc_chan_init_elem(ch, elem_size);
    if (err != 0) { cc_chan_free(ch); return NULL; }
    ch->is_owned = 1;
    ch->on_create = on_create;
    ch->on_destroy = on_destroy;
    ch->on_reset = on_reset;
    ch->items_created = 0;
    ch->max_items = capacity;
    return ch;
}

CCChan* cc_chan_create_owned_pool(size_t capacity, size_t elem_size, CCClosure0 on_create, CCClosure1 on_destroy, CCClosure1 on_reset) {
    return cc_chan_create_owned(capacity, elem_size, on_create, on_destroy, on_reset);
}

/* ============================================================================
 * Close (spec §Close)
 * ============================================================================ */
void cc_chan_close(CCChan* ch) {
    if (!ch) return;
    DBG_INC(g_dbg_close_calls);
    ch->fast_path_ok = 0;
    cc_chan_lock(ch);
    ch->closed = 1;
    /* Wake all waiters with NOTIFY_WOKEN (spec: close sets closed, wakes with WOKEN) */
    while (ch->send_waiters_head) {
        cc__fiber_wait_node* node = ch->send_waiters_head;
        remove_send_waiter(ch, node);
        /* For select: try to win; if lost, skip */
        if (node->is_select && !select_try_win(node)) continue;
        atomic_store_explicit(&node->notified, NOTIFY_WOKEN, memory_order_release);
        if (node->is_select && node->select_group) {
            cc__select_wait_group* g = (cc__select_wait_group*)node->select_group;
            atomic_fetch_add_explicit(&g->signaled, 1, memory_order_release);
        }
        wake_batch_add(node->fiber);
    }
    while (ch->recv_waiters_head) {
        cc__fiber_wait_node* node = ch->recv_waiters_head;
        remove_recv_waiter(ch, node);
        if (node->is_select && !select_try_win(node)) continue;
        atomic_store_explicit(&node->notified, NOTIFY_WOKEN, memory_order_release);
        if (node->is_select && node->select_group) {
            cc__select_wait_group* g = (cc__select_wait_group*)node->select_group;
            atomic_fetch_add_explicit(&g->signaled, 1, memory_order_release);
        }
        wake_batch_add(node->fiber);
    }
    atomic_fetch_add_explicit(&ch->gen, 1, memory_order_release);
    pthread_cond_broadcast(&ch->not_full);
    pthread_cond_broadcast(&ch->not_empty);
    cc_chan_unlock(ch);
    wake_batch_flush();
    cc__chan_signal_activity(ch);
}

void cc_chan_close_err(CCChan* ch, int err) {
    if (!ch) return;
    ch->fast_path_ok = 0;
    cc_chan_lock(ch);
    ch->closed = 1;
    ch->tx_error_code = err;
    /* Same wake-all as cc_chan_close */
    while (ch->send_waiters_head) {
        cc__fiber_wait_node* node = ch->send_waiters_head;
        remove_send_waiter(ch, node);
        if (node->is_select && !select_try_win(node)) continue;
        atomic_store_explicit(&node->notified, NOTIFY_WOKEN, memory_order_release);
        if (node->is_select && node->select_group) {
            cc__select_wait_group* g = (cc__select_wait_group*)node->select_group;
            atomic_fetch_add_explicit(&g->signaled, 1, memory_order_release);
        }
        wake_batch_add(node->fiber);
    }
    while (ch->recv_waiters_head) {
        cc__fiber_wait_node* node = ch->recv_waiters_head;
        remove_recv_waiter(ch, node);
        if (node->is_select && !select_try_win(node)) continue;
        atomic_store_explicit(&node->notified, NOTIFY_WOKEN, memory_order_release);
        if (node->is_select && node->select_group) {
            cc__select_wait_group* g = (cc__select_wait_group*)node->select_group;
            atomic_fetch_add_explicit(&g->signaled, 1, memory_order_release);
        }
        wake_batch_add(node->fiber);
    }
    atomic_fetch_add_explicit(&ch->gen, 1, memory_order_release);
    pthread_cond_broadcast(&ch->not_full);
    pthread_cond_broadcast(&ch->not_empty);
    cc_chan_unlock(ch);
    wake_batch_flush();
    cc__chan_signal_activity(ch);
}

void cc_chan_rx_close_err(CCChan* ch, int err) {
    if (!ch) return;
    ch->fast_path_ok = 0;
    cc_chan_lock(ch);
    ch->rx_error_closed = 1;
    ch->rx_error_code = err;
    /* Wake send waiters */
    while (ch->send_waiters_head) {
        cc__fiber_wait_node* node = ch->send_waiters_head;
        remove_send_waiter(ch, node);
        if (node->is_select && !select_try_win(node)) continue;
        atomic_store_explicit(&node->notified, NOTIFY_WOKEN, memory_order_release);
        if (node->is_select && node->select_group) {
            cc__select_wait_group* g = (cc__select_wait_group*)node->select_group;
            atomic_fetch_add_explicit(&g->signaled, 1, memory_order_release);
        }
        wake_batch_add(node->fiber);
    }
    atomic_fetch_add_explicit(&ch->gen, 1, memory_order_release);
    pthread_cond_broadcast(&ch->not_full);
    cc_chan_unlock(ch);
    wake_batch_flush();
    cc__chan_signal_activity(ch);
}

/* ============================================================================
 * Free
 * ============================================================================ */
void cc_chan_free(CCChan* ch) {
    if (!ch) return;

    /* For owned channels, destroy remaining items */
    if (ch->is_owned && ch->on_destroy.fn && ch->buf && ch->elem_size > 0) {
        cc_chan_lock(ch);
        if (ch->use_lockfree && ch->elem_size <= sizeof(void*)) {
            void* queue_val = NULL;
            while (lfds711_queue_bmm_dequeue(&ch->lfqueue_state, NULL, &queue_val) == 1) {
                intptr_t item_val = 0;
                memcpy(&item_val, &queue_val, ch->elem_size);
                ch->on_destroy.fn(ch->on_destroy.env, item_val);
            }
        } else if (ch->use_lockfree) {
            void* key = NULL;
            while (lfds711_queue_bmm_dequeue(&ch->lfqueue_state, NULL, &key) == 1) {
                size_t slot_idx = (size_t)(uintptr_t)key;
                char* item_ptr = (char*)ch->buf + (slot_idx * ch->elem_size);
                intptr_t item_val = 0;
                memcpy(&item_val, item_ptr, ch->elem_size < sizeof(intptr_t) ? ch->elem_size : sizeof(intptr_t));
                ch->on_destroy.fn(ch->on_destroy.env, item_val);
            }
        } else {
            size_t cnt = ch->count;
            size_t hd = ch->head;
            size_t slots = (ch->cap == 0) ? 1 : ch->cap;
            for (size_t i = 0; i < cnt; i++) {
                size_t idx = (hd + i) % slots;
                char* item_ptr = (char*)ch->buf + (idx * ch->elem_size);
                intptr_t item_val = 0;
                memcpy(&item_val, item_ptr, ch->elem_size < sizeof(intptr_t) ? ch->elem_size : sizeof(intptr_t));
                ch->on_destroy.fn(ch->on_destroy.env, item_val);
            }
        }
        cc_chan_unlock(ch);
        if (ch->on_create.drop) ch->on_create.drop(ch->on_create.env);
        if (ch->on_destroy.drop) ch->on_destroy.drop(ch->on_destroy.env);
        if (ch->on_reset.drop) ch->on_reset.drop(ch->on_reset.env);
    }

    if (ch->use_lockfree && ch->lfqueue_elements) {
        lfds711_queue_bmm_cleanup(&ch->lfqueue_state, NULL);
        free(ch->lfqueue_elements);
    }
    pthread_mutex_destroy(&ch->mu);
    pthread_cond_destroy(&ch->not_empty);
    pthread_cond_destroy(&ch->not_full);
    free(ch->buf);
    free(ch);
}

/* ============================================================================
 * Mode helpers (DROP_NEW / DROP_OLD)
 * ============================================================================ */
static int handle_drop_mode_send(CCChan* ch, const void* value) {
    if (ch->mode == CC_CHAN_MODE_DROP_NEW) return EAGAIN;
    if (ch->mode == CC_CHAN_MODE_DROP_OLD) {
        /* Drop oldest, enqueue new */
        if (ch->use_lockfree && ch->elem_size <= sizeof(void*)) {
            void* dummy;
            lf_dequeue(ch, &dummy, NULL);
            /* Re-try enqueue */
            atomic_fetch_add_explicit(&ch->lfqueue_inflight, 1, memory_order_relaxed);
            int rc = lf_enqueue(ch, value, NULL);
            atomic_fetch_sub_explicit(&ch->lfqueue_inflight, 1, memory_order_relaxed);
            if (rc == 0) return 0;
        }
        /* Mutex fallback drop */
        if (ch->count > 0) {
            ch->head = (ch->head + 1) % ch->cap;
            ch->count--;
        }
        memcpy((char*)ch->buf + (ch->tail * ch->elem_size), value, ch->elem_size);
        ch->tail = (ch->tail + 1) % ch->cap;
        ch->count++;
        return 0;
    }
    return -1; /* not a drop mode */
}

/* ============================================================================
 * SEND (spec §Send path)
 * ============================================================================ */
static int cc_chan_send_impl(CCChan* ch, const void* value, size_t value_size);
int cc_chan_send(CCChan* ch, const void* value, size_t value_size) {
    int rc = cc_chan_send_impl(ch, value, value_size);
    if (rc == 0) atomic_fetch_add_explicit(&ch->dbg_sends, 1, memory_order_relaxed);
    return rc;
}
static int cc_chan_send_impl(CCChan* ch, const void* value, size_t value_size) {
    /* === FAST PATH (branded, no mutex) === */
    if (ch->fast_path_ok && value_size == ch->elem_size) {
        int old_count = 0;
        if (lf_enqueue_minimal(ch, value, &old_count) == 0) {
            /* Bump gen so any slow-path parker sees the mutation. */
            atomic_fetch_add_explicit(&ch->gen, 1, memory_order_release);
            if (__builtin_expect(should_wake_recv(ch, old_count), 0)) {
                cc_chan_lock(ch);
                wake_one_recv_waiter(ch);
                cc_chan_unlock(ch);
                wake_batch_flush();
            }
            cc__chan_maybe_yield();
            DBG_INC(g_dbg_send_fast);
            return 0;
        }
        /* Buffer full — fall through to slow path */
    }
    if (!ch || !value || value_size == 0) return EINVAL;

    /* Owned channel: call on_reset */
    if (ch->is_owned && ch->on_reset.fn) {
        intptr_t v = 0;
        memcpy(&v, value, value_size < sizeof(intptr_t) ? value_size : sizeof(intptr_t));
        ch->on_reset.fn(ch->on_reset.env, v);
    }

    /* Deadline scope */
    CCDeadline* dl = cc__tls_current_deadline;
    if (dl && dl->cancelled) return ECANCELED;
    struct timespec ts;
    const struct timespec* p = dl ? cc_deadline_as_timespec(dl, &ts) : NULL;

    return chan_send_slow(ch, value, value_size, p);
}

/* Slow path: one clean blocking loop (spec §Send path, slow path) */
static int chan_send_slow(CCChan* ch, const void* value, size_t value_size, const struct timespec* deadline) {
    /* Ensure buffer allocated */
    cc_chan_lock(ch);
    int buferr = cc_chan_ensure_buf(ch, value_size);
    if (buferr != 0) { cc_chan_unlock(ch); return buferr; }
    cc_chan_unlock(ch);

    int in_fiber = cc__fiber_in_context();
    cc__fiber* fiber = in_fiber ? cc__fiber_current() : NULL;

    DBG_INC(g_dbg_send_slow);

    while (1) {
        /* --- Attempt phase (no mutex) --- */
        if (ch->closed) return EPIPE;
        if (ch->rx_error_closed) return ch->rx_error_code;

        /* Try lock-free enqueue (buffered, small elements) */
        if (ch->use_lockfree && ch->cap > 0 && ch->elem_size <= sizeof(void*)) {
            atomic_fetch_add_explicit(&ch->lfqueue_inflight, 1, memory_order_relaxed);
            if (ch->closed) {
                atomic_fetch_sub_explicit(&ch->lfqueue_inflight, 1, memory_order_relaxed);
                return EPIPE;
            }
            int rc = lf_enqueue(ch, value, NULL);
            atomic_fetch_sub_explicit(&ch->lfqueue_inflight, 1, memory_order_relaxed);
            if (rc == 0) {
                cc_chan_lock(ch);
                wake_one_recv_waiter(ch);
                pthread_cond_signal(&ch->not_empty);
                cc_chan_unlock(ch);
                wake_batch_flush();
                cc__chan_signal_activity(ch);
                return 0;
            }
        }

        /* Handle non-blocking modes before taking the lock */
        if (ch->mode != CC_CHAN_MODE_BLOCK && ch->cap > 0) {
            int dm = handle_drop_mode_send(ch, value);
            if (dm >= 0) {
                cc_chan_lock(ch);
                wake_one_recv_waiter(ch);
                pthread_cond_signal(&ch->not_empty);
                cc_chan_unlock(ch);
                wake_batch_flush();
                cc__chan_signal_activity(ch);
                return dm;
            }
        }

        /* --- Blocking phase (under mu) --- */
        cc_chan_lock(ch);

        /* Invariant 3: re-check closed under mu */
        if (ch->closed) { cc_chan_unlock(ch); return EPIPE; }
        if (ch->rx_error_closed) { cc_chan_unlock(ch); return ch->rx_error_code; }

        /* Direct handoff to parked receiver (Invariant 1) */
        cc__fiber_wait_node* rnode = pop_recv_waiter(ch);
        if (rnode) {
            channel_store_slot(rnode->data, value, ch->elem_size);
            atomic_store_explicit(&rnode->notified, NOTIFY_DATA, memory_order_release);
            if (rnode->is_select && rnode->select_group) {
                cc__select_wait_group* g = (cc__select_wait_group*)rnode->select_group;
                atomic_fetch_add_explicit(&g->signaled, 1, memory_order_release);
            }
            unpark_or_signal(rnode, &ch->not_empty);
            cc_chan_unlock(ch);
            wake_batch_flush();
            cc__chan_signal_activity(ch);
            DBG_INC(g_dbg_handoff_send);
            return 0;
        }

        /* Retry enqueue under mu (closes race window — spec Invariant 2) */
        if (ch->use_lockfree && ch->cap > 0 && ch->elem_size <= sizeof(void*)) {
            atomic_fetch_add_explicit(&ch->lfqueue_inflight, 1, memory_order_relaxed);
            int rc = lf_enqueue(ch, value, NULL);
            atomic_fetch_sub_explicit(&ch->lfqueue_inflight, 1, memory_order_relaxed);
            if (rc == 0) {
                wake_one_recv_waiter(ch);
                pthread_cond_signal(&ch->not_empty);
                cc_chan_unlock(ch);
                wake_batch_flush();
                cc__chan_signal_activity(ch);
                return 0;
            }
        } else if (ch->cap > 0 && !ch->use_lockfree) {
            /* Mutex-only buffered path */
            if (ch->count < ch->cap) {
                memcpy((char*)ch->buf + (ch->tail * ch->elem_size), value, ch->elem_size);
                ch->tail = (ch->tail + 1) % ch->cap;
                ch->count++;
                wake_one_recv_waiter(ch);
                pthread_cond_signal(&ch->not_empty);
                cc_chan_unlock(ch);
                wake_batch_flush();
                cc__chan_signal_activity(ch);
                return 0;
            }
        }
        /* cap==0 (unbuffered): handoff already tried above, so fall through to park */

        /* Check deadline before parking */
        if (deadline) {
            struct timespec now;
            clock_gettime(CLOCK_REALTIME, &now);
            if (now.tv_sec > deadline->tv_sec || (now.tv_sec == deadline->tv_sec && now.tv_nsec >= deadline->tv_nsec)) {
                cc_chan_unlock(ch);
                return ETIMEDOUT;
            }
        }

        /* Append waiter, unlock, park (Invariant 2)
         * Clear stale pending_unpark before making ourselves visible on the
         * waiter list — this prevents a wakeup from a previous channel op
         * from being consumed by FIBER_PARK_IF, which would cause us to
         * skip parking even though notified is still WAITING. */
        if (fiber) {
            cc__fiber_clear_pending_unpark();
            cc__fiber_wait_node node = {0};
            node.fiber = fiber;
            node.data = (void*)value;
            atomic_store_explicit(&node.notified, NOTIFY_WAITING, memory_order_release);
            add_send_waiter(ch, &node);
            cc_chan_unlock(ch);

            cc__fiber_set_park_obj(ch);
            cc__chan_wait_flag(&node.notified, NOTIFY_WAITING);

            /* Post-wake (v1-style): inspect notification under lock first. */
            cc_chan_lock(ch);
            int notified = atomic_load_explicit(&node.notified, memory_order_acquire);
            if (notified == NOTIFY_DATA) {
                cc_chan_unlock(ch);
                return 0;
            }
            if (notified == NOTIFY_SIGNAL) {
                atomic_store_explicit(&node.notified, NOTIFY_WAITING, memory_order_release);
                if (node.in_wait_list) remove_send_waiter(ch, &node);
                cc_chan_unlock(ch);
                continue;
            }
            if (node.in_wait_list) remove_send_waiter(ch, &node);
            cc_chan_unlock(ch);
            /* WOKEN/spurious: retry from top */
            continue;
        } else {
            /* OS thread: use condvar (Invariant 5 — no node on fiber list) */
            if (deadline) {
                pthread_cond_timedwait(&ch->not_full, &ch->mu, deadline);
            } else {
                pthread_cond_wait(&ch->not_full, &ch->mu);
            }
            cc_chan_unlock(ch);
            /* Retry from top */
            continue;
        }
    }
}

/* ============================================================================
 * RECV (spec §Recv path — mirror of send)
 * ============================================================================ */
static int cc_chan_recv_impl(CCChan* ch, void* out_value, size_t value_size);
int cc_chan_recv(CCChan* ch, void* out_value, size_t value_size) {
    int rc = cc_chan_recv_impl(ch, out_value, value_size);
    if (rc == 0) atomic_fetch_add_explicit(&ch->dbg_recvs, 1, memory_order_relaxed);
    return rc;
}
static int cc_chan_recv_impl(CCChan* ch, void* out_value, size_t value_size) {
    /* === FAST PATH (branded, no mutex) === */
    if (ch->fast_path_ok && value_size == ch->elem_size) {
        int old_count = 0;
        if (lf_dequeue_minimal(ch, out_value, &old_count) == 0) {
            /* Bump gen so any slow-path parker sees the mutation. */
            atomic_fetch_add_explicit(&ch->gen, 1, memory_order_release);
            if (__builtin_expect(should_wake_send(ch, old_count), 0)) {
                cc_chan_lock(ch);
                wake_one_send_waiter(ch);
                cc_chan_unlock(ch);
                wake_batch_flush();
            }
            cc__chan_maybe_yield();
            DBG_INC(g_dbg_recv_fast);
            return 0;
        }
        /* Buffer empty — check if closed before slow path */
        if (ch->closed) {
            /* Drain: try once more (in-flight enqueues may have completed) */
            if (lf_dequeue_minimal(ch, out_value, NULL) == 0) return 0;
            if (atomic_load_explicit(&ch->lfqueue_inflight, memory_order_acquire) > 0) {
                /* In-flight enqueue — fall through to slow path which will spin */
            } else {
                return ch->tx_error_code ? ch->tx_error_code : EPIPE;
            }
        }
    }
    if (!ch || !out_value || value_size == 0) return EINVAL;

    /* Owned channel: try on_create if pool is empty */
    if (ch->is_owned && ch->on_create.fn) {
        /* Attempt a non-blocking dequeue first */
        if (ch->use_lockfree && ch->elem_size == value_size && ch->elem_size <= sizeof(void*)) {
            if (lf_dequeue(ch, out_value, NULL) == 0) {
                cc_chan_lock(ch);
                wake_one_send_waiter(ch);
                cc_chan_unlock(ch);
                wake_batch_flush();
                return 0;
            }
        }
        /* Pool empty: create new item if under capacity */
        cc_chan_lock(ch);
        if (ch->items_created < ch->max_items) {
            void* new_item = ch->on_create.fn(ch->on_create.env);
            ch->items_created++;
            cc_chan_unlock(ch);
            memcpy(out_value, &new_item, value_size < sizeof(void*) ? value_size : sizeof(void*));
            return 0;
        }
        cc_chan_unlock(ch);
        /* Fall through to blocking recv */
    }

    /* Deadline scope */
    CCDeadline* dl = cc__tls_current_deadline;
    if (dl && dl->cancelled) return ECANCELED;
    struct timespec ts;
    const struct timespec* p = dl ? cc_deadline_as_timespec(dl, &ts) : NULL;

    return chan_recv_slow(ch, out_value, value_size, p);
}

/* Slow path: one clean blocking loop */
static int chan_recv_slow(CCChan* ch, void* out_value, size_t value_size, const struct timespec* deadline) {
    cc_chan_lock(ch);
    int buferr = cc_chan_ensure_buf(ch, value_size);
    if (buferr != 0) { cc_chan_unlock(ch); return buferr; }
    cc_chan_unlock(ch);

    int in_fiber = cc__fiber_in_context();
    cc__fiber* fiber = in_fiber ? cc__fiber_current() : NULL;

    DBG_INC(g_dbg_recv_slow);

    while (1) {
        /* --- Attempt phase (no mutex) --- */
        /* Try lock-free dequeue (buffered, small elements) */
        if (ch->use_lockfree && ch->cap > 0 && ch->elem_size <= sizeof(void*)) {
            int rc = lf_dequeue(ch, out_value, NULL);
            if (rc == 0) {
                cc_chan_lock(ch);
                wake_one_send_waiter(ch);
                pthread_cond_signal(&ch->not_full);
                cc_chan_unlock(ch);
                wake_batch_flush();
                cc__chan_signal_activity(ch);
                return 0;
            }
            /* Empty — check closed + inflight */
            if (ch->closed) {
                /* Drain in-flight (Invariant 6) */
                while (atomic_load_explicit(&ch->lfqueue_inflight, memory_order_acquire) > 0) {
                    rc = lf_dequeue(ch, out_value, NULL);
                    if (rc == 0) {
                        cc_chan_lock(ch);
                        wake_one_send_waiter(ch);
                        pthread_cond_signal(&ch->not_full);
                        cc_chan_unlock(ch);
                        wake_batch_flush();
                        return 0;
                    }
                    sched_yield();
                }
                /* Final try after inflight drained */
                rc = lf_dequeue(ch, out_value, NULL);
                if (rc == 0) return 0;
                return ch->tx_error_code ? ch->tx_error_code : EPIPE;
            }
        }

        /* --- Blocking phase (under mu) --- */
        cc_chan_lock(ch);

        /* Invariant 3: re-check closed */
        if (ch->closed) {
            /* For buffered: try dequeue under mu */
            if (ch->use_lockfree && ch->cap > 0 && ch->elem_size <= sizeof(void*)) {
                int rc = lf_dequeue(ch, out_value, NULL);
                if (rc == 0) {
                wake_one_send_waiter(ch);
                cc_chan_unlock(ch);
                    wake_batch_flush();
                    return 0;
                }
            } else if (ch->cap > 0 && !ch->use_lockfree && ch->count > 0) {
                memcpy(out_value, (char*)ch->buf + (ch->head * ch->elem_size), ch->elem_size);
                ch->head = (ch->head + 1) % ch->cap;
                ch->count--;
                cc_chan_unlock(ch);
                return 0;
            }
            cc_chan_unlock(ch);
            return ch->tx_error_code ? ch->tx_error_code : EPIPE;
        }

        /* Direct handoff from parked sender */
        cc__fiber_wait_node* snode = pop_send_waiter(ch);
        if (snode) {
            channel_load_slot(snode->data, out_value, ch->elem_size);
            atomic_store_explicit(&snode->notified, NOTIFY_DATA, memory_order_release);
            if (snode->is_select && snode->select_group) {
                cc__select_wait_group* g = (cc__select_wait_group*)snode->select_group;
                atomic_fetch_add_explicit(&g->signaled, 1, memory_order_release);
            }
            unpark_or_signal(snode, &ch->not_full);
            cc_chan_unlock(ch);
            wake_batch_flush();
            cc__chan_signal_activity(ch);
            DBG_INC(g_dbg_handoff_recv);
            return 0;
        }

        /* Retry dequeue under mu */
        if (ch->use_lockfree && ch->cap > 0 && ch->elem_size <= sizeof(void*)) {
            int rc = lf_dequeue(ch, out_value, NULL);
            if (rc == 0) {
                wake_one_send_waiter(ch);
                pthread_cond_signal(&ch->not_full);
                cc_chan_unlock(ch);
                wake_batch_flush();
                cc__chan_signal_activity(ch);
                return 0;
            }
        } else if (ch->cap > 0 && !ch->use_lockfree) {
            if (ch->count > 0) {
                memcpy(out_value, (char*)ch->buf + (ch->head * ch->elem_size), ch->elem_size);
                ch->head = (ch->head + 1) % ch->cap;
                ch->count--;
                wake_one_send_waiter(ch);
                pthread_cond_signal(&ch->not_full);
                cc_chan_unlock(ch);
                wake_batch_flush();
                cc__chan_signal_activity(ch);
                return 0;
            }
        }

        /* Check deadline before parking */
        if (deadline) {
            struct timespec now;
            clock_gettime(CLOCK_REALTIME, &now);
            if (now.tv_sec > deadline->tv_sec || (now.tv_sec == deadline->tv_sec && now.tv_nsec >= deadline->tv_nsec)) {
                cc_chan_unlock(ch);
                return ETIMEDOUT;
            }
        }

        /* Append waiter, unlock, park (Invariant 2)
         * Clear stale pending_unpark before making ourselves visible. */
        if (fiber) {
            cc__fiber_clear_pending_unpark();
            cc__fiber_wait_node node = {0};
            node.fiber = fiber;
            node.data = out_value;
            atomic_store_explicit(&node.notified, NOTIFY_WAITING, memory_order_release);
            add_recv_waiter(ch, &node);
            cc_chan_unlock(ch);

            cc__fiber_set_park_obj(ch);
            cc__chan_wait_flag(&node.notified, NOTIFY_WAITING);

            /* Post-wake (v1-style): inspect notification under lock first. */
            cc_chan_lock(ch);
            int notified = atomic_load_explicit(&node.notified, memory_order_acquire);
            if (notified == NOTIFY_DATA) {
                cc_chan_unlock(ch);
                return 0;
            }
            if (notified == NOTIFY_SIGNAL) {
                atomic_store_explicit(&node.notified, NOTIFY_WAITING, memory_order_release);
                if (node.in_wait_list) remove_recv_waiter(ch, &node);
                cc_chan_unlock(ch);
                continue;
            }
            if (node.in_wait_list) remove_recv_waiter(ch, &node);
            cc_chan_unlock(ch);
            /* WOKEN/spurious: retry */
            continue;
        } else {
            /* OS thread: condvar (Invariant 5) */
            if (deadline) {
                pthread_cond_timedwait(&ch->not_empty, &ch->mu, deadline);
            } else {
                pthread_cond_wait(&ch->not_empty, &ch->mu);
            }
            cc_chan_unlock(ch);
            continue;
        }
    }
}

/* ============================================================================
 * Try (non-blocking) send/recv
 * ============================================================================ */
int cc_chan_try_send(CCChan* ch, const void* value, size_t value_size) {
    if (!ch || !value || value_size == 0) return EINVAL;

    /* Lock-free fast path */
    if (ch->use_lockfree && ch->cap > 0 && ch->elem_size == value_size && ch->buf && ch->elem_size <= sizeof(void*)) {
        if (ch->closed) return EPIPE;
        if (ch->rx_error_closed) return ch->rx_error_code;
        atomic_fetch_add_explicit(&ch->lfqueue_inflight, 1, memory_order_relaxed);
        int rc = lf_enqueue(ch, value, NULL);
        atomic_fetch_sub_explicit(&ch->lfqueue_inflight, 1, memory_order_relaxed);
        if (rc == 0) {
            cc_chan_lock(ch);
            wake_one_recv_waiter(ch);
            pthread_cond_signal(&ch->not_empty);
            cc_chan_unlock(ch);
            wake_batch_flush();
            cc__chan_signal_activity(ch);
            return 0;
        }
        return EAGAIN;
    }

    /* Unbuffered try: only succeeds if a receiver is parked */
    if (ch->cap == 0) {
        if (ch->closed) return EPIPE;
        if (ch->rx_error_closed) return ch->rx_error_code;
        cc_chan_lock(ch);
        int buferr = cc_chan_ensure_buf(ch, value_size);
        if (buferr != 0) { cc_chan_unlock(ch); return buferr; }
        if (ch->closed) { cc_chan_unlock(ch); return EPIPE; }
        cc__fiber_wait_node* rnode = pop_recv_waiter(ch);
        if (!rnode) {
            cc_chan_unlock(ch);
            return EAGAIN;
        }
        channel_store_slot(rnode->data, value, ch->elem_size);
        atomic_store_explicit(&rnode->notified, NOTIFY_DATA, memory_order_release);
        if (rnode->is_select && rnode->select_group) {
            cc__select_wait_group* g = (cc__select_wait_group*)rnode->select_group;
            atomic_fetch_add_explicit(&g->signaled, 1, memory_order_release);
        }
        unpark_or_signal(rnode, &ch->not_empty);
        cc_chan_unlock(ch);
        wake_batch_flush();
        cc__chan_signal_activity(ch);
        return 0;
    }

    /* Mutex-buffered try */
    cc_chan_lock(ch);
    int buferr = cc_chan_ensure_buf(ch, value_size);
    if (buferr != 0) { cc_chan_unlock(ch); return buferr; }
    if (ch->closed) { cc_chan_unlock(ch); return EPIPE; }
    if (ch->rx_error_closed) { cc_chan_unlock(ch); return ch->rx_error_code; }
    if (ch->count >= ch->cap) {
        int dm = handle_drop_mode_send(ch, value);
        if (dm >= 0) {
            wake_one_recv_waiter(ch);
            pthread_cond_signal(&ch->not_empty);
            cc_chan_unlock(ch);
            wake_batch_flush();
            return dm;
        }
        cc_chan_unlock(ch);
        return EAGAIN;
    }
    memcpy((char*)ch->buf + (ch->tail * ch->elem_size), value, ch->elem_size);
    ch->tail = (ch->tail + 1) % ch->cap;
    ch->count++;
    wake_one_recv_waiter(ch);
    pthread_cond_signal(&ch->not_empty);
    cc_chan_unlock(ch);
    wake_batch_flush();
    return 0;
}

int cc_chan_try_recv(CCChan* ch, void* out_value, size_t value_size) {
    if (!ch || !out_value || value_size == 0) return EINVAL;

    /* Lock-free fast path */
    if (ch->use_lockfree && ch->cap > 0 && ch->elem_size == value_size && ch->buf && ch->elem_size <= sizeof(void*)) {
        int rc = lf_dequeue(ch, out_value, NULL);
        if (rc == 0) {
            cc_chan_lock(ch);
            wake_one_send_waiter(ch);
            pthread_cond_signal(&ch->not_full);
            cc_chan_unlock(ch);
            wake_batch_flush();
            cc__chan_signal_activity(ch);
            return 0;
        }
        if (ch->closed) {
            if (atomic_load_explicit(&ch->lfqueue_inflight, memory_order_acquire) > 0) return EAGAIN;
            return ch->tx_error_code ? ch->tx_error_code : EPIPE;
        }
        return EAGAIN;
    }

    /* Unbuffered try: only succeeds if a sender is parked */
    if (ch->cap == 0) {
        cc_chan_lock(ch);
        int buferr = cc_chan_ensure_buf(ch, value_size);
        if (buferr != 0) { cc_chan_unlock(ch); return buferr; }
        cc__fiber_wait_node* snode = pop_send_waiter(ch);
        if (!snode) {
            cc_chan_unlock(ch);
            return ch->closed ? (ch->tx_error_code ? ch->tx_error_code : EPIPE) : EAGAIN;
        }
        channel_load_slot(snode->data, out_value, ch->elem_size);
        atomic_store_explicit(&snode->notified, NOTIFY_DATA, memory_order_release);
        if (snode->is_select && snode->select_group) {
            cc__select_wait_group* g = (cc__select_wait_group*)snode->select_group;
            atomic_fetch_add_explicit(&g->signaled, 1, memory_order_release);
        }
        unpark_or_signal(snode, &ch->not_full);
        cc_chan_unlock(ch);
        wake_batch_flush();
        cc__chan_signal_activity(ch);
        return 0;
    }

    /* Mutex-buffered try */
    cc_chan_lock(ch);
    int buferr = cc_chan_ensure_buf(ch, value_size);
    if (buferr != 0) { cc_chan_unlock(ch); return buferr; }
    if (ch->count == 0) {
        cc_chan_unlock(ch);
        return ch->closed ? (ch->tx_error_code ? ch->tx_error_code : EPIPE) : EAGAIN;
    }
    memcpy(out_value, (char*)ch->buf + (ch->head * ch->elem_size), ch->elem_size);
    ch->head = (ch->head + 1) % ch->cap;
    ch->count--;
    wake_one_send_waiter(ch);
    pthread_cond_signal(&ch->not_full);
    cc_chan_unlock(ch);
    wake_batch_flush();
    return 0;
}

/* ============================================================================
 * Timed send/recv
 * ============================================================================ */
int cc_chan_timed_send(CCChan* ch, const void* value, size_t value_size, const struct timespec* abs_deadline) {
    if (!ch || !value || value_size == 0) return EINVAL;

    /* Fast path try */
    if (ch->use_lockfree && ch->cap > 0 && ch->elem_size == value_size && ch->buf && ch->elem_size <= sizeof(void*)) {
        if (ch->closed) return EPIPE;
        if (ch->rx_error_closed) return ch->rx_error_code;
        atomic_fetch_add_explicit(&ch->lfqueue_inflight, 1, memory_order_relaxed);
        int rc = lf_enqueue(ch, value, NULL);
        atomic_fetch_sub_explicit(&ch->lfqueue_inflight, 1, memory_order_relaxed);
        if (rc == 0) {
            cc_chan_lock(ch);
            wake_one_recv_waiter(ch);
            pthread_cond_signal(&ch->not_empty);
            cc_chan_unlock(ch);
            wake_batch_flush();
            cc__chan_signal_activity(ch);
            return 0;
        }
    }

    return chan_send_slow(ch, value, value_size, abs_deadline);
}

int cc_chan_timed_recv(CCChan* ch, void* out_value, size_t value_size, const struct timespec* abs_deadline) {
    if (!ch || !out_value || value_size == 0) return EINVAL;

    /* Fast path try */
    if (ch->use_lockfree && ch->cap > 0 && ch->elem_size == value_size && ch->buf && ch->elem_size <= sizeof(void*)) {
        int rc = lf_dequeue(ch, out_value, NULL);
        if (rc == 0) {
            cc_chan_lock(ch);
            wake_one_send_waiter(ch);
            pthread_cond_signal(&ch->not_full);
            cc_chan_unlock(ch);
            wake_batch_flush();
            cc__chan_signal_activity(ch);
            return 0;
        }
        if (ch->closed) {
            /* Drain in-flight */
            while (atomic_load_explicit(&ch->lfqueue_inflight, memory_order_acquire) > 0) {
                rc = lf_dequeue(ch, out_value, NULL);
                if (rc == 0) return 0;
                sched_yield();
            }
            rc = lf_dequeue(ch, out_value, NULL);
            if (rc == 0) return 0;
            return ch->tx_error_code ? ch->tx_error_code : EPIPE;
        }
    }

    return chan_recv_slow(ch, out_value, value_size, abs_deadline);
}

/* ============================================================================
 * Deadline-aware helpers
 * ============================================================================ */
int cc_chan_deadline_send(CCChan* ch, const void* value, size_t value_size, const CCDeadline* deadline) {
    if (deadline && deadline->cancelled) return ECANCELED;
    struct timespec ts;
    const struct timespec* p = cc_deadline_as_timespec(deadline, &ts);
    return cc_chan_timed_send(ch, value, value_size, p);
}

int cc_chan_deadline_recv(CCChan* ch, void* out_value, size_t value_size, const CCDeadline* deadline) {
    if (deadline && deadline->cancelled) return ECANCELED;
    struct timespec ts;
    const struct timespec* p = cc_deadline_as_timespec(deadline, &ts);
    return cc_chan_timed_recv(ch, out_value, value_size, p);
}

/* ============================================================================
 * send_take helpers (thin wrappers)
 * ============================================================================ */
int cc_chan_send_take(CCChan* ch, void* ptr) {
    if (!ch || !ch->allow_take) return EINVAL;
    if (ch->elem_size && ch->elem_size != sizeof(void*)) return EINVAL;
    return cc_chan_send(ch, &ptr, sizeof(void*));
}

int cc_chan_try_send_take(CCChan* ch, void* ptr) {
    if (!ch || !ch->allow_take) return EINVAL;
    if (ch->elem_size && ch->elem_size != sizeof(void*)) return EINVAL;
    return cc_chan_try_send(ch, &ptr, sizeof(void*));
}

int cc_chan_timed_send_take(CCChan* ch, void* ptr, const struct timespec* abs_deadline) {
    if (!ch || !ch->allow_take) return EINVAL;
    if (ch->elem_size && ch->elem_size != sizeof(void*)) return EINVAL;
    return cc_chan_timed_send(ch, &ptr, sizeof(void*), abs_deadline);
}

int cc_chan_deadline_send_take(CCChan* ch, void* ptr, const CCDeadline* deadline) {
    struct timespec ts;
    const struct timespec* p = cc_deadline_as_timespec(deadline, &ts);
    return cc_chan_timed_send_take(ch, ptr, p);
}

/* Slice send_take */
static int cc_chan_check_slice_take(const CCSlice* slice) {
    if (!slice) return EINVAL;
    if (!cc_slice_is_unique(*slice)) return EINVAL;
    if (!cc_slice_is_transferable(*slice)) return EINVAL;
    if (cc_slice_is_subslice(*slice)) return EINVAL;
    return 0;
}

int cc_chan_send_take_slice(CCChan* ch, const CCSliceUnique* slice) {
    if (!ch || !ch->allow_take) return EINVAL;
    int elig = cc_chan_check_slice_take(slice);
    if (elig != 0) return elig;
    if (ch->elem_size && ch->elem_size != sizeof(CCSlice)) return EINVAL;
    return cc_chan_send(ch, slice, sizeof(CCSlice));
}

int cc_chan_try_send_take_slice(CCChan* ch, const CCSliceUnique* slice) {
    if (!ch || !ch->allow_take) return EINVAL;
    int elig = cc_chan_check_slice_take(slice);
    if (elig != 0) return elig;
    if (ch->elem_size && ch->elem_size != sizeof(CCSlice)) return EINVAL;
    return cc_chan_try_send(ch, slice, sizeof(CCSlice));
}

int cc_chan_timed_send_take_slice(CCChan* ch, const CCSliceUnique* slice, const struct timespec* abs_deadline) {
    if (!ch || !ch->allow_take) return EINVAL;
    int elig = cc_chan_check_slice_take(slice);
    if (elig != 0) return elig;
    if (ch->elem_size && ch->elem_size != sizeof(CCSlice)) return EINVAL;
    return cc_chan_timed_send(ch, slice, sizeof(CCSlice), abs_deadline);
}

int cc_chan_deadline_send_take_slice(CCChan* ch, const CCSliceUnique* slice, const CCDeadline* deadline) {
    struct timespec ts;
    const struct timespec* p = cc_deadline_as_timespec(deadline, &ts);
    return cc_chan_timed_send_take_slice(ch, slice, p);
}

/* ============================================================================
 * Nursery-aware helpers
 * ============================================================================ */
int cc_chan_nursery_send(CCChan* ch, CCNursery* n, const void* value, size_t value_size) {
    CCDeadline d = cc_nursery_as_deadline(n);
    return cc_chan_deadline_send(ch, value, value_size, &d);
}

int cc_chan_nursery_recv(CCChan* ch, CCNursery* n, void* out_value, size_t value_size) {
    CCDeadline d = cc_nursery_as_deadline(n);
    return cc_chan_deadline_recv(ch, out_value, value_size, &d);
}

int cc_chan_nursery_send_take(CCChan* ch, CCNursery* n, void* ptr) {
    CCDeadline d = cc_nursery_as_deadline(n);
    return cc_chan_deadline_send_take(ch, ptr, &d);
}

int cc_chan_nursery_send_take_slice(CCChan* ch, CCNursery* n, const CCSliceUnique* slice) {
    CCDeadline d = cc_nursery_as_deadline(n);
    return cc_chan_deadline_send_take_slice(ch, slice, &d);
}

/* ============================================================================
 * Async send/recv via executor
 * ============================================================================ */
typedef struct {
    CCChan* ch;
    const void* value;
    void* out_value;
    size_t size;
    int is_send;
    CCDeadline deadline;
    CCAsyncHandle* handle;
} CCChanAsyncCtx;

static void cc__chan_async_job(void* arg) {
    CCChanAsyncCtx* ctx = (CCChanAsyncCtx*)arg;
    int err = 0;
    if (cc_deadline_expired(&ctx->deadline)) {
        err = ETIMEDOUT;
    } else {
        if (ctx->is_send) err = cc_chan_deadline_send(ctx->ch, ctx->value, ctx->size, &ctx->deadline);
        else err = cc_chan_deadline_recv(ctx->ch, ctx->out_value, ctx->size, &ctx->deadline);
    }
    cc_chan_send(ctx->handle->done, &err, sizeof(int));
    free(ctx);
}

static int cc__chan_async_submit(CCExec* ex, CCChan* ch, const void* val, void* out, size_t size, CCChanAsync* out_async, const CCDeadline* deadline, int is_send) {
    if (!ex || !ch || !out_async) return EINVAL;
    CC_ASYNC_HANDLE_ALLOC(&out_async->handle, 1);
    CCChanAsyncCtx* ctx = (CCChanAsyncCtx*)malloc(sizeof(*ctx));
    if (!ctx) { cc_chan_free(out_async->handle.done); out_async->handle.done = NULL; return ENOMEM; }
    ctx->ch = ch; ctx->value = val; ctx->out_value = out; ctx->size = size; ctx->is_send = is_send;
    ctx->deadline = deadline ? *deadline : cc_deadline_none();
    ctx->handle = &out_async->handle;
    int sub = cc_exec_submit(ex, cc__chan_async_job, ctx);
    if (sub != 0) { free(ctx); cc_chan_free(out_async->handle.done); out_async->handle.done = NULL; return sub; }
    return 0;
}

int cc_chan_send_async(CCExec* ex, CCChan* ch, const void* value, size_t value_size, CCChanAsync* out, const CCDeadline* deadline) {
    return cc__chan_async_submit(ex, ch, value, NULL, value_size, out, deadline, 1);
}

int cc_chan_recv_async(CCExec* ex, CCChan* ch, void* out_value, size_t value_size, CCChanAsync* out, const CCDeadline* deadline) {
    return cc__chan_async_submit(ex, ch, NULL, out_value, value_size, out, deadline, 0);
}

/* ============================================================================
 * Match / Select (spec §Select / match)
 * ============================================================================ */
static int cc__chan_match_try_from(CCChanMatchCase* cases, size_t n, size_t* ready_index, size_t start) {
    if (!cases || n == 0 || !ready_index) return EINVAL;
    for (size_t k = 0; k < n; ++k) {
        size_t i = (start + k) % n;
        CCChanMatchCase* c = &cases[i];
        if (!c->ch || c->elem_size == 0) continue;
        int rc = c->is_send
            ? cc_chan_try_send(c->ch, c->send_buf, c->elem_size)
            : cc_chan_try_recv(c->ch, c->recv_buf, c->elem_size);
        if (rc == 0) { *ready_index = i; return 0; }
        if (rc == EPIPE) { *ready_index = i; return EPIPE; }
    }
    return EAGAIN;
}

int cc_chan_match_try(CCChanMatchCase* cases, size_t n, size_t* ready_index) {
    return cc__chan_match_try_from(cases, n, ready_index, 0);
}

int cc_chan_match_deadline(CCChanMatchCase* cases, size_t n, size_t* ready_index, const CCDeadline* deadline) {
    if (!cases || n == 0 || !ready_index) return EINVAL;
    struct timespec ts;
    const struct timespec* p = cc_deadline_as_timespec(deadline, &ts);
    cc__fiber* fiber = cc__fiber_in_context() ? cc__fiber_current() : NULL;
    static _Atomic uint64_t g_match_rr = 0;

    while (1) {
        /* Round-robin try */
        size_t start = n ? (size_t)(atomic_fetch_add_explicit(&g_match_rr, 1, memory_order_relaxed) % n) : 0;
        int rc = cc__chan_match_try_from(cases, n, ready_index, start);
        if (rc == 0 || rc == EPIPE) return rc;
        if (rc != EAGAIN) return rc;

        /* Check deadline */
        if (p) {
            struct timespec now;
            clock_gettime(CLOCK_REALTIME, &now);
            if (now.tv_sec > p->tv_sec || (now.tv_sec == p->tv_sec && now.tv_nsec >= p->tv_nsec))
                return ETIMEDOUT;
        }

        if (fiber && !p) {
            /* Fiber-aware select: register nodes on all channels */
            cc__fiber_clear_pending_unpark();

            cc__select_wait_group group = {0};
            group.fiber = fiber;
            atomic_store_explicit(&group.signaled, 0, memory_order_release);
            atomic_store_explicit(&group.selected_index, -1, memory_order_release);

            cc__fiber_wait_node nodes[n];
            for (size_t i = 0; i < n; ++i) {
                CCChanMatchCase* c = &cases[i];
                memset(&nodes[i], 0, sizeof(nodes[i]));
                nodes[i].fiber = fiber;
                nodes[i].data = c->is_send ? (void*)c->send_buf : c->recv_buf;
                atomic_store_explicit(&nodes[i].notified, NOTIFY_WAITING, memory_order_release);
                nodes[i].select_group = &group;
                nodes[i].select_index = i;
                nodes[i].is_select = 1;
                if (!c->ch) continue;
                pthread_mutex_lock(&c->ch->mu);
                if (c->is_send) {
                    list_append(&c->ch->send_waiters_head, &c->ch->send_waiters_tail, &nodes[i]);
                } else {
                    list_append(&c->ch->recv_waiters_head, &c->ch->recv_waiters_tail, &nodes[i]);
                }
                cc_chan_unlock(c->ch);
            }

            /* Check if any node was already notified during registration */
            for (size_t i = 0; i < n; ++i) {
                int notified = atomic_load_explicit(&nodes[i].notified, memory_order_acquire);
                if (notified == NOTIFY_DATA || notified == NOTIFY_WOKEN) {
                    /* Clean up all nodes and return */
                    for (size_t j = 0; j < n; ++j) {
                        if (!cases[j].ch) continue;
                        pthread_mutex_lock(&cases[j].ch->mu);
                        if (nodes[j].in_wait_list) {
                            if (cases[j].is_send) list_remove(&cases[j].ch->send_waiters_head, &cases[j].ch->send_waiters_tail, &nodes[j]);
                            else list_remove(&cases[j].ch->recv_waiters_head, &cases[j].ch->recv_waiters_tail, &nodes[j]);
                        }
                        cc_chan_unlock(cases[j].ch);
                    }
                    if (notified == NOTIFY_DATA) { *ready_index = i; return 0; }
                    /* WOKEN: a close or signal arrived, loop will retry */
                    goto select_continue;
                }
            }

            /* Wake any senders parked on recv channels (for handoff) */
            {
                int did_wake = 0;
                for (size_t i = 0; i < n; ++i) {
                    CCChanMatchCase* c = &cases[i];
                    if (!c->ch) continue;
                    pthread_mutex_lock(&c->ch->mu);
                    if (!c->is_send && c->ch->send_waiters_head) {
                        wake_one_send_waiter(c->ch);
                        did_wake = 1;
                    }
                    cc_chan_unlock(c->ch);
                }
                if (did_wake) wake_batch_flush();
            }

            /* Re-check after waking partners */
            for (size_t i = 0; i < n; ++i) {
                int notified = atomic_load_explicit(&nodes[i].notified, memory_order_acquire);
                if (notified == NOTIFY_DATA || notified == NOTIFY_WOKEN) {
                    for (size_t j = 0; j < n; ++j) {
                        if (!cases[j].ch) continue;
                        pthread_mutex_lock(&cases[j].ch->mu);
                        if (nodes[j].in_wait_list) {
                            if (cases[j].is_send) list_remove(&cases[j].ch->send_waiters_head, &cases[j].ch->send_waiters_tail, &nodes[j]);
                            else list_remove(&cases[j].ch->recv_waiters_head, &cases[j].ch->recv_waiters_tail, &nodes[j]);
                        }
                        cc_chan_unlock(cases[j].ch);
                    }
                    if (notified == NOTIFY_DATA) { *ready_index = i; return 0; }
                    goto select_continue;
                }
            }

            /* Park loop: wait for group.signaled to change */
            while (atomic_load_explicit(&group.selected_index, memory_order_acquire) == -1) {
                int seq = atomic_load_explicit(&group.signaled, memory_order_acquire);
                if (atomic_load_explicit(&group.selected_index, memory_order_acquire) != -1) break;
                cc__fiber_clear_pending_unpark();
                cc__chan_wait_flag(&group.signaled, seq);

                /* Check all nodes for notification */
                int saw_notify = 0;
                for (size_t i = 0; i < n; ++i) {
                    int notified = atomic_load_explicit(&nodes[i].notified, memory_order_acquire);
                    if (notified == NOTIFY_DATA || notified == NOTIFY_WOKEN) { saw_notify = 1; break; }
                }
                if (saw_notify) break;
            }

            /* Cleanup: remove all nodes from all channels */
            for (size_t i = 0; i < n; ++i) {
                if (!cases[i].ch) continue;
                pthread_mutex_lock(&cases[i].ch->mu);
                if (nodes[i].in_wait_list) {
                    if (cases[i].is_send) list_remove(&cases[i].ch->send_waiters_head, &cases[i].ch->send_waiters_tail, &nodes[i]);
                    else list_remove(&cases[i].ch->recv_waiters_head, &cases[i].ch->recv_waiters_tail, &nodes[i]);
                }
                cc_chan_unlock(cases[i].ch);
            }

            /* Check for DATA notification (direct handoff) */
            for (size_t i = 0; i < n; ++i) {
                int notified = atomic_load_explicit(&nodes[i].notified, memory_order_acquire);
                if (notified == NOTIFY_DATA) { *ready_index = i; return 0; }
            }

            /* Check selected_index winner and wait for its notification */
            int sel = atomic_load_explicit(&group.selected_index, memory_order_acquire);
            if (sel >= 0 && sel < (int)n) {
                int notified = atomic_load_explicit(&nodes[sel].notified, memory_order_acquire);
                if (notified == NOTIFY_DATA) { *ready_index = (size_t)sel; return 0; }
                if (notified == NOTIFY_WOKEN) {
                    /* Winner was woken (e.g., close) — check closed status */
                    if (cases[sel].ch && cases[sel].ch->closed) { *ready_index = (size_t)sel; return EPIPE; }
                }
                /* Winner signaled but hasn't completed: wait for it */
                for (int spin = 0; spin < 100; spin++) {
                    notified = atomic_load_explicit(&nodes[sel].notified, memory_order_acquire);
                    if (notified == NOTIFY_DATA) { *ready_index = (size_t)sel; return 0; }
                    if (notified == NOTIFY_WOKEN) break;
                    if (fiber) {
                        cc__fiber_set_park_obj(cases[sel].ch);
                        cc__chan_wait_flag(&nodes[sel].notified, NOTIFY_WAITING);
                        break;
                    }
                }
                notified = atomic_load_explicit(&nodes[sel].notified, memory_order_acquire);
                if (notified == NOTIFY_DATA) { *ready_index = (size_t)sel; return 0; }
            }

            /* WOKEN/spurious: scan all nodes for any data */
            for (size_t i = 0; i < n; ++i) {
                int notified = atomic_load_explicit(&nodes[i].notified, memory_order_acquire);
                if (notified == NOTIFY_DATA) { *ready_index = i; return 0; }
            }

            /* Fall through to retry */
            select_continue:;
        } else {
            /* Non-fiber or timed: use global broadcast condvar */
            atomic_fetch_add_explicit(&g_select_waiters, 1, memory_order_relaxed);
            pthread_mutex_lock(&g_chan_broadcast_mu);
            if (p) {
                pthread_cond_timedwait(&g_chan_broadcast_cv, &g_chan_broadcast_mu, p);
            } else {
                pthread_cond_wait(&g_chan_broadcast_cv, &g_chan_broadcast_mu);
            }
            pthread_mutex_unlock(&g_chan_broadcast_mu);
            atomic_fetch_sub_explicit(&g_select_waiters, 1, memory_order_relaxed);
        }
    }
}

int cc_chan_match_select(CCChanMatchCase* cases, size_t n, size_t* ready_index, const CCDeadline* deadline) {
    return cc_chan_match_deadline(cases, n, ready_index, deadline);
}

/* ============================================================================
 * Async select
 * ============================================================================ */
typedef struct {
    CCChanMatchCase* cases;
    size_t n;
    size_t* ready_index;
    CCAsyncHandle* handle;
    CCDeadline deadline;
} CCChanMatchAsyncCtx;

static void cc__chan_match_async_job(void* arg) {
    CCChanMatchAsyncCtx* ctx = (CCChanMatchAsyncCtx*)arg;
    int err = cc_chan_match_select(ctx->cases, ctx->n, ctx->ready_index, &ctx->deadline);
    cc_chan_send(ctx->handle->done, &err, sizeof(int));
    free(ctx);
}

int cc_chan_match_select_async(CCExec* ex, CCChanMatchCase* cases, size_t n, size_t* ready_index, CCAsyncHandle* h, const CCDeadline* deadline) {
    if (!ex || !cases || n == 0 || !ready_index || !h) return EINVAL;
    CC_ASYNC_HANDLE_ALLOC(h, 1);
    CCChanMatchAsyncCtx* ctx = (CCChanMatchAsyncCtx*)malloc(sizeof(*ctx));
    if (!ctx) { cc_chan_free(h->done); h->done = NULL; return ENOMEM; }
    ctx->cases = cases; ctx->n = n; ctx->ready_index = ready_index; ctx->handle = h;
    ctx->deadline = deadline ? *deadline : cc_deadline_none();
    int sub = cc_exec_submit(ex, cc__chan_match_async_job, ctx);
    if (sub != 0) { free(ctx); cc_chan_free(h->done); h->done = NULL; return sub; }
    return 0;
}

/* Future-based async select */
typedef struct {
    CCChanMatchCase* cases;
    size_t n;
    size_t* ready_index;
    CCFuture* fut;
    CCDeadline deadline;
} CCChanMatchFutureCtx;

static void cc__chan_match_future_job(void* arg) {
    CCChanMatchFutureCtx* ctx = (CCChanMatchFutureCtx*)arg;
    int err = cc_chan_match_select(ctx->cases, ctx->n, ctx->ready_index, &ctx->deadline);
    int out_err = err < 0 ? err : 0;
    cc_chan_send(ctx->fut->handle.done, &out_err, sizeof(int));
    free(ctx);
}

int cc_chan_match_select_future(CCExec* ex, CCChanMatchCase* cases, size_t n, size_t* ready_index, CCFuture* f, const CCDeadline* deadline) {
    if (!ex || !cases || n == 0 || !ready_index || !f) return EINVAL;
    cc_future_init(f);
    CC_ASYNC_HANDLE_ALLOC(&f->handle, 1);
    CCChanMatchFutureCtx* ctx = (CCChanMatchFutureCtx*)malloc(sizeof(*ctx));
    if (!ctx) { cc_future_free(f); return ENOMEM; }
    ctx->cases = cases; ctx->n = n; ctx->ready_index = ready_index; ctx->fut = f;
    ctx->deadline = deadline ? *deadline : cc_deadline_none();
    int sub = cc_exec_submit(ex, cc__chan_match_future_job, ctx);
    if (sub != 0) { free(ctx); cc_future_free(f); return sub; }
    return 0;
}

/* ============================================================================
 * Poll-based channel tasks (CCTaskIntptr)
 * ============================================================================ */
#include <ccc/std/task.cch>

typedef struct {
    CCChan* ch;
    void* buf;
    size_t elem_size;
    const CCDeadline* deadline;
    int is_send;
    int completed;
    int result;
    int pending_async;
    CCChanAsync async;
} CCChanTaskFrame;

static CCFutureStatus cc__chan_task_poll(void* frame, intptr_t* out_val, int* out_err) {
    CCChanTaskFrame* f = (CCChanTaskFrame*)frame;
    if (f->completed) {
        if (out_val) *out_val = (intptr_t)f->result;
        if (out_err) *out_err = f->result;
        return CC_FUTURE_READY;
    }

    if (f->pending_async) {
        int err = 0;
        int rc = cc_chan_try_recv(f->async.handle.done, &err, sizeof(err));
        if (rc == 0) {
            cc_async_handle_free(&f->async.handle);
            f->pending_async = 0; f->completed = 1; f->result = err;
            if (out_val) *out_val = (intptr_t)err; if (out_err) *out_err = err;
            return CC_FUTURE_READY;
        }
        if (rc == EPIPE) {
            cc_async_handle_free(&f->async.handle);
            f->pending_async = 0; f->completed = 1; f->result = EPIPE;
            if (out_val) *out_val = EPIPE; if (out_err) *out_err = EPIPE;
            return CC_FUTURE_READY;
        }
        return CC_FUTURE_PENDING;
    }

    if (f->deadline && cc_deadline_expired(f->deadline)) {
        f->completed = 1; f->result = ETIMEDOUT;
        if (out_val) *out_val = ETIMEDOUT; if (out_err) *out_err = ETIMEDOUT;
        return CC_FUTURE_READY;
    }

    int rc;
    if (f->is_send) rc = cc_chan_try_send(f->ch, f->buf, f->elem_size);
    else rc = cc_chan_try_recv(f->ch, f->buf, f->elem_size);

    if (rc == EAGAIN) {
        if (cc__fiber_in_context()) {
            int err;
            if (f->is_send) err = cc_chan_timed_send(f->ch, f->buf, f->elem_size, NULL);
            else err = cc_chan_timed_recv(f->ch, f->buf, f->elem_size, NULL);
            wake_batch_flush();
            f->completed = 1; f->result = err;
            if (out_val) *out_val = (intptr_t)err; if (out_err) *out_err = err;
            return CC_FUTURE_READY;
        }
        CCExec* ex = cc_async_runtime_exec();
        if (ex) {
            int sub = f->is_send
                ? cc_chan_send_async(ex, f->ch, f->buf, f->elem_size, &f->async, f->deadline)
                : cc_chan_recv_async(ex, f->ch, f->buf, f->elem_size, &f->async, f->deadline);
            if (sub == 0) f->pending_async = 1;
        }
        return CC_FUTURE_PENDING;
    }

    f->completed = 1; f->result = rc;
    if (out_val) *out_val = (intptr_t)rc; if (out_err) *out_err = rc;
    return CC_FUTURE_READY;
}

static int cc__chan_task_wait(void* frame) {
    CCChanTaskFrame* f = (CCChanTaskFrame*)frame;
    if (!f || !f->ch) return EINVAL;
    if (f->pending_async) {
        int err = cc_async_wait_deadline(&f->async.handle, f->deadline);
        f->pending_async = 0; f->completed = 1; f->result = err;
        return err;
    }
    CCChan* ch = f->ch;
    struct timespec ts_val;
    const struct timespec* p = f->deadline ? cc_deadline_as_timespec(f->deadline, &ts_val) : NULL;
    int err = f->is_send
        ? cc_chan_timed_send(ch, f->buf, f->elem_size, p)
        : cc_chan_timed_recv(ch, f->buf, f->elem_size, p);
    wake_batch_flush();
    return err;
}

static void cc__chan_task_drop(void* frame) { free(frame); }

CCTaskIntptr cc_chan_send_task(CCChan* ch, const void* value, size_t value_size) {
    CCTaskIntptr invalid = {0};
    if (!ch || !value || value_size == 0) return invalid;
    CCChanTaskFrame* f = (CCChanTaskFrame*)calloc(1, sizeof(CCChanTaskFrame));
    if (!f) return invalid;
    f->ch = ch; f->buf = (void*)value; f->elem_size = value_size;
    f->deadline = cc_current_deadline(); f->is_send = 1;
    return cc_task_intptr_make_poll_ex(cc__chan_task_poll, cc__chan_task_wait, f, cc__chan_task_drop);
}

CCTaskIntptr cc_chan_recv_task(CCChan* ch, void* out_value, size_t value_size) {
    CCTaskIntptr invalid = {0};
    if (!ch || !out_value || value_size == 0) return invalid;
    CCChanTaskFrame* f = (CCChanTaskFrame*)calloc(1, sizeof(CCChanTaskFrame));
    if (!f) return invalid;
    f->ch = ch; f->buf = out_value; f->elem_size = value_size;
    f->deadline = cc_current_deadline(); f->is_send = 0;
    return cc_task_intptr_make_poll_ex(cc__chan_task_poll, cc__chan_task_wait, f, cc__chan_task_drop);
}
