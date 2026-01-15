/*
 * Blocking channel with mutex/cond and fixed capacity.
 * Supports by-value copies and pointer payloads via size argument.
 * Provides blocking, try, timed, and CCDeadline-aware variants.
 * send_take helpers treat payloads as pointers (zero-copy for pointer payloads) when allowed.
 * Backpressure modes: block (default), drop-new, drop-old.
 * Async send/recv via executor offload.
 * Match helpers for polling/selecting across channels.
 */

#include "cc_channel.cch"
#include "cc_sched.cch"
#include "cc_nursery.cch"
#include "cc_exec.cch"
#include "cc_slice.cch"
#include "std/async_io.cch"
#include "std/future.cch"

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdio.h>

/* Defined in nursery.c (same translation unit via runtime/concurrent_c.c). */
extern __thread CCNursery* cc__tls_current_nursery;
/* Thread-local current deadline scope (set by with_deadline lowering). */
__thread CCDeadline* cc__tls_current_deadline = NULL;

CCDeadline* cc_current_deadline(void) {
    return cc__tls_current_deadline;
}

CCDeadline* cc_deadline_push(CCDeadline* d) {
    CCDeadline* prev = cc__tls_current_deadline;
    cc__tls_current_deadline = d;
    return prev;
}

void cc_deadline_pop(CCDeadline* prev) {
    cc__tls_current_deadline = prev;
}

void cc_cancel_current(void) {
    if (cc__tls_current_deadline) cc__tls_current_deadline->cancelled = 1;
}

bool cc_is_cancelled_current(void) {
    return cc__tls_current_deadline && cc__tls_current_deadline->cancelled;
}

int cc_chan_pair_create(size_t capacity,
                        CCChanMode mode,
                        bool allow_send_take,
                        size_t elem_size,
                        CCChanTx* out_tx,
                        CCChanRx* out_rx) {
    if (!out_tx || !out_rx) return EINVAL;
    out_tx->raw = NULL;
    out_rx->raw = NULL;
    CCChan* ch = cc_chan_create_mode_take(capacity, mode, allow_send_take);
    if (!ch) return ENOMEM;
    if (elem_size != 0) {
        int e = cc_chan_init_elem(ch, elem_size);
        if (e != 0) { cc_chan_free(ch); return e; }
    }
    out_tx->raw = ch;
    out_rx->raw = ch;
    return 0;
}

struct CCChan {
    size_t cap;
    size_t count;
    size_t head;
    size_t tail;
    void *buf;       // contiguous ring buffer
    size_t elem_size;
    int closed;
    CCChanMode mode;
    int allow_take;
    /* Debug/guard: if set, this channel is auto-closed by this nursery on scope exit. */
    CCNursery* autoclose_owner;
    int warned_autoclose_block;
    pthread_mutex_t mu;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
};

/* Called by nursery.c when registering `closing(ch)` (same TU). */
void cc__chan_set_autoclose_owner(CCChan* ch, CCNursery* owner) {
    if (!ch) return;
    pthread_mutex_lock(&ch->mu);
    if (!ch->autoclose_owner) ch->autoclose_owner = owner;
    ch->warned_autoclose_block = 0;
    pthread_mutex_unlock(&ch->mu);
}

static CCChan* cc_chan_create_internal(size_t capacity, CCChanMode mode, bool allow_take) {
    size_t cap = capacity ? capacity : 64;
    CCChan* ch = (CCChan*)malloc(sizeof(CCChan));
    if (!ch) return NULL;
    memset(ch, 0, sizeof(*ch));
    ch->cap = cap;
    ch->elem_size = 0; // set on first send/recv
    ch->buf = NULL;    // lazily allocated when we know elem_size
    ch->mode = mode;
    ch->allow_take = allow_take ? 1 : 0;
    pthread_mutex_init(&ch->mu, NULL);
    pthread_cond_init(&ch->not_empty, NULL);
    pthread_cond_init(&ch->not_full, NULL);
    return ch;
}

CCChan* cc_chan_create(size_t capacity) {
    return cc_chan_create_internal(capacity, CC_CHAN_MODE_BLOCK, true);
}

CCChan* cc_chan_create_mode(size_t capacity, CCChanMode mode) {
    return cc_chan_create_internal(capacity, mode, true);
}

CCChan* cc_chan_create_mode_take(size_t capacity, CCChanMode mode, bool allow_send_take) {
    return cc_chan_create_internal(capacity, mode, allow_send_take);
}

