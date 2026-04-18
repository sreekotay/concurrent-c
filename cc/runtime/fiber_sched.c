/*
* Fiber Scheduler - M:N userspace threading using minicoro
* 
* Design:
*   - Each fiber is a minicoro coroutine with its own stack
*   - N worker threads run M fibers cooperatively
*   - Blocking operations park the fiber, not the thread
*   - Worker immediately picks up next runnable fiber
*   - Coroutine pooling: freed fibers keep their coro for reuse
*
* This enables high-performance channel operations without kernel syscalls.
*/

/* Enable virtual memory backed allocator for growable stacks.
* This reserves large virtual address space (2MB) but only commits
* physical memory on demand (~4KB pages as stack grows).
* Trade-off: mco_create/destroy are slower (mmap/munmap syscalls),
* but coroutine pooling (99% reuse) amortizes this cost. */
#define MCO_USE_VMEM_ALLOCATOR

#define MINICORO_IMPL
#include "minicoro.h"
#include "fiber_internal.h"
#include "fiber_sched_boundary.h"

#ifndef CC_FIBER_UNPARK_ATTR_CONTENTION_LOCAL
#define CC_FIBER_UNPARK_ATTR_CONTENTION_LOCAL (1u << 0)
#endif

/* Note: We access minicoro internals (_mco_context, _mco_ctxbuf, _mco_wrap_main, _mco_main)
* for fast coroutine reset. These are defined in minicoro.h when MINICORO_IMPL is set. */

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sched.h>
#include <time.h>

#include "wake_primitive.h"


/* TSan annotations for synchronization and fiber context switching */
#include "tsan_helpers.h"

/* Implemented in channel.c; used by deadlock diagnostics to snapshot parked
 * channel state without reaching into channel internals from this file. */
void cc__chan_debug_dump_state(void* ch_obj, const char* prefix);
int cc__chan_debug_req_wake_match(void* ch_obj);
int cc__chan_debug_is_open(void* ch_obj);

/* High-frequency replacement probe counters are useful for deep diagnosis but
* expensive/noisy in hot loops. Keep them compile-time gated for quick A/B runs.
* 0 = off (default), 1 = on. */
#ifndef CC_V3_HEAVY_REPL_PROBE_COUNTERS
#define CC_V3_HEAVY_REPL_PROBE_COUNTERS 0
#endif

#if CC_V3_HEAVY_REPL_PROBE_COUNTERS
    atomic_fetch_add_explicit(&(g_cc_worker_gap_stats.field), (value), memory_order_relaxed)
#else
#endif

/* ============================================================================
* Guard-page stack allocator
* ============================================================================
* Allocates size + PAGE bytes via mmap, mprotects the bottom page as
* PROT_NONE so stack overflow traps deterministically instead of silently
* corrupting memory.  Performance: only runs on mco_create (cold path,
* amortized by fiber pooling).  Uses sysconf for the real page size
* (16KB on Apple Silicon, 4KB on x86-64).
*/
#include <sys/mman.h>
#include <unistd.h>

/* ============================================================================
* CPU pause for spin loops
* ============================================================================ */

static inline void cpu_pause(void) {
    #if defined(__aarch64__) || defined(__arm64__)
    __asm__ volatile("isb");
    #elif defined(__x86_64__) || defined(_M_X64)
    __asm__ volatile("pause");
    #else
    __asm__ volatile("" ::: "memory");
    #endif
}

/* ============================================================================
* High-resolution timing for instrumentation
* ============================================================================ */

