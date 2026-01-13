#include "pass_autoblock.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "comptime/symbols.h"
#include "parser/parse.h"
#include "visitor/visitor.h"

enum {
    CC_FN_ATTR_ASYNC = 1u << 0,
    CC_FN_ATTR_NOBLOCK = 1u << 1,
    CC_FN_ATTR_LATENCY_SENSITIVE = 1u << 2,
};

typedef struct {
    size_t start;     /* statement start */
    size_t end;       /* statement end (inclusive of ';') */
    size_t call_start;
    size_t call_end;  /* end of ')' */
    int line_start;
    const char* callee;
    int argc;
    int ret_is_ptr;
    int ret_is_void;
    int ret_is_structy;
    char* param_types[16]; /* owned */
    size_t indent_start;
    size_t indent_len;
} Replace;

static int cc__same_source_file(const char* a, const char* b);
static const char* cc__basename(const char* path);
static const char* cc__path_suffix2(const char* path);
static int cc__node_file_matches_this_tu(const struct CCASTRoot* root,
                                        const CCVisitorCtx* ctx,
                                        const char* file);
static size_t cc__offset_of_line_1based(const char* s, size_t len, int line_no);
static void cc__append_n(char** out, size_t* out_len, size_t* out_cap, const char* src, size_t n);
static int cc__is_word_boundary(char c);
static int cc__is_ident_char2(char c);
static int cc__find_call_span_in_range(const char* s,
                                      size_t range_start,
                                      size_t range_end,
                                      const char* callee,
                                      size_t* out_call_start,
                                      size_t* out_call_end);
static int cc__is_callee_nonblocking(const char* callee, const CCVisitorCtx* ctx);
static int cc__get_callee_signature(const char* callee,
                                   const CCVisitorCtx* ctx,
                                   int* out_argc,
                                   int* out_ret_is_ptr,
                                   int* out_ret_is_void,
                                   int* out_ret_is_structy,
                                   char* param_types[16]);
static void cc__emit_autoblock_replacement(const char* src,
                                          const Replace* rep,
                                          char** out,
                                          size_t* out_len,
                                          size_t* out_cap);

