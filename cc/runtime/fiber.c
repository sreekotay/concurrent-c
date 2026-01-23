/*
 * Fiber implementation - cross-platform stackful coroutines
 * Uses ucontext where available, fallback otherwise
 */

#include "fiber_internal.h"
#include <stdlib.h>
#include <string.h>

static _Thread_local cc__fiber* current_fiber = NULL;

#ifdef _WIN32
/* ============================================================================
 * Windows Implementation - Fiber API
 * ============================================================================ */

static _Thread_local LPVOID main_fiber = NULL;

static void CALLBACK fiber_entry_win(void* param) {
    cc__fiber* f = (cc__fiber*)param;
    current_fiber = f;
    atomic_store_explicit(&f->state, CC__FIBER_RUNNING, memory_order_release);

    if (f->fn) {
        f->result = f->fn(f->arg);
    }

    atomic_store_explicit(&f->state, CC__FIBER_DONE, memory_order_release);

    if (main_fiber) {
        SwitchToFiber(main_fiber);
    }
}

void cc__fiber_thread_init(void) {
    if (!main_fiber) {
        main_fiber = ConvertThreadToFiber(NULL);
    }
}

cc__fiber* cc__fiber_create(cc__fiber_fn fn, void* arg, size_t stack_size) {
    if (stack_size == 0) stack_size = CC__FIBER_STACK_SIZE;

    cc__fiber* f = (cc__fiber*)calloc(1, sizeof(cc__fiber));
    if (!f) return NULL;

    f->fn = fn;
    f->arg = arg;
    f->stack_size = stack_size;
    atomic_store_explicit(&f->state, CC__FIBER_CREATED, memory_order_release);

    f->ctx = CreateFiber(stack_size, fiber_entry_win, f);
    if (!f->ctx) {
        free(f);
        return NULL;
    }

    return f;
}

void cc__fiber_free(cc__fiber* f) {
    if (!f) return;
    if (f->ctx) DeleteFiber(f->ctx);
    free(f);
}

void cc__fiber_switch_to(cc__fiber* f) {
    if (!f || !f->ctx) return;

    current_fiber = f;
    int state = atomic_load_explicit(&f->state, memory_order_acquire);
    if (state == CC__FIBER_CREATED || state == CC__FIBER_READY || state == CC__FIBER_PARKED) {
        atomic_store_explicit(&f->state, CC__FIBER_RUNNING, memory_order_release);
    }

    SwitchToFiber(f->ctx);
}

void cc__fiber_yield(void) {
    cc__fiber* f = current_fiber;
    if (f) atomic_store_explicit(&f->state, CC__FIBER_READY, memory_order_release);
    if (main_fiber) SwitchToFiber(main_fiber);
}

void cc__fiber_park(void) {
    cc__fiber* f = current_fiber;
    if (f) atomic_store_explicit(&f->state, CC__FIBER_PARKED, memory_order_release);
    if (main_fiber) SwitchToFiber(main_fiber);
}

void cc__fiber_exit(void* result) {
    cc__fiber* f = current_fiber;
    if (f) {
        f->result = result;
        atomic_store_explicit(&f->state, CC__FIBER_DONE, memory_order_release);
    }
    if (main_fiber) SwitchToFiber(main_fiber);
}

#elif defined(__aarch64__) || defined(__arm64__)
/* ============================================================================
 * ARM64 Implementation - True context switching via inline assembly
 * ============================================================================ */

#include <sys/mman.h>

/* ARM64 context: callee-saved registers + SP + LR */
typedef struct {
    uint64_t x19, x20, x21, x22, x23, x24, x25, x26, x27, x28;
    uint64_t x29;  /* Frame pointer */
    uint64_t x30;  /* Link register (return address) */
    uint64_t sp;   /* Stack pointer */
    /* SIMD callee-saved: v8-v15 (lower 64 bits = d8-d15) */
    uint64_t d8, d9, d10, d11, d12, d13, d14, d15;
} cc__arm64_context;

static _Thread_local cc__arm64_context scheduler_ctx;

