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
            int *p = arena_alloc(int, &a, 10);  // 40 bytes per alloc
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
            arena_alloc(int, &a, 10);
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
        int *p0 = arena_alloc(int, &a, 4);
        if (!p0) return 3;
        for (int i = 0; i < 4; i++) p0[i] = i;

        // Take checkpoint in block 0
        CCArenaCheckpoint cp = cc_arena_checkpoint(&a);
        if (cp.block_idx != 0) { printf("FAIL: cp block_idx should be 0\n"); return 3; }

        // Force growth past block 0
        for (int i = 0; i < 50; i++) {
            arena_alloc(int, &a, 10);
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
        CCArena a = cc_arena_heap_budget(64, 3);  // at most 3 blocks
        if (!a.base) return 4;
        if (a.block_max != 3) { printf("FAIL: expected block_max=3\n"); return 4; }

        // Keep allocating until budget is exhausted
        int alloc_count = 0;
        while (alloc_count < 10000) {
            int *p = arena_alloc(int, &a, 10);
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
        uint8_t buf[128];
        CCArena a;
        cc_arena_init(&a, buf, sizeof(buf));
        if (a.block_max != 1) { printf("FAIL: init should be fixed\n"); return 5; }

        // Fill it up
        int *p = arena_alloc(int, &a, 30);  // 120 bytes
        if (!p) { printf("FAIL: initial alloc\n"); return 5; }

        // This should fail (fixed arena, no growth)
        int *q = arena_alloc(int, &a, 30);
        if (q != NULL) { printf("FAIL: fixed arena should not grow\n"); return 5; }
        printf("  fixed: correctly rejected overflow OK\n");
        // No cc_arena_free needed (user-backed)
    }

    printf("arena_growable_smoke ok\n");
    return 0;
}
