/* Extracted from the working implementation in `cc/src/visitor/visitor.c`.
   Goal: keep semantics identical while shrinking visitor.c over time.
*/

#include "pass_closure_calls.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util/text.h"
#include "visitor/pass_common.h"

/* Local aliases for the shared helpers */
#define cc__is_ident_start_char cc_is_ident_start
#define cc__is_ident_char2 cc_is_ident_char

static int cc__is_keyword_tok(const char* s, size_t n) {
    static const char* kw[] = {
        "if","else","for","while","do","switch","case","default","break","continue","return",
        "sizeof","struct","union","enum","typedef","static","extern","const","volatile","restrict",
        "void","char","short","int","long","float","double","_Bool","signed","unsigned",
        "goto","auto","register","_Atomic","_Alignas","_Alignof","_Thread_local",
        "true","false","NULL"
    };
    for (size_t i = 0; i < sizeof(kw) / sizeof(kw[0]); i++) {
        if (strlen(kw[i]) == n && strncmp(kw[i], s, n) == 0) return 1;
    }
    return 0;
}

static int cc__name_in_list(char** xs, int n, const char* s, size_t slen) {
    for (int i = 0; i < n; i++) {
        if (!xs[i]) continue;
        if (strlen(xs[i]) == slen && strncmp(xs[i], s, slen) == 0) return 1;
    }
    return 0;
}

