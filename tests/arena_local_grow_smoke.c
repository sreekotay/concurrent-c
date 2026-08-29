/* Exclusive-owner local_grow crosses slabs without the shared alloc path. */
#include <ccc/std/prelude.cch>
#include <stdio.h>
#include <string.h>

int main(void) {
    CCArena a = cc_arena_heap(256);
    size_t i;
    size_t n = 0;
    unsigned char *last = NULL;

    if (!a.base) {
        printf("FAIL: heap\n");
        return 1;
    }
    a.a->block_max = 4;

    for (i = 0; i < 64; i++) {
        unsigned char *p = (unsigned char *)cc_arena_alloc_local_grow(a, 64, 8);
        if (!p) {
            printf("FAIL: local_grow alloc %zu\n", i);
            return 1;
        }
        p[0] = (unsigned char)i;
        last = p;
        n++;
    }
    {
        unsigned blocks = (unsigned)a.a->block_idx;
        if (blocks < 1) {
            printf("FAIL: expected slab growth (block_idx=%u)\n", blocks);
            return 1;
        }
        {
            void *grown = cc_arena_realloc_local_grow(&a, last, 64, 200, 8);
            if (!grown) {
                printf("FAIL: realloc_local_grow\n");
                return 1;
            }
            if (((unsigned char *)grown)[0] != (unsigned char)(n - 1)) {
                printf("FAIL: realloc_local_grow lost prefix\n");
                return 1;
            }
        }
        cc_arena_free(&a);
        printf("arena_local_grow_smoke OK n=%zu blocks=%u\n", n, blocks);
    }
    return 0;
}
