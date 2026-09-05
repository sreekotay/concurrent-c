/* Printer: CcUnit → C text and a source map. See print.h.
 *
 * Identity mode replays every token's leading trivia and text from the
 * lexed file in top-level declaration order, so the output equals the
 * input when the declaration spans tile the token array. Every token the
 * tree does not reach is a diagnostic, as is a gap or an overlap between
 * consecutive top-level spans.
 *
 * Lowered mode prints C from the node structure. Text reaches the buffer
 * through three primitives so line accounting stays exact: synthesized
 * text (pr_str), raw source bytes (pr_raw: token text, comments, and the
 * verbatim spans of constructs that lowering does not handle yet), and
 * newlines (pr_nl). The printer keeps two logical positions. The `pin` is
 * the user's (file, line) that the text being emitted came from; every
 * statement, declaration, field and comment sets it from its first byte.
 * The `host` is the (file, line) the host compiler will attribute the
 * next physical line to, given the `#line` directives emitted so far.
 * When a physical line starts and the two differ, a `#line` re-pins the
 * host. Raw bytes copied from the source move the pin along with the
 * source line at every newline they contain, so a multi-line verbatim
 * span or comment needs no directive inside it (none could be placed
 * inside a token or a comment anyway): a directive is placed only before
 * the first byte of a raw chunk, and chunks are whole tokens or whole
 * trivia pieces. The source map gets one entry per emitted physical line,
 * directive lines included. */
#include "print.h"
#include "parse.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PR_INDENT "    "
#define PR_MAX_UNCOVERED_REPORTS 25

typedef struct Pr {
    CcBuf *out;
    const CcLexFile *f;
    CcArena *arena;
    CcDiag *d;
    CcPrintOpts opts;         /* copy with `path` defaulted */
    CcSrcMap *map;
    uint32_t map_cap;
    uint32_t emit_line;       /* 1-based physical line being filled */
    size_t line_start;        /* out->len at the start of emit_line */
    int at_line_start;        /* nothing emitted on emit_line yet */
    /* pin: where the text being emitted came from */
    const char *pin_path;
    uint32_t pin_line;
    /* host: what the host compiler will attribute the next line to */
    const char *host_path;
    uint32_t host_line;
    int host_known;
    int host_reset_pending;   /* a user `#line` was copied: apply after its newline */
    const char *host_reset_path;
    uint32_t host_reset_line;
    uint32_t col_line;        /* emit_line whose col_delta has been recorded */
    int indent;               /* indentation level of the construct being printed */
    uint32_t trailing_tok;    /* index + 1 of the token whose leading trivia starts with a
                                 comment already printed as a trailing comment; 0 = none */
    uint32_t trailing_len;    /* bytes of that trivia already printed */
    uint32_t last_lead_tok;   /* index + 1 of the token whose leading trivia was last replayed */
    int n_span_reports;
} Pr;

/* ---- diagnostics ------------------------------------------------------ */

static void pr_diag_off(Pr *p, uint32_t off, uint32_t len, const char *fmt, ...) {
    CcBuf b;
    va_list ap;
    CcLoc loc;
    if (!p->d) return;
    cc_buf_init(&b);
    va_start(ap, fmt);
    cc_buf_vprintf(&b, fmt, ap);
    va_end(ap);
    if (off > p->f->len) off = p->f->len;
    loc = cc_lex_loc(p->f, off);
    cc_diag_emit_at(p->d, CC_SEV_ERROR, loc, p->f->src, p->f->len, off, len, "%s", b.data);
    cc_buf_free(&b);
}

static int pr_span_ok(Pr *p, CcSpan s, const char *what) {
    uint32_t n = p->f->n_toks;
    if (s.first <= s.last && s.last + 1 < n) return 1;
    if (p->n_span_reports++ < PR_MAX_UNCOVERED_REPORTS) {
        uint32_t off = s.first < n ? p->f->toks[s.first].off : p->f->len;
        pr_diag_off(p, off, 0, "%s node has an invalid token span [%u..%u] (file has %u tokens)",
                    what, (unsigned)s.first, (unsigned)s.last, (unsigned)n);
    }
    return 0;
}

/* ---- positions ---------------------------------------------------------- */

static const char *pr_path_of(const Pr *p, const CcLoc *loc) {
    if (!loc->path || (p->f->path && strcmp(loc->path, p->f->path) == 0)) return p->opts.path;
    return loc->path;
}

static void pr_set_pin(Pr *p, uint32_t off) {
    CcLoc loc;
    if (off > p->f->len) off = p->f->len;
    loc = cc_lex_loc(p->f, off);
    p->pin_path = pr_path_of(p, &loc);
    p->pin_line = loc.line;
}

static void pr_set_pin_tok(Pr *p, uint32_t tok) {
    if (tok < p->f->n_toks) pr_set_pin(p, p->f->toks[tok].off);
}

/* ---- emission primitives ------------------------------------------------ */

static void pr_map_push(Pr *p, const char *path, uint32_t line) {
    CcSrcMapEntry *e;
    if (!p->map) return;
    if (p->map->n == p->map_cap) {
        uint32_t ncap = p->map_cap ? p->map_cap * 2 : 128;
        CcSrcMapEntry *ne = CC_NEW_N(p->arena, CcSrcMapEntry, ncap);
        if (p->map->n) memcpy(ne, p->map->entries, p->map->n * sizeof *ne);
        p->map->entries = ne;
        p->map_cap = ncap;
    }
    e = &p->map->entries[p->map->n++];
    e->emit_line = p->emit_line;
    e->path = path;
    e->line = line;
    e->col_delta = 0;
}

static void pr_push_quoted_path(CcBuf *out, const char *path) {
    const char *s = path ? path : "<input>";
    for (; *s; s++) {
        if (*s == '"' || *s == '\\') cc_buf_push_char(out, '\\');
        cc_buf_push_char(out, *s);
    }
}

/* Emit `#line` for the pin; the current line is empty. */
static void pr_directive(Pr *p) {
    cc_buf_printf(p->out, "#line %u \"", (unsigned)p->pin_line);
    pr_push_quoted_path(p->out, p->pin_path);
    cc_buf_push_str(p->out, "\"\n");
    pr_map_push(p, p->pin_path, p->pin_line);
    p->emit_line++;
    p->line_start = p->out->len;
    p->host_path = p->pin_path;
    p->host_line = p->pin_line;
    p->host_known = 1;
}

static int pr_same_path(const char *a, const char *b) {
    if (a == b) return 1;
    if (!a || !b) return 0;
    return strcmp(a, b) == 0;
}

/* Called before the first byte of content on a physical line. */
static void pr_line_begin(Pr *p, int allow_directive) {
    if (!p->at_line_start) return;
    if (allow_directive && p->opts.line_directives && p->pin_line &&
        (!p->host_known || p->host_line != p->pin_line || !pr_same_path(p->host_path, p->pin_path)))
        pr_directive(p);
    pr_map_push(p, p->pin_path, p->pin_line);
    p->at_line_start = 0;
}

static void pr_nl(Pr *p) {
    if (p->at_line_start) {
        /* an empty line: still attributed, so re-pin it when the host drifted
         * (a multi-line source statement printed on fewer lines) */
        if (p->opts.line_directives && p->pin_line &&
            (!p->host_known || p->host_line != p->pin_line || !pr_same_path(p->host_path, p->pin_path)))
            pr_directive(p);
        pr_map_push(p, p->pin_path, p->pin_line);
    }
    cc_buf_push_char(p->out, '\n');
    p->emit_line++;
    p->line_start = p->out->len;
    p->at_line_start = 1;
    if (p->host_reset_pending) {
        p->host_reset_pending = 0;
        p->host_known = 1;
        p->host_path = p->host_reset_path;
        p->host_line = p->host_reset_line;
    } else if (p->host_known) {
        p->host_line++;
    }
}

