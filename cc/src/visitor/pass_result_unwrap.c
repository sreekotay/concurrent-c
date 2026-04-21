#include "pass_result_unwrap.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ir/ir.h"
#include "ir/verifier.h"
#include "util/path.h"
#include "util/result_fn_registry.h"
#include "util/text.h"
#include "visitor/pass_common.h"
#include "visitor/visitor.h"

#define cc__append_n cc_sb_append
#define cc__append_str cc_sb_append_cstr
CC_DEFINE_SB_APPEND_FMT

/* Extract the callee identifier from a plain `ident(...)` call expression.
 *
 * Writes the identifier (NUL-terminated) into `out` if the call is of the
 * form `IDENT(...)` with optional whitespace between `IDENT` and `(`.
 * Returns 1 on success, 0 if the call is not a plain name call (e.g.
 * `obj.method(...)`, `(f)(...)`, `ptr->method(...)`, or the trimmed text
 * does not end with `)`).  Used to look up the declared error type of the
 * callee in `cc_result_fn_registry_get_err_type`. */
static int cc__ru_extract_plain_callee(const char* s, size_t a, size_t b,
                                        char* out, size_t out_sz) {
    if (!out || out_sz == 0) return 0;
    out[0] = 0;
    while (a < b && isspace((unsigned char)s[a])) a++;
    while (b > a && isspace((unsigned char)s[b - 1])) b--;
    if (a >= b) return 0;
    if (s[b - 1] != ')') return 0;
    if (!cc_is_ident_start(s[a])) return 0;
    size_t name_a = a;
    size_t name_b = a;
    while (name_b < b && cc_is_ident_char(s[name_b])) name_b++;
    if (name_b == name_a) return 0;
    size_t k = name_b;
    while (k < b && isspace((unsigned char)s[k])) k++;
    if (k >= b || s[k] != '(') return 0;
    size_t name_len = name_b - name_a;
    if (name_len + 1 > out_sz) return 0;
    memcpy(out, s + name_a, name_len);
    out[name_len] = 0;
    return 1;
}

/* Emit the error-binder declaration for an unwrap expansion.
 *
 * Emit the "error binder" line for the unified unwrap lowering:
 *   __typeof__(__cc_uw_err_at(tmpv, "expr", "file", "line")) binder =
 *       __cc_uw_err_at(tmpv, "expr", "file", "line");
 *
 * The `_Generic`-backed `__cc_uw_err_at` macro resolves at compile time to
 *   - `(tmpv).u.error` when `tmpv` is a Result struct, and
 *   - `__cc_err_null_at(...)` (yielding a `CCError`) when `tmpv` is a
 *     raw pointer.
 *
 * So a single lowering call shape works for BOTH the Result-struct and
 * pointer-returning-call variants of `!>` / `?>` — no source-scan needed
 * to pick between them at pass time. */
static size_t cc__skip_ws_comments_forward(const char* s, size_t n, size_t i);

static void cc__ru_emit_uw_err_binder(char** out, size_t* ol, size_t* oc,
                                       const char* s, size_t call_a, size_t call_b,
                                       const char* tmpv, const char* binder,
                                       const char* file, int line) {
    /* Prefer the typed binder path when the callee is a plain name whose
     * declared error type we tracked in the result-fn registry.  This is
     * what makes `!>(e) report(e)` type-check in parser mode when `report`
     * expects the declared error type (e.g. `CCIoError`) rather than the
     * generic parser-mode error struct. */
    char callee[128];
    char err_type[256];
    if (cc__ru_extract_plain_callee(s, call_a, call_b, callee, sizeof(callee)) &&
        cc_result_fn_registry_get_err_type(callee, strlen(callee),
                                            err_type, sizeof(err_type))) {
        cc_sb_append_fmt(out, ol, oc,
                         "%s %s = *(%s*)(void*)&((%s).u.error); ",
                         err_type, binder, err_type, tmpv);
        return;
    }
    /* Fallback: untyped callee (method call, chained expression, or a
     * pointer-returning expression with no registry entry).  Use
     * `__typeof__(__cc_uw_err_at(...))` so the binder resolves to:
     *   - the Result struct's declared error field for Result LHS, and
     *   - `CCError` for raw-pointer LHS (via the default _Generic arm). */
    cc__append_str(out, ol, oc, "__typeof__(");
    cc_sb_append_uw_err_at(out, ol, oc, tmpv, s, call_a, call_b, file, line);
    cc_sb_append_fmt(out, ol, oc, ") %s = ", binder);
    cc_sb_append_uw_err_at(out, ol, oc, tmpv, s, call_a, call_b, file, line);
    cc__append_str(out, ol, oc, "; ");
}

/* Mangle the user's `!>(e) BODY` binder into a `__cc_pu_bind_<id>_<name>`
 * identifier.  The `__cc_pu_` prefix already matches `async_ast`'s
 * no-frame-lift rule for unwrap-pass temporaries, so the mangled binder
 * stays a true local inside `@async` bodies (bug [F9]) — without the
 * mangling `async_ast` would frame-lift the binder and turn
 * `TYPE e = ...;` into the invalid `TYPE __f->e = ...;` after the
 * identifier-rewrite pass.
 *
 * `src[0..n)` is the already-processed body text; every word-bounded
 * occurrence of `binder` (skipping comments / string literals) is
 * rewritten to the mangled name.  Caller owns the returned buffer.
 *
 * See docs/known-bugs/redis_idiomatic_async.md — [F9]. */
static int cc__pu_mangle_binder_in_body(const char* src, size_t n,
                                        const char* binder, const char* mangled,
                                        char** out_buf, size_t* out_len) {
    size_t from_n = strlen(binder);
    size_t to_n = strlen(mangled);
    if (from_n == 0) {
        char* buf = (char*)malloc(n + 1);
        if (!buf) return -1;
        if (n) memcpy(buf, src, n);
        buf[n] = 0;
        *out_buf = buf;
        *out_len = n;
        return 0;
    }
    char* out = NULL;
    size_t ol = 0, oc = 0;
    size_t p = 0;
    while (p < n) {
        size_t found = cc_find_ident_top_level(src, p, n, binder, from_n);
        if (found >= n) {
            cc__append_n(&out, &ol, &oc, src + p, n - p);
            break;
        }
        cc__append_n(&out, &ol, &oc, src + p, found - p);
        cc__append_n(&out, &ol, &oc, mangled, to_n);
        p = found + from_n;
    }
    if (!out) {
        out = (char*)malloc(1);
        if (!out) return -1;
        out[0] = 0;
    }
    *out_buf = out;
    *out_len = ol;
    return 0;
}

/* ------------------------------------------------------------------
 * `?>` — default-value expression operator (Swift/C# `??`, Kotlin `?:`).
 *
 * Recognized forms (shorthand: TMP = __cc_pu_r_N):
 *
 *   Case 1  EXPR ?> DEFAULT_EXPR
 *       => ({ __typeof__(EXPR) TMP = (EXPR);
 *             cc_is_ok(TMP) ? cc_value(TMP) : (DEFAULT_EXPR); })
 *
 *   Case 2  EXPR ?>(e) RHS_EXPR
 *       => ({ __typeof__(EXPR) TMP = (EXPR);
 *             cc_is_ok(TMP) ? cc_value(TMP)
 *                           : ({ __typeof__(cc_error(TMP)) e = cc_error(TMP);
 *                                (RHS_EXPR); }); })
 *
 * `?>` is strictly a VALUE operator.  Its RHS must be a pure C expression
 * that produces a `T`.  Divergent statements (`return`, `break`, `continue`,
 * `goto`, `@err(IDENT);`, noreturn calls) and block bodies `{ ... }` on the
 * RHS are compile errors — those are the province of `!>` at expression
 * position (see below).
 *
 * Scanning strategy:
 *   - Forward-scan comment/string-aware for the first `?>`.
 *   - Backward-scan from the operator to an expression-start boundary
 *     (`;`, `{`, `}`, `,`, `=` (not `==`/`!=`/`<=`/`>=`), `(`, `?`, `:`,
 *     `&&`, `||`, or SOF) with balanced paren/bracket/brace tracking.
 *   - Optionally consume `(ident)` as a binder.  If the `(...)` contents
 *     are not a bare identifier we leave the `(` alone and treat it as
 *     the start of a parenthesized RHS expression; this keeps
 *     `?> (7 + 8)` and similar parenthesized defaults working.
 *   - If the next non-ws token starts an error-handling shape
 *     (`return`/`break`/`continue`/`goto`/`{`/`@err`), emit a diagnostic
 *     steering the user to `!>`.  Otherwise scan forward as a C
 *     expression up to the usual RHS end-markers.
 *   - Splice in the statement-expression lowering and restart until no
 *     `?>` remains.
 * ---------------------------------------------------------------- */

/* Forward declaration: full definition lives later in the file alongside
 * the other block/divergence helpers.  Used by the expression-position
 * `!>` handler for block-body tail-divergence checks. */
static int cc__pu_body_diverges(const char* body, size_t blen);

/* Scan forward to find the byte index of the first `?>` that is not inside
 * a comment or string literal.  Returns 1 and writes *out_pos on success,
 * 0 if no such occurrence exists. */
static int cc__find_unwrap_token(const char* s, size_t n, size_t* out_pos) {
    int in_str = 0;
    char qch = 0;
    int in_line_comment = 0;
    int in_block_comment = 0;
    for (size_t i = 0; i < n; i++) {
        char ch = s[i];
        if (in_line_comment) {
            if (ch == '\n') in_line_comment = 0;
            continue;
        }
        if (in_block_comment) {
            if (ch == '*' && i + 1 < n && s[i + 1] == '/') {
                in_block_comment = 0;
                i++;
            }
            continue;
        }
        if (in_str) {
            if (ch == '\\' && i + 1 < n) { i++; continue; }
            if (ch == qch) in_str = 0;
            continue;
        }
        if (ch == '/' && i + 1 < n && s[i + 1] == '/') {
            in_line_comment = 1; i++; continue;
        }
        if (ch == '/' && i + 1 < n && s[i + 1] == '*') {
            in_block_comment = 1; i++; continue;
        }
        if (ch == '"' || ch == '\'') { in_str = 1; qch = ch; continue; }
        if (ch == '?' && i + 1 < n && s[i + 1] == '>') {
            /* Reject if the preceding non-ws char suggests we misparsed a
             * different operator (e.g. '??>' trigraph / digraph lookalikes).
             * In practice the CC source does not emit those so a literal
             * match is fine, but we still guard `?>?` style weirdness. */
            if (out_pos) *out_pos = i;
            return 1;
        }
    }
    return 0;
}

/* Check if the `=` byte at position `pos` is part of a compound operator
 * that should NOT act as an expression boundary for our purposes.
 * Compound assignments (`+=`, `-=`, `*=`, ...) still reset the RHS, but
 * equality/relational operators (`==`, `!=`, `<=`, `>=`) do not, because
 * `?>` is meant to bind tighter than them. We treat any `=` whose preceding
 * char is one of `=`, `!`, `<`, `>` as NOT-a-boundary. Preceding `+`, `-`,
 * `*`, `/`, `%`, `|`, `&`, `^` keep `=` as a boundary (assignment). */
static int cc__eq_is_boundary(const char* s, size_t n, size_t pos) {
    if (pos == 0) return 1;
    char p = s[pos - 1];
    if (p == '=' || p == '!' || p == '<' || p == '>') return 0;
    /* Also skip the second `=` in `==`: if next char is `=`, this is the
     * first char of `==` and should not split. */
    if (pos + 1 < n && s[pos + 1] == '=') return 0;
    (void)n;
    return 1;
}

/* Skip a string/char literal scanning BACKWARD. On entry *i points at the
 * closing quote char; on return *i points at the opening quote char (or 0
 * if we ran off the start). Best effort; C escape handling backward is
 * ambiguous but string contents almost never contain `?>` so the worst
 * case is an early stop on a mis-identified quote. */
static void cc__skip_str_backward(const char* s, size_t* i) {
    char q = s[*i];
    if (*i == 0) return;
    size_t k = *i;
    while (k > 0) {
        k--;
        if (s[k] == q) {
            /* Check for escape: count trailing backslashes. */
            size_t bs = 0;
            size_t m = k;
            while (m > 0 && s[m - 1] == '\\') { bs++; m--; }
            if ((bs & 1) == 0) { *i = k; return; }
        }
    }
    *i = 0;
}

/* Skip a block comment scanning BACKWARD.  On entry *i points at the `/`
 * of the closing delimiter; the caller has verified s[*i - 1] == '*'.
 * On return *i points at the `/` of the opening delimiter, or 0 if we
 * ran off the start without finding one.  Needed to keep the LHS scan
 * from mis-parsing block-comment contents as real code — a block comment
 * containing braces, parens, or semicolons is otherwise indistinguishable
 * from surrounding source and blew past the enclosing statement boundary
 * (see examples/hello.ccs reproducer). */
static void cc__skip_block_comment_backward(const char* s, size_t* i) {
    if (*i < 1) { *i = 0; return; }
    size_t k = *i - 1;
    while (k > 0) {
        k--;
        if (s[k] == '/' && k + 1 < *i && s[k + 1] == '*') {
            *i = k;
            return;
        }
    }
    *i = 0;
}

/* Check whether the scanner is currently inside a single-line `//` comment
 * on the line that contains position `pos`.  We walk back from `pos` to
 * the previous newline (or SOF); if we find a `//` (not inside a string)
 * before reaching `pos`, then `pos` sits inside a line comment and should
 * be skipped.  Strings on the same line are respected via a simple forward
 * re-scan from line start. */
static int cc__pos_in_line_comment(const char* s, size_t pos) {
    size_t line_start = pos;
    while (line_start > 0 && s[line_start - 1] != '\n') line_start--;
    int in_str = 0;
    char qch = 0;
    for (size_t k = line_start; k < pos; k++) {
        char c = s[k];
        if (in_str) {
            if (c == '\\' && k + 1 < pos) { k++; continue; }
            if (c == qch) in_str = 0;
            continue;
        }
        if (c == '"' || c == '\'') { in_str = 1; qch = c; continue; }
        if (c == '/' && k + 1 < pos && s[k + 1] == '/') return 1;
    }
    return 0;
}

/* Find the start of the LHS expression by scanning backward from `from`
 * (exclusive). Returns the position of the first byte of the LHS. */
