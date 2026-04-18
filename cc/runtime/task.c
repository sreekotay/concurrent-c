/*
 * Unified CCTask runtime (async, future, and spawn tasks).
 */

#include <ccc/std/task.cch>
#include <ccc/cc_sched.cch>

#include <ccc/cc_exec.cch>
#include <ccc/cc_channel.cch>
#include <ccc/cc_nursery.cch>
#include <ccc/cc_atomic.cch>
#include "fiber_internal.h"
#include "sched_v2.h"

/* Unified deadlock tracking (defined in fiber_sched.c) */
void cc__deadlock_thread_block(void);
void cc__deadlock_thread_unblock(void);
void cc__fiber_set_worker_affinity(int worker_id);

#include <errno.h>

#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* Internal task data layouts (stored in CCTask._data) */
typedef struct {
    CCChan* done;
    volatile int cancelled;
    intptr_t result;
    CCClosure0 c;
} CCTaskHeap;

#ifndef CC_TASK_INTERNAL_TYPES_DEFINED
#define CC_TASK_INTERNAL_TYPES_DEFINED
/* Internal representation for FUTURE kind tasks */
typedef struct {
    CCFuture fut;
    void* heap;
} CCTaskFutureInternal;

/* Internal representation for POLL kind tasks */
typedef struct {
    cc_task_poll_fn poll;
    int (*wait)(void* frame);
    void* frame;
    void (*drop)(void* frame);
} CCTaskPollInternal;

/* Internal representation for SPAWN kind tasks */
typedef struct {
    struct CCSpawnTask* spawn;
} CCTaskSpawnInternal;

/* Internal representation for V2 hybrid scheduler fiber tasks.
 *
 * Post V1 retirement: CC_TASK_KIND_FIBER / CC_TASK_KIND_POOL are ABI-reserved
 * in cc_sched.cch but no code path creates them anymore — every fiber-bearing
 * task is CC_TASK_KIND_FIBER_V2. The old CCTaskFiberInternal / CCTaskPoolInternal
 * internal layouts and the V1 cc_fiber_spawn/join/task_free/... surface have
 * been deleted along with the V1 worker loop. */
typedef struct fiber_v2 fiber_v2;
typedef struct {
    fiber_v2* fiber;
} CCTaskFiberV2Internal;

typedef struct {
    CCTask task;
} CCAsyncTaskV2Bridge;

/* Accessor macros to get internal data from CCTask */
#define TASK_FUTURE(t) ((CCTaskFutureInternal*)((t)->_data))
#define TASK_POLL(t) ((CCTaskPollInternal*)((t)->_data))
#define TASK_SPAWN(t) ((CCTaskSpawnInternal*)((t)->_data))
#define TASK_FIBER_V2(t) ((CCTaskFiberV2Internal*)((t)->_data))
#endif /* CC_TASK_INTERNAL_TYPES_DEFINED */

/* Spawn task poll functions (defined in scheduler.c) */
int cc_thread_task_poll_done(struct CCSpawnTask* task);
void* cc_thread_task_get_result(struct CCSpawnTask* task);
int cc_thread_task_join_fiber(struct CCSpawnTask* task, void** out_result);

/* Fiber context detection (defined in fiber_sched.c) */
int cc__fiber_in_context(void);
void* cc__fiber_current(void);

/* Cooperative yield to global queue (defined in fiber_sched.c) */
void cc__fiber_yield_global(void);

static CCExec* g_task_exec = NULL;
static pthread_mutex_t g_task_exec_mu = PTHREAD_MUTEX_INITIALIZER;
static cc_atomic_u64 g_task_submit_failures = 0;

/* Optional join/wait instrumentation for debugging throughput gaps.
 * Enable with CC_TASK_WAIT_STATS=1 and dump at exit with CC_TASK_WAIT_STATS_DUMP=1. */
void cc__fiber_unpark_stats(uint64_t* out_calls, uint64_t* out_enqueues);
void cc__fiber_join_park_stats(uint64_t* out_joins, uint64_t* out_loops);
void cc__fiber_join_help_stats(uint64_t* out_attempts, uint64_t* out_hits);

typedef struct {
    _Atomic int mode;              /* -1 unknown, 0 off, 1 on */
    _Atomic int dump_mode;         /* -1 unknown, 0 off, 1 on */
    _Atomic int atexit_registered; /* 0/1 */
    _Atomic uint64_t block_calls_total;
    _Atomic uint64_t block_spawn_calls;
    _Atomic uint64_t block_fiber_calls;
    _Atomic uint64_t block_spawn_wait_ns;
    _Atomic uint64_t block_fiber_wait_ns;
    _Atomic uint64_t fiber_join_calls;
    _Atomic uint64_t fiber_join_wait_ns;
    _Atomic uint64_t fiber_result_tls_copies;
} cc_task_wait_stats;

