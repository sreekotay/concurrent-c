/* cclex: exercise the lexer on real files.
 *
 *   cclex --roundtrip FILE...   every byte belongs to exactly one token; the
 *                               concatenation of trivia + text must be the
 *                               file. Also rejects CC_TK_ERROR tokens and a
 *                               NUMBER containing `..`. Exit 1 on any failure.
 *   cclex --dump FILE           one token per line: `line:col kind text`
 *   cclex --loc FILE OFF        logical location of byte offset OFF */
#include "lex.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *kind_name(CcTokKind k) {
    switch (k) {
    case CC_TK_EOF: return "EOF";
    case CC_TK_IDENT: return "IDENT";
    case CC_TK_AT_WORD: return "AT_WORD";
    case CC_TK_AT: return "AT";
    case CC_TK_NUMBER: return "NUMBER";
    case CC_TK_CHAR: return "CHAR";
    case CC_TK_STRING: return "STRING";
    case CC_TK_TEMPLATE: return "TEMPLATE";
    case CC_TK_PUNCT: return "PUNCT";
    case CC_TK_PP: return "PP";
    case CC_TK_ERROR: return "ERROR";
    }
    return "?";
}

static CcLexFile *lex_path(CcArena *a, CcDiag *d, const char *path) {
    size_t len = 0;
    char *src = cc_read_file(a, path, &len);
    if (!src) {
        fprintf(stderr, "cclex: %s: %s\n", path, strerror(errno));
        return NULL;
    }
    return cc_lex(a, d, path, src, len);
}

/* Escape a token's text so it fits on one output line. */
static void print_escaped(const char *s, size_t n) {
    size_t i;
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
        case '\n': fputs("\\n", stdout); break;
        case '\r': fputs("\\r", stdout); break;
        case '\t': fputs("\\t", stdout); break;
        case '\\': fputs("\\\\", stdout); break;
        default:
            if (c < 0x20 || c == 0x7f) printf("\\x%02x", c);
            else fputc(c, stdout);
        }
    }
}

static int has_dotdot(const char *s, size_t n) {
    size_t i;
    for (i = 1; i < n; i++)
        if (s[i - 1] == '.' && s[i] == '.') return 1;
    return 0;
}

static int check_tokens(const CcLexFile *f, int *had_error_tok) {
    uint32_t i;
    int ok = 1;
    if (f->n_toks == 0 || f->toks[f->n_toks - 1].kind != CC_TK_EOF) {
        printf("MISMATCH %s: token stream does not end in EOF\n", f->path);
        return 0;
    }
    for (i = 0; i < f->n_toks; i++) {
        const CcToken *t = &f->toks[i];
        uint32_t expect = i == 0 ? 0 : f->toks[i - 1].off + f->toks[i - 1].len;
        if (t->lead_off != expect || t->lead_off + t->lead_len != t->off || t->off + t->len > f->len) {
            printf("MISMATCH %s at byte %u (token %u: lead %u+%u, text %u+%u)\n", f->path,
                   (unsigned)expect, (unsigned)i, (unsigned)t->lead_off, (unsigned)t->lead_len,
                   (unsigned)t->off, (unsigned)t->len);
            return 0;
        }
        if (t->kind == CC_TK_ERROR) {
            *had_error_tok = 1;
            ok = 0;
        }
        if (t->kind == CC_TK_NUMBER && has_dotdot(f->src + t->off, t->len)) {
            printf("BADNUM %s at byte %u: NUMBER text contains `..`: ", f->path, (unsigned)t->off);
            print_escaped(f->src + t->off, t->len);
            putchar('\n');
            ok = 0;
        }
    }
    if (f->toks[f->n_toks - 1].off != f->len || f->toks[f->n_toks - 1].len != 0) {
        printf("MISMATCH %s at byte %u: EOF token does not end at the file end (%u)\n", f->path,
               (unsigned)f->toks[f->n_toks - 1].off, (unsigned)f->len);
        return 0;
    }
    return ok;
}