static size_t cc__find_lhs_start_backward_raw(const char* s, size_t from) {
    int par = 0, brk = 0, br = 0;
    size_t i = from;
    while (i > 0) {
        i--;
        char c = s[i];
        /* Block comment: the closing delimiter seen backward is `/` at
         * s[i] preceded by `*` at s[i-1].  Skip back to the matching
         * opening delimiter. */
        if (c == '/' && i > 0 && s[i - 1] == '*') {
            cc__skip_block_comment_backward(s, &i);
            continue;
        }
        /* Line comment: if this byte lies inside a `// ...` on its line,
         * jump back to the line start so we don't misparse the comment
         * body. */
        if (c != '\n' && cc__pos_in_line_comment(s, i)) {
            while (i > 0 && s[i] != '\n') i--;
            continue;
        }
        if (c == '"' || c == '\'') {
            cc__skip_str_backward(s, &i);
            continue;
        }
        if (c == ')') { par++; continue; }
        if (c == '(') {
            if (par > 0) { par--; continue; }
            return i + 1;
        }
        if (c == ']') { brk++; continue; }
        if (c == '[') { if (brk > 0) brk--; continue; }
        if (c == '}') { br++; continue; }
        if (c == '{') {
            if (br > 0) { br--; continue; }
            return i + 1;
        }
        if (par > 0 || brk > 0 || br > 0) continue;

        if (c == ';' || c == ',') return i + 1;
        if (c == '?' || c == ':') return i + 1;
        if (c == '=' && cc__eq_is_boundary(s, from, i)) return i + 1;
        if (c == '&' && i > 0 && s[i - 1] == '&') return i + 1;
        if (c == '|' && i > 0 && s[i - 1] == '|') return i + 1;
    }
    return 0;
}

/* Wrapper over the raw scanner that additionally strips a leading
 * `return` keyword from the LHS.  `return EXPR ?> DEFAULT;` is a
 * statement, not an expression — the `return` belongs to the enclosing
 * statement and must not be swallowed into the ternary lowering's
 * `__typeof__(...)`.  By advancing `lhs_start` past `return`, the
 * existing emit logic keeps the keyword as prefix text and hands only
 * `EXPR` to the lowering. */
static size_t cc__find_lhs_start_backward(const char* s, size_t from) {
    size_t start = cc__find_lhs_start_backward_raw(s, from);
    size_t j = start;
    while (j < from && (s[j] == ' ' || s[j] == '\t' ||
                        s[j] == '\n' || s[j] == '\r')) j++;
    if (j + 6 <= from && memcmp(s + j, "return", 6) == 0 &&
        (j + 6 == from || !cc_is_ident_char(s[j + 6])) &&
        (j == 0 || !cc_is_ident_char(s[j - 1]))) {
        return j + 6;
    }
    return start;
}

/* Find the end (exclusive) of the RHS expression by scanning forward from
 * `from`. Returns the position of the first byte that is NOT part of the
 * RHS (i.e. the terminator). */
static int cc__find_rhs_end_forward(const char* s, size_t n, size_t from, size_t* out_end) {
    int par = 0, brk = 0, br = 0;
    int in_str = 0;
    char qch = 0;
    int in_line_comment = 0;
    int in_block_comment = 0;
    for (size_t i = from; i < n; i++) {
        char ch = s[i];
        char ch2 = (i + 1 < n) ? s[i + 1] : 0;
        if (in_line_comment) {
            if (ch == '\n') in_line_comment = 0;
            continue;
        }
        if (in_block_comment) {
            if (ch == '*' && ch2 == '/') { in_block_comment = 0; i++; }
            continue;
        }
        if (in_str) {
            if (ch == '\\' && i + 1 < n) { i++; continue; }
            if (ch == qch) in_str = 0;
            continue;
        }
        if (ch == '/' && ch2 == '/') { in_line_comment = 1; i++; continue; }
        if (ch == '/' && ch2 == '*') { in_block_comment = 1; i++; continue; }
        if (ch == '"' || ch == '\'') { in_str = 1; qch = ch; continue; }

        if (ch == '(') { par++; continue; }
        if (ch == '[') { brk++; continue; }
        if (ch == '{') { br++; continue; }
        if (ch == ')') {
            if (par == 0) { *out_end = i; return 1; }
            par--;
            continue;
        }
        if (ch == ']') {
            if (brk == 0) { *out_end = i; return 1; }
            brk--;
            continue;
        }
        if (ch == '}') {
            if (br == 0) { *out_end = i; return 1; }
            br--;
            continue;
        }
        if (par > 0 || brk > 0 || br > 0) continue;

        if (ch == ';' || ch == ',') { *out_end = i; return 1; }
        if (ch == '?') {
            /* Another `?>` at our depth ends the current RHS; a plain `?`
             * (ternary) also ends it because we bind tighter than `?:`. */
            *out_end = i; return 1;
        }
        if (ch == ':') { *out_end = i; return 1; }
        if (ch == '&' && ch2 == '&') { *out_end = i; return 1; }
        if (ch == '|' && ch2 == '|') { *out_end = i; return 1; }
    }
    *out_end = n;
    return 1;
}

static void cc__line_from_pos(const char* s, size_t pos, int* line) {
    int ln = 1;
    for (size_t i = 0; i < pos && s[i]; i++) {
        if (s[i] == '\n') ln++;
    }
    *line = ln;
}

static void cc__trim_range(const char* s, size_t* a, size_t* b) {
    /* Trim whitespace AND leading/trailing block/line comments from the
     * edges of [*a, *b).  Without stripping comments, the `!>` / `?>`
     * rewrites blank any preceding block comment along with the LHS
     * expression (see examples/hello.ccs: a block comment between
     * `int main() {` and `cc_nursery_create()` was being overwritten
     * by the replacement text's whitespace padding).  The edge-trim
     * keeps real comment text intact by advancing the range past it
     * so only the actual expression range is rewritten. */
    int progress = 1;
    while (progress && *a < *b) {
        progress = 0;
        while (*a < *b && isspace((unsigned char)s[*a])) { (*a)++; progress = 1; }
        if (*a + 1 < *b && s[*a] == '/' && s[*a + 1] == '*') {
            size_t k = *a + 2;
            while (k + 1 < *b && !(s[k] == '*' && s[k + 1] == '/')) k++;
            if (k + 1 < *b) { *a = k + 2; progress = 1; }
            else { *a = *b; progress = 1; }
        }
        if (*a + 1 < *b && s[*a] == '/' && s[*a + 1] == '/') {
            size_t k = *a + 2;
            while (k < *b && s[k] != '\n') k++;
            *a = k;
            progress = 1;
        }
    }
    progress = 1;
    while (progress && *b > *a) {
        progress = 0;
        while (*b > *a && isspace((unsigned char)s[*b - 1])) { (*b)--; progress = 1; }
        if (*b >= *a + 2 && s[*b - 1] == '/' && s[*b - 2] == '*') {
            size_t k = *b - 2;
            while (k > *a && !(s[k] == '/' && k + 1 < *b && s[k + 1] == '*')) k--;
            if (k >= *a && s[k] == '/' && k + 1 < *b && s[k + 1] == '*') {
                *b = k;
                progress = 1;
            }
        }
    }
}

/* Return 1 if the substring s[i..i+strlen(kw)) is exactly `kw` with no
 * identifier characters immediately before or after (word-boundary match). */
static int cc__match_ident_kw(const char* s, size_t n, size_t i, const char* kw) {
    size_t kn = strlen(kw);
    if (i + kn > n) return 0;
    if (memcmp(s + i, kw, kn) != 0) return 0;
    if (i > 0 && cc_is_ident_char(s[i - 1])) return 0;
    if (i + kn < n && cc_is_ident_char(s[i + kn])) return 0;
    return 1;
}

/* Return 1 if the byte range s[a..b) is a valid (non-empty) C identifier. */
static int cc__range_is_ident(const char* s, size_t a, size_t b) {
    if (b <= a) return 0;
    if (!cc_is_ident_start(s[a])) return 0;
    for (size_t k = a + 1; k < b; k++) {
        if (!cc_is_ident_char(s[k])) return 0;
    }
    return 1;
}

/* Scan forward from `from` to the next `;` at depth 0 (paren / bracket /
 * brace balanced, string/comment aware).  Leaving the current depth without
 * finding a `;` (e.g. running into an unmatched closing paren/brace) is
 * considered failure.  Returns 1 on success with *out_semi set, 0 otherwise.
 */
static int cc__find_semi_forward(const char* s, size_t n, size_t from, size_t* out_semi) {
    int par = 0, brk = 0, br = 0;
    int in_str = 0;
    char qch = 0;
    int in_line_comment = 0;
    int in_block_comment = 0;
    for (size_t i = from; i < n; i++) {
        char ch = s[i];
        char ch2 = (i + 1 < n) ? s[i + 1] : 0;
        if (in_line_comment) {
            if (ch == '\n') in_line_comment = 0;
            continue;
        }
        if (in_block_comment) {
            if (ch == '*' && ch2 == '/') { in_block_comment = 0; i++; }
            continue;
        }
        if (in_str) {
            if (ch == '\\' && i + 1 < n) { i++; continue; }
            if (ch == qch) in_str = 0;
            continue;
        }
        if (ch == '/' && ch2 == '/') { in_line_comment = 1; i++; continue; }
        if (ch == '/' && ch2 == '*') { in_block_comment = 1; i++; continue; }
        if (ch == '"' || ch == '\'') { in_str = 1; qch = ch; continue; }

        if (ch == '(') { par++; continue; }
        if (ch == '[') { brk++; continue; }
        if (ch == '{') { br++; continue; }
        if (ch == ')') { if (par == 0) return 0; par--; continue; }
        if (ch == ']') { if (brk == 0) return 0; brk--; continue; }
        if (ch == '}') { if (br == 0) return 0; br--; continue; }

        if (par == 0 && brk == 0 && br == 0 && ch == ';') {
            if (out_semi) *out_semi = i;
            return 1;
        }
    }
    return 0;
}

/* Single pass: find the first `?>` and rewrite it in place, emitting the
 * lowering. Returns 1 if a substitution was made, 0 if no `?>` was found,
 * -1 on error. */