static cc_task_wait_stats g_cc_task_wait_stats = {
    .mode = -1,
    .dump_mode = -1,
    .atexit_registered = 0
};

static inline uint64_t cc__mono_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static int cc_task_wait_stats_enabled(void) {
    int mode = atomic_load_explicit(&g_cc_task_wait_stats.mode, memory_order_acquire);
    if (mode >= 0) return mode;
    mode = (getenv("CC_TASK_WAIT_STATS") || getenv("CC_TASK_WAIT_STATS_DUMP")) ? 1 : 0;
    int expected = -1;
    (void)atomic_compare_exchange_strong_explicit(&g_cc_task_wait_stats.mode,
                                                   &expected,
                                                   mode,
                                                   memory_order_release,
                                                   memory_order_acquire);
    return atomic_load_explicit(&g_cc_task_wait_stats.mode, memory_order_acquire);
}

static int cc_task_wait_stats_dump_enabled(void) {
    int mode = atomic_load_explicit(&g_cc_task_wait_stats.dump_mode, memory_order_acquire);
    if (mode >= 0) return mode;
    mode = getenv("CC_TASK_WAIT_STATS_DUMP") ? 1 : 0;
    int expected = -1;
    (void)atomic_compare_exchange_strong_explicit(&g_cc_task_wait_stats.dump_mode,
                                                   &expected,
                                                   mode,
                                                   memory_order_release,
                                                   memory_order_acquire);
    return atomic_load_explicit(&g_cc_task_wait_stats.dump_mode, memory_order_acquire);
}

static void cc_task_wait_stats_dump(void) {
    if (!cc_task_wait_stats_enabled()) return;
    uint64_t total = atomic_load_explicit(&g_cc_task_wait_stats.block_calls_total, memory_order_relaxed);
    uint64_t spawn_calls = atomic_load_explicit(&g_cc_task_wait_stats.block_spawn_calls, memory_order_relaxed);
    uint64_t fiber_calls = atomic_load_explicit(&g_cc_task_wait_stats.block_fiber_calls, memory_order_relaxed);
    uint64_t spawn_wait_ns = atomic_load_explicit(&g_cc_task_wait_stats.block_spawn_wait_ns, memory_order_relaxed);
    uint64_t fiber_wait_ns = atomic_load_explicit(&g_cc_task_wait_stats.block_fiber_wait_ns, memory_order_relaxed);
    uint64_t fiber_join_calls = atomic_load_explicit(&g_cc_task_wait_stats.fiber_join_calls, memory_order_relaxed);
    uint64_t fiber_join_wait_ns = atomic_load_explicit(&g_cc_task_wait_stats.fiber_join_wait_ns, memory_order_relaxed);
    uint64_t tls_copies = atomic_load_explicit(&g_cc_task_wait_stats.fiber_result_tls_copies, memory_order_relaxed);
    uint64_t unpark_calls = 0;
    uint64_t unpark_enqueues = 0;
    uint64_t join_park_joins = 0;
    uint64_t join_park_loops = 0;
    uint64_t help_attempts = 0;
    uint64_t help_hits = 0;
    cc__fiber_unpark_stats(&unpark_calls, &unpark_enqueues);
    cc__fiber_join_park_stats(&join_park_joins, &join_park_loops);
    cc__fiber_join_help_stats(&help_attempts, &help_hits);

    fprintf(stderr, "\n=== CC_TASK_WAIT_STATS ===\n");
    fprintf(stderr, "block_on calls: total=%llu spawn=%llu fiber=%llu\n",
            (unsigned long long)total,
            (unsigned long long)spawn_calls,
            (unsigned long long)fiber_calls);
    if (spawn_calls) {
        fprintf(stderr, "spawn join wait: total=%.3f ms avg=%.3f us\n",
                (double)spawn_wait_ns / 1000000.0,
                (double)spawn_wait_ns / (double)spawn_calls / 1000.0);
    }
    if (fiber_calls) {
        fprintf(stderr, "fiber block wait: total=%.3f ms avg=%.3f us\n",
                (double)fiber_wait_ns / 1000000.0,
                (double)fiber_wait_ns / (double)fiber_calls / 1000.0);
    }
    if (fiber_join_calls) {
        fprintf(stderr, "fiber join wait: total=%.3f ms avg=%.3f us\n",
                (double)fiber_join_wait_ns / 1000000.0,
                (double)fiber_join_wait_ns / (double)fiber_join_calls / 1000.0);
        fprintf(stderr, "fiber wake path: unpark_calls=%llu enqueues=%llu per_join=(%.3f/%.3f)\n",
                (unsigned long long)unpark_calls,
                (unsigned long long)unpark_enqueues,
                (double)unpark_calls / (double)fiber_join_calls,
                (double)unpark_enqueues / (double)fiber_join_calls);
        if (join_park_joins) {
            fprintf(stderr, "fiber join park loops: joins=%llu loops=%llu avg_loops=%.3f\n",
                    (unsigned long long)join_park_joins,
                    (unsigned long long)join_park_loops,
                    (double)join_park_loops / (double)join_park_joins);
        }
        if (help_attempts) {
            fprintf(stderr, "fiber help-first join: attempts=%llu hits=%llu hit_rate=%.1f%%\n",
                    (unsigned long long)help_attempts,
                    (unsigned long long)help_hits,
                    100.0 * (double)help_hits / (double)help_attempts);
        }
    }
    fprintf(stderr, "fiber result TLS copies: %llu\n",
            (unsigned long long)tls_copies);
    fprintf(stderr, "==========================\n");
}

