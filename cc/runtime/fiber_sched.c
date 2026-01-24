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
 * Spin-then-condvar wait primitive
 * ============================================================================ */

typedef struct {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    _Atomic int waiters;
} spin_condvar;

#define SPIN_CONDVAR_INIT { PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, 0 }

static inline void spin_condvar_init(spin_condvar* sc) {
    pthread_mutex_init(&sc->mu, NULL);
    pthread_cond_init(&sc->cv, NULL);
    atomic_store(&sc->waiters, 0);
}

static inline void spin_condvar_destroy(spin_condvar* sc) {
    pthread_mutex_destroy(&sc->mu);
    pthread_cond_destroy(&sc->cv);
}

/* Spin-then-condvar wait: spin for a bit, then fall back to condvar */
#define SPIN_FAST_ITERS 32
#define SPIN_YIELD_ITERS 64

static inline void spin_condvar_wait(spin_condvar* sc, _Atomic int* flag, int target) {
    /* Fast path: spin with cpu_pause */
    for (int i = 0; i < SPIN_FAST_ITERS; i++) {
        if (atomic_load_explicit(flag, memory_order_acquire) == target) return;
        cpu_pause();
    }
    
    /* Medium path: spin with sched_yield */
    for (int i = 0; i < SPIN_YIELD_ITERS; i++) {
        if (atomic_load_explicit(flag, memory_order_acquire) == target) return;
        sched_yield();
    }
    
    /* Slow path: condvar wait */
    atomic_fetch_add_explicit(&sc->waiters, 1, memory_order_relaxed);
    pthread_mutex_lock(&sc->mu);
    while (atomic_load_explicit(flag, memory_order_acquire) != target) {
        pthread_cond_wait(&sc->cv, &sc->mu);
    }
    pthread_mutex_unlock(&sc->mu);
    atomic_fetch_sub_explicit(&sc->waiters, 1, memory_order_relaxed);
}

/* Signal waiters if any are waiting on condvar */
static inline void spin_condvar_signal(spin_condvar* sc) {
    if (atomic_load_explicit(&sc->waiters, memory_order_relaxed) > 0) {
        pthread_mutex_lock(&sc->mu);
        pthread_cond_signal(&sc->cv);
        pthread_mutex_unlock(&sc->mu);
    }
}

static inline void spin_condvar_broadcast(spin_condvar* sc) {
    if (atomic_load_explicit(&sc->waiters, memory_order_relaxed) > 0) {
        pthread_mutex_lock(&sc->mu);
        pthread_cond_broadcast(&sc->cv);
        pthread_mutex_unlock(&sc->mu);
    }
}

/* ============================================================================
 * Configuration
 * ============================================================================ */

#ifndef CC_FIBER_WORKERS
#define CC_FIBER_WORKERS 0  /* 0 = detect at runtime */
#endif

#ifndef CC_FIBER_STACK_SIZE
#define CC_FIBER_STACK_SIZE (32 * 1024)  /* 32KB per fiber */
#endif

#ifndef CC_FIBER_QUEUE_SIZE
#define CC_FIBER_QUEUE_SIZE 65536  /* 64K slots to handle large workloads */
#endif

#define MAX_WORKERS 64

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
    mco_coro* coro;           /* minicoro coroutine handle */
    void* (*fn)(void*);       /* User function */
    void* arg;                /* User argument */
    void* result;             /* Return value */
    _Atomic int state;
    _Atomic int done;
    _Atomic int running_lock; /* Serialize resume/unpark */
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
    for (int retry = 0; retry < 1000; retry++) {
        size_t tail = atomic_load_explicit(&q->tail, memory_order_relaxed);
        size_t head = atomic_load_explicit(&q->head, memory_order_acquire);
        
        if (tail - head >= CC_FIBER_QUEUE_SIZE) {
            sched_yield();
            continue;
        }
        
        if (atomic_compare_exchange_weak_explicit(&q->tail, &tail, tail + 1,
                                                   memory_order_release,
                                                   memory_order_relaxed)) {
            atomic_store_explicit(&q->slots[tail % CC_FIBER_QUEUE_SIZE], f, memory_order_release);
            return 0;
        }
    }
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
    
    pthread_mutex_t wake_mu;
    pthread_cond_t wake_cv;
    _Atomic size_t pending;
    
    /* Global join condvar for waiters */
    spin_condvar join_cv;
    
    /* Track worker states for smarter waking */
    _Atomic size_t active;      /* Workers currently executing fibers */
    _Atomic size_t sleeping;    /* Workers blocked on condvar */
    _Atomic size_t spinning;    /* Workers actively polling (not sleeping yet) */
    
    /* Stats */
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
    atomic_store_explicit(&q->slots[tail % LOCAL_QUEUE_SIZE], f, memory_order_relaxed);
    atomic_store_explicit(&q->tail, tail + 1, memory_order_release);
    return 0;
}

