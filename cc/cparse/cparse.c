#include "cparse.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct {
    CpParse *out;
    const char *path;
    const CpTok *toks;
    int ntoks;
    int i;
} Parser;

static const CpTok k_eof = { CP_TOK_EOF, "", 0, 0, 0 };

static char *cp_str(const char *s, int n) {
    char *d;
    if (n < 0) n = (int)strlen(s);
    d = (char *)malloc((size_t)n + 1);
    if (!d) return NULL;
    memcpy(d, s, (size_t)n);
    d[n] = 0;
    return d;
}

static const CpTok *cur(const Parser *ps) {
    if (ps->i < 0 || ps->i >= ps->ntoks) return &k_eof;
    return &ps->toks[ps->i];
}

static void adv(Parser *ps) {
    if (ps->i < ps->ntoks) ps->i++;
}

static int tok_is(const Parser *ps, CpTokKind k, const char *lit) {
    const CpTok *t = cur(ps);
    size_t n;
    if (t->kind != k) return 0;
    if (!lit) return 1;
    n = strlen(lit);
    return t->len == n && memcmp(ps->out->src + t->offset, lit, n) == 0;
}

static int tok_ident(const Parser *ps, const char *lit) {
    return tok_is(ps, CP_TOK_IDENT, lit);
}

static int tok_n_is(const Parser *ps, int di, CpTokKind k, const char *lit) {
    int i = ps->i + di;
    const CpTok *t;
    size_t n;
    if (i < 0 || i >= ps->ntoks) return 0;
    t = &ps->toks[i];
    if (t->kind != k) return 0;
    if (!lit) return 1;
    n = strlen(lit);
    return t->len == n && memcmp(ps->out->src + t->offset, lit, n) == 0;
}

static void offset_linecol(const char *src, int len, size_t off, int *line,
                           int *col) {
    int ln = 1, c = 1;
    size_t i;
    if (off > (size_t)len) off = (size_t)len;
    for (i = 0; i < off; i++) {
        if (src[i] == '\n') {
            ln++;
            c = 1;
        } else {
            c++;
        }
    }
    *line = ln;
    *col = c;
}

static int fail(Parser *ps, const char *msg) {
    const CpTok *t = cur(ps);
    free(ps->out->err.msg);
    ps->out->err.msg = (char *)malloc(strlen(msg) + 1);
    if (ps->out->err.msg) memcpy(ps->out->err.msg, msg, strlen(msg) + 1);
    offset_linecol(ps->out->src, ps->out->len, t->offset, &ps->out->err.line,
                   &ps->out->err.col);
    return 0;
}

static CpNode *node_new(Parser *ps, CpKind k, int start) {
    CpNode *n = (CpNode *)calloc(1, sizeof(CpNode));
    if (!n) return NULL;
    n->kind = k;
    n->src = ps->out->src;
    n->start = start;
    n->live = -1;
    return n;
}

static int kids_push(CpNode ***arr, int *n, CpNode *kid) {
    CpNode **nb = (CpNode **)realloc(*arr, (size_t)(*n + 1) * sizeof(CpNode *));
    if (!nb) return 0;
    *arr = nb;
    (*arr)[*n] = kid;
    (*n)++;
    return 1;
}

static char *spell_dup(Parser *ps) {
    const CpTok *t = cur(ps);
    return cp_str(ps->out->src + t->offset, (int)t->len);
}

/* ---- line splice (same as FileTape) ---- */

static size_t line_splice_copy(const char *in, size_t n, char *out) {
    size_t o = 0, i = 0;
    while (i < n) {
        if (in[i] == '\\' && i + 1 < n && in[i + 1] == '\n') {
            i += 2;
            continue;
        }
        if (in[i] == '\\' && i + 2 < n && in[i + 1] == '\r' && in[i + 2] == '\n') {
            i += 3;
            continue;
        }
        out[o++] = in[i++];
    }
    return o;
}

static char *splice_own(const char *src, int len, int *out_len) {
    char *buf;
    size_t m;
    if (len < 0) len = (int)strlen(src);
    buf = (char *)malloc((size_t)len + 1);
    if (!buf) return NULL;
    m = line_splice_copy(src, (size_t)len, buf);
    buf[m] = 0;
    *out_len = (int)m;
    return buf;
}

/* ---- FileTape-shaped lex (pp_tok.rules KEEP set) ---- */

static const char *k_punct_multi[] = {
    "...", "<<=", ">>=", "->", "=>", "!>", "?>", "=<!", "<?", "::", "++", "--",
    "<<", ">>", "<=", ">=", "==", "!=", "&&", "||", "*=", "/=", "%=", "+=",
    "-=", "&=", "^=", "|=", "##", NULL
};

static int starts_with(const char *s, int len, int pos, const char *lit) {
    int n = (int)strlen(lit);
    if (pos + n > len) return 0;
    return memcmp(s + pos, lit, (size_t)n) == 0;
}

int cparse_lex(const char *src, int len, CpTok **out, int *n) {
    CpTok *toks = NULL;
    int nt = 0, cap = 0;
    int pos = 0;

    if (len < 0) len = src ? (int)strlen(src) : 0;
    *out = NULL;
    *n = 0;

    while (pos < len) {
        int c, start, i, plen;
        CpTokKind kind;
        CpTok *t;

        if (isspace((unsigned char)src[pos])) {
            pos++;
            continue;
        }
        if (pos + 1 < len && src[pos] == '/' && src[pos + 1] == '/') {
            pos += 2;
            while (pos < len && src[pos] != '\n') pos++;
            continue;
        }
        if (pos + 1 < len && src[pos] == '/' && src[pos + 1] == '*') {
            pos += 2;
            while (pos + 1 < len && !(src[pos] == '*' && src[pos + 1] == '/'))
                pos++;
            if (pos + 1 < len) pos += 2;
            continue;
        }

        start = pos;
        c = (unsigned char)src[pos];
        kind = CP_TOK_PUNCT;
        if (isalpha(c) || c == '_') {
            kind = CP_TOK_IDENT;
            pos++;
            while (pos < len &&
                   (isalnum((unsigned char)src[pos]) || src[pos] == '_'))
                pos++;
        } else if (isdigit(c) || (c == '.' && pos + 1 < len &&
                                 isdigit((unsigned char)src[pos + 1]))) {
            kind = CP_TOK_NUM;
            pos++;
            for (;;) {
                if (pos < len &&
                    (src[pos] == 'e' || src[pos] == 'E' || src[pos] == 'p' ||
                     src[pos] == 'P') &&
                    pos + 1 < len &&
                    (src[pos + 1] == '+' || src[pos + 1] == '-')) {
                    pos += 2;
                    continue;
                }
                if (pos < len &&
                    (isalnum((unsigned char)src[pos]) || src[pos] == '.' ||
                     src[pos] == '_')) {
                    pos++;
                    continue;
                }
                break;
            }
        } else if (c == '"' || c == '\'') {
            char q = (char)c;
            kind = (q == '"') ? CP_TOK_STR : CP_TOK_CHR;
            pos++;
            while (pos < len) {
                if (src[pos] == '\\' && pos + 1 < len) {
                    pos += 2;
                    continue;
                }
                if (src[pos] == q) {
                    pos++;
                    break;
                }
                if (src[pos] == '\n') break;
                pos++;
            }
        } else if (c == '`') {
            kind = CP_TOK_STR;
            pos++;
            while (pos < len && src[pos] != '`') pos++;
            if (pos < len) pos++;
        } else {
            plen = 0;
            for (i = 0; k_punct_multi[i]; i++) {
                int m = (int)strlen(k_punct_multi[i]);
                if (m > plen && starts_with(src, len, pos, k_punct_multi[i]))
                    plen = m;
            }
            if (plen == 0) {
                if (strchr("#@[](){}.&*+-~!/%<>^|?:;=,", (char)c))
                    plen = 1;
                else
                    return -1;
            }
            pos += plen;
        }

        if (nt >= cap) {
            int nc = cap ? cap * 2 : 64;
            CpTok *nb = (CpTok *)realloc(toks, (size_t)nc * sizeof(CpTok));
            if (!nb) {
                free(toks);
                return -1;
            }
            toks = nb;
            cap = nc;
        }
        t = &toks[nt++];
        t->kind = kind;
        t->offset = (size_t)start;
        t->len = (size_t)(pos - start);
        t->ptr = src + start;
        t->file_id = 1;
    }
    *out = toks;
    *n = nt;
    return 0;
}

static const char *tok_kind_name(CpTokKind k) {
    switch (k) {
    case CP_TOK_IDENT: return "IDENT";
    case CP_TOK_NUM: return "NUM";
    case CP_TOK_STR: return "STR";
    case CP_TOK_CHR: return "CHR";
    case CP_TOK_PUNCT: return "PUNCT";
    case CP_TOK_EOF: return "EOF";
    default: return "?";
    }
}

int cparse_dump_tokens(const char *src, const CpTok *toks, int n, char **out,
                       size_t *len) {
    size_t cap = 256, used = 0;
    char *buf = (char *)malloc(cap);
    int i;
    if (!buf) return -1;
    buf[0] = 0;
    for (i = 0; i < n; i++) {
        char line[512];
        const char *sp = toks[i].ptr ? toks[i].ptr : src + toks[i].offset;
        int m = snprintf(line, sizeof(line), "%s %.*s\n", tok_kind_name(toks[i].kind),
                         (int)toks[i].len, sp);
        if (m < 0) {
            free(buf);
            return -1;
        }
        if (used + (size_t)m + 1 > cap) {
            size_t nc = cap * 2;
            char *nb;
            while (nc < used + (size_t)m + 1) nc *= 2;
            nb = (char *)realloc(buf, nc);
            if (!nb) {
                free(buf);
                return -1;
            }
            buf = nb;
            cap = nc;
        }
        memcpy(buf + used, line, (size_t)m);
        used += (size_t)m;
        buf[used] = 0;
    }
    *out = buf;
    if (len) *len = used;
    return 0;
}

/* ---- directives from '#' at bol + rest of physical line ---- */

typedef enum {
    DIR_NONE = 0,
    DIR_IF,
    DIR_IFDEF,
    DIR_IFNDEF,
    DIR_ELSE,
    DIR_ELIF,
    DIR_ENDIF,
    DIR_DEFINE,
    DIR_UNDEF,
    DIR_OTHER
} DirKind;

static int at_bol(const char *src, size_t off) {
    size_t i = off;
    while (i > 0 && src[i - 1] != '\n') {
        char c = src[i - 1];
        if (c != ' ' && c != '\t' && c != '\r' && c != '\v' && c != '\f')
            return 0;
        i--;
    }
    return 1;
}

static int dir_line_end(const Parser *ps, size_t start) {
    int end = (int)start;
    while (end < ps->out->len && ps->out->src[end] != '\n') end++;
    return end;
}

static const char *tok_ptr(const Parser *ps, const CpTok *t) {
    if (t->ptr) return t->ptr;
    if (ps->out->src && (int)t->offset < ps->out->len) return ps->out->src + t->offset;
    return "";
}

