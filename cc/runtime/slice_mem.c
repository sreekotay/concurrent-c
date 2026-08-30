/* Slice libc isolate. Linked via concurrent_c.c so memcpy / memmove /
 * memset / malloc / strtod stay in this TU.
 *
 * Host C: rewrite only remaps includes; Result sugar is already lowered.
 * Do not include <ccc/std/slice.cch> — its CC__ARENA_HANDLE macros would
 * rewrite these definitions. */
#include <ccc/cc_slice.cch>
#include <ccc/cc_arena.cch>
#include <ccc/cc_result.cch>
#include <ccc/std/slice_packed.cch>
#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifndef CCResult_CCSlice_CCError_DEFINED
#define CCResult_CCSlice_CCError_DEFINED 1
CC_DECL_RESULT_SPEC(CCResult_CCSlice_CCError, CCSlice, CCError)
#endif
#ifndef CCResult_CCSliceHdr_CCError_DEFINED
#define CCResult_CCSliceHdr_CCError_DEFINED 1
CC_DECL_RESULT_SPEC(CCResult_CCSliceHdr_CCError, CCSliceHdr, CCError)
#endif
#ifndef CCResult_double_CCError_DEFINED
#define CCResult_double_CCError_DEFINED 1
CC_DECL_RESULT_SPEC(CCResult_double_CCError, double, CCError)
#endif
#ifndef CCResult_charptr_CCError_DEFINED
#define CCResult_charptr_CCError_DEFINED 1
CC_DECL_RESULT_SPEC(CCResult_charptr_CCError, char*, CCError)
#endif
#ifndef CCResult_CCSlice_uint32_t_CCError_DEFINED
#define CCResult_CCSlice_uint32_t_CCError_DEFINED 1
CC_DECL_RESULT_SPEC(CCResult_CCSlice_uint32_t_CCError, CCSlice_uint32_t, CCError)
#endif

#ifndef CC_SLICE_TO_F64_STACK_MAX
#define CC_SLICE_TO_F64_STACK_MAX 256
#endif

CCResult_bool_CCError cc_slice_copy(CCSlice* dst, CCSlice src) {
    if (!dst || (dst->len && !dst->ptr) || (src.len && !src.ptr)) {
        return cc_err_CCResult_bool_CCError(
            CC_ERROR(CC_ERR_INVALID_ARG, "slice copy: null"));
    }
    if (src.len > dst->len) {
        return cc_err_CCResult_bool_CCError(
            CC_ERROR(CC_ERR_INVALID_ARG, "slice copy: dest too short"));
    }
    if (src.len) memcpy(dst->ptr, src.ptr, src.len);
    return cc_ok_CCResult_bool_CCError(true);
}

CCResult_bool_CCError cc_slice_copy_overlap(CCSlice* dst, CCSlice src) {
    if (!dst || (dst->len && !dst->ptr) || (src.len && !src.ptr)) {
        return cc_err_CCResult_bool_CCError(
            CC_ERROR(CC_ERR_INVALID_ARG, "slice copy_overlap: null"));
    }
    if (src.len > dst->len) {
        return cc_err_CCResult_bool_CCError(
            CC_ERROR(CC_ERR_INVALID_ARG, "slice copy_overlap: dest too short"));
    }
    if (src.len) memmove(dst->ptr, src.ptr, src.len);
    return cc_ok_CCResult_bool_CCError(true);
}

CCResult_bool_CCError cc_slice_fill(CCSlice* dst, char c) {
    if (!dst || (dst->len && !dst->ptr)) {
        return cc_err_CCResult_bool_CCError(
            CC_ERROR(CC_ERR_INVALID_ARG, "slice fill: null"));
    }
    if (dst->len) memset(dst->ptr, (unsigned char)c, dst->len);
    return cc_ok_CCResult_bool_CCError(true);
}