static int cc__rewrite_result_unwrap_once(const CCVisitorCtx* ctx,
                                          const char* s,
                                          size_t n,
                                          char** out_buf,
                                          size_t* out_len) {
    size_t op_at = 0;
    if (!cc__find_unwrap_token(s, n, &op_at)) return 0;

    size_t lhs_start = cc__find_lhs_start_backward(s, op_at);
    size_t lhs_a = lhs_start;
    size_t lhs_b = op_at;
    cc__trim_range(s, &lhs_a, &lhs_b);

    int line_no = 1;
    cc__line_from_pos(s, op_at, &line_no);

    /* --- Optional binder: `?>(e) ...` or `?> (e) ...` ------------------ */
    size_t scan = op_at + 2;
    scan = cc_skip_ws_len(s, n, scan);

    int has_binder = 0;
    char binder[128];
    binder[0] = 0;
    if (scan < n && s[scan] == '(') {
        size_t rpar = 0;
        if (!cc_find_matching_paren(s, n, scan, &rpar)) {
            char rel[1024];
            const char* f = cc_path_rel_to_repo(
                ctx && ctx->input_path ? ctx->input_path : "<input>", rel, sizeof(rel));
            cc_pass_error_cat(f, line_no, 1, CC_ERR_SYNTAX,
                              "unclosed '(' in '?>' binder");
            return -1;
        }
        size_t ba = scan + 1;
        size_t bb = rpar;
        cc__trim_range(s, &ba, &bb);
        if (cc__range_is_ident(s, ba, bb)) {
            size_t blen = bb - ba;
            if (blen >= sizeof(binder)) blen = sizeof(binder) - 1;
            memcpy(binder, s + ba, blen);
            binder[blen] = 0;
            has_binder = 1;
            scan = rpar + 1;
        } else {
            /* Disambiguate binder-intended from parenthesized-RHS-expression.
             * A parenthesized expression like `?> (7 + 8)` is a complete RHS;
             * the token after `)` is the usual RHS terminator (`;`, `,`, `:`,
             * `?`, `)`, `]`, `}`, `&&`, `||`) or a C operator that continues
             * the arithmetic expression (`+`, `-`, `*`, `/`, `%`, etc.).
             *
             * A binder-intended form is followed by something that starts a
             * fresh expression or divergent statement: an identifier start, a
             * digit, `(`, `{`, or a string/char literal.  Empty parens `()`
             * are never a valid C expression so they are always binder-intended.
             */
            size_t post = cc_skip_ws_len(s, n, rpar + 1);
            int binder_intended = 0;
            if (bb <= ba) {
                binder_intended = 1;
            } else if (post < n) {
                char pc = s[post];
                if (cc_is_ident_start(pc) || (pc >= '0' && pc <= '9') ||
                    pc == '(' || pc == '{' || pc == '"' || pc == '\'') {
                    binder_intended = 1;
                }
            }
            if (binder_intended) {
                char rel[1024];
                const char* f = cc_path_rel_to_repo(
                    ctx && ctx->input_path ? ctx->input_path : "<input>", rel, sizeof(rel));
                cc_pass_error_cat(f, line_no, 1, CC_ERR_SYNTAX,
                                  "expected identifier in '?>(...)'");
                return -1;
            }
            /* Otherwise leave `scan` at the `(` and treat it as a parenthesized
             * RHS expression.  This preserves `?> (7 + 8)` / `?> (a,b,c)` style
             * defaults where the contents are not a single identifier. */
        }
    }

    /* --- RHS shape check: `?>` is value-only ---------------------------- */
    size_t rhs_scan = cc_skip_ws_len(s, n, scan);
    if (rhs_scan < n) {
        int misuse = 0;
        if (cc__match_ident_kw(s, n, rhs_scan, "return") ||
            cc__match_ident_kw(s, n, rhs_scan, "break") ||
            cc__match_ident_kw(s, n, rhs_scan, "continue") ||
            cc__match_ident_kw(s, n, rhs_scan, "goto")) {
            misuse = 1;
        } else if (s[rhs_scan] == '{') {
            misuse = 1;
        } else if (rhs_scan + 4 <= n && memcmp(s + rhs_scan, "@err", 4) == 0 &&
                   !(rhs_scan + 11 <= n && memcmp(s + rhs_scan, "@errhandler", 11) == 0) &&
                   (rhs_scan + 4 == n || !cc_is_ident_char(s[rhs_scan + 4]))) {
            misuse = 1;
        }
        if (misuse) {
            char rel[1024];
            const char* f = cc_path_rel_to_repo(
                ctx && ctx->input_path ? ctx->input_path : "<input>", rel, sizeof(rel));
            cc_pass_error_cat(f, line_no, 1, CC_ERR_SYNTAX,
                              "'?>' RHS must be a value expression; use '!>' for error-handling logic");
            return -1;
        }
    }

    size_t rhs_start = scan;
    size_t rhs_end = 0;
    if (!cc__find_rhs_end_forward(s, n, rhs_start, &rhs_end)) return -1;

    size_t rhs_a = rhs_start;
    size_t rhs_b = rhs_end;
    cc__trim_range(s, &rhs_a, &rhs_b);

    if (lhs_b <= lhs_a) {
        char rel[1024];
        const char* f = cc_path_rel_to_repo(
            ctx && ctx->input_path ? ctx->input_path : "<input>", rel, sizeof(rel));
        cc_pass_error_cat(f, line_no, 1, CC_ERR_SYNTAX,
                          "missing expression before '?>'");
        return -1;
    }
    if (rhs_b <= rhs_a) {
        char rel[1024];
        const char* f = cc_path_rel_to_repo(
            ctx && ctx->input_path ? ctx->input_path : "<input>", rel, sizeof(rel));
        cc_pass_error_cat(f, line_no, 1, CC_ERR_SYNTAX,
                          "missing default expression after '?>'");
        return -1;
    }

    static int g_unwrap_id = 0;
    int id = ++g_unwrap_id;
    char tmpv[48];
    snprintf(tmpv, sizeof(tmpv), "__cc_pu_r_%d", id);

    char* out = NULL;
    size_t ol = 0, oc = 0;

    /* Copy prefix up to and including whitespace before LHS. */
    cc__append_n(&out, &ol, &oc, s, lhs_start);
    if (lhs_a > lhs_start) cc__append_n(&out, &ol, &oc, s + lhs_start, lhs_a - lhs_start);

    /* Case 1 / 2: ternary, optionally with scoped binder on the err arm.
     * The unified `__cc_uw_*` macros in cc_result.cch dispatch at compile
     * time via `_Generic` — Result-struct LHSs extract `.ok` / `.u.value`
     * / `.u.error`; raw-pointer LHSs get `== NULL` / identity /
     * synthesized `CC_ERR_NULL`.  So the lowering no longer cares whether
     * the LHS is a pointer-returning call or a Result-typed expression. */
    char rel[1024];
    const char* f = cc_path_rel_to_repo(
        ctx && ctx->input_path ? ctx->input_path : "<input>", rel, sizeof(rel));

    cc__append_str(&out, &ol, &oc, "({ __typeof__(");
    cc__append_n(&out, &ol, &oc, s + lhs_a, lhs_b - lhs_a);
    cc_sb_append_fmt(&out, &ol, &oc, ") %s = (", tmpv);
    cc__append_n(&out, &ol, &oc, s + lhs_a, lhs_b - lhs_a);
    cc_sb_append_fmt(&out, &ol, &oc,
                     "); !__cc_uw_is_err(%s) ? __cc_uw_value(%s) : ",
                     tmpv, tmpv);
    if (has_binder) {
        /* Mangle user binder -> `__cc_pu_bind_<id>_<name>` and rewrite
         * the RHS expression accordingly (bug [F9]).  The mangled name
         * keeps the binder off async_ast's frame-lift list. */
        char mangled_binder[256];
        snprintf(mangled_binder, sizeof(mangled_binder), "__cc_pu_bind_%d_%s", id, binder);
        char* mangled_rhs = NULL;
        size_t mangled_rhs_len = 0;
        const char* emit_rhs = s + rhs_a;
        size_t emit_rhs_len = rhs_b - rhs_a;
        if (cc__pu_mangle_binder_in_body(s + rhs_a, rhs_b - rhs_a,
                                         binder, mangled_binder,
                                         &mangled_rhs, &mangled_rhs_len) == 0 && mangled_rhs) {
            emit_rhs = mangled_rhs;
            emit_rhs_len = mangled_rhs_len;
        }
        cc__append_str(&out, &ol, &oc, "({ ");
        cc__ru_emit_uw_err_binder(&out, &ol, &oc, s, lhs_a, lhs_b, tmpv, mangled_binder, f, line_no);
        cc__append_str(&out, &ol, &oc, "(");
        cc__append_n(&out, &ol, &oc, emit_rhs, emit_rhs_len);
        cc__append_str(&out, &ol, &oc, "); })");
        free(mangled_rhs);
    } else {
        cc__append_str(&out, &ol, &oc, "(");
        cc__append_n(&out, &ol, &oc, s + rhs_a, rhs_b - rhs_a);
        cc__append_str(&out, &ol, &oc, ")");
    }
    cc__append_str(&out, &ol, &oc, "; })");

    /* Copy any whitespace between rhs_b and rhs_end that we stripped, then
     * the rest of the input starting at rhs_end. */
    if (rhs_end > rhs_b) cc__append_n(&out, &ol, &oc, s + rhs_b, rhs_end - rhs_b);
    if (rhs_end < n) cc__append_n(&out, &ol, &oc, s + rhs_end, n - rhs_end);

    *out_buf = out;
    *out_len = ol;
    return 1;
}

/* ------------------------------------------------------------------
 * `!>` — error-handling operator, usable at statement OR expression
 * position.  Position is determined by the character immediately before
 * the LHS call expression: `;`, `{`, `}`, or SOF → statement position;
 * anything else (`=`, `(`, `,`, `?`, `:`, `&&`, `||`, `return`, ...) →
 * expression position.
 *
 * At STATEMENT position the recognized forms are (slice 4/5 legacy):
 *
 *   Form A:  CALL !>;                               (no binder)
 *   Form B:  CALL !> STMT;                          (no binder)
 *   Form C:  CALL !> { BLOCK }                      (no binder)
 *   Form D:  CALL !> (IDENT) STMT;                  (binder)
 *   Form E:  CALL !> (IDENT) { BLOCK }              (binder)
 *
 * At statement position the body may fall through.
 *
 * At EXPRESSION position the recognized forms are:
 *
 *   Form P:  CALL !>;                               (bare delegate)
 *   Form Q:  CALL !> DIVERGENT_STMT;                (no binder)
 *   Form R:  CALL !> { STMT; ...; DIVERGENT_STMT }  (no binder, block)
 *   Form S:  CALL !>(IDENT) DIVERGENT_STMT;         (binder)
 *   Form T:  CALL !>(IDENT) { STMT; ...; DIVERGENT_STMT } (binder, block)
 *
 * The body (or block tail) MUST visibly diverge (same list used by the
 * `@errhandler` divergence check: `return`, `break`, `continue`, `goto`,
 * `@err(IDENT);`, or a call to a hardcoded-noreturn function).  The
 * expression-position lowering is:
 *
 *   ({ __typeof__(CALL) tmp = (CALL);
 *      if (cc_is_err(tmp)) {
 *          [__typeof__(cc_error(tmp)) BINDER = cc_error(tmp);]
 *          <BODY>
 *      }
 *      cc_value(tmp);
 *   })
 *
 * Form P (`CALL !>;` at expression position) synthesizes a fresh binder
 * name, locates the nearest enclosing `@errhandler`, substitutes the
 * handler's parameter name with the synthesized binder in the handler
 * body, and splices that as `<BODY>`.  It is a compile error if no
 * enclosing `@errhandler` is in scope.
 *
 * At STATEMENT position, slice-4 forms (A/B/C) rewrite into the
 * pre-existing legacy `@err` surface that `cc__rewrite_err_syntax`
 * already handles:
 *
 *   CALL !>;            => CALL @err;
 *   CALL !> STMT;       => CALL @err STMT;
 *   CALL !> { BLOCK }   => CALL @err { BLOCK };
 *   CALL !> { BLOCK };  => CALL @err { BLOCK };
 *
 * Slice 5 forms (D/E) take a direct `__typeof__`-based lowering without
 * routing through the legacy `@err(DECL){}` surface:
 *
 *   CALL !> (IDENT) { BODY }   =>
 *       { __typeof__(CALL) __cc_pu_s_N = (CALL);
 *         if (cc_is_err(__cc_pu_s_N)) {
 *             __typeof__(cc_error(__cc_pu_s_N)) IDENT = cc_error(__cc_pu_s_N);
 *             BODY_PROCESSED
 *         } }
 *
 *   CALL !> (IDENT) STMT;      =>  same, with BODY = `STMT` (semicolon
 *                                  re-emitted inside the synthetic block).
 *
 * BODY_PROCESSED is BODY with `@err(IDENT);` forwards rewritten (slice
 * 6) into an inlined copy of the lexically nearest `@errhandler(DECL)
 * { ... }` registration's body, with its parameter name substituted to
 * IDENT so references continue to resolve.  Code textually following
 * `@err(IDENT);` in the same block is diagnosed as unreachable.  An
 * `@err(X);` with X not matching any in-scope binder is rejected.
 *
 * The binder IDENT exists only inside the generated error branch of the
 * `if (cc_is_err(...)) { ... }` block, so it is naturally invisible to
 * the success path and to code following the `!>` statement.
 * ---------------------------------------------------------------- */

/* Scan forward for the first `!>` at a word boundary outside of strings
 * and comments.  Mirrors cc__find_unwrap_token but for the `!>` token. */
static int cc__find_bang_token_from(const char* s, size_t n, size_t start,
                                     size_t* out_pos) {
    int in_str = 0;
    char qch = 0;
    int in_line_comment = 0;
    int in_block_comment = 0;
    for (size_t i = start; i < n; i++) {
        char ch = s[i];
        if (in_line_comment) {
            if (ch == '\n') in_line_comment = 0;
            continue;
        }
        if (in_block_comment) {
            if (ch == '*' && i + 1 < n && s[i + 1] == '/') {
                in_block_comment = 0;
                i++;
            }
            continue;
        }
        if (in_str) {
            if (ch == '\\' && i + 1 < n) { i++; continue; }
            if (ch == qch) in_str = 0;
            continue;
        }
        if (ch == '/' && i + 1 < n && s[i + 1] == '/') {
            in_line_comment = 1; i++; continue;
        }
        if (ch == '/' && i + 1 < n && s[i + 1] == '*') {
            in_block_comment = 1; i++; continue;
        }
        if (ch == '"' || ch == '\'') { in_str = 1; qch = ch; continue; }
        if (ch == '!' && i + 1 < n && s[i + 1] == '>') {
            /* Disambiguate against `!=`: `!=` has `=` after `!`, so the
             * `s[i+1] == '>'` check already excludes it.  Any leading
             * punctuation is fine — `!>` is a two-char operator at word
             * boundary regardless of what precedes it. */
            if (out_pos) *out_pos = i;
            return 1;
        }
    }
    return 0;
}

/* Classify whether the text `s[lhs_a..lhs_b)` is the type-specifier prefix
 * of a declaration (e.g. `static bool`, `RedisRequest*`, `int64_t`) rather
 * than a callable expression (`foo()`, `obj->method()`).  The heuristic:
 * a callable expression must contain a balanced `(...)` pair; a type
 * specifier never does.  String/comment context is irrelevant here: the
 * LHS comes from a stmt-position back-scan that already stopped at the
 * previous `;`, `{`, or `}`, so any `(` or `)` we see belongs to the LHS
 * itself. */
static int cc__bang_lhs_looks_like_decl(const char* s, size_t lhs_a,
                                        size_t lhs_b) {
    if (lhs_b <= lhs_a) return 0;
    for (size_t i = lhs_a; i < lhs_b; i++) {
        if (s[i] == '(' || s[i] == ')') return 0;
    }
    return 1;
}

/* Word-boundary substitution: replace every occurrence of `from` (as a
 * whole identifier token) in [body, body+bl) with `to`.  String/comment
 * aware is not required for the bodies we feed in (short preprocessed
 * slices), but we keep the replacement itself identifier-boundary so
 * `err.kind` is substituted cleanly while `error_kind` is not. */
static char* cc__pu_subst_word(const char* body, size_t bl,
                               const char* from, const char* to,
                               size_t* out_len) {
    if (!from || !from[0] || !to) {
        char* r = (char*)malloc(bl + 1);
        if (!r) return NULL;
        memcpy(r, body, bl);
        r[bl] = 0;
        if (out_len) *out_len = bl;
        return r;
    }
    size_t fn = strlen(from), tn = strlen(to);
    char* r = NULL;
    size_t rl = 0, rc = 0;
    size_t i = 0;
    while (i < bl) {
        if (i + fn <= bl && memcmp(body + i, from, fn) == 0 &&
            (i == 0 || !cc_is_ident_char(body[i - 1])) &&
            (i + fn >= bl || !cc_is_ident_char(body[i + fn]))) {
            cc__append_n(&r, &rl, &rc, to, tn);
            i += fn;
        } else {
            cc__append_n(&r, &rl, &rc, body + i, 1);
            i++;
        }
    }
    /* Null terminate for safety; report length separately. */
    cc__append_n(&r, &rl, &rc, "", 1);
    rl--;
    if (out_len) *out_len = rl;
    return r;
}

/* Extract the last identifier from a textual C parameter-declaration
 * like `CCError e` / `struct foo *err` / `__typeof__(...) e`.  Writes a
 * NUL-terminated name into `name`; empty name on failure. */
static void cc__pu_extract_param_name(const char* decl, size_t dl,
                                      char* name, size_t nc) {
    if (nc == 0) return;
    name[0] = 0;
    while (dl > 0 && isspace((unsigned char)decl[dl - 1])) dl--;
    size_t end = dl;
    while (end > 0 && cc_is_ident_char(decl[end - 1]))
        end--;
    size_t id_a = end, id_b = dl;
    if (id_b > id_a && (id_b - id_a) < nc) {
        memcpy(name, decl + id_a, id_b - id_a);
        name[id_b - id_a] = 0;
    }
}

/* Find the lexically-nearest `@errhandler(DECL) { BODY }` registration
 * strictly preceding `pos` in `s`.  On success writes the trimmed
 * declaration text (into out_decl / *out_decl_len) and the body text
 * (into *out_body / *out_body_len).  The returned body pointer aliases
 * into the input buffer; the caller must not free it.  Also writes the
 * source byte offset of the `@` of the matching `@errhandler` into
 * *out_decl_pos (so callers can recover its line number for diagnostics).
 * Returns 1 on success, 0 if no registration is found.
 *
 * The scan is string/comment aware and uses a simple "latest-wins"
 * policy which is adequate for the tests in this slice: the handler
 * registered closest to the `!>` position wins regardless of intervening
 * scope changes.  A richer scope model can be layered on later if a
 * real-world program exposes a gap. */
