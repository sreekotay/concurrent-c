/* Checkpoint = lazy mark, promoted to a child only when it must be.
 * Forwarding stays on the host, pre-mark regrow promotes, tip rewinds,
 * nesting, abandon, reset with a promoted child, tiny hosts, hard caps,
 * and the epoch chain. */
#include <ccc/std/prelude.cch>
#include <stdio.h>
#include <string.h>

static int fail(int code, const char *msg) {
    printf("FAIL: %s\n", msg);
    return code;
}

static int test_mark_regrow_promote_pop(void) {
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
    off0 = cc_arena_slab_offset(a.a);

    cp = cc_arena_checkpoint(a);
    if (!cp.arena || cp.parent != a.a || cp.offset != off0 || !cp.id) return fail(1, "checkpoint shape");
    if (a.a->active) return fail(1, "a mark is not a child");
    if (a.a->mark_depth != 1 || cc__arena_floor(a.a) != off0) return fail(1, "mark on the host");
    if (cc_arena_slab_offset(a.a) != off0) return fail(1, "tip untouched by the mark");

    /* Fresh allocation keeps landing on the host's own slab. */
    s = cc_arena_alloc(a, 100, 8);
    if (!s) return fail(1, "scratch");
    if (cc__arena_owner_host(a.a, s) != a.a) return fail(1, "scratch stays on the host");
    if (!cc__arena_in_scratch(a.a, s) || cc__arena_in_scratch(a.a, pre)) return fail(1, "scratch range");
    if (cc_arena_ptr_tier(a, s) != CC_ARENA_TIER_L1) return fail(1, "scratch tier L1");

    /* Same-handle regrow of a pre-mark block must not grow into scratch:
     * the mark becomes a child and the block moves to a fresh host slab. */
    grown = cc_arena_realloc(a, a, pre, 64, 200, 8);
    if (!grown) return fail(1, "regrow");
    if (!a.a->active || !(a.a->active->_flags & CC_ARENA_FLAG_PROMOTED_CHILD)) return fail(1, "regrow promoted the mark");
    if (cc__arena_owner_host(a.a, grown) != a.a) return fail(1, "regrow stays with the host");
    if (a.a->slab->block_idx != 1) return fail(1, "regrow took a host extent");
    if (((unsigned char *)grown)[0] != 0x11 || ((unsigned char *)grown)[63] != 0x11)
        return fail(1, "regrow copied the prefix");
    if (cc__arena_owner_host(a.a, s) != a.a->active) return fail(1, "scratch now belongs to the child");
    if (cc__arena_find_block(a.a, s)) return fail(1, "host must not claim child bytes");

    /* Release of a scratch pointer through the host handle reaches the child. */
    if (!cc_arena_release(a, s)) return fail(1, "release scratch via host");

    if (!cc_arena_restore(cp)) return fail(1, "restore");
    if (a.a->active) return fail(1, "no active child after restore");
    if (cc_arena_restore(cp)) return fail(1, "consumed handle refuses");
    /* The old root slab is now an extent (the regrow grew the host); the
     * pop returned its offset to where the mark began. */
    if (a.a->slab->prev == NULL || cc__slab_offset(a.a->slab->prev) != off0)
        return fail(1, "tail popped on the marked slab");
    if (a.a->slab->prev->tail_carved != a.a->slab->prev->capacity) return fail(1, "carve mark cleared");
    after = cc_arena_alloc(a, 8, 8);
    if (!after) return fail(1, "alloc after restore");
    if (cc__arena_owner_host(a.a, after) != a.a) return fail(1, "post-restore alloc is the host's");
    cc_arena_free(&a);
    printf("  mark / regrow promotes / pop OK\n");
    return 0;
}

