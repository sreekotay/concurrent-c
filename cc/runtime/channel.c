/*
 * Blocking channel with mutex/cond and fixed capacity.
 * Supports by-value copies and pointer payloads via size argument.
 * Provides blocking, try, timed, and CCDeadline-aware variants.
 * send_take helpers treat payloads as pointers (zero-copy for pointer payloads) when allowed.
 * Backpressure modes: block (default), drop-new, drop-old.
 * Async send/recv via executor offload.
 * Match helpers for polling/selecting across channels.
 *
 * Lock-free MPMC queue for buffered channels (cap > 0):
 * Uses liblfds bounded queue for the hot path, with mutex fallback for blocking.
 */

#include <ccc/cc_channel.cch>

/* liblfds lock-free data structures */
#if defined(__APPLE__) && defined(__MACH__)
#define LFDS711_PAL_OPERATING_SYSTEM
#define LFDS711_PAL_OS_STRING "Darwin"
#define LFDS711_PAL_ASSERT(expression) do { if (!(expression)) LFDS711_MISC_DELIBERATELY_CRASH; } while (0)
#endif
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
#include <ccc/std/net.cch>
#include <ccc/std/async_io.cch>
#include <ccc/std/future.cch>

#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <sched.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdio.h>
#include <stdatomic.h>
#include <stdint.h>

#define CC_CHAN_NOTIFY_NONE   0
#define CC_CHAN_NOTIFY_DATA   1
#define CC_CHAN_NOTIFY_CANCEL 2
#define CC_CHAN_NOTIFY_CLOSE  3
#define CC_CHAN_NOTIFY_SIGNAL 4

/* spinlock_condvar.h is available but pthread_cond works better for channel
 * waiter synchronization due to the mutex integration pattern. */

/* ============================================================================
 * Fiber-Aware Blocking Infrastructure
 * ============================================================================ */

#include "fiber_internal.h"
#include "fiber_sched_boundary.h"
#include "wait_select_internal.h"
#include "channel_wait_internal.h"
/* fiber_sched.c is now included in concurrent_c.c */

/* Defined in nursery.c (same translation unit via runtime/concurrent_c.c). */
extern __thread CCNursery* cc__tls_current_nursery;
/* Thread-local current deadline scope (set by with_deadline lowering). */
__thread CCDeadline* cc__tls_current_deadline = NULL;


static int cc_channel_recv_dedupe_enabled(void) {
    static int enabled = -1;
    if (enabled < 0) {
        const char* v = getenv("CC_CHAN_RECV_DEDUPE");
        enabled = (!v || v[0] != '0') ? 1 : 0;
    }
    return enabled;
}


typedef struct __attribute__((aligned(64))) {
    _Atomic size_t seq;
    void* value;
} cc__ring_cell;



#define cc__chan_dbg_select_event(e, n) ((void)0)
#define cc__chan_dbg_select_group(e, g) ((void)0)
#define cc__chan_dbg_select_wait(e, g, idx, n) ((void)0)
#define cc__chan_debug_invariant(ch, where, msg) ((void)0)
#define cc__chan_debug_check_recv_close(ch, where) ((void)0)


static inline void cc__chan_cpu_pause(void) {
#if defined(__aarch64__) || defined(__arm64__)
    __asm__ volatile("isb");
#elif defined(__x86_64__) || defined(_M_X64)
    __asm__ volatile("pause");
#else
    __asm__ volatile("" ::: "memory");
#endif
}

/* Select handoff accounting (always available, printed when CC_CHAN_DEBUG=1) */
static _Atomic uint64_t g_dbg_select_data_set = 0;      /* sender set notified=DATA on select node */
static _Atomic uint64_t g_dbg_select_data_returned = 0;  /* cc_chan_match_deadline returned 0 via DATA */
static _Atomic uint64_t g_dbg_select_try_returned = 0;   /* cc_chan_match_deadline returned 0 via try */
static _Atomic uint64_t g_dbg_select_close_returned = 0; /* cc_chan_match_deadline returned EPIPE */

static inline void cc__chan_select_dbg_inc(_Atomic uint64_t* counter) {
#if CC_V3_DIAGNOSTICS
    atomic_fetch_add_explicit(counter, 1, memory_order_relaxed);
#else
    (void)counter;
#endif
}


/* ============================================================================
 * Batch Wake Operations
 * ============================================================================ */

#define WAKE_BATCH_SIZE 32

typedef struct {
    cc__fiber* fibers[WAKE_BATCH_SIZE];
    uint32_t attribs[WAKE_BATCH_SIZE];
    size_t count;
} wake_batch;

static __thread wake_batch tls_wake_batch = {{NULL}, {0}, 0};

/* Forward declaration */
static inline void wake_batch_flush(void);
static inline uint32_t cc__chan_wake_sched_attrib(CCChan* ch);
static cc__fiber_wait_node* cc__chan_pop_send_waiter(CCChan* ch);
static inline void cc__chan_finish_recv_from_send_waiter(CCChan* ch, cc__fiber_wait_node* snode, void* out_value);

/* Add a fiber to the wake batch */
static inline void wake_batch_add(cc__fiber* f) {
    if (!f) return;
    wake_batch* b = &tls_wake_batch;
    if (b->count >= WAKE_BATCH_SIZE) {
        wake_batch_flush();
    }
    b->fibers[b->count++] = f;
}

static inline void wake_batch_add_chan(CCChan* ch, cc__fiber* f) {
    if (!f) return;
    wake_batch* b = &tls_wake_batch;
    if (b->count >= WAKE_BATCH_SIZE) {
        wake_batch_flush();
    }
    b->fibers[b->count] = f;
    b->attribs[b->count] = cc__chan_wake_sched_attrib(ch);
    b->count++;
}

/* Flush all pending wakes */
static inline void wake_batch_flush(void) {
    wake_batch* b = &tls_wake_batch;
    for (size_t i = 0; i < b->count; i++) {
        if (b->fibers[i]) {
            cc__fiber_unpark_channel_attrib(b->attribs[i]);
            cc_sched_fiber_wake((CCSchedFiber*)b->fibers[i]);
            b->fibers[i] = NULL;
            b->attribs[i] = 0;
        }
    }
    b->count = 0;
}

typedef struct {
    _Atomic int* flag;
    int expected;
    const char* reason;
    void* obj;
    cc__fiber_wait_node* node;
} cc__chan_wait_flag_ctx;

static inline int cc__chan_recv_empty_trace_enabled(void) {
    static _Atomic int cached = -1;
    int value = atomic_load_explicit(&cached, memory_order_relaxed);
    if (value >= 0) return value;
    const char* env = getenv("CC_CHAN_TRACE_RECV_EMPTY");
    int enabled = (env && env[0] == '1') ? 1 : 0;
    int expected = -1;
    (void)atomic_compare_exchange_strong_explicit(&cached, &expected, enabled,
                                                  memory_order_relaxed,
                                                  memory_order_relaxed);
    return atomic_load_explicit(&cached, memory_order_relaxed);
}

static inline int cc__chan_flow_trace_enabled(void) {
    static _Atomic int cached = -1;
    int value = atomic_load_explicit(&cached, memory_order_relaxed);
    if (value >= 0) return value;
    const char* env = getenv("CC_CHAN_TRACE_FLOW");
    int enabled = (env && env[0] == '1') ? 1 : 0;
    int expected = -1;
    (void)atomic_compare_exchange_strong_explicit(&cached, &expected, enabled,
                                                  memory_order_relaxed,
                                                  memory_order_relaxed);
    return atomic_load_explicit(&cached, memory_order_relaxed);
}

static inline int cc__chan_recv_empty_trace_match_obj(void* obj) {
    static _Atomic int mode = -1; /* -1 unknown, 0 all, 1 filtered */
    static uintptr_t target = 0;
    int cached = atomic_load_explicit(&mode, memory_order_relaxed);
    if (cached < 0) {
        const char* env = getenv("CC_CHAN_TRACE_OBJ");
        int next_mode = 0;
        uintptr_t next_target = 0;
        if (env && env[0]) {
            next_target = (uintptr_t)strtoull(env, NULL, 0);
            next_mode = (next_target != 0) ? 1 : 0;
        }
        target = next_target;
        int expected = -1;
        (void)atomic_compare_exchange_strong_explicit(&mode, &expected, next_mode,
                                                      memory_order_relaxed,
                                                      memory_order_relaxed);
        cached = atomic_load_explicit(&mode, memory_order_relaxed);
    }
    if (cached == 0) return 1;
    return (uintptr_t)obj == target;
}

static inline void* cc__chan_trace_item_ptr(const void* item, size_t elem_size) {
    uintptr_t bits = 0;
    if (!item || elem_size == 0) return NULL;
    size_t copy = elem_size < sizeof(bits) ? elem_size : sizeof(bits);
    memcpy(&bits, item, copy);
    return (void*)bits;
}

static inline const char* cc__chan_notify_name(int notify) {
    switch (notify) {
        case CC_CHAN_NOTIFY_NONE: return "none";
        case CC_CHAN_NOTIFY_DATA: return "data";
        case CC_CHAN_NOTIFY_CANCEL: return "cancel";
        case CC_CHAN_NOTIFY_CLOSE: return "close";
        case CC_CHAN_NOTIFY_SIGNAL: return "signal";
        default: return "unknown";
    }
}

static inline int cc__chan_close_trace_enabled(void) {
    static _Atomic int cached = -1;
    int value = atomic_load_explicit(&cached, memory_order_relaxed);
    if (value >= 0) return value;
    const char* env = getenv("CC_CHAN_TRACE_CLOSE");
    int enabled = (env && env[0] == '1') ? 1 : 0;
    int expected = -1;
    (void)atomic_compare_exchange_strong_explicit(&cached, &expected, enabled,
                                                  memory_order_relaxed,
                                                  memory_order_relaxed);
    return atomic_load_explicit(&cached, memory_order_relaxed);
}

static inline int cc__chan_req_wake_trace_enabled(void) {
    static _Atomic int cached = -1;
    int value = atomic_load_explicit(&cached, memory_order_relaxed);
    if (value >= 0) return value;
    const char* env = getenv("CC_CHAN_TRACE_REQ_WAKE");
    int enabled = (env && env[0] == '1') ? 1 : 0;
    int expected = -1;
    (void)atomic_compare_exchange_strong_explicit(&cached, &expected, enabled,
                                                  memory_order_relaxed,
                                                  memory_order_relaxed);
    return atomic_load_explicit(&cached, memory_order_relaxed);
}

int cc__chan_debug_req_wake_match(void* ch_obj);

static inline void cc__chan_trace_recv_empty(CCChan* ch,
                                             const char* event,
                                             cc__fiber_wait_node* node,
                                             int notify) {
    if (!cc__chan_recv_empty_trace_enabled()) return;
    if (!cc__chan_recv_empty_trace_match_obj(ch)) return;
    fprintf(stderr,
            "CC_CHAN_TRACE: %s ch=%p node=%p fiber=%p ticket=%" PRIu64
            " notify=%s(%d) in_list=%d\n",
            event,
            (void*)ch,
            (void*)node,
            node ? (void*)node->fiber : NULL,
            node ? (uint64_t)node->wait_ticket : 0,
            cc__chan_notify_name(notify),
            notify,
            node ? node->in_wait_list : 0);
}

static inline void cc__chan_trace_req_wake(CCChan* ch,
                                           const char* event,
                                           const void* item,
                                           cc__fiber_wait_node* node);

static inline size_t cc__chan_waiter_list_len(cc__fiber_wait_node* node) {
    size_t n = 0;
    while (node) {
        n++;
        node = node->next;
    }
    return n;
}

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

static void cc__chan_wait_flag_park(void* waitable, CCSchedFiber* fiber, void* io) {
    (void)fiber;
    (void)io;
    cc__chan_wait_flag_ctx* ctx = (cc__chan_wait_flag_ctx*)waitable;
    if (ctx->obj && ctx->reason && strcmp(ctx->reason, "chan_recv_wait_empty") == 0) {
        cc__chan_trace_recv_empty((CCChan*)ctx->obj, "recv_park", ctx->node,
                                  atomic_load_explicit(ctx->flag, memory_order_relaxed));
    }
    cc__fiber_set_park_obj(ctx->obj);
    CC_FIBER_PARK_IF(ctx->flag, ctx->expected,
                     ctx->reason ? ctx->reason : "chan_wait_notified");
}

static cc_sched_wait_result cc__chan_wait_flag_park_until(void* waitable, CCSchedFiber* fiber, void* io,
                                                          const struct timespec* abs_deadline) {
    (void)fiber;
    (void)io;
    cc__chan_wait_flag_ctx* ctx = (cc__chan_wait_flag_ctx*)waitable;
    if (ctx->obj && ctx->reason && strcmp(ctx->reason, "chan_recv_wait_empty") == 0) {
        cc__chan_trace_recv_empty((CCChan*)ctx->obj, "recv_park_until", ctx->node,
                                  atomic_load_explicit(ctx->flag, memory_order_relaxed));
    }
    cc__fiber_set_park_obj(ctx->obj);
    return CC_FIBER_PARK_IF_UNTIL(ctx->flag, ctx->expected, abs_deadline,
                                  ctx->reason ? ctx->reason : "chan_wait_notified")
               ? CC_SCHED_WAIT_TIMEOUT
               : CC_SCHED_WAIT_PARKED;
}

static inline cc_sched_wait_result cc__chan_wait_notified_deadline(cc__fiber_wait_node* node,
                                                                   const struct timespec* abs_deadline,
                                                                   const char* reason,
                                                                   void* obj) {
    if (atomic_load_explicit(&node->notified, memory_order_acquire) != 0) {
        return CC_SCHED_WAIT_OK;
    }
    cc__chan_wait_flag_ctx ctx = {
        .flag = &node->notified,
        .expected = 0,
        .reason = reason,
        .obj = obj,
        .node = node,
    };
    const cc_sched_waitable_ops ops = {
        .try_complete = cc__chan_wait_flag_try_complete,
        .publish = cc__chan_wait_flag_publish,
        .unpublish = cc__chan_wait_flag_unpublish,
        .park = cc__chan_wait_flag_park,
        .park_until = cc__chan_wait_flag_park_until,
    };
    cc_sched_wait_result rc = abs_deadline
                                  ? cc_sched_fiber_wait_until(&ctx, NULL, &ops, abs_deadline)
                                  : cc_sched_fiber_wait(&ctx, NULL, &ops);
    return rc;
}

static inline cc_sched_wait_result cc__chan_wait_notified(cc__fiber_wait_node* node, const char* reason, void* obj) {
    return cc__chan_wait_notified_deadline(node, NULL, reason, obj);
}

static inline cc_sched_wait_result cc__chan_wait_notified_mark_close(cc__fiber_wait_node* node,
                                                                     const struct timespec* abs_deadline,
                                                                     const char* reason,
                                                                     void* obj) {
    cc_sched_wait_result wait_rc = cc__chan_wait_notified_deadline(node, abs_deadline, reason, obj);
    if (wait_rc == CC_SCHED_WAIT_CLOSED) {
        atomic_store_explicit(&node->notified, CC_CHAN_NOTIFY_CLOSE, memory_order_release);
    }
    return wait_rc;
}

static _Atomic int g_chan_minimal_path_mode = -1; /* -1 unknown, 0 off, 1 on */
static _Atomic int g_chan_mutex_minimal_mode = -1; /* -1 unknown, 0 off, 1 on */
static _Atomic int g_chan_recv_wake_target = -1; /* -1 unknown, otherwise bounded wake target */

static inline int cc__chan_minimal_path_enabled(void) {
    int cached = atomic_load_explicit(&g_chan_minimal_path_mode, memory_order_relaxed);
    if (cached < 0) {
        /* Default ON: minimal fast path removes substantial overhead from
         * lock-free buffered steady-state traffic. Set to 0 to opt out. */
        const char* env = getenv("CC_CHAN_MINIMAL_FAST_PATH");
        int enabled = !(env && env[0] == '0');
        int expected = -1;
        (void)atomic_compare_exchange_strong_explicit(&g_chan_minimal_path_mode,
                                                      &expected,
                                                      enabled,
                                                      memory_order_relaxed,
                                                      memory_order_relaxed);
        cached = atomic_load_explicit(&g_chan_minimal_path_mode, memory_order_relaxed);
    }
    return cached;
}

static inline int cc__chan_mutex_minimal_enabled(void) {
    int cached = atomic_load_explicit(&g_chan_mutex_minimal_mode, memory_order_relaxed);
    if (cached < 0) {
        const char* env = getenv("CC_CHAN_MUTEX_MINIMAL");
        int enabled = (env && env[0] == '1');
        int expected = -1;
        (void)atomic_compare_exchange_strong_explicit(&g_chan_mutex_minimal_mode,
                                                      &expected,
                                                      enabled,
                                                      memory_order_relaxed,
                                                      memory_order_relaxed);
        cached = atomic_load_explicit(&g_chan_mutex_minimal_mode, memory_order_relaxed);
    }
    return cached;
}

static inline int cc__chan_recv_wake_target(void) {
    int cached = atomic_load_explicit(&g_chan_recv_wake_target, memory_order_relaxed);
    if (cached < 0) {
        /* Default to 1: once a receiver is already on the way, let it drain
         * the hot queue before waking more. Raise with env for tuning. */
        const char* env = getenv("CC_CHAN_RECV_WAKE_TARGET");
        int target = 1;
        if (env && env[0]) {
            int parsed = atoi(env);
            if (parsed > 0) target = parsed;
        }
        int expected = -1;
        (void)atomic_compare_exchange_strong_explicit(&g_chan_recv_wake_target,
                                                      &expected,
                                                      target,
                                                      memory_order_relaxed,
                                                      memory_order_relaxed);
        cached = atomic_load_explicit(&g_chan_recv_wake_target, memory_order_relaxed);
    }
    return cached;
}


static inline int cc__chan_lf_count(CCChan* ch);

/* ============================================================================
 * Fiber Wait Queue Helpers - moved after struct definition
 * ============================================================================ */

CCDeadline* cc_current_deadline(void) {
    return cc__tls_current_deadline;
}

CCDeadline* cc_deadline_push(CCDeadline* d) {
    CCDeadline* prev = cc__tls_current_deadline;
    cc__tls_current_deadline = d;
    return prev;
}

void cc_deadline_pop(CCDeadline* prev) {
    cc__tls_current_deadline = prev;
}

void cc_cancel_current(void) {
    if (cc__tls_current_deadline) cc__tls_current_deadline->cancelled = 1;
}

bool cc_is_cancelled_current(void) {
    return cc__tls_current_deadline && cc__tls_current_deadline->cancelled;
}

/* Forward declarations */
static CCChan* cc_chan_create_internal(size_t capacity, CCChanMode mode, bool allow_take, bool is_sync, CCChanTopology topology);
static void cc__chan_broadcast_activity(void);
static inline int cc__queue_dequeue_raw(CCChan* ch, void** out_val);
static inline int cc__queue_enqueue_value(CCChan* ch, const void* value);
static inline int cc__queue_dequeue_value(CCChan* ch, void* out_value);
static void cc__chan_signal_activity(CCChan* ch);
static void cc__chan_signal_recv_ready(CCChan* ch);

/* Global broadcast condvar for multi-channel select (@match).
   Simple approach: any channel activity signals this global condvar.
   Waiters in @match wait on this. Spurious wakeups are handled by retrying. */
static pthread_mutex_t g_chan_broadcast_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_chan_broadcast_cv = PTHREAD_COND_INITIALIZER;
static _Atomic int g_select_waiters = 0;  /* Count of threads waiting in select */

/* ========\====================================================================
 * Channel Close/Wait Invariants (Debug Aid)
 *
 * 1) Close stops admission: send must not accept new work after close.
 * 2) Recv drains in-flight work: recv may return close only when no buffered
 *    items and no in-flight enqueue remain (lock-free path).
 * 3) Select: only one winner; non-winners must cancel and rearm.
 * ============================================================================ */

typedef struct cc__select_wait_group {
    cc__fiber* fiber;
    _Atomic int signaled;
    _Atomic int selected_index;
} cc__select_wait_group;


static inline int cc__chan_waiter_ticket_valid(cc__fiber_wait_node* node) {
    if (!node) return 0;
    /* Non-fiber waiters (pthread/condvar paths) do not participate in fiber-frame
     * reuse and therefore have no ABA ticket contract to validate. */
    if (!node->fiber) return 1;
    if (node->wait_ticket == 0) return 1; /* Legacy/unpublished node */
    return cc__fiber_wait_ticket_matches(node->fiber, node->wait_ticket);
}

static inline int cc__chan_waiter_ticket_valid_dbg(cc__fiber_wait_node* node,
                                                    const char* where) {
    (void)where;
    if (cc__chan_waiter_ticket_valid(node)) return 1;
    return 0;
}


struct CCChan {
    size_t cap;
    size_t count;              /* Only used for unbuffered (cap==0) and mutex fallback */
    size_t head;               /* Only used for unbuffered (cap==0) and mutex fallback */
    size_t tail;               /* Only used for unbuffered (cap==0) and mutex fallback */
    void *buf;                 /* Data buffer: ring buffer for mutex path, slot array for lock-free */
    size_t elem_size;
    int closed;
    int fast_path_ok;          /* Brand: 1 = minimal fast path eligible (lockfree, small elem, not owned) */
    int tx_error_code;         /* Error code when tx closed with error (downstream propagation) */
    int rx_error_closed;       /* Flag: rx side was error-closed */
    int rx_error_code;         /* Error code from rx side (upstream propagation to senders) */
    /* Structured CCIoError preserved across close-with-error / cancel.
     * When set, the typed-result helper cc_chan_result_with(ch, err, is_recv)
     * returns Err(tx_io_error|rx_io_error) verbatim instead of mapping via
     * cc_io_from_errno. Both kind and os_code survive the channel handoff. */
    CCIoError tx_io_error;
    CCIoError rx_io_error;
    uint8_t tx_io_error_set;
    uint8_t rx_io_error_set;
    CCChanMode mode;
    int allow_take;
    int is_sync;               /* 1 = sync (blocks OS thread), 0 = async (cooperative) */
    CCChanTopology topology;
    /* Rendezvous (unbuffered) support: cap==0 */
    int rv_has_value;
    int rv_recv_waiters;
    
    /* Ordered channel (task channel) support */
    int is_ordered;                                  /* 1 = ordered (recv awaits CCTask, extracts result) */
    
    /* Owned channel (pool) support */
    int is_owned;                                    /* 1 = owned channel (pool semantics) */
    CCClosure0 on_create;                           /* Called when pool empty and recv called */
    CCClosure1 on_destroy;                          /* Called for each item on channel free */
    CCClosure1 on_reset;                            /* Called on item when returned via send */
    size_t items_created;                           /* Number of items created by on_create */
    size_t max_items;                               /* Maximum items pool can create */
    
    /* Debug/guard: if set, this channel is auto-closed by this nursery on scope exit. */
    CCNursery* autoclose_owner;
    int warned_autoclose_block;
    pthread_mutex_t mu;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
    /* Fiber wait queues for fiber-aware blocking */
    cc__fiber_wait_node* send_waiters_head;
    cc__fiber_wait_node* send_waiters_tail;
    cc__fiber_wait_node* recv_waiters_head;
    cc__fiber_wait_node* recv_waiters_tail;
    cc__fiber_wait_node* recv_signal_hint;          /* Next recv waiter to scan when signaling. */
    /* has_send_waiters stays boolean. has_recv_waiters is now a count of
     * linked recv waiters with notified==NONE, so senders can avoid both the
     * O(N) refresh walk and signal-all thundering herds. */
    _Atomic int has_send_waiters;
    _Atomic int has_recv_waiters;
    _Atomic int recv_wake_inflight;                 /* Signaled recv waiters not yet consumed. */
    _Atomic int thread_recv_waiters;                /* Non-fiber lock-free recv waiters on not_empty. */
    
    /* Lock-free MPMC queue for buffered channels (cap > 0) */
    int use_lockfree;                               /* 1 = use lock-free queue, 0 = use mutex */
    int use_ring_queue;                             /* 1 = use internal ring queue backend */
    size_t lfqueue_cap;                             /* Actual capacity (rounded up to power of 2) */
    struct lfds711_queue_bmm_state lfqueue_state;   /* liblfds queue state */
    struct lfds711_queue_bmm_element *lfqueue_elements; /* Pre-allocated element array */
    cc__ring_cell* ring_cells;                      /* Internal ring queue storage */
    _Atomic size_t ring_head __attribute__((aligned(128)));
    _Atomic size_t ring_tail __attribute__((aligned(128)));
    _Atomic int lfqueue_count __attribute__((aligned(128))); /* Approximate count for fast full/empty check */
    _Atomic int lfqueue_inflight;                   /* Active lock-free enqueue attempts */
    _Atomic size_t slot_counter;                    /* Per-channel slot counter for large elements */
    CCSocketSignal* recv_signal;                    /* If set, signaled when recv may make progress */


};

static inline uint32_t cc__chan_wake_sched_attrib(CCChan* ch) {
    if (ch && ch->fast_path_ok) {
        return CC_FIBER_UNPARK_ATTR_CONTENTION_LOCAL;
    }
    return CC_FIBER_UNPARK_ATTR_NONE;
}

static inline void cc__chan_trace_flow(CCChan* ch,
                                       const char* event,
                                       const void* item,
                                       int rc) {
    if (!cc__chan_flow_trace_enabled()) return;
    if (!cc__chan_recv_empty_trace_match_obj(ch)) return;
    int lf_count = ch->use_lockfree
        ? atomic_load_explicit(&ch->lfqueue_count, memory_order_relaxed)
        : (int)ch->count;
    int inflight = atomic_load_explicit(&ch->lfqueue_inflight, memory_order_relaxed);
    int has_recv_waiters = atomic_load_explicit(&ch->has_recv_waiters, memory_order_relaxed);
    int has_send_waiters = atomic_load_explicit(&ch->has_send_waiters, memory_order_relaxed);
    fprintf(stderr,
            "CC_CHAN_FLOW: %s ch=%p item=%p rc=%d cap=%zu count=%zu lf_count=%d inflight=%d closed=%d recv_waiters=%d send_waiters=%d\n",
            event,
            (void*)ch,
            cc__chan_trace_item_ptr(item, ch->elem_size),
            rc,
            ch->cap,
            ch->count,
            lf_count,
            inflight,
            ch->closed,
            has_recv_waiters,
            has_send_waiters);
}

