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

typedef struct CcMacroDef CcMacroDef;

typedef struct CcSym {
    CcName name;
    CcDecl *decl;              /* the declaring node (in this unit or a header unit) */
    CcUnit *unit;              /* which unit declared it */
    enum { CC_SYM_FUNC = 1, CC_SYM_VAR, CC_SYM_TYPE, CC_SYM_ENUMERATOR, CC_SYM_MACRO } kind;
    CcType *type;              /* function type / variable type / aliased type */
    int is_header;             /* declared by an included header (or a macro expansion of one) */
    int is_definition;         /* function with a body / struct with fields / macro with a body */
    CcMacroDef *macro;         /* CC_SYM_MACRO: the parsed `#define` (all alternates) */
    struct CcSym *next;        /* hash chain */
} CcSym;

/* A `#define`, as text: object-like or function-like, with its alternates
 * (the same name defined again under another `#if` branch). Bodies are
 * expanded for file-scope invocations so macro-made declarations
 * (CC_DECL_RESULT_SPEC, CC_DECL_SLICE, CC_MAP_DECL_ARENA ...) are indexed
 * like written ones. */
struct CcMacroDef {
    CcName name;
    int function_like;
    int variadic;
    const char **params;       /* function-like: parameter names */
    size_t n_params;
    const char *body;          /* body text, continuations joined, comments stripped */
    CcDecl *decl;
    CcUnit *unit;
    CcMacroDef *alt;           /* next definition of the same name */
};

/* One rule read out of a `.ufcs` hook body: `if (method == "m") return
 * emit("callee")`, `case "m": return @slice("callee")`. */
typedef struct CcUfcsRule {
    CcName method;
    CcName callee;
    int by_value;              /* returned through cc_ufcs_emit_value* */
    struct CcUfcsRule *next;
} CcUfcsRule;

/* A `@typehooks on Subject { ... }` registration. Subjects: an exact type
 * ("CCArena"), a pointer subject ("MyRes*"), a trailing-`*` family
 * ("Map_*"), or `*`. */
typedef struct CcHookReg {
    CcName subject;            /* as written */
    CcName base;               /* subject without the trailing `*` (or `_*`) */
    int any;                   /* `*` */
    int family;                /* `Base_*` */
    int ptr;                   /* `Base*` */
    CcDecl *decl;
    CcUnit *unit;
    CcName create_fn;          /* first create overload; "_new" style suffixes are kept as written */
    CcName create_fn2;
    CcName destroy_fn;
    CcName pre_destroy_fn;
    CcName ufcs_fn;            /* handler name, or "<lambda>" */
    CcExpr *ufcs_value;        /* the .ufcs expression */
    CcUfcsRule *rules;         /* literal rules read from the handler body */
    CcName ufcs_prefix;        /* `return concat("prefix_", method)`: composed callee prefix */
    int ufcs_prefix_by_value;
    int ufcs_rejects;          /* default path returns the empty slice: no composition after the rules */
    int ufcs_opaque;           /* handler body could not be read as rules (computed names) */
    int has_len, has_access, has_cast, has_niche, has_sink;
    CcName sink_fn;            /* .ufcs_sink callee: unresolved methods lower to sink(&recv, "m", n, args...) */
    struct CcHookReg *next;
} CcHookReg;

/* A method entry in a type's method set. `callee` is the C function to call;
 * `recv_by_ptr` says whether the receiver is passed as `&recv`. `source`
 * records where the mapping came from, for diagnostics. */
typedef struct CcMethod {
    CcName method;
    CcName callee;
    int recv_by_ptr;
    const char *source;        /* "typehooks", "DECL_UFCS", "Type_method", "cc_snake_method", "snake_method", "Result" */
    const char *origin;        /* where the mapping was read: the hook, registration or declaration site */
    const char *recv_path;     /* `as:` faces walked to reach the callee's object, dot-joined ("file", "a.file"); NULL when the receiver is it */
    CcSym *sym;                /* the callee's declaration */
    struct CcMethod *next;
} CcMethod;

typedef struct CcTypeInfo {
    CcName name;               /* canonical spelling ("CCString", "Map_int_int", "CCResult_int_CCError") */
    CcSym *sym;
    CcMethod *methods;         /* resolved so far (cache, in resolution order) */
    CcName destroy_fn;         /* from @typehooks .destroy, or NULL */
    CcName create_fn;          /* from @typehooks .create, or NULL */
    CcName pre_destroy_fn;
    CcHookReg *hooks;          /* the narrowest @typehooks registration that matches, or NULL */
    CcName family;             /* generic family for an instance ("Map" for Map_int_int), or NULL */
    CcTypeList targs;          /* the instance's type arguments */
    CcName ufcs_registered_by; /* "CC_MAP_DECL_UFCS" when a *_DECL_UFCS(Name) names this type */
    int instantiated;          /* the family factory / spec macro has been expanded for it */
    int is_result;             /* CCResult_T_E */
    CcType *result_value;
    CcType *result_err;
    int result_optional;
    int result_declared_in_header; /* spec comes from a CC_DECL_RESULT_SPEC in a header: do not re-emit */
    CcUnit *result_decl_unit;  /* the unit whose CC_DECL_RESULT_SPEC declared it, or NULL */
    uint32_t result_decl_line;
    struct CcTypeInfo *next;
} CcTypeInfo;

