/*
 * Hybrid Scheduler V2 — global-queue fiber/thread scheduler.
 *
 * Fibers:
 *   - QUEUED  -> on the global runnable queue
 *   - RUNNING -> owned by one BUSY worker
 *   - PARKED  -> blocked until an external signal re-enqueues it
 *   - DEAD    -> completed
 *
 * Workers:
 *   - BUSY -> draining/running fibers from the global queue
 *   - IDLE -> available for work or sleeping on the worker wake primitive
 *
 * A RUNNING fiber may carry an internal "signal pending" bit so the
 * post-resume commit path requeues it instead of parking. That bit is an
 * implementation detail, not a separate fiber state.
 */

#include "sched_v2.h"
#include "wake_primitive.h"
#include "fiber_internal.h"
#include "minicoro.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <stdbool.h>
#include <unistd.h>
#include <sched.h>
#include <time.h>

#if defined(__APPLE__)
#include <os/lock.h>
#endif

/* ============================================================================
 * v2_slock: short-critical-section lock
 *
 * We use this only for micro critical sections (four or five pointer stores
 * in the intrusive ready queue). On macOS, os_unfair_lock is a userspace
 * adaptive lock — uncontended acquire is a single CAS (~3-5 ns), contended
 * path falls into __ulock_wait. That's 3-4x faster than pthread_mutex
 * firstfit on the uncontended path, which dominates here.
 *
 * On non-Apple platforms we fall back to an atomic-flag spinlock with a
 * bounded busy loop + sched_yield. On Linux we could use pthread_spinlock,
 * but a plain atomic spin keeps the abstraction portable and avoids any
 * glibc-vs-musl quirks; the critical section is a handful of pointer ops.
 * ============================================================================ */
#if defined(__APPLE__)
typedef os_unfair_lock v2_slock;
#define V2_SLOCK_INIT OS_UNFAIR_LOCK_INIT
static inline void v2_slock_init(v2_slock* l)   { *l = (os_unfair_lock)OS_UNFAIR_LOCK_INIT; }
static inline void v2_slock_lock(v2_slock* l)   { os_unfair_lock_lock(l); }
static inline void v2_slock_unlock(v2_slock* l) { os_unfair_lock_unlock(l); }
#else
typedef struct { _Atomic uint32_t state; } v2_slock;
#define V2_SLOCK_INIT {0}
static inline void v2_slock_init(v2_slock* l) {
    atomic_store_explicit(&l->state, 0, memory_order_relaxed);
}
static inline void v2_slock_lock(v2_slock* l) {
    for (int spins = 0; ; ++spins) {
        uint32_t expected = 0;
        if (atomic_compare_exchange_weak_explicit(&l->state, &expected, 1u,
                memory_order_acquire, memory_order_relaxed)) {
            return;
        }
        if (spins < 64) {
#if defined(__x86_64__) || defined(__i386__)
            __asm__ volatile("pause" ::: "memory");
#elif defined(__aarch64__) || defined(__arm__)
            __asm__ volatile("yield" ::: "memory");
#else
            __asm__ volatile("" ::: "memory");
#endif
        } else {
            sched_yield();
        }
    }
}
static inline void v2_slock_unlock(v2_slock* l) {
    atomic_store_explicit(&l->state, 0u, memory_order_release);
}
#endif

/* ============================================================================
 * Configuration
 * ============================================================================ */

/* Hard cap on active worker slots in the scheduler pool.
 *
 * At steady state we run at CC_V2_THREADS (~= CPU count). Sysmon's
 * syscall-age eviction replaces aged workers in place — the orphaned
 * thread leaves the pool (pthread_detach) while a fresh worker takes
 * over the same slot — so this cap bounds active pool size, not total
 * threads ever alive. Orphans are off-books and bounded only by
 * V2_ORPHAN_SAFETY_CAP (see below). */
#define V2_MAX_THREADS     256
#define V2_GLOBAL_QUEUE_SIZE 4096
#if defined(__OPTIMIZE__)
#define V2_FIBER_STACK_SIZE (2 * 1024 * 1024)
#else
/* Unoptimized builds grow frame size substantially; keep debug binaries usable. */
#define V2_FIBER_STACK_SIZE (8 * 1024 * 1024)
#endif
/* Tick cadence: fast enough that V2_SYSMON_SYSCALL_AGE_NS (below) gives
 * bounded detection latency. 20 ms tick + 20 ms age threshold yields worst
 * case ~40 ms before a kidnapped worker is detached. */
#define V2_SYSMON_INTERVAL_MS 20

/* Syscall-age detach: a worker whose current fiber has been running (not
 * yielded) for this long is assumed to be kidnapped in a blocking kernel
 * syscall. Sysmon evicts it in place (detach old, spawn replacement into
 * the same slot). The kidnapped worker runs its fiber to completion, sees
 * the slot generation has moved, and exits.
 *
 * 20 ms is a deliberate middle ground: long enough to tolerate normal CPU
 * bursts (deflate chunks, JSON parsing, etc), short enough to keep blocking
 * IO from starving the ready queue for a noticeable fraction of a second. */
#define V2_SYSMON_SYSCALL_AGE_NS (20ull * 1000000ull)

/* Safety cap on concurrent orphans (evicted workers still draining their
 * kidnapped syscalls, off the scheduler pool). This is NOT a policy knob
 * for the common case — at steady state sysmon creates exactly as many
 * replacements as there are queued fibers, self-regulating via the
 * per-tick budget. This cap exists so a pathological app that spawns tens
 * of thousands of concurrent blocking fibers can't take the machine down
 * with kernel-stack exhaustion. When reached, sysmon simply skips
 * eviction for that tick and queued work waits a little longer. */
#define V2_ORPHAN_SAFETY_CAP 4096

/* ============================================================================
 * Fiber
 * ============================================================================ */

enum {
    V2_YIELD_PARK = 0,  /* Fiber wants to park (default after yield) */
    V2_YIELD_YIELD = 1, /* Fiber wants to re-enqueue (voluntary yield) */
};

enum {
    FIBER_V2_STATE_MASK = 0x0f,
    FIBER_V2_FLAG_SIGNAL_PENDING = 0x10,
    FIBER_V2_STATE_COUNT = 5,
};

static inline int fiber_v2_state_base(int raw_state) {
    return raw_state & FIBER_V2_STATE_MASK;
}

static inline int fiber_v2_state_has_signal_pending(int raw_state) {
    return (raw_state & FIBER_V2_FLAG_SIGNAL_PENDING) != 0;
}

struct fiber_v2 {
    mco_coro*  coro;
    _Atomic int state;    /* Base state plus FIBER_V2_FLAG_SIGNAL_PENDING. */
    int        last_thread_id;
    uint64_t   generation;

    void* (*entry_fn)(void*);
    void*      entry_arg;
    void*      result;
    _Atomic int done;         /* Set to 1 when fiber completes, before recycling */
    char       result_buf[48] __attribute__((aligned(8)));
    _Atomic uint64_t wait_ticket;
    int        yield_kind;      /* V2_YIELD_PARK or V2_YIELD_YIELD */
    const char* park_reason;

    /* Deadlock-detector metadata. All four are written from V2 fiber context
     * via the cc__fiber_* / cc_deadlock_suppress / cc_external_wait shims so
     * the detector can walk g_v2.all_fibers and classify each parked fiber.
     *   park_obj              — what we're parked on (CCChan*, V2 fiber for
     *                            sched_v2_join, etc).
     *   deadlock_suppress_depth — mirror of tls_deadlock_suppress_depth,
     *                            pinned to the fiber so it survives migration
     *                            across V2 workers.
     *   external_wait_depth   — mirror of tls_external_wait_depth, same
     *                            purpose.
     * A V2 fiber with suppress_depth>0 OR external_wait_depth>0 is exempt
     * from deadlock detection. */
    void*      park_obj;
    uint32_t   deadlock_suppress_depth;
    uint32_t   external_wait_depth;

    wake_primitive done_wake;
    cc__fiber* _Atomic join_waiter_fiber;
    CCNursery* saved_nursery;
    CCNursery* admission_nursery;

    /* Intrusive linked list for free list */
    fiber_v2*  next;
    fiber_v2*  all_next;
};

/* ============================================================================
 * Intrusive MPMC ready queue (v2_slock + doubly-linked fiber list).
 *
 * The critical section is ~4-5 pointer stores; we use v2_slock instead of
 * pthread_mutex so the uncontended acquire collapses to a single CAS on
 * macOS (os_unfair_lock) and an atomic-flag spin elsewhere. The previous
 * pthread_mutex-based version showed up in profiles as the top
 * __psynch_mutexwait / _pthread_mutex_firstfit_lock_slow consumer under
 * redis_hybrid load; at ~8M push/pop pairs/sec the uncontended mutex cost
 * alone was a noticeable fraction of total time.
 * ============================================================================ */

typedef struct {
    v2_slock mu;
    fiber_v2* head;
    fiber_v2* tail;
    _Atomic size_t count;
} v2_queue;

static void v2_queue_init(v2_queue* q) {
    v2_slock_init(&q->mu);
    q->head = NULL;
    q->tail = NULL;
    atomic_store_explicit(&q->count, 0, memory_order_relaxed);
}

/* Returns the pre-push count (i.e. queue depth before this push).
 * Used by sched_v2_enqueue_runnable to skip the wake syscall when the
 * queue is already deep enough that a drainer is known to be on it. */
static int v2_queue_push(v2_queue* q, fiber_v2* f) {
    f->next = NULL;
    v2_slock_lock(&q->mu);
    if (q->tail) {
        q->tail->next = f;
    } else {
        q->head = f;
    }
    q->tail = f;
    int prev = atomic_fetch_add_explicit(&q->count, 1, memory_order_relaxed);
    v2_slock_unlock(&q->mu);
    return prev;
}

static fiber_v2* v2_queue_pop(v2_queue* q) {
    if (atomic_load_explicit(&q->count, memory_order_relaxed) == 0) return NULL;
    v2_slock_lock(&q->mu);
    fiber_v2* f = q->head;
    if (f) {
        q->head = f->next;
        if (!q->head) q->tail = NULL;
        f->next = NULL;
        atomic_fetch_sub_explicit(&q->count, 1, memory_order_relaxed);
    }
    v2_slock_unlock(&q->mu);
    return f;
}

/* ============================================================================
 * Thread state
 * ============================================================================ */

typedef struct {
    pthread_t    handle;
    int          id;
    _Atomic int  alive;     /* 1 while worker loop is running */
    _Atomic int  is_idle;   /* 1 = IDLE worker, 0 = BUSY worker */
    /* Wall-clock ns of the most recent fiber dispatch on this worker.
     * 0 when no fiber is currently executing. Sysmon scans this to detect
     * workers stuck in non-cooperative kernel syscalls. */
    _Atomic uint64_t dispatch_start_ns;
    /* Slot-identity generation. Bumped by sysmon every time a new worker
     * thread is installed into this slot (replacing an evicted orphan). A
     * running worker caches its generation at entry (tls_v2_my_generation);
     * when it observes slot.generation != my_gen, it's been orphaned and
     * must exit its loop without touching slot.wake or slot.is_idle. This
     * is the identity channel between sysmon and the worker — no separate
     * "detached" flag is needed. */
    _Atomic uint64_t generation;
    wake_primitive wake;
} thread_v2;

/* ============================================================================
 * Global scheduler state
 * ============================================================================ */

struct sched_v2_state {
    thread_v2    threads[V2_MAX_THREADS];
    _Atomic int  num_threads;
    _Atomic int  running;
    _Atomic int  idle_workers;
    int          allow_expand;

    /* Single global ready queue */
    v2_queue ready_queue;

