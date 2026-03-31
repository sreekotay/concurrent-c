/* Extracted from the working implementation in `cc/src/visitor/visitor.c`.
   Goal: keep semantics identical while shrinking visitor.c over time. */

#include "pass_ufcs.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "preprocess/preprocess.h"
#include "util/path.h"
#include "visitor/pass_common.h"
#include "visitor/ufcs.h"

struct CC__UFCSSpan {
    size_t start; /* inclusive */
    size_t end;   /* exclusive */
};

static size_t cc__scan_receiver_start_left(const char* s, size_t range_start, size_t sep_pos);
static int cc__is_ident_start_char(char c);
static int cc__is_ident_char_char(char c);
static size_t cc__scan_member_chain_start_left(const char* s, size_t range_start, size_t sep_pos);
static int cc__span_from_anchor_and_end(const char* s,
                                       size_t range_start,
                                       size_t sep_pos,
                                       size_t end_pos_excl,
                                       struct CC__UFCSSpan* out_span);
static int cc__find_ufcs_span_in_range(const char* s,
                                       size_t range_start,
                                       size_t range_end,
                                       const char* method,
                                       int occurrence_1based,
                                       struct CC__UFCSSpan* out_span);
static size_t cc__ufcs_extend_chain_end(const char* s, size_t len, size_t end);
static void cc__ufcs_extract_receiver_expr(const char* expr, char* out, size_t out_cap);
static int cc__rewrite_ufcs_text_fallback(const char* in_src,
                                          size_t in_len,
                                          char** out_src,
                                          size_t* out_len);

static void cc__trim_span(const char** start, const char** end) {
    if (!start || !end || !*start || !*end) return;
    while (*start < *end && isspace((unsigned char)**start)) (*start)++;
    while (*end > *start && isspace((unsigned char)*((*end) - 1))) (*end)--;
}

static void cc__ufcs_extract_receiver_expr(const char* expr, char* out, size_t out_cap) {
    const char* start = expr;
    const char* end = expr ? expr + strlen(expr) : NULL;
    int par = 0, br = 0, brc = 0;
    if (!out || out_cap == 0) return;
    out[0] = '\0';
    if (!expr) return;
    for (const char* p = expr; *p; p++) {
        char c = *p;
        if (c == '"' || c == '\'') {
            char q = c;
            p++;
            while (*p) {
                if (*p == '\\' && p[1]) { p++; continue; }
                if (*p == q) break;
                p++;
            }
            continue;
        }
        if (c == '(') { par++; continue; }
        if (c == ')' && par > 0) { par--; continue; }
        if (c == '[') { br++; continue; }
        if (c == ']' && br > 0) { br--; continue; }
        if (c == '{') { brc++; continue; }
        if (c == '}' && brc > 0) { brc--; continue; }
        if (par || br || brc) continue;
        if (c == '.' || (c == '-' && p[1] == '>')) {
            const char* sep_end = p + ((c == '.') ? 1 : 2);
            const char* method = sep_end;
            while (*method && isspace((unsigned char)*method)) method++;
            if (!cc__is_ident_start_char(*method)) continue;
            while (cc__is_ident_char_char(*method)) method++;
            while (*method && isspace((unsigned char)*method)) method++;
            if (*method != '(') continue;
            end = p;
            break;
        }
    }
    cc__trim_span(&start, &end);
    if (end <= start) return;
    {
        size_t n = (size_t)(end - start);
        if (n >= out_cap) n = out_cap - 1;
        memcpy(out, start, n);
        out[n] = '\0';
    }
}