/* Synthesized text without newlines. */
static void pr_str(Pr *p, const char *s) {
    if (!*s) return;
    pr_line_begin(p, 1);
    cc_buf_push_str(p->out, s);
}

static void pr_indent(Pr *p, int n) {
    int i;
    pr_line_begin(p, 1);
    for (i = 0; i < n; i++) cc_buf_push_str(p->out, PR_INDENT);
}

/* Copy `len` bytes of the source starting at `off`. Only the first byte may
 * trigger a `#line`; later line starts inside the chunk move the pin to the
 * source line they continue. */
static void pr_raw(Pr *p, uint32_t off, uint32_t len) {
    const char *s;
    uint32_t i = 0;
    if (!len) return;
    if (off > p->f->len || len > p->f->len - off) {
        pr_diag_off(p, 0, 0, "internal: raw range [%u, +%u) is outside the file", (unsigned)off, (unsigned)len);
        return;
    }
    s = p->f->src + off;
    while (i < len) {
        uint32_t j = i;
        while (j < len && s[j] != '\n') j++;
        if (j > i) {
            pr_line_begin(p, i == 0);
            cc_buf_push(p->out, s + i, j - i);
        }
        if (j == len) break;
        pr_nl(p);
        if (off + j + 1 < p->f->len) pr_set_pin(p, off + j + 1);
        i = j + 1;
    }
}

/* Find the lexer's line mark that follows a `#line` token, if any. */
static const CcLineMark *pr_mark_after(const Pr *p, const CcToken *t) {
    uint32_t end = t->off + t->len, i;
    for (i = 0; i < p->f->n_marks; i++) {
        uint32_t m = p->f->marks[i].off;
        if (m >= end && m <= end + 2) return &p->f->marks[i];
        if (m > end + 2) break;
    }
    return NULL;
}

/* The text of one token (no trivia). A user `#line` copied verbatim rebases
 * the host as well, so the host stays in step with cc_lex_loc. */
static void pr_tok_text(Pr *p, uint32_t tok) {
    const CcToken *t;
    if (tok >= p->f->n_toks) {
        pr_diag_off(p, p->f->len, 0, "internal: token index %u out of range", (unsigned)tok);
        return;
    }
    t = &p->f->toks[tok];
    if (t->kind == CC_TK_PP) {
        const CcLineMark *m = pr_mark_after(p, t);
        if (m) {
            CcLoc loc;
            loc.path = m->path;
            loc.line = m->logical_line;
            loc.col = 0;
            p->host_reset_pending = 1;
            p->host_reset_path = pr_path_of(p, &loc);
            p->host_reset_line = m->logical_line;
        }
    }
    pr_raw(p, t->off, t->len);
}

static uint32_t trivia_comment_end(const char *src, uint32_t i, uint32_t end);

/* Leading trivia verbatim, as pieces: each comment, each newline and each
 * whitespace run is its own chunk, so a `#line` can be placed after a
 * newline between tokens (never inside a comment) when the host drifted. */
static void pr_trivia_raw(Pr *p, uint32_t off, uint32_t len) {
    const char *src = p->f->src;
    uint32_t i = off, end = off + len;
    if (off > p->f->len || len > p->f->len - off) {
        pr_diag_off(p, 0, 0, "internal: trivia range [%u, +%u) is outside the file", (unsigned)off, (unsigned)len);
        return;
    }
    while (i < end) {
        uint32_t j = trivia_comment_end(src, i, end);
        if (j > i) { pr_raw(p, i, j - i); i = j; continue; }
        if (src[i] == '\n') {
            pr_nl(p);
            if (i + 1 < p->f->len) pr_set_pin(p, i + 1);
            i++;
            continue;
        }
        j = i;
        while (j < end && src[j] != '\n' && !(src[j] == '/' && j + 1 < end && (src[j + 1] == '/' || src[j + 1] == '*'))) j++;
        pr_raw(p, i, j - i);
        i = j;
    }
}

/* Tokens first..last verbatim: the first without its leading trivia. */
static void pr_span(Pr *p, CcSpan s, const char *what) {
    uint32_t i;
    if (!pr_span_ok(p, s, what)) return;
    for (i = s.first; i <= s.last; i++) {
        const CcToken *t = &p->f->toks[i];
        if (i > s.first) pr_trivia_raw(p, t->lead_off, t->lead_len);
        pr_tok_text(p, i);
    }
}

/* ---- trivia replay -------------------------------------------------------- */

/* If a comment starts at src[i], its end (exclusive); else i. */
static uint32_t trivia_comment_end(const char *src, uint32_t i, uint32_t end) {
    if (i + 1 < end && src[i] == '/' && src[i + 1] == '/') {
        i += 2;
        while (i < end && src[i] != '\n') i++;
        return i;
    }
    if (i + 1 < end && src[i] == '/' && src[i + 1] == '*') {
        i += 2;
        while (i + 1 < end && !(src[i] == '*' && src[i + 1] == '/')) i++;
        return i + 1 < end ? i + 2 : end;
    }
    return i;
}

/* Replay the comments and blank lines of a token's leading trivia at
 * `indent`. Runs of newlines collapse to at most one blank line; the
 * newline the printer already emitted after the previous item counts as
 * the first of the run. */
static void pr_lead(Pr *p, uint32_t tok, int indent) {
    const CcToken *t;
    const char *src = p->f->src;
    uint32_t off, end, i;
    int run = 0, emitted;
    if (tok >= p->f->n_toks) return;
    if (p->last_lead_tok == tok + 1) return; /* a split declaration shares this token */
    p->last_lead_tok = tok + 1;
    t = &p->f->toks[tok];
    off = t->lead_off;
    end = off + t->lead_len;
    i = off;
    if (p->trailing_tok == tok + 1) i = off + p->trailing_len;
    emitted = p->at_line_start ? 1 : 0;
    while (i < end) {
        char c = src[i];
        uint32_t j;
        if (c == '\n') {
            run++;
            if ((run < 2 ? run : 2) > emitted) {
                pr_set_pin(p, i); /* the blank line is the one this newline ends */
                pr_nl(p);
                emitted++;
            }
            i++;
            continue;
        }
        j = trivia_comment_end(src, i, end);
        if (j > i) {
            if (p->at_line_start) {
                pr_set_pin(p, i);
                pr_indent(p, indent);
            } else {
                pr_str(p, " ");
            }
            pr_raw(p, i, j - i);
            run = 0;
            emitted = 0;
            i = j;
            continue;
        }
        i++; /* whitespace */
    }
}

/* Print the comments that follow the item just printed on its own line
 * (the start of the next token's leading trivia), so `a; // note` keeps
 * its note on the line. */
static void pr_trailing(Pr *p, uint32_t tok) {
    const CcToken *t;
    const char *src = p->f->src;
    uint32_t off, end, i, consumed = 0;
    if (tok >= p->f->n_toks) return;
    t = &p->f->toks[tok];
    off = t->lead_off;
    end = off + t->lead_len;
    i = off;
    while (i < end) {
        char c = src[i];
        uint32_t j;
        if (c == ' ' || c == '\t' || c == '\r') { i++; continue; }
        j = trivia_comment_end(src, i, end);
        if (j == i) break;
        if (memchr(src + i, '\n', j - i)) break; /* spans lines: leave it for the lead replay */
        pr_str(p, " ");
        pr_raw(p, i, j - i);
        i = j;
        consumed = i - off;
    }
    if (consumed) {
        p->trailing_tok = tok + 1;
        p->trailing_len = consumed;
    }
}

/* Position for a statement or declaration: replay its leading trivia, pin
 * its line, indent (or separate from a same-line comment), and record the
 * column delta for the source map. */
