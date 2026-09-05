/* The AST of the clean lowerer.
 *
 * One typed tree for a translation unit: declarations, statements,
 * expressions and types are separate node families with a tagged union
 * each. Every node records the token range it was parsed from (`first`,
 * `last`, inclusive indexes into CcLexFile.toks), so any node can be
 * printed back exactly, positioned for a diagnostic, or replaced by lowered
 * nodes that inherit its range. Nodes never own text: names are interned
 * pointers into the arena, literals point at their token.
 *
 * The parser builds this tree for the whole C11 surface the corpus uses
 * plus every Concurrent-C form; it does not type-check C. A construct the
 * parser cannot model is a diagnosed error, not a raw span. The one raw
 * node kind is CC_D_PP for preprocessor lines, which are opaque by design.
 *
 * Lowering rewrites CC nodes into C nodes in place (a CcExpr of kind
 * CC_E_UNWRAP becomes a CC_E_CALL, a CcStmt of kind CC_S_DEFER becomes
 * blocks), so the printer only ever sees C kinds plus the CC kinds it has
 * been told are printable verbatim. */
#ifndef CC_LOWER_AST_H
#define CC_LOWER_AST_H
#include <stddef.h>
#include <stdint.h>
#include "mem.h"
#include "lex.h"

typedef struct CcExpr CcExpr;
typedef struct CcStmt CcStmt;
typedef struct CcDecl CcDecl;
typedef struct CcType CcType;
typedef struct CcParam CcParam;
typedef struct CcInit CcInit;

typedef CC_LIST(CcExpr) CcExprList;
typedef CC_LIST(CcStmt) CcStmtList;
typedef CC_LIST(CcDecl) CcDeclList;
typedef CC_LIST(CcType) CcTypeList;
typedef CC_LIST(CcParam) CcParamList;
typedef CC_LIST(CcInit) CcInitList;

/* Token range of a node. `first`/`last` are inclusive indexes; a synthetic
 * node made by lowering copies the range of the node it replaces. */
typedef struct CcSpan { uint32_t first, last; } CcSpan;

/* An interned identifier. */
typedef const char *CcName;

/* ---- Types ---------------------------------------------------------- */

typedef enum CcTypeKind {
    CC_T_NAMED = 1,   /* int, unsigned long, struct tag, typedef name, _Bool ... : name + spec bits */
    CC_T_POINTER,     /* T*            base = T */
    CC_T_ARRAY,       /* T[n] / T[] / T[static n] / T[*]  base = T, size = n or NULL */
    CC_T_FUNC,        /* T (params)    base = return, params */
    CC_T_STRUCT,      /* struct/union [tag] { fields } or forward reference; fields NULL when not a definition */
    CC_T_ENUM,        /* enum [tag] { enumerators } */
    CC_T_TYPEOF,      /* __typeof__(expr) / typeof(T); also a comptime reflection member used as a type (`m.ret r = ...`) */
    CC_T_ATOMIC,      /* _Atomic(T) */
    /* Concurrent-C */
    CC_T_RESULT,      /* T!>(E) / T?>(E)    base = T, err = E, optional = ?> */
    CC_T_SLICE,       /* T[:] T[n:] T[:!] T[:0] T[:0!]   base = T, fixed = n or NULL, unique, sentinel */
    CC_T_CHAN,        /* T[~cap topo dir]   base = T, cap = expr or NULL, topology text, dir = '<' or '>' , ordered */
    CC_T_GENERIC,     /* Name::[args]       name, args */
    CC_T_SCOPED,      /* @scoped type T::[..]  (declaration form; see CC_D_SCOPED_TYPE) */
    CC_T_AUTO,        /* @auto(src)  in `@auto(src) name(arena) @destroy;`: typeof_expr = src */
    CC_T_MACRO,       /* NAME(args) used as a type (a macro expanding to a type, e.g. CCRes(T, E)): name, args */
    CC_T_VALUE        /* a value in a generic / macro type argument list (`SmallVec::[int, 8]`): value */
} CcTypeKind;

