/*
 * Closure helpers (early).
 */

#include "cc_closure.cch"
#include "cc_sched.cch"
#include "cc_nursery.cch"

#include <errno.h>
#include <stdlib.h>

typedef struct {
    CCClosure0 c;
} CCClosure0Heap;

static void* cc__closure0_trampoline(void* p) {
    CCClosure0Heap* h = (CCClosure0Heap*)p;
    if (!h) return NULL;
    CCClosure0 c = h->c;
    free(h);
    void* r = NULL;
    if (c.fn) r = c.fn(c.env);
    if (c.drop) c.drop(c.env);
    return r;
}

int cc_spawn_closure0(CCTask** out_task, CCClosure0 c) {
    if (!out_task || !c.fn) return EINVAL;
    CCClosure0Heap* h = (CCClosure0Heap*)malloc(sizeof(CCClosure0Heap));
    if (!h) return ENOMEM;
    h->c = c;
    int err = cc_spawn(out_task, cc__closure0_trampoline, h);
    if (err != 0) free(h);
    return err;
}

int cc_nursery_spawn_closure0(CCNursery* n, CCClosure0 c) {
    if (!n || !c.fn) return EINVAL;
    CCClosure0Heap* h = (CCClosure0Heap*)malloc(sizeof(CCClosure0Heap));
    if (!h) return ENOMEM;
    h->c = c;
    int err = cc_nursery_spawn(n, cc__closure0_trampoline, h);
    if (err != 0) free(h);
    return err;
}

int cc_run_blocking_closure0(CCClosure0 c) {
    if (!c.fn) return EINVAL;
    CCTask* t = NULL;
    int err = cc_spawn_closure0(&t, c);
    if (err != 0) return err;
    int j = cc_task_join(t);
    cc_task_free(t);
    return j;
}

void* cc_closure1_call(CCClosure1 c, void* arg0) {
    if (!c.fn) return NULL;
    void* r = c.fn(c.env, arg0);
    if (c.drop) c.drop(c.env);
    return r;
}