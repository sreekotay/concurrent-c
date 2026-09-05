/* Printer: CcUnit → C text plus a source map.
 *
 * Two modes. Identity prints every token's trivia and text back, so the
 * output equals the input byte for byte; it exists to prove the parser
 * covers a file (every token must be reachable from the tree). Lowered
 * printing walks the tree: C nodes print as C, trivia (comments, blank
 * lines) is replayed from the tokens a node spans, and every emitted line
 * records the logical (file, line) of the node it came from. A `#line` is
 * emitted whenever that changes; a multi-line synthetic expansion of one
 * source line re-pins that line before every physical line it emits. */
#ifndef CC_LOWER_PRINT_H
#define CC_LOWER_PRINT_H
#include "ast.h"

typedef struct CcSrcMapEntry {
    uint32_t emit_line;     /* 1-based line in the emitted text */
    const char *path;       /* logical user path */
    uint32_t line;          /* logical user line */
    int32_t col_delta;      /* emitted column - user column for text copied verbatim; 0 when unknown */
} CcSrcMapEntry;

typedef struct CcSrcMap {
    CcSrcMapEntry *entries; /* one per emitted line, in order */
    uint32_t n;
} CcSrcMap;

typedef struct CcPrintOpts {
    int identity;           /* print tokens verbatim (round-trip mode) */
    int line_directives;    /* emit `#line` (off for #pragma(@linenumbers) off) */
    const char *path;       /* path spelling for `#line`; default the unit's file path as given */
    int header_mode;        /* .h product: `#pragma once`, drop function bodies that are not static inline */
} CcPrintOpts;

/* Print the unit into `out`. In identity mode the tree is walked to check
 * coverage: a token not reached by any node is a diagnostic
 * ("token not covered by the tree: ...") so a parser gap cannot hide. The
 * source map is filled for lowered mode (NULL to skip). */
void cc_print_unit(CcBuf *out, CcSrcMap *map, CcArena *a, CcDiag *d, const CcUnit *u,
                   const CcPrintOpts *opts);

/* Print one expression / statement / type into `out` (lowered mode only),
 * for tests and for diagnostics that quote a construct in the user's spelling. */
void cc_print_expr(CcBuf *out, const CcLexFile *f, const CcExpr *e);
void cc_print_stmt(CcBuf *out, const CcLexFile *f, const CcStmt *s, int indent);
void cc_print_type(CcBuf *out, const CcLexFile *f, const CcType *t, const char *declarator_name);

#endif
