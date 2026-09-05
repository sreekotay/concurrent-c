/* cclower: drives the printer over real files.
 *
 *   cclower --identity [--known-types F] FILE...
 *       Lex, parse and print in identity mode; compare with the input.
 *       Prints `ok FILE` or `MISMATCH FILE at byte N (line L)` plus the
 *       diagnostics; exits non-zero on any mismatch, uncovered token or
 *       parse error.
 *   cclower --print [--known-types F] [--no-line] [--header] [-o OUT] FILE
 *       Lowered-mode print with `#line` to OUT (or stdout) and the source
 *       map beside it as OUT.map (`emit_line path line col_delta` per
 *       line). The printed text is checked against the map: every emitted
 *       line has an entry, and the host compiler's attribution (from the
 *       `#line` directives) agrees with it.
 *   cclower --is-plain-c [--known-types F] FILE
 *       Exit 0 when the tree has no Concurrent-C node kinds.
 *
 * The parse mode follows the extension: .cch is header mode, .shcc script
 * mode, everything else source mode. */
#include "lex.h"
#include "parse.h"
#include "print.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct Loaded {
    CcArena arena;
    CcDiag diag;
    CcIntern *in;
    CcLexFile *file;
    CcUnit *unit;
    const char *src;
    size_t len;
} Loaded;

static const char **read_known_types(CcArena *a, const char *path) {
    size_t len, i, n = 0, cap = 0;
    char *text = cc_read_file(a, path, &len);
    const char **list = NULL;
    if (!text) {
        fprintf(stderr, "cclower: cannot read %s: %s\n", path, strerror(errno));
        exit(2);
    }
    for (i = 0; i < len;) {
        size_t j;
        while (i < len && (text[i] == ' ' || text[i] == '\t' || text[i] == '\r' || text[i] == '\n')) i++;
        if (i >= len) break;
        j = i;
        while (j < len && !(text[j] == ' ' || text[j] == '\t' || text[j] == '\r' || text[j] == '\n')) j++;
        if (text[i] != '#') {
            if (n + 1 >= cap) {
                size_t ncap = cap ? cap * 2 : 64;
                const char **nl = CC_NEW_N(a, const char *, ncap);
                if (n) memcpy(nl, list, n * sizeof *nl);
                list = nl;
                cap = ncap;
            }
            list[n++] = cc_arena_strndup(a, text + i, j - i);
        }
        i = j;
    }
    if (!list) list = CC_NEW_N(a, const char *, 1);
    list[n] = NULL;
    return list;
}

static CcUnitMode mode_for(const char *path) {
    size_t n = strlen(path);
    if (n >= 4 && strcmp(path + n - 4, ".cch") == 0) return CC_MODE_HEADER;
    if (n >= 5 && strcmp(path + n - 5, ".shcc") == 0) return CC_MODE_SCRIPT;
    return CC_MODE_SOURCE;
}

static int load(Loaded *L, const char *path, const char **known) {
    CcParseOpts po;
    cc_arena_init(&L->arena, 0);
    cc_diag_init(&L->diag, &L->arena);
    L->src = cc_read_file(&L->arena, path, &L->len);
    if (!L->src) {
        fprintf(stderr, "cclower: cannot read %s: %s\n", path, strerror(errno));
        return 0;
    }
    L->in = cc_intern_new(&L->arena);
    L->file = cc_lex(&L->arena, &L->diag, path, L->src, L->len);
    memset(&po, 0, sizeof po);
    po.mode = mode_for(path);
    po.known_types = known;
    po.allow_top_level_stmts = po.mode == CC_MODE_SCRIPT;
    L->unit = cc_parse(&L->arena, &L->diag, L->in, L->file, &po);
    return 1;
}

static void unload(Loaded *L) {
    cc_arena_free(&L->arena);
}

/* ---- --identity ---------------------------------------------------------- */