static inline uint64_t rdtsc(void) {
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

/* Spawn timing breakdown (enabled by CC_SPAWN_TIMING env var) */

static _Atomic uint64_t g_cc_fiber_unpark_calls = 0;
static _Atomic uint64_t g_cc_fiber_unpark_enqueues = 0;
static _Atomic uint64_t g_cc_fiber_unpark_by_reason[CC_FIBER_UNPARK_REASON_COUNT] = {0};
static _Atomic uint64_t g_cc_join_park_joins = 0;
static _Atomic uint64_t g_cc_join_park_loops = 0;
static _Atomic uint64_t g_cc_join_help_attempts = 0;  /* help-first steal attempts */
static _Atomic uint64_t g_cc_join_help_hits = 0;      /* help-first steal successes */
static _Atomic size_t g_total_timed_parked = 0;
static _Atomic size_t g_deadlock_suppressed_timed_parked = 0;
static _Atomic int g_cc_join_help_mode = -1;          /* -1 unknown, 0 off, 1 on */

static const char* cc__fiber_unpark_reason_name(cc__fiber_unpark_reason reason) {
    switch (reason) {
        case CC_FIBER_UNPARK_REASON_GENERIC: return "generic";
        case CC_FIBER_UNPARK_REASON_SCHED_API: return "sched_api";
        case CC_FIBER_UNPARK_REASON_SOCKET_SIGNAL: return "socket_signal";
        case CC_FIBER_UNPARK_REASON_IO_KQUEUE_PERSISTENT: return "io_kq_persist";
        case CC_FIBER_UNPARK_REASON_IO_KQUEUE_ONESHOT: return "io_kq_oneshot";
        case CC_FIBER_UNPARK_REASON_IO_POLL: return "io_poll";
        case CC_FIBER_UNPARK_REASON_TASK_DONE: return "task_done";
        case CC_FIBER_UNPARK_REASON_TIMER: return "timer";
        case CC_FIBER_UNPARK_REASON_JOIN: return "join";
        case CC_FIBER_UNPARK_REASON_ENQUEUE: return "enqueue";
        case CC_FIBER_UNPARK_REASON_COUNT: break;
    }
    return "unknown";
}

void cc__fiber_unpark_stats(uint64_t* out_calls, uint64_t* out_enqueues) {
    if (out_calls) {
#if CC_V3_DIAGNOSTICS
        *out_calls = atomic_load_explicit(&g_cc_fiber_unpark_calls, memory_order_relaxed);
#else
        *out_calls = 0;
#endif
    }
    if (out_enqueues) {
#if CC_V3_DIAGNOSTICS
        *out_enqueues = atomic_load_explicit(&g_cc_fiber_unpark_enqueues, memory_order_relaxed);
#else
        *out_enqueues = 0;
#endif
    }
}

void cc__fiber_dump_unpark_reason_stats(void) {
    fprintf(stderr, "  wake sources:");
    for (int i = 0; i < CC_FIBER_UNPARK_REASON_COUNT; ++i) {
        uint64_t count = atomic_load_explicit(&g_cc_fiber_unpark_by_reason[i], memory_order_relaxed);
        fprintf(stderr, " %s=%llu",
                cc__fiber_unpark_reason_name((cc__fiber_unpark_reason)i),
                (unsigned long long)count);
    }
    fprintf(stderr, "\n");
}

void cc__fiber_join_park_stats(uint64_t* out_joins, uint64_t* out_loops) {
    if (out_joins) {
        *out_joins = atomic_load_explicit(&g_cc_join_park_joins, memory_order_relaxed);
    }
    if (out_loops) {
        *out_loops = atomic_load_explicit(&g_cc_join_park_loops, memory_order_relaxed);
    }
}

void cc__fiber_join_help_stats(uint64_t* out_attempts, uint64_t* out_hits) {
    if (out_attempts) {
        *out_attempts = atomic_load_explicit(&g_cc_join_help_attempts, memory_order_relaxed);
    }
    if (out_hits) {
        *out_hits = atomic_load_explicit(&g_cc_join_help_hits, memory_order_relaxed);
    }
}


void cc_fiber_dump_timing(void) {
}

/* Forward declarations - defined in nursery.c */
void cc_nursery_dump_timing(void);
typedef struct CCNursery CCNursery;

/* Forward declarations - defined in sched_v2.c */
typedef struct fiber_v2 fiber_v2;
#define CC_FIBER_V2_TAG_LOCAL ((uintptr_t)1)
static inline int cc__is_v2_fiber_local(cc__fiber* f) {
    return ((uintptr_t)f & CC_FIBER_V2_TAG_LOCAL) != 0;
}
static inline fiber_v2* cc__untag_v2_fiber_local(cc__fiber* f) {
    return (fiber_v2*)((uintptr_t)f & ~CC_FIBER_V2_TAG_LOCAL);
}
int    sched_v2_in_context(void);
fiber_v2* sched_v2_current_fiber(void);
void   sched_v2_park(void);
void   sched_v2_yield(void);
void   sched_v2_set_park_reason(const char* reason);
void   sched_v2_signal(fiber_v2* f);
uint64_t sched_v2_fiber_publish_wait_ticket(fiber_v2* f);
int      sched_v2_fiber_wait_ticket_matches(fiber_v2* f, uint64_t ticket);
void*  sched_v2_current_result_buf(size_t size);
/* Deadlock-detector metadata setters (see sched_v2.h). Forward declared
 * here so the V1 shims in this file can route to them without pulling in
 * the full sched_v2.h include from fiber_sched.c. */
void   sched_v2_fiber_set_park_obj(fiber_v2* f, void* obj);
void   sched_v2_fiber_inc_deadlock_suppress(fiber_v2* f);
void   sched_v2_fiber_dec_deadlock_suppress(fiber_v2* f);
int    sched_v2_fiber_deadlock_suppressed(fiber_v2* f);
void   sched_v2_fiber_inc_external_wait(fiber_v2* f);
void   sched_v2_fiber_dec_external_wait(fiber_v2* f);
int    sched_v2_fiber_external_wait_active(fiber_v2* f);
void   sched_v2_debug_dump_fiber(fiber_v2* f, const char* prefix);
void   sched_v2_debug_dump_state(const char* prefix);
bool cc_nursery_is_cancelled(const CCNursery* n);
/* cc__tls_current_nursery is the per-OS-thread nursery context pointer.
* Fibers must restore it on migration; see worker_run_fiber(). */
extern __thread CCNursery* cc__tls_current_nursery;

void cc__fiber_park_if(_Atomic int* flag, int expected, const char* reason, const char* file, int line);

/* ============================================================================
* Spin-then-condvar constants
* 
* Tuned for high-throughput channel operations. More spinning reduces condvar
* syscall overhead at the cost of CPU usage when idle. Override via env vars:
*   CC_SPIN_FAST_ITERS=128   (default: 128)
*   CC_SPIN_YIELD_ITERS=8    (default: 8)
*
* Rationale: for CPU-bound workloads (e.g. pigz), workers idle between task
* completions for ~4ms.  Spinning for 200µs (1024 + 64 iters) wastes 7× that
* per pipeline cycle across all idle workers.  128 fast + 8 yield (~26µs) is
* long enough to catch work that arrives from a co-located fiber while keeping
* idle CPU overhead below 1% per cycle.  Override via env var for I/O-bound
* workloads that benefit from lower sleep/wake frequency.
*
* Note: profiling with macOS `sample` under redis_hybrid shows ~10-15% of
* one classic worker's active time in swtch_pri (sched_yield) from the loop
* below, but empirical 10-run sweeps show Y=8 produces higher median
* throughput and *fewer* tail stalls than Y=0.  The yield-before-park phase
* appears to have net-positive scheduling effects beyond the raw CPU it
* consumes — leaving the default at 8.
* ============================================================================ */

#define SPIN_FAST_ITERS_DEFAULT 128
#define SPIN_YIELD_ITERS_DEFAULT 8


/* Backward compatibility macros - used in hot paths, cached after first call */
#define SPIN_FAST_ITERS get_spin_fast_iters()
#define SPIN_YIELD_ITERS get_spin_yield_iters()

/* ============================================================================
* Configuration
* ============================================================================ */

#ifndef CC_FIBER_WORKERS
#define CC_FIBER_WORKERS 0  /* 0 = detect at runtime */
#endif

#ifndef CC_FIBER_STACK_SIZE
/* With MCO_USE_VMEM_ALLOCATOR, physical memory is only committed on demand.
* We can use large virtual stack (2MB) with low physical memory cost. */
#ifdef MCO_USE_VMEM_ALLOCATOR
#define CC_FIBER_STACK_SIZE (2 * 1024 * 1024)  /* 2MB virtual, ~8KB physical */
#else
#define CC_FIBER_STACK_SIZE (128 * 1024)  /* 128KB per fiber */
#endif
#endif

#ifndef CC_FIBER_QUEUE_INITIAL
#define CC_FIBER_QUEUE_INITIAL 4096  /* Start small, grow on demand */
#endif

#define MAX_WORKERS 64
#define CACHE_LINE_SIZE 64

/* ============================================================================
* Fiber State
* ============================================================================ */

/* Unified control word — encodes lifecycle state + exclusive worker ownership.
* Replaces the old (fiber_state enum + running_lock) pair.
*
*   CTRL_IDLE     (0)   — in pool, available for reuse
*   CTRL_QUEUED  (-1)   — in a run queue
*   CTRL_PARKED  (-2)   — parked, stack quiescent, safe to resume
*   CTRL_PARKING (-3)   — legacy (unused with yield-before-commit)
*   CTRL_DONE    (-4)   — completed, stack quiescent, safe for joiner to reclaim.
*                          Implementation extension for join signaling — the
*                          trampoline sets this after mco_resume returns so
*                          joiners know the stack is truly idle.  Not in the
*                          base spec; documented in spec/scheduler_v2.md §DONE.
*   > 0: CTRL_OWNED(wid) = wid+1 — stack exclusively owned by worker wid
*
* The CAS QUEUED→OWNED(wid) atomically claims "running" AND "I own the stack",
* eliminating the need for a separate running_lock. */
#define CTRL_IDLE       ((int64_t) 0)
#define CTRL_QUEUED     ((int64_t)-1)
#define CTRL_PARKED     ((int64_t)-2)
#define CTRL_PARKING    ((int64_t)-3)
#define CTRL_DONE       ((int64_t)-4)
#define CTRL_OWNED(wid)      ((int64_t)((wid) + 1))
#define CTRL_OWNED_TEMP      ((int64_t)0x7FFF)   /* replacement worker sentinel */
#define CTRL_IS_OWNED(v)     ((v) > 0)
#define CTRL_OWNER(v)        ((int)((v) - 1))

/* yield_dest values — set by fiber, consumed by worker trampoline */
#define YIELD_NONE   0
#define YIELD_LOCAL  1
#define YIELD_GLOBAL 2
#define YIELD_SLEEP  3
#define YIELD_PARK   4

typedef struct fiber_task {
    /* Hot path fields - accessed during execution */
    mco_coro* coro;           /* minicoro coroutine handle */
    void* (*fn)(void*);       /* User function */
    void* arg;                /* User argument */
    void* result;             /* Return value */
    char result_buf[48];      /* Fiber-local storage for struct results (avoids malloc) */
    _Atomic int64_t control;  /* Unified control word (see encoding above) */
    _Atomic uint64_t generation; /* Monotonic incarnation tag for pooled reuse */
    _Atomic int done;
    _Atomic uint64_t wake_counter; /* Debug-only wake counter */
    _Atomic uint64_t wait_ticket;  /* Monotonic ticket for waiter ABA defense */
    _Atomic int pending_unpark; /* Latch for early unpark while running */
    _Atomic uint64_t last_transition; /* rdtsc timestamp of last state transition (stall diagnostic) */
    
    /* Yield-before-commit: fiber sets yield_dest then mco_yield().
    * The worker trampoline enqueues after mco_resume returns.
    * 0=none, 1=local queue, 2=global queue, 3=sleep queue, 4=park */
    int yield_dest;
    
    /* Park parameters — carried across mco_yield for trampoline commit */
    _Atomic int* park_flag;       /* Condition flag to re-check (NULL = unconditional) */
    int park_expected;            /* Expected value of park_flag */
    
    /* Per-fiber join synchronization */
    _Atomic int join_waiters;           /* Count of threads/fibers waiting to join */
    struct fiber_task* _Atomic join_waiter_fiber;  /* Single waiting fiber (common case) */
    _Atomic int join_lock;              /* Spinlock for done/waiter handshake */
    pthread_mutex_t join_mu;            /* Mutex for join condvar (thread waiters) */
    pthread_cond_t join_cv;             /* Per-fiber condvar for thread waiters */
    _Atomic int join_cv_initialized;    /* Lazy init flag for condvar (atomic for race-free signaling) */
    void* tsan_fiber;                   /* TSan fiber handle (if enabled) */
    
    /* Debug info for deadlock detection */
    const char* park_reason;  /* Why fiber parked (e.g., "chan_send", "chan_recv", "join") */
    const char* park_file;    /* Source file where park was requested */
    int park_line;            /* Source line where park was requested */
    void* park_obj;           /* Optional related object (e.g., channel pointer) */
    uintptr_t fiber_id;       /* Unique ID for this fiber (for debug output) */
    
    int last_worker_id;       /* Worker affinity hint: last worker that ran this fiber (-1 = none) */
    unsigned char last_worker_src; /* 0=none, 1=run, 2=affinity, 3=chan_partner */
    uint64_t spawn_publish_tsc;      /* Diagnostics: publish timestamp for spawn->run latency */
    uint64_t spawn_global_pop_tsc;   /* Diagnostics: first global-pop timestamp for spawned fiber */
    unsigned char spawn_publish_route;   /* 1=local, 2=inbox, 3=global */
    int spawn_publish_target_worker;     /* Diagnostics: intended worker for local/inbox publish */
    unsigned char spawn_publish_active0; /* 1 if active==0 at publish time */
    unsigned char spawn_publish_forced_spill; /* 1 if startup inbox budget forced global spill */
    unsigned char spawn_global_pop_valid; /* 1 if global-pop timestamp is valid */
    unsigned char spawn_publish_valid;   /* 1 if publish metadata is populated */
    
    struct timespec sleep_deadline;  /* Deadline for cc_sleep_ms timer parking */
    struct timespec timed_park_deadline; /* Deadline for timer-backed guarded park */
    struct fiber_task* timer_next;       /* Intrusive link for timer wait queue */
    int timed_park_requested;            /* Park request should arm a timer on commit */
    _Atomic int timed_park_registered;   /* Timer wait is visible to the timer queue */
    _Atomic int timed_park_fired;        /* Timer path woke this fiber */
    CCNursery* saved_nursery;       /* Nursery context saved across fiber migrations */
    unsigned deadlock_suppress_depth; /* Local deadlock-suppression scope carried across yields */
    unsigned external_wait_depth;   /* External-progress wait scope carried across yields */
    unsigned external_wait_scoped;  /* Dynamic external-wait scope is currently active */
    unsigned external_wait_parked;  /* This park instance contributed to g_external_wait_parked */
    CCNursery* admission_nursery;   /* Admission boundary checked on first entry */
    
    /* Debug: track who last enqueued this fiber */
    int enqueue_src;          /* Enum: 1=trampoline_yield, 2=spawn, 3=park_abort_pending,
                                4=park_abort_flag, 5=park_abort_cas, 6=park_self_unpark,
                                7=park_post_flag, 8=unpark, 9=commit_park_abort */
    int64_t enqueue_ctrl;     /* Control word at enqueue time */
    int enqueue_done;         /* done flag at enqueue time */
    int enqueue_dest;         /* yield_dest at enqueue time */
    
    struct fiber_task* next;  /* For free list / queues */
} fiber_task;

typedef struct {
    fiber_task* fiber;
    uint64_t generation;
} runnable_ref;

typedef struct {
    fiber_task* _Atomic fiber;
    _Atomic uint64_t generation;
} runnable_slot;

typedef struct fiber_overflow_node {
    runnable_ref ref;
    struct fiber_overflow_node* next;
} fiber_overflow_node;

typedef enum {
    CC_WL_ACTIVE = 0,
    CC_WL_IDLE_SPIN = 1,
    CC_WL_SLEEP = 2,
    CC_WL_DRAINING = 3,
    CC_WL_DEAD = 4,
} cc_worker_lifecycle;

/* Optional spec assertion mode for transition/enqueue legality checks.
* Enable with CC_V3_SPEC_ASSERT=1 when auditing §9/§10 linearization points. */


/* General-purpose V1 diagnostic-counter gate.  Mirrors the V2 sched_v2.c
 * gate.  Many g_sched.pressure_*, g_sched.promotion_count,
 * g_cc_fiber_unpark_*, g_cc_join_*, g_fibers_spawned, etc. counters used to
 * be unconditionally incremented on hot paths (pressure eval, park/unpark,
 * join) purely for diagnostics.  Opt in with CC_SCHED_STATS=1 to populate
 * them.  The macros fall through to a no-op when disabled, so the RMW and
 * the cache-line contention disappear entirely. */
static int cc_sched_diag_stats_enabled(void) {
    static int mode = -1;
    if (mode >= 0) return mode;
    const char* env = getenv("CC_SCHED_STATS");
    mode = (env && env[0] && !(env[0] == '0' && env[1] == '\0')) ? 1 : 0;
    return mode;
}

#define SCHED_DIAG_STAT_INC(counter) \
    do { \
        if (__builtin_expect(cc_sched_diag_stats_enabled(), 0)) \
            atomic_fetch_add_explicit(&(counter), 1, memory_order_relaxed); \
    } while (0)

#define SCHED_DIAG_STAT_ADD(counter, delta) \
    do { \
        if (__builtin_expect(cc_sched_diag_stats_enabled(), 0)) \
            atomic_fetch_add_explicit(&(counter), (delta), memory_order_relaxed); \
    } while (0)

/* ============================================================================
* Lock-Free MPMC Queue with overflow list
*
* Fast path: fixed-size lock-free ring buffer (CC_FIBER_QUEUE_INITIAL slots).
* Slow path: when the ring is full, overflow to a mutex-protected linked list.
* Workers drain the overflow list back into the ring when they find it empty.
* This keeps the common case lock-free while handling arbitrary spawn bursts.
* ============================================================================ */

typedef struct {
    runnable_slot slots[CC_FIBER_QUEUE_INITIAL];
    _Atomic size_t head;
    _Atomic size_t tail;
    _Atomic int nonempty_hint;  /* 0 likely empty, 1 maybe non-empty */
    /* Overflow list for when ring is full */
    pthread_mutex_t overflow_mu;
    fiber_overflow_node* overflow_head;
    fiber_overflow_node* overflow_tail;
    _Atomic size_t overflow_count;
} fiber_queue;

/* Try to push to the lock-free ring. Returns 0 on success, -1 if full. */
/* Push: try ring first, fall back to overflow list. Never blocks. */
/* Check if queue has items (non-destructive peek) */
/* Pop from overflow list (caller should try ring first). */
/* Global run-queue helpers (implemented after scheduler state declaration). */

/* ============================================================================
* Sleep Queue — timer-based fiber parking for cc_sleep_ms
*
* Instead of busy-yielding sleeping fibers back through the run queue,
* we park them here.  Sysmon (or workers during idle) scans the list
* every millisecond and re-enqueues fibers whose deadline has passed.
* This eliminates O(N) queue churn for N concurrent sleepers.
* ============================================================================ */

typedef struct {
    pthread_mutex_t mu;
    fiber_task* head;        /* Singly-linked list of sleeping fibers (via ->next) */
    _Atomic size_t count;    /* Approximate count for quick peek */
} sleep_queue;


typedef struct {
    pthread_mutex_t mu;
    fiber_task* head;        /* Singly-linked list of timed parked fibers (via ->timer_next) */
    _Atomic size_t count;    /* Approximate queue size for quick peek */
} timed_park_queue;

void cc__fiber_unpark(void* fiber_ptr);
void cc__fiber_unpark_tagged(void* fiber_ptr, cc__fiber_unpark_reason reason);

/* Park a fiber for sleep.  Caller must have already set f->sleep_deadline
* and transitioned the fiber control word to CTRL_QUEUED (or similar).
* The fiber will be resumed by sq_drain when its deadline passes. */
/* Drain expired sleepers back into the run queue.
* Returns the number of fibers woken. */
/* ============================================================================
* Scheduler State
* ============================================================================ */

/* Per-worker local queue for spawn locality */
#define LOCAL_QUEUE_SIZE 256
/* Depth threshold above which new spawns are routed to other workers' inboxes
* for parallel execution.  Value 1 means only the very first spawn goes into
* the spawning worker's own local queue; every subsequent spawn from the same
* nursery is routed to a different worker's inbox immediately.
*
* This ensures independent channel pairs land on separate workers from the
* start, regardless of spawn order.  Co-location within a pair is established
* by the direct-handoff hint: when the producer (on worker A) first sends to
* the channel it wakes the consumer with a hint for worker A, pulling the
* consumer onto A's local queue — so the pair runs co-operatively on A for
* all subsequent iterations at zero cross-worker cost.
*
* Raising this value above 1 causes multiple producers (or multiple consumers)
* from independent pairs to share the spawning worker, and the direct-handoff
* then pulls all their partners onto the same worker too — serialising what
* should be parallel work. */
#define CC_SPAWN_LOCAL_DEPTH_LIMIT 2
#define CC_CHAN_UNPARK_LOCAL_DEPTH_LIMIT 2
/* Per-worker inbox for cross-thread spawns.
* If this fills, we fall back to the global queue and optionally warn. */
#define INBOX_QUEUE_SIZE 1024

typedef struct {
    runnable_slot slots[LOCAL_QUEUE_SIZE];
    _Atomic size_t head;
    _Atomic size_t tail;
} local_queue;

typedef struct {
    runnable_slot slots[INBOX_QUEUE_SIZE];
    _Atomic size_t head;
    _Atomic size_t tail;
} inbox_queue;

static _Atomic size_t g_inbox_overflow = 0;
static _Atomic int g_inbox_warned = 0;
static int g_inbox_debug = -1;  /* -1 = not checked, 0 = disabled, 1 = enabled */
static int g_inbox_dump = -1;   /* -1 = not checked, 0 = disabled, 1 = enabled */






/* Number of items currently in the inbox (approximate; used for spawn pairing). */
typedef struct {
    pthread_t* workers;
    size_t num_workers;
    _Atomic int running;
    
    fiber_queue* run_queue;         /* Global queue array base (1 or many shards) */
    size_t run_queue_count;         /* Number of global queue shards */
    local_queue* local_queues;      /* Per-worker local queues */
    inbox_queue* inbox_queues;      /* Per-worker MPMC inbox queues */
    fiber_task* _Atomic free_list;
    
    wake_primitive wake_prim;           /* Global wake (shutdown, global-queue, join) */
    char _pad_wake[CACHE_LINE_SIZE];    /* Isolate from pending */
    wake_primitive* worker_wake_prims;  /* Per-worker wake primitives: workers sleep here */
    
    /* HIGHLY CONTENDED: updated on every spawn and complete - needs own cache line */
    _Atomic size_t pending;
    char _pad_pending[CACHE_LINE_SIZE - sizeof(_Atomic size_t)];
    _Atomic int64_t pressure;  /* Signed/saturating pressure signal (§7), updated at admit/complete edges */
    
    /* Track worker states for smarter waking - cache line padded to avoid false sharing */
    _Atomic size_t active;      /* Workers currently executing fibers */
    char _pad_active[CACHE_LINE_SIZE - sizeof(_Atomic size_t)];
    
    _Atomic size_t sleeping;    /* Workers blocked on condvar */
    char _pad_sleeping[CACHE_LINE_SIZE - sizeof(_Atomic size_t)];
    
    _Atomic size_t spinning;    /* Workers actively polling (not sleeping yet) */
    char _pad_spinning[CACHE_LINE_SIZE - sizeof(_Atomic size_t)];
    
    /* Global parked-fiber count for deadlock detection (lightweight, always maintained). */
    _Atomic size_t total_parked;
    _Atomic size_t deadlock_suppressed_parked;

    /* Per-worker parked counts — diagnostic only, updated only when gap-stats enabled. */
    _Atomic size_t* worker_parked;
    
    /* Hybrid promotion (sysmon): per-worker heartbeat updated once per batch loop.
    * Sysmon detects stuck workers by checking if heartbeat hasn't updated.
    * Cache-line aligned so sysmon reads don't false-share with worker writes. */
    struct { _Atomic uint64_t heartbeat; char _pad[CACHE_LINE_SIZE - sizeof(_Atomic uint64_t)]; }* worker_heartbeat;
    _Atomic unsigned char* worker_lifecycle;  /* Debug lifecycle labels (§6): ACTIVE/IDLE_SPIN/SLEEP/DRAINING/DEAD */
    _Atomic unsigned char* worker_first_sleep_seen; /* Trace-only: first ACTIVE->SLEEP marker per worker */
    _Atomic uint64_t* worker_boot_tsc; /* Trace-only: first DEAD->ACTIVE timestamp per worker */
    _Atomic uint64_t lifecycle_illegal_transitions;
    
    /* Sysmon thread: spawns temp workers when CPU-bound fibers stall */
    pthread_t sysmon_thread;
    int sysmon_started;  /* 1 if pthread_create succeeded (shutdown joins only then) */
    _Atomic int sysmon_running;
    _Atomic size_t temp_worker_count;
    _Atomic uint64_t last_promotion_cycles;  /* rdtsc of last temp worker spawn (rate limit) */
    _Atomic size_t promotion_count;         /* Stats: total temp workers ever spawned */
    _Atomic uint64_t pressure_samples;      /* Sysmon samples for pressure telemetry */
    _Atomic uint64_t pressure_positive_samples;
    _Atomic uint64_t pressure_negative_samples;
    _Atomic uint64_t pressure_negative_streak_max;
    _Atomic uint64_t pressure_gate_passes;
    _Atomic uint64_t pressure_block_no_work;
    _Atomic uint64_t pressure_block_nonpositive;
    _Atomic uint64_t pressure_block_idle_capacity;
    _Atomic uint64_t pressure_block_not_stuck;
    _Atomic uint64_t pressure_promoted_workers;
    
    /* Stats - less hot, can share cache lines */
    _Atomic size_t blocked_threads;  /* Threads blocked in cc_block_on (not fiber parking) */
    _Atomic size_t completed;
    _Atomic size_t coro_reused;
    _Atomic size_t coro_created;
    _Atomic int startup_phase;      /* 0=STARTUP deterministic policy, 1=RUN */
    _Atomic size_t startup_run_count;
    _Atomic size_t startup_target_runs;
} fiber_sched;

static fiber_sched g_sched = {0};
enum {
    CC_STARTUP_PHASE = 0,
    CC_RUN_PHASE = 1
};
static _Atomic int g_initialized = 0;
static _Atomic size_t g_external_wait_parked = 0;
/* Non-fiber threads currently inside a cc_external_wait_enter/leave scope.
 * Consulted by both the V1 and V2 deadlock detectors as evidence that an
 * external progress source exists (e.g. a blocking kqueue wait), which
 * suppresses the deadlock verdict when the only remaining internal waits
 * are receivers on still-open channels. Exported (non-static) so
 * sched_v2_check_deadlock in sched_v2.c can read it directly rather than
 * through a getter — this is a single atomic load. */
_Atomic size_t g_external_wait_threads = 0;
static _Atomic size_t g_requested_workers = 0;  /* User-requested worker count (0 = auto) */
static _Atomic int g_cc_v3_pressure_stats_atexit = 0;
static _Atomic int g_sharded_runq_mode = -1; /* -1 unknown, 0 off, 1 on */
static _Atomic size_t g_global_pop_rr = 0;
static _Atomic uint32_t g_spawn_nw_startup_admit_wakev = UINT32_MAX;
static _Atomic size_t g_spawn_nw_startup_admit_remaining = 0;

typedef enum {
    SPAWN_ROUTE_NONE = 0,
    SPAWN_ROUTE_LOCAL = 1,
    SPAWN_ROUTE_INBOX = 2,
    SPAWN_ROUTE_GLOBAL = 3
} spawn_publish_route;



typedef enum {
    CC_WAKE_REASON_SYSMON_SLEEPQ = 0,
    CC_WAKE_REASON_SPAWN_GLOBAL_EDGE = 1,
    CC_WAKE_REASON_SPAWN_NONGLOBAL = 2,
    CC_WAKE_REASON_JOIN_THREAD = 3,
    CC_WAKE_REASON_UNPARK_GLOBAL_EDGE = 4,
    CC_WAKE_REASON_UNPARK_NONGLOBAL = 5,
    CC_WAKE_REASON_COUNT = 6
} cc_wake_reason;

static inline uint64_t cc__mono_ns_sched(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}



static inline uint64_t cc__mono_ns_sched(void);







/* Cheap global-work hint probe: checks shard nonempty hints only. */


/* Set the number of worker threads before scheduler init */
void cc_sched_set_num_workers(size_t n) {
    atomic_store(&g_requested_workers, n);
}

/* Get the current number of workers (returns requested count if not yet initialized) */
size_t cc_sched_get_num_workers(void) {
    if (atomic_load_explicit(&g_initialized, memory_order_acquire) == 2) {
        return g_sched.num_workers;
    }
    return atomic_load(&g_requested_workers);
}
static _Atomic uintptr_t g_next_fiber_id = 1;       /* Counter for unique fiber IDs */


/* Per-worker thread-local state */
static __thread fiber_task* tls_current_fiber = NULL;
static __thread unsigned tls_deadlock_suppress_depth = 0;
static __thread unsigned tls_external_wait_depth = 0;
static __thread int tls_worker_id = -1;  /* -1 = not a worker thread */


/* Return the current scheduler base-worker ID, or -1 if the calling thread
* is not a base worker (e.g. a temp/timer worker or a non-scheduler thread).
* Used by channel wakeup code to implement direct-handoff routing. */
int cc__sched_current_worker_id(void) {
    /* Prefer a V2 worker id when we're on a V2 worker thread. Under the V2-
     * default scheduler, V1's tls_worker_id is -1 for V2 workers, so the
     * legacy probe (as used by nursery_spawn_placement_workers4_smoke and
     * similar affinity tests) would always report "no worker" without this
     * bridge. */
    int v2 = sched_v2_current_worker_id();
    if (v2 >= 0) return v2;
    return tls_worker_id;
}


/* Called when a thread is about to block in cc_block_on.
* Only tracks blocking on fiber worker threads - blocking on executor threads
* (e.g., cc_block_all) is expected and shouldn't trigger deadlock detection. */
void cc__deadlock_thread_block(void) {
    if (tls_worker_id < 0) return;  /* Not a fiber worker thread */
    atomic_fetch_add_explicit(&g_sched.blocked_threads, 1, memory_order_release);
    /* Don't check immediately - let time-based detection handle it */
}

/* Called when a thread unblocks from cc_block_on */
void cc__deadlock_thread_unblock(void) {
    if (tls_worker_id < 0) return;  /* Not a fiber worker thread */
    atomic_fetch_sub_explicit(&g_sched.blocked_threads, 1, memory_order_relaxed);
}

void cc_deadlock_suppress_enter(void) {
    if (tls_current_fiber) {
        if (tls_deadlock_suppress_depth < UINT32_MAX) tls_deadlock_suppress_depth++;
        tls_current_fiber->deadlock_suppress_depth = tls_deadlock_suppress_depth;
        return;
    }
    /* V2 fiber path: track scope directly on the V2 fiber. We can't rely
     * on the V1 TLS because a V2 fiber may migrate between V2 workers
     * across park/resume, and TLS per-worker would leak depths between
     * unrelated fibers. Thread-scoped callers (no fiber at all) still
     * fall through to the V1 TLS counter below. */
    fiber_v2* v2f = sched_v2_current_fiber();
    if (v2f) {
        sched_v2_fiber_inc_deadlock_suppress(v2f);
        return;
    }
    if (tls_deadlock_suppress_depth < UINT32_MAX) tls_deadlock_suppress_depth++;
}

void cc_deadlock_suppress_leave(void) {
    if (tls_current_fiber) {
        if (tls_deadlock_suppress_depth == 0) return;
        tls_deadlock_suppress_depth--;
        tls_current_fiber->deadlock_suppress_depth = tls_deadlock_suppress_depth;
        return;
    }
    fiber_v2* v2f = sched_v2_current_fiber();
    if (v2f) {
        sched_v2_fiber_dec_deadlock_suppress(v2f);
        return;
    }
    if (tls_deadlock_suppress_depth == 0) return;
    tls_deadlock_suppress_depth--;
}

int cc_deadlock_suppressed(void) {
    if (tls_current_fiber) return tls_deadlock_suppress_depth > 0;
    fiber_v2* v2f = sched_v2_current_fiber();
    if (v2f) return sched_v2_fiber_deadlock_suppressed(v2f);
    return tls_deadlock_suppress_depth > 0;
}

/* Provisional mechanism: we currently model "externally driven wait" as a
 * dynamic scope on the current execution context. Semantically this really
 * belongs at specific runtime wait sites, but the scope form gives the
 * scheduler a simple way to carry the classification across a park/resume. */
void cc_external_wait_enter(void) {
    if (tls_current_fiber) {
        if (tls_external_wait_depth < UINT32_MAX) tls_external_wait_depth++;
        tls_current_fiber->external_wait_depth = tls_external_wait_depth;
        tls_current_fiber->external_wait_scoped = 1;
        return;
    }
    /* V2 fiber path: pin the scope on the V2 fiber, not on the worker
     * TLS. Same rationale as cc_deadlock_suppress_enter — V2 fibers can
     * migrate workers across park/resume. */
    fiber_v2* v2f = sched_v2_current_fiber();
    if (v2f) {
        sched_v2_fiber_inc_external_wait(v2f);
        return;
    }
    /* Thread-scoped caller (executor thread, test main, etc). Track per
     * thread and count distinct threads in g_external_wait_threads so the
     * V1+V2 deadlock detectors can treat external-wait threads as
     * legitimate progress makers. */
    if (tls_external_wait_depth < UINT32_MAX) tls_external_wait_depth++;
    if (tls_external_wait_depth == 1) {
        atomic_fetch_add_explicit(&g_external_wait_threads, 1, memory_order_relaxed);
    }
}

void cc_external_wait_leave(void) {
    if (tls_current_fiber) {
        if (tls_external_wait_depth == 0) return;
        tls_external_wait_depth--;
        tls_current_fiber->external_wait_depth = tls_external_wait_depth;
        if (tls_external_wait_depth == 0) {
            tls_current_fiber->external_wait_scoped = 0;
        }
        return;
    }
    fiber_v2* v2f = sched_v2_current_fiber();
    if (v2f) {
        sched_v2_fiber_dec_external_wait(v2f);
        return;
    }
    if (tls_external_wait_depth == 0) return;
    tls_external_wait_depth--;
    if (tls_external_wait_depth == 0) {
        atomic_fetch_sub_explicit(&g_external_wait_threads, 1, memory_order_relaxed);
    }
}

int cc_external_wait_active(void) {
    if (tls_current_fiber) return tls_external_wait_depth > 0;
    fiber_v2* v2f = sched_v2_current_fiber();
    if (v2f) return sched_v2_fiber_external_wait_active(v2f);
    return tls_external_wait_depth > 0;
}

/* Fast local queue push (single producer) */
/* Check if local queue has items (non-destructive peek) */
/* Return number of items in local queue (approximate, for routing decisions) */
/* Fast local queue pop (owner only - but must handle concurrent stealers) 
* Uses atomic exchange to claim slot first, then try to advance head once.
* Limited retries to avoid infinite loop under pathological contention. */
/* Global-edge wake policy for count convergence:
* require sleepers; allowing active workers avoids missed-wake starvation when
* runnable work appears during mixed active/sleeping transitions. */
/* Wake a specific worker if it is sleeping, using its per-worker primitive.
* Used after pushing to that worker's inbox: zero thundering herd, zero
* wasted spin cycles on all other workers.  Falls back to the global wake
* primitive when per-worker prims are not yet allocated (startup). */
/* Startup guard: only allow non-worker inbox publication once at least one
* worker is actively executing. Before that, fall back to global queue. */

/* Work stealing: steal from another worker's queue.
* Uses atomic exchange to claim slot first, then CAS to advance head. */

/* Dump scheduler state for debugging hangs */
void cc_fiber_dump_state(const char* reason) {
    fprintf(stderr, "\n=== FIBER SCHEDULER STATE: %s ===\n", reason ? reason : "");
    fprintf(stderr, "  pending=%zu active=%zu sleeping=%zu parked=%zu completed=%zu\n",
            atomic_load(&g_sched.pending),
            atomic_load(&g_sched.active),
            atomic_load(&g_sched.sleeping),
            atomic_load_explicit(&g_sched.total_parked, memory_order_relaxed),
            atomic_load(&g_sched.completed));
    fprintf(stderr, "================================\n\n");
}

/* Dump spawn stats */
void cc_fiber_dump_spawn_stats(void) {
    size_t reused = atomic_load(&g_sched.coro_reused);
    size_t created = atomic_load(&g_sched.coro_created);
    size_t total = reused + created;
    
    if (total == 0) {
        fprintf(stderr, "\n=== SPAWN STATS: no spawns recorded ===\n");
        return;
    }
    
    fprintf(stderr, "\n=== SPAWN STATS (%zu spawns) ===\n", total);
    fprintf(stderr, "  coro reused: %zu (%.1f%%)\n", reused, 100.0 * reused / total);
    fprintf(stderr, "  coro created: %zu (%.1f%%)\n", created, 100.0 * created / total);
    fprintf(stderr, "  hybrid promotion temp workers spawned: %zu\n", (size_t)atomic_load(&g_sched.promotion_count));
    fprintf(stderr, "================================\n\n");
}

/* ============================================================================
* Fiber Pool (with coroutine reuse)
* ============================================================================ */

/* Return a fiber to the global free list for reuse.
*
* Pool state: fibers in the free list retain their coroutine stack and
* join_cv (expensive to recreate).  Their control word may be CTRL_DONE
* (set by the trampoline when the fiber completed).  fiber_alloc()
* transitions control back to CTRL_IDLE before handing out a pooled fiber.
*
* The per-worker idle_pool (thread-local, accessed in fiber_alloc) is the
* fast path; this global free list is the slow path used when the local
* pool is full or when called from a non-worker context. */
/* Fully destroy a fiber (called during shutdown) */
/* ============================================================================
* Error Handling
* ============================================================================ */

/* ============================================================================
* Fiber Entry Point
* ============================================================================ */

/* Simple spinlock for join handshake - ensures proper ordering between
* child setting done=1 and parent registering as waiter */
/* ============================================================================
* Worker Thread
* ============================================================================ */

/* Helper to resume fiber with error checking.
* With the unified control word the caller already holds exclusive ownership
* (CTRL_OWNED(wid)), so no separate running_lock is needed. */
#define WORKER_BATCH_SIZE 16  /* Standard batch size */
#define STEAL_BATCH_SIZE (LOCAL_QUEUE_SIZE / 2)  /* Steal up to half the victim's queue */
#define FAIRNESS_CHECK_INTERVAL 61  /* Inject global pop after this many local-only batches (prime) */

#define ORPHAN_THRESHOLD_CYCLES 3000000      /* ~1ms at 3GHz; retained for heartbeat/stall heuristics */

/* Simple xorshift64 PRNG for randomized victim selection */
static inline uint64_t xorshift64(uint64_t* state) {
    uint64_t x = *state;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    *state = x;
    return x;
}






/* V1 scheduler init/shutdown are stubs; the V2 scheduler auto-initializes
 * via sched_v2_spawn() on first use (see sched_v2.c). These remain as
 * private hooks only for the legacy cc_fiber_spawn
 * entry points in this file, which are themselves headed for removal. */
int cc_fiber_sched_init(size_t num_workers) {
    (void)num_workers;
    atomic_store_explicit(&g_initialized, 2, memory_order_release);
    return 0;
}

void cc_fiber_sched_shutdown(void) {
    atomic_store_explicit(&g_initialized, 0, memory_order_release);
}



fiber_task* cc_fiber_spawn(void* (*fn)(void*), void* arg) {
    if (!fn) return NULL;
    /* V1 retired: route straight to V2 and return a V2-tagged handle
     * so cc_fiber_join below can dispatch via sched_v2_join. Kept as a
     * compatibility shim for the legacy fiber_task*-based surface used
     * by a couple of perf harnesses. */
    fiber_v2* v2 = sched_v2_spawn(fn, arg);
    if (!v2) return NULL;
    return (fiber_task*)((uintptr_t)v2 | CC_FIBER_V2_TAG_LOCAL);
}

int cc_fiber_join(fiber_task* f, void** out_result) {
    if (!f) return -1;
    if (cc__is_v2_fiber_local((cc__fiber*)f)) {
        return sched_v2_join(cc__untag_v2_fiber_local((cc__fiber*)f), out_result);
    }
    /* V1 fibers no longer exist under V2-default; the V1 cc_fiber_spawn
     * shim always returns V2-tagged handles. */
    return -1;
}

void cc_fiber_set_spawn_nursery_override(CCNursery* nursery) {
    /* V1 retired: the override was consulted by the V1 spawn router.
     * V2 routes fibers through nurseries directly. Kept as a symbol stub. */
    (void)nursery;
}

void cc_fiber_task_free(fiber_task* f) {
    /* V1 retired: cc_fiber_spawn returns a V2-tagged fiber_v2*. Canonical V2
     * ownership is join() + fiber_release(); sched_v2_join drops the join
     * reference but does NOT free the fiber_v2 / stack — that is explicitly
     * what fiber_release exists for. The legacy V1 contract was
     * "join then free", so this shim has to call release here or we leak
     * the entire fiber allocation (including its mmap'd stack) on every
     * call, which is what hard-panicked the Mac in perf_spawn_ladder. */
    if (!f) return;
    if (cc__is_v2_fiber_local((cc__fiber*)f)) {
        sched_v2_fiber_release(cc__untag_v2_fiber_local((cc__fiber*)f));
    }
}


/* ============================================================================
* Fiber Parking (for channel blocking)
* ============================================================================ */

int cc__fiber_in_context(void) {
    return tls_current_fiber != NULL || sched_v2_in_context();
}

void* cc__fiber_current(void) {
    if (tls_current_fiber) return tls_current_fiber;
    fiber_v2* v2f = sched_v2_current_fiber();
    if (v2f) return (void*)((uintptr_t)v2f | 1); /* tagged v2 pointer */
    return NULL;
}

/* Get pointer to fiber-local result buffer (48 bytes).
* Use this to store task results without malloc.
* Returns NULL if not in fiber context. */
__attribute__((noinline))
void* cc_task_result_ptr(size_t size) {
    if (tls_current_fiber && size <= sizeof(tls_current_fiber->result_buf)) {
        return tls_current_fiber->result_buf;
    }
    /* V2 hybrid scheduler fiber */
    void* v2_buf = sched_v2_current_result_buf(size);
    if (v2_buf) return v2_buf;
    return NULL;
}


static int cc__fiber_park_if_impl(_Atomic int* flag, int expected, const struct timespec* abs_deadline,
                                const char* reason, const char* file, int line) {
    if (sched_v2_in_context()) {
        (void)file; (void)line;
        if (flag && atomic_load_explicit(flag, memory_order_acquire) != expected) return 0;
        if (abs_deadline) {
            struct timespec now;
            clock_gettime(CLOCK_REALTIME, &now);
            if (now.tv_sec > abs_deadline->tv_sec ||
                (now.tv_sec == abs_deadline->tv_sec && now.tv_nsec >= abs_deadline->tv_nsec)) {
                return 1; /* timed out */
            }
        }
        /* Publish park_reason so the deadlock detector can classify the
         * wait (chan_recv_wait_empty etc). sched_v2_park snapshots this
         * into f->park_reason at yield time; we clear after resume so
         * stale values don't leak into a subsequent unrelated park. */
        sched_v2_set_park_reason(reason);
        sched_v2_park();
        sched_v2_set_park_reason(NULL);
        if (abs_deadline) {
            struct timespec now;
            clock_gettime(CLOCK_REALTIME, &now);
            if (now.tv_sec > abs_deadline->tv_sec ||
                (now.tv_sec == abs_deadline->tv_sec && now.tv_nsec >= abs_deadline->tv_nsec)) {
                return 1; /* timed out */
            }
        }
        return 0;
    }
    return 0;
}

void cc__fiber_park_if(_Atomic int* flag, int expected, const char* reason, const char* file, int line) {
    (void)cc__fiber_park_if_impl(flag, expected, NULL, reason, file, line);
}

void cc__fiber_suspend_until_ready(_Atomic int* flag, int expected,
                                   const char* reason, const char* file, int line) {
    cc_external_wait_enter();
    if (sched_v2_in_context()) {
        /* V2 path: spin-check then park */
        (void)file; (void)line;
        sched_v2_set_park_reason(reason);
        while (atomic_load_explicit(flag, memory_order_acquire) == expected) {
            sched_v2_park();
        }
        sched_v2_set_park_reason(NULL);
        cc_external_wait_leave();
        return;
    }
    fiber_task* f = tls_current_fiber;
    if (!f || !f->coro) {
        cc_external_wait_leave();
        return;
    }
    f->external_wait_parked = 0;
    cc__fiber_park_if(flag, expected, reason, file, line);
    cc_external_wait_leave();
}

int cc__fiber_suspend_until_ready_or_cancel(_Atomic int* flag, int expected,
                                            const char* reason, const char* file, int line) {
    cc_external_wait_enter();
    CCNursery* cur_nursery = cc__tls_current_nursery;
    if (sched_v2_in_context()) {
        (void)file; (void)line;
        sched_v2_set_park_reason(reason);
        while (atomic_load_explicit(flag, memory_order_acquire) == expected) {
            if (cur_nursery && cc_nursery_is_cancelled(cur_nursery)) {
                sched_v2_set_park_reason(NULL);
                cc_external_wait_leave();
                return ECANCELED;
            }
            sched_v2_park();
        }
        sched_v2_set_park_reason(NULL);
        cc_external_wait_leave();
        return 0;
    }
    fiber_task* f = tls_current_fiber;
    if (!f || !f->coro) {
        cc_external_wait_leave();
        return 0;
    }
    f->external_wait_parked = 0;
    while (atomic_load_explicit(flag, memory_order_acquire) == expected) {
        if (cur_nursery && cc_nursery_is_cancelled(cur_nursery)) {
            cc_external_wait_leave();
            return ECANCELED;
        }
        cc__fiber_park_if(flag, expected, reason, file, line);
    }
    cc_external_wait_leave();
    return 0;
}

int cc__fiber_park_if_until(_Atomic int* flag, int expected, const struct timespec* abs_deadline,
                            const char* reason, const char* file, int line) {
    return cc__fiber_park_if_impl(flag, expected, abs_deadline, reason, file, line);
}

void cc__fiber_park(void) {
    if (sched_v2_in_context()) { sched_v2_park(); return; }
    cc__fiber_park_if(NULL, 0, "unknown", NULL, 0);
}

void cc__fiber_park_reason(const char* reason, const char* file, int line) {
    if (sched_v2_in_context()) {
        (void)file; (void)line;
        sched_v2_set_park_reason(reason);
        sched_v2_park();
        sched_v2_set_park_reason(NULL);
        return;
    }
    cc__fiber_park_if(NULL, 0, reason, file, line);
}

void cc__fiber_set_park_obj(void* obj) {
    fiber_task* f = tls_current_fiber;
    if (f) {
        f->park_obj = obj;
        return;
    }
    /* V2-fiber-on-V2-worker path: pin park_obj on the V2 fiber so the V2
     * deadlock detector (running on sysmon) can correlate the parked
     * fiber with the waitable it's blocked on (channel, join target,
     * etc). Exactly mirrors the V1 field on fiber_task. */
    sched_v2_fiber_set_park_obj(sched_v2_current_fiber(), obj);
}

void cc__fiber_clear_pending_unpark(void) {
    if (sched_v2_in_context()) return; /* V2 fibers: CAS-based, no pending_unpark */
    fiber_task* f = tls_current_fiber;
    if (!f) return;
    int old = atomic_exchange_explicit(&f->pending_unpark, 0, memory_order_acq_rel);
    if (old) {
        static int park_dbg = -1;
        if (park_dbg == -1) {
            park_dbg = 0;
        }
    }
}

uint64_t cc__fiber_publish_wait_ticket(void* fiber_ptr) {
    if (!fiber_ptr) return 0;
    if (cc__is_v2_fiber_local((cc__fiber*)fiber_ptr)) {
        return sched_v2_fiber_publish_wait_ticket(cc__untag_v2_fiber_local((cc__fiber*)fiber_ptr));
    }
    fiber_task* f = (fiber_task*)fiber_ptr;
    return atomic_fetch_add_explicit(&f->wait_ticket, 1, memory_order_acq_rel) + 1;
}

int cc__fiber_wait_ticket_matches(void* fiber_ptr, uint64_t ticket) {
    if (!fiber_ptr) return 0;
    if (cc__is_v2_fiber_local((cc__fiber*)fiber_ptr)) {
        return sched_v2_fiber_wait_ticket_matches(cc__untag_v2_fiber_local((cc__fiber*)fiber_ptr), ticket);
    }
    fiber_task* f = (fiber_task*)fiber_ptr;
    uint64_t cur = atomic_load_explicit(&f->wait_ticket, memory_order_acquire);
    return cur == ticket;
}

static void cc__fiber_unpark_impl(void* fiber_ptr) {
    if (!fiber_ptr) return;
    if ((uintptr_t)fiber_ptr & 1) {
        sched_v2_signal((fiber_v2*)((uintptr_t)fiber_ptr & ~(uintptr_t)1));
        return;
    }
}

void cc__fiber_unpark_tagged(void* fiber_ptr, cc__fiber_unpark_reason reason) {
    if ((unsigned)reason >= CC_FIBER_UNPARK_REASON_COUNT) {
        reason = CC_FIBER_UNPARK_REASON_GENERIC;
    }
    SCHED_DIAG_STAT_INC(g_cc_fiber_unpark_by_reason[reason]);
    cc__fiber_unpark_impl(fiber_ptr);
}

void cc__fiber_unpark(void* fiber_ptr) {
    cc__fiber_unpark_tagged(fiber_ptr, CC_FIBER_UNPARK_REASON_GENERIC);
}


void cc__fiber_unpark_channel_attrib(uint32_t attrib_flags) {
    /* V1 retired: the attribute flags steered V1 worker-pool wake heuristics
     * (sleepers/startup spin tracking) that no longer exist. V2 dispatches
     * wakes directly via sched_v2_signal on each waiter, so this is a no-op. */
    (void)attrib_flags;
}

void cc__fiber_sched_enqueue(void* fiber_ptr) {
    cc__fiber_unpark_tagged(fiber_ptr, CC_FIBER_UNPARK_REASON_ENQUEUE);
}

/* Cooperative yield: give other fibers a chance to run.
* Re-enqueues current fiber and switches to scheduler.
* Used for fairness in producer-consumer patterns. */
void cc__fiber_yield(void) {
    if (sched_v2_in_context()) {
        sched_v2_yield();
        return;
    }
    sched_yield();
}

/* Public API: cooperative yield for user code.
* Equivalent to Go's runtime.Gosched() — re-enqueues the current fiber
* on the local worker queue and switches to the scheduler.
* Falls back to sched_yield() outside a fiber context. */
void cc_yield(void) {
    cc__fiber_yield();
}

/* Yield to the GLOBAL run queue.
* Unlike cc__fiber_yield which pushes to the local queue (where the same
* worker immediately re-pops it), this puts the fiber in the global queue.
* Other workers can steal it, and fibers already in the global queue
* (e.g. a closer fiber) get a fair turn. */
void cc__fiber_yield_global(void) {
    if (sched_v2_in_context()) {
        sched_v2_yield();
        return;
    }
    sched_yield();
}

/* Signal to the sysmon that the current worker is still alive and doing
* productive work.  Call this from long-running tasks that do not yield
* (e.g. CPU-bound pool tasks) to prevent the orphan-threshold detector
* from treating the worker as "stuck" and spawning hybrid-promotion
* threads unnecessarily. */
/* Park the current fiber for `ms` milliseconds.
* The fiber is removed from the run queue entirely and placed on the
* sleep queue.  Sysmon drains expired sleepers every ~250µs.
* This avoids the O(N) queue churn of yield-and-recheck. */
void cc__fiber_sleep_park(unsigned int ms) {
    fiber_task* current = tls_current_fiber;
    if (!current || !current->coro) {
        /* Not in fiber context — fall back to OS sleep */
        struct timespec ts = { .tv_sec = ms / 1000,
                            .tv_nsec = (long)(ms % 1000) * 1000000L };
        while (nanosleep(&ts, &ts) == -1 && errno == EINTR) {}
        return;
    }
    /* Compute deadline */
    clock_gettime(CLOCK_MONOTONIC, &current->sleep_deadline);
    uint64_t ns = (uint64_t)ms * 1000000ULL;
    current->sleep_deadline.tv_nsec += (long)(ns % 1000000000ULL);
    current->sleep_deadline.tv_sec  += (time_t)(ns / 1000000000ULL);
    if (current->sleep_deadline.tv_nsec >= 1000000000L) {
        current->sleep_deadline.tv_nsec -= 1000000000L;
        current->sleep_deadline.tv_sec  += 1;
    }
    /* Yield-before-commit: trampoline will place on sleep queue. */
    current->yield_dest = YIELD_SLEEP;
    mco_yield(current->coro);
}

int cc__fiber_sched_active(void) {
    return atomic_load_explicit(&g_initialized, memory_order_acquire) == 2;
}

