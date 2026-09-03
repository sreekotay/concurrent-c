/* Parse: unwrap / bang-bind / result local / ptr unwrap.
 * Included from pp_ast_parse_stmt.cch (needs parse_block / parse_stmt). */
#pragma once

static int parse_destroy_tail(Parser* p, AstNode* n, char* mode, size_t mode_cap);

/* True when println/eprintln(…) is followed by !> (not bare ';'). */
static int peek_println_has_bang(Parser* p) {
    int j;
    int depth;
    if (!p || p->i >= p->n) return 0;
    if (shadow_kw(p_peek(p)) != SHADOW_KW_PRINTLN &&
        shadow_kw(p_peek(p)) != SHADOW_KW_EPRINTLN)
        return 0;
    if (p->i + 2 >= p->n || !tok_eq(p->toks[p->i + 1], TK_PUNCT, "("))
        return 0;
    j = p->i + 1;
    depth = 0;
    while (j < p->n) {
        Token t = p->toks[j];
        if (tok_eq(t, TK_PUNCT, "(")) depth++;
        else if (tok_eq(t, TK_PUNCT, ")")) {
            depth--;
            if (depth == 0) {
                j++;
                break;
            }
        }
        j++;
    }
    return j < p->n && tok_eq(p->toks[j], TK_PUNCT, "!>");
}

static AstNode* parse_println_bang(Parser* p) {
    ShadowKwKind kw = shadow_kw(p_peek(p));
    if (kw != SHADOW_KW_PRINTLN && kw != SHADOW_KW_EPRINTLN) return NULL;
    int is_eprint = (kw == SHADOW_KW_EPRINTLN);
    p_next(p);
    if (!p_accept(p, TK_PUNCT, "(")) {
        parser_fail(p, p_peek(p), "expected '(' after println");
        return NULL;
    }
    int is_tpl = 0;
    char payload[256];
    char arena[128];
    payload[0] = 0;
    arena[0] = 0;
    if (tok_eq(p_peek(p), TK_PUNCT, "@")) {
        if (!parse_at_string_arena(p, payload, sizeof(payload), arena,
                                   sizeof(arena)))
            return NULL;
        is_tpl = 1;
    } else {
        Token s = p_next(p);
        /* String literal, or CCString/ident expr (cc_println accepts both). */
        if (s.kind != TK_STR && s.kind != TK_IDENT) {
            parser_fail(p, s, "expected string or @string(...) in println(...)");
            return NULL;
        }
        slice_to(payload, sizeof(payload), s.spell);
        /* Adjacent string concat: println("a" "b"); */
        while (s.kind == TK_STR && p_peek(p).kind == TK_STR) {
            Token s2 = p_next(p);
            char piece[128];
            size_t pl = strlen(payload);
            slice_to(piece, sizeof(piece), s2.spell);
            if (pl + 1 + strlen(piece) + 1 < sizeof(payload)) {
                payload[pl++] = ' ';
                snprintf(payload + pl, sizeof(payload) - pl, "%s", piece);
            }
        }
    }
    if (!p_accept(p, TK_PUNCT, ")")) {
        parser_fail(p, p_peek(p), "expected ')' after println arg");
        return NULL;
    }
    if (!p_accept(p, TK_PUNCT, "!>")) {
        parser_fail(p, p_peek(p), "expected '!>' after println(...)");
        return NULL;
    }
    /* !> { stmts } ; — bare handler block (no bind), for both string and tpl. */
    if (tok_eq(p_peek(p), TK_PUNCT, "{")) {
        p_next(p);
        AstNode* n = ast_new(p, is_tpl ? AST_PRINTLN_TPL : AST_PRINTLN_BANG);
        if (!n) return NULL;
        n->a = ast_arena_cstr(p, payload);
        if (is_tpl && arena[0]) n->c = ast_arena_cstr(p, arena);
        if (is_eprint) n->d = ast_arena_cstr(p, "e");
        while (!tok_eq(p_peek(p), TK_PUNCT, "}") && p_peek(p).kind != TK_EOF && !p->err) {
            AstNode* st = parse_stmt(p);
            if (!st) return NULL;
            if (!ast_body_push(p, n, st)) return NULL;
        }
        if (!p_accept(p, TK_PUNCT, "}") || !p_accept(p, TK_PUNCT, ";")) {
            parser_fail(p, p_peek(p), "expected '};' after !> { ... }");
            return NULL;
        }
        return n;
    }
    /* Compose-then-default: !>(e) { ... } ; — string form (hello beachhead) */
    if (tok_eq(p_peek(p), TK_PUNCT, "(")) {
        p_next(p);
        Token bind = p_next(p);
        if (bind.kind != TK_IDENT) {
            parser_fail(p, bind, "expected identifier in '!> (...)'");
            return NULL;
        }
        if (!p_accept(p, TK_PUNCT, ")")) {
            parser_fail(p, p_peek(p), "expected ')' after !>(bind");
            return NULL;
        }
        if (!p_accept(p, TK_PUNCT, "{")) {
            parser_fail(p, p_peek(p), "expected '{' after !>(bind)");
            return NULL;
        }
        AstNode* n = ast_new(p, AST_PRINTLN_BANG_BIND);
        if (!n) return NULL;
        n->a = ast_arena_cstr(p, payload);
        n->b = ast_arena_slice(p, bind.spell);
        while (!tok_eq(p_peek(p), TK_PUNCT, "}") && p_peek(p).kind != TK_EOF && !p->err) {
            AstNode* st = parse_stmt(p);
            if (!st) return NULL;
            if (!ast_body_push(p, n, st)) return NULL;
        }
        if (!p_accept(p, TK_PUNCT, "}")) {
            parser_fail(p, p_peek(p), "expected '}' to close !>(...) handler");
            return NULL;
        }
        if (!p_accept(p, TK_PUNCT, ";")) {
            parser_fail(p, p_peek(p), "expected ';' after !>(...) { ... }");
            return NULL;
        }
        return n;
    }
    if (!p_accept(p, TK_PUNCT, ";")) {
        parser_fail(p, p_peek(p), "expected ';' after println(...) !>");
        return NULL;
    }
    AstNode* n = ast_new(p, is_tpl ? AST_PRINTLN_TPL : AST_PRINTLN_BANG);
    if (!n) return NULL;
    n->a = ast_arena_cstr(p, payload);
    if (is_tpl && arena[0]) n->c = ast_arena_cstr(p, arena);
    if (is_eprint) n->d = ast_arena_cstr(p, "e");
    return n;
}


/* Lvalue ending at `=` at depth 0: `*p`, `p->f`, `p.f`, `a[i]`, name.
 * Rejects `Type name =` (second ident at depth 0). */
