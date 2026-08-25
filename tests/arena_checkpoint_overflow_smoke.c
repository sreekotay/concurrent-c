/* Checkpoint/restore across overflow: per-object (malloc ctor) and chunked
 * (heap/stack after slab budget). live_allocs is restored; older-epoch
 * overflow release disables rewind; current-epoch overflow release does not. */
#include <ccc/std/prelude.cch>
#include <stdio.h>
#include <string.h>

static int fail(int code, const char *msg) {
    printf("FAIL: %s\n", msg);
    return code;
}

static int test_malloc_ctor_epoch_drain(void) {
    CCArena a = cc_arena_malloc(64);
    void *keep;
    void *drop;
    CCArenaCheckpoint cp;
    if (!a.base) return fail(1, "malloc ctor");

    keep = cc_arena_alloc(&a, 128, 8);
    if (!keep) {
        cc_arena_free(&a);
        return fail(1, "pre-checkpoint overflow");
    }
    memset(keep, 0x11, 128);
    if (!(a.a->_flags & CC_ARENA_FLAG_USED_HEAP_OVERFLOW)) {
        cc_arena_free(&a);
        return fail(1, "expected overflow before checkpoint");
    }
    if (a.a->_flags & CC_ARENA_FLAG_NON_REWINDABLE) {
        cc_arena_free(&a);
        return fail(1, "overflow alloc must stay rewindable");
    }

    cp = cc_arena_checkpoint(&a);
    if (cp.arena == NULL) {
        cc_arena_free(&a);
        return fail(1, "checkpoint after overflow alloc");
    }

    drop = cc_arena_alloc(&a, 128, 8);
    if (!drop) {
        cc_arena_free(&a);
        return fail(1, "post-checkpoint overflow");
    }
    memset(drop, 0x22, 128);

    cc_arena_restore(cp);
    if (((unsigned char *)keep)[0] != 0x11) {
        cc_arena_free(&a);
        return fail(1, "keep-set overflow clobbered");
    }
    {
        int n = 0;
        CCArenaOvfHeader *h;
        for (h = a.a->ovf_head; h; h = h->next) n++;
        if (n != 1) {
            cc_arena_free(&a);
            return fail(1, "post-checkpoint overflow should be drained");
        }
        if (a.a->ovf_head != cc__arena_ovf_header(keep)) {
            cc_arena_free(&a);
            return fail(1, "keep-set overflow should remain on ovf_head");
        }
    }
    (void)drop;
    if (!cc_arena_release(&a, keep)) {
        cc_arena_free(&a);
        return fail(1, "keep-set overflow should still be owned");
    }
    cc_arena_free(&a);
    printf("  malloc ctor: overflow checkpoint drain OK\n");
    return 0;
}

static int test_live_allocs_and_post_epoch_release(void) {
    CCArena a = cc_arena_malloc(256);
    void *slab;
    void *ovf;
    CCArenaCheckpoint cp;
    if (!a.base) return fail(2, "malloc ctor live");

    slab = cc_arena_alloc(&a, 32, 8);
    if (!slab) {
        cc_arena_free(&a);
        return fail(2, "slab alloc");
    }
    cp = cc_arena_checkpoint(&a);
    if (cp.arena == NULL || cp.live_allocs != 1) {
        cc_arena_free(&a);
        return fail(2, "checkpoint live_allocs");
    }

    ovf = cc_arena_alloc(&a, 512, 8);
    if (!ovf) {
        cc_arena_free(&a);
        return fail(2, "post-checkpoint overflow");
    }
    if (!cc_arena_release(&a, ovf)) {
        cc_arena_free(&a);
        return fail(2, "current-epoch overflow release");
    }
    if (a.a->_flags & CC_ARENA_FLAG_NON_REWINDABLE) {
        cc_arena_free(&a);
        return fail(2, "current-epoch overflow release must stay rewindable");
    }

    cc_arena_restore(cp);
    if (cc_atomic_load(&a.a->live_allocs) != 1) {
        cc_arena_free(&a);
        return fail(2, "live_allocs after restore");
    }
    if (!cc_arena_release(&a, slab)) {
        cc_arena_free(&a);
        return fail(2, "pre-checkpoint slab after restore");
    }
    cc_arena_free(&a);
    printf("  live_allocs + current-epoch overflow release OK\n");
    return 0;
}

