/*
 * Unified CCTask runtime (async, future, and spawn tasks).
 */

#include <ccc/std/task.cch>
#include <ccc/cc_sched.cch>

#include <ccc/cc_exec.cch>
#include <ccc/cc_channel.cch>
#include <ccc/cc_nursery.cch>
#include <ccc/cc_atomic.cch>

/* Unified deadlock tracking (defined in fiber_sched.c) */
void cc__deadlock_thread_block(void);
void cc__deadlock_thread_unblock(void);

#include <errno.h>

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
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

/* Internal representation for FIBER kind tasks */
typedef struct fiber_task fiber_task;
typedef struct {
    fiber_task* fiber;
} CCTaskFiberInternal;

/* Accessor macros to get internal data from CCTask */
#define TASK_FUTURE(t) ((CCTaskFutureInternal*)((t)->_data))
#define TASK_POLL(t) ((CCTaskPollInternal*)((t)->_data))
#define TASK_SPAWN(t) ((CCTaskSpawnInternal*)((t)->_data))
#define TASK_FIBER(t) ((CCTaskFiberInternal*)((t)->_data))
#endif /* CC_TASK_INTERNAL_TYPES_DEFINED */

/* Fiber functions (defined in fiber_sched.c) */
fiber_task* cc_fiber_spawn(void* (*fn)(void*), void* arg);
int cc_fiber_join(fiber_task* f, void** out_result);
void cc_fiber_task_free(fiber_task* f);
int cc_fiber_poll_done(fiber_task* f);
void* cc_fiber_get_result(fiber_task* f);

/* Spawn task poll functions (defined in scheduler.c) */
int cc_thread_task_poll_done(struct CCSpawnTask* task);
void* cc_thread_task_get_result(struct CCSpawnTask* task);

static CCExec* g_task_exec = NULL;
static pthread_mutex_t g_task_exec_mu = PTHREAD_MUTEX_INITIALIZER;
static cc_atomic_u64 g_task_submit_failures = 0;

/* cc__env_size defined in scheduler.c */

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
    if (t->kind == CC_TASK_KIND_FIBER) {
        CCTaskFiberInternal* fi = TASK_FIBER(t);
        if (!fi->fiber) return CC_FUTURE_ERR;
        if (cc_fiber_poll_done(fi->fiber)) {
            if (out_val) *out_val = (intptr_t)cc_fiber_get_result(fi->fiber);
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
    } else if (t->kind == CC_TASK_KIND_FIBER) {
        CCTaskFiberInternal* fi = TASK_FIBER(t);
        if (fi->fiber) {
            cc_fiber_task_free(fi->fiber);
        }
    }
    memset(t, 0, sizeof(*t));
}

/* Helper to set fiber in task internal data */
static void cc__set_fiber_task(CCTask* t, fiber_task* f) {
    CCTaskFiberInternal* fi = TASK_FIBER(t);
    fi->fiber = f;
}

/* Spawn an M:N fiber task. Returns CCTask with kind=CC_TASK_KIND_FIBER. */
CCTask cc_fiber_spawn_task(void* (*fn)(void*), void* arg) {
    CCTask out;
    memset(&out, 0, sizeof(out));
    if (!fn) return out;
    
    fiber_task* f = cc_fiber_spawn(fn, arg);
    if (!f) return out;
    
    out.kind = CC_TASK_KIND_FIBER;
    cc__set_fiber_task(&out, f);
    return out;
}

/* Helper function that unpacks and calls a closure for fibers */
static void* cc__fiber_closure0_wrapper(void* arg) {
    CCClosure0* pc = (CCClosure0*)arg;
    void* result = pc->fn(pc->env);
    if (pc->drop) pc->drop(pc->env);
    free(pc);
    return result;
}

/* Spawn a fiber from a 0-arg closure. */
CCTask cc_fiber_spawn_closure0(CCClosure0 c) {
    CCTask out;
    memset(&out, 0, sizeof(out));
    if (!c.fn) return out;
    
    /* Create a heap copy of the closure */
    CCClosure0* heap_c = (CCClosure0*)malloc(sizeof(CCClosure0));
    if (!heap_c) return out;
    *heap_c = c;
    
    return cc_fiber_spawn_task(cc__fiber_closure0_wrapper, heap_c);
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
    cc__deadlock_thread_block();  /* Track that this thread is blocking */
    
    /* Handle spawn tasks directly with join */
    if (t.kind == CC_TASK_KIND_SPAWN) {
        CCTaskSpawnInternal* s = TASK_SPAWN(&t);
        if (s->spawn) {
            void* result = NULL;
            cc_thread_task_join_result(s->spawn, &result);
            r = (intptr_t)result;
            cc_thread_task_free(s->spawn);
        }
        cc__deadlock_thread_unblock();
        return r;
    }
    
    /* Handle fiber tasks with fiber join */
    if (t.kind == CC_TASK_KIND_FIBER) {
        CCTaskFiberInternal* fi = TASK_FIBER(&t);
        if (fi->fiber) {
            void* result = NULL;
            cc_fiber_join(fi->fiber, &result);
            r = (intptr_t)result;
            cc_fiber_task_free(fi->fiber);
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

/* Block until all tasks complete. Runs tasks concurrently using a nursery.
   Returns 0 on success, error code if any task fails.
   Results are stored in the results array (must be at least count elements).
   Note: Takes ownership of the tasks (they are freed after completion). */
int cc_block_all(int count, CCTask* tasks, intptr_t* results) {
    if (count <= 0) return 0;
    if (!tasks) return EINVAL;

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

    /* Spawn a thread for each task */
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

    /* Wait for first SUCCESS (non-zero result indicates error for now) */
    int found_success = 0;
    int completed = 0;
    CCBlockRaceResult first_result = {0};
    
    while (completed < count && !found_success) {
        CCBlockRaceResult msg;
        int recv_err = cc_chan_recv(done_chan, &msg, sizeof(msg));
        if (recv_err != 0) break;
        
        completed++;
        
        /* For now, treat any completion as success (no error propagation yet) */
        /* TODO: Add proper error handling when tasks can return errors */
        found_success = 1;
        first_result = msg;
    }

    if (found_success) {
        if (winner) *winner = first_result.index;
        if (result) *result = first_result.result;
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