static int cc__pu_find_outer_errhandler(const char* s, size_t n, size_t pos,
                                        char* out_decl, size_t out_decl_sz,
                                        size_t* out_decl_len,
                                        const char** out_body,
                                        size_t* out_body_len,
                                        size_t* out_decl_pos) {
    int in_str = 0;
    char qch = 0;
    int in_lc = 0, in_bc = 0;
    int found = 0;
    size_t end = (pos <= n) ? pos : n;
    for (size_t i = 0; i < end; i++) {
        char ch = s[i];
        char ch2 = (i + 1 < n) ? s[i + 1] : 0;
        if (in_lc) { if (ch == '\n') in_lc = 0; continue; }
        if (in_bc) { if (ch == '*' && ch2 == '/') { in_bc = 0; i++; } continue; }
        if (in_str) {
            if (ch == '\\' && i + 1 < n) { i++; continue; }
            if (ch == qch) in_str = 0;
            continue;
        }
        if (ch == '/' && ch2 == '/') { in_lc = 1; i++; continue; }
        if (ch == '/' && ch2 == '*') { in_bc = 1; i++; continue; }
        if (ch == '"' || ch == '\'') { in_str = 1; qch = ch; continue; }

        if (ch != '@') continue;
        if (i + 11 > n) continue;
        if (memcmp(s + i, "@errhandler", 11) != 0) continue;
        if (i > 0 && cc_is_ident_char(s[i - 1])) continue;
        if (i + 11 < n && cc_is_ident_char(s[i + 11])) continue;
        size_t j = i + 11;
        while (j < n && isspace((unsigned char)s[j])) j++;
        if (j >= n || s[j] != '(') continue;
        size_t rpar = 0;
        if (!cc_find_matching_paren(s, n, j, &rpar)) continue;
        size_t decl_a = j + 1, decl_b = rpar;
        while (decl_a < decl_b && isspace((unsigned char)s[decl_a])) decl_a++;
        while (decl_b > decl_a && isspace((unsigned char)s[decl_b - 1])) decl_b--;
        size_t k = rpar + 1;
        while (k < n && isspace((unsigned char)s[k])) k++;
        if (k >= n || s[k] != '{') continue;
        size_t rbrace = 0;
        if (!cc_find_matching_brace(s, n, k, &rbrace)) continue;
        /* A valid registration that sits before `pos`.  Record as the
         * running best and continue scanning so a later (closer) one
         * overwrites. */
        size_t dl = decl_b - decl_a;
        if (dl >= out_decl_sz) dl = out_decl_sz - 1;
        memcpy(out_decl, s + decl_a, dl);
        out_decl[dl] = 0;
        if (out_decl_len) *out_decl_len = dl;
        *out_body = s + k + 1;
        *out_body_len = rbrace - (k + 1);
        if (out_decl_pos) *out_decl_pos = i;
        found = 1;
        /* Jump past the body to avoid re-scanning into it. */
        i = rbrace;
    }
    return found;
}

/* ---- Handler divergence check (Feature C) ------------------------
 *
 * When a `@err(BINDER);` forward reaches its registered `@errhandler`,
 * the handler's body becomes the final action of the enclosing frame:
 * control never returns to the forwarding statement.  The handler body
 * must therefore end in a statement that visibly diverges.  Recognized
 * divergent trailing statements:
 *   - `return [EXPR];`
 *   - `break;` / `continue;` / `goto IDENT;`
 *   - `@err(IDENT);` (nested forward)
 *   - A call to one of a small noreturn allowlist:
 *     exit, _Exit, _exit, abort, longjmp, siglongjmp, pthread_exit,
 *     __builtin_unreachable, __builtin_trap
 *   - A bare compound block `{ ... }` whose last statement satisfies
 *     the rule recursively.
 *
 * Block-structured statements like `if (cond) { return X; }` are NOT
 * treated as divergent even if every inner branch returns — the rule
 * is intentionally syntactic and conservative; users who need a real
 * guard can wrap the tail in a `return`/`exit`/`@err(...)`/`goto`. */

static const char* const CC_PU_NORETURN_FNS[] = {
    "exit", "_Exit", "_exit", "abort",
    "longjmp", "siglongjmp", "pthread_exit",
    "__builtin_unreachable", "__builtin_trap",
};

static int cc__pu_body_diverges(const char* body, size_t blen);

/* Inspect the text in body[a..b) — expected to be a single top-level
 * statement — and return 1 iff it visibly diverges. */
static int cc__pu_stmt_diverges(const char* t, size_t a, size_t b) {
    while (b > a && isspace((unsigned char)t[b - 1])) b--;
    while (a < b && isspace((unsigned char)t[a])) a++;
    if (a >= b) return 0;

    size_t p = a;
    /* Bare compound block `{ ... }` — recurse into its body. */
    if (t[p] == '{' && t[b - 1] == '}') {
        return cc__pu_body_diverges(t + p + 1, (b - 1) - (p + 1));
    }
    /* Otherwise must be `;`-terminated. */
    if (t[b - 1] != ';') return 0;
    size_t end = b - 1;
    while (end > p && isspace((unsigned char)t[end - 1])) end--;
    if (end <= p) return 0;

    static const char* const kws[] = { "return", "break", "continue", "goto" };
    for (size_t k = 0; k < sizeof(kws) / sizeof(kws[0]); k++) {
        size_t kl = strlen(kws[k]);
        if (p + kl <= end && memcmp(t + p, kws[k], kl) == 0 &&
            (p + kl == end || !cc_is_ident_char(t[p + kl]))) {
            return 1;
        }
    }

    /* `@err(IDENT);` nested forward (but not `@errhandler`). */
    if (p + 4 <= end && memcmp(t + p, "@err", 4) == 0 &&
        !(p + 11 <= end && memcmp(t + p, "@errhandler", 11) == 0) &&
        (p + 4 == end || !cc_is_ident_char(t[p + 4]))) {
        size_t q = p + 4;
        while (q < end && isspace((unsigned char)t[q])) q++;
        if (q < end && t[q] == '(') return 1;
    }

    /* Call to a known noreturn function: IDENT (...). */
    for (size_t k = 0; k < sizeof(CC_PU_NORETURN_FNS) / sizeof(CC_PU_NORETURN_FNS[0]); k++) {
        const char* fn = CC_PU_NORETURN_FNS[k];
        size_t kl = strlen(fn);
        if (p + kl <= end && memcmp(t + p, fn, kl) == 0 &&
            (p + kl == end || !cc_is_ident_char(t[p + kl]))) {
            size_t q = p + kl;
            while (q < end && isspace((unsigned char)t[q])) q++;
            if (q < end && t[q] == '(') return 1;
        }
    }

    return 0;
}

/* Find the next top-level statement in body[*io_i..blen).  A statement
 * ends at the first `;` at depth 0 or at the `}` that closes the first
 * `{` we opened at depth 0 (to handle block-structured statements
 * uniformly with bare compound blocks). */
static void cc__pu_next_stmt(const char* body, size_t blen,
                             size_t* io_i, size_t* out_a, size_t* out_b) {
    size_t i = *io_i;
    /* Skip leading whitespace/comments. */
    while (i < blen) {
        char c = body[i];
        char c2 = (i + 1 < blen) ? body[i + 1] : 0;
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') { i++; continue; }
        if (c == '/' && c2 == '/') {
            i += 2;
            while (i < blen && body[i] != '\n') i++;
            continue;
        }
        if (c == '/' && c2 == '*') {
            i += 2;
            while (i + 1 < blen && !(body[i] == '*' && body[i + 1] == '/')) i++;
            if (i + 1 < blen) i += 2;
            continue;
        }
        break;
    }
    if (i >= blen) {
        *io_i = i;
        *out_a = i;
        *out_b = i;
        return;
    }

    *out_a = i;
    int par = 0, brk = 0, br = 0;
    int in_str = 0;
    char qch = 0;
    int in_lc = 0, in_bc = 0;
    for (; i < blen; i++) {
        char ch = body[i];
        char ch2 = (i + 1 < blen) ? body[i + 1] : 0;
        if (in_lc) { if (ch == '\n') in_lc = 0; continue; }
        if (in_bc) { if (ch == '*' && ch2 == '/') { in_bc = 0; i++; } continue; }
        if (in_str) {
            if (ch == '\\' && i + 1 < blen) { i++; continue; }
            if (ch == qch) in_str = 0;
            continue;
        }
        if (ch == '/' && ch2 == '/') { in_lc = 1; i++; continue; }
        if (ch == '/' && ch2 == '*') { in_bc = 1; i++; continue; }
        if (ch == '"' || ch == '\'') { in_str = 1; qch = ch; continue; }
        if (ch == '(') { par++; continue; }
        if (ch == '[') { brk++; continue; }
        if (ch == '{') { br++; continue; }
        if (ch == ')') { if (par) par--; continue; }
        if (ch == ']') { if (brk) brk--; continue; }
        if (ch == '}') {
            if (br) {
                br--;
                if (br == 0 && par == 0 && brk == 0) {
                    *out_b = i + 1;
                    *io_i = i + 1;
                    return;
                }
            }
            continue;
        }
        if (par == 0 && brk == 0 && br == 0 && ch == ';') {
            *out_b = i + 1;
            *io_i = i + 1;
            return;
        }
    }
    *out_b = blen;
    *io_i = blen;
}

/* Top-level: does the handler body diverge at its last statement? */
static int cc__pu_body_diverges(const char* body, size_t blen) {
    size_t i = 0;
    size_t last_a = 0, last_b = 0;
    int have = 0;
    while (i < blen) {
        size_t a = 0, b = 0;
        size_t prev = i;
        cc__pu_next_stmt(body, blen, &i, &a, &b);
        if (i == prev) break;
        if (b > a) {
            last_a = a;
            last_b = b;
            have = 1;
        }
    }
    if (!have) return 0;
    return cc__pu_stmt_diverges(body, last_a, last_b);
}

/* Scan forward from `from` (a position inside a block) to the next
 * unmatched `}` at depth 0, treating string/comment content as inert.
 * Returns the position of the `}`; if we fall off the end, returns n. */
static size_t cc__pu_find_enclosing_brace_close(const char* s, size_t n, size_t from) {
    int par = 0, brk = 0, br = 0;
    int in_str = 0;
    char qch = 0;
    int in_lc = 0, in_bc = 0;
    for (size_t i = from; i < n; i++) {
        char ch = s[i];
        char ch2 = (i + 1 < n) ? s[i + 1] : 0;
        if (in_lc) { if (ch == '\n') in_lc = 0; continue; }
        if (in_bc) { if (ch == '*' && ch2 == '/') { in_bc = 0; i++; } continue; }
        if (in_str) {
            if (ch == '\\' && i + 1 < n) { i++; continue; }
            if (ch == qch) in_str = 0;
            continue;
        }
        if (ch == '/' && ch2 == '/') { in_lc = 1; i++; continue; }
        if (ch == '/' && ch2 == '*') { in_bc = 1; i++; continue; }
        if (ch == '"' || ch == '\'') { in_str = 1; qch = ch; continue; }
        if (ch == '(') { par++; continue; }
        if (ch == '[') { brk++; continue; }
        if (ch == '{') { br++; continue; }
        if (ch == ')') { if (par) par--; continue; }
        if (ch == ']') { if (brk) brk--; continue; }
        if (ch == '}') {
            if (par || brk || br) { if (br) br--; continue; }
            return i;
        }
    }
    return n;
}

/* Walk from `from` up to `limit` looking for any non-empty, non-label,
 * non-comment, non-whitespace content.  Returns the position of the
 * first byte of such content, or `limit` if none was found.  A "label"
 * here is `IDENT:` at statement position; we skip it (labels themselves
 * are not executable statements, and the dead-code rule is about
 * executable statements). */
static size_t cc__pu_find_next_stmt_byte(const char* s, size_t from, size_t limit) {
    size_t i = from;
    while (i < limit) {
        char ch = s[i];
        char ch2 = (i + 1 < limit) ? s[i + 1] : 0;
        if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n') { i++; continue; }
        if (ch == '/' && ch2 == '/') {
            i += 2;
            while (i < limit && s[i] != '\n') i++;
            continue;
        }
        if (ch == '/' && ch2 == '*') {
            i += 2;
            while (i + 1 < limit && !(s[i] == '*' && s[i + 1] == '/')) i++;
            if (i + 1 < limit) i += 2;
            continue;
        }
        /* Possibly a label: IDENT : */
        if (cc_is_ident_start(ch)) {
            size_t k = i + 1;
            while (k < limit && cc_is_ident_char(s[k])) k++;
            size_t m = k;
            while (m < limit &&
                   (s[m] == ' ' || s[m] == '\t' || s[m] == '\r' || s[m] == '\n'))
                m++;
            if (m < limit && s[m] == ':' &&
                !(m + 1 < limit && s[m + 1] == ':')) {
                i = m + 1;
                continue;
            }
        }
        return i;
    }
    return limit;
}

/* Process the body text of a `!> (BINDER) BODY` form.  Rewrite every
 * `@err(IDENT);` forward inside the body:
 *   - If IDENT == BINDER: inline outer_body with outer_param → BINDER
 *     substitution (wrapping in a block for safety).  Enforce dead-code
 *     rule for the same-block tail.
 *   - Otherwise: diagnose as unbound.
 * Returns 1 on success with *out_buf / *out_len filled (caller frees).
 * Returns -1 on diagnostic. */
