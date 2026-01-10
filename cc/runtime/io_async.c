#include "cc_exec.cch"
#include "std/io.cch"
#include "cc_sched.cch"
#include "std/async_io.cch"
#include "cc_async_runtime.cch"
#include "cc_async_backend.cch"

#include <stdlib.h>
#include <errno.h>

static inline bool cc__deadline_expired(const CCDeadline* d) {
    return d && cc_deadline_expired(d);
}

static inline bool cc__cancelled(CCAsyncHandle* h) {
    return h && h->cancelled;
}

static inline int cc__check_pre(CCAsyncHandle* h, const CCDeadline* d) {
    if (cc__cancelled(h)) return ECANCELED;
    if (cc__deadline_expired(d)) return ETIMEDOUT;
    return 0;
}

static inline const CCAsyncBackendOps* cc__backend(void** out_ctx) {
    return cc_async_runtime_backend(out_ctx);
}

// ---------------- read_all ----------------

typedef struct {
    CCFile *file;
    CCArena *arena;
    CCSlice *out_slice;
    CCAsyncHandle *handle;
    CCDeadline deadline;
} CCReadAllCtx;

static void cc__job_read_all(void *arg) {
    CCReadAllCtx *ctx = (CCReadAllCtx*)arg;
    int err = cc__check_pre(ctx->handle, &ctx->deadline);
    if (err == 0) {
        CCResultSliceIoError res = cc_file_read_all(ctx->file, ctx->arena);
        err = res.is_err ? EIO : 0;
        if (!res.is_err && ctx->out_slice) *(ctx->out_slice) = res.ok;
    }
    cc_chan_send(ctx->handle->done, &err, sizeof(int));
    free(ctx);
}

int cc_file_read_all_async(CCExec* ex, CCFile *file, CCArena *arena, CCSlice* out, CCAsyncHandle* h) {
    return cc_file_read_all_async_deadline(ex, file, arena, out, h, NULL);
}

int cc_file_read_all_async_deadline(CCExec* ex, CCFile *file, CCArena *arena, CCSlice* out, CCAsyncHandle* h, const CCDeadline* deadline) {
    if (!file || !arena || !out || !h) return EINVAL;
    int pre = cc__check_pre(h, deadline);
    if (pre != 0) return pre;
    void* bctx = NULL;
    const CCAsyncBackendOps* b = cc__backend(&bctx);
    if (b && b->read_all) {
        return b->read_all(bctx, file, arena, out, h, deadline);
    }
    if (!ex) return EINVAL;
    CC_ASYNC_HANDLE_ALLOC(h, 1);
    CCReadAllCtx *ctx = (CCReadAllCtx*)malloc(sizeof(*ctx));
    if (!ctx) { cc_chan_free(h->done); h->done = NULL; return ENOMEM; }
    ctx->file = file; ctx->arena = arena; ctx->out_slice = out; ctx->handle = h; ctx->deadline = deadline ? *deadline : cc_deadline_none();
    int sub = cc_exec_submit(ex, cc__job_read_all, ctx);
    if (sub != 0) { free(ctx); cc_chan_free(h->done); h->done = NULL; return sub; }
    return 0;
}

// ---------------- read ----------------

typedef struct {
    CCFile *file;
    CCArena *arena;
    size_t n;
    CCFileReadResult *out;
    CCAsyncHandle *handle;
    CCDeadline deadline;
} CCReadCtx;

static void cc__job_read(void *arg) {
    CCReadCtx *ctx = (CCReadCtx*)arg;
    int err = cc__check_pre(ctx->handle, &ctx->deadline);
    if (err == 0) {
        CCResultFileReadIoError res = cc_file_read(ctx->file, ctx->arena, ctx->n);
        err = res.is_err ? EIO : 0;
        if (!res.is_err && ctx->out) *(ctx->out) = res.ok;
    }
    cc_chan_send(ctx->handle->done, &err, sizeof(int));
    free(ctx);
}

int cc_file_read_async(CCExec* ex, CCFile *file, CCArena *arena, size_t n, CCFileReadResult* out, CCAsyncHandle* h) {
    return cc_file_read_async_deadline(ex, file, arena, n, out, h, NULL);
}

