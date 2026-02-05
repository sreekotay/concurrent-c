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

/* Note: We access minicoro internals (_mco_context, _mco_ctxbuf, _mco_wrap_main, _mco_main)
 * for fast coroutine reset. These are defined in minicoro.h when MINICORO_IMPL is set. */

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sched.h>
#include <time.h>

#include "wake_primitive.h"

/* TSan annotations for synchronization and fiber context switching */
#include "tsan_helpers.h"

/* ============================================================================
 * CPU pause for spin loops
 * ============================================================================ */

static inline void cpu_pause(void) {
    #if defined(__x86_64__) || defined(_M_X64)
    __asm__ volatile("pause");
    #elif defined(__aarch64__) || defined(__arm64__)
    __asm__ volatile("yield");
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
typedef struct {
    _Atomic uint64_t alloc_cycles;
    _Atomic uint64_t coro_cycles;
    _Atomic uint64_t push_cycles;
    _Atomic uint64_t wake_cycles;
    _Atomic uint64_t total_cycles;
    _Atomic size_t count;
    _Atomic size_t wake_calls;      /* How many times we actually woke a worker */
    _Atomic size_t wake_skipped;    /* How many times sleeping was 0 */
} spawn_timing;

static spawn_timing g_spawn_timing = {0};
static int g_timing_enabled = -1;  /* -1 = not checked, 0 = disabled, 1 = enabled */

static int spawn_timing_enabled(void) {
    if (g_timing_enabled < 0) {
        g_timing_enabled = getenv("CC_SPAWN_TIMING") != NULL;
    }
    return g_timing_enabled;
}

void cc_fiber_dump_timing(void) {
    size_t count = atomic_load(&g_spawn_timing.count);
    if (count == 0) {
        fprintf(stderr, "\n=== SPAWN TIMING: no spawns recorded ===\n");
        return;
    }
    
    uint64_t alloc = atomic_load(&g_spawn_timing.alloc_cycles);
    uint64_t coro = atomic_load(&g_spawn_timing.coro_cycles);
    uint64_t push = atomic_load(&g_spawn_timing.push_cycles);
    uint64_t wake = atomic_load(&g_spawn_timing.wake_cycles);
    uint64_t total = atomic_load(&g_spawn_timing.total_cycles);
    size_t wake_calls = atomic_load(&g_spawn_timing.wake_calls);
    size_t wake_skipped = atomic_load(&g_spawn_timing.wake_skipped);
    
    fprintf(stderr, "\n=== SPAWN TIMING (%zu spawns) ===\n", count);
    fprintf(stderr, "  Total:      %8.1f cycles/spawn (100.0%%)\n", (double)total / count);
    fprintf(stderr, "  Breakdown:\n");
    fprintf(stderr, "    alloc:    %8.1f cycles/spawn (%5.1f%%)\n", (double)alloc / count, 100.0 * alloc / total);
    fprintf(stderr, "    coro:     %8.1f cycles/spawn (%5.1f%%)\n", (double)coro / count, 100.0 * coro / total);
    fprintf(stderr, "    push:     %8.1f cycles/spawn (%5.1f%%)\n", (double)push / count, 100.0 * push / total);
    fprintf(stderr, "    wake:     %8.1f cycles/spawn (%5.1f%%)\n", (double)wake / count, 100.0 * wake / total);
    fprintf(stderr, "  Wake stats: %zu calls, %zu skipped (%.1f%% hit rate)\n", 
            wake_calls, wake_skipped, 
            count > 0 ? 100.0 * wake_calls / count : 0.0);
    fprintf(stderr, "================================\n\n");
}

/* Forward declaration - defined in nursery.c */
void cc_nursery_dump_timing(void);

/* ============================================================================
 * Spin-then-condvar constants
 * 
 * Tuned for high-throughput channel operations. More spinning reduces condvar
 * syscall overhead at the cost of CPU usage when idle. Override via env vars:
 *   CC_SPIN_FAST_ITERS=512   (default: 256)
 *   CC_SPIN_YIELD_ITERS=32   (default: 16)
 * ============================================================================ */

#define SPIN_FAST_ITERS_DEFAULT 256
#define SPIN_YIELD_ITERS_DEFAULT 16

static _Atomic int g_spin_fast_iters = -1;   /* -1 = not initialized */
static _Atomic int g_spin_yield_iters = -1;

static int get_spin_fast_iters(void) {
    int val = atomic_load_explicit(&g_spin_fast_iters, memory_order_acquire);
    if (val < 0) {
        const char* env = getenv("CC_SPIN_FAST_ITERS");
        int new_val = env ? atoi(env) : SPIN_FAST_ITERS_DEFAULT;
        if (new_val <= 0) new_val = SPIN_FAST_ITERS_DEFAULT;
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
        if (new_val <= 0) new_val = SPIN_YIELD_ITERS_DEFAULT;
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

#ifndef CC_FIBER_QUEUE_SIZE
#define CC_FIBER_QUEUE_SIZE 65536  /* 64K slots to handle large workloads */
#endif

#define MAX_WORKERS 64
#define CACHE_LINE_SIZE 64

/* ============================================================================
 * Fiber State
 * ============================================================================ */

typedef enum {
    FIBER_CREATED,
    FIBER_READY,
    FIBER_RUNNING,
    FIBER_PARKED,
    FIBER_DONE
} fiber_state;

typedef struct fiber_task {
    /* Hot path fields - accessed during execution */
    mco_coro* coro;           /* minicoro coroutine handle */
    void* (*fn)(void*);       /* User function */
    void* arg;                /* User argument */
    void* result;             /* Return value */
    char result_buf[48];      /* Fiber-local storage for struct results (avoids malloc) */
    _Atomic int state;
    _Atomic int done;
    _Atomic int running_lock; /* Serialize resume/unpark */
    _Atomic int unpark_pending; /* Wake happened before park */
    
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
    uintptr_t fiber_id;       /* Unique ID for this fiber (for debug output) */
    
    struct fiber_task* next;  /* For free list / queues */
} fiber_task;

/* ============================================================================
 * Lock-Free MPMC Queue
 * ============================================================================ */

typedef struct {
    fiber_task* _Atomic slots[CC_FIBER_QUEUE_SIZE];
    _Atomic size_t head;
    _Atomic size_t tail;
} fiber_queue;

static int fq_push(fiber_queue* q, fiber_task* f) {
    int pause_round = 0;
    for (int retry = 0; retry < 1000; retry++) {
        size_t tail = atomic_load_explicit(&q->tail, memory_order_relaxed);
        size_t head = atomic_load_explicit(&q->head, memory_order_acquire);

        if (tail - head >= CC_FIBER_QUEUE_SIZE) {
            if (++pause_round >= 16) {
                pause_round = 0;
                sched_yield();
            } else {
                cpu_pause();
            }
            continue;
        }

        if (atomic_compare_exchange_weak_explicit(&q->tail, &tail, tail + 1,
                                                   memory_order_release,
                                                   memory_order_relaxed)) {
            atomic_store_explicit(&q->slots[tail % CC_FIBER_QUEUE_SIZE], f, memory_order_release);
            return 0;
        }
        pause_round = 0;
        cpu_pause();
    }
    sched_yield();
    return -1;
}

static inline void fq_push_blocking(fiber_queue* q, fiber_task* f) {
    while (fq_push(q, f) != 0) {
        sched_yield();
    }
}


/* Check if global queue has items (non-destructive peek) */
static int fq_peek(fiber_queue* q) {
    size_t head = atomic_load_explicit(&q->head, memory_order_relaxed);
    size_t tail = atomic_load_explicit(&q->tail, memory_order_acquire);
    return head < tail;
}

static fiber_task* fq_pop(fiber_queue* q) {
    for (int retry = 0; retry < 100; retry++) {
        size_t head = atomic_load_explicit(&q->head, memory_order_relaxed);
        size_t tail = atomic_load_explicit(&q->tail, memory_order_acquire);
        
        if (head >= tail) return NULL;
        
        size_t idx = head % CC_FIBER_QUEUE_SIZE;
        fiber_task* f = atomic_load_explicit(&q->slots[idx], memory_order_acquire);
        
        if (!f) {
            for (int i = 0; i < 10; i++) cpu_pause();
            continue;
        }
        
        if (atomic_compare_exchange_weak_explicit(&q->head, &head, head + 1,
                                                   memory_order_release,
                                                   memory_order_relaxed)) {
            atomic_store_explicit(&q->slots[idx], NULL, memory_order_relaxed);
            return f;
        }
    }
    return NULL;
}

/* ============================================================================
 * Scheduler State
 * ============================================================================ */

/* Per-worker local queue for spawn locality */
#define LOCAL_QUEUE_SIZE 256
/* Per-worker inbox for cross-thread spawns.
 * If this fills, we fall back to the global queue and optionally warn. */
#define INBOX_QUEUE_SIZE 1024

typedef struct {
    fiber_task* _Atomic slots[LOCAL_QUEUE_SIZE];
    _Atomic size_t head;
    _Atomic size_t tail;
} local_queue;

typedef struct {
    fiber_task* _Atomic slots[INBOX_QUEUE_SIZE];
    _Atomic size_t head;
    _Atomic size_t tail;
} inbox_queue;

static _Atomic size_t g_inbox_overflow = 0;
static _Atomic int g_inbox_warned = 0;
static int g_inbox_debug = -1;  /* -1 = not checked, 0 = disabled, 1 = enabled */
static int g_inbox_dump = -1;   /* -1 = not checked, 0 = disabled, 1 = enabled */

static int inbox_debug_enabled(void) {
    if (g_inbox_debug < 0) {
        g_inbox_debug = getenv("CC_DEBUG_INBOX") != NULL;
    }
    return g_inbox_debug;
}

static int inbox_dump_enabled(void) {
    if (g_inbox_dump < 0) {
        g_inbox_dump = getenv("CC_DEBUG_INBOX_DUMP") != NULL;
    }
    return g_inbox_dump;
}

static int join_debug_enabled(void) {
    static int enabled = -1;
    if (enabled < 0) {
        enabled = getenv("CC_DEBUG_JOIN") != NULL;
    }
    return enabled;
}

static void cc__fiber_dump_queue_state(void);

static int iq_push(inbox_queue* q, fiber_task* f) {
    int pause_round = 0;
    for (int retry = 0; retry < 1000; retry++) {
        size_t tail = atomic_load_explicit(&q->tail, memory_order_relaxed);
        size_t head = atomic_load_explicit(&q->head, memory_order_acquire);
        if (tail - head >= INBOX_QUEUE_SIZE) {
            if (++pause_round >= 16) {
                pause_round = 0;
                sched_yield();
            } else {
                cpu_pause();
            }
            continue;
        }
        if (atomic_compare_exchange_weak_explicit(&q->tail, &tail, tail + 1,
                                                   memory_order_release,
                                                   memory_order_relaxed)) {
            atomic_store_explicit(&q->slots[tail % INBOX_QUEUE_SIZE], f, memory_order_release);
            return 0;
        }
        pause_round = 0;
        cpu_pause();
    }
    sched_yield();
    atomic_fetch_add_explicit(&g_inbox_overflow, 1, memory_order_relaxed);
    if (inbox_debug_enabled()) {
        int expected = 0;
        if (atomic_compare_exchange_strong(&g_inbox_warned, &expected, 1)) {
            fprintf(stderr,
                    "[cc] inbox full (size=%d); falling back to global queue\n",
                    INBOX_QUEUE_SIZE);
        }
    }
    return -1;
}

static int iq_peek(inbox_queue* q) {
    size_t head = atomic_load_explicit(&q->head, memory_order_relaxed);
    size_t tail = atomic_load_explicit(&q->tail, memory_order_acquire);
    return head < tail;
}

static fiber_task* iq_pop(inbox_queue* q) {
    for (int retry = 0; retry < 100; retry++) {
        size_t head = atomic_load_explicit(&q->head, memory_order_relaxed);
        size_t tail = atomic_load_explicit(&q->tail, memory_order_acquire);
        if (head >= tail) return NULL;
        size_t idx = head % INBOX_QUEUE_SIZE;
        fiber_task* f = atomic_load_explicit(&q->slots[idx], memory_order_acquire);
        if (!f) {
            for (int i = 0; i < 10; i++) cpu_pause();
            continue;
        }
        if (atomic_compare_exchange_weak_explicit(&q->head, &head, head + 1,
                                                   memory_order_release,
                                                   memory_order_relaxed)) {
            atomic_store_explicit(&q->slots[idx], NULL, memory_order_relaxed);
            return f;
        }
    }
    return NULL;
}

typedef struct {
    pthread_t* workers;
    size_t num_workers;
    _Atomic int running;
    
    fiber_queue* run_queue;         /* Global queue for overflow and cross-worker */
    local_queue* local_queues;      /* Per-worker local queues */
    inbox_queue* inbox_queues;      /* Per-worker MPMC inbox queues */
    fiber_task* _Atomic free_list;
    
    wake_primitive wake_prim;       /* Fast worker wake (futex/ulock instead of condvar) */
    char _pad_wake[CACHE_LINE_SIZE];  /* Isolate wake_prim from pending */
    
    /* HIGHLY CONTENDED: updated on every spawn and complete - needs own cache line */
    _Atomic size_t pending;
    char _pad_pending[CACHE_LINE_SIZE - sizeof(_Atomic size_t)];
    
    /* Track worker states for smarter waking - cache line padded to avoid false sharing */
    _Atomic size_t active;      /* Workers currently executing fibers */
    char _pad_active[CACHE_LINE_SIZE - sizeof(_Atomic size_t)];
    
    _Atomic size_t sleeping;    /* Workers blocked on condvar */
    char _pad_sleeping[CACHE_LINE_SIZE - sizeof(_Atomic size_t)];
    
    _Atomic size_t spinning;    /* Workers actively polling (not sleeping yet) */
    char _pad_spinning[CACHE_LINE_SIZE - sizeof(_Atomic size_t)];
    
    /* Per-worker parked counts - avoids global mutex and cache line bouncing */
    _Atomic size_t* worker_parked;
    
    /* Hybrid promotion (sysmon): per-worker heartbeat updated once per batch loop.
     * Sysmon detects stuck workers by checking if heartbeat hasn't updated.
     * Cache-line aligned so sysmon reads don't false-share with worker writes. */
    struct { _Atomic uint64_t heartbeat; char _pad[CACHE_LINE_SIZE - sizeof(_Atomic uint64_t)]; }* worker_heartbeat;
    
    /* Sysmon thread: spawns temp workers when CPU-bound fibers stall */
    pthread_t sysmon_thread;
    int sysmon_started;  /* 1 if pthread_create succeeded (shutdown joins only then) */
    _Atomic int sysmon_running;
    _Atomic size_t temp_worker_count;
    _Atomic uint64_t last_promotion_cycles;  /* rdtsc of last temp worker spawn (rate limit) */
    _Atomic size_t promotion_count;         /* Stats: total temp workers ever spawned */
    
    /* Stats - less hot, can share cache lines */
    _Atomic size_t blocked_threads;  /* Threads blocked in cc_block_on (not fiber parking) */
    _Atomic size_t completed;
    _Atomic size_t coro_reused;
    _Atomic size_t coro_created;
} fiber_sched;

static fiber_sched g_sched = {0};
static _Atomic int g_initialized = 0;
static _Atomic int g_deadlock_reported = 0;
static _Atomic uint64_t g_deadlock_first_seen = 0;  /* Timestamp when deadlock state first seen */
static _Atomic size_t g_requested_workers = 0;  /* User-requested worker count (0 = auto) */

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

/* Parked fibers list for debugging - only used when CC_DEBUG_DEADLOCK is set */
#ifdef CC_DEBUG_DEADLOCK
static pthread_mutex_t g_parked_list_mu = PTHREAD_MUTEX_INITIALIZER;
static fiber_task* g_parked_list_head = NULL;
#endif

/* Helper: sum per-worker parked counts */
static inline size_t get_total_parked(void) {
    size_t total = 0;
    if (!g_sched.worker_parked) return 0;
    for (size_t i = 0; i < g_sched.num_workers; i++) {
        total += atomic_load_explicit(&g_sched.worker_parked[i], memory_order_relaxed);
    }
    return total;
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
    size_t temp_workers = atomic_load_explicit(&g_sched.temp_worker_count, memory_order_acquire);
    size_t total_workers = g_sched.num_workers + temp_workers;
    
    /* Potential deadlock: all workers unavailable + parked fibers waiting */
    size_t unavailable = sleeping + blocked;
    if (unavailable >= total_workers && parked > 0) {
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
            fprintf(stderr, "  Fibers:  %zu parked (waiting), %zu completed total\n",
                    parked, (size_t)atomic_load(&g_sched.completed));
            fprintf(stderr, "\n");
            cc__fiber_dump_queue_state();
            
#ifdef CC_DEBUG_DEADLOCK
            /* Dump parked fibers - only available with CC_DEBUG_DEADLOCK */
            fprintf(stderr, "Parked fibers (waiting for unpark that will never come):\n");
            pthread_mutex_lock(&g_parked_list_mu);
            fiber_task* f = g_parked_list_head;
            int count = 0;
            while (f && count < 20) {  /* Limit output to first 20 */
                const char* reason = f->park_reason ? f->park_reason : "unknown";
                if (f->park_file && f->park_line > 0) {
                    fprintf(stderr, "  [fiber %lu] %s at %s:%d\n",
                            (unsigned long)f->fiber_id, reason, f->park_file, f->park_line);
                } else {
                    fprintf(stderr, "  [fiber %lu] %s\n",
                            (unsigned long)f->fiber_id, reason);
                }
                f = f->next;
                count++;
            }
            if (f) {
                fprintf(stderr, "  ... and %zu more\n", parked - 20);
            }
            pthread_mutex_unlock(&g_parked_list_mu);
#else
            fprintf(stderr, "(Compile with -DCC_DEBUG_DEADLOCK for detailed fiber info)\n");
#endif
            
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
static __thread int tls_worker_id = -1;  /* -1 = not a worker thread */
static __thread void* tls_tsan_sched_fiber = NULL;

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

/* Fast local queue push (single producer) */
static inline int lq_push(local_queue* q, fiber_task* f) {
    size_t tail = atomic_load_explicit(&q->tail, memory_order_relaxed);
    size_t head = atomic_load_explicit(&q->head, memory_order_acquire);
    if (tail - head >= LOCAL_QUEUE_SIZE) return -1;  /* Full */
    /* Use release on slot store to ensure closure contents are visible to consumer.
     * The consumer uses acquire on the exchange, creating a release-acquire pair. */
    atomic_store_explicit(&q->slots[tail % LOCAL_QUEUE_SIZE], f, memory_order_release);
    atomic_store_explicit(&q->tail, tail + 1, memory_order_release);
    return 0;
}

/* Check if local queue has items (non-destructive peek) */
static inline int lq_peek(local_queue* q) {
    size_t head = atomic_load_explicit(&q->head, memory_order_relaxed);
    size_t tail = atomic_load_explicit(&q->tail, memory_order_acquire);
    return head < tail;
}

/* Fast local queue pop (owner only - but must handle concurrent stealers) 
 * Uses atomic exchange to claim slot first, then try to advance head once.
 * Limited retries to avoid infinite loop under pathological contention. */
static inline fiber_task* lq_pop(local_queue* q) {
    for (int retry = 0; retry < 64; retry++) {
        size_t head = atomic_load_explicit(&q->head, memory_order_relaxed);
        size_t tail = atomic_load_explicit(&q->tail, memory_order_acquire);
        if (head >= tail) return NULL;  /* Empty */
        
        size_t idx = head % LOCAL_QUEUE_SIZE;
        
        /* Atomically exchange slot with NULL to claim it */
        fiber_task* f = atomic_exchange_explicit(&q->slots[idx], NULL, memory_order_acquire);
        if (!f) {
            /* Lost race with stealer - they cleared slot, try to help advance head */
            atomic_compare_exchange_weak_explicit(&q->head, &head, head + 1,
                                                   memory_order_release, memory_order_relaxed);
            continue;
        }
        
        /* We got the task. Try to advance head once.
         * If CAS fails, someone else advanced it for us - that's fine. */
        atomic_compare_exchange_weak_explicit(&q->head, &head, head + 1,
                                               memory_order_release, memory_order_relaxed);
        return f;
    }
    return NULL;  /* Too much contention, let caller try global queue */
}

static inline void wake_one_if_sleeping(int timing) {
    size_t spinning = atomic_load_explicit(&g_sched.spinning, memory_order_relaxed);
    if (spinning == 0) {
        size_t sleeping = atomic_load_explicit(&g_sched.sleeping, memory_order_relaxed);
        if (sleeping > 0) {
            wake_primitive_wake_one(&g_sched.wake_prim);
            if (timing) atomic_fetch_add_explicit(&g_spawn_timing.wake_calls, 1, memory_order_relaxed);
        } else {
            if (timing) atomic_fetch_add_explicit(&g_spawn_timing.wake_skipped, 1, memory_order_relaxed);
        }
    } else {
        if (timing) atomic_fetch_add_explicit(&g_spawn_timing.wake_skipped, 1, memory_order_relaxed);
    }
}

static inline void wake_one_if_sleeping_unconditional(int timing) {
    size_t sleeping = atomic_load_explicit(&g_sched.sleeping, memory_order_relaxed);
    if (sleeping > 0) {
        wake_primitive_wake_one(&g_sched.wake_prim);
        if (timing) atomic_fetch_add_explicit(&g_spawn_timing.wake_calls, 1, memory_order_relaxed);
    } else {
        if (timing) atomic_fetch_add_explicit(&g_spawn_timing.wake_skipped, 1, memory_order_relaxed);
    }
}

static void cc__fiber_dump_queue_state(void) {
    if (!inbox_dump_enabled()) return;
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
    fprintf(stderr, "  run_queue: head=%zu tail=%zu\n",
            atomic_load(&g_sched.run_queue->head),
            atomic_load(&g_sched.run_queue->tail));
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

/* Work stealing: steal from another worker's queue.
 * Uses atomic exchange to claim slot first, then CAS to advance head. */
static inline fiber_task* lq_steal(local_queue* q) {
    size_t head = atomic_load_explicit(&q->head, memory_order_acquire);
    size_t tail = atomic_load_explicit(&q->tail, memory_order_acquire);
    if (head >= tail) return NULL;  /* Empty */
    
    size_t idx = head % LOCAL_QUEUE_SIZE;
    
    /* Atomically exchange slot with NULL to claim it */
    fiber_task* f = atomic_exchange_explicit(&q->slots[idx], NULL, memory_order_acquire);
    if (!f) return NULL;  /* Lost race */
    
    /* We got the task. Now try to advance head. If we fail, someone else advanced it. */
    if (!atomic_compare_exchange_weak_explicit(&q->head, &head, head + 1,
                                                memory_order_release, memory_order_relaxed)) {
        /* Lost CAS but we still have the task - another thread advanced head.
         * This can happen if owner also exchanged slot (both got NULL except us).
         * But since we got f != NULL, we're the winner. Just return it. */
    }
    return f;
}

/* Batch work stealing: steal up to half the victim's queue.
 * This amortizes the cost of coordinating the steal across multiple tasks.
 * Returns number of tasks stolen (stored in out_tasks array). */
static inline size_t lq_steal_batch(local_queue* q, fiber_task** out_tasks, size_t max_steal) {
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
        fiber_task* f = atomic_exchange_explicit(&q->slots[idx], NULL, memory_order_acquire);
        if (!f) {
            /* Lost race - try to help advance head and continue */
            atomic_compare_exchange_weak_explicit(&q->head, &head, head + 1,
                                                   memory_order_release, memory_order_relaxed);
            continue;
        }
        
        /* Got a task - try to advance head */
        atomic_compare_exchange_weak_explicit(&q->head, &head, head + 1,
                                               memory_order_release, memory_order_relaxed);
        out_tasks[stolen++] = f;
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
        size_t head = atomic_load(&g_sched.run_queue->head);
        size_t tail = atomic_load(&g_sched.run_queue->tail);
        fprintf(stderr, "  run_queue: head=%zu tail=%zu (approx %zu items)\n",
                head, tail, (tail - head) % CC_FIBER_QUEUE_SIZE);
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
            /* Reuse pooled fiber - reset state but KEEP the coro, join_cv, and fiber_id */
            f->fn = NULL;
            f->arg = NULL;
            f->result = NULL;
            atomic_store(&f->state, FIBER_CREATED);
            atomic_store(&f->done, 0);
            atomic_store(&f->running_lock, 0);
            atomic_store(&f->unpark_pending, 0);
            atomic_store(&f->join_waiters, 0);
            atomic_store(&f->join_waiter_fiber, NULL);
            atomic_store(&f->join_lock, 0);
            f->tsan_fiber = TSAN_FIBER_CREATE();
            f->park_reason = NULL;
            f->park_file = NULL;
            f->park_line = 0;
            f->next = NULL;
            /* f->coro, f->join_cv, and f->fiber_id are kept for reuse! */
            return f;
        }
    }
    
    /* Allocate new fiber */
    fiber_task* nf = (fiber_task*)calloc(1, sizeof(fiber_task));
    if (nf) {
        atomic_store_explicit(&nf->join_cv_initialized, 0, memory_order_relaxed);
        atomic_store(&nf->join_waiters, 0);
        atomic_store(&nf->join_waiter_fiber, NULL);
        atomic_store(&nf->join_lock, 0);
        nf->tsan_fiber = TSAN_FIBER_CREATE();
        nf->fiber_id = atomic_fetch_add(&g_next_fiber_id, 1);
    }
    return nf;
}

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
        fprintf(stderr, "Fiber: %p, state=%d, done=%d\n", 
                (void*)f, atomic_load(&f->state), atomic_load(&f->done));
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
        f->result = f->fn(f->arg);
    }
    /* Always use handshake lock to ensure proper ordering between
     * child setting done=1 and parent registering as waiter.
     * The fast path (checking join_waiters without lock) had a race where
     * we could miss waiter registrations due to memory ordering. */
    join_spinlock_lock(&f->join_lock);
    atomic_store_explicit(&f->done, 1, memory_order_release);
    fiber_task* waiter = atomic_exchange_explicit(&f->join_waiter_fiber, NULL, memory_order_acq_rel);
    join_spinlock_unlock(&f->join_lock);
    if (join_debug_enabled()) {
        int waiters = atomic_load_explicit(&f->join_waiters, memory_order_relaxed);
        fprintf(stderr,
                "[join] fiber_entry done: fiber=%lu waiter=%s waiters=%d state=%d\n",
                (unsigned long)f->fiber_id,
                waiter ? "set" : "null",
                waiters,
                atomic_load_explicit(&f->state, memory_order_relaxed));
    }
    
    /* Set state to DONE BEFORE signaling waiters.
     * This ensures the fiber is fully "completed" before joiners return
     * and potentially free the fiber to the pool. If we set done=1 before
     * state=DONE, the joiner could return and free the fiber while we're
     * still writing to f->state. */
    atomic_store_explicit(&f->state, FIBER_DONE, memory_order_release);
    atomic_fetch_sub_explicit(&g_sched.pending, 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&g_sched.completed, 1, memory_order_relaxed);
    
    if (waiter) {
        cc__fiber_unpark(waiter);
    }
    
    /* Signal thread waiters via condvar if initialized */
    if (atomic_load_explicit(&f->join_cv_initialized, memory_order_acquire)) {
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

/* Helper to resume fiber with error checking */
static void fiber_resume(fiber_task* f) {
    if (!f->coro) {
        fiber_panic("NULL coroutine", f, MCO_INVALID_POINTER);
    }
    
    /* Acquire running lock - serializes resume with unpark */
    int expected = 0;
    if (!atomic_compare_exchange_strong_explicit(&f->running_lock, &expected, 1,
                                                  memory_order_acq_rel,
                                                  memory_order_acquire)) {
        fiber_panic("double resume detected", f, MCO_INVALID_OPERATION);
    }
    
    mco_state st = mco_status(f->coro);
    if (st != MCO_SUSPENDED) {
        atomic_store_explicit(&f->running_lock, 0, memory_order_release);
        fiber_panic("coroutine not in suspended state", f, MCO_NOT_SUSPENDED);
    }
    
    /* Switch TSan to the fiber context before resuming. */
    TSAN_FIBER_SWITCH(f->tsan_fiber);
    mco_result res = mco_resume(f->coro);
    /* Switch back to scheduler context after resume returns. */
    TSAN_FIBER_SWITCH(tls_tsan_sched_fiber);
    
    /* Release running lock */
    atomic_store_explicit(&f->running_lock, 0, memory_order_release);
    
    if (res != MCO_SUCCESS) {
        fiber_panic("mco_resume failed", f, res);
    }
}

#define WORKER_BATCH_SIZE 16  /* Standard batch size */
#define STEAL_BATCH_SIZE (LOCAL_QUEUE_SIZE / 2)  /* Steal up to half the victim's queue */

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
#define ORPHAN_THRESHOLD_CYCLES 750000       /* ~0.25ms at 3GHz - faster stuck detection */
#define MAX_EXTRA_WORKERS 8                  /* scale up to 2x cores max */
#define ORPHAN_COOLDOWN_CYCLES 250000        /* ~0.08ms between batch spawns */

/* Check if there's work in global queue (not local - local is "owned" by workers) */
static int sysmon_has_global_pending(void) {
    return g_sched.run_queue ? fq_peek(g_sched.run_queue) : 0;
}

static int sysmon_has_pending_work(void) {
    if (g_sched.run_queue && fq_peek(g_sched.run_queue)) return 1;
    for (size_t i = 0; i < g_sched.num_workers; i++) {
        if (lq_peek(&g_sched.local_queues[i])) return 1;
        if (iq_peek(&g_sched.inbox_queues[i])) return 1;
    }
    return 0;
}

/* Count total pending tasks across all queues (for predictive scaling) */
static size_t sysmon_count_pending(void) {
    size_t count = 0;
    /* Global queue: approximate by checking if non-empty */
    if (g_sched.run_queue && fq_peek(g_sched.run_queue)) count += 8; /* assume decent batch */
    /* Local queues: count actual tasks */
    for (size_t i = 0; i < g_sched.num_workers; i++) {
        local_queue* q = &g_sched.local_queues[i];
        size_t head = atomic_load_explicit(&q->head, memory_order_relaxed);
        size_t tail = atomic_load_explicit(&q->tail, memory_order_relaxed);
        if (tail > head) count += (tail - head);
    }
    return count;
}

/* Replacement worker: same as regular worker but uses global queue only.
 * Permanent - sleeps when idle (doesn't burn CPU competing). */
static void* replacement_worker(void* arg) {
    (void)arg;
    if (!tls_tsan_sched_fiber) {
        tls_tsan_sched_fiber = TSAN_FIBER_CREATE();
        TSAN_FIBER_SWITCH(tls_tsan_sched_fiber);
    }
    uint64_t rng_state = rdtsc();

    while (atomic_load_explicit(&g_sched.running, memory_order_acquire)) {
        fiber_task* f = g_sched.run_queue ? fq_pop(g_sched.run_queue) : NULL;
        
        /* Try stealing from any worker */
        if (!f && g_sched.num_workers > 0) {
            size_t victim = (size_t)(xorshift64(&rng_state) % g_sched.num_workers);
            f = iq_pop(&g_sched.inbox_queues[victim]);
            if (!f) {
                f = lq_steal(&g_sched.local_queues[victim]);
            }
        }
        
        if (f) {
            tls_current_fiber = f;
            fiber_resume(f);
            tls_current_fiber = NULL;
        } else {
            /* Brief spin then short sleep - balance responsiveness vs CPU burn */
            for (int spin = 0; spin < 64; spin++) {
                f = g_sched.run_queue ? fq_pop(g_sched.run_queue) : NULL;
                if (f) goto got_work;
                cpu_pause();
            }
            /* Short sleep - wake primitive wakes us when work arrives */
            atomic_fetch_add_explicit(&g_sched.sleeping, 1, memory_order_release);
            uint32_t wake_val = atomic_load_explicit(&g_sched.wake_prim.value, memory_order_acquire);
            if (!g_sched.run_queue || !fq_peek(g_sched.run_queue)) {
                wake_primitive_wait_timeout(&g_sched.wake_prim, wake_val, 5);
            }
            atomic_fetch_sub_explicit(&g_sched.sleeping, 1, memory_order_relaxed);
            continue;
        got_work:
            tls_current_fiber = f;
            fiber_resume(f);
            tls_current_fiber = NULL;
        }
    }
    atomic_fetch_sub(&g_sched.temp_worker_count, 1);
    return NULL;
}

static void* sysmon_main(void* arg) {
    (void)arg;
    while (atomic_load_explicit(&g_sched.sysmon_running, memory_order_acquire)) {
        usleep(SYSMON_CHECK_US);

        size_t current = atomic_load(&g_sched.temp_worker_count);
        if (current >= MAX_EXTRA_WORKERS) continue;
        
        /* Scale up if there's pending work in ANY queue (auto-scale for CPU-bound) */
        if (!sysmon_has_pending_work()) continue;
        
        /* Check for stuck workers */
        uint64_t now = rdtsc();
        size_t stuck = 0;
        for (size_t i = 0; i < g_sched.num_workers; i++) {
            uint64_t hb = atomic_load_explicit(&g_sched.worker_heartbeat[i].heartbeat, memory_order_acquire);
            if (hb != 0 && (now - hb) >= ORPHAN_THRESHOLD_CYCLES)
                stuck++;
        }
        
        if (stuck == 0) continue;
        
        /* Exponential growth: add 50% of current total each scale event */
        size_t total_workers = g_sched.num_workers + current;
        size_t to_spawn = total_workers / 2;  /* grow by 50% */
        if (to_spawn < 1) to_spawn = 1;
        if (to_spawn > (MAX_EXTRA_WORKERS - current))
            to_spawn = MAX_EXTRA_WORKERS - current;
        
        if (to_spawn == 0) continue;
        
        /* Rate limit overall scaling bursts */
        uint64_t last = atomic_load_explicit(&g_sched.last_promotion_cycles, memory_order_acquire);
        if ((now - last) < ORPHAN_COOLDOWN_CYCLES) continue;
        if (!atomic_compare_exchange_strong(&g_sched.last_promotion_cycles, &last, now))
            continue;
        
        /* Spawn all needed workers at once */
#ifdef CC_DEBUG_SYSMON
        fprintf(stderr, "[sysmon] scaling: stuck=%zu, spawning %zu (total will be %zu)\n",
                stuck, to_spawn, total_workers + to_spawn);
#endif
        for (size_t s = 0; s < to_spawn; s++) {
            if (!atomic_compare_exchange_strong(&g_sched.temp_worker_count, &current, current + 1))
                break;
            current++;
            
            pthread_t tid;
            pthread_attr_t attr;
            pthread_attr_init(&attr);
            pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
            if (pthread_create(&tid, &attr, replacement_worker, NULL) == 0)
                atomic_fetch_add(&g_sched.promotion_count, 1);
            else
                atomic_fetch_sub(&g_sched.temp_worker_count, 1);
            pthread_attr_destroy(&attr);
        }
    }
    return NULL;
}

static inline void worker_run_fiber(fiber_task* f) {
    tls_current_fiber = f;
    fiber_resume(f);
    tls_current_fiber = NULL;
}

static inline fiber_task* worker_try_steal_one(int worker_id, uint64_t* rng_state) {
    if (g_sched.num_workers <= 1) return NULL;
    size_t victim = (size_t)(xorshift64(rng_state) % g_sched.num_workers);
    if ((int)victim == worker_id) return NULL;
    fiber_task* f = iq_pop(&g_sched.inbox_queues[victim]);
    if (f) return f;
    return lq_steal(&g_sched.local_queues[victim]);
}

static void* worker_main(void* arg) {
    int worker_id = (int)(size_t)arg;
    tls_worker_id = worker_id;
    local_queue* my_queue = &g_sched.local_queues[worker_id];
    fiber_task* batch[WORKER_BATCH_SIZE];
    fiber_task* steal_buf[STEAL_BATCH_SIZE];
    
    /* Initialize TSan fiber context for the scheduler thread.
     * This models user-space fiber switches for TSan. */
    if (!tls_tsan_sched_fiber) {
        tls_tsan_sched_fiber = TSAN_FIBER_CREATE();
        TSAN_FIBER_SWITCH(tls_tsan_sched_fiber);
    }

    /* Per-worker PRNG state for randomized stealing */
    uint64_t rng_state = (uint64_t)worker_id * 0x9E3779B97F4A7C15ULL + rdtsc();
    
    while (atomic_load_explicit(&g_sched.running, memory_order_acquire)) {
        /* Priority 1: Pop from local queue (no contention) */
        size_t count = 0;
        while (count < WORKER_BATCH_SIZE) {
            fiber_task* f = lq_pop(my_queue);
            if (!f) break;
            batch[count++] = f;
        }
        
        /* Priority 2: Pop from inbox queue */
        while (count < WORKER_BATCH_SIZE) {
            fiber_task* f = iq_pop(&g_sched.inbox_queues[worker_id]);
            if (!f) break;
            batch[count++] = f;
        }
        
        /* Priority 3: Pop from global queue */
        while (count < WORKER_BATCH_SIZE) {
            fiber_task* f = g_sched.run_queue ? fq_pop(g_sched.run_queue) : NULL;
            if (!f) break;
            batch[count++] = f;
        }
        
        /* Priority 4: Batch steal from other workers with randomized victim selection */
        if (count == 0 && g_sched.num_workers > 1) {
            /* Randomize starting victim to avoid thundering herd */
            size_t start = (size_t)(xorshift64(&rng_state) % g_sched.num_workers);
            
            for (size_t j = 0; j < g_sched.num_workers; j++) {
                size_t victim = (start + j) % g_sched.num_workers;
                if ((int)victim == worker_id) continue;
                
                fiber_task* inbox_task = iq_pop(&g_sched.inbox_queues[victim]);
                if (inbox_task) {
                    batch[count++] = inbox_task;
                    break;  /* Got work, stop stealing */
                }
                
                /* Batch steal: take up to half the victim's queue */
                size_t stolen = lq_steal_batch(&g_sched.local_queues[victim], 
                                               steal_buf, STEAL_BATCH_SIZE);
                if (stolen > 0) {
                    /* Execute first task immediately, put rest in batch or local queue */
                    batch[count++] = steal_buf[0];
                    for (size_t s = 1; s < stolen && count < WORKER_BATCH_SIZE; s++) {
                        batch[count++] = steal_buf[s];
                    }
                    /* Any remaining stolen tasks go to our local queue */
                    for (size_t s = count; s < stolen; s++) {
                        if (lq_push(my_queue, steal_buf[s]) != 0) {
                            /* Local queue full, put overflow in global */
                            fq_push(g_sched.run_queue, steal_buf[s]);
                        }
                    }
                    break;  /* Got work, stop stealing */
                }
            }
        }
        
        if (count > 0) {
            /* Update heartbeat when executing work (sysmon checks for stuck workers) */
            atomic_store_explicit(&g_sched.worker_heartbeat[worker_id].heartbeat, rdtsc(), memory_order_relaxed);
            
            /* Batch execute */
            for (size_t i = 0; i < count; i++) {
                worker_run_fiber(batch[i]);
            }
            continue;
        }
        
        /* No work - enter spinning state */
        atomic_fetch_add_explicit(&g_sched.spinning, 1, memory_order_relaxed);
        
        /* Spin briefly checking local, global, and stealing */
        for (int spin = 0; spin < SPIN_FAST_ITERS; spin++) {
            fiber_task* f = lq_pop(my_queue);
            if (!f) f = iq_pop(&g_sched.inbox_queues[worker_id]);
            if (!f && g_sched.run_queue) f = fq_pop(g_sched.run_queue);
            /* Try to steal every 16 spins */
            if (!f && (spin & 15) == 15) {
                f = worker_try_steal_one(worker_id, &rng_state);
            }
            if (f) {
                atomic_fetch_sub_explicit(&g_sched.spinning, 1, memory_order_relaxed);
                worker_run_fiber(f);
                goto next_iteration;
            }
            cpu_pause();
        }
        
        /* Yield a few times before sleeping, with steal attempts */
        for (int y = 0; y < SPIN_YIELD_ITERS; y++) {
            sched_yield();
            fiber_task* f = lq_pop(my_queue);
            if (!f) f = iq_pop(&g_sched.inbox_queues[worker_id]);
            if (!f && g_sched.run_queue) f = fq_pop(g_sched.run_queue);
            /* Try to steal every 4 yields */
            if (!f && (y & 3) == 3) {
                f = worker_try_steal_one(worker_id, &rng_state);
            }
            if (f) {
                atomic_fetch_sub_explicit(&g_sched.spinning, 1, memory_order_relaxed);
                worker_run_fiber(f);
                goto next_iteration;
            }
        }
        
        atomic_fetch_sub_explicit(&g_sched.spinning, 1, memory_order_relaxed);
        
        /* One last steal attempt before sleeping */
        if (g_sched.num_workers > 1) {
            for (size_t j = 0; j < g_sched.num_workers; j++) {
                fiber_task* f = worker_try_steal_one(worker_id, &rng_state);
                if (f) {
                    worker_run_fiber(f);
                    goto next_iteration;
                }
            }
        }
        
        /* Sleep using fast wake primitive (futex/ulock instead of condvar) */
        /* Note: we check queue emptiness, not pending count, because parked fibers
         * are "pending" but not runnable. We wake when new runnable work is added. */
        atomic_fetch_add_explicit(&g_sched.sleeping, 1, memory_order_release);
        
        /* Check for deadlock: all workers sleeping + parked fibers + no runnable work */
        cc__fiber_check_deadlock();
        
        uint32_t wake_val = atomic_load_explicit(&g_sched.wake_prim.value, memory_order_acquire);
        /* Sleep while no runnable work (check local queue, global queue) and still running.
         * Wake primitive is signaled when fibers are spawned or unparked.
         * Periodically wake to check for deadlock (every ~500ms). */
        while (atomic_load_explicit(&g_sched.running, memory_order_relaxed)) {
            /* Check if there's runnable work */
            if (lq_peek(my_queue) || iq_peek(&g_sched.inbox_queues[worker_id]) ||
                (g_sched.run_queue && fq_peek(g_sched.run_queue))) break;
            /* Check for stealable work in other queues */
            int found_stealable = 0;
            for (size_t i = 0; i < g_sched.num_workers; i++) {
                if ((int)i != worker_id &&
                    (lq_peek(&g_sched.local_queues[i]) || iq_peek(&g_sched.inbox_queues[i]))) {
                    found_stealable = 1;
                    break;
                }
            }
            if (found_stealable) break;
            /* Use timed wait to periodically check for deadlock */
            wake_primitive_wait_timeout(&g_sched.wake_prim, wake_val, 500);
            wake_val = atomic_load_explicit(&g_sched.wake_prim.value, memory_order_acquire);
            cc__fiber_check_deadlock();
        }
        atomic_fetch_sub_explicit(&g_sched.sleeping, 1, memory_order_relaxed);
        
        next_iteration:;
    }
    
    tls_worker_id = -1;
    return NULL;
}

/* ============================================================================
 * Public API
 * ============================================================================ */

static void cc_fiber_atexit_stats(void) {
    if (atomic_load(&g_initialized) != 2) return;
    
    if (getenv("CC_FIBER_STATS")) {
        cc_fiber_dump_spawn_stats();
    }
    if (getenv("CC_SPAWN_TIMING")) {
        cc_fiber_dump_timing();
        cc_nursery_dump_timing();
    }
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
            /* Start at 1x cores, auto-scale to 2x for CPU-bound work */
            num_workers = n > 0 ? (size_t)n : 4;
            #endif
        }
    }
    if (num_workers > MAX_WORKERS) num_workers = MAX_WORKERS;
    
    memset(&g_sched, 0, sizeof(g_sched));
    g_sched.num_workers = num_workers;
    atomic_store(&g_sched.running, 1);
    g_sched.run_queue = (fiber_queue*)calloc(1, sizeof(fiber_queue));
    g_sched.local_queues = (local_queue*)calloc(num_workers, sizeof(local_queue));
    g_sched.inbox_queues = (inbox_queue*)calloc(num_workers, sizeof(inbox_queue));
    g_sched.workers = (pthread_t*)calloc(num_workers, sizeof(pthread_t));
    g_sched.worker_parked = (_Atomic size_t*)calloc(num_workers, sizeof(_Atomic size_t));
    g_sched.worker_heartbeat = (typeof(g_sched.worker_heartbeat))calloc(
        num_workers, sizeof(*g_sched.worker_heartbeat));
    if (!g_sched.run_queue || !g_sched.local_queues || !g_sched.inbox_queues ||
        !g_sched.workers || !g_sched.worker_parked || !g_sched.worker_heartbeat) {
        free(g_sched.run_queue);
        free(g_sched.local_queues);
        free(g_sched.inbox_queues);
        free(g_sched.workers);
        free(g_sched.worker_parked);
        free(g_sched.worker_heartbeat);
        fprintf(stderr, "[cc] fiber scheduler init failed: out of memory\n");
        abort();
    }
    wake_primitive_init(&g_sched.wake_prim);
    
    if (getenv("CC_FIBER_STATS") || getenv("CC_VERBOSE")) {
        fprintf(stderr, "[cc] fiber scheduler initialized with %zu workers\n", num_workers);
    }
    
    for (size_t i = 0; i < num_workers; i++) {
        pthread_create(&g_sched.workers[i], NULL, worker_main, (void*)i);
    }

    if (num_workers > 1) {
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
    
    /* Dump spawn timing stats if env var is set */
    if (getenv("CC_FIBER_STATS")) {
        cc_fiber_dump_spawn_stats();
    }
    if (getenv("CC_SPAWN_TIMING")) {
        cc_fiber_dump_timing();
        cc_nursery_dump_timing();
    }
    
    atomic_store_explicit(&g_sched.running, 0, memory_order_release);
    atomic_store_explicit(&g_sched.sysmon_running, 0, memory_order_release);
    wake_primitive_wake_all(&g_sched.wake_prim);

    if (g_sched.sysmon_started) {
        pthread_join(g_sched.sysmon_thread, NULL);
        g_sched.sysmon_started = 0;
    }
    for (size_t i = 0; i < g_sched.num_workers; i++) {
        pthread_join(g_sched.workers[i], NULL);
    }
    
    free(g_sched.workers);
    free(g_sched.local_queues);
    free(g_sched.inbox_queues);
    free(g_sched.worker_parked);
    free(g_sched.worker_heartbeat);
    free(g_sched.run_queue);
    g_sched.workers = NULL;
    g_sched.local_queues = NULL;
    g_sched.inbox_queues = NULL;
    g_sched.worker_parked = NULL;
    g_sched.worker_heartbeat = NULL;
    g_sched.run_queue = NULL;

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
    int timing = spawn_timing_enabled();
    uint64_t t0 = 0, t1, t2, t3, t4;
    
    if (timing) t0 = rdtsc();
    
    if (atomic_load_explicit(&g_initialized, memory_order_acquire) != 2) {
        cc_fiber_sched_init(0);
    }
    
    fiber_task* f = fiber_alloc();
    if (!f) return NULL;
    
    if (timing) t1 = rdtsc();
    
    f->fn = fn;
    f->arg = arg;
    atomic_store(&f->state, FIBER_READY);
    
    /* TSan release: establish synchronization with acquire in fiber_entry */
    TSAN_RELEASE(arg);
    
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
            
            /* Reset context: just the register state, no memset needed */
            _mco_ctxbuf* ctx = &((_mco_context*)co->context)->ctx;
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
        mco_result res = mco_create(&f->coro, &desc);
        if (res != MCO_SUCCESS) {
            fiber_free(f);
            return NULL;
        }
    }
    
    if (timing) t2 = rdtsc();
    
    /* Workers use round-robin to inbox queues for even spread.
     * If target is self, use local queue for the fast path. */
    int pushed_local = 0;
    int pushed_inbox = 0;
    static _Atomic size_t spawn_counter = 0;
    size_t target = atomic_fetch_add_explicit(&spawn_counter, 1, memory_order_relaxed) % g_sched.num_workers;
    if (tls_worker_id >= 0 && (size_t)tls_worker_id == target) {
        if (lq_push(&g_sched.local_queues[target], f) == 0) {
            pushed_local = 1;
        }
    } else {
        if (iq_push(&g_sched.inbox_queues[target], f) == 0) {
            pushed_inbox = 1;
            pushed_local = 1;
        }
    }
    
    /* Non-worker spawns or local queue full: use global queue */
    if (!pushed_local) {
        if (fq_push(g_sched.run_queue, f) != 0) {
            fiber_free(f);
            return NULL;
        }
    }
    
    if (timing) t3 = rdtsc();
    
    atomic_fetch_add_explicit(&g_sched.pending, 1, memory_order_relaxed);
    
    /* Wake a sleeping worker if any are sleeping and none are spinning */
    if (pushed_inbox) {
        /* Inbox enqueue may target a sleeping worker unrelated to the current
         * spawner, so we must wake unconditionally to avoid inbox starvation. */
        wake_one_if_sleeping_unconditional(timing);
    } else {
        wake_one_if_sleeping(timing);
    }
    
    if (timing) {
        t4 = rdtsc();
        atomic_fetch_add_explicit(&g_spawn_timing.alloc_cycles, t1 - t0, memory_order_relaxed);
        atomic_fetch_add_explicit(&g_spawn_timing.coro_cycles, t2 - t1, memory_order_relaxed);
        atomic_fetch_add_explicit(&g_spawn_timing.push_cycles, t3 - t2, memory_order_relaxed);
        atomic_fetch_add_explicit(&g_spawn_timing.wake_cycles, t4 - t3, memory_order_relaxed);
        atomic_fetch_add_explicit(&g_spawn_timing.total_cycles, t4 - t0, memory_order_relaxed);
        atomic_fetch_add_explicit(&g_spawn_timing.count, 1, memory_order_relaxed);
    }
    
    /* Track reuse stats (only when CC_FIBER_STATS is set) */
    static int stats_enabled = -1;
    if (stats_enabled < 0) stats_enabled = getenv("CC_FIBER_STATS") != NULL;
    if (stats_enabled) {
        if (reused) {
            atomic_fetch_add_explicit(&g_sched.coro_reused, 1, memory_order_relaxed);
        } else {
            atomic_fetch_add_explicit(&g_sched.coro_created, 1, memory_order_relaxed);
        }
    }
    
    return f;
}

/* Helper: wait for fiber to fully finish executing.
 * This is needed because done=1 is set while fiber_entry is still running,
 * but we can't return the fiber to the pool until fiber_entry has completed.
 * 
 * We MUST always wait for running_lock=0, even from fiber context. The previous
 * assumption that "if we're executing, the child's mco_resume must have returned"
 * is only true if we're on the SAME worker. If the joiner and child are on
 * different workers, the child's mco_resume might not have returned yet.
 * Returning the fiber to the pool prematurely causes tls_current_fiber to
 * point to a reused fiber with a different coro, leading to stack mismatch. */
static inline void wait_for_fiber_done_state(fiber_task* f) {
    if (!f) return;
    
    /* Wait for state=FIBER_DONE */
    for (int i = 0; i < 1000; i++) {
        if (atomic_load_explicit(&f->state, memory_order_acquire) == FIBER_DONE) {
            goto state_done;
        }
        cpu_pause();
    }
    while (atomic_load_explicit(&f->state, memory_order_acquire) != FIBER_DONE) {
        sched_yield();
    }
    
state_done:
    /* Always wait for running_lock=0 - the child's worker must finish mco_resume
     * before we can safely return the fiber to the pool. */
    for (int i = 0; i < 10000; i++) {
        if (atomic_load_explicit(&f->running_lock, memory_order_acquire) == 0) {
            return;
        }
        cpu_pause();
    }
    while (atomic_load_explicit(&f->running_lock, memory_order_acquire) != 0) {
        sched_yield();
    }
}

int cc_fiber_join(fiber_task* f, void** out_result) {
    if (!f) return -1;
    
    /* Get current fiber context (if any) - affects whether we park or use condvar */
    fiber_task* current = tls_current_fiber;
    if (join_debug_enabled()) {
        fprintf(stderr,
                "[join] start: target=%lu current=%s done=%d waiters=%d state=%d\n",
                (unsigned long)f->fiber_id,
                current ? "fiber" : "thread",
                atomic_load_explicit(&f->done, memory_order_relaxed),
                atomic_load_explicit(&f->join_waiters, memory_order_relaxed),
                atomic_load_explicit(&f->state, memory_order_relaxed));
    }
    
    /* Fast path - already done */
    if (atomic_load_explicit(&f->done, memory_order_acquire)) {
        wait_for_fiber_done_state(f);
        if (out_result) *out_result = f->result;
        return 0;
    }
    
    /* Spin for fast tasks (32 iterations with cpu_pause) */
    for (int i = 0; i < SPIN_FAST_ITERS; i++) {
        if (atomic_load_explicit(&f->done, memory_order_acquire)) {
            wait_for_fiber_done_state(f);
            if (out_result) *out_result = f->result;
            return 0;
        }
        cpu_pause();
    }
    
    /* Medium path: spin with sched_yield */
    for (int i = 0; i < SPIN_YIELD_ITERS; i++) {
        if (atomic_load_explicit(&f->done, memory_order_acquire)) {
            wait_for_fiber_done_state(f);
            if (out_result) *out_result = f->result;
            return 0;
        }
        sched_yield();
    }
    
    /* Register as waiter */
    atomic_fetch_add_explicit(&f->join_waiters, 1, memory_order_acq_rel);
    if (join_debug_enabled()) {
        fprintf(stderr,
                "[join] registered: target=%lu waiters=%d done=%d\n",
                (unsigned long)f->fiber_id,
                atomic_load_explicit(&f->join_waiters, memory_order_relaxed),
                atomic_load_explicit(&f->done, memory_order_relaxed));
    }
    
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
        if (join_debug_enabled()) {
            fprintf(stderr,
                    "[join] waiter_set: target=%lu waiter=%lu\n",
                    (unsigned long)f->fiber_id,
                    (unsigned long)current->fiber_id);
        }
        
        /* Now park until woken. At this point, either:
         * 1. Child hasn't completed yet - will see our registration and unpark us
         * 2. Child completed while we held lock - done=1, handled above */
        while (!atomic_load_explicit(&f->done, memory_order_acquire)) {
            /* Check if unpark already happened before we try to park */
            if (atomic_exchange_explicit(&current->unpark_pending, 0, memory_order_acq_rel)) {
                continue;  /* Already woken, re-check done flag */
            }
            
            atomic_store_explicit(&current->state, FIBER_PARKED, memory_order_release);
            
            /* Check again after setting PARKED - race window closed */
            if (atomic_exchange_explicit(&current->unpark_pending, 0, memory_order_acq_rel)) {
                atomic_store_explicit(&current->state, FIBER_RUNNING, memory_order_release);
                continue;  /* Already woken, re-check done flag */
            }
            
            /* Final check for done flag after setting PARKED */
            if (atomic_load_explicit(&f->done, memory_order_acquire)) {
                atomic_store_explicit(&current->state, FIBER_RUNNING, memory_order_release);
                break;
            }
            
            /* Re-check state - if child already unparked us (changed PARKED->READY),
             * don't yield. */
            int cur_state = atomic_load_explicit(&current->state, memory_order_acquire);
            if (cur_state != FIBER_PARKED) {
                /* Child already changed our state - we were unparked */
                atomic_store_explicit(&current->state, FIBER_RUNNING, memory_order_release);
                continue;
            }
            
            /* Full memory barrier + final done check before committing to yield.
             * This ensures we see the latest f->done value after all our stores
             * (including state=PARKED) are visible to other threads. */
            atomic_thread_fence(memory_order_seq_cst);
            if (atomic_load_explicit(&f->done, memory_order_seq_cst)) {
                atomic_store_explicit(&current->state, FIBER_RUNNING, memory_order_release);
                break;
            }
            
            /* Per-worker parked count - no global contention */
            int wid = tls_worker_id;
            if (wid >= 0) {
                atomic_fetch_add_explicit(&g_sched.worker_parked[wid], 1, memory_order_relaxed);
            }
            mco_yield(current->coro);
            if (join_debug_enabled()) {
                fprintf(stderr,
                        "[join] resumed: target=%lu waiter=%lu done=%d\n",
                        (unsigned long)f->fiber_id,
                        (unsigned long)current->fiber_id,
                        atomic_load_explicit(&f->done, memory_order_relaxed));
            }
            if (wid >= 0) {
                atomic_fetch_sub_explicit(&g_sched.worker_parked[wid], 1, memory_order_relaxed);
            }
            atomic_store_explicit(&current->state, FIBER_RUNNING, memory_order_release);
        }
    } else {
        /* Not in fiber context - use condvar (safe to block thread) */
        
        /* Lazy init condvar with CAS to avoid double init */
        int expected = 0;
        if (atomic_compare_exchange_strong_explicit(&f->join_cv_initialized, &expected, 1,
                                                     memory_order_acq_rel, memory_order_acquire)) {
            /* We won the race - initialize the condvar */
            pthread_mutex_init(&f->join_mu, NULL);
            pthread_cond_init(&f->join_cv, NULL);
        }
        /* else: another thread already initialized it */
        
        /* Re-check done after init - fiber may have completed during init window */
        if (atomic_load_explicit(&f->done, memory_order_acquire)) {
            atomic_fetch_sub_explicit(&f->join_waiters, 1, memory_order_relaxed);
            wait_for_fiber_done_state(f);
            if (out_result) *out_result = f->result;
            return 0;
        }
        
        pthread_mutex_lock(&f->join_mu);
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

/* ============================================================================
 * Fiber Parking (for channel blocking)
 * ============================================================================ */

int cc__fiber_in_context(void) {
    return tls_current_fiber != NULL;
}

void* cc__fiber_current(void) {
    return tls_current_fiber;
}

/* Get pointer to fiber-local result buffer (48 bytes).
 * Use this to store task results without malloc.
 * Returns NULL if not in fiber context. */
void* cc_task_result_ptr(size_t size) {
    if (!tls_current_fiber || size > sizeof(tls_current_fiber->result_buf)) {
        return NULL;
    }
    return tls_current_fiber->result_buf;
}

#ifdef CC_DEBUG_DEADLOCK
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
#endif

void cc__fiber_park_reason(const char* reason, const char* file, int line) {
    fiber_task* f = tls_current_fiber;
    if (!f || !f->coro) return;
    
    /* CRITICAL: Verify we're on the correct stack before parking.
     * This check catches a rare race where tls_current_fiber might not match
     * the actually executing coroutine. Instead of crashing with a stack overflow
     * in mco_yield, we detect and handle it gracefully. */
    volatile size_t _stack_check;
    size_t stack_addr = (size_t)&_stack_check;
    size_t stack_min = (size_t)f->coro->stack_base;
    size_t stack_max = stack_min + f->coro->stack_size;
    if (stack_addr < stack_min || stack_addr > stack_max) {
#ifdef CC_DEBUG_FIBER
        fprintf(stderr, "[CC DEBUG] cc__fiber_park: tls_current_fiber mismatch, skipping park\n");
#endif
        /* Don't park - the fiber state will be handled by the caller or timeout */
        return;
    }
    
    /* Store debug info */
    f->park_reason = reason;
    f->park_file = file;
    f->park_line = line;
    
    /* If an unpark raced before we try to park, skip parking. */
    if (atomic_exchange_explicit(&f->unpark_pending, 0, memory_order_acq_rel)) {
        return;
    }

    atomic_store_explicit(&f->state, FIBER_PARKED, memory_order_release);

    /* If an unpark raced after we set PARKED but before we yield, skip parking. */
    if (atomic_exchange_explicit(&f->unpark_pending, 0, memory_order_acq_rel)) {
        atomic_store_explicit(&f->state, FIBER_RUNNING, memory_order_release);
        return;
    }

    /* Per-worker parked count - no global contention */
    int wid = tls_worker_id;
    if (wid >= 0) {
        atomic_fetch_add_explicit(&g_sched.worker_parked[wid], 1, memory_order_relaxed);
    }
#ifdef CC_DEBUG_DEADLOCK
    parked_list_add(f);
#endif
    
    /* Yield back to worker */
    mco_yield(f->coro);
    
    /* Resumed by unpark */
#ifdef CC_DEBUG_DEADLOCK
    parked_list_remove(f);
#endif
    if (wid >= 0) {
        atomic_fetch_sub_explicit(&g_sched.worker_parked[wid], 1, memory_order_relaxed);
    }
    atomic_store_explicit(&f->state, FIBER_RUNNING, memory_order_release);
    f->park_reason = NULL;
    f->park_file = NULL;
    f->park_line = 0;
}

void cc__fiber_park(void) {
    cc__fiber_park_reason("unknown", NULL, 0);
}

void cc__fiber_unpark(void* fiber_ptr) {
    fiber_task* f = (fiber_task*)fiber_ptr;
    if (!f) return;
    
    /* Spin-wait if fiber is being resumed */
    int spins = 0;
    while (atomic_load_explicit(&f->running_lock, memory_order_acquire)) {
        if (++spins > 1000) {
            spins = 0;
            sched_yield();
        }
        cpu_pause();
    }
    
    /* CAS: PARKED -> READY. If fiber isn't PARKED yet, set unpark_pending
     * so the upcoming park will skip sleeping. */
    int expected = FIBER_PARKED;
    if (!atomic_compare_exchange_strong_explicit(&f->state, &expected, FIBER_READY,
                                                  memory_order_acq_rel,
                                                  memory_order_acquire)) {
        if (expected == FIBER_DONE) {
            if (join_debug_enabled()) {
                fprintf(stderr,
                        "[join] unpark: fiber=%lu already done\n",
                        (unsigned long)f->fiber_id);
            }
            return;  /* Already completed, nothing to do */
        }
        /* Fiber is READY (executing) or RUNNING - record pending wake.
         * This handles the race where the fiber is in cc_fiber_join about to park
         * but hasn't set PARKED yet. The fiber will check unpark_pending before
         * actually yielding. */
        atomic_store_explicit(&f->unpark_pending, 1, memory_order_release);
        if (join_debug_enabled()) {
            fprintf(stderr,
                    "[join] unpark: fiber=%lu state=%d pending=1\n",
                    (unsigned long)f->fiber_id,
                    expected);
        }
        return;
    }
    if (join_debug_enabled()) {
        fprintf(stderr,
                "[join] unpark: fiber=%lu -> READY\n",
                (unsigned long)f->fiber_id);
    }

    /* Re-enqueue to LOCAL queue if we're in a worker thread.
     * This provides better cache locality for ping-pong patterns where
     * the unparker is likely to be the fiber that will process the response.
     * Work stealing naturally redistributes work if one queue gets overloaded. */
    int pushed_local = 0;
    if (tls_worker_id >= 0) {
        if (lq_push(&g_sched.local_queues[tls_worker_id], f) == 0) {
            pushed_local = 1;
        }
    }
    
    /* Fallback to global queue if not in worker context or local queue is full */
    if (!pushed_local) {
        fq_push_blocking(g_sched.run_queue, f);
    }
    
    /* Wake ONE sleeping worker - work stealing will naturally distribute load.
     * Using wake_one instead of wake_all reduces wake overhead and avoids
     * thundering herd. If more workers are needed, they'll wake when they
     * find stealable work in other queues. */
    wake_one_if_sleeping_unconditional(0);
}

void cc__fiber_sched_enqueue(void* fiber_ptr) {
    cc__fiber_unpark(fiber_ptr);
}

/* Cooperative yield: give other fibers a chance to run.
 * Re-enqueues current fiber and switches to scheduler.
 * Used for fairness in producer-consumer patterns. */
void cc__fiber_yield(void) {
    fiber_task* current = tls_current_fiber;
    if (!current || !current->coro) {
        /* Not in fiber context - OS yield */
        sched_yield();
        return;
    }
    
    /* Push self back onto run queue */
    if (tls_worker_id >= 0) {
        if (lq_push(&g_sched.local_queues[tls_worker_id], current) != 0) {
            /* Local queue full, use global */
            fq_push_blocking(g_sched.run_queue, current);
        }
    } else {
        fq_push_blocking(g_sched.run_queue, current);
    }
    
    /* Yield to scheduler - will pick up next runnable fiber */
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
