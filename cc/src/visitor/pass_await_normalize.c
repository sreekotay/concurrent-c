#include "pass_await_normalize.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "parser/parse.h"
#include "visitor/visitor.h"

typedef struct {
    size_t start;
    size_t end;
    size_t insert_off;
    size_t trim_start;
    size_t trim_end;
    char tmp[64];
    char* insert_text; /* owned */
} AwaitRep;

static int cc__same_source_file(const char* a, const char* b);
static const char* cc__basename(const char* path);
static const char* cc__path_suffix2(const char* path);
static size_t cc__offset_of_line_1based(const char* s, size_t len, int line_no);
static size_t cc__offset_of_line_col_1based(const char* s, size_t len, int line_no, int col_no);
static int cc__node_file_matches_this_tu(const struct CCASTRoot* root,
                                        const CCVisitorCtx* ctx,
                                        const char* file);
static void cc__append_n(char** out, size_t* out_len, size_t* out_cap, const char* src, size_t n);
static void cc__append_str(char** out, size_t* out_len, size_t* out_cap, const char* src);

int cc__rewrite_await_exprs_with_nodes(const struct CCASTRoot* root,
                                      const CCVisitorCtx* ctx,
                                      const char* in_src,
                                      size_t in_len,
                                      char** out_src,
                                      size_t* out_len) {
    fprintf(stderr, "CC: await_normalize: starting, root->node_count=%d\n", root ? root->node_count : 0);
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

    AwaitRep reps[128];
    int rep_n = 0;

    for (int i = 0; i < root->node_count && rep_n < (int)(sizeof(reps) / sizeof(reps[0])); i++) {
        if (n[i].kind != 6) continue; /* AWAIT */
        if (n[i].line_start <= 0 || n[i].col_start <= 0) continue;
        if (n[i].line_end <= 0 || n[i].col_end <= 0) continue;
        if (!cc__node_file_matches_this_tu(root, ctx, n[i].file)) continue;

        size_t a_s = cc__offset_of_line_col_1based(in_src, in_len, n[i].line_start, n[i].col_start);
        size_t a_e = cc__offset_of_line_col_1based(in_src, in_len, n[i].line_end, n[i].col_end);
        if (a_e <= a_s || a_e > in_len) continue;

        /* Find the actual 'await' keyword */
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

        /* Require inside an @async function */
        int cur = n[i].parent;
        int is_async = 0;
        int best_line = n[i].line_start;
        while (cur >= 0 && cur < root->node_count) {
            if (n[cur].kind == 12) {
                if (((unsigned int)n[cur].aux2 & (unsigned int)CC_FN_ATTR_ASYNC) != 0) is_async = 1;
            }
            if ((n[cur].kind == 15 || n[cur].kind == 14 || n[cur].kind == 5) &&
                n[cur].line_start > 0 && n[cur].line_start < best_line) {
                best_line = n[cur].line_start;
            }
            cur = n[cur].parent;
        }
        if (!is_async) continue;

        /* Skip if await is already at statement start */
        size_t p = line_off;
        while (p < in_len && (in_src[p] == ' ' || in_src[p] == '\t')) p++;
        if (p == a_s) continue; /* await at start of statement line */

        /* Check for assignment: x = await */
        for (size_t k = a_s; k > line_off; k--) {
            char c = in_src[k - 1];
            if (c == '\n') break;
            if (c == '=') continue; /* skip this await */
        }

        /* Check for return await */
        if (p + 6 <= in_len && memcmp(in_src + p, "return", 6) == 0) {
            size_t q = p + 6;
            while (q < in_len && (in_src[q] == ' ' || in_src[q] == '\t')) q++;
            if (q == a_s) continue;
        }

        /* Compute insertion offset at start of the enclosing statement line */
        size_t insert_off = cc__offset_of_line_1based(in_src, in_len, best_line);
        if (insert_off > in_len) insert_off = in_len;

        char tmp[64];
        snprintf(tmp, sizeof(tmp), "__cc_aw_l%d_%d", n[i].line_start, rep_n);

        AwaitRep r = {0};
        r.start = a_s;
        r.end = a_e;
        r.insert_off = insert_off;
        strncpy(r.tmp, tmp, sizeof(r.tmp) - 1);
        reps[rep_n++] = r;
    }

    if (rep_n == 0) return 0;

    /* Compute trimmed ranges */
    for (int i = 0; i < rep_n; i++) {
        size_t t0 = reps[i].start;
        size_t t1 = reps[i].end;
        while (t0 < t1 && (in_src[t0] == ' ' || in_src[t0] == '\t' || in_src[t0] == '\n' || in_src[t0] == '\r')) t0++;
        while (t1 > t0 && (in_src[t1 - 1] == ' ' || in_src[t1 - 1] == '\t' || in_src[t1 - 1] == '\n' || in_src[t1 - 1] == '\r')) t1--;
        reps[i].trim_start = t0;
        reps[i].trim_end = t1;
    }

    /* Build insertion texts */
    for (int i = 0; i < rep_n; i++) {
        size_t insert_off = reps[i].insert_off;
        size_t ind_end = insert_off;
        while (ind_end < in_len && (in_src[ind_end] == ' ' || in_src[ind_end] == '\t')) ind_end++;
        size_t ind_len = ind_end - insert_off;

        /* Build await text with nested replacements */
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
            if (!did) {
                cc__append_n(&await_txt, &await_len, &await_cap, in_src + cur, 1);
                cur++;
            }
        }

        if (!await_txt || await_len == 0) {
            free(await_txt);
            continue;
        }

        /* Insert two statements: decl + assignment */
        size_t ins_cap = ind_len * 2 + await_len + 256;
        char* ins = (char*)malloc(ins_cap);
        if (!ins) {
            free(await_txt);
            continue;
        }

        int wn = 0;
        wn += snprintf(ins + (size_t)wn, ins_cap - (size_t)wn, "%.*sintptr_t %s = 0;\n",
                       (int)ind_len, in_src + insert_off, reps[i].tmp);
        wn += snprintf(ins + (size_t)wn, ins_cap - (size_t)wn, "%.*s%s = %.*s;\n",
                       (int)ind_len, in_src + insert_off, reps[i].tmp, (int)await_len, await_txt);
        free(await_txt);

        if (wn <= 0) {
            free(ins);
            continue;
        }
        reps[i].insert_text = ins;
    }

    /* Sort by start asc */
    for (int i = 0; i < rep_n; i++) {
        for (int j = i + 1; j < rep_n; j++) {
            if (reps[j].start < reps[i].start) {
                AwaitRep t = reps[i];
                reps[i] = reps[j];
                reps[j] = t;
            }
        }
    }

    /* Build output */
    char* out = NULL;
    size_t outl = 0, outc = 0;

    int ins_idx[128];
    for (int i = 0; i < rep_n; i++) ins_idx[i] = i;
    /* sort by insert_off asc */
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
        /* Emit insertions at this offset */
        if (ins_p < rep_n && reps[ins_idx[ins_p]].insert_off == cur_off) {
            int tmp_idx[128];
            int tmp_n = 0;
            size_t off = reps[ins_idx[ins_p]].insert_off;
            while (ins_p < rep_n && reps[ins_idx[ins_p]].insert_off == off) {
                tmp_idx[tmp_n++] = ins_idx[ins_p++];
            }
            /* Sort by descending start (inner first) */
            for (int a = 0; a < tmp_n; a++) {
                for (int b = a + 1; b < tmp_n; b++) {
                    if (reps[tmp_idx[b]].start > reps[tmp_idx[a]].start) {
                        int t = tmp_idx[a]; tmp_idx[a] = tmp_idx[b]; tmp_idx[b] = t;
                    }
                }
            }
            for (int k = 0; k < tmp_n; k++) {
                cc__append_str(&out, &outl, &outc, reps[tmp_idx[k]].insert_text);
            }
        }

        /* Emit replacements */
        if (rep_i < rep_n && reps[rep_i].start == cur_off) {
            cc__append_str(&out, &outl, &outc, reps[rep_i].tmp);
            cur_off = reps[rep_i].end;
            rep_i++;
            continue;
        }

        /* Emit original char */
        cc__append_n(&out, &outl, &outc, in_src + cur_off, 1);
        cur_off++;
    }

    /* Cleanup */
    for (int i = 0; i < rep_n; i++) {
        free(reps[i].insert_text);
    }

    if (!out) return 0;
    *out_src = out;
    *out_len = outl;
    return 1;
}

