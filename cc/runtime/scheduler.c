/*
 * Executor-backed scheduler facade with cooperative deadlines.
 */

#include <ccc/cc_sched.cch>
#include <ccc/cc_exec.cch>
#include <ccc/std/task.cch>
#include "fiber_internal.h"

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

struct CCSpawnTask {
    void* (*fn)(void*);
    void* arg;
    void* result;
    int done;
    int detached;
    pthread_mutex_t mu;
    pthread_cond_t cv;
    /* Fiber-aware join support.
     * done_atomic mirrors done but is safe to use with cc__fiber_park_if.
     * Set with release ordering AFTER result is stored and AFTER done=1. */
    _Atomic int done_atomic;
    cc__fiber* _Atomic waiter_fiber;
};

/* Accessor to set spawn task in CCTask._data */
static inline void cc__set_spawn_task(CCTask* t, struct CCSpawnTask* task) {
    /* CCTaskSpawnInternal layout: just a pointer at offset 0 */
    struct CCSpawnTask** ptr = (struct CCSpawnTask**)t->_data;
    *ptr = task;
}

static CCExec* g_sched_exec = NULL;
static pthread_mutex_t g_sched_mu = PTHREAD_MUTEX_INITIALIZER;

static size_t cc__env_size(const char* name, size_t fallback) {
    const char* v = getenv(name);
    if (!v || !*v) return fallback;
    char* end = NULL;
    unsigned long n = strtoul(v, &end, 10);
    if (!end || end == v || *end != 0) return fallback;
    return (size_t)n;
}

static size_t cc__default_workers(void) {
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    if (n > 0) return (size_t)n;
    return 4;
}

static CCExec* cc__sched_exec_lazy(void) {
    pthread_mutex_lock(&g_sched_mu);
    if (!g_sched_exec) {
        /* Unify with fiber scheduler: check programmatic setting first, then env */
        size_t workers = cc_sched_get_num_workers();
        if (workers == 0) {
            workers = cc__env_size("CC_WORKERS", cc__default_workers());
        }
        size_t qcap = cc__env_size("CC_SPAWN_QUEUE_CAP", 1024);
        g_sched_exec = cc_exec_create(workers, qcap);
    }
    CCExec* ex = g_sched_exec;
    pthread_mutex_unlock(&g_sched_mu);
    return ex;
}

int cc_scheduler_init(void) {
    return cc__sched_exec_lazy() ? 0 : -1;
}

void cc_scheduler_shutdown(void) {
    pthread_mutex_lock(&g_sched_mu);
    if (g_sched_exec) {
        cc_exec_shutdown(g_sched_exec);
        cc_exec_free(g_sched_exec);
        g_sched_exec = NULL;
    }
    pthread_mutex_unlock(&g_sched_mu);
}

int cc_scheduler_stats(CCSchedulerStats* out) {
    if (!out) return EINVAL;
    CCExec* ex = cc__sched_exec_lazy();
    if (!ex) return ENOMEM;
    CCExecStats stats;
    int err = cc_exec_stats(ex, &stats);
    if (err != 0) return err;
    out->workers = stats.workers;
    out->queue_cap = stats.queue_cap;
    out->queue_len = stats.queue_len;
    return 0;
}

static void cc__spawn_task_free_internal(struct CCSpawnTask* task) {
    if (!task) return;
    pthread_mutex_destroy(&task->mu);
    pthread_cond_destroy(&task->cv);
    free(task);
}

static void cc__spawn_task_job(void* arg) {
    struct CCSpawnTask* task = (struct CCSpawnTask*)arg;
    if (!task) return;
    void* r = NULL;
    if (task->fn) r = task->fn(task->arg);
    pthread_mutex_lock(&task->mu);
    task->result = r;
    task->done = 1;
    /* Grab any waiting fiber under the lock (prevents it missing done=1). */
    cc__fiber* waiter = (cc__fiber*)atomic_exchange_explicit(
        &task->waiter_fiber, NULL, memory_order_acq_rel);
    /* done_atomic release store: ensures result/done are visible to fiber
     * via cc__fiber_park_if's acquire load on done_atomic. */
    atomic_store_explicit(&task->done_atomic, 1, memory_order_release);
    pthread_cond_broadcast(&task->cv);
    int detach = task->detached;
    pthread_mutex_unlock(&task->mu);
    /* Wake fiber outside lock — unpark is safe to call from any thread. */
    if (waiter) cc__fiber_unpark(waiter);
    if (detach) {
        cc__spawn_task_free_internal(task);
    }
}

