#include "pass_closure_literal_ast.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util/path.h"
#include "util/text.h"
#include "visitor/pass_common.h"
#include "visitor/pass_type_syntax.h"

#ifndef CC_TCC_EXT_AVAILABLE
#error "CC_TCC_EXT_AVAILABLE is required (patched TCC stub-AST required)."
#endif

/* Alias shared types for local use */
typedef CCNodeView NodeView;


/* Local aliases for the shared helpers */
#define cc__is_ident_start_char cc_is_ident_start
#define cc__is_ident_char2 cc_is_ident_char

static size_t cc__find_closure_start_from_arrow(const char* src, size_t span_start, size_t arrow_off) {
    if (!src) return span_start;
    if (arrow_off <= span_start) return span_start;
    /* arrow_off points at '=' in '=>'. Walk backwards to find start of params. */
    size_t j = arrow_off;
    while (j > span_start && (src[j - 1] == ' ' || src[j - 1] == '\t' || src[j - 1] == '\r' || src[j - 1] == '\n')) j--;
    if (j <= span_start) return span_start;
    char prev = src[j - 1];
    if (prev == ')') {
        int par = 0;
        for (size_t k = j; k-- > span_start; ) {
            char ch = src[k];
            if (ch == ')') par++;
            else if (ch == '(') {
                par--;
                if (par == 0) return k;
            }
        }
        return span_start;
    }
    if (cc__is_ident_char2(prev)) {
        size_t k = j - 1;
        while (k > span_start && cc__is_ident_char2(src[k - 1])) k--;
        return k;
    }
    return span_start;
}

/* Best-effort start offset for closures when stub-AST columns are missing (e.g. closures originating from macro-expanded text). */
static int cc__closure_start_off_best_effort(const char* src, size_t len,
                                            int line_start, int line_end, int col_start,
                                            size_t* out_off, int* out_col_1based) {
    if (!src || !out_off || line_start <= 0) return 0;
    size_t lo = cc__offset_of_line_1based(src, len, line_start);
    if (col_start > 0) {
        /* Guard against bogus column spans (some macro-origin closures can have garbage col_start). */
        size_t line_hi = cc__offset_of_line_1based(src, len, line_start + 1);
        if (line_hi > len) line_hi = len;
        size_t cand = cc__offset_of_line_col_1based(src, len, line_start, col_start);
        if (cand < line_hi) {
            size_t j = cand;
            while (j < line_hi && (src[j] == ' ' || src[j] == '\t')) j++;
            /* Heuristic: closure literal starts at '(' (paren params), '[' (capture list), '@' (@unsafe), or an identifier (single-param form). */
            if (j < line_hi && (src[j] == '(' || src[j] == '[' || src[j] == '@' || cc__is_ident_start_char(src[j]))) {
            *out_off = cand;
            if (out_col_1based) *out_col_1based = col_start;
            return 1;
            }
        }
        /* fall through to arrow search */
    }
    int le = (line_end > 0) ? line_end : line_start;
    size_t hi = cc__offset_of_line_1based(src, len, le + 1);
    if (hi > len) hi = len;
    if (lo >= hi) return 0;
    /* Find first '=>' within the span and derive closure start from it. */
    size_t arrow = (size_t)-1;
    for (size_t i = lo; i + 1 < hi; i++) {
        if (src[i] == '=' && src[i + 1] == '>') { arrow = i; break; }
    }
    if (arrow == (size_t)-1) return 0;
    size_t st = cc__find_closure_start_from_arrow(src, lo, arrow);
    *out_off = st;
    if (out_col_1based) *out_col_1based = 1 + (int)(st - lo);
    return 1;
}

/* Best-effort: infer end offset of a closure literal when stub-AST didn't record col_end.
   We scan from `start_off` until we can match `=>` and then find the end of the body. */
static size_t cc__infer_closure_end_off(const char* src, size_t len, size_t start_off) {
    if (!src || start_off >= len) return len;
    size_t i = start_off;
    /* find '=>' */
    while (i + 1 < len) {
        if (src[i] == '=' && src[i + 1] == '>') { i += 2; break; }
        i++;
    }
    if (i >= len) return len;
    /* Scan body: if we see a '{' at top level, treat it as a block body and match braces.
       Otherwise treat as expression body and stop at a delimiter at top level. */
    int par = 0, brk = 0;
    int in_str = 0;
    char qch = 0;
    for (; i < len; i++) {
        char ch = src[i];
        if (in_str) {
            if (ch == '\\' && i + 1 < len) { i++; continue; }
            if (ch == qch) in_str = 0;
            continue;
        }
        if (ch == '"' || ch == '\'') { in_str = 1; qch = ch; continue; }
        if (ch == '(') { par++; continue; }
        if (ch == ')') {
            if (par == 0 && brk == 0) break;
            if (par) par--;
            continue;
        }
        if (ch == '[') { brk++; continue; }
        if (ch == ']') {
            if (brk == 0 && par == 0) break;
            if (brk) brk--;
            continue;
        }
        if (ch == '{' && par == 0 && brk == 0) {
            /* Block body: match braces starting here. */
            int br2 = 0;
            int in2 = 0;  /* in string */
            char q2 = 0;
            int in_lc = 0; /* in line comment */
            int in_bc = 0; /* in block comment */
            for (; i < len; i++) {
                char c2 = src[i];
                /* Handle line comment */
                if (in_lc) {
                    if (c2 == '\n') in_lc = 0;
                    continue;
                }
                /* Handle block comment */
                if (in_bc) {
                    if (c2 == '*' && i + 1 < len && src[i + 1] == '/') { in_bc = 0; i++; }
                    continue;
                }
                /* Handle string */
                if (in2) {
                    if (c2 == '\\' && i + 1 < len) { i++; continue; }
                    if (c2 == q2) in2 = 0;
                    continue;
                }
                /* Start of line comment */
                if (c2 == '/' && i + 1 < len && src[i + 1] == '/') { in_lc = 1; i++; continue; }
                /* Start of block comment */
                if (c2 == '/' && i + 1 < len && src[i + 1] == '*') { in_bc = 1; i++; continue; }
                /* Start of string */
                if (c2 == '"' || c2 == '\'') { in2 = 1; q2 = c2; continue; }
                if (c2 == '{') br2++;
                else if (c2 == '}') {
                    br2--;
                    if (br2 == 0) { i++; break; }
                }
            }
            return (i <= len) ? i : len;
        }
        /* Expression body: stop at delimiter at top level. */
        if (par == 0 && brk == 0) {
            if (ch == ',' || ch == ';' || ch == '\n') break;
        }
    }
    return i;
}

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

static void cc__append_n(char** buf, size_t* len, size_t* cap, const char* s, size_t n) {
    if (!buf || !len || !cap || !s) return;
    if (*len + n + 1 > *cap) {
        size_t nc = *cap ? *cap * 2 : 1024;
        while (nc < *len + n + 1) nc *= 2;
        char* nb = (char*)realloc(*buf, nc);
        if (!nb) return;
        *buf = nb;
        *cap = nc;
    }
    memcpy(*buf + *len, s, n);
    *len += n;
    (*buf)[*len] = 0;
}

static void cc__append_str(char** buf, size_t* len, size_t* cap, const char* s) {
    if (!s) return;
    cc__append_n(buf, len, cap, s, strlen(s));
}

static void cc__append_fmt(char** buf, size_t* len, size_t* cap, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char tmp[1024];
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n <= 0) return;
    if ((size_t)n >= sizeof(tmp)) {
        char* big = (char*)malloc((size_t)n + 1);
        if (!big) return;
        va_start(ap, fmt);
        vsnprintf(big, (size_t)n + 1, fmt, ap);
        va_end(ap);
        cc__append_n(buf, len, cap, big, (size_t)n);
        free(big);
        return;
    }
    cc__append_n(buf, len, cap, tmp, (size_t)n);
}

typedef struct {
    int id;
    int brace_depth_after_open;
} CCBodyNurseryFrame;

/* Best-effort lowering of @nursery/spawn inside a closure body block.
   NOTE: This is intentionally scoped to closure bodies (generated code), not the main TU rewrite. */