int cc__rewrite_autoblocking_calls_with_nodes(const struct CCASTRoot* root,
                                             const CCVisitorCtx* ctx,
                                             const char* in_src,
                                             size_t in_len,
                                             char** out_src,
                                             size_t* out_len) {
    if (!root || !ctx || !ctx->symbols || !in_src || !out_src || !out_len) return 0;
    *out_src = NULL;
    *out_len = 0;
    if (!root->nodes || root->node_count <= 0) return 0;

    const struct NodeView {
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
    }* n = (const struct NodeView*)root->nodes;

    Replace* reps = NULL;
    int rep_n = 0;
    int rep_cap = 0;

    for (int i = 0; i < root->node_count; i++) {
        if (n[i].kind != 5) continue; /* CALL */
        int is_ufcs = (n[i].aux2 & 2) != 0;
        if (is_ufcs) continue;
        if (!n[i].aux_s1) continue; /* callee name */
        if (!cc__node_file_matches_this_tu(root, ctx, n[i].file)) continue;

        /* Find enclosing function decl-item and check @async attr. */
        int cur = n[i].parent;
        const char* owner = NULL;
        unsigned int owner_attrs = 0;
        while (cur >= 0 && cur < root->node_count) {
            if (n[cur].kind == 12 && n[cur].aux_s1 && n[cur].aux_s2 && strchr(n[cur].aux_s2, '(') &&
                cc__node_file_matches_this_tu(root, ctx, n[cur].file)) {
                owner = n[cur].aux_s1;
                owner_attrs = (unsigned int)n[cur].aux2;
                break;
            }
            cur = n[cur].parent;
        }
        if (!owner || ((owner_attrs & CC_FN_ATTR_ASYNC) == 0)) continue;

        /* Only skip known-nonblocking callees; if we don't know attrs, assume blocking. */
        if (cc__is_callee_nonblocking(n[i].aux_s1, ctx)) continue;

        /* Range based on lines [line_start, line_end]. */
        size_t rs = cc__offset_of_line_1based(in_src, in_len, n[i].line_start);
        size_t re = cc__offset_of_line_1based(in_src, in_len, n[i].line_end + 1);
        if (re > in_len) re = in_len;

        size_t call_start = 0, call_end = 0;
        if (!cc__find_call_span_in_range(in_src, rs, re, n[i].aux_s1, &call_start, &call_end))
            continue;

        /* Get signature for this callee. */
        int argc = 0, ret_is_ptr = 0, ret_is_void = 0, ret_is_structy = 0;
        char* param_types[16] = {0};
        if (!cc__get_callee_signature(n[i].aux_s1, ctx, &argc, &ret_is_ptr, &ret_is_void, &ret_is_structy, param_types))
            continue;

        /* Find statement boundaries. */
        size_t stmt_start = rs;
        size_t stmt_end = re - 1; /* exclude newline */

        /* Find indentation. */
        size_t indent_start = stmt_start;
        while (indent_start > 0 && (in_src[indent_start - 1] == ' ' || in_src[indent_start - 1] == '\t'))
            indent_start--;
        size_t indent_len = stmt_start - indent_start;

        if (rep_n == rep_cap) {
            rep_cap = rep_cap ? rep_cap * 2 : 32;
            reps = (Replace*)realloc(reps, (size_t)rep_cap * sizeof(*reps));
            if (!reps) return 0;
        }

        reps[rep_n++] = (Replace){
            .start = stmt_start,
            .end = stmt_end,
            .call_start = call_start,
            .call_end = call_end,
            .line_start = n[i].line_start,
            .callee = n[i].aux_s1,
            .argc = argc,
            .ret_is_ptr = ret_is_ptr,
            .ret_is_void = ret_is_void,
            .ret_is_structy = ret_is_structy,
            .indent_start = indent_start,
            .indent_len = indent_len,
        };

        /* Copy param types */
        for (int j = 0; j < 16 && j < argc; j++) {
            if (param_types[j]) {
                reps[rep_n - 1].param_types[j] = param_types[j];
            }
        }
    }

    if (rep_n == 0) {
        free(reps);
        return 0;
    }

    /* Emit rewritten source */
    char* out = NULL;
    size_t out_len2 = 0, out_cap2 = 0;
    size_t cur = 0;

    for (int i = 0; i < rep_n; i++) {
        if (reps[i].start > cur) {
            cc__append_n(&out, &out_len2, &out_cap2, in_src + cur, reps[i].start - cur);
        }

        cc__emit_autoblock_replacement(in_src, &reps[i], &out, &out_len2, &out_cap2);
        cur = reps[i].end + 1; /* skip the ';' */
    }

    if (cur < in_len) {
        cc__append_n(&out, &out_len2, &out_cap2, in_src + cur, in_len - cur);
    }

    /* Cleanup */
    for (int i = 0; i < rep_n; i++) {
        for (int j = 0; j < 16; j++) {
            if (reps[i].param_types[j]) free(reps[i].param_types[j]);
        }
    }
    free(reps);

    if (!out) return 0;
    *out_src = out;
    *out_len = out_len2;
    return 1;
}

/* Helper implementations */

