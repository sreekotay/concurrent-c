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

/* ---- Directive-aware user-line accounting (the "line ledger") ----
 *
 * Mid-pipeline buffers carry a coordinate ledger: raw `#line N "path"`
 * directives surviving from earlier phases (e.g. the @grammar expansion
 * brackets) plus masked `CC_LN` same-line comment markers added by phase-3
 * passes (see cc__mask_line_directives / cc__unmask_line_directives in
 * visit_codegen.c).  Both forms occupy a whole line, and the line FOLLOWING
 * a ledger line is user line N of the named file.
 *
 * A raw physical newline count is therefore NOT a user line once any
 * expansion inflated the buffer (redis_idiomatic.ccs: @grammar adds ~266
 * lines above user line 571, so raw counts there run +266 and eventually
 * point past EOF).  Every pass that stamps user coordinates into markers
 * or `#line` directives must consult the ledger via this helper instead.
 *
 * Returns the user line (>= 1) of the line containing byte `off`.
 * `base_line` names the line buf[0] sits on: pass 1 for whole-TU buffers,
 * or a slice's first-line coordinate for sub-buffer parses.  When the
 * governing ledger entry names a path, out_path/out_path_len are set to
 * a slice INTO buf (not NUL-terminated); they are left untouched otherwise.
 *
 * Note: ledger lines are recognized by line shape alone (same policy as
 * cc__unmask_line_directives) — a string literal spelled to look like a
 * marker AND alone on its line is not defended against. */
static inline int cc_ledger_parse_line(const char* buf, size_t ls, size_t le,
                                       long* out_n, const char** out_path, size_t* out_path_len) {
    size_t ss = ls, q, num_start;
    while (ss < le && (buf[ss] == ' ' || buf[ss] == '\t')) ss++;
    /* masked marker: /;*CC_LN N PATH*;/ (sans semicolons), `*;/` at EOL */
    if (ss + 8 <= le && memcmp(buf + ss, "/*CC_LN ", 8) == 0 &&
        le >= ss + 2 && buf[le - 2] == '*' && buf[le - 1] == '/') {
        q = ss + 8;
        num_start = q;
        while (q < le && buf[q] >= '0' && buf[q] <= '9') q++;
        if (q == num_start) return 0;
        *out_n = strtol(buf + num_start, NULL, 10);
        if (q < le - 2 && buf[q] == ' ') {
            *out_path = buf + q + 1;
            *out_path_len = (le - 2) - (q + 1);
        }
        return (q == le - 2) || (q < le - 2 && buf[q] == ' ');
    }
    /* raw directive: # ws* line ws+ N [ws+ "PATH"] */
    if (ss < le && buf[ss] == '#') {
        q = ss + 1;
        while (q < le && (buf[q] == ' ' || buf[q] == '\t')) q++;
        if (q + 4 > le || memcmp(buf + q, "line", 4) != 0) return 0;
        q += 4;
        if (q < le && buf[q] != ' ' && buf[q] != '\t') return 0;
        while (q < le && (buf[q] == ' ' || buf[q] == '\t')) q++;
        num_start = q;
        while (q < le && buf[q] >= '0' && buf[q] <= '9') q++;
        if (q == num_start) return 0;
        *out_n = strtol(buf + num_start, NULL, 10);
        while (q < le && (buf[q] == ' ' || buf[q] == '\t')) q++;
        if (q < le && buf[q] == '"') {
            size_t ps = q + 1, pe = le;
            while (pe > ps && buf[pe - 1] != '"') pe--;
            if (pe > ps) {
                *out_path = buf + ps;
                *out_path_len = pe - 1 - ps;
            }
        }
        return 1;
    }
    return 0;
}

static inline int cc_user_line_for_offset(const char* buf, size_t len, size_t off, int base_line,
                                          const char** out_path, size_t* out_path_len) {
    int ln = (base_line > 0) ? base_line : 1;
    size_t i = 0;
    if (!buf) return ln;
    if (off > len) off = len;
    while (i < len) {
        size_t ls = i, le = i;
        long n = 0;
        const char* p = NULL;
        size_t pl = 0;
        while (le < len && buf[le] != '\n') le++;
        if (off <= le) break; /* off sits on this (possibly partial) line */
        if (cc_ledger_parse_line(buf, ls, le, &n, &p, &pl) && n > 0) {
            ln = (int)n; /* next physical line IS user line n */
            if (p && out_path && out_path_len) { *out_path = p; *out_path_len = pl; }
        } else {
            ln++;
        }
        i = le + 1;
    }
    return ln;
}

/* Diff-based ledger stamp for whole-buffer rewrites.  When a once-style
 * rewrite (old -> new) changed the net physical line count, every user
 * coordinate stamped below it by a later pass would drift; this returns a
 * malloc'd copy of `newb` with a masked CC_LN resync inserted at the start
 * of the line AFTER the changed region (NULL when the rewrite was
 * line-neutral, buffers are equal, or on OOM — caller keeps `newb`).
 * The marker's user line is computed from the OLD buffer's ledger, which
 * is self-consistent up to the change.  fallback_path is used when no
 * ledger entry governs the site. */
static inline char* cc_ledger_stamp_rewrite(const char* oldb, size_t oldn,
                                            const char* newb, size_t newn,
                                            const char* fallback_path,
                                            size_t* out_len) {
    size_t p = 0, s = 0, onl = 0, nnl = 0, e, ins, eo, b, mlen;
    const char* lp = NULL;
    size_t lpl = 0;
    int ul, mn;
    char mark[1152];
    char* out;
    if (!oldb || !newb || !out_len) return NULL;
    while (p < oldn && p < newn && oldb[p] == newb[p]) p++;
    if (p == oldn && p == newn) return NULL; /* identical */
    while (s < oldn - p && s < newn - p && oldb[oldn - 1 - s] == newb[newn - 1 - s]) s++;
    for (b = p; b < oldn - s; b++)
        if (oldb[b] == '\n') onl++;
    for (b = p; b < newn - s; b++)
        if (newb[b] == '\n') nnl++;
    if (onl == nnl) return NULL; /* line-neutral rewrite */
    /* Insertion point in NEW: start of the line after the changed region. */
    e = newn - s;
    while (e < newn && newb[e] != '\n') e++;
    ins = (e < newn) ? e + 1 : newn;
    /* User line of that next line, from the OLD buffer's ledger. */
    eo = oldn - s;
    while (eo < oldn && oldb[eo] != '\n') eo++;
    ul = cc_user_line_for_offset(oldb, oldn, eo, 1, &lp, &lpl);
    if (eo < oldn) ul++;
    if (lp && lpl > 0 && lpl < 1024) {
        mn = snprintf(mark, sizeof(mark), "/*CC_LN %d %.*s*/\n", ul, (int)lpl, lp);
    } else {
        mn = snprintf(mark, sizeof(mark), "/*CC_LN %d %s*/\n", ul,
                      (fallback_path && fallback_path[0]) ? fallback_path : "<cc_input>");
    }
    if (mn <= 0 || (size_t)mn >= sizeof(mark)) return NULL;
    mlen = (size_t)mn;
    out = (char*)malloc(newn + mlen + 1);
    if (!out) return NULL;
    memcpy(out, newb, ins);
    memcpy(out + ins, mark, mlen);
    memcpy(out + ins + mlen, newb + ins, newn - ins);
    out[newn + mlen] = 0;
    *out_len = newn + mlen;
    return out;
}

/* ---- Positional keyword match ----
 *
 * Word-bounded keyword recognizer used by every raw-text scanner that asks
 * "does identifier `kw` start exactly at `pos`?" (the @comptime intrinsic
 * scanners in emit_plan.c / symbols.c, sigil lowering passes, etc.).  A match
 * requires `kw` at `pos` AND that neither the char before nor after is an
 * identifier char, so `cc_instantiate_vec` does not match inside
 * `my_cc_instantiate_vector`.  Returns 1 on match, 0 otherwise. */
static inline int cc_match_ident_kw(const char* src, size_t len, size_t pos, const char* kw) {
    size_t klen;
    if (!src || !kw) return 0;
    klen = strlen(kw);
    if (klen == 0 || pos + klen > len) return 0;
    if (memcmp(src + pos, kw, klen) != 0) return 0;
    if (pos > 0 && cc_is_ident_char(src[pos - 1])) return 0;
    if (pos + klen < len && cc_is_ident_char(src[pos + klen])) return 0;
    return 1;
}

/* ---- @comptime block recognizer ----
 *
 * If `src[at]` begins a `@comptime { ... }` block, set `*out_body_l` to the
 * index of the opening `{` and `*out_body_r` to its matching `}`, and return
 * 1; otherwise return 0.  Whitespace and comments between `comptime` and the
 * `{` are skipped.  This is the single definition of "what is a comptime
 * block, and where does its body span" shared by every comptime-intrinsic
 * scanner (emit_plan.c, symbols.c).  Forward-declared helpers used here are
 * defined below in this header. */
static inline size_t cc_skip_ws_and_comments(const char* src, size_t len, size_t i);
static inline int cc_find_matching_brace(const char* b, size_t bl, size_t lbrace, size_t* out_rbrace);

