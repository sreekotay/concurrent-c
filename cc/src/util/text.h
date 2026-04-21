/*
 * Shared text manipulation helpers for compiler passes.
 * 
 * These are static inline to avoid link-time coupling between passes
 * while eliminating code duplication.
 */
#ifndef CC_UTIL_TEXT_H
#define CC_UTIL_TEXT_H

#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- Identifier character predicates ---- */

static inline int cc_is_ident_start(char c) {
    return (c == '_') || ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'));
}

static inline int cc_is_ident_char(char c) {
    return cc_is_ident_start(c) || (c >= '0' && c <= '9');
}

/* ---- Whitespace helpers ---- */

static inline const char* cc_skip_ws(const char* s) {
    if (!s) return s;
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') s++;
    return s;
}

static inline size_t cc_skip_ws_len(const char* s, size_t len, size_t start) {
    while (start < len && (s[start] == ' ' || s[start] == '\t' || 
                           s[start] == '\r' || s[start] == '\n')) {
        start++;
    }
    return start;
}

/* ---- String builder (dynamic buffer) ---- */

static inline void cc_sb_append(char** buf, size_t* len, size_t* cap, const char* s, size_t n) {
    if (!buf || !len || !cap || !s || n == 0) return;
    size_t need = *len + n + 1;
    if (need > *cap) {
        size_t nc = (*cap ? *cap * 2 : 1024);
        while (nc < need) nc *= 2;
        char* nb = (char*)realloc(*buf, nc);
        if (!nb) return;
        *buf = nb;
        *cap = nc;
    }
    memcpy(*buf + *len, s, n);
    *len += n;
    (*buf)[*len] = 0;
}

static inline void cc_sb_append_cstr(char** buf, size_t* len, size_t* cap, const char* s) {
    if (!s) return;
    cc_sb_append(buf, len, cap, s, strlen(s));
}

static inline void cc_sb_append_vfmt(char** buf, size_t* len, size_t* cap, const char* fmt, va_list ap) {
    if (!buf || !len || !cap || !fmt) return;
    if (!*buf) {
        *cap = 256;
        *buf = (char*)malloc(*cap);
        if (!*buf) return;
        (*buf)[0] = 0;
        *len = 0;
    }
    for (;;) {
        size_t avail = (*cap > *len) ? (*cap - *len) : 0;
        va_list ap2;
        va_copy(ap2, ap);
        int n = vsnprintf(*buf + *len, avail, fmt, ap2);
        va_end(ap2);
        if (n < 0) return;
        if ((size_t)n < avail) { *len += (size_t)n; return; }
        *cap = (*cap ? *cap * 2 : 256) + (size_t)n + 16;
        *buf = (char*)realloc(*buf, *cap);
        if (!*buf) return;
    }
}

/* Note: cc_sb_append_fmt is a variadic wrapper - define in each file to avoid inline+variadic issues */
#define CC_DEFINE_SB_APPEND_FMT \
    static void cc_sb_append_fmt(char** buf, size_t* len, size_t* cap, const char* fmt, ...) { \
        va_list ap; \
        va_start(ap, fmt); \
        cc_sb_append_vfmt(buf, len, cap, fmt, ap); \
        va_end(ap); \
    }

/* ---- String duplication helpers ---- */

static inline char* cc_strndup(const char* s, size_t n) {
    if (!s) return NULL;
    char* out = (char*)malloc(n + 1);
    if (!out) return NULL;
    memcpy(out, s, n);
    out[n] = 0;
    return out;
}

static inline char* cc_strndup_trim_ws(const char* s, size_t n) {
    if (!s) return NULL;
    size_t i = 0;
    while (i < n && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) i++;
    size_t j = n;
    while (j > i && (s[j - 1] == ' ' || s[j - 1] == '\t' || s[j - 1] == '\n' || s[j - 1] == '\r')) j--;
    size_t m = (j > i) ? (j - i) : 0;
    char* out = (char*)malloc(m + 1);
    if (!out) return NULL;
    if (m) memcpy(out, s + i, m);
    out[m] = 0;
    return out;
}

static inline char* cc_dup_slice(const char* b, size_t start, size_t end) {
    if (!b || end <= start) return strdup("");
    return cc_strndup(b + start, end - start);
}

/* ---- Matching bracket/paren/brace helpers ---- */

