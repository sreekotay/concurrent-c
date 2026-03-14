/*
 * Structured concurrency nursery built on the fiber scheduler.
 * 
 * spawn() pushes tasks to global queue, workers execute them.
 * Nursery tracks tasks for join and handles cancellation/deadlines.
 */

#include <ccc/cc_nursery.cch>
#include <ccc/cc_channel.cch>
#include <ccc/cc_arena.cch>

#include "wake_primitive.h"

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "fiber_sched_boundary.h"

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
    _Atomic uint64_t setup_cycles;
    _Atomic uint64_t fiber_spawn_cycles;
    _Atomic uint64_t mutex_cycles;
    _Atomic uint64_t spawn_total_cycles;
    _Atomic uint64_t wait_join_cycles;
    _Atomic uint64_t wait_free_cycles;
    _Atomic uint64_t wait_close_cycles;
    _Atomic uint64_t wait_total_cycles;
    _Atomic size_t spawn_count;
    _Atomic size_t wait_calls;
    _Atomic size_t wait_tasks_joined;
    _Atomic size_t wait_channels_closed;
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
    size_t spawn_count = atomic_load(&g_nursery_timing.spawn_count);
    size_t wait_calls = atomic_load(&g_nursery_timing.wait_calls);
    uint64_t setup = atomic_load(&g_nursery_timing.setup_cycles);
    uint64_t spawn = atomic_load(&g_nursery_timing.fiber_spawn_cycles);
    uint64_t mutex = atomic_load(&g_nursery_timing.mutex_cycles);
    uint64_t spawn_total = atomic_load(&g_nursery_timing.spawn_total_cycles);
    uint64_t wait_join = atomic_load(&g_nursery_timing.wait_join_cycles);
    uint64_t wait_free = atomic_load(&g_nursery_timing.wait_free_cycles);
    uint64_t wait_close = atomic_load(&g_nursery_timing.wait_close_cycles);
    uint64_t wait_total = atomic_load(&g_nursery_timing.wait_total_cycles);
    size_t wait_tasks = atomic_load(&g_nursery_timing.wait_tasks_joined);
    size_t wait_closes = atomic_load(&g_nursery_timing.wait_channels_closed);

    if (spawn_count) {
        fprintf(stderr, "\n=== NURSERY SPAWN TIMING (%zu spawns) ===\n", spawn_count);
        fprintf(stderr, "  Total:        %8.1f cycles/spawn (100.0%%)\n", (double)spawn_total / spawn_count);
        fprintf(stderr, "  Breakdown:\n");
        fprintf(stderr, "    setup:       %8.1f cycles/spawn (%5.1f%%)\n",
                (double)setup / spawn_count, 100.0 * setup / spawn_total);
        fprintf(stderr, "    fiber_spawn: %8.1f cycles/spawn (%5.1f%%)\n",
                (double)spawn / spawn_count, 100.0 * spawn / spawn_total);
        fprintf(stderr, "    mutex:       %8.1f cycles/spawn (%5.1f%%)\n",
                (double)mutex / spawn_count, 100.0 * mutex / spawn_total);
    }
    if (wait_calls) {
        fprintf(stderr, "\n=== NURSERY WAIT TIMING (%zu waits) ===\n", wait_calls);
        fprintf(stderr, "  Total:        %8.1f cycles/wait (100.0%%)\n", (double)wait_total / wait_calls);
        fprintf(stderr, "  Tasks joined: %8.1f tasks/wait\n",
                wait_calls ? (double)wait_tasks / wait_calls : 0.0);
        fprintf(stderr, "  Ch closed:    %8.1f chans/wait\n",
                wait_calls ? (double)wait_closes / wait_calls : 0.0);
        fprintf(stderr, "  Breakdown:\n");
        fprintf(stderr, "    join:        %8.1f cycles/wait (%5.1f%%)  %.1f cycles/task\n",
                (double)wait_join / wait_calls,
                wait_total ? 100.0 * wait_join / wait_total : 0.0,
                wait_tasks ? (double)wait_join / wait_tasks : 0.0);
        fprintf(stderr, "    free:        %8.1f cycles/wait (%5.1f%%)  %.1f cycles/task\n",
                (double)wait_free / wait_calls,
                wait_total ? 100.0 * wait_free / wait_total : 0.0,
                wait_tasks ? (double)wait_free / wait_tasks : 0.0);
        fprintf(stderr, "    close:       %8.1f cycles/wait (%5.1f%%)  %.1f cycles/chan\n",
                (double)wait_close / wait_calls,
                wait_total ? 100.0 * wait_close / wait_total : 0.0,
                wait_closes ? (double)wait_close / wait_closes : 0.0);
    }
    if (spawn_count || wait_calls) {
        fprintf(stderr, "==========================================\n\n");
    }
}

