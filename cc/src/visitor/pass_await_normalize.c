/* Extracted from the working implementation in `cc/src/visitor/visitor.c`.
   Goal: keep semantics identical while shrinking visitor.c over time. */

#include "pass_await_normalize.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util/text.h"
#include "visitor/pass_common.h"

/* Local aliases for the shared helpers */
#define cc__append_n cc_sb_append
#define cc__append_str cc_sb_append_cstr

/* Alias shared types for local use */
typedef CCNodeView NodeView;

static size_t cc__skip_ws_comments(const char* s, size_t n, size_t i) {
    while (i < n) {
        char c = s[i];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') { i++; continue; }
        if (c == '/' && i + 1 < n && s[i + 1] == '/') {
            i += 2;
            while (i < n && s[i] != '\n') i++;
            continue;
        }
        if (c == '/' && i + 1 < n && s[i + 1] == '*') {
            i += 2;
            while (i + 1 < n && !(s[i] == '*' && s[i + 1] == '/')) i++;
            if (i + 1 < n) i += 2;
            continue;
        }
        break;
    }
    return i;
}

static size_t cc__scan_string_lit(const char* s, size_t n, size_t i, char quote) {
    /* i points at opening quote */
    if (i >= n || s[i] != quote) return i;
    i++;
    while (i < n) {
        char c = s[i++];
        if (c == '\\') { if (i < n) i++; continue; }
        if (c == quote) break;
    }
    return i;
}

static size_t cc__scan_matching_delim(const char* s, size_t n, size_t i, char open, char close) {
    if (i >= n || s[i] != open) return i;
    int depth = 1;
    i++;
    while (i < n && depth > 0) {
        i = cc__skip_ws_comments(s, n, i);
        if (i >= n) break;
        char c = s[i];
        if (c == '"' || c == '\'') { i = cc__scan_string_lit(s, n, i, c); continue; }
        if (c == open) { depth++; i++; continue; }
        if (c == close) { depth--; i++; continue; }
        /* Handle nested (), [], {} as well while scanning. */
        if (c == '(') { i = cc__scan_matching_delim(s, n, i, '(', ')'); continue; }
        if (c == '[') { i = cc__scan_matching_delim(s, n, i, '[', ']'); continue; }
        if (c == '{') { i = cc__scan_matching_delim(s, n, i, '{', '}'); continue; }
        i++;
    }
    return i;
}

