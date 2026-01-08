/*
 * Thread-safe bump allocator for per-request arenas.
 *
 * API is intentionally minimal and C11-only. The implementation lives in
 * cc/runtime/arena.c.
 */
#ifndef CC_ARENA_H
#define CC_ARENA_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint8_t *base;
    size_t capacity;
    size_t offset;
    // padding for future flags; keeps struct 24 bytes on LP64
    uint32_t _reserved;
} CCArena;

// Allocation helpers --------------------------------------------------------

static inline size_t cc__align_up(size_t value, size_t align) {
    size_t a = align ? align : sizeof(void *);
    return (value + (a - 1)) & ~(a - 1);
}

// Initialize an arena with caller-provided backing storage.
// Returns 0 on success, non-zero on invalid parameters.
static inline int cc_arena_init(CCArena *arena, void *buffer, size_t capacity) {
    if (!arena || !buffer || capacity == 0) {
        return -1;
    }
    arena->base = (uint8_t *)buffer;
    arena->capacity = capacity;
    arena->offset = 0;
    arena->_reserved = 0;
    return 0;
}

// Allocate `size` bytes aligned to `align` (power-of-two, >=1).
// Returns NULL on exhaustion; no automatic growth.
static inline void *cc_arena_alloc(CCArena *arena, size_t size, size_t align) {
    if (!arena || !arena->base || size == 0) {
        return NULL;
    }
    size_t aligned_offset = cc__align_up(arena->offset, align);
    if (aligned_offset > arena->capacity) {
        return NULL;
    }
    size_t new_offset = aligned_offset + size;
    if (new_offset > arena->capacity) {
        return NULL;
    }
    void *ptr = arena->base + aligned_offset;
    arena->offset = new_offset;
    return ptr;
}

// Reset arena to empty. Does not free backing storage.
static inline void cc_arena_reset(CCArena *arena) {
    if (!arena) return;
    arena->offset = 0;
}

// Convenience: compute how many bytes remain.
static inline size_t cc_arena_remaining(const CCArena *arena) {
    if (!arena || !arena->base || arena->capacity < arena->offset) {
        return 0;
    }
    return arena->capacity - arena->offset;
}

#endif // CC_ARENA_H