static inline void cc_task_wait_stats_maybe_init(void) {
    if (!cc_task_wait_stats_enabled() || !cc_task_wait_stats_dump_enabled()) return;
    int expected = 0;
    if (atomic_compare_exchange_strong_explicit(&g_cc_task_wait_stats.atexit_registered,
                                                &expected,
                                                1,
                                                memory_order_acq_rel,
                                                memory_order_acquire)) {
        atexit(cc_task_wait_stats_dump);
    }
}

/* cc__env_size defined in scheduler.c */

/* ================================================================
 * Runtime-level fiber task pool [RETIRED]
 *
 * The CCFiberPool / CCPoolSlot / cc_pool_runner_fn infrastructure was a
 * V1-only optimization: a static set of 8 V1 fibers loop-consuming a work
 * channel so cc_fiber_spawn_task could skip the per-spawn fiber allocation.
 * cc_fpool_ensure_init() was never called from anywhere in the codebase, so
 * the pool was already dormant at V2 promotion time. With V1 retired the
 * entire stack is gone; cc_fiber_spawn_task now aliases directly to
 * cc_fiber_spawn_task_v2 (see below). CC_TASK_KIND_POOL is kept in the
 * ABI enum for source-compat but no code path ever creates one.
 * ================================================================ */

static size_t cc__default_blocking_workers(void) {
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    if (n > 0 && n < 4) return (size_t)n;
    return 4;
}

static CCExec* cc__task_exec_lazy(void) {
    pthread_mutex_lock(&g_task_exec_mu);
    if (!g_task_exec) {
        size_t workers = cc__env_size("CC_BLOCKING_WORKERS", cc__default_blocking_workers());
        size_t qcap = cc__env_size("CC_BLOCKING_QUEUE_CAP", 256);
        g_task_exec = cc_exec_create(workers, qcap);
    }
    CCExec* ex = g_task_exec;
    pthread_mutex_unlock(&g_task_exec_mu);
    return ex;
}

static void cc__task_job(void* arg) {
    CCTaskHeap* h = (CCTaskHeap*)arg;
    if (!h) return;

    int err = 0;
    if (h->cancelled) {
        err = ECANCELED;
    } else if (h->c.fn) {
        void* r = h->c.fn(h->c.env);
        h->result = (intptr_t)r;
        if (h->c.drop) h->c.drop(h->c.env);
    } else {
        err = EINVAL;
    }

    if (h->done) {
        (void)cc_chan_send(h->done, &err, sizeof(err));
    }
}

CCTask cc_run_blocking_task(CCClosure0 c) {
    CCTask out;
    memset(&out, 0, sizeof(out));
    if (!c.fn) return out;

    CCExec* ex = cc__task_exec_lazy();
    if (!ex) return out;

    CCTaskHeap* h = (CCTaskHeap*)calloc(1, sizeof(CCTaskHeap));
    if (!h) return out;
    h->done = cc_chan_create(1);
    if (!h->done) {
        free(h);
        return out;
    }
    h->cancelled = 0;
    h->result = 0;
    h->c = c;

    out.kind = CC_TASK_KIND_FUTURE;
    CCTaskFutureInternal* fut = TASK_FUTURE(&out);
    cc_future_init(&fut->fut);
    fut->fut.handle.done = h->done;
    fut->fut.handle.cancelled = 0;
    fut->fut.result = &h->result;
    fut->heap = h;

    int sub = cc_exec_submit(ex, cc__task_job, h);
    if (sub != 0) {
        cc_atomic_fetch_add(&g_task_submit_failures, 1);
        if (h->done) {
            cc_chan_close(h->done);
            cc_chan_free(h->done);
            h->done = NULL;
        }
        free(h);
        memset(&out, 0, sizeof(out));
        return out;
    }
    return out;
}

