/* Recursive-descent parser for Concurrent-C. See parse.h for the contract.
 *
 * Layout of this file:
 *   1. parser state, keywords, the typedef-name scope stack, token helpers,
 *      diagnostics and recovery, preprocessor-line handling
 *   2. types: specifiers, declarators, struct/enum bodies, parameters
 *   3. expressions, templates and closures
 *   4. statements
 *   5. declarations and the unit
 *
 * Every construct is parsed into the typed tree from ast.h; the only raw
 * spans are preprocessor lines, inactive `#if` regions and @grammar bodies,
 * which are opaque by design. */
#include "parse.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ======================================================================
 * 1. Parser state
 * ====================================================================== */

typedef enum Kw {
    KW_NONE = 0,
    KW_AUTO, KW_BREAK, KW_CASE, KW_CHAR, KW_CONST, KW_CONTINUE, KW_DEFAULT, KW_DO, KW_DOUBLE,
    KW_ELSE, KW_ENUM, KW_EXTERN, KW_FLOAT, KW_FOR, KW_GOTO, KW_IF, KW_INLINE, KW_INT, KW_LONG,
    KW_REGISTER, KW_RESTRICT, KW_RETURN, KW_SHORT, KW_SIGNED, KW_SIZEOF, KW_STATIC, KW_STRUCT,
    KW_SWITCH, KW_TYPEDEF, KW_UNION, KW_UNSIGNED, KW_VOID, KW_VOLATILE, KW_WHILE,
    KW_ALIGNAS, KW_ALIGNOF, KW_ATOMIC, KW_BOOL, KW_COMPLEX, KW_GENERIC, KW_NORETURN,
    KW_STATIC_ASSERT, KW_THREAD_LOCAL,
    /* GNU / MSVC spellings */
    KW_ATTRIBUTE, KW_ATTRIBUTE2, KW_EXTENSION, KW_INLINE2, KW_INLINE3, KW_RESTRICT2, KW_RESTRICT3,
    KW_CONST2, KW_CONST3, KW_VOLATILE2, KW_VOLATILE3, KW_SIGNED2, KW_SIGNED3, KW_TYPEOF, KW_TYPEOF2,
    KW_TYPEOF3, KW_ALIGNOF2, KW_ALIGNOF3, KW_ASM, KW_ASM2, KW_ASM3, KW_THREAD, KW_INT128,
    KW_DECLSPEC, KW_BUILTIN_VA_LIST, KW_FORCEINLINE, KW_FLOAT128, KW_NULLPTR_T, KW_ALIGNAS2, KW_ALIGNOF4,
    /* Concurrent-C bare words (contextual) */
    KW_IN, KW_UNSAFE, KW_SPAWN, KW_SEQ, KW_WAIT, KW_CACHE, KW_WORKER, KW_ON, KW_AS, KW_ASYNC,
    KW_CLOSING, KW_TYPE, KW_CC_GENERIC_FACTORY, KW_CC_GENERIC_FACTORY_EXTEND,
    KW_COUNT
} Kw;

static const char *const kw_text[KW_COUNT] = {
    "",
    "auto", "break", "case", "char", "const", "continue", "default", "do", "double",
    "else", "enum", "extern", "float", "for", "goto", "if", "inline", "int", "long",
    "register", "restrict", "return", "short", "signed", "sizeof", "static", "struct",
    "switch", "typedef", "union", "unsigned", "void", "volatile", "while",
    "_Alignas", "_Alignof", "_Atomic", "_Bool", "_Complex", "_Generic", "_Noreturn",
    "_Static_assert", "_Thread_local",
    "__attribute__", "__attribute", "__extension__", "__inline", "__inline__", "__restrict", "__restrict__",
    "__const", "__const__", "__volatile", "__volatile__", "__signed", "__signed__", "__typeof__", "__typeof",
    "typeof", "__alignof__", "__alignof", "__asm__", "__asm", "asm", "__thread", "__int128",
    "__declspec", "__builtin_va_list", "__forceinline", "__float128", "nullptr_t", "alignas", "alignof",
    "in", "unsafe", "spawn", "seq", "wait", "cache", "worker", "on", "as", "async",
    "closing", "type", "CC_GENERIC_FACTORY", "CC_GENERIC_FACTORY_EXTEND",
};

/* One name binding in the scope stack. */
typedef struct Sym {
    CcName name;
    int is_type;
    int depth;
    struct Sym *shadowed;   /* the binding this one hides */
} Sym;

typedef struct SymSlot { CcName key; Sym *top; } SymSlot;

typedef struct Frame {
    struct Frame *outer;
    CC_LIST(Sym) syms;
    int depth;
} Frame;

/* Shared between a parser and the sub-parsers it spawns for template slots. */
typedef struct Syms {
    CcArena *a;
    SymSlot *slots;
    size_t cap, n;
    Frame *frame;
    int depth;
} Syms;

/* `#if` nesting seen through preprocessor lines, for skipping regions that
 * are statically inactive (`__cplusplus`, `#if 0`). */
typedef struct PpCond {
    int else_dead;   /* the branch after `#else` / `#elif` is inactive */
    int taken;       /* some branch was already active (for #elif) */
} PpCond;

typedef struct P {
    CcArena *a;
    CcDiag *d;
    CcIntern *in;
    CcLexFile *f;
    CcParseOpts opts;
    const CcToken *t;
    uint32_t n;
    uint32_t i;
    CcUnit *u;
    Syms *syms;
    CcName kw[KW_COUNT];
    CcName n_typeview, n_tag, n_task, n_packed, n_attribute, n_alignas, n_declspec, n_asm;
    CcName n_parallel_join, n_typeof;
    /* index of a `;` or `}` that already terminated the statement being
     * parsed (an unwrap body / closure block consumed it); `expect_semi`
     * accepts it in place of a `;` */
    uint32_t consumed_terminator;
    /* a bare template literal is allowed as an operand (inside @emit args) */
    int tpl_ok;
    /* nesting of the `#if` stack */
    CC_LIST(PpCond) pp;
    /* statement-position parse of `@parallel` as a declaration initializer */
    CcStmt *bind_stmt;
    int in_slot;             /* parsing a template slot */
} P;

/* ---- names ----------------------------------------------------------- */

static CcName tok_name(P *p, uint32_t i) {
    const CcToken *t = &p->t[i];
    return cc_intern(p->in, p->f->src + t->off, t->len);
}

static Kw kw_of(P *p, uint32_t i) {
    CcName nm;
    int k;
    if (p->t[i].kind != CC_TK_IDENT) return KW_NONE;
    nm = tok_name(p, i);
    for (k = 1; k < KW_IN; k++)   /* the CC contextual words after KW_IN are ordinary names */
        if (p->kw[k] == nm) return (Kw)k;
    return KW_NONE;
}

static int is_kw(P *p, uint32_t i, Kw k) {
    return p->t[i].kind == CC_TK_IDENT && tok_name(p, i) == p->kw[k];
}

/* ---- scope stack ----------------------------------------------------- */

static Syms *syms_new(CcArena *a) {
    Syms *s = CC_NEW(a, Syms);
    s->a = a;
    s->cap = 1024;
    s->slots = CC_NEW_N(a, SymSlot, s->cap);
    s->frame = CC_NEW(a, Frame);
    s->frame->depth = 0;
    s->depth = 0;
    return s;
}

static size_t syms_hash(CcName k, size_t cap) {
    uintptr_t h = (uintptr_t)k;
    h ^= h >> 17;
    h *= 0x9E3779B97F4A7C15ull;
    h ^= h >> 29;
    return (size_t)h & (cap - 1);
}

static SymSlot *syms_slot(Syms *s, CcName k) {
    size_t j = syms_hash(k, s->cap);
    for (;;) {
        if (!s->slots[j].key || s->slots[j].key == k) return &s->slots[j];
        j = (j + 1) & (s->cap - 1);
    }
}

static void syms_grow(Syms *s) {
    size_t ncap = s->cap * 2, i;
    SymSlot *old = s->slots;
    size_t oldcap = s->cap;
    s->slots = CC_NEW_N(s->a, SymSlot, ncap);
    s->cap = ncap;
    for (i = 0; i < oldcap; i++)
        if (old[i].key) *syms_slot(s, old[i].key) = old[i];
}

static Sym *sym_lookup(Syms *s, CcName k) {
    SymSlot *sl = syms_slot(s, k);
    return sl->key ? sl->top : NULL;
}

static void sym_declare(Syms *s, CcName k, int is_type) {
    SymSlot *sl;
    Sym *sy;
    if (s->n * 2 >= s->cap) syms_grow(s);
    sl = syms_slot(s, k);
    if (!sl->key) { sl->key = k; s->n++; }
    /* redeclaring in the same scope with the same kind: nothing to do */
    if (sl->top && sl->top->depth == s->frame->depth && sl->top->is_type == is_type) return;
    sy = CC_NEW(s->a, Sym);
    sy->name = k;
    sy->is_type = is_type;
    sy->depth = s->frame->depth;
    sy->shadowed = sl->top;
    sl->top = sy;
    CC_LIST_PUSH(s->a, &s->frame->syms, sy);
}

/* Declare at file scope from an inner scope (forward-referenced type names). */
static void sym_declare_global(Syms *s, CcName k) {
    SymSlot *sl;
    Sym *sy, **link;
    Frame *fr = s->frame;
    if (s->n * 2 >= s->cap) syms_grow(s);
    sl = syms_slot(s, k);
    if (!sl->key) { sl->key = k; s->n++; }
    while (fr->outer) fr = fr->outer;
    sy = CC_NEW(s->a, Sym);
    sy->name = k;
    sy->is_type = 1;
    sy->depth = 0;
    /* insert below every binding of an inner scope */
    link = &sl->top;
    while (*link && (*link)->depth > 0) link = &(*link)->shadowed;
    sy->shadowed = *link;
    *link = sy;
    CC_LIST_PUSH(s->a, &fr->syms, sy);
}

static void scope_push(Syms *s) {
    Frame *fr = CC_NEW(s->a, Frame);
    fr->outer = s->frame;
    fr->depth = s->frame->depth + 1;
    s->frame = fr;
    s->depth = fr->depth;
}

static void scope_pop(Syms *s) {
    Frame *fr = s->frame;
    size_t i;
    if (!fr->outer) return;
    for (i = fr->syms.n; i > 0; i--) {
        Sym *sy = fr->syms.items[i - 1];
        SymSlot *sl = syms_slot(s, sy->name);
        /* unlink this binding wherever it sits in the chain */
        Sym **link = &sl->top;
        while (*link && *link != sy) link = &(*link)->shadowed;
        if (*link) *link = sy->shadowed;
    }
    s->frame = fr->outer;
    s->depth = s->frame->depth;
}

static int name_is_type(P *p, CcName nm) {
    Sym *sy = sym_lookup(p->syms, nm);
    return sy && sy->is_type;
}

static int name_is_var(P *p, CcName nm) {
    Sym *sy = sym_lookup(p->syms, nm);
    return sy && !sy->is_type;
}

static void declare_type(P *p, CcName nm) {
    sym_declare(p->syms, nm, 1);
    if (p->syms->depth == 0) CC_LIST_PUSH(p->a, &p->u->typedef_names, nm);
}

static void declare_var(P *p, CcName nm) { sym_declare(p->syms, nm, 0); }

static void assume_type(P *p, CcName nm) {
    if (name_is_type(p, nm)) return;
    sym_declare_global(p->syms, nm);
    CC_LIST_PUSH(p->a, &p->u->assumed_types, nm);
}

/* ---- token helpers --------------------------------------------------- */

static int name_is_all_caps(CcName nm) {
    const char *q;
    int letters = 0;
    for (q = nm; *q; q++) {
        if (*q >= 'a' && *q <= 'z') return 0;
        if (*q >= 'A' && *q <= 'Z') letters = 1;
    }
    return letters;
}


static uint32_t idx(P *p, uint32_t k) {
    uint32_t j = p->i + k;
    return j < p->n ? j : p->n - 1;
}
static const CcToken *tk(P *p, uint32_t k) { return &p->t[idx(p, k)]; }
static int at_eof(P *p) { return p->t[p->i].kind == CC_TK_EOF; }
static int tok_is_p(const CcToken *t, CcPunct pc) { return t->kind == CC_TK_PUNCT && t->punct == pc; }
static int at_p(P *p, CcPunct pc) { return tok_is_p(&p->t[p->i], pc); }
static int at_p2(P *p, CcPunct a, CcPunct b) { return at_p(p, a) && tok_is_p(tk(p, 1), b); }
static int at_kw(P *p, Kw k) { return is_kw(p, p->i, k); }
static int at_word(P *p, const char *w) { return cc_tok_is_at(p->f, &p->t[p->i], w); }
static int tok_is_word(P *p, uint32_t i, const char *w) { return cc_tok_is_at(p->f, &p->t[i], w); }
static int at_ident(P *p) { return p->t[p->i].kind == CC_TK_IDENT; }
static void adv(P *p) { if (p->i + 1 < p->n) p->i++; }
static uint32_t last_tok(P *p) { return p->i ? p->i - 1 : 0; }
static CcSpan span_from(P *p, uint32_t first) { CcSpan s; s.first = first; s.last = last_tok(p); if (s.last < s.first) s.last = s.first; return s; }
static CcSpan span2(uint32_t a, uint32_t b) { CcSpan s; s.first = a; s.last = b < a ? a : b; return s; }
static int accept_p(P *p, CcPunct pc) { if (at_p(p, pc)) { adv(p); return 1; } return 0; }
static int accept_kw(P *p, Kw k) { if (at_kw(p, k)) { adv(p); return 1; } return 0; }

/* Adjacent tokens (no trivia between). */
static int adjacent(P *p, uint32_t i) { return i + 1 < p->n && p->t[i + 1].lead_len == 0; }

/* ---- diagnostics ----------------------------------------------------- */

static void verr_at(P *p, uint32_t tok, const char *fmt, va_list ap) {
    const CcToken *t = &p->t[tok < p->n ? tok : p->n - 1];
    CcLoc loc = cc_lex_loc(p->f, t->off);
    char *text;
    int n;
    va_list ap2;
    va_copy(ap2, ap);
    n = vsnprintf(NULL, 0, fmt, ap2);
    va_end(ap2);
    if (n < 0) { fprintf(stderr, "cc: parser: bad diagnostic format\n"); abort(); }
    text = (char *)cc_arena_alloc(p->a, (size_t)n + 1, 1);
    vsnprintf(text, (size_t)n + 1, fmt, ap);
    cc_diag_emit_at(p->d, CC_SEV_ERROR, loc, p->f->src, p->f->len, t->off,
                    t->len ? t->len : 1, "%s", text);
}

static void err_at(P *p, uint32_t tok, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    verr_at(p, tok, fmt, ap);
    va_end(ap);
}

static void err_here(P *p, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    verr_at(p, p->i, fmt, ap);
    va_end(ap);
}

/* The user's spelling of a token for messages. */
static const char *tok_desc(P *p, uint32_t i) {
    const CcToken *t = &p->t[i < p->n ? i : p->n - 1];
    if (t->kind == CC_TK_EOF) return p->in_slot ? "the end of the ${...} slot" : "end of file";
    if (t->kind == CC_TK_PP) return "a preprocessor line";
    if (t->kind == CC_TK_TEMPLATE) return "a template literal";
    if (t->kind == CC_TK_STRING) return "a string literal";
    return cc_arena_printf(p->a, "'%.*s'", (int)(t->len > 40 ? 40 : t->len), p->f->src + t->off);
}

static unsigned tok_line(P *p, uint32_t i) {
    const CcToken *t = &p->t[i < p->n ? i : p->n - 1];
    return (unsigned)cc_lex_loc(p->f, t->off).line;
}

/* Expect a punctuator; `what` names the construct being closed, `open` the
 * token that opened it (or the same token when not applicable). */
static int expect_close(P *p, CcPunct pc, const char *what, uint32_t open) {
    if (accept_p(p, pc)) return 1;
    if (open != p->i && open < p->n)
        err_here(p, "expected '%s' to close the %s on line %u, found %s", cc_punct_text(pc), what,
                 tok_line(p, open), tok_desc(p, p->i));
    else
        err_here(p, "expected '%s' after the %s, found %s", cc_punct_text(pc), what, tok_desc(p, p->i));
    return 0;
}

static int expect_p(P *p, CcPunct pc, const char *what) {
    if (accept_p(p, pc)) return 1;
    err_here(p, "expected '%s' in %s, found %s", cc_punct_text(pc), what, tok_desc(p, p->i));
    return 0;
}

/* A statement terminator: `;`, or a `;`/`}` an inner construct already
 * consumed on behalf of this statement. */
static int expect_semi(P *p, const char *what) {
    if (accept_p(p, CC_P_SEMI)) return 1;
    if (p->i > 0 && p->consumed_terminator == p->i - 1) return 1;
    err_here(p, "expected ';' after %s, found %s", what, tok_desc(p, p->i));
    return 0;
}

/* ---- recovery -------------------------------------------------------- */

/* Skip a balanced bracket group starting at the current opener. */
static void skip_balanced(P *p) {
    int depth = 0;
    for (;;) {
        const CcToken *t = &p->t[p->i];
        if (t->kind == CC_TK_EOF) return;
        if (t->kind == CC_TK_PUNCT) {
            if (t->punct == CC_P_LPAREN || t->punct == CC_P_LBRACKET || t->punct == CC_P_LBRACE) depth++;
            else if (t->punct == CC_P_RPAREN || t->punct == CC_P_RBRACKET || t->punct == CC_P_RBRACE) {
                depth--;
                if (depth <= 0) { adv(p); return; }
            }
        }
        adv(p);
    }
}

/* Recover at the next `;` (consumed) or `}` (left in place) at the current
 * nesting depth; a `{` met on the way is skipped whole and ends the sync. */
static void sync(P *p, uint32_t start) {
    if (p->i == start && !at_eof(p) && !at_p(p, CC_P_SEMI) && !at_p(p, CC_P_RBRACE)) adv(p);
    for (;;) {
        const CcToken *t = &p->t[p->i];
        if (t->kind == CC_TK_EOF) return;
        if (t->kind == CC_TK_PUNCT) {
            if (t->punct == CC_P_SEMI) { adv(p); return; }
            if (t->punct == CC_P_RBRACE) return;
            if (t->punct == CC_P_LBRACE) { skip_balanced(p); return; }
            if (t->punct == CC_P_LPAREN || t->punct == CC_P_LBRACKET) { skip_balanced(p); continue; }
        }
        adv(p);
    }
}

/* Index of the token matching the opener at `i` (same kind), or n-1. */
static uint32_t match_close(P *p, uint32_t i) {
    int depth = 0;
    for (; i < p->n; i++) {
        const CcToken *t = &p->t[i];
        if (t->kind == CC_TK_EOF) return p->n - 1;
        if (t->kind != CC_TK_PUNCT) continue;
        if (t->punct == CC_P_LPAREN || t->punct == CC_P_LBRACKET || t->punct == CC_P_LBRACE) depth++;
        else if (t->punct == CC_P_RPAREN || t->punct == CC_P_RBRACKET || t->punct == CC_P_RBRACE) {
            depth--;
            if (depth == 0) return i;
        }
    }
    return p->n - 1;
}

/* ---- preprocessor lines ---------------------------------------------- */

/* Text of a PP token after `#` and blanks, with line splices removed. */
static const char *pp_body(P *p, uint32_t i, size_t *len_out) {
    const CcToken *t = &p->t[i];
    const char *s = p->f->src + t->off;
    CcBuf b;
    size_t j;
    char *out;
    cc_buf_init(&b);
    for (j = 1; j < t->len; j++) {
        if (s[j] == '\\' && j + 1 < t->len && s[j + 1] == '\n') { j++; continue; }
        if (s[j] == '\\' && j + 2 < t->len && s[j + 1] == '\r' && s[j + 2] == '\n') { j += 2; continue; }
        cc_buf_push_char(&b, s[j]);
    }
    j = 0;
    while (j < b.len && (b.data[j] == ' ' || b.data[j] == '\t')) j++;
    out = cc_arena_strndup(p->a, b.data + j, b.len - j);
    *len_out = b.len - j;
    cc_buf_free(&b);
    return out;
}

static int starts_word(const char *s, size_t n, const char *w) {
    size_t wl = strlen(w);
    if (n < wl || memcmp(s, w, wl) != 0) return 0;
    if (n == wl) return 1;
    return !((s[wl] >= 'a' && s[wl] <= 'z') || (s[wl] >= 'A' && s[wl] <= 'Z') || (s[wl] >= '0' && s[wl] <= '9') || s[wl] == '_');
}

typedef enum PpKind { PP_OTHER, PP_IF, PP_ELIF, PP_ELSE, PP_ENDIF } PpKind;

/* Classify a directive; for #if/#ifdef/#ifndef also decide statically when
 * possible: *val = 0 false, 1 true, -1 unknown. */
static PpKind pp_classify(const char *s, size_t n, int *val) {
    const char *q;
    size_t m;
    *val = -1;
    if (starts_word(s, n, "endif")) return PP_ENDIF;
    if (starts_word(s, n, "else")) return PP_ELSE;
    if (starts_word(s, n, "elif")) return PP_ELIF;
    if (starts_word(s, n, "ifdef") || starts_word(s, n, "ifndef")) {
        int neg = s[2] == 'n';
        q = s + (neg ? 6 : 5);
        m = n - (size_t)(q - s);
        while (m && (*q == ' ' || *q == '\t')) { q++; m--; }
        if (starts_word(q, m, "__cplusplus")) *val = neg ? 1 : 0;
        return PP_IF;
    }
    if (starts_word(s, n, "if")) {
        int neg = 0;
        q = s + 2;
        m = n - 2;
        while (m && (*q == ' ' || *q == '\t')) { q++; m--; }
        if (m && *q == '!') { neg = 1; q++; m--; while (m && (*q == ' ' || *q == '\t')) { q++; m--; } }
        if (starts_word(q, m, "defined")) {
            q += 7; m -= 7;
            while (m && (*q == ' ' || *q == '\t' || *q == '(')) { q++; m--; }
            if (starts_word(q, m, "__cplusplus")) {
                /* `defined(__cplusplus) && ...` is false; `!defined(...) || x` unknown */
                q += 11; m -= 11;
                while (m && (*q == ' ' || *q == '\t' || *q == ')')) { q++; m--; }
                if (!neg && (m == 0 || (m >= 2 && q[0] == '&' && q[1] == '&'))) *val = 0;
                else if (neg && m == 0) *val = 1;
            }
        } else if (starts_word(q, m, "__cplusplus")) {
            *val = 0;
        } else if (!neg && m == 1 && (*q == '0' || *q == '1')) {
            *val = *q - '0';
        } else if (neg && m == 1 && (*q == '0' || *q == '1')) {
            *val = *q == '0';
        }
        return PP_IF;
    }
    return PP_OTHER;
}

/* Skip tokens after an inactive directive up to (not including) the
 * `#else`/`#elif`/`#endif` that ends the region. Returns the index of the
 * last skipped token, or `i` when nothing was skipped. */
static uint32_t pp_skip_region(P *p, uint32_t i) {
    int depth = 0;
    uint32_t j = i + 1, last = i;
    for (; j < p->n; j++) {
        const CcToken *t = &p->t[j];
        if (t->kind == CC_TK_EOF) break;
        if (t->kind == CC_TK_PP) {
            size_t n;
            int v;
            const char *s = pp_body(p, j, &n);
            PpKind k = pp_classify(s, n, &v);
            if (k == PP_IF) depth++;
            else if (k == PP_ENDIF) { if (depth == 0) break; depth--; }
            else if ((k == PP_ELSE || k == PP_ELIF) && depth == 0) break;
        }
        last = j;
    }
    p->i = last + 1;
    return last;
}

/* Called for every PP token in a list position: maintains the `#if` stack
 * and skips inactive regions. `*last` receives the last token covered. */
static void pp_note(P *p, uint32_t i, uint32_t *last, int *skipped) {
    size_t n;
    int v;
    const char *s = pp_body(p, i, &n);
    PpKind k = pp_classify(s, n, &v);
    *last = i;
    *skipped = 0;
    if (p->i == i) adv(p);
    if (k == PP_IF) {
        PpCond *c = CC_NEW(p->a, PpCond);
        c->else_dead = v == 1;
        c->taken = v == 1;
        CC_LIST_PUSH(p->a, &p->pp, c);
        if (v == 0) { *last = pp_skip_region(p, i); *skipped = *last != i; }
    } else if (k == PP_ELIF || k == PP_ELSE) {
        PpCond *c = p->pp.n ? p->pp.items[p->pp.n - 1] : NULL;
        if (c && c->else_dead) { *last = pp_skip_region(p, i); *skipped = *last != i; }
        else if (c) {
            if (k == PP_ELIF) {
                /* the #if branch was inactive: this branch is active when
                 * its condition is not statically false */
                if (v == 0) { *last = pp_skip_region(p, i); *skipped = *last != i; }
                else c->else_dead = 1;
            } else c->else_dead = 1;
        }
    } else if (k == PP_ENDIF) {
        if (p->pp.n) p->pp.n--;
    }
}

/* ---- attributes ------------------------------------------------------ */

static CcAttr *attr_new(P *p, uint32_t first, CcName name, CcName value) {
    CcAttr *a = CC_NEW(p->a, CcAttr);
    a->span = span_from(p, first);
    a->name = name;
    a->value = value;
    return a;
}

static void attr_append(CcAttr **head, CcAttr *a) {
    CcAttr **link = head;
    if (!a) return;
    while (*link) link = &(*link)->next;
    *link = a;
}

static int at_attr_start(P *p) {
    Kw k = kw_of(p, p->i);
    return k == KW_ATTRIBUTE || k == KW_ATTRIBUTE2 || k == KW_ALIGNAS || k == KW_DECLSPEC || k == KW_ALIGNAS2;
}

/* `__attribute__((...))`, `_Alignas(...)`, `__declspec(...)`: the whole
 * balanced group. The value is the single identifier inside when the
 * argument is exactly one identifier (`unused`, `packed`). */
static CcAttr *parse_attr(P *p) {
    uint32_t first = p->i;
    Kw k = kw_of(p, p->i);
    CcName name = tok_name(p, p->i);
    CcName value = NULL;
    adv(p);
    if (!at_p(p, CC_P_LPAREN)) {
        err_here(p, "expected '(' after %s", name);
        return attr_new(p, first, name, NULL);
    }
    {
        uint32_t close = match_close(p, p->i);
        uint32_t inner = p->i + 1;
        if (k == KW_ATTRIBUTE || k == KW_ATTRIBUTE2) {
            if (tok_is_p(&p->t[inner], CC_P_LPAREN)) inner++;
        }
        if (p->t[inner].kind == CC_TK_IDENT &&
            (tok_is_p(&p->t[inner + 1], CC_P_RPAREN) || tok_is_p(&p->t[inner + 1], CC_P_LPAREN) ||
             tok_is_p(&p->t[inner + 1], CC_P_COMMA)))
            value = tok_name(p, inner);
        p->i = close;
        if (p->t[p->i].kind == CC_TK_EOF) err_at(p, first, "unterminated %s", name);
        else adv(p);
    }
    return attr_new(p, first, name, value);
}

/* Zero or more attributes; appended to *head. Also accepts a GCC asm label
 * `asm("sym")`. */
static void parse_attrs(P *p, CcAttr **head) {
    for (;;) {
        if (at_attr_start(p)) { attr_append(head, parse_attr(p)); continue; }
        {
            Kw k = kw_of(p, p->i);
            if ((k == KW_ASM || k == KW_ASM2 || k == KW_ASM3) && tok_is_p(tk(p, 1), CC_P_LPAREN)) {
                uint32_t first = p->i;
                adv(p);
                skip_balanced(p);
                attr_append(head, attr_new(p, first, p->n_asm, NULL));
                continue;
            }
        }
        break;
    }
}

/* `@tag:NAME` in a comment right before a declaration, and `@task` CCDoc
 * comments, become attributes of the declaration. */