/* Find matching ')' for '(' at position lpar. Returns 1 on success, 0 on failure. */
static inline int cc_find_matching_paren(const char* b, size_t bl, size_t lpar, size_t* out_rpar) {
    if (!b || lpar >= bl || b[lpar] != '(') return 0;
    int par = 1, brk = 0, br = 0;
    int ins = 0; char q = 0;
    int in_lc = 0, in_bc = 0;
    for (size_t p = lpar + 1; p < bl; p++) {
        char ch = b[p];
        char ch2 = (p + 1 < bl) ? b[p + 1] : 0;
        if (in_lc) { if (ch == '\n') in_lc = 0; continue; }
        if (in_bc) { if (ch == '*' && ch2 == '/') { in_bc = 0; p++; } continue; }
        if (ins) { if (ch == '\\' && p + 1 < bl) { p++; continue; } if (ch == q) ins = 0; continue; }
        if (ch == '/' && ch2 == '/') { in_lc = 1; p++; continue; }
        if (ch == '/' && ch2 == '*') { in_bc = 1; p++; continue; }
        if (ch == '"' || ch == '\'') { ins = 1; q = ch; continue; }
        if (ch == '(') par++;
        else if (ch == ')') { par--; if (par == 0) { if (out_rpar) *out_rpar = p; return 1; } }
        else if (ch == '[') brk++;
        else if (ch == ']') { if (brk) brk--; }
        else if (ch == '{') br++;
        else if (ch == '}') { if (br) br--; }
    }
    return 0;
}

/* Find matching '}' for '{' at position lbrace. Returns 1 on success, 0 on failure. */
static inline int cc_find_matching_brace(const char* b, size_t bl, size_t lbrace, size_t* out_rbrace) {
    if (!b || lbrace >= bl || b[lbrace] != '{') return 0;
    int br = 1, par = 0, brk = 0;
    int ins = 0; char q = 0;
    int in_lc = 0, in_bc = 0;
    for (size_t p = lbrace + 1; p < bl; p++) {
        char ch = b[p];
        char ch2 = (p + 1 < bl) ? b[p + 1] : 0;
        if (in_lc) { if (ch == '\n') in_lc = 0; continue; }
        if (in_bc) { if (ch == '*' && ch2 == '/') { in_bc = 0; p++; } continue; }
        if (ins) { if (ch == '\\' && p + 1 < bl) { p++; continue; } if (ch == q) ins = 0; continue; }
        if (ch == '/' && ch2 == '/') { in_lc = 1; p++; continue; }
        if (ch == '/' && ch2 == '*') { in_bc = 1; p++; continue; }
        if (ch == '"' || ch == '\'') { ins = 1; q = ch; continue; }
        if (ch == '(') par++;
        else if (ch == ')') { if (par) par--; }
        else if (ch == '[') brk++;
        else if (ch == ']') { if (brk) brk--; }
        else if (ch == '{') br++;
        else if (ch == '}') { br--; if (br == 0) { if (out_rbrace) *out_rbrace = p; return 1; } }
    }
    return 0;
}

/* Find matching ']' for '[' at position lbrack. Returns 1 on success, 0 on failure. */
static inline int cc_find_matching_bracket(const char* b, size_t bl, size_t lbrack, size_t* out_rbrack) {
    if (!b || lbrack >= bl || b[lbrack] != '[') return 0;
    int brk = 1, par = 0, br = 0;
    int ins = 0; char q = 0;
    int in_lc = 0, in_bc = 0;
    for (size_t p = lbrack + 1; p < bl; p++) {
        char ch = b[p];
        char ch2 = (p + 1 < bl) ? b[p + 1] : 0;
        if (in_lc) { if (ch == '\n') in_lc = 0; continue; }
        if (in_bc) { if (ch == '*' && ch2 == '/') { in_bc = 0; p++; } continue; }
        if (ins) { if (ch == '\\' && p + 1 < bl) { p++; continue; } if (ch == q) ins = 0; continue; }
        if (ch == '/' && ch2 == '/') { in_lc = 1; p++; continue; }
        if (ch == '/' && ch2 == '*') { in_bc = 1; p++; continue; }
        if (ch == '"' || ch == '\'') { ins = 1; q = ch; continue; }
        if (ch == '(') par++;
        else if (ch == ')') { if (par) par--; }
        else if (ch == '{') br++;
        else if (ch == '}') { if (br) br--; }
        else if (ch == '[') brk++;
        else if (ch == ']') { brk--; if (brk == 0) { if (out_rbrack) *out_rbrack = p; return 1; } }
    }
    return 0;
}

/* ---- Skip whitespace and comments ---- */

