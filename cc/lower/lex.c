/* Lexer for Concurrent-C. See lex.h for the contract.
 *
 * Invariant: every byte of the input belongs to exactly one token, either as
 * leading trivia or as text, and the CC_TK_EOF token carries the trailing
 * trivia; concatenating trivia + text over all tokens reproduces the file.
 * Bad input never stops the lexer: it becomes a CC_TK_ERROR token with a
 * diagnostic and lexing resumes after it. */
#include "lex.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct CcLexer {
    CcArena *a;
    CcDiag *d;
    CcLexFile f;             /* line_starts final; toks/marks point at the growing arrays */
    const char *src;
    uint32_t len;
    uint32_t pos;
    uint32_t cap_toks;
    uint32_t cap_marks;
    const char **paths;      /* interned mark paths */
    uint32_t n_paths, cap_paths;
    const char *cur_path;    /* logical path in effect; NULL = the file's own */
    int at_line_start;       /* only trivia since the previous newline */
    int comment_to_eof;      /* trivia stopped at an unterminated block comment */
} CcLexer;

/* ---- growth ---------------------------------------------------------- */

static void *cc__grow(void *p, uint32_t *cap, size_t elem, const char *what) {
    uint32_t ncap = *cap ? *cap * 2 : 64;
    void *q;
    if (ncap < *cap || (size_t)ncap > SIZE_MAX / elem) {
        fprintf(stderr, "cc: lexer: %s array cannot grow past %u entries\n", what, *cap);
        abort();
    }
    q = realloc(p, (size_t)ncap * elem);
    if (!q) {
        fprintf(stderr, "cc: out of memory: lexer %s array (%zu bytes)\n", what,
                (size_t)ncap * elem);
        abort();
    }
    *cap = ncap;
    return q;
}

#define AT(L, i) ((i) < (L)->len ? (unsigned char)(L)->src[i] : 0u)

/* ---- character classes ----------------------------------------------- */

static int cc__is_digit(unsigned c) { return c >= '0' && c <= '9'; }
static int cc__is_alpha(unsigned c) { return (c | 0x20) >= 'a' && (c | 0x20) <= 'z'; }
/* Bytes >= 0x80 are UTF-8 pieces of extended identifier characters, as GCC
 * and clang accept them. */
static int cc__is_ident_start(unsigned c) { return cc__is_alpha(c) || c == '_' || c >= 0x80; }
static int cc__is_ident_cont(unsigned c) { return cc__is_ident_start(c) || cc__is_digit(c) || c == '$'; }
static int cc__is_hspace(unsigned c) { return c == ' ' || c == '\t' || c == '\v' || c == '\f' || c == '\r'; }

/* ---- physical positions ---------------------------------------------- */

static uint32_t cc__line_index(const CcLexFile *f, uint32_t off) {
    uint32_t lo = 0, hi = f->n_lines; /* last index with line_starts[i] <= off */
    while (hi - lo > 1) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (f->line_starts[mid] <= off) lo = mid; else hi = mid;
    }
    return lo;
}

static void cc__check_off(const CcLexFile *f, uint32_t off, const char *fn) {
    if (off > f->len) {
        fprintf(stderr, "cc: %s: offset %u past end of %s (%u bytes)\n", fn, (unsigned)off,
                f->path ? f->path : "<memory>", (unsigned)f->len);
        abort();
    }
}

void cc_lex_phys(const CcLexFile *f, uint32_t off, uint32_t *line, uint32_t *col) {
    uint32_t li;
    cc__check_off(f, off, "cc_lex_phys");
    li = cc__line_index(f, off);
    if (line) *line = li + 1;
    if (col) *col = off - f->line_starts[li] + 1;
}

CcLoc cc_lex_loc(const CcLexFile *f, uint32_t off) {
    CcLoc loc;
    uint32_t pline, pcol;
    cc_lex_phys(f, off, &pline, &pcol);
    loc.path = f->path;
    loc.line = pline;
    loc.col = pcol;
    if (f->n_marks) {
        uint32_t lo = 0, hi = f->n_marks; /* last mark with off <= off, or none */
        if (f->marks[0].off <= off) {
            while (hi - lo > 1) {
                uint32_t mid = lo + (hi - lo) / 2;
                if (f->marks[mid].off <= off) lo = mid; else hi = mid;
            }
            loc.line = f->marks[lo].logical_line + (pline - f->marks[lo].phys_line);
            if (f->marks[lo].path) loc.path = f->marks[lo].path;
        }
    }
    return loc;
}