static void collect_trivia_attrs(P *p, uint32_t tok, CcAttr **head) {
    const CcToken *t = &p->t[tok];
    const char *s = p->f->src + t->lead_off;
    const char *e = s + t->lead_len;
    const char *q = s;
    while (q < e) {
        if (q + 1 < e && q[0] == '/' && (q[1] == '*' || q[1] == '/')) {
            int block = q[1] == '*';
            const char *c = q + 2;
            const char *cend;
            if (block) {
                cend = c;
                while (cend + 1 < e && !(cend[0] == '*' && cend[1] == '/')) cend++;
            } else {
                cend = c;
                while (cend < e && *cend != '\n') cend++;
            }
            /* scan the comment for @tag: and @task */
            {
                const char *r = c;
                while (r < cend) {
                    if (*r == '@' && cend - r > 5 && memcmp(r, "@tag:", 5) == 0) {
                        const char *nm = r + 5, *ne = nm;
                        while (ne < cend && ((*ne >= 'a' && *ne <= 'z') || (*ne >= 'A' && *ne <= 'Z') ||
                                              (*ne >= '0' && *ne <= '9') || *ne == '_')) ne++;
                        if (ne > nm) {
                            CcAttr *a = CC_NEW(p->a, CcAttr);
                            a->span = span2(tok, tok);
                            a->name = p->n_tag;
                            a->value = cc_intern(p->in, nm, (size_t)(ne - nm));
                            attr_append(head, a);
                        }
                        r = ne;
                        continue;
                    }
                    if (*r == '@' && cend - r >= 5 && memcmp(r, "@task", 5) == 0 &&
                        (cend - r == 5 || r[5] == ' ' || r[5] == '\t' || r[5] == '\n' || r[5] == '\r')) {
                        CcAttr *a = CC_NEW(p->a, CcAttr);
                        a->span = span2(tok, tok);
                        a->name = p->n_task;
                        a->value = NULL;
                        attr_append(head, a);
                        r += 5;
                        continue;
                    }
                    r++;
                }
            }
            q = block ? cend + 2 : cend;
            continue;
        }
        q++;
    }
}

/* ======================================================================
 * 2. Types
 * ====================================================================== */

static CcExpr *parse_expr(P *p);
static CcExpr *parse_assign(P *p);
static CcExpr *parse_cond(P *p);
static CcExpr *parse_unary(P *p);
static CcStmt *parse_at_stmt(P *p);
static CcStmt *parse_block(P *p);
static CcStmt *parse_stmt(P *p);
static CcInit *parse_initializer(P *p);
static CcType *parse_type_name(P *p);
static int is_type_start(P *p, uint32_t i);

typedef struct Specs {
    uint32_t first;
    uint32_t flags;      /* CC_S_* / CC_Q_* / CC_F_* */
    CcType *type;        /* NULL when no type specifier was seen */
    CcAttr *attrs;
    int saw_any;         /* consumed at least one token */
    int saw_type;
} Specs;

static CcType *type_named(P *p, CcName name, uint32_t first) {
    CcType *t = cc_type_new(p->a, CC_T_NAMED, span_from(p, first));
    t->name = name;
    return t;
}

/* Builtin type keyword accumulation. */
typedef struct Builtin {
    int n_void, n_char, n_short, n_int, n_long, n_float, n_double, n_signed, n_unsigned, n_bool,
        n_complex, n_int128, n_float128;
} Builtin;

static int builtin_add(Builtin *b, Kw k) {
    switch (k) {
    case KW_VOID: b->n_void++; return 1;
    case KW_CHAR: b->n_char++; return 1;
    case KW_SHORT: b->n_short++; return 1;
    case KW_INT: b->n_int++; return 1;
    case KW_LONG: b->n_long++; return 1;
    case KW_FLOAT: b->n_float++; return 1;
    case KW_DOUBLE: b->n_double++; return 1;
    case KW_SIGNED: case KW_SIGNED2: case KW_SIGNED3: b->n_signed++; return 1;
    case KW_UNSIGNED: b->n_unsigned++; return 1;
    case KW_BOOL: b->n_bool++; return 1;
    case KW_COMPLEX: b->n_complex++; return 1;
    case KW_INT128: b->n_int128++; return 1;
    case KW_FLOAT128: b->n_float128++; return 1;
    default: return 0;
    }
}

static int builtin_any(const Builtin *b) {
    return b->n_void || b->n_char || b->n_short || b->n_int || b->n_long || b->n_float || b->n_double ||
           b->n_signed || b->n_unsigned || b->n_bool || b->n_complex || b->n_int128 || b->n_float128;
}

static CcName builtin_name(P *p, const Builtin *b) {
    char buf[64];
    buf[0] = 0;
    if (b->n_void) strcpy(buf, "void");
    else if (b->n_bool) strcpy(buf, "_Bool");
    else if (b->n_float128) strcpy(buf, "__float128");
    else {
        if (b->n_unsigned) strcat(buf, "unsigned ");
        else if (b->n_signed && b->n_char) strcat(buf, "signed ");
        if (b->n_char) strcat(buf, "char");
        else if (b->n_short) strcat(buf, "short");
        else if (b->n_int128) strcat(buf, "__int128");
        else if (b->n_long >= 2) strcat(buf, "long long");
        else if (b->n_long == 1 && b->n_double) strcat(buf, "long double");
        else if (b->n_long == 1) strcat(buf, "long");
        else if (b->n_float) strcat(buf, "float");
        else if (b->n_double) strcat(buf, "double");
        else strcat(buf, "int");
        if (b->n_complex) strcat(buf, " _Complex");
    }
    return cc_intern(p->in, buf, strlen(buf));
}

/* Does the token at `i` (an identifier) start a declarator, as seen after
 * an unknown identifier that might be a type name? */
static int declarator_follows(P *p, uint32_t i) {
    const CcToken *t = &p->t[i];
    if (t->kind == CC_TK_IDENT) {
        Kw k = kw_of(p, i);
        if (is_kw(p, i, KW_IN)) return 0;
        if (k == KW_NONE) return 1;
        /* `Foo const *p`, `Foo __attribute__((x)) y` */
        return k == KW_CONST || k == KW_CONST2 || k == KW_CONST3 || k == KW_VOLATILE || k == KW_VOLATILE2 ||
               k == KW_VOLATILE3 || k == KW_RESTRICT || k == KW_RESTRICT2 || k == KW_RESTRICT3 ||
               k == KW_ATTRIBUTE || k == KW_ATTRIBUTE2;
    }
    if (t->kind == CC_TK_PUNCT) {
        if (t->punct == CC_P_STAR) {
            /* `Foo * bar` is a declaration; `a * b` where a is a variable was
             * excluded by the caller. Require a declarator-ish continuation. */
            uint32_t j = i + 1;
            while (j < p->n && (tok_is_p(&p->t[j], CC_P_STAR) ||
                                (p->t[j].kind == CC_TK_IDENT && (kw_of(p, j) == KW_CONST || kw_of(p, j) == KW_RESTRICT ||
                                                                  kw_of(p, j) == KW_RESTRICT2 || kw_of(p, j) == KW_VOLATILE))))
                j++;
            return (p->t[j].kind == CC_TK_IDENT && kw_of(p, j) == KW_NONE && !is_kw(p, j, KW_IN)) ||
                   (tok_is_p(&p->t[j], CC_P_LPAREN) && (tok_is_p(&p->t[j + 1], CC_P_STAR) || tok_is_p(&p->t[j + 1], CC_P_LPAREN))) ||
                   tok_is_p(&p->t[j], CC_P_RPAREN) || tok_is_p(&p->t[j], CC_P_COMMA) ||
                   tok_is_p(&p->t[j], CC_P_LBRACKET) ||
                   ((tok_is_p(&p->t[j], CC_P_UNWRAP) || tok_is_p(&p->t[j], CC_P_UNWRAP_OR)) && tok_is_p(&p->t[j + 1], CC_P_LPAREN));
        }
        if (t->punct == CC_P_LBRACKET)
            return tok_is_p(&p->t[i + 1], CC_P_COLON) || tok_is_p(&p->t[i + 1], CC_P_COLONCOLON) || tok_is_p(&p->t[i + 1], CC_P_TILDE) ||
                   (p->t[i + 1].kind == CC_TK_NUMBER && tok_is_p(&p->t[i + 2], CC_P_COLON));
        if (t->punct == CC_P_LPAREN) {
            /* `T (*fp)(int)` / `T (*arr)[3]` versus `macro(*p == 1)` */
            uint32_t j = i + 1;
            if (!tok_is_p(&p->t[j], CC_P_STAR) && !tok_is_p(&p->t[j], CC_P_CARET)) return 0;
            while (tok_is_p(&p->t[j], CC_P_STAR) || tok_is_p(&p->t[j], CC_P_CARET) ||
                   (p->t[j].kind == CC_TK_IDENT && (kw_of(p, j) == KW_CONST || kw_of(p, j) == KW_RESTRICT || kw_of(p, j) == KW_RESTRICT2)))
                j++;
            if (p->t[j].kind != CC_TK_IDENT || kw_of(p, j) != KW_NONE) return 0;
            j++;
            if (!tok_is_p(&p->t[j], CC_P_RPAREN)) return 0;
            return tok_is_p(&p->t[j + 1], CC_P_LPAREN) || tok_is_p(&p->t[j + 1], CC_P_LBRACKET);
        }
        if (t->punct == CC_P_UNWRAP || t->punct == CC_P_UNWRAP_OR)
            return tok_is_p(&p->t[i + 1], CC_P_LPAREN);
        if (t->punct == CC_P_COLONCOLON) {
            uint32_t close;
            if (!tok_is_p(&p->t[i + 1], CC_P_LBRACKET)) return 0;
            close = match_close(p, i + 1);
            return !tok_is_p(&p->t[close + 1], CC_P_LPAREN) && !tok_is_p(&p->t[close + 1], CC_P_DOT);
        }
    }
    if (t->kind == CC_TK_AT_WORD) return tok_is_word(p, i, "typeview");
    return 0;
}

/* In `X Y ...` with X unknown: is Y the type (so X is a specifier macro)?
 * Y is the type when a declarator follows it, or when a keyword type follows
 * (`CC_A CC_B void f()`); `T name[:] = ...` keeps Y as the declarator. */
static int second_is_type(P *p, uint32_t y) {
    const CcToken *n = &p->t[y + 1];
    if (is_type_start(p, y + 1) && kw_of(p, y + 1) != KW_NONE) return 1;
    if (n->kind == CC_TK_IDENT) {
        const CcToken *after = &p->t[y + 2];
        if (kw_of(p, y + 1) != KW_NONE || is_kw(p, y + 1, KW_IN)) return 0;
        /* Z after Y: `X Y Z(` / `X Y Z;` / `X Y Z =` — Z is the declarator (it may
         * well be a name declared before, as a definition after its prototype) */
        if (after->kind == CC_TK_PUNCT &&
            (after->punct == CC_P_SEMI || after->punct == CC_P_ASSIGN || after->punct == CC_P_COMMA || after->punct == CC_P_LPAREN ||
             after->punct == CC_P_LBRACKET || after->punct == CC_P_RPAREN || after->punct == CC_P_COLON))
            return 1;
        if (name_is_var(p, tok_name(p, y + 1))) return 0;
        return second_is_type(p, y + 1) || declarator_follows(p, y + 2);
    }
    if (n->kind == CC_TK_PUNCT) {
        if (n->punct == CC_P_STAR) return declarator_follows(p, y + 1);
        if (n->punct == CC_P_LPAREN) return tok_is_p(&p->t[y + 2], CC_P_STAR) || tok_is_p(&p->t[y + 2], CC_P_LPAREN);
        if (n->punct == CC_P_UNWRAP || n->punct == CC_P_UNWRAP_OR) return tok_is_p(&p->t[y + 2], CC_P_LPAREN);
        if (n->punct == CC_P_COLONCOLON) return tok_is_p(&p->t[y + 2], CC_P_LBRACKET);
        if (n->punct == CC_P_LBRACKET) {
            uint32_t close = match_close(p, y + 1);
            return (tok_is_p(&p->t[y + 2], CC_P_COLON) || tok_is_p(&p->t[y + 2], CC_P_COLONCOLON) || tok_is_p(&p->t[y + 2], CC_P_TILDE) ||
                    (p->t[y + 2].kind == CC_TK_NUMBER && tok_is_p(&p->t[y + 3], CC_P_COLON))) &&
                   (p->t[close + 1].kind == CC_TK_IDENT || tok_is_p(&p->t[close + 1], CC_P_STAR));
        }
    }
    return 0;
}

/* Can the identifier at `i`, unknown to the scope stack, be taken as a type
 * name in specifier position? */
static int unknown_ident_is_type(P *p, uint32_t i) {
    CcName nm = tok_name(p, i);
    if (name_is_var(p, nm)) return 0;
    return declarator_follows(p, i + 1);
}

static CcType *parse_struct_or_union(P *p);
static CcType *parse_enum(P *p);
static void parse_params(P *p, CcType *fn);
static CcType *parse_generic_args(P *p, CcName family, uint32_t first);
static CcType *parse_type_or_value_arg(P *p);

/* `[:]` `[n:]` `[:!]` `[:0]` `[:0!]` after a type, at the `[`. */
static CcType *parse_slice_suffix(P *p, CcType *base, uint32_t first) {
    CcType *t = cc_type_new(p->a, CC_T_SLICE, span2(first, first));
    uint32_t open = p->i;
    int extra = 0;
    t->base = base;
    adv(p); /* [ */
    if (!at_p(p, CC_P_COLON) && !at_p(p, CC_P_COLONCOLON)) t->fixed = parse_cond(p);
    if (accept_p(p, CC_P_COLONCOLON)) extra = 1;
    else if (!expect_p(p, CC_P_COLON, "the slice type")) { sync(p, open); return t; }
    if (p->t[p->i].kind == CC_TK_NUMBER && cc_tok_is(p->f, &p->t[p->i], "0")) { t->sentinel = 1; adv(p); }
    if (accept_p(p, CC_P_BANG)) t->unique = 1;
    /* nested ranks: `T[::]` is `T[:][:]`, refinements sit on the innermost rank */
    for (;;) {
        if (accept_p(p, CC_P_COLONCOLON)) { extra += 2; continue; }
        if (accept_p(p, CC_P_COLON)) { extra++; continue; }
        break;
    }
    expect_close(p, CC_P_RBRACKET, "slice type", open);
    t->span = span_from(p, first);
    while (extra-- > 0) {
        CcType *outer = cc_type_new(p->a, CC_T_SLICE, t->span);
        outer->base = t;
        t = outer;
    }
    return t;
}

/* `[~cap topo ordered dir]` after a type, at the `[`. */
static CcType *parse_chan_suffix(P *p, CcType *base, uint32_t first) {
    CcType *t = cc_type_new(p->a, CC_T_CHAN, span2(first, first));
    uint32_t open = p->i;
    t->base = base;
    adv(p); /* [ */
    adv(p); /* ~ */
    /* capacity: anything up to a topology / direction / ordered */
    if (!at_p(p, CC_P_LT) && !at_p(p, CC_P_GT) && !at_p(p, CC_P_RBRACKET) &&
        !(at_ident(p) && (cc_tok_is(p->f, &p->t[p->i], "ordered") || cc_tok_is(p->f, &p->t[p->i], "sync") ||
                          cc_tok_is(p->f, &p->t[p->i], "async") ||
                          (p->t[p->i].len == 1 && tok_is_p(tk(p, 1), CC_P_COLON))))) {
        t->cap = parse_unary(p);
    }
    /* topology `1:1`, `N:1`, `1:N`, `N:N` */
    if ((at_ident(p) || p->t[p->i].kind == CC_TK_NUMBER) && tok_is_p(tk(p, 1), CC_P_COLON) &&
        (tk(p, 2)->kind == CC_TK_IDENT || tk(p, 2)->kind == CC_TK_NUMBER)) {
        const CcToken *a = &p->t[p->i], *b = tk(p, 2);
        t->topology = cc_intern(p->in, p->f->src + a->off, (size_t)(b->off + b->len - a->off));
        adv(p); adv(p); adv(p);
    }
    for (;;) {
        if (at_ident(p) && cc_tok_is(p->f, &p->t[p->i], "ordered")) { t->ordered = 1; adv(p); continue; }
        if (at_ident(p) && cc_tok_is(p->f, &p->t[p->i], "sync")) { t->sync = 1; adv(p); continue; }
        if (at_ident(p) && cc_tok_is(p->f, &p->t[p->i], "async")) { t->sync = 0; adv(p); continue; }
        break;
    }
    if (at_ident(p) && cc_tok_is(p->f, &p->t[p->i], "owned")) {
        t->owned = 1;
        adv(p);
        if (at_p(p, CC_P_LBRACE)) t->chan_hooks = parse_initializer(p);
    }
    if (at_p(p, CC_P_LT)) { t->dir = '<'; adv(p); }
    else if (at_p(p, CC_P_GT)) { t->dir = '>'; adv(p); }
    else if (!t->owned) err_here(p, "expected '<' or '>' to give the channel type its direction, found %s", tok_desc(p, p->i));
    if (accept_p(p, CC_P_COMMA)) {
        if (at_ident(p)) { t->chan_policy = tok_name(p, p->i); adv(p); }
        else err_here(p, "expected a drop policy name after ',' in the channel type, found %s", tok_desc(p, p->i));
    }
    expect_close(p, CC_P_RBRACKET, "channel type", open);
    t->span = span_from(p, first);
    return t;
}

/* `!>(E)` / `?>(E)` after a type. */
static CcType *parse_result_suffix(P *p, CcType *base, uint32_t first) {
    CcType *t = cc_type_new(p->a, CC_T_RESULT, span2(first, first));
    uint32_t open;
    t->base = base;
    t->optional = at_p(p, CC_P_UNWRAP_OR);
    adv(p);
    open = p->i;
    if (!expect_p(p, CC_P_LPAREN, t->optional ? "the ?>(E) type" : "the !>(E) type")) return t;
    t->err = parse_type_name(p);
    expect_close(p, CC_P_RPAREN, "error type", open);
    t->span = span_from(p, first);
    return t;
}

/* Is a CC type suffix (`!>(`, `?>(`, `[:`, `[~`, `[n:`) at `i`, possibly
 * after a run of `*` / qualifiers that then belong to the value type? */
static int cc_suffix_at(P *p, uint32_t i, uint32_t *after_ptrs) {
    uint32_t j = i;
    while (j < p->n && (tok_is_p(&p->t[j], CC_P_STAR) ||
                        (p->t[j].kind == CC_TK_IDENT && (kw_of(p, j) == KW_CONST || kw_of(p, j) == KW_VOLATILE ||
                                                          kw_of(p, j) == KW_RESTRICT || kw_of(p, j) == KW_RESTRICT2 ||
                                                          kw_of(p, j) == KW_RESTRICT3))))
        j++;
    if (after_ptrs) *after_ptrs = j;
    if ((tok_is_p(&p->t[j], CC_P_UNWRAP) || tok_is_p(&p->t[j], CC_P_UNWRAP_OR)) && tok_is_p(&p->t[j + 1], CC_P_LPAREN))
        return 1;
    if (tok_is_p(&p->t[j], CC_P_LBRACKET)) {
        if (tok_is_p(&p->t[j + 1], CC_P_COLON) || tok_is_p(&p->t[j + 1], CC_P_COLONCOLON) || tok_is_p(&p->t[j + 1], CC_P_TILDE)) return 1;
        if (p->t[j + 1].kind == CC_TK_NUMBER && tok_is_p(&p->t[j + 2], CC_P_COLON)) return 1;
        if (p->t[j + 1].kind == CC_TK_IDENT && tok_is_p(&p->t[j + 2], CC_P_COLON) && tok_is_p(&p->t[j + 3], CC_P_RBRACKET)) return 1;
    }
    return 0;
}

/* Wrap `base` in the CC suffixes that follow it (with pointer absorption). */
static CcType *parse_cc_suffixes(P *p, CcType *base, uint32_t first) {
    uint32_t after;
    while (cc_suffix_at(p, p->i, &after)) {
        while (p->i < after) {
            if (at_p(p, CC_P_STAR)) {
                CcType *pt = cc_type_new(p->a, CC_T_POINTER, span2(first, p->i));
                pt->base = base;
                base = pt;
                adv(p);
            } else {
                Kw k = kw_of(p, p->i);
                if (k == KW_CONST) base->quals |= CC_Q_CONST;
                else if (k == KW_VOLATILE) base->quals |= CC_Q_VOLATILE;
                else base->quals |= CC_Q_RESTRICT;
                base->span.last = p->i;
                adv(p);
            }
        }
        if (at_p(p, CC_P_LBRACKET)) {
            if (tok_is_p(tk(p, 1), CC_P_TILDE)) base = parse_chan_suffix(p, base, first);
            else base = parse_slice_suffix(p, base, first);
        } else base = parse_result_suffix(p, base, first);
    }
    return base;
}

/* Declaration specifiers. `force_type`: an unknown identifier is a type
 * name regardless of what follows (abstract contexts: casts, sizeof,
 * _Generic arms, parameters). */
static void parse_specs(P *p, Specs *S, int force_type) {
    Builtin b;
    uint32_t bfirst = 0;
    memset(S, 0, sizeof *S);
    memset(&b, 0, sizeof b);
    S->first = p->i;
    for (;;) {
        const CcToken *t = &p->t[p->i];
        Kw k;
        if (t->kind == CC_TK_AT_WORD) {
            if (at_word(p, "async")) { S->flags |= CC_F_ASYNC; adv(p); S->saw_any = 1; continue; }
            if (at_word(p, "blocking")) { S->flags |= CC_F_BLOCKING; adv(p); S->saw_any = 1; continue; }
            if (at_word(p, "nonblocking") || at_word(p, "noblock")) { S->flags |= CC_F_NONBLOCKING; adv(p); S->saw_any = 1; continue; }
            if (at_word(p, "latency_sensitive")) { S->flags |= CC_F_LATENCY_SENSITIVE; adv(p); S->saw_any = 1; continue; }
            if (at_word(p, "typeview") && tok_is_p(tk(p, 1), CC_P_LPAREN)) {
                uint32_t first = p->i;
                CcName v = NULL;
                adv(p); adv(p);
                if (at_ident(p)) { v = tok_name(p, p->i); adv(p); }
                else err_here(p, "expected the view name in @typeview(Name)");
                expect_close(p, CC_P_RPAREN, "@typeview(Name)", first + 1);
                attr_append(&S->attrs, attr_new(p, first, p->n_typeview, v));
                S->saw_any = 1;
                continue;
            }
            if (at_word(p, "auto") && tok_is_p(tk(p, 1), CC_P_LPAREN) && !S->saw_type) {
                uint32_t first = p->i, open = p->i + 1;
                CcType *t2 = cc_type_new(p->a, CC_T_AUTO, span2(first, first));
                adv(p); adv(p);
                t2->typeof_expr = parse_expr(p);
                expect_close(p, CC_P_RPAREN, "@auto(src)", open);
                t2->span = span_from(p, first);
                S->type = t2;
                S->saw_type = S->saw_any = 1;
                continue;
            }
            break;
        }
        if (t->kind == CC_TK_PP && S->saw_any && !S->saw_type) {
            /* a preprocessor line among the specifiers (`#if` around an attribute) */
            uint32_t first = p->i, last; int skipped;
            pp_note(p, p->i, &last, &skipped);
            attr_append(&S->attrs, attr_new(p, first, cc_intern(p->in, "#", 1), NULL));
            continue;
        }
        if (t->kind != CC_TK_IDENT) break;
        k = kw_of(p, p->i);
        switch (k) {
        case KW_TYPEDEF: S->flags |= CC_S_TYPEDEF; adv(p); S->saw_any = 1; continue;
        case KW_EXTERN: S->flags |= CC_S_EXTERN; adv(p); S->saw_any = 1; continue;
        case KW_STATIC: S->flags |= CC_S_STATIC; adv(p); S->saw_any = 1; continue;
        case KW_AUTO: S->flags |= CC_S_AUTO; adv(p); S->saw_any = 1; continue;
        case KW_REGISTER: S->flags |= CC_S_REGISTER; adv(p); S->saw_any = 1; continue;
        case KW_THREAD_LOCAL: case KW_THREAD: S->flags |= CC_S_THREAD; adv(p); S->saw_any = 1; continue;
        case KW_CONST: case KW_CONST2: case KW_CONST3: S->flags |= CC_Q_CONST; adv(p); S->saw_any = 1; continue;
        case KW_VOLATILE: case KW_VOLATILE2: case KW_VOLATILE3: S->flags |= CC_Q_VOLATILE; adv(p); S->saw_any = 1; continue;
        case KW_RESTRICT: case KW_RESTRICT2: case KW_RESTRICT3: S->flags |= CC_Q_RESTRICT; adv(p); S->saw_any = 1; continue;
        case KW_INLINE: case KW_INLINE2: case KW_INLINE3: case KW_FORCEINLINE: S->flags |= CC_F_INLINE; adv(p); S->saw_any = 1; continue;
        case KW_NORETURN: S->flags |= CC_F_NORETURN; adv(p); S->saw_any = 1; continue;
        case KW_EXTENSION: adv(p); S->saw_any = 1; continue;
        case KW_ATTRIBUTE: case KW_ATTRIBUTE2: case KW_ALIGNAS: case KW_DECLSPEC: case KW_ALIGNAS2:
            attr_append(&S->attrs, parse_attr(p)); S->saw_any = 1; continue;
        case KW_ATOMIC:
            if (tok_is_p(tk(p, 1), CC_P_LPAREN) && !S->saw_type) {
                uint32_t first = p->i, open = p->i + 1;
                CcType *t2 = cc_type_new(p->a, CC_T_ATOMIC, span2(first, first));
                adv(p); adv(p);
                t2->base = parse_type_name(p);
                expect_close(p, CC_P_RPAREN, "_Atomic(type)", open);
                t2->span = span_from(p, first);
                S->type = t2;
                S->saw_type = S->saw_any = 1;
                continue;
            }
            S->flags |= CC_Q_ATOMIC; adv(p); S->saw_any = 1; continue;
        case KW_STRUCT: case KW_UNION:
            if (S->saw_type) break;
            S->type = parse_struct_or_union(p);
            S->saw_type = S->saw_any = 1;
            continue;
        case KW_ENUM:
            if (S->saw_type) break;
            S->type = parse_enum(p);
            S->saw_type = S->saw_any = 1;
            continue;
        case KW_TYPEOF: case KW_TYPEOF2: case KW_TYPEOF3: {
            uint32_t first = p->i, open = p->i + 1;
            CcType *t2;
            if (S->saw_type) break;
            t2 = cc_type_new(p->a, CC_T_TYPEOF, span2(first, first));
            adv(p);
            if (!expect_p(p, CC_P_LPAREN, "typeof")) { S->type = t2; S->saw_type = S->saw_any = 1; continue; }
            if (is_type_start(p, p->i)) t2->typeof_type = parse_type_name(p);
            else t2->typeof_expr = parse_expr(p);
            expect_close(p, CC_P_RPAREN, "typeof", open);
            t2->span = span_from(p, first);
            S->type = t2;
            S->saw_type = S->saw_any = 1;
            continue;
        }
        case KW_BUILTIN_VA_LIST: case KW_NULLPTR_T:
            if (S->saw_type) break;
            S->type = type_named(p, tok_name(p, p->i), p->i);
            adv(p);
            S->type->span = span_from(p, S->type->span.first);
            S->saw_type = S->saw_any = 1;
            continue;
        default: break;
        }
        if (builtin_add(&b, k)) {
            if (S->saw_type && !builtin_any(&b)) break;
            if (!bfirst) bfirst = p->i;
            if (!S->saw_type) {
                S->type = cc_type_new(p->a, CC_T_NAMED, span2(p->i, p->i));
                bfirst = p->i;
            }
            adv(p);
            S->type->span = span_from(p, bfirst);
            S->type->name = builtin_name(p, &b);
            S->saw_type = S->saw_any = 1;
            continue;
        }
        if (k != KW_NONE) break;
        /* an identifier: typedef name, generic family, or the declarator */
        if (S->saw_type) break;
        {
            CcName nm = tok_name(p, p->i);
            uint32_t first = p->i;
            int known = name_is_type(p, nm);
            if (known && name_is_all_caps(nm) && tok_is_p(tk(p, 1), CC_P_LPAREN) && !tok_is_p(tk(p, 2), CC_P_STAR) &&
                !tok_is_p(tk(p, 2), CC_P_LPAREN) && !tok_is_p(tk(p, 2), CC_P_RPAREN))
                known = 0; /* `CC_CAT_3(a, b, c) name`: a macro-made type, not the recorded name */
            if (!known) {
                /* `m.ret x`: a comptime reflection member used as a type */
                if (tok_is_p(tk(p, 1), CC_P_DOT) && tk(p, 2)->kind == CC_TK_IDENT && tk(p, 3)->kind == CC_TK_IDENT &&
                    kw_of(p, p->i + 3) == KW_NONE && !name_is_type(p, nm)) {
                    CcType *t2 = cc_type_new(p->a, CC_T_TYPEOF, span2(first, first));
                    CcExpr *m = cc_expr_new(p->a, CC_E_IDENT, span2(first, first));
                    m->name = nm; m->tok = first;
                    adv(p);
                    while (at_p(p, CC_P_DOT) && tk(p, 1)->kind == CC_TK_IDENT) {
                        CcExpr *x = cc_expr_new(p->a, CC_E_MEMBER, span2(first, first));
                        adv(p);
                        x->a = m; x->name = tok_name(p, p->i); adv(p);
                        x->span = span_from(p, first);
                        m = x;
                    }
                    t2->typeof_expr = m;
                    t2->span = span_from(p, first);
                    S->type = t2;
                    S->saw_type = S->saw_any = 1;
                    continue;
                }
                if (name_is_var(p, nm)) break;
                /* `NAME(args) declarator`: a macro that expands to a type */
                if (tok_is_p(tk(p, 1), CC_P_LPAREN) && !tok_is_p(tk(p, 2), CC_P_STAR) && !tok_is_p(tk(p, 2), CC_P_LPAREN)) {
                    uint32_t close = match_close(p, p->i + 1);
                    const CcToken *after = &p->t[close + 1];
                    int is_macro_type = (after->kind == CC_TK_IDENT && kw_of(p, close + 1) == KW_NONE && !after->at_line_start &&
                                         !is_kw(p, close + 1, KW_IN)) ||
                                        (tok_is_p(after, CC_P_STAR) && (p->t[close + 2].kind == CC_TK_IDENT || tok_is_p(&p->t[close + 2], CC_P_STAR)) && !after->at_line_start) ||
                                        (force_type && (tok_is_p(after, CC_P_RPAREN) || tok_is_p(after, CC_P_COMMA) || tok_is_p(after, CC_P_STAR) ||
                                                        tok_is_p(after, CC_P_RBRACKET)));
                    if (!is_macro_type) break;
                    {
                        CcType *t2 = cc_type_new(p->a, CC_T_MACRO, span2(first, first));
                        uint32_t open = p->i + 1;
                        t2->name = nm;
                        adv(p); adv(p);
                        while (!at_p(p, CC_P_RPAREN) && !at_eof(p)) {
                            CcType *arg = parse_type_or_value_arg(p);
                            CC_LIST_PUSH(p->a, &t2->args, arg);
                            if (!accept_p(p, CC_P_COMMA)) break;
                        }
                        expect_close(p, CC_P_RPAREN, "macro type argument list", open);
                        t2->span = span_from(p, first);
                        S->type = t2;
                        S->saw_type = S->saw_any = 1;
                        continue;
                    }
                }
                /* `static_inline void f()` / `local voidpf f()` / `CC_A CC_B void f()`: a macro
                 * used as a specifier; the next identifier must itself be a type that is followed by a
                 * declarator (so `uint16_t u16[:] = ...` keeps u16 as the declarator) */
                if ((is_type_start(p, p->i + 1) && kw_of(p, p->i + 1) != KW_NONE) ||
                    (tk(p, 1)->kind == CC_TK_AT_WORD && (tok_is_word(p, p->i + 1, "async") ||
                    tok_is_word(p, p->i + 1, "blocking") || tok_is_word(p, p->i + 1, "nonblocking") || tok_is_word(p, p->i + 1, "noblock"))) ||
                    (!force_type && tk(p, 1)->kind == CC_TK_IDENT && kw_of(p, p->i + 1) == KW_NONE && !name_is_var(p, tok_name(p, p->i + 1)) &&
                     !is_kw(p, p->i + 1, KW_IN) && second_is_type(p, p->i + 1))) {
                    Kw nk = kw_of(p, p->i + 1);
                    if (nk != KW_ATTRIBUTE && nk != KW_ATTRIBUTE2) {
                        attr_append(&S->attrs, attr_new(p, first, nm, NULL));
                        adv(p);
                        S->attrs->span = span2(first, first);
                        S->saw_any = 1;
                        continue;
                    }
                }
                if (!force_type && !unknown_ident_is_type(p, p->i)) break;
                /* `name::[...]` is a generic type (or call: the caller checked) */
                assume_type(p, nm);
            }
            adv(p);
            if (at_p2(p, CC_P_COLONCOLON, CC_P_LBRACKET)) S->type = parse_generic_args(p, nm, first);
            else S->type = type_named(p, nm, first);
            S->saw_type = S->saw_any = 1;
            continue;
        }
    }
    if (S->type) {
        if (S->flags & (CC_Q_CONST | CC_Q_VOLATILE | CC_Q_RESTRICT | CC_Q_ATOMIC)) {
            S->type->quals |= S->flags & (CC_Q_CONST | CC_Q_VOLATILE | CC_Q_RESTRICT | CC_Q_ATOMIC);
        }
        S->type = parse_cc_suffixes(p, S->type, S->type->span.first);
        /* trailing qualifiers after the type: `Foo const *p` */
        for (;;) {
            Kw k = kw_of(p, p->i);
            if (k == KW_CONST || k == KW_CONST2 || k == KW_CONST3) { S->type->quals |= CC_Q_CONST; adv(p); continue; }
            if (k == KW_VOLATILE || k == KW_VOLATILE2 || k == KW_VOLATILE3) { S->type->quals |= CC_Q_VOLATILE; adv(p); continue; }
            if (k == KW_RESTRICT || k == KW_RESTRICT2 || k == KW_RESTRICT3) { S->type->quals |= CC_Q_RESTRICT; adv(p); continue; }
            if (k == KW_ATTRIBUTE || k == KW_ATTRIBUTE2) { attr_append(&S->attrs, parse_attr(p)); continue; }
            break;
        }
        if (S->attrs && !S->type->attrs) {
            /* a @typeview(Name) qualifier belongs to the type */
            CcAttr *a;
            for (a = S->attrs; a; a = a->next)
                if (a->name == p->n_typeview) {
                    CcAttr *copy = CC_NEW(p->a, CcAttr);
                    *copy = *a;
                    copy->next = NULL;
                    attr_append(&S->type->attrs, copy);
                }
        }
    }
}