static int test_rewind_on_root(void) {
    CCArena a = cc_arena_heap(4096);
    void *pre;
    void *after;
    size_t off0;
    CCArenaCheckpoint cp;
    if (!a.base) return fail(2, "heap");
    pre = cc_arena_alloc(a, 48, 8);
    if (!pre) return fail(2, "pre");
    off0 = cc_arena_slab_offset(a.a);
    cp = cc_arena_checkpoint(a);
    if (!cp.arena) return fail(2, "cp");
    if (!cc_arena_alloc(a, 500, 8)) return fail(2, "scratch");
    if (cc_arena_slab_live(a.a) != 2) return fail(2, "scratch counted on the host");
    if (!cc_arena_restore(cp)) return fail(2, "restore");
    if (a.a->active || a.a->mark_depth) return fail(2, "nothing promoted, mark popped");
    if (cc_arena_slab_offset(a.a) != off0) return fail(2, "tip back at the mark");
    if (cc_arena_slab_live(a.a) != 1) return fail(2, "scratch no longer counted");
    after = cc_arena_alloc(a, 8, 8);
    if ((uint8_t *)after != a.a->slab->base + off0) return fail(2, "next alloc at the mark");
    /* A pre-mark object at the tip released during scratch becomes a hole,
     * never a pop below the mark. */
    cp = cc_arena_checkpoint(a);
    if (!cp.arena) return fail(2, "cp2");
    if (!cc_arena_release_sized(a, after, 8)) return fail(2, "release pre-mark tip object");
    if (cc_arena_slab_offset(a.a) != cp.offset) return fail(2, "tip never drops below the floor");
    if (!cc_arena_restore(cp)) return fail(2, "restore2");
    if (cc_arena_slab_offset(a.a) != cp.offset) return fail(2, "restore keeps the mark tip");
    cc_arena_free(&a);
    printf("  rewind on root OK\n");
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
    if (!x || cc__arena_owner_host(a.a, x) != a.a) return fail(3, "outer scratch");
    inner = cc_arena_checkpoint(a);
    if (!inner.arena || inner.parent != a.a || inner.id == outer.id) return fail(3, "inner nests on the host");
    if (a.a->mark_depth != 2) return fail(3, "two marks");
    x = cc_arena_alloc(a, 32, 8);
    if (!x || cc__arena_owner_host(a.a, x) != a.a) return fail(3, "inner scratch");
    if (cc_arena_restore(outer)) return fail(3, "outer pinned by armed inner");
    if (!cc_arena_checkpoint_abandon(&inner)) return fail(3, "abandon inner");
    if (inner.arena) return fail(3, "abandon consumed the handle");
    if (a.a->mark_depth != 2 || cc__arena_mark_at(a.a, 1)->armed) return fail(3, "abandoned mark stays, disarmed");
    x = cc_arena_alloc(a, 32, 8);
    if (!x || cc__arena_owner_host(a.a, x) != a.a) return fail(3, "allocs keep landing above the abandoned mark");
    if (!cc_arena_restore(outer)) return fail(3, "outer restore after abandon");
    if (cc_arena_slab_offset(a.a) != 0 || a.a->active || a.a->mark_depth) return fail(3, "all scratch gone");
    cc_arena_free(&a);
    printf("  nested / abandon OK\n");
    return 0;
}

static int test_depth_promotes(void) {
    CCArena a = cc_arena_heap(8192);
    CCArenaCheckpoint cps[CC_ARENA_MARK_DEPTH + 2];
    int i;
    if (!a.base) return fail(7, "heap");
    for (i = 0; i < CC_ARENA_MARK_DEPTH + 2; i++) {
        cps[i] = cc_arena_checkpoint(a);
        if (!cps[i].arena) return fail(7, "deep checkpoint arms");
        if (!cc_arena_alloc(a, 16, 8)) return fail(7, "deep scratch");
    }
    /* The fifth mark promoted the first four; the outermost is the child. */
    if (!a.a->active || !(a.a->active->_flags & CC_ARENA_FLAG_PROMOTED_CHILD)) return fail(7, "depth promoted");
    if (a.a->mark_depth != 0) return fail(7, "host marks moved");
    if (cc_arena_restore(cps[0])) return fail(7, "outer pinned while inner marks are armed");
    for (i = CC_ARENA_MARK_DEPTH + 1; i >= 0; i--) {
        if (!cc_arena_restore(cps[i])) return fail(7, "LIFO restore through the promoted child");
    }
    if (a.a->active || a.a->mark_depth || cc_arena_slab_offset(a.a) != 0) return fail(7, "everything rewound");
    cc_arena_free(&a);
    printf("  depth promotes OK\n");
    return 0;
}

static int test_reset_and_free_with_promoted_child(void) {
    CCArena a = cc_arena_heap(4096);
    CCArenaCheckpoint cp;
    if (!a.base) return fail(4, "heap");
    cp = cc_arena_checkpoint(a);
    if (!cp.arena) return fail(4, "cp");
    if (!cc_arena_alloc(a, 3000, 8)) return fail(4, "scratch");
    if (a.a->active) return fail(4, "fits: still a mark");
    if (!cc_arena_alloc(a, 3000, 8)) return fail(4, "scratch outgrows the slab");
    if (!a.a->active) return fail(4, "outgrowing promoted the mark");
    cc_arena_reset(a);
    if (a.a->active || a.a->children || a.a->mark_depth) return fail(4, "reset tore down the child and marks");
    if (cc_arena_slab_offset(a.a) != 0 || a.a->slab->block_idx != 0) return fail(4, "reset state");
    if (cc_arena_restore(cp)) return fail(4, "stale handle refuses after reset");
    cp = cc_arena_checkpoint(a);
    if (!cp.arena) return fail(4, "cp after reset");
    if (!cc_arena_alloc(a, 3000, 8) || !cc_arena_alloc(a, 3000, 8)) return fail(4, "promote again");
    cc_arena_free(&a); /* frees the promoted child on the way */
    printf("  reset / free with promoted child OK\n");
    return 0;
}

