/* Parser: token array → CcUnit. Recursive descent over the C11 surface the
 * corpus uses (declarators, initializers, expressions with C precedence,
 * statements, GNU extensions the stdlib relies on: __attribute__,
 * __typeof__, statement expressions, _Generic, __builtin_* as ordinary
 * calls) plus every Concurrent-C form in ast.h.
 *
 * Errors are collected in the CcDiag; the parser recovers at the next
 * statement or declaration boundary and continues, so one run reports every
 * error in a file. A construct the parser does not model is an error naming
 * the construct at its first token; there is no raw fallback.
 *
 * Typedef names: the parser keeps a scope stack of typedef names (seeded by
 * `known_types`, which the driver fills from the declaration index of the
 * included headers) to resolve the C declaration/expression ambiguity. An
 * unknown identifier in type position followed by a declarator is treated as
 * a type and recorded, as C compilers do for forward references, and noted
 * so the index can confirm it later. */
#ifndef CC_LOWER_PARSE_H
#define CC_LOWER_PARSE_H
#include "ast.h"
#include "diag.h"

typedef enum CcUnitMode {
    CC_MODE_SOURCE = 0,  /* .ccs: a translation unit with bodies */
    CC_MODE_HEADER,      /* .cch: declarations; static inline bodies kept; CC_D_COMPTIME_* allowed */
    CC_MODE_SCRIPT       /* .shcc: top-level statements allowed (synthetic main is the driver's job) */
} CcUnitMode;

typedef struct CcParseOpts {
    CcUnitMode mode;
    const char **known_types;   /* NULL-terminated list of typedef names in scope before the file */
    int allow_top_level_stmts;  /* CC_MODE_SCRIPT implies 1 */
} CcParseOpts;

/* Parse one lexed file. Always returns a unit (possibly with errors in `d`). */
CcUnit *cc_parse(CcArena *a, CcDiag *d, CcIntern *in, CcLexFile *f, const CcParseOpts *opts);

/* Parse the text of a template slot `${expr}` as an expression. `off`/`len`
 * locate the slot text in the file so diagnostics land on the user's line. */
CcExpr *cc_parse_slot_expr(CcArena *a, CcDiag *d, CcIntern *in, CcLexFile *f,
                           uint32_t off, uint32_t len);

/* Debug dump of a unit as an indented tree, one node per line:
 *   kind [first..last] name-or-op  (line:col)
 * Used by the round-trip tool and by tests. */
void cc_ast_dump(const CcUnit *u, void *stream /* FILE* */);

/* Walk every node (pre-order). Returning nonzero from a callback stops that
 * subtree. Used by coverage checks, lowering and the index. */
typedef struct CcVisitor {
    void *ctx;
    int (*on_decl)(struct CcVisitor *v, CcDecl *d);
    int (*on_stmt)(struct CcVisitor *v, CcStmt *s);
    int (*on_expr)(struct CcVisitor *v, CcExpr *e);
    int (*on_type)(struct CcVisitor *v, CcType *t);
} CcVisitor;
void cc_ast_walk(CcUnit *u, CcVisitor *v);

#endif
