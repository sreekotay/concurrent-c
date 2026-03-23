/*
 * cc_closure_helper.h - Generated lowering helpers
 *
 * This header provides generated-C helpers shared by closure lowering,
 * spawn patterns, and other synthesized codegen utilities.
 */
#ifndef CC_CLOSURE_HELPER_H
#define CC_CLOSURE_HELPER_H

#include <stdlib.h>
#include <stdint.h>

/* --- Closure declaration/definition macros --- */

/* Forward-declare a CCClosure0 entry and make function */
#define CC_CLOSURE0_DECL(n) \
    static void* __cc_closure_entry_##n(void*); \
    static CCClosure0 __cc_closure_make_##n(void)

/* Define a simple CCClosure0 with no captures - use with { body; return NULL; } */
#define CC_CLOSURE0_SIMPLE(n) \
    static CCClosure0 __cc_closure_make_##n(void) { \
        return cc_closure0_make(__cc_closure_entry_##n, NULL, NULL); \
    } \
    static void* __cc_closure_entry_##n(void* __p)

/* --- Generated lowering helpers --- */
#define CC_CLOSURE_ENV_ALLOC(env_ty, var) \
    env_ty* var = (env_ty*)malloc(sizeof(env_ty)); \
    if (!(var)) abort()

#define CC_CLOSURE_ENV_NURSERY_ALLOC(nursery, env_ty, var) \
    env_ty* var = (env_ty*)cc_nursery_closure_env_alloc((nursery), sizeof(env_ty), _Alignof(env_ty)); \
    if (!(var)) abort()

#define CC_TASK_RESULT_PTR_OR_RETURN(type, var) \
    type* var = (type*)cc_task_result_ptr(sizeof(type)); \
    if (!(var)) return NULL

#define CC_SEND_TASK_OR_JOIN(tx_expr, task_var) \
    do { \
        int __cc_send_err = cc_chan_send((tx_expr).raw, &(task_var), sizeof(task_var)); \
        if (__cc_send_err != 0) { \
            (void)cc_block_on_intptr(task_var); \
        } \
    } while (0)

#define __cc_ret(id, value) \
    do { \
        __cc_retval_##id = (value); \
        __cc_ret_set_##id = 1; \
        goto __cc_cleanup_##id; \
    } while (0)

#define __cc_ret_ok(id, value) \
    do { \
        cc_ok_into(__cc_retval_##id, (value)); \
        __cc_ret_set_##id = 1; \
        goto __cc_cleanup_##id; \
    } while (0)

#define __cc_ret_err(id, err) \
    do { \
        cc_err_into(__cc_retval_##id, (err)); \
        __cc_ret_set_##id = 1; \
        goto __cc_cleanup_##id; \
    } while (0)

/* --- TSan synchronization for closure captures --- */
#if defined(__SANITIZE_THREAD__) || (defined(__has_feature) && __has_feature(thread_sanitizer))
extern void __tsan_release(void* addr);
extern void __tsan_acquire(void* addr);
#define CC_TSAN_RELEASE(addr) do { if (addr) __tsan_release(addr); } while(0)
#define CC_TSAN_ACQUIRE(addr) do { if (addr) __tsan_acquire(addr); } while(0)
#else
#define CC_TSAN_RELEASE(addr) ((void)0)
#define CC_TSAN_ACQUIRE(addr) ((void)0)
#endif

/* --- Basic spawn thunks --- */
typedef struct { void (*fn)(void); } __cc_spawn_void_arg;
static inline void* __cc_spawn_thunk_void(void* p) {
    __cc_spawn_void_arg* a = (__cc_spawn_void_arg*)p;
    if (a && a->fn) a->fn();
    free(a);
    return NULL;
}

typedef struct { void (*fn)(int); int arg; } __cc_spawn_int_arg;
static inline void* __cc_spawn_thunk_int(void* p) {
    __cc_spawn_int_arg* a = (__cc_spawn_int_arg*)p;
    if (a && a->fn) a->fn(a->arg);
    free(a);
    return NULL;
}

/* --- Ordered channel spawn helpers (spawn into pattern) --- */
typedef struct { void* (*fn)(void*); void* arg; } __spawn_into_arg;
static inline void* __spawn_into_thunk(void* p) {
    __spawn_into_arg* a = (__spawn_into_arg*)p;
    typedef struct { intptr_t __result; } __caps_t;
    __caps_t* cap = (__caps_t*)cc_task_result_ptr(sizeof(__caps_t));
    void* fn_result = a->fn ? a->fn(a->arg) : NULL;
    free(a);
    /* If the called function already stored its structured result in the same
     * cc_task_result_ptr buffer (evidenced by returning that same pointer),
     * do not overwrite - preserves the caller's layout (e.g. Result at offset 0). */
    if (fn_result != (void*)cap) {
        if (cap) cap->__result = (intptr_t)fn_result;
    }
    return cap;
}

static inline CCTask __spawn_into_call(void* (*fn)(void*), void* arg) {
    __spawn_into_arg* a = (__spawn_into_arg*)malloc(sizeof(__spawn_into_arg));
    if (!a) abort();
    a->fn = fn;
    a->arg = arg;
    return cc_fiber_spawn_task(__spawn_into_thunk, a);
}

#endif /* CC_CLOSURE_HELPER_H */
