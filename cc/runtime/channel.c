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
#include <pthread.h>
#include <sched.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdio.h>
#include <stdatomic.h>
#include <stdint.h>

/* spinlock_condvar.h is available but pthread_cond works better for channel
 * waiter synchronization due to the mutex integration pattern. */

/* ============================================================================
 * Fiber-Aware Blocking Infrastructure
 * ============================================================================ */

#include "fiber_internal.h"
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
            cc__fiber_unpark(b->fibers[i]);
            b->fibers[i] = NULL;
        }
    }
    b->count = 0;
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
                                      CCChanTx* out_tx,
                                      CCChanRx* out_rx) {
    if (!out_tx || !out_rx) return NULL;
    out_tx->raw = NULL;
    out_rx->raw = NULL;
    CCChanTopology topo = (CCChanTopology)topology;
    CCChan* ch = cc_chan_create_internal(capacity, mode, allow_send_take, is_sync, topo);
    if (!ch) return NULL;
    if (elem_size != 0) {
        int e = cc_chan_init_elem(ch, elem_size);
        if (e != 0) { cc_chan_free(ch); return NULL; }
    }
    out_tx->raw = ch;
    out_rx->raw = ch;
    return ch;
}

/* Global broadcast condvar for multi-channel select (@match).
   Simple approach: any channel activity signals this global condvar.
   Waiters in @match wait on this. Spurious wakeups are handled by retrying. */
static pthread_mutex_t g_chan_broadcast_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_chan_broadcast_cv = PTHREAD_COND_INITIALIZER;
static _Atomic int g_select_waiters = 0;  /* Count of threads waiting in select */

struct CCChan {
    size_t cap;
    size_t count;              /* Only used for unbuffered (cap==0) and mutex fallback */
    size_t head;               /* Only used for unbuffered (cap==0) and mutex fallback */
    size_t tail;               /* Only used for unbuffered (cap==0) and mutex fallback */
    void *buf;                 /* Data buffer: ring buffer for mutex path, slot array for lock-free */
    size_t elem_size;
    int closed;
    CCChanMode mode;
    int allow_take;
    int is_sync;               /* 1 = sync (blocks OS thread), 0 = async (cooperative) */
    CCChanTopology topology;
    /* Rendezvous (unbuffered) support: cap==0 */
    int rv_has_value;
    int rv_recv_waiters;
    
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
    
    /* Lock-free MPMC queue for buffered channels (cap > 0) */
    int use_lockfree;                               /* 1 = use lock-free queue, 0 = use mutex */
    size_t lfqueue_cap;                             /* Actual capacity (rounded up to power of 2) */
    struct lfds711_queue_bmm_state lfqueue_state;   /* liblfds queue state */
    struct lfds711_queue_bmm_element *lfqueue_elements; /* Pre-allocated element array */
    _Atomic int lfqueue_count;                      /* Approximate count for fast full/empty check */
    _Atomic size_t slot_counter;                    /* Per-channel slot counter for large elements */
    _Atomic int recv_fairness_ctr;                  /* For yield-every-N fairness */
};

static inline void cc_chan_lock(CCChan* ch) {
    pthread_mutex_lock(&ch->mu);
}

/* ============================================================================
 * Fiber Wait Queue Helpers
 * ============================================================================ */

/* Add a fiber to a waiter queue (must hold ch->mu) */
static void cc__chan_add_waiter(cc__fiber_wait_node** head, cc__fiber_wait_node** tail, cc__fiber_wait_node* node) {
    if (!node) return;
    node->next = NULL;
    node->prev = *tail;
    if (*tail) {
        (*tail)->next = node;
    } else {
        *head = node;
    }
    *tail = node;
}

/* Add a fiber to send waiters queue (must hold ch->mu) */
static void cc__chan_add_send_waiter(CCChan* ch, cc__fiber_wait_node* node) {
    if (!ch || !node) return;
    cc__chan_add_waiter(&ch->send_waiters_head, &ch->send_waiters_tail, node);
}

/* Add a fiber to recv waiters queue (must hold ch->mu) */
static void cc__chan_add_recv_waiter(CCChan* ch, cc__fiber_wait_node* node) {
    if (!ch || !node) return;
    cc__chan_add_waiter(&ch->recv_waiters_head, &ch->recv_waiters_tail, node);
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
}

static void cc__chan_remove_send_waiter(CCChan* ch, cc__fiber_wait_node* node) {
    if (!ch || !node) return;
    cc__chan_remove_waiter_list(&ch->send_waiters_head, &ch->send_waiters_tail, node);
}

static void cc__chan_remove_recv_waiter(CCChan* ch, cc__fiber_wait_node* node) {
    if (!ch || !node) return;
    cc__chan_remove_waiter_list(&ch->recv_waiters_head, &ch->recv_waiters_tail, node);
}

