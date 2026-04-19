#include <ccc/cc_exec.cch>

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    cc_exec_fn fn;
    void *arg;
} CCExecJob;

struct CCExec {
    pthread_t *threads;
    size_t nthreads;

    CCExecJob *queue;
    size_t qcap;
    size_t qhead;
    size_t qtail;
    size_t qlen;
    int unbounded; /* 1 = queue grows on demand; 0 = hard cap enforced */

    pthread_mutex_t mu;
    pthread_cond_t cv_not_empty;
    pthread_cond_t cv_not_full;

    int shutting_down;
};

/* Grow ring buffer to new_cap (must be > current qcap). Called with mu held.
 * Linearises the circular queue into [0, qlen) of the new buffer. */
static int cc__exec_grow(CCExec* ex, size_t new_cap) {
    CCExecJob *nb = (CCExecJob *)malloc(new_cap * sizeof(CCExecJob));
    if (!nb) return ENOMEM;
    for (size_t i = 0; i < ex->qlen; ++i) {
        nb[i] = ex->queue[(ex->qhead + i) % ex->qcap];
    }
    free(ex->queue);
    ex->queue = nb;
    ex->qcap = new_cap;
    ex->qhead = 0;
    ex->qtail = ex->qlen;
    return 0;
}

static void* cc_exec_worker(void *arg) {
    CCExec *ex = (CCExec *)arg;
    while (1) {
        pthread_mutex_lock(&ex->mu);
        while (ex->qlen == 0 && !ex->shutting_down) {
            pthread_cond_wait(&ex->cv_not_empty, &ex->mu);
        }
        if (ex->qlen == 0 && ex->shutting_down) {
            pthread_mutex_unlock(&ex->mu);
            break;
        }
        CCExecJob job = ex->queue[ex->qhead];
        ex->qhead = (ex->qhead + 1) % ex->qcap;
        ex->qlen--;
        pthread_cond_signal(&ex->cv_not_full);
        pthread_mutex_unlock(&ex->mu);
        if (job.fn) job.fn(job.arg);
    }
    return NULL;
}

CCExec* cc_exec_create(size_t workers, size_t queue_cap) {
    size_t n = workers ? workers : 4;
    /* queue_cap == 0 means "unbounded": start small and grow on demand. */
    int unbounded = (queue_cap == 0);
    size_t cap = unbounded ? 128 : queue_cap;
    CCExec *ex = (CCExec *)malloc(sizeof(CCExec));
    if (!ex) return NULL;
    memset(ex, 0, sizeof(*ex));
    ex->threads = (pthread_t *)malloc(n * sizeof(pthread_t));
    ex->queue = (CCExecJob *)malloc(cap * sizeof(CCExecJob));
    if (!ex->threads || !ex->queue) {
        free(ex->threads); free(ex->queue); free(ex);
        return NULL;
    }
    ex->nthreads = n;
    ex->qcap = cap;
    ex->unbounded = unbounded;
    ex->qhead = ex->qtail = ex->qlen = 0;
    pthread_mutex_init(&ex->mu, NULL);
    pthread_cond_init(&ex->cv_not_empty, NULL);
    pthread_cond_init(&ex->cv_not_full, NULL);
    ex->shutting_down = 0;
    for (size_t i = 0; i < n; ++i) {
        if (pthread_create(&ex->threads[i], NULL, cc_exec_worker, ex) != 0) {
            ex->shutting_down = 1;
            // Best-effort join already started threads
            for (size_t j = 0; j < i; ++j) pthread_join(ex->threads[j], NULL);
            pthread_mutex_destroy(&ex->mu);
            pthread_cond_destroy(&ex->cv_not_empty);
            pthread_cond_destroy(&ex->cv_not_full);
            free(ex->threads); free(ex->queue); free(ex);
            return NULL;
        }
    }
    return ex;
}