static int roundtrip(const char *path) {
    CcArena a;
    CcDiag d;
    CcLexFile *f;
    CcBuf out;
    uint32_t i;
    int ok = 1, had_error_tok = 0;
    cc_arena_init(&a, 0);
    cc_diag_init(&d, &a);
    f = lex_path(&a, &d, path);
    if (!f) { cc_arena_free(&a); return 0; }
    cc_buf_init(&out);
    for (i = 0; i < f->n_toks; i++)
        cc_buf_push(&out, f->src + f->toks[i].lead_off, f->toks[i].lead_len + f->toks[i].len);
    if (out.len != f->len || memcmp(out.data, f->src, f->len) != 0) {
        size_t n = 0;
        while (n < out.len && n < f->len && out.data[n] == f->src[n]) n++;
        printf("MISMATCH %s at byte %zu\n", path, n);
        ok = 0;
    } else if (!check_tokens(f, &had_error_tok)) {
        ok = 0;
    }
    if (had_error_tok) printf("ERROR %s: stream contains CC_TK_ERROR tokens\n", path);
    if (d.msgs.n) {
        fflush(stdout);
        cc_diag_print(&d, stderr);
    }
    if (d.n_errors) ok = 0;
    if (ok) printf("ok %s\n", path);
    cc_buf_free(&out);
    cc_arena_free(&a);
    return ok;
}

static int dump(const char *path) {
    CcArena a;
    CcDiag d;
    CcLexFile *f;
    uint32_t i;
    cc_arena_init(&a, 0);
    cc_diag_init(&d, &a);
    f = lex_path(&a, &d, path);
    if (!f) { cc_arena_free(&a); return 1; }
    for (i = 0; i < f->n_toks; i++) {
        const CcToken *t = &f->toks[i];
        printf("%u:%u %s", (unsigned)t->line, (unsigned)t->col, kind_name(t->kind));
        if (t->kind == CC_TK_PUNCT) printf("(%s)", cc_punct_text(t->punct));
        putchar(' ');
        print_escaped(f->src + t->off, t->len);
        putchar('\n');
    }
    for (i = 0; i < f->n_marks; i++)
        printf("mark off=%u phys=%u logical=%u path=%s\n", (unsigned)f->marks[i].off,
               (unsigned)f->marks[i].phys_line, (unsigned)f->marks[i].logical_line,
               f->marks[i].path ? f->marks[i].path : "(file)");
    fflush(stdout);
    cc_diag_print(&d, stderr);
    cc_arena_free(&a);
    return d.n_errors ? 1 : 0;
}

static int loc(const char *path, const char *off_text) {
    CcArena a;
    CcDiag d;
    CcLexFile *f;
    char *end;
    unsigned long off;
    CcLoc l;
    errno = 0;
    off = strtoul(off_text, &end, 10);
    if (errno || *end || end == off_text) {
        fprintf(stderr, "cclex: bad offset '%s'\n", off_text);
        return 2;
    }
    cc_arena_init(&a, 0);
    cc_diag_init(&d, &a);
    f = lex_path(&a, &d, path);
    if (!f) { cc_arena_free(&a); return 1; }
    if (off > f->len) {
        fprintf(stderr, "cclex: offset %lu past end of %s (%u bytes)\n", off, path, (unsigned)f->len);
        cc_arena_free(&a);
        return 2;
    }
    l = cc_lex_loc(f, (uint32_t)off);
    printf("%s:%u:%u\n", l.path, (unsigned)l.line, (unsigned)l.col);
    cc_diag_print(&d, stderr);
    cc_arena_free(&a);
    return 0;
}

static int usage(void) {
    fprintf(stderr, "usage: cclex --roundtrip FILE...\n"
                    "       cclex --dump FILE\n"
                    "       cclex --loc FILE OFF\n");
    return 2;
}

int main(int argc, char **argv) {
    if (argc < 3) return usage();
    if (strcmp(argv[1], "--roundtrip") == 0) {
        int i, failed = 0;
        for (i = 2; i < argc; i++)
            if (!roundtrip(argv[i])) failed++;
        if (failed) fprintf(stderr, "cclex: %d of %d files failed\n", failed, argc - 2);
        return failed ? 1 : 0;
    }
    if (strcmp(argv[1], "--dump") == 0 && argc == 3) return dump(argv[2]);
    if (strcmp(argv[1], "--loc") == 0 && argc == 4) return loc(argv[2], argv[3]);
    return usage();
}
