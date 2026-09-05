/* AST support: interning, node constructors, kind names, the debug dump and
 * the pre-order walk. See ast.h and parse.h for the contracts. */
#include "ast.h"
#include "parse.h"
#include <stdio.h>
#include <string.h>

/* ---- interning ------------------------------------------------------- */

struct CcIntern {
    CcArena *a;
    const char **slots;   /* open addressing, NULL = empty */
    size_t cap;           /* power of two */
    size_t n;
};

static uint64_t cc__hash(const char *s, size_t n) {
    uint64_t h = 1469598103934665603ull;
    size_t i;
    for (i = 0; i < n; i++) {
        h ^= (unsigned char)s[i];
        h *= 1099511628211ull;
    }
    return h;
}

CcIntern *cc_intern_new(CcArena *a) {
    CcIntern *in = CC_NEW(a, CcIntern);
    in->a = a;
    in->cap = 1024;
    in->slots = CC_NEW_N(a, const char *, in->cap);
    in->n = 0;
    return in;
}

static void cc__intern_grow(CcIntern *in) {
    size_t ncap = in->cap * 2, i;
    const char **ns = CC_NEW_N(in->a, const char *, ncap);
    for (i = 0; i < in->cap; i++) {
        const char *s = in->slots[i];
        size_t j;
        if (!s) continue;
        j = (size_t)cc__hash(s, strlen(s)) & (ncap - 1);
        while (ns[j]) j = (j + 1) & (ncap - 1);
        ns[j] = s;
    }
    in->slots = ns;
    in->cap = ncap;
}

CcName cc_intern(CcIntern *in, const char *s, size_t n) {
    size_t j;
    char *copy;
    if (in->n * 2 >= in->cap) cc__intern_grow(in);
    j = (size_t)cc__hash(s, n) & (in->cap - 1);
    for (;;) {
        const char *k = in->slots[j];
        if (!k) break;
        if (strlen(k) == n && memcmp(k, s, n) == 0) return k;
        j = (j + 1) & (in->cap - 1);
    }
    copy = cc_arena_strndup(in->a, s, n);
    in->slots[j] = copy;
    in->n++;
    return copy;
}

/* ---- constructors ---------------------------------------------------- */

CcExpr *cc_expr_new(CcArena *a, CcExprKind k, CcSpan span) {
    CcExpr *e = CC_NEW(a, CcExpr);
    e->kind = k;
    e->span = span;
    return e;
}

CcStmt *cc_stmt_new(CcArena *a, CcStmtKind k, CcSpan span) {
    CcStmt *s = CC_NEW(a, CcStmt);
    s->kind = k;
    s->span = span;
    return s;
}

CcDecl *cc_decl_new(CcArena *a, CcDeclKind k, CcSpan span) {
    CcDecl *d = CC_NEW(a, CcDecl);
    d->kind = k;
    d->span = span;
    return d;
}

CcType *cc_type_new(CcArena *a, CcTypeKind k, CcSpan span) {
    CcType *t = CC_NEW(a, CcType);
    t->kind = k;
    t->span = span;
    return t;
}

/* ---- kind names ------------------------------------------------------ */

const char *cc_expr_kind_name(CcExprKind k) {
    switch (k) {
    case CC_E_IDENT: return "ident";
    case CC_E_NUMBER: return "number";
    case CC_E_CHAR: return "char";
    case CC_E_STRING: return "string";
    case CC_E_PAREN: return "paren";
    case CC_E_UNARY: return "unary";
    case CC_E_BINARY: return "binary";
    case CC_E_ASSIGN: return "assign";
    case CC_E_TERNARY: return "ternary";
    case CC_E_CALL: return "call";
    case CC_E_INDEX: return "index";
    case CC_E_MEMBER: return "member";
    case CC_E_CAST: return "cast";
    case CC_E_SIZEOF_TYPE: return "sizeof type";
    case CC_E_COMPOUND: return "compound literal";
    case CC_E_GENERIC_SEL: return "_Generic";
    case CC_E_STMT_EXPR: return "statement expression";
    case CC_E_COMMA: return "comma";
    case CC_E_UFCS: return "UFCS call";
    case CC_E_TYPE_SCOPED: return "Type.fn call";
    case CC_E_GENERIC_FN: return "generic call";
    case CC_E_UNWRAP: return "!>";
    case CC_E_UNWRAP_BODY: return "!> body";
    case CC_E_UNWRAP_OR: return "?>";
    case CC_E_UNWRAP_DESTROY: return "!> @destroy";
    case CC_E_TEMPLATE: return "@string";
    case CC_E_SLICE_LIT: return "@slice";
    case CC_E_SCRATCH: return "@scratch";
    case CC_E_CLOSURE: return "closure";
    case CC_E_AWAIT: return "@await";
    case CC_E_COMPTIME: return "@comptime()";
    case CC_E_CREATE: return "@create";
    case CC_E_EMIT: return "@emit";
    case CC_E_VARIANT_LIT: return "variant literal";
    case CC_E_CALL_MODE: return "call mode";
    case CC_E_MOVE: return "move";
    case CC_E_RANGE: return "range";
    case CC_E_PP: return "preprocessor line";
    case CC_E_TYPE_ARG: return "type argument";
    }
    return "?expr";
}