static int shadow_unwrap_lvalue_eq(Parser* p, int start) {
    int j = start;
    int depth = 0;
    if (!p || start < 0 || start >= p->n) return -1;
    while (j < p->n && tok_eq(p->toks[j], TK_PUNCT, "*")) j++;
    if (j >= p->n || p->toks[j].kind != TK_IDENT) return -1;
    j++;
    while (j < p->n) {
        Token t = p->toks[j];
        if (tok_eq(t, TK_PUNCT, "[")) {
            depth++;
            j++;
            continue;
        }
        if (tok_eq(t, TK_PUNCT, "]")) {
            if (depth == 0) return -1;
            depth--;
            j++;
            continue;
        }
        if (tok_eq(t, TK_PUNCT, "(") || tok_eq(t, TK_PUNCT, "{")) {
            if (depth == 0) return -1;
            depth++;
            j++;
            continue;
        }
        if (tok_eq(t, TK_PUNCT, ")") || tok_eq(t, TK_PUNCT, "}")) {
            if (depth == 0) return -1;
            depth--;
            j++;
            continue;
        }
        if (depth > 0) {
            j++;
            continue;
        }
        if (tok_eq(t, TK_PUNCT, "=")) return j;
        if (tok_eq(t, TK_PUNCT, ".") || tok_eq(t, TK_PUNCT, "->")) {
            j++;
            if (j >= p->n || p->toks[j].kind != TK_IDENT) return -1;
            j++;
            continue;
        }
        return -1;
    }
    return -1;
}

/* Call (free-name, generic, or UFCS) immediately followed by `!>`.
 * `=` at depth 0 refuses `Type name = f() !>` so decl-unwrap keeps it. */
static int shadow_unwrap_call_bang(Parser* p, int start) {
    int j;
    int depth = 0;
    int saw_call = 0;
    if (!p || start < 0 || start >= p->n) return -1;
    if (p->toks[start].kind != TK_IDENT) return -1;
    for (j = start; j < p->n; j++) {
        Token t = p->toks[j];
        if (tok_eq(t, TK_PUNCT, "(") || tok_eq(t, TK_PUNCT, "[") ||
            tok_eq(t, TK_PUNCT, "{")) {
            depth++;
            continue;
        }
        if (tok_eq(t, TK_PUNCT, ")") || tok_eq(t, TK_PUNCT, "]") ||
            tok_eq(t, TK_PUNCT, "}")) {
            if (depth == 0) return -1;
            if (tok_eq(t, TK_PUNCT, ")") && depth == 1) saw_call = 1;
            depth--;
            continue;
        }
        if (depth > 0) continue;
        if (tok_eq(t, TK_PUNCT, "!>")) return saw_call ? j : -1;
        /* Bare `=` and `;` refuse so decl-unwrap / Form-P assign keep the
         * statement. Compound assigns (`sum += f() !>;`) must also refuse —
         * otherwise `sum += f()` is stolen as the "call" and Form-P never
         * rewrites the bang. */
        if (tok_eq(t, TK_PUNCT, "=") || tok_eq(t, TK_PUNCT, ";") ||
            tok_eq(t, TK_PUNCT, "+=") || tok_eq(t, TK_PUNCT, "-=") ||
            tok_eq(t, TK_PUNCT, "*=") || tok_eq(t, TK_PUNCT, "/=") ||
            tok_eq(t, TK_PUNCT, "%=") || tok_eq(t, TK_PUNCT, "&=") ||
            tok_eq(t, TK_PUNCT, "|=") || tok_eq(t, TK_PUNCT, "^=") ||
            tok_eq(t, TK_PUNCT, "<<=") || tok_eq(t, TK_PUNCT, ">>="))
            return -1;
        if (saw_call && !tok_eq(t, TK_PUNCT, ".") &&
            !tok_eq(t, TK_PUNCT, "->") && !tok_eq(t, TK_PUNCT, "::"))
            return -1;
    }
    return -1;
}

static int unwrap_stmt_bind(Parser* p, AstNode* n, const char* call,
                            const char* lhs, AstNode* ue) {
    if (!n) return 0;
    n->a = ast_arena_cstr(p, call);
    if (lhs && lhs[0]) n->e = ast_arena_cstr(p, lhs);
    if (ue && !ast_attach_ufcs_kid(p, n, ue)) {
        parser_fail(p, p_peek(p), "too many UFCS attachments on unwrap");
        return 0;
    }
    return 1;
}

static AstNode* parse_stmt_unwrap(Parser* p) {
    int save = p->i;
    char lhs[256];
    Token callee;
    int bang;
    int c0;
    char call[2048];
    AstNode* ue = NULL;
    lhs[0] = 0;
    /* `lvalue = call !>` — assign-unwrap (parse_assign defers on `!>`). */
    {
        int eq = shadow_unwrap_lvalue_eq(p, p->i);
        if (eq >= 0) {
            if (eq + 3 < p->n &&
                tok_eq(p->toks[eq + 1], TK_PUNCT, "@") &&
                shadow_kw(p->toks[eq + 2]) == SHADOW_KW_PARALLEL) {
                if (!span_text(p, p->i, eq, lhs, sizeof(lhs))) {
                    parser_fail(p, p_peek(p), "unwrap lvalue too long");
                    return NULL;
                }
                while (p->i < eq) p_next(p);
                p_next(p); /* = */
                return parse_parallel_unwrap_rhs(p, lhs, NULL);
            }
            if (shadow_unwrap_call_bang(p, eq + 1) < 0) return NULL;
            if (!span_text(p, p->i, eq, lhs, sizeof(lhs))) {
                parser_fail(p, p_peek(p), "unwrap lvalue too long");
                return NULL;
            }
            while (p->i < eq) p_next(p);
            p_next(p); /* = */
        }
    }
    callee = p_peek(p);
    bang = shadow_unwrap_call_bang(p, p->i);
    if (bang < 0) {
        p->i = save;
        return NULL;
    }

    c0 = p->i;
    if (!span_text(p, c0, bang, call, sizeof(call))) {
        parser_fail(p, callee, "unwrap call too long");
        return NULL;
    }
    ue = parse_ufcs_expr_range(p, c0, bang);
    while (p->i < bang) p_next(p);
    p_next(p); /* !> or ?> */
    if (tok_eq(p_peek(p), TK_PUNCT, "{")) {
        p_next(p);
        AstNode* n = ast_new(p, AST_STMT_UNWRAP);
        if (!unwrap_stmt_bind(p, n, call, lhs, ue)) return NULL;
        n->c = ast_arena_cstr(p, "bang_block");
        while (!tok_eq(p_peek(p), TK_PUNCT, "}") && p_peek(p).kind != TK_EOF && !p->err) {
            AstNode* st = parse_stmt(p);
            if (!st) return NULL;
            if (!ast_body_push(p, n, st)) return NULL;
        }
        if (!p_accept(p, TK_PUNCT, "}")) {
            parser_fail(p, p_peek(p), "expected '}' after !> { ... }");
            return NULL;
        }
        (void)p_accept(p, TK_PUNCT, ";"); /* optional — nosemi form */
        return n;
    }
    if (tok_eq(p_peek(p), TK_PUNCT, "(")) {
        p_next(p);
        Token bind = p_next(p);
        if (bind.kind != TK_IDENT) {
            parser_fail(p, bind, "expected identifier in '!> (...)'");
            return NULL;
        }
        if (!p_accept(p, TK_PUNCT, ")")) {
            parser_fail(p, p_peek(p), "expected ')' after bind");
            return NULL;
        }
        AstNode* n = ast_new(p, AST_STMT_UNWRAP);
        if (!unwrap_stmt_bind(p, n, call, lhs, ue)) return NULL;
        n->d = ast_arena_slice(p, bind.spell);
        if (tok_eq(p_peek(p), TK_PUNCT, "{")) {
            p_next(p);
            n->c = ast_arena_cstr(p, "bang_block");
            while (!tok_eq(p_peek(p), TK_PUNCT, "}") && p_peek(p).kind != TK_EOF && !p->err) {
                AstNode* st = parse_stmt(p);
                if (!st) return NULL;
                if (!ast_body_push(p, n, st)) return NULL;
            }
            if (!p_accept(p, TK_PUNCT, "}")) {
                parser_fail(p, p_peek(p), "expected '}' after !>(...) { ... }");
                return NULL;
            }
            (void)p_accept(p, TK_PUNCT, ";"); /* optional — nosemi form */
            return n;
        }
        /* Binder requires a body: `!> (e) ;` is ill-formed. */
        if (tok_eq(p_peek(p), TK_PUNCT, ";")) {
            parser_fail(p, p_peek(p), "expected body after '!> (e)'");
            return NULL;
        }
        n->c = ast_arena_cstr(p, "bang_stmt");
        {
            AstNode* st = parse_stmt(p);
            if (!st) return NULL;
            if (!ast_body_push(p, n, st)) return NULL;
        }
        return n;
    }
    if (!tok_eq(p_peek(p), TK_PUNCT, ";")) {
        AstNode* n = ast_new(p, AST_STMT_UNWRAP);
        if (!unwrap_stmt_bind(p, n, call, lhs, ue)) return NULL;
        n->c = ast_arena_cstr(p, "bang_stmt");
        {
            AstNode* st = parse_stmt(p);
            if (!st) return NULL;
            if (!ast_body_push(p, n, st)) return NULL;
        }
        return n;
    }
    p_next(p); /* ; */
    AstNode* n = ast_new(p, AST_STMT_UNWRAP);
    if (!unwrap_stmt_bind(p, n, call, lhs, ue)) return NULL;
    /* Assign form `lvalue = call() !>;` needs bang_stmt so Ok is stored
     * (empty mode falls through to try_call and discards Ok).
     * Bare `call() !>;` must keep mode empty — safety/emit use that for
     * typed @errhandler match, ambiguous @as refuse, and require_eh. */
    if (lhs[0]) n->c = ast_arena_cstr(p, "bang_stmt");
    return n;
}