static int cc__pu_process_bang_body(const CCVisitorCtx* ctx,
                                    const char* src, size_t src_n,
                                    size_t op_at, int op_line,
                                    const char* body, size_t body_len,
                                    const char* binder,
                                    const char* outer_body, size_t outer_body_len,
                                    const char* outer_param,
                                    int outer_found, size_t outer_decl_pos,
                                    char** out_buf, size_t* out_len) {
    (void)op_at;
    char rel[1024];
    const char* relf = cc_path_rel_to_repo(
        ctx && ctx->input_path ? ctx->input_path : "<input>", rel, sizeof(rel));

    /* The body pointer aliases into `src`; its absolute offset lets us
     * recover source line numbers for diagnostics pointing at bytes
     * inside the body (e.g. the first byte of unreachable code). */
    size_t body_src_off = (size_t)(body - src);
    (void)src_n;

    char* out = NULL;
    size_t ol = 0, oc = 0;

    int in_str = 0;
    char qch = 0;
    int in_lc = 0, in_bc = 0;
    size_t i = 0;
    while (i < body_len) {
        char ch = body[i];
        char ch2 = (i + 1 < body_len) ? body[i + 1] : 0;
        if (in_lc) {
            cc__append_n(&out, &ol, &oc, &ch, 1);
            if (ch == '\n') in_lc = 0;
            i++;
            continue;
        }
        if (in_bc) {
            cc__append_n(&out, &ol, &oc, &ch, 1);
            if (ch == '*' && ch2 == '/') {
                cc__append_n(&out, &ol, &oc, &ch2, 1);
                in_bc = 0;
                i += 2;
                continue;
            }
            i++;
            continue;
        }
        if (in_str) {
            cc__append_n(&out, &ol, &oc, &ch, 1);
            if (ch == '\\' && i + 1 < body_len) {
                cc__append_n(&out, &ol, &oc, &ch2, 1);
                i += 2;
                continue;
            }
            if (ch == qch) in_str = 0;
            i++;
            continue;
        }
        if (ch == '/' && ch2 == '/') {
            cc__append_n(&out, &ol, &oc, body + i, 2);
            in_lc = 1;
            i += 2;
            continue;
        }
        if (ch == '/' && ch2 == '*') {
            cc__append_n(&out, &ol, &oc, body + i, 2);
            in_bc = 1;
            i += 2;
            continue;
        }
        if (ch == '"' || ch == '\'') {
            cc__append_n(&out, &ol, &oc, &ch, 1);
            in_str = 1;
            qch = ch;
            i++;
            continue;
        }

        /* Look for `@err(` at word boundary — not `@errhandler`. */
        if (ch == '@' && i + 4 <= body_len &&
            memcmp(body + i, "@err", 4) == 0 &&
            !(i + 11 <= body_len && memcmp(body + i, "@errhandler", 11) == 0) &&
            (i + 4 >= body_len || !cc_is_ident_char(body[i + 4]))) {
            size_t j = i + 4;
            while (j < body_len && isspace((unsigned char)body[j])) j++;
            if (j < body_len && body[j] == '(') {
                size_t rpar = 0;
                if (!cc_find_matching_paren(body, body_len, j, &rpar)) {
                    cc_pass_error_cat(relf, op_line, 1, CC_ERR_SYNTAX,
                                      "unclosed '(' in @err(...) forward");
                    free(out);
                    return -1;
                }
                /* Must be `@err(IDENT)` followed by `;`. */
                size_t ia = j + 1, ib = rpar;
                while (ia < ib && isspace((unsigned char)body[ia])) ia++;
                while (ib > ia && isspace((unsigned char)body[ib - 1])) ib--;
                size_t semi = rpar + 1;
                while (semi < body_len && isspace((unsigned char)body[semi])) semi++;
                if (!cc__range_is_ident(body, ia, ib) ||
                    semi >= body_len || body[semi] != ';') {
                    /* Not a `@err(IDENT);` call — leave verbatim.  This
                     * lets nested old-surface `@err(CCError x) {...};`
                     * constructs fall through untouched if somebody
                     * mixes them. */
                    cc__append_n(&out, &ol, &oc, &ch, 1);
                    i++;
                    continue;
                }
                char idname[128];
                size_t idlen = ib - ia;
                if (idlen >= sizeof(idname)) idlen = sizeof(idname) - 1;
                memcpy(idname, body + ia, idlen);
                idname[idlen] = 0;

                if (!binder || !binder[0] || strcmp(idname, binder) != 0) {
                    int err_line = op_line;
                    cc__line_from_pos(src, body_src_off + i, &err_line);
                    if (binder && binder[0]) {
                        cc_pass_error_cat(relf, err_line, 1, CC_ERR_SYNTAX,
                                          "@err(%s) forward references unknown binder '%s' (expected '%s')",
                                          idname, idname, binder);
                    } else {
                        cc_pass_error_cat(relf, err_line, 1, CC_ERR_SYNTAX,
                                          "@err(%s) forward references unknown binder '%s'",
                                          idname, idname);
                    }
                    free(out);
                    return -1;
                }

                if (!outer_found || !outer_body || outer_body_len == 0) {
                    int err_line = op_line;
                    cc__line_from_pos(src, body_src_off + i, &err_line);
                    cc_pass_error_cat(relf, err_line, 1, CC_ERR_SYNTAX,
                                      "@err(%s) forward has no enclosing @errhandler in scope",
                                      idname);
                    free(out);
                    return -1;
                }

                /* Feature C: the handler we are about to forward to
                 * must visibly diverge, because `@err(IDENT);` never
                 * returns.  Diagnose at the handler declaration line. */
                if (!cc__pu_body_diverges(outer_body, outer_body_len)) {
                    int decl_line = op_line;
                    cc__line_from_pos(src, outer_decl_pos, &decl_line);
                    cc_pass_error_cat(relf, decl_line, 1, CC_ERR_SYNTAX,
                                      "@errhandler body must visibly diverge (end with return/break/continue/goto, @err(e);, or a call to exit/abort/longjmp/etc.)");
                    free(out);
                    return -1;
                }

                /* Dead-code check: within the current block of BODY,
                 * any non-label, non-comment statement after the `;`
                 * is unreachable.  Find the enclosing `}` at depth 0
                 * of `body` relative to `i`. */
                size_t block_close = cc__pu_find_enclosing_brace_close(body, body_len, semi + 1);
                size_t next = cc__pu_find_next_stmt_byte(body, semi + 1, block_close);
                if (next < block_close) {
                    int dead_line = op_line;
                    cc__line_from_pos(src, body_src_off + next, &dead_line);
                    cc_pass_error_cat(relf, dead_line, 1, CC_ERR_SYNTAX,
                                      "unreachable code after '@err(%s);' (it never returns)",
                                      idname);
                    free(out);
                    return -1;
                }

                /* Inline the outer handler's body, substituting its
                 * parameter name → our binder so user references in
                 * the handler body keep resolving. */
                size_t sub_len = 0;
                char* subst = cc__pu_subst_word(outer_body, outer_body_len,
                                                (outer_param && outer_param[0]) ? outer_param : "",
                                                binder, &sub_len);
                if (!subst) {
                    free(out);
                    return -1;
                }
                cc__append_str(&out, &ol, &oc, "{ ");
                cc__append_n(&out, &ol, &oc, subst, sub_len);
                cc__append_str(&out, &ol, &oc, " }");
                free(subst);

                i = semi + 1;
                continue;
            }
        }

        cc__append_n(&out, &ol, &oc, &ch, 1);
        i++;
    }

    if (!out) {
        /* Empty body: emit a zero-length buffer. */
        out = (char*)malloc(1);
        if (!out) return -1;
        out[0] = 0;
    }
    *out_buf = out;
    *out_len = ol;
    return 1;
}

/* Emit the direct `__typeof__`-based lowering for a `!> (IDENT) BODY`
 * form.  `call_a..call_b` is the trimmed LHS span (the CALL).  `body`
 * is the body text; it may be the contents of `{ ... }` (block form) or
 * a single `STMT;` / `EXPR;`.  `trailing_semi` is 1 iff the outer form
 * already included a terminating `;` that must not be re-emitted (so
 * we do not produce a stray empty statement). */
static int cc__rewrite_bang_binder(const CCVisitorCtx* ctx,
                                   const char* s, size_t n,
                                   size_t call_a, size_t call_b,
                                   const char* binder,
                                   const char* body, size_t body_len,
                                   int body_is_expr,
                                   int op_line,
                                   size_t splice_from, size_t splice_to,
                                   char** out_buf, size_t* out_len) {
    /* Locate the nearest enclosing @errhandler registration so we can
     * inline its body when an `@err(binder);` forward appears. */
    char outer_decl[256];
    size_t outer_decl_len = 0;
    const char* outer_body = NULL;
    size_t outer_body_len = 0;
    size_t outer_decl_pos = 0;
    int outer_found = 0;
    char outer_param[128];
    outer_param[0] = 0;
    if (cc__pu_find_outer_errhandler(s, n, call_a,
                                     outer_decl, sizeof(outer_decl), &outer_decl_len,
                                     &outer_body, &outer_body_len,
                                     &outer_decl_pos)) {
        cc__pu_extract_param_name(outer_decl, outer_decl_len,
                                  outer_param, sizeof(outer_param));
        outer_found = 1;
    }

    char* processed = NULL;
    size_t processed_len = 0;
    if (cc__pu_process_bang_body(ctx, s, n, call_a, op_line,
                                 body, body_len, binder,
                                 outer_body, outer_body_len,
                                 outer_param,
                                 outer_found, outer_decl_pos,
                                 &processed, &processed_len) < 0) {
        return -1;
    }

    static int g_bang_id = 0;
    int id = ++g_bang_id;
    char tmpv[48];
    snprintf(tmpv, sizeof(tmpv), "__cc_pu_s_%d", id);

    /* Mangle user binder `e` -> `__cc_pu_bind_<id>_e` so async_ast's
     * existing `__cc_pu_` skip rule keeps the binder a true local
     * inside `@async` bodies (bug [F9]). */
    char mangled_binder[256];
    snprintf(mangled_binder, sizeof(mangled_binder), "__cc_pu_bind_%d_%s", id, binder);
    char* mangled_body = NULL;
    size_t mangled_body_len = 0;
    if (cc__pu_mangle_binder_in_body(processed, processed_len,
                                     binder, mangled_binder,
                                     &mangled_body, &mangled_body_len) == 0 && mangled_body) {
        free(processed);
        processed = mangled_body;
        processed_len = mangled_body_len;
    }

    char* out = NULL;
    size_t ol = 0, oc = 0;
    cc__append_n(&out, &ol, &oc, s, splice_from);

    char rel[1024];
    const char* f = cc_path_rel_to_repo(
        ctx && ctx->input_path ? ctx->input_path : "<input>", rel, sizeof(rel));
    cc__append_str(&out, &ol, &oc, "{ __typeof__(");
    cc__append_n(&out, &ol, &oc, s + call_a, call_b - call_a);
    cc_sb_append_fmt(&out, &ol, &oc, ") %s = (", tmpv);
    cc__append_n(&out, &ol, &oc, s + call_a, call_b - call_a);
    cc_sb_append_fmt(&out, &ol, &oc, "); if (__cc_uw_is_err(%s)) { ", tmpv);
    cc__ru_emit_uw_err_binder(&out, &ol, &oc, s, call_a, call_b, tmpv, mangled_binder, f, op_line);
    cc__append_n(&out, &ol, &oc, processed, processed_len);
    if (body_is_expr) {
        /* Expression body: terminate with `;` so it reads as a statement. */
        cc__append_str(&out, &ol, &oc, ";");
    }
    cc__append_str(&out, &ol, &oc, " } }");

    free(processed);

    /* Append whatever followed the binder form in the source. */
    if (splice_to < n) cc__append_n(&out, &ol, &oc, s + splice_to, n - splice_to);

    *out_buf = out;
    *out_len = ol;
    return 1;
}

/* Locate the start of the LHS "call" expression for a `!>` at `op_at`.
 *
 * The LHS cannot cross a statement boundary (`;`, `{`, `}`).  It may
 * cross expression-level boundaries (`=`, `(`, `,`, `?`, `:`, `&&`,
 * `||`, `@`), which indicate `!>` is at expression position; crossing a
 * statement boundary (or SOF) means statement position.  Balanced
 * `(...)` / `[...]` are consumed so calls like `foo(bar, baz)` are kept
 * intact.  `out_is_stmt_pos` (if non-NULL) is set to 1 iff the boundary
 * immediately before the LHS indicates statement position. */
static size_t cc__find_bang_lhs_start_ex(const char* s, size_t op_at,
                                         int* out_is_stmt_pos) {
    int par = 0, brk = 0;
    size_t i = op_at;
    while (i > 0) {
        i--;
        char c = s[i];
        /* Skip block-comment bodies: closing delimiter seen backward is
         * `/` preceded by `*`.  Without this the scan happily walks
         * through commented-out code and picks up phantom braces/parens
         * as the statement boundary (examples/hello.ccs repro). */
        if (c == '/' && i > 0 && s[i - 1] == '*') {
            cc__skip_block_comment_backward(s, &i);
            continue;
        }
        if (c != '\n' && cc__pos_in_line_comment(s, i)) {
            while (i > 0 && s[i] != '\n') i--;
            continue;
        }
        if (c == '"' || c == '\'') {
            cc__skip_str_backward(s, &i);
            continue;
        }
        if (c == ')') { par++; continue; }
        if (c == '(') {
            if (par > 0) { par--; continue; }
            /* Unmatched `(` — expression-position boundary (argument start,
             * parenthesized expr, etc.). */
            if (out_is_stmt_pos) *out_is_stmt_pos = 0;
            return i + 1;
        }
        if (c == ']') { brk++; continue; }
        if (c == '[') { if (brk > 0) brk--; continue; }
        if (par > 0 || brk > 0) continue;

        /* Statement-position boundaries. */
        if (c == ';' || c == '{' || c == '}') {
            if (out_is_stmt_pos) *out_is_stmt_pos = 1;
            return i + 1;
        }
        /* Expression-position boundaries. */
        if (c == ',' || c == '?' || c == ':' || c == '@') {
            if (out_is_stmt_pos) *out_is_stmt_pos = 0;
            return i + 1;
        }
        if (c == '=' && cc__eq_is_boundary(s, op_at, i)) {
            if (out_is_stmt_pos) *out_is_stmt_pos = 0;
            return i + 1;
        }
        if (c == '&' && i > 0 && s[i - 1] == '&') {
            if (out_is_stmt_pos) *out_is_stmt_pos = 0;
            return i + 1;
        }
        if (c == '|' && i > 0 && s[i - 1] == '|') {
            if (out_is_stmt_pos) *out_is_stmt_pos = 0;
            return i + 1;
        }
    }
    /* SOF → statement position (top-level). */
    if (out_is_stmt_pos) *out_is_stmt_pos = 1;
    return 0;
}

static size_t cc__find_bang_lhs_start(const char* s, size_t op_at) {
    return cc__find_bang_lhs_start_ex(s, op_at, NULL);
}

/* Expression-position `!>` rewrite.  Emits the `({ ... })`
 * statement-expression lowering after validating that the body diverges.
 * `call_start` is the first byte of the LHS call (already adjusted past
 * any leading `return` keyword by the caller). */
