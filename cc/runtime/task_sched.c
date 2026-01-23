/*
 * Task scheduler: one global queue, N worker threads.
 * 
 * Design:
 *   - spawn() pushes task to global MPMC queue
 *   - N worker threads pop and execute tasks
 *   - Tasks use atomic done flag (no per-task mutex/cond)
 *   - Task pool avoids malloc/free per spawn
 *
 * Configuration:
 *   CC_WORKERS - number of worker threads (default: CPU count)
 *   CC_TASK_QUEUE_SIZE - queue capacity (default: 4096)
 *   CC_TASK_POOL_SIZE - task pool size (default: 1024)
 */

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

#ifndef CC_WORKERS
#define CC_WORKERS 0  /* 0 = detect at runtime */
#endif

#ifndef CC_TASK_QUEUE_SIZE
#define CC_TASK_QUEUE_SIZE 4096
#endif

#ifndef CC_TASK_POOL_SIZE
#define CC_TASK_POOL_SIZE 1024
#endif

#define MAX_WORKERS 64

/* CPU pause hint for spin loops */
static inline void cpu_pause(void) {
    #if defined(__x86_64__) || defined(_M_X64)
    __asm__ volatile("pause");
    #elif defined(__aarch64__) || defined(__arm64__)
    __asm__ volatile("yield");
    #endif
}

/* ============================================================================
 * Task - lightweight work unit
 * ============================================================================ */

typedef struct fiber_task {
    void* (*fn)(void*);
    void* arg;
    void* result;
    _Atomic int done;
    struct fiber_task* next;  /* Free list link */
} fiber_task;

/* ============================================================================
 * Lock-Free MPMC Queue
 * ============================================================================ */

typedef struct {
    fiber_task* _Atomic slots[CC_TASK_QUEUE_SIZE];
    _Atomic size_t head;
    _Atomic size_t tail;
} task_queue;

static inline int queue_push(task_queue* q, fiber_task* t) {
    for (int retry = 0; retry < 1000; retry++) {
        size_t tail = atomic_load_explicit(&q->tail, memory_order_relaxed);
        size_t head = atomic_load_explicit(&q->head, memory_order_acquire);
        
        if (tail - head >= CC_TASK_QUEUE_SIZE) {
            sched_yield();
            continue;
        }
        
        if (atomic_compare_exchange_weak_explicit(&q->tail, &tail, tail + 1,
                                                   memory_order_release,
                                                   memory_order_relaxed)) {
            atomic_store_explicit(&q->slots[tail % CC_TASK_QUEUE_SIZE], t, memory_order_release);
            return 0;
        }
    }
    return -1;  /* Queue full */
}

static inline fiber_task* queue_pop(task_queue* q) {
    for (int retry = 0; retry < 100; retry++) {
        size_t head = atomic_load_explicit(&q->head, memory_order_relaxed);
        size_t tail = atomic_load_explicit(&q->tail, memory_order_acquire);
        
        if (head >= tail) return NULL;  /* Empty */
        
        size_t idx = head % CC_TASK_QUEUE_SIZE;
        fiber_task* t = atomic_load_explicit(&q->slots[idx], memory_order_acquire);
        
        if (!t) {
            /* Producer claimed slot but hasn't stored yet - brief spin */
            for (int i = 0; i < 10; i++) cpu_pause();
            continue;
        }
        
        if (atomic_compare_exchange_weak_explicit(&q->head, &head, head + 1,
                                                   memory_order_release,
                                                   memory_order_relaxed)) {
            atomic_store_explicit(&q->slots[idx], NULL, memory_order_relaxed);
            return t;
        }
    }
    return NULL;  /* Contention, let caller retry */
}

/* ============================================================================
 * Scheduler State
 * ============================================================================ */

typedef struct {
    pthread_t workers[MAX_WORKERS];
    size_t num_workers;
    _Atomic int running;
    
    task_queue queue;
    fiber_task* _Atomic free_list;
    
    pthread_mutex_t mu;
    pthread_cond_t cv;
    _Atomic size_t pending;       /* Tasks queued but not done */
    
    /* Observability counters */
    _Atomic size_t active;        /* Workers currently executing a task */
    _Atomic size_t sleeping;      /* Workers blocked on condvar */
    _Atomic size_t queued;        /* Tasks in queue (not yet picked up) */
    _Atomic size_t parked;        /* Tasks/fibers parked (blocked on channel etc) */
    _Atomic size_t completed;     /* Total tasks completed (lifetime) */
} scheduler;

