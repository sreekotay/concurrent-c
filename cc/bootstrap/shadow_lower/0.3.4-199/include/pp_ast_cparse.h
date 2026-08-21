/* FileTape → cparse overlay. C-only field lists and C-only file-scope
 * functions go through libcparse; Concurrent-C / UFCS bodies stay on
 * the beachhead. */
#pragma once

enum { CP_FLAT_FIELD = 1, CP_FLAT_PPDIR = 2, CP_FLAT_FUNC = 3 };
enum { SHADOW_CP_TOK_CAP = 4096, SHADOW_CP_FLAT_CAP = 256 };

typedef struct {
    int kind;
    const char* ptr;
    size_t len;
    int file_id;
    size_t offset;
} CpTok;

typedef struct {
    int kind;
    char name[128];
    char text[512];
    int start;
    int end;
} CpFlat;

extern int cparse_flat_fields(const char* src, int len, const CpTok* toks,
                              int ntoks, CpFlat* out, int cap, int* n,
                              char* err, int errcap);
extern int cparse_match_func(const char* src, int len, const CpTok* toks,
                             int ntoks, char* name, int ncap, int* lbrace_i,
                             char* err, int errcap);

static int shadow_toks_are_c(Parser* p, int lo, int hi) {
    int i;
    for (i = lo; i < hi && i < p->n; i++) {
        Token t = p->toks[i];
        if (t.kind == TK_IDENT && i + 1 < hi &&
            tok_eq(p->toks[i + 1], TK_PUNCT, "<") &&
            t.spell.len > 0 && t.spell.ptr[0] >= 'A' && t.spell.ptr[0] <= 'Z')
            return 0; /* Vec<T> / Map<K,V> */
        if (t.kind != TK_PUNCT) continue;
        if (tok_eq(t, TK_PUNCT, "=>") || tok_eq(t, TK_PUNCT, "!>") ||
            tok_eq(t, TK_PUNCT, "?>") || tok_eq(t, TK_PUNCT, "@") ||
            tok_eq(t, TK_PUNCT, "::") || tok_eq(t, TK_PUNCT, "[~"))
            return 0;
        if (tok_eq(t, TK_PUNCT, "[") && i + 1 < hi &&
            (tok_eq(p->toks[i + 1], TK_PUNCT, ":") ||
             tok_eq(p->toks[i + 1], TK_PUNCT, "~")))
            return 0;
        /* `== .arm` / `case .arm` — variant, not C. */
        if (tok_eq(t, TK_PUNCT, ".") && i + 1 < hi &&
            p->toks[i + 1].kind == TK_IDENT) {
            if (i > lo && (tok_eq(p->toks[i - 1], TK_PUNCT, "==") ||
                           tok_eq(p->toks[i - 1], TK_PUNCT, "!=") ||
                           (p->toks[i - 1].kind == TK_IDENT &&
                            spell_eq(p->toks[i - 1].spell, "case"))))
                return 0;
        }
        /* `recv = { .arm = … }` — variant assign; C needs `(T){`. */
        if (tok_eq(t, TK_PUNCT, "=") && i + 1 < hi &&
            tok_eq(p->toks[i + 1], TK_PUNCT, "{"))
            return 0;
    }
    return 1;
}

static int shadow_fill_cptoks(Parser* p, int lo, int hi, CpTok* out, int cap) {
    int n = hi - lo;
    int i;
    if (n < 0 || n > cap) return -1;
    for (i = 0; i < n; i++) {
        Token t = p->toks[lo + i];
        out[i].kind = (int)t.kind;
        out[i].ptr = t.spell.ptr;
        out[i].len = t.spell.len;
        out[i].file_id = t.file_id;
        out[i].offset = t.offset;
    }
    return n;
}

static int shadow_match_pair(Parser* p, int open, const char* op,
                             const char* cl) {
    int depth = 1;
    int i;
    if (open < 0 || open >= p->n || !tok_eq(p->toks[open], TK_PUNCT, op))
        return -1;
    for (i = open + 1; i < p->n; i++) {
        if (tok_eq(p->toks[i], TK_PUNCT, op)) depth++;
        else if (tok_eq(p->toks[i], TK_PUNCT, cl)) {
            depth--;
            if (depth == 0) return i;
        }
    }
    return -1;
}

