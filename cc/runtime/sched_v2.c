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
#include <mach/mach_time.h>
#endif

/* Ready-depth cell consumed by the lowering's inline @parallel deny gate
 * (declared in cc_sched.cch). Boots pointing at a static zero so a read
 * that somehow precedes scheduler init sees an empty queue (gate stays
 * open — the safe, admit-everything direction); sched_v2_init_impl
 * repoints it at the real queue counter. */
static volatile size_t g_par_depth_boot = 0;
volatile size_t* __cc_par_depth_addr = &g_par_depth_boot;

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
#if defined(__TINYC__)
            cc_cpu_yield_port();
#elif defined(__x86_64__) || defined(__i386__)
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
    /* Times this fiber left the CPU without dying (park or requeue).
     * Consumers: the adaptive spawn gate's sampler (scheduler.c), which
     * rejects arm-duration samples from fibers that ever suspended —
     * their wall time measures the subtree they joined, not their own
     * body. Relaxed atomics: incremented by the worker that ran the
     * fiber, read from inside the fiber possibly after migration. */
    _Atomic uint32_t suspends;

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
    _Atomic uint32_t deadlock_suppress_depth;
    _Atomic uint32_t external_wait_depth;

    /* Deadline-aware park.  When a caller parks with an absolute deadline
     * (e.g. @with_deadline wrapping a cc_chan_send that finds the channel
     * full), they publish it here before handing off to sched_v2_park().
     * sysmon's per-tick walk (sched_v2_wake_expired_parkers) signals any
     * parked fiber whose deadline has passed, which delivers a spurious
     * wake back into cc__fiber_park_if_impl; that function's post-park
     * deadline check then returns 1 (timeout) so the channel layer
     * returns ETIMEDOUT to @with_deadline.  Resolution is bounded by
     * V2_SYSMON_INTERVAL_MS (20 ms today), which is fine for correctness
     * — tests only care about observing ETIMEDOUT at all, not about
     * tight latency.  Deadline fields are atomic so sysmon can sample
     * them without all_fibers_mu or owning the fiber stack. */
    _Atomic int64_t park_deadline_sec;
    _Atomic int64_t park_deadline_nsec;
    _Atomic int     has_park_deadline;

    wake_primitive done_wake;
    cc__fiber* _Atomic join_waiter_fiber;
    void* current_deadline_scope;
    CCNurseryHost* saved_nursery;
    CCNurseryHost* admission_nursery;
    void* par_gate; /* CCParallel*; cancel wakes parks on this fiber */
    int   par_slot; /* dest live-index slot; -1 if not listed */

    /* R1 — user-facing async backtrace metadata.
     *
     * Set by `cc_nursery_spawn_async_named` (via the runner stamping its
     * own fiber on first execution) so any code running on this fiber can
     * answer "what async task am I?" via `cc_rt_diag_current_async_info`.
     * Cleared on fiber alloc/recycle so a pooled fiber never inherits the
     * previous task's name.
     *
     * Strings are caller-owned C string literals embedded in the lowered
     * C output (the spawn-site `__FILE__` and a sanitized callee name) —
     * no copy, no allocation on the spawn path. */
    const char* diag_user_name;
    const char* diag_file;
    int         diag_line;

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
 * pipelined request/reply server workloads; at ~8M push/pop pairs/sec the
 * uncontended mutex cost alone was a noticeable fraction of total time.
 * ============================================================================ */

typedef struct {
    v2_slock mu;
    fiber_v2* head;
    fiber_v2* tail;
    _Atomic size_t count;
    /* Monotonic pop counter. Read by sysmon's pool-growth recheck: a
     * counter that hasn't advanced across a recheck window while count>0
     * means the queue is backlogged with no drainer making progress. */
    _Atomic uint64_t pops;
} v2_queue;

static void v2_queue_init(v2_queue* q) {
    v2_slock_init(&q->mu);
    q->head = NULL;
    q->tail = NULL;
    atomic_store_explicit(&q->count, 0, memory_order_relaxed);
    atomic_store_explicit(&q->pops, 0, memory_order_relaxed);
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
        atomic_fetch_add_explicit(&q->pops, 1, memory_order_relaxed);
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
    /* Monotonic dispatch epoch: bumped to a fresh value each time this
     * worker starts running a fiber, reset to 0 when the fiber returns.
     * Sysmon scans this on each tick and compares against a locally-
     * cached snapshot from the previous tick: a worker that is still on
     * the same non-zero epoch has been running the same fiber for ≥1
     * sysmon tick (V2_SYSMON_INTERVAL_MS) and is a candidate for
     * in-place eviction as a kidnapped syscall.
     *
     * This replaces the prior `dispatch_start_ns` nanosecond timestamp:
     * the hot path no longer pays for mach_absolute_time/clock_gettime
     * per dispatch (each worker maintains a thread-local counter and
     * does a single relaxed store). Aging resolution is one tick
     * (20 ms today), which is what the previous 20 ms threshold also
     * provided in practice. */
    _Atomic uint64_t dispatch_epoch;
    /* Slot-identity generation. Bumped by sysmon every time a new worker
     * thread is installed into this slot (replacing an evicted orphan). A
     * running worker caches its generation at entry (tls_v2_my_generation);
     * when it observes slot.generation != my_gen, it's been orphaned and
     * must exit its loop without touching slot.wake or slot.is_idle. This
     * is the identity channel between sysmon and the worker — no separate
     * "detached" flag is needed. */
    _Atomic uint64_t generation;
    /* Parks executed while this slot was running a fiber. Per-slot so
     * the park path is an uncontended relaxed increment; sysmon sums
     * the live prefix when a grow episode is armed. */
    _Atomic uint64_t parks;
    wake_primitive wake;
} thread_v2;

/* ============================================================================
 * Global scheduler state
 * ============================================================================ */

struct sched_v2_state {
    thread_v2    threads[V2_MAX_THREADS];
    _Atomic int  num_threads;
    int          max_threads;
    _Atomic int  running;
    _Atomic int  idle_workers;
    int          allow_expand;
    pthread_mutex_t start_mu;

    /* Single global ready queue */
    v2_queue ready_queue;