/* Save current context to `from`, load context from `to` */
static inline void cc__arm64_swap_context(cc__arm64_context* from, cc__arm64_context* to) {
    __asm__ volatile (
        /* Save callee-saved GPRs to `from` */
        "stp x19, x20, [%0, #0]\n"
        "stp x21, x22, [%0, #16]\n"
        "stp x23, x24, [%0, #32]\n"
        "stp x25, x26, [%0, #48]\n"
        "stp x27, x28, [%0, #64]\n"
        "stp x29, x30, [%0, #80]\n"
        "mov x9, sp\n"
        "str x9, [%0, #96]\n"
        /* Save SIMD callee-saved (d8-d15) */
        "stp d8, d9, [%0, #104]\n"
        "stp d10, d11, [%0, #120]\n"
        "stp d12, d13, [%0, #136]\n"
        "stp d14, d15, [%0, #152]\n"
        
        /* Load callee-saved GPRs from `to` */
        "ldp x19, x20, [%1, #0]\n"
        "ldp x21, x22, [%1, #16]\n"
        "ldp x23, x24, [%1, #32]\n"
        "ldp x25, x26, [%1, #48]\n"
        "ldp x27, x28, [%1, #64]\n"
        "ldp x29, x30, [%1, #80]\n"
        "ldr x9, [%1, #96]\n"
        "mov sp, x9\n"
        /* Load SIMD callee-saved */
        "ldp d8, d9, [%1, #104]\n"
        "ldp d10, d11, [%1, #120]\n"
        "ldp d12, d13, [%1, #136]\n"
        "ldp d14, d15, [%1, #152]\n"
        :
        : "r"(from), "r"(to)
        : "x9", "memory"
    );
}

/* Entry point for new fibers */
static void cc__arm64_fiber_entry(void) {
    cc__fiber* f = current_fiber;
    if (f && f->fn) {
        f->result = f->fn(f->arg);
    }
    atomic_store_explicit(&f->state, CC__FIBER_DONE, memory_order_release);
    
    /* Return to scheduler */
    cc__arm64_context dummy;
    cc__arm64_swap_context(&dummy, &scheduler_ctx);
}

void cc__fiber_thread_init(void) {
    memset(&scheduler_ctx, 0, sizeof(scheduler_ctx));
}

cc__fiber* cc__fiber_create(cc__fiber_fn fn, void* arg, size_t stack_size) {
    if (stack_size == 0) stack_size = CC__FIBER_STACK_SIZE;

    cc__fiber* f = (cc__fiber*)calloc(1, sizeof(cc__fiber));
    if (!f) return NULL;

    /* Allocate stack with guard page */
    size_t page_size = 4096;
    size_t total_size = stack_size + page_size;
    void* mem = mmap(NULL, total_size, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mem == MAP_FAILED) {
        free(f);
        return NULL;
    }
    
    /* Guard page at bottom of stack */
    mprotect(mem, page_size, PROT_NONE);
    
    f->stack = mem;
    f->stack_size = total_size;
    f->fn = fn;
    f->arg = arg;
    atomic_store_explicit(&f->state, CC__FIBER_CREATED, memory_order_release);
    
    /* Set up initial context */
    cc__arm64_context* ctx = (cc__arm64_context*)calloc(1, sizeof(cc__arm64_context));
    if (!ctx) {
        munmap(mem, total_size);
        free(f);
        return NULL;
    }
    
    /* Stack grows down; start at top, 16-byte aligned */
    uint64_t stack_top = (uint64_t)((char*)mem + total_size);
    stack_top &= ~(uint64_t)15;  /* 16-byte alignment */
    
    ctx->sp = stack_top;
    ctx->x30 = (uint64_t)cc__arm64_fiber_entry;  /* Return address = entry point */
    ctx->x29 = 0;  /* Frame pointer */
    
    f->ctx = ctx;

    return f;
}

void cc__fiber_free(cc__fiber* f) {
    if (!f) return;
    if (f->stack) munmap(f->stack, f->stack_size);
    if (f->ctx) free(f->ctx);
    free(f);
}

void cc__fiber_switch_to(cc__fiber* f) {
    if (!f || !f->ctx) return;

    current_fiber = f;
    int state = atomic_load_explicit(&f->state, memory_order_acquire);
    if (state == CC__FIBER_CREATED || state == CC__FIBER_READY || state == CC__FIBER_PARKED) {
        atomic_store_explicit(&f->state, CC__FIBER_RUNNING, memory_order_release);
    }

    cc__arm64_swap_context(&scheduler_ctx, (cc__arm64_context*)f->ctx);
}

void cc__fiber_yield(void) {
    cc__fiber* f = current_fiber;
    if (!f || !f->ctx) return;
    
    atomic_store_explicit(&f->state, CC__FIBER_READY, memory_order_release);
    cc__arm64_swap_context((cc__arm64_context*)f->ctx, &scheduler_ctx);
}

void cc__fiber_park(void) {
    cc__fiber* f = current_fiber;
    if (!f || !f->ctx) return;
    
    atomic_store_explicit(&f->state, CC__FIBER_PARKED, memory_order_release);
    cc__arm64_swap_context((cc__arm64_context*)f->ctx, &scheduler_ctx);
}

