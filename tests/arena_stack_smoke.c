#include <ccc/std/prelude.cch>
#include <stdio.h>
#include <string.h>

int main(void) {
    // --- cc_arena_stack: declares storage + arena, stack-first growable ---
    {
        cc_arena_stack(a, 64);
        uint8_t *stack_buf = a.a->base;
        if (!stack_buf) {
            printf("FAIL: cc_arena_stack init\n");
            return 1;
        }
        if (a.a->block_max != CC_ARENA_DEFAULT_BLOCK_MAX) {
            printf("FAIL: expected default block_max=%u\n", CC_ARENA_DEFAULT_BLOCK_MAX);
            return 1;
        }
        if (!(a.a->_flags & CC_ARENA_FLAG_ALLOW_HEAP_OVERFLOW)) {
            printf("FAIL: stack should allow overflow by default\n");
            return 1;
        }
        if (a.a->_flags & CC_ARENA_FLAG_HEAP_OWNED) {
            printf("FAIL: first slab must not be heap-owned\n");
            return 1;
        }

        void *p = cc_arena_alloc(a, 48, 8);
        if (!p || a.a->base != stack_buf) {
            printf("FAIL: first alloc should stay on stack slab\n");
            return 1;
        }

        void *q = cc_arena_alloc(a, 64, 8);
        if (!q || a.a->block_idx == 0) {
            printf("FAIL: should overflow to heap\n");
            return 1;
        }

        cc_arena_reset(a);
        if (a.a->base != stack_buf || a.a->block_idx != 0 || a.a->prev != NULL) {
            printf("FAIL: reset must restore stack slab\n");
            return 1;
        }
        if (cc_atomic_load(&a.a->offset) != 0) {
            printf("FAIL: offset after reset\n");
            return 1;
        }

        cc_arena_free(&a);
        if (a.base != NULL) {
            printf("FAIL: free clears arena handle\n");
            return 1;
        }
        printf("  cc_arena_stack: stack slab, heap overflow, reset, free OK\n");
    }

    // --- cc_arena_stack + block_max=1, overflow off: stack-only ---
    {
        cc_arena_stack(fix, 96);
        fix.a->block_max = 1;
        if (!cc_arena_set_heap_overflow(fix, false)) {
            printf("FAIL: disable fixed-stack overflow\n");
            return 2;
        }
        if (!fix.base || fix.a->block_max != 1) {
            printf("FAIL: stack fixed init\n");
            return 2;
        }
        if (!cc_arena_alloc(fix, 16, 8)) {
            printf("FAIL: first alloc fixed stack\n");
            return 2;
        }
        if (cc_arena_alloc(fix, 96, 8) != NULL) {
            printf("FAIL: block_max=1 must not grow to heap\n");
            return 2;
        }
        printf("  cc_arena_stack + block_max=1 OK\n");
    }

    // --- cc_arena_stack + block_max=2, overflow off: 2 slabs then NULL ---
    {
        cc_arena_stack(b, 96);
        b.a->block_max = 2;
        if (!cc_arena_set_heap_overflow(b, false)) {
            printf("FAIL: disable stack overflow\n");
            return 3;
        }
        if (!b.base || b.a->block_max != 2) {
            printf("FAIL: stack two-block init\n");
            return 3;
        }
        if (!cc_arena_alloc(b, 16, 8)) {
            printf("FAIL: fill first slab\n");
            return 3;
        }
        if (!cc_arena_alloc(b, 96, 8)) {
            printf("FAIL: grow to second slab\n");
            return 3;
        }
        if (b.a->block_idx != 1) {
            printf("FAIL: expected block_idx 1\n");
            return 3;
        }
        while (cc_arena_alloc(b, 256, 8)) {
        }
        while (cc_arena_alloc(b, 1, 1)) {
        }
        if (cc_arena_alloc(b, 64, 8) != NULL) {
            printf("FAIL: third slab should be blocked by budget\n");
            return 3;
        }
        cc_arena_reset(b);
        cc_arena_free(&b);
        printf("  cc_arena_stack + block_max=2 OK\n");
    }

    // --- cc_arena_would_fit + cc_arena_alloc_local_grow on stack arena ---
    {
        cc_arena_stack(c, 64);
        if (!cc_arena_would_fit(c.a, 32, 8)) {
            printf("FAIL: would_fit empty slab\n");
            return 4;
        }
        if (!cc_arena_alloc_local(c, 40, 8)) {
            printf("FAIL: local fill\n");
            return 4;
        }
        if (cc_arena_would_fit(c.a, 32, 8)) {
            printf("FAIL: would_fit should fail when slab tight\n");
            return 4;
        }
        if (!cc_arena_alloc_local_grow(c, 32, 8)) {
            printf("FAIL: local_grow spill\n");
            return 4;
        }
        cc_arena_free(&c);
        printf("  would_fit + local_grow on cc_arena_stack OK\n");
    }

    printf("arena_stack_smoke ok\n");
    return 0;
}
