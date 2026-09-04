/*
 * Closure helpers (early).
 */

#include <ccc/cc_closure.cch>
#include <ccc/cc_arena.cch>
#include <ccc/cc_sched.cch>
#include <ccc/cc_nursery.cch>

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <string.h>

/* TSan annotations for closure capture synchronization */
#include "tsan_helpers.h"

/* ---- Closure environment pool -------------------------------------------
 *
 * Closure-literal environments are tiny (a few pointers) and short-lived
 * (single-shot call-or-drop), which makes malloc/free per closure pure
 * overhead on hot paths.  Back them with CCArenaPool size classes instead.
 *
 * Pools are thread-local because CCArenaPool's freelist is CAS-safe from
 * any thread but its bump-grow fallback (cc_arena_alloc) is not: with one
 * pool per thread the bump path only ever runs on the owning thread, while
 * a cross-thread free simply pushes onto the freeing thread's freelist.
 * That migration is safe because pools live for the process (no thread-exit
 * destructor): no pool arena is ever freed, so a slot parked on another
 * thread's freelist can never dangle.  Memory cost is bounded by each
 * thread's peak live envs (4 KiB slabs, grown on demand).
 *
 * Sizes above the largest class fall back to malloc/free; the size is
 * passed back in at free (generated drops know sizeof(env)), so no
 * per-block header is needed. */

/* Doubling size classes from 16 (two pointers — the common env shape).
 * Free makes a slot available for the next alloc of its class; nothing
 * else to manage. */
enum { CC__ENV_NCLASS = 5 };
static const size_t cc__env_class_size[CC__ENV_NCLASS] = { 16, 32, 64, 128, 256 };

typedef struct {
    CCArenaPool pool[CC__ENV_NCLASS];
    int ready;
} CCEnvPools;

#if defined(__TINYC__)
/* Host TCC has no _Thread_local; park pools in the pthread TLS bundle. */
static CCEnvPools* cc__env_pools_ptr(void) {
    cc_rt_tls* t = cc_rt_tls_get();
    if (!t) return NULL;
    if (!t->env_pools) t->env_pools = calloc(1, sizeof(CCEnvPools));
    return (CCEnvPools*)t->env_pools;
}
#define cc__env_pools (*cc__env_pools_ptr())
#else
static _Thread_local CCEnvPools cc__env_pools;
#endif

static int cc__env_class_for(size_t size) {
    for (int i = 0; i < CC__ENV_NCLASS; i++) {
        if (size <= cc__env_class_size[i]) return i;
    }
    return -1;
}

static int cc__env_pools_ensure(void) {
    if (cc__env_pools.ready) return 1;
    for (int i = 0; i < CC__ENV_NCLASS; i++) {
        if (cc_arena_pool(&cc__env_pools.pool[i], cc__env_class_size[i]) != 0) {
            while (i-- > 0) cc_arena_pool_destroy(&cc__env_pools.pool[i]);
            return 0;
        }
    }
    cc__env_pools.ready = 1;
    return 1;
}

void* cc_closure_env_alloc(size_t size) {
    int cls = cc__env_class_for(size);
    if (cls < 0) return malloc(size);
    /* Init failure falls back to malloc — functionally identical.  Allocate
     * the full class size: the matching free classifies by size and may push
     * this block onto a pool freelist (on a thread whose pools are live),
     * where it can be handed back out as a class-sized slot. */
    if (!cc__env_pools_ensure()) return malloc(cc__env_class_size[cls]);
    return cc_arena_pool_alloc(&cc__env_pools.pool[cls]);
}

void cc_closure_env_free(void* p, size_t size) {
    if (!p) return;
    int cls = cc__env_class_for(size);
    if (cls < 0) {
        free(p);
        return;
    }
    /* A class-sized block is pool memory (interior arena pointer) — it must
     * be pushed onto a pool freelist, never free()d.  A cross-thread free
     * can land before this thread's first alloc, so ensure pools here too.
     * If init fails the block is stranded (leaked, safe); its home arena
     * lives for the process either way. */
    if (!cc__env_pools_ensure()) return;
    cc_arena_pool_free(&cc__env_pools.pool[cls], p);
}

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

CCResult_void_CCError cc_nursery_spawn_closure0_host(CCNurseryHost* n, CCClosure0 c) {
    CCClosure0Heap* h;
    CCResult_void_CCError r;
    if (!n || !c.fn)
        return cc_err_CCResult_void_CCError(
            CC_ERROR(CC_ERR_INVALID_ARG, "cc_nursery_spawn_closure0"));
    h = (CCClosure0Heap*)malloc(sizeof(CCClosure0Heap));
    if (!h)
        return cc_err_CCResult_void_CCError(
            CC_ERROR(CC_ERR_OUT_OF_MEMORY, "cc_nursery_spawn_closure0"));
    h->c = c;
    /* TSan release: sync with acquire in trampoline to make captures visible */
    TSAN_RELEASE(c.env);
    r = cc_nursery_spawn_host(n, cc__closure0_trampoline, h);
    if (!r.ok) free(h);
    return r;
}

