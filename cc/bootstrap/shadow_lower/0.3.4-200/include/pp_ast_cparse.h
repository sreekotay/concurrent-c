/* FileTape → cparse overlay. C-only field lists go through libcparse.
 * File-scope functions stay on the beachhead: emit reprints FileTape, which
 * drops overlay meaning (defaults, @typeview, string switch, safety). */
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

