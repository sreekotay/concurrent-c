#include <ccc/std/prelude.cch>
#include <stdio.h>
#include <string.h>

int main(void) {
    // --- Test 1: Basic growth ---
    // Start with a tiny arena (64 bytes) that must grow to hold 4KB of data.
    {
        CCArena a = cc_arena_heap(64);
        if (!a.base) { printf("FAIL: heap alloc\n"); return 1; }
        if (a.block_max != 0) { printf("FAIL: expected unbounded\n"); return 1; }
        if (a.block_idx != 0) { printf("FAIL: expected block_idx=0\n"); return 1; }

        // Allocate enough to force several growths
        for (int i = 0; i < 100; i++) {
            int *p = cc_arena_alloc_T_count(int, &a, 10);  // 40 bytes per alloc
            if (!p) { printf("FAIL: growth alloc at i=%d\n", i); return 1; }
            for (int j = 0; j < 10; j++) p[j] = i * 10 + j;
        }

        if (a.block_idx == 0) { printf("FAIL: expected growth (block_idx > 0)\n"); return 1; }
        printf("  growth: block_idx=%d OK\n", a.block_idx);
        cc_arena_free(&a);
    }

    // --- Test 2: Reset unwinds growth ---
    {
        CCArena a = cc_arena_heap(64);
        if (!a.base) return 2;

        for (int i = 0; i < 50; i++) {
            cc_arena_alloc_T_count(int, &a, 10);
        }
        int saved_idx = a.block_idx;
        if (saved_idx == 0) { printf("FAIL: no growth before reset\n"); return 2; }

        cc_arena_reset(&a);
        if (a.block_idx != 0) { printf("FAIL: block_idx not 0 after reset\n"); return 2; }
        if (a.prev != NULL) { printf("FAIL: prev not NULL after reset\n"); return 2; }
        if (cc_atomic_load(&a.offset) != 0) { printf("FAIL: offset not 0 after reset\n"); return 2; }
        printf("  reset: unwound from block_idx=%d OK\n", saved_idx);
        cc_arena_free(&a);
    }

    // --- Test 3: Checkpoint/restore across blocks ---
    {
        CCArena a = cc_arena_heap(64);
        if (!a.base) return 3;

        // Allocate in block 0
        int *p0 = cc_arena_alloc_T_count(int, &a, 4);
        if (!p0) return 3;
        for (int i = 0; i < 4; i++) p0[i] = i;

        // Take checkpoint in block 0
        CCArenaCheckpoint cp = cc_arena_checkpoint(&a);
        if (cp.block_idx != 0) { printf("FAIL: cp block_idx should be 0\n"); return 3; }

        // Force growth past block 0
        for (int i = 0; i < 50; i++) {
            cc_arena_alloc_T_count(int, &a, 10);
        }
        int grown_idx = a.block_idx;
        if (grown_idx == 0) { printf("FAIL: expected growth\n"); return 3; }

        // Restore checkpoint - should unwind all grown blocks
        cc_arena_restore(cp);
        if (a.block_idx != 0) { printf("FAIL: restore didn't unwind to block 0, got %d\n", a.block_idx); return 3; }

        // Data before checkpoint should still be valid
        for (int i = 0; i < 4; i++) {
            if (p0[i] != i) { printf("FAIL: data corrupted after restore\n"); return 3; }
        }
        printf("  checkpoint/restore: unwound from block_idx=%d to 0 OK\n", grown_idx);
        cc_arena_free(&a);
    }

    // --- Test 4: Budget exhaustion ---
    {
        CCArena a = cc_arena_heap(64);
        a.block_max = 3;  // at most 3 blocks
        if (!a.base) return 4;
        if (a.block_max != 3) { printf("FAIL: expected block_max=3\n"); return 4; }

        // Keep allocating until budget is exhausted
        int alloc_count = 0;
        while (alloc_count < 10000) {
            int *p = cc_arena_alloc_T_count(int, &a, 10);
            if (!p) break;
            alloc_count++;
        }

        if (alloc_count >= 10000) { printf("FAIL: budget not enforced\n"); return 4; }
        if (a.block_idx + 1 < a.block_max) { printf("FAIL: budget not reached\n"); return 4; }
        printf("  budget: exhausted after %d allocs, block_idx=%d/%d OK\n",
               alloc_count, a.block_idx, a.block_max);
        cc_arena_free(&a);
    }

    // --- Test 5: Fixed arena (block_max=1) never grows ---
    {
        uint8_t buf[192];
        CCArena a;
        cc_arena_buffer(&a, buf, sizeof(buf));
        if (a.block_max != 1) { printf("FAIL: buffer should be fixed\n"); return 5; }

        // Fill it up
        int *p = cc_arena_alloc_T_count(int, &a, 30);  // 120 bytes
        if (!p) { printf("FAIL: initial alloc\n"); return 5; }

        // This should fail (fixed arena, no growth)
        int *q = cc_arena_alloc_T_count(int, &a, 30);
        if (q != NULL) { printf("FAIL: fixed arena should not grow\n"); return 5; }
        printf("  fixed: correctly rejected overflow OK\n");
        // No cc_arena_free needed (user-backed)
    }

    // --- Test 6: Stack (user) first block, overflow to heap ---
    {
        uint8_t buf[64];
        CCArena a;
        if (cc_arena_buffer(&a, buf, sizeof(buf)) != 0) {
            printf("FAIL: buffer init\n");
            return 6;
        }
        a.block_max = 0;
        if (a.block_max != 0) {
            printf("FAIL: expected unbounded block_max\n");
            return 6;
        }
        if (a._flags & CC_ARENA_FLAG_HEAP_OWNED) {
            printf("FAIL: initial buffer should not be heap-owned\n");
            return 6;
        }

        void *p = cc_arena_alloc(&a, 128, 8);
        if (!p) {
            printf("FAIL: stack-first arena should grow to heap\n");
            return 6;
        }
        if (a.block_idx == 0) {
            printf("FAIL: expected growth off stack block\n");
            return 6;
        }
        memset(p, 0xab, 128);

        cc_arena_reset(&a);
        if (a.base != buf || a.block_idx != 0 || a.prev != NULL) {
            printf("FAIL: reset should restore stack block\n");
            return 6;
        }

        void *q = cc_arena_alloc(&a, 128, 8);
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
        uint8_t buf[256];
        CCArena a;
        if (cc_arena_buffer(&a, buf, sizeof(buf)) != 0) {
            printf("FAIL: release test init\n");
            return 7;
        }
        if (!cc_arena_set_heap_overflow(&a, true)) {
            printf("FAIL: enable heap overflow\n");
            return 7;
        }

        void *p = cc_arena_alloc(&a, 64, 8);
        if (!p) {
            printf("FAIL: tracked alloc in fixed arena\n");
            return 7;
        }
        if (!cc_arena_release(&a, p)) {
            printf("FAIL: release tracked arena ptr\n");
            return 7;
        }
        if (!cc_arena_release(&a, p)) {
            printf("FAIL: coarse repeat release should be tolerated\n");
            return 7;
        }
        void *p2 = cc_arena_alloc(&a, 64, 8);
        if (!p2 || p2 != p) {
            printf("FAIL: release should make current block reusable\n");
            return 7;
        }
        if (!cc_arena_release(&a, p2)) {
            printf("FAIL: release second tracked ptr\n");
            return 7;
        }

        void *spill = cc_arena_alloc(&a, 512, 8);
        if (!spill) {
            printf("FAIL: explicit heap overflow fallback\n");
            return 7;
        }
        if (!(a._flags & CC_ARENA_FLAG_USED_HEAP_OVERFLOW)) {
            printf("FAIL: expected used heap overflow flag\n");
            return 7;
        }
        if (!cc_arena_release(&a, spill)) {
            printf("FAIL: release heap overflow ptr\n");
            return 7;
        }

        void *foreign = malloc(24);
        if (!foreign) {
            printf("FAIL: foreign alloc\n");
            return 7;
        }
        if (!cc_arena_release(&a, foreign)) {
            printf("FAIL: permissive fallback free\n");
            return 7;
        }

        CCArenaCheckpoint cp = cc_arena_checkpoint(&a);
        if (cp.arena != NULL) {
            printf("FAIL: checkpoint should be disabled after release/spill\n");
            return 7;
        }

        cc_arena_reset(&a);
        if (a._flags & (CC_ARENA_FLAG_USED_HEAP_OVERFLOW | CC_ARENA_FLAG_NON_REWINDABLE)) {
            printf("FAIL: reset should clear non-rewindable flags\n");
            return 7;
        }
        if (cc_arena_checkpoint(&a).arena == NULL) {
            printf("FAIL: checkpoint should work again after reset\n");
            return 7;
        }
        cc_arena_free(&a);
        printf("  release + explicit heap overflow + checkpoint gating OK\n");
    }

    // --- Test 8: releasing an old block must not rewind the new root ---
    {
        CCArena a = cc_arena_heap(64);
        if (!a.base) {
            printf("FAIL: grow-safe release init\n");
            return 8;
        }

        void *old_root = cc_arena_alloc(&a, 48, 8);
        if (!old_root) {
            printf("FAIL: old-root alloc\n");
            return 8;
        }

        uint8_t *new_root = (uint8_t*)cc_arena_alloc(&a, 128, 8);
        if (!new_root) {
            printf("FAIL: new-root alloc after grow\n");
            return 8;
        }
        memset(new_root, 0x5a, 128);

        if (!cc_arena_release(&a, old_root)) {
            printf("FAIL: release old-root alloc after grow\n");
            return 8;
        }

        uint8_t *later = (uint8_t*)cc_arena_alloc(&a, 64, 8);
        if (!later) {
            printf("FAIL: alloc after releasing old-root\n");
            return 8;
        }
        if (later == new_root) {
            printf("FAIL: releasing old-root rewound the new root\n");
            return 8;
        }
        for (size_t i = 0; i < 128; ++i) {
            if (new_root[i] != 0x5a) {
                printf("FAIL: new-root data corrupted after releasing old-root\n");
                return 8;
            }
        }

        cc_arena_free(&a);
        printf("  grow-safe release across blocks OK\n");
    }

    printf("arena_growable_smoke ok\n");
    return 0;
}
