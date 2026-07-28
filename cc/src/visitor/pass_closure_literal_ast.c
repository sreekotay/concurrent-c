#include "pass_closure_literal_ast.h"
#include "../diag/mangle.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util/path.h"
#include "util/text.h"
#include "util/text_scan.h"
#include "preprocess/preprocess.h"
#include "preprocess/type_registry.h"
#include "visitor/pass_common.h"
#include "visitor/pass_defer_syntax.h"
#include "visitor/pass_err_syntax.h"
#include "visitor/pass_result_unwrap.h"
#include "visitor/pass_type_syntax.h"

#ifndef CC_TCC_EXT_AVAILABLE
#error "CC_TCC_EXT_AVAILABLE is required (patched TCC stub-AST required)."
#endif

/* Alias shared types for local use */
typedef CCNodeView NodeView;


/* Local aliases for the shared helpers */
#define cc__is_ident_start_char cc_is_ident_start
#define cc__is_ident_char2 cc_is_ident_char

static int cc__is_internal_generated_name(const char* s, size_t n) {
    return s && n >= 5 && strncmp(s, "__cc_", 5) == 0;
}

static int cc__skip_generated_name_for_capture_scan(const char* s, size_t n) {
    if (!cc__is_internal_generated_name(s, n)) return 0;
    /* Auto-block rewrite temps must remain capturable by lowered closures. */
    if (n >= 8 && strncmp(s, "__cc_ab_", 8) == 0) return 0;
    return 1;
}

static char* cc__dup_decl_type_text(const char* ty_s,
                                    const char* ty_e,
                                    unsigned char* out_flags);

static int cc__capture_type_text_usable(const char* ty, const char* name) {
    const char* s = ty;
    const char* e = NULL;
    int has_ident = 0;
    if (!ty) return 0;
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') s++;
    e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n')) e--;
    if (e <= s) return 0;
    if (name && strlen(name) == (size_t)(e - s) && strncmp(s, name, (size_t)(e - s)) == 0) return 0;
    if (strstr(s, "/*") || strstr(s, "*/") || strchr(s, '`')) return 0;
    for (const char* p = s; p < e; p++) {
        if (cc__is_ident_start_char(*p)) { has_ident = 1; break; }
    }
    return has_ident;
}

static int cc__looks_like_macro_constant(const char* s, size_t n) {
    int has_alpha = 0;
    if (!s || n == 0) return 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c >= 'a' && c <= 'z') return 0;
        if ((c >= 'A' && c <= 'Z') || c == '_') has_alpha = 1;
        else if (c >= '0' && c <= '9') {
            continue;
        } else {
            return 0;
        }
    }
    return has_alpha;
}

/* Walk forward from `from` looking for the next `=>` token at the top
 * level of the buffer, skipping over C comments, string literals, and
 * character literals.  Returns the offset of the `=` byte, or `(size_t)-1`
 * if none is found before `lim` (or end-of-buffer).
 *
 * This is the comment-safe replacement for the bare loop
 *   `for (i = from; i + 1 < lim; i++) if (src[i]=='=' && src[i+1]=='>') ...`
 * which has caused real bugs: a `// ... => exit` comment between two
 * real closures latched the recovery scanner onto the comment's `=>`,
 * producing a fake closure descriptor at the comment's text — and the
 * real closure's call site then went unrewritten because its descriptor
 * was misresolved (syscall_kidnap.ccs reproducer). */
static size_t cc__find_next_arrow_skipping_inert(const char* src, size_t lim, size_t from) {
    if (!src) return (size_t)-1;
    CCInertScan scan;
    cc_inert_scan_init(&scan, NULL);
    scan.at_line_start = 0;  /* mid-buffer slice */
    size_t i = from;
    while (i + 1 < lim) {
        if (cc_inert_scan_step(&scan, src, lim, &i)) continue;
        if (src[i] == '=' && src[i + 1] == '>') return i;
        i++;
    }
    return (size_t)-1;
}

/* Best-effort: infer end offset of a closure literal when stub-AST didn't record col_end.
   We scan from `start_off` until we can match `=>` and then find the end of the body. */
