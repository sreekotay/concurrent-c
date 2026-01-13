#include "visitor.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdarg.h>
#include <unistd.h>

#include "visitor/ufcs.h"
#include "visitor/pass_strip_markers.h"
#include "visitor/pass_await_normalize.h"
#include "visitor/pass_ufcs.h"
#include "visitor/pass_closure_calls.h"
#include "visitor/pass_autoblock.h"
#include "visitor/visitor_fileutil.h"
#include "parser/tcc_bridge.h"
#include "preprocess/preprocess.h"

#ifndef CC_TCC_EXT_AVAILABLE
#error "CC_TCC_EXT_AVAILABLE is required (patched TCC stub-AST required)."
#endif

/* AST-driven async lowering (implemented in `cc/src/visitor/async_ast.c`). */
int cc_async_rewrite_state_machine_ast(const CCASTRoot* root,
                                       const CCVisitorCtx* ctx,
                                       const char* in_src,
                                       size_t in_len,
                                       char** out_src,
                                       size_t* out_len);

/* --- Closure scan/lowering helpers (best-effort, early) ---
   Goal: allow `spawn(() => { ... })` to lower to valid C by generating a
   top-level env+thunk and rewriting the spawn statement to use CCClosure0. */

typedef struct {
    int start_line;
    int end_line;
    int nursery_id;
    int id;
    int start_col; /* 0-based, in start_line */
    int end_col;   /* 0-based, in end_line (exclusive) */
    int param_count;   /* 0..2 (early) */
    char* param0_name; /* owned; NULL if param_count<1 */
    char* param1_name; /* owned; NULL if param_count<2 */
    char* param0_type; /* owned; NULL if inferred */
    char* param1_type; /* owned; NULL if inferred */
    char** cap_names;
    char** cap_types; /* parallel to cap_names; NULL if unknown */
    unsigned char* cap_flags; /* parallel; bit0=is_slice, bit1=move-only */
    int cap_count;
    char* body; /* includes surrounding { ... } */
} CCClosureDesc;

/* Forward decl (used by early checkers). */
static int cc__scan_spawn_closures(const char* src,
                                  size_t src_len,
                                  const char* src_path,
                                  int line_base,
                                  int* io_next_closure_id,
                                  CCClosureDesc** out_descs,
                                  int* out_count,
                                  int** out_line_map,
                                  int* out_line_cap,
                                  char** out_protos,
                                  size_t* out_protos_len,
                                  char** out_defs,
                                  size_t* out_defs_len);

