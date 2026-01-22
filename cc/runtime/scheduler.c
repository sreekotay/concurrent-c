/*
 * Executor-backed scheduler facade with cooperative deadlines.
 */

#include <ccc/cc_sched.cch>
#include <ccc/cc_exec.cch>

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

struct CCTask {
    void* (*fn)(void*);
    void* arg;
    void* result;
    int done;
    int detached;
    pthread_mutex_t mu;
    pthread_cond_t cv;
};

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
        size_t workers = cc__env_size("CC_WORKERS", cc__default_workers());
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

static void cc__task_free_internal(CCTask* task) {
    if (!task) return;
    pthread_mutex_destroy(&task->mu);
    pthread_cond_destroy(&task->cv);
    free(task);
}

static void cc__task_job(void* arg) {
    CCTask* task = (CCTask*)arg;
    if (!task) return;
    void* r = NULL;
    if (task->fn) r = task->fn(task->arg);
    pthread_mutex_lock(&task->mu);
    task->result = r;
    task->done = 1;
    pthread_cond_broadcast(&task->cv);
    int detach = task->detached;
    pthread_mutex_unlock(&task->mu);
    if (detach) {
        cc__task_free_internal(task);
    }
}

int cc_spawn(CCTask** out_task, void* (*fn)(void*), void* arg) {
    if (!out_task || !fn) return EINVAL;
    *out_task = NULL;
    CCExec* ex = cc__sched_exec_lazy();
    if (!ex) return ENOMEM;
    CCTask* task = (CCTask*)calloc(1, sizeof(CCTask));
    if (!task) return ENOMEM;
    task->fn = fn;
    task->arg = arg;
    pthread_mutex_init(&task->mu, NULL);
    pthread_cond_init(&task->cv, NULL);
    int err = cc_exec_submit(ex, cc__task_job, task);
    if (err != 0) {
        cc__task_free_internal(task);
        return err;
    }
    *out_task = task;
    return 0;
}

int cc_task_join(CCTask* task) {
    if (!task) return EINVAL;
    pthread_mutex_lock(&task->mu);
    while (!task->done) {
        pthread_cond_wait(&task->cv, &task->mu);
    }
    pthread_mutex_unlock(&task->mu);
    return 0;
}

int cc_task_join_result(CCTask* task, void** out_result) {
    if (!task) return EINVAL;
    pthread_mutex_lock(&task->mu);
    while (!task->done) {
        pthread_cond_wait(&task->cv, &task->mu);
    }
    if (out_result) *out_result = task->result;
    pthread_mutex_unlock(&task->mu);
    return 0;
}

void cc_task_free(CCTask* task) {
    if (!task) return;
    pthread_mutex_lock(&task->mu);
    if (task->done) {
        pthread_mutex_unlock(&task->mu);
        cc__task_free_internal(task);
        return;
    }
    task->detached = 1;
    pthread_mutex_unlock(&task->mu);
}

int cc_sleep_ms(unsigned int ms) {
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

void cc_cancel(CCDeadline* d) { if (d) d->cancelled = 1; }
bool cc_is_cancelled(const CCDeadline* d) { return d && d->cancelled; }

const struct timespec* cc_deadline_as_timespec(const CCDeadline* d, struct timespec* out) {
    if (!d || d->deadline.tv_sec == 0) return NULL;
    if (out) *out = d->deadline;
    return out;
}