int cc_blocking_pool_stats(CCExecStats* out_exec, uint64_t* out_submit_failures) {
    if (out_submit_failures) {
        *out_submit_failures = (uint64_t)cc_atomic_load(&g_task_submit_failures);
    }
    CCExec* ex = cc__task_exec_lazy();
    if (!out_exec) return ex ? 0 : ENOMEM;
    if (!ex) {
        memset(out_exec, 0, sizeof(*out_exec));
        return 0;
    }
    return cc_exec_stats(ex, out_exec);
}

CCFutureStatus cc_task_poll(CCTask* t, intptr_t* out_val, int* out_err) {
    if (!t) return CC_FUTURE_ERR;
    if (t->kind == CC_TASK_KIND_FUTURE) {
        CCTaskFutureInternal* fut = TASK_FUTURE(t);
        CCFutureStatus st = cc_future_poll(&fut->fut, out_err);
        if (st == CC_FUTURE_READY && out_val && fut->fut.result) {
            *out_val = *(const intptr_t*)fut->fut.result;
        }
        return st;
    }
    if (t->kind == CC_TASK_KIND_POLL) {
        CCTaskPollInternal* p = TASK_POLL(t);
        if (!p->poll) return CC_FUTURE_ERR;
        return p->poll(p->frame, out_val, out_err);
    }
    if (t->kind == CC_TASK_KIND_SPAWN) {
        CCTaskSpawnInternal* s = TASK_SPAWN(t);
        if (!s->spawn) return CC_FUTURE_ERR;
        if (cc_thread_task_poll_done(s->spawn)) {
            if (out_val) *out_val = (intptr_t)cc_thread_task_get_result(s->spawn);
            if (out_err) *out_err = 0;
            return CC_FUTURE_READY;
        }
        return CC_FUTURE_PENDING;
    }
    if (t->kind == CC_TASK_KIND_FIBER_V2) {
        CCTaskFiberV2Internal* fv = TASK_FIBER_V2(t);
        if (!fv->fiber) return CC_FUTURE_ERR;
        if (sched_v2_fiber_done(fv->fiber)) {
            if (out_val) *out_val = (intptr_t)sched_v2_fiber_result(fv->fiber);
            if (out_err) *out_err = 0;
            return CC_FUTURE_READY;
        }
        return CC_FUTURE_PENDING;
    }
    return CC_FUTURE_ERR;
}

CCTask cc_task_make_poll(cc_task_poll_fn poll, void* frame, void (*drop)(void*)) {
    CCTask t;
    memset(&t, 0, sizeof(t));
    if (!poll || !frame) return t;
    t.kind = CC_TASK_KIND_POLL;
    CCTaskPollInternal* p = TASK_POLL(&t);
    p->poll = poll;
    p->wait = NULL;
    p->frame = frame;
    p->drop = drop;
    return t;
}

CCTask cc_task_make_poll_ex(cc_task_poll_fn poll, int (*wait)(void*), void* frame, void (*drop)(void*)) {
    CCTask t;
    memset(&t, 0, sizeof(t));
    if (!poll || !frame) return t;
    t.kind = CC_TASK_KIND_POLL;
    CCTaskPollInternal* p = TASK_POLL(&t);
    p->poll = poll;
    p->wait = wait;
    p->frame = frame;
    p->drop = drop;
    return t;
}

void cc_task_free(CCTask* t) {
    if (!t) return;
    if (t->kind == CC_TASK_KIND_FUTURE) {
        CCTaskFutureInternal* fut = TASK_FUTURE(t);
        CCTaskHeap* h = (CCTaskHeap*)fut->heap;
        if (fut->fut.handle.done) {
            cc_future_free(&fut->fut);
        }
        if (h) {
            h->cancelled = 1;
            free(h);
        }
    } else if (t->kind == CC_TASK_KIND_POLL) {
        CCTaskPollInternal* p = TASK_POLL(t);
        if (p->drop && p->frame) p->drop(p->frame);
        p->poll = NULL;
        p->frame = NULL;
        p->drop = NULL;
    } else if (t->kind == CC_TASK_KIND_SPAWN) {
        CCTaskSpawnInternal* s = TASK_SPAWN(t);
        if (s->spawn) {
            cc_thread_task_free(s->spawn);
        }
    } else if (t->kind == CC_TASK_KIND_FIBER_V2) {
        /* V2 fibers are pool-recycled automatically on completion — nothing to free */
    }
    memset(t, 0, sizeof(*t));
}

static void cc__set_fiber_v2_task(CCTask* t, fiber_v2* f) {
    CCTaskFiberV2Internal* fv = TASK_FIBER_V2(t);
    fv->fiber = f;
}