static void pr_item_start(Pr *p, CcSpan span, int indent) {
    pr_lead(p, span.first, indent);
    pr_set_pin_tok(p, span.first);
    if (p->at_line_start) pr_indent(p, indent);
    else pr_str(p, " ");
    pr_line_begin(p, 1);
    if (p->map && p->col_line != p->emit_line && p->map->n &&
        p->map->entries[p->map->n - 1].emit_line == p->emit_line &&
        span.first < p->f->n_toks && span.last < p->f->n_toks) {
        const CcToken *t0 = &p->f->toks[span.first], *t1 = &p->f->toks[span.last];
        p->col_line = p->emit_line;
        if (t0->line == t1->line) {
            CcLoc loc = cc_lex_loc(p->f, t0->off);
            int32_t emitted_col = (int32_t)(p->out->len - p->line_start) + 1;
            p->map->entries[p->map->n - 1].col_delta = emitted_col - (int32_t)loc.col;
        }
    }
}

/* ---- CC-ness ---------------------------------------------------------------- */

#define CC_F_CC_MASK (CC_F_ASYNC | CC_F_BLOCKING | CC_F_NONBLOCKING | CC_F_LATENCY_SENSITIVE | CC_F_COMPTIME)

static int type_is_cc(const CcType *t) { return t->kind >= CC_T_RESULT; }
static int expr_is_cc(const CcExpr *e) {
    return e->kind >= CC_E_UFCS && e->kind != CC_E_PP && e->kind != CC_E_TYPE_ARG;
}
static int stmt_is_cc(const CcStmt *s) {
    return s->kind >= CC_S_DEFER || (s->kind == CC_S_CASE && s->case_arm) ||
           (s->kind == CC_S_SWITCH && s->is_variant_switch);
}
static int decl_is_cc(const CcDecl *d) {
    return (d->kind >= CC_D_TYPEHOOKS && d->kind != CC_D_MACRO_CALL && d->kind != CC_D_STMT) ||
           d->destroy || d->detach ||
           (d->specs & CC_F_CC_MASK) != 0;
}

/* ---- forward declarations ---------------------------------------------------- */

static void pr_expr_p(Pr *p, const CcExpr *e, int min_prec);
static void pt_type(Pr *p, const CcType *t, const char *name);
static void pr_stmt_line(Pr *p, const CcStmt *s);
static void pr_stmt_text(Pr *p, const CcStmt *s);
static void pr_block(Pr *p, const CcStmt *s);
static void pr_items(Pr *p, const CcStmtList *l);
static void pr_decl(Pr *p, const CcDecl *d);
static void pr_init(Pr *p, const CcInit *in);

/* A preprocessor line that sits inside a construct printed on one line
 * (an argument list, an initializer, a member list): on a line of its own,
 * then the construct resumes pinned to the token after it. */
static void pr_trivia_raw(Pr *p, uint32_t off, uint32_t len);

static void pr_pp_inside(Pr *p, uint32_t tok, CcSpan span) {
    uint32_t last = tok;
    if (!p->at_line_start) pr_nl(p);
    pr_set_pin_tok(p, tok);
    pr_tok_text(p, tok);
    /* an inactive `#if` region is kept as the directive's span: verbatim */
    if (span.first <= tok && span.last > tok && span.last + 1 < p->f->n_toks) {
        uint32_t i;
        for (i = tok + 1; i <= span.last; i++) {
            const CcToken *t = &p->f->toks[i];
            pr_trivia_raw(p, t->lead_off, t->lead_len);
            pr_tok_text(p, i);
        }
        last = span.last;
    }
    pr_nl(p);
    if (last + 1 < p->f->n_toks) pr_set_pin_tok(p, last + 1);
}

/* Whether a `,` follows the span in the source; lists that carry
 * preprocessor lines keep the user's comma placement. */
static int src_comma_after(const Pr *p, CcSpan s) {
    uint32_t i = s.last + 1;
    return i < p->f->n_toks && cc_tok_is_punct(&p->f->toks[i], CC_P_COMMA);
}

/* ---- attributes, specifiers ------------------------------------------------ */

static void pr_attrs(Pr *p, const CcAttr *a, int leading_space) {
    for (; a; a = a->next) {
        if (leading_space) pr_str(p, " ");
        pr_span(p, a->span, "attribute");
        if (!leading_space) pr_str(p, " ");
    }
}

static void pr_quals(Pr *p, uint32_t q, int trailing_space) {
    static const struct { uint32_t bit; const char *word; } tab[] = {
        { CC_Q_CONST, "const" }, { CC_Q_VOLATILE, "volatile" },
        { CC_Q_RESTRICT, "restrict" }, { CC_Q_ATOMIC, "_Atomic" },
    };
    size_t i;
    int first = 1;
    for (i = 0; i < sizeof tab / sizeof tab[0]; i++) {
        if (!(q & tab[i].bit)) continue;
        if (!first) pr_str(p, " ");
        pr_str(p, tab[i].word);
        first = 0;
    }
    if (!first && trailing_space) pr_str(p, " ");
}

static void pr_specs(Pr *p, uint32_t specs) {
    static const struct { uint32_t bit; const char *word; } tab[] = {
        { CC_S_TYPEDEF, "typedef" }, { CC_S_EXTERN, "extern" }, { CC_S_STATIC, "static" },
        { CC_S_THREAD, "_Thread_local" }, { CC_S_AUTO, "auto" }, { CC_S_REGISTER, "register" },
        { CC_F_INLINE, "inline" }, { CC_F_NORETURN, "_Noreturn" },
    };
    size_t i;
    for (i = 0; i < sizeof tab / sizeof tab[0]; i++)
        if (specs & tab[i].bit) { pr_str(p, tab[i].word); pr_str(p, " "); }
    pr_quals(p, specs & (CC_Q_CONST | CC_Q_VOLATILE | CC_Q_RESTRICT | CC_Q_ATOMIC), 1);
}

/* ---- types -------------------------------------------------------------------- */

static int type_is_derived(const CcType *t) {
    return t->kind == CC_T_POINTER || t->kind == CC_T_ARRAY || t->kind == CC_T_FUNC;
}

static const CcType *type_base(const CcType *t) {
    while (t && type_is_derived(t) && t->base) t = t->base;
    return t;
}

static void pt_params(Pr *p, const CcType *t) {
    size_t i;
    pr_str(p, "(");
    if (t->params.n == 0) {
        pr_str(p, t->has_prototype ? "void" : "");
    }
    for (i = 0; i < t->params.n; i++) {
        const CcParam *pa = t->params.items[i];
        if (i) pr_str(p, ", ");
        if (!pa) continue;
        if (pa->is_variadic || !pa->type) { pr_str(p, "..."); continue; }
        if (pa->default_value) { pr_span(p, pa->span, "parameter"); continue; }
        pt_type(p, pa->type, pa->name);
        pr_attrs(p, pa->attrs, 1);
    }
    pr_str(p, ")");
}

/* The part of a declarator before the name (pointers, opening parens). */
static void pt_prefix(Pr *p, const CcType *t) {
    if (!t || !type_is_derived(t) || !t->base) return;
    pt_prefix(p, t->base);
    if (t->kind == CC_T_POINTER) {
        if (type_is_derived(t->base)) {
            if (t->base->kind != CC_T_POINTER) pr_str(p, "(");
        }
        pr_str(p, "*");
        pr_quals(p, t->quals, 1);
    }
}

/* The part after the name (array sizes, parameter lists, closing parens). */
static void pt_suffix(Pr *p, const CcType *t) {
    if (!t || !type_is_derived(t) || !t->base) return;
    switch (t->kind) {
    case CC_T_POINTER:
        if (type_is_derived(t->base) && t->base->kind != CC_T_POINTER) pr_str(p, ")");
        break;
    case CC_T_ARRAY:
        pr_str(p, "[");
        if (t->array_static) pr_str(p, "static ");
        pr_quals(p, t->quals, 1);
        if (t->array_star) pr_str(p, "*");
        else if (t->size) pr_expr_p(p, t->size, 1);
        pr_str(p, "]");
        break;
    case CC_T_FUNC:
        pt_params(p, t);
        break;
    default:
        break;
    }
    pt_suffix(p, t->base);
}