static int run_identity(const char *path, const char **known) {
    Loaded L;
    CcBuf out;
    CcPrintOpts po;
    int rc = 0;
    uint32_t n_parse_errors;
    if (!load(&L, path, known)) return 1;
    n_parse_errors = L.diag.n_errors;
    memset(&po, 0, sizeof po);
    po.identity = 1;
    cc_buf_init(&out);
    cc_print_unit(&out, NULL, &L.arena, &L.diag, L.unit, &po);
    if (out.len != L.len || memcmp(out.data, L.src, L.len) != 0) {
        size_t i = 0, line = 1;
        size_t lim = out.len < L.len ? out.len : L.len;
        while (i < lim && out.data[i] == L.src[i]) i++;
        for (size_t k = 0; k < i && k < L.len; k++) if (L.src[k] == '\n') line++;
        printf("MISMATCH %s at byte %zu (line %zu)\n", path, i, line);
        rc = 1;
    } else if (L.diag.n_errors) {
        printf("%s %s (%u parse error(s), %u printer error(s))\n",
               n_parse_errors ? "PARSE-ERROR" : "UNCOVERED", path, (unsigned)n_parse_errors,
               (unsigned)(L.diag.n_errors - n_parse_errors));
        rc = 1;
    } else {
        printf("ok %s\n", path);
    }
    if (L.diag.msgs.n) cc_diag_print(&L.diag, stderr);
    cc_buf_free(&out);
    unload(&L);
    return rc;
}

/* ---- --is-plain-c -------------------------------------------------------- */

typedef struct Plain {
    const CcLexFile *f;
    const char *first_what;
    uint32_t first_tok;
} Plain;

static void plain_note(Plain *pl, const char *what, uint32_t tok) {
    if (pl->first_what) return;
    pl->first_what = what;
    pl->first_tok = tok;
}

#define CC_F_CC_MASK (CC_F_ASYNC | CC_F_BLOCKING | CC_F_NONBLOCKING | CC_F_LATENCY_SENSITIVE | CC_F_COMPTIME)

static int plain_decl(CcVisitor *v, CcDecl *d) {
    Plain *pl = (Plain *)v->ctx;
    if ((d->kind >= CC_D_TYPEHOOKS && d->kind != CC_D_MACRO_CALL) || d->destroy || d->detach ||
        (d->specs & CC_F_CC_MASK))
        plain_note(pl, cc_decl_kind_name(d->kind), d->span.first);
    return 0;
}
static int plain_stmt(CcVisitor *v, CcStmt *s) {
    Plain *pl = (Plain *)v->ctx;
    if (s->kind >= CC_S_DEFER || (s->kind == CC_S_CASE && s->case_arm) ||
        (s->kind == CC_S_SWITCH && s->is_variant_switch))
        plain_note(pl, cc_stmt_kind_name(s->kind), s->span.first);
    return 0;
}
static int plain_expr(CcVisitor *v, CcExpr *e) {
    Plain *pl = (Plain *)v->ctx;
    if (e->kind >= CC_E_UFCS && e->kind != CC_E_PP && e->kind != CC_E_TYPE_ARG)
        plain_note(pl, cc_expr_kind_name(e->kind), e->span.first);
    return 0;
}
static int plain_type(CcVisitor *v, CcType *t) {
    Plain *pl = (Plain *)v->ctx;
    size_t i;
    if (t->kind >= CC_T_RESULT) plain_note(pl, cc_type_kind_name(t->kind), t->span.first);
    for (i = 0; i < t->params.n; i++)
        if (t->params.items[i] && t->params.items[i]->default_value)
            plain_note(pl, "parameter default", t->params.items[i]->span.first);
    return 0;
}

static int run_is_plain_c(const char *path, const char **known) {
    Loaded L;
    Plain pl;
    CcVisitor v;
    int rc;
    if (!load(&L, path, known)) return 2;
    memset(&pl, 0, sizeof pl);
    pl.f = L.file;
    memset(&v, 0, sizeof v);
    v.ctx = &pl;
    v.on_decl = plain_decl;
    v.on_stmt = plain_stmt;
    v.on_expr = plain_expr;
    v.on_type = plain_type;
    cc_ast_walk(L.unit, &v);
    if (L.unit->has_shebang) plain_note(&pl, "#!ccc", 0);
    if (L.diag.n_errors) {
        printf("parse-error %s\n", path);
        cc_diag_print(&L.diag, stderr);
        rc = 2;
    } else if (pl.first_what) {
        CcLoc loc = cc_lex_loc(L.file, pl.first_tok < L.file->n_toks ? L.file->toks[pl.first_tok].off : 0);
        printf("cc %s (%s at line %u)\n", path, pl.first_what, (unsigned)loc.line);
        rc = 1;
    } else {
        printf("plain %s\n", path);
        rc = 0;
    }
    unload(&L);
    return rc;
}

