/* ccparse: drive the clean lowerer's parser over files.
 *
 *   ccparse --check FILE...            parse; `ok FILE (N decls)` or diagnostics
 *   ccparse --dump FILE                print the tree
 *   ccparse --known-types LIST.txt     extra typedef names, one per line (repeatable)
 *   ccparse --collect-types OUT        append every file-scope typedef name seen to OUT
 *   ccparse --mode source|header|script   override the extension-based mode
 *   ccparse --quiet                    only print files with diagnostics
 *   ccparse --pp-check FILE...         also report preprocessor lines no node keeps (a parser gap)
 *
 * Exit status is non-zero when any file produced an error. */
#include "parse.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- preprocessor-line coverage: every CC_TK_PP token must be held by a node ---- */

typedef struct PpCover { unsigned char *seen; uint32_t n; } PpCover;

static void cover_range(PpCover *c, CcSpan sp) {
    uint32_t i;
    for (i = sp.first; i <= sp.last && i < c->n; i++) c->seen[i] = 1;
}

static void cover_init(PpCover *c, CcInit *in) {
    size_t i;
    if (!in) return;
    if (in->is_pp) cover_range(c, in->span);
    for (i = 0; i < in->list.n; i++) cover_init(c, in->list.items[i]);
}

static void cover_attrs(PpCover *c, CcAttr *a) {
    for (; a; a = a->next)
        if (a->name && a->name[0] == '#') cover_range(c, a->span);
}

static int cover_decl(CcVisitor *v, CcDecl *d) {
    PpCover *c = (PpCover *)v->ctx;
    if (d->kind == CC_D_PP || d->kind == CC_D_PRAGMA_CC || d->kind == CC_D_GRAMMAR) cover_range(c, d->span);
    cover_init(c, d->init);
    cover_attrs(c, d->attrs);
    return 0;
}

static int cover_expr(CcVisitor *v, CcExpr *e) {
    PpCover *c = (PpCover *)v->ctx;
    CcGenericSelArm *g;
    if (e->kind == CC_E_PP) cover_range(c, e->span);
    cover_init(c, e->init);
    for (g = e->arms; g; g = g->next)
        if (g->is_pp) c->seen[g->pp_tok] = 1;
    return 0;
}

static int cover_type(CcVisitor *v, CcType *t) {
    PpCover *c = (PpCover *)v->ctx;
    CcField *f;
    CcEnumerator *en;
    size_t i;
    for (f = t->fields; f; f = f->next) { if (f->is_pp) cover_range(c, f->span); cover_attrs(c, f->attrs); }
    for (en = t->enumerators; en; en = en->next) if (en->is_pp) cover_range(c, en->span);
    for (i = 0; i < t->params.n; i++) { if (t->params.items[i]->is_pp) cover_range(c, t->params.items[i]->span); cover_attrs(c, t->params.items[i]->attrs); }
    cover_init(c, t->chan_hooks);
    cover_attrs(c, t->attrs);
    return 0;
}

static int pp_check(CcUnit *u) {
    PpCover c;
    CcVisitor v;
    uint32_t i;
    int bad = 0;
    c.n = u->file->n_toks;
    c.seen = (unsigned char *)calloc(c.n, 1);
    if (!c.seen) { fprintf(stderr, "ccparse: out of memory\n"); exit(2); }
    memset(&v, 0, sizeof v);
    v.ctx = &c;
    v.on_decl = cover_decl;
    v.on_expr = cover_expr;
    v.on_type = cover_type;
    cc_ast_walk(u, &v);
    for (i = 0; i < c.n; i++) {
        const CcToken *t = &u->file->toks[i];
        if (t->kind == CC_TK_PP && !c.seen[i]) {
            CcLoc loc = cc_lex_loc(u->file, t->off);
            printf("%s:%u:%u: gap: preprocessor line not kept by any node: %.*s\n", loc.path, (unsigned)loc.line,
                   (unsigned)loc.col, (int)(t->len > 60 ? 60 : t->len), u->file->src + t->off);
            bad = 1;
        }
    }
    free(c.seen);
    return bad;
}

static CcUnitMode mode_of(const char *path, const char *override) {
    size_t n = strlen(path);
    if (override) {
        if (!strcmp(override, "header")) return CC_MODE_HEADER;
        if (!strcmp(override, "script")) return CC_MODE_SCRIPT;
        return CC_MODE_SOURCE;
    }
    if (n >= 4 && !strcmp(path + n - 4, ".cch")) return CC_MODE_HEADER;
    if (n >= 5 && !strcmp(path + n - 5, ".shcc")) return CC_MODE_SCRIPT;
    return CC_MODE_SOURCE;
}