/* Storage class, qualifiers and function specifiers, as bits. */
enum {
    CC_Q_CONST = 1u << 0, CC_Q_VOLATILE = 1u << 1, CC_Q_RESTRICT = 1u << 2, CC_Q_ATOMIC = 1u << 3,
    CC_S_STATIC = 1u << 4, CC_S_EXTERN = 1u << 5, CC_S_TYPEDEF = 1u << 6, CC_S_REGISTER = 1u << 7,
    CC_S_AUTO = 1u << 8, CC_S_THREAD = 1u << 9, CC_F_INLINE = 1u << 10, CC_F_NORETURN = 1u << 11,
    /* Concurrent-C function markers */
    CC_F_ASYNC = 1u << 16, CC_F_BLOCKING = 1u << 17, CC_F_NONBLOCKING = 1u << 18,
    CC_F_LATENCY_SENSITIVE = 1u << 19, CC_F_COMPTIME = 1u << 20
};

/* An attribute as written: `__attribute__((...))`, `_Alignas(...)`, `as: name`,
 * `@tag:NAME`, or a CC declaration attribute. Kept as its token span plus a
 * parsed name so lowering can look for the ones it knows. Two more spellings
 * the corpus needs: a macro used as a specifier (`static_inline void f()`:
 * name = the macro's name, value NULL) and a preprocessor line between
 * specifiers (name = "#", span = the line). */
typedef struct CcAttr {
    CcSpan span;
    CcName name;        /* "__attribute__", "_Alignas", "as", "tag", ... */
    CcName value;       /* the single identifier argument when there is one, else NULL */
    struct CcAttr *next;
} CcAttr;

typedef struct CcField {
    CcSpan span;
    CcType *type;
    CcName name;        /* NULL for an anonymous struct/union member */
    CcExpr *bit_width;  /* NULL unless a bit-field */
    CcAttr *attrs;
    int is_pp;          /* a preprocessor line inside the member list: pp_tok, nothing else set */
    uint32_t pp_tok;
    struct CcField *next;
} CcField;

typedef struct CcEnumerator {
    CcSpan span;
    CcName name;
    CcExpr *value;      /* NULL when implicit */
    int is_pp;          /* a preprocessor line inside the enumerator list: pp_tok, nothing else set */
    uint32_t pp_tok;
    CcStmt *comptime;   /* a `@comptime for` / `@comptime if` inside the list that emits enumerators */
    struct CcEnumerator *next;
} CcEnumerator;

struct CcParam {
    CcSpan span;
    CcType *type;       /* NULL for `...` */
    CcName name;        /* NULL when unnamed */
    CcExpr *default_value; /* CC: `int x = 1` parameter default, else NULL */
    CcAttr *attrs;
    int is_variadic;
    int is_pp;          /* a preprocessor line inside the parameter list: pp_tok, nothing else set */
    uint32_t pp_tok;
};

struct CcType {
    CcTypeKind kind;
    CcSpan span;
    uint32_t quals;     /* CC_Q_* on this level (const applies to the pointer for CC_T_POINTER, etc.) */
    CcType *base;       /* pointee / element / return / result value */
    CcName name;        /* CC_T_NAMED: the full spelling, canonicalised ("unsigned long", "struct Foo", "MyType");
                           CC_T_STRUCT/ENUM: the tag or NULL; CC_T_GENERIC: the family name */
    /* CC_T_NAMED */
    int is_struct_kw, is_union_kw, is_enum_kw; /* `struct Foo` vs plain `Foo` */
    /* CC_T_ARRAY */
    CcExpr *size;
    int array_static;   /* T[static n] */
    int array_star;     /* T[*] */
    /* CC_T_FUNC */
    CcParamList params;
    int has_prototype;  /* `()` vs `(void)`: 0 for empty parens */
    /* CC_T_STRUCT / CC_T_ENUM */
    CcField *fields;
    CcEnumerator *enumerators;
    int is_union;
    int is_definition;
    /* CC_T_TYPEOF */
    CcExpr *typeof_expr;
    CcType *typeof_type;
    /* CC_T_RESULT */
    CcType *err;
    int optional;
    /* CC_T_SLICE */
    CcExpr *fixed;      /* T[n:] */
    int unique, sentinel;
    /* CC_T_CHAN */
    CcExpr *cap;
    CcName topology;    /* "1:1", "N:1", NULL */
    int dir;            /* '<' or '>' */
    int ordered;
    int sync;           /* T[~ ... sync ...]: blocking channel */
    CcName chan_policy; /* T[~cap dir, Policy]: the drop policy name, or NULL */
    int owned;          /* T[~cap owned { hooks }]: owned channel; chan_hooks is the hook initializer list */
    CcInit *chan_hooks;
    /* CC_T_VALUE */
    CcExpr *value;
    /* CC_T_GENERIC */
    CcTypeList args;
    CcAttr *attrs;
};

