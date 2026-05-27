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
#include <stdbool.h>

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