static void cc__maybe_record_decl(char*** scope_names,
                                 char*** scope_types,
                                 unsigned char** scope_flags,
                                 int* scope_counts,
                                 int depth,
                                 const char* line) {
    if (!scope_names || !scope_types || !scope_flags || !scope_counts || depth < 0 || depth >= 256 || !line) return;
    const char* p = line;
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '#' || *p == '\0') return;
    const char* semi = strchr(p, ';');
    if (!semi) return;
    /* Ignore function prototypes (best-effort):
       if we see '(' before ';' and there is no '=' before that '(', it's likely a prototype/declarator. */
    const char* lp = strchr(p, '(');
    if (lp && lp < semi) {
        const char* eq = strchr(p, '=');
        if (!eq || eq > lp) return;
    }

    /* Find the declared variable name as the last identifier before '=', ',', or ';'. */
    const char* name_s = NULL;
    size_t name_n = 0;
    const char* cur = p;
    while (cur < semi) {
        if (*cur == '"' || *cur == '\'') {
            char q = *cur++;
            while (cur < semi) {
                if (*cur == '\\' && (cur + 1) < semi) { cur += 2; continue; }
                if (*cur == q) { cur++; break; }
                cur++;
            }
            continue;
        }
        if (*cur == '=' || *cur == ',' || *cur == ';') break;
        if (!cc__is_ident_start_char(*cur)) { cur++; continue; }
        const char* s = cur++;
        while (cur < semi && cc__is_ident_char2(*cur)) cur++;
        size_t n = (size_t)(cur - s);
        if (n == 0 || cc__is_keyword_tok(s, n)) continue;
        name_s = s;
        name_n = n;
    }
    if (!name_s || name_n == 0) return;
    /* Type is everything from p to name_s (trimmed). */
    const char* ty_s = p;
    const char* ty_e = name_s;
    while (ty_s < ty_e && (*ty_s == ' ' || *ty_s == '\t')) ty_s++;
    while (ty_e > ty_s && (ty_e[-1] == ' ' || ty_e[-1] == '\t')) ty_e--;
    if (ty_e <= ty_s) return;

    int cur_n = scope_counts[depth];
    if (cc__name_in_list(scope_names[depth], cur_n, name_s, name_n)) return;

    /* Build a file-scope-safe type string.
       If the type uses CC slice syntax (`T[:]`/`T[:!]`), map it to CCSlice (plus pointer stars if present). */
    int is_slice = 0;
    int slice_has_bang = 0;
    int ptr_n = 0;
    for (const char* s = ty_s; s < ty_e; s++) {
        if (*s == '*') ptr_n++;
        if (*s == '[') {
            const char* t = s;
            while (t < ty_e && *t != ']') t++;
            if (t < ty_e) {
                for (const char* u = s; u < t; u++) {
                    if (*u == ':') is_slice = 1;
                    if (*u == '!') slice_has_bang = 1;
                }
            }
        }
    }

    char* ty = NULL;
    if (is_slice) {
        const char* base = "CCSlice";
        size_t bt = strlen(base);
        ty = (char*)malloc(bt + (size_t)ptr_n + 1);
        if (!ty) return;
        memcpy(ty, base, bt);
        for (int i = 0; i < ptr_n; i++) ty[bt + (size_t)i] = '*';
        ty[bt + (size_t)ptr_n] = '\0';
    } else {
        size_t tn = (size_t)(ty_e - ty_s);
        ty = (char*)malloc(tn + 1);
        if (!ty) return;
        memcpy(ty, ty_s, tn);
        ty[tn] = '\0';
    }

    char* name = (char*)malloc(name_n + 1);
    if (!name) { free(ty); return; }
    memcpy(name, name_s, name_n);
    name[name_n] = '\0';
    char** next = (char**)realloc(scope_names[depth], (size_t)(cur_n + 1) * sizeof(char*));
    if (!next) { free(name); free(ty); return; }
    scope_names[depth] = next;
    char** tnext = (char**)realloc(scope_types[depth], (size_t)(cur_n + 1) * sizeof(char*));
    if (!tnext) { free(name); free(ty); return; }
    scope_types[depth] = tnext;
    unsigned char* fnext = (unsigned char*)realloc(scope_flags[depth], (size_t)(cur_n + 1) * sizeof(unsigned char));
    if (!fnext) { free(name); free(ty); return; }
    scope_flags[depth] = fnext;

    /* Flags: bit0 = is_slice(CCSlice), bit1 = move-only slice hint. */
    unsigned char flags = 0;
    if (strcmp(ty, "CCSlice") == 0) flags |= 1;
    if (is_slice && slice_has_bang) flags |= 2;
    /* Provenance hint (more "real"): detect unique-id construction in initializer.
       - cc_slice_make_id(..., true/1, ...)
       - CC_SLICE_ID_UNIQUE bit present in an id expression
       This is still best-effort text parsing until we have a typed AST. */
    if ((flags & 1) != 0) {
        const char* eq = strchr(name_s, '=');
        if (eq && eq < semi) {
            if (strstr(eq, "CC_SLICE_ID_UNIQUE")) flags |= 2;
            const char* mk = strstr(eq, "cc_slice_make_id");
            if (mk) {
                const char* lp2 = strchr(mk, '(');
                if (lp2) {
                    const char* t = lp2 + 1;
                    int comma = 0;
                    while (*t && t < semi) {
                        if (*t == '"' || *t == '\'') {
                            char qq = *t++;
                            while (*t && t < semi) {
                                if (*t == '\\' && t[1]) { t += 2; continue; }
                                if (*t == qq) { t++; break; }
                                t++;
                            }
                            continue;
                        }
                        if (*t == ',') {
                            comma++;
                            if (comma == 1) {
                                t++;
                                while (*t == ' ' || *t == '\t') t++;
                                if (strncmp(t, "true", 4) == 0) { flags |= 2; break; }
                                if (*t == '1') { flags |= 2; break; }
                            }
                        }
                        t++;
                    }
                }
            }
        }
    }

    scope_names[depth][cur_n] = name;
    scope_types[depth][cur_n] = ty;
    scope_flags[depth][cur_n] = flags;
    scope_counts[depth] = cur_n + 1;
}

static const char* cc__lookup_decl_type(char** scope_names,
                                       char** scope_types,
                                       int n,
                                       const char* name) {
    if (!scope_names || !scope_types || !name) return NULL;
    for (int i = 0; i < n; i++) {
        if (!scope_names[i] || !scope_types[i]) continue;
        if (strcmp(scope_names[i], name) == 0) return scope_types[i];
    }
    return NULL;
}

