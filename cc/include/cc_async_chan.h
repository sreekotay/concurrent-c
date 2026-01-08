/*
 * Native async channels (state-machine style, no executor threads).
 * Immediate completion when a counterpart is available; otherwise queues pending ops.
 */
#ifndef CC_ASYNC_CHAN_H
#define CC_ASYNC_CHAN_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#include "cc_sched.h"
#include "cc_channel.h"
#include "std/async_io.h"

typedef struct CCAsyncChan CCAsyncChan;

typedef struct {
    CCAsyncHandle handle;
    CCChanAsyncStatus status;
} CCAsyncChanOp;

CCAsyncChan* cc_async_chan_create(size_t capacity, CCChanMode mode, bool allow_send_take);
void cc_async_chan_close(CCAsyncChan* ch);
void cc_async_chan_free(CCAsyncChan* ch);

int cc_async_chan_send(CCAsyncChan* ch, const void* value, size_t value_size, CCAsyncChanOp* op);
int cc_async_chan_recv(CCAsyncChan* ch, void* out_value, size_t value_size, CCAsyncChanOp* op);

// Convenience wrappers with deadlines: return ETIMEDOUT if no progress until deadline.
int cc_async_chan_send_deadline(CCAsyncChan* ch, const void* value, size_t value_size, CCAsyncChanOp* op, const CCDeadline* d);
int cc_async_chan_recv_deadline(CCAsyncChan* ch, void* out_value, size_t value_size, CCAsyncChanOp* op, const CCDeadline* d);

#endif // CC_ASYNC_CHAN_H