static int cc__rewrite_ufcs_text_fallback(const char* in_src,
                                          size_t in_len,
                                          char** out_src,
                                          size_t* out_len) {
    if (!in_src || !out_src || !out_len) return 0;
    *out_src = NULL;
    *out_len = 0;

    size_t out_cap = in_len + 1;
    char* out = (char*)malloc(out_cap);
    if (!out) return -1;
    size_t out_n = 0;
    int changed = 0;

    for (size_t line_start = 0; line_start < in_len; ) {
        size_t line_end = line_start;
        while (line_end < in_len && in_src[line_end] != '\n') line_end++;
        size_t line_len = line_end - line_start;
        int has_nl = (line_end < in_len && in_src[line_end] == '\n') ? 1 : 0;

        char* line = (char*)malloc(line_len + 1);
        if (!line) {
            free(out);
            return -1;
        }
        memcpy(line, in_src + line_start, line_len);
        line[line_len] = '\0';

        const char* emit = line;
        char* rewritten = NULL;
        const char* trim = line;
        while (*trim == ' ' || *trim == '\t' || *trim == '\r') trim++;
        if (*trim != '#') {
            size_t rew_cap = line_len * 2 + 256;
            rewritten = (char*)malloc(rew_cap);
            if (!rewritten) {
                free(line);
                free(out);
                return -1;
            }
            cc_ufcs_set_source_context(in_src, line_start);
            int rc = cc_ufcs_rewrite_line(line, rewritten, rew_cap);
            cc_ufcs_set_source_context(NULL, 0);
            if (rc == CC_UFCS_REWRITE_OK && strcmp(rewritten, line) != 0) {
                emit = rewritten;
                changed = 1;
            }
        }

        size_t emit_len = strlen(emit);
        if (out_n + emit_len + (size_t)has_nl + 1 > out_cap) {
            size_t new_cap = (out_cap * 2 > out_n + emit_len + (size_t)has_nl + 1)
                           ? out_cap * 2
                           : out_n + emit_len + (size_t)has_nl + 1;
            char* next = (char*)realloc(out, new_cap);
            if (!next) {
                free(rewritten);
                free(line);
                free(out);
                return -1;
            }
            out = next;
            out_cap = new_cap;
        }
        memcpy(out + out_n, emit, emit_len);
        out_n += emit_len;
        if (has_nl) out[out_n++] = '\n';

        free(rewritten);
        free(line);
        line_start = line_end + (size_t)has_nl;
    }

    out[out_n] = '\0';
    if (!changed) {
        free(out);
        return 0;
    }
    *out_src = out;
    *out_len = out_n;
    return 1;
}

