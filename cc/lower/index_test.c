/* ccindex: exercise the declaration index on real files.
 *
 *   ccindex [-I dir]... --dump FILE            every symbol, type info, hook and Result spec
 *   ccindex [-I dir]... --resolve FILE Type m  the callee for `x.m()` on a receiver of type Type
 *   ccindex [-I dir]... --gaps FILE...         every UFCS call site in the files, classified by
 *                                              how the index resolves it (markdown on stdout)
 *   ccindex [-I dir]... --sites FILE           one line per UFCS site with its resolution
 *
 * The default include directory is cc/include (run from the repository
 * root). Diagnostics from the index and the parser print to stderr. */
#include "index.h"
#include "parse.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct Session {
    CcArena arena;
    CcDiag diag;
    CcIntern *intern;
    CcIndex *ix;
    CcUnit *unit;
    CcIndexOpts iopts;
} Session;

static const char *include_dirs[16];
static size_t n_include_dirs;

static CcUnitMode mode_for(const char *path) {
    size_t n = strlen(path);
    if (n > 4 && strcmp(path + n - 4, ".cch") == 0) return CC_MODE_HEADER;
    if (n > 5 && strcmp(path + n - 5, ".shcc") == 0) return CC_MODE_SCRIPT;
    return CC_MODE_SOURCE;
}

static int session_open(Session *s, const char *path) {
    size_t len = 0;
    char *src;
    CcLexFile *f;
    CcParseOpts po;
    cc_arena_init(&s->arena, 1 << 20);
    cc_diag_init(&s->diag, &s->arena);
    s->intern = cc_intern_new(&s->arena);
    s->ix = cc_index_new(&s->arena, &s->diag, s->intern);
    memset(&s->iopts, 0, sizeof s->iopts);
    s->iopts.include_dirs = include_dirs;
    src = cc_read_file(&s->arena, path, &len);
    if (!src) {
        fprintf(stderr, "ccindex: %s: %s\n", path, strerror(errno));
        return 0;
    }
    f = cc_lex(&s->arena, &s->diag, path, src, len);
    memset(&po, 0, sizeof po);
    po.mode = mode_for(path);
    cc_index_preload_includes(s->ix, f, &s->iopts, &po);
    po.known_types = cc_index_known_types(s->ix);
    s->unit = cc_parse(&s->arena, &s->diag, s->intern, f, &po);
    cc_index_add_unit(s->ix, s->unit, po.mode == CC_MODE_HEADER);
    return 1;
}

static void session_close(Session *s) {
    cc_diag_print(&s->diag, stderr);
    cc_arena_free(&s->arena);
}

static const char *sym_kind_name(int k) {
    switch (k) {
    case CC_SYM_FUNC: return "func";
    case CC_SYM_VAR: return "var";
    case CC_SYM_TYPE: return "type";
    case CC_SYM_ENUMERATOR: return "enumerator";
    case CC_SYM_MACRO: return "macro";
    }
    return "?";
}

static const char *unit_label(const CcUnit *u) {
    return u && u->file && u->file->path ? u->file->path : "?";
}

/* ---- --dump ------------------------------------------------------------ */