/* ---- Expressions ------------------------------------------------------ */

typedef enum CcExprKind {
    CC_E_IDENT = 1,   /* name */
    CC_E_NUMBER,      /* token text */
    CC_E_CHAR,
    CC_E_STRING,      /* one or more adjacent literal tokens: strings list */
    CC_E_PAREN,       /* (e) */
    CC_E_UNARY,       /* op e  : op in {+ - ! ~ * & ++ -- sizeof _Alignof}; postfix ++/-- flagged */
    CC_E_BINARY,      /* a op b */
    CC_E_ASSIGN,      /* a op= b */
    CC_E_TERNARY,     /* c ? a : b */
    CC_E_CALL,        /* callee(args) */
    CC_E_INDEX,       /* a[i] */
    CC_E_MEMBER,      /* a.name / a->name */
    CC_E_CAST,        /* (T) e */
    CC_E_SIZEOF_TYPE, /* sizeof(T) / _Alignof(T) */
    CC_E_COMPOUND,    /* (T){ init } */
    CC_E_GENERIC_SEL, /* _Generic(e, T: e, default: e) */
    CC_E_STMT_EXPR,   /* ({ stmts }) GNU; also a bare `{ stmts }` block handed to a macro argument (`repeat8({ ... })`) */
    CC_E_COMMA,       /* a, b */
    /* Concurrent-C */
    CC_E_UFCS,        /* recv.method(args) or recv.method::[targs](args); recv NULL for a bare `.method` is not allowed */
    CC_E_TYPE_SCOPED, /* Type.fn(args) — receiver is a type name */
    CC_E_GENERIC_FN,  /* name::[targs](args), e.g. vec_new::[int](a) */
    CC_E_UNWRAP,      /* e !>          (statement or expression position; handler = errhandler) */
    CC_E_UNWRAP_BODY, /* e !> body  /  e !>(e) body   body is a CcStmt (block or single) */
    CC_E_UNWRAP_OR,   /* e ?> default  /  e ?>(e) default */
    CC_E_UNWRAP_DESTROY, /* e !> @destroy [{ D }]  (declaration initializer form; the enclosing CcDecl also
                            gets destroy = 1 and destroy_body = the block, so lifetime sugar has one home) */
    CC_E_TEMPLATE,    /* @string(`...`, arena) / @string(policy, `...`, arena) / @string(`...`) / @string(expr, arena) */
    CC_E_SLICE_LIT,   /* @slice("...") */
    CC_E_SCRATCH,     /* @scratch / @scratch(N) as an arena operand */
    CC_E_CLOSURE,     /* (params) => [captures] body   ; body is a CC_S_BLOCK, or a CC_S_EXPR statement wrapping
                         the expression body (`x => x + 1`); a lone untyped parameter needs no parentheses */
    CC_E_AWAIT,       /* @await e */
    CC_E_COMPTIME,    /* @comptime(e) value position */
    CC_E_CREATE,      /* @create(args) / name@(args) constructor sugar; callee type from context */
    CC_E_EMIT,        /* @emit(`...`, arena) inside comptime */
    CC_E_VARIANT_LIT, /* .arm(args) / .arm  as a value (variant construction) */
    CC_E_CALL_MODE,   /* @blocking e / @nonblocking e / @noblock e (call-site mode) */
    CC_E_MOVE,        /* cc_move(x) is an ordinary call; this kind is reserved */
    CC_E_RANGE,       /* lo..hi (only inside for-in / @parallel for headers) */
    CC_E_PP,          /* a preprocessor line inside an argument / _Generic arm list: tok */
    CC_E_TYPE_ARG     /* a type in argument position: offsetof(T, m), cc_ok(T, v), cc_unwrap_as(r, T): type */
} CcExprKind;

