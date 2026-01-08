/*
 * Header-only string and slice helpers for Concurrent-C stdlib (phase 1).
 *
 * C ABI uses prefixed names (CCString, cc_string_*). Short aliases are
 * provided only via std/prelude.h when CC_ENABLE_SHORT_NAMES is defined.
 */
#ifndef CC_STD_STRING_H
#define CC_STD_STRING_H

#include <stddef.h>
#include <stdint.h>
#ifndef __has_include
#define __has_include(x) 0
#endif
#if __has_include(<stdbool.h>)
#include <stdbool.h>
#else
#ifndef __bool_true_false_are_defined
typedef int bool;
#define true 1
#define false 0
#define __bool_true_false_are_defined 1
#endif
#endif
#include <ctype.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <limits.h>
#include <alloca.h>
#include <stdint.h>
#include <math.h>

#include "../cc_runtime.h"

typedef struct {
    char *ptr;
    size_t len;
    size_t cap;
    CCArena *arena; // backing arena for growth
} CCString;

// Forward declare string type for helpers that return CCString later.
// ------------------------- Parse error enums ------------------------------
typedef enum {
    CC_I64_PARSE_INVALID_CHAR = 1,
    CC_I64_PARSE_OVERFLOW,
    CC_I64_PARSE_UNDERFLOW,
} CC_I64ParseError;

typedef enum {
    CC_U64_PARSE_INVALID_CHAR = 1,
    CC_U64_PARSE_OVERFLOW,
} CC_U64ParseError;

typedef enum {
    CC_F64_PARSE_INVALID_CHAR = 1,
    CC_F64_PARSE_OVERFLOW,
} CC_F64ParseError;

typedef enum {
    CC_BOOL_PARSE_INVALID_VALUE = 1,
} CC_BoolParseError;

CC_DECL_RESULT(CCResultI64Parse, int64_t, CC_I64ParseError)
CC_DECL_RESULT(CCResultU64Parse, uint64_t, CC_U64ParseError)
CC_DECL_RESULT(CCResultF64Parse, double, CC_F64ParseError)
CC_DECL_RESULT(CCResultBoolParse, bool, CC_BoolParseError)

// ------------------------- Slice helpers ----------------------------------

typedef struct {
    CCSlice *items;
    size_t len;
} CCSliceArray;

static inline size_t cc_slice_len(CCSlice s) { return s.len; }

static inline bool cc_slice_is_ascii(CCSlice s) {
    const unsigned char *p = (const unsigned char *)s.ptr;
    for (size_t i = 0; i < s.len; ++i) {
        if (p[i] > 0x7F) return false;
    }
    return true;
}

static inline bool cc_slice_get(CCSlice s, size_t idx, char *out) {
    if (idx >= s.len || !s.ptr) return false;
    if (out) *out = ((char *)s.ptr)[idx];
    return true;
}

// Use cc_slice_sub from cc_slice.h

static inline CCSlice cc_slice_clone(CCArena *arena, CCSlice s) {
    if (!arena || s.len == 0) return s.len == 0 ? cc_slice_empty() : s;
    void *buf = cc_arena_alloc(arena, s.len, sizeof(char));
    if (!buf) return cc_slice_empty();
    if (s.ptr && s.len) memcpy(buf, s.ptr, s.len);
    return cc_slice_from_parts(buf, s.len, CC_SLICE_ID_NONE, s.len);
}

static inline char *cc_slice_c_str(CCArena *arena, CCSlice s) {
    if (!arena) return NULL;
    char *buf = (char *)cc_arena_alloc(arena, s.len + 1, sizeof(char));
    if (!buf) return NULL;
    if (s.ptr && s.len) memcpy(buf, s.ptr, s.len);
    buf[s.len] = '\0';
    return buf;
}

static inline bool cc_slice_starts_with(CCSlice s, CCSlice prefix) {
    return s.len >= prefix.len && memcmp(s.ptr, prefix.ptr, prefix.len) == 0;
}

static inline bool cc_slice_ends_with(CCSlice s, CCSlice suffix) {
    if (s.len < suffix.len) return false;
    return memcmp((uint8_t *)s.ptr + (s.len - suffix.len), suffix.ptr, suffix.len) == 0;
}

static inline size_t cc_slice_index_of(CCSlice s, CCSlice needle, bool *found) {
    if (needle.len == 0 || needle.len > s.len) { if (found) *found = false; return 0; }
    const uint8_t *hay = (const uint8_t *)s.ptr;
    for (size_t i = 0; i + needle.len <= s.len; ++i) {
        if (memcmp(hay + i, needle.ptr, needle.len) == 0) {
            if (found) *found = true; return i;
        }
    }
    if (found) *found = false; return 0;
}