    /* Fiber free list (lock-free CAS stack) */
    fiber_v2* _Atomic free_list;
    pthread_mutex_t all_fibers_mu;
    fiber_v2* all_fibers;
    _Atomic size_t fiber_count;

    /* Sysmon */
    pthread_t    sysmon_handle;
    wake_primitive sysmon_wake;

    /* Init */
    pthread_once_t init_once;
    int            initialized;
};

static struct sched_v2_state g_v2 = {
    .init_once = PTHREAD_ONCE_INIT,
};

static _Atomic uint64_t g_v2_sysmon_stall_detect = 0;
extern void cc__io_wait_dump_kq_diag(void);
extern void cc__fiber_dump_unpark_reason_stats(void);
extern void cc_sched_wait_many_dump_diag(void);
extern void cc__socket_wait_dump_diag(void);

/* Diagnostic counters */
static _Atomic uint64_t g_v2_fibers_alive = 0;
static _Atomic uint64_t g_v2_signal_ok = 0;
static _Atomic uint64_t g_v2_signal_pending = 0;
static _Atomic uint64_t g_v2_signal_running_pending_set = 0;
static _Atomic uint64_t g_v2_signal_running_pending_already_set = 0;
static _Atomic uint64_t g_v2_signal_dropped = 0;
static _Atomic uint64_t g_v2_signal_dropped_state[FIBER_V2_STATE_COUNT] = {0};
static _Atomic uint64_t g_v2_parks = 0;
static _Atomic uint64_t g_v2_run_yield_requeue = 0;
static _Atomic uint64_t g_v2_run_dead = 0;
static _Atomic uint64_t g_v2_run_commit_parked = 0;
static _Atomic uint64_t g_v2_run_pending_requeue = 0;
static _Atomic uint64_t g_v2_run_commit_park_fail_state[FIBER_V2_STATE_COUNT] = {0};
static _Atomic uint64_t g_v2_worker_idle_entries = 0;
static _Atomic uint64_t g_v2_worker_busy_from_recheck = 0;
static _Atomic uint64_t g_v2_worker_busy_from_wake = 0;
/* Wake path instrumentation (producer side of sched_v2_wake(-1)).
 *   wake_calls_ext:       every external wake invocation (after fence).
 *   wake_no_idle:          returned without scanning — idle_workers==0 fast path
 *                          (no CAS, no syscall, pure fence cost).
 *   wake_issued:           we CAS-claimed an idle worker and issued
 *                          wake_primitive_wake_one (= ulock syscall).
 *   wake_scan_miss:        scanned threads but lost every CAS race (another
 *                          producer or the worker itself already claimed it).
 *   worker_self_drain:     fibers run through the tid==worker_hint path
 *                          without any syscall. */
static _Atomic uint64_t g_v2_wake_calls_ext = 0;
static _Atomic uint64_t g_v2_wake_no_idle = 0;
static _Atomic uint64_t g_v2_wake_issued = 0;
static _Atomic uint64_t g_v2_wake_scan_miss = 0;
/* Pushes that skipped sched_v2_wake because the ready queue was already
 * deep enough (>= g_v2_wake_skip_depth) that an existing drainer/wake-in-
 * flight will pick up the new item via self-drain. Saves the seq_cst fence
 * and idle-worker scan on the enqueue hot path. Correctness net: the
 * parking-worker Dekker recheck catches the push->park transition race,
 * and sysmon issues an unconditional safety-net wake every tick. */
static _Atomic uint64_t g_v2_wake_skipped_deep = 0;
static _Atomic uint64_t g_v2_worker_self_drain = 0;
static _Atomic uint64_t g_v2_mco_resume_fail = 0;
static _Atomic uint64_t g_v2_mco_yield_fail = 0;
static _Atomic uint64_t g_v2_mco_stack_overflow = 0;
static _Atomic uint64_t g_v2_mco_fail_logs = 0;
/* Fiber-pool effectiveness: how often a spawn reused an existing coroutine
 * (and its ~2 MB stack) vs. had to allocate a fresh one. */
static _Atomic uint64_t g_v2_coro_reuse = 0;
static _Atomic uint64_t g_v2_coro_fresh = 0;

/* sched_v2_join outcome distribution: how often the joiner found the task
 * already done at entry, caught it during the spin, or had to fully park. */
static _Atomic uint64_t g_v2_join_fast = 0;
static _Atomic uint64_t g_v2_join_spin_hit = 0;
static _Atomic uint64_t g_v2_join_park_fiber = 0;
static _Atomic uint64_t g_v2_join_park_thread = 0;

/* Syscall-age detach counters. Cheap and always on (no V2_STAT_INC gate):
 * only touched by sysmon and by the orphan exit path, both low-frequency.
 *   evicted_total : monotonic count of slot-in-place replacements.
 *   orphans_alive : live orphans right now (sysmon increments on evict,
 *                   orphan decrements when its thread_v2_main returns).
 *   orphans_cap_hit : sysmon skipped an eviction because safety cap was
 *                     reached. Non-zero = pathological workload. */
static _Atomic uint64_t g_v2_sysmon_evicted_total = 0;
static _Atomic int64_t  g_v2_orphans_alive = 0;
static _Atomic uint64_t g_v2_orphans_cap_hit = 0;

/* Master toggle: set CC_V2_SYSMON_DETACH=0 to disable the detach mechanism
 * (pool becomes hard-capped at CC_V2_THREADS as before). Default: enabled. */
static _Atomic int g_v2_sysmon_detach_enabled = 1;

static inline uint64_t v2_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* Diagnostic-counter gate.  Writes to the g_v2_* stat counters on the hot
 * signal/wake/park/resume paths used to be unconditional -- at multi-million
 * ops/sec this became ~10-20M contended RMWs/s on cold globals across worker
 * threads purely for diagnostics that are only dumped when CC_V2_STATS is
 * opted in.  Set from sched_v2_global_init().  The V2_STAT_INC macro branches
 * out the RMW entirely in the common case (no stats). */
static _Atomic int g_v2_stats_enabled = 0;

static inline int cc_v2_stats_enabled(void) {
    return atomic_load_explicit(&g_v2_stats_enabled, memory_order_relaxed);
}

#define V2_STAT_INC(counter) \
    do { \
        if (__builtin_expect(cc_v2_stats_enabled(), 0)) \
            atomic_fetch_add_explicit(&(counter), 1, memory_order_relaxed); \
    } while (0)
#define V2_STAT_DEC(counter) \
    do { \
        if (__builtin_expect(cc_v2_stats_enabled(), 0)) \
            atomic_fetch_sub_explicit(&(counter), 1, memory_order_relaxed); \
    } while (0)

/* Tunable via CC_V2_JOIN_SPIN env var. Default 0: measurement on pigz (and
 * any workload where the joinee runs much longer than the spin budget)
 * showed the spin never catches a ready task — all joins either hit the
 * fast path at entry or have to park anyway. A non-zero value is retained
 * as an env-var knob for low-latency workloads that might benefit. */
static int g_v2_join_spin = 0;

/* Tunable via CC_V2_WAKE_SKIP_DEPTH env var.
 *
 * On every sched_v2_enqueue_runnable we push one fiber and then call
 * sched_v2_wake(-1), which does a seq_cst fence + idle_workers scan.
 * Once the ready queue is already deep enough that a drainer is either
 * actively running (self-draining inline via sched_v2_wake(tid)) or
 * a previous push already kicked a wake, the N-th push's wake is
 * redundant work. We bound the per-push cost by skipping the wake call
 * entirely when the pre-push depth was >= this threshold.
 *
 * 0 disables (always wake = legacy behaviour). Default 4: a 0->4 ramp
 * still fires up to 4 wakes, after which the fan-out is capped and the
 * drainers+self-drain carry the load. Sysmon's every-tick safety-net
 * wake (see sched_v2_sysmon_main) bounds any under-utilisation to one
 * tick (~20ms). */
static int g_v2_wake_skip_depth = 4;

/* Histogram bucket for park-reason diagnostics. We dedupe by pointer because
 * park reasons are static strings; falling back to strcmp keeps the table
 * small without missing accidental copies. */
typedef struct {
    const char* reason;
    uint64_t count;
} park_reason_bucket;

#define V2_DIAG_REASON_BUCKETS 16

static void sched_v2_diag_scan_fibers(uint64_t state_counts[FIBER_V2_STATE_COUNT],
                                      uint64_t* parked_wait_many,
                                      uint64_t* parked_wait,
                                      uint64_t* parked_other,
                                      uint64_t* parked_unknown,
                                      park_reason_bucket reason_buckets[V2_DIAG_REASON_BUCKETS],
                                      size_t* reason_bucket_count) {
    for (int i = 0; i < FIBER_V2_STATE_COUNT; ++i) {
        state_counts[i] = 0;
    }
    *parked_wait_many = 0;
    *parked_wait = 0;
    *parked_other = 0;
    *parked_unknown = 0;
    if (reason_buckets) {
        for (size_t i = 0; i < V2_DIAG_REASON_BUCKETS; ++i) {
            reason_buckets[i].reason = NULL;
            reason_buckets[i].count = 0;
        }
    }
    if (reason_bucket_count) *reason_bucket_count = 0;

    pthread_mutex_lock(&g_v2.all_fibers_mu);
    for (fiber_v2* f = g_v2.all_fibers; f; f = f->all_next) {
        int state = fiber_v2_state_base(atomic_load_explicit(&f->state, memory_order_acquire));
        if (state >= 0 && state < FIBER_V2_STATE_COUNT) {
            state_counts[state]++;
            if (state == FIBER_V2_PARKED) {
                const char* r = f->park_reason;
                if (!r) {
                    (*parked_unknown)++;
                } else if (strcmp(r, "cc_sched_fiber_wait_many") == 0) {
                    (*parked_wait_many)++;
                } else if (strcmp(r, "cc_sched_fiber_wait") == 0) {
                    (*parked_wait)++;
                } else {
                    (*parked_other)++;
                }
                if (reason_buckets && reason_bucket_count) {
                    const char* key = r ? r : "(null)";
                    int found = 0;
                    for (size_t i = 0; i < *reason_bucket_count; ++i) {
                        if (reason_buckets[i].reason == key ||
                            (reason_buckets[i].reason && key &&
                             strcmp(reason_buckets[i].reason, key) == 0)) {
                            reason_buckets[i].count++;
                            found = 1;
                            break;
                        }
                    }
                    if (!found && *reason_bucket_count < V2_DIAG_REASON_BUCKETS) {
                        reason_buckets[*reason_bucket_count].reason = key;
                        reason_buckets[*reason_bucket_count].count = 1;
                        (*reason_bucket_count)++;
                    }
                }
            }
        }
    }
    pthread_mutex_unlock(&g_v2.all_fibers_mu);
}

/* Per-thread state */
static __thread int tls_v2_thread_id = -1;
static __thread uint64_t tls_v2_my_generation = 0;
static __thread fiber_v2* tls_v2_current_fiber = NULL;
static __thread const char* tls_v2_park_reason = NULL;
extern __thread CCNursery* cc__tls_current_nursery;
bool cc_nursery_is_cancelled(const CCNursery* n);

static void fiber_v2_entry(mco_coro* co) {
    fiber_v2* f = (fiber_v2*)mco_get_user_data(co);
    if (!f) return;
    atomic_thread_fence(memory_order_acquire);
    CCNursery* prev_nursery = cc__tls_current_nursery;
    cc__tls_current_nursery = f->saved_nursery;
    if (f->entry_fn) {
        if (f->admission_nursery && cc_nursery_is_cancelled(f->admission_nursery)) {
            f->result = NULL;
        } else {
            f->result = f->entry_fn(f->entry_arg);
        }
    } else {
        f->result = NULL;
    }
    f->admission_nursery = NULL;
    cc__tls_current_nursery = prev_nursery;
}

