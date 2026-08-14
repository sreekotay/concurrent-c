#include "pass_result_unwrap.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ir/ir.h"
#include "ir/verifier.h"
#include "result_spec.h"
#include "util/path.h"
#include "util/result_fn_registry.h"
#include "util/text.h"
#include "util/text_scan.h"
#include "visitor/errhandler_lookup.h"
#include "visitor/pass_common.h"
#include "visitor/ufcs.h"
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

/* Resolve `recv.method(...)` / `recv->method(...)` to the default snake_case
 * callee (`CCListener.accept` → `cc_listener_accept`) when the receiver is a
 * simple identifier whose declared type is visible earlier in `s`.  Used so
 * `!>` binders still get the Result error arm type if a UFCS form somehow
 * reaches unwrap before text/AST UFCS rewriting. */
static int cc__ru_extract_ufcs_callee(const char* s, size_t n,
                                       size_t a, size_t b,
                                       char* out, size_t out_sz) {
    char method[64];
    char recv[128];
    char recv_type[256];
    size_t sep, method_a, method_b, recv_a, recv_b, k;
    size_t pos;
    if (!s || !out || out_sz == 0 || a >= b || b > n) return 0;
    out[0] = 0;
    while (a < b && isspace((unsigned char)s[a])) a++;
    while (b > a && isspace((unsigned char)s[b - 1])) b--;
    if (a >= b || s[b - 1] != ')') return 0;
    /* Find the top-level call's '(' just before the trailing `)`.
     * Walk from inside that `)` with depth 1; the old loop counted the
     * trailing `)` first so the matching `(` never saw depth 0. */
    {
        int depth = 1;
        size_t open = (size_t)-1;
        size_t j = b - 1; /* s[b-1] == ')' */
        while (j > a) {
            j--;
            if (s[j] == ')') depth++;
            else if (s[j] == '(') {
                depth--;
                if (depth == 0) { open = j; break; }
            }
        }
        if (open == (size_t)-1 || open <= a) return 0;
        k = open;
        while (k > a && isspace((unsigned char)s[k - 1])) k--;
        method_b = k;
        while (k > a && cc_is_ident_char(s[k - 1])) k--;
        method_a = k;
        if (method_a >= method_b || method_b - method_a >= sizeof(method)) return 0;
        while (k > a && isspace((unsigned char)s[k - 1])) k--;
        if (k > a && s[k - 1] == '.') {
            sep = k - 1;
        } else if (k >= a + 2 && s[k - 2] == '-' && s[k - 1] == '>') {
            sep = k - 2;
        } else {
            return 0;
        }
        memcpy(method, s + method_a, method_b - method_a);
        method[method_b - method_a] = 0;
        recv_b = sep;
        while (recv_b > a && isspace((unsigned char)s[recv_b - 1])) recv_b--;
        recv_a = recv_b;
        while (recv_a > a && cc_is_ident_char(s[recv_a - 1])) recv_a--;
        if (recv_a >= recv_b || recv_b - recv_a >= sizeof(recv)) return 0;
        /* Only simple identifier receivers (ln.accept / ln_ptr->accept). */
        if (recv_a > a && cc_is_ident_char(s[recv_a - 1])) return 0;
        memcpy(recv, s + recv_a, recv_b - recv_a);
        recv[recv_b - recv_a] = 0;
    }
    /* Scan earlier declarations for `Type recv` / `Type* recv`. */
    recv_type[0] = 0;
    pos = 0;
    while (pos < recv_a) {
        size_t hit = cc_find_ident_top_level(s, pos, recv_a, recv, strlen(recv));
        size_t after, before, ty_a, ty_b, stars, tl;
        if (hit >= recv_a) break;
        after = hit + strlen(recv);
        after = cc_skip_ws_and_comments(s, n, after);
        if (after < n && s[after] == '(') { pos = after; continue; }
        before = hit;
        while (before > 0 && isspace((unsigned char)s[before - 1])) before--;
        stars = 0;
        while (before > 0 && s[before - 1] == '*') {
            stars++;
            before--;
            while (before > 0 && isspace((unsigned char)s[before - 1])) before--;
        }
        /* After rskip of spaces between type and name, `before` is the
         * exclusive end of the type token (points at the space or name). */
        ty_b = before;
        ty_a = before;
        while (ty_a > 0 && cc_is_ident_char(s[ty_a - 1])) ty_a--;
        if (ty_b > ty_a && ty_b - ty_a + stars + 1 < sizeof(recv_type)) {
            tl = ty_b - ty_a;
            memcpy(recv_type, s + ty_a, tl);
            recv_type[tl] = 0;
            while (stars-- > 0) {
                size_t cur = strlen(recv_type);
                if (cur + 1 >= sizeof(recv_type)) break;
                recv_type[cur] = '*';
                recv_type[cur + 1] = 0;
            }
        }
        pos = hit + strlen(recv);
    }
    if (!recv_type[0]) return 0;
    if (getenv("CC_DEBUG_RU_UFCS"))
        fprintf(stderr, "ru_ufcs_callee: recv='%s' type='%s' method='%s'\n",
                recv, recv_type, method);
    if (!cc_ufcs_compose_default_callee(out, out_sz, recv_type, method)) return 0;
    return 1;
}