const char *cc_lex_line_text(CcArena *a, const CcLexFile *f, uint32_t off, uint32_t *col_out) {
    uint32_t li, start, end;
    cc__check_off(f, off, "cc_lex_line_text");
    li = cc__line_index(f, off);
    start = f->line_starts[li];
    end = li + 1 < f->n_lines ? f->line_starts[li + 1] - 1 : f->len;
    if (end > start && f->src[end - 1] == '\r') end--;
    if (col_out) *col_out = off - start + 1;
    return cc_arena_strndup(a, f->src + start, end - start);
}

/* ---- diagnostics ----------------------------------------------------- */

static void cc__lex_diag(CcLexer *L, CcSeverity sev, uint32_t off, uint32_t len, const char *fmt, ...) {
    CcLoc loc = cc_lex_loc(&L->f, off);
    va_list ap;
    char *text;
    va_start(ap, fmt);
    {
        va_list ap2;
        int n;
        va_copy(ap2, ap);
        n = vsnprintf(NULL, 0, fmt, ap2);
        va_end(ap2);
        if (n < 0) { fprintf(stderr, "cc: lexer: bad diagnostic format\n"); abort(); }
        text = (char *)cc_arena_alloc(L->a, (size_t)n + 1, 1);
        vsnprintf(text, (size_t)n + 1, fmt, ap);
    }
    va_end(ap);
    cc_diag_emit_at(L->d, sev, loc, L->src, L->len, off, len, "%s", text);
}

/* ---- marks ----------------------------------------------------------- */

static const char *cc__intern_path(CcLexer *L, const char *s, size_t n) {
    uint32_t i;
    char *q;
    for (i = 0; i < L->n_paths; i++)
        if (strlen(L->paths[i]) == n && memcmp(L->paths[i], s, n) == 0) return L->paths[i];
    q = cc_arena_strndup(L->a, s, n);
    if (L->n_paths == L->cap_paths)
        L->paths = (const char **)cc__grow(L->paths, &L->cap_paths, sizeof *L->paths, "path");
    L->paths[L->n_paths++] = q;
    return q;
}

/* `off` is the first byte of the line after the directive. */
static void cc__add_mark(CcLexer *L, uint32_t off, uint32_t logical_line, const char *path) {
    CcLineMark *m;
    if (L->f.n_marks == L->cap_marks)
        L->f.marks = (CcLineMark *)cc__grow(L->f.marks, &L->cap_marks, sizeof *L->f.marks, "mark");
    m = &L->f.marks[L->f.n_marks++];
    m->off = off;
    m->phys_line = cc__line_index(&L->f, off) + 1;
    m->logical_line = logical_line;
    m->path = path;
    L->cur_path = path;
}

/* Parse `N` and an optional `"path"` (or, when `bare_ok`, an unquoted path
 * running to the end of the text) from a directive tail. Returns 1 and fills
 * the outputs when the text starts with a line number. `*path_out` is left
 * as the path in effect when none is given. A quoted path keeps its bytes
 * except that `\\` and `\"` are unescaped. */
static int cc__parse_mark_tail(CcLexer *L, const char *s, const char *end, int bare_ok,
                               uint32_t diag_off, uint32_t *n_out, const char **path_out) {
    unsigned long long n = 0;
    const char *p = s;
    while (p < end && (*p == ' ' || *p == '\t')) p++;
    if (p >= end || !cc__is_digit((unsigned char)*p)) return 0;
    while (p < end && cc__is_digit((unsigned char)*p)) {
        n = n * 10 + (unsigned)(*p - '0');
        if (n > UINT32_MAX) {
            cc__lex_diag(L, CC_SEV_ERROR, diag_off, (uint32_t)(end - s), "line number too large");
            return 0;
        }
        p++;
    }
    while (p < end && (*p == ' ' || *p == '\t')) p++;
    if (p < end && *p == '"') {
        CcBuf b;
        const char *q = p + 1;
        cc_buf_init(&b);
        while (q < end && *q != '"') {
            if (*q == '\\' && q + 1 < end && (q[1] == '\\' || q[1] == '"')) q++;
            cc_buf_push_char(&b, *q);
            q++;
        }
        if (q >= end) {
            cc_buf_free(&b);
            cc__lex_diag(L, CC_SEV_ERROR, diag_off + (uint32_t)(p - s), 1,
                         "unterminated path in line directive");
            return 0;
        }
        *path_out = cc__intern_path(L, b.data, b.len);
        cc_buf_free(&b);
    } else if (bare_ok && p < end) {
        const char *q = end;
        while (q > p && (q[-1] == ' ' || q[-1] == '\t')) q--;
        *path_out = cc__intern_path(L, p, (size_t)(q - p));
    }
    *n_out = (uint32_t)n;
    return 1;
}