/* An argument of a generic or macro type: a type, or a value (CC_T_VALUE). */
static CcType *parse_type_or_value_arg(P *p) {
    if (is_type_start(p, p->i) ||
        (at_ident(p) && kw_of(p, p->i) == KW_NONE && !name_is_var(p, tok_name(p, p->i)) &&
         (tok_is_p(tk(p, 1), CC_P_COMMA) || tok_is_p(tk(p, 1), CC_P_RPAREN) || tok_is_p(tk(p, 1), CC_P_RBRACKET) ||
          tok_is_p(tk(p, 1), CC_P_STAR) || tok_is_p(tk(p, 1), CC_P_LBRACKET) || at_p2(p, CC_P_COLONCOLON, CC_P_LBRACKET))))
        return parse_type_name(p);
    {
        uint32_t af = p->i;
        CcType *arg = cc_type_new(p->a, CC_T_VALUE, span2(af, af));
        arg->value = parse_cond(p);
        arg->span = span_from(p, af);
        return arg;
    }
}

/* `Name::[T, U]` at the `::`. */
static CcType *parse_generic_args(P *p, CcName family, uint32_t first) {
    CcType *t = cc_type_new(p->a, CC_T_GENERIC, span2(first, first));
    uint32_t open;
    t->name = family;
    adv(p); /* :: */
    open = p->i;
    adv(p); /* [ */
    while (!at_p(p, CC_P_RBRACKET) && !at_eof(p)) {
        CcType *arg = parse_type_or_value_arg(p);
        CC_LIST_PUSH(p->a, &t->args, arg);
        if (!accept_p(p, CC_P_COMMA)) break;
    }
    expect_close(p, CC_P_RBRACKET, "generic argument list", open);
    t->span = span_from(p, first);
    return t;
}

/* Does a type name start at token `i`? (keywords, known typedef names,
 * qualifiers, `struct` ...) */
static int is_type_start(P *p, uint32_t i) {
    const CcToken *t = &p->t[i];
    Kw k;
    if (t->kind == CC_TK_AT_WORD) return tok_is_word(p, i, "typeview") || tok_is_word(p, i, "auto");
    if (t->kind != CC_TK_IDENT) return 0;
    k = kw_of(p, i);
    switch (k) {
    case KW_VOID: case KW_CHAR: case KW_SHORT: case KW_INT: case KW_LONG: case KW_FLOAT: case KW_DOUBLE:
    case KW_SIGNED: case KW_SIGNED2: case KW_SIGNED3: case KW_UNSIGNED: case KW_BOOL: case KW_COMPLEX:
    case KW_STRUCT: case KW_UNION: case KW_ENUM: case KW_CONST: case KW_CONST2: case KW_CONST3:
    case KW_VOLATILE: case KW_VOLATILE2: case KW_VOLATILE3: case KW_RESTRICT: case KW_RESTRICT2:
    case KW_RESTRICT3: case KW_ATOMIC: case KW_TYPEOF: case KW_TYPEOF2: case KW_TYPEOF3:
    case KW_TYPEDEF: case KW_EXTERN: case KW_STATIC: case KW_AUTO: case KW_REGISTER: case KW_THREAD_LOCAL:
    case KW_THREAD: case KW_INLINE: case KW_INLINE2: case KW_INLINE3: case KW_FORCEINLINE: case KW_NORETURN:
    case KW_EXTENSION: case KW_ATTRIBUTE: case KW_ATTRIBUTE2: case KW_ALIGNAS: case KW_DECLSPEC:
    case KW_INT128: case KW_BUILTIN_VA_LIST: case KW_FLOAT128: case KW_NULLPTR_T: case KW_ALIGNAS2:
        return 1;
    case KW_NONE: {
        CcName nm = tok_name(p, i);
        return name_is_type(p, nm);
    }
    default: return 0;
    }
}

/* Abstract-declarator aware declarator parse. Returns the full type; the
 * name (if any) goes to *name_out with its token. */
static CcType *parse_declarator(P *p, CcType *base, int abstract_ok, CcName *name_out, uint32_t *name_tok, CcAttr **attrs);

typedef struct Suffix {
    CcType *t;            /* an ARRAY / FUNC / SLICE / CHAN shell with base unset */
} Suffix;

static void apply_quals_to(CcType *t, uint32_t q) { t->quals |= q; }

static CcType *parse_declarator(P *p, CcType *base, int abstract_ok, CcName *name_out, uint32_t *name_tok, CcAttr **attrs) {
    uint32_t first = p->i;
    CcType *t = base;
    uint32_t nested_start = 0, nested_end = 0;
    CC_LIST(CcType) suffixes;
    size_t k;
    memset(&suffixes, 0, sizeof suffixes);
    if (name_out) *name_out = NULL;
    /* pointers */
    for (;;) {
        if (at_p(p, CC_P_STAR)) {
            CcType *pt = cc_type_new(p->a, CC_T_POINTER, span2(first, p->i));
            pt->base = t;
            t = pt;
            adv(p);
            continue;
        }
        if (at_p(p, CC_P_CARET)) { /* blocks extension: treat as a pointer */
            CcType *pt = cc_type_new(p->a, CC_T_POINTER, span2(first, p->i));
            pt->base = t;
            t = pt;
            adv(p);
            continue;
        }
        {
            Kw kw = kw_of(p, p->i);
            if (kw == KW_CONST || kw == KW_CONST2 || kw == KW_CONST3) { apply_quals_to(t, CC_Q_CONST); adv(p); continue; }
            if (kw == KW_VOLATILE || kw == KW_VOLATILE2 || kw == KW_VOLATILE3) { apply_quals_to(t, CC_Q_VOLATILE); adv(p); continue; }
            if (kw == KW_RESTRICT || kw == KW_RESTRICT2 || kw == KW_RESTRICT3) { apply_quals_to(t, CC_Q_RESTRICT); adv(p); continue; }
            if (kw == KW_ATOMIC && !tok_is_p(tk(p, 1), CC_P_LPAREN)) { apply_quals_to(t, CC_Q_ATOMIC); adv(p); continue; }
            if (kw == KW_ATTRIBUTE || kw == KW_ATTRIBUTE2) { if (attrs) attr_append(attrs, parse_attr(p)); else parse_attr(p); continue; }
            if (kw == KW_EXTENSION) { adv(p); continue; }
        }
        break;
    }
    /* the name, or a nested declarator */
    if (at_p(p, CC_P_LPAREN)) {
        const CcToken *n1 = tk(p, 1);
        int nested = tok_is_p(n1, CC_P_STAR) || tok_is_p(n1, CC_P_LPAREN) || tok_is_p(n1, CC_P_CARET) ||
                     (n1->kind == CC_TK_IDENT && (kw_of(p, p->i + 1) == KW_ATTRIBUTE || kw_of(p, p->i + 1) == KW_ATTRIBUTE2));
        if (!nested && n1->kind == CC_TK_IDENT && kw_of(p, p->i + 1) == KW_NONE && !name_is_type(p, tok_name(p, p->i + 1)) &&
            tok_is_p(tk(p, 2), CC_P_RPAREN) && !abstract_ok)
            nested = 1;
        if (nested) {
            nested_start = p->i;
            nested_end = match_close(p, p->i);
            p->i = nested_end;
            if (p->t[p->i].kind == CC_TK_EOF) { err_at(p, nested_start, "unterminated parenthesised declarator"); return t; }
            adv(p);
        }
    }
    if (!nested_start && at_ident(p) && kw_of(p, p->i) == KW_NONE &&
        !(abstract_ok && name_is_type(p, tok_name(p, p->i)) &&
          !(tok_is_p(tk(p, 1), CC_P_COMMA) || tok_is_p(tk(p, 1), CC_P_RPAREN) || tok_is_p(tk(p, 1), CC_P_ASSIGN) ||
            tok_is_p(tk(p, 1), CC_P_LBRACKET)))) {
        if (name_out) *name_out = tok_name(p, p->i);
        if (name_tok) *name_tok = p->i;
        adv(p);
    } else if (!nested_start && !abstract_ok) {
        if (at_ident(p) && kw_of(p, p->i) != KW_NONE)
            err_here(p, "'%.*s' is a keyword and cannot be a declarator name", (int)p->t[p->i].len, p->f->src + p->t[p->i].off);
        else
            err_here(p, "expected a name in the declarator, found %s", tok_desc(p, p->i));
        return t;
    }
    /* suffixes */
    for (;;) {
        if (at_p(p, CC_P_LBRACKET)) {
            uint32_t open = p->i;
            const CcToken *n1 = tk(p, 1);
            CcType *s;
            if (tok_is_p(n1, CC_P_TILDE)) { s = parse_chan_suffix(p, NULL, open); CC_LIST_PUSH(p->a, &suffixes, s); continue; }
            if (tok_is_p(n1, CC_P_COLON) || tok_is_p(n1, CC_P_COLONCOLON) || (n1->kind == CC_TK_NUMBER && tok_is_p(tk(p, 2), CC_P_COLON))) {
                s = parse_slice_suffix(p, NULL, open); CC_LIST_PUSH(p->a, &suffixes, s); continue;
            }
            s = cc_type_new(p->a, CC_T_ARRAY, span2(open, open));
            adv(p);
            for (;;) {
                Kw kw = kw_of(p, p->i);
                if (kw == KW_STATIC) { s->array_static = 1; adv(p); continue; }
                if (kw == KW_CONST || kw == KW_CONST2 || kw == KW_CONST3) { s->quals |= CC_Q_CONST; adv(p); continue; }
                if (kw == KW_VOLATILE) { s->quals |= CC_Q_VOLATILE; adv(p); continue; }
                if (kw == KW_RESTRICT || kw == KW_RESTRICT2 || kw == KW_RESTRICT3) { s->quals |= CC_Q_RESTRICT; adv(p); continue; }
                break;
            }
            if (at_p(p, CC_P_STAR) && tok_is_p(tk(p, 1), CC_P_RBRACKET)) { s->array_star = 1; adv(p); }
            else if (!at_p(p, CC_P_RBRACKET)) {
                s->size = parse_assign(p);
                if (at_p(p, CC_P_COLON)) {
                    /* `T name[n:]` written after the name */
                    CcType *sl = cc_type_new(p->a, CC_T_SLICE, span2(open, open));
                    sl->fixed = s->size;
                    adv(p);
                    if (p->t[p->i].kind == CC_TK_NUMBER && cc_tok_is(p->f, &p->t[p->i], "0")) { sl->sentinel = 1; adv(p); }
                    if (accept_p(p, CC_P_BANG)) sl->unique = 1;
                    expect_close(p, CC_P_RBRACKET, "slice type", open);
                    sl->span = span_from(p, open);
                    CC_LIST_PUSH(p->a, &suffixes, sl);
                    continue;
                }
            }
            expect_close(p, CC_P_RBRACKET, "array declarator", open);
            s->span = span_from(p, open);
            CC_LIST_PUSH(p->a, &suffixes, s);
            continue;
        }
        if (at_p(p, CC_P_LPAREN)) {
            CcType *fn = cc_type_new(p->a, CC_T_FUNC, span2(p->i, p->i));
            parse_params(p, fn);
            fn->span = span_from(p, fn->span.first);
            CC_LIST_PUSH(p->a, &suffixes, fn);
            continue;
        }
        break;
    }
    /* build: suffixes apply right to left onto the pointer type */
    for (k = suffixes.n; k > 0; k--) {
        CcType *s = suffixes.items[k - 1];
        CcType *inner = s;
        while (inner->kind == CC_T_SLICE && inner->base) inner = inner->base; /* nested `[::]` ranks */
        inner->base = t;
        s->span.first = first;
        t = s;
    }
    /* a nested declarator wraps around everything so far */
    if (nested_start) {
        uint32_t save = p->i;
        p->i = nested_start + 1;
        t = parse_declarator(p, t, abstract_ok, name_out, name_tok, attrs);
        if (p->i != nested_end)
            err_here(p, "unexpected %s inside the parenthesised declarator", tok_desc(p, p->i));
        p->i = save;
    }
    if (t != base) t->span = span_from(p, first);
    return t;
}

/* A type name: specifiers plus an abstract declarator. */
static CcType *parse_type_name(P *p) {
    Specs S;
    CcType *t;
    uint32_t first = p->i;
    parse_specs(p, &S, 1);
    if (!S.type) {
        err_here(p, "expected a type, found %s", tok_desc(p, p->i));
        t = type_named(p, cc_intern(p->in, "int", 3), first);
        return t;
    }
    t = parse_declarator(p, S.type, 1, NULL, NULL, &S.attrs);
    if (S.attrs && t) attr_append(&t->attrs, S.attrs);
    return t;
}

/* Parameter list at `(`. */
static void parse_params(P *p, CcType *fn) {
    uint32_t open = p->i;
    adv(p);
    if (accept_p(p, CC_P_RPAREN)) { fn->has_prototype = 0; return; }
    fn->has_prototype = 1;
    if (at_kw(p, KW_VOID) && tok_is_p(tk(p, 1), CC_P_RPAREN)) { adv(p); adv(p); return; }
    for (;;) {
        CcParam *pa = CC_NEW(p->a, CcParam);
        uint32_t first = p->i;
        if (p->t[p->i].kind == CC_TK_PP) {
            uint32_t last; int skipped;
            pa->is_pp = 1;
            pa->pp_tok = p->i;
            pp_note(p, p->i, &last, &skipped);
            pa->span = span2(first, last);
            CC_LIST_PUSH(p->a, &fn->params, pa);
            if (at_p(p, CC_P_RPAREN)) break;
            continue;
        }
        if (at_p(p, CC_P_ELLIPSIS)) {
            pa->is_variadic = 1;
            adv(p);
            pa->span = span_from(p, first);
            CC_LIST_PUSH(p->a, &fn->params, pa);
        } else if (at_ident(p) && kw_of(p, p->i) == KW_NONE && !name_is_type(p, tok_name(p, p->i)) &&
                   (tok_is_p(tk(p, 1), CC_P_COMMA) || tok_is_p(tk(p, 1), CC_P_RPAREN) || tok_is_p(tk(p, 1), CC_P_ASSIGN))) {
            /* a bare name: closure parameter or identifier-list parameter */
            pa->name = tok_name(p, p->i);
            adv(p);
            if (accept_p(p, CC_P_ASSIGN)) pa->default_value = parse_assign(p);
            pa->span = span_from(p, first);
            CC_LIST_PUSH(p->a, &fn->params, pa);
        } else {
            Specs S;
            parse_specs(p, &S, 1);
            if (!S.type) {
                err_here(p, "expected a parameter declaration, found %s", tok_desc(p, p->i));
                /* skip to the next `,` or `)` */
                while (!at_eof(p) && !at_p(p, CC_P_COMMA) && !at_p(p, CC_P_RPAREN)) {
                    if (at_p(p, CC_P_LPAREN) || at_p(p, CC_P_LBRACKET) || at_p(p, CC_P_LBRACE)) skip_balanced(p); else adv(p);
                }
                if (at_p(p, CC_P_RPAREN)) break;
                if (!accept_p(p, CC_P_COMMA)) break;
                continue;
            }
            pa->type = parse_declarator(p, S.type, 1, &pa->name, NULL, &S.attrs);
            parse_attrs(p, &S.attrs);
            pa->attrs = S.attrs;
            if (accept_p(p, CC_P_ASSIGN)) pa->default_value = parse_assign(p);
            pa->span = span_from(p, first);
            CC_LIST_PUSH(p->a, &fn->params, pa);
        }
        if (accept_p(p, CC_P_COMMA)) {
            if (at_p(p, CC_P_RPAREN)) break;
            continue;
        }
        break;
    }
    expect_close(p, CC_P_RPAREN, "parameter list", open);
}

/* Members of a struct/union body, at the token after `{`. */
static void parse_members(P *p, CcType *st, uint32_t open) {
    CcField **link = &st->fields;
    while (!at_p(p, CC_P_RBRACE) && !at_eof(p)) {
        uint32_t first = p->i;
        Specs S;
        if (p->t[p->i].kind == CC_TK_PP) {
            CcField *f = CC_NEW(p->a, CcField);
            uint32_t last; int skipped;
            f->is_pp = 1;
            f->pp_tok = p->i;
            pp_note(p, p->i, &last, &skipped);
            f->span = span2(first, last);
            *link = f; link = &f->next;
            continue;
        }
        if (accept_p(p, CC_P_SEMI)) continue;
        if (at_kw(p, KW_STATIC_ASSERT)) {
            /* C11 allows it among members; keep it as an anonymous field
             * with a typeof-like shell is wrong, so diagnose honestly */
            err_here(p, "_Static_assert inside a struct body is not supported by this parser");
            sync(p, first);
            continue;
        }
        parse_specs(p, &S, 0);
        if (!S.type) {
            err_here(p, "expected a member declaration, found %s", tok_desc(p, p->i));
            sync(p, first);
            continue;
        }
        if (at_p(p, CC_P_SEMI) || (at_p(p, CC_P_COLON))) {
            /* anonymous struct/union member, or an unnamed bit-field */
            CcField *f = CC_NEW(p->a, CcField);
            f->type = S.type;
            f->attrs = S.attrs;
            if (accept_p(p, CC_P_COLON)) f->bit_width = parse_cond(p);
            parse_attrs(p, &f->attrs);
            expect_semi(p, "the member declaration");
            f->span = span_from(p, first);
            *link = f; link = &f->next;
            continue;
        }
        for (;;) {
            CcField *f = CC_NEW(p->a, CcField);
            uint32_t dfirst = p->i;
            CcAttr *attrs = NULL;
            f->type = parse_declarator(p, S.type, 0, &f->name, NULL, &attrs);
            if (accept_p(p, CC_P_COLON)) f->bit_width = parse_cond(p);
            parse_attrs(p, &attrs);
            f->attrs = S.attrs;
            attr_append(&f->attrs, attrs);
            if (accept_p(p, CC_P_COMMA)) {
                f->span = span2(dfirst == first ? first : dfirst, last_tok(p));
                *link = f; link = &f->next;
                continue;
            }
            expect_semi(p, "the member declaration");
            f->span = span2(dfirst == first ? first : dfirst, last_tok(p));
            *link = f; link = &f->next;
            break;
        }
        if (p->i == first) sync(p, first);
    }
    expect_close(p, CC_P_RBRACE, "struct body", open);
}

static CcType *parse_struct_or_union(P *p) {
    uint32_t first = p->i;
    CcType *t = cc_type_new(p->a, CC_T_STRUCT, span2(first, first));
    t->is_union = at_kw(p, KW_UNION);
    adv(p);
    parse_attrs(p, &t->attrs);
    if (at_ident(p) && kw_of(p, p->i) == KW_NONE) { t->name = tok_name(p, p->i); adv(p); }
    if (at_p(p, CC_P_LBRACE)) {
        uint32_t open = p->i;
        adv(p);
        t->is_definition = 1;
        parse_members(p, t, open);
        parse_attrs(p, &t->attrs);
    } else if (!t->name) {
        err_here(p, "expected a tag or '{' after '%s', found %s", t->is_union ? "union" : "struct", tok_desc(p, p->i));
    }
    t->span = span_from(p, first);
    return t;
}

static CcType *parse_enum(P *p) {
    uint32_t first = p->i;
    CcType *t = cc_type_new(p->a, CC_T_ENUM, span2(first, first));
    adv(p);
    parse_attrs(p, &t->attrs);
    if (at_ident(p) && kw_of(p, p->i) == KW_NONE) { t->name = tok_name(p, p->i); adv(p); }
    if (at_p(p, CC_P_COLON)) { /* C23 fixed underlying type */
        adv(p);
        t->base = parse_type_name(p);
    }
    if (at_p(p, CC_P_LBRACE)) {
        uint32_t open = p->i;
        CcEnumerator **link = &t->enumerators;
        adv(p);
        t->is_definition = 1;
        while (!at_p(p, CC_P_RBRACE) && !at_eof(p)) {
            CcEnumerator *e = CC_NEW(p->a, CcEnumerator);
            uint32_t efirst = p->i;
            if (p->t[p->i].kind == CC_TK_PP) {
                uint32_t last; int skipped;
                e->is_pp = 1;
                e->pp_tok = p->i;
                pp_note(p, p->i, &last, &skipped);
                e->span = span2(efirst, last);
                *link = e; link = &e->next;
                continue;
            }
            if (at_word(p, "comptime")) {
                e->comptime = parse_at_stmt(p);
                e->span = span_from(p, efirst);
                *link = e; link = &e->next;
                accept_p(p, CC_P_COMMA);
                continue;
            }
            if (!at_ident(p)) {
                err_here(p, "expected an enumerator name, found %s", tok_desc(p, p->i));
                sync(p, efirst);
                if (at_p(p, CC_P_RBRACE)) break;
                continue;
            }
            e->name = tok_name(p, p->i);
            declare_var(p, e->name);
            adv(p);
            {
                CcAttr *ea = NULL;
                parse_attrs(p, &ea);
            }
            if (accept_p(p, CC_P_ASSIGN)) e->value = parse_cond(p);
            e->span = span_from(p, efirst);
            *link = e; link = &e->next;
            if (!accept_p(p, CC_P_COMMA)) break;
        }
        expect_close(p, CC_P_RBRACE, "enum body", open);
        parse_attrs(p, &t->attrs);
    } else if (!t->name) {
        err_here(p, "expected a tag or '{' after 'enum', found %s", tok_desc(p, p->i));
    }
    t->span = span_from(p, first);
    return t;
}

/* ======================================================================
 * 3. Expressions
 * ====================================================================== */

static CcExpr *parse_unary(P *p);
static CcExpr *parse_postfix(P *p, CcExpr *e);
static CcExpr *parse_primary(P *p);
static CcExpr *parse_closure(P *p, uint32_t first, int is_unsafe, int is_async);
static void parse_args(P *p, CcExprList *out, const char *what);
static CcTplPart *parse_template_parts(P *p, uint32_t tok);

static CcExpr *expr_error(P *p) {
    CcExpr *e = cc_expr_new(p->a, CC_E_IDENT, span2(p->i, p->i));
    e->name = cc_intern(p->in, "<error>", 7);
    e->tok = p->i;
    return e;
}