static void dump(Session *s) {
    CcIndex *ix = s->ix;
    size_t i;
    printf("# units (%zu)\n", ix->units.n);
    for (i = 0; i < ix->units.n; i++)
        printf("  %s %s\n", ix->unit_is_header.items[i][0] == '1' ? "header" : "unit  ", unit_label(ix->units.items[i]));
    printf("# expansions: %u (parse errors inside them: %u)\n", ix->n_expansions, ix->n_expansion_errors);
    printf("# symbols (%zu)\n", ix->syms.n);
    for (i = 0; i < ix->syms.n; i++) {
        CcSym *sym = ix->syms.items[i];
        CcLoc loc = cc_index_sym_loc(sym);
        const char *canon = "-";
        if (sym->type) {
            if (sym->kind == CC_SYM_FUNC && sym->type->kind == CC_T_FUNC) {
                CcBuf b;
                size_t k;
                cc_buf_init(&b);
                cc_buf_push_str(&b, cc_index_canon(ix, sym->type->base));
                cc_buf_push_char(&b, '(');
                for (k = 0; k < sym->type->params.n; k++) {
                    CcParam *p = sym->type->params.items[k];
                    if (k) cc_buf_push_str(&b, ", ");
                    cc_buf_push_str(&b, p->is_variadic || !p->type ? "..." : cc_index_canon(ix, p->type));
                }
                cc_buf_push_char(&b, ')');
                canon = cc_arena_strdup(&s->arena, b.data);
                cc_buf_free(&b);
            } else {
                canon = cc_index_canon(ix, sym->type);
            }
        } else if (sym->kind == CC_SYM_MACRO && sym->macro) {
            canon = sym->macro->function_like ? "function-like" : "object-like";
        }
        printf("%s %s : %s (%s:%u)%s%s\n", sym_kind_name(sym->kind), sym->name, canon,
               loc.path ? loc.path : "?", loc.line,
               sym->kind == CC_SYM_FUNC && cc_index_sym_noreturn(sym) ? " [noreturn]" : "",
               sym->is_definition && sym->kind == CC_SYM_FUNC ? " [definition]" : "");
    }
    printf("# typehooks (%zu)\n", ix->hooks.n);
    for (i = 0; i < ix->hooks.n; i++) {
        CcHookReg *h = ix->hooks.items[i];
        CcUfcsRule *r;
        printf("@typehooks on %s", h->subject);
        if (h->create_fn) printf(" create=%s%s%s", h->create_fn, h->create_fn2 ? "," : "", h->create_fn2 ? h->create_fn2 : "");
        if (h->pre_destroy_fn) printf(" pre_destroy=%s", h->pre_destroy_fn);
        if (h->destroy_fn) printf(" destroy=%s", h->destroy_fn);
        if (h->ufcs_fn) {
            printf(" ufcs=%s", h->ufcs_fn);
            if (h->ufcs_prefix) printf(" [prefix %s%s]", h->ufcs_prefix, h->ufcs_prefix_by_value ? " by-value" : "");
            if (h->ufcs_rejects) printf(" [rejects]");
            if (h->ufcs_opaque) printf(" [opaque]");
            for (r = h->rules; r; r = r->next) printf(" %s->%s%s", r->method, r->callee, r->by_value ? "(v)" : "");
        }
        if (h->has_len) printf(" len");
        if (h->has_access) printf(" access");
        if (h->has_cast) printf(" cast");
        if (h->has_niche) printf(" niche");
        if (h->has_sink) printf(" ufcs_sink");
        printf("\n");
    }
    printf("# types (%zu)\n", ix->types.n);
    for (i = 0; i < ix->types.n; i++) {
        CcTypeInfo *t = ix->types.items[i];
        CcMethod **ms = NULL;
        size_t n = cc_index_methods_of(ix, t, &ms), k;
        printf("type %s", t->name);
        if (t->family) printf(" family=%s", t->family);
        if (t->hooks) printf(" hooks=%s", t->hooks->subject);
        if (t->create_fn) printf(" create=%s", t->create_fn);
        if (t->pre_destroy_fn) printf(" pre_destroy=%s", t->pre_destroy_fn);
        if (t->destroy_fn) printf(" destroy=%s", t->destroy_fn);
        if (t->ufcs_registered_by) printf(" registered=%s", t->ufcs_registered_by);
        if (t->is_result) printf(" result%s%s", t->result_optional ? " optional" : "", t->result_declared_in_header ? " header-declared" : "");
        printf("\n");
        for (k = 0; k < n; k++)
            printf("    .%s -> %s%s [%s] %s\n", ms[k]->method, ms[k]->callee, ms[k]->recv_by_ptr ? "(&recv)" : "(recv)", ms[k]->source, ms[k]->origin ? ms[k]->origin : "");
    }
    printf("# result specs used by the unit (%zu)\n", ix->result_specs.n);
    for (i = 0; i < ix->result_specs.n; i++) {
        CcTypeInfo *t = ix->result_specs.items[i];
        printf("%s : %s !> %s%s : %s\n", t->name,
               t->result_value ? cc_index_canon(ix, t->result_value) : "void",
               t->result_err ? cc_index_canon(ix, t->result_err) : "?",
               t->result_optional ? " (?> discardable)" : "",
               t->result_declared_in_header
                   ? cc_arena_printf(&s->arena, "declared in header %s:%u", unit_label(t->result_decl_unit), t->result_decl_line)
                   : t->result_decl_unit ? cc_arena_printf(&s->arena, "declared in the unit %s:%u", unit_label(t->result_decl_unit), t->result_decl_line)
                                          : "TU must emit the spec");
    }
    printf("# result specs declared by headers\n");
    for (i = 0; i < ix->types.n; i++) {
        CcTypeInfo *t = ix->types.items[i];
        if (t->is_result && t->result_declared_in_header)
            printf("%s (%s:%u)\n", t->name, unit_label(t->result_decl_unit), t->result_decl_line);
    }
}

/* ---- receiver typing for UFCS sites ------------------------------------ */

typedef struct Scope {
    struct Scope *up;
    CC_LIST(const char) names;
    CC_LIST(CcType) types;
} Scope;

typedef struct Site {
    const char *type;      /* canonical receiver type or NULL */
    const char *method;
    const char *source;    /* resolution source or NULL */
    const char *callee;
    const char *reason;    /* diagnostic when unresolved */
    const char *file;
    uint32_t line;
    int recv_kind;         /* expression kind of the receiver */
} Site;