/* Wake one send waiter (must hold ch->mu) - uses batch */
static void cc__chan_wake_one_send_waiter(CCChan* ch) {
    if (!ch || !ch->send_waiters_head) return;
    cc__fiber_wait_node* node = ch->send_waiters_head;
    ch->send_waiters_head = node->next;
    if (ch->send_waiters_head) {
        ch->send_waiters_head->prev = NULL;
    } else {
        ch->send_waiters_tail = NULL;
    }
    atomic_store_explicit(&node->notified, 1, memory_order_release);
    wake_batch_add(node->fiber);
}

/* Signal a recv waiter to wake and try the buffer (must hold ch->mu).
 * Does NOT set notified - the waiter remains in the queue and should check
 * the buffer. Uses simple FIFO - work stealing provides natural load balancing. */
static void cc__chan_signal_recv_waiter(CCChan* ch) {
    if (!ch || !ch->recv_waiters_head) return;
    /* Just wake the head fiber - it will try the buffer */
    wake_batch_add(ch->recv_waiters_head->fiber);
}

/* Pop a send waiter (must hold ch->mu). */
static cc__fiber_wait_node* cc__chan_pop_send_waiter(CCChan* ch) {
    if (!ch) return NULL;
    while (ch->send_waiters_head) {
        cc__fiber_wait_node* node = ch->send_waiters_head;
        if (atomic_load_explicit(&node->notified, memory_order_acquire) == 2) {
            ch->send_waiters_head = node->next;
            if (ch->send_waiters_head) {
                ch->send_waiters_head->prev = NULL;
            } else {
                ch->send_waiters_tail = NULL;
            }
            node->next = node->prev = NULL;
            continue;
        }
        ch->send_waiters_head = node->next;
        if (ch->send_waiters_head) {
            ch->send_waiters_head->prev = NULL;
        } else {
            ch->send_waiters_tail = NULL;
        }
        node->next = node->prev = NULL;
        return node;
    }
    return NULL;
}

