/* FileTape → cparse overlay. Struct member lists go through libcparse;
 * Concurrent-C fields are re-parsed by the overlay (not flatten-as-C).
 * C-only file-scope functions: cparse confirms the envelope; overlay builds
 * AST params/body kids (never whole-fn FileTape reprint). */
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
extern int cparse_match_func(const char* src, int len, const CpTok* toks,
                             int ntoks, char* name, int ncap, int* lbrace_i,
                             char* err, int errcap);
extern int cparse_func_stmt_spans(const char* src, int len, const CpTok* toks,
                                  int ntoks, int* starts, int* ends, int cap,
                                  int* n, char* err, int errcap);

/* Defined later in this TU (parse_stmt / parse_ext). */
static AstNode* parse_stmt(Parser* p);
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

/* Meaning tokens — not type-position sugar (`[:]`, `!>(E)`). */
static int shadow_toks_need_overlay_field(Parser* p, int lo, int hi) {
    int i;
    for (i = lo; i < hi && i < p->n; i++) {
        Token t = p->toks[i];
        if (t.kind != TK_PUNCT) continue;
        if (tok_eq(t, TK_PUNCT, "=>") || tok_eq(t, TK_PUNCT, "@") ||
            tok_eq(t, TK_PUNCT, "::"))
            return 1;
    }
    return 0;
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

/* ` [N][M]…` after a field name — not a fn-ptr / bitfield. */
static int shadow_c_flat_array_suffix(const char* s) {
    if (!s) return 0;
    while (*s == ' ' || *s == '\t') s++;
    if (*s != '[') return 0;
    while (*s == '[') {
        int d = 1;
        s++;
        while (*s && d) {
            if (*s == '[') d++;
            else if (*s == ']') d--;
            s++;
        }
        if (d) return 0;
        while (*s == ' ' || *s == '\t') s++;
    }
    return *s == 0;
}

/* Last ident match of `name` in flatten text. 0 if none / ambiguous. */
static char* shadow_c_flat_name_at(char* text, const char* name) {
    char* at = NULL;
    char* p;
    size_t nlen;
    if (!text || !name || !name[0]) return NULL;
    nlen = strlen(name);
    p = text;
    while ((p = strstr(p, name)) != NULL) {
        int ok_b = (p == text) || p[-1] == ' ' || p[-1] == '\t' || p[-1] == '*';
        char nxt = p[nlen];
        int ok_a = !nxt || nxt == '[' || nxt == ' ' || nxt == '\t' || nxt == ',';
        if (ok_b && ok_a) at = p;
        p += nlen;
    }
    return at;
}

static int shadow_attach_c_flat_field(Parser* p, AstNode* st, const CpFlat* f,
                                      int list_lo) {
    AstNode* n;
    FileTape* ft;
    size_t nlen;
    size_t tlen;
    char* name_at;
    ft = tape_by_id(p->cache, p->toks[list_lo < p->n ? list_lo : 0].file_id);
    n = ast_new(p, AST_FIELD_SIMPLE);
    if (!n) return 0;
    snprintf(n->a, sizeof(n->a), "%s", f->text);
    snprintf(n->b, sizeof(n->b), "%s", f->name);
    /* Emit wants a=type, b=declarator. Name-at-end strips (`int x`).
     * Arrays (`int spec_a[6]`) leave `[6]` after the ident — without a
     * split, we marked raw and reprinted the declarator span, dropping
     * the type on comma kids (`spec_b[6];`). Fn-ptrs / nested `{` /
     * flatten-overflow stay raw. */
    nlen = strlen(f->name);
    tlen = strlen(n->a);
    name_at = (nlen > 0 && strchr(f->text, '{') == NULL)
                  ? shadow_c_flat_name_at(n->a, f->name)
                  : NULL;
    if (name_at && name_at[nlen] == 0) {
        n->a[name_at - n->a] = 0;
        tlen = strlen(n->a);
        while (tlen > 0 &&
               (n->a[tlen - 1] == ' ' || n->a[tlen - 1] == '\t'))
            n->a[--tlen] = 0;
    } else if (name_at && shadow_c_flat_array_suffix(name_at + nlen)) {
        char decl[160];
        snprintf(decl, sizeof(decl), "%s", name_at);
        n->a[name_at - n->a] = 0;
        tlen = strlen(n->a);
        while (tlen > 0 &&
               (n->a[tlen - 1] == ' ' || n->a[tlen - 1] == '\t'))
            n->a[--tlen] = 0;
        snprintf(n->b, sizeof(n->b), "%s", decl);
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
        return 0; /* miss — caller tapes C shape or fails on !> / @ */
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
        FileTape* ft;
        n = ast_new(p, AST_RAW_LINE);
        if (!n) return 0;
        ft = tape_by_id(p->cache, p->toks[list_lo < p->n ? list_lo : 0].file_id);
        if (f->text[0]) {
            snprintf(n->a, sizeof(n->a), "%s", f->text);
            shadow_trim_ppdir(n->a);
            snprintf(n->e, sizeof(n->e), "ppdir");
        } else if (ft && ft->bytes && f->end > f->start &&
                   (size_t)f->end <= ft->len) {
            /* Oversized directive: FileTape span, never chopped text. */
            snprintf(n->e, sizeof(n->e), "raw");
            n->file_id = p->toks[list_lo < p->n ? list_lo : 0].file_id;
            n->span_off = (size_t)f->start;
            n->span_len = (size_t)(f->end - f->start);
        } else {
            parser_fail(p, p_peek(p), "cparse ppdir: empty without FileTape");
            return 0;
        }
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
            !shadow_toks_are_c(p, flo, fhi)) {
            if (shadow_attach_cc_field(p, st, flo, fhi)) return 1;
            if (p->err) return 0;
            /* C declarator (fn-ptr) with type-position sugar: same emit
             * rewrite as fn params. @ / => / :: stay overlay-only. */
            if (shadow_toks_need_overlay_field(p, flo, fhi)) {
                parser_fail(p, p->toks[flo],
                            "cparse CC field: overlay parse failed");
                return 0;
            }
        }
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

static int shadow_match_pair(Parser* p, int open, const char* op, const char* cl) {
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

static int shadow_find_punct(Parser* p, int from, const char* lit) {
    int i;
    for (i = from; i < p->n; i++) {
        if (tok_eq(p->toks[i], TK_PUNCT, lit)) return i;
    }
    return -1;
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

/* Param default `x = 3` is overlay meaning (emit peels; FileTape must not). */
static int shadow_fn_params_have_default(Parser* p, int lo, int hi) {
    int i;
    int depth = 0;
    int in_params = 0;
    for (i = lo; i < hi && i < p->n; i++) {
        Token t = p->toks[i];
        if (!in_params) {
            if (tok_eq(t, TK_PUNCT, "(")) {
                in_params = 1;
                depth = 1;
            }
            continue;
        }
        if (tok_eq(t, TK_PUNCT, "(") || tok_eq(t, TK_PUNCT, "[") ||
            tok_eq(t, TK_PUNCT, "{"))
            depth++;
        else if (tok_eq(t, TK_PUNCT, ")") || tok_eq(t, TK_PUNCT, "]") ||
                 tok_eq(t, TK_PUNCT, "}")) {
            depth--;
            if (depth == 0) return 0; /* end of params */
        } else if (depth == 1 && tok_eq(t, TK_PUNCT, "="))
            return 1;
    }
    return 0;
}

/* Tape rewrite can hide `@` from tokens; check source bytes. */
static int shadow_fn_span_has_at(Parser* p, int lo, int hi) {
    FileTape* ft;
    size_t a, b, i;
    if (lo < 0 || hi <= lo || hi > p->n) return 0;
    ft = tape_by_id(p->cache, p->toks[lo].file_id);
    if (!ft || !ft->bytes) return 0;
    a = p->toks[lo].offset;
    b = p->toks[hi - 1].offset + p->toks[hi - 1].spell.len;
    if (b > ft->len) b = ft->len;
    for (i = a; i < b; i++) {
        if (ft->bytes[i] == '@') return 1;
    }
    return 0;
}

/* Set while a C-only file-scope fn matched cparse; overlay must attach kids
 * (no soft RAW_LINE / FileTape whole-fn fallback). */
static int shadow_cparse_fn_owned = 0;
/* Re-enter beachhead builders without re-matching cparse. */
static int shadow_cparse_fn_in_attach = 0;

static int shadow_cparse_fn_take_owned(void) {
    int o = shadow_cparse_fn_owned;
    shadow_cparse_fn_owned = 0;
    return o;
}

/* C-only file-scope function envelope. 1 = matched (overlay must attach
 * kids), 0 = not this shape, -1 = cparse failed loud. */
static int shadow_cparse_try_file_fn(Parser* p) {
    CpTok* cpt = NULL;
    char err[192];
    char name[128];
    FileTape* ft;
    int lb;
    int rb;
    int nneed;
    int ntok;
    int rel = -1;
    shadow_cparse_fn_owned = 0;
    if (!shadow_looks_like_fn_def(p)) return 0;
    lb = shadow_find_punct(p, p->i, "{");
    if (lb < 0) return 0;
    rb = shadow_match_pair(p, lb, "{", "}");
    /* Unclosed body: beachhead names the function (`expected '}' to close
     * 'first_draw'`). Do not steal with a generic "unclosed function". */
    if (rb < 0) return 0;
    {
        int overlay = 0;
        if (!shadow_toks_are_c(p, p->i, rb + 1)) overlay = 1;
        if (shadow_toks_have_ufcs_call(p, p->i, rb + 1)) overlay = 1;
        if (shadow_fn_params_have_default(p, p->i, lb)) overlay = 1;
        if (shadow_fn_span_has_at(p, p->i, rb + 1)) overlay = 1;
        nneed = (rb + 1) - p->i;
        if (nneed < 0) return 0;
        cpt = (CpTok*)malloc((size_t)nneed * sizeof(CpTok));
        if (nneed > 0 && !cpt) {
            parser_fail(p, p_peek(p), "cparse fn: oom");
            return -1;
        }
        ntok = shadow_fill_cptoks(p, p->i, rb + 1, cpt, nneed > 0 ? nneed : 0);
        if (ntok < 0) {
            parser_fail(p, p_peek(p), "cparse fn: token fill failed");
            free(cpt);
            return -1;
        }
        ft = tape_by_id(p->cache, p->toks[p->i].file_id);
        if (!ft || !ft->bytes) {
            parser_fail(p, p_peek(p), "cparse fn: missing FileTape");
            free(cpt);
            return -1;
        }
        err[0] = 0;
        name[0] = 0;
        if (cparse_match_func(ft->bytes, (int)ft->len, cpt, ntok, name,
                              (int)sizeof(name), &rel, err,
                              (int)sizeof(err)) != 0) {
            free(cpt);
            /* Overlay (Result / defaults / @ / UFCS): beachhead attaches.
             * Pure-C miss: yield to beachhead (macros, shapes cparse
             * still lacks) — never loud-fail a false envelope steal. */
            if (overlay) {
                shadow_cparse_fn_owned = 1;
                return 1;
            }
            return 0;
        }
        free(cpt);
        (void)rel;
        shadow_cparse_fn_owned = 1;
        return 1;
    }
}

/* One FileTape stmt kid for a token range [lo, hi). */
static AstNode* shadow_tape_tok_range(Parser* p, int lo, int hi) {
    AstNode* raw;
    Token t0, last;
    size_t off0, off1;
    if (lo < 0 || hi <= lo || hi > p->n) return NULL;
    t0 = p->toks[lo];
    last = p->toks[hi - 1];
    off0 = t0.offset;
    off1 = last.offset + last.spell.len;
    raw = ast_new(p, AST_RAW_LINE);
    if (!raw) return NULL;
    snprintf(raw->e, sizeof(raw->e), "tape");
    snprintf(raw->a, sizeof(raw->a), "%zu", off0);
    snprintf(raw->b, sizeof(raw->b), "%zu", off1);
    raw->file_id = t0.file_id;
    raw->tok_off = off0;
    return raw;
}

/* After '{': attach body kids. Hard-parse overlay stmts (never FileTape
 * reprint of !> / @ / =>). Pure-C soft misses may tape one cparse stmt. */
static int shadow_cparse_attach_fn_body(Parser* p, AstNode* n, int fn_start,
                                        int body0) {
    FileTape* ft;
    CpTok* cpt = NULL;
    int starts[256];
    int ends[256];
    int nspans = 0;
    char err[192];
    int nneed;
    int ntok;
    int rb;
    int k;
    int saved_soft;
    int saved_err;
    n->kids = &p->kids_storage[p->nkstore];
    n->nkids = 0;
    rb = shadow_match_pair(p, body0 - 1, "{", "}");
    if (rb < 0) {
        parser_fail(p, p->toks[body0 > 0 ? body0 - 1 : body0],
                    "unclosed function");
        return 0;
    }
    ft = tape_by_id(p->cache, p->toks[fn_start].file_id);
    if (!ft || !ft->bytes) {
        parser_fail(p, p_peek(p), "cparse fn body: missing FileTape");
        return 0;
    }
    nneed = (rb + 1) - fn_start;
    /* cparse stmt spans are C-shaped; !> / @ chains must not be sliced. */
    if (nneed > 0 && shadow_toks_are_c(p, body0, rb)) {
        cpt = (CpTok*)malloc((size_t)nneed * sizeof(CpTok));
        if (!cpt) {
            parser_fail(p, p_peek(p), "cparse fn body: oom");
            return 0;
        }
        ntok = shadow_fill_cptoks(p, fn_start, rb + 1, cpt, nneed);
        err[0] = 0;
        if (ntok >= 0 &&
            cparse_func_stmt_spans(ft->bytes, (int)ft->len, cpt, ntok, starts,
                                   ends, 256, &nspans, err,
                                   (int)sizeof(err)) == 0 &&
            nspans > 0) {
            free(cpt);
            cpt = NULL;
            saved_soft = p->soft_stmt;
            saved_err = p->err;
            for (k = 0; k < nspans; k++) {
                int flo = 0, fhi = 0;
                int is_c;
                AstNode* s = NULL;
                if (!shadow_toks_covering(p, body0, rb, starts[k], ends[k],
                                          &flo, &fhi) ||
                    flo >= fhi) {
                    continue;
                }
                is_c = shadow_toks_are_c(p, flo, fhi);
                p->i = flo;
                p->err = saved_err;
                /* Beachhead hard parse; C-only soft miss may tape one span. */
                p->soft_stmt = 0;
                s = parse_stmt(p);
                p->soft_stmt = saved_soft;
                if (s && !p->err && p->i >= fhi) {
                    if (!ast_kids_push(p, s)) return 0;
                    n->nkids++;
                    continue;
                }
                if (!is_c) {
                    if (!p->err)
                        parser_fail(p, p->toks[flo],
                                    "cparse fn: overlay stmt attach failed");
                    return 0;
                }
                p->err = saved_err;
                s = shadow_tape_tok_range(p, flo, fhi);
                if (!s || !ast_kids_push(p, s)) return 0;
                n->nkids++;
                p->i = fhi;
            }
            p->i = rb;
            return 1;
        }
        free(cpt);
        cpt = NULL;
    }
    /* No cparse spans: hard-parse like beachhead parse_fn (no body tape). */
    saved_soft = p->soft_stmt;
    p->i = body0;
    p->soft_stmt = 0;
    while (!tok_eq(p_peek(p), TK_PUNCT, "}") && p_peek(p).kind != TK_EOF &&
           !p->err) {
        AstNode* s = parse_stmt(p);
        if (!s) {
            p->soft_stmt = saved_soft;
            if (!p->err)
                parser_fail(p, p_peek(p),
                            "cparse fn: overlay body attach failed");
            return 0;
        }
        if (!ast_kids_push(p, s)) {
            p->soft_stmt = saved_soft;
            return 0;
        }
        n->nkids++;
    }
    p->soft_stmt = saved_soft;
    return 1;
}
