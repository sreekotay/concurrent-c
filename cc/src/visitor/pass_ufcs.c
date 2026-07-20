/* Extracted from the working implementation in `cc/src/visitor/visitor.c`.
   Goal: keep semantics identical while shrinking visitor.c over time. */

#include "pass_ufcs.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "preprocess/preprocess.h"
#include "util/path.h"
#include "util/text.h"
#include "util/text_scan.h"
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
static int cc__rewrite_ufcs_text_fallback(const CCVisitorCtx* ctx,
                                          const char* in_src,
                                          size_t in_len,
                                          char** out_src,
                                          size_t* out_len);
static int cc__line_has_await_keyword(const char* line);
static int cc__line_starts_with_decl_kw(const char* line);
static size_t cc__count_lines_to_offset(const char* s, size_t n, size_t off);

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

/* Cheap line-level detector for the `await` keyword.  Used by the text fallback
 * to mirror what the AST path discovers via parent traversal: if a line
 * contains a real `await ` token (not part of an identifier, string, or
 * comment) before the UFCS separator, we treat the call as await-context so
 * channel ops lower to the task-returning variants. */
static int cc__line_has_await_keyword(const char* line) {
    if (!line) return 0;
    size_t n = strlen(line);
    CCInertScan s;
    cc_inert_scan_init(&s, NULL);
    /* `line` is a single in-buffer line slice; not the start of a file, so a
     * leading `#` here would be code (preprocessor directives never reach this
     * fallback path).  Disable the `#` heuristic to preserve byte-equivalent
     * behavior with the previous hand-rolled scanner. */
    s.at_line_start = 0;
    for (size_t i = 0; i < n; ) {
        if (cc_inert_scan_step(&s, line, n, &i)) continue;
        if (line[i] != 'a' || i + 5 > n) { i++; continue; }
        if (memcmp(line + i, "await", 5) != 0) { i++; continue; }
        /* Word boundary: preceding char (if any) and following char must not
         * be identifier chars. */
        if (i > 0 && cc__is_ident_char_char(line[i - 1])) { i++; continue; }
        char nx = line[i + 5];
        if (cc__is_ident_char_char(nx)) { i++; continue; }
        return 1;
    }
    return 0;
}

/* Detect lines that look like a declaration introducer (typedef, struct/union
 * forward decl, etc).  These never carry executable UFCS, and producing a
 * spurious "unresolved UFCS" error from the fallback for such a line would be
 * a regression vs. the AST path. */
static int cc__line_starts_with_decl_kw(const char* line) {
    if (!line) return 0;
    while (*line == ' ' || *line == '\t') line++;
    static const char* kws[] = { "typedef", "struct", "union", "enum", "extern", "static", "#" };
    for (size_t i = 0; i < sizeof(kws) / sizeof(kws[0]); i++) {
        size_t kl = strlen(kws[i]);
        if (strncmp(line, kws[i], kl) == 0 &&
            (line[kl] == '\0' || line[kl] == ' ' || line[kl] == '\t' || line[kl] == '(')) {
            return 1;
        }
    }
    return 0;
}

static size_t cc__count_lines_to_offset(const char* s, size_t n, size_t off) {
    size_t lines = 1;
    size_t end = off < n ? off : n;
    for (size_t i = 0; i < end; i++) {
        if (s[i] == '\n') lines++;
    }
    return lines;
}