static int cc__append_str(char** buf, size_t* len, size_t* cap, const char* s) {
    if (!buf || !len || !cap || !s) return 0;
    size_t n = strlen(s);
    size_t need = *len + n + 1;
    if (need > *cap) {
        size_t nc = *cap ? *cap * 2 : 1024;
        while (nc < need) nc *= 2;
        char* nb = (char*)realloc(*buf, nc);
        if (!nb) return 0;
        *buf = nb;
        *cap = nc;
    }
    memcpy(*buf + *len, s, n);
    *len += n;
    (*buf)[*len] = '\0';
    return 1;
}

static int cc__append_n(char** buf, size_t* len, size_t* cap, const char* s, size_t n) {
    if (!buf || !len || !cap || !s) return 0;
    if (n == 0) return 1;
    size_t need = *len + n + 1;
    if (need > *cap) {
        size_t nc = *cap ? *cap * 2 : 1024;
        while (nc < need) nc *= 2;
        char* nb = (char*)realloc(*buf, nc);
        if (!nb) return 0;
        *buf = nb;
        *cap = nc;
    }
    memcpy(*buf + *len, s, n);
    *len += n;
    (*buf)[*len] = '\0';
    return 1;
}

static int cc__append_fmt(char** buf, size_t* len, size_t* cap, const char* fmt, ...) {
    if (!buf || !len || !cap || !fmt) return 0;
    char tmp[4096];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n <= 0) return 0;
    tmp[sizeof(tmp) - 1] = '\0';
    return cc__append_str(buf, len, cap, tmp);
}

static int cc__is_word_boundary(char c) {
    return !(cc__is_ident_char2(c));
}

static int cc__find_nth_callee_call_span_in_range(const char* s,
                                                  size_t range_start,
                                                  size_t range_end,
                                                  const char* callee,
                                                  int occ_1based,
                                                  size_t* out_name_start,
                                                  size_t* out_lparen,
                                                  size_t* out_rparen_end) {
    if (!s || !callee || !out_name_start || !out_lparen || !out_rparen_end) return 0;
    if (occ_1based <= 0) occ_1based = 1;
    size_t n = strlen(callee);
    if (n == 0) return 0;
    if (range_end <= range_start) return 0;

    int occ = 0;
    for (size_t i = range_start; i + n < range_end; i++) {
        if (memcmp(s + i, callee, n) != 0) continue;
        char before = (i == 0) ? '\0' : s[i - 1];
        char after = (i + n < range_end) ? s[i + n] : '\0';
        if (i > 0 && !cc__is_word_boundary(before)) continue;
        if (i + n < range_end && !cc__is_word_boundary(after) && after != ' ' && after != '\t' && after != '\n' && after != '\r') continue;

        size_t j = i + n;
        while (j < range_end && (s[j] == ' ' || s[j] == '\t' || s[j] == '\n' || s[j] == '\r')) j++;
        if (j >= range_end || s[j] != '(') continue;
        occ++;
        if (occ != occ_1based) continue;

        size_t lparen = j;
        /* Find matching ')' */
        int par = 0, brk = 0, br = 0;
        int ins = 0; char q = 0;
        for (size_t k = lparen + 1; k < range_end; k++) {
            char ch = s[k];
            if (ins) {
                if (ch == '\\' && k + 1 < range_end) { k++; continue; }
                if (ch == q) ins = 0;
                continue;
            }
            if (ch == '"' || ch == '\'') { ins = 1; q = ch; continue; }
            if (ch == '(') par++;
            else if (ch == ')') {
                if (par == 0 && brk == 0 && br == 0) {
                    *out_name_start = i;
                    *out_lparen = lparen;
                    *out_rparen_end = k + 1;
                    return 1;
                }
                if (par) par--;
            } else if (ch == '[') brk++;
            else if (ch == ']') { if (brk) brk--; }
            else if (ch == '{') br++;
            else if (ch == '}') { if (br) br--; }
        }
        return 0;
    }
    return 0;
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
                                        const char* file) {
    if (!ctx || !ctx->input_path || !file) return 0;
    if (cc__same_source_file(ctx->input_path, file)) return 1;
    if (root && root->lowered_path && cc__same_source_file(root->lowered_path, file)) return 1;
    return 0;
}