void cc_chan_close(CCChan* ch) {
    if (!ch) return;
    pthread_mutex_lock(&ch->mu);
    ch->closed = 1;
    pthread_cond_broadcast(&ch->not_empty);
    pthread_cond_broadcast(&ch->not_full);
    pthread_mutex_unlock(&ch->mu);
}

void cc_chan_free(CCChan* ch) {
    if (!ch) return;
    pthread_mutex_destroy(&ch->mu);
    pthread_cond_destroy(&ch->not_empty);
    pthread_cond_destroy(&ch->not_full);
    free(ch->buf);
    free(ch);
}

// Ensure buffer is allocated with the given element size; only allowed to set once.
static int cc_chan_ensure_buf(CCChan* ch, size_t elem_size) {
    if (ch->elem_size == 0) {
        ch->elem_size = elem_size;
        ch->buf = malloc(ch->cap * elem_size);
        if (!ch->buf) return ENOMEM;
        return 0;
    }
    if (ch->elem_size != elem_size) return EINVAL;
    return 0;
}

// Initialize element size eagerly (typed channels). Allocates buffer once.
int cc_chan_init_elem(CCChan* ch, size_t elem_size) {
    if (!ch || elem_size == 0) return EINVAL;
    return cc_chan_ensure_buf(ch, elem_size);
}

static int cc_chan_wait_full(CCChan* ch, const struct timespec* deadline) {
    int err = 0;
    while (!ch->closed && ch->count == ch->cap && err == 0) {
        if (deadline) {
            err = pthread_cond_timedwait(&ch->not_full, &ch->mu, deadline);
            if (err == ETIMEDOUT) return ETIMEDOUT;
        } else {
            pthread_cond_wait(&ch->not_full, &ch->mu);
        }
    }
    return ch->closed ? EPIPE : 0;
}

static int cc_chan_wait_empty(CCChan* ch, const struct timespec* deadline) {
    int err = 0;
    /* Runtime guard (opt-in): blocking recv on an autoclose channel from inside the same nursery
       is a common deadlock foot-gun (recv-until-close inside the nursery). */
    if (!deadline && !ch->closed && ch->count == 0 &&
        ch->autoclose_owner && cc__tls_current_nursery &&
        ch->autoclose_owner == cc__tls_current_nursery) {
        const char* g = getenv("CC_NURSERY_CLOSING_RUNTIME_GUARD");
        if (g && g[0] == '1') {
            if (!ch->warned_autoclose_block) {
                ch->warned_autoclose_block = 1;
                fprintf(stderr,
                        "CC: runtime guard: blocking cc_chan_recv() on a `closing(...)` channel from inside the same nursery "
                        "may deadlock (use a sentinel/explicit close, or drain outside the nursery)\n");
            }
            return EDEADLK;
        }
    }
    while (!ch->closed && ch->count == 0 && err == 0) {
        if (deadline) {
            err = pthread_cond_timedwait(&ch->not_empty, &ch->mu, deadline);
            if (err == ETIMEDOUT) return ETIMEDOUT;
        } else {
            pthread_cond_wait(&ch->not_empty, &ch->mu);
        }
    }
    if (ch->closed && ch->count == 0) return EPIPE;
    return 0;
}

static void cc_chan_enqueue(CCChan* ch, const void* value) {
    void *slot = (uint8_t*)ch->buf + ch->tail * ch->elem_size;
    memcpy(slot, value, ch->elem_size);
    ch->tail = (ch->tail + 1) % ch->cap;
    ch->count++;
    pthread_cond_signal(&ch->not_empty);
}

static void cc_chan_dequeue(CCChan* ch, void* out_value) {
    void *slot = (uint8_t*)ch->buf + ch->head * ch->elem_size;
    memcpy(out_value, slot, ch->elem_size);
    ch->head = (ch->head + 1) % ch->cap;
    ch->count--;
    pthread_cond_signal(&ch->not_full);
}

static int cc_chan_handle_full_send(CCChan* ch, const void* value, const struct timespec* deadline) {
    (void)value;
    if (ch->mode == CC_CHAN_MODE_BLOCK) {
        return cc_chan_wait_full(ch, deadline);
    } else if (ch->mode == CC_CHAN_MODE_DROP_NEW) {
        return EAGAIN;
    } else { // DROP_OLD
        ch->head = (ch->head + 1) % ch->cap;
        ch->count--;
        return 0;
    }
}

