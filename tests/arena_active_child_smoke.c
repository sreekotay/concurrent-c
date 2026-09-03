/* Checkpoint = active tail child. Forwarding, parent-owned regrow, tip pop,
 * nesting, abandon, reset with an active child, heap-rooted fallback, hard
 * caps, and the query helpers across the active chain. */
#include <ccc/std/prelude.cch>
#include <stdio.h>
#include <string.h>

static int fail(int code, const char *msg) {
    printf("FAIL: %s\n", msg);
    return code;
}

static int test_forward_regrow_pop(void) {
    CCArena a = cc_arena_heap(4096);
    void *pre;
    void *s;
    void *grown;
    void *after;
    size_t off0;
    CCArenaCheckpoint cp;
    if (!a.base) return fail(1, "heap");
    pre = cc_arena_alloc(a, 64, 8);
    if (!pre) return fail(1, "pre");
    memset(pre, 0x11, 64);
    off0 = cc_atomic_load(&a.a->offset);

    cp = cc_arena_checkpoint(a);
    if (!cp.arena || cp.parent != a.a || cp.offset != off0) return fail(1, "checkpoint shape");
    if (a.a->active != cp.arena) return fail(1, "child is active");
    if (!(cp.arena->_flags & CC_ARENA_FLAG_TAIL_CHILD)) return fail(1, "tail child");
    if (cc_atomic_load(&a.a->offset) != a.a->capacity) return fail(1, "parent tip parked at capacity");

    /* Fresh allocation through the parent handle lands in the child. */
    s = cc_arena_alloc(a, 100, 8);
    if (!s) return fail(1, "scratch");
    if (cc__arena_owner_host(a.a, s) != cp.arena) return fail(1, "scratch owned by child");
    if (cc__arena_find_block(a.a, s)) return fail(1, "parent must not claim child bytes");
    if (cc_arena_ptr_tier(a, s) != CC_ARENA_TIER_NONE) return fail(1, "parent tier: not the parent's");
    if (cc_arena_ptr_tier(cp.arena, s) != CC_ARENA_TIER_L1) return fail(1, "child tier L1");

    /* Same-handle regrow of a parent-owned block stays in the parent: the
     * tip is the child's, so the parent takes a fresh extent. */
    grown = cc_arena_realloc(a, a, pre, 64, 200, 8);
    if (!grown) return fail(1, "regrow");
    if (cc__arena_owner_host(a.a, grown) != a.a) return fail(1, "regrow stays with the parent");
    if (a.a->block_idx != 1) return fail(1, "regrow took a parent extent");
    if (((unsigned char *)grown)[0] != 0x11 || ((unsigned char *)grown)[63] != 0x11)
        return fail(1, "regrow copied the prefix");

    /* Release of a child pointer through the parent handle reaches the child. */
    if (!cc_arena_release(a, s)) return fail(1, "release scratch via parent");
    if (cc_arena_release(a, s)) return fail(1, "double release refused");

    if (!cc_arena_restore(cp)) return fail(1, "restore");
    if (a.a->active) return fail(1, "no active child after restore");
    if (cc_arena_restore(cp)) return fail(1, "consumed handle refuses");
    /* The old root slab is now an extent (the regrow grew the root); the
     * pop returned its offset to where the child began. */
    if (a.a->prev == NULL || cc_atomic_load(&a.a->prev->offset) != off0)
        return fail(1, "tail popped on the carved slab");
    if (a.a->prev->tail_carved != a.a->prev->capacity) return fail(1, "carve mark cleared");
    after = cc_arena_alloc(a, 8, 8);
    if (!after) return fail(1, "alloc after restore");
    if (cc__arena_owner_host(a.a, after) != a.a) return fail(1, "post-restore alloc is the parent's");
    cc_arena_free(&a);
    printf("  forward / regrow / pop OK\n");
    return 0;
}