const char *cc_stmt_kind_name(CcStmtKind k) {
    switch (k) {
    case CC_S_EXPR: return "expression";
    case CC_S_DECL: return "declaration";
    case CC_S_BLOCK: return "block";
    case CC_S_IF: return "if";
    case CC_S_WHILE: return "while";
    case CC_S_DO: return "do";
    case CC_S_FOR: return "for";
    case CC_S_SWITCH: return "switch";
    case CC_S_CASE: return "case";
    case CC_S_RETURN: return "return";
    case CC_S_BREAK: return "break";
    case CC_S_CONTINUE: return "continue";
    case CC_S_GOTO: return "goto";
    case CC_S_LABEL: return "label";
    case CC_S_ASM: return "asm";
    case CC_S_DEFER: return "@defer";
    case CC_S_CANCEL_DEFER: return "@cancel_defer";
    case CC_S_ERRHANDLER: return "@errhandler";
    case CC_S_ERR_FWD: return "@err";
    case CC_S_UNWRAP: return "!> statement";
    case CC_S_FOR_IN: return "for in";
    case CC_S_PARALLEL: return "@parallel";
    case CC_S_PARALLEL_FOR: return "@parallel for";
    case CC_S_PARALLEL_DEST: return "@parallel(dest)";
    case CC_S_SERIAL: return "@serial";
    case CC_S_STAGE: return "@stage";
    case CC_S_WITH_DEADLINE: return "@with_deadline";
    case CC_S_WITH: return "@with";
    case CC_S_CLOSING: return "@closing";
    case CC_S_SPAWN_BLOCK: return "@spawn";
    case CC_S_MODE_BLOCK: return "mode block";
    case CC_S_UNSAFE: return "unsafe";
    case CC_S_COMPTIME_IF: return "@comptime if";
    case CC_S_COMPTIME_BLOCK: return "@comptime block";
    case CC_S_COMPTIME_FOR: return "@comptime for";
    }
    return "?stmt";
}

const char *cc_decl_kind_name(CcDeclKind k) {
    switch (k) {
    case CC_D_VAR: return "variable";
    case CC_D_FUNC: return "function";
    case CC_D_TYPEDEF: return "typedef";
    case CC_D_TAGGED: return "tag declaration";
    case CC_D_STATIC_ASSERT: return "_Static_assert";
    case CC_D_PP: return "preprocessor line";
    case CC_D_EMPTY: return "empty declaration";
    case CC_D_TYPEHOOKS: return "@typehooks";
    case CC_D_TYPEVIEW: return "@typeview";
    case CC_D_VARIANT: return "@variant";
    case CC_D_GRAMMAR: return "@grammar";
    case CC_D_GENERIC_FACTORY: return "CC_GENERIC_FACTORY";
    case CC_D_COMPTIME_FN: return "@comptime function";
    case CC_D_COMPTIME_BLOCK: return "@comptime block";
    case CC_D_COMPTIME_IF: return "@comptime if";
    case CC_D_SCOPED_TYPE: return "@scoped type";
    case CC_D_PRAGMA_CC: return "#pragma(@)";
    case CC_D_LINK: return "@link";
    case CC_D_TASK_DOC: return "@task doc";
    case CC_D_MACRO_CALL: return "macro call";
    case CC_D_STMT: return "statement";
    }
    return "?decl";
}

const char *cc_type_kind_name(CcTypeKind k) {
    switch (k) {
    case CC_T_NAMED: return "type";
    case CC_T_POINTER: return "pointer";
    case CC_T_ARRAY: return "array";
    case CC_T_FUNC: return "function type";
    case CC_T_STRUCT: return "struct";
    case CC_T_ENUM: return "enum";
    case CC_T_TYPEOF: return "typeof";
    case CC_T_ATOMIC: return "_Atomic";
    case CC_T_RESULT: return "result type";
    case CC_T_SLICE: return "slice type";
    case CC_T_CHAN: return "channel type";
    case CC_T_GENERIC: return "generic type";
    case CC_T_SCOPED: return "@scoped type";
    case CC_T_AUTO: return "@auto";
    case CC_T_MACRO: return "macro type";
    case CC_T_VALUE: return "value argument";
    }
    return "?type";
}

/* ---- dump ------------------------------------------------------------ */

typedef struct CcDump {
    FILE *out;
    const CcLexFile *f;
} CcDump;