int cc_chan_send(CCChan* ch, const void* value, size_t value_size) {
    if (!ch || !value || value_size == 0) return EINVAL;
    /* Deadline scope: if caller installed a current deadline, use deadline-aware send. */
    if (cc__tls_current_deadline) {
        return cc_chan_deadline_send(ch, value, value_size, cc__tls_current_deadline);
    }
    pthread_mutex_lock(&ch->mu);
    int err = cc_chan_ensure_buf(ch, value_size);
    if (err != 0) { pthread_mutex_unlock(&ch->mu); return err; }
    if (ch->closed) { pthread_mutex_unlock(&ch->mu); return EPIPE; }
    if (ch->count == ch->cap) {
        err = cc_chan_handle_full_send(ch, value, NULL);
        if (err != 0) { pthread_mutex_unlock(&ch->mu); return err; }
    }
    cc_chan_enqueue(ch, value);
    pthread_mutex_unlock(&ch->mu);
    return 0;
}

int cc_chan_recv(CCChan* ch, void* out_value, size_t value_size) {
    if (!ch || !out_value || value_size == 0) return EINVAL;
    /* Deadline scope: if caller installed a current deadline, use deadline-aware recv. */
    if (cc__tls_current_deadline) {
        return cc_chan_deadline_recv(ch, out_value, value_size, cc__tls_current_deadline);
    }
    pthread_mutex_lock(&ch->mu);
    int err = cc_chan_ensure_buf(ch, value_size);
    if (err != 0) { pthread_mutex_unlock(&ch->mu); return err; }
    err = cc_chan_wait_empty(ch, NULL);
    if (err != 0) { pthread_mutex_unlock(&ch->mu); return err; }
    cc_chan_dequeue(ch, out_value);
    pthread_mutex_unlock(&ch->mu);
    return 0;
}

int cc_chan_try_send(CCChan* ch, const void* value, size_t value_size) {
    if (!ch || !value || value_size == 0) return EINVAL;
    pthread_mutex_lock(&ch->mu);
    int err = cc_chan_ensure_buf(ch, value_size);
    if (err != 0) { pthread_mutex_unlock(&ch->mu); return err; }
    if (ch->closed) { pthread_mutex_unlock(&ch->mu); return EPIPE; }
    if (ch->count == ch->cap) {
        err = cc_chan_handle_full_send(ch, value, NULL);
        if (err != 0) { pthread_mutex_unlock(&ch->mu); return err; }
    }
    cc_chan_enqueue(ch, value);
    pthread_mutex_unlock(&ch->mu);
    return 0;
}

int cc_chan_try_recv(CCChan* ch, void* out_value, size_t value_size) {
    if (!ch || !out_value || value_size == 0) return EINVAL;
    pthread_mutex_lock(&ch->mu);
    int err = cc_chan_ensure_buf(ch, value_size);
    if (err != 0) { pthread_mutex_unlock(&ch->mu); return err; }
    if (ch->count == 0) { pthread_mutex_unlock(&ch->mu); return ch->closed ? EPIPE : EAGAIN; }
    cc_chan_dequeue(ch, out_value);
    pthread_mutex_unlock(&ch->mu);
    return 0;
}

int cc_chan_timed_send(CCChan* ch, const void* value, size_t value_size, const struct timespec* abs_deadline) {
    if (!ch || !value || value_size == 0) return EINVAL;
    pthread_mutex_lock(&ch->mu);
    int err = cc_chan_ensure_buf(ch, value_size);
    if (err != 0) { pthread_mutex_unlock(&ch->mu); return err; }
    if (ch->closed) { pthread_mutex_unlock(&ch->mu); return EPIPE; }
    if (ch->count == ch->cap) {
        err = cc_chan_handle_full_send(ch, value, abs_deadline);
        if (err != 0) { pthread_mutex_unlock(&ch->mu); return err; }
    }
    cc_chan_enqueue(ch, value);
    pthread_mutex_unlock(&ch->mu);
    return 0;
}