int cc_file_read_async_deadline(CCExec* ex, CCFile *file, CCArena *arena, size_t n, CCFileReadResult* out, CCAsyncHandle* h, const CCDeadline* deadline) {
    if (!file || !arena || !out || !h) return EINVAL;
    int pre = cc__check_pre(h, deadline);
    if (pre != 0) return pre;
    void* bctx = NULL;
    const CCAsyncBackendOps* b = cc__backend(&bctx);
    if (b && b->read) return b->read(bctx, file, arena, n, out, h, deadline);
    if (!ex) return EINVAL;
    CC_ASYNC_HANDLE_ALLOC(h, 1);
    CCReadCtx *ctx = (CCReadCtx*)malloc(sizeof(*ctx));
    if (!ctx) { cc_chan_free(h->done); h->done = NULL; return ENOMEM; }
    ctx->file = file; ctx->arena = arena; ctx->n = n; ctx->out = out; ctx->handle = h; ctx->deadline = deadline ? *deadline : cc_deadline_none();
    int sub = cc_exec_submit(ex, cc__job_read, ctx);
    if (sub != 0) { free(ctx); cc_chan_free(h->done); h->done = NULL; return sub; }
    return 0;
}

// ---------------- read_line ----------------

typedef struct {
    CCFile *file;
    CCArena *arena;
    CCFileReadResult *out;
    CCAsyncHandle *handle;
    CCDeadline deadline;
} CCReadLineCtx;

static void cc__job_read_line(void *arg) {
    CCReadLineCtx *ctx = (CCReadLineCtx*)arg;
    int err = cc__check_pre(ctx->handle, &ctx->deadline);
    if (err == 0) {
        CCResultFileReadIoError res = cc_file_read_line(ctx->file, ctx->arena);
        err = res.is_err ? EIO : 0;
        if (!res.is_err && ctx->out) *(ctx->out) = res.ok;
    }
    cc_chan_send(ctx->handle->done, &err, sizeof(int));
    free(ctx);
}

int cc_file_read_line_async(CCExec* ex, CCFile *file, CCArena *arena, CCFileReadResult* out, CCAsyncHandle* h) {
    return cc_file_read_line_async_deadline(ex, file, arena, out, h, NULL);
}

int cc_file_read_line_async_deadline(CCExec* ex, CCFile *file, CCArena *arena, CCFileReadResult* out, CCAsyncHandle* h, const CCDeadline* deadline) {
    if (!file || !arena || !out || !h) return EINVAL;
    int pre = cc__check_pre(h, deadline);
    if (pre != 0) return pre;
    void* bctx = NULL;
    const CCAsyncBackendOps* b = cc__backend(&bctx);
    if (b && b->read_line) return b->read_line(bctx, file, arena, out, h, deadline);
    if (!ex) return EINVAL;
    CC_ASYNC_HANDLE_ALLOC(h, 1);
    CCReadLineCtx *ctx = (CCReadLineCtx*)malloc(sizeof(*ctx));
    if (!ctx) { cc_chan_free(h->done); h->done = NULL; return ENOMEM; }
    ctx->file = file; ctx->arena = arena; ctx->out = out; ctx->handle = h; ctx->deadline = deadline ? *deadline : cc_deadline_none();
    int sub = cc_exec_submit(ex, cc__job_read_line, ctx);
    if (sub != 0) { free(ctx); cc_chan_free(h->done); h->done = NULL; return sub; }
    return 0;
}

// ---------------- write ----------------

typedef struct {
    CCFile *file;
    CCSlice data;
    size_t *out_written;
    CCAsyncHandle *handle;
    CCDeadline deadline;
} CCWriteCtx;

static void cc__job_write(void *arg) {
    CCWriteCtx *ctx = (CCWriteCtx*)arg;
    int err = cc__check_pre(ctx->handle, &ctx->deadline);
    if (err == 0) {
        CCResultSizeIoError res = cc_file_write(ctx->file, ctx->data);
        err = res.is_err ? EIO : 0;
        if (!res.is_err && ctx->out_written) *(ctx->out_written) = res.ok;
    }
    cc_chan_send(ctx->handle->done, &err, sizeof(int));
    free(ctx);
}

int cc_file_write_async(CCExec* ex, CCFile *file, CCSlice data, size_t* out_written, CCAsyncHandle* h) {
    return cc_file_write_async_deadline(ex, file, data, out_written, h, NULL);
}

