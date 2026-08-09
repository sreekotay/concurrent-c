/* Slab realloc at the bump tip extends/shrinks in place (no new pointer). */
#include <ccc/std/prelude.cch>
#include <stdio.h>
#include <string.h>

int main(void) {
    CCArena a = cc_arena_heap(kilobytes(4));
    unsigned char *p;
    void *grown;
    void *shrunk;
    size_t off_after_first;
    if (!a.base) {
        printf("FAIL: heap\n");
        return 1;
    }

    p = (unsigned char *)cc_arena_alloc(&a, 64, 8);
    if (!p) {
        printf("FAIL: alloc\n");
        return 1;
    }
    memset(p, 0xab, 64);
    off_after_first = cc_atomic_load(&a.offset);

    grown = cc_arena_realloc(&a, &a, p, 64, 128, 8);
    if (grown != p) {
        printf("FAIL: tip grow should be in-place (got %p want %p)\n", grown, (void *)p);
        return 1;
    }
    if (cc_atomic_load(&a.offset) != off_after_first + 64) {
        printf("FAIL: tip grow offset\n");
        return 1;
    }
    {
        size_t i;
        for (i = 0; i < 64; i++) {
            if (p[i] != 0xab) {
                printf("FAIL: tip grow preserved prefix\n");
                return 1;
            }
        }
    }

    shrunk = cc_arena_realloc(&a, &a, p, 128, 32, 8);
    if (shrunk != p) {
        printf("FAIL: tip shrink should be in-place\n");
        return 1;
    }
    if (cc_atomic_load(&a.offset) != (size_t)((unsigned char *)p - a.base) + 32) {
        printf("FAIL: tip shrink offset\n");
        return 1;
    }

    /* Non-tip: allocate past p, then realloc p — must move. */
    {
        void *hole = cc_arena_alloc(&a, 16, 8);
        void *moved;
        if (!hole) {
            printf("FAIL: hole alloc\n");
            return 1;
        }
        moved = cc_arena_realloc(&a, &a, p, 32, 64, 8);
        if (moved == p) {
            printf("FAIL: non-tip realloc should copy\n");
            return 1;
        }
        if (!moved) {
            printf("FAIL: non-tip realloc\n");
            return 1;
        }
    }

    cc_arena_free(&a);
    printf("arena_realloc_tip_smoke OK\n");
    return 0;
}