static int cc__same_source_file(const char* a, const char* b) {
    if (!a || !b) return 0;
    if (strcmp(a, b) == 0) return 1;

    const char* a_base = cc__basename(a);
    const char* b_base = cc__basename(b);
    if (!a_base || !b_base || strcmp(a_base, b_base) != 0) return 0;

    /* Prefer 2-component suffix match (handles duplicate basenames across dirs). */
    const char* a_suf = cc__path_suffix2(a);
    const char* b_suf = cc__path_suffix2(b);
    if (a_suf && b_suf && strcmp(a_suf, b_suf) == 0) return 1;

    /* Fallback: basename-only match. */
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

static int cc__find_call_span_in_range(const char* s,
                                      size_t range_start,
                                      size_t range_end,
                                      const char* callee,
                                      size_t* out_call_start,
                                      size_t* out_call_end) {
    if (!s || !callee || !out_call_start || !out_call_end) return 0;
    size_t n = strlen(callee);
    if (n == 0) return 0;

    for (size_t i = range_start; i + n < range_end; i++) {
        if (!cc__is_word_boundary(i > range_start ? s[i - 1] : 0)) continue;
        if (memcmp(s + i, callee, n) != 0) continue;
        if (!cc__is_word_boundary(s[i + n])) continue;

        /* Skip if not followed by whitespace or '(', or if preceded by '.' or '->' (UFCS). */
        size_t after = i + n;
        while (after < range_end && (s[after] == ' ' || s[after] == '\t')) after++;
        if (after >= range_end || s[after] != '(') continue;

        /* Check for UFCS prefix. */
        int ufcs = 0;
        if (i >= 2) {
            if (s[i - 2] == '-' && s[i - 1] == '>') ufcs = 1;
            else if (s[i - 1] == '.') ufcs = 1;
        } else if (i >= 1) {
            if (s[i - 1] == '.') ufcs = 1;
        }
        if (ufcs) continue;

        *out_call_start = i;

        /* Find matching ')'. */
        size_t p = after;
        int depth = 0;
        while (p < range_end) {
            char c = s[p++];
            if (c == '(') depth++;
            else if (c == ')') {
                depth--;
                if (depth == 0) {
                    *out_call_end = p;
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

static int cc__is_word_boundary(char c) {
    return !(cc__is_ident_char2(c));
}

static int cc__is_ident_char2(char c) {
    return (c == '_' || isalnum((unsigned char)c));
}

static int cc__is_callee_nonblocking(const char* callee, const CCVisitorCtx* ctx) {
    /* Simplified: no functions are considered nonblocking for now */
    /* TODO: Implement proper attribute lookup */
    return 0;
}

static int cc__get_callee_signature(const char* callee,
                                   const CCVisitorCtx* ctx,
                                   int* out_argc,
                                   int* out_ret_is_ptr,
                                   int* out_ret_is_void,
                                   int* out_ret_is_structy,
                                   char* param_types[16]) {
    /* Simplified: assume all functions return intptr_t and take no args for now */
    /* TODO: Implement proper function signature lookup */
    *out_argc = 0;
    *out_ret_is_ptr = 0;
    *out_ret_is_void = 0;
    *out_ret_is_structy = 0;
    return 1;
}

static void cc__emit_autoblock_replacement(const char* src,
                                          const Replace* rep,
                                          char** out,
                                          size_t* out_len,
                                          size_t* out_cap) {
    if (!src || !rep || !out || !out_len || !out_cap) return;

    /* Emit indentation */
    if (rep->indent_len > 0) {
        cc__append_n(out, out_len, out_cap, src + rep->indent_start, rep->indent_len);
    }

    /* Emit auto-blocking call: await cc_run_blocking_task_intptr(...) */
    char buf[1024];
    int len = snprintf(buf, sizeof(buf), "await cc_run_blocking_task_intptr(");
    if (len < 0 || (size_t)len >= sizeof(buf)) return;
    cc__append_n(out, out_len, out_cap, buf, (size_t)len);

    /* Emit closure: () => { return %s( */
    cc__append_n(out, out_len, out_cap, "() => { return ", 14);
    cc__append_n(out, out_len, out_cap, rep->callee, strlen(rep->callee));
    cc__append_n(out, out_len, out_cap, "(", 1);

    /* Extract and emit args from original call */
    if (rep->call_start < rep->call_end) {
        size_t args_start = rep->call_start + strlen(rep->callee);
        while (args_start < rep->call_end && (src[args_start] == ' ' || src[args_start] == '\t')) args_start++;
        if (args_start < rep->call_end && src[args_start] == '(') args_start++;
        size_t args_end = rep->call_end;
        if (args_end > args_start && src[args_end - 1] == ')') args_end--;

        if (args_start < args_end) {
            cc__append_n(out, out_len, out_cap, src + args_start, args_end - args_start);
        }
    }

    /* Close closure and await call */
    cc__append_n(out, out_len, out_cap, "); })", 5);
}