void cc__fiber_exit(void* result) {
    cc__fiber* f = current_fiber;
    if (!f) return;

    f->result = result;
    atomic_store_explicit(&f->state, CC__FIBER_DONE, memory_order_release);
    
    if (f->ctx) {
        cc__arm64_context dummy;
        cc__arm64_swap_context(&dummy, &scheduler_ctx);
    }
}

#else
/* ============================================================================
 * POSIX Implementation - ucontext for Linux, macOS x86_64, etc.
 * ============================================================================ */

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"

#define _XOPEN_SOURCE 700
#include <ucontext.h>

static ucontext_t scheduler_ctx;

static void fiber_entry_posix(intptr_t arg1, intptr_t arg2) {
    cc__fiber* f = (cc__fiber*)arg1;
    (void)arg2;
    current_fiber = f;
    atomic_store_explicit(&f->state, CC__FIBER_RUNNING, memory_order_release);

    if (f && f->fn) {
        f->result = f->fn(f->arg);
    }

    atomic_store_explicit(&f->state, CC__FIBER_DONE, memory_order_release);

    setcontext(&scheduler_ctx);
}

void cc__fiber_thread_init(void) {
    getcontext(&scheduler_ctx);
}

cc__fiber* cc__fiber_create(cc__fiber_fn fn, void* arg, size_t stack_size) {
    if (stack_size == 0) stack_size = CC__FIBER_STACK_SIZE;

    cc__fiber* f = (cc__fiber*)calloc(1, sizeof(cc__fiber));
    if (!f) return NULL;

    f->stack = malloc(stack_size);
    if (!f->stack) {
        free(f);
        return NULL;
    }

    f->fn = fn;
    f->arg = arg;
    f->stack_size = stack_size;
    atomic_store_explicit(&f->state, CC__FIBER_CREATED, memory_order_release);

    /* Allocate ucontext separately */
    ucontext_t* ctx = (ucontext_t*)calloc(1, sizeof(ucontext_t));
    if (!ctx) {
        free(f->stack);
        free(f);
        return NULL;
    }

    if (getcontext(ctx) == -1) {
        free(ctx);
        free(f->stack);
        free(f);
        return NULL;
    }

    ctx->uc_stack.ss_sp = f->stack;
    ctx->uc_stack.ss_size = stack_size;
    ctx->uc_link = &scheduler_ctx;

    makecontext(ctx, (void (*)(void))fiber_entry_posix, 2, (intptr_t)f, 0);
    f->ctx = ctx;

    return f;
}

void cc__fiber_free(cc__fiber* f) {
    if (!f) return;
    if (f->ctx) free(f->ctx);
    if (f->stack) free(f->stack);
    free(f);
}

void cc__fiber_switch_to(cc__fiber* f) {
    if (!f || !f->ctx) return;

    current_fiber = f;
    int state = atomic_load_explicit(&f->state, memory_order_acquire);
    if (state == CC__FIBER_CREATED || state == CC__FIBER_READY || state == CC__FIBER_PARKED) {
        atomic_store_explicit(&f->state, CC__FIBER_RUNNING, memory_order_release);
    }

    swapcontext(&scheduler_ctx, (ucontext_t*)f->ctx);
}

void cc__fiber_yield(void) {
    cc__fiber* f = current_fiber;
    if (!f || !f->ctx) return;

    atomic_store_explicit(&f->state, CC__FIBER_READY, memory_order_release);
    swapcontext((ucontext_t*)f->ctx, &scheduler_ctx);
}

void cc__fiber_park(void) {
    cc__fiber* f = current_fiber;
    if (!f || !f->ctx) return;

    atomic_store_explicit(&f->state, CC__FIBER_PARKED, memory_order_release);
    swapcontext((ucontext_t*)f->ctx, &scheduler_ctx);
}

void cc__fiber_exit(void* result) {
    cc__fiber* f = current_fiber;
    if (!f) return;

    f->result = result;
    atomic_store_explicit(&f->state, CC__FIBER_DONE, memory_order_release);

    setcontext(&scheduler_ctx);
}

#pragma GCC diagnostic pop

#endif /* Platform selection */

cc__fiber* cc__fiber_current(void) {
    return current_fiber;
}

int cc__fiber_in_context(void) {
    return current_fiber != NULL;
}

void cc__fiber_unpark(cc__fiber* f) {
    if (!f) return;
    
    int state = atomic_load_explicit(&f->state, memory_order_acquire);
    if (state == CC__FIBER_PARKED) {
        /* CAS to ensure only one thread unparks */
        if (atomic_compare_exchange_strong_explicit(&f->state, &state, CC__FIBER_READY,
                                                      memory_order_release,
                                                      memory_order_acquire)) {
            /* Successfully unparked - enqueue to scheduler */
            cc__fiber_sched_enqueue(f);
        }
    }
}