CCResult_CCSlice_CCError (cc_slice_clone_into)(CCSlice* src, CCArena arena) {
    CCSlice stable;
    if (!src || !cc_arena_is_live(arena)) {
        return cc_err_CCResult_CCSlice_CCError(
            CC_ERROR(CC_ERR_INVALID_ARG, "clone_into without arena"));
    }
    if (src->len == 0) {
        return cc_ok_CCResult_CCSlice_CCError(cc_slice_empty());
    }
    stable = cc_arena_alloc_slice_bytes(arena, src->len);
    if (!stable.ptr) {
        return cc_err_CCResult_CCSlice_CCError(
            CC_ERROR(CC_ERR_OUT_OF_MEMORY, "clone slice"));
    }
    memcpy(stable.ptr, src->ptr, src->len);
    return cc_ok_CCResult_CCSlice_CCError(stable);
}

CCResult_CCSlice_CCError (cc_slice_to_c)(CCSlice* s, CCArena arena) {
    CCSlice src;
    CCSlice raw;
    if (!s) {
        return cc_err_CCResult_CCSlice_CCError(
            CC_ERROR(CC_ERR_INVALID_ARG, "to_c: null slice"));
    }
    src = *s;
    if (cc_slice_is_cstr(src) && src.ptr)
        return cc_ok_CCResult_CCSlice_CCError(src);
    if (!cc_arena_is_live(arena)) {
        return cc_err_CCResult_CCSlice_CCError(
            CC_ERROR(CC_ERR_INVALID_ARG, "to_c without arena"));
    }
    raw = cc_arena_alloc_slice_bytes(arena, src.len + 1);
    if (!raw.ptr) {
        return cc_err_CCResult_CCSlice_CCError(
            CC_ERROR(CC_ERR_OUT_OF_MEMORY, "to_c"));
    }
    if (src.len && src.ptr) memcpy(raw.ptr, src.ptr, src.len);
    raw.ptr[src.len] = 0;
    raw.len = src.len;
    raw.id |= CC_SLICE_ID_CSTR;
    return cc_ok_CCResult_CCSlice_CCError(raw);
}

CCResult_charptr_CCError (cc_slice_to_cstr)(CCSlice* s, CCArena arena) {
    CCResult_CCSlice_CCError z = cc_slice_to_c(s, arena);
    if (!z.ok) {
        return cc_err_CCResult_charptr_CCError(z.u.error);
    }
    return cc_ok_CCResult_charptr_CCError(z.u.value.ptr);
}

CCResult_CCSliceHdr_CCError (cc_slice_hdr_clone_into)(CCSliceHdr* src, CCArena arena) {
    CCSlice stable;
    CCSliceHdr out;
    if (!src || !cc_arena_is_live(arena)) {
        return cc_err_CCResult_CCSliceHdr_CCError(
            CC_ERROR(CC_ERR_INVALID_ARG, "clone_into without arena"));
    }
    if (src->len == 0) {
        out.ptr = NULL;
        out.len = 0;
        return cc_ok_CCResult_CCSliceHdr_CCError(out);
    }
    stable = cc_arena_alloc_slice_bytes(arena, src->len);
    if (!stable.ptr) {
        return cc_err_CCResult_CCSliceHdr_CCError(
            CC_ERROR(CC_ERR_OUT_OF_MEMORY, "clone slice hdr"));
    }
    memcpy(stable.ptr, src->ptr, src->len);
    out.ptr = stable.ptr;
    out.len = stable.len;
    return cc_ok_CCResult_CCSliceHdr_CCError(out);
}