/* Pointer-based whitespace + comment skipper.
 *
 * Equivalent to `cc_skip_ws` but also eats `// ...` line comments and
 * `/ * ... * /` block comments.  Used by raw-text rewrite passes that
 * classify statements by their leading token: a comment sitting before a
 * decl would otherwise hide the type keyword and push the scanner onto
 * the generic fallback path, which leaks the type prefix into the
 * emitted output (e.g. `size_t __f->x = 0;`).  See
 * `cc/src/visitor/async_ast.c` for the original bug. */
static inline const char* cc_skip_ws_and_comments_ptr(const char* s) {
    if (!s) return s;
    while (*s) {
        char c = *s;
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') { s++; continue; }
        if (c == '/' && s[1] == '/') {
            s += 2;
            while (*s && *s != '\n') s++;
            continue;
        }
        if (c == '/' && s[1] == '*') {
            s += 2;
            while (*s && !(*s == '*' && s[1] == '/')) s++;
            if (*s) s += 2;
            continue;
        }
        break;
    }
    return s;
}

static inline size_t cc_skip_ws_and_comments(const char* src, size_t len, size_t i) {
    while (i < len) {
        char c = src[i];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') { i++; continue; }
        if (c == '/' && i + 1 < len) {
            if (src[i + 1] == '/') {
                i += 2;
                while (i < len && src[i] != '\n') i++;
                continue;
            }
            if (src[i + 1] == '*') {
                i += 2;
                while (i + 1 < len && !(src[i] == '*' && src[i + 1] == '/')) i++;
                if (i + 1 < len) i += 2;
                continue;
            }
        }
        break;
    }
    return i;
}

/* ---- Structural forward search (comment/string/bracket aware) ----
 *
 * Raw-text lowering passes repeatedly ask "given a function/decl/sigil at
 * position P, where is the next `(` / `{` / identifier X?"  The naive
 * answers (`strchr`, `strstr`) accept any byte match, including bytes
 * inside `/ * ... * /` and `// ...` comments, string/char literals, and
 * nested bracket groups.  That has been the root cause of a long tail of
 * lowering bugs where the scanner drifts into a comment's parenthetical,
 * a neighbouring statement-expression `({ ... })`, or the body of an
 * already-rewritten sibling function — and emits garbage.
 *
 * These helpers answer the same questions but respect C lexical
 * structure: they skip line comments (`// ... \n`), block comments
 * (`/ * ... * /`), and `"..." / '...'` literals, and — for
 * `cc_find_char_top_level` — only return matches at paren/brace/bracket
 * nesting depth zero relative to the starting position.  That last
 * property is what lets callers say "find the `(` that opens the param
 * list, not any `(` that happens to appear inside a declarator tag or a
 * `__typeof__(...)` stmt-expr that was already emitted by an earlier
 * pass."
 *
 * Both functions are pure forward searches over the half-open range
 * `src[start..end)`.  They return `end` if no match is found, so callers
 * can use `pos < end` as a success check. */

/* Forward-find the first top-level occurrence of `ch` in src[start..end),
 * skipping line/block comments and string/char literals, and skipping
 * over balanced `()` / `[]` / `{}` groups so that only depth-0 matches
 * are returned.  Note: this means if `ch == '('`, the very first `(` at
 * depth 0 is returned and the scan does not descend into it. */
static inline size_t cc_find_char_top_level(const char* src, size_t start, size_t end, char ch) {
    if (!src || start >= end) return end;
    int par = 0, brk = 0, br = 0;
    int in_lc = 0, in_bc = 0;
    int ins = 0; char q = 0;
    for (size_t p = start; p < end; p++) {
        char c = src[p];
        char c2 = (p + 1 < end) ? src[p + 1] : 0;
        /* Line-comment ends AT the `\n`, so the terminator is not part
         * of the comment and may still be a valid delimiter match for
         * callers searching for newline boundaries. */
        if (in_lc) { if (c == '\n') { in_lc = 0; } else continue; }
        if (in_bc) { if (c == '*' && c2 == '/') { in_bc = 0; p++; } continue; }
        if (ins) { if (c == '\\' && p + 1 < end) { p++; continue; } if (c == q) ins = 0; continue; }
        if (c == '/' && c2 == '/') { in_lc = 1; p++; continue; }
        if (c == '/' && c2 == '*') { in_bc = 1; p++; continue; }
        if (c == '"' || c == '\'') { ins = 1; q = c; continue; }
        if (par == 0 && brk == 0 && br == 0 && c == ch) return p;
        if (c == '(') par++;
        else if (c == ')') { if (par) par--; }
        else if (c == '[') brk++;
        else if (c == ']') { if (brk) brk--; }
        else if (c == '{') br++;
        else if (c == '}') { if (br) br--; }
    }
    return end;
}

