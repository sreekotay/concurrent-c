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
/* fiber_sched.c is now included in concurrent_c.c */

/* Defined in nursery.c (same translation unit via runtime/concurrent_c.c). */
extern __thread CCNursery* cc__tls_current_nursery;
/* Thread-local current deadline scope (set by with_deadline lowering). */
__thread CCDeadline* cc__tls_current_deadline = NULL;

/* ============================================================================
 * Channel timing instrumentation
 * Enables CC_CHANNEL_TIMING=1 to report send/recv lock/enqueue/dequeue costs.
 * ============================================================================
 */

typedef struct {
    _Atomic uint64_t send_cycles;
    _Atomic uint64_t send_lock_cycles;
    _Atomic uint64_t send_enqueue_cycles;
    _Atomic uint64_t send_wake_cycles;
    _Atomic uint64_t recv_cycles;
    _Atomic uint64_t recv_lock_cycles;
    _Atomic uint64_t recv_dequeue_cycles;
    _Atomic uint64_t recv_wake_cycles;
    _Atomic size_t send_count;
    _Atomic size_t recv_count;
} channel_timing;

static channel_timing g_channel_timing = {0};
static int g_channel_timing_enabled = -1;

static inline uint64_t channel_rdtsc(void) {
    #if defined(__x86_64__) || defined(_M_X64)
    unsigned int lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
    #elif defined(__aarch64__) || defined(__arm64__)
    uint64_t val;
    __asm__ volatile("mrs %0, cntvct_el0" : "=r"(val));
    return val;
    #else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
    #endif
}

static void channel_timing_dump(void) {
    size_t send = atomic_load_explicit(&g_channel_timing.send_count, memory_order_relaxed);
    size_t recv = atomic_load_explicit(&g_channel_timing.recv_count, memory_order_relaxed);
    if (send == 0 && recv == 0) return;
    fprintf(stderr, "\n=== CHANNEL TIMING ===\n");
    if (send) {
        uint64_t total = atomic_load_explicit(&g_channel_timing.send_cycles, memory_order_relaxed);
        uint64_t lock = atomic_load_explicit(&g_channel_timing.send_lock_cycles, memory_order_relaxed);
        uint64_t enqueue = atomic_load_explicit(&g_channel_timing.send_enqueue_cycles, memory_order_relaxed);
        uint64_t wake = atomic_load_explicit(&g_channel_timing.send_wake_cycles, memory_order_relaxed);
        fprintf(stderr, "  send: total=%8.1f cycles (%zu ops)\n", (double)total / send, send);
        fprintf(stderr, "    lock=%8.1f cycles/op (%5.1f%%) enqueue=%8.1f cycles/op (%5.1f%%)\n",
                (double)lock / send, total ? 100.0 * lock / total : 0.0,
                (double)enqueue / send, total ? 100.0 * enqueue / total : 0.0);
        fprintf(stderr, "    wake=%8.1f cycles/op (%5.1f%%)\n",
                (double)wake / send, total ? 100.0 * wake / total : 0.0);
    }
    if (recv) {
        uint64_t total = atomic_load_explicit(&g_channel_timing.recv_cycles, memory_order_relaxed);
        uint64_t lock = atomic_load_explicit(&g_channel_timing.recv_lock_cycles, memory_order_relaxed);
        uint64_t dequeue = atomic_load_explicit(&g_channel_timing.recv_dequeue_cycles, memory_order_relaxed);
        uint64_t wake = atomic_load_explicit(&g_channel_timing.recv_wake_cycles, memory_order_relaxed);
        fprintf(stderr, "  recv: total=%8.1f cycles (%zu ops)\n", (double)total / recv, recv);
        fprintf(stderr, "    lock=%8.1f cycles/op (%5.1f%%) dequeue=%8.1f cycles/op (%5.1f%%)\n",
                (double)lock / recv, total ? 100.0 * lock / total : 0.0,
                (double)dequeue / recv, total ? 100.0 * dequeue / total : 0.0);
        fprintf(stderr, "    wake=%8.1f cycles/op (%5.1f%%)\n",
                (double)wake / recv, total ? 100.0 * wake / total : 0.0);
    }
    fprintf(stderr, "======================\n\n");
}

static int channel_timing_enabled(void) {
    if (g_channel_timing_enabled < 0) {
        g_channel_timing_enabled = getenv("CC_CHANNEL_TIMING") ? 1 : 0;
        if (g_channel_timing_enabled) {
            atexit(channel_timing_dump);
        }
    }
    return g_channel_timing_enabled;
}

static inline void channel_timing_record_send(uint64_t start, uint64_t lock, uint64_t enqueue, uint64_t wake, uint64_t end) {
    atomic_fetch_add_explicit(&g_channel_timing.send_cycles, end - start, memory_order_relaxed);
    atomic_fetch_add_explicit(&g_channel_timing.send_lock_cycles, lock - start, memory_order_relaxed);
    atomic_fetch_add_explicit(&g_channel_timing.send_enqueue_cycles, enqueue - lock, memory_order_relaxed);
    atomic_fetch_add_explicit(&g_channel_timing.send_wake_cycles, end - wake, memory_order_relaxed);
    atomic_fetch_add_explicit(&g_channel_timing.send_count, 1, memory_order_relaxed);
}

static inline void channel_timing_record_recv(uint64_t start, uint64_t lock, uint64_t dequeue, uint64_t wake, uint64_t end) {
    atomic_fetch_add_explicit(&g_channel_timing.recv_cycles, end - start, memory_order_relaxed);
    atomic_fetch_add_explicit(&g_channel_timing.recv_lock_cycles, lock - start, memory_order_relaxed);
    atomic_fetch_add_explicit(&g_channel_timing.recv_dequeue_cycles, dequeue - lock, memory_order_relaxed);
    atomic_fetch_add_explicit(&g_channel_timing.recv_wake_cycles, end - wake, memory_order_relaxed);
    atomic_fetch_add_explicit(&g_channel_timing.recv_count, 1, memory_order_relaxed);
}

/* ============================================================================
 * Channel debug counters (lock-free focus)
 * ============================================================================ */

typedef struct {
    _Atomic uint64_t lf_enq_attempt;
    _Atomic uint64_t lf_enq_ok;
    _Atomic uint64_t lf_enq_fail;
    _Atomic uint64_t lf_deq_attempt;
    _Atomic uint64_t lf_deq_ok;
    _Atomic uint64_t lf_deq_fail;
    _Atomic uint64_t lf_send_waiter_add;
    _Atomic uint64_t lf_recv_waiter_add;
    _Atomic uint64_t lf_send_waiter_remove;
    _Atomic uint64_t lf_recv_waiter_remove;
    _Atomic uint64_t lf_send_waiter_wake;
    _Atomic uint64_t lf_recv_waiter_wake;
    _Atomic uint64_t lf_send_notify_signal;
    _Atomic uint64_t lf_send_notify_close;
    _Atomic uint64_t lf_send_notify_cancel;
    _Atomic uint64_t lf_recv_notify_signal;
    _Atomic uint64_t lf_recv_notify_data;
    _Atomic uint64_t lf_recv_notify_close;
    _Atomic uint64_t lf_recv_notify_cancel;
    _Atomic uint64_t lf_recv_wake_no_waiter;
    _Atomic uint64_t lf_close_calls;
    _Atomic uint64_t lf_close_drain_calls;
    _Atomic uint64_t lf_direct_send;
    _Atomic uint64_t lf_direct_recv;
    _Atomic uint64_t lf_send_ok;      /* Total successful sends */
    _Atomic uint64_t lf_recv_ok;      /* Total successful recvs */
    _Atomic uint64_t lf_has_recv_waiters_true;
    _Atomic uint64_t lf_has_recv_waiters_false;
    _Atomic uint64_t lf_has_send_waiters_true;
    _Atomic uint64_t lf_has_send_waiters_false;
    _Atomic uint64_t lf_wake_lock_send;
    _Atomic uint64_t lf_wake_lock_recv;
    _Atomic uint64_t lf_waiter_ticket_stale;
} CCChanDebugCounters;

typedef struct {
    _Atomic size_t seq;
    void* value;
} cc__ring_cell;

static CCChanDebugCounters g_chan_dbg = {0};
static int g_chan_dbg_enabled = -1;
static int g_chan_dbg_verbose = -1;
static _Atomic uint64_t g_chan_dbg_close_seq = 0;
static _Atomic uintptr_t g_chan_dbg_last_close = 0;

static inline int cc__chan_dbg_enabled(void);
static inline int cc__chan_dbg_verbose_enabled(void);


static inline int cc__chan_dbg_enabled(void) {
    if (g_chan_dbg_enabled == -1) {
        const char* v = getenv("CC_CHAN_DEBUG");
        g_chan_dbg_enabled = (v && v[0] == '1') ? 1 : 0;
    }
    return g_chan_dbg_enabled;
}

static inline int cc__chan_dbg_verbose_enabled(void) {
    if (g_chan_dbg_verbose == -1) {
        const char* v = getenv("CC_CHAN_DEBUG_VERBOSE");
        g_chan_dbg_verbose = (v && v[0] == '1') ? 1 : 0;
    }
    return g_chan_dbg_verbose;
}

/* Verbose logging gated on channel closed flag -- only emit noise once
 * the channel is closing / closed so we capture the deadlock-relevant
 * window without drowning in happy-path output.
 * Defined as a macro to avoid forward-declaration issues with CCChan. */
#define cc__chan_dbg_verbose_closing(ch) \
    (cc__chan_dbg_verbose_enabled() && (ch)->closed)

static inline void cc__chan_dbg_select_event(const char* event, cc__fiber_wait_node* node) {
    if (!cc__chan_dbg_verbose_enabled()) return;
    if (!node || !node->is_select || !node->select_group) return;
    fprintf(stderr,
            "CC_CHAN_DEBUG: select_%s group=%p node=%p fiber=%p idx=%zu notified=%d\n",
            event,
            node->select_group,
            (void*)node,
            (void*)node->fiber,
            node->select_index,
            atomic_load_explicit(&node->notified, memory_order_relaxed));
}

static inline void cc__chan_dbg_inc(_Atomic uint64_t* c) {
    if (!cc__chan_dbg_enabled()) return;
    atomic_fetch_add_explicit(c, 1, memory_order_relaxed);
}

/* Select handoff accounting (always available, printed when CC_CHAN_DEBUG=1) */
static _Atomic uint64_t g_dbg_select_data_set = 0;      /* sender set notified=DATA on select node */
static _Atomic uint64_t g_dbg_select_data_returned = 0;  /* cc_chan_match_deadline returned 0 via DATA */
static _Atomic uint64_t g_dbg_select_try_returned = 0;   /* cc_chan_match_deadline returned 0 via try */
static _Atomic uint64_t g_dbg_select_close_returned = 0; /* cc_chan_match_deadline returned EPIPE */

void cc__chan_debug_dump_global(void) {
    if (!cc__chan_dbg_enabled()) return;
    fprintf(stderr, "Channel debug counters (lock-free):\n");
    fprintf(stderr, "  enqueue attempts: %" PRIu64 "\n", atomic_load_explicit(&g_chan_dbg.lf_enq_attempt, memory_order_relaxed));
    fprintf(stderr, "  enqueue ok:       %" PRIu64 "\n", atomic_load_explicit(&g_chan_dbg.lf_enq_ok, memory_order_relaxed));
    fprintf(stderr, "  enqueue fail:     %" PRIu64 "\n", atomic_load_explicit(&g_chan_dbg.lf_enq_fail, memory_order_relaxed));
    fprintf(stderr, "  dequeue attempts: %" PRIu64 "\n", atomic_load_explicit(&g_chan_dbg.lf_deq_attempt, memory_order_relaxed));
    fprintf(stderr, "  dequeue ok:       %" PRIu64 "\n", atomic_load_explicit(&g_chan_dbg.lf_deq_ok, memory_order_relaxed));
    fprintf(stderr, "  dequeue fail:     %" PRIu64 "\n", atomic_load_explicit(&g_chan_dbg.lf_deq_fail, memory_order_relaxed));
    fprintf(stderr, "  send waiters add: %" PRIu64 "\n", atomic_load_explicit(&g_chan_dbg.lf_send_waiter_add, memory_order_relaxed));
    fprintf(stderr, "  recv waiters add: %" PRIu64 "\n", atomic_load_explicit(&g_chan_dbg.lf_recv_waiter_add, memory_order_relaxed));
    fprintf(stderr, "  send waiters rm:  %" PRIu64 "\n", atomic_load_explicit(&g_chan_dbg.lf_send_waiter_remove, memory_order_relaxed));
    fprintf(stderr, "  recv waiters rm:  %" PRIu64 "\n", atomic_load_explicit(&g_chan_dbg.lf_recv_waiter_remove, memory_order_relaxed));
    fprintf(stderr, "  send waiters wake:%" PRIu64 "\n", atomic_load_explicit(&g_chan_dbg.lf_send_waiter_wake, memory_order_relaxed));
    fprintf(stderr, "  recv waiters wake:%" PRIu64 "\n", atomic_load_explicit(&g_chan_dbg.lf_recv_waiter_wake, memory_order_relaxed));
    fprintf(stderr, "  send notify sig:  %" PRIu64 "\n", atomic_load_explicit(&g_chan_dbg.lf_send_notify_signal, memory_order_relaxed));
    fprintf(stderr, "  send notify close:%" PRIu64 "\n", atomic_load_explicit(&g_chan_dbg.lf_send_notify_close, memory_order_relaxed));
    fprintf(stderr, "  send notify cancel:%" PRIu64 "\n", atomic_load_explicit(&g_chan_dbg.lf_send_notify_cancel, memory_order_relaxed));
    fprintf(stderr, "  recv notify sig:  %" PRIu64 "\n", atomic_load_explicit(&g_chan_dbg.lf_recv_notify_signal, memory_order_relaxed));
    fprintf(stderr, "  recv notify data: %" PRIu64 "\n", atomic_load_explicit(&g_chan_dbg.lf_recv_notify_data, memory_order_relaxed));
    fprintf(stderr, "  recv notify close:%" PRIu64 "\n", atomic_load_explicit(&g_chan_dbg.lf_recv_notify_close, memory_order_relaxed));
    fprintf(stderr, "  recv notify cancel:%" PRIu64 "\n", atomic_load_explicit(&g_chan_dbg.lf_recv_notify_cancel, memory_order_relaxed));
    fprintf(stderr, "  recv wake no wait:%" PRIu64 "\n", atomic_load_explicit(&g_chan_dbg.lf_recv_wake_no_waiter, memory_order_relaxed));
    fprintf(stderr, "  close calls:      %" PRIu64 "\n", atomic_load_explicit(&g_chan_dbg.lf_close_calls, memory_order_relaxed));
    fprintf(stderr, "  close drain calls:%" PRIu64 "\n", atomic_load_explicit(&g_chan_dbg.lf_close_drain_calls, memory_order_relaxed));
    fprintf(stderr, "  direct send:      %" PRIu64 "\n", atomic_load_explicit(&g_chan_dbg.lf_direct_send, memory_order_relaxed));
    fprintf(stderr, "  direct recv:      %" PRIu64 "\n", atomic_load_explicit(&g_chan_dbg.lf_direct_recv, memory_order_relaxed));
    fprintf(stderr, "  SEND OK TOTAL:    %" PRIu64 "\n", atomic_load_explicit(&g_chan_dbg.lf_send_ok, memory_order_relaxed));
    fprintf(stderr, "  RECV OK TOTAL:    %" PRIu64 "\n", atomic_load_explicit(&g_chan_dbg.lf_recv_ok, memory_order_relaxed));
    fprintf(stderr, "  has_recv_waiters true/false: %" PRIu64 " / %" PRIu64 "\n",
            atomic_load_explicit(&g_chan_dbg.lf_has_recv_waiters_true, memory_order_relaxed),
            atomic_load_explicit(&g_chan_dbg.lf_has_recv_waiters_false, memory_order_relaxed));
    fprintf(stderr, "  has_send_waiters true/false: %" PRIu64 " / %" PRIu64 "\n",
            atomic_load_explicit(&g_chan_dbg.lf_has_send_waiters_true, memory_order_relaxed),
            atomic_load_explicit(&g_chan_dbg.lf_has_send_waiters_false, memory_order_relaxed));
    fprintf(stderr, "  wake-path lock enter send/recv: %" PRIu64 " / %" PRIu64 "\n",
            atomic_load_explicit(&g_chan_dbg.lf_wake_lock_send, memory_order_relaxed),
            atomic_load_explicit(&g_chan_dbg.lf_wake_lock_recv, memory_order_relaxed));
    fprintf(stderr, "  stale waiter tickets:%" PRIu64 "\n",
            atomic_load_explicit(&g_chan_dbg.lf_waiter_ticket_stale, memory_order_relaxed));
    fprintf(stderr, "  last close ptr:   %p (seq=%" PRIu64 ")\n",
            (void*)atomic_load_explicit(&g_chan_dbg_last_close, memory_order_relaxed),
            atomic_load_explicit(&g_chan_dbg_close_seq, memory_order_relaxed));
    fprintf(stderr, "\nSelect handoff accounting:\n");
    uint64_t ds = atomic_load_explicit(&g_dbg_select_data_set, memory_order_relaxed);
    uint64_t dr = atomic_load_explicit(&g_dbg_select_data_returned, memory_order_relaxed);
    uint64_t tr = atomic_load_explicit(&g_dbg_select_try_returned, memory_order_relaxed);
    uint64_t cr = atomic_load_explicit(&g_dbg_select_close_returned, memory_order_relaxed);
    fprintf(stderr, "  select notified=DATA set:     %" PRIu64 "\n", ds);
    fprintf(stderr, "  select returned via DATA:     %" PRIu64 "\n", dr);
    fprintf(stderr, "  select returned via try:      %" PRIu64 "\n", tr);
    fprintf(stderr, "  select returned via CLOSE:    %" PRIu64 "\n", cr);
    fprintf(stderr, "  DELTA (set - data - try):     %" PRId64 "\n", (int64_t)(ds - dr - tr));
}


