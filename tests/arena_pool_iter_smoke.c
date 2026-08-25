/* Smoke test for the pool-element iteration API added to cc_arena.cch:
 *
 *   CCArenaPoolIter cc_arena_pool_iter(CCArenaPool* p);      // begin
 *   void*           cc_arena_pool_iter_next(CCArenaPoolIter* it); // -> next / NULL
 *
 * These walk every element a CCArenaPool has bump-allocated, across the arena's
 * slab chain (root block newest, `prev` older).  Contract (per the header): valid
 * only when there have been NO interleaved individual frees and the arena holds
 * ONLY this pool's elements — the build-once / iterate pattern used here.
 *
 * Two properties are pinned:
 *   1. Single fixed slab: N elements allocated into an arena large enough to
 *      never grow => iteration yields exactly N elements in EXACT allocation
 *      order (we store `i` into each and expect them back 0,1,2,...,N-1).
 *   2. Growable arena (small initial slab): enough elements to force several
 *      slabs linked by `prev` => iteration count == number allocated, exercising
 *      the prev-chain crossing.
 */

#include <ccc/std/prelude.cch>
#include <stdio.h>
#include <string.h>
#include <stdint.h>

/* sizeof == 16 (multiple of 8) so pool stride == elem_size, tight packing. */
typedef struct Elem {
    long v;
    long pad;
} Elem;

int main(void) {
    /* --- Property 1: single fixed slab, exact allocation order --------------- */
    {
        const long N = 100;

        /* Arena large enough to hold all N elements without ever growing, so the
         * whole pool lives in one slab and iteration order == allocation order. */
        CCArena a = cc_arena_create(64 * 1024);
        if (!a.base) { printf("FAIL: cc_arena_create returned empty\n"); return 1; }

        CCArenaPool pool;
        cc_arena_pool_init(&pool, &a, sizeof(Elem));

        for (long i = 0; i < N; i++) {
            Elem *e = (Elem*)cc_arena_pool_alloc(&pool);
            if (!e) { printf("FAIL: pool_alloc %ld returned NULL\n", i); return 1; }
            e->v = i;
            e->pad = -1;
        }

        /* Single slab => no growth. */
        if (a.a->block_idx != 0 || a.a->prev != NULL) {
            printf("FAIL: fixed-slab arena unexpectedly grew (block_idx=%u, prev=%p)\n",
                   a.a->block_idx, (void*)a.a->prev);
            return 1;
        }

        long count = 0;
        CCArenaPoolIter it = cc_arena_pool_iter(&pool);
        for (Elem *e; (e = (Elem*)cc_arena_pool_iter_next(&it)) != NULL; ) {
            if (e->v != count) {
                printf("FAIL: order mismatch at %ld: got v=%ld\n", count, e->v);
                return 1;
            }
            count++;
        }
        if (count != N) {
            printf("FAIL: single-slab count %ld != %ld\n", count, N);
            return 1;
        }

        cc_arena_free(&a);
        printf("  single fixed slab: %ld elements, exact allocation order OK\n", N);
    }

    /* --- Property 2: growable arena crossing the prev-chain ------------------- */
    {
        const long N = 5000;

        /* Small initial slab => 5000 * 16B = ~80KB forces many slabs. */
        CCArena a = cc_arena_heap(4096);
        if (!a.base) { printf("FAIL: cc_arena_heap returned empty\n"); return 2; }

        CCArenaPool pool;
        cc_arena_pool_init(&pool, &a, sizeof(Elem));

        for (long i = 0; i < N; i++) {
            Elem *e = (Elem*)cc_arena_pool_alloc(&pool);
            if (!e) { printf("FAIL: growable pool_alloc %ld returned NULL\n", i); return 2; }
            e->v = i;
            e->pad = 0;
        }

        /* Must have grown to more than one slab to actually exercise `prev`. */
        if (a.a->prev == NULL || a.a->block_idx == 0) {
            printf("FAIL: growable arena did not span multiple slabs (block_idx=%u)\n",
                   a.a->block_idx);
            return 2;
        }

        long count = 0;
        CCArenaPoolIter it = cc_arena_pool_iter(&pool);
        for (Elem *e; (e = (Elem*)cc_arena_pool_iter_next(&it)) != NULL; ) {
            count++;
        }
        if (count != N) {
            printf("FAIL: multi-slab count %ld != %ld\n", count, N);
            return 2;
        }

        uint32_t slabs = a.a->block_idx + 1;
        cc_arena_free(&a);
        printf("  growable arena: %ld elements across %u slabs OK\n", N, slabs);
    }

    printf("arena_pool_iter_smoke OK\n");
    return 0;
}
