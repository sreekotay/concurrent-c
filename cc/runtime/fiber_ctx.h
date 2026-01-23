/*
 * Minimal fiber context switching.
 * 
 * This implements the bare minimum needed for M:N scheduling:
 * - fiber_ctx_init: Set up a fiber's initial context
 * - fiber_ctx_switch: Switch from one fiber to another
 *
 * Supported platforms:
 * - x86_64 (macOS, Linux)
 * - ARM64 (macOS Apple Silicon, Linux)
 */

#ifndef CC_FIBER_CTX_H
#define CC_FIBER_CTX_H

#include <stddef.h>
#include <stdint.h>

/*
 * Fiber context - stores saved registers.
 * Layout must match assembly in fiber_ctx_switch.
 */
typedef struct fiber_ctx {
#if defined(__x86_64__) || defined(_M_X64)
    /* x86_64: callee-saved registers + rip + rsp */
    void* rip;  /* Return address */
    void* rsp;  /* Stack pointer */
    void* rbx;
    void* rbp;
    void* r12;
    void* r13;
    void* r14;
    void* r15;
#elif defined(__aarch64__) || defined(__arm64__)
    /* ARM64: callee-saved registers + lr + sp */
    void* lr;   /* Link register (return address) */
    void* sp;   /* Stack pointer */
    void* x19;
    void* x20;
    void* x21;
    void* x22;
    void* x23;
    void* x24;
    void* x25;
    void* x26;
    void* x27;
    void* x28;
    void* x29;  /* Frame pointer */
    /* Note: SIMD registers d8-d15 should be saved if using floating point */
#else
    #error "Unsupported platform for fiber context switching"
#endif
} fiber_ctx;

/*
 * Initialize a fiber context to start at `entry(arg)` with the given stack.
 * Stack should be pre-allocated and stack_size should be the size in bytes.
 * The stack grows downward.
 */
void fiber_ctx_init(fiber_ctx* ctx, void* stack, size_t stack_size,
                    void (*entry)(void*), void* arg);

/*
 * Switch from `from` context to `to` context.
 * Saves current state into `from`, loads state from `to`.
 * When `to` was previously switched away from, this returns there.
 * When `to` is freshly initialized, this calls its entry function.
 */
void fiber_ctx_switch(fiber_ctx* from, fiber_ctx* to);

#endif /* CC_FIBER_CTX_H */
