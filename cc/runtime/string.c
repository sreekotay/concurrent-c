#include <ccc/cc_arena.cch>
#include <ccc/cc_slice.cch>
#include <ccc/std/string.cch>

#include <string.h>

CCString cc_string_with_capacity(CCArena *arena, size_t cap) {
    CCString s = cc_string_new();
    if (cap > CC_STRING_INLINE_CAP) {
        if (!cc_string_reserve(&s, cap, arena)) {
            CCString empty = {0};
            return empty;
        }
    }
    return s;
}

CCString cc_string_from_slice(CCArena *arena, CCSlice slice) {
    CCString s = cc_string_new();
    if (!cc_string_push(&s, slice, arena)) {
        CCString empty = {0};
        return empty;
    }
    return s;
}

CCString* cc_string_push_buffer(CCString *str, const char *buffer, uint32_t len, CCArena *arena) {
    char *dst;
    size_t new_len;
    if (!str) return NULL;
    new_len = (size_t)str->len + (size_t)len;
    if (new_len > UINT32_MAX) return NULL;
    dst = cc_string_reserve(str, new_len + 1, arena);
    if (!dst) return NULL;
    if (buffer && len) {
        memcpy(dst + str->len, buffer, (size_t)len);
    }
    str->len = (uint32_t)new_len;
    dst[str->len] = '\0';
    return str;
}

CCString* cc_string_push_slice(CCString *str, CCSlice data, CCArena *arena) {
    if (data.len > UINT32_MAX) return NULL;
    return cc_string_push_buffer(str, (const char *)data.ptr, (uint32_t)data.len, arena);
}

CCString* cc_string_clear(CCString *str) {
    char *dst;
    if (!str) return NULL;
    str->len = 0;
    dst = cc_string_data(str);
    if (dst) dst[0] = '\0';
    return str;
}

CCSlice cc_string_as_slice(const CCString *str) {
    const char *data;
    if (!str) return cc_slice_empty();
    data = cc_string_data_const(str);
    if (!data) return cc_slice_empty();
    return cc_slice_from_parts(
        (void *)data,
        str->len,
        cc_slice_make_id(cc_string_provenance(str), false, false, false),
        str->cap
    );
}

const char *cc_string_cstr(CCString *str, CCArena *arena) {
    char *data;
    if (!str) return NULL;
    if (str->len + 1 > str->cap) {
        data = cc_string_reserve(str, str->len + 1, arena);
        if (!data) return NULL;
    } else {
        data = cc_string_data(str);
    }
    if (!data) return NULL;
    data[str->len] = '\0';
    return data;
}

uint64_t cc_string_provenance(const CCString *str) {
    CCStringHeapHeader *header;
    if (!str) return 0;
    if (cc_string_is_inline(str)) return CC_SLICE_ID_UNTRACKED;
    header = cc__string_heap_header(str);
    return header ? header->provenance : 0;
}