static const char *cc__op_text(CcOp op) {
    switch (op) {
    case CC_OP_NONE: return "";
    case CC_OP_ADD: case CC_OP_POS: return "+";
    case CC_OP_SUB: case CC_OP_NEG: return "-";
    case CC_OP_MUL: case CC_OP_DEREF: return "*";
    case CC_OP_DIV: return "/";
    case CC_OP_MOD: return "%";
    case CC_OP_SHL: return "<<";
    case CC_OP_SHR: return ">>";
    case CC_OP_LT: return "<";
    case CC_OP_GT: return ">";
    case CC_OP_LE: return "<=";
    case CC_OP_GE: return ">=";
    case CC_OP_EQ: return "==";
    case CC_OP_NE: return "!=";
    case CC_OP_AND: case CC_OP_ADDR: return "&";
    case CC_OP_XOR: return "^";
    case CC_OP_OR: return "|";
    case CC_OP_LAND: return "&&";
    case CC_OP_LOR: return "||";
    case CC_OP_NOT: return "!";
    case CC_OP_BITNOT: return "~";
    case CC_OP_PREINC: return "++";
    case CC_OP_PREDEC: return "--";
    case CC_OP_POSTINC: return "post++";
    case CC_OP_POSTDEC: return "post--";
    case CC_OP_SIZEOF: return "sizeof";
    case CC_OP_ALIGNOF: return "_Alignof";
    case CC_OP_ASSIGN: return "=";
    case CC_OP_ADD_ASSIGN: return "+=";
    case CC_OP_SUB_ASSIGN: return "-=";
    case CC_OP_MUL_ASSIGN: return "*=";
    case CC_OP_DIV_ASSIGN: return "/=";
    case CC_OP_MOD_ASSIGN: return "%=";
    case CC_OP_SHL_ASSIGN: return "<<=";
    case CC_OP_SHR_ASSIGN: return ">>=";
    case CC_OP_AND_ASSIGN: return "&=";
    case CC_OP_XOR_ASSIGN: return "^=";
    case CC_OP_OR_ASSIGN: return "|=";
    }
    return "?";
}

static void cc__indent(CcDump *d, int depth) {
    int i;
    for (i = 0; i < depth; i++) fputs("  ", d->out);
}

static void cc__pos(CcDump *d, CcSpan span) {
    const CcToken *t;
    if (!d->f || span.first >= d->f->n_toks) { fputs("\n", d->out); return; }
    t = &d->f->toks[span.first];
    fprintf(d->out, " (%u:%u)\n", (unsigned)t->line, (unsigned)t->col);
}

static void cc__tok_text(CcDump *d, uint32_t tok) {
    const CcToken *t;
    if (!d->f || tok >= d->f->n_toks) return;
    t = &d->f->toks[tok];
    fprintf(d->out, "%.*s", (int)t->len, d->f->src + t->off);
}

static void cc__dump_type(CcDump *d, const CcType *t, int depth);
static void cc__dump_expr(CcDump *d, const CcExpr *e, int depth);
static void cc__dump_init(CcDump *d, const CcInit *in, int depth);
static void cc__dump_stmt(CcDump *d, const CcStmt *s, int depth);
static void cc__dump_decl(CcDump *d, const CcDecl *dc, int depth);

static void cc__dump_attrs(CcDump *d, const CcAttr *a, int depth) {
    for (; a; a = a->next) {
        cc__indent(d, depth);
        fprintf(d->out, "attr [%u..%u] %s", a->span.first, a->span.last, a->name ? a->name : "");
        if (a->value) fprintf(d->out, " %s", a->value);
        cc__pos(d, a->span);
    }
}

static void cc__dump_params(CcDump *d, const CcParamList *pl, int depth) {
    size_t i;
    for (i = 0; i < pl->n; i++) {
        const CcParam *p = pl->items[i];
        cc__indent(d, depth);
        fprintf(d->out, "param [%u..%u] %s", p->span.first, p->span.last,
                p->is_variadic ? "..." : (p->name ? p->name : ""));
        cc__pos(d, p->span);
        if (p->type) cc__dump_type(d, p->type, depth + 1);
        if (p->default_value) cc__dump_expr(d, p->default_value, depth + 1);
        cc__dump_attrs(d, p->attrs, depth + 1);
    }
}