/* Resolve the callee name for binder typing: plain call, else UFCS default. */
static int cc__ru_extract_callee_for_binder(const char* s, size_t n,
                                             size_t call_a, size_t call_b,
                                             char* out, size_t out_sz) {
    if (cc__ru_extract_plain_callee(s, call_a, call_b, out, out_sz)) return 1;
    return cc__ru_extract_ufcs_callee(s, n, call_a, call_b, out, out_sz);
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

/* Pull the error-type suffix out of a mangled `CCResult_<Ok>_<Err>` name.
 * Prefer the stdlib-predeclared table (exact ok/err split) so Err types that
 * themselves contain underscores (e.g. `CCHttpErrorInfo`) are not truncated
 * by the last-`_` heuristic.  Fall back to the last `_` segment after the
 * `CCResult_` prefix when the ok-type itself contains underscores
 * (e.g. `CCResult_CCSocket_CCNetError`). */
static int cc__ru_err_type_from_result_name(const char* result_name,
                                             char* out, size_t out_sz) {
    const char* p;
    const char* last_us;
    size_t len;
    const CCStdlibPredeclaredResult* pre;
    if (!result_name || !out || out_sz == 0) return 0;
    out[0] = 0;
    if (strncmp(result_name, "CCResult_", 9) != 0) return 0;
    pre = cc_result_spec_lookup_stdlib_predeclared(result_name);
    if (pre && pre->err_type) {
        len = strlen(pre->err_type);
        if (len + 1 > out_sz) return 0;
        memcpy(out, pre->err_type, len);
        out[len] = 0;
        return 1;
    }
    p = result_name + 9;
    last_us = strrchr(p, '_');
    if (!last_us || last_us[1] == 0) return 0;
    len = strlen(last_us + 1);
    if (len + 1 > out_sz) return 0;
    memcpy(out, last_us + 1, len);
    out[len] = 0;
    return 1;
}

static void cc__ru_emit_uw_err_binder(char** out, size_t* ol, size_t* oc,
                                       const char* s, size_t n,
                                       size_t call_a, size_t call_b,
                                       const char* tmpv, const char* binder,
                                       const char* file, int line) {
    /* Prefer the typed binder path when the callee is a plain name (or a
     * UFCS form we can lower to one) whose declared error type we know.
     * Gives the binder the user-facing error type name (e.g. `CCNetError`)
     * rather than an anonymous `__typeof__` expansion — and avoids the
     * default `__cc_uw_err_at` arm typing the binder as ambient `CCError`.
     *
     * Since parser-mode Result specs now emit distinct typed structs
     * (see `cc/include/ccc/cc_result.cch` + preprocess.c), `(tmp).u.error`
     * already has the declared error type in both parser mode and the
     * real compile; no `*(E*)(void*)&` through-pointer dance needed.
     *
     * R3: even on the optimized direct-field-access path we must still
     * push the propagation site onto the runtime unwrap chain, so an
     * `@errhandler` walking the chain sees this `!>` site.  Without
     * this the typed-callee path would silently bypass `__cc_uw_err_at`
     * and the chain would only show non-typed propagations — a hole
     * exactly where most production code lives.  The record() call sits
     * on the *error* branch only (since the binder declaration only runs
     * after the `is_err` guard succeeds), so it does not fire on the
     * fast Ok path. */
    char callee[128];
    char err_type[256];
    char result_type[256];
    int have_err = 0;
    if (cc__ru_extract_callee_for_binder(s, n, call_a, call_b, callee, sizeof(callee))) {
        if (cc_result_fn_registry_get_err_type(callee, strlen(callee),
                                                err_type, sizeof(err_type))) {
            have_err = 1;
        } else {
            /* Scan for `CCResult_*_<E> <callee>(` (works for composed UFCS
             * names like `cc_listener_accept` declared in std headers). */
            size_t pos = 0;
            size_t clen = strlen(callee);
            while (pos < n) {
                size_t hit = cc_find_ident_top_level(s, pos, n, callee, clen);
                size_t after, p, end, len;
                if (hit >= n) break;
                after = cc_skip_ws_and_comments(s, n, hit + clen);
                if (after < n && s[after] == '(') {
                    /* Comment-aware: `CCResult_T_E / *doc* / foo(` still
                     * reads the return type. */
                    p = cc_rskip_ws_and_comments(s, hit);
                    end = p;
                    while (p > 0 && cc_is_ident_char(s[p - 1])) p--;
                    len = end - p;
                    if (len > 9 && memcmp(s + p, "CCResult_", 9) == 0 &&
                        len + 1 <= sizeof(result_type)) {
                        memcpy(result_type, s + p, len);
                        result_type[len] = 0;
                        have_err = cc__ru_err_type_from_result_name(result_type,
                                                                    err_type, sizeof(err_type));
                        if (have_err) break;
                    }
                }
                pos = hit + clen;
            }
        }
    }
    if (have_err) {
        const char* f = file ? file : "<input>";
        size_t fl = strlen(f);
        cc__append_str(out, ol, oc, "cc_rt_diag_record_unwrap_site(");
        cc_sb_append_c_string_literal(out, ol, oc, f, 0, fl);
        cc_sb_append_fmt(out, ol, oc, ", \"%d\"); ", line);
        cc_sb_append_fmt(out, ol, oc,
                         "%s %s = (%s).u.error; ",
                         err_type, binder, tmpv);
        return;
    }
    /* Fallback: untyped callee (method call, chained expression, or a
     * pointer-returning expression with no registry entry).  Use
     * `__typeof__(__cc_uw_err_at(...))` so the binder resolves to:
     *   - the Result struct's declared error field for Result LHS, and
     *   - `CCError` for raw-pointer LHS (via the default _Generic arm).
     *
     * R3 record() fires inside the `__cc_uw_err_at` macro arms (see
     * cc_result.cch + the per-TU enumerated arms in visit_codegen.c),
     * so no explicit record() call is needed here — the macro
     * expansion below already side-effects on each evaluation. */
    cc__append_str(out, ol, oc, "__typeof__(");
    cc_sb_append_uw_err_at(out, ol, oc, tmpv, s, call_a, call_b, file, line);
    cc_sb_append_fmt(out, ol, oc, ") %s = ", binder);
    cc_sb_append_uw_err_at(out, ol, oc, tmpv, s, call_a, call_b, file, line);
    cc__append_str(out, ol, oc, "; ");
}

static int cc__ru_find_callee_result_type(const char* s, size_t n,
                                          size_t call_a, size_t call_b,
                                          char* out, size_t out_sz) {
    char callee[128];
    if (!out || out_sz == 0) return 0;
    out[0] = 0;
    /* Plain `fn(...)` first; else UFCS `recv.method(...)` → default snake
     * callee (e.g. `part.clone_into(a)` → `cc_slice_clone_into`). Bang
     * expression lowering used to require a plain callee here, so UFCS
     * calls fell through to `__typeof__(recv.method(...))` + `__cc_uw_value`
     * before AST/text UFCS could lower the call — main-pass typing then
     * saw the sugared form and failed (`int`/`CCSliceShared` noise). */
    if (!cc__ru_extract_callee_for_binder(s, n, call_a, call_b, callee,
                                          sizeof(callee))) {
        return 0;
    }

    size_t callee_len = strlen(callee);
    size_t pos = 0;
    while (pos < n) {
        size_t hit = cc_find_ident_top_level(s, pos, n, callee, callee_len);
        if (hit >= n) break;
        size_t after = cc_skip_ws_and_comments(s, n, hit + callee_len);
        if (after < n && s[after] == '(') {
            size_t p = cc_rskip_ws_and_comments(s, hit);
            size_t end = p;
            while (p > 0 && cc_is_ident_char(s[p - 1])) p--;
            size_t len = end - p;
            if (len > 9 && memcmp(s + p, "CCResult_", 9) == 0 && len + 1 <= out_sz) {
                memcpy(out, s + p, len);
                out[len] = 0;
                return 1;
            }
            {
                /* Line-head walk, comment-aware: a `;`/`}`/newline inside
                 * a block comment is not a statement boundary. */
                size_t line_a = p;
                while (line_a > 0) {
                    if (line_a >= 2 && s[line_a - 1] == '/' && s[line_a - 2] == '*') {
                        size_t c2 = line_a - 2, op = (size_t)-1;
                        while (c2 > 0) {
                            c2--;
                            if (s[c2] == '*' && c2 > 0 && s[c2 - 1] == '/') { op = c2 - 1; break; }
                        }
                        if (op != (size_t)-1) { line_a = op; continue; }
                    }
                    if (s[line_a - 1] == '\n' || s[line_a - 1] == ';' || s[line_a - 1] == '}') break;
                    line_a--;
                }
                size_t bang = cc_find_substr_top_level(s, line_a, hit, "!>", 2);
                if (bang < hit) {
                    size_t err_l = cc_skip_ws_and_comments(s, n, bang + 2);
                    size_t err_r = 0;
                    if (err_l < hit && s[err_l] == '(' &&
                        cc_find_matching_paren(s, n, err_l, &err_r) &&
                        err_r < hit) {
                        size_t ok_b = cc_rskip_ws_and_comments(s, bang);
                        if (ok_b < line_a) ok_b = line_a;
                        size_t ok_a = ok_b;
                        while (ok_a > line_a && cc_is_ident_char(s[ok_a - 1])) ok_a--;
                        size_t err_a = err_l + 1, err_b = err_r;
                        while (err_a < err_b && isspace((unsigned char)s[err_a])) err_a++;
                        while (err_b > err_a && isspace((unsigned char)s[err_b - 1])) err_b--;
                        if (ok_b > ok_a && err_b > err_a) {
                            char ok_m[96];
                            char err_m[96];
                            size_t ok_len = ok_b - ok_a;
                            size_t err_len = err_b - err_a;
                            if (ok_len == 5 && memcmp(s + ok_a, "_Bool", 5) == 0) {
                                memcpy(ok_m, "bool", 5);
                            } else if (ok_len < sizeof(ok_m)) {
                                memcpy(ok_m, s + ok_a, ok_len);
                                ok_m[ok_len] = 0;
                            } else {
                                ok_m[0] = 0;
                            }
                            if (err_len < sizeof(err_m)) {
                                memcpy(err_m, s + err_a, err_len);
                                err_m[err_len] = 0;
                            } else {
                                err_m[0] = 0;
                            }
                            if (ok_m[0] && err_m[0]) {
                                int wrote = snprintf(out, out_sz, "CCResult_%s_%s", ok_m, err_m);
                                if (wrote > 0 && (size_t)wrote < out_sz) return 1;
                            }
                        }
                    }
                }
            }
        }
        pos = hit + callee_len;
    }

    /* Header static-inline callees are invisible in the current buffer
     * (`#include <...>` is not expanded here).  Consult the registry seeded
     * when included .cch trees were registered / when surface `T !>(E) name`
     * forms were rewritten. */
    if (cc_result_fn_registry_get_result_type(callee, callee_len, out, out_sz)) {
        return 1;
    }
    return 0;
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
 * a comment, string literal, or preprocessor directive body.  Returns 1
 * and writes *out_pos on success, 0 if no such occurrence exists.
 * Routed through `CCInertScan` for shared state-machine logic. */
static int cc__find_unwrap_token(const char* s, size_t n, size_t* out_pos) {
    CCInertScan scan;
    cc_inert_scan_init(&scan, NULL);
    size_t i = 0;
    while (i < n) {
        if (cc_inert_scan_step(&scan, s, n, &i)) continue;
        if (s[i] == '?' && i + 1 < n && s[i + 1] == '>') {
            /* Reject if the preceding non-ws char suggests we misparsed a
             * different operator (e.g. '??>' trigraph / digraph lookalikes).
             * In practice the CC source does not emit those so a literal
             * match is fine, but we still guard `?>?` style weirdness. */
            if (out_pos) *out_pos = i;
            return 1;
        }
        i++;
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
 * closing quote char; on return *i points at the opening quote char.
 * Naive "enter string mode + treat `\\` as escape while walking backward"
 * mis-parses `'\n'` / `'\t'` / `'\\'`: the escape skip consumes the
 * opening quote and leaves the scanner stuck in-string.  Resolve the
 * opener with a forward scan from the line start (proper C escape rules)
 * and jump to it — same approach as pass_err_syntax's
 * `cc__err_skip_string_or_char_backward`. */
static void cc__skip_str_backward(const char* s, size_t* i) {
    size_t close = *i;
    char qch = s[close];
    if (qch != '"' && qch != '\'') return;
    size_t line_start = close;
    while (line_start > 0 && s[line_start - 1] != '\n') line_start--;
    size_t k = line_start;
    size_t open = close;
    int in_q = 0;
    char cur_q = 0;
    while (k <= close) {
        char c = s[k];
        if (!in_q) {
            if (c == '"' || c == '\'') {
                in_q = 1;
                cur_q = c;
                open = k;
                if (k == close) break;
                k++;
                continue;
            }
            k++;
            continue;
        }
        if (c == '\\' && k + 1 <= close) {
            k += 2;
            continue;
        }
        if (c == cur_q) {
            if (k == close) {
                *i = open;
                return;
            }
            in_q = 0;
            cur_q = 0;
            k++;
            continue;
        }
        k++;
    }
    *i = open;
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
        if (c != '\n' && cc_scan_pos_in_line_comment(s, i)) {
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
 * RHS (i.e. the terminator).  Routed through `CCInertScan` so the same
 * state machine governs all visitor passes. */
static int cc__find_rhs_end_forward(const char* s, size_t n, size_t from, size_t* out_end) {
    int par = 0, brk = 0, br = 0;
    CCInertScan scan;
    cc_inert_scan_init(&scan, NULL);
    size_t i = from;
    while (i < n) {
        if (cc_inert_scan_step(&scan, s, n, &i)) continue;
        char ch = s[i];
        char ch2 = (i + 1 < n) ? s[i + 1] : 0;

        if (ch == '(') { par++; i++; continue; }
        if (ch == '[') { brk++; i++; continue; }
        if (ch == '{') { br++;  i++; continue; }
        if (ch == ')') {
            if (par == 0) { *out_end = i; return 1; }
            par--; i++; continue;
        }
        if (ch == ']') {
            if (brk == 0) { *out_end = i; return 1; }
            brk--; i++; continue;
        }
        if (ch == '}') {
            if (br == 0) { *out_end = i; return 1; }
            br--; i++; continue;
        }
        if (par > 0 || brk > 0 || br > 0) { i++; continue; }

        if (ch == ';' || ch == ',') { *out_end = i; return 1; }
        if (ch == '?') {
            /* Another `?>` at our depth ends the current RHS; a plain `?`
             * (ternary) also ends it because we bind tighter than `?:`. */
            *out_end = i; return 1;
        }
        if (ch == ':') { *out_end = i; return 1; }
        if (ch == '&' && ch2 == '&') { *out_end = i; return 1; }
        if (ch == '|' && ch2 == '|') { *out_end = i; return 1; }
        i++;
    }
    *out_end = n;
    return 1;
}

/* Ledger-aware: honors `#line`/CC_LN markers in the intermediate buffer
 * so recorded unwrap sites and diagnostics carry *user* lines, not
 * physical lines of the lowered text. */
static void cc__line_from_pos(const char* s, size_t pos, int* line) {
    *line = cc_user_line_for_offset(s, pos, pos, 1, NULL, NULL);
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

static void cc__ru_emit_is_err(char** out, size_t* ol, size_t* oc,
                               const char* result_type,
                               const char* tmpv) {
    if (result_type && result_type[0]) {
        cc_sb_append_fmt(out, ol, oc, "%s_is_err(%s)", result_type, tmpv);
    } else {
        cc_sb_append_fmt(out, ol, oc, "__cc_uw_is_err(%s)", tmpv);
    }
}

static void cc__ru_emit_value(char** out, size_t* ol, size_t* oc,
                              const char* result_type,
                              const char* tmpv) {
    if (result_type && result_type[0]) {
        cc_sb_append_fmt(out, ol, oc, "%s_unwrap(%s)", result_type, tmpv);
    } else {
        cc_sb_append_fmt(out, ol, oc, "__cc_uw_value(%s)", tmpv);
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
    /* Find the next top-level `;` after `from`, skipping comments,
     * string/char literals, preprocessor-directive bodies, and
     * nested `()`/`[]`/`{}` groups.  Routed through `CCInertScan`
     * so the same state machine governs all visitor passes. */
    int par = 0, brk = 0, br = 0;
    CCInertScan scan;
    cc_inert_scan_init(&scan, NULL);
    size_t i = from;
    while (i < n) {
        if (cc_inert_scan_step(&scan, s, n, &i)) continue;
        char ch = s[i];
        if (ch == '(') { par++; i++; continue; }
        if (ch == '[') { brk++; i++; continue; }
        if (ch == '{') { br++;  i++; continue; }
        if (ch == ')') { if (par == 0) return 0; par--; i++; continue; }
        if (ch == ']') { if (brk == 0) return 0; brk--; i++; continue; }
        if (ch == '}') { if (br  == 0) return 0; br--;  i++; continue; }
        if (par == 0 && brk == 0 && br == 0 && ch == ';') {
            if (out_semi) *out_semi = i;
            return 1;
        }
        i++;
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
    scan = cc_skip_ws_and_comments(s, n, scan);

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
            size_t post = cc_skip_ws_and_comments(s, n, rpar + 1);
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
    size_t rhs_scan = cc_skip_ws_and_comments(s, n, scan);
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
    char result_type[256];
    snprintf(tmpv, sizeof(tmpv), "__cc_pu_r_%d", id);
    result_type[0] = 0;
    (void)cc__ru_find_callee_result_type(s, n, lhs_a, lhs_b,
                                         result_type, sizeof(result_type));

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

    if (result_type[0]) {
        cc_sb_append_fmt(&out, &ol, &oc, "({ %s %s = (", result_type, tmpv);
    } else {
        cc__append_str(&out, &ol, &oc, "({ __typeof__(");
        cc__append_n(&out, &ol, &oc, s + lhs_a, lhs_b - lhs_a);
        cc_sb_append_fmt(&out, &ol, &oc, ") %s = (", tmpv);
    }
    cc__append_n(&out, &ol, &oc, s + lhs_a, lhs_b - lhs_a);
    cc__append_str(&out, &ol, &oc, "); !");
    cc__ru_emit_is_err(&out, &ol, &oc, result_type, tmpv);
    cc__append_str(&out, &ol, &oc, " ? ");
    cc__ru_emit_value(&out, &ol, &oc, result_type, tmpv);
    cc__append_str(&out, &ol, &oc, " : ");
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
        cc__ru_emit_uw_err_binder(&out, &ol, &oc, s, n, lhs_a, lhs_b, tmpv, mangled_binder, f, line_no);
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
    /* Find the next `!>` token at top level (skipping comments,
     * string literals, char literals, and preprocessor-directive
     * bodies).  Without this, a benign `#define UNUSED_UNWRAP(expr)
     * expr !>` would be mis-rewritten as if `expr !>` were real
     * source-position code; without the comment/string handling, a
     * `// expr !>` line comment or `"... !>"` string would similarly
     * trip the scanner.  Routed through the shared `CCInertScan`
     * helper so the same state machine governs all visitor passes
     * (see `cc/src/util/text_scan.h`). */
    CCInertScan scan;
    cc_inert_scan_init(&scan, NULL);
    size_t i = start;
    while (i < n) {
        if (cc_inert_scan_step(&scan, s, n, &i)) continue;
        /* Disambiguate against `!=`: `!=` has `=` after `!`, so the
         * `s[i+1] == '>'` check already excludes it.  Any leading
         * punctuation is fine — `!>` is a two-char operator at word
         * boundary regardless of what precedes it. */
        if (s[i] == '!' && i + 1 < n && s[i + 1] == '>') {
            if (out_pos) *out_pos = i;
            return 1;
        }
        i++;
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
/* Comment/string-aware so that a leading block comment like the one in
 * front of `@noblock static RedisReply !>(CCError) execute_request(...)`
 * in real_projects/redis/redis_idiomatic.ccs (which happens to mention
 * `\`RedisReply !>(CCError)\`` in prose) doesn't mask the decl form.
 * Kept in sync with cc_ir_lhs_is_parenless in cc/src/ir/ir.c. */
static int cc__bang_lhs_looks_like_decl(const char* s, size_t lhs_a,
                                        size_t lhs_b) {
    if (lhs_b <= lhs_a) return 0;
    CCInertScan scan;
    cc_inert_scan_init(&scan, NULL);
    scan.at_line_start = 0;  /* mid-buffer slice */
    size_t i = lhs_a;
    while (i < lhs_b) {
        if (cc_inert_scan_step(&scan, s, lhs_b, &i)) continue;
        char c = s[i];
        if (c == '(' || c == ')') return 0;
        i++;
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

/* Find the in-scope `@errhandler` whose parameter type matches Result E
 * (exact, else unique `@as` path).  Brace-scoped; fail closed on miss.
 * Body aliases into `s`.  *out_as_path empty on exact match. */
static int cc__pu_find_outer_errhandler(const char* s, size_t n, size_t pos,
                                        size_t call_a, size_t call_b,
                                        char* out_decl, size_t out_decl_sz,
                                        size_t* out_decl_len,
                                        const char** out_body,
                                        size_t* out_body_len,
                                        size_t* out_decl_pos,
                                        char* out_err_type, size_t out_err_type_sz,
                                        char* out_as_path, size_t out_as_path_sz,
                                        int* out_have_handlers,
                                        int* out_as_diag,
                                        int* out_ambient) {
    return cc_errhandler_find_for_call(s, n, pos, call_a, call_b,
                                       out_decl, out_decl_sz, out_decl_len,
                                       out_body, out_body_len, out_decl_pos,
                                       out_err_type, out_err_type_sz,
                                       out_as_path, out_as_path_sz,
                                       out_have_handlers,
                                       out_as_diag, out_ambient);
}

/* Bare `!>;` whose matching `@errhandler(E)` body contains the call would
 * re-enter that handler. Diagnose — report via a helper (cc_error_log /
 * cc_error_exit) or `!> { abort(); }`, not `!>;` inside the same-E body. */
static int cc__pu_bare_bang_reenters_handler(const char* s, size_t call_a,
                                             const char* outer_body,
                                             size_t outer_body_len) {
    size_t body_off;
    if (!s || !outer_body || outer_body_len == 0) return 0;
    if (outer_body < s) return 0;
    body_off = (size_t)(outer_body - s);
    return call_a >= body_off && call_a < body_off + outer_body_len;
}

static void cc__pu_diag_same_e_reenter(const char* f, int line) {
    cc_pass_error_cat(f, line, 1, CC_ERR_SYNTAX,
                      "bare '!>;' inside matching '@errhandler' would re-enter "
                      "the same handler; use a helper (cc_error_exit) or "
                      "'!> { abort(); }' for reporting");
}

static void cc__pu_errhandler_miss_diag(const char* f, int line,
                                        const char* err_type, int have_handlers,
                                        int as_diag, const char* bare_msg) {
    char msg[256];
    const char* et = (err_type && err_type[0]) ? err_type : "CCError";
    if (!have_handlers) {
        cc_pass_error_cat(f, line, 1, CC_ERR_SYNTAX, "%s", bare_msg);
        return;
    }
    if (as_diag == CC_ERRHANDLER_AS_AMBIG) {
        snprintf(msg, sizeof(msg),
                 "ambiguous '@errhandler' for error type '%s': "
                 "multiple as: faces match in-scope handlers",
                 et);
        cc_pass_error_cat(f, line, 1, CC_ERR_SYNTAX, "%s", msg);
        cc_pass_note(f, line, 1,
                     "exact '@errhandler(%s)' wins; otherwise keep a single face-typed handler in scope",
                     et);
        return;
    }
    if (as_diag == CC_ERRHANDLER_AS_CYCLE) {
        snprintf(msg, sizeof(msg),
                 "cyclic as: path while matching '@errhandler' for error type '%s'",
                 et);
        cc_pass_error_cat(f, line, 1, CC_ERR_SYNTAX, "%s", msg);
        return;
    }
    snprintf(msg, sizeof(msg), "no matching '@errhandler' for error type '%s'", et);
    cc_pass_error_cat(f, line, 1, CC_ERR_SYNTAX, "%s", msg);
}

/* Emit binder typed as the handler param, projecting Result E through
 * `@as` path when non-empty: `CCError b = (tmp).u.error.base;`. */
static void cc__ru_emit_handler_err_binder(char** out, size_t* ol, size_t* oc,
                                           const char* s, size_t n,
                                           size_t call_a, size_t call_b,
                                           const char* tmpv, const char* binder,
                                           const char* handler_decl,
                                           const char* as_path,
                                           const char* file, int line) {
    char param_type[128];
    char param_name[64];
    param_type[0] = 0;
    param_name[0] = 0;
    if (handler_decl && handler_decl[0] &&
        cc_errhandler_split_param_decl(handler_decl,
                                       param_type, sizeof(param_type),
                                       param_name, sizeof(param_name)) &&
        param_type[0] && as_path && as_path[0]) {
        const char* f = file ? file : "<input>";
        size_t fl = strlen(f);
        cc__append_str(out, ol, oc, "cc_rt_diag_record_unwrap_site(");
        cc_sb_append_c_string_literal(out, ol, oc, f, 0, fl);
        cc_sb_append_fmt(out, ol, oc, ", \"%d\"); ", line);
        cc_sb_append_fmt(out, ol, oc, "%s %s = (%s).u.error.%s; ",
                         param_type, binder, tmpv, as_path);
        return;
    }
    cc__ru_emit_uw_err_binder(out, ol, oc, s, n, call_a, call_b, tmpv, binder,
                              file, line);
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
 *     exit, _Exit, _exit, abort, cc_error_exit, longjmp, siglongjmp,
 *     pthread_exit, __builtin_unreachable, __builtin_trap
 *   - A bare compound block `{ ... }` whose last statement satisfies
 *     the rule recursively.
 *
 * Block-structured statements like `if (cond) { return X; }` are NOT
 * treated as divergent even if every inner branch returns — the rule
 * is intentionally syntactic and conservative; users who need a real
 * guard can wrap the tail in a `return`/`exit`/`@err(...)`/`goto`. */

static const char* const CC_PU_NORETURN_FNS[] = {
    "exit", "_Exit", "_exit", "abort",
    "cc_error_exit",
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
    /* Phase 1: skip leading whitespace/comments. */
    {
        CCInertScan skip;
        cc_inert_scan_init(&skip, NULL);
        skip.at_line_start = 0;  /* mid-buffer slice */
        while (i < blen) {
            if (cc_inert_scan_step(&skip, body, blen, &i)) continue;
            char c = body[i];
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') { i++; continue; }
            break;
        }
    }
    if (i >= blen) {
        *io_i = i;
        *out_a = i;
        *out_b = i;
        return;
    }

    *out_a = i;
    int par = 0, brk = 0, br = 0;
    CCInertScan scan;
    cc_inert_scan_init(&scan, NULL);
    scan.at_line_start = 0;  /* mid-buffer slice */
    while (i < blen) {
        if (cc_inert_scan_step(&scan, body, blen, &i)) continue;
        char ch = body[i];
        if (ch == '(') { par++; i++; continue; }
        if (ch == '[') { brk++; i++; continue; }
        if (ch == '{') { br++; i++; continue; }
        if (ch == ')') { if (par) par--; i++; continue; }
        if (ch == ']') { if (brk) brk--; i++; continue; }
        if (ch == '}') {
            if (br) {
                br--;
                if (br == 0 && par == 0 && brk == 0) {
                    *out_b = i + 1;
                    *io_i = i + 1;
                    return;
                }
            }
            i++;
            continue;
        }
        if (par == 0 && brk == 0 && br == 0 && ch == ';') {
            *out_b = i + 1;
            *io_i = i + 1;
            return;
        }
        i++;
    }
    *out_b = blen;
    *io_i = blen;
}

/* Top-level: does the handler body diverge at its last statement? */
/* Does the token at `scan` end the bare `!>` form?
 *
 * `!>` has no closing delimiter — its handler body runs to the terminating
 * `;` — so what follows decides which form was written.  The bare form (defer
 * to the enclosing `@errhandler`) applies wherever the next token CANNOT BEGIN
 * A STATEMENT, since a handler body is one: a closer or separator ends the
 * expression, and so does any operator that only ever appears infix.
 *
 * Prefixes that can begin a statement stay a handler body, because there is no
 * honest way to choose: in `f() !> *p = 0;` the `*p = 0;` reads equally well as
 * a body or as a multiplication, and guessing would silently drop one of them.
 * Parenthesising the unwrap — `(f() !>) * p` — says which was meant, and `)`
 * is a terminator, so the escape hatch is always available. */
static int cc__pu_bare_terminator(const char* s, size_t n, size_t scan) {
    char c, d;
    if (scan >= n) return 0;
    c = s[scan];
    d = (scan + 1 < n) ? s[scan + 1] : 0;
    /* Closers and separators: the expression is over. */
    if (c == ';' || c == ')' || c == ',' || c == ']' || c == '}' || c == ':')
        return 1;
    /* Two-character infix operators whose first byte is otherwise ambiguous. */
    if (d == '=' && (c == '!' || c == '=' || c == '<' || c == '>' ||
                     c == '+' || c == '-' || c == '*' || c == '/' ||
                     c == '%' || c == '&' || c == '|' || c == '^'))
        return 1;
    if ((c == '&' && d == '&') || (c == '|' && d == '|') ||
        (c == '<' && d == '<') || (c == '>' && d == '>'))
        return 1;
    /* Single-character operators that are only ever infix. */
    if (c == '<' || c == '>' || c == '=' || c == '?' ||
        c == '%' || c == '/' || c == '^' || c == '|')
        return 1;
    return 0;
}

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
    CCInertScan scan;
    cc_inert_scan_init(&scan, NULL);
    scan.at_line_start = 0;  /* mid-buffer slice */
    size_t i = from;
    while (i < n) {
        if (cc_inert_scan_step(&scan, s, n, &i)) continue;
        char ch = s[i];
        if (ch == '(') { par++; i++; continue; }
        if (ch == '[') { brk++; i++; continue; }
        if (ch == '{') { br++; i++; continue; }
        if (ch == ')') { if (par) par--; i++; continue; }
        if (ch == ']') { if (brk) brk--; i++; continue; }
        if (ch == '}') {
            if (par || brk || br) { if (br) br--; i++; continue; }
            return i;
        }
        i++;
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
    CCInertScan scan;
    cc_inert_scan_init(&scan, NULL);
    scan.at_line_start = 0;  /* mid-buffer probe */
    size_t i = from;
    while (i < limit) {
        if (cc_inert_scan_step(&scan, s, limit, &i)) continue;
        char ch = s[i];
        if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n') { i++; continue; }
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

/* No-binder `!> { ... }` / `!> STMT` must not contain `@err(IDENT);` —
 * that forward needs `!>(e) { ... @err(e); }`. Diagnose before leaking
 * `@err` through to the host C parser. Returns -1 if a forward was found. */
static int cc__pu_reject_err_forward_without_binder(
    const CCVisitorCtx* ctx, const char* s, size_t n,
    size_t body_a, size_t body_b, int fallback_line) {
    CCInertScan scan;
    size_t i;
    char rel[1024];
    const char* f;
    if (!s || body_a >= body_b || body_b > n) return 0;
    f = cc_path_rel_to_repo(
        ctx && ctx->input_path ? ctx->input_path : "<input>", rel, sizeof(rel));
    cc_inert_scan_init(&scan, ctx ? ctx->input_path : NULL);
    scan.at_line_start = 0;
    i = body_a;
    while (i < body_b) {
        if (cc_inert_scan_step(&scan, s, n, &i)) continue;
        if (s[i] == '@' && i + 4 <= body_b && memcmp(s + i, "@err", 4) == 0 &&
            !(i + 11 <= body_b && memcmp(s + i, "@errhandler", 11) == 0) &&
            (i + 4 >= body_b || !cc_is_ident_char(s[i + 4]))) {
            size_t j = i + 4;
            size_t rpar = 0;
            size_t ia, ib, semi;
            int err_line = fallback_line;
            while (j < body_b && isspace((unsigned char)s[j])) j++;
            if (j >= body_b || s[j] != '(') {
                i++;
                continue;
            }
            if (!cc_find_matching_paren(s, n, j, &rpar) || rpar >= body_b) {
                i++;
                continue;
            }
            ia = j + 1;
            ib = rpar;
            while (ia < ib && isspace((unsigned char)s[ia])) ia++;
            while (ib > ia && isspace((unsigned char)s[ib - 1])) ib--;
            semi = rpar + 1;
            while (semi < body_b && isspace((unsigned char)s[semi])) semi++;
            if (!cc__range_is_ident(s, ia, ib) || semi >= body_b ||
                s[semi] != ';') {
                i++;
                continue;
            }
            cc__line_from_pos(s, i, &err_line);
            cc_pass_error_cat(f, err_line, 1, CC_ERR_SYNTAX,
                              "'@err(%.*s)' requires an error binder; use "
                              "'!>(%.*s) { ...; @err(%.*s); }'",
                              (int)(ib - ia), s + ia,
                              (int)(ib - ia), s + ia,
                              (int)(ib - ia), s + ia);
            cc_pass_note(f, err_line, 1,
                         "statements before '@err' must also consume Results "
                         "(e.g. println(...) !>;)");
            return -1;
        }
        i++;
    }
    return 0;
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
                                    const char* outer_decl,
                                    const char* outer_as_path,
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

    CCInertScan scan;
    cc_inert_scan_init(&scan, ctx ? ctx->input_path : NULL);
    scan.at_line_start = 0;  /* body is a mid-buffer slice */
    size_t i = 0;
    while (i < body_len) {
        size_t before = i;
        if (cc_inert_scan_step(&scan, body, body_len, &i)) {
            cc__append_n(&out, &ol, &oc, body + before, i - before);
            continue;
        }
        char ch = body[i];

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
                                          "'@err(%s)' requires an error binder; use "
                                          "'!>(%s) { ...; @err(%s); }'",
                                          idname, idname, idname);
                        cc_pass_note(relf, err_line, 1,
                                     "statements before '@err' must also consume Results "
                                     "(e.g. println(...) !>;)");
                    }
                    free(out);
                    return -1;
                }

                if (!outer_found || !outer_body || outer_body_len == 0) {
                    int err_line = op_line;
                    cc__line_from_pos(src, body_src_off + i, &err_line);
                    cc_pass_error_cat(relf, err_line, 1, CC_ERR_SYNTAX,
                                      "@err(%s) forward has no matching '@errhandler' in scope",
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
                 * the handler body keep resolving.  When the handler
                 * matched via `@as`, project the binder into the
                 * handler param type first (by-value member select). */
                size_t sub_len = 0;
                char proj_name[64];
                const char* subst_to = binder;
                proj_name[0] = 0;
                if (outer_as_path && outer_as_path[0] && outer_decl &&
                    outer_decl[0]) {
                    char pty[128], pname[64];
                    static int g_as_proj_id = 0;
                    if (cc_errhandler_split_param_decl(outer_decl, pty, sizeof(pty),
                                                       pname, sizeof(pname)) &&
                        pty[0]) {
                        snprintf(proj_name, sizeof(proj_name),
                                 "__cc_eh_as_%d", ++g_as_proj_id);
                        subst_to = proj_name;
                    }
                }
                char* subst = cc__pu_subst_word(outer_body, outer_body_len,
                                                (outer_param && outer_param[0]) ? outer_param : "",
                                                subst_to, &sub_len);
                if (!subst) {
                    free(out);
                    return -1;
                }
                cc__append_str(&out, &ol, &oc, "{ ");
                if (proj_name[0]) {
                    char pty[128], pname[64];
                    cc_errhandler_split_param_decl(outer_decl, pty, sizeof(pty),
                                                   pname, sizeof(pname));
                    cc_sb_append_fmt(&out, &ol, &oc, "%s %s = (%s).%s; ",
                                     pty, proj_name, binder, outer_as_path);
                }
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
    /* Type-matched in-scope @errhandler for `@err(binder);` forwards. */
    char outer_decl[256];
    size_t outer_decl_len = 0;
    const char* outer_body = NULL;
    size_t outer_body_len = 0;
    size_t outer_decl_pos = 0;
    int outer_found = 0;
    char outer_param[128];
    char outer_err_type[128];
    char outer_as_path[CC_ERRHANDLER_AS_PATH_MAX];
    int outer_have_handlers = 0;
    int outer_as_diag = CC_ERRHANDLER_AS_NONE;
    int outer_ambient = 0;
    outer_param[0] = 0;
    outer_err_type[0] = 0;
    outer_as_path[0] = 0;
    if (cc__pu_find_outer_errhandler(s, n, call_a, call_a, call_b,
                                     outer_decl, sizeof(outer_decl), &outer_decl_len,
                                     &outer_body, &outer_body_len,
                                     &outer_decl_pos,
                                     outer_err_type, sizeof(outer_err_type),
                                     outer_as_path, sizeof(outer_as_path),
                                     &outer_have_handlers,
                                     &outer_as_diag, &outer_ambient)) {
        (void)outer_ambient;
        cc__pu_extract_param_name(outer_decl, outer_decl_len,
                                  outer_param, sizeof(outer_param));
        outer_found = 1;
    } else if (outer_as_diag == CC_ERRHANDLER_AS_AMBIG ||
               outer_as_diag == CC_ERRHANDLER_AS_CYCLE) {
        char rel[1024];
        const char* ff = cc_path_rel_to_repo(
            ctx && ctx->input_path ? ctx->input_path : "<input>", rel, sizeof(rel));
        cc__pu_errhandler_miss_diag(ff, op_line, outer_err_type, outer_have_handlers,
                                    outer_as_diag,
                                    "'!>' requires an enclosing '@errhandler' in scope");
        return -1;
    }

    char* processed = NULL;
    size_t processed_len = 0;
    if (cc__pu_process_bang_body(ctx, s, n, call_a, op_line,
                                 body, body_len, binder,
                                 outer_body, outer_body_len,
                                 outer_param,
                                 outer_decl, outer_as_path,
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
    char result_type[256];
    result_type[0] = 0;
    if (cc__ru_find_callee_result_type(s, n, call_a, call_b,
                                       result_type, sizeof(result_type))) {
        cc_sb_append_fmt(&out, &ol, &oc, "{ %s %s = (", result_type, tmpv);
    } else {
        cc__append_str(&out, &ol, &oc, "{ __typeof__(");
        cc__append_n(&out, &ol, &oc, s + call_a, call_b - call_a);
        cc_sb_append_fmt(&out, &ol, &oc, ") %s = (", tmpv);
    }
    cc__append_n(&out, &ol, &oc, s + call_a, call_b - call_a);
    cc__append_str(&out, &ol, &oc, ");\n    if (");
    cc__ru_emit_is_err(&out, &ol, &oc, result_type, tmpv);
    cc__append_str(&out, &ol, &oc, ") {\n        ");
    cc__ru_emit_uw_err_binder(&out, &ol, &oc, s, n, call_a, call_b, tmpv, mangled_binder, f, op_line);
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
        if (c != '\n' && cc_scan_pos_in_line_comment(s, i)) {
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
        /* `@IDENT` decl-specs / statement-tokens (`@noblock`, `@async`,
         * `@errhandler(...)`, `@destroy`, `@err(...)`, ...) are transparent
         * to the LHS walk: they sit in front of a return type in a
         * function signature (`@noblock static Reply !>(CCError) fn(...)`)
         * or just before a statement, so we keep walking back through the
         * `@` to find the real statement boundary.  Without this, the `@`
         * was treated as an expression-position boundary, is_stmt_pos
         * flipped to 0, decl-form detection was skipped, and the scanner
         * ran the expression-position `!>` rewriter on a function signature
         * — tripping "expected ';' terminating '!>' body" from the
         * forward-scan walking past EOF (see redis_idiomatic
         * `@noblock static RedisReply !>(CCError) execute_request`). */
        if (c == '@') {
            size_t j = i + 1;
            if (j < op_at && cc_is_ident_start(s[j])) continue;
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

    size_t scan = cc_skip_ws_and_comments(s, n, op_at + 2);
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
        size_t ba = cc_skip_ws_and_comments(s, rpar, scan + 1);
        size_t bb = cc_rskip_ws_and_comments(s, rpar);
        if (bb < ba) bb = ba;
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
        scan = cc_skip_ws_and_comments(s, n, rpar + 1);
    }

    if (scan >= n) {
        cc_pass_error_cat(f, line_no, 1, CC_ERR_SYNTAX,
                          has_binder
                              ? "expected body after '!> (e)'"
                              : "expected body after '!>'");
        return -1;
    }
    if ((scan + 8 <= n && memcmp(s + scan, "@destroy", 8) == 0 &&
         (scan + 8 == n || !cc_is_ident_char(s[scan + 8]))) ||
        (scan + 6 <= n && memcmp(s + scan, "@defer", 6) == 0 &&
         (scan + 6 == n || !cc_is_ident_char(s[scan + 6])))) {
        return 0;
    }

    /* Type-matched in-scope @errhandler (bare `!>;` and `@err` forwards). */
    char outer_decl[256];
    size_t outer_decl_len = 0;
    const char* outer_body = NULL;
    size_t outer_body_len = 0;
    size_t outer_decl_pos = 0;
    char outer_param[128];
    char outer_err_type[128];
    char outer_as_path[CC_ERRHANDLER_AS_PATH_MAX];
    int outer_have_handlers = 0;
    int outer_as_diag = CC_ERRHANDLER_AS_NONE;
    int outer_ambient = 0;
    outer_param[0] = 0;
    outer_err_type[0] = 0;
    outer_as_path[0] = 0;
    int outer_found = cc__pu_find_outer_errhandler(
        s, n, call_a, call_a, call_b,
        outer_decl, sizeof(outer_decl), &outer_decl_len,
        &outer_body, &outer_body_len, &outer_decl_pos,
        outer_err_type, sizeof(outer_err_type),
        outer_as_path, sizeof(outer_as_path),
        &outer_have_handlers, &outer_as_diag, &outer_ambient);
    (void)outer_ambient;
    if (outer_found) {
        cc__pu_extract_param_name(outer_decl, outer_decl_len,
                                  outer_param, sizeof(outer_param));
    }

    /* Form P: bare `!>` at expression position.  Synthesize a binder, inline
     * the outer handler body.  Same-E re-entry is ill-formed.  The lowering is
     * a self-contained `({ ... })` that leaves the terminator in place, so it
     * composes wherever the terminator says the expression ended — a `;` after
     * a declaration, but equally a `)`, a `,` or an infix operator. */
    if (cc__pu_bare_terminator(s, n, scan) && !has_binder) {
        if (!outer_found) {
            cc__pu_errhandler_miss_diag(
                f, line_no, outer_err_type, outer_have_handlers, outer_as_diag,
                "'!>;' at expression position requires an enclosing '@errhandler' in scope");
            return -1;
        }
        if (cc__pu_bare_bang_reenters_handler(s, call_a, outer_body,
                                              outer_body_len)) {
            cc__pu_diag_same_e_reenter(f, line_no);
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
        char result_type[256];
        snprintf(tmpv, sizeof(tmpv), "__cc_pu_e_%d", tid);
        result_type[0] = 0;
        (void)cc__ru_find_callee_result_type(s, n, call_a, call_b,
                                             result_type, sizeof(result_type));

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
        if (result_type[0]) {
            cc_sb_append_fmt(&out, &ol, &oc, "({ %s %s = (", result_type, tmpv);
        } else {
            cc__append_str(&out, &ol, &oc, "({ __typeof__(");
            cc__append_n(&out, &ol, &oc, s + call_a, call_b - call_a);
            cc_sb_append_fmt(&out, &ol, &oc, ") %s = (", tmpv);
        }
        cc__append_n(&out, &ol, &oc, s + call_a, call_b - call_a);
        cc__append_str(&out, &ol, &oc, ");\n    if (");
        cc__ru_emit_is_err(&out, &ol, &oc, result_type, tmpv);
        cc__append_str(&out, &ol, &oc, ") {\n        ");
        cc__ru_emit_handler_err_binder(&out, &ol, &oc, s, n, call_a, call_b,
                                       tmpv, binder, outer_decl, outer_as_path,
                                       ff, line_no);
        cc__append_n(&out, &ol, &oc, substituted, sub_len);
        cc__append_str(&out, &ol, &oc, " } ");
        cc__ru_emit_value(&out, &ol, &oc, result_type, tmpv);
        cc__append_str(&out, &ol, &oc, "; })");
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

    /* An unlowered chain link: `!>` directly followed by `.method(`
     * means the fallible-chain pass could not type this hop's producer.
     * Say that — the generic divergence error misleads here. */
    if (s[scan] == '.') {
        cc_pass_error_cat(f, line_no, 1, CC_ERR_TYPE,
                          "'!>' links a chain hop here, but the hop's producer "
                          "could not be typed as a Result; declare the "
                          "producer's Result return where the chain can see "
                          "it, or bind this hop to a typed Result and chain "
                          "from the binding");
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
        {
            size_t after_body = cc_skip_ws_and_comments(s, n, rbrace + 1);
            if (after_body < n && s[after_body] == '.') {
                cc_pass_error_cat(f, line_no, 1, CC_ERR_TYPE,
                                  "'!>' links a chain hop here, but the hop's "
                                  "producer could not be typed as a Result; "
                                  "declare the producer's Result return where "
                                  "the chain can see it, or bind this hop to "
                                  "a typed Result and chain from the binding");
                return -1;
            }
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
                                 outer_decl, outer_as_path,
                                 outer_found, outer_decl_pos,
                                 &processed, &processed_len) < 0) {
        return -1;
    }

    static int g_expr_tmp_id2 = 0;
    int tid = ++g_expr_tmp_id2;
    char tmpv[48];
    char result_type[256];
    snprintf(tmpv, sizeof(tmpv), "__cc_pu_x_%d", tid);
    result_type[0] = 0;

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
    if (cc__ru_find_callee_result_type(s, n, call_a, call_b,
                                       result_type, sizeof(result_type))) {
        cc_sb_append_fmt(&out, &ol, &oc, "({ %s %s = (", result_type, tmpv);
    } else {
        cc__append_str(&out, &ol, &oc, "({ __typeof__(");
        cc__append_n(&out, &ol, &oc, s + call_a, call_b - call_a);
        cc_sb_append_fmt(&out, &ol, &oc, ") %s = (", tmpv);
    }
    cc__append_n(&out, &ol, &oc, s + call_a, call_b - call_a);
    cc__append_str(&out, &ol, &oc, ");\n    if (");
    cc__ru_emit_is_err(&out, &ol, &oc, result_type, tmpv);
    cc__append_str(&out, &ol, &oc, ") {\n        ");
    if (has_binder) {
        cc__ru_emit_uw_err_binder(&out, &ol, &oc, s, n, call_a, call_b, tmpv, mangled_binder, f, line_no);
    }
    cc__append_n(&out, &ol, &oc, processed, processed_len);
    if (!is_block) {
        /* Single-statement form: include trailing `;` if the stmt was
         * emitted without one (body_b already included the `;`, so
         * processed already ends with `;` — skip). */
        (void)0;
    }
    cc__append_str(&out, &ol, &oc, " } ");
    cc__ru_emit_value(&out, &ol, &oc, result_type, tmpv);
    cc__append_str(&out, &ol, &oc, "; })");
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
                                 size_t* out_len,
                                 int skip_statement_bang) {
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
        scan = cc_skip_ws_and_comments(s, n, after);
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
         * `return` don't hide the keyword and drag it into the operand.
         *
         * Also run when the LHS walk already classified the site as
         * expression-position (e.g. nested parens): `return foo() !>;`
         * must still strip the keyword or resolve sees `return foo(...)`
         * and falls back to ambient CCError. */
        {
            size_t a = cc_skip_ws_and_comments(s, op_at, call_start);
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
        if (skip_statement_bang && is_stmt_pos) {
            search_from = op_at + 2;
            continue;
        }
        {
            size_t after_op = cc_skip_ws_and_comments(s, n, op_at + 2);
            if (after_op + 8 <= n && memcmp(s + after_op, "@destroy", 8) == 0 &&
                (after_op + 8 == n || !cc_is_ident_char(s[after_op + 8]))) {
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
        size_t ba = cc_skip_ws_and_comments(s, rpar, scan + 1);
        size_t bb = cc_rskip_ws_and_comments(s, rpar);
        if (bb < ba) bb = ba;
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

        size_t after_bind = cc_skip_ws_and_comments(s, n, rpar + 1);
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
            size_t tail = cc_skip_ws_and_comments(s, n, splice_to);
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
        /* Form A: `CALL !>;` — plain dispatch to the default handler.
         * Same-E re-entry inside that handler is ill-formed. */
        size_t call_a = call_start, call_b = op_at;
        char outer_decl[256];
        size_t outer_decl_len = 0;
        const char* outer_body = NULL;
        size_t outer_body_len = 0;
        size_t outer_decl_pos = 0;
        char outer_err_type[128];
        char outer_as_path[CC_ERRHANDLER_AS_PATH_MAX];
        int outer_have_handlers = 0;
        int outer_as_diag = CC_ERRHANDLER_AS_NONE;
        int outer_ambient = 0;
        cc__trim_range(s, &call_a, &call_b);
        outer_err_type[0] = 0;
        outer_as_path[0] = 0;
        if (call_b > call_a &&
            cc__pu_find_outer_errhandler(
                s, n, call_a, call_a, call_b,
                outer_decl, sizeof(outer_decl), &outer_decl_len,
                &outer_body, &outer_body_len, &outer_decl_pos,
                outer_err_type, sizeof(outer_err_type),
                outer_as_path, sizeof(outer_as_path),
                &outer_have_handlers, &outer_as_diag, &outer_ambient) &&
            cc__pu_bare_bang_reenters_handler(s, call_a, outer_body,
                                              outer_body_len)) {
            char rel[1024];
            const char* f = cc_path_rel_to_repo(
                ctx && ctx->input_path ? ctx->input_path : "<input>", rel, sizeof(rel));
            (void)outer_decl_pos;
            (void)outer_ambient;
            cc__pu_diag_same_e_reenter(f, line_no);
            return -1;
        }
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
        if (cc__pu_reject_err_forward_without_binder(
                ctx, s, n, scan + 1, rbrace, line_no) < 0)
            return -1;
        /* Form C: `CALL !> { BLOCK }` or `CALL !> { BLOCK };`.  The legacy
         * `@err { ... };` shorthand requires a trailing `;`, so synthesize
         * one when the input omitted it. */
        size_t post = cc_skip_ws_and_comments(s, n, rbrace + 1);
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
        if (cc__pu_reject_err_forward_without_binder(
                ctx, s, n, scan, semi, line_no) < 0)
            return -1;
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
 * On by default; opt out with `CC_STRICT_RESULT_UNWRAP=0`.  Runs as a final pass
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
    return !(e && e[0] == '0' && e[1] == 0);
}

/* Walk backwards from `from` (exclusive) skipping whitespace and
 * comments (`(void) / *x* / foo();` and `return / *x* / foo()` classify
 * by their real preceding token).  Returns the index of the non-ws
 * non-comment char (or SIZE_MAX sentinel if we fell off the start). */
static size_t cc__back_skip_ws(const char* s, size_t from) {
    size_t i = cc_rskip_ws_and_comments(s, from);
    return i > 0 ? i - 1 : (size_t)-1;
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

static int cc__strict_unhandled_scan(const CCVisitorCtx* ctx,
                                     const char* s, size_t n) {
    if (!cc__strict_enabled()) return 0;
    char rel[1024];
    const char* f = cc_path_rel_to_repo(
        ctx && ctx->input_path ? ctx->input_path : "<input>", rel, sizeof(rel));

    int any_err = 0;
    CCInertScan scan;
    cc_inert_scan_init(&scan, ctx ? ctx->input_path : NULL);
    size_t i = 0;
    while (i < n) {
        if (cc_inert_scan_step(&scan, s, n, &i)) continue;

        /* M1 Phase 4 step (b): filter header-origin tokens.  No-op today
         * (find-only scanner; raw .ccs has no `#line` directives so
         * in_user_file is always 1).  Activates after the buffer swap and
         * prevents `any_err` from being raised on result-fn calls that
         * live entirely inside inlined CC runtime headers. */
        if (!scan.in_user_file) { i++; continue; }

        char c = s[i];

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
        size_t after_id = cc_skip_ws_and_comments(s, n, id_b);
        if (after_id >= n || s[after_id] != '(') continue;

        size_t rpar = 0;
        if (!cc_find_matching_paren(s, n, after_id, &rpar)) continue;

        /* Gate (b): char after balanced ')' must be ';'. */
        size_t post = cc_skip_ws_and_comments(s, n, rpar + 1);
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

        /* Gate (e): statement-expression tail.  `... NAME(...); })` makes
         * the call the value of the enclosing `({ ... })`; the consumer
         * of the statement-expression owns the result. */
        {
            size_t tail = cc_skip_ws_and_comments(s, n, post + 1);
            if (tail < n && s[tail] == '}') {
                size_t after_rb = cc_skip_ws_and_comments(s, n, tail + 1);
                if (after_rb < n && s[after_rb] == ')') continue;
            }
        }

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

/* Find the offset of the `!>` / `?>` sigil within `raw_text` relative
 * to the node's span start.
 *
 * This was a plain `strstr`, on the guarantee that the IR carver had already
 * ensured a node's only sigil was real code.  That guarantee does not survive
 * template lowering: a `@string` template whose PROSE contains `!>` becomes a
 * C string literal inside the node's span, `strstr` finds the sigil in there,
 * and the rewrite splices `@err` into the middle of the user's text.  In the
 * script register nothing lowers the template earlier, which is why only that
 * register showed it.  Returns SIZE_MAX on not-found. */
static size_t cc__ir_node_sigil_offset(const CCIrNode* n) {
    CCInertScan scan;
    size_t i = 0, len;
    char lead;
    if (!n || !n->raw_text) return (size_t)-1;
    lead = (n->kind == CC_IR_UNWRAP_BANG) ? '!' : '?';
    len = n->raw_len ? n->raw_len : strlen(n->raw_text);
    cc_inert_scan_init(&scan, NULL);
    while (i < len) {
        if (cc_inert_scan_step(&scan, n->raw_text, len, &i)) continue;
        if (n->raw_text[i] == lead && i + 1 < len && n->raw_text[i + 1] == '>')
            return i;
        i++;
    }
    return (size_t)-1;
}

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
    size_t sig_off = cc__ir_node_sigil_offset(n);
    if (sig_off == (size_t)-1) return 0;
    if (sig_off + 2 >= n->raw_len) return 0;
    return 1;
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

        /* No-binder UNWRAP_BANG: substitute `!>` -> `@err`. Form A same-E
         * re-entry is diagnosed (ill-formed). `@err(IDENT);` inside a
         * no-binder body needs `!>(e)` — diagnose here (IR path skips
         * legacy Form B/C checks). */
        size_t sig_off = cc__ir_node_sigil_offset(c);
        if (sig_off == (size_t)-1 || sig_off + 2 > c->raw_len) {
            /* Defensive: the recogniser should never produce this,
             * but if it did, fall back to literal emission so the
             * legacy loop still rewrites it correctly. */
            cc__append_n(&buf, &bl, &bc, c->raw_text, c->raw_len);
            continue;
        }
        {
            int bang_line = 1;
            size_t file_body_a = c->span.byte_start + sig_off + 2;
            size_t file_body_b = c->span.byte_start + c->raw_len;
            if (file_body_b > in_len) file_body_b = in_len;
            cc__line_from_pos(in, c->span.byte_start + sig_off, &bang_line);
            if (cc__pu_reject_err_forward_without_binder(
                    ctx, in, in_len, file_body_a, file_body_b, bang_line) < 0) {
                free(buf);
                rc = -1;
                goto out;
            }
        }
        {
            size_t after_sig = cc_skip_ws_and_comments(c->raw_text, c->raw_len, sig_off + 2);
            int form_a = (after_sig < c->raw_len && c->raw_text[after_sig] == ';');
            if (form_a) {
                size_t file_call_a = c->span.byte_start;
                size_t file_call_b = c->span.byte_start + sig_off;
                char outer_decl[256];
                size_t outer_decl_len = 0;
                const char* outer_body = NULL;
                size_t outer_body_len = 0;
                size_t outer_decl_pos = 0;
                char outer_err_type[128];
                char outer_as_path[CC_ERRHANDLER_AS_PATH_MAX];
                int outer_have_handlers = 0;
                int outer_as_diag = CC_ERRHANDLER_AS_NONE;
                int outer_ambient = 0;
                int call_line = 1;
                outer_err_type[0] = 0;
                outer_as_path[0] = 0;
                cc__trim_range(in, &file_call_a, &file_call_b);
                if (file_call_b > file_call_a &&
                    cc__pu_find_outer_errhandler(
                        in, in_len, file_call_a, file_call_a, file_call_b,
                        outer_decl, sizeof(outer_decl), &outer_decl_len,
                        &outer_body, &outer_body_len, &outer_decl_pos,
                        outer_err_type, sizeof(outer_err_type),
                        outer_as_path, sizeof(outer_as_path),
                        &outer_have_handlers, &outer_as_diag,
                        &outer_ambient) &&
                    cc__pu_bare_bang_reenters_handler(in, file_call_a,
                                                      outer_body,
                                                      outer_body_len)) {
                    char rel[1024];
                    const char* f = cc_path_rel_to_repo(
                        ctx && ctx->input_path ? ctx->input_path : "<input>",
                        rel, sizeof(rel));
                    (void)outer_decl_pos;
                    (void)outer_ambient;
                    cc__line_from_pos(in, file_call_a, &call_line);
                    cc__pu_diag_same_e_reenter(f, call_line);
                    free(buf);
                    return -1;
                }
            }
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

int cc__rewrite_result_unwrap_with_options(const CCVisitorCtx* ctx,
                                           const char* in_src,
                                           size_t in_len,
                                           char** out_src,
                                           size_t* out_len,
                                           int skip_statement_bang) {
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
            /* Ledger resync for a non-line-neutral preprocess (no-op when
             * neutral; see the fixed-point loops below). */
            size_t sl = 0;
            char* stamped = cc_ledger_stamp_rewrite(
                cur, curlen, ir_pp, ir_pp_len,
                ctx && ctx->input_path ? ctx->input_path : "<cc_input>", &sl);
            if (stamped) {
                free(ir_pp);
                ir_pp = stamped;
                ir_pp_len = sl;
            }
            free(cur);
            cur    = ir_pp;
            curlen = ir_pp_len;
            any    = 1;
        }
    }

    if (need_rewrite) {
        const char* stamp_path = ctx && ctx->input_path ? ctx->input_path : "<cc_input>";
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
            /* Ledger resync: a multi-line lowering (handler bodies keep the
             * user's newlines) shifts every user coordinate stamped below
             * it by later passes (defer prologues, closure #line anchors,
             * async machine markers).  cc_ledger_stamp_rewrite is a no-op
             * for line-neutral rewrites. */
            {
                size_t sl = 0;
                char* stamped = cc_ledger_stamp_rewrite(cur, curlen, next, nl, stamp_path, &sl);
                if (stamped) {
                    free(next);
                    next = stamped;
                    nl = sl;
                }
            }
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
            int r = cc__rewrite_bang_once(ctx, cur, curlen, &next, &nl,
                                          skip_statement_bang);
            if (r < 0) {
                free(cur);
                return -1;
            }
            if (r == 0) break;
            /* Ledger resync (see the `?>` loop above). */
            {
                size_t sl = 0;
                char* stamped = cc_ledger_stamp_rewrite(cur, curlen, next, nl, stamp_path, &sl);
                if (stamped) {
                    free(next);
                    next = stamped;
                    nl = sl;
                }
            }
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

int cc__rewrite_result_unwrap(const CCVisitorCtx* ctx,
                              const char* in_src,
                              size_t in_len,
                              char** out_src,
                              size_t* out_len) {
    return cc__rewrite_result_unwrap_with_options(ctx, in_src, in_len,
                                                  out_src, out_len,
                                                  0);
}