static int test_pop_on_root(void) {
    CCArena a = cc_arena_heap(4096);
    void *pre;
    void *after;
    size_t off0;
    CCArenaCheckpoint cp;
    if (!a.base) return fail(2, "heap");
    pre = cc_arena_alloc(a, 48, 8);
    if (!pre) return fail(2, "pre");
    off0 = cc_atomic_load(&a.a->offset);
    cp = cc_arena_checkpoint(a);
    if (!cp.arena) return fail(2, "cp");
    if (!cc_arena_alloc(a, 500, 8)) return fail(2, "scratch");
    if (!cc_arena_restore(cp)) return fail(2, "restore");
    if (cc_atomic_load(&a.a->offset) != off0) return fail(2, "tip popped to the checkpoint offset");
    if (cc_atomic_load(&a.a->live_allocs) != 1) return fail(2, "child region no longer counted");
    after = cc_arena_alloc(a, 8, 8);
    if ((uint8_t *)after != a.a->base + off0) return fail(2, "next alloc at the checkpoint offset");
    cc_arena_free(&a);
    printf("  pop on root OK\n");
    return 0;
}

static int test_nested_and_abandon(void) {
    CCArena a = cc_arena_heap(8192);
    CCArenaCheckpoint outer;
    CCArenaCheckpoint inner;
    void *x;
    if (!a.base) return fail(3, "heap");
    outer = cc_arena_checkpoint(a);
    if (!outer.arena) return fail(3, "outer");
    x = cc_arena_alloc(a, 32, 8);
    if (!x || cc__arena_owner_host(a.a, x) != outer.arena) return fail(3, "outer scratch");
    inner = cc_arena_checkpoint(a);
    if (!inner.arena || inner.parent != outer.arena) return fail(3, "inner nests in outer");
    x = cc_arena_alloc(a, 32, 8);
    if (!x || cc__arena_owner_host(a.a, x) != inner.arena) return fail(3, "inner scratch");
    if (cc_arena_remaining(a) != cc_arena_remaining(inner.arena)) return fail(3, "remaining reports the innermost");
    if (cc_arena_restore(outer)) return fail(3, "outer pinned by armed inner");
    if (!cc_arena_checkpoint_abandon(&inner)) return fail(3, "abandon inner");
    if (outer.arena->active == NULL) return fail(3, "abandoned inner still active");
    x = cc_arena_alloc(a, 32, 8);
    if (!x || cc__arena_owner_host(a.a, x) != outer.arena->active) return fail(3, "allocs keep landing in the abandoned inner");
    if (!cc_arena_restore(outer)) return fail(3, "outer restore after abandon");
    if (cc_atomic_load(&a.a->offset) != 0 || a.a->active) return fail(3, "all scratch gone");
    cc_arena_free(&a);
    printf("  nested / abandon OK\n");
    return 0;
}

static int test_reset_and_free_with_active_child(void) {
    CCArena a = cc_arena_heap(4096);
    CCArenaCheckpoint cp;
    if (!a.base) return fail(4, "heap");
    cp = cc_arena_checkpoint(a);
    if (!cp.arena) return fail(4, "cp");
    if (!cc_arena_alloc(a, 3000, 8)) return fail(4, "scratch");
    if (!cc_arena_alloc(a, 3000, 8)) return fail(4, "scratch grows the child");
    cc_arena_reset(a);
    if (a.a->active || a.a->children) return fail(4, "reset tore down the child");
    if (cc_atomic_load(&a.a->offset) != 0 || a.a->block_idx != 0) return fail(4, "reset state");
    if (cc_arena_restore(cp)) return fail(4, "stale handle refuses after reset");
    cp = cc_arena_checkpoint(a);
    if (!cp.arena) return fail(4, "cp after reset");
    cc_arena_free(&a); /* frees the active child on the way */
    printf("  reset / free with active child OK\n");
    return 0;
}