/* ---- --print --------------------------------------------------------------- */

/* Parse `#line N "path"` / `# N "path"` at the start of a line. Returns 1
 * and fills line/path (path is a malloc'd copy, or NULL when absent). */
static int parse_line_directive(const char *s, size_t n, unsigned *line, char **path) {
    size_t i = 0, j;
    unsigned v = 0;
    while (i < n && (s[i] == ' ' || s[i] == '\t')) i++;
    if (i >= n || s[i] != '#') return 0;
    i++;
    while (i < n && (s[i] == ' ' || s[i] == '\t')) i++;
    if (i + 4 <= n && memcmp(s + i, "line", 4) == 0) {
        i += 4;
        if (i >= n || !(s[i] == ' ' || s[i] == '\t')) return 0;
        while (i < n && (s[i] == ' ' || s[i] == '\t')) i++;
    }
    if (i >= n || s[i] < '0' || s[i] > '9') return 0;
    while (i < n && s[i] >= '0' && s[i] <= '9') { v = v * 10 + (unsigned)(s[i] - '0'); i++; }
    while (i < n && (s[i] == ' ' || s[i] == '\t')) i++;
    *line = v;
    *path = NULL;
    if (i < n && s[i] == '"') {
        CcBuf b;
        cc_buf_init(&b);
        for (j = i + 1; j < n && s[j] != '"'; j++) {
            if (s[j] == '\\' && j + 1 < n) j++;
            cc_buf_push_char(&b, s[j]);
        }
        *path = cc_buf_take(&b);
        cc_buf_free(&b);
    }
    return 1;
}

/* Check the printed text against its map: one entry per line and, when
 * directives are on, the host's attribution equals the map's. Returns the
 * number of problems. */
static int verify_map(const char *text, size_t len, const CcSrcMap *map, int directives, FILE *err) {
    size_t i = 0, lineno = 1, n_lines = 0;
    int problems = 0;
    char *host_path = NULL;
    unsigned host_line = 0;
    while (i < len) {
        size_t j = i;
        const CcSrcMapEntry *e;
        unsigned dl;
        char *dp;
        while (j < len && text[j] != '\n') j++;
        n_lines++;
        if (lineno > map->n || map->entries[lineno - 1].emit_line != lineno) {
            fprintf(err, "map: emitted line %zu has no source map entry\n", lineno);
            problems++;
            e = NULL;
        } else {
            e = &map->entries[lineno - 1];
        }
        if (parse_line_directive(text + i, j - i, &dl, &dp)) {
            host_line = dl;
            if (dp) { free(host_path); host_path = dp; }
        } else {
            if (directives && host_line && e) {
                if (host_line != e->line || !host_path || strcmp(host_path, e->path ? e->path : "") != 0) {
                    fprintf(err, "map: emitted line %zu is attributed to %s:%u by the host but to %s:%u by the map\n",
                            lineno, host_path ? host_path : "?", host_line, e->path ? e->path : "?", (unsigned)e->line);
                    problems++;
                }
            }
            if (host_line) host_line++;
        }
        lineno++;
        i = j < len ? j + 1 : j;
    }
    if (map->n != n_lines) {
        fprintf(err, "map: %u entries for %zu emitted lines\n", (unsigned)map->n, n_lines);
        problems++;
    }
    free(host_path);
    return problems;
}