/* Backward-find the first top-level occurrence of any char in `delims`
 * when scanning from `end` towards `start` in `src`.  Like the forward
 * helper, this:
 *   - skips `/ * ... * /` and `// ...\n` comments,
 *   - skips `"..."` / `'...'` literals,
 *   - treats `()` / `[]` / `{}` as balanced groups: an inner `(` can
 *     match an inner `)` and the scan will not stop on either, but a
 *     lone opener with no preceding closer at the current depth WILL
 *     stop the scan (matching the `cc__scan_back_to_delim` semantics
 *     for C declarators, where `(` delimits the start of a parenthesized
 *     declarator).
 *
 * Returns a position `p` in `[start..end]` such that `src[p]` is the
 * matching delim (or `(` at depth 0), with leading whitespace skipped
 * forward so the returned position points at the first non-whitespace
 * character following the delim — matching the convention used by the
 * original `cc__scan_back_to_delim`.  If no delim is found the function
 * returns `start` (still skipped past leading whitespace).
 *
 * `delims` is a null-terminated string of delimiter characters.  A
 * common choice for C decl boundaries is `";{},<\n"`.
 *
 * Implementation note: this walks backward one byte at a time.  Because
 * comments and strings are only delimited forward-in-time, detecting
 * them during a backward walk requires a prepass.  We do a lightweight
 * prepass that labels every byte in `[start..end]` as either "code" or
 * "skip", then do the backward scan over labeled bytes only.  That's
 * O(end - start) space but still linear time and avoids the tricky
 * "am I inside a comment?" question on the backward pass. */
static inline size_t cc_rfind_char_top_level(const char* src, size_t start, size_t end,
                                              const char* delims) {
    if (!src || !delims || start >= end) return start;
    size_t span = end - start;
    /* `mask[i] == 1` => src[start+i] is inside a comment or string
     * literal and should be skipped.  Caller buffers up to 1<<20 bytes;
     * that's the largest source region any pass scans in practice.  For
     * anything larger, fall back to naive backward scan (which is still
     * better than the pre-metaclass state since most backward-scan
     * call sites run on short local regions). */
    if (span > (1u << 20)) {
        size_t i = end;
        while (i > start) {
            char c = src[i - 1];
            for (const char* d = delims; *d; d++) {
                if (c == *d) {
                    size_t p = i;
                    while (p < end && (src[p] == ' ' || src[p] == '\t')) p++;
                    return p;
                }
            }
            i--;
        }
        size_t p = start;
        while (p < end && (src[p] == ' ' || src[p] == '\t')) p++;
        return p;
    }
    unsigned char stackbuf[1024];
    unsigned char* mask = stackbuf;
    unsigned char* heapbuf = NULL;
    if (span > sizeof(stackbuf)) {
        heapbuf = (unsigned char*)malloc(span);
        if (!heapbuf) {
            size_t p = start;
            while (p < end && (src[p] == ' ' || src[p] == '\t')) p++;
            return p;
        }
        mask = heapbuf;
    }
    /* Forward prepass: mark comment / string / char-literal bytes. */
    {
        int in_lc = 0, in_bc = 0;
        int ins = 0; char q = 0;
        for (size_t i = 0; i < span; i++) {
            char c = src[start + i];
            char c2 = (i + 1 < span) ? src[start + i + 1] : 0;
            if (in_lc) {
                /* Line-comment ends AT the `\n`, so the terminator is
                 * not part of the comment — leave it unmasked so the
                 * backward scan can still treat it as a line delim. */
                if (c == '\n') { in_lc = 0; mask[i] = 0; continue; }
                mask[i] = 1;
                continue;
            }
            if (in_bc) {
                mask[i] = 1;
                if (c == '*' && c2 == '/') {
                    if (i + 1 < span) { mask[i + 1] = 1; i++; }
                    in_bc = 0;
                }
                continue;
            }
            if (ins) {
                mask[i] = 1;
                if (c == '\\' && i + 1 < span) {
                    if (i + 1 < span) { mask[i + 1] = 1; i++; }
                    continue;
                }
                if (c == q) ins = 0;
                continue;
            }
            if (c == '/' && c2 == '/') {
                mask[i] = 1;
                if (i + 1 < span) { mask[i + 1] = 1; i++; }
                in_lc = 1;
                continue;
            }
            if (c == '/' && c2 == '*') {
                mask[i] = 1;
                if (i + 1 < span) { mask[i + 1] = 1; i++; }
                in_bc = 1;
                continue;
            }
            if (c == '"' || c == '\'') {
                mask[i] = 1;
                ins = 1; q = c;
                continue;
            }
            mask[i] = 0;
        }
    }
    /* Backward scan on code bytes only, tracking paren/bracket nesting. */
    int paren_depth = 0, brack_depth = 0, brace_depth = 0;
    size_t i = span;
    size_t hit = (size_t)-1;
    while (i > 0) {
        i--;
        if (mask[i]) continue;
        char c = src[start + i];
        if (c == ')') { paren_depth++; continue; }
        if (c == '(') {
            if (paren_depth > 0) { paren_depth--; continue; }
            /* Position after the `(` — caller expects the first byte
             * of whatever sits inside the parenthesized declarator. */
            hit = i + 1;
            break;
        }
        if (c == ']') { brack_depth++; continue; }
        if (c == '[') { if (brack_depth > 0) brack_depth--; continue; }
        if (c == '}') { brace_depth++; continue; }
        if (c == '{') {
            if (brace_depth > 0) { brace_depth--; continue; }
            /* `{` is a delim if listed. */
        }
        if (paren_depth == 0 && brack_depth == 0 && brace_depth == 0) {
            for (const char* d = delims; *d; d++) {
                if (c == *d) { hit = i + 1; goto done; }
            }
        }
    }
done:
    {
        size_t p = (hit == (size_t)-1) ? start : (start + hit);
        while (p < end && (src[p] == ' ' || src[p] == '\t')) p++;
        if (heapbuf) free(heapbuf);
        return p;
    }
}

