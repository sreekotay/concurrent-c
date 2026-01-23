/*
 * Simple structured concurrency nursery built on the fiber scheduler.
 */

#include <ccc/cc_nursery.cch>
#include <ccc/cc_channel.cch>
#include <ccc/cc_deadlock_detect.cch>

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

/* Forward declaration for fiber_task */
typedef struct fiber_task fiber_task;

/* Fiber scheduler functions */
fiber_task* cc_fiber_spawn(void* (*fn)(void*), void* arg);
int cc_fiber_join(fiber_task* t, void** out_result);
void cc_fiber_task_free(fiber_task* t);

/* Thread-local: current nursery for code running inside nursery-spawned tasks.
   Used by optional runtime deadlock guard in channel.c. */
__thread CCNursery* cc__tls_current_nursery = NULL;

struct CCNursery {
    CCTask** tasks;
    size_t count;
    size_t cap;
    int cancelled;
    struct timespec deadline;
    CCChan** closing;
    size_t closing_count;
    size_t closing_cap;
    pthread_mutex_t mu;
};

/* Defined in channel.c (same translation unit via runtime/concurrent_c.c). */
void cc__chan_set_autoclose_owner(CCChan* ch, CCNursery* owner);

int cc_nursery_add_closing_tx(CCNursery* n, CCChanTx tx) {
    return cc_nursery_add_closing_chan(n, tx.raw);
}

typedef struct {
    CCNursery* n;
    void* (*fn)(void*);
    void* arg;
} CCNurseryThunk;

static void* cc__nursery_task_trampoline(void* p) {
    CCNurseryThunk* th = (CCNurseryThunk*)p;
    if (!th) return NULL;
    CCNursery* nn = th->n;
    void* (*ff)(void*) = th->fn;
    void* aa = th->arg;
    free(th);
    cc__tls_current_nursery = nn;
    void* r = ff ? ff(aa) : NULL;
    cc__tls_current_nursery = NULL;
    return r;
}

CCNursery* cc_nursery_create(void) {
    /* Initialize deadlock detection on first nursery (lazy init, checks env var) */
    cc_deadlock_detect_init();
    
    CCNursery* n = (CCNursery*)malloc(sizeof(CCNursery));
    if (!n) return NULL;
    memset(n, 0, sizeof(*n));
    n->cap = 8;
    n->tasks = (CCTask**)calloc(n->cap, sizeof(CCTask*));
    if (!n->tasks) {
        free(n);
        return NULL;
    }
    pthread_mutex_init(&n->mu, NULL);
    n->deadline.tv_sec = 0;
    n->deadline.tv_nsec = 0;
    n->closing = NULL;
    n->closing_cap = 0;
    n->closing_count = 0;
    return n;
}

void cc_nursery_cancel(CCNursery* n) {
    if (!n) return;
    pthread_mutex_lock(&n->mu);
    n->cancelled = 1;
    pthread_mutex_unlock(&n->mu);
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

static int cc_nursery_grow(CCNursery* n) {
    size_t new_cap = n->cap ? n->cap * 2 : 8;
    CCTask** nt = (CCTask**)realloc(n->tasks, new_cap * sizeof(CCTask*));
    if (!nt) return ENOMEM;
    memset(nt + n->cap, 0, (new_cap - n->cap) * sizeof(fiber_task*));
    n->tasks = nt;
    n->cap = new_cap;
    return 0;
}

int cc_nursery_spawn(CCNursery* n, void* (*fn)(void*), void* arg) {
    if (!n || !fn) return EINVAL;

    CCNurseryThunk* th = (CCNurseryThunk*)malloc(sizeof(CCNurseryThunk));
    if (!th) return ENOMEM;
    th->n = n;
    th->fn = fn;
    th->arg = arg;

    CCTask* t = NULL;
    int err = cc_spawn(&t, cc__nursery_task_trampoline, th);
    if (err != 0) {
        free(th);
        return err;
    }

    pthread_mutex_lock(&n->mu);
    // Grow if needed (rare case in benchmarks)
    if (n->count == n->cap) {
        int grow_err = cc_nursery_grow(n);
        if (grow_err != 0) {
            pthread_mutex_unlock(&n->mu);
            cc_task_free(t);
            free(th);
            return grow_err;
        }
    }
    n->tasks[n->count++] = t;
    pthread_mutex_unlock(&n->mu);
    return 0;
}

int cc_nursery_wait(CCNursery* n) {
    if (!n) return EINVAL;
    int first_err = 0;
    // IMPORTANT (spec): join children first, then close registered channels.
    // The closing(...) clause exists to avoid close-before-send races.
    for (size_t i = 0; i < n->count; ++i) {
        if (!n->tasks[i]) continue;
        int err = cc_task_join(n->tasks[i]);
        if (first_err == 0 && err != 0) {
            first_err = err;
        }
        cc_task_free(n->tasks[i]);
        n->tasks[i] = NULL;
    }
    for (size_t i = 0; i < n->closing_count; ++i) {
        if (n->closing[i]) cc_chan_close(n->closing[i]);
    }
    n->count = 0;
    return first_err;
}

void cc_nursery_free(CCNursery* n) {
    if (!n) return;
    for (size_t i = 0; i < n->count; ++i) {
        if (n->tasks[i]) {
            cc_task_free(n->tasks[i]);
        }
    }
    // Close registered channels as a last step (best-effort safety if user never waited).
    for (size_t i = 0; i < n->closing_count; ++i) {
        if (n->closing[i]) cc_chan_close(n->closing[i]);
    }
    free(n->tasks);
    free(n->closing);
    pthread_mutex_destroy(&n->mu);
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