static void cc__dump_type(CcDump *d, const CcType *t, int depth) {
    size_t i;
    if (!t) return;
    cc__indent(d, depth);
    fprintf(d->out, "%s [%u..%u]", cc_type_kind_name(t->kind), t->span.first, t->span.last);
    if (t->name) fprintf(d->out, " %s", t->name);
    if (t->quals & CC_Q_CONST) fputs(" const", d->out);
    if (t->quals & CC_Q_VOLATILE) fputs(" volatile", d->out);
    if (t->quals & CC_Q_RESTRICT) fputs(" restrict", d->out);
    if (t->quals & CC_Q_ATOMIC) fputs(" _Atomic", d->out);
    switch (t->kind) {
    case CC_T_ARRAY:
        if (t->array_static) fputs(" static", d->out);
        if (t->array_star) fputs(" [*]", d->out);
        break;
    case CC_T_FUNC:
        if (!t->has_prototype) fputs(" ()", d->out);
        break;
    case CC_T_STRUCT:
        if (t->is_union) fputs(" union", d->out);
        if (t->is_definition) fputs(" definition", d->out);
        break;
    case CC_T_ENUM:
        if (t->is_definition) fputs(" definition", d->out);
        break;
    case CC_T_RESULT:
        if (t->optional) fputs(" ?>", d->out);
        break;
    case CC_T_SLICE:
        fprintf(d->out, " [%s:%s%s]", t->fixed ? "n" : "", t->sentinel ? "0" : "", t->unique ? "!" : "");
        break;
    case CC_T_CHAN:
        fprintf(d->out, " [~%s%s%s%s %c]", t->cap ? "cap " : "", t->topology ? t->topology : "",
                t->ordered ? " ordered" : "", t->sync ? " sync" : "", t->dir ? t->dir : '?');
        break;
    default: break;
    }
    cc__pos(d, t->span);
    cc__dump_attrs(d, t->attrs, depth + 1);
    if (t->base) cc__dump_type(d, t->base, depth + 1);
    if (t->size) cc__dump_expr(d, t->size, depth + 1);
    if (t->kind == CC_T_FUNC) cc__dump_params(d, &t->params, depth + 1);
    {
        const CcField *fl;
        for (fl = t->fields; fl; fl = fl->next) {
            cc__indent(d, depth + 1);
            if (fl->is_pp) {
                fprintf(d->out, "preprocessor line [%u..%u] ", fl->span.first, fl->span.last);
                cc__tok_text(d, fl->pp_tok);
            } else {
                fprintf(d->out, "field [%u..%u] %s", fl->span.first, fl->span.last, fl->name ? fl->name : "<anonymous>");
            }
            cc__pos(d, fl->span);
            if (fl->type) cc__dump_type(d, fl->type, depth + 2);
            if (fl->bit_width) cc__dump_expr(d, fl->bit_width, depth + 2);
            cc__dump_attrs(d, fl->attrs, depth + 2);
        }
    }
    {
        const CcEnumerator *en;
        for (en = t->enumerators; en; en = en->next) {
            cc__indent(d, depth + 1);
            if (en->is_pp) {
                fprintf(d->out, "preprocessor line [%u..%u] ", en->span.first, en->span.last);
                cc__tok_text(d, en->pp_tok);
            } else {
                fprintf(d->out, "enumerator [%u..%u] %s", en->span.first, en->span.last, en->name ? en->name : "");
            }
            cc__pos(d, en->span);
            if (en->value) cc__dump_expr(d, en->value, depth + 2);
            if (en->comptime) cc__dump_stmt(d, en->comptime, depth + 2);
        }
    }
    if (t->typeof_expr) cc__dump_expr(d, t->typeof_expr, depth + 1);
    if (t->typeof_type) cc__dump_type(d, t->typeof_type, depth + 1);
    if (t->err) cc__dump_type(d, t->err, depth + 1);
    if (t->fixed) cc__dump_expr(d, t->fixed, depth + 1);
    if (t->cap) cc__dump_expr(d, t->cap, depth + 1);
    if (t->chan_hooks) cc__dump_init(d, t->chan_hooks, depth + 1);
    if (t->value) cc__dump_expr(d, t->value, depth + 1);
    for (i = 0; i < t->args.n; i++) cc__dump_type(d, t->args.items[i], depth + 1);
}

static void cc__dump_init(CcDump *d, const CcInit *in, int depth) {
    const CcDesignator *ds;
    size_t i;
    if (!in) return;
    cc__indent(d, depth);
    if (in->is_pp) {
        fprintf(d->out, "preprocessor line [%u..%u] ", in->span.first, in->span.last);
        cc__tok_text(d, in->pp_tok);
    } else {
        fprintf(d->out, "%s [%u..%u]", in->is_list ? "initializer list" : "initializer", in->span.first, in->span.last);
    }
    for (ds = in->designators; ds; ds = ds->next) {
        if (ds->field) fprintf(d->out, " .%s", ds->field);
        else fputs(" [i]", d->out);
    }
    cc__pos(d, in->span);
    for (ds = in->designators; ds; ds = ds->next) {
        if (ds->index) cc__dump_expr(d, ds->index, depth + 1);
        if (ds->index_hi) cc__dump_expr(d, ds->index_hi, depth + 1);
    }
    if (in->expr) cc__dump_expr(d, in->expr, depth + 1);
    for (i = 0; i < in->list.n; i++) cc__dump_init(d, in->list.items[i], depth + 1);
}

static void cc__dump_tpl(CcDump *d, const CcTplPart *pt, int depth) {
    for (; pt; pt = pt->next) {
        cc__indent(d, depth);
        if (pt->is_slot) {
            fprintf(d->out, "slot [%u..%u]", pt->span.first, pt->span.last);
            if (pt->tag) fprintf(d->out, " $~%s", pt->tag);
        } else {
            fprintf(d->out, "%s [%u..%u] %.*s", pt->is_verbatim ? "verbatim" : "literal",
                    pt->span.first, pt->span.last, (int)(pt->len > 40 ? 40 : pt->len),
                    d->f ? d->f->src + pt->off : "");
        }
        cc__pos(d, pt->span);
        if (pt->expr) {
            const CcLexFile *save = d->f;   /* slot expressions index the slot's own token array */
            if (pt->file) d->f = pt->file;
            cc__dump_expr(d, pt->expr, depth + 1);
            d->f = save;
        }
    }
}