static int cc__is_ident_start(char c) {
    return (c == '_' || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'));
}
static int cc__is_ident_char(char c) {
    return cc__is_ident_start(c) || (c >= '0' && c <= '9');
}

static int cc__is_tok_boundary(char c) {
    return !(c == '_' || isalnum((unsigned char)c));
}

static int cc__scan_line_for_await_tokens(const char* s,
                                          size_t n,
                                          size_t line_off,
                                          size_t* out_offs,
                                          int out_cap) {
    if (!s || !out_offs || out_cap <= 0) return 0;
    int k = 0;
    size_t i = line_off;
    while (i + 5 <= n) {
        if (s[i] == '\n') break;
        if (memcmp(s + i, "await", 5) == 0) {
            char before = (i > line_off) ? s[i - 1] : ' ';
            char after = (i + 5 < n) ? s[i + 5] : ' ';
            if (cc__is_tok_boundary(before) && cc__is_tok_boundary(after)) {
                if (k < out_cap) out_offs[k++] = i;
            }
        }
        i++;
    }
    return k;
}

static size_t cc__await_kw_off_for_node(const void* nodes,
                                        int node_count,
                                        const char* in_src,
                                        size_t in_len,
                                        int idx) {
    /* Assign `await` tokens on a line to await-nodes on that line by increasing col_start.
       This is robust when stub-AST col_start points somewhere inside/near the operand. */
    if (!nodes || node_count <= 0 || !in_src || in_len == 0 || idx < 0 || idx >= node_count) return (size_t)-1;
    const NodeView* n = (const NodeView*)nodes;
    if (n[idx].kind != 6) return (size_t)-1;
    if (n[idx].line_start <= 0) return (size_t)-1;

    /* Rank among await nodes on the same file+line by col_start (tie-break by node index). */
    int rank = 0;
    for (int j = 0; j < node_count; j++) {
        if (j == idx) continue;
        if (n[j].kind != 6) continue;
        if (n[j].line_start != n[idx].line_start) continue;
        if (n[j].file && n[idx].file && strcmp(n[j].file, n[idx].file) != 0) continue;
        if (n[j].col_start < n[idx].col_start) rank++;
        else if (n[j].col_start == n[idx].col_start && j < idx) rank++;
    }

    size_t line_off = cc__offset_of_line_1based(in_src, in_len, n[idx].line_start);
    size_t toks[32];
    int tn = cc__scan_line_for_await_tokens(in_src, in_len, line_off, toks, (int)(sizeof(toks) / sizeof(toks[0])));
    if (tn <= 0) return (size_t)-1;
    if (rank < 0) rank = 0;
    if (rank >= tn) rank = tn - 1;
    return toks[rank];
}

/* Best-effort: find the end offset of the unary-expression operand of `await`.
   This intentionally does NOT use the stub-AST `col_end` (often inaccurate in expressions). */
static size_t cc__infer_await_expr_end(const char* s, size_t n, size_t await_kw_off) {
    size_t i = await_kw_off;
    if (i + 5 > n || memcmp(s + i, "await", 5) != 0) return await_kw_off;
    i += 5;
    i = cc__skip_ws_comments(s, n, i);
    if (i >= n) return i;

    /* Consume prefix operators/casts/parenthesized expressions in a simple way:
       keep consuming leading unary-ish tokens, then a primary, then postfix chains. */
    int progress_guard = 0;
consume_unary_prefix:
    if (i >= n || progress_guard++ > 2048) return i;
    i = cc__skip_ws_comments(s, n, i);
    if (i >= n) return i;

    /* Prefix ++/-- */
    if (i + 1 < n && ((s[i] == '+' && s[i + 1] == '+') || (s[i] == '-' && s[i + 1] == '-'))) {
        i += 2;
        goto consume_unary_prefix;
    }
    /* Simple one-char prefix ops */
    if (s[i] == '+' || s[i] == '-' || s[i] == '!' || s[i] == '~' || s[i] == '&' || s[i] == '*') {
        i += 1;
        goto consume_unary_prefix;
    }
    /* Parenthesized group / cast. We just consume the whole (...) and allow chaining. */
    if (s[i] == '(') {
        i = cc__scan_matching_delim(s, n, i, '(', ')');
        /* After a cast/group, there may be more unary/prefix (e.g., (T*)p). */
        goto consume_unary_prefix;
    }

    /* Primary: identifier / number / string / char / brace-init (compound literal-ish). */
    if (s[i] == '"' || s[i] == '\'') {
        i = cc__scan_string_lit(s, n, i, s[i]);
    } else if (s[i] == '{') {
        i = cc__scan_matching_delim(s, n, i, '{', '}');
    } else if (cc__is_ident_start(s[i])) {
        i++;
        while (i < n && cc__is_ident_char(s[i])) i++;
    } else if ((s[i] >= '0' && s[i] <= '9') || (s[i] == '.' && i + 1 < n && (s[i + 1] >= '0' && s[i + 1] <= '9'))) {
        i++;
        while (i < n) {
            char c = s[i];
            if ((c >= '0' && c <= '9') || c == '.' || c == '_' || c == 'x' || c == 'X' ||
                (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F') || c == 'p' || c == 'P' || c == 'e' || c == 'E' ||
                c == '+' || c == '-' || c == 'u' || c == 'U' || c == 'l' || c == 'L') {
                i++;
                continue;
            }
            break;
        }
    } else {
        /* Unknown token; give up with a tiny span to avoid corrupting output. */
        return await_kw_off;
    }

    /* Postfix chain: calls, indexing, member access, post ++/-- */
    for (int step = 0; step < 2048; step++) {
        size_t j = cc__skip_ws_comments(s, n, i);
        if (j >= n) { i = j; break; }
        if (s[j] == '(') { i = cc__scan_matching_delim(s, n, j, '(', ')'); continue; }
        if (s[j] == '[') { i = cc__scan_matching_delim(s, n, j, '[', ']'); continue; }
        if (j + 1 < n && s[j] == '-' && s[j + 1] == '>') {
            j += 2;
            j = cc__skip_ws_comments(s, n, j);
            if (j < n && cc__is_ident_start(s[j])) {
                j++;
                while (j < n && cc__is_ident_char(s[j])) j++;
                i = j;
                continue;
            }
            i = j;
            continue;
        }
        if (s[j] == '.') {
            j += 1;
            j = cc__skip_ws_comments(s, n, j);
            if (j < n && cc__is_ident_start(s[j])) {
                j++;
                while (j < n && cc__is_ident_char(s[j])) j++;
                i = j;
                continue;
            }
            i = j;
            continue;
        }
        if (j + 1 < n && ((s[j] == '+' && s[j + 1] == '+') || (s[j] == '-' && s[j + 1] == '-'))) {
            i = j + 2;
            continue;
        }
        break;
    }

    return i;
}

int cc__rewrite_await_exprs_with_nodes(const CCASTRoot* root,
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

    enum { CC_FN_ATTR_ASYNC = 1u << 0 };

    typedef struct {
        size_t start;
        size_t end;
        size_t insert_off;
        size_t trim_start;
        size_t trim_end;
        char tmp[64];
        char* insert_text; /* owned */
    } AwaitRep;

    AwaitRep reps[128];
    int rep_n = 0;

    if (getenv("CC_DEBUG_AWAIT_REWRITE")) {
        int aw = 0;
        for (int i = 0; i < root->node_count; i++) if (n[i].kind == 6) aw++;
        fprintf(stderr, "CC_DEBUG_AWAIT_REWRITE: await nodes in stub AST: %d\n", aw);
        int shown = 0;
        for (int i = 0; i < root->node_count && shown < 5; i++) {
            if (n[i].kind != 6) continue;
            if (n[i].line_start <= 0 || n[i].col_start <= 0) continue;
            size_t os = cc__offset_of_line_col_1based(in_src, in_len, n[i].line_start, n[i].col_start);
            fprintf(stderr, "CC_DEBUG_AWAIT_REWRITE:  node[%d] file=%s line=%d col=%d off=%zu head='%.16s'\n",
                    i, n[i].file ? n[i].file : "<null>", n[i].line_start, n[i].col_start, os,
                    (os < in_len) ? (in_src + os) : "<oob>");
            shown++;
        }
    }

    for (int i = 0; i < root->node_count && rep_n < (int)(sizeof(reps) / sizeof(reps[0])); i++) {
        if (n[i].kind != 6) continue; /* AWAIT */
        if (n[i].line_start <= 0 || n[i].col_start <= 0) continue;
        if (n[i].line_end <= 0 || n[i].col_end <= 0) continue;
        size_t a_s = cc__offset_of_line_col_1based(in_src, in_len, n[i].line_start, n[i].col_start);
        size_t a_e = cc__offset_of_line_col_1based(in_src, in_len, n[i].line_end, n[i].col_end);
        if (a_e <= a_s || a_e > in_len) continue;

        /* Recover a stable `await` keyword offset, even when the stub node points at the operand. */
        {
            size_t kw = cc__await_kw_off_for_node(root->nodes, root->node_count, in_src, in_len, i);
            if (kw != (size_t)-1) a_s = kw;
            if (a_s + 5 > in_len || memcmp(in_src + a_s, "await", 5) != 0) continue;
        }

        /* Do NOT trust stub-AST col_end for await-exprs inside larger expressions.
           Infer end by scanning a single unary-expression operand from the `await` keyword. */
        {
            size_t inferred = cc__infer_await_expr_end(in_src, in_len, a_s);
            if (inferred > a_s + 5 && inferred <= in_len) a_e = inferred;
        }

        if (getenv("CC_DEBUG_AWAIT_REWRITE")) {
            size_t sn = a_e > a_s ? (a_e - a_s) : 0;
            if (sn > 96) sn = 96;
            fprintf(stderr,
                    "CC_DEBUG_AWAIT_REWRITE: pick await rep tmp_idx=%d node=%d start=%zu end=%zu text='%.96s'\n",
                    rep_n, i, a_s, a_e, (a_s < in_len) ? (in_src + a_s) : "<oob>");
        }

        /* Require inside an @async function (otherwise leave it; checker will error). */
        int cur = n[i].parent;
        int is_async = 0;
        int best_line = n[i].line_start;
        while (cur >= 0 && cur < root->node_count) {
            if (n[cur].kind == 12) {
                /* Any enclosing decl-item marked async implies we're inside @async. */
                if (((unsigned int)n[cur].aux2 & (unsigned int)CC_FN_ATTR_ASYNC) != 0) is_async = 1;
            }
            /* Find earliest line start among nearby statement-ish ancestors. */
            if ((n[cur].kind == 15 || n[cur].kind == 14 || n[cur].kind == 5) &&
                n[cur].line_start > 0 && n[cur].line_start < best_line) {
                best_line = n[cur].line_start;
            }
            cur = n[cur].parent;
        }
        if (!is_async) continue;

        /* Skip if await is already statement-root-ish: `await ...;`, `x = await ...;`, `return await ...;` */
        {
            size_t line_off = cc__offset_of_line_1based(in_src, in_len, n[i].line_start);
            size_t p = line_off;
            while (p < in_len && (in_src[p] == ' ' || in_src[p] == '\t')) p++;
            if (p == a_s) continue; /* await at start of statement line */

            /* Check if immediate lhs assignment `= await` by scanning backward for '=' on same line before await. */
            for (size_t k = a_s; k > line_off; k--) {
                char c = in_src[k - 1];
                if (c == '\n') break;
                if (c == '=') { goto skip_this_await; }
            }

            /* Check `return await` by scanning from line start. */
            if (p + 6 <= in_len && memcmp(in_src + p, "return", 6) == 0) {
                size_t q = p + 6;
                while (q < in_len && (in_src[q] == ' ' || in_src[q] == '\t')) q++;
                if (q == a_s) continue;
            }
        }

        /* Compute insertion offset at start of the enclosing statement line. */
        size_t insert_off = cc__offset_of_line_1based(in_src, in_len, best_line);
        if (insert_off > in_len) insert_off = in_len;

        char tmp[64];
        snprintf(tmp, sizeof(tmp), "__cc_aw_l%d_%d", n[i].line_start, rep_n);

        AwaitRep r;
        memset(&r, 0, sizeof(r));
        r.start = a_s;
        r.end = a_e;
        r.insert_off = insert_off;
        /* Trim bounds computed later (after all reps known) */
        strncpy(r.tmp, tmp, sizeof(r.tmp) - 1);
        reps[rep_n++] = r;
        continue;

    skip_this_await:
        (void)0;
    }

    if (rep_n == 0) return 0;

    /* Compute trimmed ranges now. */
    for (int i = 0; i < rep_n; i++) {
        size_t t0 = reps[i].start;
        size_t t1 = reps[i].end;
        while (t0 < t1 && (in_src[t0] == ' ' || in_src[t0] == '\t' || in_src[t0] == '\n' || in_src[t0] == '\r')) t0++;
        while (t1 > t0 && (in_src[t1 - 1] == ' ' || in_src[t1 - 1] == '\t' || in_src[t1 - 1] == '\n' || in_src[t1 - 1] == '\r')) t1--;
        reps[i].trim_start = t0;
        reps[i].trim_end = t1;
    }

    /* Build insertion texts. Ensure nested awaits inside an await-expression are replaced
+       by the corresponding temp names (so outer hoists don't contain raw inner `await`). */
    for (int i = 0; i < rep_n; i++) {
        /* Indent prefix for this insertion */
        size_t insert_off = reps[i].insert_off;
        size_t ind_end = insert_off;
        while (ind_end < in_len && (in_src[ind_end] == ' ' || in_src[ind_end] == '\t')) ind_end++;
        size_t ind_len = ind_end - insert_off;

        /* Build await text with nested replacements. */
        char* await_txt = NULL;
        size_t await_len = 0, await_cap = 0;
        size_t cur = reps[i].trim_start;
        size_t end = reps[i].trim_end;
        while (cur < end) {
            int did = 0;
            for (int j = 0; j < rep_n; j++) {
                if (j == i) continue;
                if (reps[j].trim_start >= reps[i].trim_start &&
                    reps[j].trim_end <= reps[i].trim_end &&
                    reps[j].trim_start == cur) {
                    cc__append_str(&await_txt, &await_len, &await_cap, reps[j].tmp);
                    cur = reps[j].trim_end;
                    did = 1;
                    break;
                }
            }
            if (did) continue;
            cc__append_n(&await_txt, &await_len, &await_cap, in_src + cur, 1);
            cur++;
        }
        if (!await_txt || await_len == 0) { free(await_txt); continue; }

        /* Insert two statements: decl + assignment */
        size_t ins_cap = ind_len * 2 + await_len + 256;
        char* ins = (char*)malloc(ins_cap);
        if (!ins) { free(await_txt); continue; }
        int wn = 0;
        wn += snprintf(ins + (size_t)wn, ins_cap - (size_t)wn, "%.*sintptr_t %s = 0;\n",
                       (int)ind_len, in_src + insert_off, reps[i].tmp);
        wn += snprintf(ins + (size_t)wn, ins_cap - (size_t)wn, "%.*s%s = %.*s;\n",
                       (int)ind_len, in_src + insert_off, reps[i].tmp, (int)await_len, await_txt);
        free(await_txt);
        if (wn <= 0) { free(ins); continue; }
        reps[i].insert_text = ins;
    }

    /* Sort by start asc for replacements; insertions will be handled by bucketed offsets. */
    for (int i = 0; i < rep_n; i++) {
        for (int j = i + 1; j < rep_n; j++) {
            if (reps[j].start < reps[i].start) {
                AwaitRep t = reps[i];
                reps[i] = reps[j];
                reps[j] = t;
            }
        }
    }

    /* Build output streaming: emit insertions when reaching an insertion offset. */
    char* out = NULL;
    size_t outl = 0, outc = 0;

    int ins_idx[128];
    for (int i = 0; i < rep_n; i++) ins_idx[i] = i;
    /* sort indices by insert_off asc */
    for (int i = 0; i < rep_n; i++) {
        for (int j = i + 1; j < rep_n; j++) {
            if (reps[ins_idx[j]].insert_off < reps[ins_idx[i]].insert_off) {
                int t = ins_idx[i]; ins_idx[i] = ins_idx[j]; ins_idx[j] = t;
            }
        }
    }
    int ins_p = 0;

    size_t cur_off = 0;
    int rep_i = 0;
    while (cur_off < in_len) {
        /* Emit any insertions at this offset (may be multiple). */
        if (ins_p < rep_n && reps[ins_idx[ins_p]].insert_off == cur_off) {
            /* Collect all with this insert_off, then emit in descending start order (inner first). */
            int tmp_idx[128];
            int tmp_n = 0;
            size_t off = reps[ins_idx[ins_p]].insert_off;
            while (ins_p < rep_n && reps[ins_idx[ins_p]].insert_off == off) {
                tmp_idx[tmp_n++] = ins_idx[ins_p++];
            }
            for (int a = 0; a < tmp_n; a++) {
                for (int b = a + 1; b < tmp_n; b++) {
                    if (reps[tmp_idx[b]].start > reps[tmp_idx[a]].start) {
                        int t = tmp_idx[a]; tmp_idx[a] = tmp_idx[b]; tmp_idx[b] = t;
                    }
                }
            }
            for (int k = 0; k < tmp_n; k++) {
                const char* it = reps[tmp_idx[k]].insert_text;
                if (it) cc__append_str(&out, &outl, &outc, it);
            }
        }
        /* Apply next replacement if it starts here. */
        if (rep_i < rep_n && reps[rep_i].start == cur_off) {
            cc__append_str(&out, &outl, &outc, reps[rep_i].tmp);
            cur_off = reps[rep_i].end;
            rep_i++;
            continue;
        }
        /* Otherwise copy one byte */
        cc__append_n(&out, &outl, &outc, in_src + cur_off, 1);
        cur_off++;
    }
    /* Insertions at EOF */
    while (ins_p < rep_n && reps[ins_idx[ins_p]].insert_off == cur_off) {
        const char* it = reps[ins_idx[ins_p]].insert_text;
        if (it) cc__append_str(&out, &outl, &outc, it);
        ins_p++;
    }

    for (int i = 0; i < rep_n; i++) free(reps[i].insert_text);
    if (!out) return 0;
    *out_src = out;
    *out_len = outl;
    return 1;
}

/* NEW: Collect await normalization edits into EditBuffer.
   NOTE: This pass has complex insertion and replacement logic.
   For now, this function runs the rewrite and uses a coarse-grained edit.
   Future: refactor to collect edits directly. */
int cc__collect_await_normalize_edits(const CCASTRoot* root,
                                      const CCVisitorCtx* ctx,
                                      CCEditBuffer* eb) {
    if (!root || !ctx || !eb || !eb->src) return 0;

    char* rewritten = NULL;
    size_t rewritten_len = 0;
    int r = cc__rewrite_await_exprs_with_nodes(root, ctx, eb->src, eb->src_len, &rewritten, &rewritten_len);
    if (r <= 0 || !rewritten) return 0;

    if (rewritten_len != eb->src_len || memcmp(rewritten, eb->src, eb->src_len) != 0) {
        if (cc_edit_buffer_add(eb, 0, eb->src_len, rewritten, 70, "await_normalize") == 0) {
            free(rewritten);
            return 1;
        }
    }
    free(rewritten);
    return 0;
}