static size_t cc__infer_closure_end_off(const char* src, size_t len, size_t start_off) {
    if (!src || start_off >= len) return len;
    /* Phase 1: find the closure's `=>` using the shared comment-safe
     * helper.  Original phase-1 walked raw bytes; a `// ... => ...`
     * line comment between start_off and the real `=>` could latch
     * onto the comment's arrow (the syscall_kidnap.ccs reproducer
     * documented at `cc__find_next_arrow_skipping_inert`). */
    size_t arrow = cc__find_next_arrow_skipping_inert(src, len, start_off);
    if (arrow == (size_t)-1) return len;
    size_t i = arrow + 2;  /* past `=>` */
    if (i >= len) return len;

    /* Phase 2: scan body.  If we see a `{` at top level, treat it as a
     * block body and match braces (delegating to `cc_find_matching_brace`).
     * Otherwise treat as expression body and stop at a delimiter at top
     * level.  Behavior change: original tracked strings only (no chars,
     * no comments); CCInertScan makes both branches comment- and
     * char-literal-aware. */
    int par = 0, brk = 0;
    CCInertScan scan;
    cc_inert_scan_init(&scan, NULL);
    scan.at_line_start = 0;  /* mid-buffer slice */
    while (i < len) {
        if (cc_inert_scan_step(&scan, src, len, &i)) continue;
        char ch = src[i];
        if (ch == '(') { par++; i++; continue; }
        if (ch == ')') {
            if (par == 0 && brk == 0) break;
            if (par) par--;
            i++;
            continue;
        }
        if (ch == '[') { brk++; i++; continue; }
        if (ch == ']') {
            if (brk == 0 && par == 0) break;
            if (brk) brk--;
            i++;
            continue;
        }
        if (ch == '{' && par == 0 && brk == 0) {
            /* Phase 3: block body — delegate to the shared brace
             * matcher (handles strings/chars/comments correctly). */
            size_t rbrace = 0;
            if (cc_find_matching_brace(src, len, i, &rbrace)) {
                return rbrace + 1;
            }
            return len;
        }
        /* Expression body: stop at delimiter at top level. */
        if (par == 0 && brk == 0) {
            if (ch == ',' || ch == ';' || ch == '\n') break;
        }
        i++;
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

static int cc__name_in_list(char* const* xs, int n, const char* s, size_t slen) {
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

/* Rewrite a `<base>_make(args)` closure constructor call to its
 * nursery-aware variant `<base>_make_nursery(nursery, args)`.  Used when a
 * closure-make expression is being spawned into a nursery (the nursery
 * variant uses the nursery's arena for the env allocation instead of malloc).
 *
 * Detection: the function name ends in `_make` (suffix-based since the
 * 2026-05-28 mangler integration; the legacy `__cc_closure_make_<id>` prefix
 * naming was retired so this scanner can no longer rely on a fixed prefix
 * shape and instead looks at the trailing role tag).  */
static char* cc__rewrite_closure_make_for_nursery(const char* s, size_t n, const char* nursery_expr) {
    if (!s || !nursery_expr) return NULL;
    size_t lo = 0, hi = n;
    while (lo < hi && (s[lo] == ' ' || s[lo] == '\t' || s[lo] == '\r' || s[lo] == '\n')) lo++;
    while (hi > lo && (s[hi - 1] == ' ' || s[hi - 1] == '\t' || s[hi - 1] == '\r' || s[hi - 1] == '\n')) hi--;
    if (hi <= lo) return NULL;

    size_t fn_s = lo;
    if (!(isalpha((unsigned char)s[fn_s]) || s[fn_s] == '_')) return NULL;
    size_t fn_e = fn_s + 1;
    while (fn_e < hi && (isalnum((unsigned char)s[fn_e]) || s[fn_e] == '_')) fn_e++;

    /* Require the function name to end with `_make` (5 chars).  Allows us to
     * splice in `_nursery` after `_make` without touching the location-tagged
     * base prefix.  Reject anything that doesn't end this way so callers
     * passing in unrelated calls fall back to keeping the original expression. */
    static const char make_suffix[] = "_make";
    const size_t make_suffix_len = sizeof(make_suffix) - 1;
    if ((fn_e - fn_s) <= make_suffix_len) return NULL;
    if (memcmp(s + fn_e - make_suffix_len, make_suffix, make_suffix_len) != 0) return NULL;

    size_t p = fn_e;
    while (p < hi && (s[p] == ' ' || s[p] == '\t')) p++;
    if (p >= hi || s[p] != '(' || s[hi - 1] != ')') return NULL;

    size_t args_s = p + 1;
    size_t args_e = hi - 1;
    while (args_s < args_e && (s[args_s] == ' ' || s[args_s] == '\t')) args_s++;
    while (args_e > args_s && (s[args_e - 1] == ' ' || s[args_e - 1] == '\t')) args_e--;

    char* out = NULL;
    size_t out_len = 0, out_cap = 0;
    /* Emit `<base>_make_nursery` where `<base>` is everything before `_make`. */
    cc__append_n(&out, &out_len, &out_cap, s + fn_s, fn_e - fn_s - make_suffix_len);
    cc__append_str(&out, &out_len, &out_cap, "_make_nursery");
    cc__append_str(&out, &out_len, &out_cap, "(");
    cc__append_str(&out, &out_len, &out_cap, nursery_expr);
    if (args_e > args_s) {
        cc__append_str(&out, &out_len, &out_cap, ", ");
        cc__append_n(&out, &out_len, &out_cap, s + args_s, args_e - args_s);
    }
    cc__append_str(&out, &out_len, &out_cap, ")");
    return out;
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
                           "%.*sCCNursery* __cc_nursery_body%d_%d = cc_nursery_create(NULL);\n"
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
            char nursery_name[64];
            snprintf(nursery_name, sizeof(nursery_name), "__cc_nursery_body%d_%d", closure_id, nid);
            char* nursery_make = cc__rewrite_closure_make_for_nursery(a0, (size_t)(a1 - a0), nursery_name);
            cc__append_fmt(&out, &out_len, &out_cap, "%.*s{ CCClosure0 __c = ", (int)ind, line_start);
            if (nursery_make) cc__append_str(&out, &out_len, &out_cap, nursery_make);
            else cc__append_n(&out, &out_len, &out_cap, a0, (size_t)(a1 - a0));
            cc__append_fmt(&out, &out_len, &out_cap,
                           "; cc_nursery_spawn_closure0(__cc_nursery_body%d_%d, __c); }\n",
                           closure_id, nid);
            free(nursery_make);
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

static void cc__maybe_record_decl_stmt(char*** scope_names,
                                       char*** scope_types,
                                       unsigned char** scope_flags,
                                       int* scope_counts,
                                       int depth,
                                       const char* stmt,
                                       const char* stmt_end) {
    if (!scope_names || !scope_types || !scope_flags || !scope_counts || depth < 0 || depth >= 256 || !stmt) return;
    const char* p = stmt;
    for (;;) {
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
        if (p[0] == '/' && p[1] == '/') {
            p += 2;
            while (*p && *p != '\n') p++;
            continue;
        }
        if (p[0] == '/' && p[1] == '*') {
            p += 2;
            while (p < stmt_end && !(p[0] == '*' && p[1] == '/')) p++;
            if (p < stmt_end) p += 2;
            continue;
        }
        break;
    }
    if (*p == '#' || *p == '\0') return;
    if ((strncmp(p, "for", 3) == 0 && !cc__is_ident_char2(p[3])) ||
        (strncmp(p, "if", 2) == 0 && !cc__is_ident_char2(p[2])) ||
        (strncmp(p, "while", 5) == 0 && !cc__is_ident_char2(p[5])) ||
        (strncmp(p, "switch", 6) == 0 && !cc__is_ident_char2(p[6]))) {
        return;
    }
    const char* semi = stmt_end;
    if (!semi || semi <= p) return;
    /* Base + bounded length for the indexed CCInertScan walks below.
     * `stmt` is the slice base; valid offsets are [p_off, semi_off). */
    size_t p_off = (size_t)(p - stmt);
    size_t semi_off = (size_t)(semi - stmt);

    /* Ignore function prototypes (best-effort) */
    {
        CCInertScan scan;
        cc_inert_scan_init(&scan, NULL);
        scan.at_line_start = 0;
        size_t lp_off = (size_t)-1;
        size_t eq_off = (size_t)-1;
        size_t i = p_off;
        while (i < semi_off) {
            if (cc_inert_scan_step(&scan, stmt, semi_off, &i)) continue;
            char c = stmt[i];
            if (c == '(' && lp_off == (size_t)-1) lp_off = i;
            if (c == '=' && eq_off == (size_t)-1) eq_off = i;
            i++;
        }
        if (lp_off != (size_t)-1 && (eq_off == (size_t)-1 || eq_off > lp_off)) return;
    }
    /* Ignore member assignments like `g.block = ...` or `p->field = ...`.
       Two-pass: first find `=`, then check for `.`/`->` before it. */
    {
        CCInertScan scan;
        cc_inert_scan_init(&scan, NULL);
        scan.at_line_start = 0;
        const char* eq = NULL;
        size_t i = p_off;
        while (i < semi_off) {
            if (cc_inert_scan_step(&scan, stmt, semi_off, &i)) continue;
            char c = stmt[i];
            char c2 = (i + 1 < semi_off) ? stmt[i + 1] : 0;
            if (c == '=' && c2 != '=') { eq = stmt + i; break; }
            i++;
        }
        if (eq) {
            for (const char* t = p; t < eq; t++) {
                if (*t == '.' || (*t == '>' && t > p && t[-1] == '-')) return;
            }
        }
    }
    /* Collect top-level comma positions to handle multi-declarations
     * like: CCChanTx g_tx0, g_tx1, g_tx2; */
    const char* comma_pos[64];
    int comma_n = 0;
    {
        CCInertScan scan;
        cc_inert_scan_init(&scan, NULL);
        scan.at_line_start = 0;
        int paren = 0, bracket = 0;
        size_t i = p_off;
        while (i < semi_off) {
            if (cc_inert_scan_step(&scan, stmt, semi_off, &i)) continue;
            char c = stmt[i];
            if (c == '(') paren++;
            else if (c == ')' && paren > 0) paren--;
            else if (c == '[') bracket++;
            else if (c == ']' && bracket > 0) bracket--;
            else if (c == ',' && paren == 0 && bracket == 0 && comma_n < 64) {
                comma_pos[comma_n++] = stmt + i;
            }
            i++;
        }
    }

    /* Parse the first segment (before the first comma, or the whole statement)
     * to extract the base type and first variable name. */
    const char* first_seg_end = (comma_n > 0) ? comma_pos[0] : semi;
    const char* name_s = NULL;
    size_t name_n = 0;
    {
        CCInertScan scan;
        cc_inert_scan_init(&scan, NULL);
        scan.at_line_start = 0;
        size_t seg_end_off = (size_t)(first_seg_end - stmt);
        size_t i = p_off;
        while (i < seg_end_off) {
            if (cc_inert_scan_step(&scan, stmt, seg_end_off, &i)) continue;
            char c = stmt[i];
            if (c == '=' || c == ';') break;
            /* The declarator name precedes its array dimensions: stop at the
               first `[` so a macro dimension (`T name[MACRO_N]`) cannot
               overwrite the name with the dimension identifier. */
            if (c == '[') break;
            if (!cc__is_ident_start_char(c)) { i++; continue; }
            size_t s_off = i;
            i++;
            while (i < seg_end_off && cc__is_ident_char2(stmt[i])) i++;
            size_t tok_n = i - s_off;
            if (tok_n == 0 || cc__is_keyword_tok(stmt + s_off, tok_n)) continue;
            name_s = stmt + s_off;
            name_n = tok_n;
        }
    }
    if (!name_s || name_n == 0) return;
    const char* ty_s = p;
    const char* ty_e = name_s;
    {
        const char* after = name_s + name_n;
        int has_ident = 0;
        while (after <= semi && (*after == ' ' || *after == '\t')) after++;
        if (after <= semi && (*after == '=' || *after == ';' || *after == ',' || *after == '[')) {
            /* looks like a valid declaration */
        } else if (after > semi && comma_n > 0) {
            /* multi-decl: name is past first comma, fall through */
        } else {
            return;
        }
        for (const char* q = ty_s; q < ty_e; q++) {
            if (cc__is_ident_start_char(*q)) { has_ident = 1; break; }
        }
        if (!has_ident) return;
    }
    while (ty_s < ty_e && (*ty_s == ' ' || *ty_s == '\t')) ty_s++;
    while (ty_e > ty_s && (ty_e[-1] == ' ' || ty_e[-1] == '\t')) ty_e--;
    if (ty_e <= ty_s) return;

    unsigned char flags = 0;
    char* ty = cc__dup_decl_type_text(ty_s, ty_e, &flags);
    if (!ty) return;

    /* Detect user `T* p = &local` — value-capturing p then writing *p / p->f is
     * the SHAPE-T7 smuggle past reference-capture mutation bans (bit 3 / 0x08).
     * Skip compiler temps (cc_ab_*, __cc_*, …) from autoblock / lowering. */
    {
        int synthetic = (name_n >= 4 && name_s[0] == 'c' && name_s[1] == 'c' && name_s[2] == '_') ||
                        (name_n >= 5 && name_s[0] == '_' && name_s[1] == '_' &&
                         name_s[2] == 'c' && name_s[3] == 'c' && name_s[4] == '_');
        int is_ptr = 0;
        int is_const_ptr = 0;
        for (const char* q = ty; *q; q++) {
            if (*q == '*') { is_ptr = 1; break; }
        }
        if (is_ptr) {
            const char* star = strchr(ty, '*');
            if (star) {
                const char* c = strstr(ty, "const");
                if (c && c < star) is_const_ptr = 1;
            }
        }
        if (!synthetic && is_ptr && !is_const_ptr) {
            CCInertScan scan;
            cc_inert_scan_init(&scan, NULL);
            scan.at_line_start = 0;
            size_t i = p_off;
            size_t eq = (size_t)-1;
            while (i < semi_off) {
                if (cc_inert_scan_step(&scan, stmt, semi_off, &i)) continue;
                if (stmt[i] == '=' && (i + 1 >= semi_off || stmt[i + 1] != '=')) {
                    eq = i;
                    break;
                }
                i++;
            }
            if (eq != (size_t)-1) {
                size_t j = eq + 1;
                while (j < semi_off && (stmt[j] == ' ' || stmt[j] == '\t' ||
                                        stmt[j] == '\n' || stmt[j] == '\r'))
                    j++;
                if (j < semi_off && stmt[j] == '&') {
                    j++;
                    while (j < semi_off && (stmt[j] == ' ' || stmt[j] == '\t')) j++;
                    if (j < semi_off && cc__is_ident_start_char(stmt[j])) {
                        flags |= 0x08; /* aliases_outer_local */
                    }
                }
            }
        }
    }

    /* Helper: record a single name+type in the scope arrays. */
    #define RECORD_NAME_IN_SCOPE(nm_s, nm_n, ty_dup) do { \
        int cur_n_ = scope_counts[depth]; \
        if (cc__name_in_list(scope_names[depth], cur_n_, (nm_s), (nm_n))) break; \
        char* nm_ = (char*)malloc((nm_n) + 1); \
        if (!nm_) break; \
        memcpy(nm_, (nm_s), (nm_n)); nm_[(nm_n)] = '\0'; \
        char** nn_ = (char**)realloc(scope_names[depth], (size_t)(cur_n_ + 1) * sizeof(char*)); \
        if (!nn_) { free(nm_); break; } scope_names[depth] = nn_; \
        char** tn_ = (char**)realloc(scope_types[depth], (size_t)(cur_n_ + 1) * sizeof(char*)); \
        if (!tn_) { free(nm_); break; } scope_types[depth] = tn_; \
        unsigned char* fn_ = (unsigned char*)realloc(scope_flags[depth], (size_t)(cur_n_ + 1) * sizeof(unsigned char)); \
        if (!fn_) { free(nm_); break; } scope_flags[depth] = fn_; \
        scope_names[depth][cur_n_] = nm_; \
        scope_types[depth][cur_n_] = strdup(ty_dup); \
        scope_flags[depth][cur_n_] = flags; \
        scope_counts[depth] = cur_n_ + 1; \
    } while (0)

    RECORD_NAME_IN_SCOPE(name_s, name_n, ty);

    /* For comma-separated multi-declarations, record each additional name
     * with the same base type. */
    for (int ci = 0; ci < comma_n; ci++) {
        const char* seg_s = comma_pos[ci] + 1;
        const char* seg_e = (ci + 1 < comma_n) ? comma_pos[ci + 1] : semi;
        while (seg_s < seg_e && (*seg_s == ' ' || *seg_s == '\t' || *seg_s == '\n' || *seg_s == '\r')) seg_s++;
        const char* ex_name = NULL;
        size_t ex_name_n = 0;
        const char* cur = seg_s;
        while (cur < seg_e) {
            if (*cur == '=' || *cur == ';') break;
            if (*cur == '[') break; /* name precedes array dims (macro dim) */
            if (!cc__is_ident_start_char(*cur)) { cur++; continue; }
            const char* s = cur++;
            while (cur < seg_e && cc__is_ident_char2(*cur)) cur++;
            size_t n = (size_t)(cur - s);
            if (n == 0 || cc__is_keyword_tok(s, n)) continue;
            ex_name = s;
            ex_name_n = n;
        }
        if (ex_name && ex_name_n > 0) {
            RECORD_NAME_IN_SCOPE(ex_name, ex_name_n, ty);
        }
    }
    #undef RECORD_NAME_IN_SCOPE
    free(ty);
}

static void cc__maybe_record_decl(char*** scope_names,
                                  char*** scope_types,
                                  unsigned char** scope_flags,
                                  int* scope_counts,
                                  int depth,
                                  const char* line) {
    if (!scope_names || !scope_types || !scope_flags || !scope_counts || depth < 0 || depth >= 256 || !line) return;
    size_t n = strlen(line);
    size_t stmt_off = 0;
    while (stmt_off < n) {
        int paren_depth = 0, brace_depth = 0, bracket_depth = 0;
        CCInertScan scan;
        cc_inert_scan_init(&scan, NULL);
        size_t i = stmt_off;
        size_t semi_off = (size_t)-1;
        while (i < n) {
            if (cc_inert_scan_step(&scan, line, n, &i)) continue;
            char c = line[i];
            if (c == '(') paren_depth++;
            else if (c == ')') { if (paren_depth > 0) paren_depth--; }
            else if (c == '{') brace_depth++;
            else if (c == '}') { if (brace_depth > 0) brace_depth--; }
            else if (c == '[') bracket_depth++;
            else if (c == ']') { if (bracket_depth > 0) bracket_depth--; }
            else if (c == ';' && paren_depth == 0 && brace_depth == 0 && bracket_depth == 0) {
                semi_off = i;
                break;
            }
            i++;
        }
        if (semi_off == (size_t)-1) break;
        cc__maybe_record_decl_stmt(scope_names, scope_types, scope_flags, scope_counts,
                                   depth, line + stmt_off, line + semi_off);
        stmt_off = semi_off + 1;
    }
}

static size_t cc__last_top_level_semi_offset(const char* line) {
    if (!line) return 0;
    size_t n = strlen(line);
    size_t last = 0;  /* 0 = no semi found */
    int paren_depth = 0, brace_depth = 0, bracket_depth = 0;
    CCInertScan scan;
    cc_inert_scan_init(&scan, NULL);
    size_t i = 0;
    while (i < n) {
        if (cc_inert_scan_step(&scan, line, n, &i)) continue;
        char c = line[i];
        if (c == '(') paren_depth++;
        else if (c == ')') { if (paren_depth > 0) paren_depth--; }
        else if (c == '{') brace_depth++;
        else if (c == '}') { if (brace_depth > 0) brace_depth--; }
        else if (c == '[') bracket_depth++;
        else if (c == ']') { if (bracket_depth > 0) bracket_depth--; }
        else if (c == ';' && paren_depth == 0 && brace_depth == 0 && bracket_depth == 0) {
            last = i + 1;
        }
        i++;
    }
    return last;
}

static int cc__has_top_level_brace(const char* line) {
    if (!line) return 0;
    size_t n = strlen(line);
    int paren_depth = 0, bracket_depth = 0;
    CCInertScan scan;
    cc_inert_scan_init(&scan, NULL);
    /* Note: callers pass comment-stripped text (built from
     * `cc__src_strip_comments_and_strings`), so original's lack of
     * comment tracking was harmless.  Migrated form adds comment
     * awareness for free — defensive against any future caller. */
    size_t i = 0;
    while (i < n) {
        if (cc_inert_scan_step(&scan, line, n, &i)) continue;
        char c = line[i];
        if (c == '(') paren_depth++;
        else if (c == ')') { if (paren_depth > 0) paren_depth--; }
        else if (c == '[') bracket_depth++;
        else if (c == ']') { if (bracket_depth > 0) bracket_depth--; }
        else if ((c == '{' || c == '}') && paren_depth == 0 && bracket_depth == 0) return 1;
        i++;
    }
    return 0;
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

static char* cc__dup_decl_type_text(const char* ty_s,
                                    const char* ty_e,
                                    unsigned char* out_flags) {
    if (out_flags) *out_flags = 0;
    if (!ty_s || !ty_e || ty_e <= ty_s) return NULL;

    int is_slice = 0;
    int slice_has_bang = 0;
    int is_chan_tx = 0;
    int is_chan_rx = 0;
    int ptr_n = 0;
    for (const char* s = ty_s; s < ty_e; s++) {
        if (*s == '*') ptr_n++;
        if (*s == '[') {
            const char* t = s;
            while (t < ty_e && *t != ']') t++;
            if (t < ty_e) {
                int has_tilde = 0, has_gt = 0, has_lt = 0;
                for (const char* u = s; u < t; u++) {
                    if (*u == ':') is_slice = 1;
                    if (*u == '!') slice_has_bang = 1;
                    if (*u == '~') has_tilde = 1;
                    if (*u == '>') has_gt = 1;
                    if (*u == '<') has_lt = 1;
                }
                if (has_tilde && has_gt) is_chan_tx = 1;
                if (has_tilde && has_lt) is_chan_rx = 1;
            }
        }
    }

    char* ty = NULL;
    if (is_chan_tx) {
        const char* base = "CCChanTx";
        size_t bt = strlen(base);
        ty = (char*)malloc(bt + (size_t)ptr_n + 1);
        if (!ty) return NULL;
        memcpy(ty, base, bt);
        for (int i = 0; i < ptr_n; i++) ty[bt + (size_t)i] = '*';
        ty[bt + (size_t)ptr_n] = '\0';
    } else if (is_chan_rx) {
        const char* base = "CCChanRx";
        size_t bt = strlen(base);
        ty = (char*)malloc(bt + (size_t)ptr_n + 1);
        if (!ty) return NULL;
        memcpy(ty, base, bt);
        for (int i = 0; i < ptr_n; i++) ty[bt + (size_t)i] = '*';
        ty[bt + (size_t)ptr_n] = '\0';
    } else if (is_slice) {
        const char* base = "CCSlice";
        size_t bt = strlen(base);
        ty = (char*)malloc(bt + (size_t)ptr_n + 1);
        if (!ty) return NULL;
        memcpy(ty, base, bt);
        for (int i = 0; i < ptr_n; i++) ty[bt + (size_t)i] = '*';
        ty[bt + (size_t)ptr_n] = '\0';
    } else {
        size_t tn = (size_t)(ty_e - ty_s);
        ty = (char*)malloc(tn + 1);
        if (!ty) return NULL;
        memcpy(ty, ty_s, tn);
        ty[tn] = '\0';
    }

    if (out_flags) {
        unsigned char flags = 0;
        if (strcmp(ty, "CCSlice") == 0) flags |= 1;
        if (is_slice && slice_has_bang) flags |= 2;
        *out_flags = flags;
    }
    return ty;
}

/* Produce a copy of src[0..len) with the contents of line comments, block
 * comments, and string/char literals replaced by spaces (newlines preserved).
 * All offsets remain valid in the returned buffer. Used by text-based decl
 * scanners so that an identifier appearing inside a comment cannot masquerade
 * as a real decl, and the backward "type text" walks cannot pick up tokens
 * from a preceding comment line into the inferred type. */
static char* cc__src_strip_comments_and_strings(const char* src, size_t len) {
    if (!src) return NULL;
    char* out = (char*)malloc(len + 1);
    if (!out) return NULL;
    memcpy(out, src, len);
    out[len] = '\0';
    /* Inert-kind discrimination (Batch F1 pattern): snapshot pre-step
     * inert flags so we can blank consumed [before, i) bytes correctly.
     * Rules:
     *   - newlines preserved (line numbers stay aligned)
     *   - string/char delimiters preserved on entry AND exit transitions
     *     so the surrounding `"`/`'` stay intact
     *   - everything else inside any inert region (comments / string
     *     bodies / char bodies / pp directives) → ASCII space
     *
     * Note: this is a behavior CHANGE for pp directives — the original
     * didn't recognise `#line`/`#define`/etc. as inert, so the `#`
     * leaked through as a code byte (and downstream decl scanners had
     * the early `if (*p == '#' || *p == '\0') return;` guard to bail).
     * Migrated version blanks pp-directive bodies; the guard is now
     * unreachable for those cases (still kept as defense in depth). */
    CCInertScan scan;
    cc_inert_scan_init(&scan, NULL);
    size_t i = 0;
    while (i < len) {
        int pre_str = scan.in_str;
        int pre_chr = scan.in_chr;
        size_t before = i;
        if (!cc_inert_scan_step(&scan, src, len, &i)) {
            i++;
            continue;
        }
        int post_str = scan.in_str;
        int post_chr = scan.in_chr;
        for (size_t k = before; k < i; k++) {
            char c = src[k];
            if (c == '\n') continue;
            /* Preserve string/char delimiters on entry/exit transitions. */
            if (c == '"' && pre_str != post_str) continue;
            if (c == '\'' && pre_chr != post_chr) continue;
            out[k] = ' ';
        }
    }
    return out;
}

static char* cc__lookup_internal_generated_decl_type(const char* src,
                                                     size_t before_off,
                                                     const char* name,
                                                     unsigned char* out_flags) {
    size_t name_len;
    size_t src_len;
    char* clean = NULL;
    char* result = NULL;
    if (out_flags) *out_flags = 0;
    if (!src || !name || !name[0]) return NULL;
    name_len = strlen(name);
    src_len = strlen(src);
    if (before_off > src_len) before_off = src_len;

    /* Operate on a comment/string-stripped copy so that an occurrence of `name`
     * inside a block comment cannot be picked up as a synthetic decl, and the
     * backward walk that builds the "type text" cannot drag tokens out of a
     * preceding comment line into the inferred type. Offsets are preserved. */
    clean = cc__src_strip_comments_and_strings(src, src_len);
    if (!clean) return NULL;

    for (size_t pos = before_off; pos-- > 0; ) {
        if (pos + name_len > before_off) continue;
        if (memcmp(clean + pos, name, name_len) != 0) continue;
        if (pos > 0 && cc__is_ident_char2(clean[pos - 1])) continue;
        if (pos + name_len < src_len && cc__is_ident_char2(clean[pos + name_len])) continue;
        {
            const char* after = clean + pos + name_len;
            while (*after == ' ' || *after == '\t') after++;
            if (*after != '=' && *after != ';' && *after != '[') continue;
        }

        const char* ty_s = clean + pos;
        while (ty_s > clean) {
            char prev = ty_s[-1];
            if (prev == '\n' || prev == ';' || prev == '{' || prev == '}') break;
            ty_s--;
        }
        while (*ty_s == ' ' || *ty_s == '\t') ty_s++;
        const char* ty_e = clean + pos;
        while (ty_e > ty_s && (ty_e[-1] == ' ' || ty_e[-1] == '\t')) ty_e--;
        if (ty_e <= ty_s) continue;
        {
            int has_ident = 0;
            int has_eq = 0;
            int has_member_access = 0;
            int has_expr_punct = 0;
            for (const char* p = ty_s; p < ty_e; p++) {
                if (*p == '=') { has_eq = 1; break; }
                if (*p == '.' || (*p == '-' && p + 1 < ty_e && p[1] == '>'))
                    has_member_access = 1;
                /* A decl's type prefix never contains call/operator
                   punctuation; `f(&name[...]` is an expression, not a decl
                   of `name` (placement-workers4 capture-scanner bug). */
                if (*p == '(' || *p == ')' || *p == '&' || *p == ',')
                    has_expr_punct = 1;
                if (cc__is_ident_start_char(*p)) has_ident = 1;
            }
            if (has_eq || !has_ident || has_member_access || has_expr_punct) continue;
        }
        result = cc__dup_decl_type_text(ty_s, ty_e, out_flags);
        break;
    }
    free(clean);
    return result;
}

static char* cc__lookup_decl_type_by_text_fallback(const char* src,
                                                   size_t before_off,
                                                   const char* name,
                                                   unsigned char* out_flags) {
    if (out_flags) *out_flags = 0;
    if (!src || !name || !name[0]) return NULL;
    size_t src_len = strlen(src);
    if (before_off > src_len) before_off = src_len;
    char* clean = cc__src_strip_comments_and_strings(src, before_off);
    if (!clean) return NULL;
    char** scope_names[2] = {0};
    char** scope_types[2] = {0};
    unsigned char* scope_flags[2] = {0};
    int scope_counts[2] = {0};
    char* decl_carry = NULL;
    char* found_type = NULL;
    unsigned char found_flags = 0;
    size_t off = 0;
    while (off < before_off) {
        size_t line_end = off;
        while (line_end < before_off && clean[line_end] != '\n') line_end++;
        size_t line_len = line_end - off;
        {
            const char* line_s = clean + off;
            const char* line_e = clean + line_end;
            while (line_s < line_e && (*line_s == ' ' || *line_s == '\t' || *line_s == '\r')) line_s++;
            if (line_s < line_e && *line_s == '#') {
                if (decl_carry) decl_carry[0] = '\0';
                off = line_end < before_off && clean[line_end] == '\n' ? line_end + 1 : line_end;
                continue;
            }
        }
        size_t carry_len = decl_carry ? strlen(decl_carry) : 0;
        char* next_carry = (char*)realloc(decl_carry, carry_len + line_len + 2);
        if (!next_carry) {
            free(found_type);
            found_type = NULL;
            break;
        }
        decl_carry = next_carry;
        memcpy(decl_carry + carry_len, clean + off, line_len);
        decl_carry[carry_len + line_len] = '\n';
        decl_carry[carry_len + line_len + 1] = '\0';
        cc__maybe_record_decl(scope_names, scope_types, scope_flags, scope_counts, 1, decl_carry);
        {
            const char* ty = scope_counts[1] > 0
                           ? cc__lookup_decl_type(scope_names[1], scope_types[1], scope_counts[1], name)
                           : NULL;
            if (ty) {
                char* next_found = strdup(ty);
                if (next_found) {
                    free(found_type);
                    found_type = next_found;
                    found_flags = cc__lookup_decl_flags(scope_names[1], scope_flags[1], scope_counts[1], name);
                }
            }
        }
        {
            size_t consumed = cc__last_top_level_semi_offset(decl_carry);
            if (consumed > 0) {
                size_t remain = strlen(decl_carry + consumed);
                memmove(decl_carry, decl_carry + consumed, remain + 1);
            } else if (cc__has_top_level_brace(decl_carry)) {
                decl_carry[0] = '\0';
            }
        }
        off = line_end < before_off && clean[line_end] == '\n' ? line_end + 1 : line_end;
    }
    for (int i = 0; i < scope_counts[1]; i++) free(scope_names[1][i]);
    free(scope_names[1]);
    for (int i = 0; i < scope_counts[1]; i++) free(scope_types[1][i]);
    free(scope_types[1]);
    free(scope_flags[1]);
    free(decl_carry);
    free(clean);
    if (found_type && out_flags) *out_flags = found_flags;
    return found_type;
}

static char* cc__lookup_top_level_decl_type_by_text(const char* src,
                                                    size_t before_off,
                                                    const char* name,
                                                    unsigned char* out_flags) {
    char** scope_names[1] = {0};
    char** scope_types[1] = {0};
    unsigned char* scope_flags[1] = {0};
    int scope_counts[1] = {0};
    char* decl_carry = NULL;
    char* found_type = NULL;
    unsigned char found_flags = 0;
    int depth = 0;
    size_t off = 0;

    if (out_flags) *out_flags = 0;
    if (!src || !name || !name[0]) return NULL;
    if (before_off > strlen(src)) before_off = strlen(src);
    char* clean = cc__src_strip_comments_and_strings(src, before_off);
    if (!clean) return NULL;

    while (off < before_off) {
        size_t line_end = off;
        while (line_end < before_off && clean[line_end] != '\n') line_end++;
        size_t line_len = line_end - off;
        {
            const char* line_s = clean + off;
            const char* line_e = clean + line_end;
            while (line_s < line_e && (*line_s == ' ' || *line_s == '\t' || *line_s == '\r')) line_s++;
            if (line_s < line_e && *line_s == '#') {
                if (decl_carry) decl_carry[0] = '\0';
                off = line_end < before_off && clean[line_end] == '\n' ? line_end + 1 : line_end;
                continue;
            }
        }
        if (depth == 0) {
            size_t carry_len = decl_carry ? strlen(decl_carry) : 0;
            char* next_carry = (char*)realloc(decl_carry, carry_len + line_len + 2);
            if (!next_carry) {
                free(found_type);
                found_type = NULL;
                break;
            }
            decl_carry = next_carry;
            memcpy(decl_carry + carry_len, clean + off, line_len);
            decl_carry[carry_len + line_len] = '\n';
            decl_carry[carry_len + line_len + 1] = '\0';
            cc__maybe_record_decl(scope_names, scope_types, scope_flags, scope_counts, 0, decl_carry);
            {
                const char* ty = scope_counts[0] > 0
                               ? cc__lookup_decl_type(scope_names[0], scope_types[0], scope_counts[0], name)
                               : NULL;
                if (ty) {
                    char* next_found = strdup(ty);
                    if (next_found) {
                        free(found_type);
                        found_type = next_found;
                        found_flags = cc__lookup_decl_flags(scope_names[0], scope_flags[0], scope_counts[0], name);
                    }
                }
            }
            {
                size_t consumed = cc__last_top_level_semi_offset(decl_carry);
                if (consumed > 0) {
                    size_t remain = strlen(decl_carry + consumed);
                    memmove(decl_carry, decl_carry + consumed, remain + 1);
                } else if (cc__has_top_level_brace(decl_carry)) {
                    decl_carry[0] = '\0';
                }
            }
        }

        for (size_t i = off; i < line_end; i++) {
            if (clean[i] == '{') depth++;
            else if (clean[i] == '}' && depth > 0) depth--;
        }
        off = line_end < before_off && clean[line_end] == '\n' ? line_end + 1 : line_end;
    }

    for (int i = 0; i < scope_counts[0]; i++) free(scope_names[0][i]);
    free(scope_names[0]);
    for (int i = 0; i < scope_counts[0]; i++) free(scope_types[0][i]);
    free(scope_types[0]);
    free(scope_flags[0]);
    free(decl_carry);
    free(clean);
    if (found_type && out_flags) *out_flags = found_flags;
    return found_type;
}

/* Fallback: look up a captured name in the nearest function signature. */

static void cc__collect_caps_from_block(char*** scope_names,
                                        int* scope_counts,
                                        const char* src,
                                        size_t before_off,
                                        int max_depth,
                                        const char* block,
                                        const char* ignore_name0,
                                        const char* ignore_name1,
                                        char* const* local_decl_names,
                                        int local_decl_count,
                                        char*** out_caps,
                                        int* out_cap_count) {
    if (!scope_names || !scope_counts || !block || !out_caps || !out_cap_count) return;
    const char* p = block;
    char** scan_local_names = NULL;
    int scan_local_count = 0;
    while (*p) {
        if (*p == '/' && p[1] == '/') {
            p += 2;
            while (*p && *p != '\n') p++;
            continue;
        }
        if (*p == '/' && p[1] == '*') {
            p += 2;
            while (*p && !(p[0] == '*' && p[1] == '/')) p++;
            if (*p) p += 2;
            continue;
        }
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
        if (cc__skip_generated_name_for_capture_scan(s, n)) continue;
        if (cc__looks_like_macro_constant(s, n)) continue;
        if (ignore_name0 && strlen(ignore_name0) == n && strncmp(ignore_name0, s, n) == 0) continue;
        if (ignore_name1 && strlen(ignore_name1) == n && strncmp(ignore_name1, s, n) == 0) continue;
        if (cc__name_in_list(local_decl_names, local_decl_count, s, n)) continue;
        if (cc__name_in_list(scan_local_names, scan_local_count, s, n)) continue;
        {
            const char* prev = s;
            const char* next = p;
            while (prev > block && (prev[-1] == ' ' || prev[-1] == '\t' || prev[-1] == '\n' || prev[-1] == '\r')) {
                prev--;
            }
            while (*next == ' ' || *next == '\t' || *next == '\n' || *next == '\r') next++;
            if (prev > block && (prev[-1] == '.' || (prev[-1] == '>' && prev > block + 1 && prev[-2] == '-'))) {
                continue;
            }
            /* Skip type-position tokens in local declarations / casts, e.g.
               `Resource r;`, `Item* x = ...`, `MyData*!>(Err) res = ...`, `(Item*)p`. */
            if (*next == '*' || *next == '[' || *next == '!') {
                continue;
            }
            {
                int prev_decl_like = 0;
                if (prev > block && cc__is_ident_char2(prev[-1])) {
                    prev_decl_like = 1;
                } else if (prev > block && prev[-1] == '*') {
                    const char* outer = prev - 1;
                    while (outer > block && (outer[-1] == ' ' || outer[-1] == '\t' ||
                                             outer[-1] == '\n' || outer[-1] == '\r')) {
                        outer--;
                    }
                    if (outer > block &&
                        (cc__is_ident_char2(outer[-1]) || outer[-1] == '*' || outer[-1] == ']')) {
                        prev_decl_like = 1;
                    }
                } else if (prev > block && (prev[-1] == ']' || prev[-1] == ')')) {
                    prev_decl_like = 1;
                }
            if ((*next == ';' || *next == '=' || *next == '[') &&
                prev_decl_like) {
                if (!cc__name_in_list(scan_local_names, scan_local_count, s, n)) {
                    char* nm = (char*)malloc(n + 1);
                    char** next_names = NULL;
                    if (nm) {
                        memcpy(nm, s, n);
                        nm[n] = '\0';
                        next_names = (char**)realloc(scan_local_names, (size_t)(scan_local_count + 1) * sizeof(char*));
                        if (next_names) {
                            scan_local_names = next_names;
                            scan_local_names[scan_local_count++] = nm;
                        } else {
                            free(nm);
                        }
                    }
                }
                continue;
            }
            }
            if ((prev > block && prev[-1] == '(') && *next == ')') {
                const char* outer = prev - 1;
                while (outer > block && (outer[-1] == ' ' || outer[-1] == '\t' ||
                                         outer[-1] == '\n' || outer[-1] == '\r')) {
                    outer--;
                }
                if (outer == block || !cc__is_ident_char2(outer[-1])) {
                    continue;
                }
            }
            if (cc__is_ident_start_char(*next)) {
                if (prev == s || prev[-1] == ';' || prev[-1] == '{' || prev[-1] == '}' ||
                    prev[-1] == '\n' || prev[-1] == '\r' || prev[-1] == '(') {
                    continue;
                }
            }
        }
        int found = 0;
        for (int d = max_depth; d >= 1 && !found; d--) {
            if (cc__name_in_list(scope_names[d], scope_counts[d], s, n)) found = 1;
        }
        if (!found && src && before_off > 0) {
            char probe[128];
            unsigned char probe_flags = 0;
            char* probe_ty = NULL;
            if (n < sizeof(probe)) {
                memcpy(probe, s, n);
                probe[n] = '\0';
                probe_ty = cc__lookup_decl_type_by_text_fallback(src, before_off, probe, &probe_flags);
                if (probe_ty && !cc__capture_type_text_usable(probe_ty, probe)) {
                    free(probe_ty);
                    probe_ty = NULL;
                }
                if (!probe_ty) probe_ty = cc__lookup_internal_generated_decl_type(src, before_off, probe, &probe_flags);
                if (probe_ty && !cc__capture_type_text_usable(probe_ty, probe)) {
                    free(probe_ty);
                    probe_ty = NULL;
                }
                if (probe_ty) {
                    found = 1;
                    free(probe_ty);
                }
            }
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
    for (int i = 0; i < scan_local_count; i++) free(scan_local_names[i]);
    free(scan_local_names);
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

static void cc__free_string_list(char** xs, int n) {
    if (!xs) return;
    for (int i = 0; i < n; i++) free(xs[i]);
    free(xs);
}

static void cc__collect_decl_names_from_block_text(const char* block,
                                                   char*** out_names,
                                                   int* out_n) {
    char** scope_names[2] = {0};
    char** scope_types[2] = {0};
    unsigned char* scope_flags[2] = {0};
    int scope_counts[2] = {0};
    char* decl_carry = NULL;
    size_t off = 0;
    const char* scan = block;
    size_t scan_len = strlen(block);
    if (!out_names || !out_n) return;
    *out_names = NULL;
    *out_n = 0;
    if (!block) return;
    {
        size_t lo = 0, hi = scan_len;
        while (lo < hi && (scan[lo] == ' ' || scan[lo] == '\t' || scan[lo] == '\r' || scan[lo] == '\n')) lo++;
        while (hi > lo && (scan[hi - 1] == ' ' || scan[hi - 1] == '\t' || scan[hi - 1] == '\r' || scan[hi - 1] == '\n')) hi--;
        if (hi > lo && scan[lo] == '{' && scan[hi - 1] == '}') {
            scan += lo + 1;
            scan_len = hi - lo - 2;
        }
    }
    char* scan_clean = cc__src_strip_comments_and_strings(scan, scan_len);
    if (!scan_clean) return;
    while (off < scan_len) {
        size_t line_end = off;
        while (line_end < scan_len && scan_clean[line_end] != '\n') line_end++;
        size_t line_len = line_end - off;
        size_t carry_len = decl_carry ? strlen(decl_carry) : 0;
        char* next_carry = (char*)realloc(decl_carry, carry_len + line_len + 2);
        if (!next_carry) break;
        decl_carry = next_carry;
        memcpy(decl_carry + carry_len, scan_clean + off, line_len);
        decl_carry[carry_len + line_len] = '\n';
        decl_carry[carry_len + line_len + 1] = '\0';
        cc__maybe_record_decl(scope_names, scope_types, scope_flags, scope_counts, 1, decl_carry);
        {
            size_t consumed = cc__last_top_level_semi_offset(decl_carry);
            if (consumed > 0) {
                size_t remain = strlen(decl_carry + consumed);
                memmove(decl_carry, decl_carry + consumed, remain + 1);
            } else if (cc__has_top_level_brace(decl_carry)) {
                decl_carry[0] = '\0';
            }
        }
        off = (line_end < scan_len && scan_clean[line_end] == '\n') ? line_end + 1 : line_end;
    }
    free(scan_clean);
    if (scope_counts[1] > 0) {
        *out_names = cc__dup_string_list(scope_names[1], scope_counts[1]);
        *out_n = *out_names ? scope_counts[1] : 0;
    }
    for (int d = 0; d < 2; d++) {
        for (int i = 0; i < scope_counts[d]; i++) free(scope_names[d][i]);
        free(scope_names[d]);
        for (int i = 0; i < scope_counts[d]; i++) free(scope_types[d][i]);
        free(scope_types[d]);
        free(scope_flags[d]);
    }
    free(decl_carry);
}

typedef struct {
    char* name;
    char** param_types;
    char** param_names;  /* Parameter names for closure capture */
    int param_count;
    int line_start;      /* Line where function body starts */
    int line_end;        /* Line where function body ends */
} CCFuncSig;

static const char* cc__lookup_param_type_in_sig(const CCFuncSig* sig, const char* name);

/* Fallback: look up a captured name in the nearest function signature. */
static const char* cc__lookup_param_type_for_closure(const CCFuncSig* sigs,
                                                     int sig_n,
                                                     const char* name,
                                                     int closure_line) {
    if (!sigs || sig_n <= 0 || !name || closure_line <= 0) return NULL;
    int best_idx = -1;
    int best_line = -1;
    for (int i = 0; i < sig_n; i++) {
        if (!sigs[i].name || sigs[i].line_start <= 0) continue;
        if (sigs[i].line_start <= closure_line && sigs[i].line_start > best_line) {
            best_line = sigs[i].line_start;
            best_idx = i;
        }
    }
    if (best_idx < 0) return NULL;
    for (int p = 0; p < sigs[best_idx].param_count; p++) {
        if (!sigs[best_idx].param_names || !sigs[best_idx].param_types) break;
        const char* nm = sigs[best_idx].param_names[p];
        const char* ty = sigs[best_idx].param_types[p];
        if (nm && ty && strcmp(nm, name) == 0) return ty;
    }
    return NULL;
}

static const char* cc__lookup_param_type_by_src(const CCFuncSig* sigs,
                                                int sig_n,
                                                const char* src,
                                                size_t closure_off,
                                                const char* param_name) {
    if (!sigs || sig_n <= 0 || !src || !param_name) return NULL;
    const CCFuncSig* best_sig = NULL;
    size_t best_off = 0;
    /* Only search the region before `closure_off` — the goal is to
     * find the most recent call to one of `sigs[*]` whose bounded
     * argument position would supply the closure's param types. */
    size_t scan_end = closure_off;
    for (int i = 0; i < sig_n; i++) {
        if (!sigs[i].name) continue;
        size_t name_len = strlen(sigs[i].name);
        if (name_len == 0) continue;
        size_t last_off = (size_t)-1;
        size_t scan_pos = 0;
        while (scan_pos < scan_end) {
            /* Comment/string-aware, word-bounded identifier find —
             * pre-metaclass this used `strstr` which would match
             * `sigs[i].name` inside block comments and string
             * literals (e.g. `/ * calls foo(x) * /`) and pull the
             * "last call" offset into dead text.  See util/text.h. */
            size_t hit = cc_find_ident_top_level(src, scan_pos, scan_end,
                                                 sigs[i].name, name_len);
            if (hit >= scan_end) break;
            size_t after = hit + name_len;
            const char* q = src + after;
            while (after < scan_end && (*q == ' ' || *q == '\t')) { q++; after++; }
            if (after < scan_end && *q == '(') {
                last_off = hit;
            }
            scan_pos = hit + name_len;
        }
        if (last_off != (size_t)-1) {
            if (last_off >= best_off) {
                best_off = last_off;
                best_sig = &sigs[i];
            }
        }
    }
    return cc__lookup_param_type_in_sig(best_sig, param_name);
}

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

static const char* cc__lookup_param_type_in_sig(const CCFuncSig* sig, const char* name) {
    if (!sig || !name) return NULL;
    for (int p = 0; p < sig->param_count; p++) {
        if (!sig->param_names || !sig->param_types) break;
        const char* nm = sig->param_names[p];
        const char* ty = sig->param_types[p];
        if (nm && ty && strcmp(nm, name) == 0) return ty;
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

/* Check if a type string represents a safe wrapper that allows mutation in
   reference captures. Safe wrappers: @atomic T, Atomic<T>, cc_atomic_*,
   Mutex<T>, CCChan*, CCChanTx, CCChanRx. */
static int cc__is_safe_wrapper_type(const char* ty) {
    if (!ty) return 0;
    /* Skip leading whitespace */
    while (*ty == ' ' || *ty == '\t') ty++;
    /* @atomic prefix */
    if (strncmp(ty, "@atomic", 7) == 0 && (ty[7] == ' ' || ty[7] == '\t' || ty[7] == '\0')) return 1;
    /* Atomic<T> */
    if (strncmp(ty, "Atomic<", 7) == 0) return 1;
    /* Concrete C atomic typedefs from cc_atomic.cch */
    if (strncmp(ty, "cc_atomic_", 10) == 0) return 1;
    if (strncmp(ty, "_Atomic ", 8) == 0 || strncmp(ty, "_Atomic(", 8) == 0) return 1;
    /* Mutex<T> */
    if (strncmp(ty, "Mutex<", 6) == 0) return 1;
    /* CCChan* */
    if (strncmp(ty, "CCChan", 6) == 0) return 1;
    /* CCChanTx, CCChanRx */
    if (strncmp(ty, "CCChanTx", 8) == 0 || strncmp(ty, "CCChanRx", 8) == 0) return 1;
    return 0;
}

/* Registered synchronization library capture (spec §6.1): capturing an
   exclusive domain / mutex / guard into a task closure allows sibling
   reference-capture mutation under that library's protocol. Unlike
   Mutex<T>/atomics, the protected data is a separate capture. */
static int cc__is_registered_sync_capture_type(const char* ty) {
    if (!ty) return 0;
    while (*ty == ' ' || *ty == '\t') ty++;
    if (strncmp(ty, "const ", 6) == 0) {
        ty += 6;
        while (*ty == ' ' || *ty == '\t') ty++;
    }
    if (strncmp(ty, "CCExclusiveMutex", 16) == 0) return 1;
    if (strncmp(ty, "CCExclusiveGuard", 16) == 0) return 1;
    if (strncmp(ty, "CCExclusive", 11) == 0) {
        const char* p = ty + 11;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '*') return 1;
    }
    return 0;
}

typedef enum {
    CC_MUT_NONE = 0,
    CC_MUT_WRITE = 1,
    CC_MUT_ADDR_OF_ESCAPES = 2,
    CC_MUT_ADDR_OF_NONCONST_CALL = 3,
} CCMutationKind;

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
    {
        CCInertScan scan;
        cc_inert_scan_init(&scan, NULL);
        scan.at_line_start = 0;  /* mid-body slice */
        size_t lim = (amp_off < n) ? amp_off : n;
        size_t i = lp + 1;
        while (i < lim) {
            if (cc_inert_scan_step(&scan, body, n, &i)) continue;
            char c = body[i];
            if (c == '(') p++;
            else if (c == ')' && p > 0) p--;
            else if (c == '{') b++;
            else if (c == '}' && b > 0) b--;
            else if (c == '[') sq++;
            else if (c == ']' && sq > 0) sq--;
            else if (c == ',' && p == 0 && b == 0 && sq == 0) argi++;
            i++;
        }
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

/* True if the identifier ending at `end` (exclusive) is a task/thread escape
 * callee: surface `spawn` / `spawn_async` / `spawn_thread`, lowered
 * `cc_nursery_spawn_closure0` / `spawnhybrid*`, or `*_send_task`. */
static int cc__ident_is_task_escape_callee(const char* src, size_t end) {
    if (!src || end == 0) return 0;
    size_t start = end;
    while (start > 0 && cc__is_ident_char2(src[start - 1])) start--;
    if (start == end) return 0;
    /* "spawn" as an ident segment: spawn, spawn_async, cc_nursery_spawn_closure0, … */
    for (size_t i = start; i + 5 <= end; i++) {
        if (memcmp(src + i, "spawn", 5) != 0) continue;
        if (i > start && src[i - 1] != '_') continue;
        size_t after = i + 5;
        if (after == end || src[after] == '_' ||
            (src[after] >= '0' && src[after] <= '9')) {
            return 1;
        }
    }
    /* Channel task enqueue: send_task / cc_channel_send_task / … */
    if (end >= start + 9 && memcmp(src + end - 9, "send_task", 9) == 0) return 1;
    return 0;
}

/* Callee name ending at `end` (exclusive) is send_into / try_send_into,
 * including cc_channel_* forms. */
static int cc__callee_is_send_into_name(const char* src, size_t end) {
    if (!src || end < 9) return 0;
    if (memcmp(src + end - 9, "send_into", 9) != 0) return 0;
    size_t s = end - 9;
    if (s == 0) return 1;
    char p = src[s - 1];
    /* `.send_into` / whitespace, or `*_send_into` / `*try_send_into` */
    return !cc__is_ident_char2(p) || p == '_';
}

static size_t cc__skip_ws_left(const char* src, size_t i) {
    while (i > 0 && (src[i - 1] == ' ' || src[i - 1] == '\t' ||
                     src[i - 1] == '\n' || src[i - 1] == '\r')) i--;
    return i;
}

static size_t cc__skip_ws_right(const char* src, size_t n, size_t i) {
    while (i < n && (src[i] == ' ' || src[i] == '\t' ||
                     src[i] == '\n' || src[i] == '\r')) i++;
    return i;
}

/* Scan a primary receiver expression leftward ending at `end` (exclusive):
 * ident / . / -> chains (and a leading &). Returns start offset, or end on fail. */
static size_t cc__recv_expr_start(const char* src, size_t end) {
    size_t i = cc__skip_ws_left(src, end);
    if (i == 0) return end;
    for (;;) {
        if (i == 0 || !cc__is_ident_char2(src[i - 1])) return end;
        while (i > 0 && cc__is_ident_char2(src[i - 1])) i--;
        size_t j = cc__skip_ws_left(src, i);
        if (j >= 2 && src[j - 1] == '>' && src[j - 2] == '-') {
            i = cc__skip_ws_left(src, j - 2);
            continue;
        }
        if (j >= 1 && src[j - 1] == '.') {
            i = cc__skip_ws_left(src, j - 1);
            continue;
        }
        if (j >= 1 && src[j - 1] == '&') return j - 1;
        return i;
    }
}

/* Fill missing builder param types from a typed channel send_into call:
 * slot -> T*, arena -> CCArena*.  Explicit annotations are left alone.
 * (Takes type out-params so it can live above CCClosureDesc's definition.) */
static void cc__infer_send_into_builder_param_types(const char* src, size_t len,
                                                    size_t start_off,
                                                    int param_count,
                                                    char** param0_type,
                                                    char** param1_type) {
    if (!src || !param0_type || !param1_type || param_count != 2) return;
    if (*param0_type && *param1_type) return;

    size_t i = start_off;
    int paren = 0;
    while (i > 0) {
        i--;
        char c = src[i];
        if (c == ')') { paren++; continue; }
        if (c == '(') {
            if (paren > 0) { paren--; continue; }

            size_t callee_end = cc__skip_ws_left(src, i);
            if (callee_end == 0 || !cc__is_ident_char2(src[callee_end - 1])) continue;
            size_t callee_start = callee_end;
            while (callee_start > 0 && cc__is_ident_char2(src[callee_start - 1])) callee_start--;
            if (!cc__callee_is_send_into_name(src, callee_end)) continue;

            char recv_buf[256];
            const char* handle_ty = NULL;
            size_t before = cc__skip_ws_left(src, callee_start);
            int is_method = (before > 0 && src[before - 1] == '.');

            if (is_method) {
                /* recv.try_send_into( builder, arena ) — closure is arg0 */
                size_t recv_end = before - 1;
                size_t recv_start = cc__recv_expr_start(src, recv_end);
                if (recv_start >= recv_end || recv_end - recv_start >= sizeof(recv_buf)) continue;
                memcpy(recv_buf, src + recv_start, recv_end - recv_start);
                recv_buf[recv_end - recv_start] = 0;
            } else {
                /* cc_channel_try_send_into( tx, builder, arena ) — closure is arg1 */
                size_t arg0_a = cc__skip_ws_right(src, len, i + 1);
                size_t p = arg0_a;
                int depth = 0;
                while (p < start_off) {
                    char ch = src[p];
                    if (ch == '(' || ch == '[' || ch == '{') depth++;
                    else if (ch == ')' || ch == ']' || ch == '}') {
                        if (depth == 0) break;
                        depth--;
                    } else if (ch == ',' && depth == 0) break;
                    p++;
                }
                if (p >= start_off || src[p] != ',' || p <= arg0_a) continue;
                size_t arg0_b = cc__skip_ws_left(src, p);
                if (arg0_b <= arg0_a || arg0_b - arg0_a >= sizeof(recv_buf)) continue;
                memcpy(recv_buf, src + arg0_a, arg0_b - arg0_a);
                recv_buf[arg0_b - arg0_a] = 0;
            }

            {
                CCTypeRegistry* reg = cc_type_registry_get_global();
                const char* field_ty = NULL;
                const char* elem = NULL;
                if (!reg) return;
                /* Trailing field on `conn->reply_tx`: keep the raw field typedef
                 * (ReplyTx).  Receiver resolve alias-normalizes that to bare
                 * CCChanTx and drops the element type. */
                {
                    const char* p = recv_buf + strlen(recv_buf);
                    while (p > recv_buf && (p[-1] == ' ' || p[-1] == '\t')) p--;
                    const char* field_end = p;
                    while (p > recv_buf && cc__is_ident_char2(p[-1])) p--;
                    const char* field_start = p;
                    while (p > recv_buf && (p[-1] == ' ' || p[-1] == '\t')) p--;
                    int is_field = 0;
                    if (p > recv_buf && p[-1] == '.') is_field = 1;
                    else if (p >= recv_buf + 2 && p[-1] == '>' && p[-2] == '-') is_field = 1;
                    if (is_field && field_end > field_start) {
                        char field[128];
                        size_t fl = (size_t)(field_end - field_start);
                        if (fl < sizeof(field)) {
                            memcpy(field, field_start, fl);
                            field[fl] = 0;
                            field_ty = cc_type_registry_lookup_unique_field_type(reg, field);
                        }
                    }
                }
                handle_ty = cc_type_registry_resolve_receiver_expr_at(reg, recv_buf, src, start_off, NULL);
                if (!handle_ty) handle_ty = cc_type_registry_resolve_receiver_expr(reg, recv_buf, NULL);
                if (!handle_ty) handle_ty = cc_type_registry_lookup_var(reg, recv_buf);
                if (!handle_ty) handle_ty = field_ty;

                /* Bare CCChanTx/CCChanRx (local decl after rewrite, or typedef
                 * alias normalize) loses the element type. Recover via the
                 * typed var registration (tx → CCChanTx_<Elem>) or the field
                 * typedef name (ReplyTx → same). */
                {
                    int bare_chan =
                        handle_ty &&
                        ((strcmp(handle_ty, "CCChanTx") == 0) ||
                         (strcmp(handle_ty, "CCChanRx") == 0));
                    if (bare_chan) {
                        const char* typed = cc_type_registry_lookup_var(reg, recv_buf);
                        if (typed &&
                            (strncmp(typed, "CCChanTx_", 9) == 0 ||
                             strncmp(typed, "CCChanRx_", 9) == 0)) {
                            handle_ty = typed;
                        } else if (field_ty) {
                            handle_ty = field_ty;
                        }
                    }
                }

                elem = cc_type_registry_lookup_channel_elem_type(reg, handle_ty);
                if (!elem && handle_ty) {
                    const char* via_var = cc_type_registry_lookup_var(reg, handle_ty);
                    if (via_var) elem = cc_type_registry_lookup_channel_elem_type(reg, via_var);
                }
                if (!elem && field_ty && field_ty != handle_ty) {
                    elem = cc_type_registry_lookup_channel_elem_type(reg, field_ty);
                    if (!elem) {
                        const char* via_var = cc_type_registry_lookup_var(reg, field_ty);
                        if (via_var) elem = cc_type_registry_lookup_channel_elem_type(reg, via_var);
                    }
                }
                if (!elem && handle_ty) {
                    const char* aliased = cc_type_registry_lookup_alias(reg, handle_ty);
                    if (aliased) {
                        elem = cc_type_registry_lookup_channel_elem_type(reg, aliased);
                        if (!elem) {
                            const char* via_var = cc_type_registry_lookup_var(reg, aliased);
                            if (via_var) elem = cc_type_registry_lookup_channel_elem_type(reg, via_var);
                        }
                    }
                }
                if (!elem || !elem[0]) return;

                if (!*param0_type) {
                    size_t el = strlen(elem);
                    char* ty = (char*)malloc(el + 2);
                    if (!ty) return;
                    memcpy(ty, elem, el);
                    ty[el] = '*';
                    ty[el + 1] = 0;
                    *param0_type = ty;
                }
                if (!*param1_type) {
                    *param1_type = strdup("CCArena*");
                }
            }
            return;
        }
        if (paren == 0 && (c == ';' || c == '{' || c == '}')) return;
    }
}

/* True if this closure literal escapes onto a task/thread (spawn/async/
 * send_task), surface or already-lowered. Pointer-alias mutation is a
 * SHAPE-T7 concurrent smuggle; sync `CCClosureN` stores/calls may still
 * write through `T* p = &local`. */
static int cc__closure_in_task_escape_arg(const char* src, size_t len, size_t start_off) {
    if (!src || start_off > len) return 0;
    size_t i = start_off;
    int paren = 0;
    while (i > 0) {
        i--;
        char c = src[i];
        if (c == ')') {
            paren++;
            continue;
        }
        if (c == '(') {
            if (paren > 0) {
                paren--;
                continue;
            }
            /* Callee immediately before '('. */
            size_t j = i;
            while (j > 0 && (src[j - 1] == ' ' || src[j - 1] == '\t' ||
                             src[j - 1] == '\n' || src[j - 1] == '\r')) j--;
            if (cc__ident_is_task_escape_callee(src, j)) return 1;
            /* Keep scanning: spawn(helper(() => …)) still escapes. */
            continue;
        }
        if (paren == 0 && (c == ';' || c == '{' || c == '}')) return 0;
    }
    return 0;
}

/* Returns non-zero if the body writes through pointer `var_name`
 * (`*p =`, `(*p)++`, `p->field =`, …). Used for value-captured aliases. */
static int cc__find_ptr_pointee_mutation_in_body(const char* body,
                                                 const char* var_name,
                                                 size_t* out_offset) {
    if (!body || !var_name || !var_name[0]) return 0;
    size_t var_len = strlen(var_name);
    size_t body_len = strlen(body);
    CCInertScan scan;
    cc_inert_scan_init(&scan, NULL);
    size_t i = 0;
    while (i < body_len) {
        if (cc_inert_scan_step(&scan, body, body_len, &i)) continue;

        /* ++*p / --*p */
        if (i + 2 + var_len <= body_len &&
            ((body[i] == '+' && body[i + 1] == '+') ||
             (body[i] == '-' && body[i + 1] == '-'))) {
            size_t j = i + 2;
            while (j < body_len && (body[j] == ' ' || body[j] == '\t')) j++;
            if (j < body_len && body[j] == '*') {
                j++;
                while (j < body_len && (body[j] == ' ' || body[j] == '\t')) j++;
                if (j + var_len <= body_len && strncmp(body + j, var_name, var_len) == 0) {
                    char after = (j + var_len < body_len) ? body[j + var_len] : 0;
                    if (!cc__is_ident_char2(after)) {
                        if (out_offset) *out_offset = i;
                        return 1;
                    }
                }
            }
        }

        if (!cc__is_ident_start_char(body[i])) { i++; continue; }
        if (i > 0 && cc__is_ident_char2(body[i - 1])) { i++; continue; }
        if (i + var_len > body_len || strncmp(body + i, var_name, var_len) != 0) {
            i++;
            continue;
        }
        char after = (i + var_len < body_len) ? body[i + var_len] : 0;
        if (cc__is_ident_char2(after)) { i++; continue; }

        /* Skip if this is a field name: foo.p or foo->p */
        if (i >= 2) {
            size_t k = i - 1;
            while (k > 0 && (body[k] == ' ' || body[k] == '\t')) k--;
            if (body[k] == '.' || (body[k] == '>' && k > 0 && body[k - 1] == '-')) {
                i++;
                continue;
            }
        }

        /* *p  (pointee write) — look for unary * before the name */
        if (i > 0) {
            size_t k = i - 1;
            while (k > 0 && (body[k] == ' ' || body[k] == '\t')) k--;
            if (body[k] == '*') {
                /* not multiply: previous non-space before * should not be ident/)/] */
                size_t m = k;
                if (m > 0) {
                    m--;
                    while (m > 0 && (body[m] == ' ' || body[m] == '\t')) m--;
                    char prev = body[m];
                    if (cc__is_ident_char2(prev) || prev == ')' || prev == ']' || prev == '\'') {
                        /* likely binary * — skip */
                    } else {
                        size_t j = i + var_len;
                        while (j < body_len && (body[j] == ' ' || body[j] == '\t')) j++;
                        if (j + 1 < body_len &&
                            ((body[j] == '+' && body[j + 1] == '+') ||
                             (body[j] == '-' && body[j + 1] == '-'))) {
                            if (out_offset) *out_offset = k;
                            return 1;
                        }
                        if (j < body_len && body[j] == '=' &&
                            (j + 1 >= body_len || body[j + 1] != '=')) {
                            if (out_offset) *out_offset = k;
                            return 1;
                        }
                        if (j + 1 < body_len && body[j + 1] == '=' &&
                            (body[j] == '+' || body[j] == '-' || body[j] == '*' ||
                             body[j] == '/' || body[j] == '%' || body[j] == '&' ||
                             body[j] == '|' || body[j] == '^')) {
                            if (out_offset) *out_offset = k;
                            return 1;
                        }
                    }
                } else {
                    size_t j = i + var_len;
                    while (j < body_len && (body[j] == ' ' || body[j] == '\t')) j++;
                    if (j < body_len && body[j] == '=' &&
                        (j + 1 >= body_len || body[j + 1] != '=')) {
                        if (out_offset) *out_offset = k;
                        return 1;
                    }
                }
            }
        }

        /* p->field = … */
        {
            size_t j = i + var_len;
            while (j < body_len && (body[j] == ' ' || body[j] == '\t')) j++;
            if (j + 1 < body_len && body[j] == '-' && body[j + 1] == '>') {
                j += 2;
                while (j < body_len && (body[j] == ' ' || body[j] == '\t')) j++;
                if (j < body_len && cc__is_ident_start_char(body[j])) {
                    while (j < body_len && cc__is_ident_char2(body[j])) j++;
                    while (j < body_len && (body[j] == ' ' || body[j] == '\t')) j++;
                    if (j + 1 < body_len &&
                        ((body[j] == '+' && body[j + 1] == '+') ||
                         (body[j] == '-' && body[j + 1] == '-'))) {
                        if (out_offset) *out_offset = i;
                        return 1;
                    }
                    if (j < body_len && body[j] == '=' &&
                        (j + 1 >= body_len || body[j + 1] != '=')) {
                        if (out_offset) *out_offset = i;
                        return 1;
                    }
                    if (j + 1 < body_len && body[j + 1] == '=' &&
                        (body[j] == '+' || body[j] == '-' || body[j] == '*' ||
                         body[j] == '/' || body[j] == '%' || body[j] == '&' ||
                         body[j] == '|' || body[j] == '^')) {
                        if (out_offset) *out_offset = i;
                        return 1;
                    }
                }
            }
        }

        i++;
    }
    return 0;
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

    CCInertScan scan;
    cc_inert_scan_init(&scan, NULL);
    size_t i = 0;
    while (i < body_len) {
        if (cc_inert_scan_step(&scan, body, body_len, &i)) continue;

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
        if (!cc__is_ident_start_char(body[i])) { i++; continue; }
        if (i > 0 && cc__is_ident_char2(body[i-1])) { i++; continue; }
        if (i + var_len > body_len) { i++; continue; }
        if (strncmp(body + i, var_name, var_len) != 0) { i++; continue; }
        char after = (i + var_len < body_len) ? body[i + var_len] : 0;
        if (cc__is_ident_char2(after)) { i++; continue; }
        /* Skip struct field accesses: ptr->field or obj.field */
        if (i >= 2) {
            size_t k = i - 1;
            while (k > 0 && (body[k] == ' ' || body[k] == '\t')) k--;
            if (body[k] == '.' || (body[k] == '>' && k > 0 && body[k-1] == '-')) { i++; continue; }
        }

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

        /* Advance past the matched identifier; original used `i = j - 1`
         * relying on for's `++i` to land at `j`.  In the while form,
         * just set `i = j` directly. */
        i = j;
    }
    return 0;
}

typedef struct {
    int id;
    /* `sym_base` is the location-tagged base symbol used for every emitted
     * closure helper (entry / make / make_nursery / env / env_drop /
     * env_nursery_drop). It comes from `cc_diag_mangle_symbol` and takes the
     * form `cc_closure__N<id>__line<L>_col<C>` — so any name appearing in a
     * crash backtrace or symbolicated profile is immediately self-locating
     * (sequence #, source line, source column). Two closures expanded by the
     * same macro at identical line/col still get unique names via the N<id>
     * sequence tiebreaker. Populated once per descriptor build; freed in
     * cc__free_closure_desc. The legacy `__cc_closure_<role>_<id>` integer
     * naming was retired 2026-05-28 (M4: mangler integration). */
    char* sym_base;
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
    char** explicit_cap_inits; /* parallel to names; NULL = plain capture, else init expr text */
    unsigned char* explicit_cap_flags; /* bit 0: is_ref */
    int explicit_cap_count;
    char** cap_names;
    char** cap_types;
    unsigned char* cap_flags;
    int cap_count;
    char* body_text; /* original body (includes braces for block bodies) */
} CCClosureDesc;

/* Rewrite calls to captured closure variables within a closure body.
   E.g., if 'inc' is captured as CCClosure1, rewrite `inc(x)` to `cc_closure1_call(inc, (intptr_t)(x))`. */
static char* cc__rewrite_captured_closure_calls_in_body(const char* body, const CCClosureDesc* d) {
    if (!body || !d || d->cap_count == 0) return NULL;

    /* Build list of captured closure names and their arities. */
    int closure_cap_n = 0;
    for (int i = 0; i < d->cap_count; i++) {
        if (!d->cap_types || !d->cap_types[i]) continue;
        if (strstr(d->cap_types[i], "CCClosure1") || strstr(d->cap_types[i], "CCClosure2")) {
            closure_cap_n++;
        }
    }
    if (closure_cap_n == 0) return NULL;

    char* out = NULL;
    size_t out_len = 0, out_cap = 0;
    size_t n = strlen(body);
    size_t pos = 0;

    while (pos < n) {
        /* Try to find a call to a captured closure. */
        int found_call = 0;
        size_t best_start = n;
        int best_cap_idx = -1;

        for (int i = 0; i < d->cap_count; i++) {
            if (!d->cap_names || !d->cap_names[i]) continue;
            if (!d->cap_types || !d->cap_types[i]) continue;
            if (!strstr(d->cap_types[i], "CCClosure1") && !strstr(d->cap_types[i], "CCClosure2")) continue;

            const char* name = d->cap_names[i];
            size_t name_len = strlen(name);
            if (name_len == 0) continue;

            /* Search for name followed by '(' */
            for (size_t j = pos; j + name_len < n; j++) {
                if (memcmp(body + j, name, name_len) != 0) continue;
                /* Check word boundary before */
                if (j > 0 && (cc_is_ident_char(body[j - 1]))) continue;
                /* Check word boundary after (next char is whitespace or '(') */
                size_t after = j + name_len;
                while (after < n && (body[after] == ' ' || body[after] == '\t' || body[after] == '\n' || body[after] == '\r')) after++;
                if (after >= n || body[after] != '(') continue;
                /* Found a call */
                if (j < best_start) {
                    best_start = j;
                    best_cap_idx = i;
                    found_call = 1;
                }
                break;
            }
        }

        if (!found_call) {
            /* No more calls, copy the rest. */
            cc__append_n(&out, &out_len, &out_cap, body + pos, n - pos);
            break;
        }

        /* Copy text before the call. */
        if (best_start > pos) {
            cc__append_n(&out, &out_len, &out_cap, body + pos, best_start - pos);
        }

        /* Parse and rewrite the call. */
        const char* name = d->cap_names[best_cap_idx];
        size_t name_len = strlen(name);
        int is_arity2 = (strstr(d->cap_types[best_cap_idx], "CCClosure2") != NULL);

        size_t after_name = best_start + name_len;
        while (after_name < n && (body[after_name] == ' ' || body[after_name] == '\t' || body[after_name] == '\n' || body[after_name] == '\r')) after_name++;
        if (after_name >= n || body[after_name] != '(') {
            /* Shouldn't happen, just copy name and continue. */
            cc__append_n(&out, &out_len, &out_cap, body + best_start, name_len);
            pos = best_start + name_len;
            continue;
        }

        size_t lparen = after_name;
        /* Find matching ')' */
        int par = 0, brk = 0, br = 0;
        int ins = 0; char q = 0;
        size_t rparen = n;
        for (size_t k = lparen + 1; k < n; k++) {
            char ch = body[k];
            if (ins) {
                if (ch == '\\' && k + 1 < n) { k++; continue; }
                if (ch == q) ins = 0;
                continue;
            }
            if (ch == '"' || ch == '\'') { ins = 1; q = ch; continue; }
            if (ch == '(') par++;
            else if (ch == ')') {
                if (par == 0 && brk == 0 && br == 0) { rparen = k; break; }
                if (par) par--;
            } else if (ch == '[') brk++;
            else if (ch == ']') { if (brk) brk--; }
            else if (ch == '{') br++;
            else if (ch == '}') { if (br) br--; }
        }

        if (rparen >= n) {
            /* Malformed, just copy as-is. */
            cc__append_n(&out, &out_len, &out_cap, body + best_start, name_len + 1);
            pos = lparen + 1;
            continue;
        }

        /* Extract args */
        size_t args_start = lparen + 1;
        size_t args_end = rparen;

        if (is_arity2) {
            /* Find comma at top level. */
            size_t comma = 0;
            par = 0; brk = 0; br = 0; ins = 0; q = 0;
            for (size_t k = args_start; k < args_end; k++) {
                char ch = body[k];
                if (ins) {
                    if (ch == '\\' && k + 1 < args_end) { k++; continue; }
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
                else if (ch == ',' && par == 0 && brk == 0 && br == 0) { comma = k; break; }
            }
            if (comma) {
                cc__append_fmt(&out, &out_len, &out_cap, "cc_closure2_call(%s, (intptr_t)(", name);
                cc__append_n(&out, &out_len, &out_cap, body + args_start, comma - args_start);
                cc__append_str(&out, &out_len, &out_cap, "), (intptr_t)(");
                cc__append_n(&out, &out_len, &out_cap, body + comma + 1, args_end - comma - 1);
                cc__append_str(&out, &out_len, &out_cap, "))");
            } else {
                /* No comma found, emit as arity-1 fallback. */
                cc__append_fmt(&out, &out_len, &out_cap, "cc_closure1_call(%s, (intptr_t)(", name);
                cc__append_n(&out, &out_len, &out_cap, body + args_start, args_end - args_start);
                cc__append_str(&out, &out_len, &out_cap, "))");
            }
        } else {
            cc__append_fmt(&out, &out_len, &out_cap, "cc_closure1_call(%s, (intptr_t)(", name);
            cc__append_n(&out, &out_len, &out_cap, body + args_start, args_end - args_start);
            cc__append_str(&out, &out_len, &out_cap, "))");
        }

        pos = rparen + 1;
    }

    return out;
}

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

/* Reference captures and pointer-like value captures use an opaque make-factory
 * ABI so the generated env allocator can stay simple and preserve qualifiers. */
static int cc__capture_needs_opaque(int is_ref, const char* ty) {
    if (is_ref) return 1;
    if (!ty) return 0;
    const char* p = ty + strlen(ty);
    while (p > ty && *(p-1) == ' ') p--;
    return p > ty && *(p-1) == '*';
}

/* When an opaque capture preserves a const-qualified pointee, use const void*
 * in the generated make-function signature so calls like const char* -> void*
 * do not trigger qualifier-discard warnings. */
static int cc__capture_needs_const_opaque(int is_ref, const char* ty) {
    if (!cc__capture_needs_opaque(is_ref, ty) || !ty) return 0;
    return strstr(ty, "const") != NULL;
}

static void cc__append_closure_make_proto(char** buf,
                                          size_t* len,
                                          size_t* cap,
                                          const char* closure_type,
                                          const char* sym_base,
                                          int nursery_variant,
                                          const CCClosureDesc* d) {
    if (!buf || !len || !cap || !closure_type || !sym_base || !d) return;
    if (nursery_variant) cc__append_fmt(buf, len, cap, "static %s %s_make_nursery(", closure_type, sym_base);
    else cc__append_fmt(buf, len, cap, "static %s %s_make(", closure_type, sym_base);
    if (nursery_variant) {
        cc__append_str(buf, len, cap, "CCNursery* __cc_nursery");
        if (d->cap_count > 0) cc__append_str(buf, len, cap, ", ");
    }
    for (int ci = 0; ci < d->cap_count; ci++) {
        int is_ref = (d->cap_flags && (d->cap_flags[ci] & 4) != 0);
        const char* ty = d->cap_types[ci] ? d->cap_types[ci] : "int";
        const char* nm = d->cap_names[ci] ? d->cap_names[ci] : "__cap";
        if (ci > 0) cc__append_str(buf, len, cap, ", ");
        if (cc__capture_needs_opaque(is_ref, ty)) {
            cc__append_fmt(buf, len, cap, "%s __cc_opaque_%s",
                           cc__capture_needs_const_opaque(is_ref, ty) ? "const void*" : "void*",
                           nm);
        } else {
            cc__append_fmt(buf, len, cap, "%s %s", ty, nm);
        }
    }
    if (!nursery_variant && d->cap_count == 0) cc__append_str(buf, len, cap, "void");
    cc__append_str(buf, len, cap, ");\n");
}

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
    int params_from_source = 0;
    char p0[128] = {0}, p1[128] = {0};
    char t0[128] = {0}, t1[128] = {0};

    /* trim left */
    size_t l0 = 0, l1 = arrow;
    while (l0 < l1 && (s[l0] == ' ' || s[l0] == '\t')) l0++;
    while (l1 > l0 && (s[l1 - 1] == ' ' || s[l1 - 1] == '\t')) l1--;

    /* Skip `@unsafe` when parsing params. */
    if (l0 + 7 <= l1 && s[l0] == '@' && memcmp(s + l0 + 1, "unsafe", 6) == 0) {
        size_t u1 = l0 + 7;
        if (u1 == l1 || !cc__is_ident_char2(s[u1])) {
            out->is_unsafe = 1;
            l0 = u1;
            while (l0 < l1 && (s[l0] == ' ' || s[l0] == '\t')) l0++;
        }
    }

    /* OLD SYNTAX DEPRECATION: [captures] before params is no longer supported.
       NEW SYNTAX (v3): captures come AFTER the arrow: () => [x, y] body */
    if (l0 < l1 && s[l0] == '[') {
        /* Error: old syntax used */
        return 0;
    }

    if (l0 < l1 && s[l0] == '(') {
        /* ( ... ) */
        size_t rp = l1;
        while (rp > l0 && s[rp - 1] != ')') rp--;
        if (rp <= l0) return 0;
        params_from_source = 1;
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
        params_from_source = 1;
        size_t q = l0 + 1;
        while (q < l1 && cc__is_ident_char2(s[q])) q++;
        size_t nn = q - l0;
        if (nn == 0 || nn >= sizeof(p0) || cc__is_keyword_tok(s + l0, nn)) return 0;
        memcpy(p0, s + l0, nn);
        p0[nn] = 0;
        param_count = 1;
    }

    if (!params_from_source && aux_param_count >= 0 && aux_param_count != param_count) {
        /* Fall back to the AST count only when the source span did not expose
         * an explicit parameter form.  The parser can report arity 0 when a
         * captured closure is passed through an unresolved macro/UFCS callee;
         * in that case `(a, b) => [capture] { ... }` remains authoritative. */
        param_count = aux_param_count;
        if (param_count == 0) { p0[0] = 0; p1[0] = 0; t0[0] = 0; t1[0] = 0; }
        if (param_count == 1 && p0[0] == 0) { /* leave unnamed */ }
        if (param_count == 2 && (p0[0] == 0 || p1[0] == 0)) { /* leave unnamed */ }
    }

    /* Parse body start (skip ws). */
    size_t b0 = arrow + 2;
    while (b0 < n && (s[b0] == ' ' || s[b0] == '\t' || s[b0] == '\r' || s[b0] == '\n')) b0++;
    if (b0 >= n) return 0;

    /* NEW SYNTAX (v3): Optional capture list `[x, &y]` comes AFTER the arrow */
    if (s[b0] == '[') {
        size_t j = b0 + 1;
        while (j < n && (s[j] == ' ' || s[j] == '\t')) j++;
        /* Disallow capture-all sugar: `[&]` and `[=]` */
        if (j + 1 < n && s[j] == '&' && s[j + 1] == ']') return -2; /* capture-all [&] banned */
        if (j < n && s[j] == '=' && (j + 1 == n || s[j + 1] == ']')) return -3; /* capture-all [=] banned */

        /* Find matching ']' */
        int sq = 1;
        size_t k = b0 + 1;
        for (; k < n; k++) {
            if (s[k] == '[') sq++;
            else if (s[k] == ']') { sq--; if (sq == 0) break; }
        }
        if (k >= n || s[k] != ']') return 0;
        size_t cap_l = b0 + 1;
        size_t cap_r = k;

        /* Parse entries: (&)? ident | ident = expr, comma-separated.
         * Init form `[alias = &local]` value-captures the expression under a
         * fresh name (C++-style init-capture). Capture-all `[=]` / `[&]` banned. */
        {
            char** names = NULL;
            char** inits = NULL;
            unsigned char* flags = NULL;
            int nn = 0;
            size_t p = cap_l;
            while (p < cap_r) {
                while (p < cap_r && (s[p] == ' ' || s[p] == '\t' || s[p] == '\n' || s[p] == '\r')) p++;
                if (p >= cap_r) break;
                if (s[p] == ',') { p++; continue; }
                if (s[p] == '=') {
                    /* capture-all not allowed */
                    for (int i = 0; i < nn; i++) {
                        free(names[i]);
                        free(inits ? inits[i] : NULL);
                    }
                    free(names); free(inits); free(flags);
                    return 0;
                }
                int is_ref = 0;
                if (s[p] == '&') { is_ref = 1; p++; }
                while (p < cap_r && (s[p] == ' ' || s[p] == '\t')) p++;
                if (p >= cap_r || !cc__is_ident_start_char(s[p])) {
                    /* capture-all like `[&]` or malformed */
                    for (int i = 0; i < nn; i++) {
                        free(names[i]);
                        free(inits ? inits[i] : NULL);
                    }
                    free(names); free(inits); free(flags);
                    return 0;
                }
                size_t ns = p;
                p++;
                while (p < cap_r && cc__is_ident_char2(s[p])) p++;
                size_t nl = p - ns;
                char* nm = (char*)malloc(nl + 1);
                if (!nm) {
                    for (int i = 0; i < nn; i++) {
                        free(names[i]);
                        free(inits ? inits[i] : NULL);
                    }
                    free(names); free(inits); free(flags);
                    return 0;
                }
                memcpy(nm, s + ns, nl);
                nm[nl] = 0;
                while (p < cap_r && (s[p] == ' ' || s[p] == '\t')) p++;
                char* init_txt = NULL;
                if (p < cap_r && s[p] == '=') {
                    /* Init-capture: name = expr. Not combinable with [&name]. */
                    if (is_ref) {
                        free(nm);
                        for (int i = 0; i < nn; i++) {
                            free(names[i]);
                            free(inits ? inits[i] : NULL);
                        }
                        free(names); free(inits); free(flags);
                        return 0;
                    }
                    p++;
                    while (p < cap_r && (s[p] == ' ' || s[p] == '\t' || s[p] == '\n' || s[p] == '\r')) p++;
                    size_t es = p;
                    int paren = 0, brace = 0, bracket = 0;
                    while (p < cap_r) {
                        char c = s[p];
                        if (c == '(') paren++;
                        else if (c == ')') { if (paren > 0) paren--; }
                        else if (c == '{') brace++;
                        else if (c == '}') { if (brace > 0) brace--; }
                        else if (c == '[') bracket++;
                        else if (c == ']') { if (bracket > 0) bracket--; }
                        else if (c == ',' && paren == 0 && brace == 0 && bracket == 0) break;
                        p++;
                    }
                    size_t ee = p;
                    while (ee > es && (s[ee - 1] == ' ' || s[ee - 1] == '\t' ||
                                       s[ee - 1] == '\n' || s[ee - 1] == '\r')) {
                        ee--;
                    }
                    if (ee <= es) {
                        free(nm);
                        for (int i = 0; i < nn; i++) {
                            free(names[i]);
                            free(inits ? inits[i] : NULL);
                        }
                        free(names); free(inits); free(flags);
                        return 0;
                    }
                    init_txt = (char*)malloc((ee - es) + 1);
                    if (!init_txt) {
                        free(nm);
                        for (int i = 0; i < nn; i++) {
                            free(names[i]);
                            free(inits ? inits[i] : NULL);
                        }
                        free(names); free(inits); free(flags);
                        return 0;
                    }
                    memcpy(init_txt, s + es, ee - es);
                    init_txt[ee - es] = 0;
                }
                /* dedupe */
                int dup = 0;
                for (int q = 0; q < nn; q++) if (names[q] && strcmp(names[q], nm) == 0) { dup = 1; break; }
                if (dup) { free(nm); free(init_txt); continue; }
                char** nnames = (char**)realloc(names, (size_t)(nn + 1) * sizeof(char*));
                char** ninits = (char**)realloc(inits, (size_t)(nn + 1) * sizeof(char*));
                unsigned char* nflags = (unsigned char*)realloc(flags, (size_t)(nn + 1) * sizeof(unsigned char));
                if (!nnames || !ninits || !nflags) {
                    free(nm);
                    free(init_txt);
                    free(nnames); free(ninits); free(nflags);
                    for (int i = 0; i < nn; i++) {
                        free(names[i]);
                        free(inits ? inits[i] : NULL);
                    }
                    free(names); free(inits); free(flags);
                    return 0;
                }
                names = nnames;
                inits = ninits;
                flags = nflags;
                names[nn] = nm;
                inits[nn] = init_txt;
                flags[nn] = (unsigned char)(is_ref ? 1 : 0);
                nn++;
            }
            out->explicit_cap_names = names;
            out->explicit_cap_inits = inits;
            out->explicit_cap_flags = flags;
            out->explicit_cap_count = nn;
        }

        /* Move past capture list to find body */
        b0 = k + 1;
        while (b0 < n && (s[b0] == ' ' || s[b0] == '\t' || s[b0] == '\r' || s[b0] == '\n')) b0++;
        if (b0 >= n) return 0;
    }

    size_t body_start = b0;
    size_t body_end = n;
    if (s[body_start] == '{') {
        /* Find matching '}' within literal span using the shared helper
         * (`util/text.h::cc_find_matching_brace`); it already skips
         * strings, char literals, and comments correctly. */
        size_t rbrace = 0;
        if (!cc_find_matching_brace(s, n, body_start, &rbrace)) return 0;
        body_end = rbrace + 1;
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
    free(d->sym_base);
    free(d->param0_name);
    free(d->param1_name);
    free(d->param0_type);
    free(d->param1_type);
    for (int i = 0; i < d->explicit_cap_count; i++) {
        free(d->explicit_cap_names ? d->explicit_cap_names[i] : NULL);
        free(d->explicit_cap_inits ? d->explicit_cap_inits[i] : NULL);
    }
    free(d->explicit_cap_names);
    free(d->explicit_cap_inits);
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

static int cc__cap_is_explicit(const CCClosureDesc* d, const char* name) {
    if (!d || !name || !d->explicit_cap_names) return 0;
    for (int i = 0; i < d->explicit_cap_count; i++) {
        if (!d->explicit_cap_names[i]) continue;
        if (strcmp(d->explicit_cap_names[i], name) == 0) return 1;
    }
    return 0;
}

/* Init-capture expression for `name`, or NULL for a plain capture. */
static const char* cc__cap_init_expr(const CCClosureDesc* d, const char* name) {
    if (!d || !name || !d->explicit_cap_names || !d->explicit_cap_inits) return NULL;
    for (int i = 0; i < d->explicit_cap_count; i++) {
        if (!d->explicit_cap_names[i]) continue;
        if (strcmp(d->explicit_cap_names[i], name) == 0) {
            return d->explicit_cap_inits[i];
        }
    }
    return NULL;
}

/* Parse supported init-capture forms into a base identifier and optional address-of.
 * Supported: `ident`, `&ident`, and one layer of parentheses around those. */
static int cc__parse_init_capture_base(const char* init, char* out_name, size_t out_cap, int* out_is_addr) {
    if (!init || !out_name || out_cap == 0 || !out_is_addr) return 0;
    *out_is_addr = 0;
    out_name[0] = 0;
    const char* s = init;
    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') s++;
    const char* e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\n' || e[-1] == '\r')) e--;
    if (e > s && *s == '(' && e[-1] == ')') {
        s++;
        e--;
        while (s < e && (*s == ' ' || *s == '\t')) s++;
        while (e > s && (e[-1] == ' ' || e[-1] == '\t')) e--;
    }
    if (s < e && *s == '&') {
        *out_is_addr = 1;
        s++;
        while (s < e && (*s == ' ' || *s == '\t')) s++;
    }
    if (s >= e || !cc__is_ident_start_char(*s)) return 0;
    const char* ns = s;
    s++;
    while (s < e && cc__is_ident_char2(*s)) s++;
    if (s != e) return 0; /* only simple ident / &ident */
    size_t nl = (size_t)(s - ns);
    if (nl + 1 > out_cap) return 0;
    memcpy(out_name, ns, nl);
    out_name[nl] = 0;
    return 1;
}

static char* cc__make_call_expr(const CCClosureDesc* d) {
    if (!d) return NULL;
    char* b = NULL;
    size_t bl = 0, bc = 0;
    cc__append_fmt(&b, &bl, &bc, "%s_make(", d->sym_base);
    if (d->cap_count == 0) {
        cc__append_str(&b, &bl, &bc, ")");
    } else {
        for (int i = 0; i < d->cap_count; i++) {
            if (i) cc__append_str(&b, &bl, &bc, ", ");
            int is_ref = (d->cap_flags && (d->cap_flags[i] & 4) != 0);
            int mo = (!is_ref && d->cap_flags && (d->cap_flags[i] & 2) != 0);
            const char* init = cc__cap_init_expr(d, d->cap_names[i]);
            if (is_ref) cc__append_str(&b, &bl, &bc, "&");
            if (mo) cc__append_str(&b, &bl, &bc, "cc_move(");
            if (init) {
                cc__append_str(&b, &bl, &bc, init);
            } else {
                cc__append_str(&b, &bl, &bc, d->cap_names[i] ? d->cap_names[i] : "0");
            }
            if (mo) cc__append_str(&b, &bl, &bc, ")");
        }
        cc__append_str(&b, &bl, &bc, ")");
    }
    return b;
}

/* milestone 4b: marker offsets (closure-head positions derived from
 * /*CC_CLO:N*\/ anchors) passed down from the public wrapper.  When
 * `marker_n` equals the in-TU closure-node count, each closure's start
 * offset is taken verbatim from `marker_offs[k]` (exact, comment-safe)
 * rather than the (line,col)+arrow-scan heuristic + recovery fallback. */
static int cc__rewrite_closure_literals_with_nodes_impl(const CCASTRoot* root,
                                            const CCVisitorCtx* ctx,
                                            const char* in_src,
                                            size_t in_len,
                                            const size_t* marker_offs,
                                            int marker_n,
                                            char** out_src,
                                            size_t* out_len,
                                            char** out_protos,
                                            size_t* out_protos_len,
                                            char** out_defs,
                                            size_t* out_defs_len);

/* Public entry point.  Scan the codegen buffer for the /*CC_CLO:N*\/
 * closure-ID markers — injected at parse-build time for user closures and by
 * every closure SYNTHESIZER (autoblock) for generated ones — record each
 * closure-head offset (the byte just past its marker), then neutralize the
 * marker comments to spaces in a private working copy.  Spaces preserve every
 * byte offset (so the stub-AST line/col spans still line up) while removing the
 * markers from the text the @unsafe back-scan, span parsing and the emitted C
 * all see.
 *
 * Markers whose IDs appear in root->skipped_clo_ids are PRUNED: those sat
 * inside #if-regions that conditional compilation discarded during this
 * root's parse (TCC's preprocess_skip records them exactly), so they have no
 * CLOSURE nodes.  After pruning, marker count == closure-node count is an
 * INVARIANT — the impl fails loudly on mismatch instead of guessing. */
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
    if (!in_src || in_len == 0) {
        return cc__rewrite_closure_literals_with_nodes_impl(
            root, ctx, in_src, in_len, NULL, 0,
            out_src, out_len, out_protos, out_protos_len, out_defs, out_defs_len);
    }

    char* work = (char*)malloc(in_len + 1);
    if (!work) return -1;
    memcpy(work, in_src, in_len);
    work[in_len] = '\0';

    size_t* offs = NULL;
    int offs_n = 0, offs_cap = 0;
    size_t p = 0;
    while (p + 10 < in_len) {
        if (work[p] == '/' && work[p + 1] == '*' &&
            work[p + 2] == 'C' && work[p + 3] == 'C' && work[p + 4] == '_' &&
            work[p + 5] == 'C' && work[p + 6] == 'L' && work[p + 7] == 'O' &&
            work[p + 8] == ':') {
            size_t q = p + 9;
            int id = 0;
            while (q < in_len && work[q] >= '0' && work[q] <= '9') {
                id = id * 10 + (work[q] - '0');
                q++;
            }
            if (q + 1 < in_len && work[q] == '*' && work[q + 1] == '/') {
                size_t after = q + 2;
                int skipped = 0;
                if (id > 0 && root && root->skipped_clo_ids) {
                    for (int t = 0; t < root->skipped_clo_count; t++) {
                        if (root->skipped_clo_ids[t] == id) { skipped = 1; break; }
                    }
                }
                if (!skipped) {
                    if (offs_n == offs_cap) {
                        int nc = offs_cap ? offs_cap * 2 : 32;
                        size_t* g = (size_t*)realloc(offs, (size_t)nc * sizeof(size_t));
                        if (!g) { free(offs); free(work); return -1; }
                        offs = g;
                        offs_cap = nc;
                    }
                    offs[offs_n++] = after;
                }
                /* Neutralize the marker comment bytes to spaces. */
                for (size_t b = p; b < after; b++) work[b] = ' ';
                p = after;
                continue;
            }
        }
        p++;
    }

    int rc = cc__rewrite_closure_literals_with_nodes_impl(
        root, ctx, work, in_len, offs, offs_n,
        out_src, out_len, out_protos, out_protos_len, out_defs, out_defs_len);

    free(offs);
    free(work);
    return rc;
}

static int cc__rewrite_closure_literals_with_nodes_impl(const CCASTRoot* root,
                                            const CCVisitorCtx* ctx,
                                            const char* in_src,
                                            size_t in_len,
                                            const size_t* marker_offs,
                                            int marker_n,
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
                sigs[idx].line_end = n[i].line_end;
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

    /* TCC may defer and replay function-call arguments in reverse order, so
     * recorder order is not source order for closure literals nested in call
     * arguments.  Stable-sort by exact parse-buffer offsets; fall back to
     * source line/column only when an offset is unavailable.  Marker pairing
     * below depends on this order. */
    for (int i = 1; i < idx_n; i++) {
        int key = idxs[i];
        int j = i;
        while (j > 0) {
            const NodeView* a = &n[idxs[j - 1]];
            const NodeView* b = &n[key];
            int a_after_b = 0;
            if (a->off_start >= 0 && b->off_start >= 0) {
                a_after_b = a->off_start > b->off_start;
            } else if (a->line_start != b->line_start) {
                a_after_b = a->line_start > b->line_start;
            } else {
                a_after_b = a->col_start > b->col_start;
            }
            if (!a_after_b) break;
            idxs[j] = idxs[j - 1];
            j--;
        }
        idxs[j] = key;
    }

    /* Marker k is closure k: the marker producer scans the buffer linearly
     * and idxs is in source order, so the k-th surviving marker is the k-th
     * closure's exact, comment-safe start offset. */
    int use_markers = (marker_offs && marker_n == idx_n);
    if (getenv("CC_DEBUG_CLOSURE_SPANS")) {
        fprintf(stderr, "CC_DEBUG_CLOSURE_SPANS: markers=%d closures=%d use_markers=%d\n",
                marker_n, idx_n, use_markers);
    }
    /* FAIL LOUDLY: after #if-skip pruning, marker count == closure-node
     * count is an invariant (parse-build injects a marker per user closure,
     * every synthesizer marks its generated closures, and TCC reports the
     * markers conditional compilation discarded).  A mismatch means a
     * producer forgot its marker or the pruning accounting broke — guessing
     * spans with heuristics is how closures silently bound to the wrong
     * source text, so we refuse instead. */
    if (!use_markers && idx_n > 0) {
        const char* path = ctx->input_path ? ctx->input_path : "<input>";
        if (marker_n < idx_n) {
            /* More AST closures than markers in the buffer: the extras were
             * materialized by MACRO EXPANSION — a marker comment cannot
             * survive CPP token replay (comments are stripped when a
             * #define body is tokenized), so a closure literal written
             * inside a macro definition can never carry its identity.
             * This is a language limitation, not a compiler bug: say so,
             * and point at the expansion sites (node line/col attribute to
             * the invocation). */
            fprintf(stderr,
                    "cc: error: %s: %d closure literal(s) come from macro "
                    "expansion; closure literals inside #define bodies are "
                    "not supported\n",
                    path, idx_n - marker_n);
            fprintf(stderr,
                    "  hint: define the closure at the use site, or make the "
                    "macro take the closure as a parameter\n");
            for (int k = 0; k < idx_n; k++) {
                const NodeView* cn = &n[idxs[k]];
                fprintf(stderr, "  note: closure at %s:%d\n",
                        cn->file ? cn->file : path, cn->line_start);
            }
        } else {
            fprintf(stderr,
                    "cc: internal error: closure marker/node count mismatch "
                    "(markers=%d closures=%d) in %s — a closure producer did "
                    "not emit its /*CC_CLO:N*/ marker, or #if-skip pruning "
                    "miscounted\n",
                    marker_n, idx_n, path);
        }
        free(idxs);
        return -1;
    }

    CCClosureDesc* descs = (CCClosureDesc*)calloc((size_t)idx_n, sizeof(CCClosureDesc));
    if (!descs) { free(idxs); return 0; }

    for (int k = 0; k < idx_n; k++) {
        int i = idxs[k];
        CCClosureDesc* d = &descs[k];
        d->id = k + 1;
        d->start_line = n[i].line_start;
        d->end_line = n[i].line_end;
        /* Exact, comment-safe start offset from the closure-ID marker.
         * The marker comment has already been neutralized to spaces in
         * `in_src` by the wrapper, so the @unsafe back-scan and span
         * parsing below behave exactly as for an unmarked closure.
         * (The (line,col)+arrow-scan heuristic and its whole-buffer `=>`
         * recovery fallback are gone: markers are the only identity.) */
        size_t start_off = marker_offs[k];
        int start_col1 = 1;
        if (start_off >= in_len) { cc__free_closure_desc(d); continue; }
        {
            size_t ln_lo = start_off;
            while (ln_lo > 0 && in_src[ln_lo - 1] != '\n') ln_lo--;
            start_col1 = 1 + (int)(start_off - ln_lo);
        }
        if (getenv("CC_DEBUG_CLOSURE_SPANS")) {
            fprintf(stderr, "CC_DEBUG_CLOSURE_SPANS: id=%d MARKER off=%zu col=%d\n",
                    d->id, start_off, start_col1);
        }
        d->start_col = start_col1 - 1;
        d->end_col = (n[i].col_end > 0) ? (n[i].col_end - 1) : -1;
        d->start_off = start_off;

        /* Mint the location-tagged base symbol for every emitted helper
         * (entry/make/env/...). N<id> is the dedup tiebreaker; line+col
         * carries the source location into every backtrace frame. See the
         * comment on `sym_base` in the CCClosureDesc struct definition. */
        {
            char user_ident[16];
            snprintf(user_ident, sizeof(user_ident), "N%d", d->id);
            int col1_for_sym = (d->start_col >= 0) ? (d->start_col + 1) : 1;
            d->sym_base = cc_diag_mangle_symbol(CC_CONSTRUCT_CLOSURE,
                                                user_ident,
                                                d->start_line,
                                                col1_for_sym);
            if (!d->sym_base) {
                /* OOM: fall back to a deterministic, non-empty string so the
                 * emit loop below cannot dereference NULL. The compile will
                 * still likely fail downstream, but we won't crash here. */
                char fallback[32];
                snprintf(fallback, sizeof(fallback), "cc_closure__N%d__line0_col0", d->id);
                d->sym_base = strdup(fallback);
            }
        }

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
            cc_pass_error_cat(f, n[i].line_start, 1, CC_ERR_CLOSURE, "capture-all [&] is not allowed");
            cc__free_closure_desc(d);
            free(descs);
            free(idxs);
            cc__free_func_sigs(sigs, sig_n);
            return -1;
        }
        if (pr == -3) {
            const char* f = (n[i].file && n[i].file[0]) ? n[i].file : (ctx->input_path ? ctx->input_path : "<input>");
            cc_pass_error_cat(f, n[i].line_start, 1, CC_ERR_CLOSURE, "capture-all [=] is not allowed");
            cc__free_closure_desc(d);
            free(descs);
            free(idxs);
            cc__free_func_sigs(sigs, sig_n);
            return -1;
        }
        if (pr == 1) {
            cc__infer_send_into_builder_param_types(in_src, in_len, d->start_off,
                                                    d->param_count,
                                                    &d->param0_type,
                                                    &d->param1_type);
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

    char* in_src_decl_scan = cc__src_strip_comments_and_strings(in_src, in_len);
    if (!in_src_decl_scan) {
        for (int q = 0; q < idx_n; q++) cc__free_closure_desc(&descs[q]);
        free(descs);
        free(idxs);
        cc__free_func_sigs(sigs, sig_n);
        return -1;
    }

    const char* cur = in_src;
    char* decl_carry = NULL;
    size_t off = 0;
    while (off < in_len && *cur) {
        const char* line_start = cur;
        const char* nl = memchr(cur, '\n', in_len - off);
        const char* line_end = nl ? nl : (in_src + in_len);
        size_t line_len = (size_t)(line_end - line_start);

        /* Record decls using a rolling statement buffer so multi-line decls like
           `CCAbIntptr x =\n  (CCAbIntptr)(...) ;` are visible to capture inference. */
        {
            size_t carry_len = decl_carry ? strlen(decl_carry) : 0;
            char* next_carry = (char*)realloc(decl_carry, carry_len + line_len + 2);
            if (!next_carry) {
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
                free(in_src_decl_scan);
                return -1;
            }
            decl_carry = next_carry;
            memcpy(decl_carry + carry_len, in_src_decl_scan + off, line_len);
            decl_carry[carry_len + line_len] = '\n';
            decl_carry[carry_len + line_len + 1] = '\0';
            cc__maybe_record_decl(scope_names, scope_types, scope_flags, scope_counts, depth, decl_carry);
            {
                size_t consumed = cc__last_top_level_semi_offset(decl_carry);
                if (consumed > 0) {
                    size_t remain = strlen(decl_carry + consumed);
                    memmove(decl_carry, decl_carry + consumed, remain + 1);
                } else if (cc__has_top_level_brace(decl_carry)) {
                    decl_carry[0] = '\0';
                }
            }
        }

        /* Process any closures that start on or before the end of this line. */
        while (cur_closure < idx_n && descs[cur_closure].start_off < (off + line_len + 1)) {
            CCClosureDesc* d = &descs[cur_closure];
            /* Start with explicit captures from `[ ... ]` (if any), then add implicit value captures
               from free-var scan of the body (ignore param names). */
            char** caps = cc__dup_string_list(d->explicit_cap_names, d->explicit_cap_count);
            int cap_n = d->explicit_cap_count;
            if (!caps && d->explicit_cap_count > 0) {
                cc_pass_error_cat(ctx->input_path ? ctx->input_path : "<input>",
                        d->start_line, (d->start_col >= 0 ? (d->start_col + 1) : 1),
                        CC_ERR_CLOSURE, "out of memory while building captures");
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
                free(in_src_decl_scan);
                return -1;
            }
            if (d->body_text) {
                char** local_decl_names = NULL;
                int local_decl_count = 0;
                cc__collect_decl_names_from_block_text(d->body_text, &local_decl_names, &local_decl_count);
                cc__collect_caps_from_block(scope_names, scope_counts, in_src, d->start_off, depth,
                                            d->body_text,
                                            d->param0_name,
                                            d->param1_name,
                                            local_decl_names,
                                            local_decl_count,
                                            &caps, &cap_n);
                cc__free_string_list(local_decl_names, local_decl_count);
            }
            if (caps && cap_n > 0) {
                int keep_n = 0;
                for (int ci = 0; ci < cap_n; ci++) {
                    const char* cap = caps[ci];
                    unsigned char fl_global = 0;
                    char* global_ty = NULL;
                    int keep_cap = 1;

                    if (!cap) continue;
                    if (!cc__cap_is_explicit(d, cap)) {
                        global_ty = cc__lookup_top_level_decl_type_by_text(in_src, d->start_off, cap, &fl_global);
                        if (global_ty && cc__capture_type_text_usable(global_ty, cap)) {
                            keep_cap = 0;
                        }
                        free(global_ty);
                    }

                    if (keep_cap) {
                        caps[keep_n++] = caps[ci];
                    } else {
                        free(caps[ci]);
                    }
                }
                cap_n = keep_n;
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
                        const char* init = cc__cap_init_expr(d, caps[ci]);
                        char init_base[128];
                        int init_is_addr = 0;
                        const char* lookup_name = caps[ci];
                        if (init) {
                            if (!cc__parse_init_capture_base(init, init_base, sizeof(init_base),
                                                             &init_is_addr)) {
                                int col1 = d->start_col >= 0 ? (d->start_col + 1) : 1;
                                fprintf(stderr,
                                        "%s:%d:%d: error: CC: unsupported init-capture '%s = %s' "
                                        "(supported forms: ident, &ident)\n",
                                        ctx->input_path ? ctx->input_path : "<input>",
                                        d->start_line,
                                        col1,
                                        caps[ci] ? caps[ci] : "?",
                                        init);
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
                                free(in_src_decl_scan);
                                return -1;
                            }
                            lookup_name = init_base;
                        }
                        for (int dd = depth; dd >= 1 && !ty; dd--) {
                            ty = cc__lookup_decl_type(scope_names[dd], scope_types[dd], scope_counts[dd], lookup_name);
                            if (ty) fl = cc__lookup_decl_flags(scope_names[dd], scope_flags[dd], scope_counts[dd], lookup_name);
                        }
                        if (ty && !cc__capture_type_text_usable(ty, lookup_name)) {
                            ty = NULL;
                            fl = 0;
                        }
                        if (!ty) {
                            ty = cc__lookup_param_type_by_src(sigs, sig_n, in_src, d->start_off, lookup_name);
                        }
                        if (ty && !cc__capture_type_text_usable(ty, lookup_name)) {
                            ty = NULL;
                            fl = 0;
                        }
                        if (!ty) {
                            ty = cc__lookup_param_type_for_closure(sigs, sig_n, lookup_name, d->start_line);
                        }
                        if (ty && !cc__capture_type_text_usable(ty, lookup_name)) {
                            ty = NULL;
                            fl = 0;
                        }
                        if (!ty) {
                            unsigned char fl_global = 0;
                            char* ty_global = cc__lookup_top_level_decl_type_by_text(in_src, d->start_off, lookup_name, &fl_global);
                            if (ty_global && cc__capture_type_text_usable(ty_global, lookup_name)) {
                                d->cap_types[ci] = ty_global;
                                ty = d->cap_types[ci];
                                fl = fl_global;
                            } else {
                                free(ty_global);
                            }
                        }
                        if (!ty) {
                            unsigned char fl_fb = 0;
                            char* ty_fb = cc__lookup_decl_type_by_text_fallback(in_src, d->start_off, lookup_name, &fl_fb);
                            if (ty_fb && cc__capture_type_text_usable(ty_fb, lookup_name)) {
                                d->cap_types[ci] = ty_fb;
                                ty = d->cap_types[ci];
                                fl = fl_fb;
                            } else {
                                free(ty_fb);
                            }
                        }
                        if (!ty) {
                            unsigned char fl_gen = 0;
                            char* ty_gen = cc__lookup_internal_generated_decl_type(in_src, d->start_off, lookup_name, &fl_gen);
                            if (ty_gen && cc__capture_type_text_usable(ty_gen, lookup_name)) {
                                d->cap_types[ci] = ty_gen;
                                ty = d->cap_types[ci];
                                fl = fl_gen;
                            } else {
                                free(ty_gen);
                            }
                        }
                        if (ty && init_is_addr) {
                            /* `alias = &local` → capture type is pointer-to(local). */
                            size_t tlen = strlen(ty);
                            char* ptr_ty = (char*)malloc(tlen + 2);
                            if (ptr_ty) {
                                memcpy(ptr_ty, ty, tlen);
                                ptr_ty[tlen] = '*';
                                ptr_ty[tlen + 1] = 0;
                                free(d->cap_types[ci]);
                                d->cap_types[ci] = ptr_ty;
                                ty = ptr_ty;
                                fl |= 0x08; /* aliases_outer_local (same as T* p = &local) */
                            }
                        }
                        if (ty) {
                            char canonical_ty[256];
                            const char* emit_ty = ty;
                            CCTypeRegistry* reg = cc_type_registry_get_global();
                            if (reg &&
                                cc_type_registry_canonicalize_type_name(
                                    reg, ty, canonical_ty, sizeof(canonical_ty)) &&
                                canonical_ty[0]) {
                                emit_ty = canonical_ty;
                            }
                            if (!d->cap_types[ci] ||
                                strcmp(d->cap_types[ci], emit_ty) != 0) {
                                char* owned_ty = strdup(emit_ty);
                                if (owned_ty) {
                                    free(d->cap_types[ci]);
                                    d->cap_types[ci] = owned_ty;
                                }
                            }
                        }
                        
                        if (cc__cap_is_ref(d, caps[ci])) fl |= 4; /* bit 2: reference capture */
                        d->cap_flags[ci] = fl;
                        if (!ty) {
                            int col1 = d->start_col >= 0 ? (d->start_col + 1) : 1;
                            if (init) {
                                fprintf(stderr,
                                        "%s:%d:%d: error: CC: cannot infer type for init-capture '%s = %s'\n",
                                        ctx->input_path ? ctx->input_path : "<input>",
                                        d->start_line,
                                        col1,
                                        caps[ci] ? caps[ci] : "?",
                                        init);
                            } else {
                                fprintf(stderr,
                                        "%s:%d:%d: error: CC: cannot infer type for captured name '%s' (currently supports simple decls like 'int x = ...;' or 'T* p = ...;')\n",
                                        ctx->input_path ? ctx->input_path : "<input>",
                                        d->start_line,
                                        col1,
                                        caps[ci] ? caps[ci] : "?");
                            }
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
                            free(in_src_decl_scan);
                            return -1;
                        }
                    }
                }
            }
            /* Register resolved captures in the scope table so that nested
               closures can find them through the normal scope walk instead of
               falling through to error-prone text-based fallbacks. */
            if (d->cap_count > 0 && d->cap_types && depth >= 0 && depth < 255) {
                for (int ci = 0; ci < d->cap_count; ci++) {
                    const char* nm = d->cap_names ? d->cap_names[ci] : NULL;
                    const char* ty = d->cap_types[ci];
                    if (!nm || !ty) continue;
                    if (cc__name_in_list(scope_names[depth], scope_counts[depth], nm, strlen(nm))) continue;
                    int cur_n = scope_counts[depth];
                    char** nn = (char**)realloc(scope_names[depth], (size_t)(cur_n + 1) * sizeof(char*));
                    char** tn = (char**)realloc(scope_types[depth], (size_t)(cur_n + 1) * sizeof(char*));
                    unsigned char* fn = (unsigned char*)realloc(scope_flags[depth], (size_t)(cur_n + 1) * sizeof(unsigned char));
                    if (nn && tn && fn) {
                        scope_names[depth] = nn;
                        scope_types[depth] = tn;
                        scope_flags[depth] = fn;
                        scope_names[depth][cur_n] = strdup(nm);
                        scope_types[depth][cur_n] = strdup(ty);
                        scope_flags[depth][cur_n] = 0;
                        scope_counts[depth] = cur_n + 1;
                    }
                }
            }
            /* Check for mutations to reference-captured variables, and for
             * value-captured pointer aliases of outer locals in task-escaping
             * closures (unless @unsafe or a registered sync library is
             * captured alongside — e.g. CCExclusiveMutex + [&total]).
             * Sync CCClosure stores/calls may still write through `T* p = &local`. */
            int has_sync_lib = 0;
            if (!d->is_unsafe && d->cap_count > 0 && d->cap_types) {
                for (int si = 0; si < d->cap_count; si++) {
                    if (cc__is_registered_sync_capture_type(d->cap_types[si])) {
                        has_sync_lib = 1;
                        break;
                    }
                }
            }
            if (!d->is_unsafe && !has_sync_lib && d->body_text && d->cap_count > 0) {
                int escapes = cc__closure_in_task_escape_arg(in_src, in_len, d->start_off);
                for (int ci = 0; ci < d->cap_count; ci++) {
                    int is_ref = (d->cap_flags && (d->cap_flags[ci] & 4) != 0);
                    int aliases_local = (d->cap_flags && (d->cap_flags[ci] & 0x08) != 0);
                    const char* ty = d->cap_types ? d->cap_types[ci] : NULL;
                    if (cc__is_safe_wrapper_type(ty)) continue;
                    const char* nm = d->cap_names[ci];
                    /* Ignore compiler-generated pointer temps (autoblock retp, …). */
                    if (!is_ref && aliases_local && nm &&
                        (strncmp(nm, "cc_", 3) == 0 || strncmp(nm, "__cc_", 5) == 0)) {
                        continue;
                    }
                    size_t mut_off = 0;
                    CCMutationKind mk = CC_MUT_NONE;
                    const char* callee = NULL;
                    const char* pty = NULL;
                    int hit = 0;
                    if (is_ref) {
                        hit = cc__find_mutation_in_body(d->body_text, nm, sigs, sig_n,
                                                        &mk, &mut_off, &callee, &pty);
                    } else if (aliases_local && escapes) {
                        hit = cc__find_ptr_pointee_mutation_in_body(d->body_text, nm, &mut_off);
                        if (hit) mk = CC_MUT_WRITE;
                    } else {
                        continue;
                    }
                    if (hit) {
                        int col1 = d->start_col >= 0 ? (d->start_col + 1) : 1;
                        if (!is_ref && aliases_local) {
                            fprintf(stderr,
                                    "%s:%d:%d: error: mutation through value-captured pointer '%s' that aliases an outer local\n",
                                    ctx->input_path ? ctx->input_path : "<input>",
                                    d->start_line,
                                    col1,
                                    nm ? nm : "?");
                            fprintf(stderr,
                                    "  = note: capturing `T* p = &x` then writing `*p` bypasses shared-ref mutation checks\n"
                                    "  = help: capture [&x] with Atomic/CCExclusive, or @unsafe, or don't alias\n");
                        } else if (mk == CC_MUT_ADDR_OF_NONCONST_CALL && callee) {
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
                                    "  = help: make the parameter 'const %s*' for read-only, or capture a CCExclusive / @unsafe\n",
                                    ty ? ty : "T");
                        } else if (mk == CC_MUT_ADDR_OF_ESCAPES) {
                            fprintf(stderr,
                                    "%s:%d:%d: error: taking address of shared reference '%s' may allow mutation (data race)\n",
                                    ctx->input_path ? ctx->input_path : "<input>",
                                    d->start_line,
                                    col1,
                                    nm ? nm : "?");
                            fprintf(stderr,
                                    "  = help: pass as 'const %s*' to a known read-only function, or capture a CCExclusive / @unsafe\n",
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
                                    "  = help: use @atomic %s, capture a CCExclusiveMutex, or @unsafe [&%s]\n",
                                    ty ? ty : "T", nm ? nm : "var");
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
                        free(in_src_decl_scan);
                        cc__free_func_sigs(sigs, sig_n);
                        return -1;
                    }
                }
            }
            cur_closure++;
        }

        /* Update brace depth and clear scope on close (best-effort, same as old scanner). */
        for (const char* x = in_src_decl_scan + off; x < in_src_decl_scan + off + line_len; x++) {
            if (*x == '{') {
                depth++;
                /* When entering a new scope, check if this is a function body and register its parameters.
                   This enables closures to capture function parameters, not just local variables. */
                for (int si = 0; si < sig_n; si++) {
                    if (sigs[si].line_start == line_num && sigs[si].param_names && sigs[si].name) {
                        size_t fname_len = strlen(sigs[si].name);
                        int name_found = 0;
                        for (const char* q = in_src_decl_scan + off; q + fname_len <= in_src_decl_scan + off + line_len; q++) {
                            if (memcmp(q, sigs[si].name, fname_len) == 0 &&
                                (q == in_src_decl_scan + off || !cc__is_ident_char2(q[-1])) &&
                                (q + fname_len >= in_src_decl_scan + off + line_len || !cc__is_ident_char2(q[fname_len]))) {
                                name_found = 1;
                                break;
                            }
                        }
                        if (!name_found) continue;
                        for (int pi = 0; pi < sigs[si].param_count; pi++) {
                            if (!sigs[si].param_names[pi] || !sigs[si].param_types[pi]) continue;
                            
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
    free(in_src_decl_scan);

    /* Emit protos/defs and build rewrite edits for all closure literals. */
    char* protos = NULL;
    size_t protos_len = 0, protos_cap = 0;
    char* defs = NULL;
    size_t defs_len = 0, defs_cap = 0;

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

        /* Emit exact closure declarations for the EOF-generated section, where
           all user-visible types are already in scope.  We inline what
           `CC_CLOSURE0_DECL` would have produced rather than using the macro:
           since 2026-05-28 names use a location-tagged base symbol (`d->sym_base`,
           e.g. `cc_closure__N7__line42_col15`) that doesn't fit the macro's
           `__cc_closure_<role>_##n` token-paste shape. The macros in
           `cc_closure_helper.h` remain as conveniences for any non-pass user. */
        if (d->param_count == 0 && d->cap_count == 0) {
            cc__append_fmt(&protos, &protos_len, &protos_cap, "static void* %s_entry(void*);\n", d->sym_base);
            cc__append_fmt(&protos, &protos_len, &protos_cap, "static CCClosure0 %s_make(void);\n", d->sym_base);
            cc__append_fmt(&protos, &protos_len, &protos_cap, "static CCClosure0 %s_make_nursery(CCNursery* __cc_nursery);\n", d->sym_base);
        } else {
            if (d->param_count == 0) cc__append_fmt(&protos, &protos_len, &protos_cap, "static void* %s_entry(void*);\n", d->sym_base);
            else if (d->param_count == 1) cc__append_fmt(&protos, &protos_len, &protos_cap, "static void* %s_entry(void*, intptr_t);\n", d->sym_base);
            else cc__append_fmt(&protos, &protos_len, &protos_cap, "static void* %s_entry(void*, intptr_t, intptr_t);\n", d->sym_base);
            const char* cty_p = (d->param_count == 0 ? "CCClosure0" : (d->param_count == 1 ? "CCClosure1" : "CCClosure2"));
            if (d->cap_count == 0) {
                cc__append_fmt(&protos, &protos_len, &protos_cap, "static %s %s_make(void);\n", cty_p, d->sym_base);
                cc__append_fmt(&protos, &protos_len, &protos_cap, "static %s %s_make_nursery(CCNursery* __cc_nursery);\n", cty_p, d->sym_base);
            } else {
                cc__append_closure_make_proto(&protos, &protos_len, &protos_cap, cty_p, d->sym_base, 0, d);
                cc__append_closure_make_proto(&protos, &protos_len, &protos_cap, cty_p, d->sym_base, 1, d);
            }
        }
        /* Blank line between adjacent closure proto groups so the
         * lowered C reads as paired (DECL + make-helper(s)) blocks
         * rather than a wall of consecutive declarations. */
        cc__append_str(&protos, &protos_len, &protos_cap, "\n");

        /* defs: env+drop+make */
        const char* cty = (d->param_count == 0 ? "CCClosure0" : (d->param_count == 1 ? "CCClosure1" : "CCClosure2"));
        const char* mkfn = (d->param_count == 0 ? "cc_closure0_make" : (d->param_count == 1 ? "cc_closure1_make" : "cc_closure2_make"));

        if (ctx && ctx->input_path && d->start_line > 0) {
            /* The env/drop/make scaffolding below is GENERATED code — do
             * not map its ~35 lines onto the user file (a literal near EOF
             * would then map past EOF as the mapping drifts line by line).
             * Attribute it to the "<cc-closures>" pseudo-file at a
             * defs-relative physical line; the copied user BODY gets its
             * own ledger-aware `#line <user> "<input>"` anchor right
             * before the entry body (see below). */
            size_t defs_line = 1;
            for (size_t b = 0; b < defs_len; b++)
                if (defs[b] == '\n') defs_line++;
            cc__append_fmt(&defs, &defs_len, &defs_cap, "#line %zu \"<cc-closures>\"\n",
                           defs_line + 1);
        }
        cc__append_fmt(&defs, &defs_len, &defs_cap,
                       "/* CC closure %d */\n", d->id);

        if (d->cap_count > 0) {
            cc__append_fmt(&defs, &defs_len, &defs_cap, "typedef struct %s_env {\n", d->sym_base);
            for (int ci = 0; ci < d->cap_count; ci++) {
                int is_ref = (d->cap_flags && (d->cap_flags[ci] & 4) != 0);
                const char* ty = d->cap_types[ci] ? d->cap_types[ci] : "int";
                const char* nm = d->cap_names[ci] ? d->cap_names[ci] : "__cap";
                if (cc__capture_needs_opaque(is_ref, ty)) {
                    cc__append_fmt(&defs, &defs_len, &defs_cap, "  %s %s;\n",
                                   cc__capture_needs_const_opaque(is_ref, ty) ? "const void*" : "void*",
                                   nm);
                } else if (is_ref) {
                    cc__append_fmt(&defs, &defs_len, &defs_cap, "  %s* %s;\n", ty, nm);
                } else {
                    cc__append_fmt(&defs, &defs_len, &defs_cap, "  %s %s;\n", ty, nm);
                }
            }
            cc__append_str(&defs, &defs_len, &defs_cap, "} ");
            cc__append_fmt(&defs, &defs_len, &defs_cap, "%s_env;\n", d->sym_base);
            cc__append_fmt(&defs, &defs_len, &defs_cap,
                           "static void %s_env_drop(void* p) { if (p) free(p); }\n",
                           d->sym_base);
            cc__append_fmt(&defs, &defs_len, &defs_cap,
                           "static void %s_env_nursery_drop(void* p) { (void)p; }\n",
                           d->sym_base);

            cc__append_fmt(&defs, &defs_len, &defs_cap, "static %s %s_make(", cty, d->sym_base);
            for (int ci = 0; ci < d->cap_count; ci++) {
                if (ci) cc__append_str(&defs, &defs_len, &defs_cap, ", ");
                int is_ref = (d->cap_flags && (d->cap_flags[ci] & 4) != 0);
                const char* ty = d->cap_types[ci] ? d->cap_types[ci] : "int";
                const char* nm = d->cap_names[ci] ? d->cap_names[ci] : "__cap";
                if (cc__capture_needs_opaque(is_ref, ty)) {
                    /* Match the opaque proto so declaration and definition agree. */
                    cc__append_fmt(&defs, &defs_len, &defs_cap, "%s __cc_opaque_%s",
                                   cc__capture_needs_const_opaque(is_ref, ty) ? "const void*" : "void*",
                                   nm);
                } else if (is_ref) {
                    cc__append_fmt(&defs, &defs_len, &defs_cap, "%s* %s", ty, nm);
                } else {
                    cc__append_fmt(&defs, &defs_len, &defs_cap, "%s %s", ty, nm);
                }
            }
            cc__append_str(&defs, &defs_len, &defs_cap, ") {\n");
            cc__append_fmt(&defs, &defs_len, &defs_cap,
                           "  CC_CLOSURE_ENV_ALLOC(%s_env, __env);\n",
                           d->sym_base);
            /* Cast opaque void* params back to their concrete types.  These
             * casts are safe because the definitions live at end-of-file where
             * all user-defined types are in scope. */
            for (int ci = 0; ci < d->cap_count; ci++) {
                int is_ref = (d->cap_flags && (d->cap_flags[ci] & 4) != 0);
                const char* ty = d->cap_types[ci] ? d->cap_types[ci] : "int";
                const char* nm = d->cap_names[ci] ? d->cap_names[ci] : "__cap";
                if (cc__capture_needs_opaque(is_ref, ty)) {
                    if (is_ref) {
                        cc__append_fmt(&defs, &defs_len, &defs_cap,
                                       "  __typeof__((%s*)0) %s = (__typeof__((%s*)0))__cc_opaque_%s;\n",
                                       ty, nm, ty, nm);
                    } else {
                        cc__append_fmt(&defs, &defs_len, &defs_cap,
                                       "  __typeof__((%s)0) %s = (__typeof__((%s)0))__cc_opaque_%s;\n",
                                       ty, nm, ty, nm);
                    }
                }
            }
            for (int ci = 0; ci < d->cap_count; ci++) {
                cc__append_fmt(&defs, &defs_len, &defs_cap,
                               "  __env->%s = %s;\n",
                               d->cap_names[ci] ? d->cap_names[ci] : "__cap",
                               d->cap_names[ci] ? d->cap_names[ci] : "__cap");
            }
            /* TSan release: ensure captured values are visible to worker thread */
            cc__append_str(&defs, &defs_len, &defs_cap, "  CC_TSAN_RELEASE(__env);\n");
            cc__append_fmt(&defs, &defs_len, &defs_cap,
                           "  return %s(%s_entry, __env, %s_env_drop);\n",
                           mkfn, d->sym_base, d->sym_base);
            cc__append_str(&defs, &defs_len, &defs_cap, "}\n");

            cc__append_fmt(&defs, &defs_len, &defs_cap, "static %s %s_make_nursery(CCNursery* __cc_nursery", cty, d->sym_base);
            for (int ci = 0; ci < d->cap_count; ci++) {
                cc__append_str(&defs, &defs_len, &defs_cap, ", ");
                int is_ref = (d->cap_flags && (d->cap_flags[ci] & 4) != 0);
                const char* ty = d->cap_types[ci] ? d->cap_types[ci] : "int";
                const char* nm = d->cap_names[ci] ? d->cap_names[ci] : "__cap";
                if (cc__capture_needs_opaque(is_ref, ty)) {
                    cc__append_fmt(&defs, &defs_len, &defs_cap, "%s __cc_opaque_%s",
                                   cc__capture_needs_const_opaque(is_ref, ty) ? "const void*" : "void*",
                                   nm);
                } else if (is_ref) {
                    cc__append_fmt(&defs, &defs_len, &defs_cap, "%s* %s", ty, nm);
                } else {
                    cc__append_fmt(&defs, &defs_len, &defs_cap, "%s %s", ty, nm);
                }
            }
            cc__append_str(&defs, &defs_len, &defs_cap, ") {\n");
            cc__append_fmt(&defs, &defs_len, &defs_cap,
                           "  CC_CLOSURE_ENV_NURSERY_ALLOC(__cc_nursery, %s_env, __env);\n",
                           d->sym_base);
            for (int ci = 0; ci < d->cap_count; ci++) {
                int is_ref = (d->cap_flags && (d->cap_flags[ci] & 4) != 0);
                const char* ty = d->cap_types[ci] ? d->cap_types[ci] : "int";
                const char* nm = d->cap_names[ci] ? d->cap_names[ci] : "__cap";
                if (cc__capture_needs_opaque(is_ref, ty)) {
                    if (is_ref) {
                        cc__append_fmt(&defs, &defs_len, &defs_cap,
                                       "  __typeof__((%s*)0) %s = (__typeof__((%s*)0))__cc_opaque_%s;\n",
                                       ty, nm, ty, nm);
                    } else {
                        cc__append_fmt(&defs, &defs_len, &defs_cap,
                                       "  __typeof__((%s)0) %s = (__typeof__((%s)0))__cc_opaque_%s;\n",
                                       ty, nm, ty, nm);
                    }
                }
            }
            for (int ci = 0; ci < d->cap_count; ci++) {
                cc__append_fmt(&defs, &defs_len, &defs_cap,
                               "  __env->%s = %s;\n",
                               d->cap_names[ci] ? d->cap_names[ci] : "__cap",
                               d->cap_names[ci] ? d->cap_names[ci] : "__cap");
            }
            cc__append_str(&defs, &defs_len, &defs_cap, "  CC_TSAN_RELEASE(__env);\n");
            cc__append_fmt(&defs, &defs_len, &defs_cap,
                           "  return %s(%s_entry, __env, %s_env_nursery_drop);\n",
                           mkfn, d->sym_base, d->sym_base);
            cc__append_str(&defs, &defs_len, &defs_cap, "}\n");
        } else {
            /* Inline what `CC_CLOSURE0_SIMPLE` would have produced — see note
             * at the proto cluster above re: location-tagged base symbol. */
            if (d->param_count == 0) {
                cc__append_fmt(&defs, &defs_len, &defs_cap,
                               "static CCClosure0 %s_make(void) { return cc_closure0_make(%s_entry, NULL, NULL); }\n",
                               d->sym_base, d->sym_base);
                cc__append_fmt(&defs, &defs_len, &defs_cap,
                               "static void* %s_entry(void* __p) {\n  (void)__p;\n",
                               d->sym_base);
            } else {
                cc__append_fmt(&defs, &defs_len, &defs_cap,
                               "static %s %s_make(void) { return %s(%s_entry, NULL, NULL); }\n",
                               cty, d->sym_base, mkfn, d->sym_base);
                cc__append_fmt(&defs, &defs_len, &defs_cap,
                               "static %s %s_make_nursery(CCNursery* __cc_nursery) { (void)__cc_nursery; return %s_make(); }\n",
                               cty, d->sym_base, d->sym_base);
            }
        }

        /* defs: entry function body. For simple CCClosure0 (no params, no
         * captures), the entry-fn header was already emitted inline above
         * (right after the make function) along with `(void)__p;` — so the
         * loop here only emits headers for the other variants. */
        int simple_closure0 = (d->param_count == 0 && d->cap_count == 0);
        if (!simple_closure0) {
            if (d->param_count == 0) {
                cc__append_fmt(&defs, &defs_len, &defs_cap, "static void* %s_entry(void* __p) {\n", d->sym_base);
            } else if (d->param_count == 1) {
                cc__append_fmt(&defs, &defs_len, &defs_cap, "static void* %s_entry(void* __p, intptr_t __arg0) {\n", d->sym_base);
            } else {
                cc__append_fmt(&defs, &defs_len, &defs_cap, "static void* %s_entry(void* __p, intptr_t __arg0, intptr_t __arg1) {\n", d->sym_base);
            }
        }
        if (d->cap_count > 0) {
            cc__append_fmt(&defs, &defs_len, &defs_cap, "  %s_env* __env = (%s_env*)__p;\n", d->sym_base, d->sym_base);
            cc__append_str(&defs, &defs_len, &defs_cap, "  CC_TSAN_ACQUIRE(__env);\n");
            for (int ci = 0; ci < d->cap_count; ci++) {
                int is_ref = (d->cap_flags && (d->cap_flags[ci] & 4) != 0);
                const char* ty = d->cap_types[ci] ? d->cap_types[ci] : "int";
                const char* nm = d->cap_names[ci] ? d->cap_names[ci] : "__cap";
                if (is_ref) {
                    /* Reference capture: dereference the stored pointer.
                       Use a local reference-like alias. */
                    if (cc__capture_needs_opaque(is_ref, ty)) {
                        cc__append_fmt(&defs, &defs_len, &defs_cap,
                                       "  __typeof__((%s*)0) __cc_ref_%s = (__typeof__((%s*)0))__env->%s;\n",
                                       ty, nm, ty, nm);
                    } else {
                        cc__append_fmt(&defs, &defs_len, &defs_cap,
                                       "  %s* __cc_ref_%s = __env->%s;\n", ty, nm, nm);
                    }
                    cc__append_fmt(&defs, &defs_len, &defs_cap, "#define %s (*__cc_ref_%s)\n", nm, nm);
                } else {
                    if (cc__capture_needs_opaque(is_ref, ty)) {
                        cc__append_fmt(&defs, &defs_len, &defs_cap,
                                       "  __typeof__((%s)0) %s = (__typeof__((%s)0))__env->%s;\n",
                                       ty, nm, ty, nm);
                    } else {
                        cc__append_fmt(&defs, &defs_len, &defs_cap,
                                       "  %s %s = __env->%s;\n", ty, nm, nm);
                    }
                }
            }
        } else if (!simple_closure0) {
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
        /* Map diagnostics within the closure body back to the original source
         * location (ledger-aware user line; see above). */
        {
            char rel[1024];
            int user_line = cc_user_line_for_offset(in_src, in_len, d->start_off, 1, NULL, NULL);
            cc__append_fmt(&defs, &defs_len, &defs_cap, "#line %d \"%s\"\n",
                           user_line,
                           cc_path_rel_to_repo(ctx->input_path ? ctx->input_path : "<input>", rel, sizeof(rel)));
        }
        if (lowered_body && lowered_body[0] == '{') {
            char* lowered2 = cc__lower_nursery_spawn_in_body_text(d->id, lowered_body);
            if (!lowered2) lowered2 = strdup(lowered_body);
            char* lowered_tpl = cc_rewrite_string_templates_text(lowered2, strlen(lowered2),
                                                                 ctx ? ctx->input_path : NULL);
            const char* src_for_templates = lowered_tpl ? lowered_tpl : lowered2;
            /* Rewrite T!E result types in closure body */
            char* lowered3 = cc__rewrite_result_types_text(ctx, src_for_templates, strlen(src_for_templates));
            char* lowered_err = NULL;
            size_t lowered_err_len = 0;
            const char* src_after_err = lowered3 ? lowered3 : src_for_templates;
            size_t src_after_err_n = strlen(src_after_err);
            char* lowered_ru = NULL;
            size_t lowered_ru_len = 0;
            if (cc__rewrite_result_unwrap(ctx, src_after_err, src_after_err_n, &lowered_ru, &lowered_ru_len) > 0 &&
                lowered_ru) {
                src_after_err = lowered_ru;
                src_after_err_n = lowered_ru_len;
            }
            if (cc__rewrite_err_syntax(ctx, src_after_err, src_after_err_n, &lowered_err, &lowered_err_len) > 0 &&
                lowered_err) {
                src_after_err = lowered_err;
                src_after_err_n = lowered_err_len;
            }
            /* Rewrite @defer in closure body */
            char* lowered4 = NULL;
            size_t lowered4_len = 0;
            const char* src_for_defer = src_after_err;
            if (cc__rewrite_defer_syntax(ctx, src_for_defer, src_after_err_n, &lowered4, &lowered4_len) > 0 && lowered4) {
                /* Rewrite captured closure calls in the body */
                char* lowered5 = cc__rewrite_captured_closure_calls_in_body(lowered4, d);
                if (lowered5) {
                    cc__append_fmt(&defs, &defs_len, &defs_cap, "  %s\n", lowered5);
                    free(lowered5);
                } else {
                    cc__append_fmt(&defs, &defs_len, &defs_cap, "  %s\n", lowered4);
                }
                free(lowered4);
            } else {
                /* Rewrite captured closure calls in the body */
                char* lowered5 = cc__rewrite_captured_closure_calls_in_body(src_for_defer, d);
                if (lowered5) {
                    cc__append_fmt(&defs, &defs_len, &defs_cap, "  %s\n", lowered5);
                    free(lowered5);
                } else {
                    cc__append_fmt(&defs, &defs_len, &defs_cap, "  %s\n", src_for_defer);
                }
            }
            free(lowered_err);
            free(lowered_ru);
            free(lowered_tpl);
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
        if (simple_closure0) {
            cc__append_fmt(&defs, &defs_len, &defs_cap,
                           "static CCClosure0 %s_make_nursery(CCNursery* __cc_nursery) { (void)__cc_nursery; return %s_make(); }\n\n",
                           d->sym_base, d->sym_base);
        }

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
            /* File-scope forward decls written into `*out_protos` are the
             * only proto-placement strategy.  The caller places them after
             * `#include`s and before the first function definition via
             * `find_protos_insertion_point` (see edit_buffer.c).  The
             * legacy in-source walker `cc__closure_proto_insert_off` —
             * which tried to land `static T fn();` decls at the top of
             * the closure's enclosing function body — was deleted
             * 2026-05-28 along with its `skip_inline_protos=0` opt-out
             * (the walker was brittle when closures sat inside `if`/`for`
             * blocks because `static` decls are not allowed at block
             * scope per C99 6.7.1). */
            edits[en++] = (Edit){ .start = d->start_off, .end = d->end_off, .repl = call };
            /* The literal spanned N source lines; the make-call is one
             * line, which would shift every diagnostic below by N-1
             * (oracle: diag_oracle_closure_fail).  Keep the call on one
             * clean line and drop a masked #line resync AFTER the
             * statement's line instead — human-shaped emitted C, exact
             * coordinates (unmasked at write time).  in_src is 1:1 with
             * user lines at this stage. */
            {
                size_t span_nl = 0;
                for (size_t b = d->start_off; b < d->end_off; b++)
                    if (in_src[b] == '\n') span_nl++;
                if (span_nl > 0 && en < (int)(sizeof(edits) / sizeof(edits[0]))) {
                    size_t nl = d->end_off;
                    while (nl < in_len && in_src[nl] != '\n') nl++;
                    /* Ledger-aware USER line of the line AFTER the stmt
                     * (raw newline counts run high below upstream
                     * expansions like @grammar; honor #line/CC_LN). */
                    int next_line = cc_user_line_for_offset(in_src, in_len, nl, 1, NULL, NULL);
                    if (nl < in_len) next_line++; /* marker anchors the line AFTER the stmt */
                    char mark[1152];
                    int mn = snprintf(mark, sizeof(mark), "/*CC_LN %d %s*/\n", next_line,
                                      (ctx && ctx->input_path) ? ctx->input_path : "<cc_input>");
                    if (mn > 0 && (size_t)mn < sizeof(mark)) {
                        size_t ins_at = (nl < in_len) ? nl + 1 : in_len;
                        char* mrepl = strdup(mark);
                        if (mrepl) {
                            edits[en++] = (Edit){ .start = ins_at, .end = ins_at, .repl = mrepl };
                        }
                    }
                }
            }
        } else {
            free(call);
        }
    }
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

    free(decl_carry);
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

/* `cc__collect_closure_edits` (fake-per-span CCEditBuffer wrapper around
 * `cc__rewrite_closure_literals_with_nodes`) was deleted 2026-05-28 — it had
 * zero callers and would have emitted a wholesale `[0, src_len)` edit anyway,
 * which doesn't compose with the genuinely per-span Phase-3 stage-2 batch.
 * The whole-file API above is the only supported closure-literal entry point;
 * see `visit_codegen.c` Phase 5 and ARCHITECTURE.md §6. */