static void cc__dump_expr(CcDump *d, const CcExpr *e, int depth) {
    size_t i;
    const CcCapture *c;
    const CcGenericSelArm *g;
    if (!e) return;
    cc__indent(d, depth);
    fprintf(d->out, "%s [%u..%u]", cc_expr_kind_name(e->kind), e->span.first, e->span.last);
    switch (e->kind) {
    case CC_E_IDENT: fprintf(d->out, " %s", e->name ? e->name : ""); break;
    case CC_E_NUMBER: case CC_E_CHAR: case CC_E_STRING: case CC_E_PP:
        fputc(' ', d->out);
        cc__tok_text(d, e->tok);
        if (e->kind == CC_E_STRING && e->n_string_toks > 1) fprintf(d->out, " (+%u)", e->n_string_toks - 1);
        break;
    case CC_E_UNARY: case CC_E_BINARY: case CC_E_ASSIGN:
        fprintf(d->out, " %s", cc__op_text(e->op));
        break;
    case CC_E_MEMBER:
        fprintf(d->out, " %s%s", e->arrow ? "->" : ".", e->name ? e->name : "");
        break;
    case CC_E_UFCS: case CC_E_TYPE_SCOPED: case CC_E_GENERIC_FN: case CC_E_VARIANT_LIT:
    case CC_E_CALL_MODE:
        fprintf(d->out, " %s", e->name ? e->name : "");
        break;
    case CC_E_UNWRAP_BODY: case CC_E_UNWRAP_OR:
        if (e->binder) fprintf(d->out, " (%s)", e->binder);
        break;
    case CC_E_CLOSURE:
        if (e->closure_unsafe) fputs(" @unsafe", d->out);
        if (e->closure_async) fputs(" async", d->out);
        break;
    case CC_E_CREATE:
        if (e->create_var) fprintf(d->out, " %s@", e->create_var);
        break;
    default: break;
    }
    cc__pos(d, e->span);
    if (e->type) cc__dump_type(d, e->type, depth + 1);
    if (e->a) cc__dump_expr(d, e->a, depth + 1);
    if (e->b) cc__dump_expr(d, e->b, depth + 1);
    if (e->c) cc__dump_expr(d, e->c, depth + 1);
    for (i = 0; i < e->targs.n; i++) cc__dump_type(d, e->targs.items[i], depth + 1);
    for (i = 0; i < e->args.n; i++) cc__dump_expr(d, e->args.items[i], depth + 1);
    if (e->init) cc__dump_init(d, e->init, depth + 1);
    for (g = e->arms; g; g = g->next) {
        cc__indent(d, depth + 1);
        fprintf(d->out, "%s\n", g->type ? "generic arm" : "generic default");
        if (g->type) cc__dump_type(d, g->type, depth + 2);
        if (g->expr) cc__dump_expr(d, g->expr, depth + 2);
    }
    if (e->tpl_policy) cc__dump_expr(d, e->tpl_policy, depth + 1);
    cc__dump_tpl(d, e->tpl_parts, depth + 1);
    if (e->tpl_arena) cc__dump_expr(d, e->tpl_arena, depth + 1);
    if (e->scratch_bytes) cc__dump_expr(d, e->scratch_bytes, depth + 1);
    if (e->kind == CC_E_CLOSURE) cc__dump_params(d, &e->params, depth + 1);
    for (c = e->captures; c; c = c->next) {
        cc__indent(d, depth + 1);
        fprintf(d->out, "capture [%u..%u] %s%s%s", c->span.first, c->span.last,
                c->is_safe ? "@safe " : "", c->by_ref ? "&" : "", c->name ? c->name : "");
        cc__pos(d, c->span);
        if (c->init) cc__dump_expr(d, c->init, depth + 2);
    }
    if (e->body) cc__dump_stmt(d, e->body, depth + 1);
}

static void cc__dump_arms(CcDump *d, const CcParallelArm *arm, int depth) {
    for (; arm; arm = arm->next) {
        cc__indent(d, depth);
        fprintf(d->out, "arm [%u..%u]", arm->span.first, arm->span.last);
        if (arm->target) fprintf(d->out, " %s =", arm->target);
        if (arm->unwrap) fputs(" !>", d->out);
        cc__pos(d, arm->span);
        if (arm->expr) cc__dump_expr(d, arm->expr, depth + 1);
        if (arm->serial) cc__dump_stmt(d, arm->serial, depth + 1);
    }
}