typedef struct Walker {
    Session *s;
    Scope *scope;
    CC_LIST(Site) sites;
} Walker;

static void scope_push(Walker *w) {
    Scope *sc = CC_NEW(&w->s->arena, Scope);
    sc->up = w->scope;
    w->scope = sc;
}

static void scope_pop(Walker *w) { w->scope = w->scope->up; }

static void scope_declare(Walker *w, CcName name, CcType *type) {
    if (!name || !w->scope) return;
    CC_LIST_PUSH(&w->s->arena, &w->scope->names, name);
    CC_LIST_PUSH(&w->s->arena, &w->scope->types, type);
}

static CcType *scope_lookup(Walker *w, const char *name) {
    Scope *sc;
    for (sc = w->scope; sc; sc = sc->up) {
        size_t i = sc->names.n;
        while (i--)
            if (strcmp(sc->names.items[i], name) == 0) return sc->types.items[i];
    }
    return NULL;
}

static CcType *type_of_expr(Walker *w, CcExpr *e);

static CcType *peel(CcType *t) {
    while (t && (t->kind == CC_T_POINTER || t->kind == CC_T_ATOMIC || t->kind == CC_T_ARRAY)) t = t->base;
    return t;
}

static CcType *synth(Walker *w, CcTypeKind k, CcType *base) {
    CcSpan sp = {0, 0};
    CcType *t = cc_type_new(&w->s->arena, k, sp);
    t->base = base;
    return t;
}

static CcType *named(Walker *w, const char *name) {
    CcSpan sp = {0, 0};
    CcType *t = cc_type_new(&w->s->arena, CC_T_NAMED, sp);
    t->name = cc_intern(w->s->intern, name, strlen(name));
    return t;
}

/* The struct definition a type resolves to (typedef chains and tags). */
static const CcType *struct_of_type(Walker *w, CcType *t, int depth) {
    CcIndex *ix = w->s->ix;
    CcSym *sym;
    if (!t || depth > 8) return NULL;
    t = peel(t);
    if (!t) return NULL;
    if (t->kind == CC_T_STRUCT) {
        if (t->fields) return t;
        if (t->name) {
            sym = cc_index_sym(ix, cc_arena_printf(&w->s->arena, "%s %s", t->is_union ? "union" : "struct", t->name));
            if (sym && sym->type && sym->type->fields) return sym->type;
        }
        return NULL;
    }
    if (t->kind == CC_T_NAMED && t->name) {
        CcName cn = cc_index_canon(ix, t);
        sym = cc_index_sym(ix, cn);
        if (!sym || sym->kind != CC_SYM_TYPE) return NULL;
        if (sym->type == t) return NULL;
        return struct_of_type(w, sym->type, depth + 1);
    }
    if (t->kind == CC_T_GENERIC || t->kind == CC_T_SLICE) {
        CcName cn = cc_index_canon(ix, t);
        sym = cc_index_sym(ix, cn);
        if (sym && sym->kind == CC_SYM_TYPE && sym->type && sym->type != t) return struct_of_type(w, sym->type, depth + 1);
    }
    return NULL;
}

static CcType *field_type(Walker *w, CcType *recv, const char *field) {
    const CcType *st = struct_of_type(w, recv, 0);
    const CcField *f;
    if (!st) return NULL;
    for (f = st->fields; f; f = f->next) {
        if (f->name && strcmp(f->name, field) == 0) return f->type;
        if (!f->name && f->type && (f->type->kind == CC_T_STRUCT)) {
            const CcField *g;
            for (g = f->type->fields; g; g = g->next)
                if (g->name && strcmp(g->name, field) == 0) return g->type;
        }
    }
    return NULL;
}

static CcType *return_type_of_sym(CcSym *sym) {
    if (!sym || sym->kind != CC_SYM_FUNC || !sym->type || sym->type->kind != CC_T_FUNC) return NULL;
    return sym->type->base;
}

static CcMethod *resolve_site(Walker *w, CcType *recv_type, const char *method, const char **type_name, const char **reason) {
    CcIndex *ix = w->s->ix;
    CcType *t = peel(recv_type);
    CcTypeInfo *info;
    CcMethod *m;
    const char *cand = NULL;
    *type_name = NULL;
    *reason = NULL;
    if (!t) return NULL;
    if (t->kind == CC_T_TYPEOF && !t->typeof_type) return NULL;
    *type_name = cc_index_canon(ix, t);
    info = cc_index_type_get(ix, *type_name);
    m = cc_index_method(ix, info, cc_intern(w->s->intern, method, strlen(method)), &cand);
    if (!m) *reason = cand;
    return m;
}

