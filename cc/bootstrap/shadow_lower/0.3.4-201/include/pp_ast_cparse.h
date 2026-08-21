/* FileTape → cparse overlay. Struct member lists go through libcparse;
 * Concurrent-C fields are re-parsed by the overlay (not flatten-as-C).
 * File-scope functions stay on the beachhead: emit reprints FileTape, which
 * drops overlay meaning (defaults, @typeview, string switch, safety). */
#pragma once

enum { CP_FLAT_FIELD = 1, CP_FLAT_PPDIR = 2, CP_FLAT_FUNC = 3 };
enum { SHADOW_CP_FLAT_CAP = 256 };

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

/* Defined later in this TU (parse_stmt / parse_ext). */
static AstNode* parse_slice_var(Parser* p);
static AstNode* parse_chan_var(Parser* p);
static AstNode* parse_field_result(Parser* p);
static AstNode* parse_field_simple(Parser* p);
static AstNode* parse_int_decl(Parser* p, AstKind kind);

/* 0 = Concurrent-C / UFCS / generics — overlay must rewrite, not emit as C. */
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

static void shadow_trim_ppdir(char* s) {
    size_t n;
    if (!s) return;
    n = strlen(s);
    while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' ' ||
                     s[n - 1] == '\t')) {
        s[--n] = 0;
    }
}

/* Tokens overlapping FileTape [start, end) inside [list_lo, list_hi). */
static int shadow_toks_covering(Parser* p, int list_lo, int list_hi, int start,
                                int end, int* out_lo, int* out_hi) {
    int i;
    int a = -1;
    int b = -1;
    if (!out_lo || !out_hi || start < 0 || end < start) return 0;
    for (i = list_lo; i < list_hi && i < p->n; i++) {
        size_t toff = p->toks[i].offset;
        size_t tend = toff + p->toks[i].spell.len;
        if (tend > (size_t)start && toff < (size_t)end) {
            if (a < 0) a = i;
            b = i + 1;
        }
    }
    if (a < 0) return 0;
    *out_lo = a;
    *out_hi = b;
    return 1;
}

static int shadow_attach_c_flat_field(Parser* p, AstNode* st, const CpFlat* f,
                                      int list_lo) {
    AstNode* n;
    FileTape* ft;
    size_t nlen;
    size_t tlen;
    ft = tape_by_id(p->cache, p->toks[list_lo < p->n ? list_lo : 0].file_id);
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
    if (ft && ft->bytes && f->end > f->start && (size_t)f->end <= ft->len) {
        n->file_id = p->toks[list_lo < p->n ? list_lo : 0].file_id;
        n->span_off = (size_t)f->start;
        n->span_len = (size_t)(f->end - f->start);
        while (n->span_len > 0 &&
               ft->bytes[n->span_off + n->span_len - 1] == ';')
            n->span_len--;
    }
    if (!ast_kids_push(p, n)) return 0;
    st->nkids++;
    return 1;
}

/* Overlay meaning: rebind to the FileTape token span and run beachhead
 * field parsers. Do not attach flatten text (would reprint !> / [:]). */
static int shadow_attach_cc_field(Parser* p, AstNode* st, int flo, int fhi) {
    int saved = p->i;
    AstNode* n;
    p->i = flo;
    n = parse_slice_var(p);
    if (!n && !p->err) n = parse_chan_var(p);
    if (!n && !p->err) n = parse_field_result(p);
    if (!n && !p->err) n = parse_int_decl(p, AST_FIELD_INT);
    if (!n && !p->err) n = parse_field_simple(p);
    if (p->err) {
        p->i = saved;
        return 0;
    }
    if (!n) {
        p->i = saved;
        parser_fail(p, p->toks[flo], "cparse CC field: overlay parse failed");
        return 0;
    }
    /* Consumed through ';' — must not leave tokens in the field unread. */
    if (p->i < fhi) {
        p->i = saved;
        parser_fail(p, p->toks[flo], "cparse CC field: incomplete overlay parse");
        return 0;
    }
    p->i = saved;
    if (!ast_kids_push(p, n)) return 0;
    st->nkids++;
    return 1;
}

static int shadow_attach_cparse_field(Parser* p, AstNode* st, const CpFlat* f,
                                      int list_lo, int list_hi) {
    AstNode* n;
    if (f->kind == CP_FLAT_PPDIR) {
        n = ast_new(p, AST_RAW_LINE);
        if (!n) return 0;
        snprintf(n->a, sizeof(n->a), "%s", f->text);
        shadow_trim_ppdir(n->a);
        snprintf(n->e, sizeof(n->e), "ppdir");
        if (!ast_kids_push(p, n)) return 0;
        st->nkids++;
        return 1;
    }
    if (f->kind != CP_FLAT_FIELD) return 0;
    {
        int flo = 0;
        int fhi = 0;
        if (shadow_toks_covering(p, list_lo, list_hi, f->start, f->end, &flo,
                                 &fhi) &&
            !shadow_toks_are_c(p, flo, fhi))
            return shadow_attach_cc_field(p, st, flo, fhi);
    }
    return shadow_attach_c_flat_field(p, st, f, list_lo);
}

/* Member list: cparse owns the shape. No beachhead fallback. */
static int parse_struct_fields_via_cparse(Parser* p, AstNode* st, int close) {
    CpTok* cpt = NULL;
    CpFlat* flat = NULL;
    char err[192];
    FileTape* ft;
    int list_lo = p->i;
    int nneed;
    int ntok;
    int flat_cap;
    int nflat = 0;
    int k;
    int ok = 0;
    ft = tape_by_id(p->cache, p->toks[list_lo < p->n ? list_lo : 0].file_id);
    if (!ft || !ft->bytes) {
        parser_fail(p, p_peek(p), "cparse fields: missing FileTape");
        return 0;
    }
    if (close < list_lo) {
        parser_fail(p, p_peek(p), "cparse fields: unclosed struct");
        return 0;
    }
    nneed = close - list_lo;
    cpt = (CpTok*)malloc((size_t)nneed * sizeof(CpTok));
    if (nneed > 0 && !cpt) {
        parser_fail(p, p_peek(p), "cparse fields: oom");
        return 0;
    }
    ntok = shadow_fill_cptoks(p, list_lo, close, cpt, nneed > 0 ? nneed : 0);
    if (ntok < 0) {
        parser_fail(p, p_peek(p), "cparse fields: token fill failed");
        free(cpt);
        return 0;
    }
    flat_cap = ntok + 8;
    if (flat_cap < SHADOW_CP_FLAT_CAP) flat_cap = SHADOW_CP_FLAT_CAP;
    flat = (CpFlat*)malloc((size_t)flat_cap * sizeof(CpFlat));
    if (!flat) {
        parser_fail(p, p_peek(p), "cparse fields: oom");
        free(cpt);
        return 0;
    }
    err[0] = 0;
    if (cparse_flat_fields(ft->bytes, (int)ft->len, cpt, ntok, flat, flat_cap,
                           &nflat, err, (int)sizeof(err)) != 0) {
        parser_fail(p, p_peek(p), err[0] ? err : "cparse fields failed");
        free(flat);
        free(cpt);
        return 0;
    }
    for (k = 0; k < nflat; k++) {
        if (!shadow_attach_cparse_field(p, st, &flat[k], list_lo, close)) {
            if (!p->err)
                parser_fail(p, p_peek(p), "cparse field attach failed");
            free(flat);
            free(cpt);
            return 0;
        }
    }
    p->i = close;
    ok = 1;
    free(flat);
    free(cpt);
    return ok;
}
