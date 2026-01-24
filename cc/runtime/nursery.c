/*
 * Structured concurrency nursery built on the fiber scheduler.
 * 
 * spawn() pushes tasks to global queue, workers execute them.
 * Nursery tracks tasks for join and handles cancellation/deadlines.
 */

#include <ccc/cc_nursery.cch>
#include <ccc/cc_channel.cch>
#include <ccc/cc_deadlock_detect.cch>

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ============================================================================
 * Nursery spawn timing instrumentation
 * ============================================================================ */

static inline uint64_t nursery_rdtsc(void) {
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

typedef struct {
    _Atomic uint64_t thunk_alloc_cycles;
    _Atomic uint64_t fiber_spawn_cycles;
    _Atomic uint64_t mutex_cycles;
    _Atomic uint64_t total_cycles;
    _Atomic size_t count;
} nursery_timing;

static nursery_timing g_nursery_timing = {0};
static int g_nursery_timing_enabled = -1;

static int nursery_timing_enabled(void) {
    if (g_nursery_timing_enabled < 0) {
        g_nursery_timing_enabled = getenv("CC_SPAWN_TIMING") != NULL;
    }
    return g_nursery_timing_enabled;
}

void cc_nursery_dump_timing(void) {
    size_t count = atomic_load(&g_nursery_timing.count);
    if (count == 0) return;
    
    uint64_t thunk = atomic_load(&g_nursery_timing.thunk_alloc_cycles);
    uint64_t spawn = atomic_load(&g_nursery_timing.fiber_spawn_cycles);
    uint64_t mutex = atomic_load(&g_nursery_timing.mutex_cycles);
    uint64_t total = atomic_load(&g_nursery_timing.total_cycles);
    
    fprintf(stderr, "\n=== NURSERY SPAWN TIMING (%zu spawns) ===\n", count);
    fprintf(stderr, "  Total:        %8.1f cycles/spawn (100.0%%)\n", (double)total / count);
    fprintf(stderr, "  Breakdown:\n");
    fprintf(stderr, "    thunk_alloc: %8.1f cycles/spawn (%5.1f%%)\n", (double)thunk / count, 100.0 * thunk / total);
    fprintf(stderr, "    fiber_spawn: %8.1f cycles/spawn (%5.1f%%)\n", (double)spawn / count, 100.0 * spawn / total);
    fprintf(stderr, "    mutex:       %8.1f cycles/spawn (%5.1f%%)\n", (double)mutex / count, 100.0 * mutex / total);
    fprintf(stderr, "==========================================\n\n");
}

/* Forward declaration for fiber_task */
typedef struct fiber_task fiber_task;

/* Fiber scheduler functions */
fiber_task* cc_fiber_spawn(void* (*fn)(void*), void* arg);
int cc_fiber_join(fiber_task* t, void** out_result);
void cc_fiber_task_free(fiber_task* t);

/* Thread-local: current nursery for code running inside nursery-spawned tasks.
   Used by optional runtime deadlock guard in channel.c. */
__thread CCNursery* cc__tls_current_nursery = NULL;

struct CCNursery {
    fiber_task** tasks;     /* Tasks spawned in this nursery */
    size_t count;
    size_t cap;
    int cancelled;
    struct timespec deadline;
    CCChan** closing;
    size_t closing_count;
    size_t closing_cap;
    pthread_mutex_t mu;
};

/* Defined in channel.c (same translation unit via runtime/concurrent_c.c). */
void cc__chan_set_autoclose_owner(CCChan* ch, CCNursery* owner);

int cc_nursery_add_closing_tx(CCNursery* n, CCChanTx tx) {
    return cc_nursery_add_closing_chan(n, tx.raw);
}

typedef struct CCNurseryThunk {
    CCNursery* n;
    void* (*fn)(void*);
    void* arg;
    struct CCNurseryThunk* next;  /* For free list pooling */
} CCNurseryThunk;

/* Lock-free thunk pool */
static CCNurseryThunk* _Atomic g_thunk_free_list = NULL;

static CCNurseryThunk* thunk_alloc(void) {
    CCNurseryThunk* th = atomic_load_explicit(&g_thunk_free_list, memory_order_acquire);
    while (th) {
        CCNurseryThunk* next = th->next;
        if (atomic_compare_exchange_weak_explicit(&g_thunk_free_list, &th, next,
                                                   memory_order_release,
                                                   memory_order_acquire)) {
            th->next = NULL;
            return th;
        }
    }
    return (CCNurseryThunk*)malloc(sizeof(CCNurseryThunk));
}

static void thunk_free(CCNurseryThunk* th) {
    if (!th) return;
    CCNurseryThunk* head;
    do {
        head = atomic_load_explicit(&g_thunk_free_list, memory_order_relaxed);
        th->next = head;
    } while (!atomic_compare_exchange_weak_explicit(&g_thunk_free_list, &head, th,
                                                     memory_order_release,
                                                     memory_order_relaxed));
}

static void* cc__nursery_task_trampoline(void* p) {
    CCNurseryThunk* th = (CCNurseryThunk*)p;
    if (!th) return NULL;
    CCNursery* nn = th->n;
    void* (*ff)(void*) = th->fn;
    void* aa = th->arg;
    thunk_free(th);  /* Return to pool instead of free */
    cc__tls_current_nursery = nn;
    void* r = ff ? ff(aa) : NULL;
    cc__tls_current_nursery = NULL;
    return r;
}

CCNursery* cc_nursery_create(void) {
    /* Initialize deadlock detection on first nursery (lazy init, checks env var) */
    cc_deadlock_detect_init();
    
    CCNursery* n = (CCNursery*)malloc(sizeof(CCNursery));
    if (!n) return NULL;
    memset(n, 0, sizeof(*n));
    n->cap = 8;
    n->tasks = (fiber_task**)calloc(n->cap, sizeof(fiber_task*));
    if (!n->tasks) {
        free(n);
        return NULL;
    }
    pthread_mutex_init(&n->mu, NULL);
    n->deadline.tv_sec = 0;
    n->deadline.tv_nsec = 0;
    n->closing = NULL;
    n->closing_cap = 0;
    n->closing_count = 0;
    return n;
}

void cc_nursery_cancel(CCNursery* n) {
    if (!n) return;
    pthread_mutex_lock(&n->mu);
    n->cancelled = 1;
    pthread_mutex_unlock(&n->mu);
}

void cc_nursery_set_deadline(CCNursery* n, struct timespec abs_deadline) {
    if (!n) return;
    pthread_mutex_lock(&n->mu);
    n->deadline = abs_deadline;
    pthread_mutex_unlock(&n->mu);
}

const struct timespec* cc_nursery_deadline(const CCNursery* n, struct timespec* out) {
    if (!n || n->deadline.tv_sec == 0) return NULL;
    if (out) *out = n->deadline;
    return out;
}

CCDeadline cc_nursery_as_deadline(const CCNursery* n) {
    CCDeadline d = cc_deadline_none();
    if (!n) { d.cancelled = 1; return d; }
    d.cancelled = n->cancelled;
    d.deadline = n->deadline;
    return d;
}

bool cc_nursery_is_cancelled(const CCNursery* n) {
    if (!n) return true;
    if (n->cancelled) return true;
    if (n->deadline.tv_sec == 0) return false;
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    if (now.tv_sec > n->deadline.tv_sec) return true;
    if (now.tv_sec == n->deadline.tv_sec && now.tv_nsec >= n->deadline.tv_nsec) return true;
    return false;
}

static int cc_nursery_grow(CCNursery* n) {
    size_t new_cap = n->cap ? n->cap * 2 : 8;
    fiber_task** nt = (fiber_task**)realloc(n->tasks, new_cap * sizeof(fiber_task*));
    if (!nt) return ENOMEM;
    memset(nt + n->cap, 0, (new_cap - n->cap) * sizeof(fiber_task*));
    n->tasks = nt;
    n->cap = new_cap;
    return 0;
}

int cc_nursery_spawn(CCNursery* n, void* (*fn)(void*), void* arg) {
    if (!n || !fn) return EINVAL;
    
    int timing = nursery_timing_enabled();
    uint64_t t0 = 0, t1, t2, t3;
    
    if (timing) t0 = nursery_rdtsc();

    CCNurseryThunk* th = thunk_alloc();
    if (!th) return ENOMEM;
    th->n = n;
    th->fn = fn;
    th->arg = arg;

    if (timing) t1 = nursery_rdtsc();

    /* Spawn task on fiber scheduler */
    fiber_task* t = cc_fiber_spawn(cc__nursery_task_trampoline, th);
    if (!t) {
        free(th);
        return ENOMEM;
    }

    if (timing) t2 = nursery_rdtsc();

    pthread_mutex_lock(&n->mu);
    /* Grow if needed */
    if (n->count == n->cap) {
        int grow_err = cc_nursery_grow(n);
        if (grow_err != 0) {
            pthread_mutex_unlock(&n->mu);
            cc_fiber_task_free(t);
            free(th);
            return grow_err;
        }
    }
    n->tasks[n->count++] = t;
    pthread_mutex_unlock(&n->mu);
    
    if (timing) {
        t3 = nursery_rdtsc();
        atomic_fetch_add_explicit(&g_nursery_timing.thunk_alloc_cycles, t1 - t0, memory_order_relaxed);
        atomic_fetch_add_explicit(&g_nursery_timing.fiber_spawn_cycles, t2 - t1, memory_order_relaxed);
        atomic_fetch_add_explicit(&g_nursery_timing.mutex_cycles, t3 - t2, memory_order_relaxed);
        atomic_fetch_add_explicit(&g_nursery_timing.total_cycles, t3 - t0, memory_order_relaxed);
        atomic_fetch_add_explicit(&g_nursery_timing.count, 1, memory_order_relaxed);
    }
    
    return 0;
}

int cc_nursery_wait(CCNursery* n) {
    if (!n) return EINVAL;
    int first_err = 0;
    
    /* Join all tasks - spec: join children first, then close channels */
    for (size_t i = 0; i < n->count; ++i) {
        if (!n->tasks[i]) continue;
        int err = cc_fiber_join(n->tasks[i], NULL);
        if (first_err == 0 && err != 0) {
            first_err = err;
        }
        cc_fiber_task_free(n->tasks[i]);
        n->tasks[i] = NULL;
    }
    
    /* Close registered channels */
    for (size_t i = 0; i < n->closing_count; ++i) {
        if (n->closing[i]) cc_chan_close(n->closing[i]);
    }
    n->count = 0;
    return first_err;
}

void cc_nursery_free(CCNursery* n) {
    if (!n) return;
    for (size_t i = 0; i < n->count; ++i) {
        if (n->tasks[i]) {
            cc_fiber_task_free(n->tasks[i]);
        }
    }
    /* Close registered channels as a last step */
    for (size_t i = 0; i < n->closing_count; ++i) {
        if (n->closing[i]) cc_chan_close(n->closing[i]);
    }
    free(n->tasks);
    free(n->closing);
    pthread_mutex_destroy(&n->mu);
    free(n);
}

int cc_nursery_add_closing_chan(CCNursery* n, CCChan* ch) {
    if (!n || !ch) return EINVAL;
    pthread_mutex_lock(&n->mu);
    if (n->closing_count == n->closing_cap) {
        size_t new_cap = n->closing_cap ? n->closing_cap * 2 : 4;
        CCChan** nc = (CCChan**)realloc(n->closing, new_cap * sizeof(CCChan*));
        if (!nc) { pthread_mutex_unlock(&n->mu); return ENOMEM; }
        memset(nc + n->closing_cap, 0, (new_cap - n->closing_cap) * sizeof(CCChan*));
        n->closing = nc;
        n->closing_cap = new_cap;
    }
    n->closing[n->closing_count++] = ch;
    pthread_mutex_unlock(&n->mu);
    /* Mark channel with its autoclose owner for optional runtime guard. */
    cc__chan_set_autoclose_owner(ch, n);
    return 0;
}