static int test_tiny_host_and_hard_cap(void) {
    /* Tiny host: the mark arms; scratch that does not fit promotes and the
     * child grows on its own. */
    {
        CCArena a = cc_arena_heap(64);
        CCArenaCheckpoint cp;
        void *s;
        if (!a.base) return fail(5, "heap");
        cp = cc_arena_checkpoint(a);
        if (!cp.arena || cp.arena != a.a) return fail(5, "mark arms on a tiny host");
        s = cc_arena_alloc(a, 100, 8);
        if (!s || !a.a->active) return fail(5, "scratch promoted");
        if (!(a.a->active->_flags & CC_ARENA_FLAG_HOST_OWNED)) return fail(5, "promoted child owns its host");
        if (cc__arena_owner_host(a.a, s) != a.a->active) return fail(5, "scratch in the child");
        if (!cc_arena_restore(cp)) return fail(5, "restore promoted child");
        if (a.a->active) return fail(5, "cleared");
        cc_arena_free(&a);
    }
    /* Hard cap (FIXED, overflow off): the mark arms with no tail to spare;
     * scratch that fits works, scratch past the cap fails closed. */
    {
        _Alignas(CCArenaHost) uint8_t buf[CC_ARENA_REGION_BYTES(96)];
        CCArena a = cc_arena_wrap_region(buf, sizeof(buf), CC_ARENA_FIXED);
        CCArenaCheckpoint cp;
        void *p;
        if (!a.base) return fail(5, "fixed");
        cp = cc_arena_checkpoint(a);
        if (!cp.arena) return fail(5, "hard cap arms");
        p = cc_arena_alloc(a, 16, 8);
        if (!p || cc__arena_owner_host(a.a, p) != a.a) return fail(5, "scratch on the host");
        if (cc_arena_alloc(a, 4096, 8)) return fail(5, "scratch past the cap fails closed");
        if (!cc_arena_restore(cp)) return fail(5, "restore fixed");
        p = cc_arena_alloc(a, 16, 8);
        if (!p || cc__arena_owner_host(a.a, p) != a.a) return fail(5, "still the host's");
        cc_arena_free(&a);
    }
    {
        _Alignas(CCArenaHost) uint8_t buf[CC_ARENA_REGION_BYTES(1024)];
        CCArena a = cc_arena_wrap_region(buf, sizeof(buf), CC_ARENA_FIXED);
        CCArenaCheckpoint cp;
        if (!a.base) return fail(5, "fixed big");
        cp = cc_arena_checkpoint(a);
        if (!cp.arena) return fail(5, "mark on fixed");
        if (!cc_arena_alloc(a, 256, 8)) return fail(5, "scratch fits");
        if (cc_arena_alloc(a, 4096, 8)) return fail(5, "scratch past the cap fails closed");
        if (a.a->active && a.a->active->block_max != CC_ARENA_FIXED) return fail(5, "child inherits the hard cap");
        if (!cc_arena_restore(cp)) return fail(5, "restore fixed");
        cc_arena_free(&a);
    }
    printf("  tiny host / hard cap OK\n");
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
    if (!cc_arena_would_fit(a, 64, 8)) return fail(6, "would_fit");
    mid = cc_arena_alloc_slice_bytes(a, 16);
    if (!mid.ptr) return fail(6, "mid");
    if (cc_slice_id_epoch(mid.id) == cc_slice_id_epoch(pre.id)) return fail(6, "scratch epoch differs");
    if (!cc_slice_is_from_arena_epoch(mid, a.a)) return fail(6, "live via the host");
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
    if ((rc = test_mark_regrow_promote_pop()) != 0) return rc;
    if ((rc = test_rewind_on_root()) != 0) return rc;
    if ((rc = test_nested_and_abandon()) != 0) return rc;
    if ((rc = test_depth_promotes()) != 0) return rc;
    if ((rc = test_reset_and_free_with_promoted_child()) != 0) return rc;
    if ((rc = test_tiny_host_and_hard_cap()) != 0) return rc;
    if ((rc = test_would_fit_and_epoch()) != 0) return rc;
    printf("arena_active_child_smoke OK\n");
    return 0;
}