int cc__rewrite_ufcs_spans_with_nodes(const CCASTRoot* root,
                                     const CCVisitorCtx* ctx,
                                     const char* in_src,
                                     size_t in_len,
                                     char** out_src,
                                     size_t* out_len) {
    if (!root || !ctx || !ctx->input_path || !in_src || !out_src || !out_len) return 0;
    *out_src = NULL;
    *out_len = 0;
    if (!root->nodes || root->node_count <= 0) return 0;

    struct NodeView {
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
    };
    const struct NodeView* n = (const struct NodeView*)root->nodes;

    /* Collect UFCS call nodes (line spans + method), then rewrite each span in-place. */
    struct UFCSNode {
        int line_start;
        int line_end;
        int col_start;
        int col_end;
        const char* method;
        const char* recv_type;   /* Receiver type name from TCC (e.g., "Point", "Vec_int") */
        int occurrence_1based;
        int is_under_await;      /* 1 if this UFCS call is inside an `await` expression */
        int recv_type_is_ptr;    /* 1 if receiver's resolved type is a pointer (from TCC) */
    };
    struct UFCSNode* nodes = NULL;
    int node_count = 0;
    int node_cap = 0;

    for (int i = 0; i < root->node_count; i++) {
        if (n[i].kind != 5) continue;         /* CALL */
        if ((n[i].aux2 & 4) == 0) continue;   /* only real UFCS calls */
        if (!n[i].aux_s1) continue;
        /* Macro-expanded UFCS calls can inherit the header file in TCC's stub AST
           even when line/col still point at user source text. Keep the node and
           let the later span finder prove whether it exists in the current source. */
        int ls = n[i].line_start;
        int le = n[i].line_end;
        if (ls <= 0) continue;
        if (le < ls) le = ls;
        if (node_count == node_cap) {
            node_cap = node_cap ? node_cap * 2 : 32;
            nodes = (struct UFCSNode*)realloc(nodes, (size_t)node_cap * sizeof(*nodes));
            if (!nodes) return 0;
        }
        int occ = (n[i].aux2 >> 8) & 0x00ffffff;
        if (occ <= 0) occ = 1;
        /* bit1 of aux2 = parser fallback says pass receiver directly */
        int recv_type_is_ptr = (n[i].aux2 & 2) ? 1 : 0;
        /* Check if this node is under an AWAIT ancestor */
        int under_await = 0;
        for (int p = n[i].parent; p >= 0 && p < root->node_count; p = n[p].parent) {
            if (n[p].kind == 6) { under_await = 1; break; } /* CC_AST_NODE_AWAIT = 6 */
        }
        nodes[node_count++] = (struct UFCSNode){
            .line_start = ls,
            .line_end = le,
            .col_start = n[i].col_start,
            .col_end = n[i].col_end,
            .method = n[i].aux_s1,
            .recv_type = n[i].aux_s2,  /* Receiver type name from TCC */
            .occurrence_1based = occ,
            .is_under_await = under_await,
            .recv_type_is_ptr = recv_type_is_ptr,
        };
    }

    char* cur = (char*)malloc(in_len + 1);
    if (!cur) { free(nodes); return 0; }
    memcpy(cur, in_src, in_len);
    cur[in_len] = '\0';
    size_t cur_len = in_len;

    /* Sort nodes from later source positions to earlier ones.
       Rewriting bottom-up keeps AST line/col anchors valid for nodes that
       have not been processed yet, even when earlier rewrites change line width. */
    for (int i = 0; i < node_count; i++) {
        for (int j = i + 1; j < node_count; j++) {
            int swap = 0;
            if (nodes[j].line_start > nodes[i].line_start) swap = 1;
            else if (nodes[j].line_start == nodes[i].line_start) {
                if (nodes[j].col_start > 0 && nodes[i].col_start > 0) {
                    if (nodes[j].col_start > nodes[i].col_start) swap = 1;
                    else if (nodes[j].col_start == nodes[i].col_start &&
                             nodes[j].col_end > nodes[i].col_end) swap = 1;
                } else if (nodes[j].line_end > nodes[i].line_end) {
                    swap = 1;
                }
            }
            if (swap) {
                struct UFCSNode tmp = nodes[i];
                nodes[i] = nodes[j];
                nodes[j] = tmp;
            }
        }
    }

    /* Track rewritten spans to avoid double-rewriting chains. */
    struct CC__UFCSSpan* done = NULL;
    int done_count = 0;
    int done_cap = 0;

    for (int i = 0; i < node_count; i++) {
        int ls = nodes[i].line_start;
        int le = nodes[i].line_end;
        if (ls <= 0) continue;
        if (le < ls) le = ls;
        size_t rs = cc__offset_of_line_1based(cur, cur_len, ls);
        size_t re = (le == ls) ? cc__offset_of_line_1based(cur, cur_len, le + 1) : cc__offset_of_line_1based(cur, cur_len, le + 1);
        if (re > cur_len) re = cur_len;
        if (rs >= re) continue;

        struct CC__UFCSSpan sp;
        if (nodes[i].col_start > 0 && nodes[i].col_end > 0 && nodes[i].line_end > 0) {
            size_t sep_pos = cc__offset_of_line_col_1based(cur, cur_len, nodes[i].line_start, nodes[i].col_start);
            size_t end_pos = cc__offset_of_line_col_1based(cur, cur_len, nodes[i].line_end, nodes[i].col_end);
            if (!cc__span_from_anchor_and_end(cur, rs, sep_pos, end_pos, &sp))
                continue;
        } else {
            if (!cc__find_ufcs_span_in_range(cur, rs, re, nodes[i].method, nodes[i].occurrence_1based, &sp))
                continue;
        }
        if (sp.end > cur_len || sp.start >= sp.end) continue;

        /* Extend span to include chained UFCS segments like ".foo(...).bar(...)" */
        sp.end = cc__ufcs_extend_chain_end(cur, cur_len, sp.end);

        /* Skip if this span is fully inside an already rewritten span. */
        int covered = 0;
        for (int k = 0; k < done_count; k++) {
            if (sp.start >= done[k].start && sp.end <= done[k].end) {
                covered = 1;
                break;
            }
        }
        if (covered) continue;

        size_t expr_len = sp.end - sp.start;
        size_t out_cap = expr_len * 2 + 256; /* Extra space for task variants */
        char* out_buf = (char*)malloc(out_cap);
        if (!out_buf) continue;
        char* expr = (char*)malloc(expr_len + 1);
        if (!expr) { free(out_buf); continue; }
        memcpy(expr, cur + sp.start, expr_len);
        expr[expr_len] = '\0';
        {
            const char* p = expr;
            while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
            if ((strncmp(p, "@defer", 6) == 0 &&
                 (p[6] == '\0' || p[6] == ' ' || p[6] == '\t' || p[6] == '\n' || p[6] == '\r' || p[6] == '(')) ||
                strstr(p, "@defer") != NULL) {
                free(expr);
                free(out_buf);
                continue;
            }
        }
        /* Use full rewrite with await context, type info, and receiver type. */
        {
            cc_ufcs_set_source_context(cur, sp.start);
            int rewrite_rc = cc_ufcs_rewrite_line_full(expr, out_buf, out_cap, nodes[i].is_under_await,
                                                       nodes[i].recv_type_is_ptr, nodes[i].recv_type);
            cc_ufcs_set_source_context(NULL, 0);
            if (rewrite_rc == CC_UFCS_REWRITE_UNRESOLVED) {
                char rel[1024];
                char recv_expr[256];
                const char* file = cc_path_rel_to_repo(ctx->input_path ? ctx->input_path : "<input>", rel, sizeof(rel));
                int col = nodes[i].col_start > 0 ? nodes[i].col_start : 1;
                cc__ufcs_extract_receiver_expr(expr, recv_expr, sizeof(recv_expr));
                if (nodes[i].recv_type && nodes[i].recv_type[0]) {
                    cc_pass_error_cat(file, nodes[i].line_start, col, CC_ERR_TYPE,
                                      "no UFCS method '%s' for receiver type '%s'",
                                      nodes[i].method ? nodes[i].method : "<unknown>",
                                      nodes[i].recv_type);
                } else {
                    cc_pass_error_cat(file, nodes[i].line_start, col, CC_ERR_TYPE,
                                      "cannot resolve UFCS method '%s' because the receiver type is unknown",
                                      nodes[i].method ? nodes[i].method : "<unknown>");
                }
                if (recv_expr[0]) {
                    cc_pass_note(file, nodes[i].line_start, col, "receiver expression: %s", recv_expr);
                }
                cc_pass_note(file, nodes[i].line_start, col, "offending call: %s", expr);
                cc_pass_note(file, nodes[i].line_start, col,
                             "hint: UFCS dispatch is strict; register an exact or wildcard owner, or call the lowered function explicitly");
                free(expr);
                free(out_buf);
                free(nodes);
                free(done);
                free(cur);
                return -1;
            }
            if (rewrite_rc == CC_UFCS_REWRITE_OK) {
                size_t repl_len = strlen(out_buf);
                size_t new_len = cur_len - expr_len + repl_len;
                char* next = (char*)malloc(new_len + 1);
                if (next) {
                    memcpy(next, cur, sp.start);
                    memcpy(next + sp.start, out_buf, repl_len);
                    memcpy(next + sp.start + repl_len, cur + sp.end, cur_len - sp.end);
                    next[new_len] = '\0';
                    free(cur);
                    cur = next;
                    cur_len = new_len;
                }
            }
        }
        free(expr);
        free(out_buf);

        if (done_count == done_cap) {
            done_cap = done_cap ? done_cap * 2 : 16;
            done = (struct CC__UFCSSpan*)realloc(done, (size_t)done_cap * sizeof(*done));
        }
        if (done) {
            done[done_count++] = sp;
        }
    }

    free(nodes);
    free(done);
    *out_src = cur;
    *out_len = cur_len;
    return 1;
}