static void pt_struct_body(Pr *p, const CcType *t) {
    const CcField *f;
    int outer = p->indent;
    uint32_t rb = t->span.last;
    pr_str(p, " {");
    pr_nl(p);
    p->indent = outer + 1;
    for (f = t->fields; f; f = f->next) {
        if (f->is_pp) { pr_pp_inside(p, f->pp_tok, f->span); continue; }
        pr_item_start(p, f->span, p->indent);
        if (f->type) pt_type(p, f->type, f->name);
        else if (f->name) pr_str(p, f->name);
        if (f->bit_width) {
            pr_str(p, " : ");
            pr_expr_p(p, f->bit_width, 2);
        }
        pr_attrs(p, f->attrs, 1);
        pr_str(p, ";");
        pr_trailing(p, f->span.last + 1);
        pr_nl(p);
    }
    p->indent = outer;
    if (rb < p->f->n_toks && cc_tok_is_punct(&p->f->toks[rb], CC_P_RBRACE)) {
        pr_lead(p, rb, outer + 1);
        pr_set_pin_tok(p, rb);
    }
    if (p->at_line_start) pr_indent(p, outer);
    else pr_str(p, " ");
    pr_str(p, "}");
}

static void pt_enum_body(Pr *p, const CcType *t) {
    const CcEnumerator *e;
    int outer = p->indent, has_pp = 0;
    uint32_t rb = t->span.last;
    pr_str(p, " {");
    pr_nl(p);
    p->indent = outer + 1;
    for (e = t->enumerators; e; e = e->next) if (e->is_pp) has_pp = 1;
    for (e = t->enumerators; e; e = e->next) {
        if (e->is_pp) { pr_pp_inside(p, e->pp_tok, e->span); continue; }
        pr_item_start(p, e->span, p->indent);
        pr_str(p, e->name ? e->name : "");
        if (e->value) {
            pr_str(p, " = ");
            pr_expr_p(p, e->value, 2);
        }
        if (has_pp ? src_comma_after(p, e->span) : e->next != NULL) pr_str(p, ",");
        pr_trailing(p, e->span.last + 1);
        pr_nl(p);
    }
    p->indent = outer;
    if (rb < p->f->n_toks && cc_tok_is_punct(&p->f->toks[rb], CC_P_RBRACE)) {
        pr_lead(p, rb, outer + 1);
        pr_set_pin_tok(p, rb);
    }
    if (p->at_line_start) pr_indent(p, outer);
    else pr_str(p, " ");
    pr_str(p, "}");
}

/* The specifier part: qualifiers and the base type. */
static void pt_base(Pr *p, const CcType *t) {
    if (!t) { pr_str(p, "int"); return; }
    if (type_is_cc(t)) { pr_span(p, t->span, cc_type_kind_name(t->kind)); return; }
    pr_quals(p, t->quals, 1);
    switch (t->kind) {
    case CC_T_NAMED:
        pr_str(p, t->name ? t->name : "int");
        pr_attrs(p, t->attrs, 1);
        break;
    case CC_T_STRUCT:
        pr_str(p, t->is_union ? "union" : "struct");
        pr_attrs(p, t->attrs, 1);
        if (t->name) { pr_str(p, " "); pr_str(p, t->name); }
        if (t->is_definition) pt_struct_body(p, t);
        break;
    case CC_T_ENUM:
        pr_str(p, "enum");
        pr_attrs(p, t->attrs, 1);
        if (t->name) { pr_str(p, " "); pr_str(p, t->name); }
        if (t->is_definition) pt_enum_body(p, t);
        break;
    case CC_T_TYPEOF:
        pr_str(p, "__typeof__(");
        if (t->typeof_expr) pr_expr_p(p, t->typeof_expr, 0);
        else if (t->typeof_type) pt_type(p, t->typeof_type, NULL);
        pr_str(p, ")");
        break;
    case CC_T_ATOMIC:
        pr_str(p, "_Atomic(");
        pt_type(p, t->base, NULL);
        pr_str(p, ")");
        break;
    case CC_T_POINTER:
    case CC_T_ARRAY:
    case CC_T_FUNC:
        /* a derived type without a base: nothing to name */
        pr_diag_off(p, t->span.first < p->f->n_toks ? p->f->toks[t->span.first].off : 0, 0,
                    "%s type has no base type", cc_type_kind_name(t->kind));
        pr_span(p, t->span, cc_type_kind_name(t->kind));
        break;
    default:
        pr_diag_off(p, t->span.first < p->f->n_toks ? p->f->toks[t->span.first].off : 0, 0,
                    "printer: unknown type kind %d", (int)t->kind);
        pr_span(p, t->span, "type");
        break;
    }
}

/* Type with a declarator: `int (*name)[3]`, or abstract when name is NULL. */
static void pt_type(Pr *p, const CcType *t, const char *name) {
    const CcType *base = type_base(t);
    pt_base(p, base);
    if (name || t != base) pr_str(p, " ");
    pt_prefix(p, t);
    if (name) pr_str(p, name);
    pt_suffix(p, t);
}

/* ---- expressions ------------------------------------------------------------ */

enum {
    PREC_COMMA = 0, PREC_ASSIGN = 1, PREC_TERNARY = 2, PREC_LOR = 3, PREC_LAND = 4,
    PREC_OR = 5, PREC_XOR = 6, PREC_AND = 7, PREC_EQ = 8, PREC_REL = 9, PREC_SHIFT = 10,
    PREC_ADD = 11, PREC_MUL = 12, PREC_UNARY = 13, PREC_POSTFIX = 14, PREC_PRIMARY = 15
};

static int binop_prec(CcOp op) {
    switch (op) {
    case CC_OP_MUL: case CC_OP_DIV: case CC_OP_MOD: return PREC_MUL;
    case CC_OP_ADD: case CC_OP_SUB: return PREC_ADD;
    case CC_OP_SHL: case CC_OP_SHR: return PREC_SHIFT;
    case CC_OP_LT: case CC_OP_GT: case CC_OP_LE: case CC_OP_GE: return PREC_REL;
    case CC_OP_EQ: case CC_OP_NE: return PREC_EQ;
    case CC_OP_AND: return PREC_AND;
    case CC_OP_XOR: return PREC_XOR;
    case CC_OP_OR: return PREC_OR;
    case CC_OP_LAND: return PREC_LAND;
    case CC_OP_LOR: return PREC_LOR;
    default: return PREC_ASSIGN;
    }
}

static const char *op_text(CcOp op) {
    switch (op) {
    case CC_OP_ADD: return "+"; case CC_OP_SUB: return "-"; case CC_OP_MUL: return "*";
    case CC_OP_DIV: return "/"; case CC_OP_MOD: return "%"; case CC_OP_SHL: return "<<";
    case CC_OP_SHR: return ">>"; case CC_OP_LT: return "<"; case CC_OP_GT: return ">";
    case CC_OP_LE: return "<="; case CC_OP_GE: return ">="; case CC_OP_EQ: return "==";
    case CC_OP_NE: return "!="; case CC_OP_AND: return "&"; case CC_OP_XOR: return "^";
    case CC_OP_OR: return "|"; case CC_OP_LAND: return "&&"; case CC_OP_LOR: return "||";
    case CC_OP_NEG: return "-"; case CC_OP_POS: return "+"; case CC_OP_NOT: return "!";
    case CC_OP_BITNOT: return "~"; case CC_OP_DEREF: return "*"; case CC_OP_ADDR: return "&";
    case CC_OP_PREINC: case CC_OP_POSTINC: return "++";
    case CC_OP_PREDEC: case CC_OP_POSTDEC: return "--";
    case CC_OP_SIZEOF: return "sizeof"; case CC_OP_ALIGNOF: return "_Alignof";
    case CC_OP_ASSIGN: return "="; case CC_OP_ADD_ASSIGN: return "+=";
    case CC_OP_SUB_ASSIGN: return "-="; case CC_OP_MUL_ASSIGN: return "*=";
    case CC_OP_DIV_ASSIGN: return "/="; case CC_OP_MOD_ASSIGN: return "%=";
    case CC_OP_SHL_ASSIGN: return "<<="; case CC_OP_SHR_ASSIGN: return ">>=";
    case CC_OP_AND_ASSIGN: return "&="; case CC_OP_XOR_ASSIGN: return "^=";
    case CC_OP_OR_ASSIGN: return "|=";
    default: return "?op?";
    }
}