int cc__chan_debug_req_wake_match(void* ch_obj) {
    CCChan* ch = (CCChan*)ch_obj;
    if (!cc__chan_req_wake_trace_enabled()) return 0;
    if (!ch) return 0;
    if (!cc__chan_recv_empty_trace_match_obj(ch)) return 0;
    return ch->cap > 1;
}

int cc__chan_debug_is_open(void* ch_obj) {
    CCChan* ch = (CCChan*)ch_obj;
    if (!ch) return 0;
    pthread_mutex_lock(&ch->mu);
    int is_open = !ch->closed;
    pthread_mutex_unlock(&ch->mu);
    return is_open;
}

static inline void cc__chan_trace_req_wake(CCChan* ch,
                                           const char* event,
                                           const void* item,
                                           cc__fiber_wait_node* node) {
    if (!cc__chan_debug_req_wake_match(ch)) return;
    int notify = node ? atomic_load_explicit(&node->notified, memory_order_relaxed) : -1;
    int lf_count = ch->use_lockfree
        ? atomic_load_explicit(&ch->lfqueue_count, memory_order_relaxed)
        : (int)ch->count;
    int has_recv_waiters = atomic_load_explicit(&ch->has_recv_waiters, memory_order_relaxed);
    fprintf(stderr,
            "CC_REQ_WAKE: %s ch=%p item=%p node=%p fiber=%p ticket=%" PRIu64
            " notify=%s(%d) cap=%zu count=%zu lf_count=%d closed=%d has_recv_waiters=%d recv_head=%p\n",
            event,
            (void*)ch,
            cc__chan_trace_item_ptr(item, ch->elem_size),
            (void*)node,
            node ? (void*)node->fiber : NULL,
            node ? (uint64_t)node->wait_ticket : 0,
            cc__chan_notify_name(notify),
            notify,
            ch->cap,
            ch->count,
            lf_count,
            ch->closed,
            has_recv_waiters,
            (void*)ch->recv_waiters_head);
}

static inline void cc__chan_trace_req_recv(CCChan* ch,
                                           const char* event,
                                           cc__fiber_wait_node* node,
                                           int notify,
                                           int rc) {
    if (!cc__chan_debug_req_wake_match(ch)) return;
    int lf_count = ch->use_lockfree
        ? atomic_load_explicit(&ch->lfqueue_count, memory_order_relaxed)
        : (int)ch->count;
    int has_recv_waiters = atomic_load_explicit(&ch->has_recv_waiters, memory_order_relaxed);
    fprintf(stderr,
            "CC_REQ_RECV: %s ch=%p node=%p fiber=%p ticket=%" PRIu64
            " notify=%s(%d) rc=%d cap=%zu count=%zu lf_count=%d closed=%d has_recv_waiters=%d recv_head=%p\n",
            event,
            (void*)ch,
            (void*)node,
            node ? (void*)node->fiber : NULL,
            node ? (uint64_t)node->wait_ticket : 0,
            cc__chan_notify_name(notify),
            notify,
            rc,
            ch->cap,
            ch->count,
            lf_count,
            ch->closed,
            has_recv_waiters,
            (void*)ch->recv_waiters_head);
}

static inline void cc__chan_trace_close(CCChan* ch,
                                        const char* event,
                                        cc__fiber_wait_node* node,
                                        int notify) {
    if (!cc__chan_close_trace_enabled()) return;
    if (!cc__chan_recv_empty_trace_match_obj(ch)) return;
    int lf_count = ch->use_lockfree
        ? atomic_load_explicit(&ch->lfqueue_count, memory_order_relaxed)
        : (int)ch->count;
    fprintf(stderr,
            "CC_CHAN_CLOSE: %s ch=%p node=%p fiber=%p ticket=%" PRIu64
            " notify=%s(%d) closed=%d rx_err_closed=%d cap=%zu count=%zu lf_count=%d rv_has=%d rv_recv=%d recv_q=%zu send_q=%zu\n",
            event,
            (void*)ch,
            (void*)node,
            node ? (void*)node->fiber : NULL,
            node ? (uint64_t)node->wait_ticket : 0,
            cc__chan_notify_name(notify),
            notify,
            ch ? ch->closed : 0,
            ch ? ch->rx_error_closed : 0,
            ch ? ch->cap : 0,
            ch ? ch->count : 0,
            lf_count,
            ch ? ch->rv_has_value : 0,
            ch ? ch->rv_recv_waiters : 0,
            ch ? cc__chan_waiter_list_len(ch->recv_waiters_head) : 0,
            ch ? cc__chan_waiter_list_len(ch->send_waiters_head) : 0);
}

void cc__chan_debug_dump_state(void* ch_obj, const char* prefix) {
    CCChan* ch = (CCChan*)ch_obj;
    if (!ch) return;
    int lf_count = ch->use_lockfree
        ? atomic_load_explicit(&ch->lfqueue_count, memory_order_relaxed)
        : (int)ch->count;
    int inflight = atomic_load_explicit(&ch->lfqueue_inflight, memory_order_relaxed);
    int has_recv_waiters = atomic_load_explicit(&ch->has_recv_waiters, memory_order_relaxed);
    int has_send_waiters = atomic_load_explicit(&ch->has_send_waiters, memory_order_relaxed);
    fprintf(stderr,
            "%sch=%p cap=%zu count=%zu lf_count=%d inflight=%d closed=%d rx_err_closed=%d rv_has_value=%d rv_recv_waiters=%d has_recv_waiters=%d has_send_waiters=%d recv_head=%p send_head=%p\n",
            prefix ? prefix : "",
            (void*)ch,
            ch->cap,
            ch->count,
            lf_count,
            inflight,
            ch->closed,
            ch->rx_error_closed,
            ch->rv_has_value,
            ch->rv_recv_waiters,
            has_recv_waiters,
            has_send_waiters,
            (void*)ch->recv_waiters_head,
            (void*)ch->send_waiters_head);
}


static inline void cc_chan_lock(CCChan* ch) { pthread_mutex_lock(&ch->mu); }
static inline void cc_chan_unlock(CCChan* ch) { pthread_mutex_unlock(&ch->mu); }

int cc_chan_pair_create(size_t capacity,
                        CCChanMode mode,
                        bool allow_send_take,
                        size_t elem_size,
                        CCChanTx* out_tx,
                        CCChanRx* out_rx) {
    return cc_chan_pair_create_ex(capacity, mode, allow_send_take, elem_size, false, out_tx, out_rx);
}

int cc_chan_pair_create_ex(size_t capacity,
                           CCChanMode mode,
                           bool allow_send_take,
                           size_t elem_size,
                           bool is_sync,
                           CCChanTx* out_tx,
                           CCChanRx* out_rx) {
    return cc_chan_pair_create_full(capacity, mode, allow_send_take, elem_size, is_sync, CC_CHAN_TOPO_DEFAULT, out_tx, out_rx);
}

int cc_chan_pair_create_full(size_t capacity,
                             CCChanMode mode,
                             bool allow_send_take,
                             size_t elem_size,
                             bool is_sync,
                             int topology,
                             CCChanTx* out_tx,
                             CCChanRx* out_rx) {
    if (!out_tx || !out_rx) return EINVAL;
    out_tx->raw = NULL;
    out_rx->raw = NULL;
    CCChanTopology topo = (CCChanTopology)topology;
    CCChan* ch = cc_chan_create_internal(capacity, mode, allow_send_take, is_sync, topo);
    if (!ch) return ENOMEM;
    if (elem_size != 0) {
        int e = cc_chan_init_elem(ch, elem_size);
        if (e != 0) { cc_chan_free(ch); return e; }
    }
    out_tx->raw = ch;
    out_rx->raw = ch;
    return 0;
}

/* Returns CCChan* for assignment; returns NULL on error */
CCChan* cc_chan_pair_create_returning(size_t capacity,
                                      CCChanMode mode,
                                      bool allow_send_take,
                                      size_t elem_size,
                                      bool is_sync,
                                      int topology,
                                      bool is_ordered,
                                      CCChanTx* out_tx,
                                      CCChanRx* out_rx) {
    if (!out_tx || !out_rx) return NULL;
    out_tx->raw = NULL;
    out_rx->raw = NULL;
    CCChanTopology topo = (CCChanTopology)topology;
    CCChan* ch = cc_chan_create_internal(capacity, mode, allow_send_take, is_sync, topo);
    if (!ch) return NULL;
    ch->is_ordered = is_ordered ? 1 : 0;
    if (elem_size != 0) {
        int e = cc_chan_init_elem(ch, elem_size);
        if (e != 0) { cc_chan_free(ch); return NULL; }
    }
    out_tx->raw = ch;
    out_rx->raw = ch;
    return ch;
}

/* ============================================================================
 * Fiber Wait Queue Helpers
 * ============================================================================ */

/*
 * Wait-node lifetime/ABA contract (§8):
 * - Nodes are stack-owned by the waiting fiber/select frame and are only linked
 *   while that frame is alive.
 * - Wake/claim paths must validate node->wait_ticket before touching notified
 *   or enqueueing node->fiber; mismatches are treated as stale and skipped.
 * - Unlinking (in_wait_list=0) happens under ch->mu before a node can be reused.
 */

/* Add a fiber to a waiter queue (must hold ch->mu) */
static void cc__chan_add_waiter(cc__fiber_wait_node** head, cc__fiber_wait_node** tail, cc__fiber_wait_node* node) {
    if (!node) return;
    if (node->fiber && node->wait_ticket == 0) {
        /* Single-waiter ops publish here. Multi-node select publishes one
         * shared ticket in the caller and preloads node->wait_ticket. */
        node->wait_ticket = cc__fiber_publish_wait_ticket(node->fiber);
    }
    node->next = NULL;
    node->prev = *tail;
    if (*tail) {
        (*tail)->next = node;
    } else {
        *head = node;
    }
    *tail = node;
    /* LP (§10 Waiter publish LP): node becomes discoverable to channel wakers. */
    node->in_wait_list = 1;
}

static int cc__chan_select_try_win(cc__fiber_wait_node* node) {
    if (!node->is_select || !node->select_group) return 1;
    return cc__wait_select_try_win(node->select_group, node->select_index);
}

static inline void cc__chan_select_cancel_node(cc__fiber_wait_node* node) {
    if (!node) return;
    if (!cc__chan_waiter_ticket_valid_dbg(node, "select_cancel")) return;
    atomic_store_explicit(&node->notified, CC_CHAN_NOTIFY_CANCEL, memory_order_release);
    if (node->is_select && node->select_group) {
        cc__select_wait_group* group = (cc__select_wait_group*)node->select_group;
        atomic_fetch_add_explicit(&group->signaled, 1, memory_order_release);
    }
    if (node->fiber) {
        wake_batch_add(node->fiber);
    }
}

/* Add a fiber to send waiters queue (must hold ch->mu) */
static void cc__chan_add_send_waiter(CCChan* ch, cc__fiber_wait_node* node) {
    if (!ch || !node) return;
    if (ch->use_lockfree) {
    }
    cc__chan_add_waiter(&ch->send_waiters_head, &ch->send_waiters_tail, node);
    atomic_store_explicit(&ch->has_send_waiters, 1, memory_order_release);
}

/* Add a fiber to recv waiters queue (must hold ch->mu) */
static inline void cc__chan_recv_waiter_count_inc(CCChan* ch) {
    atomic_fetch_add_explicit(&ch->has_recv_waiters, 1, memory_order_release);
}

static inline void cc__chan_recv_waiter_count_dec(CCChan* ch) {
    int old = atomic_fetch_sub_explicit(&ch->has_recv_waiters, 1, memory_order_release);
    if (old <= 1) {
        atomic_store_explicit(&ch->has_recv_waiters, 0, memory_order_release);
    }
}

static inline void cc__chan_recv_wake_inflight_inc(CCChan* ch) {
    atomic_fetch_add_explicit(&ch->recv_wake_inflight, 1, memory_order_release);
}

static inline void cc__chan_recv_wake_inflight_dec(CCChan* ch) {
    int old = atomic_fetch_sub_explicit(&ch->recv_wake_inflight, 1, memory_order_release);
    if (old <= 1) {
        atomic_store_explicit(&ch->recv_wake_inflight, 0, memory_order_release);
    }
}

static inline void cc__chan_thread_recv_waiter_inc(CCChan* ch) {
    atomic_fetch_add_explicit(&ch->thread_recv_waiters, 1, memory_order_release);
}

static inline void cc__chan_thread_recv_waiter_dec(CCChan* ch) {
    int old = atomic_fetch_sub_explicit(&ch->thread_recv_waiters, 1, memory_order_release);
    if (old <= 1) {
        atomic_store_explicit(&ch->thread_recv_waiters, 0, memory_order_release);
    }
}

static inline int cc__chan_recv_wake_budget(CCChan* ch) {
    int target = cc__chan_recv_wake_target();
    int inflight = atomic_load_explicit(&ch->recv_wake_inflight, memory_order_acquire);
    int deficit = target - inflight;
    return deficit > 0 ? deficit : 0;
}

static inline void cc__chan_recv_signal_hint_advance(CCChan* ch, cc__fiber_wait_node* node, cc__fiber_wait_node* next) {
    if (!ch) return;
    if (ch->recv_signal_hint == node) {
        ch->recv_signal_hint = next ? next : ch->recv_waiters_head;
    }
    if (!ch->recv_waiters_head) {
        ch->recv_signal_hint = NULL;
    } else if (!ch->recv_signal_hint) {
        ch->recv_signal_hint = ch->recv_waiters_head;
    }
}

static inline void cc__chan_recv_waiter_rearm(CCChan* ch, cc__fiber_wait_node* node) {
    if (!ch || !node || !node->in_wait_list) return;
    cc__chan_recv_waiter_count_inc(ch);
    if (!ch->recv_signal_hint) {
        ch->recv_signal_hint = node;
    }
}

static inline void cc__chan_recv_waiter_consume_signal(CCChan* ch, cc__fiber_wait_node* node) {
    if (!ch || !node || !node->in_wait_list) return;
    cc__chan_recv_wake_inflight_dec(ch);
}

static void cc__chan_add_recv_waiter(CCChan* ch, cc__fiber_wait_node* node) {
    if (!ch || !node) return;
    if (ch->use_lockfree) {
    }
    cc__chan_add_waiter(&ch->recv_waiters_head, &ch->recv_waiters_tail, node);
    cc__chan_recv_waiter_count_inc(ch);
    if (!ch->recv_signal_hint) {
        ch->recv_signal_hint = node;
    }
    cc__chan_trace_recv_empty(ch, "recv_add", node,
                              atomic_load_explicit(&node->notified, memory_order_relaxed));
}

/* Remove a fiber from a wait queue (must hold ch->mu) */
static void cc__chan_remove_waiter_list(cc__fiber_wait_node** head, cc__fiber_wait_node** tail, cc__fiber_wait_node* node) {
    if (!node) return;
    if (node->prev) {
        node->prev->next = node->next;
    } else {
        *head = node->next;
    }
    if (node->next) {
        node->next->prev = node->prev;
    } else {
        *tail = node->prev;
    }
    node->prev = node->next = NULL;
    node->in_wait_list = 0;
}

/* has_*_waiters should mean "there exists an unnotified waiter worth waking",
 * not merely "some waiter node still exists in the list". Otherwise a single
 * already-signaled waiter keeps the hot path taking the wake lock on every op
 * until it runs and unlinks itself. */
static int cc__chan_has_wakeable_waiter(cc__fiber_wait_node* head) {
    for (cc__fiber_wait_node* node = head; node; node = node->next) {
        int notify = atomic_load_explicit(&node->notified, memory_order_acquire);
        if (notify == CC_CHAN_NOTIFY_NONE) {
            return 1;
        }
    }
    return 0;
}

static inline void cc__chan_refresh_has_send_waiters(CCChan* ch) {
    atomic_store_explicit(&ch->has_send_waiters,
                          cc__chan_has_wakeable_waiter(ch->send_waiters_head),
                          memory_order_release);
}

static void cc__chan_remove_send_waiter(CCChan* ch, cc__fiber_wait_node* node) {
    if (!ch || !node) return;
    if (!node->in_wait_list) {
        /* Node already removed by wake_one — clear the Dekker flag now
         * that the sender has processed its wake. */
        if (!ch->send_waiters_head)
            atomic_store_explicit(&ch->has_send_waiters, 0, memory_order_release);
        return;
    }
    if (ch->use_lockfree) {
    }
    cc__chan_remove_waiter_list(&ch->send_waiters_head, &ch->send_waiters_tail, node);
    cc__chan_refresh_has_send_waiters(ch);
}

static void cc__chan_remove_recv_waiter(CCChan* ch, cc__fiber_wait_node* node) {
    if (!ch || !node) return;
    if (!node->in_wait_list) {
        /* Node was unlinked by another path (e.g. wake_all_waiters on close).
         * If the list is now empty, clear the counter and hint. */
        if (!ch->recv_waiters_head) {
            atomic_store_explicit(&ch->has_recv_waiters, 0, memory_order_release);
            ch->recv_signal_hint = NULL;
        }
        cc__chan_trace_recv_empty(ch, "recv_remove_unlinked", node,
                                  atomic_load_explicit(&node->notified, memory_order_relaxed));
        return;
    }
    if (ch->use_lockfree) {
    }
    int notify = atomic_load_explicit(&node->notified, memory_order_acquire);
    cc__fiber_wait_node* next = node->next;
    cc__chan_trace_recv_empty(ch, "recv_remove_linked", node,
                              atomic_load_explicit(&node->notified, memory_order_relaxed));
    cc__chan_remove_waiter_list(&ch->recv_waiters_head, &ch->recv_waiters_tail, node);
    if (notify == CC_CHAN_NOTIFY_NONE) {
        cc__chan_recv_waiter_count_dec(ch);
    } else if (notify == CC_CHAN_NOTIFY_SIGNAL) {
        cc__chan_recv_wake_inflight_dec(ch);
    }
    cc__chan_recv_signal_hint_advance(ch, node, next);
}

int cc__chan_publish_recv_wait_select(CCChan* ch,
                                      cc__fiber_wait_node* node,
                                      void* out_value,
                                      uint64_t wait_ticket,
                                      void* select_group,
                                      size_t select_index) {
    if (!ch || !node) return CC__CHAN_WAIT_CLOSE;

    cc__fiber* fiber = cc__fiber_in_context() ? cc__fiber_current() : NULL;
    cc_chan_lock(ch);

    if (ch->closed) {
        int closed = ch->closed;
        pthread_mutex_unlock(&ch->mu);
        return closed ? CC__CHAN_WAIT_CLOSE : CC__CHAN_WAIT_PUBLISHED;
    }

    if (ch->cap == 0) {
        cc__fiber_wait_node* snode = ch->send_waiters_head ? cc__chan_pop_send_waiter(ch) : NULL;
        if (snode) {
            cc__chan_finish_recv_from_send_waiter(ch, snode, out_value);
            cc__chan_signal_recv_ready(ch);
            pthread_mutex_unlock(&ch->mu);
            wake_batch_flush();
            return CC__CHAN_WAIT_DATA;
        }
    } else {
        int ready = ch->use_lockfree ? (cc__chan_lf_count(ch) > 0) : (ch->count > 0);
        if (ready) {
            pthread_mutex_unlock(&ch->mu);
            return CC__CHAN_WAIT_SIGNAL;
        }
    }

    memset(node, 0, sizeof(*node));
    node->fiber = fiber;
    node->wait_ticket = wait_ticket ? wait_ticket : (fiber ? cc__fiber_publish_wait_ticket(fiber) : 0);
    node->data = out_value;
    node->select_group = select_group;
    node->select_index = select_index;
    node->is_select = 1;
    atomic_store_explicit(&node->notified, 0, memory_order_release);
    cc__chan_add_recv_waiter(ch, node);
    cc__chan_signal_activity(ch);
    pthread_mutex_unlock(&ch->mu);
    return CC__CHAN_WAIT_PUBLISHED;
}

int cc__chan_finish_recv_wait_select(CCChan* ch, cc__fiber_wait_node* node) {
    if (!ch || !node) return CC_CHAN_NOTIFY_CLOSE;
    cc_chan_lock(ch);
    int notify = atomic_load_explicit(&node->notified, memory_order_acquire);
    if (notify != CC_CHAN_NOTIFY_DATA) {
        cc__chan_remove_recv_waiter(ch, node);
    }
    pthread_mutex_unlock(&ch->mu);
    return notify;
}

/* Wake one send waiter (must hold ch->mu) - uses batch */
static void cc__chan_wake_one_send_waiter(CCChan* ch) {
    if (!ch || !ch->send_waiters_head) return;
    while (ch->send_waiters_head) {
        cc__fiber_wait_node* node = ch->send_waiters_head;
        ch->send_waiters_head = node->next;
        if (ch->send_waiters_head) {
            ch->send_waiters_head->prev = NULL;
        } else {
            ch->send_waiters_tail = NULL;
        }
        node->next = node->prev = NULL;
        node->in_wait_list = 0;
        if (!cc__chan_select_try_win(node)) {
            if (ch->use_lockfree) {
            }
            cc__chan_select_cancel_node(node);
            continue;
        }
        if (!cc__chan_waiter_ticket_valid_dbg(node, "wake_one_send")) {
            continue;
        }
        atomic_store_explicit(&node->notified, CC_CHAN_NOTIFY_SIGNAL, memory_order_release);
        if (ch->use_lockfree) {
        }
        if (node->is_select && node->select_group) {
            cc__select_wait_group* group = (cc__select_wait_group*)node->select_group;
            atomic_fetch_add_explicit(&group->signaled, 1, memory_order_release);
        }
        if (ch->use_lockfree) {
        }
        cc__chan_refresh_has_send_waiters(ch);
        wake_batch_add_chan(ch, node->fiber);
        return;
    }
    /* All nodes were cancelled selects — list is now empty, clear flag */
    cc__chan_refresh_has_send_waiters(ch);
}

/* Signal a recv waiter to wake and try the buffer (must hold ch->mu).
 * The waiter remains in the queue until it runs and removes itself, so we must
 * only signal nodes that are still genuinely waiting (notified == NONE).
 * Re-signaling an already-signaled node creates wake storms without making
 * forward progress. Uses simple FIFO - work stealing provides natural load
 * balancing. */
static int cc__chan_signal_recv_waiters(CCChan* ch, int max_wake) {
    if (!ch || max_wake <= 0) return 0;
    if (!ch->recv_waiters_head) return 0;
    cc__fiber_wait_node* start = ch->recv_signal_hint ? ch->recv_signal_hint : ch->recv_waiters_head;
    if (!start) start = ch->recv_waiters_head;
    int signaled = 0;
    for (int pass = 0; pass < 2 && signaled < max_wake; ++pass) {
        cc__fiber_wait_node* stop = (pass == 0) ? NULL : start;
        cc__fiber_wait_node* node = (pass == 0) ? start : ch->recv_waiters_head;
        for (; node && node != stop; node = node->next) {
            cc__fiber_wait_node* next = node->next;
            int notify = atomic_load_explicit(&node->notified, memory_order_acquire);
            if (cc_channel_recv_dedupe_enabled() && notify != CC_CHAN_NOTIFY_NONE) {
                continue;
            }
            if (!cc__chan_select_try_win(node)) {
                cc__chan_select_cancel_node(node);
                cc__chan_recv_waiter_count_dec(ch);
                continue;
            }
            if (!cc__chan_waiter_ticket_valid_dbg(node, "signal_recv")) {
                continue;
            }
            cc__chan_trace_req_wake(ch, "signal_recv_before", NULL, node);
            cc__chan_trace_recv_empty(ch, "recv_signal_before", node, notify);
            atomic_store_explicit(&node->notified, CC_CHAN_NOTIFY_SIGNAL, memory_order_release);
            cc__chan_recv_waiter_count_dec(ch);
            cc__chan_recv_wake_inflight_inc(ch);
            if (node->is_select && node->select_group) {
                cc__select_wait_group* group = (cc__select_wait_group*)node->select_group;
                atomic_fetch_add_explicit(&group->signaled, 1, memory_order_release);
            }
            cc__chan_trace_req_wake(ch, "signal_recv_after", NULL, node);
            cc__chan_trace_recv_empty(ch, "recv_signal_after", node,
                                      atomic_load_explicit(&node->notified, memory_order_relaxed));
            wake_batch_add_chan(ch, node->fiber);
            cc__chan_trace_req_wake(ch, "signal_recv_wake_batch_add", NULL, node);
            cc__chan_trace_recv_empty(ch, "recv_wake_batch_add", node,
                                      atomic_load_explicit(&node->notified, memory_order_relaxed));
            signaled++;
            if (signaled >= max_wake) {
                ch->recv_signal_hint = next ? next : ch->recv_waiters_head;
                return signaled;
            }
        }
        if (start == ch->recv_waiters_head) {
            break;
        }
    }
    ch->recv_signal_hint = ch->recv_waiters_head;
    return signaled;
}

static void cc__chan_signal_recv_waiter(CCChan* ch) {
    (void)cc__chan_signal_recv_waiters(ch, 1);
}

