#include <ccc/std/prelude.cch>
#include <ccc/std/map.cch>
#include <assert.h>
#include <string.h>

Vec(int, IntVec);

static inline size_t hash_i32(int k) {
    return cc_map_hash_i32(k);
}

static inline int eq_i32(int a, int b) { return a == b; }

Map(int, int, IntMap, hash_i32, eq_i32);

typedef CCArena (*ArenaFactory)(void);

static CCArena make_fixed_root_arena(void) {
    static _Alignas(CCArenaHost) uint8_t buffer[8192];
    return cc_arena_create_buffer(buffer, sizeof(buffer), CC_ARENA_FIXED);
}

static void test_empty_heap_overflow_rejected(void) {
    CCArena arena = (CCArena){0};
    assert(!arena.base);
    assert(!cc_arena_set_heap_overflow(arena, true));
}

static CCArena make_heap_overflow_arena(void) {
    CCArena arena = cc_arena_malloc(8192);
    assert(arena.base != NULL);
    return arena;
}

static void test_direct_release(ArenaFactory make_arena) {
    uint8_t buffer[512];
    CCArena arena = make_arena();
    (void)buffer;

    void *p = cc_arena_alloc(arena, 32, 8);
    assert(p != NULL);
    assert(cc_arena_slab_live(arena.a) == 1);

    assert(cc_arena_release(arena, p));
    assert(cc_arena_slab_live(arena.a) == 0);
    /* Last-live root release rewinds; a checkpoint child arms and restores. */
    {
        CCArenaCheckpoint cp = cc_arena_checkpoint(arena);
        assert(cp.arena != NULL && cp.parent == arena.a);
        assert(cc_arena_restore(cp));
    }

    void *q = cc_arena_alloc(arena, 32, 8);
    assert(q == p);
    assert(cc_arena_slab_live(arena.a) == 1);

    /* Non-last release: a hole. Holes never disable a checkpoint. */
    void *r = cc_arena_alloc(arena, 32, 8);
    assert(r != NULL);
    assert(cc_arena_release(arena, q));
    assert(cc_arena_slab_live(arena.a) == 1);
    {
        CCArenaCheckpoint cp = cc_arena_checkpoint(arena);
        assert(cp.arena != NULL);
        assert(cc_arena_restore(cp));
    }
    /* Sized release at the tip pops it; the hole below stays until reset. */
    {
        size_t off = cc_arena_slab_offset(arena.a);
        assert(cc_arena_release_sized(arena, r, 32));
        assert(cc_arena_slab_offset(arena.a) == 0); /* last live: full rewind */
        (void)off;
    }

    cc_arena_free(&arena);
}

static void test_vec_release_on_growth(ArenaFactory make_arena) {
    CCArena arena = make_arena();

    IntVec v = IntVec_init(arena, 2);
    assert(v.data != NULL);
    /* Owner header + payload. */
    assert(cc_arena_slab_live(arena.a) == 2);

    assert(IntVec_push(&v, 10) == 0);
    assert(IntVec_push(&v, 20) == 0);
    assert(IntVec_push(&v, 30) == 0); /* forces growth */

    assert(v.data != NULL);
    assert(IntVec_len(&v) == 3);
    /* Tip-in-place regrow keeps the pointer; a move releases the old
     * payload (sized). Either way header + one payload stay live. */
    assert(cc_arena_slab_live(arena.a) == 2);

    /* Destroy: a payload that regrew in place still sits right after its
     * header and ends the slab, so header and payload pop together and
     * nothing is listed. A payload that moved leaves the header behind,
     * listed for rebirth and still counted live. */
    IntVec_destroy(&v);
    {
        size_t live = cc_arena_slab_live(arena.a);
        assert(live <= 1);
        assert((live == 1) == (arena.a->owner_free != NULL));
    }

    cc_arena_free(&arena);
}

static void test_string_release_on_growth(ArenaFactory make_arena) {
    CCArena arena = make_arena();

    CCString s = cc_string_new();
    assert(cc_arena_slab_live(arena.a) == 0);

    assert(cc_string_push(&s, "ab", arena) != NULL);
    assert(cc_string_is_inline(&s));
    assert(cc_string_push(&s, "cdefghijklmnop", arena) != NULL); /* promotes */

    assert(cc_string_data(&s) != NULL);
    assert(strcmp(cc_string_cstr(&s, arena), "abcdefghijklmnop") == 0);
    /* Owner header + payload. */
    assert(cc_arena_slab_live(arena.a) == 2);

    cc_string_destroy(&s);
    assert(cc_string_len(&s) == 0 && cc_string_data(&s) == NULL);
    assert(cc_arena_slab_live(arena.a) == 0); /* header + payload popped together */
    assert(arena.a->owner_free == NULL);

    cc_arena_free(&arena);
}

static void test_map_release_on_resize_and_destroy(ArenaFactory make_arena) {
    CCArena arena = make_arena();

    IntMap *m = IntMap_init(arena);
    assert(m != NULL);
    assert(cc_arena_slab_live(arena.a) == 1); /* map handle */

    for (int i = 0; i < 64; ++i) {
        assert(IntMap_insert(m, i, i + 100) == 0);
    }

    for (int i = 0; i < 64; ++i) {
        int *value = IntMap_get(m, i);
        assert(value && *value == i + 100);
    }

    /* Patched map core keeps one stable handle plus one table allocation. */
    assert(cc_arena_slab_live(arena.a) == 2);

    IntMap_destroy(m);
    assert(cc_arena_slab_live(arena.a) == 0);

    cc_arena_free(&arena);
}

static void run_release_suite(ArenaFactory make_arena) {
    test_direct_release(make_arena);
    test_vec_release_on_growth(make_arena);
    test_string_release_on_growth(make_arena);
    test_map_release_on_resize_and_destroy(make_arena);
}

int main(void) {
    test_empty_heap_overflow_rejected();
    run_release_suite(make_fixed_root_arena);
    run_release_suite(make_heap_overflow_arena);
    cc_std_out_write(cc_slice_from_buffer("arena release container smoke ok\n", 34));
    return 0;
}
