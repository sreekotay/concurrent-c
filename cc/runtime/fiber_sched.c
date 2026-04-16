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

static size_t cc_page_size(void) {
    static size_t ps = 0;
    if (!ps) ps = (size_t)sysconf(_SC_PAGESIZE);
    return ps;
}

static void* cc_guarded_alloc(size_t size, void* allocator_data) {
    (void)allocator_data;
    size_t pg = cc_page_size();
    size_t total = size + pg;
    void* ptr = mmap(NULL, total, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (ptr == MAP_FAILED) return NULL;
    if (mprotect(ptr, pg, PROT_NONE) != 0) {
        munmap(ptr, total);
        return NULL;
    }
    return (char*)ptr + pg;
}

static void cc_guarded_dealloc(void* ptr, size_t size, void* allocator_data) {
    (void)allocator_data;
    size_t pg = cc_page_size();
    void* real = (char*)ptr - pg;
    munmap(real, size + pg);
}

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

static int g_timing_enabled = -1;  /* -1 = not checked, 0 = disabled, 1 = enabled */
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

static inline int cc_join_help_enabled(void) {
    int mode = atomic_load_explicit(&g_cc_join_help_mode, memory_order_acquire);
    if (mode >= 0) return mode;
    const char* env = getenv("CC_JOIN_HELP");
    mode = (!env || env[0] != '0') ? 1 : 0;
    int expected = -1;
    if (!atomic_compare_exchange_strong_explicit(&g_cc_join_help_mode,
                                                 &expected,
                                                 mode,
                                                 memory_order_release,
                                                 memory_order_acquire)) {
        mode = atomic_load_explicit(&g_cc_join_help_mode, memory_order_acquire);
    }
    return mode;
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
bool cc_nursery_is_cancelled(const CCNursery* n);
/* cc__tls_current_nursery is the per-OS-thread nursery context pointer.
* Fibers must restore it on migration; see worker_run_fiber(). */
extern __thread CCNursery* cc__tls_current_nursery;
static __thread CCNursery* cc__tls_spawn_nursery_override = NULL;

#if CC_RUNTIME_V3
struct fiber_task;
struct fiber_task* cc_sched_worker_next(void);
struct fiber_task* cc_sched_worker_idle_probe(void);
#endif
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
* ============================================================================ */

#define SPIN_FAST_ITERS_DEFAULT 128
#define SPIN_YIELD_ITERS_DEFAULT 8

static _Atomic int g_spin_fast_iters = -1;   /* -1 = not initialized */
static _Atomic int g_spin_yield_iters = -1;

static int get_spin_fast_iters(void) {
    int val = atomic_load_explicit(&g_spin_fast_iters, memory_order_acquire);
    if (val < 0) {
        const char* env = getenv("CC_SPIN_FAST_ITERS");
        int new_val = env ? atoi(env) : SPIN_FAST_ITERS_DEFAULT;
        if (new_val < 0) new_val = SPIN_FAST_ITERS_DEFAULT;
        /* Use compare-and-swap to ensure only one thread initializes */
        int expected = -1;
        if (atomic_compare_exchange_strong_explicit(&g_spin_fast_iters, &expected, new_val,
                                                    memory_order_release,
                                                    memory_order_acquire)) {
            val = new_val;
        } else {
            /* Another thread initialized it, use their value */
            val = atomic_load_explicit(&g_spin_fast_iters, memory_order_acquire);
        }
    }
    return val;
}

static int get_spin_yield_iters(void) {
    int val = atomic_load_explicit(&g_spin_yield_iters, memory_order_acquire);
    if (val < 0) {
        const char* env = getenv("CC_SPIN_YIELD_ITERS");
        int new_val = env ? atoi(env) : SPIN_YIELD_ITERS_DEFAULT;
        if (new_val < 0) new_val = SPIN_YIELD_ITERS_DEFAULT;
        /* Use compare-and-swap to ensure only one thread initializes */
        int expected = -1;
        if (atomic_compare_exchange_strong_explicit(&g_spin_yield_iters, &expected, new_val,
                                                    memory_order_release,
                                                    memory_order_acquire)) {
            val = new_val;
        } else {
            /* Another thread initialized it, use their value */
            val = atomic_load_explicit(&g_spin_yield_iters, memory_order_acquire);
        }
    }
    return val;
}

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
    struct fiber_task* debug_next;  /* For all-fibers debug list (deadlock dump) */
} fiber_task;

typedef struct {
    fiber_task* fiber;
    uint64_t generation;
} runnable_ref;

static inline int cc__fiber_deadlock_suppressed(const fiber_task* f) {
    return f && f->deadlock_suppress_depth > 0;
}

static inline int cc__fiber_external_wait_active(const fiber_task* f) {
    return f && f->external_wait_depth > 0;
}

static inline int cc__fiber_external_wait_scoped(const fiber_task* f) {
    return f && f->external_wait_scoped;
}

typedef struct {
    fiber_task* _Atomic fiber;
    _Atomic uint64_t generation;
} runnable_slot;

typedef struct fiber_overflow_node {
    runnable_ref ref;
    struct fiber_overflow_node* next;
} fiber_overflow_node;

static inline runnable_ref runnable_ref_null(void) {
    runnable_ref ref = {0};
    return ref;
}

static inline runnable_ref runnable_ref_snapshot(fiber_task* f) {
    runnable_ref ref = {
        .fiber = f,
        .generation = atomic_load_explicit(&f->generation, memory_order_relaxed),
    };
    return ref;
}

static inline int runnable_ref_matches_current(runnable_ref ref) {
    if (!ref.fiber) return 0;
    return atomic_load_explicit(&ref.fiber->generation, memory_order_acquire) == ref.generation;
}

static inline runnable_ref runnable_ref_validate(runnable_ref ref) {
    return runnable_ref_matches_current(ref) ? ref : runnable_ref_null();
}

static inline void runnable_slot_publish(runnable_slot* slot, runnable_ref ref) {
    atomic_store_explicit(&slot->generation, ref.generation, memory_order_relaxed);
    atomic_store_explicit(&slot->fiber, ref.fiber, memory_order_release);
}

static inline fiber_task* runnable_slot_load_fiber(runnable_slot* slot) {
    return atomic_load_explicit(&slot->fiber, memory_order_acquire);
}

static inline runnable_ref runnable_slot_snapshot_and_clear(runnable_slot* slot, fiber_task* f) {
    runnable_ref ref = {
        .fiber = f,
        .generation = atomic_load_explicit(&slot->generation, memory_order_relaxed),
    };
    atomic_store_explicit(&slot->fiber, NULL, memory_order_relaxed);
    atomic_store_explicit(&slot->generation, 0, memory_order_relaxed);
    return ref;
}

static inline runnable_ref runnable_slot_take_exchange(runnable_slot* slot) {
    fiber_task* f = atomic_exchange_explicit(&slot->fiber, NULL, memory_order_acquire);
    if (!f) return runnable_ref_null();
    runnable_ref ref = {
        .fiber = f,
        .generation = atomic_load_explicit(&slot->generation, memory_order_relaxed),
    };
    atomic_store_explicit(&slot->generation, 0, memory_order_relaxed);
    return ref;
}

typedef enum {
    CC_WL_ACTIVE = 0,
    CC_WL_IDLE_SPIN = 1,
    CC_WL_SLEEP = 2,
    CC_WL_DRAINING = 3,
    CC_WL_DEAD = 4,
} cc_worker_lifecycle;

/* Optional spec assertion mode for transition/enqueue legality checks.
* Enable with CC_V3_SPEC_ASSERT=1 when auditing §9/§10 linearization points. */
static inline int cc_v3_spec_assert_enabled(void) {
    static int enabled = -1;
    if (enabled < 0) {
        const char* v = getenv("CC_V3_SPEC_ASSERT");
        enabled = (v && v[0] == '1') ? 1 : 0;
    }
    return enabled;
}

static inline void cc_v3_assert_enqueue_runnable(fiber_task* f, const char* queue_name) {
    if (!cc_v3_spec_assert_enabled() || !f) return;
    int64_t ctrl = atomic_load_explicit(&f->control, memory_order_acquire);
    if (ctrl != CTRL_QUEUED) {
        fprintf(stderr,
                "CC_V3_SPEC_ASSERT: enqueue_non_runnable queue=%s fiber=%lu control=%lld\n",
                queue_name,
                (unsigned long)f->fiber_id,
                (long long)ctrl);
        abort();
    }
}

static inline void cc_v3_assert_control_is(fiber_task* f, int64_t expected_ctrl, const char* edge_name) {
    if (!cc_v3_spec_assert_enabled() || !f) return;
    int64_t cur = atomic_load_explicit(&f->control, memory_order_acquire);
    if (cur != expected_ctrl) {
        fprintf(stderr,
                "CC_V3_SPEC_ASSERT: transition_mismatch edge=%s fiber=%lu expected=%lld actual=%lld\n",
                edge_name,
                (unsigned long)f->fiber_id,
                (long long)expected_ctrl,
                (long long)cur);
        abort();
    }
}

static inline void cc_v3_assert_matrix_row(fiber_task* f, const char* row_name, int predicate) {
    if (!cc_v3_spec_assert_enabled() || !f) return;
    if (predicate) return;
    fprintf(stderr,
            "CC_V3_SPEC_ASSERT: matrix_row_violation row=%s fiber=%lu\n",
            row_name ? row_name : "(unknown)",
            (unsigned long)f->fiber_id);
    abort();
}

static inline int cc_v3_worker_lifecycle_enabled(void) {
    static int enabled = -1;
    if (enabled < 0) {
        const char* v = getenv("CC_V3_WORKER_LIFECYCLE_ASSERT");
        enabled = (v && v[0] == '1') ? 1 : 0;
    }
    return enabled;
}

static inline int64_t cc_sched_pressure_add(int64_t delta);

static inline const char* cc_v3_worker_lifecycle_name(cc_worker_lifecycle st) {
    switch (st) {
        case CC_WL_ACTIVE: return "ACTIVE";
        case CC_WL_IDLE_SPIN: return "IDLE_SPIN";
        case CC_WL_SLEEP: return "SLEEP";
        case CC_WL_DRAINING: return "DRAINING";
        case CC_WL_DEAD: return "DEAD";
        default: return "UNKNOWN";
    }
}

static inline const char* cc__last_worker_src_name(unsigned char src) {
    switch (src) {
        case 1: return "run";
        case 2: return "affinity";
        case 3: return "chan_partner";
        default: return "none";
    }
}

static inline const char* cc__enqueue_src_name(int src) {
    switch (src) {
        case 1: return "yield";
        case 2: return "spawn";
        case 3: return "park_abort_pending";
        case 4: return "park_abort_flag";
        case 5: return "park_abort_cas";
        case 6: return "park_self_unpark";
        case 7: return "park_post_flag";
        case 8: return "unpark";
        case 9: return "commit_park_abort";
        default: return "none";
    }
}

static inline int cc_trace_nw_spawn_enabled(void) {
    static int enabled = -1;
    if (enabled < 0) {
        enabled = 0;
    }
    return enabled;
}

static inline int cc_trace_chan_wake_enabled_sched(void) {
    static int enabled = -1;
    if (enabled < 0) {
        enabled = 0;
    }
    return enabled;
}

static inline int cc_trace_spawn_run_enabled(void) {
    static int enabled = -1;
    if (enabled < 0) {
        enabled = 0;
    }
    return enabled;
}

static inline int cc_trace_fiber_migrate_enabled(void) {
    static int enabled = -1;
    if (enabled < 0) {
        enabled = 0;
    }
    return enabled;
}

static inline uint64_t cc_trace_fiber_migrate_limit(void) {
    return 0;
}

static inline const char* cc__spawn_publish_route_name(unsigned char route) {
    switch (route) {
        case 1: return "local";
        case 2: return "inbox";
        case 3: return "global";
        default: return "none";
    }
}

static inline int cc_nonworker_spawn_group_enabled(void) {
    static int enabled = -1;
    if (enabled < 0) {
        enabled = 0;
    }
    return enabled;
}

static inline int cc_nonworker_keep_sleeping_target_enabled(void) {
    static int enabled = -1;
    if (enabled < 0) {
        const char* v = getenv("CC_NW_SPAWN_KEEP_SLEEPING");
        enabled = (!v || v[0] != '0') ? 1 : 0;
    }
    return enabled;
}

static inline int cc_v3_worker_lifecycle_is_legal(cc_worker_lifecycle from, cc_worker_lifecycle to) {
    if (from == to) return 1;
    switch (from) {
        case CC_WL_ACTIVE:
            return to == CC_WL_IDLE_SPIN || to == CC_WL_SLEEP || to == CC_WL_DRAINING || to == CC_WL_DEAD;
        case CC_WL_IDLE_SPIN:
            return to == CC_WL_ACTIVE || to == CC_WL_SLEEP || to == CC_WL_DRAINING || to == CC_WL_DEAD;
        case CC_WL_SLEEP:
            return to == CC_WL_ACTIVE || to == CC_WL_IDLE_SPIN || to == CC_WL_DRAINING || to == CC_WL_DEAD;
        case CC_WL_DRAINING:
            return to == CC_WL_DEAD;
        case CC_WL_DEAD:
        default:
            return to == CC_WL_ACTIVE || to == CC_WL_DEAD;
    }
}

static inline void cc_v3_worker_lifecycle_set(int worker_id, cc_worker_lifecycle next, const char* edge);


#if CC_RUNTIME_V3 && CC_V3_DIAGNOSTICS
static _Atomic int g_cc_v3_worker_stats_mode = -1;
static _Atomic int g_cc_v3_worker_stats_atexit = 0;
static _Atomic int g_cc_v3_worker_dump_mode = -1;
static _Atomic uint64_t g_cc_v3_wm_batches = 0;
static _Atomic uint64_t g_cc_v3_wm_seed_v3_batches = 0;
static _Atomic uint64_t g_cc_v3_wm_local_pulls = 0;
static _Atomic uint64_t g_cc_v3_wm_inbox_pulls = 0;
static _Atomic uint64_t g_cc_v3_wm_global_pulls = 0;
static _Atomic uint64_t g_cc_v3_wm_steal_inbox_pulls = 0;
static _Atomic uint64_t g_cc_v3_wm_steal_local_pulls = 0;
static _Atomic uint64_t g_cc_v3_wm_idle_probe_hits = 0;
static _Atomic uint64_t g_cc_v3_wm_empty_loops = 0;

static inline int cc_v3_worker_stats_enabled(void) {
    int mode = atomic_load_explicit(&g_cc_v3_worker_stats_mode, memory_order_acquire);
    if (mode >= 0) return mode;
    mode = 0;
    int expected = -1;
    (void)atomic_compare_exchange_strong_explicit(&g_cc_v3_worker_stats_mode,
                                                &expected,
                                                mode,
                                                memory_order_release,
                                                memory_order_acquire);
    return atomic_load_explicit(&g_cc_v3_worker_stats_mode, memory_order_acquire);
}

static inline int cc_v3_worker_dump_enabled(void) {
    int mode = atomic_load_explicit(&g_cc_v3_worker_dump_mode, memory_order_acquire);
    if (mode >= 0) return mode;
    mode = 0;
    int expected = -1;
    (void)atomic_compare_exchange_strong_explicit(&g_cc_v3_worker_dump_mode,
                                                &expected,
                                                mode,
                                                memory_order_release,
                                                memory_order_acquire);
    return atomic_load_explicit(&g_cc_v3_worker_dump_mode, memory_order_acquire);
}

static void cc_v3_dump_worker_stats(void) {
    if (!cc_v3_worker_stats_enabled()) return;
    uint64_t batches = atomic_load_explicit(&g_cc_v3_wm_batches, memory_order_relaxed);
    uint64_t seed_v3 = atomic_load_explicit(&g_cc_v3_wm_seed_v3_batches, memory_order_relaxed);
    uint64_t local = atomic_load_explicit(&g_cc_v3_wm_local_pulls, memory_order_relaxed);
    uint64_t inbox = atomic_load_explicit(&g_cc_v3_wm_inbox_pulls, memory_order_relaxed);
    uint64_t global = atomic_load_explicit(&g_cc_v3_wm_global_pulls, memory_order_relaxed);
    uint64_t steal_inbox = atomic_load_explicit(&g_cc_v3_wm_steal_inbox_pulls, memory_order_relaxed);
    uint64_t steal_local = atomic_load_explicit(&g_cc_v3_wm_steal_local_pulls, memory_order_relaxed);
    uint64_t idle_hits = atomic_load_explicit(&g_cc_v3_wm_idle_probe_hits, memory_order_relaxed);
    uint64_t empty = atomic_load_explicit(&g_cc_v3_wm_empty_loops, memory_order_relaxed);
    uint64_t pulls = local + inbox + global + steal_inbox + steal_local;
    if (batches == 0 && pulls == 0 && idle_hits == 0 && empty == 0) return;
    fprintf(stderr,
            "\n=== V3 WORKER MAIN STATS ===\n"
            "batches=%llu seed_v3_batches=%llu idle_probe_hits=%llu empty_loops=%llu\n"
            "pulls local=%llu inbox=%llu global=%llu steal_inbox=%llu steal_local=%llu total=%llu\n"
            "pull_pct local=%.1f inbox=%.1f global=%.1f steal_inbox=%.1f steal_local=%.1f\n"
            "============================\n\n",
            (unsigned long long)batches,
            (unsigned long long)seed_v3,
            (unsigned long long)idle_hits,
            (unsigned long long)empty,
            (unsigned long long)local,
            (unsigned long long)inbox,
            (unsigned long long)global,
            (unsigned long long)steal_inbox,
            (unsigned long long)steal_local,
            (unsigned long long)pulls,
            pulls ? (100.0 * (double)local / (double)pulls) : 0.0,
            pulls ? (100.0 * (double)inbox / (double)pulls) : 0.0,
            pulls ? (100.0 * (double)global / (double)pulls) : 0.0,
            pulls ? (100.0 * (double)steal_inbox / (double)pulls) : 0.0,
            pulls ? (100.0 * (double)steal_local / (double)pulls) : 0.0);
}

static inline void cc_v3_worker_stats_maybe_init(void) {
    if (!cc_v3_worker_stats_enabled() || !cc_v3_worker_dump_enabled()) return;
    int expected = 0;
    if (atomic_compare_exchange_strong_explicit(&g_cc_v3_worker_stats_atexit,
                                                &expected,
                                                1,
                                                memory_order_acq_rel,
                                                memory_order_acquire)) {
        atexit(cc_v3_dump_worker_stats);
    }
}
#endif

typedef struct {
    _Atomic uint64_t spin_entries;
    _Atomic uint64_t yield_calls;
    _Atomic uint64_t yield_found_work;
    _Atomic uint64_t sleep_entries;
    _Atomic uint64_t sleep_wait_calls;
    _Atomic uint64_t sleep_exits;
    _Atomic uint64_t wake_one_calls;
    _Atomic uint64_t wake_one_with_sleepers;
    _Atomic uint64_t wake_one_delivered;
    _Atomic uint64_t wake_target_calls;
    _Atomic uint64_t wake_target_delivered;
    _Atomic uint64_t wake_target_skipped_not_sleeping;
    _Atomic uint64_t wake_unconditional_calls;
    _Atomic uint64_t wake_unconditional_delivered;
    _Atomic uint64_t wake_unconditional_no_sleepers;
} cc_sched_wait_stats;

static cc_sched_wait_stats g_cc_sched_wait_stats = {0};

typedef struct {
    _Atomic uint64_t nonworker_global_publish;
    _Atomic uint64_t nonworker_global_edge;
    _Atomic uint64_t nonworker_global_wake_one;
    _Atomic uint64_t global_pop_hits;
    _Atomic uint64_t run_attempts;
    _Atomic uint64_t run_claims;
    _Atomic uint64_t run_skip_stale;
} cc_sched_io_wake_stats;

static cc_sched_io_wake_stats g_cc_sched_io_wake_stats = {0};

static int cc_sched_wait_stats_enabled(void) {
    static int mode = -1;
    if (mode >= 0) return mode;
    const char* env = getenv("CC_SCHED_WAIT_STATS");
    mode = (env && env[0] && !(env[0] == '0' && env[1] == '\0')) ? 1 : 0;
    return mode;
}

static inline void cc_sched_wait_stat_inc(_Atomic uint64_t* counter) {
    if (!cc_sched_wait_stats_enabled()) return;
    atomic_fetch_add_explicit(counter, 1, memory_order_relaxed);
}

static int cc_sched_io_wake_stats_enabled(void) {
    static int mode = -1;
    if (mode >= 0) return mode;
    const char* env = getenv("CC_SCHED_IO_WAKE_STATS");
    mode = (env && env[0] && !(env[0] == '0' && env[1] == '\0')) ? 1 : 0;
    return mode;
}

static inline void cc_sched_io_wake_stat_inc(_Atomic uint64_t* counter) {
    if (!cc_sched_io_wake_stats_enabled()) return;
    atomic_fetch_add_explicit(counter, 1, memory_order_relaxed);
}

static void cc_sched_wait_stats_dump(void) {
    if (!cc_sched_wait_stats_enabled()) return;
    uint64_t spin_entries = atomic_load_explicit(&g_cc_sched_wait_stats.spin_entries, memory_order_relaxed);
    uint64_t yield_calls = atomic_load_explicit(&g_cc_sched_wait_stats.yield_calls, memory_order_relaxed);
    uint64_t yield_found_work = atomic_load_explicit(&g_cc_sched_wait_stats.yield_found_work, memory_order_relaxed);
    uint64_t sleep_entries = atomic_load_explicit(&g_cc_sched_wait_stats.sleep_entries, memory_order_relaxed);
    uint64_t sleep_wait_calls = atomic_load_explicit(&g_cc_sched_wait_stats.sleep_wait_calls, memory_order_relaxed);
    uint64_t sleep_exits = atomic_load_explicit(&g_cc_sched_wait_stats.sleep_exits, memory_order_relaxed);
    uint64_t wake_one_calls = atomic_load_explicit(&g_cc_sched_wait_stats.wake_one_calls, memory_order_relaxed);
    uint64_t wake_one_with_sleepers = atomic_load_explicit(&g_cc_sched_wait_stats.wake_one_with_sleepers, memory_order_relaxed);
    uint64_t wake_one_delivered = atomic_load_explicit(&g_cc_sched_wait_stats.wake_one_delivered, memory_order_relaxed);
    uint64_t wake_target_calls = atomic_load_explicit(&g_cc_sched_wait_stats.wake_target_calls, memory_order_relaxed);
    uint64_t wake_target_delivered = atomic_load_explicit(&g_cc_sched_wait_stats.wake_target_delivered, memory_order_relaxed);
    uint64_t wake_target_skipped = atomic_load_explicit(&g_cc_sched_wait_stats.wake_target_skipped_not_sleeping, memory_order_relaxed);
    uint64_t wake_unconditional_calls = atomic_load_explicit(&g_cc_sched_wait_stats.wake_unconditional_calls, memory_order_relaxed);
    uint64_t wake_unconditional_delivered = atomic_load_explicit(&g_cc_sched_wait_stats.wake_unconditional_delivered, memory_order_relaxed);
    uint64_t wake_unconditional_no_sleepers = atomic_load_explicit(&g_cc_sched_wait_stats.wake_unconditional_no_sleepers, memory_order_relaxed);
    if (spin_entries == 0 && yield_calls == 0 && sleep_entries == 0 &&
        wake_one_calls == 0 && wake_target_calls == 0 && wake_unconditional_calls == 0) {
        return;
    }
    fprintf(stderr,
            "\n[cc:sched_wait] spin_entries=%llu yield_calls=%llu yield_found_work=%llu "
            "sleep_entries=%llu sleep_wait_calls=%llu sleep_exits=%llu\n"
            "[cc:sched_wait] wake_one calls=%llu sleepers=%llu delivered=%llu "
            "wake_target calls=%llu delivered=%llu skipped_not_sleeping=%llu "
            "wake_unconditional calls=%llu delivered=%llu no_sleepers=%llu\n",
            (unsigned long long)spin_entries,
            (unsigned long long)yield_calls,
            (unsigned long long)yield_found_work,
            (unsigned long long)sleep_entries,
            (unsigned long long)sleep_wait_calls,
            (unsigned long long)sleep_exits,
            (unsigned long long)wake_one_calls,
            (unsigned long long)wake_one_with_sleepers,
            (unsigned long long)wake_one_delivered,
            (unsigned long long)wake_target_calls,
            (unsigned long long)wake_target_delivered,
            (unsigned long long)wake_target_skipped,
            (unsigned long long)wake_unconditional_calls,
            (unsigned long long)wake_unconditional_delivered,
            (unsigned long long)wake_unconditional_no_sleepers);
}

static void cc_sched_io_wake_stats_dump(void) {
    if (!cc_sched_io_wake_stats_enabled()) return;
    uint64_t nonworker_global_publish = atomic_load_explicit(&g_cc_sched_io_wake_stats.nonworker_global_publish, memory_order_relaxed);
    uint64_t nonworker_global_edge = atomic_load_explicit(&g_cc_sched_io_wake_stats.nonworker_global_edge, memory_order_relaxed);
    uint64_t nonworker_global_wake_one = atomic_load_explicit(&g_cc_sched_io_wake_stats.nonworker_global_wake_one, memory_order_relaxed);
    uint64_t global_pop_hits = atomic_load_explicit(&g_cc_sched_io_wake_stats.global_pop_hits, memory_order_relaxed);
    uint64_t run_attempts = atomic_load_explicit(&g_cc_sched_io_wake_stats.run_attempts, memory_order_relaxed);
    uint64_t run_claims = atomic_load_explicit(&g_cc_sched_io_wake_stats.run_claims, memory_order_relaxed);
    uint64_t run_skip_stale = atomic_load_explicit(&g_cc_sched_io_wake_stats.run_skip_stale, memory_order_relaxed);
    if (nonworker_global_publish == 0 && global_pop_hits == 0 && run_attempts == 0) {
        return;
    }
    fprintf(stderr,
            "[cc:sched_io_wake] nonworker_global publish=%llu edge=%llu wake_one=%llu "
            "global_pop_hits=%llu run_attempts=%llu run_claims=%llu run_skip_stale=%llu\n",
            (unsigned long long)nonworker_global_publish,
            (unsigned long long)nonworker_global_edge,
            (unsigned long long)nonworker_global_wake_one,
            (unsigned long long)global_pop_hits,
            (unsigned long long)run_attempts,
            (unsigned long long)run_claims,
            (unsigned long long)run_skip_stale);
}

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

static void fq_init(fiber_queue* q) {
    memset(q->slots, 0, sizeof(q->slots));
    atomic_store(&q->head, 0);
    atomic_store(&q->tail, 0);
    atomic_store(&q->nonempty_hint, 0);
    pthread_mutex_init(&q->overflow_mu, NULL);
    q->overflow_head = NULL;
    q->overflow_tail = NULL;
    atomic_store(&q->overflow_count, 0);
}

/* Try to push to the lock-free ring. Returns 0 on success, -1 if full. */
static int fq_push_ring(fiber_queue* q, fiber_task* f) {
    cc_v3_assert_enqueue_runnable(f, "global_ring");
    runnable_ref ref = runnable_ref_snapshot(f);
    int spins = 0;
    for (int retry = 0; retry < 64; retry++) {
        size_t tail = atomic_load_explicit(&q->tail, memory_order_relaxed);
        size_t head = atomic_load_explicit(&q->head, memory_order_acquire);

        if (tail - head >= CC_FIBER_QUEUE_INITIAL) {
            return -1;  /* Ring full */
        }

        if (atomic_compare_exchange_weak_explicit(&q->tail, &tail, tail + 1,
                                                memory_order_release,
                                                memory_order_relaxed)) {
            size_t idx = tail % CC_FIBER_QUEUE_INITIAL;
            runnable_slot_publish(&q->slots[idx], ref);
            atomic_store_explicit(&q->nonempty_hint, 1, memory_order_release);
            return 0;
        }
        for (int i = 0; i <= spins; i++)
            cpu_pause();
        spins++;
    }
    return -1;
}

/* Push: try ring first, fall back to overflow list. Never blocks. */
static void fq_push_blocking(fiber_queue* q, fiber_task* f) {
    if (fq_push_ring(q, f) == 0) return;
    
    /* Ring full — use overflow list with a publication snapshot. */
    fiber_overflow_node* node = (fiber_overflow_node*)malloc(sizeof(*node));
    if (!node) {
        while (fq_push_ring(q, f) != 0) {
            sched_yield();
        }
        return;
    }
    node->ref = runnable_ref_snapshot(f);
    node->next = NULL;
    pthread_mutex_lock(&q->overflow_mu);
    if (q->overflow_tail) {
        q->overflow_tail->next = node;
    } else {
        q->overflow_head = node;
    }
    q->overflow_tail = node;
    atomic_fetch_add_explicit(&q->overflow_count, 1, memory_order_relaxed);
    atomic_store_explicit(&q->nonempty_hint, 1, memory_order_release);
    pthread_mutex_unlock(&q->overflow_mu);
}

/* Check if queue has items (non-destructive peek) */
static int fq_peek(fiber_queue* q) {
    if (!atomic_load_explicit(&q->nonempty_hint, memory_order_acquire)) {
        return 0;
    }
    size_t head = atomic_load_explicit(&q->head, memory_order_relaxed);
    size_t tail = atomic_load_explicit(&q->tail, memory_order_acquire);
    if (head < tail) return 1;
    int has_overflow = atomic_load_explicit(&q->overflow_count, memory_order_relaxed) > 0;
    if (!has_overflow) {
        atomic_store_explicit(&q->nonempty_hint, 0, memory_order_release);
    }
    return has_overflow;
}

/* Pop from overflow list (caller should try ring first). */
static runnable_ref fq_pop_overflow(fiber_queue* q) {
    if (atomic_load_explicit(&q->overflow_count, memory_order_relaxed) == 0)
        return runnable_ref_null();
    pthread_mutex_lock(&q->overflow_mu);
    fiber_overflow_node* node = q->overflow_head;
    runnable_ref ref = runnable_ref_null();
    if (node) {
        q->overflow_head = node->next;
        if (!q->overflow_head) q->overflow_tail = NULL;
        atomic_fetch_sub_explicit(&q->overflow_count, 1, memory_order_relaxed);
        ref = node->ref;
    }
    pthread_mutex_unlock(&q->overflow_mu);
    if (node) free(node);
    return ref;
}

static runnable_ref fq_pop_overflow_live(fiber_queue* q) {
    runnable_ref ref = fq_pop_overflow(q);
    while (ref.fiber) {
        ref = runnable_ref_validate(ref);
        if (ref.fiber) return ref;
        ref = fq_pop_overflow(q);
    }
    return runnable_ref_null();
}

static runnable_ref fq_pop(fiber_queue* q) {
    if (!atomic_load_explicit(&q->nonempty_hint, memory_order_acquire)) {
        return runnable_ref_null();
    }
    /* Try lock-free ring first */
    for (int retry = 0; retry < 1000; retry++) {
        size_t head = atomic_load_explicit(&q->head, memory_order_relaxed);
        size_t tail = atomic_load_explicit(&q->tail, memory_order_acquire);
        
        if (head >= tail) break;  /* Ring empty, try overflow */
        
        size_t idx = head % CC_FIBER_QUEUE_INITIAL;
        fiber_task* f = runnable_slot_load_fiber(&q->slots[idx]);
        
        if (!f) {
            /* Writer incremented tail but hasn't written slot yet.
            * Spin-wait for the slot to be populated. */
            for (int i = 0; i < 100; i++) {
                cpu_pause();
                f = runnable_slot_load_fiber(&q->slots[idx]);
                if (f) break;
            }
            if (!f) continue;
        }
        
        if (atomic_compare_exchange_weak_explicit(&q->head, &head, head + 1,
                                                memory_order_relaxed,
                                                memory_order_relaxed)) {
            runnable_ref ref = runnable_slot_snapshot_and_clear(&q->slots[idx], f);
            size_t new_head = head + 1;
            size_t cur_tail = atomic_load_explicit(&q->tail, memory_order_acquire);
            if (new_head >= cur_tail &&
                atomic_load_explicit(&q->overflow_count, memory_order_relaxed) == 0) {
                atomic_store_explicit(&q->nonempty_hint, 0, memory_order_release);
            }
            ref = runnable_ref_validate(ref);
            if (!ref.fiber) continue;
            return ref;
        }
    }
    /* Ring empty — check overflow */
    runnable_ref of = fq_pop_overflow_live(q);
    if (!of.fiber) {
        size_t head = atomic_load_explicit(&q->head, memory_order_relaxed);
        size_t tail = atomic_load_explicit(&q->tail, memory_order_acquire);
        if (head >= tail &&
            atomic_load_explicit(&q->overflow_count, memory_order_relaxed) == 0) {
            atomic_store_explicit(&q->nonempty_hint, 0, memory_order_release);
        }
        return runnable_ref_null();
    }
    return of;
}

/* Global run-queue helpers (implemented after scheduler state declaration). */
static inline int sched_global_push(fiber_task* f, int preferred_worker);
static inline runnable_ref sched_global_pop_any(void);
static inline runnable_ref sched_global_pop_for_worker(int worker_id);
static inline int sched_global_peek_any(void);

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

static sleep_queue g_sleep_queue;

typedef struct {
    pthread_mutex_t mu;
    fiber_task* head;        /* Singly-linked list of timed parked fibers (via ->timer_next) */
    _Atomic size_t count;    /* Approximate queue size for quick peek */
} timed_park_queue;

static timed_park_queue g_timed_park_queue;
void cc__fiber_unpark(void* fiber_ptr);
void cc__fiber_unpark_tagged(void* fiber_ptr, cc__fiber_unpark_reason reason);

static void sq_init(void) {
    pthread_mutex_init(&g_sleep_queue.mu, NULL);
    g_sleep_queue.head = NULL;
    atomic_store(&g_sleep_queue.count, 0);
    pthread_mutex_init(&g_timed_park_queue.mu, NULL);
    g_timed_park_queue.head = NULL;
    atomic_store(&g_timed_park_queue.count, 0);
}

static void sq_destroy(void) {
    pthread_mutex_destroy(&g_sleep_queue.mu);
    pthread_mutex_destroy(&g_timed_park_queue.mu);
}

/* Park a fiber for sleep.  Caller must have already set f->sleep_deadline
* and transitioned the fiber control word to CTRL_QUEUED (or similar).
* The fiber will be resumed by sq_drain when its deadline passes. */
static void sq_push(fiber_task* f) {
    pthread_mutex_lock(&g_sleep_queue.mu);
    f->next = g_sleep_queue.head;
    g_sleep_queue.head = f;
    atomic_fetch_add_explicit(&g_sleep_queue.count, 1, memory_order_relaxed);
    pthread_mutex_unlock(&g_sleep_queue.mu);
}

/* Drain expired sleepers back into the run queue.
* Returns the number of fibers woken. */
static size_t sq_drain(void) {
    if (atomic_load_explicit(&g_sleep_queue.count, memory_order_relaxed) == 0)
        return 0;
    
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    
    pthread_mutex_lock(&g_sleep_queue.mu);
    fiber_task** pp = &g_sleep_queue.head;
    size_t woken = 0;
    while (*pp) {
        fiber_task* f = *pp;
        if (now.tv_sec > f->sleep_deadline.tv_sec ||
            (now.tv_sec == f->sleep_deadline.tv_sec &&
            now.tv_nsec >= f->sleep_deadline.tv_nsec)) {
            /* Deadline passed — remove from sleep list and enqueue */
            *pp = f->next;
            f->next = NULL;
            atomic_fetch_sub_explicit(&g_sleep_queue.count, 1, memory_order_relaxed);
            sched_global_push(f, f->last_worker_id);
            woken++;
        } else {
            pp = &f->next;
        }
    }
    pthread_mutex_unlock(&g_sleep_queue.mu);
    return woken;
}

static void tpq_push(fiber_task* f) {
    pthread_mutex_lock(&g_timed_park_queue.mu);
    f->timer_next = g_timed_park_queue.head;
    g_timed_park_queue.head = f;
    atomic_fetch_add_explicit(&g_timed_park_queue.count, 1, memory_order_relaxed);
    pthread_mutex_unlock(&g_timed_park_queue.mu);
}

static int tpq_remove_if_present(fiber_task* target) {
    if (!target) return 0;
    int removed = 0;
    pthread_mutex_lock(&g_timed_park_queue.mu);
    fiber_task** pp = &g_timed_park_queue.head;
    while (*pp) {
        fiber_task* f = *pp;
        if (f == target) {
            *pp = f->timer_next;
            if (atomic_load_explicit(&g_timed_park_queue.count, memory_order_relaxed) > 0) {
                atomic_fetch_sub_explicit(&g_timed_park_queue.count, 1, memory_order_relaxed);
            }
            f->timer_next = NULL;
            removed = 1;
            break;
        }
        pp = &f->timer_next;
    }
    pthread_mutex_unlock(&g_timed_park_queue.mu);
    return removed;
}

static size_t tpq_drain(void) {
    if (atomic_load_explicit(&g_timed_park_queue.count, memory_order_relaxed) == 0) {
        return 0;
    }

    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);

    pthread_mutex_lock(&g_timed_park_queue.mu);
    fiber_task** pp = &g_timed_park_queue.head;
    fiber_task* expired = NULL;
    size_t woken = 0;
    while (*pp) {
        fiber_task* f = *pp;
        int registered = atomic_load_explicit(&f->timed_park_registered, memory_order_acquire);
        int is_expired = registered &&
            (now.tv_sec > f->timed_park_deadline.tv_sec ||
            (now.tv_sec == f->timed_park_deadline.tv_sec &&
            now.tv_nsec >= f->timed_park_deadline.tv_nsec));
        if (!registered || is_expired) {
            *pp = f->timer_next;
            f->timer_next = expired;
            expired = f;
            atomic_fetch_sub_explicit(&g_timed_park_queue.count, 1, memory_order_relaxed);
            if (is_expired &&
                atomic_exchange_explicit(&f->timed_park_registered, 0, memory_order_acq_rel)) {
                atomic_store_explicit(&f->timed_park_fired, 1, memory_order_release);
                atomic_fetch_sub_explicit(&g_total_timed_parked, 1, memory_order_relaxed);
                if (cc__fiber_deadlock_suppressed(f)) {
                    atomic_fetch_sub_explicit(&g_deadlock_suppressed_timed_parked, 1, memory_order_relaxed);
                }
                woken++;
            }
        } else {
            pp = &f->timer_next;
        }
    }
    pthread_mutex_unlock(&g_timed_park_queue.mu);

    while (expired) {
        fiber_task* f = expired;
        expired = expired->timer_next;
        f->timer_next = NULL;
        if (atomic_load_explicit(&f->timed_park_fired, memory_order_acquire)) {
            cc__fiber_unpark_tagged(f, CC_FIBER_UNPARK_REASON_TIMER);
        }
    }
    return woken;
}

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