static inline size_t cc_slice_last_index_of(CCSlice s, CCSlice needle, bool *found) {
    if (needle.len == 0 || needle.len > s.len) { if (found) *found = false; return 0; }
    const uint8_t *hay = (const uint8_t *)s.ptr;
    for (size_t i = s.len - needle.len + 1; i-- > 0;) {
        if (memcmp(hay + i, needle.ptr, needle.len) == 0) {
            if (found) *found = true; return i;
        }
        if (i == 0) break;
    }
    if (found) *found = false; return 0;
}

static inline size_t cc_slice_count(CCSlice s, CCSlice needle) {
    bool found = false;
    size_t idx = 0, cnt = 0;
    while (idx < s.len) {
        size_t pos = cc_slice_index_of(cc_slice_sub(s, idx, s.len), needle, &found);
        if (!found) break;
        cnt++;
        idx += pos + (needle.len ? needle.len : 1);
        if (needle.len == 0) break;
    }
    return cnt;
}

static inline size_t cc__trim_left_idx(CCSlice s) {
    size_t i = 0;
    const unsigned char *p = (const unsigned char *)s.ptr;
    while (i < s.len && isspace(p[i])) i++;
    return i;
}

static inline size_t cc__trim_right_idx(CCSlice s) {
    if (s.len == 0) return 0;
    const unsigned char *p = (const unsigned char *)s.ptr;
    size_t i = s.len;
    while (i > 0 && isspace(p[i-1])) i--;
    return i;
}

static inline CCSlice cc_slice_trim(CCSlice s) {
    size_t start = cc__trim_left_idx(s);
    CCSlice sub = cc_slice_sub(s, start, s.len);
    size_t end = cc__trim_right_idx(sub);
    return cc_slice_sub(sub, 0, end);
}

static inline CCSlice cc_slice_trim_left(CCSlice s) {
    return cc_slice_sub(s, cc__trim_left_idx(s), s.len);
}

static inline CCSlice cc_slice_trim_right(CCSlice s) {
    CCSlice sub = s;
    size_t end = cc__trim_right_idx(sub);
    return cc_slice_sub(sub, 0, end);
}

static inline bool cc__in_set(char c, CCSlice set) {
    const char *p = (const char *)set.ptr;
    for (size_t i = 0; i < set.len; ++i) {
        if (p[i] == c) return true;
    }
    return false;
}

static inline CCSlice cc_slice_trim_set(CCSlice s, CCSlice chars) {
    size_t start = 0, end = s.len;
    const char *p = (const char *)s.ptr;
    while (start < end && cc__in_set(p[start], chars)) start++;
    while (end > start && cc__in_set(p[end-1], chars)) end--;
    return cc_slice_sub(s, start, end);
}

static inline CCSliceArray cc_slice_split_all(CCArena *arena, CCSlice s, CCSlice delim) {
    CCSliceArray arr = {0};
    if (!arena) return arr;
    if (delim.len == 0) {
        arr.len = 1;
        arr.items = (CCSlice *)cc_arena_alloc(arena, sizeof(CCSlice), sizeof(void*));
        if (arr.items) arr.items[0] = s;
        return arr;
    }
    size_t parts = 1 + cc_slice_count(s, delim);
    arr.items = (CCSlice *)cc_arena_alloc(arena, parts * sizeof(CCSlice), sizeof(void*));
    if (!arr.items) return arr;
    size_t idx = 0; size_t out = 0;
    while (idx <= s.len && out < parts) {
        bool found = false;
        size_t pos = cc_slice_index_of(cc_slice_sub(s, idx, s.len), delim, &found);
        if (!found) {
            arr.items[out++] = cc_slice_sub(s, idx, s.len);
            break;
        }
        arr.items[out++] = cc_slice_sub(s, idx, idx + pos);
        idx += pos + delim.len;
    }
    arr.len = out;
    return arr;
}

// ------------------------- String builder ---------------------------------

CCString cc_string_new(CCArena *arena, size_t initial_cap);
CCString cc_string_from_slice(CCArena *arena, CCSlice slice);
int cc_string_reserve(CCArena *arena, CCString *str, size_t need);
int cc_string_append_slice(CCArena *arena, CCString *str, CCSlice data);
int cc_string_append_cstr(CCArena *arena, CCString *str, const char *cstr);
CCSlice cc_string_as_slice(const CCString *str);
const char *cc_string_cstr(CCString *str);

static inline int cc_string_append_char(CCArena *arena, CCString *str, char c) {
    CCSlice s = cc_slice_from_buffer(&c, 1);
    return cc_string_append_slice(arena, str, s);
}
static inline int cc_string_append_int(CCArena *arena, CCString *str, int64_t v) {
    char buf[32];
    int n = snprintf(buf, sizeof(buf), "%lld", (long long)v);
    if (n < 0) return -1;
    return cc_string_append_slice(arena, str, cc_slice_from_buffer(buf, (size_t)n));
}
static inline int cc_string_append_uint(CCArena *arena, CCString *str, uint64_t v) {
    char buf[32];
    int n = snprintf(buf, sizeof(buf), "%llu", (unsigned long long)v);
    if (n < 0) return -1;
    return cc_string_append_slice(arena, str, cc_slice_from_buffer(buf, (size_t)n));
}
static inline int cc_string_append_float(CCArena *arena, CCString *str, double v) {
    char buf[64];
    int n = snprintf(buf, sizeof(buf), "%g", v);
    if (n < 0) return -1;
    return cc_string_append_slice(arena, str, cc_slice_from_buffer(buf, (size_t)n));
}

