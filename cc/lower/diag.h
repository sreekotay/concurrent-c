/* One diagnostic sink. Every message has a location in the user's file, a
 * severity, and the construct in the user's spelling. Messages are collected,
 * not printed as they happen, so a stage never masks the next one; the driver
 * prints them once, sorted by position, and decides the exit status. */
#ifndef CC_LOWER_DIAG_H
#define CC_LOWER_DIAG_H
#include <stddef.h>
#include <stdint.h>
#include "mem.h"

typedef enum CcSeverity { CC_SEV_NOTE, CC_SEV_WARNING, CC_SEV_ERROR } CcSeverity;

/* A location is logical: the file and line the user sees after `#line`
 * rebasing, plus a column. `path` is the spelling to print (as given on the
 * command line, or the `#line` path); `line`/`col` are 1-based; 0 means
 * unknown and is printed without that part. */
typedef struct CcLoc {
    const char *path;
    uint32_t line;
    uint32_t col;
} CcLoc;

typedef struct CcDiagMsg {
    CcSeverity sev;
    CcLoc loc;
    const char *text;        /* one line, no trailing newline */
    const char *source_line; /* the user's line for a caret snippet, or NULL */
    uint32_t caret_col;      /* 1-based column for the caret, 0 = none */
    uint32_t caret_len;      /* underline length, 0 = one column */
} CcDiagMsg;

typedef struct CcDiag {
    CcArena *arena;
    CC_LIST(CcDiagMsg) msgs;
    uint32_t n_errors;
    uint32_t n_warnings;
} CcDiag;

void cc_diag_init(CcDiag *d, CcArena *a);
void cc_diag_emit(CcDiag *d, CcSeverity sev, CcLoc loc, const char *fmt, ...);
/* Same, with a caret snippet: `src` is the whole file text and `off` the byte
 * offset of the caret in it (the snippet line is cut from `src`). */
void cc_diag_emit_at(CcDiag *d, CcSeverity sev, CcLoc loc, const char *src,
                     size_t src_len, size_t off, size_t len, const char *fmt, ...);
/* Print all messages, sorted by (path, line, col), as
 *   path:line:col: error: text
 *   <source line>
 *   ^~~~
 * Returns n_errors. */
uint32_t cc_diag_print(const CcDiag *d, void *stream /* FILE* */);

#endif