static void cc__fiber_dump_queue_state(void);

static int iq_push_with_edge(inbox_queue* q, fiber_task* f, int* was_empty) {
    if (was_empty) *was_empty = 0;
    cc_v3_assert_enqueue_runnable(f, "inbox");
    runnable_ref ref = runnable_ref_snapshot(f);
    int spins = 0;
    for (int retry = 0; retry < 1000; retry++) {
        size_t tail = atomic_load_explicit(&q->tail, memory_order_relaxed);
        size_t head = atomic_load_explicit(&q->head, memory_order_acquire);
        if (tail - head >= INBOX_QUEUE_SIZE) {
            sched_yield();
            continue;
        }
        if (atomic_compare_exchange_weak_explicit(&q->tail, &tail, tail + 1,
                                                memory_order_release,
                                                memory_order_relaxed)) {
            size_t idx = tail % INBOX_QUEUE_SIZE;
            runnable_slot_publish(&q->slots[idx], ref);
            if (was_empty) *was_empty = (tail == head);
            return 0;
        }
        for (int i = 0; i <= spins; i++)
            cpu_pause();
        spins++;
    }
    sched_yield();
    atomic_fetch_add_explicit(&g_inbox_overflow, 1, memory_order_relaxed);
    return -1;
}

static int iq_peek(inbox_queue* q) {
    size_t head = atomic_load_explicit(&q->head, memory_order_relaxed);
    size_t tail = atomic_load_explicit(&q->tail, memory_order_acquire);
    return head < tail;
}

/* Number of items currently in the inbox (approximate; used for spawn pairing). */
static inline size_t iq_depth(inbox_queue* q) {
    size_t head = atomic_load_explicit(&q->head, memory_order_relaxed);
    size_t tail = atomic_load_explicit(&q->tail, memory_order_acquire);
    return (tail > head) ? (tail - head) : 0;
}

static runnable_ref iq_pop(inbox_queue* q) {
    for (int retry = 0; retry < 1000; retry++) {
        size_t head = atomic_load_explicit(&q->head, memory_order_relaxed);
        size_t tail = atomic_load_explicit(&q->tail, memory_order_acquire);
        if (head >= tail) return runnable_ref_null();
        size_t idx = head % INBOX_QUEUE_SIZE;
        fiber_task* f = runnable_slot_load_fiber(&q->slots[idx]);
        if (!f) {
            /* Writer incremented tail but hasn't written slot yet.
            * Spin-wait for the slot to be populated. This window is very short
            * (just a single store instruction), so we spin aggressively. */
            for (int i = 0; i < 100; i++) {
                cpu_pause();
                f = runnable_slot_load_fiber(&q->slots[idx]);
                if (f) break;
            }
            if (!f) continue;  /* Retry from the top */
        }
        if (atomic_compare_exchange_weak_explicit(&q->head, &head, head + 1,
                                                memory_order_relaxed,
                                                memory_order_relaxed)) {
            runnable_ref ref = runnable_ref_validate(runnable_slot_snapshot_and_clear(&q->slots[idx], f));
            if (!ref.fiber) continue;
            return ref;
        }
    }
    return runnable_ref_null();
}

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
static _Atomic int g_deadlock_reported = 0;
static _Atomic uint64_t g_deadlock_first_seen = 0;  /* Timestamp when deadlock state first seen */
static _Atomic size_t g_external_wait_parked = 0;
static _Atomic size_t g_external_wait_threads = 0;
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

static inline const char* cc_wake_reason_name(cc_wake_reason reason) {
    switch (reason) {
        case CC_WAKE_REASON_SYSMON_SLEEPQ: return "sysmon_sleepq";
        case CC_WAKE_REASON_SPAWN_GLOBAL_EDGE: return "spawn_global";
        case CC_WAKE_REASON_SPAWN_NONGLOBAL: return "spawn_non_global";
        case CC_WAKE_REASON_JOIN_THREAD: return "join_thread";
        case CC_WAKE_REASON_UNPARK_GLOBAL_EDGE: return "unpark_global";
        case CC_WAKE_REASON_UNPARK_NONGLOBAL: return "unpark_non_global";
        default: return "unknown";
    }
}

static inline uint64_t cc__mono_ns_sched(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}



static inline uint64_t cc__mono_ns_sched(void);







static inline int cc_sharded_runq_enabled(void) {
    int mode = atomic_load_explicit(&g_sharded_runq_mode, memory_order_acquire);
    if (mode >= 0) return mode;
    const char* v = getenv("CC_SHARDED_RUNQ");
    mode = (v && v[0] == '1') ? 1 : 0;
    int expected = -1;
    (void)atomic_compare_exchange_strong_explicit(&g_sharded_runq_mode,
                                                &expected,
                                                mode,
                                                memory_order_release,
                                                memory_order_acquire);
    return atomic_load_explicit(&g_sharded_runq_mode, memory_order_acquire);
}

static inline size_t sched_global_queue_count(void) {
    return g_sched.run_queue_count ? g_sched.run_queue_count : 1;
}

static inline size_t sched_primary_shard_for_worker(int worker_id) {
    size_t n = sched_global_queue_count();
    if (n <= 1 || worker_id < 0) return 0;
    size_t w = (size_t)worker_id;
    return (w * 2) % n;
}

static inline size_t sched_secondary_shard_for_worker(int worker_id) {
    size_t n = sched_global_queue_count();
    if (n <= 1 || worker_id < 0) return 0;
    return (sched_primary_shard_for_worker(worker_id) + 1) % n;
}

static inline int fq_likely_empty(fiber_queue* q) {
    if (atomic_load_explicit(&q->nonempty_hint, memory_order_acquire)) {
        return 0;
    }
    size_t head = atomic_load_explicit(&q->head, memory_order_relaxed);
    size_t tail = atomic_load_explicit(&q->tail, memory_order_acquire);
    if (head < tail) return 0;
    return atomic_load_explicit(&q->overflow_count, memory_order_relaxed) == 0;
}

static inline int sched_global_push(fiber_task* f, int preferred_worker) {
    if (!g_sched.run_queue) return 0;
    size_t n = sched_global_queue_count();
    size_t idx = 0;
    if (n > 1 && preferred_worker >= 0) {
        idx = sched_primary_shard_for_worker(preferred_worker);
    } else if (n > 1) {
        idx = (size_t)(atomic_fetch_add_explicit(&g_global_pop_rr, 1, memory_order_relaxed) % n);
    }
    fiber_queue* q = &g_sched.run_queue[idx];
    int was_empty = fq_likely_empty(q);
    fq_push_blocking(q, f);
    return was_empty;
}

static inline void spawn_mark_global_pop(fiber_task* f) {
    if (!f) return;
    if (f->spawn_publish_valid &&
        f->spawn_publish_route == (unsigned char)SPAWN_ROUTE_GLOBAL &&
        f->spawn_publish_active0) {
    (void)f;

}

}

static inline runnable_ref sched_global_pop_any(void) {
    if (!g_sched.run_queue) return runnable_ref_null();
    size_t n = sched_global_queue_count();
    size_t start = (size_t)atomic_fetch_add_explicit(&g_global_pop_rr, 1, memory_order_relaxed);
    for (size_t i = 0; i < n; i++) {
        size_t idx = (start + i) % n;
        if (!atomic_load_explicit(&g_sched.run_queue[idx].nonempty_hint, memory_order_acquire)) {
            continue;
        }
        runnable_ref ref = fq_pop(&g_sched.run_queue[idx]);
        if (ref.fiber) {
            spawn_mark_global_pop(ref.fiber);
            return ref;
        }
    }
    return runnable_ref_null();
}

static inline runnable_ref sched_global_pop_for_worker(int worker_id) {
    if (!g_sched.run_queue) return runnable_ref_null();
    size_t n = sched_global_queue_count();
    if (n <= 1 || worker_id < 0) {
        if (!atomic_load_explicit(&g_sched.run_queue[0].nonempty_hint, memory_order_acquire)) {
            return runnable_ref_null();
        }
        runnable_ref ref0 = fq_pop(&g_sched.run_queue[0]);
        if (ref0.fiber) {
            if (ref0.fiber->enqueue_src == 8) {
                cc_sched_io_wake_stat_inc(&g_cc_sched_io_wake_stats.global_pop_hits);
            }
            spawn_mark_global_pop(ref0.fiber);
        }
        return ref0;
    }

    size_t p = sched_primary_shard_for_worker(worker_id);
    runnable_ref ref = runnable_ref_null();
    if (atomic_load_explicit(&g_sched.run_queue[p].nonempty_hint, memory_order_acquire)) {
        ref = fq_pop(&g_sched.run_queue[p]);
    }
    if (ref.fiber) {
        if (ref.fiber->enqueue_src == 8) {
            cc_sched_io_wake_stat_inc(&g_cc_sched_io_wake_stats.global_pop_hits);
        }
        spawn_mark_global_pop(ref.fiber);
        return ref;
    }
    size_t s = sched_secondary_shard_for_worker(worker_id);
    if (s != p) {
        if (atomic_load_explicit(&g_sched.run_queue[s].nonempty_hint, memory_order_acquire)) {
            ref = fq_pop(&g_sched.run_queue[s]);
        }
        if (ref.fiber) {
            if (ref.fiber->enqueue_src == 8) {
                cc_sched_io_wake_stat_inc(&g_cc_sched_io_wake_stats.global_pop_hits);
            }
            spawn_mark_global_pop(ref.fiber);
            return ref;
        }
    }
    size_t start = (size_t)atomic_fetch_add_explicit(&g_global_pop_rr, 1, memory_order_relaxed);
    for (size_t i = 0; i < n; i++) {
        size_t idx = (start + i) % n;
        if (idx == p || idx == s) continue;
        if (!atomic_load_explicit(&g_sched.run_queue[idx].nonempty_hint, memory_order_acquire)) {
            continue;
        }
        ref = fq_pop(&g_sched.run_queue[idx]);
        if (ref.fiber) {
            if (ref.fiber->enqueue_src == 8) {
                cc_sched_io_wake_stat_inc(&g_cc_sched_io_wake_stats.global_pop_hits);
            }
            spawn_mark_global_pop(ref.fiber);
            return ref;
        }
    }
    return runnable_ref_null();
}

static inline int sched_global_peek_any(void) {
    if (!g_sched.run_queue) return 0;
    size_t n = sched_global_queue_count();
    for (size_t i = 0; i < n; i++) {
        if (fq_peek(&g_sched.run_queue[i])) return 1;
    }
    return 0;
}

/* Cheap global-work hint probe: checks shard nonempty hints only. */
static inline int sched_global_has_hint_any(void) {
    if (!g_sched.run_queue) return 0;
    size_t n = sched_global_queue_count();
    for (size_t i = 0; i < n; i++) {
        if (atomic_load_explicit(&g_sched.run_queue[i].nonempty_hint, memory_order_acquire)) {
            return 1;
        }
    }
    return 0;
}

static inline void cc_v3_worker_lifecycle_set(int worker_id, cc_worker_lifecycle next, const char* edge) {
    if (worker_id < 0 || !g_sched.worker_lifecycle) return;
    _Atomic unsigned char* slot = &g_sched.worker_lifecycle[(size_t)worker_id];

    const int lifecycle_asserts = cc_v3_worker_lifecycle_enabled();

    cc_worker_lifecycle prev = (cc_worker_lifecycle)atomic_exchange_explicit(
        slot, (unsigned char)next, memory_order_acq_rel);
    if (!lifecycle_asserts) return;
    if (cc_v3_worker_lifecycle_is_legal(prev, next)) return;
    atomic_fetch_add_explicit(&g_sched.lifecycle_illegal_transitions, 1, memory_order_relaxed);
    fprintf(stderr,
            "CC_V3_LIFECYCLE_ASSERT: worker=%d edge=%s illegal %s->%s\n",
            worker_id,
            edge ? edge : "(unknown)",
            cc_v3_worker_lifecycle_name(prev),
            cc_v3_worker_lifecycle_name(next));
    abort();
}

static inline int64_t cc_sched_pressure_add(int64_t delta) {
    /* Hot-path update (spawn/complete): keep it to a single atomic op.
    * int64 range is effectively unbounded for runtime pressure magnitudes here. */
    return atomic_fetch_add_explicit(&g_sched.pressure, delta, memory_order_relaxed) + delta;
}



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

/* Parked fibers list for debugging (runtime-gated). */
static pthread_mutex_t g_parked_list_mu = PTHREAD_MUTEX_INITIALIZER;
static fiber_task* g_parked_list_head = NULL;


/* All-fibers list for deadlock debugging (append-only, debug-gated). */
static pthread_mutex_t g_all_fibers_mu = PTHREAD_MUTEX_INITIALIZER;
static fiber_task* g_all_fibers_head = NULL;
static _Atomic size_t g_fibers_spawned = 0;  /* Total fibers ever created */

static void all_fibers_track(fiber_task* f) {
    if (!f) return;
    pthread_mutex_lock(&g_all_fibers_mu);
    f->debug_next = g_all_fibers_head;
    g_all_fibers_head = f;
    pthread_mutex_unlock(&g_all_fibers_mu);
}

static inline size_t get_total_parked(void) {
    return atomic_load_explicit(&g_sched.total_parked, memory_order_relaxed);
}

static inline size_t get_total_deadlock_suppressed_parked(void) {
    return atomic_load_explicit(&g_sched.deadlock_suppressed_parked, memory_order_relaxed);
}

static inline size_t get_total_timed_parked(void) {
    return atomic_load_explicit(&g_total_timed_parked, memory_order_relaxed);
}

static inline size_t get_total_deadlock_suppressed_timed_parked(void) {
    return atomic_load_explicit(&g_deadlock_suppressed_timed_parked, memory_order_relaxed);
}

static void cc__fiber_dump_parked_channel_state(const fiber_task* f) {
    if (!f || !f->park_reason || !f->park_obj) return;
    if (strncmp(f->park_reason, "chan_", 5) != 0) return;

    int control = (int)atomic_load_explicit(&f->control, memory_order_acquire);
    int pending_unpark = atomic_load_explicit(&f->pending_unpark, memory_order_relaxed);

    fprintf(stderr,
            "    fiber state: control=%d pending_unpark=%d\n",
            control,
            pending_unpark);
    cc__chan_debug_dump_state(f->park_obj, "    chan state: ");
}

static void cc__fiber_dump_parked_fibers(void) {
    size_t shown = 0;
    size_t parked_total = 0;
    size_t internal_total = 0;
    pthread_mutex_lock(&g_all_fibers_mu);
    for (fiber_task* f = g_all_fibers_head; f; f = f->debug_next) {
        int64_t control = atomic_load_explicit(&f->control, memory_order_acquire);
        if (control != CTRL_PARKED) continue;
        parked_total++;
        if (f->external_wait_parked || cc__fiber_external_wait_scoped(f) || cc__fiber_deadlock_suppressed(f)) continue;
        internal_total++;
        if (shown < 8) {
            fprintf(stderr,
                    "  parked fiber id=%llu reason=%s where=%s:%d obj=%p external=%u suppressed=%u pending_unpark=%d control=%lld\n",
                    (unsigned long long)f->fiber_id,
                    f->park_reason ? f->park_reason : "unknown",
                    f->park_file ? f->park_file : "?",
                    f->park_line,
                    f->park_obj,
                    (unsigned)f->external_wait_parked,
                    (unsigned)cc__fiber_deadlock_suppressed(f),
                    atomic_load_explicit(&f->pending_unpark, memory_order_relaxed),
                    (long long)control);
            cc__fiber_dump_parked_channel_state(f);
            shown++;
        }
    }
    pthread_mutex_unlock(&g_all_fibers_mu);
    if (parked_total == 0) {
        fprintf(stderr, "  parked fibers: none visible in debug list\n");
        return;
    }
    if (internal_total == 0) {
        fprintf(stderr, "  parked fibers: no internal parked fibers visible\n");
        return;
    }
    if (internal_total > shown) {
        fprintf(stderr, "  ... %zu more internal parked fibers not shown\n", internal_total - shown);
    }
}

static int cc__fiber_is_open_channel_recv_wait(const fiber_task* f) {
    return f &&
           f->park_reason &&
           (strcmp(f->park_reason, "chan_recv_wait_empty") == 0 ||
            strcmp(f->park_reason, "chan_recv_wait_rendezvous") == 0) &&
           f->park_obj &&
           cc__chan_debug_is_open(f->park_obj);
}

static int cc__fiber_only_open_channel_recv_waits(void) {
    int saw_internal = 0;
    pthread_mutex_lock(&g_all_fibers_mu);
    for (fiber_task* f = g_all_fibers_head; f; f = f->debug_next) {
        int64_t control = atomic_load_explicit(&f->control, memory_order_acquire);
        if (control != CTRL_PARKED) continue;
        if (f->external_wait_parked || cc__fiber_external_wait_scoped(f) || cc__fiber_deadlock_suppressed(f)) continue;
        saw_internal = 1;
        if (!cc__fiber_is_open_channel_recv_wait(f)) {
            pthread_mutex_unlock(&g_all_fibers_mu);
            return 0;
        }
    }
    pthread_mutex_unlock(&g_all_fibers_mu);
    return saw_internal;
}

static size_t cc__fiber_count_external_wait_scope_parked(void) {
    size_t count = 0;
    pthread_mutex_lock(&g_all_fibers_mu);
    for (fiber_task* f = g_all_fibers_head; f; f = f->debug_next) {
        int64_t control = atomic_load_explicit(&f->control, memory_order_acquire);
        if (control != CTRL_PARKED) continue;
        if (f->external_wait_parked || cc__fiber_external_wait_scoped(f)) count++;
    }
    pthread_mutex_unlock(&g_all_fibers_mu);
    return count;
}


/* Get monotonic time in milliseconds */
static uint64_t cc__monotonic_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

