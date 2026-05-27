#include "cc_l2_rewriter.h"

#include "../util/text_scan.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

/* ----------------------------------------------------------------
 * Small text-builder utilities (local; we deliberately don't pull
 * in cc_sb_append* here so this module stays standalone and easy
 * to lift if a future "L2 prelude pipeline" wants to own it).
 * ---------------------------------------------------------------- */

static int cc__l2_reserve(char** buf, size_t* len, size_t* cap, size_t extra) {
    if (*len + extra + 1 <= *cap) return 0;
    size_t nc = *cap ? *cap : 256;
    while (nc < *len + extra + 1) nc *= 2;
    char* nb = (char*)realloc(*buf, nc);
    if (!nb) return -1;
    *buf = nb;
    *cap = nc;
    return 0;
}

static int cc__l2_append(char** buf, size_t* len, size_t* cap,
                         const char* s, size_t n) {
    if (cc__l2_reserve(buf, len, cap, n)) return -1;
    memcpy(*buf + *len, s, n);
    *len += n;
    (*buf)[*len] = 0;
    return 0;
}

static int cc__l2_append_cstr(char** buf, size_t* len, size_t* cap, const char* s) {
    return cc__l2_append(buf, len, cap, s, strlen(s));
}

/* ----------------------------------------------------------------
 * Identifier helpers
 * ---------------------------------------------------------------- */