int cc_file_write_async_deadline(CCExec* ex, CCFile *file, CCSlice data, size_t* out_written, CCAsyncHandle* h, const CCDeadline* deadline) {
    if (!file || !h) return EINVAL;
    int pre = cc__check_pre(h, deadline);
    if (pre != 0) return pre;
    void* bctx = NULL;
    const CCAsyncBackendOps* b = cc__backend(&bctx);
    if (b && b->write) return b->write(bctx, file, data, out_written, h, deadline);
    if (!ex) return EINVAL;
    CC_ASYNC_HANDLE_ALLOC(h, 1);
    CCWriteCtx *ctx = (CCWriteCtx*)malloc(sizeof(*ctx));
    if (!ctx) { cc_chan_free(h->done); h->done = NULL; return ENOMEM; }
    ctx->file = file; ctx->data = data; ctx->out_written = out_written; ctx->handle = h; ctx->deadline = deadline ? *deadline : cc_deadline_none();
    int sub = cc_exec_submit(ex, cc__job_write, ctx);
    if (sub != 0) { free(ctx); cc_chan_free(h->done); h->done = NULL; return sub; }
    return 0;
}

// ---------------- open/close ----------------

typedef struct { CCFile* file; const char* path; const char* mode; CCAsyncHandle* h; CCDeadline deadline; } CCOpenCtx;
static void cc__job_open(void* arg) {
    CCOpenCtx* c = (CCOpenCtx*)arg;
    int err = cc__check_pre(c->h, &c->deadline);
    if (err == 0) {
        err = cc_file_open(c->file, c->path, c->mode);
    }
    cc_chan_send(c->h->done, &err, sizeof(int));
    free(c);
}

int cc_file_open_async(CCExec* ex, CCFile *file, const char *path, const char *mode, CCAsyncHandle* h) {
    return cc_file_open_async_deadline(ex, file, path, mode, h, NULL);
}

int cc_file_open_async_deadline(CCExec* ex, CCFile *file, const char *path, const char *mode, CCAsyncHandle* h, const CCDeadline* deadline) {
    if (!file || !path || !mode || !h) return EINVAL;
    int pre = cc__check_pre(h, deadline);
    if (pre != 0) return pre;
    void* bctx = NULL;
    const CCAsyncBackendOps* b = cc__backend(&bctx);
    if (b && b->open) return b->open(bctx, file, path, mode, h, deadline);
    if (!ex) return EINVAL;
    CC_ASYNC_HANDLE_ALLOC(h, 1);
    CCOpenCtx* ctx = (CCOpenCtx*)malloc(sizeof(*ctx));
    if (!ctx) { cc_chan_free(h->done); h->done = NULL; return ENOMEM; }
    ctx->file = file; ctx->path = path; ctx->mode = mode; ctx->h = h; ctx->deadline = deadline ? *deadline : cc_deadline_none();
    int sub = cc_exec_submit(ex, cc__job_open, ctx);
    if (sub != 0) { free(ctx); cc_chan_free(h->done); h->done = NULL; return sub; }
    return 0;
}

typedef struct { CCFile* file; CCAsyncHandle* h; CCDeadline deadline; } CCCloseCtx;
static void cc__job_close(void* arg) {
    CCCloseCtx* c = (CCCloseCtx*)arg;
    int err = cc__check_pre(c->h, &c->deadline);
    if (err == 0) {
        cc_file_close(c->file);
    }
    cc_chan_send(c->h->done, &err, sizeof(int));
    free(c);
}

int cc_file_close_async_deadline(CCExec* ex, CCFile *file, CCAsyncHandle* h, const CCDeadline* deadline) {
    if (!file || !h) return EINVAL;
    int pre = cc__check_pre(h, deadline);
    if (pre != 0) return pre;
    void* bctx = NULL;
    const CCAsyncBackendOps* b = cc__backend(&bctx);
    if (b && b->close) return b->close(bctx, file, h, deadline);
    if (!ex) return EINVAL;
    CC_ASYNC_HANDLE_ALLOC(h, 1);
    CCCloseCtx* ctx = (CCCloseCtx*)malloc(sizeof(*ctx));
    if (!ctx) { cc_chan_free(h->done); h->done = NULL; return ENOMEM; }
    ctx->file = file; ctx->h = h; ctx->deadline = deadline ? *deadline : cc_deadline_none();
    int sub = cc_exec_submit(ex, cc__job_close, ctx);
    if (sub != 0) { free(ctx); cc_chan_free(h->done); h->done = NULL; return sub; }
    return 0;
}

int cc_file_close_async(CCExec* ex, CCFile *file, CCAsyncHandle* h) {
    return cc_file_close_async_deadline(ex, file, h, NULL);
}