static int cc__is_ident_start_char(char c) { return (c == '_' || isalpha((unsigned char)c)); }
static int cc__is_ident_char2(char c) { return (c == '_' || isalnum((unsigned char)c)); }

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
            /* allow spaces: [ : ] or [ : ! ] */
            while (t < ty_e && *t != ']') t++;
            if (t < ty_e) {
                /* very small heuristic: contains ':' inside brackets => slice-ish */
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
                const char* lp = strchr(mk, '(');
                if (lp) {
                    /* Parse 2nd argument (unique) in cc_slice_make_id(a, unique, ...). */
                    const char* t = lp + 1;
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
                                /* now at start of arg2 */
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

static unsigned char cc__lookup_decl_flags(char** scope_names,
                                           unsigned char* scope_flags,
                                           int n,
                                           const char* name) {
    if (!scope_names || !scope_flags || !name) return 0;
    for (int i = 0; i < n; i++) {
        if (!scope_names[i]) continue;
        if (strcmp(scope_names[i], name) == 0) return scope_flags[i];
    }
    return 0;
}

/* (deleted deprecated moved-name tracking; superseded by stub-AST checker.c) */

/* (deleted deprecated text-based slice checker; superseded by stub-AST checker.c) */

static void cc__collect_caps_from_block(char*** scope_names,
                                        int* scope_counts,
                                        int max_depth,
                                        const char* block,
                                        const char* ignore_name0,
                                        const char* ignore_name1,
                                        char*** out_caps,
                                        int* out_cap_count) {
    if (!scope_names || !scope_counts || !block || !out_caps || !out_cap_count) return;
    const char* p = block;
    while (*p) {
        if (*p == '"' || *p == '\'') {
            char q = *p++;
            while (*p) {
                if (*p == '\\' && p[1]) { p += 2; continue; }
                if (*p == q) { p++; break; }
                p++;
            }
            continue;
        }
        if (!cc__is_ident_start_char(*p)) { p++; continue; }
        const char* s = p++;
        while (cc__is_ident_char2(*p)) p++;
        size_t n = (size_t)(p - s);
        if (cc__is_keyword_tok(s, n)) continue;
        if (ignore_name0 && strlen(ignore_name0) == n && strncmp(ignore_name0, s, n) == 0) continue;
        if (ignore_name1 && strlen(ignore_name1) == n && strncmp(ignore_name1, s, n) == 0) continue;
        /* ignore member access */
        if (s > block && (s[-1] == '.' || (s[-1] == '>' && s > block + 1 && s[-2] == '-'))) continue;
        int found = 0;
        /* Only treat non-global names as captures for now.
           Globals (depth 0) can be referenced directly and should not force capture/env. */
        for (int d = max_depth; d >= 1 && !found; d--) {
            if (cc__name_in_list(scope_names[d], scope_counts[d], s, n)) found = 1;
        }
        if (!found) continue;
        if (cc__name_in_list(*out_caps, *out_cap_count, s, n)) continue;
        char* name = (char*)malloc(n + 1);
        if (!name) continue;
        memcpy(name, s, n);
        name[n] = '\0';
        char** next = (char**)realloc(*out_caps, (size_t)(*out_cap_count + 1) * sizeof(char*));
        if (!next) { free(name); continue; }
        *out_caps = next;
        (*out_caps)[*out_cap_count] = name;
        (*out_cap_count)++;
    }
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

/* cc__ab_only_ws_comments moved to pass_autoblock.c */

#ifdef CC_TCC_EXT_AVAILABLE
static size_t cc__offset_of_line_1based(const char* s, size_t len, int line_no);
static size_t cc__offset_of_line_col_1based(const char* s, size_t len, int line_no, int col_no);
static int cc__node_file_matches_this_tu(const CCASTRoot* root,
                                         const CCVisitorCtx* ctx,
                                         const char* file);
#endif

#ifdef CC_TCC_EXT_AVAILABLE
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
#endif

/* Forward decl: used by closure scan to recursively lower closure bodies. */
static char* cc__lower_cc_in_block_text(const char* text,
                                       size_t text_len,
                                       const char* src_path,
                                       int base_line,
                                       int* io_next_closure_id,
                                       char** out_more_protos,
                                       size_t* out_more_protos_len,
                                       char** out_more_defs,
                                       size_t* out_more_defs_len);

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



/* (deleted unused CCStubCallSpan + deprecated cc__linecol_to_offset helper) */



typedef struct {
    int line_start;
    int col_start;
    int line_end;
    int col_end;
    const char* callee; /* identifier */
    int occ_1based;     /* Nth occurrence of this callee call on the start line */
    int arity;          /* 1 or 2 */
} CCClosureCallNode;

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

/* Closure-call rewrite lives in pass_closure_calls.c now (cc__rewrite_all_closure_calls_with_nodes). */

/* Autoblock rewrite lives in pass_autoblock.c now (cc__rewrite_autoblocking_calls_with_nodes). */

/* Scan `src` for spawn closures and generate top-level thunks.
   Outputs:
   - `*out_descs`, `*out_count`
   - `*out_line_map`: 1-based line -> (index+1) into desc array, 0 if none
   - `*out_line_cap`: number of lines allocated
   - `*out_defs`: C defs to emit before #line 1 */
static int cc__scan_spawn_closures(const char* src,
                                  size_t src_len,
                                  const char* src_path,
                                  int line_base,
                                  int* io_next_closure_id,
                                  CCClosureDesc** out_descs,
                                  int* out_count,
                                  int** out_line_map,
                                  int* out_line_cap,
                                  char** out_protos,
                                  size_t* out_protos_len,
                                  char** out_defs,
                                  size_t* out_defs_len) {
    if (!src || !out_descs || !out_count || !out_line_map || !out_line_cap || !out_defs || !out_defs_len) return 0;
    *out_descs = NULL;
    *out_count = 0;
    *out_line_map = NULL;
    *out_line_cap = 0;
    if (out_protos) *out_protos = NULL;
    if (out_protos_len) *out_protos_len = 0;
    *out_defs = NULL;
    *out_defs_len = 0;

    int lines = 1;
    for (size_t i = 0; i < src_len; i++) if (src[i] == '\n') lines++;
    int* line_map = (int*)calloc((size_t)lines + 2, sizeof(int));
    if (!line_map) return 0;

    CCClosureDesc* descs = NULL;
    int count = 0, cap = 0;
    char* protos = NULL;
    size_t protos_len = 0, protos_cap = 0;
    char* defs = NULL;
    size_t defs_len = 0, defs_cap = 0;

    char** scope_names[256];
    char** scope_types[256];
    unsigned char* scope_flags[256];
    int scope_counts[256];
    for (int i = 0; i < 256; i++) { scope_names[i] = NULL; scope_types[i] = NULL; scope_flags[i] = NULL; scope_counts[i] = 0; }
    int depth = 0;
    int nursery_stack[128];
    int nursery_depth[128];
    int nursery_top = -1;
    int nursery_counter = 0;

    const char* cur = src;
    int line_no = 1;
    while ((size_t)(cur - src) < src_len && *cur) {
        const char* line_start = cur;
        const char* nl = strchr(cur, '\n');
        const char* line_end = nl ? nl : (src + src_len);
        size_t line_len = (size_t)(line_end - line_start);

        char tmp_line[1024];
        size_t cp = line_len < sizeof(tmp_line) - 1 ? line_len : sizeof(tmp_line) - 1;
        memcpy(tmp_line, line_start, cp);
        tmp_line[cp] = '\0';
        cc__maybe_record_decl(scope_names, scope_types, scope_flags, scope_counts, depth, tmp_line);

        /* nursery marker */
        const char* t = line_start;
        while (t < line_end && (*t == ' ' || *t == '\t')) t++;
        if ((line_end - t) >= 8 && strncmp(t, "@nursery", 8) == 0) {
            int nid = ++nursery_counter;
            if (nursery_top + 1 < 128) {
                nursery_stack[++nursery_top] = nid;
                nursery_depth[nursery_top] = -1;
            }
        }

        /* closure literal:
           - `() => { ... }` / `() => expr`
           - `(x) => { ... }` / `(x) => expr`
           - `x => { ... }` / `x => expr`
           (best-effort scan) */
        {
            int consumed_literal = 0;
            const char* s = line_start;
            int in_str = 0;
            char str_q = 0;
            while (s < line_end) {
                char c = *s;
                if (in_str) {
                    if (c == '\\' && (s + 1) < line_end) { s += 2; continue; }
                    if (c == str_q) in_str = 0;
                    s++;
                    continue;
                }
                if (c == '"' || c == '\'') { in_str = 1; str_q = c; s++; continue; }
                int param_count = 0;
                char param0[128] = {0};
                char param1[128] = {0};
                char param0_type[128] = {0};
                char param1_type[128] = {0};
                const char* p = NULL;

                if (c == '(') {
                    /* Parse `( ... ) =>` where `...` is empty, `x`, `x,y`, `int x`, `int x, int y`, etc. */
                    const char* rp = s + 1;
                    while (rp < line_end && *rp != ')') rp++;
                    if (rp >= line_end || *rp != ')') { s++; continue; }

                    const char* after_rp = rp + 1;
                    while (after_rp < line_end && (*after_rp == ' ' || *after_rp == '\t')) after_rp++;
                    if ((after_rp + 2) > line_end || after_rp[0] != '=' || after_rp[1] != '>') { s++; continue; }
                    p = after_rp + 2;

                    /* Parse params substring [s+1, rp). */
                    const char* ps = s + 1;
                    const char* pe = rp;
                    while (ps < pe && (*ps == ' ' || *ps == '\t')) ps++;
                    while (pe > ps && (pe[-1] == ' ' || pe[-1] == '\t')) pe--;

                    param_count = 0;
                    if (ps < pe) {
                        /* Split by top-level commas (no nesting expected in early param list). */
                        const char* seg_s = ps;
                        int seg_idx = 0;
                        for (const char* z = ps; z <= pe; z++) {
                            int at_end = (z == pe);
                            if (!at_end && *z != ',') continue;
                            const char* seg_e = z;
                            while (seg_s < seg_e && (*seg_s == ' ' || *seg_s == '\t')) seg_s++;
                            while (seg_e > seg_s && (seg_e[-1] == ' ' || seg_e[-1] == '\t')) seg_e--;
                            if (seg_e <= seg_s) { seg_s = z + 1; continue; }

                            /* Find last identifier in segment: it's the param name; prefix is type (optional). */
                            const char* nm_e = seg_e;
                            while (nm_e > seg_s && !cc__is_ident_char2(nm_e[-1])) nm_e--;
                            const char* nm_s = nm_e;
                            while (nm_s > seg_s && cc__is_ident_char2(nm_s[-1])) nm_s--;
                            if (nm_s >= nm_e || !cc__is_ident_start_char(*nm_s)) { break; }

                            size_t nm_n = (size_t)(nm_e - nm_s);
                            if (nm_n >= sizeof(param0)) { break; }

                            const char* ty_s = seg_s;
                            const char* ty_e = nm_s;
                            while (ty_e > ty_s && (ty_e[-1] == ' ' || ty_e[-1] == '\t')) ty_e--;

                            if (seg_idx == 0) {
                                memcpy(param0, nm_s, nm_n); param0[nm_n] = '\0';
                                if (ty_e > ty_s) {
                                    size_t tn = (size_t)(ty_e - ty_s);
                                    if (tn >= sizeof(param0_type)) tn = sizeof(param0_type) - 1;
                                    memcpy(param0_type, ty_s, tn); param0_type[tn] = '\0';
                                } else {
                                    param0_type[0] = '\0';
                                }
                                param_count = 1;
                            } else if (seg_idx == 1) {
                                memcpy(param1, nm_s, nm_n); param1[nm_n] = '\0';
                                if (ty_e > ty_s) {
                                    size_t tn = (size_t)(ty_e - ty_s);
                                    if (tn >= sizeof(param1_type)) tn = sizeof(param1_type) - 1;
                                    memcpy(param1_type, ty_s, tn); param1_type[tn] = '\0';
                                } else {
                                    param1_type[0] = '\0';
                                }
                                param_count = 2;
                            } else {
                                break;
                            }

                            seg_idx++;
                            seg_s = z + 1;
                        }
                        if (param_count == 0) { s++; continue; }
                    }
                } else if (cc__is_ident_start_char(c)) {
                    /* x => ... */
                    const char* n0 = s;
                    const char* q = s + 1;
                    while (q < line_end && cc__is_ident_char2(*q)) q++;
                    size_t nn = (size_t)(q - n0);
                    if (nn == 0 || nn >= sizeof(param0) || cc__is_keyword_tok(n0, nn)) { s++; continue; }
                    const char* r = q;
                    while (r < line_end && (*r == ' ' || *r == '\t')) r++;
                    if ((r + 2) <= line_end && r[0] == '=' && r[1] == '>') {
                        memcpy(param0, n0, nn);
                        param0[nn] = '\0';
                        param_count = 1;
                        param0_type[0] = '\0';
                        p = r + 2;
                    } else {
                        s++; continue;
                    }
                } else {
                    s++; continue;
                }

                while (p < line_end && (*p == ' ' || *p == '\t')) p++;
                if (p >= line_end) { s++; continue; }

                const char* body_start = p;
                const char* body_end = NULL; /* exclusive */
                int end_line = line_no;
                int end_col = 0;

                if (*body_start == '{') {
                    const char* b = body_start;
                    int br = 0;
                    int in_str2 = 0;
                    char q2 = 0;
                    while ((size_t)(b - src) < src_len) {
                        char ch = *b++;
                        if (in_str2) {
                            if (ch == '\\' && (size_t)(b - src) < src_len) { b++; continue; }
                            if (ch == q2) in_str2 = 0;
                            continue;
                        }
                        if (ch == '"' || ch == '\'') { in_str2 = 1; q2 = ch; continue; }
                        if (ch == '{') br++;
                        else if (ch == '}') { br--; if (br == 0) break; }
                    }
                    if (br != 0) { s++; continue; }
                    body_end = b;
                } else {
                    /* expression body: scan until delimiter at nesting depth 0 */
                    const char* b = body_start;
                    int par = 0, brk = 0;
                    int in_str2 = 0;
                    char q2 = 0;
                    while ((size_t)(b - src) < src_len) {
                        char ch = *b;
                        if (in_str2) {
                            if (ch == '\\' && (size_t)((b + 1) - src) < src_len) { b += 2; continue; }
                            if (ch == q2) in_str2 = 0;
                            b++;
                            continue;
                        }
                        if (ch == '"' || ch == '\'') { in_str2 = 1; q2 = ch; b++; continue; }
                        if (ch == '(') par++;
                        else if (ch == ')') { if (par == 0 && brk == 0) break; par--; }
                        else if (ch == '[') brk++;
                        else if (ch == ']') { if (brk == 0 && par == 0) break; brk--; }
                        if (par == 0 && brk == 0) {
                            if (ch == ',' || ch == ';' || ch == '}' || ch == '\n') break;
                        }
                        b++;
                    }
                    body_end = b;
                }

                /* Compute end_line/end_col based on body_end */
                for (const char* x = body_start; x < body_end; x++) if (*x == '\n') end_line++;
                const char* last_nl = NULL;
                for (const char* x = body_start; x < body_end; x++) if (*x == '\n') last_nl = x;
                if (last_nl) end_col = (int)(body_end - (last_nl + 1));
                else end_col = (int)(body_end - line_start);

                int nid = (nursery_top >= 0) ? nursery_stack[nursery_top] : 0;

                char* body = (char*)malloc((size_t)(body_end - body_start) + 1);
                if (!body) { free(line_map); return 0; }
                memcpy(body, body_start, (size_t)(body_end - body_start));
                body[(size_t)(body_end - body_start)] = '\0';

                                char** caps = NULL;
                                int cap_n = 0;
                                cc__collect_caps_from_block(scope_names, scope_counts, depth, body,
                                                            (param_count >= 1 ? param0 : NULL),
                                                            (param_count >= 2 ? param1 : NULL),
                                                            &caps, &cap_n);
                                char** cap_types = NULL;
                                unsigned char* cap_flags = NULL;
                                if (cap_n > 0) {
                                    cap_types = (char**)calloc((size_t)cap_n, sizeof(char*));
                                    cap_flags = (unsigned char*)calloc((size_t)cap_n, sizeof(unsigned char));
                                    if (!cap_types || !cap_flags) { free(cap_types); free(cap_flags); free(body); free(line_map); return 0; }
                                    for (int ci = 0; ci < cap_n; ci++) {
                                        const char* ty = NULL;
                                        unsigned char fl = 0;
                                        for (int d = depth; d >= 1 && !ty; d--) {
                                            ty = cc__lookup_decl_type(scope_names[d], scope_types[d], scope_counts[d], caps[ci]);
                                            if (ty) fl = cc__lookup_decl_flags(scope_names[d], scope_flags[d], scope_counts[d], caps[ci]);
                                        }
                                        if (ty) {
                                            cap_types[ci] = strdup(ty);
                                        }
                                        cap_flags[ci] = fl;
                                    }
                                }

                if (count == cap) {
                    cap = cap ? cap * 2 : 16;
                    CCClosureDesc* nd = (CCClosureDesc*)realloc(descs, (size_t)cap * sizeof(CCClosureDesc));
                    if (!nd) { free(body); free(line_map); return 0; }
                    descs = nd;
                }
                int id = io_next_closure_id ? (*io_next_closure_id)++ : (count + 1);
                int abs_line = (line_base > 0 ? (line_base + line_no - 1) : line_no);
                int start_col = (int)(s - line_start);
                                descs[count++] = (CCClosureDesc){
                    .start_line = line_no,
                    .end_line = end_line,
                    .nursery_id = nid,
                    .id = id,
                    .start_col = start_col,
                    .end_col = end_col,
                    .param_count = param_count,
                    .param0_name = (param_count >= 1) ? strdup(param0) : NULL,
                    .param1_name = (param_count >= 2) ? strdup(param1) : NULL,
                    .param0_type = (param_count >= 1 && param0_type[0]) ? strdup(param0_type) : NULL,
                    .param1_type = (param_count >= 2 && param1_type[0]) ? strdup(param1_type) : NULL,
                    .cap_names = caps,
                                    .cap_types = cap_types,
                                    .cap_flags = cap_flags,
                    .cap_count = cap_n,
                    .body = body,
                };
                if (line_no <= lines) line_map[line_no] = count; /* 1-based index */

                {
                    char pb[128];
                    if (param_count == 0)
                        snprintf(pb, sizeof(pb), "static void* __cc_closure_entry_%d(void*);\n", id);
                    else if (param_count == 1)
                        snprintf(pb, sizeof(pb), "static void* __cc_closure_entry_%d(void*, intptr_t);\n", id);
                    else
                        snprintf(pb, sizeof(pb), "static void* __cc_closure_entry_%d(void*, intptr_t, intptr_t);\n", id);
                    cc__append_str(&protos, &protos_len, &protos_cap, pb);
                }
                {
                    /* Factory that captures by value into a heap env and returns a CCClosure0. */
                    cc__append_fmt(&protos, &protos_len, &protos_cap, "static %s __cc_closure_make_%d(",
                                   (param_count == 0 ? "CCClosure0" : (param_count == 1 ? "CCClosure1" : "CCClosure2")),
                                   id);
                    if (cap_n == 0) {
                        cc__append_str(&protos, &protos_len, &protos_cap, "void");
                    } else {
                for (int ci = 0; ci < cap_n; ci++) {
                            if (ci) cc__append_str(&protos, &protos_len, &protos_cap, ", ");
                            cc__append_fmt(&protos, &protos_len, &protos_cap,
                                           "%s %s",
                                           cap_types && cap_types[ci] ? cap_types[ci] : "int",
                                           caps[ci] ? caps[ci] : "__cap");
                        }
                    }
                    cc__append_str(&protos, &protos_len, &protos_cap, ");\n");
                }

                /* Always emit a closure definition. If there are captures, emit an env + make() helper. */
                {
                    char* more_protos = NULL;
                    size_t more_protos_len = 0;
                    char* more_defs = NULL;
                    size_t more_defs_len = 0;
                    char* lowered = NULL;
                    /* Only lower nested CC constructs inside block bodies for now.
                       (Expression bodies may need a separate lowering path that doesn't inject directives.) */
                    if (body && body[0] == '{') {
                        lowered = cc__lower_cc_in_block_text(body, strlen(body),
                                                             src_path, abs_line,
                                                             io_next_closure_id,
                                                             &more_protos, &more_protos_len,
                                                             &more_defs, &more_defs_len);
                    }
                    if (more_protos && more_protos_len) cc__append_str(&protos, &protos_len, &protos_cap, more_protos);
                    if (more_defs && more_defs_len) cc__append_str(&defs, &defs_len, &defs_cap, more_defs);
                    free(more_protos);
                    free(more_defs);

                    cc__append_fmt(&defs, &defs_len, &defs_cap,
                                   "/* CC closure %d (from %s:%d) */\n",
                                   id, src_path ? src_path : "<src>", abs_line);

                    if (cap_n > 0) {
                        cc__append_fmt(&defs, &defs_len, &defs_cap, "typedef struct __cc_closure_env_%d {\n", id);
                        for (int ci = 0; ci < cap_n; ci++) {
                            cc__append_fmt(&defs, &defs_len, &defs_cap,
                                           "  %s %s;\n",
                                           cap_types && cap_types[ci] ? cap_types[ci] : "int",
                                           caps[ci] ? caps[ci] : "__cap");
                        }
                        cc__append_str(&defs, &defs_len, &defs_cap, "} ");
                        cc__append_fmt(&defs, &defs_len, &defs_cap, "__cc_closure_env_%d;\n", id);
                        cc__append_fmt(&defs, &defs_len, &defs_cap,
                                       "static void __cc_closure_env_%d_drop(void* p) { if (p) free(p); }\n",
                                       id);
                        cc__append_fmt(&defs, &defs_len, &defs_cap, "static %s __cc_closure_make_%d(",
                                       (param_count == 0 ? "CCClosure0" : (param_count == 1 ? "CCClosure1" : "CCClosure2")),
                                       id);
                        for (int ci = 0; ci < cap_n; ci++) {
                            if (ci) cc__append_str(&defs, &defs_len, &defs_cap, ", ");
                            cc__append_fmt(&defs, &defs_len, &defs_cap,
                                           "%s %s",
                                           cap_types && cap_types[ci] ? cap_types[ci] : "int",
                                           caps[ci] ? caps[ci] : "__cap");
                        }
                        cc__append_str(&defs, &defs_len, &defs_cap, ") {\n");
                        cc__append_fmt(&defs, &defs_len, &defs_cap,
                                       "  __cc_closure_env_%d* __env = (__cc_closure_env_%d*)malloc(sizeof(__cc_closure_env_%d));\n",
                                       id, id, id);
                        cc__append_str(&defs, &defs_len, &defs_cap, "  if (!__env) abort();\n");
                        for (int ci = 0; ci < cap_n; ci++) {
                            int mo = (cap_flags && (cap_flags[ci] & 2) != 0);
                            cc__append_fmt(&defs, &defs_len, &defs_cap,
                                           "  __env->%s = %s%s%s;\n",
                                           caps[ci] ? caps[ci] : "__cap",
                                           mo ? "cc_move(" : "",
                                           caps[ci] ? caps[ci] : "__cap",
                                           mo ? ")" : "");
                        }
                        cc__append_fmt(&defs, &defs_len, &defs_cap,
                                       "  return %s(__cc_closure_entry_%d, __env, __cc_closure_env_%d_drop);\n",
                                       (param_count == 0 ? "cc_closure0_make" : (param_count == 1 ? "cc_closure1_make" : "cc_closure2_make")),
                                       id, id);
                        cc__append_str(&defs, &defs_len, &defs_cap, "}\n");
                    } else {
                        cc__append_fmt(&defs, &defs_len, &defs_cap,
                                       "static %s __cc_closure_make_%d(void) { return %s(__cc_closure_entry_%d, NULL, NULL); }\n",
                                       (param_count == 0 ? "CCClosure0" : (param_count == 1 ? "CCClosure1" : "CCClosure2")),
                                       id,
                                       (param_count == 0 ? "cc_closure0_make" : (param_count == 1 ? "cc_closure1_make" : "cc_closure2_make")),
                                       id);
                    }

                    cc__append_fmt(&defs, &defs_len, &defs_cap,
                                   "static void* __cc_closure_entry_%d(%s) {\n",
                                   id,
                                   (param_count == 0 ? "void* __p" : (param_count == 1 ? "void* __p, intptr_t __arg0" : "void* __p, intptr_t __arg0, intptr_t __arg1")));
                    if (cap_n > 0) {
                        cc__append_fmt(&defs, &defs_len, &defs_cap,
                                       "  __cc_closure_env_%d* __env = (__cc_closure_env_%d*)__p;\n",
                                       id, id);
                        for (int ci = 0; ci < cap_n; ci++) {
                            int mo = (cap_flags && (cap_flags[ci] & 2) != 0);
                            cc__append_fmt(&defs, &defs_len, &defs_cap,
                                           "  %s %s = %s__env->%s%s;\n",
                                           cap_types && cap_types[ci] ? cap_types[ci] : "int",
                                           caps[ci] ? caps[ci] : "__cap",
                                           mo ? "cc_move(" : "",
                                           caps[ci] ? caps[ci] : "__cap",
                                           mo ? ")" : "");
                        }
                    } else {
                        cc__append_str(&defs, &defs_len, &defs_cap, "  (void)__p;\n");
                    }
                    if (param_count == 1) {
                        if (param0[0]) {
                            if (param0_type[0]) {
                                cc__append_fmt(&defs, &defs_len, &defs_cap, "  %s %s = (%s)__arg0;\n", param0_type, param0, param0_type);
                            } else {
                                cc__append_fmt(&defs, &defs_len, &defs_cap, "  intptr_t %s = __arg0;\n", param0);
                            }
                        }
                        else cc__append_str(&defs, &defs_len, &defs_cap, "  (void)__arg0;\n");
                    } else if (param_count == 2) {
                        if (param0[0]) {
                            if (param0_type[0]) {
                                cc__append_fmt(&defs, &defs_len, &defs_cap, "  %s %s = (%s)__arg0;\n", param0_type, param0, param0_type);
                            } else {
                                cc__append_fmt(&defs, &defs_len, &defs_cap, "  intptr_t %s = __arg0;\n", param0);
                            }
                        }
                        else cc__append_str(&defs, &defs_len, &defs_cap, "  (void)__arg0;\n");
                        if (param1[0]) {
                            if (param1_type[0]) {
                                cc__append_fmt(&defs, &defs_len, &defs_cap, "  %s %s = (%s)__arg1;\n", param1_type, param1, param1_type);
                            } else {
                                cc__append_fmt(&defs, &defs_len, &defs_cap, "  intptr_t %s = __arg1;\n", param1);
                            }
                        }
                        else cc__append_str(&defs, &defs_len, &defs_cap, "  (void)__arg1;\n");
                    }

                    cc__append_fmt(&defs, &defs_len, &defs_cap,
                                   "#line %d \"%s\"\n",
                                   abs_line, src_path ? src_path : "<src>");
                    if (body && body[0] == '{') {
                        cc__append_fmt(&defs, &defs_len, &defs_cap, "  %s\n", lowered ? lowered : body);
                    } else {
                        cc__append_fmt(&defs, &defs_len, &defs_cap, "  (void)(%s);\n", lowered ? lowered : body);
                    }
                    free(lowered);
                    cc__append_str(&defs, &defs_len, &defs_cap, "  return NULL;\n}\n\n");
                }

                /* advance cursor to end of literal */
                cur = body_end;
                line_no = end_line;
                /* If we ended at newline boundary, allow outer loop to progress normally. */
                if (*cur == '\n') { cur++; line_no++; }
                consumed_literal = 1;
                break;
            }
            if (consumed_literal) continue;
        }

        /* brace depth + scope cleanup (best-effort) */
        for (const char* x = line_start; x < line_end; x++) {
            if (*x == '{') {
                depth++;
                if (nursery_top >= 0 && nursery_depth[nursery_top] < 0) nursery_depth[nursery_top] = depth;
            } else if (*x == '}') {
                if (nursery_top >= 0 && nursery_depth[nursery_top] == depth) nursery_top--;
                if (depth > 0) {
                    for (int j = 0; j < scope_counts[depth]; j++) free(scope_names[depth][j]);
                    free(scope_names[depth]); scope_names[depth] = NULL;
                    for (int j = 0; j < scope_counts[depth]; j++) free(scope_types[depth][j]);
                    free(scope_types[depth]); scope_types[depth] = NULL;
                    free(scope_flags[depth]); scope_flags[depth] = NULL;
                    scope_counts[depth] = 0;
                    depth--;
                }
            }
        }

        if (!nl) break;
        cur = nl + 1;
        line_no++;
    }

    /* scope names/types cleanup */
    for (int d = 0; d < 256; d++) {
        for (int i = 0; i < scope_counts[d]; i++) free(scope_names[d][i]);
        free(scope_names[d]);
        for (int i = 0; i < scope_counts[d]; i++) free(scope_types[d][i]);
        free(scope_types[d]);
        free(scope_flags[d]);
    }

    *out_descs = descs;
    *out_count = count;
    *out_line_map = line_map;
    *out_line_cap = lines + 2;
    if (out_protos) *out_protos = protos;
    if (out_protos_len) *out_protos_len = protos_len;
    *out_defs = defs;
    *out_defs_len = defs_len;
    return 1;
}

/* Lower a block-ish snippet of CC/C code in-memory (used for closure bodies).
   Best-effort: currently handles @nursery + spawn closure-literals. */
static char* cc__lower_cc_snippet(const char* text,
                                 size_t text_len,
                                 const char* src_path,
                                 int base_line,
                                 CCClosureDesc* closure_descs,
                                 int closure_count,
                                 int* closure_line_map,
                                 int closure_line_cap) {
    if (!text || text_len == 0) return NULL;
    char* out = NULL;
    size_t out_len = 0, out_cap = 0;

    int nursery_counter = 0;
    int nursery_id_stack[128];
    int nursery_depth_stack[128];
    int nursery_top = -1;
    int brace_depth = 0;

    const char* cur = text;
    int line_no = 1;
    while ((size_t)(cur - text) < text_len && *cur) {
        const char* line_start = cur;
        const char* nl = memchr(cur, '\n', (size_t)(text + text_len - cur));
        const char* line_end = nl ? nl : (text + text_len);
        size_t line_len = (size_t)(line_end - line_start);

        char line_buf[2048];
        size_t cp = line_len < sizeof(line_buf) - 1 ? line_len : sizeof(line_buf) - 1;
        memcpy(line_buf, line_start, cp);
        line_buf[cp] = '\0';

        const char* p = line_buf;
        while (*p == ' ' || *p == '\t') p++;
        int abs_line = (base_line > 0 ? (base_line + line_no - 1) : line_no);

        /* Lower @nursery marker into a runtime nursery scope. */
        if (strncmp(p, "@nursery", 8) == 0 && (p[8] == ' ' || p[8] == '\t' || p[8] == '\n' || p[8] == '\r' || p[8] == '{')) {
            size_t indent_len = (size_t)(p - line_buf);
            char indent[256];
            if (indent_len >= sizeof(indent)) indent_len = sizeof(indent) - 1;
            memcpy(indent, line_buf, indent_len);
            indent[indent_len] = '\0';

            int id = ++nursery_counter;
            if (nursery_top + 1 < (int)(sizeof(nursery_id_stack) / sizeof(nursery_id_stack[0]))) {
                nursery_id_stack[++nursery_top] = id;
                nursery_depth_stack[nursery_top] = 0;
            }
            cc__append_fmt(&out, &out_len, &out_cap, "#line %d \"%s\"\n", abs_line, src_path ? src_path : "<src>");
            cc__append_fmt(&out, &out_len, &out_cap, "%sCCNursery* __cc_nursery%d = cc_nursery_create();\n", indent, id);
            cc__append_fmt(&out, &out_len, &out_cap, "%sif (!__cc_nursery%d) abort();\n", indent, id);
            cc__append_fmt(&out, &out_len, &out_cap, "%s{\n", indent);
            brace_depth++;
            if (nursery_top >= 0) nursery_depth_stack[nursery_top] = brace_depth;
            cc__append_fmt(&out, &out_len, &out_cap, "#line %d \"%s\"\n", abs_line + 1, src_path ? src_path : "<src>");
            goto next_line;
        }

        /* Lower spawn(() => { ... }) inside a nursery to cc_nursery_spawn_closure0. */
        if (strncmp(p, "spawn", 5) == 0 && (p[5] == ' ' || p[5] == '\t')) {
            int cur_nursery_id = (nursery_top >= 0) ? nursery_id_stack[nursery_top] : 0;
            const char* s0 = p + 5;
            while (*s0 == ' ' || *s0 == '\t') s0++;
            if (*s0 == '(') {
                /* Closure literal: spawn(() => { ... }); uses closure_line_map from the pre-scan. */
                if (closure_line_map && line_no > 0 && line_no < closure_line_cap) {
                    int idx1 = closure_line_map[line_no];
                    if (idx1 > 0 && idx1 <= closure_count) {
                        CCClosureDesc* cd = &closure_descs[idx1 - 1];
                        cc__append_fmt(&out, &out_len, &out_cap, "#line %d \"%s\"\n", abs_line, src_path ? src_path : "<src>");
                        cc__append_str(&out, &out_len, &out_cap, "{\n");
                        if (cur_nursery_id == 0) {
                            cc__append_str(&out, &out_len, &out_cap, "/* TODO: spawn outside nursery */\n");
                        } else {
                            cc__append_fmt(&out, &out_len, &out_cap, "  CCClosure0 __c = __cc_closure_make_%d(", cd->id);
                            if (cd->cap_count == 0) {
                                cc__append_str(&out, &out_len, &out_cap, ");\n");
                            } else {
                                for (int ci = 0; ci < cd->cap_count; ci++) {
                                    if (ci) cc__append_str(&out, &out_len, &out_cap, ", ");
                                    int mo = (cd->cap_flags && (cd->cap_flags[ci] & 2) != 0);
                                    if (mo) cc__append_str(&out, &out_len, &out_cap, "cc_move(");
                                    cc__append_str(&out, &out_len, &out_cap, cd->cap_names[ci] ? cd->cap_names[ci] : "0");
                                    if (mo) cc__append_str(&out, &out_len, &out_cap, ")");
                                }
                                cc__append_str(&out, &out_len, &out_cap, ");\n");
                            }
                            cc__append_fmt(&out, &out_len, &out_cap, "  cc_nursery_spawn_closure0(__cc_nursery%d, __c);\n", cur_nursery_id);
                        }
                        cc__append_str(&out, &out_len, &out_cap, "}\n");

                        /* Skip original closure text lines (multiline). */
                        int target_end = cd->end_line;
                        while (line_no < target_end) {
                            if (!nl) break;
                            cur = nl + 1;
                            line_no++;
                            nl = memchr(cur, '\n', (size_t)(text + text_len - cur));
                        }
                        cc__append_fmt(&out, &out_len, &out_cap, "#line %d \"%s\"\n", base_line + line_no, src_path ? src_path : "<src>");
                        goto next_line;
                    }
                }
            }
        }

        /* Before emitting a close brace, emit nursery epilogue if this closes a nursery scope. */
        if (p[0] == '}') {
            if (nursery_top >= 0 && nursery_depth_stack[nursery_top] == brace_depth) {
                size_t indent_len = (size_t)(p - line_buf);
                char indent[256];
                if (indent_len >= sizeof(indent)) indent_len = sizeof(indent) - 1;
                memcpy(indent, line_buf, indent_len);
                indent[indent_len] = '\0';

                int id = nursery_id_stack[nursery_top--];
                cc__append_fmt(&out, &out_len, &out_cap, "#line %d \"%s\"\n", abs_line, src_path ? src_path : "<src>");
                cc__append_fmt(&out, &out_len, &out_cap, "%s  cc_nursery_wait(__cc_nursery%d);\n", indent, id);
                cc__append_fmt(&out, &out_len, &out_cap, "%s  cc_nursery_free(__cc_nursery%d);\n", indent, id);
                cc__append_fmt(&out, &out_len, &out_cap, "#line %d \"%s\"\n", abs_line, src_path ? src_path : "<src>");
            }
        }

        /* Default: emit original line. */
        cc__append_fmt(&out, &out_len, &out_cap, "#line %d \"%s\"\n", abs_line, src_path ? src_path : "<src>");
        cc__append_str(&out, &out_len, &out_cap, line_buf);
        cc__append_str(&out, &out_len, &out_cap, "\n");

        /* Update brace depth. */
        for (size_t i = 0; i < cp; i++) {
            if (line_buf[i] == '{') brace_depth++;
            else if (line_buf[i] == '}') { if (brace_depth > 0) brace_depth--; }
        }

    next_line:
        if (!nl) break;
        cur = nl + 1;
        line_no++;
    }

    return out;
}

/* Recursively lower CC constructs inside a closure body, while collecting any additional closure thunks. */
static char* cc__lower_cc_in_block_text(const char* text,
                                       size_t text_len,
                                       const char* src_path,
                                       int base_line,
                                       int* io_next_closure_id,
                                       char** out_more_protos,
                                       size_t* out_more_protos_len,
                                       char** out_more_defs,
                                       size_t* out_more_defs_len) {
    if (out_more_protos) *out_more_protos = NULL;
    if (out_more_protos_len) *out_more_protos_len = 0;
    if (out_more_defs) *out_more_defs = NULL;
    if (out_more_defs_len) *out_more_defs_len = 0;
    if (!text || text_len == 0) return NULL;

    /* Pre-scan this snippet for nested spawn closures; this will also recursively generate their thunks. */
    CCClosureDesc* nested_descs = NULL;
    int nested_count = 0;
    int* nested_line_map = NULL;
    int nested_line_cap = 0;
    char* nested_protos = NULL;
    size_t nested_protos_len = 0;
    char* nested_defs = NULL;
    size_t nested_defs_len = 0;
    (void)cc__scan_spawn_closures(text, text_len, src_path,
                                 base_line, io_next_closure_id,
                                 &nested_descs, &nested_count,
                                 &nested_line_map, &nested_line_cap,
                                 &nested_protos, &nested_protos_len,
                                 &nested_defs, &nested_defs_len);

    if (out_more_protos) *out_more_protos = nested_protos;
    else free(nested_protos);
    if (out_more_protos_len) *out_more_protos_len = nested_protos_len;

    if (out_more_defs) *out_more_defs = nested_defs;
    else free(nested_defs);
    if (out_more_defs_len) *out_more_defs_len = nested_defs_len;

    char* lowered = cc__lower_cc_snippet(text, text_len, src_path, base_line,
                                         nested_descs, nested_count,
                                         nested_line_map, nested_line_cap);

    if (nested_descs) {
        for (int i = 0; i < nested_count; i++) {
            for (int j = 0; j < nested_descs[i].cap_count; j++) free(nested_descs[i].cap_names[j]);
            free(nested_descs[i].cap_names);
            for (int j = 0; j < nested_descs[i].cap_count; j++) free(nested_descs[i].cap_types ? nested_descs[i].cap_types[j] : NULL);
            free(nested_descs[i].cap_types);
            free(nested_descs[i].cap_flags);
            free(nested_descs[i].body);
        }
        free(nested_descs);
    }
    free(nested_line_map);

    return lowered;
}

/* Strip CC decl markers so output is valid C. This is used regardless of whether
   TCC extensions are available, because the output C is compiled by the host compiler. */
/* cc__read_entire_file / cc__write_temp_c_file are implemented in visitor_fileutil.c */

static const char* cc__basename(const char* path) {
    if (!path) return NULL;
    const char* last = path;
    for (const char* p = path; *p; p++) {
        if (*p == '/' || *p == '\\') last = p + 1;
    }
    return last;
}

/* Return pointer to a stable suffix (last 2 path components) inside `path`.
   If `path` has fewer than 2 components, returns basename. */
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

    /* Prefer 2-component suffix match (handles duplicate basenames across dirs). */
    const char* a_suf = cc__path_suffix2(a);
    const char* b_suf = cc__path_suffix2(b);
    if (a_suf && b_suf && strcmp(a_suf, b_suf) == 0) return 1;

    /* Fallback: basename-only match. */
    return 1;
}

/* UFCS span rewrite lives in pass_ufcs.c now (cc__rewrite_ufcs_spans_with_nodes). */

#ifdef CC_TCC_EXT_AVAILABLE
static int cc__node_file_matches_this_tu(const CCASTRoot* root,
                                        const CCVisitorCtx* ctx,
                                        const char* node_file) {
    if (!ctx || !ctx->input_path || !node_file) return 0;
    if (cc__same_source_file(ctx->input_path, node_file)) return 1;
    if (root && root->lowered_path && cc__same_source_file(root->lowered_path, node_file)) return 1;
    return 0;
}

static int cc__arena_args_for_line(const CCASTRoot* root,
                                   const char* src_path,
                                   int line_no,
                                   const char** out_name,
                                   const char** out_size_expr) {
    if (!root || !root->nodes || root->node_count <= 0 || !src_path || line_no <= 0)
        return 0;
    if (out_name) *out_name = NULL;
    if (out_size_expr) *out_size_expr = NULL;

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
    for (int i = 0; i < root->node_count; i++) {
        if (n[i].kind != 4) /* CC_AST_NODE_ARENA */
            continue;
        /* Prefer node file matching against input or lowered temp file. */
        if (!cc__same_source_file(src_path, n[i].file))
            continue;
        if (n[i].line_start != line_no)
            continue;

        if (out_name) *out_name = n[i].aux_s1;
        if (out_size_expr) *out_size_expr = n[i].aux_s2;
        return 1;
    }
    return 0;
}
#endif


int cc_visit(const CCASTRoot* root, CCVisitorCtx* ctx, const char* output_path) {
    if (!ctx || !ctx->symbols || !output_path) return EINVAL;
    const char* src_path = ctx->input_path ? ctx->input_path : "<cc_input>";
    FILE* out = fopen(output_path, "w");
    if (!out) return errno ? errno : -1;

    /* Optional: dump TCC stub nodes for debugging wiring. */
    if (root && root->nodes && root->node_count > 0) {
        const char* dump = getenv("CC_DUMP_TCC_STUB_AST");
        if (dump && dump[0] == '1') {
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
            } CCASTStubNodeView;
            const CCASTStubNodeView* n = (const CCASTStubNodeView*)root->nodes;
            fprintf(stderr, "[cc] stub ast nodes: %d\n", root->node_count);
            int max_dump = root->node_count;
            if (max_dump > 4000) max_dump = 4000;
            for (int i = 0; i < max_dump; i++) {
                fprintf(stderr,
                        "  [%d] kind=%d parent=%d file=%s lines=%d..%d aux1=%d aux2=%d aux_s1=%s aux_s2=%s\n",
                        i,
                        n[i].kind,
                        n[i].parent,
                        n[i].file ? n[i].file : "<null>",
                        n[i].line_start,
                        n[i].line_end,
                        n[i].aux1,
                        n[i].aux2,
                        n[i].aux_s1 ? n[i].aux_s1 : "<null>",
                        n[i].aux_s2 ? n[i].aux_s2 : "<null>");
            }
            if (max_dump != root->node_count)
                fprintf(stderr, "  ... truncated (%d total)\n", root->node_count);
        }
    }

    /* For final codegen we read the original source and lower UFCS/@arena here.
       The preprocessor's temp file exists only to make TCC parsing succeed. */
    /* Read original source once; we may rewrite UFCS spans before @arena lowering. */
    char* src_all = NULL;
    size_t src_len = 0;
    if (ctx->input_path) {
#ifdef CC_TCC_EXT_AVAILABLE
        cc__read_entire_file(ctx->input_path, &src_all, &src_len);
#else
        cc__read_entire_file(ctx->input_path, &src_all, &src_len);
#endif
    }

    char* src_ufcs = src_all;
    size_t src_ufcs_len = src_len;

#ifdef CC_TCC_EXT_AVAILABLE
    if (src_all && root && root->nodes && root->node_count > 0) {
        char* rewritten = NULL;
        size_t rewritten_len = 0;
        if (cc__rewrite_ufcs_spans_with_nodes(root, ctx, src_all, src_len, &rewritten, &rewritten_len)) {
            src_ufcs = rewritten;
            src_ufcs_len = rewritten_len;
        }
    }
#endif

    /* Rewrite closure calls anywhere (including nested + multiline) using stub CALL nodes. */
#ifdef CC_TCC_EXT_AVAILABLE
    char* src_calls = NULL;
    size_t src_calls_len = 0;
    if (src_ufcs && root && root->nodes && root->node_count > 0) {
        if (cc__rewrite_all_closure_calls_with_nodes(root, ctx, src_ufcs, src_ufcs_len, &src_calls, &src_calls_len)) {
            if (src_ufcs != src_all) free(src_ufcs);
            src_ufcs = src_calls;
            src_ufcs_len = src_calls_len;
        }
    }
#endif

    /* Auto-blocking (first cut): inside @async functions, wrap statement-form calls to known
       non-@async/non-@noblock functions in cc_run_blocking_closure0(() => { ... }). */
#ifdef CC_TCC_EXT_AVAILABLE
    if (src_ufcs && root && root->nodes && root->node_count > 0 && ctx->symbols) {
        char* rewritten = NULL;
        size_t rewritten_len = 0;
        if (cc__rewrite_autoblocking_calls_with_nodes(root, ctx, src_ufcs, src_ufcs_len, &rewritten, &rewritten_len)) {
            if (src_ufcs != src_all) free(src_ufcs);
            src_ufcs = rewritten;
            src_ufcs_len = rewritten_len;
        }
    }
#endif

    /* Normalize `await <expr>` used inside larger expressions into temp hoists so the
       text-based async state machine can lower it (AST-driven span rewrite). */
#ifdef CC_TCC_EXT_AVAILABLE
    if (src_ufcs && root && root->nodes && root->node_count > 0) {
        char* rewritten = NULL;
        size_t rewritten_len = 0;
        if (cc__rewrite_await_exprs_with_nodes(root, ctx, src_ufcs, src_ufcs_len, &rewritten, &rewritten_len)) {
            if (src_ufcs != src_all) free(src_ufcs);
            src_ufcs = rewritten;
            src_ufcs_len = rewritten_len;
        }
        if (getenv("CC_DEBUG_AWAIT_REWRITE") && src_ufcs) {
            const char* needle = "@async int f";
            const char* p = strstr(src_ufcs, needle);
            if (!p) p = strstr(src_ufcs, "@async");
            if (p) {
                fprintf(stderr, "CC_DEBUG_AWAIT_REWRITE: ---- snippet ----\n");
                size_t off = (size_t)(p - src_ufcs);
                size_t take = 800;
                if (off + take > src_ufcs_len) take = src_ufcs_len - off;
                fwrite(p, 1, take, stderr);
                fprintf(stderr, "\nCC_DEBUG_AWAIT_REWRITE: ---- end ----\n");
            }
        }
    }
#endif

    /* AST-driven @async lowering (state machine).
       IMPORTANT: earlier rewrites can introduce new statements (auto-blocking temps, await-hoists, etc).
       We re-parse the rewritten TU with patched TCC to get an updated stub-AST before lowering async. */
    if (src_ufcs && ctx && ctx->symbols) {
        char* tmp_path = cc__write_temp_c_file(src_ufcs, src_ufcs_len, ctx->input_path);
        char pp_path[128];
        int pp_err = tmp_path ? cc_preprocess_file(tmp_path, pp_path, sizeof(pp_path)) : EINVAL;
        const char* use_path = (pp_err == 0) ? pp_path : tmp_path;
        if (getenv("CC_DEBUG_REPARSE")) {
            fprintf(stderr, "CC: reparse: tmp=%s pp=%s pp_err=%d use=%s\n",
                    tmp_path ? tmp_path : "<null>",
                    (pp_err == 0) ? pp_path : "<n/a>",
                    pp_err,
                    use_path ? use_path : "<null>");
        }
        CCASTRoot* root2 = use_path ? cc_tcc_bridge_parse_to_ast(use_path, ctx->input_path, ctx->symbols) : NULL;
        if (!root2) {
            if (tmp_path) {
                if (!getenv("CC_KEEP_REPARSE")) unlink(tmp_path);
                free(tmp_path);
            }
            fclose(out);
            if (src_ufcs != src_all) free(src_ufcs);
            free(src_all);
            return EINVAL;
        }
        if (pp_err == 0) root2->lowered_is_temp = 1;
        if (getenv("CC_DEBUG_REPARSE")) {
            fprintf(stderr, "CC: reparse: stub ast node_count=%d\n", root2->node_count);
        }

        char* rewritten = NULL;
        size_t rewritten_len = 0;
        int ar = cc_async_rewrite_state_machine_ast(root2, ctx, src_ufcs, src_ufcs_len, &rewritten, &rewritten_len);
        cc_tcc_bridge_free_ast(root2);
        if (tmp_path) {
            if (!getenv("CC_KEEP_REPARSE")) unlink(tmp_path);
            free(tmp_path);
        }
        if (pp_err == 0 && !(getenv("CC_KEEP_REPARSE"))) {
            unlink(pp_path);
        }
        if (ar < 0) {
            fclose(out);
            if (src_ufcs != src_all) free(src_ufcs);
            free(src_all);
            return EINVAL;
        }
        if (ar > 0) {
            if (src_ufcs != src_all) free(src_ufcs);
            src_ufcs = rewritten;
            src_ufcs_len = rewritten_len;
        }
    }

    /* Strip CC decl markers so output is valid C (run after async lowering so it can see `@async`). */
    if (src_ufcs) {
        char* stripped = NULL;
        size_t stripped_len = 0;
        if (cc__strip_cc_decl_markers(src_ufcs, src_ufcs_len, &stripped, &stripped_len)) {
            if (src_ufcs != src_all) free(src_ufcs);
            src_ufcs = stripped;
            src_ufcs_len = stripped_len;
        }
    }

    /* NOTE: slice move/provenance checking is now handled by the stub-AST checker pass
       (`cc/src/visitor/checker.c`) before visitor lowering. */

    fprintf(out, "/* CC visitor: passthrough of lowered C (preprocess + TCC parse) */\n");
    fprintf(out, "#include <stdlib.h>\n");
    fprintf(out, "#include <stdint.h>\n");
    fprintf(out, "#include \"cc_nursery.cch\"\n");
    fprintf(out, "#include \"cc_closure.cch\"\n");
    fprintf(out, "#include \"cc_slice.cch\"\n");
    fprintf(out, "#include \"cc_runtime.cch\"\n");
    fprintf(out, "#include \"std/task_intptr.cch\"\n");
    /* Helper alias: used for auto-blocking arg binding to avoid accidental hoisting of these temps. */
    fprintf(out, "typedef intptr_t CCAbIntptr;\n");
    /* Spawn thunks are emitted later (after parsing source) as static fns in this TU. */
    fprintf(out, "\n");
    fprintf(out, "/* --- CC spawn lowering helpers (best-effort) --- */\n");
    fprintf(out, "typedef struct { void (*fn)(void); } __cc_spawn_void_arg;\n");
    fprintf(out, "static void* __cc_spawn_thunk_void(void* p) {\n");
    fprintf(out, "  __cc_spawn_void_arg* a = (__cc_spawn_void_arg*)p;\n");
    fprintf(out, "  if (a && a->fn) a->fn();\n");
    fprintf(out, "  free(a);\n");
    fprintf(out, "  return NULL;\n");
    fprintf(out, "}\n");
    fprintf(out, "typedef struct { void (*fn)(int); int arg; } __cc_spawn_int_arg;\n");
    fprintf(out, "static void* __cc_spawn_thunk_int(void* p) {\n");
    fprintf(out, "  __cc_spawn_int_arg* a = (__cc_spawn_int_arg*)p;\n");
    fprintf(out, "  if (a && a->fn) a->fn(a->arg);\n");
    fprintf(out, "  free(a);\n");
    fprintf(out, "  return NULL;\n");
    fprintf(out, "}\n");
    fprintf(out, "/* --- end spawn helpers --- */\n\n");

    /* Pre-scan for spawn closures so we can emit valid top-level thunk defs. */
    CCClosureDesc* closure_descs = NULL;
    int closure_count = 0;
    int* closure_line_map = NULL; /* 1-based line -> (index+1) */
    int closure_line_cap = 0;
    char* closure_protos = NULL;
    size_t closure_protos_len = 0;
    char* closure_defs = NULL;
    size_t closure_defs_len = 0;
    if (src_ufcs) {
        int closure_next_id = 1;
        cc__scan_spawn_closures(src_ufcs, src_ufcs_len, src_path,
                                1, &closure_next_id,
                                &closure_descs, &closure_count,
                                &closure_line_map, &closure_line_cap,
                                &closure_protos, &closure_protos_len,
                                &closure_defs, &closure_defs_len);
    }

    /* Capture type check (best-effort):
       We can only lower captures when we can infer a file-scope-safe type string for each captured name. */
    if (closure_descs) {
        for (int i = 0; i < closure_count; i++) {
            for (int ci = 0; ci < closure_descs[i].cap_count; ci++) {
                if (closure_descs[i].cap_types && closure_descs[i].cap_types[ci]) continue;
                int col1 = closure_descs[i].start_col >= 0 ? (closure_descs[i].start_col + 1) : 1;
                fprintf(stderr,
                        "%s:%d:%d: error: CC: cannot infer type for captured name '%s' (capture-by-copy currently supports simple decls like 'int x = ...;' or 'T* p = ...;')\n",
                        src_path,
                        closure_descs[i].start_line,
                        col1,
                        closure_descs[i].cap_names && closure_descs[i].cap_names[ci] ? closure_descs[i].cap_names[ci] : "?");
                fclose(out);
                for (int k = 0; k < closure_count; k++) {
                    for (int j = 0; j < closure_descs[k].cap_count; j++) free(closure_descs[k].cap_names[j]);
                    free(closure_descs[k].cap_names);
                    for (int j = 0; j < closure_descs[k].cap_count; j++) free(closure_descs[k].cap_types ? closure_descs[k].cap_types[j] : NULL);
                    free(closure_descs[k].cap_types);
                    free(closure_descs[k].param1_name);
                    free(closure_descs[k].param0_name);
                    free(closure_descs[k].param0_type);
                    free(closure_descs[k].param1_type);
                    free(closure_descs[k].body);
                }
                free(closure_descs);
                free(closure_line_map);
                free(closure_protos);
                free(closure_defs);
                if (src_ufcs != src_all) free(src_ufcs);
                free(src_all);
                return EINVAL;
            }
        }
    }

    /* Captures are lowered via __cc_closure_make_N factories. */
    if (closure_protos && closure_protos_len > 0) {
        fputs("/* --- CC closure forward decls --- */\n", out);
        fwrite(closure_protos, 1, closure_protos_len, out);
        fputs("/* --- end closure forward decls --- */\n\n", out);
    }

    /* Preserve diagnostics mapping to the original input where possible. */
    fprintf(out, "#line 1 \"%s\"\n", src_path);

    if (src_ufcs) {
        FILE* in = fmemopen(src_ufcs, src_ufcs_len, "r");
        if (!in) {
            /* Fallback to old path */
            in = fopen(ctx->input_path, "r");
        }
        /* Map of multiline UFCS call spans: start_line -> end_line (inclusive). */
        int* ufcs_ml_end = NULL;
        int ufcs_ml_cap = 0;
        unsigned char* ufcs_single = NULL;
        int ufcs_single_cap = 0;
        /* Multiline spawn stmt spans: start_line -> end_line (inclusive). */
        int* spawn_ml_end = NULL;
        int spawn_ml_cap = 0;
        /* Spawn arg count by stmt start line (from stub AST direct children). */
        unsigned char* spawn_argc = NULL;
        int spawn_argc_cap = 0;
        if (root && root->nodes && root->node_count > 0) {
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
            int max_start = 0;
            int max_spawn = 0;
            for (int i = 0; i < root->node_count; i++) {
                if (n[i].kind != 5) continue;          /* CALL */
                int is_ufcs = (n[i].aux2 & 2) != 0;
                if (!is_ufcs) continue;                /* only UFCS-marked calls */
                if (!n[i].aux_s1) continue;
                if (!cc__node_file_matches_this_tu(root, ctx, n[i].file)) continue;
                if (n[i].line_end > n[i].line_start && n[i].line_start > max_start)
                    max_start = n[i].line_start;
                if (n[i].line_start > ufcs_single_cap)
                    ufcs_single_cap = n[i].line_start;
            }
            for (int i = 0; i < root->node_count; i++) {
                if (!cc__node_file_matches_this_tu(root, ctx, n[i].file)) continue;
                if (n[i].kind == 3 && n[i].aux_s1 && strcmp(n[i].aux_s1, "spawn") == 0) {
                    if (n[i].line_end > n[i].line_start && n[i].line_start > max_spawn)
                        max_spawn = n[i].line_start;
                }
            }
            if (max_start > 0) {
                ufcs_ml_cap = max_start + 1;
                ufcs_ml_end = (int*)calloc((size_t)ufcs_ml_cap, sizeof(int));
                if (ufcs_ml_end) {
                    for (int i = 0; i < root->node_count; i++) {
                        if (n[i].kind != 5) continue;
                        int is_ufcs = (n[i].aux2 & 2) != 0;
                        if (!is_ufcs) continue;
                        if (!n[i].aux_s1) continue;
                        if (!cc__node_file_matches_this_tu(root, ctx, n[i].file)) continue;
                        if (n[i].line_end > n[i].line_start &&
                            n[i].line_start > 0 &&
                            n[i].line_start < ufcs_ml_cap) {
                            int st = n[i].line_start;
                            if (n[i].line_end > ufcs_ml_end[st])
                                ufcs_ml_end[st] = n[i].line_end;
                        }
                    }
                }
            }
            if (ufcs_single_cap > 0) {
                ufcs_single = (unsigned char*)calloc((size_t)ufcs_single_cap + 1, 1);
                if (ufcs_single) {
                    for (int i = 0; i < root->node_count; i++) {
                        if (n[i].kind != 5) continue;
                        int is_ufcs = (n[i].aux2 & 2) != 0;
                        if (!is_ufcs) continue;
                        if (!n[i].aux_s1) continue;
                        if (!cc__node_file_matches_this_tu(root, ctx, n[i].file)) continue;
                        if (n[i].line_start > 0 && n[i].line_start <= ufcs_single_cap)
                            ufcs_single[n[i].line_start] = 1;
                    }
                }
            }

            if (max_spawn > 0) {
                spawn_ml_cap = max_spawn + 1;
                spawn_ml_end = (int*)calloc((size_t)spawn_ml_cap, sizeof(int));
                if (spawn_ml_end) {
                    for (int i = 0; i < root->node_count; i++) {
                        if (!cc__node_file_matches_this_tu(root, ctx, n[i].file)) continue;
                        if (n[i].kind != 3) continue;
                        if (!n[i].aux_s1 || strcmp(n[i].aux_s1, "spawn") != 0) continue;
                        if (n[i].line_end > n[i].line_start &&
                            n[i].line_start > 0 &&
                            n[i].line_start < spawn_ml_cap) {
                            if (n[i].line_end > spawn_ml_end[n[i].line_start])
                                spawn_ml_end[n[i].line_start] = n[i].line_end;
                        }
                    }
                }

                spawn_argc_cap = spawn_ml_cap;
                spawn_argc = (unsigned char*)calloc((size_t)spawn_argc_cap, 1);
                if (spawn_argc) {
                    for (int si = 0; si < root->node_count; si++) {
                        if (!cc__node_file_matches_this_tu(root, ctx, n[si].file)) continue;
                        if (n[si].kind != 3) continue;
                        if (!n[si].aux_s1 || strcmp(n[si].aux_s1, "spawn") != 0) continue;
                        int ls = n[si].line_start;
                        if (ls <= 0 || ls >= spawn_argc_cap) continue;
                        int argc = 0;
                        for (int j = 0; j < root->node_count; j++) {
                            if (n[j].parent != si) continue;
                            if (!cc__node_file_matches_this_tu(root, ctx, n[j].file)) continue;
                            argc++;
                        }
                        if (argc < 0) argc = 0;
                        if (argc > 255) argc = 255;
                        spawn_argc[ls] = (unsigned char)argc;
                    }
                }
            }

        }

        int arena_stack[128];
        int arena_top = -1;
        int arena_counter = 0;
        int nursery_depth_stack[128];
        int nursery_id_stack[128];
        int nursery_top = -1;
        int nursery_counter = 0;

        /* Basic scope tracking for @defer. This is a line-based best-effort implementation:
           - @defer stmt; registers stmt to run before the closing brace of the current scope.
           - @defer name: stmt; registers a named defer.
           - cancel name; disables a named defer.
           This does NOT support cross-line defers robustly yet, but unblocks correct-ish flow. */
        typedef struct {
            int depth;
            int active;
            int line_no;
            char name[64];   /* empty = unnamed */
            char stmt[512];  /* original stmt suffix */
        } CCDeferItem;
        CCDeferItem defers[512];
        int defer_count = 0;

        /* Track local decls (best-effort) so we can recognize CCClosure1 variables for call lowering. */
        char** decl_names[256];
        char** decl_types[256];
        unsigned char* decl_flags[256];
        int decl_counts[256];
        for (int i = 0; i < 256; i++) {
            decl_names[i] = NULL;
            decl_types[i] = NULL;
            decl_flags[i] = NULL;
            decl_counts[i] = 0;
        }

        int brace_depth = 0;
        /* nursery id stack is used for spawn lowering */
        int src_line_no = 0;
        const int cc_line_cap = 65536;
        const int cc_rewritten_cap = 131072;
        char* line = (char*)malloc((size_t)cc_line_cap);
        char* rewritten = (char*)malloc((size_t)cc_rewritten_cap);
        if (!line || !rewritten) {
            free(line);
            free(rewritten);
            fclose(in);
            fclose(out);
            if (src_ufcs != src_all) free(src_ufcs);
            free(src_all);
            return ENOMEM;
        }
        while (fgets(line, cc_line_cap, in)) {
            src_line_no++;
            char *p = line;
            while (*p == ' ' || *p == '\t') p++;

            /* Track decls before any rewriting so we can later lower `c(arg);` for CCClosure1 vars. */
            cc__maybe_record_decl(decl_names, decl_types, decl_flags, decl_counts, brace_depth, line);

            /* Multiline spawn lowering (buffer by stub span) */
            if (spawn_ml_end && src_line_no > 0 && src_line_no < spawn_ml_cap && spawn_ml_end[src_line_no] > src_line_no) {
                int is_closure_literal_spawn = (closure_line_map &&
                                               src_line_no > 0 &&
                                               src_line_no < closure_line_cap &&
                                               closure_line_map[src_line_no] > 0);
                if (!is_closure_literal_spawn) {
                int start_line = src_line_no;
                int end_line = spawn_ml_end[src_line_no];
                int expected_argc = (spawn_argc && start_line > 0 && start_line < spawn_argc_cap) ? (int)spawn_argc[start_line] : 0;
                size_t buf_cap = 1024, buf_len = 0;
                char* buf = (char*)malloc(buf_cap);
                if (!buf) { /* fallthrough */ }
                else {
                    buf[0] = '\0';
                    size_t ll = strnlen(line, (size_t)cc_line_cap);
                    if (buf_len + ll + 1 > buf_cap) { while (buf_len + ll + 1 > buf_cap) buf_cap *= 2; buf = (char*)realloc(buf, buf_cap); }
                    if (!buf) { /* fallthrough */ }
                    else {
                        memcpy(buf + buf_len, line, ll); buf_len += ll; buf[buf_len] = '\0';
                        while (src_line_no < end_line && fgets(line, cc_line_cap, in)) {
                            src_line_no++;
                            ll = strnlen(line, (size_t)cc_line_cap);
                            if (buf_len + ll + 1 > buf_cap) { while (buf_len + ll + 1 > buf_cap) buf_cap *= 2; buf = (char*)realloc(buf, buf_cap); if (!buf) break; }
                            if (!buf) break;
                            memcpy(buf + buf_len, line, ll); buf_len += ll; buf[buf_len] = '\0';
                        }
                        if (buf) {
                            /* Reuse the existing single-line spawn parser but on the buffered chunk. */
                            char* pp = buf;
                            while (*pp == ' ' || *pp == '\t') pp++;
                            if (strncmp(pp, "spawn", 5) == 0 && (pp[5] == ' ' || pp[5] == '\t')) {
                                int cur_nursery_id = (nursery_top >= 0) ? nursery_id_stack[nursery_top] : 0;
                                const char* s0 = pp + 5;
                                while (*s0 == ' ' || *s0 == '\t') s0++;
                                if (*s0 == '(') {
                                    s0++;
                                    /* Find matching ')' at depth. */
                                    const char* expr_start = s0;
                                    const char* p2 = expr_start;
                                    int par = 0, brk = 0, br = 0;
                                    int ins = 0;
                                    char qch = 0;
                                    while (*p2) {
                                        char ch = *p2;
                                        if (ins) {
                                            if (ch == '\\' && p2[1]) { p2 += 2; continue; }
                                            if (ch == qch) ins = 0;
                                            p2++;
                                            continue;
                                        }
                                        if (ch == '"' || ch == '\'') { ins = 1; qch = ch; p2++; continue; }
                                        if (ch == '(') par++;
                                        else if (ch == ')') { if (par == 0 && brk == 0 && br == 0) break; par--; }
                                        else if (ch == '[') brk++;
                                        else if (ch == ']') { if (brk) brk--; }
                                        else if (ch == '{') br++;
                                        else if (ch == '}') { if (br) br--; }
                                        p2++;
                                    }
                                    if (*p2 == ')') {
                                        const char* expr_end = p2;
                                        while (expr_end > expr_start && (expr_end[-1] == ' ' || expr_end[-1] == '\t' || expr_end[-1] == '\n' || expr_end[-1] == '\r')) expr_end--;
                                        size_t expr_len = (size_t)(expr_end - expr_start);
                                        /* top-level comma split */
                                        int comma_pos[2] = { -1, -1 };
                                        int comma_n = 0;
                                        int dpar = 0, dbrk2 = 0, dbr2 = 0;
                                        int ins2 = 0; char q2 = 0;
                                        for (size_t ii = 0; ii < expr_len; ii++) {
                                            char ch = expr_start[ii];
                                            if (ins2) {
                                                if (ch == '\\' && ii + 1 < expr_len) { ii++; continue; }
                                                if (ch == q2) ins2 = 0;
                                                continue;
                                            }
                                            if (ch == '"' || ch == '\'') { ins2 = 1; q2 = ch; continue; }
                                            if (ch == '(') dpar++;
                                            else if (ch == ')') { if (dpar) dpar--; }
                                            else if (ch == '[') dbrk2++;
                                            else if (ch == ']') { if (dbrk2) dbrk2--; }
                                            else if (ch == '{') dbr2++;
                                            else if (ch == '}') { if (dbr2) dbr2--; }
                                            else if (ch == ',' && dpar == 0 && dbrk2 == 0 && dbr2 == 0) {
                                                if (comma_n < 2) comma_pos[comma_n++] = (int)ii;
                                            }
                                        }
                                        if (cur_nursery_id != 0) {
                                            int argc = expected_argc ? expected_argc : (comma_n + 1);
                                            if (argc < 1) argc = 1;
                                            if (argc > 3) argc = 3;

                                            if (argc == 1 && comma_n == 0) {
                                                const char* c0 = expr_start;
                                                size_t c0_len = expr_len;
                                                while (c0_len > 0 && (c0[c0_len - 1] == ' ' || c0[c0_len - 1] == '\t')) c0_len--;
                                                while (c0_len > 0 && (*c0 == ' ' || *c0 == '\t')) { c0++; c0_len--; }
                                                fprintf(out, "#line %d \"%s\"\n", start_line, src_path);
                                                fprintf(out, "{ CCClosure0 __c = %.*s; cc_nursery_spawn_closure0(__cc_nursery%d, __c); }\n",
                                                        (int)c0_len, c0, cur_nursery_id);
                                                free(buf);
                                                fprintf(out, "#line %d \"%s\"\n", src_line_no + 1, src_path);
                                                continue;
                                            }
                                            if (argc == 2 && comma_n >= 1) {
                                                const char* c0 = expr_start;
                                                const char* c1 = expr_start + comma_pos[0] + 1;
                                                size_t c0_len = (size_t)comma_pos[0];
                                                size_t c1_len = expr_len - (size_t)comma_pos[0] - 1;
                                                while (c0_len > 0 && (c0[c0_len - 1] == ' ' || c0[c0_len - 1] == '\t')) c0_len--;
                                                while (c1_len > 0 && (*c1 == ' ' || *c1 == '\t')) { c1++; c1_len--; }
                                                while (c1_len > 0 && (c1[c1_len - 1] == ' ' || c1[c1_len - 1] == '\t')) c1_len--;
                                                fprintf(out, "#line %d \"%s\"\n", start_line, src_path);
                                                fprintf(out, "{ CCClosure1 __c = %.*s; cc_nursery_spawn_closure1(__cc_nursery%d, __c, (intptr_t)(%.*s)); }\n",
                                                        (int)c0_len, c0, cur_nursery_id, (int)c1_len, c1);
                                                free(buf);
                                                fprintf(out, "#line %d \"%s\"\n", src_line_no + 1, src_path);
                                                continue;
                                            }
                                            if (argc == 3 && comma_n >= 2) {
                                                const char* c0 = expr_start;
                                                const char* c1 = expr_start + comma_pos[0] + 1;
                                                const char* c2 = expr_start + comma_pos[1] + 1;
                                                size_t c0_len = (size_t)comma_pos[0];
                                                size_t c1_len = (size_t)(comma_pos[1] - comma_pos[0] - 1);
                                                size_t c2_len = expr_len - (size_t)comma_pos[1] - 1;
                                                while (c0_len > 0 && (c0[c0_len - 1] == ' ' || c0[c0_len - 1] == '\t')) c0_len--;
                                                while (c1_len > 0 && (*c1 == ' ' || *c1 == '\t')) { c1++; c1_len--; }
                                                while (c1_len > 0 && (c1[c1_len - 1] == ' ' || c1[c1_len - 1] == '\t')) c1_len--;
                                                while (c2_len > 0 && (*c2 == ' ' || *c2 == '\t')) { c2++; c2_len--; }
                                                while (c2_len > 0 && (c2[c2_len - 1] == ' ' || c2[c2_len - 1] == '\t')) c2_len--;
                                                fprintf(out, "#line %d \"%s\"\n", start_line, src_path);
                                                fprintf(out, "{ CCClosure2 __c = %.*s; cc_nursery_spawn_closure2(__cc_nursery%d, __c, (intptr_t)(%.*s), (intptr_t)(%.*s)); }\n",
                                                        (int)c0_len, c0, cur_nursery_id,
                                                        (int)c1_len, c1,
                                                        (int)c2_len, c2);
                                                free(buf);
                                                fprintf(out, "#line %d \"%s\"\n", src_line_no + 1, src_path);
                                                continue;
                                            }
                                        }
                                    }
                                }
                            }
                            /* Fallback: just emit buffered chunk */
                            fprintf(out, "#line %d \"%s\"\n", start_line, src_path);
                            fwrite(buf, 1, buf_len, out);
                            free(buf);
                            fprintf(out, "#line %d \"%s\"\n", src_line_no + 1, src_path);
                            continue;
                        }
                    }
                }
                }
            }

            /* cancel <name>; */
            if (strncmp(p, "cancel", 6) == 0 && (p[6] == ' ' || p[6] == '\t')) {
                char nm[64] = {0};
                if (sscanf(p + 6, " %63[^; \t\r\n]", nm) == 1) {
                    for (int i = defer_count - 1; i >= 0; i--) {
                        if (defers[i].active && defers[i].name[0] && strcmp(defers[i].name, nm) == 0) {
                            defers[i].active = 0;
                            break;
                        }
                    }
                }
                fprintf(out, "#line %d \"%s\"\n", src_line_no, src_path);
                fprintf(out, "/* TODO: cancel %s; */\n", nm[0] ? nm : "<unknown>");
                continue;
            }

            /* Lower @arena syntax marker into a plain C block. The preprocessor already injected
               the arena binding/free lines inside the block. */
            if (strncmp(p, "@arena", 6) == 0) {
                const char* name_tok = "arena";
                const char* size_tok = "kilobytes(4)";
#ifdef CC_TCC_EXT_AVAILABLE
                const char* rec_name = NULL;
                const char* rec_size = NULL;
                /* Try matching arena node against either input_path or lowered_path. */
                if (cc__arena_args_for_line(root, ctx->input_path, src_line_no, &rec_name, &rec_size) ||
                    (root && root->lowered_path &&
                     cc__arena_args_for_line(root, root->lowered_path, src_line_no, &rec_name, &rec_size))) {
                    if (rec_name && rec_name[0]) name_tok = rec_name;
                    if (rec_size && rec_size[0]) size_tok = rec_size;
                }
#endif

                size_t indent_len = (size_t)(p - line);
                char indent[256];
                if (indent_len >= sizeof(indent)) indent_len = sizeof(indent) - 1;
                memcpy(indent, line, indent_len);
                indent[indent_len] = '\0';
                int id = ++arena_counter;
                if (arena_top + 1 < (int)(sizeof(arena_stack) / sizeof(arena_stack[0])))
                    arena_stack[++arena_top] = id;

                /* Map generated prologue to the @arena source line for better diagnostics. */
                fprintf(out, "#line %d \"%s\"\n", src_line_no, src_path);
                fprintf(out, "%s{\n", indent);
                fprintf(out, "%s  CCArena __cc_arena%d = cc_heap_arena(%s);\n", indent, id, size_tok);
                fprintf(out, "%s  CCArena* %s = &__cc_arena%d;\n", indent, name_tok, id);
                brace_depth++; /* we emitted an opening brace */
                fprintf(out, "#line %d \"%s\"\n", src_line_no + 1, src_path);
                continue;
            }

            /* @defer [name:] stmt; */
            if (strncmp(p, "@defer", 6) == 0 && (p[6] == ' ' || p[6] == '\t')) {
                char* rest = p + 6;
                while (*rest == ' ' || *rest == '\t') rest++;
                /* Parse optional name: */
                char nm[64] = {0};
                const char* stmt = rest;
                const char* colon = strchr(rest, ':');
                if (colon) {
                    /* treat as name: if name token is identifier-ish and ':' precedes a space */
                    size_t nlen = (size_t)(colon - rest);
                    if (nlen > 0 && nlen < sizeof(nm)) {
                        int ok = 1;
                        for (size_t i = 0; i < nlen; i++) {
                            char c = rest[i];
                            if (!(isalnum((unsigned char)c) || c == '_')) { ok = 0; break; }
                        }
                        if (ok) {
                            memcpy(nm, rest, nlen);
                            nm[nlen] = '\0';
                            stmt = colon + 1;
                            while (*stmt == ' ' || *stmt == '\t') stmt++;
                        }
                    }
                }
                if (defer_count < (int)(sizeof(defers) / sizeof(defers[0]))) {
                    CCDeferItem* d = &defers[defer_count++];
                    d->depth = brace_depth;
                    d->active = 1;
                    d->line_no = src_line_no;
                    d->name[0] = '\0';
                    if (nm[0]) strncpy(d->name, nm, sizeof(d->name) - 1);
                    d->stmt[0] = '\0';
                    strncpy(d->stmt, stmt, sizeof(d->stmt) - 1);
                }
                fprintf(out, "#line %d \"%s\"\n", src_line_no, src_path);
                fprintf(out, "/* @defer recorded */\n");
                continue;
            }

            /* Lower @nursery marker into a runtime nursery scope. */
            if (strncmp(p, "@nursery", 8) == 0 && (p[8] == ' ' || p[8] == '\t' || p[8] == '\n' || p[8] == '\r' || p[8] == '{')) {
                size_t indent_len = (size_t)(p - line);
                char indent[256];
                if (indent_len >= sizeof(indent)) indent_len = sizeof(indent) - 1;
                memcpy(indent, line, indent_len);
                indent[indent_len] = '\0';
                int id = ++nursery_counter;
                if (nursery_top + 1 < (int)(sizeof(nursery_id_stack) / sizeof(nursery_id_stack[0]))) {
                    nursery_id_stack[++nursery_top] = id;
                    /* Will be set after we account for the '{' we emit below. */
                    nursery_depth_stack[nursery_top] = 0;
                }
                fprintf(out, "#line %d \"%s\"\n", src_line_no, src_path);
                /* Declare nursery in the surrounding scope, then emit a plain C block for the nursery body.
                   This keeps the nursery pointer in-scope even if epilogues are emitted later (best-effort). */
                fprintf(out, "%sCCNursery* __cc_nursery%d = cc_nursery_create();\n", indent, id);
                fprintf(out, "%sif (!__cc_nursery%d) abort();\n", indent, id);
                fprintf(out, "%s{\n", indent);
                brace_depth++; /* account for the '{' we emitted */
                if (nursery_top >= 0) nursery_depth_stack[nursery_top] = brace_depth;
                fprintf(out, "#line %d \"%s\"\n", src_line_no + 1, src_path);
                continue;
            }

            /* Lower spawn(...) inside a nursery to cc_nursery_spawn. Supports:
               - spawn (fn());
               - spawn (fn(<int literal>));
               Otherwise falls back to a plain call with a TODO. */
            if (strncmp(p, "spawn", 5) == 0 && (p[5] == ' ' || p[5] == '\t')) {
                int cur_nursery_id = (nursery_top >= 0) ? nursery_id_stack[nursery_top] : 0;
                const char* s0 = p + 5;
                while (*s0 == ' ' || *s0 == '\t') s0++;
                if (*s0 == '(') {
                    s0++;
                    while (*s0 == ' ' || *s0 == '\t') s0++;

                    /* Closure literal: spawn(() => { ... }); uses pre-scan + top-level thunks. */
                    if (closure_line_map && src_line_no > 0 && src_line_no < closure_line_cap) {
                        int idx1 = closure_line_map[src_line_no];
                        if (idx1 > 0 && idx1 <= closure_count) {
                            CCClosureDesc* cd = &closure_descs[idx1 - 1];
                            if (cd->param_count != 0) {
                                /* Not supported in spawn yet. Fall back to other spawn lowering paths. */
                            } else {
                            fprintf(out, "#line %d \"%s\"\n", src_line_no, src_path);
                            fprintf(out, "{\n");
                            fprintf(out, "  CCClosure0 __c = __cc_closure_make_%d(", cd->id);
                            if (cd->cap_count == 0) {
                                fprintf(out, ");\n");
                            } else {
                                for (int ci = 0; ci < cd->cap_count; ci++) {
                                    if (ci) fprintf(out, ", ");
                                    int mo = (cd->cap_flags && (cd->cap_flags[ci] & 2) != 0);
                                    if (mo) fprintf(out, "cc_move(");
                                    fprintf(out, "%s", cd->cap_names[ci] ? cd->cap_names[ci] : "0");
                                    if (mo) fprintf(out, ")");
                                }
                                fprintf(out, ");\n");
                            }
                            fprintf(out, "  cc_nursery_spawn_closure0(__cc_nursery%d, __c);\n", cur_nursery_id);
                            fprintf(out, "}\n");
                            /* Skip original closure text lines (multiline). */
                            while (src_line_no < cd->end_line && fgets(line, cc_line_cap, in)) {
                                src_line_no++;
                            }
                            /* Resync source mapping after eliding original closure text. */
                            fprintf(out, "#line %d \"%s\"\n", src_line_no + 1, src_path);
                            continue;
                            }
                        }
                    }

                    /* spawn(<closure_expr>); where the expression is a CCClosure0 value.
                       Best-effort heuristic: accept identifiers and cc_closure0_make(...). */
                    {
                        const char* expr_start = s0;
                        const char* p2 = expr_start;
                        int par = 0;
                        while (*p2) {
                            if (*p2 == '(') par++;
                            else if (*p2 == ')') {
                                if (par == 0) break;
                                par--;
                            }
                            p2++;
                        }
                        if (*p2 == ')') {
                            const char* expr_end = p2;
                            while (expr_end > expr_start && (expr_end[-1] == ' ' || expr_end[-1] == '\t')) expr_end--;
                            size_t expr_len = (size_t)(expr_end - expr_start);
                            /* Support spawn(c, arg) for CCClosure1 and spawn(c, a, b) for CCClosure2 (nursery only). */
                            {
                                /* Find top-level commas */
                                int comma_pos[2] = { -1, -1 };
                                int comma_n = 0;
                                int dpar = 0, dbrk = 0, dbr = 0;
                                int ins = 0;
                                char qch = 0;
                                for (size_t i = 0; i < expr_len; i++) {
                                    char ch = expr_start[i];
                                    if (ins) {
                                        if (ch == '\\' && i + 1 < expr_len) { i++; continue; }
                                        if (ch == qch) ins = 0;
                                        continue;
                                    }
                                    if (ch == '"' || ch == '\'') { ins = 1; qch = ch; continue; }
                                    if (ch == '(') dpar++;
                                    else if (ch == ')') { if (dpar) dpar--; }
                                    else if (ch == '[') dbrk++;
                                    else if (ch == ']') { if (dbrk) dbrk--; }
                                    else if (ch == '{') dbr++;
                                    else if (ch == '}') { if (dbr) dbr--; }
                                    else if (ch == ',' && dpar == 0 && dbrk == 0 && dbr == 0) {
                                        if (comma_n < 2) comma_pos[comma_n++] = (int)i;
                                    }
                                }

                                if (cur_nursery_id != 0 && (comma_n == 1 || comma_n == 2)) {
                                    const char* c0 = expr_start;
                                    const char* c1 = expr_start + comma_pos[0] + 1;
                                    const char* c2 = (comma_n == 2) ? (expr_start + comma_pos[1] + 1) : NULL;

                                    size_t c0_len = (size_t)comma_pos[0];
                                    size_t c1_len = (comma_n == 2) ? (size_t)(comma_pos[1] - comma_pos[0] - 1) : (expr_len - (size_t)comma_pos[0] - 1);
                                    size_t c2_len = (comma_n == 2) ? (expr_len - (size_t)comma_pos[1] - 1) : 0;

                                    while (c0_len > 0 && (c0[c0_len - 1] == ' ' || c0[c0_len - 1] == '\t')) c0_len--;
                                    while (c1_len > 0 && (*c1 == ' ' || *c1 == '\t')) { c1++; c1_len--; }
                                    while (c1_len > 0 && (c1[c1_len - 1] == ' ' || c1[c1_len - 1] == '\t')) c1_len--;
                                    if (c2) {
                                        while (c2_len > 0 && (*c2 == ' ' || *c2 == '\t')) { c2++; c2_len--; }
                                        while (c2_len > 0 && (c2[c2_len - 1] == ' ' || c2[c2_len - 1] == '\t')) c2_len--;
                                    }

                                    fprintf(out, "#line %d \"%s\"\n", src_line_no, src_path);
                                    if (comma_n == 1) {
                                        fprintf(out, "{ CCClosure1 __c = %.*s; cc_nursery_spawn_closure1(__cc_nursery%d, __c, (intptr_t)(%.*s)); }\n",
                                                (int)c0_len, c0, cur_nursery_id,
                                                (int)c1_len, c1);
                                    } else {
                                        fprintf(out, "{ CCClosure2 __c = %.*s; cc_nursery_spawn_closure2(__cc_nursery%d, __c, (intptr_t)(%.*s), (intptr_t)(%.*s)); }\n",
                                                (int)c0_len, c0, cur_nursery_id,
                                                (int)c1_len, c1,
                                                (int)c2_len, c2 ? c2 : "0");
                                    }
                                    continue;
                                }
                            }
                            int looks_ident = 0;
                            if (expr_len > 0 && (isalpha((unsigned char)expr_start[0]) || expr_start[0] == '_')) {
                                looks_ident = 1;
                                for (size_t i = 1; i < expr_len; i++) {
                                    char c = expr_start[i];
                                    if (!(isalnum((unsigned char)c) || c == '_')) { looks_ident = 0; break; }
                                }
                            }
                            int looks_make = 0;
                            if (expr_len >= 15) {
                                for (size_t i = 0; i + 15 <= expr_len; i++) {
                                    if (memcmp(expr_start + i, "cc_closure0_make", 15) == 0) { looks_make = 1; break; }
                                }
                            }
                            if (looks_ident || looks_make) {
                                fprintf(out, "#line %d \"%s\"\n", src_line_no, src_path);
                                if (cur_nursery_id == 0) {
                                    fprintf(out, "/* TODO: spawn outside nursery */ %s", line);
                                } else {
                                    fprintf(out, "{ CCClosure0 __c = %.*s; cc_nursery_spawn_closure0(__cc_nursery%d, __c); }\n",
                                            (int)expr_len, expr_start, cur_nursery_id);
                                }
                                continue;
                            }
                        }
                    }

                    char fn[64] = {0};
                    long arg = 0;
                    int has_arg = 0;
                    if (sscanf(s0, "%63[_A-Za-z0-9]%n", fn, &(int){0}) >= 1) {
                        const char* lp = strchr(s0, '(');
                        const char* rp = lp ? strchr(lp, ')') : NULL;
                        if (lp && rp && lp < rp) {
                            /* check for single integer literal inside */
                            const char* inside = lp + 1;
                            while (*inside == ' ' || *inside == '\t') inside++;
                            if (*inside == '-' || isdigit((unsigned char)*inside)) {
                                char* endp = NULL;
                                arg = strtol(inside, &endp, 10);
                                if (endp) {
                                    while (*endp == ' ' || *endp == '\t') endp++;
                                    if (*endp == ')' ) has_arg = 1;
                                }
                            }
                            /* no-arg case */
                            if (!has_arg) {
                                const char* inside2 = lp + 1;
                                while (*inside2 == ' ' || *inside2 == '\t') inside2++;
                                if (*inside2 == ')' ) has_arg = 0;
                            }
                        }
                    }

                    fprintf(out, "#line %d \"%s\"\n", src_line_no, src_path);
                    if (cur_nursery_id == 0) {
                        fprintf(out, "/* TODO: spawn outside nursery */ %s", line);
                        continue;
                    }
                    if (fn[0] && !has_arg) {
                        fprintf(out, "{ __cc_spawn_void_arg* __a = (__cc_spawn_void_arg*)malloc(sizeof(__cc_spawn_void_arg));\n");
                        fprintf(out, "  if (!__a) abort();\n");
                        fprintf(out, "  __a->fn = %s;\n", fn);
                        fprintf(out, "  cc_nursery_spawn(__cc_nursery%d, __cc_spawn_thunk_void, __a);\n", cur_nursery_id);
                        fprintf(out, "}\n");
                        continue;
                    }
                    if (fn[0] && has_arg) {
                        fprintf(out, "{ __cc_spawn_int_arg* __a = (__cc_spawn_int_arg*)malloc(sizeof(__cc_spawn_int_arg));\n");
                        fprintf(out, "  if (!__a) abort();\n");
                        fprintf(out, "  __a->fn = %s;\n", fn);
                        fprintf(out, "  __a->arg = (int)%ld;\n", arg);
                        fprintf(out, "  cc_nursery_spawn(__cc_nursery%d, __cc_spawn_thunk_int, __a);\n", cur_nursery_id);
                        fprintf(out, "}\n");
                        continue;
                    }
                    fprintf(out, "/* TODO: spawn lowering */ %s", line);
                    continue;
                }
            }

            /* Closure literal used as an expression:
               rewrite `() => { ... }` / `() => expr` to a CCClosure0 value. */
            if (closure_line_map && src_line_no > 0 && src_line_no < closure_line_cap) {
                int idx1 = closure_line_map[src_line_no];
                if (idx1 > 0 && idx1 <= closure_count) {
                    CCClosureDesc* cd = &closure_descs[idx1 - 1];
                    size_t line_len2 = strlen(line);
                    if (cd->start_col < 0 || (size_t)cd->start_col > line_len2) {
                        /* Unexpected; just pass through. */
                    } else {
                        fprintf(out, "#line %d \"%s\"\n", src_line_no, src_path);
                        fwrite(line, 1, (size_t)cd->start_col, out);
                        if (cd->cap_count > 0) {
                            fprintf(out, "__cc_closure_make_%d(", cd->id);
                            for (int ci = 0; ci < cd->cap_count; ci++) {
                                if (ci) fputs(", ", out);
                                int mo = (cd->cap_flags && (cd->cap_flags[ci] & 2) != 0);
                                if (mo) fputs("cc_move(", out);
                                fputs(cd->cap_names[ci] ? cd->cap_names[ci] : "0", out);
                                if (mo) fputs(")", out);
                            }
                            fputs(")", out);
                        } else {
                            fprintf(out, "__cc_closure_make_%d()", cd->id);
                        }

                        if (cd->end_line == src_line_no) {
                            if (cd->end_col >= 0 && (size_t)cd->end_col < line_len2) {
                                fputs(line + cd->end_col, out);
                            } else {
                                fputc('\n', out);
                            }
                            continue;
                        }

                        /* Multiline literal: skip until end_line, then emit suffix. */
                        fputc('\n', out);
                        while (src_line_no < cd->end_line && fgets(line, cc_line_cap, in)) {
                            src_line_no++;
                        }
                        line_len2 = strlen(line);
                        fprintf(out, "#line %d \"%s\"\n", src_line_no, src_path);
                        if (cd->end_col >= 0 && (size_t)cd->end_col < line_len2) {
                            fputs(line + cd->end_col, out);
                        } else {
                            fputc('\n', out);
                        }
                        fprintf(out, "#line %d \"%s\"\n", src_line_no + 1, src_path);
                        continue;
                    }
                }
            }
            if (arena_top >= 0 && p[0] == '}') {
                int id = arena_stack[arena_top--];
                size_t indent_len = (size_t)(p - line);
                char indent[256];
                if (indent_len >= sizeof(indent)) indent_len = sizeof(indent) - 1;
                memcpy(indent, line, indent_len);
                indent[indent_len] = '\0';
                /* Map generated epilogue to the closing brace line for diagnostics. */
                fprintf(out, "#line %d \"%s\"\n", src_line_no, src_path);
                fprintf(out, "%s  cc_heap_arena_free(&__cc_arena%d);\n", indent, id);
                fprintf(out, "#line %d \"%s\"\n", src_line_no, src_path);
            }

            /* Before emitting a close brace, emit any @defer statements at this depth. */
            if (p[0] == '}') {
                /* If this brace closes an active nursery scope, emit nursery epilogue inside the scope. */
                if (nursery_top >= 0 && nursery_depth_stack[nursery_top] == brace_depth) {
                    size_t indent_len = (size_t)(p - line);
                    char indent[256];
                    if (indent_len >= sizeof(indent)) indent_len = sizeof(indent) - 1;
                    memcpy(indent, line, indent_len);
                    indent[indent_len] = '\0';

                    int id = nursery_id_stack[nursery_top--];
                    fprintf(out, "#line %d \"%s\"\n", src_line_no, src_path);
                    fprintf(out, "%s  cc_nursery_wait(__cc_nursery%d);\n", indent, id);
                    fprintf(out, "%s  cc_nursery_free(__cc_nursery%d);\n", indent, id);
                    fprintf(out, "#line %d \"%s\"\n", src_line_no, src_path);
                }

                for (int i = defer_count - 1; i >= 0; i--) {
                    if (defers[i].active && defers[i].depth == brace_depth) {
                        fprintf(out, "#line %d \"%s\"\n", defers[i].line_no, src_path);
                        fprintf(out, "%s", defers[i].stmt);
                        /* Ensure newline */
                        size_t sl = strnlen(defers[i].stmt, sizeof(defers[i].stmt));
                        if (sl == 0 || defers[i].stmt[sl - 1] != '\n')
                            fprintf(out, "\n");
                        defers[i].active = 0;
                    }
                }
                /* The source brace closes the current depth. */
                if (brace_depth > 0) brace_depth--;
            }

            /* Update brace depth for opening braces on this line (best-effort). */
            for (char* q = line; *q; q++) {
                if (*q == '{') brace_depth++;
            }

            /* If this line starts a recorded multiline UFCS call, buffer until its end line and
               rewrite the whole chunk (handles multi-line argument lists). */
            if (ufcs_ml_end && src_line_no > 0 && src_line_no < ufcs_ml_cap && ufcs_ml_end[src_line_no] > src_line_no) {
                int end_line = ufcs_ml_end[src_line_no];
                size_t buf_cap = 1024;
                size_t buf_len = 0;
                char* buf = (char*)malloc(buf_cap);
                if (!buf) {
                    fputs(line, out);
                    continue;
                }
                buf[0] = '\0';
                size_t ll = strnlen(line, (size_t)cc_line_cap);
                if (buf_len + ll + 1 > buf_cap) {
                    while (buf_len + ll + 1 > buf_cap) buf_cap *= 2;
                    buf = (char*)realloc(buf, buf_cap);
                    if (!buf) continue;
                }
                memcpy(buf + buf_len, line, ll);
                buf_len += ll;
                buf[buf_len] = '\0';

                while (src_line_no < end_line && fgets(line, cc_line_cap, in)) {
                    src_line_no++;
                    ll = strnlen(line, (size_t)cc_line_cap);
                    if (buf_len + ll + 1 > buf_cap) {
                        while (buf_len + ll + 1 > buf_cap) buf_cap *= 2;
                        buf = (char*)realloc(buf, buf_cap);
                        if (!buf) break;
                    }
                    memcpy(buf + buf_len, line, ll);
                    buf_len += ll;
                    buf[buf_len] = '\0';
                }

                size_t out_cap = buf_len * 2 + 128;
                char* out_buf = (char*)malloc(out_cap);
                if (out_buf && cc_ufcs_rewrite_line(buf, out_buf, out_cap) == 0) {
                    fputs(out_buf, out);
                } else {
                    fputs(buf, out);
                }
                free(out_buf);
                free(buf);
                continue;
            }

            /* Single-line UFCS lowering: only on lines where TCC recorded a UFCS-marked call. */
            if (ufcs_single && src_line_no > 0 && src_line_no <= ufcs_single_cap && ufcs_single[src_line_no]) {
                if (cc_ufcs_rewrite_line(line, rewritten, (size_t)cc_rewritten_cap) != 0) {
                    strncpy(rewritten, line, (size_t)cc_rewritten_cap - 1);
                    rewritten[(size_t)cc_rewritten_cap - 1] = '\0';
                }
                fputs(rewritten, out);
            } else {
                fputs(line, out);
            }
        }
        free(line);
        free(rewritten);
        free(ufcs_ml_end);
        free(ufcs_single);
        free(spawn_ml_end);
        free(spawn_argc);
        fclose(in);

        /* decl table cleanup */
        for (int d = 0; d < 256; d++) {
            for (int i = 0; i < decl_counts[d]; i++) free(decl_names[d][i]);
            free(decl_names[d]);
            for (int i = 0; i < decl_counts[d]; i++) free(decl_types[d][i]);
            free(decl_types[d]);
            free(decl_flags[d]);
        }

        if (closure_descs) {
            for (int i = 0; i < closure_count; i++) {
                for (int j = 0; j < closure_descs[i].cap_count; j++) free(closure_descs[i].cap_names[j]);
                free(closure_descs[i].cap_names);
                for (int j = 0; j < closure_descs[i].cap_count; j++) free(closure_descs[i].cap_types ? closure_descs[i].cap_types[j] : NULL);
                free(closure_descs[i].cap_types);
                free(closure_descs[i].cap_flags);
                    free(closure_descs[i].param1_name);
                    free(closure_descs[i].param0_name);
                    free(closure_descs[i].param0_type);
                    free(closure_descs[i].param1_type);
                free(closure_descs[i].body);
            }
            free(closure_descs);
        }
        free(closure_line_map);
        free(closure_protos);
        if (closure_defs && closure_defs_len > 0) {
            /* Emit closure definitions at end-of-file so global names are in scope. */
            fputs("\n/* --- CC generated closures --- */\n", out);
            fwrite(closure_defs, 1, closure_defs_len, out);
            fputs("/* --- end generated closures --- */\n", out);
        }
        free(closure_defs);
        if (src_ufcs != src_all) free(src_ufcs);
        free(src_all);
    } else {
        // Fallback stub when input is unavailable.
        fprintf(out,
                "#include \"std/prelude.cch\"\n"
                "int main(void) {\n"
                "  CCArena a = cc_heap_arena(kilobytes(1));\n"
                "  CCString s = cc_string_new(&a, 0);\n"
                "  cc_string_append_cstr(&a, &s, \"Hello, \");\n"
                "  cc_string_append_cstr(&a, &s, \"Concurrent-C via UFCS!\\n\");\n"
                "  cc_std_out_write(cc_string_as_slice(&s));\n"
                "  return 0;\n"
                "}\n");
    }

    fclose(out);
    return 0;
}