static CcType *type_of_expr(Walker *w, CcExpr *e) {
    CcIndex *ix = w->s->ix;
    if (!e) return NULL;
    switch (e->kind) {
    case CC_E_IDENT: {
        CcType *t = scope_lookup(w, e->name);
        CcSym *sym;
        if (t) return t;
        sym = cc_index_sym(ix, e->name);
        if (sym && sym->kind == CC_SYM_VAR) return sym->type;
        return NULL;
    }
    case CC_E_PAREN: return type_of_expr(w, e->a);
    case CC_E_CAST:
    case CC_E_COMPOUND: return e->type;
    case CC_E_STRING: return synth(w, CC_T_POINTER, named(w, "char"));
    case CC_E_NUMBER: return named(w, "int");
    case CC_E_UNARY:
        if (e->op == CC_OP_ADDR) return synth(w, CC_T_POINTER, type_of_expr(w, e->a));
        if (e->op == CC_OP_DEREF) {
            CcType *t = type_of_expr(w, e->a);
            return t && (t->kind == CC_T_POINTER || t->kind == CC_T_ARRAY) ? t->base : NULL;
        }
        return type_of_expr(w, e->a);
    case CC_E_ASSIGN: return type_of_expr(w, e->a);
    case CC_E_TERNARY: return type_of_expr(w, e->b);
    case CC_E_COMMA: return type_of_expr(w, e->b);
    case CC_E_MEMBER: {
        CcType *t = type_of_expr(w, e->a);
        if (!t) return NULL;
        return field_type(w, t, e->name);
    }
    case CC_E_INDEX: {
        CcType *t = type_of_expr(w, e->a);
        if (!t) return NULL;
        if (t->kind == CC_T_POINTER || t->kind == CC_T_ARRAY || t->kind == CC_T_SLICE) return t->base;
        return NULL;
    }
    case CC_E_CALL: {
        CcExpr *c = e->a;
        while (c && c->kind == CC_E_PAREN) c = c->a;
        if (c && c->kind == CC_E_IDENT) return return_type_of_sym(cc_index_sym(ix, c->name));
        if (c && c->kind == CC_E_MEMBER && c->arrow && c->name) {
            /* `p->m(args)`: a method call unless the struct has a field `m` */
            CcType *rt = type_of_expr(w, c->a);
            const char *tn, *reason;
            CcMethod *m;
            if (!rt) return NULL;
            if (field_type(w, rt, c->name)) return NULL;
            m = resolve_site(w, rt, c->name, &tn, &reason);
            if (m && m->sym) return return_type_of_sym(m->sym);
        }
        return NULL;
    }
    case CC_E_TYPE_SCOPED: {
        const char *tn = e->type ? cc_index_canon(ix, e->type) : NULL;
        if (!tn) return NULL;
        return return_type_of_sym(cc_index_sym(ix, cc_arena_printf(&w->s->arena, "%s_%s", tn, e->name)));
    }
    case CC_E_UFCS: {
        CcType *rt = type_of_expr(w, e->a);
        const char *tn, *reason;
        CcMethod *m = rt ? resolve_site(w, rt, e->name, &tn, &reason) : NULL;
        if (m && m->sym) return return_type_of_sym(m->sym);
        return NULL;
    }
    case CC_E_UNWRAP:
    case CC_E_UNWRAP_BODY:
    case CC_E_UNWRAP_OR:
    case CC_E_UNWRAP_DESTROY: {
        CcType *t = type_of_expr(w, e->a);
        if (t && t->kind == CC_T_RESULT) return t->base;
        return NULL;
    }
    case CC_E_TEMPLATE:
        return e->tpl_arena ? named(w, "CCString") : synth(w, CC_T_SLICE, named(w, "char"));
    case CC_E_SLICE_LIT: return synth(w, CC_T_SLICE, named(w, "char"));
    case CC_E_AWAIT:
    case CC_E_CALL_MODE:
    case CC_E_COMPTIME: return type_of_expr(w, e->a);
    default: return NULL;
    }
}

static void walk_stmt(Walker *w, CcStmt *s);
static void walk_expr(Walker *w, CcExpr *e);

static void walk_init(Walker *w, CcInit *in) {
    size_t i;
    if (!in) return;
    if (in->expr) walk_expr(w, in->expr);
    for (i = 0; i < in->list.n; i++) walk_init(w, in->list.items[i]);
}

static void walk_decl_local(Walker *w, CcDecl *d) {
    if (!d) return;
    if (d->kind == CC_D_VAR || d->kind == CC_D_FUNC) {
        if (d->kind == CC_D_VAR) scope_declare(w, d->name, d->type);
        walk_init(w, d->init);
        if (d->destroy_body) walk_stmt(w, d->destroy_body);
        if (d->body) walk_stmt(w, d->body);
    } else if (d->kind == CC_D_TYPEDEF) {
        scope_declare(w, d->name, NULL);
    }
}