static int run_print(const char *path, const char **known, const char *out_path, int line_directives,
                     int header_mode) {
    Loaded L;
    CcBuf out;
    CcSrcMap map;
    CcPrintOpts po;
    int rc = 0, problems;
    uint32_t n_parse_errors;
    if (!load(&L, path, known)) return 2;
    n_parse_errors = L.diag.n_errors;
    memset(&po, 0, sizeof po);
    po.line_directives = line_directives;
    po.header_mode = header_mode;
    po.path = path;
    cc_buf_init(&out);
    cc_print_unit(&out, &map, &L.arena, &L.diag, L.unit, &po);
    if (out_path) {
        FILE *fp = fopen(out_path, "wb");
        CcBuf mp;
        uint32_t k;
        if (!fp) {
            fprintf(stderr, "cclower: cannot write %s: %s\n", out_path, strerror(errno));
            rc = 2;
        } else {
            fwrite(out.data, 1, out.len, fp);
            fclose(fp);
        }
        cc_buf_init(&mp);
        cc_buf_push_str(&mp, out_path);
        cc_buf_push_str(&mp, ".map");
        fp = fopen(mp.data, "wb");
        if (!fp) {
            fprintf(stderr, "cclower: cannot write %s: %s\n", mp.data, strerror(errno));
            rc = 2;
        } else {
            for (k = 0; k < map.n; k++)
                fprintf(fp, "%u %s %u %d\n", (unsigned)map.entries[k].emit_line,
                        map.entries[k].path ? map.entries[k].path : "?", (unsigned)map.entries[k].line,
                        (int)map.entries[k].col_delta);
            fclose(fp);
        }
        cc_buf_free(&mp);
    } else {
        fwrite(out.data, 1, out.len, stdout);
    }
    problems = verify_map(out.data, out.len, &map, line_directives, stderr);
    if (L.diag.msgs.n) cc_diag_print(&L.diag, stderr);
    if (L.diag.n_errors) {
        fprintf(stderr, "cclower: %s: %u parse error(s), %u printer error(s)\n", path,
                (unsigned)n_parse_errors, (unsigned)(L.diag.n_errors - n_parse_errors));
        rc = 1;
    }
    if (problems) {
        fprintf(stderr, "cclower: %s: %d source map problem(s)\n", path, problems);
        rc = 1;
    }
    cc_buf_free(&out);
    unload(&L);
    return rc;
}

/* ---- main ---------------------------------------------------------------------- */

static void usage(void) {
    fprintf(stderr,
            "usage: cclower --identity [--known-types F] FILE...\n"
            "       cclower --print [--known-types F] [--no-line] [--header] [-o OUT] FILE\n"
            "       cclower --is-plain-c [--known-types F] FILE\n");
    exit(2);
}

int main(int argc, char **argv) {
    enum { M_NONE, M_IDENTITY, M_PRINT, M_PLAIN } mode = M_NONE;
    const char *known_path = NULL, *out_path = NULL;
    const char **known = NULL;
    int line_directives = 1, header_mode = 0, i, rc = 0, n_files = 0;
    CcArena opt_arena;
    cc_arena_init(&opt_arena, 0);
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--identity") == 0) mode = M_IDENTITY;
        else if (strcmp(argv[i], "--print") == 0) mode = M_PRINT;
        else if (strcmp(argv[i], "--is-plain-c") == 0) mode = M_PLAIN;
        else if (strcmp(argv[i], "--known-types") == 0 && i + 1 < argc) known_path = argv[++i];
        else if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) out_path = argv[++i];
        else if (strcmp(argv[i], "--no-line") == 0) line_directives = 0;
        else if (strcmp(argv[i], "--header") == 0) header_mode = 1;
        else if (argv[i][0] == '-' && argv[i][1]) usage();
    }
    if (mode == M_NONE) usage();
    if (known_path) known = read_known_types(&opt_arena, known_path);
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--known-types") == 0 || strcmp(argv[i], "-o") == 0) { i++; continue; }
        if (argv[i][0] == '-' && argv[i][1]) continue;
        n_files++;
        switch (mode) {
        case M_IDENTITY: rc |= run_identity(argv[i], known); break;
        case M_PRINT: rc |= run_print(argv[i], known, out_path, line_directives, header_mode); break;
        case M_PLAIN: rc |= run_is_plain_c(argv[i], known); break;
        default: break;
        }
    }
    if (!n_files) usage();
    cc_arena_free(&opt_arena);
    return rc;
}