typedef enum CcOp {
    CC_OP_NONE = 0,
    CC_OP_ADD, CC_OP_SUB, CC_OP_MUL, CC_OP_DIV, CC_OP_MOD, CC_OP_SHL, CC_OP_SHR,
    CC_OP_LT, CC_OP_GT, CC_OP_LE, CC_OP_GE, CC_OP_EQ, CC_OP_NE,
    CC_OP_AND, CC_OP_XOR, CC_OP_OR, CC_OP_LAND, CC_OP_LOR,
    CC_OP_NEG, CC_OP_POS, CC_OP_NOT, CC_OP_BITNOT, CC_OP_DEREF, CC_OP_ADDR,
    CC_OP_PREINC, CC_OP_PREDEC, CC_OP_POSTINC, CC_OP_POSTDEC, CC_OP_SIZEOF, CC_OP_ALIGNOF,
    CC_OP_ASSIGN, CC_OP_ADD_ASSIGN, CC_OP_SUB_ASSIGN, CC_OP_MUL_ASSIGN, CC_OP_DIV_ASSIGN,
    CC_OP_MOD_ASSIGN, CC_OP_SHL_ASSIGN, CC_OP_SHR_ASSIGN, CC_OP_AND_ASSIGN, CC_OP_XOR_ASSIGN,
    CC_OP_OR_ASSIGN
} CcOp;

/* One `${expr}` or `$~tag{expr}` slot or a literal run inside a template. */
typedef struct CcTplPart {
    CcSpan span;              /* within the template token; span.first == span.last == the template token */
    uint32_t off, len;        /* byte range of the part inside the file */
    int is_slot;
    int is_verbatim;          /* `${{ raw }}` span: a literal run whose bytes carry no escapes */
    CcName tag;               /* $~tag{...} */
    CcExpr *expr;             /* parsed slot expression (parsed from the slot text) */
    CcLexFile *file;          /* the slot's own token array: `expr` spans index it; its
                                 offsets, src and line table are the enclosing file's */
    struct CcTplPart *next;
} CcTplPart;

typedef struct CcCapture {
    CcSpan span;
    CcName name;
    int by_ref;               /* [&x] */
    int is_safe;              /* [@safe &x] */
    CcExpr *init;             /* [p = &x]: an init-capture, else NULL */
    struct CcCapture *next;
} CcCapture;

typedef struct CcGenericSelArm {
    CcType *type;             /* NULL for default */
    CcExpr *expr;
    int is_pp;                /* a preprocessor line between arms: pp_tok, nothing else set */
    uint32_t pp_tok;
    struct CcGenericSelArm *next;
} CcGenericSelArm;