/* Spawn an M:N fiber task.
 *
 * Post V2-promotion: this is a source-compat alias for
 * cc_fiber_spawn_task_v2. The V1 fiber pool (cc_fpool_*) is no longer
 * initialized or dispatched to; every task is a V2 fiber. The alias is kept
 * so compiler-emitted code (pass_nursery_spawn_ast.c, pass_channel_syntax.c,
 * pass_autoblock.c) and external callers continue to work untouched during
 * the V1 retirement window. */
CCTask cc_fiber_spawn_task(void* (*fn)(void*), void* arg) {
    return cc_fiber_spawn_task_v2(fn, arg);
}

/* Helper function that unpacks and calls a closure for fibers */
static void* cc__fiber_closure0_wrapper(void* arg) {
    CCClosure0* pc = (CCClosure0*)arg;
    void* result = pc->fn(pc->env);
    if (pc->drop) pc->drop(pc->env);
    free(pc);
    return result;
}

static intptr_t cc__task_take_v2_result(fiber_v2* fiber, void* result) {
    static __thread char tls_v2_result[48] __attribute__((aligned(8)));
    if (!fiber || !result) return 0;

    char* buf = sched_v2_fiber_result_buf(fiber);
    if (buf && (char*)result >= buf && (char*)result < buf + 48) {
        memcpy(tls_v2_result, result, sizeof(tls_v2_result));
        return (intptr_t)tls_v2_result;
    }
    return (intptr_t)result;
}

/* Spawn a fiber from a 0-arg closure.
 *
 * Post V2-promotion: alias for cc_fiber_spawn_closure0_v2 (the
 * cc_fiber_spawn_task it calls is itself now the V2 alias, so this would
 * route to V2 regardless, but we go direct to _v2 to skip the extra hop). */
CCTask cc_fiber_spawn_closure0(CCClosure0 c) {
    return cc_fiber_spawn_closure0_v2(c);
}

CCTask cc_fiber_spawn_task_v2(void* (*fn)(void*), void* arg) {
    CCTask out;
    memset(&out, 0, sizeof(out));
    if (!fn) return out;
    fiber_v2* f = sched_v2_spawn(fn, arg);
    if (!f) return out;
    out.kind = CC_TASK_KIND_FIBER_V2;
    cc__set_fiber_v2_task(&out, f);
    return out;
}

CCTask cc_fiber_spawn_closure0_v2(CCClosure0 c) {
    CCTask out;
    memset(&out, 0, sizeof(out));
    if (!c.fn) return out;

    CCClosure0* heap_c = (CCClosure0*)malloc(sizeof(CCClosure0));
    if (!heap_c) return out;
    *heap_c = c;

    out = cc_fiber_spawn_task_v2(cc__fiber_closure0_wrapper, heap_c);
    if (out.kind == CC_TASK_KIND_INVALID) {
        free(heap_c);
    }
    return out;
}

static void* cc__async_task_v2_bridge_runner(void* arg) {
    CCAsyncTaskV2Bridge* bridge = (CCAsyncTaskV2Bridge*)arg;
    CCTask task;
    intptr_t r = 0;

    if (!bridge) return NULL;
    task = bridge->task;
    free(bridge);
    r = cc_block_on_intptr(task);
    return (void*)r;
}

typedef struct {
    int yielded;
} CCTaskYieldFrame;

static CCFutureStatus cc__task_yield_poll(void* frame, intptr_t* out_val, int* out_err) {
    CCTaskYieldFrame* f = (CCTaskYieldFrame*)frame;
    if (out_err) *out_err = 0;
    if (!f) return CC_FUTURE_ERR;
    if (!f->yielded) {
        f->yielded = 1;
        return CC_FUTURE_PENDING;
    }
    if (out_val) *out_val = 0;
    return CC_FUTURE_READY;
}

static int cc__task_yield_wait(void* frame) {
    (void)frame;
    if (cc__fiber_in_context()) {
        cc__fiber_yield_global();
        return 0;
    }
    sched_yield();
    return 0;
}

static void cc__task_yield_drop(void* frame) {
    free(frame);
}

CCTask cc_task_yield_once(void) {
    CCTaskYieldFrame* frame = (CCTaskYieldFrame*)calloc(1, sizeof(*frame));
    if (!frame) {
        CCTask t;
        memset(&t, 0, sizeof(t));
        return t;
    }
    return cc_task_make_poll_ex(cc__task_yield_poll, cc__task_yield_wait, frame, cc__task_yield_drop);
}

