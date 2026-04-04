#ifndef REDIS_LOCAL_READY_QUEUE_H
#define REDIS_LOCAL_READY_QUEUE_H

#include <stddef.h>
#include <stdatomic.h>
#include <string.h>

typedef struct CCLocalReadyQueue {
    _Atomic size_t head;
    _Atomic size_t tail;
    _Atomic int ready;
    size_t slots;
    size_t elem_size;
    void* storage;
} CCLocalReadyQueue;

typedef int (*CCLocalReadyQueueNotifyFn)(void* ctx);

static inline void cc_local_ready_queue_init(CCLocalReadyQueue* q,
                                             void* storage,
                                             size_t slots,
                                             size_t elem_size) {
    if (!q || !storage || slots == 0 || elem_size == 0) return;
    atomic_store_explicit(&q->head, 0, memory_order_relaxed);
    atomic_store_explicit(&q->tail, 0, memory_order_relaxed);
    atomic_store_explicit(&q->ready, 0, memory_order_relaxed);
    q->slots = slots;
    q->elem_size = elem_size;
    q->storage = storage;
}

static inline int cc_local_ready_queue_has_data(CCLocalReadyQueue* q) {
    if (!q) return 0;
    size_t head = atomic_load_explicit(&q->head, memory_order_acquire);
    size_t tail = atomic_load_explicit(&q->tail, memory_order_acquire);
    return head != tail;
}

static inline void* cc__local_ready_queue_push(CCLocalReadyQueue* q,
                                               const void* value,
                                               int* became_ready) {
    if (became_ready) *became_ready = 0;
    if (!q || !q->storage || !value || q->slots == 0 || q->elem_size == 0) return NULL;
    size_t tail = atomic_load_explicit(&q->tail, memory_order_relaxed);
    size_t head = atomic_load_explicit(&q->head, memory_order_acquire);
    size_t next = (tail + 1) % q->slots;
    if (next == head) return NULL;
    char* slot = (char*)q->storage + (tail * q->elem_size);
    memcpy(slot, value, q->elem_size);
    atomic_store_explicit(&q->tail, next, memory_order_release);
    if (atomic_exchange_explicit(&q->ready, 1, memory_order_acq_rel) == 0) {
        if (became_ready) *became_ready = 1;
    }
    return slot;
}

static inline int cc__local_ready_queue_cancel_push(CCLocalReadyQueue* q) {
    if (!q || !q->storage || q->slots == 0 || q->elem_size == 0) return 0;
    size_t tail = atomic_load_explicit(&q->tail, memory_order_relaxed);
    size_t head = atomic_load_explicit(&q->head, memory_order_acquire);
    if (head == tail) return 0;

    size_t prev = (tail + q->slots - 1) % q->slots;
    atomic_store_explicit(&q->tail, prev, memory_order_release);
    if (prev == head) {
        atomic_store_explicit(&q->ready, 0, memory_order_release);
    }
    return 1;
}

/* Submit one item and, if this queue became ready, run the notifier.
 * Returns the stored slot pointer on success and writes status=1.
 * On failure returns NULL and writes status:
 *   0  -> queue full
 *  -1  -> notification failed after enqueue; enqueue has been rolled back. */
static inline void* cc_local_ready_queue_submit(CCLocalReadyQueue* q,
                                                const void* value,
                                                CCLocalReadyQueueNotifyFn notify,
                                                void* notify_ctx,
                                                int* status) {
    int became_ready = 0;
    if (status) *status = 0;
    void* slot = cc__local_ready_queue_push(q, value, &became_ready);
    if (!slot) return NULL;
    if (became_ready && notify && !notify(notify_ctx)) {
        (void)cc__local_ready_queue_cancel_push(q);
        if (status) *status = -1;
        return NULL;
    }
    if (status) *status = 1;
    return slot;
}

static inline int cc_local_ready_queue_try_pop(CCLocalReadyQueue* q, void* out) {
    if (!q || !q->storage || !out || q->slots == 0 || q->elem_size == 0) return 0;
    size_t head = atomic_load_explicit(&q->head, memory_order_relaxed);
    size_t tail = atomic_load_explicit(&q->tail, memory_order_acquire);
    if (head == tail) return 0;
    char* slot = (char*)q->storage + (head * q->elem_size);
    memcpy(out, slot, q->elem_size);
    atomic_store_explicit(&q->head, (head + 1) % q->slots, memory_order_release);
    return 1;
}

static inline int cc_local_ready_queue_continue_drain(CCLocalReadyQueue* q) {
    if (!q) return 0;
    atomic_store_explicit(&q->ready, 0, memory_order_release);
    if (!cc_local_ready_queue_has_data(q)) return 0;
    if (atomic_exchange_explicit(&q->ready, 1, memory_order_acq_rel) == 0) {
        return 1;
    }
    return 0;
}

#endif /* REDIS_LOCAL_READY_QUEUE_H */