static inline int cc_match_comptime_block(const char* src, size_t len, size_t at,
                                          size_t* out_body_l, size_t* out_body_r) {
    size_t body_l, body_r = 0;
    if (!src || at >= len || src[at] != '@') return 0;
    if (!cc_match_ident_kw(src, len, at + 1, "comptime")) return 0;
    body_l = cc_skip_ws_and_comments(src, len, at + 1 + (sizeof("comptime") - 1));
    if (body_l >= len || src[body_l] != '{') return 0;
    if (!cc_find_matching_brace(src, len, body_l, &body_r)) return 0;
    if (out_body_l) *out_body_l = body_l;
    if (out_body_r) *out_body_r = body_r;
    return 1;
}

/* ---- C string-literal parse ----
 *
 * Decode a `"..."` literal starting at `*io_pos` (which must point at the
 * opening quote) into `out`, advancing `*io_pos` past the closing quote.
 * Recognizes the common escapes (`\n \t \r \\ \" \0`); any other escaped
 * char is taken literally.  Overlong content is truncated to `out_cap-1`
 * (the scan still consumes through the closing quote).  Returns 1 on a
 * well-formed literal, 0 otherwise.  This is the single decoder shared by
 * the @comptime intrinsic scanners (string-literal args like type names,
 * instantiation operands, and `cc_emit_cstr` fragments). */
static inline int cc_parse_c_string_literal(const char* src, size_t len, size_t* io_pos,
                                            char* out, size_t out_cap) {
    size_t i, o = 0;
    if (!src || !io_pos || !out || out_cap == 0) return 0;
    i = *io_pos;
    if (i >= len || src[i] != '"') return 0;
    i++;
    while (i < len) {
        char c = src[i++];
        if (c == '"') {
            out[o < out_cap ? o : out_cap - 1] = '\0';
            *io_pos = i;
            return 1;
        }
        if (c == '\\' && i < len) {
            char e = src[i++];
            switch (e) {
            case 'n': c = '\n'; break;
            case 't': c = '\t'; break;
            case 'r': c = '\r'; break;
            case '\\': c = '\\'; break;
            case '"': c = '"'; break;
            case '0': c = '\0'; break;
            default: c = e; break;
            }
        }
        if (o + 1 < out_cap) out[o++] = c;
    }
    return 0;
}

/* ---- Whitespace helpers ---- */

static inline const char* cc_skip_ws(const char* s) {
    if (!s) return s;
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') s++;
    return s;
}

/* No comment-blind forward skip: every inter-token gap in CC syntax can
 * hold a comment, so use `cc_skip_ws_and_comments` below. */

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
        /* A backtick template is one lexical region: its text is not C, so
         * braces, quotes and apostrophes inside it must not count.  Backtick
         * is not a C token, so skipping here is unambiguous. */
        if (ch == '`') {
            p++;
            while (p < bl && b[p] != '`') { if (b[p] == '\\' && p + 1 < bl) p++; p++; }
            continue;
        }
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
        /* A backtick template is one lexical region: its text is not C, so
         * braces, quotes and apostrophes inside it must not count.  Backtick
         * is not a C token, so skipping here is unambiguous. */
        if (ch == '`') {
            p++;
            while (p < bl && b[p] != '`') { if (b[p] == '\\' && p + 1 < bl) p++; p++; }
            continue;
        }
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
        /* A backtick template is one lexical region: its text is not C, so
         * braces, quotes and apostrophes inside it must not count.  Backtick
         * is not a C token, so skipping here is unambiguous. */
        if (ch == '`') {
            p++;
            while (p < bl && b[p] != '`') { if (b[p] == '\\' && p + 1 < bl) p++; p++; }
            continue;
        }
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

/* Backward analogue of `cc_skip_ws_and_comments`.  `pos` is an EXCLUSIVE
 * end: the function returns the largest `p <= pos` such that `src[p-1]`
 * is the previous real-code byte (or 0 if none), skipping whitespace,
 * `/ * ... * /` block comments and `// ...` line comments while walking
 * left.  Callers that used `while (p > 0 && isspace(src[p-1])) p--;`
 * before probing the previous token can substitute this to keep the
 * probe honest across an intervening comment.
 *
 * Block comments are rewound with the established backward idiom (find
 * the terminating `* /`, search left for the opening `/ *`); adjacent
 * `* /` cannot appear in real C code outside a comment, so the entry
 * test is safe.  Line comments are forward-verified from the start of
 * the physical line (string/char literals on the line respected), which
 * is the same fidelity as `cc_scan_pos_in_line_comment`. */
static inline size_t cc_rskip_ws_and_comments(const char* src, size_t pos) {
    if (!src) return pos;
    for (;;) {
        while (pos > 0) {
            char c = src[pos - 1];
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n' ||
                c == '\f' || c == '\v') { pos--; continue; }
            break;
        }
        if (pos == 0) return 0;
        if (src[pos - 1] == '/' && pos >= 2 && src[pos - 2] == '*') {
            /* Terminating `* /`: rewind to the matching opener. */
            size_t k = pos - 2;
            size_t open = (size_t)-1;
            while (k > 0) {
                k--;
                if (src[k] == '*' && k > 0 && src[k - 1] == '/') { open = k - 1; break; }
            }
            if (open == (size_t)-1) return pos; /* unterminated: treat as code */
            pos = open;
            continue;
        }
        /* Tail of a `// ...` line comment?  Forward-verify from the
         * physical line start; stop at the first `//` that is outside
         * string/char literals and outside a same-line block comment. */
        {
            size_t ls = pos - 1;
            size_t lc = (size_t)-1;
            int ins = 0; char q = 0;
            size_t k;
            while (ls > 0 && src[ls - 1] != '\n') ls--;
            k = ls;
            while (k < pos) {
                char c = src[k];
                if (ins) {
                    if (c == '\\' && k + 1 < pos) { k += 2; continue; }
                    if (c == q) ins = 0;
                    k++;
                    continue;
                }
                if (c == '"' || c == '\'') { ins = 1; q = c; k++; continue; }
                if (c == '/' && k + 1 < pos && src[k + 1] == '/') { lc = k; break; }
                if (c == '/' && k + 1 < pos && src[k + 1] == '*') {
                    /* Same-line block comment: hop over it (terminated
                     * before `pos`) or stop scanning (still open at
                     * `pos`, which callers walking back from real code
                     * never hit). */
                    size_t m = k + 2;
                    while (m + 1 < pos && !(src[m] == '*' && src[m + 1] == '/')) m++;
                    if (m + 1 < pos) { k = m + 2; continue; }
                    break;
                }
                k++;
            }
            if (lc != (size_t)-1) { pos = lc; continue; }
        }
        return pos;
    }
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
        if (c == '[') {
            if (brack_depth > 0) { brack_depth--; continue; }
            /* Unmatched opening `[` to the left: we've reached the enclosing
             * bracket context (e.g. a `Name::[T[:], U]` generic-arg list, where
             * the element type is `T[:]`).  Stop here — the type/declarator
             * starts just after it.  Mirrors the `(` case above; without this
             * a backward type scan crosses the `::[` and swallows the container
             * head (`Map::[char` -> `CCSlice`). */
            hit = i + 1;
            break;
        }
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
    if (nl == 0 || nl > len) return 0;
    /* Fast reject: if the literal never appears, skip the inert comment/string
     * walk. Callers use this as an early-out on large buffers (include-expanded
     * registration text); paying a full inert scan when the needle is absent
     * was almost as expensive as the rewrite it was meant to avoid. */
    {
        int present = 0;
        for (size_t i = 0; i + nl <= len; ++i) {
            if (src[i] == needle[0] && memcmp(src + i, needle, nl) == 0) {
                present = 1;
                break;
            }
        }
        if (!present) return 0;
    }
    return cc_find_substr_top_level(src, 0, len, needle, nl) < len;
}

/* ---- One-pass surface-feature bitmap ----
 *
 * Phase-3 / preprocess presence gates used to call
 * `cc_contains_token_top_level` once per needle on the same buffer.  Each call
 * re-walks the TU (or at least memmem's it).  Suite wall time is dominated by
 * many small TUs, so the fixed per-check tax adds up.  Scan once; test bits.
 *
 * Bits are set for matches outside comments/strings (same policy as
 * `cc_contains_token_top_level`).  False positives are safe for gates that
 * only skip work; keep the denser rewrite path for correctness. */