struct CcExpr {
    CcExprKind kind;
    CcSpan span;
    CcOp op;                  /* UNARY / BINARY / ASSIGN */
    CcExpr *a, *b, *c;        /* operands: unary a; binary a,b; ternary a,b,c; call a=callee; index a,b; member a; cast a; ufcs a=recv */
    CcName name;              /* IDENT; MEMBER field; UFCS/TYPE_SCOPED/GENERIC_FN method or function; VARIANT_LIT arm; CALL_MODE mode ("blocking"...) */
    int arrow;                /* MEMBER: a->name */
    CcExprList args;          /* CALL / UFCS / TYPE_SCOPED / GENERIC_FN / CREATE / VARIANT_LIT / SLICE_LIT */
    CcTypeList targs;         /* UFCS / GENERIC_FN: ::[T...] */
    CcType *type;             /* CAST / SIZEOF_TYPE / COMPOUND / TYPE_SCOPED receiver type */
    CcInit *init;             /* COMPOUND */
    CcGenericSelArm *arms;    /* GENERIC_SEL (a = controlling expr) */
    CcStmt *body;             /* STMT_EXPR (block) / UNWRAP_BODY / UNWRAP_DESTROY (destroy block or NULL) / CLOSURE body */
    CcName binder;            /* UNWRAP_BODY / UNWRAP_OR: `(e)` error binder or NULL */
    /* TEMPLATE */
    CcExpr *tpl_policy;       /* @string(policy, `...`, arena) */
    CcTplPart *tpl_parts;     /* literal runs and slots; NULL for the direct form @string(expr, arena) */
    CcExpr *tpl_arena;        /* arena operand: an expression, a CC_E_SCRATCH, or NULL for the bounded stack form */
    /* SCRATCH */
    CcExpr *scratch_bytes;    /* @scratch(N) or NULL */
    /* CLOSURE */
    CcParamList params;
    CcCapture *captures;
    int closure_unsafe;       /* @unsafe () => ... */
    int closure_async;        /* async () => ... */
    /* CREATE */
    CcName create_var;        /* name@(args): the declared name (also the enclosing declarator) */
    /* literal tokens */
    uint32_t tok;             /* NUMBER / CHAR / STRING first token; IDENT token */
    uint32_t n_string_toks;   /* STRING: adjacent literals concatenated by C */
};

/* ---- Initializers ----------------------------------------------------- */

typedef struct CcDesignator {
    CcName field;             /* .field */
    CcExpr *index;            /* [i] */
    CcExpr *index_hi;         /* [lo ... hi] GNU */
    struct CcDesignator *next;
} CcDesignator;

struct CcInit {
    CcSpan span;
    CcDesignator *designators; /* NULL for positional */
    CcExpr *expr;             /* scalar initializer, or NULL when `list` is used */
    CcInitList list;          /* { ... } */
    int is_list;
    int is_pp;                /* a preprocessor line inside an initializer list: pp_tok, nothing else set */
    uint32_t pp_tok;
};

/* ---- Statements ------------------------------------------------------- */

typedef enum CcStmtKind {
    CC_S_EXPR = 1,    /* e; (e may be NULL for `;`); a macro loop `FOREACH(m, k, v) { ... }` / `try { ... }` keeps
                         its block in `body` (has_braces) */
    CC_S_DECL,        /* a declaration in statement position: decl */
    CC_S_BLOCK,       /* { stmts } */
    CC_S_IF,          /* if (cond) then [else] */
    CC_S_WHILE,
    CC_S_DO,
    CC_S_FOR,         /* for (init; cond; step) body  init is a decl or an expr */
    CC_S_SWITCH,      /* switch (e) body;  CC: @switch (e) with `case .arm(bind):` labels */
    CC_S_CASE,        /* case e: / case lo ... hi: / case .arm(bind): / default: — label stmt, `inner` is the labelled statement or NULL */
    CC_S_RETURN,
    CC_S_BREAK,
    CC_S_CONTINUE,
    CC_S_GOTO,
    CC_S_LABEL,       /* name: inner */
    CC_S_ASM,         /* __asm__ ... ; kept as a span */
    /* Concurrent-C */
    CC_S_DEFER,       /* @defer [name:] stmt / @defer(ok|err) stmt */
    CC_S_CANCEL_DEFER,/* @cancel_defer name; */
    CC_S_ERRHANDLER,  /* @errhandler(E e) stmt-or-block */
    CC_S_ERR_FWD,     /* @err(e); */
    CC_S_UNWRAP,      /* e !>;  and  e !> body;  as a statement (expr is CC_E_UNWRAP / CC_E_UNWRAP_BODY) */
    CC_S_FOR_IN,      /* for (x in xs) / @for (&x in xs) / @for (a, b in xs, ys) / for (i in lo..hi);
                         a `} !>` / `} !>(e) body` tail after the block is par_tail (as for @parallel) */
    CC_S_PARALLEL,    /* @parallel [spawn] [(pred)] [seq (cond)] { arms }  and  CCParallel h = @parallel {...}  (as expr: see CC_E_... no: statement with optional bind) */
    CC_S_PARALLEL_FOR,/* @parallel for (i in lo..hi) body  /  @parallel [seq (c)] wait (ts) for (i in lo..hi) [worker (w)] [cache (a, b)] body */
    CC_S_PARALLEL_DEST,/* @parallel(h) { stmts } */
    CC_S_SERIAL,      /* @serial { stmts } (inside @parallel) */
    CC_S_STAGE,       /* @stage (gate, args) { stmts } */
    CC_S_WITH_DEADLINE,/* @with_deadline(e) [as h] { } */
    CC_S_WITH,        /* @with(arena = a) { } */
    CC_S_CLOSING,     /* @closing(tx, ...) { } / @closing(tx) spawn(...) */
    CC_S_SPAWN_BLOCK, /* @spawn { } / @nursery closing(tx) { } */
    CC_S_MODE_BLOCK,  /* @nonblocking { } / @blocking { } */
    CC_S_UNSAFE,      /* unsafe { } */
    CC_S_COMPTIME_IF, /* @comptime if (c) { } [else { }] */
    CC_S_COMPTIME_BLOCK, /* @comptime { } (statement position) */
    CC_S_COMPTIME_FOR /* @comptime for (m in type_of(T).methods) { } */
} CcStmtKind;

