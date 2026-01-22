/*
 * Executor-backed Task<intptr_t> (bridge for async bring-up).
 */

#include <ccc/std/task_intptr.cch>

#include <ccc/cc_exec.cch>
#include <ccc/cc_channel.cch>
#include <ccc/cc_nursery.cch>
#include <ccc/cc_deadlock_detect.cch>
#include <ccc/cc_atomic.cch>

#include <errno.h>

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct {
    CCChan* done;
    volatile int cancelled;
    intptr_t result;
    CCClosure0 c;
} CCTaskIntptrHeap;

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
    t.poll.wait = NULL;
    t.poll.frame = frame;
    t.poll.drop = drop;
    return t;
}

CCTaskIntptr cc_task_intptr_make_poll_ex(cc_task_intptr_poll_fn poll, int (*wait)(void*), void* frame, void (*drop)(void*)) {
    CCTaskIntptr t;
    memset(&t, 0, sizeof(t));
    if (!poll || !frame) return t;
    t.kind = CC_TASK_INTPTR_KIND_POLL;
    t.poll.poll = poll;
    t.poll.wait = wait;
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

/* Cancel a task and wake up anyone blocked on it.
   This closes the done channel, causing cc_block_on_intptr to return immediately. */
void cc_task_intptr_cancel(CCTaskIntptr* t) {
    if (!t) return;
    if (t->kind == CC_TASK_INTPTR_KIND_FUTURE) {
        CCTaskIntptrHeap* h = (CCTaskIntptrHeap*)t->future.heap;
        if (h) {
            h->cancelled = 1;
            /* Close the done channel to wake up blocked waiters */
            if (h->done) {
                cc_chan_close(h->done);
            }
        }
        /* Also mark the future handle as cancelled */
        t->future.fut.handle.cancelled = 1;
    } else if (t->kind == CC_TASK_INTPTR_KIND_POLL) {
        /* For poll-based tasks, we can't easily cancel - just mark frame for cleanup */
        /* The poll function should check cancellation state */
    }
}

intptr_t cc_block_on_intptr(CCTaskIntptr t) {
    intptr_t r = 0;
    int err = 0;
    cc_deadlock_enter_blocking(CC_BLOCK_ON_TASK);
    for (;;) {
        CCFutureStatus st = cc_task_intptr_poll(&t, &r, &err);
        if (st == CC_FUTURE_PENDING) {
            if (t.kind == CC_TASK_INTPTR_KIND_FUTURE) {
                /* For "future" tasks, block directly on the done channel once and then return the result.
                   This avoids spin-polling and avoids needing to preserve the completion for poll(). */
                err = cc_async_wait(&t.future.fut.handle);
                if (err == 0 && t.future.fut.result) r = *(const intptr_t*)t.future.fut.result;
                break;
            } else if (t.kind == CC_TASK_INTPTR_KIND_POLL && t.poll.wait) {
                /* Task has a wait function - use it to block efficiently */
                (void)t.poll.wait(t.poll.frame);
            }
            /* For POLL tasks without wait: tight-loop poll (no sleep).
               We're the sole driver, so spinning is correct.
               This makes @async functions without await points fast. */
            continue;
        }
        break;
    }
    cc_deadlock_exit_blocking();
    cc_deadlock_progress();  /* Task completed */
    cc_task_intptr_free(&t);
    (void)err;
    return r;
}


/* --- cc_block_all implementation --- */

typedef struct {
    CCTaskIntptr task;      /* Copy of the task (we own it) */
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
int cc_block_all(int count, CCTaskIntptr* tasks, intptr_t* results) {
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
    CCTaskIntptr task;
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
int cc_block_race(int count, CCTaskIntptr* tasks, int* winner, intptr_t* result) {
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
            cc_task_intptr_cancel(&slots[i].task);
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
int cc_block_any(int count, CCTaskIntptr* tasks, int* winner, intptr_t* result) {
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
            cc_task_intptr_cancel(&slots[i].task);
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