/* ============================================================================
 * Batch Wake Operations
 * ============================================================================ */

#define WAKE_BATCH_SIZE 16

typedef struct {
    cc__fiber* fibers[WAKE_BATCH_SIZE];
    size_t count;
} wake_batch;

static __thread wake_batch tls_wake_batch = {{NULL}, 0};

/* Forward declaration */
static inline void wake_batch_flush(void);

/* Add a fiber to the wake batch */
static inline void wake_batch_add(cc__fiber* f) {
    if (!f) return;
    wake_batch* b = &tls_wake_batch;
    if (b->count >= WAKE_BATCH_SIZE) {
        wake_batch_flush();
    }
    b->fibers[b->count++] = f;
}

/* Flush all pending wakes */
static inline void wake_batch_flush(void) {
    wake_batch* b = &tls_wake_batch;
    for (size_t i = 0; i < b->count; i++) {
        if (b->fibers[i]) {
            cc_sched_fiber_wake((CCSchedFiber*)b->fibers[i]);
            b->fibers[i] = NULL;
        }
    }
    b->count = 0;
}

typedef struct cc__chan_wait_notified_ctx {
    cc__fiber_wait_node* node;
} cc__chan_wait_notified_ctx;

static bool cc__chan_wait_notified_try_complete(void* waitable, CCSchedFiber* fiber, void* io) {
    (void)fiber;
    (void)io;
    cc__chan_wait_notified_ctx* ctx = (cc__chan_wait_notified_ctx*)waitable;
    return atomic_load_explicit(&ctx->node->notified, memory_order_acquire) != 0;
}

static bool cc__chan_wait_notified_publish(void* waitable, CCSchedFiber* fiber, void* io) {
    (void)waitable;
    (void)fiber;
    (void)io;
    return true;
}

static void cc__chan_wait_notified_unpublish(void* waitable, CCSchedFiber* fiber) {
    (void)waitable;
    (void)fiber;
}

static void cc__chan_wait_notified_park(void* waitable, CCSchedFiber* fiber, void* io) {
    (void)fiber;
    (void)io;
    cc__chan_wait_notified_ctx* ctx = (cc__chan_wait_notified_ctx*)waitable;
    CC_FIBER_PARK_IF(&ctx->node->notified, 0, "chan_wait_notified");
}

static inline cc_sched_wait_result cc__chan_wait_notified(cc__fiber_wait_node* node) {
    if (atomic_load_explicit(&node->notified, memory_order_acquire) != 0) {
        return CC_SCHED_WAIT_OK;
    }
    cc__chan_wait_notified_ctx ctx = {.node = node};
    const cc_sched_waitable_ops ops = {
        .try_complete = cc__chan_wait_notified_try_complete,
        .publish = cc__chan_wait_notified_publish,
        .unpublish = cc__chan_wait_notified_unpublish,
        .park = cc__chan_wait_notified_park,
    };
    return cc_sched_fiber_wait(&ctx, NULL, &ops);
}

static inline cc_sched_wait_result cc__chan_wait_notified_mark_close(cc__fiber_wait_node* node) {
    cc_sched_wait_result wait_rc = cc__chan_wait_notified(node);
    if (wait_rc == CC_SCHED_WAIT_CLOSED) {
        atomic_store_explicit(&node->notified, CC_CHAN_NOTIFY_CLOSE, memory_order_release);
    }
    return wait_rc;
}

/* ============================================================================
 * Cooperative yield for lockfree fast path
 * Prevents starvation when many fibers loop on lockfree send/recv
 * ============================================================================ */
/* Cooperative yield for lockfree fast path.
 * Unlike cc__fiber_yield (which pushes to the local queue and can be
 * immediately re-popped by the same worker), cc__fiber_yield_global pushes
 * to the GLOBAL queue so other fibers waiting there get a fair chance.
 * Prevents starvation when many fibers loop on lockfree send/recv. */
#define CC_LF_YIELD_INTERVAL 32
static __thread unsigned int cc__tls_lf_ops = 0;
static _Atomic int g_chan_minimal_path_mode = -1; /* -1 unknown, 0 off, 1 on */

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

static inline void cc__chan_maybe_yield(void) {
    if (++cc__tls_lf_ops >= CC_LF_YIELD_INTERVAL) {
        cc__tls_lf_ops = 0;
        if (cc__fiber_in_context()) cc__fiber_yield_global();
    }
}

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
static void cc__chan_signal_activity(CCChan* ch);

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

static inline void cc__chan_dbg_select_group(const char* event, cc__select_wait_group* group) {
    if (!cc__chan_dbg_verbose_enabled()) return;
    if (!group) return;
    fprintf(stderr,
            "CC_CHAN_DEBUG: select_%s group=%p fiber=%p selected=%d signaled=%d\n",
            event,
            (void*)group,
            (void*)group->fiber,
            atomic_load_explicit(&group->selected_index, memory_order_relaxed),
            atomic_load_explicit(&group->signaled, memory_order_relaxed));
}

static inline void cc__chan_dbg_select_wait(const char* event, cc__select_wait_group* group, size_t idx, int notified) {
    if (!cc__chan_dbg_verbose_enabled()) return;
    if (!group) return;
    fprintf(stderr,
            "CC_CHAN_DEBUG: select_%s group=%p fiber=%p idx=%zu selected=%d notified=%d signaled=%d\n",
            event,
            (void*)group,
            (void*)group->fiber,
            idx,
            atomic_load_explicit(&group->selected_index, memory_order_relaxed),
            notified,
            atomic_load_explicit(&group->signaled, memory_order_relaxed));
}

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
    if (cc__chan_waiter_ticket_valid(node)) return 1;
    cc__chan_dbg_inc(&g_chan_dbg.lf_waiter_ticket_stale);
    if (cc__chan_dbg_verbose_enabled() && node) {
        fprintf(stderr,
                "CC_CHAN_DEBUG: stale_waiter_ticket where=%s node=%p fiber=%p ticket=%" PRIu64 "\n",
                where ? where : "(unknown)",
                (void*)node,
                (void*)node->fiber,
                (uint64_t)node->wait_ticket);
    }
    return 0;
}

static inline void cc__chan_debug_invariant(CCChan* ch, const char* where, const char* msg);
static inline void cc__chan_debug_check_recv_close(CCChan* ch, const char* where);

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
    /* Dekker flags: set by add_waiter, cleared by remove_waiter (NOT by wake_one).
     * This ensures the flag remains visible between wake_one removing the node
     * and the woken fiber calling remove_waiter, closing the lost-wakeup window. */
    _Atomic int has_send_waiters;
    _Atomic int has_recv_waiters;
    
    /* Lock-free MPMC queue for buffered channels (cap > 0) */
    int use_lockfree;                               /* 1 = use lock-free queue, 0 = use mutex */
    int use_ring_queue;                             /* 1 = use internal ring queue backend */
    size_t lfqueue_cap;                             /* Actual capacity (rounded up to power of 2) */
    struct lfds711_queue_bmm_state lfqueue_state;   /* liblfds queue state */
    struct lfds711_queue_bmm_element *lfqueue_elements; /* Pre-allocated element array */
    cc__ring_cell* ring_cells;                      /* Internal ring queue storage */
    _Atomic size_t ring_head;
    _Atomic size_t ring_tail;
    _Atomic int lfqueue_count;                      /* Approximate count for fast full/empty check */
    _Atomic int lfqueue_inflight;                   /* Active lock-free enqueue attempts */
    _Atomic size_t slot_counter;                    /* Per-channel slot counter for large elements */
    _Atomic int recv_fairness_ctr;                  /* For yield-every-N fairness */

    /* Debug counters (per-channel, lock-free focus) */
    _Atomic uint64_t dbg_lf_enq_ok;
    _Atomic uint64_t dbg_lf_deq_ok;
    _Atomic uint64_t dbg_lf_send_calls;
    _Atomic uint64_t dbg_lf_recv_calls;
    _Atomic uint64_t dbg_lf_direct_send;
    _Atomic uint64_t dbg_lf_direct_recv;
    _Atomic uint64_t dbg_lf_recv_remove_zero;
    _Atomic uint64_t dbg_lf_recv_waiter_add;
    _Atomic uint64_t dbg_lf_recv_waiter_wake;
    _Atomic uint64_t dbg_lf_recv_wake_no_waiter;

    /* Debug counters for unbuffered (rendezvous) channels */
    _Atomic uint64_t dbg_rv_send_handoff;       /* sender found recv waiter, did handoff */
    _Atomic uint64_t dbg_rv_send_parked;        /* sender parked waiting for receiver */
    _Atomic uint64_t dbg_rv_send_got_data;      /* sender woke with notified=DATA */
    _Atomic uint64_t dbg_rv_send_got_signal;    /* sender woke with notified=SIGNAL */
    _Atomic uint64_t dbg_rv_send_got_zero;      /* sender woke with notified=0 (spurious) */
    _Atomic uint64_t dbg_rv_send_inner_handoff; /* sender found recv waiter in inner loop (line 1880) */
    _Atomic uint64_t dbg_rv_recv_handoff;       /* receiver found send waiter, did handoff */
    _Atomic uint64_t dbg_rv_recv_parked;        /* receiver parked waiting for sender */
    _Atomic uint64_t dbg_rv_recv_got_data;      /* receiver woke with notified=DATA */
    _Atomic uint64_t dbg_rv_recv_got_signal;    /* receiver woke with notified=SIGNAL */
    _Atomic uint64_t dbg_rv_recv_got_zero;      /* receiver woke with notified=0 (spurious) */
    _Atomic uint64_t dbg_rv_recv_park_skip;     /* receiver park_if returned (pending_unpark) */
};

static inline void cc__chan_debug_invariant(CCChan* ch, const char* where, const char* msg) {
    if (!cc__chan_dbg_enabled()) return;
    fprintf(stderr, "CC_CHAN_INVARIANT: %s ch=%p %s\n", where, (void*)ch, msg);
}

static inline void cc__chan_debug_check_recv_close(CCChan* ch, const char* where) {
    if (!cc__chan_dbg_enabled() || !ch || !ch->closed) return;
    if (ch->use_lockfree) {
        int inflight = atomic_load_explicit(&ch->lfqueue_inflight, memory_order_acquire);
        int count = atomic_load_explicit(&ch->lfqueue_count, memory_order_acquire);
        if (count > 0 || inflight > 0) {
            fprintf(stderr,
                    "CC_CHAN_INVARIANT: %s ch=%p recv_close with inflight=%d count=%d\n",
                    where, (void*)ch, inflight, count);
        }
    } else if (ch->cap > 0 && ch->count > 0) {
        fprintf(stderr,
                "CC_CHAN_INVARIANT: %s ch=%p recv_close with count=%zu\n",
                where, (void*)ch, ch->count);
    }
}

