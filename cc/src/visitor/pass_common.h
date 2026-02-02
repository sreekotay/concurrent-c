/* pass_common.h - Shared infrastructure for visitor passes.
 *
 * This header consolidates duplicated code across pass_*.c files:
 * - NodeView struct for accessing stub-AST nodes
 * - Path matching helpers (basename, same_source_file)
 * - Line/col to byte offset conversions
 * - File matching against current translation unit
 *
 * Usage: #include "visitor/pass_common.h" in each pass file,
 * then remove the local duplicates.
 */

#ifndef CC_VISITOR_PASS_COMMON_H
#define CC_VISITOR_PASS_COMMON_H

#include <stddef.h>
#include <string.h>

#include "visitor/visitor.h"
#include "visitor/text_span.h"

/* ============================================================================
 * NodeView - Unified view into stub-AST nodes
 *
 * The patched TCC emits stub-AST nodes with this layout. Previously each pass
 * defined its own copy of this struct.
 * ============================================================================ */

typedef struct CCNodeView {
    int kind;           /* Node kind (CC_AST_NODE_*) */
    int parent;         /* Parent node index, or -1 */
    const char* file;   /* Source file path */
    int line_start;     /* 1-based start line */
    int line_end;       /* 1-based end line */
    int col_start;      /* 1-based start column (0 if unavailable) */
    int col_end;        /* 1-based end column (0 if unavailable) */
    int aux1;           /* Node-specific auxiliary data */
    int aux2;           /* Node-specific auxiliary data */
    const char* aux_s1; /* Node-specific string (e.g., method name) */
    const char* aux_s2; /* Node-specific string (e.g., type name) */
} CCNodeView;

/* Node kinds from patched TCC (keep in sync with tcc.h) */
enum {
    CC_AST_NODE_UNKNOWN = 0,
    CC_AST_NODE_DECL = 1,
    CC_AST_NODE_BLOCK = 2,
    CC_AST_NODE_STMT = 3,
    CC_AST_NODE_ARENA = 4,
    CC_AST_NODE_CALL = 5,
    CC_AST_NODE_AWAIT = 6,
    CC_AST_NODE_SEND_TAKE = 7,
    CC_AST_NODE_SUBSLICE = 8,
    CC_AST_NODE_CLOSURE = 9,
    CC_AST_NODE_IDENT = 10,
    CC_AST_NODE_CONST = 11,
    CC_AST_NODE_DECL_ITEM = 12,
    CC_AST_NODE_MEMBER = 13,
    CC_AST_NODE_ASSIGN = 14,
    CC_AST_NODE_RETURN = 15,
    CC_AST_NODE_PARAM = 16,
    CC_AST_NODE_FUNC = 17,
    CC_AST_NODE_BINARY = 18,
    CC_AST_NODE_TRY = 19,
    CC_AST_NODE_IF = 20,
    CC_AST_NODE_FOR = 21,
    CC_AST_NODE_WHILE = 22,
    CC_AST_NODE_UNARY = 23,
    CC_AST_NODE_SIZEOF = 24,
    CC_AST_NODE_STRUCT = 25,
    CC_AST_NODE_STRUCT_FIELD = 26,
    CC_AST_NODE_TYPEDEF = 27,
    CC_AST_NODE_INDEX = 28,
    CC_AST_NODE_ENUM = 29,
    CC_AST_NODE_ENUM_VALUE = 30,
};

/* ============================================================================
 * Path Matching Helpers
 * ============================================================================ */

/* Return pointer to basename (after last / or \) */
static inline const char* cc_pass_basename(const char* path) {
    if (!path) return NULL;
    const char* last = path;
    for (const char* p = path; *p; p++) {
        if (*p == '/' || *p == '\\') last = p + 1;
    }
    return last;
}

/* Return pointer to last 2 path components (for better matching) */
static inline const char* cc_pass_path_suffix2(const char* path) {
    if (!path) return NULL;
    const char* end = path + strlen(path);
    int seps = 0;
    for (const char* p = end; p > path; ) {
        p--;
        if (*p == '/' || *p == '\\') {
            seps++;
            if (seps == 2) return p + 1;
        }
    }
    return cc_pass_basename(path);
}

/* Check if two paths refer to the same source file.
 * Handles: exact match, basename match, 2-component suffix match */
static inline int cc_pass_same_file(const char* a, const char* b) {
    if (!a || !b) return 0;
    if (strcmp(a, b) == 0) return 1;
    
    const char* a_base = cc_pass_basename(a);
    const char* b_base = cc_pass_basename(b);
    if (!a_base || !b_base || strcmp(a_base, b_base) != 0) return 0;
    
    /* Prefer 2-component suffix match */
    const char* a_suf = cc_pass_path_suffix2(a);
    const char* b_suf = cc_pass_path_suffix2(b);
    if (a_suf && b_suf && strcmp(a_suf, b_suf) == 0) return 1;
    
    /* Fallback: basename-only match */
    return 1;
}

