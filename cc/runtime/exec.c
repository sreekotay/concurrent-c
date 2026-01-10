#include "cc_exec.cch"

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

    pthread_mutex_t mu;
    pthread_cond_t cv_not_empty;
    pthread_cond_t cv_not_full;

    int shutting_down;
};

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
    size_t cap = queue_cap ? queue_cap : 128;
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
    if (ex->qlen == ex->qcap) { pthread_mutex_unlock(&ex->mu); return EAGAIN; }
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

void cc_exec_free(CCExec* ex) {
    if (!ex) return;
    pthread_mutex_destroy(&ex->mu);
    pthread_cond_destroy(&ex->cv_not_empty);
    pthread_cond_destroy(&ex->cv_not_full);
    free(ex->threads);
    free(ex->queue);
    free(ex);
}

