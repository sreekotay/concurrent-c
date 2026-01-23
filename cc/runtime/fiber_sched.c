/*
 * Lightweight fiber scheduler for spawn/join operations.
 * Eliminates per-task mutex/cond overhead.
 * Features: work-stealing, per-worker local queues, task pooling.
 */

#include "fiber_internal.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ============================================================================
 * Configuration
 * ============================================================================ */

#define FIBER_SCHED_MAX_WORKERS 64
#define FIBER_SCHED_QUEUE_SIZE 4096
#define FIBER_SCHED_LOCAL_QUEUE_SIZE 256
#define FIBER_SCHED_STEAL_BATCH 8

/* ============================================================================
 * Fiber Task - lightweight task without per-task mutex/cond
 * ============================================================================ */

typedef struct fiber_task {
    void* (*fn)(void*);
    void* arg;
    void* result;
    _Atomic int done;           /* Atomic completion flag */
    _Atomic int waiting;        /* Number of waiters */
    struct fiber_task* next;    /* For free list */
} fiber_task;

/* ============================================================================
 * Work-stealing deque (Chase-Lev style)
 * Push/pop from bottom (owner), steal from top (thieves)
 * ============================================================================ */

typedef struct {
    fiber_task* _Atomic tasks[FIBER_SCHED_LOCAL_QUEUE_SIZE];
    _Atomic size_t top;     /* Thieves steal from here */
    _Atomic size_t bottom;  /* Owner pushes/pops here */
    char padding[64];       /* Cache line padding */
} work_stealing_deque;

/* Owner pushes to bottom */
static inline int deque_push(work_stealing_deque* q, fiber_task* t) {
    size_t bottom = atomic_load_explicit(&q->bottom, memory_order_relaxed);
    size_t top = atomic_load_explicit(&q->top, memory_order_acquire);
    
    if (bottom - top >= FIBER_SCHED_LOCAL_QUEUE_SIZE) {
        return -1;  /* Full */
    }
    
    size_t idx = bottom % FIBER_SCHED_LOCAL_QUEUE_SIZE;
    atomic_store_explicit(&q->tasks[idx], t, memory_order_relaxed);
    atomic_thread_fence(memory_order_release);
    atomic_store_explicit(&q->bottom, bottom + 1, memory_order_relaxed);
    return 0;
}

/* Owner pops from bottom (LIFO for locality) */
static inline fiber_task* deque_pop(work_stealing_deque* q) {
    size_t bottom = atomic_load_explicit(&q->bottom, memory_order_relaxed);
    if (bottom == 0) return NULL;
    
    bottom = bottom - 1;
    atomic_store_explicit(&q->bottom, bottom, memory_order_relaxed);
    atomic_thread_fence(memory_order_seq_cst);
    size_t top = atomic_load_explicit(&q->top, memory_order_relaxed);
    
    if (top > bottom) {
        /* Queue was empty */
        atomic_store_explicit(&q->bottom, top, memory_order_relaxed);
        return NULL;
    }
    
    size_t idx = bottom % FIBER_SCHED_LOCAL_QUEUE_SIZE;
    fiber_task* t = atomic_load_explicit(&q->tasks[idx], memory_order_relaxed);
    
    if (top == bottom) {
        /* Last element - race with stealers */
        if (!atomic_compare_exchange_strong_explicit(&q->top, &top, top + 1,
                                                      memory_order_seq_cst,
                                                      memory_order_relaxed)) {
            t = NULL;  /* Lost race */
        }
        atomic_store_explicit(&q->bottom, top + 1, memory_order_relaxed);
    }
    return t;
}

/* Thief steals from top (FIFO) */
static inline fiber_task* deque_steal(work_stealing_deque* q) {
    size_t top = atomic_load_explicit(&q->top, memory_order_acquire);
    atomic_thread_fence(memory_order_seq_cst);
    size_t bottom = atomic_load_explicit(&q->bottom, memory_order_acquire);
    
    if (top >= bottom) {
        return NULL;  /* Empty */
    }
    
    size_t idx = top % FIBER_SCHED_LOCAL_QUEUE_SIZE;
    fiber_task* t = atomic_load_explicit(&q->tasks[idx], memory_order_relaxed);
    
    if (!atomic_compare_exchange_strong_explicit(&q->top, &top, top + 1,
                                                  memory_order_seq_cst,
                                                  memory_order_relaxed)) {
        return NULL;  /* Lost race */
    }
    return t;
}