/* ============================================================================
 * Fiber pool
 * ============================================================================ */

static fiber_v2* fiber_v2_alloc(void) {
    /* Try free list first */
    fiber_v2* f = atomic_load_explicit(&g_v2.free_list, memory_order_acquire);
    while (f) {
        fiber_v2* next = f->next;
        if (atomic_compare_exchange_weak_explicit(&g_v2.free_list, &f, next,
                memory_order_release, memory_order_acquire)) {
            f->generation++;
            f->next = NULL;
            /* Keep f->coro as-is: fiber_v2_free left the minicoro allocation
             * (coro struct + ~2 MB stack) alive so spawn can mco_init in
             * place. Setting it NULL here would force a fresh alloc every
             * time and defeat the pool. */
            f->result = NULL;
            f->entry_fn = NULL;
            f->entry_arg = NULL;
            f->last_thread_id = -1;
            f->saved_nursery = NULL;
            f->admission_nursery = NULL;
            atomic_store_explicit(&f->done, 0, memory_order_relaxed);
            atomic_store_explicit(&f->wait_ticket, 0, memory_order_relaxed);
            atomic_store_explicit(&f->join_waiter_fiber, NULL, memory_order_relaxed);
            f->yield_kind = V2_YIELD_PARK;
            f->park_reason = NULL;
            f->park_obj = NULL;
            f->deadlock_suppress_depth = 0;
            f->external_wait_depth = 0;
            atomic_store_explicit(&f->state, FIBER_V2_IDLE, memory_order_relaxed);
            V2_STAT_INC(g_v2_fibers_alive);
            return f;
        }
    }

    f = (fiber_v2*)calloc(1, sizeof(fiber_v2));
    if (!f) return NULL;
    f->coro = NULL;
    f->last_thread_id = -1;
    f->park_reason = NULL;
    f->saved_nursery = NULL;
    f->admission_nursery = NULL;
    atomic_store_explicit(&f->join_waiter_fiber, NULL, memory_order_relaxed);
    wake_primitive_init(&f->done_wake);

    atomic_fetch_add_explicit(&g_v2.fiber_count, 1, memory_order_relaxed);
    V2_STAT_INC(g_v2_fibers_alive);
    pthread_mutex_lock(&g_v2.all_fibers_mu);
    f->all_next = g_v2.all_fibers;
    g_v2.all_fibers = f;
    pthread_mutex_unlock(&g_v2.all_fibers_mu);
    return f;
}

static void fiber_v2_free(fiber_v2* f) {
    V2_STAT_DEC(g_v2_fibers_alive);
    atomic_store_explicit(&f->state, FIBER_V2_IDLE, memory_order_relaxed);
    f->entry_fn = NULL;
    f->entry_arg = NULL;
    f->result = NULL;
    f->saved_nursery = NULL;
    f->admission_nursery = NULL;
    /* Clear detector metadata so the next spawn starts clean and the
     * detector never observes stale park_obj/suppress/external-wait state
     * on a pooled fiber. */
    f->park_obj = NULL;
    f->deadlock_suppress_depth = 0;
    f->external_wait_depth = 0;
    /* Pool the coroutine memory (including its stack): mco_uninit just marks
     * the coro DEAD and runs platform teardown (a no-op on ucontext), while
     * preserving the allocation so the next spawn can re-init in place
     * instead of paying another ~2 MB alloc/free. */
    if (f->coro) {
        (void)mco_uninit(f->coro);
    }
    fiber_v2* head;
    do {
        head = atomic_load_explicit(&g_v2.free_list, memory_order_relaxed);
        f->next = head;
    } while (!atomic_compare_exchange_weak_explicit(&g_v2.free_list, &head, f,
            memory_order_release, memory_order_relaxed));
}

/* ============================================================================
 * Wake infrastructure: try_wake_one, try_expand_pool, sched_v2_wake
 * ============================================================================ */

static void* thread_v2_main(void* arg);
static void sched_v2_wake(int worker_hint);

static inline int sched_v2_finish_join(fiber_v2* f, void** out_result) {
    if (out_result) *out_result = f->result;
    return 0;
}

/* Claim one idle worker thread and wake it. Returns 1 on success. */
static int sched_v2_try_wake_one(void) {
    if (atomic_load_explicit(&g_v2.idle_workers, memory_order_acquire) <= 0) {
        return 0;
    }
    int n = atomic_load_explicit(&g_v2.num_threads, memory_order_acquire);
    for (int i = 0; i < n; i++) {
        int exp = 1;
        if (atomic_compare_exchange_strong_explicit(&g_v2.threads[i].is_idle, &exp, 0,
                memory_order_acq_rel, memory_order_relaxed)) {
            atomic_fetch_sub_explicit(&g_v2.idle_workers, 1, memory_order_acq_rel);
            V2_STAT_INC(g_v2_wake_issued);
            wake_primitive_wake_one(&g_v2.threads[i].wake);
            return 1;
        }
    }
    V2_STAT_INC(g_v2_wake_scan_miss);
    return 0;
}

/* Expand thread pool by one. Returns 1 if a thread was created. */
static int sched_v2_try_expand_pool(void) {
    if (!g_v2.allow_expand) return 0;
    int current = atomic_load_explicit(&g_v2.num_threads, memory_order_acquire);
    if (current >= V2_MAX_THREADS) return 0;
    int new_id = atomic_fetch_add_explicit(&g_v2.num_threads, 1, memory_order_acq_rel);
    if (new_id >= V2_MAX_THREADS) {
        atomic_fetch_sub_explicit(&g_v2.num_threads, 1, memory_order_relaxed);
        return 0;
    }
    g_v2.threads[new_id].id = new_id;
    atomic_store_explicit(&g_v2.threads[new_id].is_idle, 0, memory_order_relaxed);
    wake_primitive_init(&g_v2.threads[new_id].wake);
    atomic_store_explicit(&g_v2.threads[new_id].alive, 0, memory_order_relaxed);
    atomic_store_explicit(&g_v2.threads[new_id].dispatch_start_ns, 0,
                          memory_order_relaxed);
    /* First worker in this slot: generation starts at 0. */
    atomic_store_explicit(&g_v2.threads[new_id].generation, 0,
                          memory_order_release);
    pthread_create(&g_v2.threads[new_id].handle, NULL,
                   thread_v2_main, (void*)(intptr_t)new_id);
    return 1;
}

static void thread_v2_run_fiber(int tid, fiber_v2* f);

static void sched_v2_enqueue_runnable(fiber_v2* f) {
    int prev = v2_queue_push(&g_v2.ready_queue, f);
    /* If the queue was already deep, a drainer is on it (or a previous
     * push just woke one) and will self-drain to our item. Skip the
     * seq_cst fence + idle-worker scan on this push. Correctness is
     * held by:
     *   - the 0->N-1 pushes still wake on their own,
     *   - the parking-worker Dekker recheck (see thread_v2_main) catches
     *     the push->park transition race, and
     *   - sysmon tick issues an unconditional safety-net wake if any
     *     stranded idle worker slips through.
     * Set CC_V2_WAKE_SKIP_DEPTH=0 to restore legacy always-wake. */
    if (g_v2_wake_skip_depth > 0 && prev >= g_v2_wake_skip_depth) {
        V2_STAT_INC(g_v2_wake_skipped_deep);
        return;
    }
    sched_v2_wake(-1);
}

/*
 * Central wake primitive.
 *
 * worker_hint >= 0 (must equal tls_v2_thread_id):
 *   Caller offers itself as a free worker. Drains ready queue inline:
 *   pop fiber, run it, repeat until queue empty.
 *
 * worker_hint < 0:
 *   External caller (kqueue thread, channel code, sysmon).
 *   Wakes idle workers while the ready queue has work.
 */
static void sched_v2_wake(int worker_hint) {
    if (worker_hint >= 0 && worker_hint == tls_v2_thread_id) {
        while (atomic_load_explicit(&g_v2.running, memory_order_acquire)) {
            fiber_v2* f = v2_queue_pop(&g_v2.ready_queue);
            if (!f) return;
            V2_STAT_INC(g_v2_worker_self_drain);
            /* Mark the dispatch so sysmon can detect kidnapped syscalls.
             * Sysmon reads this with relaxed order; the only timing we
             * care about is "approximately how long has this fiber run". */
            atomic_store_explicit(&g_v2.threads[worker_hint].dispatch_start_ns,
                                  v2_now_ns(), memory_order_relaxed);
            thread_v2_run_fiber(worker_hint, f);
            atomic_store_explicit(&g_v2.threads[worker_hint].dispatch_start_ns,
                                  0, memory_order_relaxed);
            /* Identity check: sysmon may have evicted us in place while
             * the fiber was running (see sched_v2_sysmon_evict_aged_workers).
             * If slot.generation has moved beyond our cached value, a new
             * worker now owns this slot — we must not touch slot.wake or
             * slot.is_idle again. Abandon the self-drain; the outer loop
             * will catch the same mismatch and exit cleanly. */
            if (atomic_load_explicit(&g_v2.threads[worker_hint].generation,
                                     memory_order_acquire)
                != tls_v2_my_generation) {
                return;
            }
        }
        return;
    }
    V2_STAT_INC(g_v2_wake_calls_ext);

    /* External: wake IDLE workers while there's work in the queue.
     *
     * We're on the "producer" side of a Dekker-style store-load pair:
     *   producer: push task (count++)     ; load idle_workers
     *   worker:   mark idle (is_idle=1)   ; load queue.count
     *
     * On weakly-ordered architectures (arm64), release-store + acquire-load
     * does NOT forbid the load from being reordered before the store. Without
     * a full barrier between them, both sides can observe the pre-publish
     * state of the other and both go to sleep, leaving tasks stranded.
     *
     * The push already happened under a mutex above; insert a seq_cst fence
     * here before the idle_workers check so the pairing is airtight. */
    atomic_thread_fence(memory_order_seq_cst);

    if (atomic_load_explicit(&g_v2.idle_workers, memory_order_acquire) <= 0) {
        V2_STAT_INC(g_v2_wake_no_idle);
        return;
    }
    while (atomic_load_explicit(&g_v2.ready_queue.count, memory_order_relaxed) > 0 &&
           atomic_load_explicit(&g_v2.idle_workers, memory_order_acquire) > 0) {
        if (!sched_v2_try_wake_one()) {
            sched_v2_try_expand_pool();
            break;
        }
    }
}

/* ============================================================================
 * Run fiber + post-yield commit
 * ============================================================================ */

