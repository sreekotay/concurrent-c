/*
 * cc_closure_helper.h - Closure/spawn lowering helpers
 *
 * This header provides TSan synchronization macros and spawn thunks
 * used by codegen for closure captures and spawn patterns.
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
    cap->__result = (intptr_t)(a->fn ? a->fn(a->arg) : 0);
    free(a);
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
