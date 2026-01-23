/*
 * Fiber Scheduler - M:N userspace threading using minicoro
 * 
 * Design:
 *   - Fibers are minicoro coroutines with their own stacks
 *   - N worker threads run M fibers cooperatively
 *   - Blocking operations park the fiber, not the thread
 *   - Worker immediately picks up next runnable fiber
 *
 * This enables high-performance channel operations without kernel syscalls.
 */

#define MINICORO_IMPL
#include "minicoro.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sched.h>

/* ============================================================================
 * Configuration
 * ============================================================================ */

#ifndef CC_FIBER_WORKERS
#define CC_FIBER_WORKERS 0  /* 0 = detect at runtime */
#endif

#ifndef CC_FIBER_STACK_SIZE
#define CC_FIBER_STACK_SIZE (128 * 1024)  /* 128KB per fiber */
#endif

#ifndef CC_FIBER_QUEUE_SIZE
#define CC_FIBER_QUEUE_SIZE 65536  /* 64K slots to handle large workloads */
#endif

/* Debug tracing - compile with -DCC_FIBER_DEBUG_TRACE=1 to enable */
#ifndef CC_FIBER_DEBUG_TRACE
#define CC_FIBER_DEBUG_TRACE 0
#endif

#if CC_FIBER_DEBUG_TRACE
#define FIBER_TRACE(fmt, ...) fprintf(stderr, "[FIBER:%lu] " fmt "\n", (unsigned long)pthread_self(), ##__VA_ARGS__)
#else
#define FIBER_TRACE(fmt, ...) ((void)0)
#endif

#define MAX_WORKERS 64

static inline void cpu_pause(void) {
    #if defined(__x86_64__) || defined(_M_X64)
    __asm__ volatile("pause");
    #elif defined(__aarch64__) || defined(__arm64__)
    __asm__ volatile("yield");
    #endif
}

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

typedef struct {
    pthread_t workers[MAX_WORKERS];
    size_t num_workers;
    _Atomic int running;
    
    fiber_queue run_queue;
    fiber_task* _Atomic free_list;
    
    pthread_mutex_t wake_mu;
    pthread_cond_t wake_cv;
    _Atomic size_t pending;
    
    /* Stats */
    _Atomic size_t active;
    _Atomic size_t sleeping;
    _Atomic size_t parked;
    _Atomic size_t completed;
} fiber_sched;

static fiber_sched g_sched = {0};
static _Atomic int g_initialized = 0;

/* Per-worker state */
static __thread fiber_task* tls_current_fiber = NULL;

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

/* ============================================================================
 * Fiber Pool
 * ============================================================================ */

static fiber_task* fiber_alloc(void) {
    fiber_task* f = atomic_load_explicit(&g_sched.free_list, memory_order_acquire);
    while (f) {
        fiber_task* next = f->next;
        if (atomic_compare_exchange_weak_explicit(&g_sched.free_list, &f, next,
                                                   memory_order_release,
                                                   memory_order_acquire)) {
            f->fn = NULL;
            f->arg = NULL;
            f->result = NULL;
            atomic_store(&f->state, FIBER_CREATED);
            atomic_store(&f->done, 0);
            atomic_store(&f->running_lock, 0);
            f->next = NULL;
            f->coro = NULL;
            return f;
        }
    }
    return (fiber_task*)calloc(1, sizeof(fiber_task));
}

static void fiber_free(fiber_task* f) {
    if (!f) return;
    if (f->coro) {
        mco_destroy(f->coro);
        f->coro = NULL;
    }
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
    
    /* Acquire running lock - this serializes resume with unpark.
       The CAS also detects double-resume which would be a bug. */
    int expected = 0;
    if (!atomic_compare_exchange_strong_explicit(&f->running_lock, &expected, 1,
                                                  memory_order_acq_rel,
                                                  memory_order_acquire)) {
        fiber_panic("double resume detected (fiber was enqueued twice)", f, MCO_INVALID_OPERATION);
    }
    
    mco_state st = mco_status(f->coro);
    if (st != MCO_SUSPENDED) {
        atomic_store_explicit(&f->running_lock, 0, memory_order_release);
        fiber_panic("coroutine not in suspended state", f, MCO_NOT_SUSPENDED);
    }
    
    mco_result res = mco_resume(f->coro);
    
    /* Release running lock - unparks waiting on this will now proceed */
    atomic_store_explicit(&f->running_lock, 0, memory_order_release);
    
    if (res != MCO_SUCCESS) {
        fiber_panic("mco_resume failed", f, res);
    }
}