static int is_hash_bol(const Parser *ps) {
    const CpTok *t = cur(ps);
    return t->kind == CP_TOK_PUNCT && t->len == 1 &&
           ps->out->src[t->offset] == '#' && at_bol(ps->out->src, t->offset);
}

/* Stage-2 FileTape: the whole `#ifdef NAME` line is one IDENT. */
static int is_glued_dir(const Parser *ps) {
    const CpTok *t = cur(ps);
    const char *p;
    if (t->kind != CP_TOK_IDENT || t->len < 2) return 0;
    p = tok_ptr(ps, t);
    return p[0] == '#';
}

static void dir_trim(const char *s, int *lo, int *hi) {
    while (*lo < *hi && isspace((unsigned char)s[*lo])) (*lo)++;
    while (*hi > *lo && isspace((unsigned char)s[*hi - 1])) (*hi)--;
}

static DirKind dir_kind_slice(const char *s, int lo, int hi, int *arg_lo,
                              int *arg_hi) {
    int wlo, whi;
    dir_trim(s, &lo, &hi);
    if (lo < hi && s[lo] == '#') lo++;
    dir_trim(s, &lo, &hi);
    wlo = lo;
    while (lo < hi && (isalnum((unsigned char)s[lo]) || s[lo] == '_')) lo++;
    whi = lo;
    dir_trim(s, &lo, &hi);
    if (arg_lo) *arg_lo = lo;
    if (arg_hi) *arg_hi = hi;
    if (whi - wlo == 2 && memcmp(s + wlo, "if", 2) == 0) return DIR_IF;
    if (whi - wlo == 5 && memcmp(s + wlo, "ifdef", 5) == 0) return DIR_IFDEF;
    if (whi - wlo == 6 && memcmp(s + wlo, "ifndef", 6) == 0) return DIR_IFNDEF;
    if (whi - wlo == 4 && memcmp(s + wlo, "else", 4) == 0) return DIR_ELSE;
    if (whi - wlo == 4 && memcmp(s + wlo, "elif", 4) == 0) return DIR_ELIF;
    if (whi - wlo == 5 && memcmp(s + wlo, "endif", 5) == 0) return DIR_ENDIF;
    if (whi - wlo == 6 && memcmp(s + wlo, "define", 6) == 0) return DIR_DEFINE;
    if (whi - wlo == 5 && memcmp(s + wlo, "undef", 5) == 0) return DIR_UNDEF;
    return DIR_OTHER;
}

static int peek_dir(const Parser *ps, DirKind *k, int *arg_lo, int *arg_hi,
                    int *line_lo, int *line_hi) {
    int lo, hi;
    const CpTok *t = cur(ps);
    if (is_hash_bol(ps)) {
        lo = (int)t->offset;
        hi = dir_line_end(ps, (size_t)lo);
    } else if (is_glued_dir(ps)) {
        lo = (int)t->offset;
        hi = (int)t->offset + (int)t->len;
    } else {
        return 0;
    }
    if (line_lo) *line_lo = lo;
    if (line_hi) *line_hi = hi;
    *k = dir_kind_slice(ps->out->src, lo, hi, arg_lo, arg_hi);
    return 1;
}

static void consume_dir_line(Parser *ps, int line_hi) {
    while (ps->i < ps->ntoks && (int)ps->toks[ps->i].offset < line_hi) ps->i++;
}

static char *dir_first_ident(Parser *ps, int lo, int hi) {
    const char *s = ps->out->src;
    int a = lo, b;
    while (a < hi && isspace((unsigned char)s[a])) a++;
    b = a;
    if (a < hi && (isalpha((unsigned char)s[a]) || s[a] == '_')) {
        b++;
        while (b < hi && (isalnum((unsigned char)s[b]) || s[b] == '_')) b++;
        return cp_str(s + a, b - a);
    }
    return cp_str(s + lo, hi > lo ? hi - lo : 0);
}

/* ---- parse ---- */

static int parse_group(Parser *ps, CpNode ***arr, int *n, int in_struct,
                       int stop_else_endif);
static int parse_if(Parser *ps, CpNode **out, int in_struct);

static int skip_attr(Parser *ps) {
    int depth;
    if (!tok_ident(ps, "__attribute__")) return 1;
    adv(ps);
    if (!tok_is(ps, CP_TOK_PUNCT, "("))
        return fail(ps, "expected '(' after __attribute__");
    depth = 0;
    do {
        if (tok_is(ps, CP_TOK_PUNCT, "(")) depth++;
        else if (tok_is(ps, CP_TOK_PUNCT, ")")) depth--;
        else if (cur(ps)->kind == CP_TOK_EOF)
            return fail(ps, "unclosed __attribute__");
        adv(ps);
    } while (depth > 0);
    return 1;
}

static int is_c_type_kw(const Parser *ps) {
    return tok_ident(ps, "const") || tok_ident(ps, "volatile") ||
           tok_ident(ps, "restrict") || tok_ident(ps, "_Atomic") ||
           tok_ident(ps, "unsigned") || tok_ident(ps, "signed") ||
           tok_ident(ps, "long") || tok_ident(ps, "short") ||
           tok_ident(ps, "int") || tok_ident(ps, "char") ||
           tok_ident(ps, "void") || tok_ident(ps, "bool") ||
           tok_ident(ps, "_Bool") || tok_ident(ps, "float") ||
           tok_ident(ps, "double") || tok_ident(ps, "struct") ||
           tok_ident(ps, "union") || tok_ident(ps, "enum");
}

static int parse_field(Parser *ps, CpNode **out) {
    CpNode *n;
    int start = (int)cur(ps)->offset;
    char *name = NULL;
    int depth = 0;
    int saw_builtin = 0;
    int after_sue = 0;
    int n_user = 0;
    if (cur(ps)->kind == CP_TOK_EOF || is_hash_bol(ps) ||
        tok_is(ps, CP_TOK_PUNCT, "}"))
        return fail(ps, "expected field");
    while (cur(ps)->kind != CP_TOK_EOF) {
        if (depth == 0 && (is_hash_bol(ps) || tok_is(ps, CP_TOK_PUNCT, "}")))
            break;
        if (depth == 0 && tok_is(ps, CP_TOK_PUNCT, ";")) break;
        if (tok_is(ps, CP_TOK_PUNCT, "{") || tok_is(ps, CP_TOK_PUNCT, "(") ||
            tok_is(ps, CP_TOK_PUNCT, "["))
            depth++;
        else if (tok_is(ps, CP_TOK_PUNCT, "}") || tok_is(ps, CP_TOK_PUNCT, ")") ||
                 tok_is(ps, CP_TOK_PUNCT, "]")) {
            if (depth > 0) depth--;
        } else if (depth == 0 && cur(ps)->kind == CP_TOK_IDENT) {
            if (tok_ident(ps, "__attribute__")) {
                if (!skip_attr(ps)) {
                    free(name);
                    return 0;
                }
                continue;
            }
            if (tok_ident(ps, "struct") || tok_ident(ps, "union") ||
                tok_ident(ps, "enum")) {
                saw_builtin = 1;
                after_sue = 1;
            } else if (is_c_type_kw(ps)) {
                saw_builtin = 1;
                after_sue = 0;
            } else if (after_sue) {
                after_sue = 0; /* tag */
            } else {
                n_user++;
                if (saw_builtin && n_user > 1) {
                    free(name);
                    return fail(ps, "expected ',' or ';' in field");
                }
            }
            free(name);
            name = spell_dup(ps);
            if (!name) return fail(ps, "oom");
        }
        adv(ps);
    }
    if (!tok_is(ps, CP_TOK_PUNCT, ";")) {
        free(name);
        return fail(ps, "expected ';' after field");
    }
    if (!name) return fail(ps, "field missing name");
    n = node_new(ps, CP_FIELD, start);
    if (!n) {
        free(name);
        return fail(ps, "oom");
    }
    n->name = name;
    n->end = (int)(cur(ps)->offset + cur(ps)->len);
    adv(ps);
    *out = n;
    return 1;
}

static int parse_struct(Parser *ps, CpNode **out) {
    CpNode *n;
    int start = (int)cur(ps)->offset;
    if (!tok_ident(ps, "typedef")) return fail(ps, "expected typedef");
    adv(ps);
    if (!tok_ident(ps, "struct")) return fail(ps, "expected 'struct' after typedef");
    adv(ps);
    n = node_new(ps, CP_STRUCT, start);
    if (!n) return fail(ps, "oom");
    if (cur(ps)->kind == CP_TOK_IDENT) {
        n->name = spell_dup(ps);
        if (!n->name) return fail(ps, "oom");
        adv(ps);
    }
    if (!tok_is(ps, CP_TOK_PUNCT, "{")) {
        /* `typedef struct Tag Alias;` — no field list */
        if (cur(ps)->kind == CP_TOK_IDENT) {
            free(n->name);
            n->name = spell_dup(ps);
            if (!n->name) return fail(ps, "oom");
            adv(ps);
        }
        if (!tok_is(ps, CP_TOK_PUNCT, ";"))
            return fail(ps, "expected ';' after typedef struct");
        n->kind = CP_TYPEDEF;
        n->end = (int)(cur(ps)->offset + cur(ps)->len);
        adv(ps);
        *out = n;
        return 1;
    }
    adv(ps);
    if (!parse_group(ps, &n->kids, &n->nkid, 1, 1)) return 0;
    if (!tok_is(ps, CP_TOK_PUNCT, "}")) return fail(ps, "expected '}' after struct fields");
    adv(ps);
    if (cur(ps)->kind == CP_TOK_IDENT) {
        if (!n->name) {
            n->name = spell_dup(ps);
            if (!n->name) return fail(ps, "oom");
        }
        adv(ps);
    }
    if (!tok_is(ps, CP_TOK_PUNCT, ";"))
        return fail(ps, "expected ';' after typedef struct");
    n->end = (int)(cur(ps)->offset + cur(ps)->len);
    adv(ps);
    *out = n;
    return 1;
}

/* File-scope `struct Tag;` / `struct Tag { … };` (and `union`). */
static int parse_struct_tag(Parser *ps, CpNode **out) {
    int start = (int)cur(ps)->offset;
    CpNode *n;
    if (!tok_ident(ps, "struct") && !tok_ident(ps, "union"))
        return fail(ps, "expected struct or union");
    adv(ps);
    n = node_new(ps, CP_STRUCT, start);
    if (!n) return fail(ps, "oom");
    if (cur(ps)->kind == CP_TOK_IDENT) {
        n->name = spell_dup(ps);
        if (!n->name) return fail(ps, "oom");
        adv(ps);
    }
    if (tok_is(ps, CP_TOK_PUNCT, "{")) {
        adv(ps);
        if (!parse_group(ps, &n->kids, &n->nkid, 1, 1)) return 0;
        if (!tok_is(ps, CP_TOK_PUNCT, "}"))
            return fail(ps, "expected '}' after struct fields");
        adv(ps);
    } else if (!n->name) {
        return fail(ps, "expected struct tag or '{'");
    }
    if (!tok_is(ps, CP_TOK_PUNCT, ";"))
        return fail(ps, "expected ';' after struct");
    n->end = (int)(cur(ps)->offset + cur(ps)->len);
    adv(ps);
    *out = n;
    return 1;
}