CCResult_double_CCError cc_slice_to_f64(const CCSlice* s_ptr) {
    CCSlice s = s_ptr ? *s_ptr : cc_slice_empty();
    char stack_buf[CC_SLICE_TO_F64_STACK_MAX];
    char* buf;
    char* end = NULL;
    double v;
    int consumed_all, range;
    if (!s.ptr || s.len == 0) {
        return cc_err_CCResult_double_CCError(
            CC_ERROR(CC_ERR_PARSE, "to_f64: empty input"));
    }
    buf = (s.len + 1 <= sizeof(stack_buf)) ? stack_buf : (char*)malloc(s.len + 1);
    if (!buf) {
        return cc_err_CCResult_double_CCError(
            CC_ERROR(CC_ERR_OUT_OF_MEMORY, "to_f64: buffer"));
    }
    memcpy(buf, s.ptr, s.len);
    buf[s.len] = '\0';
    errno = 0;
    v = strtod(buf, &end);
    consumed_all = (end == buf + s.len);
    range = ((v == HUGE_VAL || v == -HUGE_VAL) && errno == ERANGE);
    if (buf != stack_buf) free(buf);
    if (!consumed_all) {
        return cc_err_CCResult_double_CCError(
            CC_ERROR(CC_ERR_PARSE, "to_f64: not a number"));
    }
    if (range) {
        return cc_err_CCResult_double_CCError(
            CC_ERROR(CC_ERR_INVALID_ARG, "to_f64: out of range"));
    }
    return cc_ok_CCResult_double_CCError(v);
}

CCResult_CCSlice_uint32_t_CCError (cc_slice_utf8_codepoints)(const CCSlice *s_ptr,
                                                            CCArena arena) {
    CCSlice s = s_ptr ? *s_ptr : cc_slice_empty();
    const unsigned char *p = (const unsigned char *)s.ptr;
    size_t i = 0, n = 0;
    uint32_t *cp;
    CCSlice base;
    CCSlice_uint32_t out;
    if (!cc_arena_is_live(arena)) {
        return cc_err_CCResult_CCSlice_uint32_t_CCError(
            CC_ERROR(CC_ERR_INVALID_ARG, "utf8_codepoints without arena"));
    }
    memset(&out, 0, sizeof(out));
    if (s.len == 0) {
        return cc_ok_CCResult_CCSlice_uint32_t_CCError(out);
    }
    if (!p) {
        return cc_err_CCResult_CCSlice_uint32_t_CCError(
            CC_ERROR(CC_ERR_INVALID_ARG, "utf8_codepoints: null pointer"));
    }
    base = cc_arena_alloc_slice(arena, sizeof(uint32_t), s.len, _Alignof(uint32_t));
    if (!base.ptr) {
        return cc_err_CCResult_CCSlice_uint32_t_CCError(
            CC_ERROR(CC_ERR_OUT_OF_MEMORY, "utf8_codepoints"));
    }
    cp = (uint32_t *)base.ptr;
    while (i + 8 <= s.len) {
        uint64_t w;
        memcpy(&w, p + i, 8);
        if (w & 0x8080808080808080ull) break;
        for (size_t k = 0; k < 8; k++) cp[n + k] = p[i + k];
        n += 8;
        i += 8;
    }
    while (i < s.len && p[i] < 0x80) cp[n++] = p[i++];
    while (i < s.len) {
        unsigned char c = p[i];
        uint32_t v;
        size_t more;
        size_t k;
        if (c < 0x80) {
            cp[n++] = c;
            i++;
            continue;
        }
        if (c < 0xC2) {
            return cc_err_CCResult_CCSlice_uint32_t_CCError(
                CC_ERROR(CC_ERR_PARSE, "utf8_codepoints: invalid leading byte"));
        } else if (c < 0xE0) {
            v = c & 0x1Fu;
            more = 1;
        } else if (c < 0xF0) {
            v = c & 0x0Fu;
            more = 2;
        } else if (c < 0xF5) {
            v = c & 0x07u;
            more = 3;
        } else {
            return cc_err_CCResult_CCSlice_uint32_t_CCError(
                CC_ERROR(CC_ERR_PARSE, "utf8_codepoints: invalid leading byte"));
        }
        if (i + more >= s.len) {
            return cc_err_CCResult_CCSlice_uint32_t_CCError(
                CC_ERROR(CC_ERR_PARSE, "utf8_codepoints: truncated sequence"));
        }
        for (k = 1; k <= more; k++) {
            unsigned char cont = p[i + k];
            if ((cont & 0xC0u) != 0x80u) {
                return cc_err_CCResult_CCSlice_uint32_t_CCError(
                    CC_ERROR(CC_ERR_PARSE, "utf8_codepoints: bad continuation"));
            }
            v = (v << 6) | (uint32_t)(cont & 0x3Fu);
        }
        if (more == 1 && v < 0x80u) {
            return cc_err_CCResult_CCSlice_uint32_t_CCError(
                CC_ERROR(CC_ERR_PARSE, "utf8_codepoints: overlong encoding"));
        }
        if (more == 2 && v < 0x800u) {
            return cc_err_CCResult_CCSlice_uint32_t_CCError(
                CC_ERROR(CC_ERR_PARSE, "utf8_codepoints: overlong encoding"));
        }
        if (more == 3 && v < 0x10000u) {
            return cc_err_CCResult_CCSlice_uint32_t_CCError(
                CC_ERROR(CC_ERR_PARSE, "utf8_codepoints: overlong encoding"));
        }
        if (v >= 0xD800u && v <= 0xDFFFu) {
            return cc_err_CCResult_CCSlice_uint32_t_CCError(
                CC_ERROR(CC_ERR_PARSE, "utf8_codepoints: surrogate code point"));
        }
        if (v > 0x10FFFFu) {
            return cc_err_CCResult_CCSlice_uint32_t_CCError(
                CC_ERROR(CC_ERR_PARSE, "utf8_codepoints: code point out of range"));
        }
        cp[n++] = v;
        i += more + 1;
    }
    base.len = n;
    out.base = base;
    return cc_ok_CCResult_CCSlice_uint32_t_CCError(out);
}