void cc__chan_debug_dump_chan(void* ch_ptr) {
    if (!cc__chan_dbg_enabled() || !ch_ptr) return;
    CCChan* ch = (CCChan*)ch_ptr;
    int locked = pthread_mutex_trylock(&ch->mu) == 0;
    size_t send_waiters = 0;
    size_t recv_waiters = 0;
    if (locked) {
        for (cc__fiber_wait_node* n = ch->send_waiters_head; n; n = n->next) send_waiters++;
        for (cc__fiber_wait_node* n = ch->recv_waiters_head; n; n = n->next) recv_waiters++;
    }
    lfds711_pal_uint_t lf_ri = ch->use_lockfree ? ch->lfqueue_state.read_index : 0;
    lfds711_pal_uint_t lf_wi = ch->use_lockfree ? ch->lfqueue_state.write_index : 0;
    lfds711_pal_uint_t lf_ne = ch->use_lockfree ? ch->lfqueue_state.number_elements : 0;
    lfds711_pal_uint_t lf_mask = ch->use_lockfree ? ch->lfqueue_state.mask : 0;
    lfds711_pal_uint_t lf_est = lf_wi - lf_ri;
    fprintf(stderr,
            "  [chan %p] cap=%zu elem=%zu closed=%d rx_err=%d lf=%d lfcap=%zu count=%d inflight=%d ne=%" PRIu64 " mask=%" PRIu64 " ri=%" PRIu64 " wi=%" PRIu64 " est=%" PRIu64 " send_waiters=%zu recv_waiters=%zu%s\n",
            (void*)ch,
            ch->cap,
            ch->elem_size,
            ch->closed,
            ch->rx_error_closed,
            ch->use_lockfree,
            ch->lfqueue_cap,
            atomic_load_explicit(&ch->lfqueue_count, memory_order_acquire),
            atomic_load_explicit(&ch->lfqueue_inflight, memory_order_relaxed),
            (uint64_t)lf_ne,
            (uint64_t)lf_mask,
            (uint64_t)lf_ri,
            (uint64_t)lf_wi,
            (uint64_t)lf_est,
            send_waiters,
            recv_waiters,
            locked ? "" : " (lock busy)");
    fprintf(stderr,
            "    dbg: send_calls=%" PRIu64 " recv_calls=%" PRIu64 " enq_ok=%" PRIu64 " deq_ok=%" PRIu64 "\n",
            atomic_load_explicit(&ch->dbg_lf_send_calls, memory_order_relaxed),
            atomic_load_explicit(&ch->dbg_lf_recv_calls, memory_order_relaxed),
            atomic_load_explicit(&ch->dbg_lf_enq_ok, memory_order_relaxed),
            atomic_load_explicit(&ch->dbg_lf_deq_ok, memory_order_relaxed));
    fprintf(stderr,
            "    dbg: direct_send=%" PRIu64 " direct_recv=%" PRIu64 " recv_rm0=%" PRIu64 " recv_add=%" PRIu64 " recv_wake=%" PRIu64 " wake_no_waiter=%" PRIu64 "\n",
            atomic_load_explicit(&ch->dbg_lf_direct_send, memory_order_relaxed),
            atomic_load_explicit(&ch->dbg_lf_direct_recv, memory_order_relaxed),
            atomic_load_explicit(&ch->dbg_lf_recv_remove_zero, memory_order_relaxed),
            atomic_load_explicit(&ch->dbg_lf_recv_waiter_add, memory_order_relaxed),
            atomic_load_explicit(&ch->dbg_lf_recv_waiter_wake, memory_order_relaxed),
            atomic_load_explicit(&ch->dbg_lf_recv_wake_no_waiter, memory_order_relaxed));
    /* Unbuffered (rendezvous) debug counters */
    if (ch->cap == 0) {
        fprintf(stderr,
                "    rv_send: handoff=%" PRIu64 " inner_handoff=%" PRIu64 " parked=%" PRIu64
                " got_data=%" PRIu64 " got_signal=%" PRIu64 " got_zero=%" PRIu64 "\n",
                atomic_load_explicit(&ch->dbg_rv_send_handoff, memory_order_relaxed),
                atomic_load_explicit(&ch->dbg_rv_send_inner_handoff, memory_order_relaxed),
                atomic_load_explicit(&ch->dbg_rv_send_parked, memory_order_relaxed),
                atomic_load_explicit(&ch->dbg_rv_send_got_data, memory_order_relaxed),
                atomic_load_explicit(&ch->dbg_rv_send_got_signal, memory_order_relaxed),
                atomic_load_explicit(&ch->dbg_rv_send_got_zero, memory_order_relaxed));
        fprintf(stderr,
                "    rv_recv: handoff=%" PRIu64 " parked=%" PRIu64
                " got_data=%" PRIu64 " got_signal=%" PRIu64 " got_zero=%" PRIu64 " park_skip=%" PRIu64 "\n",
                atomic_load_explicit(&ch->dbg_rv_recv_handoff, memory_order_relaxed),
                atomic_load_explicit(&ch->dbg_rv_recv_parked, memory_order_relaxed),
                atomic_load_explicit(&ch->dbg_rv_recv_got_data, memory_order_relaxed),
                atomic_load_explicit(&ch->dbg_rv_recv_got_signal, memory_order_relaxed),
                atomic_load_explicit(&ch->dbg_rv_recv_got_zero, memory_order_relaxed),
                atomic_load_explicit(&ch->dbg_rv_recv_park_skip, memory_order_relaxed));
    }
    if (locked) {
        pthread_mutex_unlock(&ch->mu);
    }
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
    if (cc__chan_dbg_enabled()) {
        fprintf(stderr, "CC_CHAN_DEBUG: pair_create ch=%p tx=%p rx=%p cap=%zu elem=%zu\n",
                (void*)ch, (void*)out_tx->raw, (void*)out_rx->raw, capacity, elem_size);
    }
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
    if (cc__chan_dbg_enabled()) {
        fprintf(stderr, "CC_CHAN_DEBUG: pair_create_returning ch=%p tx=%p rx=%p cap=%zu elem=%zu\n",
                (void*)ch, (void*)out_tx->raw, (void*)out_rx->raw, capacity, elem_size);
    }
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
    cc__select_wait_group* group = (cc__select_wait_group*)node->select_group;
    int sel = atomic_load_explicit(&group->selected_index, memory_order_acquire);
    if (sel == (int)node->select_index) {
        cc__chan_dbg_select_event("already", node);
        return 1;
    }
    if (sel != -1) {
        return 0;
    }
    int expected = -1;
    if (atomic_compare_exchange_strong_explicit(&group->selected_index, &expected,
                                                (int)node->select_index,
                                                memory_order_acq_rel,
                                                memory_order_acquire)) {
        cc__chan_dbg_select_event("win", node);
        return 1;
    }
    return 0;
}

static inline void cc__chan_select_cancel_node(cc__fiber_wait_node* node) {
    if (!node) return;
    if (!cc__chan_waiter_ticket_valid_dbg(node, "select_cancel")) return;
    atomic_store_explicit(&node->notified, CC_CHAN_NOTIFY_CANCEL, memory_order_release);
    if (node->is_select && node->select_group) {
        cc__select_wait_group* group = (cc__select_wait_group*)node->select_group;
        int sig_before = atomic_load_explicit(&group->signaled, memory_order_acquire);
        atomic_fetch_add_explicit(&group->signaled, 1, memory_order_release);
        if (cc__chan_dbg_enabled()) {
            int sig_after = atomic_load_explicit(&group->signaled, memory_order_acquire);
            fprintf(stderr, "CC_CHAN_DEBUG: cancel_node_signaled fiber=%p group=%p sig=%d->%d\n",
                    (void*)node->fiber, (void*)group, sig_before, sig_after);
        }
    }
    if (node->fiber) {
        if (cc__chan_dbg_enabled() && node->is_select && node->select_group) {
            cc__select_wait_group* g = (cc__select_wait_group*)node->select_group;
            fprintf(stderr, "CC_CHAN_DEBUG: wake_batch_add_cancel fiber=%p group=%p sel=%d sig=%d\n",
                    (void*)node->fiber, (void*)g,
                    atomic_load_explicit(&g->selected_index, memory_order_acquire),
                    atomic_load_explicit(&g->signaled, memory_order_acquire));
        }
        wake_batch_add(node->fiber);
    }
}

/* Add a fiber to send waiters queue (must hold ch->mu) */
static void cc__chan_add_send_waiter(CCChan* ch, cc__fiber_wait_node* node) {
    if (!ch || !node) return;
    if (ch->use_lockfree) {
        cc__chan_dbg_inc(&g_chan_dbg.lf_send_waiter_add);
    }
    cc__chan_add_waiter(&ch->send_waiters_head, &ch->send_waiters_tail, node);
    atomic_store_explicit(&ch->has_send_waiters, 1, memory_order_seq_cst);
}

/* Add a fiber to recv waiters queue (must hold ch->mu) */
static void cc__chan_add_recv_waiter(CCChan* ch, cc__fiber_wait_node* node) {
    if (!ch || !node) return;
    if (ch->use_lockfree) {
        cc__chan_dbg_inc(&g_chan_dbg.lf_recv_waiter_add);
        cc__chan_dbg_inc(&ch->dbg_lf_recv_waiter_add);
    }
    cc__chan_add_waiter(&ch->recv_waiters_head, &ch->recv_waiters_tail, node);
    atomic_store_explicit(&ch->has_recv_waiters, 1, memory_order_seq_cst);
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

static void cc__chan_remove_send_waiter(CCChan* ch, cc__fiber_wait_node* node) {
    if (!ch || !node) return;
    if (!node->in_wait_list) {
        /* Node already removed by wake_one — clear the Dekker flag now
         * that the sender has processed its wake. */
        if (!ch->send_waiters_head)
            atomic_store_explicit(&ch->has_send_waiters, 0, memory_order_seq_cst);
        return;
    }
    if (ch->use_lockfree) {
        cc__chan_dbg_inc(&g_chan_dbg.lf_send_waiter_remove);
    }
    cc__chan_remove_waiter_list(&ch->send_waiters_head, &ch->send_waiters_tail, node);
    if (!ch->send_waiters_head)
        atomic_store_explicit(&ch->has_send_waiters, 0, memory_order_seq_cst);
}

static void cc__chan_remove_recv_waiter(CCChan* ch, cc__fiber_wait_node* node) {
    if (!ch || !node) return;
    if (!node->in_wait_list) {
        /* Node already removed by signal_recv_waiter — clear the Dekker flag. */
        if (!ch->recv_waiters_head)
            atomic_store_explicit(&ch->has_recv_waiters, 0, memory_order_seq_cst);
        return;
    }
    if (ch->use_lockfree) {
        cc__chan_dbg_inc(&g_chan_dbg.lf_recv_waiter_remove);
    }
    cc__chan_remove_waiter_list(&ch->recv_waiters_head, &ch->recv_waiters_tail, node);
    if (!ch->recv_waiters_head)
        atomic_store_explicit(&ch->has_recv_waiters, 0, memory_order_seq_cst);
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
                cc__chan_dbg_inc(&g_chan_dbg.lf_send_notify_cancel);
            }
            cc__chan_select_cancel_node(node);
            continue;
        }
        if (!cc__chan_waiter_ticket_valid_dbg(node, "wake_one_send")) {
            continue;
        }
        atomic_store_explicit(&node->notified, CC_CHAN_NOTIFY_SIGNAL, memory_order_release);
        if (ch->use_lockfree) {
            cc__chan_dbg_inc(&g_chan_dbg.lf_send_notify_signal);
        }
        if (node->is_select && node->select_group) {
            cc__select_wait_group* group = (cc__select_wait_group*)node->select_group;
            atomic_fetch_add_explicit(&group->signaled, 1, memory_order_release);
            cc__chan_dbg_select_event("signal_send", node);
        }
        if (ch->use_lockfree) {
            cc__chan_dbg_inc(&g_chan_dbg.lf_send_waiter_wake);
        }
        /* Do NOT clear has_send_waiters here — the woken fiber hasn't completed
         * its operation yet. Leave the flag set so the Dekker protocol continues
         * to protect the woken fiber until it either re-registers or finishes.
         * The flag will be cleared by cc__chan_remove_send_waiter when the fiber
         * runs and removes itself. */
        wake_batch_add(node->fiber);
        return;
    }
    /* All nodes were cancelled selects — list is now empty, clear flag */
    if (!ch->send_waiters_head)
        atomic_store_explicit(&ch->has_send_waiters, 0, memory_order_seq_cst);
}

/* Signal a recv waiter to wake and try the buffer (must hold ch->mu).
 * Does NOT set notified - the waiter remains in the queue and should check
 * the buffer. Uses simple FIFO - work stealing provides natural load balancing. */
static void cc__chan_signal_recv_waiter(CCChan* ch) {
    if (!ch) return;
    if (!ch->recv_waiters_head) {
        if (ch->use_lockfree && atomic_load_explicit(&ch->lfqueue_count, memory_order_relaxed) > 0) {
            cc__chan_dbg_inc(&g_chan_dbg.lf_recv_wake_no_waiter);
            cc__chan_dbg_inc(&ch->dbg_lf_recv_wake_no_waiter);
        }
        return;
    }
    /* Wake the first selectable waiter */
    for (cc__fiber_wait_node* node = ch->recv_waiters_head; node; node = node->next) {
        if (!cc__chan_select_try_win(node)) {
            if (ch->use_lockfree) {
                cc__chan_dbg_inc(&g_chan_dbg.lf_recv_notify_cancel);
            }
            cc__chan_select_cancel_node(node);
            continue;
        }
        if (!cc__chan_waiter_ticket_valid_dbg(node, "signal_recv")) {
            continue;
        }
        atomic_store_explicit(&node->notified, CC_CHAN_NOTIFY_SIGNAL, memory_order_release);
        if (ch->use_lockfree) {
            cc__chan_dbg_inc(&g_chan_dbg.lf_recv_notify_signal);
        }
        if (node->is_select && node->select_group) {
            cc__select_wait_group* group = (cc__select_wait_group*)node->select_group;
            atomic_fetch_add_explicit(&group->signaled, 1, memory_order_release);
            cc__chan_dbg_select_event("signal_recv", node);
        }
        if (ch->use_lockfree) {
            cc__chan_dbg_inc(&g_chan_dbg.lf_recv_waiter_wake);
            cc__chan_dbg_inc(&ch->dbg_lf_recv_waiter_wake);
        }
        wake_batch_add(node->fiber);
        return;
    }
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
            ch->recv_waiters_head = node->next;
            if (ch->recv_waiters_head) {
                ch->recv_waiters_head->prev = NULL;
            } else {
                ch->recv_waiters_tail = NULL;
            }
            node->next = node->prev = NULL;
            node->in_wait_list = 0;
            continue;
        }
        if (node->is_select && !cc__chan_select_try_win(node)) {
            ch->recv_waiters_head = node->next;
            if (ch->recv_waiters_head) {
                ch->recv_waiters_head->prev = NULL;
            } else {
                ch->recv_waiters_tail = NULL;
            }
            node->next = node->prev = NULL;
            node->in_wait_list = 0;
            cc__chan_select_cancel_node(node);
            continue;
        }
        if (!cc__chan_waiter_ticket_valid_dbg(node, "pop_recv")) {
            ch->recv_waiters_head = node->next;
            if (ch->recv_waiters_head) {
                ch->recv_waiters_head->prev = NULL;
            } else {
                ch->recv_waiters_tail = NULL;
            }
            node->next = node->prev = NULL;
            node->in_wait_list = 0;
            continue;
        }
        ch->recv_waiters_head = node->next;
        if (ch->recv_waiters_head) {
            ch->recv_waiters_head->prev = NULL;
        } else {
            ch->recv_waiters_tail = NULL;
        }
        node->next = node->prev = NULL;
        node->in_wait_list = 0;
        return node;
    }
    return NULL;
}

/* Wake one recv waiter for close (notified=3 means "woken by close") */
static void cc__chan_wake_one_recv_waiter_close(CCChan* ch) {
    if (!ch || !ch->recv_waiters_head) return;
    cc__fiber_wait_node* node = ch->recv_waiters_head;
    ch->recv_waiters_head = node->next;
    if (ch->recv_waiters_head) {
        ch->recv_waiters_head->prev = NULL;
    } else {
        ch->recv_waiters_tail = NULL;
    }
    node->next = node->prev = NULL;
    node->in_wait_list = 0;
    if (node->is_select && !cc__chan_select_try_win(node)) {
        if (ch->use_lockfree) {
            cc__chan_dbg_inc(&g_chan_dbg.lf_recv_notify_cancel);
        }
        cc__chan_select_cancel_node(node);
        return;
    }
    if (!cc__chan_waiter_ticket_valid_dbg(node, "wake_close_recv")) {
        return;
    }
    atomic_store_explicit(&node->notified, CC_CHAN_NOTIFY_CLOSE, memory_order_release);  /* close */
    if (ch->use_lockfree) {
        cc__chan_dbg_inc(&g_chan_dbg.lf_recv_notify_close);
    }
    if (node->is_select && node->select_group) {
        cc__select_wait_group* group = (cc__select_wait_group*)node->select_group;
    atomic_fetch_add_explicit(&group->signaled, 1, memory_order_release);
        cc__chan_dbg_select_event("signal_close_recv", node);
    }
    wake_batch_add(node->fiber);
}

