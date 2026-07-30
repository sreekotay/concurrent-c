/*
 * text_scan.h - shared lexical scanner state for CC visitor passes.
 *
 * The visitor passes in `cc/src/visitor/pass_*.c` all walk their input
 * buffer one byte at a time looking for CC-specific token patterns
 * (`=>`, `[~`, `@async`, `!>`, `@defer`, `o->m(...)`, etc).  Each pass
 * has historically maintained its own copy of the same lexer state
 * machine — comment/string/char-literal/preprocessor-directive
 * skipping — which is both noisy and bug-prone (see the closure
 * scanner bug fixed in commit 43e0ebc, the three `in_pp` fixes in
 * commit 842dd8c, etc.).
 *
 * This header centralizes that state.  It is the visitor-side
 * analogue of `CCScannerState` in `cc/src/preprocess/preprocess.c`
 * (kept separate because the preprocess version is static-internal
 * and used heavily by phase-1; promoting it would risk regressions
 * across 16 call sites).
 *
 * Beyond the comment/string/pp tracking that `CCScannerState`
 * already does, `CCInertScan` adds one extra capability that's
 * critical for the M1 visitor refactor: it parses `#line N "file"`
 * directives as they're seen and exposes an `in_user_file` flag.
 * When the visitor's working buffer is the pre-expanded source
 * (i.e. with inlined CC runtime headers — `<ccc/cc_channel.cch>`,
 * `<ccc/std/vec.cch>`, etc.), text scanners can then filter out
 * matches that originated in those headers and only act on tokens
 * that came from the user TU.  See
 * `cc/docs/COMPILER_CLEANUP_STATUS.md` "M1 visitor refactor / (c)".
 *
 * Usage pattern (rewrite loop):
 *
 *     CCInertScan s;
 *     cc_inert_scan_init(&s, ctx->input_path);
 *     for (size_t i = 0; i < n; ) {
 *         size_t before = i;
 *         if (cc_inert_scan_step(&s, src, n, &i)) {
 *             // inert content (comment/string/pp/etc.) — copy verbatim
 *             out_append(src + before, i - before);
 *             continue;
 *         }
 *         // src[i] is real code
 *         if (!s.in_user_file) {
 *             out_append(src + i, 1);
 *             i++;
 *             continue;
 *         }
 *         // try to match CC tokens at src[i] ...
 *         out_append(src + i, 1);
 *         i++;
 *     }
 *
 * Usage pattern (find-only scan, no output):
 *
 *     CCInertScan s;
 *     cc_inert_scan_init(&s, ctx->input_path);
 *     for (size_t i = 0; i < n; ) {
 *         if (cc_inert_scan_step(&s, src, n, &i)) continue;
 *         if (!s.in_user_file) { i++; continue; }
 *         if (src[i] == '@' && cc_match_tok(src, n, i, "@defer")) {
 *             ...
 *         }
 *         i++;
 *     }
 *
 * The functions are header-only `static inline` so each TU gets its
 * own copy (zero link-time coupling) and the compiler can inline
 * the per-byte step into the caller's hot loop.
 */
#ifndef CC_UTIL_TEXT_SCAN_H
#define CC_UTIL_TEXT_SCAN_H

#include <stddef.h>
#include <string.h>

/* Inert-region lexer state.  See header banner for usage. */
typedef struct {
    /* Lexer state */
    int  in_line_comment;
    int  in_block_comment;
    int  in_str;
    int  in_chr;
    char qch;            /* quote char for the active string literal */
    int  in_pp;          /* inside a preprocessor directive body */
    int  pp_continued;   /* prev non-WS char was '\' (line continuation) */
    int  at_line_start;  /* prev non-WS char was '\n' (or BOF) */

    /* #line tracking.  `user_file` is the path passed to
     * `cc_inert_scan_init`; matches are by basename (so `path/foo.ccs`
     * and `foo.ccs` both count as the user TU).  `current_file` holds
     * the filename from the most recent `#line N "..."` directive
     * encountered; before any directive, `in_user_file` is 1
     * (assumption: scanning starts in the user TU). */
    const char* user_file;
    size_t      user_file_basename_off;
    size_t      user_file_basename_len;
    int         in_user_file;
    int         current_line;
    size_t      current_file_len;
    char        current_file[256];
} CCInertScan;

/* Internal: basename portion of `path` (no slash). */
static inline void cc__inert_scan_basename(const char* path, size_t* off, size_t* len) {
    if (!path) { *off = 0; *len = 0; return; }
    size_t n = strlen(path);
    size_t i = n;
    while (i > 0 && path[i - 1] != '/' && path[i - 1] != '\\') i--;
    *off = i;
    *len = n - i;
}