static int binop_of(const CcToken *t, int *prec, CcOp *op) {
    if (t->kind != CC_TK_PUNCT) return 0;
    switch (t->punct) {
    case CC_P_OROR: *prec = 1; *op = CC_OP_LOR; return 1;
    case CC_P_ANDAND: *prec = 2; *op = CC_OP_LAND; return 1;
    case CC_P_PIPE: *prec = 3; *op = CC_OP_OR; return 1;
    case CC_P_CARET: *prec = 4; *op = CC_OP_XOR; return 1;
    case CC_P_AMP: *prec = 5; *op = CC_OP_AND; return 1;
    case CC_P_EQ: *prec = 6; *op = CC_OP_EQ; return 1;
    case CC_P_NE: *prec = 6; *op = CC_OP_NE; return 1;
    case CC_P_LT: *prec = 7; *op = CC_OP_LT; return 1;
    case CC_P_GT: *prec = 7; *op = CC_OP_GT; return 1;
    case CC_P_LE: *prec = 7; *op = CC_OP_LE; return 1;
    case CC_P_GE: *prec = 7; *op = CC_OP_GE; return 1;
    case CC_P_SHL: *prec = 8; *op = CC_OP_SHL; return 1;
    case CC_P_SHR: *prec = 8; *op = CC_OP_SHR; return 1;
    case CC_P_PLUS: *prec = 9; *op = CC_OP_ADD; return 1;
    case CC_P_MINUS: *prec = 9; *op = CC_OP_SUB; return 1;
    case CC_P_STAR: *prec = 10; *op = CC_OP_MUL; return 1;
    case CC_P_SLASH: *prec = 10; *op = CC_OP_DIV; return 1;
    case CC_P_PERCENT: *prec = 10; *op = CC_OP_MOD; return 1;
    default: return 0;
    }
}

static int assignop_of(const CcToken *t, CcOp *op) {
    if (t->kind != CC_TK_PUNCT) return 0;
    switch (t->punct) {
    case CC_P_ASSIGN: *op = CC_OP_ASSIGN; return 1;
    case CC_P_MUL_ASSIGN: *op = CC_OP_MUL_ASSIGN; return 1;
    case CC_P_DIV_ASSIGN: *op = CC_OP_DIV_ASSIGN; return 1;
    case CC_P_MOD_ASSIGN: *op = CC_OP_MOD_ASSIGN; return 1;
    case CC_P_ADD_ASSIGN: *op = CC_OP_ADD_ASSIGN; return 1;
    case CC_P_SUB_ASSIGN: *op = CC_OP_SUB_ASSIGN; return 1;
    case CC_P_SHL_ASSIGN: *op = CC_OP_SHL_ASSIGN; return 1;
    case CC_P_SHR_ASSIGN: *op = CC_OP_SHR_ASSIGN; return 1;
    case CC_P_AND_ASSIGN: *op = CC_OP_AND_ASSIGN; return 1;
    case CC_P_XOR_ASSIGN: *op = CC_OP_XOR_ASSIGN; return 1;
    case CC_P_OR_ASSIGN: *op = CC_OP_OR_ASSIGN; return 1;
    default: return 0;
    }
}

static CcExpr *parse_expr(P *p) {
    CcExpr *e = parse_assign(p);
    while (at_p(p, CC_P_COMMA)) {
        CcExpr *c;
        adv(p);
        c = cc_expr_new(p->a, CC_E_COMMA, span2(e->span.first, e->span.first));
        c->a = e;
        c->b = parse_assign(p);
        c->span = span_from(p, e->span.first);
        e = c;
    }
    return e;
}

static CcExpr *parse_legacy_err_tail(P *p, CcExpr *lhs);

/* Legacy `=<! expr [: default] @err ...`: the right-hand side after `=<!`,
 * mapped onto the !> / ?> nodes. */
static int at_legacy_bang(P *p) {
    return at_p(p, CC_P_ASSIGN) && tok_is_p(tk(p, 1), CC_P_LT) && tok_is_p(tk(p, 2), CC_P_BANG) &&
           adjacent(p, p->i) && adjacent(p, p->i + 1);
}

static CcExpr *parse_legacy_bang_rhs(P *p) {
    CcExpr *rhs;
    adv(p); adv(p); adv(p); /* = < ! */
    rhs = parse_cond(p);
    if (at_p(p, CC_P_COLON)) {
        CcExpr *e = cc_expr_new(p->a, CC_E_UNWRAP_OR, rhs->span);
        adv(p);
        e->a = rhs;
        e->b = parse_cond(p);
        e->span = span_from(p, rhs->span.first);
        rhs = e;
    }
    if (at_word(p, "err")) rhs = parse_legacy_err_tail(p, rhs);
    else if (rhs->kind != CC_E_UNWRAP && rhs->kind != CC_E_UNWRAP_BODY && rhs->kind != CC_E_UNWRAP_OR)
        err_here(p, "'=<!' needs a trailing '@err', found %s", tok_desc(p, p->i));
    return rhs;
}

static CcStmt *parse_parallel(P *p);

static CcExpr *parse_assign(P *p) {
    CcExpr *lhs = parse_cond(p);
    CcOp op;
    if (at_p(p, CC_P_ASSIGN) && tok_is_word(p, p->i + 1, "parallel") && !p->bind_stmt) {
        /* `lvalue = @parallel { ... } tail;`: a @parallel statement assigning its join */
        CcStmt *ps;
        adv(p);
        ps = parse_parallel(p);
        ps->par_target = lhs;
        ps->span.first = lhs->span.first;
        p->bind_stmt = ps;
        return lhs;
    }
    if (at_p(p, CC_P_ASSIGN) && tok_is_p(tk(p, 1), CC_P_LBRACE)) {
        /* `x = { .f = v };`: an initializer list assigned to an existing object */
        CcExpr *e = cc_expr_new(p->a, CC_E_ASSIGN, lhs->span);
        CcExpr *c;
        adv(p);
        c = cc_expr_new(p->a, CC_E_COMPOUND, span2(p->i, p->i));
        c->init = parse_initializer(p);
        c->span = c->init->span;
        e->op = CC_OP_ASSIGN;
        e->a = lhs;
        e->b = c;
        e->span = span_from(p, lhs->span.first);
        return e;
    }
    if (at_legacy_bang(p)) {
        CcExpr *e = cc_expr_new(p->a, CC_E_ASSIGN, lhs->span);
        e->op = CC_OP_ASSIGN;
        e->a = lhs;
        e->b = parse_legacy_bang_rhs(p);
        e->span = span_from(p, lhs->span.first);
        return e;
    }
    if (assignop_of(&p->t[p->i], &op)) {
        CcExpr *e = cc_expr_new(p->a, CC_E_ASSIGN, lhs->span);
        adv(p);
        e->op = op;
        e->a = lhs;
        e->b = parse_assign(p);
        e->span = span_from(p, lhs->span.first);
        return e;
    }
    return lhs;
}

static CcExpr *parse_binary(P *p, int min_prec) {
    CcExpr *left = parse_unary(p);
    for (;;) {
        int prec;
        CcOp op;
        CcExpr *e;
        if (!binop_of(&p->t[p->i], &prec, &op) || prec < min_prec) break;
        adv(p);
        e = cc_expr_new(p->a, CC_E_BINARY, left->span);
        e->op = op;
        e->a = left;
        e->b = parse_binary(p, prec + 1);
        e->span = span_from(p, left->span.first);
        left = e;
    }
    return left;
}

static CcExpr *parse_cond(P *p) {
    CcExpr *c = parse_binary(p, 1);
    if (at_p(p, CC_P_QUESTION)) {
        CcExpr *e = cc_expr_new(p->a, CC_E_TERNARY, c->span);
        uint32_t q = p->i;
        adv(p);
        e->a = c;
        if (!at_p(p, CC_P_COLON)) e->b = parse_expr(p); /* GNU `a ?: b` leaves b NULL */
        expect_close(p, CC_P_COLON, "conditional expression", q);
        e->c = parse_cond(p);
        e->span = span_from(p, c->span.first);
        return e;
    }
    return c;
}

/* Is `(` at `i` the start of a cast or compound literal? */
static int paren_is_type(P *p, uint32_t i) {
    uint32_t j = i + 1;
    const CcToken *t = &p->t[j];
    if (is_type_start(p, j)) {
        /* `(Type)` alone is ambiguous with `(name)` only for typedef names;
         * a typedef name in scope is a type. */
        return 1;
    }
    if (t->kind == CC_TK_IDENT && kw_of(p, j) == KW_NONE && !name_is_var(p, tok_name(p, j))) {
        uint32_t k = j + 1;
        uint32_t close;
        /* `(MACRO(args))expr`: a cast through a macro that expands to a type */
        if (tok_is_p(&p->t[k], CC_P_LPAREN) && !tok_is_p(&p->t[k + 1], CC_P_RPAREN)) {
            uint32_t inner = match_close(p, k) + 1;
            const CcToken *after;
            int stars = 0;
            while (tok_is_p(&p->t[inner], CC_P_STAR)) { inner++; stars++; }
            if (!tok_is_p(&p->t[inner], CC_P_RPAREN)) return 0;
            after = &p->t[inner + 1];
            if (after->kind == CC_TK_IDENT) return kw_of(p, inner + 1) == KW_NONE || kw_of(p, inner + 1) == KW_SIZEOF;
            if (after->kind == CC_TK_NUMBER || after->kind == CC_TK_CHAR || after->kind == CC_TK_STRING) return 1;
            return stars && after->kind == CC_TK_PUNCT && (after->punct == CC_P_LPAREN || after->punct == CC_P_AMP || after->punct == CC_P_STAR);
        }
        /* `(Name::[T])`, `(Name*)`, `(Name[:])` */
        if (tok_is_p(&p->t[k], CC_P_COLONCOLON) && tok_is_p(&p->t[k + 1], CC_P_LBRACKET)) return 1;
        if (tok_is_p(&p->t[k], CC_P_STAR)) {
            while (tok_is_p(&p->t[k], CC_P_STAR) || (p->t[k].kind == CC_TK_IDENT && kw_of(p, k) == KW_CONST)) k++;
            return tok_is_p(&p->t[k], CC_P_RPAREN) || tok_is_p(&p->t[k], CC_P_LBRACKET);
        }
        if (tok_is_p(&p->t[k], CC_P_LBRACKET))
            return tok_is_p(&p->t[k + 1], CC_P_COLON) || tok_is_p(&p->t[k + 1], CC_P_TILDE) ||
                   (p->t[k + 1].kind == CC_TK_NUMBER && tok_is_p(&p->t[k + 2], CC_P_COLON)) ||
                   tok_is_p(&p->t[k + 1], CC_P_RBRACKET);
        if (!tok_is_p(&p->t[k], CC_P_RPAREN)) return 0;
        close = k;
        t = &p->t[close + 1];
        if (tok_is_p(t, CC_P_LBRACE)) return 1; /* compound literal */
        if (t->kind == CC_TK_IDENT) {
            Kw kw = kw_of(p, close + 1);
            return kw == KW_NONE || kw == KW_SIZEOF || kw == KW_ALIGNOF;
        }
        if (t->kind == CC_TK_NUMBER || t->kind == CC_TK_CHAR || t->kind == CC_TK_STRING || t->kind == CC_TK_AT_WORD) return 1;
        if (t->kind == CC_TK_PUNCT)
            return t->punct == CC_P_LPAREN || t->punct == CC_P_BANG || t->punct == CC_P_TILDE || t->punct == CC_P_DOT;
    }
    return 0;
}

static CcExpr *unary_node(P *p, CcOp op, uint32_t first, CcExpr *a) {
    CcExpr *e = cc_expr_new(p->a, CC_E_UNARY, span2(first, first));
    e->op = op;
    e->a = a;
    e->span = span_from(p, first);
    return e;
}

static CcExpr *parse_unary(P *p) {
    const CcToken *t = &p->t[p->i];
    uint32_t first = p->i;
    if (t->kind == CC_TK_PUNCT) {
        switch (t->punct) {
        case CC_P_INC: adv(p); return unary_node(p, CC_OP_PREINC, first, parse_unary(p));
        case CC_P_DEC: adv(p); return unary_node(p, CC_OP_PREDEC, first, parse_unary(p));
        case CC_P_AMP: adv(p); return unary_node(p, CC_OP_ADDR, first, parse_unary(p));
        case CC_P_STAR: adv(p); return unary_node(p, CC_OP_DEREF, first, parse_unary(p));
        case CC_P_PLUS: adv(p); return unary_node(p, CC_OP_POS, first, parse_unary(p));
        case CC_P_MINUS: adv(p); return unary_node(p, CC_OP_NEG, first, parse_unary(p));
        case CC_P_TILDE: adv(p); return unary_node(p, CC_OP_BITNOT, first, parse_unary(p));
        case CC_P_BANG: adv(p); return unary_node(p, CC_OP_NOT, first, parse_unary(p));
        case CC_P_ANDAND: { /* GNU &&label */
            CcExpr *e;
            adv(p);
            e = unary_node(p, CC_OP_ADDR, first, unary_node(p, CC_OP_ADDR, first, parse_unary(p)));
            return e;
        }
        case CC_P_LPAREN:
            if (tok_is_p(&p->t[match_close(p, p->i) + 1], CC_P_FAT_ARROW)) return parse_closure(p, first, 0, 0);
            if (paren_is_type(p, p->i)) {
                uint32_t open = p->i;
                CcType *ty;
                adv(p);
                ty = parse_type_name(p);
                expect_close(p, CC_P_RPAREN, "cast", open);
                if (at_p(p, CC_P_LBRACE)) {
                    CcExpr *e = cc_expr_new(p->a, CC_E_COMPOUND, span2(first, first));
                    e->type = ty;
                    e->init = parse_initializer(p);
                    e->span = span_from(p, first);
                    return parse_postfix(p, e);
                } else {
                    CcExpr *e = cc_expr_new(p->a, CC_E_CAST, span2(first, first));
                    e->type = ty;
                    e->a = parse_unary(p);
                    e->span = span_from(p, first);
                    return e;
                }
            }
            break;
        default: break;
        }
    } else if (t->kind == CC_TK_IDENT) {
        Kw k = kw_of(p, p->i);
        if (k == KW_SIZEOF || k == KW_ALIGNOF || k == KW_ALIGNOF2 || k == KW_ALIGNOF3 || k == KW_ALIGNOF4) {
            CcOp op = k == KW_SIZEOF ? CC_OP_SIZEOF : CC_OP_ALIGNOF;
            adv(p);
            if (at_p(p, CC_P_LPAREN) && paren_is_type(p, p->i) && !tok_is_p(&p->t[match_close(p, p->i) + 1], CC_P_LBRACE)) {
                uint32_t open = p->i;
                CcExpr *e = cc_expr_new(p->a, CC_E_SIZEOF_TYPE, span2(first, first));
                adv(p);
                e->op = op;
                e->type = parse_type_name(p);
                expect_close(p, CC_P_RPAREN, op == CC_OP_SIZEOF ? "sizeof" : "_Alignof", open);
                e->span = span_from(p, first);
                return e;
            }
            return unary_node(p, op, first, parse_unary(p));
        }
        if (k == KW_EXTENSION) { adv(p); return parse_unary(p); }
        if (k == KW_NONE && tok_is_p(tk(p, 1), CC_P_LPAREN) && !name_is_var(p, tok_name(p, p->i)) &&
            !name_is_type(p, tok_name(p, p->i)) && is_type_start(p, p->i + 2) &&
            (kw_of(p, p->i + 2) != KW_NONE || tok_is_p(tk(p, 3), CC_P_STAR) || tok_is_p(tk(p, 3), CC_P_RPAREN))) {
            /* `constcast(T *)x`: a macro cast; the type inside the parentheses, the operand after */
            uint32_t close = match_close(p, p->i + 1);
            const CcToken *after = &p->t[close + 1];
            if ((after->kind == CC_TK_IDENT && kw_of(p, close + 1) == KW_NONE) || after->kind == CC_TK_NUMBER ||
                (after->kind == CC_TK_PUNCT && (after->punct == CC_P_LPAREN || after->punct == CC_P_AMP || after->punct == CC_P_STAR))) {
                CcExpr *e = cc_expr_new(p->a, CC_E_CAST, span2(first, first));
                CcType *mt = cc_type_new(p->a, CC_T_MACRO, span2(first, first));
                mt->name = tok_name(p, p->i);
                adv(p); adv(p);
                CC_LIST_PUSH(p->a, &mt->args, parse_type_name(p));
                expect_close(p, CC_P_RPAREN, "macro cast", first + 1);
                mt->span = span_from(p, first);
                e->type = mt;
                e->a = parse_unary(p);
                e->span = span_from(p, first);
                return e;
            }
        }
    } else if (t->kind == CC_TK_AT_WORD) {
        if (at_word(p, "await")) {
            CcExpr *e = cc_expr_new(p->a, CC_E_AWAIT, span2(first, first));
            adv(p);
            e->a = parse_unary(p);
            e->span = span_from(p, first);
            return e;
        }
        if ((at_word(p, "blocking") || at_word(p, "nonblocking") || at_word(p, "noblock")) && !tok_is_p(tk(p, 1), CC_P_LBRACE)) {
            CcExpr *e = cc_expr_new(p->a, CC_E_CALL_MODE, span2(first, first));
            e->name = cc_intern(p->in, p->f->src + t->off + 1, t->len - 1);
            adv(p);
            e->a = parse_unary(p);
            e->span = span_from(p, first);
            return e;
        }
    }
    return parse_postfix(p, parse_primary(p));
}

/* After `!>`: bare unwrap, `!> body`, `!>(e) body`, `!> @destroy [{ D }]`. */
static CcExpr *parse_unwrap_tail(P *p, CcExpr *lhs) {
    uint32_t first = lhs->span.first;
    CcName binder = NULL;
    uint32_t op_tok = p->i;
    if (lhs->kind == CC_E_NUMBER || lhs->kind == CC_E_CHAR || lhs->kind == CC_E_STRING)
        err_here(p, "'!>' needs a Result on its left, not a %s literal", cc_expr_kind_name(lhs->kind));
    adv(p); /* !> */
    if (at_p(p, CC_P_LPAREN) && !(tk(p, 1)->kind == CC_TK_IDENT && tok_is_p(tk(p, 2), CC_P_RPAREN))) {
        if (tok_is_p(tk(p, 1), CC_P_RPAREN))
            err_at(p, p->i + 1, "'!>()' needs a name to bind the error: write `!>(e) body`");
        else
            err_at(p, p->i + 1, "'!>(...)' binds the error to a name, found %s", tok_desc(p, p->i + 1));
        skip_balanced(p);
    }
    if (at_p(p, CC_P_LPAREN) && tk(p, 1)->kind == CC_TK_IDENT && tok_is_p(tk(p, 2), CC_P_RPAREN)) {
        const CcToken *after = tk(p, 3);
        int term = after->kind == CC_TK_EOF ||
                   (after->kind == CC_TK_PUNCT && (after->punct == CC_P_SEMI || after->punct == CC_P_COMMA ||
                                                   after->punct == CC_P_RPAREN || after->punct == CC_P_RBRACKET ||
                                                   after->punct == CC_P_RBRACE || after->punct == CC_P_COLON));
        if (!term) {
            binder = tok_name(p, p->i + 1);
            adv(p); adv(p); adv(p);
        }
    }
    if (at_word(p, "destroy")) {
        CcExpr *e = cc_expr_new(p->a, CC_E_UNWRAP_DESTROY, span2(first, first));
        e->a = lhs;
        adv(p);
        if (at_p(p, CC_P_LBRACE)) e->body = parse_block(p);
        e->span = span_from(p, first);
        return e;
    }
    {
        const CcToken *t = &p->t[p->i];
        int body = tok_is_p(t, CC_P_LBRACE) || t->kind == CC_TK_IDENT ||
                   (t->kind == CC_TK_AT_WORD && !at_word(p, "detach") && !at_word(p, "destroy"));
        if (!body && binder) {
            err_here(p, "'!>(%s)' binds the error for a body, but no body follows (found %s)", binder, tok_desc(p, p->i));
            body = 0;
        }
        if (body) {
            CcExpr *e = cc_expr_new(p->a, CC_E_UNWRAP_BODY, span2(first, first));
            e->a = lhs;
            e->binder = binder;
            scope_push(p->syms);
            if (binder) declare_var(p, binder);
            if (at_p(p, CC_P_LBRACE)) e->body = parse_block(p);
            else e->body = parse_stmt(p);
            scope_pop(p->syms);
            p->consumed_terminator = last_tok(p);
            e->span = span_from(p, first);
            return e;
        }
    }
    {
        CcExpr *e = cc_expr_new(p->a, CC_E_UNWRAP, span2(first, op_tok));
        e->a = lhs;
        e->span = span_from(p, first);
        return e;
    }
}

/* Legacy postfix `expr @err`, `expr @err(E e) body`, `expr @err body`:
 * the same nodes as `!>`. */
static CcExpr *parse_legacy_err_tail(P *p, CcExpr *lhs) {
    uint32_t first = lhs->span.first, op_tok = p->i;
    CcName binder = NULL;
    adv(p); /* @err */
    if (at_p(p, CC_P_LPAREN)) {
        uint32_t open = p->i;
        adv(p);
        if (at_ident(p) && tok_is_p(tk(p, 1), CC_P_RPAREN)) { binder = tok_name(p, p->i); adv(p); }
        else {
            Specs S;
            parse_specs(p, &S, 1);
            if (!S.type) err_here(p, "expected the error binder in @err(E e), found %s", tok_desc(p, p->i));
            else {
                uint32_t nt = 0;
                parse_declarator(p, S.type, 1, &binder, &nt, &S.attrs);
            }
        }
        expect_close(p, CC_P_RPAREN, "@err(...) binder", open);
    }
    {
        const CcToken *t = &p->t[p->i];
        int body = tok_is_p(t, CC_P_LBRACE) || t->kind == CC_TK_IDENT ||
                   (t->kind == CC_TK_AT_WORD && !at_word(p, "detach") && !at_word(p, "destroy"));
        if (body) {
            CcExpr *e = cc_expr_new(p->a, CC_E_UNWRAP_BODY, span2(first, first));
            e->a = lhs;
            e->binder = binder;
            scope_push(p->syms);
            if (binder) declare_var(p, binder);
            if (at_p(p, CC_P_LBRACE)) e->body = parse_block(p);
            else e->body = parse_stmt(p);
            scope_pop(p->syms);
            p->consumed_terminator = last_tok(p);
            e->span = span_from(p, first);
            return e;
        }
        if (binder) err_here(p, "'@err(%s)' binds the error for a body, but no body follows (found %s)", binder, tok_desc(p, p->i));
    }
    {
        CcExpr *e = cc_expr_new(p->a, CC_E_UNWRAP, span2(first, op_tok));
        e->a = lhs;
        e->span = span_from(p, first);
        return e;
    }
}

/* After `?>`: `?> default` / `?>(e) default`. */
static CcExpr *parse_unwrap_or_tail(P *p, CcExpr *lhs) {
    uint32_t first = lhs->span.first;
    CcExpr *e = cc_expr_new(p->a, CC_E_UNWRAP_OR, span2(first, first));
    e->a = lhs;
    if (lhs->kind == CC_E_NUMBER || lhs->kind == CC_E_CHAR || lhs->kind == CC_E_STRING)
        err_here(p, "'?>' needs a Result on its left, not a %s literal", cc_expr_kind_name(lhs->kind));
    adv(p); /* ?> */
    if (at_p(p, CC_P_LPAREN) && !p->t[p->i].after_space && !(tk(p, 1)->kind == CC_TK_IDENT && tok_is_p(tk(p, 2), CC_P_RPAREN))) {
        if (tok_is_p(tk(p, 1), CC_P_RPAREN))
            err_at(p, p->i + 1, "'?>()' needs a name to bind the error: write `?>(e) default`");
        else
            err_at(p, p->i + 1, "'?>(...)' binds the error to a name, found %s", tok_desc(p, p->i + 1));
        skip_balanced(p);
    }
    if (at_p(p, CC_P_LPAREN) && tk(p, 1)->kind == CC_TK_IDENT && tok_is_p(tk(p, 2), CC_P_RPAREN)) {
        const CcToken *after = tk(p, 3);
        int term = after->kind == CC_TK_EOF ||
                   (after->kind == CC_TK_PUNCT && (after->punct == CC_P_SEMI || after->punct == CC_P_COMMA ||
                                                   after->punct == CC_P_RPAREN || after->punct == CC_P_RBRACKET ||
                                                   after->punct == CC_P_RBRACE));
        if (!term) {
            e->binder = tok_name(p, p->i + 1);
            adv(p); adv(p); adv(p);
        }
    }
    scope_push(p->syms);
    if (e->binder) declare_var(p, e->binder);
    if (at_p(p, CC_P_SEMI) || at_p(p, CC_P_RPAREN) || at_p(p, CC_P_COMMA) || at_eof(p))
        err_here(p, "'?>' needs a default value on its right, found %s", tok_desc(p, p->i));
    else
        e->b = parse_cond(p);
    scope_pop(p->syms);
    e->span = span_from(p, first);
    return e;
}

static CcExpr *parse_postfix(P *p, CcExpr *e) {
    for (;;) {
        const CcToken *t = &p->t[p->i];
        uint32_t first = e->span.first;
        if (t->kind == CC_TK_AT_WORD && at_word(p, "err") && !p->in_slot) {
            CcExpr *x = parse_legacy_err_tail(p, e);
            if (x->kind == CC_E_UNWRAP_BODY) return x;
            e = x;
            continue;
        }
        if (t->kind != CC_TK_PUNCT) break;
        switch (t->punct) {
        case CC_P_LBRACKET: {
            CcExpr *x = cc_expr_new(p->a, CC_E_INDEX, span2(first, first));
            uint32_t open = p->i;
            adv(p);
            x->a = e;
            x->b = parse_expr(p);
            expect_close(p, CC_P_RBRACKET, "index", open);
            x->span = span_from(p, first);
            e = x;
            continue;
        }
        case CC_P_LPAREN: {
            CcExpr *x = cc_expr_new(p->a, CC_E_CALL, span2(first, first));
            x->a = e;
            parse_args(p, &x->args, "call");
            x->span = span_from(p, first);
            e = x;
            continue;
        }
        case CC_P_DOT: case CC_P_ARROW: {
            int arrow = t->punct == CC_P_ARROW;
            CcName name;
            adv(p);
            if (!at_ident(p)) {
                err_here(p, "expected a member name after '%s', found %s", arrow ? "->" : ".", tok_desc(p, p->i));
                return e;
            }
            name = tok_name(p, p->i);
            adv(p);
            /* `x.m(args)` and `p->m(args)` are both UFCS sites; `arrow` records
             * the spelling. Whether `m` is really a field (a function pointer
             * call, plain C) is the index's question, not the parser's. */
            if (at_p(p, CC_P_LPAREN) || at_p2(p, CC_P_COLONCOLON, CC_P_LBRACKET)) {
                CcExpr *x;
                int type_scoped = !arrow && ((e->kind == CC_E_IDENT && name_is_type(p, e->name)) || e->kind == CC_E_TYPE_ARG);
                x = cc_expr_new(p->a, type_scoped ? CC_E_TYPE_SCOPED : CC_E_UFCS, span2(first, first));
                x->name = name;
                x->arrow = arrow;
                if (type_scoped) {
                    if (e->kind == CC_E_TYPE_ARG) x->type = e->type;
                    else x->type = type_named(p, e->name, e->span.first), x->type->span = e->span;
                } else x->a = e;
                if (at_p(p, CC_P_COLONCOLON)) {
                    uint32_t open = p->i + 1;
                    adv(p); adv(p);
                    while (!at_p(p, CC_P_RBRACKET) && !at_eof(p)) {
                        CcType *ta = parse_type_name(p);
                        CC_LIST_PUSH(p->a, &x->targs, ta);
                        if (!accept_p(p, CC_P_COMMA)) break;
                    }
                    expect_close(p, CC_P_RBRACKET, "generic argument list", open);
                    if (!at_p(p, CC_P_LPAREN)) {
                        err_here(p, "expected '(' after '%s::[...]' to make the UFCS call, found %s", name, tok_desc(p, p->i));
                        x->span = span_from(p, first);
                        return x;
                    }
                }
                parse_args(p, &x->args, "call");
                x->span = span_from(p, first);
                e = x;
                continue;
            }
            {
                CcExpr *x = cc_expr_new(p->a, CC_E_MEMBER, span2(first, first));
                x->a = e;
                x->name = name;
                x->arrow = arrow;
                x->span = span_from(p, first);
                e = x;
            }
            continue;
        }
        case CC_P_INC: adv(p); e = unary_node(p, CC_OP_POSTINC, first, e); continue;
        case CC_P_DEC: adv(p); e = unary_node(p, CC_OP_POSTDEC, first, e); continue;
        case CC_P_UNWRAP: {
            CcExpr *x = parse_unwrap_tail(p, e);
            if (x->kind == CC_E_UNWRAP_BODY) return x;
            e = x;
            continue;
        }
        case CC_P_UNWRAP_OR:
            return parse_unwrap_or_tail(p, e);
        default: break;
        }
        break;
    }
    return e;
}