static void thread_v2_run_fiber(int tid, fiber_v2* f) {
    int expected = fiber_v2_state_base(atomic_load_explicit(&f->state, memory_order_acquire));
    if (expected != FIBER_V2_QUEUED) {
        fprintf(stderr, "[sched_v2] BUG: thread %d got fiber in state %d\n", tid, expected);
        return;
    }
    atomic_store_explicit(&f->state, FIBER_V2_RUNNING, memory_order_release);
    f->last_thread_id = tid;
    tls_v2_current_fiber = f;
    f->park_reason = NULL;
    /* park_obj is intentionally NOT cleared here: cc__fiber_set_park_obj
     * writes it just before the park handshake, and the detector only
     * consults it for fibers currently in state FIBER_V2_PARKED, so
     * stale values on a RUNNING fiber never show up in deadlock reports. */
    f->yield_kind = V2_YIELD_PARK;

    /* Just-in-time coroutine binding.
     *   coro == NULL          -> freshly-allocated fiber_v2, no stack yet.
     *   status == MCO_DEAD    -> pooled fiber whose stack survived via
     *                             mco_uninit in fiber_v2_free; reset it to
     *                             the entry trampoline in place.
     *   status == MCO_SUSPENDED -> fiber that previously parked and is now
     *                             being resumed; nothing to do. */
    if (!f->coro) {
        mco_desc desc = mco_desc_init(fiber_v2_entry, V2_FIBER_STACK_SIZE);
        desc.user_data = f;
        if (mco_create(&f->coro, &desc) != MCO_SUCCESS) {
            fprintf(stderr, "[sched_v2] mco_create failed on worker pickup\n");
            abort();
        }
        V2_STAT_INC(g_v2_coro_fresh);
    } else if (mco_status(f->coro) == MCO_DEAD) {
        mco_desc desc = mco_desc_init(fiber_v2_entry, V2_FIBER_STACK_SIZE);
        desc.user_data = f;
        if (mco_init(f->coro, &desc) != MCO_SUCCESS) {
            /* Pooled memory lost; drop it and alloc fresh. */
            (void)mco_destroy(f->coro);
            f->coro = NULL;
            if (mco_create(&f->coro, &desc) != MCO_SUCCESS) {
                fprintf(stderr, "[sched_v2] mco_create fallback failed on worker pickup\n");
                abort();
            }
            V2_STAT_INC(g_v2_coro_fresh);
        } else {
            V2_STAT_INC(g_v2_coro_reuse);
        }
    }

    mco_result res = mco_resume(f->coro);

    tls_v2_current_fiber = NULL;
    atomic_fetch_add_explicit(&g_v2_sysmon_stall_detect, 1, memory_order_relaxed);
    if (res != MCO_SUCCESS) {
        V2_STAT_INC(g_v2_mco_resume_fail);
        fprintf(stderr, "[sched_v2] mco_resume failed rc=%d\n", (int)res);
        abort();
    }

    if (mco_status(f->coro) == MCO_DEAD) {
        atomic_store_explicit(&f->state, FIBER_V2_DEAD, memory_order_release);
        atomic_store_explicit(&f->done, 1, memory_order_release);
        /* Dekker pair with sched_v2_join waiter: completer stores done then
         * loads join_waiter_fiber; waiter stores join_waiter_fiber then loads
         * done. A release store + acq_rel RMW on a different object is
         * insufficient on ARM64: the store buffer can hide our done=1 from
         * the waiter's load while also hiding the waiter's publish from our
         * RMW, yielding a lost wake. Full fence on both sides forces at
         * least one side to observe the other. */
        atomic_thread_fence(memory_order_seq_cst);
        cc__fiber* waiter =
            atomic_exchange_explicit(&f->join_waiter_fiber, NULL, memory_order_acq_rel);
        if (waiter) {
            cc__fiber_unpark_tagged(waiter, CC_FIBER_UNPARK_REASON_TASK_DONE);
        }
        wake_primitive_wake_all(&f->done_wake);
        V2_STAT_INC(g_v2_run_dead);
        return;
    }

    int raw_state = atomic_load_explicit(&f->state, memory_order_acquire);
    if (f->yield_kind == V2_YIELD_YIELD || fiber_v2_state_has_signal_pending(raw_state)) {
        atomic_store_explicit(&f->state, FIBER_V2_QUEUED, memory_order_release);
        sched_v2_enqueue_runnable(f);
        if (f->yield_kind == V2_YIELD_YIELD) {
            V2_STAT_INC(g_v2_run_yield_requeue);
        } else {
            V2_STAT_INC(g_v2_run_pending_requeue);
        }
        return;
    }

    expected = FIBER_V2_RUNNING;
    if (!atomic_compare_exchange_strong_explicit(&f->state, &expected, FIBER_V2_PARKED,
            memory_order_acq_rel, memory_order_relaxed)) {
        int fail_state = fiber_v2_state_base(expected);
        if (fail_state >= 0 && fail_state < FIBER_V2_STATE_COUNT) {
            V2_STAT_INC(g_v2_run_commit_park_fail_state[fail_state]);
        }
        atomic_store_explicit(&f->state, FIBER_V2_QUEUED, memory_order_release);
        sched_v2_enqueue_runnable(f);
        V2_STAT_INC(g_v2_run_pending_requeue);
        return;
    }
    V2_STAT_INC(g_v2_run_commit_parked);
}

/* ============================================================================
 * Signal: publish runnable work through state only
 * ============================================================================ */

void sched_v2_signal(fiber_v2* f) {
    while (1) {
        int expected = atomic_load_explicit(&f->state, memory_order_acquire);
        int base_state = fiber_v2_state_base(expected);
        if (base_state == FIBER_V2_QUEUED) {
            V2_STAT_INC(g_v2_signal_ok);
            return;
        }
        if (base_state == FIBER_V2_RUNNING) {
            if (fiber_v2_state_has_signal_pending(expected)) {
                V2_STAT_INC(g_v2_signal_running_pending_already_set);
                V2_STAT_INC(g_v2_signal_pending);
                return;
            }
            int desired = expected | FIBER_V2_FLAG_SIGNAL_PENDING;
            if (!atomic_compare_exchange_weak_explicit(&f->state, &expected, desired,
                    memory_order_acq_rel, memory_order_relaxed)) {
                continue;
            }
            V2_STAT_INC(g_v2_signal_running_pending_set);
            V2_STAT_INC(g_v2_signal_pending);
            return;
        }
        if (base_state == FIBER_V2_PARKED) {
            int desired = FIBER_V2_QUEUED;
            if (!atomic_compare_exchange_weak_explicit(&f->state, &expected, desired,
                    memory_order_acq_rel, memory_order_relaxed)) {
                continue;
            }
            V2_STAT_INC(g_v2_signal_ok);
            sched_v2_enqueue_runnable(f);
            return;
        }
        V2_STAT_INC(g_v2_signal_dropped);
        if (base_state >= 0 && base_state < FIBER_V2_STATE_COUNT) {
            V2_STAT_INC(g_v2_signal_dropped_state[base_state]);
        }
        return;
    }
}

/* ============================================================================
 * Park / Yield (unchanged — fiber-side API)
 * ============================================================================ */

void sched_v2_park(void) {
    fiber_v2* f = tls_v2_current_fiber;
    if (!f) return;

    V2_STAT_INC(g_v2_parks);
    f->park_reason = tls_v2_park_reason;
    f->yield_kind = V2_YIELD_PARK;
    mco_result res = mco_yield(f->coro);
    if (res != MCO_SUCCESS) {
        V2_STAT_INC(g_v2_mco_yield_fail);
        fprintf(stderr, "[sched_v2] mco_yield(park) failed rc=%d\n", (int)res);
        abort();
    }
}

void sched_v2_yield(void) {
    fiber_v2* f = tls_v2_current_fiber;
    if (!f) return;

    f->yield_kind = V2_YIELD_YIELD;
    mco_result res = mco_yield(f->coro);
    if (res != MCO_SUCCESS) {
        V2_STAT_INC(g_v2_mco_yield_fail);
        fprintf(stderr, "[sched_v2] mco_yield(yield) failed rc=%d\n", (int)res);
        abort();
    }
}

void sched_v2_set_park_reason(const char* reason) {
    tls_v2_park_reason = reason;
}

/* ============================================================================
 * Deadlock-detector metadata setters (per-fiber state)
 *
 * Writes happen from a V2 worker thread while the fiber is RUNNING (i.e.
 * the scope-enter/leave call itself executes on the fiber's stack); reads
 * happen from the sysmon thread while the fiber is PARKED. We serialize
 * via g_v2.all_fibers_mu in the reader (sched_v2_check_deadlock), and the
 * writes on this side are plain stores to fields the writer owns. A
 * relaxed atomic isn't required here: the scope enter/leave always
 * precedes the park handshake, whose own release/acquire publishes the
 * new depth alongside state=FIBER_V2_PARKED. sysmon acquires
 * all_fibers_mu, which synchronizes-with the fiber pool-add release in
 * fiber_v2_alloc, and re-reads state with memory_order_acquire, which
 * pairs with the park-commit release. The depth therefore lands in the
 * reader's view before it treats the fiber as parked.
 * ============================================================================ */

void sched_v2_fiber_set_park_obj(fiber_v2* f, void* obj) {
    if (!f) return;
    f->park_obj = obj;
}

void sched_v2_fiber_inc_deadlock_suppress(fiber_v2* f) {
    if (!f) return;
    if (f->deadlock_suppress_depth < UINT32_MAX) f->deadlock_suppress_depth++;
}

void sched_v2_fiber_dec_deadlock_suppress(fiber_v2* f) {
    if (!f) return;
    if (f->deadlock_suppress_depth > 0) f->deadlock_suppress_depth--;
}

int sched_v2_fiber_deadlock_suppressed(fiber_v2* f) {
    if (!f) return 0;
    return f->deadlock_suppress_depth > 0;
}

void sched_v2_fiber_inc_external_wait(fiber_v2* f) {
    if (!f) return;
    if (f->external_wait_depth < UINT32_MAX) f->external_wait_depth++;
}

void sched_v2_fiber_dec_external_wait(fiber_v2* f) {
    if (!f) return;
    if (f->external_wait_depth > 0) f->external_wait_depth--;
}

int sched_v2_fiber_external_wait_active(fiber_v2* f) {
    if (!f) return 0;
    return f->external_wait_depth > 0;
}

/* ============================================================================
 * Context queries
 * ============================================================================ */

int sched_v2_in_context(void) {
    return tls_v2_current_fiber != NULL;
}

fiber_v2* sched_v2_current_fiber(void) {
    return tls_v2_current_fiber;
}

int sched_v2_current_worker_id(void) {
    return tls_v2_thread_id;
}

/* ============================================================================
 * Thread main loop
 * ============================================================================ */

static void* thread_v2_main(void* arg) {
    int tid = (int)(intptr_t)arg;
    tls_v2_thread_id = tid;
    /* Cache our slot generation once at entry. Any future mismatch means
     * sysmon has evicted us and installed a replacement — we exit without
     * touching slot.wake or slot.is_idle (those now belong to the new
     * worker). See pthread_create synchronize-with guarantee: anything
     * sysmon wrote before pthread_create is visible to us here. */
    tls_v2_my_generation = atomic_load_explicit(&g_v2.threads[tid].generation,
                                                memory_order_acquire);
    atomic_store_explicit(&g_v2.threads[tid].alive, 1, memory_order_release);

    while (atomic_load_explicit(&g_v2.running, memory_order_acquire)) {
        /* Drain: run ready fibers inline until queue is empty (or sysmon
         * evicted us, in which case sched_v2_wake returns early). */
        sched_v2_wake(tid);

        if (!atomic_load_explicit(&g_v2.running, memory_order_acquire)) break;

        /* End-of-loop identity check: if slot.generation has moved, we are
         * an orphan. Exit before re-parking on slot.wake (which now belongs
         * to the replacement worker). Our kidnapped fiber has already
         * returned by now; the OS thread simply dies. */
        if (atomic_load_explicit(&g_v2.threads[tid].generation,
                                 memory_order_acquire)
            != tls_v2_my_generation) {
            break;
        }

        /* No work — let this BUSY worker become IDLE.
         * Dekker protocol: mark idle, read wake counter, recheck, sleep.
         *
         * The store-load pair (mark is_idle=1 ; load queue.count) needs a
         * full seq_cst fence on arm64 — release-store + acquire-load alone
         * allows the architecture to reorder the load before the store,
         * which would race with the producer's symmetric "push count ; load
         * idle_workers" and leave a task stranded while the worker sleeps.
         * See sched_v2_wake for the matching fence on the producer side. */
        atomic_store_explicit(&g_v2.threads[tid].is_idle, 1, memory_order_release);
        atomic_fetch_add_explicit(&g_v2.idle_workers, 1, memory_order_acq_rel);
        V2_STAT_INC(g_v2_worker_idle_entries);
        atomic_thread_fence(memory_order_seq_cst);
        uint32_t val = atomic_load_explicit(&g_v2.threads[tid].wake.value,
                                            memory_order_acquire);

        /* Recheck: work appeared after we marked ourselves idle. */
        if (atomic_load_explicit(&g_v2.ready_queue.count, memory_order_acquire) > 0) {
            if (atomic_exchange_explicit(&g_v2.threads[tid].is_idle, 0, memory_order_acq_rel)) {
                atomic_fetch_sub_explicit(&g_v2.idle_workers, 1, memory_order_acq_rel);
            }
            V2_STAT_INC(g_v2_worker_busy_from_recheck);
            continue;
        }

        wake_primitive_wait(&g_v2.threads[tid].wake, val);
        if (atomic_exchange_explicit(&g_v2.threads[tid].is_idle, 0, memory_order_acq_rel)) {
            atomic_fetch_sub_explicit(&g_v2.idle_workers, 1, memory_order_acq_rel);
        }
        if (atomic_load_explicit(&g_v2.ready_queue.count, memory_order_acquire) > 0) {
            V2_STAT_INC(g_v2_worker_busy_from_wake);
        }
    }

    /* If we left because slot.generation moved, we are an orphan. The
     * living_alive counter for the slot now belongs to the replacement
     * worker (which set its own alive=1 at entry), so we must NOT clear
     * alive here — that would racily overwrite the new worker's flag.
     * Only clean-exit (running=0) workers clear alive.
     *
     * We always decrement g_v2_orphans_alive if our cached generation is
     * stale (which is also the only way to reach here without running=0). */
    int orphaned = (atomic_load_explicit(&g_v2.threads[tid].generation,
                                         memory_order_acquire)
                    != tls_v2_my_generation);
    if (orphaned) {
        atomic_fetch_sub_explicit(&g_v2_orphans_alive, 1, memory_order_relaxed);
    } else {
        atomic_store_explicit(&g_v2.threads[tid].alive, 0, memory_order_release);
    }
    return NULL;
}

