#include <ccc/std/prelude.cch>
#include <ccc/std/map.cch>
#include <ccc/std/map_heap.cch>
#include <ccc/std/string.cch>

#include <stdio.h>
#include <string.h>

typedef struct {
    void *ptr;
    uint32_t len;
    uint32_t alen;
    uint64_t id;
} CompactSlice;

CC_MAP_DECL_SLICE(CompactSlice, SliceCompactMap);
CC_MAP_DECL_HEAP_SLICE(CompactSlice, SliceCompactHeapMap);

static inline CCSlice compact_as_slice(CompactSlice s) {
    return cc_slice_from_parts(s.ptr, (size_t)s.len, s.id, (size_t)s.alen);
}

static inline CompactSlice compact_from_slice(CCSlice s) {
    CompactSlice out;
    out.ptr = s.ptr;
    out.len = (uint32_t)s.len;
    out.alen = (uint32_t)cc_slice_capacity(s);
    out.id = s.id;
    return out;
}

static CCSlice clone_cstr(CCArena *arena, const char *s) {
    return cc_slice_clone(arena, cc_slice_from_cstr(s));
}

static CompactSlice clone_value(CCArena *arena, const char *s) {
    CCSlice src = cc_slice_from_cstr(s);
    size_t n = src.len + 1;
    char *buf = (char *)cc_arena_alloc(arena, n, 1);
    if (!buf) return (CompactSlice){0};
    memcpy(buf, s, src.len);
    buf[src.len] = '\0';
    return compact_from_slice(cc_slice_from_parts(buf, src.len, 0, n));
}

static int run_arena_map(void) {
    CCArena arena = cc_arena_heap(1024 * 64);
    if (!arena.base) return 1;

    SliceCompactMap *m = SliceCompactMap_init(&arena);
    if (!m) return 2;

    for (int i = 0; i < 2000; i++) {
        char key_buf[32];
        snprintf(key_buf, sizeof(key_buf), "key:%d", i);
        CCSlice key = clone_cstr(&arena, key_buf);
        CompactSlice value = clone_value(&arena, "seed");
        if (!key.ptr || !value.ptr) return 3;
        if (SliceCompactMap_insert(m, key, value) != 0) return 4;
    }

    for (int rep = 0; rep < 200000; rep++) {
        char key_buf[32];
        snprintf(key_buf, sizeof(key_buf), "key:%d", rep % 2000);
        CCSlice query = cc_slice_from_cstr(key_buf);
        CompactSlice *cell = SliceCompactMap_get_ptr(m, query);
        if (!cell) {
            fprintf(stderr, "arena map: missing cell at rep=%d\n", rep);
            return 5;
        }

        CompactSlice replacement = clone_value(&arena, "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx");
        if (!replacement.ptr) return 6;
        if (cell->ptr) (void)cc_arena_release(&arena, cell->ptr);
        *cell = replacement;

        cell = SliceCompactMap_get_ptr(m, query);
        if (!cell) {
            fprintf(stderr, "arena map: missing cell after replace at rep=%d\n", rep);
            return 7;
        }
        CCSlice s = compact_as_slice(*cell);
        if (s.len == 0 || !s.ptr) {
            fprintf(stderr, "arena map: bad replacement slice at rep=%d\n", rep);
            return 8;
        }
    }

    return 0;
}

static int run_heap_map(void) {
    CCArena arena = cc_arena_heap(1024 * 64);
    if (!arena.base) return 21;

    SliceCompactHeapMap *m = SliceCompactHeapMap_init();
    if (!m) return 22;

    for (int i = 0; i < 2000; i++) {
        char key_buf[32];
        snprintf(key_buf, sizeof(key_buf), "key:%d", i);
        CCSlice key = clone_cstr(&arena, key_buf);
        CompactSlice value = clone_value(&arena, "seed");
        if (!key.ptr || !value.ptr) return 23;
        if (SliceCompactHeapMap_insert(m, key, value) != 0) return 24;
    }

    for (int rep = 0; rep < 200000; rep++) {
        char key_buf[32];
        snprintf(key_buf, sizeof(key_buf), "key:%d", rep % 2000);
        CCSlice query = cc_slice_from_cstr(key_buf);
        CompactSlice *cell = SliceCompactHeapMap_get_ptr(m, query);
        if (!cell) {
            fprintf(stderr, "heap map: missing cell at rep=%d\n", rep);
            return 25;
        }

        CompactSlice replacement = clone_value(&arena, "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx");
        if (!replacement.ptr) return 26;
        if (cell->ptr) (void)cc_arena_release(&arena, cell->ptr);
        *cell = replacement;

        cell = SliceCompactHeapMap_get_ptr(m, query);
        if (!cell) {
            fprintf(stderr, "heap map: missing cell after replace at rep=%d\n", rep);
            return 27;
        }
        CCSlice s = compact_as_slice(*cell);
        if (s.len == 0 || !s.ptr) {
            fprintf(stderr, "heap map: bad replacement slice at rep=%d\n", rep);
            return 28;
        }
    }

    return 0;
}

int main(void) {
    int rc = run_arena_map();
    if (rc != 0) return rc;
    rc = run_heap_map();
    if (rc != 0) return rc;
    printf("map_slice_arena_churn_smoke: PASS\n");
    return 0;
}
