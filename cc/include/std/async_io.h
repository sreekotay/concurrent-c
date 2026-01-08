/*
 * Async I/O wrappers using the portable CCExec offload executor.
 * Callers submit an async op with a CCAsyncHandle and wait with cc_async_wait().
 * Result data is written into caller-provided storage.
 */
#ifndef CC_STD_ASYNC_IO_H
#define CC_STD_ASYNC_IO_H

#include <stddef.h>
#include <errno.h>
#include <stdio.h>

#include "../cc_exec.h"
#include "../cc_sched.h"

/* Minimal channel API surface to avoid header cycles. */
typedef struct CCChan CCChan;
int cc_chan_recv(CCChan* ch, void* out_value, size_t value_size);
int cc_chan_timed_recv(CCChan* ch, void* out_value, size_t value_size, const struct timespec* abs_deadline);
int cc_chan_try_recv(CCChan* ch, void* out_value, size_t value_size);
int cc_chan_send(CCChan* ch, const void* value, size_t value_size);
void cc_chan_close(CCChan* ch);
void cc_chan_free(CCChan* ch);
CCChan* cc_chan_create(size_t capacity);

typedef struct CCAsyncHandle {
    CCChan* done;
    volatile int cancelled;
} CCAsyncHandle;

static inline void cc_async_handle_init(CCAsyncHandle* h) {
    if (!h) return;
    h->done = NULL;
    h->cancelled = 0;
}

static inline void cc_async_handle_free(CCAsyncHandle* h) {
    if (!h) return;
    if (h->done) {
        cc_chan_close(h->done);
        cc_chan_free(h->done);
        h->done = NULL;
    }
    h->cancelled = 0;
}

// Wait for completion; returns submitted error code (0 on success) or channel error.
static inline int cc_async_wait(CCAsyncHandle* h) {
    if (!h || !h->done) return EINVAL;
    int err = 0;
    int rc = cc_chan_recv(h->done, &err, sizeof(int));
    cc_async_handle_free(h);
    if (rc != 0) return rc;
    return err;
}

// Wait with absolute deadline; returns ETIMEDOUT on timeout.
static inline int cc_async_wait_timed(CCAsyncHandle* h, const struct timespec* abs_deadline) {
    if (!h || !h->done) return EINVAL;
    int err = 0;
    int rc = cc_chan_timed_recv(h->done, &err, sizeof(int), abs_deadline);
    if (rc == 0) {
        cc_async_handle_free(h);
        return err;
    }
    return rc;
}

static inline void cc_async_cancel(CCAsyncHandle* h) {
    if (!h) return;
    h->cancelled = 1;
}

// Wait with CCDeadline helper.
static inline int cc_async_wait_deadline(CCAsyncHandle* h, const CCDeadline* deadline) {
    struct timespec ts;
    const struct timespec* p = cc_deadline_as_timespec(deadline, &ts);
    return cc_async_wait_timed(h, p);
}

// Internal helper macro to allocate handle channel
#define CC_ASYNC_HANDLE_ALLOC(handle_ptr, cap)                  \
    do {                                                        \
        (handle_ptr)->done = cc_chan_create(cap);               \
        if (!(handle_ptr)->done) return ENOMEM;                 \
        (handle_ptr)->cancelled = 0;                            \
    } while (0)

#endif // CC_STD_ASYNC_IO_H