enum {
    CC_SURF_AWAIT              = 1ull << 0,
    CC_SURF_AT_ASYNC           = 1ull << 1,
    CC_SURF_AT_BLOCKING        = 1ull << 2,
    CC_SURF_AT_NOBLOCK         = 1ull << 3,
    CC_SURF_AT_NONBLOCKING     = 1ull << 4,
    CC_SURF_FAT_ARROW          = 1ull << 5,  /* => */
    CC_SURF_BANG_GT            = 1ull << 6,  /* !> */
    CC_SURF_Q_GT               = 1ull << 7,  /* ?> */
    CC_SURF_AT_DEFER           = 1ull << 8,
    CC_SURF_CANCEL             = 1ull << 9,
    CC_SURF_SEND_TASK          = 1ull << 10,
    CC_SURF_CC_CHANNEL_SEND_TASK = 1ull << 11,
    CC_SURF_CCCLOSURE          = 1ull << 12,
    CC_SURF_AT_DESTROY         = 1ull << 13,
    CC_SURF_AT_ERRHANDLER      = 1ull << 14,
    CC_SURF_AT_ERR             = 1ull << 15,
    CC_SURF_EQ_LT_BANG         = 1ull << 16, /* =<! */
    CC_SURF_LT_Q               = 1ull << 17, /* <? */
    CC_SURF_CCSLICE_FAMILY     = 1ull << 18, /* CCSlice / Shared / Unique */
    CC_SURF_CC_PATH_IO         = 1ull << 19, /* cc_path_ / cc_file_ / cc_dir_ / cc_glob / cc_command / cc_sh_run */
    CC_SURF_AT_MATCH           = 1ull << 20,
    CC_SURF_WITH_DEADLINE      = 1ull << 21,
    CC_SURF_AT_LINK            = 1ull << 22,
    CC_SURF_CC_CONCURRENT      = 1ull << 23,
    CC_SURF_AT_STRING          = 1ull << 24,
    CC_SURF_AT_SLICE           = 1ull << 25,
    CC_SURF_AT_EMIT            = 1ull << 26,
    CC_SURF_AT_SCRATCH         = 1ull << 27,
    CC_SURF_TYPE_OF            = 1ull << 28,
    CC_SURF_AT_AWAIT           = 1ull << 29,
    CC_SURF_AT_WITH_DEADLINE   = 1ull << 30,
    CC_SURF_AT_PARALLEL        = 1ull << 31
};

static inline int cc__surf_match_at(const char* src, size_t len, size_t p,
                                    const char* lit, size_t lit_len, int word) {
    if (p + lit_len > len) return 0;
    if (memcmp(src + p, lit, lit_len) != 0) return 0;
    if (word) {
        if (p > 0 && cc_is_ident_char(src[p - 1])) return 0;
        if (p + lit_len < len && cc_is_ident_char(src[p + lit_len])) return 0;
    }
    return 1;
}

/* Like word-match but only left-bounded: `CCClosure1` / `CCSliceShared` still
 * count as the `CCClosure` / `CCSlice` family (matches contains_token_top_level
 * substring policy for these prefixes). */
static inline int cc__surf_match_prefix(const char* src, size_t len, size_t p,
                                        const char* lit, size_t lit_len) {
    if (p + lit_len > len) return 0;
    if (memcmp(src + p, lit, lit_len) != 0) return 0;
    if (p > 0 && cc_is_ident_char(src[p - 1])) return 0;
    return 1;
}