static void shadow_trim_ppdir(char* s) {
    size_t n;
    if (!s) return;
    n = strlen(s);
    while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' ' ||
                     s[n - 1] == '\t')) {
        s[--n] = 0;
    }
}

static int shadow_attach_cparse_field(Parser* p, AstNode* st, const CpFlat* f) {
    AstNode* n;
    FileTape* ft = tape_by_id(p->cache, p->toks[p->i].file_id);
    if (f->kind == CP_FLAT_PPDIR) {
        n = ast_new(p, AST_RAW_LINE);
        if (!n) return 0;
        snprintf(n->a, sizeof(n->a), "%s", f->text);
        shadow_trim_ppdir(n->a);
        snprintf(n->e, sizeof(n->e), "ppdir");
    } else if (f->kind == CP_FLAT_FIELD) {
        size_t nlen;
        size_t tlen;
        n = ast_new(p, AST_FIELD_SIMPLE);
        if (!n) return 0;
        snprintf(n->a, sizeof(n->a), "%s", f->text);
        snprintf(n->b, sizeof(n->b), "%s", f->name);
        /* Emit wants a=type, b=name. Arrays / fn-ptrs do not end in the
         * name (`size_t bstack[512]`). Nested `{` and flatten-overflow
         * (empty text) reprint FileTape — do not strip a type that emit
         * would then copy into 160 bytes. */
        nlen = strlen(f->name);
        tlen = strlen(n->a);
        if (nlen > 0 && tlen >= nlen && strchr(f->text, '{') == NULL &&
            strcmp(n->a + tlen - nlen, f->name) == 0) {
            n->a[tlen - nlen] = 0;
            tlen = strlen(n->a);
            while (tlen > 0 &&
                   (n->a[tlen - 1] == ' ' || n->a[tlen - 1] == '\t'))
                n->a[--tlen] = 0;
        } else {
            snprintf(n->e, sizeof(n->e), "raw");
        }
        if (ft && ft->bytes && f->end > f->start &&
            (size_t)f->end <= ft->len) {
            n->file_id = p->toks[p->i].file_id;
            n->span_off = (size_t)f->start;
            n->span_len = (size_t)(f->end - f->start);
            while (n->span_len > 0 &&
                   ft->bytes[n->span_off + n->span_len - 1] == ';')
                n->span_len--;
        }
    } else {
        return 0;
    }
    if (!ast_kids_push(p, n)) return 0;
    st->nkids++;
    return 1;
}

/* C-only member list: cparse owns it. No beachhead fallback. */
static int parse_struct_fields_via_cparse(Parser* p, AstNode* st, int close) {
    CpTok cpt[SHADOW_CP_TOK_CAP];
    CpFlat flat[SHADOW_CP_FLAT_CAP];
    char err[192];
    FileTape* ft;
    int ntok;
    int nflat = 0;
    int k;
    ft = tape_by_id(p->cache, p->toks[p->i < p->n ? p->i : 0].file_id);
    if (!ft || !ft->bytes) {
        parser_fail(p, p_peek(p), "cparse fields: missing FileTape");
        return 0;
    }
    ntok = shadow_fill_cptoks(p, p->i, close, cpt, SHADOW_CP_TOK_CAP);
    if (ntok < 0) {
        parser_fail(p, p_peek(p), "cparse fields: token cap");
        return 0;
    }
    err[0] = 0;
    if (cparse_flat_fields(ft->bytes, (int)ft->len, cpt, ntok, flat,
                           SHADOW_CP_FLAT_CAP, &nflat, err,
                           (int)sizeof(err)) != 0) {
        parser_fail(p, p_peek(p), err[0] ? err : "cparse fields failed");
        return 0;
    }
    for (k = 0; k < nflat; k++) {
        if (!shadow_attach_cparse_field(p, st, &flat[k])) {
            parser_fail(p, p_peek(p), "cparse field attach failed");
            return 0;
        }
    }
    p->i = close;
    return 1;
}