static int cc__rewrite_bang_expr_once(const CCVisitorCtx* ctx,
                                      const char* s, size_t n,
                                      size_t op_at, size_t call_start,
                                      int line_no,
                                      char** out_buf, size_t* out_len) {
    char rel[1024];
    const char* f = cc_path_rel_to_repo(
        ctx && ctx->input_path ? ctx->input_path : "<input>", rel, sizeof(rel));

    size_t call_a = call_start, call_b = op_at;
    cc__trim_range(s, &call_a, &call_b);
    if (call_b <= call_a) {
        cc_pass_error_cat(f, line_no, 1, CC_ERR_SYNTAX,
                          "missing expression before '!>'");
        return -1;
    }

    size_t scan = cc_skip_ws_len(s, n, op_at + 2);
    int has_binder = 0;
    char binder[128];
    binder[0] = 0;

    if (scan < n && s[scan] == '(') {
        size_t rpar = 0;
        if (!cc_find_matching_paren(s, n, scan, &rpar)) {
            cc_pass_error_cat(f, line_no, 1, CC_ERR_SYNTAX,
                              "unclosed '(' in '!>' binder");
            return -1;
        }
        size_t ba = scan + 1, bb = rpar;
        while (ba < bb && isspace((unsigned char)s[ba])) ba++;
        while (bb > ba && isspace((unsigned char)s[bb - 1])) bb--;
        if (!cc__range_is_ident(s, ba, bb)) {
            cc_pass_error_cat(f, line_no, 1, CC_ERR_SYNTAX,
                              "expected identifier in '!> (...)'");
            return -1;
        }
        size_t blen = bb - ba;
        if (blen >= sizeof(binder)) blen = sizeof(binder) - 1;
        memcpy(binder, s + ba, blen);
        binder[blen] = 0;
        has_binder = 1;
        scan = cc_skip_ws_len(s, n, rpar + 1);
    }

    if (scan >= n) {
        cc_pass_error_cat(f, line_no, 1, CC_ERR_SYNTAX,
                          has_binder
                              ? "expected body after '!> (e)'"
                              : "expected body after '!>'");
        return -1;
    }

    /* Locate the nearest enclosing @errhandler (needed for bare form and
     * for any `@err(binder);` forwards inside a user-provided body). */
    char outer_decl[256];
    size_t outer_decl_len = 0;
    const char* outer_body = NULL;
    size_t outer_body_len = 0;
    size_t outer_decl_pos = 0;
    char outer_param[128];
    outer_param[0] = 0;
    int outer_found = cc__pu_find_outer_errhandler(
        s, n, call_a,
        outer_decl, sizeof(outer_decl), &outer_decl_len,
        &outer_body, &outer_body_len, &outer_decl_pos);
    if (outer_found) {
        cc__pu_extract_param_name(outer_decl, outer_decl_len,
                                  outer_param, sizeof(outer_param));
    }

    /* Form P: bare `!>;` at expression position.  Synthesize a binder,
     * inline the outer handler body. */
    if (s[scan] == ';' && !has_binder) {
        if (!outer_found) {
            cc_pass_error_cat(f, line_no, 1, CC_ERR_SYNTAX,
                              "'!>;' at expression position requires an enclosing '@errhandler' in scope");
            return -1;
        }
        if (!cc__pu_body_diverges(outer_body, outer_body_len)) {
            int decl_line = line_no;
            cc__line_from_pos(s, outer_decl_pos, &decl_line);
            cc_pass_error_cat(f, decl_line, 1, CC_ERR_SYNTAX,
                              "@errhandler body must visibly diverge when used as an expression-position '!>;' delegate");
            return -1;
        }
        static int g_bang_expr_bare_id = 0;
        int id = ++g_bang_expr_bare_id;
        char synth[48];
        snprintf(synth, sizeof(synth), "__cc_pu_be_%d", id);
        strncpy(binder, synth, sizeof(binder));
        binder[sizeof(binder) - 1] = 0;
        has_binder = 1;

        size_t sub_len = 0;
        char* substituted = cc__pu_subst_word(
            outer_body, outer_body_len,
            (outer_param[0] ? outer_param : ""),
            binder, &sub_len);
        if (!substituted) return -1;

        static int g_expr_tmp_id = 0;
        int tid = ++g_expr_tmp_id;
        char tmpv[48];
        snprintf(tmpv, sizeof(tmpv), "__cc_pu_e_%d", tid);

        /* Leave the source `;` in place: in `int v = f() !>;` it terminates
         * the enclosing declaration.  The generated expression is a
         * parenthesised `({ ... })` with no trailing `;` so we must not
         * swallow the only `;` in sight. */
        size_t splice_to = scan;

        char rel[1024];
        const char* ff = cc_path_rel_to_repo(
            ctx && ctx->input_path ? ctx->input_path : "<input>", rel, sizeof(rel));
        char* out = NULL;
        size_t ol = 0, oc = 0;
        cc__append_n(&out, &ol, &oc, s, call_start);
        cc__append_str(&out, &ol, &oc, "({ __typeof__(");
        cc__append_n(&out, &ol, &oc, s + call_a, call_b - call_a);
        cc_sb_append_fmt(&out, &ol, &oc, ") %s = (", tmpv);
        cc__append_n(&out, &ol, &oc, s + call_a, call_b - call_a);
        cc_sb_append_fmt(&out, &ol, &oc, "); if (__cc_uw_is_err(%s)) { ", tmpv);
        cc__ru_emit_uw_err_binder(&out, &ol, &oc, s, call_a, call_b, tmpv, binder, ff, line_no);
        cc__append_n(&out, &ol, &oc, substituted, sub_len);
        cc_sb_append_fmt(&out, &ol, &oc, " } __cc_uw_value(%s); })", tmpv);
        free(substituted);
        if (splice_to < n) cc__append_n(&out, &ol, &oc, s + splice_to, n - splice_to);
        *out_buf = out;
        *out_len = ol;
        return 1;
    }

    /* Reject bare `!>(e);` form — a binder requires a body. */
    if (s[scan] == ';' && has_binder) {
        cc_pass_error_cat(f, line_no, 1, CC_ERR_SYNTAX,
                          "expected body after '!> (%s)'", binder);
        return -1;
    }

    /* Parse a single-statement body or block body, then require divergence. */
    size_t body_a = 0, body_b = 0;
    int is_block = 0;
    size_t splice_to = 0;
    if (s[scan] == '{') {
        size_t rbrace = 0;
        if (!cc_find_matching_brace(s, n, scan, &rbrace)) {
            cc_pass_error_cat(f, line_no, 1, CC_ERR_SYNTAX,
                              "unclosed '{' in '!>' body");
            return -1;
        }
        body_a = scan + 1;
        body_b = rbrace;
        splice_to = rbrace + 1;
        is_block = 1;
        if (!cc__pu_body_diverges(s + body_a, body_b - body_a)) {
            cc_pass_error_cat(f, line_no, 1, CC_ERR_SYNTAX,
                              "expression-position '!>' body must diverge (return/break/continue/goto/@err/exit/abort/etc.)");
            return -1;
        }
    } else {
        size_t semi = 0;
        if (!cc__find_semi_forward(s, n, scan, &semi)) {
            cc_pass_error_cat(f, line_no, 1, CC_ERR_SYNTAX,
                              "expected ';' terminating '!>' body");
            return -1;
        }
        body_a = scan;
        body_b = semi + 1; /* include `;` so cc__pu_stmt_diverges sees it */
        /* Leave the source `;` in place so it terminates the enclosing
         * statement (e.g. `int x = f() !> return cc_err;`).  The body `;`
         * is re-emitted explicitly inside the if-branch below. */
        splice_to = semi;
        if (!cc__pu_stmt_diverges(s, body_a, body_b)) {
            cc_pass_error_cat(f, line_no, 1, CC_ERR_SYNTAX,
                              "expression-position '!>' body must diverge (return/break/continue/goto/@err/exit/abort/etc.)");
            return -1;
        }
    }

    /* Process `@err(IDENT);` forwards inside the body. */
    char* processed = NULL;
    size_t processed_len = 0;
    if (cc__pu_process_bang_body(ctx, s, n, call_a, line_no,
                                 s + body_a, body_b - body_a,
                                 binder,
                                 outer_body, outer_body_len,
                                 outer_param,
                                 outer_found, outer_decl_pos,
                                 &processed, &processed_len) < 0) {
        return -1;
    }

    static int g_expr_tmp_id2 = 0;
    int tid = ++g_expr_tmp_id2;
    char tmpv[48];
    snprintf(tmpv, sizeof(tmpv), "__cc_pu_x_%d", tid);

    /* Mangle user binder to `__cc_pu_bind_<id>_<name>` (bug [F9]). */
    char mangled_binder[256];
    if (has_binder) {
        snprintf(mangled_binder, sizeof(mangled_binder), "__cc_pu_bind_%d_%s", tid, binder);
        char* mangled_body = NULL;
        size_t mangled_body_len = 0;
        if (cc__pu_mangle_binder_in_body(processed, processed_len,
                                         binder, mangled_binder,
                                         &mangled_body, &mangled_body_len) == 0 && mangled_body) {
            free(processed);
            processed = mangled_body;
            processed_len = mangled_body_len;
        }
    }

    char* out = NULL;
    size_t ol = 0, oc = 0;
    cc__append_n(&out, &ol, &oc, s, call_start);
    cc__append_str(&out, &ol, &oc, "({ __typeof__(");
    cc__append_n(&out, &ol, &oc, s + call_a, call_b - call_a);
    cc_sb_append_fmt(&out, &ol, &oc, ") %s = (", tmpv);
    cc__append_n(&out, &ol, &oc, s + call_a, call_b - call_a);
    cc_sb_append_fmt(&out, &ol, &oc, "); if (__cc_uw_is_err(%s)) { ", tmpv);
    if (has_binder) {
        cc__ru_emit_uw_err_binder(&out, &ol, &oc, s, call_a, call_b, tmpv, mangled_binder, f, line_no);
    }
    cc__append_n(&out, &ol, &oc, processed, processed_len);
    if (!is_block) {
        /* Single-statement form: include trailing `;` if the stmt was
         * emitted without one (body_b already included the `;`, so
         * processed already ends with `;` — skip). */
        (void)0;
    }
    cc_sb_append_fmt(&out, &ol, &oc, " } __cc_uw_value(%s); })", tmpv);
    free(processed);
    if (splice_to < n) cc__append_n(&out, &ol, &oc, s + splice_to, n - splice_to);
    *out_buf = out;
    *out_len = ol;
    return 1;
}

/* Single pass: find the first `!>` and rewrite it in place into either
 * the slice-5 binder lowering (if a `(` follows) or the legacy `@err`
 * equivalent (slice-4 bare / block / single-statement forms).
 * Returns 1 on substitution, 0 if no `!>` remains, -1 on error. */