/* ============================================================================
 * Sysmon
 * ============================================================================ */

static int sched_v2_detect_num_threads(void) {
    const char* env = getenv("CC_V2_THREADS");
    if (env && env[0]) {
        int n = atoi(env);
        if (n > 0 && n <= V2_MAX_THREADS) return n;
    }
    /* Honor CC_WORKERS as a fallback for tests/tools written against the
     * V1 knob. Under V2-default the worker count primarily affects sysmon
     * stall detection cadence (a single V2 worker still multiplexes many
     * fibers), but tests that set CC_WORKERS=1 for deterministic
     * deadlock-detection timing expect that hint to take effect. */
    env = getenv("CC_WORKERS");
    if (env && env[0]) {
        int n = atoi(env);
        if (n > 0 && n <= V2_MAX_THREADS) return n;
    }
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    if (n < 1) n = 4;
    /* Default to 1x CPU count, matching the cc_thread_spawn executor pool
     * (scheduler.c: cc__default_workers). CPU-bound workloads (pigz level 6
     * deflate) are fully saturated at one worker per core; pushing to 2x
     * adds context-switch pressure from oversubscription with the classic
     * scheduler's own workers and measurably hurts throughput (~5% on
     * pigz 100MB silesia). For I/O-bound or mixed workloads where parked
     * V2 workers can hide latency, raise via CC_V2_THREADS.
     *
     * Hard-capped at V2_MAX_THREADS. */
    if (n > V2_MAX_THREADS) n = V2_MAX_THREADS;
    return (int)n;
}

static int sched_v2_count_idle_workers(void) {
    int n = atomic_load_explicit(&g_v2.num_threads, memory_order_relaxed);
    int idle = 0;
    for (int i = 0; i < n; i++) {
        if (atomic_load_explicit(&g_v2.threads[i].is_idle, memory_order_relaxed))
            idle++;
    }
    return idle;
}

/* Evict an aged worker from slot `i` and install a fresh replacement
 * thread into the same slot. The old worker becomes an orphan: it keeps
 * running its kidnapped fiber in the kernel syscall, then, on return to
 * the worker loop, observes that slot.generation has moved past its
 * cached value and exits. pthread_detach hands its stack to the OS for
 * self-reclaim at exit — nobody joins orphans.
 *
 * Ordering rationale (all observable by the new thread via the
 * pthread_create synchronize-with guarantee):
 *   1. detach old handle (once; handle field about to be overwritten).
 *   2. reset per-slot transient state (wake, is_idle, dispatch_start_ns).
 *   3. bump slot.generation (the identity token the new thread will read
 *      at entry into tls_v2_my_generation).
 *   4. pthread_create(&slot.handle, ..., tid) — writes new handle AND
 *      releases all prior stores to the new thread.
 */
static void sched_v2_sysmon_evict_worker_in_place(int i) {
    pthread_t old_handle = g_v2.threads[i].handle;
    pthread_detach(old_handle);

    /* The orphan is mid-syscall, not parked on slot.wake (dispatch_start_ns
     * being nonzero is what qualified it for eviction). Resetting wake is
     * safe and gives the new worker a fresh counter. */
    wake_primitive_init(&g_v2.threads[i].wake);
    atomic_store_explicit(&g_v2.threads[i].is_idle, 0, memory_order_relaxed);
    atomic_store_explicit(&g_v2.threads[i].dispatch_start_ns, 0,
                          memory_order_relaxed);
    /* The new thread will set alive=1 at entry; leaving it at 1 here is
     * intentional — the slot is never "not alive" across an eviction. */

    atomic_fetch_add_explicit(&g_v2.threads[i].generation, 1,
                              memory_order_release);

    atomic_fetch_add_explicit(&g_v2_orphans_alive, 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&g_v2_sysmon_evicted_total, 1,
                              memory_order_relaxed);

    pthread_create(&g_v2.threads[i].handle, NULL,
                   thread_v2_main, (void*)(intptr_t)i);
}

/* Scan all active workers for ones stuck in a kidnapped fiber. Evict in
 * place — but only as many as we actually need to drain the current
 * ready-queue backlog.
 *
 * The per-tick budget (= backlog) is critical: without it, sysmon would
 * repeatedly evict every aged worker on every tick (including freshly
 * promoted ones once their own long fiber crossed the age threshold),
 * cascading orphans without bound while no queued work actually runs. With
 * the budget: we only over-commit enough threads to make forward progress
 * on queued work. A long CPU-bound fiber with an empty queue is never
 * evicted (nobody is waiting on that worker anyway).
 *
 * V2_ORPHAN_SAFETY_CAP is a pure safety net, not a policy knob. If the app
 * ever spawns enough simultaneous blocking fibers to bump against it, it
 * is itself pathological — sysmon simply waits one tick. */
static void sched_v2_sysmon_evict_aged_workers(void) {
    if (!atomic_load_explicit(&g_v2_sysmon_detach_enabled,
                              memory_order_relaxed)) {
        return;
    }
    size_t backlog = atomic_load_explicit(&g_v2.ready_queue.count,
                                          memory_order_relaxed);
    if (backlog == 0) return;

    uint64_t now = v2_now_ns();
    int n = atomic_load_explicit(&g_v2.num_threads, memory_order_acquire);
    size_t budget = backlog;
    for (int i = 0; i < n && budget > 0; i++) {
        uint64_t t0 = atomic_load_explicit(&g_v2.threads[i].dispatch_start_ns,
                                           memory_order_relaxed);
        if (t0 == 0) continue;
        if (now - t0 < V2_SYSMON_SYSCALL_AGE_NS) continue;

        int64_t live_orphans = atomic_load_explicit(&g_v2_orphans_alive,
                                                    memory_order_relaxed);
        if (live_orphans >= V2_ORPHAN_SAFETY_CAP) {
            atomic_fetch_add_explicit(&g_v2_orphans_cap_hit, 1,
                                      memory_order_relaxed);
            break;
        }

        sched_v2_sysmon_evict_worker_in_place(i);
        budget--;
    }
}