typedef struct Names { const char **items; size_t n, cap; } Names;

static void names_push(Names *ns, const char *s) {
    if (ns->n == ns->cap) {
        ns->cap = ns->cap ? ns->cap * 2 : 256;
        ns->items = (const char **)realloc(ns->items, ns->cap * sizeof *ns->items);
        if (!ns->items) { fprintf(stderr, "ccparse: out of memory\n"); exit(2); }
    }
    ns->items[ns->n++] = s;
}

static int read_names(Names *ns, const char *path) {
    FILE *fp = fopen(path, "r");
    char line[4096];
    if (!fp) { fprintf(stderr, "ccparse: cannot open %s: %s\n", path, strerror(errno)); return 0; }
    while (fgets(line, sizeof line, fp)) {
        size_t n = strlen(line);
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r' || line[n - 1] == ' ')) line[--n] = 0;
        if (!n || line[0] == '#') continue;
        {
            char *copy = (char *)malloc(n + 1);
            if (!copy) { fprintf(stderr, "ccparse: out of memory\n"); exit(2); }
            memcpy(copy, line, n + 1);
            names_push(ns, copy);
        }
    }
    fclose(fp);
    return 1;
}

int main(int argc, char **argv) {
    int i, dump = 0, quiet = 0, rc = 0, ppcheck = 0;
    const char *collect = NULL, *mode_override = NULL;
    Names known;
    memset(&known, 0, sizeof known);
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--check")) continue;
        if (!strcmp(argv[i], "--dump")) { dump = 1; continue; }
        if (!strcmp(argv[i], "--quiet")) { quiet = 1; continue; }
        if (!strcmp(argv[i], "--pp-check")) { ppcheck = 1; continue; }
        if (!strcmp(argv[i], "--known-types") && i + 1 < argc) { if (!read_names(&known, argv[++i])) return 2; continue; }
        if (!strcmp(argv[i], "--collect-types") && i + 1 < argc) { collect = argv[++i]; continue; }
        if (!strcmp(argv[i], "--mode") && i + 1 < argc) { mode_override = argv[++i]; continue; }
        if (argv[i][0] == '-' && argv[i][1]) { fprintf(stderr, "ccparse: unknown option %s\n", argv[i]); return 2; }
        break;
    }
    if (i >= argc) {
        fprintf(stderr, "usage: ccparse [--check|--dump] [--known-types LIST]... [--collect-types OUT] [--mode M] FILE...\n");
        return 2;
    }
    names_push(&known, NULL);
    for (; i < argc; i++) {
        const char *path = argv[i];
        CcArena arena;
        CcDiag diag;
        CcIntern *in;
        CcLexFile *f;
        CcUnit *u;
        CcParseOpts opts;
        size_t len = 0;
        char *src;
        cc_arena_init(&arena, 0);
        cc_diag_init(&diag, &arena);
        src = cc_read_file(&arena, path, &len);
        if (!src) {
            fprintf(stderr, "ccparse: cannot read %s: %s\n", path, strerror(errno));
            rc = 1;
            cc_arena_free(&arena);
            continue;
        }
        in = cc_intern_new(&arena);
        f = cc_lex(&arena, &diag, path, src, len);
        memset(&opts, 0, sizeof opts);
        opts.mode = mode_of(path, mode_override);
        opts.known_types = known.items;
        u = cc_parse(&arena, &diag, in, f, &opts);
        if (dump) cc_ast_dump(u, stdout);
        if (ppcheck && pp_check(u)) rc = 1;
        if (diag.n_errors || diag.n_warnings) {
            cc_diag_print(&diag, stdout);
            if (diag.n_errors) rc = 1;
        } else if (!quiet) {
            printf("ok %s (%zu decls)\n", path, u->decls.n);
        }
        if (collect && u->typedef_names.n) {
            FILE *out = fopen(collect, "a");
            size_t k;
            if (!out) { fprintf(stderr, "ccparse: cannot append to %s: %s\n", collect, strerror(errno)); return 2; }
            for (k = 0; k < u->typedef_names.n; k++) fprintf(out, "%s\n", u->typedef_names.items[k]);
            fclose(out);
        }
        fflush(stdout);
        cc_arena_free(&arena);
    }
    return rc;
}
