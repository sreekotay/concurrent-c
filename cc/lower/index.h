/* Declaration index: what the lowerer knows about names, from declarations.
 *
 * Built from the unit itself and from every `.cch` it includes (each parsed
 * in header mode with the same parser). Answers the questions the lowering
 * pass asks, so the pass carries no tables of stdlib names:
 *   - is `f` a function, what does it return, is that a Result, of what?
 *   - is `T` a type, a typedef of what, a struct with which fields?
 *   - which method set does `x.m()` resolve against for a receiver of type
 *     `T` (type hooks, `CC_*_DECL_UFCS` registrations, `Type_m`, `cc_<snake>_m`)?
 *   - does `T` have a destroy hook, and what is it?
 *   - which argument of `f` is its arena (attribute on the declaration)?
 *   - may a call to `f` be discarded (its declared type is `T?>(E)`)?
 *   - what Result specs must this TU emit, and which come from headers?
 * Missing knowledge is a diagnostic at the use site, never a guess. */
#ifndef CC_LOWER_INDEX_H
#define CC_LOWER_INDEX_H
#include "ast.h"
#include "diag.h"
#include "parse.h"

typedef struct CcSym {
    CcName name;
    CcDecl *decl;              /* the declaring node (in this unit or a header unit) */
    CcUnit *unit;              /* which unit declared it */
    enum { CC_SYM_FUNC = 1, CC_SYM_VAR, CC_SYM_TYPE, CC_SYM_ENUMERATOR, CC_SYM_MACRO } kind;
    CcType *type;              /* function type / variable type / aliased type */
    struct CcSym *next;        /* hash chain */
} CcSym;

/* A method entry in a type's method set. `callee` is the C function to call;
 * `recv_by_ptr` says whether the receiver is passed as `&recv`. `source`
 * records where the mapping came from, for diagnostics. */
typedef struct CcMethod {
    CcName method;
    CcName callee;
    int recv_by_ptr;
    const char *source;        /* "@typehooks on T", "CC_MAP_DECL_UFCS", "Type_method", "cc_snake_method" */
    CcSym *sym;                /* the callee's declaration */
    struct CcMethod *next;
} CcMethod;

typedef struct CcTypeInfo {
    CcName name;               /* canonical spelling ("CCString", "Map_int_int", "CCResult_int_CCError") */
    CcSym *sym;
    CcMethod *methods;
    CcName destroy_fn;         /* from @typehooks .destroy, or NULL */
    CcName create_fn;          /* from @typehooks .create, or NULL */
    CcName pre_destroy_fn;
    int is_result;             /* CCResult_T_E */
    CcType *result_value;
    CcType *result_err;
    int result_optional;
    int result_declared_in_header; /* spec comes from a CC_DECL_RESULT_SPEC in a header: do not re-emit */
    struct CcTypeInfo *next;
} CcTypeInfo;

typedef struct CcIndex {
    CcArena *arena;
    CcDiag *diag;
    CcIntern *intern;
    CC_LIST(CcUnit) units;     /* the unit plus each included header unit, in include order */
    CcSym **buckets;           /* symbol table */
    size_t n_buckets;
    CcTypeInfo **type_buckets;
    size_t n_type_buckets;
    /* Result specs the TU needs, in first-use order; each has name/value/err. */
    CC_LIST(CcTypeInfo) result_specs;
} CcIndex;

typedef struct CcIndexOpts {
    const char **include_dirs; /* NULL-terminated search path for <...> and "..." */
    const char *quote_dir;     /* directory of the unit for "..." includes */
} CcIndexOpts;

CcIndex *cc_index_new(CcArena *a, CcDiag *d, CcIntern *in);
/* Add a parsed unit; `is_header` units contribute declarations only. */
void cc_index_add_unit(CcIndex *ix, CcUnit *u, int is_header);
/* Resolve the `#include` lines of `u` that name `.cch` files, parse each in
 * header mode (once per path), and add them. System headers (`<stdio.h>`)
 * are not read; their names are unknown to the index, which is fine: C
 * names the lowerer does not need to reason about stay C. */
void cc_index_load_includes(CcIndex *ix, CcUnit *u, const CcIndexOpts *opts, const CcParseOpts *popts);

CcSym      *cc_index_sym(const CcIndex *ix, CcName name);
CcTypeInfo *cc_index_type(const CcIndex *ix, CcName canonical);
/* Canonical spelling of a type node: "int", "unsigned long", "struct Foo",
 * "CCString", "CCResult_int_CCError", "Map_int_int", "CCSlice" for char[:]. */
CcName      cc_index_canon(CcIndex *ix, const CcType *t);
/* Method lookup for a receiver type; NULL when nothing declares it, with
 * `candidates` (arena string) listing the near misses for the diagnostic. */
CcMethod   *cc_index_method(CcIndex *ix, CcTypeInfo *recv, CcName method, const char **candidates);
/* Register the Result spec for (value, err, optional) and return its info;
 * the printer emits the spec block once for every entry not declared in a header. */
CcTypeInfo *cc_index_result(CcIndex *ix, CcType *value, CcType *err, int optional);

#endif