// Convenience wrappers matching CC surface API (no explicit arena argument).
static inline CCString string_new(CCArena *arena) { return cc_string_new(arena, 0); }
static inline int string_append(CCString *str, const char *cstr) { return cc_string_append_cstr(str ? str->arena : NULL, str, cstr); }
static inline CCSlice string_as_slice(const CCString *str) { return cc_string_as_slice(str); }

// ------------------------- Parse helpers ----------------------------------

static inline CCResultI64Parse cc_slice_parse_i64(CCSlice s, int base) {
    if (!s.ptr || s.len == 0) return cc_err_CCResultI64Parse(CC_I64_PARSE_INVALID_CHAR);
    char *end = NULL;
    char *buf = (char *)alloca(s.len + 1);
    memcpy(buf, s.ptr, s.len); buf[s.len] = '\0';
    errno = 0;
    long long v = strtoll(buf, &end, base);
    if (end == buf) return cc_err_CCResultI64Parse(CC_I64_PARSE_INVALID_CHAR);
    if ((v == LLONG_MAX || v == LLONG_MIN) && errno == ERANGE) {
        return cc_err_CCResultI64Parse(v == LLONG_MAX ? CC_I64_PARSE_OVERFLOW : CC_I64_PARSE_UNDERFLOW);
    }
    return cc_ok_CCResultI64Parse((int64_t)v);
}

static inline CCResultU64Parse cc_slice_parse_u64(CCSlice s, int base) {
    if (!s.ptr || s.len == 0) return cc_err_CCResultU64Parse(CC_U64_PARSE_INVALID_CHAR);
    char *end = NULL;
    char *buf = (char *)alloca(s.len + 1);
    memcpy(buf, s.ptr, s.len); buf[s.len] = '\0';
    errno = 0;
    unsigned long long v = strtoull(buf, &end, base);
    if (end == buf) return cc_err_CCResultU64Parse(CC_U64_PARSE_INVALID_CHAR);
    if (v == ULLONG_MAX && errno == ERANGE) {
        return cc_err_CCResultU64Parse(CC_U64_PARSE_OVERFLOW);
    }
    return cc_ok_CCResultU64Parse((uint64_t)v);
}

static inline CCResultF64Parse cc_slice_parse_f64(CCSlice s) {
    if (!s.ptr || s.len == 0) return cc_err_CCResultF64Parse(CC_F64_PARSE_INVALID_CHAR);
    char *end = NULL;
    char *buf = (char *)alloca(s.len + 1);
    memcpy(buf, s.ptr, s.len); buf[s.len] = '\0';
    errno = 0;
    double v = strtod(buf, &end);
    if (end == buf) return cc_err_CCResultF64Parse(CC_F64_PARSE_INVALID_CHAR);
    if ((v == HUGE_VAL || v == -HUGE_VAL) && errno == ERANGE) {
        return cc_err_CCResultF64Parse(CC_F64_PARSE_OVERFLOW);
    }
    return cc_ok_CCResultF64Parse(v);
}

static inline CCResultBoolParse cc_slice_parse_bool(CCSlice s) {
    static const char *true_lit = "true";
    static const char *false_lit = "false";
    if (s.len == 4 && memcmp(s.ptr, true_lit, 4) == 0) {
        return cc_ok_CCResultBoolParse(true);
    }
    if (s.len == 5 && memcmp(s.ptr, false_lit, 5) == 0) {
        return cc_ok_CCResultBoolParse(false);
    }
    return cc_err_CCResultBoolParse(CC_BOOL_PARSE_INVALID_VALUE);
}

// ------------------------- Hash helpers ------------------------------------

static inline uint64_t cc_fnv1a64(const void *data, size_t len) {
    const uint8_t *p = (const uint8_t *)data;
    uint64_t hash = 14695981039346656037ULL;
    for (size_t i = 0; i < len; ++i) {
        hash ^= p[i];
        hash *= 1099511628211ULL;
    }
    return hash;
}

static inline uint64_t cc_slice_hash64(CCSlice s) {
    if (!s.ptr || s.len == 0) return 0xcbf29ce484222325ULL;
    return cc_fnv1a64(s.ptr, s.len);
}

static inline bool cc_slice_eq(CCSlice a, CCSlice b) {
    if (a.len != b.len) return false;
    if (a.ptr == b.ptr) return true;
    if (!a.ptr || !b.ptr) return false;
    return memcmp(a.ptr, b.ptr, a.len) == 0;
}

#endif // CC_STD_STRING_H