/* Argument list at `(`: expressions, with types allowed as arguments
 * (offsetof, cc_ok(T, v)) and preprocessor lines between arguments. */
static void parse_args(P *p, CcExprList *out, const char *what) {
    uint32_t open = p->i;
    adv(p);
    while (!at_p(p, CC_P_RPAREN) && !at_eof(p)) {
        CcExpr *a;
        if (p->t[p->i].kind == CC_TK_PP) {
            uint32_t last; int skipped;
            a = cc_expr_new(p->a, CC_E_PP, span2(p->i, p->i));
            a->tok = p->i;
            pp_note(p, p->i, &last, &skipped);
            a->span.last = last;
            CC_LIST_PUSH(p->a, out, a);
            continue;
        }
        if (is_type_start(p, p->i)) {
            Kw k = kw_of(p, p->i);
            int type_arg = 1;
            if (k == KW_NONE && p->t[p->i].kind == CC_TK_IDENT) {
                const CcToken *n1 = tk(p, 1);
                type_arg = tok_is_p(n1, CC_P_COMMA) || tok_is_p(n1, CC_P_RPAREN) || tok_is_p(n1, CC_P_STAR) ||
                           (tok_is_p(n1, CC_P_LBRACKET) && (tok_is_p(tk(p, 2), CC_P_COLON) || tok_is_p(tk(p, 2), CC_P_TILDE) || tok_is_p(tk(p, 2), CC_P_RBRACKET))) ||
                           (tok_is_p(n1, CC_P_COLONCOLON) && tok_is_p(tk(p, 2), CC_P_LBRACKET) &&
                            !tok_is_p(&p->t[match_close(p, p->i + 2) + 1], CC_P_LPAREN) &&
                            !tok_is_p(&p->t[match_close(p, p->i + 2) + 1], CC_P_DOT));
            } else if (k == KW_ATTRIBUTE || k == KW_ATTRIBUTE2 || k == KW_EXTENSION) type_arg = 0;
            if (type_arg) {
                uint32_t first = p->i;
                a = cc_expr_new(p->a, CC_E_TYPE_ARG, span2(first, first));
                a->type = parse_type_name(p);
                a->span = span_from(p, first);
                a = parse_postfix(p, a);
                CC_LIST_PUSH(p->a, out, a);
                if (!accept_p(p, CC_P_COMMA)) break;
                continue;
            }
        }
        if (at_p(p, CC_P_LBRACE)) {
            /* `repeat8({ stmts })`: a brace block handed to a macro */
            a = cc_expr_new(p->a, CC_E_STMT_EXPR, span2(p->i, p->i));
            a->body = parse_block(p);
            a->span = a->body->span;
        } else a = parse_assign(p);
        CC_LIST_PUSH(p->a, out, a);
        if (p->t[p->i].kind == CC_TK_PP) continue;
        if (!accept_p(p, CC_P_COMMA)) break;
    }
    expect_close(p, CC_P_RPAREN, what, open);
}

/* Closure at `(`: `(params) => [captures] body`. */
static CcExpr *parse_closure(P *p, uint32_t first, int is_unsafe, int is_async) {
    CcExpr *e = cc_expr_new(p->a, CC_E_CLOSURE, span2(first, first));
    CcType fnshell;
    size_t i;
    memset(&fnshell, 0, sizeof fnshell);
    e->closure_unsafe = is_unsafe;
    e->closure_async = is_async;
    if (at_ident(p)) {
        /* `x => body`: one untyped parameter without parentheses */
        CcParam *pa = CC_NEW(p->a, CcParam);
        pa->name = tok_name(p, p->i);
        pa->span = span2(p->i, p->i);
        adv(p);
        CC_LIST_PUSH(p->a, &fnshell.params, pa);
        fnshell.has_prototype = 1;
    } else parse_params(p, &fnshell);
    e->params = fnshell.params;
    scope_push(p->syms);
    for (i = 0; i < e->params.n; i++)
        if (e->params.items[i]->name) declare_var(p, e->params.items[i]->name);
    if (!accept_p(p, CC_P_FAT_ARROW)) {
        err_here(p, "expected '=>' after the closure parameters, found %s", tok_desc(p, p->i));
        scope_pop(p->syms);
        e->span = span_from(p, first);
        return e;
    }
    if (at_p(p, CC_P_LBRACKET)) {
        uint32_t open = p->i;
        CcCapture **link = &e->captures;
        adv(p);
        while (!at_p(p, CC_P_RBRACKET) && !at_eof(p)) {
            CcCapture *c = CC_NEW(p->a, CcCapture);
            uint32_t cfirst = p->i;
            if (at_word(p, "safe")) { c->is_safe = 1; adv(p); }
            if (accept_p(p, CC_P_AMP)) c->by_ref = 1;
            if (!at_ident(p)) {
                err_here(p, "expected a captured variable name in the closure capture list, found %s", tok_desc(p, p->i));
                break;
            }
            c->name = tok_name(p, p->i);
            adv(p);
            if (accept_p(p, CC_P_ASSIGN)) c->init = parse_assign(p);
            c->span = span_from(p, cfirst);
            *link = c; link = &c->next;
            if (!accept_p(p, CC_P_COMMA)) break;
        }
        expect_close(p, CC_P_RBRACKET, "closure capture list", open);
    }
    if (at_p(p, CC_P_LBRACE)) {
        e->body = parse_block(p);
    } else {
        CcExpr *x = parse_assign(p);
        CcStmt *s = cc_stmt_new(p->a, CC_S_EXPR, x->span);
        s->expr = x;
        e->body = s;
    }
    scope_pop(p->syms);
    e->span = span_from(p, first);
    return e;
}

/* ---- templates ------------------------------------------------------- */

/* Lex a byte range of the file as its own token array with file-relative
 * offsets, so slot expressions position like everything else. */
static CcLexFile *slot_lex(P *p, uint32_t off, uint32_t len) {
    char *buf = (char *)malloc((size_t)off + len + 1);
    CcLexFile *sf;
    uint32_t li;
    if (!buf) { fprintf(stderr, "cc: out of memory lexing a template slot\n"); abort(); }
    memset(buf, ' ', off);
    for (li = 1; li < p->f->n_lines && p->f->line_starts[li] <= off; li++) buf[p->f->line_starts[li] - 1] = '\n';
    memcpy(buf + off, p->f->src + off, len);
    buf[off + len] = 0;
    sf = cc_lex(p->a, p->d, p->f->path, buf, (size_t)off + len);
    free(buf);
    sf->src = p->f->src;
    sf->len = p->f->len;
    sf->line_starts = p->f->line_starts;
    sf->n_lines = p->f->n_lines;
    sf->marks = p->f->marks;
    sf->n_marks = p->f->n_marks;
    if (sf->n_toks) {
        sf->toks[0].lead_off = off;
        sf->toks[0].lead_len = 0;
    }
    return sf;
}

static CcExpr *parse_slot_with(P *parent, CcLexFile **file_out, uint32_t off, uint32_t len) {
    P q = *parent;
    CcExpr *e;
    q.f = slot_lex(parent, off, len);
    q.t = q.f->toks;
    q.n = q.f->n_toks;
    q.i = 0;
    q.in_slot = 1;
    q.tpl_ok = 0;
    q.consumed_terminator = UINT32_MAX;
    q.bind_stmt = NULL;
    *file_out = q.f;
    if (at_eof(&q)) {
        err_here(&q, "empty ${} slot in the template");
        return expr_error(&q);
    }
    e = parse_expr(&q);
    if (!at_eof(&q)) err_here(&q, "unexpected %s after the slot expression", tok_desc(&q, q.i));
    return e;
}

CcExpr *cc_parse_slot_expr(CcArena *a, CcDiag *d, CcIntern *in, CcLexFile *f, uint32_t off, uint32_t len) {
    P p;
    CcLexFile *sf;
    int k;
    memset(&p, 0, sizeof p);
    p.a = a; p.d = d; p.in = in; p.f = f;
    p.t = f->toks; p.n = f->n_toks;
    p.u = CC_NEW(a, CcUnit);
    p.u->arena = a; p.u->file = f;
    p.syms = syms_new(a);
    for (k = 1; k < KW_COUNT; k++) p.kw[k] = cc_intern(in, kw_text[k], strlen(kw_text[k]));
    p.n_typeview = cc_intern(in, "typeview", 8);
    p.n_tag = cc_intern(in, "tag", 3);
    p.n_task = cc_intern(in, "task", 4);
    p.n_packed = cc_intern(in, "packed", 6);
    p.n_asm = cc_intern(in, "asm", 3);
    p.n_parallel_join = cc_intern(in, "@parallel", 9);
    p.consumed_terminator = UINT32_MAX;
    return parse_slot_with(&p, &sf, off, len);
}

/* Find the `}` closing a slot whose text starts at `i` (just after `${`),
 * skipping nested braces and quoted literals. Returns the index of the
 * closing brace or `end` when unterminated. */
static uint32_t slot_close(const char *s, uint32_t i, uint32_t end) {
    int depth = 1;
    while (i < end) {
        char c = s[i];
        if (c == '"' || c == '\'') {
            char q = c;
            i++;
            while (i < end && s[i] != q) { if (s[i] == '\\') i++; i++; }
            i++;
            continue;
        }
        if (c == '{' || c == '(' || c == '[') depth++;
        else if (c == '}' || c == ')' || c == ']') { depth--; if (depth == 0 && c == '}') return i; }
        i++;
    }
    return end;
}

static CcTplPart *tpl_part(P *p, uint32_t tok, uint32_t off, uint32_t len) {
    CcTplPart *pt = CC_NEW(p->a, CcTplPart);
    pt->span = span2(tok, tok);
    pt->off = off;
    pt->len = len;
    return pt;
}

static CcTplPart *parse_template_parts(P *p, uint32_t tok) {
    const CcToken *t = &p->t[tok];
    const char *s = p->f->src;
    uint32_t i = t->off + 1, end = t->off + t->len - 1; /* between the backticks */
    uint32_t run = i;
    CcTplPart *head = NULL, **link = &head;
    if (t->len < 2) return NULL;
    while (i < end) {
        char c = s[i];
        if (c == '\\') { i += 2; continue; }
        if (c == '$' && i + 1 < end && (s[i + 1] == '{' || s[i + 1] == '~')) {
            uint32_t slot_start = i;
            uint32_t body, close;
            CcName tag = NULL;
            if (s[i + 1] == '~') {
                uint32_t j = i + 2, k;
                while (j < end && ((s[j] >= 'a' && s[j] <= 'z') || (s[j] >= 'A' && s[j] <= 'Z') || (s[j] >= '0' && s[j] <= '9') || s[j] == '_')) j++;
                if (j == i + 2 || j >= end || s[j] != '{') { i++; continue; } /* not a tagged slot: literal */
                k = j;
                tag = cc_intern(p->in, s + i + 2, k - (i + 2));
                body = k + 1;
            } else body = i + 2;
            if (run < slot_start) { *link = tpl_part(p, tok, run, slot_start - run); link = &(*link)->next; }
            if (!tag && body < end && s[body] == '{') {
                /* `${{ verbatim }}` */
                uint32_t j = body + 1;
                while (j + 1 < end && !(s[j] == '}' && s[j + 1] == '}')) j++;
                if (j + 1 >= end) {
                    err_at(p, tok, "unterminated ${{ verbatim span in the template on line %u", tok_line(p, tok));
                    *link = tpl_part(p, tok, body + 1, end - (body + 1)); (*link)->is_verbatim = 1; link = &(*link)->next;
                    run = end; i = end; break;
                }
                *link = tpl_part(p, tok, body + 1, j - (body + 1)); (*link)->is_verbatim = 1; link = &(*link)->next;
                i = j + 2;
                run = i;
                continue;
            }
            close = slot_close(s, body, end);
            if (close >= end) {
                err_at(p, tok, "unterminated ${ slot in the template on line %u", tok_line(p, tok));
                run = end; i = end; break;
            }
            {
                CcTplPart *pt = tpl_part(p, tok, body, close - body);
                pt->is_slot = 1;
                pt->tag = tag;
                pt->expr = parse_slot_with(p, &pt->file, body, close - body);
                *link = pt; link = &pt->next;
            }
            i = close + 1;
            run = i;
            continue;
        }
        i++;
    }
    if (run < end) { *link = tpl_part(p, tok, run, end - run); link = &(*link)->next; }
    if (!head) { head = tpl_part(p, tok, t->off + 1, 0); }
    return head;
}

static CcExpr *template_literal(P *p) {
    CcExpr *e = cc_expr_new(p->a, CC_E_TEMPLATE, span2(p->i, p->i));
    e->tok = p->i;
    e->tpl_parts = parse_template_parts(p, p->i);
    adv(p);
    return e;
}

/* `@string(...)`: classify the arguments into policy / template / arena. */
static CcExpr *parse_at_string(P *p) {
    uint32_t first = p->i;
    CcExpr *e = cc_expr_new(p->a, CC_E_TEMPLATE, span2(first, first));
    CcExprList args;
    size_t i, ti = (size_t)-1;
    int save = p->tpl_ok;
    memset(&args, 0, sizeof args);
    adv(p);
    if (!at_p(p, CC_P_LPAREN)) { err_here(p, "expected '(' after @string, found %s", tok_desc(p, p->i)); e->span = span_from(p, first); return e; }
    p->tpl_ok = 1;
    parse_args(p, &args, "@string");
    p->tpl_ok = save;
    e->span = span_from(p, first);
    for (i = 0; i < args.n; i++)
        if (args.items[i]->kind == CC_E_TEMPLATE && args.items[i]->tpl_parts && !args.items[i]->tpl_arena && args.items[i]->tok) { ti = i; break; }
    if (args.n == 0) { err_at(p, first, "@string needs a template or a value: @string(`...`, arena) / @string(expr, arena)"); return e; }
    if (ti == (size_t)-1) {
        /* direct form: @string(expr[, arena]) */
        e->a = args.items[0];
        if (args.n >= 2) e->tpl_arena = args.items[1];
        if (args.n > 2) err_at(p, args.items[2]->span.first, "@string(expr, arena) takes at most two arguments");
        return e;
    }
    e->tpl_parts = args.items[ti]->tpl_parts;
    e->tok = args.items[ti]->tok;
    if (ti >= 1) e->tpl_policy = args.items[0];
    if (ti > 1) err_at(p, args.items[1]->span.first, "@string takes at most one policy before the template");
    if (ti + 1 < args.n) e->tpl_arena = args.items[ti + 1];
    if (ti + 2 < args.n) err_at(p, args.items[ti + 2]->span.first, "@string takes at most one arena after the template");
    return e;
}

static CcExpr *parse_at_emit(P *p) {
    uint32_t first = p->i;
    CcExpr *e = cc_expr_new(p->a, CC_E_EMIT, span2(first, first));
    size_t i;
    int save = p->tpl_ok;
    adv(p);
    if (!at_p(p, CC_P_LPAREN)) { err_here(p, "expected '(' after @emit, found %s", tok_desc(p, p->i)); e->span = span_from(p, first); return e; }
    p->tpl_ok = 1;
    parse_args(p, &e->args, "@emit");
    p->tpl_ok = save;
    e->span = span_from(p, first);
    for (i = 0; i < e->args.n; i++) {
        if (e->args.items[i]->kind == CC_E_TEMPLATE && e->args.items[i]->tpl_parts) {
            e->tpl_parts = e->args.items[i]->tpl_parts;
            e->tok = e->args.items[i]->tok;
            if (i + 1 < e->args.n) e->tpl_arena = e->args.items[i + 1];
            break;
        }
    }
    if (i == e->args.n) err_at(p, first, "@emit needs a template literal argument");
    return e;
}

static CcExpr *parse_primary(P *p) {
    const CcToken *t = &p->t[p->i];
    uint32_t first = p->i;
    switch (t->kind) {
    case CC_TK_IDENT: {
        Kw k = kw_of(p, p->i);
        CcExpr *e;
        if (k == KW_GENERIC) {
            uint32_t open;
            CcGenericSelArm **link;
            e = cc_expr_new(p->a, CC_E_GENERIC_SEL, span2(first, first));
            adv(p);
            open = p->i;
            if (!expect_p(p, CC_P_LPAREN, "_Generic")) return e;
            e->a = parse_assign(p);
            link = &e->arms;
            while (accept_p(p, CC_P_COMMA) || p->t[p->i].kind == CC_TK_PP) {
                CcGenericSelArm *arm = CC_NEW(p->a, CcGenericSelArm);
                if (p->t[p->i].kind == CC_TK_PP) {
                    uint32_t last; int skipped;
                    arm->is_pp = 1;
                    arm->pp_tok = p->i;
                    pp_note(p, p->i, &last, &skipped);
                    *link = arm; link = &arm->next;
                    if (at_p(p, CC_P_RPAREN)) break;
                    if (!at_p(p, CC_P_COMMA) && !at_kw(p, KW_DEFAULT) && !is_type_start(p, p->i)) break;
                    if (at_p(p, CC_P_COMMA)) continue;
                }
                if (at_p(p, CC_P_RPAREN)) break;
                if (accept_kw(p, KW_DEFAULT)) arm->type = NULL;
                else arm->type = parse_type_name(p);
                if (!expect_p(p, CC_P_COLON, "the _Generic association")) break;
                arm->expr = parse_assign(p);
                *link = arm; link = &arm->next;
            }
            expect_close(p, CC_P_RPAREN, "_Generic", open);
            e->span = span_from(p, first);
            return e;
        }
        if (at_kw(p, KW_ASYNC) && tok_is_p(tk(p, 1), CC_P_LPAREN)) {
            uint32_t close = match_close(p, p->i + 1);
            if (tok_is_p(&p->t[close + 1], CC_P_FAT_ARROW)) {
                adv(p);
                return parse_closure(p, first, 0, 1);
            }
        }
        if (k != KW_NONE && k != KW_BUILTIN_VA_LIST) {
            if (is_type_start(p, p->i)) {
                /* a type where an expression is expected: `sizeof`-less type argument use */
                CcExpr *ta = cc_expr_new(p->a, CC_E_TYPE_ARG, span2(first, first));
                ta->type = parse_type_name(p);
                ta->span = span_from(p, first);
                return ta;
            }
            err_here(p, "expected an expression, found the keyword '%.*s'", (int)t->len, p->f->src + t->off);
            return expr_error(p);
        }
        if (tok_is_p(tk(p, 1), CC_P_COLONCOLON) && tok_is_p(tk(p, 2), CC_P_LBRACKET)) {
            CcName nm = tok_name(p, p->i);
            CcType *g;
            adv(p);
            g = parse_generic_args(p, nm, first);
            if (at_p(p, CC_P_LPAREN)) {
                e = cc_expr_new(p->a, CC_E_GENERIC_FN, span2(first, first));
                e->name = nm;
                e->targs = g->args;
                parse_args(p, &e->args, "generic call");
                e->span = span_from(p, first);
                return e;
            }
            e = cc_expr_new(p->a, CC_E_TYPE_ARG, span2(first, first));
            e->type = g;
            e->span = span_from(p, first);
            return e;
        }
        if (tok_is_p(tk(p, 1), CC_P_FAT_ARROW)) return parse_closure(p, first, 0, 0);
        e = cc_expr_new(p->a, CC_E_IDENT, span2(first, first));
        e->name = tok_name(p, p->i);
        e->tok = p->i;
        adv(p);
        return e;
    }
    case CC_TK_NUMBER: case CC_TK_CHAR: {
        CcExpr *e = cc_expr_new(p->a, t->kind == CC_TK_NUMBER ? CC_E_NUMBER : CC_E_CHAR, span2(first, first));
        e->tok = p->i;
        adv(p);
        return e;
    }
    case CC_TK_STRING: {
        CcExpr *e = cc_expr_new(p->a, CC_E_STRING, span2(first, first));
        e->tok = p->i;
        for (;;) {
            if (p->t[p->i].kind == CC_TK_STRING) { e->n_string_toks++; adv(p); continue; }
            /* `"%" PRIu64 "\n"`: a macro that expands to a string literal inside the run */
            if (at_ident(p) && kw_of(p, p->i) == KW_NONE && !name_is_var(p, tok_name(p, p->i)) && !name_is_type(p, tok_name(p, p->i)) &&
                (tk(p, 1)->kind == CC_TK_STRING || tok_is_p(tk(p, 1), CC_P_RPAREN) || tok_is_p(tk(p, 1), CC_P_COMMA) ||
                 tok_is_p(tk(p, 1), CC_P_SEMI)) && name_is_all_caps(tok_name(p, p->i))) {
                e->n_string_toks++;
                adv(p);
                continue;
            }
            break;
        }
        e->span = span_from(p, first);
        return e;
    }
    case CC_TK_TEMPLATE: {
        CcExpr *e;
        if (!p->tpl_ok) err_here(p, "a template literal must be the argument of @string(...) or @emit(...)");
        e = template_literal(p);
        e->span = span_from(p, first);
        return e;
    }
    case CC_TK_PUNCT:
        if (t->punct == CC_P_LPAREN) {
            uint32_t close;
            if (tok_is_p(tk(p, 1), CC_P_LBRACE)) {
                CcExpr *e = cc_expr_new(p->a, CC_E_STMT_EXPR, span2(first, first));
                adv(p);
                e->body = parse_block(p);
                expect_close(p, CC_P_RPAREN, "statement expression", first);
                e->span = span_from(p, first);
                return e;
            }
            close = match_close(p, p->i);
            if (tok_is_p(&p->t[close + 1], CC_P_FAT_ARROW)) return parse_closure(p, first, 0, 0);
            {
                CcExpr *e = cc_expr_new(p->a, CC_E_PAREN, span2(first, first));
                adv(p);
                e->a = parse_expr(p);
                expect_close(p, CC_P_RPAREN, "parenthesised expression", first);
                e->span = span_from(p, first);
                return e;
            }
        }
        if (t->punct == CC_P_DOT && tk(p, 1)->kind == CC_TK_IDENT) {
            CcExpr *e = cc_expr_new(p->a, CC_E_VARIANT_LIT, span2(first, first));
            adv(p);
            e->name = tok_name(p, p->i);
            adv(p);
            if (at_p(p, CC_P_LPAREN)) parse_args(p, &e->args, "variant literal");
            e->span = span_from(p, first);
            return e;
        }
        break;
    case CC_TK_AT_WORD: {
        CcExpr *e;
        if (at_word(p, "string")) return parse_at_string(p);
        if (at_word(p, "emit")) return parse_at_emit(p);
        if (at_word(p, "slice")) {
            e = cc_expr_new(p->a, CC_E_SLICE_LIT, span2(first, first));
            adv(p);
            if (!at_p(p, CC_P_LPAREN)) { err_here(p, "expected '(' after @slice, found %s", tok_desc(p, p->i)); return e; }
            parse_args(p, &e->args, "@slice");
            if (e->args.n != 1 || e->args.items[0]->kind != CC_E_STRING)
                err_at(p, first, "@slice takes exactly one string literal: @slice(\"...\")");
            e->span = span_from(p, first);
            return e;
        }
        if (at_word(p, "scratch")) {
            e = cc_expr_new(p->a, CC_E_SCRATCH, span2(first, first));
            adv(p);
            if (at_p(p, CC_P_LPAREN)) {
                uint32_t open = p->i;
                adv(p);
                e->scratch_bytes = parse_expr(p);
                expect_close(p, CC_P_RPAREN, "@scratch(N)", open);
            }
            e->span = span_from(p, first);
            return e;
        }
        if (at_word(p, "create")) {
            e = cc_expr_new(p->a, CC_E_CREATE, span2(first, first));
            adv(p);
            if (!at_p(p, CC_P_LPAREN)) { err_here(p, "expected '(' after @create, found %s", tok_desc(p, p->i)); return e; }
            parse_args(p, &e->args, "@create");
            e->span = span_from(p, first);
            return e;
        }
        if (at_word(p, "comptime") && tok_is_p(tk(p, 1), CC_P_LPAREN)) {
            uint32_t open = p->i + 1;
            e = cc_expr_new(p->a, CC_E_COMPTIME, span2(first, first));
            adv(p); adv(p);
            e->a = parse_expr(p);
            expect_close(p, CC_P_RPAREN, "@comptime(...)", open);
            e->span = span_from(p, first);
            return e;
        }
        if (at_word(p, "unsafe") && tok_is_p(tk(p, 1), CC_P_LPAREN)) {
            adv(p);
            return parse_closure(p, first, 1, 0);
        }
        if (at_word(p, "destroy") || at_word(p, "detach")) {
            err_here(p, "'%.*s' is only allowed after a declaration initializer", (int)t->len, p->f->src + t->off);
            return expr_error(p);
        }
        if (at_word(p, "parallel")) {
            err_here(p, "@parallel is a statement; bind its join with `CCParallel h = @parallel { ... } !>;`");
            return expr_error(p);
        }
        err_here(p, "unknown @word '%.*s' in an expression", (int)t->len, p->f->src + t->off);
        return expr_error(p);
    }
    case CC_TK_AT:
        err_here(p, "a bare '@' belongs in `T name@(args)`; expected an expression");
        return expr_error(p);
    case CC_TK_PP:
        err_here(p, "a preprocessor line cannot appear inside an expression");
        return expr_error(p);
    case CC_TK_ERROR:
        adv(p);
        return expr_error(p);
    default: break;
    }
    err_here(p, "expected an expression, found %s", tok_desc(p, p->i));
    return expr_error(p);
}

/* ---- initializers ---------------------------------------------------- */

static CcInit *parse_initializer(P *p) {
    CcInit *in = CC_NEW(p->a, CcInit);
    uint32_t first = p->i;
    if (at_p(p, CC_P_LBRACE)) {
        uint32_t open = p->i;
        in->is_list = 1;
        adv(p);
        while (!at_p(p, CC_P_RBRACE) && !at_eof(p)) {
            CcInit *item;
            uint32_t ifirst = p->i;
            if (p->t[p->i].kind == CC_TK_PP) {
                uint32_t last; int skipped;
                item = CC_NEW(p->a, CcInit);
                item->is_pp = 1;
                item->pp_tok = p->i;
                pp_note(p, p->i, &last, &skipped);
                item->span = span2(ifirst, last);
                CC_LIST_PUSH(p->a, &in->list, item);
                continue;
            }
            /* designators: `.f`, `[i]`, `[lo ... hi]`, chained, then `=`;
             * a bare `.arm` is a variant literal, not a designator */
            {
                CcDesignator *head = NULL, **link = &head;
                uint32_t j = p->i;
                int is_desig = 0;
                for (;;) {
                    if (tok_is_p(&p->t[j], CC_P_DOT) && p->t[j + 1].kind == CC_TK_IDENT) j += 2;
                    else if (tok_is_p(&p->t[j], CC_P_LBRACKET)) j = match_close(p, j) + 1;
                    else break;
                }
                if (j > p->i && (tok_is_p(&p->t[j], CC_P_ASSIGN) || (tok_is_p(&p->t[j - 1], CC_P_RBRACKET)))) is_desig = 1;
                if (j > p->i && tok_is_p(&p->t[j], CC_P_ASSIGN)) is_desig = 1;
                if (is_desig) {
                    for (;;) {
                        CcDesignator *ds;
                        if (at_p(p, CC_P_DOT) && tk(p, 1)->kind == CC_TK_IDENT) {
                            ds = CC_NEW(p->a, CcDesignator);
                            adv(p);
                            ds->field = tok_name(p, p->i);
                            adv(p);
                        } else if (at_p(p, CC_P_LBRACKET)) {
                            uint32_t bopen = p->i;
                            ds = CC_NEW(p->a, CcDesignator);
                            adv(p);
                            ds->index = parse_cond(p);
                            if (accept_p(p, CC_P_ELLIPSIS)) ds->index_hi = parse_cond(p);
                            expect_close(p, CC_P_RBRACKET, "designator", bopen);
                        } else break;
                        *link = ds; link = &ds->next;
                    }
                    if (!accept_p(p, CC_P_ASSIGN)) {
                        /* GNU obsolete `field:` form is not supported; `[i]` without `=` neither */
                        err_here(p, "expected '=' after the designator, found %s", tok_desc(p, p->i));
                    }
                }
                item = parse_initializer(p);
                item->designators = head;
                item->span = span_from(p, ifirst);
            }
            CC_LIST_PUSH(p->a, &in->list, item);
            if (p->t[p->i].kind == CC_TK_PP) continue;
            if (!accept_p(p, CC_P_COMMA)) break;
        }
        expect_close(p, CC_P_RBRACE, "initializer list", open);
        in->span = span_from(p, first);
        return in;
    }
    in->expr = parse_assign(p);
    in->span = span_from(p, first);
    return in;
}

