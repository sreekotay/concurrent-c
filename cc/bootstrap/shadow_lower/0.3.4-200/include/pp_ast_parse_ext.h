/* External / TU parsers: functions, typedefs, parse_tu, ast_kind_name.
 * Requires pp_ast_core.cch + pp_ast_parse_stmt.cch. */
#pragma once

/* [const] [struct] Ret[*+] name ( params ) { body } — ret in e (default int). */
static AstNode* parse_fn(Parser* p) {
    int ti = p->i;
    int has_const = 0;
    int has_struct = 0;
    int nstars = 0;
    int name_i;
    int j;
    int depth;
    Token rty;
    ShadowKwKind rkw;
    if (ti < p->n && shadow_kw(p->toks[ti]) == SHADOW_KW_CONST) {
        has_const = 1;
        ti++;
    }
    if (ti < p->n && shadow_kw(p->toks[ti]) == SHADOW_KW_STRUCT) {
        has_struct = 1;
        ti++;
    }
    if (ti >= p->n) return NULL;
    rty = p->toks[ti];
    rkw = shadow_kw(rty);
    if (has_struct) {
        if (rty.kind != TK_IDENT) return NULL;
    } else if (rty.kind != TK_IDENT && rkw != SHADOW_KW_INT &&
               rkw != SHADOW_KW_VOID && rkw != SHADOW_KW_CHAR &&
               rkw != SHADOW_KW_BOOL && rkw != SHADOW_KW_SIZE_T)
        return NULL;
    /* `T!>(E)` is parse_result_fn. `T[:]` / `T[:!]` is a slice return. */
    if (!has_struct && ti + 1 < p->n &&
        tok_eq(p->toks[ti + 1], TK_PUNCT, "!>"))
        return NULL;
    name_i = ti + 1;
    {
        int is_slice = 0;
        int is_slice_unique = 0;
        if (!has_struct && name_i + 2 < p->n &&
            tok_eq(p->toks[name_i], TK_PUNCT, "[") &&
            tok_eq(p->toks[name_i + 1], TK_PUNCT, ":")) {
            if (tok_eq(p->toks[name_i + 2], TK_PUNCT, "]")) {
                is_slice = 1;
                name_i += 3;
            } else if (name_i + 3 < p->n &&
                       tok_eq(p->toks[name_i + 2], TK_PUNCT, "!") &&
                       tok_eq(p->toks[name_i + 3], TK_PUNCT, "]")) {
                is_slice = 1;
                is_slice_unique = 1;
                name_i += 4;
            } else
                return NULL;
        }
        while (name_i < p->n && tok_eq(p->toks[name_i], TK_PUNCT, "*")) {
            nstars++;
            name_i++;
        }
        if (name_i + 3 >= p->n) return NULL;
        if (p->toks[name_i].kind != TK_IDENT) return NULL;
        if (!tok_eq(p->toks[name_i + 1], TK_PUNCT, "(")) return NULL;
        /* Find matching `)` then `{` */
        j = name_i + 2;
        depth = 1;
        while (j < p->n && depth > 0) {
            if (tok_eq(p->toks[j], TK_PUNCT, "(")) depth++;
            else if (tok_eq(p->toks[j], TK_PUNCT, ")")) { depth--; if (depth == 0) break; }
            j++;
        }
        if (j >= p->n || !tok_eq(p->toks[j], TK_PUNCT, ")")) return NULL;
        if (j + 1 >= p->n || !tok_eq(p->toks[j + 1], TK_PUNCT, "{")) return NULL;

        if (has_const) p_next(p); /* const */
        if (has_struct) p_next(p); /* struct */
        p_next(p); /* ret / tag */
        if (is_slice) {
            p_next(p); /* [ */
            p_next(p); /* : */
            if (is_slice_unique) p_next(p); /* ! */
            p_next(p); /* ] */
        }
        {
            int stars_eat = nstars;
            while (stars_eat-- > 0) p_next(p); /* * */
        }
        Token name = p_next(p);
        p_next(p); /* ( */
        int p0 = p->i;
        while (p->i < j) p_next(p);
        char params[4096] = {0};
        if (p0 < p->i) {
            if (!ast_spell_token_range(p, p0, p->i, params, sizeof(params)))
                return NULL;
        }
        p_next(p); /* ) */
        p_next(p); /* { */
        AstNode* n = ast_new(p, AST_FN);
        if (!n) return NULL;
        slice_to(n->a, sizeof(n->a), name.spell);
        snprintf(n->b, sizeof(n->b), "%s", params);
        {
            char rbuf[256];
            int stars = nstars;
            if (has_const && has_struct)
                snprintf(rbuf, sizeof(rbuf), "const struct %.*s",
                         (int)rty.spell.len, rty.spell.ptr);
            else if (has_struct)
                snprintf(rbuf, sizeof(rbuf), "struct %.*s",
                         (int)rty.spell.len, rty.spell.ptr);
            else if (has_const)
                snprintf(rbuf, sizeof(rbuf), "const %.*s",
                         (int)rty.spell.len, rty.spell.ptr);
            else
                slice_to(rbuf, sizeof(rbuf), rty.spell);
            if (is_slice) {
                size_t al = strlen(rbuf);
                const char* suf = is_slice_unique ? "[:!]" : "[:]";
                size_t sl = strlen(suf);
                if (al + sl < sizeof(rbuf))
                    memcpy(rbuf + al, suf, sl + 1);
            }
            while (stars-- > 0) {
                size_t al = strlen(rbuf);
                if (al + 1 < sizeof(rbuf)) {
                    rbuf[al] = '*';
                    rbuf[al + 1] = 0;
                }
            }
            snprintf(n->e, sizeof(n->e), "%s", rbuf);
        }
        n->kids = &p->kids_storage[p->nkstore];
        while (!tok_eq(p_peek(p), TK_PUNCT, "}") && p_peek(p).kind != TK_EOF && !p->err) {
            AstNode* s = parse_stmt(p);
            if (!s) return NULL;
            if (!ast_kids_push(p, s)) return NULL;
            n->nkids++;
        }
        if (!p_accept(p, TK_PUNCT, "}")) {
            parser_fail(p, p_peek(p), "expected '}' to close function");
            return NULL;
        }
        return n;
    }
}

/* () => { } ; */
static AstNode* parse_closure_lit(Parser* p) {
    if (!tok_eq(p_peek(p), TK_PUNCT, "(")) return NULL;
    if (p->i + 5 >= p->n) return NULL;
    if (!tok_eq(p->toks[p->i + 1], TK_PUNCT, ")") ||
        !tok_eq(p->toks[p->i + 2], TK_PUNCT, "=>") ||
        !tok_eq(p->toks[p->i + 3], TK_PUNCT, "{") ||
        !tok_eq(p->toks[p->i + 4], TK_PUNCT, "}"))
        return NULL;
    p_next(p); p_next(p); p_next(p); p_next(p); p_next(p);
    p_accept(p, TK_PUNCT, ";");
    return ast_new(p, AST_CLOSURE_LIT);
}

/* @with_deadline(expr) [as name] { stmts } */
static AstNode* parse_with_deadline(Parser* p) {
    if (!tok_eq(p_peek(p), TK_PUNCT, "@")) return NULL;
    if (p->i + 1 >= p->n || shadow_kw(p->toks[p->i + 1]) != SHADOW_KW_WITH_DEADLINE)
        return NULL;
    p_next(p); /* @ */
    p_next(p); /* with_deadline */
    if (!tok_eq(p_peek(p), TK_PUNCT, "(")) {
        parser_fail(p, p_peek(p), "expected '(' after @with_deadline");
        return NULL;
    }
    int e0 = p->i + 1;
    if (!skip_parens(p)) {
        parser_fail(p, p_peek(p), "unterminated @with_deadline(...)");
        return NULL;
    }
    int e1 = p->i - 1;
    AstNode* n = ast_new(p, AST_WITH_DEADLINE);
    if (!n) return NULL;
    if (e0 < e1 && !span_text(p, e0, e1, n->a, sizeof(n->a))) {
        parser_fail(p, p_peek(p), "@with_deadline expr too long");
        return NULL;
    }
    if (shadow_kw(p_peek(p)) == SHADOW_KW_AS) {
        p_next(p);
        Token bind = p_next(p);
        if (bind.kind != TK_IDENT) {
            parser_fail(p, bind, "expected name after @with_deadline … as");
            return NULL;
        }
        slice_to(n->b, sizeof(n->b), bind.spell);
    }
    if (!tok_eq(p_peek(p), TK_PUNCT, "{")) {
        parser_fail(p, p_peek(p), "expected '{' after @with_deadline");
        return NULL;
    }
    p_next(p);
    while (!tok_eq(p_peek(p), TK_PUNCT, "}") && p_peek(p).kind != TK_EOF && !p->err) {
        AstNode* s = parse_stmt(p);
        if (!s) return NULL;
        if (n->nbody >= SHADOW_BODY_CAP) {
            parser_fail_body_cap(p, p_peek(p), "@with_deadline body");
            return NULL;
        }
        n->body[n->nbody++] = s;
    }
    if (!p_accept(p, TK_PUNCT, "}")) {
        parser_fail(p, p_peek(p), "expected '}' to close @with_deadline");
        return NULL;
    }
    return n;
}

/* @parallel { name = expr; | @serial { … }; … }
 * @parallel (pred) { … }
 * @parallel for (name in lo..hi) { … } */