static int cc__rewrite_bang_once(const CCVisitorCtx* ctx,
                                 const char* s,
                                 size_t n,
                                 char** out_buf,
                                 size_t* out_len) {
    size_t op_at = 0;
    size_t search_from = 0;
    int is_stmt_pos = 1;
    size_t call_start = 0;
    int line_no = 1;
    size_t after = 0;
    size_t scan = 0;

    /* Skip past declaration-form sigils (`T !>(E) name(...)` and
     * `T !>(E) var = ...`): the type-syntax pass rewrites those into
     * `CCResult_T_E ...` later.  If we treat them as expression-position
     * binders we either mis-rewrite them or emit a spurious
     * "expected ';' terminating '!> (E) body'" diagnostic when the
     * forward `;` search walks past EOF. */
    for (;;) {
        if (!cc__find_bang_token_from(s, n, search_from, &op_at)) return 0;

        line_no = 1;
        cc__line_from_pos(s, op_at, &line_no);

        after = op_at + 2;
        scan = cc_skip_ws_len(s, n, after);
        if (scan >= n) {
            char rel[1024];
            const char* f = cc_path_rel_to_repo(
                ctx && ctx->input_path ? ctx->input_path : "<input>", rel, sizeof(rel));
            cc_pass_error_cat(f, line_no, 1, CC_ERR_SYNTAX,
                              "unexpected end of input after '!>'");
            return -1;
        }

        is_stmt_pos = 1;
        call_start = cc__find_bang_lhs_start_ex(s, op_at, &is_stmt_pos);
        /* Special-case: if LHS textually begins with `return`, we are
         * actually at expression position (the `!>` operand is `return`'s
         * expression).  Use the comment-aware forward skip so any block
         * or line comments between the preceding statement boundary and
         * `return` don't hide the keyword and drag it into the operand. */
        if (is_stmt_pos) {
            size_t a = cc__skip_ws_comments_forward(s, op_at, call_start);
            if (a + 6 <= op_at && memcmp(s + a, "return", 6) == 0 &&
                (a + 6 == op_at || !cc_is_ident_char(s[a + 6]))) {
                is_stmt_pos = 0;
                call_start = a + 6;
            }
        }

        /* Declaration-form detection: only stmt-position `!>` tokens whose
         * LHS text contains no parens can be a type annotation like
         * `bool !>(CCError) f(...)`.  Expression-position and parenful
         * LHS (`foo() !> ...`) always go through the usual dispatch. */
        if (is_stmt_pos && s[scan] == '(') {
            size_t lhs_a = call_start, lhs_b = op_at;
            cc__trim_range(s, &lhs_a, &lhs_b);
            if (cc__bang_lhs_looks_like_decl(s, lhs_a, lhs_b)) {
                search_from = op_at + 2;
                continue;
            }
        }
        break;
    }

    if (!is_stmt_pos) {
        return cc__rewrite_bang_expr_once(ctx, s, n, op_at, call_start,
                                          line_no, out_buf, out_len);
    }

    /* --- Slice 5: optional `(IDENT)` binder --------------------------- */
    if (s[scan] == '(') {
        size_t rpar = 0;
        if (!cc_find_matching_paren(s, n, scan, &rpar)) {
            char rel[1024];
            const char* f = cc_path_rel_to_repo(
                ctx && ctx->input_path ? ctx->input_path : "<input>", rel, sizeof(rel));
            cc_pass_error_cat(f, line_no, 1, CC_ERR_SYNTAX,
                              "unclosed '(' in '!>' binder");
            return -1;
        }
        size_t ba = scan + 1, bb = rpar;
        while (ba < bb && isspace((unsigned char)s[ba])) ba++;
        while (bb > ba && isspace((unsigned char)s[bb - 1])) bb--;
        if (!cc__range_is_ident(s, ba, bb)) {
            char rel[1024];
            const char* f = cc_path_rel_to_repo(
                ctx && ctx->input_path ? ctx->input_path : "<input>", rel, sizeof(rel));
            cc_pass_error_cat(f, line_no, 1, CC_ERR_SYNTAX,
                              "expected identifier in '!> (...)'");
            return -1;
        }
        char binder[128];
        size_t blen = bb - ba;
        if (blen >= sizeof(binder)) blen = sizeof(binder) - 1;
        memcpy(binder, s + ba, blen);
        binder[blen] = 0;

        size_t call_start = cc__find_bang_lhs_start(s, op_at);
        size_t call_a = call_start, call_b = op_at;
        cc__trim_range(s, &call_a, &call_b);
        if (call_b <= call_a) {
            char rel[1024];
            const char* f = cc_path_rel_to_repo(
                ctx && ctx->input_path ? ctx->input_path : "<input>", rel, sizeof(rel));
            cc_pass_error_cat(f, line_no, 1, CC_ERR_SYNTAX,
                              "missing expression before '!>'");
            return -1;
        }

        size_t after_bind = cc_skip_ws_len(s, n, rpar + 1);
        if (after_bind >= n || s[after_bind] == ';') {
            char rel[1024];
            const char* f = cc_path_rel_to_repo(
                ctx && ctx->input_path ? ctx->input_path : "<input>", rel, sizeof(rel));
            cc_pass_error_cat(f, line_no, 1, CC_ERR_SYNTAX,
                              "expected body after '!> (e)'");
            return -1;
        }

        if (s[after_bind] == '{') {
            size_t rbrace = 0;
            if (!cc_find_matching_brace(s, n, after_bind, &rbrace)) {
                char rel[1024];
                const char* f = cc_path_rel_to_repo(
                    ctx && ctx->input_path ? ctx->input_path : "<input>", rel, sizeof(rel));
                cc_pass_error_cat(f, line_no, 1, CC_ERR_SYNTAX,
                                  "unclosed '{' in '!> (%s)' body", binder);
                return -1;
            }
            size_t body_a = after_bind + 1, body_b = rbrace;
            /* Eat a following `;` so we do not leave a stray empty stmt. */
            size_t splice_to = rbrace + 1;
            size_t tail = cc_skip_ws_len(s, n, splice_to);
            if (tail < n && s[tail] == ';') splice_to = tail + 1;
            return cc__rewrite_bang_binder(ctx, s, n, call_a, call_b, binder,
                                           s + body_a, body_b - body_a,
                                           /*body_is_expr=*/0,
                                           line_no, call_start, splice_to,
                                           out_buf, out_len);
        } else {
            /* Single statement / expression body: scan to terminating `;` at depth 0. */
            size_t semi = 0;
            if (!cc__find_semi_forward(s, n, after_bind, &semi)) {
                char rel[1024];
                const char* f = cc_path_rel_to_repo(
                    ctx && ctx->input_path ? ctx->input_path : "<input>", rel, sizeof(rel));
                cc_pass_error_cat(f, line_no, 1, CC_ERR_SYNTAX,
                                  "expected ';' terminating '!> (%s)' body", binder);
                return -1;
            }
            size_t body_a = after_bind, body_b = semi;
            size_t splice_to = semi + 1;
            return cc__rewrite_bang_binder(ctx, s, n, call_a, call_b, binder,
                                           s + body_a, body_b - body_a,
                                           /*body_is_expr=*/1,
                                           line_no, call_start, splice_to,
                                           out_buf, out_len);
        }
    }

    /* --- Slice 4: no binder — rewrite to legacy `@err` surface. ------- */

    /* Detect form and, for Form C, whether a trailing `;` needs to be
     * synthesized after the closing brace. */
    size_t insert_semi_at = 0; /* 0 sentinel: no insertion */

    if (s[scan] == ';') {
        /* Form A: `CALL !>;` — plain dispatch to the default handler.  The
         * existing `;` following `!>` stays put; we only replace the two
         * sigil bytes with `@err`. */
    } else if (s[scan] == '{') {
        size_t rbrace = 0;
        if (!cc_find_matching_brace(s, n, scan, &rbrace)) {
            char rel[1024];
            const char* f = cc_path_rel_to_repo(
                ctx && ctx->input_path ? ctx->input_path : "<input>", rel, sizeof(rel));
            cc_pass_error_cat(f, line_no, 1, CC_ERR_SYNTAX,
                              "unclosed '{' in '!>' body");
            return -1;
        }
        /* Form C: `CALL !> { BLOCK }` or `CALL !> { BLOCK };`.  The legacy
         * `@err { ... };` shorthand requires a trailing `;`, so synthesize
         * one when the input omitted it. */
        size_t post = cc_skip_ws_len(s, n, rbrace + 1);
        if (post >= n || s[post] != ';') {
            insert_semi_at = rbrace + 1;
        }
    } else {
        /* Form B: `CALL !> STMT;`.  Confirm a terminating `;` exists at
         * depth 0; we don't need its position for the rewrite but want a
         * clean diagnostic if the statement is malformed. */
        size_t semi = 0;
        if (!cc__find_semi_forward(s, n, scan, &semi)) {
            char rel[1024];
            const char* f = cc_path_rel_to_repo(
                ctx && ctx->input_path ? ctx->input_path : "<input>", rel, sizeof(rel));
            cc_pass_error_cat(f, line_no, 1, CC_ERR_SYNTAX,
                              "expected ';' terminating '!>' body");
            return -1;
        }
        (void)semi;
    }

    char* out = NULL;
    size_t ol = 0, oc = 0;
    cc__append_n(&out, &ol, &oc, s, op_at);
    cc__append_str(&out, &ol, &oc, "@err");
    if (insert_semi_at) {
        /* Splice in a `;` immediately after the closing `}` of Form C. */
        size_t mid_from = op_at + 2;
        cc__append_n(&out, &ol, &oc, s + mid_from, insert_semi_at - mid_from);
        cc__append_str(&out, &ol, &oc, ";");
        if (insert_semi_at < n) {
            cc__append_n(&out, &ol, &oc, s + insert_semi_at, n - insert_semi_at);
        }
    } else {
        if (op_at + 2 < n) {
            cc__append_n(&out, &ol, &oc, s + op_at + 2, n - (op_at + 2));
        }
    }

    *out_buf = out;
    *out_len = ol;
    return 1;
}

/* ------------------------------------------------------------------
 * Slice 7: unhandled-result call diagnostic.
 *
 * Gated on env var `CC_STRICT_RESULT_UNWRAP=1`.  Runs as a final pass
 * AFTER `?>` and `!>` have been lowered, so every legitimate consumer
 * has wrapped the underlying call in an assignment / ternary
 * construct.  What remains textually as `NAME(...)` at statement
 * position is by construction unhandled.
 *
 * The acceptor rules (conservative — err on the side of NOT firing):
 *   - The call is consumed by `?>` / `!>` / `@err` in the text (these
 *     have been rewritten already except `@err`, but rewriting leaves
 *     the call as RHS of an `=` which our expression-context check
 *     accepts anyway).
 *   - The call is the RHS of an `=` (any-depth: assignment, initialiser,
 *     compound assignment).
 *   - The call follows a `return` keyword.
 *   - The call is prefixed by `(void)` — the one explicit-discard form.
 *   - The call is part of a larger expression (prev non-ws char is one
 *     of `=`, `,`, `(`, `?`, `:`, `!`, `|`, `&`, `^`, `~`, `+`, `-`,
 *     `*`, `/`, `%`, `<`, `>`, or an identifier like `return`).
 *   - The call is followed by anything other than `;` — that means it
 *     participates in a larger expression (condition, argument, etc.).
 *
 * We fire ONLY when all of:
 *   (a) The NAME is a registered result-returning function.
 *   (b) The character immediately after the balanced `)` (skipping
 *       ws/comments) is `;`.
 *   (c) The non-ws char immediately preceding NAME is one of `;`, `{`,
 *       `}`, or beginning-of-buffer (i.e. statement position).
 *   (d) The call is NOT preceded by `(void)`.
 * ---------------------------------------------------------------- */

static int cc__strict_enabled(void) {
    const char* e = getenv("CC_STRICT_RESULT_UNWRAP");
    return (e && e[0] == '1' && e[1] == 0);
}

/* Walk backwards from `from` (exclusive) skipping whitespace.  Returns
 * the index of the non-ws char (or SIZE_MAX sentinel if we fell off the
 * start). */
static size_t cc__back_skip_ws(const char* s, size_t from) {
    size_t i = from;
    while (i > 0) {
        char c = s[i - 1];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') { i--; continue; }
        return i - 1;
    }
    return (size_t)-1;
}

/* Check if the non-ws chars immediately preceding position `pre` form a
 * `(void)` cast.  Returns 1 if yes. */
static int cc__is_preceded_by_void_cast(const char* s, size_t pre) {
    /* `pre` is the index of the char immediately before NAME (already
     * stepped back one from NAME start).  We expect the sequence
     *   '(' 'v' 'o' 'i' 'd' ')'   with optional whitespace.
     */
    if (pre == (size_t)-1) return 0;
    if (s[pre] != ')') return 0;
    size_t j = cc__back_skip_ws(s, pre);
    /* After ')', skip ws going back through the token list. */
    /* Expect 'd' */
    if (j == (size_t)-1 || s[j] != 'd') return 0;
    if (j == 0 || s[j - 1] != 'i') return 0;
    if (j < 2 || s[j - 2] != 'o') return 0;
    if (j < 3 || s[j - 3] != 'v') return 0;
    /* Ensure no identifier char precedes the 'v'. */
    if (j >= 4 && cc_is_ident_char(s[j - 4])) return 0;
    size_t k = (j >= 4) ? j - 4 : (size_t)-1;
    k = cc__back_skip_ws(s, k + 1);
    if (k == (size_t)-1 || s[k] != '(') return 0;
    return 1;
}

/* Check if position `pre` is immediately preceded by the identifier
 * `return` at a word boundary.  Returns 1 if yes. */
static int cc__is_preceded_by_ident(const char* s, size_t pre, const char* ident) {
    if (pre == (size_t)-1) return 0;
    size_t il = strlen(ident);
    if (pre + 1 < il) return 0;
    size_t start = pre + 1 - il;
    if (memcmp(s + start, ident, il) != 0) return 0;
    /* Word-boundary: char before `start` must not be an ident char. */
    if (start > 0 && cc_is_ident_char(s[start - 1])) return 0;
    return 1;
}

/* Find end of identifier starting at `i`. */
static size_t cc__ident_end(const char* s, size_t n, size_t i) {
    while (i < n && cc_is_ident_char(s[i])) i++;
    return i;
}

/* Skip whitespace and line/block comments forward. */
static size_t cc__skip_ws_comments_forward(const char* s, size_t n, size_t i) {
    while (i < n) {
        char c = s[i];
        char c2 = (i + 1 < n) ? s[i + 1] : 0;
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') { i++; continue; }
        if (c == '/' && c2 == '/') {
            i += 2;
            while (i < n && s[i] != '\n') i++;
            continue;
        }
        if (c == '/' && c2 == '*') {
            i += 2;
            while (i + 1 < n && !(s[i] == '*' && s[i + 1] == '/')) i++;
            if (i + 1 < n) i += 2;
            continue;
        }
        break;
    }
    return i;
}

static int cc__strict_unhandled_scan(const CCVisitorCtx* ctx,
                                     const char* s, size_t n) {
    if (!cc__strict_enabled()) return 0;
    char rel[1024];
    const char* f = cc_path_rel_to_repo(
        ctx && ctx->input_path ? ctx->input_path : "<input>", rel, sizeof(rel));

    int any_err = 0;
    int in_str = 0; char qch = 0;
    int in_lc = 0, in_bc = 0;
    size_t i = 0;
    while (i < n) {
        char c = s[i];
        char c2 = (i + 1 < n) ? s[i + 1] : 0;
        if (in_lc) { if (c == '\n') in_lc = 0; i++; continue; }
        if (in_bc) { if (c == '*' && c2 == '/') { in_bc = 0; i += 2; continue; } i++; continue; }
        if (in_str) {
            if (c == '\\' && i + 1 < n) { i += 2; continue; }
            if (c == qch) in_str = 0;
            i++;
            continue;
        }
        if (c == '/' && c2 == '/') { in_lc = 1; i += 2; continue; }
        if (c == '/' && c2 == '*') { in_bc = 1; i += 2; continue; }
        if (c == '"' || c == '\'') { in_str = 1; qch = c; i++; continue; }

        /* Look for identifier starts. */
        if (!cc_is_ident_start(c)) { i++; continue; }
        /* Word-boundary: only start tokens that are not preceded by an
         * ident char.  Since we're already at an identifier start and the
         * previous iteration advanced exactly one char, the only way the
         * prev char is an ident char is if we're mid-token (which won't
         * happen because ident chars would have been handled above and
         * we advance one at a time).  Belt-and-braces check: */
        if (i > 0 && cc_is_ident_char(s[i - 1])) { i++; continue; }

        size_t id_a = i;
        size_t id_b = cc__ident_end(s, n, i);
        size_t id_len = id_b - id_a;
        i = id_b;

        /* Only proceed if this identifier names a registered
         * result-returning function. */
        if (!cc_result_fn_registry_contains(s + id_a, id_len)) continue;

        /* Must be immediately followed (after ws) by '('. */
        size_t after_id = cc__skip_ws_comments_forward(s, n, id_b);
        if (after_id >= n || s[after_id] != '(') continue;

        size_t rpar = 0;
        if (!cc_find_matching_paren(s, n, after_id, &rpar)) continue;

        /* Gate (b): char after balanced ')' must be ';'. */
        size_t post = cc__skip_ws_comments_forward(s, n, rpar + 1);
        if (post >= n || s[post] != ';') continue;

        /* Gate (c): non-ws char before NAME must be `;`, `{`, `}`, or
         * start-of-buffer.  Use the same walk-back-skipping-ws rule. */
        size_t prev = cc__back_skip_ws(s, id_a);
        int at_stmt_pos = 0;
        if (prev == (size_t)-1) {
            at_stmt_pos = 1;
        } else {
            char pc = s[prev];
            if (pc == ';' || pc == '{' || pc == '}') at_stmt_pos = 1;
            /* Labels: `IDENT :` puts us at statement position too.  Being
             * conservative, also accept `:` as statement-position.  The
             * alternative meaning is a ternary false-arm which IS
             * expression context; to avoid false positives on ternaries
             * we scan further back — if prev was preceded by an unmatched
             * `?` at depth 0, it's a ternary and we should NOT flag.
             * That requires a depth-balanced backwards scan, which we
             * already avoid for simplicity.  So we only fire on `;` /
             * `{` / `}` / SOF, never on `:`.  This means `label: f();`
             * is a false negative, which is acceptable. */
        }
        if (!at_stmt_pos) continue;

        /* Gate (d): `(void)` escape. */
        if (cc__is_preceded_by_void_cast(s, prev)) continue;

        /* Gate (also accepted): `return NAME(...)` — conservative.  If
         * `return` is directly before NAME (with ws) we won't reach here
         * because `return` ends in an ident char and `prev` would then
         * point at `n` not at `;`/`{`/`}`.  So return-consumed calls are
         * auto-accepted by the stmt-pos gate.  Left as an explicit check
         * for robustness against future changes to gate (c). */
        if (cc__is_preceded_by_ident(s, prev, "return")) continue;

        /* Emit diagnostic. */
        int line_no = 1;
        cc__line_from_pos(s, id_a, &line_no);
        char name_buf[128];
        size_t nl = id_len < sizeof(name_buf) - 1 ? id_len : sizeof(name_buf) - 1;
        memcpy(name_buf, s + id_a, nl);
        name_buf[nl] = 0;
        cc_pass_error_cat(f, line_no, 1, "unhandled-result",
                          "call to '%s' returns a result-typed value; "
                          "consume with '?>', '!>', '@err', assign to a "
                          "result-typed destination, 'return', or cast to "
                          "'(void)' to explicitly discard",
                          name_buf);
        any_err = 1;
    }
    return any_err ? -1 : 0;
}

