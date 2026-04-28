#include <ccc/std/prelude.cch>
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
    static uint8_t buffer[8192];
    return cc_arena_create_buffer(buffer, sizeof(buffer), CC_ARENA_FIXED);
}

static void test_empty_heap_overflow_rejected(void) {
    CCArena arena = cc_arena_heap(0);
    assert(!arena.base);
    assert(!cc_arena_set_heap_overflow(&arena, true));
}

static CCArena make_heap_overflow_arena(void) {
    CCArena arena = cc_arena_heap(8192);
    assert(arena.base != NULL);
    arena.block_max = 1;
    assert(cc_arena_set_heap_overflow(&arena, true));
    return arena;
}

static void test_direct_release(ArenaFactory make_arena) {
    uint8_t buffer[512];
    CCArena arena = make_arena();
    (void)buffer;

    void *p = cc_arena_alloc(&arena, 32, 8);
    assert(p != NULL);
    assert(cc_atomic_load(&arena.live_allocs) == 1);

    assert(cc_arena_release(&arena, p));
    assert(cc_atomic_load(&arena.live_allocs) == 0);

    void *q = cc_arena_alloc(&arena, 32, 8);
    assert(q == p);
    assert(cc_atomic_load(&arena.live_allocs) == 1);

    cc_arena_free(&arena);
}

static void test_vec_release_on_growth(ArenaFactory make_arena) {
    CCArena arena = make_arena();

    IntVec v = IntVec_init(&arena, 2);
    assert(v.data != NULL);
    assert(cc_atomic_load(&arena.live_allocs) == 1);

    void *initial_data = v.data;
    assert(IntVec_push(&v, 10) == 0);
    assert(IntVec_push(&v, 20) == 0);
    assert(IntVec_push(&v, 30) == 0); /* forces growth */

    assert(v.data != NULL);
    assert(v.data != initial_data);
    assert(IntVec_len(&v) == 3);
    assert(cc_atomic_load(&arena.live_allocs) == 1);

    cc_arena_free(&arena);
}

static void test_string_release_on_growth(ArenaFactory make_arena) {
    CCArena arena = make_arena();

    CCString s = cc_string_new();
    assert(cc_atomic_load(&arena.live_allocs) == 0);

    void *initial_data = cc_string_data(&s);
    assert(cc_string_push(&s, "ab", &arena) != NULL);
    assert(cc_string_push(&s, "cdefghijklmnop", &arena) != NULL); /* forces growth */

    assert(cc_string_data(&s) != NULL);
    assert(cc_string_data(&s) != initial_data);
    assert(strcmp(cc_string_cstr(&s, &arena), "abcdefghijklmnop") == 0);
    assert(cc_atomic_load(&arena.live_allocs) == 1);

    cc_arena_free(&arena);
}

static void test_map_release_on_resize_and_destroy(ArenaFactory make_arena) {
    CCArena arena = make_arena();

    IntMap *m = IntMap_init(&arena);
    assert(m != NULL);
    assert(cc_atomic_load(&arena.live_allocs) == 1); /* map handle */

    for (int i = 0; i < 64; ++i) {
        assert(IntMap_insert(m, i, i + 100) == 0);
    }

    for (int i = 0; i < 64; ++i) {
        int *value = IntMap_get(m, i);
        assert(value && *value == i + 100);
    }

    /* Patched map core keeps one stable handle plus one table allocation. */
    assert(cc_atomic_load(&arena.live_allocs) == 2);

    IntMap_destroy(m);
    assert(cc_atomic_load(&arena.live_allocs) == 0);

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