static int is_postfix_op(CcOp op) { return op == CC_OP_POSTINC || op == CC_OP_POSTDEC; }

static int expr_prec(const CcExpr *e) {
    if (expr_is_cc(e)) return PREC_PRIMARY;
    switch (e->kind) {
    case CC_E_COMMA: return PREC_COMMA;
    case CC_E_ASSIGN: return PREC_ASSIGN;
    case CC_E_TERNARY: return PREC_TERNARY;
    case CC_E_BINARY: return binop_prec(e->op);
    case CC_E_UNARY: return is_postfix_op(e->op) ? PREC_POSTFIX : PREC_UNARY;
    case CC_E_CAST: case CC_E_SIZEOF_TYPE: return PREC_UNARY;
    case CC_E_CALL: case CC_E_INDEX: case CC_E_MEMBER: case CC_E_COMPOUND: return PREC_POSTFIX;
    default: return PREC_PRIMARY;
    }
}

static void pr_args(Pr *p, const CcExprList *l) {
    size_t i;
    int has_pp = 0;
    for (i = 0; i < l->n; i++)
        if (l->items[i] && l->items[i]->kind == CC_E_PP) has_pp = 1;
    for (i = 0; i < l->n; i++) {
        const CcExpr *e = l->items[i];
        if (!e) continue;
        if (e->kind == CC_E_PP) { pr_pp_inside(p, e->tok, e->span); continue; }
        pr_expr_p(p, e, PREC_ASSIGN);
        if (has_pp) {
            if (src_comma_after(p, e->span))
                pr_str(p, i + 1 < l->n && l->items[i + 1] && l->items[i + 1]->kind == CC_E_PP ? "," : ", ");
        } else if (i + 1 < l->n) {
            pr_str(p, ", ");
        }
    }
}

/* `- -x`, `+ +x`, `- --x`, `& &x`: the operand must not fuse with the operator. */
static int unary_needs_space_guard(CcOp outer, const CcExpr *operand) {
    CcOp inner;
    if (operand->kind != CC_E_UNARY || is_postfix_op(operand->op)) return 0;
    inner = operand->op;
    switch (outer) {
    case CC_OP_NEG: return inner == CC_OP_NEG || inner == CC_OP_PREDEC;
    case CC_OP_POS: return inner == CC_OP_POS || inner == CC_OP_PREINC;
    case CC_OP_ADDR: return inner == CC_OP_ADDR;
    default: return 0;
    }
}

static void pr_expr_node(Pr *p, const CcExpr *e) {
    if (expr_is_cc(e)) { pr_span(p, e->span, cc_expr_kind_name(e->kind)); return; }
    switch (e->kind) {
    case CC_E_IDENT:
        if (e->name) pr_str(p, e->name);
        else pr_tok_text(p, e->tok);
        break;
    case CC_E_NUMBER:
    case CC_E_CHAR:
        pr_tok_text(p, e->tok);
        break;
    case CC_E_STRING: {
        uint32_t n = e->n_string_toks ? e->n_string_toks : 1, k;
        for (k = 0; k < n; k++) {
            if (k) pr_str(p, " ");
            pr_tok_text(p, e->tok + k);
        }
        break;
    }
    case CC_E_PAREN:
        pr_str(p, "(");
        if (e->a) pr_expr_p(p, e->a, PREC_COMMA);
        pr_str(p, ")");
        break;
    case CC_E_UNARY:
        if (!e->a) { pr_span(p, e->span, "unary"); break; }
        if (is_postfix_op(e->op)) {
            pr_expr_p(p, e->a, PREC_POSTFIX);
            pr_str(p, op_text(e->op));
        } else if (e->op == CC_OP_SIZEOF || e->op == CC_OP_ALIGNOF) {
            pr_str(p, op_text(e->op));
            if (e->a->kind != CC_E_PAREN) pr_str(p, " ");
            pr_expr_p(p, e->a, PREC_UNARY);
        } else {
            pr_str(p, op_text(e->op));
            if (unary_needs_space_guard(e->op, e->a)) {
                pr_str(p, "(");
                pr_expr_p(p, e->a, PREC_COMMA);
                pr_str(p, ")");
            } else {
                pr_expr_p(p, e->a, PREC_UNARY);
            }
        }
        break;
    case CC_E_BINARY: {
        int prec = binop_prec(e->op);
        if (!e->a || !e->b) { pr_span(p, e->span, "binary"); break; }
        pr_expr_p(p, e->a, prec);
        pr_str(p, " ");
        pr_str(p, op_text(e->op));
        pr_str(p, " ");
        pr_expr_p(p, e->b, prec + 1);
        break;
    }
    case CC_E_ASSIGN:
        if (!e->a || !e->b) { pr_span(p, e->span, "assignment"); break; }
        pr_expr_p(p, e->a, PREC_UNARY);
        pr_str(p, " ");
        pr_str(p, op_text(e->op));
        pr_str(p, " ");
        pr_expr_p(p, e->b, PREC_ASSIGN);
        break;
    case CC_E_TERNARY:
        if (!e->a || !e->c) { pr_span(p, e->span, "conditional"); break; }
        pr_expr_p(p, e->a, PREC_LOR);
        pr_str(p, " ? ");
        if (e->b) pr_expr_p(p, e->b, PREC_COMMA); /* GNU `a ?: b` has no middle */
        pr_str(p, e->b ? " : " : ": ");
        pr_expr_p(p, e->c, PREC_TERNARY);
        break;
    case CC_E_CALL:
        if (!e->a) { pr_span(p, e->span, "call"); break; }
        pr_expr_p(p, e->a, PREC_POSTFIX);
        pr_str(p, "(");
        pr_args(p, &e->args);
        pr_str(p, ")");
        break;
    case CC_E_INDEX:
        if (!e->a || !e->b) { pr_span(p, e->span, "index"); break; }
        pr_expr_p(p, e->a, PREC_POSTFIX);
        pr_str(p, "[");
        pr_expr_p(p, e->b, PREC_COMMA);
        pr_str(p, "]");
        break;
    case CC_E_MEMBER:
        if (!e->a) { pr_span(p, e->span, "member"); break; }
        pr_expr_p(p, e->a, PREC_POSTFIX);
        pr_str(p, e->arrow ? "->" : ".");
        pr_str(p, e->name ? e->name : "");
        break;
    case CC_E_CAST:
        if (!e->a || !e->type) { pr_span(p, e->span, "cast"); break; }
        pr_str(p, "(");
        pt_type(p, e->type, NULL);
        pr_str(p, ")");
        if (e->a->kind == CC_E_PAREN) {
            pr_expr_node(p, e->a);
        } else {
            pr_str(p, "(");
            pr_expr_p(p, e->a, PREC_COMMA);
            pr_str(p, ")");
        }
        break;
    case CC_E_SIZEOF_TYPE:
        pr_str(p, e->op == CC_OP_ALIGNOF ? "_Alignof(" : "sizeof(");
        if (e->type) pt_type(p, e->type, NULL);
        pr_str(p, ")");
        break;
    case CC_E_COMPOUND:
        pr_str(p, "(");
        if (e->type) pt_type(p, e->type, NULL);
        pr_str(p, ")");
        if (e->init) pr_init(p, e->init);
        else pr_str(p, "{}");
        break;
    case CC_E_GENERIC_SEL: {
        const CcGenericSelArm *arm;
        pr_str(p, "_Generic(");
        if (e->a) pr_expr_p(p, e->a, PREC_ASSIGN);
        for (arm = e->arms; arm; arm = arm->next) {
            if (!arm->type && arm->expr && arm->expr->kind == CC_E_PP) {
                pr_pp_inside(p, arm->expr->tok, arm->expr->span);
                continue;
            }
            pr_str(p, ", ");
            if (arm->type) pt_type(p, arm->type, NULL);
            else pr_str(p, "default");
            pr_str(p, ": ");
            if (arm->expr) pr_expr_p(p, arm->expr, PREC_ASSIGN);
        }
        pr_str(p, ")");
        break;
    }
    case CC_E_STMT_EXPR: {
        int outer = p->indent;
        pr_str(p, "({");
        if (e->body && e->body->kind == CC_S_BLOCK) {
            uint32_t rb = e->body->span.last;
            pr_nl(p);
            p->indent = outer + 1;
            pr_items(p, &e->body->stmts);
            p->indent = outer;
            if (rb < p->f->n_toks && cc_tok_is_punct(&p->f->toks[rb], CC_P_RBRACE)) {
                pr_lead(p, rb, outer + 1);
                pr_set_pin_tok(p, rb);
            }
            if (p->at_line_start) pr_indent(p, outer);
            else pr_str(p, " ");
        } else if (e->body) {
            pr_str(p, " ");
            pr_stmt_text(p, e->body);
            pr_str(p, " ");
        }
        pr_str(p, "})");
        break;
    }
    case CC_E_PP:
        pr_pp_inside(p, e->tok, e->span);
        break;
    case CC_E_TYPE_ARG:
        if (e->type) pt_type(p, e->type, NULL);
        else pr_span(p, e->span, "type argument");
        break;
    case CC_E_COMMA:
        if (!e->a || !e->b) { pr_span(p, e->span, "comma"); break; }
        pr_expr_p(p, e->a, PREC_COMMA);
        pr_str(p, ", ");
        pr_expr_p(p, e->b, PREC_ASSIGN);
        break;
    default:
        pr_diag_off(p, e->span.first < p->f->n_toks ? p->f->toks[e->span.first].off : 0, 0,
                    "printer: unknown expression kind %d", (int)e->kind);
        pr_span(p, e->span, "expression");
        break;
    }
}