static void visit_ufcs(Walker *w, CcExpr *e) {
    Site *site;
    CcType *rt;
    /* the parser spells `@parallel ... wait (ts)` and the join's `!>.wait()` tail
     * as UFCS nodes on the ident `@parallel`: not user call sites */
    {
        const CcExpr *root = e->a;
        while (root && (root->kind == CC_E_PAREN || root->kind == CC_E_UNWRAP || root->kind == CC_E_UNWRAP_OR ||
                        root->kind == CC_E_UNWRAP_BODY || root->kind == CC_E_UFCS))
            root = root->a;
        if (root && root->kind == CC_E_IDENT && root->name && root->name[0] == '@') return;
    }
    site = CC_NEW(&w->s->arena, Site);
    rt = type_of_expr(w, e->a);
    const CcToken *t = &w->s->unit->file->toks[e->span.first];
    CcLoc loc = cc_lex_loc(w->s->unit->file, t->off);
    site->method = e->name;
    site->file = loc.path;
    site->line = loc.line;
    site->recv_kind = e->a ? (int)e->a->kind : 0;
    if (rt) {
        const char *tn = NULL, *reason = NULL;
        CcMethod *m = resolve_site(w, rt, e->name, &tn, &reason);
        site->type = tn;
        if (m) {
            site->source = m->source;
            site->callee = m->callee;
        } else {
            site->reason = reason;
        }
    }
    CC_LIST_PUSH(&w->s->arena, &w->sites, site);
}

/* `p->m(args)`: the parser keeps it as a call of a member; it is a UFCS
 * site unless the struct really has a field `m`. */
static void visit_arrow_call(Walker *w, CcExpr *call) {
    CcExpr *mem = call->a;
    CcExpr probe;
    CcType *rt;
    if (!mem || mem->kind != CC_E_MEMBER || !mem->arrow || !mem->name) return;
    rt = type_of_expr(w, mem->a);
    if (rt && field_type(w, rt, mem->name)) return;
    probe = *call;
    probe.kind = CC_E_UFCS;
    probe.a = mem->a;
    probe.name = mem->name;
    visit_ufcs(w, &probe);
}

/* A statement-level macro invocation (`cc_arena_stack(a, n);`) may declare
 * locals: expand it and read the declarations out of the expansion. A
 * macro whose expansion invokes other macros is followed a few levels. */
static void declare_from_macro_text(Walker *w, const char *text, int depth) {
    CcDiag scratch;
    CcLexFile *f;
    CcParseOpts po;
    CcUnit *u;
    size_t i;
    if (depth > 4) return;
    text = cc_arena_printf(&w->s->arena, "void __cc_probe(void) {\n%s;\n}\n", text);
    cc_diag_init(&scratch, &w->s->arena);
    f = cc_lex(&w->s->arena, &scratch, "<macro statement>", text, strlen(text));
    memset(&po, 0, sizeof po);
    po.mode = CC_MODE_SOURCE;
    po.known_types = cc_index_known_types(w->s->ix);
    u = cc_parse(&w->s->arena, &scratch, w->s->intern, f, &po);
    if (u->decls.n != 1 || !u->decls.items[0]->body) return;
    for (i = 0; i < u->decls.items[0]->body->stmts.n; i++) {
        CcStmt *t = u->decls.items[0]->body->stmts.items[i];
        if (t->kind == CC_S_DECL && t->decl && t->decl->kind == CC_D_VAR) scope_declare(w, t->decl->name, t->decl->type);
        if (t->kind == CC_S_EXPR && t->expr && t->expr->kind == CC_E_CALL && t->expr->a && t->expr->a->kind == CC_E_IDENT) {
            const char *inner = cc_index_expand_call(w->s->ix, u, t->expr->span);
            if (inner) declare_from_macro_text(w, inner, depth + 1);
        }
    }
}

static void declare_from_macro_stmt(Walker *w, CcStmt *s) {
    CcExpr *e = s->expr;
    const char *text;
    if (!e || e->kind != CC_E_CALL || !e->a || e->a->kind != CC_E_IDENT) return;
    text = cc_index_expand_call(w->s->ix, w->s->unit, e->span);
    if (text) declare_from_macro_text(w, text, 0);
}

static void walk_expr(Walker *w, CcExpr *e) {
    size_t i;
    CcGenericSelArm *g;
    CcTplPart *pt;
    if (!e) return;
    if (e->kind == CC_E_UFCS) visit_ufcs(w, e);
    if (e->kind == CC_E_CALL) visit_arrow_call(w, e);
    if (e->kind == CC_E_CLOSURE) {
        scope_push(w);
        for (i = 0; i < e->params.n; i++) scope_declare(w, e->params.items[i]->name, e->params.items[i]->type);
        if (e->body) walk_stmt(w, e->body);
        scope_pop(w);
        return;
    }
    walk_expr(w, e->a);
    walk_expr(w, e->b);
    walk_expr(w, e->c);
    for (i = 0; i < e->args.n; i++) walk_expr(w, e->args.items[i]);
    walk_init(w, e->init);
    for (g = e->arms; g; g = g->next) walk_expr(w, g->expr);
    walk_expr(w, e->tpl_policy);
    for (pt = e->tpl_parts; pt; pt = pt->next) walk_expr(w, pt->expr);
    walk_expr(w, e->tpl_arena);
    walk_expr(w, e->scratch_bytes);
    if (e->body) {
        scope_push(w);
        if (e->kind == CC_E_UNWRAP_BODY && e->binder && e->a) {
            CcType *t = type_of_expr(w, e->a);
            scope_declare(w, e->binder, t && t->kind == CC_T_RESULT ? t->err : NULL);
        }
        walk_stmt(w, e->body);
        scope_pop(w);
    }
}