/* Pop a send waiter (must hold ch->mu). */
static cc__fiber_wait_node* cc__chan_pop_send_waiter(CCChan* ch) {
    if (!ch) return NULL;
    while (ch->send_waiters_head) {
        cc__fiber_wait_node* node = ch->send_waiters_head;
        int notify = atomic_load_explicit(&node->notified, memory_order_acquire);
        /* Skip nodes that are already notified. Only pop nodes with
         * notified=NONE (0) that are truly waiting for space. */
        if (notify != CC_CHAN_NOTIFY_NONE) {
            ch->send_waiters_head = node->next;
            if (ch->send_waiters_head) {
                ch->send_waiters_head->prev = NULL;
            } else {
                ch->send_waiters_tail = NULL;
            }
            node->next = node->prev = NULL;
            node->in_wait_list = 0;
            continue;
        }
        if (node->is_select && !cc__chan_select_try_win(node)) {
            ch->send_waiters_head = node->next;
            if (ch->send_waiters_head) {
                ch->send_waiters_head->prev = NULL;
            } else {
                ch->send_waiters_tail = NULL;
            }
            node->next = node->prev = NULL;
            node->in_wait_list = 0;
            cc__chan_select_cancel_node(node);
            continue;
        }
        if (!cc__chan_waiter_ticket_valid_dbg(node, "pop_send")) {
            ch->send_waiters_head = node->next;
            if (ch->send_waiters_head) {
                ch->send_waiters_head->prev = NULL;
            } else {
                ch->send_waiters_tail = NULL;
            }
            node->next = node->prev = NULL;
            node->in_wait_list = 0;
            continue;
        }
        ch->send_waiters_head = node->next;
        if (ch->send_waiters_head) {
            ch->send_waiters_head->prev = NULL;
        } else {
            ch->send_waiters_tail = NULL;
        }
        node->next = node->prev = NULL;
        node->in_wait_list = 0;
        return node;
    }
    return NULL;
}

/* Pop a recv waiter (must hold ch->mu). */
static cc__fiber_wait_node* cc__chan_pop_recv_waiter(CCChan* ch) {
    if (!ch) return NULL;
    while (ch->recv_waiters_head) {
        cc__fiber_wait_node* node = ch->recv_waiters_head;
        int notify = atomic_load_explicit(&node->notified, memory_order_acquire);
        /* Skip nodes that are already notified (CANCEL, CLOSE, DATA) or
         * were signaled to try the buffer (SIGNAL). Only pop nodes with
         * notified=NONE (0) that are truly waiting for data. */
        if (notify != CC_CHAN_NOTIFY_NONE) {
            cc__fiber_wait_node* next = node->next;
            ch->recv_waiters_head = node->next;
            if (ch->recv_waiters_head) {
                ch->recv_waiters_head->prev = NULL;
            } else {
                ch->recv_waiters_tail = NULL;
            }
            node->next = node->prev = NULL;
            node->in_wait_list = 0;
            if (notify == CC_CHAN_NOTIFY_SIGNAL) {
                cc__chan_recv_wake_inflight_dec(ch);
            }
            cc__chan_recv_signal_hint_advance(ch, node, next);
            continue;
        }
        if (node->is_select && !cc__chan_select_try_win(node)) {
            cc__fiber_wait_node* next = node->next;
            ch->recv_waiters_head = node->next;
            if (ch->recv_waiters_head) {
                ch->recv_waiters_head->prev = NULL;
            } else {
                ch->recv_waiters_tail = NULL;
            }
            node->next = node->prev = NULL;
            node->in_wait_list = 0;
            cc__chan_select_cancel_node(node);
            cc__chan_recv_waiter_count_dec(ch);
            cc__chan_recv_signal_hint_advance(ch, node, next);
            continue;
        }
        if (!cc__chan_waiter_ticket_valid_dbg(node, "pop_recv")) {
            cc__fiber_wait_node* next = node->next;
            ch->recv_waiters_head = node->next;
            if (ch->recv_waiters_head) {
                ch->recv_waiters_head->prev = NULL;
            } else {
                ch->recv_waiters_tail = NULL;
            }
            node->next = node->prev = NULL;
            node->in_wait_list = 0;
            cc__chan_recv_waiter_count_dec(ch);
            cc__chan_recv_signal_hint_advance(ch, node, next);
            continue;
        }
        cc__fiber_wait_node* next = node->next;
        ch->recv_waiters_head = node->next;
        if (ch->recv_waiters_head) {
            ch->recv_waiters_head->prev = NULL;
        } else {
            ch->recv_waiters_tail = NULL;
        }
        node->next = node->prev = NULL;
        node->in_wait_list = 0;
        cc__chan_recv_waiter_count_dec(ch);
        cc__chan_recv_signal_hint_advance(ch, node, next);
        return node;
    }
    return NULL;
}

/* Wake one recv waiter for close (notified=3 means "woken by close") */
static void cc__chan_wake_one_recv_waiter_close(CCChan* ch) {
    if (!ch || !ch->recv_waiters_head) return;
    cc__fiber_wait_node* node = ch->recv_waiters_head;
    int notify = atomic_load_explicit(&node->notified, memory_order_acquire);
    cc__fiber_wait_node* next = node->next;
    cc__chan_trace_close(ch, "wake_one_recv_close_before", node,
                         atomic_load_explicit(&node->notified, memory_order_relaxed));
    ch->recv_waiters_head = node->next;
    if (ch->recv_waiters_head) {
        ch->recv_waiters_head->prev = NULL;
    } else {
        ch->recv_waiters_tail = NULL;
    }
    node->next = node->prev = NULL;
    node->in_wait_list = 0;
    if (notify == CC_CHAN_NOTIFY_NONE) {
        cc__chan_recv_waiter_count_dec(ch);
    } else if (notify == CC_CHAN_NOTIFY_SIGNAL) {
        cc__chan_recv_wake_inflight_dec(ch);
    }
    cc__chan_recv_signal_hint_advance(ch, node, next);
    if (node->is_select && !cc__chan_select_try_win(node)) {
        if (ch->use_lockfree) {
        }
        cc__chan_select_cancel_node(node);
        return;
    }
    if (!cc__chan_waiter_ticket_valid_dbg(node, "wake_close_recv")) {
        return;
    }
    atomic_store_explicit(&node->notified, CC_CHAN_NOTIFY_CLOSE, memory_order_release);  /* close */
    cc__chan_trace_close(ch, "wake_one_recv_close_after", node, CC_CHAN_NOTIFY_CLOSE);
    if (ch->use_lockfree) {
    }
    if (node->is_select && node->select_group) {
        cc__select_wait_group* group = (cc__select_wait_group*)node->select_group;
    atomic_fetch_add_explicit(&group->signaled, 1, memory_order_release);
    }
    wake_batch_add_chan(ch, node->fiber);
}

/* Wake one send waiter for close (notified=3 means "woken by close") */
static void cc__chan_wake_one_send_waiter_close(CCChan* ch) {
    if (!ch || !ch->send_waiters_head) return;
    cc__fiber_wait_node* node = ch->send_waiters_head;
    cc__chan_trace_close(ch, "wake_one_send_close_before", node,
                         atomic_load_explicit(&node->notified, memory_order_relaxed));
    ch->send_waiters_head = node->next;
    if (ch->send_waiters_head) {
        ch->send_waiters_head->prev = NULL;
    } else {
        ch->send_waiters_tail = NULL;
    }
    node->next = node->prev = NULL;
    node->in_wait_list = 0;
    if (node->is_select && !cc__chan_select_try_win(node)) {
        if (ch->use_lockfree) {
        }
        cc__chan_select_cancel_node(node);
        return;
    }
    if (!cc__chan_waiter_ticket_valid_dbg(node, "wake_close_send")) {
        return;
    }
    atomic_store_explicit(&node->notified, CC_CHAN_NOTIFY_CLOSE, memory_order_release);  /* close */
    cc__chan_trace_close(ch, "wake_one_send_close_after", node, CC_CHAN_NOTIFY_CLOSE);
    if (ch->use_lockfree) {
    }
    if (node->is_select && node->select_group) {
        cc__select_wait_group* group = (cc__select_wait_group*)node->select_group;
    atomic_fetch_add_explicit(&group->signaled, 1, memory_order_release);
    }
    wake_batch_add_chan(ch, node->fiber);
}

/* Wake all waiters (for close) - batched, uses notified=3 */
static void cc__chan_wake_all_waiters(CCChan* ch) {
    if (!ch) return;
    /* Wake all send waiters with close signal */
    while (ch->send_waiters_head) {
        cc__chan_wake_one_send_waiter_close(ch);
    }
    /* Wake all recv waiters with close signal */
    while (ch->recv_waiters_head) {
        cc__chan_wake_one_recv_waiter_close(ch);
    }
}

/* Called by nursery.c when registering `closing(ch)` (same TU). */
void cc__chan_set_autoclose_owner(CCChan* ch, CCNursery* owner) {
    if (!ch) return;
    cc_chan_lock(ch);
    if (!ch->autoclose_owner) ch->autoclose_owner = owner;
    ch->warned_autoclose_block = 0;
    pthread_mutex_unlock(&ch->mu);
}

/* Signal the global broadcast condvar for multi-channel select.
   Called when any channel state changes. Simple and deadlock-free.
   Only broadcasts if there are active select waiters (fast path). */
static void cc__chan_broadcast_activity(void) {
    /* Fast path: skip if no select waiters */
    if (atomic_load_explicit(&g_select_waiters, memory_order_relaxed) == 0) {
        return;
    }
    pthread_mutex_lock(&g_chan_broadcast_mu);
    pthread_cond_broadcast(&g_chan_broadcast_cv);
    pthread_mutex_unlock(&g_chan_broadcast_mu);
}

static void cc__chan_signal_activity(CCChan* ch) {
    (void)ch;
    cc__chan_broadcast_activity();
}

static void cc__chan_signal_recv_ready(CCChan* ch) {
    if (ch && ch->recv_signal) {
        cc_socket_signal_signal(ch->recv_signal);
    }
    cc__chan_broadcast_activity();
}

static inline void cc__chan_post_lockfree_enqueue_signal_receivers(CCChan* ch,
                                                                   const void* value,
                                                                   const char* trace_event) {
    /* Keep the lock-free send fast path lockless unless a receiver is actually
     * parked. We previously took ch->mu after every successful enqueue to fix
     * non-fiber recv missed wakes, which fixed cc_block_race/any but caused a
     * major shared-contention regression. thread_recv_waiters lets us preserve
     * the wakeup fix without re-serializing the hot enqueue path. */
    int fiber_waiters = atomic_load_explicit(&ch->has_recv_waiters, memory_order_acquire);
    int thread_waiters = atomic_load_explicit(&ch->thread_recv_waiters, memory_order_acquire);
    if (fiber_waiters == 0 && thread_waiters == 0) {
        /* No fiber/thread receivers parked.  Still need to poke the socket
         * signal if one is registered (used by wait_recv_or_socket consumers
         * like handle_client) so the pipe-based waiter wakes up. */
        if (ch->recv_signal)
            cc_socket_signal_signal(ch->recv_signal);
        return;
    }
    cc_chan_lock(ch);
    if (fiber_waiters || atomic_load_explicit(&ch->has_recv_waiters, memory_order_acquire)) {
        int wake_budget = cc__chan_recv_wake_budget(ch);
        if (wake_budget > 0) {
            if (trace_event && trace_event[0]) {
                cc__chan_trace_req_wake(ch, trace_event, value, NULL);
            }
            (void)cc__chan_signal_recv_waiters(ch, wake_budget);
        }
    }
    if (thread_waiters || atomic_load_explicit(&ch->thread_recv_waiters, memory_order_acquire)) {
        pthread_cond_signal(&ch->not_empty);
    }
    pthread_mutex_unlock(&ch->mu);
    wake_batch_flush();
    cc__chan_signal_recv_ready(ch);
}

/* Wait briefly for any channel activity. Used by async poll loops when
   the inner task is blocked on a channel but the outer state machine
   doesn't have a wait function. Returns after timeout or when any
   channel broadcasts activity. */
void cc_chan_wait_any_activity_timeout(int timeout_us) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_nsec += timeout_us * 1000;
    if (ts.tv_nsec >= 1000000000) {
        ts.tv_sec += ts.tv_nsec / 1000000000;
        ts.tv_nsec %= 1000000000;
    }
    
    atomic_fetch_add_explicit(&g_select_waiters, 1, memory_order_relaxed);
    pthread_mutex_lock(&g_chan_broadcast_mu);
    pthread_cond_timedwait(&g_chan_broadcast_cv, &g_chan_broadcast_mu, &ts);
    pthread_mutex_unlock(&g_chan_broadcast_mu);
    atomic_fetch_sub_explicit(&g_select_waiters, 1, memory_order_relaxed);
}

/* Round up to next power of 2 (required by liblfds) */
static inline size_t next_power_of_2(size_t n) {
    if (n == 0) return 1;
    n--;
    n |= n >> 1;
    n |= n >> 2;
    n |= n >> 4;
    n |= n >> 8;
    n |= n >> 16;
#if SIZE_MAX > 0xFFFFFFFF
    n |= n >> 32;
#endif
    return n + 1;
}