/* `extern "C" {` / `extern "C++" {` — opener only. The matching `}` is
 * often in a later `#ifdef __cplusplus` arm, so we do not nest a group. */
static int parse_extern_link_open(Parser *ps, CpNode **out) {
    int start = (int)cur(ps)->offset;
    CpNode *n;
    const CpTok *lang;
    if (!tok_ident(ps, "extern")) return fail(ps, "expected extern");
    if (!tok_n_is(ps, 1, CP_TOK_STR, "\"C\"") &&
        !tok_n_is(ps, 1, CP_TOK_STR, "\"C++\""))
        return fail(ps, "expected extern \"C\"");
    if (!tok_n_is(ps, 2, CP_TOK_PUNCT, "{"))
        return fail(ps, "expected '{' after extern \"C\"");
    lang = &ps->toks[ps->i + 1];
    n = node_new(ps, CP_DIR, start);
    if (!n) return fail(ps, "oom");
    n->name = cp_str("extern", 6);
    n->attr = cp_str(ps->out->src + lang->offset, (int)lang->len);
    if (!n->name || !n->attr) return fail(ps, "oom");
    adv(ps);
    adv(ps);
    n->end = (int)(cur(ps)->offset + cur(ps)->len);
    adv(ps);
    *out = n;
    return 1;
}

static int parse_link_close(Parser *ps, CpNode **out) {
    CpNode *n;
    int start = (int)cur(ps)->offset;
    if (!tok_is(ps, CP_TOK_PUNCT, "}")) return fail(ps, "expected '}'");
    n = node_new(ps, CP_DIR, start);
    if (!n) return fail(ps, "oom");
    n->name = cp_str("extern", 6);
    n->attr = cp_str("}", 1);
    if (!n->name || !n->attr) return fail(ps, "oom");
    n->end = (int)(cur(ps)->offset + cur(ps)->len);
    adv(ps);
    *out = n;
    return 1;
}

static int parse_func(Parser *ps, CpNode **out) {
    CpNode *n;
    int start = (int)cur(ps)->offset;
    int attr_lo = -1, attr_hi = -1;
    char *name = NULL;
    int depth, body_end = start;
    if (cur(ps)->kind == CP_TOK_EOF) return fail(ps, "expected declaration");
    for (;;) {
        if (tok_ident(ps, "__attribute__")) {
            attr_lo = (int)cur(ps)->offset;
            if (!skip_attr(ps)) return 0;
            attr_hi = (int)cur(ps)->offset;
            continue;
        }
        if (cur(ps)->kind == CP_TOK_IDENT) {
            int save = ps->i;
            char *cand = spell_dup(ps);
            adv(ps);
            while (tok_is(ps, CP_TOK_PUNCT, "*")) adv(ps);
            if (tok_is(ps, CP_TOK_PUNCT, "(")) {
                name = cand;
                break;
            }
            free(cand);
            ps->i = save;
            adv(ps);
            continue;
        }
        if (tok_is(ps, CP_TOK_PUNCT, "*")) {
            adv(ps);
            continue;
        }
        return fail(ps, "expected function declarator");
    }
    if (!name) return fail(ps, "function missing name");
    if (!tok_is(ps, CP_TOK_PUNCT, "("))
        return fail(ps, "expected '(' after function name");
    depth = 0;
    do {
        if (tok_is(ps, CP_TOK_PUNCT, "(")) depth++;
        else if (tok_is(ps, CP_TOK_PUNCT, ")")) depth--;
        else if (cur(ps)->kind == CP_TOK_EOF)
            return fail(ps, "unclosed function parameter list");
        adv(ps);
    } while (depth > 0);
    while (tok_ident(ps, "__attribute__")) {
        attr_lo = (int)cur(ps)->offset;
        if (!skip_attr(ps)) return 0;
        attr_hi = (int)cur(ps)->offset;
    }
    if (tok_is(ps, CP_TOK_PUNCT, ";")) {
        body_end = (int)(cur(ps)->offset + cur(ps)->len);
        adv(ps);
    } else if (tok_is(ps, CP_TOK_PUNCT, "{")) {
        depth = 0;
        do {
            if (tok_is(ps, CP_TOK_PUNCT, "{")) depth++;
            else if (tok_is(ps, CP_TOK_PUNCT, "}")) {
                depth--;
                body_end = (int)(cur(ps)->offset + cur(ps)->len);
            } else if (cur(ps)->kind == CP_TOK_EOF)
                return fail(ps, "unclosed function body");
            adv(ps);
        } while (depth > 0);
    } else {
        free(name);
        return fail(ps, "expected '{' or ';' after function params");
    }
    n = node_new(ps, CP_FUNC, start);
    if (!n) {
        free(name);
        return fail(ps, "oom");
    }
    n->name = name;
    if (attr_lo >= 0 && attr_hi > attr_lo) {
        while (attr_hi > attr_lo &&
               isspace((unsigned char)ps->out->src[attr_hi - 1]))
            attr_hi--;
        n->attr = cp_str(ps->out->src + attr_lo, attr_hi - attr_lo);
    }
    n->end = body_end;
    *out = n;
    return 1;
}

static int parse_define(Parser *ps, CpNode **out) {
    int arg_lo, arg_hi, line_lo, line_hi, nlo, nhi;
    DirKind dk;
    CpNode *n;
    const char *s;
    if (!peek_dir(ps, &dk, &arg_lo, &arg_hi, &line_lo, &line_hi) ||
        dk != DIR_DEFINE)
        return fail(ps, "expected #define");
    s = ps->out->src;
    nlo = arg_lo;
    while (nlo < arg_hi && isspace((unsigned char)s[nlo])) nlo++;
    nhi = nlo;
    if (nlo >= arg_hi || !(isalpha((unsigned char)s[nlo]) || s[nlo] == '_'))
        return fail(ps, "#define missing name");
    nhi++;
    while (nhi < arg_hi && (isalnum((unsigned char)s[nhi]) || s[nhi] == '_'))
        nhi++;
    n = node_new(ps, CP_DEFINE, line_lo);
    if (!n) return fail(ps, "oom");
    n->name = cp_str(s + nlo, nhi - nlo);
    if (nhi < arg_hi && s[nhi] == '(') {
        n->is_func = 1;
        n->attr = cp_str(s + nhi, arg_hi - nhi);
        if (!n->attr) return fail(ps, "oom");
    } else {
        int blo = nhi;
        while (blo < arg_hi && isspace((unsigned char)s[blo])) blo++;
        n->attr = cp_str(s + blo, arg_hi > blo ? arg_hi - blo : 0);
        if (!n->attr) return fail(ps, "oom");
    }
    n->end = line_hi;
    consume_dir_line(ps, line_hi);
    *out = n;
    return 1;
}

static int parse_dir_passthrough(Parser *ps, CpNode **out) {
    int arg_lo, arg_hi, line_lo, line_hi, lo, hi, wlo, whi;
    DirKind dk;
    CpNode *n;
    const char *s;
    if (!peek_dir(ps, &dk, &arg_lo, &arg_hi, &line_lo, &line_hi))
        return fail(ps, "expected directive");
    s = ps->out->src;
    n = node_new(ps, CP_DIR, line_lo);
    if (!n) return fail(ps, "oom");
    lo = line_lo;
    hi = line_hi;
    dir_trim(s, &lo, &hi);
    if (lo < hi && s[lo] == '#') lo++;
    dir_trim(s, &lo, &hi);
    wlo = lo;
    while (lo < hi && (isalnum((unsigned char)s[lo]) || s[lo] == '_')) lo++;
    whi = lo;
    n->name = cp_str(s + wlo, whi > wlo ? whi - wlo : 0);
    if (!n->name) return fail(ps, "oom");
    if (dk == DIR_UNDEF) {
        int ulo = arg_lo, uhi;
        while (ulo < arg_hi && isspace((unsigned char)s[ulo])) ulo++;
        uhi = ulo;
        while (uhi < arg_hi &&
               (isalnum((unsigned char)s[uhi]) || s[uhi] == '_'))
            uhi++;
        if (uhi > ulo) {
            n->attr = cp_str(s + ulo, uhi - ulo);
            if (!n->attr) return fail(ps, "oom");
        }
    }
    n->end = line_hi;
    consume_dir_line(ps, line_hi);
    *out = n;
    return 1;
}

static int parse_raw_decl(Parser *ps, CpNode **out) {
    int start = (int)cur(ps)->offset;
    int depth = 0;
    int last_lo = -1, last_n = 0;
    CpNode *n;
    while (cur(ps)->kind != CP_TOK_EOF) {
        if (depth == 0 && tok_is(ps, CP_TOK_PUNCT, ";")) {
            n = node_new(ps, CP_TYPEDEF, start);
            if (!n) return fail(ps, "oom");
            n->end = (int)(cur(ps)->offset + cur(ps)->len);
            if (last_n > 0) {
                n->name = cp_str(ps->out->src + last_lo, last_n);
                if (!n->name) return fail(ps, "oom");
            }
            adv(ps);
            *out = n;
            return 1;
        }
        if (cur(ps)->kind == CP_TOK_IDENT) {
            last_lo = (int)cur(ps)->offset;
            last_n = (int)cur(ps)->len;
        }
        if (tok_is(ps, CP_TOK_PUNCT, "{") || tok_is(ps, CP_TOK_PUNCT, "(") ||
            tok_is(ps, CP_TOK_PUNCT, "["))
            depth++;
        else if ((tok_is(ps, CP_TOK_PUNCT, "}") || tok_is(ps, CP_TOK_PUNCT, ")") ||
                  tok_is(ps, CP_TOK_PUNCT, "]")) &&
                 depth > 0)
            depth--;
        adv(ps);
    }
    return fail(ps, "unterminated declaration");
}

typedef struct {
    const char *src;
    const CpTok *toks;
    int n;
    int i;
    int no_eval;
    int (*isdef)(void *ctx, const char *name, size_t len);
    void *ctx;
    const CpIfOpts *opts;
    char err[192];
} IfP;

static const CpTok *ifp_peek(const IfP *p) {
    if (p->i < 0 || p->i >= p->n) return NULL;
    return &p->toks[p->i];
}

static int ifp_is(const IfP *p, CpTokKind k, const char *lit) {
    const CpTok *t = ifp_peek(p);
    size_t n;
    const char *sp;
    if (!t || t->kind != k) return 0;
    if (!lit) return 1;
    n = strlen(lit);
    if (t->len != n) return 0;
    sp = t->ptr ? t->ptr : p->src + t->offset;
    return memcmp(sp, lit, n) == 0;
}

static int ifp_fail(IfP *p, const char *m) {
    snprintf(p->err, sizeof(p->err), "%s", m);
    return 0;
}

static int ifp_cond(IfP *p, long long *out);

static const char *ifp_spell(const IfP *p, const CpTok *t) {
    return t->ptr ? t->ptr : p->src + t->offset;
}

