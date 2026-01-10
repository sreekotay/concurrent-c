#include "cc_arena.cch"
#include "cc_slice.cch"
#include "std/string.cch"

#include <string.h>

static char *cc_string_alloc(CCArena *arena, size_t cap) {
    if (!arena || cap == 0) {
        return NULL;
    }
    return (char *)cc_arena_alloc(arena, cap, sizeof(char));
}

CCString cc_string_new(CCArena *arena, size_t initial_cap) {
    size_t cap = initial_cap ? initial_cap : 64;
    char *buf = cc_string_alloc(arena, cap);
    if (!buf) {
        CCString empty = {0};
        return empty;
    }
    buf[0] = '\0';
    CCString s = {.ptr = buf, .len = 0, .cap = cap, .arena = arena};
    return s;
}

CCString cc_string_from_slice(CCArena *arena, CCSlice slice) {
    CCString s = cc_string_new(arena, slice.len + 1);
    if (!s.ptr) {
        return s;
    }
    if (slice.ptr && slice.len) {
        memcpy(s.ptr, slice.ptr, slice.len);
    }
    s.len = slice.len;
    s.ptr[s.len] = '\0';
    return s;
}

int cc_string_reserve(CCArena *arena, CCString *str, size_t need) {
    if (!str) {
        return -1;
    }
    CCArena* a = arena ? arena : str->arena;
    if (!a) return -1;
    size_t required = need + 1;
    if (str->cap >= required) {
        return 0;
    }
    size_t new_cap = str->cap ? str->cap : 64;
    while (new_cap < required) {
        new_cap *= 2;
    }
    char *new_buf = cc_string_alloc(a, new_cap);
    if (!new_buf) {
        return -1;
    }
    if (str->ptr && str->len) {
        memcpy(new_buf, str->ptr, str->len);
    }
    new_buf[str->len] = '\0';
    str->ptr = new_buf;
    str->cap = new_cap;
    str->arena = a;
    return 0;
}

int cc_string_append_slice(CCArena *arena, CCString *str, CCSlice data) {
    if (!str) {
        return -1;
    }
    CCArena* a = arena ? arena : str->arena;
    if (!a) return -1;
    size_t new_len = str->len + data.len;
    if (cc_string_reserve(a, str, new_len) != 0) {
        return -1;
    }
    if (data.ptr && data.len) {
        memcpy(str->ptr + str->len, data.ptr, data.len);
    }
    str->len = new_len;
    str->ptr[str->len] = '\0';
    str->arena = a;
    return 0;
}

int cc_string_append_cstr(CCArena *arena, CCString *str, const char *cstr) {
    if (!cstr) {
        return 0;
    }
    CCSlice slice = cc_slice_from_buffer((void *)cstr, strlen(cstr));
    return cc_string_append_slice(arena ? arena : (str ? str->arena : NULL), str, slice);
}

CCSlice cc_string_as_slice(const CCString *str) {
    if (!str || !str->ptr) {
        return cc_slice_empty();
    }
    return cc_slice_from_parts(str->ptr, str->len, CC_SLICE_ID_NONE, str->cap);
}

const char *cc_string_cstr(CCString *str) {
    if (!str || !str->ptr) {
        return NULL;
    }
    str->ptr[str->len] = '\0';
    return str->ptr;
}

#ifdef CC_ENABLE_SHORT_NAMES
CCString string_new(CCArena *arena) { return cc_string_new(arena, 0); }
int string_append(CCString *str, const char *cstr) { return cc_string_append_cstr(str ? str->arena : NULL, str, cstr); }
CCSlice string_as_slice(const CCString *str) { return cc_string_as_slice(str); }
#endif