/* ======================================================================
 * 4. Statements
 * ====================================================================== */

/* From part 5. */
static int parse_declaration(P *p, CcDeclList *out, int file_scope);
static int at_cc_decl_word(P *p);
static CcDecl *pp_decl(P *p);

/* Does a declaration start here, in statement position? */
static int stmt_is_decl(P *p) {
    const CcToken *t = &p->t[p->i];
    if (t->kind == CC_TK_AT_WORD) {
        if (at_word(p, "typeview") && tok_is_p(tk(p, 1), CC_P_LPAREN)) return 1;
        if (at_word(p, "auto") && tok_is_p(tk(p, 1), CC_P_LPAREN)) return 1;
        if (at_word(p, "async") || at_word(p, "latency_sensitive")) return 1;
        if ((at_word(p, "blocking") || at_word(p, "nonblocking") || at_word(p, "noblock")) &&
            (is_type_start(p, p->i + 1) || tok_is_word(p, p->i + 1, "async")))
            return 1;
        return 0;
    }
    if (t->kind != CC_TK_IDENT) return 0;
    if (tok_is_p(tk(p, 1), CC_P_COLON) && kw_of(p, p->i) == KW_NONE) return 0; /* label */
    if (at_kw(p, KW_STATIC_ASSERT)) return 1;
    {
        Kw k = kw_of(p, p->i);
        if (k != KW_NONE) return is_type_start(p, p->i);
    }
    {
        CcName nm = tok_name(p, p->i);
        if (name_is_type(p, nm)) {
            const CcToken *n1 = tk(p, 1);
            if (tok_is_p(n1, CC_P_DOT)) return 0;
            if (tok_is_p(n1, CC_P_LPAREN) && name_is_all_caps(nm) && !tok_is_p(tk(p, 2), CC_P_STAR) && !tok_is_p(tk(p, 2), CC_P_LPAREN)) {
                /* `CC_CAT_3(a, b, c) name = ...`: a macro-made type */
                uint32_t close = match_close(p, p->i + 1);
                const CcToken *after = &p->t[close + 1];
                return after->kind == CC_TK_IDENT && kw_of(p, close + 1) == KW_NONE && !after->at_line_start &&
                       (tok_is_p(&p->t[close + 2], CC_P_ASSIGN) || tok_is_p(&p->t[close + 2], CC_P_SEMI) ||
                        tok_is_p(&p->t[close + 2], CC_P_COMMA) || tok_is_p(&p->t[close + 2], CC_P_LBRACKET));
            }
            if (tok_is_p(n1, CC_P_COLONCOLON) && tok_is_p(tk(p, 2), CC_P_LBRACKET)) {
                uint32_t close = match_close(p, p->i + 2);
                return !(tok_is_p(&p->t[close + 1], CC_P_LPAREN) || tok_is_p(&p->t[close + 1], CC_P_DOT));
            }
            if (tok_is_p(n1, CC_P_LPAREN) && !tok_is_p(tk(p, 2), CC_P_STAR) && !tok_is_p(tk(p, 2), CC_P_LPAREN) &&
                !tok_is_p(tk(p, 2), CC_P_CARET) && !(tk(p, 2)->kind == CC_TK_IDENT && tok_is_p(tk(p, 3), CC_P_RPAREN)))
                return 0; /* T(...) is a macro call */
            if (tok_is_p(n1, CC_P_ARROW) || tok_is_p(n1, CC_P_ASSIGN) || tok_is_p(n1, CC_P_SEMI)) return 0;
            return 1;
        }
        /* comptime reflection: `m.ret r = ...` declares r with the type m.ret */
        if (tok_is_p(tk(p, 1), CC_P_DOT) && tk(p, 2)->kind == CC_TK_IDENT && tk(p, 3)->kind == CC_TK_IDENT &&
            kw_of(p, p->i + 3) == KW_NONE && (tok_is_p(tk(p, 4), CC_P_ASSIGN) || tok_is_p(tk(p, 4), CC_P_SEMI)))
            return 1;
        if (name_is_var(p, nm)) return 0;
        if (declarator_follows(p, p->i + 1)) return 1;
        /* `MACRO(args) name = ...`: a macro that expands to a type */
        if (tok_is_p(tk(p, 1), CC_P_LPAREN) && !tok_is_p(tk(p, 2), CC_P_STAR) && !tok_is_p(tk(p, 2), CC_P_LPAREN)) {
            uint32_t close = match_close(p, p->i + 1);
            const CcToken *after = &p->t[close + 1];
            if (after->kind == CC_TK_IDENT && kw_of(p, close + 1) == KW_NONE && !after->at_line_start && !is_kw(p, close + 1, KW_IN) &&
                (tok_is_p(&p->t[close + 2], CC_P_ASSIGN) || tok_is_p(&p->t[close + 2], CC_P_SEMI) || tok_is_p(&p->t[close + 2], CC_P_LBRACKET) ||
                 tok_is_p(&p->t[close + 2], CC_P_COMMA) || tok_is_p(&p->t[close + 2], CC_P_LPAREN)))
                return 1;
            if (tok_is_p(after, CC_P_STAR) && !after->at_line_start && p->t[close + 2].kind == CC_TK_IDENT && kw_of(p, close + 2) == KW_NONE)
                return 1;
        }
        return 0;
    }
}

static CcStmt *stmt_decl_wrap(P *p, CcDecl *d) {
    CcStmt *s = cc_stmt_new(p->a, CC_S_DECL, d->span);
    s->decl = d;
    return s;
}

/* The body of if/else/while/for/do. A body wrapped in `#if ... #endif`
 * lines becomes a brace-less block holding the lines and the statements. */
static CcStmt *parse_body_stmt(P *p) {
    CcStmt *b;
    int depth = 0, parsed = 0;
    uint32_t first = p->i;
    if (p->t[p->i].kind != CC_TK_PP) return parse_stmt(p);
    b = cc_stmt_new(p->a, CC_S_BLOCK, span2(first, first));
    for (;;) {
        if (p->t[p->i].kind == CC_TK_PP) {
            size_t n = 0; int v;
            const char *body = pp_body(p, p->i, &n);
            PpKind k = pp_classify(body, n, &v);
            if (k == PP_IF) depth++;
            else if (k == PP_ENDIF) depth--;
            CC_LIST_PUSH(p->a, &b->stmts, stmt_decl_wrap(p, pp_decl(p)));
            if (depth <= 0 && parsed) break;
            continue;
        }
        if (at_eof(p) || at_p(p, CC_P_RBRACE)) break;
        if (at_kw(p, KW_ELSE) && depth > 0) break;
        CC_LIST_PUSH(p->a, &b->stmts, parse_stmt(p));
        parsed = 1;
        if (depth <= 0) break;
    }
    b->span = span_from(p, first);
    return b;
}

static void end_stmt(P *p, const char *what, uint32_t start) {
    if (!expect_semi(p, what)) sync(p, start);
}

/* Parse a for-in header at the first binder; returns a FOR_IN shell with
 * binders and iterables, positioned after `)`. */
static void parse_for_in_header(P *p, CcStmt *s, uint32_t open) {
    if (accept_p(p, CC_P_AMP)) s->by_ref = 1;
    if (!at_ident(p)) { err_here(p, "expected the loop variable in the for-in header, found %s", tok_desc(p, p->i)); }
    else { s->name = tok_name(p, p->i); adv(p); }
    if (accept_p(p, CC_P_COMMA)) {
        if (accept_p(p, CC_P_AMP)) s->by_ref2 = 1;
        if (!at_ident(p)) err_here(p, "expected the second loop variable in the for-in header, found %s", tok_desc(p, p->i));
        else { s->name2 = tok_name(p, p->i); adv(p); }
    }
    if (!accept_kw(p, KW_IN)) err_here(p, "expected 'in' in the for-in header, found %s", tok_desc(p, p->i));
    for (;;) {
        CcExpr *it;
        if (at_p(p, CC_P_LPAREN) && (is_type_start(p, p->i + 1) || tok_is_p(tk(p, 1), CC_P_RPAREN))) {
            /* a parenthesised parameter list as a comptime sequence */
            CcType *fn = cc_type_new(p->a, CC_T_FUNC, span2(p->i, p->i));
            it = cc_expr_new(p->a, CC_E_TYPE_ARG, span2(p->i, p->i));
            parse_params(p, fn);
            fn->span = span_from(p, fn->span.first);
            it->type = fn;
            it->span = fn->span;
        } else it = parse_assign(p);
        if (at_p(p, CC_P_DOTDOT)) {
            CcExpr *r = cc_expr_new(p->a, CC_E_RANGE, it->span);
            adv(p);
            r->a = it;
            r->b = parse_assign(p);
            r->span = span_from(p, it->span.first);
            it = r;
        }
        CC_LIST_PUSH(p->a, &s->exprs, it);
        if (!accept_p(p, CC_P_COMMA)) break;
    }
    if (s->exprs.n) s->expr2 = s->exprs.items[0];
    expect_close(p, CC_P_RPAREN, "for-in header", open);
}

/* `(` [&]IDENT [, [&]IDENT] in  — lookahead for a for-in header */
static int for_in_ahead(P *p, uint32_t i) {
    uint32_t j = i;
    if (tok_is_p(&p->t[j], CC_P_AMP)) j++;
    if (p->t[j].kind != CC_TK_IDENT) return 0;
    j++;
    if (is_kw(p, j, KW_IN)) return 1;
    if (!tok_is_p(&p->t[j], CC_P_COMMA)) return 0;
    j++;
    if (tok_is_p(&p->t[j], CC_P_AMP)) j++;
    if (p->t[j].kind != CC_TK_IDENT) return 0;
    return is_kw(p, j + 1, KW_IN);
}

static CcStmt *parse_for(P *p, int is_at_for) {
    uint32_t first = p->i, open;
    CcStmt *s;
    adv(p); /* for / @for */
    open = p->i;
    if (!at_p(p, CC_P_LPAREN)) {
        err_here(p, "expected '(' after '%s', found %s", is_at_for ? "@for" : "for", tok_desc(p, p->i));
        s = cc_stmt_new(p->a, CC_S_FOR, span_from(p, first));
        return s;
    }
    if (for_in_ahead(p, p->i + 1)) {
        s = cc_stmt_new(p->a, CC_S_FOR_IN, span2(first, first));
        s->is_at_for = is_at_for;
        adv(p);
        parse_for_in_header(p, s, open);
        scope_push(p->syms);
        if (s->name) declare_var(p, s->name);
        if (s->name2) declare_var(p, s->name2);
        s->body = parse_stmt(p);
        scope_pop(p->syms);
        if (s->body && s->body->kind == CC_S_BLOCK && (at_p(p, CC_P_UNWRAP) || at_p(p, CC_P_UNWRAP_OR))) {
            /* `} !>;` / `} !>(e) { ... }`: the walk's Result, unwrapped */
            CcExpr *join = cc_expr_new(p->a, CC_E_IDENT, span2(first, first));
            join->name = p->n_parallel_join;
            join->tok = first;
            s->par_tail = parse_postfix(p, join);
            accept_p(p, CC_P_SEMI);
        }
        s->span = span_from(p, first);
        return s;
    }
    if (is_at_for) err_here(p, "@for needs a for-in header: @for ([&]x in xs)");
    s = cc_stmt_new(p->a, CC_S_FOR, span2(first, first));
    adv(p);
    scope_push(p->syms);
    if (!at_p(p, CC_P_SEMI)) {
        if (stmt_is_decl(p)) {
            CcDeclList dl;
            size_t k;
            memset(&dl, 0, sizeof dl);
            parse_declaration(p, &dl, 0);
            if (dl.n) s->decl = dl.items[0];
            for (k = 1; k < dl.n; k++) CC_LIST_PUSH(p->a, &s->stmts, stmt_decl_wrap(p, dl.items[k]));
        } else {
            s->init_expr = parse_expr(p);
            expect_p(p, CC_P_SEMI, "the for header");
        }
    } else adv(p);
    if (!at_p(p, CC_P_SEMI)) s->expr = parse_expr(p);
    expect_p(p, CC_P_SEMI, "the for header");
    if (!at_p(p, CC_P_RPAREN)) s->expr2 = parse_expr(p);
    expect_close(p, CC_P_RPAREN, "for header", open);
    s->body = parse_body_stmt(p);
    scope_pop(p->syms);
    s->span = span_from(p, first);
    return s;
}

/* Body block into s->stmts; sets has_braces. */
static void parse_body_into(P *p, CcStmt *s, const char *what) {
    CcStmt *b;
    if (!at_p(p, CC_P_LBRACE)) {
        err_here(p, "expected '{' to open the %s body, found %s", what, tok_desc(p, p->i));
        return;
    }
    b = parse_block(p);
    s->stmts = b->stmts;
    s->has_braces = 1;
}

static CcExpr *parse_paren_expr(P *p, const char *what) {
    uint32_t open = p->i;
    CcExpr *e;
    if (!expect_p(p, CC_P_LPAREN, what)) return NULL;
    e = parse_expr(p);
    expect_close(p, CC_P_RPAREN, what, open);
    return e;
}

static void parse_paren_list(P *p, CcExprList *out, const char *what) {
    uint32_t open = p->i;
    if (!at_p(p, CC_P_LPAREN)) { err_here(p, "expected '(' after %s, found %s", what, tok_desc(p, p->i)); return; }
    adv(p);
    while (!at_p(p, CC_P_RPAREN) && !at_eof(p)) {
        CcExpr *e = parse_assign(p);
        CC_LIST_PUSH(p->a, out, e);
        if (!accept_p(p, CC_P_COMMA)) break;
    }
    expect_close(p, CC_P_RPAREN, what, open);
}

/* `@parallel ...` at the `@parallel` token. */
static CcStmt *parse_parallel(P *p) {
    uint32_t first = p->i;
    CcStmt *s;
    adv(p);
    /* `@parallel(h) { }`: no space before the parenthesis */
    if (at_p(p, CC_P_LPAREN) && !p->t[p->i].after_space) {
        s = cc_stmt_new(p->a, CC_S_PARALLEL_DEST, span2(first, first));
        s->par_dest = parse_paren_expr(p, "@parallel(dest)");
        parse_body_into(p, s, "@parallel(dest)");
        s->span = span_from(p, first);
        return s;
    }
    s = cc_stmt_new(p->a, CC_S_PARALLEL, span2(first, first));
    for (;;) {
        if (accept_kw(p, KW_SPAWN)) { s->par_spawn = 1; continue; }
        if (at_p(p, CC_P_LPAREN)) {
            uint32_t open = p->i;
            adv(p);
            if (at_p(p, CC_P_RPAREN)) err_here(p, "@parallel (pred) needs a predicate expression between the parentheses");
            else s->par_pred = parse_expr(p);
            expect_close(p, CC_P_RPAREN, "@parallel (pred)", open);
            continue;
        }
        if (at_kw(p, KW_SEQ) && tok_is_p(tk(p, 1), CC_P_LPAREN)) { adv(p); s->par_seq = parse_paren_expr(p, "seq (cond)"); continue; }
        if (at_kw(p, KW_WAIT) && tok_is_p(tk(p, 1), CC_P_LPAREN)) { adv(p); s->par_wait = parse_paren_expr(p, "wait (ts)"); continue; }
        if (at_kw(p, KW_CACHE) && tok_is_p(tk(p, 1), CC_P_LPAREN)) { adv(p); parse_paren_list(p, &s->par_cache, "cache (...)"); continue; }
        if (at_kw(p, KW_WORKER) && tok_is_p(tk(p, 1), CC_P_LPAREN)) {
            adv(p); adv(p);
            if (at_ident(p)) { s->par_worker = tok_name(p, p->i); adv(p); }
            else err_here(p, "expected the worker binder name in worker (w), found %s", tok_desc(p, p->i));
            expect_p(p, CC_P_RPAREN, "worker (w)");
            continue;
        }
        break;
    }
    if (at_kw(p, KW_FOR) || at_word(p, "for")) {
        uint32_t open = p->i + 1;
        s->kind = CC_S_PARALLEL_FOR;
        s->is_at_for = p->t[p->i].kind == CC_TK_AT_WORD;
        adv(p);
        if (!expect_p(p, CC_P_LPAREN, "@parallel for")) { s->span = span_from(p, first); return s; }
        if (!for_in_ahead(p, p->i)) err_here(p, "@parallel for needs a for-in header: for (i in lo..hi)");
        parse_for_in_header(p, s, open);
        /* trailing clauses after the header */
        for (;;) {
            if (at_kw(p, KW_WORKER) && tok_is_p(tk(p, 1), CC_P_LPAREN)) {
                adv(p); adv(p);
                if (at_ident(p)) { s->par_worker = tok_name(p, p->i); adv(p); }
                else err_here(p, "expected the worker binder name in worker (w), found %s", tok_desc(p, p->i));
                expect_p(p, CC_P_RPAREN, "worker (w)");
                continue;
            }
            if (at_kw(p, KW_CACHE) && tok_is_p(tk(p, 1), CC_P_LPAREN)) { adv(p); parse_paren_list(p, &s->par_cache, "cache (...)"); continue; }
            break;
        }
        scope_push(p->syms);
        if (s->name) declare_var(p, s->name);
        if (s->name2) declare_var(p, s->name2);
        if (s->par_worker) declare_var(p, s->par_worker);
        if (at_p(p, CC_P_LBRACE)) { s->body = parse_block(p); s->has_braces = 1; }
        else s->body = parse_stmt(p);
        scope_pop(p->syms);
    } else {
        /* arms */
        uint32_t open = p->i;
        CcParallelArm **link = &s->arms;
        if (!at_p(p, CC_P_LBRACE)) {
            err_here(p, "expected '{' to open the @parallel arms, found %s", tok_desc(p, p->i));
            s->span = span_from(p, first);
            return s;
        }
        adv(p);
        s->has_braces = 1;
        scope_push(p->syms);
        while (!at_p(p, CC_P_RBRACE) && !at_eof(p)) {
            CcParallelArm *arm = CC_NEW(p->a, CcParallelArm);
            uint32_t afirst = p->i;
            if (p->t[p->i].kind == CC_TK_PP) {
                uint32_t last; int skipped;
                pp_note(p, p->i, &last, &skipped);
                continue;
            }
            if (accept_p(p, CC_P_SEMI)) continue;
            if (at_word(p, "serial")) {
                CcStmt *ser = cc_stmt_new(p->a, CC_S_SERIAL, span2(p->i, p->i));
                adv(p);
                parse_body_into(p, ser, "@serial");
                ser->span = span_from(p, afirst);
                arm->serial = ser;
            } else {
                CcStmt *st = parse_stmt(p);
                if (st->kind == CC_S_UNWRAP && st->expr && st->expr->kind == CC_E_UNWRAP) {
                    arm->unwrap = 1;
                    arm->expr = st->expr->a;
                } else if (st->kind == CC_S_EXPR && st->expr && st->expr->kind == CC_E_ASSIGN && st->expr->op == CC_OP_ASSIGN &&
                           st->expr->a->kind == CC_E_IDENT) {
                    arm->target = st->expr->a->name;
                    arm->expr = st->expr->b;
                } else if (st->kind == CC_S_EXPR && st->expr) {
                    arm->expr = st->expr;
                } else {
                    /* any other statement (a loop, a declaration) is an arm of its own */
                    arm->serial = st;
                }
            }
            arm->span = span_from(p, afirst);
            *link = arm; link = &arm->next;
            if (p->i == afirst) sync(p, afirst);
        }
        scope_pop(p->syms);
        expect_close(p, CC_P_RBRACE, "@parallel arms", open);
    }
    /* tail: `!>.wait()!>` and friends, applied to the join */
    if (at_p(p, CC_P_UNWRAP) || at_p(p, CC_P_UNWRAP_OR) || at_p(p, CC_P_DOT)) {
        CcExpr *join = cc_expr_new(p->a, CC_E_IDENT, span2(first, first));
        join->name = p->n_parallel_join;
        join->tok = first;
        s->par_tail = parse_postfix(p, join);
    }
    accept_p(p, CC_P_SEMI);
    s->span = span_from(p, first);
    return s;
}

static CcStmt *parse_case(P *p) {
    uint32_t first = p->i;
    CcStmt *s = cc_stmt_new(p->a, CC_S_CASE, span2(first, first));
    if (accept_kw(p, KW_DEFAULT)) s->is_default = 1;
    else {
        adv(p); /* case */
        if (at_p(p, CC_P_DOT) && tk(p, 1)->kind == CC_TK_IDENT && !tok_is_p(tk(p, 2), CC_P_DOT)) {
            adv(p);
            s->case_arm = tok_name(p, p->i);
            adv(p);
            if (at_p(p, CC_P_LPAREN)) {
                uint32_t open = p->i;
                adv(p);
                if (at_ident(p)) { s->case_bind = tok_name(p, p->i); declare_var(p, s->case_bind); adv(p); }
                else err_here(p, "expected the binder name in case .%s(bind), found %s", s->case_arm, tok_desc(p, p->i));
                expect_close(p, CC_P_RPAREN, "case arm binder", open);
            }
        } else {
            s->expr = parse_cond(p);
            if (accept_p(p, CC_P_ELLIPSIS)) s->expr2 = parse_cond(p);
        }
    }
    if (!accept_p(p, CC_P_COLON)) {
        err_here(p, "expected ':' after the %s label, found %s", s->is_default ? "default" : "case", tok_desc(p, p->i));
        s->span = span_from(p, first);
        return s;
    }
    if (!at_p(p, CC_P_RBRACE) && !at_eof(p)) s->body = parse_stmt(p);
    s->span = span_from(p, first);
    return s;
}