static int ifp_name_is(const char *sp, size_t n, const char *lit) {
    size_t m = strlen(lit);
    return n == m && memcmp(sp, lit, m) == 0;
}

static int ifp_is_has_name(const char *sp, size_t n) {
    return ifp_name_is(sp, n, "__has_feature") ||
           ifp_name_is(sp, n, "__has_builtin") ||
           ifp_name_is(sp, n, "__has_include");
}

static char *g_sys_inc[16];
static int g_nsys_inc;
static int g_sys_ready;

static void cp_sys_inc_add(const char *dir) {
    char *d;
    size_t n;
    if (!dir || !dir[0] || g_nsys_inc >= (int)(sizeof(g_sys_inc) / sizeof(g_sys_inc[0])))
        return;
    if (dir[0] != '/') return;
    n = strlen(dir);
    d = (char *)malloc(n + 1);
    if (!d) return;
    memcpy(d, dir, n + 1);
    g_sys_inc[g_nsys_inc++] = d;
}

/* Emit-compiler include roots (`CC=` / `cc`). stdbool.h lives in clang's
 * resource dir, not the SDK's /usr/include. */
static void cp_sys_inc_from_cc(const char *flag, int append_include) {
    const char *cc = getenv("CC");
    char cmd[512], buf[512];
    FILE *f;
    size_t n;
    if (!cc || !cc[0] || strchr(cc, ' ') || strchr(cc, ';') || strchr(cc, '|') ||
        strchr(cc, '&'))
        cc = "cc";
    if (snprintf(cmd, sizeof(cmd), "%s %s 2>/dev/null", cc, flag) >=
        (int)sizeof(cmd))
        return;
    f = popen(cmd, "r");
    if (!f) return;
    if (fgets(buf, sizeof(buf), f)) {
        n = strlen(buf);
        while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) buf[--n] = 0;
        if (append_include && n > 0 && n + 9 < sizeof(buf)) {
            memcpy(buf + n, "/include", 9);
            n += 8;
        }
        if (n > 0) cp_sys_inc_add(buf);
    }
    pclose(f);
}

static void cp_sys_inc_init(void) {
    if (g_sys_ready) return;
    g_sys_ready = 1;
    cp_sys_inc_from_cc("-print-resource-dir", 1);
    cp_sys_inc_from_cc("-print-file-name=include", 0);
    cp_sys_inc_add("/usr/include");
    cp_sys_inc_add("/usr/local/include");
#ifdef __APPLE__
    {
        FILE *f = popen("xcrun --show-sdk-path 2>/dev/null", "r");
        if (f) {
            char buf[512];
            if (fgets(buf, sizeof(buf), f)) {
                size_t n = strlen(buf);
                while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r'))
                    buf[--n] = 0;
                if (n > 0 && n + 13 < sizeof(buf)) {
                    memcpy(buf + n, "/usr/include", 13);
                    cp_sys_inc_add(buf);
                }
            }
            pclose(f);
        }
    }
#endif
}

static int cp_readable(const char *path) {
    return path && path[0] && access(path, R_OK) == 0;
}

static int cp_join_ok(const char *dir, const char *rel, char *buf, size_t cap) {
    if (!dir || !dir[0] || !rel) return 0;
    if (snprintf(buf, cap, "%s/%s", dir, rel) >= (int)cap) return 0;
    return cp_readable(buf);
}

static int cp_has_include(IfP *p, const char *path, int angle) {
    char buf[1024];
    int i;
    if (!path || !path[0]) return 0;
    if (path[0] == '/') return cp_readable(path);
    if (!angle) {
        if (p->opts && p->opts->file_dir &&
            cp_join_ok(p->opts->file_dir, path, buf, sizeof(buf)))
            return 1;
        if (cp_readable(path)) return 1;
    }
    if (p->opts) {
        for (i = 0; i < p->opts->ninc; i++) {
            if (cp_join_ok(p->opts->inc_dirs[i], path, buf, sizeof(buf)))
                return 1;
        }
    }
    cp_sys_inc_init();
    for (i = 0; i < g_nsys_inc; i++) {
        if (cp_join_ok(g_sys_inc[i], path, buf, sizeof(buf))) return 1;
    }
    return 0;
}

static int cp_has_feature(IfP *p, const char *name, size_t n) {
    if (ifp_name_is(name, n, "thread_sanitizer"))
        return p->isdef && p->isdef(p->ctx, "__SANITIZE_THREAD__", 19);
    if (ifp_name_is(name, n, "address_sanitizer"))
        return p->isdef && p->isdef(p->ctx, "__SANITIZE_ADDRESS__", 20);
    return 0;
}

/* Emit-CC answers, not "whoever built libcparse". One clang/gcc set
 * for now. Later: table per backend; unknown name → probe `$CC -E`
 * and cache (compiler, name). See docs/c-parser.md. */
static const char *k_has_builtins[] = {
    "__builtin_add_overflow",
    "__builtin_sub_overflow",
    "__builtin_mul_overflow",
    "__builtin_expect",
    "__builtin_bswap16",
    "__builtin_bswap32",
    "__builtin_bswap64",
    "__builtin_clz",
    "__builtin_clzl",
    "__builtin_clzll",
    "__builtin_ctz",
    "__builtin_ctzl",
    "__builtin_ctzll",
    "__builtin_popcount",
    "__builtin_popcountll",
    "__builtin_unreachable",
    "__builtin_trap",
    "__builtin_assume_aligned",
    NULL
};

static int cp_has_builtin(const char *name, size_t n) {
    int i;
    for (i = 0; k_has_builtins[i]; i++) {
        if (ifp_name_is(name, n, k_has_builtins[i])) return 1;
    }
    return 0;
}

static int ifp_num(const char *s, size_t n, long long *out) {
    char tmp[80];
    char *end = NULL;
    unsigned long long u;
    int j;
    if (n == 0 || n >= sizeof(tmp)) return 0;
    memcpy(tmp, s, n);
    tmp[n] = 0;
    j = (int)n;
    while (j > 0 && (tmp[j - 1] == 'u' || tmp[j - 1] == 'U' ||
                     tmp[j - 1] == 'l' || tmp[j - 1] == 'L'))
        tmp[--j] = 0;
    if (j == 0) return 0;
    u = strtoull(tmp, &end, 0);
    if (!end || *end) return 0;
    *out = (long long)u;
    return 1;
}

static int ifp_skip_parens(IfP *p) {
    int depth;
    if (!ifp_is(p, CP_TOK_PUNCT, "(")) return ifp_fail(p, "expected '('");
    depth = 0;
    do {
        if (ifp_is(p, CP_TOK_PUNCT, "(")) depth++;
        else if (ifp_is(p, CP_TOK_PUNCT, ")")) depth--;
        else if (!ifp_peek(p)) return ifp_fail(p, "unclosed '(' in #if");
        p->i++;
    } while (depth > 0);
    return 1;
}

static int ifp_has_include(IfP *p, long long *out) {
    char path[512];
    int n = 0;
    int angle = 0;
    p->i++;
    if (!ifp_is(p, CP_TOK_PUNCT, "("))
        return ifp_fail(p, "__has_include missing '('");
    p->i++;
    if (ifp_peek(p) && ifp_peek(p)->kind == CP_TOK_STR) {
        const CpTok *t = ifp_peek(p);
        const char *sp = ifp_spell(p, t);
        size_t len = t->len;
        if (len >= 2 && (sp[0] == '"' || sp[0] == '\'')) {
            sp++;
            len -= 2;
        }
        if (len >= sizeof(path)) return ifp_fail(p, "__has_include path too long");
        memcpy(path, sp, len);
        path[len] = 0;
        p->i++;
    } else if (ifp_is(p, CP_TOK_PUNCT, "<")) {
        angle = 1;
        p->i++;
        while (!ifp_is(p, CP_TOK_PUNCT, ">")) {
            const CpTok *t = ifp_peek(p);
            const char *sp;
            if (!t) return ifp_fail(p, "unclosed __has_include <");
            sp = ifp_spell(p, t);
            if (n + (int)t->len >= (int)sizeof(path))
                return ifp_fail(p, "__has_include path too long");
            memcpy(path + n, sp, t->len);
            n += (int)t->len;
            p->i++;
        }
        path[n] = 0;
        p->i++;
    } else {
        return ifp_fail(p, "__has_include wants \"file\" or <file>");
    }
    if (!ifp_is(p, CP_TOK_PUNCT, ")"))
        return ifp_fail(p, "__has_include missing ')'");
    p->i++;
    *out = p->no_eval ? 0 : cp_has_include(p, path, angle);
    return 1;
}

static int ifp_has_ident_arg(IfP *p, const char *who, long long *out,
                             int (*fn)(IfP *, const char *, size_t)) {
    const CpTok *t;
    const char *sp;
    p->i++;
    if (!ifp_is(p, CP_TOK_PUNCT, "(")) {
        snprintf(p->err, sizeof(p->err), "%s missing '('", who);
        return 0;
    }
    p->i++;
    t = ifp_peek(p);
    if (!t || t->kind != CP_TOK_IDENT) {
        snprintf(p->err, sizeof(p->err), "%s missing name", who);
        return 0;
    }
    sp = ifp_spell(p, t);
    *out = p->no_eval ? 0 : fn(p, sp, t->len);
    p->i++;
    if (!ifp_is(p, CP_TOK_PUNCT, ")")) {
        snprintf(p->err, sizeof(p->err), "%s missing ')'", who);
        return 0;
    }
    p->i++;
    return 1;
}

static int ifp_feature_cb(IfP *p, const char *name, size_t n) {
    return cp_has_feature(p, name, n);
}

static int ifp_builtin_cb(IfP *p, const char *name, size_t n) {
    (void)p;
    return cp_has_builtin(name, n);
}

static int ifp_primary(IfP *p, long long *out) {
    const CpTok *t = ifp_peek(p);
    const char *sp;
    if (!t) return ifp_fail(p, "empty #if / #elif");
    if (ifp_is(p, CP_TOK_IDENT, "defined")) {
        size_t n;
        p->i++;
        if (ifp_is(p, CP_TOK_PUNCT, "(")) p->i++;
        t = ifp_peek(p);
        if (!t || t->kind != CP_TOK_IDENT)
            return ifp_fail(p, "defined() missing name");
        sp = ifp_spell(p, t);
        n = t->len;
        if (ifp_is_has_name(sp, n))
            *out = 1;
        else
            *out = p->isdef && p->isdef(p->ctx, sp, n) ? 1 : 0;
        p->i++;
        if (ifp_is(p, CP_TOK_PUNCT, ")")) p->i++;
        return 1;
    }
    if (ifp_is(p, CP_TOK_IDENT, "__has_include"))
        return ifp_has_include(p, out);
    if (ifp_is(p, CP_TOK_IDENT, "__has_feature"))
        return ifp_has_ident_arg(p, "__has_feature", out, ifp_feature_cb);
    if (ifp_is(p, CP_TOK_IDENT, "__has_builtin"))
        return ifp_has_ident_arg(p, "__has_builtin", out, ifp_builtin_cb);
    if (t->kind == CP_TOK_IDENT) {
        size_t n = t->len;
        sp = ifp_spell(p, t);
        p->i++;
        if (ifp_is(p, CP_TOK_PUNCT, "(")) {
            if (p->opts && p->opts->unknown_call) {
                if (!ifp_skip_parens(p)) return 0;
                *out = 0;
                return 1;
            }
            return ifp_fail(p, "unknown call in #if (not a macro or __has_*)");
        }
        (void)sp;
        (void)n;
        *out = 0;
        return 1;
    }
    if (t->kind == CP_TOK_NUM) {
        sp = ifp_spell(p, t);
        if (!ifp_num(sp, t->len, out))
            return ifp_fail(p, "bad #if number");
        p->i++;
        return 1;
    }
    if (t->kind == CP_TOK_CHR) {
        sp = ifp_spell(p, t);
        if (t->len >= 3 && sp[0] == '\'' && sp[t->len - 1] == '\'')
            *out = (unsigned char)sp[1];
        else
            return ifp_fail(p, "bad #if character constant");
        p->i++;
        return 1;
    }
    if (ifp_is(p, CP_TOK_PUNCT, "(")) {
        p->i++;
        if (!ifp_cond(p, out)) return 0;
        if (!ifp_is(p, CP_TOK_PUNCT, ")"))
            return ifp_fail(p, "expected ')' in #if");
        p->i++;
        return 1;
    }
    return ifp_fail(p, "bad #if primary");
}

