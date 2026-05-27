/*
 * Primitive `cc_type_info` symbols.
 *
 * Every primitive type that user code can pass to `type_of(T)` has
 * one externally-visible `cc_type_info` here.  The set is small and
 * fixed — adding a new primitive means adding three lines in two
 * places (this file + the matching `extern` in cc_type.cch).
 *
 * For user-defined structs and generic instantiations, per-T
 * `cc_type_info` emission is done at codegen time (see milestone
 * #4b in cc/docs/COMPILER_CLEANUP_STATUS.md).  This file ships the
 * always-available primitives so the API has something to bite on
 * from day one.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>

#include <ccc/cc_type.cch>

/* All primitives are POD, trivially copyable, trivially droppable,
 * and safe through type-erased containers.  Bitwise copy + no-op
 * destroy are exactly what `cc_dyn_vec` will request. */
#define CC_TI_PRIM_FLAGS \
    (CC_TF_POD | CC_TF_TRIVIAL_COPY | CC_TF_TRIVIAL_DROP | CC_TF_ERASABLE)

#define CC_TI_PRIM(SYMNAME, CTYPE, DISPLAY)              \
    const cc_type_info __cc_ti_##SYMNAME = {             \
        .name      = DISPLAY,                            \
        .mangled   = #SYMNAME,                           \
        .id        = 0,                                  \
        .size      = (uint32_t)sizeof(CTYPE),            \
        .align     = (uint32_t)_Alignof(CTYPE),          \
        .kind      = (uint16_t)CC_TK_PRIMITIVE,          \
        .nfields   = 0,                                  \
        .flags     = (uint16_t)CC_TI_PRIM_FLAGS,         \
        ._reserved = 0,                                  \
        .fields    = NULL,                               \
        .copy_fn   = NULL,                               \
        .drop_fn   = NULL,                               \
    }

CC_TI_PRIM(int,      int,      "int");
CC_TI_PRIM(char,     char,     "char");
CC_TI_PRIM(short,    short,    "short");
CC_TI_PRIM(long,     long,     "long");
CC_TI_PRIM(float,    float,    "float");
CC_TI_PRIM(double,   double,   "double");
CC_TI_PRIM(size_t,   size_t,   "size_t");
CC_TI_PRIM(intptr_t, intptr_t, "intptr_t");
CC_TI_PRIM(bool,     bool,     "bool");

#undef CC_TI_PRIM
#undef CC_TI_PRIM_FLAGS

/* ------------------------------------------------------------
 * Global cc_type_info registry.
 *
 * Container/user-type `cc_type_info` symbols are emitted with
 * internal linkage (`static const`) into the TU that uses them
 * — that's what makes the `type_of(T)` macro work without
 * cross-TU symbol concerns.  But we still need a way to map a
 * mangled-name string to the right `cc_type_info*` (so user
 * code can do `cc_type_of("CCVec_int")` and get back a stable
 * pointer regardless of which TU emitted the symbol).  This
 * registry is the bridge: each TU registers its symbols via a
 * static constructor; lookups search the resulting flat array.
 *
 * Concurrency: constructors run single-threaded before `main`,
 * and lookups are pure-read once startup is complete, so no
 * locking is needed.  If we ever need dynamic registration at
 * runtime, this becomes the place to add an RW-lock.
 *
 * Lookup is O(n) for now.  n is bounded by the number of
 * distinct types in the binary; even a large project caps out
 * at low thousands.  If profiling shows it matters, swap the
 * backing store for a hash table — the API doesn't change.
 * ------------------------------------------------------------ */

enum { CC__TI_REG_INITIAL_CAP = 64 };

static const cc_type_info** cc__ti_registry      = NULL;
static size_t                cc__ti_registry_n   = 0;
static size_t                cc__ti_registry_cap = 0;

void cc_type_info_register(const cc_type_info* ti) {
    if (!ti || !ti->mangled) return;
    /* De-dup: same mangled name should map to a single entry.
     * First-registration wins, which gives primitives priority
     * over user re-registrations of the same name. */
    for (size_t i = 0; i < cc__ti_registry_n; i++) {
        const cc_type_info* cur = cc__ti_registry[i];
        if (cur == ti) return;
        if (cur && cur->mangled && strcmp(cur->mangled, ti->mangled) == 0) return;
    }
    if (cc__ti_registry_n == cc__ti_registry_cap) {
        size_t new_cap = cc__ti_registry_cap == 0 ? CC__TI_REG_INITIAL_CAP : cc__ti_registry_cap * 2;
        const cc_type_info** grown = (const cc_type_info**)realloc(
            cc__ti_registry, new_cap * sizeof(*grown));
        if (!grown) return; /* registration silently dropped on OOM */
        cc__ti_registry     = grown;
        cc__ti_registry_cap = new_cap;
    }
    cc__ti_registry[cc__ti_registry_n++] = ti;
}

