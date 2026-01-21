#include "cc_async_chan.cch"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>

typedef struct PendingOp {
    CCAsyncChanOp *op;
    void *buf;    // owned copy for send; recv uses caller buf
    size_t size;
    int is_send;
} PendingOp;

struct CCAsyncChan {
    size_t cap;
    size_t count;
    size_t head;
    size_t tail;
    void *buf;
    size_t elem_size;
    int closed;
    CCChanMode mode;
    int allow_take;

    PendingOp *sends;
    size_t send_len, send_cap;
    PendingOp *recvs;
    size_t recv_len, recv_cap;

    pthread_mutex_t mu;
};

static int ensure_elem(CCAsyncChan* ch, size_t elem_size) {
    if (ch->elem_size == 0) { ch->elem_size = elem_size; return 0; }
    if (ch->elem_size != elem_size) return EINVAL;
    return 0;
}

static int ensure_buf(CCAsyncChan* ch) {
    if (!ch->buf) {
        ch->buf = malloc(ch->cap * ch->elem_size);
        if (!ch->buf) return ENOMEM;
    }
    return 0;
}

static int ensure_queue(PendingOp **arr, size_t *cap, size_t len) {
    if (len < *cap) return 0;
    size_t new_cap = *cap ? (*cap * 2) : 8;
    PendingOp *p = (PendingOp*)realloc(*arr, new_cap * sizeof(PendingOp));
    if (!p) return ENOMEM;
    *arr = p;
    *cap = new_cap;
    return 0;
}

static void complete_op(CCAsyncChanOp* op, int err) {
    if (!op) return;
    cc_chan_send(op->handle.done, &err, sizeof(int));
}

CCAsyncChan* cc_async_chan_create(size_t capacity, CCChanMode mode, bool allow_send_take) {
    CCAsyncChan* ch = (CCAsyncChan*)malloc(sizeof(CCAsyncChan));
    if (!ch) return NULL;
    memset(ch, 0, sizeof(*ch));
    ch->cap = capacity ? capacity : 64;
    ch->mode = mode;
    ch->allow_take = allow_send_take ? 1 : 0;
    pthread_mutex_init(&ch->mu, NULL);
    return ch;
}

void cc_async_chan_close(CCAsyncChan* ch) {
    if (!ch) return;
    pthread_mutex_lock(&ch->mu);
    ch->closed = 1;
    for (size_t i = 0; i < ch->send_len; ++i) complete_op(ch->sends[i].op, EPIPE);
    for (size_t i = 0; i < ch->recv_len; ++i) complete_op(ch->recvs[i].op, EPIPE);
    ch->send_len = ch->recv_len = 0;
    pthread_mutex_unlock(&ch->mu);
}

void cc_async_chan_free(CCAsyncChan* ch) {
    if (!ch) return;
    cc_async_chan_close(ch);
    free(ch->sends);
    free(ch->recvs);
    free(ch->buf);
    pthread_mutex_destroy(&ch->mu);
    free(ch);
}

static void fulfill_send(CCAsyncChan* ch, PendingOp *send_op, void* dst) {
    memcpy(dst, send_op->buf, ch->elem_size);
    complete_op(send_op->op, 0);
    free(send_op->buf);
}

static void fulfill_recv(CCAsyncChan* ch, PendingOp *recv_op, const void* src) {
    memcpy(recv_op->buf, src, ch->elem_size);
    complete_op(recv_op->op, 0);
}

static int push_send(CCAsyncChan* ch, const void* value, CCAsyncChanOp* op) {
    int err = ensure_queue(&ch->sends, &ch->send_cap, ch->send_len + 1);
    if (err != 0) return err;
    void *copy = malloc(ch->elem_size);
    if (!copy) return ENOMEM;
    memcpy(copy, value, ch->elem_size);
    PendingOp *p = &ch->sends[ch->send_len++];
    p->op = op; p->buf = copy; p->size = ch->elem_size; p->is_send = 1;
    return 0;
}

static int push_recv(CCAsyncChan* ch, void* out, CCAsyncChanOp* op) {
    int err = ensure_queue(&ch->recvs, &ch->recv_cap, ch->recv_len + 1);
    if (err != 0) return err;
    PendingOp *p = &ch->recvs[ch->recv_len++];
    p->op = op; p->buf = out; p->size = ch->elem_size; p->is_send = 0;
    return 0;
}

static int match_pending(CCAsyncChan* ch) {
    // Match send->recv if both pending
    if (ch->send_len > 0 && ch->recvs && ch->recv_len > 0) {
        PendingOp send_op = ch->sends[0];
        PendingOp recv_op = ch->recvs[0];
        memmove(ch->sends, ch->sends + 1, (ch->send_len - 1) * sizeof(PendingOp));
        ch->send_len--;
        memmove(ch->recvs, ch->recvs + 1, (ch->recv_len - 1) * sizeof(PendingOp));
        ch->recv_len--;
        fulfill_recv(ch, &recv_op, send_op.buf);
        fulfill_send(ch, &send_op, send_op.buf); // send already uses its own buffer
        return 1;
    }
    return 0;
}

static int buffer_enqueue(CCAsyncChan* ch, const void* value) {
    int err = ensure_buf(ch);
    if (err != 0) return err;
    if (ch->count == ch->cap) {
        if (ch->mode == CC_CHAN_MODE_DROP_NEW) return EAGAIN;
        if (ch->mode == CC_CHAN_MODE_DROP_OLD) {
            ch->head = (ch->head + 1) % ch->cap;
            ch->count--;
        } else {
            return EAGAIN;
        }
    }
    void *slot = (uint8_t*)ch->buf + ch->tail * ch->elem_size;
    memcpy(slot, value, ch->elem_size);
    ch->tail = (ch->tail + 1) % ch->cap;
    ch->count++;
    return 0;
}