/* Stats snapshot for external queries */
typedef struct {
    size_t num_workers;       /* Total worker threads */
    size_t active;            /* Workers executing tasks */
    size_t sleeping;          /* Workers blocked on condvar */
    size_t idle;              /* Workers spinning/yielding (= num_workers - active - sleeping) */
    size_t queued;            /* Tasks waiting in queue */
    size_t parked;            /* Tasks blocked on I/O/channels */
    size_t pending;           /* Total tasks not yet done */
    size_t completed;         /* Lifetime completed count */
} cc_sched_stats;

static scheduler g_sched = {0};
static _Atomic int g_initialized = 0;  /* 0=no, 1=initializing, 2=ready */
static _Atomic int g_deadlock_reported = 0;

/* Forward declaration */
void cc_sched_get_stats(cc_sched_stats* stats);

/* Deadlock handler - called when scheduler detects no progress possible */
static void cc__sched_deadlock_detected(void) {
    /* Only report once */
    int expected = 0;
    if (!atomic_compare_exchange_strong(&g_deadlock_reported, &expected, 1)) {
        return;
    }
    
    cc_sched_stats s;
    cc_sched_get_stats(&s);
    
    fprintf(stderr, "\n=== DEADLOCK DETECTED ===\n");
    fprintf(stderr, "Workers: %zu total, %zu active, %zu idle, %zu sleeping\n",
            s.num_workers, s.active, s.idle, s.sleeping);
    fprintf(stderr, "Tasks: %zu queued, %zu parked, %zu pending, %zu completed\n",
            s.queued, s.parked, s.pending, s.completed);
    fprintf(stderr, "All tasks are parked with no runnable work.\n");
    fprintf(stderr, "=========================\n\n");
    
    /* TODO: could make this configurable (abort vs continue) */
    /* For now, just report - user code might unblock via timeout or external event */
}

/* ============================================================================
 * Task Pool
 * ============================================================================ */

static fiber_task* task_alloc(void) {
    /* Try free list first */
    fiber_task* t = atomic_load_explicit(&g_sched.free_list, memory_order_acquire);
    while (t) {
        fiber_task* next = t->next;
        if (atomic_compare_exchange_weak_explicit(&g_sched.free_list, &t, next,
                                                   memory_order_release,
                                                   memory_order_acquire)) {
            memset(t, 0, sizeof(*t));
            return t;
        }
    }
    return (fiber_task*)calloc(1, sizeof(fiber_task));
}

static void task_free(fiber_task* t) {
    if (!t) return;
    fiber_task* head;
    do {
        head = atomic_load_explicit(&g_sched.free_list, memory_order_relaxed);
        t->next = head;
    } while (!atomic_compare_exchange_weak_explicit(&g_sched.free_list, &head, t,
                                                     memory_order_release,
                                                     memory_order_relaxed));
}

/* ============================================================================
 * Worker Thread
 * ============================================================================ */

static inline void execute_task(fiber_task* t) {
    atomic_fetch_add_explicit(&g_sched.active, 1, memory_order_relaxed);
    
    t->result = t->fn ? t->fn(t->arg) : NULL;
    atomic_store_explicit(&t->done, 1, memory_order_release);
    
    atomic_fetch_sub_explicit(&g_sched.active, 1, memory_order_relaxed);
    atomic_fetch_sub_explicit(&g_sched.pending, 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&g_sched.completed, 1, memory_order_relaxed);
}