static void cc__dump_stmt(CcDump *d, const CcStmt *s, int depth) {
    size_t i;
    if (!s) return;
    cc__indent(d, depth);
    fprintf(d->out, "%s [%u..%u]", cc_stmt_kind_name(s->kind), s->span.first, s->span.last);
    if (s->name) fprintf(d->out, " %s", s->name);
    if (s->name2) fprintf(d->out, ", %s", s->name2);
    switch (s->kind) {
    case CC_S_DEFER:
        if (s->defer_on == 'o') fputs(" (ok)", d->out);
        else if (s->defer_on == 'e') fputs(" (err)", d->out);
        break;
    case CC_S_CASE:
        if (s->is_default) fputs(" default", d->out);
        if (s->case_arm) fprintf(d->out, " .%s", s->case_arm);
        if (s->case_bind) fprintf(d->out, "(%s)", s->case_bind);
        break;
    case CC_S_SWITCH:
        if (s->is_variant_switch) fputs(" @switch", d->out);
        break;
    case CC_S_FOR_IN:
        if (s->is_at_for) fputs(" @for", d->out);
        if (s->by_ref) fputs(" &", d->out);
        break;
    case CC_S_PARALLEL: case CC_S_PARALLEL_FOR:
        if (s->par_spawn) fputs(" spawn", d->out);
        if (s->par_worker) fprintf(d->out, " worker(%s)", s->par_worker);
        break;
    default: break;
    }
    cc__pos(d, s->span);
    if (s->type) cc__dump_type(d, s->type, depth + 1);
    if (s->decl) cc__dump_decl(d, s->decl, depth + 1);
    if (s->init_expr) cc__dump_expr(d, s->init_expr, depth + 1);
    if (s->expr) cc__dump_expr(d, s->expr, depth + 1);
    if (s->expr2 && !(s->exprs.n && s->exprs.items[0] == s->expr2)) cc__dump_expr(d, s->expr2, depth + 1);
    for (i = 0; i < s->exprs.n; i++) cc__dump_expr(d, s->exprs.items[i], depth + 1);
    if (s->par_pred) cc__dump_expr(d, s->par_pred, depth + 1);
    if (s->par_seq) cc__dump_expr(d, s->par_seq, depth + 1);
    if (s->par_wait) cc__dump_expr(d, s->par_wait, depth + 1);
    for (i = 0; i < s->par_cache.n; i++) cc__dump_expr(d, s->par_cache.items[i], depth + 1);
    if (s->par_dest) cc__dump_expr(d, s->par_dest, depth + 1);
    if (s->par_target) cc__dump_expr(d, s->par_target, depth + 1);
    if (s->closing_spawn) cc__dump_expr(d, s->closing_spawn, depth + 1);
    cc__dump_arms(d, s->arms, depth + 1);
    if (s->body) cc__dump_stmt(d, s->body, depth + 1);
    for (i = 0; i < s->stmts.n; i++) cc__dump_stmt(d, s->stmts.items[i], depth + 1);
    if (s->else_body) {
        cc__indent(d, depth + 1);
        fputs("else\n", d->out);
        cc__dump_stmt(d, s->else_body, depth + 2);
    }
    if (s->par_tail) {
        cc__indent(d, depth + 1);
        fputs("tail\n", d->out);
        cc__dump_expr(d, s->par_tail, depth + 2);
    }
}

static void cc__dump_decl(CcDump *d, const CcDecl *dc, int depth) {
    size_t i;
    const CcHookEntry *h;
    const CcVariantArm *va;
    if (!dc) return;
    cc__indent(d, depth);
    fprintf(d->out, "%s [%u..%u]", cc_decl_kind_name(dc->kind), dc->span.first, dc->span.last);
    if (dc->name) fprintf(d->out, " %s", dc->name);
    if (dc->kind == CC_D_PP) {
        fputc(' ', d->out);
        cc__tok_text(d, dc->tok);
        if (dc->pp_skipped_region) fputs(" (inactive region)", d->out);
    }
    if (dc->kind == CC_D_PRAGMA_CC) fprintf(d->out, " %s %s", dc->pragma_name ? dc->pragma_name : "", dc->pragma_value ? dc->pragma_value : "");
    if (dc->kind == CC_D_GRAMMAR) fprintf(d->out, " (%s) body %u+%u", dc->engine ? dc->engine : "", dc->body_off, dc->body_len);
    if (dc->kind == CC_D_GENERIC_FACTORY && dc->factory_extend) fputs(" extend", d->out);
    if (dc->specs & CC_S_STATIC) fputs(" static", d->out);
    if (dc->specs & CC_S_EXTERN) fputs(" extern", d->out);
    if (dc->specs & CC_S_TYPEDEF) fputs(" typedef", d->out);
    if (dc->specs & CC_S_THREAD) fputs(" _Thread_local", d->out);
    if (dc->specs & CC_F_INLINE) fputs(" inline", d->out);
    if (dc->specs & CC_F_NORETURN) fputs(" _Noreturn", d->out);
    if (dc->specs & CC_F_ASYNC) fputs(" @async", d->out);
    if (dc->specs & CC_F_BLOCKING) fputs(" @blocking", d->out);
    if (dc->specs & CC_F_NONBLOCKING) fputs(" @nonblocking", d->out);
    if (dc->specs & CC_F_LATENCY_SENSITIVE) fputs(" @latency_sensitive", d->out);
    if (dc->specs & CC_F_COMPTIME) fputs(" @comptime", d->out);
    if (dc->destroy) fputs(" @destroy", d->out);
    if (dc->detach) fputs(" @detach", d->out);
    cc__pos(d, dc->span);
    cc__dump_attrs(d, dc->attrs, depth + 1);
    if (dc->type) cc__dump_type(d, dc->type, depth + 1);
    if (dc->init) cc__dump_init(d, dc->init, depth + 1);
    if (dc->expr) cc__dump_expr(d, dc->expr, depth + 1);
    if (dc->factory_arity) cc__dump_expr(d, dc->factory_arity, depth + 1);
    if (dc->assert_expr) cc__dump_expr(d, dc->assert_expr, depth + 1);
    if (dc->cond) cc__dump_expr(d, dc->cond, depth + 1);
    for (h = dc->entries; h; h = h->next) {
        const CcViewItem *vi;
        cc__indent(d, depth + 1);
        fprintf(d->out, "entry [%u..%u] %s", h->span.first, h->span.last, h->field ? h->field : "");
        for (vi = h->items; vi; vi = vi->next)
            fprintf(d->out, " %s%s", vi->deny ? "^" : "", vi->name ? vi->name : "");
        cc__pos(d, h->span);
        for (vi = h->items; vi; vi = vi->next)
            if (vi->cast) cc__dump_type(d, vi->cast, depth + 2);
        if (h->value) cc__dump_expr(d, h->value, depth + 2);
        if (h->body) cc__dump_stmt(d, h->body, depth + 2);
    }
    for (va = dc->arms; va; va = va->next) {
        cc__indent(d, depth + 1);
        fprintf(d->out, "arm [%u..%u] %s", va->span.first, va->span.last, va->name ? va->name : "");
        cc__pos(d, va->span);
        cc__dump_params(d, &va->payload, depth + 2);
    }
    if (dc->body) cc__dump_stmt(d, dc->body, depth + 1);
    if (dc->destroy_body) {
        cc__indent(d, depth + 1);
        fputs("@destroy body\n", d->out);
        cc__dump_stmt(d, dc->destroy_body, depth + 2);
    }
    for (i = 0; i < dc->then_decls.n; i++) cc__dump_decl(d, dc->then_decls.items[i], depth + 1);
    if (dc->else_decls.n) {
        cc__indent(d, depth + 1);
        fputs("else\n", d->out);
        for (i = 0; i < dc->else_decls.n; i++) cc__dump_decl(d, dc->else_decls.items[i], depth + 2);
    }
}