static int cc__is_ident_start_char(char c) {
    return isalpha((unsigned char)c) || c == '_';
}

static int cc__is_ident_char_char(char c) {
    return isalnum((unsigned char)c) || c == '_';
}

static size_t cc__scan_member_chain_start_left(const char* s, size_t range_start, size_t sep_pos) {
    size_t r_end = sep_pos;
    size_t seg_start;
    size_t q;
    if (!s || sep_pos <= range_start) return sep_pos;
    while (r_end > range_start && isspace((unsigned char)s[r_end - 1])) r_end--;
    if (r_end == range_start) return sep_pos;

    seg_start = r_end;
    while (seg_start > range_start && cc__is_ident_char_char(s[seg_start - 1])) seg_start--;
    if (seg_start == r_end || !cc__is_ident_start_char(s[seg_start])) return sep_pos;

    for (;;) {
        q = seg_start;
        while (q > range_start && isspace((unsigned char)s[q - 1])) q--;
        if (q > range_start && s[q - 1] == '.') {
            q--;
        } else if (q > range_start + 1 && s[q - 2] == '-' && s[q - 1] == '>') {
            q -= 2;
        } else {
            break;
        }
        while (q > range_start && isspace((unsigned char)s[q - 1])) q--;
        if (q <= range_start || !cc__is_ident_char_char(s[q - 1])) break;
        seg_start = q;
        while (seg_start > range_start && cc__is_ident_char_char(s[seg_start - 1])) seg_start--;
        if (!cc__is_ident_start_char(s[seg_start])) return sep_pos;
    }

    return seg_start;
}