static int ifp_unary(IfP *p, long long *out) {
    if (ifp_is(p, CP_TOK_PUNCT, "!")) {
        p->i++;
        if (!ifp_unary(p, out)) return 0;
        *out = !*out;
        return 1;
    }
    if (ifp_is(p, CP_TOK_PUNCT, "+")) {
        p->i++;
        return ifp_unary(p, out);
    }
    if (ifp_is(p, CP_TOK_PUNCT, "-")) {
        p->i++;
        if (!ifp_unary(p, out)) return 0;
        *out = -*out;
        return 1;
    }
    if (ifp_is(p, CP_TOK_PUNCT, "~")) {
        p->i++;
        if (!ifp_unary(p, out)) return 0;
        *out = ~*out;
        return 1;
    }
    return ifp_primary(p, out);
}

static int ifp_mul(IfP *p, long long *out) {
    long long r;
    if (!ifp_unary(p, out)) return 0;
    for (;;) {
        if (ifp_is(p, CP_TOK_PUNCT, "*")) {
            p->i++;
            if (!ifp_unary(p, &r)) return 0;
            *out = *out * r;
        } else if (ifp_is(p, CP_TOK_PUNCT, "/")) {
            p->i++;
            if (!ifp_unary(p, &r)) return 0;
            if (r == 0) {
                if (p->no_eval) *out = 0;
                else return ifp_fail(p, "#if division by zero");
            } else {
                *out = *out / r;
            }
        } else if (ifp_is(p, CP_TOK_PUNCT, "%")) {
            p->i++;
            if (!ifp_unary(p, &r)) return 0;
            if (r == 0) {
                if (p->no_eval) *out = 0;
                else return ifp_fail(p, "#if modulo by zero");
            } else {
                *out = *out % r;
            }
        } else {
            break;
        }
    }
    return 1;
}

static int ifp_add(IfP *p, long long *out) {
    long long r;
    if (!ifp_mul(p, out)) return 0;
    for (;;) {
        if (ifp_is(p, CP_TOK_PUNCT, "+")) {
            p->i++;
            if (!ifp_mul(p, &r)) return 0;
            *out = *out + r;
        } else if (ifp_is(p, CP_TOK_PUNCT, "-")) {
            p->i++;
            if (!ifp_mul(p, &r)) return 0;
            *out = *out - r;
        } else {
            break;
        }
    }
    return 1;
}

static int ifp_shift(IfP *p, long long *out) {
    long long r;
    if (!ifp_add(p, out)) return 0;
    for (;;) {
        if (ifp_is(p, CP_TOK_PUNCT, "<<")) {
            p->i++;
            if (!ifp_add(p, &r)) return 0;
            if (p->no_eval) *out = 0;
            else if (r < 0 || r >= 64)
                return ifp_fail(p, "#if shift out of range");
            else
                *out = (long long)((unsigned long long)*out << (unsigned)r);
        } else if (ifp_is(p, CP_TOK_PUNCT, ">>")) {
            p->i++;
            if (!ifp_add(p, &r)) return 0;
            if (p->no_eval) *out = 0;
            else if (r < 0 || r >= 64)
                return ifp_fail(p, "#if shift out of range");
            else
                *out = *out >> r;
        } else {
            break;
        }
    }
    return 1;
}

static int ifp_rel(IfP *p, long long *out) {
    long long r;
    if (!ifp_shift(p, out)) return 0;
    for (;;) {
        if (ifp_is(p, CP_TOK_PUNCT, "<")) {
            p->i++;
            if (!ifp_shift(p, &r)) return 0;
            *out = *out < r;
        } else if (ifp_is(p, CP_TOK_PUNCT, ">")) {
            p->i++;
            if (!ifp_shift(p, &r)) return 0;
            *out = *out > r;
        } else if (ifp_is(p, CP_TOK_PUNCT, "<=")) {
            p->i++;
            if (!ifp_shift(p, &r)) return 0;
            *out = *out <= r;
        } else if (ifp_is(p, CP_TOK_PUNCT, ">=")) {
            p->i++;
            if (!ifp_shift(p, &r)) return 0;
            *out = *out >= r;
        } else {
            break;
        }
    }
    return 1;
}

static int ifp_eq(IfP *p, long long *out) {
    long long r;
    if (!ifp_rel(p, out)) return 0;
    for (;;) {
        if (ifp_is(p, CP_TOK_PUNCT, "==")) {
            p->i++;
            if (!ifp_rel(p, &r)) return 0;
            *out = *out == r;
        } else if (ifp_is(p, CP_TOK_PUNCT, "!=")) {
            p->i++;
            if (!ifp_rel(p, &r)) return 0;
            *out = *out != r;
        } else {
            break;
        }
    }
    return 1;
}

static int ifp_bitand(IfP *p, long long *out) {
    long long r;
    if (!ifp_eq(p, out)) return 0;
    while (ifp_is(p, CP_TOK_PUNCT, "&")) {
        p->i++;
        if (!ifp_eq(p, &r)) return 0;
        *out = *out & r;
    }
    return 1;
}

static int ifp_bitxor(IfP *p, long long *out) {
    long long r;
    if (!ifp_bitand(p, out)) return 0;
    while (ifp_is(p, CP_TOK_PUNCT, "^")) {
        p->i++;
        if (!ifp_bitand(p, &r)) return 0;
        *out = *out ^ r;
    }
    return 1;
}

static int ifp_bitor(IfP *p, long long *out) {
    long long r;
    if (!ifp_bitxor(p, out)) return 0;
    while (ifp_is(p, CP_TOK_PUNCT, "|")) {
        p->i++;
        if (!ifp_bitxor(p, &r)) return 0;
        *out = *out | r;
    }
    return 1;
}

static int ifp_and(IfP *p, long long *out) {
    long long r;
    if (!ifp_bitor(p, out)) return 0;
    while (ifp_is(p, CP_TOK_PUNCT, "&&")) {
        int skip;
        p->i++;
        skip = (*out == 0);
        if (skip) p->no_eval++;
        if (!ifp_bitor(p, &r)) return 0;
        if (skip) p->no_eval--;
        *out = *out && r;
    }
    return 1;
}

static int ifp_or(IfP *p, long long *out) {
    long long r;
    if (!ifp_and(p, out)) return 0;
    while (ifp_is(p, CP_TOK_PUNCT, "||")) {
        int skip;
        p->i++;
        skip = (*out != 0);
        if (skip) p->no_eval++;
        if (!ifp_and(p, &r)) return 0;
        if (skip) p->no_eval--;
        *out = *out || r;
    }
    return 1;
}

static int ifp_cond(IfP *p, long long *out) {
    long long t, f;
    if (!ifp_or(p, out)) return 0;
    if (!ifp_is(p, CP_TOK_PUNCT, "?")) return 1;
    p->i++;
    if (*out) {
        if (!ifp_cond(p, &t)) return 0;
        if (!ifp_is(p, CP_TOK_PUNCT, ":")) return ifp_fail(p, "expected ':' in #if ?:");
        p->i++;
        p->no_eval++;
        if (!ifp_cond(p, &f)) return 0;
        p->no_eval--;
        *out = t;
    } else {
        p->no_eval++;
        if (!ifp_cond(p, &t)) return 0;
        p->no_eval--;
        if (!ifp_is(p, CP_TOK_PUNCT, ":")) return ifp_fail(p, "expected ':' in #if ?:");
        p->i++;
        if (!ifp_cond(p, &f)) return 0;
        *out = f;
    }
    return 1;
}

int cparse_eval_if_toks_ex(const char *src, const CpTok *toks, int n,
                           int (*isdef)(void *ctx, const char *name, size_t len),
                           void *ctx, long long *out, char *err, int errcap,
                           const CpIfOpts *opts) {
    IfP p;
    memset(&p, 0, sizeof(p));
    p.src = src ? src : "";
    p.toks = toks;
    p.n = n;
    p.isdef = isdef;
    p.ctx = ctx;
    p.opts = opts;
    if (!ifp_cond(&p, out)) {
        if (err && errcap) snprintf(err, (size_t)errcap, "%s", p.err);
        return -1;
    }
    if (p.i < p.n) {
        if (err && errcap)
            snprintf(err, (size_t)errcap, "#if trailing tokens");
        return -1;
    }
    return 0;
}

int cparse_eval_if_toks(const char *src, const CpTok *toks, int n,
                        int (*isdef)(void *ctx, const char *name, size_t len),
                        void *ctx, long long *out, char *err, int errcap) {
    return cparse_eval_if_toks_ex(src, toks, n, isdef, ctx, out, err, errcap,
                                  NULL);
}

static int ifp_no_defs(void *ctx, const char *name, size_t len) {
    (void)ctx;
    (void)name;
    (void)len;
    return 0;
}

static int parse_if_operand(Parser *ps, CpNode *n, int lo, int hi) {
    const char *s = ps->out->src;
    CpTok *toks = NULL;
    int nt = 0;
    long long v = 0;
    char err[192];
    dir_trim(s, &lo, &hi);
    if (lo >= hi) return fail(ps, "empty #if / #elif");
    n->name = cp_str(s + lo, hi - lo);
    if (!n->name) return fail(ps, "oom");
    n->if_form = CP_IF_EXPR;
    if (cparse_lex(n->name, hi - lo, &toks, &nt) != 0) {
        free(toks);
        return fail(ps, "bad #if / #elif tokens");
    }
    err[0] = 0;
    {
        CpIfOpts opts;
        memset(&opts, 0, sizeof(opts));
        opts.unknown_call = 1;
        if (cparse_eval_if_toks_ex(n->name, toks, nt, ifp_no_defs, NULL, &v, err,
                                   (int)sizeof(err), &opts) != 0) {
            free(toks);
            return fail(ps, err[0] ? err : "bad #if / #elif");
        }
    }
    free(toks);
    return 1;
}