int cc_chan_timed_recv(CCChan* ch, void* out_value, size_t value_size, const struct timespec* abs_deadline) {
    if (!ch || !out_value || value_size == 0) return EINVAL;
    pthread_mutex_lock(&ch->mu);
    int err = cc_chan_ensure_buf(ch, value_size);
    if (err != 0) { pthread_mutex_unlock(&ch->mu); return err; }
    err = cc_chan_wait_empty(ch, abs_deadline);
    if (err != 0) { pthread_mutex_unlock(&ch->mu); return err; }
    cc_chan_dequeue(ch, out_value);
    pthread_mutex_unlock(&ch->mu);
    return 0;
}

int cc_chan_deadline_send(CCChan* ch, const void* value, size_t value_size, const CCDeadline* deadline) {
    if (deadline && cc_is_cancelled(deadline)) return ECANCELED;
    struct timespec ts;
    const struct timespec* p = cc_deadline_as_timespec(deadline, &ts);
    return cc_chan_timed_send(ch, value, value_size, p);
}

int cc_chan_deadline_recv(CCChan* ch, void* out_value, size_t value_size, const CCDeadline* deadline) {
    if (deadline && cc_is_cancelled(deadline)) return ECANCELED;
    struct timespec ts;
    const struct timespec* p = cc_deadline_as_timespec(deadline, &ts);
    return cc_chan_timed_recv(ch, out_value, value_size, p);
}

int cc_chan_send_take(CCChan* ch, void* ptr) {
    if (!ch) return EINVAL;
    if (!ch->allow_take) return EINVAL;
    if (ch->elem_size && ch->elem_size != sizeof(void*)) return EINVAL;
    return cc_chan_send(ch, &ptr, sizeof(void*));
}

int cc_chan_try_send_take(CCChan* ch, void* ptr) {
    if (!ch) return EINVAL;
    if (!ch->allow_take) return EINVAL;
    if (ch->elem_size && ch->elem_size != sizeof(void*)) return EINVAL;
    return cc_chan_try_send(ch, &ptr, sizeof(void*));
}

int cc_chan_timed_send_take(CCChan* ch, void* ptr, const struct timespec* abs_deadline) {
    if (!ch) return EINVAL;
    if (!ch->allow_take) return EINVAL;
    if (ch->elem_size && ch->elem_size != sizeof(void*)) return EINVAL;
    return cc_chan_timed_send(ch, &ptr, sizeof(void*), abs_deadline);
}

int cc_chan_deadline_send_take(CCChan* ch, void* ptr, const CCDeadline* deadline) {
    struct timespec ts;
    const struct timespec* p = cc_deadline_as_timespec(deadline, &ts);
    return cc_chan_timed_send_take(ch, ptr, p);
}

static int cc_chan_check_slice_take(const CCSlice* slice) {
    if (!slice) return EINVAL;
    if (!cc_slice_is_unique(*slice)) return EINVAL;
    if (!cc_slice_is_transferable(*slice)) return EINVAL;
    if (cc_slice_is_subslice(*slice)) return EINVAL;
    return 0;
}

int cc_chan_send_take_slice(CCChan* ch, const CCSlice* slice) {
    if (!ch) return EINVAL;
    if (!ch->allow_take) return EINVAL;
    int elig = cc_chan_check_slice_take(slice);
    if (elig != 0) return elig;
    if (ch->elem_size && ch->elem_size != sizeof(CCSlice)) return EINVAL;
    return cc_chan_send(ch, slice, sizeof(CCSlice));
}

int cc_chan_try_send_take_slice(CCChan* ch, const CCSlice* slice) {
    if (!ch) return EINVAL;
    if (!ch->allow_take) return EINVAL;
    int elig = cc_chan_check_slice_take(slice);
    if (elig != 0) return elig;
    if (ch->elem_size && ch->elem_size != sizeof(CCSlice)) return EINVAL;
    return cc_chan_try_send(ch, slice, sizeof(CCSlice));
}

int cc_chan_timed_send_take_slice(CCChan* ch, const CCSlice* slice, const struct timespec* abs_deadline) {
    if (!ch) return EINVAL;
    if (!ch->allow_take) return EINVAL;
    int elig = cc_chan_check_slice_take(slice);
    if (elig != 0) return elig;
    if (ch->elem_size && ch->elem_size != sizeof(CCSlice)) return EINVAL;
    return cc_chan_timed_send(ch, slice, sizeof(CCSlice), abs_deadline);
}

int cc_chan_deadline_send_take_slice(CCChan* ch, const CCSlice* slice, const CCDeadline* deadline) {
    struct timespec ts;
    const struct timespec* p = cc_deadline_as_timespec(deadline, &ts);
    return cc_chan_timed_send_take_slice(ch, slice, p);
}