/* ------------------------------------------------------------------
 * Slice 6b: IR-driven rewrite for no-binder `!>` forms.
 *
 * Step 1.3b.iii of the AST-truth refactor (see docs/refactor-ast-truth.md).
 * Handles the three forms whose lowering is the legacy `!> -> @err`
 * substitution, no complex synthesis required:
 *
 *   Form A:  EXPR !>;                 -> EXPR @err;
 *   Form B:  EXPR !> STMT;            -> EXPR @err STMT;
 *   Form C:  EXPR !> { BLOCK }[;]     -> EXPR @err { BLOCK };
 *                                        (trailing `;` synthesized if absent)
 *
 * All three are captured as an UNWRAP_BANG node with binder_name == NULL.
 * We walk the IR tree in source order, emit OPAQUE_TEXT chunks verbatim,
 * emit matching UNWRAP_BANG nodes via the substitution above, and leave
 * everything else (binder bangs, ?> nodes) as literal raw_text so the
 * legacy fixed-point loop picks them up downstream.  Binder forms and
 * the `?>` family still live in the legacy path; subsequent 1.3b.iv /
 * 1.3b.v steps port those.
 *
 * On success returns 1 with a newly malloc'd buffer; 0 if there was
 * nothing to rewrite; -1 on allocation failure.  The caller owns
 * `*out_buf` and must free it.
 * ---------------------------------------------------------------- */

/* True iff `n` is a statement-position `!>` with no binder whose tail
 * was successfully parsed by the recogniser (Form A / B / C).  Expression-
 * position `!>` has a wholly different lowering (stmt-expr `({ ... })`
 * block), so it stays in the legacy path.  A tail-scan failure (empty
 * binder `()`, EOF mid-body, etc.) also stays in the legacy path so the
 * user gets the proper diagnostic from cc__rewrite_bang_once. */
static int cc__ir_is_nobinder_stmt_bang(const CCIrNode* n) {
    if (!n || n->kind != CC_IR_UNWRAP_BANG) return 0;
    if (n->as.unwrap.binder_name != NULL)    return 0;
    if (!n->as.unwrap.stmt_position)         return 0;
    /* Recogniser extended the span past the 2-byte sigil iff the tail
     * parse succeeded; on failure span_end stops right after `!>`.
     * raw_len <= sig_off + 2 is the "failed" shape. */
    const char* hit = strstr(n->raw_text, "!>");
    if (!hit) return 0;
    size_t sig_off = (size_t)(hit - n->raw_text);
    if (sig_off + 2 >= n->raw_len) return 0;
    return 1;
}

/* Find the offset of the `!>` / `?>` sigil within `raw_text` relative
 * to the node's span start.  Fast path: since the IR carver guaranteed
 * exactly one sigil per UNWRAP_* node and that the sigil is not inside
 * a comment or string literal, a plain strstr over the NUL-terminated
 * raw_text suffices.  Returns SIZE_MAX on (defensively) not-found. */
static size_t cc__ir_node_sigil_offset(const CCIrNode* n) {
    if (!n || !n->raw_text) return (size_t)-1;
    const char* needle = (n->kind == CC_IR_UNWRAP_BANG) ? "!>" : "?>";
    const char* hit = strstr(n->raw_text, needle);
    if (!hit) return (size_t)-1;
    return (size_t)(hit - n->raw_text);
}

static int cc__rewrite_nobinder_bangs_via_ir(const CCVisitorCtx* ctx,
                                             const char* in,
                                             size_t in_len,
                                             char** out_buf,
                                             size_t* out_len) {
    (void)ctx;
    if (!out_buf || !out_len) return 0;
    *out_buf = NULL;
    *out_len = 0;
    if (!in || in_len == 0) return 0;

    /* Quick check: is there any no-binder `!>` present?  If not, bail
     * out without paying the IR build cost.  The check mirrors the
     * recogniser (sigil presence is a strict superset of no-binder
     * sigil presence), so any false-positive just means we build the
     * IR and discover zero targets. */
    int has_bang = 0;
    for (size_t i = 0; i + 1 < in_len; i++) {
        if (in[i] == '!' && in[i + 1] == '>') { has_bang = 1; break; }
    }
    if (!has_bang) return 0;

    CCIrArena* arena = cc_ir_arena_create();
    if (!arena) return -1;

    int rc = 0;
    CCIrNode* root = cc_ir_build_from_stub(arena, /*root*/ NULL,
                                           in, in_len,
                                           ctx ? ctx->input_path : NULL);
    if (!root) { rc = -1; goto out; }

    /* Scan for targets first.  If none, skip emission entirely so the
     * legacy loop sees the untouched buffer. */
    int any_target = 0;
    for (size_t i = 0; i < root->children_len; i++) {
        if (cc__ir_is_nobinder_stmt_bang(root->children[i])) { any_target = 1; break; }
    }
    if (!any_target) goto out;

    if (getenv("CC_IR_REWRITE_TRACE")) {
        size_t count = 0;
        for (size_t i = 0; i < root->children_len; i++) {
            if (cc__ir_is_nobinder_stmt_bang(root->children[i])) count++;
        }
        fprintf(stderr,
                "[CC_IR_REWRITE] %s: rewriting %zu no-binder `!>` via IR\n",
                ctx && ctx->input_path ? ctx->input_path : "<input>", count);
    }

    char*  buf = NULL;
    size_t bl  = 0, bc = 0;

    for (size_t i = 0; i < root->children_len; i++) {
        const CCIrNode* c = root->children[i];
        if (!cc__ir_is_nobinder_stmt_bang(c)) {
            /* Everything else (opaque text, binder bangs, ?> nodes)
             * flows through verbatim.  The legacy loop handles any
             * remaining sigils. */
            cc__append_n(&buf, &bl, &bc, c->raw_text, c->raw_len);
            continue;
        }

        /* No-binder UNWRAP_BANG: substitute `!>` -> `@err`, preserve
         * surrounding bytes, synthesize `;` if the span didn't end
         * with one (Form C without trailing semicolon). */
        size_t sig_off = cc__ir_node_sigil_offset(c);
        if (sig_off == (size_t)-1 || sig_off + 2 > c->raw_len) {
            /* Defensive: the recogniser should never produce this,
             * but if it did, fall back to literal emission so the
             * legacy loop still rewrites it correctly. */
            cc__append_n(&buf, &bl, &bc, c->raw_text, c->raw_len);
            continue;
        }
        /* Prefix: LHS + any whitespace before the sigil. */
        cc__append_n(&buf, &bl, &bc, c->raw_text, sig_off);
        cc__append_str(&buf, &bl, &bc, "@err");
        /* Tail after `!>`.  For Form A this is just `;`; Form B has
         * ` STMT;`; Form C has ` { BLOCK }` possibly followed by `;`. */
        size_t tail_a = sig_off + 2;
        cc__append_n(&buf, &bl, &bc, c->raw_text + tail_a, c->raw_len - tail_a);

        /* Form C semicolon synthesis: if the span didn't already end
         * with `;`, add one so the downstream `@err` expansion parses
         * as a standalone statement.  Matches the legacy
         * `insert_semi_at` branch in cc__rewrite_bang_once. */
        if (c->raw_len == 0 || c->raw_text[c->raw_len - 1] != ';') {
            cc__append_str(&buf, &bl, &bc, ";");
        }
    }

    *out_buf = buf;
    *out_len = bl;
    rc = 1;

out:
    cc_ir_arena_destroy(arena);
    return rc;
}

int cc__rewrite_result_unwrap(const CCVisitorCtx* ctx,
                              const char* in_src,
                              size_t in_len,
                              char** out_src,
                              size_t* out_len) {
    if (!out_src || !out_len) return 0;
    *out_src = NULL;
    *out_len = 0;
    if (!in_src || in_len == 0) return 0;

    /* Phase-1 IR roundtrip check.  Under CC_VERIFY_IR=1 we build an IR
     * from the pass input, emit it back to text, and assert the
     * result is byte-identical to the input.  This proves the IR
     * skeleton (arena + build_from_stub + emit) is correct on real
     * source before step 1.3 starts carving typed nodes out of the
     * OPAQUE_TEXT passthrough.  See docs/refactor-ast-truth.md
     * phase 1.  No behavioural effect when the env var is unset. */
    if (cc_ir_verify_active()) {
        CCIrArena* arena = cc_ir_arena_create();
        if (arena) {
            CCIrNode* ir = cc_ir_build_from_stub(
                arena, /* root */ NULL,
                in_src, in_len,
                ctx ? ctx->input_path : NULL);
            if (ir) {
                if (getenv("CC_IR_DUMP")) {
                    fprintf(stderr,
                            "[CC_IR_DUMP] result_unwrap input for %s (%zu children):\n",
                            ctx && ctx->input_path ? ctx->input_path : "<input>",
                            ir->children_len);
                    cc_ir_dump(ir, stderr);
                }
                char*  emit_buf = NULL;
                size_t emit_len = 0;
                if (cc_ir_emit_text(ir, &emit_buf, &emit_len) == 0) {
                    (void)cc_ir_verify_diff("result_unwrap.roundtrip",
                                            in_src,   in_len,
                                            emit_buf, emit_len);
                }
                free(emit_buf);
            }
            cc_ir_arena_destroy(arena);
        }
    }

    /* Cheap early-out: if neither `?>` nor `!>` is present anywhere, skip
     * the operator rewrites.  We still need to run the strict
     * unhandled-result scan when `CC_STRICT_RESULT_UNWRAP=1`, because the
     * relevant result-typed declarations have already been rewritten by
     * `cc__rewrite_result_types` before us and no `!>` / `?>` sigil
     * remains in that case. */
    int need_rewrite = 0;
    {
        const char* p = in_src;
        size_t remaining = in_len;
        while (remaining >= 2) {
            size_t step;
            const char* q = NULL;
            const char* q1 = (const char*)memchr(p, '?', remaining - 1);
            const char* q2 = (const char*)memchr(p, '!', remaining - 1);
            if (q1 && (!q2 || q1 < q2)) q = q1;
            else if (q2) q = q2;
            if (!q) break;
            if (q[1] == '>') { need_rewrite = 1; break; }
            step = (size_t)(q - p) + 1;
            p += step;
            remaining -= step;
        }
    }
    int strict = cc__strict_enabled();
    if (!need_rewrite && !strict) return 0;

    char* cur = (char*)malloc(in_len + 1);
    if (!cur) return -1;
    memcpy(cur, in_src, in_len);
    cur[in_len] = 0;
    size_t curlen = in_len;

    int any = 0;

    /* Step 1.3b.iii: IR-driven preprocess for no-binder `!>` forms.
     * Rewrites Form A / B / C to the `@err` surface using the IR
     * recogniser's structured payload, then hands off to the legacy
     * fixed-point loop below for remaining sigils (binder `!>` and
     * the whole `?>` family).  The legacy loop is a no-op for TUs
     * whose only unwrap sigils were no-binder bangs, which covers a
     * significant fraction of real code (everywhere a default handler
     * is acceptable).
     *
     * Emits byte-identical output to pure-legacy under the full test
     * suite — verified by running CC_VERIFY_IR=1 before commit. */
    if (need_rewrite) {
        char* ir_pp = NULL;
        size_t ir_pp_len = 0;
        int ir_rc = cc__rewrite_nobinder_bangs_via_ir(ctx, cur, curlen,
                                                     &ir_pp, &ir_pp_len);
        if (ir_rc < 0) { free(cur); return -1; }
        if (ir_rc == 1 && ir_pp) {
            free(cur);
            cur    = ir_pp;
            curlen = ir_pp_len;
            any    = 1;
        }
    }

    if (need_rewrite) {
        /* Process `?>` expression operators to fixed point. */
        for (int iter = 0; iter < 64; iter++) {
            char* next = NULL;
            size_t nl = 0;
            int r = cc__rewrite_result_unwrap_once(ctx, cur, curlen, &next, &nl);
            if (r < 0) {
                free(cur);
                return -1;
            }
            if (r == 0) break;
            free(cur);
            cur = next;
            curlen = nl;
            any = 1;
        }
        /* Process `!>` statement operators to fixed point.  Running after `?>`
         * is a convenience: the two operators are disjoint in terms of the
         * lowering they produce, so either order works, but doing `?>` first
         * means any `!>` that textually contains a `?>` inside its body has
         * already been reduced when the outer rewrite fires. */
        for (int iter = 0; iter < 64; iter++) {
            char* next = NULL;
            size_t nl = 0;
            int r = cc__rewrite_bang_once(ctx, cur, curlen, &next, &nl);
            if (r < 0) {
                free(cur);
                return -1;
            }
            if (r == 0) break;
            free(cur);
            cur = next;
            curlen = nl;
            any = 1;
        }
    }

    /* Slice 7: strict unhandled-result scan.  Runs on the fully-rewritten
     * text so that legitimate `?>`/`!>` consumers have already been
     * subsumed into assignment-like constructs. */
    if (strict) {
        if (cc__strict_unhandled_scan(ctx, cur, curlen) < 0) {
            free(cur);
            return -1;
        }
    }

    if (!any) {
        free(cur);
        return 0;
    }
    *out_src = cur;
    *out_len = curlen;
    return 1;
}