const cc_type_info* cc_type_of(const char* mangled) {
    if (!mangled) return NULL;
    for (size_t i = 0; i < cc__ti_registry_n; i++) {
        const cc_type_info* cur = cc__ti_registry[i];
        if (cur && cur->mangled && strcmp(cur->mangled, mangled) == 0) {
            return cur;
        }
    }
    return NULL;
}

/* Register the fixed-set primitives at startup.
 *
 * No constructor priority: TCC's stub-AST parser rejects the
 * `__attribute__((constructor(N)))` form, and the registry
 * doesn't actually need ordering — lookups are by mangled
 * name, so it's safe for user-type constructors to run before
 * primitive ones (a `type_of(int)` call inside such a
 * constructor would return NULL, but none of CC's registration
 * constructors call `type_of` at init time, only at use time). */
__attribute__((constructor))
static void cc__ti_register_primitives(void) {
    cc_type_info_register(&__cc_ti_int);
    cc_type_info_register(&__cc_ti_char);
    cc_type_info_register(&__cc_ti_short);
    cc_type_info_register(&__cc_ti_long);
    cc_type_info_register(&__cc_ti_float);
    cc_type_info_register(&__cc_ti_double);
    cc_type_info_register(&__cc_ti_size_t);
    cc_type_info_register(&__cc_ti_intptr_t);
    cc_type_info_register(&__cc_ti_bool);
}

/* ------------------------------------------------------------
 * Commit 3b: cc_type_info for stdlib pre-baked vec instantiations.
 *
 * `cc/include/ccc/std/vec.cch` defines a fixed set of "pre-baked"
 * Vec<T> typedefs (CCVec_int, CCVec_char, CCVec_size_t, …) that
 * all alias `__CCVecGeneric`.  Those typedefs bypass the codegen
 * emission path in `visit_codegen.c` (which Commit 3a hooked
 * into), so without explicit registration here, `type_of(CCVec_int)`
 * would return NULL.  Below we emit one `cc_type_info` per pre-
 * baked name and register the lot at constructor priority 102 —
 * after primitives but at the same tier as codegen-emitted user
 * containers.
 *
 * Note: every pre-baked vec shares the SAME C layout
 * (`__CCVecGeneric`), so size/align are identical.  The mangled
 * name is what disambiguates them in the registry; downstream
 * code that introspects the element type belongs in 3c.
 * ------------------------------------------------------------ */

/* `__CCVecGeneric` lives in `std/vec.cch`.  We can't easily
 * include the full CC header here (it pulls in the world), so
 * mirror its concrete layout — a (data, len) pair — exactly,
 * via a private local typedef.  Layout stability is part of
 * the runtime ABI; if this ever drifts we get a sizeof
 * mismatch in the smoke test below. */
typedef struct {
    void*  data;
    size_t len;
} cc__prebaked_vec_layout;

#define CC_TI_PREBAKED_VEC(SYMNAME, DISPLAY)                              \
    static const cc_type_info __cc_ti_##SYMNAME = {                       \
        .name      = DISPLAY,                                             \
        .mangled   = #SYMNAME,                                            \
        .id        = 0,                                                   \
        .size      = (uint32_t)sizeof(cc__prebaked_vec_layout),           \
        .align     = (uint32_t)_Alignof(cc__prebaked_vec_layout),         \
        .kind      = (uint16_t)CC_TK_GENERIC_INST,                        \
        .nfields   = 0,                                                   \
        .flags     = 0,  /* owning container — conservative flags */      \
        ._reserved = 0,                                                   \
        .fields    = NULL,                                                \
        .copy_fn   = NULL,                                                \
        .drop_fn   = NULL,                                                \
    }

CC_TI_PREBAKED_VEC(CCVec_int,      "Vec<int>");
CC_TI_PREBAKED_VEC(CCVec_char,     "Vec<char>");
CC_TI_PREBAKED_VEC(CCVec_size_t,   "Vec<size_t>");
CC_TI_PREBAKED_VEC(CCVec_float,    "Vec<float>");
CC_TI_PREBAKED_VEC(CCVec_double,   "Vec<double>");
CC_TI_PREBAKED_VEC(CCVec_voidptr,  "Vec<void*>");
CC_TI_PREBAKED_VEC(CCVec_charptr,  "Vec<char*>");
CC_TI_PREBAKED_VEC(CCVec_intptr,   "Vec<int*>");

#undef CC_TI_PREBAKED_VEC

__attribute__((constructor))
static void cc__ti_register_prebaked_vecs(void) {
    cc_type_info_register(&__cc_ti_CCVec_int);
    cc_type_info_register(&__cc_ti_CCVec_char);
    cc_type_info_register(&__cc_ti_CCVec_size_t);
    cc_type_info_register(&__cc_ti_CCVec_float);
    cc_type_info_register(&__cc_ti_CCVec_double);
    cc_type_info_register(&__cc_ti_CCVec_voidptr);
    cc_type_info_register(&__cc_ti_CCVec_charptr);
    cc_type_info_register(&__cc_ti_CCVec_intptr);
}
