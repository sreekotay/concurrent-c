#include <ccc/std/future.cch>
#include <errno.h>
#include <stdlib.h>

int cc_async_wait(CCAsyncHandle* h) {
    int err = 0;
    int rc;
    if (!h || !h->done) return EINVAL;
    rc = cc_chan_recv(h->done, &err, sizeof(int));
    cc_async_handle_free(h);
    if (rc != 0) return rc;
    return err;
}

int cc_async_wait_timed(CCAsyncHandle* h, const struct timespec* abs_deadline) {
    int err = 0;
    int rc;
    if (!h || !h->done) return EINVAL;
    rc = cc_chan_timed_recv(h->done, &err, sizeof(int), abs_deadline);
    if (rc == 0) {
        cc_async_handle_free(h);
        return err;
    }
    return rc;
}

static CCFutureStatus cc__future_from_err(int err, int *out_err) {
    if (err == 0) return CC_FUTURE_READY;
    if (out_err) *out_err = err;
    if (err == ECANCELED) return CC_FUTURE_CANCELLED;
    if (err == ETIMEDOUT) return CC_FUTURE_TIMEOUT;
    return CC_FUTURE_ERR;
}

CCFutureStatus cc_future_poll(CCFuture *f, int *out_err) {
    int err = 0;
    int rc;
    if (!f) return CC_FUTURE_ERR;
    if (!f->handle.done) return CC_FUTURE_PENDING;
    rc = cc_chan_try_recv(f->handle.done, &err, sizeof(int));
    if (rc == EAGAIN) return CC_FUTURE_PENDING;
    if (rc == 0) {
        cc_chan_close(f->handle.done);
        cc_chan_free(f->handle.done);
        f->handle.done = NULL;
        return cc__future_from_err(err, out_err);
    }
    return CC_FUTURE_ERR;
}

CCFutureStatus cc_future_wait(CCFuture *f, int *out_err) {
    int err;
    if (!f) return CC_FUTURE_ERR;
    err = cc_async_wait(&f->handle);
    return cc__future_from_err(err, out_err);
}

CCFutureStatus cc_future_wait_deadline(CCFuture *f, const CCDeadline* deadline,
                                      int *out_err) {
    int err;
    if (!f) return CC_FUTURE_ERR;
    err = cc_async_wait_deadline(&f->handle, deadline);
    return cc__future_from_err(err, out_err);
}

int cc_future_wait_peek_err(CCFuture* f, int* out_err) {
    int err = 0;
    int rc;
    if (!f || !f->handle.done) return EINVAL;
    rc = cc_chan_recv(f->handle.done, &err, sizeof(int));
    if (rc != 0) return rc;
    rc = cc_chan_send(f->handle.done, &err, sizeof(int));
    if (rc != 0) return rc;
    if (out_err) *out_err = err;
    return 0;
}

static void cc__future_job(void *arg) {
    CCFutureCBThunk *c = (CCFutureCBThunk*)arg;
    int err = 0;
    CCFutureStatus st = cc_future_wait(c->f, &err);
    c->cb(st, c->user, err);
    free(c);
}

int cc_future_on_complete(CCExec* ex, CCFuture *f, cc_future_cb cb, void *user) {
    CCFutureCBThunk *ctx;
    int sub;
    if (!ex || !f || !cb) return EINVAL;
    ctx = (CCFutureCBThunk*)malloc(sizeof(CCFutureCBThunk));
    if (!ctx) return ENOMEM;
    ctx->f = f;
    ctx->cb = cb;
    ctx->user = user;
    sub = cc_exec_submit(ex, cc__future_job, ctx);
    if (sub != 0) {
        free(ctx);
        return sub;
    }
    return 0;
}