static size_t cc__scan_receiver_start_left(const char* s, size_t range_start, size_t sep_pos) {
    if (!s || sep_pos <= range_start) return range_start;
    {
        size_t chain_start = cc__scan_member_chain_start_left(s, range_start, sep_pos);
        if (chain_start < sep_pos) return chain_start;
    }
    size_t r_end = sep_pos;
    while (r_end > range_start && isspace((unsigned char)s[r_end - 1])) r_end--;
    if (r_end == range_start) return range_start;

    int par = 0, br = 0, brc = 0;
    size_t r = r_end;
    while (r > range_start) {
        char c = s[r - 1];
        if (c == ')') { par++; r--; continue; }
        if (c == ']') { br++; r--; continue; }
        if (c == '}') { brc++; r--; continue; }
        if (c == '(' && par > 0) { par--; r--; continue; }
        if (c == '[' && br > 0) { br--; r--; continue; }
        if (c == '{' && brc > 0) { brc--; r--; continue; }
        if (par || br || brc) { r--; continue; }
        if (c == ',' || c == ';' || c == '=' || c == '\n' ||
            c == '+' || c == '-' || c == '*' || c == '/' || c == '%' ||
            c == '&' || c == '|' || c == '^' || c == '!' || c == '~' ||
            c == '<' || c == '>' || c == '?' || c == ':' ) {
            break;
        }
        r--;
    }
    while (r < r_end && isspace((unsigned char)s[r])) r++;
    return r;
}

static int cc__span_from_anchor_and_end(const char* s,
                                       size_t range_start,
                                       size_t sep_pos,
                                       size_t end_pos_excl,
                                       struct CC__UFCSSpan* out_span) {
    size_t actual_sep_pos = sep_pos;
    if (!s || !out_span) return 0;
    if (sep_pos < range_start) return 0;
    if (end_pos_excl <= sep_pos) return 0;
    if (!(s[actual_sep_pos] == '.' ||
          (actual_sep_pos + 1 < end_pos_excl && s[actual_sep_pos] == '-' && s[actual_sep_pos + 1] == '>'))) {
        size_t p = actual_sep_pos;
        while (p > range_start && isspace((unsigned char)s[p - 1])) p--;
        while (p > range_start && cc__is_ident_char_char(s[p - 1])) p--;
        while (p > range_start && isspace((unsigned char)s[p - 1])) p--;
        if (p > range_start && s[p - 1] == '.') {
            actual_sep_pos = p - 1;
        } else if (p > range_start + 1 && s[p - 2] == '-' && s[p - 1] == '>') {
            actual_sep_pos = p - 2;
        }
    }
    out_span->start = cc__scan_receiver_start_left(s, range_start, actual_sep_pos);
    out_span->end = end_pos_excl;
    return out_span->start < out_span->end;
}