static int parse_if_ex(Parser *ps, CpNode **out, int in_struct, int as_elif);

static int parse_if(Parser *ps, CpNode **out, int in_struct) {
    return parse_if_ex(ps, out, in_struct, 0);
}

static int parse_if_ex(Parser *ps, CpNode **out, int in_struct, int as_elif) {
    int arg_lo, arg_hi, line_lo, line_hi;
    DirKind dk;
    CpNode *n;
    if (!peek_dir(ps, &dk, &arg_lo, &arg_hi, &line_lo, &line_hi))
        return fail(ps, as_elif ? "expected #elif" : "expected #if / #ifdef / #ifndef");
    if (as_elif) {
        if (dk != DIR_ELIF) return fail(ps, "expected #elif");
    } else if (dk != DIR_IF && dk != DIR_IFDEF && dk != DIR_IFNDEF) {
        return fail(ps, "expected #if / #ifdef / #ifndef");
    }
    n = node_new(ps, CP_IF, line_lo);
    if (!n) return fail(ps, "oom");
    n->end = line_hi;
    n->is_elif = as_elif;
    if (dk == DIR_IFDEF) {
        n->if_form = CP_IF_IFDEF;
        n->name = dir_first_ident(ps, arg_lo, arg_hi);
    } else if (dk == DIR_IFNDEF) {
        n->if_form = CP_IF_IFNDEF;
        n->name = dir_first_ident(ps, arg_lo, arg_hi);
    } else if (!parse_if_operand(ps, n, arg_lo, arg_hi)) {
        return 0;
    }
    if (!n->name) return fail(ps, "oom");
    consume_dir_line(ps, line_hi);
    if (!parse_group(ps, &n->then_kids, &n->nthen, in_struct, 1)) return 0;
    if (peek_dir(ps, &dk, NULL, NULL, &line_lo, &line_hi)) {
        if (dk == DIR_ELIF) {
            CpNode *el = NULL;
            if (!parse_if_ex(ps, &el, in_struct, 1)) return 0;
            if (!kids_push(&n->else_kids, &n->nelse, el)) return fail(ps, "oom");
        } else if (dk == DIR_ELSE) {
            n->else_start = line_lo;
            n->else_end = line_hi;
            consume_dir_line(ps, line_hi);
            if (!parse_group(ps, &n->else_kids, &n->nelse, in_struct, 1))
                return 0;
        }
    }
    if (!as_elif) {
        if (!peek_dir(ps, &dk, NULL, NULL, &line_lo, &line_hi) ||
            dk != DIR_ENDIF)
            return fail(ps, "expected #endif");
        n->endif_start = line_lo;
        n->endif_end = line_hi;
        consume_dir_line(ps, line_hi);
    }
    *out = n;
    return 1;
}

static int parse_group(Parser *ps, CpNode ***arr, int *n, int in_struct,
                       int stop_else_endif) {
    for (;;) {
        CpNode *kid = NULL;
        DirKind dk;
        if (cur(ps)->kind == CP_TOK_EOF) {
            if (stop_else_endif && !in_struct)
                return fail(ps, "unclosed #if (missing #endif)");
            return 1;
        }
        if (in_struct && tok_is(ps, CP_TOK_PUNCT, "}")) return 1;
        if (peek_dir(ps, &dk, NULL, NULL, NULL, NULL)) {
            if (dk == DIR_ELSE || dk == DIR_ELIF || dk == DIR_ENDIF) {
                if (stop_else_endif) return 1;
                return fail(ps, "unexpected #else / #elif / #endif");
            }
            if (dk == DIR_IF || dk == DIR_IFDEF || dk == DIR_IFNDEF) {
                if (!parse_if(ps, &kid, in_struct)) return 0;
            } else if (dk == DIR_DEFINE) {
                if (in_struct)
                    return fail(ps, "#define inside struct is not step 1");
                if (!parse_define(ps, &kid)) return 0;
            } else if (dk == DIR_UNDEF || dk == DIR_OTHER) {
                if (in_struct)
                    return fail(ps, "directive inside struct is not step 1");
                if (!parse_dir_passthrough(ps, &kid)) return 0;
            } else {
                return fail(ps, "unsupported directive in step 1");
            }
        } else if (in_struct) {
            if (!parse_field(ps, &kid)) return 0;
        } else if (tok_ident(ps, "struct") || tok_ident(ps, "union")) {
            if (!parse_struct_tag(ps, &kid)) return 0;
        } else if (tok_ident(ps, "extern") &&
                   (tok_n_is(ps, 1, CP_TOK_STR, "\"C\"") ||
                    tok_n_is(ps, 1, CP_TOK_STR, "\"C++\"")) &&
                   tok_n_is(ps, 2, CP_TOK_PUNCT, "{")) {
            if (!parse_extern_link_open(ps, &kid)) return 0;
        } else if (!in_struct && tok_is(ps, CP_TOK_PUNCT, "}")) {
            if (!parse_link_close(ps, &kid)) return 0;
        } else if (tok_ident(ps, "typedef")) {
            if (ps->i + 1 < ps->ntoks) {
                const CpTok *nx = &ps->toks[ps->i + 1];
                if (nx->kind == CP_TOK_IDENT && nx->len == 6 &&
                    memcmp(ps->out->src + nx->offset, "struct", 6) == 0) {
                    if (!parse_struct(ps, &kid)) return 0;
                } else if (!parse_raw_decl(ps, &kid)) {
                    return 0;
                }
            } else {
                return fail(ps, "expected type after typedef");
            }
        } else if (tok_is(ps, CP_TOK_PUNCT, ";")) {
            adv(ps);
            continue;
        } else {
            if (!parse_func(ps, &kid)) return 0;
        }
        if (kid && !kids_push(arr, n, kid)) return fail(ps, "oom");
    }
}

static int parse_into(Parser *ps) {
    ps->out->root = node_new(ps, CP_TU, 0);
    if (!ps->out->root) return fail(ps, "oom");
    ps->out->root->end = ps->out->len;
    if (!parse_group(ps, &ps->out->root->kids, &ps->out->root->nkid, 0, 0))
        return 0;
    if (cur(ps)->kind != CP_TOK_EOF)
        return fail(ps, "trailing tokens after translation unit");
    return 1;
}

static int parse_into_fields(Parser *ps) {
    ps->out->root = node_new(ps, CP_TU, 0);
    if (!ps->out->root) return fail(ps, "oom");
    ps->out->root->end = ps->out->len;
    if (!parse_group(ps, &ps->out->root->kids, &ps->out->root->nkid, 1, 0))
        return 0;
    if (cur(ps)->kind != CP_TOK_EOF && !tok_is(ps, CP_TOK_PUNCT, "}"))
        return fail(ps, "trailing tokens after struct fields");
    return 1;
}

int cparse_tokens(const char *path, const char *src, int len,
                  const CpTok *toks, int ntoks, CpParse *out) {
    Parser ps;
    char *copy;
    if (len < 0) len = src ? (int)strlen(src) : 0;
    memset(out, 0, sizeof(*out));
    copy = (char *)malloc((size_t)len + 1);
    if (!copy) {
        out->err.msg = cp_str("oom", 3);
        return -1;
    }
    if (src && len) memcpy(copy, src, (size_t)len);
    copy[len] = 0;
    out->src = copy;
    out->len = len;
    memset(&ps, 0, sizeof(ps));
    ps.out = out;
    ps.path = path ? path : "<buffer>";
    ps.toks = toks;
    ps.ntoks = ntoks;
    if (!parse_into(&ps)) {
        char *m = out->err.msg;
        int line = out->err.line, col = out->err.col;
        out->err.msg = NULL;
        cparse_free(out);
        out->err.msg = m;
        out->err.line = line;
        out->err.col = col;
        return -1;
    }
    (void)path;
    return 0;
}

static int parse_copy_run(const char *path, const char *src, int len,
                          const CpTok *toks, int ntoks, CpParse *out,
                          int (*into)(Parser *)) {
    Parser ps;
    char *copy;
    if (len < 0) len = src ? (int)strlen(src) : 0;
    memset(out, 0, sizeof(*out));
    copy = (char *)malloc((size_t)len + 1);
    if (!copy) {
        out->err.msg = cp_str("oom", 3);
        return -1;
    }
    if (src && len) memcpy(copy, src, (size_t)len);
    copy[len] = 0;
    out->src = copy;
    out->len = len;
    memset(&ps, 0, sizeof(ps));
    ps.out = out;
    ps.path = path ? path : "<buffer>";
    ps.toks = toks;
    ps.ntoks = ntoks;
    if (!into(&ps)) {
        char *m = out->err.msg;
        int line = out->err.line, col = out->err.col;
        out->err.msg = NULL;
        cparse_free(out);
        out->err.msg = m;
        out->err.line = line;
        out->err.col = col;
        return -1;
    }
    return 0;
}

int cparse_field_group(const char *path, const char *src, int len,
                       const CpTok *toks, int ntoks, CpParse *out) {
    return parse_copy_run(path, src, len, toks, ntoks, out, parse_into_fields);
}

static void flat_trim(char *s) {
    int t = (int)strlen(s);
    while (t > 0 && (s[t - 1] == '\n' || s[t - 1] == '\r' || s[t - 1] == ' ' ||
                     s[t - 1] == '\t'))
        s[--t] = 0;
}