void cc_ast_dump(const CcUnit *u, void *stream) {
    CcDump d;
    size_t i;
    d.out = (FILE *)stream;
    d.f = u->file;
    fprintf(d.out, "unit %s (%zu decls%s)\n", u->file && u->file->path ? u->file->path : "<memory>",
            u->decls.n, u->has_shebang ? ", #!" : "");
    for (i = 0; i < u->decls.n; i++) cc__dump_decl(&d, u->decls.items[i], 1);
}

/* ---- walk ------------------------------------------------------------ */

static void cc__walk_type(CcVisitor *v, CcType *t);
static void cc__walk_expr(CcVisitor *v, CcExpr *e);
static void cc__walk_init(CcVisitor *v, CcInit *in);
static void cc__walk_stmt(CcVisitor *v, CcStmt *s);
static void cc__walk_decl(CcVisitor *v, CcDecl *d);

static void cc__walk_params(CcVisitor *v, CcParamList *pl) {
    size_t i;
    for (i = 0; i < pl->n; i++) {
        CcParam *p = pl->items[i];
        if (p->type) cc__walk_type(v, p->type);
        if (p->default_value) cc__walk_expr(v, p->default_value);
    }
}

static void cc__walk_type(CcVisitor *v, CcType *t) {
    size_t i;
    CcField *fl;
    CcEnumerator *en;
    if (!t) return;
    if (v->on_type && v->on_type(v, t)) return;
    if (t->base) cc__walk_type(v, t->base);
    if (t->size) cc__walk_expr(v, t->size);
    if (t->kind == CC_T_FUNC) cc__walk_params(v, &t->params);
    for (fl = t->fields; fl; fl = fl->next) {
        if (fl->type) cc__walk_type(v, fl->type);
        if (fl->bit_width) cc__walk_expr(v, fl->bit_width);
    }
    for (en = t->enumerators; en; en = en->next) {
        if (en->value) cc__walk_expr(v, en->value);
        if (en->comptime) cc__walk_stmt(v, en->comptime);
    }
    if (t->typeof_expr) cc__walk_expr(v, t->typeof_expr);
    if (t->typeof_type) cc__walk_type(v, t->typeof_type);
    if (t->err) cc__walk_type(v, t->err);
    if (t->fixed) cc__walk_expr(v, t->fixed);
    if (t->cap) cc__walk_expr(v, t->cap);
    if (t->chan_hooks) cc__walk_init(v, t->chan_hooks);
    if (t->value) cc__walk_expr(v, t->value);
    for (i = 0; i < t->args.n; i++) cc__walk_type(v, t->args.items[i]);
}

static void cc__walk_init(CcVisitor *v, CcInit *in) {
    CcDesignator *ds;
    size_t i;
    if (!in) return;
    for (ds = in->designators; ds; ds = ds->next) {
        if (ds->index) cc__walk_expr(v, ds->index);
        if (ds->index_hi) cc__walk_expr(v, ds->index_hi);
    }
    if (in->expr) cc__walk_expr(v, in->expr);
    for (i = 0; i < in->list.n; i++) cc__walk_init(v, in->list.items[i]);
}