int cc_exec_submit(CCExec* ex, cc_exec_fn fn, void *arg) {
    if (!ex || !fn) return EINVAL;
    pthread_mutex_lock(&ex->mu);
    if (ex->shutting_down) { pthread_mutex_unlock(&ex->mu); return EINVAL; }
    if (ex->qlen == ex->qcap) {
        if (ex->unbounded) {
            /* Double the ring buffer. On OOM fall back to EAGAIN so callers
             * that used to see EAGAIN still behave sanely. */
            size_t new_cap = ex->qcap * 2;
            if (new_cap < ex->qcap) { /* overflow guard */
                pthread_mutex_unlock(&ex->mu); return EAGAIN;
            }
            int gerr = cc__exec_grow(ex, new_cap);
            if (gerr != 0) { pthread_mutex_unlock(&ex->mu); return EAGAIN; }
        } else {
            pthread_mutex_unlock(&ex->mu); return EAGAIN;
        }
    }
    ex->queue[ex->qtail].fn = fn;
    ex->queue[ex->qtail].arg = arg;
    ex->qtail = (ex->qtail + 1) % ex->qcap;
    ex->qlen++;
    pthread_cond_signal(&ex->cv_not_empty);
    pthread_mutex_unlock(&ex->mu);
    return 0;
}

/* Blocking variant: if the queue is bounded and full, wait on cv_not_full
 * until a worker drains a slot. Intended for callers (e.g. the blocking I/O
 * pool) where backpressure is the whole point of bounding the queue.
 * Never returns EAGAIN; only EINVAL (shutdown / bad args). */
int cc_exec_submit_blocking(CCExec* ex, cc_exec_fn fn, void *arg) {
    if (!ex || !fn) return EINVAL;
    pthread_mutex_lock(&ex->mu);
    for (;;) {
        if (ex->shutting_down) { pthread_mutex_unlock(&ex->mu); return EINVAL; }
        if (ex->qlen < ex->qcap) break;
        if (ex->unbounded) {
            size_t new_cap = ex->qcap * 2;
            if (new_cap < ex->qcap) {
                pthread_cond_wait(&ex->cv_not_full, &ex->mu);
                continue;
            }
            int gerr = cc__exec_grow(ex, new_cap);
            if (gerr != 0) {
                pthread_cond_wait(&ex->cv_not_full, &ex->mu);
                continue;
            }
            break;
        }
        pthread_cond_wait(&ex->cv_not_full, &ex->mu);
    }
    ex->queue[ex->qtail].fn = fn;
    ex->queue[ex->qtail].arg = arg;
    ex->qtail = (ex->qtail + 1) % ex->qcap;
    ex->qlen++;
    pthread_cond_signal(&ex->cv_not_empty);
    pthread_mutex_unlock(&ex->mu);
    return 0;
}

void cc_exec_shutdown(CCExec* ex) {
    if (!ex) return;
    pthread_mutex_lock(&ex->mu);
    ex->shutting_down = 1;
    pthread_cond_broadcast(&ex->cv_not_empty);
    pthread_cond_broadcast(&ex->cv_not_full);
    pthread_mutex_unlock(&ex->mu);
    for (size_t i = 0; i < ex->nthreads; ++i) {
        pthread_join(ex->threads[i], NULL);
    }
}

int cc_exec_stats(CCExec* ex, CCExecStats* out) {
    if (!ex || !out) return EINVAL;
    pthread_mutex_lock(&ex->mu);
    out->workers = ex->nthreads;
    out->queue_cap = ex->qcap;
    out->queue_len = ex->qlen;
    out->shutting_down = ex->shutting_down;
    pthread_mutex_unlock(&ex->mu);
    return 0;
}

void cc_exec_free(CCExec* ex) {
    if (!ex) return;
    pthread_mutex_destroy(&ex->mu);
    pthread_cond_destroy(&ex->cv_not_empty);
    pthread_cond_destroy(&ex->cv_not_full);
    free(ex->threads);
    free(ex->queue);
    free(ex);
}