CCTask cc_async_closure0_start_v2(CCAsyncClosure0 c) {
    CCTask task = cc_async_closure0_start(c);
    CCAsyncTaskV2Bridge* bridge;
    CCTask out;

    if (task.kind == CC_TASK_KIND_INVALID) return task;
    if (task.kind == CC_TASK_KIND_FIBER || task.kind == CC_TASK_KIND_FIBER_V2) return task;

    bridge = (CCAsyncTaskV2Bridge*)malloc(sizeof(*bridge));
    if (!bridge) {
        cc_task_free(&task);
        memset(&out, 0, sizeof(out));
        return out;
    }
    bridge->task = task;
    out = cc_fiber_spawn_task_v2(cc__async_task_v2_bridge_runner, bridge);
    if (out.kind == CC_TASK_KIND_INVALID) {
        cc_task_free(&task);
        free(bridge);
    }
    return out;
}

/* Cancel a task and wake up anyone blocked on it.
   This closes the done channel, causing cc_block_on_intptr to return immediately. */
void cc_task_cancel(CCTask* t) {
    if (!t) return;
    if (t->kind == CC_TASK_KIND_FUTURE) {
        CCTaskFutureInternal* fut = TASK_FUTURE(t);
        CCTaskHeap* h = (CCTaskHeap*)fut->heap;
        if (h) {
            h->cancelled = 1;
            /* Close the done channel to wake up blocked waiters */
            if (h->done) {
                cc_chan_close(h->done);
            }
        }
        /* Also mark the future handle as cancelled */
        fut->fut.handle.cancelled = 1;
    } else if (t->kind == CC_TASK_KIND_POLL) {
        /* For poll-based tasks, we can't easily cancel - just mark frame for cleanup */
        /* The poll function should check cancellation state */
    } else if (t->kind == CC_TASK_KIND_SPAWN) {
        /* Spawn tasks can't be cancelled mid-flight - pthread doesn't support that safely */
    }
}

intptr_t cc_block_on_intptr(CCTask t) {
    intptr_t r = 0;
    int err = 0;
    const int wait_stats = cc_task_wait_stats_enabled();
    if (wait_stats) {
        cc_task_wait_stats_maybe_init();
        atomic_fetch_add_explicit(&g_cc_task_wait_stats.block_calls_total, 1, memory_order_relaxed);
    }
    cc__deadlock_thread_block();  /* Track that this thread is blocking */
    
    /* Handle spawn tasks directly with join */
    if (t.kind == CC_TASK_KIND_SPAWN) {
        CCTaskSpawnInternal* s = TASK_SPAWN(&t);
        uint64_t wait_start_ns = 0;
        if (wait_stats) {
            atomic_fetch_add_explicit(&g_cc_task_wait_stats.block_spawn_calls, 1, memory_order_relaxed);
            wait_start_ns = cc__mono_ns();
        }
        if (s->spawn) {
            void* result = NULL;
            /* Use fiber-aware join when called from fiber context: park the
             * fiber instead of blocking the OS worker thread via condvar.
             * This keeps the worker free to run other fibers during the wait
             * and avoids a kernel condvar round-trip on every completion. */
            if (cc__fiber_in_context()) {
                cc_thread_task_join_fiber(s->spawn, &result);
            } else {
                cc_thread_task_join_result(s->spawn, &result);
            }
            r = (intptr_t)result;
            cc_thread_task_free(s->spawn);
        }
        if (wait_stats) {
            uint64_t wait_ns = cc__mono_ns() - wait_start_ns;
            atomic_fetch_add_explicit(&g_cc_task_wait_stats.block_spawn_wait_ns, wait_ns, memory_order_relaxed);
        }
        cc__deadlock_thread_unblock();
        return r;
    }
    
    if (t.kind == CC_TASK_KIND_FIBER_V2) {
        CCTaskFiberV2Internal* fv = TASK_FIBER_V2(&t);
        if (fv->fiber) {
            void* result = NULL;
            sched_v2_join(fv->fiber, &result);
            r = cc__task_take_v2_result(fv->fiber, result);
            sched_v2_fiber_release(fv->fiber);
        }
        cc__deadlock_thread_unblock();
        return r;
    }
    
    for (;;) {
        CCFutureStatus st = cc_task_poll(&t, &r, &err);
        if (st == CC_FUTURE_PENDING) {
            if (t.kind == CC_TASK_KIND_FUTURE) {
                /* For "future" tasks, block directly on the done channel once and then return the result.
                   This avoids spin-polling and avoids needing to preserve the completion for poll(). */
                CCTaskFutureInternal* fut = TASK_FUTURE(&t);
                err = cc_async_wait(&fut->fut.handle);
                if (err == 0 && fut->fut.result) r = *(const intptr_t*)fut->fut.result;
                break;
            } else if (t.kind == CC_TASK_KIND_POLL) {
                CCTaskPollInternal* p = TASK_POLL(&t);
                if (p->wait) {
                    /* Task has a wait function - use it to block efficiently */
                    (void)p->wait(p->frame);
                }
            }
            /* For POLL tasks without wait: tight loop. These are pure state machines
               making progress on every poll (no external blocking). No yield needed. */
            continue;
        }
        break;
    }
    cc__deadlock_thread_unblock();
    cc_task_free(&t);
    (void)err;
    return r;
}