static void cc__walk_expr(CcVisitor *v, CcExpr *e) {
    size_t i;
    CcGenericSelArm *g;
    CcTplPart *pt;
    if (!e) return;
    if (v->on_expr && v->on_expr(v, e)) return;
    if (e->type) cc__walk_type(v, e->type);
    if (e->a) cc__walk_expr(v, e->a);
    if (e->b) cc__walk_expr(v, e->b);
    if (e->c) cc__walk_expr(v, e->c);
    for (i = 0; i < e->targs.n; i++) cc__walk_type(v, e->targs.items[i]);
    for (i = 0; i < e->args.n; i++) cc__walk_expr(v, e->args.items[i]);
    if (e->init) cc__walk_init(v, e->init);
    for (g = e->arms; g; g = g->next) {
        if (g->type) cc__walk_type(v, g->type);
        if (g->expr) cc__walk_expr(v, g->expr);
    }
    if (e->tpl_policy) cc__walk_expr(v, e->tpl_policy);
    for (pt = e->tpl_parts; pt; pt = pt->next)
        if (pt->expr) cc__walk_expr(v, pt->expr);
    if (e->tpl_arena) cc__walk_expr(v, e->tpl_arena);
    if (e->scratch_bytes) cc__walk_expr(v, e->scratch_bytes);
    if (e->kind == CC_E_CLOSURE) {
        CcCapture *c;
        cc__walk_params(v, &e->params);
        for (c = e->captures; c; c = c->next)
            if (c->init) cc__walk_expr(v, c->init);
    }
    if (e->body) cc__walk_stmt(v, e->body);
}

static void cc__walk_stmt(CcVisitor *v, CcStmt *s) {
    size_t i;
    CcParallelArm *arm;
    if (!s) return;
    if (v->on_stmt && v->on_stmt(v, s)) return;
    if (s->type) cc__walk_type(v, s->type);
    if (s->decl) cc__walk_decl(v, s->decl);
    if (s->init_expr) cc__walk_expr(v, s->init_expr);
    if (s->expr) cc__walk_expr(v, s->expr);
    if (s->expr2 && !(s->exprs.n && s->exprs.items[0] == s->expr2)) cc__walk_expr(v, s->expr2);
    for (i = 0; i < s->exprs.n; i++) cc__walk_expr(v, s->exprs.items[i]);
    if (s->par_pred) cc__walk_expr(v, s->par_pred);
    if (s->par_seq) cc__walk_expr(v, s->par_seq);
    if (s->par_wait) cc__walk_expr(v, s->par_wait);
    for (i = 0; i < s->par_cache.n; i++) cc__walk_expr(v, s->par_cache.items[i]);
    if (s->par_dest) cc__walk_expr(v, s->par_dest);
    if (s->par_target) cc__walk_expr(v, s->par_target);
    if (s->closing_spawn) cc__walk_expr(v, s->closing_spawn);
    for (arm = s->arms; arm; arm = arm->next) {
        if (arm->expr) cc__walk_expr(v, arm->expr);
        if (arm->serial) cc__walk_stmt(v, arm->serial);
    }
    if (s->body) cc__walk_stmt(v, s->body);
    for (i = 0; i < s->stmts.n; i++) cc__walk_stmt(v, s->stmts.items[i]);
    if (s->else_body) cc__walk_stmt(v, s->else_body);
    if (s->par_tail) cc__walk_expr(v, s->par_tail);
}

static void cc__walk_decl(CcVisitor *v, CcDecl *d) {
    size_t i;
    CcHookEntry *h;
    CcVariantArm *va;
    if (!d) return;
    if (v->on_decl && v->on_decl(v, d)) return;
    if (d->type) cc__walk_type(v, d->type);
    if (d->init) cc__walk_init(v, d->init);
    if (d->expr) cc__walk_expr(v, d->expr);
    if (d->factory_arity) cc__walk_expr(v, d->factory_arity);
    if (d->assert_expr) cc__walk_expr(v, d->assert_expr);
    if (d->cond) cc__walk_expr(v, d->cond);
    for (h = d->entries; h; h = h->next) {
        CcViewItem *vi;
        for (vi = h->items; vi; vi = vi->next)
            if (vi->cast) cc__walk_type(v, vi->cast);
        if (h->value) cc__walk_expr(v, h->value);
        if (h->body) cc__walk_stmt(v, h->body);
    }
    for (va = d->arms; va; va = va->next) cc__walk_params(v, &va->payload);
    if (d->body) cc__walk_stmt(v, d->body);
    if (d->destroy_body) cc__walk_stmt(v, d->destroy_body);
    for (i = 0; i < d->then_decls.n; i++) cc__walk_decl(v, d->then_decls.items[i]);
    for (i = 0; i < d->else_decls.n; i++) cc__walk_decl(v, d->else_decls.items[i]);
}

void cc_ast_walk(CcUnit *u, CcVisitor *v) {
    size_t i;
    for (i = 0; i < u->decls.n; i++) cc__walk_decl(v, u->decls.items[i]);
}