CCResult_CCSlicePacked_CCError (cc_slice_to_packed)(CCSlice *src, CCArena arena) {
    CCSlice s;
    CCSlicePacked r;
    char *block;
    char *data;
    size_t need;
    if (!src) {
        return cc_err_CCResult_CCSlicePacked_CCError(
            CC_ERROR(CC_ERR_INVALID_ARG, "CCSlicePacked to_packed without slice"));
    }
    s = *src;
    if (s.len > (size_t)UINT32_MAX) {
        return cc_err_CCResult_CCSlicePacked_CCError(
            CC_ERROR(CC_ERR_INVALID_ARG, "CCSlicePacked length exceeds u32"));
    }
    if (s.len == 0) {
        return cc_ok_CCResult_CCSlicePacked_CCError(cc_slice_packed_empty());
    }
    if (s.len <= (size_t)CC_SLICE_PACKED_INLINE_MAX) {
        if (!s.ptr) {
            return cc_err_CCResult_CCSlicePacked_CCError(
                CC_ERROR(CC_ERR_INVALID_ARG, "CCSlicePacked inline without ptr"));
        }
        return cc_ok_CCResult_CCSlicePacked_CCError(
            cc__slice_packed_pack_inline((const char *)s.ptr, (uint32_t)s.len));
    }
    if (!cc_arena_is_live(arena)) {
        return cc_err_CCResult_CCSlicePacked_CCError(
            CC_ERROR(CC_ERR_INVALID_ARG, "CCSlicePacked heap form needs arena"));
    }
    if (!s.ptr) {
        return cc_err_CCResult_CCSlicePacked_CCError(
            CC_ERROR(CC_ERR_INVALID_ARG, "CCSlicePacked heap without ptr"));
    }
    need = sizeof(uint32_t) + s.len;
    block = (char *)cc_arena_alloc(arena, need, sizeof(uint32_t));
    if (!block) {
        return cc_err_CCResult_CCSlicePacked_CCError(
            CC_ERROR(CC_ERR_OUT_OF_MEMORY, "CCSlicePacked arena alloc failed"));
    }
    {
        uint32_t n = (uint32_t)s.len;
        memcpy(block, &n, sizeof(n));
    }
    data = block + sizeof(uint32_t);
    memcpy(data, s.ptr, s.len);
    if (((uintptr_t)data & 3u) != 0) {
        return cc_err_CCResult_CCSlicePacked_CCError(
            CC_ERROR(CC_ERR_INVALID_ARG, "CCSlicePacked heap data misaligned"));
    }
    r = cc__slice_packed_from_heap_data(data);
    return cc_ok_CCResult_CCSlicePacked_CCError(r);
}