static char* cc__lower_nursery_spawn_in_body_text(int closure_id, const char* body) {
    if (!body) return NULL;
    size_t n = strlen(body);
    if (n == 0) return strdup(body);
    const char* p = body;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if (*p != '{') return strdup(body);

    char* out = NULL;
    size_t out_len = 0, out_cap = 0;

    CCBodyNurseryFrame stack[64];
    int top = -1;
    int brace_depth = 0;
    int nursery_counter = 0;

    const char* cur = body;
    while (*cur) {
        const char* line_start = cur;
        const char* nl = strchr(cur, '\n');
        const char* line_end = nl ? nl : (cur + strlen(cur));
        size_t line_len = (size_t)(line_end - line_start);

        size_t ind = 0;
        while (ind < line_len && (line_start[ind] == ' ' || line_start[ind] == '\t')) ind++;

        /* @nursery */
        const char* nur = NULL;
        for (size_t i = 0; i + 8 <= line_len; i++) {
            if (memcmp(line_start + i, "@nursery", 8) == 0) { nur = line_start + i; break; }
        }
        if (nur) {
            const char* brc = NULL;
            for (const char* x = nur; x < line_end; x++) {
                if (*x == '{') { brc = x; break; }
            }
            if (!brc) { free(out); return NULL; }
            nursery_counter++;
            int nid = nursery_counter;
            cc__append_fmt(&out, &out_len, &out_cap,
                           "%.*sCCNursery* __cc_nursery_body%d_%d = cc_nursery_create();\n"
                           "%.*sif (!__cc_nursery_body%d_%d) abort();\n"
                           "%.*s{\n",
                           (int)ind, line_start, closure_id, nid,
                           (int)ind, line_start, closure_id, nid,
                           (int)ind, line_start);
            brace_depth++;
            if (top + 1 < (int)(sizeof(stack) / sizeof(stack[0]))) {
                stack[++top] = (CCBodyNurseryFrame){ .id = nid, .brace_depth_after_open = brace_depth };
            }
            cur = nl ? (nl + 1) : line_end;
            continue;
        }

        /* spawn */
        const char* sp = NULL;
        for (size_t i = 0; i + 5 <= line_len; i++) {
            if (memcmp(line_start + i, "spawn", 5) == 0) { sp = line_start + i; break; }
        }
        if (sp) {
            if (top < 0) { free(out); return NULL; }
            const char* lp = NULL;
            const char* rp = NULL;
            for (const char* x = sp; x < line_end; x++) if (*x == '(') { lp = x; break; }
            for (const char* x = line_end; x-- > sp; ) if (*x == ')') { rp = x; break; }
            if (!lp || !rp || rp <= lp) { free(out); return NULL; }
            const char* a0 = lp + 1;
            const char* a1 = rp;
            while (a0 < a1 && (*a0 == ' ' || *a0 == '\t')) a0++;
            while (a1 > a0 && (a1[-1] == ' ' || a1[-1] == '\t')) a1--;
            int nid = stack[top].id;
            cc__append_fmt(&out, &out_len, &out_cap, "%.*s{ CCClosure0 __c = ", (int)ind, line_start);
            cc__append_n(&out, &out_len, &out_cap, a0, (size_t)(a1 - a0));
            cc__append_fmt(&out, &out_len, &out_cap,
                           "; cc_nursery_spawn_closure0(__cc_nursery_body%d_%d, __c); }\n",
                           closure_id, nid);
            cur = nl ? (nl + 1) : line_end;
            continue;
        }

        /* Inject epilogue before closing brace of an active nursery. */
        int closes_nursery = 0;
        if (top >= 0) {
            int opens = 0, closes = 0;
            for (const char* x = line_start; x < line_end; x++) {
                if (*x == '{') opens++;
                else if (*x == '}') closes++;
            }
            int new_depth = brace_depth + opens - closes;
            if (closes > 0 && new_depth == stack[top].brace_depth_after_open - 1) closes_nursery = 1;
        }
        if (closes_nursery) {
            int nid = stack[top].id;
            cc__append_fmt(&out, &out_len, &out_cap,
                           "%.*s  cc_nursery_wait(__cc_nursery_body%d_%d);\n"
                           "%.*s  cc_nursery_free(__cc_nursery_body%d_%d);\n",
                           (int)ind, line_start, closure_id, nid,
                           (int)ind, line_start, closure_id, nid);
            top--;
        }

        cc__append_n(&out, &out_len, &out_cap, line_start, line_len);
        if (nl) cc__append_n(&out, &out_len, &out_cap, "\n", 1);

        for (const char* x = line_start; x < line_end; x++) {
            if (*x == '{') brace_depth++;
            else if (*x == '}') { if (brace_depth > 0) brace_depth--; }
        }

        cur = nl ? (nl + 1) : line_end;
    }

    return out ? out : strdup(body);
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
    /* Ignore function prototypes (best-effort) */
    const char* lp = strchr(p, '(');
    if (lp && lp < semi) {
        const char* eq = strchr(p, '=');
        if (!eq || eq > lp) return;
    }
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
        if (*cur == '=' || *cur == ';') break;
        if (!cc__is_ident_start_char(*cur)) { cur++; continue; }
        const char* s = cur++;
        while (cur < semi && cc__is_ident_char2(*cur)) cur++;
        size_t n = (size_t)(cur - s);
        if (n == 0 || cc__is_keyword_tok(s, n)) continue;
        name_s = s;
        name_n = n;
    }
    if (!name_s || name_n == 0) return;
    const char* ty_s = p;
    const char* ty_e = name_s;
    while (ty_s < ty_e && (*ty_s == ' ' || *ty_s == '\t')) ty_s++;
    while (ty_e > ty_s && (ty_e[-1] == ' ' || ty_e[-1] == '\t')) ty_e--;
    if (ty_e <= ty_s) return;

    int cur_n = scope_counts[depth];
    if (cc__name_in_list(scope_names[depth], cur_n, name_s, name_n)) return;

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

    unsigned char flags = 0;
    if (strcmp(ty, "CCSlice") == 0) flags |= 1;
    if (is_slice && slice_has_bang) flags |= 2;

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
        if (s > block && (s[-1] == '.' || (s[-1] == '>' && s > block + 1 && s[-2] == '-'))) continue;
        int found = 0;
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

static char** cc__dup_string_list(char** xs, int n) {
    if (!xs || n <= 0) return NULL;
    char** out = (char**)calloc((size_t)n, sizeof(char*));
    if (!out) return NULL;
    for (int i = 0; i < n; i++) {
        out[i] = xs[i] ? strdup(xs[i]) : NULL;
    }
    return out;
}

typedef struct {
    char* name;
    char** param_types;
    char** param_names;  /* Parameter names for closure capture */
    int param_count;
    int line_start;      /* Line where function body starts */
} CCFuncSig;

static void cc__free_func_sigs(CCFuncSig* sigs, int n) {
    if (!sigs) return;
    for (int i = 0; i < n; i++) {
        free(sigs[i].name);
        for (int k = 0; k < sigs[i].param_count; k++) {
            free(sigs[i].param_types ? sigs[i].param_types[k] : NULL);
            free(sigs[i].param_names ? sigs[i].param_names[k] : NULL);
        }
        free(sigs[i].param_types);
        free(sigs[i].param_names);
    }
    free(sigs);
}

static const CCFuncSig* cc__lookup_sig(const CCFuncSig* sigs, int n, const char* name) {
    if (!sigs || n <= 0 || !name) return NULL;
    for (int i = 0; i < n; i++) {
        if (!sigs[i].name) continue;
        if (strcmp(sigs[i].name, name) == 0) return &sigs[i];
    }
    return NULL;
}

static int cc__param_is_const_ptr(const char* ty) {
    if (!ty) return 0;
    /* Best-effort: require both "const" and "*" somewhere in the type string. */
    if (!strstr(ty, "const")) return 0;
    if (!strchr(ty, '*')) return 0;
    return 1;
}

/* Check if a type string represents a safe wrapper that allows mutation in reference captures.
   Safe wrappers: @atomic T, Atomic<T>, Mutex<T>, CCChan*, CCChanTx, CCChanRx */
static int cc__is_safe_wrapper_type(const char* ty) {
    if (!ty) return 0;
    /* Skip leading whitespace */
    while (*ty == ' ' || *ty == '\t') ty++;
    /* @atomic prefix */
    if (strncmp(ty, "@atomic", 7) == 0 && (ty[7] == ' ' || ty[7] == '\t' || ty[7] == '\0')) return 1;
    /* Atomic<T> */
    if (strncmp(ty, "Atomic<", 7) == 0) return 1;
    /* Mutex<T> */
    if (strncmp(ty, "Mutex<", 6) == 0) return 1;
    /* CCChan* */
    if (strncmp(ty, "CCChan", 6) == 0) return 1;
    /* CCChanTx, CCChanRx */
    if (strncmp(ty, "CCChanTx", 8) == 0 || strncmp(ty, "CCChanRx", 8) == 0) return 1;
    return 0;
}

typedef enum {
    CC_MUT_NONE = 0,
    CC_MUT_WRITE = 1,
    CC_MUT_ADDR_OF_ESCAPES = 2,
    CC_MUT_ADDR_OF_NONCONST_CALL = 3,
} CCMutationKind;

static int cc__scan_skip_string_comment(const char* s, size_t n, size_t* io) {
    size_t i = *io;
    if (i >= n) return 0;
    if (s[i] == '"') {
        i++;
        while (i < n) {
            if (s[i] == '\\' && i + 1 < n) { i += 2; continue; }
            if (s[i] == '"') { i++; break; }
            i++;
        }
        *io = i;
        return 1;
    }
    if (s[i] == '\'' && i + 1 < n) {
        i++;
        if (i < n && s[i] == '\\' && i + 1 < n) i += 2;
        else i++;
        if (i < n && s[i] == '\'') i++;
        *io = i;
        return 1;
    }
    if (s[i] == '/' && i + 1 < n && s[i + 1] == '/') {
        i += 2;
        while (i < n && s[i] != '\n') i++;
        *io = i;
        return 1;
    }
    if (s[i] == '/' && i + 1 < n && s[i + 1] == '*') {
        i += 2;
        while (i + 1 < n && !(s[i] == '*' && s[i + 1] == '/')) i++;
        if (i + 1 < n) i += 2;
        *io = i;
        return 1;
    }
    return 0;
}

/* Best-effort classification for `&var` usage:
   - If inside a call arglist for a known function and the corresponding param is `const T*`, treat as read-only (OK).
   - If inside a call arglist but param is `T*` (or unknown), treat as potential write.
   - If not clearly inside a call, treat as address escape (potential write). */
static int cc__addr_of_is_readonly_call(const char* body,
                                       size_t amp_off,
                                       const char* var_name,
                                       const CCFuncSig* sigs,
                                       int sig_n,
                                       const char** out_callee,
                                       const char** out_param_ty) {
    if (out_callee) *out_callee = NULL;
    if (out_param_ty) *out_param_ty = NULL;
    if (!body || !var_name || !var_name[0]) return 0;

    size_t n = strlen(body);
    if (amp_off >= n) return 0;

    /* Find the arglist '(' that contains this '&' by scanning backward and balancing parens. */
    int par = 0;
    size_t lp = (size_t)-1;
    for (size_t i = amp_off; i > 0; i--) {
        char c = body[i - 1];
        if (c == ')') par++;
        else if (c == '(') {
            if (par == 0) { lp = i - 1; break; }
            par--;
        }
    }
    if (lp == (size_t)-1) return 0;

    /* Find callee identifier immediately before '(' (skip ws). */
    size_t j = lp;
    while (j > 0 && (body[j - 1] == ' ' || body[j - 1] == '\t')) j--;
    size_t end = j;
    while (j > 0 && cc__is_ident_char2(body[j - 1])) j--;
    if (j == end || !cc__is_ident_start_char(body[j])) return 0;
    size_t name_len = end - j;
    char callee[128];
    if (name_len >= sizeof(callee)) return 0;
    memcpy(callee, body + j, name_len);
    callee[name_len] = 0;

    const CCFuncSig* sig = cc__lookup_sig(sigs, sig_n, callee);
    if (!sig) return 0;

    /* Determine arg index by scanning from lp+1 to amp_off counting top-level commas. */
    int argi = 0;
    int p = 0, b = 0, sq = 0;
    for (size_t i = lp + 1; i < amp_off && i < n; i++) {
        size_t ii = i;
        if (cc__scan_skip_string_comment(body, n, &ii)) { i = ii ? (ii - 1) : i; continue; }
        char c = body[i];
        if (c == '(') p++;
        else if (c == ')' && p > 0) p--;
        else if (c == '{') b++;
        else if (c == '}' && b > 0) b--;
        else if (c == '[') sq++;
        else if (c == ']' && sq > 0) sq--;
        else if (c == ',' && p == 0 && b == 0 && sq == 0) argi++;
    }

    if (argi < 0 || argi >= sig->param_count) {
        if (out_callee) *out_callee = sig->name;
        return 0;
    }
    const char* pty = sig->param_types ? sig->param_types[argi] : NULL;
    if (out_callee) *out_callee = sig->name;
    if (out_param_ty) *out_param_ty = pty;
    return cc__param_is_const_ptr(pty);
}