/* Forward-find the first word-bounded occurrence of `name` (length
 * `name_len`) in src[start..end), skipping comments and strings.  Word
 * boundary = the chars immediately before the match and immediately
 * after must not be identifier chars.  Returns position of first char
 * of the match, or `end` if not found.  Unlike `cc_find_char_top_level`,
 * this DOES return matches inside nested brackets (so it can find e.g.
 * the function name inside `int handle_client(int x)` where the name
 * sits between the return type and the param `(`).  Callers who need
 * top-level-only lookups should post-filter. */
static inline size_t cc_find_ident_top_level(const char* src, size_t start, size_t end,
                                             const char* name, size_t name_len) {
    if (!src || !name || name_len == 0 || start + name_len > end) return end;
    int in_lc = 0, in_bc = 0;
    int ins = 0; char q = 0;
    for (size_t p = start; p + name_len <= end; p++) {
        char c = src[p];
        char c2 = (p + 1 < end) ? src[p + 1] : 0;
        if (in_lc) { if (c == '\n') in_lc = 0; continue; }
        if (in_bc) { if (c == '*' && c2 == '/') { in_bc = 0; p++; } continue; }
        if (ins) { if (c == '\\' && p + 1 < end) { p++; continue; } if (c == q) ins = 0; continue; }
        if (c == '/' && c2 == '/') { in_lc = 1; p++; continue; }
        if (c == '/' && c2 == '*') { in_bc = 1; p++; continue; }
        if (c == '"' || c == '\'') { ins = 1; q = c; continue; }
        if (memcmp(src + p, name, name_len) != 0) continue;
        if (p > start && cc_is_ident_char(src[p - 1])) continue;
        if (p + name_len < end && cc_is_ident_char(src[p + name_len])) continue;
        return p;
    }
    return end;
}

/* Forward-find the first occurrence of literal substring `needle` (length
 * `needle_len`) in src[start..end), skipping comments and string
 * literals.  Unlike `cc_find_ident_top_level`, this does NOT require
 * word boundaries around the match — it's suitable for multi-char
 * sigils like `!>`, `?>`, `=<!`, `@async`, `@destroy`, where the
 * surrounding chars are typically punctuation anyway.
 *
 * Returns position of first byte of the match, or `end` if not found.
 *
 * Use this as a drop-in replacement for `strstr(buf, needle)` when
 * `buf` contains user source text and a match inside a comment or
 * string literal would mis-steer a lowering decision. */
static inline size_t cc_find_substr_top_level(const char* src, size_t start, size_t end,
                                              const char* needle, size_t needle_len) {
    if (!src || !needle || needle_len == 0 || start + needle_len > end) return end;
    int in_lc = 0, in_bc = 0;
    int ins = 0; char q = 0;
    for (size_t p = start; p + needle_len <= end; p++) {
        char c = src[p];
        char c2 = (p + 1 < end) ? src[p + 1] : 0;
        if (in_lc) { if (c == '\n') in_lc = 0; continue; }
        if (in_bc) { if (c == '*' && c2 == '/') { in_bc = 0; p++; } continue; }
        if (ins) { if (c == '\\' && p + 1 < end) { p++; continue; } if (c == q) ins = 0; continue; }
        if (c == '/' && c2 == '/') { in_lc = 1; p++; continue; }
        if (c == '/' && c2 == '*') { in_bc = 1; p++; continue; }
        if (c == '"' || c == '\'') { ins = 1; q = c; continue; }
        if (memcmp(src + p, needle, needle_len) == 0) return p;
    }
    return end;
}