static int flat_one(const CpNode *node, const char *src, CpFlat *out, int cap,
                    int *np) {
    CpFlat *f;
    int i, lo, hi;
    if (!node || *np >= cap) return 0;
    if (node->kind == CP_TU || node->kind == CP_STRUCT) {
        for (i = 0; i < node->nkid; i++)
            if (!flat_one(node->kids[i], src, out, cap, np)) return 0;
        return 1;
    }
    if (node->kind == CP_IF) {
        if (*np >= cap) return 0;
        f = &out[(*np)++];
        memset(f, 0, sizeof(*f));
        f->kind = CP_FLAT_PPDIR;
        f->start = node->start;
        f->end = node->end;
        lo = node->start;
        hi = node->end;
        if (hi < lo) hi = lo;
        if (hi - lo >= (int)sizeof(f->text)) hi = lo + (int)sizeof(f->text) - 1;
        memcpy(f->text, src + lo, (size_t)(hi - lo));
        flat_trim(f->text);
        if (node->name) snprintf(f->name, sizeof(f->name), "%s", node->name);
        for (i = 0; i < node->nthen; i++)
            if (!flat_one(node->then_kids[i], src, out, cap, np)) return 0;
        if (node->nelse == 1 && node->else_kids[0] &&
            node->else_kids[0]->kind == CP_IF && node->else_kids[0]->is_elif) {
            if (!flat_one(node->else_kids[0], src, out, cap, np)) return 0;
        } else if (node->nelse || node->else_end > node->else_start) {
            if (*np >= cap) return 0;
            f = &out[(*np)++];
            memset(f, 0, sizeof(*f));
            f->kind = CP_FLAT_PPDIR;
            f->start = node->else_start;
            f->end = node->else_end;
            lo = node->else_start;
            hi = node->else_end;
            if (hi > lo) {
                if (hi - lo >= (int)sizeof(f->text))
                    hi = lo + (int)sizeof(f->text) - 1;
                memcpy(f->text, src + lo, (size_t)(hi - lo));
                flat_trim(f->text);
            } else {
                snprintf(f->text, sizeof(f->text), "#else");
            }
            snprintf(f->name, sizeof(f->name), "else");
            for (i = 0; i < node->nelse; i++)
                if (!flat_one(node->else_kids[i], src, out, cap, np)) return 0;
        }
        if (node->is_elif) return 1;
        if (*np >= cap) return 0;
        f = &out[(*np)++];
        memset(f, 0, sizeof(*f));
        f->kind = CP_FLAT_PPDIR;
        f->start = node->endif_start;
        f->end = node->endif_end;
        lo = node->endif_start;
        hi = node->endif_end;
        if (hi > lo) {
            if (hi - lo >= (int)sizeof(f->text)) hi = lo + (int)sizeof(f->text) - 1;
            memcpy(f->text, src + lo, (size_t)(hi - lo));
            flat_trim(f->text);
        } else {
            snprintf(f->text, sizeof(f->text), "#endif");
        }
        snprintf(f->name, sizeof(f->name), "endif");
        return 1;
    }
    if (node->kind != CP_FIELD && node->kind != CP_FUNC &&
        node->kind != CP_DEFINE && node->kind != CP_DIR &&
        node->kind != CP_TYPEDEF)
        return 1;
    f = &out[(*np)++];
    memset(f, 0, sizeof(*f));
    if (node->kind == CP_FUNC)
        f->kind = CP_FLAT_FUNC;
    else if (node->kind == CP_DIR)
        f->kind = CP_FLAT_PPDIR;
    else
        f->kind = CP_FLAT_FIELD;
    f->start = node->start;
    f->end = node->end;
    if (node->name) snprintf(f->name, sizeof(f->name), "%s", node->name);
    lo = node->start;
    hi = node->end;
    while (hi > lo && src[hi - 1] == ';') hi--;
    if (hi < lo) hi = lo;
    if (hi - lo >= (int)sizeof(f->text)) return 0;
    memcpy(f->text, src + lo, (size_t)(hi - lo));
    return 1;
}

int cparse_flat_from_parse(const CpParse *p, CpFlat *out, int cap, int *n) {
    if (!p || !p->root || !out || !n) return -1;
    *n = 0;
    if (!flat_one(p->root, p->src, out, cap, n)) return -1;
    return 0;
}

int cparse_flat_fields(const char *src, int len, const CpTok *toks, int ntoks,
                       CpFlat *out, int cap, int *n, char *err, int errcap) {
    CpParse p;
    int rc;
    memset(&p, 0, sizeof(p));
    rc = cparse_field_group("<fields>", src, len, toks, ntoks, &p);
    if (rc != 0) {
        if (err && errcap)
            snprintf(err, (size_t)errcap, "%s",
                     p.err.msg ? p.err.msg : "cparse fields failed");
        cparse_free(&p);
        return -1;
    }
    rc = cparse_flat_from_parse(&p, out, cap, n);
    cparse_free(&p);
    return rc;
}

int cparse_match_func(const char *src, int len, const CpTok *toks, int ntoks,
                      char *name, int ncap, int *lbrace_i, char *err,
                      int errcap) {
    CpParse p;
    CpNode *fn;
    int i;
    memset(&p, 0, sizeof(p));
    if (cparse_tokens("<fn>", src, len, toks, ntoks, &p) != 0) {
        if (err && errcap)
            snprintf(err, (size_t)errcap, "%s",
                     p.err.msg ? p.err.msg : "cparse fn failed");
        cparse_free(&p);
        return -1;
    }
    if (!p.root || p.root->nkid != 1 || p.root->kids[0]->kind != CP_FUNC) {
        if (err && errcap)
            snprintf(err, (size_t)errcap, "expected one C function");
        cparse_free(&p);
        return -1;
    }
    fn = p.root->kids[0];
    if (name && ncap > 0)
        snprintf(name, (size_t)ncap, "%s", fn->name ? fn->name : "");
    *lbrace_i = -1;
    for (i = 0; i < ntoks; i++) {
        if (toks[i].kind == CP_TOK_PUNCT && toks[i].len == 1) {
            const char *sp = toks[i].ptr ? toks[i].ptr : src + toks[i].offset;
            if (sp[0] == '{' && (int)toks[i].offset >= fn->start) {
                *lbrace_i = i;
                break;
            }
        }
    }
    cparse_free(&p);
    if (*lbrace_i < 0) {
        if (err && errcap) snprintf(err, (size_t)errcap, "function missing '{'");
        return -1;
    }
    return 0;
}

int cparse_lex_bytes(const char *src, int len, char **bytes, int *blen,
                     CpTok **toks, int *n) {
    char *spliced;
    int slen = 0;
    *bytes = NULL;
    *toks = NULL;
    *n = 0;
    if (blen) *blen = 0;
    spliced = splice_own(src, len, &slen);
    if (!spliced) return -1;
    if (cparse_lex(spliced, slen, toks, n) != 0) {
        free(spliced);
        return -1;
    }
    *bytes = spliced;
    if (blen) *blen = slen;
    return 0;
}

int cparse_buffer(const char *path, const char *src, int len, CpParse *out) {
    char *spliced = NULL;
    int slen = 0;
    CpTok *toks = NULL;
    int ntoks = 0;
    int rc;
    memset(out, 0, sizeof(*out));
    if (cparse_lex_bytes(src, len, &spliced, &slen, &toks, &ntoks) != 0) {
        out->err.msg = cp_str("lex failed", 10);
        return -1;
    }
    rc = cparse_tokens(path, spliced, slen, toks, ntoks, out);
    free(spliced);
    free(toks);
    return rc;
}

int cparse_file(const char *path, CpParse *out) {
    FILE *f;
    char *buf = NULL;
    size_t cap = 0, n = 0;
    int c;
    memset(out, 0, sizeof(*out));
    f = fopen(path, "rb");
    if (!f) {
        out->err.msg = cp_str("cannot open file", 16);
        return -1;
    }
    while ((c = fgetc(f)) != EOF) {
        if (n + 1 >= cap) {
            size_t nc = cap ? cap * 2 : 4096;
            char *nb = (char *)realloc(buf, nc);
            if (!nb) {
                free(buf);
                fclose(f);
                out->err.msg = cp_str("oom", 3);
                return -1;
            }
            buf = nb;
            cap = nc;
        }
        buf[n++] = (char)c;
    }
    fclose(f);
    if (!buf) {
        buf = (char *)malloc(1);
        if (!buf) return -1;
        buf[0] = 0;
    } else {
        buf[n] = 0;
    }
    {
        int rc = cparse_buffer(path, buf, (int)n, out);
        free(buf);
        return rc;
    }
}

static void node_free(CpNode *n) {
    int i;
    if (!n) return;
    for (i = 0; i < n->nkid; i++) node_free(n->kids[i]);
    for (i = 0; i < n->nthen; i++) node_free(n->then_kids[i]);
    for (i = 0; i < n->nelse; i++) node_free(n->else_kids[i]);
    free(n->kids);
    free(n->then_kids);
    free(n->else_kids);
    free(n->name);
    free(n->attr);
    free(n);
}

void cparse_free(CpParse *p) {
    if (!p) return;
    node_free(p->root);
    free(p->src);
    free(p->err.msg);
    memset(p, 0, sizeof(*p));
}

void cpenv_init(CpEnv *e) { memset(e, 0, sizeof(*e)); }

void cpenv_free(CpEnv *e) {
    int i;
    if (!e) return;
    for (i = 0; i < e->n; i++) {
        free(e->names[i]);
        if (e->bodies) free(e->bodies[i]);
    }
    free(e->names);
    free(e->bodies);
    free(e->is_func);
    memset(e, 0, sizeof(*e));
}

int cpenv_has(const CpEnv *e, const char *name) {
    int i;
    if (!e || !name) return 0;
    for (i = 0; i < e->n; i++)
        if (strcmp(e->names[i], name) == 0) return 1;
    return 0;
}

static int cpenv_grow(CpEnv *e, int nc) {
    char **nn, **nb;
    int *nf;
    int old = e->cap;
    nn = (char **)realloc(e->names, (size_t)nc * sizeof(char *));
    nb = (char **)realloc(e->bodies, (size_t)nc * sizeof(char *));
    nf = (int *)realloc(e->is_func, (size_t)nc * sizeof(int));
    if (!nn || !nb || !nf) {
        if (nn) e->names = nn;
        if (nb) e->bodies = nb;
        if (nf) e->is_func = nf;
        return 0;
    }
    memset(nb + old, 0, (size_t)(nc - old) * sizeof(char *));
    memset(nf + old, 0, (size_t)(nc - old) * sizeof(int));
    e->names = nn;
    e->bodies = nb;
    e->is_func = nf;
    e->cap = nc;
    return 1;
}

int cpenv_define_body(CpEnv *e, const char *name, const char *body) {
    char *dn, *db;
    int i;
    if (!e || !name) return 0;
    if (!body) body = "";
    for (i = 0; i < e->n; i++) {
        if (strcmp(e->names[i], name) == 0) {
            db = (char *)malloc(strlen(body) + 1);
            if (!db) return 0;
            memcpy(db, body, strlen(body) + 1);
            free(e->bodies[i]);
            e->bodies[i] = db;
            e->is_func[i] = 0;
            return 1;
        }
    }
    if (e->n >= e->cap && !cpenv_grow(e, e->cap ? e->cap * 2 : 8)) return 0;
    dn = (char *)malloc(strlen(name) + 1);
    db = (char *)malloc(strlen(body) + 1);
    if (!dn || !db) {
        free(dn);
        free(db);
        return 0;
    }
    memcpy(dn, name, strlen(name) + 1);
    memcpy(db, body, strlen(body) + 1);
    e->names[e->n] = dn;
    e->bodies[e->n] = db;
    e->is_func[e->n] = 0;
    e->n++;
    return 1;
}

int cpenv_define_func(CpEnv *e, const char *name, const char *rest) {
    int i;
    if (!cpenv_define_body(e, name, rest ? rest : "()")) return 0;
    for (i = 0; i < e->n; i++) {
        if (strcmp(e->names[i], name) == 0) {
            e->is_func[i] = 1;
            return 1;
        }
    }
    return 0;
}

int cpenv_undef(CpEnv *e, const char *name) {
    int i;
    if (!e || !name) return 0;
    for (i = 0; i < e->n; i++) {
        if (strcmp(e->names[i], name) != 0) continue;
        free(e->names[i]);
        free(e->bodies[i]);
        if (i + 1 < e->n) {
            memmove(&e->names[i], &e->names[i + 1],
                    (size_t)(e->n - i - 1) * sizeof(char *));
            memmove(&e->bodies[i], &e->bodies[i + 1],
                    (size_t)(e->n - i - 1) * sizeof(char *));
            memmove(&e->is_func[i], &e->is_func[i + 1],
                    (size_t)(e->n - i - 1) * sizeof(int));
        }
        e->n--;
        return 1;
    }
    return 1;
}