static int test_pre_epoch_overflow_release_refuses_restore(void) {
    CCArena a = cc_arena_malloc(64);
    void *keep;
    CCArenaCheckpoint cp;
    CCArenaCheckpoint later;
    if (!a.base) return fail(3, "malloc ctor disable");

    keep = cc_arena_alloc(&a, 128, 8);
    if (!keep) {
        cc_arena_free(&a);
        return fail(3, "overflow keep");
    }
    cp = cc_arena_checkpoint(&a);
    if (cp.arena == NULL) {
        cc_arena_free(&a);
        return fail(3, "checkpoint");
    }
    if (!cc_arena_release(&a, keep)) {
        cc_arena_free(&a);
        return fail(3, "release keep-set overflow");
    }
    if (a.a->_flags & CC_ARENA_FLAG_NON_REWINDABLE) {
        cc_arena_free(&a);
        return fail(3, "overflow release must not set a slab-hole flag");
    }
    if (cc_arena_restore(cp)) {
        cc_arena_free(&a);
        return fail(3, "restore of punctured keep-set must refuse");
    }
    later = cc_arena_checkpoint(&a);
    if (later.arena == NULL) {
        cc_arena_free(&a);
        return fail(3, "new checkpoint must work after dropped/punctured handle");
    }
    if (!cc_arena_restore(later)) {
        cc_arena_free(&a);
        return fail(3, "restore of later checkpoint");
    }
    cc_arena_free(&a);
    printf("  punctured keep-set refuses restore; new checkpoint OK\n");
    return 0;
}

static int test_last_live_does_not_unbreak_keep_set(void) {
    CCArena a = cc_arena_malloc(256);
    void *keep;
    void *slab;
    CCArenaCheckpoint cp;
    if (!a.base) return fail(6, "malloc ctor last-live");

    keep = cc_arena_alloc(&a, 128, 8);
    if (!keep) {
        cc_arena_free(&a);
        return fail(6, "overflow keep");
    }
    cp = cc_arena_checkpoint(&a);
    if (cp.arena == NULL) {
        cc_arena_free(&a);
        return fail(6, "checkpoint");
    }
    if (!cc_arena_release(&a, keep)) {
        cc_arena_free(&a);
        return fail(6, "release keep");
    }
    slab = cc_arena_alloc(&a, 32, 8);
    if (!slab) {
        cc_arena_free(&a);
        return fail(6, "slab after puncture");
    }
    if (!cc_arena_release(&a, slab)) {
        cc_arena_free(&a);
        return fail(6, "last-live slab release");
    }
    if (a.a->_flags & CC_ARENA_FLAG_NON_REWINDABLE) {
        cc_arena_free(&a);
        return fail(6, "last-live should clear slab hole");
    }
    if (cc_arena_restore(cp)) {
        cc_arena_free(&a);
        return fail(6, "last-live must not make a punctured keep-set restorable");
    }
    cc_arena_free(&a);
    printf("  last-live does not unbreak overflow keep-set OK\n");
    return 0;
}

static int test_discarded_checkpoint_does_not_poison(void) {
    CCArena a = cc_arena_malloc(64);
    void *keep;
    CCArenaCheckpoint later;
    if (!a.base) return fail(7, "malloc ctor discard");

    keep = cc_arena_alloc(&a, 128, 8);
    if (!keep) {
        cc_arena_free(&a);
        return fail(7, "overflow");
    }
    (void)cc_arena_checkpoint(&a); /* generation barrier; handle dropped */
    if (!cc_arena_release(&a, keep)) {
        cc_arena_free(&a);
        return fail(7, "release after discarded checkpoint");
    }
    later = cc_arena_checkpoint(&a);
    if (later.arena == NULL) {
        cc_arena_free(&a);
        return fail(7, "discarded checkpoint must not block a new capture");
    }
    if (!cc_arena_restore(later)) {
        cc_arena_free(&a);
        return fail(7, "restore later after discard");
    }
    cc_arena_free(&a);
    printf("  discarded checkpoint does not poison later capture OK\n");
    return 0;
}

static int test_stale_nested_restore_refuses(void) {
    CCArena a = cc_arena_heap(kilobytes(1));
    CCArenaCheckpoint cp1;
    CCArenaCheckpoint cp2;
    if (!a.base) return fail(8, "heap nested");

    if (!cc_arena_alloc(&a, 32, 8)) {
        cc_arena_free(&a);
        return fail(8, "first slab");
    }
    cp1 = cc_arena_checkpoint(&a);
    if (!cc_arena_alloc(&a, 32, 8)) {
        cc_arena_free(&a);
        return fail(8, "second slab");
    }
    cp2 = cc_arena_checkpoint(&a);
    if (!cc_arena_alloc(&a, 32, 8)) {
        cc_arena_free(&a);
        return fail(8, "third slab");
    }
    if (!cc_arena_restore(cp1)) {
        cc_arena_free(&a);
        return fail(8, "restore older checkpoint");
    }
    if (cc_arena_restore(cp2)) {
        cc_arena_free(&a);
        return fail(8, "stale nested restore must refuse");
    }
    cc_arena_free(&a);
    printf("  stale nested restore refuses OK\n");
    return 0;
}