static CcStmt *parse_at_stmt(P *p) {
    uint32_t first = p->i;
    const CcToken *t = &p->t[p->i];
    CcStmt *s;
    if (at_word(p, "defer")) {
        s = cc_stmt_new(p->a, CC_S_DEFER, span2(first, first));
        adv(p);
        if (at_p(p, CC_P_LPAREN) && !p->t[p->i].after_space && tk(p, 1)->kind == CC_TK_IDENT && tok_is_p(tk(p, 2), CC_P_RPAREN)) {
            if (cc_tok_is(p->f, tk(p, 1), "ok")) s->defer_on = 'o';
            else if (cc_tok_is(p->f, tk(p, 1), "err")) s->defer_on = 'e';
            else err_at(p, p->i + 1, "@defer takes (ok) or (err), not (%.*s)", (int)tk(p, 1)->len, p->f->src + tk(p, 1)->off);
            adv(p); adv(p); adv(p);
        }
        if (at_ident(p) && tok_is_p(tk(p, 1), CC_P_COLON) && kw_of(p, p->i) == KW_NONE) {
            s->name = tok_name(p, p->i);
            adv(p); adv(p);
        }
        s->body = parse_stmt(p);
        s->span = span_from(p, first);
        return s;
    }
    if (at_word(p, "cancel_defer")) {
        s = cc_stmt_new(p->a, CC_S_CANCEL_DEFER, span2(first, first));
        adv(p);
        if (at_ident(p)) { s->name = tok_name(p, p->i); adv(p); }
        else err_here(p, "expected the @defer name after @cancel_defer, found %s", tok_desc(p, p->i));
        end_stmt(p, "@cancel_defer", first);
        s->span = span_from(p, first);
        return s;
    }
    if (at_word(p, "errhandler") && tok_is_p(tk(p, 1), CC_P_LPAREN) && tk(p, 2)->kind == CC_TK_IDENT &&
        tok_is_p(tk(p, 3), CC_P_RPAREN) && tok_is_p(tk(p, 4), CC_P_SEMI)) {
        /* legacy `@errhandler(e);`: forward e to the handler, like `@err(e);` */
        s = cc_stmt_new(p->a, CC_S_ERR_FWD, span2(first, first));
        adv(p);
        s->expr = parse_paren_expr(p, "@errhandler(e)");
        if (s->expr && s->expr->kind == CC_E_IDENT) s->name = s->expr->name;
        end_stmt(p, "@errhandler(e)", first);
        s->span = span_from(p, first);
        return s;
    }
    if (at_word(p, "errhandler")) {
        uint32_t open;
        s = cc_stmt_new(p->a, CC_S_ERRHANDLER, span2(first, first));
        adv(p);
        open = p->i;
        if (!expect_p(p, CC_P_LPAREN, "@errhandler")) { s->span = span_from(p, first); return s; }
        {
            Specs S;
            parse_specs(p, &S, 1);
            if (!S.type) err_here(p, "expected the error type in @errhandler(E e), found %s", tok_desc(p, p->i));
            else {
                uint32_t ntok = 0;
                s->type = parse_declarator(p, S.type, 1, &s->name, &ntok, &S.attrs);
            }
        }
        expect_close(p, CC_P_RPAREN, "@errhandler(E e)", open);
        scope_push(p->syms);
        if (s->name) declare_var(p, s->name);
        s->body = parse_stmt(p);
        scope_pop(p->syms);
        s->span = span_from(p, first);
        return s;
    }
    if (at_word(p, "err")) {
        s = cc_stmt_new(p->a, CC_S_ERR_FWD, span2(first, first));
        adv(p);
        s->expr = parse_paren_expr(p, "@err(e)");
        if (s->expr && s->expr->kind == CC_E_IDENT) s->name = s->expr->name;
        end_stmt(p, "@err(e)", first);
        s->span = span_from(p, first);
        return s;
    }
    if (at_word(p, "parallel")) return parse_parallel(p);
    if (at_word(p, "serial")) {
        err_here(p, "'@serial' is an arm of @parallel { ... } and cannot stand alone");
        s = cc_stmt_new(p->a, CC_S_SERIAL, span2(first, first));
        adv(p);
        parse_body_into(p, s, "@serial");
        s->span = span_from(p, first);
        return s;
    }
    if (at_word(p, "stage")) {
        CcExprList xs;
        size_t k;
        memset(&xs, 0, sizeof xs);
        s = cc_stmt_new(p->a, CC_S_STAGE, span2(first, first));
        adv(p);
        parse_paren_list(p, &xs, "@stage");
        if (xs.n) s->expr = xs.items[0];
        else err_at(p, first, "@stage needs a gate: @stage (gate, args...) { ... }");
        for (k = 1; k < xs.n; k++) CC_LIST_PUSH(p->a, &s->exprs, xs.items[k]);
        parse_body_into(p, s, "@stage");
        s->span = span_from(p, first);
        return s;
    }
    if (at_word(p, "with_deadline")) {
        s = cc_stmt_new(p->a, CC_S_WITH_DEADLINE, span2(first, first));
        adv(p);
        s->expr = parse_paren_expr(p, "@with_deadline");
        scope_push(p->syms);
        if (accept_kw(p, KW_AS)) {
            if (at_ident(p)) { s->name = tok_name(p, p->i); declare_var(p, s->name); adv(p); }
            else err_here(p, "expected the handle name after 'as' in @with_deadline, found %s", tok_desc(p, p->i));
        }
        parse_body_into(p, s, "@with_deadline");
        scope_pop(p->syms);
        s->span = span_from(p, first);
        return s;
    }
    if (at_word(p, "with")) {
        s = cc_stmt_new(p->a, CC_S_WITH, span2(first, first));
        adv(p);
        parse_paren_list(p, &s->exprs, "@with");
        parse_body_into(p, s, "@with");
        s->span = span_from(p, first);
        return s;
    }
    if (at_word(p, "closing")) {
        s = cc_stmt_new(p->a, CC_S_CLOSING, span2(first, first));
        adv(p);
        parse_paren_list(p, &s->exprs, "@closing");
        if (at_p(p, CC_P_LBRACE)) parse_body_into(p, s, "@closing");
        else {
            s->closing_spawn = parse_expr(p);
            end_stmt(p, "@closing(tx) spawn(...)", first);
        }
        s->span = span_from(p, first);
        return s;
    }
    if (at_word(p, "spawn") || at_word(p, "nursery")) {
        s = cc_stmt_new(p->a, CC_S_SPAWN_BLOCK, span2(first, first));
        s->name = cc_intern(p->in, p->f->src + t->off + 1, t->len - 1);
        adv(p);
        if (accept_kw(p, KW_CLOSING)) parse_paren_list(p, &s->exprs, "closing");
        parse_body_into(p, s, s->name);
        s->span = span_from(p, first);
        return s;
    }
    if ((at_word(p, "nonblocking") || at_word(p, "blocking") || at_word(p, "noblock")) && tok_is_p(tk(p, 1), CC_P_LBRACE)) {
        s = cc_stmt_new(p->a, CC_S_MODE_BLOCK, span2(first, first));
        s->name = cc_intern(p->in, p->f->src + t->off + 1, t->len - 1);
        adv(p);
        parse_body_into(p, s, s->name);
        s->span = span_from(p, first);
        return s;
    }
    if (at_word(p, "switch")) {
        s = cc_stmt_new(p->a, CC_S_SWITCH, span2(first, first));
        s->is_variant_switch = 1;
        adv(p);
        s->expr = parse_paren_expr(p, "@switch");
        s->body = parse_stmt(p);
        s->span = span_from(p, first);
        return s;
    }
    if (at_word(p, "for")) return parse_for(p, 1);
    if (at_word(p, "comptime")) {
        const CcToken *n1 = tk(p, 1);
        if (is_kw(p, p->i + 1, KW_IF)) {
            s = cc_stmt_new(p->a, CC_S_COMPTIME_IF, span2(first, first));
            adv(p); adv(p);
            s->expr = parse_paren_expr(p, "@comptime if");
            s->body = parse_stmt(p);
            if (accept_kw(p, KW_ELSE)) s->else_body = parse_stmt(p);
            s->span = span_from(p, first);
            return s;
        }
        if (is_kw(p, p->i + 1, KW_FOR)) {
            uint32_t open;
            s = cc_stmt_new(p->a, CC_S_COMPTIME_FOR, span2(first, first));
            adv(p); adv(p);
            open = p->i;
            if (!expect_p(p, CC_P_LPAREN, "@comptime for")) { s->span = span_from(p, first); return s; }
            if (!for_in_ahead(p, p->i)) err_here(p, "@comptime for needs a for-in header: @comptime for (m in seq)");
            parse_for_in_header(p, s, open);
            scope_push(p->syms);
            if (s->name) declare_var(p, s->name);
            if (s->name2) declare_var(p, s->name2);
            if (at_p(p, CC_P_LBRACE)) parse_body_into(p, s, "@comptime for");
            else s->body = parse_stmt(p);
            scope_pop(p->syms);
            s->span = span_from(p, first);
            return s;
        }
        if (tok_is_p(n1, CC_P_LBRACE)) {
            s = cc_stmt_new(p->a, CC_S_COMPTIME_BLOCK, span2(first, first));
            adv(p);
            parse_body_into(p, s, "@comptime");
            s->span = span_from(p, first);
            return s;
        }
        if (tok_is_p(n1, CC_P_LPAREN)) {
            /* @comptime(expr) as an expression statement */
            s = cc_stmt_new(p->a, CC_S_EXPR, span2(first, first));
            s->expr = parse_expr(p);
            end_stmt(p, "the expression statement", first);
            s->span = span_from(p, first);
            return s;
        }
        /* `@comptime stmt`: a single comptime statement */
        s = cc_stmt_new(p->a, CC_S_COMPTIME_BLOCK, span2(first, first));
        adv(p);
        {
            CcStmt *inner = parse_stmt(p);
            CC_LIST_PUSH(p->a, &s->stmts, inner);
        }
        s->span = span_from(p, first);
        return s;
    }
    if (at_word(p, "destroy") || at_word(p, "detach")) {
        err_here(p, "'%.*s' is only allowed after a declaration initializer", (int)t->len, p->f->src + t->off);
        s = cc_stmt_new(p->a, CC_S_EXPR, span2(first, first));
        sync(p, first);
        s->span = span_from(p, first);
        return s;
    }
    if (at_cc_decl_word(p)) {
        CcDeclList dl;
        memset(&dl, 0, sizeof dl);
        parse_declaration(p, &dl, 0);
        if (dl.n == 1) return stmt_decl_wrap(p, dl.items[0]);
        s = cc_stmt_new(p->a, CC_S_BLOCK, span_from(p, first));
        {
            size_t k;
            for (k = 0; k < dl.n; k++) CC_LIST_PUSH(p->a, &s->stmts, stmt_decl_wrap(p, dl.items[k]));
        }
        return s;
    }
    if (at_word(p, "match") || at_word(p, "arena") || at_word(p, "arena_init") || at_word(p, "restricted") ||
        at_word(p, "spawn_async") || at_word(p, "cache")) {
        err_here(p, "'%.*s' is not part of the language", (int)t->len, p->f->src + t->off);
        s = cc_stmt_new(p->a, CC_S_EXPR, span2(first, first));
        sync(p, first);
        s->span = span_from(p, first);
        return s;
    }
    /* expression statement starting with an @word (@await, @string, ...) */
    {
        CcExpr *e;
        if (!(at_word(p, "await") || at_word(p, "blocking") || at_word(p, "nonblocking") || at_word(p, "noblock") ||
              at_word(p, "string") || at_word(p, "emit") || at_word(p, "slice") || at_word(p, "scratch") ||
              at_word(p, "create") || at_word(p, "unsafe"))) {
            err_here(p, "unknown @word '%.*s'", (int)t->len, p->f->src + t->off);
            s = cc_stmt_new(p->a, CC_S_EXPR, span2(first, first));
            sync(p, first);
            s->span = span_from(p, first);
            return s;
        }
        e = parse_expr(p);
        s = cc_stmt_new(p->a, (e->kind == CC_E_UNWRAP || e->kind == CC_E_UNWRAP_BODY) ? CC_S_UNWRAP : CC_S_EXPR, e->span);
        s->expr = e;
        end_stmt(p, "the expression statement", first);
        s->span = span_from(p, first);
        return s;
    }
}

static CcStmt *parse_stmt(P *p) {
    const CcToken *t = &p->t[p->i];
    uint32_t first = p->i;
    CcStmt *s;
    p->bind_stmt = NULL;
    if (t->kind == CC_TK_PP) {
        CcDecl *d = pp_decl(p);
        return stmt_decl_wrap(p, d);
    }
    if (t->kind == CC_TK_EOF) {
        err_here(p, "expected a statement, found end of file");
        return cc_stmt_new(p->a, CC_S_EXPR, span2(first, first));
    }
    if (t->kind == CC_TK_ERROR) {
        adv(p);
        return cc_stmt_new(p->a, CC_S_EXPR, span2(first, first));
    }
    if (t->kind == CC_TK_PUNCT) {
        if (t->punct == CC_P_SEMI) { adv(p); return cc_stmt_new(p->a, CC_S_EXPR, span2(first, first)); }
        if (t->punct == CC_P_LBRACE) return parse_block(p);
    }
    if (t->kind == CC_TK_AT_WORD) return parse_at_stmt(p);
    if (t->kind == CC_TK_IDENT) {
        Kw k = kw_of(p, p->i);
        switch (k) {
        case KW_IF: {
            s = cc_stmt_new(p->a, CC_S_IF, span2(first, first));
            adv(p);
            s->expr = parse_paren_expr(p, "if");
            s->body = parse_body_stmt(p);
            {
                /* `#ifdef X ... else ...`: preprocessor lines between the body and its else
                 * join the then-body as a brace-less block */
                uint32_t j = p->i;
                while (p->t[j].kind == CC_TK_PP) j++;
                if (j > p->i && is_kw(p, j, KW_ELSE)) {
                    CcStmt *b = s->body;
                    if (!b || b->kind != CC_S_BLOCK || b->has_braces) {
                        b = cc_stmt_new(p->a, CC_S_BLOCK, s->body ? s->body->span : span2(p->i, p->i));
                        if (s->body) CC_LIST_PUSH(p->a, &b->stmts, s->body);
                        s->body = b;
                    }
                    while (p->t[p->i].kind == CC_TK_PP) CC_LIST_PUSH(p->a, &b->stmts, stmt_decl_wrap(p, pp_decl(p)));
                    b->span.last = last_tok(p);
                }
            }
            if (accept_kw(p, KW_ELSE)) s->else_body = parse_body_stmt(p);
            s->span = span_from(p, first);
            return s;
        }
        case KW_WHILE: {
            s = cc_stmt_new(p->a, CC_S_WHILE, span2(first, first));
            adv(p);
            s->expr = parse_paren_expr(p, "while");
            s->body = parse_body_stmt(p);
            s->span = span_from(p, first);
            return s;
        }
        case KW_DO: {
            s = cc_stmt_new(p->a, CC_S_DO, span2(first, first));
            adv(p);
            s->body = parse_body_stmt(p);
            if (!accept_kw(p, KW_WHILE)) err_here(p, "expected 'while' after the do body, found %s", tok_desc(p, p->i));
            else s->expr = parse_paren_expr(p, "do-while");
            end_stmt(p, "the do-while statement", first);
            s->span = span_from(p, first);
            return s;
        }
        case KW_FOR: return parse_for(p, 0);
        case KW_SWITCH: {
            s = cc_stmt_new(p->a, CC_S_SWITCH, span2(first, first));
            adv(p);
            s->expr = parse_paren_expr(p, "switch");
            s->body = parse_stmt(p);
            s->span = span_from(p, first);
            return s;
        }
        case KW_CASE: case KW_DEFAULT: return parse_case(p);
        case KW_BREAK: case KW_CONTINUE: {
            s = cc_stmt_new(p->a, k == KW_BREAK ? CC_S_BREAK : CC_S_CONTINUE, span2(first, first));
            adv(p);
            end_stmt(p, k == KW_BREAK ? "break" : "continue", first);
            s->span = span_from(p, first);
            return s;
        }
        case KW_RETURN: {
            s = cc_stmt_new(p->a, CC_S_RETURN, span2(first, first));
            adv(p);
            if (!at_p(p, CC_P_SEMI)) s->expr = parse_expr(p);
            end_stmt(p, "return", first);
            s->span = span_from(p, first);
            return s;
        }
        case KW_GOTO: {
            s = cc_stmt_new(p->a, CC_S_GOTO, span2(first, first));
            adv(p);
            if (at_ident(p)) { s->name = tok_name(p, p->i); adv(p); }
            else if (at_p(p, CC_P_STAR)) s->expr = parse_expr(p); /* GNU computed goto */
            else err_here(p, "expected a label after goto, found %s", tok_desc(p, p->i));
            end_stmt(p, "goto", first);
            s->span = span_from(p, first);
            return s;
        }
        case KW_ASM: case KW_ASM2: case KW_ASM3: {
            s = cc_stmt_new(p->a, CC_S_ASM, span2(first, first));
            adv(p);
            while (at_ident(p) && kw_of(p, p->i) != KW_NONE) adv(p); /* volatile, goto, inline */
            if (at_p(p, CC_P_LPAREN)) skip_balanced(p);
            else err_here(p, "expected '(' after asm, found %s", tok_desc(p, p->i));
            end_stmt(p, "the asm statement", first);
            s->span = span_from(p, first);
            return s;
        }
        case KW_UNSAFE:
            if (tok_is_p(tk(p, 1), CC_P_LBRACE)) {
                s = cc_stmt_new(p->a, CC_S_UNSAFE, span2(first, first));
                adv(p);
                parse_body_into(p, s, "unsafe");
                s->span = span_from(p, first);
                return s;
            }
            break;
        case KW_ELSE:
            err_here(p, "'else' without a matching 'if'");
            adv(p);
            return parse_stmt(p);
        default: break;
        }
        if (k == KW_NONE && tok_is_p(tk(p, 1), CC_P_COLON) && !tok_is_p(tk(p, 2), CC_P_COLON)) {
            s = cc_stmt_new(p->a, CC_S_LABEL, span2(first, first));
            s->name = tok_name(p, p->i);
            adv(p); adv(p);
            {
                CcAttr *la = NULL;
                parse_attrs(p, &la);
            }
            if (!at_p(p, CC_P_RBRACE) && !at_eof(p)) s->body = parse_stmt(p);
            s->span = span_from(p, first);
            return s;
        }
    }
    if (stmt_is_decl(p)) {
        CcDeclList dl;
        memset(&dl, 0, sizeof dl);
        parse_declaration(p, &dl, 0);
        if (p->bind_stmt) { s = p->bind_stmt; p->bind_stmt = NULL; return s; }
        if (dl.n == 1) return stmt_decl_wrap(p, dl.items[0]);
        s = cc_stmt_new(p->a, CC_S_BLOCK, span_from(p, first));
        {
            size_t k;
            for (k = 0; k < dl.n; k++) CC_LIST_PUSH(p->a, &s->stmts, stmt_decl_wrap(p, dl.items[k]));
        }
        return s;
    }
    if (at_ident(p) && kw_of(p, p->i) == KW_NONE && tok_is_p(tk(p, 1), CC_P_LBRACE) && !name_is_var(p, tok_name(p, p->i)) &&
        !name_is_type(p, tok_name(p, p->i))) {
        /* `try { ... }`: a macro statement with a body */
        CcExpr *e = cc_expr_new(p->a, CC_E_IDENT, span2(first, first));
        e->name = tok_name(p, p->i);
        e->tok = p->i;
        adv(p);
        s = cc_stmt_new(p->a, CC_S_EXPR, e->span);
        s->expr = e;
        s->body = parse_block(p);
        s->has_braces = 1;
        s->span = span_from(p, first);
        return s;
    }
    {
        CcExpr *e = parse_expr(p);
        if (p->bind_stmt) { s = p->bind_stmt; p->bind_stmt = NULL; s->span = span_from(p, first); return s; }
        s = cc_stmt_new(p->a, (e->kind == CC_E_UNWRAP || e->kind == CC_E_UNWRAP_BODY) ? CC_S_UNWRAP : CC_S_EXPR, e->span);
        s->expr = e;
        if (e->kind == CC_E_CALL && e->a->kind == CC_E_IDENT && at_p(p, CC_P_LBRACE) && !name_is_var(p, e->a->name)) {
            /* `FOREACH(m, k, v) { ... }`: a macro loop with a body */
            s->body = parse_block(p);
            s->has_braces = 1;
            s->span = span_from(p, first);
            return s;
        }
        if (e->kind == CC_E_CALL && e->a->kind == CC_E_IDENT && !name_is_var(p, e->a->name) && !name_is_type(p, e->a->name) &&
            p->t[p->i].at_line_start && tok_is_p(&p->t[p->i - 1], CC_P_RPAREN) &&
            (p->t[p->i].kind == CC_TK_PP || p->t[p->i].kind == CC_TK_IDENT || p->t[p->i].kind == CC_TK_AT_WORD ||
             at_p(p, CC_P_RBRACE) || at_p(p, CC_P_STAR) || at_p(p, CC_P_AMP))) {
            /* `MACRO(args)` alone on its line, the callee undeclared, and the next line unable to
             * continue the expression: a macro statement without `;` (yyjson's repeatN_incr) */
            s->span = span_from(p, first);
            return s;
        }
        end_stmt(p, "the expression statement", first);
        s->span = span_from(p, first);
        return s;
    }
}

/* Append the statement(s) starting here to `list`; a declaration with
 * several declarators contributes one CC_S_DECL each. */
static void parse_stmt_into(P *p, CcStmtList *list) {
    uint32_t start = p->i;
    if (p->t[p->i].kind == CC_TK_PP) {
        CC_LIST_PUSH(p->a, list, stmt_decl_wrap(p, pp_decl(p)));
        return;
    }
    if (p->t[p->i].kind != CC_TK_AT_WORD && stmt_is_decl(p)) {
        CcDeclList dl;
        size_t k;
        memset(&dl, 0, sizeof dl);
        p->bind_stmt = NULL;
        parse_declaration(p, &dl, 0);
        if (p->bind_stmt) { CC_LIST_PUSH(p->a, list, p->bind_stmt); p->bind_stmt = NULL; }
        else for (k = 0; k < dl.n; k++) CC_LIST_PUSH(p->a, list, stmt_decl_wrap(p, dl.items[k]));
    } else {
        CcStmt *s = parse_stmt(p);
        CC_LIST_PUSH(p->a, list, s);
    }
    if (p->i == start) sync(p, start);
}

static CcStmt *parse_block(P *p) {
    uint32_t open = p->i;
    CcStmt *b = cc_stmt_new(p->a, CC_S_BLOCK, span2(open, open));
    if (!at_p(p, CC_P_LBRACE)) {
        err_here(p, "expected '{', found %s", tok_desc(p, p->i));
        return b;
    }
    adv(p);
    b->has_braces = 1;
    scope_push(p->syms);
    while (!at_p(p, CC_P_RBRACE) && !at_eof(p)) parse_stmt_into(p, &b->stmts);
    scope_pop(p->syms);
    expect_close(p, CC_P_RBRACE, "block", open);
    b->span = span_from(p, open);
    return b;
}

/* ======================================================================
 * 5. Declarations and the unit
 * ====================================================================== */

/* A preprocessor line as a declaration, with `#pragma(@...)` recognised and
 * inactive `#if` regions skipped. */
static CcDecl *pp_decl(P *p) {
    uint32_t i = p->i, last;
    int skipped;
    size_t n;
    const char *s = pp_body(p, i, &n);
    CcDecl *d;
    const CcToken *t = &p->t[i];
    if (t->len >= 2 && p->f->src[t->off + 1] == '!' && i == 0) p->u->has_shebang = 1;
    if (n >= 8 && memcmp(s, "pragma(@", 8) == 0) {
        const char *q = s + 8, *e = s + n, *ne;
        d = cc_decl_new(p->a, CC_D_PRAGMA_CC, span2(i, i));
        ne = q;
        while (ne < e && *ne != ')') ne++;
        d->pragma_name = cc_intern(p->in, q, (size_t)(ne - q));
        if (ne < e) {
            const char *v = ne + 1, *ve;
            while (v < e && (*v == ' ' || *v == '\t')) v++;
            ve = v;
            while (ve < e && *ve != ' ' && *ve != '\t' && *ve != '\r') ve++;
            if (ve > v) d->pragma_value = cc_intern(p->in, v, (size_t)(ve - v));
        } else err_at(p, i, "expected ')' to close #pragma(@%s", d->pragma_name);
        d->tok = i;
        adv(p);
        return d;
    }
    d = cc_decl_new(p->a, CC_D_PP, span2(i, i));
    d->tok = i;
    pp_note(p, i, &last, &skipped);
    d->span.last = last;
    d->pp_skipped_region = skipped;
    return d;
}

static int at_cc_decl_word(P *p) {
    if (p->t[p->i].kind != CC_TK_AT_WORD) return 0;
    if (at_word(p, "typeview")) return !tok_is_p(tk(p, 1), CC_P_LPAREN);
    return at_word(p, "typehooks") || at_word(p, "variant") || at_word(p, "grammar") || at_word(p, "scoped") ||
           at_word(p, "link") || (at_word(p, "comptime") && !tok_is_p(tk(p, 1), CC_P_LPAREN));
}

/* The target of `on`: a type, `T*`, `Name_*`, or `*`. */
static CcType *parse_hook_target(P *p) {
    uint32_t first = p->i;
    if (at_p(p, CC_P_STAR)) {
        CcType *t = type_named(p, cc_intern(p->in, "*", 1), first);
        adv(p);
        t->span = span_from(p, first);
        return t;
    }
    if (at_ident(p) && kw_of(p, p->i) == KW_NONE && tok_is_p(tk(p, 1), CC_P_STAR) && adjacent(p, p->i) &&
        p->f->src[p->t[p->i].off + p->t[p->i].len - 1] == '_') {
        CcName nm = cc_intern(p->in, p->f->src + p->t[p->i].off, p->t[p->i].len + 1);
        CcType *t = type_named(p, nm, first);
        adv(p); adv(p);
        t->span = span_from(p, first);
        return t;
    }
    return parse_type_name(p);
}

static CcDecl *parse_typehooks(P *p) {
    uint32_t first = p->i, open;
    CcDecl *d = cc_decl_new(p->a, CC_D_TYPEHOOKS, span2(first, first));
    CcHookEntry **link = &d->entries;
    adv(p);
    if (!accept_kw(p, KW_ON)) err_here(p, "expected 'on' after @typehooks, found %s", tok_desc(p, p->i));
    d->type = parse_hook_target(p);
    open = p->i;
    if (!expect_p(p, CC_P_LBRACE, "@typehooks on T { ... }")) { d->span = span_from(p, first); return d; }
    while (!at_p(p, CC_P_RBRACE) && !at_eof(p)) {
        CcHookEntry *h = CC_NEW(p->a, CcHookEntry);
        uint32_t hfirst = p->i;
        if (p->t[p->i].kind == CC_TK_PP) { uint32_t last; int sk; pp_note(p, p->i, &last, &sk); continue; }
        accept_p(p, CC_P_DOT);
        if (!at_ident(p)) {
            err_here(p, "expected a hook name like .destroy in the @typehooks body, found %s", tok_desc(p, p->i));
            sync(p, hfirst);
            if (at_p(p, CC_P_RBRACE)) break;
            continue;
        }
        h->field = tok_name(p, p->i);
        adv(p);
        if (!accept_p(p, CC_P_ASSIGN)) {
            err_here(p, "expected '=' after .%s in the @typehooks body, found %s", h->field, tok_desc(p, p->i));
        } else if (at_p(p, CC_P_LBRACE)) {
            h->body = parse_block(p);
        } else {
            h->value = parse_assign(p);
        }
        h->span = span_from(p, hfirst);
        *link = h; link = &h->next;
        if (!accept_p(p, CC_P_COMMA)) {
            if (!at_p(p, CC_P_RBRACE)) { err_here(p, "expected ',' or '}' after the .%s hook, found %s", h->field, tok_desc(p, p->i)); sync(p, hfirst); if (!at_p(p, CC_P_RBRACE)) continue; }
            break;
        }
    }
    expect_close(p, CC_P_RBRACE, "@typehooks body", open);
    accept_p(p, CC_P_SEMI);
    d->span = span_from(p, first);
    return d;
}

/* One `key: items;` line of a @typeview body. */
static CcHookEntry *parse_view_entry(P *p) {
    CcHookEntry *h = CC_NEW(p->a, CcHookEntry);
    CcViewItem **link = &h->items;
    uint32_t first = p->i;
    h->field = tok_name(p, p->i);
    adv(p); /* key */
    adv(p); /* : */
    while (!at_p(p, CC_P_SEMI) && !at_p(p, CC_P_RBRACE) && !at_eof(p)) {
        CcViewItem *it = CC_NEW(p->a, CcViewItem);
        uint32_t ifirst = p->i;
        if (accept_p(p, CC_P_CARET)) it->deny = 1;
        if (at_p(p, CC_P_LPAREN)) {
            uint32_t open = p->i;
            adv(p);
            it->cast = parse_type_name(p);
            expect_close(p, CC_P_RPAREN, "face cast", open);
        }
        if (at_ident(p) || at_p(p, CC_P_STAR)) {
            /* glue adjacent identifier / `*` tokens into one pattern */
            uint32_t start = p->t[p->i].off, end;
            adv(p);
            while ((at_ident(p) || at_p(p, CC_P_STAR)) && p->t[p->i].lead_len == 0) adv(p);
            end = p->t[p->i - 1].off + p->t[p->i - 1].len;
            it->name = cc_intern(p->in, p->f->src + start, end - start);
        } else if (!it->cast) {
            err_here(p, "expected a member name in the @typeview '%s:' list, found %s", h->field, tok_desc(p, p->i));
            break;
        }
        it->span = span_from(p, ifirst);
        *link = it; link = &it->next;
        if (!accept_p(p, CC_P_COMMA)) break;
    }
    if (!accept_p(p, CC_P_SEMI)) err_here(p, "expected ';' after the @typeview '%s:' list, found %s", h->field, tok_desc(p, p->i));
    h->span = span_from(p, first);
    return h;
}

/* `@typeview [Name] on T { ... }` at `@typeview`; `is_typedef` when a
 * `typedef` came before (the alias declarator follows the body). */
static void parse_typeview(P *p, CcDeclList *out, uint32_t first, int is_typedef) {
    CcDecl *d = cc_decl_new(p->a, CC_D_TYPEVIEW, span2(first, first));
    CcHookEntry **link = &d->entries;
    uint32_t open;
    adv(p); /* @typeview */
    if (at_ident(p) && !at_kw(p, KW_ON)) { d->name = tok_name(p, p->i); adv(p); }
    if (!accept_kw(p, KW_ON)) err_here(p, "expected 'on' after @typeview, found %s", tok_desc(p, p->i));
    d->type = parse_hook_target(p);
    open = p->i;
    if (!expect_p(p, CC_P_LBRACE, "@typeview on T { ... }")) { d->span = span_from(p, first); CC_LIST_PUSH(p->a, out, d); return; }
    while (!at_p(p, CC_P_RBRACE) && !at_eof(p)) {
        uint32_t hfirst = p->i;
        if (p->t[p->i].kind == CC_TK_PP) { uint32_t last; int sk; pp_note(p, p->i, &last, &sk); continue; }
        if (at_ident(p) && tok_is_p(tk(p, 1), CC_P_COLON)) {
            CcHookEntry *h = parse_view_entry(p);
            *link = h; link = &h->next;
        } else if (at_p(p, CC_P_DOT) || (at_ident(p) && tok_is_p(tk(p, 1), CC_P_ASSIGN))) {
            CcHookEntry *h = CC_NEW(p->a, CcHookEntry);
            accept_p(p, CC_P_DOT);
            h->field = tok_name(p, p->i);
            adv(p);
            if (accept_p(p, CC_P_ASSIGN)) {
                if (at_p(p, CC_P_LBRACE)) h->body = parse_block(p); else h->value = parse_assign(p);
            } else err_here(p, "expected '=' after %s in the @typeview body", h->field);
            if (!accept_p(p, CC_P_COMMA)) accept_p(p, CC_P_SEMI);
            h->span = span_from(p, hfirst);
            *link = h; link = &h->next;
        } else {
            err_here(p, "expected a @typeview entry like `as: field;` or `r: a, b;`, found %s", tok_desc(p, p->i));
            sync(p, hfirst);
            if (at_p(p, CC_P_RBRACE)) break;
        }
        if (p->i == hfirst) sync(p, hfirst);
    }
    expect_close(p, CC_P_RBRACE, "@typeview body", open);
    if (is_typedef) {
        /* `typedef @typeview V on T { } *Alias;`: the alias is a typedef of T with the view */
        d->span = span_from(p, first);
        CC_LIST_PUSH(p->a, out, d);
        for (;;) {
            uint32_t dfirst = p->i;
            CcDecl *td = cc_decl_new(p->a, CC_D_TYPEDEF, span2(dfirst, dfirst));
            CcType *base = type_named(p, d->type->name, d->type->span.first);
            CcAttr *va = CC_NEW(p->a, CcAttr);
            uint32_t ntok = 0;
            base->span = d->type->span;
            va->span = span2(first, first);
            va->name = p->n_typeview;
            va->value = d->name;
            base->attrs = va;
            td->specs = CC_S_TYPEDEF;
            td->type = parse_declarator(p, base, 0, &td->name, &ntok, &td->attrs);
            if (td->name) declare_type(p, td->name);
            if (accept_p(p, CC_P_COMMA)) { td->span = span_from(p, dfirst); CC_LIST_PUSH(p->a, out, td); continue; }
            if (!expect_semi(p, "the typedef")) sync(p, dfirst);
            td->span = span_from(p, dfirst);
            CC_LIST_PUSH(p->a, out, td);
            break;
        }
        return;
    }
    accept_p(p, CC_P_SEMI);
    d->span = span_from(p, first);
    CC_LIST_PUSH(p->a, out, d);
}