/* Fast local queue pop (single consumer - owner) */
static inline fiber_task* lq_pop(local_queue* q) {
    size_t head = atomic_load_explicit(&q->head, memory_order_relaxed);
    size_t tail = atomic_load_explicit(&q->tail, memory_order_acquire);
    if (head >= tail) return NULL;  /* Empty */
    fiber_task* f = atomic_load_explicit(&q->slots[head % LOCAL_QUEUE_SIZE], memory_order_relaxed);
    atomic_store_explicit(&q->head, head + 1, memory_order_release);
    return f;
}

/* Work stealing: steal half from another worker's queue */
static inline fiber_task* lq_steal(local_queue* q) {
    size_t head = atomic_load_explicit(&q->head, memory_order_acquire);
    size_t tail = atomic_load_explicit(&q->tail, memory_order_acquire);
    if (head >= tail) return NULL;  /* Empty */
    fiber_task* f = atomic_load_explicit(&q->slots[head % LOCAL_QUEUE_SIZE], memory_order_acquire);
    if (!f) return NULL;
    if (atomic_compare_exchange_weak_explicit(&q->head, &head, head + 1,
                                               memory_order_release, memory_order_relaxed)) {
        return f;
    }
    return NULL;  /* Lost race */
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
            /* Reuse pooled fiber - reset state but KEEP the coro */
            f->fn = NULL;
            f->arg = NULL;
            f->result = NULL;
            atomic_store(&f->state, FIBER_CREATED);
            atomic_store(&f->done, 0);
            atomic_store(&f->running_lock, 0);
            f->next = NULL;
            /* f->coro is kept for reuse! */
            return f;
        }
    }
    return (fiber_task*)calloc(1, sizeof(fiber_task));
}