static int cc__rewrite_ufcs_text_fallback(const CCVisitorCtx* ctx,
                                          const char* in_src,
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
            int is_under_await = cc__line_has_await_keyword(line);
            cc_ufcs_set_source_context(in_src, line_start);
            /* Use the full rewrite entry point so await-context lowers channel
             * ops to the task-returning variants, matching the AST path.  We
             * pass recv_type_is_ptr=0 / recv_type=NULL because the text path
             * has no AST hint; the rewriter falls back to the type registry
             * (which is now seeded with pointer-ness for Map<K,V>). */
            int rc = cc_ufcs_rewrite_line_full(line, rewritten, rew_cap,
                                               is_under_await,
                                               /*recv_type_is_ptr=*/0,
                                               /*recv_type=*/NULL);
            cc_ufcs_set_source_context(NULL, 0);
            if (rc == CC_UFCS_REWRITE_UNRESOLVED && !cc__line_starts_with_decl_kw(line)) {
                /* Mirror the AST path's hard-error behavior so a strictly
                 * unresolved UFCS in fallback territory (typically a closure
                 * body or macro argument the AST couldn't see into) does not
                 * silently slip through to the host C compiler with a bogus
                 * `recv.method(args)` lowering. */
                char rel[1024];
                const char* file = "<input>";
                if (ctx && ctx->input_path) {
                    file = cc_path_rel_to_repo(ctx->input_path, rel, sizeof(rel));
                }
                size_t lineno = cc__count_lines_to_offset(in_src, in_len, line_start);
                cc_pass_error_cat(file, (int)lineno, 1, CC_ERR_TYPE,
                                  "cannot resolve UFCS call (text fallback)");
                cc_pass_note(file, (int)lineno, 1, "offending line: %s", line);
                cc_pass_note(file, (int)lineno, 1,
                             "hint: register an exact or wildcard owner via "
                             "cc_type_register, or call the lowered function explicitly");
                free(rewritten);
                free(line);
                free(out);
                return -1;
            }
            if (rc == CC_UFCS_REWRITE_OK && strcmp(rewritten, line) != 0) {
                if (getenv("CC_DEBUG_UFCS_FALLBACK")) {
                    fprintf(stderr, "[ufcs-fb] BEFORE: %s\n[ufcs-fb] AFTER:  %s\n", line, rewritten);
                }
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
        long off_start;   /* byte offsets in parse buffer; -1 unknown (layout mirror of tcc.h) */
        long off_end;
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
        const char* recv_type;   /* Receiver type name from TCC (e.g., "Point", "CCVec_int") */
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
        if (getenv("CC_DEBUG_UFCS_NODES")) {
            fprintf(stderr, "[cc:ufcs-node] line=%d..%d method=%s recv=%s occ=%d\n",
                    ls, le, n[i].aux_s1 ? n[i].aux_s1 : "?",
                    n[i].aux_s2 ? n[i].aux_s2 : "?", occ);
        }
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
        int have_span = 0;
        struct CC__UFCSSpan lax_sp;
        int have_lax = 0;
        if (nodes[i].col_start > 0 && nodes[i].col_end > 0 && nodes[i].line_end > 0) {
            size_t sep_pos = cc__offset_of_line_col_1based(cur, cur_len, nodes[i].line_start, nodes[i].col_start);
            size_t end_pos = cc__offset_of_line_col_1based(cur, cur_len, nodes[i].line_end, nodes[i].col_end);
            if (cc__span_from_anchor_and_end(cur, rs, sep_pos, end_pos, &sp)) {
                have_span = 1;
            } else if (sep_pos >= rs && sep_pos < end_pos && end_pos <= cur_len) {
                /* Preserve the legacy lax-anchor span (receiver scan + TCC end) as a
                   last-resort fallback: TCC occasionally reports col_start past the
                   true UFCS separator (e.g., points to the method identifier or to
                   the outer macro's closing paren for macro-arg UFCS). Range-based
                   method lookup handles most of those; keep lax as a safety net for
                   rarer cases where line numbers are also off. */
                lax_sp.start = cc__scan_receiver_start_left(cur, rs, sep_pos);
                lax_sp.end = end_pos;
                if (lax_sp.start < lax_sp.end) have_lax = 1;
            }
        }
        /* Fall back to range-based search by method name if the anchor-based
           resolver rejected the TCC-reported col_start/col_end. This is the
           primary path for UFCS calls inside macro-argument contexts like
           assert(obj->m() == 0), where TCC attributes the call to the outer
           macro's closing paren rather than the inner method separator. */
        if (!have_span) {
            if (cc__find_ufcs_span_in_range(cur, rs, re, nodes[i].method, nodes[i].occurrence_1based, &sp)) {
                have_span = 1;
            }
        }
        /* Last resort: use the lax-anchor span computed above. The line-based
           rewriter (cc_ufcs_rewrite_line_full) will verify a UFCS operator is
           present in the span and return NO_MATCH silently if not, so a slightly
           too-wide span here cannot corrupt output. */
        if (!have_span && have_lax) {
            sp = lax_sp;
            have_span = 1;
        }
        if (!have_span) continue;
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
            size_t p_len = strlen(p);
            int defers = cc_find_substr_top_level(p, 0, p_len, "@defer", 6) < p_len;
            if ((strncmp(p, "@defer", 6) == 0 &&
                 (p[6] == '\0' || p[6] == ' ' || p[6] == '\t' || p[6] == '\n' || p[6] == '\r' || p[6] == '(')) ||
                defers) {
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
        if (q > range_start && s[q - 1] == ')') {
            int depth = 1;
            size_t pp = q - 1;
            while (pp > range_start && depth > 0) {
                pp--;
                if (s[pp] == ')') depth++;
                else if (s[pp] == '(') depth--;
            }
            if (depth == 0) {
                seg_start = pp;
                continue;
            }
            break;
        }
        if (q > range_start && s[q - 1] == ']') {
            int depth = 1;
            size_t pp = q - 1;
            while (pp > range_start && depth > 0) {
                pp--;
                if (s[pp] == ']') depth++;
                else if (s[pp] == '[') depth--;
            }
            if (depth == 0) {
                seg_start = pp;
                continue;
            }
            break;
        }
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
        /* `->` is member-access — consume both chars so the receiver chain
           extends past it (see cc__find_ufcs_span_in_range for details). */
        if (c == '>' && r >= range_start + 2 && s[r - 2] == '-') {
            r -= 2;
            continue;
        }
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
    int resolved = 0;
    if (!s || !out_span) return 0;
    if (sep_pos < range_start) return 0;
    if (end_pos_excl <= sep_pos) return 0;
    if (s[actual_sep_pos] == '.') {
        resolved = 1;
    } else if (actual_sep_pos + 1 < end_pos_excl &&
               s[actual_sep_pos] == '-' && s[actual_sep_pos + 1] == '>') {
        resolved = 1;
    } else {
        /* TCC sometimes reports a column past the actual UFCS separator — e.g.,
           it points at the method identifier, the closing paren of the call,
           or (for calls inside macro expansions such as assert(obj->m() == 0))
           the closing paren of the macro invocation itself. Scan back conservatively
           to find a real `.`/`->` UFCS separator. If we cannot prove one is present,
           report failure so the caller can try range-based resolution instead. */
        size_t p = actual_sep_pos;
        while (p > range_start && isspace((unsigned char)s[p - 1])) p--;
        while (p > range_start && cc__is_ident_char_char(s[p - 1])) p--;
        while (p > range_start && isspace((unsigned char)s[p - 1])) p--;
        if (p > range_start && s[p - 1] == '.') {
            actual_sep_pos = p - 1;
            resolved = 1;
        } else if (p > range_start + 1 && s[p - 2] == '-' && s[p - 1] == '>') {
            actual_sep_pos = p - 2;
            resolved = 1;
        }
    }
    if (!resolved) return 0;
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

            /* `->` is member-access, not a delimiter: it binds the identifier
               on its left into the receiver chain (e.g. `obj->field.method()`).
               Consume both chars and continue scanning left.  Without this,
               `-`/`>` would break the scan and receiver would be truncated to
               just the trailing segment. */
            if (c == '>' && r >= range_start + 2 && s[r - 2] == '-') {
                r -= 2;
                continue;
            }

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

/* Per-span edit collector.  Walks UFCS-flagged CALL nodes in TCC's stub AST,
 * resolves each call's source span against `eb->src`, runs
 * `cc_ufcs_rewrite_line_full()` on the extracted text, and pushes a
 * `[span.start, span.end) → lowered_text` edit per non-subsumed call.
 *
 * The legacy `cc__rewrite_ufcs_spans_with_nodes()` rewriter performed
 * in-place buffer mutation between nodes (processing bottom-up so unmutated
 * regions kept their TCC-reported line/col offsets).  That worked but
 * produced a single whole-buffer edit when wrapped in this collector,
 * which would have collided with other Phase-3 collectors once batching
 * became the default Phase-3 path (see PIPELINE.md).
 *
 * This per-span variant resolves every span against the ORIGINAL `eb->src`
 * (offsets stay consistent because the source never mutates) and uses the
 * same `done[]` containment check the wholesale path used to suppress
 * subsumed nested chain segments.  The result: each UFCS call site
 * produces exactly one edit, surgical and non-overlapping, so other
 * collectors can run in the same edit buffer without collision.
 *
 * `CC_UFCS_TEXT_FALLBACK=1` (debug only, off by default) preserves the
 * legacy wholesale fallback path — emits a single whole-buffer edit.
 * This is intentionally compatible only with the non-batched pipeline;
 * it exists purely as a diagnostic for AST-resolver regressions.
 */
/* AST nodes speak LOGICAL coordinates (TCC honors #line directives), but the
 * buffer being edited may contain large spliced regions (e.g. @grammar
 * lowerings) that change physical line counts and bracket themselves with
 * #line. Map a logical (file,line) to a physical offset by walking the
 * directives; returns (size_t)-1 when the logical line never materializes
 * (caller falls back to the legacy physical mapping). */
static int cc__file_matches(const char* cur, const char* want) {
    if (!want || !want[0]) return 1;
    if (!cur || !cur[0]) return 1;   /* pre-directive prefix: the main file */
    if (strcmp(cur, want) == 0) return 1;
    {   /* tolerate absolute-vs-relative spellings: compare basenames */
        const char* cb = strrchr(cur, '/');
        const char* wb = strrchr(want, '/');
        return strcmp(cb ? cb + 1 : cur, wb ? wb + 1 : want) == 0;
    }
}

static size_t cc__offset_of_logical_line(const char* s, size_t n,
                                         const char* want_file, int want_line) {
    char cur_file[1024] = {0};
    int cur_line = 1;
    size_t i = 0;
    int saw_directive = 0;
    size_t last = (size_t)-1;
    while (i < n) {
        /* line-start directive? `# <num> ["file"]` or `#line <num> ["file"]` */
        size_t j = i;
        while (j < n && (s[j] == ' ' || s[j] == '\t')) j++;
        if (j < n && s[j] == '#') {
            size_t k = j + 1;
            while (k < n && (s[k] == ' ' || s[k] == '\t')) k++;
            if (k + 4 <= n && strncmp(s + k, "line", 4) == 0) k += 4;
            while (k < n && (s[k] == ' ' || s[k] == '\t')) k++;
            if (k < n && s[k] >= '0' && s[k] <= '9') {
                int ln = 0;
                while (k < n && s[k] >= '0' && s[k] <= '9') ln = ln * 10 + (s[k++] - '0');
                while (k < n && (s[k] == ' ' || s[k] == '\t')) k++;
                if (k < n && s[k] == '"') {
                    size_t fs = ++k, o = 0;
                    while (k < n && s[k] != '"') k++;
                    for (size_t m = fs; m < k && o + 1 < sizeof(cur_file); m++)
                        cur_file[o++] = s[m];
                    cur_file[o] = '\0';
                }
                saw_directive = 1;
                cur_line = ln;
                while (i < n && s[i] != '\n') i++;
                if (i < n) i++;
                continue;
            }
        }
        if (cur_line == want_line && cc__file_matches(cur_file, want_file)) {
            /* keep scanning: a spliced region's #line attributes GENERATED
             * text to the declaration's line, and its physical lines then
             * increment into ranges that collide with later user lines. The
             * real user line is re-established by the closing directive, so
             * the LAST logical match wins. */
            last = i;
        }
        while (i < n && s[i] != '\n') i++;
        if (i < n) i++;
        cur_line++;
    }
    (void)saw_directive;
    return last;
}

int cc__collect_ufcs_edits(const CCASTRoot* root,
                           const CCVisitorCtx* ctx,
                           CCEditBuffer* eb) {
    if (!root || !ctx || !ctx->input_path || !eb || !eb->src) return 0;
    if (!root->nodes || root->node_count <= 0) {
        /* Even with no AST nodes the text-only fallback may want to run. */
        if (!getenv("CC_UFCS_TEXT_FALLBACK")) return 0;
    }

    cc_ufcs_set_symbols(ctx->symbols);

    const char* in_src = eb->src;
    size_t in_len = eb->src_len;

    /* CC_UFCS_TEXT_FALLBACK debug path: run the legacy whole-buffer text
     * rewriter and emit a single coarse edit.  Mutually exclusive with
     * batched mode (would collide); intended only for AST-resolver
     * debugging where bypassing the AST path is the whole point. */
    if (getenv("CC_UFCS_TEXT_FALLBACK")) {
        char* fallback = NULL;
        size_t fallback_len = 0;
        int fr = cc__rewrite_ufcs_text_fallback(ctx, in_src, in_len, &fallback, &fallback_len);
        cc_ufcs_set_symbols(NULL);
        if (fr < 0) { free(fallback); return -1; }
        if (fr <= 0 || !fallback) { free(fallback); return 0; }
        if (fallback_len == in_len && memcmp(fallback, in_src, in_len) == 0) {
            free(fallback);
            return 0;
        }
        int rc = cc_edit_buffer_add(eb, 0, in_len, fallback, 100, "ufcs");
        free(fallback);
        return rc == 0 ? 1 : -1;
    }

    typedef CCNodeView NodeView;
    const NodeView* n = (const NodeView*)root->nodes;

    /* Collect UFCS-flagged CALL nodes.  Mirrors the rewriter exactly. */
    struct UFCSNode {
        int line_start;
        int line_end;
        int col_start;
        int col_end;
        const char* method;
        const char* recv_type;
        int occurrence_1based;
        int is_under_await;
        int recv_type_is_ptr;
        const char* file;
    };
    struct UFCSNode* nodes = NULL;
    int node_count = 0;
    int node_cap = 0;

    for (int i = 0; i < root->node_count; i++) {
        if (n[i].kind != 5) continue;          /* CALL */
        if ((n[i].aux2 & 4) == 0) continue;    /* UFCS marker */
        if (!n[i].aux_s1) continue;
        int ls = n[i].line_start;
        int le = n[i].line_end;
        if (ls <= 0) continue;
        if (le < ls) le = ls;
        if (node_count == node_cap) {
            int new_cap = node_cap ? node_cap * 2 : 32;
            struct UFCSNode* nn = (struct UFCSNode*)realloc(nodes, (size_t)new_cap * sizeof(*nn));
            if (!nn) { free(nodes); cc_ufcs_set_symbols(NULL); return 0; }
            nodes = nn;
            node_cap = new_cap;
        }
        int occ = (n[i].aux2 >> 8) & 0x00ffffff;
        if (occ <= 0) occ = 1;
        int recv_type_is_ptr = (n[i].aux2 & 2) ? 1 : 0;
        int under_await = 0;
        for (int p = n[i].parent; p >= 0 && p < root->node_count; p = n[p].parent) {
            if (n[p].kind == 6) { under_await = 1; break; }
        }
        nodes[node_count++] = (struct UFCSNode){
            .line_start = ls,
            .line_end = le,
            .col_start = n[i].col_start,
            .col_end = n[i].col_end,
            .method = n[i].aux_s1,
            .recv_type = n[i].aux_s2,
            .occurrence_1based = occ,
            .is_under_await = under_await,
            .recv_type_is_ptr = recv_type_is_ptr,
            .file = n[i].file,
        };
        if (getenv("CC_DEBUG_UFCS_NODES")) {
            fprintf(stderr, "[cc:ufcs-node2] file=%s line=%d..%d method=%s recv=%s occ=%d\n",
                    n[i].file ? n[i].file : "?", ls, le,
                    n[i].aux_s1 ? n[i].aux_s1 : "?",
                    n[i].aux_s2 ? n[i].aux_s2 : "?", occ);
        }
    }

    if (node_count == 0) {
        free(nodes);
        cc_ufcs_set_symbols(NULL);
        return 0;
    }

    /* Bottom-up sort: process later-source nodes first so the `done[]`
     * containment check catches nested chain segments (inner `.m1()` calls
     * that get absorbed by the outer chain's receiver scan).  Order doesn't
     * affect offset validity (we never mutate in_src), but matches the
     * original rewriter's iteration order so containment relationships
     * resolve identically. */
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

    struct CC__UFCSSpan* done = NULL;
    int done_count = 0;
    int done_cap = 0;

    int edits_added = 0;
    int err = 0;

    for (int i = 0; i < node_count && !err; i++) {
        int ls = nodes[i].line_start;
        int le = nodes[i].line_end;
        if (ls <= 0) continue;
        if (le < ls) le = ls;
        /* logical (#line-aware) mapping first — spliced regions (@grammar)
         * change physical line counts; the legacy physical mapping stays as
         * the fallback for buffers/coordinates without directives */
        size_t rs = cc__offset_of_logical_line(in_src, in_len, nodes[i].file, ls);
        size_t re = (rs == (size_t)-1) ? (size_t)-1
                  : cc__offset_of_logical_line(in_src, in_len, nodes[i].file, le + 1);
        if (rs == (size_t)-1) {
            rs = cc__offset_of_line_1based(in_src, in_len, ls);
            re = cc__offset_of_line_1based(in_src, in_len, le + 1);
        } else if (re == (size_t)-1) {
            re = in_len;
        }
        if (re > in_len) re = in_len;
        if (rs >= re) continue;

        struct CC__UFCSSpan sp;
        int have_span = 0;
        struct CC__UFCSSpan lax_sp;
        int have_lax = 0;
        if (nodes[i].col_start > 0 && nodes[i].col_end > 0 && nodes[i].line_end > 0) {
            size_t sep_pos = cc__offset_of_line_col_1based(in_src, in_len, nodes[i].line_start, nodes[i].col_start);
            size_t end_pos = cc__offset_of_line_col_1based(in_src, in_len, nodes[i].line_end, nodes[i].col_end);
            if (cc__span_from_anchor_and_end(in_src, rs, sep_pos, end_pos, &sp)) {
                have_span = 1;
            } else if (sep_pos >= rs && sep_pos < end_pos && end_pos <= in_len) {
                lax_sp.start = cc__scan_receiver_start_left(in_src, rs, sep_pos);
                lax_sp.end = end_pos;
                if (lax_sp.start < lax_sp.end) have_lax = 1;
            }
        }
        if (!have_span) {
            if (cc__find_ufcs_span_in_range(in_src, rs, re, nodes[i].method, nodes[i].occurrence_1based, &sp)) {
                have_span = 1;
            }
        }
        if (!have_span) {
            /* Coordinate ambiguity: TCC-ext nodes may carry PHYSICAL lines
             * while the buffer's #line directives (from spliced regions)
             * make the logical mapping above land on an unrelated user line
             * that happens to share the number.  The span finder is the
             * arbiter: if the logical range doesn't contain the call, retry
             * the plain physical mapping before dropping the node. */
            size_t prs = cc__offset_of_line_1based(in_src, in_len, ls);
            size_t pre = cc__offset_of_line_1based(in_src, in_len, le + 1);
            if (pre > in_len) pre = in_len;
            if (prs < pre && prs != rs) {
                if (nodes[i].col_start > 0 && nodes[i].col_end > 0 && nodes[i].line_end > 0) {
                    size_t sep_pos = cc__offset_of_line_col_1based(in_src, in_len, nodes[i].line_start, nodes[i].col_start);
                    size_t end_pos = cc__offset_of_line_col_1based(in_src, in_len, nodes[i].line_end, nodes[i].col_end);
                    if (cc__span_from_anchor_and_end(in_src, prs, sep_pos, end_pos, &sp)) {
                        have_span = 1;
                    }
                }
                if (!have_span &&
                    cc__find_ufcs_span_in_range(in_src, prs, pre, nodes[i].method, nodes[i].occurrence_1based, &sp)) {
                    have_span = 1;
                }
            }
        }
        if (!have_span && have_lax) {
            sp = lax_sp;
            have_span = 1;
        }
        if (!have_span) continue;
        if (sp.end > in_len || sp.start >= sp.end) continue;

        sp.end = cc__ufcs_extend_chain_end(in_src, in_len, sp.end);

        int covered = 0;
        for (int k = 0; k < done_count; k++) {
            if (sp.start >= done[k].start && sp.end <= done[k].end) {
                covered = 1;
                break;
            }
        }
        if (covered) continue;

        size_t expr_len = sp.end - sp.start;
        size_t out_cap = expr_len * 2 + 256;
        char* out_buf = (char*)malloc(out_cap);
        if (!out_buf) continue;
        char* expr = (char*)malloc(expr_len + 1);
        if (!expr) { free(out_buf); continue; }
        memcpy(expr, in_src + sp.start, expr_len);
        expr[expr_len] = '\0';
        {
            const char* p = expr;
            while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
            size_t p_len = strlen(p);
            int defers = cc_find_substr_top_level(p, 0, p_len, "@defer", 6) < p_len;
            if ((strncmp(p, "@defer", 6) == 0 &&
                 (p[6] == '\0' || p[6] == ' ' || p[6] == '\t' || p[6] == '\n' || p[6] == '\r' || p[6] == '(')) ||
                defers) {
                free(expr);
                free(out_buf);
                continue;
            }
        }
        cc_ufcs_set_source_context(in_src, sp.start);
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
            err = -1;
        } else if (rewrite_rc == CC_UFCS_REWRITE_OK) {
            if (cc_edit_buffer_add(eb, sp.start, sp.end, out_buf, 100, "ufcs") == 0) {
                edits_added++;
                if (done_count == done_cap) {
                    int new_cap = done_cap ? done_cap * 2 : 16;
                    struct CC__UFCSSpan* nd = (struct CC__UFCSSpan*)realloc(done, (size_t)new_cap * sizeof(*nd));
                    if (nd) { done = nd; done_cap = new_cap; }
                }
                if (done && done_count < done_cap) done[done_count++] = sp;
            }
        }
        free(expr);
        free(out_buf);
    }

    free(nodes);
    free(done);
    cc_ufcs_set_symbols(NULL);
    if (err < 0) return -1;
    return edits_added;
}