/* Forward declaration for fiber_task */
typedef struct fiber_task fiber_task;

/* Fiber scheduler functions */
fiber_task* cc_fiber_spawn(void* (*fn)(void*), void* arg);
int cc_fiber_join(fiber_task* t, void** out_result);
void cc_fiber_task_free(fiber_task* t);
void cc_fiber_set_spawn_nursery_override(CCNursery* nursery);

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
    CCArena closure_env_arena;
    pthread_mutex_t mu;
    wake_primitive cancel_wake;  /* Broadcast on cancel for O(1) wake */
};

/* Defined in channel.c (same translation unit via runtime/concurrent_c.c). */
void cc__chan_set_autoclose_owner(CCChan* ch, CCNursery* owner);

int cc_nursery_add_closing_tx(CCNursery* n, CCChanTx tx) {
    return cc_nursery_add_closing_chan(n, tx.raw);
}

static CCNursery* cc__nursery_alloc(void) {
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
    wake_primitive_init(&n->cancel_wake);
    n->deadline.tv_sec = 0;
    n->deadline.tv_nsec = 0;
    n->closing = NULL;
    n->closing_cap = 0;
    n->closing_count = 0;
    n->closure_env_arena = cc_arena_heap(1024);
    if (!n->closure_env_arena.base) {
        pthread_mutex_destroy(&n->mu);
        wake_primitive_destroy(&n->cancel_wake);
        free(n->tasks);
        free(n);
        return NULL;
    }
    return n;
}

CCNursery* cc_nursery_create(void) {
    return cc__nursery_alloc();
}

CCNursery* cc_nursery_create_child(CCNursery* parent) {
    CCNursery* n = cc__nursery_alloc();
    if (!n) return NULL;
    if (!parent) return n;

    /* Low-risk inheritance for the new explicit nursery-handle path:
       snapshot current parent cancellation/deadline state without creating
       a live parent pointer dependency. This keeps the helper safe even while
       destroy/ownership ordering is still being designed. */
    if (cc_nursery_is_cancelled(parent)) {
        n->cancelled = 1;
    }
    {
        struct timespec inherited_deadline;
        if (cc_nursery_deadline(parent, &inherited_deadline)) {
            n->deadline = inherited_deadline;
        }
    }
    return n;
}

void* cc_nursery_closure_env_alloc(CCNursery* n, size_t size, size_t align) {
    if (!n || size == 0) return NULL;
    /* TODO: The arena-backed path gives closures under a nursery a clean,
       deterministic lifetime model, but the spawn benchmark breakdown showed
       env alloc/free is not the throughput bottleneck. If we revisit this for
       performance, prototype a nursery-scoped reclaimable allocator (local heap
       or pooled size classes) under the same explicit lowering shape. */
    return cc_arena_alloc(&n->closure_env_arena, size, align);
}