int cc_chan_nursery_send(CCChan* ch, CCNursery* n, const void* value, size_t value_size) {
    CCDeadline d = cc_nursery_as_deadline(n);
    return cc_chan_deadline_send(ch, value, value_size, &d);
}

int cc_chan_nursery_recv(CCChan* ch, CCNursery* n, void* out_value, size_t value_size) {
    CCDeadline d = cc_nursery_as_deadline(n);
    return cc_chan_deadline_recv(ch, out_value, value_size, &d);
}

int cc_chan_nursery_send_take(CCChan* ch, CCNursery* n, void* ptr) {
    CCDeadline d = cc_nursery_as_deadline(n);
    return cc_chan_deadline_send_take(ch, ptr, &d);
}

int cc_chan_nursery_send_take_slice(CCChan* ch, CCNursery* n, const CCSlice* slice) {
    CCDeadline d = cc_nursery_as_deadline(n);
    return cc_chan_deadline_send_take_slice(ch, slice, &d);
}

// Async channel operations via executor

typedef struct {
    CCChan* ch;
    const void* value;
    void* out_value;
    size_t size;
    int is_send; // 1 send, 0 recv
    CCDeadline deadline;
    CCAsyncHandle *handle;
} CCChanAsyncCtx;

static void cc__chan_async_job(void *arg) {
    CCChanAsyncCtx *ctx = (CCChanAsyncCtx*)arg;
    int err = 0;
    if (cc_deadline_expired(&ctx->deadline)) {
        err = ETIMEDOUT;
    } else {
        if (ctx->is_send) err = cc_chan_deadline_send(ctx->ch, ctx->value, ctx->size, &ctx->deadline);
        else err = cc_chan_deadline_recv(ctx->ch, ctx->out_value, ctx->size, &ctx->deadline);
    }
    cc_chan_send(ctx->handle->done, &err, sizeof(int));
    free(ctx);
}

static int cc__chan_async_submit(CCExec* ex, CCChan* ch, const void* val, void* out, size_t size, CCChanAsync* out_async, const CCDeadline* deadline, int is_send) {
    if (!ex || !ch || !out_async) return EINVAL;
    CC_ASYNC_HANDLE_ALLOC(&out_async->handle, 1);
    CCChanAsyncCtx *ctx = (CCChanAsyncCtx*)malloc(sizeof(*ctx));
    if (!ctx) { cc_chan_free(out_async->handle.done); out_async->handle.done = NULL; return ENOMEM; }
    ctx->ch = ch; ctx->value = val; ctx->out_value = out; ctx->size = size; ctx->is_send = is_send;
    ctx->deadline = deadline ? *deadline : cc_deadline_none();
    ctx->handle = &out_async->handle;
    int sub = cc_exec_submit(ex, cc__chan_async_job, ctx);
    if (sub != 0) { free(ctx); cc_chan_free(out_async->handle.done); out_async->handle.done = NULL; return sub; }
    return 0;
}

int cc_chan_send_async(CCExec* ex, CCChan* ch, const void* value, size_t value_size, CCChanAsync* out, const CCDeadline* deadline) {
    return cc__chan_async_submit(ex, ch, value, NULL, value_size, out, deadline, 1);
}

int cc_chan_recv_async(CCExec* ex, CCChan* ch, void* out_value, size_t value_size, CCChanAsync* out, const CCDeadline* deadline) {
    return cc__chan_async_submit(ex, ch, NULL, out_value, value_size, out, deadline, 0);
}

// Non-blocking match helper
int cc_chan_match_try(CCChanMatchCase* cases, size_t n, size_t* ready_index) {
    if (!cases || n == 0 || !ready_index) return EINVAL;
    for (size_t i = 0; i < n; ++i) {
        CCChanMatchCase *c = &cases[i];
        if (!c->ch || c->elem_size == 0) continue;
        int rc;
        if (c->is_send) {
            rc = cc_chan_try_send(c->ch, c->send_buf, c->elem_size);
        } else {
            rc = cc_chan_try_recv(c->ch, c->recv_buf, c->elem_size);
        }
        if (rc == 0) { *ready_index = i; return 0; }
        if (rc == EPIPE) { *ready_index = i; return EPIPE; }
    }
    return EAGAIN;
}