/* NEW unified API: cc_thread_spawn returns CCTask value */
CCTask cc_thread_spawn(void* (*fn)(void*), void* arg) {
    CCTask out;
    memset(&out, 0, sizeof(out));
    if (!fn) return out;
    CCExec* ex = cc__sched_exec_lazy();
    if (!ex) return out;
    struct CCSpawnTask* task = (struct CCSpawnTask*)calloc(1, sizeof(struct CCSpawnTask));
    if (!task) return out;
    task->fn = fn;
    task->arg = arg;
    pthread_mutex_init(&task->mu, NULL);
    pthread_cond_init(&task->cv, NULL);
    int err = cc_exec_submit(ex, cc__spawn_task_job, task);
    if (err != 0) {
        cc__spawn_task_free_internal(task);
        return out;
    }
    out.kind = CC_TASK_KIND_SPAWN;
    cc__set_spawn_task(&out, task);
    return out;
}

/* Helper function that unpacks and calls a closure */
static void* cc__closure0_wrapper(void* arg) {
    CCClosure0* pc = (CCClosure0*)arg;
    void* result = pc->fn(pc->env);
    if (pc->drop) pc->drop(pc->env);
    free(pc);
    return result;
}

/* NEW unified API: cc_thread_spawn_closure0 returns CCTask value */
CCTask cc_thread_spawn_closure0(CCClosure0 c) {
    /* Wrap closure in a simple function that calls the closure */
    /* For simplicity, we allocate and pass the closure as arg */
    CCTask out;
    memset(&out, 0, sizeof(out));
    if (!c.fn) return out;
    
    /* Create a heap copy of the closure */
    CCClosure0* heap_c = (CCClosure0*)malloc(sizeof(CCClosure0));
    if (!heap_c) return out;
    *heap_c = c;
    
    return cc_thread_spawn(cc__closure0_wrapper, heap_c);
}

/* Legacy API for backward compatibility */
int cc_thread_spawn_legacy(struct CCSpawnTask** out_task, void* (*fn)(void*), void* arg) {
    if (!out_task || !fn) return EINVAL;
    *out_task = NULL;
    CCExec* ex = cc__sched_exec_lazy();
    if (!ex) return ENOMEM;
    struct CCSpawnTask* task = (struct CCSpawnTask*)calloc(1, sizeof(struct CCSpawnTask));
    if (!task) return ENOMEM;
    task->fn = fn;
    task->arg = arg;
    pthread_mutex_init(&task->mu, NULL);
    pthread_cond_init(&task->cv, NULL);
    int err = cc_exec_submit(ex, cc__spawn_task_job, task);
    if (err != 0) {
        cc__spawn_task_free_internal(task);
        return err;
    }
    *out_task = task;
    return 0;
}

int cc_thread_task_join(struct CCSpawnTask* task) {
    if (!task) return EINVAL;
    pthread_mutex_lock(&task->mu);
    while (!task->done) {
        pthread_cond_wait(&task->cv, &task->mu);
    }
    pthread_mutex_unlock(&task->mu);
    return 0;
}

int cc_thread_task_join_result(struct CCSpawnTask* task, void** out_result) {
    if (!task) return EINVAL;
    pthread_mutex_lock(&task->mu);
    while (!task->done) {
        pthread_cond_wait(&task->cv, &task->mu);
    }
    if (out_result) *out_result = task->result;
    pthread_mutex_unlock(&task->mu);
    return 0;
}

/* Fiber-aware join: park the calling fiber instead of blocking the worker thread.
 * Must only be called from within a fiber context (cc__fiber_in_context() == 1).
 *
 * Protocol:
 *   1. Lock mutex, check done — fast path if already done.
 *   2. Register current fiber as waiter (under lock, so completion can't race past it).
 *   3. Unlock, then park on done_atomic via cc__fiber_park_if.
 *      If completion fires between unlock and park, pending_unpark or the flag
 *      check in cc__fiber_park_if prevents the park.
 *   4. On wakeup done_atomic==1, result is visible via release/acquire ordering. */