/* Ok[!>*]!>(Err) name ;  — struct field (no init). */
static AstNode* parse_field_result(Parser* p) {
    char okty[128], errty[128], rname[128];
    Token err, name;
    int j;
    if (!peek_result_shape(p)) return NULL;
    if (p->i + 5 >= p->n) return NULL;
    j = peek_result_ok_type_end(p, p->i);
    if (j < 0) return NULL;
    while (j < p->n && tok_eq(p->toks[j], TK_PUNCT, "*")) j++;
    if (j >= p->n || (!tok_eq(p->toks[j], TK_PUNCT, "!>") &&
                      !tok_eq(p->toks[j], TK_PUNCT, "?>")))
        return NULL;
    j++;
    if (tok_eq(p->toks[j], TK_PUNCT, "(")) {
        j++;
        if (j >= p->n || p->toks[j].kind != TK_IDENT) return NULL;
        j++;
        if (j >= p->n || !tok_eq(p->toks[j], TK_PUNCT, ")")) return NULL;
        j++;
    } else {
        if (j >= p->n || p->toks[j].kind != TK_IDENT) return NULL;
        j++;
    }
    if (j >= p->n || p->toks[j].kind != TK_IDENT) return NULL;
    j++;
    if (j < p->n && tok_eq(p->toks[j], TK_PUNCT, "[")) {
        int depth = 0;
        while (j < p->n) {
            if (tok_eq(p->toks[j], TK_PUNCT, "[")) depth++;
            else if (tok_eq(p->toks[j], TK_PUNCT, "]")) {
                depth--;
                if (depth == 0) {
                    j++;
                    break;
                }
            }
            j++;
        }
    }
    if (j >= p->n || !tok_eq(p->toks[j], TK_PUNCT, ";")) return NULL;
    {
        int ty0 = p->i;
        int ty1 = peek_c_int_type_end(p, ty0);
        if (ty1 > ty0) {
            if (!ast_spell_token_range(p, ty0, ty1, okty, sizeof(okty)) &&
                !span_text(p, ty0, ty1, okty, sizeof(okty)))
                return NULL;
            while (p->i < ty1) p_next(p);
        } else {
            Token ok = p_next(p);
            slice_to(okty, sizeof(okty), ok.spell);
        }
    }
    if (!shadow_parse_result_ok_slice_suffix(p, okty, sizeof(okty))) return NULL;
    if (tok_eq(p_peek(p), TK_PUNCT, "*")) {
        p_next(p);
        size_t al = strlen(okty);
        if (al + 1 < sizeof(okty)) {
            okty[al] = '*';
            okty[al + 1] = 0;
        }
    }
    p_next(p); /* !> or ?> */
    if (tok_eq(p_peek(p), TK_PUNCT, "(")) {
        p_next(p);
        err = p_next(p);
        p_next(p); /* ) */
    } else {
        err = p_next(p);
    }
    name = p_next(p);
    while (tok_eq(p_peek(p), TK_PUNCT, "[")) {
        int depth = 0;
        p_next(p);
        while (p->i < p->n) {
            if (tok_eq(p_peek(p), TK_PUNCT, "[")) depth++;
            else if (tok_eq(p_peek(p), TK_PUNCT, "]")) {
                depth--;
                if (depth < 0) {
                    p_next(p);
                    break;
                }
            }
            p_next(p);
        }
    }
    if (!p_accept(p, TK_PUNCT, ";")) return NULL;
    slice_to(errty, sizeof(errty), err.spell);
    shadow_result_ok_ty_host(okty, sizeof(okty));
    ast_result_name(okty, errty, rname, sizeof(rname));
    AstNode* n = ast_new(p, AST_FIELD_SIMPLE);
    if (!n) return NULL;
    n->a = ast_arena_cstr(p, rname);
    n->b = ast_arena_slice(p, name.spell);
    return n;
}

