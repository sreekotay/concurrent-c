#include "pass_nursery_spawn_ast.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "visitor/text_span.h"

#ifndef CC_TCC_EXT_AVAILABLE
#error "CC_TCC_EXT_AVAILABLE is required (patched TCC stub-AST required)."
#endif

/* Keep in sync with async_ast.c's CC_AST_KIND enum. */
enum {
    CC_AST_NODE_UNKNOWN = 0,
    CC_AST_NODE_DECL = 1,
    CC_AST_NODE_BLOCK = 2,
    CC_AST_NODE_STMT = 3,
};

typedef struct {
    int kind;
    int parent;
    const char* file;
    int line_start;
    int line_end;
    int col_start;
    int col_end;
    int aux1;
    int aux2;
    const char* aux_s1;
    const char* aux_s2;
} NodeView;

static const char* cc__basename(const char* path) {
    if (!path) return NULL;
    const char* last = path;
    for (const char* p = path; *p; p++) {
        if (*p == '/' || *p == '\\') last = p + 1;
    }
    return last;
}

static const char* cc__path_suffix2(const char* path) {
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
    return cc__basename(path);
}

static int cc__same_source_file(const char* a, const char* b) {
    if (!a || !b) return 0;
    if (strcmp(a, b) == 0) return 1;
    const char* a_base = cc__basename(a);
    const char* b_base = cc__basename(b);
    if (!a_base || !b_base || strcmp(a_base, b_base) != 0) return 0;
    const char* a_suf = cc__path_suffix2(a);
    const char* b_suf = cc__path_suffix2(b);
    if (a_suf && b_suf && strcmp(a_suf, b_suf) == 0) return 1;
    return 1;
}

static int cc__node_file_matches_this_tu(const CCASTRoot* root,
                                        const CCVisitorCtx* ctx,
                                        const char* node_file) {
    if (!ctx || !ctx->input_path || !node_file) return 0;
    if (cc__same_source_file(ctx->input_path, node_file)) return 1;
    if (root && root->lowered_path && cc__same_source_file(root->lowered_path, node_file)) return 1;
    return 0;
}


static int cc__find_substr_in_range(const char* s,
                                   size_t start,
                                   size_t end,
                                   const char* needle,
                                   size_t needle_len,
                                   size_t* out_pos) {
    if (!s || !needle || needle_len == 0) return 0;
    if (start > end) return 0;
    if (end - start < needle_len) return 0;
    for (size_t i = start; i + needle_len <= end; i++) {
        if (memcmp(s + i, needle, needle_len) == 0) {
            if (out_pos) *out_pos = i;
            return 1;
        }
    }
    return 0;
}

static int cc__scan_matching_rbrace(const char* s, size_t len, size_t lbrace_off, size_t* out_rbrace_off) {
    if (!s || lbrace_off >= len || s[lbrace_off] != '{') return 0;
    int depth = 0;
    int in_str = 0;
    char qch = 0;
    int in_line_comment = 0;
    int in_block_comment = 0;
    for (size_t i = lbrace_off; i < len; i++) {
        char ch = s[i];
        if (in_line_comment) {
            if (ch == '\n') in_line_comment = 0;
            continue;
        }
        if (in_block_comment) {
            if (ch == '*' && i + 1 < len && s[i + 1] == '/') { in_block_comment = 0; i++; }
            continue;
        }
        if (in_str) {
            if (ch == '\\' && i + 1 < len) { i++; continue; }
            if (ch == qch) in_str = 0;
            continue;
        }
        if (ch == '/' && i + 1 < len && s[i + 1] == '/') { in_line_comment = 1; i++; continue; }
        if (ch == '/' && i + 1 < len && s[i + 1] == '*') { in_block_comment = 1; i++; continue; }
        if (ch == '"' || ch == '\'') { in_str = 1; qch = ch; continue; }

        if (ch == '{') depth++;
        else if (ch == '}') {
            depth--;
            if (depth == 0) {
                if (out_rbrace_off) *out_rbrace_off = i;
                return 1;
            }
        }
    }
    return 0;
}

/* Best-effort start offset for stmt markers when stub-AST columns are missing.
   Uses AST-provided line spans; hard-errors if the marker can't be located within that span. */