int cc_thread_task_join_fiber(struct CCSpawnTask* task, void** out_result) {
    if (!task) return EINVAL;
    pthread_mutex_lock(&task->mu);
    if (!task->done) {
        /* Register as waiter under lock so the completion handler can see us. */
        atomic_store_explicit(&task->waiter_fiber,
                              (cc__fiber*)cc__fiber_current(),
                              memory_order_relaxed);
        pthread_mutex_unlock(&task->mu);
        /* Park until done_atomic is set.
         * cc__fiber_park_if checks pending_unpark first, then the flag — no
         * lost wakeup is possible even if completion fires before we yield. */
        CC_FIBER_PARK_IF(&task->done_atomic, 0, "spawn_join");
        /* Clear stale waiter registration in case park bailed on flag or
         * pending_unpark without the completion handler clearing it. */
        atomic_store_explicit(&task->waiter_fiber, NULL, memory_order_relaxed);
    } else {
        pthread_mutex_unlock(&task->mu);
    }
    /* done_atomic acquire (in park_if or flag check) pairs with the release store
     * in cc__spawn_task_job, so task->result is visible here. */
    if (out_result) *out_result = task->result;
    return 0;
}

void cc_thread_task_free(struct CCSpawnTask* task) {
    if (!task) return;
    pthread_mutex_lock(&task->mu);
    if (task->done) {
        pthread_mutex_unlock(&task->mu);
        cc__spawn_task_free_internal(task);
        return;
    }
    task->detached = 1;
    pthread_mutex_unlock(&task->mu);
}

/* Non-blocking poll: check if thread task is done without blocking */
int cc_thread_task_poll_done(struct CCSpawnTask* task) {
    if (!task) return 0;
    pthread_mutex_lock(&task->mu);
    int done = task->done;
    pthread_mutex_unlock(&task->mu);
    return done;
}

/* Get result from a completed thread task (caller must ensure task is done) */
void* cc_thread_task_get_result(struct CCSpawnTask* task) {
    if (!task) return NULL;
    pthread_mutex_lock(&task->mu);
    void* result = task->result;
    pthread_mutex_unlock(&task->mu);
    return result;
}

int cc_sleep_ms(unsigned int ms) {
    /* Fiber-aware: park the fiber on the sleep queue with a deadline.
     * Sysmon drains expired sleepers every ~250µs and re-enqueues them.
     * This avoids O(N) queue churn when many fibers sleep concurrently. */
    if (cc__fiber_in_context()) {
        cc__fiber_sleep_park(ms);
        return 0;
    }
    /* Thread context: block the OS thread directly. */
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    while (nanosleep(&ts, &ts) == -1 && errno == EINTR) {
        continue;
    }
    return 0;
}

CCDeadline cc_deadline_none(void) {
    CCDeadline d; d.deadline.tv_sec = 0; d.deadline.tv_nsec = 0; d.cancelled = 0; return d; }

CCDeadline cc_deadline_after_ms(uint64_t ms) {
    CCDeadline d = cc_deadline_none();
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    uint64_t nsec = (uint64_t)now.tv_nsec + (ms % 1000) * 1000000ULL;
    d.deadline.tv_sec = now.tv_sec + (time_t)(ms / 1000) + (time_t)(nsec / 1000000000ULL);
    d.deadline.tv_nsec = (long)(nsec % 1000000000ULL);
    return d;
}

bool cc_deadline_expired(const CCDeadline* d) {
    if (!d || d->cancelled) return true;
    if (d->deadline.tv_sec == 0) return false;
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    if (now.tv_sec > d->deadline.tv_sec) return true;
    if (now.tv_sec == d->deadline.tv_sec && now.tv_nsec >= d->deadline.tv_nsec) return true;
    return false;
}

/* Undefine language-level macros to expose the CCDeadline*-taking API. */
#undef cc_cancel
#undef cc_is_cancelled
void cc_cancel(CCDeadline* d) { if (d) d->cancelled = 1; }
bool cc_is_cancelled(const CCDeadline* d) { return d && d->cancelled; }

const struct timespec* cc_deadline_as_timespec(const CCDeadline* d, struct timespec* out) {
    if (!d || d->deadline.tv_sec == 0) return NULL;
    if (out) *out = d->deadline;
    return out;
}