static void walk_stmt(Walker *w, CcStmt *s) {
    size_t i;
    CcParallelArm *arm;
    if (!s) return;
    scope_push(w);
    if (s->kind == CC_S_DECL) walk_decl_local(w, s->decl);
    else if (s->decl) walk_decl_local(w, s->decl);
    if (s->kind == CC_S_EXPR) declare_from_macro_stmt(w, s);
    if (s->kind == CC_S_ERRHANDLER || s->kind == CC_S_ERR_FWD) scope_declare(w, s->name, s->type);
    if (s->kind == CC_S_FOR_IN) {
        CcType *it = s->exprs.n ? type_of_expr(w, s->exprs.items[0]) : type_of_expr(w, s->expr2);
        CcType *elem = it && (it->kind == CC_T_SLICE || it->kind == CC_T_ARRAY || it->kind == CC_T_POINTER) ? it->base : NULL;
        if (s->name2) {
            scope_declare(w, s->name, named(w, "size_t"));
            scope_declare(w, s->name2, elem);
        } else {
            scope_declare(w, s->name, elem);
        }
    }
    if (s->kind == CC_S_WITH_DEADLINE && s->name) scope_declare(w, s->name, NULL);
    if ((s->kind == CC_S_PARALLEL || s->kind == CC_S_PARALLEL_FOR) && s->name) scope_declare(w, s->name, s->type);
    walk_expr(w, s->init_expr);
    walk_expr(w, s->expr);
    walk_expr(w, s->expr2);
    for (i = 0; i < s->exprs.n; i++) walk_expr(w, s->exprs.items[i]);
    walk_expr(w, s->par_pred);
    walk_expr(w, s->par_seq);
    walk_expr(w, s->par_wait);
    for (i = 0; i < s->par_cache.n; i++) walk_expr(w, s->par_cache.items[i]);
    walk_expr(w, s->par_dest);
    walk_expr(w, s->closing_spawn);
    for (arm = s->arms; arm; arm = arm->next) {
        walk_expr(w, arm->expr);
        walk_stmt(w, arm->serial);
    }
    if (s->kind == CC_S_BLOCK || s->stmts.n) {
        /* declarations in a block are visible to the statements after them:
         * one scope for the whole block, entered here */
        for (i = 0; i < s->stmts.n; i++) {
            CcStmt *t = s->stmts.items[i];
            if (t->kind == CC_S_DECL) {
                walk_decl_local(w, t->decl);
            } else {
                /* bindings that outlive the statement: a macro's declarations,
                 * `CCParallel h = @parallel {...}` */
                if (t->kind == CC_S_EXPR) declare_from_macro_stmt(w, t);
                if ((t->kind == CC_S_PARALLEL || t->kind == CC_S_PARALLEL_FOR) && t->name) scope_declare(w, t->name, t->type);
                walk_stmt(w, t);
            }
        }
    }
    walk_stmt(w, s->body);
    walk_stmt(w, s->else_body);
    walk_expr(w, s->par_tail);
    scope_pop(w);
}

static void walk_unit(Walker *w) {
    CcUnit *u = w->s->unit;
    size_t i, k;
    for (i = 0; i < u->decls.n; i++) {
        CcDecl *d = u->decls.items[i];
        if (d->kind == CC_D_FUNC || d->kind == CC_D_COMPTIME_FN) {
            scope_push(w);
            if (d->type)
                for (k = 0; k < d->type->params.n; k++) scope_declare(w, d->type->params.items[k]->name, d->type->params.items[k]->type);
            walk_stmt(w, d->body);
            scope_pop(w);
        } else if (d->kind == CC_D_VAR) {
            scope_push(w);
            walk_init(w, d->init);
            scope_pop(w);
        } else if (d->kind == CC_D_TYPEHOOKS) {
            CcHookEntry *e;
            scope_push(w);
            for (e = d->entries; e; e = e->next) {
                walk_expr(w, e->value);
                walk_stmt(w, e->body);
            }
            scope_pop(w);
        } else if (d->kind == CC_D_COMPTIME_BLOCK || d->kind == CC_D_GENERIC_FACTORY) {
            scope_push(w);
            walk_stmt(w, d->body);
            scope_pop(w);
        }
    }
}

