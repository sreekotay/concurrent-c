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

static int g_spin_fast_iters = -1;   /* -1 = not initialized */
static int g_spin_yield_iters = -1;

static int get_spin_fast_iters(void) {
    if (g_spin_fast_iters < 0) {
        const char* env = getenv("CC_SPIN_FAST_ITERS");
        g_spin_fast_iters = env ? atoi(env) : SPIN_FAST_ITERS_DEFAULT;
        if (g_spin_fast_iters <= 0) g_spin_fast_iters = SPIN_FAST_ITERS_DEFAULT;
    }
    return g_spin_fast_iters;
}

static int get_spin_yield_iters(void) {
    if (g_spin_yield_iters < 0) {
        const char* env = getenv("CC_SPIN_YIELD_ITERS");
        g_spin_yield_iters = env ? atoi(env) : SPIN_YIELD_ITERS_DEFAULT;
        if (g_spin_yield_iters <= 0) g_spin_yield_iters = SPIN_YIELD_ITERS_DEFAULT;
    }
    return g_spin_yield_iters;
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
    _Atomic int state;
    _Atomic int done;
    _Atomic int running_lock; /* Serialize resume/unpark */
    _Atomic int unpark_pending; /* Wake happened before park */
    
    /* Per-fiber join synchronization */
    _Atomic int join_waiters;           /* Count of threads/fibers waiting to join */
    struct fiber_task* _Atomic join_waiter_fiber;  /* Single waiting fiber (common case) */
    pthread_mutex_t join_mu;            /* Mutex for join condvar (thread waiters) */
    pthread_cond_t join_cv;             /* Per-fiber condvar for thread waiters */
    int join_cv_initialized;            /* Lazy init flag for condvar */
    
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

typedef struct {
    fiber_task* _Atomic slots[LOCAL_QUEUE_SIZE];
    _Atomic size_t head;
    _Atomic size_t tail;
} local_queue;

typedef struct {
    pthread_t workers[MAX_WORKERS];
    size_t num_workers;
    _Atomic int running;
    
    fiber_queue run_queue;          /* Global queue for overflow and cross-worker */
    local_queue local_queues[MAX_WORKERS];  /* Per-worker local queues */
    fiber_task* _Atomic free_list;
    
    wake_primitive wake_prim;       /* Fast worker wake (futex/ulock instead of condvar) */
    _Atomic size_t pending;
    
    /* Track worker states for smarter waking - cache line padded to avoid false sharing */
    _Atomic size_t active;      /* Workers currently executing fibers */
    char _pad_active[CACHE_LINE_SIZE - sizeof(_Atomic size_t)];
    
    _Atomic size_t sleeping;    /* Workers blocked on condvar */
    char _pad_sleeping[CACHE_LINE_SIZE - sizeof(_Atomic size_t)];
    
    _Atomic size_t spinning;    /* Workers actively polling (not sleeping yet) */
    char _pad_spinning[CACHE_LINE_SIZE - sizeof(_Atomic size_t)];
    
    /* Stats - less hot, can share cache lines */
    _Atomic size_t parked;
    _Atomic size_t completed;
    _Atomic size_t coro_reused;
    _Atomic size_t coro_created;
} fiber_sched;

static fiber_sched g_sched = {0};
static _Atomic int g_initialized = 0;

/* Per-worker thread-local state */
static __thread fiber_task* tls_current_fiber = NULL;
static __thread int tls_worker_id = -1;  /* -1 = not a worker thread */

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

/* Dump scheduler state for debugging hangs */
void cc_fiber_dump_state(const char* reason) {
    fprintf(stderr, "\n=== FIBER SCHEDULER STATE: %s ===\n", reason ? reason : "");
    fprintf(stderr, "  pending=%zu active=%zu sleeping=%zu parked=%zu completed=%zu\n",
            atomic_load(&g_sched.pending),
            atomic_load(&g_sched.active),
            atomic_load(&g_sched.sleeping),
            atomic_load(&g_sched.parked),
            atomic_load(&g_sched.completed));
    fprintf(stderr, "  run_queue: head=%zu tail=%zu (approx %zu items)\n",
            atomic_load(&g_sched.run_queue.head),
            atomic_load(&g_sched.run_queue.tail),
            (atomic_load(&g_sched.run_queue.tail) - atomic_load(&g_sched.run_queue.head)) % CC_FIBER_QUEUE_SIZE);
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
            /* Reuse pooled fiber - reset state but KEEP the coro and join_cv */
            f->fn = NULL;
            f->arg = NULL;
            f->result = NULL;
            atomic_store(&f->state, FIBER_CREATED);
            atomic_store(&f->done, 0);
            atomic_store(&f->running_lock, 0);
            atomic_store(&f->unpark_pending, 0);
            atomic_store(&f->join_waiters, 0);
            atomic_store(&f->join_waiter_fiber, NULL);
            f->next = NULL;
            /* f->coro and f->join_cv are kept for reuse! */
            return f;
        }
    }
    
    /* Allocate new fiber */
    fiber_task* nf = (fiber_task*)calloc(1, sizeof(fiber_task));
    if (nf) {
        nf->join_cv_initialized = 0;
        atomic_store(&nf->join_waiters, 0);
        atomic_store(&nf->join_waiter_fiber, NULL);
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
    if (f->join_cv_initialized) {
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

/* TSan annotation to establish synchronization - tells TSan that this point
 * synchronizes with a corresponding __tsan_release on the same address. */
#if defined(__SANITIZE_THREAD__) || defined(__has_feature) && __has_feature(thread_sanitizer)
extern void __tsan_acquire(void* addr);
extern void __tsan_release(void* addr);
#define TSAN_ACQUIRE(addr) __tsan_acquire(addr)
#define TSAN_RELEASE(addr) __tsan_release(addr)
#else
#define TSAN_ACQUIRE(addr) ((void)0)
#define TSAN_RELEASE(addr) ((void)0)
#endif

static void fiber_entry(mco_coro* co) {
    fiber_task* f = (fiber_task*)mco_get_user_data(co);
    if (f && f->fn) {
        /* Acquire fence + TSan annotation ensures all writes by spawner 
         * (including closure captures) are visible before we execute. */
        atomic_thread_fence(memory_order_acquire);
        TSAN_ACQUIRE(f->arg);  /* Tell TSan: sync with release on same address */
        f->result = f->fn(f->arg);
    }
    atomic_store_explicit(&f->state, FIBER_DONE, memory_order_release);
    atomic_store_explicit(&f->done, 1, memory_order_release);
    atomic_fetch_sub_explicit(&g_sched.pending, 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&g_sched.completed, 1, memory_order_relaxed);
    
    /* Signal per-fiber joiners - targeted wakeup */
    if (atomic_load_explicit(&f->join_waiters, memory_order_acquire) > 0) {
        /* First, unpark any fiber waiter (common case for nested nurseries) */
        fiber_task* waiter = atomic_exchange_explicit(&f->join_waiter_fiber, NULL, memory_order_acq_rel);
        if (waiter) {
            cc__fiber_unpark(waiter);
        }
        
        /* Then signal thread waiters via condvar if initialized */
        if (atomic_load_explicit((_Atomic int*)&f->join_cv_initialized, memory_order_acquire)) {
            pthread_mutex_lock(&f->join_mu);
            pthread_cond_broadcast(&f->join_cv);
            pthread_mutex_unlock(&f->join_mu);
        }
    }
    
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
    
    mco_result res = mco_resume(f->coro);
    
    /* Release running lock */
    atomic_store_explicit(&f->running_lock, 0, memory_order_release);
    
    if (res != MCO_SUCCESS) {
        fiber_panic("mco_resume failed", f, res);
    }
}

#define WORKER_BATCH_SIZE 16

static void* worker_main(void* arg) {
    int worker_id = (int)(size_t)arg;
    tls_worker_id = worker_id;
    local_queue* my_queue = &g_sched.local_queues[worker_id];
    fiber_task* batch[WORKER_BATCH_SIZE];
    
    while (atomic_load_explicit(&g_sched.running, memory_order_acquire)) {
        /* Priority 1: Pop from local queue (no contention) */
        size_t count = 0;
        while (count < WORKER_BATCH_SIZE) {
            fiber_task* f = lq_pop(my_queue);
            if (!f) break;
            batch[count++] = f;
        }
        
        /* Priority 2: Pop from global queue */
        while (count < WORKER_BATCH_SIZE) {
            fiber_task* f = fq_pop(&g_sched.run_queue);
            if (!f) break;
            batch[count++] = f;
        }
        
        /* Priority 3: Steal from other workers */
        if (count == 0) {
            for (size_t i = 0; i < g_sched.num_workers && count < WORKER_BATCH_SIZE; i++) {
                if ((int)i == worker_id) continue;
                fiber_task* f = lq_steal(&g_sched.local_queues[i]);
                if (f) batch[count++] = f;
            }
        }
        
        if (count > 0) {
            /* Batch execute */
            for (size_t i = 0; i < count; i++) {
                fiber_task* f = batch[i];
                tls_current_fiber = f;
                fiber_resume(f);
            }
            tls_current_fiber = NULL;
            continue;
        }
        
        /* No work - enter spinning state */
        atomic_fetch_add_explicit(&g_sched.spinning, 1, memory_order_relaxed);
        
        /* Spin briefly checking local, global, and stealing */
        for (int spin = 0; spin < SPIN_FAST_ITERS; spin++) {
            fiber_task* f = lq_pop(my_queue);
            if (!f) f = fq_pop(&g_sched.run_queue);
            if (f) {
                atomic_fetch_sub_explicit(&g_sched.spinning, 1, memory_order_relaxed);
                tls_current_fiber = f;
                fiber_resume(f);
                tls_current_fiber = NULL;
                goto next_iteration;
            }
            cpu_pause();
        }
        
        /* Yield a few times before sleeping */
        for (int y = 0; y < SPIN_YIELD_ITERS; y++) {
            sched_yield();
            fiber_task* f = lq_pop(my_queue);
            if (!f) f = fq_pop(&g_sched.run_queue);
            if (f) {
                atomic_fetch_sub_explicit(&g_sched.spinning, 1, memory_order_relaxed);
                tls_current_fiber = f;
                fiber_resume(f);
                tls_current_fiber = NULL;
                goto next_iteration;
            }
        }
        
        atomic_fetch_sub_explicit(&g_sched.spinning, 1, memory_order_relaxed);
        
        /* Check if there's still pending work before sleeping */
        if (atomic_load_explicit(&g_sched.pending, memory_order_relaxed) > 0) {
            continue;
        }
        
        /* Sleep using fast wake primitive (futex/ulock instead of condvar) */
        atomic_fetch_add_explicit(&g_sched.sleeping, 1, memory_order_relaxed);
        uint32_t wake_val = atomic_load_explicit(&g_sched.wake_prim.value, memory_order_acquire);
        while (atomic_load_explicit(&g_sched.pending, memory_order_relaxed) == 0 &&
               atomic_load_explicit(&g_sched.running, memory_order_relaxed)) {
            wake_primitive_wait(&g_sched.wake_prim, wake_val);
            wake_val = atomic_load_explicit(&g_sched.wake_prim.value, memory_order_acquire);
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
        #if CC_FIBER_WORKERS > 0
        num_workers = CC_FIBER_WORKERS;
        #else
        long n = sysconf(_SC_NPROCESSORS_ONLN);
        num_workers = n > 0 ? (size_t)n : 4;
        #endif
    }
    if (num_workers > MAX_WORKERS) num_workers = MAX_WORKERS;
    
    memset(&g_sched, 0, sizeof(g_sched));
    g_sched.num_workers = num_workers;
    atomic_store(&g_sched.running, 1);
    wake_primitive_init(&g_sched.wake_prim);
    
    for (size_t i = 0; i < num_workers; i++) {
        pthread_create(&g_sched.workers[i], NULL, worker_main, (void*)i);
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
    wake_primitive_wake_all(&g_sched.wake_prim);
    
    for (size_t i = 0; i < g_sched.num_workers; i++) {
        pthread_join(g_sched.workers[i], NULL);
    }
    
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
        mco_state st = mco_status(f->coro);
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
    
    /* Try local queue first if we're inside a worker thread */
    int pushed_local = 0;
    if (tls_worker_id >= 0) {
        if (lq_push(&g_sched.local_queues[tls_worker_id], f) == 0) {
            pushed_local = 1;
        }
    }
    
    /* Fallback to global queue */
    if (!pushed_local) {
        if (fq_push(&g_sched.run_queue, f) != 0) {
            fiber_free(f);
            return NULL;
        }
    }
    
    if (timing) t3 = rdtsc();
    
    atomic_fetch_add_explicit(&g_sched.pending, 1, memory_order_relaxed);
    
    /* Wake a sleeping worker only if:
     * - We pushed to global queue (local queue will be picked up by owner)
     * - No workers are spinning */
    if (!pushed_local) {
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
    } else {
        if (timing) atomic_fetch_add_explicit(&g_spawn_timing.wake_skipped, 1, memory_order_relaxed);
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
    
    /* Track reuse stats (lightweight) */
    if (reused) {
        atomic_fetch_add_explicit(&g_sched.coro_reused, 1, memory_order_relaxed);
    } else {
        atomic_fetch_add_explicit(&g_sched.coro_created, 1, memory_order_relaxed);
    }
    
    return f;
}

int cc_fiber_join(fiber_task* f, void** out_result) {
    if (!f) return -1;
    
    /* Fast path - already done */
    if (atomic_load_explicit(&f->done, memory_order_acquire)) {
        if (out_result) *out_result = f->result;
        return 0;
    }
    
    /* Spin for fast tasks (32 iterations with cpu_pause) */
    for (int i = 0; i < SPIN_FAST_ITERS; i++) {
        if (atomic_load_explicit(&f->done, memory_order_acquire)) {
            if (out_result) *out_result = f->result;
            return 0;
        }
        cpu_pause();
    }
    
    /* Medium path: spin with sched_yield */
    for (int i = 0; i < SPIN_YIELD_ITERS; i++) {
        if (atomic_load_explicit(&f->done, memory_order_acquire)) {
            if (out_result) *out_result = f->result;
            return 0;
        }
        sched_yield();
    }
    
    /* Register as waiter */
    atomic_fetch_add_explicit(&f->join_waiters, 1, memory_order_acq_rel);
    
    /* Check again - fiber might have completed during registration */
    if (atomic_load_explicit(&f->done, memory_order_acquire)) {
        atomic_fetch_sub_explicit(&f->join_waiters, 1, memory_order_relaxed);
        if (out_result) *out_result = f->result;
        return 0;
    }
    
    /* Slow path: choose strategy based on context */
    fiber_task* current = tls_current_fiber;
    
    if (current && current->coro) {
        /* We're inside a fiber - PARK instead of blocking the worker thread.
         * This is critical for nested nurseries to avoid deadlock. */
        
        /* Store ourselves as the waiter fiber */
        fiber_task* expected = NULL;
        if (atomic_compare_exchange_strong_explicit(&f->join_waiter_fiber, &expected, current,
                                                     memory_order_acq_rel, memory_order_acquire)) {
            /* Successfully registered as fiber waiter - park until woken */
            while (!atomic_load_explicit(&f->done, memory_order_acquire)) {
                atomic_store_explicit(&current->state, FIBER_PARKED, memory_order_release);
                atomic_fetch_add_explicit(&g_sched.parked, 1, memory_order_relaxed);
                mco_yield(current->coro);
                atomic_fetch_sub_explicit(&g_sched.parked, 1, memory_order_relaxed);
                atomic_store_explicit(&current->state, FIBER_RUNNING, memory_order_release);
            }
        } else {
            /* Another fiber already waiting - fall back to spin-wait 
             * (rare case, and condvar would deadlock) */
            while (!atomic_load_explicit(&f->done, memory_order_acquire)) {
                sched_yield();
            }
        }
    } else {
        /* Not in fiber context - use condvar (safe to block thread) */
        
        /* Lazy init condvar */
        if (!f->join_cv_initialized) {
            pthread_mutex_init(&f->join_mu, NULL);
            pthread_cond_init(&f->join_cv, NULL);
            f->join_cv_initialized = 1;
        }
        
        pthread_mutex_lock(&f->join_mu);
        while (!atomic_load_explicit(&f->done, memory_order_acquire)) {
            pthread_cond_wait(&f->join_cv, &f->join_mu);
        }
        pthread_mutex_unlock(&f->join_mu);
    }
    
    atomic_fetch_sub_explicit(&f->join_waiters, 1, memory_order_relaxed);
    
    if (out_result) *out_result = f->result;
    return 0;
}

void cc_fiber_task_free(fiber_task* f) {
    if (f) {
        fiber_free(f);
    }
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

void cc__fiber_park(void) {
    fiber_task* f = tls_current_fiber;
    if (!f || !f->coro) return;
    
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

    atomic_fetch_add_explicit(&g_sched.parked, 1, memory_order_relaxed);
    
    /* Yield back to worker */
    mco_yield(f->coro);
    
    /* Resumed by unpark */
    atomic_fetch_sub_explicit(&g_sched.parked, 1, memory_order_relaxed);
    atomic_store_explicit(&f->state, FIBER_RUNNING, memory_order_release);
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
        if (expected == FIBER_READY) {
            return;
        }
        /* Fiber is still RUNNING; record pending wake and return. */
        atomic_store_explicit(&f->unpark_pending, 1, memory_order_release);
        return;
    }

    /* Re-enqueue */
    while (fq_push(&g_sched.run_queue, f) != 0) {
        sched_yield();
    }
    
    /* Wake a worker if any are sleeping - unparked fibers need immediate attention */
    size_t sleeping = atomic_load_explicit(&g_sched.sleeping, memory_order_relaxed);
    if (sleeping > 0) {
        wake_primitive_wake_one(&g_sched.wake_prim);
    }
}

void cc__fiber_sched_enqueue(void* fiber_ptr) {
    cc__fiber_unpark(fiber_ptr);
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