/* A block comment at [start, end) (end just past its closing delimiter), on
 * one physical line: record a CC_LN mark if it is one. The path may be
 * quoted or bare (`CC_LN 22 /abs/path.shcc`, as the driver emits it). */
static void cc__comment_mark(CcLexer *L, uint32_t start, uint32_t end) {
    const char *s = L->src + start + 2, *e = L->src + end - 2;
    const char *p = s;
    uint32_t n;
    const char *path = L->cur_path;
    uint32_t next_line;
    while (p < e && (*p == ' ' || *p == '\t')) p++;
    if ((size_t)(e - p) < 6 || memcmp(p, "CC_LN", 5) != 0 || (p[5] != ' ' && p[5] != '\t')) return;
    if (!cc__parse_mark_tail(L, p + 5, e, 1, start, &n, &path)) return;
    next_line = end;
    while (next_line < L->len && L->src[next_line] != '\n') next_line++;
    if (next_line < L->len) next_line++;
    cc__add_mark(L, next_line, n, path);
}

/* A preprocessor line at [start, end): record a `#line N "p"` / `# N "p"`
 * mark if it is one. `next` is the first byte of the following line. */
static void cc__pp_mark(CcLexer *L, uint32_t start, uint32_t end, uint32_t next) {
    CcBuf spliced;
    const char *s, *e, *p;
    uint32_t i, n;
    const char *path = L->cur_path;
    cc_buf_init(&spliced);
    for (i = start + 1; i < end; i++) {
        if (L->src[i] == '\\' && i + 1 < end && L->src[i + 1] == '\n') { i++; continue; }
        if (L->src[i] == '\\' && i + 2 < end && L->src[i + 1] == '\r' && L->src[i + 2] == '\n') { i += 2; continue; }
        cc_buf_push_char(&spliced, L->src[i]);
    }
    s = spliced.data;
    e = s + spliced.len;
    p = s;
    while (p < e && (*p == ' ' || *p == '\t')) p++;
    if ((size_t)(e - p) >= 4 && memcmp(p, "line", 4) == 0 && (e - p == 4 || !cc__is_ident_cont((unsigned char)p[4])))
        p += 4;
    if (cc__parse_mark_tail(L, p, e, 0, start, &n, &path))
        cc__add_mark(L, next, n, path);
    cc_buf_free(&spliced);
}

/* ---- trivia ---------------------------------------------------------- */

/* Advance over whitespace, comments and line splices. Sets at_line_start
 * when a newline is crossed. Stops at an unterminated block comment with
 * comment_to_eof set. */
static void cc__skip_trivia(CcLexer *L) {
    uint32_t i = L->pos;
    if (i == 0 && L->len >= 3 && (unsigned char)L->src[0] == 0xEF &&
        (unsigned char)L->src[1] == 0xBB && (unsigned char)L->src[2] == 0xBF)
        i = 3; /* UTF-8 byte order mark */
    for (;;) {
        unsigned c = AT(L, i);
        if (i >= L->len) break;
        if (c == '\n') { i++; L->at_line_start = 1; continue; }
        if (cc__is_hspace(c)) { i++; continue; }
        if (c == '\\') {
            if (AT(L, i + 1) == '\n') { i += 2; continue; }
            if (AT(L, i + 1) == '\r' && AT(L, i + 2) == '\n') { i += 3; continue; }
            break;
        }
        if (c == '/' && AT(L, i + 1) == '/') {
            i += 2;
            while (i < L->len && L->src[i] != '\n') {
                if (L->src[i] == '\\' && AT(L, i + 1) == '\n') { i += 2; continue; }
                if (L->src[i] == '\\' && AT(L, i + 1) == '\r' && AT(L, i + 2) == '\n') { i += 3; continue; }
                i++;
            }
            continue;
        }
        if (c == '/' && AT(L, i + 1) == '*') {
            uint32_t start = i, j = i + 2;
            int crossed_newline = 0;
            for (;;) {
                if (j >= L->len) { L->pos = start; L->comment_to_eof = 1; return; }
                if (L->src[j] == '*' && AT(L, j + 1) == '/') { j += 2; break; }
                if (L->src[j] == '\n') crossed_newline = 1;
                j++;
            }
            if (crossed_newline) L->at_line_start = 1;
            else cc__comment_mark(L, start, j);
            i = j;
            continue;
        }
        break;
    }
    L->pos = i;
}