CCResult_CCNursery_CCError cc_nursery_spawn_child_closure0(CCNursery parent, CCClosure0 c) {
    CCResult_CCNursery_CCError r;
    if (!c.fn)
        return cc_err_CCResult_CCNursery_CCError(
            CC_ERROR(CC_ERR_INVALID_ARG, "cc_nursery_spawn_child_closure0: null fn"));
    r = cc_nursery_create_child(parent);
    if (!r.ok) return r;
    {
        CCResult_void_CCError s = cc_nursery_spawn_closure0(r.u.value, c);
        if (!s.ok) {
            cc_nursery_free(r.u.value);
            return cc_err_CCResult_CCNursery_CCError(s.u.error);
        }
    }
    return r;
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

CCResult_void_CCError cc_nursery_spawn_closure1_host(CCNurseryHost* n, CCClosure1 c, intptr_t arg0) {
    CCClosure1Heap* h;
    CCResult_void_CCError r;
    if (!n || !c.fn)
        return cc_err_CCResult_void_CCError(
            CC_ERROR(CC_ERR_INVALID_ARG, "cc_nursery_spawn_closure1"));
    h = (CCClosure1Heap*)malloc(sizeof(CCClosure1Heap));
    if (!h)
        return cc_err_CCResult_void_CCError(
            CC_ERROR(CC_ERR_OUT_OF_MEMORY, "cc_nursery_spawn_closure1"));
    h->c = c;
    h->arg0 = arg0;
    /* TSan release: sync with acquire in trampoline to make captures visible */
    TSAN_RELEASE(c.env);
    r = cc_nursery_spawn_host(n, cc__closure1_trampoline, h);
    if (!r.ok) free(h);
    return r;
}

CCResult_CCNursery_CCError cc_nursery_spawn_child_closure1(CCNursery parent, CCClosure1 c, intptr_t arg0) {
    CCResult_CCNursery_CCError r;
    if (!c.fn)
        return cc_err_CCResult_CCNursery_CCError(
            CC_ERROR(CC_ERR_INVALID_ARG, "cc_nursery_spawn_child_closure1: null fn"));
    r = cc_nursery_create_child(parent);
    if (!r.ok) return r;
    {
        CCResult_void_CCError s = cc_nursery_spawn_closure1(r.u.value, c, arg0);
        if (!s.ok) {
            cc_nursery_free(r.u.value);
            return cc_err_CCResult_CCNursery_CCError(s.u.error);
        }
    }
    return r;
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

CCResult_void_CCError cc_nursery_spawn_closure2_host(CCNurseryHost* n, CCClosure2 c, intptr_t arg0, intptr_t arg1) {
    CCClosure2Heap* h;
    CCResult_void_CCError r;
    if (!n || !c.fn)
        return cc_err_CCResult_void_CCError(
            CC_ERROR(CC_ERR_INVALID_ARG, "cc_nursery_spawn_closure2"));
    h = (CCClosure2Heap*)malloc(sizeof(CCClosure2Heap));
    if (!h)
        return cc_err_CCResult_void_CCError(
            CC_ERROR(CC_ERR_OUT_OF_MEMORY, "cc_nursery_spawn_closure2"));
    h->c = c;
    h->arg0 = arg0;
    h->arg1 = arg1;
    /* TSan release: sync with acquire in trampoline to make captures visible */
    TSAN_RELEASE(c.env);
    r = cc_nursery_spawn_host(n, cc__closure2_trampoline, h);
    if (!r.ok) free(h);
    return r;
}

CCResult_CCNursery_CCError cc_nursery_spawn_child_closure2(CCNursery parent, CCClosure2 c, intptr_t arg0, intptr_t arg1) {
    CCResult_CCNursery_CCError r;
    if (!c.fn)
        return cc_err_CCResult_CCNursery_CCError(
            CC_ERROR(CC_ERR_INVALID_ARG, "cc_nursery_spawn_child_closure2: null fn"));
    r = cc_nursery_create_child(parent);
    if (!r.ok) return r;
    {
        CCResult_void_CCError s = cc_nursery_spawn_closure2(r.u.value, c, arg0, arg1);
        if (!s.ok) {
            cc_nursery_free(r.u.value);
            return cc_err_CCResult_CCNursery_CCError(s.u.error);
        }
    }
    return r;
}

int cc_run_blocking_closure0(CCClosure0 c) {
    if (!c.fn) return EINVAL;
    CCTask t = cc_thread_spawn_closure0(c);
    if (t.kind == CC_TASK_KIND_INVALID) return ENOMEM;
    (void)cc_block_on_intptr(t);  /* blocks until done and frees task */
    return 0;
}

void* cc_run_blocking_closure0_ptr(CCClosure0 c) {
    if (!c.fn) return NULL;
    CCTask t = cc_thread_spawn_closure0(c);
    if (t.kind == CC_TASK_KIND_INVALID) return NULL;
    return (void*)cc_block_on_intptr(t);  /* blocks until done and frees task */
}

CCTask cc_async_closure0_start(CCAsyncClosure0 c) {
    CCTask out;
    memset(&out, 0, sizeof(out));
    if (!c.start) return out;
    atomic_thread_fence(memory_order_acquire);
    TSAN_ACQUIRE(c.env);
    c.start(c.env, &out);
    if (out.kind == CC_TASK_KIND_INVALID && c.drop) c.drop(c.env);
    return out;
}

CCResult_void_CCError cc_nursery_spawn_async_closure0_host(CCNurseryHost* n, CCAsyncClosure0 c) {
    if (!n || !c.start)
        return cc_err_CCResult_void_CCError(
            CC_ERROR(CC_ERR_INVALID_ARG, "cc_nursery_spawn_async_closure0"));
    /* V2 is the default; use the V2 async-task start so non-fiber tasks get
     * bridged through the V2 scheduler. */
    return cc_nursery_spawn_async_named_host(n, cc_async_closure0_start_v2(c),
                                            NULL, NULL, 0);
}

void* cc_closure0_call(CCClosure0 c) {
    if (!c.fn) return NULL;
    void* r = c.fn(c.env);
    if (c.drop) c.drop(c.env);
    return r;
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