static inline unsigned long long cc_scan_surface_features(const char* src, size_t len) {
    unsigned long long bits = 0;
    int in_lc = 0, in_bc = 0, ins = 0;
    char q = 0;
    if (!src || len == 0) return 0;
    for (size_t p = 0; p < len; p++) {
        char c = src[p];
        char c2 = (p + 1 < len) ? src[p + 1] : 0;
        if (in_lc) { if (c == '\n') in_lc = 0; continue; }
        if (in_bc) { if (c == '*' && c2 == '/') { in_bc = 0; p++; } continue; }
        if (ins) {
            if (c == '\\' && p + 1 < len) { p++; continue; }
            if (c == q) ins = 0;
            continue;
        }
        if (c == '/' && c2 == '/') { in_lc = 1; p++; continue; }
        if (c == '/' && c2 == '*') { in_bc = 1; p++; continue; }
        if (c == '"' || c == '\'') { ins = 1; q = c; continue; }

        if (c == '=' && c2 == '>') {
            bits |= CC_SURF_FAT_ARROW;
            p++;
            continue;
        }
        if (c == '!' && c2 == '>') {
            bits |= CC_SURF_BANG_GT;
            p++;
            continue;
        }
        if (c == '?' && c2 == '>') {
            bits |= CC_SURF_Q_GT;
            p++;
            continue;
        }
        if (c == '<' && c2 == '?') {
            bits |= CC_SURF_LT_Q;
            p++;
            continue;
        }
        if (c == '=' && c2 == '<' && p + 2 < len && src[p + 2] == '!') {
            bits |= CC_SURF_EQ_LT_BANG;
            p += 2;
            continue;
        }
        if (c == '@') {
            if (cc__surf_match_at(src, len, p, "@async", 6, 1)) bits |= CC_SURF_AT_ASYNC;
            else if (cc__surf_match_at(src, len, p, "@blocking", 9, 1)) bits |= CC_SURF_AT_BLOCKING;
            else if (cc__surf_match_at(src, len, p, "@noblock", 8, 1)) bits |= CC_SURF_AT_NOBLOCK;
            else if (cc__surf_match_at(src, len, p, "@nonblocking", 12, 1)) bits |= CC_SURF_AT_NONBLOCKING;
            else if (cc__surf_match_at(src, len, p, "@defer", 6, 1)) bits |= CC_SURF_AT_DEFER;
            else if (cc__surf_match_at(src, len, p, "@destroy", 8, 1)) bits |= CC_SURF_AT_DESTROY;
            else if (cc__surf_match_at(src, len, p, "@errhandler", 11, 1)) bits |= CC_SURF_AT_ERRHANDLER;
            else if (cc__surf_match_at(src, len, p, "@err", 4, 1)) bits |= CC_SURF_AT_ERR;
            else if (cc__surf_match_at(src, len, p, "@match", 6, 1)) bits |= CC_SURF_AT_MATCH;
            else if (cc__surf_match_at(src, len, p, "@link", 5, 1)) bits |= CC_SURF_AT_LINK;
            else if (cc__surf_match_at(src, len, p, "@string", 7, 1)) bits |= CC_SURF_AT_STRING;
            else if (cc__surf_match_at(src, len, p, "@slice", 6, 1)) bits |= CC_SURF_AT_SLICE;
            else if (cc__surf_match_at(src, len, p, "@emit", 5, 1)) bits |= CC_SURF_AT_EMIT;
            else if (cc__surf_match_at(src, len, p, "@scratch", 8, 1)) bits |= CC_SURF_AT_SCRATCH;
            else if (cc__surf_match_at(src, len, p, "@await", 6, 1)) bits |= CC_SURF_AT_AWAIT;
            else if (cc__surf_match_at(src, len, p, "@with_deadline", 14, 1))
                bits |= CC_SURF_AT_WITH_DEADLINE;
            else if (cc__surf_match_at(src, len, p, "@parallel", 9, 1))
                bits |= CC_SURF_AT_PARALLEL;
            continue;
        }
        if (c == 'a' && cc__surf_match_at(src, len, p, "await", 5, 1)) {
            bits |= CC_SURF_AWAIT;
            continue;
        }
        if (c == 'c') {
            if (cc__surf_match_at(src, len, p, "cancel", 6, 1)) bits |= CC_SURF_CANCEL;
            else if (cc__surf_match_prefix(src, len, p, "cc_channel_send_task", 20))
                bits |= CC_SURF_CC_CHANNEL_SEND_TASK; /* also *_hybrid */
            else if (cc__surf_match_at(src, len, p, "cc_concurrent", 13, 1))
                bits |= CC_SURF_CC_CONCURRENT;
            else if (cc__surf_match_at(src, len, p, "cc_path_exists", 14, 1) ||
                     cc__surf_match_at(src, len, p, "cc_path_is_dir", 14, 1) ||
                     cc__surf_match_at(src, len, p, "cc_path_is_file", 15, 1) ||
                     cc__surf_match_at(src, len, p, "cc_path_join", 12, 1) ||
                     cc__surf_match_at(src, len, p, "cc_script_path_join", 19, 1) ||
                     cc__surf_match_at(src, len, p, "cc_file_open", 12, 1) ||
                     cc__surf_match_at(src, len, p, "cc_file_read_path", 17, 1) ||
                     cc__surf_match_at(src, len, p, "cc_file_write_path", 18, 1) ||
                     cc__surf_match_at(src, len, p, "cc_dir_open", 11, 1) ||
                     cc__surf_match_at(src, len, p, "cc_glob", 7, 1) ||
                     cc__surf_match_at(src, len, p, "cc_command", 10, 1) ||
                     cc__surf_match_at(src, len, p, "cc_sh_run", 9, 1) ||
                     cc__surf_match_at(src, len, p, "cc_type_of", 10, 1)) {
                if (cc__surf_match_at(src, len, p, "cc_type_of", 10, 1))
                    bits |= CC_SURF_TYPE_OF;
                else
                    bits |= CC_SURF_CC_PATH_IO;
            }
            continue;
        }
        if (c == 's' && cc__surf_match_prefix(src, len, p, "send_task", 9)) {
            bits |= CC_SURF_SEND_TASK; /* also send_task_hybrid */
            continue;
        }
        if (c == 'w' && cc__surf_match_at(src, len, p, "with_deadline", 13, 1)) {
            bits |= CC_SURF_WITH_DEADLINE;
            continue;
        }
        if (c == 't' && cc__surf_match_at(src, len, p, "type_of", 7, 1)) {
            bits |= CC_SURF_TYPE_OF;
            continue;
        }
        if (c == 'C') {
            if (cc__surf_match_prefix(src, len, p, "CCClosure", 9))
                bits |= CC_SURF_CCCLOSURE;
            else if (cc__surf_match_prefix(src, len, p, "CCSlice", 7))
                bits |= CC_SURF_CCSLICE_FAMILY; /* CCSlice / Shared / Unique */
            continue;
        }
    }
    return bits;
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
static inline void cc_sb_append_c_string_literal_max(char** buf, size_t* len, size_t* cap,
                                                      const char* src, size_t a, size_t b,
                                                      size_t vis_max) {
    if (!buf || !len || !cap) return;
    cc_sb_append(buf, len, cap, "\"", 1);
    if (src && b > a) {
        char tmp[256];
        size_t w = 0;
        int prev_space = 0;
        int truncated = 0;
        if (vis_max == 0 || vis_max > 160) vis_max = 160;
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

static inline void cc_sb_append_c_string_literal(char** buf, size_t* len, size_t* cap,
                                                  const char* src, size_t a, size_t b) {
    cc_sb_append_c_string_literal_max(buf, len, cap, src, a, b, 160);
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
    /* The expression string only situates the failure in the message —
     * file:line carry the precise diagnostic — so keep it short. */
    cc_sb_append_c_string_literal_max(buf, len, cap, src, a, b, 64);
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
    /* Short expression string: it is unused for Result temps and only
     * situates the pointer-arm message — file:line are the diagnostic. */
    cc_sb_append_c_string_literal_max(buf, len, cap, src, a, b, 64);
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
    if (!type_name || !type_name[0]) return 0;
    if (strcmp(type_name, "return") == 0 ||
        strcmp(type_name, "break") == 0 ||
        strcmp(type_name, "continue") == 0 ||
        strcmp(type_name, "goto") == 0 ||
        strcmp(type_name, "case") == 0 ||
        strcmp(type_name, "default") == 0) {
        return 1;
    }
    /* Call / address-of fragments misparsed as types, e.g.
     * `cc_listener_close(&ln);` → type=`cc_listener_close(&`, name=`ln`. */
    for (const char* p = type_name; *p; ++p) {
        if (*p == '(' || *p == ')' || *p == '&' || *p == '=') return 1;
    }
    return 0;
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
/* Strip field attribute `@as` (and legacy slash-star @as star-slash markers).
 * Attributes are AST / reflect-table facts — they must not appear in host C or
 * in anything fed to generic TCC. Comptime `f.is_as` is harvested from the
 * pre-strip Concurrent-C source (or emitted tables), never recovered from
 * comment markers. Skips comments and string/char literals. Returns malloc'd
 * buffer or NULL when unchanged / OOM.
 *
 * Name kept for call-site stability; behavior is erase, not comment-encode. */
static inline char* cc_rewrite_as_attr_to_comment(const char* src, size_t n) {
    size_t i = 0;
    size_t out_cap;
    size_t w = 0;
    char* out;
    int changed = 0;
    if (!src || n == 0) return NULL;
    out_cap = n + 8;
    out = (char*)malloc(out_cap + 1);
    if (!out) return NULL;
    while (i < n) {
        if (src[i] == '"' || src[i] == '\'') {
            char q = src[i++];
            if (w + 1 > out_cap) {
                size_t nc = out_cap * 2 + 64;
                char* nb = (char*)realloc(out, nc + 1);
                if (!nb) { free(out); return NULL; }
                out = nb; out_cap = nc;
            }
            out[w++] = q;
            while (i < n) {
                if (w + 2 > out_cap) {
                    size_t nc = out_cap * 2 + 64;
                    char* nb = (char*)realloc(out, nc + 1);
                    if (!nb) { free(out); return NULL; }
                    out = nb; out_cap = nc;
                }
                out[w++] = src[i];
                if (src[i] == '\\' && i + 1 < n) { out[w++] = src[i + 1]; i += 2; continue; }
                if (src[i] == q) { i++; break; }
                i++;
            }
            continue;
        }
        if (i + 1 < n && src[i] == '/' && src[i + 1] == '/') {
            while (i < n && src[i] != '\n') {
                if (w + 1 > out_cap) {
                    size_t nc = out_cap * 2 + 64;
                    char* nb = (char*)realloc(out, nc + 1);
                    if (!nb) { free(out); return NULL; }
                    out = nb; out_cap = nc;
                }
                out[w++] = src[i++];
            }
            continue;
        }
        if (i + 1 < n && src[i] == '/' && src[i + 1] == '*') {
            /* Drop legacy product marker entirely (was a semantic channel). */
            if (i + 7 <= n && memcmp(src + i, "/*@as*/", 7) == 0) {
                i += 7;
                changed = 1;
                continue;
            }
            while (i + 1 < n && !(src[i] == '*' && src[i + 1] == '/')) {
                if (w + 1 > out_cap) {
                    size_t nc = out_cap * 2 + 64;
                    char* nb = (char*)realloc(out, nc + 1);
                    if (!nb) { free(out); return NULL; }
                    out = nb; out_cap = nc;
                }
                out[w++] = src[i++];
            }
            if (i + 1 < n) {
                if (w + 2 > out_cap) {
                    size_t nc = out_cap * 2 + 64;
                    char* nb = (char*)realloc(out, nc + 1);
                    if (!nb) { free(out); return NULL; }
                    out = nb; out_cap = nc;
                }
                out[w++] = src[i++];
                out[w++] = src[i++];
            }
            continue;
        }
        if (i + 3 <= n && src[i] == '@' && src[i + 1] == 'a' && src[i + 2] == 's' &&
            (i + 3 == n || !cc_is_ident_char(src[i + 3]))) {
            /* Erase `@as` and a single preceding space if present. */
            if (w > 0 && (out[w - 1] == ' ' || out[w - 1] == '\t')) w--;
            i += 3;
            changed = 1;
            continue;
        }
        if (w + 1 > out_cap) {
            size_t nc = out_cap * 2 + 64;
            char* nb = (char*)realloc(out, nc + 1);
            if (!nb) { free(out); return NULL; }
            out = nb; out_cap = nc;
        }
        out[w++] = src[i++];
    }
    out[w] = '\0';
    if (!changed) { free(out); return NULL; }
    return out;
}

/* Erase `@typeview` define blocks from lowered `.h` text.
 * Faces and allow-lists are Concurrent-C AST facts (like `as:`); host C must
 * not see them. Skips comments and string/char literals. Forms:
 *   @typeview on Base { … };
 *   @typeview Mode on Base { … };
 *   typedef @typeview Mode on Base { … } Alias;
 * For the typedef form, emits `typedef Base_Restrict_Mode Alias` (optional `*`).
 * Sugar `@typeview(Mode) Base` is rewritten to `Base_Restrict_Mode`.
 * Returns malloc'd buffer or NULL when unchanged / OOM. */
static inline char* cc_strip_typeview_blocks(const char* src, size_t n) {
    size_t i = 0;
    size_t out_cap;
    size_t w = 0;
    char* out;
    int changed = 0;
    if (!src || n == 0) return NULL;
    out_cap = n + 8;
    out = (char*)malloc(out_cap + 1);
    if (!out) return NULL;
    while (i < n) {
        if (src[i] == '"' || src[i] == '\'') {
            char q = src[i++];
            if (w + 1 > out_cap) {
                size_t nc = out_cap * 2 + 64;
                char* nb = (char*)realloc(out, nc + 1);
                if (!nb) { free(out); return NULL; }
                out = nb; out_cap = nc;
            }
            out[w++] = q;
            while (i < n) {
                if (w + 2 > out_cap) {
                    size_t nc = out_cap * 2 + 64;
                    char* nb = (char*)realloc(out, nc + 1);
                    if (!nb) { free(out); return NULL; }
                    out = nb; out_cap = nc;
                }
                out[w++] = src[i];
                if (src[i] == '\\' && i + 1 < n) {
                    out[w++] = src[i + 1];
                    i += 2;
                    continue;
                }
                if (src[i] == q) { i++; break; }
                i++;
            }
            continue;
        }
        if (i + 1 < n && src[i] == '/' && src[i + 1] == '/') {
            while (i < n && src[i] != '\n') {
                if (w + 1 > out_cap) {
                    size_t nc = out_cap * 2 + 64;
                    char* nb = (char*)realloc(out, nc + 1);
                    if (!nb) { free(out); return NULL; }
                    out = nb; out_cap = nc;
                }
                out[w++] = src[i++];
            }
            continue;
        }
        if (i + 1 < n && src[i] == '/' && src[i + 1] == '*') {
            while (i + 1 < n && !(src[i] == '*' && src[i + 1] == '/')) {
                if (w + 1 > out_cap) {
                    size_t nc = out_cap * 2 + 64;
                    char* nb = (char*)realloc(out, nc + 1);
                    if (!nb) { free(out); return NULL; }
                    out = nb; out_cap = nc;
                }
                out[w++] = src[i++];
            }
            if (i + 1 < n) {
                if (w + 2 > out_cap) {
                    size_t nc = out_cap * 2 + 64;
                    char* nb = (char*)realloc(out, nc + 1);
                    if (!nb) { free(out); return NULL; }
                    out = nb; out_cap = nc;
                }
                out[w++] = src[i++];
                out[w++] = src[i++];
            }
            continue;
        }
        /* Sugar: @typeview(Mode) Base */
        if (src[i] == '@' &&
            i + 9 <= n && memcmp(src + i, "@typeview", 9) == 0 &&
            (i + 9 == n || !cc_is_ident_char(src[i + 9]))) {
            size_t kw = 9;
            size_t p = i + kw;
            size_t mode_l, mode_r, base_l, base_r;
            while (p < n && (src[p] == ' ' || src[p] == '\t' || src[p] == '\n' ||
                             src[p] == '\r'))
                p++;
            if (p < n && src[p] == '(') {
                p++;
                while (p < n && (src[p] == ' ' || src[p] == '\t')) p++;
                mode_l = p;
                while (p < n && cc_is_ident_char(src[p])) p++;
                mode_r = p;
                while (p < n && (src[p] == ' ' || src[p] == '\t')) p++;
                if (p < n && src[p] == ')' && mode_r > mode_l) {
                    p++;
                    while (p < n && (src[p] == ' ' || src[p] == '\t' ||
                                     src[p] == '\n' || src[p] == '\r'))
                        p++;
                    base_l = p;
                    while (p < n && cc_is_ident_char(src[p])) p++;
                    base_r = p;
                    if (base_r > base_l) {
                        size_t need = (base_r - base_l) + 10 + (mode_r - mode_l) + 8;
                        if (w + need > out_cap) {
                            size_t nc = out_cap * 2 + need + 64;
                            char* nb = (char*)realloc(out, nc + 1);
                            if (!nb) { free(out); return NULL; }
                            out = nb; out_cap = nc;
                        }
                        memcpy(out + w, src + base_l, base_r - base_l);
                        w += base_r - base_l;
                        memcpy(out + w, "_Restrict_", 10);
                        w += 10;
                        memcpy(out + w, src + mode_l, mode_r - mode_l);
                        w += mode_r - mode_l;
                        i = p;
                        changed = 1;
                        continue;
                    }
                }
            }
            /* Define / typedef form: @kw [Mode] on Base { … } [;] [Alias] */
            {
                size_t scan = p;
                size_t mode_l2 = 0, mode_r2 = 0, base_l2 = 0, base_r2 = 0;
                size_t brace_l = 0, brace_r = 0;
                int depth = 0;
                int ls = 0, lc = 0, st = 0, ch = 0;
                size_t typedef_at = 0;
                /* Detect `typedef` immediately before `@`. */
                {
                    size_t b = i;
                    while (b > 0 && (src[b - 1] == ' ' || src[b - 1] == '\t' ||
                                     src[b - 1] == '\n' || src[b - 1] == '\r'))
                        b--;
                    if (b >= 7 && memcmp(src + b - 7, "typedef", 7) == 0 &&
                        (b == 7 || !cc_is_ident_char(src[b - 8]))) {
                        typedef_at = b - 7;
                        while (w > 0 && (out[w - 1] == ' ' || out[w - 1] == '\t' ||
                                         out[w - 1] == '\n' || out[w - 1] == '\r'))
                            w--;
                        if (w >= 7 && memcmp(out + w - 7, "typedef", 7) == 0)
                            w -= 7;
                        while (w > 0 && (out[w - 1] == ' ' || out[w - 1] == '\t'))
                            w--;
                    }
                }
                /* Unnamed: `on Base` — named: `Mode on Base`. */
                if (scan + 2 < n && src[scan] == 'o' && src[scan + 1] == 'n' &&
                    !cc_is_ident_char(src[scan + 2])) {
                    scan += 2;
                } else if (scan < n && cc_is_ident_char(src[scan])) {
                    mode_l2 = scan;
                    while (scan < n && cc_is_ident_char(src[scan])) scan++;
                    mode_r2 = scan;
                    while (scan < n && (src[scan] == ' ' || src[scan] == '\t' ||
                                        src[scan] == '\n' || src[scan] == '\r'))
                        scan++;
                    if (!(scan + 2 < n && src[scan] == 'o' && src[scan + 1] == 'n' &&
                          !cc_is_ident_char(src[scan + 2]))) {
                        /* not a define form */
                        mode_l2 = mode_r2 = 0;
                        scan = p;
                    } else {
                        scan += 2;
                    }
                }
                while (scan < n && (src[scan] == ' ' || src[scan] == '\t' ||
                                    src[scan] == '\n' || src[scan] == '\r'))
                    scan++;
                base_l2 = scan;
                while (scan < n && cc_is_ident_char(src[scan])) scan++;
                /* Trailing glob `*` on subject (`CCSlice_*`). */
                if (scan < n && src[scan] == '*') scan++;
                base_r2 = scan;
                while (scan < n && (src[scan] == ' ' || src[scan] == '\t' ||
                                    src[scan] == '\n' || src[scan] == '\r'))
                    scan++;
                if (scan < n && src[scan] == '{' && base_r2 > base_l2) {
                    brace_l = scan;
                    for (; scan < n; scan++) {
                        char d = src[scan];
                        char d2 = (scan + 1 < n) ? src[scan + 1] : 0;
                        if (lc) { if (d == '\n') lc = 0; continue; }
                        if (ls) {
                            if (d == '*' && d2 == '/') { ls = 0; scan++; }
                            continue;
                        }
                        if (st) {
                            if (d == '\\' && d2) { scan++; continue; }
                            if (d == '"') st = 0;
                            continue;
                        }
                        if (ch) {
                            if (d == '\\' && d2) { scan++; continue; }
                            if (d == '\'') ch = 0;
                            continue;
                        }
                        if (d == '/' && d2 == '/') { lc = 1; scan++; continue; }
                        if (d == '/' && d2 == '*') { ls = 1; scan++; continue; }
                        if (d == '"') { st = 1; continue; }
                        if (d == '\'') { ch = 1; continue; }
                        if (d == '{') {
                            depth++;
                            continue;
                        }
                        if (d == '}') {
                            if (depth == 0) break;
                            depth--;
                            if (depth == 0) {
                                brace_r = scan;
                                break;
                            }
                        }
                    }
                    if (brace_r > brace_l) {
                        size_t end = brace_r + 1;
                        while (end < n && (src[end] == ' ' || src[end] == '\t' ||
                                           src[end] == '\n' || src[end] == '\r'))
                            end++;
                        /* typedef @typeview Mode on Base {…} Alias; */
                        if (typedef_at && mode_r2 > mode_l2) {
                            size_t al = end, ar;
                            int stars = 0;
                            while (al < n && (src[al] == ' ' || src[al] == '\t' ||
                                              src[al] == '\n' || src[al] == '\r' ||
                                              src[al] == '*')) {
                                if (src[al] == '*') stars = 1;
                                al++;
                            }
                            ar = al;
                            while (ar < n && cc_is_ident_char(src[ar])) ar++;
                            if (ar > al) {
                                size_t need = 16 + (base_r2 - base_l2) + 10 +
                                              (mode_r2 - mode_l2) + (ar - al) + 4;
                                if (w + need > out_cap) {
                                    size_t nc = out_cap * 2 + need + 64;
                                    char* nb = (char*)realloc(out, nc + 1);
                                    if (!nb) { free(out); return NULL; }
                                    out = nb; out_cap = nc;
                                }
                                memcpy(out + w, "typedef ", 8);
                                w += 8;
                                memcpy(out + w, src + base_l2, base_r2 - base_l2);
                                w += base_r2 - base_l2;
                                memcpy(out + w, "_Restrict_", 10);
                                w += 10;
                                memcpy(out + w, src + mode_l2, mode_r2 - mode_l2);
                                w += mode_r2 - mode_l2;
                                if (stars) out[w++] = '*';
                                out[w++] = ' ';
                                memcpy(out + w, src + al, ar - al);
                                w += ar - al;
                                end = ar;
                                while (end < n &&
                                       (src[end] == ' ' || src[end] == '\t'))
                                    end++;
                                if (end < n && src[end] == ';') {
                                    out[w++] = ';';
                                    end++;
                                }
                                i = end;
                                changed = 1;
                                continue;
                            }
                        }
                        if (end < n && src[end] == ';') end++;
                        /* Plain define: erase entirely. */
                        i = end;
                        changed = 1;
                        continue;
                    }
                }
            }
            /* Fall through: copy `@` and continue (malformed stays visible). */
        }
        if (w + 1 > out_cap) {
            size_t nc = out_cap * 2 + 64;
            char* nb = (char*)realloc(out, nc + 1);
            if (!nb) { free(out); return NULL; }
            out = nb; out_cap = nc;
        }
        out[w++] = src[i++];
    }
    out[w] = '\0';
    if (!changed) { free(out); return NULL; }
    return out;
}

/* Collect field names from `as:` groups in a @typeview body. */
static inline int cc_typeview_body_as_names(const char* body, size_t n,
                                            char names[][64], int cap) {
    size_t i = 0;
    int in_as = 0;
    int nn = 0;
    if (!body || !names || cap <= 0) return 0;
    while (i < n) {
        char tok[64];
        size_t t = 0;
        while (i < n && (body[i] == ' ' || body[i] == '\t' || body[i] == '\n' ||
                         body[i] == '\r' || body[i] == '{' || body[i] == '}' ||
                         body[i] == ';' || body[i] == ','))
            i++;
        if (i >= n) break;
        if (body[i] == ':') {
            i++;
            continue;
        }
        if (i + 1 < n && body[i] == '/' && body[i + 1] == '/') {
            while (i < n && body[i] != '\n') i++;
            continue;
        }
        if (i + 1 < n && body[i] == '/' && body[i + 1] == '*') {
            i += 2;
            while (i + 1 < n && !(body[i] == '*' && body[i + 1] == '/')) i++;
            if (i + 1 < n) i += 2;
            continue;
        }
        while (i < n && (cc_is_ident_char(body[i]) || body[i] == '*')) {
            if (t + 1 < sizeof(tok)) tok[t++] = body[i];
            i++;
        }
        tok[t] = 0;
        while (i < n && (body[i] == ' ' || body[i] == '\t' || body[i] == '\n' ||
                         body[i] == '\r'))
            i++;
        if (i < n && body[i] == ':') {
            i++;
            in_as = (strcmp(tok, "as") == 0);
            continue;
        }
        if (in_as && tok[0] && nn < cap) {
            snprintf(names[nn], 64, "%s", tok);
            nn++;
        }
    }
    return nn;
}

/* Names from `@typeview [Mode] on type_name { as: … }` in src (comments/strings skipped). */
static inline int cc_typeview_as_names_for_type(const char* src, size_t n,
                                                const char* type_name,
                                                char names[][64], int cap) {
    size_t i = 0;
    int nn = 0;
    size_t tlen;
    if (!src || !type_name || !type_name[0] || !names || cap <= 0) return 0;
    tlen = strlen(type_name);
    while (i < n && nn < cap) {
        size_t p, base_l, base_r, brace;
        int depth;
        if (src[i] == '"' || src[i] == '\'') {
            char q = src[i++];
            while (i < n) {
                if (src[i] == '\\' && i + 1 < n) {
                    i += 2;
                    continue;
                }
                if (src[i] == q) {
                    i++;
                    break;
                }
                i++;
            }
            continue;
        }
        if (i + 1 < n && src[i] == '/' && src[i + 1] == '/') {
            while (i < n && src[i] != '\n') i++;
            continue;
        }
        if (i + 1 < n && src[i] == '/' && src[i + 1] == '*') {
            i += 2;
            while (i + 1 < n && !(src[i] == '*' && src[i + 1] == '/')) i++;
            if (i + 1 < n) i += 2;
            continue;
        }
        if (!(src[i] == '@' && i + 9 <= n && memcmp(src + i, "@typeview", 9) == 0 &&
              (i + 9 == n || !cc_is_ident_char(src[i + 9])))) {
            i++;
            continue;
        }
        p = i + 9;
        while (p < n && (src[p] == ' ' || src[p] == '\t' || src[p] == '\n' ||
                         src[p] == '\r'))
            p++;
        if (p + 2 < n && src[p] == 'o' && src[p + 1] == 'n' &&
            !cc_is_ident_char(src[p + 2])) {
            p += 2;
        } else if (p < n && cc_is_ident_start(src[p])) {
            while (p < n && cc_is_ident_char(src[p])) p++;
            while (p < n && (src[p] == ' ' || src[p] == '\t' || src[p] == '\n' ||
                             src[p] == '\r'))
                p++;
            if (!(p + 2 < n && src[p] == 'o' && src[p + 1] == 'n' &&
                  !cc_is_ident_char(src[p + 2]))) {
                i++;
                continue;
            }
            p += 2;
        } else {
            i++;
            continue;
        }
        while (p < n && (src[p] == ' ' || src[p] == '\t' || src[p] == '\n' ||
                         src[p] == '\r'))
            p++;
        base_l = p;
        while (p < n && cc_is_ident_char(src[p])) p++;
        if (p < n && src[p] == '*') p++;
        base_r = p;
        if (base_r <= base_l || (base_r - base_l) != tlen ||
            memcmp(src + base_l, type_name, tlen) != 0) {
            i = (p > i) ? p : i + 1;
            continue;
        }
        while (p < n && (src[p] == ' ' || src[p] == '\t' || src[p] == '\n' ||
                         src[p] == '\r'))
            p++;
        if (p >= n || src[p] != '{') {
            i = p > i ? p : i + 1;
            continue;
        }
        brace = p;
        depth = 0;
        for (; p < n; p++) {
            if (src[p] == '{') depth++;
            else if (src[p] == '}') {
                depth--;
                if (depth == 0) {
                    p++;
                    break;
                }
            }
        }
        nn += cc_typeview_body_as_names(src + brace, p - brace, names + nn,
                                        cap - nn);
        i = p;
    }
    return nn;
}

static inline int cc__th_grow(char** out, size_t* cap, size_t w, size_t need) {
    size_t nc;
    char* nb;
    if (w + need <= *cap) return 0;
    nc = *cap * 2 + need + 64;
    nb = (char*)realloc(*out, nc + 1);
    if (!nb) return -1;
    *out = nb;
    *cap = nc;
    return 0;
}

/* Copy a @typehooks body, wrapping `.create = ident` / `.destroy = ident`
 * as cc_type_*_call("ident") so the register object type-checks. Calls
 * (`cc_type_create_call(...)`) and other fields are copied unchanged. */
static inline int cc__th_copy_hooks_body(const char* src, size_t body_l, size_t body_n,
                                         char** out, size_t* w, size_t* cap) {
    size_t i = body_l;
    size_t end = body_l + body_n;
    int ls = 0, lc = 0, st = 0, ch = 0;
    while (i < end) {
        char d = src[i];
        char d2 = (i + 1 < end) ? src[i + 1] : 0;
        if (lc) {
            if (cc__th_grow(out, cap, *w, 1) != 0) return -1;
            (*out)[(*w)++] = d;
            if (d == '\n') lc = 0;
            i++;
            continue;
        }
        if (ls) {
            if (cc__th_grow(out, cap, *w, 1) != 0) return -1;
            (*out)[(*w)++] = d;
            if (d == '*' && d2 == '/') {
                if (cc__th_grow(out, cap, *w, 1) != 0) return -1;
                (*out)[(*w)++] = d2;
                i += 2;
                ls = 0;
                continue;
            }
            i++;
            continue;
        }
        if (st) {
            if (cc__th_grow(out, cap, *w, 1) != 0) return -1;
            (*out)[(*w)++] = d;
            if (d == '\\' && d2) {
                if (cc__th_grow(out, cap, *w, 1) != 0) return -1;
                (*out)[(*w)++] = d2;
                i += 2;
                continue;
            }
            if (d == '"') st = 0;
            i++;
            continue;
        }
        if (ch) {
            if (cc__th_grow(out, cap, *w, 1) != 0) return -1;
            (*out)[(*w)++] = d;
            if (d == '\\' && d2) {
                if (cc__th_grow(out, cap, *w, 1) != 0) return -1;
                (*out)[(*w)++] = d2;
                i += 2;
                continue;
            }
            if (d == '\'') ch = 0;
            i++;
            continue;
        }
        if (d == '/' && d2 == '/') {
            lc = 1;
            if (cc__th_grow(out, cap, *w, 2) != 0) return -1;
            (*out)[(*w)++] = d;
            (*out)[(*w)++] = d2;
            i += 2;
            continue;
        }
        if (d == '/' && d2 == '*') {
            ls = 1;
            if (cc__th_grow(out, cap, *w, 2) != 0) return -1;
            (*out)[(*w)++] = d;
            (*out)[(*w)++] = d2;
            i += 2;
            continue;
        }
        if (d == '"') {
            st = 1;
            if (cc__th_grow(out, cap, *w, 1) != 0) return -1;
            (*out)[(*w)++] = d;
            i++;
            continue;
        }
        if (d == '\'') {
            ch = 1;
            if (cc__th_grow(out, cap, *w, 1) != 0) return -1;
            (*out)[(*w)++] = d;
            i++;
            continue;
        }
        if (d == '.') {
            size_t k = i + 1;
            int which = 0;
            if (k + 6 <= end && memcmp(src + k, "create", 6) == 0 &&
                (k + 6 >= end || !cc_is_ident_char(src[k + 6]))) {
                which = 1;
                k += 6;
            } else if (k + 7 <= end && memcmp(src + k, "destroy", 7) == 0 &&
                       (k + 7 >= end || !cc_is_ident_char(src[k + 7]))) {
                which = 2;
                k += 7;
            }
            if (which) {
                size_t eq = k;
                while (eq < end && (src[eq] == ' ' || src[eq] == '\t' ||
                                    src[eq] == '\n' || src[eq] == '\r'))
                    eq++;
                if (eq < end && src[eq] == '=') {
                    size_t id = eq + 1;
                    while (id < end && (src[id] == ' ' || src[id] == '\t' ||
                                        src[id] == '\n' || src[id] == '\r'))
                        id++;
                    if (id < end && cc_is_ident_start(src[id])) {
                        size_t id_e = id;
                        size_t look;
                        while (id_e < end && cc_is_ident_char(src[id_e])) id_e++;
                        look = id_e;
                        while (look < end && (src[look] == ' ' || src[look] == '\t' ||
                                              src[look] == '\n' || src[look] == '\r'))
                            look++;
                        if (look >= end || src[look] != '(') {
                            const char* pref = (which == 1)
                                ? "cc_type_create_call(\""
                                : "cc_type_destroy_call(\"";
                            size_t pref_n = strlen(pref);
                            size_t need = (id - i) + pref_n + (id_e - id) + 2;
                            if (cc__th_grow(out, cap, *w, need) != 0) return -1;
                            memcpy(*out + *w, src + i, id - i);
                            *w += (id - i);
                            memcpy(*out + *w, pref, pref_n);
                            *w += pref_n;
                            memcpy(*out + *w, src + id, id_e - id);
                            *w += (id_e - id);
                            (*out)[(*w)++] = '"';
                            (*out)[(*w)++] = ')';
                            i = id_e;
                            continue;
                        }
                    }
                }
            }
        }
        if (cc__th_grow(out, cap, *w, 1) != 0) return -1;
        (*out)[(*w)++] = d;
        i++;
    }
    return 0;
}

/* `@typehooks on Subject[*]? { .field = expr, … };` →
 * `@comptime { (void)cc_type_register("Subject*", (CCTypeHooks){ … }); }`
 * Body is a strict C designated-initializer (one RHS expression per arm).
 * Returns malloc'd buffer, or NULL when unchanged / OOM. */
static inline char* cc_rewrite_typehooks_to_register(const char* src, size_t n) {
    size_t i = 0;
    size_t out_cap;
    size_t w = 0;
    char* out;
    int changed = 0;
    if (!src || n == 0) return NULL;
    out_cap = n + 128;
    out = (char*)malloc(out_cap + 1);
    if (!out) return NULL;
    while (i < n) {
        if (src[i] == '"' || src[i] == '\'') {
            char q = src[i++];
            if (w + 1 > out_cap) {
                size_t nc = out_cap * 2 + 64;
                char* nb = (char*)realloc(out, nc + 1);
                if (!nb) { free(out); return NULL; }
                out = nb; out_cap = nc;
            }
            out[w++] = q;
            while (i < n) {
                if (w + 2 > out_cap) {
                    size_t nc = out_cap * 2 + 64;
                    char* nb = (char*)realloc(out, nc + 1);
                    if (!nb) { free(out); return NULL; }
                    out = nb; out_cap = nc;
                }
                out[w++] = src[i];
                if (src[i] == '\\' && i + 1 < n) {
                    out[w++] = src[i + 1];
                    i += 2;
                    continue;
                }
                if (src[i] == q) { i++; break; }
                i++;
            }
            continue;
        }
        if (i + 1 < n && src[i] == '/' && src[i + 1] == '/') {
            while (i < n && src[i] != '\n') {
                if (w + 1 > out_cap) {
                    size_t nc = out_cap * 2 + 64;
                    char* nb = (char*)realloc(out, nc + 1);
                    if (!nb) { free(out); return NULL; }
                    out = nb; out_cap = nc;
                }
                out[w++] = src[i++];
            }
            continue;
        }
        if (i + 1 < n && src[i] == '/' && src[i + 1] == '*') {
            while (i + 1 < n && !(src[i] == '*' && src[i + 1] == '/')) {
                if (w + 1 > out_cap) {
                    size_t nc = out_cap * 2 + 64;
                    char* nb = (char*)realloc(out, nc + 1);
                    if (!nb) { free(out); return NULL; }
                    out = nb; out_cap = nc;
                }
                out[w++] = src[i++];
            }
            if (i + 1 < n) {
                if (w + 2 > out_cap) {
                    size_t nc = out_cap * 2 + 64;
                    char* nb = (char*)realloc(out, nc + 1);
                    if (!nb) { free(out); return NULL; }
                    out = nb; out_cap = nc;
                }
                out[w++] = src[i++];
                out[w++] = src[i++];
            }
            continue;
        }
        if (src[i] == '@' && i + 10 <= n && memcmp(src + i, "@typehooks", 10) == 0 &&
            (i + 10 == n || !cc_is_ident_char(src[i + 10]))) {
            size_t p = i + 10;
            size_t sub_l, sub_r, brace_l = 0, brace_r = 0;
            int depth = 0;
            int ls = 0, lc = 0, st = 0, ch = 0;
            while (p < n && (src[p] == ' ' || src[p] == '\t' || src[p] == '\n' ||
                             src[p] == '\r'))
                p++;
            if (!(p + 2 < n && src[p] == 'o' && src[p + 1] == 'n' &&
                  !cc_is_ident_char(src[p + 2]))) {
                /* not `on` — copy '@' and continue */
                if (w + 1 > out_cap) {
                    size_t nc = out_cap * 2 + 64;
                    char* nb = (char*)realloc(out, nc + 1);
                    if (!nb) { free(out); return NULL; }
                    out = nb; out_cap = nc;
                }
                out[w++] = src[i++];
                continue;
            }
            p += 2;
            while (p < n && (src[p] == ' ' || src[p] == '\t' || src[p] == '\n' ||
                             src[p] == '\r'))
                p++;
            sub_l = p;
            while (p < n && cc_is_ident_char(src[p])) p++;
            /* Optional trailing `*` (glob or pointer subject). */
            if (p < n && src[p] == '*') p++;
            sub_r = p;
            while (p < n && (src[p] == ' ' || src[p] == '\t' || src[p] == '\n' ||
                             src[p] == '\r'))
                p++;
            if (p >= n || src[p] != '{' || sub_r <= sub_l) {
                if (w + 1 > out_cap) {
                    size_t nc = out_cap * 2 + 64;
                    char* nb = (char*)realloc(out, nc + 1);
                    if (!nb) { free(out); return NULL; }
                    out = nb; out_cap = nc;
                }
                out[w++] = src[i++];
                continue;
            }
            brace_l = p;
            for (; p < n; p++) {
                char d = src[p];
                char d2 = (p + 1 < n) ? src[p + 1] : 0;
                if (lc) { if (d == '\n') lc = 0; continue; }
                if (ls) {
                    if (d == '*' && d2 == '/') { ls = 0; p++; }
                    continue;
                }
                if (st) {
                    if (d == '\\' && d2) { p++; continue; }
                    if (d == '"') st = 0;
                    continue;
                }
                if (ch) {
                    if (d == '\\' && d2) { p++; continue; }
                    if (d == '\'') ch = 0;
                    continue;
                }
                if (d == '/' && d2 == '/') { lc = 1; p++; continue; }
                if (d == '/' && d2 == '*') { ls = 1; p++; continue; }
                if (d == '"') { st = 1; continue; }
                if (d == '\'') { ch = 1; continue; }
                if (d == '{') { depth++; continue; }
                if (d == '}') {
                    if (depth == 0) break;
                    depth--;
                    if (depth == 0) {
                        brace_r = p;
                        break;
                    }
                }
            }
            if (brace_r > brace_l) {
                size_t end = brace_r + 1;
                size_t body_l = brace_l + 1;
                size_t body_n = brace_r - body_l;
                size_t sub_n = sub_r - sub_l;
                size_t need;
                while (end < n && (src[end] == ' ' || src[end] == '\t' ||
                                   src[end] == '\n' || src[end] == '\r'))
                    end++;
                if (end < n && src[end] == ';') end++;
                /* @comptime { (void)cc_type_register("SUB", (CCTypeHooks){ BODY }); } */
                need = 64 + sub_n + body_n + 96;
                if (w + need > out_cap) {
                    size_t nc = out_cap * 2 + need + 64;
                    char* nb = (char*)realloc(out, nc + 1);
                    if (!nb) { free(out); return NULL; }
                    out = nb; out_cap = nc;
                }
                memcpy(out + w, "@comptime { (void)cc_type_register(\"", 36);
                w += 36;
                memcpy(out + w, src + sub_l, sub_n);
                w += sub_n;
                memcpy(out + w, "\", (CCTypeHooks){", 17);
                w += 17;
                if (cc__th_copy_hooks_body(src, body_l, body_n, &out, &w, &out_cap) != 0) {
                    free(out);
                    return NULL;
                }
                if (cc__th_grow(&out, &out_cap, w, 5) != 0) {
                    free(out);
                    return NULL;
                }
                memcpy(out + w, "}); }", 5);
                w += 5;
                i = end;
                changed = 1;
                continue;
            }
        }
        if (w + 1 > out_cap) {
            size_t nc = out_cap * 2 + 64;
            char* nb = (char*)realloc(out, nc + 1);
            if (!nb) { free(out); return NULL; }
            out = nb; out_cap = nc;
        }
        out[w++] = src[i++];
    }
    out[w] = '\0';
    if (!changed) { free(out); return NULL; }
    return out;
}

/* Erase `@typehooks on Subject { … };` from lowered `.h` text (Concurrent-C
 * registration fact — host C must not see it). */
static inline char* cc_strip_typehooks_blocks(const char* src, size_t n) {
    size_t i = 0;
    size_t out_cap;
    size_t w = 0;
    char* out;
    int changed = 0;
    if (!src || n == 0) return NULL;
    out_cap = n + 8;
    out = (char*)malloc(out_cap + 1);
    if (!out) return NULL;
    while (i < n) {
        if (src[i] == '"' || src[i] == '\'') {
            char q = src[i++];
            if (w + 1 > out_cap) {
                size_t nc = out_cap * 2 + 64;
                char* nb = (char*)realloc(out, nc + 1);
                if (!nb) { free(out); return NULL; }
                out = nb; out_cap = nc;
            }
            out[w++] = q;
            while (i < n) {
                if (w + 2 > out_cap) {
                    size_t nc = out_cap * 2 + 64;
                    char* nb = (char*)realloc(out, nc + 1);
                    if (!nb) { free(out); return NULL; }
                    out = nb; out_cap = nc;
                }
                out[w++] = src[i];
                if (src[i] == '\\' && i + 1 < n) {
                    out[w++] = src[i + 1];
                    i += 2;
                    continue;
                }
                if (src[i] == q) { i++; break; }
                i++;
            }
            continue;
        }
        if (i + 1 < n && src[i] == '/' && src[i + 1] == '/') {
            while (i < n && src[i] != '\n') {
                if (w + 1 > out_cap) {
                    size_t nc = out_cap * 2 + 64;
                    char* nb = (char*)realloc(out, nc + 1);
                    if (!nb) { free(out); return NULL; }
                    out = nb; out_cap = nc;
                }
                out[w++] = src[i++];
            }
            continue;
        }
        if (i + 1 < n && src[i] == '/' && src[i + 1] == '*') {
            while (i + 1 < n && !(src[i] == '*' && src[i + 1] == '/')) {
                if (w + 1 > out_cap) {
                    size_t nc = out_cap * 2 + 64;
                    char* nb = (char*)realloc(out, nc + 1);
                    if (!nb) { free(out); return NULL; }
                    out = nb; out_cap = nc;
                }
                out[w++] = src[i++];
            }
            if (i + 1 < n) {
                if (w + 2 > out_cap) {
                    size_t nc = out_cap * 2 + 64;
                    char* nb = (char*)realloc(out, nc + 1);
                    if (!nb) { free(out); return NULL; }
                    out = nb; out_cap = nc;
                }
                out[w++] = src[i++];
                out[w++] = src[i++];
            }
            continue;
        }
        if (src[i] == '@' && i + 10 <= n && memcmp(src + i, "@typehooks", 10) == 0 &&
            (i + 10 == n || !cc_is_ident_char(src[i + 10]))) {
            size_t p = i + 10;
            int depth = 0;
            int ls = 0, lc = 0, st = 0, ch = 0;
            int erased = 0;
            while (p < n && src[p] != '{') p++;
            if (p < n && src[p] == '{') {
                for (; p < n; p++) {
                    char d = src[p];
                    char d2 = (p + 1 < n) ? src[p + 1] : 0;
                    if (lc) { if (d == '\n') lc = 0; continue; }
                    if (ls) {
                        if (d == '*' && d2 == '/') { ls = 0; p++; }
                        continue;
                    }
                    if (st) {
                        if (d == '\\' && d2) { p++; continue; }
                        if (d == '"') st = 0;
                        continue;
                    }
                    if (ch) {
                        if (d == '\\' && d2) { p++; continue; }
                        if (d == '\'') ch = 0;
                        continue;
                    }
                    if (d == '/' && d2 == '/') { lc = 1; p++; continue; }
                    if (d == '/' && d2 == '*') { ls = 1; p++; continue; }
                    if (d == '"') { st = 1; continue; }
                    if (d == '\'') { ch = 1; continue; }
                    if (d == '{') { depth++; continue; }
                    if (d == '}') {
                        if (depth == 0) break;
                        depth--;
                        if (depth == 0) {
                            p++;
                            while (p < n && (src[p] == ' ' || src[p] == '\t' ||
                                             src[p] == '\n' || src[p] == '\r'))
                                p++;
                            if (p < n && src[p] == ';') p++;
                            i = p;
                            changed = 1;
                            erased = 1;
                            break;
                        }
                    }
                }
            }
            if (erased) continue;
        }
        if (w + 1 > out_cap) {
            size_t nc = out_cap * 2 + 64;
            char* nb = (char*)realloc(out, nc + 1);
            if (!nb) { free(out); return NULL; }
            out = nb; out_cap = nc;
        }
        out[w++] = src[i++];
    }
    out[w] = '\0';
    if (!changed) { free(out); return NULL; }
    return out;
}

/* Parse `Type name [@as|/@as/] [= init]` in [stmt, stmt_end).
 * When out_is_as is non-NULL, set to 1 if a trailing `@as` follows the name. */
static inline void cc_parse_decl_name_and_type_ex(const char* stmt,
                                                  const char* stmt_end,
                                                  char* out_name,
                                                  size_t out_name_sz,
                                                  char* out_type,
                                                  size_t out_type_sz,
                                                  int* out_is_as) {
    const char* p = stmt;
    const char* semi = stmt_end;
    const char* name_s = NULL;
    size_t name_n = 0;
    const char* multi_decl_type_end = NULL;
    const char* cur;
    if (out_is_as) *out_is_as = 0;
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
    /* Multi-declarator support (`T a, b;`): remember the first TOP-LEVEL
     * comma and the identifier that precedes it (the first declarator), so
     * the type does not swallow earlier declarators when the LAST declarator
     * is reported as the name.  Without this, `CCChanTx tx1, tx2;` parsed as
     * name="tx2", type="CCChanTx tx1," and downstream UFCS lowering composed
     * garbage callees like `cc_chan_tx tx1,_close(&tx2)`. */
    {
        int dpar = 0, dbr = 0, dbrc = 0;
        const char* first_decl_s = NULL;
        const char* last_top_ident_s = NULL;
        int saw_top_comma = 0;
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
            /* Trailing field attrs (`@as`) — stop; name is already captured. */
            if (*cur == '@' && dpar == 0 && dbr == 0 && dbrc == 0) break;
            if (*cur == '(') { dpar++; cur++; continue; }
            if (*cur == ')') { if (dpar > 0) dpar--; cur++; continue; }
            if (*cur == '[') { dbr++; cur++; continue; }
            if (*cur == ']') { if (dbr > 0) dbr--; cur++; continue; }
            if (*cur == '{') { dbrc++; cur++; continue; }
            if (*cur == '}') { if (dbrc > 0) dbrc--; cur++; continue; }
            if (*cur == ',' && dpar == 0 && dbr == 0 && dbrc == 0 && !saw_top_comma) {
                saw_top_comma = 1;
                first_decl_s = last_top_ident_s;
                cur++;
                continue;
            }
            if (!cc_is_ident_start(*cur)) { cur++; continue; }
            {
                const char* s = cur++;
                while (cur < semi && cc_is_ident_char(*cur)) cur++;
                name_s = s;
                name_n = (size_t)(cur - s);
                if (dpar == 0 && dbr == 0 && dbrc == 0) last_top_ident_s = s;
            }
        }
        if (saw_top_comma && first_decl_s && first_decl_s > p && first_decl_s < name_s) {
            multi_decl_type_end = first_decl_s;
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
        /* Named `@as` embed: source form `Type name @as;` is rewritten to a
         * block-comment marker before TCC parse, so both spellings arrive
         * here.  Ordinary comments around the attribute are inert, but the
         * shared skip cannot tell the marker from a real comment — so scan
         * the run it consumed for the marker rather than testing only the
         * byte it landed on. */
        {
            size_t base = (size_t)(after - p);
            size_t lim  = (size_t)(semi - p);
            size_t k    = cc_skip_ws_and_comments(p, lim, base);
            for (size_t q = base; q + 7 <= k; q++) {
                if (p[q] == '/' && p[q + 1] == '*' && p[q + 2] == '@' &&
                    p[q + 3] == 'a' && p[q + 4] == 's' && p[q + 5] == '*' &&
                    p[q + 6] == '/') {
                    if (out_is_as) *out_is_as = 1;
                    break;
                }
            }
            after = p + k;
        }
        if (after + 3 <= semi && after[0] == '@' && after[1] == 'a' && after[2] == 's' &&
            (after + 3 == semi || !cc_is_ident_char(after[3]))) {
            if (out_is_as) *out_is_as = 1;
            after = p + cc_skip_ws_and_comments(p, (size_t)(semi - p),
                                                (size_t)(after + 3 - p));
        }
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
        const char* ty_e = multi_decl_type_end ? multi_decl_type_end : name_s;
        /* A declaration's type is its specifier TOKENS; a comment anywhere in
         * that span is a separator, not text.  Copy code only, collapsing each
         * inert run to one space, so the recorded type matches what the same
         * declaration without comments would record. */
        size_t span = (size_t)(ty_e > ty_s ? ty_e - ty_s : 0);
        size_t si = 0;
        size_t w = 0;
        while (si < span && w + 1 < out_type_sz) {
            size_t k = cc_skip_ws_and_comments(ty_s, span, si);
            if (k > si) {
                if (w > 0) out_type[w++] = ' ';
                si = k;
                continue;
            }
            out_type[w++] = ty_s[si++];
        }
        while (w > 0 && out_type[w - 1] == ' ') w--;
        if (w == 0) return;
        out_type[w] = '\0';
        if (name_n >= out_name_sz) name_n = out_name_sz - 1;
        memcpy(out_name, name_s, name_n);
        out_name[name_n] = '\0';
    }
}

static inline void cc_parse_decl_name_and_type(const char* stmt,
                                               const char* stmt_end,
                                               char* out_name,
                                               size_t out_name_sz,
                                               char* out_type,
                                               size_t out_type_sz) {
    cc_parse_decl_name_and_type_ex(stmt, stmt_end, out_name, out_name_sz,
                                   out_type, out_type_sz, NULL);
}

#endif /* CC_UTIL_TEXT_H */
