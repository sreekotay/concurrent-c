/*
 * `cc_dyn_vec` — single implementation, any element type.
 *
 * Dispatch is via `cc_type_info::size`, `align`, `copy_fn`,
 * `drop_fn`.  See `cc/include/ccc/cc_dyn_vec.cch` for the API
 * contract and the contrast with monomorphized `CC_VEC_DECL_ARENA`.
 *
 * Backing storage piggybacks on the existing `cc_vec_*` infra so
 * arena/realloc/header semantics line up perfectly with `Vec<T>`.
 */

#include <stdint.h>
#include <string.h>

#include <ccc/cc_dyn_vec.cch>
#undef cc_dyn_vec_init

int cc_dyn_vec_init(cc_dyn_vec* v, CCArena arena,
                    const cc_type_info* ti, size_t initial_cap) {
    if (!v || !cc_arena_is_live(arena) || !ti) return -1;
    v->ti = ti;
    return cc_vec_init(&v->base, arena, ti->size, ti->align, initial_cap);
}

size_t cc_dyn_vec_len(const cc_dyn_vec* v) {
    if (!v) return 0;
    return v->base.len;
}

void* cc_dyn_vec_at(const cc_dyn_vec* v, size_t idx) {
    if (!v || !v->base.data || idx >= v->base.len) return NULL;
    return (uint8_t*)v->base.data + (idx * (size_t)v->ti->size);
}

int cc_dyn_vec_reserve(cc_dyn_vec* v, size_t need) {
    if (!v || !v->ti) return -1;
    return cc_vec_reserve(&v->base, v->ti->size, v->ti->align, need);
}

int cc_dyn_vec_push(cc_dyn_vec* v, const void* elem) {
    if (!v || !v->ti || !elem) return -1;
    void* slot = cc_vec_push_slot(&v->base, v->ti->size, v->ti->align);
    if (!slot) return -1;
    if (v->ti->copy_fn) {
        v->ti->copy_fn(slot, elem);
    } else {
        memcpy(slot, elem, v->ti->size);
    }
    return 0;
}

int cc_dyn_vec_pop(cc_dyn_vec* v, void* out) {
    if (!v || !v->ti || v->base.len == 0) return -1;
    size_t last = v->base.len - 1;
    void* slot = (uint8_t*)v->base.data + (last * (size_t)v->ti->size);
    if (out) {
        /* Move semantics: caller takes ownership; bitwise copy is
         * always correct for moves regardless of copy_fn. */
        memcpy(out, slot, v->ti->size);
    } else {
        if (v->ti->drop_fn) v->ti->drop_fn(slot);
    }
    v->base.len = last;
    return 0;
}

void cc_dyn_vec_clear(cc_dyn_vec* v) {
    if (!v || !v->ti) return;
    if (v->ti->drop_fn) {
        for (size_t i = 0; i < v->base.len; i++) {
            void* slot = (uint8_t*)v->base.data + (i * (size_t)v->ti->size);
            v->ti->drop_fn(slot);
        }
    }
    v->base.len = 0;
}

void cc_dyn_vec_free(cc_dyn_vec* v) {
    if (!v) return;
    cc_dyn_vec_clear(v);
    v->base.data = NULL;
    v->base.len  = 0;
}