static void* sched_v2_sysmon_main(void* arg) {
    (void)arg;
    uint64_t last_run_count = 0;
    int stall_ticks = 0;
    /* Stall diagnostic cadence scaled to the (now faster) tick interval:
     * fire at ~2 s of genuine stall, print every ~2 s thereafter. */
    const int STALL_DIAG_TICKS = 2000 / V2_SYSMON_INTERVAL_MS;
    while (atomic_load_explicit(&g_v2.running, memory_order_acquire)) {
        uint32_t val = atomic_load_explicit(&g_v2.sysmon_wake.value, memory_order_acquire);
        wake_primitive_wait_timeout(&g_v2.sysmon_wake, val, V2_SYSMON_INTERVAL_MS);

        if (!atomic_load_explicit(&g_v2.running, memory_order_acquire)) break;

        /* Syscall-age eviction runs every tick: cheap scan, high payoff. */
        sched_v2_sysmon_evict_aged_workers();

        /* Deadlock detector: runs every tick. Internal checks (idle
         * count + ready queue depth + first-seen timestamp) short-circuit
         * the all_fibers walk when the system is healthy, so the
         * amortized cost on the hot path is just two atomic loads. */
        sched_v2_check_deadlock();

        /* Unconditional every-tick safety net: if any work is queued AND
         * any worker is idle, issue a wake. sched_v2_wake(-1) itself is
         * cheap when there's no idle worker (short-circuits on the
         * idle_workers==0 check). This closes the one correctness gap
         * left open by CC_V2_WAKE_SKIP_DEPTH: a push that lands while
         * some worker is parked and the queue was already deep won't
         * wake anyone, but this tick will, bounded to ~V2_SYSMON_INTERVAL_MS
         * (20ms). Producer-exceeds-drainer bursts self-heal here. */
        if (atomic_load_explicit(&g_v2.ready_queue.count, memory_order_relaxed) > 0 &&
            atomic_load_explicit(&g_v2.idle_workers, memory_order_relaxed) > 0) {
            sched_v2_wake(-1);
        }

        uint64_t cur_run = atomic_load_explicit(&g_v2_sysmon_stall_detect, memory_order_relaxed);
        if (cur_run == last_run_count) {
            stall_ticks++;

            if (stall_ticks >= STALL_DIAG_TICKS && (stall_ticks % STALL_DIAG_TICKS) == 0) {
                int n = atomic_load_explicit(&g_v2.num_threads, memory_order_relaxed);
                int idle_n = sched_v2_count_idle_workers();
                size_t gq = atomic_load_explicit(&g_v2.ready_queue.count, memory_order_relaxed);
                uint64_t alive = atomic_load_explicit(&g_v2_fibers_alive, memory_order_relaxed);
                uint64_t sig_ok = atomic_load_explicit(&g_v2_signal_ok, memory_order_relaxed);
                uint64_t sig_pend = atomic_load_explicit(&g_v2_signal_pending, memory_order_relaxed);
                uint64_t sig_run_set = atomic_load_explicit(&g_v2_signal_running_pending_set, memory_order_relaxed);
                uint64_t sig_run_already = atomic_load_explicit(&g_v2_signal_running_pending_already_set, memory_order_relaxed);
                uint64_t sig_drop = atomic_load_explicit(&g_v2_signal_dropped, memory_order_relaxed);
                uint64_t parks = atomic_load_explicit(&g_v2_parks, memory_order_relaxed);
                uint64_t run_yield = atomic_load_explicit(&g_v2_run_yield_requeue, memory_order_relaxed);
                uint64_t run_dead = atomic_load_explicit(&g_v2_run_dead, memory_order_relaxed);
                uint64_t run_parked = atomic_load_explicit(&g_v2_run_commit_parked, memory_order_relaxed);
                uint64_t run_pending = atomic_load_explicit(&g_v2_run_pending_requeue, memory_order_relaxed);
                uint64_t worker_idle_entries = atomic_load_explicit(&g_v2_worker_idle_entries, memory_order_relaxed);
                uint64_t worker_busy_recheck = atomic_load_explicit(&g_v2_worker_busy_from_recheck, memory_order_relaxed);
                uint64_t worker_busy_wake = atomic_load_explicit(&g_v2_worker_busy_from_wake, memory_order_relaxed);
                uint64_t state_counts[FIBER_V2_STATE_COUNT];
                uint64_t parked_wait_many = 0;
                uint64_t parked_wait = 0;
                uint64_t parked_other = 0;
                uint64_t parked_unknown = 0;
                park_reason_bucket reason_buckets[V2_DIAG_REASON_BUCKETS];
                size_t reason_bucket_count = 0;
                sched_v2_diag_scan_fibers(state_counts,
                                          &parked_wait_many,
                                          &parked_wait,
                                          &parked_other,
                                          &parked_unknown,
                                          reason_buckets,
                                          &reason_bucket_count);
                fprintf(stderr, "[sched_v2 sysmon] STALL #%d: threads=%d idle=%d global_q=%zu fibers_alive=%llu\n",
                        stall_ticks / STALL_DIAG_TICKS, n, idle_n, gq, (unsigned long long)alive);
                fprintf(stderr, "  signals: ok=%llu pending=%llu dropped=%llu  parks: total=%llu\n",
                        (unsigned long long)sig_ok, (unsigned long long)sig_pend, (unsigned long long)sig_drop,
                        (unsigned long long)parks);
                fprintf(stderr, "  workers: idle_entries=%llu busy_from_recheck=%llu busy_from_wake=%llu\n",
                        (unsigned long long)worker_idle_entries,
                        (unsigned long long)worker_busy_recheck,
                        (unsigned long long)worker_busy_wake);
                fprintf(stderr, "  wake path: mark_pending=%llu already_pending=%llu  park(commit=%llu pending_to_queue=%llu) exits(dead=%llu yield=%llu) park_fail: IDLE=%llu QUEUED=%llu RUNNING=%llu PARKED=%llu DEAD=%llu\n",
                        (unsigned long long)sig_run_set,
                        (unsigned long long)sig_run_already,
                        (unsigned long long)run_parked,
                        (unsigned long long)run_pending,
                        (unsigned long long)run_dead,
                        (unsigned long long)run_yield,
                        (unsigned long long)atomic_load_explicit(&g_v2_run_commit_park_fail_state[0], memory_order_relaxed),
                        (unsigned long long)atomic_load_explicit(&g_v2_run_commit_park_fail_state[1], memory_order_relaxed),
                        (unsigned long long)atomic_load_explicit(&g_v2_run_commit_park_fail_state[2], memory_order_relaxed),
                        (unsigned long long)atomic_load_explicit(&g_v2_run_commit_park_fail_state[3], memory_order_relaxed),
                        (unsigned long long)atomic_load_explicit(&g_v2_run_commit_park_fail_state[4], memory_order_relaxed));
                fprintf(stderr, "  minicoro: resume_fail=%llu yield_fail=%llu stack_overflow=%llu\n",
                        (unsigned long long)atomic_load_explicit(&g_v2_mco_resume_fail, memory_order_relaxed),
                        (unsigned long long)atomic_load_explicit(&g_v2_mco_yield_fail, memory_order_relaxed),
                        (unsigned long long)atomic_load_explicit(&g_v2_mco_stack_overflow, memory_order_relaxed));
                fprintf(stderr, "  coro pool: reuse=%llu fresh=%llu\n",
                        (unsigned long long)atomic_load_explicit(&g_v2_coro_reuse, memory_order_relaxed),
                        (unsigned long long)atomic_load_explicit(&g_v2_coro_fresh, memory_order_relaxed));
                fprintf(stderr, "  drop by state: IDLE=%llu QUEUED=%llu RUNNING=%llu PARKED=%llu DEAD=%llu\n",
                        (unsigned long long)atomic_load_explicit(&g_v2_signal_dropped_state[0], memory_order_relaxed),
                        (unsigned long long)atomic_load_explicit(&g_v2_signal_dropped_state[1], memory_order_relaxed),
                        (unsigned long long)atomic_load_explicit(&g_v2_signal_dropped_state[2], memory_order_relaxed),
                        (unsigned long long)atomic_load_explicit(&g_v2_signal_dropped_state[3], memory_order_relaxed),
                        (unsigned long long)atomic_load_explicit(&g_v2_signal_dropped_state[4], memory_order_relaxed));
                fprintf(stderr, "  fiber scan: state(IDLE=%llu QUEUED=%llu RUNNING=%llu PARKED=%llu DEAD=%llu)\n",
                        (unsigned long long)state_counts[0],
                        (unsigned long long)state_counts[1],
                        (unsigned long long)state_counts[2],
                        (unsigned long long)state_counts[3],
                        (unsigned long long)state_counts[4]);
                fprintf(stderr, "  parked reasons: wait_many=%llu wait=%llu other=%llu unknown=%llu\n",
                        (unsigned long long)parked_wait_many,
                        (unsigned long long)parked_wait,
                        (unsigned long long)parked_other,
                        (unsigned long long)parked_unknown);
                if (reason_bucket_count > 0) {
                    fprintf(stderr, "  park reason histogram:");
                    for (size_t i = 0; i < reason_bucket_count; ++i) {
                        fprintf(stderr, " [%s]=%llu",
                                reason_buckets[i].reason,
                                (unsigned long long)reason_buckets[i].count);
                    }
                    fprintf(stderr, "\n");
                }
                cc__socket_wait_dump_diag();
                cc__fiber_dump_unpark_reason_stats();
                cc_sched_wait_many_dump_diag();
                cc__io_wait_dump_kq_diag();
            }
        } else {
            stall_ticks = 0;
            last_run_count = cur_run;
        }
    }
    return NULL;
}

/* ============================================================================
 * Init / shutdown
 * ============================================================================ */

static void sched_v2_atexit_dump_stats(void) {
    fprintf(stderr, "[sched_v2 stats] coro_pool: reuse=%llu fresh=%llu  "
                    "spawns: parks=%llu dead=%llu\n",
            (unsigned long long)atomic_load_explicit(&g_v2_coro_reuse, memory_order_relaxed),
            (unsigned long long)atomic_load_explicit(&g_v2_coro_fresh, memory_order_relaxed),
            (unsigned long long)atomic_load_explicit(&g_v2_parks, memory_order_relaxed),
            (unsigned long long)atomic_load_explicit(&g_v2_run_dead, memory_order_relaxed));
    fprintf(stderr, "[sched_v2 stats] join (spin=%d): fast=%llu spin_hit=%llu "
                    "park_fiber=%llu park_thread=%llu\n",
            g_v2_join_spin,
            (unsigned long long)atomic_load_explicit(&g_v2_join_fast, memory_order_relaxed),
            (unsigned long long)atomic_load_explicit(&g_v2_join_spin_hit, memory_order_relaxed),
            (unsigned long long)atomic_load_explicit(&g_v2_join_park_fiber, memory_order_relaxed),
            (unsigned long long)atomic_load_explicit(&g_v2_join_park_thread, memory_order_relaxed));
    fprintf(stderr, "[sched_v2 stats] wake: ext_calls=%llu no_idle=%llu issued=%llu scan_miss=%llu  "
                    "skipped_deep=%llu (depth>=%d)  "
                    "worker_self_drain=%llu  worker_idle_entries=%llu (recheck=%llu from_wake=%llu)\n",
            (unsigned long long)atomic_load_explicit(&g_v2_wake_calls_ext, memory_order_relaxed),
            (unsigned long long)atomic_load_explicit(&g_v2_wake_no_idle, memory_order_relaxed),
            (unsigned long long)atomic_load_explicit(&g_v2_wake_issued, memory_order_relaxed),
            (unsigned long long)atomic_load_explicit(&g_v2_wake_scan_miss, memory_order_relaxed),
            (unsigned long long)atomic_load_explicit(&g_v2_wake_skipped_deep, memory_order_relaxed),
            g_v2_wake_skip_depth,
            (unsigned long long)atomic_load_explicit(&g_v2_worker_self_drain, memory_order_relaxed),
            (unsigned long long)atomic_load_explicit(&g_v2_worker_idle_entries, memory_order_relaxed),
            (unsigned long long)atomic_load_explicit(&g_v2_worker_busy_from_recheck, memory_order_relaxed),
            (unsigned long long)atomic_load_explicit(&g_v2_worker_busy_from_wake, memory_order_relaxed));
    fprintf(stderr, "[sched_v2 stats] sysmon_evict: evicted_total=%llu orphans_alive=%lld cap_hit=%llu\n",
            (unsigned long long)atomic_load_explicit(&g_v2_sysmon_evicted_total, memory_order_relaxed),
            (long long)atomic_load_explicit(&g_v2_orphans_alive, memory_order_relaxed),
            (unsigned long long)atomic_load_explicit(&g_v2_orphans_cap_hit, memory_order_relaxed));
}

static void sched_v2_init_impl(void) {
    atomic_store_explicit(&g_v2.running, 1, memory_order_release);
    atomic_store_explicit(&g_v2.idle_workers, 0, memory_order_relaxed);
    v2_queue_init(&g_v2.ready_queue);
    pthread_mutex_init(&g_v2.all_fibers_mu, NULL);
    g_v2.all_fibers = NULL;
    wake_primitive_init(&g_v2.sysmon_wake);

    const char* stats_env = getenv("CC_V2_STATS");
    if (stats_env && stats_env[0] && stats_env[0] != '0') {
        atomic_store_explicit(&g_v2_stats_enabled, 1, memory_order_relaxed);
        atexit(sched_v2_atexit_dump_stats);
    }
    const char* detach_env = getenv("CC_V2_SYSMON_DETACH");
    if (detach_env && detach_env[0] == '0') {
        atomic_store_explicit(&g_v2_sysmon_detach_enabled, 0,
                              memory_order_relaxed);
    }
    /* The periodic sysmon printer also needs the counters populated. */
    const char* sysmon_stats_env = getenv("CC_V2_SYSMON_STATS");
    if (sysmon_stats_env && sysmon_stats_env[0] == '1') {
        atomic_store_explicit(&g_v2_stats_enabled, 1, memory_order_relaxed);
    }
    const char* spin_env = getenv("CC_V2_JOIN_SPIN");
    if (spin_env) {
        char* end = NULL;
        long v = strtol(spin_env, &end, 10);
        if (end != spin_env && v >= 0 && v <= 65536) {
            g_v2_join_spin = (int)v;
        }
    }
    const char* wsd_env = getenv("CC_V2_WAKE_SKIP_DEPTH");
    if (wsd_env) {
        char* end = NULL;
        long v = strtol(wsd_env, &end, 10);
        if (end != wsd_env && v >= 0 && v <= 65536) {
            g_v2_wake_skip_depth = (int)v;
        }
    }

    int n = sched_v2_detect_num_threads();
    atomic_store_explicit(&g_v2.num_threads, n, memory_order_release);
    /* Keep the default pool fixed for now. The current expansion heuristic
     * overreacts on I/O-heavy workloads and recreates the worker-thrash we are
     * trying to avoid. */
    g_v2.allow_expand = 0;

    for (int i = 0; i < n; i++) {
        g_v2.threads[i].id = i;
        atomic_store_explicit(&g_v2.threads[i].is_idle, 0, memory_order_relaxed);
        atomic_store_explicit(&g_v2.threads[i].alive, 0, memory_order_relaxed);
        atomic_store_explicit(&g_v2.threads[i].dispatch_start_ns, 0,
                              memory_order_relaxed);
        /* Initial worker in this slot: generation 0. First eviction will
         * bump it to 1; the cached tls_v2_my_generation in the orphan
         * stays at 0 and the mismatch triggers exit. */
        atomic_store_explicit(&g_v2.threads[i].generation, 0,
                              memory_order_release);
        wake_primitive_init(&g_v2.threads[i].wake);
        pthread_create(&g_v2.threads[i].handle, NULL,
                       thread_v2_main, (void*)(intptr_t)i);
    }

    pthread_create(&g_v2.sysmon_handle, NULL, sched_v2_sysmon_main, NULL);
    g_v2.initialized = 1;
}

