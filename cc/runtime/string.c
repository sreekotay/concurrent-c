#include <ccc/cc_arena.cch>
#include <ccc/cc_slice.cch>
#include <ccc/std/string.cch>

#undef cc_string_push_buffer
#undef cc_string_push_slice
#undef cc_string_push_cstr
#undef cc_string_push_char
#undef cc_string_push_int
#undef cc_string_push_uint
#undef cc_string_cstr
#undef cc_string_from_slice
#undef cc_string_with_capacity
#undef cc_string_reserve
#undef cc_string_release
#undef cc_slice_clone
#undef cc_slice_c_str
#undef cc_slice_concat_into
#undef cc_string_materialize_in
#undef CCString_materialize_in

#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

CCString cc_string_with_capacity(CCArena arena, size_t cap) {
    CCString s = cc_string_new();
    if (cap > CC_STRING_INLINE_CAP) {
        (void)cc_string_reserve(&s, cap, arena); /* failure poisons s */
    }
    return s;
}

CCString cc_string_from_slice(CCArena arena, CCSlice slice) {
    CCString s = cc_string_new();
    (void)cc_string_push(&s, slice, arena); /* failure poisons s */
    return s;
}

CCString* cc_string_push_buffer(CCString *str, const char *buffer, uint32_t len, CCArena arena) {
    char *dst;
    size_t new_len;
    if (!str || cc_string_failed(str)) return NULL;
    new_len = (size_t)str->len + (size_t)len;
    if (new_len > UINT32_MAX) { cc__string_poison(str); return NULL; }
    dst = cc_string_reserve(str, new_len + 1, arena);
    if (!dst) return NULL;
    if (buffer && len) {
        memcpy(dst + str->len, buffer, (size_t)len);
    }
    str->len = (uint32_t)new_len;
    dst[str->len] = '\0';
    return str;
}

CCString* cc_string_push_slice(CCString *str, CCSlice data, CCArena arena) {
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
    if (!str || cc_string_failed(str)) return cc_slice_empty();
    data = cc_string_data_const(str);
    if (!data) return cc_slice_empty();
    return cc_slice_from_parts(
        (void *)data,
        str->len,
        cc_slice_make_id(cc_string_provenance(str), false, false, false));
}