/* --- cc_block_all implementation --- */

typedef struct {
    CCTask task;            /* Copy of the task (we own it) */
    intptr_t* result_slot;  /* Where to store the result */
} CCBlockAllSlot;

static void* cc__block_all_worker(void* arg) {
    CCBlockAllSlot* slot = (CCBlockAllSlot*)arg;
    if (!slot) return NULL;
    intptr_t r = cc_block_on_intptr(slot->task);
    if (slot->result_slot) *slot->result_slot = r;
    return NULL;
}

static int cc__task_kind_inline_block_safe(CCTask t) {
    switch (t.kind) {
        case CC_TASK_KIND_INVALID:
        case CC_TASK_KIND_SPAWN:
        case CC_TASK_KIND_FIBER_V2:
            return 1;
        case CC_TASK_KIND_FUTURE:
        case CC_TASK_KIND_POLL:
        default:
            return 0;
    }
}

/* Block until all tasks complete. Runs tasks concurrently using a nursery.
   Returns 0 on success, error code if any task fails.
   Results are stored in the results array (must be at least count elements).
   Note: Takes ownership of the tasks (they are freed after completion). */
int cc_block_all(int count, CCTask* tasks, intptr_t* results) {
    if (count <= 0) return 0;
    if (!tasks) return EINVAL;

    /* Fast path: tasks that are already started/running can be joined inline.
     * This avoids spawning a second layer of waiter fibers just to call
     * cc_block_on_intptr() on existing fiber/spawn/pool tasks. Keep the
     * nursery-wrapper path for FUTURE/POLL tasks so lazy async state machines
     * still make progress concurrently. */
    int inline_safe = 1;
    for (int i = 0; i < count; i++) {
        if (!cc__task_kind_inline_block_safe(tasks[i])) {
            inline_safe = 0;
            break;
        }
    }
    if (inline_safe) {
        for (int i = 0; i < count; i++) {
            intptr_t r = cc_block_on_intptr(tasks[i]);
            if (results) results[i] = r;
        }
        return 0;
    }

    CCNursery* n = cc_nursery_create();
    if (!n) return ENOMEM;

    CCBlockAllSlot* slots = (CCBlockAllSlot*)calloc((size_t)count, sizeof(CCBlockAllSlot));
    if (!slots) {
        cc_nursery_free(n);
        return ENOMEM;
    }

    for (int i = 0; i < count; i++) {
        slots[i].task = tasks[i];  /* Copy task */
        slots[i].result_slot = results ? &results[i] : NULL;
    }

    /* Spawn a waiter fiber for each task */
    for (int i = 0; i < count; i++) {
        int err = cc_nursery_spawn(n, cc__block_all_worker, &slots[i]);
        if (err != 0) {
            cc_nursery_cancel(n);
            cc_nursery_wait(n);
            cc_nursery_free(n);
            free(slots);
            return err;
        }
    }

    int err = cc_nursery_wait(n);
    cc_nursery_free(n);
    free(slots);
    return err;
}

/* --- cc_block_race implementation --- */

typedef struct {
    CCTask task;
    int index;
    CCChan* done_chan;  /* Shared channel to signal completion */
    intptr_t result;
    int error;
    volatile int* winner_flag;  /* Set to 1 when first completes */
} CCBlockRaceSlot;

typedef struct {
    int index;
    intptr_t result;
    int error;
} CCBlockRaceResult;

static void* cc__block_race_worker(void* arg) {
    CCBlockRaceSlot* slot = (CCBlockRaceSlot*)arg;
    if (!slot) return NULL;
    
    slot->result = cc_block_on_intptr(slot->task);
    slot->error = 0;  /* TODO: capture actual errors */
    
    /* Signal completion */
    CCBlockRaceResult msg = { slot->index, slot->result, slot->error };
    cc_chan_send(slot->done_chan, &msg, sizeof(msg));
    
    return NULL;
}

/* Block until first task completes. Returns immediately when any task finishes.
   winner: index of the task that completed first
   result: result of the winning task
   Returns 0 on success.
   Note: Other tasks continue running in background (cancelled on nursery cleanup). */
