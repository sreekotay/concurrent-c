/* Parse: legacy @err / =<! err syntax (pass_err_syntax beachhead).
 * Included from pp_ast_parse_stmt.cch. */
#pragma once

static int parse_err_is_fwd_at(Parser* p, int at) {
    if (at + 2 >= p->n || !tok_eq(p->toks[at], TK_PUNCT, "@")) return 0;
    if (shadow_kw(p->toks[at + 1]) != SHADOW_KW_ERR) return 0;
    if (at + 2 >= p->n || !tok_eq(p->toks[at + 2], TK_PUNCT, "(")) return 0;
    int k = at + 3;
    int par = 1;
    while (k < p->n && par > 0) {
        if (tok_eq(p->toks[k], TK_PUNCT, "(")) par++;
        else if (tok_eq(p->toks[k], TK_PUNCT, ")")) par--;
        k++;
    }
    return k < p->n && tok_eq(p->toks[k], TK_PUNCT, ";");
}

static int parse_err_suffix(Parser* p, AstNode* n) {
    if (!p_accept(p, TK_PUNCT, "@")) return 0;
    if (p->i >= p->n || shadow_kw(p_peek(p)) != SHADOW_KW_ERR) return 0;
    p_next(p); /* err */
    if (tok_eq(p_peek(p), TK_PUNCT, "(")) {
        p_next(p);
        Token t1 = p_next(p);
        if (t1.kind != TK_IDENT) {
            parser_fail(p, t1, "expected bind name in @err(...)");
            return 0;
        }
        if (tok_eq(p_peek(p), TK_PUNCT, ")")) {
            n->d = ast_arena_slice(p, t1.spell);
            p_next(p);
        } else {
            Token bind = p_next(p);
            if (bind.kind != TK_IDENT) {
                parser_fail(p, bind, "expected bind name in @err(Type name)");
                return 0;
            }
            if (!p_accept(p, TK_PUNCT, ")")) {
                parser_fail(p, p_peek(p), "expected ')' after @err(...)");
                return 0;
            }
            do { char __ast_tmp[4096]; snprintf(__ast_tmp, sizeof(__ast_tmp), "%.*s %.*s",
                     (int)t1.spell.len, t1.spell.ptr,
                     (int)bind.spell.len, bind.spell.ptr); n->d = ast_arena_cstr(p, __ast_tmp); } while (0);
        }
    }
    if (tok_eq(p_peek(p), TK_PUNCT, "{")) {
        p_next(p);
        while (!tok_eq(p_peek(p), TK_PUNCT, "}") && p_peek(p).kind != TK_EOF &&
               !p->err) {
            AstNode* st = parse_stmt(p);
            if (!st) return 0;
            if (!ast_body_push(p, n, st)) return 0;
        }
        if (!p_accept(p, TK_PUNCT, "}")) {
            parser_fail(p, p_peek(p), "expected '}' after @err { ... }");
            return 0;
        }
    } else if (!tok_eq(p_peek(p), TK_PUNCT, ";")) {
        AstNode* st = parse_stmt(p);
        if (!st) return 0;
        if (!ast_body_push(p, n, st)) return 0;
    }
    if (tok_eq(p_peek(p), TK_PUNCT, ";"))
        p_next(p);
    else if (n->nbody == 0) {
        parser_fail(p, p_peek(p), "expected ';' after @err");
        return 0;
    }
    return 1;
}

/* Statement containing postfix `@err` or `=<! … @err`. */
static AstNode* parse_err_syntax_stmt(Parser* p) {
    int start = p->i;
    int j = start;
    int depth = 0;
    int eq_lt_bang = -1;
    int err_at = -1;
    while (j < p->n) {
        Token t = p->toks[j];
        if (tok_eq(t, TK_PUNCT, "(") || tok_eq(t, TK_PUNCT, "{") ||
            tok_eq(t, TK_PUNCT, "["))
            depth++;
        else if (tok_eq(t, TK_PUNCT, ")") || tok_eq(t, TK_PUNCT, "}") ||
                 tok_eq(t, TK_PUNCT, "]"))
            depth--;
        else if (depth == 0) {
            if (tok_eq(t, TK_PUNCT, "=<!"))
                eq_lt_bang = j;
            if (tok_eq(t, TK_PUNCT, "@") && j + 1 < p->n &&
                shadow_kw(p->toks[j + 1]) == SHADOW_KW_ERR &&
                !parse_err_is_fwd_at(p, j))
                err_at = j;
            if (tok_eq(t, TK_PUNCT, ";"))
                break;
        }
        j++;
    }
    if (err_at < 0 || j >= p->n || !tok_eq(p->toks[j], TK_PUNCT, ";"))
        return NULL;
    if (eq_lt_bang >= 0 && eq_lt_bang > err_at)
        return NULL;

    AstNode* n = ast_new(p, AST_ERR_SYNTAX);
    if (!n) return NULL;

    int expr_a, expr_b;
    if (eq_lt_bang >= 0) {
        if ((((n->a = ast_arena_span(p, start, eq_lt_bang))), p->err)) {
            parser_fail(p, p->toks[start], "=<! lhs too long");
            return NULL;
        }
        p->i = eq_lt_bang + 1; /* =<! */
        expr_a = p->i;
        int k = expr_a;
        depth = 0;
        int colon = -1;
        while (k < err_at) {
            Token t = p->toks[k];
            if (tok_eq(t, TK_PUNCT, "(") || tok_eq(t, TK_PUNCT, "{") ||
                tok_eq(t, TK_PUNCT, "["))
                depth++;
            else if (tok_eq(t, TK_PUNCT, ")") || tok_eq(t, TK_PUNCT, "}") ||
                     tok_eq(t, TK_PUNCT, "]"))
                depth--;
            else if (depth == 0 && tok_eq(t, TK_PUNCT, ":"))
                colon = k;
            k++;
        }
        if (colon >= 0) {
            if ((((n->b = ast_arena_span(p, expr_a, colon))), p->err) ||
                (((n->e = ast_arena_span(p, colon + 1, err_at))), p->err)) {
                parser_fail(p, p->toks[expr_a], "=<! expr/default too long");
                return NULL;
            }
            n->c = ast_arena_cstr(p, "colon");
        } else {
            if ((((n->b = ast_arena_span(p, expr_a, err_at))), p->err)) {
                parser_fail(p, p->toks[expr_a], "=<! expr too long");
                return NULL;
            }
        }
        expr_b = err_at;
    } else {
        n->a = NULL;
        expr_a = start;
        expr_b = err_at;
        if ((((n->b = ast_arena_span(p, expr_a, expr_b))), p->err)) {
            parser_fail(p, p->toks[start], "@err expr too long");
            return NULL;
        }
    }

    p->i = err_at;
    if (!parse_err_suffix(p, n))
        return NULL;
    return n;
}