/* A local view of the stub AST node layout. */
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

/* NOTE: do not use FUNC/PARAM arity to decide closure calls.
   A regular function call also has an arity; we only rewrite calls whose callee is typed as CCClosure1/2. */

typedef struct {
    int line_start;
    int col_start;
    int line_end;
    int col_end;
    const char* callee; /* identifier */
    int occ_1based;
    int arity; /* 1 or 2 */
} CCClosureCallNode;

typedef struct {
    size_t name_start;
    size_t lparen;
    size_t rparen_end;
    int arity;
    int parent;          /* index in spans array, -1 if none */
    int* children;
    int child_n;
    int child_cap;
} CCClosureCallSpan;

static void cc__span_add_child(CCClosureCallSpan* spans, int parent, int child) {
    if (!spans || parent < 0 || child < 0) return;
    CCClosureCallSpan* p = &spans[parent];
    if (p->child_n == p->child_cap) {
        p->child_cap = p->child_cap ? p->child_cap * 2 : 4;
        p->children = (int*)realloc(p->children, (size_t)p->child_cap * sizeof(int));
        if (!p->children) { p->child_cap = 0; p->child_n = 0; return; }
    }
    p->children[p->child_n++] = child;
}

static void cc__emit_range_with_call_spans(const char* src,
                                          size_t start,
                                          size_t end,
                                          const CCClosureCallSpan* spans,
                                          int span_idx,
                                          char** io_out,
                                          size_t* io_len,
                                          size_t* io_cap);

static void cc__emit_arg_range(const char* src,
                              size_t a0,
                              size_t a1,
                              const CCClosureCallSpan* spans,
                              int span_idx,
                              char** io_out,
                              size_t* io_len,
                              size_t* io_cap) {
    cc__emit_range_with_call_spans(src, a0, a1, spans, span_idx, io_out, io_len, io_cap);
}

static void cc__emit_call_replacement(const char* src,
                                      const char* callee,
                                      const CCClosureCallSpan* spans,
                                      int span_idx,
                                      char** io_out,
                                      size_t* io_len,
                                      size_t* io_cap) {
    const CCClosureCallSpan* sp = &spans[span_idx];
    size_t args_s = sp->lparen + 1;
    size_t args_e = sp->rparen_end - 1;
    /* Find comma for arity=2 in original args text (top-level only). */
    size_t comma = 0;
    if (sp->arity == 2) {
        int par = 0, brk = 0, br = 0;
        int ins = 0; char q = 0;
        for (size_t i = args_s; i < args_e; i++) {
            char ch = src[i];
            if (ins) {
                if (ch == '\\' && i + 1 < args_e) { i++; continue; }
                if (ch == q) ins = 0;
                continue;
            }
            if (ch == '"' || ch == '\'') { ins = 1; q = ch; continue; }
            if (ch == '(') par++;
            else if (ch == ')') { if (par) par--; }
            else if (ch == '[') brk++;
            else if (ch == ']') { if (brk) brk--; }
            else if (ch == '{') br++;
            else if (ch == '}') { if (br) br--; }
            else if (ch == ',' && par == 0 && brk == 0 && br == 0) { comma = i; break; }
        }
        if (!comma) return; /* malformed; emit nothing */
    }

    const char* fn = (sp->arity == 1) ? "cc_closure1_call" : "cc_closure2_call";
    cc__append_fmt(io_out, io_len, io_cap, "%s(%s, (intptr_t)(", fn, callee);
    if (sp->arity == 1) {
        cc__emit_arg_range(src, args_s, args_e, spans, span_idx, io_out, io_len, io_cap);
        cc__append_str(io_out, io_len, io_cap, "))");
    } else {
        cc__emit_arg_range(src, args_s, comma, spans, span_idx, io_out, io_len, io_cap);
        cc__append_str(io_out, io_len, io_cap, "), (intptr_t)(");
        cc__emit_arg_range(src, comma + 1, args_e, spans, span_idx, io_out, io_len, io_cap);
        cc__append_str(io_out, io_len, io_cap, "))");
    }
}