typedef struct CcParallelArm {
    CcSpan span;
    CcName target;            /* `a = expr;` arm: a; NULL for `expr !>;` or @serial */
    CcExpr *expr;             /* the arm expression (NULL for @serial) */
    CcStmt *serial;           /* CC_S_SERIAL block, or any other statement written as an arm (a loop, a declaration) */
    int unwrap;               /* `expr !>;` */
    struct CcParallelArm *next;
} CcParallelArm;

struct CcStmt {
    CcStmtKind kind;
    CcSpan span;
    CcExpr *expr;             /* EXPR; IF/WHILE/DO/SWITCH cond; RETURN value; CASE value (lo); FOR cond; WITH_DEADLINE deadline; STAGE gate; UNWRAP; COMPTIME_IF cond */
    CcExpr *expr2;            /* FOR step; CASE hi (range); FOR_IN iterable (or range lo..hi as CC_E_RANGE) — the same
                                 node as exprs.items[0]; a parenthesised parameter list as a comptime sequence is a
                                 CC_E_TYPE_ARG over a CC_T_FUNC */
    CcDecl *decl;             /* DECL; FOR init decl */
    CcExpr *init_expr;        /* FOR init expression */
    CcStmt *body;             /* IF then; loops; SWITCH; LABEL/CASE inner; DEFER; ERRHANDLER; blocks: use `stmts` */
    CcStmt *else_body;        /* IF else; COMPTIME_IF else */
    CcStmtList stmts;         /* BLOCK / SERIAL / STAGE / WITH* / CLOSING / SPAWN_BLOCK / MODE_BLOCK / UNSAFE / COMPTIME_* bodies;
                                 FOR: the declarations after the first in `for (int i = 0, j = n; ...)`, as CC_S_DECL */
    CcName name;              /* GOTO/LABEL; DEFER name; CANCEL_DEFER; ERRHANDLER binder; ERR_FWD binder; FOR_IN first binder; WITH_DEADLINE `as h`; MODE_BLOCK mode; PARALLEL bind name (CCParallel h = ...) */
    CcName name2;             /* FOR_IN second binder (@for (a, b in ...)) */
    CcType *type;             /* ERRHANDLER error type; PARALLEL bind type */
    CcExprList exprs;         /* FOR_IN iterables (one or two); STAGE args; CLOSING channels */
    /* DEFER */
    int defer_on;             /* 0 always, 'o' ok, 'e' err */
    /* CASE */
    CcName case_arm;          /* case .arm(bind): */
    CcName case_bind;
    int is_default;
    int is_variant_switch;    /* @switch */
    /* FOR_IN */
    int by_ref;               /* @for (&x in xs) */
    int by_ref2;              /* @for (a, &b in xs, ys): the second binder is by reference */
    int is_at_for;            /* @for vs for */
    /* PARALLEL */
    CcParallelArm *arms;
    int par_spawn;            /* @parallel spawn */
    CcExpr *par_pred;         /* @parallel (pred) */
    CcExpr *par_seq;          /* seq (cond) */
    CcExpr *par_wait;         /* wait (ts) */
    CcName par_worker;        /* worker (w) */
    CcExprList par_cache;     /* cache (a, b) */
    CcExpr *par_dest;         /* @parallel(h) */
    CcExpr *par_target;       /* `lvalue = @parallel { ... }`: the assigned join target (an existing variable) */
    CcExpr *par_tail;         /* the `!>.wait()!>` chain after the block, as an expression over the join, or NULL;
                                 the join is a CC_E_IDENT named "@parallel" at the statement's first token */
    /* CLOSING */
    CcExpr *closing_spawn;    /* @closing(tx) spawn(...) — the spawn call */
    int has_braces;
};

