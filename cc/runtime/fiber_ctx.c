/*
 * Minimal fiber context switching implementation.
 * 
 * Uses inline assembly for x86_64 and ARM64.
 */

#include "fiber_ctx.h"
#include <string.h>

/* Entry trampoline - called when a fiber starts */
static void fiber_entry_trampoline(void);

#if defined(__x86_64__) || defined(_M_X64)
/* ============================================================================
 * x86_64 Implementation
 * ============================================================================ */

/*
 * fiber_ctx_switch - Switch contexts (x86_64)
 * 
 * Saves: rbx, rbp, r12-r15, rsp, rip (via return address)
 * Arguments: rdi = from, rsi = to
 */
__attribute__((naked))
void fiber_ctx_switch(fiber_ctx* from, fiber_ctx* to) {
    __asm__ volatile (
        /* Save callee-saved registers to `from` context */
        "movq %%rbx,  16(%rdi)\n"   /* from->rbx */
        "movq %%rbp,  24(%rdi)\n"   /* from->rbp */
        "movq %%r12,  32(%rdi)\n"   /* from->r12 */
        "movq %%r13,  40(%rdi)\n"   /* from->r13 */
        "movq %%r14,  48(%rdi)\n"   /* from->r14 */
        "movq %%r15,  56(%rdi)\n"   /* from->r15 */
        
        /* Save stack pointer and return address */
        "movq %%rsp,   8(%rdi)\n"   /* from->rsp */
        "movq (%%rsp), %%rax\n"     /* Get return address from stack */
        "movq %%rax,   0(%rdi)\n"   /* from->rip */
        
        /* Load callee-saved registers from `to` context */
        "movq 16(%rsi), %%rbx\n"    /* to->rbx */
        "movq 24(%rsi), %%rbp\n"    /* to->rbp */
        "movq 32(%rsi), %%r12\n"    /* to->r12 */
        "movq 40(%rsi), %%r13\n"    /* to->r13 */
        "movq 48(%rsi), %%r14\n"    /* to->r14 */
        "movq 56(%rsi), %%r15\n"    /* to->r15 */
        
        /* Load stack pointer and jump to saved address */
        "movq  8(%rsi), %%rsp\n"    /* to->rsp */
        "jmpq *0(%rsi)\n"           /* Jump to to->rip */
    );
}

void fiber_ctx_init(fiber_ctx* ctx, void* stack, size_t stack_size,
                    void (*entry)(void*), void* arg) {
    memset(ctx, 0, sizeof(*ctx));
    
    /* Stack grows down - start at top, aligned to 16 bytes */
    uintptr_t sp = (uintptr_t)stack + stack_size;
    sp = sp & ~15ULL;  /* 16-byte align */
    
    /* Set up stack for entry:
     * We need to push the argument and set up for the trampoline.
     * The trampoline will pop the entry function and arg, then call entry(arg).
     */
    sp -= 8;  /* Alignment padding */
    sp -= 8; *(void**)sp = arg;    /* Argument */
    sp -= 8; *(void**)sp = entry;  /* Entry function pointer */
    sp -= 8; *(void**)sp = NULL;   /* Fake return address (fiber never returns) */
    
    ctx->rsp = (void*)sp;
    ctx->rip = (void*)fiber_entry_trampoline;
}

/* Trampoline: pops entry and arg from stack, calls entry(arg) */
__attribute__((naked))
static void fiber_entry_trampoline(void) {
    __asm__ volatile (
        "popq %%rax\n"              /* Pop entry function */
        "popq %%rdi\n"              /* Pop arg into first argument register */
        "addq $8, %%rsp\n"          /* Skip alignment padding */
        "callq *%%rax\n"            /* Call entry(arg) */
        /* If entry returns, we have a problem - just spin */
        "1: jmp 1b\n"
    );
}

#elif defined(__aarch64__) || defined(__arm64__)
/* ============================================================================
 * ARM64 Implementation
 * ============================================================================ */

/*
 * fiber_ctx_switch - Switch contexts (ARM64)
 * 
 * Saves: x19-x29, lr, sp
 * Arguments: x0 = from, x1 = to
 */
__attribute__((naked))
void fiber_ctx_switch(fiber_ctx* from, fiber_ctx* to) {
    __asm__ volatile (
        /* Save callee-saved registers to `from` context */
        "mov x2, sp\n"
        "stp lr,  x2,  [x0, #0]\n"   /* from->lr, from->sp */
        "stp x19, x20, [x0, #16]\n"
        "stp x21, x22, [x0, #32]\n"
        "stp x23, x24, [x0, #48]\n"
        "stp x25, x26, [x0, #64]\n"
        "stp x27, x28, [x0, #80]\n"
        "str x29,      [x0, #96]\n"  /* from->x29 (fp) */
        
        /* Load callee-saved registers from `to` context */
        "ldp lr,  x2,  [x1, #0]\n"   /* to->lr, to->sp */
        "mov sp, x2\n"
        "ldp x19, x20, [x1, #16]\n"
        "ldp x21, x22, [x1, #32]\n"
        "ldp x23, x24, [x1, #48]\n"
        "ldp x25, x26, [x1, #64]\n"
        "ldp x27, x28, [x1, #80]\n"
        "ldr x29,      [x1, #96]\n"  /* to->x29 (fp) */
        
        "ret\n"                       /* Return to lr (to->lr) */
    );
}

void fiber_ctx_init(fiber_ctx* ctx, void* stack, size_t stack_size,
                    void (*entry)(void*), void* arg) {
    memset(ctx, 0, sizeof(*ctx));
    
    /* Stack grows down - start at top, aligned to 16 bytes */
    uintptr_t sp = (uintptr_t)stack + stack_size;
    sp = sp & ~15ULL;  /* 16-byte align */
    
    /* Reserve space for entry args (stored in callee-saved regs) */
    ctx->sp = (void*)sp;
    ctx->lr = (void*)fiber_entry_trampoline;
    ctx->x19 = (void*)entry;  /* Store entry in callee-saved register */
    ctx->x20 = arg;           /* Store arg in callee-saved register */
}

/* Trampoline: entry is in x19, arg is in x20 */
__attribute__((naked))
static void fiber_entry_trampoline(void) {
    __asm__ volatile (
        "mov x0, x20\n"             /* Move arg to first argument register */
        "blr x19\n"                 /* Call entry(arg) */
        /* If entry returns, spin */
        "1: b 1b\n"
    );
}

#endif