static void pr_expr_p(Pr *p, const CcExpr *e, int min_prec) {
    int paren;
    if (!e) return;
    paren = expr_prec(e) < min_prec;
    if (paren) pr_str(p, "(");
    pr_expr_node(p, e);
    if (paren) pr_str(p, ")");
}

/* ---- initializers -------------------------------------------------------------- */

static void pr_init(Pr *p, const CcInit *in) {
    size_t i;
    int has_pp = 0;
    if (!in) return;
    if (!in->is_list) {
        if (in->expr) pr_expr_p(p, in->expr, PREC_ASSIGN);
        return;
    }
    if (in->list.n == 0) { pr_str(p, "{}"); return; }
    for (i = 0; i < in->list.n; i++)
        if (in->list.items[i] && in->list.items[i]->is_pp) has_pp = 1;
    pr_str(p, "{ ");
    for (i = 0; i < in->list.n; i++) {
        const CcInit *item = in->list.items[i];
        const CcDesignator *ds;
        if (!item) continue;
        if (item->is_pp) { pr_pp_inside(p, item->pp_tok, item->span); continue; }
        for (ds = item->designators; ds; ds = ds->next) {
            if (ds->field) {
                pr_str(p, ".");
                pr_str(p, ds->field);
            } else {
                pr_str(p, "[");
                if (ds->index) pr_expr_p(p, ds->index, PREC_TERNARY);
                if (ds->index_hi) {
                    pr_str(p, " ... ");
                    pr_expr_p(p, ds->index_hi, PREC_TERNARY);
                }
                pr_str(p, "]");
            }
        }
        if (item->designators) pr_str(p, " = ");
        pr_init(p, item);
        if (has_pp) {
            if (src_comma_after(p, item->span))
                pr_str(p, i + 1 < in->list.n && in->list.items[i + 1] && in->list.items[i + 1]->is_pp ? "," : ", ");
        } else if (i + 1 < in->list.n) {
            pr_str(p, ", ");
        }
    }
    pr_str(p, " }");
}

/* ---- statements ------------------------------------------------------------------ */

/* Print a loop/if body after its header. Returns 1 when it ended with `}`. */
static int pr_body(Pr *p, const CcStmt *body) {
    int outer = p->indent;
    if (!body) { pr_str(p, ";"); return 0; }
    if (body->kind == CC_S_BLOCK && !stmt_is_cc(body)) {
        pr_str(p, " ");
        pr_block(p, body);
        return 1;
    }
    pr_nl(p);
    p->indent = outer + 1;
    pr_stmt_line(p, body);
    p->indent = outer;
    return 0;
}

static void pr_labelled_inner(Pr *p, const CcStmt *inner, int same_indent) {
    int outer = p->indent;
    if (!inner) return;
    if (inner->kind == CC_S_CASE || inner->kind == CC_S_LABEL) {
        pr_str(p, " ");
        pr_stmt_text(p, inner);
        return;
    }
    if (inner->kind == CC_S_BLOCK && !stmt_is_cc(inner)) {
        pr_str(p, " ");
        pr_block(p, inner);
        return;
    }
    pr_nl(p);
    p->indent = same_indent ? outer : outer + 1;
    pr_stmt_line(p, inner);
    p->indent = outer;
}