/* ---- --sites / --gaps -------------------------------------------------- */

typedef struct Count {
    const char *key;
    const char *key2;
    size_t n;
    struct Count *next;
} Count;

static Count *count_add(CcArena *a, Count **list, const char *key, const char *key2) {
    Count *c;
    for (c = *list; c; c = c->next)
        if (strcmp(c->key, key) == 0 && ((!key2 && !c->key2) || (key2 && c->key2 && strcmp(c->key2, key2) == 0))) {
            c->n++;
            return c;
        }
    c = CC_NEW(a, Count);
    c->key = cc_arena_strdup(a, key);
    c->key2 = key2 ? cc_arena_strdup(a, key2) : NULL;
    c->n = 1;
    c->next = *list;
    *list = c;
    return c;
}

static int count_cmp(const void *pa, const void *pb) {
    const Count *a = *(Count *const *)pa, *b = *(Count *const *)pb;
    if (a->n != b->n) return a->n < b->n ? 1 : -1;
    {
        int c = strcmp(a->key, b->key);
        if (c) return c;
    }
    if (!a->key2 || !b->key2) return 0;
    return strcmp(a->key2, b->key2);
}

static Count **count_sorted(CcArena *a, Count *list, size_t *n_out) {
    size_t n = 0, i = 0;
    Count *c, **arr;
    for (c = list; c; c = c->next) n++;
    arr = CC_NEW_N(a, Count *, n + 1);
    for (c = list; c; c = c->next) arr[i++] = c;
    qsort(arr, n, sizeof *arr, count_cmp);
    *n_out = n;
    return arr;
}