static int test_heap_fallback_and_hard_cap(void) {
    /* Tiny tail: the child is heap-rooted, dies at restore. */
    {
        CCArena a = cc_arena_heap(64);
        CCArenaCheckpoint cp;
        void *s;
        if (!a.base) return fail(5, "heap");
        cp = cc_arena_checkpoint(a);
        if (!cp.arena) return fail(5, "heap-rooted child arms");
        if (cp.arena->_flags & CC_ARENA_FLAG_TAIL_CHILD) return fail(5, "not a tail child");
        if (!(cp.arena->_flags & CC_ARENA_FLAG_REGION_OWNED)) return fail(5, "child owns its region");
        s = cc_arena_alloc(a, 100, 8);
        if (!s || cc__arena_owner_host(a.a, s) != cp.arena) return fail(5, "scratch in heap child");
        if (!cc_arena_restore(cp)) return fail(5, "restore heap child");
        if (a.a->active) return fail(5, "cleared");
        cc_arena_free(&a);
    }
    /* Hard cap (FIXED, overflow off) with no tail: unarmed, allocations
     * keep landing in the parent, nothing mallocs. */
    {
        _Alignas(CCArenaHost) uint8_t buf[CC_ARENA_REGION_BYTES(96)];
        CCArena a = cc_arena_wrap_region(buf, sizeof(buf), CC_ARENA_FIXED);
        CCArenaCheckpoint cp;
        void *p;
        if (!a.base) return fail(5, "fixed");
        cp = cc_arena_checkpoint(a);
        if (cp.arena) return fail(5, "hard cap without tail stays unarmed");
        if (cc_arena_restore(cp)) return fail(5, "unarmed restore refuses");
        p = cc_arena_alloc(a, 16, 8);
        if (!p || cc__arena_owner_host(a.a, p) != a.a) return fail(5, "still the parent's");
        cc_arena_free(&a);
    }
    /* Hard cap with room: tail child, no malloc; scratch past the tail
     * fails closed instead of spilling. */
    {
        _Alignas(CCArenaHost) uint8_t buf[CC_ARENA_REGION_BYTES(1024)];
        CCArena a = cc_arena_wrap_region(buf, sizeof(buf), CC_ARENA_FIXED);
        CCArenaCheckpoint cp;
        if (!a.base) return fail(5, "fixed big");
        cp = cc_arena_checkpoint(a);
        if (!cp.arena || !(cp.arena->_flags & CC_ARENA_FLAG_TAIL_CHILD)) return fail(5, "tail child on fixed");
        if (cp.arena->block_max != CC_ARENA_FIXED) return fail(5, "child inherits the hard cap");
        if (!cc_arena_alloc(a, 256, 8)) return fail(5, "scratch fits");
        if (cc_arena_alloc(a, 4096, 8)) return fail(5, "scratch past the cap fails closed");
        if (!cc_arena_restore(cp)) return fail(5, "restore fixed");
        cc_arena_free(&a);
    }
    printf("  heap fallback / hard cap OK\n");
    return 0;
}

static int test_would_fit_and_epoch(void) {
    CCArena a = cc_arena_heap(2048);
    CCArenaCheckpoint cp;
    CCSlice pre;
    CCSlice mid;
    if (!a.base) return fail(6, "heap");
    pre = cc_arena_alloc_slice_bytes(a, 16);
    if (!pre.ptr) return fail(6, "pre");
    cp = cc_arena_checkpoint(a);
    if (!cp.arena) return fail(6, "cp");
    if (!cc_arena_would_fit(a, 64, 8)) return fail(6, "would_fit consults the child");
    mid = cc_arena_alloc_slice_bytes(a, 16);
    if (!mid.ptr) return fail(6, "mid");
    if (cc_slice_id_epoch(mid.id) == cc_slice_id_epoch(pre.id)) return fail(6, "child epoch differs");
    if (!cc_slice_is_from_arena_epoch(mid, a.a)) return fail(6, "live via parent handle");
    if (!cc_slice_is_from_arena_epoch(pre, a.a)) return fail(6, "pre still live");
    if (!cc_arena_restore(cp)) return fail(6, "restore");
    if (cc_slice_is_from_arena_epoch(mid, a.a)) return fail(6, "scratch view dead");
    if (!cc_slice_is_from_arena_epoch(pre, a.a)) return fail(6, "pre live after restore");
    cc_arena_free(&a);
    printf("  would_fit / epoch chain OK\n");
    return 0;
}

int main(void) {
    int rc;
    if ((rc = test_forward_regrow_pop()) != 0) return rc;
    if ((rc = test_pop_on_root()) != 0) return rc;
    if ((rc = test_nested_and_abandon()) != 0) return rc;
    if ((rc = test_reset_and_free_with_active_child()) != 0) return rc;
    if ((rc = test_heap_fallback_and_hard_cap()) != 0) return rc;
    if ((rc = test_would_fit_and_epoch()) != 0) return rc;
    printf("arena_active_child_smoke OK\n");
    return 0;
}