static int shadow_find_punct(Parser* p, int from, const char* lit) {
    int i;
    for (i = from; i < p->n; i++) {
        if (tok_eq(p->toks[i], TK_PUNCT, lit)) return i;
    }
    return -1;
}

static int shadow_match_brace(Parser* p, int open) {
    return shadow_match_pair(p, open, "{", "}");
}

/* `recv.meth(` / `recv->meth(` — overlay UFCS, not C. */
static int shadow_toks_have_ufcs_call(Parser* p, int lo, int hi) {
    int i;
    for (i = lo; i + 2 < hi && i + 2 < p->n; i++) {
        if (!tok_eq(p->toks[i], TK_PUNCT, ".") &&
            !tok_eq(p->toks[i], TK_PUNCT, "->"))
            continue;
        if (p->toks[i + 1].kind != TK_IDENT) continue;
        if (tok_eq(p->toks[i + 2], TK_PUNCT, "(")) return 1;
    }
    return 0;
}

/* File-scope `T name(...) {` — not `T x[] = {` / `T x = {`. */
static int shadow_looks_like_fn_def(Parser* p) {
    int i;
    int depth = 0;
    int saw_paren = 0;
    for (i = p->i; i < p->n; i++) {
        Token t = p->toks[i];
        if (tok_eq(t, TK_PUNCT, "{")) return saw_paren && depth == 0;
        if (depth == 0 && tok_eq(t, TK_PUNCT, ";")) return 0;
        if (tok_eq(t, TK_PUNCT, "(") || tok_eq(t, TK_PUNCT, "[") ||
            tok_eq(t, TK_PUNCT, "{")) {
            if (tok_eq(t, TK_PUNCT, "(") && depth == 0) saw_paren = 1;
            if (!tok_eq(t, TK_PUNCT, "{")) depth++;
        } else if (tok_eq(t, TK_PUNCT, ")") || tok_eq(t, TK_PUNCT, "]")) {
            if (depth > 0) depth--;
        }
    }
    return 0;
}

/* C-only file-scope function (sig + body). 1 = matched, 0 = not this
 * shape (CC / UFCS / not a fn def), -1 = cparse failed loud.
 * cparse owns the stmt list; overlay tapes the span for emit. */
static int shadow_cparse_file_fn(Parser* p, char* name, int ncap,
                                 int* lbrace_abs, int* rbrace_abs) {
    CpTok cpt[SHADOW_CP_TOK_CAP];
    char err[192];
    FileTape* ft;
    int lb;
    int rb;
    int ntok;
    int rel = -1;
    size_t blen;
    if (!shadow_looks_like_fn_def(p)) return 0;
    lb = shadow_find_punct(p, p->i, "{");
    if (lb < 0) return 0;
    rb = shadow_match_brace(p, lb);
    if (rb < 0) {
        parser_fail(p, p->toks[lb], "unclosed function");
        return -1;
    }
    if (!shadow_toks_are_c(p, p->i, rb + 1)) return 0;
    if (shadow_toks_have_ufcs_call(p, p->i, rb + 1)) return 0;
    blen = p->toks[rb].offset + p->toks[rb].spell.len - p->toks[lb].offset;
    if (blen >= 4096) return 0;
    ntok = shadow_fill_cptoks(p, p->i, rb + 1, cpt, SHADOW_CP_TOK_CAP);
    if (ntok < 0) {
        parser_fail(p, p_peek(p), "cparse fn: token cap");
        return -1;
    }
    ft = tape_by_id(p->cache, p->toks[p->i].file_id);
    if (!ft || !ft->bytes) {
        parser_fail(p, p_peek(p), "cparse fn: missing FileTape");
        return -1;
    }
    err[0] = 0;
    if (cparse_match_func(ft->bytes, (int)ft->len, cpt, ntok, name, ncap, &rel,
                          err, (int)sizeof(err)) != 0) {
        parser_fail(p, p_peek(p), err[0] ? err : "cparse fn failed");
        return -1;
    }
    *lbrace_abs = p->i + rel;
    *rbrace_abs = rb;
    return 1;
}