static void pr_stmt_text(Pr *p, const CcStmt *s) {
    int indent = p->indent;
    if (stmt_is_cc(s)) { pr_span(p, s->span, cc_stmt_kind_name(s->kind)); return; }
    switch (s->kind) {
    case CC_S_EXPR:
        if (s->expr) pr_expr_p(p, s->expr, PREC_COMMA);
        pr_str(p, ";");
        break;
    case CC_S_DECL:
        if (s->decl) pr_decl(p, s->decl);
        else pr_span(p, s->span, "declaration");
        break;
    case CC_S_BLOCK:
        pr_block(p, s);
        break;
    case CC_S_IF: {
        int brace;
        pr_str(p, "if (");
        pr_expr_p(p, s->expr, PREC_COMMA);
        pr_str(p, ")");
        brace = pr_body(p, s->body);
        if (s->else_body) {
            if (brace) pr_str(p, " else");
            else { pr_nl(p); pr_indent(p, indent); pr_str(p, "else"); }
            if (s->else_body->kind == CC_S_IF && !stmt_is_cc(s->else_body)) {
                pr_str(p, " ");
                pr_stmt_text(p, s->else_body);
            } else {
                pr_body(p, s->else_body);
            }
        }
        break;
    }
    case CC_S_WHILE:
        pr_str(p, "while (");
        pr_expr_p(p, s->expr, PREC_COMMA);
        pr_str(p, ")");
        pr_body(p, s->body);
        break;
    case CC_S_DO: {
        int brace;
        pr_str(p, "do");
        brace = pr_body(p, s->body);
        if (!brace) { pr_nl(p); pr_indent(p, indent); }
        else pr_str(p, " ");
        pr_str(p, "while (");
        pr_expr_p(p, s->expr, PREC_COMMA);
        pr_str(p, ");");
        break;
    }
    case CC_S_FOR:
        pr_str(p, "for (");
        if (s->decl) {
            pr_decl(p, s->decl);
        } else {
            if (s->init_expr) pr_expr_p(p, s->init_expr, PREC_COMMA);
            pr_str(p, ";");
        }
        if (s->expr) { pr_str(p, " "); pr_expr_p(p, s->expr, PREC_COMMA); }
        pr_str(p, ";");
        if (s->expr2) { pr_str(p, " "); pr_expr_p(p, s->expr2, PREC_COMMA); }
        pr_str(p, ")");
        pr_body(p, s->body);
        break;
    case CC_S_SWITCH:
        pr_str(p, "switch (");
        pr_expr_p(p, s->expr, PREC_COMMA);
        pr_str(p, ")");
        pr_body(p, s->body);
        break;
    case CC_S_CASE:
        if (s->is_default) {
            pr_str(p, "default:");
        } else {
            pr_str(p, "case ");
            pr_expr_p(p, s->expr, PREC_TERNARY);
            if (s->expr2) { pr_str(p, " ... "); pr_expr_p(p, s->expr2, PREC_TERNARY); }
            pr_str(p, ":");
        }
        pr_labelled_inner(p, s->body, 0);
        break;
    case CC_S_LABEL:
        pr_str(p, s->name ? s->name : "");
        pr_str(p, ":");
        if (!s->body) pr_str(p, ";");
        else pr_labelled_inner(p, s->body, 1);
        break;
    case CC_S_RETURN:
        pr_str(p, "return");
        if (s->expr) { pr_str(p, " "); pr_expr_p(p, s->expr, PREC_COMMA); }
        pr_str(p, ";");
        break;
    case CC_S_BREAK:
        pr_str(p, "break;");
        break;
    case CC_S_CONTINUE:
        pr_str(p, "continue;");
        break;
    case CC_S_GOTO:
        pr_str(p, "goto ");
        pr_str(p, s->name ? s->name : "");
        pr_str(p, ";");
        break;
    case CC_S_ASM:
        pr_span(p, s->span, "asm");
        break;
    default:
        pr_diag_off(p, s->span.first < p->f->n_toks ? p->f->toks[s->span.first].off : 0, 0,
                    "printer: unknown statement kind %d", (int)s->kind);
        pr_span(p, s->span, "statement");
        break;
    }
    p->indent = indent;
}

/* A statement on its own line at p->indent (trivia, pin, indentation). */
static void pr_stmt_line(Pr *p, const CcStmt *s) {
    int indent = p->indent;
    if (!s) return;
    pr_item_start(p, s->span, indent);
    pr_stmt_text(p, s);
    p->indent = indent;
}

/* The items of a block at p->indent; statements after a case label are
 * indented one level deeper. */
static void pr_items(Pr *p, const CcStmtList *l) {
    int base = p->indent, in_case = 0;
    size_t i;
    for (i = 0; i < l->n; i++) {
        const CcStmt *st = l->items[i];
        if (!st) continue;
        if (st->kind == CC_S_CASE) in_case = 1;
        p->indent = st->kind == CC_S_CASE || st->kind == CC_S_LABEL ? base : base + in_case;
        pr_stmt_line(p, st);
        pr_trailing(p, st->span.last + 1);
        pr_nl(p);
    }
    p->indent = base;
}

/* `{` items `}` starting at the current position, closing at p->indent. */
static void pr_block(Pr *p, const CcStmt *s) {
    int outer = p->indent;
    uint32_t rb = s->span.last;
    pr_str(p, "{");
    pr_nl(p);
    p->indent = outer + 1;
    pr_items(p, &s->stmts);
    p->indent = outer;
    if (rb < p->f->n_toks && cc_tok_is_punct(&p->f->toks[rb], CC_P_RBRACE)) {
        pr_lead(p, rb, outer + 1);
        pr_set_pin_tok(p, rb);
    }
    if (p->at_line_start) pr_indent(p, outer);
    else pr_str(p, " ");
    pr_str(p, "}");
}

/* ---- declarations ------------------------------------------------------------------ */

static void pr_decl(Pr *p, const CcDecl *d) {
    int indent = p->indent;
    if (decl_is_cc(d)) { pr_span(p, d->span, cc_decl_kind_name(d->kind)); return; }
    switch (d->kind) {
    case CC_D_VAR: {
        const CcType *base = type_base(d->type);
        pr_specs(p, d->specs & ~(base ? base->quals : 0u));
        pr_attrs(p, d->attrs, 0);
        pt_type(p, d->type, d->name);
        if (d->init) {
            pr_str(p, " = ");
            pr_init(p, d->init);
        }
        pr_str(p, ";");
        break;
    }
    case CC_D_FUNC: {
        const CcType *base = type_base(d->type);
        int keep_body = d->body != NULL;
        if (keep_body && p->opts.header_mode &&
            !((d->specs & CC_S_STATIC) && (d->specs & CC_F_INLINE)))
            keep_body = 0;
        pr_specs(p, d->specs & ~(base ? base->quals : 0u));
        pr_attrs(p, d->attrs, 0);
        pt_type(p, d->type, d->name);
        if (!keep_body) {
            pr_str(p, ";");
        } else if (d->body->kind == CC_S_BLOCK && !stmt_is_cc(d->body)) {
            pr_str(p, " ");
            pr_block(p, d->body);
        } else {
            pr_nl(p);
            p->indent = indent + 1;
            pr_stmt_line(p, d->body);
            p->indent = indent;
        }
        break;
    }
    case CC_D_TYPEDEF: {
        const CcType *base = type_base(d->type);
        pr_str(p, "typedef ");
        pr_specs(p, d->specs & ~CC_S_TYPEDEF & ~(base ? base->quals : 0u));
        pr_attrs(p, d->attrs, 0);
        pt_type(p, d->type, d->name);
        pr_str(p, ";");
        break;
    }
    case CC_D_TAGGED:
        pr_specs(p, d->specs);
        pr_attrs(p, d->attrs, 0);
        pt_type(p, d->type, NULL);
        pr_str(p, ";");
        break;
    case CC_D_STATIC_ASSERT:
        pr_str(p, "_Static_assert(");
        pr_expr_p(p, d->assert_expr, PREC_ASSIGN);
        if (d->assert_msg_tok < p->f->n_toks && p->f->toks[d->assert_msg_tok].kind == CC_TK_STRING) {
            pr_str(p, ", ");
            pr_tok_text(p, d->assert_msg_tok);
        }
        pr_str(p, ");");
        break;
    case CC_D_PP:
        if (!p->at_line_start && p->out->len > p->line_start) {
            /* a directive must start its line; a same-line comment before it moves up */
            size_t k = p->line_start;
            int only_space = 1;
            for (; k < p->out->len; k++)
                if (p->out->data[k] != ' ' && p->out->data[k] != '\t') { only_space = 0; break; }
            if (!only_space) { pr_nl(p); pr_indent(p, indent); }
        }
        if (d->pp_skipped_region || d->span.last > d->tok || d->span.first < d->tok)
            pr_span(p, d->span, "preprocessor line");
        else if (d->tok < p->f->n_toks && p->f->toks[d->tok].kind == CC_TK_PP)
            pr_tok_text(p, d->tok);
        else
            pr_span(p, d->span, "preprocessor line");
        break;
    case CC_D_STMT:
        if (d->body) pr_stmt_text(p, d->body);
        else pr_span(p, d->span, "statement");
        break;
    case CC_D_MACRO_CALL:
        if (d->expr) pr_expr_p(p, d->expr, PREC_COMMA);
        else pr_span(p, d->span, "macro invocation");
        if (d->span.last < p->f->n_toks && cc_tok_is_punct(&p->f->toks[d->span.last], CC_P_SEMI))
            pr_str(p, ";");
        break;
    case CC_D_EMPTY:
        pr_str(p, ";");
        break;
    default:
        pr_diag_off(p, d->span.first < p->f->n_toks ? p->f->toks[d->span.first].off : 0, 0,
                    "printer: unknown declaration kind %d", (int)d->kind);
        pr_span(p, d->span, "declaration");
        break;
    }
    p->indent = indent;
}