static void* worker_main(void* arg) {
    (void)arg;
    
    while (atomic_load_explicit(&g_sched.running, memory_order_acquire)) {
        fiber_task* f = fq_pop(&g_sched.run_queue);
        
        if (f) {
            tls_current_fiber = f;
            atomic_fetch_add_explicit(&g_sched.active, 1, memory_order_relaxed);
            atomic_store_explicit(&f->state, FIBER_RUNNING, memory_order_release);
            
            /* Resume the fiber with error checking */
            fiber_resume(f);
            
            atomic_fetch_sub_explicit(&g_sched.active, 1, memory_order_relaxed);
            tls_current_fiber = NULL;
            
            /* Fiber is now either:
             * - FIBER_DONE: completed, owned by nursery
             * - FIBER_PARKED: waiting, will be re-queued by cc__fiber_unpark
             * - FIBER_READY: was parked and already unparked (race), already in queue
             * We do NOT re-queue here - unpark handles re-queuing parked fibers.
             */
            continue;
        }
        
        /* Spin briefly then retry */
        for (int spin = 0; spin < 32; spin++) {
            cpu_pause();
        }
        
        f = fq_pop(&g_sched.run_queue);
        if (f) {
            /* Got work after spin - run it */
            tls_current_fiber = f;
            atomic_fetch_add_explicit(&g_sched.active, 1, memory_order_relaxed);
            atomic_store_explicit(&f->state, FIBER_RUNNING, memory_order_release);
            fiber_resume(f);
            atomic_fetch_sub_explicit(&g_sched.active, 1, memory_order_relaxed);
            tls_current_fiber = NULL;
            /* Don't re-queue - unpark handles it */
            continue;
        }
        
        if (atomic_load_explicit(&g_sched.pending, memory_order_relaxed) > 0) {
            sched_yield();
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
    }
    
    return NULL;
}

/* ============================================================================
 * Public API
 * ============================================================================ */

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
    
    for (size_t i = 0; i < num_workers; i++) {
        pthread_create(&g_sched.workers[i], NULL, worker_main, NULL);
    }
    
    atomic_store_explicit(&g_initialized, 2, memory_order_release);
    return 0;
}

void cc_fiber_sched_shutdown(void) {
    if (atomic_load_explicit(&g_initialized, memory_order_acquire) != 2) return;
    
    atomic_store_explicit(&g_sched.running, 0, memory_order_release);
    pthread_mutex_lock(&g_sched.wake_mu);
    pthread_cond_broadcast(&g_sched.wake_cv);
    pthread_mutex_unlock(&g_sched.wake_mu);
    
    for (size_t i = 0; i < g_sched.num_workers; i++) {
        pthread_join(g_sched.workers[i], NULL);
    }
    
    fiber_task* f = atomic_load(&g_sched.free_list);
    while (f) {
        fiber_task* next = f->next;
        if (f->coro) mco_destroy(f->coro);
        free(f);
        f = next;
    }
    
    pthread_mutex_destroy(&g_sched.wake_mu);
    pthread_cond_destroy(&g_sched.wake_cv);
    atomic_store(&g_initialized, 0);
}

fiber_task* cc_fiber_spawn(void* (*fn)(void*), void* arg) {
    if (atomic_load_explicit(&g_initialized, memory_order_acquire) != 2) {
        cc_fiber_sched_init(0);
    }
    
    fiber_task* f = fiber_alloc();
    if (!f) return NULL;
    
    f->fn = fn;
    f->arg = arg;
    atomic_store(&f->state, FIBER_READY);
    
    /* Create minicoro coroutine - pass stack size to get correct coro_size */
    mco_desc desc = mco_desc_init(fiber_entry, CC_FIBER_STACK_SIZE);
    desc.user_data = f;
    
    mco_result res = mco_create(&f->coro, &desc);
    if (res != MCO_SUCCESS) {
        fiber_free(f);
        return NULL;
    }
    
    if (fq_push(&g_sched.run_queue, f) != 0) {
        fiber_free(f);
        return NULL;
    }
    
    size_t prev = atomic_fetch_add_explicit(&g_sched.pending, 1, memory_order_release);
    if (prev == 0) {
        pthread_mutex_lock(&g_sched.wake_mu);
        pthread_cond_broadcast(&g_sched.wake_cv);
        pthread_mutex_unlock(&g_sched.wake_mu);
    }
    
    return f;
}

int cc_fiber_join(fiber_task* f, void** out_result) {
    if (!f) return -1;
    
    if (atomic_load_explicit(&f->done, memory_order_acquire)) {
        if (out_result) *out_result = f->result;
        return 0;
    }
    
    for (int i = 0; i < 100; i++) {
        if (atomic_load_explicit(&f->done, memory_order_acquire)) {
            if (out_result) *out_result = f->result;
            return 0;
        }
        cpu_pause();
    }
    
    while (!atomic_load_explicit(&f->done, memory_order_acquire)) {
        sched_yield();
    }
    
    if (out_result) *out_result = f->result;
    return 0;
}

void cc_fiber_task_free(fiber_task* f) {
    /* Fiber is freed when caller (nursery) is done with it */
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
    
    /* Yield back to worker - worker will not re-queue us */
    mco_yield(f->coro);
    
    /* Resumed by unpark */
    atomic_fetch_sub_explicit(&g_sched.parked, 1, memory_order_relaxed);
    atomic_store_explicit(&f->state, FIBER_RUNNING, memory_order_release);
}

void cc__fiber_unpark(void* fiber_ptr) {
    fiber_task* f = (fiber_task*)fiber_ptr;
    if (!f) return;
    
    /* Spin-wait if fiber is currently being resumed.
       This is simpler than the wake_pending mechanism and avoids races. */
    int spins = 0;
    while (atomic_load_explicit(&f->running_lock, memory_order_acquire)) {
        if (++spins > 1000) {
            spins = 0;
            sched_yield();
        }
        cpu_pause();
    }
    
    /* Use CAS to atomically transition PARKED -> READY.
       This prevents double-unpark race conditions. */
    int expected = FIBER_PARKED;
    if (!atomic_compare_exchange_strong_explicit(&f->state, &expected, FIBER_READY,
                                                  memory_order_acq_rel,
                                                  memory_order_acquire)) {
        return;  /* Not parked, or another thread already unparked */
    }
    
    /* Push to run queue - retry indefinitely if full */
    while (fq_push(&g_sched.run_queue, f) != 0) {
        sched_yield();  /* Queue full, yield and retry */
    }
    
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
