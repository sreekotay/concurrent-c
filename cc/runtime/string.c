#include <ccc/cc_arena.cch>
#include <ccc/cc_slice.cch>
#include <ccc/std/string.cch>

#include <string.h>

CCString CCString_new(CCArena *arena) {
    CCString s = {0};
    if (!arena) return s;
    s.cap = CC_STRING_INLINE_CAP;
    s.arena = arena;
    memset(s.inline_buf, 0, sizeof(s.inline_buf));
    return s;
}

CCString CCString_from_slice(CCArena *arena, CCSlice slice) {
    CCString s = CCString_new(arena);
    if (!s.arena) return s;
    if (!CCString_push(&s, slice)) {
        CCString empty = {0};
        return empty;
    }
    return s;
}

CCString* CCString_push_slice(CCString *str, CCSlice data) {
    char *dst;
    size_t new_len;
    if (!str || !str->arena) return NULL;
    new_len = (size_t)str->len + data.len;
    if (new_len > UINT32_MAX) return NULL;
    if (CCString_reserve(str, new_len + 1) != 0) return NULL;
    dst = cc_string_data(str);
    if (!dst) return NULL;
    if (data.ptr && data.len) {
        memcpy(dst + str->len, data.ptr, data.len);
    }
    str->len = (uint32_t)new_len;
    cc__string_sync_heap_len(str);
    dst[str->len] = '\0';
    return str;
}

CCString* CCString_clear(CCString *str) {
    char *dst;
    if (!str) return NULL;
    str->len = 0;
    cc__string_sync_heap_len(str);
    dst = cc_string_data(str);
    if (dst) dst[0] = '\0';
    return str;
}

CCSlice CCString_as_slice(const CCString *str) {
    const char *data;
    if (!str) return cc_slice_empty();
    data = cc_string_data_const(str);
    if (!data) return cc_slice_empty();
    return cc_slice_from_parts(
        (void *)data,
        str->len,
        cc_slice_make_id(CCString_provenance(str), false, false, false),
        str->cap
    );
}

const char *CCString_cstr(CCString *str) {
    char *data;
    if (!str) return NULL;
    if (str->len + 1 > str->cap) {
        if (CCString_reserve(str, str->len + 1) != 0) return NULL;
    }
    data = cc_string_data(str);
    if (!data) return NULL;
    data[str->len] = '\0';
    return data;
}

uint64_t CCString_provenance(const CCString *str) {
    CCVec v;
    if (!str) return 0;
    if (cc_string_is_inline(str)) return CC_SLICE_ID_UNTRACKED;
    if (!cc_string_heap_data_const(str)) return 0;
    v = cc__string_heap_view(str);
    return cc_vec_provenance(&v);
}