/* C-only file-scope fn: keep AST_STATIC_FN so Type_meth registers for
 * UFCS. Body stays a tape span — do not drop the method table. */
static AstNode* shadow_cparse_fn_tape(Parser* p, const char* name, int rb) {
    AstNode* n = ast_new(p, AST_STATIC_FN);
    Token t0 = p->toks[p->i];
    Token last = p->toks[rb];
    FileTape* ft;
    int i, name_i = -1, par = -1, rp = -1;
    size_t nlen;
    if (!n) return NULL;
    nlen = name ? strlen(name) : 0;
    snprintf(n->b, sizeof(n->b), "%s", name ? name : "");
    snprintf(n->e, sizeof(n->e), "cparse");
    n->file_id = t0.file_id;
    n->tok_off = t0.offset;
    n->span_off = t0.offset;
    n->span_len = (last.offset + last.spell.len > t0.offset)
                      ? (last.offset + last.spell.len - t0.offset)
                      : 0;
    for (i = p->i; i < rb && i < p->n; i++) {
        if (nlen && p->toks[i].kind == TK_IDENT &&
            p->toks[i].spell.len == nlen &&
            memcmp(p->toks[i].spell.ptr, name, nlen) == 0)
            name_i = i;
        if (name_i >= 0 && tok_eq(p->toks[i], TK_PUNCT, "(")) {
            par = i;
            break;
        }
    }
    ft = tape_by_id(p->cache, t0.file_id);
    if (par >= 0) {
        rp = shadow_match_pair(p, par, "(", ")");
        if (rp > par + 1 && ft && ft->bytes) {
            size_t o0 = p->toks[par + 1].offset;
            size_t o1 = p->toks[rp].offset;
            if (o1 > o0 && o1 - o0 < sizeof(n->c)) {
                memcpy(n->c, ft->bytes + o0, o1 - o0);
                n->c[o1 - o0] = 0;
                {
                    size_t cl = strlen(n->c);
                    while (cl > 0 &&
                           (n->c[cl - 1] == ' ' || n->c[cl - 1] == '\t' ||
                            n->c[cl - 1] == '\n'))
                        n->c[--cl] = 0;
                }
            }
        } else if (rp == par + 1) {
            snprintf(n->c, sizeof(n->c), "void");
        }
    }
    if (name_i > p->i && ft && ft->bytes) {
        int rs = p->i;
        while (rs < name_i) {
            if (shadow_kw(p->toks[rs]) == SHADOW_KW_STATIC ||
                shadow_kw(p->toks[rs]) == SHADOW_KW_INLINE)
                rs++;
            else
                break;
        }
        if (rs < name_i) {
            size_t o0 = p->toks[rs].offset;
            size_t o1 = p->toks[name_i].offset;
            if (o1 > o0 && o1 - o0 < sizeof(n->a)) {
                memcpy(n->a, ft->bytes + o0, o1 - o0);
                n->a[o1 - o0] = 0;
                {
                    size_t al = strlen(n->a);
                    while (al > 0 &&
                           (n->a[al - 1] == ' ' || n->a[al - 1] == '\t'))
                        n->a[--al] = 0;
                }
            }
        }
    }
    if (ft && ft->bytes && n->span_len > 0) {
        int lb = shadow_find_punct(p, p->i, "{");
        if (lb >= 0 && lb < rb) {
            size_t o0 = p->toks[lb].offset;
            size_t o1 = p->toks[rb].offset + p->toks[rb].spell.len;
            if (o1 > o0 && o1 - o0 < sizeof(n->d)) {
                memcpy(n->d, ft->bytes + o0, o1 - o0);
                n->d[o1 - o0] = 0;
            }
        }
    }
    if (p->pending_fn_attrs && name && name[0])
        shadow_fn_attr_register(name, p->pending_fn_attrs, 0);
    p->i = rb + 1;
    return n;
}
