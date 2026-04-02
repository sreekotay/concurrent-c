#include <ccc/cc_arena.cch>
#include <ccc/cc_slice.cch>
#include <ccc/std/string.cch>

#include <string.h>

CCString CCString_new(CCArena *arena) {
    CCString s = Vec_char_init(arena, 0);
    if (s.data) s.data[0] = '\0';
    return s;
}

CCString CCString_from_slice(CCArena *arena, CCSlice slice) {
    CCString s = CCString_new(arena);
    if (!s.data) return s;
    if (!CCString_push(&s, slice)) {
        CCString empty = {0};
        return empty;
    }
    return s;
}

CCString* CCString_push_slice(CCString *str, CCSlice data) {
    if (!str || !str->arena) return NULL;
    size_t new_len = str->len + data.len;
    if (Vec_char_reserve(str, new_len + 1) != 0) return NULL;
    if (data.ptr && data.len) {
        memcpy(str->data + str->len, data.ptr, data.len);
    }
    str->len = new_len;
    str->data[str->len] = '\0';
    return str;
}

CCString* CCString_clear(CCString *str) {
    if (!str) return NULL;
    str->len = 0;
    if (str->data) str->data[0] = '\0';
    return str;
}

CCSlice CCString_as_slice(const CCString *str) {
    if (!str || !str->data) {
        return cc_slice_empty();
    }
    return cc_slice_from_parts(
        str->data,
        str->len,
        cc_slice_make_id(str->provenance, false, false, false),
        str->cap
    );
}

const char *CCString_cstr(CCString *str) {
    if (!str || !str->data) {
        return NULL;
    }
    if (str->len + 1 > str->cap) {
        if (Vec_char_reserve(str, str->len + 1) != 0) return NULL;
    }
    str->data[str->len] = '\0';
    return str->data;
}