/* Initialize scanner.  `user_file` may be NULL (in_user_file always 1). */
static inline void cc_inert_scan_init(CCInertScan* s, const char* user_file) {
    if (!s) return;
    s->in_line_comment = 0;
    s->in_block_comment = 0;
    s->in_str = 0;
    s->in_chr = 0;
    s->qch = 0;
    s->in_pp = 0;
    s->pp_continued = 0;
    s->at_line_start = 1;
    s->user_file = user_file;
    cc__inert_scan_basename(user_file, &s->user_file_basename_off, &s->user_file_basename_len);
    /* Default: until we see a `#line` directive, assume we're in the user TU. */
    s->in_user_file = 1;
    s->current_line = 0;
    s->current_file_len = 0;
    s->current_file[0] = '\0';
}

/* Internal: classify `path` against the user TU's basename.  Returns
 * 1 if `path` likely refers to the user TU.  We compare basenames
 * because `#line` directives emit different prefixes depending on
 * how the file was reached (relative vs absolute vs CPP-inferred). */
static inline int cc__inert_scan_path_is_user_tu(const CCInertScan* s,
                                                  const char* path,
                                                  size_t      path_len) {
    if (!s || !path || path_len == 0) return 0;
    if (!s->user_file || s->user_file_basename_len == 0) return 1;
    /* Extract basename of `path` */
    size_t i = path_len;
    while (i > 0 && path[i - 1] != '/' && path[i - 1] != '\\') i--;
    size_t pn = path_len - i;
    if (pn != s->user_file_basename_len) return 0;
    return memcmp(path + i,
                  s->user_file + s->user_file_basename_off,
                  pn) == 0;
}

/* Internal: parse a `#line N "file" [flags]` or `# N "file"` directive
 * starting at byte `i` (which is the `#`).  Updates `current_file` /
 * `current_line` / `in_user_file` if the parse succeeds.  Silent
 * no-op otherwise (e.g. on `#define`, `#include`, `#if`, etc.). */
static inline void cc__inert_scan_try_parse_line_directive(CCInertScan* s,
                                                            const char* src,
                                                            size_t n,
                                                            size_t i) {
    if (!s || !src || i >= n || src[i] != '#') return;
    size_t k = i + 1;
    while (k < n && (src[k] == ' ' || src[k] == '\t')) k++;
    /* Optional `line` keyword. */
    if (k + 4 <= n && memcmp(src + k, "line", 4) == 0 &&
        (k + 4 == n || (src[k + 4] == ' ' || src[k + 4] == '\t'))) {
        k += 4;
        while (k < n && (src[k] == ' ' || src[k] == '\t')) k++;
    }
    /* Line number (one or more digits). */
    if (k >= n || src[k] < '0' || src[k] > '9') return;
    int line_num = 0;
    while (k < n && src[k] >= '0' && src[k] <= '9') {
        line_num = line_num * 10 + (src[k] - '0');
        k++;
    }
    s->current_line = line_num;
    while (k < n && (src[k] == ' ' || src[k] == '\t')) k++;
    /* Optional quoted filename. */
    if (k >= n || src[k] != '"') return;
    k++;
    size_t fs = k;
    while (k < n && src[k] != '"' && src[k] != '\n') k++;
    if (k >= n || src[k] != '"') return;
    size_t flen = k - fs;
    if (flen >= sizeof(s->current_file)) flen = sizeof(s->current_file) - 1;
    memcpy(s->current_file, src + fs, flen);
    s->current_file[flen] = '\0';
    s->current_file_len = flen;
    s->in_user_file = cc__inert_scan_path_is_user_tu(s, s->current_file, flen);
}

/* Step the scanner.  If `*pos` is at the start of an inert region
 * (comment / string / char literal / preprocessor directive body),
 * advance `*pos` past the entire region in a single call when
 * possible (multi-char sequences like `//`, `/ *`, `* /` are handled
 * atomically; multi-line constructs unwind across multiple calls).
 *
 * Returns 1 if the call advanced `*pos` over inert content (caller
 * should `continue` rather than process `src[*pos]`).  Returns 0 if
 * `*pos` is at real code (caller is responsible for advancing).
 *
 * This is the same contract as `cc_scanner_skip_non_code` in
 * `cc/src/preprocess/preprocess.c`, just with the addition of
 * `#line`-directive tracking. */