/* Pop a recv waiter (must hold ch->mu). */
static cc__fiber_wait_node* cc__chan_pop_recv_waiter(CCChan* ch) {
    if (!ch) return NULL;
    while (ch->recv_waiters_head) {
        cc__fiber_wait_node* node = ch->recv_waiters_head;
        if (atomic_load_explicit(&node->notified, memory_order_acquire) == 2) {
            ch->recv_waiters_head = node->next;
            if (ch->recv_waiters_head) {
                ch->recv_waiters_head->prev = NULL;
            } else {
                ch->recv_waiters_tail = NULL;
            }
            node->next = node->prev = NULL;
            continue;
        }
        ch->recv_waiters_head = node->next;
        if (ch->recv_waiters_head) {
            ch->recv_waiters_head->prev = NULL;
        } else {
            ch->recv_waiters_tail = NULL;
        }
        node->next = node->prev = NULL;
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
    atomic_store_explicit(&node->notified, 3, memory_order_release);  /* 3 = close */
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
    atomic_store_explicit(&node->notified, 3, memory_order_release);  /* 3 = close */
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
    ch->lfqueue_cap = 0;
    ch->lfqueue_elements = NULL;
    atomic_store(&ch->lfqueue_count, 0);
    atomic_store(&ch->slot_counter, 0);
    atomic_store(&ch->recv_fairness_ctr, 0);
    
    if (cap > 1) {  /* Only use lock-free for cap > 1 (liblfds needs at least 2) */
        /* Buffered channel: allocate lock-free queue */
        size_t lfcap = next_power_of_2(cap);
        ch->lfqueue_cap = lfcap;
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

void cc_chan_close(CCChan* ch) {
    if (!ch) return;
    pthread_mutex_lock(&ch->mu);
    ch->closed = 1;
    pthread_cond_broadcast(&ch->not_empty);
    pthread_cond_broadcast(&ch->not_full);
    /* Wake all waiting fibers */
    cc__chan_wake_all_waiters(ch);
    pthread_mutex_unlock(&ch->mu);
    wake_batch_flush();  /* Flush fiber wakes immediately */
    cc__chan_broadcast_activity();
}

void cc_chan_free(CCChan* ch) {
    if (!ch) return;
    
    /* Clean up lock-free queue if used */
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
        if (fiber && !deadline) {
            /* Fiber-aware blocking: park the fiber instead of condvar wait */
            while (!ch->closed && (ch->rv_has_value || (ch->rv_recv_waiters == 0 && !ch->recv_waiters_head))) {
                cc__fiber_wait_node node = {0};
                node.fiber = fiber;
                atomic_store(&node.notified, 0);
                cc__chan_add_send_waiter(ch, &node);

                pthread_mutex_unlock(&ch->mu);
                if (!atomic_load_explicit(&node.notified, memory_order_acquire)) {
                    CC_FIBER_PARK("chan_send: waiting for receiver (rendezvous)");
                }
                pthread_mutex_lock(&ch->mu);

                if (!atomic_load_explicit(&node.notified, memory_order_acquire)) {
                    cc__chan_remove_send_waiter(ch, &node);
                }
            }
        } else {
            /* Traditional condvar blocking */
            while (!ch->closed && (ch->rv_has_value || ch->rv_recv_waiters == 0) && err == 0) {
                if (deadline) {
                    err = pthread_cond_timedwait(&ch->not_full, &ch->mu, deadline);
                    if (err == ETIMEDOUT) { return ETIMEDOUT; }
                } else {
                    pthread_cond_wait(&ch->not_full, &ch->mu);
                }
            }
        }
        
        return ch->closed ? EPIPE : 0;
    }

    /* Buffered channel */
    if (fiber && !deadline) {
        /* Fiber-aware blocking */
        while (!ch->closed && ch->count == ch->cap) {
            cc__fiber_wait_node node = {0};
            node.fiber = fiber;
            atomic_store(&node.notified, 0);
            cc__chan_add_send_waiter(ch, &node);

            pthread_mutex_unlock(&ch->mu);
            CC_FIBER_PARK("chan_send: waiting for space");
            pthread_mutex_lock(&ch->mu);

            if (!atomic_load_explicit(&node.notified, memory_order_acquire)) {
                cc__chan_remove_send_waiter(ch, &node);
            }
        }
    } else {
        /* Traditional condvar blocking */
        while (!ch->closed && ch->count == ch->cap && err == 0) {
            if (deadline) {
                err = pthread_cond_timedwait(&ch->not_full, &ch->mu, deadline);
                if (err == ETIMEDOUT) { return ETIMEDOUT; }
            } else {
                pthread_cond_wait(&ch->not_full, &ch->mu);
            }
        }
    }
    
    return ch->closed ? EPIPE : 0;
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

        

        if (fiber && !deadline) {
            /* Fiber-aware blocking */
            while (!ch->closed && !ch->rv_has_value) {
                cc__fiber_wait_node node = {0};
                node.fiber = fiber;
                atomic_store(&node.notified, 0);
                cc__chan_add_recv_waiter(ch, &node);

                pthread_mutex_unlock(&ch->mu);

                /* If we were already notified before parking, skip the park. */
                if (!atomic_load_explicit(&node.notified, memory_order_acquire)) {
                    CC_FIBER_PARK("chan_recv: waiting for sender (rendezvous)");
                }

                pthread_mutex_lock(&ch->mu);

                if (!atomic_load_explicit(&node.notified, memory_order_acquire)) {
                    cc__chan_remove_recv_waiter(ch, &node);
                }
            }
        } else {
            /* Spinlock-condvar blocking (spin then sleep) */
            while (!ch->closed && !ch->rv_has_value && err == 0) {
                if (deadline) {
                    err = pthread_cond_timedwait(&ch->not_empty, &ch->mu, deadline);
                    if (err == ETIMEDOUT) { ch->rv_recv_waiters--; return ETIMEDOUT; }
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
            return EPIPE;
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

    

    if (fiber && !deadline) {
        /* Fiber-aware blocking */
        while (!ch->closed && ch->count == 0) {
            cc__fiber_wait_node node = {0};
            node.fiber = fiber;
            atomic_store(&node.notified, 0);
            cc__chan_add_recv_waiter(ch, &node);

            pthread_mutex_unlock(&ch->mu);
            CC_FIBER_PARK("chan_send: waiting for space");
            pthread_mutex_lock(&ch->mu);

            if (!atomic_load_explicit(&node.notified, memory_order_acquire)) {
                cc__chan_remove_recv_waiter(ch, &node);
            }
        }
    } else {
        /* Spinlock-condvar blocking (spin then sleep) */
        while (!ch->closed && ch->count == 0 && err == 0) {
            if (deadline) {
                err = pthread_cond_timedwait(&ch->not_empty, &ch->mu, deadline);
                if (err == ETIMEDOUT) { return ETIMEDOUT; }
            } else {
                pthread_cond_wait(&ch->not_empty, &ch->mu);
            }
        }
    }
    
    if (ch->closed && ch->count == 0) return EPIPE;
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
        cc__chan_broadcast_activity();
        return;
    }
    /* Buffered: signal waiters */
    void *slot = (uint8_t*)ch->buf + ch->tail * ch->elem_size;
    channel_store_slot(slot, value, ch->elem_size);
    ch->tail = (ch->tail + 1) % ch->cap;
    ch->count++;
    pthread_cond_signal(&ch->not_empty);
    cc__chan_signal_recv_waiter(ch);
    cc__chan_broadcast_activity();
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
        cc__chan_broadcast_activity();
        return;
    }
    /* Buffered: signal waiters */
    void *slot = (uint8_t*)ch->buf + ch->head * ch->elem_size;
    channel_load_slot(slot, out_value, ch->elem_size);
    ch->head = (ch->head + 1) % ch->cap;
    ch->count--;
    pthread_cond_signal(&ch->not_full);
    cc__chan_wake_one_send_waiter(ch);
    cc__chan_broadcast_activity();
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

/* Try lock-free enqueue. Returns 0 on success, EAGAIN if full.
 * Must NOT hold ch->mu when calling this.
 * ONLY valid for small elements (elem_size <= sizeof(void*)). */
static int cc_chan_try_enqueue_lockfree(CCChan* ch, const void* value) {
    if (!ch->use_lockfree || ch->cap == 0 || !ch->buf) return EAGAIN;
    if (ch->elem_size > sizeof(void*)) {
        fprintf(stderr, "BUG: cc_chan_try_enqueue_lockfree called with large element (size=%zu)\n", ch->elem_size);
        return EAGAIN;
    }
    
    /* Small element: store directly in pointer (zero-copy for ints, pointers, etc.) */
    void *queue_val = NULL;
    memcpy(&queue_val, value, ch->elem_size);
    
    /* Try to enqueue - liblfds returns 1 on success, 0 if full */
    int ok = lfds711_queue_bmm_enqueue(&ch->lfqueue_state, NULL, queue_val);
    return ok ? 0 : EAGAIN;
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
    
    void *key, *val;
    
    /* Try to dequeue - liblfds returns 1 on success, 0 if empty */
    int ok = lfds711_queue_bmm_dequeue(&ch->lfqueue_state, &key, &val);
    if (!ok) return EAGAIN;
    
    /* Small element: stored directly in pointer */
    memcpy(out_value, &val, ch->elem_size);
    
    return 0;
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

    while (!ch->closed) {
        /* If a receiver is waiting, handoff directly */
        cc__fiber_wait_node* rnode = cc__chan_pop_recv_waiter(ch);
        if (rnode) {
            channel_store_slot(rnode->data, value, ch->elem_size);
            atomic_store_explicit(&rnode->notified, 1, memory_order_release);
            if (rnode->fiber) {
                wake_batch_add(rnode->fiber);
            } else {
                pthread_cond_signal(&ch->not_empty);
            }
            if (ch->rv_recv_waiters > 0) ch->rv_recv_waiters--;
            return 0;
        }

        /* No receiver; wait */
        cc__fiber_wait_node node = {0};
        node.fiber = (fiber && !deadline) ? fiber : NULL;
        node.data = (void*)value;
        atomic_store(&node.notified, 0);
        cc__chan_add_send_waiter(ch, &node);

        while (!ch->closed && !atomic_load_explicit(&node.notified, memory_order_acquire) && err == 0) {
            if (fiber && !deadline) {
                pthread_mutex_unlock(&ch->mu);
                if (!atomic_load_explicit(&node.notified, memory_order_acquire)) {
                    CC_FIBER_PARK("chan_send: waiting for receiver");
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
            if (notify_val == 1) {
                /* notified=1 means a receiver actually took our data */
                return 0;
            }
            if (notify_val == 3) {
                /* notified=3 means woken by close */
                return EPIPE;
            }
        }

        if (deadline && err == ETIMEDOUT) {
            atomic_store_explicit(&node.notified, 2, memory_order_release);
            cc__chan_remove_send_waiter(ch, &node);  /* Remove from queue before returning */
            return ETIMEDOUT;
        }
        if (ch->closed) {
            atomic_store_explicit(&node.notified, 2, memory_order_release);
            cc__chan_remove_send_waiter(ch, &node);  /* Remove from queue before returning */
            return EPIPE;
        }
    }
    return EPIPE;
}

static int cc_chan_recv_unbuffered(CCChan* ch, void* out_value, const struct timespec* deadline) {
    cc__fiber* fiber = cc__fiber_in_context() ? cc__fiber_current() : NULL;
    int err = 0;

    while (!ch->closed) {
        /* If a sender is waiting, handoff directly */
        cc__fiber_wait_node* snode = cc__chan_pop_send_waiter(ch);
        if (snode) {
            channel_load_slot(snode->data, out_value, ch->elem_size);
            atomic_store_explicit(&snode->notified, 1, memory_order_release);
            if (snode->fiber) {
                wake_batch_add(snode->fiber);
            } else {
                pthread_cond_signal(&ch->not_full);
            }
            return 0;
        }

        /* No sender; wait */
        ch->rv_recv_waiters++;
        cc__fiber_wait_node node = {0};
        node.fiber = (fiber && !deadline) ? fiber : NULL;
        node.data = out_value;
        atomic_store(&node.notified, 0);
        cc__chan_add_recv_waiter(ch, &node);

        while (!ch->closed && !atomic_load_explicit(&node.notified, memory_order_acquire) && err == 0) {
            if (fiber && !deadline) {
                pthread_mutex_unlock(&ch->mu);
                if (!atomic_load_explicit(&node.notified, memory_order_acquire)) {
                    CC_FIBER_PARK("chan_recv: waiting for sender");
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
            if (notify_val == 1) {
                /* notified=1 means a sender actually delivered data */
                if (ch->rv_recv_waiters > 0) ch->rv_recv_waiters--;
                return 0;
            }
            if (notify_val == 3) {
                /* notified=3 means woken by close with no data */
                if (ch->rv_recv_waiters > 0) ch->rv_recv_waiters--;
                return EPIPE;
            }
        }

        if (deadline && err == ETIMEDOUT) {
            atomic_store_explicit(&node.notified, 2, memory_order_release);
            cc__chan_remove_recv_waiter(ch, &node);  /* Remove from queue before returning */
            if (ch->rv_recv_waiters > 0) ch->rv_recv_waiters--;
            return ETIMEDOUT;
        }
        if (ch->closed) {
            atomic_store_explicit(&node.notified, 2, memory_order_release);
            cc__chan_remove_recv_waiter(ch, &node);  /* Remove from queue before returning */
            if (ch->rv_recv_waiters > 0) ch->rv_recv_waiters--;
            return EPIPE;
        }
    }
    return EPIPE;
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
    if (!ch || !value || value_size == 0) return EINVAL;
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
        
        /* Direct handoff: if receivers waiting, give item directly to one.
         * This must be done under lock to coordinate with the fair queue. */
        if (ch->recv_waiters_head != NULL) {
            cc_chan_lock(ch);
            if (ch->closed) { pthread_mutex_unlock(&ch->mu); return EPIPE; }
            cc__fiber_wait_node* rnode = cc__chan_pop_recv_waiter(ch);
            if (rnode) {
                /* Direct handoff to waiting receiver */
                channel_store_slot(rnode->data, value, ch->elem_size);
                atomic_store_explicit(&rnode->notified, 1, memory_order_release);
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
                cc__chan_broadcast_activity();
                return 0;
            }
            pthread_mutex_unlock(&ch->mu);
        }
        
        /* No waiters - try lock-free enqueue to buffer */
        int rc = cc_chan_try_enqueue_lockfree(ch, value);
        if (rc == 0) {
            if (timing) {
                uint64_t done = channel_rdtsc();
                channel_timing_record_send(t0, t0, done, done, done);
            }
            /* Signal any waiters that might have joined the queue */
            if (ch->recv_waiters_head != NULL) {
                cc_chan_lock(ch);
                cc__chan_signal_recv_waiter(ch);
                pthread_mutex_unlock(&ch->mu);
                wake_batch_flush();
            }
            cc__chan_broadcast_activity();
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
    
    /* Standard mutex path (unbuffered, initial setup, or lock-free full) */
    cc_chan_lock(ch);
    if (timing) t_lock = channel_rdtsc();
    int err = cc_chan_ensure_buf(ch, value_size);
    if (err != 0) { pthread_mutex_unlock(&ch->mu); return err; }
    if (ch->closed) { pthread_mutex_unlock(&ch->mu); return EPIPE; }
    
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
            cc__chan_broadcast_activity();
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
            pthread_mutex_unlock(&ch->mu);
            int rc = cc_chan_try_enqueue_lockfree(ch, value);
            if (rc == 0) {
                
                if (timing) t_enqueue = channel_rdtsc();
                cc_chan_lock(ch);
                cc__chan_signal_recv_waiter(ch);
                pthread_cond_signal(&ch->not_empty);
                pthread_mutex_unlock(&ch->mu);
                if (timing) t_wake = channel_rdtsc();
                wake_batch_flush();
                cc__chan_broadcast_activity();
                if (timing) {
                    uint64_t done = channel_rdtsc();
                    channel_timing_record_send(t0, t_lock, t_enqueue, t_wake, done);
                }
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
                CC_FIBER_PARK("chan_send: buffer full, waiting for space");
                pthread_mutex_lock(&ch->mu);
                if (!atomic_load_explicit(&node.notified, memory_order_acquire)) {
                    cc__chan_remove_send_waiter(ch, &node);
                }
            } else {
                pthread_cond_wait(&ch->not_full, &ch->mu);
            }
        }
        
        pthread_mutex_unlock(&ch->mu);
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

int cc_chan_recv(CCChan* ch, void* out_value, size_t value_size) {
    if (!ch || !out_value || value_size == 0) return EINVAL;
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
        int rc = cc_chan_try_dequeue_lockfree(ch, out_value);
        if (rc == 0) {
            if (timing) {
                uint64_t done = channel_rdtsc();
                channel_timing_record_recv(t0, t0, done, done, done);
            }
            if (ch->send_waiters_head) {
                cc_chan_lock(ch);
                cc__chan_wake_one_send_waiter(ch);
                pthread_cond_signal(&ch->not_full);
                pthread_mutex_unlock(&ch->mu);
                wake_batch_flush();
            }
            cc__chan_broadcast_activity();
            return 0;
        }
        if (ch->closed) {
            cc_chan_lock(ch);
            if (ch->closed && cc_chan_try_dequeue_lockfree(ch, out_value) != 0) {
                pthread_mutex_unlock(&ch->mu);
                return EPIPE;
            }
            pthread_mutex_unlock(&ch->mu);
            return 0;
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
        /* Try lock-free dequeue */
        pthread_mutex_unlock(&ch->mu);
        int rc = cc_chan_try_dequeue_lockfree(ch, out_value);
        if (rc == 0) {
            
            if (timing) t_dequeue = channel_rdtsc();
            cc_chan_lock(ch);
            cc__chan_wake_one_send_waiter(ch);
            pthread_cond_signal(&ch->not_full);
            pthread_mutex_unlock(&ch->mu);
            if (timing) t_wake = channel_rdtsc();
            wake_batch_flush();
            cc__chan_broadcast_activity();
            if (timing) {
                uint64_t done = channel_rdtsc();
                channel_timing_record_recv(t0, t_lock, t_dequeue, t_wake, done);
            }
            return 0;
        }
        pthread_mutex_lock(&ch->mu);

        if (ch->closed) break;
        
        /* Wait for data */
        if (fiber) {
            cc__fiber_wait_node node = {0};
            node.fiber = fiber;
            node.data = out_value;  /* For direct handoff */
            atomic_store(&node.notified, 0);
            cc__chan_add_recv_waiter(ch, &node);
            pthread_mutex_unlock(&ch->mu);
            CC_FIBER_PARK("chan_recv: buffer empty, waiting for data");
            pthread_mutex_lock(&ch->mu);
            int notified = atomic_load_explicit(&node.notified, memory_order_acquire);
            if (notified == 1) {
                /* Sender did direct handoff - data is already in out_value */
                pthread_mutex_unlock(&ch->mu);
                if (timing) {
                    uint64_t done = channel_rdtsc();
                    channel_timing_record_recv(t0, t_lock, done, done, done);
                }
                return 0;
            }
            if (notified == 3 || ch->closed) {
                /* Channel closed while we were waiting */
                cc__chan_remove_recv_waiter(ch, &node);
                break;
            }
            cc__chan_remove_recv_waiter(ch, &node);
        } else {
            pthread_cond_wait(&ch->not_empty, &ch->mu);
        }
    }
    
    /* Channel closed - try one more dequeue for any remaining data */
    pthread_mutex_unlock(&ch->mu);
    int rc = cc_chan_try_dequeue_lockfree(ch, out_value);
    
    if (rc == 0) {
        if (timing) {
            uint64_t done = channel_rdtsc();
            channel_timing_record_recv(t0, t_lock, done, done, done);
        }
        return 0;
    }
    return EPIPE;
}


int cc_chan_try_send(CCChan* ch, const void* value, size_t value_size) {
    if (!ch || !value || value_size == 0) return EINVAL;
    
    /* Lock-free fast path for buffered channels with small elements.
     * Large elements (> sizeof(void*)) use mutex path to avoid slot wrap-around race. */
    if (ch->use_lockfree && ch->cap > 0 && ch->elem_size == value_size && ch->buf &&
        ch->elem_size <= sizeof(void*)) {
        if (ch->closed) return EPIPE;
        int rc = cc_chan_try_enqueue_lockfree(ch, value);
        if (rc == 0) {
            /* Signal any waiters */
            if (ch->recv_waiters_head) {
                cc_chan_lock(ch);
                cc__chan_signal_recv_waiter(ch);
                pthread_cond_signal(&ch->not_empty);
                pthread_mutex_unlock(&ch->mu);
                wake_batch_flush();
            }
            cc__chan_broadcast_activity();
            return 0;
        }
        return EAGAIN;
    }
    
    /* Unbuffered channels: check closed before mutex path */
    if (ch->cap == 0 && ch->closed) return EPIPE;
    
    /* Standard mutex path */
    pthread_mutex_lock(&ch->mu);
    int err = cc_chan_ensure_buf(ch, value_size);
    if (err != 0) { pthread_mutex_unlock(&ch->mu); return err; }
    if (ch->closed) { pthread_mutex_unlock(&ch->mu); return EPIPE; }
    if (ch->cap == 0) {
        /* Non-blocking rendezvous: only send if a receiver is waiting. */
        cc__fiber_wait_node* rnode = cc__chan_pop_recv_waiter(ch);
        if (!rnode) {
            pthread_mutex_unlock(&ch->mu);
            return ch->closed ? EPIPE : EAGAIN;
        }
        channel_store_slot(rnode->data, value, ch->elem_size);
        atomic_store_explicit(&rnode->notified, 1, memory_order_release);
        if (rnode->fiber) {
            wake_batch_add(rnode->fiber);
        } else {
            pthread_cond_signal(&ch->not_empty);
        }
        if (ch->rv_recv_waiters > 0) ch->rv_recv_waiters--;
        pthread_mutex_unlock(&ch->mu);
        wake_batch_flush();
        return 0;
    }
    
    /* Buffered with lock-free small elements: try lock-free first */
    if (ch->use_lockfree && ch->elem_size <= sizeof(void*)) {
        pthread_mutex_unlock(&ch->mu);
        int rc = cc_chan_try_enqueue_lockfree(ch, value);
        if (rc == 0) {
            cc_chan_lock(ch);
            cc__chan_signal_recv_waiter(ch);
            pthread_cond_signal(&ch->not_empty);
            pthread_mutex_unlock(&ch->mu);
            wake_batch_flush();
            cc__chan_broadcast_activity();
            return 0;
        }
        return EAGAIN;
    }
    
    if (ch->count == ch->cap) {
        err = cc_chan_handle_full_send(ch, value, NULL);
        if (err != 0) { pthread_mutex_unlock(&ch->mu); return err; }
    }
    cc_chan_enqueue(ch, value);
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
            if (ch->send_waiters_head) {
                cc_chan_lock(ch);
                cc__chan_wake_one_send_waiter(ch);
                pthread_cond_signal(&ch->not_full);
                pthread_mutex_unlock(&ch->mu);
                wake_batch_flush();
            }
            cc__chan_broadcast_activity();
            return 0;
        }
        return ch->closed ? EPIPE : EAGAIN;
    }
    
    /* Standard mutex path */
    pthread_mutex_lock(&ch->mu);
    int err = cc_chan_ensure_buf(ch, value_size);
    if (err != 0) { pthread_mutex_unlock(&ch->mu); return err; }
    if (ch->cap == 0) {
        cc__fiber_wait_node* snode = cc__chan_pop_send_waiter(ch);
        if (!snode) {
            pthread_mutex_unlock(&ch->mu);
            return ch->closed ? EPIPE : EAGAIN;
        }
        channel_load_slot(snode->data, out_value, ch->elem_size);
        atomic_store_explicit(&snode->notified, 1, memory_order_release);
        if (snode->fiber) {
            wake_batch_add(snode->fiber);
        } else {
            pthread_cond_signal(&ch->not_full);
        }
        pthread_mutex_unlock(&ch->mu);
        wake_batch_flush();
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
            cc__chan_broadcast_activity();
            return 0;
        }
        return ch->closed ? EPIPE : EAGAIN;
    }
    
    if (ch->count == 0) { pthread_mutex_unlock(&ch->mu); return ch->closed ? EPIPE : EAGAIN; }
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
        if (ch->closed) return EPIPE;
        int rc = cc_chan_try_enqueue_lockfree(ch, value);
        if (rc == 0) {
            /* Signal any recv waiters */
            if (ch->recv_waiters_head) {
                pthread_mutex_lock(&ch->mu);
                cc__chan_signal_recv_waiter(ch);
                pthread_cond_signal(&ch->not_empty);
                pthread_mutex_unlock(&ch->mu);
                wake_batch_flush();
            }
            cc__chan_broadcast_activity();
            return 0;
        }
        /* Lock-free failed (queue full), fall through to blocking path */
    }
    
    pthread_mutex_lock(&ch->mu);
    int err = cc_chan_ensure_buf(ch, value_size);
    if (err != 0) { pthread_mutex_unlock(&ch->mu); return err; }
    if (ch->closed) { pthread_mutex_unlock(&ch->mu); return EPIPE; }
    if (ch->cap == 0) {
        err = cc_chan_send_unbuffered(ch, value, abs_deadline);
        pthread_mutex_unlock(&ch->mu);
        wake_batch_flush();
        return err;
    }
    
    /* For lock-free channels, poll while waiting */
    if (ch->use_lockfree) {
        while (!ch->closed) {
            pthread_mutex_unlock(&ch->mu);
            int rc = cc_chan_try_enqueue_lockfree(ch, value);
            if (rc == 0) {
                pthread_mutex_lock(&ch->mu);
                cc__chan_signal_recv_waiter(ch);
                pthread_cond_signal(&ch->not_empty);
                pthread_mutex_unlock(&ch->mu);
                wake_batch_flush();
                cc__chan_broadcast_activity();
                return 0;
            }
            pthread_mutex_lock(&ch->mu);
            if (ch->closed) break;
            
            /* Timed wait - wake periodically to check lock-free queue */
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
            /* Wake any send waiters */
            if (ch->send_waiters_head) {
                pthread_mutex_lock(&ch->mu);
                cc__chan_wake_one_send_waiter(ch);
                pthread_cond_signal(&ch->not_full);
                pthread_mutex_unlock(&ch->mu);
                wake_batch_flush();
            }
            cc__chan_broadcast_activity();
            return 0;
        }
        /* Check if closed and empty */
        if (ch->closed) {
            pthread_mutex_lock(&ch->mu);
            if (ch->closed && cc_chan_try_dequeue_lockfree(ch, out_value) != 0) {
                pthread_mutex_unlock(&ch->mu);
                return EPIPE;
            }
            pthread_mutex_unlock(&ch->mu);
            return 0;
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
    
    /* For lock-free channels, poll the lock-free queue while waiting */
    if (ch->use_lockfree) {
        while (!ch->closed) {
            pthread_mutex_unlock(&ch->mu);
            int rc = cc_chan_try_dequeue_lockfree(ch, out_value);
            if (rc == 0) {
                pthread_mutex_lock(&ch->mu);
                cc__chan_wake_one_send_waiter(ch);
                pthread_cond_signal(&ch->not_full);
                pthread_mutex_unlock(&ch->mu);
                wake_batch_flush();
                cc__chan_broadcast_activity();
                return 0;
            }
            pthread_mutex_lock(&ch->mu);
            if (ch->closed) break;
            
            /* Timed wait - wake periodically to check lock-free queue */
            struct timespec poll_deadline;
            clock_gettime(CLOCK_REALTIME, &poll_deadline);
            poll_deadline.tv_nsec += 10000000; /* 10ms */
            if (poll_deadline.tv_nsec >= 1000000000) {
                poll_deadline.tv_nsec -= 1000000000;
                poll_deadline.tv_sec++;
            }
            /* Use earlier of poll deadline or caller deadline */
            const struct timespec* wait_deadline = abs_deadline;
            if (abs_deadline && (poll_deadline.tv_sec < abs_deadline->tv_sec ||
                (poll_deadline.tv_sec == abs_deadline->tv_sec && poll_deadline.tv_nsec < abs_deadline->tv_nsec))) {
                wait_deadline = &poll_deadline;
            }
            
            err = pthread_cond_timedwait(&ch->not_empty, &ch->mu, wait_deadline ? wait_deadline : &poll_deadline);
            if (err == ETIMEDOUT) {
                /* Check if caller deadline expired */
                if (abs_deadline) {
                    struct timespec now;
                    clock_gettime(CLOCK_REALTIME, &now);
                    if (now.tv_sec > abs_deadline->tv_sec ||
                        (now.tv_sec == abs_deadline->tv_sec && now.tv_nsec >= abs_deadline->tv_nsec)) {
                        pthread_mutex_unlock(&ch->mu);
                        return ETIMEDOUT;
                    }
                }
                /* Poll deadline expired, loop to check lock-free queue again */
            }
        }
        pthread_mutex_unlock(&ch->mu);
        return ch->closed ? EPIPE : ETIMEDOUT;
    }
    
    err = cc_chan_wait_empty(ch, abs_deadline);
    if (err != 0) { pthread_mutex_unlock(&ch->mu); return err; }
    cc_chan_dequeue(ch, out_value);
    pthread_mutex_unlock(&ch->mu);
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

// Non-blocking match helper
int cc_chan_match_try(CCChanMatchCase* cases, size_t n, size_t* ready_index) {
    if (!cases || n == 0 || !ready_index) return EINVAL;
    for (size_t i = 0; i < n; ++i) {
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

int cc_chan_match_deadline(CCChanMatchCase* cases, size_t n, size_t* ready_index, const CCDeadline* deadline) {
    if (!cases || n == 0 || !ready_index) return EINVAL;
    struct timespec ts;
    const struct timespec* p = cc_deadline_as_timespec(deadline, &ts);
    
    /* Multi-channel select: Use global broadcast condvar.
       Any channel activity (send/recv/close) wakes all waiters.
       Simple, deadlock-free, at cost of some spurious wakeups. */
    while (1) {
        int rc = cc_chan_match_try(cases, n, ready_index);
        if (rc == 0 || rc == EPIPE) return rc;
        if (rc != EAGAIN) return rc;
        if (p) {
            struct timespec now; clock_gettime(CLOCK_REALTIME, &now);
            if (now.tv_sec > p->tv_sec || (now.tv_sec == p->tv_sec && now.tv_nsec >= p->tv_nsec)) {
                return ETIMEDOUT;
            }
        }
        
        /* Wait for any channel activity */
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

#include <ccc/std/task_intptr.cch>

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