static int buffer_dequeue(CCAsyncChan* ch, void* out) {
    if (ch->count == 0) return EAGAIN;
    void *slot = (uint8_t*)ch->buf + ch->head * ch->elem_size;
    memcpy(out, slot, ch->elem_size);
    ch->head = (ch->head + 1) % ch->cap;
    ch->count--;
    return 0;
}

int cc_async_chan_send(CCAsyncChan* ch, const void* value, size_t value_size, CCAsyncChanOp* op) {
    if (!ch || !value || !op || value_size == 0) return EINVAL;
    pthread_mutex_lock(&ch->mu);
    int err = ensure_elem(ch, value_size);
    if (err == 0) err = ensure_buf(ch);
    if (err != 0) { pthread_mutex_unlock(&ch->mu); return err; }
    CC_ASYNC_HANDLE_ALLOC(&op->handle, 1);
    op->status = CC_CHAN_ASYNC_PENDING;
    if (match_pending(ch)) {
        complete_op(op, 0);
        pthread_mutex_unlock(&ch->mu);
        return 0;
    }
    // If recv pending, match immediately
    if (ch->recv_len > 0) {
        PendingOp recv_op = ch->recvs[0];
        memmove(ch->recvs, ch->recvs + 1, (ch->recv_len - 1) * sizeof(PendingOp));
        ch->recv_len--;
        memcpy(recv_op.buf, value, ch->elem_size);
        complete_op(recv_op.op, 0);
        complete_op(op, 0);
        pthread_mutex_unlock(&ch->mu);
        return 0;
    }
    // Try buffer
    err = buffer_enqueue(ch, value);
    if (err == 0) {
        complete_op(op, 0);
    } else if (err == EAGAIN && ch->mode == CC_CHAN_MODE_BLOCK) {
        err = push_send(ch, value, op);
    }
    pthread_mutex_unlock(&ch->mu);
    return err;
}

static int cc_async_chan_check_slice_take(CCAsyncChan* ch, const CCSlice* slice) {
    if (!ch || !slice) return EINVAL;
    if (!ch->allow_take) return EINVAL;
    if (!cc_slice_is_unique(*slice)) return EINVAL;
    if (!cc_slice_is_transferable(*slice)) return EINVAL;
    if (cc_slice_is_subslice(*slice)) return EINVAL;
    return 0;
}

int cc_async_chan_send_take_slice(CCAsyncChan* ch, const CCSlice* slice, CCAsyncChanOp* op) {
    int elig = cc_async_chan_check_slice_take(ch, slice);
    if (elig != 0) return elig;
    return cc_async_chan_send(ch, slice, sizeof(CCSlice), op);
}

int cc_async_chan_recv(CCAsyncChan* ch, void* out_value, size_t value_size, CCAsyncChanOp* op) {
    if (!ch || !out_value || !op || value_size == 0) return EINVAL;
    pthread_mutex_lock(&ch->mu);
    int err = ensure_elem(ch, value_size);
    if (err != 0) { pthread_mutex_unlock(&ch->mu); return err; }
    CC_ASYNC_HANDLE_ALLOC(&op->handle, 1);
    op->status = CC_CHAN_ASYNC_PENDING;
    // If buffer has data
    if (buffer_dequeue(ch, out_value) == 0) {
        complete_op(op, 0);
        pthread_mutex_unlock(&ch->mu);
        return 0;
    }
    // If send pending
    if (ch->send_len > 0) {
        PendingOp send_op = ch->sends[0];
        memmove(ch->sends, ch->sends + 1, (ch->send_len - 1) * sizeof(PendingOp));
        ch->send_len--;
        memcpy(out_value, send_op.buf, ch->elem_size);
        complete_op(send_op.op, 0);
        free(send_op.buf);
        complete_op(op, 0);
        pthread_mutex_unlock(&ch->mu);
        return 0;
    }
    // Otherwise queue recv
    err = push_recv(ch, out_value, op);
    pthread_mutex_unlock(&ch->mu);
    return err;
}

static int wait_until(const CCDeadline* d) {
    if (!d || d->deadline.tv_sec == 0) return 0;
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    if (now.tv_sec > d->deadline.tv_sec || (now.tv_sec == d->deadline.tv_sec && now.tv_nsec >= d->deadline.tv_nsec)) return ETIMEDOUT;
    struct timespec sleep_ts = {0, 1000000};
    nanosleep(&sleep_ts, NULL);
    return 0;
}

int cc_async_chan_send_deadline(CCAsyncChan* ch, const void* value, size_t value_size, CCAsyncChanOp* op, const CCDeadline* d) {
    int err = cc_async_chan_send(ch, value, value_size, op);
    while (err == EAGAIN) {
        if (wait_until(d) == ETIMEDOUT) return ETIMEDOUT;
        err = cc_async_chan_send(ch, value, value_size, op);
    }
    return err;
}

int cc_async_chan_recv_deadline(CCAsyncChan* ch, void* out_value, size_t value_size, CCAsyncChanOp* op, const CCDeadline* d) {
    int err = cc_async_chan_recv(ch, out_value, value_size, op);
    while (err == EAGAIN) {
        if (wait_until(d) == ETIMEDOUT) return ETIMEDOUT;
        err = cc_async_chan_recv(ch, out_value, value_size, op);
    }
    return err;
}