typedef struct CcIndexOpts {
    const char **include_dirs; /* NULL-terminated search path for <...> and "..." */
    const char *quote_dir;     /* directory of the unit for "..." includes */
} CcIndexOpts;

typedef struct CcIndex {
    CcArena *arena;
    CcDiag *diag;
    CcIntern *intern;
    CC_LIST(CcUnit) units;     /* the unit plus each included header unit, in include order */
    CC_LIST(const char) unit_is_header; /* parallel to units: "1" / "0" */
    CcSym **buckets;           /* symbol table */
    size_t n_buckets;
    CcTypeInfo **type_buckets;
    size_t n_type_buckets;
    /* Result specs the TU needs, in first-use order; each has name/value/err. */
    CC_LIST(CcTypeInfo) result_specs;
    /* Everything below is bookkeeping for dumps and for the resolution steps. */
    CC_LIST(CcSym) syms;       /* every symbol, in declaration order */
    CC_LIST(CcTypeInfo) types; /* every type info, in creation order */
    CC_LIST(CcHookReg) hooks;  /* every @typehooks registration, in declaration order */
    CC_LIST(CcDecl) factories; /* CC_GENERIC_FACTORY declarations */
    CC_LIST(CcUnit) factory_units;
    CC_LIST(const char) typedef_names; /* for the parser's known_types */
    CC_LIST(const char) loaded_paths;  /* canonical paths of loaded headers */
    CC_LIST(const char) loading_paths; /* headers whose includes are being loaded (cycle guard) */
    CcIndexOpts opts;          /* the include search path of the last load, for expansions that #include */
    int expansion_depth;       /* nesting of macro / factory expansions being indexed */
    uint32_t n_expansions;
    uint32_t n_expansion_errors; /* parse errors inside expansions (not reported as diagnostics) */
} CcIndex;

CcIndex *cc_index_new(CcArena *a, CcDiag *d, CcIntern *in);
/* Add a parsed unit; `is_header` units contribute declarations only. */
void cc_index_add_unit(CcIndex *ix, CcUnit *u, int is_header);
/* Resolve the `#include` lines of `u` that name `.cch` files, parse each in
 * header mode (once per path), and add them. System headers (`<stdio.h>`)
 * are not read; their names are unknown to the index, which is fine: C
 * names the lowerer does not need to reason about stay C. */
void cc_index_load_includes(CcIndex *ix, CcUnit *u, const CcIndexOpts *opts, const CcParseOpts *popts);
/* Same, from a lexed file that is not parsed yet: lets the driver load the
 * headers of a unit before parsing the unit itself, so the parser sees
 * every typedef name from the headers. */
void cc_index_preload_includes(CcIndex *ix, CcLexFile *f, const CcIndexOpts *opts, const CcParseOpts *popts);
/* NULL-terminated list of every typedef name the index knows, for CcParseOpts.known_types. */
const char **cc_index_known_types(CcIndex *ix);

CcSym      *cc_index_sym(const CcIndex *ix, CcName name);
CcTypeInfo *cc_index_type(const CcIndex *ix, CcName canonical);
/* Canonical spelling of a type node: "int", "unsigned long", "struct Foo",
 * "CCString", "CCResult_int_CCError", "Map_int_int", "CCSlice" for char[:]. */
CcName      cc_index_canon(CcIndex *ix, const CcType *t);
/* Method lookup for a receiver type; NULL when nothing declares it, with
 * `candidates` (arena string) listing the near misses for the diagnostic. */
/* How the receiver is written at the call site, for the bare-name tier.
   CC_RECV_PTR: the receiver expression is a pointer; CC_RECV_CONST: what
   it names is const. */
#define CC_RECV_PTR   1u
#define CC_RECV_CONST 2u
CcMethod   *cc_index_method_recv(CcIndex *ix, CcTypeInfo *recv, CcName method, unsigned recv_shape, const char **candidates);
#define cc_index_method(ix, recv, method, cand) cc_index_method_recv((ix), (recv), (method), 0u, (cand))
/* Register the Result spec for (value, err, optional) and return its info;
 * the printer emits the spec block once for every entry not declared in a header. */
CcTypeInfo *cc_index_result(CcIndex *ix, CcType *value, CcType *err, int optional);

/* The type info for a canonical name, created on first use (with its hooks,
 * family instantiation and registrations resolved). */
CcTypeInfo *cc_index_type_get(CcIndex *ix, CcName canonical);
/* Every method the type has, from every source, for dumps and for the
 * candidates list: fills `out` (arena list) and returns its count. */
size_t cc_index_methods_of(CcIndex *ix, CcTypeInfo *info, CcMethod ***out);
/* Expand a function-like macro invocation `NAME(args)` spanning `sp` in
 * `u` with its `#define`; NULL when NAME is not a known function-like macro.
 * Lets a caller learn what a statement-level macro (`cc_arena_stack(a, n)`)
 * declares. */
const char *cc_index_expand_call(CcIndex *ix, const CcUnit *u, CcSpan sp);
/* Logical location of a symbol's declaration. */
CcLoc cc_index_sym_loc(const CcSym *s);
/* `cc_<snake>` spelling of a type name: "CCJsVal" -> "cc_js_val", "RedisConn" -> "redis_conn". */
CcName cc_index_snake(CcIndex *ix, CcName type_name);
/* Does a call to this function never return (`_Noreturn`, `noreturn` attribute, a macro that spells one)? */
int cc_index_sym_noreturn(const CcSym *s);

#endif