int cpenv_define(CpEnv *e, const char *name) {
    return cpenv_define_body(e, name, "1");
}

static int env_name_defined(const CpEnv *e, const char *name) {
    if (!name) return 0;
    if (strcmp(name, "__has_include") == 0 ||
        strcmp(name, "__has_feature") == 0 ||
        strcmp(name, "__has_builtin") == 0)
        return 1;
    return cpenv_has(e, name);
}

static int if_taken(const CpNode *n, const CpEnv *env) {
    long long v = 0;
    char err[192];
    if (n->if_form == CP_IF_CONST) return n->if_const != 0;
    if (n->if_form == CP_IF_IFDEF) return env_name_defined(env, n->name);
    if (n->if_form == CP_IF_IFNDEF) return !env_name_defined(env, n->name);
    if (n->if_form != CP_IF_EXPR || !n->name) return 0;
    if (cparse_eval_if_expr(n->name, env, &v, err, (int)sizeof(err)) != 0) {
        fprintf(stderr, "error: #if evaluate: %s\n", err[0] ? err : n->name);
        return 0;
    }
    return v != 0;
}

static void eval_list(CpNode **kids, int n, CpEnv *env, int live);

void cparse_evaluate(CpNode *n, CpEnv *env, int parent_live) {
    int i;
    if (!n) return;
    n->live = parent_live;
    if (n->kind == CP_DEFINE) {
        if (parent_live && n->name) {
            if (n->is_func)
                cpenv_define_func(env, n->name, n->attr ? n->attr : "()");
            else
                cpenv_define_body(env, n->name, n->attr ? n->attr : "");
        }
        return;
    }
    if (n->kind == CP_DIR) {
        if (parent_live && n->name && n->attr && strcmp(n->name, "undef") == 0)
            cpenv_undef(env, n->attr);
        return;
    }
    if (n->kind == CP_IF) {
        int take = parent_live && if_taken(n, env);
        eval_list(n->then_kids, n->nthen, env, take);
        eval_list(n->else_kids, n->nelse, env, parent_live && !take);
        return;
    }
    for (i = 0; i < n->nkid; i++) cparse_evaluate(n->kids[i], env, parent_live);
}

static void eval_list(CpNode **kids, int n, CpEnv *env, int live) {
    int i;
    for (i = 0; i < n; i++) cparse_evaluate(kids[i], env, live);
}

typedef struct {
    char *buf;
    size_t n;
    size_t cap;
} Buf;

static int buf_grow(Buf *b, size_t add) {
    size_t nc;
    char *nb;
    if (b->n + add + 1 <= b->cap) return 1;
    nc = b->cap ? b->cap * 2 : 256;
    while (nc < b->n + add + 1) nc *= 2;
    nb = (char *)realloc(b->buf, nc);
    if (!nb) return 0;
    b->buf = nb;
    b->cap = nc;
    return 1;
}

static int buf_add(Buf *b, const char *s, int n) {
    if (n < 0) n = (int)strlen(s);
    if (!buf_grow(b, (size_t)n)) return 0;
    memcpy(b->buf + b->n, s, (size_t)n);
    b->n += (size_t)n;
    b->buf[b->n] = 0;
    return 1;
}

static int buf_slice(Buf *b, const char *src, int lo, int hi) {
    if (hi < lo) hi = lo;
    return buf_add(b, src + lo, hi - lo);
}

static int buf_nl(Buf *b) {
    if (b->n && b->buf[b->n - 1] != '\n') return buf_add(b, "\n", 1);
    return 1;
}

static int dump_preserve_node(const CpNode *n, Buf *b);

static int dump_preserve_list(CpNode **kids, int n, Buf *b) {
    int i;
    for (i = 0; i < n; i++) {
        if (!dump_preserve_node(kids[i], b)) return 0;
        if (!buf_nl(b)) return 0;
    }
    return 1;
}

static int dump_preserve_node(const CpNode *n, Buf *b) {
    if (!n) return 1;
    switch (n->kind) {
    case CP_TU:
        return dump_preserve_list(n->kids, n->nkid, b);
    case CP_STRUCT:
        return buf_slice(b, n->src, n->start, n->end);
    case CP_FIELD:
        return buf_slice(b, n->src, n->start, n->end);
    case CP_FUNC:
        return buf_slice(b, n->src, n->start, n->end);
    case CP_DEFINE:
        return buf_slice(b, n->src, n->start, n->end);
    case CP_DIR:
        return buf_slice(b, n->src, n->start, n->end);
    case CP_TYPEDEF:
        return buf_slice(b, n->src, n->start, n->end);
    case CP_IF:
        if (!buf_slice(b, n->src, n->start, n->end)) return 0;
        if (!buf_nl(b)) return 0;
        if (!dump_preserve_list(n->then_kids, n->nthen, b)) return 0;
        if (n->nelse == 1 && n->else_kids[0] && n->else_kids[0]->kind == CP_IF &&
            n->else_kids[0]->is_elif) {
            if (!dump_preserve_node(n->else_kids[0], b)) return 0;
            if (!buf_nl(b)) return 0;
        } else if (n->else_end > n->else_start) {
            if (!buf_slice(b, n->src, n->else_start, n->else_end)) return 0;
            if (!buf_nl(b)) return 0;
            if (!dump_preserve_list(n->else_kids, n->nelse, b)) return 0;
        }
        if (n->is_elif) return 1;
        if (n->endif_end > n->endif_start)
            return buf_slice(b, n->src, n->endif_start, n->endif_end);
        return buf_add(b, "#endif", -1);
    default:
        return 1;
    }
}

int cparse_dump_preserve(const CpNode *n, char **out, size_t *len) {
    Buf b;
    memset(&b, 0, sizeof(b));
    if (!dump_preserve_node(n, &b)) {
        free(b.buf);
        return -1;
    }
    if (!buf_nl(&b)) {
        free(b.buf);
        return -1;
    }
    *out = b.buf ? b.buf : (char *)calloc(1, 1);
    if (len) *len = b.n;
    return *out ? 0 : -1;
}

static int dump_eval_node(const CpNode *n, Buf *b, int indent);

static int dump_eval_list(CpNode **kids, int n, Buf *b, int indent) {
    int i;
    for (i = 0; i < n; i++)
        if (!dump_eval_node(kids[i], b, indent)) return 0;
    return 1;
}

static int indent_sp(Buf *b, int n) {
    int i;
    for (i = 0; i < n; i++)
        if (!buf_add(b, "  ", -1)) return 0;
    return 1;
}

static int dump_eval_node(const CpNode *n, Buf *b, int indent) {
    char live[16];
    int i;
    if (!n) return 1;
    snprintf(live, sizeof(live), " live=%d", n->live < 0 ? -1 : n->live);
    switch (n->kind) {
    case CP_TU:
        return dump_eval_list(n->kids, n->nkid, b, indent);
    case CP_DEFINE:
        if (!indent_sp(b, indent)) return 0;
        if (!buf_add(b, "define ", -1)) return 0;
        if (n->name && !buf_add(b, n->name, -1)) return 0;
        if (!buf_add(b, live, -1)) return 0;
        return buf_add(b, "\n", 1);
    case CP_DIR:
        if (!indent_sp(b, indent)) return 0;
        if (!buf_add(b, "dir ", -1)) return 0;
        if (n->name && !buf_add(b, n->name, -1)) return 0;
        if (n->attr) {
            if (!buf_add(b, " ", 1)) return 0;
            if (!buf_add(b, n->attr, -1)) return 0;
        }
        if (!buf_add(b, live, -1)) return 0;
        return buf_add(b, "\n", 1);
    case CP_TYPEDEF:
        if (!indent_sp(b, indent)) return 0;
        if (!buf_add(b, "typedef ", -1)) return 0;
        if (n->name && !buf_add(b, n->name, -1)) return 0;
        if (!buf_add(b, live, -1)) return 0;
        return buf_add(b, "\n", 1);
    case CP_STRUCT:
        if (!indent_sp(b, indent)) return 0;
        if (!buf_add(b, "struct ", -1)) return 0;
        if (n->name && !buf_add(b, n->name, -1)) return 0;
        if (!buf_add(b, live, -1)) return 0;
        if (!buf_add(b, "\n", 1)) return 0;
        for (i = 0; i < n->nkid; i++)
            if (!dump_eval_node(n->kids[i], b, indent + 1)) return 0;
        return 1;
    case CP_FIELD:
        if (!indent_sp(b, indent)) return 0;
        if (!buf_add(b, "field ", -1)) return 0;
        if (n->name && !buf_add(b, n->name, -1)) return 0;
        if (!buf_add(b, live, -1)) return 0;
        return buf_add(b, "\n", 1);
    case CP_FUNC:
        if (!indent_sp(b, indent)) return 0;
        if (!buf_add(b, "func ", -1)) return 0;
        if (n->name && !buf_add(b, n->name, -1)) return 0;
        if (n->attr) {
            if (!buf_add(b, " ", 1)) return 0;
            if (!buf_add(b, n->attr, -1)) return 0;
        }
        if (!buf_add(b, live, -1)) return 0;
        return buf_add(b, "\n", 1);
    case CP_IF:
        if (!indent_sp(b, indent)) return 0;
        if (n->is_elif) {
            if (!buf_add(b, "#elif ", -1)) return 0;
        } else if (n->if_form == CP_IF_IFDEF) {
            if (!buf_add(b, "#ifdef ", -1)) return 0;
        } else if (n->if_form == CP_IF_IFNDEF) {
            if (!buf_add(b, "#ifndef ", -1)) return 0;
        } else {
            if (!buf_add(b, "#if ", -1)) return 0;
        }
        if (n->name && !buf_add(b, n->name, -1)) return 0;
        if (!buf_add(b, live, -1)) return 0;
        if (!buf_add(b, "\n", 1)) return 0;
        if (!dump_eval_list(n->then_kids, n->nthen, b, indent + 1)) return 0;
        if (n->nelse == 1 && n->else_kids[0] && n->else_kids[0]->kind == CP_IF &&
            n->else_kids[0]->is_elif) {
            if (!dump_eval_node(n->else_kids[0], b, indent)) return 0;
        } else if (n->nelse || n->else_end > n->else_start) {
            if (!indent_sp(b, indent)) return 0;
            if (!buf_add(b, "#else\n", -1)) return 0;
            if (!dump_eval_list(n->else_kids, n->nelse, b, indent + 1)) return 0;
        }
        if (n->is_elif) return 1;
        if (!indent_sp(b, indent)) return 0;
        return buf_add(b, "#endif\n", -1);
    default:
        return 1;
    }
}

int cparse_dump_evaluate(const CpNode *n, char **out, size_t *len) {
    Buf b;
    memset(&b, 0, sizeof(b));
    if (!dump_eval_node(n, &b, 0)) {
        free(b.buf);
        return -1;
    }
    *out = b.buf ? b.buf : (char *)calloc(1, 1);
    if (len) *len = b.n;
    return *out ? 0 : -1;
}
