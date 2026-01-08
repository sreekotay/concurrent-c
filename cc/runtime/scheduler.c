/*
 * Minimal pthread-backed scheduler facade with cooperative deadlines.
 */

#include "cc_sched.h"

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <time.h>

struct CCTask {
    pthread_t thread;
};

int cc_scheduler_init(void) { return 0; }
void cc_scheduler_shutdown(void) { }

typedef struct {
    void* (*fn)(void*);
    void* arg;
} CCThunk;

static void* cc_task_trampoline(void* p) {
    CCThunk* t = (CCThunk*)p;
    void* (*fn)(void*) = t->fn;
    void* arg = t->arg;
    free(t);
    return fn ? fn(arg) : NULL;
}

int cc_spawn(CCTask** out_task, void* (*fn)(void*), void* arg) {
    if (!out_task || !fn) return EINVAL;
    *out_task = NULL;
    CCTask* task = (CCTask*)malloc(sizeof(CCTask));
    if (!task) return ENOMEM;
    CCThunk* thunk = (CCThunk*)malloc(sizeof(CCThunk));
    if (!thunk) { free(task); return ENOMEM; }
    thunk->fn = fn;
    thunk->arg = arg;
    int err = pthread_create(&task->thread, NULL, cc_task_trampoline, thunk);
    if (err != 0) { free(thunk); free(task); return err; }
    *out_task = task;
    return 0;
}

int cc_task_join(CCTask* task) {
    if (!task) return EINVAL;
    return pthread_join(task->thread, NULL);
}

void cc_task_free(CCTask* task) {
    if (!task) return;
    free(task);
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

