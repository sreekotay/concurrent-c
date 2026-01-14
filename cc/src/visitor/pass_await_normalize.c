/* Extracted from the working implementation in `cc/src/visitor/visitor.c`.
   Goal: keep semantics identical while shrinking visitor.c over time. */

#include "pass_await_normalize.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "visitor/text_span.h"

static void cc__append_n(char** out, size_t* out_len, size_t* out_cap, const char* src, size_t n);
static void cc__append_str(char** out, size_t* out_len, size_t* out_cap, const char* src);

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

    typedef struct NodeView {
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

        /* Best-effort: many nodes record `col_start` at the operand; recover the `await` keyword
           by scanning backward on the same line for the nearest `await` token. */
        {
            size_t line_off = cc__offset_of_line_1based(in_src, in_len, n[i].line_start);
            size_t k = a_s;
            size_t found = (size_t)-1;
            while (k > line_off + 4) {
                size_t s0 = k - 5;
                if (memcmp(in_src + s0, "await", 5) == 0) {
                    char before = (s0 > line_off) ? in_src[s0 - 1] : ' ';
                    char after = (s0 + 5 < in_len) ? in_src[s0 + 5] : ' ';
                    int before_ok = !(before == '_' || isalnum((unsigned char)before));
                    int after_ok = !(after == '_' || isalnum((unsigned char)after));
                    if (before_ok && after_ok) { found = s0; break; }
                }
                k--;
            }
            if (found != (size_t)-1) a_s = found;
            if (a_s + 5 > in_len || memcmp(in_src + a_s, "await", 5) != 0) continue;
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


static void cc__append_n(char** out, size_t* out_len, size_t* out_cap, const char* src, size_t n) {
    if (!out || !out_len || !out_cap || !src) return;
    if (*out_len + n + 1 >= *out_cap) {
        *out_cap = *out_cap ? (*out_cap * 2) : 256;
        if (*out_cap < *out_len + n + 1) *out_cap = *out_len + n + 256;
        *out = (char*)realloc(*out, *out_cap);
        if (!*out) return;
    }
    memcpy(*out + *out_len, src, n);
    *out_len += n;
    (*out)[*out_len] = '\0';
}

static void cc__append_str(char** out, size_t* out_len, size_t* out_cap, const char* src) {
    if (!src) return;
    cc__append_n(out, out_len, out_cap, src, strlen(src));
}