void sched_v2_ensure_init(void) {
    pthread_once(&g_v2.init_once, sched_v2_init_impl);
}

void sched_v2_shutdown(void) {
    if (!g_v2.initialized) return;
    atomic_store_explicit(&g_v2.running, 0, memory_order_release);

    int n = atomic_load_explicit(&g_v2.num_threads, memory_order_relaxed);
    for (int i = 0; i < n; i++) {
        wake_primitive_wake_one(&g_v2.threads[i].wake);
    }
    wake_primitive_wake_one(&g_v2.sysmon_wake);

    for (int i = 0; i < n; i++) {
        /* slot.handle always points to the CURRENT worker in this slot.
         * Evicted orphans were pthread_detached at eviction time and are
         * no longer reachable here — they self-reclaim when their
         * kidnapped syscall eventually returns (or when the process
         * exits, whichever comes first). */
        pthread_join(g_v2.threads[i].handle, NULL);
    }
    pthread_join(g_v2.sysmon_handle, NULL);

    fiber_v2* f = g_v2.all_fibers;
    while (f) {
        fiber_v2* next = f->all_next;
        wake_primitive_destroy(&f->done_wake);
        if (f->coro) {
            (void)mco_destroy(f->coro);
            f->coro = NULL;
        }
        free(f);
        f = next;
    }
    atomic_store_explicit(&g_v2.free_list, NULL, memory_order_relaxed);
    g_v2.all_fibers = NULL;
    g_v2.initialized = 0;
}

/* ============================================================================
 * Accessors
 * ============================================================================ */

int sched_v2_fiber_done(fiber_v2* f) {
    return f && atomic_load_explicit(&f->done, memory_order_acquire);
}

void* sched_v2_fiber_result(fiber_v2* f) {
    return f ? f->result : NULL;
}

char* sched_v2_fiber_result_buf(fiber_v2* f) {
    return f ? f->result_buf : NULL;
}

void sched_v2_fiber_release(fiber_v2* f) {
    if (f) fiber_v2_free(f);
}

void* sched_v2_current_result_buf(size_t size) {
    fiber_v2* f = tls_v2_current_fiber;
    if (!f || size > sizeof(f->result_buf)) return NULL;
    return f->result_buf;
}

static const char* fiber_v2_state_name(int base) {
    switch (base) {
        case FIBER_V2_IDLE:    return "IDLE";
        case FIBER_V2_QUEUED:  return "QUEUED";
        case FIBER_V2_RUNNING: return "RUNNING";
        case FIBER_V2_PARKED:  return "PARKED";
        case FIBER_V2_DEAD:    return "DEAD";
        default:               return "?";
    }
}

void sched_v2_debug_dump_fiber(fiber_v2* f, const char* prefix) {
    if (!prefix) prefix = "    ";
    if (!f) {
        fprintf(stderr, "%sv2_fiber=NULL\n", prefix);
        return;
    }
    int raw = atomic_load_explicit(&f->state, memory_order_acquire);
    int done = atomic_load_explicit(&f->done, memory_order_acquire);
    cc__fiber* waiter = atomic_load_explicit(&f->join_waiter_fiber, memory_order_acquire);
    int base = fiber_v2_state_base(raw);
    const char* reason = f->park_reason ? f->park_reason : "-";
    fprintf(stderr,
            "%sv2_fiber=%p state=%s(0x%x) done=%d waiter=%p last_thread=%d reason=%s\n",
            prefix, (void*)f, fiber_v2_state_name(base), (unsigned)raw, done,
            (void*)waiter, f->last_thread_id, reason);
}

void sched_v2_debug_dump_state(const char* prefix) {
    if (!prefix) prefix = "  ";
    if (!g_v2.initialized) {
        fprintf(stderr, "%sv2 sched: uninitialized\n", prefix);
        return;
    }
    int n = atomic_load_explicit(&g_v2.num_threads, memory_order_acquire);
    int idle = atomic_load_explicit(&g_v2.idle_workers, memory_order_acquire);
    size_t rq = atomic_load_explicit(&g_v2.ready_queue.count, memory_order_relaxed);
    size_t alive = atomic_load_explicit(&g_v2.fiber_count, memory_order_relaxed);
    uint64_t stall_before = atomic_load_explicit(&g_v2_sysmon_stall_detect, memory_order_relaxed);
    uint64_t ready_ok = atomic_load_explicit(&g_v2_run_dead, memory_order_relaxed);
    fprintf(stderr,
            "%sv2 sched: threads=%d idle=%d ready_queue=%zu alive_fibers=%zu stall_detect=%llu dead_total=%llu\n",
            prefix, n, idle, rq, alive,
            (unsigned long long)stall_before,
            (unsigned long long)ready_ok);
    fflush(stderr);
    struct timespec ts = {0, 200 * 1000 * 1000}; /* 200 ms */
    nanosleep(&ts, NULL);
    uint64_t stall_after = atomic_load_explicit(&g_v2_sysmon_stall_detect, memory_order_relaxed);
    uint64_t ready_ok2 = atomic_load_explicit(&g_v2_run_dead, memory_order_relaxed);
    fprintf(stderr,
            "%sv2 sched (after 200ms): stall_detect=%llu (delta=%llu) dead_total=%llu (delta=%llu)\n",
            prefix,
            (unsigned long long)stall_after,
            (unsigned long long)(stall_after - stall_before),
            (unsigned long long)ready_ok2,
            (unsigned long long)(ready_ok2 - ready_ok));
    fflush(stderr);
    pthread_mutex_lock(&g_v2.all_fibers_mu);
    size_t count_by_state[FIBER_V2_STATE_COUNT] = {0};
    size_t total = 0;
    for (fiber_v2* f = g_v2.all_fibers; f; f = f->all_next) {
        int base = fiber_v2_state_base(atomic_load_explicit(&f->state, memory_order_acquire));
        if (base >= 0 && base < FIBER_V2_STATE_COUNT) count_by_state[base]++;
        total++;
    }
    fprintf(stderr,
            "%sv2 fibers: total=%zu idle=%zu queued=%zu running=%zu parked=%zu dead=%zu\n",
            prefix, total,
            count_by_state[FIBER_V2_IDLE],
            count_by_state[FIBER_V2_QUEUED],
            count_by_state[FIBER_V2_RUNNING],
            count_by_state[FIBER_V2_PARKED],
            count_by_state[FIBER_V2_DEAD]);
    size_t shown = 0;
    for (fiber_v2* f = g_v2.all_fibers; f; f = f->all_next) {
        int raw = atomic_load_explicit(&f->state, memory_order_acquire);
        int base = fiber_v2_state_base(raw);
        if (base == FIBER_V2_DEAD || base == FIBER_V2_IDLE) continue;
        if (shown++ >= 24) break;
        int done = atomic_load_explicit(&f->done, memory_order_acquire);
        cc__fiber* waiter = atomic_load_explicit(&f->join_waiter_fiber, memory_order_acquire);
        fprintf(stderr,
                "%s  v2_fiber=%p state=%s(0x%x) done=%d waiter=%p last_thread=%d reason=%s\n",
                prefix, (void*)f, fiber_v2_state_name(base), (unsigned)raw, done,
                (void*)waiter, f->last_thread_id, f->park_reason ? f->park_reason : "-");
    }
    pthread_mutex_unlock(&g_v2.all_fibers_mu);
    fflush(stderr);
}

/* ============================================================================
 * Deadlock detector
 *
 * Port of the V1 detector (cc__fiber_check_deadlock in fiber_sched.c) to
 * walk V2 fibers. Called from sysmon every tick. Preserves V1's public
 * contract:
 *   - Abort with _exit(124) when all V2 workers are idle, the ready queue
 *     is empty, and there is at least one V2 fiber parked in a state that
 *     is not deadlock-suppressed and not in an external-wait scope.
 *   - Don't abort if every internally-parked fiber is waiting to receive
 *     on an *open* channel AND some external-wait thread is providing
 *     progress (this is the "I/O-driven progress" exemption V1 had).
 *   - Require the condition to persist for ~1 s before declaring deadlock.
 *   - Opt out via CC_DEADLOCK_ABORT=0 (print banner, keep running).
 *
 * The idle/queue checks deliberately use sysmon's own snapshot of the V2
 * scheduler, not a V1 sleeping/blocked count. V1's "workers blocked in
 * cc_block_on" signal isn't relevant here: V2's wake primitive doesn't
 * consume worker slots the way V1's cond-var waits did, so "all workers
 * IDLE + empty ready queue" is an equivalent latch.
 * ============================================================================ */

extern _Atomic size_t g_external_wait_threads;
int cc__chan_debug_is_open(void* ch_obj);
void cc__chan_debug_dump_state(void* ch_obj, const char* prefix);

static _Atomic uint64_t g_v2_deadlock_first_seen = 0;
static _Atomic int g_v2_deadlock_reported = 0;

/* Persist-time before declaring deadlock. 1 s matches V1; enough to ride
 * out cc_block_all startup transients without annoying tests. */
#define SCHED_V2_DEADLOCK_PERSIST_MS 1000u

static uint64_t sched_v2_monotonic_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

static int sched_v2_fiber_is_open_chan_recv_wait(const fiber_v2* f) {
    if (!f || !f->park_reason || !f->park_obj) return 0;
    if (strcmp(f->park_reason, "chan_recv_wait_empty") != 0 &&
        strcmp(f->park_reason, "chan_recv_wait_rendezvous") != 0) {
        return 0;
    }
    return cc__chan_debug_is_open(f->park_obj);
}

/* Walk the fiber list once under the all_fibers_mu, classifying parked
 * fibers. Output counters:
 *   *internal_parked    — parked and NOT suppressed/external-wait
 *   *suppressed_parked  — parked and deadlock_suppress_depth > 0
 *   *external_parked    — parked and external_wait_depth > 0
 *   *saw_only_open_recv — 1 iff every internal_parked entry is a recv on
 *                          an open channel (caller uses this with the
 *                          external-wait exemption); 0 if we saw a
 *                          non-recv or recv on a closed channel. When
 *                          internal_parked == 0, returned value is 1 but
 *                          meaningless (caller must check internal_parked).
 */
static void sched_v2_classify_parked_fibers(size_t* internal_parked,
                                            size_t* suppressed_parked,
                                            size_t* external_parked,
                                            int* saw_only_open_recv) {
    *internal_parked = 0;
    *suppressed_parked = 0;
    *external_parked = 0;
    *saw_only_open_recv = 1;
    pthread_mutex_lock(&g_v2.all_fibers_mu);
    for (fiber_v2* f = g_v2.all_fibers; f; f = f->all_next) {
        int base = fiber_v2_state_base(
            atomic_load_explicit(&f->state, memory_order_acquire));
        if (base != FIBER_V2_PARKED) continue;
        if (f->external_wait_depth > 0) { (*external_parked)++; continue; }
        if (f->deadlock_suppress_depth > 0) { (*suppressed_parked)++; continue; }
        (*internal_parked)++;
        if (!sched_v2_fiber_is_open_chan_recv_wait(f)) *saw_only_open_recv = 0;
    }
    pthread_mutex_unlock(&g_v2.all_fibers_mu);
}

