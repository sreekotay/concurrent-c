/* Extracted from the working implementation in `cc/src/visitor/visitor.c`.
   Goal: keep semantics identical while shrinking visitor.c over time. */

#include "pass_ufcs.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "visitor/pass_common.h"
#include "visitor/ufcs.h"

struct CC__UFCSSpan {
    size_t start; /* inclusive */
    size_t end;   /* exclusive */
};

static size_t cc__scan_receiver_start_left(const char* s, size_t range_start, size_t sep_pos);
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
        if (!n[i].aux_s1) continue;           /* only UFCS calls */
        if (!cc_pass_same_file(ctx->input_path, n[i].file) &&
            !(root->lowered_path && cc_pass_same_file(root->lowered_path, n[i].file)))
            continue;
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
        /* bit1 of aux2 = receiver's resolved type is a pointer (from TCC) */
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

    /* Sort nodes so rewrites on the same line proceed right-to-left.
       This keeps line/col offsets valid after earlier replacements. */
    for (int i = 0; i < node_count; i++) {
        for (int j = i + 1; j < node_count; j++) {
            int li = nodes[i].line_end - nodes[i].line_start;
            int lj = nodes[j].line_end - nodes[j].line_start;
            int swap = 0;
            if (lj > li) swap = 1;
            else if (lj == li) {
                if (nodes[j].line_start < nodes[i].line_start) swap = 1;
                else if (nodes[j].line_start == nodes[i].line_start) {
                    /* Same line: process right-to-left by column when available. */
                    if (nodes[j].col_start > 0 && nodes[i].col_start > 0 &&
                        nodes[j].col_start > nodes[i].col_start) {
                        swap = 1;
                    }
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
        /* Use full rewrite with await context, type info, and receiver type */
        if (cc_ufcs_rewrite_line_full(expr, out_buf, out_cap, nodes[i].is_under_await, 
                                       nodes[i].recv_type_is_ptr, nodes[i].recv_type) == 0) {
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


static size_t cc__scan_receiver_start_left(const char* s, size_t range_start, size_t sep_pos) {
    if (!s || sep_pos <= range_start) return range_start;
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
    if (!s || !out_span) return 0;
    if (sep_pos < range_start) return 0;
    if (end_pos_excl <= sep_pos) return 0;
    out_span->start = cc__scan_receiver_start_left(s, range_start, sep_pos);
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

    char* rewritten = NULL;
    size_t rewritten_len = 0;
    int r = cc__rewrite_ufcs_spans_with_nodes(root, ctx, eb->src, eb->src_len, &rewritten, &rewritten_len);
    if (r <= 0 || !rewritten) return 0;

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