/* ---- Declarations ----------------------------------------------------- */

typedef enum CcDeclKind {
    CC_D_VAR = 1,     /* T name [= init] [@destroy [{D}]] [@detach];  one declarator (the parser splits `int a, b;`) */
    CC_D_FUNC,        /* T name(params) body-or-; */
    CC_D_TYPEDEF,     /* typedef T name; */
    CC_D_TAGGED,      /* struct/union/enum definition or forward declaration as a declaration: type */
    CC_D_STATIC_ASSERT,
    CC_D_PP,          /* a preprocessor line: opaque token */
    CC_D_EMPTY,       /* ; */
    /* Concurrent-C */
    CC_D_TYPEHOOKS,   /* @typehooks on T { .create = f, ... }; */
    CC_D_TYPEVIEW,    /* @typeview [Name] on T { as: base; allow: ...; }; */
    CC_D_VARIANT,     /* @variant Name { arm(T x); ... }; */
    CC_D_GRAMMAR,     /* @grammar(engine) Name {~~~~ body ~~~~} — body kept as a span (fenced, opaque by design) */
    CC_D_GENERIC_FACTORY, /* CC_GENERIC_FACTORY(Name) { body } / _EXTEND */
    CC_D_COMPTIME_FN, /* @comptime T name(params) { body } */
    CC_D_COMPTIME_BLOCK, /* @comptime { } at file scope; also `@comptime for (...) { }` and `@comptime expr;` at file
                            scope, whose single statement is `body` */
    CC_D_COMPTIME_IF, /* @comptime if (c) { decls } [else { decls }] */
    CC_D_SCOPED_TYPE, /* @scoped type Name::[T]; */
    CC_D_PRAGMA_CC,   /* #pragma(@parallel) off — parsed from a PP token */
    CC_D_LINK,        /* @link("lib") */
    CC_D_TASK_DOC,    /* a CCDoc `@task` comment attached to the following function (kept as span) */
    CC_D_MACRO_CALL,  /* NAME(args) or NAME at file scope with no declarator (a macro invocation
                         such as CC_DECL_RESULT_SPEC(...)); expr is the call / ident, `;` optional */
    CC_D_STMT         /* a top-level statement in a script (.shcc): body */
} CcDeclKind;

typedef struct CcViewItem {    /* one item of a @typeview entry: `name`, `^name`, `out_*`, `(Type)name` */
    CcSpan span;
    CcName name;               /* the item spelling; `*` inside means a glob */
    int deny;                  /* `^name` */
    CcType *cast;              /* `(Type)name` */
    struct CcViewItem *next;
} CcViewItem;

typedef struct CcHookEntry {   /* .destroy = fn / .ufcs = { ... } / niche = { ... } / as: a, b; */
    CcSpan span;
    CcName field;
    CcExpr *value;             /* an expression, or NULL when `body` is a block */
    CcStmt *body;
    CcViewItem *items;         /* @typeview entries: the `key: items;` list */
    struct CcHookEntry *next;
} CcHookEntry;

typedef struct CcVariantArm {
    CcSpan span;
    CcName name;
    CcParamList payload;       /* zero or more typed fields */
    struct CcVariantArm *next;
} CcVariantArm;