/* ============================================================================
 * Global MPMC queue (for external submissions)
 * ============================================================================ */

typedef struct {
    fiber_task* _Atomic tasks[FIBER_SCHED_QUEUE_SIZE];
    _Atomic size_t head;
    _Atomic size_t tail;
} task_queue;

static inline int queue_push(task_queue* q, fiber_task* t) {
    size_t tail = atomic_load_explicit(&q->tail, memory_order_relaxed);
    size_t head = atomic_load_explicit(&q->head, memory_order_acquire);
    
    if (tail - head >= FIBER_SCHED_QUEUE_SIZE) {
        return -1;  /* Full */
    }
    
    size_t idx = tail % FIBER_SCHED_QUEUE_SIZE;
    atomic_store_explicit(&q->tasks[idx], t, memory_order_relaxed);
    atomic_store_explicit(&q->tail, tail + 1, memory_order_release);
    return 0;
}

static inline fiber_task* queue_pop(task_queue* q) {
    size_t head = atomic_load_explicit(&q->head, memory_order_relaxed);
    size_t tail = atomic_load_explicit(&q->tail, memory_order_acquire);
    
    if (head >= tail) {
        return NULL;  /* Empty */
    }
    
    size_t idx = head % FIBER_SCHED_QUEUE_SIZE;
    fiber_task* t = atomic_load_explicit(&q->tasks[idx], memory_order_relaxed);
    
    if (atomic_compare_exchange_weak_explicit(&q->head, &head, head + 1,
                                               memory_order_release,
                                               memory_order_relaxed)) {
        return t;
    }
    return NULL;  /* Contention, try again */
}

/* ============================================================================
 * Per-Worker State
 * ============================================================================ */

typedef struct {
    work_stealing_deque local_queue;  /* Worker's local deque */
    size_t worker_id;
    _Atomic uint64_t tasks_executed;  /* Stats */
    _Atomic uint64_t tasks_stolen;    /* Stats */
    uint32_t rng_state;               /* For random victim selection */
} worker_state;

/* ============================================================================
 * Fiber Scheduler
 * ============================================================================ */

typedef struct {
    pthread_t workers[FIBER_SCHED_MAX_WORKERS];
    worker_state worker_states[FIBER_SCHED_MAX_WORKERS];
    size_t num_workers;
    task_queue global_queue;  /* For external submissions */
    _Atomic int running;
    
    /* Task pool to avoid malloc/free per spawn */
    fiber_task* _Atomic free_list;
    
    /* Wakeup mechanism */
    pthread_mutex_t wake_mu;
    pthread_cond_t wake_cv;
    _Atomic int pending_count;
} fiber_scheduler;

static fiber_scheduler g_sched = {0};
static _Atomic int g_sched_initialized = 0;
static _Thread_local size_t tls_worker_id = SIZE_MAX;  /* Which worker are we? */

/* Get task from pool or allocate new */
static fiber_task* task_alloc(void) {
    /* Try free list first */
    fiber_task* t = atomic_load_explicit(&g_sched.free_list, memory_order_acquire);
    while (t) {
        fiber_task* next = t->next;
        if (atomic_compare_exchange_weak_explicit(&g_sched.free_list, &t, next,
                                                   memory_order_release,
                                                   memory_order_acquire)) {
            memset(t, 0, sizeof(fiber_task));
            return t;
        }
    }
    
    /* Allocate new */
    return (fiber_task*)calloc(1, sizeof(fiber_task));
}

/* Return task to pool */
static void task_free(fiber_task* t) {
    if (!t) return;
    
    fiber_task* head = atomic_load_explicit(&g_sched.free_list, memory_order_relaxed);
    do {
        t->next = head;
    } while (!atomic_compare_exchange_weak_explicit(&g_sched.free_list, &head, t,
                                                     memory_order_release,
                                                     memory_order_relaxed));
}

