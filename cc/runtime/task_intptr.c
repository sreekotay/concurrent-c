/*
 * Executor-backed Task<intptr_t> (bridge for async bring-up).
 */

#include "std/task_intptr.cch"

#include "cc_exec.cch"
#include "cc_channel.cch"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    CCChan* done;
    volatile int cancelled;
    intptr_t result;
    CCClosure0 c;
} CCTaskIntptrHeap;

static CCExec* g_task_exec = NULL;
static pthread_mutex_t g_task_exec_mu = PTHREAD_MUTEX_INITIALIZER;

static CCExec* cc__task_exec_lazy(void) {
    pthread_mutex_lock(&g_task_exec_mu);
    if (!g_task_exec) {
        g_task_exec = cc_exec_create(4, 256);
    }
    CCExec* ex = g_task_exec;
    pthread_mutex_unlock(&g_task_exec_mu);
    return ex;
}

static void cc__task_intptr_job(void* arg) {
    CCTaskIntptrHeap* h = (CCTaskIntptrHeap*)arg;
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

CCTaskIntptr cc_run_blocking_task_intptr(CCClosure0 c) {
    CCTaskIntptr out;
    memset(&out, 0, sizeof(out));
    if (!c.fn) return out;

    CCExec* ex = cc__task_exec_lazy();
    if (!ex) return out;

    CCTaskIntptrHeap* h = (CCTaskIntptrHeap*)calloc(1, sizeof(CCTaskIntptrHeap));
    if (!h) return out;
    h->done = cc_chan_create(1);
    if (!h->done) {
        free(h);
        return out;
    }
    h->cancelled = 0;
    h->result = 0;
    h->c = c;

    out.kind = CC_TASK_INTPTR_KIND_FUTURE;
    cc_future_init(&out.future.fut);
    out.future.fut.handle.done = h->done;
    out.future.fut.handle.cancelled = 0;
    out.future.fut.result = &h->result;
    out.future.heap = h;

    int sub = cc_exec_submit(ex, cc__task_intptr_job, h);
    if (sub != 0) {
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

CCFutureStatus cc_task_intptr_poll(CCTaskIntptr* t, intptr_t* out_val, int* out_err) {
    if (!t) return CC_FUTURE_ERR;
    if (t->kind == CC_TASK_INTPTR_KIND_FUTURE) {
        CCFutureStatus st = cc_future_poll(&t->future.fut, out_err);
        if (st == CC_FUTURE_READY && out_val && t->future.fut.result) {
            *out_val = *(const intptr_t*)t->future.fut.result;
        }
        return st;
    }
    if (t->kind == CC_TASK_INTPTR_KIND_POLL) {
        if (!t->poll.poll) return CC_FUTURE_ERR;
        return t->poll.poll(t->poll.frame, out_val, out_err);
    }
    return CC_FUTURE_ERR;
}

CCTaskIntptr cc_task_intptr_make_poll(cc_task_intptr_poll_fn poll, void* frame, void (*drop)(void*)) {
    CCTaskIntptr t;
    memset(&t, 0, sizeof(t));
    if (!poll || !frame) return t;
    t.kind = CC_TASK_INTPTR_KIND_POLL;
    t.poll.poll = poll;
    t.poll.frame = frame;
    t.poll.drop = drop;
    return t;
}

void cc_task_intptr_free(CCTaskIntptr* t) {
    if (!t) return;
    if (t->kind == CC_TASK_INTPTR_KIND_FUTURE) {
        CCTaskIntptrHeap* h = (CCTaskIntptrHeap*)t->future.heap;
        if (t->future.fut.handle.done) {
            cc_future_free(&t->future.fut);
        }
        if (h) {
            h->cancelled = 1;
            free(h);
        }
    } else if (t->kind == CC_TASK_INTPTR_KIND_POLL) {
        if (t->poll.drop && t->poll.frame) t->poll.drop(t->poll.frame);
        t->poll.poll = NULL;
        t->poll.frame = NULL;
        t->poll.drop = NULL;
    }
    memset(t, 0, sizeof(*t));
}

intptr_t cc_block_on_intptr(CCTaskIntptr t) {
    intptr_t r = 0;
    int err = 0;
    for (;;) {
        CCFutureStatus st = cc_task_intptr_poll(&t, &r, &err);
        if (st == CC_FUTURE_PENDING) {
            (void)cc_sleep_ms(1);
            continue;
        }
        break;
    }
    cc_task_intptr_free(&t);
    (void)err;
    return r;
}

