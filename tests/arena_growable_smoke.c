#include <ccc/std/prelude.cch>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

int main(void) {
    // --- Test 1: Basic growth ---
    // Start with a tiny arena (64 bytes) that must grow to hold 4KB of data.
    {
        CCArena a = cc_arena_heap(64);
        if (!a.base) { printf("FAIL: heap alloc\n"); return 1; }
        if (a.a->block_max != CC_ARENA_DEFAULT_BLOCK_MAX) {
            printf("FAIL: expected default block_max=%u\n", CC_ARENA_DEFAULT_BLOCK_MAX);
            return 1;
        }
        if (a.a->slab->block_idx != 0) { printf("FAIL: expected block_idx=0\n"); return 1; }

        // Allocate enough to force several growths / overflow past the budget
        for (int i = 0; i < 100; i++) {
            int *p = cc_arena_alloc_T_count(int, a, 10);  // 40 bytes per alloc
            if (!p) { printf("FAIL: growth alloc at i=%d\n", i); return 1; }
            for (int j = 0; j < 10; j++) p[j] = i * 10 + j;
        }

        if (a.a->slab->block_idx == 0 && !(a.a->_flags & CC_ARENA_FLAG_USED_HEAP_OVERFLOW)) {
            printf("FAIL: expected growth or overflow\n");
            return 1;
        }
        printf("  growth: block_idx=%d overflow=%d OK\n", a.a->slab->block_idx,
               (a.a->_flags & CC_ARENA_FLAG_USED_HEAP_OVERFLOW) ? 1 : 0);
        cc_arena_free(&a);
    }

    // --- Test 2: Reset unwinds growth ---
    {
        CCArena a = cc_arena_heap(64);
        if (!a.base) return 2;

        for (int i = 0; i < 50; i++) {
            cc_arena_alloc_T_count(int, a, 10);
        }
        int saved_idx = a.a->slab->block_idx;
        if (saved_idx == 0) { printf("FAIL: no growth before reset\n"); return 2; }

        cc_arena_reset(a);
        if (a.a->slab->block_idx != 0) { printf("FAIL: block_idx not 0 after reset\n"); return 2; }
        if (a.a->slab->prev != NULL) { printf("FAIL: prev not NULL after reset\n"); return 2; }
        if (cc_arena_slab_offset(a.a) != 0) { printf("FAIL: offset not 0 after reset\n"); return 2; }
        printf("  reset: unwound from block_idx=%d OK\n", saved_idx);
        cc_arena_free(&a);
    }

    // --- Test 3: Checkpoint/restore across blocks ---
    {
        CCArena a = cc_arena_heap(64);
        if (!a.base) return 3;

        // Allocate in block 0
        int *p0 = cc_arena_alloc_T_count(int, a, 4);
        if (!p0) return 3;
        for (int i = 0; i < 4; i++) p0[i] = i;

        // Take checkpoint in block 0: a mark; the first scratch that does
        // not fit the 64-byte L1 promotes it to a child
        CCArenaCheckpoint cp = cc_arena_checkpoint(a);
        if (!cp.arena || cp.parent != a.a) { printf("FAIL: checkpoint mark\n"); return 3; }

        // Force growth: the scratch child (4 KiB L1) grows its own extents
        for (int i = 0; i < 400; i++) {
            if (!cc_arena_alloc_T_count(int, a, 10)) { printf("FAIL: scratch alloc\n"); return 3; }
        }
        if (!a.a->active) { printf("FAIL: scratch past the tail must promote the mark\n"); return 3; }
        int grown_idx = a.a->active->slab->block_idx;
        if (grown_idx == 0 && !(a.a->active->_flags & CC_ARENA_FLAG_USED_HEAP_OVERFLOW)) {
            printf("FAIL: expected child growth\n"); return 3;
        }
        if (a.a->slab->block_idx != 0) { printf("FAIL: parent must not grow for scratch\n"); return 3; }

        // Restore destroys the child and everything it grew
        if (!cc_arena_restore(cp)) { printf("FAIL: restore\n"); return 3; }
        if (a.a->slab->block_idx != 0) { printf("FAIL: parent block_idx after restore %d\n", a.a->slab->block_idx); return 3; }
        if (cc_arena_slab_live(a.a) != 1) {
            printf("FAIL: restore live_allocs want 1 got %zu\n",
                   (size_t)cc_arena_slab_live(a.a));
            return 3;
        }

        // Data before checkpoint should still be valid
        for (int i = 0; i < 4; i++) {
            if (p0[i] != i) { printf("FAIL: data corrupted after restore\n"); return 3; }
        }
        printf("  checkpoint/restore: child grew to block_idx=%d and died OK\n", grown_idx);
        cc_arena_free(&a);
    }

    // --- Test 4: Budget fail-closed when overflow is disabled ---
    {
        CCArena a = cc_arena_heap(64);
        a.a->block_max = 3;
        if (!cc_arena_set_heap_overflow(a, false)) {
            printf("FAIL: disable overflow\n");
            return 4;
        }
        if (!a.base) return 4;

        int alloc_count = 0;
        while (alloc_count < 10000) {
            int *p = cc_arena_alloc_T_count(int, a, 10);
            if (!p) break;
            alloc_count++;
        }

        if (alloc_count >= 10000) { printf("FAIL: budget not enforced\n"); return 4; }
        if (a.a->slab->block_idx + 1 < a.a->block_max) { printf("FAIL: budget not reached\n"); return 4; }
        if (a.a->_flags & CC_ARENA_FLAG_USED_HEAP_OVERFLOW) {
            printf("FAIL: overflow should stay off\n");
            return 4;
        }
        printf("  budget: exhausted after %d allocs, block_idx=%d/%d OK\n",
               alloc_count, a.a->slab->block_idx, a.a->block_max);
        cc_arena_free(&a);
    }

    // --- Test 4b: default budget then malloc overflow ---
    {
        CCArena a = cc_arena_heap(64);
        int saw_ovf = 0;
        int i;
        if (!a.base || a.a->block_max != CC_ARENA_DEFAULT_BLOCK_MAX) return 41;
        for (i = 0; i < 500; i++) {
            if (!cc_arena_alloc_T_count(int, a, 10)) {
                printf("FAIL: default path should overflow not NULL\n");
                return 41;
            }
            if (a.a->_flags & CC_ARENA_FLAG_USED_HEAP_OVERFLOW) saw_ovf = 1;
        }
        if (!saw_ovf) {
            printf("FAIL: expected tier-3 overflow after slab budget\n");
            return 41;
        }
        if (a.a->slab->block_idx + 1 > CC_ARENA_DEFAULT_BLOCK_MAX) {
            printf("FAIL: grew past default budget (%u)\n", a.a->slab->block_idx);
            return 41;
        }
        printf("  default budget→overflow: block_idx=%d OK\n", a.a->slab->block_idx);
        cc_arena_free(&a);
    }

    // --- Test 5: Fixed arena (block_max=1) never grows ---
    {
        _Alignas(CCArenaHost) uint8_t buf[CC_ARENA_REGION_BYTES(192)];
        CCArena a = cc_arena_wrap_region(buf, sizeof(buf), CC_ARENA_FIXED);
        if (a.a->block_max != 1) { printf("FAIL: buffer should be fixed\n"); return 5; }

        // Fill it up
        int *p = cc_arena_alloc_T_count(int, a, 30);  // 120 bytes
        if (!p) { printf("FAIL: initial alloc\n"); return 5; }

        // This should fail (fixed arena, no growth)
        int *q = cc_arena_alloc_T_count(int, a, 30);
        if (q != NULL) { printf("FAIL: fixed arena should not grow\n"); return 5; }
        printf("  fixed: correctly rejected overflow OK\n");
        // No cc_arena_free needed (user-backed)
    }

    // --- Test 6: Stack (user) first block, overflow to heap ---
    {
        _Alignas(CCArenaHost) uint8_t buf[CC_ARENA_REGION_BYTES(64)];
        CCArena a;
        if (cc_arena_init_region(buf, sizeof(buf), CC_ARENA_FIXED) != 0) {
            printf("FAIL: buffer init\n");
            return 6;
        }
        a = cc_arena_handle((CCArenaHost *)buf);
        uint8_t *l1 = a.a->slab->base;
        a.a->block_max = 0;
        if (a.a->block_max != 0) {
            printf("FAIL: expected unbounded block_max\n");
            return 6;
        }
        if ((a.a->slab->flags & CC_ARENA_SLAB_HEAP_OWNED)) {
            printf("FAIL: initial buffer should not be heap-owned\n");
            return 6;
        }

        void *p = cc_arena_alloc(a, 128, 8);
        if (!p) {
            printf("FAIL: stack-first arena should grow to heap\n");
            return 6;
        }
        if (a.a->slab->block_idx == 0) {
            printf("FAIL: expected growth off stack block\n");
            return 6;
        }
        memset(p, 0xab, 128);

        cc_arena_reset(a);
        if (a.a->slab->base != l1 || a.a->slab->block_idx != 0 || a.a->slab->prev != NULL) {
            printf("FAIL: reset should restore stack block\n");
            return 6;
        }

        void *q = cc_arena_alloc(a, 128, 8);
        if (!q) {
            printf("FAIL: alloc after reset\n");
            return 6;
        }
        cc_arena_free(&a);
        if (a.base != NULL) {
            printf("FAIL: free should clear root\n");
            return 6;
        }
        printf("  stack-first + heap overflow + reset/free OK\n");
    }

    // --- Test 7: release resets current block and heap-overflow setter is explicit ---
    {
        _Alignas(CCArenaHost) uint8_t buf[CC_ARENA_REGION_BYTES(256)];
        CCArena a;
        if (cc_arena_init_region(buf, sizeof(buf), CC_ARENA_FIXED) != 0) {
            printf("FAIL: release test init\n");
            return 7;
        }
        a = cc_arena_handle((CCArenaHost *)buf);
        if (!cc_arena_set_heap_overflow(a, true)) {
            printf("FAIL: enable heap overflow\n");
            return 7;
        }

        void *p = cc_arena_alloc(a, 64, 8);
        if (!p) {
            printf("FAIL: tracked alloc in fixed arena\n");
            return 7;
        }
        if (!cc_arena_release(a, p)) {
            printf("FAIL: release tracked arena ptr\n");
            return 7;
        }
        if (cc_arena_release(a, p) != 0) {
            printf("FAIL: double release should fail\n");
            return 7;
        }
        void *p2 = cc_arena_alloc(a, 64, 8);
        if (!p2 || p2 != p) {
            printf("FAIL: release should make current block reusable\n");
            return 7;
        }
        if (!cc_arena_release(a, p2)) {
            printf("FAIL: release second tracked ptr\n");
            return 7;
        }

        void *spill = cc_arena_alloc(a, 512, 8);
        if (!spill) {
            printf("FAIL: explicit heap overflow fallback\n");
            return 7;
        }
        if (!(a.a->_flags & CC_ARENA_FLAG_USED_HEAP_OVERFLOW)) {
            printf("FAIL: expected used heap overflow flag\n");
            return 7;
        }
        size_t spill_bytes = cc_arena_overflow_raw_bytes(a);
        if (spill_bytes < 512) {
            printf("FAIL: expected overflow byte accounting\n");
            return 7;
        }
        memset(spill, 0x5a, 512);
        void *bigger_spill = cc_arena_realloc(a, a, spill, 512, 1024, 8);
        if (!bigger_spill) {
            printf("FAIL: overflow realloc\n");
            return 7;
        }
        unsigned char *bp = (unsigned char*)bigger_spill;
        for (size_t i = 0; i < 512; ++i) {
            if (bp[i] != 0x5a) {
                printf("FAIL: overflow realloc preserved bytes\n");
                return 7;
            }
        }
        if (cc_arena_overflow_raw_bytes(a) < 1024) {
            printf("FAIL: overflow byte accounting after realloc\n");
            return 7;
        }
        if (!cc_arena_release(a, bigger_spill)) {
            printf("FAIL: release heap overflow ptr\n");
            return 7;
        }
        if (cc_arena_overflow_raw_bytes(a) != 0) {
            printf("FAIL: overflow byte accounting after release\n");
            return 7;
        }
        /* Overflow release never disables a checkpoint. */
        {
            CCArenaCheckpoint cp = cc_arena_checkpoint(a);
            if (cp.arena == NULL) {
                printf("FAIL: checkpoint should work after overflow release\n");
                return 7;
            }
            if (!cc_arena_restore(cp)) {
                printf("FAIL: restore after overflow release\n");
                return 7;
            }
        }

        void *moved_src = cc_arena_alloc(a, 64, 8);
        CCArena moved_dst = cc_arena_heap(1024);
        if (!moved_src || !moved_dst.base) {
            printf("FAIL: cross-arena realloc setup\n");
            return 7;
        }
        memset(moved_src, 0xa5, 64);
        void *moved = cc_arena_realloc(a, moved_dst, moved_src, 64, 96, 8);
        if (!moved) {
            printf("FAIL: cross-arena realloc\n");
            return 7;
        }
        unsigned char *mp = (unsigned char*)moved;
        for (size_t i = 0; i < 64; ++i) {
            if (mp[i] != 0xa5) {
                printf("FAIL: cross-arena realloc preserved bytes\n");
                return 7;
            }
        }
        if (cc__arena_find_slab(a, moved) || !cc__arena_find_slab(moved_dst, moved)) {
            printf("FAIL: cross-arena realloc ownership\n");
            return 7;
        }
        cc_arena_free(&moved_dst);

        void *foreign = malloc(24);
        if (!foreign) {
            printf("FAIL: foreign alloc\n");
            return 7;
        }
        /* Overflow ownership is fail-closed: foreign pointers are refused. */
        if (cc_arena_release(a, foreign)) {
            printf("FAIL: foreign overflow release should be refused\n");
            return 7;
        }
        free(foreign);

        cc_arena_reset(a);
        if (a.a->_flags & CC_ARENA_FLAG_USED_HEAP_OVERFLOW) {
            printf("FAIL: reset should clear the used-overflow flag\n");
            return 7;
        }
        {
            CCArenaCheckpoint cp = cc_arena_checkpoint(a);
            if (cp.arena == NULL) {
                printf("FAIL: checkpoint should work again after reset\n");
                return 7;
            }
            cc_arena_restore(cp);
        }
        cc_arena_free(&a);
        printf("  release + explicit heap overflow + checkpoint gating OK\n");
    }

    printf("arena_growable_smoke ok\n");
    return 0;
}