void cc_nursery_cancel(CCNursery* n) {
    if (!n) return;
    pthread_mutex_lock(&n->mu);
    n->cancelled = 1;
    /* Snapshot tasks while holding lock */
    size_t task_count = n->count;
    fiber_task** tasks_snapshot = NULL;
    if (task_count > 0) {
        tasks_snapshot = (fiber_task**)malloc(task_count * sizeof(fiber_task*));
        if (tasks_snapshot) {
            for (size_t i = 0; i < task_count; i++) {
                tasks_snapshot[i] = n->tasks[i];
            }
        }
    }
    pthread_mutex_unlock(&n->mu);
    
    /* Broadcast to wake any fibers waiting on this nursery's cancel primitive */
    wake_primitive_wake_all(&n->cancel_wake);
    
    /* Unpark all tasks in this nursery so they can check cancellation.
     * This is O(n) but ensures no fiber stays parked after cancel. */
    if (tasks_snapshot) {
        for (size_t i = 0; i < task_count; i++) {
            if (tasks_snapshot[i]) {
                cc_sched_fiber_wake((CCSchedFiber*)tasks_snapshot[i]);
            }
        }
        free(tasks_snapshot);
    }
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

/* Check if current fiber's nursery is cancelled (convenience for user code). */
bool cc_cancelled(void) {
    return cc_nursery_is_cancelled(cc__tls_current_nursery);
}

/* Get the cancel wake generation for the current nursery (0 if none).
 * Used by channel waits to detect cancellation. */
uint32_t cc_nursery_cancel_gen(const CCNursery* n) {
    if (!n) return 0;
    return atomic_load_explicit(&n->cancel_wake.value, memory_order_acquire);
}

/* Wait on the nursery's cancel primitive with timeout (ms).
 * Returns immediately if cancel_gen changed (i.e., cancelled). */
void cc_nursery_cancel_wait(CCNursery* n, uint32_t expected_gen, uint32_t timeout_ms) {
    if (!n) return;
    wake_primitive_wait_timeout(&n->cancel_wake, expected_gen, timeout_ms);
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

    if (timing) t1 = t0;

    /* Spawn task on fiber scheduler.
     * Non-worker sibling grouping is runtime-gated because it can help some
     * startup locality cases but regress broader workloads like pigz. */
    cc_sched_nonworker_spawn_group_hint((const void*)n, 2);
    cc_fiber_set_spawn_nursery_override(n);
    fiber_task* t = cc_fiber_spawn(fn, arg);
    cc_fiber_set_spawn_nursery_override(NULL);
    if (!t) {
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
            return grow_err;
        }
    }
    n->tasks[n->count++] = t;
    pthread_mutex_unlock(&n->mu);
    
    if (timing) {
        t3 = nursery_rdtsc();
        atomic_fetch_add_explicit(&g_nursery_timing.setup_cycles, t1 - t0, memory_order_relaxed);
        atomic_fetch_add_explicit(&g_nursery_timing.fiber_spawn_cycles, t2 - t1, memory_order_relaxed);
        atomic_fetch_add_explicit(&g_nursery_timing.mutex_cycles, t3 - t2, memory_order_relaxed);
        atomic_fetch_add_explicit(&g_nursery_timing.spawn_total_cycles, t3 - t0, memory_order_relaxed);
        atomic_fetch_add_explicit(&g_nursery_timing.spawn_count, 1, memory_order_relaxed);
    }
    
    return 0;
}

int cc_nursery_wait(CCNursery* n) {
    if (!n) return EINVAL;
    int first_err = 0;
    int timing = nursery_timing_enabled();
    uint64_t t0 = 0;
    uint64_t join_cycles = 0;
    uint64_t free_cycles = 0;
    uint64_t close_cycles = 0;
    size_t joined = 0;
    size_t closed = 0;
    if (timing) t0 = nursery_rdtsc();
    
    /* Join all tasks - spec: join children first, then close channels.
     * If cancelled, fibers should exit promptly when they check cc_cancelled()
     * or when channel operations return ECANCELED. */
    for (size_t i = 0; i < n->count; ++i) {
        if (!n->tasks[i]) continue;
        uint64_t step0 = timing ? nursery_rdtsc() : 0;
        int err = cc_fiber_join(n->tasks[i], NULL);
        uint64_t step1 = timing ? nursery_rdtsc() : 0;
        if (first_err == 0 && err != 0) {
            first_err = err;
        }
        cc_fiber_task_free(n->tasks[i]);
        uint64_t step2 = timing ? nursery_rdtsc() : 0;
        if (timing) {
            join_cycles += step1 - step0;
            free_cycles += step2 - step1;
        }
        n->tasks[i] = NULL;
        joined++;
    }
    
    /* Close registered channels */
    for (size_t i = 0; i < n->closing_count; ++i) {
        if (n->closing[i]) {
            uint64_t step0 = timing ? nursery_rdtsc() : 0;
            cc_chan_close(n->closing[i]);
            uint64_t step1 = timing ? nursery_rdtsc() : 0;
            if (timing) {
                close_cycles += step1 - step0;
            }
            closed++;
        }
    }
    n->count = 0;
    if (timing) {
        uint64_t t1 = nursery_rdtsc();
        atomic_fetch_add_explicit(&g_nursery_timing.wait_join_cycles, join_cycles, memory_order_relaxed);
        atomic_fetch_add_explicit(&g_nursery_timing.wait_free_cycles, free_cycles, memory_order_relaxed);
        atomic_fetch_add_explicit(&g_nursery_timing.wait_close_cycles, close_cycles, memory_order_relaxed);
        atomic_fetch_add_explicit(&g_nursery_timing.wait_total_cycles, t1 - t0, memory_order_relaxed);
        atomic_fetch_add_explicit(&g_nursery_timing.wait_calls, 1, memory_order_relaxed);
        atomic_fetch_add_explicit(&g_nursery_timing.wait_tasks_joined, joined, memory_order_relaxed);
        atomic_fetch_add_explicit(&g_nursery_timing.wait_channels_closed, closed, memory_order_relaxed);
    }
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
    cc_arena_free(&n->closure_env_arena);
    pthread_mutex_destroy(&n->mu);
    wake_primitive_destroy(&n->cancel_wake);
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