/* ---- tokens ---------------------------------------------------------- */

static const char *const cc__punct_text[CC_P_COUNT] = {
    "",
    "[", "]", "(", ")", "{", "}",
    ".", "->", "++", "--", "&", "*", "+", "-",
    "~", "!", "/", "%", "<<", ">>", "<", ">",
    "<=", ">=", "==", "!=", "^", "|", "&&", "||",
    "?", ":", ";", "...", "=", "*=",
    "/=", "%=", "+=", "-=", "<<=",
    ">>=", "&=", "^=", "|=", ",",
    "#", "##",
    "!>", "?>", "=>", "::", "..", "`", "$",
};

const char *cc_punct_text(CcPunct p) {
    if ((unsigned)p >= CC_P_COUNT) return NULL;
    return cc__punct_text[p];
}

/* Longest punctuator starting at `pos`; CC_P_NONE when none matches. */
static CcPunct cc__match_punct(CcLexer *L, uint32_t pos, uint32_t *len_out) {
    uint32_t want;
    for (want = 3; want >= 1; want--) {
        unsigned p;
        if (pos + want > L->len) continue;
        for (p = 1; p < CC_P_COUNT; p++) {
            if (p == CC_P_BACKTICK) continue; /* a backtick always starts a template */
            if (strlen(cc__punct_text[p]) == want && memcmp(L->src + pos, cc__punct_text[p], want) == 0) {
                *len_out = want;
                return (CcPunct)p;
            }
        }
    }
    return CC_P_NONE;
}

static uint32_t cc__scan_number(CcLexer *L, uint32_t i) {
    i++; /* first digit, or the `.` before one */
    for (;;) {
        unsigned c = AT(L, i);
        if ((c == 'e' || c == 'E' || c == 'p' || c == 'P') && (AT(L, i + 1) == '+' || AT(L, i + 1) == '-')) { i += 2; continue; }
        if (cc__is_alpha(c) || cc__is_digit(c) || c == '_') { i++; continue; }
        if (c == '.' && AT(L, i + 1) != '.') { i++; continue; }
        break;
    }
    return i;
}

/* End of the physical line containing `start` (the newline excluded, and a
 * `\r` before it too, so CR LF stays whole in the following trivia). */
static uint32_t cc__line_end(CcLexer *L, uint32_t start) {
    uint32_t i = start;
    while (i < L->len && L->src[i] != '\n') i++;
    if (i > start + 1 && L->src[i - 1] == '\r') i--;
    return i;
}

/* Quoted literal starting at the opening quote `start` (prefix already
 * consumed). Returns the end offset, or 0 when unterminated. */
static uint32_t cc__scan_quoted(CcLexer *L, uint32_t i, unsigned quote) {
    i++;
    for (;;) {
        unsigned c;
        if (i >= L->len) return 0;
        c = (unsigned char)L->src[i];
        if (c == '\\') {
            if (i + 1 >= L->len) return 0;
            if (L->src[i + 1] == '\n') { i += 2; continue; }
            if (L->src[i + 1] == '\r' && AT(L, i + 2) == '\n') { i += 3; continue; }
            i += 2;
            continue;
        }
        if (c == '\n') return 0;
        i++;
        if (c == quote) return i;
    }
}

static uint32_t cc__scan_template(CcLexer *L, uint32_t i) {
    i++;
    for (;;) {
        unsigned c;
        if (i >= L->len) return 0;
        c = (unsigned char)L->src[i];
        if (c == '\\') { if (i + 1 >= L->len) return 0; i += 2; continue; }
        i++;
        if (c == '`') return i;
    }
}

/* Whole preprocessor line from the `#` at `i`; returns the end of its text
 * and stores the first byte of the following line in `*next`. */