static inline int cc_inert_scan_step(CCInertScan* s, const char* src,
                                      size_t n, size_t* pos) {
    if (!s || !src || !pos || *pos >= n) return 0;
    size_t i = *pos;
    char c = src[i];
    char c2 = (i + 1 < n) ? src[i + 1] : 0;

    /* Preprocessor body (covers #define / #include / #if / #line). */
    if (s->in_pp) {
        if (c == '\n') {
            if (s->pp_continued) { s->pp_continued = 0; }
            else { s->in_pp = 0; s->at_line_start = 1; }
            *pos = i + 1;
            return 1;
        }
        if (c == '\\') {
            size_t k = i + 1;
            while (k < n && (src[k] == ' ' || src[k] == '\t')) k++;
            s->pp_continued = (k < n && src[k] == '\n') ? 1 : 0;
            *pos = i + 1;
            return 1;
        }
        if (c != ' ' && c != '\t') s->pp_continued = 0;
        *pos = i + 1;
        return 1;
    }

    /* Line comment */
    if (s->in_line_comment) {
        if (c == '\n') { s->in_line_comment = 0; s->at_line_start = 1; }
        *pos = i + 1;
        return 1;
    }

    /* Block comment */
    if (s->in_block_comment) {
        if (c == '*' && c2 == '/') {
            s->in_block_comment = 0;
            *pos = i + 2;
            return 1;
        }
        *pos = i + 1;
        return 1;
    }

    /* String literal */
    if (s->in_str) {
        if (c == '\\' && i + 1 < n) { *pos = i + 2; return 1; }
        if (c == s->qch) { s->in_str = 0; }
        *pos = i + 1;
        return 1;
    }

    /* Char literal */
    if (s->in_chr) {
        if (c == '\\' && i + 1 < n) { *pos = i + 2; return 1; }
        if (c == '\'') { s->in_chr = 0; }
        *pos = i + 1;
        return 1;
    }

    /* Detect entry into a comment / string / char literal / pp directive. */
    if (c == '/' && c2 == '/') {
        s->in_line_comment = 1;
        *pos = i + 2;
        return 1;
    }
    if (c == '/' && c2 == '*') {
        s->in_block_comment = 1;
        *pos = i + 2;
        return 1;
    }
    if (c == '"') {
        s->in_str = 1; s->qch = '"';
        *pos = i + 1;
        return 1;
    }
    if (c == '\'') {
        s->in_chr = 1;
        *pos = i + 1;
        return 1;
    }
    if (s->at_line_start && c == '#') {
        cc__inert_scan_try_parse_line_directive(s, src, n, i);
        s->in_pp = 1;
        s->pp_continued = 0;
        s->at_line_start = 0;
        *pos = i + 1;
        return 1;
    }

    /* Real code byte.  Update at_line_start so the next iteration can
     * still detect a `#` directive on a fresh line. */
    if (c == '\n') s->at_line_start = 1;
    else if (c != ' ' && c != '\t' && c != '\r') s->at_line_start = 0;
    return 0;
}

/* ---- Directive-head parsing (harvest-before-skip) ----
 *
 * Inert scanning consumes preprocessor directives whole, which is right
 * for rewrite passes but wrong for probes that must READ a directive's
 * head — "does this text #define a function-like macro named X?".  The
 * pattern is to parse the head at the `#` BEFORE letting the scanner
 * consume the line.  These helpers are that parse, shared, so every
 * macro-realness probe agrees on what a #define head is. */

/* Parse `# ws* define ws+ NAME` at `hash_pos`, which must be a `#` the
 * caller's scanner has established as real code at line start.  On
 * match, `name_off`/`name_len` describe the macro name span and
 * `is_fnlike` is 1 when `(` immediately follows the name (function-like
 * macro; C permits no space there).  Returns 1 when the directive is a
 * #define with a well-formed name, 0 otherwise. */
static inline int cc_scan_define_head(const char* src, size_t n, size_t hash_pos,
                                      size_t* name_off, size_t* name_len,
                                      int* is_fnlike) {
    size_t q, s;
    char c;
    if (!src || hash_pos >= n || src[hash_pos] != '#') return 0;
    q = hash_pos + 1;
    while (q < n && (src[q] == ' ' || src[q] == '\t')) q++;
    if (q + 6 > n || memcmp(src + q, "define", 6) != 0) return 0;
    q += 6;
    if (q >= n || (src[q] != ' ' && src[q] != '\t')) return 0;
    while (q < n && (src[q] == ' ' || src[q] == '\t')) q++;
    if (q >= n) return 0;
    c = src[q];
    if (!(c == '_' || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'))) return 0;
    s = q;
    while (q < n) {
        c = src[q];
        if (!(c == '_' || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9')))
            break;
        q++;
    }
    if (name_off) *name_off = s;
    if (name_len) *name_len = q - s;
    if (is_fnlike) *is_fnlike = (q < n && src[q] == '(');
    return 1;
}

/* Does the buffer #define a function-like macro named `name`?  Real
 * directives only: the `#` must be actual code at the start of a line —
 * a "#define name(" spelled inside a comment or string literal does not
 * count.  Shared answer for "a TU macro binding wins" checks. */
static inline int cc_text_defines_fnlike_macro(const char* src, size_t n,
                                               const char* name) {
    CCInertScan s;
    size_t i = 0, nlen;
    if (!src || !name) return 0;
    nlen = strlen(name);
    if (nlen == 0) return 0;
    cc_inert_scan_init(&s, NULL);
    while (i < n) {
        if (src[i] == '#' && s.at_line_start && !s.in_pp &&
            !s.in_line_comment && !s.in_block_comment && !s.in_str && !s.in_chr) {
            size_t noff = 0, nl = 0;
            int fnlike = 0;
            if (cc_scan_define_head(src, n, i, &noff, &nl, &fnlike) &&
                fnlike && nl == nlen && memcmp(src + noff, name, nlen) == 0)
                return 1;
        }
        if (cc_inert_scan_step(&s, src, n, &i)) continue;
        i++;
    }
    return 0;
}

#endif /* CC_UTIL_TEXT_SCAN_H */