/* Fast XorShift32 PRNG for random victim selection */
static inline uint32_t xorshift32(uint32_t* state) {
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

/* Try to steal work from other workers */
static fiber_task* try_steal(worker_state* ws) {
    size_t num_workers = g_sched.num_workers;
    if (num_workers <= 1) return NULL;
    
    /* Random starting point to avoid herding */
    uint32_t start = xorshift32(&ws->rng_state) % num_workers;
    
    for (size_t i = 0; i < num_workers; i++) {
        size_t victim = (start + i) % num_workers;
        if (victim == ws->worker_id) continue;
        
        fiber_task* t = deque_steal(&g_sched.worker_states[victim].local_queue);
        if (t) {
            atomic_fetch_add_explicit(&ws->tasks_stolen, 1, memory_order_relaxed);
            return t;
        }
    }
    return NULL;
}

/* Worker thread function */
static void* worker_main(void* arg) {
    size_t worker_id = (size_t)arg;
    tls_worker_id = worker_id;
    worker_state* ws = &g_sched.worker_states[worker_id];
    ws->worker_id = worker_id;
    ws->rng_state = (uint32_t)(worker_id + 1) * 2654435761u;  /* Seed PRNG */
    
    /* Initialize fiber system for this thread */
    cc__fiber_thread_init();
    
    while (atomic_load_explicit(&g_sched.running, memory_order_acquire)) {
        fiber_task* t = NULL;
        
        /* Priority 1: Local queue (LIFO for cache locality) */
        t = deque_pop(&ws->local_queue);
        
        /* Priority 2: Global queue */
        if (!t) {
            t = queue_pop(&g_sched.global_queue);
        }
        
        /* Priority 3: Steal from other workers */
        if (!t) {
            t = try_steal(ws);
        }
        
        if (t) {
            /* Execute task */
            void* result = NULL;
            if (t->fn) {
                result = t->fn(t->arg);
            }
            t->result = result;
            
            /* Mark done - use release to ensure result is visible */
            atomic_store_explicit(&t->done, 1, memory_order_release);
            
            /* Update stats and pending count */
            atomic_fetch_add_explicit(&ws->tasks_executed, 1, memory_order_relaxed);
            atomic_fetch_sub_explicit(&g_sched.pending_count, 1, memory_order_relaxed);
        } else {
            /* No work found - brief spin then check again */
            for (int spin = 0; spin < 32; spin++) {
                #if defined(__x86_64__) || defined(_M_X64)
                __asm__ volatile("pause");
                #elif defined(__aarch64__)
                __asm__ volatile("yield");
                #endif
            }
            
            /* Still no work - yield and possibly block */
            if (!t) {
                sched_yield();
                
                /* Only block if truly idle */
                if (atomic_load_explicit(&g_sched.pending_count, memory_order_relaxed) == 0) {
                    pthread_mutex_lock(&g_sched.wake_mu);
                    if (atomic_load_explicit(&g_sched.pending_count, memory_order_relaxed) == 0 &&
                        atomic_load_explicit(&g_sched.running, memory_order_relaxed)) {
                        pthread_cond_wait(&g_sched.wake_cv, &g_sched.wake_mu);
                    }
                    pthread_mutex_unlock(&g_sched.wake_mu);
                }
            }
        }
    }
    
    return NULL;
}

/* Initialize the fiber scheduler */
int cc_fiber_sched_init(size_t num_workers) {
    int expected = 0;
    if (!atomic_compare_exchange_strong(&g_sched_initialized, &expected, 1)) {
        return 0;  /* Already initialized */
    }
    
    if (num_workers == 0) {
        long n = sysconf(_SC_NPROCESSORS_ONLN);
        num_workers = n > 0 ? (size_t)n : 4;
    }
    if (num_workers > FIBER_SCHED_MAX_WORKERS) {
        num_workers = FIBER_SCHED_MAX_WORKERS;
    }
    
    memset(&g_sched, 0, sizeof(g_sched));
    g_sched.num_workers = num_workers;
    atomic_store(&g_sched.running, 1);
    pthread_mutex_init(&g_sched.wake_mu, NULL);
    pthread_cond_init(&g_sched.wake_cv, NULL);
    
    /* Initialize per-worker state */
    for (size_t i = 0; i < num_workers; i++) {
        memset(&g_sched.worker_states[i], 0, sizeof(worker_state));
    }
    
    /* Start workers */
    for (size_t i = 0; i < num_workers; i++) {
        pthread_create(&g_sched.workers[i], NULL, worker_main, (void*)i);
    }
    
    return 0;
}

/* Shutdown the fiber scheduler */
void cc_fiber_sched_shutdown(void) {
    if (!atomic_load(&g_sched_initialized)) return;
    
    /* Signal workers to stop */
    atomic_store_explicit(&g_sched.running, 0, memory_order_release);
    
    /* Wake all workers */
    pthread_mutex_lock(&g_sched.wake_mu);
    pthread_cond_broadcast(&g_sched.wake_cv);
    pthread_mutex_unlock(&g_sched.wake_mu);
    
    /* Wait for workers */
    for (size_t i = 0; i < g_sched.num_workers; i++) {
        pthread_join(g_sched.workers[i], NULL);
    }
    
    /* Clean up free list */
    fiber_task* t = atomic_load(&g_sched.free_list);
    while (t) {
        fiber_task* next = t->next;
        free(t);
        t = next;
    }
    
    pthread_mutex_destroy(&g_sched.wake_mu);
    pthread_cond_destroy(&g_sched.wake_cv);
    
    atomic_store(&g_sched_initialized, 0);
}

/* Spawn a task on the fiber scheduler */
fiber_task* cc_fiber_spawn(void* (*fn)(void*), void* arg) {
    /* Lazy init */
    if (!atomic_load_explicit(&g_sched_initialized, memory_order_acquire)) {
        cc_fiber_sched_init(0);
    }
    
    fiber_task* t = task_alloc();
    if (!t) return NULL;
    
    t->fn = fn;
    t->arg = arg;
    atomic_store(&t->done, 0);
    atomic_store(&t->waiting, 0);
    
    int queued = 0;
    
    /* If we're a worker thread, prefer local queue */
    if (tls_worker_id < g_sched.num_workers) {
        worker_state* ws = &g_sched.worker_states[tls_worker_id];
        if (deque_push(&ws->local_queue, t) == 0) {
            queued = 1;
        }
    }
    
    /* Fall back to global queue */
    if (!queued) {
        if (queue_push(&g_sched.global_queue, t) != 0) {
            task_free(t);
            return NULL;
        }
    }
    
    /* Increment pending and wake a worker */
    atomic_fetch_add_explicit(&g_sched.pending_count, 1, memory_order_relaxed);
    pthread_mutex_lock(&g_sched.wake_mu);
    pthread_cond_signal(&g_sched.wake_cv);
    pthread_mutex_unlock(&g_sched.wake_mu);
    
    return t;
}

/* ============================================================================
 * Adaptive Spinning
 * ============================================================================ */

/* Per-thread adaptive spin state */
typedef struct {
    uint64_t total_spins;       /* Total spins across all joins */
    uint64_t total_joins;       /* Number of joins */
    uint32_t avg_spins_to_done; /* EMA of spins until task completed */
    uint32_t max_spin_count;    /* Adaptive max spin count */
} adaptive_spin_state;

static _Thread_local adaptive_spin_state tls_spin_state = {0, 0, 100, 500};

/* Update adaptive spin parameters based on observed completion */
static inline void update_adaptive_spin(uint32_t spins_used, int found_work) {
    adaptive_spin_state* s = &tls_spin_state;
    s->total_joins++;
    
    if (found_work && spins_used > 0) {
        s->total_spins += spins_used;
        /* Exponential moving average: new_avg = 0.9 * old_avg + 0.1 * sample */
        s->avg_spins_to_done = (s->avg_spins_to_done * 9 + spins_used) / 10;
        
        /* Adapt max spin count: target 2x the average */
        uint32_t target = s->avg_spins_to_done * 2;
        if (target < 64) target = 64;
        if (target > 2000) target = 2000;
        
        /* Smooth adjustment */
        if (target > s->max_spin_count) {
            s->max_spin_count = (s->max_spin_count * 3 + target) / 4;
        } else {
            s->max_spin_count = (s->max_spin_count * 7 + target) / 8;
        }
    } else if (!found_work) {
        /* Task took too long - reduce spin count */
        s->max_spin_count = s->max_spin_count * 3 / 4;
        if (s->max_spin_count < 64) s->max_spin_count = 64;
    }
}

/* Join a fiber task */
int cc_fiber_join(fiber_task* t, void** out_result) {
    if (!t) return -1;
    
    /* Fast path - task might already be done */
    if (atomic_load_explicit(&t->done, memory_order_acquire)) {
        if (out_result) *out_result = t->result;
        return 0;
    }
    
    adaptive_spin_state* spin_state = &tls_spin_state;
    uint32_t max_spins = spin_state->max_spin_count;
    uint32_t spins = 0;
    
    /* Adaptive spin-wait */
    for (; spins < max_spins; spins++) {
        if (atomic_load_explicit(&t->done, memory_order_acquire)) {
            if (out_result) *out_result = t->result;
            update_adaptive_spin(spins, 1);
            return 0;
        }
        
        /* Pause instruction with increasing backoff */
        int pause_count = 1 + (spins >> 6);  /* Increase pause every 64 spins */
        for (int i = 0; i < pause_count; i++) {
            #if defined(__x86_64__) || defined(_M_X64)
            __asm__ volatile("pause");
            #elif defined(__aarch64__)
            __asm__ volatile("yield");
            #else
            /* Compiler barrier */
            __asm__ volatile("" ::: "memory");
            #endif
        }
    }
    
    /* Update stats - we exceeded spin budget */
    update_adaptive_spin(spins, 0);
    
    /* Yield loop for longer waits */
    while (!atomic_load_explicit(&t->done, memory_order_acquire)) {
        sched_yield();
    }
    
    if (out_result) {
        *out_result = t->result;
    }
    
    return 0;
}

/* Free a fiber task */
void cc_fiber_task_free(fiber_task* t) {
    task_free(t);
}

/* ============================================================================
 * Fiber Scheduler Integration for Channel Blocking
 * ============================================================================ */

/* Check if scheduler is active */
int cc__fiber_sched_active(void) {
    return atomic_load_explicit(&g_sched_initialized, memory_order_acquire);
}

/* Wrapper to execute a fiber */
static void* fiber_wrapper_fn(void* arg) {
    cc__fiber* f = (cc__fiber*)arg;
    if (!f) return NULL;
    
    /* Initialize fiber system for this worker thread */
    cc__fiber_thread_init();
    
    /* Switch to the fiber and run it */
    cc__fiber_switch_to(f);
    
    return f->result;
}

/* Enqueue an unparked fiber for execution */
void cc__fiber_sched_enqueue(cc__fiber* f) {
    if (!f) return;
    
    /* Lazy init scheduler if needed */
    if (!atomic_load_explicit(&g_sched_initialized, memory_order_acquire)) {
        cc_fiber_sched_init(0);
    }
    
    /* Spawn a task to run the fiber */
    fiber_task* t = task_alloc();
    if (!t) return;
    
    t->fn = fiber_wrapper_fn;
    t->arg = f;
    atomic_store(&t->done, 0);
    atomic_store(&t->waiting, 0);
    
    int queued = 0;
    
    /* If we're a worker thread, prefer local queue */
    if (tls_worker_id < g_sched.num_workers) {
        worker_state* ws = &g_sched.worker_states[tls_worker_id];
        if (deque_push(&ws->local_queue, t) == 0) {
            queued = 1;
        }
    }
    
    /* Fall back to global queue */
    if (!queued) {
        if (queue_push(&g_sched.global_queue, t) != 0) {
            task_free(t);
            return;
        }
    }
    
    /* Increment pending and wake a worker */
    atomic_fetch_add_explicit(&g_sched.pending_count, 1, memory_order_relaxed);
    pthread_mutex_lock(&g_sched.wake_mu);
    pthread_cond_signal(&g_sched.wake_cv);
    pthread_mutex_unlock(&g_sched.wake_mu);
}