/* Ok[!>*]!>(Err) name [= init]; — bare decl or initialized local. */
static AstNode* parse_result_local(Parser* p) {
    if (!peek_result_shape(p)) return NULL;
    if (p->i + 5 >= p->n) return NULL;
    /* Must be `Ok [[:][!]] [*] !> Err name [=|;]` not a function (`name (`). */
    int j = peek_result_ok_type_end(p, p->i);
    if (j < 0) return NULL;
    while (j < p->n && tok_eq(p->toks[j], TK_PUNCT, "*")) j++;
    if (j >= p->n || (!tok_eq(p->toks[j], TK_PUNCT, "!>") &&
                      !tok_eq(p->toks[j], TK_PUNCT, "?>")))
        return NULL;
    j++; /* after !> */
    if (tok_eq(p->toks[j], TK_PUNCT, "(")) {
        j++;
        if (j >= p->n || p->toks[j].kind != TK_IDENT) return NULL;
        j++;
        if (j >= p->n || !tok_eq(p->toks[j], TK_PUNCT, ")")) return NULL;
        j++;
    } else {
        if (j >= p->n || p->toks[j].kind != TK_IDENT) return NULL;
        j++;
    }
    if (j >= p->n || p->toks[j].kind != TK_IDENT) return NULL;
    /* Optional array dims: name[expr] — then require `=` or `;`. */
    {
        int after = j + 1;
        if (after < p->n && tok_eq(p->toks[after], TK_PUNCT, "[")) {
            int k = after;
            int depth = 0;
            while (k < p->n) {
                if (tok_eq(p->toks[k], TK_PUNCT, "[")) depth++;
                else if (tok_eq(p->toks[k], TK_PUNCT, "]")) {
                    depth--;
                    if (depth == 0) {
                        k++;
                        break;
                    }
                }
                k++;
            }
            after = k;
        }
        if (after >= p->n) return NULL;
        if (!tok_eq(p->toks[after], TK_PUNCT, "=") &&
            !tok_eq(p->toks[after], TK_PUNCT, ";"))
            return NULL;
    }

    char okty[128];
    {
        int ty0 = p->i;
        int ty1 = peek_c_int_type_end(p, ty0);
        if (ty1 > ty0) {
            if (!ast_spell_token_range(p, ty0, ty1, okty, sizeof(okty)) &&
                !span_text(p, ty0, ty1, okty, sizeof(okty)))
                return NULL;
            while (p->i < ty1) p_next(p);
        } else {
            Token ok = p_next(p);
            slice_to(okty, sizeof(okty), ok.spell);
        }
    }
    if (!shadow_parse_result_ok_slice_suffix(p, okty, sizeof(okty))) return NULL;
    if (tok_eq(p_peek(p), TK_PUNCT, "*")) {
        p_next(p);
        size_t al = strlen(okty);
        if (al + 1 < sizeof(okty)) {
            okty[al] = '*';
            okty[al + 1] = 0;
        }
    }
    p_next(p); /* !> or ?> */
    Token err;
    if (tok_eq(p_peek(p), TK_PUNCT, "(")) {
        p_next(p);
        err = p_next(p);
        p_next(p); /* ) */
    } else {
        err = p_next(p);
    }
    Token name = p_next(p);
    char dims[128] = {0};
    if (tok_eq(p_peek(p), TK_PUNCT, "[")) {
        int ds = p->i;
        int depth = 0;
        p_next(p);
        while (p->i < p->n) {
            if (tok_eq(p_peek(p), TK_PUNCT, "[")) depth++;
            else if (tok_eq(p_peek(p), TK_PUNCT, "]")) {
                depth--;
                if (depth < 0) {
                    if (!span_text(p, ds, p->i + 1, dims, sizeof(dims)))
                        return NULL;
                    p_next(p);
                    break;
                }
            }
            p_next(p);
        }
    }
    AstNode* n = ast_new(p, AST_RESULT_LOCAL);
    if (!n) return NULL;
    n->a = ast_arena_slice(p, name.spell);
    n->b = ast_arena_slice(p, err.spell);
    n->c = ast_arena_cstr(p, okty);
    if (dims[0]) n->e = ast_arena_cstr(p, dims);
    else n->e = NULL;
    if (p_accept(p, TK_PUNCT, ";")) return n; /* bare decl */
    if (!p_accept(p, TK_PUNCT, "=")) {
        parser_fail(p, p_peek(p), "expected '=' or ';' after result local");
        return NULL;
    }
    int c0 = p->i;
    int depth = 0;
    while (p->i < p->n) {
        Token t = p_peek(p);
        if (tok_eq(t, TK_PUNCT, "(") || tok_eq(t, TK_PUNCT, "{") ||
            tok_eq(t, TK_PUNCT, "["))
            depth++;
        else if (tok_eq(t, TK_PUNCT, ")") || tok_eq(t, TK_PUNCT, "}") ||
                 tok_eq(t, TK_PUNCT, "]"))
            depth--;
        else if (depth == 0 && tok_eq(t, TK_PUNCT, ";")) break;
        p_next(p);
    }
    {
        int c1 = p->i;
        AstNode* cl = NULL;
        char init[512];
        int had_cl =
            parse_call_arg_closure(p, c0, c1, init, sizeof(init), "callarg",
                                   &cl);
        if (p->err) return NULL;
        if (had_cl) {
            n->d = ast_arena_cstr(p, init);
            if (cl) {
                spawn_infer_value_caps(p, cl);
                if (!ast_dbody_push(p, n, cl)) return NULL;
            }
            p->i = c1;
        } else {
            AstNode* ue = parse_ufcs_expr_range(p, c0, c1);
            p->i = c1;
            if (!({ char __spell_tmp[4096]; int __spell_ok = ast_spell_token_range(p, c0, c1, __spell_tmp, sizeof(__spell_tmp)); if (__spell_ok) n->d = ast_arena_cstr(p, __spell_tmp); __spell_ok; }) &&
                (((n->d = ast_arena_span(p, c0, c1))), p->err)) {
                parser_fail(p, name, "result local init too long");
                return NULL;
            }
            if (ue) (void)ast_attach_ufcs_kid(p, n, ue);
        }
    }
    if (!p_accept(p, TK_PUNCT, ";")) {
        parser_fail(p, p_peek(p), "expected ';' after result local");
        return NULL;
    }
    return n;
}

/* Type name = <call-expr> !>|?> … ;  (int/size_t/bool/IDENT beachhead) */
/* Consume `.meth()[!>.meth()]*;` into n->e and set mode bang_chain.
 * Keeps any prior handler body/binder for the first hop. Returns 1 on
 * success, 0 on hard fail (n already allocated). */
static int parse_unwrap_ufcs_chain_tail(Parser* p, AstNode* n, Token at) {
    int c0;
    if (!tok_eq(p_peek(p), TK_PUNCT, ".")) return 0;
    c0 = p->i;
    for (;;) {
        p_next(p); /* . */
        if (p_peek(p).kind != TK_IDENT) {
            parser_fail(p, p_peek(p), "expected method after !>");
            return 0;
        }
        p_next(p);
        if (!tok_eq(p_peek(p), TK_PUNCT, "(") || !skip_parens(p)) {
            parser_fail(p, p_peek(p), "expected method call after !>");
            return 0;
        }
        if (tok_eq(p_peek(p), TK_PUNCT, "!>")) {
            p_next(p); /* !> or ?> */
            /* Another hop: !>.meth(…). Terminal !> before ;/@destroy ends chain. */
            if (tok_eq(p_peek(p), TK_PUNCT, ".")) continue;
            break;
        }
        break;
    }
    /* Preserve lhs type: e is overwritten with the chain; park type on mode. */
    char saved_ty[128];
    saved_ty[0] = 0;
    if (n->e && n->e[0])
        snprintf(saved_ty, sizeof(saved_ty), "%s", n->e);
    if ((((n->e = ast_arena_span(p, c0, p->i))), p->err)) {
        parser_fail(p, at, "!> chain too long");
        return 0;
    }
    if (!p_accept(p, TK_PUNCT, ";")) {
        parser_fail(p, p_peek(p), "expected ';' after !> chain");
        return 0;
    }
    if (saved_ty[0])
        do { char __ast_tmp[4096]; snprintf(__ast_tmp, sizeof(__ast_tmp), "bang_chain:%s", saved_ty); n->c = ast_arena_cstr(p, __ast_tmp); } while (0);
    else
        n->c = ast_arena_cstr(p, "bang_chain");
    return 1;
}