static CcDecl *parse_variant(P *p) {
    uint32_t first = p->i, open;
    CcDecl *d = cc_decl_new(p->a, CC_D_VARIANT, span2(first, first));
    CcVariantArm **link = &d->arms;
    adv(p);
    if (at_p(p, CC_P_LPAREN)) {
        uint32_t afirst = p->i;
        adv(p);
        if (at_ident(p)) { attr_append(&d->attrs, attr_new(p, afirst, tok_name(p, p->i), NULL)); adv(p); }
        else err_here(p, "expected a layout word like 'packed' in @variant(...), found %s", tok_desc(p, p->i));
        expect_close(p, CC_P_RPAREN, "@variant(...)", afirst);
        if (d->attrs) d->attrs->span = span_from(p, afirst);
    }
    if (at_ident(p) && kw_of(p, p->i) == KW_NONE) { d->name = tok_name(p, p->i); declare_type(p, d->name); adv(p); }
    else err_here(p, "expected the variant name after @variant, found %s", tok_desc(p, p->i));
    open = p->i;
    if (!expect_p(p, CC_P_LBRACE, "@variant Name { ... }")) { d->span = span_from(p, first); return d; }
    while (!at_p(p, CC_P_RBRACE) && !at_eof(p)) {
        CcVariantArm *arm = CC_NEW(p->a, CcVariantArm);
        uint32_t afirst = p->i;
        if (p->t[p->i].kind == CC_TK_PP) { uint32_t last; int sk; pp_note(p, p->i, &last, &sk); continue; }
        if (!at_ident(p)) {
            err_here(p, "expected an arm like `name: Type;` in the @variant body, found %s", tok_desc(p, p->i));
            sync(p, afirst);
            if (at_p(p, CC_P_RBRACE)) break;
            continue;
        }
        arm->name = tok_name(p, p->i);
        adv(p);
        if (accept_p(p, CC_P_COLON)) {
            CcParam *pa = CC_NEW(p->a, CcParam);
            uint32_t pfirst = p->i;
            pa->type = parse_type_name(p);
            pa->span = span_from(p, pfirst);
            CC_LIST_PUSH(p->a, &arm->payload, pa);
        } else if (at_p(p, CC_P_LPAREN)) {
            CcType shell;
            memset(&shell, 0, sizeof shell);
            parse_params(p, &shell);
            arm->payload = shell.params;
        }
        if (!accept_p(p, CC_P_SEMI)) {
            if (!accept_p(p, CC_P_COMMA))
                err_here(p, "expected ';' after the variant arm '%s', found %s", arm->name, tok_desc(p, p->i));
        }
        arm->span = span_from(p, afirst);
        *link = arm; link = &arm->next;
    }
    expect_close(p, CC_P_RBRACE, "@variant body", open);
    accept_p(p, CC_P_SEMI);
    d->span = span_from(p, first);
    return d;
}

static CcDecl *parse_grammar(P *p) {
    uint32_t first = p->i, open, fence = 0, j;
    CcDecl *d = cc_decl_new(p->a, CC_D_GRAMMAR, span2(first, first));
    adv(p);
    if (at_p(p, CC_P_LPAREN)) {
        uint32_t o = p->i;
        adv(p);
        if (at_ident(p)) { d->engine = tok_name(p, p->i); adv(p); }
        else err_here(p, "expected the engine name in @grammar(engine), found %s", tok_desc(p, p->i));
        expect_close(p, CC_P_RPAREN, "@grammar(engine)", o);
    } else err_here(p, "expected '(engine)' after @grammar, found %s", tok_desc(p, p->i));
    if (at_ident(p)) { d->name = tok_name(p, p->i); declare_type(p, d->name); adv(p); }
    else err_here(p, "expected the grammar name after @grammar(engine), found %s", tok_desc(p, p->i));
    open = p->i;
    if (!expect_p(p, CC_P_LBRACE, "@grammar body")) { d->span = span_from(p, first); return d; }
    /* the fence: a run of adjacent `~` right after `{` */
    j = p->i;
    while (tok_is_p(&p->t[j], CC_P_TILDE) && (j == p->i || p->t[j].lead_len == 0)) { fence++; j++; }
    if (!fence) {
        err_here(p, "expected a '~~~~' fence after '{' to open the @grammar body, found %s", tok_desc(p, p->i));
        skip_balanced(p);
        p->i = open;
        skip_balanced(p);
        d->span = span_from(p, first);
        return d;
    }
    d->body_off = p->t[j - 1].off + p->t[j - 1].len;
    /* find the closing fence: `fence` adjacent `~` followed by `}` */
    for (;;) {
        uint32_t k = j, run = 0;
        if (p->t[j].kind == CC_TK_EOF) {
            err_at(p, open, "unterminated @grammar body: no closing fence before end of file");
            d->body_len = p->f->len - d->body_off;
            p->i = j;
            d->span = span_from(p, first);
            return d;
        }
        while (tok_is_p(&p->t[k], CC_P_TILDE) && (k == j || p->t[k].lead_len == 0)) { run++; k++; }
        if (run == fence && tok_is_p(&p->t[k], CC_P_RBRACE)) {
            d->body_len = p->t[j].off - d->body_off;
            p->i = k + 1;
            break;
        }
        j = run ? k : j + 1;
    }
    accept_p(p, CC_P_SEMI);
    d->span = span_from(p, first);
    return d;
}

static CcDecl *parse_factory(P *p) {
    uint32_t first = p->i, open;
    CcDecl *d = cc_decl_new(p->a, CC_D_GENERIC_FACTORY, span2(first, first));
    d->factory_extend = at_kw(p, KW_CC_GENERIC_FACTORY_EXTEND);
    adv(p);
    open = p->i;
    if (!expect_p(p, CC_P_LPAREN, "CC_GENERIC_FACTORY(Name)")) { d->span = span_from(p, first); return d; }
    if (at_ident(p)) { d->name = tok_name(p, p->i); adv(p); }
    else err_here(p, "expected the family name in CC_GENERIC_FACTORY(Name), found %s", tok_desc(p, p->i));
    if (accept_p(p, CC_P_COMMA)) d->factory_arity = parse_assign(p);
    expect_close(p, CC_P_RPAREN, "CC_GENERIC_FACTORY(Name)", open);
    if (at_p(p, CC_P_LBRACE)) d->body = parse_block(p);
    else err_here(p, "expected '{' to open the CC_GENERIC_FACTORY body, found %s", tok_desc(p, p->i));
    d->span = span_from(p, first);
    return d;
}

static void parse_decl_or_stmt_into(P *p, CcDeclList *out);

static void parse_decl_list_until_brace(P *p, CcDeclList *out) {
    while (!at_p(p, CC_P_RBRACE) && !at_eof(p)) {
        uint32_t start = p->i;
        parse_decl_or_stmt_into(p, out);
        if (p->i == start) sync(p, start);
    }
}

static int parse_declaration_specs(P *p, CcDeclList *out, uint32_t first, CcAttr *attrs, int file_scope, uint32_t extra_flags);

static void parse_comptime_decl(P *p, CcDeclList *out) {
    uint32_t first = p->i;
    CcDecl *d;
    adv(p); /* @comptime */
    if (at_kw(p, KW_IF)) {
        uint32_t open, bopen;
        d = cc_decl_new(p->a, CC_D_COMPTIME_IF, span2(first, first));
        adv(p);
        open = p->i;
        if (expect_p(p, CC_P_LPAREN, "@comptime if")) {
            d->cond = parse_expr(p);
            expect_close(p, CC_P_RPAREN, "@comptime if", open);
        }
        bopen = p->i;
        if (expect_p(p, CC_P_LBRACE, "@comptime if body")) {
            parse_decl_list_until_brace(p, &d->then_decls);
            expect_close(p, CC_P_RBRACE, "@comptime if body", bopen);
        }
        if (accept_kw(p, KW_ELSE)) {
            if (at_word(p, "comptime") && is_kw(p, p->i + 1, KW_IF)) {
                CcDeclList tmp;
                memset(&tmp, 0, sizeof tmp);
                parse_comptime_decl(p, &tmp);
                if (tmp.n) CC_LIST_PUSH(p->a, &d->else_decls, tmp.items[0]);
            } else {
                bopen = p->i;
                if (expect_p(p, CC_P_LBRACE, "@comptime else body")) {
                    parse_decl_list_until_brace(p, &d->else_decls);
                    expect_close(p, CC_P_RBRACE, "@comptime else body", bopen);
                }
            }
        }
        d->span = span_from(p, first);
        CC_LIST_PUSH(p->a, out, d);
        return;
    }
    if (at_p(p, CC_P_LBRACE)) {
        d = cc_decl_new(p->a, CC_D_COMPTIME_BLOCK, span2(first, first));
        d->body = parse_block(p);
        d->span = span_from(p, first);
        CC_LIST_PUSH(p->a, out, d);
        return;
    }
    if (at_kw(p, KW_FOR)) {
        p->i = first;
        d = cc_decl_new(p->a, CC_D_COMPTIME_BLOCK, span2(first, first));
        d->body = parse_at_stmt(p);
        d->span = span_from(p, first);
        CC_LIST_PUSH(p->a, out, d);
        return;
    }
    if (is_type_start(p, p->i) || (at_ident(p) && kw_of(p, p->i) == KW_NONE && unknown_ident_is_type(p, p->i))) {
        parse_declaration_specs(p, out, first, NULL, 1, CC_F_COMPTIME);
        return;
    }
    /* `@comptime expr;` : a comptime statement at file scope */
    d = cc_decl_new(p->a, CC_D_COMPTIME_BLOCK, span2(first, first));
    {
        CcStmt *st = parse_stmt(p);
        d->body = st;
    }
    d->span = span_from(p, first);
    CC_LIST_PUSH(p->a, out, d);
}

static CcDecl *parse_scoped(P *p) {
    uint32_t first = p->i;
    CcDecl *d = cc_decl_new(p->a, CC_D_SCOPED_TYPE, span2(first, first));
    adv(p);
    if (!accept_kw(p, KW_TYPE)) err_here(p, "expected 'type' after @scoped, found %s", tok_desc(p, p->i));
    if (at_ident(p)) {
        CcName nm = tok_name(p, p->i);
        uint32_t tf = p->i;
        d->name = nm;
        declare_type(p, nm);
        adv(p);
        if (at_p2(p, CC_P_COLONCOLON, CC_P_LBRACKET)) d->type = parse_generic_args(p, nm, tf);
        else d->type = type_named(p, nm, tf);
        d->type->kind = CC_T_SCOPED;
    } else err_here(p, "expected the type name after @scoped type, found %s", tok_desc(p, p->i));
    if (!expect_semi(p, "@scoped type")) sync(p, first);
    d->span = span_from(p, first);
    return d;
}

static CcDecl *parse_link(P *p) {
    uint32_t first = p->i, open;
    CcDecl *d = cc_decl_new(p->a, CC_D_LINK, span2(first, first));
    adv(p);
    open = p->i;
    if (expect_p(p, CC_P_LPAREN, "@link")) {
        if (p->t[p->i].kind == CC_TK_STRING) {
            const CcToken *t = &p->t[p->i];
            d->name = cc_intern(p->in, p->f->src + t->off + 1, t->len >= 2 ? t->len - 2 : 0);
            d->tok = p->i;
            adv(p);
        } else err_here(p, "expected a library name string in @link(\"lib\"), found %s", tok_desc(p, p->i));
        expect_close(p, CC_P_RPAREN, "@link", open);
    }
    accept_p(p, CC_P_SEMI);
    d->span = span_from(p, first);
    return d;
}

static CcDecl *parse_static_assert(P *p) {
    uint32_t first = p->i, open;
    CcDecl *d = cc_decl_new(p->a, CC_D_STATIC_ASSERT, span2(first, first));
    adv(p);
    open = p->i;
    if (expect_p(p, CC_P_LPAREN, "_Static_assert")) {
        d->assert_expr = parse_cond(p);
        if (accept_p(p, CC_P_COMMA)) {
            if (p->t[p->i].kind == CC_TK_STRING) { d->assert_msg_tok = p->i; while (p->t[p->i].kind == CC_TK_STRING) adv(p); }
            else err_here(p, "expected the message string in _Static_assert, found %s", tok_desc(p, p->i));
        }
        expect_close(p, CC_P_RPAREN, "_Static_assert", open);
    }
    if (!expect_semi(p, "_Static_assert")) sync(p, first);
    d->span = span_from(p, first);
    return d;
}

/* The function type a declarator names (the top of the chain), or NULL. */
static CcType *func_type_of(CcType *t) { return t && t->kind == CC_T_FUNC ? t : NULL; }

/* Declarators after the specifiers; one CcDecl per declarator. */
static int parse_declaration_specs(P *p, CcDeclList *out, uint32_t first, CcAttr *attrs, int file_scope, uint32_t extra_flags) {
    Specs S;
    int count = 0;
    (void)file_scope;
    parse_specs(p, &S, 0);
    S.flags |= extra_flags;
    attr_append(&attrs, S.attrs);
    if (!S.type) {
        if (!S.saw_any && at_ident(p) && kw_of(p, p->i) == KW_NONE &&
            (tok_is_p(tk(p, 1), CC_P_LPAREN) || tok_is_p(tk(p, 1), CC_P_SEMI))) {
            /* a macro invocation at declaration level */
            CcDecl *d = cc_decl_new(p->a, CC_D_MACRO_CALL, span2(first, first));
            CcExpr *e = cc_expr_new(p->a, CC_E_IDENT, span2(p->i, p->i));
            e->name = tok_name(p, p->i);
            e->tok = p->i;
            d->name = e->name;
            adv(p);
            if (at_p(p, CC_P_LPAREN)) {
                CcExpr *c = cc_expr_new(p->a, CC_E_CALL, e->span);
                c->a = e;
                parse_args(p, &c->args, "macro invocation");
                c->span = span_from(p, e->span.first);
                e = c;
            }
            d->expr = e;
            d->attrs = attrs;
            if (at_p(p, CC_P_LBRACE)) {
                err_here(p, "'{' after the macro invocation '%s(...)': only CC_GENERIC_FACTORY takes a body", d->name);
                skip_balanced(p);
            }
            accept_p(p, CC_P_SEMI);
            d->span = span_from(p, first);
            CC_LIST_PUSH(p->a, out, d);
            return 1;
        }
        if (S.saw_any) err_here(p, "expected a type in the declaration, found %s", tok_desc(p, p->i));
        else if (p->t[p->i].kind == CC_TK_AT_WORD) {
            const CcToken *t = &p->t[p->i];
            if (at_word(p, "serial") || at_word(p, "parallel") || at_word(p, "defer") || at_word(p, "errhandler") ||
                at_word(p, "stage") || at_word(p, "with_deadline") || at_word(p, "closing") || at_word(p, "switch") ||
                at_word(p, "for") || at_word(p, "err") || at_word(p, "cancel_defer"))
                err_here(p, "'%.*s' is a statement and cannot appear at file scope", (int)t->len, p->f->src + t->off);
            else
                err_here(p, "unknown @word '%.*s'", (int)t->len, p->f->src + t->off);
        }
        else err_here(p, "expected a declaration, found %s", tok_desc(p, p->i));
        sync(p, first);
        return 0;
    }
    if (at_p(p, CC_P_SEMI)) {
        CcDecl *d;
        if (S.type->kind == CC_T_STRUCT || S.type->kind == CC_T_ENUM || (S.flags & CC_S_TYPEDEF) == 0) {
            d = cc_decl_new(p->a, CC_D_TAGGED, span2(first, first));
            if (S.type->kind == CC_T_NAMED && !S.type->is_struct_kw) {
                d->kind = CC_D_MACRO_CALL;
                d->name = S.type->name;
                d->expr = cc_expr_new(p->a, CC_E_IDENT, S.type->span);
                d->expr->name = S.type->name;
                d->expr->tok = S.type->span.first;
            }
        } else d = cc_decl_new(p->a, CC_D_TYPEDEF, span2(first, first));
        d->type = S.type;
        d->specs = S.flags;
        d->attrs = attrs;
        adv(p);
        d->span = span_from(p, first);
        CC_LIST_PUSH(p->a, out, d);
        return 1;
    }
    for (;;) {
        uint32_t dfirst = p->i, ntok = 0;
        CcDecl *d = cc_decl_new(p->a, CC_D_VAR, span2(dfirst, dfirst));
        CcAttr *dattrs = NULL;
        d->specs = S.flags;
        d->type = parse_declarator(p, S.type, 0, &d->name, &ntok, &dattrs);
        parse_attrs(p, &dattrs);
        d->attrs = count == 0 ? attrs : NULL;
        attr_append(&d->attrs, dattrs);
        if (!d->name) {
            sync(p, dfirst);
            d->span = span_from(p, count == 0 ? first : dfirst);
            CC_LIST_PUSH(p->a, out, d);
            return count + 1;
        }
        if (S.flags & CC_S_TYPEDEF) {
            d->kind = CC_D_TYPEDEF;
            if (!(func_type_of(d->type) && name_is_all_caps(d->name) && tok_is_p(&p->t[ntok + 1], CC_P_LPAREN)))
                declare_type(p, d->name);
        } else if (func_type_of(d->type) && name_is_all_caps(d->name) && tok_is_p(&p->t[ntok + 1], CC_P_LPAREN) &&
                   (at_p(p, CC_P_ASSIGN) || at_p(p, CC_P_SEMI))) {
            /* `const double CC_CAT_3(a, b, c) = v;`: the name is a macro invocation; the macro's
             * arguments were parsed as its parameter list */
            d->kind = CC_D_VAR;
            if (at_p(p, CC_P_ASSIGN)) { adv(p); d->init = parse_initializer(p); }
        } else if (func_type_of(d->type)) {
            d->kind = CC_D_FUNC;
            if (S.flags & CC_F_COMPTIME) d->kind = CC_D_COMPTIME_FN;
            if (!(name_is_all_caps(d->name) && tok_is_p(&p->t[ntok + 1], CC_P_LPAREN))) declare_var(p, d->name);
            if (at_p(p, CC_P_LBRACE)) {
                CcType *ft = func_type_of(d->type);
                size_t k;
                scope_push(p->syms);
                for (k = 0; k < ft->params.n; k++)
                    if (ft->params.items[k]->name) declare_var(p, ft->params.items[k]->name);
                d->body = parse_block(p);
                scope_pop(p->syms);
                d->span = span_from(p, count == 0 ? first : dfirst);
                CC_LIST_PUSH(p->a, out, d);
                return count + 1;
            }
        } else {
            declare_var(p, d->name);
            if (p->t[p->i].kind == CC_TK_AT && tok_is_p(tk(p, 1), CC_P_LPAREN)) {
                /* `T name@(args)` */
                CcExpr *c = cc_expr_new(p->a, CC_E_CREATE, span2(p->i, p->i));
                CcInit *in = CC_NEW(p->a, CcInit);
                adv(p);
                c->create_var = d->name;
                parse_args(p, &c->args, "name@(args) constructor");
                c->span = span_from(p, c->span.first);
                in->expr = parse_postfix(p, c);
                in->span = in->expr->span;
                d->init = in;
                if (in->expr->kind == CC_E_UNWRAP_DESTROY) { d->destroy = 1; d->destroy_body = in->expr->body; }
            } else if (at_legacy_bang(p)) {
                CcInit *in = CC_NEW(p->a, CcInit);
                uint32_t ifirst = p->i;
                in->expr = parse_legacy_bang_rhs(p);
                in->span = span_from(p, ifirst);
                d->init = in;
            } else if (at_p(p, CC_P_ASSIGN)) {
                adv(p);
                if (at_word(p, "parallel")) {
                    CcStmt *ps = parse_parallel(p);
                    ps->name = d->name;
                    ps->type = d->type;
                    ps->span.first = first;
                    p->bind_stmt = ps;
                    d->span = span_from(p, first);
                    CC_LIST_PUSH(p->a, out, d);
                    return count + 1;
                }
                d->init = parse_initializer(p);
                if (d->init->expr && d->init->expr->kind == CC_E_UNWRAP_DESTROY) {
                    d->destroy = 1;
                    d->destroy_body = d->init->expr->body;
                }
            }
        }
        /* lifetime sugar */
        for (;;) {
            if (at_word(p, "destroy")) {
                if (d->destroy && !d->destroy_body) err_here(p, "'@destroy' given twice on '%s'", d->name);
                d->destroy = 1;
                adv(p);
                if (at_p(p, CC_P_LBRACE)) d->destroy_body = parse_block(p);
                continue;
            }
            if (at_word(p, "detach")) { d->detach = 1; adv(p); continue; }
            break;
        }
        if (d->kind == CC_D_VAR && d->type->kind == CC_T_AUTO && !d->init)
            err_at(p, dfirst, "@auto(src) %s needs '(arena)' after the name", d->name);
        if (accept_p(p, CC_P_COMMA)) {
            d->span = span2(count == 0 ? first : dfirst, last_tok(p));
            CC_LIST_PUSH(p->a, out, d);
            count++;
            continue;
        }
        if (!expect_semi(p, d->kind == CC_D_FUNC ? "the function declaration" : d->kind == CC_D_TYPEDEF ? "the typedef" : "the declaration"))
            sync(p, dfirst);
        d->span = span2(count == 0 ? first : dfirst, last_tok(p));
        CC_LIST_PUSH(p->a, out, d);
        return count + 1;
    }
}

/* One declaration (possibly several CcDecls for `int a, b;`). */
static int parse_declaration(P *p, CcDeclList *out, int file_scope) {
    uint32_t first = p->i;
    const CcToken *t = &p->t[p->i];
    CcAttr *attrs = NULL;
    (void)file_scope;
    if (t->kind == CC_TK_PP) { CC_LIST_PUSH(p->a, out, pp_decl(p)); return 1; }
    if (t->kind == CC_TK_ERROR) { adv(p); return 0; }
    if (tok_is_p(t, CC_P_SEMI)) {
        CcDecl *d = cc_decl_new(p->a, CC_D_EMPTY, span2(first, first));
        adv(p);
        CC_LIST_PUSH(p->a, out, d);
        return 1;
    }
    collect_trivia_attrs(p, first, &attrs);
    if (t->kind == CC_TK_AT_WORD) {
        if (at_word(p, "typehooks")) { CC_LIST_PUSH(p->a, out, parse_typehooks(p)); return 1; }
        if (at_word(p, "typeview") && !tok_is_p(tk(p, 1), CC_P_LPAREN)) { parse_typeview(p, out, first, 0); return 1; }
        if (at_word(p, "variant")) { CC_LIST_PUSH(p->a, out, parse_variant(p)); return 1; }
        if (at_word(p, "grammar")) { CC_LIST_PUSH(p->a, out, parse_grammar(p)); return 1; }
        if (at_word(p, "scoped")) { CC_LIST_PUSH(p->a, out, parse_scoped(p)); return 1; }
        if (at_word(p, "link")) { CC_LIST_PUSH(p->a, out, parse_link(p)); return 1; }
        if (at_word(p, "comptime") && !tok_is_p(tk(p, 1), CC_P_LPAREN)) { parse_comptime_decl(p, out); return 1; }
    }
    if (at_kw(p, KW_STATIC_ASSERT)) { CC_LIST_PUSH(p->a, out, parse_static_assert(p)); return 1; }
    if (at_kw(p, KW_TYPEDEF) && tok_is_word(p, p->i + 1, "typeview") && !tok_is_p(tk(p, 2), CC_P_LPAREN)) {
        adv(p);
        parse_typeview(p, out, first, 1);
        return 1;
    }
    if ((at_kw(p, KW_CC_GENERIC_FACTORY) || at_kw(p, KW_CC_GENERIC_FACTORY_EXTEND)) && tok_is_p(tk(p, 1), CC_P_LPAREN)) {
        CC_LIST_PUSH(p->a, out, parse_factory(p));
        return 1;
    }
    return parse_declaration_specs(p, out, first, attrs, file_scope, 0);
}

/* ---- the unit -------------------------------------------------------- */

/* Is the item here a statement rather than a declaration, at a level where
 * both are allowed (scripts, @comptime if bodies at file scope)? A macro
 * invocation on its own line (`CC_DECL_X(...)`) stays a declaration. */
static int top_is_stmt(P *p) {
    if (at_cc_decl_word(p) || stmt_is_decl(p) || at_p(p, CC_P_SEMI)) return 0;
    if (at_kw(p, KW_STATIC_ASSERT) || at_kw(p, KW_CC_GENERIC_FACTORY) || at_kw(p, KW_CC_GENERIC_FACTORY_EXTEND)) return 0;
    if (at_kw(p, KW_TYPEDEF)) return 0;
    if (at_ident(p) && kw_of(p, p->i) == KW_NONE && tok_is_p(tk(p, 1), CC_P_LPAREN) && !for_in_ahead(p, p->i + 1)) {
        uint32_t close = match_close(p, p->i + 1);
        const CcToken *after = &p->t[close + 1];
        /* `MACRO(args)` alone on its line, followed by a new line: a declaration-level macro */
        if (after->at_line_start && !name_is_var(p, tok_name(p, p->i))) return 0;
    }
    return 1;
}

static void parse_decl_or_stmt_into(P *p, CcDeclList *out) {
    uint32_t start = p->i;
    if (p->t[p->i].kind == CC_TK_PP) { CC_LIST_PUSH(p->a, out, pp_decl(p)); return; }
    if (top_is_stmt(p)) {
        CcDecl *d = cc_decl_new(p->a, CC_D_STMT, span2(start, start));
        d->body = parse_stmt(p);
        if (p->bind_stmt) { d->body = p->bind_stmt; p->bind_stmt = NULL; }
        d->span = span_from(p, start);
        CC_LIST_PUSH(p->a, out, d);
    } else {
        p->bind_stmt = NULL;
        parse_declaration(p, out, 1);
        if (p->bind_stmt) {
            CcDecl *d = cc_decl_new(p->a, CC_D_STMT, p->bind_stmt->span);
            d->body = p->bind_stmt;
            p->bind_stmt = NULL;
            CC_LIST_PUSH(p->a, out, d);
        }
    }
}

static void parse_unit(P *p) {
    int script = p->opts.mode == CC_MODE_SCRIPT || p->opts.allow_top_level_stmts;
    while (!at_eof(p)) {
        uint32_t start = p->i;
        if (p->t[p->i].kind == CC_TK_PP) {
            CC_LIST_PUSH(p->a, &p->u->decls, pp_decl(p));
            continue;
        }
        if (at_p(p, CC_P_RBRACE)) {
            err_here(p, "unexpected '}' at file scope");
            adv(p);
            continue;
        }
        if (script) parse_decl_or_stmt_into(p, &p->u->decls);
        else {
            p->bind_stmt = NULL;
            parse_declaration(p, &p->u->decls, 1);
            if (p->bind_stmt) {
                CcDecl *d = cc_decl_new(p->a, CC_D_STMT, p->bind_stmt->span);
                d->body = p->bind_stmt;
                p->bind_stmt = NULL;
                CC_LIST_PUSH(p->a, &p->u->decls, d);
            }
        }
        if (p->i == start) {
            if (at_p(p, CC_P_RBRACE) || at_p(p, CC_P_SEMI)) adv(p); else sync(p, start);
        }
    }
}

CcUnit *cc_parse(CcArena *a, CcDiag *d, CcIntern *in, CcLexFile *f, const CcParseOpts *opts) {
    P p;
    int k;
    memset(&p, 0, sizeof p);
    p.a = a; p.d = d; p.in = in; p.f = f;
    if (opts) p.opts = *opts;
    p.t = f->toks;
    p.n = f->n_toks;
    p.i = 0;
    p.u = CC_NEW(a, CcUnit);
    p.u->arena = a;
    p.u->file = f;
    p.syms = syms_new(a);
    for (k = 1; k < KW_COUNT; k++) p.kw[k] = cc_intern(in, kw_text[k], strlen(kw_text[k]));
    p.n_typeview = cc_intern(in, "typeview", 8);
    p.n_tag = cc_intern(in, "tag", 3);
    p.n_task = cc_intern(in, "task", 4);
    p.n_packed = cc_intern(in, "packed", 6);
    p.n_attribute = cc_intern(in, "__attribute__", 13);
    p.n_alignas = cc_intern(in, "_Alignas", 8);
    p.n_declspec = cc_intern(in, "__declspec", 10);
    p.n_asm = cc_intern(in, "asm", 3);
    p.n_parallel_join = cc_intern(in, "@parallel", 9);
    p.n_typeof = cc_intern(in, "typeof", 6);
    p.consumed_terminator = UINT32_MAX;
    if (p.opts.known_types) {
        const char **kt;
        for (kt = p.opts.known_types; *kt; kt++) sym_declare(p.syms, cc_intern(in, *kt, strlen(*kt)), 1);
    }
    parse_unit(&p);
    return p.u;
}
