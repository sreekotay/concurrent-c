/*
 * Slice ABI and helpers.
 *
 * Layout matches the codegen contract: {ptr,len,id,alen} (32 bytes on 64-bit).
 * - ptr  : data pointer
 * - len  : logical length of the view
 * - id   : provenance/uniqueness token (0 if not tracked)
 * - alen : available length from ptr to the end of the original allocation
 *
 * This header is intentionally small and header-only so it can be used both by
 * the runtime and generated C without additional linkage.
 */
#ifndef CC_SLICE_H
#define CC_SLICE_H

#ifndef __has_include
#define __has_include(x) 0
#endif
#if __has_include(<stdbool.h>)
#include <stdbool.h>
#else
/* Fallback for TCC environments without stdbool. */
#ifndef __bool_true_false_are_defined
typedef int bool;
#define true 1
#define false 0
#define __bool_true_false_are_defined 1
#endif
#endif
#include <stddef.h>
#include <stdint.h>

#ifndef CC_SLICE_ID_NONE
#define CC_SLICE_ID_NONE 0ULL
#endif

// Slice id layout: lower 3 bits are flags, upper bits carry provenance token.
#define CC_SLICE_FLAG_UNIQUE      (1ull << 0)
#define CC_SLICE_FLAG_TRANSFERABLE (1ull << 1)
#define CC_SLICE_FLAG_SUBSLICE    (1ull << 2)
#define CC_SLICE_ID_MASK          (~(uint64_t)0x7)

typedef struct {
    void *ptr;
    size_t len;
    uint64_t id;   // provenance | flags
    size_t alen;
} CCSlice;

static inline CCSlice cc_slice_empty(void) {
    CCSlice s = {0};
    return s;
}

static inline uint64_t cc_slice_make_id(uint64_t provenance, bool unique, bool transferable, bool is_sub) {
    uint64_t id = (provenance & CC_SLICE_ID_MASK);
    if (unique) id |= CC_SLICE_FLAG_UNIQUE;
    if (transferable) id |= CC_SLICE_FLAG_TRANSFERABLE;
    if (is_sub) id |= CC_SLICE_FLAG_SUBSLICE;
    return id;
}

static inline uint64_t cc_slice_clear_flags(uint64_t id, uint64_t flags) {
    return id & ~flags;
}

static inline CCSlice cc_slice_from_buffer(void *ptr, size_t len) {
    CCSlice s = {ptr, len, cc_slice_make_id(CC_SLICE_ID_NONE, true, true, false), len};
    return s;
}

static inline CCSlice cc_slice_from_parts(void *ptr,
                                          size_t len,
                                          uint64_t id,
                                          size_t available_len) {
    CCSlice s = {ptr, len, id, available_len};
    return s;
}

static inline bool cc_slice_is_empty(CCSlice s) {
    return s.len == 0;
}

static inline size_t cc_slice_capacity(CCSlice s) {
    return s.alen ? s.alen : s.len;
}

static inline CCSlice cc_slice_sub(CCSlice s, size_t start, size_t end) {
    if (start > end || end > s.len) {
        return cc_slice_empty();
    }
    uint8_t *base = (uint8_t *)s.ptr;
    CCSlice sub = {
        .ptr = base ? (void *)(base + start) : NULL,
        .len = end - start,
        .id = cc_slice_clear_flags(s.id, CC_SLICE_FLAG_UNIQUE) | CC_SLICE_FLAG_SUBSLICE,
        .alen = cc_slice_capacity(s) >= start ? cc_slice_capacity(s) - start : 0,
    };
    return sub;
}

#endif // CC_SLICE_H