int cc_chan_match_deadline(CCChanMatchCase* cases, size_t n, size_t* ready_index, const CCDeadline* deadline) {
    if (!cases || n == 0 || !ready_index) return EINVAL;
    struct timespec ts;
    const struct timespec* p = cc_deadline_as_timespec(deadline, &ts);
    while (1) {
        int rc = cc_chan_match_try(cases, n, ready_index);
        if (rc == 0 || rc == EPIPE) return rc;
        if (rc != EAGAIN) return rc;
        if (p) {
            struct timespec now; clock_gettime(CLOCK_REALTIME, &now);
            if (now.tv_sec > p->tv_sec || (now.tv_sec == p->tv_sec && now.tv_nsec >= p->tv_nsec)) return ETIMEDOUT;
        }
        struct timespec sleep_ts = {0, 1000000};
        nanosleep(&sleep_ts, NULL);
    }
}

int cc_chan_match_select(CCChanMatchCase* cases, size_t n, size_t* ready_index, const CCDeadline* deadline) {
    return cc_chan_match_deadline(cases, n, ready_index, deadline);
}

// Async select using executor
typedef struct {
    CCChanMatchCase* cases;
    size_t n;
    size_t* ready_index;
    CCAsyncHandle* handle;
    CCDeadline deadline;
} CCChanMatchAsyncCtx;

static void cc__chan_match_async_job(void* arg) {
    CCChanMatchAsyncCtx* ctx = (CCChanMatchAsyncCtx*)arg;
    int err = cc_chan_match_select(ctx->cases, ctx->n, ctx->ready_index, &ctx->deadline);
    cc_chan_send(ctx->handle->done, &err, sizeof(int));
    free(ctx);
}

int cc_chan_match_select_async(CCExec* ex, CCChanMatchCase* cases, size_t n, size_t* ready_index, CCAsyncHandle* h, const CCDeadline* deadline) {
    if (!ex || !cases || n == 0 || !ready_index || !h) return EINVAL;
    CC_ASYNC_HANDLE_ALLOC(h, 1);
    CCChanMatchAsyncCtx* ctx = (CCChanMatchAsyncCtx*)malloc(sizeof(*ctx));
    if (!ctx) { cc_chan_free(h->done); h->done = NULL; return ENOMEM; }
    ctx->cases = cases;
    ctx->n = n;
    ctx->ready_index = ready_index;
    ctx->handle = h;
    ctx->deadline = deadline ? *deadline : cc_deadline_none();
    int sub = cc_exec_submit(ex, cc__chan_match_async_job, ctx);
    if (sub != 0) {
        fprintf(stderr, "cc_chan_match_select_async: submit failed (%d)\n", sub);
        free(ctx);
        cc_chan_free(h->done);
        h->done = NULL;
        return sub;
    }
    return 0;
}

// Future-based async select
typedef struct {
    CCChanMatchCase* cases;
    size_t n;
    size_t* ready_index;
    CCFuture* fut;
    CCDeadline deadline;
} CCChanMatchFutureCtx;

static void cc__chan_match_future_job(void* arg) {
    CCChanMatchFutureCtx* ctx = (CCChanMatchFutureCtx*)arg;
    int err = cc_chan_match_select(ctx->cases, ctx->n, ctx->ready_index, &ctx->deadline);
    int out_err = err < 0 ? err : 0; /* For now treat success/any positive errno as success for future helper. */
    cc_chan_send(ctx->fut->handle.done, &out_err, sizeof(int));
    free(ctx);
}

int cc_chan_match_select_future(CCExec* ex, CCChanMatchCase* cases, size_t n, size_t* ready_index, CCFuture* f, const CCDeadline* deadline) {
    if (!ex || !cases || n == 0 || !ready_index || !f) return EINVAL;
    cc_future_init(f);
    CC_ASYNC_HANDLE_ALLOC(&f->handle, 1);
    CCChanMatchFutureCtx* ctx = (CCChanMatchFutureCtx*)malloc(sizeof(*ctx));
    if (!ctx) { cc_future_free(f); return ENOMEM; }
    ctx->cases = cases;
    ctx->n = n;
    ctx->ready_index = ready_index;
    ctx->fut = f;
    ctx->deadline = deadline ? *deadline : cc_deadline_none();
    int sub = cc_exec_submit(ex, cc__chan_match_future_job, ctx);
    if (sub != 0) { free(ctx); cc_future_free(f); return sub; }
    return 0;
}