static int cc__find_ufcs_span_in_range(const char* s,
                                       size_t range_start,
                                       size_t range_end,
                                       const char* method,
                                       int occurrence_1based,
                                       struct CC__UFCSSpan* out_span) {
    if (!s || !method || !out_span) return 0;
    const size_t method_len = strlen(method);
    if (method_len == 0) return 0;
    if (occurrence_1based <= 0) occurrence_1based = 1;
    int seen = 0;

    /* Find ".method" or "->method" followed by optional whitespace then '(' */
    for (size_t i = range_start; i + method_len + 2 < range_end; i++) {
        int is_arrow = 0;
        size_t sep_pos = 0;
        if (s[i] == '.' ) { is_arrow = 0; sep_pos = i; }
        else if (s[i] == '-' && i + 1 < range_end && s[i + 1] == '>') { is_arrow = 1; sep_pos = i; }
        else continue;

        size_t mpos = sep_pos + (is_arrow ? 2 : 1);
        while (mpos < range_end && isspace((unsigned char)s[mpos])) mpos++;
        if (mpos + method_len >= range_end) continue;
        if (memcmp(s + mpos, method, method_len) != 0) continue;

        size_t after = mpos + method_len;
        while (after < range_end && isspace((unsigned char)s[after])) after++;
        if (after >= range_end || s[after] != '(') continue;

        /* Match Nth occurrence. */
        seen++;
        if (seen != occurrence_1based) continue;

        /* Receiver: allow non-trivial expressions like (foo()).bar, arr[i].m, (*p).m.
           Find the start by scanning left with bracket balancing until a delimiter. */
        size_t r_end = sep_pos;
        while (r_end > range_start && isspace((unsigned char)s[r_end - 1])) r_end--;
        if (r_end == range_start) continue;

        int par = 0, br = 0, brc = 0;
        size_t r = r_end;
        while (r > range_start) {
            char c = s[r - 1];
            if (c == ')') { par++; r--; continue; }
            if (c == ']') { br++; r--; continue; }
            if (c == '}') { brc++; r--; continue; }
            if (c == '(' && par > 0) { par--; r--; continue; }
            if (c == '[' && br > 0) { br--; r--; continue; }
            if (c == '{' && brc > 0) { brc--; r--; continue; }
            if (par || br || brc) { r--; continue; }

            /* At top-level: stop on likely expression delimiters. */
            if (c == ',' || c == ';' || c == '=' || c == '\n' ||
                c == '+' || c == '-' || c == '*' || c == '/' || c == '%' ||
                c == '&' || c == '|' || c == '^' || c == '!' || c == '~' ||
                c == '<' || c == '>' || c == '?' || c == ':' ) {
                break;
            }
            /* Otherwise keep consuming (identifiers, dots, brackets, parens, spaces). */
            r--;
        }
        /* Trim any leading whitespace included in the backward scan. */
        while (r < r_end && isspace((unsigned char)s[r])) r++;
        if (r >= r_end) continue;

        /* Find matching ')' for the call, skipping strings/chars. */
        size_t p = after;
        int depth = 0;
        while (p < range_end) {
            char c = s[p++];
            if (c == '(') depth++;
            else if (c == ')') {
                depth--;
                if (depth == 0) {
                    out_span->start = r;
                    out_span->end = p;
                    return 1;
                }
            } else if (c == '"' || c == '\'') {
                char q = c;
                while (p < range_end) {
                    char d = s[p++];
                    if (d == '\\' && p < range_end) { p++; continue; }
                    if (d == q) break;
                }
            }
        }
        return 0;
    }
    return 0;
}