/* Deadlock detection: called when a worker is about to sleep or block.
* 
* We detect deadlock when ALL workers are unavailable (sleeping OR blocked
* in cc_block_on) AND there are parked fibers. To avoid false positives on
* transient states (e.g., cc_block_all startup), we require the state to
* persist for at least 1 second.
*/
static void cc__fiber_check_deadlock(void) {
    size_t sleeping = atomic_load_explicit(&g_sched.sleeping, memory_order_acquire);
    size_t blocked = atomic_load_explicit(&g_sched.blocked_threads, memory_order_acquire);
    size_t parked = get_total_parked();
    size_t suppressed_parked = get_total_deadlock_suppressed_parked();
    size_t timed_parked = get_total_timed_parked();
    size_t suppressed_timed_parked = get_total_deadlock_suppressed_timed_parked();
    size_t unsuppressed_parked = (parked > suppressed_parked) ? (parked - suppressed_parked) : 0;
    size_t unsuppressed_timed_parked =
        (timed_parked > suppressed_timed_parked) ? (timed_parked - suppressed_timed_parked) : 0;
    size_t deadlock_parked =
        (unsuppressed_parked > unsuppressed_timed_parked) ? (unsuppressed_parked - unsuppressed_timed_parked) : 0;
    size_t external_wait_parked = cc__fiber_count_external_wait_scope_parked();
    size_t external_wait_threads = atomic_load_explicit(&g_external_wait_threads, memory_order_relaxed);
    size_t external_waits = external_wait_parked + external_wait_threads;
    size_t internal_deadlock_parked =
        (deadlock_parked > external_wait_parked) ? (deadlock_parked - external_wait_parked) : 0;
    size_t temp_workers = atomic_load_explicit(&g_sched.temp_worker_count, memory_order_acquire);
    size_t total_workers = g_sched.num_workers + temp_workers;

    if (external_waits > 0 &&
        internal_deadlock_parked > 0 &&
        cc__fiber_only_open_channel_recv_waits()) {
        atomic_store(&g_deadlock_first_seen, 0);
        return;
    }
    
    /* Potential deadlock: all workers unavailable + parked fibers waiting */
    size_t unavailable = sleeping + blocked;
    if (unavailable >= total_workers && internal_deadlock_parked > 0) {
        uint64_t now = cc__monotonic_ms();
        uint64_t first = atomic_load(&g_deadlock_first_seen);
        
        if (first == 0) {
            /* First time seeing this state - record timestamp */
            atomic_compare_exchange_strong(&g_deadlock_first_seen, &first, now);
            return;
        }
        
        /* Require state to persist for 1+ seconds before declaring deadlock */
        if (now - first < 1000) return;
        
        int expected = 0;
        if (atomic_compare_exchange_strong(&g_deadlock_reported, &expected, 1)) {
            fprintf(stderr, "\n");
            fprintf(stderr, "╔══════════════════════════════════════════════════════════════╗\n");
            fprintf(stderr, "║                     DEADLOCK DETECTED                        ║\n");
            fprintf(stderr, "╚══════════════════════════════════════════════════════════════╝\n\n");
            
            fprintf(stderr, "Runtime state:\n");
            fprintf(stderr, "  Workers: %zu total (%zu base, %zu temp), %zu unavailable (sleeping or blocked)\n",
                    total_workers, g_sched.num_workers, temp_workers, unavailable);
            fprintf(stderr, "  Fibers:  %zu parked (waiting), %zu completed, %zu spawned total, %zu pending\n",
                    parked, (size_t)atomic_load(&g_sched.completed),
                    (size_t)atomic_load_explicit(&g_fibers_spawned, memory_order_relaxed),
                    (size_t)atomic_load_explicit(&g_sched.pending, memory_order_relaxed));
            if (suppressed_parked > 0) {
                fprintf(stderr, "  Ignored: %zu parked waits are inside deadlock-suppressed scopes\n",
                        suppressed_parked);
            }
            if (timed_parked > 0) {
                fprintf(stderr, "  Timers:  %zu parked waits still have deadline wakeups\n", timed_parked);
            }
            if (external_wait_parked > 0) {
                fprintf(stderr, "  I/O waits: %zu parked waits are awaiting external progress\n",
                        external_wait_parked);
            }
            fprintf(stderr, "  Internal parked fibers:\n");
            cc__fiber_dump_parked_fibers();
            fprintf(stderr, "\n");
            cc__fiber_dump_queue_state();
            fprintf(stderr, "\n");
            
            
            fprintf(stderr, "\n");
            fprintf(stderr, "Common causes:\n");
            fprintf(stderr, "  • Channel send() with no receiver, or recv() with no sender\n");
            fprintf(stderr, "  • cc_fiber_join() on a fiber that's also waiting\n");
            fprintf(stderr, "  • Circular dependency between fibers\n");
            fprintf(stderr, "\n");
            fprintf(stderr, "Debugging tips:\n");
            fprintf(stderr, "  • Check channel operations have matching send/recv pairs\n");
            fprintf(stderr, "  • Ensure channels are closed when done (triggers recv to return)\n");
            fprintf(stderr, "  • Review fiber spawn/join patterns for circular waits\n");
            fprintf(stderr, "\n");
            
            const char* abort_env = getenv("CC_DEADLOCK_ABORT");
            if (!abort_env || abort_env[0] != '0') {
                fprintf(stderr, "Aborting with exit code 124. Set CC_DEADLOCK_ABORT=0 to continue.\n");
                _exit(124);
            } else {
                fprintf(stderr, "Continuing (CC_DEADLOCK_ABORT=0 set).\n");
            }
        }
    } else {
        /* State is healthy - reset timer */
        atomic_store(&g_deadlock_first_seen, 0);
    }
}

/* Per-worker thread-local state */
static __thread fiber_task* tls_current_fiber = NULL;
static __thread unsigned tls_deadlock_suppress_depth = 0;
static __thread unsigned tls_external_wait_depth = 0;
static __thread int tls_worker_id = -1;  /* -1 = not a worker thread */
static _Atomic size_t g_spawn_counter = 0;
static const uint64_t cc_orphan_threshold_cycles_hint = 3000000ULL;

static inline int cc__io_wait_trace_enabled_sched(void) {
    static int mode = -1;
    if (mode >= 0) return mode;
    const char* env = getenv("CC_IO_WAIT_TRACE");
    mode = (env && env[0] && !(env[0] == '0' && env[1] == '\0')) ? 1 : 0;
    return mode;
}

static inline int cc__io_wait_trace_fiber(const fiber_task* f) {
    return cc__io_wait_trace_enabled_sched() &&
           f &&
           f->park_reason &&
           strcmp(f->park_reason, "io_ready") == 0;
}

static inline int cc__req_wake_trace_fiber(const fiber_task* f) {
    return f &&
           f->park_reason &&
           strcmp(f->park_reason, "chan_recv_wait_empty") == 0 &&
           cc__chan_debug_req_wake_match(f->park_obj);
}

static inline int cc__io_ready_fiber(const fiber_task* f) {
    return f &&
           f->park_reason &&
           strcmp(f->park_reason, "io_ready") == 0;
}

/* Spawn-pair grouping: when a base worker routes a fiber to a remote inbox,
* remember the target so the NEXT inbox-bound spawn from the same calling
* context can co-locate with it (if and only if the inbox still has exactly
* that one pending fiber).  Grouping two consecutive spawns to the same
* inbox ensures a producer/consumer pair starts on the same worker without
* requiring the developer to order spawns or add scheduler hints.
* Reset to -1 after each pairing so triples never form. */
static __thread int tls_spawn_pair_target = -1;
/* Non-worker nursery startup hint: consecutive sibling spawns in the same
* group stay together for a small chunk before the next chunk advances RR.
* This preserves producer/consumer locality for tiny startup bursts while
* still letting larger batches spread across workers. */
static __thread const void* tls_nonworker_spawn_group_hint = NULL;
static __thread unsigned tls_nonworker_spawn_group_hint_chunk = 0;

/* Pre-assign the calling fiber to a specific worker before its first park.
* Pool runners call this once so each parks with a distinct last_worker_id.
* Combined with the corrected saturation bypass (only fires when sleeping==0),
* this ensures each runner routes to a dedicated worker's inbox on first
* unpark — no serialisation, no thundering herd. */
void cc__fiber_set_worker_affinity(int worker_id) {
    fiber_task* f = tls_current_fiber;
    if (!f) return;
    size_t n = g_sched.num_workers;
    if (n == 0) return;
    f->last_worker_id = worker_id % (int)n;
    f->last_worker_src = 2;
}

/* Return the current scheduler base-worker ID, or -1 if the calling thread
* is not a base worker (e.g. a temp/timer worker or a non-scheduler thread).
* Used by channel wakeup code to implement direct-handoff routing. */
int cc__sched_current_worker_id(void) {
    return tls_worker_id;
}

uint64_t cc__fiber_debug_id(void* fiber_ptr) {
    fiber_task* f = (fiber_task*)fiber_ptr;
    return f ? (uint64_t)f->fiber_id : 0;
}

int cc__fiber_debug_last_worker(void* fiber_ptr) {
    fiber_task* f = (fiber_task*)fiber_ptr;
    return f ? f->last_worker_id : -1;
}

const char* cc__fiber_debug_park_reason(void* fiber_ptr) {
    fiber_task* f = (fiber_task*)fiber_ptr;
    return (f && f->park_reason) ? f->park_reason : NULL;
}

void cc_sched_nonworker_spawn_group_hint(const void* group_key, unsigned chunk_size) {
    tls_nonworker_spawn_group_hint = group_key;
    tls_nonworker_spawn_group_hint_chunk = chunk_size ? chunk_size : 1;
}


static inline size_t cc__spawn_pick_round_robin_target(void) {
    size_t target = atomic_fetch_add_explicit(&g_spawn_counter, 1, memory_order_relaxed) % g_sched.num_workers;
    if (cc_nonworker_keep_sleeping_target_enabled() &&
        g_sched.worker_lifecycle && g_sched.num_workers > 1) {
        cc_worker_lifecycle lc = (cc_worker_lifecycle)atomic_load_explicit(
            &g_sched.worker_lifecycle[target], memory_order_relaxed);
        /* A sleeping worker is still a good target for a non-worker spawn:
        * we can publish directly to its inbox and wake it specifically.
        * Only reroute away from workers that are genuinely unavailable. */
        if (lc != CC_WL_DRAINING && lc != CC_WL_DEAD) {
            return target;
        }
    }
    if (g_sched.worker_heartbeat && g_sched.num_workers > 1) {
        uint64_t hb0 = atomic_load_explicit(&g_sched.worker_heartbeat[target].heartbeat, memory_order_relaxed);
        uint64_t now_cyc = rdtsc();
        if (hb0 != 0 && (now_cyc - hb0) >= cc_orphan_threshold_cycles_hint) {
            for (size_t scan = 1; scan < g_sched.num_workers; scan++) {
                size_t cand = (target + scan) % g_sched.num_workers;
                uint64_t hb = atomic_load_explicit(&g_sched.worker_heartbeat[cand].heartbeat, memory_order_relaxed);
                if (hb != 0 && (now_cyc - hb) < cc_orphan_threshold_cycles_hint) {
                    target = cand;
                    break;
                }
            }
        }
    }
    return target;
}

static inline size_t cc__spawn_pick_nonworker_target(void) {
    const void* group_key = tls_nonworker_spawn_group_hint;
    unsigned chunk_size = tls_nonworker_spawn_group_hint_chunk;
    int reused_target = 0;
    tls_nonworker_spawn_group_hint = NULL;
    tls_nonworker_spawn_group_hint_chunk = 0;
    if (!cc_nonworker_spawn_group_enabled()) {
        return cc__spawn_pick_round_robin_target();
    }
    (void)group_key; (void)chunk_size; (void)reused_target;
    return cc__spawn_pick_round_robin_target();
}

/* Set a parked fiber's routing hint to worker_id so that its next unpark
* lands on that worker's local queue rather than a remote inbox.  Must only
* be called while the fiber is parked (not running on any thread).
* worker_id is clamped to valid base-worker range; -1 is a no-op. */
void cc__fiber_hint_channel_partner(void* fiber, int worker_id) {
    fiber_task* f = (fiber_task*)fiber;
    if (!f) return;
    size_t n = g_sched.num_workers;
    if (n == 0 || worker_id < 0 || (size_t)worker_id >= n) return;
    f->last_worker_id = worker_id;
    f->last_worker_src = 3;
}

/* Count of workers currently executing a long-running CPU-bound pool task via
* cc__fiber_pool_task_begin/end.  Sysmon excludes these workers from the
* "stuck" count so it doesn't spawn hybrid-promotion threads for them. */
static _Atomic size_t g_pool_tasks_active = 0;
/* True for scheduler-owned execution threads (base workers + replacement workers). */
static __thread int tls_sched_worker_ctx = 0;
static __thread void* tls_tsan_sched_fiber = NULL;
static __thread uint32_t tls_chan_attr_calls_batch = 0;
static __thread uint32_t tls_chan_attr_startup_batch = 0;
static __thread uint32_t tls_chan_attr_sleepers_batch = 0;
static __thread uint32_t tls_chan_attr_local_handoff = 0;

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
    if (tls_deadlock_suppress_depth < UINT32_MAX) tls_deadlock_suppress_depth++;
    if (tls_current_fiber) {
        tls_current_fiber->deadlock_suppress_depth = tls_deadlock_suppress_depth;
    }
}

void cc_deadlock_suppress_leave(void) {
    if (tls_deadlock_suppress_depth == 0) return;
    tls_deadlock_suppress_depth--;
    if (tls_current_fiber) {
        tls_current_fiber->deadlock_suppress_depth = tls_deadlock_suppress_depth;
    }
}

int cc_deadlock_suppressed(void) {
    return tls_deadlock_suppress_depth > 0;
}

/* Provisional mechanism: we currently model "externally driven wait" as a
 * dynamic scope on the current execution context. Semantically this really
 * belongs at specific runtime wait sites, but the scope form gives the
 * scheduler a simple way to carry the classification across a park/resume. */
void cc_external_wait_enter(void) {
    if (tls_external_wait_depth < UINT32_MAX) tls_external_wait_depth++;
    if (tls_current_fiber) {
        tls_current_fiber->external_wait_depth = tls_external_wait_depth;
        tls_current_fiber->external_wait_scoped = 1;
    } else if (tls_external_wait_depth == 1) {
        atomic_fetch_add_explicit(&g_external_wait_threads, 1, memory_order_relaxed);
    }
}

void cc_external_wait_leave(void) {
    if (tls_external_wait_depth == 0) return;
    tls_external_wait_depth--;
    if (tls_current_fiber) {
        tls_current_fiber->external_wait_depth = tls_external_wait_depth;
        if (tls_external_wait_depth == 0) {
            tls_current_fiber->external_wait_scoped = 0;
        }
    } else if (tls_external_wait_depth == 0) {
        atomic_fetch_sub_explicit(&g_external_wait_threads, 1, memory_order_relaxed);
    }
}

int cc_external_wait_active(void) {
    return tls_external_wait_depth > 0;
}

/* Fast local queue push (single producer) */
static inline int lq_push(local_queue* q, fiber_task* f) {
    cc_v3_assert_enqueue_runnable(f, "local");
    runnable_ref ref = runnable_ref_snapshot(f);
    size_t tail = atomic_load_explicit(&q->tail, memory_order_relaxed);
    size_t head = atomic_load_explicit(&q->head, memory_order_acquire);
    if (tail - head >= LOCAL_QUEUE_SIZE) return -1;  /* Full */
    /* Use release on slot store to ensure closure contents are visible to consumer.
    * The consumer uses acquire on the exchange, creating a release-acquire pair. */
    size_t idx = tail % LOCAL_QUEUE_SIZE;
    runnable_slot_publish(&q->slots[idx], ref);
    atomic_store_explicit(&q->tail, tail + 1, memory_order_release);
    return 0;
}

/* Check if local queue has items (non-destructive peek) */
static inline int lq_peek(local_queue* q) {
    size_t head = atomic_load_explicit(&q->head, memory_order_relaxed);
    size_t tail = atomic_load_explicit(&q->tail, memory_order_acquire);
    return head < tail;
}

/* Return number of items in local queue (approximate, for routing decisions) */
static inline size_t lq_depth(local_queue* q) {
    size_t head = atomic_load_explicit(&q->head, memory_order_relaxed);
    size_t tail = atomic_load_explicit(&q->tail, memory_order_acquire);
    return (tail > head) ? (tail - head) : 0;
}

/* Fast local queue pop (owner only - but must handle concurrent stealers) 
* Uses atomic exchange to claim slot first, then try to advance head once.
* Limited retries to avoid infinite loop under pathological contention. */
static inline runnable_ref lq_pop(local_queue* q) {
    for (int retry = 0; retry < 64; retry++) {
        size_t head = atomic_load_explicit(&q->head, memory_order_relaxed);
        size_t tail = atomic_load_explicit(&q->tail, memory_order_acquire);
        if (head >= tail) return runnable_ref_null();  /* Empty */
        
        size_t idx = head % LOCAL_QUEUE_SIZE;
        
        /* Atomically exchange slot with NULL to claim it */
        runnable_ref ref = runnable_slot_take_exchange(&q->slots[idx]);
        fiber_task* f = ref.fiber;
        if (!f) {
            /* Lost race with stealer - they cleared slot, try to help advance head */
            atomic_compare_exchange_weak_explicit(&q->head, &head, head + 1,
                                                memory_order_relaxed, memory_order_relaxed);
            continue;
        }
        
        /* We got the task. Try to advance head once.
        * If CAS fails, someone else advanced it for us - that's fine. */
        atomic_compare_exchange_weak_explicit(&q->head, &head, head + 1,
                                            memory_order_relaxed, memory_order_relaxed);
        ref = runnable_ref_validate(ref);
        if (!ref.fiber) continue;
        return ref;
    }
    return runnable_ref_null();  /* Too much contention, let caller try global queue */
}

static inline void wake_one_if_sleeping(cc_wake_reason reason) {
    cc_sched_wait_stat_inc(&g_cc_sched_wait_stats.wake_one_calls);
    size_t spinning = atomic_load_explicit(&g_sched.spinning, memory_order_relaxed);
    int allow_with_spinners = (reason == CC_WAKE_REASON_SPAWN_GLOBAL_EDGE);
    if (spinning > 0 && !allow_with_spinners) return;
    size_t sleeping = atomic_load_explicit(&g_sched.sleeping, memory_order_relaxed);
    if (sleeping == 0) return;
    cc_sched_wait_stat_inc(&g_cc_sched_wait_stats.wake_one_with_sleepers);
    int woke = 0;
    if (g_sched.worker_wake_prims && g_sched.worker_lifecycle) {
        for (size_t scan = 0; scan < g_sched.num_workers && !woke; scan++) {
            cc_worker_lifecycle lc = (cc_worker_lifecycle)atomic_load_explicit(
                &g_sched.worker_lifecycle[scan], memory_order_relaxed);
            if (lc == CC_WL_SLEEP) {
                wake_primitive_wake_one(&g_sched.worker_wake_prims[scan]);
                woke = 1;
            }
        }
    }
    if (!woke) wake_primitive_wake_one(&g_sched.wake_prim);
    cc_sched_wait_stat_inc(&g_cc_sched_wait_stats.wake_one_delivered);
}

static inline void wake_one_if_sleeping_unconditional(void) {
    cc_sched_wait_stat_inc(&g_cc_sched_wait_stats.wake_unconditional_calls);
    size_t sleeping = atomic_load_explicit(&g_sched.sleeping, memory_order_relaxed);
    if (sleeping > 0) {
        int woke = 0;
        if (g_sched.worker_wake_prims && g_sched.worker_lifecycle) {
            for (size_t scan = 0; scan < g_sched.num_workers && !woke; scan++) {
                cc_worker_lifecycle lc = (cc_worker_lifecycle)atomic_load_explicit(
                    &g_sched.worker_lifecycle[scan], memory_order_relaxed);
                if (lc == CC_WL_SLEEP) {
                    wake_primitive_wake_one(&g_sched.worker_wake_prims[scan]);
                    woke = 1;
                }
            }
        }
        if (!woke) wake_primitive_wake_one(&g_sched.wake_prim);
        cc_sched_wait_stat_inc(&g_cc_sched_wait_stats.wake_unconditional_delivered);
    } else {
        /* No sleepers: bump all per-worker counters AND the global counter so
        * any worker racing into sleep sees a changed value and re-checks. */
        cc_sched_wait_stat_inc(&g_cc_sched_wait_stats.wake_unconditional_no_sleepers);
        if (g_sched.worker_wake_prims) {
            for (size_t i = 0; i < g_sched.num_workers; i++) {
                atomic_fetch_add_explicit(&g_sched.worker_wake_prims[i].value, 1, memory_order_relaxed);
            }
        }
        atomic_fetch_add_explicit(&g_sched.wake_prim.value, 1, memory_order_relaxed);
    }
}

/* Global-edge wake policy for count convergence:
* require sleepers; allowing active workers avoids missed-wake starvation when
* runnable work appears during mixed active/sleeping transitions. */
static inline int pool_idle_for_global_edge_wake(void) {
    size_t sleeping = atomic_load_explicit(&g_sched.sleeping, memory_order_relaxed);
    return sleeping > 0;
}

/* Wake a specific worker if it is sleeping, using its per-worker primitive.
* Used after pushing to that worker's inbox: zero thundering herd, zero
* wasted spin cycles on all other workers.  Falls back to the global wake
* primitive when per-worker prims are not yet allocated (startup). */
static inline void wake_target_worker_if_sleeping(int target_worker) {
    if (target_worker < 0) return;
    cc_sched_wait_stat_inc(&g_cc_sched_wait_stats.wake_target_calls);
    if (!g_sched.worker_wake_prims || !g_sched.worker_lifecycle) {
        wake_primitive_wake_one(&g_sched.wake_prim);
        cc_sched_wait_stat_inc(&g_cc_sched_wait_stats.wake_target_delivered);
        return;
    }
    cc_worker_lifecycle lc = (cc_worker_lifecycle)atomic_load_explicit(
        &g_sched.worker_lifecycle[target_worker], memory_order_relaxed);
    if (lc == CC_WL_SLEEP) {
        wake_primitive_wake_one(&g_sched.worker_wake_prims[target_worker]);
        cc_sched_wait_stat_inc(&g_cc_sched_wait_stats.wake_target_delivered);
    } else {
        cc_sched_wait_stat_inc(&g_cc_sched_wait_stats.wake_target_skipped_not_sleeping);
    }
    /* If not sleeping (active or spinning), the worker will find the task
    * naturally in its inbox check — no wake needed. */
}

static inline int sched_in_startup_phase(void) {
    return atomic_load_explicit(&g_sched.startup_phase, memory_order_relaxed) == CC_STARTUP_PHASE;
}

static inline void sched_maybe_promote_run_phase(void) {
    if (!sched_in_startup_phase()) return;
    size_t runs = atomic_fetch_add_explicit(&g_sched.startup_run_count, 1, memory_order_relaxed) + 1;
    size_t target = atomic_load_explicit(&g_sched.startup_target_runs, memory_order_relaxed);
    if (target == 0) target = g_sched.num_workers ? g_sched.num_workers : 1;
    if (runs < target) return;
    int expected = CC_STARTUP_PHASE;
    if (atomic_compare_exchange_strong_explicit(&g_sched.startup_phase,
                                                &expected,
                                                CC_RUN_PHASE,
                                                memory_order_relaxed,
                                                memory_order_relaxed)) {
        atomic_store_explicit(&g_spawn_nw_startup_admit_wakev, UINT32_MAX, memory_order_relaxed);
        atomic_store_explicit(&g_spawn_nw_startup_admit_remaining, 0, memory_order_relaxed);
    }
}

static inline int pool_strict_idle_for_nonglobal_wake(void) {
    size_t sleeping = atomic_load_explicit(&g_sched.sleeping, memory_order_relaxed);
    size_t active = atomic_load_explicit(&g_sched.active, memory_order_relaxed);
    size_t spinning = atomic_load_explicit(&g_sched.spinning, memory_order_relaxed);
    return sleeping > 0 && active == 0 && spinning == 0;
}

static inline int pool_startup_spinning_no_sleep(void) {
    if (!sched_in_startup_phase()) return 0;
    size_t active = atomic_load_explicit(&g_sched.active, memory_order_relaxed);
    size_t spinning = atomic_load_explicit(&g_sched.spinning, memory_order_relaxed);
    size_t sleeping = atomic_load_explicit(&g_sched.sleeping, memory_order_relaxed);
    return active == 0 && spinning > 0 && sleeping == 0;
}

/* Startup guard: only allow non-worker inbox publication once at least one
* worker is actively executing. Before that, fall back to global queue. */
static inline int pool_ready_for_nonworker_inbox_publish(void) {
    size_t active = atomic_load_explicit(&g_sched.active, memory_order_relaxed);
    size_t spinning = atomic_load_explicit(&g_sched.spinning, memory_order_relaxed);
    size_t sleeping = atomic_load_explicit(&g_sched.sleeping, memory_order_relaxed);
    size_t workers = g_sched.num_workers;
    if (workers == 0) return 0;
    if (!sched_in_startup_phase()) return 1;
    /* Startup-aware gate:
    * - require scheduler presence (active, spinning, or sleeping worker)
    * - keep very small startup bursts on global for launch-order fairness
    * - once backlog reaches one worker-wave, allow inbox fan-out */
    if (active == 0 && spinning == 0 && sleeping == 0) return 0;
    return 1;
}

/* Safe to publish directly to worker inboxes once at least one worker has
* clearly entered the scheduler loop (active/spinning/sleeping). */
static void cc__fiber_dump_queue_state(void) {
    return;
    if (!g_sched.run_queue) return;
    fprintf(stderr, "\n[cc] Queue state dump:\n");
    fprintf(stderr, "  pending=%zu active=%zu sleeping=%zu spinning=%zu parked=%zu\n",
            (size_t)atomic_load(&g_sched.pending),
            (size_t)atomic_load(&g_sched.active),
            (size_t)atomic_load(&g_sched.sleeping),
            (size_t)atomic_load(&g_sched.spinning),
            get_total_parked());
    fprintf(stderr, "  workers: base=%zu temp=%zu\n",
            g_sched.num_workers,
            (size_t)atomic_load_explicit(&g_sched.temp_worker_count, memory_order_relaxed));
    size_t rq_head = 0, rq_tail = 0;
    for (size_t s = 0; s < sched_global_queue_count(); s++) {
        rq_head += atomic_load(&g_sched.run_queue[s].head);
        rq_tail += atomic_load(&g_sched.run_queue[s].tail);
    }
    fprintf(stderr, "  run_queue[%zu]: head=%zu tail=%zu\n",
            sched_global_queue_count(), rq_head, rq_tail);
    fprintf(stderr, "  inbox_overflow=%zu\n",
            (size_t)atomic_load(&g_inbox_overflow));
    for (size_t i = 0; i < g_sched.num_workers; i++) {
        local_queue* lq = &g_sched.local_queues[i];
        inbox_queue* iq = &g_sched.inbox_queues[i];
        size_t lq_head = atomic_load_explicit(&lq->head, memory_order_relaxed);
        size_t lq_tail = atomic_load_explicit(&lq->tail, memory_order_relaxed);
        size_t iq_head = atomic_load_explicit(&iq->head, memory_order_relaxed);
        size_t iq_tail = atomic_load_explicit(&iq->tail, memory_order_relaxed);
        if (lq_tail > lq_head || iq_tail > iq_head) {
            fprintf(stderr, "  worker[%zu]: local=%zu inbox=%zu\n",
                    i, lq_tail - lq_head, iq_tail - iq_head);
        }
    }
}

static inline int cc__restore_inbox_task(size_t victim, fiber_task* f);

/* Work stealing: steal from another worker's queue.
* Uses atomic exchange to claim slot first, then CAS to advance head. */
static inline int cc__task_should_stay_on_owner_local(fiber_task* f, int target_worker) {
    if (!f || target_worker < 0) return 0;
    if (f->enqueue_src == 8 && f->last_worker_id == target_worker) {
        return 1;
    }
    return 0;
}

static inline runnable_ref lq_steal(local_queue* q, int target_worker) {
    size_t head = atomic_load_explicit(&q->head, memory_order_acquire);
    size_t tail = atomic_load_explicit(&q->tail, memory_order_acquire);
    if (head >= tail) return runnable_ref_null();  /* Empty */
    
    size_t idx = head % LOCAL_QUEUE_SIZE;
    
    /* Atomically exchange slot with NULL to claim it */
    runnable_ref ref = runnable_slot_take_exchange(&q->slots[idx]);
    fiber_task* f = ref.fiber;
    if (!f) return runnable_ref_null();  /* Lost race */
    
    /* We got the task. Now try to advance head. If we fail, someone else advanced it. */
    if (!atomic_compare_exchange_weak_explicit(&q->head, &head, head + 1,
                                                memory_order_relaxed, memory_order_relaxed)) {
        /* Lost CAS but we still have the task - another thread advanced head.
        * This can happen if owner also exchanged slot (both got NULL except us).
        * But since we got f != NULL, we're the winner. Just return it. */
    }
    ref = runnable_ref_validate(ref);
    if (!ref.fiber) {
        return runnable_ref_null();
    }
    if (cc__task_should_stay_on_owner_local(f, target_worker) &&
        cc__restore_inbox_task((size_t)target_worker, f)) {
        return runnable_ref_null();
    }
    return ref;
}

/* Batch work stealing: steal up to half the victim's queue.
* This amortizes the cost of coordinating the steal across multiple tasks.
* Returns number of tasks stolen (stored in out_tasks array). */
static inline size_t lq_steal_batch(local_queue* q, int target_worker, runnable_ref* out_tasks, size_t max_steal) {
    size_t stolen = 0;
    
    /* Read queue bounds - we'll try to steal up to half */
    size_t head = atomic_load_explicit(&q->head, memory_order_acquire);
    size_t tail = atomic_load_explicit(&q->tail, memory_order_acquire);
    
    if (head >= tail) return 0;  /* Empty */
    
    size_t available = tail - head;
    size_t to_steal = available / 2;  /* Steal half */
    if (to_steal == 0) to_steal = 1;  /* At least try to steal one */
    if (to_steal > max_steal) to_steal = max_steal;
    
    /* Steal tasks one by one using atomic exchange.
    * This is safe because we only advance head after successfully claiming a slot. */
    for (size_t i = 0; i < to_steal; i++) {
        head = atomic_load_explicit(&q->head, memory_order_acquire);
        tail = atomic_load_explicit(&q->tail, memory_order_acquire);
        
        if (head >= tail) break;  /* Queue became empty */
        
        size_t idx = head % LOCAL_QUEUE_SIZE;
        
        /* Atomically claim slot */
        runnable_ref ref = runnable_slot_take_exchange(&q->slots[idx]);
        fiber_task* f = ref.fiber;
        if (!f) {
            /* Lost race - try to help advance head and continue */
            atomic_compare_exchange_weak_explicit(&q->head, &head, head + 1,
                                                memory_order_relaxed, memory_order_relaxed);
            continue;
        }
        
        /* Got a task - try to advance head */
        atomic_compare_exchange_weak_explicit(&q->head, &head, head + 1,
                                            memory_order_relaxed, memory_order_relaxed);
        ref = runnable_ref_validate(ref);
        if (!ref.fiber) {
            continue;
        }
        if (cc__task_should_stay_on_owner_local(f, target_worker) &&
            cc__restore_inbox_task((size_t)target_worker, f)) {
            continue;
        }
        out_tasks[stolen++] = ref;
    }
    
    return stolen;
}