static int cc__l2_is_ident_start(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

static int cc__l2_is_ident_char(char c) {
    return cc__l2_is_ident_start(c) || (c >= '0' && c <= '9');
}

/* Does `src[i..]` begin with the keyword `kw` (length `kwn`), with
 * a word boundary on both sides? */
static int cc__l2_match_word(const char* src, size_t n, size_t i,
                             const char* kw, size_t kwn) {
    if (i + kwn > n) return 0;
    if (memcmp(src + i, kw, kwn) != 0) return 0;
    if (i + kwn < n && cc__l2_is_ident_char(src[i + kwn])) return 0;
    if (i > 0 && cc__l2_is_ident_char(src[i - 1])) return 0;
    return 1;
}

/* Skip ASCII whitespace forwards.  Does NOT skip comments — the
 * caller's CCInertScan handles those before reaching token
 * recognition. */
static size_t cc__l2_skip_ws(const char* src, size_t n, size_t i) {
    while (i < n && (src[i] == ' ' || src[i] == '\t' ||
                     src[i] == '\r' || src[i] == '\n')) i++;
    return i;
}

/* ----------------------------------------------------------------
 * Idiom 1: `offsetof(T, F)` -> `__builtin_offsetof(T, F)`
 *
 * Match: the literal token `offsetof` followed (across optional
 * whitespace) by `(`.  Rewrite to `__builtin_offsetof` and emit
 * the rest of the call verbatim — TCC's expression parser
 * recognizes the builtin and handles the `(T,F)` payload itself.
 *
 * We DON'T need to find the matching `)` because the rewrite is
 * just a prefix replacement; the rest of the call text passes
 * through unchanged.
 *
 * Edge cases we deliberately accept:
 *   - User defined a non-standard `offsetof` macro shadowing
 *     `<stddef.h>`: we rewrite anyway.  The user-facing surface
 *     of standard C is what we contract to.
 *   - User has a struct member or local named `offsetof`: the
 *     word-boundary + lookahead-for-`(` check is enough — bare
 *     `x.offsetof` or `offsetof_thing` won't match.
 * ---------------------------------------------------------------- */

static int cc__l2_try_rewrite_offsetof(const char* src, size_t n, size_t* pi,
                                       char** out, size_t* ol, size_t* oc) {
    size_t i = *pi;
    if (!cc__l2_match_word(src, n, i, "offsetof", 8)) return 0;
    size_t k = cc__l2_skip_ws(src, n, i + 8);
    if (k >= n || src[k] != '(') return 0;
    /* Match.  Emit replacement; advance `*pi` past `offsetof` only —
     * the trailing whitespace + `(` will be copied verbatim by the
     * main loop. */
    if (cc__l2_append_cstr(out, ol, oc, "__builtin_offsetof")) return -1;
    *pi = i + 8;
    return 1;
}

/* ----------------------------------------------------------------
 * Idiom 2: `__attribute__((constructor(N)))` / `(destructor(N))`
 *           -> `__attribute__((constructor))`     / `(destructor)`
 *
 * Match: `__attribute__` token followed by `((` followed by
 * `constructor` or `destructor` followed by `(` (the priority
 * open-paren).  Scan forward to the matching `)`, then expect
 * `))` to close the attribute.  Replace the `(N)` priority span
 * with whitespace padding so byte offsets after the rewrite
 * stay close to the original.
 *
 * If anything looks off (no double parens, the inner keyword
 * isn't constructor/destructor, etc.) we bail and leave the text
 * alone so user code with other `__attribute__((...))` forms
 * (e.g. `aligned`, `noreturn`, `warn_unused_result`) passes
 * through unchanged.
 *
 * On a successful rewrite we consume the entire `__attribute__`
 * token from the input and write the cleaned-up form to `out`;
 * `*pi` is advanced to just past the closing `))`.
 * ---------------------------------------------------------------- */

static int cc__l2_try_rewrite_ctor_priority(const char* src, size_t n, size_t* pi,
                                            char** out, size_t* ol, size_t* oc) {
    size_t i = *pi;
    if (!cc__l2_match_word(src, n, i, "__attribute__", 13)) return 0;
    size_t k = cc__l2_skip_ws(src, n, i + 13);
    /* Need `((` */
    if (k + 1 >= n || src[k] != '(' || src[k + 1] != '(') return 0;
    size_t inner = cc__l2_skip_ws(src, n, k + 2);

    /* Inner keyword: constructor or destructor */
    const char* kw = NULL;
    size_t kwn = 0;
    if (cc__l2_match_word(src, n, inner, "constructor", 11)) { kw = "constructor"; kwn = 11; }
    else if (cc__l2_match_word(src, n, inner, "destructor", 10)) { kw = "destructor"; kwn = 10; }
    else return 0;

    size_t after_kw = cc__l2_skip_ws(src, n, inner + kwn);
    if (after_kw >= n || src[after_kw] != '(') return 0;

    /* Walk to matching `)` of the priority arg.  Bail on
     * anything unbalanced or unexpected (we're conservative —
     * if the spelling differs from what we recognize, leave it
     * alone). */
    size_t pp = after_kw + 1;
    int depth = 1;
    while (pp < n && depth > 0) {
        char c = src[pp];
        if (c == '(') depth++;
        else if (c == ')') depth--;
        else if (c == '\n') return 0;  /* don't span lines */
        if (depth == 0) break;
        pp++;
    }
    if (pp >= n || src[pp] != ')') return 0;
    size_t prio_close = pp;            /* position of `)` closing `(N)` */
    size_t after_prio = cc__l2_skip_ws(src, n, prio_close + 1);
    if (after_prio + 1 >= n || src[after_prio] != ')' || src[after_prio + 1] != ')') return 0;

    /* Match.  Emit `__attribute__((<kw>))`.  Anything between
     * `((` and `<kw>` (whitespace) we drop; ditto between `<kw>`
     * and the priority's `(`, and from `(N)`'s `)` to the
     * closing `))`.  This is intentional — the replacement is
     * the canonical TCC-friendly form; column drift inside the
     * one rewritten line is acceptable per the L2 contract. */
    if (cc__l2_append_cstr(out, ol, oc, "__attribute__((")) return -1;
    if (cc__l2_append(out, ol, oc, kw, kwn)) return -1;
    if (cc__l2_append_cstr(out, ol, oc, "))")) return -1;
    *pi = after_prio + 2;
    return 1;
}

/* ----------------------------------------------------------------
 * Driver: scan with CCInertScan, try each idiom at every
 * "real code" byte, fall through to verbatim copy otherwise.
 * ---------------------------------------------------------------- */

char* cc_l2_rewrite_all(const char* src, size_t src_len, size_t* out_len) {
    if (out_len) *out_len = src_len;
    if (!src) return NULL;

    char* out = NULL;
    size_t ol = 0, oc = 0;
    int any = 0;

    CCInertScan s;
    cc_inert_scan_init(&s, NULL);  /* user_file unused — we rewrite everywhere */

    size_t i = 0;
    while (i < src_len) {
        size_t before = i;
        if (cc_inert_scan_step(&s, src, src_len, &i)) {
            /* Inert region: copy verbatim. */
            if (cc__l2_append(&out, &ol, &oc, src + before, i - before)) goto oom;
            continue;
        }
        /* Real code at src[i].  Cheap first-char filters keep
         * the common path fast — only `o` and `_` can begin our
         * matched tokens.  */
        char c = src[i];
        int rc;
        if (c == 'o') {
            rc = cc__l2_try_rewrite_offsetof(src, src_len, &i, &out, &ol, &oc);
            if (rc < 0) goto oom;
            if (rc) { any = 1; continue; }
        } else if (c == '_') {
            rc = cc__l2_try_rewrite_ctor_priority(src, src_len, &i, &out, &ol, &oc);
            if (rc < 0) goto oom;
            if (rc) { any = 1; continue; }
        }
        /* No match: copy this byte and advance. */
        if (cc__l2_append(&out, &ol, &oc, src + i, 1)) goto oom;
        i++;
    }

    if (!any) {
        free(out);
        return NULL;  /* caller continues with original buffer */
    }
    if (out_len) *out_len = ol;
    return out;

oom:
    free(out);
    return NULL;
}
