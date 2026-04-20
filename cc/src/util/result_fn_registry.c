#include "util/result_fn_registry.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

/* Small additive set keyed by function name. */
typedef struct {
    char**  names;
    size_t  len;
    size_t  cap;
} cc_name_set;

static cc_name_set g_result_fns = { NULL, 0, 0 };
static cc_name_set g_pointer_fns = { NULL, 0, 0 };

static void cc__name_set_clear(cc_name_set* s) {
    for (size_t i = 0; i < s->len; i++) free(s->names[i]);
    free(s->names);
    s->names = NULL;
    s->len = 0;
    s->cap = 0;
}

static int cc__name_set_contains(const cc_name_set* s, const char* name, size_t len) {
    if (!name || len == 0) return 0;
    for (size_t i = 0; i < s->len; i++) {
        const char* n = s->names[i];
        if (!n) continue;
        size_t sl = strlen(n);
        if (sl == len && memcmp(n, name, len) == 0) return 1;
    }
    return 0;
}

static void cc__name_set_add(cc_name_set* s, const char* name, size_t len) {
    if (!name || len == 0) return;
    if (cc__name_set_contains(s, name, len)) return;
    if (s->len == s->cap) {
        size_t nc = s->cap ? s->cap * 2 : 16;
        char** nb = (char**)realloc(s->names, nc * sizeof(char*));
        if (!nb) return;
        s->names = nb;
        s->cap = nc;
    }
    char* dup = (char*)malloc(len + 1);
    if (!dup) return;
    memcpy(dup, name, len);
    dup[len] = 0;
    s->names[s->len++] = dup;
}

void cc_result_fn_registry_clear(void)                         { cc__name_set_clear(&g_result_fns); }
int  cc_result_fn_registry_contains(const char* n, size_t l)   { return cc__name_set_contains(&g_result_fns, n, l); }
void cc_result_fn_registry_add(const char* n, size_t l)        { cc__name_set_add(&g_result_fns, n, l); }

void cc_pointer_fn_registry_clear(void)                        { cc__name_set_clear(&g_pointer_fns); }
int  cc_pointer_fn_registry_contains(const char* n, size_t l)  { return cc__name_set_contains(&g_pointer_fns, n, l); }
void cc_pointer_fn_registry_add(const char* n, size_t l)       { cc__name_set_add(&g_pointer_fns, n, l); }

/* -----------------------------------------------------------------
 * Pointer-fn scanner.
 *
 * Walks raw C source text looking for declarations whose return type
 * contains a `*` and whose last-token-before-`(` is an identifier (the
 * function name).  Skips strings, char literals, line/block comments,
 * and preprocessor directives.  Recognizes statement boundaries as
 * `;`, `{`, `}`, and SOF.
 *
 * Intentionally conservative: false negatives (missing a pointer fn)
 * degrade to the existing Result-only lowering; false positives would
 * silently corrupt the lowering for a Result-returning call.  The
 * heuristic therefore rejects anything that looks like an initializer
 * (`= ...`), an expression-position call, a function-pointer typedef
 * (`T (*name)(...)`), `typedef` itself, and control keywords.
 * ----------------------------------------------------------------- */

static int cc__is_ident_start(char c) {
    return (c == '_') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}
static int cc__is_ident_char(char c) {
    return cc__is_ident_start(c) || (c >= '0' && c <= '9');
}