/* Wake one send waiter for close (notified=3 means "woken by close") */
static void cc__chan_wake_one_send_waiter_close(CCChan* ch) {
    if (!ch || !ch->send_waiters_head) return;
    cc__fiber_wait_node* node = ch->send_waiters_head;
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
            cc__chan_dbg_inc(&g_chan_dbg.lf_send_notify_cancel);
        }
        cc__chan_select_cancel_node(node);
        return;
    }
    if (!cc__chan_waiter_ticket_valid_dbg(node, "wake_close_send")) {
        return;
    }
    atomic_store_explicit(&node->notified, CC_CHAN_NOTIFY_CLOSE, memory_order_release);  /* close */
    if (ch->use_lockfree) {
        cc__chan_dbg_inc(&g_chan_dbg.lf_send_notify_close);
    }
    if (node->is_select && node->select_group) {
        cc__select_wait_group* group = (cc__select_wait_group*)node->select_group;
    atomic_fetch_add_explicit(&group->signaled, 1, memory_order_release);
        cc__chan_dbg_select_event("signal_close_send", node);
    }
    wake_batch_add(node->fiber);
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
    atomic_store(&ch->recv_fairness_ctr, 0);
    
    if (cap > 1) {  /* Only use lock-free for cap > 1 (liblfds needs at least 2) */
        const char* disable_lf = getenv("CC_CHAN_NO_LOCKFREE");
        if (disable_lf && disable_lf[0] == '1') {
            return ch;  /* Force mutex-based path for debugging */
        }
        /* Buffered channel: allocate lock-free queue */
        size_t lfcap = next_power_of_2(cap);
        ch->lfqueue_cap = lfcap;
        const char* ring_env = getenv("CC_CHAN_RING_QUEUE");
        int prefer_ring = !(ring_env && ring_env[0] == '0');
        if (prefer_ring) {
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
                /* Ring preferred, but allocation failed: fall back to liblfds. */
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
        } else {
            /* macOS requires aligned_alloc size to be multiple of alignment */
            size_t alloc_size = sizeof(struct lfds711_queue_bmm_element) * lfcap;
            size_t align = LFDS711_PAL_ATOMIC_ISOLATION_IN_BYTES;
            alloc_size = ((alloc_size + align - 1) / align) * align;
            ch->lfqueue_elements = (struct lfds711_queue_bmm_element*)
                aligned_alloc(align, alloc_size);
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

void cc_chan_close(CCChan* ch) {
    if (!ch) return;
    if (ch->use_lockfree) {
        cc__chan_dbg_inc(&g_chan_dbg.lf_close_calls);
    }
    if (cc__chan_dbg_enabled()) {
        atomic_store_explicit(&g_chan_dbg_last_close, (uintptr_t)ch, memory_order_relaxed);
        atomic_fetch_add_explicit(&g_chan_dbg_close_seq, 1, memory_order_relaxed);
    }
    ch->fast_path_ok = 0;  /* Disable minimal fast path before taking lock */
    pthread_mutex_lock(&ch->mu);
    /* LP (§10 Close LP): OPEN -> CLOSED under channel mutex. */
    ch->closed = 1;
    pthread_cond_broadcast(&ch->not_empty);
    pthread_cond_broadcast(&ch->not_full);
    /* Wake all waiting fibers */
    cc__chan_wake_all_waiters(ch);
    pthread_mutex_unlock(&ch->mu);
    wake_batch_flush();  /* Flush fiber wakes immediately */
    cc__chan_signal_activity(ch);
}

void cc_chan_close_err(CCChan* ch, int err) {
    if (!ch) return;
    if (ch->use_lockfree) {
        cc__chan_dbg_inc(&g_chan_dbg.lf_close_calls);
    }
    if (cc__chan_dbg_enabled()) {
        atomic_store_explicit(&g_chan_dbg_last_close, (uintptr_t)ch, memory_order_relaxed);
        atomic_fetch_add_explicit(&g_chan_dbg_close_seq, 1, memory_order_relaxed);
    }
    ch->fast_path_ok = 0;  /* Disable minimal fast path before taking lock */
    pthread_mutex_lock(&ch->mu);
    /* LP (§10 Close LP): OPEN -> CLOSED under channel mutex. */
    ch->closed = 1;
    ch->tx_error_code = err;
    pthread_cond_broadcast(&ch->not_empty);
    pthread_cond_broadcast(&ch->not_full);
    /* Wake all waiting fibers */
    cc__chan_wake_all_waiters(ch);
    pthread_mutex_unlock(&ch->mu);
    wake_batch_flush();  /* Flush fiber wakes immediately */
    cc__chan_signal_activity(ch);
}

void cc_chan_rx_close_err(CCChan* ch, int err) {
    if (!ch) return;
    ch->fast_path_ok = 0;  /* Disable minimal fast path */
    pthread_mutex_lock(&ch->mu);
    ch->rx_error_closed = 1;
    ch->rx_error_code = err;
    pthread_cond_broadcast(&ch->not_full);  /* Wake senders */
    /* Wake fiber send waiters too */
    while (ch->send_waiters_head) {
        cc__chan_wake_one_send_waiter_close(ch);
    }
    pthread_mutex_unlock(&ch->mu);
    wake_batch_flush();  /* Flush fiber wakes immediately */
    cc__chan_signal_activity(ch);
}

void cc_chan_free(CCChan* ch) {
    if (!ch) return;
    
    /* Dump debug counters on first free (once per process) */
    static int dumped = 0;
    if (!dumped && cc__chan_dbg_enabled()) {
        dumped = 1;
        cc__chan_debug_dump_global();
    }
    
    /* For owned channels, destroy remaining items in the buffer */
    if (ch->is_owned && ch->on_destroy.fn && ch->buf && ch->elem_size > 0) {
        pthread_mutex_lock(&ch->mu);
        if (ch->use_lockfree && ch->elem_size <= sizeof(void*)) {
            /* Lock-free path with small elements: items stored directly in queue (zero-copy) */
            void* queue_val = NULL;
            while (cc__queue_dequeue_raw(ch, &queue_val) == 1) {
                /* queue_val IS the item value (not a slot index) */
                intptr_t item_val = 0;
                memcpy(&item_val, &queue_val, ch->elem_size);
                ch->on_destroy.fn(ch->on_destroy.env, item_val);
            }
        } else if (ch->use_lockfree) {
            /* Lock-free path with large elements: items in buffer, queue holds slot indices */
            void* key = NULL;
            while (lfds711_queue_bmm_dequeue(&ch->lfqueue_state, NULL, &key) == 1) {
                size_t slot_idx = (size_t)(uintptr_t)key;
                char* item_ptr = (char*)ch->buf + (slot_idx * ch->elem_size);
                intptr_t item_val = 0;
                memcpy(&item_val, item_ptr, ch->elem_size < sizeof(intptr_t) ? ch->elem_size : sizeof(intptr_t));
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
        if (ch->use_ring_queue && elem_size > sizeof(void*)) {
            /* Ring backend is optimized for small payloads only. */
            ch->use_ring_queue = 0;
            ch->use_lockfree = 0;
        }
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
         * lockfree, buffered, small elements, not owned/ordered/sync. */
        ch->fast_path_ok = (cc__chan_minimal_path_enabled() &&
                            ch->use_lockfree && ch->cap > 0 && ch->buf &&
                            elem_size <= sizeof(void*) &&
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
                if (deadline) {
                    struct timespec now;
                    clock_gettime(CLOCK_REALTIME, &now);
                    if (now.tv_sec > deadline->tv_sec ||
                        (now.tv_sec == deadline->tv_sec && now.tv_nsec >= deadline->tv_nsec)) {
                        return ETIMEDOUT;
                    }
                }
                cc__fiber_wait_node node = {0};
                node.fiber = fiber;
                atomic_store(&node.notified, 0);
                cc__chan_add_send_waiter(ch, &node);

                pthread_mutex_unlock(&ch->mu);
                (void)cc__chan_wait_notified_mark_close(&node);
                pthread_mutex_lock(&ch->mu);
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
            if (deadline) {
                struct timespec now;
                clock_gettime(CLOCK_REALTIME, &now);
                if (now.tv_sec > deadline->tv_sec ||
                    (now.tv_sec == deadline->tv_sec && now.tv_nsec >= deadline->tv_nsec)) {
                    return ETIMEDOUT;
                }
            }
            cc__fiber_wait_node node = {0};
            node.fiber = fiber;
            atomic_store(&node.notified, 0);
            cc__chan_add_send_waiter(ch, &node);

            pthread_mutex_unlock(&ch->mu);
            cc__fiber_set_park_obj(ch);
            /* Re-check closed after unlock: if close raced between the
             * while-loop condition and add_send_waiter, wake_all_waiters
             * already ran and won't find us.  Bail out to avoid stranding. */
            if (ch->closed || ch->rx_error_closed) {
                cc_chan_lock(ch);
                cc__chan_remove_send_waiter(ch, &node);
                pthread_mutex_unlock(&ch->mu);
                break;
            }
            (void)cc__chan_wait_notified_mark_close(&node);
            pthread_mutex_lock(&ch->mu);
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
        cc__chan_debug_check_recv_close(ch, "wait_full_close");
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
        wake_batch_flush();  /* Flush wakes immediately for rendezvous */

        

        if (fiber) {
            /* Fiber-aware blocking */
            while (!ch->closed && !ch->rv_has_value) {
                if (deadline) {
                    struct timespec now;
                    clock_gettime(CLOCK_REALTIME, &now);
                    if (now.tv_sec > deadline->tv_sec ||
                        (now.tv_sec == deadline->tv_sec && now.tv_nsec >= deadline->tv_nsec)) {
                        ch->rv_recv_waiters--;
                        return ETIMEDOUT;
                    }
                }
                cc__fiber_wait_node node = {0};
                node.fiber = fiber;
                atomic_store(&node.notified, 0);
                cc__chan_add_recv_waiter(ch, &node);

                pthread_mutex_unlock(&ch->mu);

                /* Return-aware boundary wait (first migrated call site). */
                cc_sched_wait_result wait_rc = cc__chan_wait_notified_mark_close(&node);

                pthread_mutex_lock(&ch->mu);
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
            if (ch->send_waiters_head) {
                cc__chan_debug_invariant(ch, "wait_empty_rendezvous",
                                         "closed with pending send waiters");
            }
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
            if (deadline) {
                struct timespec now;
                clock_gettime(CLOCK_REALTIME, &now);
                if (now.tv_sec > deadline->tv_sec ||
                    (now.tv_sec == deadline->tv_sec && now.tv_nsec >= deadline->tv_nsec)) {
                    return ETIMEDOUT;
                }
            }
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
                break;
            }
            (void)cc__chan_wait_notified_mark_close(&node);
            pthread_mutex_lock(&ch->mu);

            int notified = atomic_load_explicit(&node.notified, memory_order_acquire);
            if (notified == CC_CHAN_NOTIFY_SIGNAL) {
                atomic_store_explicit(&node.notified, CC_CHAN_NOTIFY_NONE, memory_order_release);
                cc__chan_remove_recv_waiter(ch, &node);
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
        pthread_cond_signal(&ch->not_empty);
        cc__chan_signal_recv_waiter(ch);
        cc__chan_signal_activity(ch);
        return;
    }
    /* Buffered: signal waiters */
    void *slot = (uint8_t*)ch->buf + ch->tail * ch->elem_size;
    channel_store_slot(slot, value, ch->elem_size);
    ch->tail = (ch->tail + 1) % ch->cap;
    ch->count++;
    pthread_cond_signal(&ch->not_empty);
    cc__chan_signal_recv_waiter(ch);
    cc__chan_signal_activity(ch);
}

static void cc_chan_dequeue(CCChan* ch, void* out_value) {
    if (ch->cap == 0) {
        /* Unbuffered rendezvous: wake one sender waiting for consumption. */
        channel_load_slot(ch->buf, out_value, ch->elem_size);
        ch->rv_has_value = 0;
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
    pthread_cond_signal(&ch->not_full);
    cc__chan_wake_one_send_waiter(ch);
    cc__chan_signal_activity(ch);
}

/* ============================================================================
 * Lock-Free Queue Operations for Buffered Channels
 * ============================================================================
 * These use liblfds bounded MPMC queue for the hot path.
 * 
 * Data storage strategy:
 * - For elem_size <= sizeof(void*): store data directly in queue value pointer
 * - For elem_size > sizeof(void*): copy data to buffer slot, store pointer in queue
 * 
 * No separate count tracking - liblfds handles full/empty detection internally
 * via sequence numbers.
 */

static inline int cc__ring_enqueue_raw(CCChan* ch, void* queue_val) {
    size_t pos = atomic_load_explicit(&ch->ring_tail, memory_order_relaxed);
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
    }
}

static inline int cc__ring_dequeue_raw(CCChan* ch, void** out_val) {
    size_t pos = atomic_load_explicit(&ch->ring_head, memory_order_relaxed);
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

/* Helper: try lock-free enqueue without incrementing inflight counter.
 * Caller MUST manage lfqueue_inflight (inc before, dec after).
 * Must NOT hold ch->mu when calling this.
 * ONLY valid for small elements (elem_size <= sizeof(void*)). */
static int cc__chan_try_enqueue_lockfree_impl(CCChan* ch, const void* value) {
    if (!ch->use_lockfree || ch->cap == 0 || !ch->buf) return EAGAIN;
    if (ch->elem_size > sizeof(void*)) {
        fprintf(stderr, "BUG: cc__chan_try_enqueue_lockfree_impl called with large element (size=%zu)\n", ch->elem_size);
        return EAGAIN;
    }
    
    /* Small element: store directly in pointer (zero-copy for ints, pointers, etc.) */
    void *queue_val = NULL;
    memcpy(&queue_val, value, ch->elem_size);
    
    /* Try to enqueue - liblfds returns 1 on success, 0 if full */
    cc__chan_dbg_inc(&g_chan_dbg.lf_enq_attempt);
    /* Note: inflight managed by caller */
    int ok = cc__queue_enqueue_raw(ch, queue_val);
    if (ok) {
        atomic_fetch_add_explicit(&ch->lfqueue_count, 1, memory_order_release);
        cc__chan_dbg_inc(&g_chan_dbg.lf_enq_ok);
        cc__chan_dbg_inc(&ch->dbg_lf_enq_ok);
    } else {
        cc__chan_dbg_inc(&g_chan_dbg.lf_enq_fail);
        if (cc__chan_dbg_enabled()) {
            int count = atomic_load_explicit(&ch->lfqueue_count, memory_order_acquire);
            if (count < (int)ch->cap) {
                lfds711_pal_uint_t lf_ri = ch->lfqueue_state.read_index;
                lfds711_pal_uint_t lf_wi = ch->lfqueue_state.write_index;
                lfds711_pal_uint_t lf_ne = ch->lfqueue_state.number_elements;
                lfds711_pal_uint_t lf_mask = ch->lfqueue_state.mask;
                lfds711_pal_uint_t lf_est = lf_wi - lf_ri;
                struct lfds711_queue_bmm_element* lf_elem =
                    &ch->lfqueue_state.element_array[lf_wi & lf_mask];
                lfds711_pal_uint_t lf_seq = lf_elem->sequence_number;
                lfds711_pal_int_t lf_diff = (lfds711_pal_int_t)lf_seq - (lfds711_pal_int_t)lf_wi;
                fprintf(stderr,
                        "CC_CHAN_DEBUG: enqueue_fail_count_lt_cap ch=%p count=%d cap=%zu ne=%" PRIu64 " mask=%" PRIu64 " ri=%" PRIu64 " wi=%" PRIu64 " est=%" PRIu64 " seq=%" PRIu64 " diff=%" PRId64 "\n",
                        (void*)ch,
                        count,
                        ch->cap,
                        (uint64_t)lf_ne,
                        (uint64_t)lf_mask,
                        (uint64_t)lf_ri,
                        (uint64_t)lf_wi,
                        (uint64_t)lf_est,
                        (uint64_t)lf_seq,
                        (int64_t)lf_diff);
            }
        }
    }
    return ok ? 0 : EAGAIN;
}

/* Wrapper that manages inflight counter automatically */
static int cc_chan_try_enqueue_lockfree(CCChan* ch, const void* value) {
    atomic_fetch_add_explicit(&ch->lfqueue_inflight, 1, memory_order_relaxed);
    int rc = cc__chan_try_enqueue_lockfree_impl(ch, value);
    atomic_fetch_sub_explicit(&ch->lfqueue_inflight, 1, memory_order_relaxed);
    return rc;
}

/* Minimal-path enqueue: absolute minimum work.  No debug counters, no lfqueue_count,
 * no signal_activity, no maybe_yield.  Used only from the branded fast_path_ok path
 * where the caller has already checked the brand. */
static inline int cc__chan_enqueue_lockfree_minimal(CCChan* ch, const void* value, int* old_count_out) {
    void *queue_val = NULL;
    memcpy(&queue_val, value, ch->elem_size);
    if (!cc__queue_enqueue_raw(ch, queue_val))
        return EAGAIN;
    /* Must maintain lfqueue_count so receivers can decide whether to park.
     * Without this, a receiver checking lfqueue_count sees 0 and parks
     * even though there IS data in the queue — causing a deadlock. */
    int old_count = atomic_fetch_add_explicit(&ch->lfqueue_count, 1, memory_order_release);
    if (old_count_out) *old_count_out = old_count;
    return 0;
}

/* Minimal-path dequeue: absolute minimum work. */
static inline int cc__chan_dequeue_lockfree_minimal(CCChan* ch, void* out_value, int* old_count_out) {
    void *val;
    if (!cc__queue_dequeue_raw(ch, &val))
        return EAGAIN;
    int old_count = atomic_fetch_sub_explicit(&ch->lfqueue_count, 1, memory_order_release);
    if (old_count_out) *old_count_out = old_count;
    memcpy(out_value, &val, ch->elem_size);
    return 0;
}

/* Fast-path enqueue: no guard checks, no inflight tracking.
 * Caller MUST have already verified use_lockfree, cap>0, buf, elem_size<=sizeof(void*).
 * Use only on the hot send path where the channel is known to be open and valid. */
static inline int cc__chan_enqueue_lockfree_fast(CCChan* ch, const void* value, int* old_count_out) {
    void *queue_val = NULL;
    memcpy(&queue_val, value, ch->elem_size);
    cc__chan_dbg_inc(&g_chan_dbg.lf_enq_attempt);
    int ok = cc__queue_enqueue_raw(ch, queue_val);
    if (ok) {
        int old_count = atomic_fetch_add_explicit(&ch->lfqueue_count, 1, memory_order_release);
        if (old_count_out) *old_count_out = old_count;
        cc__chan_dbg_inc(&g_chan_dbg.lf_enq_ok);
        cc__chan_dbg_inc(&ch->dbg_lf_enq_ok);
    } else {
        cc__chan_dbg_inc(&g_chan_dbg.lf_enq_fail);
    }
    return ok ? 0 : EAGAIN;
}

/* Fast-path dequeue: no guard checks.
 * Caller MUST have already verified use_lockfree, cap>0, buf, elem_size<=sizeof(void*). */
static inline int cc__chan_dequeue_lockfree_fast(CCChan* ch, void* out_value, int* old_count_out) {
    void *val;
    cc__chan_dbg_inc(&g_chan_dbg.lf_deq_attempt);
    int ok = cc__queue_dequeue_raw(ch, &val);
    if (!ok) {
        cc__chan_dbg_inc(&g_chan_dbg.lf_deq_fail);
        return EAGAIN;
    }
    cc__chan_dbg_inc(&g_chan_dbg.lf_deq_ok);
    cc__chan_dbg_inc(&ch->dbg_lf_deq_ok);
    int old_count = atomic_fetch_sub_explicit(&ch->lfqueue_count, 1, memory_order_release);
    if (old_count_out) *old_count_out = old_count;
    memcpy(out_value, &val, ch->elem_size);
    return 0;
}

/* Try lock-free dequeue. Returns 0 on success, EAGAIN if empty.
 * Must NOT hold ch->mu when calling this.
 * ONLY valid for small elements (elem_size <= sizeof(void*)). */
static int cc_chan_try_dequeue_lockfree(CCChan* ch, void* out_value) {
    if (!ch->use_lockfree || ch->cap == 0 || !ch->buf) return EAGAIN;
    if (ch->elem_size > sizeof(void*)) {
        fprintf(stderr, "BUG: cc_chan_try_dequeue_lockfree called with large element (size=%zu)\n", ch->elem_size);
        return EAGAIN;
    }
    
    void *val;
    
    /* Try to dequeue - liblfds returns 1 on success, 0 if empty */
    cc__chan_dbg_inc(&g_chan_dbg.lf_deq_attempt);
    int ok = cc__queue_dequeue_raw(ch, &val);
    if (!ok) {
        cc__chan_dbg_inc(&g_chan_dbg.lf_deq_fail);
        if (cc__chan_dbg_enabled()) {
            int count = atomic_load_explicit(&ch->lfqueue_count, memory_order_acquire);
            if (count > 0 && !ch->use_ring_queue) {
                lfds711_pal_uint_t lf_ri = ch->lfqueue_state.read_index;
                lfds711_pal_uint_t lf_wi = ch->lfqueue_state.write_index;
                lfds711_pal_uint_t lf_ne = ch->lfqueue_state.number_elements;
                lfds711_pal_uint_t lf_mask = ch->lfqueue_state.mask;
                lfds711_pal_uint_t lf_est = lf_wi - lf_ri;
                fprintf(stderr,
                        "CC_CHAN_DEBUG: dequeue_fail_count_gt_zero ch=%p count=%d cap=%zu ne=%" PRIu64 " mask=%" PRIu64 " ri=%" PRIu64 " wi=%" PRIu64 " est=%" PRIu64 "\n",
                        (void*)ch,
                        count,
                        ch->cap,
                        (uint64_t)lf_ne,
                        (uint64_t)lf_mask,
                        (uint64_t)lf_ri,
                        (uint64_t)lf_wi,
                        (uint64_t)lf_est);
            }
        }
        return EAGAIN;
    }
    cc__chan_dbg_inc(&g_chan_dbg.lf_deq_ok);
    cc__chan_dbg_inc(&ch->dbg_lf_deq_ok);
    atomic_fetch_sub_explicit(&ch->lfqueue_count, 1, memory_order_release);
    
    /* Small element: stored directly in pointer */
    memcpy(out_value, &val, ch->elem_size);
    
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
        cc__chan_dbg_inc(&g_chan_dbg.lf_close_drain_calls);
    }
    int loops = 0;
    while (1) {
        if (cc_chan_try_dequeue_lockfree(ch, out_value) == 0) {
            if (cc__chan_dbg_enabled() && loops > 0) {
                fprintf(stderr, "CC_CHAN_DEBUG: drain_got_item ch=%p loops=%d\n", (void*)ch, loops);
            }
            return 0;
        }
        int inflight = atomic_load_explicit(&ch->lfqueue_inflight, memory_order_acquire);
        int count = atomic_load_explicit(&ch->lfqueue_count, memory_order_acquire);
        if (inflight == 0) {
            if (cc__chan_dbg_enabled()) {
                fprintf(stderr, "CC_CHAN_DEBUG: drain_epipe ch=%p count=%d inflight=%d loops=%d\n",
                        (void*)ch, count, inflight, loops);
            }
            cc__chan_debug_check_recv_close(ch, "lf_drain_close");
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
        loops++;
    }
}

/* ============================================================================
 * Unbuffered Channel (Rendezvous) Operations
 * ============================================================================
 * Unbuffered channels use mutex+condvar for direct handoff between sender
 * and receiver. This is similar to Go's unbuffered channel implementation.
 */

/* Direct handoff rendezvous helpers (cap == 0). Expects ch->mu locked. */
static int cc_chan_send_unbuffered(CCChan* ch, const void* value, const struct timespec* deadline) {
    cc__fiber* fiber = cc__fiber_in_context() ? cc__fiber_current() : NULL;
    int err = 0;

    while (!ch->closed && !ch->rx_error_closed) {
        /* If a receiver is waiting, handoff directly */
        cc__fiber_wait_node* rnode = cc__chan_pop_recv_waiter(ch);
        if (rnode) {
            cc__chan_dbg_inc(&ch->dbg_rv_send_handoff);
            channel_store_slot(rnode->data, value, ch->elem_size);
            atomic_store_explicit(&rnode->notified, CC_CHAN_NOTIFY_DATA, memory_order_release);
            if (rnode->is_select) atomic_fetch_add_explicit(&g_dbg_select_data_set, 1, memory_order_relaxed);
            if (ch->rv_recv_waiters > 0) ch->rv_recv_waiters--;
            /* IMPORTANT: Increment signaled BEFORE waking the fiber.
             * Otherwise the fiber could wake, check signaled (unchanged), and re-park
             * before we increment it - causing a lost wakeup. */
            if (rnode->is_select && rnode->select_group) {
                cc__select_wait_group* group = (cc__select_wait_group*)rnode->select_group;
                int sel = atomic_load_explicit(&group->selected_index, memory_order_acquire);
                int sig_before = atomic_load_explicit(&group->signaled, memory_order_acquire);
                if (cc__chan_dbg_enabled() && sel == -1) {
                    fprintf(stderr, "CC_CHAN_DEBUG: BUG! handoff_send but selected_index=-1 group=%p node=%p idx=%zu\n",
                            (void*)group, (void*)rnode, rnode->select_index);
                }
                atomic_fetch_add_explicit(&group->signaled, 1, memory_order_release);
                if (cc__chan_dbg_verbose_enabled()) {
                    int sig_after = atomic_load_explicit(&group->signaled, memory_order_acquire);
                    fprintf(stderr, "CC_CHAN_DEBUG: handoff_send_signaled group=%p fiber=%p sel=%d sig=%d->%d\n",
                            (void*)group, (void*)rnode->fiber, sel, sig_before, sig_after);
                }
                cc__chan_dbg_select_event("handoff_send", rnode);
            }
            if (rnode->fiber) {
                if (cc__chan_dbg_enabled() && rnode->is_select && rnode->select_group) {
                    cc__select_wait_group* g = (cc__select_wait_group*)rnode->select_group;
                    fprintf(stderr, "CC_CHAN_DEBUG: wake_batch_add_handoff fiber=%p group=%p sel=%d sig=%d\n",
                            (void*)rnode->fiber, (void*)g,
                            atomic_load_explicit(&g->selected_index, memory_order_acquire),
                            atomic_load_explicit(&g->signaled, memory_order_acquire));
                }
                wake_batch_add(rnode->fiber);
            } else {
                pthread_cond_signal(&ch->not_empty);
            }
            wake_batch_flush();
            cc__chan_signal_activity(ch);
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
                cc__fiber_wait_node* rnode2 = cc__chan_pop_recv_waiter(ch);
                if (rnode2) {
                    /* Found a receiver! Remove ourselves from send wait list and do handoff. */
                    cc__chan_dbg_inc(&ch->dbg_rv_send_inner_handoff);
                    cc__chan_remove_send_waiter(ch, &node);
                    channel_store_slot(rnode2->data, value, ch->elem_size);
                    atomic_store_explicit(&rnode2->notified, CC_CHAN_NOTIFY_DATA, memory_order_release);
                    if (rnode2->is_select) atomic_fetch_add_explicit(&g_dbg_select_data_set, 1, memory_order_relaxed);
                    if (ch->rv_recv_waiters > 0) ch->rv_recv_waiters--;
                    if (rnode2->is_select && rnode2->select_group) {
                        cc__select_wait_group* group = (cc__select_wait_group*)rnode2->select_group;
                        atomic_fetch_add_explicit(&group->signaled, 1, memory_order_release);
                    }
                    if (rnode2->fiber) {
                        wake_batch_add(rnode2->fiber);
                    } else {
                        pthread_cond_signal(&ch->not_empty);
                    }
                    wake_batch_flush();
                    cc__chan_signal_activity(ch);
                    return 0;
                }
                pthread_mutex_unlock(&ch->mu);
                if (!atomic_load_explicit(&node.notified, memory_order_acquire)) {
                    cc__fiber_set_park_obj(ch);
                    cc__chan_dbg_inc(&ch->dbg_rv_send_parked);
                    (void)cc__chan_wait_notified_mark_close(&node);
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
                cc__chan_dbg_inc(&ch->dbg_rv_send_got_signal);
                atomic_store_explicit(&node.notified, CC_CHAN_NOTIFY_NONE, memory_order_release);
                cc__chan_remove_send_waiter(ch, &node);
                continue;
            }
            if (notify_val == CC_CHAN_NOTIFY_DATA) {
                /* notified=1 means a receiver actually took our data.
                 * The receiver already popped us from the list. */
                cc__chan_dbg_inc(&ch->dbg_rv_send_got_data);
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
            cc__chan_dbg_inc(&ch->dbg_rv_send_got_zero);
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
        cc__fiber_wait_node* snode = cc__chan_pop_send_waiter(ch);
        if (snode) {
            cc__chan_dbg_inc(&ch->dbg_rv_recv_handoff);
            channel_load_slot(snode->data, out_value, ch->elem_size);
            atomic_store_explicit(&snode->notified, CC_CHAN_NOTIFY_DATA, memory_order_release);
            if (snode->is_select) atomic_fetch_add_explicit(&g_dbg_select_data_set, 1, memory_order_relaxed);
            /* IMPORTANT: Increment signaled BEFORE waking the fiber. */
            if (snode->is_select && snode->select_group) {
                cc__select_wait_group* group = (cc__select_wait_group*)snode->select_group;
                atomic_fetch_add_explicit(&group->signaled, 1, memory_order_release);
                cc__chan_dbg_select_event("handoff_recv", snode);
            }
            if (snode->fiber) {
                wake_batch_add(snode->fiber);
            } else {
                pthread_cond_signal(&ch->not_full);
            }
            wake_batch_flush();
            cc__chan_signal_activity(ch);
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
                    cc__fiber_set_park_obj(ch);
                    cc__chan_dbg_inc(&ch->dbg_rv_recv_parked);
                    (void)cc__chan_wait_notified_mark_close(&node);
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
                cc__chan_dbg_inc(&ch->dbg_rv_recv_got_signal);
                atomic_store_explicit(&node.notified, CC_CHAN_NOTIFY_NONE, memory_order_release);
                cc__chan_remove_recv_waiter(ch, &node);
                if (ch->rv_recv_waiters > 0) ch->rv_recv_waiters--;
                continue;
            }
            if (notify_val == CC_CHAN_NOTIFY_DATA) {
                /* notified=1 means a sender actually delivered data.
                 * The sender already popped us from the list. */
                cc__chan_dbg_inc(&ch->dbg_rv_recv_got_data);
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
            cc__chan_dbg_inc(&ch->dbg_rv_recv_got_zero);
            cc__chan_remove_recv_waiter(ch, &node);
            if (ch->rv_recv_waiters > 0) ch->rv_recv_waiters--;
        }

        if (ch->closed) {
            /* node already removed above */
            if (ch->send_waiters_head) {
                cc__chan_debug_invariant(ch, "recv_unbuffered", "closed with pending send waiters");
            }
            return ch->tx_error_code ? ch->tx_error_code : EPIPE;
        }
        if (deadline && err == ETIMEDOUT) {
            /* node already removed above */
            return ETIMEDOUT;
        }
    }
    if (ch->send_waiters_head) {
        cc__chan_debug_invariant(ch, "recv_unbuffered", "closed with pending send waiters");
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
     * Checks recv_waiters_head to wake parked receivers (pipeline correctness). */
    if (ch->fast_path_ok && value_size == ch->elem_size) {
        if (cc__chan_enqueue_lockfree_minimal(ch, value, NULL) == 0) {
            if (__builtin_expect(atomic_load_explicit(&ch->has_recv_waiters, memory_order_seq_cst) != 0, 0)) {
                cc__chan_dbg_inc(&g_chan_dbg.lf_has_recv_waiters_true);
                cc__chan_dbg_inc(&g_chan_dbg.lf_wake_lock_send);
                cc_chan_lock(ch);
                cc__chan_signal_recv_waiter(ch);
                pthread_mutex_unlock(&ch->mu);
                wake_batch_flush();
            } else {
                cc__chan_dbg_inc(&g_chan_dbg.lf_has_recv_waiters_false);
            }
            if (__builtin_expect(cc__fiber_in_context() != 0, 1)) {
                if (++cc__tls_lf_ops >= CC_LF_YIELD_INTERVAL) {
                    cc__tls_lf_ops = 0;
                    cc__fiber_yield_global();
                }
            }
            return 0;
        }
        /* Buffer full — fall through to full path for yield-retry / blocking */
    }
    if (!ch || !value || value_size == 0) return EINVAL;
    cc__chan_dbg_inc(&ch->dbg_lf_send_calls);
    
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
    int timing = channel_timing_enabled();
    uint64_t t0 = timing ? channel_rdtsc() : 0;
    uint64_t t_lock = 0;
    uint64_t t_enqueue = 0;
    uint64_t t_wake = 0;
    
    /* Lock-free fast path for buffered channels with small elements.
     * Large elements (> sizeof(void*)) use mutex path to avoid slot wrap-around race. */
    if (ch->use_lockfree && ch->cap > 0 && ch->elem_size == value_size && ch->buf &&
        ch->elem_size <= sizeof(void*)) {
        /* Check closed flag (relaxed read is fine, we'll verify under lock if needed) */
        if (ch->closed) return EPIPE;
        /* Check rx error closed (upstream error propagation) */
        if (ch->rx_error_closed) return ch->rx_error_code;
        
        /* Direct handoff: if receivers waiting, give item directly to one.
         * This must be done under lock to coordinate with the fair queue. */
        if (atomic_load_explicit(&ch->has_recv_waiters, memory_order_seq_cst)) {
            cc_chan_lock(ch);
            if (ch->closed) { pthread_mutex_unlock(&ch->mu); return EPIPE; }
            if (ch->rx_error_closed) { pthread_mutex_unlock(&ch->mu); return ch->rx_error_code; }
            cc__fiber_wait_node* rnode = cc__chan_pop_recv_waiter(ch);
            if (rnode) {
                /* Direct handoff to waiting receiver */
                channel_store_slot(rnode->data, value, ch->elem_size);
                if (ch->use_lockfree) {
                    cc__chan_dbg_inc(&ch->dbg_lf_direct_send);
                    cc__chan_dbg_inc(&g_chan_dbg.lf_direct_send);
                }
                if (cc__chan_dbg_enabled()) {
                    fprintf(stderr, "CC_CHAN_DEBUG: direct_send ch=%p node=%p fiber=%p in_list=%d notified=%d\n", 
                            (void*)ch, (void*)rnode, (void*)rnode->fiber, rnode->in_wait_list,
                            atomic_load_explicit(&rnode->notified, memory_order_relaxed));
                }
                atomic_store_explicit(&rnode->notified, CC_CHAN_NOTIFY_DATA, memory_order_release);
                if (rnode->is_select) atomic_fetch_add_explicit(&g_dbg_select_data_set, 1, memory_order_relaxed);
                /* IMPORTANT: Increment signaled BEFORE waking the fiber. */
                if (rnode->is_select && rnode->select_group) {
                    cc__select_wait_group* group = (cc__select_wait_group*)rnode->select_group;
                    atomic_fetch_add_explicit(&group->signaled, 1, memory_order_release);
                }
                if (rnode->fiber) {
                    wake_batch_add(rnode->fiber);
                } else {
                    pthread_cond_signal(&ch->not_empty);
                }
                /* Signal the next head to try the buffer */
                cc__chan_signal_recv_waiter(ch);
                pthread_mutex_unlock(&ch->mu);
                wake_batch_flush();
                if (timing) {
                    uint64_t done = channel_rdtsc();
                    channel_timing_record_send(t0, t0, done, done, done);
                }
                cc__chan_signal_activity(ch);
                return 0;
            }
            pthread_mutex_unlock(&ch->mu);
        }
        
        /* No waiters - try lock-free enqueue to buffer (fast path, no inflight) */
        int rc = cc__chan_enqueue_lockfree_fast(ch, value, NULL);
        if (rc != 0 && !ch->closed && cc__fiber_in_context()) {
            /* Buffer full — yield to let the receiver fiber run, then retry
             * once before falling to the expensive blocking path.
             * Use global yield so fibers on other workers can also make progress. */
            cc__fiber_yield_global();
            rc = cc__chan_enqueue_lockfree_fast(ch, value, NULL);
        }
        if (rc == 0) {
            if (timing) {
                uint64_t done = channel_rdtsc();
                channel_timing_record_send(t0, t0, done, done, done);
            }
            /* Signal any waiters that might have joined the queue.
             * Use atomic Dekker flag — recv_waiters_head is mutex-protected
             * and cannot be read safely without the lock. */
            if (atomic_load_explicit(&ch->has_recv_waiters, memory_order_seq_cst)) {
                cc__chan_dbg_inc(&g_chan_dbg.lf_has_recv_waiters_true);
                cc__chan_dbg_inc(&g_chan_dbg.lf_wake_lock_send);
                cc_chan_lock(ch);
                cc__chan_signal_recv_waiter(ch);
                pthread_mutex_unlock(&ch->mu);
                wake_batch_flush();
            } else {
                cc__chan_dbg_inc(&g_chan_dbg.lf_has_recv_waiters_false);
            }
            cc__chan_signal_activity(ch);
            cc__chan_maybe_yield();
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
    if (timing) t_lock = channel_rdtsc();
    int err = cc_chan_ensure_buf(ch, value_size);
    if (err != 0) { pthread_mutex_unlock(&ch->mu); return err; }
    if (ch->closed) {
        if (cc__chan_dbg_enabled()) {
            int count = atomic_load_explicit(&ch->lfqueue_count, memory_order_acquire);
            fprintf(stderr, "CC_CHAN_DEBUG: send_epipe_early ch=%p count=%d cap=%zu\n",
                    (void*)ch, count, ch->cap);
        }
        pthread_mutex_unlock(&ch->mu);
        return EPIPE;
    }
    if (ch->rx_error_closed) { pthread_mutex_unlock(&ch->mu); return ch->rx_error_code; }
    
    /* Unbuffered (rendezvous) channel - direct handoff */
    if (ch->cap == 0) {
        err = cc_chan_send_unbuffered(ch, value, NULL);
        pthread_mutex_unlock(&ch->mu);
        wake_batch_flush();
        return err;
    }
    
    /* Buffered channel - try lock-free again under mutex (for initial setup case) */
    if (ch->use_lockfree && ch->elem_size <= sizeof(void*)) {
        pthread_mutex_unlock(&ch->mu);
        int rc = cc_chan_try_enqueue_lockfree(ch, value);
        if (rc == 0) {
            if (timing) {
                uint64_t done = channel_rdtsc();
                channel_timing_record_send(t0, t_lock, done, done, done);
            }
            cc_chan_lock(ch);
            cc__chan_signal_recv_waiter(ch);
            pthread_cond_signal(&ch->not_empty);
            pthread_mutex_unlock(&ch->mu);
            wake_batch_flush();
            cc__chan_signal_activity(ch);
            return 0;
        }
        /* Still full - need to wait */
        cc_chan_lock(ch);
    }
    
    /* Mutex-based blocking path for lock-free channels with small elements */
    if (ch->use_lockfree && ch->elem_size <= sizeof(void*)) {
        /* For lock-free channels, wait using count approximation */
        
        cc__fiber* fiber = cc__fiber_in_context() ? cc__fiber_current() : NULL;
        
        while (!ch->closed) {
            /* Try lock-free enqueue */
            /* Increment inflight BEFORE unlocking to prevent drain race.
             * If we unlock first, drain might see inflight=0 and queue empty,
             * returning EPIPE before we enqueue. */
            atomic_fetch_add_explicit(&ch->lfqueue_inflight, 1, memory_order_relaxed);
            pthread_mutex_unlock(&ch->mu);
            int rc = cc__chan_try_enqueue_lockfree_impl(ch, value);
            atomic_fetch_sub_explicit(&ch->lfqueue_inflight, 1, memory_order_relaxed);
            if (rc == 0) {
                
                if (timing) t_enqueue = channel_rdtsc();
                cc_chan_lock(ch);
                cc__chan_signal_recv_waiter(ch);
                pthread_cond_signal(&ch->not_empty);
                pthread_mutex_unlock(&ch->mu);
                if (timing) t_wake = channel_rdtsc();
                wake_batch_flush();
                cc__chan_signal_activity(ch);
                if (timing) {
                    uint64_t done = channel_rdtsc();
                    channel_timing_record_send(t0, t_lock, t_enqueue, t_wake, done);
                }
                return 0;
            }
            if (fiber) {
                int count = atomic_load_explicit(&ch->lfqueue_count, memory_order_acquire);
                if (count < (int)ch->cap && !ch->closed) {
                    cc__fiber_yield();
                    cc_chan_lock(ch);
                    continue;
                }
            }
            cc_chan_lock(ch);
            
            /* Wait for space */
            if (fiber) {
                cc__fiber_wait_node node = {0};
                node.fiber = fiber;
                atomic_store(&node.notified, 0);
                cc__chan_add_send_waiter(ch, &node);
                pthread_mutex_unlock(&ch->mu);
                cc__fiber_set_park_obj(ch);
                /* Re-check enqueue before parking to avoid missed wakeups.
                 * This authoritative queue op replaces the old count heuristic. */
                atomic_fetch_add_explicit(&ch->lfqueue_inflight, 1, memory_order_relaxed);
                int rc = cc__chan_try_enqueue_lockfree_impl(ch, value);
                atomic_fetch_sub_explicit(&ch->lfqueue_inflight, 1, memory_order_relaxed);
                if (rc == 0) {
                    cc_chan_lock(ch);
                    cc__chan_remove_send_waiter(ch, &node);
                    cc__chan_signal_recv_waiter(ch);
                    pthread_cond_signal(&ch->not_empty);
                    pthread_mutex_unlock(&ch->mu);
                    if (timing) t_wake = channel_rdtsc();
                    wake_batch_flush();
                    cc__chan_signal_activity(ch);
                    if (timing) {
                        uint64_t done = channel_rdtsc();
                        channel_timing_record_send(t0, t_lock, t_enqueue ? t_enqueue : done, t_wake ? t_wake : done, done);
                    }
                    return 0;
                }
                /* Dekker pre-park: wake any parked receiver before we sleep.
                 * has_send_waiters is already set (from add_send_waiter above),
                 * so a receiver arriving later will see our flag and wake us. */
                if (atomic_load_explicit(&ch->has_recv_waiters, memory_order_seq_cst)) {
                    cc_chan_lock(ch);
                    cc__chan_signal_recv_waiter(ch);
                    pthread_cond_signal(&ch->not_empty);
                    cc_chan_unlock(ch);
                    wake_batch_flush();
                }
                (void)cc__chan_wait_notified_mark_close(&node);
                pthread_mutex_lock(&ch->mu);
                int notified = atomic_load_explicit(&node.notified, memory_order_acquire);
                if (notified == CC_CHAN_NOTIFY_SIGNAL) {
                    if (ch->use_lockfree) {
                        cc__chan_dbg_inc(&g_chan_dbg.lf_send_notify_signal);
                    }
                    atomic_store_explicit(&node.notified, CC_CHAN_NOTIFY_NONE, memory_order_release);
                    cc__chan_remove_send_waiter(ch, &node);
                    /* After waking, try to enqueue before checking closed.
                     * Close only prevents NEW work - a sender already waiting
                     * should complete if there's space. */
                    /* Increment inflight BEFORE unlocking to prevent drain race. */
                    atomic_fetch_add_explicit(&ch->lfqueue_inflight, 1, memory_order_relaxed);
                    pthread_mutex_unlock(&ch->mu);
                    int rc = cc__chan_try_enqueue_lockfree_impl(ch, value);
                    atomic_fetch_sub_explicit(&ch->lfqueue_inflight, 1, memory_order_relaxed);
                    if (rc == 0) {
                        if (timing) t_enqueue = channel_rdtsc();
                        cc_chan_lock(ch);
                        cc__chan_signal_recv_waiter(ch);
                        pthread_cond_signal(&ch->not_empty);
                        pthread_mutex_unlock(&ch->mu);
                        if (timing) t_wake = channel_rdtsc();
                        wake_batch_flush();
                        cc__chan_signal_activity(ch);
                        if (timing) {
                            uint64_t done = channel_rdtsc();
                            channel_timing_record_send(t0, t_lock, t_enqueue, t_wake, done);
                        }
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
                        cc__chan_dbg_inc(&g_chan_dbg.lf_send_notify_cancel);
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
        atomic_fetch_add_explicit(&ch->lfqueue_inflight, 1, memory_order_relaxed);
        pthread_mutex_unlock(&ch->mu);
        int rc = cc__chan_try_enqueue_lockfree_impl(ch, value);
        atomic_fetch_sub_explicit(&ch->lfqueue_inflight, 1, memory_order_relaxed);
        if (rc == 0) {
            if (timing) t_enqueue = channel_rdtsc();
            cc_chan_lock(ch);
            cc__chan_signal_recv_waiter(ch);
            pthread_cond_signal(&ch->not_empty);
            pthread_mutex_unlock(&ch->mu);
            if (timing) t_wake = channel_rdtsc();
            wake_batch_flush();
            cc__chan_signal_activity(ch);
            if (timing) {
                uint64_t done = channel_rdtsc();
                channel_timing_record_send(t0, t_lock, t_enqueue, t_wake, done);
            }
            if (cc__chan_dbg_enabled()) {
                fprintf(stderr, "CC_CHAN_DEBUG: send_after_close_ok ch=%p\n", (void*)ch);
            }
            return 0;
        }
        if (cc__chan_dbg_enabled()) {
            int count = atomic_load_explicit(&ch->lfqueue_count, memory_order_acquire);
            fprintf(stderr, "CC_CHAN_DEBUG: send_epipe_closed ch=%p count=%d cap=%zu\n",
                    (void*)ch, count, ch->cap);
        }
        return EPIPE;
    }
    
    /* Original mutex-based path for non-lock-free channels */
    if (ch->count == ch->cap) {
        err = cc_chan_handle_full_send(ch, value, NULL);
        if (err != 0) { pthread_mutex_unlock(&ch->mu); return err; }
    }
    cc_chan_enqueue(ch, value);
    if (timing) t_enqueue = channel_rdtsc();
    pthread_mutex_unlock(&ch->mu);
    if (timing) t_wake = channel_rdtsc();
    wake_batch_flush();  /* Flush any pending fiber wakes */
    if (timing && err == 0) {
        uint64_t done = channel_rdtsc();
        channel_timing_record_send(t0, t_lock ? t_lock : t0, t_enqueue ? t_enqueue : done, t_wake ? t_wake : done, done);
    }
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
        if (ch->use_lockfree && ch->elem_size <= sizeof(void*)) {
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
        if (cc__chan_dequeue_lockfree_minimal(ch, out_value, NULL) == 0) {
            if (__builtin_expect(atomic_load_explicit(&ch->has_send_waiters, memory_order_seq_cst) != 0, 0)) {
                cc__chan_dbg_inc(&g_chan_dbg.lf_has_send_waiters_true);
                cc__chan_dbg_inc(&g_chan_dbg.lf_wake_lock_recv);
                cc_chan_lock(ch);
                cc__chan_wake_one_send_waiter(ch);
                pthread_cond_signal(&ch->not_full);
                pthread_mutex_unlock(&ch->mu);
                wake_batch_flush();
            } else {
                cc__chan_dbg_inc(&g_chan_dbg.lf_has_send_waiters_false);
            }
            if (__builtin_expect(cc__fiber_in_context() != 0, 1)) {
                if (++cc__tls_lf_ops >= CC_LF_YIELD_INTERVAL) {
                    cc__tls_lf_ops = 0;
                    cc__fiber_yield_global();
                }
            }
            return 0;
        }
        /* Buffer empty — fall through to full path for yield-retry / blocking */
    }
    if (!ch || !out_value || value_size == 0) return EINVAL;
    cc__chan_dbg_inc(&ch->dbg_lf_recv_calls);
    
    /* Owned channel (pool) special handling */
    if (ch->is_owned) {
        int rc = cc_chan_recv_owned(ch, out_value, value_size);
        if (rc != -1) return rc;  /* -1 means "use normal recv" */
    }
    
    /* Deadline scope: if caller installed a current deadline, use deadline-aware recv. */
    if (cc__tls_current_deadline) {
        return cc_chan_deadline_recv(ch, out_value, value_size, cc__tls_current_deadline);
    }
    int timing = channel_timing_enabled();
    uint64_t t0 = timing ? channel_rdtsc() : 0;
    uint64_t t_lock = 0;
    uint64_t t_dequeue = 0;
    uint64_t t_wake = 0;
    
    /* Lock-free fast path for buffered channels with small elements.
     * Large elements (> sizeof(void*)) use mutex path to avoid slot wrap-around race. */
    if (ch->use_lockfree && ch->cap > 0 && ch->elem_size == value_size && ch->buf &&
        ch->elem_size <= sizeof(void*)) {
        int rc = cc__chan_dequeue_lockfree_fast(ch, out_value, NULL);
        if (rc != 0 && !ch->closed && cc__fiber_in_context()) {
            /* Buffer empty — yield to let the sender fiber run, then retry
             * once before falling to the expensive blocking path.
             * Use global yield so fibers on other workers can also make progress. */
            cc__fiber_yield_global();
            rc = cc__chan_dequeue_lockfree_fast(ch, out_value, NULL);
        }
        if (rc == 0) {
            if (timing) {
                uint64_t done = channel_rdtsc();
                channel_timing_record_recv(t0, t0, done, done, done);
            }
            /* Signal send waiters — use atomic Dekker flag, not the
             * mutex-protected send_waiters_head. */
            if (atomic_load_explicit(&ch->has_send_waiters, memory_order_seq_cst)) {
                cc__chan_dbg_inc(&g_chan_dbg.lf_has_send_waiters_true);
                cc__chan_dbg_inc(&g_chan_dbg.lf_wake_lock_recv);
                cc_chan_lock(ch);
                cc__chan_wake_one_send_waiter(ch);
                pthread_cond_signal(&ch->not_full);
                pthread_mutex_unlock(&ch->mu);
                wake_batch_flush();
            } else {
                cc__chan_dbg_inc(&g_chan_dbg.lf_has_send_waiters_false);
            }
            cc__chan_signal_activity(ch);
            cc__chan_maybe_yield();
            return 0;
        }
        if (ch->closed) {
            if (cc__chan_dbg_enabled()) {
                int count = atomic_load_explicit(&ch->lfqueue_count, memory_order_acquire);
                int inflight = atomic_load_explicit(&ch->lfqueue_inflight, memory_order_acquire);
                fprintf(stderr, "CC_CHAN_DEBUG: recv_fast_closed ch=%p count=%d inflight=%d\n",
                        (void*)ch, count, inflight);
            }
            return cc__chan_try_drain_lockfree_on_close(ch, out_value, NULL);
        }
        /* Fall through to blocking path */
    }
    
    /* Standard mutex path */
    pthread_mutex_lock(&ch->mu);
    if (timing) t_lock = channel_rdtsc();
    int err = cc_chan_ensure_buf(ch, value_size);
    if (err != 0) { pthread_mutex_unlock(&ch->mu); return err; }
    
    /* Unbuffered rendezvous: direct handoff */
    if (ch->cap == 0) {
        err = cc_chan_recv_unbuffered(ch, out_value, NULL);
        if (cc__chan_dbg_enabled() && err != 0) {
            fprintf(stderr, "CC_CHAN_DEBUG: recv_unbuffered_err ch=%p err=%d closed=%d rx_err=%d send_w=%p recv_w=%p\n",
                    (void*)ch, err, ch->closed, ch->rx_error_closed,
                    (void*)ch->send_waiters_head, (void*)ch->recv_waiters_head);
        }
        pthread_mutex_unlock(&ch->mu);
        if (timing) t_wake = channel_rdtsc();
        wake_batch_flush();
        if (timing && err == 0) {
            uint64_t done = channel_rdtsc();
            channel_timing_record_recv(t0, t_lock ? t_lock : t0, t_dequeue ? t_dequeue : done, t_wake ? t_wake : done, done);
        }
        return err;
    }

    /* Buffered or initial setup - use existing wait logic.
     * Large elements (> sizeof(void*)) always use mutex path to avoid slot wrap-around race. */
    if (!ch->use_lockfree || ch->elem_size > sizeof(void*)) {
        err = cc_chan_wait_empty(ch, NULL);
        if (err != 0) { pthread_mutex_unlock(&ch->mu); return err; }
        cc_chan_dequeue(ch, out_value);
        if (timing) t_dequeue = channel_rdtsc();
        /* Wake a sender waiting for space (buffered channel). */
        cc__chan_wake_one_send_waiter(ch);
        pthread_cond_signal(&ch->not_full);
        pthread_mutex_unlock(&ch->mu);
        if (timing) t_wake = channel_rdtsc();
        wake_batch_flush();
        if (timing && err == 0) {
            uint64_t done = channel_rdtsc();
            channel_timing_record_recv(t0, t_lock ? t_lock : t0, t_dequeue ? t_dequeue : done, t_wake ? t_wake : done, done);
        }
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
            
            if (timing) t_dequeue = channel_rdtsc();
            cc_chan_lock(ch);
            cc__chan_wake_one_send_waiter(ch);
            pthread_cond_signal(&ch->not_full);
            pthread_mutex_unlock(&ch->mu);
            if (timing) t_wake = channel_rdtsc();
            wake_batch_flush();
            cc__chan_signal_activity(ch);
            if (timing) {
                uint64_t done = channel_rdtsc();
                channel_timing_record_recv(t0, t_lock, t_dequeue, t_wake, done);
            }
            return 0;
        }
        if (fiber) {
            int count = atomic_load_explicit(&ch->lfqueue_count, memory_order_acquire);
            if (count > 0 && !ch->closed) {
                cc__fiber_yield();
                pthread_mutex_lock(&ch->mu);
                continue;
            }
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
            cc__fiber_set_park_obj(ch);
            if (atomic_load_explicit(&ch->lfqueue_count, memory_order_acquire) > 0) {
                /* A sender may have already popped our node and done a direct
                 * handoff (notified=DATA) between add_recv_waiter and here.
                 * If so, the data is already in out_value - return it instead
                 * of continuing to the top of the loop where it would be lost. */
                int early = atomic_load_explicit(&node.notified, memory_order_acquire);
                if (early == CC_CHAN_NOTIFY_DATA) {
                    if (ch->use_lockfree) {
                        cc__chan_dbg_inc(&g_chan_dbg.lf_recv_notify_data);
                        cc__chan_dbg_inc(&g_chan_dbg.lf_direct_recv);
                        cc__chan_dbg_inc(&ch->dbg_lf_direct_recv);
                    }
                    if (timing) {
                        uint64_t done = channel_rdtsc();
                        channel_timing_record_recv(t0, t_lock, done, done, done);
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
                            cc__chan_dbg_inc(&g_chan_dbg.lf_recv_notify_data);
                            cc__chan_dbg_inc(&g_chan_dbg.lf_direct_recv);
                            cc__chan_dbg_inc(&ch->dbg_lf_direct_recv);
                        }
                        if (timing) {
                            uint64_t done = channel_rdtsc();
                            channel_timing_record_recv(t0, t_lock, done, done, done);
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
                        cc__chan_dbg_inc(&g_chan_dbg.lf_recv_notify_data);
                        cc__chan_dbg_inc(&g_chan_dbg.lf_direct_recv);
                        cc__chan_dbg_inc(&ch->dbg_lf_direct_recv);
                    }
                    if (timing) {
                        uint64_t done = channel_rdtsc();
                        channel_timing_record_recv(t0, t_lock, done, done, done);
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
                if (pre_deq_notified == CC_CHAN_NOTIFY_DATA) {
                    /* Direct handoff — data already in out_value */
                    if (ch->use_lockfree) {
                        cc__chan_dbg_inc(&g_chan_dbg.lf_recv_notify_data);
                        cc__chan_dbg_inc(&g_chan_dbg.lf_direct_recv);
                        cc__chan_dbg_inc(&ch->dbg_lf_direct_recv);
                    }
                    if (cc__chan_dbg_enabled()) {
                        fprintf(stderr, "CC_CHAN_DEBUG: direct_recv_pre_deq ch=%p node=%p\n", (void*)ch, (void*)&node);
                    }
                    pthread_mutex_unlock(&ch->mu);
                    if (timing) {
                        uint64_t done = channel_rdtsc();
                        channel_timing_record_recv(t0, t_lock, done, done, done);
                    }
                    return 0;
                }
                if (pre_deq_notified != 0 || ch->closed) {
                    /* SIGNAL/CLOSE — remove node and retry from loop top */
                    cc__chan_remove_recv_waiter(ch, &node);
                    /* mutex held — top of while(1) unlocks */
                    continue;
                }
                /* notified==0: safe to dequeue (no direct handoff risk).
                 * Check count under mutex to decide park vs dequeue. */
                int snap_count = atomic_load_explicit(&ch->lfqueue_count, memory_order_acquire);
                if (snap_count <= 0) {
                    /* Buffer empty — stay on wait list and park.
                     * Dekker pre-park: wake any parked sender before we sleep.
                     * has_recv_waiters is already set (from add_recv_waiter above),
                     * so a sender arriving later will see our flag and wake us. */
                    pthread_mutex_unlock(&ch->mu);
                    if (atomic_load_explicit(&ch->has_send_waiters, memory_order_seq_cst)) {
                        cc_chan_lock(ch);
                        cc__chan_wake_one_send_waiter(ch);
                        pthread_cond_signal(&ch->not_full);
                        cc_chan_unlock(ch);
                        wake_batch_flush();
                    }
                    (void)cc__chan_wait_notified_mark_close(&node);
                    pthread_mutex_lock(&ch->mu);
                    goto recv_post_park_notified;
                }
                /* count > 0 — remove node under lock, then try dequeue */
                cc__chan_remove_recv_waiter(ch, &node);
                pthread_mutex_unlock(&ch->mu);
            }
            if (cc__chan_dequeue_lockfree_fast(ch, out_value, NULL) == 0) {
                cc_chan_lock(ch);
                cc__chan_wake_one_send_waiter(ch);
                pthread_cond_signal(&ch->not_full);
                pthread_mutex_unlock(&ch->mu);
                if (timing) t_wake = channel_rdtsc();
                wake_batch_flush();
                cc__chan_signal_activity(ch);
                if (timing) {
                    uint64_t done = channel_rdtsc();
                    channel_timing_record_recv(t0, t_lock, t_dequeue ? t_dequeue : done, t_wake ? t_wake : done, done);
                }
                return 0;
            }
            /* CAS contention — node already removed but we'll re-add
             * immediately at the top of the next while(1) iteration. */
            pthread_mutex_lock(&ch->mu);
            continue;
        recv_post_park_notified: ;
            int notified = atomic_load_explicit(&node.notified, memory_order_acquire);
            if (cc__chan_dbg_enabled()) {
                fprintf(stderr, "CC_CHAN_DEBUG: recv_post_park ch=%p notified=%d in_list=%d closed=%d count=%d\n",
                        (void*)ch, notified, node.in_wait_list,
                        ch->closed,
                        atomic_load_explicit(&ch->lfqueue_count, memory_order_relaxed));
            }
            if (notified == CC_CHAN_NOTIFY_SIGNAL) {
                if (ch->use_lockfree) {
                    cc__chan_dbg_inc(&g_chan_dbg.lf_recv_notify_signal);
                }
                atomic_store_explicit(&node.notified, CC_CHAN_NOTIFY_NONE, memory_order_release);
                cc__chan_remove_recv_waiter(ch, &node);
                continue;
            }
            if (notified == CC_CHAN_NOTIFY_DATA) {
                if (ch->use_lockfree) {
                    cc__chan_dbg_inc(&g_chan_dbg.lf_recv_notify_data);
                    cc__chan_dbg_inc(&g_chan_dbg.lf_direct_recv);
                    cc__chan_dbg_inc(&ch->dbg_lf_direct_recv);
                }
                if (cc__chan_dbg_enabled()) {
                    fprintf(stderr, "CC_CHAN_DEBUG: direct_recv ch=%p node=%p\n", (void*)ch, (void*)&node);
                }
                /* Sender did direct handoff - data is already in out_value */
                pthread_mutex_unlock(&ch->mu);
                if (timing) {
                    uint64_t done = channel_rdtsc();
                    channel_timing_record_recv(t0, t_lock, done, done, done);
                }
                return 0;
            }
            if (notified == CC_CHAN_NOTIFY_CLOSE || ch->closed) {
                if (ch->use_lockfree) {
                    cc__chan_dbg_inc(&g_chan_dbg.lf_recv_notify_close);
                }
                /* Channel closed while we were waiting - drain in-flight sends before returning EPIPE. */
                cc__chan_remove_recv_waiter(ch, &node);
                pthread_mutex_unlock(&ch->mu);
                int rc = cc__chan_try_drain_lockfree_on_close(ch, out_value, NULL);
                if (rc == 0 && timing) {
                    uint64_t done = channel_rdtsc();
                    channel_timing_record_recv(t0, t_lock, t_dequeue ? t_dequeue : done, t_wake ? t_wake : done, done);
                }
                return rc;
            }
            /* notified == 0: spurious wakeup or early wake via pending_unpark.
             * But a sender might have popped us and delivered data between our
             * initial notified check and now. Re-check with acquire semantics. */
            int recheck = atomic_load_explicit(&node.notified, memory_order_acquire);
            if (recheck == CC_CHAN_NOTIFY_DATA) {
                /* Data was delivered after all! */
                if (ch->use_lockfree) {
                    cc__chan_dbg_inc(&g_chan_dbg.lf_recv_notify_data);
                    cc__chan_dbg_inc(&g_chan_dbg.lf_direct_recv);
                    cc__chan_dbg_inc(&ch->dbg_lf_direct_recv);
                }
                if (cc__chan_dbg_enabled()) {
                    fprintf(stderr, "CC_CHAN_DEBUG: direct_recv_recheck ch=%p node=%p\n", (void*)ch, (void*)&node);
                }
                pthread_mutex_unlock(&ch->mu);
                if (timing) {
                    uint64_t done = channel_rdtsc();
                    channel_timing_record_recv(t0, t_lock, done, done, done);
                }
                return 0;
            }
            {
                int pre_in_list = node.in_wait_list;
                int pre_notified = atomic_load_explicit(&node.notified, memory_order_acquire);
                cc__chan_remove_recv_waiter(ch, &node);
                if (ch->use_lockfree) {
                    cc__chan_dbg_inc(&g_chan_dbg.lf_recv_notify_cancel);
                    cc__chan_dbg_inc(&ch->dbg_lf_recv_remove_zero);
                }
                if (cc__chan_dbg_enabled()) {
                    fprintf(stderr, "CC_CHAN_DEBUG: recv_remove_zero ch=%p node=%p pre_in_list=%d pre_notified=%d post_in_list=%d\n",
                            (void*)ch, (void*)&node, pre_in_list, pre_notified, node.in_wait_list);
                }
            }
        } else {
            pthread_cond_wait(&ch->not_empty, &ch->mu);
        }
    }
    
    /* Channel closed - drain in-flight sends before returning EPIPE */
    pthread_mutex_unlock(&ch->mu);
    int rc = cc__chan_try_drain_lockfree_on_close(ch, out_value, NULL);
    if (rc == 0 && timing) {
        uint64_t done = channel_rdtsc();
        channel_timing_record_recv(t0, t_lock, done, done, done);
    }
    return rc;
}


int cc_chan_try_send(CCChan* ch, const void* value, size_t value_size) {
    if (!ch || !value || value_size == 0) return EINVAL;
    
    /* Lock-free fast path for buffered channels with small elements.
     * Large elements (> sizeof(void*)) use mutex path to avoid slot wrap-around race. */
    if (ch->use_lockfree && ch->cap > 0 && ch->elem_size == value_size && ch->buf &&
        ch->elem_size <= sizeof(void*)) {
        /* Manually manage inflight to cover the gap between checking closed and enqueueing */
        atomic_fetch_add_explicit(&ch->lfqueue_inflight, 1, memory_order_relaxed);
        if (ch->closed) {
            atomic_fetch_sub_explicit(&ch->lfqueue_inflight, 1, memory_order_relaxed);
            return EPIPE;
        }
        if (ch->rx_error_closed) {
            atomic_fetch_sub_explicit(&ch->lfqueue_inflight, 1, memory_order_relaxed);
            return ch->rx_error_code;
        }
        int rc = cc__chan_try_enqueue_lockfree_impl(ch, value);
        atomic_fetch_sub_explicit(&ch->lfqueue_inflight, 1, memory_order_relaxed);
        if (rc == 0) {
            /* Signal any waiters */
            cc_chan_lock(ch);
            cc__chan_signal_recv_waiter(ch);
            pthread_cond_signal(&ch->not_empty);
            pthread_mutex_unlock(&ch->mu);
            wake_batch_flush();
            cc__chan_signal_activity(ch);
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
        if (rnode->is_select) atomic_fetch_add_explicit(&g_dbg_select_data_set, 1, memory_order_relaxed);
        if (ch->rv_recv_waiters > 0) ch->rv_recv_waiters--;
        /* IMPORTANT: Increment signaled BEFORE waking the fiber. */
        if (rnode->is_select && rnode->select_group) {
            cc__select_wait_group* group = (cc__select_wait_group*)rnode->select_group;
            atomic_fetch_add_explicit(&group->signaled, 1, memory_order_release);
        }
        if (rnode->fiber) {
            wake_batch_add(rnode->fiber);
        } else {
            pthread_cond_signal(&ch->not_empty);
        }
        pthread_mutex_unlock(&ch->mu);
        wake_batch_flush();
        cc__chan_signal_activity(ch);
        return 0;
    }
    
    /* Buffered with lock-free small elements: try lock-free first */
    if (ch->use_lockfree && ch->elem_size <= sizeof(void*)) {
        /* Increment inflight BEFORE unlocking to prevent drain race. */
        atomic_fetch_add_explicit(&ch->lfqueue_inflight, 1, memory_order_relaxed);
        pthread_mutex_unlock(&ch->mu);
        int rc = cc__chan_try_enqueue_lockfree_impl(ch, value);
        atomic_fetch_sub_explicit(&ch->lfqueue_inflight, 1, memory_order_relaxed);
        if (rc == 0) {
            cc_chan_lock(ch);
            cc__chan_signal_recv_waiter(ch);
            pthread_cond_signal(&ch->not_empty);
            pthread_mutex_unlock(&ch->mu);
            wake_batch_flush();
            cc__chan_signal_activity(ch);
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
    return 0;
}

int cc_chan_try_recv(CCChan* ch, void* out_value, size_t value_size) {
    if (!ch || !out_value || value_size == 0) return EINVAL;
    
    /* Lock-free fast path for buffered channels with small elements.
     * Large elements (> sizeof(void*)) use mutex path to avoid slot wrap-around race. */
    if (ch->use_lockfree && ch->cap > 0 && ch->elem_size == value_size && ch->buf &&
        ch->elem_size <= sizeof(void*)) {
        int rc = cc_chan_try_dequeue_lockfree(ch, out_value);
        if (rc == 0) {
            /* Wake any waiters */
            cc_chan_lock(ch);
            cc__chan_wake_one_send_waiter(ch);
            pthread_cond_signal(&ch->not_full);
            pthread_mutex_unlock(&ch->mu);
            wake_batch_flush();
            cc__chan_signal_activity(ch);
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
        if (snode->is_select) atomic_fetch_add_explicit(&g_dbg_select_data_set, 1, memory_order_relaxed);
        /* IMPORTANT: Increment signaled BEFORE waking the fiber. */
        if (snode->is_select && snode->select_group) {
            cc__select_wait_group* group = (cc__select_wait_group*)snode->select_group;
            atomic_fetch_add_explicit(&group->signaled, 1, memory_order_release);
        }
        if (snode->fiber) {
            wake_batch_add(snode->fiber);
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
    return 0;
}

int cc_chan_timed_send(CCChan* ch, const void* value, size_t value_size, const struct timespec* abs_deadline) {
    if (!ch || !value || value_size == 0) return EINVAL;
    
    /* Lock-free fast path for buffered channels with small elements.
     * Large elements (> sizeof(void*)) use mutex path to avoid slot wrap-around race. */
    if (ch->use_lockfree && ch->cap > 0 && ch->elem_size == value_size && ch->buf &&
        ch->elem_size <= sizeof(void*)) {
        /* Manually manage inflight to cover the gap between checking closed and enqueueing */
        atomic_fetch_add_explicit(&ch->lfqueue_inflight, 1, memory_order_relaxed);
        if (ch->closed) {
            atomic_fetch_sub_explicit(&ch->lfqueue_inflight, 1, memory_order_relaxed);
            return EPIPE;
        }
        if (ch->rx_error_closed) {
            atomic_fetch_sub_explicit(&ch->lfqueue_inflight, 1, memory_order_relaxed);
            return ch->rx_error_code;
        }
        int rc = cc__chan_try_enqueue_lockfree_impl(ch, value);
        atomic_fetch_sub_explicit(&ch->lfqueue_inflight, 1, memory_order_relaxed);
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
        return err;
    }
    
    /* For lock-free channels, poll while waiting */
    if (ch->use_lockfree) {
        int in_fiber = cc__fiber_in_context();
        cc__fiber* fiber_ts = in_fiber ? cc__fiber_current() : NULL;
        while (!ch->closed) {
            /* Try lock-free enqueue */
            atomic_fetch_add_explicit(&ch->lfqueue_inflight, 1, memory_order_relaxed);
            pthread_mutex_unlock(&ch->mu);
            int rc = cc__chan_try_enqueue_lockfree_impl(ch, value);
            atomic_fetch_sub_explicit(&ch->lfqueue_inflight, 1, memory_order_relaxed);
            if (rc == 0) {
                cc_chan_lock(ch);
                cc__chan_signal_recv_waiter(ch);
                pthread_cond_signal(&ch->not_empty);
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
            if (fiber_ts) {
                /* Yield-retry if count suggests space */
                int count = atomic_load_explicit(&ch->lfqueue_count, memory_order_acquire);
                if (count < (int)ch->cap && !ch->closed) {
                    cc__fiber_yield();
                    cc_chan_lock(ch);
                    continue;
                }
                /* Register as send waiter, then use robust Dekker protocol */
                cc_chan_lock(ch);
                if (ch->closed) break;
                cc__fiber_wait_node node = {0};
                node.fiber = fiber_ts;
                atomic_store(&node.notified, 0);
                cc__chan_add_send_waiter(ch, &node);
                pthread_mutex_unlock(&ch->mu);
                cc__fiber_set_park_obj(ch);
                /* Re-check count — a recv may have freed space since our
                 * enqueue attempt above. */
                if (atomic_load_explicit(&ch->lfqueue_count, memory_order_acquire) < (int)ch->cap) {
                    cc_chan_lock(ch);
                    cc__chan_remove_send_waiter(ch, &node);
                    pthread_mutex_unlock(&ch->mu);
                    continue;
                }
                /* Retry enqueue one more time (we're registered, so any future
                 * recv will wake us). */
                atomic_fetch_add_explicit(&ch->lfqueue_inflight, 1, memory_order_relaxed);
                rc = cc__chan_try_enqueue_lockfree_impl(ch, value);
                atomic_fetch_sub_explicit(&ch->lfqueue_inflight, 1, memory_order_relaxed);
                if (rc == 0) {
                    cc_chan_lock(ch);
                    cc__chan_remove_send_waiter(ch, &node);
                    cc__chan_signal_recv_waiter(ch);
                    pthread_cond_signal(&ch->not_empty);
                    pthread_mutex_unlock(&ch->mu);
                    wake_batch_flush();
                    cc__chan_signal_activity(ch);
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
                (void)cc__chan_wait_notified_mark_close(&node);
                pthread_mutex_lock(&ch->mu);
                int notified = atomic_load_explicit(&node.notified, memory_order_acquire);
                if (notified == CC_CHAN_NOTIFY_SIGNAL) {
                    atomic_store_explicit(&node.notified, CC_CHAN_NOTIFY_NONE, memory_order_release);
                    cc__chan_remove_send_waiter(ch, &node);
                    /* Retry enqueue after wake */
                    atomic_fetch_add_explicit(&ch->lfqueue_inflight, 1, memory_order_relaxed);
                    pthread_mutex_unlock(&ch->mu);
                    rc = cc__chan_try_enqueue_lockfree_impl(ch, value);
                    atomic_fetch_sub_explicit(&ch->lfqueue_inflight, 1, memory_order_relaxed);
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
    return 0;
}

int cc_chan_timed_recv(CCChan* ch, void* out_value, size_t value_size, const struct timespec* abs_deadline) {
    if (!ch || !out_value || value_size == 0) return EINVAL;
    
    /* Lock-free fast path for buffered channels with small elements.
     * Large elements (> sizeof(void*)) use mutex path to avoid slot wrap-around race. */
    if (ch->use_lockfree && ch->cap > 0 && ch->elem_size == value_size && ch->buf &&
        ch->elem_size <= sizeof(void*)) {
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
                /* Yield-retry if count suggests data */
                int count_r = atomic_load_explicit(&ch->lfqueue_count, memory_order_acquire);
                if (count_r > 0 && !ch->closed) {
                    cc__fiber_yield();
                    cc_chan_lock(ch);
                    continue;
                }
                /* Register as recv waiter, then use robust Dekker protocol */
                cc_chan_lock(ch);
                if (ch->closed) break;
                cc__fiber_wait_node node = {0};
                node.fiber = fiber_tr;
                atomic_store(&node.notified, 0);
                cc__chan_add_recv_waiter(ch, &node);
                pthread_mutex_unlock(&ch->mu);
                cc__fiber_set_park_obj(ch);
                /* Re-check count */
                if (atomic_load_explicit(&ch->lfqueue_count, memory_order_acquire) > 0) {
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
                if (atomic_load_explicit(&ch->has_send_waiters, memory_order_seq_cst)) {
                    cc_chan_lock(ch);
                    cc__chan_wake_one_send_waiter(ch);
                    pthread_cond_signal(&ch->not_full);
                    cc_chan_unlock(ch);
                    wake_batch_flush();
                }
                (void)cc__chan_wait_notified_mark_close(&node);
                pthread_mutex_lock(&ch->mu);
                int notified = atomic_load_explicit(&node.notified, memory_order_acquire);
                if (notified == CC_CHAN_NOTIFY_SIGNAL) {
                    atomic_store_explicit(&node.notified, CC_CHAN_NOTIFY_NONE, memory_order_release);
                    cc__chan_remove_recv_waiter(ch, &node);
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
            err = pthread_cond_timedwait(&ch->not_empty, &ch->mu, wait_deadline ? wait_deadline : &poll_deadline);
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
        if (rc == 0) { atomic_fetch_add_explicit(&g_dbg_select_try_returned, 1, memory_order_relaxed); return rc; }
        if (rc == EPIPE) { atomic_fetch_add_explicit(&g_dbg_select_close_returned, 1, memory_order_relaxed); return rc; }
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
                    if (notified == CC_CHAN_NOTIFY_DATA) atomic_fetch_add_explicit(&g_dbg_select_data_returned, 1, memory_order_relaxed);
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
                    if (notified == CC_CHAN_NOTIFY_DATA) atomic_fetch_add_explicit(&g_dbg_select_data_returned, 1, memory_order_relaxed);
                    return (notified == CC_CHAN_NOTIFY_DATA) ? 0 : EPIPE;
                }
            }
            int need_rearm = 0;
            if (cc__chan_dbg_enabled()) {
                fprintf(stderr, "CC_CHAN_DEBUG: select_enter_park_loop group=%p selected=%d signaled=%d\n",
                        (void*)&group,
                        atomic_load_explicit(&group.selected_index, memory_order_acquire),
                        atomic_load_explicit(&group.signaled, memory_order_acquire));
            }
            while (atomic_load_explicit(&group.selected_index, memory_order_acquire) == -1) {
                cc__chan_dbg_select_group("park", &group);
                int seq = atomic_load_explicit(&group.signaled, memory_order_acquire);
                if (atomic_load_explicit(&group.selected_index, memory_order_acquire) != -1) {
                    break;
                }
                int pre_signaled = atomic_load_explicit(&group.signaled, memory_order_acquire);
                int pre_selected = atomic_load_explicit(&group.selected_index, memory_order_acquire);
                /* Clear pending_unpark right before parking to avoid consuming a wakeup
                 * that was meant for a previous operation or a different channel. */
                cc__fiber_clear_pending_unpark();
                if (cc__chan_dbg_enabled()) {
                    fprintf(stderr, "CC_CHAN_DEBUG: select_pre_park group=%p seq=%d pre_sig=%d pre_sel=%d fiber=%p\n",
                            (void*)&group, seq, pre_signaled, pre_selected, (void*)fiber);
                }
                CC_FIBER_PARK_IF(&group.signaled, seq, "chan_match: waiting");
                int wake_signaled = atomic_load_explicit(&group.signaled, memory_order_acquire);
                int wake_selected = atomic_load_explicit(&group.selected_index, memory_order_acquire);
                if (cc__chan_dbg_enabled()) {
                    fprintf(stderr, "CC_CHAN_DEBUG: select_post_park group=%p wake_sig=%d wake_sel=%d fiber=%p\n",
                            (void*)&group, wake_signaled, wake_selected, (void*)fiber);
                }
                cc__chan_dbg_select_group("wake", &group);
                if (cc__chan_dbg_enabled() && wake_signaled == seq && wake_selected == -1) {
                    /* Check if we actually parked or skipped due to pending_unpark */
                    fprintf(stderr, "CC_CHAN_DEBUG: select_spurious_wake group=%p seq=%d pre=%d signaled=%d selected=%d fiber=%p\n",
                            (void*)&group, seq, pre_signaled, wake_signaled, wake_selected, (void*)fiber);
                }
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
                    if (cc__chan_dbg_enabled() && wake_signaled == seq && wake_selected == -1) {
                        fprintf(stderr, "CC_CHAN_DEBUG: select_check_notified i=%zu notified=%d\n", i, notified);
                    }
                    if (notified == CC_CHAN_NOTIFY_SIGNAL) {
                        atomic_store_explicit(&nodes[i].notified, CC_CHAN_NOTIFY_NONE, memory_order_release);
                        notified = CC_CHAN_NOTIFY_NONE;
                    }
                    if (notified == CC_CHAN_NOTIFY_CANCEL) {
                        atomic_store_explicit(&nodes[i].notified, CC_CHAN_NOTIFY_NONE, memory_order_release);
                        need_rearm = 1;
                        continue;
                    }
                    if (notified == CC_CHAN_NOTIFY_DATA || notified == CC_CHAN_NOTIFY_CLOSE) {
                        saw_notify = 1;
                        break;
                    }
                }
                if (saw_notify) {
                    if (cc__chan_dbg_enabled()) {
                        fprintf(stderr, "CC_CHAN_DEBUG: select_break_saw_notify group=%p fiber=%p\n",
                                (void*)&group, (void*)fiber);
                    }
                    break;
                }
                if (need_rearm) {
                    if (cc__chan_dbg_enabled()) {
                        fprintf(stderr, "CC_CHAN_DEBUG: select_break_need_rearm group=%p fiber=%p\n",
                                (void*)&group, (void*)fiber);
                    }
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
            int found_data = 0;
            for (size_t i = 0; i < n; ++i) {
                int notified = atomic_load_explicit(&nodes[i].notified, memory_order_acquire);
                if (cc__chan_dbg_enabled()) {
                    fprintf(stderr, "CC_CHAN_DEBUG: select_post_cleanup_check i=%zu notified=%d need_rearm=%d\n",
                            i, notified, need_rearm);
                }
                if (notified == CC_CHAN_NOTIFY_DATA) {
                    *ready_index = i;
                    found_data = 1;
                    atomic_fetch_add_explicit(&g_dbg_select_data_returned, 1, memory_order_relaxed);
                    return 0;
                }
                if (notified == CC_CHAN_NOTIFY_CLOSE) {
                    *ready_index = i;
                    return EPIPE;
                }
            }
            if (need_rearm) {
                if (cc__chan_dbg_enabled()) {
                    fprintf(stderr, "CC_CHAN_DEBUG: select_rearm found_data=%d\n", found_data);
                }
                continue;
            }
            int sel = atomic_load_explicit(&group.selected_index, memory_order_acquire);
            if (sel >= 0 && sel < (int)n) {
                for (;;) {
                    int notified = atomic_load_explicit(&nodes[sel].notified, memory_order_acquire);
                    if (notified == CC_CHAN_NOTIFY_SIGNAL) {
                        atomic_store_explicit(&nodes[sel].notified, CC_CHAN_NOTIFY_NONE, memory_order_release);
                        notified = CC_CHAN_NOTIFY_NONE;
                    }
                    if (notified == CC_CHAN_NOTIFY_DATA) {
                        *ready_index = (size_t)sel;
                        atomic_fetch_add_explicit(&g_dbg_select_data_returned, 1, memory_order_relaxed);
                        return 0;
                    }
                    if (notified == CC_CHAN_NOTIFY_CLOSE) {
                        *ready_index = (size_t)sel;
                        return EPIPE;
                    }
                    cc__chan_dbg_select_wait("winner_wait", &group, (size_t)sel, notified);
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
                    atomic_fetch_add_explicit(&g_dbg_select_data_returned, 1, memory_order_relaxed);
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