static int parallel_lhs_is_ident(const char* s) {
    const char* p = s ? s : "";
    while (*p == ' ' || *p == '\t') p++;
    if (!((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || *p == '_'))
        return 0;
    p++;
    while (*p) {
        if (!((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') ||
              (*p >= '0' && *p <= '9') || *p == '_')) {
            while (*p == ' ' || *p == '\t') p++;
            return *p == 0;
        }
        p++;
    }
    return 1;
}

/* Half-open `lo..hi`. `0..n` may be one pp-number; `lo..hi` is `.` `.`. */
static int parallel_split_range(Parser* p, int end, char* lo, size_t locap,
                                char* hi, size_t hicap) {
    int j = p->i;
    int depth = 0;
    int lo0 = j;
    while (j < end) {
        Token t = p->toks[j];
        if (tok_eq(t, TK_PUNCT, "(") || tok_eq(t, TK_PUNCT, "[") ||
            tok_eq(t, TK_PUNCT, "{"))
            depth++;
        else if (tok_eq(t, TK_PUNCT, ")") || tok_eq(t, TK_PUNCT, "]") ||
                 tok_eq(t, TK_PUNCT, "}"))
            depth--;
        else if (depth == 0 && tok_eq(t, TK_PUNCT, ".") && j + 1 < end &&
                 tok_eq(p->toks[j + 1], TK_PUNCT, ".")) {
            if (j <= lo0 ||
                !span_text(p, lo0, j, lo, locap) ||
                !span_text(p, j + 2, end, hi, hicap))
                return 0;
            return 1;
        } else if (depth == 0 && t.kind == TK_NUM && t.spell.len >= 3) {
            size_t k;
            for (k = 0; k + 1 < t.spell.len; k++) {
                if (t.spell.ptr[k] == '.' && t.spell.ptr[k + 1] == '.') {
                    size_t ln = k;
                    size_t hn = t.spell.len - (k + 2);
                    if (!ln || ln >= locap || hn >= hicap) return 0;
                    memcpy(lo, t.spell.ptr, ln);
                    lo[ln] = 0;
                    memcpy(hi, t.spell.ptr + k + 2, hn);
                    hi[hn] = 0;
                    if (j + 1 < end) {
                        char rest[256];
                        if (!span_text(p, j + 1, end, rest, sizeof(rest)))
                            return 0;
                        if (rest[0]) {
                            size_t used = strlen(hi);
                            if (used + 1 + strlen(rest) >= hicap) return 0;
                            hi[used] = ' ';
                            memcpy(hi + used + 1, rest, strlen(rest) + 1);
                        }
                    }
                    return hi[0] ? 1 : 0;
                }
            }
        }
        j++;
    }
    return 0;
}

static void serial_trim_ident(const char* s, char* dst, size_t cap) {
    const char* e;
    size_t n;
    if (!dst || !cap) return;
    dst[0] = 0;
    if (!s) return;
    while (*s == ' ' || *s == '\t') s++;
    e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t')) e--;
    n = (size_t)(e - s);
    if (n + 1 > cap) n = cap - 1;
    memcpy(dst, s, n);
    dst[n] = 0;
}

static int serial_local_name(AstNode* s, char* dst, size_t cap) {
    const char* name = NULL;
    if (!s || !dst || !cap) return 0;
    dst[0] = 0;
    switch (s->kind) {
    case AST_TYPED_INIT:
    case AST_VAR_DECL:
    case AST_PTR_INIT:
    case AST_VAL_DESTROY:
    case AST_PTR_UNWRAP:
    case AST_SLICE_INIT:
        name = s->b;
        break;
    case AST_VAR_UNWRAP:
    case AST_RESULT_LOCAL:
        name = s->a;
        break;
    default:
        return 0;
    }
    serial_trim_ident(name, dst, cap);
    return dst[0] != 0;
}

static void serial_walk_locals(AstNode* s, char names[][64], int* n, int cap) {
    char id[64];
    int k;
    if (!s || !n) return;
    if (serial_local_name(s, id, sizeof(id))) {
        int i, hit = 0;
        for (i = 0; i < *n; i++) {
            if (strcmp(names[i], id) == 0) {
                hit = 1;
                break;
            }
        }
        if (!hit && *n < cap) {
            snprintf(names[*n], 64, "%s", id);
            (*n)++;
        }
    }
    for (k = 0; k < s->nbody; k++)
        serial_walk_locals(s->body[k], names, n, cap);
    for (k = 0; k < s->ndbody; k++)
        serial_walk_locals(s->dbody[k], names, n, cap);
    if (s->kids) {
        for (k = 0; k < s->nkids; k++)
            serial_walk_locals(s->kids[k], names, n, cap);
    }
}

static int serial_name_in(char names[][64], int n, const char* id) {
    int i;
    if (!id || !id[0]) return 0;
    for (i = 0; i < n; i++) {
        if (strcmp(names[i], id) == 0) return 1;
    }
    return 0;
}

static void serial_walk_dests(AstNode* s, char locals[][64], int nloc,
                              char dests[][128], int* nd, int cap) {
    int k;
    if (!s || !nd) return;
    if (s->kind == AST_ASSIGN && parallel_lhs_is_ident(s->a)) {
        char id[128];
        serial_trim_ident(s->a, id, sizeof(id));
        if (id[0] && !serial_name_in(locals, nloc, id)) {
            int i, hit = 0;
            for (i = 0; i < *nd; i++) {
                if (strcmp(dests[i], id) == 0) {
                    hit = 1;
                    break;
                }
            }
            if (!hit && *nd < cap) {
                snprintf(dests[*nd], 128, "%s", id);
                (*nd)++;
            }
        }
    }
    for (k = 0; k < s->nbody; k++)
        serial_walk_dests(s->body[k], locals, nloc, dests, nd, cap);
    for (k = 0; k < s->ndbody; k++)
        serial_walk_dests(s->dbody[k], locals, nloc, dests, nd, cap);
    if (s->kids) {
        for (k = 0; k < s->nkids; k++)
            serial_walk_dests(s->kids[k], locals, nloc, dests, nd, cap);
    }
}

static AstNode* parse_serial_arm(Parser* p) {
    Token at = p_peek(p);
    AstNode* n;
    if (!tok_eq(at, TK_PUNCT, "@")) return NULL;
    if (p->i + 1 >= p->n || shadow_kw(p->toks[p->i + 1]) != SHADOW_KW_SERIAL)
        return NULL;
    p_next(p); /* @ */
    p_next(p); /* serial */
    if (!tok_eq(p_peek(p), TK_PUNCT, "{")) {
        parser_fail(p, p_peek(p), "expected '{' after @serial");
        return NULL;
    }
    p_next(p);
    n = ast_new(p, AST_SERIAL);
    if (!n) return NULL;
    while (!tok_eq(p_peek(p), TK_PUNCT, "}") && p_peek(p).kind != TK_EOF &&
           !p->err) {
        AstNode* s = parse_stmt(p);
        if (!s) return NULL;
        if (n->nbody >= SHADOW_BODY_CAP) {
            parser_fail_body_cap(p, p_peek(p), "@serial body");
            return NULL;
        }
        n->body[n->nbody++] = s;
    }
    if (!p_accept(p, TK_PUNCT, "}")) {
        parser_fail(p, p_peek(p), "expected '}' to close @serial");
        return NULL;
    }
    if (n->nbody < 1) {
        parser_fail(p, at, "@serial arm must not be empty");
        return NULL;
    }
    {
        char locals[32][64];
        char dests[8][128];
        int nloc = 0, nd = 0, k;
        for (k = 0; k < n->nbody; k++)
            serial_walk_locals(n->body[k], locals, &nloc, 32);
        for (k = 0; k < n->nbody; k++)
            serial_walk_dests(n->body[k], locals, nloc, dests, &nd, 8);
        if (nd == 0) {
            parser_fail(p, at, "@serial arm must assign a simple outer name");
            return NULL;
        }
        if (nd > 1) {
            parser_fail(p, at,
                        "@serial arm must assign exactly one simple outer name");
            return NULL;
        }
        snprintf(n->a, sizeof(n->a), "%s", dests[0]);
    }
    return n;
}

static AstNode* parse_parallel_block(Parser* p) {
    AstNode* n = ast_new(p, AST_PARALLEL);
    if (!n) return NULL;
    n->forced_seq = p->parallel_off;
    if (!tok_eq(p_peek(p), TK_PUNCT, "{")) {
        parser_fail(p, p_peek(p), "expected '{' after @parallel");
        return NULL;
    }
    p_next(p);
    while (!tok_eq(p_peek(p), TK_PUNCT, "}") && p_peek(p).kind != TK_EOF &&
           !p->err) {
        Token at = p_peek(p);
        AstNode* s;
        if (tok_eq(at, TK_PUNCT, "{")) {
            parser_fail(p, at,
                        "bare `{ }` is not an @parallel arm; use `@serial { }`");
            return NULL;
        }
        if (tok_eq(at, TK_PUNCT, "@") && p->i + 1 < p->n &&
            shadow_kw(p->toks[p->i + 1]) == SHADOW_KW_SERIAL) {
            s = parse_serial_arm(p);
            if (!s) return NULL;
        } else {
            s = parse_stmt(p);
            if (!s) return NULL;
            if (s->kind != AST_ASSIGN || (s->c[0] && strcmp(s->c, "=") != 0)) {
                parser_fail(p, at,
                            "@parallel arm must be `name = expr;` or `@serial { … }`");
                return NULL;
            }
            if (!parallel_lhs_is_ident(s->a)) {
                parser_fail(p, at,
                            "@parallel left-hand side must be a simple name");
                return NULL;
            }
        }
        if (n->nbody >= SHADOW_BODY_CAP) {
            parser_fail_body_cap(p, p_peek(p), "@parallel body");
            return NULL;
        }
        n->body[n->nbody++] = s;
    }
    if (!p_accept(p, TK_PUNCT, "}")) {
        parser_fail(p, p_peek(p), "expected '}' to close @parallel");
        return NULL;
    }
    if (n->nbody < 2) {
        parser_fail(p, p_peek(p), "@parallel needs at least two arms");
        return NULL;
    }
    return n;
}

/* gate/seq_cond are "" for the bare bisect form (`@parallel for`). */
static AstNode* parse_parallel_for(Parser* p, const char* gate,
                                   const char* seq_cond) {
    Token name;
    int rp;
    int depth;
    AstNode* n;
    p_next(p); /* for */
    if (!tok_eq(p_peek(p), TK_PUNCT, "(")) {
        parser_fail(p, p_peek(p), "expected '(' after @parallel for");
        return NULL;
    }
    p_next(p);
    name = p_next(p);
    if (name.kind != TK_IDENT) {
        parser_fail(p, name, "expected name after @parallel for (");
        return NULL;
    }
    if (!tok_eq(p_peek(p), TK_IDENT, "in")) {
        parser_fail(p, p_peek(p),
                    "expected 'in' after @parallel for (name");
        return NULL;
    }
    p_next(p); /* in */
    rp = p->i;
    depth = 1;
    while (rp < p->n && depth > 0) {
        Token t = p->toks[rp];
        if (tok_eq(t, TK_PUNCT, "(")) depth++;
        else if (tok_eq(t, TK_PUNCT, ")")) {
            depth--;
            if (depth == 0) break;
        }
        rp++;
    }
    if (rp >= p->n || depth != 0) {
        parser_fail(p, p_peek(p), "unterminated @parallel for (...)");
        return NULL;
    }
    n = ast_new(p, AST_PARALLEL_FOR);
    if (!n) return NULL;
    n->forced_seq = p->parallel_off;
    slice_to(n->a, sizeof(n->a), name.spell);
    if (gate && gate[0]) snprintf(n->e, sizeof(n->e), "%s", gate);
    if (seq_cond && seq_cond[0]) snprintf(n->f, sizeof(n->f), "%s", seq_cond);
    if (!parallel_split_range(p, rp, n->b, sizeof(n->b), n->c, sizeof(n->c))) {
        parser_fail(p, p_peek(p),
                    "expected `lo..hi` after @parallel for (name in");
        return NULL;
    }
    p->i = rp;
    if (!p_accept(p, TK_PUNCT, ")")) {
        parser_fail(p, p_peek(p), "expected ')' after @parallel for range");
        return NULL;
    }
    if (tok_eq(p_peek(p), TK_IDENT, "worker")) {
        Token w;
        p_next(p); /* worker */
        if (!gate || !gate[0]) {
            parser_fail(p, p_peek(p),
                        "worker (name) requires the wait (gate) form");
            return NULL;
        }
        if (!p_accept(p, TK_PUNCT, "(")) {
            parser_fail(p, p_peek(p), "expected '(' after worker");
            return NULL;
        }
        w = p_next(p);
        if (w.kind != TK_IDENT) {
            parser_fail(p, w, "worker binder must be a simple name");
            return NULL;
        }
        slice_to(n->g, sizeof(n->g), w.spell);
        if (!p_accept(p, TK_PUNCT, ")")) {
            parser_fail(p, p_peek(p), "worker (name) takes a single name");
            return NULL;
        }
    }
    if (!tok_eq(p_peek(p), TK_PUNCT, "{")) {
        parser_fail(p, p_peek(p), "expected '{' after @parallel for (...)");
        return NULL;
    }
    p_next(p);
    while (!tok_eq(p_peek(p), TK_PUNCT, "}") && p_peek(p).kind != TK_EOF &&
           !p->err) {
        AstNode* s = parse_stmt(p);
        if (!s) return NULL;
        if (n->nbody >= SHADOW_BODY_CAP) {
            parser_fail_body_cap(p, p_peek(p), "@parallel for body");
            return NULL;
        }
        n->body[n->nbody++] = s;
    }
    if (!p_accept(p, TK_PUNCT, "}")) {
        parser_fail(p, p_peek(p), "expected '}' to close @parallel for");
        return NULL;
    }
    return n;
}

/* `seq (cond)` / legacy `(pred)` condition spelling into dst. */
static int parallel_spell_cond(Parser* p, const char* what, char* dst,
                               size_t cap) {
    Token at = p_peek(p);
    int p0, p1;
    if (!tok_eq(at, TK_PUNCT, "(")) {
        parser_fail(p, at, "expected '(' to open condition");
        return 0;
    }
    p0 = p->i + 1;
    if (!skip_parens(p)) {
        parser_fail(p, at, "unterminated @parallel (...)");
        return 0;
    }
    p1 = p->i - 1;
    if (p0 >= p1) {
        parser_fail(p, at, "expected predicate in @parallel (...)");
        return 0;
    }
    if (!ast_spell_token_range(p, p0, p1, dst, cap) || !dst[0]) {
        parser_fail(p, at, what);
        return 0;
    }
    return 1;
}

/* `cache (name {, name})` after wait — enclosing locals adopted as scratch. */
static int parallel_spell_cache_names(Parser* p, char* dst, size_t cap) {
    Token at = p_peek(p);
    size_t n = 0;
    dst[0] = 0;
    if (!p_accept(p, TK_PUNCT, "(")) {
        parser_fail(p, at, "expected '(' after cache");
        return 0;
    }
    if (tok_eq(p_peek(p), TK_PUNCT, ")")) {
        parser_fail(p, p_peek(p), "cache (name) needs at least one name");
        return 0;
    }
    for (;;) {
        Token id = p_next(p);
        char tmp[64];
        size_t i;
        if (id.kind != TK_IDENT) {
            parser_fail(p, id, "cache binder must be a simple name");
            return 0;
        }
        slice_to(tmp, sizeof(tmp), id.spell);
        if (!tmp[0]) {
            parser_fail(p, id, "cache binder must be a simple name");
            return 0;
        }
        if (n && n + 1 < cap) dst[n++] = ',';
        i = 0;
        while (tmp[i] && n + 1 < cap) dst[n++] = tmp[i++];
        dst[n] = 0;
        if (n + 1 >= cap) {
            parser_fail(p, id, "cache (name) list too long");
            return 0;
        }
        if (p_accept(p, TK_PUNCT, ",")) continue;
        break;
    }
    if (!p_accept(p, TK_PUNCT, ")")) {
        parser_fail(p, p_peek(p), "expected ')' after cache (...)");
        return 0;
    }
    return dst[0] != 0;
}

/* Wait-for `break` that targets this construct, not an inner loop. */
static int shadow_pw_break_targets(AstNode* s, int inner_brk) {
    int k;
    int next;
    if (!s) return 0;
    if (s->kind == AST_PARALLEL || s->kind == AST_PARALLEL_FOR) return 0;
    if (s->kind == AST_BREAK && !inner_brk) return 1;
    next = inner_brk;
    if (s->kind == AST_FOR || s->kind == AST_WHILE ||
        s->kind == AST_DO_WHILE || s->kind == AST_SWITCH)
        next = inner_brk + 1;
    for (k = 0; k < s->nbody; k++) {
        if (shadow_pw_break_targets(s->body[k], next)) return 1;
    }
    for (k = 0; k < s->ndbody; k++) {
        if (shadow_pw_break_targets(s->dbody[k], next)) return 1;
    }
    if (s->kids) {
        for (k = 0; k < s->nkids; k++) {
            if (shadow_pw_break_targets(s->kids[k], next)) return 1;
        }
    }
    return 0;
}

static int shadow_pw_body_has_break(AstNode* n) {
    int k;
    if (!n) return 0;
    for (k = 0; k < n->nbody; k++) {
        if (shadow_pw_break_targets(n->body[k], 0)) return 1;
    }
    return 0;
}

static const char* shadow_pw_result_dest(AstNode* n) {
    int k;
    if (!n) return NULL;
    for (k = 0; k < n->ndbody; k++) {
        AstNode* d = n->dbody[k];
        if (d && d->kind == AST_VAR_DECL && d->b[0]) return d->b;
    }
    return NULL;
}

static AstNode* shadow_pw_bang_eh(AstNode* n) {
    int k;
    if (!n) return NULL;
    for (k = 0; k < n->ndbody; k++) {
        if (n->dbody[k] && n->dbody[k]->kind == AST_ERRHANDLER)
            return n->dbody[k];
    }
    return NULL;
}

static int parallel_attach_dest(Parser* p, AstNode* n, const char* name,
                                const char* ty) {
    AstNode* d;
    if (!n || !name || !name[0]) return 0;
    d = ast_new(p, AST_VAR_DECL);
    if (!d) return 0;
    if (ty && ty[0]) snprintf(d->a, sizeof(d->a), "%s", ty);
    snprintf(d->b, sizeof(d->b), "%s", name);
    if (n->ndbody >= SHADOW_DBODY_CAP) {
        parser_fail(p, p_peek(p), "too many @parallel attachments");
        return 0;
    }
    n->dbody[n->ndbody++] = d;
    return 1;
}

static int parse_parallel_bang_tail(Parser* p, AstNode* n) {
    Token at;
    if (!tok_eq(p_peek(p), TK_PUNCT, "!>")) return 1;
    at = p_next(p); /* !> */
    if (p_accept(p, TK_PUNCT, ";")) return 1;
    if (p_accept(p, TK_PUNCT, "(")) {
        Token bind = p_next(p);
        AstNode* eh;
        if (bind.kind != TK_IDENT) {
            parser_fail(p, bind, "expected bind name in @parallel !>(...)");
            return 0;
        }
        if (!p_accept(p, TK_PUNCT, ")")) {
            parser_fail(p, p_peek(p), "expected ')' after @parallel !>(bind");
            return 0;
        }
        eh = ast_new(p, AST_ERRHANDLER);
        if (!eh) return 0;
        snprintf(eh->a, sizeof(eh->a), "CCError");
        slice_to(eh->b, sizeof(eh->b), bind.spell);
        if (tok_eq(p_peek(p), TK_PUNCT, "{")) {
            p_next(p);
            while (!tok_eq(p_peek(p), TK_PUNCT, "}") && p_peek(p).kind != TK_EOF &&
                   !p->err) {
                AstNode* s = parse_stmt(p);
                if (!s) return 0;
                if (eh->nbody >= SHADOW_BODY_CAP) {
                    parser_fail_body_cap(p, p_peek(p), "@parallel !> body");
                    return 0;
                }
                eh->body[eh->nbody++] = s;
            }
            if (!p_accept(p, TK_PUNCT, "}")) {
                parser_fail(p, p_peek(p),
                            "expected '}' after @parallel !>(...)");
                return 0;
            }
            p_accept(p, TK_PUNCT, ";");
        } else {
            AstNode* s = parse_stmt(p);
            if (!s) return 0;
            eh->body[eh->nbody++] = s;
        }
        if (n->ndbody >= SHADOW_DBODY_CAP) {
            parser_fail(p, at, "too many @parallel attachments");
            return 0;
        }
        n->dbody[n->ndbody++] = eh;
        return 1;
    }
    parser_fail(p, at, "expected '!>;' or '!>(e) { ... }' after @parallel");
    return 0;
}

static int parallel_require_result_bind(Parser* p, AstNode* n) {
    if (!n || n->kind != AST_PARALLEL_FOR || !n->e[0]) return 1;
    if (!shadow_pw_body_has_break(n)) return 1;
    if (shadow_pw_result_dest(n)) return 1;
    parser_fail(p, p_peek(p),
                "a '@parallel wait for' that can 'break' is bool !>(CCError); "
                "bind the bool (the range did not finish)");
    return 0;
}

static AstNode* parse_parallel_finish(Parser* p, AstNode* n) {
    if (!n) return NULL;
    if (!parse_parallel_bang_tail(p, n)) return NULL;
    if (!parallel_require_result_bind(p, n)) return NULL;
    return n;
}

static AstNode* parse_parallel_head(Parser* p) {
    char pred[2048];
    char gate[256];
    AstNode* n;
    int have_wait = 0;
    if (!tok_eq(p_peek(p), TK_PUNCT, "@")) return NULL;
    if (p->i + 1 >= p->n || shadow_kw(p->toks[p->i + 1]) != SHADOW_KW_PARALLEL)
        return NULL;
    p_next(p); /* @ */
    p_next(p); /* parallel */
    if (shadow_kw(p_peek(p)) == SHADOW_KW_FOR)
        return parse_parallel_for(p, "", "");
    pred[0] = 0;
    gate[0] = 0;
    if (tok_eq(p_peek(p), TK_PUNCT, "(")) {
        /* Legacy predicate form: @parallel (pred) { arms }. */
        if (!parallel_spell_cond(p, "@parallel predicate too long", pred,
                                 sizeof(pred)))
            return NULL;
        if (shadow_kw(p_peek(p)) == SHADOW_KW_FOR) {
            parser_fail(p, p_peek(p), "expected '{' after @parallel (...)");
            return NULL;
        }
    } else if (tok_eq(p_peek(p), TK_IDENT, "seq")) {
        p_next(p); /* seq */
        if (!tok_eq(p_peek(p), TK_PUNCT, "(")) {
            parser_fail(p, p_peek(p), "expected '(' after @parallel seq");
            return NULL;
        }
        if (!parallel_spell_cond(p, "@parallel seq condition too long", pred,
                                 sizeof(pred)))
            return NULL;
    }
    if (tok_eq(p_peek(p), TK_IDENT, "wait")) {
        Token name;
        p_next(p); /* wait */
        if (!p_accept(p, TK_PUNCT, "(")) {
            parser_fail(p, p_peek(p), "expected '(' after @parallel wait");
            return NULL;
        }
        name = p_next(p);
        if (name.kind != TK_IDENT) {
            parser_fail(p, name,
                        "@parallel wait gate must be a turnstile name");
            return NULL;
        }
        slice_to(gate, sizeof(gate), name.spell);
        if (!p_accept(p, TK_PUNCT, ")")) {
            parser_fail(p, p_peek(p),
                        "@parallel wait gate must be a single name");
            return NULL;
        }
        have_wait = 1;
    }
    if (have_wait) {
        char cache[256];
        cache[0] = 0;
        if (tok_eq(p_peek(p), TK_IDENT, "cache") ||
            shadow_kw(p_peek(p)) == SHADOW_KW_CACHE) {
            p_next(p); /* cache */
            if (!parallel_spell_cache_names(p, cache, sizeof(cache)))
                return NULL;
        }
        if (shadow_kw(p_peek(p)) != SHADOW_KW_FOR) {
            parser_fail(p, p_peek(p),
                        "@parallel wait (...) [cache (...)] requires "
                        "'for (name in lo..hi)'");
            return NULL;
        }
        n = parse_parallel_for(p, gate, pred);
        if (n && cache[0]) snprintf(n->h, sizeof(n->h), "%s", cache);
        return n;
    }
    if (tok_eq(p_peek(p), TK_IDENT, "cache") ||
        shadow_kw(p_peek(p)) == SHADOW_KW_CACHE) {
        parser_fail(p, p_peek(p),
                    "cache (name) requires the wait (gate) form");
        return NULL;
    }
    if (shadow_kw(p_peek(p)) == SHADOW_KW_FOR) {
        parser_fail(p, p_peek(p),
                    "@parallel seq (...) with 'for' requires 'wait (gate)'");
        return NULL;
    }
    n = parse_parallel_block(p);
    if (n && pred[0])
        snprintf(n->e, sizeof(n->e), "%s", pred);
    return n;
}

static AstNode* parse_parallel(Parser* p) {
    return parse_parallel_finish(p, parse_parallel_head(p));
}

/* `bool fin = @parallel wait (…) for (…) { … } !>;` */
static AstNode* parse_parallel_result_init(Parser* p) {
    int save = p->i;
    Token name;
    char dest[64];
    char ty[16];
    AstNode* n;
    if (shadow_kw(p_peek(p)) == SHADOW_KW_BOOL)
        snprintf(ty, sizeof(ty), "bool");
    else if (shadow_kw(p_peek(p)) == SHADOW_KW_INT)
        snprintf(ty, sizeof(ty), "int");
    else if (p_peek(p).kind == TK_IDENT &&
             spell_eq(p_peek(p).spell, "_Bool"))
        snprintf(ty, sizeof(ty), "_Bool");
    else
        return NULL;
    p_next(p);
    if (p_peek(p).kind != TK_IDENT) {
        p->i = save;
        return NULL;
    }
    if (p->i + 3 >= p->n || !tok_eq(p->toks[p->i + 1], TK_PUNCT, "=") ||
        !tok_eq(p->toks[p->i + 2], TK_PUNCT, "@") ||
        shadow_kw(p->toks[p->i + 3]) != SHADOW_KW_PARALLEL) {
        p->i = save;
        return NULL;
    }
    name = p_next(p);
    p_next(p); /* = */
    slice_to(dest, sizeof(dest), name.spell);
    n = parse_parallel_head(p);
    if (!n) return NULL;
    if (n->kind != AST_PARALLEL_FOR || !n->e[0]) {
        parser_fail(p, p_peek(p),
                    "a bool bind is only valid on '@parallel wait for'");
        return NULL;
    }
    if (!parallel_attach_dest(p, n, dest, ty)) return NULL;
    return parse_parallel_finish(p, n);
}

/* `fin = @parallel wait (…) for (…) { … } !>;` — dest already in scope. */
static AstNode* parse_parallel_result_assign(Parser* p) {
    int save = p->i;
    Token name;
    char dest[64];
    AstNode* n;
    if (p_peek(p).kind != TK_IDENT) return NULL;
    if (p->i + 3 >= p->n || !tok_eq(p->toks[p->i + 1], TK_PUNCT, "=") ||
        !tok_eq(p->toks[p->i + 2], TK_PUNCT, "@") ||
        shadow_kw(p->toks[p->i + 3]) != SHADOW_KW_PARALLEL)
        return NULL;
    name = p_next(p);
    p_next(p); /* = */
    slice_to(dest, sizeof(dest), name.spell);
    n = parse_parallel_head(p);
    if (!n) {
        p->i = save;
        return NULL;
    }
    if (n->kind != AST_PARALLEL_FOR || !n->e[0]) {
        parser_fail(p, p_peek(p),
                    "a bool bind is only valid on '@parallel wait for'");
        return NULL;
    }
    if (!parallel_attach_dest(p, n, dest, NULL)) return NULL;
    return parse_parallel_finish(p, n);
}

/* @stage (gate[, args…]) { stmts } — a=gate receiver text (everything up
 * to the first top-level comma), b=trailing args text. Lowered to
 * gate.wait(args); stmts; gate.pass(args); the enclosing wait-for
 * guarantees the pass on the error path. */
static AstNode* parse_stage(Parser* p) {
    AstNode* n;
    Token at;
    int p0, split, rp, depth;
    if (!tok_eq(p_peek(p), TK_PUNCT, "@")) return NULL;
    if (p->i + 1 >= p->n || shadow_kw(p->toks[p->i + 1]) != SHADOW_KW_STAGE)
        return NULL;
    p_next(p); /* @ */
    at = p_next(p); /* stage */
    if (!p_accept(p, TK_PUNCT, "(")) {
        parser_fail(p, p_peek(p), "expected '(' after @stage");
        return NULL;
    }
    p0 = p->i;
    split = -1;
    rp = p->i;
    depth = 1;
    while (rp < p->n && depth > 0) {
        Token t = p->toks[rp];
        if (tok_eq(t, TK_PUNCT, "(") || tok_eq(t, TK_PUNCT, "[")) depth++;
        else if (tok_eq(t, TK_PUNCT, ")") || tok_eq(t, TK_PUNCT, "]")) {
            depth--;
            if (depth == 0) break;
        } else if (depth == 1 && split < 0 && tok_eq(t, TK_PUNCT, ","))
            split = rp;
        rp++;
    }
    if (rp >= p->n || depth != 0) {
        parser_fail(p, at, "unterminated @stage (...)");
        return NULL;
    }
    n = ast_new(p, AST_STAGE);
    if (!n) return NULL;
    if (!ast_spell_token_range(p, p0, split >= 0 ? split : rp, n->a,
                               sizeof(n->a)) ||
        !n->a[0]) {
        parser_fail(p, at, "expected gate expression in @stage (...)");
        return NULL;
    }
    if (split >= 0 &&
        !ast_spell_token_range(p, split + 1, rp, n->b, sizeof(n->b))) {
        parser_fail(p, at, "@stage args too long");
        return NULL;
    }
    p->i = rp;
    if (!p_accept(p, TK_PUNCT, ")")) {
        parser_fail(p, p_peek(p), "expected ')' after @stage (...)");
        return NULL;
    }
    if (!p_accept(p, TK_PUNCT, "{")) {
        parser_fail(p, p_peek(p), "expected '{' after @stage (...)");
        return NULL;
    }
    while (!tok_eq(p_peek(p), TK_PUNCT, "}") && p_peek(p).kind != TK_EOF &&
           !p->err) {
        AstNode* s = parse_stmt(p);
        if (!s) return NULL;
        if (n->nbody >= SHADOW_BODY_CAP) {
            parser_fail_body_cap(p, p_peek(p), "@stage body");
            return NULL;
        }
        n->body[n->nbody++] = s;
    }
    if (!p_accept(p, TK_PUNCT, "}")) {
        parser_fail(p, p_peek(p), "expected '}' to close @stage");
        return NULL;
    }
    return n;
}

/* name ++ ; */
static AstNode* parse_inc(Parser* p) {
    if (p_peek(p).kind != TK_IDENT) return NULL;
    /* lhs ++ ;  — lhs may be name or name.field */
    int j = p->i;
    int depth = 0;
    int plus = -1;
    while (j < p->n) {
        Token t = p->toks[j];
        if (tok_eq(t, TK_PUNCT, "(") || tok_eq(t, TK_PUNCT, "[") || tok_eq(t, TK_PUNCT, "{"))
            depth++;
        else if (tok_eq(t, TK_PUNCT, ")") || tok_eq(t, TK_PUNCT, "]") || tok_eq(t, TK_PUNCT, "}"))
            depth--;
        else if (depth == 0 && tok_eq(t, TK_PUNCT, "++")) {
            plus = j;
            break;
        } else if (depth == 0 && tok_eq(t, TK_PUNCT, ";")) {
            return NULL;
        }
        j++;
    }
    if (plus < 0 || plus + 1 >= p->n || !tok_eq(p->toks[plus + 1], TK_PUNCT, ";"))
        return NULL;
    int l0 = p->i;
    while (p->i < plus) p_next(p);
    char lhs[128];
    if (!span_text(p, l0, plus, lhs, sizeof(lhs))) return NULL;
    p_next(p); /* ++ */
    p_next(p); /* ; */
    AstNode* n = ast_new(p, AST_INC);
    if (!n) return NULL;
    snprintf(n->a, sizeof(n->a), "%s", lhs);
    return n;
}

/* @async [@noblock|@blocking] Ret[!>(E)] name ( params ) { body }.
 * Result returns pack through malloc (task ABI stays intptr_t). */
static AstNode* parse_async_fn(Parser* p) {
    unsigned fn_attrs = SHADOW_FN_ASYNC;
    int start;
    char rbuf[160];
    int is_result = 0;
    Token name;
    if (!tok_eq(p_peek(p), TK_PUNCT, "@")) return NULL;
    if (p->i + 1 >= p->n || shadow_kw(p->toks[p->i + 1]) != SHADOW_KW_ASYNC)
        return NULL;
    if (p->i + 5 >= p->n) return NULL;
    start = p->i;
    p_next(p); /* @ */
    p_next(p); /* async */
    /* Optional ambient @noblock / @blocking after @async. */
    if (tok_eq(p_peek(p), TK_PUNCT, "@") && p->i + 1 < p->n &&
        p->toks[p->i + 1].kind == TK_IDENT) {
        char ident[64];
        unsigned bits;
        slice_to(ident, sizeof(ident), p->toks[p->i + 1].spell);
        bits = shadow_attr_bits_from_ident(ident);
        if (bits && bits != SHADOW_FN_ASYNC) {
            fn_attrs |= bits;
            p_next(p);
            p_next(p);
        }
    }
    rbuf[0] = 0;
    if (peek_result_shape(p)) {
        char okty[96];
        char errty[64];
        int ty0 = p->i;
        int ty1 = peek_c_int_type_end(p, ty0);
        Token err;
        if (ty1 > ty0) {
            if (!ast_spell_token_range(p, ty0, ty1, okty, sizeof(okty)) &&
                !span_text(p, ty0, ty1, okty, sizeof(okty))) {
                parser_fail(p, p_peek(p), "async result ok-type too long");
                return NULL;
            }
            while (p->i < ty1) p_next(p);
        } else {
            Token ok = p_next(p);
            slice_to(okty, sizeof(okty), ok.spell);
        }
        if (!shadow_parse_result_ok_slice_suffix(p, okty, sizeof(okty))) {
            parser_fail(p, p_peek(p), "bad slice sugar on async result ok-type");
            return NULL;
        }
        while (tok_eq(p_peek(p), TK_PUNCT, "*")) {
            p_next(p);
            {
                size_t al = strlen(okty);
                if (al + 1 < sizeof(okty)) {
                    okty[al] = '*';
                    okty[al + 1] = 0;
                }
            }
        }
        if (!tok_eq(p_peek(p), TK_PUNCT, "!>")) {
            parser_fail(p, p_peek(p), "expected '!>' in async result return");
            return NULL;
        }
        p_next(p); /* !> */
        if (tok_eq(p_peek(p), TK_PUNCT, "(")) {
            p_next(p);
            err = p_next(p);
            if (err.kind != TK_IDENT) {
                parser_fail(p, err, "expected error type inside !>(...)");
                return NULL;
            }
            if (!p_accept(p, TK_PUNCT, ")")) {
                parser_fail(p, p_peek(p), "expected ')' after error type");
                return NULL;
            }
        } else {
            err = p_next(p);
            if (err.kind != TK_IDENT) {
                parser_fail(p, err, "expected error type after !>");
                return NULL;
            }
        }
        slice_to(errty, sizeof(errty), err.spell);
        shadow_result_ok_ty_host(okty, sizeof(okty));
        ast_result_name(okty, errty, rbuf, sizeof(rbuf));
        is_result = 1;
        if (p_peek(p).kind != TK_IDENT ||
            p->i + 1 >= p->n || !tok_eq(p->toks[p->i + 1], TK_PUNCT, "(")) {
            parser_fail(p, p_peek(p),
                        "expected function name after async result type");
            return NULL;
        }
    } else {
        Token rty = p->toks[p->i];
        size_t ri = 0;
        if (rty.kind != TK_IDENT && shadow_kw(rty) != SHADOW_KW_INT &&
            shadow_kw(rty) != SHADOW_KW_VOID && shadow_kw(rty) != SHADOW_KW_BOOL) {
            p->i = start;
            return NULL;
        }
        /* Ret may be a pointer: `@async Result* name(...)` / `int** name`. */
        {
            int j = p->i + 1;
            while (j < p->n && tok_eq(p->toks[j], TK_PUNCT, "*")) j++;
            if (j >= p->n || p->toks[j].kind != TK_IDENT ||
                j + 1 >= p->n || !tok_eq(p->toks[j + 1], TK_PUNCT, "(")) {
                p->i = start;
                return NULL;
            }
        }
        p_next(p); /* ret base */
        slice_to(rbuf, sizeof(rbuf), rty.spell);
        ri = strlen(rbuf);
        while (tok_eq(p_peek(p), TK_PUNCT, "*") && ri + 1 < sizeof(rbuf)) {
            rbuf[ri++] = '*';
            rbuf[ri] = 0;
            p_next(p);
        }
    }
    name = p_next(p);
    p_next(p); /* ( */
    int p0 = p->i;
    int depth = 1;
    while (p->i < p->n && depth > 0) {
        Token t = p_peek(p);
        if (tok_eq(t, TK_PUNCT, "(")) depth++;
        else if (tok_eq(t, TK_PUNCT, ")")) {
            depth--;
            if (depth == 0) break;
        }
        p_next(p);
    }
    char params[4096];
    if (p0 >= p->i) params[0] = 0;
    else if (!ast_spell_token_range(p, p0, p->i, params, sizeof(params))) {
        parser_fail(p, name, "async fn params too long");
        return NULL;
    }
    if (!p_accept(p, TK_PUNCT, ")") || !p_accept(p, TK_PUNCT, "{")) {
        parser_fail(p, p_peek(p), "expected ') {' after async fn params");
        return NULL;
    }
    AstNode* n = ast_new(p, AST_ASYNC_FN);
    if (!n) return NULL;
    snprintf(n->a, sizeof(n->a), "%s", rbuf);
    slice_to(n->b, sizeof(n->b), name.spell);
    snprintf(n->c, sizeof(n->c), "%s", params);
    if (is_result) snprintf(n->e, sizeof(n->e), "result");
    /* Register before body so nested `@await name(...)` can wrap. */
    shadow_async_fn_register(n->b, n->a);
    shadow_fn_attr_register(n->b, fn_attrs | p->pending_fn_attrs, 1);
    p->pending_fn_attrs = 0;
    int body0 = p->i;
    int saved_i = p->i, saved_err = p->err, saved_nn = p->nn, saved_nk = p->nkstore;
    int saved_async = p->in_async;
    n->kids = &p->kids_storage[p->nkstore];
    int ok_stmts = 1;
    p->in_async = 1;
    while (!tok_eq(p_peek(p), TK_PUNCT, "}") && p_peek(p).kind != TK_EOF && !p->err) {
        AstNode* s = parse_stmt(p);
        if (!s) { ok_stmts = 0; break; }
        if (!ast_kids_push(p, s)) { ok_stmts = 0; break; }
        n->nkids++;
    }
    if (ok_stmts && p_accept(p, TK_PUNCT, "}")) {
        p->in_async = saved_async;
        return n;
    }

    p->i = saved_i;
    p->err = saved_err;
    p->nn = saved_nn;
    p->nkstore = saved_nk;
    p->in_async = saved_async;
    n->nkids = 0;
    n->kids = NULL;
    depth = 1;
    while (p->i < p->n && depth > 0) {
        Token t = p_peek(p);
        if (tok_eq(t, TK_PUNCT, "{")) depth++;
        else if (tok_eq(t, TK_PUNCT, "}")) {
            depth--;
            if (depth == 0) break;
        }
        p_next(p);
    }
    if (!span_text(p, body0, p->i, n->d, sizeof(n->d))) {
        parser_fail(p, name, "async fn body too long");
        return NULL;
    }
    if (!p_accept(p, TK_PUNCT, "}")) {
        parser_fail(p, p_peek(p), "expected '}' to close async fn");
        return NULL;
    }
    return n;
}

/* static [volatile] [const] Type[*] name[[N]] [= expr]; */
static AstNode* parse_static_var(Parser* p) {
    int start;
    if (shadow_kw(p_peek(p)) != SHADOW_KW_STATIC) return NULL;
    if (p->i + 2 >= p->n) return NULL;
    /* `static inline …` is always a function (parse_static_fn). */
    if (shadow_kw(p->toks[p->i + 1]) == SHADOW_KW_INLINE) return NULL;
    start = p->i;
    int has_const = 0;
    int has_volatile = 0;
    int has_atomic = 0;
    int ti = p->i + 1;
    while (ti < p->n && tok_is_cv(p->toks[ti])) {
        if (shadow_kw(p->toks[ti]) == SHADOW_KW_CONST) has_const = 1;
        else if (spell_eq(p->toks[ti].spell, "volatile")) has_volatile = 1;
        else if (spell_eq(p->toks[ti].spell, "_Atomic")) has_atomic = 1;
        ti++;
    }
    if (ti >= p->n) return NULL;
    Token ty = p->toks[ti];
    if (ty.kind != TK_IDENT && shadow_kw(ty) != SHADOW_KW_INT &&
        shadow_kw(ty) != SHADOW_KW_BOOL && shadow_kw(ty) != SHADOW_KW_CHAR &&
        shadow_kw(ty) != SHADOW_KW_SIZE_T &&
        shadow_kw(ty) != SHADOW_KW_VOID)
        return NULL;
    int ni = ti + 1;
    /* `static struct/union/enum …` — multi-token type; tape fallback owns it.
     * Otherwise we mis-bind `struct` as the type and `timespec` as the name. */
    if (ty.kind == TK_IDENT &&
        (spell_eq(ty.spell, "struct") || spell_eq(ty.spell, "union") ||
         spell_eq(ty.spell, "enum")))
        return NULL;
    /* `static long long …` is a function/type form — not a single-token var. */
    if (ty.kind == TK_IDENT && spell_eq(ty.spell, "long") && ni < p->n &&
        p->toks[ni].kind == TK_IDENT && spell_eq(p->toks[ni].spell, "long"))
        return NULL;
    /* Multi-token C types (`unsigned char`, `signed int`, …) — tape passthrough. */
    if (ty.kind == TK_IDENT &&
        (spell_eq(ty.spell, "unsigned") || spell_eq(ty.spell, "signed") ||
         spell_eq(ty.spell, "short")) &&
        ni < p->n &&
        (p->toks[ni].kind == TK_IDENT || shadow_kw(p->toks[ni]) == SHADOW_KW_INT ||
         shadow_kw(p->toks[ni]) == SHADOW_KW_CHAR))
        return NULL;
    int is_ptr = 0;
    int post_cv_const = 0;
    int post_cv_volatile = 0;
    if (ni < p->n && tok_eq(p->toks[ni], TK_PUNCT, "*")) {
        is_ptr = 1;
        ni++;
        /* `static const T* const name` / `* volatile name`. */
        while (ni < p->n && p->toks[ni].kind == TK_IDENT) {
            if (spell_eq(p->toks[ni].spell, "const")) {
                post_cv_const = 1;
                ni++;
            } else if (spell_eq(p->toks[ni].spell, "volatile")) {
                post_cv_volatile = 1;
                ni++;
            } else
                break;
        }
    }
    if (ni >= p->n || p->toks[ni].kind != TK_IDENT) return NULL;
    /* reject functions */
    if (ni + 1 < p->n && tok_eq(p->toks[ni + 1], TK_PUNCT, "(")) return NULL;

    p_next(p); /* static */
    while (p->i < ti) p_next(p); /* cv / _Atomic */
    p_next(p); /* type */
    if (is_ptr) p_next(p);
    while (post_cv_const || post_cv_volatile) {
        if (post_cv_const && spell_eq(p_peek(p).spell, "const")) {
            p_next(p);
            post_cv_const = 0;
        } else if (post_cv_volatile &&
                   spell_eq(p_peek(p).spell, "volatile")) {
            p_next(p);
            post_cv_volatile = 0;
        } else
            break;
    }
    Token name = p_next(p);
    AstNode* n = ast_new(p, AST_STATIC_VAR);
    if (!n) return NULL;
    {
        char tbuf[128];
        slice_to(tbuf, sizeof(tbuf), ty.spell);
        if (has_atomic && has_volatile && has_const)
            snprintf(n->a, sizeof(n->a), "_Atomic volatile const %s", tbuf);
        else if (has_atomic && has_volatile)
            snprintf(n->a, sizeof(n->a), "_Atomic volatile %s", tbuf);
        else if (has_atomic && has_const)
            snprintf(n->a, sizeof(n->a), "_Atomic const %s", tbuf);
        else if (has_atomic)
            snprintf(n->a, sizeof(n->a), "_Atomic %s", tbuf);
        else if (has_volatile && has_const)
            snprintf(n->a, sizeof(n->a), "volatile const %s", tbuf);
        else if (has_volatile)
            snprintf(n->a, sizeof(n->a), "volatile %s", tbuf);
        else if (has_const)
            snprintf(n->a, sizeof(n->a), "const %s", tbuf);
        else
            snprintf(n->a, sizeof(n->a), "%s", tbuf);
    }
    slice_to(n->b, sizeof(n->b), name.spell);
    spawn_note_global(n->b);
    if (is_ptr) {
        size_t la = strlen(n->a);
        if (la + 2 <= sizeof(n->a)) {
            n->a[la] = '*';
            n->a[la + 1] = 0;
        }
    }
    if (tok_eq(p_peek(p), TK_PUNCT, "[")) {
        int a0 = p->i;
        p_next(p);
        int depth = 1;
        while (p->i < p->n && depth > 0) {
            Token t = p_peek(p);
            if (tok_eq(t, TK_PUNCT, "[")) depth++;
            else if (tok_eq(t, TK_PUNCT, "]")) {
                depth--;
                if (depth == 0) break;
            }
            p_next(p);
        }
        char dims[64];
        if (!span_text(p, a0, p->i + 1, dims, sizeof(dims))) {
            parser_fail(p, name, "static array dims too long");
            return NULL;
        }
        p_next(p); /* ] */
        snprintf(n->c, sizeof(n->c), "%s", dims);
    }
    if (tok_eq(p_peek(p), TK_PUNCT, "=")) {
        p_next(p); /* = */
        int e0 = p->i;
        int depth = 0;
        while (p->i < p->n) {
            Token t = p_peek(p);
            if (tok_eq(t, TK_PUNCT, "(") || tok_eq(t, TK_PUNCT, "[") ||
                tok_eq(t, TK_PUNCT, "{"))
                depth++;
            else if (tok_eq(t, TK_PUNCT, ")") || tok_eq(t, TK_PUNCT, "]") ||
                     tok_eq(t, TK_PUNCT, "}"))
                depth--;
            else if (depth == 0 && tok_eq(t, TK_PUNCT, ";"))
                break;
            p_next(p);
        }
        /* Grammar CLI field tables can exceed AstNode.d — soft-miss so the
         * static-decl tape fallback in parse_external_inner owns the span.
         * Probe size first: ast_spell/span_text diagnose into p->err. */
        {
            size_t need = 0;
            int ti;
            for (ti = e0; ti < p->i; ti++) {
                if (ti > e0) need++;
                need += p->toks[ti].spell.len;
            }
            if (need + 1 > sizeof(n->d)) {
                p->i = start;
                return NULL;
            }
        }
        if (!ast_spell_token_range(p, e0, p->i, n->d, sizeof(n->d)) &&
            !span_text(p, e0, p->i, n->d, sizeof(n->d))) {
            p->i = start;
            p->err = 0;
            p->err_msg[0] = 0;
            return NULL;
        }
    }
    if (!p_accept(p, TK_PUNCT, ";")) {
        /* Soft-miss: caller falls back to a tape span for the declaration.
         * Do not diag — a hard fail here poisoned product builds that later
         * refuse to cache any recovering error diagnostic. */
        p->i = start;
        p->err = 0;
        p->err_msg[0] = 0;
        return NULL;
    }
    return n;
}

/* Type[*] name [[N]] [= expr]; — file-scope global (non-static). */
static AstNode* parse_global_var(Parser* p) {
    int ti = p->i;
    Token ty = p->toks[ti];
    ShadowKwKind tkw = shadow_kw(ty);
    if (ty.kind != TK_IDENT && tkw != SHADOW_KW_INT && tkw != SHADOW_KW_BOOL &&
        tkw != SHADOW_KW_CHAR && tkw != SHADOW_KW_SIZE_T)
        return NULL;
    /* Not a type keyword (`const`/`struct`/…) — those are other parsers. */
    if (tkw != SHADOW_KW_NONE && tkw != SHADOW_KW_INT && tkw != SHADOW_KW_BOOL &&
        tkw != SHADOW_KW_CHAR && tkw != SHADOW_KW_SIZE_T)
        return NULL;
    /* Bare IDENT types: local typedefs, or foreign C names from passthrough
     * includes (not spliced into the AST scope). Seed so later uses parse;
     * host cc typechecks. */
    if (tkw == SHADOW_KW_NONE && !scope_is_typedef(p, ty.spell)) {
        char tname[128];
        slice_to(tname, sizeof(tname), ty.spell);
        shadow_seed_spelled_type_name(p, tname);
    }
    int ni = ti + 1;
    int is_ptr = 0;
    if (ni < p->n && tok_eq(p->toks[ni], TK_PUNCT, "*")) {
        is_ptr = 1;
        ni++;
    }
    if (ni >= p->n || p->toks[ni].kind != TK_IDENT) return NULL;
    if (ni + 1 < p->n && tok_eq(p->toks[ni + 1], TK_PUNCT, "(")) return NULL;
    /* `Ident * Ident;` is parse_star_stmt (typedef ptr vs mul expr). */
    if (is_ptr && ni + 1 < p->n && tok_eq(p->toks[ni + 1], TK_PUNCT, ";"))
        return NULL;
    if (ni + 1 < p->n && tok_eq(p->toks[ni + 1], TK_PUNCT, "[")) {
        if (p->toks[ni + 1].kind != TK_PUNCT || !tok_eq(p->toks[ni + 1], TK_PUNCT, "["))
            return NULL;
    }
    /* Must end with `;` (optional `=` init). */
    {
        int j = ni + 1;
        int depth = 0;
        int has_semi = 0;
        while (j < p->n) {
            Token t = p->toks[j];
            if (tok_eq(t, TK_PUNCT, "(") || tok_eq(t, TK_PUNCT, "[") ||
                tok_eq(t, TK_PUNCT, "{"))
                depth++;
            else if (tok_eq(t, TK_PUNCT, ")") || tok_eq(t, TK_PUNCT, "]") ||
                     tok_eq(t, TK_PUNCT, "}"))
                depth--;
            else if (depth == 0 && tok_eq(t, TK_PUNCT, ";")) {
                has_semi = 1;
                break;
            }
            j++;
        }
        if (!has_semi) return NULL;
    }

    {
        int decl0 = ti + 1;
        int one = peek_one_declarator(p, decl0, p->n, SHADOW_DECL_INIT);
        if (one >= 0 && one < p->n && tok_eq(p->toks[one], TK_PUNCT, ",")) {
            int semi_i = peek_decl_list_semi(
                p, decl0, SHADOW_DECL_INIT | SHADOW_DECL_FAIL);
            char names[256];
            AstNode* n;
            if (semi_i < 0) return NULL;
            if (!span_text(p, decl0, semi_i, names, sizeof(names))) {
                parser_fail(p, p->toks[decl0],
                            "global multi-declarator list too long");
                return NULL;
            }
            p_next(p); /* type */
            while (p->i < semi_i) p_next(p);
            p_next(p); /* ; */
            n = ast_new(p, AST_STATIC_VAR);
            if (!n) return NULL;
            slice_to(n->a, sizeof(n->a), ty.spell);
            snprintf(n->b, sizeof(n->b), "%s", names);
            spawn_note_global(n->b);
            snprintf(n->e, sizeof(n->e), "global");
            return n;
        }
    }

    p_next(p); /* type */
    if (is_ptr) p_next(p);
    Token name = p_next(p);
    AstNode* n = ast_new(p, AST_STATIC_VAR);
    if (!n) return NULL;
    slice_to(n->a, sizeof(n->a), ty.spell);
    slice_to(n->b, sizeof(n->b), name.spell);
    spawn_note_global(n->b);
    snprintf(n->e, sizeof(n->e), "global");
    if (is_ptr) snprintf(n->c, sizeof(n->c), "*");
    if (tok_eq(p_peek(p), TK_PUNCT, "[")) {
        int a0 = p->i;
        int depth = 1;
        p_next(p); /* [ */
        while (p->i < p->n && depth > 0) {
            Token t = p_peek(p);
            if (tok_eq(t, TK_PUNCT, "[")) depth++;
            else if (tok_eq(t, TK_PUNCT, "]")) {
                depth--;
                if (depth == 0) break;
            }
            p_next(p);
        }
        char dims[64];
        if (!span_text(p, a0, p->i + 1, dims, sizeof(dims))) {
            parser_fail(p, name, "global array dims too long");
            return NULL;
        }
        p_next(p); /* ] */
        snprintf(n->c, sizeof(n->c), "%s", dims);
    }
    /* Multi-declarator: `T a, b, c;` (no per-name init beachhead). */
    while (tok_eq(p_peek(p), TK_PUNCT, ",")) {
        Token nxt;
        size_t bl;
        p_next(p); /* , */
        nxt = p_next(p);
        if (nxt.kind != TK_IDENT) {
            parser_fail(p, nxt, "expected ident after ',' in global multi-decl");
            return NULL;
        }
        bl = strlen(n->b);
        if (bl + 1 + nxt.spell.len + 1 >= sizeof(n->b)) {
            parser_fail(p, nxt, "global multi-declarator list too long");
            return NULL;
        }
        n->b[bl++] = ',';
        n->b[bl++] = ' ';
        memcpy(n->b + bl, nxt.spell.ptr, nxt.spell.len);
        n->b[bl + nxt.spell.len] = 0;
    }
    if (tok_eq(p_peek(p), TK_PUNCT, "=")) {
        p_next(p);
        int e0 = p->i;
        int depth = 0;
        while (p->i < p->n) {
            Token t = p_peek(p);
            if (tok_eq(t, TK_PUNCT, "(") || tok_eq(t, TK_PUNCT, "[") ||
                tok_eq(t, TK_PUNCT, "{"))
                depth++;
            else if (tok_eq(t, TK_PUNCT, ")") || tok_eq(t, TK_PUNCT, "]") ||
                     tok_eq(t, TK_PUNCT, "}"))
                depth--;
            else if (depth == 0 && tok_eq(t, TK_PUNCT, ";"))
                break;
            p_next(p);
        }
        if (!ast_spell_token_range(p, e0, p->i, n->d, sizeof(n->d)) &&
            !span_text(p, e0, p->i, n->d, sizeof(n->d))) {
            parser_fail(p, name, "global init too long");
            return NULL;
        }
    }
    if (!p_accept(p, TK_PUNCT, ";")) {
        parser_fail(p, p_peek(p), "expected ';' after global var");
        return NULL;
    }
    return n;
}

/* Extract cc_emit_error("…") message into dst; 1 on hit. */
static int comptime_emit_error_msg(const char* body, char* dst, size_t cap) {
    const char* p;
    const char* q;
    size_t n;
    if (!body || !dst || !cap) return 0;
    p = strstr(body, "cc_emit_error");
    if (!p) return 0;
    p += strlen("cc_emit_error");
    while (*p == ' ' || *p == '\t') p++;
    if (*p != '(') return 0;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    if (*p != '"') return 0;
    p++;
    q = p;
    while (*q && *q != '"') q++;
    if (*q != '"') return 0;
    n = (size_t)(q - p);
    if (n + 1 > cap) n = cap - 1;
    memcpy(dst, p, n);
    dst[n] = 0;
    return 1;
}

/* @ident [(...)] [Ident] [{balanced}] [;] — a=attr; b=optional type (grammar).
 * `@comptime` keeps body text in c for ebf diagnostics (executor still TBD). */
static AstNode* parse_at_stmt(Parser* p) {
    if (!tok_eq(p_peek(p), TK_PUNCT, "@")) return NULL;
    if (p->i + 1 < p->n && shadow_kw(p->toks[p->i + 1]) == SHADOW_KW_WITH_DEADLINE)
        return parse_with_deadline(p);
    if (p->i + 1 < p->n && shadow_kw(p->toks[p->i + 1]) == SHADOW_KW_PARALLEL)
        return parse_parallel(p);
    if (p->i + 1 < p->n && shadow_kw(p->toks[p->i + 1]) == SHADOW_KW_STAGE)
        return parse_stage(p);
    if (p->i + 1 < p->n && shadow_kw(p->toks[p->i + 1]) == SHADOW_KW_SERIAL) {
        parser_fail(p, p->toks[p->i + 1],
                    "@serial is only an arm of @parallel { }");
        return NULL;
    }
    /* Real `@async Ret name(...){…}` first; spike `@async {}` refuses. */
    if (p->i + 1 < p->n && shadow_kw(p->toks[p->i + 1]) == SHADOW_KW_ASYNC) {
        AstNode* af = parse_async_fn(p);
        if (af || p->err) return af;
        /* `@async {` without a signature — body would be dropped as a
         * comment; refuse rather than silent-wrong. */
        if (p->i + 2 < p->n && tok_eq(p->toks[p->i + 2], TK_PUNCT, "{")) {
            parser_fail(p, p->toks[p->i + 1],
                        "@async {} is not supported; use "
                        "@async Ret name(...) { ... }");
            return NULL;
        }
    }
    p_next(p);
    Token name = p_next(p);
    if (name.kind != TK_IDENT) { p->err = 1; return NULL; }
    /* Retired @ forms — reject with legacy migration text (safety/diag seam). */
    if (spell_eq(name.spell, "arena")) {
        parser_fail(p, name,
                    "async: `@arena(...) { ... }` is retired; use "
                    "`CCArena a@(size) @destroy`");
        return NULL;
    }
    if (spell_eq(name.spell, "arena_init")) {
        /* Line-only locus (no column) — m0.5 origin-line oracle. */
        FileTape* ft = (p->cache && name.file_id)
                           ? tape_by_id(p->cache, name.file_id)
                           : NULL;
        int line = 1, col = 1;
        if (ft && ft->bytes)
            offset_to_linecol(ft, name.offset, &line, &col);
        (void)col;
        {
            char lfile[1024];
            fprintf(stderr,
                    "%s:%d: error: async: `@arena_init(...) { ... }` is retired; "
                    "use `CCArena a@(buf, size) @destroy` or "
                    "`cc_arena_buffer(...)` directly\n",
                    ft ? tape_diag_file(ft, name.offset, lfile, sizeof(lfile))
                       : "<input>",
                    line);
        }
        p->err = 1;
        snprintf(p->err_msg, sizeof(p->err_msg), "retired @arena_init");
        return NULL;
    }
    if (spell_eq(name.spell, "closing")) {
        parser_fail(p, name,
                    "async: `@closing(...)` is retired; use explicit ownership "
                    "with `T name@(args) @destroy { chan.close(); }`");
        return NULL;
    }
    if (spell_eq(name.spell, "nursery")) {
        parser_fail(p, name,
                    "async: `@nursery { ... }` is retired; use "
                    "`frame.create_nursery()` / `parent.create_child()` and "
                    "`n.spawn(...)`");
        return NULL;
    }
    if (spell_eq(name.spell, "match")) {
        parser_fail(p, name,
                    "syntax: '@match' was removed; multiplex channels with "
                    "cc_chan_match_select(...) (see spec) or restructure with "
                    "one fiber per source");
        return NULL;
    }
    if (spell_eq(name.spell, "restricted")) {
        parser_fail(p, name,
                    "'@restricted' was removed; use '@typeview'");
        return NULL;
    }
    if (spell_eq(name.spell, "as")) {
        parser_fail(p, name,
                    "'@as' was removed; declare '@typeview on T { as: field; }'");
        return NULL;
    }
    /* `@typeview` [Mode] on Base { r:…; w:…; rw:…; as:… }
     * Allow-list views + optional is-a faces. Unnamed `@typeview on Base`
     * applies to Base itself (no parallel type). */
    if (spell_eq(name.spell, "typeview")) {
        Token mode, on, base;
        int unnamed = 0;
        int body0 = -1, body1 = -1;
        char body[1024];
        char mangled[160];
        AstNode* n;
        CCSlice ms;
        mode = p_peek(p);
        if (mode.kind == TK_IDENT && spell_eq(mode.spell, "on")) {
            unnamed = 1;
            p_next(p); /* on */
            mode.spell.ptr = "";
            mode.spell.len = 0;
        } else {
            mode = p_next(p);
            if (mode.kind != TK_IDENT) {
                parser_fail(p, mode,
                            "expected mode name or 'on' after @typeview");
                return NULL;
            }
            on = p_next(p);
            if (on.kind != TK_IDENT || !spell_eq(on.spell, "on")) {
                parser_fail(p, on, "expected 'on' after @typeview mode name");
                return NULL;
            }
        }
        base = p_next(p);
        if (base.kind != TK_IDENT) {
            parser_fail(p, base, "expected base type after @typeview … on");
            return NULL;
        }
        {
            char subject[96];
            size_t sl;
            slice_to(subject, sizeof(subject), base.spell);
            /* Trailing `*` — type-family glob (`CCSlice_*`), same as UFCS. */
            if (tok_eq(p_peek(p), TK_PUNCT, "*")) {
                p_next(p);
                sl = strlen(subject);
                if (sl + 1 < sizeof(subject)) {
                    subject[sl] = '*';
                    subject[sl + 1] = 0;
                }
            }
            if (!tok_eq(p_peek(p), TK_PUNCT, "{")) {
                parser_fail(p, p_peek(p), "expected '{' body after @typeview");
                return NULL;
            }
            body0 = p->i;
            p_next(p);
            {
                int depth = 1;
                while (p->i < p->n && depth > 0) {
                    Token t = p_next(p);
                    if (tok_eq(t, TK_PUNCT, "{")) depth++;
                    else if (tok_eq(t, TK_PUNCT, "}")) depth--;
                }
            }
            body1 = p->i;
            p_accept(p, TK_PUNCT, ";");
            body[0] = 0;
            if (body0 >= 0 && body1 > body0) {
                if (!ast_spell_token_range(p, body0, body1, body, sizeof(body)) &&
                    !span_text(p, body0, body1, body, sizeof(body))) {
                    parser_fail(p, base, "@typeview body too long");
                    return NULL;
                }
            }
            n = ast_new(p, AST_AT_STMT);
            if (!n) return NULL;
            snprintf(n->a, sizeof(n->a), "typeview");
            if (unnamed) n->b[0] = 0;
            else slice_to(n->b, sizeof(n->b), mode.spell);
            snprintf(n->c, sizeof(n->c), "%s", body);
            snprintf(n->d, sizeof(n->d), "%s", subject);
            if (unnamed) {
                /* Keyed on Base itself — no parallel typedef. */
                snprintf(n->e, sizeof(n->e), "%s", subject);
            } else {
                if (strchr(subject, '*')) {
                    parser_fail(p, base,
                                "named @typeview mode cannot use a glob "
                                "subject");
                    return NULL;
                }
                snprintf(mangled, sizeof(mangled), "%s_Restrict_%.*s", subject,
                         (int)mode.spell.len, mode.spell.ptr);
                snprintf(n->e, sizeof(n->e), "%s", mangled);
                ms.ptr = mangled;
                ms.len = strlen(mangled);
                (void)scope_add_typedef(p, ms);
            }
            return n;
        }
    }
    /* `@typehooks on Subject[*]? { .destroy = …, };` — consume `on T {`
     * so a leftover raw form is not parsed as a global `on T`. */
    if (spell_eq(name.spell, "typehooks")) {
        Token on, base;
        int body0 = -1, body1 = -1;
        char body[1024];
        char subject[96];
        AstNode* n;
        on = p_next(p);
        if (on.kind != TK_IDENT || !spell_eq(on.spell, "on")) {
            parser_fail(p, on, "expected 'on' after @typehooks");
            return NULL;
        }
        base = p_next(p);
        if (base.kind != TK_IDENT) {
            parser_fail(p, base, "expected type after @typehooks on");
            return NULL;
        }
        slice_to(subject, sizeof(subject), base.spell);
        if (tok_eq(p_peek(p), TK_PUNCT, "*")) {
            p_next(p);
            {
                size_t sl = strlen(subject);
                if (sl + 1 < sizeof(subject)) {
                    subject[sl] = '*';
                    subject[sl + 1] = 0;
                }
            }
        }
        if (!tok_eq(p_peek(p), TK_PUNCT, "{")) {
            parser_fail(p, p_peek(p), "expected '{' body after @typehooks");
            return NULL;
        }
        body0 = p->i;
        p_next(p);
        {
            int depth = 1;
            while (p->i < p->n && depth > 0) {
                Token t = p_next(p);
                if (tok_eq(t, TK_PUNCT, "{")) depth++;
                else if (tok_eq(t, TK_PUNCT, "}")) depth--;
            }
        }
        body1 = p->i;
        p_accept(p, TK_PUNCT, ";");
        body[0] = 0;
        if (body0 >= 0 && body1 > body0) {
            if (!ast_spell_token_range(p, body0, body1, body, sizeof(body)) &&
                !span_text(p, body0, body1, body, sizeof(body))) {
                parser_fail(p, base, "@typehooks body too long");
                return NULL;
            }
        }
        n = ast_new(p, AST_AT_STMT);
        if (!n) return NULL;
        snprintf(n->a, sizeof(n->a), "typehooks");
        snprintf(n->c, sizeof(n->c), "%s", body);
        snprintf(n->d, sizeof(n->d), "%s", subject);
        return n;
    }
    /* `@await expr;` — lower via ast_spell (block_on / channel_*_task). */
    if (spell_eq(name.spell, "await")) {
        int e0 = p->i;
        int depth = 0;
        char expr[320];
        AstNode* n;
        while (p->i < p->n) {
            Token t = p_peek(p);
            if (tok_eq(t, TK_PUNCT, "(") || tok_eq(t, TK_PUNCT, "[") ||
                tok_eq(t, TK_PUNCT, "{"))
                depth++;
            else if (tok_eq(t, TK_PUNCT, ")") || tok_eq(t, TK_PUNCT, "]") ||
                     tok_eq(t, TK_PUNCT, "}")) {
                if (depth > 0) depth--;
            } else if (depth == 0 && tok_eq(t, TK_PUNCT, ";"))
                break;
            p_next(p);
        }
        if (e0 >= p->i) {
            parser_fail(p, name, "expected expression after @await");
            return NULL;
        }
        /* Spell from `@await` so channel UFCS becomes block_on(*_task). */
        if (!ast_spell_token_range(p, e0 - 2, p->i, expr, sizeof(expr))) {
            parser_fail(p, name, "@await expression too long");
            return NULL;
        }
        if (!p_accept(p, TK_PUNCT, ";")) {
            parser_fail(p, p_peek(p), "expected ';' after @await");
            return NULL;
        }
        n = ast_new(p, AST_AT_STMT);
        if (!n) return NULL;
        snprintf(n->a, sizeof(n->a), "await");
        snprintf(n->c, sizeof(n->c), "%s", expr);
        return n;
    }
    /* `@comptime if (pred) {…}` — leftover after prepare. Product prepare owns
     * folding/pruning; stage1 normally sees taken bodies only. Reject any
     * non-literal predicate that still reaches the whitelist parser. */
    if (spell_eq(name.spell, "comptime") &&
        shadow_kw(p_peek(p)) == SHADOW_KW_IF) {
        Token ift = p_next(p); /* if */
        int p0, p1;
        char pred[256];
        if (!tok_eq(p_peek(p), TK_PUNCT, "(")) {
            parser_fail(p, ift, "expected '(' after @comptime if");
            return NULL;
        }
        p0 = p->i + 1;
        if (!skip_parens(p)) {
            parser_fail(p, ift, "unterminated @comptime if predicate");
            return NULL;
        }
        p1 = p->i - 1;
        if (p0 < p1)
            (void)ast_spell_token_range(p, p0, p1, pred, sizeof(pred));
        else
            pred[0] = 0;
        /* Skip then/else blocks (balanced). */
        if (tok_eq(p_peek(p), TK_PUNCT, "{")) {
            int depth = 0;
            p_next(p);
            depth = 1;
            while (p->i < p->n && depth > 0) {
                Token t = p_next(p);
                if (tok_eq(t, TK_PUNCT, "{")) depth++;
                else if (tok_eq(t, TK_PUNCT, "}")) depth--;
            }
        }
        if (shadow_kw(p_peek(p)) == SHADOW_KW_ELSE) {
            p_next(p);
            if (tok_eq(p_peek(p), TK_PUNCT, "{")) {
                int depth = 1;
                p_next(p);
                while (p->i < p->n && depth > 0) {
                    Token t = p_next(p);
                    if (tok_eq(t, TK_PUNCT, "{")) depth++;
                    else if (tok_eq(t, TK_PUNCT, "}")) depth--;
                }
            }
        }
        /* Const beachhead: decimal/hex literal or true/false only. */
        {
            const char* s = pred;
            int ok = 0;
            while (*s == ' ' || *s == '\t') s++;
            if (strcmp(s, "true") == 0 || strcmp(s, "false") == 0 ||
                strcmp(s, "0") == 0 || strcmp(s, "1") == 0)
                ok = 1;
            else if (*s == '-' || (*s >= '0' && *s <= '9')) {
                const char* e = s;
                if (*e == '-') e++;
                if (e[0] == '0' && (e[1] == 'x' || e[1] == 'X')) {
                    e += 2;
                    while ((*e >= '0' && *e <= '9') || (*e >= 'a' && *e <= 'f') ||
                           (*e >= 'A' && *e <= 'F'))
                        e++;
                } else {
                    while (*e >= '0' && *e <= '9') e++;
                }
                while (*e == ' ' || *e == '\t') e++;
                ok = (*e == 0);
            }
            if (!ok) {
                parser_fail(p, ift,
                            "is not a compile-time constant the compiler can "
                            "decide");
                return NULL;
            }
        }
        AstNode* n = ast_new(p, AST_AT_STMT);
        if (!n) return NULL;
        snprintf(n->a, sizeof(n->a), "comptime");
        snprintf(n->b, sizeof(n->b), "if");
        snprintf(n->c, sizeof(n->c), "%s", pred);
        return n;
    }
    {
        int variant_packed = 0;
        if (tok_eq(p_peek(p), TK_PUNCT, "(")) {
            if (p->i + 1 < p->n &&
                spell_eq(p->toks[p->i + 1].spell, "packed"))
                variant_packed = 1;
            if (!skip_parens(p)) { p->err = 1; return NULL; }
        }
        Token typ = {0};
        if (p_peek(p).kind == TK_IDENT &&
            p->i + 1 < p->n && tok_eq(p->toks[p->i + 1], TK_PUNCT, "{"))
            typ = p_next(p);
        {
            int body0 = -1, body1 = -1;
            if (tok_eq(p_peek(p), TK_PUNCT, "{")) {
                body0 = p->i;
                p_next(p);
                int depth = 1;
                while (p->i < p->n && depth > 0) {
                    Token t = p_next(p);
                    if (tok_eq(t, TK_PUNCT, "{")) depth++;
                    else if (tok_eq(t, TK_PUNCT, "}")) depth--;
                }
                body1 = p->i; /* exclusive end after closing `}` */
            }
            p_accept(p, TK_PUNCT, ";");
            AstNode* n = ast_new(p, AST_AT_STMT);
            if (!n) return NULL;
            slice_to(n->a, sizeof(n->a), name.spell);
            if (typ.kind == TK_IDENT) slice_to(n->b, sizeof(n->b), typ.spell);
            if (variant_packed) snprintf(n->d, sizeof(n->d), "packed");
            /* `@grammar(…) Name` / `@variant Name` introduce a type. */
            if (typ.kind == TK_IDENT &&
                (spell_eq(name.spell, "grammar") ||
                 spell_eq(name.spell, "variant"))) {
                (void)scope_add_typedef(p, typ.spell);
                if (spell_eq(name.spell, "variant")) {
                    /* Emit also defines `NameKind` enum typedef. */
                    char kbuf[80];
                    CCSlice ks;
                    size_t nl = typ.spell.len;
                    if (nl + 4 < sizeof(kbuf)) {
                        memcpy(kbuf, typ.spell.ptr, nl);
                        memcpy(kbuf + nl, "Kind", 4);
                        kbuf[nl + 4] = 0;
                        ks.ptr = kbuf;
                        ks.len = nl + 4;
                        (void)scope_add_typedef(p, ks);
                    }
                }
            }
            /* `@variant Name { arm: Type; … };` — tape span; fail-loud if
             * the body does not fit `c[]` (silent truncate was wrong). */
            if (spell_eq(name.spell, "variant") && body0 >= 0 && body1 > body0) {
                char* body = span_cstr(p, body0, body1);
                (void)span_bind(p, body0, body1, n);
                if (body && strlen(body) + 1 <= sizeof(n->c))
                    snprintf(n->c, sizeof(n->c), "%s", body);
                else if (body) {
                    parser_fail(p, name,
                                "@variant body too large for internal buffer");
                    return NULL;
                }
            }
            if (spell_eq(name.spell, "comptime") && body0 >= 0 && body1 > body0) {
                char* body = span_cstr(p, body0, body1);
                (void)span_bind(p, body0, body1, n);
                if (body) {
                    char emsg[256];
                    if (comptime_emit_error_msg(body, emsg, sizeof(emsg))) {
                        FileTape* ft = (p->cache && name.file_id)
                                           ? tape_by_id(p->cache, name.file_id)
                                           : NULL;
                        int line = 1, col = 1;
                        if (ft && ft->bytes)
                            offset_to_linecol(ft, name.offset, &line, &col);
                        {
                            char lfile[1024];
                            fprintf(stderr, "%s:%d: error: %s\n",
                                    ft ? tape_diag_file(ft, name.offset, lfile,
                                                        sizeof(lfile))
                                       : "<input>",
                                    line,
                                    emsg[0] ? emsg : "comptime constraint violated");
                        }
                        p->err = 1;
                        return NULL;
                    }
                    {
                        const char* ep = body;
                        while ((ep = strstr(ep, "@emit")) != NULL) {
                            const char* b = ep;
                            char before;
                            int is_stmt;
                            const char* ar;
                            const char* q;
                            int has_anchor = 0;
                            while (b > body &&
                                   (b[-1] == ' ' || b[-1] == '\t' || b[-1] == '\n' ||
                                    b[-1] == '\r'))
                                b--;
                            before = (b > body) ? b[-1] : '{';
                            is_stmt = !(before == '(' || before == ',' ||
                                        before == '=' || before == '[');
                            if (b - body >= 6 && memcmp(b - 6, "return", 6) == 0)
                                is_stmt = 0;
                            ar = strstr(ep, ", arena");
                            {
                                const char* nxt = strstr(ep + 5, "@emit");
                                if (ar && nxt && ar > nxt) ar = NULL;
                            }
                            q = ep + 5;
                            while (*q == ' ' || *q == '\t') q++;
                            if (*q == '(') {
                                q++;
                                while (*q == ' ' || *q == '\t') q++;
                                if (strncmp(q, "CC_EMIT_", 8) == 0) has_anchor = 1;
                            }
                            if (is_stmt && ar) {
                                parser_fail(p, name, "discarding it emits nothing");
                                return NULL;
                            }
                            if (is_stmt && !ar && !has_anchor) {
                                parser_fail(p, name, "requires an arena");
                                return NULL;
                            }
                            ep += 5;
                        }
                    }
                    if (strlen(body) + 1 <= sizeof(n->c))
                        snprintf(n->c, sizeof(n->c), "%s", body);
                } else {
                    return NULL;
                }
            }
            return n;
        }
    }
}

/* int Name ; */
static AstNode* parse_int_decl(Parser* p, AstKind kind) {
    if (shadow_kw(p_peek(p)) != SHADOW_KW_INT) return NULL;
    /* CC shapes take precedence over plain int Name */
    if (p->i + 1 < p->n && tok_eq(p->toks[p->i + 1], TK_PUNCT, "[")) return NULL;
    if (p->i + 1 < p->n && tok_eq(p->toks[p->i + 1], TK_PUNCT, "!>")) return NULL;
    /* `int x, y;` belongs to parse_field_simple — don't steal / set err. */
    if (p->i + 2 >= p->n || p->toks[p->i + 1].kind != TK_IDENT) return NULL;
    if (!tok_eq(p->toks[p->i + 2], TK_PUNCT, ";")) return NULL;
    p_next(p);
    Token name = p_next(p);
    if (!p_accept(p, TK_PUNCT, ";")) { p->err = 1; return NULL; }
    AstNode* n = ast_new(p, kind);
    if (!n) return NULL;
    slice_to(n->a, sizeof(n->a), name.spell);
    return n;
}

/* True when source gap between name and `;` carries the lowered host-C
 * comment marker for an as: face (stage-1 skips comments). */
static int field_gap_has_as(Parser* p, Token name, Token semi) {
    FileTape* ft;
    size_t a, b, i;
    if (!p || !p->cache) return 0;
    ft = tape_by_id(p->cache, name.file_id);
    if (!ft || !ft->bytes) return 0;
    a = name.offset + name.spell.len;
    b = semi.offset;
    if (b > ft->len) b = ft->len;
    if (a >= b) return 0;
    for (i = a; i < b; i++) {
        if (i + 7 <= b && memcmp(ft->bytes + i, "/*@as*/", 7) == 0) return 1;
    }
    return 0;
}

/* [const|volatile|_Atomic] [struct] Type[*] name [, name]* [@as] ; —
 * beachhead struct field. Comment-form `@as` is recovered from the tape
 * gap (stage-1 skips comments). */
static AstNode* parse_field_simple(Parser* p) {
    int has_const = 0;
    int has_volatile = 0;
    int has_atomic = 0;
    int has_struct = 0;
    int is_ptr = 0;
    int has_as = 0;
    int is_char_slice = 0;
    int char_slice_unique = 0;
    int ti;
    Token ty;
    int ty_end;
    int name_i;
    int j;
    char tytxt[128];
    char names[128];
    Token last_name;
    Token semi;
    AstNode* n;

    ti = p->i;
    while (ti < p->n) {
        if (shadow_kw(p->toks[ti]) == SHADOW_KW_CONST) {
            has_const = 1;
            ti++;
            continue;
        }
        if (p->toks[ti].kind == TK_IDENT &&
            spell_eq(p->toks[ti].spell, "volatile")) {
            has_volatile = 1;
            ti++;
            continue;
        }
        if (p->toks[ti].kind == TK_IDENT &&
            spell_eq(p->toks[ti].spell, "_Atomic")) {
            has_atomic = 1;
            ti++;
            continue;
        }
        break;
    }
    if (ti < p->n && shadow_kw(p->toks[ti]) == SHADOW_KW_STRUCT) {
        has_struct = 1;
        ti++;
    }
    if (ti >= p->n) return NULL;
    ty = p->toks[ti];
    if (has_struct) {
        if (ty.kind != TK_IDENT) return NULL;
    } else if (ty.kind != TK_IDENT && shadow_kw(ty) != SHADOW_KW_INT &&
               shadow_kw(ty) != SHADOW_KW_CHAR && shadow_kw(ty) != SHADOW_KW_VOID &&
               shadow_kw(ty) != SHADOW_KW_BOOL && shadow_kw(ty) != SHADOW_KW_SIZE_T)
        return NULL;
    ty_end = has_struct ? 0 : peek_generic_type_end(p, ti);
    if (!has_struct && ty_end < 0) {
        int iend = peek_c_int_type_end(p, ti);
        if (iend > ti) ty_end = iend;
    }
    /* `char[:]` / `char[:0]` / `char[:!]` / `char[::]` → CCSlice. */
    if (!has_struct && ty_end < 0 && shadow_kw(ty) == SHADOW_KW_CHAR &&
        ti + 1 < p->n && tok_eq(p->toks[ti + 1], TK_PUNCT, "[")) {
        int mend = peek_slice_brack_end(p, ti + 1, p->n, &char_slice_unique);
        if (mend > ti + 1) {
            is_char_slice = 1;
            ty_end = mend;
        }
    }
    name_i = (ty_end > 0) ? ty_end : ti + 1;
    /* Soft-miss fn-ptrs / bitfields (`int (*cb)(…)`, `int x:3`) so the
     * struct-field tape fallback owns them. `int x y` fails at `y`. */
    {
        int one = peek_one_declarator(p, name_i, p->n, 0);
        if (one < 0) return NULL;
        if (one < p->n && tok_eq(p->toks[one], TK_PUNCT, "@") &&
            one + 1 < p->n && shadow_kw(p->toks[one + 1]) == SHADOW_KW_AS) {
            parser_fail(p, p->toks[one],
                        "'@as' was removed; declare '@typeview on T { as: field; }'");
            return NULL;
        }
        if (one < p->n && tok_eq(p->toks[one], TK_PUNCT, ",")) {
            j = peek_decl_list_semi(p, name_i, SHADOW_DECL_FAIL);
            if (j < 0) return NULL;
        } else if (one < p->n && tok_eq(p->toks[one], TK_PUNCT, ";"))
            j = one;
        else if (one < p->n && p->toks[one].kind == TK_IDENT) {
            parser_fail(p, p->toks[one], "expected ',' or ';' in field");
            return NULL;
        } else
            return NULL;
    }
    semi = p->toks[j];

    while (p->i < ti) p_next(p); /* cv / _Atomic [/ struct] */
    if (is_char_slice) {
        snprintf(tytxt, sizeof(tytxt), "%s",
                 char_slice_unique ? "CCSliceUnique" : "CCSlice");
        while (p->i < ty_end) p_next(p);
    } else if (ty_end > 0 && !has_struct) {
        if (!ast_spell_type_tokens(p, ti, ty_end, tytxt, sizeof(tytxt)) &&
            !p->err &&
            !ast_spell_token_range(p, ti, ty_end, tytxt, sizeof(tytxt)))
            return NULL;
        while (p->i < ty_end) p_next(p);
    } else {
        p_next(p); /* type / tag */
        if (has_struct)
            snprintf(tytxt, sizeof(tytxt), "struct %.*s",
                     (int)ty.spell.len, ty.spell.ptr);
        else
            slice_to(tytxt, sizeof(tytxt), ty.spell);
    }
    {
        char tmp[128];
        char pref[64];
        size_t pi = 0;
        pref[0] = 0;
        if (has_atomic) {
            memcpy(pref + pi, "_Atomic ", 8);
            pi += 8;
        }
        if (has_const) {
            memcpy(pref + pi, "const ", 6);
            pi += 6;
        }
        if (has_volatile) {
            memcpy(pref + pi, "volatile ", 9);
            pi += 9;
        }
        pref[pi] = 0;
        if (pref[0]) {
            snprintf(tmp, sizeof(tmp), "%s%s", pref, tytxt);
            snprintf(tytxt, sizeof(tytxt), "%s", tmp);
        }
    }
    if (!span_text(p, name_i, j, names, sizeof(names))) {
        parser_fail(p, p->toks[name_i], "field declarator list too long");
        return NULL;
    }
    last_name = decl_list_last_name(p, name_i, j);
    while (p->i < j) p_next(p);
    p_next(p); /* ; */
    if (strchr(names, '*')) is_ptr = 1;
    if (!has_as && last_name.kind == TK_IDENT)
        has_as = field_gap_has_as(p, last_name, semi);
    /* as: faces and lowered comment-form markers are value embeds — pointers
     * are ill-formed. */
    if (has_as && (is_ptr || strchr(tytxt, '*') != NULL ||
                   strchr(names, '*') != NULL)) {
        parser_fail(p, last_name.kind == TK_IDENT ? last_name : ty,
                    "as: field must be a value embed, not a pointer");
        return NULL;
    }
    n = ast_new(p, AST_FIELD_SIMPLE);
    if (!n) return NULL;
    snprintf(n->a, sizeof(n->a), "%s", tytxt);
    snprintf(n->b, sizeof(n->b), "%s", names);
    if (has_as) snprintf(n->e, sizeof(n->e), "as");
    return n;
}

/* typedef enum [Tag]? { enumerators } Alias ; */
static AstNode* parse_typedef_enum(Parser* p) {
    if (shadow_kw(p_peek(p)) != SHADOW_KW_TYPEDEF) return NULL;
    if (p->i + 1 >= p->n || shadow_kw(p->toks[p->i + 1]) != SHADOW_KW_ENUM)
        return NULL;
    p_next(p); /* typedef */
    p_next(p); /* enum */
    AstNode* n = ast_new(p, AST_TYPEDEF_ENUM);
    if (!n) return NULL;
    n->a[0] = 0;
    if (p_peek(p).kind == TK_IDENT) {
        Token tag = p_next(p);
        slice_to(n->a, sizeof(n->a), tag.spell);
    }
    if (!p_accept(p, TK_PUNCT, "{")) {
        parser_fail(p, p_peek(p), "expected '{' after typedef enum");
        return NULL;
    }
    int b0 = p->i;
    int depth = 1;
    while (p->i < p->n && depth > 0) {
        Token t = p_peek(p);
        if (tok_eq(t, TK_PUNCT, "{")) depth++;
        else if (tok_eq(t, TK_PUNCT, "}")) {
            depth--;
            if (depth == 0) break;
        }
        p_next(p);
    }
    if (!span_text(p, b0, p->i, n->d, sizeof(n->d))) {
        parser_fail(p, p_peek(p), "enum body too long");
        return NULL;
    }
    if (!p_accept(p, TK_PUNCT, "}")) {
        parser_fail(p, p_peek(p), "expected '}' after enum body");
        return NULL;
    }
    Token alias = p_next(p);
    if (alias.kind != TK_IDENT) {
        parser_fail(p, alias, "expected typedef enum alias");
        return NULL;
    }
    slice_to(n->b, sizeof(n->b), alias.spell);
    if (!p_accept(p, TK_PUNCT, ";")) {
        parser_fail(p, p_peek(p), "expected ';' after typedef enum");
        return NULL;
    }
    if (!scope_add_typedef(p, alias.spell)) { p->err = 1; return NULL; }
    return n;
}

/* switch (expr) { body } — prefer structured stmts (unwrap/case); opaque
 * text fallback for `@string` / tick-heavy bodies. */
static AstNode* parse_switch_stmt(Parser* p) {
    if (shadow_kw(p_peek(p)) != SHADOW_KW_SWITCH) return NULL;
    p_next(p); /* switch */
    if (!p_accept(p, TK_PUNCT, "(")) {
        parser_fail(p, p_peek(p), "expected '(' after switch");
        return NULL;
    }
    int e0 = p->i;
    int depth = 1;
    while (p->i < p->n && depth > 0) {
        Token t = p_peek(p);
        if (tok_eq(t, TK_PUNCT, "(")) depth++;
        else if (tok_eq(t, TK_PUNCT, ")")) {
            depth--;
            if (depth == 0) break;
        }
        p_next(p);
    }
    char expr[256];
    if (e0 >= p->i) expr[0] = 0;
    else if (!span_text(p, e0, p->i, expr, sizeof(expr))) {
        parser_fail(p, p_peek(p), "switch expr too long");
        return NULL;
    }
    if (!p_accept(p, TK_PUNCT, ")") || !p_accept(p, TK_PUNCT, "{")) {
        parser_fail(p, p_peek(p), "expected ') {' after switch");
        return NULL;
    }
    /* Structured beachhead only when the body needs stmt AST for !> / ?>.
     * Plain C switches (incl. `#if` case arms in cc_io_error) stay opaque
     * so preprocessor lines and `case X: stmt; break;` survive intact. */
    {
        int want_structured = 0;
        FileTape* ft0 = (p->cache && p_peek(p).file_id)
                            ? tape_by_id(p->cache, p_peek(p).file_id)
                            : NULL;
        if (ft0 && ft0->bytes) {
            size_t off = (size_t)p_peek(p).offset;
            size_t blen = ft0->len;
            const char* bytes = ft0->bytes;
            int d = 1;
            int in_bt = 0, in_dq = 0, in_sq = 0;
            while (off < blen && d > 0) {
                char c = bytes[off];
                if (in_bt) {
                    if (c == '\\' && off + 1 < blen) {
                        off += 2;
                        continue;
                    }
                    if (c == '`') in_bt = 0;
                    off++;
                    continue;
                }
                if (in_dq) {
                    if (c == '\\' && off + 1 < blen) {
                        off += 2;
                        continue;
                    }
                    if (c == '"') in_dq = 0;
                    off++;
                    continue;
                }
                if (in_sq) {
                    if (c == '\\' && off + 1 < blen) {
                        off += 2;
                        continue;
                    }
                    if (c == '\'') in_sq = 0;
                    off++;
                    continue;
                }
                if (c == '`') {
                    in_bt = 1;
                    off++;
                    continue;
                }
                if (c == '"') {
                    in_dq = 1;
                    off++;
                    continue;
                }
                if (c == '\'') {
                    in_sq = 1;
                    off++;
                    continue;
                }
                if (c == '{') d++;
                else if (c == '}') {
                    d--;
                    if (d == 0) break;
                } else if (c == '!' && off + 1 < blen && bytes[off + 1] == '>') {
                    want_structured = 1;
                    break;
                } else if (c == '?' && off + 1 < blen && bytes[off + 1] == '>') {
                    want_structured = 1;
                    break;
                } else if (c == '@' && off + 6 < blen &&
                           memcmp(bytes + off, "@defer", 6) == 0 &&
                           !((bytes[off + 6] >= 'A' && bytes[off + 6] <= 'Z') ||
                             (bytes[off + 6] >= 'a' && bytes[off + 6] <= 'z') ||
                             (bytes[off + 6] >= '0' && bytes[off + 6] <= '9') ||
                             bytes[off + 6] == '_')) {
                    /* @defer needs compound_body + binds; opaque text rewrite
                     * cannot resolve locals / restricted UFCS in the arm. */
                    want_structured = 1;
                    break;
                } else if (c == '@' && off + 8 < blen &&
                           memcmp(bytes + off, "@destroy", 8) == 0 &&
                           !((bytes[off + 8] >= 'A' && bytes[off + 8] <= 'Z') ||
                             (bytes[off + 8] >= 'a' && bytes[off + 8] <= 'z') ||
                             (bytes[off + 8] >= '0' && bytes[off + 8] <= '9') ||
                             bytes[off + 8] == '_')) {
                    want_structured = 1;
                    break;
                } else if (c == 'c' && off + 5 < blen &&
                           memcmp(bytes + off, "case", 4) == 0 &&
                           (off == 0 ||
                            !(((bytes[off - 1] >= 'A' && bytes[off - 1] <= 'Z') ||
                               (bytes[off - 1] >= 'a' && bytes[off - 1] <= 'z') ||
                               (bytes[off - 1] >= '0' && bytes[off - 1] <= '9') ||
                               bytes[off - 1] == '_'))) &&
                           !((bytes[off + 4] >= 'A' && bytes[off + 4] <= 'Z') ||
                             (bytes[off + 4] >= 'a' && bytes[off + 4] <= 'z') ||
                             (bytes[off + 4] >= '0' && bytes[off + 4] <= '9') ||
                             bytes[off + 4] == '_')) {
                    /* String-literal cases need stmt AST so UFCS binds
                     * (`CCShardMap* sh` in `case "DEL": { … }`) resolve.
                     * Plain `case 1:` stays opaque unless !> / ?> appears. */
                    size_t j = off + 4;
                    while (j < blen &&
                           (bytes[j] == ' ' || bytes[j] == '\t' ||
                            bytes[j] == '\n' || bytes[j] == '\r'))
                        j++;
                    if (j < blen && bytes[j] == '"') {
                        want_structured = 1;
                        break;
                    }
                }
                off++;
            }
        }
        if (!want_structured) goto switch_opaque_body;
        int body0 = p->i;
        int saved_soft = p->soft_stmt;
        int saved_err = p->err;
        AstNode* n = ast_new(p, AST_SWITCH);
        int ok = 1;
        if (!n) return NULL;
        snprintf(n->a, sizeof(n->a), "%s", expr);
        n->d[0] = 0;
        /* body[] stays on-node — kids_storage aliases the enclosing fn body
         * list (same nkstore cursor), which once emitted a bare `case 0:`.
         * Fat !> switches that exceed SHADOW_BODY_CAP fall back to opaque
         * tape span below (bang-rewrite on emit). */
        p->soft_stmt = 1;
        p->err = 0;
        while (!tok_eq(p_peek(p), TK_PUNCT, "}") &&
               p_peek(p).kind != TK_EOF && ok && !p->err) {
            Token t = p_peek(p);
            if (t.kind == TK_IDENT &&
                (spell_eq(t.spell, "case") || spell_eq(t.spell, "default"))) {
                int l0 = p->i;
                int d2 = 0;
                while (p->i < p->n) {
                    Token cur = p_peek(p);
                    if (tok_eq(cur, TK_PUNCT, "(") || tok_eq(cur, TK_PUNCT, "[") ||
                        tok_eq(cur, TK_PUNCT, "{"))
                        d2++;
                    else if (tok_eq(cur, TK_PUNCT, ")") ||
                             tok_eq(cur, TK_PUNCT, "]") ||
                             tok_eq(cur, TK_PUNCT, "}")) {
                        if (d2 > 0) d2--;
                    } else if (d2 == 0 && tok_eq(cur, TK_PUNCT, ":")) {
                        break;
                    }
                    p_next(p);
                }
                if (!tok_eq(p_peek(p), TK_PUNCT, ":")) {
                    ok = 0;
                    break;
                }
                p_next(p); /* : */
                {
                    AstNode* lab = ast_new(p, AST_RAW_LINE);
                    if (!lab ||
                        (!ast_spell_token_range(p, l0, p->i, lab->a,
                                                sizeof(lab->a)) &&
                         !span_text(p, l0, p->i, lab->a, sizeof(lab->a)))) {
                        ok = 0;
                        break;
                    }
                    /* Variant `case .arm` needs opaque body + kinded subject. */
                    if (strstr(lab->a, "case .") != NULL) {
                        ok = 0;
                        break;
                    }
                    if (n->nbody >= SHADOW_BODY_CAP) {
                        ok = 0;
                        break;
                    }
                    n->body[n->nbody++] = lab;
                }
                continue;
            }
            {
                AstNode* s = parse_stmt(p);
                if (!s || p->err) {
                    ok = 0;
                    break;
                }
                if (n->nbody >= SHADOW_BODY_CAP) {
                    ok = 0;
                    break;
                }
                n->body[n->nbody++] = s;
            }
        }
        p->soft_stmt = saved_soft;
        if (ok && tok_eq(p_peek(p), TK_PUNCT, "}") && !p->err) {
            p_next(p); /* } */
            p->err = saved_err;
            return n;
        }
        /* Structured miss — rewind and take opaque tape body. */
        p->i = body0;
        p->err = 0;
        n->nbody = 0;
    }
switch_opaque_body:
    /* Char-level brace match so `${…}` inside `@string(\`…\`)` cannot close
     * the switch (token scan sees `{`/`}` as punct even inside ticks). */
    {
        FileTape* ft = (p->cache && p_peek(p).file_id)
                           ? tape_by_id(p->cache, p_peek(p).file_id)
                           : NULL;
        const char* bytes = ft && ft->bytes ? ft->bytes : NULL;
        size_t blen = ft ? ft->len : 0;
        size_t off = (size_t)p_peek(p).offset;
        size_t start = off;
        int d = 1;
        int in_bt = 0, in_dq = 0, in_sq = 0;
        if (!bytes || off >= blen) {
            parser_fail(p, p_peek(p), "switch body tape missing");
            return NULL;
        }
        /* peeks at `{` already accepted — body starts here; depth 1 for it.
         * Escape tracking must consume `\\` pairs (prev-char checks break on
         * `case '\\':` in json_codec). */
        while (off < blen && d > 0) {
            char c = bytes[off];
            if (in_bt) {
                if (c == '\\' && off + 1 < blen) {
                    off += 2;
                    continue;
                }
                if (c == '`') in_bt = 0;
                off++;
                continue;
            }
            if (in_dq) {
                if (c == '\\' && off + 1 < blen) {
                    off += 2;
                    continue;
                }
                if (c == '"') in_dq = 0;
                off++;
                continue;
            }
            if (in_sq) {
                if (c == '\\' && off + 1 < blen) {
                    off += 2;
                    continue;
                }
                if (c == '\'') in_sq = 0;
                off++;
                continue;
            }
            if (c == '`') {
                in_bt = 1;
                off++;
                continue;
            }
            if (c == '"') {
                in_dq = 1;
                off++;
                continue;
            }
            if (c == '\'') {
                in_sq = 1;
                off++;
                continue;
            }
            if (c == '{') d++;
            else if (c == '}') {
                d--;
                if (d == 0) break;
            }
            off++;
        }
        if (d != 0) {
            parser_fail(p, p_peek(p), "unterminated switch body");
            return NULL;
        }
        {
            size_t nlen = off - start;
            AstNode* n = ast_new(p, AST_SWITCH);
            if (!n) return NULL;
            snprintf(n->a, sizeof(n->a), "%s", expr);
            n->file_id = p_peek(p).file_id;
            if (nlen + 1 <= sizeof(n->d)) {
                memcpy(n->d, bytes + start, nlen);
                n->d[nlen] = 0;
                n->span_off = 0;
                n->span_len = 0;
            } else {
                /* Keep body on the tape — d[4096] silent NULL used to leave
                 * enclosing Result fns with raw `!>` in the emit product. */
                n->d[0] = 0;
                n->span_off = start;
                n->span_len = nlen;
            }
            /* Advance token cursor to the closing `}`. */
            while (p->i < p->n) {
                Token t = p_peek(p);
                size_t toff = (size_t)t.offset;
                if (toff >= off) break;
                p_next(p);
            }
            if (!p_accept(p, TK_PUNCT, "}")) {
                parser_fail(p, p_peek(p), "expected '}' to close switch");
                return NULL;
            }
            return n;
        }
    }
}

/* typedef struct [Tag] { fields } Alias ;
 * Opaque: typedef struct Tag Alias ; */
static AstNode* parse_typedef_struct(Parser* p) {
    if (shadow_kw(p_peek(p)) != SHADOW_KW_TYPEDEF) return NULL;
    if (p->i + 1 >= p->n || shadow_kw(p->toks[p->i + 1]) != SHADOW_KW_STRUCT)
        return NULL;
    p_next(p); /* typedef */
    p_next(p); /* struct */
    AstNode* st = ast_new(p, AST_TYPEDEF_STRUCT);
    if (!st) return NULL;
    st->a[0] = 0;
    if (p_peek(p).kind == TK_IDENT) {
        Token tag = p_next(p);
        slice_to(st->a, sizeof(st->a), tag.spell);
    }
    if (p_accept(p, TK_PUNCT, "{")) {
        st->kids = &p->kids_storage[p->nkstore];
        if (!parse_struct_fields(p, st)) return NULL;
        if (!p_accept(p, TK_PUNCT, "}")) {
            parser_fail(p, p_peek(p), "expected '}' after typedef struct fields");
            return NULL;
        }
    } else if (!st->a[0]) {
        parser_fail(p, p_peek(p), "expected '{' after typedef struct");
        return NULL;
    }
    /* Opaque or closing alias after `}`. */
    Token alias = p_next(p);
    if (alias.kind != TK_IDENT) {
        parser_fail(p, alias, "expected typedef struct alias");
        return NULL;
    }
    slice_to(st->b, sizeof(st->b), alias.spell);
    if (!p_accept(p, TK_PUNCT, ";")) {
        parser_fail(p, p_peek(p), "expected ';' after typedef struct");
        return NULL;
    }
    if (!scope_add_typedef(p, alias.spell)) { p->err = 1; return NULL; }
    return st;
}

/* typedef Ret (*Name)(params); */
static AstNode* parse_typedef_fn_ptr(Parser* p) {
    if (shadow_kw(p_peek(p)) != SHADOW_KW_TYPEDEF) return NULL;
    if (p->i + 5 >= p->n) return NULL;
    Token rty = p->toks[p->i + 1];
    if (rty.kind != TK_IDENT && shadow_kw(rty) != SHADOW_KW_VOID &&
        shadow_kw(rty) != SHADOW_KW_INT && shadow_kw(rty) != SHADOW_KW_CHAR)
        return NULL;
    if (!tok_eq(p->toks[p->i + 2], TK_PUNCT, "(") ||
        !tok_eq(p->toks[p->i + 3], TK_PUNCT, "*") ||
        p->toks[p->i + 4].kind != TK_IDENT ||
        !tok_eq(p->toks[p->i + 5], TK_PUNCT, ")"))
        return NULL;
    p_next(p); /* typedef */
    p_next(p); /* ret */
    p_next(p); /* ( */
    p_next(p); /* * */
    Token name = p_next(p);
    p_next(p); /* ) */
    if (!p_accept(p, TK_PUNCT, "(")) {
        parser_fail(p, p_peek(p), "expected '(' after typedef fn-ptr name");
        return NULL;
    }
    int p0 = p->i;
    int depth = 1;
    while (p->i < p->n && depth > 0) {
        Token t = p_peek(p);
        if (tok_eq(t, TK_PUNCT, "(")) depth++;
        else if (tok_eq(t, TK_PUNCT, ")")) {
            depth--;
            if (depth == 0) break;
        }
        p_next(p);
    }
    char params[256];
    if (p0 >= p->i) params[0] = 0;
    else if (!ast_spell_token_range(p, p0, p->i, params, sizeof(params))) {
        parser_fail(p, name, "typedef fn-ptr params too long");
        return NULL;
    }
    if (!p_accept(p, TK_PUNCT, ")") || !p_accept(p, TK_PUNCT, ";")) {
        parser_fail(p, p_peek(p), "expected ');' after typedef fn-ptr");
        return NULL;
    }
    if (!scope_add_typedef(p, name.spell)) { p->err = 1; return NULL; }
    AstNode* n = ast_new(p, AST_TYPEDEF_FN_PTR);
    if (!n) return NULL;
    slice_to(n->a, sizeof(n->a), rty.spell);
    slice_to(n->b, sizeof(n->b), name.spell);
    snprintf(n->c, sizeof(n->c), "%s", params);
    return n;
}

/* typedef Elem[*][~N [sched] [ordered] >|<] Alias; */
static AstNode* parse_typedef_chan(Parser* p) {
    Token ety;
    ShadowKwKind ekw;
    int ei;
    int bi;
    int di;
    int ordered = 0;
    int is_ptr = 0;
    char cap_txt[32];
    char topo_txt[32];
    char elem_txt[64];
    Token dir;
    Token alias;
    AstNode* n;
    topo_txt[0] = 0;
    if (shadow_kw(p_peek(p)) != SHADOW_KW_TYPEDEF) return NULL;
    if (p->i + 6 >= p->n) return NULL;
    ety = p->toks[p->i + 1];
    ekw = shadow_kw(ety);
    if (ety.kind != TK_IDENT && ekw != SHADOW_KW_INT && ekw != SHADOW_KW_CHAR &&
        ekw != SHADOW_KW_BOOL && ekw != SHADOW_KW_SIZE_T && ekw != SHADOW_KW_VOID)
        return NULL;
    ei = p->i + 2;
    if (ei < p->n && tok_eq(p->toks[ei], TK_PUNCT, "*")) {
        is_ptr = 1;
        ei++;
    }
    bi = ei;
    if (bi + 1 >= p->n || !tok_eq(p->toks[bi], TK_PUNCT, "[") ||
        !tok_eq(p->toks[bi + 1], TK_PUNCT, "~"))
        return NULL;
    di = bi + 2;
    if (di < p->n &&
        (p->toks[di].kind == TK_NUM || p->toks[di].kind == TK_IDENT)) {
        slice_to(cap_txt, sizeof(cap_txt), p->toks[di].spell);
        di++;
    } else {
        snprintf(cap_txt, sizeof(cap_txt), "0");
    }
    /* `ordered` and schedule `N:1` may appear in either order. */
    for (;;) {
        if (di < p->n && shadow_kw(p->toks[di]) == SHADOW_KW_ORDERED) {
            ordered = 1;
            di++;
            continue;
        }
        if (di + 2 < p->n && tok_eq(p->toks[di + 1], TK_PUNCT, ":") &&
            p->toks[di + 2].kind == TK_NUM &&
            (p->toks[di].kind == TK_NUM || p->toks[di].kind == TK_IDENT)) {
            char left[16], right[16];
            slice_to(left, sizeof(left), p->toks[di].spell);
            slice_to(right, sizeof(right), p->toks[di + 2].spell);
            snprintf(topo_txt, sizeof(topo_txt), "%s:%s", left, right);
            di += 3;
            continue;
        }
        break;
    }
    if (di >= p->n) return NULL;
    dir = p->toks[di];
    if (!tok_eq(dir, TK_PUNCT, ">") && !tok_eq(dir, TK_PUNCT, "<")) return NULL;
    if (di + 1 >= p->n || !tok_eq(p->toks[di + 1], TK_PUNCT, "]")) return NULL;
    if (di + 2 >= p->n || p->toks[di + 2].kind != TK_IDENT) return NULL;
    if (di + 3 >= p->n || !tok_eq(p->toks[di + 3], TK_PUNCT, ";")) return NULL;
    if (ordered && tok_eq(dir, TK_PUNCT, ">")) {
        parser_fail(p, dir,
                    "'ordered' modifier only allowed on receive (<) channel");
        return NULL;
    }

    p_next(p); /* typedef */
    p_next(p); /* elem */
    if (tok_eq(p_peek(p), TK_PUNCT, "*")) p_next(p);
    p_next(p); /* [ */
    p_next(p); /* ~ */
    if (p->toks[p->i].kind == TK_NUM || p->toks[p->i].kind == TK_IDENT)
        p_next(p);
    for (;;) {
        if (shadow_kw(p_peek(p)) == SHADOW_KW_ORDERED) {
            p_next(p);
            continue;
        }
        if (p->i + 2 < p->n && tok_eq(p->toks[p->i + 1], TK_PUNCT, ":") &&
            p->toks[p->i + 2].kind == TK_NUM &&
            (p->toks[p->i].kind == TK_NUM || p->toks[p->i].kind == TK_IDENT)) {
            p_next(p);
            p_next(p);
            p_next(p);
            continue;
        }
        break;
    }
    p_next(p); /* > or < */
    p_next(p); /* ] */
    alias = p_next(p);
    p_next(p); /* ; */
    if (!scope_add_typedef(p, alias.spell)) { p->err = 1; return NULL; }
    n = ast_new(p, AST_TYPEDEF_CHAN);
    if (!n) return NULL;
    slice_to(n->a, sizeof(n->a), alias.spell);
    snprintf(n->b, sizeof(n->b), "%s", cap_txt);
    slice_to(n->c, sizeof(n->c), dir.spell);
    slice_to(elem_txt, sizeof(elem_txt), ety.spell);
    if (is_ptr) {
        size_t el = strlen(elem_txt);
        if (el + 1 < sizeof(elem_txt)) {
            elem_txt[el] = '*';
            elem_txt[el + 1] = 0;
        }
    }
    snprintf(n->d, sizeof(n->d), "%s", elem_txt);
    if (ordered && topo_txt[0])
        snprintf(n->e, sizeof(n->e), "o:%s", topo_txt);
    else if (ordered)
        snprintf(n->e, sizeof(n->e), "o");
    else if (topo_txt[0])
        snprintf(n->e, sizeof(n->e), "t:%s", topo_txt);
    return n;
}

/* [const] [struct] Ret[*] name(params); — file-scope prototype (no body). */
static AstNode* parse_fn_proto(Parser* p) {
    int ri = p->i;
    int has_const = 0;
    int has_struct = 0;
    if (ri < p->n && shadow_kw(p->toks[ri]) == SHADOW_KW_CONST) {
        has_const = 1;
        ri++;
    }
    if (ri < p->n && shadow_kw(p->toks[ri]) == SHADOW_KW_STRUCT) {
        has_struct = 1;
        ri++;
    }
    if (ri >= p->n) return NULL;
    Token rty = p->toks[ri];
    if (has_struct) {
        if (rty.kind != TK_IDENT) return NULL;
    } else if (rty.kind != TK_IDENT && shadow_kw(rty) != SHADOW_KW_INT &&
               shadow_kw(rty) != SHADOW_KW_VOID && shadow_kw(rty) != SHADOW_KW_CHAR &&
               shadow_kw(rty) != SHADOW_KW_SIZE_T && shadow_kw(rty) != SHADOW_KW_BOOL)
        return NULL;
    if (ri + 2 >= p->n) return NULL;
    int ni = ri + 1;
    int is_ptr = 0;
    int is_slice = 0;
    int is_slice_unique = 0;
    if (!has_struct && ni + 2 < p->n &&
        tok_eq(p->toks[ni], TK_PUNCT, "[") &&
        tok_eq(p->toks[ni + 1], TK_PUNCT, ":")) {
        if (tok_eq(p->toks[ni + 2], TK_PUNCT, "]")) {
            is_slice = 1;
            ni += 3;
        } else if (ni + 3 < p->n && tok_eq(p->toks[ni + 2], TK_PUNCT, "!") &&
                   tok_eq(p->toks[ni + 3], TK_PUNCT, "]")) {
            is_slice = 1;
            is_slice_unique = 1;
            ni += 4;
        }
    }
    if (ni < p->n && tok_eq(p->toks[ni], TK_PUNCT, "*")) {
        is_ptr = 1;
        ni++;
    }
    if (ni >= p->n || p->toks[ni].kind != TK_IDENT) return NULL;
    if (ni + 1 >= p->n || !tok_eq(p->toks[ni + 1], TK_PUNCT, "(")) return NULL;
    /* Must end with `);` not `){`. */
    int j = ni + 2;
    int depth = 1;
    while (j < p->n && depth > 0) {
        Token t = p->toks[j];
        if (tok_eq(t, TK_PUNCT, "(")) depth++;
        else if (tok_eq(t, TK_PUNCT, ")")) {
            depth--;
            if (depth == 0) break;
        }
        j++;
    }
    if (j + 1 >= p->n || !tok_eq(p->toks[j], TK_PUNCT, ")") ||
        !tok_eq(p->toks[j + 1], TK_PUNCT, ";"))
        return NULL;

    if (has_const) p_next(p); /* const */
    if (has_struct) p_next(p); /* struct */
    p_next(p); /* ret / tag */
    if (is_slice) {
        p_next(p); /* [ */
        p_next(p); /* : */
        if (is_slice_unique) p_next(p); /* ! */
        p_next(p); /* ] */
    }
    if (is_ptr) p_next(p); /* * */
    Token name = p_next(p);
    p_next(p); /* ( */
    int p0 = p->i;
    depth = 1;
    while (p->i < p->n && depth > 0) {
        Token t = p_peek(p);
        if (tok_eq(t, TK_PUNCT, "(")) depth++;
        else if (tok_eq(t, TK_PUNCT, ")")) {
            depth--;
            if (depth == 0) break;
        }
        p_next(p);
    }
    int p1 = p->i;
    char params[2048];
    params[0] = 0;
    if (p0 < p1 &&
        !ast_spell_token_range(p, p0, p1, params, sizeof(params))) {
        parser_fail(p, name, "fn proto params too long");
        return NULL;
    }
    if (!p_accept(p, TK_PUNCT, ")") || !p_accept(p, TK_PUNCT, ";")) {
        parser_fail(p, p_peek(p), "expected ');' after fn proto");
        return NULL;
    }
    AstNode* n = ast_new(p, AST_FN_PROTO);
    if (!n) return NULL;
    {
        char ty[256];
        if (has_const && has_struct)
            snprintf(ty, sizeof(ty), "const struct %.*s",
                     (int)rty.spell.len, rty.spell.ptr);
        else if (has_struct)
            snprintf(ty, sizeof(ty), "struct %.*s",
                     (int)rty.spell.len, rty.spell.ptr);
        else if (has_const)
            snprintf(ty, sizeof(ty), "const %.*s",
                     (int)rty.spell.len, rty.spell.ptr);
        else
            snprintf(ty, sizeof(ty), "%.*s",
                     (int)rty.spell.len, rty.spell.ptr);
        if (is_slice) {
            size_t al = strlen(ty);
            const char* suf = is_slice_unique ? "[:!]" : "[:]";
            size_t sl = strlen(suf);
            if (al + sl < sizeof(ty))
                memcpy(ty + al, suf, sl + 1);
        }
        if (is_ptr) {
            size_t al = strlen(ty);
            if (al + 1 < sizeof(ty)) { ty[al] = '*'; ty[al + 1] = 0; }
        }
        snprintf(n->a, sizeof(n->a), "%s", ty);
    }
    slice_to(n->b, sizeof(n->b), name.spell);
    if (strlen(params) + 1 <= sizeof(n->c))
        snprintf(n->c, sizeof(n->c), "%s", params);
    else {
        (void)span_bind(p, p0, p1, n);
        n->c[0] = 0;
    }
    return n;
}

/* typedef Ok !> (Err) Alias; */
static AstNode* parse_typedef_result(Parser* p) {
    if (shadow_kw(p_peek(p)) != SHADOW_KW_TYPEDEF) return NULL;
    {
        /* Do not copy Parser (~MB with AST_CAP) onto the stack. */
        int saved = p->i;
        int ok_shape;
        p->i++;
        ok_shape = peek_result_shape(p);
        p->i = saved;
        if (!ok_shape) return NULL;
    }
    if (p->i + 5 >= p->n) return NULL;
    p_next(p); /* typedef */
    Token ok = p_next(p);
    char okty[64];
    slice_to(okty, sizeof(okty), ok.spell);
    if (tok_eq(p_peek(p), TK_PUNCT, "*")) {
        p_next(p);
        size_t al = strlen(okty);
        if (al + 1 < sizeof(okty)) {
            okty[al] = '*';
            okty[al + 1] = 0;
        }
    }
    p_next(p); /* !> */
    Token err;
    if (tok_eq(p_peek(p), TK_PUNCT, "(")) {
        p_next(p);
        err = p_next(p);
        if (err.kind != TK_IDENT) { p->err = 1; return NULL; }
        if (!p_accept(p, TK_PUNCT, ")")) { p->err = 1; return NULL; }
    } else {
        err = p_next(p);
        if (err.kind != TK_IDENT) { p->err = 1; return NULL; }
    }
    Token alias = p_next(p);
    if (alias.kind != TK_IDENT) { p->err = 1; return NULL; }
    if (!p_accept(p, TK_PUNCT, ";")) { p->err = 1; return NULL; }
    if (!scope_add_typedef(p, alias.spell)) { p->err = 1; return NULL; }
    AstNode* n = ast_new(p, AST_TYPEDEF_INT);
    if (!n) return NULL;
    slice_to(n->a, sizeof(n->a), alias.spell);
    snprintf(n->c, sizeof(n->c), "%s", okty);
    slice_to(n->d, sizeof(n->d), err.spell);
    return n;
}

/* typedef [const] char[:] Alias; */
static AstNode* parse_typedef_slice(Parser* p) {
    if (shadow_kw(p_peek(p)) != SHADOW_KW_TYPEDEF) return NULL;
    int i = p->i + 1;
    int has_const = 0;
    if (i < p->n && shadow_kw(p->toks[i]) == SHADOW_KW_CONST) {
        has_const = 1;
        i++;
    }
    if (i + 4 >= p->n) return NULL;
    if (shadow_kw(p->toks[i]) != SHADOW_KW_CHAR) return NULL;
    if (!tok_eq(p->toks[i + 1], TK_PUNCT, "[") ||
        !tok_eq(p->toks[i + 2], TK_PUNCT, ":") ||
        !tok_eq(p->toks[i + 3], TK_PUNCT, "]"))
        return NULL;
    if (p->toks[i + 4].kind != TK_IDENT) return NULL;
    if (i + 5 >= p->n || !tok_eq(p->toks[i + 5], TK_PUNCT, ";")) return NULL;
    p_next(p); /* typedef */
    if (has_const) p_next(p);
    p_next(p); p_next(p); p_next(p); p_next(p); /* char [:] */
    Token alias = p_next(p);
    if (!p_accept(p, TK_PUNCT, ";")) { p->err = 1; return NULL; }
    if (!scope_add_typedef(p, alias.spell)) { p->err = 1; return NULL; }
    AstNode* n = ast_new(p, AST_TYPEDEF_INT);
    if (!n) return NULL;
    slice_to(n->a, sizeof(n->a), alias.spell);
    snprintf(n->b, sizeof(n->b), "%sCCSlice", has_const ? "const " : "");
    return n;
}

/* typedef ArrayMap::[K,V] Alias; / Map::[K,V] Alias; / Vec::[T] Alias; */
static AstNode* parse_typedef_generic(Parser* p) {
    int ty0, ty_end;
    char spelled[128];
    Token alias;
    AstNode* n;
    if (shadow_kw(p_peek(p)) != SHADOW_KW_TYPEDEF) return NULL;
    if (p->i + 1 >= p->n || p->toks[p->i + 1].kind != TK_IDENT) return NULL;
    ty0 = p->i + 1;
    ty_end = peek_generic_type_end(p, ty0);
    if (ty_end < 0) return NULL;
    if (ty_end >= p->n || p->toks[ty_end].kind != TK_IDENT) return NULL;
    if (ty_end + 1 >= p->n || !tok_eq(p->toks[ty_end + 1], TK_PUNCT, ";"))
        return NULL;
    if (!ast_spell_type_tokens(p, ty0, ty_end, spelled, sizeof(spelled)))
        return NULL;
    p_next(p); /* typedef */
    while (p->i < ty_end) p_next(p);
    alias = p_next(p);
    if (!p_accept(p, TK_PUNCT, ";")) { p->err = 1; return NULL; }
    if (!scope_add_typedef(p, alias.spell)) { p->err = 1; return NULL; }
    n = ast_new(p, AST_TYPEDEF_INT);
    if (!n) return NULL;
    slice_to(n->a, sizeof(n->a), alias.spell);
    /* ast_spell_type_tokens yields ArrayMap_K_V* / Map_K_V* / CCVec_T. */
    snprintf(n->b, sizeof(n->b), "%s", spelled);
    return n;
}

/* typedef @typeview Mode on Base { … } [*]? Alias ;
 * Defines the mode and a transparent alias in one declaration. */
static AstNode* parse_typedef_restricted(Parser* p) {
    Token mode, on, base, alias;
    int body0, body1;
    int is_ptr = 0;
    char body[1024];
    char mangled[160];
    AstNode* n;
    CCSlice ms;
    if (shadow_kw(p_peek(p)) != SHADOW_KW_TYPEDEF) return NULL;
    if (p->i + 6 >= p->n) return NULL;
    if (!tok_eq(p->toks[p->i + 1], TK_PUNCT, "@")) return NULL;
    if (p->toks[p->i + 2].kind != TK_IDENT)
        return NULL;
    if (spell_eq(p->toks[p->i + 2].spell, "restricted")) {
        parser_fail(p, p->toks[p->i + 2],
                    "'@restricted' was removed; use '@typeview'");
        return NULL;
    }
    if (!spell_eq(p->toks[p->i + 2].spell, "typeview"))
        return NULL;
    /* Sugar `typedef @typeview(Mode) Base* Alias` is rewritten on the tape
     * before parse; the define form is `@typeview Mode on Base {`. */
    if (tok_eq(p->toks[p->i + 3], TK_PUNCT, "(")) return NULL;
    if (p->toks[p->i + 3].kind != TK_IDENT) return NULL;
    if (p->toks[p->i + 4].kind != TK_IDENT ||
        !spell_eq(p->toks[p->i + 4].spell, "on"))
        return NULL;
    p_next(p); /* typedef */
    p_next(p); /* @ */
    p_next(p); /* restricted */
    mode = p_next(p);
    on = p_next(p);
    (void)on;
    base = p_next(p);
    if (base.kind != TK_IDENT) {
        parser_fail(p, base, "expected base type after @typeview … on");
        return NULL;
    }
    /* Glob subjects are only for unnamed `@typeview on Pat* { … }`. */
    if (tok_eq(p_peek(p), TK_PUNCT, "*")) {
        parser_fail(p, p_peek(p),
                    "named typedef @typeview cannot use a glob subject");
        return NULL;
    }
    if (!tok_eq(p_peek(p), TK_PUNCT, "{")) {
        parser_fail(p, p_peek(p), "expected '{' allow-list after @typeview");
        return NULL;
    }
    body0 = p->i;
    p_next(p);
    {
        int depth = 1;
        while (p->i < p->n && depth > 0) {
            Token t = p_next(p);
            if (tok_eq(t, TK_PUNCT, "{")) depth++;
            else if (tok_eq(t, TK_PUNCT, "}")) depth--;
        }
    }
    body1 = p->i;
    body[0] = 0;
    if (body0 >= 0 && body1 > body0) {
        if (!ast_spell_token_range(p, body0, body1, body, sizeof(body)) &&
            !span_text(p, body0, body1, body, sizeof(body))) {
            parser_fail(p, mode, "@typeview allow-list too long");
            return NULL;
        }
    }
    if (tok_eq(p_peek(p), TK_PUNCT, "*")) {
        is_ptr = 1;
        p_next(p);
    }
    alias = p_next(p);
    if (alias.kind != TK_IDENT) {
        parser_fail(p, alias, "expected typedef alias after @typeview body");
        return NULL;
    }
    if (!p_accept(p, TK_PUNCT, ";")) {
        parser_fail(p, p_peek(p), "expected ';' after typedef @typeview");
        return NULL;
    }
    snprintf(mangled, sizeof(mangled), "%.*s_Restrict_%.*s",
             (int)base.spell.len, base.spell.ptr, (int)mode.spell.len,
             mode.spell.ptr);
    ms.ptr = mangled;
    ms.len = strlen(mangled);
    (void)scope_add_typedef(p, ms);
    if (!scope_add_typedef(p, alias.spell)) {
        p->err = 1;
        return NULL;
    }
    n = ast_new(p, AST_AT_STMT);
    if (!n) return NULL;
    snprintf(n->a, sizeof(n->a), "typeview");
    slice_to(n->b, sizeof(n->b), mode.spell);
    snprintf(n->c, sizeof(n->c), "%s", body);
    slice_to(n->d, sizeof(n->d), base.spell);
    /* e = mangled#alias or mangled#*alias (typedef+define form). */
    snprintf(n->e, sizeof(n->e), "%s#%s%.*s", mangled, is_ptr ? "*" : "",
             (int)alias.spell.len, alias.spell.ptr);
    return n;
}

static AstNode* parse_typedef_int(Parser* p) {
    if (shadow_kw(p_peek(p)) != SHADOW_KW_TYPEDEF) return NULL;
    {
        AstNode* tr = parse_typedef_restricted(p);
        if (tr || p->err) return tr;
    }
    if (p->i + 1 < p->n && shadow_kw(p->toks[p->i + 1]) == SHADOW_KW_STRUCT)
        return parse_typedef_struct(p);
    if (p->i + 1 < p->n && shadow_kw(p->toks[p->i + 1]) == SHADOW_KW_ENUM)
        return parse_typedef_enum(p);
    {
        AstNode* rt = parse_typedef_result(p);
        if (rt || p->err) return rt;
    }
    {
        AstNode* sl = parse_typedef_slice(p);
        if (sl || p->err) return sl;
    }
    {
        AstNode* ch = parse_typedef_chan(p);
        if (ch || p->err) return ch;
    }
    {
        AstNode* fp = parse_typedef_fn_ptr(p);
        if (fp || p->err) return fp;
    }
    {
        AstNode* g = parse_typedef_generic(p);
        if (g || p->err) return g;
    }
    /* `typedef int Alias;` */
    if (p->i + 1 < p->n && shadow_kw(p->toks[p->i + 1]) == SHADOW_KW_INT) {
        p_next(p);
        p_next(p);
        Token name = p_next(p);
        if (name.kind != TK_IDENT) { p->err = 1; return NULL; }
        if (!p_accept(p, TK_PUNCT, ";")) { p->err = 1; return NULL; }
        if (!scope_add_typedef(p, name.spell)) { p->err = 1; return NULL; }
        AstNode* n = ast_new(p, AST_TYPEDEF_INT);
        if (!n) return NULL;
        slice_to(n->a, sizeof(n->a), name.spell);
        return n;
    }
    /* `typedef Ident Alias;` / `typedef Ident* Alias;` — host builtins
     * (`__builtin_va_list`) and opaque ABI aliases (pigz_cc stdarg.h). */
    if (p->i + 2 < p->n && p->toks[p->i + 1].kind == TK_IDENT) {
        int j = p->i + 2;
        int nstars = 0;
        Token alias;
        Token base;
        AstNode* n;
        while (j < p->n && tok_eq(p->toks[j], TK_PUNCT, "*")) {
            nstars++;
            j++;
        }
        if (j < p->n && p->toks[j].kind == TK_IDENT && j + 1 < p->n &&
            tok_eq(p->toks[j + 1], TK_PUNCT, ";")) {
            int stars_left = nstars;
            p_next(p); /* typedef */
            base = p_next(p);
            while (stars_left-- > 0) p_next(p); /* * */
            alias = p_next(p);
            p_next(p); /* ; */
            if (!scope_add_typedef(p, alias.spell)) { p->err = 1; return NULL; }
            n = ast_new(p, AST_TYPEDEF_INT);
            if (!n) return NULL;
            slice_to(n->a, sizeof(n->a), alias.spell);
            slice_to(n->b, sizeof(n->b), base.spell);
            while (nstars-- > 0) {
                size_t L = strlen(n->b);
                if (L + 1 < sizeof(n->b)) {
                    n->b[L] = '*';
                    n->b[L + 1] = 0;
                }
            }
            return n;
        }
    }
    /* `typedef long long Alias;` / `unsigned int Alias;` / `short Alias;` */
    if (p->i + 1 < p->n) {
        int base = p->i + 1;
        int te = peek_c_int_type_end(p, base);
        if (te > base) {
            int j = te;
            int nstars = 0;
            Token alias;
            AstNode* n;
            while (j < p->n && tok_eq(p->toks[j], TK_PUNCT, "*")) {
                nstars++;
                j++;
            }
            if (j < p->n && p->toks[j].kind == TK_IDENT && j + 1 < p->n &&
                tok_eq(p->toks[j + 1], TK_PUNCT, ";")) {
                char base_ty[64];
                int stars_left = nstars;
                base_ty[0] = 0;
                p_next(p); /* typedef */
                if (!ast_spell_token_range(p, base, te, base_ty,
                                           sizeof(base_ty)) &&
                    !span_text(p, base, te, base_ty, sizeof(base_ty))) {
                    parser_fail(p, p->toks[base],
                                "typedef: multi-token base type too long");
                    return NULL;
                }
                while (p->i < te) p_next(p);
                while (stars_left-- > 0) p_next(p); /* * */
                alias = p_next(p);
                p_next(p); /* ; */
                if (!scope_add_typedef(p, alias.spell)) {
                    p->err = 1;
                    return NULL;
                }
                n = ast_new(p, AST_TYPEDEF_INT);
                if (!n) return NULL;
                slice_to(n->a, sizeof(n->a), alias.spell);
                snprintf(n->b, sizeof(n->b), "%s", base_ty);
                while (nstars-- > 0) {
                    size_t L = strlen(n->b);
                    if (L + 1 < sizeof(n->b)) {
                        n->b[L] = '*';
                        n->b[L + 1] = 0;
                    }
                }
                return n;
            }
        }
    }
    return NULL;
}

/* Ident * Ident ;  — decl if typedef / CC* runtime type, else mul expr */
static AstNode* parse_star_stmt(Parser* p) {
    Token a = p_peek(p);
    char tname[128];
    int is_ptr_ty;
    if (a.kind != TK_IDENT) return NULL;
    /* need Ident * Ident ; */
    if (p->i + 3 >= p->n) return NULL;
    Token star = p->toks[p->i + 1];
    Token b = p->toks[p->i + 2];
    Token semi = p->toks[p->i + 3];
    if (!tok_eq(star, TK_PUNCT, "*") || b.kind != TK_IDENT || !tok_eq(semi, TK_PUNCT, ";"))
        return NULL;
    slice_to(tname, sizeof(tname), a.spell);
    /* Passthrough includes seed CCChan / CCNursery / … without typedef scope. */
    is_ptr_ty = scope_is_typedef(p, a.spell) ||
                (tname[0] == 'C' && tname[1] == 'C') ||
                (tname[0] == 'c' && tname[1] == 'c' && tname[2] == '_');
    p_next(p); p_next(p); p_next(p); p_next(p);
    AstNode* n;
    if (is_ptr_ty) {
        n = ast_new(p, AST_PTR_DECL);
    } else {
        n = ast_new(p, AST_MUL_EXPR);
    }
    if (!n) return NULL;
    slice_to(n->a, sizeof(n->a), a.spell);
    slice_to(n->b, sizeof(n->b), b.spell);
    return n;
}

/* Copy a same-line C block comment after `after` into dst (empty if none). */
static void shadow_same_line_c_comment(Parser* p, Token after, char* dst, size_t cap) {
    FileTape* ft;
    size_t i, j, n;
    if (!dst || !cap) return;
    dst[0] = 0;
    if (!p || !p->cache) return;
    ft = tape_by_id(p->cache, after.file_id);
    if (!ft || !ft->bytes) return;
    i = after.offset + after.spell.len;
    while (i < ft->len && (ft->bytes[i] == ' ' || ft->bytes[i] == '\t')) i++;
    if (i + 1 >= ft->len || ft->bytes[i] != '/' || ft->bytes[i + 1] != '*') return;
    j = i + 2;
    while (j + 1 < ft->len && !(ft->bytes[j] == '*' && ft->bytes[j + 1] == '/')) {
        if (ft->bytes[j] == '\n') return;
        j++;
    }
    if (j + 1 >= ft->len) return;
    j += 2;
    n = j - i;
    if (n >= cap) n = cap - 1;
    memcpy(dst, ft->bytes + i, n);
    dst[n] = 0;
}

static AstNode* parse_struct(Parser* p) {
    int start;
    Token t0;
    if (shadow_kw(p_peek(p)) != SHADOW_KW_STRUCT) return NULL;
    start = p->i;
    t0 = p_peek(p);
    p_next(p); /* struct */
    /* Anonymous file-scope: `struct { … } name [= init];` — tape passthrough
     * (pigz_cc `struct { … } g = {0};`). Do not set err on `{` (silent
     * degradation); either lower as tape or fail with a message. */
    if (tok_eq(p_peek(p), TK_PUNCT, "{")) {
        int depth = 0;
        size_t off0, off1;
        AstNode* n;
        Token last;
        while (p->i < p->n) {
            Token cur = p_peek(p);
            if (tok_eq(cur, TK_PUNCT, "{")) depth++;
            else if (tok_eq(cur, TK_PUNCT, "}")) {
                depth--;
                p_next(p);
                if (depth == 0) break;
                continue;
            }
            p_next(p);
        }
        if (depth != 0) {
            parser_fail(p, t0, "unterminated anonymous struct");
            return NULL;
        }
        if (p_peek(p).kind != TK_IDENT) {
            parser_fail(p, p_peek(p),
                        "expected variable name after anonymous struct");
            return NULL;
        }
        {
            Token gname = p_next(p); /* name */
            char gbuf[64];
            slice_to(gbuf, sizeof(gbuf), gname.spell);
            /* File-scope instance — bare uses in closures are not captures. */
            spawn_note_global(gbuf);
        }
        if (tok_eq(p_peek(p), TK_PUNCT, "=")) {
            int idepth = 0;
            p_next(p); /* = */
            while (p->i < p->n) {
                Token cur = p_peek(p);
                if (tok_eq(cur, TK_PUNCT, "(") || tok_eq(cur, TK_PUNCT, "{") ||
                    tok_eq(cur, TK_PUNCT, "["))
                    idepth++;
                else if (tok_eq(cur, TK_PUNCT, ")") || tok_eq(cur, TK_PUNCT, "}") ||
                         tok_eq(cur, TK_PUNCT, "]")) {
                    if (idepth > 0) idepth--;
                } else if (idepth == 0 && tok_eq(cur, TK_PUNCT, ";"))
                    break;
                p_next(p);
            }
        }
        if (!p_accept(p, TK_PUNCT, ";")) {
            parser_fail(p, p_peek(p),
                        "expected ';' after anonymous struct variable");
            return NULL;
        }
        last = p->toks[p->i - 1];
        off0 = t0.offset;
        off1 = last.offset + last.spell.len;
        n = ast_new(p, AST_RAW_LINE);
        if (!n) return NULL;
        snprintf(n->e, sizeof(n->e), "tape");
        snprintf(n->a, sizeof(n->a), "%zu", off0);
        snprintf(n->b, sizeof(n->b), "%zu", off1);
        n->file_id = t0.file_id;
        (void)start;
        return n;
    }
    {
        Token name = p_next(p);
        if (name.kind != TK_IDENT) {
            parser_fail(p, name, "expected struct tag or '{'");
            return NULL;
        }
        /* Forward: struct Tag; (+ optional same-line block comment). */
        if (tok_eq(p_peek(p), TK_PUNCT, ";")) {
            Token semi = p_next(p);
            AstNode* st = ast_new(p, AST_STRUCT);
            if (!st) return NULL;
            slice_to(st->a, sizeof(st->a), name.spell);
            snprintf(st->e, sizeof(st->e), "fwd");
            shadow_same_line_c_comment(p, semi, st->d, sizeof(st->d));
            return st;
        }
        if (!p_accept(p, TK_PUNCT, "{")) {
            parser_fail(p, p_peek(p), "expected '{' after struct tag");
            return NULL;
        }
        {
            AstNode* st = ast_new(p, AST_STRUCT);
            if (!st) return NULL;
            slice_to(st->a, sizeof(st->a), name.spell);
            st->kids = &p->kids_storage[p->nkstore];
            if (!parse_struct_fields(p, st)) return NULL;
            if (!p_accept(p, TK_PUNCT, "}")) {
                parser_fail(p, p_peek(p), "expected '}' after struct fields");
                return NULL;
            }
            p_accept(p, TK_PUNCT, ";"); /* optional trailing semi */
            return st;
        }
    }
}

/* `#ifdef` / `#else` / `#endif` (stage-2 whole-line tokens). File scope and
 * stmt bodies already keep these; struct fields must too or `#else` is a type. */
static AstNode* parse_ppdir_line(Parser* p) {
    Token t = p_peek(p);
    AstNode* n;
    if (t.kind != TK_IDENT || t.spell.len == 0 || t.spell.ptr[0] != '#')
        return NULL;
    n = ast_new(p, AST_RAW_LINE);
    if (!n) return NULL;
    if (t.spell.len >= sizeof(n->a)) {
        parser_fail(p, t, "preprocessor directive line too long");
        return NULL;
    }
    memcpy(n->a, t.spell.ptr, t.spell.len);
    n->a[t.spell.len] = 0;
    snprintf(n->e, sizeof(n->e), "ppdir");
    p_next(p);
    return n;
}

static int parse_struct_fields(Parser* p, AstNode* st) {
    int close = -1;
    int j;
    int depth = 0;
    for (j = p->i; j < p->n; j++) {
        if (tok_eq(p->toks[j], TK_PUNCT, "{")) depth++;
        else if (tok_eq(p->toks[j], TK_PUNCT, "}")) {
            if (depth == 0) {
                close = j;
                break;
            }
            depth--;
        }
    }
    /* Nested struct/union are cparse's. Flatten keeps start/end when the
     * declarator outgrows CpFlat.text; emit reprints the FileTape span. */
    if (close >= p->i && shadow_toks_are_c(p, p->i, close))
        return parse_struct_fields_via_cparse(p, st, close);
    while (!tok_eq(p_peek(p), TK_PUNCT, "}") && p_peek(p).kind != TK_EOF) {
        int start = p->i;
        AstNode* f = parse_ppdir_line(p);
        if (!f) f = parse_slice_var(p);
        if (!f) f = parse_chan_var(p);
        if (!f) f = parse_field_result(p);
        if (!f) f = parse_int_decl(p, AST_FIELD_INT);
        if (!f) f = parse_field_simple(p);
        if (p->err) return 0;
        if (!f) {
            /* Beachhead: keep unrecognized member declarators as raw text
             * (fn-ptrs, bitfields, nested anon structs) so reflection TUs parse. */
            int depth = 0;
            int end;
            while (p->i < p->n) {
                Token t = p_peek(p);
                if (depth == 0 && tok_eq(t, TK_PUNCT, ";")) break;
                if (tok_eq(t, TK_PUNCT, "{") || tok_eq(t, TK_PUNCT, "(") ||
                    tok_eq(t, TK_PUNCT, "["))
                    depth++;
                else if (tok_eq(t, TK_PUNCT, "}") || tok_eq(t, TK_PUNCT, ")") ||
                         tok_eq(t, TK_PUNCT, "]")) {
                    if (depth > 0) depth--;
                    else if (tok_eq(t, TK_PUNCT, "}")) break;
                }
                p_next(p);
            }
            end = p->i;
            if (!tok_eq(p_peek(p), TK_PUNCT, ";")) {
                parser_fail(p, p_peek(p), "bad struct field");
                return 0;
            }
            p_next(p); /* ; */
            f = ast_new(p, AST_FIELD_SIMPLE);
            if (!f) return 0;
            (void)span_bind(p, start, end, f);
            if (span_text_len(p, start, end) + 1 <= sizeof(f->a)) {
                if (!span_text(p, start, end, f->a, sizeof(f->a))) return 0;
            } else {
                f->a[0] = 0;
            }
            f->b[0] = 0;
            snprintf(f->e, sizeof(f->e), "raw");
        }
        shadow_attach_lead(p, f, start);
        if (!ast_kids_push(p, f)) return 0;
        st->nkids++;
    }
    return 1;
}

/* const Type* name[] = { init_list }; — global/file-scope array */
static AstNode* parse_global_arr(Parser* p) {
    int ci = p->i;
    int has_const = 0;
    if (shadow_kw(p->toks[ci]) == SHADOW_KW_CONST) { has_const = 1; ci++; }
    if (ci >= p->n) return NULL;
    Token ty = p->toks[ci];
    if (ty.kind != TK_IDENT && shadow_kw(ty) != SHADOW_KW_INT &&
        shadow_kw(ty) != SHADOW_KW_CHAR)
        return NULL;
    ci++;
    int is_ptr = 0;
    if (ci < p->n && tok_eq(p->toks[ci], TK_PUNCT, "*")) { is_ptr = 1; ci++; }
    if (ci >= p->n || p->toks[ci].kind != TK_IDENT) return NULL;
    ci++;
    if (ci >= p->n || !tok_eq(p->toks[ci], TK_PUNCT, "[")) return NULL;
    ci++;
    if (ci >= p->n || !tok_eq(p->toks[ci], TK_PUNCT, "]")) return NULL;
    ci++;
    if (ci >= p->n || !tok_eq(p->toks[ci], TK_PUNCT, "=")) return NULL;
    ci++;
    if (ci >= p->n || !tok_eq(p->toks[ci], TK_PUNCT, "{")) return NULL;
    /* Find matching } ; */
    int j = ci;
    int depth = 0;
    while (j < p->n) {
        if (tok_eq(p->toks[j], TK_PUNCT, "{")) depth++;
        else if (tok_eq(p->toks[j], TK_PUNCT, "}")) {
            depth--;
            if (depth == 0) { j++; break; }
        }
        j++;
    }
    if (j >= p->n || !tok_eq(p->toks[j], TK_PUNCT, ";")) return NULL;
    /* Commit */
    if (has_const) p_next(p);
    p_next(p); /* type */
    if (is_ptr) p_next(p);
    Token name = p_next(p);
    p_next(p); /* [ */
    p_next(p); /* ] */
    p_next(p); /* = */
    int init0 = p->i;
    while (p->i < j) p_next(p);
    char init[256];
    if (!span_text(p, init0, j, init, sizeof(init))) {
        parser_fail(p, name, "global array init too long");
        return NULL;
    }
    p_next(p); /* ; */
    AstNode* n = ast_new(p, AST_GLOBAL_ARR);
    if (!n) return NULL;
    if (has_const)
        snprintf(n->a, sizeof(n->a), "const %.*s%s", (int)ty.spell.len, ty.spell.ptr,
                 is_ptr ? "*" : "");
    else
        snprintf(n->a, sizeof(n->a), "%.*s%s", (int)ty.spell.len, ty.spell.ptr,
                 is_ptr ? "*" : "");
    slice_to(n->b, sizeof(n->b), name.spell);
    snprintf(n->c, sizeof(n->c), "%s", init);
    return n;
}

static AstNode* parse_external_inner(Parser* p) {
    Token t = p_peek(p);
    if (t.kind == TK_EOF) return NULL;
    AstNode* n;
    /* cpp-transparent `#if`/`#ifdef`/`#ifndef`/`#elif`/`#else`/`#endif`
     * lines from stage-2. */
    n = parse_ppdir_line(p);
    if (n || p->err) return n;
    if (tok_eq(t, TK_PUNCT, "@")) {
        if (p->i + 1 < p->n && shadow_kw(p->toks[p->i + 1]) == SHADOW_KW_SERIAL) {
            parser_fail(p, p->toks[p->i + 1],
                        "@serial is only an arm of @parallel { }");
            return NULL;
        }
        if (p->i + 1 < p->n && shadow_kw(p->toks[p->i + 1]) == SHADOW_KW_ASYNC) {
            n = parse_async_fn(p);
            if (n || p->err) return n;
        }
        int save = p->i;
        unsigned attrs = 0;
        shadow_parser_skip_decl_specs(p, &attrs);
        if (p->i + 1 < p->n && shadow_kw(p->toks[p->i + 1]) == SHADOW_KW_ASYNC) {
            p->pending_fn_attrs = attrs;
            n = parse_async_fn(p);
            if (n || p->err) return n;
            p->pending_fn_attrs = 0;
        }
        if (shadow_kw(p_peek(p)) == SHADOW_KW_STATIC) {
            p->pending_fn_attrs = attrs;
            p_next(p); /* static */
            if (peek_result_shape(p)) {
                n = parse_result_fn(p);
                if (n) {
                    snprintf(n->e, sizeof(n->e), "static");
                    p->pending_fn_attrs = 0;
                    return n;
                }
                if (p->err) {
                    p->pending_fn_attrs = 0;
                    return n;
                }
            }
            p->i = save;
            shadow_parser_skip_decl_specs(p, &attrs);
            p->pending_fn_attrs = attrs;
            n = parse_static_fn(p);
            p->pending_fn_attrs = 0;
            if (n || p->err) return n;
        }
        if (peek_result_shape(p)) {
            p->pending_fn_attrs = attrs;
            n = parse_result_fn(p);
            if (n) {
                shadow_fn_attr_register(n->a, attrs, n->nkids > 0);
                p->pending_fn_attrs = 0;
                return n;
            }
            p->pending_fn_attrs = 0;
            if (p->err) return n;
        }
        if (shadow_kw(p_peek(p)) == SHADOW_KW_VOID ||
            shadow_kw(p_peek(p)) == SHADOW_KW_INT ||
            shadow_kw(p_peek(p)) == SHADOW_KW_CHAR ||
            shadow_kw(p_peek(p)) == SHADOW_KW_BOOL ||
            shadow_kw(p_peek(p)) == SHADOW_KW_SIZE_T ||
            p_peek(p).kind == TK_IDENT) {
            p->pending_fn_attrs = attrs;
            n = parse_fn(p);
            if (n) {
                shadow_fn_attr_register(n->a, attrs, 1);
                p->pending_fn_attrs = 0;
                return n;
            }
            p->pending_fn_attrs = 0;
            if (p->err) return n;
        }
        p->i = save;
        return parse_at_stmt(p);
    }
    if (tok_eq(t, TK_PUNCT, "(")) {
        n = parse_closure_lit(p);
        if (n || p->err) return n;
    }
    if (shadow_kw(t) == SHADOW_KW_STATIC) {
        /* static [@noblock] Ok!>(Err) name(...) */
        if (p->i + 1 < p->n) {
            int save = p->i;
            unsigned sattrs = 0;
            p_next(p); /* tentatively consume static */
            shadow_parser_skip_decl_specs(p, &sattrs);
            if (peek_result_shape(p)) {
                p->pending_fn_attrs = sattrs;
                n = parse_result_fn(p);
                if (n) {
                    snprintf(n->e, sizeof(n->e), "static");
                    p->pending_fn_attrs = 0;
                    return n;
                }
                if (p->err) {
                    p->pending_fn_attrs = 0;
                    return n;
                }
            }
            p->i = save;
            if (p->err) p->err = 0;
        }
        /* static [inline] Ok!>(Err) name(...) — legacy path */
        if (p->i + 1 < p->n) {
            int save = p->i;
            int has_inline = 0;
            p_next(p); /* tentatively consume static */
            if (shadow_kw(p_peek(p)) == SHADOW_KW_INLINE) {
                has_inline = 1;
                p_next(p);
            }
            if (peek_result_shape(p)) {
                n = parse_result_fn(p);
                if (n) {
                    if (has_inline)
                        snprintf(n->e, sizeof(n->e), "static inline");
                    else
                        snprintf(n->e, sizeof(n->e), "static");
                    return n;
                }
                if (p->err) return n;
            }
            p->i = save;
        }
        /* Prefer structured static parsers; on miss/soft-fail, keep one tape
         * span for the whole declaration (grammar matchers with goto/labels,
         * large static pools, __attribute__ forms). Successful structured
         * parse must not also keep a tape span for the same offsets. */
        {
            int save = p->i;
            n = parse_static_fn(p);
            if (n) return n;
            /* CC tokens in the body: parse_static_fn already refused tape. */
            if (p->err) return NULL;
            p->i = save;
            n = parse_static_arr(p);
            if (n) return n;
            p->i = save;
            if (p->err) p->err = 0;
            n = parse_static_var(p);
            if (n) return n;
            p->i = save;
            if (p->err) p->err = 0;
            {
                int start = p->i;
                int depth = 0;
                int saw_brace = 0;
                size_t off0, off1;
                Token t0 = p->toks[start];
                while (p->i < p->n) {
                    Token cur = p_peek(p);
                    if (tok_eq(cur, TK_PUNCT, "{")) {
                        depth++;
                        saw_brace = 1;
                    } else if (tok_eq(cur, TK_PUNCT, "}")) {
                        if (depth > 0) depth--;
                        p_next(p);
                        if (saw_brace && depth == 0) {
                            /* `} ;` for aggregate init / fn body end. */
                            if (tok_eq(p_peek(p), TK_PUNCT, ";")) p_next(p);
                            break;
                        }
                        continue;
                    } else if (depth == 0 && tok_eq(cur, TK_PUNCT, ";")) {
                        p_next(p);
                        break;
                    }
                    p_next(p);
                }
                if (p->i <= start) {
                    parser_fail(p, t, "bad static declaration");
                    return NULL;
                }
                off0 = t0.offset;
                {
                    Token last = p->toks[p->i - 1];
                    off1 = last.offset + last.spell.len;
                }
                n = ast_new(p, AST_RAW_LINE);
                if (!n) return NULL;
                snprintf(n->e, sizeof(n->e), "tape");
                snprintf(n->a, sizeof(n->a), "%zu", off0);
                snprintf(n->b, sizeof(n->b), "%zu", off1);
                n->file_id = t0.file_id;
                n->tok_off = off0;
                return n;
            }
        }
    }
    if (t.kind == TK_IDENT && spell_eq(t.spell, "extern")) {
        p_next(p);
        n = parse_fn_proto(p);
        if (n) {
            snprintf(n->e, sizeof(n->e), "extern");
            return n;
        }
        if (p->err) return n;
    }
    if (shadow_kw(t) == SHADOW_KW_TYPEDEF) return parse_typedef_int(p);
    /* Bare `enum { … };` / `enum Tag { … };` — tape passthrough for host-cc. */
    if (shadow_kw(t) == SHADOW_KW_ENUM) {
        int start = p->i;
        int depth = 0;
        int saw_brace = 0;
        size_t off0, off1;
        Token t0 = p->toks[start];
        while (p->i < p->n) {
            Token cur = p_peek(p);
            if (tok_eq(cur, TK_PUNCT, "{")) {
                depth++;
                saw_brace = 1;
            } else if (tok_eq(cur, TK_PUNCT, "}")) {
                if (depth > 0) depth--;
                p_next(p);
                if (saw_brace && depth == 0) {
                    if (tok_eq(p_peek(p), TK_PUNCT, ";")) p_next(p);
                    break;
                }
                continue;
            } else if (depth == 0 && tok_eq(cur, TK_PUNCT, ";")) {
                p_next(p);
                break;
            }
            p_next(p);
        }
        if (p->i <= start) {
            parser_fail(p, t, "bad enum declaration");
            return NULL;
        }
        off0 = t0.offset;
        {
            Token last = p->toks[p->i - 1];
            off1 = last.offset + last.spell.len;
        }
        n = ast_new(p, AST_RAW_LINE);
        if (!n) return NULL;
        snprintf(n->e, sizeof(n->e), "tape");
        snprintf(n->a, sizeof(n->a), "%zu", off0);
        snprintf(n->b, sizeof(n->b), "%zu", off1);
        n->file_id = t0.file_id;
        n->tok_off = off0;
        return n;
    }
    if (shadow_kw(t) == SHADOW_KW_STRUCT) {
        /* `struct Tag *? name(...) {` / `);` before `struct Tag { … }`. */
        n = parse_fn(p);
        if (n || p->err) return n;
        n = parse_fn_proto(p);
        if (n || p->err) return n;
        return parse_struct(p);
    }
    if (peek_result_shape(p)) {
        n = parse_result_fn(p);
        if (n || p->err) return n;
    }
    if (shadow_kw(t) == SHADOW_KW_INT) {
        n = parse_slice_var(p);
        if (n || p->err) return n;
        n = parse_chan_var(p);
        if (n || p->err) return n;
        n = parse_fn(p);
        if (n || p->err) return n;
        n = parse_fn_proto(p);
        if (n || p->err) return n;
        /* `int name;` only — `int g = 0;` / arrays fall through. */
        n = parse_int_decl(p, AST_VAR_INT);
        if (n || p->err) return n;
        n = parse_global_var(p);
        if (n || p->err) return n;
        return NULL;
    }
    if (shadow_kw(t) == SHADOW_KW_VOID || shadow_kw(t) == SHADOW_KW_CHAR ||
        shadow_kw(t) == SHADOW_KW_BOOL || shadow_kw(t) == SHADOW_KW_SIZE_T ||
        t.kind == TK_IDENT) {
        n = parse_global_var(p);
        if (n || p->err) return n;
        n = parse_chan_var(p);
        if (n || p->err) return n;
        n = parse_fn(p);
        if (n || p->err) return n;
    }
    n = parse_fn_proto(p);
    if (n || p->err) return n;
    n = parse_star_stmt(p);
    if (n || p->err) return n;
    if (shadow_kw(t) == SHADOW_KW_CONST) {
        int saved = p->i;
        n = parse_global_arr(p);
        if (n || p->err) return n;
        n = parse_fn_proto(p);
        if (n || p->err) return n;
        /* `const char* name = "…";` / `const int g = 0;` */
        p->i = saved;
        p_next(p); /* const */
        n = parse_global_var(p);
        if (n || p->err) return n;
        p->i = saved;
    }
    /* Host-macro passthrough: CC_DECL_RESULT_SPEC / CC_DECL_SLICE[_SPEC] /
     * CC_MAP_DECL_* (...); — keep as text for host expand via includes. */
    if (t.kind == TK_IDENT &&
        (spell_eq(t.spell, "CC_DECL_RESULT_SPEC") ||
         spell_eq(t.spell, "CC_DECL_SLICE_SPEC") ||
         spell_eq(t.spell, "CC_DECL_SLICE") ||
         spell_eq(t.spell, "CC_MAP_DECL_ARENA") ||
         spell_eq(t.spell, "CC_MAP_DECL_UFCS") ||
         spell_eq(t.spell, "CC_ARRAY_MAP_DECL") ||
         spell_eq(t.spell, "CC_ARRAY_MAP_DECL_UFCS")) &&
        p->i + 1 < p->n && tok_eq(p->toks[p->i + 1], TK_PUNCT, "(")) {
        int a0 = p->i;
        Token kw = p_next(p); /* macro name */
        if (!skip_parens(p)) {
            parser_fail(p, p_peek(p), "unterminated host macro (...)");
            return NULL;
        }
        p_accept(p, TK_PUNCT, ";");
        n = ast_new(p, AST_RAW_LINE);
        if (!n) return NULL;
        if (!span_text(p, a0, p->i, n->a, sizeof(n->a))) {
            parser_fail(p, kw, "host macro span too long");
            return NULL;
        }
        return n;
    }
    /* CC_TYPE_INFO_BEGIN/FIELD/END(...) — no trailing `;` (macro bodies
     * supply braces / commas). Keep as text for host expand. */
    if (t.kind == TK_IDENT &&
        (spell_eq(t.spell, "CC_TYPE_INFO_BEGIN") ||
         spell_eq(t.spell, "CC_TYPE_INFO_FIELD") ||
         spell_eq(t.spell, "CC_TYPE_INFO_END")) &&
        p->i + 1 < p->n && tok_eq(p->toks[p->i + 1], TK_PUNCT, "(")) {
        int a0 = p->i;
        Token kw = p_next(p);
        if (!skip_parens(p)) {
            parser_fail(p, p_peek(p), "unterminated CC_TYPE_INFO_* (...)");
            return NULL;
        }
        n = ast_new(p, AST_RAW_LINE);
        if (!n) return NULL;
        if (!span_text(p, a0, p->i, n->a, sizeof(n->a))) {
            parser_fail(p, kw, "CC_TYPE_INFO_* span too long");
            return NULL;
        }
        return n;
    }
    /* `_Static_assert(…);` — host C11 ICE; passthrough (type_of folded at emit). */
    if (t.kind == TK_IDENT && spell_eq(t.spell, "_Static_assert") &&
        p->i + 1 < p->n && tok_eq(p->toks[p->i + 1], TK_PUNCT, "(")) {
        int a0 = p->i;
        Token kw = p_next(p);
        if (!skip_parens(p)) {
            parser_fail(p, p_peek(p), "unterminated _Static_assert(...)");
            return NULL;
        }
        p_accept(p, TK_PUNCT, ";");
        n = ast_new(p, AST_RAW_LINE);
        if (!n) return NULL;
        if (!span_text(p, a0, p->i, n->a, sizeof(n->a))) {
            parser_fail(p, kw, "_Static_assert span too long");
            return NULL;
        }
        return n;
    }
    /* CC_GENERIC_FACTORY[_EXTEND](Name[, arity]) { … return @emit(`tpl`, arena); } */
    if (t.kind == TK_IDENT &&
        (spell_eq(t.spell, "CC_GENERIC_FACTORY") ||
         spell_eq(t.spell, "CC_GENERIC_FACTORY_EXTEND")) &&
        p->i + 1 < p->n && tok_eq(p->toks[p->i + 1], TK_PUNCT, "(")) {
        int is_extend = spell_eq(t.spell, "CC_GENERIC_FACTORY_EXTEND");
        Token kw = p_next(p);
        if (!p_accept(p, TK_PUNCT, "(")) {
            parser_fail(p, p_peek(p), "expected '(' after CC_GENERIC_FACTORY");
            return NULL;
        }
        Token fam = p_next(p);
        if (fam.kind != TK_IDENT) {
            parser_fail(p, fam, "expected generic family name");
            return NULL;
        }
        char arity[16] = "0";
        if (tok_eq(p_peek(p), TK_PUNCT, ",")) {
            p_next(p);
            Token an = p_next(p);
            if (an.kind != TK_NUM) {
                parser_fail(p, an, "expected arity number");
                return NULL;
            }
            slice_to(arity, sizeof(arity), an.spell);
        }
        if (!p_accept(p, TK_PUNCT, ")")) {
            parser_fail(p, p_peek(p), "expected ')' after CC_GENERIC_FACTORY args");
            return NULL;
        }
        if (!p_accept(p, TK_PUNCT, "{")) {
            parser_fail(p, p_peek(p), "expected '{' after CC_GENERIC_FACTORY(...)");
            return NULL;
        }
        /* Scan body for backtick @emit(`…`); allow preamble stmts before it. */
        Token bt = {0};
        int depth = 1;
        int found_emit = 0;
        while (p->i < p->n && depth > 0) {
            Token cur = p_next(p);
            if (tok_eq(cur, TK_PUNCT, "{")) {
                depth++;
                continue;
            }
            if (tok_eq(cur, TK_PUNCT, "}")) {
                depth--;
                continue;
            }
            if (found_emit) continue;
            if (!tok_eq(cur, TK_PUNCT, "@")) continue;
            if (p->i >= p->n) continue;
            Token em = p->toks[p->i];
            if (em.kind != TK_IDENT || !spell_eq(em.spell, "emit")) continue;
            if (p->i + 1 >= p->n || !tok_eq(p->toks[p->i + 1], TK_PUNCT, "("))
                continue;
            if (p->i + 2 >= p->n) continue;
            Token cand = p->toks[p->i + 2];
            if (cand.kind != TK_STR || !cand.spell.ptr || cand.spell.len < 2 ||
                cand.spell.ptr[0] != '`')
                continue;
            p_next(p); /* emit */
            p_next(p); /* ( */
            bt = p_next(p); /* `…` */
            found_emit = 1;
        }
        if (!found_emit || depth != 0) {
            parser_fail(p, kw, "expected return @emit(`…`) in generic factory");
            return NULL;
        }
        n = ast_new(p, AST_GENERIC_FACTORY);
        if (!n) return NULL;
        slice_to(n->a, sizeof(n->a), fam.spell);
        shadow_parse_gfac_note(n->a);
        snprintf(n->b, sizeof(n->b), "%s", arity);
        snprintf(n->c, sizeof(n->c), "%s", is_extend ? "extend" : "base");
        {
            size_t tn = bt.spell.len >= 2 ? bt.spell.len - 2 : 0;
            if (tn >= sizeof(n->d)) tn = sizeof(n->d) - 1;
            memcpy(n->d, bt.spell.ptr + 1, tn);
            n->d[tn] = 0;
        }
        return n;
    }
    /* `__attribute__((…))` forms (constructor priority, unused, …) — opaque
     * tape span through the following decl / function. Host cc accepts them. */
    if (t.kind == TK_IDENT && spell_eq(t.spell, "__attribute__")) {
        int start = p->i;
        int depth = 0;
        int saw_brace = 0;
        size_t off0, off1;
        Token t0 = p->toks[start];
        while (p->i < p->n) {
            Token cur = p_peek(p);
            if (tok_eq(cur, TK_PUNCT, "{")) {
                depth++;
                saw_brace = 1;
            } else if (tok_eq(cur, TK_PUNCT, "}")) {
                if (depth > 0) depth--;
                p_next(p);
                if (saw_brace && depth == 0) {
                    if (tok_eq(p_peek(p), TK_PUNCT, ";")) p_next(p);
                    break;
                }
                continue;
            } else if (depth == 0 && tok_eq(cur, TK_PUNCT, ";")) {
                p_next(p);
                break;
            }
            p_next(p);
        }
        if (p->i <= start) {
            parser_fail(p, t, "bad __attribute__ declaration");
            return NULL;
        }
        off0 = t0.offset;
        {
            Token last = p->toks[p->i - 1];
            off1 = last.offset + last.spell.len;
        }
        n = ast_new(p, AST_RAW_LINE);
        if (!n) return NULL;
        snprintf(n->e, sizeof(n->e), "tape");
        snprintf(n->a, sizeof(n->a), "%zu", off0);
        snprintf(n->b, sizeof(n->b), "%zu", off1);
        n->file_id = t0.file_id;
        n->tok_off = off0;
        return n;
    }
    parser_fail(p, t, "unexpected token (shadow subset cannot lower this yet)");
    return NULL;
}

/* If lead begins with optional ws + `cmt`, advance past that prefix. */
static void shadow_lead_skip_prefix(Parser* p, AstNode* n, const char* cmt) {
    FileTape* ft;
    size_t i, cl;
    if (!n || !n->lead_len || !cmt || !cmt[0] || !p || !p->cache) return;
    ft = tape_by_id(p->cache, n->file_id);
    if (!ft || !ft->bytes) return;
    cl = strlen(cmt);
    i = n->lead_off;
    while (i < n->lead_off + n->lead_len &&
           (ft->bytes[i] == ' ' || ft->bytes[i] == '\t'))
        i++;
    if (i + cl > n->lead_off + n->lead_len) return;
    if (memcmp(ft->bytes + i, cmt, cl) != 0) return;
    i += cl;
    n->lead_len = (n->lead_off + n->lead_len) - i;
    n->lead_off = i;
}

static AstNode* parse_external(Parser* p) {
    int start;
    AstNode* n;
    if (parse_parallel_pragma_marker(p)) return ast_new(p, AST_RAW_LINE);
    start = p->i;
    n = parse_external_inner(p);
    if (n) {
        shadow_attach_lead(p, n, start);
        /* Forward struct already owns its same-line trailing comment. */
        if (p->ntu > 0) {
            AstNode* prev = p->tu_items[p->ntu - 1];
            if (prev->kind == AST_STRUCT && strcmp(prev->e, "fwd") == 0 && prev->d[0])
                shadow_lead_skip_prefix(p, n, prev->d);
        }
    }
    return n;
}

static void parser_fail_expected_decl(Parser* p) {
    Token t;
    if (!p || p->err) return;
    if (parser_fail_typeish_leftover(p)) return;
    t = p_peek(p);
    if ((t.kind == TK_IDENT || t.kind == TK_PUNCT) && t.spell.len > 0) {
        char msg[192];
        snprintf(msg, sizeof(msg), "expected declaration near '%.*s'",
                 (int)t.spell.len, t.spell.ptr);
        parser_fail(p, t, msg);
        return;
    }
    parser_fail(p, t, "expected declaration");
}

static int parse_tu(Parser* p, AstNode*** out_items, int* out_n) {
    if (!parser_ensure_storage(p)) {
        parser_fail(p, p_peek(p), "out of memory (parser tables)");
        *out_items = NULL;
        *out_n = 0;
        return 0;
    }
    p->nn = 0;
    p->nkstore = 0;
    p->ntu = 0;
    scope_push(p);
    shadow_parse_gfac_reset();
    shadow_parse_ginst_reset();
    /* Comptime fragments already collected (shadow_comptime_exec_file); seed
     * their typedef aliases so later bare uses parse as types. */
    shadow_seed_comptime_emitted_types(p);
    shadow_seed_tool_umbrella_types(p);
    shadow_async_fn_reset();
    shadow_fn_attr_reset();
    spawn_reset_closure_locals();
    while (p_peek(p).kind != TK_EOF && !p->err) {
        AstNode* e = parse_external(p);
        if (!e) {
            if (!p->err && p_peek(p).kind != TK_EOF)
                parser_fail_expected_decl(p);
            break;
        }
        if (p->ntu >= AST_CAP) {
            parser_fail_cap(p, p_peek(p), "TU item table", AST_CAP);
            break;
        }
        p->tu_items[p->ntu++] = e;
    }
    *out_items = p->tu_items;
    *out_n = p->ntu;
    return !p->err && p_peek(p).kind == TK_EOF;
}

static const char* ast_kind_name(AstKind k) {
    switch (k) {
    case AST_TYPEDEF_INT: return "typedef_int";
    case AST_VAR_INT: return "var_int";
    case AST_PTR_DECL: return "ptr_decl";
    case AST_MUL_EXPR: return "mul_expr";
    case AST_EXPR_STMT: return "expr_stmt";
    case AST_STRUCT: return "struct";
    case AST_FIELD_INT: return "field_int";
    case AST_SLICE_VAR: return "slice_var";
    case AST_SLICE_INIT: return "slice_init";
    case AST_CHAN_VAR: return "chan_var";
    case AST_RESULT_FN: return "result_fn";
    case AST_CLOSURE_LIT: return "closure_lit";
    case AST_AT_STMT: return "at_stmt";
    case AST_FN: return "fn";
    case AST_RETURN_INT: return "return_int";
    case AST_RETURN_CC: return "return_cc";
    case AST_RETURN_EXPR: return "return_expr";
    case AST_ERRHANDLER: return "errhandler";
    case AST_PRINTLN_BANG: return "println_bang";
    case AST_PRINTLN_BANG_BIND: return "println_bang_bind";
    case AST_PRINTLN_TPL: return "println_tpl";
    case AST_ERR_FWD: return "err_fwd";
    case AST_ERR_SYNTAX: return "err_syntax";
    case AST_NURSERY_DESTROY: return "nursery_destroy";
    case AST_SPAWN_CLOSURE: return "spawn_closure";
    case AST_CALL_NUM: return "call_num";
    case AST_CALL_ARGS: return "call_args";
    case AST_IF: return "if";
    case AST_BLOCK: return "block";
    case AST_VAR_UNWRAP: return "var_unwrap";
    case AST_PTR_INIT: return "ptr_init";
    case AST_STMT_UNWRAP: return "stmt_unwrap";
    case AST_RESULT_LOCAL: return "result_local";
    case AST_DEFER: return "defer";
    case AST_PTR_UNWRAP: return "ptr_unwrap";
    case AST_VOID_CAST: return "void_cast";
    case AST_STATIC_FN: return "static_fn";
    case AST_STATIC_ARR: return "static_arr";
    case AST_VAL_DESTROY: return "val_destroy";
    case AST_FOR: return "for";
    case AST_TYPED_INIT: return "typed_init";
    case AST_UFCS_STMT: return "ufcs_stmt";
    case AST_UFCS_EXPR: return "ufcs_expr";
    case AST_ASSIGN: return "assign";
    case AST_VAR_DECL: return "var_decl";
    case AST_WHILE: return "while";
    case AST_WITH_DEADLINE: return "with_deadline";
    case AST_PARALLEL: return "parallel";
    case AST_PARALLEL_FOR: return "parallel_for";
    case AST_SERIAL: return "serial";
    case AST_STAGE: return "stage";
    case AST_INC: return "inc";
    case AST_TYPEDEF_STRUCT: return "typedef_struct";
    case AST_FIELD_SIMPLE: return "field_simple";
    case AST_ASYNC_FN: return "async_fn";
    case AST_STATIC_VAR: return "static_var";
    case AST_TYPEDEF_FN_PTR: return "typedef_fn_ptr";
    case AST_FN_PROTO: return "fn_proto";
    case AST_BREAK: return "break";
    case AST_CONTINUE: return "continue";
    case AST_GOTO: return "goto";
    case AST_LABEL: return "label";
    case AST_DO_WHILE: return "do_while";
    case AST_GLOBAL_ARR: return "global_arr";
    case AST_TYPEDEF_ENUM: return "typedef_enum";
    case AST_TYPEDEF_CHAN: return "typedef_chan";
    case AST_RAW_LINE: return "raw_line";
    case AST_GENERIC_FACTORY: return "generic_factory";
    case AST_SWITCH: return "switch";
}
    return "?";
}