    /* Fiber free list.
     *
     * Guarded by free_list_mu.  This was a lock-free Treiber stack, but
     * the pop path (load head; read head->next; CAS head->next in) has
     * the classic ABA hole: with no generation tag in the CAS, a thread
     * preempted between reading `next` and the CAS can — after the head
     * fiber is popped, reused, and freed back — install a stale `next`
     * that points at a fiber which is meanwhile LIVE again.  The next
     * alloc then hands out a live fiber, which double-enqueues it on the
     * intrusive ready queue (shared `next` link) and corrupts both lists.
     * A v2_slock costs one uncontended CAS, same as the ready queue's
     * own lock which is taken on every push/pop anyway. */
    v2_slock free_list_mu;
    /* Two stacks under one lock, split by whether the record still
     * carries a pooled coroutine (struct + ~2 MB stack). Alloc prefers
     * the carrying list. With a single LIFO, a spawn burst that
     * overflowed the coro-pool cap buried every retained stack under a
     * layer of bare records; steady state then popped a bare record
     * (mmap a fresh stack) and freed at the cap (munmap) on EVERY
     * spawn/join — measured at ~37% of storm CPU — while the pooled
     * stacks sat idle at the bottom, pinning the cap's worth of memory
     * to boot. */
    fiber_v2* _Atomic free_list;      /* records with f->coro != NULL */
    fiber_v2* _Atomic free_list_bare; /* records with f->coro == NULL */
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
extern _Atomic size_t g_external_wait_threads;
/* Dest completer: take f off dest's live index. 1 = this path frees. */
int cc_parallel_claim_child(void* dest, fiber_v2* f);

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
/* Spin-before-park outcomes (self-drain path, tid==worker_hint).
 *   worker_spin_hit:   queue became non-empty during the spin budget — we
 *                      dequeued a fresh fiber and stayed BUSY (0 syscalls).
 *   worker_spin_miss:  budget exhausted with queue still empty — fall
 *                      through to the park path (mark idle + ulock_wait). */
static _Atomic uint64_t g_v2_worker_spin_hit = 0;
static _Atomic uint64_t g_v2_worker_spin_miss = 0;
/* Admission-control (CC_V2_TARGET_ACTIVE) instrumentation.
 *   admit_ok:           try_admit_running CAS succeeded — this worker
 *                       is now counted toward g_v2_running_workers.
 *   admit_fail:         try_admit_running observed running >= target;
 *                       the worker went straight to the park cycle
 *                       instead of entering the drain loop.
 *   wake_gated_target:  producer-side sched_v2_wake(-1) skipped issuing
 *                       another wake because running is already at the
 *                       target; the extra parked-idle worker stays parked. */
static _Atomic uint64_t g_v2_worker_admit_ok = 0;
static _Atomic uint64_t g_v2_worker_admit_fail = 0;
static _Atomic uint64_t g_v2_wake_gated_target = 0;
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

/* Fast wall-clock tick for the worker hot path. On Apple Silicon,
 * mach_absolute_time() returns nanoseconds directly (timebase 1/1) and
 * compiles down to a single `mrs CNTVCT_EL0` (~6 cycles). On x86 macs and
 * older hardware the timebase numer/denom is set at init; we scale once.
 * On non-Apple platforms we fall back to CLOCK_MONOTONIC.
 *
 * Consumers (sysmon eviction, park-deadline check) only care about ~ms-scale
 * differences, so we prefer the cheap path over strict portability to
 * POSIX ns. */
#if defined(__APPLE__)
static struct mach_timebase_info v2_mach_tb = { 0, 0 };
static inline void v2_mach_tb_init_once(void) {
    if (v2_mach_tb.denom == 0) {
        mach_timebase_info(&v2_mach_tb);
        if (v2_mach_tb.denom == 0) {
            /* Defensive: should never happen, but avoid divide-by-zero. */
            v2_mach_tb.numer = 1;
            v2_mach_tb.denom = 1;
        }
    }
}
static inline uint64_t v2_now_ns(void) {
    uint64_t t = mach_absolute_time();
    /* Fast path on arm64 macOS where timebase is 1/1: skip the mult/div. */
    if (v2_mach_tb.numer == 1 && v2_mach_tb.denom == 1) return t;
    return (t * v2_mach_tb.numer) / v2_mach_tb.denom;
}
#else
static inline void v2_mach_tb_init_once(void) {}
static inline uint64_t v2_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
#endif

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

/* ---------------------------------------------------------------------------
 * Worker-pool shaping knobs
 * ---------------------------------------------------------------------------
 *
 * Three knobs tune how the N-slot worker pool responds to bursty or
 * chain-heavy workloads. All three are default-off (legacy behaviour) and
 * opt-in via env var; they compose orthogonally, so you can mix them.
 *
 * The problem they address (observed clearest in pipelined fan-in servers
 * where each request chains ~4 fiber handoffs — client fiber → request
 * channel → owner fiber → reply channel → client fiber — under dozens of
 * concurrent pipelined clients):
 *
 *   - Eagerly starting N workers and letting them all drain the same global
 *     ready queue produces thundering-herd churn: one pops, N-1 miss and
 *     commit to __ulock_wait, then the next enqueue wakes them all again.
 *   - Each park/wake round-trip is ~1-2 µs of syscall time, fired tens of
 *     thousands of times per second at N>1. Beyond some point the extra
 *     workers don't add throughput — they burn syscall bandwidth and
 *     thrash the ready-queue cache line.
 *
 * The three knobs attack different points along the park/wake lifecycle:
 *
 *   SPIN_BEFORE_PARK     — shrinks the time between "queue ran dry" and
 *                          "worker actually enters __ulock_wait". Busy-wait
 *                          polls the queue for a bounded budget first, so
 *                          fibers that arrive in the gap get picked up with
 *                          zero syscalls. Doesn't change how many workers
 *                          are eligible to run.
 *
 *   PARK_EXTRAS_AT_STARTUP — shapes the *initial condition*. Sends N-1
 *                            workers to park before they ever touch the
 *                            drain loop, so the first enqueue wakes only
 *                            one. No ongoing enforcement: once an extra
 *                            gets admitted it behaves like any normal
 *                            worker forever after.
 *
 *   TARGET_ACTIVE        — enforces a *steady-state cap* on concurrently
 *                          running workers. Every drain cycle re-checks
 *                          admission; the producer-side wake loop also
 *                          refuses to wake past target. Extras that slip
 *                          through get bounced back to park on the next
 *                          loop iter. Costs a counter CAS per admission
 *                          and a branch in the wake-many loop.
 *
 * Interaction notes (for future-me reaching for these again):
 *
 *   - PARK_EXTRAS is *not* a cap: under sustained backpressure,
 *     sched_v2_wake(-1) will still wake as many idle workers as there are
 *     queued fibers. If you want to guarantee "at most K active forever",
 *     use TARGET_ACTIVE (alone or in combination).
 *   - TARGET_ACTIVE subsumes PARK_EXTRAS's steady-state effect — any extras
 *     that show up eagerly will fail admission and re-park within
 *     microseconds. The init-time thundering herd is the only thing
 *     PARK_EXTRAS uniquely addresses.
 *   - SPIN_BEFORE_PARK is orthogonal to both: it changes what a *single*
 *     worker does when its own drain runs empty, not how many workers are
 *     eligible. Useful when fibers arrive in tight bursts faster than the
 *     wake-park cycle, regardless of whether you've capped the pool.
 * --------------------------------------------------------------------------- */

/* Tunable via CC_V2_SPIN_BEFORE_PARK env var.
 *
 * Budget of cpu_relax iterations a worker spends polling the ready queue
 * after its drain loop observes an empty queue, BEFORE committing to
 * __ulock_wait. During the spin the worker stays is_idle=0, so producers
 * take the idle_workers==0 fast path and issue no wake syscall; a fiber
 * that lands in the spin window is picked up in self-drain with zero
 * syscalls on either side.
 *
 * Budget shape: arm64 "yield" is ~1-2 cycles (4096 iters ≈ 2-4 µs on
 * Apple silicon); x86 "pause" is ~25-140 cycles (scale accordingly).
 * The upper clamp at init is 1e9 so pathological sweeps are survivable.
 *
 * Safety: sysmon's in-place eviction targets workers with dispatch_epoch
 * != 0 (i.e. actively running a fiber). A spinning worker has
 * dispatch_epoch == 0, so it is not a candidate for eviction during the
 * spin.
 *
 * 0 disables (legacy: park immediately on empty queue). */
static int g_v2_spin_before_park = 0;

/* Tunable via CC_V2_PARK_EXTRAS_AT_STARTUP env var.
 *
 * When non-zero, workers with tid != 0 mark idle and park immediately at
 * thread_v2_main entry, skipping the initial self-drain attempt entirely.
 * Only the primary (tid == 0) enters the drain loop at startup; extras
 * are admitted on demand when sched_v2_wake(-1) observes real backpressure
 * (queue > 0 AND idle > 0).
 *
 * One-shot: this is an init-time shape, not a steady-state cap. Once an
 * extra has been woken and admitted it runs like any other worker and
 * parks via the normal Dekker park-when-empty path. Subsequent wakes can
 * recruit it again, and under sustained load nothing prevents all N
 * workers from being active simultaneously. For a true cap, use
 * CC_V2_TARGET_ACTIVE (alone or with this).
 *
 * Cost: one extra park at startup per extra; zero hot-path overhead
 * thereafter.
 *
 * 0 disables (legacy: all N workers enter the drain loop at startup,
 * producing a thundering herd on the first enqueue). */
static int g_v2_park_extras_at_startup = 0;

/* Tunable via CC_V2_TARGET_ACTIVE env var.
 *
 * Steady-state admission cap on concurrently running worker threads.
 * g_v2_running_workers counts workers that are NOT currently in
 * __ulock_wait — actively spinning, draining, or executing a fiber. On
 * every iteration of thread_v2_main a worker calls try_admit_running():
 *   - CAS-inc running_workers only if the result stays <= target.
 *   - On failure (target saturated), the worker skips drain, marks idle,
 *     and parks without touching the ready queue.
 *   - deadmit_running() releases the slot when the drain loop exits.
 *
 * Producer-side: sched_v2_wake(-1) short-circuits its wake-many loop once
 * running_workers >= target (counted in g_v2_wake_gated_target). Skipping
 * those wakes avoids __ulock_wake syscalls whose only effect would be to
 * make an extra cycle park → wake → admit-fail → re-park.
 *
 * Practical sizing:
 *   - 1 for chain-heavy I/O workloads where a single hot drainer keeps
 *     the ready queue L1-warm and avoids lock contention on the global
 *     queue (the fan-in request/reply server shape: many client fibers
 *     handing off to one owner fiber and back).
 *   - N (CPU count) for CPU-bound parallel workloads (pigz, etc.) that
 *     genuinely want every core drainable.
 *   - 0 = disabled (legacy: all workers always eligible, no gating).
 *
 * Cost: one atomic load + branch per drain cycle + one admission CAS per
 * drain cycle + one branch in the producer wake-many loop. Non-zero but
 * cheap enough to leave permanently enabled once a target is set. */
static int g_v2_target_active = 0;
static _Atomic int g_v2_running_workers = 0;

/* Deferred pool growth (CC_V2_EAGER_THREADS / CC_V2_GROW_RECHECK_US).
 *
 * Inline pthread_create from the push path is limited to the first
 * g_v2_eager_threads workers (default 2: thread #2 is what buys overlap
 * for the common case and is worth paying for inline). Beyond that a
 * push that finds work queued and nobody idle does NOT create a thread;
 * it sets g_v2_grow_pending (one CAS; the winner pays one sysmon wake
 * syscall) and sysmon deliberates on a fast recheck cadence
 * (g_v2_grow_recheck_us, default 25us) instead of its normal 20ms tick.
 * Window floor: below ~10us the kernel timeout resolution makes the
 * window length unreliable and a healthy pool lands too few pops per
 * window for the rate test to separate signal from jitter.
 *
 * Per recheck, sysmon samples (ready_queue.pops, slot park sums,
 * v2_now_ns) against the episode baseline. With depth =
 * ready_queue.count and n = num_threads, grow one worker iff:
 *   - the pool's aggregate drain rate is below one pop per worker per
 *     g_v2_grow_rate_us (workers blocked or running long CPU arms), OR
 *   - depth >= 2*n (backlog relative to pool size), sustained across
 *     g_v2_grow_depth_dwell consecutive rechecks,
 * AND the episode is not run-to-park: parks since baseline do not
 * exceed half the pops. A high park fraction is recv/accept or lock
 * wait multiplexing — extra workers add traffic. A low park fraction
 * with a slow drain is CPU-bound work still occupying workers.
 * Otherwise hold: a shallow, briskly-draining queue is park/wake
 * churn; a deep, high-park queue is a nursery accept/request wave.
 * Both rate and depth scale with pool size; the rate is normalized
 * by measured elapsed time, so the decision is stable against kernel
 * timeout jitter in the recheck sleep itself.
 *
 * An episode ends on a full pool, an admission gate, or true slack:
 * every worker idle and the ready queue empty, or one spare worker
 * plus an empty queue persisting for SCHED_V2_GROW_SLACK_NS (a single
 * park between CPU-bound arms is not slack). depth==0 with nobody idle
 * is saturation — keep sampling so the rate trigger can recruit.
 * The env knobs below are test overrides; defaults settle from the
 * workload.
 *
 * An optional slow-tick escalation (CC_V2_GROW_ESCALATE_TICKS, default
 * off) can additionally grow on sustained backlog regardless of rate;
 * see g_v2_grow_escalate_ticks.
 *
 * The pool remains a ratchet: workers are never culled. */
static int g_v2_eager_threads = 2;
static int g_v2_grow_recheck_us = 25;
/* Hold threshold, in "microseconds per pop per worker": the pool is
 * considered healthy while each worker averages at least one pop per
 * this many us. The recheck cadence and this bar are independent: the
 * rate is normalized by measured elapsed time since the episode
 * baseline, so checking more often does not change the measurement,
 * only the reaction latency. Lowering the bar itself (smaller value)
 * biases toward growth. CC_V2_GROW_RATE_US. */
static int g_v2_grow_rate_us = 100;
/* Depth trigger: grow when ready-queue depth >= this multiple of the
 * pool size even if the drain rate is healthy. 0 disables the depth
 * trigger (rate-only growth). CC_V2_GROW_DEPTH_X. */
static int g_v2_grow_depth_mult = 2;
/* Depth dwell: the depth trigger must hold on this many consecutive
 * rechecks before it grows. A single recheck cannot tell backlog from
 * a completion wave: on the first sample of an episode dp is a few pops
 * and dpark is 0 only because nothing has had time to park, so
 * run_to_park cannot yet say "recv churn"; and a kqueue wave for many
 * clients is depth >= 2n that drains in tens of us. Both recruited a
 * worker that then stayed for the life of the process, and on an
 * I/O-bound pool each extra live worker is global-queue contention and
 * park/wake churn without extra work (redis at 50 pipelined clients: 2
 * workers 2.9M GET/s, 4 workers 2.6M). Three rechecks is ~75us of
 * sustained depth; a producer that outpaces the pool holds it easily.
 * The rate trigger (CPU-bound arms) is not dwelled. CC_V2_GROW_DEPTH_DWELL. */
static int g_v2_grow_depth_dwell = 3;
/* Slow-tick escalation dwell: grow one worker per slow tick once the
 * ready queue has stayed non-empty (with nobody idle) for this many
 * consecutive slow ticks, regardless of the rate test.
 * CC_V2_GROW_ESCALATE_TICKS.
 *
 * Default 0 (disabled): measured across the perf suite, the rate/depth
 * triggers alone recruit correctly for every workload shape — CPU-bound
 * fibers don't park, so they produce a near-zero pop rate and grow via
 * the stall trigger; a high pop rate only comes from park/wake churn,
 * where holding is right. Escalation's only observed steady-state effect
 * was converting held contention regimes back to a full (over-recruited)
 * pool. Kept as opt-in insurance for a workload that pops briskly AND
 * scales with workers, should one appear. */
static int g_v2_grow_escalate_ticks = 0;
/* Dwell before a spare worker + empty queue counts as slack. One
 * recheck of idle is a park between long arms, not spare capacity. */
#define SCHED_V2_GROW_SLACK_NS 200000ull
static _Atomic int g_v2_grow_pending = 0;
static _Atomic uint64_t g_v2_grow_requests = 0;   /* producer defers      */
static _Atomic uint64_t g_v2_grow_stall = 0;      /* recheck: pops slow   */
static _Atomic uint64_t g_v2_grow_backlog = 0;    /* recheck: deep queue  */
static _Atomic uint64_t g_v2_grow_escalate = 0;   /* slow-tick escalation */
static _Atomic uint64_t g_v2_grow_held = 0;       /* recheck decided hold */
static _Atomic uint64_t g_v2_grow_parked = 0;     /* hold: run-to-park     */

/* Coro-pool high-water cap (tunable via CC_V2_CORO_POOL_MAX).
 *
 * fiber_v2_free retains each completed fiber's minicoro allocation (~2 MB
 * stack) on the lock-free free list so the next spawn can mco_init in place
 * instead of paying a fresh ~2 MB mmap. That reuse is a big win at steady
 * state, but the free list was previously UNBOUNDED: under a spawn burst
 * where the producer outruns the (capped) worker pool — e.g. a nursery
 * spawning 100k non-parking tasks — the free list's high-water mark climbs
 * to ~= the number of tasks. At 2 MB each that is ~200 GB of simultaneously
 * mapped stacks for 100k tasks, which exhausts the process VM map and hard-
 * crashes the machine.
 *
 * The cap bounds the number of coro-bearing fibers PARKED ON THE FREE LIST.
 * It does NOT touch live or parked-in-flight fibers (those legitimately own
 * their coro); it only decides whether a *completed* fiber keeps its stack
 * for reuse or releases it. When the pool is already at capacity the surplus
 * coro is mco_destroy'd (the 2 MB is actually unmapped) so total mapped coro
 * memory is bounded by  (live fibers) + g_v2_coro_pool_max.
 *
 * g_v2_pooled_coros is an approximate count (a free observing the cap and the
 * subsequent increment are not one atomic step), so the pool may overshoot by
 * up to ~num_threads under heavy concurrent completion — a bounded, harmless
 * slop. Default 512 -> <=1 GB of pooled stacks; set 0 to disable the cap and
 * restore the old unbounded behaviour. */
static int g_v2_coro_pool_max = 512;
static _Atomic int g_v2_pooled_coros = 0;

/* Try to become an "active running" worker. Returns 1 on success (caller
 * is counted toward g_v2_running_workers and must call deadmit_running
 * before parking) or 0 if target is already saturated. */
static int try_admit_running(void) {
    if (g_v2_target_active <= 0) {
        atomic_fetch_add_explicit(&g_v2_running_workers, 1,
                                  memory_order_acq_rel);
        V2_STAT_INC(g_v2_worker_admit_ok);
        return 1;
    }
    int cur = atomic_load_explicit(&g_v2_running_workers,
                                   memory_order_relaxed);
    while (cur < g_v2_target_active) {
        if (atomic_compare_exchange_weak_explicit(
                &g_v2_running_workers, &cur, cur + 1,
                memory_order_acq_rel, memory_order_relaxed)) {
            V2_STAT_INC(g_v2_worker_admit_ok);
            return 1;
        }
        /* cur was updated by CAS on failure */
    }
    V2_STAT_INC(g_v2_worker_admit_fail);
    return 0;
}

static void deadmit_running(void) {
    atomic_fetch_sub_explicit(&g_v2_running_workers, 1, memory_order_release);
}

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
                                      uint64_t* parked_internal,
                                      uint64_t* parked_external_wait,
                                      uint64_t* parked_deadlock_suppressed,
                                      park_reason_bucket reason_buckets[V2_DIAG_REASON_BUCKETS],
                                      size_t* reason_bucket_count) {
    for (int i = 0; i < FIBER_V2_STATE_COUNT; ++i) {
        state_counts[i] = 0;
    }
    *parked_wait_many = 0;
    *parked_wait = 0;
    *parked_other = 0;
    *parked_unknown = 0;
    *parked_internal = 0;
    *parked_external_wait = 0;
    *parked_deadlock_suppressed = 0;
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
                if (atomic_load_explicit(&f->external_wait_depth, memory_order_acquire) > 0) {
                    (*parked_external_wait)++;
                } else if (atomic_load_explicit(&f->deadlock_suppress_depth, memory_order_acquire) > 0) {
                    (*parked_deadlock_suppressed)++;
                } else {
                    (*parked_internal)++;
                }
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
#if defined(__TINYC__)
#define tls_v2_thread_id (cc_rt_tls_get()->v2_thread_id)
#define tls_v2_my_generation (cc_rt_tls_get()->v2_my_generation)
#define tls_v2_current_fiber (*(fiber_v2**)&(cc_rt_tls_get()->v2_current_fiber))
#define tls_v2_dispatch_seq (cc_rt_tls_get()->v2_dispatch_seq)
#else
static __thread int tls_v2_thread_id = -1;
static __thread uint64_t tls_v2_my_generation = 0;
static __thread fiber_v2* tls_v2_current_fiber = NULL;
/* Per-worker monotonic dispatch counter, incremented before each fiber
 * run. Published to slot.dispatch_epoch so sysmon can detect "same fiber
 * still running after one tick" without any wall-clock read on the hot
 * path. Starts at 1 so that 0 unambiguously means "no fiber running". */
static __thread uint64_t tls_v2_dispatch_seq = 0;
#endif
bool cc_nursery_is_cancelled_host(const CCNurseryHost* n);
void cc_nursery_notify_child_done(CCNurseryHost* n);

/* Mirror of nursery.c's CC_NURSERY_WORKER_FREES gate.  Latched on first
 * read so we never branch on a changing env var in the hot MCO_DEAD
 * path. Default-on; CC_NURSERY_WORKER_FREES=0 opts out. */
static int cc_v2_worker_frees_mode(void) {
    static _Atomic int cached = -1;
    int v = atomic_load_explicit(&cached, memory_order_relaxed);
    if (v >= 0) return v;
    const char* s = getenv("CC_NURSERY_WORKER_FREES");
    int newv = !(s && s[0] == '0' && s[1] == 0);
    atomic_store_explicit(&cached, newv, memory_order_relaxed);
    return newv;
}

static void fiber_v2_entry(mco_coro* co) {
    fiber_v2* f = (fiber_v2*)mco_get_user_data(co);
    if (!f) return;
    atomic_thread_fence(memory_order_acquire);
    if (f->entry_fn) {
        if (f->admission_nursery && cc_nursery_is_cancelled_host(f->admission_nursery)) {
            f->result = NULL;
        } else {
            f->result = f->entry_fn(f->entry_arg);
        }
    } else {
        f->result = NULL;
    }
    f->admission_nursery = NULL;
}

/* ============================================================================
 * Fiber pool
 * ============================================================================ */

static fiber_v2* fiber_v2_alloc(void) {
    /* Try the free lists first (see free_list_mu: pop must be ABA-safe).
     * Prefer a record that still carries a pooled coroutine — popping a
     * bare one while carrying ones exist costs an mmap now and, at the
     * coro-pool cap, a munmap on the eventual free. */
    fiber_v2* f = NULL;
    if (atomic_load_explicit(&g_v2.free_list, memory_order_acquire) ||
        atomic_load_explicit(&g_v2.free_list_bare, memory_order_acquire)) {
        v2_slock_lock(&g_v2.free_list_mu);
        f = atomic_load_explicit(&g_v2.free_list, memory_order_relaxed);
        if (f) {
            atomic_store_explicit(&g_v2.free_list, f->next, memory_order_relaxed);
        } else {
            f = atomic_load_explicit(&g_v2.free_list_bare, memory_order_relaxed);
            if (f)
                atomic_store_explicit(&g_v2.free_list_bare, f->next,
                                      memory_order_relaxed);
        }
        v2_slock_unlock(&g_v2.free_list_mu);
    }
    if (f) {
            f->generation++;
            f->next = NULL;
            /* Keep f->coro as-is: fiber_v2_free left the minicoro allocation
             * (coro struct + ~2 MB stack) alive so spawn can mco_init in
             * place. Setting it NULL here would force a fresh alloc every
             * time and defeat the pool. */
            if (f->coro) {
                /* This pooled fiber carried a retained coro; it is leaving
                 * the pool, so drop the pooled-coro count by one. */
                atomic_fetch_sub_explicit(&g_v2_pooled_coros, 1,
                                          memory_order_relaxed);
            }
            f->result = NULL;
            f->entry_fn = NULL;
            f->entry_arg = NULL;
            f->last_thread_id = -1;
            f->saved_nursery = NULL;
            f->admission_nursery = NULL;
            f->par_gate = NULL;
            f->par_slot = -1;
            atomic_store_explicit(&f->done, 0, memory_order_relaxed);
            atomic_store_explicit(&f->wait_ticket, 0, memory_order_relaxed);
            atomic_store_explicit(&f->join_waiter_fiber, NULL, memory_order_relaxed);
            f->current_deadline_scope = NULL;
            f->yield_kind = V2_YIELD_PARK;
            f->park_reason = NULL;
            f->park_obj = NULL;
            atomic_store_explicit(&f->deadlock_suppress_depth, 0u, memory_order_relaxed);
            atomic_store_explicit(&f->external_wait_depth, 0u, memory_order_relaxed);
            f->diag_user_name = NULL;
            f->diag_file = NULL;
            f->diag_line = 0;
            atomic_store_explicit(&f->has_park_deadline, 0, memory_order_relaxed);
            atomic_store_explicit(&f->state, FIBER_V2_IDLE, memory_order_relaxed);
            V2_STAT_INC(g_v2_fibers_alive);
            return f;
    }

    f = (fiber_v2*)calloc(1, sizeof(fiber_v2));
    if (!f) return NULL;
    f->coro = NULL;
    f->last_thread_id = -1;
    f->park_reason = NULL;
    f->current_deadline_scope = NULL;
    f->saved_nursery = NULL;
    f->admission_nursery = NULL;
    f->par_gate = NULL;
    f->par_slot = -1;
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
    f->current_deadline_scope = NULL;
    f->saved_nursery = NULL;
    f->admission_nursery = NULL;
    f->par_gate = NULL;
    f->par_slot = -1;
    /* Clear detector metadata so the next spawn starts clean and the
     * detector never observes stale park_obj/suppress/external-wait state
     * on a pooled fiber. */
    f->park_obj = NULL;
    atomic_store_explicit(&f->deadlock_suppress_depth, 0u, memory_order_relaxed);
    atomic_store_explicit(&f->external_wait_depth, 0u, memory_order_relaxed);
    /* Same reason: clear R1 async backtrace metadata so a pooled fiber
     * doesn't surface the previous task's name to `cc_rt_diag_current_async_info`. */
    f->diag_user_name = NULL;
    f->diag_file = NULL;
    f->diag_line = 0;
    atomic_store_explicit(&f->has_park_deadline, 0, memory_order_relaxed);
    /* Pool the coroutine memory (including its stack): mco_uninit just marks
     * the coro DEAD and runs platform teardown (a no-op on ucontext), while
     * preserving the allocation so the next spawn can re-init in place
     * instead of paying another ~2 MB alloc/free.
     *
     * BUT only up to g_v2_coro_pool_max retained coros: beyond that we
     * actually release the ~2 MB stack (mco_destroy) instead of retaining it,
     * so a spawn burst that outruns the worker pool can't grow the free list —
     * and thus mapped stack memory — without bound. See the cap rationale at
     * g_v2_coro_pool_max. */
    if (f->coro) {
        int cap = g_v2_coro_pool_max;
        if (cap > 0 &&
            atomic_load_explicit(&g_v2_pooled_coros, memory_order_relaxed) >= cap) {
            (void)mco_destroy(f->coro);
            f->coro = NULL;
        } else {
            (void)mco_uninit(f->coro);
            atomic_fetch_add_explicit(&g_v2_pooled_coros, 1, memory_order_relaxed);
        }
    }
    /* Push under free_list_mu (see the field comment: the lock-free pop
     * was ABA-unsafe, and push/pop must share the same discipline).
     * Carrying and bare records go to separate stacks so alloc can
     * prefer the carrying ones (see the field comment). */
    fiber_v2* _Atomic* list = f->coro ? &g_v2.free_list : &g_v2.free_list_bare;
    v2_slock_lock(&g_v2.free_list_mu);
    f->next = atomic_load_explicit(list, memory_order_relaxed);
    atomic_store_explicit(list, f, memory_order_release);
    v2_slock_unlock(&g_v2.free_list_mu);
}

/* ============================================================================
 * Wake infrastructure: try_wake_one, try_expand_pool, sched_v2_wake
 * ============================================================================ */

static void* thread_v2_main(void* arg);
static void sched_v2_wake(int worker_hint);

static void sched_v2_init_worker_slot(int id) {
    g_v2.threads[id].id = id;
    atomic_store_explicit(&g_v2.threads[id].is_idle, 0, memory_order_relaxed);
    atomic_store_explicit(&g_v2.threads[id].alive, 0, memory_order_relaxed);
    atomic_store_explicit(&g_v2.threads[id].dispatch_epoch, 0,
                          memory_order_relaxed);
    /* Initial worker in this slot: generation 0. First eviction will
     * bump it to 1; the cached tls_v2_my_generation in the orphan
     * stays at 0 and the mismatch triggers exit. */
    atomic_store_explicit(&g_v2.threads[id].generation, 0,
                          memory_order_release);
    atomic_store_explicit(&g_v2.threads[id].parks, 0, memory_order_relaxed);
    wake_primitive_init(&g_v2.threads[id].wake);
}

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

/* Start one additional worker lazily. Returns 1 if a thread was created. */
static int sched_v2_try_expand_pool(void) {
    if (!g_v2.allow_expand) return 0;
    /* Saturated fast path: once the pool is full it stays full (num_threads
     * only grows), so a full-pool observation here is stable and skipping
     * the mutex is always correct. Under lock-heavy saturation every push
     * whose wake finds no idle worker lands here; without this check each
     * of those pays a contended pthread mutex round trip on the wake path. */
    if (atomic_load_explicit(&g_v2.num_threads, memory_order_acquire)
        >= g_v2.max_threads) {
        return 0;
    }
    pthread_mutex_lock(&g_v2.start_mu);
    int new_id = atomic_load_explicit(&g_v2.num_threads, memory_order_acquire);
    if (new_id >= g_v2.max_threads) {
        pthread_mutex_unlock(&g_v2.start_mu);
        return 0;
    }
    sched_v2_init_worker_slot(new_id);
    if (pthread_create(&g_v2.threads[new_id].handle, NULL,
                       thread_v2_main, (void*)(intptr_t)new_id) != 0) {
        wake_primitive_destroy(&g_v2.threads[new_id].wake);
        pthread_mutex_unlock(&g_v2.start_mu);
        return 0;
    }
    atomic_store_explicit(&g_v2.num_threads, new_id + 1, memory_order_release);
    pthread_mutex_unlock(&g_v2.start_mu);
    return 1;
}

/* Ask sysmon to consider growing the pool. First requester per episode
 * pays one wake syscall; everyone else sees the flag already set. */
static void sched_v2_request_grow(void) {
    int expected = 0;
    if (atomic_compare_exchange_strong_explicit(&g_v2_grow_pending, &expected, 1,
            memory_order_acq_rel, memory_order_relaxed)) {
        V2_STAT_INC(g_v2_grow_requests);
        wake_primitive_wake_one(&g_v2.sysmon_wake);
    }
}

/* Push-path growth: create inline only up to g_v2_eager_threads, defer
 * the rest to sysmon's recheck. Returns 1 if a thread was created. */
static int sched_v2_grow_or_defer(void) {
    int n = atomic_load_explicit(&g_v2.num_threads, memory_order_acquire);
    if (n >= g_v2.max_threads) return 0;
    if (n < g_v2_eager_threads) return sched_v2_try_expand_pool();
    sched_v2_request_grow();
    return 0;
}

/* No idle worker while the ready queue is non-empty. Still grow when
 * we are under the eager cap (inline thread #2) or the queue is deeper
 * than the live pool. Skip otherwise: SPSC rendezvous and 2-arm join
 * ping-pong have ready<=n and must not CAS grow_pending / poke sysmon
 * on every handshake. Sysmon's tick is the safety net. */
static void sched_v2_grow_if_backlogged(void) {
    int n = atomic_load_explicit(&g_v2.num_threads, memory_order_acquire);
    size_t ready = atomic_load_explicit(&g_v2.ready_queue.count,
                                        memory_order_relaxed);
    if (n < g_v2_eager_threads || ready > (size_t)n) {
        (void)sched_v2_grow_or_defer();
        return;
    }
    V2_STAT_INC(g_v2_wake_no_idle);
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
            if (!f) {
                /* Spin-before-park: rather than immediately return and
                 * commit to __ulock_wait, stay BUSY for a bounded budget
                 * of cpu_relax cycles, polling the queue. Fibers that land
                 * during the window are picked up in self-drain with zero
                 * syscalls, and producers never see us as idle so they
                 * don't issue a wake syscall either.
                 *
                 * Safety: we stay is_idle=0 throughout. sysmon's in-place
                 * eviction only targets workers with dispatch_epoch!=0
                 * (it was reset at the end of the previous fiber), so we
                 * are not a candidate during the spin. Generation check
                 * runs in the outer thread_v2_main loop on return. */
                int spin = g_v2_spin_before_park;
                while (spin-- > 0) {
#if defined(__TINYC__)
                    cc_cpu_yield_port();
#elif defined(__aarch64__) || defined(__arm__)
                    __asm__ volatile("yield" ::: "memory");
#elif defined(__x86_64__) || defined(__i386__)
                    __asm__ volatile("pause" ::: "memory");
#else
                    __asm__ volatile("" ::: "memory");
#endif
                    if (atomic_load_explicit(&g_v2.ready_queue.count,
                                             memory_order_acquire) > 0) {
                        f = v2_queue_pop(&g_v2.ready_queue);
                        if (f) break;
                        /* Lost the race to another drainer; keep spinning. */
                    }
                }
                if (!f) {
                    V2_STAT_INC(g_v2_worker_spin_miss);
                    return;
                }
                V2_STAT_INC(g_v2_worker_spin_hit);
            }
            V2_STAT_INC(g_v2_worker_self_drain);
            /* Publish a fresh dispatch epoch so sysmon can detect
             * kidnapped syscalls. We use a thread-local monotonic counter
             * and a single relaxed store — no wall-clock syscall on the
             * hot path. Sysmon compares this against a snapshot it took
             * on the previous tick; if it hasn't changed and is non-zero,
             * the worker has been on the same fiber for ≥1 tick. */
            uint64_t seq = ++tls_v2_dispatch_seq;
            atomic_store_explicit(&g_v2.threads[worker_hint].dispatch_epoch,
                                  seq, memory_order_relaxed);
            thread_v2_run_fiber(worker_hint, f);
            atomic_store_explicit(&g_v2.threads[worker_hint].dispatch_epoch,
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

    while (atomic_load_explicit(&g_v2.ready_queue.count, memory_order_relaxed) > 0) {
        /* Producer-side admission gate: if we're already at the active
         * target, issuing another wake just produces a ulock_wake syscall
         * whose only effect is to make an extra worker cycle through
         * park→wake→try_admit_fail→re-park. Skip it.
         *
         * Note: count != #wakes — running_workers lags the wake by one
         * ulock_wait round-trip, so we may over-wake by ~1 per burst.
         * Good enough; the consumer-side gate catches the rest. */
        if (g_v2_target_active > 0 &&
            atomic_load_explicit(&g_v2_running_workers, memory_order_acquire)
                >= g_v2_target_active) {
            V2_STAT_INC(g_v2_wake_gated_target);
            break;
        }
        if (atomic_load_explicit(&g_v2.idle_workers, memory_order_acquire) <= 0) {
            sched_v2_grow_if_backlogged();
            break;
        }
        if (!sched_v2_try_wake_one()) {
            sched_v2_grow_if_backlogged();
            break;
        }
    }
}

/* ============================================================================
 * Run fiber + post-yield commit
 * ============================================================================ */

static void thread_v2_run_fiber(int tid, fiber_v2* f) {
    int raw = atomic_load_explicit(&f->state, memory_order_acquire);
    if (fiber_v2_state_base(raw) != FIBER_V2_QUEUED) {
        fprintf(stderr, "[sched_v2] BUG: thread %d got fiber in state %d\n", tid,
                fiber_v2_state_base(raw));
        return;
    }
    /* QUEUED -> RUNNING must be an RMW that carries SIGNAL_PENDING: a
     * signal racing this pickup CASes the pending bit onto QUEUED, and a
     * plain store here would wipe it — the fiber could then re-check its
     * wait condition before the signaler published it, park, and lose the
     * wake.  Carrying the bit makes the park-commit requeue instead. */
    for (;;) {
        int desired = FIBER_V2_RUNNING | (raw & FIBER_V2_FLAG_SIGNAL_PENDING);
        if (atomic_compare_exchange_weak_explicit(&f->state, &raw, desired,
                memory_order_acq_rel, memory_order_acquire)) {
            break;
        }
        /* CAS failure can only mean a signal toggled the pending bit;
         * only this worker moves the fiber out of QUEUED. */
    }
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
        /* Snapshot dest/nursery ownership BEFORE publishing done=1.
         *
         * The moment done=1 lands, a joiner (cc_block_on / sched_v2_join)
         * may return, call sched_v2_fiber_release, and push this fiber_v2
         * back to the pool — and a concurrent spawn may then REUSE it,
         * repopulating saved_nursery for a brand-new task.  If we read
         * f->saved_nursery after that recycle, we'd see the NEW task's
         * nursery, take the worker-frees branch below, and free a LIVE
         * fiber that is sitting in the ready queue.  fiber_v2_free then
         * clobbers f->next (the intrusive ready-queue link) with the
         * free-list link, severing the ready queue: the queue count goes
         * phantom (workers spin on count>0 with an empty chain), the
         * recycled fiber is delivered from two lists at once
         * ("BUG: got fiber in state 0" / mco_resume rc=4 aborts), and
         * cc_nursery_notify_child_done fires early for a child that never
         * ran.  Reproduced by tests/hybrid_run_to_completion_smoke.ccs
         * under parallel suite load (block_on releases t1/t2 into the
         * pool right before three nursery spawns reuse them).
         *
         * A dest child claims itself off the live index here, while f
         * cannot yet have been recycled: done is still 0, so no joiner
         * has released it. Claim is exclusive with wait / leave /
         * admit-reap. Dest-attached fibers are not nursery worker-frees
         * — if claim loses, the joiner that took the slot frees. */
        void* gate = f->par_gate;
        int claimed = 0;
        if (gate)
            claimed = cc_parallel_claim_child(gate, f);
        int worker_frees = cc_v2_worker_frees_mode();
        CCNurseryHost* adm = (worker_frees && !gate) ? f->saved_nursery : NULL;
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

        /* Worker-frees mode (CC_NURSERY_WORKER_FREES=1): nursery-owned
         * fibers have no external joiner — cc_nursery_wait waits on the
         * nursery's alive_count barrier, not on individual fibers — so
         * the worker that observes MCO_DEAD is the last touch point and
         * owns the release back to the v2 pool.  In classic mode the
         * join+release lives in cc_nursery_wait, so we leave the fiber
         * alone here. */
        /* NOTE: admission_nursery is cleared by fiber_v2_entry right
         * before the coroutine returns, so it is guaranteed NULL by
         * the time we observe MCO_DEAD here.  saved_nursery holds
         * the same value as admission_nursery at spawn time and is
         * only cleared by fiber_v2_free/alloc, so it survives
         * through entry and is the correct handle to identify a
         * nursery-owned fiber at completion.  adm was snapshotted
         * above, before done=1 could hand the fiber to a joiner. */
        if (claimed) {
            fiber_v2_free(f);
        } else if (worker_frees && adm) {
            fiber_v2_free(f);
            cc_nursery_notify_child_done(adm);
        }
        return;
    }

    /* Every path below is a suspension (yield/pending requeue or park
     * commit); the fiber comes back to the CPU later. See field comment. */
    atomic_fetch_add_explicit(&f->suspends, 1, memory_order_relaxed);

    int raw_state = atomic_load_explicit(&f->state, memory_order_acquire);
    int yk = f->yield_kind;
    if (yk == V2_YIELD_YIELD || fiber_v2_state_has_signal_pending(raw_state)) {
        /* Exchange, not store: consuming SIGNAL_PENDING must stay inside
         * the state word's RMW release chain (see sched_v2_signal), so the
         * pickup that follows this requeue acquires the signaler's prior
         * stores before the fiber re-checks its wait condition. */
        (void)atomic_exchange_explicit(&f->state, FIBER_V2_QUEUED,
                                       memory_order_acq_rel);
        sched_v2_enqueue_runnable(f);
        /* Snapshot yk before enqueue: another worker may pick the fiber
         * up and rewrite yield_kind before we classify the requeue. */
        if (yk == V2_YIELD_YIELD) {
            V2_STAT_INC(g_v2_run_yield_requeue);
        } else {
            V2_STAT_INC(g_v2_run_pending_requeue);
        }
        return;
    }

    int expected = FIBER_V2_RUNNING;
    if (!atomic_compare_exchange_strong_explicit(&f->state, &expected, FIBER_V2_PARKED,
            memory_order_acq_rel, memory_order_relaxed)) {
        int fail_state = fiber_v2_state_base(expected);
        if (fail_state >= 0 && fail_state < FIBER_V2_STATE_COUNT) {
            V2_STAT_INC(g_v2_run_commit_park_fail_state[fail_state]);
        }
        (void)atomic_exchange_explicit(&f->state, FIBER_V2_QUEUED,
                                       memory_order_acq_rel);
        sched_v2_enqueue_runnable(f);
        V2_STAT_INC(g_v2_run_pending_requeue);
        return;
    }
    V2_STAT_INC(g_v2_run_commit_parked);
}

/* ============================================================================
 * Signal: publish runnable work through state only
 * ============================================================================ */

/*
 * Signal contract: the caller published its data (release-stored a flag the
 * fiber re-checks) BEFORE calling; the fiber re-checks that flag after any
 * wake.  Two rules make this loss-proof:
 *
 *   1. Every exit lands a SUCCESSFUL RMW on f->state — even the branches
 *      that change nothing (pending already set, IDLE/DEAD drop) perform a
 *      validating CAS.  A plain-load no-op is unsound twice over: the load
 *      can race the park/requeue transitions (deciding "already pending"
 *      exactly while the fiber consumes that bit and re-checks the caller's
 *      flag it cannot yet see), and it contributes no release edge.
 *
 *   2. Together with the RMW transitions in thread_v2_run_fiber (pickup,
 *      requeue) this makes the state word an unbroken RMW release chain:
 *      the pickup's acquire that precedes the fiber's re-check observes
 *      every store made before ANY earlier signal, including ours.
 *
 * A signal on QUEUED/RUNNING sets the sticky SIGNAL_PENDING bit; pickup
 * carries it into RUNNING and the park-commit converts it to a requeue, so
 * the fiber always re-checks after the wake.  IDLE/DEAD drop (validated).
 */
void sched_v2_signal(fiber_v2* f) {
    int expected = atomic_load_explicit(&f->state, memory_order_acquire);
    for (;;) {
        int base_state = fiber_v2_state_base(expected);
        int desired;
        if (base_state == FIBER_V2_PARKED) {
            desired = FIBER_V2_QUEUED;
        } else if (base_state == FIBER_V2_QUEUED || base_state == FIBER_V2_RUNNING) {
            desired = expected | FIBER_V2_FLAG_SIGNAL_PENDING;
        } else {
            desired = expected; /* IDLE/DEAD: validated drop */
        }
        if (atomic_compare_exchange_weak_explicit(&f->state, &expected, desired,
                memory_order_acq_rel, memory_order_acquire)) {
            if (base_state == FIBER_V2_PARKED) {
                V2_STAT_INC(g_v2_signal_ok);
                sched_v2_enqueue_runnable(f);
            } else if (base_state == FIBER_V2_QUEUED || base_state == FIBER_V2_RUNNING) {
                if (fiber_v2_state_has_signal_pending(expected)) {
                    V2_STAT_INC(g_v2_signal_running_pending_already_set);
                } else {
                    V2_STAT_INC(g_v2_signal_running_pending_set);
                }
                V2_STAT_INC(g_v2_signal_pending);
            } else {
                V2_STAT_INC(g_v2_signal_dropped);
                if (base_state >= 0 && base_state < FIBER_V2_STATE_COUNT) {
                    V2_STAT_INC(g_v2_signal_dropped_state[base_state]);
                }
            }
            return;
        }
        /* CAS failure refreshed `expected`; retry against the live value. */
    }
}

/* ============================================================================
 * Park / Yield (unchanged — fiber-side API)
 * ============================================================================ */

/*
 * Yield/park entry points.
 *
 * We identify the currently-running fiber via mco_running() + user_data,
 * NOT via `tls_v2_current_fiber`.  These two *should* always agree, but
 * under optimisation — specifically @async code that alternates channel
 * sends/receives and therefore parks repeatedly — we've observed them
 * disagree on macOS arm64: mco_running() (minicoro's own __thread slot,
 * written inside _mco_prepare_jumpin on each resume) returns the correct
 * coro, while `tls_v2_current_fiber` returns a fiber_v2 that was current
 * on a *different* worker thread at some earlier point.  Passing the
 * stale f->coro to mco_yield then trips the stack-overflow check
 * (because the SP we're on is in a different coro's stack), leaving the
 * user with:
 *
 *   [mco overflow] coroutine stack overflow, try increasing the stack
 *
 * which is confusing — the stack isn't actually deep, we just passed
 * in the wrong coro.  Sourcing the coro from mco_running() makes the
 * hot path immune to that staleness because minicoro publishes
 * mco_current_co inside the resume itself, on the resuming thread,
 * just before jumping into the coro's stack.  Anything the coro then
 * reads via mco_running() is by construction a fresh, on-this-thread
 * TLS read — no callee-saved-register cache from a pre-migration
 * thread can alias it.
 *
 * Root cause of the original staleness is still under investigation:
 * static __thread file-scope variables should yield a fresh TLV
 * resolver call on each access, and the disassembly of sched_v2_yield
 * did show that pattern, yet the stale-value failure reproduced 100%
 * on an optimised build.  The CC_DEBUG_YIELD=1 diagnostic below logs
 * any remaining divergence between tls_v2_current_fiber and the
 * user_data-derived fiber so we can keep an eye on it.
 */
static _Atomic int g_sched_v2_yield_mismatches = 0;
static void sched_v2_yield_report_mismatch(const char* where,
                                            fiber_v2* tls_f,
                                            mco_coro* co,
                                            fiber_v2* real_f) {
    const char* e = getenv("CC_DEBUG_YIELD");
    if (!e || !*e || *e == '0') return;
    int n = atomic_fetch_add_explicit(&g_sched_v2_yield_mismatches, 1,
                                      memory_order_relaxed);
    if (n >= 16) return;
    fprintf(stderr,
        "[sched-v2-stale] %s: tls_fiber=%p (coro=%p) mco_running=%p "
        "user_data=%p tid=%p\n",
        where, (void*)tls_f, (void*)(tls_f ? tls_f->coro : NULL),
        (void*)co, (void*)real_f, (void*)pthread_self());
}

static inline fiber_v2* sched_v2_current_fiber_checked(const char* where,
                                                        mco_coro** out_co) {
    mco_coro* co = mco_running();
    if (!co) { *out_co = NULL; return NULL; }
    fiber_v2* f = (fiber_v2*)mco_get_user_data(co);
    if (!f) { *out_co = NULL; return NULL; }
    *out_co = co;
    if (tls_v2_current_fiber != f) {
        sched_v2_yield_report_mismatch(where, tls_v2_current_fiber, co, f);
    }
    return f;
}

void sched_v2_park(void) {
    mco_coro* co;
    fiber_v2* f = sched_v2_current_fiber_checked("park", &co);
    if (!f) return;

    V2_STAT_INC(g_v2_parks);
    {
        int tid = tls_v2_thread_id;
        if (tid >= 0 && tid < V2_MAX_THREADS) {
            atomic_fetch_add_explicit(&g_v2.threads[tid].parks, 1,
                                      memory_order_relaxed);
        }
    }
    f->yield_kind = V2_YIELD_PARK;
    mco_result res = mco_yield(co);
    if (res != MCO_SUCCESS) {
        V2_STAT_INC(g_v2_mco_yield_fail);
        fprintf(stderr, "[sched_v2] mco_yield(park) failed rc=%d\n", (int)res);
        abort();
    }
}

void sched_v2_yield(void) {
    mco_coro* co;
    fiber_v2* f = sched_v2_current_fiber_checked("yield", &co);
    if (!f) return;

    f->yield_kind = V2_YIELD_YIELD;
    mco_result res = mco_yield(co);
    if (res != MCO_SUCCESS) {
        V2_STAT_INC(g_v2_mco_yield_fail);
        fprintf(stderr, "[sched_v2] mco_yield(yield) failed rc=%d\n", (int)res);
        abort();
    }
}

void sched_v2_set_park_reason(const char* reason) {
    fiber_v2* f = sched_v2_current_fiber();
    if (!f) return;
    f->park_reason = reason;
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

/* Global count of fibers that currently have a park_deadline published.
 * Sysmon's sched_v2_wake_expired_parkers short-circuits when this is 0,
 * so the common case (no @with_deadline in flight) pays zero cost per
 * tick beyond a single relaxed load. Maintained by the set/clear pair
 * below using atomic exchange so a double-set or double-clear can't
 * desync it from the per-fiber flag. */
static _Atomic size_t g_v2_park_deadlines = 0;

/* Deadline-aware park primitives.
 *
 * Publish order: store sec/nsec, then release-exchange has_park_deadline=1.
 * Sysmon acquire-loads the flag, then loads sec/nsec — that pairs with the
 * release so the deadline sample is ordered without taking all_fibers_mu on
 * the park hot path (a global mutex here would serialize every deadline park
 * against sysmon's fiber-list walks).
 *
 * atomic_exchange on has_park_deadline keeps the global counter symmetric:
 * a repeat set does not double-increment; clear on a never-set fiber does
 * not decrement. */
void sched_v2_fiber_set_park_deadline(fiber_v2* f, const struct timespec* d) {
    if (!f || !d) return;
    atomic_store_explicit(&f->park_deadline_sec, (int64_t)d->tv_sec, memory_order_relaxed);
    atomic_store_explicit(&f->park_deadline_nsec, (int64_t)d->tv_nsec, memory_order_relaxed);
    int was = atomic_exchange_explicit(&f->has_park_deadline, 1, memory_order_release);
    if (!was) atomic_fetch_add_explicit(&g_v2_park_deadlines, 1, memory_order_relaxed);
}

void sched_v2_fiber_clear_park_deadline(fiber_v2* f) {
    if (!f) return;
    int was = atomic_exchange_explicit(&f->has_park_deadline, 0, memory_order_release);
    if (was) atomic_fetch_sub_explicit(&g_v2_park_deadlines, 1, memory_order_relaxed);
}

void sched_v2_fiber_set_par_gate(fiber_v2* f, void* gate) {
    if (f) f->par_gate = gate;
}

void* sched_v2_fiber_par_gate(fiber_v2* f) {
    return f ? f->par_gate : NULL;
}

void sched_v2_fiber_set_par_slot(fiber_v2* f, int slot) {
    if (f) f->par_slot = slot;
}

int sched_v2_fiber_par_slot(fiber_v2* f) {
    return f ? f->par_slot : -1;
}

/* Walk the all_fibers list and signal any parked fiber whose deadline
 * has passed.  Cheap when no deadlines are in flight: the global counter
 * short-circuits the walk on the first load.
 *
 * Called once per sysmon tick (see sched_v2_sysmon_main).  The resolution
 * is V2_SYSMON_INTERVAL_MS (20 ms today) — a 10 ms deadline will fire
 * 10–30 ms after being posted, which is fine for the only current caller
 * (@with_deadline wrapping blocking channel ops): tests only care about
 * seeing ETIMEDOUT, not latency. */
static void sched_v2_wake_expired_parkers(void) {
    if (atomic_load_explicit(&g_v2_park_deadlines, memory_order_relaxed) == 0) {
        return;
    }
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    pthread_mutex_lock(&g_v2.all_fibers_mu);
    for (fiber_v2* f = g_v2.all_fibers; f; f = f->all_next) {
        if (!atomic_load_explicit(&f->has_park_deadline, memory_order_acquire)) continue;
        int base = fiber_v2_state_base(
            atomic_load_explicit(&f->state, memory_order_acquire));
        if (base != FIBER_V2_PARKED) continue;
        /* Sample after the acquire on has_park_deadline so a first publish
         * is ordered. A concurrent rewrite while has==1 can tear sec/nsec;
         * at worst that delays or advances one sysmon tick — the fiber's
         * post-park clock check is authoritative. */
        int64_t sec = atomic_load_explicit(&f->park_deadline_sec, memory_order_relaxed);
        int64_t nsec = atomic_load_explicit(&f->park_deadline_nsec, memory_order_relaxed);
        if (!atomic_load_explicit(&f->has_park_deadline, memory_order_acquire)) continue;
        if ((int64_t)now.tv_sec < sec ||
            ((int64_t)now.tv_sec == sec && (int64_t)now.tv_nsec < nsec)) {
            continue;
        }
        /* Expired.  Signal via the normal unpark path; the fiber will
         * resume, its post-park deadline check will re-read the clock,
         * see expiry, and return timeout up to the channel layer.  We
         * don't clear has_park_deadline here — the fiber clears it on
         * its own resume side, which keeps the increment/decrement
         * accounting on a single owner. */
        sched_v2_signal(f);
    }
    pthread_mutex_unlock(&g_v2.all_fibers_mu);
}

void sched_v2_fiber_inc_deadlock_suppress(fiber_v2* f) {
    if (!f) return;
    if (atomic_load_explicit(&f->deadlock_suppress_depth, memory_order_relaxed) < UINT32_MAX)
        atomic_fetch_add_explicit(&f->deadlock_suppress_depth, 1u, memory_order_acq_rel);
}

void sched_v2_fiber_dec_deadlock_suppress(fiber_v2* f) {
    if (!f) return;
    if (atomic_load_explicit(&f->deadlock_suppress_depth, memory_order_relaxed) > 0)
        atomic_fetch_sub_explicit(&f->deadlock_suppress_depth, 1u, memory_order_acq_rel);
}

int sched_v2_fiber_deadlock_suppressed(fiber_v2* f) {
    if (!f) return 0;
    return atomic_load_explicit(&f->deadlock_suppress_depth, memory_order_acquire) > 0;
}

void sched_v2_fiber_inc_external_wait(fiber_v2* f) {
    if (!f) return;
    if (atomic_load_explicit(&f->external_wait_depth, memory_order_relaxed) < UINT32_MAX)
        atomic_fetch_add_explicit(&f->external_wait_depth, 1u, memory_order_acq_rel);
}

void sched_v2_fiber_dec_external_wait(fiber_v2* f) {
    if (!f) return;
    if (atomic_load_explicit(&f->external_wait_depth, memory_order_relaxed) > 0)
        atomic_fetch_sub_explicit(&f->external_wait_depth, 1u, memory_order_acq_rel);
}

/* R1 — record/read user-facing async task naming on a fiber.
 *
 * The setter is called once per fiber, by `cc__nursery_async_runner`
 * right after the runner is entered (so it executes on the new fiber's
 * own context).  Strings are NOT copied — callers pass C string literals
 * embedded in the lowered output and live for the program's duration.
 * The getter is hit by `cc_rt_diag_current_async_info`; it returns 1 if
 * any field is populated, 0 otherwise. */
void sched_v2_fiber_set_diag_name(fiber_v2* f, const char* name,
                                   const char* file, int line) {
    if (!f) return;
    f->diag_user_name = name;
    f->diag_file = file;
    f->diag_line = line;
}

int sched_v2_fiber_get_diag_name(fiber_v2* f, const char** out_name,
                                  const char** out_file, int* out_line) {
    if (!f) return 0;
    if (!f->diag_user_name && !f->diag_file) return 0;
    if (out_name) *out_name = f->diag_user_name;
    if (out_file) *out_file = f->diag_file;
    if (out_line) *out_line = f->diag_line;
    return 1;
}

int sched_v2_fiber_external_wait_active(fiber_v2* f) {
    if (!f) return 0;
    return atomic_load_explicit(&f->external_wait_depth, memory_order_acquire) > 0;
}

/* ============================================================================
 * Context queries
 * ============================================================================ */

int sched_v2_in_context(void) {
    /* mco_running() only. tls_v2_current_fiber can be a stale fiber
     * from another worker (macOS arm64 + opt). Park already ignores it;
     * exclusive lock used the TLS fallback and SIGBUS'd on a dead entry. */
    return mco_running() != NULL;
}

fiber_v2* sched_v2_current_fiber(void) {
    mco_coro* co = mco_running();
    if (!co)
        return NULL;
    return (fiber_v2*)mco_get_user_data(co);
}

CCNurseryHost* sched_v2_current_nursery(void) {
    fiber_v2* f = sched_v2_current_fiber();
    return f ? f->saved_nursery : NULL;
}

void* sched_v2_current_deadline_scope(void) {
    fiber_v2* f = sched_v2_current_fiber();
    return f ? f->current_deadline_scope : NULL;
}

void* sched_v2_deadline_scope_push(void* d) {
    fiber_v2* f = sched_v2_current_fiber();
    if (!f) return NULL;
    void* prev = f->current_deadline_scope;
    f->current_deadline_scope = d;
    return prev;
}

void sched_v2_deadline_scope_pop(void* prev) {
    fiber_v2* f = sched_v2_current_fiber();
    if (!f) return;
    f->current_deadline_scope = prev;
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

    /* Startup-park for extras (see CC_V2_PARK_EXTRAS_AT_STARTUP). Non-primary
     * workers (tid != 0) skip the initial self-drain and park immediately.
     * They are admitted on demand by sched_v2_wake(-1) when the primary
     * cannot drain fast enough. */
    if (g_v2_park_extras_at_startup && tid != 0) {
        /* Read the wake ticket BEFORE publishing is_idle (see the main
         * park below for why). */
        uint32_t val = atomic_load_explicit(&g_v2.threads[tid].wake.value,
                                            memory_order_acquire);
        atomic_store_explicit(&g_v2.threads[tid].is_idle, 1, memory_order_release);
        atomic_fetch_add_explicit(&g_v2.idle_workers, 1, memory_order_acq_rel);
        V2_STAT_INC(g_v2_worker_idle_entries);
        atomic_thread_fence(memory_order_seq_cst);
        /* Queue is empty at startup so no Dekker recheck needed. */
        wake_primitive_wait(&g_v2.threads[tid].wake, val);
        if (atomic_exchange_explicit(&g_v2.threads[tid].is_idle, 0,
                                     memory_order_acq_rel)) {
            atomic_fetch_sub_explicit(&g_v2.idle_workers, 1,
                                      memory_order_acq_rel);
        }
    }

    while (atomic_load_explicit(&g_v2.running, memory_order_acquire)) {
        /* Admission gate: try to register as one of the (up to)
         * g_v2_target_active running workers. If target is disabled
         * (0), try_admit_running always succeeds. If target is saturated,
         * we skip the drain and go straight to the park cycle — extras
         * that got woken by sched_v2_wake(-1)'s producer-side loop
         * bow out without burning cycles. */
        if (!try_admit_running()) {
            uint32_t val = atomic_load_explicit(&g_v2.threads[tid].wake.value,
                                                memory_order_acquire);
            atomic_store_explicit(&g_v2.threads[tid].is_idle, 1,
                                  memory_order_release);
            atomic_fetch_add_explicit(&g_v2.idle_workers, 1,
                                      memory_order_acq_rel);
            V2_STAT_INC(g_v2_worker_idle_entries);
            atomic_thread_fence(memory_order_seq_cst);
            /* No Dekker recheck here: we're parked because target is
             * saturated, not because the queue was empty. Pure wait. */
            wake_primitive_wait(&g_v2.threads[tid].wake, val);
            if (atomic_exchange_explicit(&g_v2.threads[tid].is_idle, 0,
                                         memory_order_acq_rel)) {
                atomic_fetch_sub_explicit(&g_v2.idle_workers, 1,
                                          memory_order_acq_rel);
            }
            continue;
        }

        /* Drain: run ready fibers inline until queue is empty (or sysmon
         * evicted us, in which case sched_v2_wake returns early). */
        sched_v2_wake(tid);

        if (!atomic_load_explicit(&g_v2.running, memory_order_acquire)) {
            deadmit_running();
            break;
        }

        /* End-of-loop identity check: if slot.generation has moved, we are
         * an orphan. Exit before re-parking on slot.wake (which now belongs
         * to the replacement worker). Our kidnapped fiber has already
         * returned by now; the OS thread simply dies. */
        if (atomic_load_explicit(&g_v2.threads[tid].generation,
                                 memory_order_acquire)
            != tls_v2_my_generation) {
            deadmit_running();
            break;
        }

        /* Drain exhausted. Release our admission slot so another worker
         * can be admitted on the next wake. */
        deadmit_running();

        /* No work — let this BUSY worker become IDLE.
         * Dekker protocol: mark idle, read wake counter, recheck, sleep.
         *
         * The store-load pair (mark is_idle=1 ; load queue.count) needs a
         * full seq_cst fence on arm64 — release-store + acquire-load alone
         * allows the architecture to reorder the load before the store,
         * which would race with the producer's symmetric "push count ; load
         * idle_workers" and leave a task stranded while the worker sleeps.
         * See sched_v2_wake for the matching fence on the producer side.
         *
         * The wake ticket is read BEFORE is_idle is published. A waker
         * that finds is_idle==1 CASes it to 0, bumps wake.value and issues
         * the wake. If we read the ticket after that bump we would park on
         * the new value with is_idle already 0: not idle, not running,
         * and no waker will ever look at us again — a worker lost for the
         * life of the process. Read first, and a wake that lands after
         * the read makes wake_primitive_wait return at once. */
        uint32_t val = atomic_load_explicit(&g_v2.threads[tid].wake.value,
                                            memory_order_acquire);
        atomic_store_explicit(&g_v2.threads[tid].is_idle, 1, memory_order_release);
        atomic_fetch_add_explicit(&g_v2.idle_workers, 1, memory_order_acq_rel);
        V2_STAT_INC(g_v2_worker_idle_entries);
        atomic_thread_fence(memory_order_seq_cst);

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

static uint64_t sched_v2_park_sum(void) {
    int n = atomic_load_explicit(&g_v2.num_threads, memory_order_acquire);
    uint64_t s = 0;
    for (int i = 0; i < n; i++) {
        s += atomic_load_explicit(&g_v2.threads[i].parks, memory_order_relaxed);
    }
    return s;
}

/* Run-to-park multiplexing: more than half the episode's pops parked.
 * dp_pops == 0 is CPU saturation between pops, not a park wave. */
static int sched_v2_grow_run_to_park(uint64_t dp_pops, uint64_t dp_parks) {
    return dp_pops > 0 && dp_parks * 2ull > dp_pops;
}

int sched_v2_live_workers(void) {
    sched_v2_ensure_init();
    return atomic_load_explicit(&g_v2.num_threads, memory_order_acquire);
}

int sched_v2_max_workers(void) {
    sched_v2_ensure_init();
    return g_v2.max_threads;
}

static int sched_v2_count_idle_workers(void);

int sched_v2_idle_workers(void) {
    sched_v2_ensure_init();
    return sched_v2_count_idle_workers();
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
 *   2. reset per-slot transient state (wake, is_idle, dispatch_epoch).
 *   3. bump slot.generation (the identity token the new thread will read
 *      at entry into tls_v2_my_generation).
 *   4. pthread_create(&slot.handle, ..., tid) — writes new handle AND
 *      releases all prior stores to the new thread.
 */
static void sched_v2_sysmon_evict_worker_in_place(int i) {
    pthread_t old_handle = g_v2.threads[i].handle;
    pthread_detach(old_handle);

    /* The orphan is mid-syscall, not parked on slot.wake (dispatch_epoch
     * being nonzero is what qualified it for eviction). Resetting wake is
     * safe and gives the new worker a fresh counter. */
    wake_primitive_init(&g_v2.threads[i].wake);
    atomic_store_explicit(&g_v2.threads[i].is_idle, 0, memory_order_relaxed);
    atomic_store_explicit(&g_v2.threads[i].dispatch_epoch, 0,
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
/* Sysmon-local cache of per-worker dispatch epoch seen on the previous
 * tick. Sole reader/writer is sched_v2_sysmon_evict_aged_workers, which
 * runs on the single sysmon thread — no atomics needed. A worker whose
 * current epoch equals the cached value AND is non-zero has been on the
 * same fiber across at least one full sysmon tick (>= 20 ms today) and
 * is flagged for in-place eviction.
 *
 * Slot replacement (sched_v2_sysmon_evict_worker_in_place) resets
 * dispatch_epoch to 0 and the replacement worker starts a fresh TLS
 * counter, so the stale cache entry naturally clears on the next tick. */
static uint64_t g_v2_sysmon_last_epoch[V2_MAX_THREADS];

static void sched_v2_sysmon_evict_aged_workers(void) {
    if (!atomic_load_explicit(&g_v2_sysmon_detach_enabled,
                              memory_order_relaxed)) {
        return;
    }
    size_t backlog = atomic_load_explicit(&g_v2.ready_queue.count,
                                          memory_order_relaxed);
    /* Zero backlog: still refresh the cache so we don't accidentally
     * flag a worker that happened to run a long fiber during a quiet
     * window and is now between dispatches. The scan is essentially
     * free (N loads from a contiguous array). */
    int n = atomic_load_explicit(&g_v2.num_threads, memory_order_acquire);
    if (backlog == 0) {
        for (int i = 0; i < n; i++) {
            g_v2_sysmon_last_epoch[i] = atomic_load_explicit(
                &g_v2.threads[i].dispatch_epoch, memory_order_relaxed);
        }
        return;
    }

    size_t budget = backlog;
    for (int i = 0; i < n; i++) {
        uint64_t cur = atomic_load_explicit(&g_v2.threads[i].dispatch_epoch,
                                            memory_order_relaxed);
        uint64_t prev = g_v2_sysmon_last_epoch[i];
        g_v2_sysmon_last_epoch[i] = cur;

        if (budget == 0) continue;          /* keep refreshing the cache */
        if (cur == 0) continue;              /* no fiber running */
        if (cur != prev) continue;           /* dispatched something new */

        int64_t live_orphans = atomic_load_explicit(&g_v2_orphans_alive,
                                                    memory_order_relaxed);
        if (live_orphans >= V2_ORPHAN_SAFETY_CAP) {
            atomic_fetch_add_explicit(&g_v2_orphans_cap_hit, 1,
                                      memory_order_relaxed);
            continue;
        }

        sched_v2_sysmon_evict_worker_in_place(i);
        /* Eviction resets the slot's epoch to 0; clear our cache so the
         * replacement worker starts from a clean state. */
        g_v2_sysmon_last_epoch[i] = 0;
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
    /* Pool-growth recheck state (see g_v2_grow_pending block comment). */
    uint64_t grow_base_pops = 0;
    uint64_t grow_base_parks = 0;
    uint64_t grow_base_ns = 0;
    int grow_have_baseline = 0;
    int grow_depth_streak = 0;
    uint64_t grow_slack_since_ns = 0;
    int slow_prev_backlog = 0;
    uint64_t last_slow_ns = 0;
    v2_mach_tb_init_once();
    while (atomic_load_explicit(&g_v2.running, memory_order_acquire)) {
        uint32_t val = atomic_load_explicit(&g_v2.sysmon_wake.value, memory_order_acquire);
        if (atomic_load_explicit(&g_v2_grow_pending, memory_order_acquire)) {
            wake_primitive_wait_timeout_us(&g_v2.sysmon_wake, val,
                                           (uint32_t)g_v2_grow_recheck_us);
        } else {
            wake_primitive_wait_timeout(&g_v2.sysmon_wake, val, V2_SYSMON_INTERVAL_MS);
        }

        if (!atomic_load_explicit(&g_v2.running, memory_order_acquire)) break;

        /* CC_V2_IDLE_TRACE: every 100ms, the idle counter against the
         * per-slot is_idle flags and the running count. A worker parked
         * with is_idle=0 and not running is a lost worker. */
        {
            static int trace = -1;
            static uint64_t last_ns = 0;
            if (trace < 0) trace = getenv("CC_V2_IDLE_TRACE") ? 1 : 0;
            if (trace) {
                uint64_t now = v2_now_ns();
                if (now - last_ns > 100000000ull) {
                    int n = atomic_load_explicit(&g_v2.num_threads, memory_order_relaxed);
                    int real_idle = sched_v2_count_idle_workers();
                    fprintf(stderr, "[idle-trace] idle_workers=%d is_idle=%d/%d running=%d queue=%zu grow_pending=%d\n",
                            atomic_load_explicit(&g_v2.idle_workers, memory_order_relaxed),
                            real_idle, n,
                            atomic_load_explicit(&g_v2_running_workers, memory_order_relaxed),
                            atomic_load_explicit(&g_v2.ready_queue.count, memory_order_relaxed),
                            atomic_load_explicit(&g_v2_grow_pending, memory_order_relaxed));
                    last_ns = now;
                }
            }
        }

        /* Deferred pool growth: decide grow/hold once per recheck window. */
        if (atomic_load_explicit(&g_v2_grow_pending, memory_order_acquire)) {
            size_t depth = atomic_load_explicit(&g_v2.ready_queue.count,
                                                memory_order_relaxed);
            int n = atomic_load_explicit(&g_v2.num_threads, memory_order_acquire);
            int admit_ok = (g_v2_target_active <= 0 ||
                            atomic_load_explicit(&g_v2_running_workers,
                                                 memory_order_acquire)
                                < g_v2_target_active);
            int idle = atomic_load_explicit(&g_v2.idle_workers,
                                            memory_order_acquire);
            int slack_now = (depth == 0 && idle > 0);
            uint64_t slack_now_ns = 0;
            int slack_done = 0;
            if (slack_now) {
                slack_now_ns = v2_now_ns();
                if (grow_slack_since_ns == 0)
                    grow_slack_since_ns = slack_now_ns;
                slack_done = (idle >= n) ||
                    (slack_now_ns - grow_slack_since_ns >= SCHED_V2_GROW_SLACK_NS);
            } else {
                grow_slack_since_ns = 0;
            }
            if (n >= g_v2.max_threads || !admit_ok || slack_done) {
                atomic_store_explicit(&g_v2_grow_pending, 0, memory_order_release);
                grow_have_baseline = 0;
                grow_depth_streak = 0;
                grow_slack_since_ns = 0;
            } else {
                uint64_t pops = atomic_load_explicit(&g_v2.ready_queue.pops,
                                                     memory_order_relaxed);
                uint64_t parks = sched_v2_park_sum();
                uint64_t now = v2_now_ns();
                if (!grow_have_baseline) {
                    /* One baseline per demand episode, captured on the
                     * first recheck (single writer; a producer-side write
                     * would be clobbered by every push at saturation).
                     * The rate below is cumulative since episode start. */
                    grow_base_pops = pops;
                    grow_base_parks = parks;
                    grow_base_ns = now;
                    grow_have_baseline = 1;
                } else {
                    uint64_t dp = pops - grow_base_pops;
                    uint64_t dpark = parks - grow_base_parks;
                    uint64_t dt = now - grow_base_ns;
                    /* Need a long-enough sample for the rate to mean
                     * anything; an early/spurious wake defers the decision
                     * to the next recheck. The bar is one pop per worker
                     * per grow_rate_us, so the sample must span at least
                     * that period: in a shorter window a healthy pool is
                     * expected to show zero pops, which the test would
                     * read as a stall (and run_to_park cannot answer at
                     * dp == 0). */
                    if (dt >= (uint64_t)g_v2_grow_rate_us * 1000ull) {
                        int run_to_park = sched_v2_grow_run_to_park(dp, dpark);
                        int rate_slow = (dp * (uint64_t)g_v2_grow_rate_us
                                         * 1000ull < (uint64_t)n * dt);
                        int depth_hit = (g_v2_grow_depth_mult > 0 &&
                                         depth >= (size_t)(g_v2_grow_depth_mult * n));
                        /* Grow on rate: cumulative drain rate below one
                         * pop per worker per grow_rate_us. Computed as
                         * dp/dt < n/(rate_us*1000), cross-multiplied to
                         * stay in integers. Depth is the same backlog
                         * test, gated on run-to-park so an accept or
                         * request wave does not walk ncpu. */
                        /* Depth must persist (see g_v2_grow_depth_dwell). */
                        if (depth_hit && !run_to_park)
                            grow_depth_streak++;
                        else
                            grow_depth_streak = 0;
                        int depth_grow = depth_hit && !run_to_park &&
                                         grow_depth_streak >= g_v2_grow_depth_dwell;
                        if (rate_slow && !run_to_park) {
                            V2_STAT_INC(g_v2_grow_stall);
                            grow_depth_streak = 0;
                            (void)sched_v2_try_expand_pool();
                        } else if (depth_grow) {
                            V2_STAT_INC(g_v2_grow_backlog);
                            grow_depth_streak = 0;
                            (void)sched_v2_try_expand_pool();
                        } else if (run_to_park && (rate_slow || depth_hit)) {
                            V2_STAT_INC(g_v2_grow_parked);
                        } else {
                            V2_STAT_INC(g_v2_grow_held);
                        }
                    }
                }
            }
        } else {
            grow_have_baseline = 0;
            grow_depth_streak = 0;
            grow_slack_since_ns = 0;
        }

        /* Everything below assumes ~one V2_SYSMON_INTERVAL_MS between
         * runs (eviction ages workers by ticks; stall diagnostics count
         * ticks). During a growth episode the loop spins at recheck
         * cadence, so gate the slow jobs on real elapsed time. */
        uint64_t now_ns = v2_now_ns();
        if (now_ns - last_slow_ns < (uint64_t)V2_SYSMON_INTERVAL_MS * 1000000ull) {
            continue;
        }
        last_slow_ns = now_ns;

        /* Syscall-age eviction runs every tick: cheap scan, high payoff. */
        sched_v2_sysmon_evict_aged_workers();

        /* Park-deadline wakeup: signal any fiber whose @with_deadline
         * has expired while it was parked.  Short-circuits to a single
         * relaxed load when no deadlines are in flight. */
        sched_v2_wake_expired_parkers();

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

        /* Sustained-backlog escalation (growth bias): a queue that stayed
         * non-empty across two consecutive slow ticks with nobody idle
         * gets one more worker per tick, even if the recheck's rate test
         * keeps holding. Steady contention churn and steady short-task
         * parallelism are indistinguishable from queue dynamics alone;
         * biasing toward growth bounds the worst case at legacy behavior
         * (full pool), reached in tens of ms. */
        if (g_v2_grow_escalate_ticks > 0) {
            size_t d = atomic_load_explicit(&g_v2.ready_queue.count,
                                            memory_order_relaxed);
            int n = atomic_load_explicit(&g_v2.num_threads, memory_order_acquire);
            int admit_ok = (g_v2_target_active <= 0 ||
                            atomic_load_explicit(&g_v2_running_workers,
                                                 memory_order_acquire)
                                < g_v2_target_active);
            if (d > 0 &&
                atomic_load_explicit(&g_v2.idle_workers, memory_order_relaxed) == 0) {
                slow_prev_backlog++;
            } else {
                slow_prev_backlog = 0;
            }
            if (slow_prev_backlog >= g_v2_grow_escalate_ticks &&
                n < g_v2.max_threads && admit_ok) {
                V2_STAT_INC(g_v2_grow_escalate);
                (void)sched_v2_try_expand_pool();
            }
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
                uint64_t parked_internal = 0;
                uint64_t parked_external_wait = 0;
                uint64_t parked_deadlock_suppressed = 0;
                park_reason_bucket reason_buckets[V2_DIAG_REASON_BUCKETS];
                size_t reason_bucket_count = 0;
                sched_v2_diag_scan_fibers(state_counts,
                                          &parked_wait_many,
                                          &parked_wait,
                                          &parked_other,
                                          &parked_unknown,
                                          &parked_internal,
                                          &parked_external_wait,
                                          &parked_deadlock_suppressed,
                                          reason_buckets,
                                          &reason_bucket_count);
                size_t external_threads = atomic_load_explicit(&g_external_wait_threads,
                                                               memory_order_relaxed);
                uint64_t scan_live = state_counts[FIBER_V2_QUEUED] +
                                    state_counts[FIBER_V2_RUNNING] +
                                    state_counts[FIBER_V2_PARKED];
                /* Stall is "a waiter exists and no resume." IDLE leftover
                 * slots after a join are not waiters. Skip only resets the
                 * clock: leaving stall_ticks at the print threshold makes
                 * the next wave inherit a hot clock and fire on first tick. */
                if (gq == 0 && scan_live == 0) {
                    stall_ticks = 0;
                    continue;
                }
                /* Listen / accept: no runnable work, and every parked fiber
                 * is an external_wait / suppress scope (or the host thread
                 * is in cc_external_wait_enter). Same clock reset. */
                if (gq == 0 &&
                    state_counts[FIBER_V2_QUEUED] == 0 &&
                    state_counts[FIBER_V2_RUNNING] == 0 &&
                    parked_internal == 0 &&
                    (parked_external_wait > 0 ||
                     parked_deadlock_suppressed > 0 ||
                     external_threads > 0)) {
                    stall_ticks = 0;
                    continue;
                }
                /* fibers_alive is only maintained under CC_V2_STATS=1; when
                 * stats are off it reads 0 even with live parked fibers.
                 * Prefer the scan total so the banner is not actively wrong. */
                uint64_t alive_report = cc_v2_stats_enabled() ? alive : scan_live;
                fprintf(stderr, "[sched_v2 sysmon] STALL #%d: threads=%d idle=%d global_q=%zu fibers_alive=%llu%s\n",
                        stall_ticks / STALL_DIAG_TICKS, n, idle_n, gq,
                        (unsigned long long)alive_report,
                        cc_v2_stats_enabled() ? "" : " (scan)");
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
                fprintf(stderr,
                        "  deadlock classification: internal=%llu external_wait=%llu "
                        "deadlock_suppressed=%llu external_wait_threads=%zu%s\n",
                        (unsigned long long)parked_internal,
                        (unsigned long long)parked_external_wait,
                        (unsigned long long)parked_deadlock_suppressed,
                        external_threads,
                        (parked_external_wait || external_threads)
                            ? " (external waits can suppress deadlock verdicts)"
                            : "");
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
                    "spawns: parks=%llu dead=%llu  pool=%d/%d\n",
            (unsigned long long)atomic_load_explicit(&g_v2_coro_reuse, memory_order_relaxed),
            (unsigned long long)atomic_load_explicit(&g_v2_coro_fresh, memory_order_relaxed),
            (unsigned long long)atomic_load_explicit(&g_v2_parks, memory_order_relaxed),
            (unsigned long long)atomic_load_explicit(&g_v2_run_dead, memory_order_relaxed),
            atomic_load_explicit(&g_v2_pooled_coros, memory_order_relaxed),
            g_v2_coro_pool_max);
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
    fprintf(stderr, "[sched_v2 stats] grow (eager<=%d recheck=%dus rate=%dus/pop/worker depth_x=%d dwell=%d esc=%d): requests=%llu "
                    "stall=%llu backlog=%llu escalate=%llu held=%llu parked=%llu final_threads=%d/%d\n",
            g_v2_eager_threads, g_v2_grow_recheck_us, g_v2_grow_rate_us,
            g_v2_grow_depth_mult, g_v2_grow_depth_dwell, g_v2_grow_escalate_ticks,
            (unsigned long long)atomic_load_explicit(&g_v2_grow_requests, memory_order_relaxed),
            (unsigned long long)atomic_load_explicit(&g_v2_grow_stall, memory_order_relaxed),
            (unsigned long long)atomic_load_explicit(&g_v2_grow_backlog, memory_order_relaxed),
            (unsigned long long)atomic_load_explicit(&g_v2_grow_escalate, memory_order_relaxed),
            (unsigned long long)atomic_load_explicit(&g_v2_grow_held, memory_order_relaxed),
            (unsigned long long)atomic_load_explicit(&g_v2_grow_parked, memory_order_relaxed),
            atomic_load_explicit(&g_v2.num_threads, memory_order_relaxed),
            g_v2.max_threads);
    fprintf(stderr, "[sched_v2 stats] spin_before_park=%d: hit=%llu miss=%llu\n",
            g_v2_spin_before_park,
            (unsigned long long)atomic_load_explicit(&g_v2_worker_spin_hit, memory_order_relaxed),
            (unsigned long long)atomic_load_explicit(&g_v2_worker_spin_miss, memory_order_relaxed));
    fprintf(stderr, "[sched_v2 stats] target_active=%d running=%d: "
                    "admit_ok=%llu admit_fail=%llu wake_gated=%llu\n",
            g_v2_target_active,
            atomic_load_explicit(&g_v2_running_workers, memory_order_relaxed),
            (unsigned long long)atomic_load_explicit(&g_v2_worker_admit_ok, memory_order_relaxed),
            (unsigned long long)atomic_load_explicit(&g_v2_worker_admit_fail, memory_order_relaxed),
            (unsigned long long)atomic_load_explicit(&g_v2_wake_gated_target, memory_order_relaxed));
    fprintf(stderr, "[sched_v2 stats] sysmon_evict: evicted_total=%llu orphans_alive=%lld cap_hit=%llu\n",
            (unsigned long long)atomic_load_explicit(&g_v2_sysmon_evicted_total, memory_order_relaxed),
            (long long)atomic_load_explicit(&g_v2_orphans_alive, memory_order_relaxed),
            (unsigned long long)atomic_load_explicit(&g_v2_orphans_cap_hit, memory_order_relaxed));
}

static void sched_v2_init_impl(void) {
    /* Prime the mach timebase cache before any worker calls v2_now_ns on
     * the hot path. On arm64 macOS this is 1/1 (no-op fast path); still
     * cheaper to do once than to branch-check on every call. */
    v2_mach_tb_init_once();
    atomic_store_explicit(&g_v2.running, 1, memory_order_release);
    atomic_store_explicit(&g_v2.idle_workers, 0, memory_order_relaxed);
    v2_queue_init(&g_v2.ready_queue);
    /* Publish the ready-depth cell for the lowering's inline @parallel
     * deny gate (cc_sched.cch). _Atomic size_t read as volatile size_t:
     * same object representation; the gate only needs a relaxed load. */
    __cc_par_depth_addr = (volatile size_t*)&g_v2.ready_queue.count;
    v2_slock_init(&g_v2.free_list_mu);
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
    const char* sbp_env = getenv("CC_V2_SPIN_BEFORE_PARK");
    if (sbp_env) {
        char* end = NULL;
        long v = strtol(sbp_env, &end, 10);
        if (end != sbp_env && v >= 0 && v <= 1000000000L) {
            g_v2_spin_before_park = (int)v;
        }
    }
    /* Real lazy start supersedes the old "create everything, then park
     * extras" startup mode. Keep the env knob inert for compatibility:
     * additional workers are now created only on demand. */
    g_v2_park_extras_at_startup = 0;
    const char* tgt_env = getenv("CC_V2_TARGET_ACTIVE");
    if (tgt_env) {
        char* end = NULL;
        long v = strtol(tgt_env, &end, 10);
        if (end != tgt_env && v >= 0 && v <= 1024) {
            g_v2_target_active = (int)v;
        }
    }
    /* Inline pthread_create cap for the push path; growth beyond this is
     * deferred to sysmon's recheck. Set to the core count (or higher) to
     * restore legacy fully-inline growth. */
    const char* eager_env = getenv("CC_V2_EAGER_THREADS");
    if (eager_env) {
        char* end = NULL;
        long v = strtol(eager_env, &end, 10);
        if (end != eager_env && v >= 0 && v <= 1024) {
            g_v2_eager_threads = (int)v;
        }
    }
    const char* grow_env = getenv("CC_V2_GROW_RECHECK_US");
    if (grow_env) {
        char* end = NULL;
        long v = strtol(grow_env, &end, 10);
        if (end != grow_env && v >= 1 && v <= 1000000L) {
            g_v2_grow_recheck_us = (int)v;
        }
    }
    const char* rate_env = getenv("CC_V2_GROW_RATE_US");
    if (rate_env) {
        char* end = NULL;
        long v = strtol(rate_env, &end, 10);
        if (end != rate_env && v >= 1 && v <= 1000000L) {
            g_v2_grow_rate_us = (int)v;
        }
    }
    const char* depth_env = getenv("CC_V2_GROW_DEPTH_X");
    if (depth_env) {
        char* end = NULL;
        long v = strtol(depth_env, &end, 10);
        if (end != depth_env && v >= 0 && v <= 100000L) {
            g_v2_grow_depth_mult = (int)v;
        }
    }
    const char* dwell_env = getenv("CC_V2_GROW_DEPTH_DWELL");
    if (dwell_env) {
        char* end = NULL;
        long v = strtol(dwell_env, &end, 10);
        if (end != dwell_env && v >= 1 && v <= 10000L) {
            g_v2_grow_depth_dwell = (int)v;
        }
    }
    const char* esc_env = getenv("CC_V2_GROW_ESCALATE_TICKS");
    if (esc_env) {
        char* end = NULL;
        long v = strtol(esc_env, &end, 10);
        if (end != esc_env && v >= 0 && v <= 10000L) {
            g_v2_grow_escalate_ticks = (int)v;
        }
    }
    /* Coro-pool high-water cap. 0 disables the cap (legacy unbounded pool). */
    const char* pool_env = getenv("CC_V2_CORO_POOL_MAX");
    if (pool_env) {
        char* end = NULL;
        long v = strtol(pool_env, &end, 10);
        if (end != pool_env && v >= 0 && v <= (1L << 20)) {
            g_v2_coro_pool_max = (int)v;
        }
    }

    g_v2.max_threads = sched_v2_detect_num_threads();
    atomic_store_explicit(&g_v2.num_threads, 0, memory_order_release);
    g_v2.allow_expand = 1;
    pthread_mutex_init(&g_v2.start_mu, NULL);
    sched_v2_init_worker_slot(0);
    /* pthread_create is the happens-before to the new threads; store first. */
    g_v2.initialized = 1;
    if (pthread_create(&g_v2.threads[0].handle, NULL,
                       thread_v2_main, (void*)(intptr_t)0) == 0) {
        atomic_store_explicit(&g_v2.num_threads, 1, memory_order_release);
    }

    pthread_create(&g_v2.sysmon_handle, NULL, sched_v2_sysmon_main, NULL);
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
    pthread_mutex_destroy(&g_v2.start_mu);

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
    atomic_store_explicit(&g_v2.free_list_bare, NULL, memory_order_relaxed);
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

size_t sched_v2_ready_depth(void) {
    return atomic_load_explicit(&g_v2.ready_queue.count, memory_order_relaxed);
}

uint32_t sched_v2_current_fiber_suspends(void) {
    fiber_v2* f = sched_v2_current_fiber();
    return f ? atomic_load_explicit(&f->suspends, memory_order_relaxed) : 0;
}

void sched_v2_fiber_release(fiber_v2* f) {
    if (f) fiber_v2_free(f);
}

void* sched_v2_current_result_buf(size_t size) {
    fiber_v2* f = sched_v2_current_fiber();
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

int cc__chan_debug_is_open(void* ch_obj);
void cc__chan_debug_dump_state(void* ch_obj, const char* prefix);
/* R2 — forward decl for the channel diag-meta reader (defined in
 * `channel.c` where the `CCChan` layout is visible). */
struct CCChan;
int cc_chan_get_diag_meta(const struct CCChan* ch, const char** out_name,
                          const char** out_file, int* out_line);

static _Atomic uint64_t g_v2_deadlock_first_seen = 0;
static _Atomic int g_v2_deadlock_reported = 0;

/* Persist-time before declaring deadlock. 1 s matches V1; enough to ride
 * out cc_block_all startup transients. Override with CC_DEADLOCK_PERSIST_MS
 * (milliseconds) for scoped detector smokes that would otherwise spend
 * most of their wall time waiting out the latch. */
#define SCHED_V2_DEADLOCK_PERSIST_MS 1000u

static uint64_t sched_v2_deadlock_persist_ms(void) {
    const char* env = getenv("CC_DEADLOCK_PERSIST_MS");
    if (!env || !env[0]) return SCHED_V2_DEADLOCK_PERSIST_MS;
    char* end = NULL;
    unsigned long v = strtoul(env, &end, 10);
    if (end == env || (end && *end) || v == 0 || v > 60000ul)
        return SCHED_V2_DEADLOCK_PERSIST_MS;
    return (uint64_t)v;
}

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
        if (atomic_load_explicit(&f->external_wait_depth, memory_order_acquire) > 0) { (*external_parked)++; continue; }
        if (atomic_load_explicit(&f->deadlock_suppress_depth, memory_order_acquire) > 0) { (*suppressed_parked)++; continue; }
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
        if (is_parked && (atomic_load_explicit(&f->external_wait_depth, memory_order_acquire) > 0 ||
                          atomic_load_explicit(&f->deadlock_suppress_depth, memory_order_acquire) > 0)) {
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
                    (unsigned)atomic_load_explicit(&f->external_wait_depth, memory_order_acquire),
                    (unsigned)atomic_load_explicit(&f->deadlock_suppress_depth, memory_order_acquire));
            if (is_parked && !skipped && f->park_obj && f->park_reason &&
                strncmp(f->park_reason, "chan_", 5) == 0) {
                cc__chan_debug_dump_state(f->park_obj, "    chan state: ");
                /* R2 — when the park is on a channel, surface its
                 * user-facing name + creation-site source location so
                 * the deadlock dump points back at the user's `tx,rx`
                 * (or owned single-handle) declaration instead of just
                 * a raw `CCChan*`.  Channels created outside the
                 * lowered path (e.g. direct `cc_chan_create` from C
                 * tests) have no meta — silently skipped. */
                const char* chan_name = NULL;
                const char* chan_file = NULL;
                int chan_line = 0;
                if (cc_chan_get_diag_meta((const struct CCChan*)f->park_obj,
                                          &chan_name, &chan_file, &chan_line)) {
                    fprintf(stderr,
                            "    chan user: name=%s site=%s:%d\n",
                            chan_name ? chan_name : "?",
                            chan_file ? chan_file : "?",
                            chan_line);
                }
            }
            if (is_parked && !skipped) {
                /* R1 — surface the per-fiber async backtrace name in the
                 * deadlock dump too, so the reader sees both endpoints:
                 * "this task (`add_value` at file:line) is parked on
                 * channel `tx,rx` (created at file:line)". */
                const char* async_name = NULL;
                const char* async_file = NULL;
                int async_line = 0;
                if (sched_v2_fiber_get_diag_name(f, &async_name, &async_file, &async_line)) {
                    fprintf(stderr,
                            "    task user: name=%s site=%s:%d\n",
                            async_name ? async_name : "?",
                            async_file ? async_file : "?",
                            async_line);
                }
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

    /* Latch timer: require the stall to persist >= persist_ms. */
    uint64_t persist_ms = sched_v2_deadlock_persist_ms();
    uint64_t now = sched_v2_monotonic_ms();
    uint64_t first = atomic_load_explicit(&g_v2_deadlock_first_seen,
                                          memory_order_relaxed);
    if (first == 0) {
        atomic_compare_exchange_strong_explicit(
            &g_v2_deadlock_first_seen, &first, now,
            memory_order_relaxed, memory_order_relaxed);
        return;
    }
    if (now - first < persist_ms) return;

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
    /* A dest list can still hold the kick (this fiber). Parking on
     * our own done bit never completes. */
    if (f == sched_v2_current_fiber())
        return 0;

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
        #if defined(__TINYC__)
        cc_cpu_yield_port();
        #elif defined(__aarch64__) || defined(__arm64__)
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
            /* Re-publish ourselves every pass.  A completer for a PREVIOUS
             * task that recycled into this fiber_v2 can execute its
             * exchange(join_waiter_fiber, NULL) late (it runs between
             * done=1 and its final wake), stealing our registration and
             * unparking us spuriously.  If we parked again without
             * re-registering, the REAL completion would find waiter==NULL
             * and only hit done_wake — which a parked fiber never waits
             * on — a lost wake.  The store+fence re-runs the Dekker
             * publish on every iteration, making the spurious unpark
             * harmless. */
            atomic_store_explicit(&f->join_waiter_fiber,
                                  (cc__fiber*)cc__fiber_current(),
                                  memory_order_release);
            atomic_thread_fence(memory_order_seq_cst);
            if (atomic_load_explicit(&f->done, memory_order_acquire)) break;
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

fiber_v2* sched_v2_spawn_in_nursery(void* (*fn)(void*), void* arg, CCNurseryHost* nursery) {
    sched_v2_ensure_init();

    fiber_v2* f = fiber_v2_alloc();
    if (!f) return NULL;

    f->entry_fn = fn;
    f->entry_arg = arg;
    f->saved_nursery = nursery;
    f->admission_nursery = nursery;
    atomic_store_explicit(&f->suspends, 0, memory_order_relaxed);
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