static CCChan* cc_chan_create_internal(size_t capacity, CCChanMode mode, bool allow_take, bool is_sync, CCChanTopology topology) {
    size_t cap = capacity; /* capacity==0 => unbuffered rendezvous */
    CCChan* ch = (CCChan*)malloc(sizeof(CCChan));
    if (!ch) return NULL;
    memset(ch, 0, sizeof(*ch));
    ch->cap = cap;
    ch->elem_size = 0; // set on first send/recv
    ch->buf = NULL;    // lazily allocated when we know elem_size
    ch->mode = mode;
    ch->allow_take = allow_take ? 1 : 0;
    ch->is_sync = is_sync ? 1 : 0;
    ch->topology = topology;
    pthread_mutex_init(&ch->mu, NULL);
    pthread_cond_init(&ch->not_empty, NULL);
    pthread_cond_init(&ch->not_full, NULL);
    
    /* Initialize lock-free queue for buffered channels */
    ch->use_lockfree = 0;
    ch->use_ring_queue = 0;
    ch->lfqueue_cap = 0;
    ch->lfqueue_elements = NULL;
    ch->ring_cells = NULL;
    atomic_store(&ch->ring_head, 0);
    atomic_store(&ch->ring_tail, 0);
    atomic_store(&ch->lfqueue_count, 0);
    atomic_store(&ch->lfqueue_inflight, 0);
    atomic_store(&ch->slot_counter, 0);
    
    if (cap > 1) {  /* Only use lock-free for cap > 1 (liblfds needs at least 2) */
        const char* disable_lf = getenv("CC_CHAN_NO_LOCKFREE");
        if (disable_lf && disable_lf[0] == '1') {
            return ch;  /* Force mutex-based path for debugging */
        }
        /* Buffered channel: allocate lock-free queue */
        size_t lfcap = next_power_of_2(cap);
        ch->lfqueue_cap = lfcap;
        size_t align = 64;
        size_t alloc_size = sizeof(cc__ring_cell) * lfcap;
        alloc_size = ((alloc_size + align - 1) / align) * align;
        ch->ring_cells = (cc__ring_cell*)aligned_alloc(align, alloc_size);
        if (ch->ring_cells) {
            for (size_t i = 0; i < lfcap; i++) {
                atomic_init(&ch->ring_cells[i].seq, i);
                ch->ring_cells[i].value = NULL;
            }
            ch->use_ring_queue = 1;
            ch->use_lockfree = 1;
        } else {
            /* Ring allocation failed: fall back to liblfds. */
            size_t alloc_size_lfds = sizeof(struct lfds711_queue_bmm_element) * lfcap;
            size_t align_lfds = LFDS711_PAL_ATOMIC_ISOLATION_IN_BYTES;
            alloc_size_lfds = ((alloc_size_lfds + align_lfds - 1) / align_lfds) * align_lfds;
            ch->lfqueue_elements = (struct lfds711_queue_bmm_element*)
                aligned_alloc(align_lfds, alloc_size_lfds);
            if (ch->lfqueue_elements) {
                lfds711_queue_bmm_init_valid_on_current_logical_core(
                    &ch->lfqueue_state, ch->lfqueue_elements, lfcap, NULL);
                ch->use_lockfree = 1;
            }
        }
        /* If allocation fails, fall back to mutex-based (use_lockfree remains 0) */
    }
    
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

/* Create an owned channel (resource pool) with lifecycle callbacks.
 * - on_create: CCClosure0 called when recv on empty pool, returns created item (cast to void*)
 * - on_destroy: CCClosure1 called for each item on channel free, arg0 is item pointer
 * - on_reset: CCClosure1 called on item when returned via send, arg0 is item pointer (may be NULL)
 * Returns NULL on error.
 */
CCChan* cc_chan_create_owned(size_t capacity,
                             size_t elem_size,
                             CCClosure0 on_create,
                             CCClosure1 on_destroy,
                             CCClosure1 on_reset) {
    if (capacity == 0) return NULL;  /* Owned channels require capacity > 0 */
    CCChan* ch = cc_chan_create_internal(capacity, CC_CHAN_MODE_BLOCK, false, true, CC_CHAN_TOPO_DEFAULT);
    if (!ch) return NULL;
    
    int err = cc_chan_init_elem(ch, elem_size);
    if (err != 0) {
        cc_chan_free(ch);
        return NULL;
    }
    
    ch->is_owned = 1;
    ch->on_create = on_create;
    ch->on_destroy = on_destroy;
    ch->on_reset = on_reset;
    ch->items_created = 0;
    ch->max_items = capacity;
    
    return ch;
}

/* Convenience: create owned channel and get bidirectional handle.
 * Owned channels are implicitly bidirectional (both send and recv). */
CCChan* cc_chan_create_owned_pool(size_t capacity,
                                  size_t elem_size,
                                  CCClosure0 on_create,
                                  CCClosure1 on_destroy,
                                  CCClosure1 on_reset) {
    return cc_chan_create_owned(capacity, elem_size, on_create, on_destroy, on_reset);
}

int cc_chan_is_ordered(CCChan* ch) {
    return ch ? ch->is_ordered : 0;
}

void cc_chan_set_recv_signal(CCChan* ch, CCSocketSignal* sig) {
    if (!ch) return;
    pthread_mutex_lock(&ch->mu);
    ch->recv_signal = sig;
    pthread_mutex_unlock(&ch->mu);
}

CCResult_CCChanWaitOrSocketKind_CCIoError cc_chan_wait_recv_or_socket(CCChan* ch,
                                                                      CCSocketSignal* sig,
                                                                      void* out_value,
                                                                      size_t value_size) {
    if (!ch || !sig || !out_value || value_size == 0) {
        return cc_err_CCResult_CCChanWaitOrSocketKind_CCIoError(cc_io_from_errno(EINVAL));
    }

    for (;;) {
        int rc = cc_chan_try_recv(ch, out_value, value_size);
        if (rc == 0) {
            return cc_ok_CCResult_CCChanWaitOrSocketKind_CCIoError(CC_CHAN_WAIT_RECV);
        }
        if (rc == EPIPE) {
            return cc_ok_CCResult_CCChanWaitOrSocketKind_CCIoError(CC_CHAN_WAIT_CLOSED);
        }
        if (rc != EAGAIN) {
            return cc_err_CCResult_CCChanWaitOrSocketKind_CCIoError(cc_io_from_errno(rc));
        }

        uint64_t epoch = cc_socket_signal_snapshot(sig);
        rc = cc_chan_try_recv(ch, out_value, value_size);
        if (rc == 0) {
            return cc_ok_CCResult_CCChanWaitOrSocketKind_CCIoError(CC_CHAN_WAIT_RECV);
        }
        if (rc == EPIPE) {
            return cc_ok_CCResult_CCChanWaitOrSocketKind_CCIoError(CC_CHAN_WAIT_CLOSED);
        }
        if (rc != EAGAIN) {
            return cc_err_CCResult_CCChanWaitOrSocketKind_CCIoError(cc_io_from_errno(rc));
        }

        CCResult_bool_CCIoError wait_res = cc_socket_signal_wait_since(sig, epoch);
        if (!wait_res.ok) {
            return cc_err_CCResult_CCChanWaitOrSocketKind_CCIoError(wait_res.u.error);
        }
        if (wait_res.u.value) {
            return cc_ok_CCResult_CCChanWaitOrSocketKind_CCIoError(CC_CHAN_WAIT_SOCKET);
        }
    }
}

/* Unified close primitive shared by cc_chan_close / cc_chan_close_err /
 * cc_chan_rx_close_err / cc_chan_close_with / cc_chan_cancel.
 *
 * Behavior depends on which sides are being closed:
 *   - close_tx: sets ch->closed, wakes ALL fiber waiters (recv + send) via
 *     cc__chan_wake_all_waiters, broadcasts both condvars.
 *   - close_rx (without close_tx): sets ch->rx_error_closed, drains ONLY the
 *     send-waiter list (receivers may still drain queued values), broadcasts
 *     not_full only.
 *   - bilateral (close_tx && close_rx): same as close_tx path; the tx-side
 *     wake already catches both waiter classes. The rx-side bits are stamped
 *     too so senders see the structured error.
 *
 * tx_errno / rx_errno are stored on the respective side when nonzero (used by
 * legacy int-returning paths); io_err is additionally stamped on each side
 * being closed (used by the typed-result promotion in cc_chan_result_with).
 *
 * trace_tag is a short string like "close" / "close_err" / "rx_close_err" /
 * "close_with" - emitted as "<tag>_begin" / "<tag>_after_wake" around the
 * wake, preserving the pre-factoring diagnostic breadcrumbs. */
static void cc__chan_close_common(CCChan* ch,
                                  bool close_tx,
                                  bool close_rx,
                                  int tx_errno,
                                  int rx_errno,
                                  const CCIoError* io_err,
                                  const char* trace_begin,
                                  const char* trace_end) {
    if (!ch) return;
    ch->fast_path_ok = 0;  /* Disable minimal fast path before taking lock */
    pthread_mutex_lock(&ch->mu);
    /* LP (§10 Close LP): OPEN -> CLOSED under channel mutex. */
    if (close_tx) {
        ch->closed = 1;
        if (tx_errno) ch->tx_error_code = tx_errno;
        if (io_err) {
            ch->tx_io_error = *io_err;
            ch->tx_io_error_set = 1;
        }
    }
    if (close_rx) {
        ch->rx_error_closed = 1;
        if (rx_errno) ch->rx_error_code = rx_errno;
        if (io_err) {
            ch->rx_io_error = *io_err;
            ch->rx_io_error_set = 1;
        }
    }
    cc__chan_trace_close(ch, trace_begin, NULL, CC_CHAN_NOTIFY_NONE);
    /* not_empty only matters when receivers need waking (tx side is closing).
     * not_full always matters when any side is closing since senders block
     * on capacity. */
    if (close_tx) pthread_cond_broadcast(&ch->not_empty);
    pthread_cond_broadcast(&ch->not_full);
    if (close_tx) {
        /* Wake everyone: recv waiters are cancelled (channel is closed), and
         * send waiters are already covered by wake_all_waiters. */
        cc__chan_wake_all_waiters(ch);
    } else {
        /* rx-only error close: drain ONLY the send-waiter list. Recv waiters
         * are left alone because the channel is not flagged closed and may
         * still be carrying queued values they should observe. */
        while (ch->send_waiters_head) {
            cc__chan_wake_one_send_waiter_close(ch);
        }
    }
    cc__chan_trace_close(ch, trace_end, NULL, CC_CHAN_NOTIFY_CLOSE);
    pthread_mutex_unlock(&ch->mu);
    wake_batch_flush();  /* Flush fiber wakes immediately */
    cc__chan_signal_recv_ready(ch);
}

void cc_chan_close(CCChan* ch) {
    cc__chan_close_common(ch, /*close_tx=*/true, /*close_rx=*/false,
                          0, 0, NULL,
                          "close_begin", "close_after_wake_all");
}

void cc_chan_close_err(CCChan* ch, int err) {
    cc__chan_close_common(ch, /*close_tx=*/true, /*close_rx=*/false,
                          err, 0, NULL,
                          "close_err_begin", "close_err_after_wake_all");
}

void cc_chan_rx_close_err(CCChan* ch, int err) {
    cc__chan_close_common(ch, /*close_tx=*/false, /*close_rx=*/true,
                          0, err, NULL,
                          "rx_close_err_begin", "rx_close_err_after_wake");
}

/* cc_chan_close_with: bilateral close with a structured CCIoError.
 *
 * Mechanically identical to cc_chan_close (same wake path, same closed flag,
 * same fiber wake-batch flush) but peers observe Err(e) instead of Ok(false).
 * Both the kind AND os_code from `e` are preserved across the channel handoff;
 * the int-returning low-level send/recv still return an errno (e.os_code if
 * nonzero, else ECANCELED) for backward compatibility, while the typed result
 * macros route through cc_chan_result_with() which reads the stored CCIoError
 * verbatim. */
void cc_chan_close_with(CCChan* ch, CCIoError e) {
    if (!ch) return;
    /* Pick a sentinel errno so int-returning paths still signal a non-EOF
     * error. ECANCELED is the closest POSIX match for an application-level
     * cancellation and maps back to CC_IO_CANCELLED in cc_io_from_errno;
     * mismatches on kind are caught by the typed-result layer which returns
     * the stored CCIoError directly. */
    int errno_code = e.os_code ? e.os_code : ECANCELED;
    cc__chan_close_common(ch, /*close_tx=*/true, /*close_rx=*/true,
                          errno_code, errno_code, &e,
                          "close_with_begin", "close_with_after_wake_all");
}

/* cc_chan_cancel: shorthand for cc_chan_close_with(ch, {CC_IO_CANCELLED, 0}).
 * Semantically identical to cc_chan_close() — fully tears down the channel
 * for both senders and receivers — but the signal is an error rather than a
 * graceful EOF. Typical use: a task is giving up and wants peers to propagate
 * the cancellation rather than treat it as normal completion. */
void cc_chan_cancel(CCChan* ch) {
    cc_chan_close_with(ch, cc_io_error(CC_IO_CANCELLED));
}

/* Accessors used by the typed-result helper in cc_channel.cch. Defined here so
 * the struct layout stays private to channel.c. */
CCIoError cc__chan_get_close_error(CCChan* ch, bool is_recv) {
    if (!ch) return cc_io_error(CC_IO_OTHER);
    if (is_recv) {
        return ch->tx_io_error_set ? ch->tx_io_error : cc_io_error(CC_IO_OTHER);
    }
    return ch->rx_io_error_set ? ch->rx_io_error : cc_io_error(CC_IO_OTHER);
}

int cc__chan_has_close_error(CCChan* ch, bool is_recv) {
    if (!ch) return 0;
    return is_recv ? (int)ch->tx_io_error_set : (int)ch->rx_io_error_set;
}

void cc_chan_free(CCChan* ch) {
    if (!ch) return;
    
    
    /* For owned channels, destroy remaining items in the buffer */
    if (ch->is_owned && ch->on_destroy.fn && ch->buf && ch->elem_size > 0) {
        pthread_mutex_lock(&ch->mu);
        if (ch->use_lockfree) {
            unsigned char item_buf[sizeof(intptr_t)] = {0};
            while (cc__queue_dequeue_value(ch, item_buf) == 1) {
                intptr_t item_val = 0;
                memcpy(&item_val, item_buf, ch->elem_size < sizeof(intptr_t) ? ch->elem_size : sizeof(intptr_t));
                ch->on_destroy.fn(ch->on_destroy.env, item_val);
            }
        } else {
            /* Mutex path: iterate buffer and destroy items */
            size_t count = ch->count;
            size_t head = ch->head;
            size_t slots = (ch->cap == 0) ? 1 : ch->cap;
            for (size_t i = 0; i < count; i++) {
                size_t idx = (head + i) % slots;
                char* item_ptr = (char*)ch->buf + (idx * ch->elem_size);
                /* Read the item value from the buffer slot */
                intptr_t item_val = 0;
                memcpy(&item_val, item_ptr, ch->elem_size < sizeof(intptr_t) ? ch->elem_size : sizeof(intptr_t));
                ch->on_destroy.fn(ch->on_destroy.env, item_val);
            }
        }
        pthread_mutex_unlock(&ch->mu);
        
        /* Call drop on closure environments if provided */
        if (ch->on_create.drop) ch->on_create.drop(ch->on_create.env);
        if (ch->on_destroy.drop) ch->on_destroy.drop(ch->on_destroy.env);
        if (ch->on_reset.drop) ch->on_reset.drop(ch->on_reset.env);
    }
    
    /* Clean up lock-free queue if used */
    if (ch->use_lockfree && ch->lfqueue_elements) {
        lfds711_queue_bmm_cleanup(&ch->lfqueue_state, NULL);
        free(ch->lfqueue_elements);
    }
    if (ch->use_lockfree && ch->ring_cells) {
        free(ch->ring_cells);
    }
    
    pthread_mutex_destroy(&ch->mu);
    pthread_cond_destroy(&ch->not_empty);
    pthread_cond_destroy(&ch->not_full);
    free(ch->buf);
    free(ch);
}

// Ensure buffer is allocated with the given element size; only allowed to set once.
static int cc_chan_ensure_buf(CCChan* ch, size_t elem_size) {
    if (ch->elem_size == 0) {
        ch->elem_size = elem_size;
        
        if (ch->use_lockfree && ch->cap > 0) {
            /* Lock-free buffered channel: allocate data buffer using lfqueue_cap */
            ch->buf = malloc(ch->lfqueue_cap * elem_size);
            if (!ch->buf) return ENOMEM;
        } else {
            /* Mutex-based or unbuffered channel */
            size_t slots = (ch->cap == 0) ? 1 : ch->cap;
            ch->buf = malloc(slots * elem_size);
            if (!ch->buf) return ENOMEM;
        }
        /* Brand the channel for the minimal fast path if all invariants hold:
         * lockfree, buffered, not owned/ordered/sync.
         * Small elements (<=ptr) always qualify; large elements qualify when
         * backed by the internal ring queue which supports by-value copies. */
        ch->fast_path_ok = (cc__chan_minimal_path_enabled() &&
                            ch->use_lockfree && ch->cap > 0 && ch->buf &&
                            (elem_size <= sizeof(void*) || ch->use_ring_queue) &&
                            !ch->is_owned && !ch->is_ordered && !ch->is_sync);
        return 0;
    }
    if (ch->elem_size != elem_size) return EINVAL;
    return 0;
}

// Initialize element size eagerly (typed channels). Allocates buffer once.
int cc_chan_init_elem(CCChan* ch, size_t elem_size) {
    if (!ch || elem_size == 0) return EINVAL;
    return cc_chan_ensure_buf(ch, elem_size);
}

static int cc_chan_wait_full(CCChan* ch, const struct timespec* deadline) {
    int err = 0;

    /* Check if we're in fiber context for fiber-aware blocking */
    cc__fiber* fiber = cc__fiber_in_context() ? cc__fiber_current() : NULL;

    /* Unbuffered rendezvous: sender must wait for a receiver and for slot to be free. */
    if (ch->cap == 0) {
        if (fiber) {
            /* Fiber-aware blocking: park the fiber instead of condvar wait */
            while (!ch->closed && !ch->rx_error_closed && (ch->rv_has_value || (ch->rv_recv_waiters == 0 && !ch->recv_waiters_head))) {
                cc__fiber_wait_node node = {0};
                node.fiber = fiber;
                atomic_store(&node.notified, 0);
                cc__chan_add_send_waiter(ch, &node);

                pthread_mutex_unlock(&ch->mu);
                cc_sched_wait_result wait_rc = cc__chan_wait_notified_mark_close(&node, deadline, "chan_send_wait_rendezvous", ch);
                pthread_mutex_lock(&ch->mu);
                if (wait_rc == CC_SCHED_WAIT_TIMEOUT) {
                    cc__chan_remove_send_waiter(ch, &node);
                    return ETIMEDOUT;
                }
                int notified = atomic_load_explicit(&node.notified, memory_order_acquire);
                if (notified == CC_CHAN_NOTIFY_SIGNAL) {
                    atomic_store_explicit(&node.notified, CC_CHAN_NOTIFY_NONE, memory_order_release);
                    cc__chan_remove_send_waiter(ch, &node);
                    continue;
                }
                if (notified == CC_CHAN_NOTIFY_CLOSE) {
                    cc__chan_remove_send_waiter(ch, &node);
                    break;
                }
                if (!notified) {
                    cc__chan_remove_send_waiter(ch, &node);
                }
            }
        } else {
            /* Traditional condvar blocking */
            while (!ch->closed && !ch->rx_error_closed && (ch->rv_has_value || ch->rv_recv_waiters == 0) && err == 0) {
                if (deadline) {
                    err = pthread_cond_timedwait(&ch->not_full, &ch->mu, deadline);
                    if (err == ETIMEDOUT) {
                        /* Close/error wins over timeout once observed. */
                        if (ch->rx_error_closed) return ch->rx_error_code;
                        if (ch->closed) return EPIPE;
                        return ETIMEDOUT;
                    }
                } else {
                    pthread_cond_wait(&ch->not_full, &ch->mu);
                }
            }
        }
        
        if (ch->rx_error_closed) return ch->rx_error_code;
        return ch->closed ? EPIPE : 0;
    }

    /* Buffered channel */
    if (fiber) {
        /* Fiber-aware blocking */
        while (!ch->closed && !ch->rx_error_closed && ch->count == ch->cap) {
            /* Check if current nursery is cancelled - unblock so the fiber can exit */
            CCNursery* cur_nursery = cc__tls_current_nursery;
            if (cur_nursery && cc_nursery_is_cancelled(cur_nursery)) {
                return ECANCELED;
            }
            cc__fiber_wait_node node = {0};
            node.fiber = fiber;
            atomic_store(&node.notified, 0);
            cc__chan_add_send_waiter(ch, &node);

            pthread_mutex_unlock(&ch->mu);
            /* Re-check closed after unlock: if close raced between the
             * while-loop condition and add_send_waiter, wake_all_waiters
             * already ran and won't find us.  Bail out to avoid stranding. */
            if (ch->closed || ch->rx_error_closed) {
                cc_chan_lock(ch);
                cc__chan_remove_send_waiter(ch, &node);
                pthread_mutex_unlock(&ch->mu);
                break;
            }
            cc_sched_wait_result wait_rc = cc__chan_wait_notified_mark_close(&node, deadline, "chan_send_wait_full", ch);
            pthread_mutex_lock(&ch->mu);
            if (wait_rc == CC_SCHED_WAIT_TIMEOUT) {
                cc__chan_remove_send_waiter(ch, &node);
                return ETIMEDOUT;
            }
            int notified = atomic_load_explicit(&node.notified, memory_order_acquire);
            if (notified == CC_CHAN_NOTIFY_SIGNAL) {
                atomic_store_explicit(&node.notified, CC_CHAN_NOTIFY_NONE, memory_order_release);
                cc__chan_remove_send_waiter(ch, &node);
                continue;
            }
            if (notified == CC_CHAN_NOTIFY_CLOSE) {
                cc__chan_remove_send_waiter(ch, &node);
                break;
            }
            if (!notified) {
                cc__chan_remove_send_waiter(ch, &node);
            }
        }
    } else {
        /* Traditional condvar blocking */
        while (!ch->closed && !ch->rx_error_closed && ch->count == ch->cap && err == 0) {
            if (deadline) {
                err = pthread_cond_timedwait(&ch->not_full, &ch->mu, deadline);
                if (err == ETIMEDOUT) {
                    /* Close/error wins over timeout once observed. */
                    if (ch->rx_error_closed) return ch->rx_error_code;
                    if (ch->closed) return EPIPE;
                    return ETIMEDOUT;
                }
            } else {
                pthread_cond_wait(&ch->not_full, &ch->mu);
            }
        }
    }
    
    if (ch->rx_error_closed) return ch->rx_error_code;
    if (ch->closed) {
        return EPIPE;
    }
    return 0;
}

static int cc_chan_wait_empty(CCChan* ch, const struct timespec* deadline) {
    int err = 0;

    /* Check if we're in fiber context for fiber-aware blocking */
    cc__fiber* fiber = cc__fiber_in_context() ? cc__fiber_current() : NULL;

    /* Unbuffered rendezvous: receiver waits for a sender to place a value. */
    if (ch->cap == 0) {
        ch->rv_recv_waiters++;
        /* Wake exactly ONE sender - prefer fiber waiters, else signal condvar */
        if (ch->send_waiters_head) {
            cc__chan_wake_one_send_waiter(ch);
        } else {
            pthread_cond_signal(&ch->not_full);
        }

        
        if (fiber) {
            /* Fiber-aware blocking */
            while (!ch->closed && !ch->rv_has_value) {
                cc__fiber_wait_node node = {0};
                node.fiber = fiber;
                atomic_store(&node.notified, 0);
                cc__chan_add_recv_waiter(ch, &node);

                pthread_mutex_unlock(&ch->mu);
                wake_batch_flush();  /* Flush queued wakes after unlocking. */

                /* Return-aware boundary wait (first migrated call site). */
                cc_sched_wait_result wait_rc = cc__chan_wait_notified_mark_close(&node, deadline, "chan_recv_wait_rendezvous", ch);

                pthread_mutex_lock(&ch->mu);
                if (wait_rc == CC_SCHED_WAIT_TIMEOUT) {
                    cc__chan_remove_recv_waiter(ch, &node);
                    ch->rv_recv_waiters--;
                    return ETIMEDOUT;
                }
                if (wait_rc == CC_SCHED_WAIT_CLOSED) {
                    cc__chan_remove_recv_waiter(ch, &node);
                    ch->rv_recv_waiters--;
                    return ch->tx_error_code ? ch->tx_error_code : EPIPE;
                }

                if (!atomic_load_explicit(&node.notified, memory_order_acquire)) {
                    cc__chan_remove_recv_waiter(ch, &node);
                }
            }
        } else {
            /* Spinlock-condvar blocking (spin then sleep) */
            while (!ch->closed && !ch->rv_has_value && err == 0) {
                if (deadline) {
                    err = pthread_cond_timedwait(&ch->not_empty, &ch->mu, deadline);
                    if (err == ETIMEDOUT) {
                        if (ch->rv_recv_waiters > 0) ch->rv_recv_waiters--;
                        /* Close/error wins over timeout once observed. */
                        if (ch->closed) return ch->tx_error_code ? ch->tx_error_code : EPIPE;
                        return ETIMEDOUT;
                    }
                } else {
                    pthread_cond_wait(&ch->not_empty, &ch->mu);
                }
            }
        }
        
        /* NOTE: Don't decrement rv_recv_waiters here! The caller will call dequeue,
         * which wakes senders. Senders need to see rv_recv_waiters > 0 to proceed.
         * The caller must decrement rv_recv_waiters AFTER dequeue. */
        if (ch->closed && !ch->rv_has_value) {
            if (ch->rv_recv_waiters > 0) ch->rv_recv_waiters--;
            return ch->tx_error_code ? ch->tx_error_code : EPIPE;
        }
        return 0;
    }

    /* Runtime guard (opt-in): blocking recv on an autoclose channel from inside the same nursery
       is a common deadlock foot-gun (recv-until-close inside the nursery). */
    if (!deadline && !ch->closed && ch->count == 0 &&
        ch->autoclose_owner && cc__tls_current_nursery &&
        ch->autoclose_owner == cc__tls_current_nursery) {
        const char* g = getenv("CC_NURSERY_CLOSING_RUNTIME_GUARD");
        if (g && g[0] == '1') {
            if (!ch->warned_autoclose_block) {
                ch->warned_autoclose_block = 1;
                fprintf(stderr,
                        "CC: runtime guard: blocking cc_chan_recv() on a `closing(...)` channel from inside the same nursery "
                        "may deadlock (use a sentinel/explicit close, or drain outside the nursery)\n");
            }
            return EDEADLK;
        }
    }

    
    if (fiber) {
        /* Fiber-aware blocking */
        while (!ch->closed && ch->count == 0) {
            /* Re-check deadlock guard inside loop (same as initial guard above) */
            if (!deadline && !ch->closed && ch->count == 0 &&
                ch->autoclose_owner && cc__tls_current_nursery &&
                ch->autoclose_owner == cc__tls_current_nursery) {
                const char* g = getenv("CC_NURSERY_CLOSING_RUNTIME_GUARD");
                if (g && g[0] == '1') {
                    if (!ch->warned_autoclose_block) {
                        ch->warned_autoclose_block = 1;
                        fprintf(stderr,
                                "CC: runtime guard: blocking cc_chan_recv() on a `closing(...)` channel from inside the same nursery "
                                "may deadlock (use a sentinel/explicit close, or drain outside the nursery)\n");
                    }
                    return EDEADLK;
                }
            }
            cc__fiber_wait_node node = {0};
            node.fiber = fiber;
            atomic_store(&node.notified, 0);
            cc__chan_add_recv_waiter(ch, &node);

            pthread_mutex_unlock(&ch->mu);
            /* Re-check closed after unlock: if close raced between the
             * while-loop condition and add_recv_waiter, wake_all_waiters
             * already ran and won't find us.  Bail out to avoid stranding. */
            if (ch->closed) {
                cc_chan_lock(ch);
                cc__chan_remove_recv_waiter(ch, &node);
                pthread_mutex_unlock(&ch->mu);
                cc__chan_trace_recv_empty(ch, "recv_closed_before_wait", &node,
                                          atomic_load_explicit(&node.notified, memory_order_relaxed));
                break;
            }
            cc_sched_wait_result wait_rc = cc__chan_wait_notified_mark_close(&node, deadline, "chan_recv_wait_empty", ch);
            pthread_mutex_lock(&ch->mu);
            cc__chan_trace_recv_empty(ch, "recv_post_wait", &node,
                                      atomic_load_explicit(&node.notified, memory_order_relaxed));
            if (wait_rc == CC_SCHED_WAIT_TIMEOUT) {
                cc__chan_remove_recv_waiter(ch, &node);
                return ETIMEDOUT;
            }

            int notified = atomic_load_explicit(&node.notified, memory_order_acquire);
            if (notified == CC_CHAN_NOTIFY_SIGNAL) {
                cc__chan_remove_recv_waiter(ch, &node);
                atomic_store_explicit(&node.notified, CC_CHAN_NOTIFY_NONE, memory_order_release);
                continue;
            }
            if (notified == CC_CHAN_NOTIFY_CLOSE) {
                cc__chan_remove_recv_waiter(ch, &node);
                break;
            }
            if (!notified) {
                cc__chan_remove_recv_waiter(ch, &node);
            }
        }
    } else {
        /* Spinlock-condvar blocking (spin then sleep) */
        while (!ch->closed && ch->count == 0 && err == 0) {
            if (deadline) {
                err = pthread_cond_timedwait(&ch->not_empty, &ch->mu, deadline);
                if (err == ETIMEDOUT) {
                    /* Close wins over timeout once observed. */
                    if (ch->closed && ch->count == 0) return ch->tx_error_code ? ch->tx_error_code : EPIPE;
                    return ETIMEDOUT;
                }
            } else {
                pthread_cond_wait(&ch->not_empty, &ch->mu);
            }
        }
    }
    
    if (ch->closed && ch->count == 0) return ch->tx_error_code ? ch->tx_error_code : EPIPE;
    return 0;
}

static inline void channel_store_slot(void* slot, const void* value, size_t size) {
    switch (size) {
        case 1:
            *(uint8_t*)slot = *(const uint8_t*)value;
            break;
        case 2:
            *(uint16_t*)slot = *(const uint16_t*)value;
            break;
        case 4:
            *(uint32_t*)slot = *(const uint32_t*)value;
            break;
        case 8:
            *(uint64_t*)slot = *(const uint64_t*)value;
            break;
        default:
            memcpy(slot, value, size);
            break;
    }
}

static inline void channel_load_slot(const void* slot, void* out_value, size_t size) {
    switch (size) {
        case 1:
            *(uint8_t*)out_value = *(const uint8_t*)slot;
            break;
        case 2:
            *(uint16_t*)out_value = *(const uint16_t*)slot;
            break;
        case 4:
            *(uint32_t*)out_value = *(const uint32_t*)slot;
            break;
        case 8:
            *(uint64_t*)out_value = *(const uint64_t*)slot;
            break;
        default:
            memcpy(out_value, slot, size);
            break;
    }
}

static void cc_chan_enqueue(CCChan* ch, const void* value) {
    if (ch->cap == 0) {
        /* Unbuffered: always signal - rendezvous has complex handshake timing */
        channel_store_slot(ch->buf, value, ch->elem_size);
        ch->rv_has_value = 1;
        cc__chan_trace_flow(ch, "send_enqueue_mutex", value, 0);
        pthread_cond_signal(&ch->not_empty);
        cc__chan_signal_recv_waiter(ch);
        cc__chan_signal_recv_ready(ch);
        return;
    }
    /* Buffered: signal waiters */
    void *slot = (uint8_t*)ch->buf + ch->tail * ch->elem_size;
    channel_store_slot(slot, value, ch->elem_size);
    ch->tail = (ch->tail + 1) % ch->cap;
    ch->count++;
    cc__chan_trace_flow(ch, "send_enqueue_mutex", value, 0);
    pthread_cond_signal(&ch->not_empty);
    cc__chan_signal_recv_waiter(ch);
    cc__chan_signal_recv_ready(ch);
}

static void cc_chan_dequeue(CCChan* ch, void* out_value) {
    if (ch->cap == 0) {
        /* Unbuffered rendezvous: wake one sender waiting for consumption. */
        channel_load_slot(ch->buf, out_value, ch->elem_size);
        ch->rv_has_value = 0;
        cc__chan_trace_flow(ch, "recv_dequeue_mutex", out_value, 0);
        if (ch->send_waiters_head) {
            cc__chan_wake_one_send_waiter(ch);
            wake_batch_flush();
        }
        /* Also signal condvar waiters */
        pthread_cond_broadcast(&ch->not_full);
        cc__chan_signal_activity(ch);
        return;
    }
    /* Buffered: signal waiters */
    void *slot = (uint8_t*)ch->buf + ch->head * ch->elem_size;
    channel_load_slot(slot, out_value, ch->elem_size);
    ch->head = (ch->head + 1) % ch->cap;
    ch->count--;
    cc__chan_trace_flow(ch, "recv_dequeue_mutex", out_value, 0);
    pthread_cond_signal(&ch->not_full);
    cc__chan_wake_one_send_waiter(ch);
    cc__chan_signal_activity(ch);
}

/* ============================================================================
 * Lock-Free Queue Operations for Buffered Channels
 * ============================================================================
 * These use the internal ring queue or liblfds bounded MPMC queue for the hot path.
 *
 * Data storage strategy:
 * - Ring queue + elem_size <= sizeof(void*): store data directly in cell->value
 * - Ring queue + elem_size > sizeof(void*): copy data into the ring-owned slot in ch->buf
 * - liblfds + elem_size <= sizeof(void*): store data directly in queue value pointer
 *
 * Large-element lock-free storage is currently supported only by the internal ring queue.
 */

static inline void* cc__ring_slot_ptr(CCChan* ch, size_t pos) {
    return (char*)ch->buf + ((pos & (ch->lfqueue_cap - 1)) * ch->elem_size);
}

static inline int cc__ring_enqueue_spsc_value(CCChan* ch, const void* value) {
    size_t tail = atomic_load_explicit(&ch->ring_tail, memory_order_relaxed);
    size_t head = atomic_load_explicit(&ch->ring_head, memory_order_acquire);
    if ((tail - head) >= ch->cap) {
        return 0;
    }

    size_t slot = tail & (ch->lfqueue_cap - 1);
    if (ch->elem_size <= sizeof(void*)) {
        void* queue_val = NULL;
        memcpy(&queue_val, value, ch->elem_size);
        ch->ring_cells[slot].value = queue_val;
    } else {
        void* dst = cc__ring_slot_ptr(ch, tail);
        channel_store_slot(dst, value, ch->elem_size);
    }
    atomic_store_explicit(&ch->ring_tail, tail + 1, memory_order_release);
    return 1;
}

static inline int cc__ring_dequeue_spsc_value(CCChan* ch, void* out_value) {
    size_t head = atomic_load_explicit(&ch->ring_head, memory_order_relaxed);
    size_t tail = atomic_load_explicit(&ch->ring_tail, memory_order_acquire);
    if (head == tail) {
        return 0;
    }

    size_t slot = head & (ch->lfqueue_cap - 1);
    if (ch->elem_size <= sizeof(void*)) {
        void* val = ch->ring_cells[slot].value;
        memcpy(out_value, &val, ch->elem_size);
    } else {
        void* src = cc__ring_slot_ptr(ch, head);
        channel_load_slot(src, out_value, ch->elem_size);
    }
    atomic_store_explicit(&ch->ring_head, head + 1, memory_order_release);
    return 1;
}

static inline int cc__ring_enqueue_raw(CCChan* ch, void* queue_val) {
    size_t pos = atomic_load_explicit(&ch->ring_tail, memory_order_relaxed);
    int spins = 0;
    for (;;) {
        cc__ring_cell* cell = &ch->ring_cells[pos & (ch->lfqueue_cap - 1)];
        size_t seq = atomic_load_explicit(&cell->seq, memory_order_acquire);
        intptr_t dif = (intptr_t)seq - (intptr_t)pos;
        if (dif == 0) {
            if (atomic_compare_exchange_weak_explicit(&ch->ring_tail, &pos, pos + 1,
                                                      memory_order_relaxed,
                                                      memory_order_relaxed)) {
                cell->value = queue_val;
                atomic_store_explicit(&cell->seq, pos + 1, memory_order_release);
                return 1;
            }
        } else if (dif < 0) {
            return 0; /* full */
        } else {
            pos = atomic_load_explicit(&ch->ring_tail, memory_order_relaxed);
        }
        for (int i = 0; i <= spins; i++)
            cc__chan_cpu_pause();
        spins++;
    }
}

static inline int cc__ring_dequeue_raw(CCChan* ch, void** out_val) {
    size_t pos = atomic_load_explicit(&ch->ring_head, memory_order_relaxed);
    int spins = 0;
    for (;;) {
        cc__ring_cell* cell = &ch->ring_cells[pos & (ch->lfqueue_cap - 1)];
        size_t seq = atomic_load_explicit(&cell->seq, memory_order_acquire);
        intptr_t dif = (intptr_t)seq - (intptr_t)(pos + 1);
        if (dif == 0) {
            if (atomic_compare_exchange_weak_explicit(&ch->ring_head, &pos, pos + 1,
                                                      memory_order_relaxed,
                                                      memory_order_relaxed)) {
                *out_val = cell->value;
                atomic_store_explicit(&cell->seq, pos + ch->lfqueue_cap, memory_order_release);
                return 1;
            }
        } else if (dif < 0) {
            return 0; /* empty */
        } else {
            pos = atomic_load_explicit(&ch->ring_head, memory_order_relaxed);
        }
        for (int i = 0; i <= spins; i++)
            cc__chan_cpu_pause();
        spins++;
    }
}

static inline int cc__queue_enqueue_raw(CCChan* ch, void* queue_val) {
    if (ch->use_ring_queue) {
        return cc__ring_enqueue_raw(ch, queue_val);
    }
    return lfds711_queue_bmm_enqueue(&ch->lfqueue_state, NULL, queue_val);
}

static inline int cc__queue_dequeue_raw(CCChan* ch, void** out_val) {
    if (ch->use_ring_queue) {
        return cc__ring_dequeue_raw(ch, out_val);
    }
    void* key = NULL;
    return lfds711_queue_bmm_dequeue(&ch->lfqueue_state, &key, out_val);
}

static inline int cc__queue_enqueue_value(CCChan* ch, const void* value) {
    if (ch->use_ring_queue) {
        if (ch->topology == CC_CHAN_TOPO_1_1) {
            return cc__ring_enqueue_spsc_value(ch, value);
        }
        size_t pos = atomic_load_explicit(&ch->ring_tail, memory_order_relaxed);
        int spins = 0;
        for (;;) {
            cc__ring_cell* cell = &ch->ring_cells[pos & (ch->lfqueue_cap - 1)];
            size_t seq = atomic_load_explicit(&cell->seq, memory_order_acquire);
            intptr_t dif = (intptr_t)seq - (intptr_t)pos;
            if (dif == 0) {
                if (atomic_compare_exchange_weak_explicit(&ch->ring_tail, &pos, pos + 1,
                                                          memory_order_relaxed,
                                                          memory_order_relaxed)) {
                    if (ch->elem_size <= sizeof(void*)) {
                        void* queue_val = NULL;
                        memcpy(&queue_val, value, ch->elem_size);
                        cell->value = queue_val;
                    } else {
                        void* slot = cc__ring_slot_ptr(ch, pos);
                        channel_store_slot(slot, value, ch->elem_size);
                        cell->value = slot;
                    }
                    atomic_store_explicit(&cell->seq, pos + 1, memory_order_release);
                    return 1;
                }
            } else if (dif < 0) {
                return 0;
            } else {
                pos = atomic_load_explicit(&ch->ring_tail, memory_order_relaxed);
            }
            for (int i = 0; i <= spins; i++) cc__chan_cpu_pause();
            spins++;
        }
    }

    if (ch->elem_size > sizeof(void*)) {
        return 0;
    }

    void* queue_val = NULL;
    memcpy(&queue_val, value, ch->elem_size);
    return lfds711_queue_bmm_enqueue(&ch->lfqueue_state, NULL, queue_val);
}

static inline int cc__queue_dequeue_value(CCChan* ch, void* out_value) {
    if (ch->use_ring_queue) {
        if (ch->topology == CC_CHAN_TOPO_1_1) {
            return cc__ring_dequeue_spsc_value(ch, out_value);
        }
        size_t pos = atomic_load_explicit(&ch->ring_head, memory_order_relaxed);
        int spins = 0;
        for (;;) {
            cc__ring_cell* cell = &ch->ring_cells[pos & (ch->lfqueue_cap - 1)];
            size_t seq = atomic_load_explicit(&cell->seq, memory_order_acquire);
            intptr_t dif = (intptr_t)seq - (intptr_t)(pos + 1);
            if (dif == 0) {
                if (atomic_compare_exchange_weak_explicit(&ch->ring_head, &pos, pos + 1,
                                                          memory_order_relaxed,
                                                          memory_order_relaxed)) {
                    if (ch->elem_size <= sizeof(void*)) {
                        void* val = cell->value;
                        memcpy(out_value, &val, ch->elem_size);
                    } else {
                        void* slot = cc__ring_slot_ptr(ch, pos);
                        channel_load_slot(slot, out_value, ch->elem_size);
                        cell->value = slot;
                    }
                    atomic_store_explicit(&cell->seq, pos + ch->lfqueue_cap, memory_order_release);
                    return 1;
                }
            } else if (dif < 0) {
                return 0;
            } else {
                pos = atomic_load_explicit(&ch->ring_head, memory_order_relaxed);
            }
            for (int i = 0; i <= spins; i++) cc__chan_cpu_pause();
            spins++;
        }
    }

    if (ch->elem_size > sizeof(void*)) {
        return 0;
    }

    void* val = NULL;
    void* key = NULL;
    if (!lfds711_queue_bmm_dequeue(&ch->lfqueue_state, &key, &val)) {
        return 0;
    }
    memcpy(out_value, &val, ch->elem_size);
    return 1;
}

static inline void chan_inflight_inc(CCChan* ch) {
    if (!ch->use_ring_queue) {
        atomic_fetch_add_explicit(&ch->lfqueue_inflight, 1, memory_order_relaxed);
    }
}

static inline void chan_inflight_dec(CCChan* ch) {
    if (!ch->use_ring_queue) {
        atomic_fetch_sub_explicit(&ch->lfqueue_inflight, 1, memory_order_relaxed);
    }
}

static inline int cc__chan_lf_count(CCChan* ch) {
    if (ch->use_ring_queue) {
        /* Acquire ordering so the count synchronizes with enqueue/dequeue
         * CAS operations.  Relaxed loads can return stale values on ARM64,
         * which broke Dekker-style park decisions (see Fix 2). */
        size_t tail = atomic_load_explicit(&ch->ring_tail, memory_order_acquire);
        size_t head = atomic_load_explicit(&ch->ring_head, memory_order_acquire);
        return (int)(tail - head);
    }
    return atomic_load_explicit(&ch->lfqueue_count, memory_order_acquire);
}

/* Helper: try lock-free enqueue without incrementing inflight counter.
 * Caller MUST manage lfqueue_inflight (inc before, dec after).
 * Must NOT hold ch->mu when calling this.
 * Large by-value elements are supported only on the ring queue backend. */
static int cc__chan_try_enqueue_lockfree_impl(CCChan* ch, const void* value) {
    if (!ch->use_lockfree || ch->cap == 0 || !ch->buf) return EAGAIN;
    if (!ch->use_ring_queue && ch->elem_size > sizeof(void*)) {
        fprintf(stderr, "BUG: cc__chan_try_enqueue_lockfree_impl called with large element (size=%zu)\n", ch->elem_size);
        return EAGAIN;
    }

    int ok = cc__queue_enqueue_value(ch, value);
    if (ok && !ch->use_ring_queue) {
        atomic_fetch_add_explicit(&ch->lfqueue_count, 1, memory_order_release);
    }
    if (ok) {
        cc__chan_trace_flow(ch, "send_enqueue_try", value, 0);
    }
    return ok ? 0 : EAGAIN;
}

/* Wrapper that manages inflight counter automatically */
static int cc_chan_try_enqueue_lockfree(CCChan* ch, const void* value) {
    chan_inflight_inc(ch);
    int rc = cc__chan_try_enqueue_lockfree_impl(ch, value);
    chan_inflight_dec(ch);
    return rc;
}

/* Minimal-path enqueue: absolute minimum work.  No debug counters, no lfqueue_count,
 * no signal_activity, no maybe_yield.  Used only from the branded fast_path_ok path
 * where the caller has already checked the brand. */
static inline int cc__chan_enqueue_lockfree_minimal(CCChan* ch, const void* value, int* old_count_out) {
    if (!cc__queue_enqueue_value(ch, value))
        return EAGAIN;
    if (ch->use_ring_queue) {
        (void)old_count_out;
        cc__chan_trace_flow(ch, "send_enqueue_minimal", value, 0);
        return 0;
    }
    int old_count = atomic_fetch_add_explicit(&ch->lfqueue_count, 1, memory_order_release);
    if (old_count_out) *old_count_out = old_count;
    cc__chan_trace_flow(ch, "send_enqueue_minimal", value, 0);
    return 0;
}

/* Minimal-path dequeue: absolute minimum work. */
static inline int cc__chan_dequeue_lockfree_minimal(CCChan* ch, void* out_value, int* old_count_out) {
    if (!cc__queue_dequeue_value(ch, out_value))
        return EAGAIN;
    if (!ch->use_ring_queue) {
        int old_count = atomic_fetch_sub_explicit(&ch->lfqueue_count, 1, memory_order_release);
        if (old_count_out) *old_count_out = old_count;
    } else {
        (void)old_count_out;
    }
    cc__chan_trace_flow(ch, "recv_dequeue_minimal", out_value, 0);
    return 0;
}

static inline int cc__chan_enqueue_mutex_minimal(CCChan* ch, const void* value) {
    cc_chan_lock(ch);
    if (ch->closed) {
        pthread_mutex_unlock(&ch->mu);
        return EPIPE;
    }
    if (ch->rx_error_closed) {
        int err = ch->rx_error_code;
        pthread_mutex_unlock(&ch->mu);
        return err;
    }
    if (ch->count >= ch->cap) {
        pthread_mutex_unlock(&ch->mu);
        return EAGAIN;
    }
    void *slot = (uint8_t*)ch->buf + ch->tail * ch->elem_size;
    channel_store_slot(slot, value, ch->elem_size);
    ch->tail = (ch->tail + 1) % ch->cap;
    ch->count++;
    pthread_cond_signal(&ch->not_empty);
    if (atomic_load_explicit(&ch->has_recv_waiters, memory_order_acquire)) {
        cc__chan_signal_recv_waiter(ch);
    }
    cc__chan_trace_flow(ch, "send_enqueue_mutex_minimal", value, 0);
    pthread_mutex_unlock(&ch->mu);
    wake_batch_flush();
    cc__chan_signal_recv_ready(ch);
    return 0;
}

static inline int cc__chan_dequeue_mutex_minimal(CCChan* ch, void* out_value) {
    cc_chan_lock(ch);
    if (ch->count <= 0) {
        pthread_mutex_unlock(&ch->mu);
        return EAGAIN;
    }
    void *slot = (uint8_t*)ch->buf + ch->head * ch->elem_size;
    channel_load_slot(slot, out_value, ch->elem_size);
    ch->head = (ch->head + 1) % ch->cap;
    ch->count--;
    pthread_cond_signal(&ch->not_full);
    if (atomic_load_explicit(&ch->has_send_waiters, memory_order_acquire)) {
        cc__chan_wake_one_send_waiter(ch);
    }
    cc__chan_trace_flow(ch, "recv_dequeue_mutex_minimal", out_value, 0);
    pthread_mutex_unlock(&ch->mu);
    wake_batch_flush();
    return 0;
}

/* Fast-path enqueue: no guard checks, no inflight tracking.
 * Caller MUST have already verified use_lockfree, cap>0, and buf.
 * Use only on the hot send path where the channel is known to be open and valid. */
static inline int cc__chan_enqueue_lockfree_fast(CCChan* ch, const void* value, int* old_count_out) {
    int ok = cc__queue_enqueue_value(ch, value);
    if (ok) {
        if (!ch->use_ring_queue) {
            int old_count = atomic_fetch_add_explicit(&ch->lfqueue_count, 1, memory_order_release);
            if (old_count_out) *old_count_out = old_count;
        } else {
            (void)old_count_out;
        }
        cc__chan_trace_flow(ch, "send_enqueue_fast", value, 0);
    } else {
    }
    return ok ? 0 : EAGAIN;
}

/* Fast-path dequeue: no guard checks.
 * Caller MUST have already verified use_lockfree, cap>0, and buf. */
static inline int cc__chan_dequeue_lockfree_fast(CCChan* ch, void* out_value, int* old_count_out) {
    int ok = cc__queue_dequeue_value(ch, out_value);
    if (!ok) {
        return EAGAIN;
    }
    if (!ch->use_ring_queue) {
        int old_count = atomic_fetch_sub_explicit(&ch->lfqueue_count, 1, memory_order_release);
        if (old_count_out) *old_count_out = old_count;
    } else {
        (void)old_count_out;
    }
    cc__chan_trace_flow(ch, "recv_dequeue_fast", out_value, 0);
    return 0;
}

/* Try lock-free dequeue. Returns 0 on success, EAGAIN if empty.
 * Must NOT hold ch->mu when calling this.
 * Large by-value elements are supported only on the ring queue backend. */
static int cc_chan_try_dequeue_lockfree(CCChan* ch, void* out_value) {
    if (!ch->use_lockfree || ch->cap == 0 || !ch->buf) return EAGAIN;
    if (!ch->use_ring_queue && ch->elem_size > sizeof(void*)) {
        fprintf(stderr, "BUG: cc_chan_try_dequeue_lockfree called with large element (size=%zu)\n", ch->elem_size);
        return EAGAIN;
    }

    int ok = cc__queue_dequeue_value(ch, out_value);
    if (!ok) {
        return EAGAIN;
    }
    if (!ch->use_ring_queue) {
        atomic_fetch_sub_explicit(&ch->lfqueue_count, 1, memory_order_release);
    }
    cc__chan_trace_flow(ch, "recv_dequeue_try", out_value, 0);
    return 0;
}

static inline int cc__chan_timespec_expired(const struct timespec* abs_deadline) {
    if (!abs_deadline) return 0;
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    return (now.tv_sec > abs_deadline->tv_sec ||
        (now.tv_sec == abs_deadline->tv_sec && now.tv_nsec >= abs_deadline->tv_nsec));
}

static int cc__chan_try_drain_lockfree_on_close(CCChan* ch, void* out_value, const struct timespec* abs_deadline) {
    if (ch->use_lockfree) {
    }
    while (1) {
        if (cc_chan_try_dequeue_lockfree(ch, out_value) == 0) {
            return 0;
        }
        if (ch->use_ring_queue) {
            return ch->tx_error_code ? ch->tx_error_code : EPIPE;
        }
        int inflight = atomic_load_explicit(&ch->lfqueue_inflight, memory_order_acquire);
        if (inflight == 0) {
            return ch->tx_error_code ? ch->tx_error_code : EPIPE;
        }
        if (cc__chan_timespec_expired(abs_deadline)) {
            return ETIMEDOUT;
        }
        if (cc__fiber_in_context()) {
            cc__fiber_yield();
        } else {
            sched_yield();
        }
    }
}

/* ============================================================================
 * Unbuffered Channel (Rendezvous) Operations
 * ============================================================================
 * Unbuffered channels use mutex+condvar for direct handoff between sender
 * and receiver. This is similar to Go's unbuffered channel implementation.
 */

/* Direct handoff rendezvous helpers (cap == 0). Expects ch->mu locked. */
static inline void cc__chan_finish_send_to_recv_waiter(CCChan* ch, cc__fiber_wait_node* rnode, const void* value) {
    channel_store_slot(rnode->data, value, ch->elem_size);
    atomic_store_explicit(&rnode->notified, CC_CHAN_NOTIFY_DATA, memory_order_release);
    if (rnode->is_select) cc__chan_select_dbg_inc(&g_dbg_select_data_set);
    if (ch->rv_recv_waiters > 0) ch->rv_recv_waiters--;
    if (rnode->is_select && rnode->select_group) {
        cc__select_wait_group* group = (cc__select_wait_group*)rnode->select_group;
        atomic_fetch_add_explicit(&group->signaled, 1, memory_order_release);
    }
    if (rnode->fiber) {
        wake_batch_add_chan(ch, rnode->fiber);
    } else {
        pthread_cond_signal(&ch->not_empty);
    }
}

static inline int cc__chan_try_direct_handoff_recv_waiter_buffered(CCChan* ch, const void* value) {
    if (!ch) return 0;

    cc_chan_lock(ch);
    if (ch->closed) {
        pthread_mutex_unlock(&ch->mu);
        return -EPIPE;
    }
    if (ch->rx_error_closed) {
        int err = ch->rx_error_code;
        pthread_mutex_unlock(&ch->mu);
        return -err;
    }

    cc__fiber_wait_node* rnode = cc__chan_pop_recv_waiter(ch);
    if (!rnode) {
        pthread_mutex_unlock(&ch->mu);
        return 0;
    }

    channel_store_slot(rnode->data, value, ch->elem_size);
    atomic_store_explicit(&rnode->notified, CC_CHAN_NOTIFY_DATA, memory_order_release);
    if (rnode->is_select) cc__chan_select_dbg_inc(&g_dbg_select_data_set);
    if (rnode->is_select && rnode->select_group) {
        cc__select_wait_group* group = (cc__select_wait_group*)rnode->select_group;
        atomic_fetch_add_explicit(&group->signaled, 1, memory_order_release);
    }
    if (rnode->fiber) {
        wake_batch_add_chan(ch, rnode->fiber);
    } else {
        pthread_cond_signal(&ch->not_empty);
    }
    pthread_mutex_unlock(&ch->mu);
    wake_batch_flush();
    cc__chan_signal_recv_ready(ch);
    return 1;
}

static inline void cc__chan_finish_recv_from_send_waiter(CCChan* ch, cc__fiber_wait_node* snode, void* out_value) {
    channel_load_slot(snode->data, out_value, ch->elem_size);
    atomic_store_explicit(&snode->notified, CC_CHAN_NOTIFY_DATA, memory_order_release);
    if (snode->is_select) cc__chan_select_dbg_inc(&g_dbg_select_data_set);
    if (snode->is_select && snode->select_group) {
        cc__select_wait_group* group = (cc__select_wait_group*)snode->select_group;
        atomic_fetch_add_explicit(&group->signaled, 1, memory_order_release);
    }
    if (snode->fiber) {
        wake_batch_add_chan(ch, snode->fiber);
    } else {
        pthread_cond_signal(&ch->not_full);
    }
}

static int cc_chan_send_unbuffered(CCChan* ch, const void* value, const struct timespec* deadline) {
    cc__fiber* fiber = cc__fiber_in_context() ? cc__fiber_current() : NULL;
    int err = 0;

    while (!ch->closed && !ch->rx_error_closed) {
        /* If a receiver is waiting, handoff directly */
        cc__fiber_wait_node* rnode = ch->recv_waiters_head ? cc__chan_pop_recv_waiter(ch) : NULL;
        if (rnode) {
            cc__chan_finish_send_to_recv_waiter(ch, rnode, value);
            cc__chan_signal_recv_ready(ch);
            return 0;
        }

        /* No receiver; wait */
        cc__fiber_wait_node node = {0};
        node.fiber = (fiber && !deadline) ? fiber : NULL;
        node.data = (void*)value;
        atomic_store(&node.notified, 0);
        cc__chan_add_send_waiter(ch, &node);
        cc__chan_signal_activity(ch);

        while (!ch->closed && !ch->rx_error_closed && !atomic_load_explicit(&node.notified, memory_order_acquire) && err == 0) {
            /* NOTE: No nursery cancellation check here. Once we've committed
             * to the send (added ourselves to the wait list), we must complete
             * the rendezvous or exit via channel close. Bailing mid-operation
             * leaves the partner (receiver) stranded. Nursery cancellation is
             * checked at the entry to cc_chan_send(), before we commit. */
            if (fiber && !deadline) {
                /* Before releasing mutex, check if a receiver arrived while we were
                 * setting up. This closes the race where a select receiver adds its
                 * node after our pop_recv_waiter but before we park. */
                cc__fiber_wait_node* rnode2 = ch->recv_waiters_head ? cc__chan_pop_recv_waiter(ch) : NULL;
                if (rnode2) {
                    /* Found a receiver! Remove ourselves from send wait list and do handoff. */
                    cc__chan_remove_send_waiter(ch, &node);
                    cc__chan_finish_send_to_recv_waiter(ch, rnode2, value);
                    cc__chan_signal_recv_ready(ch);
                    return 0;
                }
                pthread_mutex_unlock(&ch->mu);
                if (!atomic_load_explicit(&node.notified, memory_order_acquire)) {
                    (void)cc__chan_wait_notified_mark_close(&node, NULL, "chan_send_wait_rendezvous", ch);
                }
                pthread_mutex_lock(&ch->mu);
            } else {
                if (deadline) {
                    err = pthread_cond_timedwait(&ch->not_full, &ch->mu, deadline);
                    if (err == ETIMEDOUT) break;
                } else {
                    pthread_cond_wait(&ch->not_full, &ch->mu);
                }
            }
        }

        {
            int notify_val = atomic_load_explicit(&node.notified, memory_order_acquire);
            if (notify_val == CC_CHAN_NOTIFY_SIGNAL) {
                atomic_store_explicit(&node.notified, CC_CHAN_NOTIFY_NONE, memory_order_release);
                cc__chan_remove_send_waiter(ch, &node);
                continue;
            }
            if (notify_val == CC_CHAN_NOTIFY_DATA) {
                /* notified=1 means a receiver actually took our data.
                 * The receiver already popped us from the list. */
                return 0;
            }
            if (notify_val == CC_CHAN_NOTIFY_CLOSE) {
                /* notified=3 means woken by close or rx_error_close */
                cc__chan_remove_send_waiter(ch, &node);
                return ch->rx_error_closed ? ch->rx_error_code : EPIPE;
            }
            /* notified == 0: spurious wakeup (pending_unpark consumed but no
             * actual notification).  Remove ourselves from the wait list before
             * restarting the outer loop -- otherwise the node (a stack local) is
             * re-initialized while still linked, corrupting the doubly-linked
             * list. */
            cc__chan_remove_send_waiter(ch, &node);
        }

        if (ch->rx_error_closed) {
            /* node already removed above */
            return ch->rx_error_code;
        }
        if (ch->closed) {
            /* node already removed above */
            return EPIPE;
        }
        if (deadline && err == ETIMEDOUT) {
            /* node already removed above */
            return ETIMEDOUT;
        }
    }
    return ch->rx_error_closed ? ch->rx_error_code : EPIPE;
}

static int cc_chan_recv_unbuffered(CCChan* ch, void* out_value, const struct timespec* deadline) {
    cc__fiber* fiber = cc__fiber_in_context() ? cc__fiber_current() : NULL;
    int err = 0;

    while (!ch->closed) {
        /* If a sender is waiting, handoff directly */
        cc__fiber_wait_node* snode = ch->send_waiters_head ? cc__chan_pop_send_waiter(ch) : NULL;
        if (snode) {
            cc__chan_finish_recv_from_send_waiter(ch, snode, out_value);
            cc__chan_signal_recv_ready(ch);
            return 0;
        }

        /* No sender; wait */
        ch->rv_recv_waiters++;
        cc__fiber_wait_node node = {0};
        node.fiber = (fiber && !deadline) ? fiber : NULL;
        node.data = out_value;
        atomic_store(&node.notified, 0);
        cc__chan_add_recv_waiter(ch, &node);
        cc__chan_signal_activity(ch);

        while (!ch->closed && !atomic_load_explicit(&node.notified, memory_order_acquire) && err == 0) {
            /* NOTE: No nursery cancellation check here. Once we've committed
             * to the recv (added ourselves to the wait list), we must complete
             * the rendezvous or exit via channel close. Bailing mid-operation
             * leaves the partner (sender) stranded. Nursery cancellation is
             * checked at the entry to cc_chan_recv(), before we commit. */
            if (fiber && !deadline) {
                pthread_mutex_unlock(&ch->mu);
                if (!atomic_load_explicit(&node.notified, memory_order_acquire)) {
                    (void)cc__chan_wait_notified_mark_close(&node, NULL, "chan_recv_wait_rendezvous", ch);
                }
                pthread_mutex_lock(&ch->mu);
            } else {
                if (deadline) {
                    err = pthread_cond_timedwait(&ch->not_empty, &ch->mu, deadline);
                    if (err == ETIMEDOUT) break;
                } else {
                    pthread_cond_wait(&ch->not_empty, &ch->mu);
                }
            }
        }

        {
            int notify_val = atomic_load_explicit(&node.notified, memory_order_acquire);
            if (notify_val == CC_CHAN_NOTIFY_SIGNAL) {
                cc__chan_remove_recv_waiter(ch, &node);
                atomic_store_explicit(&node.notified, CC_CHAN_NOTIFY_NONE, memory_order_release);
                if (ch->rv_recv_waiters > 0) ch->rv_recv_waiters--;
                continue;
            }
            if (notify_val == CC_CHAN_NOTIFY_DATA) {
                /* notified=1 means a sender actually delivered data.
                 * The sender already popped us from the list. */
                if (ch->rv_recv_waiters > 0) ch->rv_recv_waiters--;
                return 0;
            }
            if (notify_val == CC_CHAN_NOTIFY_CLOSE) {
                /* notified=3 means woken by close or close_err with no data */
                cc__chan_remove_recv_waiter(ch, &node);
                if (ch->rv_recv_waiters > 0) ch->rv_recv_waiters--;
                return ch->tx_error_code ? ch->tx_error_code : EPIPE;
            }
            /* notified == 0: spurious wakeup (pending_unpark consumed but no
             * actual notification).  Remove ourselves from the wait list before
             * restarting the outer loop -- otherwise the node (a stack local) is
             * re-initialized at line 1981 while still linked, corrupting the
             * doubly-linked list. */
            cc__chan_remove_recv_waiter(ch, &node);
            if (ch->rv_recv_waiters > 0) ch->rv_recv_waiters--;
        }

        if (ch->closed) {
            /* node already removed above */
            if (ch->send_waiters_head) {
            }
            return ch->tx_error_code ? ch->tx_error_code : EPIPE;
        }
        if (deadline && err == ETIMEDOUT) {
            /* node already removed above */
            return ETIMEDOUT;
        }
    }
    if (ch->send_waiters_head) {
    }
    return ch->tx_error_code ? ch->tx_error_code : EPIPE;
}

static int cc_chan_handle_full_send(CCChan* ch, const void* value, const struct timespec* deadline) {
    (void)value;
    if (ch->mode == CC_CHAN_MODE_BLOCK) {
        return cc_chan_wait_full(ch, deadline);
    } else if (ch->mode == CC_CHAN_MODE_DROP_NEW) {
        return EAGAIN;
    } else { // DROP_OLD
        ch->head = (ch->head + 1) % ch->cap;
        ch->count--;
        return 0;
    }
}

int cc_chan_send(CCChan* ch, const void* value, size_t value_size) {
    /* Minimal fast path: branded channel, just enqueue and return.
     * Skips guards, debug, timing, signal_activity.
     * Uses the post-enqueue wake path to notify parked receivers. */
    if (ch->fast_path_ok && value_size == ch->elem_size) {
        if (cc__chan_mutex_minimal_enabled()) {
            int rc = cc__chan_enqueue_mutex_minimal(ch, value);
            if (rc == 0) return 0;
            if (rc != EAGAIN) return rc;
        } else {
            if (cc__chan_enqueue_lockfree_minimal(ch, value, NULL) == 0) {
                cc__chan_post_lockfree_enqueue_signal_receivers(ch, value,
                                                                "send_minimal_seen_waiter");
                return 0;
            }
        }
        /* Buffer full — fall through to full path for yield-retry / blocking */
    }
    if (!ch || !value || value_size == 0) return EINVAL;
    
    /* Owned channel (pool): call on_reset before returning item to pool */
    if (ch->is_owned && ch->on_reset.fn) {
        /* Extract the item value from the send buffer (value points TO the item data) */
        intptr_t item_val = 0;
        memcpy(&item_val, value, value_size < sizeof(intptr_t) ? value_size : sizeof(intptr_t));
        ch->on_reset.fn(ch->on_reset.env, item_val);
    }
    
    /* Deadline scope: if caller installed a current deadline, use deadline-aware send. */
    if (cc__tls_current_deadline) {
        return cc_chan_deadline_send(ch, value, value_size, cc__tls_current_deadline);
    }
    
    /* Lock-free fast path for buffered channels.
     * Large by-value elements stay lock-free only on the ring backend. */
    if (ch->use_lockfree && ch->cap > 0 && ch->elem_size == value_size && ch->buf &&
        (ch->use_ring_queue || ch->elem_size <= sizeof(void*))) {
        /* Check closed flag (relaxed read is fine, we'll verify under lock if needed) */
        if (ch->closed) return EPIPE;
        /* Check rx error closed (upstream error propagation) */
        if (ch->rx_error_closed) return ch->rx_error_code;
        
        /* Direct handoff: if receivers waiting, give item directly to one.
         * This must be done under lock to coordinate with the fair queue. */
        if (atomic_load_explicit(&ch->has_recv_waiters, memory_order_acquire)) {
            cc__chan_trace_req_wake(ch, "send_direct_handoff_check", value, NULL);
            cc_chan_lock(ch);
            if (ch->closed) { pthread_mutex_unlock(&ch->mu); return EPIPE; }
            if (ch->rx_error_closed) { pthread_mutex_unlock(&ch->mu); return ch->rx_error_code; }
            cc__fiber_wait_node* rnode = cc__chan_pop_recv_waiter(ch);
            if (rnode) {
                /* Direct handoff to waiting receiver */
                channel_store_slot(rnode->data, value, ch->elem_size);
                if (ch->use_lockfree) {
                }
                atomic_store_explicit(&rnode->notified, CC_CHAN_NOTIFY_DATA, memory_order_release);
                if (rnode->is_select) cc__chan_select_dbg_inc(&g_dbg_select_data_set);
                /* IMPORTANT: Increment signaled BEFORE waking the fiber. */
                if (rnode->is_select && rnode->select_group) {
                    cc__select_wait_group* group = (cc__select_wait_group*)rnode->select_group;
                    atomic_fetch_add_explicit(&group->signaled, 1, memory_order_release);
                }
                if (rnode->fiber) {
                    wake_batch_add_chan(ch, rnode->fiber);
                } else {
                    pthread_cond_signal(&ch->not_empty);
                }
                cc__chan_trace_req_wake(ch, "send_direct_handoff", value, rnode);
                /* Do NOT signal remaining receivers here — the handoff
                 * consumed the item, so the post-enqueue bounded wake path
                 * does not apply here. */
                pthread_mutex_unlock(&ch->mu);
                wake_batch_flush();
                cc__chan_signal_recv_ready(ch);
                return 0;
            }
            pthread_mutex_unlock(&ch->mu);
        }
        
        /* Try lock-free enqueue to buffer (fast path, no inflight) */
        int rc = cc__chan_enqueue_lockfree_fast(ch, value, NULL);
        if (rc == 0) {
            cc__chan_post_lockfree_enqueue_signal_receivers(ch, value,
                                                            "send_enqueue_seen_waiter");
            return 0;
        }
        /* Lock-free enqueue failed (queue full) - handle mode */
        if (ch->mode == CC_CHAN_MODE_DROP_NEW) {
            return EAGAIN;
        }
        /* DROP_OLD or BLOCK mode: fall through to blocking path */
    }
    
    /* Unbuffered channels: check closed before mutex path */
    if (ch->cap == 0 && ch->closed) return EPIPE;
    if (ch->cap == 0 && ch->rx_error_closed) return ch->rx_error_code;
    
    /* Standard mutex path (unbuffered, initial setup, or lock-free full) */
    cc_chan_lock(ch);
    int err = cc_chan_ensure_buf(ch, value_size);
    if (err != 0) { pthread_mutex_unlock(&ch->mu); return err; }
    if (ch->closed) {
        pthread_mutex_unlock(&ch->mu);
        return EPIPE;
    }
    if (ch->rx_error_closed) { pthread_mutex_unlock(&ch->mu); return ch->rx_error_code; }
    
    /* Unbuffered (rendezvous) channel - direct handoff */
    if (ch->cap == 0) {
        err = cc_chan_send_unbuffered(ch, value, NULL);
        pthread_mutex_unlock(&ch->mu);
        wake_batch_flush();
        if (err == 0) cc__chan_signal_recv_ready(ch);
        return err;
    }
    
    /* Buffered channel - try lock-free again under mutex (for initial setup case) */
    if (ch->use_lockfree && (ch->use_ring_queue || ch->elem_size <= sizeof(void*))) {
        pthread_mutex_unlock(&ch->mu);
        int rc = cc_chan_try_enqueue_lockfree(ch, value);
        if (rc == 0) {
            cc_chan_lock(ch);
            cc__chan_trace_req_wake(ch, "send_try_enqueue_signal", value, NULL);
            cc__chan_signal_recv_waiter(ch);
            pthread_cond_signal(&ch->not_empty);
            pthread_mutex_unlock(&ch->mu);
            wake_batch_flush();
            cc__chan_signal_recv_ready(ch);
            return 0;
        }
        /* Still full - need to wait */
        cc_chan_lock(ch);
    }
    
    /* Blocking path for lock-free channels */
    if (ch->use_lockfree && (ch->use_ring_queue || ch->elem_size <= sizeof(void*))) {
        /* For lock-free channels, wait using count approximation */
        
        cc__fiber* fiber = cc__fiber_in_context() ? cc__fiber_current() : NULL;
        
        while (!ch->closed) {
            /* Try lock-free enqueue */
            /* Increment inflight BEFORE unlocking to prevent drain race.
             * If we unlock first, drain might see inflight=0 and queue empty,
             * returning EPIPE before we enqueue. */
            chan_inflight_inc(ch);
            pthread_mutex_unlock(&ch->mu);
            int rc = cc__chan_try_enqueue_lockfree_impl(ch, value);
            chan_inflight_dec(ch);
            if (rc == 0) {
                
                cc_chan_lock(ch);
                cc__chan_trace_req_wake(ch, "send_blocking_enqueue_signal", value, NULL);
                cc__chan_signal_recv_waiter(ch);
                pthread_cond_signal(&ch->not_empty);
                pthread_mutex_unlock(&ch->mu);
                wake_batch_flush();
                cc__chan_signal_recv_ready(ch);
                return 0;
            }
            cc_chan_lock(ch);
            
        /* Wait for space */
        if (fiber) {
            cc__fiber_wait_node node = {0};
            node.fiber = fiber;
            atomic_store(&node.notified, 0);
            cc__chan_add_send_waiter(ch, &node);
            pthread_mutex_unlock(&ch->mu);
            /* Dekker pair with cc_chan_recv consumer:
             *   producer:  publish has_send_waiters=1  ; load cell->seq
             *   consumer:  store cell->seq+=cap        ; load has_send_waiters
             * On arm64 the unlock above is a release op on the mutex but does
             * NOT order our subsequent acquire-load of cell->seq inside
             * try_enqueue against another core's load of has_send_waiters.
             * Without this fence both sides can observe the pre-publish state
             * of the other and the sender parks on a queue that just became
             * writable. See the matching fence on the consumer side in the
             * lock-free dequeue paths in cc_chan_recv. */
            atomic_thread_fence(memory_order_seq_cst);
            /* Re-check enqueue before parking to avoid missed wakeups.
             * This authoritative queue op replaces the old count heuristic. */
            chan_inflight_inc(ch);
            int rc = cc__chan_try_enqueue_lockfree_impl(ch, value);
            chan_inflight_dec(ch);
                if (rc == 0) {
                    cc_chan_lock(ch);
                    cc__chan_remove_send_waiter(ch, &node);
                    cc__chan_signal_recv_waiter(ch);
                    pthread_cond_signal(&ch->not_empty);
                    pthread_mutex_unlock(&ch->mu);
                    wake_batch_flush();
                    cc__chan_signal_recv_ready(ch);
                    return 0;
                }
                /* Dekker pre-park: wake any parked receiver before we sleep.
                 * has_send_waiters is already set (from add_send_waiter above),
                 * so a receiver arriving later will see our flag and wake us. */
                if (atomic_load_explicit(&ch->has_recv_waiters, memory_order_acquire)) {
                    cc_chan_lock(ch);
                    cc__chan_signal_recv_waiter(ch);
                    pthread_cond_signal(&ch->not_empty);
                    cc_chan_unlock(ch);
                    wake_batch_flush();
                }
                (void)cc__chan_wait_notified_mark_close(&node, NULL, "chan_send_wait_full", ch);
                pthread_mutex_lock(&ch->mu);
                int notified = atomic_load_explicit(&node.notified, memory_order_acquire);
                if (notified == CC_CHAN_NOTIFY_SIGNAL) {
                    if (ch->use_lockfree) {
                    }
                    atomic_store_explicit(&node.notified, CC_CHAN_NOTIFY_NONE, memory_order_release);
                    cc__chan_remove_send_waiter(ch, &node);
                    /* After waking, try to enqueue before checking closed.
                     * Close only prevents NEW work - a sender already waiting
                     * should complete if there's space. */
                    /* Increment inflight BEFORE unlocking to prevent drain race. */
                    chan_inflight_inc(ch);
                    pthread_mutex_unlock(&ch->mu);
                    int rc = cc__chan_try_enqueue_lockfree_impl(ch, value);
                    chan_inflight_dec(ch);
                    if (rc == 0) {
                        cc_chan_lock(ch);
                        cc__chan_signal_recv_waiter(ch);
                        pthread_cond_signal(&ch->not_empty);
                        pthread_mutex_unlock(&ch->mu);
                        wake_batch_flush();
                        cc__chan_signal_recv_ready(ch);
                        return 0;
                    }
                    /* Enqueue failed after wake — buffer still full.
                     * Loop back: the top of while() will retry enqueue,
                     * do the bounded yield-retry, then re-park if needed. */
                    cc_chan_lock(ch);
                    continue;
                }
                if (notified == CC_CHAN_NOTIFY_CLOSE) {
                    cc__chan_remove_send_waiter(ch, &node);
                    continue;
                }
                if (!notified) {
                    if (ch->use_lockfree) {
                    }
                    cc__chan_remove_send_waiter(ch, &node);
                }
            } else {
                pthread_cond_wait(&ch->not_full, &ch->mu);
            }
        }
        
        /* Channel closed - but try one more enqueue in case there's space.
         * This handles the race where close happens right as we're woken. */
        /* Increment inflight BEFORE unlocking to prevent drain race.
         * If we don't, drain might see inflight=0 and return EPIPE before we enqueue. */
        chan_inflight_inc(ch);
        pthread_mutex_unlock(&ch->mu);
        int rc = cc__chan_try_enqueue_lockfree_impl(ch, value);
        chan_inflight_dec(ch);
        if (rc == 0) {
            cc_chan_lock(ch);
            cc__chan_signal_recv_waiter(ch);
            pthread_cond_signal(&ch->not_empty);
            pthread_mutex_unlock(&ch->mu);
            wake_batch_flush();
            cc__chan_signal_recv_ready(ch);
            return 0;
        }
        return EPIPE;
    }
    
    /* Original mutex-based path for non-lock-free channels */
    if (ch->count == ch->cap) {
        err = cc_chan_handle_full_send(ch, value, NULL);
        if (err != 0) { pthread_mutex_unlock(&ch->mu); return err; }
    }
    cc_chan_enqueue(ch, value);
    pthread_mutex_unlock(&ch->mu);
    wake_batch_flush();  /* Flush any pending fiber wakes */
    cc__chan_signal_recv_ready(ch);
    return 0;
}

/* Owned channel (pool) recv: try to get from pool, or create if empty and under capacity */
static int cc_chan_recv_owned(CCChan* ch, void* out_value, size_t value_size) {
    if (!ch || !out_value || value_size == 0) return EINVAL;
    
    /* First, try non-blocking recv from the pool */
    int rc = cc_chan_try_recv(ch, out_value, value_size);
    if (rc == 0) {
        return 0;  /* Got item from pool */
    }
    
    /* Pool is empty (EAGAIN) or closed (EPIPE) - check if we can create */
    if (rc == EAGAIN) {
        pthread_mutex_lock(&ch->mu);
        
        /* Double-check: try to dequeue under lock in case of race */
        if (ch->use_lockfree && (ch->use_ring_queue || ch->elem_size <= sizeof(void*))) {
            pthread_mutex_unlock(&ch->mu);
            rc = cc_chan_try_recv(ch, out_value, value_size);
            if (rc == 0) return 0;
            pthread_mutex_lock(&ch->mu);
        } else if (ch->count > 0) {
            /* Mutex path: there are items, dequeue one */
            cc_chan_dequeue(ch, out_value);
            pthread_mutex_unlock(&ch->mu);
            return 0;
        }
        
        /* Still empty - can we create a new item? */
        if (ch->items_created < ch->max_items && ch->on_create.fn) {
            ch->items_created++;
            pthread_mutex_unlock(&ch->mu);
            
            /* Call on_create to get new item.
             * Return value semantics: the return value IS the item.
             * - For pointer pools (void*[~N owned]): returns the pointer value
             * - For struct pools: returns pointer to static/heap data to copy from
             * We copy up to value_size bytes treating returned pointer as item value. */
            void* created = ch->on_create.fn(ch->on_create.env);
            /* The return value IS the item - copy it directly */
            memcpy(out_value, &created, value_size < sizeof(void*) ? value_size : sizeof(void*));
            return 0;
        }
        
        pthread_mutex_unlock(&ch->mu);
        
        /* At capacity, must wait for item to be returned - use normal blocking recv */
        return -1;
    }
    
    /* Other error (EPIPE for closed channel) */
    return rc;
}

int cc_chan_recv(CCChan* ch, void* out_value, size_t value_size) {
    /* Minimal fast path: branded channel, just dequeue and return.
     * Skips guards, debug, timing, signal_activity.
     * Checks send_waiters_head to wake parked senders (pipeline correctness). */
    if (ch->fast_path_ok && value_size == ch->elem_size) {
        if (cc__chan_mutex_minimal_enabled()) {
            if (cc__chan_dequeue_mutex_minimal(ch, out_value) == 0) {
                return 0;
            }
        } else {
            if (cc__chan_dequeue_lockfree_minimal(ch, out_value, NULL) == 0) {
                /* Dekker pair with cc_chan_send producer: after the lock-free
                 * dequeue, force the load of has_send_waiters to be ordered
                 * AFTER the dequeue's effects on cell->seq. Without this fence
                 * arm64 may reorder the load before the dequeue is visible to
                 * the producer, while the producer (after publishing the
                 * waiter flag) may load cell->seq before our store of
                 * cell->seq is visible. Both sides observe the pre-publish
                 * state of the other and the sender parks on a queue that has
                 * just become writable. See cc_chan_send for the matching
                 * fence on the producer side. */
                atomic_thread_fence(memory_order_seq_cst);
                if (__builtin_expect(atomic_load_explicit(&ch->has_send_waiters, memory_order_acquire) != 0, 0)) {
                    cc_chan_lock(ch);
                    cc__chan_wake_one_send_waiter(ch);
                    pthread_cond_signal(&ch->not_full);
                    pthread_mutex_unlock(&ch->mu);
                    wake_batch_flush();
                }
                return 0;
            }
        }
        /* Buffer empty — fall through to full path for yield-retry / blocking */
    }
    if (!ch || !out_value || value_size == 0) return EINVAL;
    
    /* Owned channel (pool) special handling */
    if (ch->is_owned) {
        int rc = cc_chan_recv_owned(ch, out_value, value_size);
        if (rc != -1) return rc;  /* -1 means "use normal recv" */
    }
    
    /* Deadline scope: if caller installed a current deadline, use deadline-aware recv. */
    if (cc__tls_current_deadline) {
        return cc_chan_deadline_recv(ch, out_value, value_size, cc__tls_current_deadline);
    }
    
    /* Lock-free fast path for buffered channels.
     * Large by-value elements stay lock-free only on the ring backend. */
    if (ch->use_lockfree && ch->cap > 0 && ch->elem_size == value_size && ch->buf &&
        (ch->use_ring_queue || ch->elem_size <= sizeof(void*))) {
        int rc = cc__chan_dequeue_lockfree_fast(ch, out_value, NULL);
        if (rc == 0) {
            /* Signal send waiters — use atomic Dekker flag, not the
             * mutex-protected send_waiters_head.  See the note at the
             * minimal-path equivalent above: this fence is the receiver's
             * half of the Dekker pair with cc_chan_send. */
            atomic_thread_fence(memory_order_seq_cst);
            if (atomic_load_explicit(&ch->has_send_waiters, memory_order_acquire)) {
                cc_chan_lock(ch);
                cc__chan_wake_one_send_waiter(ch);
                pthread_cond_signal(&ch->not_full);
                pthread_mutex_unlock(&ch->mu);
                wake_batch_flush();
            } else {
            }
            cc__chan_signal_activity(ch);
            return 0;
        }
        if (ch->closed) {
            return cc__chan_try_drain_lockfree_on_close(ch, out_value, NULL);
        }
        /* Fall through to blocking path */
    }
    
    /* Standard mutex path */
    pthread_mutex_lock(&ch->mu);
    int err = cc_chan_ensure_buf(ch, value_size);
    if (err != 0) { pthread_mutex_unlock(&ch->mu); return err; }
    
    /* Unbuffered rendezvous: direct handoff */
    if (ch->cap == 0) {
        err = cc_chan_recv_unbuffered(ch, out_value, NULL);
        pthread_mutex_unlock(&ch->mu);
        wake_batch_flush();
        if (err == 0) cc__chan_signal_recv_ready(ch);
        return err;
    }

    /* Buffered or initial setup - use existing wait logic.
     * Large elements fall back only when the lock-free backend cannot store them. */
    if (!ch->use_lockfree || (!ch->use_ring_queue && ch->elem_size > sizeof(void*))) {
        err = cc_chan_wait_empty(ch, NULL);
        if (err != 0) { pthread_mutex_unlock(&ch->mu); return err; }
        cc_chan_dequeue(ch, out_value);
        /* Wake a sender waiting for space (buffered channel). */
        cc__chan_wake_one_send_waiter(ch);
        pthread_cond_signal(&ch->not_full);
        pthread_mutex_unlock(&ch->mu);
        wake_batch_flush();
        cc__chan_signal_activity(ch);
        return 0;
    }
    
    /* Lock-free buffered channel with small elements - blocking wait for data */
    
    cc__fiber* fiber = cc__fiber_in_context() ? cc__fiber_current() : NULL;
    
    /* Runtime guard (opt-in): blocking recv on an autoclose channel from inside the same nursery
       is a common deadlock foot-gun (recv-until-close inside the nursery). */
    if (!ch->closed && ch->autoclose_owner && cc__tls_current_nursery &&
        ch->autoclose_owner == cc__tls_current_nursery) {
        const char* g = getenv("CC_NURSERY_CLOSING_RUNTIME_GUARD");
        if (g && g[0] == '1') {
            if (!ch->warned_autoclose_block) {
                ch->warned_autoclose_block = 1;
                fprintf(stderr,
                        "CC: runtime guard: blocking cc_chan_recv() on a `closing(...)` channel from inside the same nursery "
                        "may deadlock (use a sentinel/explicit close, or drain outside the nursery)\n");
            }
            pthread_mutex_unlock(&ch->mu);
            return EDEADLK;
        }
    }
    
    while (1) {
        /* Try lock-free dequeue (fast path, guards already checked) */
        pthread_mutex_unlock(&ch->mu);
        int rc = cc__chan_dequeue_lockfree_fast(ch, out_value, NULL);
        if (rc == 0) {
            cc_chan_lock(ch);
            cc__chan_wake_one_send_waiter(ch);
            pthread_cond_signal(&ch->not_full);
            pthread_mutex_unlock(&ch->mu);
            wake_batch_flush();
            cc__chan_signal_activity(ch);
            return 0;
        }
        pthread_mutex_lock(&ch->mu);

        if (ch->closed) break;
        
        /* Check if current nursery is cancelled - unblock so the fiber can exit */
        {
            CCNursery* cur_nursery = cc__tls_current_nursery;
            if (cur_nursery && cc_nursery_is_cancelled(cur_nursery)) {
                pthread_mutex_unlock(&ch->mu);
                return ECANCELED;
            }
        }
        
        /* Re-check deadlock guard inside the loop: the initial guard above
         * may have passed when items were still available.  Now the buffer is
         * empty and the producer may be done -- detect the foot-gun. */
        if (!ch->closed && ch->autoclose_owner && cc__tls_current_nursery &&
            ch->autoclose_owner == cc__tls_current_nursery) {
            const char* g = getenv("CC_NURSERY_CLOSING_RUNTIME_GUARD");
            if (g && g[0] == '1') {
                if (!ch->warned_autoclose_block) {
                    ch->warned_autoclose_block = 1;
                    fprintf(stderr,
                            "CC: runtime guard: blocking cc_chan_recv() on a `closing(...)` channel from inside the same nursery "
                            "may deadlock (use a sentinel/explicit close, or drain outside the nursery)\n");
                }
                pthread_mutex_unlock(&ch->mu);
                return EDEADLK;
            }
        }
        
        /* Wait for data */
        if (fiber) {
            cc__fiber_wait_node node = {0};
            node.fiber = fiber;
            node.data = out_value;  /* For direct handoff */
            atomic_store(&node.notified, 0);
            cc__chan_add_recv_waiter(ch, &node);
            pthread_mutex_unlock(&ch->mu);
            if (cc__chan_lf_count(ch) > 0) {
                /* A sender may have already popped our node and done a direct
                 * handoff (notified=DATA) between add_recv_waiter and here.
                 * If so, the data is already in out_value - return it instead
                 * of continuing to the top of the loop where it would be lost. */
                int early = atomic_load_explicit(&node.notified, memory_order_acquire);
                if (early == CC_CHAN_NOTIFY_DATA) {
                    if (ch->use_lockfree) {
                    }
                    return 0;
                }
                cc_chan_lock(ch);
                /* Re-check notified under lock: a sender may have popped
                 * our node and written DATA between line 2622 and here.
                 * If so, the data is already in out_value -- return it. */
                {
                    int late = atomic_load_explicit(&node.notified, memory_order_acquire);
                    if (late == CC_CHAN_NOTIFY_DATA) {
                        pthread_mutex_unlock(&ch->mu);
                        if (ch->use_lockfree) {
                        }
                        return 0;
                    }
                }
                cc__chan_remove_recv_waiter(ch, &node);
                /* Mutex must be held at top of while(1) since line 2624
                 * does pthread_mutex_unlock as its first action. */
                continue;
            }
            /* Re-check dequeue before parking to avoid missed wakeups. */
            /* Check for direct handoff before trying dequeue - a sender may
             * have popped our node and written directly to out_value.  If we
             * dequeue from the buffer now, we'd overwrite that data. */
            {
                int early2 = atomic_load_explicit(&node.notified, memory_order_acquire);
                if (early2 == CC_CHAN_NOTIFY_DATA) {
                    if (ch->use_lockfree) {
                    }
                    return 0;
                }
            }
            /* Try lock-free dequeue before parking.
             *
             * Key invariant: the node must be on the wait list at all
             * times when we are about to park, so signal_recv_waiter
             * can find us.  We take the mutex, check notified (DATA
             * means direct handoff already completed), then check count.
             * If count>0 we remove the node under lock and try dequeue.
             * If dequeue fails (CAS contention) we re-add under lock
             * and loop — no gap where the node is off the list while
             * we could miss a signal. */
            {
                cc_chan_lock(ch);
                int pre_deq_notified = atomic_load_explicit(&node.notified, memory_order_acquire);
                cc__chan_trace_req_recv(ch, "pre_deq_check", &node, pre_deq_notified, 0);
                if (pre_deq_notified == CC_CHAN_NOTIFY_DATA) {
                    /* Direct handoff — data already in out_value */
                    if (ch->use_lockfree) {
                    }
                    pthread_mutex_unlock(&ch->mu);
                    return 0;
                }
                if (pre_deq_notified != 0 || ch->closed) {
                    /* SIGNAL/CLOSE — remove node and retry from loop top */
                    cc__chan_trace_req_recv(ch, "pre_deq_remove_retry", &node, pre_deq_notified, ch->closed ? EPIPE : 0);
                    cc__chan_remove_recv_waiter(ch, &node);
                    /* mutex held — top of while(1) unlocks */
                    continue;
                }
                /* notified==0: safe to dequeue (no direct handoff risk).
                 * Try an authoritative dequeue instead of relying on the
                 * approximate cc__chan_lf_count (which uses relaxed loads of
                 * ring_tail/ring_head and can return a stale 0 even when the
                 * ring has data).  The actual dequeue checks cell->seq with
                 * acquire ordering and is definitive. */
                cc__chan_remove_recv_waiter(ch, &node);
                pthread_mutex_unlock(&ch->mu);
                {
                    int probe_rc = cc__chan_dequeue_lockfree_fast(ch, out_value, NULL);
                    cc__chan_trace_req_recv(ch, "pre_park_probe_deq", &node, pre_deq_notified, probe_rc);
                    if (probe_rc == 0) {
                        cc_chan_lock(ch);
                        cc__chan_wake_one_send_waiter(ch);
                        pthread_cond_signal(&ch->not_full);
                        pthread_mutex_unlock(&ch->mu);
                        wake_batch_flush();
                        cc__chan_signal_activity(ch);
                        return 0;
                    }
                }
                /* Buffer truly empty — re-register on wait list and park. */
                pthread_mutex_lock(&ch->mu);
                /* Close may have fired while we were off the wait list.
                 * wake_all_waiters wouldn't have found us, so check now. */
                if (ch->closed) {
                    pthread_mutex_unlock(&ch->mu);
                    return EPIPE;
                }
                /* Re-check notified: a sender may have done a direct handoff
                 * while we were off the list. */
                {
                    int recheck_n = atomic_load_explicit(&node.notified, memory_order_acquire);
                    if (recheck_n == CC_CHAN_NOTIFY_DATA) {
                        pthread_mutex_unlock(&ch->mu);
                        return 0;
                    }
                    if (recheck_n != 0) {
                        /* SIGNAL or CLOSE while off-list — retry from loop top */
                        continue;
                    }
                }
                atomic_store(&node.notified, 0);
                cc__chan_add_recv_waiter(ch, &node);
                pthread_mutex_unlock(&ch->mu);
                if (atomic_load_explicit(&ch->has_send_waiters, memory_order_acquire)) {
                    cc_chan_lock(ch);
                    cc__chan_wake_one_send_waiter(ch);
                    pthread_cond_signal(&ch->not_full);
                    cc_chan_unlock(ch);
                    wake_batch_flush();
                }
                cc__chan_trace_req_recv(ch, "park_wait_begin", &node,
                                        atomic_load_explicit(&node.notified, memory_order_relaxed), 0);
                (void)cc__chan_wait_notified_mark_close(&node, NULL, "chan_recv_wait_empty", ch);
                pthread_mutex_lock(&ch->mu);
                goto recv_post_park_notified;
            }
        recv_post_park_notified: ;
            int notified = atomic_load_explicit(&node.notified, memory_order_acquire);
            cc__chan_trace_req_recv(ch, "post_park_notified", &node, notified, 0);
            if (notified == CC_CHAN_NOTIFY_SIGNAL) {
                if (ch->use_lockfree) {
                }
                cc__chan_remove_recv_waiter(ch, &node);
                atomic_store_explicit(&node.notified, CC_CHAN_NOTIFY_NONE, memory_order_release);
                cc__chan_trace_req_recv(ch, "post_signal_remove_retry", &node, notified, 0);
                continue;
            }
            if (notified == CC_CHAN_NOTIFY_DATA) {
                if (ch->use_lockfree) {
                }
                /* Sender did direct handoff - data is already in out_value */
                pthread_mutex_unlock(&ch->mu);
                return 0;
            }
            if (notified == CC_CHAN_NOTIFY_CLOSE || ch->closed) {
                if (ch->use_lockfree) {
                }
                /* Channel closed while we were waiting - drain in-flight sends before returning EPIPE. */
                cc__chan_remove_recv_waiter(ch, &node);
                pthread_mutex_unlock(&ch->mu);
                int rc = cc__chan_try_drain_lockfree_on_close(ch, out_value, NULL);
                return rc;
            }
            /* notified == 0: spurious wakeup or early wake via pending_unpark.
             * But a sender might have popped us and delivered data between our
             * initial notified check and now. Re-check with acquire semantics. */
            int recheck = atomic_load_explicit(&node.notified, memory_order_acquire);
            cc__chan_trace_req_recv(ch, "post_spurious_recheck", &node, recheck, 0);
            if (recheck == CC_CHAN_NOTIFY_DATA) {
                /* Data was delivered after all! */
                if (ch->use_lockfree) {
                }
                pthread_mutex_unlock(&ch->mu);
                return 0;
            }
            {
                cc__chan_remove_recv_waiter(ch, &node);
                cc__chan_trace_req_recv(ch, "post_spurious_remove", &node, recheck, 0);
                if (ch->use_lockfree) {
                }
            }
        } else {
            cc__chan_thread_recv_waiter_inc(ch);
            if (ch->closed) {
                cc__chan_thread_recv_waiter_dec(ch);
                break;
            }
            if (cc__chan_lf_count(ch) > 0) {
                cc__chan_thread_recv_waiter_dec(ch);
                continue;
            }
            pthread_cond_wait(&ch->not_empty, &ch->mu);
            cc__chan_thread_recv_waiter_dec(ch);
        }
    }
    
    /* Channel closed - drain in-flight sends before returning EPIPE */
    pthread_mutex_unlock(&ch->mu);
    int rc = cc__chan_try_drain_lockfree_on_close(ch, out_value, NULL);
    return rc;
}


int cc_chan_try_send(CCChan* ch, const void* value, size_t value_size) {
    if (!ch || !value || value_size == 0) return EINVAL;
    
    /* Lock-free fast path for buffered channels.
     * Large by-value elements stay lock-free only on the ring backend. */
    if (ch->use_lockfree && ch->cap > 0 && ch->elem_size == value_size && ch->buf &&
        (ch->use_ring_queue || ch->elem_size <= sizeof(void*))) {
        /* Manually manage inflight to cover the gap between checking closed and enqueueing */
        chan_inflight_inc(ch);
        if (ch->closed) {
            chan_inflight_dec(ch);
            return EPIPE;
        }
        if (ch->rx_error_closed) {
            chan_inflight_dec(ch);
            return ch->rx_error_code;
        }
        int rc = cc__chan_try_enqueue_lockfree_impl(ch, value);
        chan_inflight_dec(ch);
        if (rc == 0) {
            int fiber_waiters = atomic_load_explicit(&ch->has_recv_waiters, memory_order_acquire);
            int thread_waiters = atomic_load_explicit(&ch->thread_recv_waiters, memory_order_acquire);
            if (fiber_waiters || thread_waiters) {
                cc_chan_lock(ch);
                cc__chan_signal_recv_waiter(ch);
                if (thread_waiters) pthread_cond_signal(&ch->not_empty);
                pthread_mutex_unlock(&ch->mu);
                wake_batch_flush();
                cc__chan_signal_recv_ready(ch);
            } else if (ch->recv_signal) {
                cc_socket_signal_signal(ch->recv_signal);
            }
            return 0;
        }
        return EAGAIN;
    }
    
    /* Unbuffered channels: check closed before mutex path */
    if (ch->cap == 0 && ch->closed) return EPIPE;
    if (ch->rx_error_closed) return ch->rx_error_code;
    
    /* Standard mutex path */
    pthread_mutex_lock(&ch->mu);
    int err = cc_chan_ensure_buf(ch, value_size);
    if (err != 0) { pthread_mutex_unlock(&ch->mu); return err; }
    if (ch->closed) { pthread_mutex_unlock(&ch->mu); return EPIPE; }
    if (ch->rx_error_closed) { pthread_mutex_unlock(&ch->mu); return ch->rx_error_code; }
    if (ch->cap == 0) {
        /* Non-blocking rendezvous: only send if a receiver is waiting. */
        cc__fiber_wait_node* rnode = cc__chan_pop_recv_waiter(ch);
        if (!rnode) {
            pthread_mutex_unlock(&ch->mu);
            if (ch->rx_error_closed) return ch->rx_error_code;
            return ch->closed ? EPIPE : EAGAIN;
        }
        channel_store_slot(rnode->data, value, ch->elem_size);
        atomic_store_explicit(&rnode->notified, CC_CHAN_NOTIFY_DATA, memory_order_release);
        if (rnode->is_select) cc__chan_select_dbg_inc(&g_dbg_select_data_set);
        if (ch->rv_recv_waiters > 0) ch->rv_recv_waiters--;
        /* IMPORTANT: Increment signaled BEFORE waking the fiber. */
        if (rnode->is_select && rnode->select_group) {
            cc__select_wait_group* group = (cc__select_wait_group*)rnode->select_group;
            atomic_fetch_add_explicit(&group->signaled, 1, memory_order_release);
        }
        if (rnode->fiber) {
            wake_batch_add_chan(ch, rnode->fiber);
        } else {
            pthread_cond_signal(&ch->not_empty);
        }
        pthread_mutex_unlock(&ch->mu);
        wake_batch_flush();
        cc__chan_signal_recv_ready(ch);
        return 0;
    }
    
    /* Buffered with lock-free: try lock-free first */
    if (ch->use_lockfree && (ch->use_ring_queue || ch->elem_size <= sizeof(void*))) {
        /* Increment inflight BEFORE unlocking to prevent drain race. */
        chan_inflight_inc(ch);
        pthread_mutex_unlock(&ch->mu);
        int rc = cc__chan_try_enqueue_lockfree_impl(ch, value);
        chan_inflight_dec(ch);
        if (rc == 0) {
            int fiber_waiters = atomic_load_explicit(&ch->has_recv_waiters, memory_order_acquire);
            int thread_waiters = atomic_load_explicit(&ch->thread_recv_waiters, memory_order_acquire);
            if (fiber_waiters || thread_waiters) {
                cc_chan_lock(ch);
                cc__chan_signal_recv_waiter(ch);
                if (thread_waiters) pthread_cond_signal(&ch->not_empty);
                pthread_mutex_unlock(&ch->mu);
                wake_batch_flush();
                cc__chan_signal_recv_ready(ch);
            } else if (ch->recv_signal) {
                cc_socket_signal_signal(ch->recv_signal);
            }
            return 0;
        }
        return EAGAIN;
    }
    
    if (ch->count == ch->cap) {
        err = cc_chan_handle_full_send(ch, value, NULL);
        if (err != 0) { pthread_mutex_unlock(&ch->mu); return err; }
    }
    cc_chan_enqueue(ch, value);
    /* Wake a receiver waiting for data (buffered channel). */
    cc__chan_signal_recv_waiter(ch);
    pthread_cond_signal(&ch->not_empty);
    pthread_mutex_unlock(&ch->mu);
    wake_batch_flush();
    cc__chan_signal_recv_ready(ch);
    return 0;
}

int cc_chan_try_recv(CCChan* ch, void* out_value, size_t value_size) {
    if (!ch || !out_value || value_size == 0) return EINVAL;
    
    /* Lock-free fast path for buffered channels.
     * Large by-value elements stay lock-free only on the ring backend. */
    if (ch->use_lockfree && ch->cap > 0 && ch->elem_size == value_size && ch->buf &&
        (ch->use_ring_queue || ch->elem_size <= sizeof(void*))) {
        int rc = cc_chan_try_dequeue_lockfree(ch, out_value);
        if (rc == 0) {
            /* try_recv is a poll operation: caller is already active.
             * Only wake parked senders; skip signal and broadcast. */
            int send_waiters = atomic_load_explicit(&ch->has_send_waiters, memory_order_acquire);
            if (send_waiters) {
                cc_chan_lock(ch);
                cc__chan_wake_one_send_waiter(ch);
                pthread_cond_signal(&ch->not_full);
                pthread_mutex_unlock(&ch->mu);
                wake_batch_flush();
                cc__chan_signal_activity(ch);
            }
            return 0;
        }
        if (ch->closed) {
            if (atomic_load_explicit(&ch->lfqueue_inflight, memory_order_acquire) > 0) {
                return EAGAIN;
            }
            return ch->tx_error_code ? ch->tx_error_code : EPIPE;
        }
        return EAGAIN;
    }
    
    /* Standard mutex path */
    pthread_mutex_lock(&ch->mu);
    int err = cc_chan_ensure_buf(ch, value_size);
    if (err != 0) { pthread_mutex_unlock(&ch->mu); return err; }
    if (ch->cap == 0) {
        cc__fiber_wait_node* snode = cc__chan_pop_send_waiter(ch);
        if (!snode) {
            pthread_mutex_unlock(&ch->mu);
            return ch->closed ? (ch->tx_error_code ? ch->tx_error_code : EPIPE) : EAGAIN;
        }
        channel_load_slot(snode->data, out_value, ch->elem_size);
        atomic_store_explicit(&snode->notified, CC_CHAN_NOTIFY_DATA, memory_order_release);
        if (snode->is_select) cc__chan_select_dbg_inc(&g_dbg_select_data_set);
        /* IMPORTANT: Increment signaled BEFORE waking the fiber. */
        if (snode->is_select && snode->select_group) {
            cc__select_wait_group* group = (cc__select_wait_group*)snode->select_group;
            atomic_fetch_add_explicit(&group->signaled, 1, memory_order_release);
        }
        if (snode->fiber) {
            wake_batch_add_chan(ch, snode->fiber);
        } else {
            pthread_cond_signal(&ch->not_full);
        }
        pthread_mutex_unlock(&ch->mu);
        wake_batch_flush();
        cc__chan_signal_activity(ch);
        return 0;
    }
    
    /* Buffered with lock-free: try lock-free first */
    if (ch->use_lockfree) {
        pthread_mutex_unlock(&ch->mu);
        int rc = cc_chan_try_dequeue_lockfree(ch, out_value);
        if (rc == 0) {
            cc_chan_lock(ch);
            cc__chan_wake_one_send_waiter(ch);
            pthread_cond_signal(&ch->not_full);
            pthread_mutex_unlock(&ch->mu);
            wake_batch_flush();
            cc__chan_signal_activity(ch);
            return 0;
        }
        return ch->closed ? (ch->tx_error_code ? ch->tx_error_code : EPIPE) : EAGAIN;
    }
    
    if (ch->count == 0) { pthread_mutex_unlock(&ch->mu); return ch->closed ? (ch->tx_error_code ? ch->tx_error_code : EPIPE) : EAGAIN; }
    cc_chan_dequeue(ch, out_value);
    pthread_mutex_unlock(&ch->mu);
    cc__chan_signal_activity(ch);
    return 0;
}

int cc_chan_timed_send(CCChan* ch, const void* value, size_t value_size, const struct timespec* abs_deadline) {
    if (!ch || !value || value_size == 0) return EINVAL;
    
    /* Lock-free fast path for buffered channels.
     * Large by-value elements stay lock-free only on the ring backend. */
    if (ch->use_lockfree && ch->cap > 0 && ch->elem_size == value_size && ch->buf &&
        (ch->use_ring_queue || ch->elem_size <= sizeof(void*))) {
        /* Manually manage inflight to cover the gap between checking closed and enqueueing */
        chan_inflight_inc(ch);
        if (ch->closed) {
            chan_inflight_dec(ch);
            return EPIPE;
        }
        if (ch->rx_error_closed) {
            chan_inflight_dec(ch);
            return ch->rx_error_code;
        }
        int rc = cc__chan_try_enqueue_lockfree_impl(ch, value);
        chan_inflight_dec(ch);
        if (rc == 0) {
            /* Always signal pthread cond for timed waiters.
             * Unlike the fiber path, timed recv uses pthread_cond_timedwait
             * without adding itself to recv_waiters_head. */
            pthread_mutex_lock(&ch->mu);
            cc__chan_signal_recv_waiter(ch);
            pthread_cond_signal(&ch->not_empty);
            pthread_mutex_unlock(&ch->mu);
            wake_batch_flush();
            cc__chan_signal_activity(ch);
            return 0;
        }
        /* Lock-free failed (queue full), fall through to blocking path */
    }
    
    pthread_mutex_lock(&ch->mu);
    int err = cc_chan_ensure_buf(ch, value_size);
    if (err != 0) { pthread_mutex_unlock(&ch->mu); return err; }
    if (ch->closed) { pthread_mutex_unlock(&ch->mu); return EPIPE; }
    if (ch->rx_error_closed) { pthread_mutex_unlock(&ch->mu); return ch->rx_error_code; }
    if (ch->cap == 0) {
        err = cc_chan_send_unbuffered(ch, value, abs_deadline);
        pthread_mutex_unlock(&ch->mu);
        wake_batch_flush();
        if (err == 0) cc__chan_signal_recv_ready(ch);
        return err;
    }
    
    /* For lock-free channels, poll while waiting */
    if (ch->use_lockfree) {
        int in_fiber = cc__fiber_in_context();
        cc__fiber* fiber_ts = in_fiber ? cc__fiber_current() : NULL;
        while (!ch->closed) {
            /* Try lock-free enqueue */
            chan_inflight_inc(ch);
            pthread_mutex_unlock(&ch->mu);
            int rc = cc__chan_try_enqueue_lockfree_impl(ch, value);
            chan_inflight_dec(ch);
            if (rc == 0) {
                cc_chan_lock(ch);
                cc__chan_signal_recv_waiter(ch);
                pthread_cond_signal(&ch->not_empty);
                pthread_mutex_unlock(&ch->mu);
                wake_batch_flush();
                cc__chan_signal_recv_ready(ch);
                return 0;
            }
            /* Check deadline */
            if (abs_deadline) {
                struct timespec now;
                clock_gettime(CLOCK_REALTIME, &now);
                if (now.tv_sec > abs_deadline->tv_sec ||
                    (now.tv_sec == abs_deadline->tv_sec && now.tv_nsec >= abs_deadline->tv_nsec)) {
                    return ETIMEDOUT;
                }
            }
            if (fiber_ts) {
                /* Register as send waiter, then use robust Dekker protocol */
                cc_chan_lock(ch);
                if (ch->closed) break;
                cc__fiber_wait_node node = {0};
                node.fiber = fiber_ts;
                atomic_store(&node.notified, 0);
                cc__chan_add_send_waiter(ch, &node);
                pthread_mutex_unlock(&ch->mu);
                /* Dekker pair with cc_chan_recv consumer (see the
                 * non-deadline path above for full rationale). Force the
                 * publish of has_send_waiters=1 to be globally ordered
                 * before the subsequent queue state load on arm64. */
                atomic_thread_fence(memory_order_seq_cst);
                /* Re-check count — a recv may have freed space since our
                 * enqueue attempt above. */
                if (cc__chan_lf_count(ch) < (int)ch->cap) {
                    cc_chan_lock(ch);
                    cc__chan_remove_send_waiter(ch, &node);
                    pthread_mutex_unlock(&ch->mu);
                    continue;
                }
                /* Retry enqueue one more time (we're registered, so any future
                 * recv will wake us). */
                chan_inflight_inc(ch);
                rc = cc__chan_try_enqueue_lockfree_impl(ch, value);
                chan_inflight_dec(ch);
                if (rc == 0) {
                    cc_chan_lock(ch);
                    cc__chan_remove_send_waiter(ch, &node);
                    cc__chan_signal_recv_waiter(ch);
                    pthread_cond_signal(&ch->not_empty);
                    pthread_mutex_unlock(&ch->mu);
                    wake_batch_flush();
                    cc__chan_signal_recv_ready(ch);
                    return 0;
                }
                /* Dekker pre-park: wake any parked receiver before we sleep.
                 * has_send_waiters is set, so a new receiver will see us. */
                if (atomic_load_explicit(&ch->has_recv_waiters, memory_order_seq_cst)) {
                    cc_chan_lock(ch);
                    cc__chan_signal_recv_waiter(ch);
                    pthread_cond_signal(&ch->not_empty);
                    cc_chan_unlock(ch);
                    wake_batch_flush();
                }
                (void)cc__chan_wait_notified_mark_close(&node, NULL, "chan_send_wait_full", ch);
                pthread_mutex_lock(&ch->mu);
                int notified = atomic_load_explicit(&node.notified, memory_order_acquire);
                if (notified == CC_CHAN_NOTIFY_SIGNAL) {
                    atomic_store_explicit(&node.notified, CC_CHAN_NOTIFY_NONE, memory_order_release);
                    cc__chan_remove_send_waiter(ch, &node);
                    /* Retry enqueue after wake */
                    chan_inflight_inc(ch);
                    pthread_mutex_unlock(&ch->mu);
                    rc = cc__chan_try_enqueue_lockfree_impl(ch, value);
                    chan_inflight_dec(ch);
                    if (rc == 0) {
                        cc_chan_lock(ch);
                        cc__chan_signal_recv_waiter(ch);
                        pthread_cond_signal(&ch->not_empty);
                        pthread_mutex_unlock(&ch->mu);
                        wake_batch_flush();
                        cc__chan_signal_activity(ch);
                        return 0;
                    }
                    cc_chan_lock(ch);
                    continue;
                }
                if (notified == CC_CHAN_NOTIFY_CLOSE) {
                    cc__chan_remove_send_waiter(ch, &node);
                    continue;
                }
                if (!notified) {
                    cc__chan_remove_send_waiter(ch, &node);
                }
                /* Not notified or close-notified — loop will check closed */
                continue;
            }
            /* Non-fiber: condvar timed wait */
            pthread_mutex_lock(&ch->mu);
            if (ch->closed) break;
            struct timespec poll_deadline;
            clock_gettime(CLOCK_REALTIME, &poll_deadline);
            poll_deadline.tv_nsec += 10000000; /* 10ms */
            if (poll_deadline.tv_nsec >= 1000000000) {
                poll_deadline.tv_nsec -= 1000000000;
                poll_deadline.tv_sec++;
            }
            const struct timespec* wait_deadline = abs_deadline;
            if (abs_deadline && (poll_deadline.tv_sec < abs_deadline->tv_sec ||
                (poll_deadline.tv_sec == abs_deadline->tv_sec && poll_deadline.tv_nsec < abs_deadline->tv_nsec))) {
                wait_deadline = &poll_deadline;
            }
            err = pthread_cond_timedwait(&ch->not_full, &ch->mu, wait_deadline ? wait_deadline : &poll_deadline);
            if (err == ETIMEDOUT && abs_deadline) {
                struct timespec now;
                clock_gettime(CLOCK_REALTIME, &now);
                if (now.tv_sec > abs_deadline->tv_sec ||
                    (now.tv_sec == abs_deadline->tv_sec && now.tv_nsec >= abs_deadline->tv_nsec)) {
                    pthread_mutex_unlock(&ch->mu);
                    return ETIMEDOUT;
                }
            }
        }
        pthread_mutex_unlock(&ch->mu);
        return EPIPE;
    }
    
    if (ch->count == ch->cap) {
        err = cc_chan_handle_full_send(ch, value, abs_deadline);
        if (err != 0) { pthread_mutex_unlock(&ch->mu); return err; }
    }
    cc_chan_enqueue(ch, value);
    pthread_mutex_unlock(&ch->mu);
    wake_batch_flush();
    cc__chan_signal_activity(ch);
    return 0;
}

int cc_chan_timed_recv(CCChan* ch, void* out_value, size_t value_size, const struct timespec* abs_deadline) {
    if (!ch || !out_value || value_size == 0) return EINVAL;
    
    /* Lock-free fast path for buffered channels.
     * Large by-value elements stay lock-free only on the ring backend. */
    if (ch->use_lockfree && ch->cap > 0 && ch->elem_size == value_size && ch->buf &&
        (ch->use_ring_queue || ch->elem_size <= sizeof(void*))) {
        int rc = cc_chan_try_dequeue_lockfree(ch, out_value);
        if (rc == 0) {
            /* Always signal pthread cond for timed waiters.
             * Unlike the fiber path, timed send uses pthread_cond_timedwait
             * without adding itself to send_waiters_head. */
            pthread_mutex_lock(&ch->mu);
            cc__chan_wake_one_send_waiter(ch);
            pthread_cond_signal(&ch->not_full);
            pthread_mutex_unlock(&ch->mu);
            wake_batch_flush();
            cc__chan_signal_activity(ch);
            return 0;
        }
        /* Check if closed and drain any in-flight sends */
        if (ch->closed) {
            return cc__chan_try_drain_lockfree_on_close(ch, out_value, abs_deadline);
        }
        /* Lock-free failed, fall through to timed wait with polling */
    }
    
    pthread_mutex_lock(&ch->mu);
    int err = cc_chan_ensure_buf(ch, value_size);
    if (err != 0) { pthread_mutex_unlock(&ch->mu); return err; }
    if (ch->cap == 0) {
        err = cc_chan_recv_unbuffered(ch, out_value, abs_deadline);
        pthread_mutex_unlock(&ch->mu);
        wake_batch_flush();
        return err;
    }
    
    /* For lock-free channels, poll while waiting */
    if (ch->use_lockfree) {
        int in_fiber_r = cc__fiber_in_context();
        cc__fiber* fiber_tr = in_fiber_r ? cc__fiber_current() : NULL;
        while (!ch->closed) {
            pthread_mutex_unlock(&ch->mu);
            int rc = cc_chan_try_dequeue_lockfree(ch, out_value);
            if (rc == 0) {
                cc_chan_lock(ch);
                cc__chan_wake_one_send_waiter(ch);
                pthread_cond_signal(&ch->not_full);
                pthread_mutex_unlock(&ch->mu);
                wake_batch_flush();
                cc__chan_signal_activity(ch);
                return 0;
            }
            /* Check deadline */
            if (abs_deadline) {
                struct timespec now;
                clock_gettime(CLOCK_REALTIME, &now);
                if (now.tv_sec > abs_deadline->tv_sec ||
                    (now.tv_sec == abs_deadline->tv_sec && now.tv_nsec >= abs_deadline->tv_nsec)) {
                    return ETIMEDOUT;
                }
            }
            if (fiber_tr) {
                /* Register as recv waiter, then use robust Dekker protocol */
                cc_chan_lock(ch);
                if (ch->closed) break;
                cc__fiber_wait_node node = {0};
                node.fiber = fiber_tr;
                atomic_store(&node.notified, 0);
                cc__chan_add_recv_waiter(ch, &node);
                pthread_mutex_unlock(&ch->mu);
                /* Re-check count */
                if (cc__chan_lf_count(ch) > 0) {
                    cc_chan_lock(ch);
                    cc__chan_remove_recv_waiter(ch, &node);
                    pthread_mutex_unlock(&ch->mu);
                    continue;
                }
                /* Retry dequeue one more time */
                rc = cc_chan_try_dequeue_lockfree(ch, out_value);
                if (rc == 0) {
                    cc_chan_lock(ch);
                    cc__chan_remove_recv_waiter(ch, &node);
                    cc__chan_wake_one_send_waiter(ch);
                    pthread_cond_signal(&ch->not_full);
                    pthread_mutex_unlock(&ch->mu);
                    wake_batch_flush();
                    cc__chan_signal_activity(ch);
                    return 0;
                }
                /* Dekker pre-park: wake any parked sender */
                if (atomic_load_explicit(&ch->has_send_waiters, memory_order_acquire)) {
                    cc_chan_lock(ch);
                    cc__chan_wake_one_send_waiter(ch);
                    pthread_cond_signal(&ch->not_full);
                    cc_chan_unlock(ch);
                    wake_batch_flush();
                }
                (void)cc__chan_wait_notified_mark_close(&node, NULL, "chan_recv_wait_empty", ch);
                pthread_mutex_lock(&ch->mu);
                int notified = atomic_load_explicit(&node.notified, memory_order_acquire);
                if (notified == CC_CHAN_NOTIFY_SIGNAL) {
                    cc__chan_remove_recv_waiter(ch, &node);
                    atomic_store_explicit(&node.notified, CC_CHAN_NOTIFY_NONE, memory_order_release);
                    /* Retry dequeue after wake */
                    pthread_mutex_unlock(&ch->mu);
                    rc = cc_chan_try_dequeue_lockfree(ch, out_value);
                    if (rc == 0) {
                        cc_chan_lock(ch);
                        cc__chan_wake_one_send_waiter(ch);
                        pthread_cond_signal(&ch->not_full);
                        pthread_mutex_unlock(&ch->mu);
                        wake_batch_flush();
                        cc__chan_signal_activity(ch);
                        return 0;
                    }
                    cc_chan_lock(ch);
                    continue;
                }
                if (notified == CC_CHAN_NOTIFY_CLOSE) {
                    cc__chan_remove_recv_waiter(ch, &node);
                    continue;
                }
                if (!notified) {
                    cc__chan_remove_recv_waiter(ch, &node);
                }
                continue;
            }
            /* Non-fiber: condvar timed wait */
            pthread_mutex_lock(&ch->mu);
            if (ch->closed) break;
            struct timespec poll_deadline;
            clock_gettime(CLOCK_REALTIME, &poll_deadline);
            poll_deadline.tv_nsec += 10000000; /* 10ms */
            if (poll_deadline.tv_nsec >= 1000000000) {
                poll_deadline.tv_nsec -= 1000000000;
                poll_deadline.tv_sec++;
            }
            const struct timespec* wait_deadline = abs_deadline;
            if (abs_deadline && (poll_deadline.tv_sec < abs_deadline->tv_sec ||
                (poll_deadline.tv_sec == abs_deadline->tv_sec && poll_deadline.tv_nsec < abs_deadline->tv_nsec))) {
                wait_deadline = &poll_deadline;
            }
            cc__chan_thread_recv_waiter_inc(ch);
            if (ch->closed) {
                cc__chan_thread_recv_waiter_dec(ch);
                break;
            }
            if (cc__chan_lf_count(ch) > 0) {
                cc__chan_thread_recv_waiter_dec(ch);
                continue;
            }
            err = pthread_cond_timedwait(&ch->not_empty, &ch->mu, wait_deadline ? wait_deadline : &poll_deadline);
            cc__chan_thread_recv_waiter_dec(ch);
            if (err == ETIMEDOUT) {
                if (abs_deadline) {
                    struct timespec now;
                    clock_gettime(CLOCK_REALTIME, &now);
                    if (now.tv_sec > abs_deadline->tv_sec ||
                        (now.tv_sec == abs_deadline->tv_sec && now.tv_nsec >= abs_deadline->tv_nsec)) {
                            if (ch->closed) {
                                pthread_mutex_unlock(&ch->mu);
                                return cc__chan_try_drain_lockfree_on_close(ch, out_value, abs_deadline);
                            }
                            pthread_mutex_unlock(&ch->mu);
                            return ETIMEDOUT;
                    }
                }
            }
        }
        pthread_mutex_unlock(&ch->mu);
        if (ch->closed) {
            return cc__chan_try_drain_lockfree_on_close(ch, out_value, abs_deadline);
        }
        return ETIMEDOUT;
    }
    
    err = cc_chan_wait_empty(ch, abs_deadline);
    if (err != 0) { pthread_mutex_unlock(&ch->mu); return err; }
    cc_chan_dequeue(ch, out_value);
    pthread_mutex_unlock(&ch->mu);
    wake_batch_flush();
    cc__chan_signal_activity(ch);
    return 0;
}

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

int cc_chan_send_take(CCChan* ch, void* ptr) {
    if (!ch) return EINVAL;
    if (!ch->allow_take) return EINVAL;
    if (ch->elem_size && ch->elem_size != sizeof(void*)) return EINVAL;
    return cc_chan_send(ch, &ptr, sizeof(void*));
}

int cc_chan_try_send_take(CCChan* ch, void* ptr) {
    if (!ch) return EINVAL;
    if (!ch->allow_take) return EINVAL;
    if (ch->elem_size && ch->elem_size != sizeof(void*)) return EINVAL;
    return cc_chan_try_send(ch, &ptr, sizeof(void*));
}

int cc_chan_timed_send_take(CCChan* ch, void* ptr, const struct timespec* abs_deadline) {
    if (!ch) return EINVAL;
    if (!ch->allow_take) return EINVAL;
    if (ch->elem_size && ch->elem_size != sizeof(void*)) return EINVAL;
    return cc_chan_timed_send(ch, &ptr, sizeof(void*), abs_deadline);
}

int cc_chan_deadline_send_take(CCChan* ch, void* ptr, const CCDeadline* deadline) {
    struct timespec ts;
    const struct timespec* p = cc_deadline_as_timespec(deadline, &ts);
    return cc_chan_timed_send_take(ch, ptr, p);
}

static int cc_chan_check_slice_take(const CCSlice* slice) {
    if (!slice) return EINVAL;
    if (!cc_slice_is_unique(*slice)) return EINVAL;
    if (!cc_slice_is_transferable(*slice)) return EINVAL;
    if (cc_slice_is_subslice(*slice)) return EINVAL;
    return 0;
}

/* Ownership-transferring slice send functions.
 * CCSliceUnique parameter documents that caller transfers ownership. */
int cc_chan_send_take_slice(CCChan* ch, const CCSliceUnique* slice) {
    if (!ch) return EINVAL;
    if (!ch->allow_take) return EINVAL;
    int elig = cc_chan_check_slice_take(slice);
    if (elig != 0) return elig;
    if (ch->elem_size && ch->elem_size != sizeof(CCSlice)) return EINVAL;
    return cc_chan_send(ch, slice, sizeof(CCSlice));
}

int cc_chan_try_send_take_slice(CCChan* ch, const CCSliceUnique* slice) {
    if (!ch) return EINVAL;
    if (!ch->allow_take) return EINVAL;
    int elig = cc_chan_check_slice_take(slice);
    if (elig != 0) return elig;
    if (ch->elem_size && ch->elem_size != sizeof(CCSlice)) return EINVAL;
    return cc_chan_try_send(ch, slice, sizeof(CCSlice));
}

int cc_chan_timed_send_take_slice(CCChan* ch, const CCSliceUnique* slice, const struct timespec* abs_deadline) {
    if (!ch) return EINVAL;
    if (!ch->allow_take) return EINVAL;
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

// Async channel operations via executor

typedef struct {
    CCChan* ch;
    const void* value;
    void* out_value;
    size_t size;
    int is_send; // 1 send, 0 recv
    CCDeadline deadline;
    CCAsyncHandle *handle;
} CCChanAsyncCtx;

static void cc__chan_async_job(void *arg) {
    CCChanAsyncCtx *ctx = (CCChanAsyncCtx*)arg;
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
    CCChanAsyncCtx *ctx = (CCChanAsyncCtx*)malloc(sizeof(*ctx));
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

// Non-blocking match helper (optionally rotated start for fairness)
static int cc__chan_match_try_from(CCChanMatchCase* cases, size_t n, size_t* ready_index, size_t start) {
    if (!cases || n == 0 || !ready_index) return EINVAL;
    for (size_t k = 0; k < n; ++k) {
        size_t i = (start + k) % n;
        CCChanMatchCase *c = &cases[i];
        if (!c->ch || c->elem_size == 0) continue;
        int rc;
        if (c->is_send) {
            rc = cc_chan_try_send(c->ch, c->send_buf, c->elem_size);
        } else {
            rc = cc_chan_try_recv(c->ch, c->recv_buf, c->elem_size);
        }
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
    
    /* Multi-channel select: Use global broadcast condvar.
       Any channel activity (send/recv/close) wakes all waiters.
       Simple, deadlock-free, at cost of some spurious wakeups. */
    static _Atomic uint64_t g_match_rr = 0;
    while (1) {
        size_t start = n ? (size_t)(atomic_fetch_add_explicit(&g_match_rr, 1, memory_order_relaxed) % n) : 0;
        int rc = cc__chan_match_try_from(cases, n, ready_index, start);
        if (rc == 0) { cc__chan_select_dbg_inc(&g_dbg_select_try_returned); return rc; }
        if (rc == EPIPE) { cc__chan_select_dbg_inc(&g_dbg_select_close_returned); return rc; }
        if (rc != EAGAIN) return rc;
        if (p) {
            struct timespec now; clock_gettime(CLOCK_REALTIME, &now);
            if (now.tv_sec > p->tv_sec || (now.tv_sec == p->tv_sec && now.tv_nsec >= p->tv_nsec)) {
                return ETIMEDOUT;
            }
        }
        
        /* Wait for any channel activity */
        if (fiber && !p) {
            /* Clear any stale pending_unpark from previous operations.
             * This prevents a spurious wakeup from a previous select/recv/send
             * from being consumed by this new select operation. */
            cc__fiber_clear_pending_unpark();
            
            cc__select_wait_group group = {0};
            group.fiber = fiber;
            atomic_store_explicit(&group.signaled, 0, memory_order_release);
            atomic_store_explicit(&group.selected_index, -1, memory_order_release);
            cc__fiber_wait_node nodes[n];
            uint64_t select_wait_ticket = cc__fiber_publish_wait_ticket(fiber);
            for (size_t i = 0; i < n; ++i) {
                CCChanMatchCase *c = &cases[i];
                nodes[i].next = NULL;
                nodes[i].prev = NULL;
                nodes[i].fiber = fiber;
                nodes[i].wait_ticket = select_wait_ticket;
                nodes[i].data = c->is_send ? (void*)c->send_buf : c->recv_buf;
                atomic_store_explicit(&nodes[i].notified, CC_CHAN_NOTIFY_NONE, memory_order_release);
                nodes[i].select_group = &group;
                nodes[i].select_index = i;
                nodes[i].is_select = 1;
                nodes[i].in_wait_list = 0;
                if (!c->ch) continue;
                pthread_mutex_lock(&c->ch->mu);
                if (c->is_send) {
                    cc__chan_add_send_waiter(c->ch, &nodes[i]);
                } else {
                    cc__chan_add_recv_waiter(c->ch, &nodes[i]);
                }
                pthread_mutex_unlock(&c->ch->mu);
            }
            /* Check if any node was already notified while we were adding to wait lists.
             * This handles the race where a sender pops our node and does a direct handoff
             * before we finish adding all nodes. The data is already in node->data. */
            for (size_t i = 0; i < n; ++i) {
                int notified = atomic_load_explicit(&nodes[i].notified, memory_order_acquire);
                if (notified == CC_CHAN_NOTIFY_DATA || notified == CC_CHAN_NOTIFY_CLOSE) {
                    /* Data/close was delivered directly to this node - clean up and return */
                    for (size_t j = 0; j < n; ++j) {
                        CCChanMatchCase *cj = &cases[j];
                        if (!cj->ch) continue;
                        pthread_mutex_lock(&cj->ch->mu);
                        if (nodes[j].in_wait_list) {
                            if (cj->is_send) {
                                cc__chan_remove_send_waiter(cj->ch, &nodes[j]);
                            } else {
                                cc__chan_remove_recv_waiter(cj->ch, &nodes[j]);
                            }
                        }
                        pthread_mutex_unlock(&cj->ch->mu);
                    }
                    *ready_index = i;
                    if (notified == CC_CHAN_NOTIFY_DATA) cc__chan_select_dbg_inc(&g_dbg_select_data_returned);
                    return (notified == CC_CHAN_NOTIFY_DATA) ? 0 : EPIPE;
                }
            }
            /* After adding all nodes, wake any senders that are parked waiting for
             * a receiver. The woken sender will re-enter its loop, call
             * cc__chan_pop_recv_waiter (which now finds our node), and do handoff.
             * We must flush the wake batch so the sender actually runs. */
            {
                int did_wake = 0;
                for (size_t i = 0; i < n; ++i) {
                    CCChanMatchCase *c = &cases[i];
                    if (!c->ch) continue;
                    pthread_mutex_lock(&c->ch->mu);
                    if (!c->is_send && c->ch->send_waiters_head) {
                        cc__chan_wake_one_send_waiter(c->ch);
                        did_wake = 1;
                    }
                    pthread_mutex_unlock(&c->ch->mu);
                }
                if (did_wake) wake_batch_flush();
            }
            /* Re-check if any node was notified by the woken senders/receivers */
            for (size_t i = 0; i < n; ++i) {
                int notified = atomic_load_explicit(&nodes[i].notified, memory_order_acquire);
                if (notified == CC_CHAN_NOTIFY_DATA || notified == CC_CHAN_NOTIFY_CLOSE) {
                    for (size_t j = 0; j < n; ++j) {
                        CCChanMatchCase *cj = &cases[j];
                        if (!cj->ch) continue;
                        pthread_mutex_lock(&cj->ch->mu);
                        if (nodes[j].in_wait_list) {
                            if (cj->is_send) {
                                cc__chan_remove_send_waiter(cj->ch, &nodes[j]);
                            } else {
                                cc__chan_remove_recv_waiter(cj->ch, &nodes[j]);
                            }
                        }
                        pthread_mutex_unlock(&cj->ch->mu);
                    }
                    *ready_index = i;
                    if (notified == CC_CHAN_NOTIFY_DATA) cc__chan_select_dbg_inc(&g_dbg_select_data_returned);
                    return (notified == CC_CHAN_NOTIFY_DATA) ? 0 : EPIPE;
                }
            }
            int need_rearm = 0;
            while (atomic_load_explicit(&group.selected_index, memory_order_acquire) == -1) {
                int seq = atomic_load_explicit(&group.signaled, memory_order_acquire);
                if (atomic_load_explicit(&group.selected_index, memory_order_acquire) != -1) {
                    break;
                }
                /* Clear pending_unpark right before parking to avoid consuming a wakeup
                 * that was meant for a previous operation or a different channel. */
                cc__fiber_clear_pending_unpark();
                CC_FIBER_PARK_IF(&group.signaled, seq, "chan_match: waiting");
                /* NOTE: We intentionally do NOT call cc_chan_match_try() here.
                 * Doing a non-blocking try while our nodes are still in wait lists
                 * creates a race: try can succeed on channel A while a sender on
                 * channel B simultaneously pops our node and completes a handoff.
                 * Both "succeed" but only one gets counted, losing data.
                 * Instead, we rely solely on the notified flags set by senders
                 * who went through the proper try_win path. */
                int saw_notify = 0;
                for (size_t i = 0; i < n; ++i) {
                    int notified = atomic_load_explicit(&nodes[i].notified, memory_order_acquire);
                    if (notified == CC_CHAN_NOTIFY_SIGNAL) {
                        if (!cases[i].is_send && cases[i].ch) {
                            cc__chan_recv_waiter_consume_signal(cases[i].ch, &nodes[i]);
                        }
                        atomic_store_explicit(&nodes[i].notified, CC_CHAN_NOTIFY_NONE, memory_order_release);
                        if (!cases[i].is_send && cases[i].ch) {
                            cc__chan_recv_waiter_rearm(cases[i].ch, &nodes[i]);
                        }
                        notified = CC_CHAN_NOTIFY_NONE;
                    }
                    if (notified == CC_CHAN_NOTIFY_CANCEL) {
                        atomic_store_explicit(&nodes[i].notified, CC_CHAN_NOTIFY_NONE, memory_order_release);
                        if (!cases[i].is_send && cases[i].ch) {
                            cc__chan_recv_waiter_rearm(cases[i].ch, &nodes[i]);
                        }
                        need_rearm = 1;
                        continue;
                    }
                    if (notified == CC_CHAN_NOTIFY_DATA || notified == CC_CHAN_NOTIFY_CLOSE) {
                        saw_notify = 1;
                        break;
                    }
                }
                if (saw_notify) {
                    break;
                }
                if (need_rearm) {
                    break;
                }
            }
            /* Remove all nodes from wait lists. We MUST acquire the mutex for each
             * channel even if in_wait_list is 0, because another thread might be
             * in the middle of cc__chan_select_cancel_node accessing our node.
             * The mutex acquisition serializes with that access. */
            for (size_t i = 0; i < n; ++i) {
                CCChanMatchCase *c = &cases[i];
                if (!c->ch) continue;
                pthread_mutex_lock(&c->ch->mu);
                if (nodes[i].in_wait_list) {
                    if (c->is_send) {
                        cc__chan_remove_send_waiter(c->ch, &nodes[i]);
                    } else {
                        cc__chan_remove_recv_waiter(c->ch, &nodes[i]);
                    }
                }
                pthread_mutex_unlock(&c->ch->mu);
            }
            /* Check if any node has DATA or CLOSE notification.
             * This must be done BEFORE checking need_rearm, because we might
             * have seen CANCEL on one node and DATA on another. */
            for (size_t i = 0; i < n; ++i) {
                int notified = atomic_load_explicit(&nodes[i].notified, memory_order_acquire);
                if (notified == CC_CHAN_NOTIFY_DATA) {
                    *ready_index = i;
                    cc__chan_select_dbg_inc(&g_dbg_select_data_returned);
                    return 0;
                }
                if (notified == CC_CHAN_NOTIFY_CLOSE) {
                    *ready_index = i;
                    return EPIPE;
                }
            }
            if (need_rearm) {
                continue;
            }
            int sel = atomic_load_explicit(&group.selected_index, memory_order_acquire);
            if (sel >= 0 && sel < (int)n) {
                for (;;) {
                    int notified = atomic_load_explicit(&nodes[sel].notified, memory_order_acquire);
                    if (notified == CC_CHAN_NOTIFY_SIGNAL) {
                        if (!cases[sel].is_send && cases[sel].ch) {
                            cc__chan_recv_waiter_consume_signal(cases[sel].ch, &nodes[sel]);
                        }
                        atomic_store_explicit(&nodes[sel].notified, CC_CHAN_NOTIFY_NONE, memory_order_release);
                        if (!cases[sel].is_send && cases[sel].ch) {
                            cc__chan_recv_waiter_rearm(cases[sel].ch, &nodes[sel]);
                        }
                        notified = CC_CHAN_NOTIFY_NONE;
                    }
                    if (notified == CC_CHAN_NOTIFY_DATA) {
                        *ready_index = (size_t)sel;
                        cc__chan_select_dbg_inc(&g_dbg_select_data_returned);
                        return 0;
                    }
                    if (notified == CC_CHAN_NOTIFY_CLOSE) {
                        *ready_index = (size_t)sel;
                        return EPIPE;
                    }
                    if (!fiber) {
                        break;
                    }
                    cc__fiber_set_park_obj(cases[sel].ch);
                    CC_FIBER_PARK_IF(&nodes[sel].notified, CC_CHAN_NOTIFY_NONE,
                                     "chan_match: waiting for winner");
                }
            }
            for (size_t i = 0; i < n; ++i) {
                int notified = atomic_load_explicit(&nodes[i].notified, memory_order_acquire);
                if (notified == CC_CHAN_NOTIFY_DATA) {
                    *ready_index = i;
                    cc__chan_select_dbg_inc(&g_dbg_select_data_returned);
                    return 0;
                }
                if (notified == CC_CHAN_NOTIFY_CLOSE) {
                    *ready_index = i;
                    return EPIPE;
                }
            }
        } else {
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

// Async select using executor
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
    ctx->cases = cases;
    ctx->n = n;
    ctx->ready_index = ready_index;
    ctx->handle = h;
    ctx->deadline = deadline ? *deadline : cc_deadline_none();
    int sub = cc_exec_submit(ex, cc__chan_match_async_job, ctx);
    if (sub != 0) {
        fprintf(stderr, "cc_chan_match_select_async: submit failed (%d)\n", sub);
        free(ctx);
        cc_chan_free(h->done);
        h->done = NULL;
        return sub;
    }
    return 0;
}

// Future-based async select
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
    int out_err = err < 0 ? err : 0; /* For now treat success/any positive errno as success for future helper. */
    cc_chan_send(ctx->fut->handle.done, &out_err, sizeof(int));
    free(ctx);
}

int cc_chan_match_select_future(CCExec* ex, CCChanMatchCase* cases, size_t n, size_t* ready_index, CCFuture* f, const CCDeadline* deadline) {
    if (!ex || !cases || n == 0 || !ready_index || !f) return EINVAL;
    cc_future_init(f);
    CC_ASYNC_HANDLE_ALLOC(&f->handle, 1);
    CCChanMatchFutureCtx* ctx = (CCChanMatchFutureCtx*)malloc(sizeof(*ctx));
    if (!ctx) { cc_future_free(f); return ENOMEM; }
    ctx->cases = cases;
    ctx->n = n;
    ctx->ready_index = ready_index;
    ctx->fut = f;
    ctx->deadline = deadline ? *deadline : cc_deadline_none();
    int sub = cc_exec_submit(ex, cc__chan_match_future_job, ctx);
    if (sub != 0) { free(ctx); cc_future_free(f); return sub; }
    return 0;
}

/* ---- Poll-based channel tasks (CCTaskIntptr) ----
 * These return CCTaskIntptr with poll-based implementation for cooperative async.
 * Result is errno (0=success). Caller must ensure value/out_value outlives the task.
 */

#include <ccc/std/task.cch>

typedef struct {
    CCChan* ch;
    void* buf;           /* for send: source; for recv: dest */
    size_t elem_size;
    const CCDeadline* deadline;
    int is_send;         /* 1=send, 0=recv */
    int completed;
    int result;          /* 0 success, errno on error */
    int waiting;         /* recv-only: whether we've registered as a waiting receiver */
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
            f->pending_async = 0;
            f->completed = 1;
            f->result = err;
            if (out_val) *out_val = (intptr_t)f->result;
            if (out_err) *out_err = f->result;
            return CC_FUTURE_READY;
        }
        if (rc == EPIPE) {
            cc_async_handle_free(&f->async.handle);
            f->pending_async = 0;
            f->completed = 1;
            f->result = EPIPE;
            if (out_val) *out_val = (intptr_t)f->result;
            if (out_err) *out_err = f->result;
            return CC_FUTURE_READY;
        }
        return CC_FUTURE_PENDING;
    }

    /* Check deadline */
    if (f->deadline && cc_deadline_expired(f->deadline)) {
        f->completed = 1;
        f->result = ETIMEDOUT;
        if (out_val) *out_val = ETIMEDOUT;
        if (out_err) *out_err = ETIMEDOUT;
        return CC_FUTURE_READY;
    }

    int rc;
    if (f->is_send) {
        rc = cc_chan_try_send(f->ch, f->buf, f->elem_size);
    } else {
        rc = cc_chan_try_recv(f->ch, f->buf, f->elem_size);
        if (f->ch && f->ch->cap == 0) {
            /* Unbuffered rendezvous: no side-effects in poll path. */
        }
    }

    if (rc == EAGAIN) {
        /* Would block. In fiber context, do blocking directly (fiber-aware)
           instead of using the executor pool which can starve with multiple
           concurrent waiters. Pass NULL deadline to get fiber-aware blocking
           (deadline is handled at outer scope). */
        if (cc__fiber_in_context()) {
            CCChan* ch = f->ch;
            int err;
            if (ch->cap == 0) {
                /* Unbuffered: use direct handoff with fiber blocking */
                pthread_mutex_lock(&ch->mu);
                err = f->is_send
                    ? cc_chan_send_unbuffered(ch, f->buf, NULL)
                    : cc_chan_recv_unbuffered(ch, f->buf, NULL);
                pthread_mutex_unlock(&ch->mu);
            } else {
                /* Buffered: use timed send/recv with NULL deadline for fiber blocking */
                err = f->is_send
                    ? cc_chan_timed_send(ch, f->buf, f->elem_size, NULL)
                    : cc_chan_timed_recv(ch, f->buf, f->elem_size, NULL);
            }
            wake_batch_flush();
            f->completed = 1;
            f->result = err;
            if (out_val) *out_val = (intptr_t)err;
            if (out_err) *out_err = err;
            return CC_FUTURE_READY;
        }
        /* Non-fiber context: offload to async executor if available */
        CCExec* ex = cc_async_runtime_exec();
        if (ex) {
            int sub = 0;
            if (f->is_send) {
                sub = cc_chan_send_async(ex, f->ch, f->buf, f->elem_size, &f->async, f->deadline);
            } else {
                sub = cc_chan_recv_async(ex, f->ch, f->buf, f->elem_size, &f->async, f->deadline);
            }
            if (sub == 0) {
                f->pending_async = 1;
            }
        }
        return CC_FUTURE_PENDING;
    }

    /* Completed (success or error) */
    f->completed = 1;
    f->result = rc;
    if (out_val) *out_val = (intptr_t)rc;
    if (out_err) *out_err = rc;
    return CC_FUTURE_READY;
}

static int cc__chan_task_wait(void* frame) {
    /* Block until the channel can make progress (for block_on from sync context).
       Uses the channel's condition variables for efficient waiting. */
    CCChanTaskFrame* f = (CCChanTaskFrame*)frame;
    if (!f || !f->ch) return EINVAL;
    if (f->pending_async) {
        int err = cc_async_wait_deadline(&f->async.handle, f->deadline);
        f->pending_async = 0;
        f->completed = 1;
        f->result = err;
        return err;
    }
    CCChan* ch = f->ch;
    pthread_mutex_lock(&ch->mu);
    if (ch->cap == 0) {
        struct timespec ts;
        const struct timespec* p = f->deadline ? cc_deadline_as_timespec(f->deadline, &ts) : NULL;
        int err = f->is_send
            ? cc_chan_send_unbuffered(ch, f->buf, p)
            : cc_chan_recv_unbuffered(ch, f->buf, p);
        pthread_mutex_unlock(&ch->mu);
        wake_batch_flush();
        return err;
    }
    if (f->is_send) {
        while (!ch->closed && ch->count == ch->cap) {
            pthread_cond_wait(&ch->not_full, &ch->mu);
        }
    } else {
        while (!ch->closed && ch->count == 0) {
            pthread_cond_wait(&ch->not_empty, &ch->mu);
        }
    }
    pthread_mutex_unlock(&ch->mu);
    return 0;
}

static void cc__chan_task_drop(void* frame) {
    free(frame);
}

CCTaskIntptr cc_chan_send_task(CCChan* ch, const void* value, size_t value_size) {
    CCTaskIntptr invalid = {0};
    if (!ch || !value || value_size == 0) return invalid;

    CCChanTaskFrame* f = (CCChanTaskFrame*)calloc(1, sizeof(CCChanTaskFrame));
    if (!f) return invalid;

    f->ch = ch;
    f->buf = (void*)value;  /* Note: caller must ensure value outlives task */
    f->elem_size = value_size;
    f->deadline = cc_current_deadline();
    f->is_send = 1;

    return cc_task_intptr_make_poll_ex(cc__chan_task_poll, cc__chan_task_wait, f, cc__chan_task_drop);
}

CCTaskIntptr cc_chan_recv_task(CCChan* ch, void* out_value, size_t value_size) {
    CCTaskIntptr invalid = {0};
    if (!ch || !out_value || value_size == 0) return invalid;

    CCChanTaskFrame* f = (CCChanTaskFrame*)calloc(1, sizeof(CCChanTaskFrame));
    if (!f) return invalid;

    f->ch = ch;
    f->buf = out_value;
    f->elem_size = value_size;
    f->deadline = cc_current_deadline();
    f->is_send = 0;

    return cc_task_intptr_make_poll_ex(cc__chan_task_poll, cc__chan_task_wait, f, cc__chan_task_drop);
}