static AstNode* parse_var_unwrap(Parser* p) {
    ShadowKwKind tykw = shadow_kw(p_peek(p));
    Token ty = p_peek(p);
    int is_named_ty = (ty.kind == TK_IDENT);
    int name_i;
    int ty0 = p->i;
    int ty1 = -1;
    int nstars = 0;
    int is_slice_sugar = 0;
    char slice_ty[96];
    slice_ty[0] = 0;
    /* Type-position `T[:…] name` — same walker as typed_init (`[:0]`, `[:!]`). */
    {
        int spelled = -1;
        if (ast_try_spell_slice_at(p, ty0, p->n, slice_ty, sizeof(slice_ty),
                                  &spelled) &&
            spelled > 0 && spelled < p->n &&
            p->toks[spelled].kind == TK_IDENT) {
            is_slice_sugar = 1;
            name_i = spelled;
        }
    }
    if (!is_slice_sugar && tykw != SHADOW_KW_INT && tykw != SHADOW_KW_SIZE_T &&
        tykw != SHADOW_KW_BOOL && tykw != SHADOW_KW_CHAR && !is_named_ty)
        return NULL;
    /* Don't steal `int name;` / typedef / fn forms — need `Type name = … !>`.
     * Multiword (`unsigned int` / `long long`) and stars (`int**`) allowed. */
    if (!is_slice_sugar) {
        ty1 = peek_c_int_type_end(p, ty0);
        if (ty1 < 0) ty1 = ty0 + 1;
        name_i = ty1;
        while (name_i < p->n && tok_eq(p->toks[name_i], TK_PUNCT, "*")) {
            nstars++;
            name_i++;
        }
    }
    /* Pointer locals (`T* x = … !>` / `!> {…} @destroy`) stay with
     * parse_ptr_unwrap — it owns the destroy-tail forms. */
    if (nstars > 0) return NULL;
    if (name_i + 2 >= p->n) return NULL;
    if (p->toks[name_i].kind != TK_IDENT) return NULL;
    if (!tok_eq(p->toks[name_i + 1], TK_PUNCT, "=")) return NULL;
    /* Scan for !> or ?> at paren-depth 0 before ';' */
    int j = name_i + 2;
    int depth = 0;
    int op_at = -1;
    while (j < p->n) {
        Token t = p->toks[j];
        if (tok_eq(t, TK_PUNCT, "(") || tok_eq(t, TK_PUNCT, "{") || tok_eq(t, TK_PUNCT, "["))
            depth++;
        else if (tok_eq(t, TK_PUNCT, ")") || tok_eq(t, TK_PUNCT, "}") || tok_eq(t, TK_PUNCT, "]"))
            depth--;
        else if (depth == 0 && (tok_eq(t, TK_PUNCT, "!>") || tok_eq(t, TK_PUNCT, "?>"))) {
            op_at = j;
            break;
        } else if (depth == 0 && tok_eq(t, TK_PUNCT, ";")) {
            return NULL; /* plain init — not in beachhead */
        }
        j++;
    }
    if (op_at < 0) return NULL;
    /* `Type name = expr !> @destroy` — owned by parse_val_destroy. */
    if (tok_eq(p->toks[op_at], TK_PUNCT, "!>") && op_at + 2 < p->n &&
        tok_eq(p->toks[op_at + 1], TK_PUNCT, "@")) {
        ShadowKwKind ak = shadow_kw(p->toks[op_at + 2]);
        if (ak == SHADOW_KW_DESTROY || ak == SHADOW_KW_DETACH) return NULL;
    }

    char tytxt[128];
    if (is_slice_sugar) {
        snprintf(tytxt, sizeof(tytxt), "%s", slice_ty);
        while (p->i < name_i) p_next(p); /* T [ : … ] */
    } else {
        if (ty1 > ty0 + 1 || nstars > 0) {
            if (!ast_spell_token_range(p, ty0, ty1, tytxt, sizeof(tytxt)) &&
                !span_text(p, ty0, ty1, tytxt, sizeof(tytxt))) {
                parser_fail(p, ty, "unwrap lhs type too long");
                return NULL;
            }
            while (p->i < ty1) p_next(p);
            {
                int s;
                for (s = 0; s < nstars; s++) {
                    size_t al = strlen(tytxt);
                    p_next(p); /* * */
                    if (al + 1 < sizeof(tytxt)) {
                        tytxt[al] = '*';
                        tytxt[al + 1] = 0;
                    }
                }
            }
        } else {
            p_next(p); /* type */
            if (tykw == SHADOW_KW_INT) snprintf(tytxt, sizeof(tytxt), "int");
            else if (tykw == SHADOW_KW_SIZE_T)
                snprintf(tytxt, sizeof(tytxt), "size_t");
            else if (tykw == SHADOW_KW_BOOL) snprintf(tytxt, sizeof(tytxt), "bool");
            else slice_to(tytxt, sizeof(tytxt), ty.spell);
        }
    }
    Token vname = p_next(p);
    p_next(p); /* = */
    /* `@parallel {…}` is a Result producer — unwrap dest is this lhs. */
    if (tok_eq(p_peek(p), TK_PUNCT, "@") && p->i + 1 < p->n &&
        shadow_kw(p->toks[p->i + 1]) == SHADOW_KW_PARALLEL) {
        char nm[64];
        slice_to(nm, sizeof(nm), vname.spell);
        return parse_parallel_unwrap_rhs(p, nm, tytxt);
    }
    int call0 = p->i;
    while (p->i < op_at) p_next(p);
    char call[2048];
    /* Spell (rewrites @await async_fn → cc_block_on / Result pack), then
     * fall back to raw span only if the spell buffer overflows. */
    if (!ast_spell_token_range(p, call0, op_at, call, sizeof(call)) &&
        !span_text(p, call0, op_at, call, sizeof(call))) {
        parser_fail(p, vname, "unwrap call expr too long");
        return NULL;
    }
    Token op = p_next(p); /* !> or ?> */
    AstNode* n = ast_new(p, AST_VAR_UNWRAP);
    if (!n) return NULL;
    n->a = ast_arena_slice(p, vname.spell);
    n->b = ast_arena_cstr(p, call);
    /* e carries the lhs type for bang/bang_block (bang_chain overwrites e). */
    n->e = ast_arena_cstr(p, tytxt);

    if (tok_eq(op, TK_PUNCT, "!>")) {
        if (p_accept(p, TK_PUNCT, ";")) {
            n->c = ast_arena_cstr(p, "bang");
            return n;
        }
        /* !>.method(args)[!>.method(args)]*; — unwrap then UFCS hop(s) */
        if (tok_eq(p_peek(p), TK_PUNCT, ".")) {
            if (!parse_unwrap_ufcs_chain_tail(p, n, vname)) return NULL;
            return n;
        }
        if (!p_accept(p, TK_PUNCT, "(")) {
            /* Form-P terminators: `!>,` / `!>)` / infix — expression bang.
             * Multi-declarator trailer `, b = 3;` parks in d (starts with ','). */
            {
                Token pk = p_peek(p);
                int form_p = tok_eq(pk, TK_PUNCT, ",") ||
                             tok_eq(pk, TK_PUNCT, ")") ||
                             tok_eq(pk, TK_PUNCT, "]") ||
                             tok_eq(pk, TK_PUNCT, "<") ||
                             tok_eq(pk, TK_PUNCT, ">") ||
                             tok_eq(pk, TK_PUNCT, "+") ||
                             tok_eq(pk, TK_PUNCT, "-") ||
                             tok_eq(pk, TK_PUNCT, "*") ||
                             tok_eq(pk, TK_PUNCT, "/") ||
                             tok_eq(pk, TK_PUNCT, "%") ||
                             tok_eq(pk, TK_PUNCT, "?") ||
                             (tok_eq(pk, TK_PUNCT, "!") &&
                              p->i + 1 < p->n &&
                              tok_eq(p->toks[p->i + 1], TK_PUNCT, "=")) ||
                             (tok_eq(pk, TK_PUNCT, "=") &&
                              p->i + 1 < p->n &&
                              tok_eq(p->toks[p->i + 1], TK_PUNCT, "=")) ||
                             (tok_eq(pk, TK_PUNCT, "&") &&
                              p->i + 1 < p->n &&
                              tok_eq(p->toks[p->i + 1], TK_PUNCT, "&")) ||
                             (tok_eq(pk, TK_PUNCT, "|") &&
                              p->i + 1 < p->n &&
                              tok_eq(p->toks[p->i + 1], TK_PUNCT, "|"));
                if (form_p) {
                    n->c = ast_arena_cstr(p, "bang");
                    if (tok_eq(pk, TK_PUNCT, ",")) {
                        int t0 = p->i;
                        int depth = 0;
                        while (p->i < p->n) {
                            Token t = p_peek(p);
                            if (tok_eq(t, TK_PUNCT, "(") ||
                                tok_eq(t, TK_PUNCT, "{") ||
                                tok_eq(t, TK_PUNCT, "["))
                                depth++;
                            else if (tok_eq(t, TK_PUNCT, ")") ||
                                     tok_eq(t, TK_PUNCT, "}") ||
                                     tok_eq(t, TK_PUNCT, "]"))
                                depth--;
                            else if (depth == 0 && tok_eq(t, TK_PUNCT, ";"))
                                break;
                            p_next(p);
                        }
                        if ((((n->d = ast_arena_span(p, t0, p->i))), p->err)) {
                            parser_fail(p, pk, "multi-declarator after !> too long");
                            return NULL;
                        }
                        if (!p_accept(p, TK_PUNCT, ";")) {
                            parser_fail(p, p_peek(p),
                                        "expected ';' after multi-declarator bang");
                            return NULL;
                        }
                    }
                    return n;
                }
            }
            /* !> { stmts } [ .chain ]; — bare handler block (no bind). */
            if (tok_eq(p_peek(p), TK_PUNCT, "{")) {
                p_next(p);
                n->c = ast_arena_cstr(p, "bang_block");
                while (!tok_eq(p_peek(p), TK_PUNCT, "}") &&
                       p_peek(p).kind != TK_EOF && !p->err) {
                    AstNode* st = parse_stmt(p);
                    if (!st) return NULL;
                    if (!ast_body_push(p, n, st)) return NULL;
                }
                if (!p_accept(p, TK_PUNCT, "}")) {
                    parser_fail(p, p_peek(p), "expected '}' after !> { ... }");
                    return NULL;
                }
                /* !>{…}.step()!>.twice(); — keep handler, consume chain. */
                if (tok_eq(p_peek(p), TK_PUNCT, ".")) {
                    if (!parse_unwrap_ufcs_chain_tail(p, n, vname)) return NULL;
                    return n;
                }
                /* `!> { … } @destroy;` — same destroy-tail as !>(e){…}. */
                {
                    char mode[80];
                    snprintf(mode, sizeof(mode), "%s", n->c);
                    if (!parse_destroy_tail(p, n, mode, sizeof(mode)))
                        return NULL;
                    n->c = ast_arena_cstr(p, mode);
                    if (strstr(mode, "_D")) return n;
                }
                if (!p_accept(p, TK_PUNCT, ";")) {
                    parser_fail(p, p_peek(p), "expected ';' after !> { ... }");
                    return NULL;
                }
                return n;
            }
            /* !> stmt ; — break/continue/return without binder */
            n->c = ast_arena_cstr(p, "bang_stmt");
            AstNode* st = parse_stmt(p);
            if (!st) return NULL;
            if (!ast_body_push(p, n, st)) return NULL;
            return n;
        }
        Token bind = p_next(p);
        if (bind.kind != TK_IDENT) {
            parser_fail(p, bind, "expected bind name");
            return NULL;
        }
        if (!p_accept(p, TK_PUNCT, ")")) {
            parser_fail(p, p_peek(p), "expected ')' after bind");
            return NULL;
        }
        n->d = ast_arena_slice(p, bind.spell);
        if (tok_eq(p_peek(p), TK_PUNCT, "{")) {
            p_next(p);
            n->c = ast_arena_cstr(p, "bang_block");
            while (!tok_eq(p_peek(p), TK_PUNCT, "}") && p_peek(p).kind != TK_EOF && !p->err) {
                AstNode* st = parse_stmt(p);
                if (!st) return NULL;
                if (!ast_body_push(p, n, st)) return NULL;
            }
            if (!p_accept(p, TK_PUNCT, "}")) {
                parser_fail(p, p_peek(p), "expected '}' after !>(...) { ... }");
                return NULL;
            }
            /* !>(e){…}.step()!>.twice(); — do not stop before the chain. */
            if (tok_eq(p_peek(p), TK_PUNCT, ".")) {
                if (!parse_unwrap_ufcs_chain_tail(p, n, vname)) return NULL;
                return n;
            }
            /* `!>(e){…} @destroy;` — destroy-tail must not be left for the
             * next stmt (CCPyObj ownership / unwrap_destroy smokes). */
            {
                char mode[80];
                char bind[2048];
                snprintf(mode, sizeof(mode), "%s", n->c);
                snprintf(bind, sizeof(bind), "%s", n->d);
                if (!parse_destroy_tail(p, n, mode, sizeof(mode))) return NULL;
                n->c = ast_arena_cstr(p, mode);
                n->d = ast_arena_cstr(p, bind);
                if (strstr(mode, "_D")) return n; /* consumed `;` */
            }
            if (!p_accept(p, TK_PUNCT, ";")) {
                parser_fail(p, p_peek(p), "expected ';' after !>(...) { ... }");
                return NULL;
            }
            return n;
        }
        /* !>(e) stmt ; */
        n->c = ast_arena_cstr(p, "bang_stmt");
        AstNode* st = parse_stmt(p);
        if (!st) return NULL;
        if (!ast_body_push(p, n, st)) return NULL;
        return n;
    }

    /* ?> — binder is `?>(ident)` only; `?> (expr)` is a paren default. */
    if (tok_eq(p_peek(p), TK_PUNCT, "(") && p->i + 1 < p->n &&
        tok_eq(p->toks[p->i + 1], TK_PUNCT, ")")) {
        parser_fail(p, p_peek(p), "expected identifier in '?>(...)'");
        return NULL;
    }
    if (tok_eq(p_peek(p), TK_PUNCT, "(") && p->i + 2 < p->n &&
        tok_eq(p->toks[p->i + 2], TK_PUNCT, ")")) {
        if (p->toks[p->i + 1].kind != TK_IDENT) {
            parser_fail(p, p->toks[p->i + 1], "expected identifier in '?>(...)'");
            return NULL;
        }
        p_next(p);
        Token bind = p_next(p);
        p_next(p); /* ) */
        n->d = ast_arena_slice(p, bind.spell);
        /* Preserve lhs type: mode `qmark_bind:Type` (e holds default). */
        do { char __ast_tmp[4096]; snprintf(__ast_tmp, sizeof(__ast_tmp), "qmark_bind:%s", tytxt); n->c = ast_arena_cstr(p, __ast_tmp); } while (0);
    } else {
        n->c = ast_arena_cstr(p, "qmark");
        n->d = ast_arena_cstr(p, tytxt);
    }
    int d0 = p->i;
    depth = 0;
    while (p->i < p->n) {
        Token t = p_peek(p);
        if (tok_eq(t, TK_PUNCT, "(") || tok_eq(t, TK_PUNCT, "{") || tok_eq(t, TK_PUNCT, "["))
            depth++;
        else if (tok_eq(t, TK_PUNCT, ")") || tok_eq(t, TK_PUNCT, "}") || tok_eq(t, TK_PUNCT, "]"))
            depth--;
        else if (depth == 0 && tok_eq(t, TK_PUNCT, ";")) break;
        p_next(p);
    }
    if (d0 == p->i) {
        parser_fail(p, p_peek(p), "missing default expression after '?>'");
        return NULL;
    }
    if ((((n->e = ast_arena_span(p, d0, p->i))), p->err)) {
        parser_fail(p, p_peek(p), "?> default expr too long");
        return NULL;
    }
    {
        const char* rhs = n->e;
        while (*rhs == ' ' || *rhs == '\t') rhs++;
        if (rhs[0] == '{' ||
            strncmp(rhs, "continue", 8) == 0 ||
            strncmp(rhs, "break", 5) == 0 ||
            strncmp(rhs, "return", 6) == 0) {
            parser_fail(p, op,
                        "'?>' RHS must be a value expression; use '!>' for "
                        "error-handling logic");
            return NULL;
        }
    }
    if (!p_accept(p, TK_PUNCT, ";")) {
        parser_fail(p, p_peek(p), "expected ';' after ?> default");
        return NULL;
    }
    return n;
}

