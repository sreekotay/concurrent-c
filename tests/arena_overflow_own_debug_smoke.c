#include <ccc/std/prelude.cch>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Overflow ownership is stamped in the block header: foreign / wrong-arena
 * overflow release and realloc are refused instead of calling free/realloc. */
int main(void) {
    _Alignas(CCArenaHost) uint8_t buf[CC_ARENA_REGION_BYTES(64)];
    CCArena a = cc_arena_create_buffer(buf, sizeof(buf), CC_ARENA_FIXED);
    if (!a.base) return 1;
    if (!cc_arena_set_heap_overflow(a, true)) return 2;

    void *spill = cc_arena_alloc(a, 128, 8);
    if (!spill) {
        printf("FAIL: overflow alloc\n");
        return 3;
    }
    if (cc__arena_find_slab(a, spill)) {
        printf("FAIL: expected heap overflow, got slab\n");
        return 4;
    }

    void *bigger = cc_arena_realloc(a, a, spill, 128, 256, 8);
    if (!bigger) {
        printf("FAIL: overflow realloc of owned ptr\n");
        return 5;
    }
    spill = bigger;

    void *foreign = malloc(24);
    if (!foreign) return 6;
    if (cc_arena_release(a, foreign)) {
        printf("FAIL: foreign overflow release should be refused\n");
        free(foreign);
        return 7;
    }
    free(foreign);

    CCArena b = cc_arena_heap(256);
    if (!b.base) return 8;
    (void)cc_arena_set_heap_overflow(b, true);
    if (cc_arena_release(b, spill)) {
        printf("FAIL: wrong-arena overflow release should be refused\n");
        return 9;
    }
    if (cc_arena_realloc(b, b, spill, 256, 512, 8) != NULL) {
        printf("FAIL: wrong-arena overflow realloc should be refused\n");
        return 10;
    }

    if (!cc_arena_release(a, spill)) {
        printf("FAIL: owned overflow release\n");
        return 11;
    }

    cc_arena_free(&b);
    cc_arena_free(&a);
    puts("arena_overflow_own_debug_smoke: PASS");
    return 0;
}