static void cc__emit_range_with_call_spans(const char* src,
                                          size_t start,
                                          size_t end,
                                          const CCClosureCallSpan* spans,
                                          int span_idx,
                                          char** io_out,
                                          size_t* io_len,
                                          size_t* io_cap) {
    if (!src || !spans || !io_out || !io_len || !io_cap) return;
    const CCClosureCallSpan* sp = &spans[span_idx];
    /* Walk direct children and emit text around them. */
    size_t cur = start;
    for (int ci = 0; ci < sp->child_n; ci++) {
        int child = sp->children[ci];
        const CCClosureCallSpan* csp = &spans[child];
        if (csp->name_start < start || csp->rparen_end > end) continue;
        if (csp->name_start > cur) {
            cc__append_n(io_out, io_len, io_cap, src + cur, csp->name_start - cur);
        }
        /* Emit rewritten child call */
        size_t nm_s = csp->name_start;
        size_t nm_e = csp->lparen;
        while (nm_e > nm_s && (src[nm_e - 1] == ' ' || src[nm_e - 1] == '\t' || src[nm_e - 1] == '\n' || src[nm_e - 1] == '\r')) nm_e--;
        char nm[128];
        size_t nn = nm_e > nm_s ? (nm_e - nm_s) : 0;
        if (nn > 0 && nn < sizeof(nm)) {
            memcpy(nm, src + nm_s, nn);
            nm[nn] = '\0';
            cc__emit_call_replacement(src, nm, spans, child, io_out, io_len, io_cap);
        } else {
            cc__append_n(io_out, io_len, io_cap, src + csp->name_start, csp->rparen_end - csp->name_start);
        }
        cur = csp->rparen_end;
    }
    if (cur < end) cc__append_n(io_out, io_len, io_cap, src + cur, end - cur);
}