/* Type * name = callee ( NULL ) !> @destroy { stmts } ; */

static int parse_destroy_tail(Parser* p, AstNode* n, char* mode, size_t mode_cap) {
    if (!tok_eq(p_peek(p), TK_PUNCT, "@")) return 1; /* no destroy */
    if (p->i + 1 >= p->n || shadow_kw(p->toks[p->i + 1]) != SHADOW_KW_DESTROY)
        return 1;
    p_next(p); /* @ */
    p_next(p); /* destroy */
    if (p_accept(p, TK_PUNCT, ";")) {
        size_t m = strlen(mode);
        if (m + 6 < mode_cap) snprintf(mode + m, mode_cap - m, "_Dbare");
        n->d = ast_arena_cstr(p, mode);
        return 1;
    }
    if (!p_accept(p, TK_PUNCT, "{")) {
        parser_fail(p, p_peek(p), "expected ';' or '{' after @destroy");
        return 0;
    }
    while (!tok_eq(p_peek(p), TK_PUNCT, "}") && p_peek(p).kind != TK_EOF && !p->err) {
        AstNode* s = parse_stmt(p);
        if (!s) return 0;
        if (!ast_dbody_push(p, n, s)) return 0;
    }
    if (!p_accept(p, TK_PUNCT, "}") || !p_accept(p, TK_PUNCT, ";")) {
        parser_fail(p, p_peek(p), "expected '};' after @destroy { ... }");
        return 0;
    }
    {
        size_t m = strlen(mode);
        if (m + 2 < mode_cap) snprintf(mode + m, mode_cap - m, "_D");
        n->d = ast_arena_cstr(p, mode);
    }
    return 1;
}