/* Boolean predicate: does `needle` appear in src[0..len) outside
 * comments and string literals?  This is the drop-in replacement for
 * `strstr(src, needle) != NULL` in presence-check heuristics scattered
 * across the lowering passes (e.g. "does this function body contain
 * an `@async` / `await` / `!>` sigil, and therefore need the async-
 * aware code path?").
 *
 * Pre-metaclass, those checks flagged false positives any time the
 * sigil appeared in a comment — `/ * example: await x * /` was enough
 * to switch a pass onto its async path, which at best was wasted work
 * and at worst produced incorrect lowerings. */
static inline int cc_contains_token_top_level(const char* src, size_t len, const char* needle) {
    if (!src || !needle) return 0;
    size_t nl = strlen(needle);
    if (nl == 0) return 0;
    return cc_find_substr_top_level(src, 0, len, needle, nl) < len;
}

/* ---- Error-lowering helpers ----
 *
 * The `!> / ?> / @err` pointer-lowering paths all need to embed a snippet
 * of user source text (the consumed expression) inside a synthesized C
 * string literal — the `"NULL returned from <expr> at <file>:<line>"`
 * message that populates `CCError.message`.  Doing that naively with
 * `snprintf("... %.*s ...", (int)n, src+a)` breaks any time the source
 * expression spans multiple lines, contains an unescaped `"` or `\`, or
 * includes a raw control byte — the generated C no longer compiles
 * ("missing terminating `\"` character").
 *
 * The helpers below centralize the three concerns:
 *   - sanitize: collapse whitespace runs (incl. `\n`/`\t`) to a single
 *     space, escape `"` and `\`, replace control bytes with `?`, and
 *     truncate overly long snippets with `...`.
 *   - emit: append a properly-quoted C string literal to a `cc_sb_*`
 *     string builder.
 *   - compose: append a full `__cc_err_null_at("<expr>", "<file>",
 *     "<line>")` call — i.e. the complete `CCError` constructor that
 *     `cc_result.cch` exposes — so callers just supply the expression
 *     range and location.
 *
 * All five `CC_ERR_NULL` synthesis sites across `pass_result_unwrap.c`
 * and `pass_err_syntax.c` use these helpers; keeping the structural
 * `(CCError){ .kind = ..., .message = ... }` boilerplate inside the
 * runtime macro (and out of the lowering passes) means future changes
 * to the `CCError` shape don't have to be chased across N copies of the
 * same string template. */

/* Append a C string literal to the string builder, rendering
 * `src[a..b)` safely: whitespace runs collapse to a single space,
 * `"` / `\` are escaped, control bytes become `?`, and snippets longer
 * than 160 visible chars are truncated with `...`.  Emits the opening
 * and closing `"` itself. */
static inline void cc_sb_append_c_string_literal(char** buf, size_t* len, size_t* cap,
                                                  const char* src, size_t a, size_t b) {
    if (!buf || !len || !cap) return;
    cc_sb_append(buf, len, cap, "\"", 1);
    if (src && b > a) {
        char tmp[256];
        size_t w = 0;
        int prev_space = 0;
        const size_t vis_max = 160;
        int truncated = 0;
        for (size_t i = a; i < b && w + 3 < sizeof(tmp); i++) {
            unsigned char c = (unsigned char)src[i];
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n' ||
                c == '\v' || c == '\f') {
                if (!prev_space && w > 0) { tmp[w++] = ' '; prev_space = 1; }
                continue;
            }
            prev_space = 0;
            if (w >= vis_max) { truncated = 1; break; }
            if (c == '\\' || c == '"') {
                if (w + 2 >= sizeof(tmp)) { truncated = 1; break; }
                tmp[w++] = '\\';
                tmp[w++] = (char)c;
            } else if (c < 0x20 || c == 0x7f) {
                tmp[w++] = '?';
            } else {
                tmp[w++] = (char)c;
            }
        }
        while (w > 0 && tmp[w - 1] == ' ') w--;
        if (w) cc_sb_append(buf, len, cap, tmp, w);
        if (truncated) cc_sb_append(buf, len, cap, "...", 3);
    }
    cc_sb_append(buf, len, cap, "\"", 1);
}