int cc__rewrite_all_closure_calls_with_nodes(const CCASTRoot* root,
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

    /* Collect non-UFCS CALL nodes with a callee name. */
    CCClosureCallNode* calls = NULL;
    int call_n = 0;
    int call_cap = 0;
    for (int i = 0; i < root->node_count; i++) {
        if (n[i].kind != 5) continue; /* CALL */
        int is_ufcs = (n[i].aux2 & 2) != 0;
        if (is_ufcs) continue;
        if (!n[i].aux_s1) continue;
        if (!cc__node_file_matches_this_tu(root, ctx, n[i].file)) continue;
        if (call_n == call_cap) {
            call_cap = call_cap ? call_cap * 2 : 64;
            calls = (CCClosureCallNode*)realloc(calls, (size_t)call_cap * sizeof(*calls));
            if (!calls) return 0;
        }
        calls[call_n++] = (CCClosureCallNode){
            .line_start = n[i].line_start,
            .col_start = n[i].col_start,
            .line_end = n[i].line_end,
            .col_end = n[i].col_end,
            .callee = n[i].aux_s1,
            .occ_1based = 1,
            .arity = 0,
        };
    }
    if (call_n == 0) { free(calls); return 0; }

    /* Sort by (line_start, col_start). */
    for (int i = 0; i < call_n; i++) {
        for (int j = i + 1; j < call_n; j++) {
            int swap = 0;
            if (calls[j].line_start < calls[i].line_start) swap = 1;
            else if (calls[j].line_start == calls[i].line_start && calls[j].col_start < calls[i].col_start) swap = 1;
            if (swap) { CCClosureCallNode t = calls[i]; calls[i] = calls[j]; calls[j] = t; }
        }
    }

    /* Assign occurrence per (line_start, callee) so we can find spans after prior rewrites. */
    for (int i = 0; i < call_n; i++) {
        int occ = 1;
        for (int j = 0; j < i; j++) {
            if (calls[j].line_start == calls[i].line_start && calls[j].callee && calls[i].callee &&
                strcmp(calls[j].callee, calls[i].callee) == 0) occ++;
        }
        calls[i].occ_1based = occ;
    }

    /* Best-effort: build a global decl table (depth 0) for CCClosure1/2 vars. */
    char** decl_names[1] = {0};
    char** decl_types[1] = {0};
    unsigned char* decl_flags[1] = {0};
    int decl_counts[1] = {0};
    {
        const char* cur = in_src;
        const char* end = in_src + in_len;
        while (cur < end) {
            const char* nl = memchr(cur, '\n', (size_t)(end - cur));
            size_t ll = nl ? (size_t)(nl - cur) : (size_t)(end - cur);
            char tmp[1024];
            size_t cp = ll < sizeof(tmp) - 1 ? ll : sizeof(tmp) - 1;
            memcpy(tmp, cur, cp);
            tmp[cp] = '\0';
            cc__maybe_record_decl(decl_names, decl_types, decl_flags, decl_counts, 0, tmp);
            if (!nl) break;
            cur = nl + 1;
        }
    }

    /* Determine whether each call is a closure call (CCClosure1/2) based on call type string (if present)
       or declared type of the callee identifier. */
    int rewrite_n = 0;
    for (int i = 0; i < call_n; i++) {
        if (n) {
            /* Prefer: recorded callee type string on the CALL node (when available). */
            for (int k = 0; k < root->node_count; k++) {
                if (n[k].kind != 5) continue; /* CALL */
                if (!n[k].aux_s1 || strcmp(n[k].aux_s1, calls[i].callee) != 0) continue;
                if (!cc__node_file_matches_this_tu(root, ctx, n[k].file)) continue;
                if (n[k].aux_s2) {
                    if (strstr(n[k].aux_s2, "CCClosure2")) { calls[i].arity = 2; break; }
                    if (strstr(n[k].aux_s2, "CCClosure1")) { calls[i].arity = 1; break; }
                }
            }
        }
        if (!calls[i].arity) {
            const char* ty = cc__lookup_decl_type(decl_names[0], decl_types[0], decl_counts[0], calls[i].callee);
            if (ty) {
                if (strstr(ty, "CCClosure2")) calls[i].arity = 2;
                else if (strstr(ty, "CCClosure1")) calls[i].arity = 1;
            }
        }
        if (calls[i].arity) rewrite_n++;
    }
    if (rewrite_n == 0) {
        for (int i = 0; i < decl_counts[0]; i++) { free(decl_names[0][i]); free(decl_types[0][i]); }
        free(decl_names[0]); free(decl_types[0]); free(decl_flags[0]);
        free(calls);
        return 0;
    }

    /* Build call spans for closure calls. */
    CCClosureCallSpan* spans = (CCClosureCallSpan*)calloc((size_t)rewrite_n, sizeof(*spans));
    if (!spans) return 0;
    int sn = 0;
    for (int i = 0; i < call_n; i++) {
        if (!calls[i].arity) continue;
        /* Range based on lines [line_start, line_end]. */
        size_t rs = cc__offset_of_line_1based(in_src, in_len, calls[i].line_start);
        size_t re = cc__offset_of_line_1based(in_src, in_len, calls[i].line_end + 1);
        if (re > in_len) re = in_len;
        size_t nm_s = 0, lp = 0, rp_end = 0;
        if (!cc__find_nth_callee_call_span_in_range(in_src, rs, re, calls[i].callee, calls[i].occ_1based, &nm_s, &lp, &rp_end))
            continue;
        spans[sn++] = (CCClosureCallSpan){
            .name_start = nm_s,
            .lparen = lp,
            .rparen_end = rp_end,
            .arity = calls[i].arity,
            .parent = -1,
            .children = NULL,
            .child_n = 0,
            .child_cap = 0,
        };
    }
    if (sn == 0) { free(spans); spans = NULL; }

    /* Clean decl table */
    for (int i = 0; i < decl_counts[0]; i++) { free(decl_names[0][i]); free(decl_types[0][i]); }
    free(decl_names[0]); free(decl_types[0]); free(decl_flags[0]);
    free(calls);
    if (!spans) return 0;

    /* Sort spans by (name_start asc, rparen_end desc) to build nesting. */
    for (int i = 0; i < sn; i++) {
        for (int j = i + 1; j < sn; j++) {
            int swap = 0;
            if (spans[j].name_start < spans[i].name_start) swap = 1;
            else if (spans[j].name_start == spans[i].name_start && spans[j].rparen_end > spans[i].rparen_end) swap = 1;
            if (swap) { CCClosureCallSpan t = spans[i]; spans[i] = spans[j]; spans[j] = t; }
        }
    }

    int stack[256];
    int sp = 0;
    for (int i = 0; i < sn; i++) {
        while (sp > 0) {
            int top = stack[sp - 1];
            if (spans[i].name_start >= spans[top].rparen_end) { sp--; continue; }
            break;
        }
        if (sp > 0) {
            int parent = stack[sp - 1];
            spans[i].parent = parent;
            cc__span_add_child(spans, parent, i);
        }
        if (sp < (int)(sizeof(stack)/sizeof(stack[0]))) stack[sp++] = i;
    }

    /* Emit rewritten source */
    char* out = NULL;
    size_t out_len2 = 0, out_cap2 = 0;
    size_t cur = 0;
    for (int i = 0; i < sn; i++) {
        if (spans[i].parent != -1) continue;
        if (spans[i].name_start > cur) cc__append_n(&out, &out_len2, &out_cap2, in_src + cur, spans[i].name_start - cur);
        /* Emit rewritten call */
        size_t nm_s = spans[i].name_start;
        size_t nm_e = spans[i].lparen;
        while (nm_e > nm_s && (in_src[nm_e - 1] == ' ' || in_src[nm_e - 1] == '\t' || in_src[nm_e - 1] == '\n' || in_src[nm_e - 1] == '\r')) nm_e--;
        char nm[128];
        size_t nn = nm_e > nm_s ? (nm_e - nm_s) : 0;
        if (nn > 0 && nn < sizeof(nm)) {
            memcpy(nm, in_src + nm_s, nn);
            nm[nn] = '\0';
            cc__emit_call_replacement(in_src, nm, spans, i, &out, &out_len2, &out_cap2);
        } else {
            cc__append_n(&out, &out_len2, &out_cap2, in_src + spans[i].name_start, spans[i].rparen_end - spans[i].name_start);
        }
        cur = spans[i].rparen_end;
    }
    if (cur < in_len) cc__append_n(&out, &out_len2, &out_cap2, in_src + cur, in_len - cur);

    for (int i = 0; i < sn; i++) free(spans[i].children);
    free(spans);

    if (!out) return 0;
    *out_src = out;
    *out_len = out_len2;
    return 1;
}

/* NEW: Collect closure call edits into EditBuffer.
   NOTE: This pass has complex span nesting logic.
   For now, this function runs the rewrite and uses a coarse-grained edit.
   Future: refactor to collect edits directly. */
int cc__collect_closure_calls_edits(const CCASTRoot* root,
                                    const CCVisitorCtx* ctx,
                                    CCEditBuffer* eb) {
    if (!root || !ctx || !eb || !eb->src) return 0;

    char* rewritten = NULL;
    size_t rewritten_len = 0;
    int r = cc__rewrite_all_closure_calls_with_nodes(root, ctx, eb->src, eb->src_len, &rewritten, &rewritten_len);
    if (r <= 0 || !rewritten) return 0;

    if (rewritten_len != eb->src_len || memcmp(rewritten, eb->src, eb->src_len) != 0) {
        if (cc_edit_buffer_add(eb, 0, eb->src_len, rewritten, 90, "closure_calls") == 0) {
            free(rewritten);
            return 1;
        }
    }
    free(rewritten);
    return 0;
}