static int cc__stmt_marker_start_off(const CCASTRoot* root,
                                    const CCVisitorCtx* ctx,
                                    const NodeView* n,
                                    int node_i,
                                    const char* in_src,
                                    size_t in_len,
                                    const char* marker,
                                    size_t marker_len,
                                    size_t* out_start_off,
                                    int* out_col_1based) {
    if (!root || !ctx || !n || !in_src || !marker || marker_len == 0) return 0;
    if (node_i < 0 || node_i >= root->node_count) return 0;
    if (!cc__node_file_matches_this_tu(root, ctx, n[node_i].file)) return 0;
    if (n[node_i].line_start <= 0) return 0;

    size_t line_off = cc__offset_of_line_1based(in_src, in_len, n[node_i].line_start);

    if (n[node_i].col_start > 0) {
        size_t st = cc__offset_of_line_col_1based(in_src, in_len, n[node_i].line_start, n[node_i].col_start);
        if (out_start_off) *out_start_off = st;
        if (out_col_1based) *out_col_1based = n[node_i].col_start;
        return 1;
    }

    int le = (n[node_i].line_end > 0) ? n[node_i].line_end : n[node_i].line_start;
    size_t span_start = line_off;
    size_t span_end = cc__offset_of_line_1based(in_src, in_len, le + 1);
    if (span_end > in_len) span_end = in_len;
    if (span_start > in_len) return 0;

    size_t found = 0;
    if (!cc__find_substr_in_range(in_src, span_start, span_end, marker, marker_len, &found)) {
        const char* f = (n[node_i].file && n[node_i].file[0]) ? n[node_i].file : (ctx->input_path ? ctx->input_path : "<input>");
        fprintf(stderr, "%s:%d:1: error: CC: internal: stmt marker '%.*s' not found within stub-AST span (lines %d..%d)\n",
                f, n[node_i].line_start, (int)marker_len, marker, n[node_i].line_start, le);
        return -1;
    }

    if (out_start_off) *out_start_off = found;
    if (out_col_1based) *out_col_1based = 1 + (int)(found - line_off);
    return 1;
}

static void cc__append_n(char** out, size_t* out_len, size_t* out_cap, const char* s, size_t n) {
    if (!out || !out_len || !out_cap || !s) return;
    if (*out_len + n + 1 > *out_cap) {
        size_t nc = *out_cap ? *out_cap * 2 : 1024;
        while (nc < *out_len + n + 1) nc *= 2;
        char* nb = (char*)realloc(*out, nc);
        if (!nb) return;
        *out = nb;
        *out_cap = nc;
    }
    memcpy(*out + *out_len, s, n);
    *out_len += n;
    (*out)[*out_len] = 0;
}

static void cc__append_str(char** out, size_t* out_len, size_t* out_cap, const char* s) {
    if (!s) return;
    cc__append_n(out, out_len, out_cap, s, strlen(s));
}

static void cc__append_fmt(char** out, size_t* out_len, size_t* out_cap, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char tmp[1024];
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n <= 0) return;
    if ((size_t)n < sizeof(tmp)) {
        cc__append_n(out, out_len, out_cap, tmp, (size_t)n);
        return;
    }
    char* big = (char*)malloc((size_t)n + 1);
    if (!big) return;
    va_start(ap, fmt);
    vsnprintf(big, (size_t)n + 1, fmt, ap);
    va_end(ap);
    cc__append_n(out, out_len, out_cap, big, (size_t)n);
    free(big);
}