/* Returns non-zero if mutation/potential mutation found. */
static int cc__find_mutation_in_body(const char* body,
                                     const char* var_name,
                                     const CCFuncSig* sigs,
                                     int sig_n,
                                     CCMutationKind* out_kind,
                                     size_t* out_offset,
                                     const char** out_callee,
                                     const char** out_param_ty) {
    if (!body || !var_name || !var_name[0]) return 0;
    size_t var_len = strlen(var_name);
    size_t body_len = strlen(body);
    if (out_kind) *out_kind = CC_MUT_NONE;
    if (out_callee) *out_callee = NULL;
    if (out_param_ty) *out_param_ty = NULL;
    
    for (size_t i = 0; i < body_len; i++) {
        /* Skip strings/comments. */
        size_t ii = i;
        if (cc__scan_skip_string_comment(body, body_len, &ii)) { i = ii ? (ii - 1) : i; continue; }
        
        /* Check for ++var or --var */
        if (i + 1 + var_len < body_len) {
            if ((body[i] == '+' && body[i+1] == '+') || (body[i] == '-' && body[i+1] == '-')) {
                size_t j = i + 2;
                while (j < body_len && (body[j] == ' ' || body[j] == '\t')) j++;
                if (j + var_len <= body_len && strncmp(body + j, var_name, var_len) == 0) {
                    char after = (j + var_len < body_len) ? body[j + var_len] : 0;
                    if (!cc__is_ident_char2(after)) {
                        if (out_offset) *out_offset = i;
                        if (out_kind) *out_kind = CC_MUT_WRITE;
                        return 1;
                    }
                }
            }
        }
        
        /* Check for identifier at position i */
        if (!cc__is_ident_start_char(body[i])) continue;
        if (i > 0 && cc__is_ident_char2(body[i-1])) continue;
        if (i + var_len > body_len) continue;
        if (strncmp(body + i, var_name, var_len) != 0) continue;
        char after = (i + var_len < body_len) ? body[i + var_len] : 0;
        if (cc__is_ident_char2(after)) continue;
        
        /* Found var_name at position i. Check for mutation. */
        size_t j = i + var_len;
        while (j < body_len && (body[j] == ' ' || body[j] == '\t')) j++;
        
        /* var++ or var-- */
        if (j + 1 < body_len && ((body[j] == '+' && body[j+1] == '+') || (body[j] == '-' && body[j+1] == '-'))) {
            if (out_offset) *out_offset = i;
            if (out_kind) *out_kind = CC_MUT_WRITE;
            return 1;
        }
        
        /* var = ..., var += ..., var -= ..., etc. */
        if (j < body_len && body[j] == '=') {
            if (j + 1 >= body_len || body[j+1] != '=') { /* not == */
                if (out_offset) *out_offset = i;
                if (out_kind) *out_kind = CC_MUT_WRITE;
                return 1;
            }
        }
        if (j + 1 < body_len && body[j+1] == '=' && 
            (body[j] == '+' || body[j] == '-' || body[j] == '*' || body[j] == '/' ||
             body[j] == '%' || body[j] == '&' || body[j] == '|' || body[j] == '^' ||
             body[j] == '<' || body[j] == '>')) {
            /* Handle <<= and >>= */
            if ((body[j] == '<' || body[j] == '>') && j + 2 < body_len && body[j+2] == '=') {
                if (out_offset) *out_offset = i;
                if (out_kind) *out_kind = CC_MUT_WRITE;
                return 1;
            }
            if (out_offset) *out_offset = i;
            if (out_kind) *out_kind = CC_MUT_WRITE;
            return 1;
        }
        
        /* Check for &var (address-of) */
        if (i > 0) {
            size_t k = i - 1;
            while (k > 0 && (body[k] == ' ' || body[k] == '\t')) k--;
            if (body[k] == '&') {
                /* Check it's not && */
                if (k == 0 || body[k-1] != '&') {
                    const char* callee = NULL;
                    const char* pty = NULL;
                    if (cc__addr_of_is_readonly_call(body, k, var_name, sigs, sig_n, &callee, &pty)) {
                        /* read-only: ok */
                    } else {
                        if (out_offset) *out_offset = k;
                        if (out_kind) *out_kind = (callee ? CC_MUT_ADDR_OF_NONCONST_CALL : CC_MUT_ADDR_OF_ESCAPES);
                        if (out_callee) *out_callee = callee;
                        if (out_param_ty) *out_param_ty = pty;
                        return 1;
                    }
                }
            }
        }
        
        i = j - 1; /* continue scanning after this identifier */
    }
    return 0;
}

typedef struct {
    int id;
    int start_line;
    int end_line;
    int start_col;
    int end_col;
    size_t start_off;
    size_t end_off;
    size_t body_start_off;
    size_t body_end_off;
    int param_count;
    char* param0_name;
    char* param1_name;
    char* param0_type;
    char* param1_type;
    int is_unsafe;
    char** explicit_cap_names;
    unsigned char* explicit_cap_flags; /* bit 0: is_ref */
    int explicit_cap_count;
    char** cap_names;
    char** cap_types;
    unsigned char* cap_flags;
    int cap_count;
    char* body_text; /* original body (includes braces for block bodies) */
} CCClosureDesc;

typedef struct { size_t start; size_t end; char* repl; } Edit;

static int cc__edit_cmp_start_desc(const void* a, const void* b) {
    const Edit* ea = (const Edit*)a;
    const Edit* eb = (const Edit*)b;
    if (ea->start > eb->start) return -1;
    if (ea->start < eb->start) return 1;
    return 0;
}

static char* cc__rewrite_with_edits(const char* src, size_t len, Edit* edits, int n, size_t* out_len) {
    if (!src || !edits || n <= 0) return NULL;
    qsort(edits, (size_t)n, sizeof(edits[0]), cc__edit_cmp_start_desc);
    char* out = (char*)malloc(len + 1);
    if (!out) return NULL;
    memcpy(out, src, len);
    out[len] = 0;
    size_t cur_len = len;

    for (int i = 0; i < n; i++) {
        size_t s = edits[i].start;
        size_t e = edits[i].end;
        if (e > cur_len || s > e) continue;
        const char* r = edits[i].repl ? edits[i].repl : "";
        size_t repl_len = strlen(r);
        size_t new_len = cur_len - (e - s) + repl_len;
        char* nb = (char*)malloc(new_len + 1);
        if (!nb) continue;
        memcpy(nb, out, s);
        memcpy(nb + s, r, repl_len);
        memcpy(nb + s + repl_len, out + e, cur_len - e);
        nb[new_len] = 0;
        free(out);
        out = nb;
        cur_len = new_len;
    }
    if (out_len) *out_len = cur_len;
    return out;
}

static char* cc__make_call_expr(const CCClosureDesc* d); /* forward */

static char* cc__lower_nested_closures_in_body(int parent_idx,
                                               const CCClosureDesc* descs,
                                               int desc_n) {
    if (!descs || parent_idx < 0 || parent_idx >= desc_n) return NULL;
    const CCClosureDesc* p = &descs[parent_idx];
    if (!p->body_text) return NULL;
    size_t body_len = strlen(p->body_text);
    if (body_len == 0) return strdup(p->body_text);

    Edit edits[256];
    int en = 0;
    for (int i = 0; i < desc_n && en < (int)(sizeof(edits) / sizeof(edits[0])); i++) {
        if (i == parent_idx) continue;
        const CCClosureDesc* c = &descs[i];
        if (c->start_off >= p->body_start_off && c->end_off <= p->body_end_off && c->end_off > c->start_off) {
            size_t rs = c->start_off - p->body_start_off;
            size_t re = c->end_off - p->body_start_off;
            if (rs >= body_len || re > body_len || re <= rs) continue;
            char* call = cc__make_call_expr(c);
            if (!call) continue;
            edits[en++] = (Edit){ .start = rs, .end = re, .repl = call };
        }
    }
    if (en == 0) return strdup(p->body_text);
    size_t new_len = 0;
    char* out = cc__rewrite_with_edits(p->body_text, body_len, edits, en, &new_len);
    for (int i = 0; i < en; i++) free(edits[i].repl);
    return out ? out : strdup(p->body_text);
}

static int cc__closure_is_nested_in_any_other(int k, const CCClosureDesc* descs, int desc_n) {
    if (!descs || k < 0 || k >= desc_n) return 0;
    const CCClosureDesc* d = &descs[k];
    if (d->end_off <= d->start_off) return 0;
    for (int i = 0; i < desc_n; i++) {
        if (i == k) continue;
        const CCClosureDesc* p = &descs[i];
        if (p->end_off <= p->start_off) continue;
        if (p->start_off < d->start_off && p->end_off >= d->end_off) return 1;
    }
    return 0;
}