/* ============================================================================
 * Translation Unit Matching
 * ============================================================================ */

/* Check if a node's file matches the current translation unit.
 * Handles both original input path and lowered (temp) path. */
static inline int cc_pass_node_in_tu(const CCASTRoot* root,
                                     const CCVisitorCtx* ctx,
                                     const char* node_file) {
    if (!ctx || !ctx->input_path || !node_file) return 0;
    if (cc_pass_same_file(ctx->input_path, node_file)) return 1;
    if (root && root->lowered_path && cc_pass_same_file(root->lowered_path, node_file)) return 1;
    return 0;
}

/* ============================================================================
 * Span Helpers
 * ============================================================================ */

/* Get byte offset for a node's start position */
static inline size_t cc_pass_node_start_offset(const CCNodeView* n,
                                               const char* src,
                                               size_t src_len) {
    if (!n || !src || n->line_start <= 0) return 0;
    if (n->col_start > 0) {
        return cc__offset_of_line_col_1based(src, src_len, n->line_start, n->col_start);
    }
    return cc__offset_of_line_1based(src, src_len, n->line_start);
}

/* Get byte offset for a node's end position */
static inline size_t cc_pass_node_end_offset(const CCNodeView* n,
                                             const char* src,
                                             size_t src_len) {
    if (!n || !src) return src_len;
    int le = (n->line_end > 0) ? n->line_end : n->line_start;
    if (le <= 0) return src_len;
    if (n->col_end > 0) {
        return cc__offset_of_line_col_1based(src, src_len, le, n->col_end);
    }
    return cc__offset_of_line_1based(src, src_len, le + 1);
}

/* Get indentation (spaces/tabs) at start of a line */
static inline size_t cc_pass_line_indent(const char* src, size_t src_len, int line_no) {
    size_t lo = cc__offset_of_line_1based(src, src_len, line_no);
    size_t i = lo;
    while (i < src_len && (src[i] == ' ' || src[i] == '\t')) i++;
    return i - lo;
}

/* ============================================================================
 * Diagnostic Helpers (gcc/clang compatible format)
 *
 * Use these instead of ad-hoc fprintf(stderr, "CC: error: ...") calls.
 * Format: file:line:col: error: message
 * This format is recognized by IDEs for jump-to-error.
 * ============================================================================ */

#include <stdio.h>
#include <stdarg.h>

/* Error categories for consistent grep-able messages */
#define CC_ERR_SYNTAX   "syntax"
#define CC_ERR_CHANNEL  "channel"
#define CC_ERR_ASYNC    "async"
#define CC_ERR_CLOSURE  "closure"
#define CC_ERR_SLICE    "slice"
#define CC_ERR_TYPE     "type"

/* Emit error in gcc/clang format: file:line:col: error: category: message */
static inline void cc_pass_error(const char* file, int line, int col, const char* fmt, ...) {
    const char* f = file ? file : "<input>";
    int l = (line > 0) ? line : 1;
    int c = (col > 0) ? col : 1;
    fprintf(stderr, "%s:%d:%d: error: ", f, l, c);
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fprintf(stderr, "\n");
}

/* Emit categorized error: file:line:col: error: category: message */
static inline void cc_pass_error_cat(const char* file, int line, int col, 
                                     const char* category, const char* fmt, ...) {
    const char* f = file ? file : "<input>";
    int l = (line > 0) ? line : 1;
    int c = (col > 0) ? col : 1;
    fprintf(stderr, "%s:%d:%d: error: %s: ", f, l, c, category);
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fprintf(stderr, "\n");
}

/* Emit note (follow-up to error) */
static inline void cc_pass_note(const char* file, int line, int col, const char* fmt, ...) {
    const char* f = file ? file : "<input>";
    int l = (line > 0) ? line : 1;
    int c = (col > 0) ? col : 1;
    fprintf(stderr, "%s:%d:%d: note: ", f, l, c);
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fprintf(stderr, "\n");
}

/* Emit warning */
static inline void cc_pass_warning(const char* file, int line, int col, const char* fmt, ...) {
    const char* f = file ? file : "<input>";
    int l = (line > 0) ? line : 1;
    int c = (col > 0) ? col : 1;
    fprintf(stderr, "%s:%d:%d: warning: ", f, l, c);
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fprintf(stderr, "\n");
}

#endif /* CC_VISITOR_PASS_COMMON_H */