/* Append a `__cc_err_null_at("<sanitized-expr>", "<file>", "<line>")`
 * call expression to the string builder.  The three literal arguments
 * are spelled as C string literals so the runtime macro in
 * `cc_result.cch` can string-concat them at preprocessor time (no
 * runtime formatting).  Callers still need to emit the surrounding
 * `CCError <binder> = ...;` or `if (x == NULL) { ... }` scaffolding —
 * this helper only produces the error-value expression itself. */
static inline void cc_sb_append_err_null_at(char** buf, size_t* len, size_t* cap,
                                             const char* src, size_t a, size_t b,
                                             const char* file, int line) {
    if (!buf || !len || !cap) return;
    cc_sb_append(buf, len, cap, "__cc_err_null_at(", (size_t)sizeof("__cc_err_null_at(") - 1);
    cc_sb_append_c_string_literal(buf, len, cap, src, a, b);
    cc_sb_append(buf, len, cap, ", ", 2);
    /* File path goes in as a plain C string literal — the runtime doesn't
     * need it sanitized the same way the expression does, but we reuse
     * the helper so embedded quotes/backslashes (e.g. Windows paths) are
     * handled consistently. */
    {
        const char* f = file ? file : "<input>";
        size_t fl = strlen(f);
        cc_sb_append_c_string_literal(buf, len, cap, f, 0, fl);
    }
    cc_sb_append(buf, len, cap, ", \"", 3);
    {
        char lbuf[32];
        int n = snprintf(lbuf, sizeof(lbuf), "%d", line);
        if (n > 0) cc_sb_append(buf, len, cap, lbuf, (size_t)n);
    }
    cc_sb_append(buf, len, cap, "\")", 2);
}

/* Append a `__cc_uw_err_at(<tmpv>, "<sanitized-expr>", "<file>", "<line>")`
 * call expression — the _Generic-dispatching unified variant of
 * `cc_sb_append_err_null_at`.  The macro selects the Result-struct's
 * `.u.error` when `<tmpv>` is a Result type and `__cc_err_null_at(...)`
 * when `<tmpv>` is a raw pointer.  Callers don't need to scan the
 * source to decide between the two shapes. */
static inline void cc_sb_append_uw_err_at(char** buf, size_t* len, size_t* cap,
                                           const char* tmpv,
                                           const char* src, size_t a, size_t b,
                                           const char* file, int line) {
    if (!buf || !len || !cap || !tmpv) return;
    cc_sb_append(buf, len, cap, "__cc_uw_err_at(", (size_t)sizeof("__cc_uw_err_at(") - 1);
    cc_sb_append(buf, len, cap, tmpv, strlen(tmpv));
    cc_sb_append(buf, len, cap, ", ", 2);
    cc_sb_append_c_string_literal(buf, len, cap, src, a, b);
    cc_sb_append(buf, len, cap, ", ", 2);
    {
        const char* f = file ? file : "<input>";
        size_t fl = strlen(f);
        cc_sb_append_c_string_literal(buf, len, cap, f, 0, fl);
    }
    cc_sb_append(buf, len, cap, ", \"", 3);
    {
        char lbuf[32];
        int n = snprintf(lbuf, sizeof(lbuf), "%d", line);
        if (n > 0) cc_sb_append(buf, len, cap, lbuf, (size_t)n);
    }
    cc_sb_append(buf, len, cap, "\")", 2);
}

/* ---- Token checking ---- */

/* Check if a token exists in a range (word boundaries respected). */
static inline int cc_range_contains_token(const char* s, size_t n, const char* tok) {
    if (!s || !tok) return 0;
    size_t tn = strlen(tok);
    if (tn == 0 || n < tn) return 0;
    for (size_t i = 0; i + tn <= n; i++) {
        if (memcmp(s + i, tok, tn) != 0) continue;
        if (i > 0 && cc_is_ident_char(s[i - 1])) continue;
        if (i + tn < n && cc_is_ident_char(s[i + tn])) continue;
        return 1;
    }
    return 0;
}

/* ---- Declaration helpers ---- */

static inline int cc_is_non_decl_stmt_type(const char* type_name) {
    return type_name &&
           (strcmp(type_name, "return") == 0 ||
            strcmp(type_name, "break") == 0 ||
            strcmp(type_name, "continue") == 0 ||
            strcmp(type_name, "goto") == 0 ||
            strcmp(type_name, "case") == 0 ||
            strcmp(type_name, "default") == 0);
}

/* ---- Declaration parsing ---- */