static int cc__parse_closure_from_src(const char* src,
                                     size_t start_off,
                                     size_t end_off,
                                     int aux_param_count,
                                     CCClosureDesc* out) {
    if (!src || !out || end_off <= start_off) {
        if (getenv("CC_DEBUG_CLOSURE_EDITS")) {
            fprintf(stderr, "CC_DEBUG_CLOSURE_EDITS: parse_closure early return (null or bad range)\n");
        }
        return 0;
    }
    const char* s = src + start_off;
    size_t n = end_off - start_off;

    /* Find '=>' */
    size_t arrow = (size_t)-1;
    for (size_t i = 0; i + 1 < n; i++) {
        if (s[i] == '=' && s[i + 1] == '>') { arrow = i; break; }
    }
    if (arrow == (size_t)-1) return 0;

    /* Parse optional `@unsafe` prefix (expression-context marker). */
    {
        size_t u0 = 0;
        while (u0 < arrow && (s[u0] == ' ' || s[u0] == '\t')) u0++;
        if (u0 + 7 <= arrow && s[u0] == '@' && memcmp(s + u0 + 1, "unsafe", 6) == 0) {
            size_t u1 = u0 + 7;
            if (u1 == arrow || !cc__is_ident_char2(s[u1])) {
                out->is_unsafe = 1;
                /* Advance l0 trimming below will skip this prefix. */
            }
        }
    }

    /* Parse params on the left. */
    int param_count = 0;
    char p0[128] = {0}, p1[128] = {0};
    char t0[128] = {0}, t1[128] = {0};

    /* trim left */
    size_t l0 = 0, l1 = arrow;
    while (l0 < l1 && (s[l0] == ' ' || s[l0] == '\t')) l0++;
    while (l1 > l0 && (s[l1 - 1] == ' ' || s[l1 - 1] == '\t')) l1--;

    /* Skip `@unsafe` when parsing capture list/params. */
    if (l0 + 7 <= l1 && s[l0] == '@' && memcmp(s + l0 + 1, "unsafe", 6) == 0) {
        size_t u1 = l0 + 7;
        if (u1 == l1 || !cc__is_ident_char2(s[u1])) {
            out->is_unsafe = 1;
            l0 = u1;
            while (l0 < l1 && (s[l0] == ' ' || s[l0] == '\t')) l0++;
        }
    }

    /* Optional capture list: `[x, &y] ( ... ) => ...` */
    if (l0 < l1 && s[l0] == '[') {
        size_t j = l0 + 1;
        while (j < l1 && (s[j] == ' ' || s[j] == '\t')) j++;
        /* Disallow capture-all sugar: `[&]` and `[=]` */
        if (j + 1 < l1 && s[j] == '&' && s[j + 1] == ']') return -2; /* capture-all [&] banned */
        if (j < l1 && s[j] == '=' && (j + 1 == l1 || s[j + 1] == ']')) return -3; /* capture-all [=] banned */

        /* Find matching ']' */
        int sq = 1;
        size_t k = l0 + 1;
        for (; k < l1; k++) {
            if (s[k] == '[') sq++;
            else if (s[k] == ']') { sq--; if (sq == 0) break; }
        }
        if (k >= l1 || s[k] != ']') return 0;
        size_t cap_l = l0 + 1;
        size_t cap_r = k;

        /* Parse entries: (&)? ident, comma-separated */
        {
            char** names = NULL;
            unsigned char* flags = NULL;
            int nn = 0;
            size_t p = cap_l;
            while (p < cap_r) {
                while (p < cap_r && (s[p] == ' ' || s[p] == '\t')) p++;
                if (p >= cap_r) break;
                if (s[p] == ',') { p++; continue; }
                if (s[p] == '=') {
                    /* capture-all not allowed */
                    for (int i = 0; i < nn; i++) free(names[i]);
                    free(names); free(flags);
                    return 0;
                }
                int is_ref = 0;
                if (s[p] == '&') { is_ref = 1; p++; }
                while (p < cap_r && (s[p] == ' ' || s[p] == '\t')) p++;
                if (p >= cap_r || !cc__is_ident_start_char(s[p])) {
                    /* capture-all like `[&]` or malformed */
                    for (int i = 0; i < nn; i++) free(names[i]);
                    free(names); free(flags);
                    return 0;
                }
                size_t ns = p;
                p++;
                while (p < cap_r && cc__is_ident_char2(s[p])) p++;
                size_t nl = p - ns;
                char* nm = (char*)malloc(nl + 1);
                if (!nm) { for (int i = 0; i < nn; i++) free(names[i]); free(names); free(flags); return 0; }
                memcpy(nm, s + ns, nl);
                nm[nl] = 0;
                /* dedupe */
                int dup = 0;
                for (int q = 0; q < nn; q++) if (names[q] && strcmp(names[q], nm) == 0) { dup = 1; break; }
                if (dup) { free(nm); continue; }
                char** nnames = (char**)realloc(names, (size_t)(nn + 1) * sizeof(char*));
                unsigned char* nflags = (unsigned char*)realloc(flags, (size_t)(nn + 1) * sizeof(unsigned char));
                if (!nnames || !nflags) {
                    free(nm);
                    free(nnames); free(nflags);
                    for (int i = 0; i < nn; i++) free(names[i]);
                    free(names); free(flags);
                    return 0;
                }
                names = nnames;
                flags = nflags;
                names[nn] = nm;
                flags[nn] = (unsigned char)(is_ref ? 1 : 0);
                nn++;
            }
            out->explicit_cap_names = names;
            out->explicit_cap_flags = flags;
            out->explicit_cap_count = nn;
        }

        l0 = k + 1;
        while (l0 < l1 && (s[l0] == ' ' || s[l0] == '\t')) l0++;
        /* Capture list form requires paren params per spec. */
        if (l0 >= l1 || s[l0] != '(') return 0;
    }

    if (l0 < l1 && s[l0] == '(') {
        /* ( ... ) */
        size_t rp = l1;
        while (rp > l0 && s[rp - 1] != ')') rp--;
        if (rp <= l0) return 0;
        size_t ps = l0 + 1;
        size_t pe = rp - 1;
        while (ps < pe && (s[ps] == ' ' || s[ps] == '\t')) ps++;
        while (pe > ps && (s[pe - 1] == ' ' || s[pe - 1] == '\t')) pe--;
        if (ps == pe) {
            param_count = 0;
        } else {
            /* split by commas (no nesting expected) */
            const char* seg_s = s + ps;
            const char* endp = s + pe;
            int seg_idx = 0;
            const char* z = seg_s;
            while (z <= endp) {
                int at_end = (z == endp);
                if (!at_end && *z != ',') { z++; continue; }
                const char* seg_e = z;
                while (seg_s < seg_e && (*seg_s == ' ' || *seg_s == '\t')) seg_s++;
                while (seg_e > seg_s && (seg_e[-1] == ' ' || seg_e[-1] == '\t')) seg_e--;
                if (seg_e > seg_s) {
                    const char* nm_e = seg_e;
                    while (nm_e > seg_s && !cc__is_ident_char2(nm_e[-1])) nm_e--;
                    const char* nm_s = nm_e;
                    while (nm_s > seg_s && cc__is_ident_char2(nm_s[-1])) nm_s--;
                    if (nm_s < nm_e && cc__is_ident_start_char(*nm_s)) {
                        size_t nm_n = (size_t)(nm_e - nm_s);
                        const char* ty_s = seg_s;
                        const char* ty_e = nm_s;
                        while (ty_e > ty_s && (ty_e[-1] == ' ' || ty_e[-1] == '\t')) ty_e--;
                        if (seg_idx == 0) {
                            if (nm_n < sizeof(p0)) { memcpy(p0, nm_s, nm_n); p0[nm_n] = 0; }
                            if (ty_e > ty_s) {
                                size_t tn = (size_t)(ty_e - ty_s);
                                if (tn >= sizeof(t0)) tn = sizeof(t0) - 1;
                                memcpy(t0, ty_s, tn); t0[tn] = 0;
                            }
                            param_count = 1;
                        } else if (seg_idx == 1) {
                            if (nm_n < sizeof(p1)) { memcpy(p1, nm_s, nm_n); p1[nm_n] = 0; }
                            if (ty_e > ty_s) {
                                size_t tn = (size_t)(ty_e - ty_s);
                                if (tn >= sizeof(t1)) tn = sizeof(t1) - 1;
                                memcpy(t1, ty_s, tn); t1[tn] = 0;
                            }
                            param_count = 2;
                        }
                    }
                }
                seg_idx++;
                seg_s = z + 1;
                z++;
            }
        }
    } else if (l0 < l1 && cc__is_ident_start_char(s[l0])) {
        /* x => ... */
        size_t q = l0 + 1;
        while (q < l1 && cc__is_ident_char2(s[q])) q++;
        size_t nn = q - l0;
        if (nn == 0 || nn >= sizeof(p0) || cc__is_keyword_tok(s + l0, nn)) return 0;
        memcpy(p0, s + l0, nn);
        p0[nn] = 0;
        param_count = 1;
    }

    if (aux_param_count >= 0 && aux_param_count != param_count) {
        /* Prefer the AST-provided param_count when available (parser is authoritative). */
        param_count = aux_param_count;
        if (param_count == 0) { p0[0] = 0; p1[0] = 0; t0[0] = 0; t1[0] = 0; }
        if (param_count == 1 && p0[0] == 0) { /* leave unnamed */ }
        if (param_count == 2 && (p0[0] == 0 || p1[0] == 0)) { /* leave unnamed */ }
    }

    /* Parse body start (skip ws). */
    size_t b0 = arrow + 2;
    while (b0 < n && (s[b0] == ' ' || s[b0] == '\t' || s[b0] == '\r' || s[b0] == '\n')) b0++;
    if (b0 >= n) return 0;
    size_t body_start = b0;
    size_t body_end = n;
    if (s[body_start] == '{') {
        /* Find matching '}' within literal span, skipping strings and comments. */
        int br = 0;
        int in_str = 0;
        char qch = 0;
        int in_lc = 0; /* line comment */
        int in_bc = 0; /* block comment */
        size_t i = body_start;
        for (; i < n; i++) {
            char ch = s[i];
            if (in_lc) {
                if (ch == '\n') in_lc = 0;
                continue;
            }
            if (in_bc) {
                if (ch == '*' && i + 1 < n && s[i + 1] == '/') { in_bc = 0; i++; }
                continue;
            }
            if (in_str) {
                if (ch == '\\' && i + 1 < n) { i++; continue; }
                if (ch == qch) in_str = 0;
                continue;
            }
            if (ch == '/' && i + 1 < n && s[i + 1] == '/') { in_lc = 1; i++; continue; }
            if (ch == '/' && i + 1 < n && s[i + 1] == '*') { in_bc = 1; i++; continue; }
            if (ch == '"' || ch == '\'') { in_str = 1; qch = ch; continue; }
            if (ch == '{') br++;
            else if (ch == '}') {
                br--;
                if (br == 0) { i++; break; }
            }
        }
        if (br != 0) return 0;
        body_end = i;
    } else {
        /* Expression body: end at end_off (AST already bounded). */
        body_end = n;
    }

    out->param_count = param_count;
    out->param0_name = (param_count >= 1 && p0[0]) ? strdup(p0) : NULL;
    out->param1_name = (param_count >= 2 && p1[0]) ? strdup(p1) : NULL;
    out->param0_type = (param_count >= 1 && t0[0]) ? strdup(t0) : NULL;
    out->param1_type = (param_count >= 2 && t1[0]) ? strdup(t1) : NULL;

    out->body_start_off = start_off + body_start;
    out->body_end_off = start_off + body_end;
    size_t bn = body_end - body_start;
    out->body_text = (char*)malloc(bn + 1);
    if (!out->body_text) return 0;
    memcpy(out->body_text, s + body_start, bn);
    out->body_text[bn] = 0;
    return 1;
}