const char *cc_string_cstr(CCString *str, CCArena arena) {
    char *data;
    if (!str || cc_string_failed(str)) return NULL;
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

void cc__string_stack_overflow_abort(size_t need, size_t cap) {
    fprintf(stderr,
            "cc fatal: arena-less string-template overflowed its computed bound "
            "(need %zu, cap %zu) - compiler bug\n",
            need, cap);
    abort();
}

CCSlice (cc_slice_clone)(CCArena arena, CCSlice s) {
    CCSlice stable;
    if (!cc_arena_is_live(arena) || s.len == 0)
        return s.len == 0 ? cc_slice_empty() : s;
    stable = cc_arena_alloc_slice_bytes(arena, s.len);
    if (!stable.ptr) return cc_slice_empty();
    if (s.ptr && s.len) memcpy(stable.ptr, s.ptr, s.len);
    return stable;
}

char *cc_slice_c_str(CCArena arena, CCSlice s) {
    char *buf;
    if (!cc_arena_is_live(arena)) return NULL;
    buf = (char *)cc_arena_alloc(arena, s.len + 1, sizeof(char));
    if (!buf) return NULL;
    if (s.ptr && s.len) memcpy(buf, s.ptr, s.len);
    buf[s.len] = '\0';
    return buf;
}

CCSlice cc_slice_concat_into(void *dst, size_t cap, const CCSlice *parts, size_t count) {
    size_t total = cc_slice_concat_lenv(parts, count);
    uint8_t *out = (uint8_t *)dst;
    size_t off = 0;
    size_t i;
    if (total == 0) return cc_slice_empty();
    if (!out || cap < total) return cc_slice_empty();
    for (i = 0; i < count; ++i) {
        if (!parts[i].ptr || parts[i].len == 0) continue;
        memcpy(out + off, parts[i].ptr, parts[i].len);
        off += parts[i].len;
    }
    return cc_slice_from_parts(dst, total, CC_SLICE_ID_UNTRACKED);
}

#ifndef CC_SLICE_PARSE_STACK_MAX
#define CC_SLICE_PARSE_STACK_MAX 256
#endif

static char *cc__slice_parse_buf(CCSlice s, char *stack_buf, size_t stack_cap) {
    char *buf = (s.len + 1 <= stack_cap) ? stack_buf : (char *)malloc(s.len + 1);
    if (!buf) return NULL;
    memcpy(buf, s.ptr, s.len);
    buf[s.len] = '\0';
    return buf;
}

CCResult_int64_t_CC_I64ParseError cc_slice_parse_i64(CCSlice s, int base) {
    char stack_buf[CC_SLICE_PARSE_STACK_MAX];
    char *buf;
    char *end = NULL;
    long long v;
    int no_parse, range;
    if (!s.ptr || s.len == 0)
        return cc_err_CCResult_int64_t_CC_I64ParseError(CC_I64_PARSE_INVALID_CHAR);
    buf = cc__slice_parse_buf(s, stack_buf, sizeof(stack_buf));
    if (!buf)
        return cc_err_CCResult_int64_t_CC_I64ParseError(CC_I64_PARSE_INVALID_CHAR);
    errno = 0;
    v = strtoll(buf, &end, base);
    no_parse = (end == buf);
    range = ((v == LLONG_MAX || v == LLONG_MIN) && errno == ERANGE);
    if (buf != stack_buf) free(buf);
    if (no_parse)
        return cc_err_CCResult_int64_t_CC_I64ParseError(CC_I64_PARSE_INVALID_CHAR);
    if (range) {
        return cc_err_CCResult_int64_t_CC_I64ParseError(
            v == LLONG_MAX ? CC_I64_PARSE_OVERFLOW : CC_I64_PARSE_UNDERFLOW);
    }
    return cc_ok_CCResult_int64_t_CC_I64ParseError((int64_t)v);
}

CCResult_uint64_t_CC_U64ParseError cc_slice_parse_u64(CCSlice s, int base) {
    char stack_buf[CC_SLICE_PARSE_STACK_MAX];
    char *buf;
    char *end = NULL;
    unsigned long long v;
    int no_parse, range;
    if (!s.ptr || s.len == 0)
        return cc_err_CCResult_uint64_t_CC_U64ParseError(CC_U64_PARSE_INVALID_CHAR);
    buf = cc__slice_parse_buf(s, stack_buf, sizeof(stack_buf));
    if (!buf)
        return cc_err_CCResult_uint64_t_CC_U64ParseError(CC_U64_PARSE_INVALID_CHAR);
    errno = 0;
    v = strtoull(buf, &end, base);
    no_parse = (end == buf);
    range = (v == ULLONG_MAX && errno == ERANGE);
    if (buf != stack_buf) free(buf);
    if (no_parse)
        return cc_err_CCResult_uint64_t_CC_U64ParseError(CC_U64_PARSE_INVALID_CHAR);
    if (range)
        return cc_err_CCResult_uint64_t_CC_U64ParseError(CC_U64_PARSE_OVERFLOW);
    return cc_ok_CCResult_uint64_t_CC_U64ParseError((uint64_t)v);
}

CCResult_double_CC_F64ParseError cc_slice_parse_f64(CCSlice s) {
    char stack_buf[CC_SLICE_PARSE_STACK_MAX];
    char *buf;
    char *end = NULL;
    double v;
    int no_parse, range;
    if (!s.ptr || s.len == 0)
        return cc_err_CCResult_double_CC_F64ParseError(CC_F64_PARSE_INVALID_CHAR);
    buf = cc__slice_parse_buf(s, stack_buf, sizeof(stack_buf));
    if (!buf)
        return cc_err_CCResult_double_CC_F64ParseError(CC_F64_PARSE_INVALID_CHAR);
    errno = 0;
    v = strtod(buf, &end);
    no_parse = (end == buf);
    range = ((v == HUGE_VAL || v == -HUGE_VAL) && errno == ERANGE);
    if (buf != stack_buf) free(buf);
    if (no_parse)
        return cc_err_CCResult_double_CC_F64ParseError(CC_F64_PARSE_INVALID_CHAR);
    if (range)
        return cc_err_CCResult_double_CC_F64ParseError(CC_F64_PARSE_OVERFLOW);
    return cc_ok_CCResult_double_CC_F64ParseError(v);
}

CCResult_bool_CC_BoolParseError cc_slice_parse_bool(CCSlice s) {
    const char *p;
    if (s.len == 4 && s.ptr) {
        p = (const char *)s.ptr;
        if (p[0] == 't' && p[1] == 'r' && p[2] == 'u' && p[3] == 'e')
            return cc_ok_CCResult_bool_CC_BoolParseError(true);
    }
    if (s.len == 5 && s.ptr) {
        p = (const char *)s.ptr;
        if (p[0] == 'f' && p[1] == 'a' && p[2] == 'l' && p[3] == 's' && p[4] == 'e')
            return cc_ok_CCResult_bool_CC_BoolParseError(false);
    }
    return cc_err_CCResult_bool_CC_BoolParseError(CC_BOOL_PARSE_INVALID_VALUE);
}