/* Skip ws, block comments, line comments forward.  Returns new index. */
static size_t cc__pfs_skip_ws_comments(const char* s, size_t n, size_t i) {
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

/* Skip a string or char literal starting at `*i` (which points at the
 * opening quote).  On return `*i` points just past the closing quote. */
static void cc__pfs_skip_string(const char* s, size_t n, size_t* i) {
    char q = s[*i];
    (*i)++;
    while (*i < n && s[*i] != q) {
        if (s[*i] == '\\' && *i + 1 < n) { *i += 2; continue; }
        (*i)++;
    }
    if (*i < n) (*i)++;
}

/* Skip a preprocessor line.  `*i` points at the `#`.  On return `*i`
 * points just past the terminating (unescaped) newline. */
static void cc__pfs_skip_pp_line(const char* s, size_t n, size_t* i) {
    while (*i < n) {
        if (s[*i] == '\\' && *i + 1 < n && s[*i + 1] == '\n') { *i += 2; continue; }
        if (s[*i] == '\n') { (*i)++; return; }
        (*i)++;
    }
}

/* Recognize identifiers that cannot start a function declaration; if
 * the first token after a statement boundary is one of these, skip
 * ahead.  Keeps the heuristic tight against control statements and
 * storage forms we do not want to treat as pointer-returning fns. */
static int cc__pfs_is_stmt_kw(const char* s, size_t a, size_t b) {
    size_t len = b - a;
    static const char* kws[] = {
        "if", "else", "while", "for", "do", "switch", "case", "default",
        "break", "continue", "return", "goto", "typedef", "sizeof",
        "_Alignof", "alignof", "_Static_assert", "__asm__", "asm",
        NULL,
    };
    for (size_t k = 0; kws[k]; k++) {
        size_t kl = strlen(kws[k]);
        if (kl == len && memcmp(s + a, kws[k], len) == 0) return 1;
    }
    return 0;
}

/* Try to parse a single declaration starting at `start` (which is at
 * or past a statement boundary, positioned at the first non-ws/comment
 * byte).  On success — i.e. the tokens form `TYPE... * NAME (` — add
 * NAME to the pointer-fn set.  Returns the advanced position either
 * way; the caller resumes scanning from there. */
static size_t cc__pfs_try_decl(const char* s, size_t n, size_t start) {
    size_t j = start;
    int seen_star = 0;
    int ident_count = 0;
    size_t last_ident_a = 0, last_ident_b = 0;
    int first_tok = 1;

    while (j < n) {
        j = cc__pfs_skip_ws_comments(s, n, j);
        if (j >= n) return j;
        char c = s[j];

        if (c == ';' || c == '{' || c == '}' || c == ',' || c == '=' ||
            c == '[' || c == ':' || c == '?' || c == '<' || c == '>' ||
            c == '+' || c == '-' || c == '%' || c == '&' || c == '|' ||
            c == '^' || c == '!' || c == '~') {
            return j;
        }
        if (c == '#') { cc__pfs_skip_pp_line(s, n, &j); return j; }
        if (c == '"' || c == '\'') { cc__pfs_skip_string(s, n, &j); return j; }

        if (c == '*') { seen_star = 1; j++; continue; }

        if (c == '(') {
            /* Require at least two identifiers so the leading type has at
             * least one token ("T *name(" / "struct T *name(" / "const T
             * *name(" all qualify; bare "*name(" — a dereference of a call
             * — does not). */
            if (ident_count >= 2 && seen_star) {
                cc_pointer_fn_registry_add(s + last_ident_a,
                                            last_ident_b - last_ident_a);
            }
            return j;
        }

        if (cc__is_ident_start(c)) {
            size_t a = j;
            while (j < n && cc__is_ident_char(s[j])) j++;
            size_t b = j;
            if (first_tok && cc__pfs_is_stmt_kw(s, a, b)) return j;
            first_tok = 0;
            last_ident_a = a;
            last_ident_b = b;
            ident_count++;
            continue;
        }

        if (c == '/') { j++; continue; }
        j++;
    }
    return j;
}

void cc_pointer_fn_registry_scan(const char* s, size_t n) {
    if (!s || n == 0) return;
    size_t i = 0;
    int at_boundary = 1;
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
        if (c == '"' || c == '\'') { cc__pfs_skip_string(s, n, &i); continue; }
        if (c == '#' && at_boundary) { cc__pfs_skip_pp_line(s, n, &i); continue; }

        if (c == ';' || c == '{' || c == '}') {
            at_boundary = 1;
            i++;
            continue;
        }

        if (at_boundary) {
            at_boundary = 0;
            size_t end = cc__pfs_try_decl(s, n, i);
            i = (end > i) ? end : i + 1;
            continue;
        }
        i++;
    }
}