/* [const] Type *[*…] name = call(...) !>|?> … [@destroy …] ; */
static AstNode* parse_ptr_unwrap(Parser* p) {
    int has_const = 0;
    int ti = p->i;
    int nstars = 0;
    int name_i;
    Token ty;
    if (shadow_kw(p_peek(p)) == SHADOW_KW_CONST) {
        has_const = 1;
        ti++;
        if (ti >= p->n) return NULL;
    }
    ty = p->toks[ti];
    if (ty.kind != TK_IDENT && shadow_kw(ty) != SHADOW_KW_CHAR &&
        shadow_kw(ty) != SHADOW_KW_INT && shadow_kw(ty) != SHADOW_KW_VOID &&
        shadow_kw(ty) != SHADOW_KW_BOOL)
        return NULL;
    name_i = ti + 1;
    while (name_i < p->n && tok_eq(p->toks[name_i], TK_PUNCT, "*")) {
        nstars++;
        name_i++;
    }
    if (nstars < 1 || name_i + 2 >= p->n) return NULL;
    if (p->toks[name_i].kind != TK_IDENT) return NULL;
    if (!tok_eq(p->toks[name_i + 1], TK_PUNCT, "=")) return NULL;
    int j = name_i + 2;
    int depth = 0;
    int op_at = -1;
    while (j < p->n) {
        Token t = p->toks[j];
        if (tok_eq(t, TK_PUNCT, "(") || tok_eq(t, TK_PUNCT, "{") || tok_eq(t, TK_PUNCT, "["))
            depth++;
        else if (tok_eq(t, TK_PUNCT, ")") || tok_eq(t, TK_PUNCT, "}") || tok_eq(t, TK_PUNCT, "]"))
            depth--;
        else if (depth == 0 && (tok_eq(t, TK_PUNCT, "!>") || tok_eq(t, TK_PUNCT, "?>"))) {
            op_at = j;
            break;
        } else if (depth == 0 && tok_eq(t, TK_PUNCT, ";")) {
            return NULL;
        }
        j++;
    }
    if (op_at < 0) return NULL;

    if (has_const) p_next(p); /* const */
    p_next(p); /* type */
    {
        int s;
        for (s = 0; s < nstars; s++) p_next(p); /* *… */
    }
    Token name = p_next(p);
    p_next(p); /* = */
    int c0 = p->i;
    while (p->i < op_at) p_next(p);
    char call[2048];
    if (!span_text(p, c0, op_at, call, sizeof(call))) {
        parser_fail(p, name, "ptr unwrap call too long");
        return NULL;
    }
    Token op = p_next(p);
    AstNode* n = ast_new(p, AST_PTR_UNWRAP);
    if (!n) return NULL;
    {
        /* Emit always appends one `*` for AST_PTR_UNWRAP — store the pointee
         * side only (`CCChan` / `int*`), not the full declarator type. */
        char tbuf[64];
        char stars[8];
        int s;
        size_t si = 0;
        slice_to(tbuf, sizeof(tbuf), ty.spell);
        for (s = 0; s < nstars - 1 && si + 1 < sizeof(stars); s++)
            stars[si++] = '*';
        stars[si] = 0;
        if (has_const)
            do { char __ast_tmp[4096]; snprintf(__ast_tmp, sizeof(__ast_tmp), "const %s%s", tbuf, stars); n->a = ast_arena_cstr(p, __ast_tmp); } while (0);
        else
            do { char __ast_tmp[4096]; snprintf(__ast_tmp, sizeof(__ast_tmp), "%s%s", tbuf, stars); n->a = ast_arena_cstr(p, __ast_tmp); } while (0);
    }
    n->b = ast_arena_slice(p, name.spell);
    n->c = ast_arena_cstr(p, call);
    char mode[64];
    mode[0] = 0;

    if (tok_eq(op, TK_PUNCT, "!>")) {
        if (tok_eq(p_peek(p), TK_PUNCT, "@")) {
            /* !> @destroy … — handler = errhandler */
            snprintf(mode, sizeof(mode), "bang_eh");
            n->d = ast_arena_cstr(p, mode);
            if (!parse_destroy_tail(p, n, mode, sizeof(mode))) return NULL;
            return n;
        }
        if (tok_eq(p_peek(p), TK_PUNCT, "(")) {
            p_next(p);
            Token bind = p_next(p);
            if (bind.kind != TK_IDENT) {
                parser_fail(p, bind, "expected bind in !>(...)");
                return NULL;
            }
            if (!p_accept(p, TK_PUNCT, ")")) {
                parser_fail(p, p_peek(p), "expected ')' after bind");
                return NULL;
            }
            n->e = ast_arena_slice(p, bind.spell);
            snprintf(mode, sizeof(mode), "bang_block");
        } else {
            snprintf(mode, sizeof(mode), "bang_nobind");
        }
        if (tok_eq(p_peek(p), TK_PUNCT, "{")) {
            p_next(p);
            while (!tok_eq(p_peek(p), TK_PUNCT, "}") && p_peek(p).kind != TK_EOF && !p->err) {
                AstNode* st = parse_stmt(p);
                if (!st) return NULL;
                if (!ast_body_push(p, n, st)) return NULL;
            }
            if (!p_accept(p, TK_PUNCT, "}")) {
                parser_fail(p, p_peek(p), "expected '}' after !> handler");
                return NULL;
            }
        } else if (strcmp(mode, "bang_nobind") == 0) {
            /* `!>;` — empty handler (errhandler / abort path). */
            if (p_accept(p, TK_PUNCT, ";")) {
                n->d = ast_arena_cstr(p, mode);
                return n;
            }
            /* !> stmt ;  — single stmt handler without braces */
            AstNode* st = parse_stmt(p);
            if (!st) return NULL;
            if (!ast_body_push(p, n, st)) return NULL;
            n->d = ast_arena_cstr(p, mode);
            return n;
        }
        n->d = ast_arena_cstr(p, mode);
        if (p_accept(p, TK_PUNCT, ";")) return n;
        if (!tok_eq(p_peek(p), TK_PUNCT, "@")) {
            parser_fail(p, p_peek(p), "expected ';' or @destroy after !> handler");
            return NULL;
        }
        if (!parse_destroy_tail(p, n, mode, sizeof(mode))) return NULL;
        return n;
    }

    /* ?> — binder is `?>(ident)` only; `?> (expr)` is a paren default. */
    if (tok_eq(p_peek(p), TK_PUNCT, "(") && p->i + 1 < p->n &&
        tok_eq(p->toks[p->i + 1], TK_PUNCT, ")")) {
        parser_fail(p, p_peek(p), "expected identifier in '?>(...)'");
        return NULL;
    }
    if (tok_eq(p_peek(p), TK_PUNCT, "(") && p->i + 2 < p->n &&
        tok_eq(p->toks[p->i + 2], TK_PUNCT, ")")) {
        if (p->toks[p->i + 1].kind != TK_IDENT) {
            parser_fail(p, p->toks[p->i + 1], "expected identifier in '?>(...)'");
            return NULL;
        }
        p_next(p);
        Token bind = p_next(p);
        p_next(p); /* ) */
        {
            char bindname[64];
            slice_to(bindname, sizeof(bindname), bind.spell);
            snprintf(mode, sizeof(mode), "qmark_bind:%s", bindname);
        }
    } else {
        snprintf(mode, sizeof(mode), "qmark");
    }
    int d0 = p->i;
    depth = 0;
    while (p->i < p->n) {
        Token t = p_peek(p);
        if (tok_eq(t, TK_PUNCT, "(") || tok_eq(t, TK_PUNCT, "{") || tok_eq(t, TK_PUNCT, "["))
            depth++;
        else if (tok_eq(t, TK_PUNCT, ")") || tok_eq(t, TK_PUNCT, "}") || tok_eq(t, TK_PUNCT, "]"))
            depth--;
        else if (depth == 0 && (tok_eq(t, TK_PUNCT, ";") || tok_eq(t, TK_PUNCT, "@")))
            break;
        p_next(p);
    }
    if (d0 == p->i) {
        parser_fail(p, p_peek(p), "missing default expression after '?>'");
        return NULL;
    }
    if ((((n->e = ast_arena_span(p, d0, p->i))), p->err)) {
        parser_fail(p, p_peek(p), "?> default too long");
        return NULL;
    }
    {
        const char* rhs = n->e;
        while (*rhs == ' ' || *rhs == '\t') rhs++;
        if (rhs[0] == '{' ||
            strncmp(rhs, "continue", 8) == 0 ||
            strncmp(rhs, "break", 5) == 0 ||
            strncmp(rhs, "return", 6) == 0) {
            parser_fail(p, op,
                        "'?>' RHS must be a value expression; use '!>' for "
                        "error-handling logic");
            return NULL;
        }
    }
    n->d = ast_arena_cstr(p, mode);
    if (p_accept(p, TK_PUNCT, ";")) return n;
    if (!tok_eq(p_peek(p), TK_PUNCT, "@")) {
        parser_fail(p, p_peek(p), "expected ';' or @destroy after ?> default");
        return NULL;
    }
    if (!parse_destroy_tail(p, n, mode, sizeof(mode))) return NULL;
    return n;
}