/* ---- identity mode and coverage ------------------------------------------------------ */

typedef struct Cov {
    Pr *p;
    uint8_t *marks;
} Cov;

static void cov_mark(Cov *c, CcSpan s, const char *what) {
    uint32_t i;
    if (!pr_span_ok(c->p, s, what)) return;
    for (i = s.first; i <= s.last; i++) c->marks[i] = 1;
}

static int cov_decl(CcVisitor *v, CcDecl *d) { cov_mark((Cov *)v->ctx, d->span, cc_decl_kind_name(d->kind)); return 0; }
static int cov_stmt(CcVisitor *v, CcStmt *s) { cov_mark((Cov *)v->ctx, s->span, cc_stmt_kind_name(s->kind)); return 0; }
static int cov_expr(CcVisitor *v, CcExpr *e) { cov_mark((Cov *)v->ctx, e->span, cc_expr_kind_name(e->kind)); return 0; }
static int cov_type(CcVisitor *v, CcType *t) { cov_mark((Cov *)v->ctx, t->span, cc_type_kind_name(t->kind)); return 0; }

static void id_push_tok(Pr *p, uint32_t i) {
    const CcToken *t = &p->f->toks[i];
    cc_buf_push(p->out, p->f->src + t->lead_off, t->lead_len + t->len);
}

static void pr_identity(Pr *p, const CcUnit *u) {
    const CcLexFile *f = p->f;
    uint32_t n = f->n_toks, expected = 0, i, n_uncovered = 0, reported = 0;
    Cov cov;
    CcVisitor v;
    size_t k;

    for (k = 0; k < u->decls.n; k++) {
        const CcDecl *d = u->decls.items[k];
        CcSpan s;
        if (!d) continue;
        s = d->span;
        if (!pr_span_ok(p, s, cc_decl_kind_name(d->kind))) continue;
        if (s.first < expected) {
            if (s.last < expected) continue; /* a declarator split off `int a, b;` */
            pr_diag_off(p, f->toks[s.first].off, f->toks[s.first].len,
                        "top-level %s overlaps the previous declaration by %u token(s)",
                        cc_decl_kind_name(d->kind), (unsigned)(expected - s.first));
            s.first = expected;
        } else if (s.first > expected) {
            pr_diag_off(p, f->toks[expected].off, f->toks[expected].len,
                        "gap of %u token(s) between top-level declarations before '%.*s'",
                        (unsigned)(s.first - expected), (int)f->toks[s.first].len,
                        f->src + f->toks[s.first].off);
            for (i = expected; i < s.first; i++) id_push_tok(p, i);
        }
        for (i = s.first; i <= s.last; i++) id_push_tok(p, i);
        expected = s.last + 1;
    }
    if (n && expected < n - 1) {
        pr_diag_off(p, f->toks[expected].off, f->toks[expected].len,
                    "gap of %u token(s) after the last top-level declaration",
                    (unsigned)(n - 1 - expected));
        for (i = expected; i < n - 1; i++) id_push_tok(p, i);
    }
    if (n) {
        const CcToken *eof = &f->toks[n - 1];
        cc_buf_push(p->out, f->src + eof->lead_off, eof->lead_len);
    }

    /* Coverage: every token must be inside the span of some node. */
    cov.p = p;
    cov.marks = (uint8_t *)cc_arena_alloc(p->arena, n ? n : 1, 1);
    memset(&v, 0, sizeof v);
    v.ctx = &cov;
    v.on_decl = cov_decl;
    v.on_stmt = cov_stmt;
    v.on_expr = cov_expr;
    v.on_type = cov_type;
    cc_ast_walk((CcUnit *)u, &v);
    for (i = 0; n && i + 1 < n; i++) {
        if (cov.marks[i]) continue;
        n_uncovered++;
        if (reported < PR_MAX_UNCOVERED_REPORTS) {
            pr_diag_off(p, f->toks[i].off, f->toks[i].len, "token not covered by the tree: '%.*s'",
                        (int)f->toks[i].len, f->src + f->toks[i].off);
            reported++;
        }
    }
    if (n_uncovered > reported)
        pr_diag_off(p, f->len, 0, "%u more token(s) not covered by the tree", (unsigned)(n_uncovered - reported));
}

/* ---- lowered unit ---------------------------------------------------------------------- */

static void pr_init_ctx(Pr *p, CcBuf *out, const CcLexFile *f, CcArena *a, CcDiag *d,
                        CcSrcMap *map, const CcPrintOpts *opts) {
    memset(p, 0, sizeof *p);
    p->out = out;
    p->f = f;
    p->arena = a;
    p->d = d;
    if (opts) p->opts = *opts;
    if (!p->opts.path) p->opts.path = f->path ? f->path : "<input>";
    p->map = map;
    p->emit_line = 1;
    p->line_start = out->len;
    p->at_line_start = 1;
    p->pin_path = p->opts.path;
    if (f->len || f->n_toks) pr_set_pin(p, 0);
}

static void pr_unit(Pr *p, const CcUnit *u) {
    size_t k;
    if (p->opts.header_mode) {
        pr_str(p, "#pragma once");
        pr_nl(p);
    }
    for (k = 0; k < u->decls.n; k++) {
        const CcDecl *d = u->decls.items[k];
        if (!d) continue;
        p->indent = 0;
        pr_item_start(p, d->span, 0);
        pr_decl(p, d);
        pr_trailing(p, d->span.last + 1);
        pr_nl(p);
    }
    if (p->f->n_toks) pr_lead(p, p->f->n_toks - 1, 0);
    if (!p->at_line_start) pr_nl(p);
}

void cc_print_unit(CcBuf *out, CcSrcMap *map, CcArena *a, CcDiag *d, const CcUnit *u,
                   const CcPrintOpts *opts) {
    Pr p;
    CcArena local;
    int own_arena = 0;
    if (!a) {
        cc_arena_init(&local, 0);
        a = &local;
        own_arena = 1;
    }
    if (map) { map->entries = NULL; map->n = 0; }
    pr_init_ctx(&p, out, u->file, a, d, map, opts);
    if (p.opts.identity) pr_identity(&p, u);
    else pr_unit(&p, u);
    if (own_arena) {
        if (map && map->n) {
            /* the map must outlive the local arena */
            CcSrcMapEntry *e = (CcSrcMapEntry *)malloc(map->n * sizeof *e);
            if (!e) { fprintf(stderr, "cc: out of memory: source map\n"); abort(); }
            memcpy(e, map->entries, map->n * sizeof *e);
            map->entries = e;
        }
        cc_arena_free(&local);
    }
}

void cc_print_expr(CcBuf *out, const CcLexFile *f, const CcExpr *e) {
    Pr p;
    CcArena a;
    cc_arena_init(&a, 0);
    pr_init_ctx(&p, out, f, &a, NULL, NULL, NULL);
    p.at_line_start = 0;
    pr_expr_p(&p, e, PREC_COMMA);
    cc_arena_free(&a);
}

void cc_print_stmt(CcBuf *out, const CcLexFile *f, const CcStmt *s, int indent) {
    Pr p;
    CcArena a;
    cc_arena_init(&a, 0);
    pr_init_ctx(&p, out, f, &a, NULL, NULL, NULL);
    p.indent = indent;
    p.last_lead_tok = s->span.first + 1; /* quote the construct, not the comments before it */
    pr_stmt_line(&p, s);
    cc_arena_free(&a);
}

void cc_print_type(CcBuf *out, const CcLexFile *f, const CcType *t, const char *declarator_name) {
    Pr p;
    CcArena a;
    cc_arena_init(&a, 0);
    pr_init_ctx(&p, out, f, &a, NULL, NULL, NULL);
    p.at_line_start = 0;
    pt_type(&p, t, declarator_name);
    cc_arena_free(&a);
}