static void fiber_free(fiber_task* f) {
    if (!f) return;
    /* Keep the coro for pooling - don't destroy it! */
    fiber_task* head;
    do {
        head = atomic_load_explicit(&g_sched.free_list, memory_order_relaxed);
        f->next = head;
    } while (!atomic_compare_exchange_weak_explicit(&g_sched.free_list, &head, f,
                                                     memory_order_release,
                                                     memory_order_relaxed));
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

static void fiber_entry(mco_coro* co) {
    fiber_task* f = (fiber_task*)mco_get_user_data(co);
    if (f && f->fn) {
        f->result = f->fn(f->arg);
    }
    atomic_store_explicit(&f->state, FIBER_DONE, memory_order_release);
    atomic_store_explicit(&f->done, 1, memory_order_release);
    atomic_fetch_sub_explicit(&g_sched.pending, 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&g_sched.completed, 1, memory_order_relaxed);
    
    /* Signal any joiners waiting on condvar */
    spin_condvar_broadcast(&g_sched.join_cv);
    
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
        for (int spin = 0; spin < 64; spin++) {
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
        for (int y = 0; y < 4; y++) {
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
        
        /* Sleep on condvar */
        pthread_mutex_lock(&g_sched.wake_mu);
        atomic_fetch_add_explicit(&g_sched.sleeping, 1, memory_order_relaxed);
        while (atomic_load_explicit(&g_sched.pending, memory_order_relaxed) == 0 &&
               atomic_load_explicit(&g_sched.running, memory_order_relaxed)) {
            pthread_cond_wait(&g_sched.wake_cv, &g_sched.wake_mu);
        }
        atomic_fetch_sub_explicit(&g_sched.sleeping, 1, memory_order_relaxed);
        pthread_mutex_unlock(&g_sched.wake_mu);
        
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
    pthread_mutex_init(&g_sched.wake_mu, NULL);
    pthread_cond_init(&g_sched.wake_cv, NULL);
    spin_condvar_init(&g_sched.join_cv);
    
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
    pthread_mutex_lock(&g_sched.wake_mu);
    pthread_cond_broadcast(&g_sched.wake_cv);
    pthread_mutex_unlock(&g_sched.wake_mu);
    
    for (size_t i = 0; i < g_sched.num_workers; i++) {
        pthread_join(g_sched.workers[i], NULL);
    }
    
    /* Free pooled fibers (including their coros) */
    fiber_task* f = atomic_load(&g_sched.free_list);
    while (f) {
        fiber_task* next = f->next;
        if (f->coro) mco_destroy(f->coro);
        free(f);
        f = next;
    }
    
    pthread_mutex_destroy(&g_sched.wake_mu);
    pthread_cond_destroy(&g_sched.wake_cv);
    spin_condvar_destroy(&g_sched.join_cv);
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
    
    /* Reuse existing coro if available (pooling), otherwise create new */
    int reused = 0;
    if (f->coro) {
        mco_state st = mco_status(f->coro);
        if (st == MCO_DEAD) {
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
    
    atomic_fetch_add_explicit(&g_sched.pending, 1, memory_order_release);
    
    /* Wake a sleeping worker only if:
     * - We pushed to global queue (local queue will be picked up by owner)
     * - No workers are spinning */
    if (!pushed_local) {
        size_t spinning = atomic_load_explicit(&g_sched.spinning, memory_order_relaxed);
        if (spinning == 0) {
            size_t sleeping = atomic_load_explicit(&g_sched.sleeping, memory_order_relaxed);
            if (sleeping > 0) {
                pthread_mutex_lock(&g_sched.wake_mu);
                pthread_cond_signal(&g_sched.wake_cv);
                pthread_mutex_unlock(&g_sched.wake_mu);
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
    
    /* Slow path: condvar wait */
    atomic_fetch_add_explicit(&g_sched.join_cv.waiters, 1, memory_order_relaxed);
    pthread_mutex_lock(&g_sched.join_cv.mu);
    while (!atomic_load_explicit(&f->done, memory_order_acquire)) {
        pthread_cond_wait(&g_sched.join_cv.cv, &g_sched.join_cv.mu);
    }
    pthread_mutex_unlock(&g_sched.join_cv.mu);
    atomic_fetch_sub_explicit(&g_sched.join_cv.waiters, 1, memory_order_relaxed);
    
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
    
    atomic_store_explicit(&f->state, FIBER_PARKED, memory_order_release);
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
    
    /* CAS: PARKED -> READY */
    int expected = FIBER_PARKED;
    if (!atomic_compare_exchange_strong_explicit(&f->state, &expected, FIBER_READY,
                                                  memory_order_acq_rel,
                                                  memory_order_acquire)) {
        return;
    }
    
    /* Re-enqueue */
    while (fq_push(&g_sched.run_queue, f) != 0) {
        sched_yield();
    }
    
    /* Wake a worker if any are sleeping - unparked fibers need immediate attention */
    size_t sleeping = atomic_load_explicit(&g_sched.sleeping, memory_order_relaxed);
    if (sleeping > 0) {
        pthread_mutex_lock(&g_sched.wake_mu);
        pthread_cond_signal(&g_sched.wake_cv);
        pthread_mutex_unlock(&g_sched.wake_mu);
    }
}

void cc__fiber_sched_enqueue(void* fiber_ptr) {
    cc__fiber_unpark(fiber_ptr);
}

int cc__fiber_sched_active(void) {
    return atomic_load_explicit(&g_initialized, memory_order_acquire) == 2;
}