/*
 * Extract variable name and type from a C declaration statement.
 * Given a pointer range [stmt, stmt_end) representing text like "int x" or
 * "Container c", writes the variable name into out_name and the type into
 * out_type. Handles leading/inline comments, string/char literals, __CC_*
 * type macros, and rejects non-declaration statements (assignments to fields,
 * function calls, etc.).
 */
static inline void cc_parse_decl_name_and_type(const char* stmt,
                                               const char* stmt_end,
                                               char* out_name,
                                               size_t out_name_sz,
                                               char* out_type,
                                               size_t out_type_sz) {
    const char* p = stmt;
    const char* semi = stmt_end;
    const char* name_s = NULL;
    size_t name_n = 0;
    const char* cur;
    if (!stmt || !stmt_end || stmt_end <= stmt || !out_name || !out_type) return;
    out_name[0] = '\0';
    out_type[0] = '\0';
    for (;;) {
        while (p < semi && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
        if (p + 1 < semi && p[0] == '/' && p[1] == '/') {
            p += 2;
            while (p < semi && *p != '\n') p++;
            continue;
        }
        if (p + 1 < semi && p[0] == '/' && p[1] == '*') {
            p += 2;
            while (p + 1 < semi && !(p[0] == '*' && p[1] == '/')) p++;
            if (p + 1 < semi) p += 2;
            continue;
        }
        break;
    }
    if (p >= semi) return;
    cur = p;
    while (cur < semi) {
        if (cur + 1 < semi && cur[0] == '/' && cur[1] == '/') {
            cur += 2;
            while (cur < semi && *cur != '\n') cur++;
            continue;
        }
        if (cur + 1 < semi && cur[0] == '/' && cur[1] == '*') {
            cur += 2;
            while (cur + 1 < semi && !(cur[0] == '*' && cur[1] == '/')) cur++;
            if (cur + 1 < semi) cur += 2;
            continue;
        }
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
        if (!cc_is_ident_start(*cur)) { cur++; continue; }
        {
            const char* s = cur++;
            while (cur < semi && cc_is_ident_char(*cur)) cur++;
            name_s = s;
            name_n = (size_t)(cur - s);
        }
    }
    if (!name_s || name_n == 0) return;
    {
        const char* after = name_s + name_n;
        const char* eq = NULL;
        const char* lp = NULL;
        for (const char* t = p; t < semi; ++t) {
            if (t + 1 < semi && t[0] == '/' && t[1] == '/') {
                t += 2;
                while (t < semi && *t != '\n') t++;
                if (t >= semi) break;
                continue;
            }
            if (t + 1 < semi && t[0] == '/' && t[1] == '*') {
                t += 2;
                while (t + 1 < semi && !(t[0] == '*' && t[1] == '/')) t++;
                if (t + 1 < semi) t++;
                continue;
            }
            if (*t == '=' && !eq) eq = t;
            if (*t == '(' && !lp) lp = t;
            if (*t == '.' || (*t == '>' && t > p && t[-1] == '-')) {
                return;
            }
        }
        while (after < semi && (after[0] == ' ' || after[0] == '\t' ||
               after[0] == '\n' || after[0] == '\r')) after++;
        if (after < semi && *after != '=' && *after != ';' && *after != '[') {
            return;
        }
        if (lp && lp < name_s && (!eq || eq > lp)) {
            const char* macro_start = lp;
            while (macro_start > p && cc_is_ident_char(macro_start[-1])) macro_start--;
            if (!((size_t)(lp - macro_start) >= 5 && memcmp(macro_start, "__CC_", 5) == 0)) {
                return;
            }
        }
    }
    {
        const char* ty_s = p;
        const char* ty_e = name_s;
        while (ty_s < ty_e && (*ty_s == ' ' || *ty_s == '\t' || *ty_s == '\n' || *ty_s == '\r')) ty_s++;
        while (ty_e > ty_s && (ty_e[-1] == ' ' || ty_e[-1] == '\t' || ty_e[-1] == '\n' || ty_e[-1] == '\r')) ty_e--;
        if (ty_e <= ty_s) return;
        {
            size_t type_len = (size_t)(ty_e - ty_s);
            if (type_len >= out_type_sz) type_len = out_type_sz - 1;
            memcpy(out_type, ty_s, type_len);
            out_type[type_len] = '\0';
        }
        if (name_n >= out_name_sz) name_n = out_name_sz - 1;
        memcpy(out_name, name_s, name_n);
        out_name[name_n] = '\0';
    }
}

#endif /* CC_UTIL_TEXT_H */