struct CcDecl {
    CcDeclKind kind;
    CcSpan span;
    uint32_t specs;            /* CC_S_* / CC_F_* / CC_Q_* on the declaration */
    CcAttr *attrs;
    CcType *type;              /* VAR: the variable's type; FUNC: the CC_T_FUNC type; TYPEDEF: the aliased type; TAGGED: the tag type; TYPEHOOKS/TYPEVIEW: the target type */
    CcName name;               /* VAR/FUNC/TYPEDEF name; TYPEVIEW view name (NULL = anonymous); VARIANT/GRAMMAR/GENERIC_FACTORY/COMPTIME_FN/SCOPED_TYPE name; LINK library */
    CcInit *init;              /* VAR initializer (or NULL); `T name@(args)` is an initializer whose expr is CC_E_CREATE
                                  with create_var = name (postfix `!>` / `!> @destroy` may follow it) */
    CcStmt *body;              /* FUNC body (NULL for a prototype); COMPTIME_FN body; COMPTIME_BLOCK; GENERIC_FACTORY body */
    /* VAR: lifetime sugar */
    int destroy;               /* @destroy */
    CcStmt *destroy_body;      /* @destroy { D } */
    int detach;                /* @detach */
    /* FUNC */
    CcName errhandler_default; /* reserved */
    /* TYPEHOOKS / TYPEVIEW */
    CcHookEntry *entries;
    /* VARIANT */
    CcVariantArm *arms;        /* each arm `name: T;` carries one unnamed payload param of type T (`void` = no payload);
                                  the `name(T x, U y);` spelling carries the named params */
    /* GRAMMAR */
    CcName engine;
    uint32_t body_off, body_len; /* fenced body bytes */
    /* GENERIC_FACTORY */
    int factory_extend;
    CcExpr *factory_arity;     /* CC_GENERIC_FACTORY(Name, 2): the arity argument or NULL */
    /* COMPTIME_IF */
    CcExpr *cond;
    CcDeclList then_decls, else_decls;
    /* PRAGMA_CC */
    CcName pragma_name;        /* "parallel", "prelude", "linenumbers", "per_tu" */
    CcName pragma_value;       /* "off" / "on" / NULL */
    /* PP */
    uint32_t tok;              /* the CC_TK_PP token */
    int pp_skipped_region;     /* the span after `tok` is an inactive `#if` region (`__cplusplus`,
                                  `#if 0`) whose tokens are kept verbatim and were not parsed */
    /* MACRO_CALL */
    CcExpr *expr;              /* the call or identifier expression */
    /* STATIC_ASSERT */
    CcExpr *assert_expr;
    uint32_t assert_msg_tok;
};

/* ---- Translation unit ------------------------------------------------- */

typedef struct CcUnit {
    CcArena *arena;
    CcLexFile *file;
    CcDeclList decls;
    /* `#!ccc` unit header lines and file-start pragmas, as PP/PRAGMA decls at the front of `decls` */
    int has_shebang;
    /* typedef names seen at file scope, for the declaration/expression ambiguity */
    CC_LIST(const char) typedef_names;
    /* unknown identifiers the parser treated as type names (forward references,
     * macro-made types); the index confirms them later */
    CC_LIST(const char) assumed_types;
} CcUnit;

/* Interning: one pointer per distinct string for the life of the arena. */
typedef struct CcIntern CcIntern;
CcIntern *cc_intern_new(CcArena *a);
CcName cc_intern(CcIntern *in, const char *s, size_t n);

/* Convenience constructors used by the parser and by lowering. All take the
 * span of the construct they represent. */
CcExpr *cc_expr_new(CcArena *a, CcExprKind k, CcSpan span);
CcStmt *cc_stmt_new(CcArena *a, CcStmtKind k, CcSpan span);
CcDecl *cc_decl_new(CcArena *a, CcDeclKind k, CcSpan span);
CcType *cc_type_new(CcArena *a, CcTypeKind k, CcSpan span);

/* Names of kinds for dumps and diagnostics (user-facing spelling: "!>",
 * "@defer", "closure", not the enum name). */
const char *cc_expr_kind_name(CcExprKind k);
const char *cc_stmt_kind_name(CcStmtKind k);
const char *cc_decl_kind_name(CcDeclKind k);
const char *cc_type_kind_name(CcTypeKind k);

#endif