static uint32_t cc__scan_pp(CcLexer *L, uint32_t i, uint32_t *next) {
    int in_string = 0;
    for (;;) {
        unsigned c = AT(L, i);
        if (i >= L->len) break;
        if (c == '\n') break;
        if (c == '\\') {
            if (AT(L, i + 1) == '\n') { i += 2; continue; }
            if (AT(L, i + 1) == '\r' && AT(L, i + 2) == '\n') { i += 3; continue; }
            i += in_string ? 2 : 1;
            if (i > L->len) i = L->len;
            continue;
        }
        if (c == '"') { in_string = !in_string; i++; continue; }
        if (!in_string && c == '/' && AT(L, i + 1) == '*') {
            uint32_t j = i + 2;
            while (j < L->len && !(L->src[j] == '*' && AT(L, j + 1) == '/')) j++;
            if (j >= L->len) break; /* unterminated: the directive ends at this line */
            i = j + 2;
            continue;
        }
        if (!in_string && c == '/' && AT(L, i + 1) == '/') {
            while (i < L->len && L->src[i] != '\n') {
                if (L->src[i] == '\\' && AT(L, i + 1) == '\n') { i += 2; continue; }
                if (L->src[i] == '\\' && AT(L, i + 1) == '\r' && AT(L, i + 2) == '\n') { i += 3; continue; }
                i++;
            }
            break;
        }
        i++;
    }
    *next = i < L->len ? i + 1 : L->len;
    if (i > L->pos && L->src[i - 1] == '\r') i--;
    return i;
}

static void cc__push_tok(CcLexer *L, CcTokKind kind, CcPunct punct, uint32_t lead_off, uint32_t off, uint32_t end) {
    CcToken *t;
    if (L->f.n_toks == L->cap_toks)
        L->f.toks = (CcToken *)cc__grow(L->f.toks, &L->cap_toks, sizeof *L->f.toks, "token");
    t = &L->f.toks[L->f.n_toks++];
    t->kind = kind;
    t->punct = punct;
    t->off = off;
    t->len = end - off;
    t->lead_off = lead_off;
    t->lead_len = off - lead_off;
    cc_lex_phys(&L->f, off, &t->line, &t->col);
    t->at_line_start = (uint8_t)(L->at_line_start != 0);
    t->after_space = (uint8_t)(t->lead_len > 0);
    L->at_line_start = 0;
    L->pos = end;
}

static void cc__lex_one(CcLexer *L) {
    uint32_t lead_off = L->pos, i, end, plen;
    unsigned c;
    CcPunct p;
    cc__skip_trivia(L);
    i = L->pos;
    if (L->comment_to_eof) {
        L->comment_to_eof = 0;
        cc__lex_diag(L, CC_SEV_ERROR, i, 2, "unterminated comment");
        cc__push_tok(L, CC_TK_ERROR, CC_P_NONE, lead_off, i, L->len);
        return;
    }
    if (i >= L->len) {
        cc__push_tok(L, CC_TK_EOF, CC_P_NONE, lead_off, L->len, L->len);
        return;
    }
    c = (unsigned char)L->src[i];

    if (c == '#' && L->at_line_start) {
        uint32_t next;
        end = cc__scan_pp(L, i, &next);
        cc__pp_mark(L, i, end, next);
        cc__push_tok(L, CC_TK_PP, CC_P_NONE, lead_off, i, end);
        return;
    }

    /* String and character literals, with an optional encoding prefix. */
    {
        uint32_t q = i;
        int quoted = 1;
        if (c == '"' || c == '\'') q = i;
        else if ((c == 'L' || c == 'u' || c == 'U') && (AT(L, i + 1) == '"' || AT(L, i + 1) == '\'')) q = i + 1;
        else if (c == 'u' && AT(L, i + 1) == '8' && (AT(L, i + 2) == '"' || AT(L, i + 2) == '\'')) q = i + 2;
        else quoted = 0;
        if (quoted) {
            unsigned quote = (unsigned char)L->src[q];
            end = cc__scan_quoted(L, q, quote);
            if (!end) {
                end = cc__line_end(L, q);
                cc__lex_diag(L, CC_SEV_ERROR, i, end - i, quote == '"' ? "unterminated string literal"
                                                                        : "unterminated character literal");
                cc__push_tok(L, CC_TK_ERROR, CC_P_NONE, lead_off, i, end);
                return;
            }
            cc__push_tok(L, quote == '"' ? CC_TK_STRING : CC_TK_CHAR, CC_P_NONE, lead_off, i, end);
            return;
        }
    }

    if (c == '`') {
        end = cc__scan_template(L, i);
        if (!end) {
            cc__lex_diag(L, CC_SEV_ERROR, i, 1, "unterminated template literal");
            cc__push_tok(L, CC_TK_ERROR, CC_P_NONE, lead_off, i, L->len);
            return;
        }
        cc__push_tok(L, CC_TK_TEMPLATE, CC_P_NONE, lead_off, i, end);
        return;
    }

    if (cc__is_ident_start(c) || (c == '$' && cc__is_ident_cont(AT(L, i + 1)))) {
        end = i + 1;
        while (cc__is_ident_cont(AT(L, end))) end++;
        cc__push_tok(L, CC_TK_IDENT, CC_P_NONE, lead_off, i, end);
        return;
    }

    if (c == '@') {
        if (cc__is_ident_start(AT(L, i + 1))) {
            end = i + 2;
            while (cc__is_ident_cont(AT(L, end))) end++;
            cc__push_tok(L, CC_TK_AT_WORD, CC_P_NONE, lead_off, i, end);
        } else {
            cc__push_tok(L, CC_TK_AT, CC_P_NONE, lead_off, i, i + 1);
        }
        return;
    }

    if (cc__is_digit(c) || (c == '.' && cc__is_digit(AT(L, i + 1)))) {
        end = cc__scan_number(L, i);
        cc__push_tok(L, CC_TK_NUMBER, CC_P_NONE, lead_off, i, end);
        return;
    }

    p = cc__match_punct(L, i, &plen);
    if (p != CC_P_NONE) {
        cc__push_tok(L, CC_TK_PUNCT, p, lead_off, i, i + plen);
        return;
    }

    cc__lex_diag(L, CC_SEV_ERROR, i, 1, "stray byte 0x%02x", c);
    cc__push_tok(L, CC_TK_ERROR, CC_P_NONE, lead_off, i, i + 1);
}

