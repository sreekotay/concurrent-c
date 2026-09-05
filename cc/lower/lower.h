/* Lowering: CcUnit with Concurrent-C nodes → CcUnit with C nodes.
 *
 * One function per construct family, each rewriting nodes in place and
 * returning nothing but diagnostics. Every node a lowering creates copies
 * the span of the node it replaces, so the printer pins the generated lines
 * to the user's line. Lowerings consult the CcIndex for every fact about a
 * name; none carries a table of names.
 *
 * The exact C each construct produces is written down in LOWERING.md next to
 * this header. The runtime and stdlib do not change: the targets are the
 * entry points the current lowerer already emits. */
#ifndef CC_LOWER_LOWER_H
#define CC_LOWER_LOWER_H
#include "ast.h"
#include "index.h"
#include "diag.h"

typedef struct CcLowerOpts {
    int line_directives;      /* honour #pragma(@linenumbers) */
    int parallel_off;         /* #pragma(@parallel) off */
    int per_tu;               /* #pragma(@per_tu) */
    const char *unit_path;    /* spelling used in cc_rt_diag_record_unwrap_site(path, "line") */
} CcLowerOpts;

typedef struct CcLowerer {
    CcArena *arena;
    CcDiag *diag;
    CcIntern *intern;
    CcIndex *index;
    CcUnit *unit;
    CcLowerOpts opts;
    uint32_t next_temp;       /* __cc_r_N, __cc_eh_N ... one counter per unit */
} CcLowerer;

void cc_lowerer_init(CcLowerer *L, CcArena *a, CcDiag *d, CcIntern *in, CcIndex *ix,
                     CcUnit *u, const CcLowerOpts *opts);

/* The whole pipeline, in order. Each step is also callable alone for tests.
 * Steps after an error-producing step still run so a file reports every
 * error it has; the driver refuses to print C when diag->n_errors > 0. */
void cc_lower_unit(CcLowerer *L);

void cc_lower_results(CcLowerer *L);     /* Result types, cc_ok/cc_err, !> ?> forms, @errhandler, @err, @destroy, @defer */
void cc_lower_ufcs(CcLowerer *L);        /* x.m(), Type.fn(), generics, @typehooks, @variant */
void cc_lower_strings(CcLowerer *L);     /* @string templates, @scratch, @slice, slices */
void cc_lower_concurrency(CcLowerer *L); /* closures, spawn, channels, @parallel, deadlines, @closing */
void cc_lower_async(CcLowerer *L);       /* @async / @await */
void cc_lower_comptime(CcLowerer *L);    /* @comptime seam, @grammar, factories */

/* Helpers shared by the steps. */
CcName  cc_lower_temp(CcLowerer *L, const char *prefix);        /* fresh interned name */
CcExpr *cc_lower_call(CcLowerer *L, CcSpan span, CcName fn, int nargs, ...); /* CC_E_CALL with ident callee */
CcStmt *cc_lower_block(CcLowerer *L, CcSpan span);              /* empty CC_S_BLOCK */
void    cc_lower_replace_stmt(CcStmtList *list, size_t at, CcStmt *with);
void    cc_lower_insert_stmt(CcArena *a, CcStmtList *list, size_t at, CcStmt *s);

/* The function being lowered: needed for the enclosing Result type of
 * inferred cc_ok/cc_err, the @errhandler stack, the @defer scopes, and the
 * function scratch arena. */
typedef struct CcFnCtx {
    CcDecl *fn;
    CcTypeInfo *ret_result;   /* NULL when the function does not return a Result */
    CC_LIST(CcStmt) errhandlers;   /* active @errhandler statements, innermost last */
    CC_LIST(CcStmt) defers;        /* active @defer statements, innermost last */
    int uses_scratch;         /* a @scratch template appeared: declare __cc_str_scratch */
    uint32_t scratch_bytes;   /* max @scratch(N), default 1024 */
} CcFnCtx;

#endif