static void* worker_main(void* arg) {
    (void)arg;
    
    while (atomic_load_explicit(&g_sched.running, memory_order_acquire)) {
        fiber_task* t = queue_pop(&g_sched.queue);
        
        if (t) {
            atomic_fetch_sub_explicit(&g_sched.queued, 1, memory_order_relaxed);
            execute_task(t);
            continue;
        }
        
        /* Brief spin for contention */
        for (int spin = 0; spin < 32; spin++) {
            cpu_pause();
        }
        
        t = queue_pop(&g_sched.queue);
        if (t) {
            atomic_fetch_sub_explicit(&g_sched.queued, 1, memory_order_relaxed);
            execute_task(t);
            continue;
        }
        
        /* Check if there might be work (pending > 0 means tasks queued) */
        if (atomic_load_explicit(&g_sched.pending, memory_order_relaxed) > 0) {
            sched_yield();  /* Let other threads run */
            continue;
        }
        
        /* No pending work - check for deadlock before sleeping */
        size_t parked = atomic_load_explicit(&g_sched.parked, memory_order_relaxed);
        size_t queued = atomic_load_explicit(&g_sched.queued, memory_order_relaxed);
        size_t active = atomic_load_explicit(&g_sched.active, memory_order_relaxed);
        
        if (parked > 0 && queued == 0 && active == 0) {
            /* Potential deadlock: tasks parked but nothing runnable */
            cc__sched_deadlock_detected();
        }
        
        /* Sleep on condvar */
        pthread_mutex_lock(&g_sched.mu);
        atomic_fetch_add_explicit(&g_sched.sleeping, 1, memory_order_relaxed);
        while (atomic_load_explicit(&g_sched.pending, memory_order_relaxed) == 0 &&
               atomic_load_explicit(&g_sched.running, memory_order_relaxed)) {
            pthread_cond_wait(&g_sched.cv, &g_sched.mu);
        }
        atomic_fetch_sub_explicit(&g_sched.sleeping, 1, memory_order_relaxed);
        pthread_mutex_unlock(&g_sched.mu);
    }
    
    return NULL;
}

/* ============================================================================
 * Public API
 * ============================================================================ */

int cc_fiber_sched_init(size_t num_workers) {
    /* Fast path: already initialized */
    int state = atomic_load_explicit(&g_initialized, memory_order_acquire);
    if (state == 2) return 0;
    
    /* Try to claim initialization */
    int expected = 0;
    if (!atomic_compare_exchange_strong_explicit(&g_initialized, &expected, 1,
                                                  memory_order_acq_rel,
                                                  memory_order_acquire)) {
        /* Another thread is initializing - wait */
        while (atomic_load_explicit(&g_initialized, memory_order_acquire) != 2) {
            sched_yield();
        }
        return 0;
    }
    
    /* Determine worker count */
    if (num_workers == 0) {
        #if CC_WORKERS > 0
        num_workers = CC_WORKERS;
        #else
        long n = sysconf(_SC_NPROCESSORS_ONLN);
        num_workers = n > 0 ? (size_t)n : 4;
        #endif
    }
    if (num_workers > MAX_WORKERS) {
        num_workers = MAX_WORKERS;
    }
    
    /* Initialize scheduler */
    memset(&g_sched, 0, sizeof(g_sched));
    g_sched.num_workers = num_workers;
    atomic_store(&g_sched.running, 1);
    pthread_mutex_init(&g_sched.mu, NULL);
    pthread_cond_init(&g_sched.cv, NULL);
    
    /* Pre-allocate task pool */
    for (int i = 0; i < CC_TASK_POOL_SIZE; i++) {
        fiber_task* t = (fiber_task*)calloc(1, sizeof(fiber_task));
        if (t) {
            t->next = atomic_load(&g_sched.free_list);
            atomic_store(&g_sched.free_list, t);
        }
    }
    
    /* Start workers */
    for (size_t i = 0; i < num_workers; i++) {
        pthread_create(&g_sched.workers[i], NULL, worker_main, NULL);
    }
    
    atomic_store_explicit(&g_initialized, 2, memory_order_release);
    return 0;
}

void cc_fiber_sched_shutdown(void) {
    if (atomic_load_explicit(&g_initialized, memory_order_acquire) != 2) return;
    
    /* Signal workers to stop */
    atomic_store_explicit(&g_sched.running, 0, memory_order_release);
    pthread_mutex_lock(&g_sched.mu);
    pthread_cond_broadcast(&g_sched.cv);
    pthread_mutex_unlock(&g_sched.mu);
    
    /* Wait for workers */
    for (size_t i = 0; i < g_sched.num_workers; i++) {
        pthread_join(g_sched.workers[i], NULL);
    }
    
    /* Free task pool */
    fiber_task* t = atomic_load(&g_sched.free_list);
    while (t) {
        fiber_task* next = t->next;
        free(t);
        t = next;
    }
    
    pthread_mutex_destroy(&g_sched.mu);
    pthread_cond_destroy(&g_sched.cv);
    atomic_store(&g_initialized, 0);
}