static size_t cc__ufcs_extend_chain_end(const char* s, size_t len, size_t end) {
    if (!s || end >= len) return end;
    size_t p = end;
    for (;;) {
        while (p < len && isspace((unsigned char)s[p])) p++;
        if (p >= len) break;
        if (s[p] == '.') {
            p++;
        } else if (p + 1 < len && s[p] == '-' && s[p + 1] == '>') {
            p += 2;
        } else {
            break;
        }

        while (p < len && isspace((unsigned char)s[p])) p++;
        if (p >= len || (!isalpha((unsigned char)s[p]) && s[p] != '_')) break;
        while (p < len && (isalnum((unsigned char)s[p]) || s[p] == '_')) p++;
        while (p < len && isspace((unsigned char)s[p])) p++;
        if (p >= len || s[p] != '(') break;

        /* Scan to matching ')' of this call. */
        int depth = 0;
        while (p < len) {
            char c = s[p++];
            if (c == '(') depth++;
            else if (c == ')') {
                depth--;
                if (depth == 0) break;
            } else if (c == '"' || c == '\'') {
                char q = c;
                while (p < len) {
                    char d = s[p++];
                    if (d == '\\' && p < len) { p++; continue; }
                    if (d == q) break;
                }
            }
        }
        end = p;
    }
    return end;
}

/* Path helpers are now in pass_common.h */

/* NEW: Collect UFCS edits into EditBuffer.
   NOTE: UFCS is complex (incremental processing with chain extension).
   For now, this function runs the rewrite and diffs the result to extract edits.
   Future: refactor to collect edits directly without intermediate rewrite. */
int cc__collect_ufcs_edits(const CCASTRoot* root,
                           const CCVisitorCtx* ctx,
                           CCEditBuffer* eb) {
    if (!root || !ctx || !eb || !eb->src) return 0;

    cc_ufcs_set_symbols(ctx->symbols);
    char* rewritten = NULL;
    size_t rewritten_len = 0;
    int r = cc__rewrite_ufcs_spans_with_nodes(root, ctx, eb->src, eb->src_len, &rewritten, &rewritten_len);
    /* Keep the AST-driven path primary, but if it finds nothing, give the
       source-context-aware line fallback a chance to lower simple local UFCS
       that the stub AST failed to surface. */
    if ((r == 0 || !rewritten) && eb->src && eb->src_len > 0) {
        char* fallback = NULL;
        size_t fallback_len = 0;
        int fr = cc__rewrite_ufcs_text_fallback(eb->src, eb->src_len, &fallback, &fallback_len);
        if (fr < 0) {
            cc_ufcs_set_symbols(NULL);
            return -1;
        }
        if (fr > 0 && fallback) {
            rewritten = fallback;
            rewritten_len = fallback_len;
            r = 1;
        }
    }
    {
        const char* base_src = rewritten ? rewritten : eb->src;
        size_t base_len = rewritten ? rewritten_len : eb->src_len;
        char* family_rewritten = cc_rewrite_generic_family_ufcs_concrete(base_src, base_len);
        if (family_rewritten) {
            if (rewritten) free(rewritten);
            rewritten = family_rewritten;
            rewritten_len = strlen(rewritten);
            r = 1;
        }
    }
    cc_ufcs_set_symbols(NULL);
    if (r < 0) return -1;
    if (r == 0 || !rewritten) return 0;

    /* UFCS does many small same-line transforms. Rather than diff, for now we use a
       single "replace all" edit which is semantically correct but coarse.
       Future: track individual span rewrites for finer-grained edits. */
    if (rewritten_len != eb->src_len || memcmp(rewritten, eb->src, eb->src_len) != 0) {
        /* Replace entire source - this works but loses granularity.
           Since UFCS is typically first in its group, this is acceptable. */
        if (cc_edit_buffer_add(eb, 0, eb->src_len, rewritten, 100, "ufcs") == 0) {
            free(rewritten);
            return 1;
        }
    }
    free(rewritten);
    return 0;
}