static void cc__free_closure_desc(CCClosureDesc* d) {
    if (!d) return;
    free(d->param0_name);
    free(d->param1_name);
    free(d->param0_type);
    free(d->param1_type);
    for (int i = 0; i < d->explicit_cap_count; i++) free(d->explicit_cap_names ? d->explicit_cap_names[i] : NULL);
    free(d->explicit_cap_names);
    free(d->explicit_cap_flags);
    for (int i = 0; i < d->cap_count; i++) free(d->cap_names ? d->cap_names[i] : NULL);
    free(d->cap_names);
    for (int i = 0; i < d->cap_count; i++) free(d->cap_types ? d->cap_types[i] : NULL);
    free(d->cap_types);
    free(d->cap_flags);
    free(d->body_text);
    memset(d, 0, sizeof(*d));
}

static int cc__cap_is_ref(const CCClosureDesc* d, const char* name) {
    if (!d || !name || !d->explicit_cap_names || !d->explicit_cap_flags) return 0;
    for (int i = 0; i < d->explicit_cap_count; i++) {
        if (!d->explicit_cap_names[i]) continue;
        if (strcmp(d->explicit_cap_names[i], name) == 0) {
            return (d->explicit_cap_flags[i] & 1) != 0;
        }
    }
    return 0;
}

static char* cc__make_call_expr(const CCClosureDesc* d) {
    if (!d) return NULL;
    char* b = NULL;
    size_t bl = 0, bc = 0;
    cc__append_fmt(&b, &bl, &bc, "__cc_closure_make_%d(", d->id);
    if (d->cap_count == 0) {
        cc__append_str(&b, &bl, &bc, ")");
    } else {
        for (int i = 0; i < d->cap_count; i++) {
            if (i) cc__append_str(&b, &bl, &bc, ", ");
            int is_ref = (d->cap_flags && (d->cap_flags[i] & 4) != 0);
            int mo = (!is_ref && d->cap_flags && (d->cap_flags[i] & 2) != 0);
            if (is_ref) cc__append_str(&b, &bl, &bc, "&");
            if (mo) cc__append_str(&b, &bl, &bc, "cc_move(");
            cc__append_str(&b, &bl, &bc, d->cap_names[i] ? d->cap_names[i] : "0");
            if (mo) cc__append_str(&b, &bl, &bc, ")");
        }
        cc__append_str(&b, &bl, &bc, ")");
    }
    return b;
}