fiber_task* cc_fiber_spawn(void* (*fn)(void*), void* arg) {
    /* Lazy init */
    if (atomic_load_explicit(&g_initialized, memory_order_acquire) != 2) {
        cc_fiber_sched_init(0);
    }
    
    fiber_task* t = task_alloc();
    if (!t) return NULL;
    
    t->fn = fn;
    t->arg = arg;
    atomic_store(&t->done, 0);
    
    if (queue_push(&g_sched.queue, t) != 0) {
        task_free(t);
        return NULL;
    }
    
    atomic_fetch_add_explicit(&g_sched.queued, 1, memory_order_relaxed);
    
    /* Increment pending - only signal if transitioning from 0 (workers might be sleeping) */
    size_t prev = atomic_fetch_add_explicit(&g_sched.pending, 1, memory_order_release);
    if (prev == 0) {
        pthread_mutex_lock(&g_sched.mu);
        pthread_cond_broadcast(&g_sched.cv);  /* Wake all workers */
        pthread_mutex_unlock(&g_sched.mu);
    }
    
    return t;
}

int cc_fiber_join(fiber_task* t, void** out_result) {
    if (!t) return -1;
    
    /* Fast path */
    if (atomic_load_explicit(&t->done, memory_order_acquire)) {
        if (out_result) *out_result = t->result;
        return 0;
    }
    
    /* Brief spin for short tasks */
    for (int i = 0; i < 100; i++) {
        if (atomic_load_explicit(&t->done, memory_order_acquire)) {
            if (out_result) *out_result = t->result;
            return 0;
        }
        cpu_pause();
    }
    
    /* Yield for longer tasks */
    while (!atomic_load_explicit(&t->done, memory_order_acquire)) {
        sched_yield();
    }
    
    if (out_result) *out_result = t->result;
    return 0;
}

void cc_fiber_task_free(fiber_task* t) {
    task_free(t);
}

/* ============================================================================
 * Internal API (for future fiber integration)
 * ============================================================================ */

int cc__fiber_sched_active(void) {
    return atomic_load_explicit(&g_initialized, memory_order_acquire) == 2;
}

/* ============================================================================
 * Observability API
 * ============================================================================ */

void cc_sched_get_stats(cc_sched_stats* stats) {
    if (!stats) return;
    
    stats->num_workers = g_sched.num_workers;
    stats->active = atomic_load_explicit(&g_sched.active, memory_order_relaxed);
    stats->sleeping = atomic_load_explicit(&g_sched.sleeping, memory_order_relaxed);
    stats->queued = atomic_load_explicit(&g_sched.queued, memory_order_relaxed);
    stats->parked = atomic_load_explicit(&g_sched.parked, memory_order_relaxed);
    stats->pending = atomic_load_explicit(&g_sched.pending, memory_order_relaxed);
    stats->completed = atomic_load_explicit(&g_sched.completed, memory_order_relaxed);
    
    /* Compute idle = workers not active and not sleeping */
    size_t busy = stats->active + stats->sleeping;
    stats->idle = (stats->num_workers > busy) ? (stats->num_workers - busy) : 0;
}

/* Park/unpark tracking for channel blocking (called from channel.c) */
void cc__sched_task_parked(void) {
    atomic_fetch_add_explicit(&g_sched.parked, 1, memory_order_relaxed);
}

void cc__sched_task_unparked(void) {
    atomic_fetch_sub_explicit(&g_sched.parked, 1, memory_order_relaxed);
}

/* Debug dump - prints scheduler state to stderr */
void cc_sched_dump_stats(void) {
    cc_sched_stats s;
    cc_sched_get_stats(&s);
    fprintf(stderr, "[sched] workers=%zu active=%zu idle=%zu sleeping=%zu | "
                    "queued=%zu parked=%zu pending=%zu | completed=%zu\n",
            s.num_workers, s.active, s.idle, s.sleeping,
            s.queued, s.parked, s.pending, s.completed);
}