/* Dump scheduler state for debugging hangs */
void cc_fiber_dump_state(const char* reason) {
    fprintf(stderr, "\n=== FIBER SCHEDULER STATE: %s ===\n", reason ? reason : "");
    fprintf(stderr, "  pending=%zu active=%zu sleeping=%zu parked=%zu completed=%zu\n",
            atomic_load(&g_sched.pending),
            atomic_load(&g_sched.active),
            atomic_load(&g_sched.sleeping),
            get_total_parked(),
            atomic_load(&g_sched.completed));
    if (g_sched.run_queue) {
        size_t head = 0, tail = 0, overflow = 0;
        for (size_t s = 0; s < sched_global_queue_count(); s++) {
            head += atomic_load(&g_sched.run_queue[s].head);
            tail += atomic_load(&g_sched.run_queue[s].tail);
            overflow += atomic_load(&g_sched.run_queue[s].overflow_count);
        }
        fprintf(stderr, "  run_queue[%zu]: head=%zu tail=%zu (ring ~%zu + overflow %zu items)\n",
                sched_global_queue_count(), head, tail, tail - head, overflow);
    } else {
        fprintf(stderr, "  run_queue: (uninitialized)\n");
    }
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

static fiber_task* fiber_alloc(void) {
    /* Try to get from free list */
    fiber_task* f = atomic_load_explicit(&g_sched.free_list, memory_order_acquire);
    while (f) {
        fiber_task* next = f->next;
        if (atomic_compare_exchange_weak_explicit(&g_sched.free_list, &f, next,
                                                memory_order_release,
                                                memory_order_acquire)) {
            atomic_fetch_add_explicit(&f->generation, 1, memory_order_relaxed);
            /* Reuse pooled fiber - reset state but KEEP the coro and fiber_id.
            * CRITICAL: We must reset join_cv_initialized to 0. The join_cv itself
            * can be reused (it's just a pthread_cond_t), but the initialized flag
            * must be reset so that a new joiner doesn't try to wait on a stale
            * condvar that won't receive a broadcast from this new fiber instance. */
            f->fn = NULL;
            f->arg = NULL;
            f->result = NULL;
            atomic_store(&f->control, CTRL_IDLE);
            atomic_store(&f->done, 0);
            f->yield_dest = YIELD_NONE;
            f->park_flag = NULL;
            f->park_expected = 0;
            atomic_store(&f->wake_counter, 0);
            atomic_store(&f->wait_ticket, 0);
            atomic_store(&f->pending_unpark, 0);
            atomic_store(&f->timed_park_registered, 0);
            atomic_store(&f->timed_park_fired, 0);
            atomic_store(&f->join_waiters, 0);
            atomic_store(&f->join_waiter_fiber, NULL);
            atomic_store(&f->join_lock, 0);
            atomic_store(&f->join_cv_initialized, 0);  /* Reset for new fiber instance */
            f->tsan_fiber = TSAN_FIBER_CREATE();
            f->park_reason = NULL;
            f->park_file = NULL;
            f->park_line = 0;
            f->park_obj = NULL;
            f->enqueue_src = 0;
            f->last_worker_id = -1;
            f->last_worker_src = 0;
            f->spawn_publish_tsc = 0;
            f->spawn_global_pop_tsc = 0;
            f->spawn_publish_route = (unsigned char)SPAWN_ROUTE_NONE;
            f->spawn_publish_target_worker = -1;
            f->spawn_publish_active0 = 0;
            f->spawn_publish_forced_spill = 0;
            f->spawn_global_pop_valid = 0;
            f->spawn_publish_valid = 0;
            f->timed_park_requested = 0;
            f->timer_next = NULL;
            f->saved_nursery = NULL;
            f->deadlock_suppress_depth = 0;
            f->external_wait_depth = 0;
            f->external_wait_scoped = 0;
            f->external_wait_parked = 0;
            f->admission_nursery = NULL;
            f->next = NULL;
            /* f->coro and f->fiber_id are kept for reuse! */
            return f;
        }
    }
    
    /* Allocate new fiber */
    fiber_task* nf = (fiber_task*)calloc(1, sizeof(fiber_task));
    if (nf) {
        atomic_store_explicit(&nf->generation, 1, memory_order_relaxed);
        atomic_store_explicit(&nf->join_cv_initialized, 0, memory_order_relaxed);
        atomic_store_explicit(&nf->wake_counter, 0, memory_order_relaxed);
        atomic_store_explicit(&nf->wait_ticket, 0, memory_order_relaxed);
        atomic_store_explicit(&nf->pending_unpark, 0, memory_order_relaxed);
        atomic_store(&nf->join_waiters, 0);
        atomic_store(&nf->join_waiter_fiber, NULL);
        atomic_store(&nf->join_lock, 0);
        nf->tsan_fiber = TSAN_FIBER_CREATE();
        nf->fiber_id = atomic_fetch_add(&g_next_fiber_id, 1);
        nf->last_worker_id = -1;
        nf->last_worker_src = 0;
        nf->spawn_publish_tsc = 0;
        nf->spawn_global_pop_tsc = 0;
        nf->spawn_publish_route = (unsigned char)SPAWN_ROUTE_NONE;
        nf->spawn_publish_target_worker = -1;
        nf->spawn_publish_active0 = 0;
        nf->spawn_publish_forced_spill = 0;
        nf->spawn_global_pop_valid = 0;
        nf->spawn_publish_valid = 0;
        nf->saved_nursery = NULL;
        nf->deadlock_suppress_depth = 0;
        nf->external_wait_depth = 0;
        nf->external_wait_scoped = 0;
        nf->external_wait_parked = 0;
        nf->admission_nursery = NULL;
    }
    return nf;
}

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
static void fiber_free(fiber_task* f) {
    if (!f) return;
    /* Keep the coro and join_cv for pooling - don't destroy them! */
    fiber_task* head;
    do {
        head = atomic_load_explicit(&g_sched.free_list, memory_order_relaxed);
        f->next = head;
    } while (!atomic_compare_exchange_weak_explicit(&g_sched.free_list, &head, f,
                                                    memory_order_release,
                                                    memory_order_relaxed));
}

/* Fully destroy a fiber (called during shutdown) */
static void fiber_destroy(fiber_task* f) {
    if (!f) return;
    if (f->coro) mco_destroy(f->coro);
    if (atomic_load_explicit(&f->join_cv_initialized, memory_order_acquire)) {
        pthread_mutex_destroy(&f->join_mu);
        pthread_cond_destroy(&f->join_cv);
    }
    free(f);
}

/* ============================================================================
* Error Handling
* ============================================================================ */

static const char* mco_result_str(mco_result res) {
    switch (res) {
        case MCO_SUCCESS: return "success";
        case MCO_GENERIC_ERROR: return "generic error";
        case MCO_INVALID_POINTER: return "invalid pointer";
        case MCO_INVALID_COROUTINE: return "invalid coroutine";
        case MCO_NOT_SUSPENDED: return "not suspended";
        case MCO_NOT_RUNNING: return "not running";
        case MCO_MAKE_CONTEXT_ERROR: return "make context error";
        case MCO_SWITCH_CONTEXT_ERROR: return "switch context error";
        case MCO_NOT_ENOUGH_SPACE: return "not enough space";
        case MCO_OUT_OF_MEMORY: return "out of memory";
        case MCO_INVALID_ARGUMENTS: return "invalid arguments";
        case MCO_INVALID_OPERATION: return "invalid operation";
        case MCO_STACK_OVERFLOW: return "stack overflow - increase CC_FIBER_STACK_SIZE";
        default: return "unknown error";
    }
}

static void fiber_panic(const char* msg, fiber_task* f, mco_result res) {
    fprintf(stderr, "\n=== FIBER PANIC ===\n");
    fprintf(stderr, "Error: %s\n", msg);
    fprintf(stderr, "Minicoro result: %s (%d)\n", mco_result_str(res), (int)res);
    if (f) {
        fprintf(stderr, "Fiber: %p, control=%lld, done=%d\n", 
                (void*)f, (long long)atomic_load(&f->control), atomic_load(&f->done));
        if (f->coro) {
            fprintf(stderr, "Coroutine: %p, status=%d\n", 
                    (void*)f->coro, (int)mco_status(f->coro));
        }
    }
    fprintf(stderr, "Stack size: %d bytes (set CC_FIBER_STACK_SIZE to increase)\n", CC_FIBER_STACK_SIZE);
    fprintf(stderr, "===================\n\n");
    fflush(stderr);
    abort();
}

/* ============================================================================
* Fiber Entry Point
* ============================================================================ */

/* Simple spinlock for join handshake - ensures proper ordering between
* child setting done=1 and parent registering as waiter */
static inline void join_spinlock_lock(_Atomic int* lock) {
    for (;;) {
        /* Spin with pause until lock looks free */
        while (atomic_load_explicit(lock, memory_order_relaxed) != 0) {
            cpu_pause();
        }
        /* Try to acquire */
        int expected = 0;
        if (atomic_compare_exchange_weak_explicit(lock, &expected, 1,
                                                memory_order_acquire,
                                                memory_order_relaxed)) {
            return;
        }
    }
}

static inline void join_spinlock_unlock(_Atomic int* lock) {
    atomic_store_explicit(lock, 0, memory_order_release);
}

static void fiber_entry(mco_coro* co) {
    fiber_task* f = (fiber_task*)mco_get_user_data(co);
    if (f && f->fn) {
        /* Acquire fence + TSan annotation ensures all writes by spawner 
        * (including closure captures) are visible before we execute. */
        atomic_thread_fence(memory_order_acquire);
        TSAN_ACQUIRE(f->arg);  /* Tell TSan: sync with release on same address */
        /* Preserve nursery admission semantics without a spawn trampoline:
         * cancelled nursery means no new admission, even if cancellation races
         * after publication but before the fiber first runs. */
        if (f->admission_nursery && cc_nursery_is_cancelled(f->admission_nursery)) {
            f->result = NULL;
        } else {
            f->result = f->fn(f->arg);
        }
        f->admission_nursery = NULL;
    }
    /* Always use handshake lock to ensure proper ordering between
    * child setting done=1 and parent registering as waiter.
    * The fast path (checking join_waiters without lock) had a race where
    * we could miss waiter registrations due to memory ordering.
    * 
    * IMPORTANT: We also check join_cv_initialized under the lock to avoid
    * a race with thread joiners who init the condvar after we check it.
    * 
    * IMPORTANT: We do NOT set control=CTRL_DONE here — the worker trampoline
    * does that after mco_resume returns, ensuring the stack is truly quiescent.
    * Joiners observe done=1 for completion, and wait_for_fiber_done_state
    * spins on CTRL_DONE to ensure the worker has released the stack. */
    join_spinlock_lock(&f->join_lock);
    atomic_store_explicit(&f->done, 1, memory_order_release);
    atomic_fetch_sub_explicit(&g_sched.pending, 1, memory_order_relaxed);
    cc_sched_pressure_add(-1);
    atomic_fetch_add_explicit(&g_sched.completed, 1, memory_order_relaxed);
    int cv_initialized = atomic_load_explicit(&f->join_cv_initialized, memory_order_acquire);
    join_spinlock_unlock(&f->join_lock);
    
    /* Signal thread waiters via condvar if it was initialized when we checked */
    if (cv_initialized) {
        pthread_mutex_lock(&f->join_mu);
        pthread_cond_broadcast(&f->join_cv);
        pthread_mutex_unlock(&f->join_mu);
    }
    
    /* Ensure all stores are visible before returning */
    atomic_thread_fence(memory_order_release);
    
    /* Coroutine returns, will be cleaned up by caller (nursery) */
}

/* ============================================================================
* Worker Thread
* ============================================================================ */

/* Helper to resume fiber with error checking.
* With the unified control word the caller already holds exclusive ownership
* (CTRL_OWNED(wid)), so no separate running_lock is needed. */
static void fiber_resume(fiber_task* f) {
    if (!f->coro) {
        fiber_panic("NULL coroutine", f, MCO_INVALID_POINTER);
    }
    
    if (atomic_load_explicit(&f->done, memory_order_acquire)) {
        fprintf(stderr,
                "\n=== FIBER RESUME OF DONE FIBER ===\n"
                "Fiber: %p, id=%lu, control=%lld, done=1\n"
                "Coroutine: %p, status=%d\n"
                "park_reason=%s park_obj=%p\n"
                "yield_dest=%d pending_unpark=%d\n"
                "enqueue_src=%d enqueue_ctrl=%lld enqueue_done=%d enqueue_dest=%d\n"
                "==============================\n\n",
                (void*)f, (unsigned long)f->fiber_id,
                (long long)atomic_load(&f->control),
                (void*)f->coro, (int)mco_status(f->coro),
                f->park_reason ? f->park_reason : "null",
                f->park_obj,
                f->yield_dest,
                atomic_load_explicit(&f->pending_unpark, memory_order_relaxed),
                f->enqueue_src,
                (long long)f->enqueue_ctrl,
                f->enqueue_done,
                f->enqueue_dest);
        fflush(stderr);
        abort();
    }
    
    mco_state st = mco_status(f->coro);
    if (st != MCO_SUSPENDED) {
        fiber_panic("coroutine not in suspended state", f, MCO_NOT_SUSPENDED);
    }
    
    /* Switch TSan to the fiber context before resuming. */
    TSAN_FIBER_SWITCH(f->tsan_fiber);
    mco_result res = mco_resume(f->coro);
    /* Switch back to scheduler context after resume returns. */
    TSAN_FIBER_SWITCH(tls_tsan_sched_fiber);
    
    if (res != MCO_SUCCESS) {
        fiber_panic("mco_resume failed", f, res);
    }
}

#define WORKER_BATCH_SIZE 16  /* Standard batch size */
#define STEAL_BATCH_SIZE (LOCAL_QUEUE_SIZE / 2)  /* Steal up to half the victim's queue */
#define FAIRNESS_CHECK_INTERVAL 61  /* Inject global pop after this many local-only batches (prime) */

/* Simple xorshift64 PRNG for randomized victim selection */
static inline uint64_t xorshift64(uint64_t* state) {
    uint64_t x = *state;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    *state = x;
    return x;
}

/* ============================================================================
* Hybrid Promotion (Orphan Model)
* 
* When a worker gets stuck on a CPU-bound fiber, we "orphan" it:
* - The stuck worker keeps running its fiber 1:1 until completion
* - We spawn a permanent replacement worker to handle the queue
* - When the orphaned worker's fiber completes, it exits
* 
* Detection via sysmon thread - zero hot-path cost on workers.
* ============================================================================ */

#define SYSMON_CHECK_US 250                  /* check every 250us (0.25ms) - faster detection */
#define ORPHAN_THRESHOLD_CYCLES 3000000      /* ~1ms at 3GHz - reduce false stuck promotions */
#define MAX_EXTRA_WORKERS 8                  /* scale up to 2x cores max */

/* Stall diagnostic: warn if a fiber stays OWNED without a state transition
* for longer than this.  At ~3GHz, 15e9 cycles ≈ 5 seconds. */
#define STALL_THRESHOLD_CYCLES ((uint64_t)15000000000ULL)
#define STALL_SCAN_INTERVAL 8000             /* sysmon ticks between scans (8000 * 250us ≈ 2s) */
#define ORPHAN_COOLDOWN_CYCLES 250000        /* ~0.08ms between batch spawns */

static int sysmon_has_pending_work(void) {
    if (sched_global_peek_any()) return 1;
    for (size_t i = 0; i < g_sched.num_workers; i++) {
        if (lq_peek(&g_sched.local_queues[i])) return 1;
        if (iq_peek(&g_sched.inbox_queues[i])) return 1;
    }
    return 0;
}

static inline int worker_run_fiber(runnable_ref ref);
static inline int worker_run_fiber_accounted(runnable_ref ref);

/* Replacement worker: same as regular worker but uses global queue only.
* Permanent - sleeps when idle (doesn't burn CPU competing). */
/* After this many consecutive idle sleep iterations (~5ms each) with no work,
* the replacement worker retires.  200 * 5ms = ~1 second idle timeout. */
#define TEMP_IDLE_TIMEOUT_ITERS 200

/* Timer-service worker: scans ALL worker inboxes and runs any fiber found.
* Exits immediately when all inboxes are empty — intentionally never touches
* the global queue, so it cannot be kidnapped by a raw-syscall fiber.
*
* Spawned by sysmon when sq_drain_sysmon routes a timer-woken fiber to an
* inbox but no scheduler-sleeping or spinning worker exists to pick it up.
* This allows the heartbeat (or any cc_sleep_ms sleeper) to run even when
* all base+replacement workers are OS-blocked by raw syscalls. */
static _Atomic uint32_t g_timer_worker_active = 0;  /* 0=none, 1=one running */

/* Scan all worker inboxes and local queues with a random start to find
* a runnable fiber.  Used by replacement_worker at every work-finding
* point so inbox-routed fibers (e.g. timer wakeups) are always found
* before the global queue. */
static inline runnable_ref replacement_scan_inboxes(uint64_t* rng) {
    if (g_sched.num_workers == 0) return runnable_ref_null();
    size_t start = xorshift64(rng) % g_sched.num_workers;
    for (size_t s = 0; s < g_sched.num_workers; s++) {
        size_t w = (start + s) % g_sched.num_workers;
        runnable_ref ref = iq_pop(&g_sched.inbox_queues[w]);
        if (ref.fiber) {
            return ref;
        }
        ref = lq_steal(&g_sched.local_queues[w], (int)w);
        if (ref.fiber) return ref;
    }
    return runnable_ref_null();
}

static void* timer_service_worker(void* arg) {
    (void)arg;
    tls_sched_worker_ctx = 1;
    if (!tls_tsan_sched_fiber) {
        tls_tsan_sched_fiber = TSAN_FIBER_CREATE();
        TSAN_FIBER_SWITCH(tls_tsan_sched_fiber);
    }
    uint64_t rng_state = rdtsc();
    /* Scan all worker INBOXES and run whatever we find.  Stop when empty.
    * Intentionally does NOT touch local queues or the global queue — those
    * can contain raw-syscall fibers (kidnappers) that would kidnap this
    * thread and hold g_timer_worker_active=1 for the duration, blocking
    * all future emergency spawns.  sq_drain_sysmon routes timer-woken
    * fibers exclusively to inboxes, so inbox-only is sufficient. */
    while (atomic_load_explicit(&g_sched.running, memory_order_acquire)) {
        runnable_ref f = runnable_ref_null();
        if (g_sched.num_workers > 0) {
            size_t start = xorshift64(&rng_state) % g_sched.num_workers;
            for (size_t s = 0; s < g_sched.num_workers && !f.fiber; s++) {
                size_t w = (start + s) % g_sched.num_workers;
                f = iq_pop(&g_sched.inbox_queues[w]);
            }
        }
        if (!f.fiber) break;  /* All inboxes empty — exit */
        worker_run_fiber_accounted(f);
    }
    atomic_store_explicit(&g_timer_worker_active, 0, memory_order_release);
    atomic_fetch_sub_explicit(&g_sched.temp_worker_count, 1, memory_order_relaxed);
    return NULL;
}

static void* replacement_worker(void* arg) {
    (void)arg;
    static _Atomic int temp_worker_id_counter = 0;
    int my_temp_id = atomic_fetch_add(&temp_worker_id_counter, 1);
    tls_sched_worker_ctx = 1;
    if (!tls_tsan_sched_fiber) {
        tls_tsan_sched_fiber = TSAN_FIBER_CREATE();
        TSAN_FIBER_SWITCH(tls_tsan_sched_fiber);
    }
    uint64_t rng_state = rdtsc();
    int consecutive_idle = 0;
    uint32_t global_probe_gate = 0;

    while (atomic_load_explicit(&g_sched.running, memory_order_acquire)) {
        runnable_ref f = replacement_scan_inboxes(&rng_state);
        int nohint_fast_idle = 0;
        if (!f.fiber) {
            int has_hint = sched_global_has_hint_any();
            int allow_probe = ((global_probe_gate++ & 3u) == 0u);
            if (has_hint && allow_probe) {
                f = sched_global_pop_any();
                if (f.fiber) {
                } else {
                }
            } else if (has_hint) {
                nohint_fast_idle = 1;
            } else {
                nohint_fast_idle = 1;
            }
        }
        
        if (f.fiber) {
            consecutive_idle = 0;
            worker_run_fiber_accounted(f);
        } else {
            /* Brief spin then short sleep - balance responsiveness vs CPU burn */
            if (!nohint_fast_idle) {
                for (int spin = 0; spin < 64; spin++) {
                    f = runnable_ref_null();
                    int from_global_probe = 0;
                    f = replacement_scan_inboxes(&rng_state);
                    if (!f.fiber) {
                        if ((spin & 7) != 0) {
                        } else {
                            int has_hint = sched_global_has_hint_any();
                            int allow_probe = ((global_probe_gate++ & 3u) == 0u);
                            if (has_hint && allow_probe) {
                                f = sched_global_pop_any();
                            } else if (has_hint) {
                            }
                        }
                    }
                    if (f.fiber) goto got_work;
                    cpu_pause();
                }
                /* Explicitly account skipped spin probes on no-hint fast-idle path. */
            }
            /* Short sleep - wake primitive wakes us when work arrives */
            atomic_fetch_add_explicit(&g_sched.sleeping, 1, memory_order_release);
            /* Re-check queue after incrementing sleeping to close race window */
            f = replacement_scan_inboxes(&rng_state);
            int from_global_probe = 0;
            if (!f.fiber) {
                int has_hint = sched_global_has_hint_any();
                int allow_probe = ((global_probe_gate++ & 3u) == 0u);
                if (has_hint && allow_probe) {
                    f = sched_global_pop_any();
                } else if (has_hint) {
                }
            }
            if (f.fiber) {
                atomic_fetch_sub_explicit(&g_sched.sleeping, 1, memory_order_relaxed);
                goto got_work;
            }
            uint32_t wake_val = atomic_load_explicit(&g_sched.wake_prim.value, memory_order_acquire);
            if (!sched_global_peek_any() && !sysmon_has_pending_work()) {
                wake_primitive_wait_timeout(&g_sched.wake_prim, wake_val, 5);
            }
            atomic_fetch_sub_explicit(&g_sched.sleeping, 1, memory_order_relaxed);
            consecutive_idle++;
            /* Retire after sustained idle — no point burning a thread.
            * §6 drain rule: once a replacement worker enters retire/drain,
            * it must not accept new work (no global pop, no steal). */
            if (consecutive_idle >= TEMP_IDLE_TIMEOUT_ITERS) {
                break;
            }
            continue;
        got_work:
            consecutive_idle = 0;
            worker_run_fiber_accounted(f);
        }
    }
    /* Bookkeeping only (deadlock/sysmon heuristics) — not an LP. */
    tls_sched_worker_ctx = 0;
    atomic_fetch_sub_explicit(&g_sched.temp_worker_count, 1, memory_order_relaxed);
    return NULL;
}




/* Drain expired sleepers and route them to worker inboxes.
*
* Unlike sq_drain (which pushes to global), this function places each woken
* fiber into the inbox of the "freshest" (least-stale) base worker.  Base
* workers check their own inbox at Priority 2 — BEFORE the global queue — so
* a fiber in an inbox is guaranteed to run as soon as that worker finishes its
* current task, regardless of how many items are in the global queue.
* Replacement workers do a full inbox scan so they also find these fibers.
*
* If no base worker's inbox accepts the fiber (all full, or no workers), we
* fall back to sched_global_push so the fiber is never lost.
*
* Returns the number of fibers woken. */
static size_t sq_drain_sysmon(void) {
    if (atomic_load_explicit(&g_sleep_queue.count, memory_order_relaxed) == 0)
        return 0;

    struct timespec now_ts;
    clock_gettime(CLOCK_MONOTONIC, &now_ts);

    /* Collect all expired fibers under the lock, then route them outside it. */
    fiber_task* list = NULL;
    size_t woken = 0;
    pthread_mutex_lock(&g_sleep_queue.mu);
    fiber_task** pp = &g_sleep_queue.head;
    while (*pp) {
        fiber_task* f = *pp;
        if (now_ts.tv_sec > f->sleep_deadline.tv_sec ||
            (now_ts.tv_sec == f->sleep_deadline.tv_sec &&
            now_ts.tv_nsec >= f->sleep_deadline.tv_nsec)) {
            *pp = f->next;
            f->next = list;
            list = f;
            atomic_fetch_sub_explicit(&g_sleep_queue.count, 1, memory_order_relaxed);
            woken++;
        } else {
            pp = &f->next;
        }
    }
    pthread_mutex_unlock(&g_sleep_queue.mu);

    if (!list) return 0;

    /* Route each woken fiber to the inbox of the least-stale base worker.
    * A recently-active worker is most likely to check its inbox soon (e.g.
    * it may only be briefly kidnapped and about to return).  If the fiber
    * has a preferred worker from a previous run, try that first. */
    for (fiber_task* f = list; f; ) {
        fiber_task* next_f = f->next;
        f->next = NULL;

        int routed = 0;
        if (g_sched.num_workers > 0 && g_sched.worker_heartbeat) {
            /* First: try preferred worker (warm affinity cache) */
            int preferred = f->last_worker_id;
            if (preferred >= 0 && (size_t)preferred < g_sched.num_workers) {
                uint64_t hb = atomic_load_explicit(
                    &g_sched.worker_heartbeat[preferred].heartbeat,
                    memory_order_relaxed);
                if (hb != 0) {
                    int edge = 0;
                    if (iq_push_with_edge(&g_sched.inbox_queues[preferred], f, &edge) == 0) {
                        routed = 1;
                    }
                }
            }

            /* Second: find the freshest (least-stale) worker and try its inbox */
            if (!routed) {
                int best = -1;
                uint64_t best_hb = 0;
                for (size_t w = 0; w < g_sched.num_workers; w++) {
                    uint64_t hb = atomic_load_explicit(
                        &g_sched.worker_heartbeat[w].heartbeat,
                        memory_order_relaxed);
                    if (hb != 0 && hb > best_hb) {
                        best_hb = hb;
                        best = (int)w;
                    }
                }
                if (best >= 0) {
                    int edge = 0;
                    if (iq_push_with_edge(&g_sched.inbox_queues[best], f, &edge) == 0) {
                        routed = 1;
                    }
                }
            }
        }

        if (!routed) {
            /* Last resort: global queue — fiber will not be lost */
            sched_global_push(f, f->last_worker_id);
        }

        f = next_f;
    }
    return woken;
}

/* Spawn a detached OS thread running fn(NULL).
* Returns 0 on success, errno on failure. */
static int spawn_detached_worker(void* (*fn)(void*)) {
    pthread_t tid;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    int rc = pthread_create(&tid, &attr, fn, NULL);
    pthread_attr_destroy(&attr);
    return rc;
}

static void* sysmon_main(void* arg) {
    (void)arg;
    int low_worker_pressure_streak = 0;
    uint64_t last_sleepq_emerg_cycles = 0;
#if CC_V3_DIAGNOSTICS
    uint64_t negative_pressure_streak = 0;
#endif
    while (atomic_load_explicit(&g_sched.sysmon_running, memory_order_acquire)) {
        usleep(SYSMON_CHECK_US);
        
        /* Wake any fibers whose sleep deadline has passed.
        *
        * sq_drain_sysmon routes them to worker INBOXES (Priority 2, before
        * global).  If all workers are OS-blocked (kidnapped by raw syscalls),
        * sleeping==0 && spinning==0 — the normal wake is a no-op.  In that
        * case we spawn an emergency replacement worker whose full inbox scan
        * will find the woken fiber and run it. */
        {
            size_t woken = sq_drain_sysmon() + tpq_drain();
            if (woken > 0) {
                size_t sl = atomic_load_explicit(&g_sched.sleeping, memory_order_relaxed);
                size_t sp = atomic_load_explicit(&g_sched.spinning, memory_order_relaxed);
                if (sl > 0 || sp > 0) {
                    /* Workers are awake — send targeted wakes */
                    for (size_t w = 0; w < woken && w < g_sched.num_workers; w++) {
                        wake_one_if_sleeping_unconditional();
                    }
                } else {
                    /* All regular workers are OS-blocked (kidnapped by raw syscalls).
                    * Spawn a dedicated inbox-only timer-service worker to drain the
                    * inboxes we just filled and then exit.  It never touches the
                    * global queue so it can't itself get kidnapped.  The
                    * g_timer_worker_active flag ensures at most one is live at a time;
                    * a longer cooldown keeps it from racing the normal orphan path. */
                    uint64_t now_emerg = rdtsc();
                    uint32_t tw_expected = 0;
                    if ((now_emerg - last_sleepq_emerg_cycles) > (uint64_t)ORPHAN_COOLDOWN_CYCLES * 4 &&
                            atomic_compare_exchange_strong_explicit(
                                &g_timer_worker_active, &tw_expected, 1,
                                memory_order_acquire, memory_order_relaxed)) {
                        last_sleepq_emerg_cycles = now_emerg;
                        atomic_fetch_add_explicit(&g_sched.temp_worker_count, 1,
                                                memory_order_relaxed);
                        if (spawn_detached_worker(timer_service_worker) == 0)
                            atomic_fetch_add_explicit(&g_sched.promotion_count, 1,
                                                    memory_order_relaxed);
                        else {
                            atomic_fetch_sub_explicit(&g_sched.temp_worker_count, 1,
                                                    memory_order_relaxed);
                            atomic_store_explicit(&g_timer_worker_active, 0,
                                                memory_order_release);
                        }
                    }
                }
            }
        }


        size_t current = atomic_load(&g_sched.temp_worker_count);
        if (current >= MAX_EXTRA_WORKERS) continue;
        size_t sleeping = atomic_load_explicit(&g_sched.sleeping, memory_order_relaxed);
        size_t spinning = atomic_load_explicit(&g_sched.spinning, memory_order_relaxed);
        size_t active = atomic_load_explicit(&g_sched.active, memory_order_relaxed);
        int64_t pressure = atomic_load_explicit(&g_sched.pressure, memory_order_relaxed);
#if CC_V3_DIAGNOSTICS
        atomic_fetch_add_explicit(&g_sched.pressure_samples, 1, memory_order_relaxed);
        if (pressure > 0) {
            atomic_fetch_add_explicit(&g_sched.pressure_positive_samples, 1, memory_order_relaxed);
            negative_pressure_streak = 0;
        } else if (pressure < 0) {
            atomic_fetch_add_explicit(&g_sched.pressure_negative_samples, 1, memory_order_relaxed);
            if (negative_pressure_streak < UINT64_MAX) negative_pressure_streak++;
            uint64_t prev = atomic_load_explicit(&g_sched.pressure_negative_streak_max, memory_order_relaxed);
            while (negative_pressure_streak > prev) {
                if (atomic_compare_exchange_weak_explicit(&g_sched.pressure_negative_streak_max, &prev,
                                                        negative_pressure_streak,
                                                        memory_order_relaxed,
                                                        memory_order_relaxed)) {
                    break;
                }
            }
        } else {
            negative_pressure_streak = 0;
        }
#endif

        /* Scale-up gate (§7): require runnable work evidence and positive pressure. */
        if (!sysmon_has_pending_work()) {
#if CC_V3_DIAGNOSTICS
            atomic_fetch_add_explicit(&g_sched.pressure_block_no_work, 1, memory_order_relaxed);
#endif
            continue;
        }
        if (pressure <= 0) {
#if CC_V3_DIAGNOSTICS
            atomic_fetch_add_explicit(&g_sched.pressure_block_nonpositive, 1, memory_order_relaxed);
#endif
            continue;
        }
        if (g_sched.num_workers <= 2) {
            /* Small-worker mode: require sustained pressure and full base occupancy
            * before promoting replacement workers. This reduces over-promotion
            * churn while keeping responsiveness under load. */
            if (pressure > 0) {
                if (low_worker_pressure_streak < 64) low_worker_pressure_streak++;
            } else {
                low_worker_pressure_streak = 0;
            }
            if (low_worker_pressure_streak < 3) continue;
            if (active < g_sched.num_workers) continue;
        } else {
            low_worker_pressure_streak = 0;
        }
        /* If current idle capacity can plausibly absorb pressure, don't promote yet. */
        if ((sleeping + spinning) > 0 && pressure <= (int64_t)(sleeping + spinning)) {
#if CC_V3_DIAGNOSTICS
            atomic_fetch_add_explicit(&g_sched.pressure_block_idle_capacity, 1, memory_order_relaxed);
#endif
            continue;
        }
        /* Check for stuck workers */
        uint64_t now = rdtsc();
        size_t stuck = 0;
        for (size_t i = 0; i < g_sched.num_workers; i++) {
            /* Heartbeat is a heuristic liveness signal; no publication edge needed. */
            uint64_t hb = atomic_load_explicit(&g_sched.worker_heartbeat[i].heartbeat, memory_order_relaxed);
            if (hb != 0 && (now - hb) >= ORPHAN_THRESHOLD_CYCLES) {
                /* Don't count workers with channel-parked fibers as stuck.
                * A stale heartbeat + parked fibers means the worker went idle
                * waiting for unparks — correct behavior, not a CPU orphan. */
                size_t pf = g_sched.worker_parked
                    ? atomic_load_explicit(&g_sched.worker_parked[i], memory_order_relaxed)
                    : 0;
                if (pf == 0)
                    stuck++;
            }
        }
        
        /* Workers executing intentional long CPU-bound pool tasks are not
        * actually stuck — they are productively occupied.  Subtract them
        * from the stuck count before deciding to promote. */
        size_t busy = atomic_load_explicit(&g_pool_tasks_active, memory_order_relaxed);
        if (busy >= stuck) stuck = 0;
        else stuck -= busy;

        if (stuck == 0) {
#if CC_V3_DIAGNOSTICS
            atomic_fetch_add_explicit(&g_sched.pressure_block_not_stuck, 1, memory_order_relaxed);
#endif
            continue;
        }
        
        /* Conservative growth: promote one replacement worker per scale event.
        * This avoids replacement-worker overprovisioning, which can inflate
        * replacement callsite churn on bursty workloads (e.g., pigz). */
        size_t total_workers = g_sched.num_workers + current;
        size_t to_spawn = 1;
        if (to_spawn > (MAX_EXTRA_WORKERS - current))
            to_spawn = MAX_EXTRA_WORKERS - current;
        
        if (to_spawn == 0) continue;
#if CC_V3_DIAGNOSTICS
        atomic_fetch_add_explicit(&g_sched.pressure_gate_passes, 1, memory_order_relaxed);
#endif
        
        /* Rate limit overall scaling bursts */
        uint64_t last = atomic_load_explicit(&g_sched.last_promotion_cycles, memory_order_acquire);
        if ((now - last) < ORPHAN_COOLDOWN_CYCLES) continue;
        /* Promotion cooldown gate is bookkeeping-only; relaxed ordering is sufficient. */
        if (!atomic_compare_exchange_strong_explicit(&g_sched.last_promotion_cycles, &last, now,
                                                    memory_order_relaxed,
                                                    memory_order_relaxed))
            continue;
        
        /* Spawn all needed workers at once */
#if CC_V3_DIAGNOSTICS
        atomic_fetch_add_explicit(&g_sched.pressure_promoted_workers, to_spawn, memory_order_relaxed);
#endif
        for (size_t s = 0; s < to_spawn; s++) {
            /* temp_worker_count participates in heuristic accounting, not correctness LPs. */
            if (!atomic_compare_exchange_strong_explicit(&g_sched.temp_worker_count, &current, current + 1,
                                                        memory_order_relaxed,
                                                        memory_order_relaxed))
                break;
            current++;
            
            if (spawn_detached_worker(replacement_worker) == 0)
                atomic_fetch_add_explicit(&g_sched.promotion_count, 1, memory_order_relaxed);
            else
                atomic_fetch_sub_explicit(&g_sched.temp_worker_count, 1, memory_order_relaxed);
        }
    }
    return NULL;
}

/* Forward declaration — defined after park helpers */
static void worker_commit_park(fiber_task* f, int wid);

static inline int worker_run_fiber(runnable_ref ref) {
    fiber_task* f = ref.fiber;
    if (!f) return 0;
    if (!runnable_ref_matches_current(ref)) return 0;
    if (f->enqueue_src == 8) {
        cc_sched_io_wake_stat_inc(&g_cc_sched_io_wake_stats.run_attempts);
    }
    int wid = tls_worker_id;
    int64_t expected = CTRL_QUEUED;
    int64_t owned = (wid >= 0) ? CTRL_OWNED(wid) : CTRL_OWNED_TEMP;
    if (!atomic_compare_exchange_strong_explicit(&f->control, &expected, owned,
                                                memory_order_acq_rel,
                                                memory_order_acquire)) {
        if (f->enqueue_src == 8) {
            cc_sched_io_wake_stat_inc(&g_cc_sched_io_wake_stats.run_skip_stale);
        }
        if (cc__io_wait_trace_fiber(f)) {
            fprintf(stderr,
                    "[cc:io_wait:sched] run_skip fiber=%p id=%lu worker=%d control=%lld pending=%d enqueue_src=%d\n",
                    (void*)f,
                    (unsigned long)f->fiber_id,
                    wid,
                    (long long)expected,
                    atomic_load_explicit(&f->pending_unpark, memory_order_relaxed),
                    f->enqueue_src);
        }
        return 0;  /* Stale/duplicate queue entry */
    }
    if (f->enqueue_src == 8) {
        cc_sched_io_wake_stat_inc(&g_cc_sched_io_wake_stats.run_claims);
    }
    if (cc__io_wait_trace_fiber(f)) {
        fprintf(stderr,
                "[cc:io_wait:sched] run_start fiber=%p id=%lu worker=%d enqueue_src=%d enqueue_ctrl=%lld pending=%d\n",
                (void*)f,
                (unsigned long)f->fiber_id,
                wid,
                f->enqueue_src,
                (long long)f->enqueue_ctrl,
                atomic_load_explicit(&f->pending_unpark, memory_order_relaxed));
    }
    if (cc__req_wake_trace_fiber(f)) {
        fprintf(stderr,
                "CC_REQ_WAKE_RUN: start fiber=%lu worker=%d obj=%p enqueue_src=%d enqueue_ctrl=%lld pending=%d\n",
                (unsigned long)f->fiber_id,
                wid,
                f->park_obj,
                f->enqueue_src,
                (long long)f->enqueue_ctrl,
                atomic_load_explicit(&f->pending_unpark, memory_order_relaxed));
    }
    cc_v3_assert_control_is(f, owned, "RUNNABLE->RUNNING");
    tls_current_fiber = f;
    /* Only base workers should refresh run affinity. Replacement/timer workers
    * are emergency executors, not ownership targets; letting wid<0 overwrite
    * the last base-worker hint drops channel locality and later forces
    * no-affinity unparks under constrained worker counts. */
    if (wid >= 0) {
        f->last_worker_id = wid;
        f->last_worker_src = 1;
    }
    /* Restore the fiber's saved nursery context on this thread so that
    * cc_cancelled() works correctly even when the fiber migrates to a
    * different OS thread (e.g. a replacement or timer-service worker).
    * Save the previous value so we can restore it after the fiber yields. */
    CCNursery* prev_nursery = cc__tls_current_nursery;
    unsigned prev_deadlock_suppress_depth = tls_deadlock_suppress_depth;
    unsigned prev_external_wait_depth = tls_external_wait_depth;
    cc__tls_current_nursery = f->saved_nursery;
    tls_deadlock_suppress_depth = f->deadlock_suppress_depth;
    tls_external_wait_depth = f->external_wait_depth;
    fiber_resume(f);
    if (cc__io_wait_trace_fiber(f)) {
        fprintf(stderr,
                "[cc:io_wait:sched] run_resume fiber=%p id=%lu worker=%d dest=%d control=%lld pending=%d done=%d\n",
                (void*)f,
                (unsigned long)f->fiber_id,
                wid,
                f->yield_dest,
                (long long)atomic_load_explicit(&f->control, memory_order_relaxed),
                atomic_load_explicit(&f->pending_unpark, memory_order_relaxed),
                atomic_load_explicit(&f->done, memory_order_relaxed));
    }
    if (cc__req_wake_trace_fiber(f)) {
        fprintf(stderr,
                "CC_REQ_WAKE_RUN: resumed fiber=%lu worker=%d obj=%p dest=%d done=%d control=%lld pending=%d\n",
                (unsigned long)f->fiber_id,
                wid,
                f->park_obj,
                f->yield_dest,
                atomic_load_explicit(&f->done, memory_order_relaxed),
                (long long)atomic_load_explicit(&f->control, memory_order_relaxed),
                atomic_load_explicit(&f->pending_unpark, memory_order_relaxed));
    }
    f->deadlock_suppress_depth = tls_deadlock_suppress_depth;
    f->external_wait_depth = tls_external_wait_depth;
    f->saved_nursery = cc__tls_current_nursery;
    cc__tls_current_nursery = prev_nursery;
    tls_deadlock_suppress_depth = prev_deadlock_suppress_depth;
    tls_external_wait_depth = prev_external_wait_depth;
    tls_current_fiber = NULL;
    
    /* ---- Trampoline: mco_resume returned, stack is quiescent ----
    *
    * CRITICAL: Before touching any fiber fields, verify we still own it via CAS.
    * There is a race where wait_for_fiber_done_state (old code) could give up
    * its spin, the joiner frees the fiber to the pool, and a new incarnation
    * recycles it — all while this trampoline was preempted between mco_resume
    * returning and reaching this point.  If the fiber was recycled, control is
    * no longer OWNED(me), so the CAS fails and we bail out safely.
    *
    * We read yield_dest and done first (they're set by OUR coroutine run, so
    * they're valid), then use CAS to atomically transition control. */
    int dest = f->yield_dest;
    int is_done = atomic_load_explicit(&f->done, memory_order_acquire);

    if (is_done) {
        /* Fiber completed — try to set CTRL_DONE.  CAS ensures we still own it. */
        f->yield_dest = YIELD_NONE;
        int64_t exp_owned = owned;
        if (atomic_compare_exchange_strong_explicit(&f->control, &exp_owned, CTRL_DONE,
                                                    memory_order_release,
                                                    memory_order_relaxed)) {
            /* Wake joining fiber only after CTRL_DONE is visible. This avoids
            * waking it early only to spin in wait_for_fiber_done_state(). */
            join_spinlock_lock(&f->join_lock);
            fiber_task* waiter =
                atomic_exchange_explicit(&f->join_waiter_fiber, NULL, memory_order_acq_rel);
            join_spinlock_unlock(&f->join_lock);
            if (waiter) {
                cc__fiber_unpark_tagged(waiter, CC_FIBER_UNPARK_REASON_JOIN);
            }
        }
        /* If CAS failed: fiber was already recycled — another incarnation owns it.
        * Our done=1 was from OUR run; the recycled fiber has done=0.  Safe to
        * just walk away. */
    } else if (dest != YIELD_NONE) {
        f->yield_dest = YIELD_NONE;
        if (dest == YIELD_PARK) {
            /* Yield-before-commit park: worker_commit_park CAS(OWNED→PARKED)
            * handles the recycled-fiber case itself (CAS fails → bail out).
            * Skip the redundant pre-check acquire load to reduce hot-path
            * atomic overhead on the park fast path. */
            worker_commit_park(f, wid);
        } else {
            /* Yield: CAS OWNED→QUEUED then enqueue.
            * If CAS fails, fiber was recycled — bail out. */
            int64_t exp_owned2 = owned;
            if (atomic_compare_exchange_strong_explicit(&f->control, &exp_owned2, CTRL_QUEUED,
                                                        memory_order_release,
                                                        memory_order_relaxed)) {
                f->enqueue_src = 1;
                f->enqueue_ctrl = owned;
                f->enqueue_done = 0;
                f->enqueue_dest = dest;
                switch (dest) {
                    case YIELD_LOCAL:
                        if (wid >= 0 && lq_push(&g_sched.local_queues[wid], f) == 0)
                            break;
                        /* fall through to global if local full or no worker id */
                        /* fallthrough */
                    case YIELD_GLOBAL:
                        sched_global_push(f, wid);
                        break;
                    case YIELD_SLEEP:
                        sq_push(f);
                        break;
                }
            }
            /* If CAS failed: fiber recycled — bail out safely. */
        }
    }
    /* Else: no yield_dest, not done → fiber parked (committed CTRL_PARKED
    * before yielding).  Nothing to do — unpark handles it. */
    
    return 1;
}

static inline int worker_run_fiber_accounted(runnable_ref ref) {
    atomic_fetch_add_explicit(&g_sched.active, 1, memory_order_relaxed);
    int rc = worker_run_fiber(ref);
    atomic_fetch_sub_explicit(&g_sched.active, 1, memory_order_relaxed);
    sched_maybe_promote_run_phase();
    return rc;
}

static inline int cc__task_should_stay_on_target_inbox(fiber_task* f, int target_worker) {
    if (!f || target_worker < 0) return 0;
    if (f->enqueue_src == 2 &&
        f->spawn_publish_valid &&
        f->spawn_publish_route == (unsigned char)SPAWN_ROUTE_INBOX &&
        f->spawn_publish_target_worker == target_worker &&
        f->last_worker_id < 0) {
        return 1;
    }
    if (f->enqueue_src == 8 &&
        f->last_worker_id == target_worker) {
        return 1;
    }
    return 0;
}

static inline int cc__restore_inbox_task(size_t victim, fiber_task* f) {
    int edge = 0;
    for (int attempt = 0; attempt < 8; attempt++) {
        if (iq_push_with_edge(&g_sched.inbox_queues[victim], f, &edge) == 0) {
            if (edge) {
                wake_target_worker_if_sleeping((int)victim);
            }
            return 1;
        }
        cpu_pause();
    }
    return 0;
}

static inline runnable_ref worker_try_steal_one(int worker_id, uint64_t* rng_state) {
    if (g_sched.num_workers <= 1) return runnable_ref_null();
    size_t victim = (size_t)(xorshift64(rng_state) % g_sched.num_workers);
    if ((int)victim == worker_id) return runnable_ref_null();
    runnable_ref ref = iq_pop(&g_sched.inbox_queues[victim]);
    fiber_task* f = ref.fiber;
    if (f) {
        if (cc__task_should_stay_on_target_inbox(f, (int)victim) &&
            cc__restore_inbox_task(victim, f)) {
            return lq_steal(&g_sched.local_queues[victim], (int)victim);
        }
        return ref;
    }
    return lq_steal(&g_sched.local_queues[victim], (int)victim);
}

static _Atomic int g_cc_untended_first_mode = -1; /* -1 unknown, 0 off, 1 on */

static inline int cc_untended_first_enabled(void) {
    int mode = atomic_load_explicit(&g_cc_untended_first_mode, memory_order_acquire);
    if (mode >= 0) return mode;
    const char* env = getenv("CC_UNTENDED_FIRST_STEAL");
    mode = (env && env[0] == '1') ? 1 : 0;
    int expected = -1;
    if (!atomic_compare_exchange_strong_explicit(&g_cc_untended_first_mode,
                                                &expected,
                                                mode,
                                                memory_order_release,
                                                memory_order_acquire)) {
        mode = atomic_load_explicit(&g_cc_untended_first_mode, memory_order_acquire);
    }
    return mode;
}

static inline int worker_victim_likely_untended(size_t victim, uint64_t now_cycles) {
    if (!g_sched.worker_heartbeat) return 0;
    uint64_t hb = atomic_load_explicit(&g_sched.worker_heartbeat[victim].heartbeat,
                                    memory_order_relaxed);
    return (hb == 0) || ((now_cycles - hb) >= ORPHAN_THRESHOLD_CYCLES);
}

static void* worker_main(void* arg) {
    int worker_id = (int)(size_t)arg;
    tls_worker_id = worker_id;
    tls_sched_worker_ctx = 1;
    local_queue* my_queue = &g_sched.local_queues[worker_id];
    runnable_ref batch[WORKER_BATCH_SIZE];
    runnable_ref steal_buf[STEAL_BATCH_SIZE];
    
    /* Initialize TSan fiber context for the scheduler thread.
    * This models user-space fiber switches for TSan. */
    if (!tls_tsan_sched_fiber) {
        tls_tsan_sched_fiber = TSAN_FIBER_CREATE();
        TSAN_FIBER_SWITCH(tls_tsan_sched_fiber);
    }

    /* Per-worker PRNG state for randomized stealing */
    uint64_t rng_state = (uint64_t)worker_id * 0x9E3779B97F4A7C15ULL + rdtsc();
#if CC_RUNTIME_V3 && CC_V3_DIAGNOSTICS
    const int v3_stats = cc_v3_worker_stats_enabled();
    const int lifecycle_asserts = cc_v3_worker_lifecycle_enabled();
    if (v3_stats) cc_v3_worker_stats_maybe_init();
#else
    const int lifecycle_asserts = 0;
    const int v3_stats = 0;
#endif
    const int lifecycle_track = lifecycle_asserts;
    (void)v3_stats;
    /* Bounded fairness: after FAIRNESS_CHECK_INTERVAL consecutive batches
    * with no global work, inject one global queue pop so fibers pushed to
    * global (e.g. by the affinity escape hatch) don't starve. */
    int local_only_batches = 0;
    
    while (atomic_load_explicit(&g_sched.running, memory_order_acquire)) {
        if (lifecycle_track) cc_v3_worker_lifecycle_set(worker_id, CC_WL_ACTIVE, "loop_start");
        /* Fairness: periodically inject one global pop before local drain */
        size_t count = 0;
        int startup_phase = sched_in_startup_phase();
        if (local_only_batches >= FAIRNESS_CHECK_INTERVAL &&
            g_sched.run_queue &&
            sched_global_has_hint_any()) {
            runnable_ref gf = sched_global_pop_for_worker(worker_id);
            if (gf.fiber) {
                batch[count++] = gf;
                local_only_batches = 0;
#if CC_RUNTIME_V3 && CC_V3_DIAGNOSTICS
                if (v3_stats) {
                    atomic_fetch_add_explicit(&g_cc_v3_wm_global_pulls, 1, memory_order_relaxed);
                }
#endif
            }
        }
#if CC_RUNTIME_V3
        if (count == 0) {
            fiber_task* v3_next = cc_sched_worker_next();
            if (v3_next) {
                batch[count++] = runnable_ref_snapshot(v3_next);
#if CC_V3_DIAGNOSTICS
                if (v3_stats) {
                    atomic_fetch_add_explicit(&g_cc_v3_wm_seed_v3_batches, 1, memory_order_relaxed);
                }
#endif
            }
        }
#endif
        /* Startup deterministic service: always probe own inbox then one global
        * before bulk local drain. This bounds startup service lag for both lanes. */
        if (count == 0 && startup_phase) {
            runnable_ref sf = iq_pop(&g_sched.inbox_queues[worker_id]);
            if (sf.fiber) {
                batch[count++] = sf;
            }
            if (count < WORKER_BATCH_SIZE && sched_global_has_hint_any()) {
                sf = sched_global_pop_for_worker(worker_id);
                if (sf.fiber) {
                    batch[count++] = sf;
                }
            }
        }
        /* Priority 1-4: acquire runnable work */
        /* Priority 1: Pop from local queue (no contention) */
        while (count < WORKER_BATCH_SIZE) {
            runnable_ref f = lq_pop(my_queue);
            if (!f.fiber) break;
            batch[count++] = f;
#if CC_RUNTIME_V3 && CC_V3_DIAGNOSTICS
            if (v3_stats) {
                atomic_fetch_add_explicit(&g_cc_v3_wm_local_pulls, 1, memory_order_relaxed);
            }
#endif
        }
        
        /* Priority 2: Pop from inbox queue */
        while (count < WORKER_BATCH_SIZE) {
            runnable_ref f = iq_pop(&g_sched.inbox_queues[worker_id]);
            if (!f.fiber) break;
            batch[count++] = f;
#if CC_RUNTIME_V3 && CC_V3_DIAGNOSTICS
            if (v3_stats) {
                atomic_fetch_add_explicit(&g_cc_v3_wm_inbox_pulls, 1, memory_order_relaxed);
            }
#endif
        }
        
        /* Priority 3: Pop from global queue.
        * Only probe global when local+inbox had no work in this loop.
        * Fairness injection above still prevents long-term global starvation. */
        if (count == 0) {
            while (count < WORKER_BATCH_SIZE && sched_global_has_hint_any()) {
                runnable_ref f = sched_global_pop_for_worker(worker_id);
                if (!f.fiber) break;
                batch[count++] = f;
#if CC_RUNTIME_V3 && CC_V3_DIAGNOSTICS
                if (v3_stats) {
                    atomic_fetch_add_explicit(&g_cc_v3_wm_global_pulls, 1, memory_order_relaxed);
                }
#endif
            }
        }
        
        /* Priority 4: Batch steal from other workers with randomized victim selection */
        if (count == 0 && g_sched.num_workers > 1) {
            /* Randomize starting victim to avoid thundering herd */
            size_t start = (size_t)(xorshift64(&rng_state) % g_sched.num_workers);
            const int untended_first = cc_untended_first_enabled();
            const uint64_t now_cycles = rdtsc();

            /* Pass 0: adopt untended slices first. Pass 1: fallback to any slice. */
            for (int pass = 0; pass < (untended_first ? 2 : 1) && count == 0; pass++) {
                for (size_t j = 0; j < g_sched.num_workers; j++) {
                    size_t victim = (start + j) % g_sched.num_workers;
                    if ((int)victim == worker_id) continue;
                    if (untended_first && pass == 0 &&
                        !worker_victim_likely_untended(victim, now_cycles)) {
                        continue;
                    }
                    
                    runnable_ref f = iq_pop(&g_sched.inbox_queues[victim]);
                    if (f.fiber) {
                        if (cc__task_should_stay_on_target_inbox(f.fiber, (int)victim) &&
                            cc__restore_inbox_task(victim, f.fiber)) {
                            continue;
                        }
                        batch[count++] = f;
#if CC_RUNTIME_V3 && CC_V3_DIAGNOSTICS
                        if (v3_stats) {
                            atomic_fetch_add_explicit(&g_cc_v3_wm_steal_inbox_pulls, 1, memory_order_relaxed);
                        }
#endif
                        break;
                    }
                    size_t stolen = lq_steal_batch(&g_sched.local_queues[victim],
                                                (int)victim,
                                                steal_buf, STEAL_BATCH_SIZE);
                    if (stolen > 0) {
                        /* Execute first task immediately, put rest in batch or local queue */
                        batch[count++] = steal_buf[0];
#if CC_RUNTIME_V3 && CC_V3_DIAGNOSTICS
                        if (v3_stats) {
                            atomic_fetch_add_explicit(&g_cc_v3_wm_steal_local_pulls, 1, memory_order_relaxed);
                        }
#endif
                        for (size_t s = 1; s < stolen && count < WORKER_BATCH_SIZE; s++) {
                            batch[count++] = steal_buf[s];
#if CC_RUNTIME_V3 && CC_V3_DIAGNOSTICS
                            if (v3_stats) {
                                atomic_fetch_add_explicit(&g_cc_v3_wm_steal_local_pulls, 1, memory_order_relaxed);
                            }
#endif
                        }
                        /* Any remaining stolen tasks go to our local queue */
                        for (size_t s = count; s < stolen; s++) {
                            if (lq_push(my_queue, steal_buf[s].fiber) != 0) {
                                /* Local queue full, put overflow in global */
                            sched_global_push(steal_buf[s].fiber, worker_id);
                            }
                        }
                        break;  /* Got work, stop stealing */
                    }
                }
            }
        }
        
        if (count > 0) {
            if (lifecycle_track) cc_v3_worker_lifecycle_set(worker_id, CC_WL_ACTIVE, "batch_run");
#if CC_RUNTIME_V3 && CC_V3_DIAGNOSTICS
            if (v3_stats) {
                atomic_fetch_add_explicit(&g_cc_v3_wm_batches, 1, memory_order_relaxed);
            }
#endif
            /* Update heartbeat when executing work (sysmon checks for stuck workers) */
            atomic_store_explicit(&g_sched.worker_heartbeat[worker_id].heartbeat, rdtsc(), memory_order_relaxed);
            local_only_batches++;
            
            /* Batch execute */
            for (size_t i = 0; i < count; i++) {
                worker_run_fiber_accounted(batch[i]);
            }
            continue;
        }

#if CC_RUNTIME_V3
        {
            fiber_task* idle_probe = cc_sched_worker_idle_probe();
            if (idle_probe) {
#if CC_RUNTIME_V3 && CC_V3_DIAGNOSTICS
                if (v3_stats) {
                    atomic_fetch_add_explicit(&g_cc_v3_wm_idle_probe_hits, 1, memory_order_relaxed);
                }
#endif
                if (lifecycle_track) cc_v3_worker_lifecycle_set(worker_id, CC_WL_ACTIVE, "idle_probe_hit");
                atomic_store_explicit(&g_sched.worker_heartbeat[worker_id].heartbeat, rdtsc(), memory_order_relaxed);
                local_only_batches++;
                worker_run_fiber_accounted(runnable_ref_snapshot(idle_probe));
                continue;
            }
        }
#endif
        
#if CC_RUNTIME_V3 && CC_V3_DIAGNOSTICS
        if (v3_stats) {
            atomic_fetch_add_explicit(&g_cc_v3_wm_empty_loops, 1, memory_order_relaxed);
        }
#endif
        
        /* No work - enter spinning state */
        cc_v3_worker_lifecycle_set(worker_id, CC_WL_IDLE_SPIN, "enter_spin");
        size_t old_spinning = atomic_fetch_add_explicit(&g_sched.spinning, 1, memory_order_seq_cst);
        cc_sched_wait_stat_inc(&g_cc_sched_wait_stats.spin_entries);
        
        /* Spin briefly checking local, global, and stealing */
        int spin_found_work = 0;
        int spin_iters = SPIN_FAST_ITERS;
        for (int spin = 0; spin < spin_iters; spin++) {
            runnable_ref f = lq_pop(my_queue);
            if (!f.fiber) {
                f = iq_pop(&g_sched.inbox_queues[worker_id]);
            }
            if (f.fiber) spin_found_work = 1;
            if (f.fiber) {
                atomic_fetch_sub_explicit(&g_sched.spinning, 1, memory_order_relaxed);
                cc_v3_worker_lifecycle_set(worker_id, CC_WL_ACTIVE, "spin_found_work");
                worker_run_fiber_accounted(f);
                goto next_iteration;
            }
            cpu_pause();
        }
        
        /* Yield a few times before sleeping, with steal attempts.
        * On the final yield iteration also probe global: backlog-promoted
        * spawns land in global and need to be visible before we sleep.
        * Single check on the last iteration avoids the contention of
        * probing on every iteration (8 workers × 8 iters = stampede). */
        int yield_iters = SPIN_YIELD_ITERS;
        for (int y = 0; y < yield_iters; y++) {
            cc_sched_wait_stat_inc(&g_cc_sched_wait_stats.yield_calls);
            sched_yield();
            runnable_ref f = lq_pop(my_queue);
            if (!f.fiber) {
                f = iq_pop(&g_sched.inbox_queues[worker_id]);
            }
            if (!f.fiber && y == yield_iters - 1 && sched_global_has_hint_any()) {
                f = sched_global_pop_for_worker(worker_id);
            }
            if (f.fiber) {
                atomic_fetch_sub_explicit(&g_sched.spinning, 1, memory_order_relaxed);
                cc_v3_worker_lifecycle_set(worker_id, CC_WL_ACTIVE, "yield_found_work");
                cc_sched_wait_stat_inc(&g_cc_sched_wait_stats.yield_found_work);
                worker_run_fiber_accounted(f);
                goto next_iteration;
            }
        }

        /* One bounded pre-sleep steal probe: helps catch untended work without
        * reintroducing high-frequency steal churn in spin/yield loops.
        * Also check global: last chance before sleeping to catch tasks that
        * were backlog-promoted to global and whose wake signal fired while
        * we were still in the spin/yield phase above. */
        {
            runnable_ref f = runnable_ref_null();
            if (sched_global_has_hint_any()) {
                f = sched_global_pop_for_worker(worker_id);
            }
            if (!f.fiber && g_sched.num_workers > 1) {
                f = worker_try_steal_one(worker_id, &rng_state);
            }
            if (f.fiber) {
                atomic_fetch_sub_explicit(&g_sched.spinning, 1, memory_order_relaxed);
                if (lifecycle_track) cc_v3_worker_lifecycle_set(worker_id, CC_WL_ACTIVE, "pre_sleep_steal");
                worker_run_fiber_accounted(f);
                goto next_iteration;
            }
        }

        /* CRITICAL: Increment sleeping BEFORE decrementing spinning.
        * This ensures there's always at least one counter > 0 during the transition.
        * Otherwise there's a race window where spinning=0 and sleeping=0, and
        * spawners see no workers to wake. */
        atomic_fetch_add_explicit(&g_sched.sleeping, 1, memory_order_release);
        atomic_fetch_sub_explicit(&g_sched.spinning, 1, memory_order_relaxed);
        cc_v3_worker_lifecycle_set(worker_id, CC_WL_SLEEP, "enter_sleep");
        cc_sched_wait_stat_inc(&g_cc_sched_wait_stats.sleep_entries);
        
        /* Sleep using fast wake primitive (futex/ulock instead of condvar) */
        /* Note: we check queue emptiness, not pending count, because parked fibers
        * are "pending" but not runnable. We wake when new runnable work is added. */
        /* sleeping already incremented above */
        
        /* CRITICAL: Re-check queues immediately after incrementing sleeping.
        * This closes the race window where:
        * 1. Worker exits spin (spinning=0)
        * 2. Worker does steal attempts (sleeping=0 still)
        * 3. Spawner pushes work, sees sleeping=0 spinning=0, wakes (no effect)
        * 4. Worker increments sleeping, reads wake_val, waits forever
        * 
        * By checking queues HERE, after sleeping is incremented, we ensure:
        * - If spawner already pushed, we see the work and process it
        * - If spawner pushes after this check, it will see sleeping>0 and wake us */
        {
            runnable_ref f = lq_pop(my_queue);
            if (!f.fiber) {
                f = iq_pop(&g_sched.inbox_queues[worker_id]);
            }
            if (f.fiber) {
                atomic_fetch_sub_explicit(&g_sched.sleeping, 1, memory_order_relaxed);
                cc_v3_worker_lifecycle_set(worker_id, CC_WL_ACTIVE, "post_sleep_recheck");
                worker_run_fiber_accounted(f);
                goto next_iteration;
            }
        }
        
        /* Check for deadlock: all workers sleeping + parked fibers + no runnable work */
        cc__fiber_check_deadlock();
        
        /* Sleep while no runnable work (check local queue, global queue) and still running.
        * Wake primitive is signaled when fibers are spawned or unparked.
        * Periodically wake to check for deadlock (every ~500ms).
        * 
        * CRITICAL: We must read wake_val AFTER checking queues, not before!
        * Otherwise there's a race:
        * 1. Worker reads wake_val
        * 2. Worker checks queues -> empty
        * 3. Spawner pushes fiber, increments wake_val, calls wake
        * 4. Worker enters wait with OLD wake_val -> misses the wake
        * 
        * By reading wake_val after the queue check, we ensure:
        * - If spawner pushed before our check: we see the fiber
        * - If spawner pushed after our check: wake_val changed, wait returns immediately */
        while (atomic_load_explicit(&g_sched.running, memory_order_relaxed)) {
            /* Check if there's runnable work */
            /* Drain any expired timer-backed waits before checking queues. */
            if (g_sched.run_queue) {
                sq_drain();
                tpq_drain();
            }
            
            int has_local = lq_peek(my_queue);
            int has_inbox = iq_peek(&g_sched.inbox_queues[worker_id]);
            int has_global = sched_global_peek_any();
            int startup_peer_inbox = 0;
            if (!has_local && !has_inbox && !has_global && g_sched.num_workers > 1) {
                /* If wake lands on a non-target sleeper, probe peer inboxes once
                * before sleeping again so inbox work cannot strand. */
                size_t start = (size_t)(worker_id + 1) % g_sched.num_workers;
                for (size_t j = 0; j < g_sched.num_workers - 1; j++) {
                    size_t victim = (start + j) % g_sched.num_workers;
                    if ((int)victim == worker_id) continue;
                    if (iq_peek(&g_sched.inbox_queues[victim])) {
                        startup_peer_inbox = 1;
                        break;
                    }
                }
            }
            if (has_local || has_inbox || has_global || startup_peer_inbox) {
                break;
            }
            /* Read wake_val from this worker's own primitive AFTER checking queues.
            * Workers sleep on per-worker prims so wakes are targeted: inbox pushes
            * wake exactly the right worker, eliminating thundering herd. */
            atomic_thread_fence(memory_order_acquire);
            wake_primitive* my_wake = g_sched.worker_wake_prims
                                    ? &g_sched.worker_wake_prims[worker_id]
                                    : &g_sched.wake_prim;
            uint32_t wake_val = atomic_load_explicit(&my_wake->value, memory_order_acquire);
            
            /* Double-check queues after reading wake_val to close the race window */
            if (lq_peek(my_queue) || iq_peek(&g_sched.inbox_queues[worker_id]) ||
                sched_global_peek_any()) {
                break;
            }
            
            /* Use timed wait to periodically check for deadlock */
            uint64_t wait_start_ns = 0;
            cc_sched_wait_stat_inc(&g_cc_sched_wait_stats.sleep_wait_calls);
            wake_primitive_wait_timeout(my_wake, wake_val, 500);
            cc__fiber_check_deadlock();
        }
        atomic_fetch_sub_explicit(&g_sched.sleeping, 1, memory_order_relaxed);
        cc_v3_worker_lifecycle_set(worker_id, CC_WL_ACTIVE, "sleep_exit");
        cc_sched_wait_stat_inc(&g_cc_sched_wait_stats.sleep_exits);
        if (g_sched.num_workers > 1) {
            size_t start = (size_t)(worker_id + 1) % g_sched.num_workers;
            for (size_t j = 0; j < g_sched.num_workers - 1; j++) {
                size_t victim = (start + j) % g_sched.num_workers;
                if ((int)victim == worker_id) continue;
                runnable_ref peer = iq_pop(&g_sched.inbox_queues[victim]);
                if (!peer.fiber) continue;
                if (cc__task_should_stay_on_target_inbox(peer.fiber, (int)victim) &&
                    cc__restore_inbox_task(victim, peer.fiber)) {
                    continue;
                }
                worker_run_fiber_accounted(peer);
                goto next_iteration;
            }
        }
        next_iteration:;
    }
    
    if (lifecycle_track) cc_v3_worker_lifecycle_set(worker_id, CC_WL_DRAINING, "shutdown_begin");
    if (lifecycle_track) cc_v3_worker_lifecycle_set(worker_id, CC_WL_DEAD, "shutdown_done");
    tls_sched_worker_ctx = 0;
    tls_worker_id = -1;
    return NULL;
}

/* ============================================================================
* Public API
* ============================================================================ */

static void cc_fiber_atexit_stats(void) {
    if (atomic_load(&g_initialized) != 2) return;
    cc_sched_wait_stats_dump();
    cc_sched_io_wake_stats_dump();
}

int cc_fiber_sched_init(size_t num_workers) {
    int state = atomic_load_explicit(&g_initialized, memory_order_acquire);
    if (state == 2) return 0;
    
    int expected = 0;
    if (!atomic_compare_exchange_strong_explicit(&g_initialized, &expected, 1,
                                                memory_order_acq_rel,
                                                memory_order_acquire)) {
        while (atomic_load_explicit(&g_initialized, memory_order_acquire) != 2) {
            sched_yield();
        }
        return 0;
    }
    
    /* Register atexit handler for stats */
    static int atexit_registered = 0;
    if (!atexit_registered) {
        atexit(cc_fiber_atexit_stats);
        atexit_registered = 1;
    }
    
    if (num_workers == 0) {
        /* Check user-requested count first (via cc_sched_set_num_workers) */
        num_workers = atomic_load(&g_requested_workers);
        if (num_workers == 0) {
            const char* env = getenv("CC_WORKERS");
            if (env) {
                num_workers = (size_t)strtoul(env, NULL, 10);
            }
        }
        if (num_workers == 0) {
            #if CC_FIBER_WORKERS > 0
            num_workers = CC_FIBER_WORKERS;
            #else
            long n = sysconf(_SC_NPROCESSORS_ONLN);
            num_workers = n > 0 ? (size_t)n : 4;
            #endif
        }
    }
    if (num_workers > MAX_WORKERS) num_workers = MAX_WORKERS;
    
    memset(&g_sched, 0, sizeof(g_sched));
    g_sched.num_workers = num_workers;
    atomic_store(&g_sched.running, 1);
    atomic_store_explicit(&g_sched.startup_phase, CC_STARTUP_PHASE, memory_order_relaxed);
    atomic_store_explicit(&g_sched.startup_run_count, 0, memory_order_relaxed);
    atomic_store_explicit(&g_sched.startup_target_runs, num_workers * 8, memory_order_relaxed);
    g_sched.run_queue_count = 1;
    if (cc_sharded_runq_enabled() && num_workers > 1) {
        size_t shards = num_workers * 2;
        if (shards > (MAX_WORKERS * 2)) shards = MAX_WORKERS * 2;
        if (shards < 2) shards = 2;
        g_sched.run_queue_count = shards;
    }
    g_sched.run_queue = (fiber_queue*)calloc(g_sched.run_queue_count, sizeof(fiber_queue));
    for (size_t i = 0; i < g_sched.run_queue_count; i++) {
        fq_init(&g_sched.run_queue[i]);
    }
    sq_init();
    g_sched.local_queues = (local_queue*)calloc(num_workers, sizeof(local_queue));
    g_sched.inbox_queues = (inbox_queue*)calloc(num_workers, sizeof(inbox_queue));
    g_sched.workers = (pthread_t*)calloc(num_workers, sizeof(pthread_t));
    g_sched.worker_parked = (_Atomic size_t*)calloc(num_workers, sizeof(_Atomic size_t));
    g_sched.worker_heartbeat = (typeof(g_sched.worker_heartbeat))calloc(
        num_workers, sizeof(*g_sched.worker_heartbeat));
    g_sched.worker_lifecycle = (_Atomic unsigned char*)calloc(num_workers, sizeof(_Atomic unsigned char));
    g_sched.worker_first_sleep_seen = (_Atomic unsigned char*)calloc(num_workers, sizeof(_Atomic unsigned char));
    g_sched.worker_boot_tsc = (_Atomic uint64_t*)calloc(num_workers, sizeof(_Atomic uint64_t));
    g_sched.worker_wake_prims = (wake_primitive*)calloc(num_workers, sizeof(wake_primitive));
    if (!g_sched.run_queue || !g_sched.local_queues || !g_sched.inbox_queues ||
        !g_sched.workers || !g_sched.worker_parked || !g_sched.worker_heartbeat ||
        !g_sched.worker_lifecycle || !g_sched.worker_first_sleep_seen || !g_sched.worker_boot_tsc ||
        !g_sched.worker_wake_prims) {
        if (g_sched.run_queue) {
            for (size_t i = 0; i < g_sched.run_queue_count; i++) {
                pthread_mutex_destroy(&g_sched.run_queue[i].overflow_mu);
            }
        }
        free(g_sched.run_queue);
        free(g_sched.local_queues);
        free(g_sched.inbox_queues);
        free(g_sched.workers);
        free(g_sched.worker_parked);
        free(g_sched.worker_heartbeat);
        free(g_sched.worker_lifecycle);
        free(g_sched.worker_first_sleep_seen);
        free(g_sched.worker_boot_tsc);
        free(g_sched.worker_wake_prims);
        fprintf(stderr, "[cc] fiber scheduler init failed: out of memory\n");
        abort();
    }
    wake_primitive_init(&g_sched.wake_prim);
    for (size_t i = 0; i < num_workers; i++) {
        wake_primitive_init(&g_sched.worker_wake_prims[i]);
    }
    atomic_store_explicit(&g_sched.lifecycle_illegal_transitions, 0, memory_order_relaxed);
    atomic_store_explicit(&g_sched.pressure, 0, memory_order_relaxed);
    for (size_t i = 0; i < num_workers; i++) {
        atomic_store_explicit(&g_sched.worker_lifecycle[i], (unsigned char)CC_WL_DEAD, memory_order_relaxed);
    }
    
    for (size_t i = 0; i < num_workers; i++) {
        pthread_create(&g_sched.workers[i], NULL, worker_main, (void*)i);
    }

    const char* sysmon_env = getenv("CC_SYSMON");
    int enable_sysmon = !(sysmon_env && sysmon_env[0] == '0');
    if (num_workers > 1 && enable_sysmon) {
        atomic_store_explicit(&g_sched.sysmon_running, 1, memory_order_release);
        g_sched.sysmon_started = (pthread_create(&g_sched.sysmon_thread, NULL, sysmon_main, NULL) == 0);
        if (!g_sched.sysmon_started)
            atomic_store_explicit(&g_sched.sysmon_running, 0, memory_order_release);
    } else {
        atomic_store_explicit(&g_sched.sysmon_running, 0, memory_order_release);
        g_sched.sysmon_started = 0;
    }
    
    atomic_store_explicit(&g_initialized, 2, memory_order_release);
    return 0;
}

void cc_fiber_sched_shutdown(void) {
    if (atomic_load_explicit(&g_initialized, memory_order_acquire) != 2) return;

    /* Wait for all in-flight fibers to complete before tearing down.
    * The spec says "drain until active_tasks == 0".  In practice this
    * loop rarely iterates because nursery join already waits for
    * children, but it guards against shutdown racing with stragglers. */
    {
        int drain_iters = 0;
        while (atomic_load_explicit(&g_sched.pending, memory_order_acquire) > 0) {
            usleep(1000);  /* 1ms poll */
            if (++drain_iters > 5000) {  /* 5 second hard timeout */
                fprintf(stderr, "[shutdown] warning: %zu fibers still pending after 5s, proceeding\n",
                        (size_t)atomic_load_explicit(&g_sched.pending, memory_order_relaxed));
                break;
            }
        }
    }

    atomic_store_explicit(&g_sched.running, 0, memory_order_release);
    atomic_store_explicit(&g_sched.sysmon_running, 0, memory_order_release);
    wake_primitive_wake_all(&g_sched.wake_prim);
    if (g_sched.worker_wake_prims) {
        for (size_t i = 0; i < g_sched.num_workers; i++) {
            wake_primitive_wake_one(&g_sched.worker_wake_prims[i]);
        }
    }

    if (g_sched.sysmon_started) {
        pthread_join(g_sched.sysmon_thread, NULL);
        g_sched.sysmon_started = 0;
    }
    for (size_t i = 0; i < g_sched.num_workers; i++) {
        pthread_join(g_sched.workers[i], NULL);
    }
    
    /* Drain queues and free pending tasks to avoid leaks */
    if (g_sched.run_queue) {
        runnable_ref ref;
        while ((ref = sched_global_pop_any()).fiber) {
            fiber_free(ref.fiber);
        }
    }
    if (g_sched.local_queues) {
        for (size_t i = 0; i < g_sched.num_workers; i++) {
            runnable_ref ref;
            while ((ref = lq_pop(&g_sched.local_queues[i])).fiber) {
                fiber_free(ref.fiber);
            }
        }
    }
    if (g_sched.inbox_queues) {
        for (size_t i = 0; i < g_sched.num_workers; i++) {
            runnable_ref ref;
            while ((ref = iq_pop(&g_sched.inbox_queues[i])).fiber) {
                fiber_free(ref.fiber);
            }
        }
    }
    
    free(g_sched.workers);
    free(g_sched.local_queues);
    free(g_sched.inbox_queues);
    free(g_sched.worker_parked);
    free(g_sched.worker_heartbeat);
    free(g_sched.worker_lifecycle);
    free(g_sched.worker_first_sleep_seen);
    free(g_sched.worker_boot_tsc);
    sq_destroy();
    for (size_t i = 0; i < g_sched.run_queue_count; i++) {
        pthread_mutex_destroy(&g_sched.run_queue[i].overflow_mu);
    }
    free(g_sched.run_queue);
    g_sched.workers = NULL;
    g_sched.local_queues = NULL;
    g_sched.inbox_queues = NULL;
    g_sched.worker_parked = NULL;
    g_sched.worker_heartbeat = NULL;
    g_sched.worker_lifecycle = NULL;
    g_sched.worker_first_sleep_seen = NULL;
    g_sched.worker_boot_tsc = NULL;
    g_sched.run_queue = NULL;
    g_sched.run_queue_count = 0;

    /* Free pooled fibers (including their coros and join_cvs) */
    fiber_task* f = atomic_load(&g_sched.free_list);
    while (f) {
        fiber_task* next = f->next;
        fiber_destroy(f);
        f = next;
    }
    
    wake_primitive_destroy(&g_sched.wake_prim);
    atomic_store(&g_initialized, 0);
}

fiber_task* cc_fiber_spawn(void* (*fn)(void*), void* arg) {
    uint64_t t0 = 0, t1, t2, t3, t4;
    
    
    if (atomic_load_explicit(&g_initialized, memory_order_acquire) != 2) {
        cc_fiber_sched_init(0);
    }
    
    fiber_task* f = fiber_alloc();
    if (!f) return NULL;
    
    
    f->fn = fn;
    f->arg = arg;
    f->saved_nursery = cc__tls_spawn_nursery_override;
    f->admission_nursery = cc__tls_spawn_nursery_override;
    
    /* Reuse existing coro if available (pooling), otherwise create new */
    int reused = 0;
    if (f->coro) {
        mco_state st = f->coro->state;  /* Inline mco_status for speed */
        
        /* If coro is still MCO_RUNNING, the previous fiber_entry set done=1
        * but mco_resume hasn't returned yet. Spin-wait for it to exit.
        * This is a very short window - just waiting for context switch. */
        if (st == MCO_RUNNING) {
            for (int spin = 0; spin < 10000; spin++) {
                cpu_pause();
                st = f->coro->state;
                if (st != MCO_RUNNING) break;
            }
            /* After spinning, if still running, yield and retry */
            while (st == MCO_RUNNING) {
                sched_yield();
                st = f->coro->state;
            }
        }
        
        if (st == MCO_DEAD || st == MCO_SUSPENDED) {
            /* MCO_DEAD: completed coro from pool, needs context reset
            * MCO_SUSPENDED: pre-warmed coro, already has valid context */
            /* Fast path: Reset the coroutine context directly without full mco_init.
            * mco_init does expensive memsets that aren't needed for reuse.
            * We only need to reset the context registers and update metadata. */
            mco_coro* co = f->coro;
            
            /* Reset context: zero entire ctxbuf first to clear stale callee-saved
            * registers from the previous run (fp, d8-d15, etc.), then set the
            * fields needed for a fresh start. Without zeroing, stale x29 (frame
            * pointer) or SIMD registers can cause crashes on reused coroutines. */
            _mco_ctxbuf* ctx = &((_mco_context*)co->context)->ctx;
            memset(ctx, 0, sizeof(_mco_ctxbuf));
            #if defined(__aarch64__) || defined(__arm64__)
            ctx->x[0] = (void*)(co);
            ctx->x[1] = (void*)(_mco_main);
            ctx->x[2] = (void*)(0xdeaddeaddeaddead);
            ctx->sp = (void*)((size_t)co->stack_base + co->stack_size);
            ctx->lr = (void*)(_mco_wrap_main);
            #elif defined(__x86_64__) || defined(_M_X64)
            ctx->rip = (void*)(_mco_wrap_main);
            ctx->rsp = (void*)((size_t)co->stack_base + co->stack_size - 128 - sizeof(size_t));
            ((void**)ctx->rsp)[0] = (void*)(0xdeaddeaddeaddead);
            ctx->r12 = (void*)(_mco_main);
            ctx->r13 = (void*)(co);
            #else
            /* Fallback: use full mco_init for other architectures */
            mco_desc desc = mco_desc_init(fiber_entry, CC_FIBER_STACK_SIZE);
            desc.user_data = f;
            if (mco_init(f->coro, &desc) != MCO_SUCCESS) {
                mco_destroy(f->coro);
                f->coro = NULL;
            }
            #endif
            
            if (f->coro) {
                /* Update metadata */
                co->state = MCO_SUSPENDED;
                co->func = fiber_entry;
                co->user_data = f;
                
                /* TSan: Mark stack memory as reused when resetting pooled fiber.
                * This tells TSan that the stack memory is being reused for a new
                * execution context, clearing any previous TSan state for this memory.
                * Without this, TSan sees writes to the same stack address from different
                * threads and incorrectly flags it as a data race. */
                if (co->stack_base && co->stack_size > 0) {
                    TSAN_WRITE_RANGE(co->stack_base, co->stack_size);
                }
                
                reused = 1;
            }
        } else {
            /* Coro exists but not DEAD - destroy and recreate */
            mco_destroy(f->coro);
            f->coro = NULL;
        }
    }
    
    if (!f->coro) {
        /* Create new coroutine */
        mco_desc desc = mco_desc_init(fiber_entry, CC_FIBER_STACK_SIZE);
        desc.user_data = f;
        desc.alloc_cb = cc_guarded_alloc;
        desc.dealloc_cb = cc_guarded_dealloc;
        mco_result res = mco_create(&f->coro, &desc);
        if (res != MCO_SUCCESS) {
            fiber_free(f);
            return NULL;
        }
    }

    /* Publish runnable state only after the coroutine is fully initialized.
     * Otherwise a stale queue entry from a previous incarnation can observe
     * CTRL_QUEUED and resume a coroutine that is still DEAD / half-reset. */
    f->enqueue_src = 2;
    f->enqueue_ctrl = 0; /* CTRL_IDLE */
    int64_t expected_state = CTRL_IDLE;
    if (!atomic_compare_exchange_strong_explicit(&f->control, &expected_state, CTRL_QUEUED,
                                                memory_order_release,
                                                memory_order_relaxed)) {
        fiber_free(f);
        return NULL;
    }

    /* TSan release: establish synchronization with acquire in fiber_entry */
    TSAN_RELEASE(arg);
    
    
    /* Workers use local queue for self-spawns (fast path).
    * Non-workers use global queue to avoid inbox race conditions:
    * - Inbox targeting can miss if target worker already checked its inbox
    * - Global queue is checked by all workers, reducing miss probability
    * Workers spawning to other workers still use inbox for locality. */
    int pushed = 0;
    int pushed_global = 0;
    int global_edge = 0;
    int non_global_edge = 0;
    int pushed_local = 0;
    int nonworker_nonglobal_publish = 0;
    int nonworker_global_publish = 0;
    int nonworker_target_worker = -1;
    int publish_target_worker = -1;
    int startup_force_global = 0;
    int spawn_publish_route = (int)SPAWN_ROUTE_NONE;
    int spawn_publish_active0 = (atomic_load_explicit(&g_sched.active, memory_order_relaxed) == 0);
    int global_preferred = -1;
    int nonworker_ready_snapshot = -1;
    if (tls_sched_worker_ctx && g_sched.num_workers > 0) {
        /* Scheduler thread context (base + replacement workers).
        * Base workers prefer local queue; replacement workers route directly to inbox. */
        if (tls_worker_id >= 0) {
            size_t self = (size_t)tls_worker_id;
            /* Bulk-spawn distribution: keep a few fibers on the local queue so
            * small P/C pairs plus nursery-internal fibers stay co-located
            * (same worker = cooperative switch, no cross-thread wake overhead).
            * Once the local queue has CC_SPAWN_LOCAL_DEPTH_LIMIT+ waiting
            * fibers the spawning worker already has a full pipeline; subsequent
            * fibers are routed to other workers' inboxes for parallel execution.
            * 2-fiber pipelines and the nursery framework always stay below the
            * threshold; bulk spawns (pigz 16-task pattern) distribute from the
            * (N+1)th fiber.
            *
            * Idle-workers override: if no workers are spinning (they're all
            * sleeping or active) AND the local queue already has ≥2 items, the
            * sleeping workers won't find this work via their own queue checks —
            * they only check their own local+inbox.  Bypass the local queue and
            * route to inbox so the wake mechanism delivers work immediately.
            * Threshold ≥2 preserves P/C co-location: a 2-fiber P/C pair spawns
            * exactly 2 items (depth 0→1, then 1→2) and never triggers this path. */
            size_t local_depth_now = lq_depth(&g_sched.local_queues[self]);
            size_t spinning_now = atomic_load_explicit(&g_sched.spinning, memory_order_relaxed);
            int idle_override = (spinning_now == 0 && local_depth_now >= 2);
            if (!idle_override &&
                    local_depth_now < CC_SPAWN_LOCAL_DEPTH_LIMIT &&
                    lq_push(&g_sched.local_queues[self], f) == 0) {
                pushed = 1;
                pushed_local = 1;
                publish_target_worker = (int)self;
                spawn_publish_route = (int)SPAWN_ROUTE_LOCAL;
            } else {
                /* Saturation bypass: when every worker is actively running a fiber
                * (spinning==0, sleeping==0) route to the shared global pool instead
                * of pre-assigning to a specific inbox.  Any worker that finishes
                * will find this work at P3 immediately — identical to how CCExec's
                * shared queue works — eliminating the "block N and N+8 stuck behind
                * each other on the same worker" problem that causes high tail
                * queue_wait.  Guard: only fires after saturation to preserve the
                * fast-inbox path during startup when workers are still spinning. */
                size_t sleeping_now = atomic_load_explicit(&g_sched.sleeping, memory_order_relaxed);
                int saturated = (spinning_now == 0 && sleeping_now == 0 && g_sched.run_queue != NULL);
                if (saturated) {
                    int was_empty = sched_global_push(f, -1);
                    pushed = 1;
                    pushed_global = 1;
                    global_edge = was_empty;
                    spawn_publish_route = (int)SPAWN_ROUTE_GLOBAL;
                } else {
                    /* Normal path: prefer a spinning or sleeping worker.
                    * A spinning worker checks its inbox every ~1µs in the spin loop;
                    * a sleeping worker wakes on the edge signal sent below.  Either is
                    * dramatically better than landing on a worker currently running a
                    * 4ms compression block, where the block sits in the inbox waiting
                    * the full duration — exactly the source of our qw_max spikes. */
                    size_t target = atomic_fetch_add_explicit(&g_spawn_counter, 1, memory_order_relaxed) % g_sched.num_workers;
                    if (target == self && g_sched.num_workers > 1)
                        target = (target + 1) % g_sched.num_workers;

                    if ((spinning_now > 0 || sleeping_now > 0) && g_sched.worker_lifecycle && g_sched.num_workers > 1) {
                        /* Prefer an idle worker whose inbox is also empty: this
                        * prevents inbox pile-up where worker W already has tasks
                        * {A,B} in its inbox while worker X is idle with an empty
                        * inbox.  Two-tier scan: first idle-with-empty-inbox wins;
                        * fall back to first idle-with-backlog if none found. */
                        size_t idle_with_backlog = (size_t)-1;
                        for (size_t scan = 0; scan < g_sched.num_workers; scan++) {
                            size_t cand = (target + scan) % g_sched.num_workers;
                            if ((int)cand == tls_worker_id) continue;
                            cc_worker_lifecycle lc = (cc_worker_lifecycle)atomic_load_explicit(
                                &g_sched.worker_lifecycle[cand], memory_order_relaxed);
                            if (lc == CC_WL_IDLE_SPIN || lc == CC_WL_SLEEP) {
                                if (!iq_peek(&g_sched.inbox_queues[cand])) {
                                    target = cand;  /* idle + empty inbox: ideal */
                                    idle_with_backlog = (size_t)-1;  /* cancel fallback */
                                    break;
                                } else if (idle_with_backlog == (size_t)-1) {
                                    idle_with_backlog = cand;  /* remember first idle-but-busy */
                                }
                            }
                        }
                        if (idle_with_backlog != (size_t)-1) target = idle_with_backlog;
                    } else if (g_sched.worker_heartbeat && g_sched.num_workers > 1) {
                        /* Fallback: skip orphaned (truly stuck) workers */
                        uint64_t hb0 = atomic_load_explicit(&g_sched.worker_heartbeat[target].heartbeat, memory_order_relaxed);
                        uint64_t now_cyc = rdtsc();
                        if (hb0 != 0 && (now_cyc - hb0) >= ORPHAN_THRESHOLD_CYCLES) {
                            for (size_t scan = 1; scan < g_sched.num_workers; scan++) {
                                size_t cand = (target + scan) % g_sched.num_workers;
                                if ((int)cand == tls_worker_id) continue;
                                uint64_t hb = atomic_load_explicit(&g_sched.worker_heartbeat[cand].heartbeat, memory_order_relaxed);
                                if (hb != 0 && (now_cyc - hb) < ORPHAN_THRESHOLD_CYCLES) { target = cand; break; }
                            }
                        }
                    }
                    /* Inbox-to-global promotion: if the selected target already
                    * has work queued in its inbox, the worker is busy and cannot
                    * start this fiber immediately regardless of its lifecycle state.
                    * Routing to a backlogged inbox means the task waits behind
                    * however many items are already there (~0.84ms avg at cap=16).
                    * Instead promote to the shared global queue: whichever worker
                    * finishes first picks it up at P3 immediately — identical to
                    * how CCExec's shared queue eliminates inbox backlog latency.
                    * Only applies when a run_queue exists (post-init); during
                    * startup workers have empty inboxes so this path rarely fires
                    * and inbox routing remains the fast path. */
                    int target_has_backlog = (g_sched.run_queue != NULL &&
                                            iq_peek(&g_sched.inbox_queues[target]));
                    if (target_has_backlog) {
                        int was_empty = sched_global_push(f, -1);
                        pushed = 1;
                        pushed_global = 1;
                        global_edge = was_empty;
                        spawn_publish_route = (int)SPAWN_ROUTE_GLOBAL;
                    } else {
                        int inbox_edge = 0;
                        for (int attempt = 0; attempt < 8 && !pushed; attempt++) {
                            pushed = (iq_push_with_edge(&g_sched.inbox_queues[target], f, &inbox_edge) == 0);
                            if (!pushed) cpu_pause();
                        }
                        if (pushed) {
                            pushed = 1;
                            non_global_edge = inbox_edge;
                            publish_target_worker = (int)target;
                            spawn_publish_route = (int)SPAWN_ROUTE_INBOX;
                        }
                    }
                }
            }
        } else {
            /* Replacement worker: RR but skip a sleeping target if a fresher peer exists. */
            size_t target = atomic_fetch_add_explicit(&g_spawn_counter, 1, memory_order_relaxed) % g_sched.num_workers;
            if (g_sched.worker_heartbeat && g_sched.num_workers > 1) {
                uint64_t hb0 = atomic_load_explicit(&g_sched.worker_heartbeat[target].heartbeat, memory_order_relaxed);
                uint64_t now_cyc = rdtsc();
                if (hb0 != 0 && (now_cyc - hb0) >= ORPHAN_THRESHOLD_CYCLES) {
                    for (size_t scan = 1; scan < g_sched.num_workers; scan++) {
                        size_t cand = (target + scan) % g_sched.num_workers;
                        uint64_t hb = atomic_load_explicit(&g_sched.worker_heartbeat[cand].heartbeat, memory_order_relaxed);
                        if (hb != 0 && (now_cyc - hb) < ORPHAN_THRESHOLD_CYCLES) { target = cand; break; }
                    }
                }
            }
            int inbox_edge = 0;
            for (int attempt = 0; attempt < 8 && !pushed; attempt++) {
                pushed = (iq_push_with_edge(&g_sched.inbox_queues[target], f, &inbox_edge) == 0);
                if (!pushed) cpu_pause();
            }
            if (pushed) {
                pushed = 1;
                non_global_edge = inbox_edge;
                publish_target_worker = (int)target;
                spawn_publish_route = (int)SPAWN_ROUTE_INBOX;
            }
        }
    } else if (g_sched.num_workers > 0) {
        /* Non-worker spawn: preserve small nursery sibling groups in pairs,
        * otherwise fall back to the usual RR target selection. */
        size_t target = cc__spawn_pick_nonworker_target();
        /* More aggressive direct-inbox mode for non-worker spawns. */
        int nw_ready = pool_ready_for_nonworker_inbox_publish();
        int startup_phase = sched_in_startup_phase();
        size_t active_snapshot = atomic_load_explicit(&g_sched.active, memory_order_relaxed);
        size_t pending_snapshot = atomic_load_explicit(&g_sched.pending, memory_order_relaxed);
        if (!startup_phase) {
            atomic_store_explicit(&g_spawn_nw_startup_admit_wakev, UINT32_MAX, memory_order_relaxed);
            atomic_store_explicit(&g_spawn_nw_startup_admit_remaining, 0, memory_order_relaxed);
        } else if (nw_ready) {
            uint32_t wakev_now = atomic_load_explicit(&g_sched.wake_prim.value, memory_order_relaxed);
            uint32_t admit_seen =
                atomic_load_explicit(&g_spawn_nw_startup_admit_wakev, memory_order_relaxed);
            int reset_wave = 0;
            if (pending_snapshot == 0) {
                reset_wave = 1;
            } else if (admit_seen != wakev_now &&
                    atomic_compare_exchange_strong_explicit(&g_spawn_nw_startup_admit_wakev,
                                                            &admit_seen,
                                                            wakev_now,
                                                            memory_order_relaxed,
                                                            memory_order_relaxed)) {
                reset_wave = 1;
            }
            if (reset_wave) {
                atomic_store_explicit(&g_spawn_nw_startup_admit_wakev, wakev_now, memory_order_relaxed);
                atomic_store_explicit(&g_spawn_nw_startup_admit_remaining, g_sched.num_workers, memory_order_relaxed);
            }
            size_t admit_remaining =
                atomic_load_explicit(&g_spawn_nw_startup_admit_remaining, memory_order_relaxed);
            int admitted = 0;
            while (admit_remaining > 0) {
                if (atomic_compare_exchange_weak_explicit(&g_spawn_nw_startup_admit_remaining,
                                                        &admit_remaining,
                                                        admit_remaining - 1,
                                                        memory_order_relaxed,
                                                        memory_order_relaxed)) {
                    admitted = 1;
                    break;
                }
            }
            if (!admitted) {
                startup_force_global = 1;
                nw_ready = 0;
            }
        }
        nonworker_ready_snapshot = nw_ready;
        if (nw_ready) {
            int inbox_edge = 0;
            for (int attempt = 0; attempt < 4 && !pushed; attempt++) {
                pushed = (iq_push_with_edge(&g_sched.inbox_queues[target], f, &inbox_edge) == 0);
                if (!pushed) cpu_pause();
            }
            if (pushed) {
                non_global_edge = inbox_edge;
                nonworker_nonglobal_publish = 1;
                nonworker_target_worker = (int)target;
                publish_target_worker = (int)target;
                spawn_publish_route = (int)SPAWN_ROUTE_INBOX;
            }
        }
        if (!pushed) {
            /* For startup/small-burst non-worker publishes (nw_ready=false),
            * keep fallback on a shared global shard to preserve enqueue order
            * and avoid scheduling all observer-like tasks ahead of the target.
            * Once nw_ready=true, keep affinity shard preference. */
            if (startup_force_global) {
                global_preferred = 0;
            } else {
                global_preferred = nw_ready ? (int)target : 0;
            }
        }
    }
    
    /* Distribution: locality-first — spawns go to the current worker's local
    * queue when possible.  The spec suggests round-robin (SHOULD), but
    * locality-first gives better cache behavior and work stealing provides
    * equivalent load balance.  See spec/scheduler_v2.md §Run queues.
    *
    * Non-worker spawns or local queue full: fall through to global queue. */
    if (!pushed) {
        if (!tls_sched_worker_ctx && nonworker_ready_snapshot == 0) {
            /* Reassert startup fairness path if future edits touched fallback selection. */
            global_preferred = 0;
        }
        global_edge = sched_global_push(f, global_preferred);
        pushed_global = 1;
        spawn_publish_route = (int)SPAWN_ROUTE_GLOBAL;
    }
    

    
    atomic_fetch_add_explicit(&g_sched.pending, 1, memory_order_relaxed);
    cc_sched_pressure_add(1);
    
    /* Wake a sleeping worker if needed.
    * Skip if we pushed to our own local queue (we'll run it ourselves).
    * Use the conditional version: if workers are spinning, they'll find
    * the work via queue checks without a costly kernel wake syscall. */
    if (!pushed_local) {
        if (pushed_global) {
            if (global_edge && pool_idle_for_global_edge_wake()) {
                wake_one_if_sleeping(CC_WAKE_REASON_SPAWN_GLOBAL_EDGE);
            } else if (!global_edge) {
                /* Non-edge global push: happens when we promoted a backlogged-inbox
                * spawn to global (target_has_backlog path).  Active workers will
                * pick it up at P3 naturally, but sleeping workers need a nudge.
                * Only wake if there are actual sleepers — avoids a syscall when
                * all workers are busy (the common steady-state case). */
                size_t sl = atomic_load_explicit(&g_sched.sleeping, memory_order_relaxed);
                if (sl > 0) {
                    wake_one_if_sleeping(CC_WAKE_REASON_SPAWN_GLOBAL_EDGE);
                }
            } else if (nonworker_global_publish && pool_strict_idle_for_nonglobal_wake()) {
                /* If non-worker global publish lands on a non-edge, strict-idle
                * still needs a nudge to avoid occasional all-sleep stalls. */
                wake_one_if_sleeping(CC_WAKE_REASON_SPAWN_GLOBAL_EDGE);
            }
        } else {
            if (nonworker_nonglobal_publish) {
                /* Run-phase policy: non-worker non-global wake requires target
                * sleeping when active>0; strict-idle remains the safety backstop. */
                int strict_idle = pool_strict_idle_for_nonglobal_wake();
                size_t active_now = atomic_load_explicit(&g_sched.active, memory_order_relaxed);
                int target_sleep = 0;
                if (nonworker_target_worker >= 0 && g_sched.worker_lifecycle) {
                    unsigned char st = atomic_load_explicit(
                        &g_sched.worker_lifecycle[nonworker_target_worker],
                        memory_order_relaxed
                    );
                    target_sleep = ((cc_worker_lifecycle)st == CC_WL_SLEEP);
                }
                int allow_edge = non_global_edge && (active_now == 0 || target_sleep);
                if (allow_edge || strict_idle) {
                    /* Wake the specific target worker, not just any sleeper */
                    wake_target_worker_if_sleeping(nonworker_target_worker);
                }
            } else {
                int strict_idle = pool_strict_idle_for_nonglobal_wake();
                int replacement_origin = (tls_sched_worker_ctx && tls_worker_id < 0);
                int replacement_target_sleep = 0;
                if (replacement_origin && publish_target_worker >= 0 && g_sched.worker_lifecycle) {
                    unsigned char st = atomic_load_explicit(
                        &g_sched.worker_lifecycle[publish_target_worker],
                        memory_order_relaxed
                    );
                    replacement_target_sleep = ((cc_worker_lifecycle)st == CC_WL_SLEEP);
                }
                int allow_nonglobal_wake =
                    replacement_origin ? (strict_idle || replacement_target_sleep)
                                    : (non_global_edge || strict_idle);
                if (allow_nonglobal_wake) {
                    /* Wake the specific target worker, not just any sleeper */
                    wake_target_worker_if_sleeping(publish_target_worker);
                }
            }
        }
    }
    
    
    /* Track reuse stats (only when CC_FIBER_STATS is set) */
    static int stats_enabled = 0;
    if (stats_enabled) {
        if (reused) {
            atomic_fetch_add_explicit(&g_sched.coro_reused, 1, memory_order_relaxed);
        } else {
            atomic_fetch_add_explicit(&g_sched.coro_created, 1, memory_order_relaxed);
        }
    }
    
    /* Debug: track fiber in all-fibers list and log lifecycle event */
    if (!reused) {
        all_fibers_track(f);
    }
    atomic_fetch_add_explicit(&g_fibers_spawned, 1, memory_order_relaxed);
    
    return f;
}

/* Helper: wait for fiber to fully finish executing.
* With the unified control word, CTRL_DONE is set under the join_lock
* together with done=1, so once we see done=1, control should be CTRL_DONE
* (with only a brief memory ordering delay possible).
*
* No separate running_lock wait is needed — when fiber_resume returns on the
* worker, the worker's trampoline immediately stores CTRL_DONE (under the
* join_lock), so the control word is already CTRL_DONE by the time the worker
* finishes with the fiber. */
static inline void wait_for_fiber_done_state(fiber_task* f) {
    if (!f) return;
    
    /* Wait for control=CTRL_DONE.  The gap between done=1 (set inside the
    * coroutine by fiber_entry) and CTRL_DONE (set by the worker trampoline
    * via CAS OWNED(wid)→CTRL_DONE after mco_resume returns) is extremely
    * short — a few instructions on the SAME worker thread.  But the joiner
    * may be on a different core and observe done=1 before CTRL_DONE.
    *
    * We MUST wait: if we return early and the caller frees the fiber, the
    * owning worker's trampoline could race with the new incarnation.  Even
    * with the CAS-based trampoline, cc_fiber_spawn's memset of the coroutine
    * context could corrupt the old trampoline's stack if it hasn't finished
    * the context switch yet.
    *
    * Strategy: tight cpu_pause spin (covers >99.9% of cases), then fall back
    * to sched_yield() which is a safe OS-level yield hint (not a fiber yield). */
    for (int i = 0; i < 10000; i++) {
        if (atomic_load_explicit(&f->control, memory_order_acquire) == CTRL_DONE) {
            return;
        }
        cpu_pause();
    }
    /* Fallback: sched_yield loop.  This is safe in both fiber and thread context
    * because sched_yield() is just an OS thread hint — it does NOT call
    * mco_yield and does NOT cause reentrancy.  The gap should be at most a few
    * instructions, so this loop is extremely unlikely to iterate more than once. */
    while (atomic_load_explicit(&f->control, memory_order_acquire) != CTRL_DONE) {
        sched_yield();
    }
}

int cc_fiber_join(fiber_task* f, void** out_result) {
    if (!f) return -1;
    
    /* Get current fiber context (if any) - affects whether we park or use condvar */
    fiber_task* current = tls_current_fiber;
    
    /* Fast path - already done */
    if (atomic_load_explicit(&f->done, memory_order_acquire)) {
        wait_for_fiber_done_state(f);
        if (out_result) *out_result = f->result;
        return 0;
    }
    
    /* Spin phases - ONLY for thread context.
    * 
    * In fiber context, spinning blocks the worker thread and prevents it from
    * running other fibers - including the one we're waiting for! This causes
    * priority inversion: if the target fiber is in our worker's queue, it can
    * never run because we're blocking the worker.
    * 
    * In thread context, spinning is safe because we're not blocking any fiber
    * scheduler worker - we're just a regular thread waiting. */
    if (!current || !current->coro) {
        /* Thread context: spin phases are safe */
        
        /* Fast spin with cpu_pause */
        for (int i = 0; i < SPIN_FAST_ITERS; i++) {
            if (atomic_load_explicit(&f->done, memory_order_acquire)) {
                wait_for_fiber_done_state(f);
                if (out_result) *out_result = f->result;
                return 0;
            }
            cpu_pause();
        }
        
        /* Medium spin with sched_yield */
        for (int i = 0; i < SPIN_YIELD_ITERS; i++) {
            if (atomic_load_explicit(&f->done, memory_order_acquire)) {
                wait_for_fiber_done_state(f);
                if (out_result) *out_result = f->result;
                return 0;
            }
            sched_yield();
        }
    }
    /* Fiber context: skip spin phases, go straight to registration/parking */
    
    /* Register as waiter */
    atomic_fetch_add_explicit(&f->join_waiters, 1, memory_order_acq_rel);
    
    /* Check again - fiber might have completed during registration */
    if (atomic_load_explicit(&f->done, memory_order_acquire)) {
        atomic_fetch_sub_explicit(&f->join_waiters, 1, memory_order_relaxed);
        wait_for_fiber_done_state(f);
        if (out_result) *out_result = f->result;
        return 0;
    }

    /* Slow path: choose strategy based on context */
    
    if (current && current->coro) {
        /* We're inside a fiber - PARK instead of blocking the worker thread.
        * This is critical for nested nurseries to avoid deadlock. */
        
        /* Handshake lock: ensures proper ordering between us checking done and
        * setting join_waiter_fiber, and the child setting done and reading waiter.
        * Either we see done=1 (child completed first), OR the child will see
        * our registration (we registered first). No lost wakeups possible. */
        join_spinlock_lock(&f->join_lock);
        
        /* Re-check done under lock */
        if (atomic_load_explicit(&f->done, memory_order_acquire)) {
            join_spinlock_unlock(&f->join_lock);
            atomic_fetch_sub_explicit(&f->join_waiters, 1, memory_order_relaxed);
            wait_for_fiber_done_state(f);
            if (out_result) *out_result = f->result;
            return 0;
        }
        
        /* Store ourselves as the waiter fiber - guaranteed visible to child */
        atomic_store_explicit(&f->join_waiter_fiber, current, memory_order_release);
        join_spinlock_unlock(&f->join_lock);

        /* Help-first join: if target is still queued (not yet started), steer it
        * into our worker's inbox so it runs immediately after we park — instead
        * of waiting behind active compressions on some other worker.
        *
        * Protocol (safe against concurrent stealers):
        *   1. CAS target control: CTRL_QUEUED → CTRL_OWNED(my_wid) — claim it.
        *   2. Reset control back to CTRL_QUEUED — so worker_run_fiber's CAS works.
        *   3. Push a new pointer into our inbox.
        * Any stale pointer still in the original queue will fail worker_run_fiber's
        * CAS and be silently dropped.  If another worker claims the fiber between
        * step 2 and step 3, their CAS wins; our inbox push adds a harmless stale
        * entry that also fails CAS.  No double-execution is possible. */
        int my_wid = tls_worker_id;
        if (cc_join_help_enabled() && my_wid >= 0 && g_sched.inbox_queues) {
            atomic_fetch_add_explicit(&g_cc_join_help_attempts, 1, memory_order_relaxed);
            int64_t ctrl = CTRL_QUEUED;
            int64_t owned = CTRL_OWNED(my_wid);
            if (atomic_compare_exchange_strong_explicit(&f->control, &ctrl, owned,
                                                        memory_order_acq_rel,
                                                        memory_order_relaxed)) {
                atomic_store_explicit(&f->control, CTRL_QUEUED, memory_order_release);
                iq_push_with_edge(&g_sched.inbox_queues[my_wid], f, NULL);
                atomic_fetch_add_explicit(&g_cc_join_help_hits, 1, memory_order_relaxed);
            }
        }

        /* Now park until woken. At this point, either:
        * 1. Child hasn't completed yet - will see our registration and unpark us
        * 2. Child completed while we held lock - done=1, handled above */
        /* Fiber-join path already runs on an active worker thread; parking this
        * fiber yields back to that worker loop immediately, so no extra wake
        * nudge is needed here. */
        int park_loops = 0;
        atomic_fetch_add_explicit(&g_cc_join_park_joins, 1, memory_order_relaxed);
        while (!atomic_load_explicit(&f->done, memory_order_acquire)) {
            park_loops++;
            atomic_fetch_add_explicit(&g_cc_join_park_loops, 1, memory_order_relaxed);
            cc__fiber_park_if(&f->done, 0, "join", __FILE__, __LINE__);
        }
    } else {
        /* Not in fiber context - use condvar (safe to block thread) */
        
        /* Use join_lock to synchronize with fiber completion.
        * The fiber checks join_cv_initialized under this lock, so we must
        * also hold it when setting join_cv_initialized and checking done.
        * This prevents a race where:
        * 1. Fiber sets done=1, reads cv_initialized=0, skips broadcast
        * 2. Joiner sets cv_initialized=1, checks done (misses it), waits forever */
        join_spinlock_lock(&f->join_lock);
        
        /* Check done under lock first */
        if (atomic_load_explicit(&f->done, memory_order_acquire)) {
            join_spinlock_unlock(&f->join_lock);
            atomic_fetch_sub_explicit(&f->join_waiters, 1, memory_order_relaxed);
            wait_for_fiber_done_state(f);
            if (out_result) *out_result = f->result;
            return 0;
        }
        
        /* Lazy init condvar with CAS to avoid double init */
        int expected = 0;
        if (atomic_compare_exchange_strong_explicit(&f->join_cv_initialized, &expected, 1,
                                                    memory_order_acq_rel, memory_order_acquire)) {
            /* We won the race - initialize the condvar */
            pthread_mutex_init(&f->join_mu, NULL);
            pthread_cond_init(&f->join_cv, NULL);
        }
        /* else: another thread already initialized it */
        
        /* Lock the condvar mutex BEFORE releasing join_lock.
        * This ensures we're ready to receive the broadcast before the fiber
        * can send it. The fiber holds join_mu while broadcasting, so:
        * - If fiber hasn't broadcast yet: we'll catch it in pthread_cond_wait
        * - If fiber is broadcasting now: we block on join_mu until it's done,
        *   then see done=1 in the while loop and exit immediately */
        pthread_mutex_lock(&f->join_mu);
        
        /* Re-check done under BOTH locks - fiber may have completed during init */
        if (atomic_load_explicit(&f->done, memory_order_acquire)) {
            pthread_mutex_unlock(&f->join_mu);
            join_spinlock_unlock(&f->join_lock);
            atomic_fetch_sub_explicit(&f->join_waiters, 1, memory_order_relaxed);
            wait_for_fiber_done_state(f);
            if (out_result) *out_result = f->result;
            return 0;
        }
        
        /* Now release join_lock. The fiber will see cv_initialized=1 and
        * try to broadcast, but we already hold join_mu so it will block
        * until we're in pthread_cond_wait. */
        join_spinlock_unlock(&f->join_lock);
        
        /* CRITICAL: Wake a worker before blocking. If all workers are sleeping,
        * the fiber we're waiting for will never run. This ensures at least one
        * worker is awake to process the fiber queue. */
        wake_one_if_sleeping(CC_WAKE_REASON_JOIN_THREAD);
        
        while (!atomic_load_explicit(&f->done, memory_order_acquire)) {
            pthread_cond_wait(&f->join_cv, &f->join_mu);
        }
        pthread_mutex_unlock(&f->join_mu);
    }
    
    atomic_fetch_sub_explicit(&f->join_waiters, 1, memory_order_relaxed);
    
    wait_for_fiber_done_state(f);
    if (out_result) *out_result = f->result;
    return 0;
}

void cc_fiber_set_spawn_nursery_override(CCNursery* nursery) {
    cc__tls_spawn_nursery_override = nursery;
}

void cc_fiber_task_free(fiber_task* f) {
    if (f) {
        fiber_free(f);
    }
}

/* Non-blocking poll: check if fiber is done without blocking */
int cc_fiber_poll_done(fiber_task* f) {
    if (!f) return 1;  /* NULL fiber is "done" */
    return atomic_load_explicit(&f->done, memory_order_acquire);
}

/* Get result from a completed fiber (only valid after poll_done returns true) */
void* cc_fiber_get_result(fiber_task* f) {
    if (!f) return NULL;
    return f->result;
}

/* TLS pointer to the current pool slot's result_buf_copy.
* Set by cc_pool_runner_fn before invoking a task, cleared after.
* cc_task_result_ptr reads this to direct structured results into the
* slot's stable buffer instead of the fiber's result_buf. */
__thread char* cc_tls_pool_slot_result_buf = NULL;

__attribute__((noinline))
void cc__fiber_set_pool_slot_buf(char* buf) {
    cc_tls_pool_slot_result_buf = buf;
}

/* Get pointer to fiber's result_buf (for range checks in task.c) */
void* cc_fiber_get_result_buf(fiber_task* f) {
    if (!f) return NULL;
    return f->result_buf;
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
    char* pool_buf = cc_tls_pool_slot_result_buf;
    if (pool_buf) {
        if (size > 48) return NULL;
        return pool_buf;
    }
    if (tls_current_fiber && size <= sizeof(tls_current_fiber->result_buf)) {
        return tls_current_fiber->result_buf;
    }
    /* V2 hybrid scheduler fiber */
    void* v2_buf = sched_v2_current_result_buf(size);
    if (v2_buf) return v2_buf;
    return NULL;
}

/* Internal: add fiber to parked list for debugging */
static void parked_list_add(fiber_task* f) {
    pthread_mutex_lock(&g_parked_list_mu);
    f->next = g_parked_list_head;
    g_parked_list_head = f;
    pthread_mutex_unlock(&g_parked_list_mu);
}

/* Internal: remove fiber from parked list */
static void parked_list_remove(fiber_task* f) {
    pthread_mutex_lock(&g_parked_list_mu);
    fiber_task** pp = &g_parked_list_head;
    while (*pp) {
        if (*pp == f) {
            *pp = f->next;
            f->next = NULL;
            break;
        }
        pp = &(*pp)->next;
    }
    pthread_mutex_unlock(&g_parked_list_mu);
}

/* worker_commit_park — called by the trampoline (worker_run_fiber) after
* mco_resume returns for a YIELD_PARK fiber.  The stack is now quiescent,
* so we can safely transition to PARKED without a double-resume race.
*
* Protocol (yield-before-commit):
*   1. Check pending_unpark — if set, someone already wants us awake → re-enqueue.
*   2. Re-check park_flag condition — if changed, abort park → re-enqueue.
*   3. CAS OWNED → PARKED.
*   4. Post-commit Dekker: re-check pending_unpark (seq_cst) — if set,
*      CAS PARKED → QUEUED and re-enqueue.
*
* Re-enqueueing here is safe because the stack is quiescent. */
static void worker_commit_park(fiber_task* f, int wid) {
    int64_t owned = (wid >= 0) ? CTRL_OWNED(wid) : CTRL_OWNED_TEMP;

    /* 1. Pre-check pending_unpark */
    if (atomic_exchange_explicit(&f->pending_unpark, 0, memory_order_acq_rel)) {
        /* Someone called unpark while the fiber was running — don't park.
        * CAS OWNED→QUEUED to verify we still own it. */
        int64_t exp_o = owned;
        if (!atomic_compare_exchange_strong_explicit(&f->control, &exp_o, CTRL_QUEUED,
                                                    memory_order_release,
                                                    memory_order_relaxed)) {
            f->timed_park_requested = 0;
            return;  /* Recycled — bail out */
        }
        f->timed_park_requested = 0;
        f->enqueue_src = 3; f->enqueue_ctrl = owned;
        sched_global_push(f, -1);
        return;
    }

    /* 2. Re-check flag condition (if any) */
    _Atomic int* flag = f->park_flag;
    int expected_val = f->park_expected;
    f->park_flag = NULL;  /* consumed */
    if (flag && atomic_load_explicit(flag, memory_order_acquire) != expected_val) {
        /* Condition changed — don't park, re-enqueue.
        * CAS OWNED→QUEUED to verify we still own it. */
        int64_t exp_o2 = owned;
        if (!atomic_compare_exchange_strong_explicit(&f->control, &exp_o2, CTRL_QUEUED,
                                                    memory_order_release,
                                                    memory_order_relaxed)) {
            f->timed_park_requested = 0;
            return;  /* Recycled — bail out */
        }
        f->timed_park_requested = 0;
        f->enqueue_src = 4; f->enqueue_ctrl = owned;
        sched_global_push(f, -1);
        return;
    }

    /* 3. CAS OWNED → PARKED */
    int64_t exp = owned;
    if (!atomic_compare_exchange_strong_explicit(&f->control, &exp, CTRL_PARKED,
                                                memory_order_acq_rel,
                                                memory_order_acquire)) {
        /* CAS failed — fiber was recycled or state changed unexpectedly.
        * Do NOT re-enqueue — we don't own it anymore. */
        f->timed_park_requested = 0;
        return;
    }

    /* 4. Post-commit Dekker check: unpark may have stored pending_unpark=1
    *    after our pre-check but before the PARKED commit.  seq_cst on both
    *    sides prevents the store-load reorder. */
    if (atomic_exchange_explicit(&f->pending_unpark, 0, memory_order_seq_cst)) {
        int64_t parked_exp = CTRL_PARKED;
        if (atomic_compare_exchange_strong_explicit(&f->control, &parked_exp, CTRL_QUEUED,
                                                    memory_order_acq_rel,
                                                    memory_order_acquire)) {
            /* §9 row: PARKED -> RUNNABLE (via self-recovery wake claim). */
            cc_v3_assert_matrix_row(f, "PARKED->RUNNABLE(self-recovery)", 1);
            /* We un-parked ourselves — re-enqueue. */
            f->timed_park_requested = 0;
            f->enqueue_src = 6; f->enqueue_ctrl = CTRL_PARKED;
            sched_global_push(f, -1);
            return;
        }
        /* CAS failed: someone already unparked us (PARKED→QUEUED in unpark path).
        * They enqueued us too, so nothing to do. */
        f->timed_park_requested = 0;
        return;
    }

    /* 5. Re-check flag one more time post-commit */
    if (flag && atomic_load_explicit(flag, memory_order_seq_cst) != expected_val) {
        int64_t parked_exp2 = CTRL_PARKED;
        if (atomic_compare_exchange_strong_explicit(&f->control, &parked_exp2, CTRL_QUEUED,
                                                    memory_order_acq_rel,
                                                    memory_order_acquire)) {
            /* §9 row: PARKED -> RUNNABLE (post-commit flag wake). */
            cc_v3_assert_matrix_row(f, "PARKED->RUNNABLE(post-commit-flag)", 1);
            f->timed_park_requested = 0;
            f->enqueue_src = 7; f->enqueue_ctrl = CTRL_PARKED;
            sched_global_push(f, -1);
            return;
        }
        /* Already unparked by someone else. */
        f->timed_park_requested = 0;
        return;
    }

    /* Successfully parked — update counters. */
    if (f->timed_park_requested) {
        f->timed_park_requested = 0;
        atomic_store_explicit(&f->timed_park_registered, 1, memory_order_release);
        tpq_push(f);
        atomic_fetch_add_explicit(&g_total_timed_parked, 1, memory_order_relaxed);
        if (cc__fiber_deadlock_suppressed(f)) {
            atomic_fetch_add_explicit(&g_deadlock_suppressed_timed_parked, 1, memory_order_relaxed);
        }
    }
    atomic_fetch_add_explicit(&g_sched.total_parked, 1, memory_order_relaxed);
    if (cc__fiber_deadlock_suppressed(f)) {
        atomic_fetch_add_explicit(&g_sched.deadlock_suppressed_parked, 1, memory_order_relaxed);
    }
    if (cc__fiber_external_wait_active(f)) {
        f->external_wait_parked = 1;
        atomic_fetch_add_explicit(&g_external_wait_parked, 1, memory_order_relaxed);
    } else {
        f->external_wait_parked = 0;
    }
}

static int cc__fiber_park_if_impl(_Atomic int* flag, int expected, const struct timespec* abs_deadline,
                                const char* reason, const char* file, int line) {
    if (sched_v2_in_context()) {
        (void)reason; (void)file; (void)line;
        if (flag && atomic_load_explicit(flag, memory_order_acquire) != expected) return 0;
        if (abs_deadline) {
            struct timespec now;
            clock_gettime(CLOCK_REALTIME, &now);
            if (now.tv_sec > abs_deadline->tv_sec ||
                (now.tv_sec == abs_deadline->tv_sec && now.tv_nsec >= abs_deadline->tv_nsec)) {
                return 1; /* timed out */
            }
        }
        sched_v2_park();
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
    fiber_task* f = tls_current_fiber;
    if (!f || !f->coro) return 0;
    f->external_wait_depth = tls_external_wait_depth;
    f->external_wait_scoped = (tls_external_wait_depth > 0);

#if CC_V3_DIAGNOSTICS || defined(CC_DEBUG_FIBER)
    /* Debug-only stack ownership validation. */
    volatile size_t _stack_check;
    size_t stack_addr = (size_t)&_stack_check;
    size_t stack_min = (size_t)f->coro->stack_base;
    size_t stack_max = stack_min + f->coro->stack_size;
    if (stack_addr < stack_min || stack_addr > stack_max) {
#ifdef CC_DEBUG_FIBER
        fprintf(stderr, "[CC DEBUG] cc__fiber_park: tls_current_fiber mismatch, skipping park\n");
#endif
        return 0;
    }
#endif

    /* Always retain park metadata so deadlock dumps stay informative in normal builds. */
    f->park_reason = reason;
    f->park_file = file;
    f->park_line = line;

#if CC_V3_DIAGNOSTICS
    static int park_dbg = -1;
    if (park_dbg == -1) {
        park_dbg = 0;
    }
#endif

    /* Fast-path: pending_unpark already set — don't even yield. */
    if (atomic_exchange_explicit(&f->pending_unpark, 0, memory_order_acq_rel)) {
#if CC_V3_DIAGNOSTICS
#endif
        return 0;
    }

    /* Fast-path: flag condition already changed. */
    if (flag && atomic_load_explicit(flag, memory_order_acquire) != expected) {
#if CC_V3_DIAGNOSTICS
#endif
        return 0;
    }

    if (abs_deadline) {
        struct timespec now;
        clock_gettime(CLOCK_REALTIME, &now);
        if (now.tv_sec > abs_deadline->tv_sec ||
            (now.tv_sec == abs_deadline->tv_sec && now.tv_nsec >= abs_deadline->tv_nsec)) {
            return 1;
        }
    }

    /* Yield-before-commit: stash park parameters and yield.
    * The trampoline (worker_commit_park) will do the actual OWNED→PARKED
    * transition after the stack is quiescent, eliminating the double-resume
    * race where an unparker CASes PARKED→QUEUED and another worker resumes
    * the fiber while it's still live on this worker's stack. */
    f->park_flag = flag;
    f->park_expected = expected;
    f->timed_park_requested = abs_deadline ? 1 : 0;
    if (abs_deadline) {
        f->timed_park_deadline = *abs_deadline;
        atomic_store_explicit(&f->timed_park_fired, 0, memory_order_release);
    }
    f->yield_dest = YIELD_PARK;
    mco_yield(f->coro);

    /* Resumed — either:
    *   (a) worker_commit_park decided not to park (pending_unpark / flag change)
    *       and re-enqueued us, or
    *   (b) we were fully parked, then unparked and re-enqueued.
    * In both cases, worker_run_fiber already set CTRL_OWNED(wid). */
#if CC_V3_DIAGNOSTICS || defined(CC_DEBUG_FIBER)
    f->park_reason = NULL;
    f->park_file = NULL;
    f->park_line = 0;
#endif
    f->park_obj = NULL;
    f->timed_park_requested = 0;
    return atomic_exchange_explicit(&f->timed_park_fired, 0, memory_order_acq_rel) ? 1 : 0;
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
    if (!f) return;
    f->park_obj = obj;
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
    fiber_task* f = (fiber_task*)fiber_ptr;
    if (!f) return;
#if CC_V3_DIAGNOSTICS
    atomic_fetch_add_explicit(&g_cc_fiber_unpark_calls, 1, memory_order_relaxed);
#endif
    int trace_recv_empty = 0;
    int trace_req_wake = 0;
    int claimed_parked = 0;
    int channel_local_handoff = 0;
    if (tls_chan_attr_local_handoff) {
        channel_local_handoff = 1;
        tls_chan_attr_local_handoff = 0;
    }
    if (atomic_exchange_explicit(&f->timed_park_registered, 0, memory_order_acq_rel)) {
        /* Another wake path won before the timer fired. Unlink the stale timed
        * wait node now so a pooled fiber cannot be reused while still present
        * in the intrusive timer queue. */
        (void)tpq_remove_if_present(f);
        atomic_fetch_sub_explicit(&g_total_timed_parked, 1, memory_order_relaxed);
        if (cc__fiber_deadlock_suppressed(f)) {
            atomic_fetch_sub_explicit(&g_deadlock_suppressed_timed_parked, 1, memory_order_relaxed);
        }
    }


    /* LP (§10 Waker claim + wake enqueue, control-word substrate):
    * CAS PARKED -> QUEUED is the single-owner wake claim + runnable publish.
    * If PARKING, the fiber's stack is still active
    * on its owning worker — we must NOT enqueue.  Just set pending_unpark
    * and let the owning worker's park_if see it and bail out. */
    int64_t expected = CTRL_PARKED;
    if (atomic_compare_exchange_strong_explicit(&f->control, &expected, CTRL_QUEUED,
                                                memory_order_acq_rel,
                                                memory_order_acquire)) {
        claimed_parked = 1;
        goto queued;
    }
    if (expected == CTRL_PARKING) {
        /* LP (§10 Waker sees PARKING): publish wake-pending only. */
        /* Fiber is mid-park — its stack is still live on another worker.
        * Set pending_unpark so park_if will abort when it checks.
        * Do NOT CAS or enqueue — the owning worker handles everything. */
        if (trace_req_wake) {
            fprintf(stderr,
                    "CC_REQ_WAKE_UNPARK: parking fiber=%lu obj=%p pending_before=%d\n",
                    (unsigned long)f->fiber_id,
                    f->park_obj,
                    atomic_load_explicit(&f->pending_unpark, memory_order_relaxed));
        }
        atomic_store_explicit(&f->pending_unpark, 1, memory_order_seq_cst);
        if (trace_req_wake) {
            fprintf(stderr,
                    "CC_REQ_WAKE_UNPARK: parking_set_pending fiber=%lu obj=%p pending_after=%d\n",
                    (unsigned long)f->fiber_id,
                    f->park_obj,
                    atomic_load_explicit(&f->pending_unpark, memory_order_relaxed));
        }
        return;
    }
    static int chan_dbg_verbose = -1;
    if (chan_dbg_verbose == -1) {
        const char* env = getenv("CC_CHAN_TRACE_RECV_EMPTY");
        chan_dbg_verbose = (env && env[0] == '1') ? 1 : 0;
    }
    if (chan_dbg_verbose &&
        f->park_reason &&
        strcmp(f->park_reason, "chan_recv_wait_empty") == 0) {
        trace_recv_empty = 1;
    }
    if (f->park_reason &&
        strcmp(f->park_reason, "chan_recv_wait_empty") == 0 &&
        cc__chan_debug_req_wake_match(f->park_obj)) {
        trace_req_wake = 1;
    }
    if (trace_recv_empty) {
        fprintf(stderr,
                "CC_CHAN_DEBUG: unpark_observe fiber=%lu control=%lld reason=%s obj=%p pending=%d\n",
                (unsigned long)f->fiber_id,
                (long long)expected,
                f->park_reason,
                f->park_obj,
                atomic_load_explicit(&f->pending_unpark, memory_order_relaxed));
    }
    if (trace_req_wake) {
        fprintf(stderr,
                "CC_REQ_WAKE_UNPARK: observe fiber=%lu control=%lld reason=%s obj=%p pending=%d\n",
                (unsigned long)f->fiber_id,
                (long long)expected,
                f->park_reason ? f->park_reason : "unknown",
                f->park_obj,
                atomic_load_explicit(&f->pending_unpark, memory_order_relaxed));
    }
    if (CTRL_IS_OWNED(expected) || expected == CTRL_QUEUED) {
        /* Fiber is running or already queued — latch early wake so a later
        * park doesn't lose it. */
        /* seq_cst: Dekker ordering — unpark does store(pending_unpark)
        * then read(control), park does store(control) then
        * read(pending_unpark). seq_cst on both sides prevents the
        * store-load reordering that would cause a lost wakeup. */
        atomic_store_explicit(&f->pending_unpark, 1, memory_order_seq_cst);
        if (trace_req_wake) {
            fprintf(stderr,
                    "CC_REQ_WAKE_UNPARK: latch_pending fiber=%lu control=%lld obj=%p pending=%d\n",
                    (unsigned long)f->fiber_id,
                    (long long)expected,
                    f->park_obj,
                    atomic_load_explicit(&f->pending_unpark, memory_order_relaxed));
        }
    }
    if (expected == CTRL_DONE) {
        return;  /* Already completed, nothing to do */
    }
    return;
queued:
    /* §9 row: PARKED -> RUNNABLE by waker claim owner only. */
    cc_v3_assert_matrix_row(f, "PARKED->RUNNABLE(waker)", claimed_parked);
    if (trace_recv_empty) {
        fprintf(stderr,
                "CC_CHAN_DEBUG: unpark_enqueue fiber=%lu reason=%s obj=%p\n",
                (unsigned long)f->fiber_id,
                f->park_reason ? f->park_reason : "unknown",
                f->park_obj);
    }
    if (trace_req_wake) {
        fprintf(stderr,
                "CC_REQ_WAKE_UNPARK: enqueue fiber=%lu reason=%s obj=%p\n",
                (unsigned long)f->fiber_id,
                f->park_reason ? f->park_reason : "unknown",
                f->park_obj);
    }
#if CC_V3_DIAGNOSTICS
    atomic_fetch_add_explicit(&g_cc_fiber_unpark_enqueues, 1, memory_order_relaxed);
#endif
    /* Decrement parked counter (incremented by worker_commit_park). */
    atomic_fetch_sub_explicit(&g_sched.total_parked, 1, memory_order_relaxed);
    if (cc__fiber_deadlock_suppressed(f)) {
        atomic_fetch_sub_explicit(&g_sched.deadlock_suppressed_parked, 1, memory_order_relaxed);
    }
    if (f->external_wait_parked) {
        atomic_fetch_sub_explicit(&g_external_wait_parked, 1, memory_order_relaxed);
        f->external_wait_parked = 0;
    }
    f->enqueue_src = 8;
    f->enqueue_ctrl = CTRL_PARKED;

    /* Re-enqueue using fiber affinity hint.
    * If the fiber previously ran on a specific worker, prefer that worker's
    * queue (inbox if cross-worker, local if same worker).  This keeps
    * communicating fiber pairs on the same core, reducing __ulock_wake
    * cross-core wakes.  Work stealing naturally redistributes load.
    *
    * Starvation escape (spec §Affinity): if the preferred worker's heartbeat
    * is stale, divert to global queue so other workers can pick up the fiber. */
    int pushed = 0;
    int pushed_global = 0;
    int global_edge = 0;
    int pushed_to_current_local = 0;
    int divert_stale = 0;
    int nonworker_publish = (tls_worker_id < 0);
    int preferred = f->last_worker_id;
    const char* preferred_src = cc__last_worker_src_name(f->last_worker_src);
    const char* route_name = "none";
    int route_target = -2;
    /* Sticky by default: preserve preferred-worker affinity for channel wakes. */
    if (nonworker_publish) {
        if (cc__io_ready_fiber(f) &&
            preferred >= 0 &&
            preferred < (int)g_sched.num_workers &&
            g_sched.worker_lifecycle) {
            cc_worker_lifecycle lc = (cc_worker_lifecycle)atomic_load_explicit(
                &g_sched.worker_lifecycle[preferred], memory_order_relaxed);
            if (lc == CC_WL_ACTIVE || lc == CC_WL_IDLE_SPIN) {
                int inbox_edge = 0;
                for (int attempt = 0; attempt < 16 && !pushed; attempt++) {
                    pushed = (iq_push_with_edge(&g_sched.inbox_queues[preferred], f, &inbox_edge) == 0);
                    if (!pushed) cpu_pause();
                }
                if (pushed) {
                    route_name = "io_ready_hot_inbox";
                    route_target = preferred;
                }
            }
        }
    }
    if (!pushed && nonworker_publish) {
        /* Non-worker wakeups must not rely on a specific inbox wake race.
         * Publish globally so any worker can observe the runnable fiber. */
        cc_sched_io_wake_stat_inc(&g_cc_sched_io_wake_stats.nonworker_global_publish);
        global_edge = sched_global_push(f, preferred);
        if (global_edge) {
            cc_sched_io_wake_stat_inc(&g_cc_sched_io_wake_stats.nonworker_global_edge);
        }
        pushed_global = 1;
        pushed = 1;
        route_name = "nonworker_global";
        route_target = -1;
    } else if (preferred >= 0 && preferred < (int)g_sched.num_workers) {
        int divert = 0;
        if (preferred != tls_worker_id && g_sched.worker_heartbeat) {
            uint64_t hb = atomic_load_explicit(
                &g_sched.worker_heartbeat[preferred].heartbeat,
                memory_order_relaxed);
            uint64_t age = (hb != 0) ? (rdtsc() - hb) : 0;
            if (hb != 0 && age >= (ORPHAN_THRESHOLD_CYCLES * 4ULL)) {
                divert = 1;  /* Worker appears stuck */
                divert_stale = 1;
            }
        }
        if (divert) {
            /* Stale-affinity escape: prefer peer inbox reroute first so work
            * stays on non-global lanes; fall back to global if inbox push fails. */
            if (g_sched.num_workers > 1) {
                size_t target = (size_t)(atomic_fetch_add_explicit(&g_global_pop_rr, 1, memory_order_relaxed) %
                                        g_sched.num_workers);
                if ((int)target == preferred) {
                    target = (target + 1) % g_sched.num_workers;
                }
                int inbox_edge = 0;
                for (int attempt = 0; attempt < 16 && !pushed; attempt++) {
                    pushed = (iq_push_with_edge(&g_sched.inbox_queues[target], f, &inbox_edge) == 0);
                    if (!pushed) cpu_pause();
                }
                if (pushed) {
                    route_name = "divert_inbox";
                    route_target = (int)target;
                }
            }
            if (!pushed) {
                global_edge = sched_global_push(f, preferred);
                pushed_global = 1;
                pushed = 1;
                route_name = "divert_global";
                route_target = -1;
            }
        } else if (preferred == tls_worker_id) {
            pushed = (lq_push(&g_sched.local_queues[preferred], f) == 0);
            if (pushed) {
                route_name = "local_same";
                route_target = preferred;
            }
            if (pushed) pushed_to_current_local = 1;
        } else {
            /* Different worker: use its inbox for locality.
            * Skip only if it is provably sleeping (stale hb) AND spinning
            * peers exist — they'll pick up the work without a wake syscall. */
            int use_preferred = 1;
            if (channel_local_handoff && tls_worker_id >= 0) {
                size_t sleeping_now = atomic_load_explicit(&g_sched.sleeping, memory_order_relaxed);
                if (sleeping_now == 0) {
                    size_t local_depth_now = lq_depth(&g_sched.local_queues[tls_worker_id]);
                    if (local_depth_now < CC_CHAN_UNPARK_LOCAL_DEPTH_LIMIT &&
                            lq_push(&g_sched.local_queues[tls_worker_id], f) == 0) {
                        pushed = 1;
                        pushed_to_current_local = 1;
                        use_preferred = 0;
                        route_name = "channel_local";
                        route_target = tls_worker_id;
                    }
                }
            }
            if (g_sched.worker_heartbeat) {
                uint64_t hb = atomic_load_explicit(
                    &g_sched.worker_heartbeat[preferred].heartbeat, memory_order_relaxed);
                /* Skip preferred inbox only when it is stale AND spinning peers
                * exist — they will pick up the work immediately without a
                * wake syscall.  When spinning==0 the saturation bypass below
                * handles the routing decision instead. */
                if (hb != 0 && (rdtsc() - hb) >= ORPHAN_THRESHOLD_CYCLES &&
                    atomic_load_explicit(&g_sched.spinning, memory_order_relaxed) > 0) {
                    use_preferred = 0;
                }
            }
            /* Saturation bypass: when every worker is actively running a fiber
            * (spinning==0 && sleeping==0), route to the current worker's local
            * queue rather than the preferred worker's inbox.  The current
            * worker is about to become free (it is calling unpark from the
            * completing fiber) and will pick up this fiber immediately — vs
            * waiting a full task cycle for the preferred (busy) worker.
            *
            * This bypass is only for the cross-worker case. It is required
            * for syscall-kidnap recovery: when all base workers are
            * OS-blocked, spinning==0 && sleeping==0, and the bypass routes
            * the timer/heartbeat fiber to the timer-service worker's local
            * queue so it runs immediately without going to a kidnapped inbox. */
            if (use_preferred && tls_worker_id >= 0 &&
                    atomic_load_explicit(&g_sched.spinning, memory_order_relaxed) == 0 &&
                    atomic_load_explicit(&g_sched.sleeping, memory_order_relaxed) == 0) {
                pushed = (lq_push(&g_sched.local_queues[tls_worker_id], f) == 0);
                if (pushed) {
                    pushed_to_current_local = 1;
                    use_preferred = 0;
                    route_name = "local_bypass";
                    route_target = tls_worker_id;
                }
            }
            if (use_preferred) {
                int inbox_edge = 0;
                for (int attempt = 0; attempt < 16 && !pushed; attempt++) {
                    pushed = (iq_push_with_edge(&g_sched.inbox_queues[preferred], f, &inbox_edge) == 0);
                    if (!pushed) cpu_pause();
                }
                if (pushed) {
                    route_name = "preferred_inbox";
                    route_target = preferred;
                }
            }
            /* use_preferred=0: fall through to local/global fallback below */
        }
    } else if (tls_worker_id >= 0) {
        /* No affinity yet — fall back to current worker's local queue */
        pushed = (lq_push(&g_sched.local_queues[tls_worker_id], f) == 0);
        if (pushed) {
            pushed_to_current_local = 1;
            route_name = "local_no_affinity";
            route_target = tls_worker_id;
        }
    }
    
    /* Fallback to global queue if not in worker context or target queue is full */
    if (!pushed) {
        /* Secondary fallback: current worker local queue avoids global churn and
        * keeps progress local when preferred inbox is transiently full. */
        if (tls_worker_id >= 0 && lq_push(&g_sched.local_queues[tls_worker_id], f) == 0) {
            pushed = 1;
            pushed_to_current_local = 1;
            route_name = "local_fallback";
            route_target = tls_worker_id;
        } else {
            /* Non-worker/saturated fallback: try another inbox before global. */
            if (g_sched.num_workers > 0) {
                size_t target = (size_t)(atomic_fetch_add_explicit(&g_global_pop_rr, 1, memory_order_relaxed) %
                                        g_sched.num_workers);
                int inbox_edge = 0;
                for (int attempt = 0; attempt < 16 && !pushed; attempt++) {
                    pushed = (iq_push_with_edge(&g_sched.inbox_queues[target], f, &inbox_edge) == 0);
                    if (!pushed) cpu_pause();
                }
                if (pushed) {
                    route_name = "peer_inbox_fallback";
                    route_target = (int)target;
                }
            }
            if (!pushed) {
                global_edge = sched_global_push(f, preferred);
                pushed_global = 1;
                route_name = "global_fallback";
                route_target = -1;
            }
        }
    }

    
    /* Wake a sleeping worker — but skip the costly kernel wake syscall if
    * we pushed to our OWN local queue (the current worker will find the
    * fiber on its next loop iteration without any kernel transition). */
    if (!pushed_to_current_local) {
        if (pushed_global) {
            if (global_edge && pool_strict_idle_for_nonglobal_wake()) {
                if (divert_stale) {
                    /* Stale-divert is advisory; avoid extra wakes here. */
                } else {
                    wake_one_if_sleeping(CC_WAKE_REASON_UNPARK_GLOBAL_EDGE);
                }
            } else if (nonworker_publish && pool_strict_idle_for_nonglobal_wake()) {
                size_t sleepers = atomic_load_explicit(&g_sched.sleeping, memory_order_relaxed);
                if (sleepers > 0) {
                    cc_sched_io_wake_stat_inc(&g_cc_sched_io_wake_stats.nonworker_global_wake_one);
                    wake_one_if_sleeping(CC_WAKE_REASON_UNPARK_GLOBAL_EDGE);
                }
            }
        } else if (route_target >= 0) {
            /* Inbox-affinity unparks: wake the worker that actually received the
            * task, not merely the original preference. Those can differ on
            * peer-inbox fallback / stale-divert paths. */
            wake_target_worker_if_sleeping(route_target);
        }
    }
    if (trace_req_wake) {
        fprintf(stderr,
                "CC_REQ_WAKE_UNPARK: route fiber=%lu preferred=%d route=%s target=%d pushed=%d global=%d local_current=%d pending=%d\n",
                (unsigned long)f->fiber_id,
                preferred,
                route_name,
                route_target,
                pushed,
                pushed_global,
                pushed_to_current_local,
                atomic_load_explicit(&f->pending_unpark, memory_order_relaxed));
    }
    if (cc__io_wait_trace_fiber(f)) {
        fprintf(stderr,
                "[cc:io_wait:sched] unpark_route fiber=%p id=%lu preferred=%d route=%s target=%d pushed=%d global=%d local_current=%d sleepers=%zu spinning=%zu pending=%d\n",
                (void*)f,
                (unsigned long)f->fiber_id,
                preferred,
                route_name,
                route_target,
                pushed,
                pushed_global,
                pushed_to_current_local,
                atomic_load_explicit(&g_sched.sleeping, memory_order_relaxed),
                atomic_load_explicit(&g_sched.spinning, memory_order_relaxed),
                atomic_load_explicit(&f->pending_unpark, memory_order_relaxed));
    }
}

void cc__fiber_unpark_tagged(void* fiber_ptr, cc__fiber_unpark_reason reason) {
    if ((unsigned)reason >= CC_FIBER_UNPARK_REASON_COUNT) {
        reason = CC_FIBER_UNPARK_REASON_GENERIC;
    }
    atomic_fetch_add_explicit(&g_cc_fiber_unpark_by_reason[reason], 1, memory_order_relaxed);
    cc__fiber_unpark_impl(fiber_ptr);
}

void cc__fiber_unpark(void* fiber_ptr) {
    cc__fiber_unpark_tagged(fiber_ptr, CC_FIBER_UNPARK_REASON_GENERIC);
}


void cc__fiber_unpark_channel_attrib(uint32_t attrib_flags) {
    tls_chan_attr_calls_batch++;
    if ((attrib_flags & CC_FIBER_UNPARK_ATTR_CONTENTION_LOCAL) && tls_worker_id >= 0) {
        tls_chan_attr_local_handoff = 1;
    }
    if (pool_startup_spinning_no_sleep()) {
        tls_chan_attr_startup_batch++;
        if (atomic_load_explicit(&g_sched.sleeping, memory_order_relaxed) > 0) {
            tls_chan_attr_sleepers_batch++;
        }
    }
    if (tls_chan_attr_calls_batch >= 256 || tls_chan_attr_startup_batch >= 32 || tls_chan_attr_sleepers_batch >= 32) {
        tls_chan_attr_calls_batch = 0;
        tls_chan_attr_startup_batch = 0;
        tls_chan_attr_sleepers_batch = 0;
    }
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
    fiber_task* current = tls_current_fiber;
    if (!current || !current->coro) {
        /* Not in fiber context - OS yield */
        sched_yield();
        return;
    }
    
    /* Yield-before-commit: tell the trampoline to enqueue us on the local
    * queue after mco_resume returns.  Control stays OWNED(me) until the
    * trampoline transitions it to QUEUED, so no other worker can touch us. */
    current->yield_dest = YIELD_LOCAL;
    mco_yield(current->coro);
    /* When we resume, worker_run_fiber has already set CTRL_OWNED(new_wid). */
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
    fiber_task* current = tls_current_fiber;
    if (!current || !current->coro) {
        sched_yield();
        return;
    }
    /* Yield-before-commit: trampoline will enqueue on global queue. */
    current->yield_dest = YIELD_GLOBAL;
    mco_yield(current->coro);
}

/* Signal to the sysmon that the current worker is still alive and doing
* productive work.  Call this from long-running tasks that do not yield
* (e.g. CPU-bound pool tasks) to prevent the orphan-threshold detector
* from treating the worker as "stuck" and spawning hybrid-promotion
* threads unnecessarily. */
void cc__fiber_touch_heartbeat(void) {
    int wid = tls_worker_id;
    if (wid < 0 || !g_sched.worker_heartbeat) return;
    atomic_store_explicit(&g_sched.worker_heartbeat[wid].heartbeat,
                        rdtsc(), memory_order_relaxed);
}

/* Notify the sysmon that the current fiber is about to execute a long-running
* CPU-bound operation (e.g. a pool task).  The sysmon will not count this
* worker as "stuck" for the duration, suppressing hybrid-promotion spawns. */
void cc__fiber_pool_task_begin(void) {
    atomic_fetch_add_explicit(&g_pool_tasks_active, 1, memory_order_relaxed);
    /* Also reset the heartbeat so the 1ms threshold starts fresh. */
    cc__fiber_touch_heartbeat();
}

void cc__fiber_pool_task_end(void) {
    cc__fiber_touch_heartbeat();
    atomic_fetch_sub_explicit(&g_pool_tasks_active, 1, memory_order_relaxed);
}

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

/* Pre-warm the fiber pool by creating N fibers with coroutines.
* Call this at startup to avoid cold-start penalty on first nursery.
* Returns number of fibers successfully pre-warmed. */
int cc_fiber_pool_prewarm(size_t n) {
    /* Ensure scheduler is initialized */
    if (atomic_load_explicit(&g_initialized, memory_order_acquire) != 2) {
        cc_fiber_sched_init(0);
    }
    
    size_t created = 0;
    for (size_t i = 0; i < n; i++) {
        fiber_task* f = (fiber_task*)calloc(1, sizeof(fiber_task));
        if (!f) break;
        
        /* Create coroutine */
        mco_desc desc = mco_desc_init(fiber_entry, CC_FIBER_STACK_SIZE);
        desc.user_data = f;
        desc.alloc_cb = cc_guarded_alloc;
        desc.dealloc_cb = cc_guarded_dealloc;
        mco_result res = mco_create(&f->coro, &desc);
        if (res != MCO_SUCCESS) {
            free(f);
            break;
        }
        
        /* Add to free list */
        fiber_free(f);
        created++;
    }
    
    return (int)created;
}