int cc_block_race(int count, CCTask* tasks, int* winner, intptr_t* result) {
    if (count <= 0) return EINVAL;
    if (!tasks) return EINVAL;

    CCChan* done_chan = cc_chan_create(count);
    if (!done_chan) return ENOMEM;

    CCNursery* n = cc_nursery_create();
    if (!n) {
        cc_chan_free(done_chan);
        return ENOMEM;
    }

    volatile int winner_flag = 0;
    CCBlockRaceSlot* slots = (CCBlockRaceSlot*)calloc((size_t)count, sizeof(CCBlockRaceSlot));
    if (!slots) {
        cc_nursery_free(n);
        cc_chan_free(done_chan);
        return ENOMEM;
    }

    for (int i = 0; i < count; i++) {
        slots[i].task = tasks[i];
        slots[i].index = i;
        slots[i].done_chan = done_chan;
        slots[i].winner_flag = &winner_flag;
    }

    /* Spawn all tasks */
    for (int i = 0; i < count; i++) {
        int err = cc_nursery_spawn(n, cc__block_race_worker, &slots[i]);
        if (err != 0) {
            cc_nursery_cancel(n);
            cc_nursery_wait(n);
            cc_nursery_free(n);
            cc_chan_free(done_chan);
            free(slots);
            return err;
        }
    }

    /* Wait for first completion */
    CCBlockRaceResult msg;
    int recv_err = cc_chan_recv(done_chan, &msg, sizeof(msg));
    
    if (winner) *winner = msg.index;
    if (result) *result = msg.result;

    /* Cancel remaining tasks - this wakes up workers blocked in cc_block_on_intptr */
    for (int i = 0; i < count; i++) {
        if (i != msg.index) {
            cc_task_cancel(&slots[i].task);
        }
    }
    
    /* Now wait for all workers to finish (they should exit quickly after cancel) */
    cc_nursery_cancel(n);
    cc_nursery_wait(n);
    cc_nursery_free(n);
    cc_chan_close(done_chan);
    cc_chan_free(done_chan);
    free(slots);

    return recv_err;
}

/* --- cc_block_any implementation --- */

/* Block until first SUCCESSFUL task completes. Only fails if ALL tasks fail.
   Current convention: non-negative result indicates success, negative indicates failure.
   winner: index of the first successful task
   result: result of the winning task
   Returns 0 if any task succeeded, ECANCELED if all failed. */
int cc_block_any(int count, CCTask* tasks, int* winner, intptr_t* result) {
    if (count <= 0) return EINVAL;
    if (!tasks) return EINVAL;

    CCChan* done_chan = cc_chan_create(count);
    if (!done_chan) return ENOMEM;

    CCNursery* n = cc_nursery_create();
    if (!n) {
        cc_chan_free(done_chan);
        return ENOMEM;
    }

    CCBlockRaceSlot* slots = (CCBlockRaceSlot*)calloc((size_t)count, sizeof(CCBlockRaceSlot));
    if (!slots) {
        cc_nursery_free(n);
        cc_chan_free(done_chan);
        return ENOMEM;
    }

    for (int i = 0; i < count; i++) {
        slots[i].task = tasks[i];
        slots[i].index = i;
        slots[i].done_chan = done_chan;
    }

    /* Spawn all tasks */
    for (int i = 0; i < count; i++) {
        int err = cc_nursery_spawn(n, cc__block_race_worker, &slots[i]);
        if (err != 0) {
            cc_nursery_cancel(n);
            cc_nursery_wait(n);
            cc_nursery_free(n);
            cc_chan_free(done_chan);
            free(slots);
            return err;
        }
    }

    /* Wait for first SUCCESS (non-negative result).
       Continue draining completions while tasks fail. */
    int found_success = 0;
    int completed = 0;
    CCBlockRaceResult first_result = {0};
    
    while (completed < count && !found_success) {
        CCBlockRaceResult msg;
        int recv_err = cc_chan_recv(done_chan, &msg, sizeof(msg));
        if (recv_err != 0) break;
        
        completed++;
        
        if (msg.result >= 0) {
            found_success = 1;
            first_result = msg;
        }
    }

    if (found_success) {
        if (winner) *winner = first_result.index;
        if (result) *result = first_result.result;
    } else {
        if (winner) *winner = -1;
        if (result) *result = -1;
    }

    /* Cancel remaining tasks - this wakes up workers blocked in cc_block_on_intptr */
    for (int i = 0; i < count; i++) {
        if (!found_success || i != first_result.index) {
            cc_task_cancel(&slots[i].task);
        }
    }
    
    /* Now wait for all workers to finish */
    cc_nursery_cancel(n);
    cc_nursery_wait(n);
    cc_nursery_free(n);
    cc_chan_close(done_chan);
    cc_chan_free(done_chan);
    free(slots);

    return found_success ? 0 : ECANCELED;
}