CcLexFile *cc_lex(CcArena *a, CcDiag *d, const char *path, const char *src, size_t len) {
    CcLexer L;
    CcLexFile *out;
    uint32_t i, cap_lines = 0;
    if (len >= UINT32_MAX) {
        fprintf(stderr, "cc: %s: file of %zu bytes is too large to lex (offsets are 32-bit)\n",
                path ? path : "<memory>", len);
        abort();
    }
    memset(&L, 0, sizeof L);
    L.a = a;
    L.d = d;
    L.src = src;
    L.len = (uint32_t)len;
    L.f.path = path;
    L.f.src = src;
    L.f.len = (uint32_t)len;
    L.at_line_start = 1;

    /* Physical line table first: token positions and diagnostics need it. */
    L.f.line_starts = (uint32_t *)cc__grow(NULL, &cap_lines, sizeof(uint32_t), "line");
    L.f.line_starts[L.f.n_lines++] = 0;
    for (i = 0; i < L.len; i++) {
        if (src[i] != '\n') continue;
        if (L.f.n_lines == cap_lines)
            L.f.line_starts = (uint32_t *)cc__grow(L.f.line_starts, &cap_lines, sizeof(uint32_t), "line");
        L.f.line_starts[L.f.n_lines++] = i + 1;
    }

    do cc__lex_one(&L); while (L.f.toks[L.f.n_toks - 1].kind != CC_TK_EOF);

    out = CC_NEW(a, CcLexFile);
    *out = L.f;
    out->toks = (CcToken *)cc_arena_dup(a, L.f.toks, (size_t)L.f.n_toks * sizeof *L.f.toks);
    out->line_starts = (uint32_t *)cc_arena_dup(a, L.f.line_starts, (size_t)L.f.n_lines * sizeof(uint32_t));
    out->marks = L.f.n_marks ? (CcLineMark *)cc_arena_dup(a, L.f.marks, (size_t)L.f.n_marks * sizeof *L.f.marks) : NULL;
    free(L.f.toks);
    free(L.f.line_starts);
    free(L.f.marks);
    free(L.paths);
    return out;
}

/* ---- token helpers --------------------------------------------------- */

int cc_tok_is(const CcLexFile *f, const CcToken *t, const char *text) {
    size_t n = strlen(text);
    return t->len == n && memcmp(f->src + t->off, text, n) == 0;
}

int cc_tok_is_punct(const CcToken *t, CcPunct p) {
    return t->kind == CC_TK_PUNCT && t->punct == p;
}

int cc_tok_is_ident(const CcLexFile *f, const CcToken *t, const char *name) {
    return t->kind == CC_TK_IDENT && cc_tok_is(f, t, name);
}

int cc_tok_is_at(const CcLexFile *f, const CcToken *t, const char *word) {
    size_t n = strlen(word);
    return t->kind == CC_TK_AT_WORD && t->len == n + 1 && memcmp(f->src + t->off + 1, word, n) == 0;
}