/* Helper implementations */

static size_t cc__offset_of_line_1based(const char* s, size_t len, int line_no) {
    if (!s || line_no <= 1) return 0;
    int cur = 1;
    for (size_t i = 0; i < len; i++) {
        if (s[i] == '\n') {
            cur++;
            if (cur == line_no) return i + 1;
        }
    }
    return len;
}

static size_t cc__offset_of_line_col_1based(const char* s, size_t len, int line_no, int col_no) {
    if (!s) return 0;
    if (line_no <= 1 && col_no <= 1) return 0;
    if (col_no <= 1) return cc__offset_of_line_1based(s, len, line_no);
    size_t loff = cc__offset_of_line_1based(s, len, line_no);
    size_t off = loff + (size_t)(col_no - 1);
    if (off > len) off = len;
    return off;
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

static void cc__append_n(char** out, size_t* out_len, size_t* out_cap, const char* src, size_t n) {
    if (!out || !out_len || !out_cap || !src) return;
    if (*out_len + n >= *out_cap) {
        *out_cap = *out_cap ? *out_cap * 2 : 256;
        if (*out_cap < *out_len + n) *out_cap = *out_len + n + 256;
        *out = (char*)realloc(*out, *out_cap);
        if (!*out) return;
    }
    memcpy(*out + *out_len, src, n);
    *out_len += n;
}

static int cc__node_file_matches_this_tu(const struct CCASTRoot* root,
                                        const CCVisitorCtx* ctx,
                                        const char* file) {
    if (!ctx || !ctx->input_path || !file) return 0;
    if (cc__same_source_file(ctx->input_path, file)) return 1;
    if (root && root->lowered_path && cc__same_source_file(root->lowered_path, file)) return 1;
    return 0;
}

static void cc__append_str(char** out, size_t* out_len, size_t* out_cap, const char* src) {
    if (!src) return;
    cc__append_n(out, out_len, out_cap, src, strlen(src));
}