/* Print the same "internal parked fibers" block the V1 detector emits,
 * but formatted for V2 fibers. Called inside the DEADLOCK banner. Prints
 * at most 32 fibers; callers that need the full dump can rely on
 * sched_v2_debug_dump_state which follows. */
static void sched_v2_dump_parked_fibers_for_verdict(void) {
    size_t shown = 0;
    size_t total = 0;
    size_t parked_total = 0;
    size_t internal_total = 0;
    size_t skipped_total = 0;
    pthread_mutex_lock(&g_v2.all_fibers_mu);
    for (fiber_v2* f = g_v2.all_fibers; f; f = f->all_next) {
        total++;
        int base = fiber_v2_state_base(
            atomic_load_explicit(&f->state, memory_order_acquire));
        int is_parked = (base == FIBER_V2_PARKED);
        if (is_parked) parked_total++;
        int skipped = 0;
        if (is_parked && (f->external_wait_depth > 0 ||
                          f->deadlock_suppress_depth > 0)) {
            skipped = 1;
            skipped_total++;
        } else if (is_parked) {
            internal_total++;
        }
        if (shown < 32) {
            fprintf(stderr,
                    "  %sv2_fiber=%p state=%s reason=%s obj=%p last_thread=%d "
                    "external_wait_depth=%u suppress_depth=%u\n",
                    is_parked ? (skipped ? "(skipped parked) " : "(parked) ")
                              : "(not parked) ",
                    (void*)f, fiber_v2_state_name(base),
                    f->park_reason ? f->park_reason : "-",
                    f->park_obj, f->last_thread_id,
                    (unsigned)f->external_wait_depth,
                    (unsigned)f->deadlock_suppress_depth);
            if (is_parked && !skipped && f->park_obj && f->park_reason &&
                strncmp(f->park_reason, "chan_", 5) == 0) {
                cc__chan_debug_dump_state(f->park_obj, "    chan state: ");
            }
            shown++;
        }
    }
    pthread_mutex_unlock(&g_v2.all_fibers_mu);
    fprintf(stderr,
            "  fiber totals: total=%zu parked=%zu internal_parked=%zu "
            "skipped_parked=%zu\n",
            total, parked_total, internal_total, skipped_total);
    if (total > shown) {
        fprintf(stderr, "  ... %zu more fibers not shown\n", total - shown);
    }
}

void sched_v2_check_deadlock(void) {
    if (!g_v2.initialized) return;
    /* One-shot: if we already reported, don't re-enter. */
    if (atomic_load_explicit(&g_v2_deadlock_reported,
                             memory_order_acquire)) {
        return;
    }

    int n = atomic_load_explicit(&g_v2.num_threads, memory_order_acquire);
    if (n <= 0) return;
    int idle = sched_v2_count_idle_workers();
    size_t rq = atomic_load_explicit(&g_v2.ready_queue.count,
                                     memory_order_relaxed);
    if (idle < n || rq > 0) {
        /* Some worker busy or work pending — not stalled. Reset latch. */
        atomic_store_explicit(&g_v2_deadlock_first_seen, 0,
                              memory_order_relaxed);
        return;
    }

    size_t internal_parked = 0, suppressed_parked = 0, external_parked = 0;
    int saw_only_open_recv = 1;
    sched_v2_classify_parked_fibers(&internal_parked, &suppressed_parked,
                                    &external_parked, &saw_only_open_recv);

    if (internal_parked == 0) {
        /* All parks are exempt (suppressed or external-wait) — not a
         * deadlock per V1's contract. Reset latch. */
        atomic_store_explicit(&g_v2_deadlock_first_seen, 0,
                              memory_order_relaxed);
        return;
    }

    /* External-progress exemption: if any fiber or non-fiber thread is
     * inside a cc_external_wait scope AND the only internally-parked
     * fibers are receivers on still-open channels, assume the external
     * source will eventually produce. Reset the latch instead of
     * declaring deadlock. Matches V1 exactly:
     *   external_waits = external_parked_fibers + external_wait_threads
     *   if (external_waits > 0 && only_open_recv_waits) { reset; return; }
     * The open-channel check is the key: if any internal park is NOT on
     * an open-channel recv, the external source has no plausible way to
     * unblock it, so we proceed to latch the deadlock verdict. */
    size_t external_threads = atomic_load_explicit(&g_external_wait_threads,
                                                   memory_order_relaxed);
    size_t external_waits = external_parked + external_threads;
    if (external_waits > 0 && saw_only_open_recv) {
        atomic_store_explicit(&g_v2_deadlock_first_seen, 0,
                              memory_order_relaxed);
        return;
    }

    /* Latch timer: require the stall to persist >= SCHED_V2_DEADLOCK_PERSIST_MS. */
    uint64_t now = sched_v2_monotonic_ms();
    uint64_t first = atomic_load_explicit(&g_v2_deadlock_first_seen,
                                          memory_order_relaxed);
    if (first == 0) {
        atomic_compare_exchange_strong_explicit(
            &g_v2_deadlock_first_seen, &first, now,
            memory_order_relaxed, memory_order_relaxed);
        return;
    }
    if (now - first < SCHED_V2_DEADLOCK_PERSIST_MS) return;

    /* Claim the report slot. */
    int expected = 0;
    if (!atomic_compare_exchange_strong_explicit(
            &g_v2_deadlock_reported, &expected, 1,
            memory_order_acq_rel, memory_order_relaxed)) {
        return;
    }

    fprintf(stderr, "\n");
    fprintf(stderr, "╔══════════════════════════════════════════════════════════════╗\n");
    fprintf(stderr, "║                     DEADLOCK DETECTED                        ║\n");
    fprintf(stderr, "╚══════════════════════════════════════════════════════════════╝\n\n");

    fprintf(stderr, "Runtime state:\n");
    fprintf(stderr, "  V2 workers: %d total, %d idle (all idle is the stall signal)\n",
            n, idle);
    fprintf(stderr, "  V2 fibers:  %zu parked internal, %zu parked external-wait, "
                    "%zu parked deadlock-suppressed\n",
            internal_parked, external_parked, suppressed_parked);
    if (external_threads > 0) {
        fprintf(stderr, "  External threads: %zu in external-wait scope "
                        "(not a deadlock reliever because at least one "
                        "internal park is not an open-channel recv)\n",
                external_threads);
    }
    fprintf(stderr, "  Internal parked fibers:\n");
    sched_v2_dump_parked_fibers_for_verdict();
    fprintf(stderr, "\n");
    sched_v2_debug_dump_state("  ");
    fprintf(stderr, "\n");

    fprintf(stderr, "Common causes:\n");
    fprintf(stderr, "  • Channel send() with no receiver, or recv() with no sender\n");
    fprintf(stderr, "  • cc_fiber_join() on a fiber that's also waiting\n");
    fprintf(stderr, "  • Circular dependency between fibers\n\n");
    fprintf(stderr, "Debugging tips:\n");
    fprintf(stderr, "  • Check channel operations have matching send/recv pairs\n");
    fprintf(stderr, "  • Ensure channels are closed when done (triggers recv to return)\n");
    fprintf(stderr, "  • Review fiber spawn/join patterns for circular waits\n\n");

    const char* abort_env = getenv("CC_DEADLOCK_ABORT");
    if (!abort_env || abort_env[0] != '0') {
        fprintf(stderr, "Aborting with exit code 124. Set CC_DEADLOCK_ABORT=0 to continue.\n");
        fflush(stderr);
        _exit(124);
    }
    fprintf(stderr, "Continuing (CC_DEADLOCK_ABORT=0 set).\n");
    fflush(stderr);
}

/* ============================================================================
 * Join: block until fiber completes
 * ============================================================================ */

int sched_v2_join(fiber_v2* f, void** out_result) {
    if (!f) return -1;

    if (atomic_load_explicit(&f->done, memory_order_acquire)) {
        V2_STAT_INC(g_v2_join_fast);
        return sched_v2_finish_join(f, out_result);
    }

    int spin = g_v2_join_spin;
    for (int i = 0; i < spin; i++) {
        if (atomic_load_explicit(&f->done, memory_order_acquire)) {
            V2_STAT_INC(g_v2_join_spin_hit);
            return sched_v2_finish_join(f, out_result);
        }
        #if defined(__aarch64__) || defined(__arm64__)
        __asm__ volatile("yield");
        #elif defined(__x86_64__)
        __asm__ volatile("pause");
        #endif
    }

    if (cc__fiber_in_context()) {
        atomic_store_explicit(&f->join_waiter_fiber,
                              (cc__fiber*)cc__fiber_current(),
                              memory_order_release);
        /* Dekker pair with thread_v2_run_fiber completer: waiter publishes
         * itself then loads done; completer stores done then loads the
         * waiter pointer. Without a full fence on ARM64 the store-load can
         * reorder on both sides, so the completer may read NULL while the
         * waiter still observes done==0 -- a lost wake. */
        atomic_thread_fence(memory_order_seq_cst);
        V2_STAT_INC(g_v2_join_park_fiber);
        /* Expose the awaited V2 fiber via park_obj so the deadlock dump can
         * correlate the parked waiter with its target task. */
        cc__fiber_set_park_obj(f);
        while (!atomic_load_explicit(&f->done, memory_order_acquire)) {
            cc__fiber_clear_pending_unpark();
            CC_FIBER_PARK_IF(&f->done, 0, "sched_v2_join");
        }
        atomic_store_explicit(&f->join_waiter_fiber, NULL, memory_order_relaxed);
        return sched_v2_finish_join(f, out_result);
    }
    V2_STAT_INC(g_v2_join_park_thread);
    while (!atomic_load_explicit(&f->done, memory_order_acquire)) {
        uint32_t wait_val = atomic_load_explicit(&f->done_wake.value, memory_order_acquire);
        if (atomic_load_explicit(&f->done, memory_order_acquire)) break;
        wake_primitive_wait(&f->done_wake, wait_val);
    }

    return sched_v2_finish_join(f, out_result);
}

/* ============================================================================
 * Spawn: allocate fiber + enqueue + wake
 * ============================================================================ */

fiber_v2* sched_v2_spawn(void* (*fn)(void*), void* arg) {
    return sched_v2_spawn_in_nursery(fn, arg, NULL);
}

fiber_v2* sched_v2_spawn_in_nursery(void* (*fn)(void*), void* arg, CCNursery* nursery) {
    sched_v2_ensure_init();

    fiber_v2* f = fiber_v2_alloc();
    if (!f) return NULL;

    f->entry_fn = fn;
    f->entry_arg = arg;
    f->saved_nursery = nursery;
    f->admission_nursery = nursery;
    /* Do NOT create/init the coroutine here.
     *
     * We park the task on the global run queue with only fn/arg attached;
     * the worker that picks it up binds a coroutine on first resume
     * (allocating fresh only if the pool is empty). This keeps the producer
     * path to "alloc task record + enqueue + wake" and moves the stack
     * setup cost onto the worker that will actually execute the closure —
     * so the number of fresh mco_create calls is bounded by the number of
     * workers that can ever be running concurrently, not by the shape of
     * the producer. */
    atomic_store_explicit(&f->state, FIBER_V2_QUEUED, memory_order_release);

    sched_v2_enqueue_runnable(f);

    return f;
}

/* ============================================================================
 * Wait-ticket support (for kqueue / multi-wait integration)
 * ============================================================================ */

uint64_t sched_v2_fiber_publish_wait_ticket(fiber_v2* f) {
    if (!f) return 0;
    return atomic_fetch_add_explicit(&f->wait_ticket, 1, memory_order_acq_rel) + 1;
}

int sched_v2_fiber_wait_ticket_matches(fiber_v2* f, uint64_t ticket) {
    if (!f) return 0;
    uint64_t cur = atomic_load_explicit(&f->wait_ticket, memory_order_acquire);
    return cur == ticket;
}