int cc__rewrite_closure_literals_with_nodes(const CCASTRoot* root,
                                           const CCVisitorCtx* ctx,
                                           const char* in_src,
                                           size_t in_len,
                                           char** out_src,
                                           size_t* out_len,
                                           char** out_protos,
                                           size_t* out_protos_len,
                                           char** out_defs,
                                           size_t* out_defs_len) {
    if (!root || !ctx || !in_src || !out_src || !out_len || !out_protos || !out_protos_len || !out_defs || !out_defs_len) return 0;
    *out_src = NULL;
    *out_len = 0;
    *out_protos = NULL;
    *out_protos_len = 0;
    *out_defs = NULL;
    *out_defs_len = 0;
    if (!root->nodes || root->node_count <= 0) return 0;

    const NodeView* n = (const NodeView*)root->nodes;

    /* Build a best-effort function signature table from stub-AST FUNC/PARAM nodes.
       Used to allow `&x` only when passed to a known `const T*` parameter (2B),
       AND to register function parameters as capturable variables (closure support). */
    CCFuncSig* sigs = NULL;
    int sig_n = 0;
    {
        typedef struct { char** tys; char** names; int n; int cap; } Tmp;
        Tmp* tmp = (Tmp*)calloc((size_t)root->node_count, sizeof(Tmp));
        if (tmp) {
            for (int i = 0; i < root->node_count; i++) {
                if (n[i].kind != 16) continue; /* CC_AST_NODE_PARAM */
                int p = n[i].parent;
                if (p < 0 || p >= root->node_count) continue;
                if (n[p].kind != 17) continue; /* CC_AST_NODE_FUNC */
                if (!n[i].aux_s2) continue;
                if (!cc_pass_node_in_tu(root, ctx, n[p].file)) continue;
                if (tmp[p].n == tmp[p].cap) {
                    int nc = tmp[p].cap ? tmp[p].cap * 2 : 8;
                    char** nt = (char**)realloc(tmp[p].tys, (size_t)nc * sizeof(char*));
                    char** nn = (char**)realloc(tmp[p].names, (size_t)nc * sizeof(char*));
                    if (!nt || !nn) { free(nt); free(nn); continue; }
                    tmp[p].tys = nt;
                    tmp[p].names = nn;
                    tmp[p].cap = nc;
                }
                tmp[p].tys[tmp[p].n] = strdup(n[i].aux_s2);
                tmp[p].names[tmp[p].n] = n[i].aux_s1 ? strdup(n[i].aux_s1) : NULL;
                tmp[p].n++;
            }
            for (int i = 0; i < root->node_count; i++) {
                if (n[i].kind != 17) continue; /* CC_AST_NODE_FUNC */
                if (!n[i].aux_s1) continue;
                if (!cc_pass_node_in_tu(root, ctx, n[i].file)) continue;
                /* Insert/replace by name. */
                int idx = -1;
                for (int k = 0; k < sig_n; k++) {
                    if (sigs && sigs[k].name && strcmp(sigs[k].name, n[i].aux_s1) == 0) { idx = k; break; }
                }
                if (idx < 0) {
                    CCFuncSig* ns = (CCFuncSig*)realloc(sigs, (size_t)(sig_n + 1) * sizeof(CCFuncSig));
                    if (!ns) break;
                    sigs = ns;
                    idx = sig_n++;
                    memset(&sigs[idx], 0, sizeof(sigs[idx]));
                } else {
                    free(sigs[idx].name);
                    for (int k = 0; k < sigs[idx].param_count; k++) {
                        free(sigs[idx].param_types ? sigs[idx].param_types[k] : NULL);
                        free(sigs[idx].param_names ? sigs[idx].param_names[k] : NULL);
                    }
                    free(sigs[idx].param_types);
                    free(sigs[idx].param_names);
                    sigs[idx].name = NULL;
                    sigs[idx].param_types = NULL;
                    sigs[idx].param_names = NULL;
                    sigs[idx].param_count = 0;
                }
                sigs[idx].name = strdup(n[i].aux_s1);
                sigs[idx].param_types = tmp[i].tys;
                sigs[idx].param_names = tmp[i].names;
                sigs[idx].param_count = tmp[i].n;
                sigs[idx].line_start = n[i].line_start;
                tmp[i].tys = NULL;
                tmp[i].names = NULL;
                tmp[i].n = 0;
                tmp[i].cap = 0;
            }
            /* cleanup tmp leftovers */
            for (int i = 0; i < root->node_count; i++) {
                for (int k = 0; k < tmp[i].n; k++) {
                    free(tmp[i].tys ? tmp[i].tys[k] : NULL);
                    free(tmp[i].names ? tmp[i].names[k] : NULL);
                }
                free(tmp[i].tys);
                free(tmp[i].names);
            }
            free(tmp);
        }
    }

    /* Collect closure nodes in this TU. */
    int idxs_cap = 512;
    int* idxs = (int*)malloc((size_t)idxs_cap * sizeof(int));
    int idx_n = 0;
    if (!idxs) return 0;
    for (int i = 0; i < root->node_count; i++) {
        if (n[i].kind != CC_AST_NODE_CLOSURE) continue;
        if (!cc_pass_node_in_tu(root, ctx, n[i].file)) continue;
        if (n[i].line_start <= 0) continue;
        if (n[i].line_end <= 0) continue;
        if (idx_n == idxs_cap) {
            idxs_cap *= 2;
            int* ni = (int*)realloc(idxs, (size_t)idxs_cap * sizeof(int));
            if (!ni) break;
            idxs = ni;
        }
        idxs[idx_n++] = i;
    }
    if (idx_n == 0) { free(idxs); return 0; }

    /* Sort by best-effort start offset asc for stable ID assignment. */
    for (int i = 0; i < idx_n - 1; i++) {
        for (int j = i + 1; j < idx_n; j++) {
            size_t ai = 0, aj = 0;
            int ci = 1, cj = 1;
            cc__closure_start_off_best_effort(in_src, in_len, n[idxs[i]].line_start, n[idxs[i]].line_end, n[idxs[i]].col_start, &ai, &ci);
            cc__closure_start_off_best_effort(in_src, in_len, n[idxs[j]].line_start, n[idxs[j]].line_end, n[idxs[j]].col_start, &aj, &cj);
            if (aj < ai) { int t = idxs[i]; idxs[i] = idxs[j]; idxs[j] = t; }
        }
    }

    CCClosureDesc* descs = (CCClosureDesc*)calloc((size_t)idx_n, sizeof(CCClosureDesc));
    if (!descs) { free(idxs); return 0; }

    for (int k = 0; k < idx_n; k++) {
        int i = idxs[k];
        CCClosureDesc* d = &descs[k];
        d->id = k + 1;
        d->start_line = n[i].line_start;
        d->end_line = n[i].line_end;
        size_t start_off = 0;
        int start_col1 = 1;
        if (!cc__closure_start_off_best_effort(in_src, in_len, n[i].line_start, n[i].line_end, n[i].col_start, &start_off, &start_col1)) {
            cc__free_closure_desc(d);
            continue;
        }
        d->start_col = start_col1 - 1;
        d->end_col = (n[i].col_end > 0) ? (n[i].col_end - 1) : -1;
        d->start_off = start_off;
        
        /* Check for `@unsafe` prefix before the closure span (TCC consumes it separately).
           If found, expand the start_off to include it so the rewrite removes it. */
        if (start_off >= 7) {
            size_t j = start_off - 1;
            while (j > 0 && (in_src[j] == ' ' || in_src[j] == '\t')) j--;
            if (j >= 6 && in_src[j] == 'e' && strncmp(in_src + j - 5, "unsafe", 6) == 0) {
                size_t u = j - 5;
                if (u > 0 && in_src[u-1] == '@') {
                    d->is_unsafe = 1;
                    d->start_off = u - 1; /* Include @unsafe in the span to rewrite */
                }
            }
        }
        
        /* Stub-AST end spans for closures are not reliable in nested/multiline contexts.
           Always infer end from the actual source text (find => then match body). */
        d->end_off = cc__infer_closure_end_off(in_src, in_len, d->start_off);
        if (d->end_off > in_len) d->end_off = in_len;
        if (d->start_off >= d->end_off) { cc__free_closure_desc(d); continue; }
        if (getenv("CC_DEBUG_CLOSURE_SPANS")) {
            const char* f = (n[i].file && n[i].file[0]) ? n[i].file : (ctx->input_path ? ctx->input_path : "<input>");
            size_t tail_s = (d->end_off > 32) ? (d->end_off - 32) : 0;
            size_t tail_n = d->end_off - tail_s;
            fprintf(stderr, "CC_DEBUG_CLOSURE_SPANS: id=%d file=%s line=%d col_start=%d start_off=%zu end_off=%zu tail=\"%.*s\"\n",
                    d->id, f, n[i].line_start, n[i].col_start, d->start_off, d->end_off, (int)tail_n, in_src + tail_s);
            /* Also show what text is at start_off */
            size_t show_len = (d->end_off - d->start_off > 60) ? 60 : (d->end_off - d->start_off);
            fprintf(stderr, "CC_DEBUG_CLOSURE_SPANS:   start_text=\"%.*s\"\n", (int)show_len, in_src + d->start_off);
        }
        int pr = cc__parse_closure_from_src(in_src, d->start_off, d->end_off, n[i].aux1, d);
        if (getenv("CC_DEBUG_CLOSURE_EDITS")) {
            fprintf(stderr, "CC_DEBUG_CLOSURE_EDITS: parse_closure id=%d returned %d body_text=%s\n",
                    d->id, pr, d->body_text ? "yes" : "NULL");
        }
        if (pr == -2) {
            const char* f = (n[i].file && n[i].file[0]) ? n[i].file : (ctx->input_path ? ctx->input_path : "<input>");
            fprintf(stderr, "%s:%d: error: capture-all [&] is not allowed\n", f, n[i].line_start);
            cc__free_closure_desc(d);
            free(descs);
            free(idxs);
            cc__free_func_sigs(sigs, sig_n);
            return -1;
        }
        if (pr == -3) {
            const char* f = (n[i].file && n[i].file[0]) ? n[i].file : (ctx->input_path ? ctx->input_path : "<input>");
            fprintf(stderr, "%s:%d: error: capture-all [=] is not allowed\n", f, n[i].line_start);
            cc__free_closure_desc(d);
            free(descs);
            free(idxs);
            cc__free_func_sigs(sigs, sig_n);
            return -1;
        }
    }

    /* Walk file text in order, record simple decls, and compute captures for each closure at its location. */
    char** scope_names[256];
    char** scope_types[256];
    unsigned char* scope_flags[256];
    int scope_counts[256];
    for (int d = 0; d < 256; d++) { scope_names[d] = NULL; scope_types[d] = NULL; scope_flags[d] = NULL; scope_counts[d] = 0; }
    int depth = 0;
    int cur_closure = 0;
    int line_num = 1;  /* Current line number (1-based) for function param registration */

    const char* cur = in_src;
    size_t off = 0;
    while (off < in_len && *cur) {
        const char* line_start = cur;
        const char* nl = memchr(cur, '\n', in_len - off);
        const char* line_end = nl ? nl : (in_src + in_len);
        size_t line_len = (size_t)(line_end - line_start);

        /* Record decls on this line at current depth. */
        char tmp[1024];
        size_t cp = line_len < sizeof(tmp) - 1 ? line_len : sizeof(tmp) - 1;
        memcpy(tmp, line_start, cp);
        tmp[cp] = 0;
        cc__maybe_record_decl(scope_names, scope_types, scope_flags, scope_counts, depth, tmp);

        /* Process any closures that start on or before the end of this line. */
        while (cur_closure < idx_n && descs[cur_closure].start_off < (off + line_len + 1)) {
            CCClosureDesc* d = &descs[cur_closure];
            /* Start with explicit captures from `[ ... ]` (if any), then add implicit value captures
               from free-var scan of the body (ignore param names). */
            char** caps = cc__dup_string_list(d->explicit_cap_names, d->explicit_cap_count);
            int cap_n = d->explicit_cap_count;
            if (!caps && d->explicit_cap_count > 0) {
                fprintf(stderr, "%s:%d:%d: error: CC: out of memory while building closure captures\n",
                        ctx->input_path ? ctx->input_path : "<input>",
                        d->start_line,
                        (d->start_col >= 0 ? (d->start_col + 1) : 1));
                for (int q = 0; q < idx_n; q++) cc__free_closure_desc(&descs[q]);
                free(descs);
                for (int dd = 0; dd < 256; dd++) {
                    for (int k2 = 0; k2 < scope_counts[dd]; k2++) free(scope_names[dd][k2]);
                    free(scope_names[dd]);
                    for (int k2 = 0; k2 < scope_counts[dd]; k2++) free(scope_types[dd][k2]);
                    free(scope_types[dd]);
                    free(scope_flags[dd]);
                }
                free(idxs);
                return -1;
            }
            if (d->body_text) {
                cc__collect_caps_from_block(scope_names, scope_counts, depth,
                                            d->body_text,
                                            d->param0_name,
                                            d->param1_name,
                                            &caps, &cap_n);
            }
            d->cap_names = caps;
            d->cap_count = cap_n;
            if (cap_n > 0) {
                d->cap_types = (char**)calloc((size_t)cap_n, sizeof(char*));
                d->cap_flags = (unsigned char*)calloc((size_t)cap_n, sizeof(unsigned char));
                if (!d->cap_types || !d->cap_flags) {
                    free(d->cap_types); free(d->cap_flags);
                    d->cap_types = NULL; d->cap_flags = NULL;
                } else {
                    for (int ci = 0; ci < cap_n; ci++) {
                        const char* ty = NULL;
                        unsigned char fl = 0;
                        for (int dd = depth; dd >= 1 && !ty; dd--) {
                            ty = cc__lookup_decl_type(scope_names[dd], scope_types[dd], scope_counts[dd], caps[ci]);
                            if (ty) fl = cc__lookup_decl_flags(scope_names[dd], scope_flags[dd], scope_counts[dd], caps[ci]);
                        }
                        if (ty) d->cap_types[ci] = strdup(ty);
                        if (cc__cap_is_ref(d, caps[ci])) fl |= 4; /* bit 2: reference capture */
                        d->cap_flags[ci] = fl;
                        if (!ty) {
                            int col1 = d->start_col >= 0 ? (d->start_col + 1) : 1;
                            fprintf(stderr,
                                    "%s:%d:%d: error: CC: cannot infer type for captured name '%s' (currently supports simple decls like 'int x = ...;' or 'T* p = ...;')\n",
                                    ctx->input_path ? ctx->input_path : "<input>",
                                    d->start_line,
                                    col1,
                                    caps[ci] ? caps[ci] : "?");
                            /* cleanup */
                            for (int q = 0; q < idx_n; q++) cc__free_closure_desc(&descs[q]);
                            free(descs);
                            for (int dd = 0; dd < 256; dd++) {
                                for (int k2 = 0; k2 < scope_counts[dd]; k2++) free(scope_names[dd][k2]);
                                free(scope_names[dd]);
                                for (int k2 = 0; k2 < scope_counts[dd]; k2++) free(scope_types[dd][k2]);
                                free(scope_types[dd]);
                                free(scope_flags[dd]);
                            }
                            free(idxs);
                            return -1;
                        }
                    }
                }
            }
            /* Check for mutations to reference-captured variables (unless @unsafe). */
            if (!d->is_unsafe && d->body_text && d->cap_count > 0) {
                for (int ci = 0; ci < d->cap_count; ci++) {
                    int is_ref = (d->cap_flags && (d->cap_flags[ci] & 4) != 0);
                    if (!is_ref) continue;
                    const char* ty = d->cap_types ? d->cap_types[ci] : NULL;
                    if (cc__is_safe_wrapper_type(ty)) continue;
                    const char* nm = d->cap_names[ci];
                    size_t mut_off = 0;
                    CCMutationKind mk = CC_MUT_NONE;
                    const char* callee = NULL;
                    const char* pty = NULL;
                    if (cc__find_mutation_in_body(d->body_text, nm, sigs, sig_n, &mk, &mut_off, &callee, &pty)) {
                        int col1 = d->start_col >= 0 ? (d->start_col + 1) : 1;
                        if (mk == CC_MUT_ADDR_OF_NONCONST_CALL && callee) {
                            fprintf(stderr,
                                    "%s:%d:%d: error: passing '&%s' to '%s' may mutate shared state (data race)\n",
                                    ctx->input_path ? ctx->input_path : "<input>",
                                    d->start_line,
                                    col1,
                                    nm ? nm : "var",
                                    callee);
                            if (pty) {
                                fprintf(stderr, "  = note: parameter type is '%s' (not const)\n", pty);
                            } else {
                                fprintf(stderr, "  = note: callee parameter is not known to be 'const T*'\n");
                            }
                            fprintf(stderr,
                                    "  = help: make the parameter 'const %s*' for read-only, or use a safe wrapper / @unsafe\n",
                                    ty ? ty : "T");
                        } else if (mk == CC_MUT_ADDR_OF_ESCAPES) {
                            fprintf(stderr,
                                    "%s:%d:%d: error: taking address of shared reference '%s' may allow mutation (data race)\n",
                                    ctx->input_path ? ctx->input_path : "<input>",
                                    d->start_line,
                                    col1,
                                    nm ? nm : "?");
                            fprintf(stderr,
                                    "  = help: pass as 'const %s*' to a known read-only function, or use a safe wrapper / @unsafe\n",
                                    ty ? ty : "T");
                        } else {
                            fprintf(stderr,
                                    "%s:%d:%d: error: mutation of shared reference '%s' in closure\n",
                                    ctx->input_path ? ctx->input_path : "<input>",
                                    d->start_line,
                                    col1,
                                    nm ? nm : "?");
                            fprintf(stderr,
                                    "  = note: concurrent mutation causes data races\n"
                                    "  = help: use @atomic %s, Mutex<%s>, or @unsafe [&%s]\n",
                                    ty ? ty : "T", ty ? ty : "T", nm ? nm : "var");
                        }
                        /* cleanup and fail */
                        for (int q = 0; q < idx_n; q++) cc__free_closure_desc(&descs[q]);
                        free(descs);
                        for (int dd = 0; dd < 256; dd++) {
                            for (int k2 = 0; k2 < scope_counts[dd]; k2++) free(scope_names[dd][k2]);
                            free(scope_names[dd]);
                            for (int k2 = 0; k2 < scope_counts[dd]; k2++) free(scope_types[dd][k2]);
                            free(scope_types[dd]);
                            free(scope_flags[dd]);
                        }
                        free(idxs);
                        cc__free_func_sigs(sigs, sig_n);
                        return -1;
                    }
                }
            }
            cur_closure++;
        }

        /* Update brace depth and clear scope on close (best-effort, same as old scanner). */
        for (const char* x = line_start; x < line_end; x++) {
            if (*x == '{') {
                depth++;
                /* When entering a new scope, check if this is a function body and register its parameters.
                   This enables closures to capture function parameters, not just local variables. */
                for (int si = 0; si < sig_n; si++) {
                    if (sigs[si].line_start == line_num && sigs[si].param_names) {
                        for (int pi = 0; pi < sigs[si].param_count; pi++) {
                            if (!sigs[si].param_names[pi] || !sigs[si].param_types[pi]) continue;
                            /* Register this parameter as a declaration at current depth */
                            int cur_n = scope_counts[depth];
                            char** next = (char**)realloc(scope_names[depth], (size_t)(cur_n + 1) * sizeof(char*));
                            char** tnext = (char**)realloc(scope_types[depth], (size_t)(cur_n + 1) * sizeof(char*));
                            unsigned char* fnext = (unsigned char*)realloc(scope_flags[depth], (size_t)(cur_n + 1) * sizeof(unsigned char));
                            if (next && tnext && fnext) {
                                scope_names[depth] = next;
                                scope_types[depth] = tnext;
                                scope_flags[depth] = fnext;
                                scope_names[depth][cur_n] = strdup(sigs[si].param_names[pi]);
                                scope_types[depth][cur_n] = strdup(sigs[si].param_types[pi]);
                                scope_flags[depth][cur_n] = 0;
                                scope_counts[depth] = cur_n + 1;
                            }
                        }
                        break;
                    }
                }
            } else if (*x == '}') {
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
        off = (size_t)(cur - in_src);
        line_num++;  /* Advance line counter for function param registration */
    }

    /* Emit protos/defs and build rewrite edits for all closure literals. */
    char* protos = NULL;
    size_t protos_len = 0, protos_cap = 0;
    char* defs = NULL;
    size_t defs_len = 0, defs_cap = 0;

    cc__append_str(&defs, &defs_len, &defs_cap, "/* --- CC generated closures --- */\n");

    Edit edits[2048];
    int en = 0;
    if (getenv("CC_DEBUG_CLOSURE_EDITS")) {
        fprintf(stderr, "CC_DEBUG_CLOSURE_EDITS: processing %d closure descriptors\n", idx_n);
    }
    for (int k = 0; k < idx_n && en < (int)(sizeof(edits) / sizeof(edits[0])); k++) {
        CCClosureDesc* d = &descs[k];
        if (getenv("CC_DEBUG_CLOSURE_EDITS")) {
            fprintf(stderr, "CC_DEBUG_CLOSURE_EDITS: desc[%d] id=%d body_text=%s\n",
                    k, d->id, d->body_text ? "yes" : "NULL");
        }
        if (!d->body_text) continue;

        /* protos - entry function and make function prototypes */
        if (d->param_count == 0) cc__append_fmt(&protos, &protos_len, &protos_cap, "static void* __cc_closure_entry_%d(void*);\n", d->id);
        else if (d->param_count == 1) cc__append_fmt(&protos, &protos_len, &protos_cap, "static void* __cc_closure_entry_%d(void*, intptr_t);\n", d->id);
        else cc__append_fmt(&protos, &protos_len, &protos_cap, "static void* __cc_closure_entry_%d(void*, intptr_t, intptr_t);\n", d->id);

        /* __cc_closure_make_N prototype - includes captured variable types */
        const char* cty_p = (d->param_count == 0 ? "CCClosure0" : (d->param_count == 1 ? "CCClosure1" : "CCClosure2"));
        cc__append_fmt(&protos, &protos_len, &protos_cap, "static %s __cc_closure_make_%d(", cty_p, d->id);
        if (d->cap_count == 0) {
            cc__append_str(&protos, &protos_len, &protos_cap, "void");
        } else {
            for (int ci = 0; ci < d->cap_count; ci++) {
                if (ci) cc__append_str(&protos, &protos_len, &protos_cap, ", ");
                int is_ref = (d->cap_flags && (d->cap_flags[ci] & 4) != 0);
                const char* ty = d->cap_types && d->cap_types[ci] ? d->cap_types[ci] : "int";
                const char* nm = d->cap_names[ci] ? d->cap_names[ci] : "__cap";
                if (is_ref) {
                    cc__append_fmt(&protos, &protos_len, &protos_cap, "%s* %s", ty, nm);
                } else {
                    cc__append_fmt(&protos, &protos_len, &protos_cap, "%s %s", ty, nm);
                }
            }
        }
        cc__append_str(&protos, &protos_len, &protos_cap, ");\n");

        /* defs: env+drop+make */
        cc__append_fmt(&defs, &defs_len, &defs_cap,
                       "/* CC closure %d (from %s:%d) */\n",
                       d->id,
                       ctx->input_path ? ctx->input_path : "<input>",
                       d->start_line);

        const char* cty = (d->param_count == 0 ? "CCClosure0" : (d->param_count == 1 ? "CCClosure1" : "CCClosure2"));
        const char* mkfn = (d->param_count == 0 ? "cc_closure0_make" : (d->param_count == 1 ? "cc_closure1_make" : "cc_closure2_make"));

        if (d->cap_count > 0) {
            cc__append_fmt(&defs, &defs_len, &defs_cap, "typedef struct __cc_closure_env_%d {\n", d->id);
            for (int ci = 0; ci < d->cap_count; ci++) {
                int is_ref = (d->cap_flags && (d->cap_flags[ci] & 4) != 0);
                const char* ty = d->cap_types[ci] ? d->cap_types[ci] : "int";
                const char* nm = d->cap_names[ci] ? d->cap_names[ci] : "__cap";
                if (is_ref) {
                    cc__append_fmt(&defs, &defs_len, &defs_cap, "  %s* %s;\n", ty, nm);
                } else {
                    cc__append_fmt(&defs, &defs_len, &defs_cap, "  %s %s;\n", ty, nm);
                }
            }
            cc__append_str(&defs, &defs_len, &defs_cap, "} ");
            cc__append_fmt(&defs, &defs_len, &defs_cap, "__cc_closure_env_%d;\n", d->id);
            cc__append_fmt(&defs, &defs_len, &defs_cap,
                           "static void __cc_closure_env_%d_drop(void* p) { if (p) free(p); }\n",
                           d->id);

            cc__append_fmt(&defs, &defs_len, &defs_cap, "static %s __cc_closure_make_%d(", cty, d->id);
            for (int ci = 0; ci < d->cap_count; ci++) {
                if (ci) cc__append_str(&defs, &defs_len, &defs_cap, ", ");
                int is_ref = (d->cap_flags && (d->cap_flags[ci] & 4) != 0);
                const char* ty = d->cap_types[ci] ? d->cap_types[ci] : "int";
                const char* nm = d->cap_names[ci] ? d->cap_names[ci] : "__cap";
                if (is_ref) {
                    cc__append_fmt(&defs, &defs_len, &defs_cap, "%s* %s", ty, nm);
                } else {
                    cc__append_fmt(&defs, &defs_len, &defs_cap, "%s %s", ty, nm);
                }
            }
            cc__append_str(&defs, &defs_len, &defs_cap, ") {\n");
            cc__append_fmt(&defs, &defs_len, &defs_cap,
                           "  __cc_closure_env_%d* __env = (__cc_closure_env_%d*)malloc(sizeof(__cc_closure_env_%d));\n",
                           d->id, d->id, d->id);
            cc__append_str(&defs, &defs_len, &defs_cap, "  if (!__env) abort();\n");
            for (int ci = 0; ci < d->cap_count; ci++) {
                cc__append_fmt(&defs, &defs_len, &defs_cap,
                               "  __env->%s = %s;\n",
                               d->cap_names[ci] ? d->cap_names[ci] : "__cap",
                               d->cap_names[ci] ? d->cap_names[ci] : "__cap");
            }
            /* TSan release: ensure captured values are visible to worker thread */
            cc__append_str(&defs, &defs_len, &defs_cap, "  CC_TSAN_RELEASE(__env);\n");
            cc__append_fmt(&defs, &defs_len, &defs_cap,
                           "  return %s(__cc_closure_entry_%d, __env, __cc_closure_env_%d_drop);\n",
                           mkfn, d->id, d->id);
            cc__append_str(&defs, &defs_len, &defs_cap, "}\n");
        } else {
            cc__append_fmt(&defs, &defs_len, &defs_cap,
                           "static %s __cc_closure_make_%d(void) { return %s(__cc_closure_entry_%d, NULL, NULL); }\n",
                           cty, d->id, mkfn, d->id);
        }

        /* defs: entry */
        if (d->param_count == 0) {
            cc__append_fmt(&defs, &defs_len, &defs_cap, "static void* __cc_closure_entry_%d(void* __p) {\n", d->id);
        } else if (d->param_count == 1) {
            cc__append_fmt(&defs, &defs_len, &defs_cap, "static void* __cc_closure_entry_%d(void* __p, intptr_t __arg0) {\n", d->id);
        } else {
            cc__append_fmt(&defs, &defs_len, &defs_cap, "static void* __cc_closure_entry_%d(void* __p, intptr_t __arg0, intptr_t __arg1) {\n", d->id);
        }
        if (d->cap_count > 0) {
            cc__append_fmt(&defs, &defs_len, &defs_cap, "  __cc_closure_env_%d* __env = (__cc_closure_env_%d*)__p;\n", d->id, d->id);
            for (int ci = 0; ci < d->cap_count; ci++) {
                int is_ref = (d->cap_flags && (d->cap_flags[ci] & 4) != 0);
                const char* ty = d->cap_types[ci] ? d->cap_types[ci] : "int";
                const char* nm = d->cap_names[ci] ? d->cap_names[ci] : "__cap";
                if (is_ref) {
                    /* Reference capture: dereference the stored pointer.
                       Use a local reference-like alias. */
                    cc__append_fmt(&defs, &defs_len, &defs_cap, "  %s* __cc_ref_%s = __env->%s;\n", ty, nm, nm);
                    cc__append_fmt(&defs, &defs_len, &defs_cap, "#define %s (*__cc_ref_%s)\n", nm, nm);
                } else {
                    cc__append_fmt(&defs, &defs_len, &defs_cap, "  %s %s = __env->%s;\n", ty, nm, nm);
                }
            }
        } else {
            cc__append_str(&defs, &defs_len, &defs_cap, "  (void)__p;\n");
        }

        if (d->param_count == 1) {
            if (d->param0_name) {
                if (d->param0_type) cc__append_fmt(&defs, &defs_len, &defs_cap, "  %s %s = (%s)__arg0;\n", d->param0_type, d->param0_name, d->param0_type);
                else cc__append_fmt(&defs, &defs_len, &defs_cap, "  intptr_t %s = __arg0;\n", d->param0_name);
            } else {
                cc__append_str(&defs, &defs_len, &defs_cap, "  (void)__arg0;\n");
            }
        } else if (d->param_count == 2) {
            if (d->param0_name) {
                if (d->param0_type) cc__append_fmt(&defs, &defs_len, &defs_cap, "  %s %s = (%s)__arg0;\n", d->param0_type, d->param0_name, d->param0_type);
                else cc__append_fmt(&defs, &defs_len, &defs_cap, "  intptr_t %s = __arg0;\n", d->param0_name);
            } else cc__append_str(&defs, &defs_len, &defs_cap, "  (void)__arg0;\n");
            if (d->param1_name) {
                if (d->param1_type) cc__append_fmt(&defs, &defs_len, &defs_cap, "  %s %s = (%s)__arg1;\n", d->param1_type, d->param1_name, d->param1_type);
                else cc__append_fmt(&defs, &defs_len, &defs_cap, "  intptr_t %s = __arg1;\n", d->param1_name);
            } else cc__append_str(&defs, &defs_len, &defs_cap, "  (void)__arg1;\n");
        }

        char* lowered_body = cc__lower_nested_closures_in_body(k, descs, idx_n);
        if (!lowered_body) lowered_body = strdup(d->body_text);
        /* Map diagnostics within the closure body back to the original source location. */
        {
            char rel[1024];
            cc__append_fmt(&defs, &defs_len, &defs_cap, "#line %d \"%s\"\n",
                           d->start_line,
                           cc_path_rel_to_repo(ctx->input_path ? ctx->input_path : "<input>", rel, sizeof(rel)));
        }
        if (lowered_body && lowered_body[0] == '{') {
            char* lowered2 = cc__lower_nursery_spawn_in_body_text(d->id, lowered_body);
            if (!lowered2) lowered2 = strdup(lowered_body);
            /* Rewrite T!E result types in closure body */
            char* lowered3 = cc__rewrite_result_types_text(ctx, lowered2, strlen(lowered2));
            cc__append_fmt(&defs, &defs_len, &defs_cap, "  %s\n", lowered3 ? lowered3 : lowered2);
            free(lowered3);
            free(lowered2);
        } else {
            cc__append_fmt(&defs, &defs_len, &defs_cap, "  (void)(%s);\n", lowered_body ? lowered_body : "0");
        }
        free(lowered_body);
        /* Undefine reference capture macros to avoid polluting subsequent code. */
        for (int ci = 0; ci < d->cap_count; ci++) {
            int is_ref = (d->cap_flags && (d->cap_flags[ci] & 4) != 0);
            if (is_ref && d->cap_names[ci]) {
                cc__append_fmt(&defs, &defs_len, &defs_cap, "#undef %s\n", d->cap_names[ci]);
            }
        }
        cc__append_str(&defs, &defs_len, &defs_cap, "  return NULL;\n}\n\n");

        char* call = cc__make_call_expr(d);
        if (!call) continue;
        /* Important: do not apply nested closure edits to the main source buffer.
           The outermost closure rewrite removes the body text from the source, and we rewrite nested closures
           inside the generated entry function body separately (cc__lower_nested_closures_in_body). */
        int is_nested = cc__closure_is_nested_in_any_other(k, descs, idx_n);
        if (getenv("CC_DEBUG_CLOSURE_EDITS")) {
            fprintf(stderr, "CC_DEBUG_CLOSURE_EDITS: id=%d nested=%d start=%zu end=%zu\n",
                    d->id, is_nested, d->start_off, d->end_off);
        }
        if (!is_nested) {
            edits[en++] = (Edit){ .start = d->start_off, .end = d->end_off, .repl = call };
        } else {
            free(call);
        }
    }
    cc__append_str(&defs, &defs_len, &defs_cap, "/* --- end generated closures --- */\n");

    if (getenv("CC_DEBUG_CLOSURE_EDITS")) {
        fprintf(stderr, "CC_DEBUG_CLOSURE_EDITS: applying %d edits to source (len=%zu)\n", en, in_len);
    }

    /* Apply edits to source. */
    size_t rewritten_len = 0;
    char* rewritten = cc__rewrite_with_edits(in_src, in_len, edits, en, &rewritten_len);
    for (int i = 0; i < en; i++) free(edits[i].repl);

    /* cleanup scope maps */
    for (int dd = 0; dd < 256; dd++) {
        for (int k2 = 0; k2 < scope_counts[dd]; k2++) free(scope_names[dd][k2]);
        free(scope_names[dd]);
        for (int k2 = 0; k2 < scope_counts[dd]; k2++) free(scope_types[dd][k2]);
        free(scope_types[dd]);
        free(scope_flags[dd]);
    }

    for (int q = 0; q < idx_n; q++) cc__free_closure_desc(&descs[q]);
    free(descs);
    free(idxs);
    cc__free_func_sigs(sigs, sig_n);

    if (!rewritten) {
        free(protos);
        free(defs);
        return 0;
    }
    *out_src = rewritten;
    *out_len = rewritten_len;
    *out_protos = protos;
    *out_protos_len = protos_len;
    *out_defs = defs;
    *out_defs_len = defs_len;
    return 1;
}

/* NEW: Collect closure literal edits into EditBuffer.
   NOTE: This pass generates protos and defs that must be emitted separately.
   The function adds the source edits to eb, and protos/defs via add_protos/add_defs.
   Returns number of edits added (>= 0), or -1 on error. */
int cc__collect_closure_edits(const CCASTRoot* root,
                              const CCVisitorCtx* ctx,
                              CCEditBuffer* eb) {
    if (!root || !ctx || !eb || !eb->src) return 0;

    char* rewritten = NULL;
    size_t rewritten_len = 0;
    char* protos = NULL;
    size_t protos_len = 0;
    char* defs = NULL;
    size_t defs_len = 0;

    int r = cc__rewrite_closure_literals_with_nodes(root, ctx, eb->src, eb->src_len,
                                                    &rewritten, &rewritten_len,
                                                    &protos, &protos_len,
                                                    &defs, &defs_len);
    if (r < 0) {
        free(rewritten);
        free(protos);
        free(defs);
        return -1;
    }
    if (r == 0 || !rewritten) {
        free(protos);
        free(defs);
        return 0;
    }

    int edits_added = 0;

    /* Add protos and defs to the EditBuffer */
    if (protos && protos_len > 0) {
        cc_edit_buffer_add_protos(eb, protos, protos_len);
    }
    if (defs && defs_len > 0) {
        cc_edit_buffer_add_defs(eb, defs, defs_len);
    }

    /* Add source replacement edit */
    if (rewritten_len != eb->src_len || memcmp(rewritten, eb->src, eb->src_len) != 0) {
        if (cc_edit_buffer_add(eb, 0, eb->src_len, rewritten, 60, "closure_literals") == 0) {
            edits_added = 1;
        }
    }

    free(rewritten);
    free(protos);
    free(defs);
    return edits_added;
}