int main(int argc, char **argv) {
    int i = 1;
    const char *mode = NULL;
    include_dirs[n_include_dirs++] = "cc/include";
    include_dirs[n_include_dirs] = NULL;
    while (i < argc && argv[i][0] == '-' && strcmp(argv[i], "--dump") && strcmp(argv[i], "--resolve") &&
           strcmp(argv[i], "--gaps") && strcmp(argv[i], "--sites")) {
        if (strcmp(argv[i], "-I") == 0 && i + 1 < argc && n_include_dirs < 15) {
            if (n_include_dirs == 1 && strcmp(include_dirs[0], "cc/include") == 0) n_include_dirs = 0;
            include_dirs[n_include_dirs++] = argv[i + 1];
            include_dirs[n_include_dirs] = NULL;
            i += 2;
        } else {
            fprintf(stderr, "ccindex: unknown option %s\n", argv[i]);
            return 2;
        }
    }
    if (i < argc) mode = argv[i++];
    if (!mode) {
        fprintf(stderr, "usage: ccindex [-I dir] --dump FILE | --resolve FILE Type method | --sites FILE | --gaps FILE...\n");
        return 2;
    }
    if (strcmp(mode, "--dump") == 0 && i < argc) {
        Session s;
        if (!session_open(&s, argv[i])) return 1;
        dump(&s);
        session_close(&s);
        return 0;
    }
    if (strcmp(mode, "--resolve") == 0 && i + 2 < argc) {
        Session s;
        CcTypeInfo *info;
        CcMethod *m;
        const char *cand = NULL;
        int rc;
        if (!session_open(&s, argv[i])) return 1;
        if (strpbrk(argv[i + 1], ":[*!? ")) {
            /* a type spelling (`Vec::[int]`, `char[:]`, `int!>(CCError)`): parse it as a declaration */
            const char *text = cc_arena_printf(&s.arena, "%s __cc_probe;\n", argv[i + 1]);
            CcDiag scratch;
            CcLexFile *f;
            CcParseOpts po;
            CcUnit *u;
            cc_diag_init(&scratch, &s.arena);
            f = cc_lex(&s.arena, &scratch, "<type>", text, strlen(text));
            memset(&po, 0, sizeof po);
            po.mode = CC_MODE_HEADER;
            po.known_types = cc_index_known_types(s.ix);
            u = cc_parse(&s.arena, &scratch, s.intern, f, &po);
            if (scratch.n_errors || u->decls.n != 1 || !u->decls.items[0]->type) {
                cc_diag_print(&scratch, stderr);
                fprintf(stderr, "ccindex: cannot read the type '%s'\n", argv[i + 1]);
                return 2;
            }
            info = cc_index_type_get(s.ix, cc_index_canon(s.ix, peel(u->decls.items[0]->type)));
        } else {
            info = cc_index_type_get(s.ix, cc_intern(s.intern, argv[i + 1], strlen(argv[i + 1])));
        }
        m = cc_index_method(s.ix, info, cc_intern(s.intern, argv[i + 2], strlen(argv[i + 2])), &cand);
        if (m) {
            printf("%s.%s -> %s(%srecv, ...) [%s] %s\n", argv[i + 1], argv[i + 2], m->callee, m->recv_by_ptr ? "&" : "", m->source, m->origin ? m->origin : "");
            rc = 0;
        } else {
            printf("error: %s\n", cand ? cand : "unresolved");
            rc = 1;
        }
        session_close(&s);
        return rc;
    }
    if ((strcmp(mode, "--sites") == 0 || strcmp(mode, "--gaps") == 0) && i < argc) {
        int gaps = strcmp(mode, "--gaps") == 0;
        CcArena agg;
        Count *by_source = NULL, *unresolved = NULL, *unknown_methods = NULL, *unknown_recv = NULL, *reasons = NULL;
        size_t total = 0, n_resolved = 0, n_unresolved = 0, n_unknown = 0, n_files = 0, n_parse_err_files = 0;
        cc_arena_init(&agg, 1 << 20);
        for (; i < argc; i++) {
            Session s;
            Walker w;
            size_t k;
            if (!session_open(&s, argv[i])) continue;
            n_files++;
            memset(&w, 0, sizeof w);
            w.s = &s;
            walk_unit(&w);
            {
                /* diagnostics on the unit's own lines (header diagnostics repeat per file) */
                size_t d;
                int own = 0;
                for (d = 0; d < s.diag.msgs.n; d++)
                    if (s.diag.msgs.items[d]->sev == CC_SEV_ERROR && s.diag.msgs.items[d]->loc.path &&
                        strcmp(s.diag.msgs.items[d]->loc.path, argv[i]) == 0)
                        own = 1;
                if (own) {
                    n_parse_err_files++;
                    fprintf(stderr, "parse errors on its own lines: %s\n", argv[i]);
                }
            }
            for (k = 0; k < w.sites.n; k++) {
                Site *site = w.sites.items[k];
                total++;
                if (!gaps)
                    printf("%s:%u %s.%s -> %s%s%s\n", site->file ? site->file : argv[i], site->line, site->type ? site->type : "?",
                           site->method, site->source ? site->callee : "UNRESOLVED", site->source ? " [" : " ",
                           site->source ? cc_arena_printf(&s.arena, "%s]", site->source) : site->reason ? site->reason : "receiver type unknown");
                if (site->source) {
                    n_resolved++;
                    count_add(&agg, &by_source, site->source, NULL);
                } else if (site->type) {
                    n_unresolved++;
                    count_add(&agg, &unresolved, site->type, site->method);
                    if (site->reason) count_add(&agg, &reasons, site->reason, NULL);
                } else {
                    n_unknown++;
                    count_add(&agg, &unknown_methods, site->method, NULL);
                    count_add(&agg, &unknown_recv, cc_expr_kind_name((CcExprKind)site->recv_kind), NULL);
                }
            }
            if (gaps) {
                /* parse diagnostics are noise for the gap table; count them */
                cc_arena_free(&s.arena);
            } else {
                session_close(&s);
            }
        }
        if (gaps) {
            size_t n, k;
            Count **arr;
            printf("Files: %zu (%zu with parse errors on their own lines). UFCS call sites: %zu.\n\n", n_files, n_parse_err_files, total);
            printf("| Outcome | Sites |\n|---|---|\n");
            arr = count_sorted(&agg, by_source, &n);
            for (k = 0; k < n; k++) printf("| resolved: %s | %zu |\n", arr[k]->key, arr[k]->n);
            printf("| resolved (all sources) | %zu |\n", n_resolved);
            printf("| unresolved, receiver type known | %zu |\n", n_unresolved);
            printf("| receiver type unknown | %zu |\n\n", n_unknown);
            printf("### Unresolved with a known receiver type\n\n");
            printf("(Type, method) pairs no declaration, hook or registration names.\n\n| Receiver type | Method | Sites |\n|---|---|---|\n");
            arr = count_sorted(&agg, unresolved, &n);
            for (k = 0; k < n; k++) printf("| `%s` | `%s` | %zu |\n", arr[k]->key, arr[k]->key2, arr[k]->n);
            printf("\n### Receiver type unknown\n\nBy receiver expression kind:\n\n| Receiver kind | Sites |\n|---|---|\n");
            arr = count_sorted(&agg, unknown_recv, &n);
            for (k = 0; k < n; k++) printf("| %s | %zu |\n", arr[k]->key, arr[k]->n);
            printf("\nMost frequent methods on untyped receivers:\n\n| Method | Sites |\n|---|---|\n");
            arr = count_sorted(&agg, unknown_methods, &n);
            for (k = 0; k < n && k < 40; k++) printf("| `%s` | %zu |\n", arr[k]->key, arr[k]->n);
        }
        cc_arena_free(&agg);
        return 0;
    }
    fprintf(stderr, "usage: ccindex [-I dir] --dump FILE | --resolve FILE Type method | --sites FILE | --gaps FILE...\n");
    return 2;
}