static int test_stack_chunk_epoch_drain(void) {
    cc_arena_stack(s, 64);
    void *grown;
    void *keep;
    void *drop;
    CCArenaCheckpoint cp;
    uint8_t *stack_buf = s.a->base;
    int n;

    s.a->block_max = 2; /* root + one grow, then chunk overflow */
    if (!cc_arena_alloc(&s, 32, 8)) {
        cc_arena_free(&s);
        return fail(4, "stack root fill");
    }
    grown = cc_arena_alloc(&s, 128, 8);
    if (!grown || !cc__arena_find_block(&s, grown)) {
        cc_arena_free(&s);
        return fail(4, "stack grow");
    }
    keep = cc_arena_alloc(&s, 5000, 8);
    if (!keep || cc__arena_find_block(&s, keep) || !s.a->ovf_chunks) {
        cc_arena_free(&s);
        return fail(4, "stack chunk overflow keep");
    }
    memset(keep, 0x33, 128);

    cp = cc_arena_checkpoint(&s);
    if (cp.arena == NULL) {
        cc_arena_free(&s);
        return fail(4, "stack checkpoint after overflow");
    }

    drop = cc_arena_alloc(&s, 128, 8);
    if (!drop) {
        cc_arena_free(&s);
        return fail(4, "stack post-checkpoint overflow");
    }

    cc_arena_restore(cp);
    (void)stack_buf;
    if (((unsigned char *)keep)[0] != 0x33) {
        cc_arena_free(&s);
        return fail(4, "stack keep-set clobbered");
    }
    n = 0;
    {
        CCArenaOvfChunk *c;
        for (c = s.a->ovf_chunks; c; c = c->next) n++;
    }
    if (n != 1) {
        cc_arena_free(&s);
        return fail(4, "stack post-checkpoint chunk should be drained");
    }
    (void)drop;
    if (!cc_arena_release(&s, keep)) {
        cc_arena_free(&s);
        return fail(4, "stack keep-set still owned");
    }
    cc_arena_free(&s);
    printf("  stack: chunk overflow checkpoint drain OK\n");
    return 0;
}

static int test_heap_chunk_epoch_drain(void) {
    CCArena a = cc_arena_heap(64);
    void *grown;
    void *keep;
    void *drop;
    CCArenaCheckpoint cp;
    int n;
    if (!a.base) return fail(5, "heap ctor");
    a.a->block_max = 2;

    if (!cc_arena_alloc(&a, 32, 8)) {
        cc_arena_free(&a);
        return fail(5, "heap root fill");
    }
    grown = cc_arena_alloc(&a, 128, 8);
    if (!grown || !cc__arena_find_block(&a, grown)) {
        cc_arena_free(&a);
        return fail(5, "heap grow");
    }
    keep = cc_arena_alloc(&a, 5000, 8);
    if (!keep || cc__arena_find_block(&a, keep) || !a.a->ovf_chunks) {
        cc_arena_free(&a);
        return fail(5, "heap chunk overflow keep");
    }
    memset(keep, 0x44, 200);

    cp = cc_arena_checkpoint(&a);
    if (cp.arena == NULL) {
        cc_arena_free(&a);
        return fail(5, "heap checkpoint after overflow");
    }

    drop = cc_arena_alloc(&a, 200, 8);
    if (!drop) {
        cc_arena_free(&a);
        return fail(5, "heap post-checkpoint overflow");
    }

    cc_arena_restore(cp);
    if (((unsigned char *)keep)[0] != 0x44) {
        cc_arena_free(&a);
        return fail(5, "heap keep-set clobbered");
    }
    n = 0;
    {
        CCArenaOvfChunk *c;
        for (c = a.a->ovf_chunks; c; c = c->next) n++;
    }
    if (n != 1) {
        cc_arena_free(&a);
        return fail(5, "heap post-checkpoint chunk should be drained");
    }
    (void)drop;
    if (!cc_arena_release(&a, keep)) {
        cc_arena_free(&a);
        return fail(5, "heap keep-set still owned");
    }
    cc_arena_free(&a);
    printf("  heap: chunk overflow checkpoint drain OK\n");
    return 0;
}

int main(void) {
    int rc;
    if ((rc = test_malloc_ctor_epoch_drain()) != 0) return rc;
    if ((rc = test_live_allocs_and_post_epoch_release()) != 0) return rc;
    if ((rc = test_pre_epoch_overflow_release_refuses_restore()) != 0) return rc;
    if ((rc = test_stack_chunk_epoch_drain()) != 0) return rc;
    if ((rc = test_heap_chunk_epoch_drain()) != 0) return rc;
    if ((rc = test_last_live_does_not_unbreak_keep_set()) != 0) return rc;
    if ((rc = test_discarded_checkpoint_does_not_poison()) != 0) return rc;
    if ((rc = test_stale_nested_restore_refuses()) != 0) return rc;
    printf("arena_checkpoint_overflow_smoke OK\n");
    return 0;
}
