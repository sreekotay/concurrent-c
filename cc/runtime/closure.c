/*
 * Closure helpers (early).
 */

#include <ccc/cc_closure.cch>
#include <ccc/cc_sched.cch>
#include <ccc/cc_nursery.cch>

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdatomic.h>

/* TSan annotations for closure capture synchronization */
#include "tsan_helpers.h"

typedef struct {
    CCClosure0 c;
} CCClosure0Heap;

static void* cc__closure0_trampoline(void* p) {
    CCClosure0Heap* h = (CCClosure0Heap*)p;
    if (!h) return NULL;
    CCClosure0 c = h->c;
    free(h);
    /* Acquire fence + TSan annotation ensures captured values are visible */
    atomic_thread_fence(memory_order_acquire);
    TSAN_ACQUIRE(c.env);
    void* r = NULL;
    if (c.fn) r = c.fn(c.env);
    if (c.drop) c.drop(c.env);
    return r;
}

int cc_nursery_spawn_closure0(CCNursery* n, CCClosure0 c) {
    if (!n || !c.fn) return EINVAL;
    CCClosure0Heap* h = (CCClosure0Heap*)malloc(sizeof(CCClosure0Heap));
    if (!h) return ENOMEM;
    h->c = c;
    /* TSan release: sync with acquire in trampoline to make captures visible */
    TSAN_RELEASE(c.env);
    int err = cc_nursery_spawn(n, cc__closure0_trampoline, h);
    if (err != 0) free(h);
    return err;
}

typedef struct {
    CCClosure1 c;
    intptr_t arg0;
} CCClosure1Heap;

static void* cc__closure1_trampoline(void* p) {
    CCClosure1Heap* h = (CCClosure1Heap*)p;
    if (!h) return NULL;
    CCClosure1 c = h->c;
    intptr_t a0 = h->arg0;
    free(h);
    /* Acquire fence + TSan annotation ensures captured values are visible */
    atomic_thread_fence(memory_order_acquire);
    TSAN_ACQUIRE(c.env);
    void* r = NULL;
    if (c.fn) r = c.fn(c.env, a0);
    if (c.drop) c.drop(c.env);
    return r;
}

int cc_nursery_spawn_closure1(CCNursery* n, CCClosure1 c, intptr_t arg0) {
    if (!n || !c.fn) return EINVAL;
    CCClosure1Heap* h = (CCClosure1Heap*)malloc(sizeof(CCClosure1Heap));
    if (!h) return ENOMEM;
    h->c = c;
    h->arg0 = arg0;
    /* TSan release: sync with acquire in trampoline to make captures visible */
    TSAN_RELEASE(c.env);
    int err = cc_nursery_spawn(n, cc__closure1_trampoline, h);
    if (err != 0) free(h);
    return err;
}

typedef struct {
    CCClosure2 c;
    intptr_t arg0;
    intptr_t arg1;
} CCClosure2Heap;

static void* cc__closure2_trampoline(void* p) {
    CCClosure2Heap* h = (CCClosure2Heap*)p;
    if (!h) return NULL;
    CCClosure2 c = h->c;
    intptr_t a0 = h->arg0;
    intptr_t a1 = h->arg1;
    free(h);
    /* Acquire fence + TSan annotation ensures captured values are visible */
    atomic_thread_fence(memory_order_acquire);
    TSAN_ACQUIRE(c.env);
    void* r = NULL;
    if (c.fn) r = c.fn(c.env, a0, a1);
    if (c.drop) c.drop(c.env);
    return r;
}

int cc_nursery_spawn_closure2(CCNursery* n, CCClosure2 c, intptr_t arg0, intptr_t arg1) {
    if (!n || !c.fn) return EINVAL;
    CCClosure2Heap* h = (CCClosure2Heap*)malloc(sizeof(CCClosure2Heap));
    if (!h) return ENOMEM;
    h->c = c;
    h->arg0 = arg0;
    h->arg1 = arg1;
    /* TSan release: sync with acquire in trampoline to make captures visible */
    TSAN_RELEASE(c.env);
    int err = cc_nursery_spawn(n, cc__closure2_trampoline, h);
    if (err != 0) free(h);
    return err;
}

int cc_run_blocking_closure0(CCClosure0 c) {
    if (!c.fn) return EINVAL;
    CCTask t = cc_spawn_closure0(c);
    if (t.kind == CC_TASK_KIND_INVALID) return ENOMEM;
    (void)cc_block_on_intptr(t);  /* blocks until done and frees task */
    return 0;
}

void* cc_run_blocking_closure0_ptr(CCClosure0 c) {
    if (!c.fn) return NULL;
    CCTask t = cc_spawn_closure0(c);
    if (t.kind == CC_TASK_KIND_INVALID) return NULL;
    return (void*)cc_block_on_intptr(t);  /* blocks until done and frees task */
}

void* cc_closure1_call(CCClosure1 c, intptr_t arg0) {
    if (!c.fn) return NULL;
    void* r = c.fn(c.env, arg0);
    if (c.drop) c.drop(c.env);
    return r;
}

void* cc_closure2_call(CCClosure2 c, intptr_t arg0, intptr_t arg1) {
    if (!c.fn) return NULL;
    void* r = c.fn(c.env, arg0, arg1);
    if (c.drop) c.drop(c.env);
    return r;
}