static int cc__is_ident_start(char c) {
    return (c == '_' || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'));
}
static int cc__is_ident_char(char c) {
    return cc__is_ident_start(c) || (c >= '0' && c <= '9');
}

static size_t cc__skip_ws(const char* s, size_t i, size_t end) {
    while (i < end && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) i++;
    return i;
}

/* Parse `closing(a, b, c)` within [start, end). Returns count (0 if not present), or -1 on error. */
static int cc__parse_closing_clause(const CCVisitorCtx* ctx,
                                   const NodeView* n,
                                   int nursery_node_i,
                                   const char* src,
                                   size_t start,
                                   size_t end,
                                   char names[][64],
                                   int cap) {
    (void)ctx;
    if (!src || start >= end || !names || cap <= 0) return 0;

    size_t pos = (size_t)-1;
    if (!cc__find_substr_in_range(src, start, end, "closing", 7, &pos)) return 0;

    size_t i = pos + 7;
    i = cc__skip_ws(src, i, end);
    if (i >= end || src[i] != '(') {
        const char* f = (n && n[nursery_node_i].file) ? n[nursery_node_i].file : "<input>";
        fprintf(stderr, "%s:%d:1: error: expected '(' after @nursery closing\n", f, (n ? n[nursery_node_i].line_start : 1));
        return -1;
    }
    i++; /* consume '(' */
    int nn = 0;
    for (;;) {
        i = cc__skip_ws(src, i, end);
        if (i >= end) break;
        if (src[i] == ')') { i++; break; }
        if (!cc__is_ident_start(src[i])) {
            const char* f = (n && n[nursery_node_i].file) ? n[nursery_node_i].file : "<input>";
            fprintf(stderr, "%s:%d:1: error: expected identifier in @nursery closing(...)\n", f, (n ? n[nursery_node_i].line_start : 1));
            return -1;
        }
        size_t s0 = i;
        i++;
        while (i < end && cc__is_ident_char(src[i])) i++;
        size_t sl = i - s0;
        if (nn >= cap) {
            const char* f = (n && n[nursery_node_i].file) ? n[nursery_node_i].file : "<input>";
            fprintf(stderr, "%s:%d:1: error: too many channels in @nursery closing(...) (max %d)\n",
                    f, (n ? n[nursery_node_i].line_start : 1), cap);
            return -1;
        }
        if (sl >= 64) sl = 63;
        memcpy(names[nn], src + s0, sl);
        names[nn][sl] = '\0';
        nn++;
        i = cc__skip_ws(src, i, end);
        if (i < end && src[i] == ',') { i++; continue; }
        if (i < end && src[i] == ')') { i++; break; }
        /* Anything else is malformed. */
        const char* f = (n && n[nursery_node_i].file) ? n[nursery_node_i].file : "<input>";
        fprintf(stderr, "%s:%d:1: error: malformed @nursery closing(...) clause\n", f, (n ? n[nursery_node_i].line_start : 1));
        return -1;
    }
    if (nn == 0) {
        const char* f = (n && n[nursery_node_i].file) ? n[nursery_node_i].file : "<input>";
        fprintf(stderr, "%s:%d:1: error: @nursery closing(...) requires at least one channel\n", f, (n ? n[nursery_node_i].line_start : 1));
        return -1;
    }
    return nn;
}

typedef struct {
    size_t start;
    size_t end;
    char* repl;
} Edit;

static int cc__edit_cmp_start_asc(const void* a, const void* b) {
    const Edit* ea = (const Edit*)a;
    const Edit* eb = (const Edit*)b;
    if (ea->start < eb->start) return -1;
    if (ea->start > eb->start) return 1;
    /* tie-break: longer span first */
    size_t la = ea->end - ea->start;
    size_t lb = eb->end - eb->start;
    if (la > lb) return -1;
    if (la < lb) return 1;
    return 0;
}

typedef struct {
    int node_i;
    size_t start_off;
    int id;
} NurseryId;

static int cc__nursery_id_cmp_start_asc(const void* a, const void* b) {
    const NurseryId* na = (const NurseryId*)a;
    const NurseryId* nb = (const NurseryId*)b;
    if (na->start_off < nb->start_off) return -1;
    if (na->start_off > nb->start_off) return 1;
    return na->node_i - nb->node_i;
}

static int cc__build_nursery_id_map(const CCASTRoot* root,
                                   const CCVisitorCtx* ctx,
                                   const char* in_src,
                                   size_t in_len,
                                   int* out_id_by_node_i,
                                   int out_cap,
                                   int* out_max_id) {
    if (!root || !root->nodes || !ctx || !in_src || !out_id_by_node_i || out_cap <= 0) return 0;
    const NodeView* n = (const NodeView*)root->nodes;

    NurseryId tmp[512];
    int tn = 0;

    for (int i = 0; i < root->node_count && tn < (int)(sizeof(tmp) / sizeof(tmp[0])); i++) {
        if (i >= out_cap) break;
        if (n[i].kind != CC_AST_NODE_STMT) continue;
        if (!n[i].aux_s1 || strcmp(n[i].aux_s1, "nursery") != 0) continue;
        size_t start = 0;
        int col1 = 1;
        int sr = cc__stmt_marker_start_off(root, ctx, n, i, in_src, in_len, "@nursery", 8, &start, &col1);
        if (sr == 0) continue;
        if (sr < 0) return 0;
        tmp[tn++] = (NurseryId){ .node_i = i, .start_off = start, .id = 0 };
    }

    qsort(tmp, (size_t)tn, sizeof(tmp[0]), cc__nursery_id_cmp_start_asc);
    for (int i = 0; i < out_cap; i++) out_id_by_node_i[i] = 0;

    int id = 0;
    for (int k = 0; k < tn; k++) {
        id++;
        tmp[k].id = id;
        out_id_by_node_i[tmp[k].node_i] = id;
    }
    if (out_max_id) *out_max_id = id;
    return 1;
}

static int cc__find_enclosing_nursery_node_i(const CCASTRoot* root, int spawn_node_i) {
    if (!root || !root->nodes) return -1;
    const NodeView* n = (const NodeView*)root->nodes;
    int cur = spawn_node_i;
    int guard = 0;
    while (cur >= 0 && cur < root->node_count && guard++ < 4096) {
        if (n[cur].kind == CC_AST_NODE_STMT && n[cur].aux_s1 && strcmp(n[cur].aux_s1, "nursery") == 0) {
            return cur;
        }
        cur = n[cur].parent;
    }
    return -1;
}

static size_t cc__line_indent_len(const char* s, size_t len, int line_no) {
    size_t lo = cc__offset_of_line_1based(s, len, line_no);
    size_t i = lo;
    while (i < len && (s[i] == ' ' || s[i] == '\t')) i++;
    return i - lo;
}

static int cc__parse_int_literal(const char* s, size_t n, long* out_v) {
    if (!s || n == 0 || !out_v) return 0;
    size_t i = 0;
    while (i < n && (s[i] == ' ' || s[i] == '\t')) i++;
    if (i >= n) return 0;
    char buf[64];
    size_t bn = 0;
    if (s[i] == '-') { buf[bn++] = s[i++]; }
    if (i >= n || !isdigit((unsigned char)s[i])) return 0;
    while (i < n && isdigit((unsigned char)s[i]) && bn + 1 < sizeof(buf)) {
        buf[bn++] = s[i++];
    }
    buf[bn] = 0;
    while (i < n && (s[i] == ' ' || s[i] == '\t')) i++;
    if (i != n) return 0;
    *out_v = strtol(buf, NULL, 10);
    return 1;
}

/* Try to parse `ident()` or `ident(<intlit>)`. Returns 1 if matches and sets fields. */
static int cc__parse_simple_fn_call(const char* s, size_t n,
                                   char* out_fn, size_t out_fn_cap,
                                   int* out_has_arg, long* out_arg) {
    if (!s || !out_fn || out_fn_cap == 0 || !out_has_arg || !out_arg) return 0;
    size_t i = 0;
    while (i < n && (s[i] == ' ' || s[i] == '\t')) i++;
    if (i >= n) return 0;
    if (!(isalpha((unsigned char)s[i]) || s[i] == '_')) return 0;
    size_t fn_s = i;
    i++;
    while (i < n && (isalnum((unsigned char)s[i]) || s[i] == '_')) i++;
    size_t fn_e = i;
    while (i < n && (s[i] == ' ' || s[i] == '\t')) i++;
    if (i >= n || s[i] != '(') return 0;
    i++;
    while (i < n && (s[i] == ' ' || s[i] == '\t')) i++;
    size_t inside_s = i;
    /* find closing ) */
    while (i < n && s[i] != ')') i++;
    if (i >= n || s[i] != ')') return 0;
    size_t inside_e = i;
    i++; /* consume ) */
    while (i < n && (s[i] == ' ' || s[i] == '\t')) i++;
    if (i != n) return 0;

    size_t fn_len = fn_e - fn_s;
    if (fn_len + 1 > out_fn_cap) return 0;
    memcpy(out_fn, s + fn_s, fn_len);
    out_fn[fn_len] = 0;

    /* inside can be empty or int literal */
    while (inside_s < inside_e && (s[inside_s] == ' ' || s[inside_s] == '\t')) inside_s++;
    while (inside_e > inside_s && (s[inside_e - 1] == ' ' || s[inside_e - 1] == '\t')) inside_e--;
    if (inside_s == inside_e) {
        *out_has_arg = 0;
        *out_arg = 0;
        return 1;
    }
    long v = 0;
    if (!cc__parse_int_literal(s + inside_s, inside_e - inside_s, &v)) return 0;
    *out_has_arg = 1;
    *out_arg = v;
    return 1;
}

static int cc__split_top_level_commas(const char* s, size_t n, int* out_pos, int out_cap) {
    int par = 0, brk = 0, br = 0;
    int ins = 0;
    char qch = 0;
    int k = 0;
    for (size_t i = 0; i < n; i++) {
        char ch = s[i];
        if (ins) {
            if (ch == '\\' && i + 1 < n) { i++; continue; }
            if (ch == qch) ins = 0;
            continue;
        }
        if (ch == '"' || ch == '\'') { ins = 1; qch = ch; continue; }
        if (ch == '(') par++;
        else if (ch == ')') { if (par) par--; }
        else if (ch == '[') brk++;
        else if (ch == ']') { if (brk) brk--; }
        else if (ch == '{') br++;
        else if (ch == '}') { if (br) br--; }
        else if (ch == ',' && par == 0 && brk == 0 && br == 0) {
            if (k < out_cap) out_pos[k++] = (int)i;
        }
    }
    return k;
}

static size_t cc__infer_spawn_stmt_end_off(const char* s, size_t len, size_t start_off) {
    if (!s || start_off >= len) return 0;
    size_t i = start_off;
    /* Find first '(' after 'spawn'. */
    while (i < len && s[i] != '(') i++;
    if (i >= len || s[i] != '(') return 0;
    int par = 0;
    int in_str = 0;
    char qch = 0;
    /* Scan to matching ')' for the spawn call. */
    for (; i < len; i++) {
        char ch = s[i];
        if (in_str) {
            if (ch == '\\' && i + 1 < len) { i++; continue; }
            if (ch == qch) in_str = 0;
            continue;
        }
        if (ch == '"' || ch == '\'') { in_str = 1; qch = ch; continue; }
        if (ch == '(') par++;
        else if (ch == ')') {
            if (par > 0) par--;
            if (par == 0) { i++; break; }
        }
    }
    if (i > len) i = len;
    /* Consume trailing whitespace and optional ';'. */
    while (i < len && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r')) i++;
    if (i < len && s[i] == ';') i++;
    return i;
}

static void cc__trim_span(const char* s, size_t* io_off, size_t* io_len) {
    size_t off = *io_off;
    size_t len = *io_len;
    while (len > 0 && (s[off] == ' ' || s[off] == '\t')) { off++; len--; }
    while (len > 0 && (s[off + len - 1] == ' ' || s[off + len - 1] == '\t')) { len--; }
    *io_off = off;
    *io_len = len;
}

int cc__rewrite_spawn_stmts_with_nodes(const CCASTRoot* root,
                                      const CCVisitorCtx* ctx,
                                      const char* in_src,
                                      size_t in_len,
                                      char** out_src,
                                      size_t* out_len) {
    if (!root || !ctx || !in_src || !out_src || !out_len) return 0;
    *out_src = NULL;
    *out_len = 0;
    if (!root->nodes || root->node_count <= 0) return 0;
    const NodeView* n = (const NodeView*)root->nodes;

    int* id_by_node = (int*)calloc((size_t)root->node_count, sizeof(int));
    if (!id_by_node) return 0;
    int max_id = 0;
    cc__build_nursery_id_map(root, ctx, in_src, in_len, id_by_node, root->node_count, &max_id);

    Edit edits[1024];
    int en = 0;

    for (int i = 0; i < root->node_count && en < (int)(sizeof(edits) / sizeof(edits[0])); i++) {
        if (n[i].kind != CC_AST_NODE_STMT) continue;
        if (!n[i].aux_s1 || strcmp(n[i].aux_s1, "spawn") != 0) continue;
        if (n[i].line_end <= 0) continue;

        size_t start = 0;
        int col1 = 1;
        int sr = cc__stmt_marker_start_off(root, ctx, n, i, in_src, in_len, "spawn", 5, &start, &col1);
        if (sr == 0) continue;
        if (sr < 0) { free(id_by_node); return -1; }

        int nursery_i = cc__find_enclosing_nursery_node_i(root, i);
        int nid = (nursery_i >= 0 && nursery_i < root->node_count) ? id_by_node[nursery_i] : 0;
        if (nid == 0) {
            const char* f = (n[i].file && n[i].file[0]) ? n[i].file : (ctx->input_path ? ctx->input_path : "<input>");
            fprintf(stderr, "%s:%d:%d: error: CC: 'spawn' must be inside an '@nursery { ... }' block\n",
                    f, n[i].line_start, col1);
            free(id_by_node);
            return -1;
        }

        /* Prefer a syntax-driven end for spawn statements; stub stmt end spans are often too wide (nested/multiline). */
        size_t end = cc__infer_spawn_stmt_end_off(in_src, in_len, start);
        if (end == 0) {
            if (n[i].col_end > 0) {
                end = cc__offset_of_line_col_1based(in_src, in_len, n[i].line_end, n[i].col_end);
            } else {
                end = cc__offset_of_line_1based(in_src, in_len, n[i].line_end + 1);
            }
        }
        if (start >= in_len) continue;
        if (end > in_len) end = in_len;
        if (end <= start) continue;

        /* Extract statement text and find (...) args. */
        const char* stmt = in_src + start;
        size_t stmt_len = end - start;

        /* Find first '(' and last ')' within span. */
        size_t lp = (size_t)-1;
        for (size_t k = 0; k < stmt_len; k++) {
            if (stmt[k] == '(') { lp = k; break; }
        }
        if (lp == (size_t)-1) continue;
        size_t rp = (size_t)-1;
        for (size_t k = stmt_len; k-- > lp; ) {
            if (stmt[k] == ')') { rp = k; break; }
        }
        if (rp == (size_t)-1 || rp <= lp) continue;

        size_t arg_off = lp + 1;
        size_t arg_len = rp - (lp + 1);
        cc__trim_span(stmt, &arg_off, &arg_len);

        /* Determine indentation from the original line. */
        size_t ind_len = cc__line_indent_len(in_src, in_len, n[i].line_start);
        size_t line_off = cc__offset_of_line_1based(in_src, in_len, n[i].line_start);
        const char* indent = (line_off + ind_len <= in_len) ? (in_src + line_off) : "";

        char* repl = NULL;
        size_t repl_len = 0, repl_cap = 0;

        /* Prefer the "simple function call" spawn form when it matches exactly. */
        {
            char fn[96] = {0};
            int has_arg = 0;
            long arg = 0;
            /* If the arg is a closure factory call, treat it as a CCClosure{0,1,2} value, not a void fn. */
            int looks_closure_make = 0;
            for (size_t k = 0; k + 16 <= arg_len; k++) {
                if (memcmp(stmt + arg_off + k, "__cc_closure_make_", 16) == 0) { looks_closure_make = 1; break; }
            }
            if (!looks_closure_make && cc__parse_simple_fn_call(stmt + arg_off, arg_len, fn, sizeof(fn), &has_arg, &arg)) {
                char b[512];
                if (!has_arg) {
                    int nn = snprintf(b, sizeof(b),
                                      "%.*s{ __cc_spawn_void_arg* __a = (__cc_spawn_void_arg*)malloc(sizeof(__cc_spawn_void_arg));\n"
                                      "%.*s  if (!__a) abort();\n"
                                      "%.*s  __a->fn = %s;\n"
                                      "%.*s  cc_nursery_spawn(__cc_nursery%d, __cc_spawn_thunk_void, __a);\n"
                                      "%.*s}\n",
                                      (int)ind_len, indent,
                                      (int)ind_len, indent,
                                      (int)ind_len, indent, fn,
                                      (int)ind_len, indent, nid,
                                      (int)ind_len, indent);
                    if (nn > 0 && (size_t)nn < sizeof(b)) {
                        cc__append_n(&repl, &repl_len, &repl_cap, b, (size_t)nn);
                    }
                } else {
                    int nn = snprintf(b, sizeof(b),
                                      "%.*s{ __cc_spawn_int_arg* __a = (__cc_spawn_int_arg*)malloc(sizeof(__cc_spawn_int_arg));\n"
                                      "%.*s  if (!__a) abort();\n"
                                      "%.*s  __a->fn = %s;\n"
                                      "%.*s  __a->arg = (int)%ld;\n"
                                      "%.*s  cc_nursery_spawn(__cc_nursery%d, __cc_spawn_thunk_int, __a);\n"
                                      "%.*s}\n",
                                      (int)ind_len, indent,
                                      (int)ind_len, indent,
                                      (int)ind_len, indent, fn,
                                      (int)ind_len, indent, arg,
                                      (int)ind_len, indent, nid,
                                      (int)ind_len, indent);
                    if (nn > 0 && (size_t)nn < sizeof(b)) {
                        cc__append_n(&repl, &repl_len, &repl_cap, b, (size_t)nn);
                    }
                }
            }
        }

        /* Otherwise interpret as closure spawn forms. */
        if (!repl) {
            int commas[2] = {0, 0};
            int comma_n = cc__split_top_level_commas(stmt + arg_off, arg_len, commas, 2);

            if (comma_n == 1 || comma_n == 2) {
                /* CCClosure1 / CCClosure2 */
                size_t c0_off = arg_off;
                size_t c0_len = (size_t)commas[0];
                size_t c1_off = arg_off + (size_t)commas[0] + 1;
                size_t c1_len = (comma_n == 2) ? ((size_t)commas[1] - (size_t)commas[0] - 1) : (arg_len - (size_t)commas[0] - 1);
                size_t c2_off = (comma_n == 2) ? (arg_off + (size_t)commas[1] + 1) : 0;
                size_t c2_len = (comma_n == 2) ? (arg_len - (size_t)commas[1] - 1) : 0;

                cc__trim_span(stmt, &c0_off, &c0_len);
                cc__trim_span(stmt, &c1_off, &c1_len);
                if (comma_n == 2) cc__trim_span(stmt, &c2_off, &c2_len);

                if (comma_n == 1) {
                    cc__append_n(&repl, &repl_len, &repl_cap, indent, ind_len);
                    cc__append_str(&repl, &repl_len, &repl_cap, "{ CCClosure1 __c = ");
                    cc__append_n(&repl, &repl_len, &repl_cap, stmt + c0_off, c0_len);
                    cc__append_fmt(&repl, &repl_len, &repl_cap,
                                   "; cc_nursery_spawn_closure1(__cc_nursery%d, __c, (intptr_t)(", nid);
                    cc__append_n(&repl, &repl_len, &repl_cap, stmt + c1_off, c1_len);
                    cc__append_str(&repl, &repl_len, &repl_cap, ")); }\n");
                } else {
                    cc__append_n(&repl, &repl_len, &repl_cap, indent, ind_len);
                    cc__append_str(&repl, &repl_len, &repl_cap, "{ CCClosure2 __c = ");
                    cc__append_n(&repl, &repl_len, &repl_cap, stmt + c0_off, c0_len);
                    cc__append_fmt(&repl, &repl_len, &repl_cap,
                                   "; cc_nursery_spawn_closure2(__cc_nursery%d, __c, (intptr_t)(", nid);
                    cc__append_n(&repl, &repl_len, &repl_cap, stmt + c1_off, c1_len);
                    cc__append_str(&repl, &repl_len, &repl_cap, "), (intptr_t)(");
                    cc__append_n(&repl, &repl_len, &repl_cap, stmt + c2_off, c2_len);
                    cc__append_str(&repl, &repl_len, &repl_cap, ")); }\n");
                }
            } else {
                /* CCClosure0 */
                cc__append_n(&repl, &repl_len, &repl_cap, indent, ind_len);
                cc__append_str(&repl, &repl_len, &repl_cap, "{ CCClosure0 __c = ");
                cc__append_n(&repl, &repl_len, &repl_cap, stmt + arg_off, arg_len);
                cc__append_fmt(&repl, &repl_len, &repl_cap,
                               "; cc_nursery_spawn_closure0(__cc_nursery%d, __c); }\n", nid);
            }
        }

        if (!repl) continue;
        edits[en++] = (Edit){ .start = start, .end = end, .repl = repl };
    }

    free(id_by_node);

    if (en == 0) return 0;

    qsort(edits, (size_t)en, sizeof(edits[0]), cc__edit_cmp_start_asc);

    /* Build output with splices. */
    char* out = NULL;
    size_t outl = 0, outc = 0;
    size_t cur = 0;
    for (int i = 0; i < en; i++) {
        if (edits[i].start < cur) continue; /* overlapping; ignore */
        cc__append_n(&out, &outl, &outc, in_src + cur, edits[i].start - cur);
        cc__append_str(&out, &outl, &outc, edits[i].repl);
        cur = edits[i].end;
    }
    if (cur < in_len) cc__append_n(&out, &outl, &outc, in_src + cur, in_len - cur);

    for (int i = 0; i < en; i++) free(edits[i].repl);

    *out_src = out;
    *out_len = outl;
    return 1;
}

int cc__rewrite_nursery_blocks_with_nodes(const CCASTRoot* root,
                                         const CCVisitorCtx* ctx,
                                         const char* in_src,
                                         size_t in_len,
                                         char** out_src,
                                         size_t* out_len) {
    if (!root || !ctx || !in_src || !out_src || !out_len) return 0;
    *out_src = NULL;
    *out_len = 0;
    if (!root->nodes || root->node_count <= 0) return 0;
    const NodeView* n = (const NodeView*)root->nodes;

    int* id_by_node = (int*)calloc((size_t)root->node_count, sizeof(int));
    if (!id_by_node) return 0;
    int max_id = 0;
    cc__build_nursery_id_map(root, ctx, in_src, in_len, id_by_node, root->node_count, &max_id);

    Edit edits[1024];
    int en = 0;

    for (int i = 0; i < root->node_count && en < (int)(sizeof(edits) / sizeof(edits[0])); i++) {
        if (n[i].kind != CC_AST_NODE_STMT) continue;
        if (!n[i].aux_s1 || strcmp(n[i].aux_s1, "nursery") != 0) continue;
        if (n[i].line_end <= 0) continue;

        int id = id_by_node[i];
        if (id <= 0) continue;

        size_t start = 0;
        int col1 = 1;
        int sr = cc__stmt_marker_start_off(root, ctx, n, i, in_src, in_len, "@nursery", 8, &start, &col1);
        if (sr == 0) continue;
        if (sr < 0) { free(id_by_node); return -1; }
        size_t end = 0;
        if (n[i].col_end > 0) {
            end = cc__offset_of_line_col_1based(in_src, in_len, n[i].line_end, n[i].col_end);
        } else {
            end = cc__offset_of_line_1based(in_src, in_len, n[i].line_end + 1);
        }
        if (start >= in_len) continue;
        if (end > in_len) end = in_len;
        if (end <= start) continue;

        /* Find '{' and last '}' within span. */
        size_t brace = (size_t)-1;
        for (size_t p = start; p < end; p++) {
            if (in_src[p] == '{') { brace = p; break; }
        }
        if (brace == (size_t)-1) continue;
        size_t close = (size_t)-1;
        if (!cc__scan_matching_rbrace(in_src, in_len, brace, &close)) continue;
        if (close == (size_t)-1 || close <= brace) continue;

        size_t ind_len = cc__line_indent_len(in_src, in_len, n[i].line_start);
        size_t line_off = cc__offset_of_line_1based(in_src, in_len, n[i].line_start);
        const char* indent = (line_off + ind_len <= in_len) ? (in_src + line_off) : "";

        /* IMPORTANT: wrap in a compound statement so this lowering is valid in statement contexts like:
           `if (cond) @nursery { ... }` (C does not allow declarations as the controlled statement). */
        char* pro = NULL;
        size_t pro_len = 0, pro_cap = 0;
        cc__append_fmt(&pro, &pro_len, &pro_cap,
                       "%.*s{\n"
                       "%.*sCCNursery* __cc_nursery%d = cc_nursery_create();\n"
                       "%.*sif (!__cc_nursery%d) abort();\n",
                       (int)ind_len, indent,
                       (int)ind_len, indent, id,
                       (int)ind_len, indent, id);
        if (!pro) continue;

        /* Optional: closing(ch1, ch2) clause -> register channels for auto-close. */
        {
            char chans[16][64];
            int cn = cc__parse_closing_clause(ctx, n, i, in_src, start, brace, chans, 16);
            if (cn < 0) { free(pro); free(id_by_node); return -1; }
            for (int ci = 0; ci < cn; ci++) {
                cc__append_fmt(&pro, &pro_len, &pro_cap,
                               "%.*scc_nursery_add_closing_chan(__cc_nursery%d, %s);\n",
                               (int)ind_len, indent, id, chans[ci]);
            }
        }

        /* Replace [start, brace+1) with prologue. */
        {
            edits[en++] = (Edit){ .start = start, .end = brace + 1, .repl = pro };
        }

        /* Insert epilogue right before the closing brace. */
        if (en + 1 < (int)(sizeof(edits) / sizeof(edits[0]))) {
            size_t close_line_off = cc__offset_of_line_1based(in_src, in_len, n[i].line_end);
            size_t close_ind_len = cc__line_indent_len(in_src, in_len, n[i].line_end);
            const char* cindent = (close_line_off + close_ind_len <= in_len) ? (in_src + close_line_off) : "";
            char epi[256];
            int en2 = snprintf(epi, sizeof(epi),
                               "%.*s  cc_nursery_wait(__cc_nursery%d);\n"
                               "%.*s  cc_nursery_free(__cc_nursery%d);\n",
                               (int)close_ind_len, cindent, id,
                               (int)close_ind_len, cindent, id);
            if (en2 > 0 && (size_t)en2 < sizeof(epi)) {
                char* r = (char*)malloc((size_t)en2 + 1);
                if (r) {
                    memcpy(r, epi, (size_t)en2);
                    r[en2] = 0;
                    edits[en++] = (Edit){ .start = close, .end = close, .repl = r };
                }
            }
        }
    }

    free(id_by_node);

    if (en == 0) return 0;
    qsort(edits, (size_t)en, sizeof(edits[0]), cc__edit_cmp_start_asc);

    char* out = NULL;
    size_t outl = 0, outc = 0;
    size_t cur = 0;
    for (int i = 0; i < en; i++) {
        if (edits[i].start < cur) continue;
        cc__append_n(&out, &outl, &outc, in_src + cur, edits[i].start - cur);
        cc__append_str(&out, &outl, &outc, edits[i].repl);
        cur = edits[i].end;
    }
    if (cur < in_len) cc__append_n(&out, &outl, &outc, in_src + cur, in_len - cur);

    for (int i = 0; i < en; i++) free(edits[i].repl);
    *out_src = out;
    *out_len = outl;
    return 1;
}

