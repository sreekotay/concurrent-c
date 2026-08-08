/* Statement / expression parsers (called from parse_stmt and externals).
 * Requires pp_ast_core.cch. */
#pragma once

static int try_parse_spawn_cond(Parser* p, int c0, int c1, AstNode** out_sp,
                                int* out_after);
static AstNode* parse_stmt(Parser* p);
static AstNode* parse_block(Parser* p);
static AstNode* parse_ufcs_expr_range(Parser* p, int i0, int i1);
static int ast_attach_ufcs_kid(Parser* p, AstNode* parent, AstNode* ue);
static int parse_call_arg_closure(Parser* p, int c0, int end, char* dst,
                                  size_t cap, const char* tag,
                                  AstNode** out_cl);
static void spawn_infer_value_caps(AstNode* n);

/* int[:] Name ; — soft-miss when followed by `=` (typed/slice init owns it). */
static AstNode* parse_slice_var(Parser* p) {
    int saved = p->i;
    if (!peek_int_base(p)) return NULL;
    if (p->i + 5 >= p->n) return NULL;
    if (!tok_eq(p->toks[p->i + 1], TK_PUNCT, "[") ||
        !tok_eq(p->toks[p->i + 2], TK_PUNCT, ":") ||
        !tok_eq(p->toks[p->i + 3], TK_PUNCT, "]"))
        return NULL;
    if (p->i + 5 < p->n && !tok_eq(p->toks[p->i + 5], TK_PUNCT, ";")) {
        /* `int[:] name = …` / other forms — not this beachhead. */
        return NULL;
    }
    p_next(p); p_next(p); p_next(p); p_next(p);
    Token name = p_next(p);
    if (name.kind != TK_IDENT) {
        p->i = saved;
        return NULL;
    }
    if (!p_accept(p, TK_PUNCT, ";")) {
        p->i = saved;
        return NULL;
    }
    AstNode* n = ast_new(p, AST_SLICE_VAR);
    if (!n) return NULL;
    slice_to(n->a, sizeof(n->a), name.spell);
    return n;
}

/* `T name[:] = {…};` or `T[:] name = {…};` — typed slice brace literal.
 * Soft-miss when RHS is not `{…}` so string/empty inits fall to typed_init. */
static AstNode* parse_slice_init(Parser* p) {
    int ty0 = p->i;
    int ty1;
    int name_i;
    int mend;
    int unique = 0;
    int type_pos = 0;
    Token name;
    char elem[64];
    if (p->i + 6 >= p->n) return NULL;
    ty1 = peek_slice_elem_type_end(p, ty0, p->n);
    if (ty1 < 0) return NULL;
    /* Type-position: T[:] name = { */
    if (ty1 < p->n && tok_eq(p->toks[ty1], TK_PUNCT, "[")) {
        mend = peek_slice_brack_end(p, ty1, p->n, &unique);
        if (mend < 0 || mend >= p->n || p->toks[mend].kind != TK_IDENT)
            return NULL;
        name_i = mend;
        mend = name_i + 1;
        if (mend >= p->n || !tok_eq(p->toks[mend], TK_PUNCT, "=")) return NULL;
        type_pos = 1;
    } else {
        /* Declarator: T name[:] = { */
        name_i = ty1;
        if (name_i >= p->n || p->toks[name_i].kind != TK_IDENT) return NULL;
        if (name_i + 1 >= p->n || !tok_eq(p->toks[name_i + 1], TK_PUNCT, "["))
            return NULL;
        mend = peek_slice_brack_end(p, name_i + 1, p->n, &unique);
        if (mend < 0 || mend >= p->n || !tok_eq(p->toks[mend], TK_PUNCT, "="))
            return NULL;
    }
    /* Non-brace RHS (string lit, call, …) — let parse_typed_init own it. */
    if (mend + 1 >= p->n || !tok_eq(p->toks[mend + 1], TK_PUNCT, "{"))
        return NULL;
    if (type_pos) {
        while (p->i < name_i) p_next(p);
        name = p_next(p);
    } else {
        while (p->i < name_i) p_next(p);
        name = p_next(p);
        while (p->i < mend) p_next(p);
    }
    p_next(p); /* = */
    {
        int b0 = p->i;
        int depth = 0;
        while (p->i < p->n) {
            Token t = p_peek(p);
            if (tok_eq(t, TK_PUNCT, "{")) depth++;
            else if (tok_eq(t, TK_PUNCT, "}")) {
                depth--;
                p_next(p);
                if (depth == 0) break;
                continue;
            }
            p_next(p);
        }
        if (depth != 0) {
            parser_fail(p, p_peek(p), "unterminated slice init");
            return NULL;
        }
        {
            char init[256];
            AstNode* n;
            if (!span_text(p, b0, p->i, init, sizeof(init))) {
                parser_fail(p, name, "slice init too long");
                return NULL;
            }
            if (!p_accept(p, TK_PUNCT, ";")) {
                parser_fail(p, p_peek(p), "expected ';' after slice init");
                return NULL;
            }
            n = ast_new(p, AST_SLICE_INIT);
            if (!n) return NULL;
            if (shadow_kw(p->toks[ty0]) == SHADOW_KW_CHAR && ty1 == ty0 + 1)
                snprintf(n->a, sizeof(n->a), "char");
            else {
                /* Keep C spelling (spaces) for the buffer type; mangle for
                 * CCSlice_* lives in shadow_slice_ty. */
                if (!ast_spell_token_range(p, ty0, ty1, elem, sizeof(elem)) &&
                    !span_text(p, ty0, ty1, elem, sizeof(elem)))
                    ast_mangle_slice_elem(p, ty0, ty1, elem, sizeof(elem));
                snprintf(n->a, sizeof(n->a), "%s", elem);
            }
            slice_to(n->b, sizeof(n->b), name.spell);
            snprintf(n->c, sizeof(n->c), "%s", init);
            if (unique) snprintf(n->e, sizeof(n->e), "!");
            return n;
        }
    }
}

/* Elem[~[Cap] [ordered|sched|sync|Drop…]* Dir [, Drop…] ] name;
 * Cap is NUM or IDENT (default 0). ordered/schedule either order.
 * Also `T[:][~…]`, `Ok!>(Err)[~…]`, and `T[~N owned {…}] name;`. */
static AstNode* parse_chan_var(Parser* p) {
    Token ety = p_peek(p);
    ShadowKwKind ekw = shadow_kw(ety);
    int bi;
    int di;
    int ordered = 0;
    int is_ptr = 0;
    int is_slice = 0;
    int is_result = 0;
    int is_sync = 0;
    int is_owned = 0;
    int bp_mode = 0; /* 0 block, 1 Drop/DropNew, 2 DropOld */
    int owned_brace = -1;
    int owned_end = -1;
    char cap_txt[32];
    char topo_txt[32];
    char elem_txt[64];
    char ok_txt[32];
    char err_txt[32];
    char chan_init[256];
    Token dir;
    Token dtok;
    Token name;
    AstNode* n;
    topo_txt[0] = 0;
    ok_txt[0] = err_txt[0] = 0;
    chan_init[0] = 0;
    if (ety.kind != TK_IDENT && ekw != SHADOW_KW_INT && ekw != SHADOW_KW_CHAR &&
        ekw != SHADOW_KW_BOOL && ekw != SHADOW_KW_SIZE_T && ekw != SHADOW_KW_VOID)
        return NULL;
    if (p->i + 5 >= p->n) return NULL;
    /* Result element: `Ok!>(Err)` / `Ok !>(Err)` before `[~`. */
    bi = p->i + 1;
    if (bi < p->n && tok_eq(p->toks[bi], TK_PUNCT, "*")) {
        is_ptr = 1;
        bi++;
    }
    if (bi < p->n && tok_eq(p->toks[bi], TK_PUNCT, "!>")) {
        int ej = bi + 1;
        is_result = 1;
        slice_to(ok_txt, sizeof(ok_txt), ety.spell);
        if (is_ptr) {
            size_t ol = strlen(ok_txt);
            if (ol + 1 < sizeof(ok_txt)) {
                ok_txt[ol] = '*';
                ok_txt[ol + 1] = 0;
            }
        }
        if (ej < p->n && tok_eq(p->toks[ej], TK_PUNCT, "(")) {
            ej++;
            if (ej >= p->n || p->toks[ej].kind != TK_IDENT) return NULL;
            slice_to(err_txt, sizeof(err_txt), p->toks[ej].spell);
            ej++;
            if (ej >= p->n || !tok_eq(p->toks[ej], TK_PUNCT, ")")) return NULL;
            ej++;
        } else {
            if (ej >= p->n || p->toks[ej].kind != TK_IDENT) return NULL;
            slice_to(err_txt, sizeof(err_txt), p->toks[ej].spell);
            ej++;
        }
        bi = ej;
        is_ptr = 0; /* already folded into ok_txt */
    }
    /* Optional slice marker on element: `T[:]` / `T[:N]` / `T[:!]` before `[~`. */
    if (!is_result && bi + 2 < p->n && tok_eq(p->toks[bi], TK_PUNCT, "[") &&
        tok_eq(p->toks[bi + 1], TK_PUNCT, ":")) {
        int sj = bi + 2;
        if (sj < p->n && p->toks[sj].kind == TK_NUM) sj++;
        if (sj < p->n && tok_eq(p->toks[sj], TK_PUNCT, "!")) sj++;
        if (sj < p->n && tok_eq(p->toks[sj], TK_PUNCT, "]")) {
            is_slice = 1;
            bi = sj + 1;
        }
    }
    if (bi + 1 >= p->n || !tok_eq(p->toks[bi], TK_PUNCT, "[") ||
        !tok_eq(p->toks[bi + 1], TK_PUNCT, "~"))
        return NULL;
    di = bi + 2;
    /* Cap: NUM, IDENT, or `(expr)` — not the left side of schedule `N:1`. */
    if (di < p->n && tok_eq(p->toks[di], TK_PUNCT, "(")) {
        int depth = 0;
        int k = di;
        while (k < p->n) {
            if (tok_eq(p->toks[k], TK_PUNCT, "(")) depth++;
            else if (tok_eq(p->toks[k], TK_PUNCT, ")")) {
                depth--;
                if (depth == 0) {
                    k++;
                    break;
                }
            }
            k++;
        }
        if (depth != 0 || k <= di) return NULL;
        if (!ast_spell_token_range(p, di, k, cap_txt, sizeof(cap_txt)) &&
            !span_text(p, di, k, cap_txt, sizeof(cap_txt)))
            return NULL;
        di = k;
    } else if (di < p->n &&
               (p->toks[di].kind == TK_NUM || p->toks[di].kind == TK_IDENT) &&
               !(di + 2 < p->n && tok_eq(p->toks[di + 1], TK_PUNCT, ":") &&
                 p->toks[di + 2].kind == TK_NUM)) {
        int is_mod = 0;
        if (p->toks[di].kind == TK_IDENT) {
            if (spell_eq(p->toks[di].spell, "sync") ||
                spell_eq(p->toks[di].spell, "async") ||
                spell_eq(p->toks[di].spell, "owned") ||
                spell_eq(p->toks[di].spell, "Drop") ||
                spell_eq(p->toks[di].spell, "DropNew") ||
                spell_eq(p->toks[di].spell, "DropOld") ||
                spell_eq(p->toks[di].spell, "Drop_New") ||
                spell_eq(p->toks[di].spell, "Drop_Old") ||
                shadow_kw(p->toks[di]) == SHADOW_KW_ORDERED)
                is_mod = 1;
        }
        if (!is_mod) {
            slice_to(cap_txt, sizeof(cap_txt), p->toks[di].spell);
            di++;
        } else {
            snprintf(cap_txt, sizeof(cap_txt), "0");
        }
    } else {
        snprintf(cap_txt, sizeof(cap_txt), "0");
    }
    /* Modifiers before direction: ordered / schedule / sync / Drop / owned. */
    for (;;) {
        if (di >= p->n) return NULL;
        if (shadow_kw(p->toks[di]) == SHADOW_KW_ORDERED) {
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
        if (p->toks[di].kind == TK_IDENT &&
            spell_eq(p->toks[di].spell, "sync")) {
            is_sync = 1;
            di++;
            continue;
        }
        if (p->toks[di].kind == TK_IDENT &&
            spell_eq(p->toks[di].spell, "async")) {
            is_sync = 0;
            di++;
            continue;
        }
        if (p->toks[di].kind == TK_IDENT &&
            (spell_eq(p->toks[di].spell, "Drop") ||
             spell_eq(p->toks[di].spell, "DropNew") ||
             spell_eq(p->toks[di].spell, "Drop_New"))) {
            bp_mode = 1;
            di++;
            continue;
        }
        if (p->toks[di].kind == TK_IDENT &&
            (spell_eq(p->toks[di].spell, "DropOld") ||
             spell_eq(p->toks[di].spell, "Drop_Old"))) {
            bp_mode = 2;
            di++;
            continue;
        }
        if (p->toks[di].kind == TK_IDENT &&
            spell_eq(p->toks[di].spell, "owned")) {
            is_owned = 1;
            di++;
            if (di >= p->n || !tok_eq(p->toks[di], TK_PUNCT, "{")) {
                parser_fail(p, p->toks[di < p->n ? di : p->n - 1],
                            "channel: owned channel requires { ... } block");
                return NULL;
            }
            owned_brace = di;
            {
                int depth = 0;
                int k = di;
                while (k < p->n) {
                    if (tok_eq(p->toks[k], TK_PUNCT, "{")) depth++;
                    else if (tok_eq(p->toks[k], TK_PUNCT, "}")) {
                        depth--;
                        if (depth == 0) {
                            owned_end = k;
                            break;
                        }
                    }
                    k++;
                }
            }
            if (owned_end < 0) {
                parser_fail(p, p->toks[owned_brace],
                            "channel: unterminated owned block");
                return NULL;
            }
            di = owned_end + 1;
            continue;
        }
        if (tok_eq(p->toks[di], TK_PUNCT, ",")) {
            di++;
            continue;
        }
        break;
    }
    if (di >= p->n) return NULL;
    if (is_owned) {
        if (!tok_eq(p->toks[di], TK_PUNCT, "]")) {
            parser_fail(p, p->toks[di],
                        "channel: expected ']' after owned block");
            return NULL;
        }
        dir = (Token){0};
    } else if (tok_eq(p->toks[di], TK_PUNCT, "]")) {
        parser_fail(p, p->toks[di],
                    "channel: channel handle type requires direction");
        return NULL;
    } else if (tok_eq(p->toks[di], TK_PUNCT, ">") ||
               tok_eq(p->toks[di], TK_PUNCT, "<")) {
        dir = p->toks[di];
        di++;
        if (di < p->n &&
            ((tok_eq(dir, TK_PUNCT, ">") && tok_eq(p->toks[di], TK_PUNCT, "<")) ||
             (tok_eq(dir, TK_PUNCT, "<") && tok_eq(p->toks[di], TK_PUNCT, ">")))) {
            parser_fail(p, dir,
                        "channel: channel handle type cannot be both");
            return NULL;
        }
        /* Optional `, Drop` / `, DropOld` after direction. */
        for (;;) {
            if (di < p->n && tok_eq(p->toks[di], TK_PUNCT, ",")) {
                di++;
                continue;
            }
            if (di < p->n && p->toks[di].kind == TK_IDENT &&
                (spell_eq(p->toks[di].spell, "Drop") ||
                 spell_eq(p->toks[di].spell, "DropNew") ||
                 spell_eq(p->toks[di].spell, "Drop_New"))) {
                bp_mode = 1;
                di++;
                continue;
            }
            if (di < p->n && p->toks[di].kind == TK_IDENT &&
                (spell_eq(p->toks[di].spell, "DropOld") ||
                 spell_eq(p->toks[di].spell, "Drop_Old"))) {
                bp_mode = 2;
                di++;
                continue;
            }
            break;
        }
        if (di >= p->n || !tok_eq(p->toks[di], TK_PUNCT, "]")) return NULL;
    } else {
        return NULL;
    }
    if (ordered && !is_owned && tok_eq(dir, TK_PUNCT, ">")) {
        parser_fail(p, dir,
                    "'ordered' modifier only allowed on receive (<) channel");
        return NULL;
    }

    /* Consume tokens and build AST. */
    p_next(p); /* elem ty / Ok */
    /* `Ok*` before `!>(Err)` — consume star for both Result and plain elems. */
    if (tok_eq(p_peek(p), TK_PUNCT, "*")) p_next(p); /* * */
    if (is_result) {
        p_next(p); /* !> */
        if (tok_eq(p_peek(p), TK_PUNCT, "(")) {
            p_next(p); /* ( */
            p_next(p); /* Err */
            p_next(p); /* ) */
        } else {
            p_next(p); /* Err */
        }
    }
    if (is_slice) {
        p_next(p); /* [ */
        p_next(p); /* : */
        if (p_peek(p).kind == TK_NUM) p_next(p);
        if (tok_eq(p_peek(p), TK_PUNCT, "!")) p_next(p);
        p_next(p); /* ] */
    }
    p_next(p); /* [ */
    p_next(p); /* ~ */
    /* Cap / modifiers / direction — mirror the lookahead loop. */
    for (;;) {
        Token t = p_peek(p);
        if (t.kind == TK_EOF) break;
        if (tok_eq(t, TK_PUNCT, "(") && cap_txt[0] == '(') {
            int depth = 0;
            while (p->i < p->n) {
                Token bt = p_peek(p);
                if (tok_eq(bt, TK_PUNCT, "(")) depth++;
                else if (tok_eq(bt, TK_PUNCT, ")")) {
                    depth--;
                    p_next(p);
                    if (depth == 0) break;
                    continue;
                }
                p_next(p);
            }
            continue;
        }
        if ((t.kind == TK_NUM || t.kind == TK_IDENT) &&
            p->i + 2 < p->n && tok_eq(p->toks[p->i + 1], TK_PUNCT, ":") &&
            p->toks[p->i + 2].kind == TK_NUM) {
            p_next(p);
            p_next(p);
            p_next(p); /* schedule */
            continue;
        }
        if ((t.kind == TK_NUM || t.kind == TK_IDENT) &&
            strcmp(cap_txt, "0") != 0 &&
            ((t.kind == TK_NUM && spell_eq(t.spell, cap_txt)) ||
             (t.kind == TK_IDENT && spell_eq(t.spell, cap_txt)))) {
            p_next(p); /* cap */
            continue;
        }
        if (shadow_kw(t) == SHADOW_KW_ORDERED) {
            p_next(p);
            continue;
        }
        if (t.kind == TK_IDENT &&
            (spell_eq(t.spell, "sync") || spell_eq(t.spell, "async") ||
             spell_eq(t.spell, "Drop") || spell_eq(t.spell, "DropNew") ||
             spell_eq(t.spell, "DropOld") || spell_eq(t.spell, "Drop_New") ||
             spell_eq(t.spell, "Drop_Old"))) {
            p_next(p);
            continue;
        }
        if (t.kind == TK_IDENT && spell_eq(t.spell, "owned")) {
            p_next(p); /* owned */
            if (!tok_eq(p_peek(p), TK_PUNCT, "{")) {
                parser_fail(p, p_peek(p),
                            "channel: owned channel requires { ... } block");
                return NULL;
            }
            /* Skip the brace block; closures reparsed below into dbody. */
            {
                int depth = 0;
                while (p->i < p->n) {
                    Token bt = p_peek(p);
                    if (tok_eq(bt, TK_PUNCT, "{")) depth++;
                    else if (tok_eq(bt, TK_PUNCT, "}")) {
                        depth--;
                        p_next(p);
                        if (depth == 0) break;
                        continue;
                    }
                    p_next(p);
                }
            }
            continue;
        }
        if (tok_eq(t, TK_PUNCT, ",")) {
            p_next(p);
            continue;
        }
        if (tok_eq(t, TK_PUNCT, ">") || tok_eq(t, TK_PUNCT, "<")) {
            dtok = p_next(p);
            continue;
        }
        if (tok_eq(t, TK_PUNCT, "]")) break;
        break;
    }
    if (!p_accept(p, TK_PUNCT, "]")) {
        parser_fail(p, p_peek(p), "expected ']' after channel handle type");
        return NULL;
    }
    name = p_next(p);
    if (name.kind != TK_IDENT) {
        parser_fail(p, name, "expected channel declarator name");
        return NULL;
    }
    /* Multi-declarator: `int[~N >] tx1, tx2;` — names packed comma-separated.
     * Optional init `int[~N >] tx = tx_h;` packed into e as `;=expr` after
     * ordered/topo flags (see below). */
    {
        char names[128];
        size_t nl;
        slice_to(names, sizeof(names), name.spell);
        nl = strlen(names);
        while (p_accept(p, TK_PUNCT, ",")) {
            Token n2 = p_next(p);
            char piece[48];
            size_t pl;
            if (n2.kind != TK_IDENT) {
                parser_fail(p, n2, "expected channel declarator name");
                return NULL;
            }
            slice_to(piece, sizeof(piece), n2.spell);
            pl = strlen(piece);
            if (nl + 1 + pl + 1 >= sizeof(names)) {
                parser_fail(p, n2, "channel multi-declarator list too long");
                return NULL;
            }
            names[nl++] = ',';
            memcpy(names + nl, piece, pl);
            nl += pl;
            names[nl] = 0;
        }
        if (tok_eq(p_peek(p), TK_PUNCT, "=")) {
            int e0;
            int depth = 0;
            if (strchr(names, ',')) {
                parser_fail(p, p_peek(p),
                            "channel init only allowed on a single "
                            "declarator");
                return NULL;
            }
            p_next(p); /* = */
            e0 = p->i;
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
            if (!ast_spell_token_range(p, e0, p->i, chan_init,
                                       sizeof(chan_init)) &&
                !span_text(p, e0, p->i, chan_init, sizeof(chan_init))) {
                parser_fail(p, name, "channel init too long");
                return NULL;
            }
        }
        if (!p_accept(p, TK_PUNCT, ";")) {
            parser_fail(p, p_peek(p), "expected ';' after channel declaration");
            return NULL;
        }
        n = ast_new(p, AST_CHAN_VAR);
        if (!n) return NULL;
        snprintf(n->a, sizeof(n->a), "%s", names);
    }
    snprintf(n->b, sizeof(n->b), "%s", cap_txt);
    if (is_owned)
        snprintf(n->c, sizeof(n->c), "owned");
    else
        slice_to(n->c, sizeof(n->c), dtok.spell);
    if (is_result) {
        snprintf(elem_txt, sizeof(elem_txt), "CCResult_%s_%s", ok_txt, err_txt);
    } else {
        slice_to(elem_txt, sizeof(elem_txt), ety.spell);
        if (is_ptr) {
            size_t el = strlen(elem_txt);
            if (el + 1 < sizeof(elem_txt)) {
                elem_txt[el] = '*';
                elem_txt[el + 1] = 0;
            }
        }
        if (is_slice) {
            size_t el = strlen(elem_txt);
            if (el + 3 < sizeof(elem_txt)) {
                elem_txt[el] = '[';
                elem_txt[el + 1] = ':';
                elem_txt[el + 2] = ']';
                elem_txt[el + 3] = 0;
            }
        }
    }
    snprintf(n->d, sizeof(n->d), "%s", elem_txt);
    /* e: ordered/topo + sync/bp packing for register/emit. */
    {
        char base[64];
        base[0] = 0;
        if (is_owned)
            snprintf(base, sizeof(base), "owned");
        else if (ordered && topo_txt[0])
            snprintf(base, sizeof(base), "o:%s", topo_txt);
        else if (ordered)
            snprintf(base, sizeof(base), "o");
        else if (topo_txt[0])
            snprintf(base, sizeof(base), "t:%s", topo_txt);
        snprintf(n->e, sizeof(n->e), "%s%s%s", base, is_sync ? ";s" : "",
                 bp_mode == 1 ? ";dn" : bp_mode == 2 ? ";do" : "");
        if (chan_init[0]) {
            size_t el = strlen(n->e);
            if (el + 2 + strlen(chan_init) + 1 < sizeof(n->e))
                snprintf(n->e + el, sizeof(n->e) - el, ";=%s", chan_init);
            else {
                parser_fail(p, name, "channel init too long for meta pack");
                return NULL;
            }
        }
    }
    /* Owned block: parse `.create/.destroy/.reset = (…) => {…}` into dbody. */
    if (is_owned && owned_brace >= 0 && owned_end > owned_brace) {
        int saved = p->i;
        int k = owned_brace + 1; /* inside `{` */
        while (k < owned_end && !p->err) {
            char field[32];
            char dummy[8];
            AstNode* cl = NULL;
            int c0, c1;
            int depth;
            while (k < owned_end &&
                   (tok_eq(p->toks[k], TK_PUNCT, ",") ||
                    p->toks[k].kind == TK_EOF))
                k++;
            if (k >= owned_end) break;
            if (!tok_eq(p->toks[k], TK_PUNCT, ".") || k + 2 >= owned_end ||
                p->toks[k + 1].kind != TK_IDENT ||
                !tok_eq(p->toks[k + 2], TK_PUNCT, "=")) {
                parser_fail(p, p->toks[k],
                            "channel: owned block expects .field = closure");
                return NULL;
            }
            slice_to(field, sizeof(field), p->toks[k + 1].spell);
            k += 3; /* . field = */
            c0 = k;
            depth = 0;
            c1 = k;
            while (c1 < owned_end) {
                if (tok_eq(p->toks[c1], TK_PUNCT, "(") ||
                    tok_eq(p->toks[c1], TK_PUNCT, "{") ||
                    tok_eq(p->toks[c1], TK_PUNCT, "["))
                    depth++;
                else if (tok_eq(p->toks[c1], TK_PUNCT, ")") ||
                         tok_eq(p->toks[c1], TK_PUNCT, "}") ||
                         tok_eq(p->toks[c1], TK_PUNCT, "]")) {
                    if (depth > 0) depth--;
                } else if (depth == 0 && tok_eq(p->toks[c1], TK_PUNCT, ","))
                    break;
                c1++;
            }
            p->i = c0;
            if (!parse_call_arg_closure(p, c0, c1, dummy, sizeof(dummy), field,
                                        &cl) ||
                !cl) {
                if (!p->err)
                    parser_fail(p, p->toks[c0],
                                "channel: owned closure failed to parse");
                return NULL;
            }
            if (strcmp(field, "create") != 0)
                snprintf(cl->c, sizeof(cl->c), "owned:%s", field);
            if (n->ndbody >= SHADOW_BODY_CAP) {
                parser_fail_body_cap(p, p->toks[c0], "owned channel closures");
                return NULL;
            }
            spawn_infer_value_caps(cl);
            n->dbody[n->ndbody++] = cl;
            k = c1;
            if (k < owned_end && tok_eq(p->toks[k], TK_PUNCT, ",")) k++;
        }
        p->i = saved;
        if (p->err) return NULL;
    }
    return n;
}

/* Track Result ok-type const-ness so sharing one box across `const T*` /
 * `T*` is not silent (result_type_canonical_smoke). */
static char g_shadow_rq_name[16][96];
static unsigned g_shadow_rq_qual[16];
static int g_shadow_rq_line[16];
static int g_shadow_nrq;

static void shadow_result_note_ok_qual(Parser* p, Token at, const char* okty,
                                      const char* errty, int had_const) {
    char mok[64], merr[64], concrete[160];
    unsigned quals = had_const ? 1u : 0u;
    int i, line = 1, col = 1;
    size_t j, k;
    if (!okty || !errty) return;
    /* Local mangle: spaces→_, *→ptr (matches shadow_mangle_type). */
    j = 0;
    for (k = 0; okty[k] && j + 1 < sizeof(mok); k++) {
        char c = okty[k];
        if (c == ' ' || c == '\t') {
            if (j && mok[j - 1] != '_') mok[j++] = '_';
        } else if (c == '*') {
            if (j + 3 < sizeof(mok) - 1) {
                mok[j++] = 'p';
                mok[j++] = 't';
                mok[j++] = 'r';
            }
        } else if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                   (c >= '0' && c <= '9') || c == '_')
            mok[j++] = c;
    }
    mok[j] = 0;
    j = 0;
    for (k = 0; errty[k] && j + 1 < sizeof(merr); k++) {
        char c = errty[k];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '_')
            merr[j++] = c;
    }
    merr[j] = 0;
    snprintf(concrete, sizeof(concrete), "CCResult_%s_%s", mok, merr);
    for (i = 0; i < g_shadow_nrq; i++) {
        if (strcmp(g_shadow_rq_name[i], concrete) != 0) continue;
        if (g_shadow_rq_qual[i] == quals) return;
        {
            FileTape* ft =
                (p && p->cache) ? tape_by_id(p->cache, at.file_id) : NULL;
            const char* path = (ft && ft->path) ? ft->path : "<input>";
            const char* tests = strstr(path, "tests/");
            if (ft && ft->bytes)
                offset_to_linecol(ft->bytes, ft->len, at.offset, &line, &col);
            fprintf(stderr,
                    "%s:%d:%d: warning: type: '%s' is declared with a %s "
                    "ok type here and a %s one at line %d; qualifiers are "
                    "not part of a box's identity, so both share one box "
                    "and the first spelling wins\n",
                    tests ? tests : path, line, col, concrete,
                    quals ? "const" : "unqualified",
                    g_shadow_rq_qual[i] ? "const" : "unqualified",
                    g_shadow_rq_line[i]);
        }
        return;
    }
    if (g_shadow_nrq < 16) {
        FileTape* ft =
            (p && p->cache) ? tape_by_id(p->cache, at.file_id) : NULL;
        if (ft && ft->bytes)
            offset_to_linecol(ft->bytes, ft->len, at.offset, &line, &col);
        snprintf(g_shadow_rq_name[g_shadow_nrq],
                 sizeof(g_shadow_rq_name[0]), "%s", concrete);
        g_shadow_rq_qual[g_shadow_nrq] = quals;
        g_shadow_rq_line[g_shadow_nrq] = line;
        g_shadow_nrq++;
    }
}

/* Ok!>Err or Ok!>(Err) Name ( params ) ;|{ body } */
static AstNode* parse_result_fn(Parser* p) {
    unsigned decl_attrs = 0;
    int ty0, ty1;
    char okty[96];
    int had_const = 0;
    int specs0 = p->i;
    int j;
    /* Callers may already have consumed `static`/`const` (parse_external
     * static+result path). Look back before skipping remaining specs. */
    for (j = p->i - 1; j >= 0 && j >= p->i - 6; j--) {
        Token t = p->toks[j];
        if (shadow_kw(t) == SHADOW_KW_CONST) {
            had_const = 1;
            break;
        }
        if (shadow_is_cc_storage_kw(t)) continue;
        break;
    }
    shadow_parser_skip_decl_specs(p, &decl_attrs);
    for (j = specs0; j < p->i; j++) {
        if (shadow_kw(p->toks[j]) == SHADOW_KW_CONST) {
            had_const = 1;
            break;
        }
    }
    if (!peek_result_shape(p)) return NULL;
    if (p->i + 6 >= p->n) return NULL;
    ty0 = p->i;
    ty1 = peek_c_int_type_end(p, ty0);
    if (ty1 > ty0) {
        if (!ast_spell_token_range(p, ty0, ty1, okty, sizeof(okty)) &&
            !span_text(p, ty0, ty1, okty, sizeof(okty))) {
            parser_fail(p, p_peek(p), "result ok-type too long");
            return NULL;
        }
        while (p->i < ty1) p_next(p);
    } else {
        Token ok = p_next(p);
        slice_to(okty, sizeof(okty), ok.spell);
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
    p_next(p); /* !> */
    Token err;
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
    Token name = p_next(p);
    if (name.kind != TK_IDENT) {
        parser_fail(p, name, "expected function name after result type");
        return NULL;
    }
    {
        char errty[64];
        slice_to(errty, sizeof(errty), err.spell);
        shadow_result_note_ok_qual(p, name, okty, errty, had_const);
    }
    if (!p_accept(p, TK_PUNCT, "(")) { parser_fail(p, p_peek(p), "expected '('"); return NULL; }
    /* Match AstNode.d (4096): multi-line redis Result protos exceed 255. */
    char param_ty[4096] = {0};
    if (shadow_kw(p_peek(p)) == SHADOW_KW_VOID) {
        p_next(p);
    } else if (p_peek(p).kind != TK_PUNCT || !tok_eq(p_peek(p), TK_PUNCT, ")")) {
        int p0 = p->i;
        int depth = 1; /* already inside params `(` */
        int save = p->i;
        (void)save;
        while (p->i < p->n && depth > 0) {
            Token t = p_peek(p);
            if (tok_eq(t, TK_PUNCT, "(")) depth++;
            else if (tok_eq(t, TK_PUNCT, ")")) {
                depth--;
                if (depth == 0) break;
            }
            p_next(p);
        }
        if (!ast_spell_token_range(p, p0, p->i, param_ty, sizeof(param_ty))) {
            parser_fail(p, p_peek(p), "result fn params too long for shadow beachhead");
            return NULL;
        }
    }
    if (!p_accept(p, TK_PUNCT, ")")) { parser_fail(p, p_peek(p), "expected ')'"); return NULL; }
    AstNode* n = ast_new(p, AST_RESULT_FN);
    if (!n) return NULL;
    slice_to(n->a, sizeof(n->a), name.spell);
    slice_to(n->b, sizeof(n->b), err.spell);
    snprintf(n->c, sizeof(n->c), "%s", okty);
    snprintf(n->d, sizeof(n->d), "%s", param_ty);
    if (p_accept(p, TK_PUNCT, ";")) {
        if (decl_attrs | p->pending_fn_attrs)
            shadow_fn_attr_register(n->a, decl_attrs | p->pending_fn_attrs, 0);
        p->pending_fn_attrs = 0;
        return n;
    }
    if (!p_accept(p, TK_PUNCT, "{")) {
        parser_fail(p, p_peek(p), "expected ';' or '{' after result function header");
        return NULL;
    }
    n->kids = &p->kids_storage[p->nkstore];
    while (!tok_eq(p_peek(p), TK_PUNCT, "}") && p_peek(p).kind != TK_EOF && !p->err) {
        AstNode* s = parse_stmt(p);
        if (!s) return NULL;
        if (!ast_kids_push(p, s)) return NULL;
        n->nkids++;
    }
    if (!p_accept(p, TK_PUNCT, "}")) {
        parser_fail(p, p_peek(p), "expected '}' to close result function");
        return NULL;
    }
    if (decl_attrs | p->pending_fn_attrs)
        shadow_fn_attr_register(n->a, decl_attrs | p->pending_fn_attrs, 1);
    p->pending_fn_attrs = 0;
    return n;
}

/* @errhandler ( bind ) ;  — delegate to enclosing @errhandler in @err body */
static AstNode* parse_err_delegate(Parser* p) {
    if (!tok_eq(p_peek(p), TK_PUNCT, "@")) return NULL;
    if (p->i + 1 >= p->n || shadow_kw(p->toks[p->i + 1]) != SHADOW_KW_ERRHANDLER)
        return NULL;
    if (p->i + 4 >= p->n) return NULL;
    if (!tok_eq(p->toks[p->i + 2], TK_PUNCT, "(")) return NULL;
    if (p->toks[p->i + 3].kind != TK_IDENT) return NULL;
    if (!tok_eq(p->toks[p->i + 4], TK_PUNCT, ")")) return NULL;
    p_next(p); /* @ */
    p_next(p); /* errhandler */
    p_next(p); /* ( */
    Token bind = p_next(p);
    p_next(p); /* ) */
    if (!p_accept(p, TK_PUNCT, ";")) {
        parser_fail(p, p_peek(p), "expected ';' after @errhandler(...)");
        return NULL;
    }
    AstNode* n = ast_new(p, AST_ERR_FWD);
    if (!n) return NULL;
    slice_to(n->a, sizeof(n->a), bind.spell);
    snprintf(n->c, sizeof(n->c), "delegate");
    return n;
}

/* @errhandler(Type bind) handler(bind);  OR  @errhandler(Type bind) { stmts } */
static AstNode* parse_errhandler(Parser* p) {
    if (!tok_eq(p_peek(p), TK_PUNCT, "@")) return NULL;
    if (p->i + 1 >= p->n || shadow_kw(p->toks[p->i + 1]) != SHADOW_KW_ERRHANDLER)
        return NULL;
    p_next(p); /* @ */
    p_next(p); /* errhandler */
    if (!p_accept(p, TK_PUNCT, "(")) {
        parser_fail(p, p_peek(p), "expected '(' after @errhandler");
        return NULL;
    }
    Token ty = p_next(p);
    if (ty.kind != TK_IDENT) {
        parser_fail(p, ty, "expected @errhandler(Type name)");
        return NULL;
    }
    if (tok_eq(p_peek(p), TK_PUNCT, ")")) {
        p_next(p); /* ) */
        if (!p_accept(p, TK_PUNCT, ";")) {
            parser_fail(p, p_peek(p), "expected ';' after @errhandler(...)");
            return NULL;
        }
        AstNode* fwd = ast_new(p, AST_ERR_FWD);
        if (!fwd) return NULL;
        slice_to(fwd->a, sizeof(fwd->a), ty.spell);
        snprintf(fwd->c, sizeof(fwd->c), "delegate");
        return fwd;
    }
    Token bind = p_next(p);
    if (bind.kind != TK_IDENT) {
        parser_fail(p, bind, "expected @errhandler(Type name)");
        return NULL;
    }
    if (!p_accept(p, TK_PUNCT, ")")) {
        parser_fail(p, p_peek(p), "expected ')' after errhandler bind");
        return NULL;
    }
    AstNode* n = ast_new(p, AST_ERRHANDLER);
    if (!n) return NULL;
    slice_to(n->a, sizeof(n->a), ty.spell);
    slice_to(n->b, sizeof(n->b), bind.spell);
    if (tok_eq(p_peek(p), TK_PUNCT, "{")) {
        p_next(p);
        while (!tok_eq(p_peek(p), TK_PUNCT, "}") && p_peek(p).kind != TK_EOF && !p->err) {
            AstNode* st = parse_stmt(p);
            if (!st) return NULL;
            if (n->nbody >= SHADOW_BODY_CAP) {
                parser_fail_body_cap(p, p_peek(p), "@errhandler body");
                return NULL;
            }
            n->body[n->nbody++] = st;
        }
        if (!p_accept(p, TK_PUNCT, "}")) {
            parser_fail(p, p_peek(p), "expected '}' to close @errhandler");
            return NULL;
        }
        return n;
    }
    Token handler = p_next(p);
    if (handler.kind != TK_IDENT) {
        parser_fail(p, handler, "expected handler call or '{' after @errhandler(...)");
        return NULL;
    }
    if (!p_accept(p, TK_PUNCT, "(")) { p->err = 1; return NULL; }
    Token arg = p_next(p);
    char bind_name[128];
    slice_to(bind_name, sizeof(bind_name), bind.spell);
    if (arg.kind != TK_IDENT || !spell_eq(arg.spell, bind_name)) {
        parser_fail(p, arg, "errhandler call arg must match bind name");
        return NULL;
    }
    if (!p_accept(p, TK_PUNCT, ")") || !p_accept(p, TK_PUNCT, ";")) {
        p->err = 1;
        return NULL;
    }
    slice_to(n->c, sizeof(n->c), handler.spell);
    return n;
}

/* @err ( Ident ) ; */
static AstNode* parse_err_fwd(Parser* p) {
    if (!tok_eq(p_peek(p), TK_PUNCT, "@")) return NULL;
    if (p->i + 1 >= p->n || shadow_kw(p->toks[p->i + 1]) != SHADOW_KW_ERR)
        return NULL;
    p_next(p); /* @ */
    p_next(p); /* err */
    if (!p_accept(p, TK_PUNCT, "(")) {
        parser_fail(p, p_peek(p), "expected '(' after @err");
        return NULL;
    }
    Token bind = p_next(p);
    if (bind.kind != TK_IDENT) {
        parser_fail(p, bind, "expected bind name in @err(...)");
        return NULL;
    }
    if (!p_accept(p, TK_PUNCT, ")") || !p_accept(p, TK_PUNCT, ";")) {
        p->err = 1;
        return NULL;
    }
    AstNode* n = ast_new(p, AST_ERR_FWD);
    if (!n) return NULL;
    slice_to(n->a, sizeof(n->a), bind.spell);
    n->e[0] = 'h'; /* @errhandler(bind) delegation */
    return n;
}

/* @ string ( `tpl` , @ scratch | arena-expr ) — tpl body (no ticks) in dst;
 * optional arena spelling in arena_dst (empty ⇒ @scratch). */
static int parse_at_string_arena(Parser* p, char* dst, size_t cap,
                                 char* arena_dst, size_t arena_cap) {
    if (!tok_eq(p_peek(p), TK_PUNCT, "@")) return 0;
    if (p->i + 1 >= p->n || shadow_kw(p->toks[p->i + 1]) != SHADOW_KW_STRING) return 0;
    p_next(p); /* @ */
    p_next(p); /* string */
    if (!p_accept(p, TK_PUNCT, "(")) {
        parser_fail(p, p_peek(p), "expected '(' after @string");
        return 0;
    }
    Token bt = p_next(p);
    if (bt.kind != TK_STR || !bt.spell.ptr || bt.spell.len < 2 || bt.spell.ptr[0] != '`') {
        parser_fail(p, bt, "expected backtick template in @string(...)");
        return 0;
    }
    if (!p_accept(p, TK_PUNCT, ",")) {
        parser_fail(p, p_peek(p), "expected ',' after @string template");
        return 0;
    }
    if (arena_dst && arena_cap) arena_dst[0] = 0;
    if (tok_eq(p_peek(p), TK_PUNCT, "@")) {
        p_next(p);
        Token scratch = p_next(p);
        if (scratch.kind != TK_IDENT || !spell_eq(scratch.spell, "scratch")) {
            parser_fail(p, scratch, "expected @scratch");
            return 0;
        }
        /* Optional `@scratch(N)` size hint — stack arena sizing is emit-side. */
        if (tok_eq(p_peek(p), TK_PUNCT, "(")) {
            if (!skip_parens(p)) {
                parser_fail(p, p_peek(p), "unterminated @scratch(...)");
                return 0;
            }
        }
    } else {
        /* Arena expr: `&arena` / `arena` / `foo.bar`. */
        int a0 = p->i;
        int depth = 0;
        while (p->i < p->n) {
            Token t = p_peek(p);
            if (tok_eq(t, TK_PUNCT, "(") || tok_eq(t, TK_PUNCT, "[") ||
                tok_eq(t, TK_PUNCT, "{"))
                depth++;
            else if (tok_eq(t, TK_PUNCT, ")") || tok_eq(t, TK_PUNCT, "]") ||
                     tok_eq(t, TK_PUNCT, "}")) {
                if (depth == 0) break;
                depth--;
            } else if (depth == 0 && tok_eq(t, TK_PUNCT, ","))
                break;
            p_next(p);
        }
        if (a0 == p->i) {
            parser_fail(p, p_peek(p),
                        "expected @scratch or arena after @string template");
            return 0;
        }
        if (arena_dst && arena_cap) {
            if (!ast_spell_token_range(p, a0, p->i, arena_dst, arena_cap) &&
                !span_text(p, a0, p->i, arena_dst, arena_cap)) {
                parser_fail(p, p->toks[a0], "@string arena expr too long");
                return 0;
            }
        }
    }
    if (!p_accept(p, TK_PUNCT, ")")) {
        parser_fail(p, p_peek(p), "expected ')' after @string(...)");
        return 0;
    }
    /* strip ticks */
    size_t n = bt.spell.len >= 2 ? bt.spell.len - 2 : 0;
    if (n >= cap) n = cap - 1;
    memcpy(dst, bt.spell.ptr + 1, n);
    dst[n] = 0;
    return 1;
}
static int parse_at_string_scratch(Parser* p, char* dst, size_t cap) {
    return parse_at_string_arena(p, dst, cap, NULL, 0);
}

/* println/eprintln ( STR | @string(...) ) !> ;  OR  … !> ( Ident ) { stmts } ; */
static AstNode* parse_return_int(Parser* p) {
    if (shadow_kw(p_peek(p)) != SHADOW_KW_RETURN) return NULL;
    p_next(p);
    Token t = p_peek(p);
    if (t.kind == TK_IDENT && (spell_eq(t.spell, "cc_ok") || spell_eq(t.spell, "cc_err"))) {
        Token which = p_next(p);
        if (!tok_eq(p_peek(p), TK_PUNCT, "(")) {
            parser_fail(p, p_peek(p), "expected '(' after cc_ok/cc_err");
            return NULL;
        }
        int a0 = p->i + 1; /* first tok inside parens */
        if (!skip_parens(p)) {
            parser_fail(p, p_peek(p), "unterminated cc_ok/cc_err(...)");
            return NULL;
        }
        int a1 = p->i - 1; /* index of `)` — args are a0 .. a1 */
        char args[256];
        if (a0 >= a1) args[0] = 0;
        else if (!ast_spell_token_range(p, a0, a1, args, sizeof(args)) &&
                 !span_text(p, a0, a1, args, sizeof(args))) {
            parser_fail(p, which, "cc_ok/cc_err args too long");
            return NULL;
        }
        if (!p_accept(p, TK_PUNCT, ";")) { p->err = 1; return NULL; }
        AstNode* n = ast_new(p, AST_RETURN_CC);
        if (!n) return NULL;
        snprintf(n->a, sizeof(n->a), "%s", spell_eq(which.spell, "cc_ok") ? "ok" : "err");
        snprintf(n->b, sizeof(n->b), "%s", args);
        return n;
    }
    if (t.kind == TK_NUM && p->i + 1 < p->n && tok_eq(p->toks[p->i + 1], TK_PUNCT, ";")) {
        Token num = p_next(p);
        p_next(p); /* ; */
        AstNode* n = ast_new(p, AST_RETURN_INT);
        if (!n) return NULL;
        slice_to(n->a, sizeof(n->a), num.spell);
        return n;
    }
    /* General return expr (ternary, NULL, UFCS, …) [!> …]; */
    int e0 = p->i;
    int depth = 0;
    int bang_at = -1;
    while (p->i < p->n) {
        Token x = p_peek(p);
        if (tok_eq(x, TK_PUNCT, "(") || tok_eq(x, TK_PUNCT, "[") ||
            tok_eq(x, TK_PUNCT, "{"))
            depth++;
        else if (tok_eq(x, TK_PUNCT, ")") || tok_eq(x, TK_PUNCT, "]") ||
                 tok_eq(x, TK_PUNCT, "}"))
            depth--;
        else if (depth == 0 && tok_eq(x, TK_PUNCT, "!>")) {
            bang_at = p->i;
            break;
        } else if (depth == 0 && tok_eq(x, TK_PUNCT, ";"))
            break;
        p_next(p);
    }
    AstNode* n = ast_new(p, AST_RETURN_EXPR);
    if (!n) return NULL;
    {
        int e1 = (bang_at >= 0) ? bang_at : p->i;
        AstNode* ue = NULL;
        AstNode* cl = NULL;
        char dummy[8];
        int had_cl = 0;
        /* Lift `return () => {…};` into dbody so emit/safety see SPAWN_CLOSURE. */
        if (bang_at < 0)
            had_cl = parse_call_arg_closure(p, e0, e1, dummy, sizeof(dummy),
                                            "callarg", &cl);
        if (p->err) return NULL;
        if (had_cl) {
            n->a[0] = 0;
            if (cl) {
                if (n->ndbody >= SHADOW_BODY_CAP) {
                    parser_fail(p, p_peek(p),
                                "too many return closure attachments");
                    return NULL;
                }
                spawn_infer_value_caps(cl);
                n->dbody[n->ndbody++] = cl;
            }
        } else {
            ue = parse_ufcs_expr_range(p, e0, e1);
            if (e0 >= e1)
                n->a[0] = 0;
            else if (!ast_spell_token_range(p, e0, e1, n->a, sizeof(n->a)) &&
                     !span_text(p, e0, e1, n->a, sizeof(n->a))) {
                parser_fail(p, p_peek(p), "return expr too long");
                return NULL;
            }
            if (ue) (void)ast_attach_ufcs_kid(p, n, ue);
        }
    }
    if (bang_at < 0) {
        if (!p_accept(p, TK_PUNCT, ";")) { p->err = 1; return NULL; }
        return n;
    }
    p_next(p); /* !> */
    /* `return expr !>;` — errhandler */
    if (p_accept(p, TK_PUNCT, ";")) {
        snprintf(n->e, sizeof(n->e), "bang");
        return n;
    }
    if (tok_eq(p_peek(p), TK_PUNCT, "(")) {
        p_next(p);
        Token bind = p_next(p);
        if (bind.kind != TK_IDENT) {
            parser_fail(p, bind, "expected bind name in return !>(...)");
            return NULL;
        }
        if (!p_accept(p, TK_PUNCT, ")")) {
            parser_fail(p, p_peek(p), "expected ')' after return !>(bind");
            return NULL;
        }
        slice_to(n->d, sizeof(n->d), bind.spell);
    }
    if (tok_eq(p_peek(p), TK_PUNCT, "{")) {
        p_next(p);
        snprintf(n->e, sizeof(n->e), "bang_block");
        while (!tok_eq(p_peek(p), TK_PUNCT, "}") && p_peek(p).kind != TK_EOF &&
               !p->err) {
            AstNode* st = parse_stmt(p);
            if (!st) return NULL;
            if (n->nbody >= SHADOW_BODY_CAP) {
                parser_fail_body_cap(p, p_peek(p), "return !> { ... } body");
                return NULL;
            }
            n->body[n->nbody++] = st;
        }
        if (!p_accept(p, TK_PUNCT, "}")) {
            parser_fail(p, p_peek(p), "expected '}' after return !> { ... }");
            return NULL;
        }
        (void)p_accept(p, TK_PUNCT, ";");
        return n;
    }
    /* `return expr !>(e) stmt;` or `return expr !> stmt;` */
    snprintf(n->e, sizeof(n->e), "bang_stmt");
    {
        AstNode* st = parse_stmt(p);
        if (!st) return NULL;
        n->body[0] = st;
        n->nbody = 1;
    }
    return n;
}

static AstNode* parse_block(Parser* p) {
    if (!tok_eq(p_peek(p), TK_PUNCT, "{")) return NULL;
    p_next(p);
    AstNode* n = ast_new(p, AST_BLOCK);
    if (!n) return NULL;
    while (!tok_eq(p_peek(p), TK_PUNCT, "}") && p_peek(p).kind != TK_EOF && !p->err) {
        AstNode* st = parse_stmt(p);
        if (!st) return NULL;
        if (n->nbody >= SHADOW_BODY_CAP) {
            parser_fail_body_cap(p, p_peek(p), "block");
            return NULL;
        }
        n->body[n->nbody++] = st;
    }
    if (!p_accept(p, TK_PUNCT, "}")) {
        parser_fail(p, p_peek(p), "expected '}' to close block");
        return NULL;
    }
    return n;
}

/* Unwrap / spawn extracted — forward decls for control-flow parsers above. */
static AstNode* parse_println_bang(Parser* p);
static AstNode* parse_stmt_unwrap(Parser* p);
static AstNode* parse_result_local(Parser* p);
static AstNode* parse_var_unwrap(Parser* p);
static int parse_destroy_tail(Parser* p, AstNode* n, char* mode, size_t mode_cap);
static AstNode* parse_ptr_unwrap(Parser* p);
static AstNode* parse_nursery_destroy(Parser* p);
static int shadow_is_arrow_brace(Parser* p, int i, int end, int* out_brace);
static int find_arrow_closure(Parser* p, int c0, int end, int* out_rp);
static int parse_closure_formals_text(Parser* p, int lp, int rp, char* dst,
                                      size_t cap);
static int parse_call_arg_closure(Parser* p, int c0, int end, char* dst,
                                  size_t cap, const char* tag,
                                  AstNode** out_cl);
static void spawn_infer_value_caps(AstNode* n);
static void spawn_note_closure_local(const char* name);
static void spawn_reset_closure_locals(void);
static AstNode* parse_spawn_closure(Parser* p);
static AstNode* parse_send_task_closure(Parser* p);

static AstNode* parse_if_stmt(Parser* p) {
    if (shadow_kw(p_peek(p)) != SHADOW_KW_IF) return NULL;
    p_next(p);
    if (!tok_eq(p_peek(p), TK_PUNCT, "(")) {
        parser_fail(p, p_peek(p), "expected '(' after if");
        return NULL;
    }
    int c0 = p->i + 1;
    if (!skip_parens(p)) {
        parser_fail(p, p_peek(p), "unterminated if (...)");
        return NULL;
    }
    int c1 = p->i - 1;
    AstNode* n = ast_new(p, AST_IF);
    if (!n) return NULL;
    if (c0 >= c1) n->a[0] = 0;
    else {
        AstNode* sp = NULL;
        int sp_after = -1;
        if (try_parse_spawn_cond(p, c0, c1, &sp, &sp_after) && sp) {
            /* Cond = spawn call + trailing compare (e.g. ` != 0`). */
            if (sp_after < c1) {
                if (!ast_spell_token_range(p, sp_after, c1, n->a, sizeof(n->a)) &&
                    !span_text(p, sp_after, c1, n->a, sizeof(n->a))) {
                    parser_fail(p, p_peek(p), "if spawn-cond suffix too long");
                    return NULL;
                }
            } else {
                n->a[0] = 0;
            }
            if (n->ndbody >= SHADOW_BODY_CAP) {
                parser_fail(p, p_peek(p), "too many if-cond attachments");
                return NULL;
            }
            n->dbody[n->ndbody++] = sp;
            snprintf(n->e, sizeof(n->e), "spawn_cond");
        } else {
            AstNode* ue = parse_ufcs_expr_range(p, c0, c1);
            if (!ast_spell_token_range(p, c0, c1, n->a, sizeof(n->a)) &&
                !span_text(p, c0, c1, n->a, sizeof(n->a))) {
                parser_fail(p, p_peek(p), "if condition too long");
                return NULL;
            }
            if (ue) (void)ast_attach_ufcs_kid(p, n, ue);
        }
    }
    AstNode* body = tok_eq(p_peek(p), TK_PUNCT, "{") ? parse_block(p) : parse_stmt(p);
    if (!body) return NULL;
    n->body[0] = body;
    n->nbody = 1;
    if (shadow_kw(p_peek(p)) == SHADOW_KW_ELSE) {
        p_next(p);
        AstNode* eb;
        if (shadow_kw(p_peek(p)) == SHADOW_KW_IF)
            eb = parse_if_stmt(p);
        else if (tok_eq(p_peek(p), TK_PUNCT, "{"))
            eb = parse_block(p);
        else
            eb = parse_stmt(p);
        if (!eb) return NULL;
        n->body[1] = eb;
        n->nbody = 2;
    }
    return n;
}

/* @defer stmt;  |  @defer label: {…};  |  @defer(ok|err) stmt|{…} */
static AstNode* parse_defer(Parser* p) {
    if (!tok_eq(p_peek(p), TK_PUNCT, "@")) return NULL;
    if (p->i + 1 >= p->n || shadow_kw(p->toks[p->i + 1]) != SHADOW_KW_DEFER)
        return NULL;
    p_next(p); /* @ */
    p_next(p); /* defer */
    AstNode* n = ast_new(p, AST_DEFER);
    if (!n) return NULL;
    /* `@defer(ok|err)` only — `@defer (void)x;` is a cast stmt, not a mode. */
    if (tok_eq(p_peek(p), TK_PUNCT, "(") && p->i + 2 < p->n &&
        p->toks[p->i + 1].kind == TK_IDENT &&
        (spell_eq(p->toks[p->i + 1].spell, "ok") ||
         spell_eq(p->toks[p->i + 1].spell, "err")) &&
        tok_eq(p->toks[p->i + 2], TK_PUNCT, ")")) {
        p_next(p); /* ( */
        Token mode = p_next(p);
        slice_to(n->c, sizeof(n->c), mode.spell);
        p_next(p); /* ) */
    } else if (p_peek(p).kind == TK_IDENT && p->i + 1 < p->n &&
               tok_eq(p->toks[p->i + 1], TK_PUNCT, ":")) {
        /* @defer name: { … }; — label is informational for beachhead. */
        Token lab = p_next(p);
        slice_to(n->a, sizeof(n->a), lab.spell);
        p_next(p); /* : */
    }
    if (tok_eq(p_peek(p), TK_PUNCT, "{")) {
        AstNode* blk = parse_block(p);
        if (!blk) return NULL;
        /* Flatten one block into defer body */
        for (int k = 0; k < blk->nbody; k++) {
            if (n->nbody >= SHADOW_BODY_CAP) {
                parser_fail_body_cap(p, p_peek(p), "@defer body");
                return NULL;
            }
            n->body[n->nbody++] = blk->body[k];
        }
        p_accept(p, TK_PUNCT, ";"); /* `@defer name: {…};` */
        return n;
    }
    AstNode* st = parse_stmt(p);
    if (!st) return NULL;
    n->body[0] = st;
    n->nbody = 1;
    return n;
}

/* Type * name = call(...);  (not nursery @destroy) */
/* Type * name ; — bare pointer decl (no initializer). */
static AstNode* parse_ptr_decl_stmt(Parser* p) {
    Token ty = p_peek(p);
    int is_void = shadow_kw(ty) == SHADOW_KW_VOID;
    if (ty.kind != TK_IDENT && !is_void) return NULL;
    if (p->i + 3 >= p->n) return NULL;
    if (!tok_eq(p->toks[p->i + 1], TK_PUNCT, "*")) return NULL;
    if (p->toks[p->i + 2].kind != TK_IDENT) return NULL;
    if (!tok_eq(p->toks[p->i + 3], TK_PUNCT, ";")) return NULL;
    p_next(p); /* type */
    p_next(p); /* * */
    Token name = p_next(p);
    p_next(p); /* ; */
    AstNode* n = ast_new(p, AST_VAR_DECL);
    if (!n) return NULL;
    if (is_void) {
        snprintf(n->a, sizeof(n->a), "void*");
    } else {
        char base[128];
        slice_to(base, sizeof(base), ty.spell);
        snprintf(n->a, sizeof(n->a), "%s*", base);
    }
    slice_to(n->b, sizeof(n->b), name.spell);
    return n;
}

static AstNode* parse_ptr_init(Parser* p) {
    /* Forms: T *x = …;  const T *x = …;  T const *x = …;
     * unsigned/signed T *x = …;  void *x = …; */
    int i0 = p->i;
    int ti = i0;
    char tyn[96];
    size_t to = 0;
    int is_void = 0;
    Token name;
    if (ti < p->n && shadow_kw(p->toks[ti]) == SHADOW_KW_CONST) ti++;
    if (ti >= p->n) return NULL;
    if (p->toks[ti].kind == TK_IDENT &&
        (spell_eq(p->toks[ti].spell, "unsigned") ||
         spell_eq(p->toks[ti].spell, "signed"))) {
        ti++;
        if (ti >= p->n || p->toks[ti].kind != TK_IDENT) return NULL;
    }
    {
        Token ty = p->toks[ti];
        is_void = shadow_kw(ty) == SHADOW_KW_VOID;
        if (ty.kind != TK_IDENT && !is_void && shadow_kw(ty) != SHADOW_KW_CHAR &&
            shadow_kw(ty) != SHADOW_KW_INT)
            return NULL;
        ti++;
    }
    /* East-const: `char const *`. */
    if (ti < p->n && shadow_kw(p->toks[ti]) == SHADOW_KW_CONST) ti++;
    if (ti + 2 >= p->n) return NULL;
    if (!tok_eq(p->toks[ti], TK_PUNCT, "*")) return NULL;
    if (p->toks[ti + 1].kind != TK_IDENT) return NULL;
    if (!tok_eq(p->toks[ti + 2], TK_PUNCT, "=")) return NULL;
    /* Nursery form takes precedence */
    {
        int j = ti + 3;
        int depth = 0;
        while (j < p->n) {
            Token t = p->toks[j];
            if (tok_eq(t, TK_PUNCT, "(")) depth++;
            else if (tok_eq(t, TK_PUNCT, ")")) depth--;
            else if (depth == 0 && tok_eq(t, TK_PUNCT, "!>")) return NULL;
            else if (depth == 0 && tok_eq(t, TK_PUNCT, ";")) break;
            j++;
        }
    }
    if (!ast_spell_token_range(p, i0, ti, tyn, sizeof(tyn)) &&
        !span_text(p, i0, ti, tyn, sizeof(tyn)))
        return NULL;
    while (tyn[to]) to++;
    while (to && (tyn[to - 1] == ' ' || tyn[to - 1] == '\t')) tyn[--to] = 0;
    p->i = ti;
    p_next(p); /* * */
    name = p_next(p);
    p_next(p); /* = */
    {
        int c0 = p->i;
        int depth = 0;
        AstNode* n;
        while (p->i < p->n) {
            Token t = p_peek(p);
            if (tok_eq(t, TK_PUNCT, "(") || tok_eq(t, TK_PUNCT, "[")) depth++;
            else if (tok_eq(t, TK_PUNCT, ")") || tok_eq(t, TK_PUNCT, "]"))
                depth--;
            else if (depth == 0 && tok_eq(t, TK_PUNCT, ";")) break;
            p_next(p);
        }
        n = ast_new(p, AST_PTR_INIT);
        if (!n) return NULL;
        if (is_void)
            snprintf(n->a, sizeof(n->a), "void");
        else
            snprintf(n->a, sizeof(n->a), "%s", tyn);
        slice_to(n->b, sizeof(n->b), name.spell);
        if (!span_text(p, c0, p->i, n->c, sizeof(n->c))) {
            parser_fail(p, name, "ptr init expr too long");
            return NULL;
        }
        if (!p_accept(p, TK_PUNCT, ";")) {
            parser_fail(p, p_peek(p), "expected ';' after ptr init");
            return NULL;
        }
        return n;
    }
}

/* call(...) !>;  or  call(...) !> { stmts }; */
static AstNode* parse_val_destroy(Parser* p) {
    Token ty = p_peek(p);
    if (ty.kind != TK_IDENT) return NULL;
    if (p->i + 4 >= p->n) return NULL;
    int is_ptr = 0;
    int name_i = p->i + 1;
    if (tok_eq(p->toks[p->i + 1], TK_PUNCT, "*")) {
        is_ptr = 1;
        name_i = p->i + 2;
        if (name_i + 2 >= p->n) return NULL;
    }
    if (p->toks[name_i].kind != TK_IDENT) return NULL;
    if (!tok_eq(p->toks[name_i + 1], TK_PUNCT, "=")) return NULL;
    /* Must see @destroy / @detach before ';' at depth 0. */
    int j = name_i + 2;
    int depth = 0;
    int saw = 0;
    int is_detach = 0;
    while (j < p->n) {
        Token t = p->toks[j];
        if (tok_eq(t, TK_PUNCT, "(") || tok_eq(t, TK_PUNCT, "{") || tok_eq(t, TK_PUNCT, "["))
            depth++;
        else if (tok_eq(t, TK_PUNCT, ")") || tok_eq(t, TK_PUNCT, "}") || tok_eq(t, TK_PUNCT, "]"))
            depth--;
        else if (depth == 0 && tok_eq(t, TK_PUNCT, "@") && j + 1 < p->n) {
            ShadowKwKind ak = shadow_kw(p->toks[j + 1]);
            if (ak == SHADOW_KW_DESTROY || ak == SHADOW_KW_DETACH) {
                saw = 1;
                is_detach = (ak == SHADOW_KW_DETACH);
                break;
            }
        } else if (depth == 0 && tok_eq(t, TK_PUNCT, ";")) {
            return NULL;
        }
        j++;
    }
    if (!saw) return NULL;

    p_next(p); /* type */
    if (is_ptr) p_next(p); /* * */
    Token name = p_next(p);
    p_next(p); /* = */
    int c0 = p->i;
    char init[256];
    AstNode* create_cl = NULL;
    int had_cl = parse_call_arg_closure(p, c0, j, init, sizeof(init), "create",
                                        &create_cl);
    if (p->err) return NULL;
    if (!had_cl) {
        while (p->i < j) p_next(p);
        if (!ast_spell_token_range(p, c0, j, init, sizeof(init)) &&
            !span_text(p, c0, j, init, sizeof(init))) {
            parser_fail(p, name, "val @destroy/@detach init too long");
            return NULL;
        }
    }
    {
        AstNode* ue = (!had_cl) ? parse_ufcs_expr_range(p, c0, j) : NULL;
        p_next(p); /* @ */
        p_next(p); /* destroy|detach */
        AstNode* n = ast_new(p, AST_VAL_DESTROY);
        if (!n) return NULL;
        slice_to(n->a, sizeof(n->a), ty.spell);
        slice_to(n->b, sizeof(n->b), name.spell);
        snprintf(n->c, sizeof(n->c), "%s", init);
        if (is_ptr) snprintf(n->d, sizeof(n->d), "*");
        if (ue) (void)ast_attach_ufcs_kid(p, n, ue);
        /* Closure on dbody (like UFCS) — not kids_storage (fn may still append). */
        if (create_cl) {
            if (n->ndbody >= SHADOW_BODY_CAP) {
                parser_fail(p, name, "too many @create closure attachments");
                return NULL;
            }
            spawn_infer_value_caps(create_cl);
            n->dbody[n->ndbody++] = create_cl;
        }
        if (is_detach) {
            if (!p_accept(p, TK_PUNCT, ";")) {
                parser_fail(p, p_peek(p), "expected ';' after @detach");
                return NULL;
            }
            snprintf(n->e, sizeof(n->e), "_detach");
            return n;
        }
        if (p_accept(p, TK_PUNCT, ";")) {
            snprintf(n->e, sizeof(n->e), "_Dbare");
            return n;
        }
        if (!p_accept(p, TK_PUNCT, "{")) {
            parser_fail(p, p_peek(p), "expected ';' or '{' after @destroy");
            return NULL;
        }
        while (!tok_eq(p_peek(p), TK_PUNCT, "}") && p_peek(p).kind != TK_EOF &&
               !p->err) {
            AstNode* s = parse_stmt(p);
            if (!s) return NULL;
            if (n->nbody >= SHADOW_BODY_CAP) {
                parser_fail_body_cap(p, p_peek(p), "@destroy body");
                return NULL;
            }
            n->body[n->nbody++] = s;
        }
        if (!p_accept(p, TK_PUNCT, "}") || !p_accept(p, TK_PUNCT, ";")) {
            parser_fail(p, p_peek(p), "expected '};' after @destroy { ... }");
            return NULL;
        }
        snprintf(n->e, sizeof(n->e), "_D");
        return n;
    }
}

/* Source bytes for toks[i0 .. i1) (exclusive end token index). */
static int pp_stmt_tok_span(Parser* p, int i0, int i1, const char** out_s,
                            size_t* out_n) {
    FileTape* ft;
    size_t start, end;
    if (!p || i0 < 0 || i1 <= i0 || i1 > p->n) return 0;
    ft = (p->cache) ? tape_by_id(p->cache, p->toks[i0].file_id) : NULL;
    if (!ft || !ft->bytes) return 0;
    start = p->toks[i0].offset;
    end = p->toks[i1 - 1].offset + p->toks[i1 - 1].spell.len;
    if (end < start || end > ft->len) return 0;
    *out_s = ft->bytes + start;
    *out_n = end - start;
    return 1;
}

/* PpStmt owns recognition for break/continue/goto/label/raw_line shapes. */
static int pp_stmt_owns(Parser* p, int i0, int i1) {
    const char* s;
    size_t n;
    if (!pp_stmt_tok_span(p, i0, i1, &s, &n)) return 0;
    return PpStmt_match(s, n);
}

/* break; or continue; — shape owned by PpStmt. */
static AstNode* parse_break_continue(Parser* p) {
    ShadowKwKind kw = shadow_kw(p_peek(p));
    if (kw != SHADOW_KW_BREAK && kw != SHADOW_KW_CONTINUE) return NULL;
    if (p->i + 1 >= p->n || !tok_eq(p->toks[p->i + 1], TK_PUNCT, ";")) {
        p->err = 1;
        return NULL;
    }
    if (!pp_stmt_owns(p, p->i, p->i + 2)) {
        p->err = 1;
        return NULL;
    }
    p_next(p);
    p_next(p); /* ; */
    return ast_new(p, kw == SHADOW_KW_BREAK ? AST_BREAK : AST_CONTINUE);
}

/* goto label; — must win over `Type name;` (goto is an IDENT). */
static AstNode* parse_goto(Parser* p) {
    Token t = p_peek(p);
    Token lab;
    if (t.kind != TK_IDENT || !spell_eq(t.spell, "goto")) return NULL;
    if (p->i + 2 >= p->n || p->toks[p->i + 1].kind != TK_IDENT ||
        !tok_eq(p->toks[p->i + 2], TK_PUNCT, ";")) {
        if (p->i + 1 >= p->n || p->toks[p->i + 1].kind != TK_IDENT)
            parser_fail(p, p->i + 1 < p->n ? p->toks[p->i + 1] : t,
                        "expected label after goto");
        else
            parser_fail(p, p_peek(p), "expected ';' after goto label");
        return NULL;
    }
    if (!pp_stmt_owns(p, p->i, p->i + 3)) {
        parser_fail(p, t, "goto form rejected by PpStmt grammar");
        return NULL;
    }
    p_next(p); /* goto */
    lab = p_next(p);
    p_next(p); /* ; */
    {
        AstNode* n = ast_new(p, AST_GOTO);
        if (!n) return NULL;
        slice_to(n->a, sizeof(n->a), lab.spell);
        return n;
    }
}

/* label: — statement label (before another stmt or alone before `}`). */
static AstNode* parse_label(Parser* p) {
    Token t = p_peek(p);
    if (t.kind != TK_IDENT) return NULL;
    if (p->i + 1 >= p->n || !tok_eq(p->toks[p->i + 1], TK_PUNCT, ":"))
        return NULL;
    /* Avoid `case` / `default` (switch) and digraph typenames. */
    if (spell_eq(t.spell, "case") || spell_eq(t.spell, "default")) return NULL;
    if (!pp_stmt_owns(p, p->i, p->i + 2)) return NULL;
    p_next(p);
    p_next(p); /* : */
    {
        AstNode* n = ast_new(p, AST_LABEL);
        if (!n) return NULL;
        slice_to(n->a, sizeof(n->a), t.spell);
        return n;
    }
}

/* do { stmts } while ( cond ) ; */
static AstNode* parse_do_while(Parser* p) {
    if (shadow_kw(p_peek(p)) != SHADOW_KW_DO) return NULL;
    p_next(p);
    if (!tok_eq(p_peek(p), TK_PUNCT, "{")) {
        parser_fail(p, p_peek(p), "expected '{' after do");
        return NULL;
    }
    p_next(p);
    AstNode* n = ast_new(p, AST_DO_WHILE);
    if (!n) return NULL;
    while (!tok_eq(p_peek(p), TK_PUNCT, "}") && p_peek(p).kind != TK_EOF && !p->err) {
        AstNode* s = parse_stmt(p);
        if (!s) return NULL;
        if (n->nbody >= SHADOW_BODY_CAP) {
            parser_fail_body_cap(p, p_peek(p), "do body");
            return NULL;
        }
        n->body[n->nbody++] = s;
    }
    if (!p_accept(p, TK_PUNCT, "}")) {
        parser_fail(p, p_peek(p), "expected '}' to close do");
        return NULL;
    }
    if (shadow_kw(p_peek(p)) != SHADOW_KW_WHILE) {
        parser_fail(p, p_peek(p), "expected 'while' after do { ... }");
        return NULL;
    }
    p_next(p);
    if (!tok_eq(p_peek(p), TK_PUNCT, "(")) {
        parser_fail(p, p_peek(p), "expected '(' after do ... while");
        return NULL;
    }
    int c0 = p->i + 1;
    if (!skip_parens(p)) {
        parser_fail(p, p_peek(p), "unterminated do ... while (...)");
        return NULL;
    }
    int c1 = p->i - 1;
    if (c0 < c1) {
        if (!span_text(p, c0, c1, n->a, sizeof(n->a))) {
            parser_fail(p, p_peek(p), "do while condition too long");
            return NULL;
        }
    }
    if (!p_accept(p, TK_PUNCT, ";")) {
        parser_fail(p, p_peek(p), "expected ';' after do ... while (...)");
        return NULL;
    }
    return n;
}

/* for ( hdr ) { stmts } | for ( hdr ) stmt */
static AstNode* parse_for(Parser* p) {
    if (shadow_kw(p_peek(p)) != SHADOW_KW_FOR) return NULL;
    p_next(p);
    if (!tok_eq(p_peek(p), TK_PUNCT, "(")) {
        parser_fail(p, p_peek(p), "expected '(' after for");
        return NULL;
    }
    int h0 = p->i + 1;
    if (!skip_parens(p)) {
        parser_fail(p, p_peek(p), "unterminated for (...)");
        return NULL;
    }
    int h1 = p->i - 1;
    AstNode* n = ast_new(p, AST_FOR);
    if (!n) return NULL;
    if (h0 >= h1) n->a[0] = 0;
    else if (!span_text(p, h0, h1, n->a, sizeof(n->a))) {
        parser_fail(p, p_peek(p), "for header too long");
        return NULL;
    }
    if (tok_eq(p_peek(p), TK_PUNCT, "{")) {
        p_next(p); /* { */
        while (!tok_eq(p_peek(p), TK_PUNCT, "}") && p_peek(p).kind != TK_EOF &&
               !p->err) {
            AstNode* s = parse_stmt(p);
            if (!s) return NULL;
            if (n->nbody >= SHADOW_BODY_CAP) {
                parser_fail_body_cap(p, p_peek(p), "for body");
                return NULL;
            }
            n->body[n->nbody++] = s;
        }
        if (!p_accept(p, TK_PUNCT, "}")) {
            parser_fail(p, p_peek(p), "expected '}' to close for");
            return NULL;
        }
    } else {
        AstNode* s = parse_stmt(p);
        if (!s) return NULL;
        n->body[0] = s;
        n->nbody = 1;
    }
    return n;
}

/* CC_ARRAY_MAP_FOREACH(h, k, v) stmt — lower to dense-row for (std macro). */
static AstNode* parse_array_map_foreach(Parser* p) {
    Token kw = p_peek(p);
    char h[80], k[80], v[80];
    int a0, a1, c1, c2, i, depth;
    AstNode* n;
    if (kw.kind != TK_IDENT || !spell_eq(kw.spell, "CC_ARRAY_MAP_FOREACH"))
        return NULL;
    if (p->i + 1 >= p->n || !tok_eq(p->toks[p->i + 1], TK_PUNCT, "("))
        return NULL;
    p_next(p); /* CC_ARRAY_MAP_FOREACH */
    a0 = p->i + 1;
    if (!skip_parens(p)) {
        parser_fail(p, p_peek(p), "unterminated CC_ARRAY_MAP_FOREACH(...)");
        return NULL;
    }
    a1 = p->i - 1; /* ')' */
    c1 = c2 = -1;
    depth = 0;
    for (i = a0; i < a1; i++) {
        Token t = p->toks[i];
        if (tok_eq(t, TK_PUNCT, "(") || tok_eq(t, TK_PUNCT, "[") ||
            tok_eq(t, TK_PUNCT, "{"))
            depth++;
        else if (tok_eq(t, TK_PUNCT, ")") || tok_eq(t, TK_PUNCT, "]") ||
                 tok_eq(t, TK_PUNCT, "}"))
            depth--;
        else if (depth == 0 && tok_eq(t, TK_PUNCT, ",")) {
            if (c1 < 0) c1 = i;
            else if (c2 < 0) c2 = i;
            else {
                parser_fail(p, t, "CC_ARRAY_MAP_FOREACH expects 3 args");
                return NULL;
            }
        }
    }
    if (c1 < 0 || c2 < 0 ||
        !span_text(p, a0, c1, h, sizeof(h)) ||
        !span_text(p, c1 + 1, c2, k, sizeof(k)) ||
        !span_text(p, c2 + 1, a1, v, sizeof(v))) {
        parser_fail(p, kw, "CC_ARRAY_MAP_FOREACH(h, k, v) args");
        return NULL;
    }
    n = ast_new(p, AST_FOR);
    if (!n) return NULL;
    if (snprintf(n->a, sizeof(n->a),
                 "size_t __cc_am_i = 0; "
                 "(%s) && __cc_am_i < (%s)->len && "
                 "(((%s) = (%s)->dense[__cc_am_i].key), "
                 "((%s) = (%s)->dense[__cc_am_i].val), 1); "
                 "++__cc_am_i",
                 h, h, k, h, v, h) >= (int)sizeof(n->a)) {
        parser_fail(p, kw, "CC_ARRAY_MAP_FOREACH header too long");
        return NULL;
    }
    if (tok_eq(p_peek(p), TK_PUNCT, "{")) {
        p_next(p); /* { */
        while (!tok_eq(p_peek(p), TK_PUNCT, "}") && p_peek(p).kind != TK_EOF &&
               !p->err) {
            AstNode* s = parse_stmt(p);
            if (!s) return NULL;
            if (n->nbody >= SHADOW_BODY_CAP) {
                parser_fail_body_cap(p, p_peek(p), "foreach body");
                return NULL;
            }
            n->body[n->nbody++] = s;
        }
        if (!p_accept(p, TK_PUNCT, "}")) {
            parser_fail(p, p_peek(p), "expected '}' to close CC_ARRAY_MAP_FOREACH");
            return NULL;
        }
    } else {
        AstNode* s = parse_stmt(p);
        if (!s) return NULL;
        n->body[0] = s;
        n->nbody = 1;
    }
    return n;
}

/* [const] Type[*] name [dims] = expr|{init} ;  (no unwrap / @destroy)
 * Beachhead also accepts anonymous `struct { … }` types (reflect smokes). */
static AstNode* parse_typed_init(Parser* p) {
    int has_const = 0;
    int has_volatile = 0;
    int has_atomic = 0;
    int has_struct = 0;
    int is_anon_struct = 0;
    int start_i = p->i;
    int ti = start_i;
    /* Leading cv-qualifiers: `const` / `volatile` / `_Atomic` (any order). */
    while (ti < p->n) {
        if (shadow_kw(p->toks[ti]) == SHADOW_KW_CONST) {
            has_const = 1;
            ti++;
            continue;
        }
        if (spell_eq(p->toks[ti].spell, "volatile")) {
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
    if (ti != start_i && p->i + 4 >= p->n) return NULL;
    if (ti < p->n && shadow_kw(p->toks[ti]) == SHADOW_KW_STRUCT) {
        has_struct = 1;
        ti++;
        if (ti >= p->n) return NULL;
    }
    Token ty = p->toks[ti];
    /* Retired angle-bracket generics: Vec<T> / Map<K,V>. */
    if (!has_struct && ty.kind == TK_IDENT && ti + 1 < p->n &&
        tok_eq(p->toks[ti + 1], TK_PUNCT, "<") &&
        (spell_eq(ty.spell, "Vec") || spell_eq(ty.spell, "Map"))) {
        if (spell_eq(ty.spell, "Vec"))
            parser_fail(p, ty, "retired generic spelling 'Vec<T>'");
        else
            parser_fail(p, ty, "retired generic spelling 'Map<K, V>'");
        return NULL;
    }
    if (has_struct && tok_eq(ty, TK_PUNCT, "{")) {
        is_anon_struct = 1;
    } else if (ty.kind != TK_IDENT && shadow_kw(ty) != SHADOW_KW_BOOL &&
               shadow_kw(ty) != SHADOW_KW_SIZE_T &&
               shadow_kw(ty) != SHADOW_KW_INT &&
               shadow_kw(ty) != SHADOW_KW_CHAR)
        return NULL;
    if (ti + 3 >= p->n) return NULL;
    int ty0 = has_struct ? start_i + (has_const || has_volatile || has_atomic ? 1 : 0) : ti;
    /* When both const and volatile precede `struct`, ty0 is after both. */
    if (has_struct) ty0 = ti - 1; /* `struct` token index */
    int ty_end = has_struct ? -1 : peek_generic_type_end(p, ti);
    if (!has_struct && ty_end < 0) {
        int iend = peek_c_int_type_end(p, ti);
        if (iend > ti) ty_end = iend;
    }
    if (is_anon_struct) {
        int depth = 0;
        int j = ti;
        for (; j < p->n; j++) {
            if (tok_eq(p->toks[j], TK_PUNCT, "{")) depth++;
            else if (tok_eq(p->toks[j], TK_PUNCT, "}")) {
                depth--;
                if (depth == 0) {
                    j++;
                    break;
                }
            }
        }
        if (depth != 0 || j >= p->n) return NULL;
        ty_end = j;
    }
    /* `T[:…]` / `char[:0]` / `char[::]` / `char[:!]` → CCSlice[_T]
     * (unique forms record `!` for safety). */
    int slice_ty_end = -1;
    int slice_unique = 0;
    int slice_elem0 = ti;
    int slice_elem1 = -1;
    if (!has_struct && !is_anon_struct) {
        int te = (ty_end > 0) ? ty_end : peek_slice_elem_type_end(p, ti, p->n);
        if (te > ti && te < p->n && tok_eq(p->toks[te], TK_PUNCT, "[")) {
            int mend = peek_slice_brack_end(p, te, p->n, &slice_unique);
            if (mend > 0) {
                slice_ty_end = mend;
                slice_elem1 = te;
                ty_end = mend; /* consume through `]` */
            }
        }
    }
    int is_type_slice = (slice_ty_end > 0);
    int ni = (ty_end > 0) ? ty_end : (is_type_slice ? slice_ty_end : ti + 1);
    int decl_i = ni; /* start of interleaved `*` / cv declarator prefix */
    int nstars = 0;
    int post_cv_const = 0;
    int post_cv_volatile = 0;
    /* `*`, `* const`, `* const *`, `**`, … — cv may sit between stars. */
    for (;;) {
        int advanced = 0;
        while (ni < p->n && tok_eq(p->toks[ni], TK_PUNCT, "*")) {
            nstars++;
            ni++;
            advanced = 1;
        }
        while (ni < p->n && p->toks[ni].kind == TK_IDENT) {
            if (shadow_kw(p->toks[ni]) == SHADOW_KW_CONST) {
                post_cv_const = 1;
                ni++;
                advanced = 1;
                continue;
            }
            if (spell_eq(p->toks[ni].spell, "volatile")) {
                post_cv_volatile = 1;
                ni++;
                advanced = 1;
                continue;
            }
            break;
        }
        if (!advanced) break;
    }
    if (ni >= p->n || p->toks[ni].kind != TK_IDENT) return NULL;
    int name_i = ni;
    ni++;
    /* Optional array dims: name[expr] */
    int dims_start = -1, dims_end = -1;
    if (ni < p->n && tok_eq(p->toks[ni], TK_PUNCT, "[")) {
        dims_start = ni;
        int depth = 0;
        while (ni < p->n) {
            if (tok_eq(p->toks[ni], TK_PUNCT, "[")) depth++;
            else if (tok_eq(p->toks[ni], TK_PUNCT, "]")) {
                depth--;
                if (depth == 0) { ni++; break; }
            }
            ni++;
        }
        dims_end = ni;
    }
    /* `const T* name[N];` (no initializer) or `const T* name = …;`. */
    int has_init = (ni < p->n && tok_eq(p->toks[ni], TK_PUNCT, "="));
    if (ni >= p->n) return NULL;
    if (!has_init && !tok_eq(p->toks[ni], TK_PUNCT, ";")) return NULL;
    int j = ni;
    if (has_init) {
        /* Reject if later unwrap / @destroy (other parsers own those). */
        j = ni + 1;
        int depth = 0;
        while (j < p->n) {
            Token t = p->toks[j];
            if (tok_eq(t, TK_PUNCT, "(") || tok_eq(t, TK_PUNCT, "{") ||
                tok_eq(t, TK_PUNCT, "["))
                depth++;
            else if (tok_eq(t, TK_PUNCT, ")") || tok_eq(t, TK_PUNCT, "}") ||
                     tok_eq(t, TK_PUNCT, "]"))
                depth--;
            else if (depth == 0 &&
                     (tok_eq(t, TK_PUNCT, "!>") || tok_eq(t, TK_PUNCT, "?>")))
                return NULL;
            else if (depth == 0 && tok_eq(t, TK_PUNCT, "@")) {
                if (j + 1 < p->n &&
                    shadow_kw(p->toks[j + 1]) == SHADOW_KW_DESTROY)
                    return NULL;
            } else if (depth == 0 && tok_eq(t, TK_PUNCT, ";"))
                break;
            j++;
        }
        if (j >= p->n || !tok_eq(p->toks[j], TK_PUNCT, ";")) return NULL;
    }

    /* Consume leading cv-qualifiers. For `struct Tag`, ti is the Tag — stop
     * before `struct` (ty0) so the has_struct path can consume both tokens. */
    while (p->i < (has_struct ? ty0 : ti)) p_next(p);
    char tytxt[256];
    if (is_anon_struct) {
        if (!ast_spell_token_range(p, ty0, ty_end, tytxt, sizeof(tytxt)) &&
            !span_text(p, ty0, ty_end, tytxt, sizeof(tytxt))) {
            parser_fail(p, ty, "anonymous struct type too long");
            return NULL;
        }
        while (p->i < ty_end) p_next(p);
    } else if (has_struct) {
        p_next(p); /* struct */
        p_next(p); /* Tag */
        snprintf(tytxt, sizeof(tytxt), "struct %.*s", (int)ty.spell.len,
                 ty.spell.ptr);
    } else if (ty_end > 0) {
        if (!ast_spell_type_tokens(p, ty0, ty_end, tytxt, sizeof(tytxt)) &&
            !ast_spell_token_range(p, ty0, ty_end, tytxt, sizeof(tytxt)))
            return NULL;
        while (p->i < ty_end) p_next(p);
        /* Pair::[int,double] → Pair_int_double; seed so a later bare use
         * passes the CamelCase unknown-type gate before factory emit. */
        shadow_seed_spelled_type_name(p, tytxt);
    } else {
        p_next(p);
        slice_to(tytxt, sizeof(tytxt), ty.spell);
        /* Bare CamelCase IDENT types must be known — catch
         * `SomeUndefinedType` early (stdlib `int64_t` / snake_case pass). */
        if (!has_struct && ty.kind == TK_IDENT &&
            shadow_kw(ty) == SHADOW_KW_NONE &&
            tytxt[0] >= 'A' && tytxt[0] <= 'Z' &&
            !scope_is_typedef(p, ty.spell) &&
            !(tytxt[0] == 'C' && tytxt[1] == 'C') &&
            !(tytxt[0] == 'c' && tytxt[1] == 'c' && tytxt[2] == '_')) {
            char msg[160];
            snprintf(msg, sizeof(msg), "unknown type name '%s'", tytxt);
            parser_fail(p, ty, msg);
            return NULL;
        }
    }
    while (p->i < name_i) p_next(p); /* `*` / cv declarator prefix */
    Token name = p_next(p);
    char dims_txt[128] = {0};
    int decl_slice_unique = 0;
    if (dims_start >= 0) {
        int duniq = 0;
        int dend = peek_slice_brack_end(p, dims_start, dims_end, &duniq);
        if (dend == dims_end && !is_type_slice) {
            /* Declarator `T name[:…]` — fold marker into the type. */
            char elem[64];
            char sty[96];
            int e1 = peek_slice_elem_type_end(p, ti, name_i);
            if (e1 < 0) e1 = ti + 1;
            if (shadow_kw(ty) == SHADOW_KW_CHAR && e1 == ti + 1)
                snprintf(sty, sizeof(sty), "%s",
                         duniq ? "CCSliceUnique" : "CCSlice");
            else {
                ast_mangle_slice_elem(p, ti, e1, elem, sizeof(elem));
                snprintf(sty, sizeof(sty), "CCSlice_%s", elem);
            }
            snprintf(tytxt, sizeof(tytxt), "%s", sty);
            decl_slice_unique = duniq;
            while (p->i < dims_end) p_next(p);
        } else {
            if (!span_text(p, dims_start, dims_end, dims_txt, sizeof(dims_txt)))
                return NULL;
            while (p->i < dims_end) p_next(p);
        }
    }
    char expr[512];
    AstNode* call_cl = NULL;
    AstNode* ue = NULL;
    expr[0] = 0;
    if (has_init) {
        p_next(p); /* = */
        int c0 = p->i;
        /* Match AstNode.c — compound inits (reflect field tables) need headroom. */
        int had_cl = parse_call_arg_closure(p, c0, j, expr, sizeof(expr),
                                            "callarg", &call_cl);
        if (p->err) return NULL;
        if (!had_cl) {
            while (p->i < j) p_next(p);
            ue = parse_ufcs_expr_range(p, c0, j);
            /* Empty RHS (`int x = ;`): TCC-shaped diagnostic with .cch provenance. */
            if (c0 >= j) {
                FileTape* ft =
                    p->cache ? tape_by_id(p->cache, name.file_id) : NULL;
                const char* path = (ft && ft->path) ? ft->path : "<input>";
                const char* tests = strstr(path, "tests/");
                int oline = 1, ocol = 1;
                if (ft && ft->bytes)
                    offset_to_linecol(ft->bytes, ft->len, name.offset, &oline,
                                      &ocol);
                fprintf(stderr,
                        "%s:%d: error: expression expected before ';'\n",
                        tests ? tests : path, oline);
                p->err = 1;
                snprintf(p->err_msg, sizeof(p->err_msg),
                         "expression expected before ';'");
                return NULL;
            } else if (!ast_spell_token_range(p, c0, j, expr, sizeof(expr))) {
                parser_fail(p, name, "typed init expr too long");
                return NULL;
            }
        }
    }
    p_next(p); /* ; */
    AstNode* n = ast_new(p, has_init ? AST_TYPED_INIT : AST_VAR_DECL);
    if (!n) return NULL;
    {
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
        if (pref[0])
            snprintf(n->a, sizeof(n->a), "%s%s", pref, tytxt);
        else
            snprintf(n->a, sizeof(n->a), "%s", tytxt);
    }
    slice_to(n->b, sizeof(n->b), name.spell);
    if (has_init) snprintf(n->c, sizeof(n->c), "%s", expr);
    else if (dims_txt[0])
        snprintf(n->c, sizeof(n->c), "%s", dims_txt); /* VAR_DECL dims slot */
    else
        n->c[0] = 0;
    if (name_i > decl_i) {
        /* Preserve order: `* const *` ≠ `** const`. */
        if (!ast_spell_token_range(p, decl_i, name_i, n->d, sizeof(n->d)) &&
            !span_text(p, decl_i, name_i, n->d, sizeof(n->d))) {
            /* Fall back to stars-then-cv (single post-cv cluster). */
            size_t di = 0;
            int s;
            for (s = 0; s < nstars && di + 1 < sizeof(n->d); s++)
                n->d[di++] = '*';
            if (post_cv_const && di + 7 < sizeof(n->d)) {
                memcpy(n->d + di, " const", 6);
                di += 6;
            }
            if (post_cv_volatile && di + 10 < sizeof(n->d)) {
                memcpy(n->d + di, " volatile", 9);
                di += 9;
            }
            n->d[di] = 0;
        }
    }
    /* No-init VAR_DECL emit uses a=type (with stars) and c=dims. */
    if (!has_init && n->d[0]) {
        size_t al = strlen(n->a);
        size_t dl = strlen(n->d);
        if (al + dl + 1 < sizeof(n->a)) {
            memcpy(n->a + al, n->d, dl + 1);
            n->d[0] = 0;
        }
    }
    if (has_init) {
        if (dims_txt[0]) snprintf(n->e, sizeof(n->e), "%s", dims_txt);
        else if (slice_unique || decl_slice_unique)
            snprintf(n->e, sizeof(n->e), "!"); /* safety: T[:!] unique fact */
    } else if (slice_unique || decl_slice_unique)
        snprintf(n->e, sizeof(n->e), "!");
    if (ue) (void)ast_attach_ufcs_kid(p, n, ue);
    if (call_cl) {
        if (n->ndbody >= SHADOW_BODY_CAP) {
            parser_fail(p, name, "too many call-arg closure attachments");
            return NULL;
        }
        /* `CCClosure1 inc = () => …` — later `inc(x)` may capture `inc`. */
        if (strstr(n->a, "CCClosure")) spawn_note_closure_local(n->b);
        spawn_infer_value_caps(call_cl);
        n->dbody[n->ndbody++] = call_cl;
    }
    return n;
}

/* After method IDENT at meth_i: optional `::[…]` then `(`.
 * Returns `(` index, or -1. Sets targ0/targ1 to exclusive type-arg span
 * inside the brackets (or -1,-1 when omitted). */
static int ufcs_paren_after_meth(Parser* p, int meth_i, int lim,
                                 int* targ0, int* targ1) {
    int j;
    int depth;
    int br;
    if (targ0) *targ0 = -1;
    if (targ1) *targ1 = -1;
    if (!p || meth_i < 0 || meth_i + 1 >= lim) return -1;
    if (p->toks[meth_i].kind != TK_IDENT) return -1;
    j = meth_i + 1;
    if (tok_eq(p->toks[j], TK_PUNCT, "(")) return j;
    if (j + 2 >= lim) return -1;
    if (!tok_eq(p->toks[j], TK_PUNCT, "::") ||
        !tok_eq(p->toks[j + 1], TK_PUNCT, "["))
        return -1;
    br = j + 1;
    depth = 0;
    for (j = br; j < lim; j++) {
        Token t = p->toks[j];
        if (tok_eq(t, TK_PUNCT, "[")) depth++;
        else if (tok_eq(t, TK_PUNCT, "]")) {
            depth--;
            if (depth == 0) {
                if (targ0) *targ0 = br + 1;
                if (targ1) *targ1 = j;
                j++;
                if (j < lim && tok_eq(p->toks[j], TK_PUNCT, "(")) return j;
                return -1;
            }
        }
    }
    return -1;
}

/* Build UFCS node from token spans (recv / meth / args [/ type args]). */
static AstNode* ast_make_ufcs(Parser* p, AstKind kind, int r0, int call_op,
                              int a0, int a1, int targ0, int targ1) {
    char recv[128];
    char targs[128];
    Token op, meth;
    AstNode* n;
    if (r0 < 0 || call_op <= r0 || call_op + 2 >= p->n) return NULL;
    if (!ast_spell_token_range(p, r0, call_op, recv, sizeof(recv)) &&
        !span_text(p, r0, call_op, recv, sizeof(recv)))
        return NULL;
    op = p->toks[call_op];
    meth = p->toks[call_op + 1];
    if (meth.kind != TK_IDENT) return NULL;
    n = ast_new(p, kind);
    if (!n) return NULL;
    snprintf(n->a, sizeof(n->a), "%s", recv);
    slice_to(n->b, sizeof(n->b), meth.spell);
    targs[0] = 0;
    if (targ0 >= 0 && targ1 > targ0) {
        if (!ast_spell_token_range(p, targ0, targ1, targs, sizeof(targs)) &&
            !span_text(p, targ0, targ1, targs, sizeof(targs)))
            return NULL;
    }
    /* e: arrow + optional member type args (`::T`). */
    if (tok_eq(op, TK_PUNCT, "->")) {
        if (targs[0])
            snprintf(n->e, sizeof(n->e), "->::%s", targs);
        else
            snprintf(n->e, sizeof(n->e), "->");
    } else if (targs[0]) {
        snprintf(n->e, sizeof(n->e), "::%s", targs);
    }
    if (a0 < a1) {
        /* Lift `(T x) => {…}` call args (same as assign / (void) / create).
         * Spelling the body into c[] leaves `=>` unlowered and overflows. */
        AstNode* cl = NULL;
        char cl_args[2048];
        int save_i = p->i;
        int had_cl = parse_call_arg_closure(p, a0, a1, cl_args, sizeof(cl_args),
                                            "callarg", &cl);
        if (p->err) return NULL;
        p->i = save_i;
        if (had_cl) {
            snprintf(n->c, sizeof(n->c), "%s", cl_args);
            if (cl) {
                if (n->ndbody >= SHADOW_BODY_CAP) {
                    parser_fail(p, p->toks[a0],
                                "too many UFCS closure attachments");
                    return NULL;
                }
                spawn_infer_value_caps(cl);
                n->dbody[n->ndbody++] = cl;
            }
        } else {
            if (!ast_spell_token_range(p, a0, a1, n->c, sizeof(n->c)) &&
                !span_text(p, a0, a1, n->c, sizeof(n->c)))
                return NULL;
            /* Nested UFCS: whole args, or each top-level comma piece. */
            {
                AstNode* nested = parse_ufcs_expr_range(p, a0, a1);
                if (nested) {
                    (void)ast_attach_ufcs_kid(p, n, nested);
                } else {
                    int s = a0;
                    int depth = 0;
                    int k;
                    for (k = a0; k <= a1; k++) {
                        int at_end = (k == a1);
                        int at_comma = 0;
                        if (!at_end) {
                            Token t = p->toks[k];
                            if (tok_eq(t, TK_PUNCT, "(") ||
                                tok_eq(t, TK_PUNCT, "[") ||
                                tok_eq(t, TK_PUNCT, "{"))
                                depth++;
                            else if (tok_eq(t, TK_PUNCT, ")") ||
                                     tok_eq(t, TK_PUNCT, "]") ||
                                     tok_eq(t, TK_PUNCT, "}"))
                                depth--;
                            else if (depth == 0 && tok_eq(t, TK_PUNCT, ","))
                                at_comma = 1;
                        }
                        if (at_comma || at_end) {
                            if (k > s) {
                                AstNode* piece = parse_ufcs_expr_range(p, s, k);
                                if (piece)
                                    (void)ast_attach_ufcs_kid(p, n, piece);
                            }
                            s = k + 1;
                        }
                    }
                }
            }
        }
    }
    /* Nested UFCS as receiver when recv is itself a call chain. */
    {
        AstNode* ruf = parse_ufcs_expr_range(p, r0, call_op);
        if (ruf) (void)ast_attach_ufcs_kid(p, n, ruf);
    }
    return n;
}

/* Exact-range UFCS expr: toks[i0..i1) == recv (.|->) meth [::[T]] ( args ). */
static AstNode* parse_ufcs_expr_range(Parser* p, int i0, int i1) {
    int j, depth, call_op, a0, a1, par, targ0, targ1;
    if (!p || i0 < 0 || i1 > p->n || i1 - i0 < 4) return NULL;
    if (p->toks[i0].kind != TK_IDENT && !tok_eq(p->toks[i0], TK_PUNCT, "*") &&
        !tok_eq(p->toks[i0], TK_PUNCT, "&") && !tok_eq(p->toks[i0], TK_PUNCT, "("))
        return NULL;
    depth = 0;
    call_op = -1;
    targ0 = targ1 = -1;
    for (j = i0; j < i1; j++) {
        Token t = p->toks[j];
        if (tok_eq(t, TK_PUNCT, "(") || tok_eq(t, TK_PUNCT, "[") ||
            tok_eq(t, TK_PUNCT, "{"))
            depth++;
        else if (tok_eq(t, TK_PUNCT, ")") || tok_eq(t, TK_PUNCT, "]") ||
                 tok_eq(t, TK_PUNCT, "}"))
            depth--;
        else if (depth == 0 &&
                 (tok_eq(t, TK_PUNCT, ".") || tok_eq(t, TK_PUNCT, "->")) &&
                 j + 1 < i1 && p->toks[j + 1].kind == TK_IDENT) {
            int t0, t1, pr;
            pr = ufcs_paren_after_meth(p, j + 1, i1, &t0, &t1);
            if (pr >= 0) {
                call_op = j;
                targ0 = t0;
                targ1 = t1;
            }
        }
    }
    if (call_op < 0 || call_op <= i0) return NULL;
    /* `a.foo() && b.bar()` / `x + 1 <= y->cap()` are not one UFCS call with
     * recv spanning the operator — peel lowers the trailing call in text. */
    {
        int d2 = 0, k;
        for (k = i0; k < call_op; k++) {
            Token t = p->toks[k];
            if (tok_eq(t, TK_PUNCT, "(") || tok_eq(t, TK_PUNCT, "[") ||
                tok_eq(t, TK_PUNCT, "{"))
                d2++;
            else if (tok_eq(t, TK_PUNCT, ")") || tok_eq(t, TK_PUNCT, "]") ||
                     tok_eq(t, TK_PUNCT, "}"))
                d2--;
            else if (d2 == 0 &&
                     (tok_eq(t, TK_PUNCT, "&&") || tok_eq(t, TK_PUNCT, "||") ||
                      tok_eq(t, TK_PUNCT, "?") || tok_eq(t, TK_PUNCT, ",") ||
                      tok_eq(t, TK_PUNCT, "+") || tok_eq(t, TK_PUNCT, "-") ||
                      tok_eq(t, TK_PUNCT, "*") || tok_eq(t, TK_PUNCT, "/") ||
                      tok_eq(t, TK_PUNCT, "%") || tok_eq(t, TK_PUNCT, "<") ||
                      tok_eq(t, TK_PUNCT, ">") || tok_eq(t, TK_PUNCT, "<=") ||
                      tok_eq(t, TK_PUNCT, ">=") || tok_eq(t, TK_PUNCT, "==") ||
                      tok_eq(t, TK_PUNCT, "!=") || tok_eq(t, TK_PUNCT, "<<") ||
                      tok_eq(t, TK_PUNCT, ">>")))
                return NULL;
            else if (d2 == 0 && tok_eq(t, TK_PUNCT, "&") && k + 1 < call_op &&
                     tok_eq(p->toks[k + 1], TK_PUNCT, "&"))
                return NULL;
            else if (d2 == 0 && tok_eq(t, TK_PUNCT, "|") && k + 1 < call_op &&
                     tok_eq(p->toks[k + 1], TK_PUNCT, "|"))
                return NULL;
        }
    }
    par = ufcs_paren_after_meth(p, call_op + 1, i1, &targ0, &targ1);
    if (par < 0 || !tok_eq(p->toks[par], TK_PUNCT, "(")) return NULL;
    a0 = par + 1;
    depth = 0;
    for (j = par; j < i1; j++) {
        Token t = p->toks[j];
        if (tok_eq(t, TK_PUNCT, "(")) depth++;
        else if (tok_eq(t, TK_PUNCT, ")")) {
            depth--;
            if (depth == 0) {
                a1 = j;
                if (j + 1 != i1) return NULL; /* must consume full range */
                return ast_make_ufcs(p, AST_UFCS_EXPR, i0, call_op, a0, a1,
                                     targ0, targ1);
            }
        }
    }
    return NULL;
}

/* Attach expr UFCS on dbody — never kids_storage (would interleave with the
 * enclosing fn/block kids list while that list is still being filled). */
static int ast_attach_ufcs_kid(Parser* p, AstNode* parent, AstNode* ue) {
    (void)p;
    if (!parent || !ue) return 0;
    if (parent->ndbody >= SHADOW_BODY_CAP) return 0;
    parent->dbody[parent->ndbody++] = ue;
    return 1;
}

/* recv.method(args);  or  store->field.method(args);  (+ optional !>;)
 * Also `@string(...).method(...)[!>;]` — template receiver. */
static AstNode* parse_ufcs_stmt(Parser* p) {
    int save = p->i;
    int await_prefix = 0;
    /* `@await tx.send(…) !>;` — same UFCS stmt shape with await edge. */
    if (tok_eq(p_peek(p), TK_PUNCT, "@") && p->i + 1 < p->n &&
        shadow_kw(p->toks[p->i + 1]) == SHADOW_KW_AWAIT) {
        await_prefix = 1;
        p_next(p);
        p_next(p);
    } else if (shadow_kw(p_peek(p)) == SHADOW_KW_AWAIT) {
        await_prefix = 1;
        p_next(p);
    }
    if (!(p_peek(p).kind == TK_IDENT ||
          (tok_eq(p_peek(p), TK_PUNCT, "@") && p->i + 1 < p->n &&
           shadow_kw(p->toks[p->i + 1]) == SHADOW_KW_STRING))) {
        p->i = save;
        return NULL;
    }
    /* Find trailing `.|->` IDENT [::[T]] ( … ) [!>] ; at depth 0. */
    int j = p->i;
    int depth = 0;
    int call_op = -1; /* index of . or -> before method */
    int targ0 = -1, targ1 = -1;
    int semi = -1;
    while (j < p->n) {
        Token t = p->toks[j];
        if (tok_eq(t, TK_PUNCT, "(") || tok_eq(t, TK_PUNCT, "[") || tok_eq(t, TK_PUNCT, "{"))
            depth++;
        else if (tok_eq(t, TK_PUNCT, ")") || tok_eq(t, TK_PUNCT, "]") || tok_eq(t, TK_PUNCT, "}"))
            depth--;
        else if (depth == 0 &&
                 (tok_eq(t, TK_PUNCT, ".") || tok_eq(t, TK_PUNCT, "->")) &&
                 j + 1 < p->n && p->toks[j + 1].kind == TK_IDENT) {
            int t0, t1, pr;
            pr = ufcs_paren_after_meth(p, j + 1, p->n, &t0, &t1);
            if (pr >= 0) {
                call_op = j;
                targ0 = t0;
                targ1 = t1;
            }
        } else if (depth == 0 && tok_eq(t, TK_PUNCT, ";")) {
            semi = j;
            break;
        }
        j++;
    }
    if (call_op < 0 || semi < 0 || call_op <= p->i) {
        p->i = save;
        return NULL;
    }
    /* `lhs = recv.meth(...);` is an assign, not a UFCS statement. */
    {
        int k, d = 0;
        for (k = p->i; k < call_op; k++) {
            Token t = p->toks[k];
            if (tok_eq(t, TK_PUNCT, "(") || tok_eq(t, TK_PUNCT, "[") ||
                tok_eq(t, TK_PUNCT, "{"))
                d++;
            else if (tok_eq(t, TK_PUNCT, ")") || tok_eq(t, TK_PUNCT, "]") ||
                     tok_eq(t, TK_PUNCT, "}"))
                d--;
            else if (d == 0 &&
                     (tok_eq(t, TK_PUNCT, "=") || tok_eq(t, TK_PUNCT, "+=") ||
                      tok_eq(t, TK_PUNCT, "-=") || tok_eq(t, TK_PUNCT, "*="))) {
                p->i = save;
                return NULL;
            }
        }
    }
    /* Optional !>` immediately before `;` */
    int end_call = semi;
    int is_bang = 0;
    if (semi > 0 && tok_eq(p->toks[semi - 1], TK_PUNCT, "!>")) {
        is_bang = 1;
        end_call = semi - 1;
    }

    int r0 = p->i;
    int par = ufcs_paren_after_meth(p, call_op + 1, semi, &targ0, &targ1);
    if (par < 0) {
        p->i = save;
        return NULL;
    }
    while (p->i < call_op) p_next(p);
    Token op = p_next(p); /* . or -> */
    Token meth = p_next(p);
    while (p->i < par) p_next(p); /* optional ::[…] */
    int a0 = p->i + 1;
    if (!skip_parens(p)) {
        parser_fail(p, p_peek(p), "unterminated UFCS call");
        return NULL;
    }
    int a1 = p->i - 1;
    AstNode* n = ast_make_ufcs(p, AST_UFCS_STMT, r0, call_op, a0, a1, targ0,
                               targ1);
    if (!n) {
        parser_fail(p, meth, "UFCS fields too long");
        return NULL;
    }
    (void)op;
    /* Re-scan: !>;  or  !>(bind){stmts};  or bare ; */
    if (tok_eq(p_peek(p), TK_PUNCT, "!>")) {
        p_next(p);
        if (tok_eq(p_peek(p), TK_PUNCT, "(")) {
            p_next(p);
            Token bind = p_next(p);
            if (bind.kind != TK_IDENT || !p_accept(p, TK_PUNCT, ")")) {
                parser_fail(p, p_peek(p), "expected !>(bind)");
                return NULL;
            }
            char bindn[64];
            slice_to(bindn, sizeof(bindn), bind.spell);
            snprintf(n->d, sizeof(n->d), "bang_block:%s", bindn);
            if (!p_accept(p, TK_PUNCT, "{")) {
                parser_fail(p, p_peek(p), "expected '{' after UFCS !>(bind)");
                return NULL;
            }
            while (!tok_eq(p_peek(p), TK_PUNCT, "}") && p_peek(p).kind != TK_EOF && !p->err) {
                AstNode* s = parse_stmt(p);
                if (!s) return NULL;
                if (n->nbody >= SHADOW_BODY_CAP) {
                    parser_fail_body_cap(p, p_peek(p), "UFCS !>(bind) body");
                    return NULL;
                }
                n->body[n->nbody++] = s;
            }
            if (!p_accept(p, TK_PUNCT, "}") || !p_accept(p, TK_PUNCT, ";")) {
                parser_fail(p, p_peek(p), "expected '};' after UFCS !>(bind) body");
                return NULL;
            }
            if (await_prefix) {
                /* bang_block:bind → bang_await_block:bind */
                char tmp[96];
                snprintf(tmp, sizeof(tmp), "bang_await_block:%s", bindn);
                snprintf(n->d, sizeof(n->d), "%s", tmp);
            }
            return n;
        }
        snprintf(n->d, sizeof(n->d), await_prefix ? "bang_await" : "bang");
        if (!p_accept(p, TK_PUNCT, ";")) {
            parser_fail(p, p_peek(p), "expected ';' after UFCS !>");
            return NULL;
        }
        return n;
    }
    if (await_prefix && !is_bang) {
        /* `@await tx.send(…);` without `!>` — still mark await for emit. */
        snprintf(n->d, sizeof(n->d), "await");
    }
    if (!p_accept(p, TK_PUNCT, ";")) {
        parser_fail(p, p_peek(p), "expected ';' after UFCS call");
        return NULL;
    }
    (void)is_bang;
    (void)end_call;
    return n;
}

/* lhs = rhs ;  or  lhs += rhs ;  (lhs may start with `*`) */
static AstNode* parse_assign_stmt(Parser* p) {
    if (p_peek(p).kind != TK_IDENT && !tok_eq(p_peek(p), TK_PUNCT, "*")) return NULL;
    /* Typed decls start with type keywords — typed_init / ptr_init own them.
     * Without this, `const char* const x[] = {…}` is stolen as assign and the
     * brace RHS spills a 288-byte spell buffer. */
    {
        ShadowKwKind sk = shadow_kw(p_peek(p));
        if (sk == SHADOW_KW_CONST || sk == SHADOW_KW_INT ||
            sk == SHADOW_KW_CHAR || sk == SHADOW_KW_BOOL ||
            sk == SHADOW_KW_SIZE_T || sk == SHADOW_KW_VOID ||
            sk == SHADOW_KW_STRUCT)
            return NULL;
    }
    int j = p->i;
    int depth = 0;
    int eq = -1;
    while (j < p->n) {
        Token t = p->toks[j];
        if (tok_eq(t, TK_PUNCT, "(") || tok_eq(t, TK_PUNCT, "[") || tok_eq(t, TK_PUNCT, "{"))
            depth++;
        else if (tok_eq(t, TK_PUNCT, ")") || tok_eq(t, TK_PUNCT, "]") || tok_eq(t, TK_PUNCT, "}"))
            depth--;
        else if (depth == 0 && eq < 0 &&
                 (tok_eq(t, TK_PUNCT, "!>") || tok_eq(t, TK_PUNCT, "?>"))) {
            /* Bare / lhs unwrap forms — var_unwrap / stmt_unwrap own them. */
            return NULL;
        } else if (depth == 0 && eq < 0 &&
                 (tok_eq(t, TK_PUNCT, "=") || tok_eq(t, TK_PUNCT, "+=") ||
                  tok_eq(t, TK_PUNCT, "-=") || tok_eq(t, TK_PUNCT, "*=") ||
                  tok_eq(t, TK_PUNCT, "/=") || tok_eq(t, TK_PUNCT, "%=") ||
                  tok_eq(t, TK_PUNCT, "&=") || tok_eq(t, TK_PUNCT, "|=") ||
                  tok_eq(t, TK_PUNCT, "^=") || tok_eq(t, TK_PUNCT, "<<=") ||
                  tok_eq(t, TK_PUNCT, ">>="))) {
            eq = j;
        } else if (depth == 0 && tok_eq(t, TK_PUNCT, ";")) {
            break;
        }
        /* After `=`: Form-P `sum += f() !>;` — keep scanning; emit rewrites. */
        j++;
    }
    if (eq < 0 || j >= p->n || !tok_eq(p->toks[j], TK_PUNCT, ";")) return NULL;
    /* Don't steal `Type name = …` (typed_init / val_destroy run first).
     * Still allow plain `name = …` when eq is immediately after the ident. */

    int l0 = p->i;
    while (p->i < eq) p_next(p);
    char lhs[128];
    if (!span_text(p, l0, eq, lhs, sizeof(lhs))) return NULL;
    Token op = p_next(p); /* = or += … */
    int r0 = p->i;
    char rhs[2048];
    AstNode* cl = NULL;
    int had_cl = parse_call_arg_closure(p, r0, j, rhs, sizeof(rhs), "callarg",
                                        &cl);
    if (p->err) return NULL;
    AstNode* ue = NULL;
    if (!had_cl) {
        while (p->i < j) p_next(p);
        ue = parse_ufcs_expr_range(p, r0, j);
        if (!ast_spell_token_range(p, r0, j, rhs, sizeof(rhs)) &&
            !span_text(p, r0, j, rhs, sizeof(rhs)))
            return NULL;
    }
    p_next(p); /* ; */
    AstNode* n = ast_new(p, AST_ASSIGN);
    if (!n) return NULL;
    snprintf(n->a, sizeof(n->a), "%s", lhs);
    snprintf(n->b, sizeof(n->b), "%s", rhs);
    slice_to(n->c, sizeof(n->c), op.spell);
    if (ue) (void)ast_attach_ufcs_kid(p, n, ue);
    if (cl) {
        if (n->ndbody >= SHADOW_BODY_CAP) {
            parser_fail(p, p_peek(p), "too many assign closure attachments");
            return NULL;
        }
        spawn_infer_value_caps(cl);
        n->dbody[n->ndbody++] = cl;
    }
    return n;
}

/* (void) expr ; */
static AstNode* parse_void_cast(Parser* p) {
    int e0;
    int e1;
    int depth = 0;
    AstNode* n;
    AstNode* cl = NULL;
    AstNode* ue;
    char expr[256];
    int had_cl;
    if (!tok_eq(p_peek(p), TK_PUNCT, "(")) return NULL;
    if (p->i + 3 >= p->n) return NULL;
    if (shadow_kw(p->toks[p->i + 1]) != SHADOW_KW_VOID) return NULL;
    if (!tok_eq(p->toks[p->i + 2], TK_PUNCT, ")")) return NULL;
    p_next(p); p_next(p); p_next(p); /* ( void ) */
    e0 = p->i;
    while (p->i < p->n) {
        Token t = p_peek(p);
        if (tok_eq(t, TK_PUNCT, "(") || tok_eq(t, TK_PUNCT, "[") || tok_eq(t, TK_PUNCT, "{"))
            depth++;
        else if (tok_eq(t, TK_PUNCT, ")") || tok_eq(t, TK_PUNCT, "]") || tok_eq(t, TK_PUNCT, "}"))
            depth--;
        else if (depth == 0 && tok_eq(t, TK_PUNCT, ";"))
            break;
        p_next(p);
    }
    if (e0 >= p->i) return NULL;
    e1 = p->i;
    n = ast_new(p, AST_VOID_CAST);
    if (!n) return NULL;
    /* Prefer structured call-arg closure so bodies are not spelled into a[256]. */
    had_cl = parse_call_arg_closure(p, e0, e1, expr, sizeof(expr), "callarg",
                                    &cl);
    if (p->err) return NULL;
    if (had_cl) {
        snprintf(n->a, sizeof(n->a), "%s", expr);
        if (cl) {
            if (n->ndbody >= SHADOW_BODY_CAP) {
                parser_fail(p, p_peek(p), "too many (void) closure attachments");
                return NULL;
            }
            spawn_infer_value_caps(cl);
            n->dbody[n->ndbody++] = cl;
        }
    } else {
        p->i = e1;
        ue = parse_ufcs_expr_range(p, e0, e1);
        if (!ast_spell_token_range(p, e0, e1, n->a, sizeof(n->a)) &&
            !span_text(p, e0, e1, n->a, sizeof(n->a))) {
            parser_fail(p, p_peek(p), "(void) expr too long");
            return NULL;
        }
        if (ue) (void)ast_attach_ufcs_kid(p, n, ue);
    }
    if (!p_accept(p, TK_PUNCT, ";")) {
        parser_fail(p, p_peek(p), "expected ';' after (void) expr");
        return NULL;
    }
    return n;
}

/* Type name ;  or  Type name[expr] ;  (no initializer) */
static AstNode* parse_var_decl(Parser* p) {
    int has_struct = 0;
    int ti = p->i;
    int ty0 = p->i;
    Token ty;
    int is_type_slice = 0;
    if (shadow_kw(p_peek(p)) == SHADOW_KW_STRUCT) {
        has_struct = 1;
        if (p->i + 3 >= p->n) return NULL;
        ti = p->i + 1;
        ty = p->toks[ti];
        if (ty.kind != TK_IDENT) return NULL;
        ty0 = p->i;
    } else {
        ty = p_peek(p);
        if (ty.kind != TK_IDENT && shadow_kw(ty) != SHADOW_KW_INT &&
            shadow_kw(ty) != SHADOW_KW_CHAR && shadow_kw(ty) != SHADOW_KW_BOOL &&
            shadow_kw(ty) != SHADOW_KW_SIZE_T)
            return NULL;
        if (p->i + 2 >= p->n) return NULL;
        ty0 = ti;
    }
    int ty_end = has_struct ? -1 : peek_generic_type_end(p, ti);
    if (!has_struct && ty_end < 0) {
        int iend = peek_c_int_type_end(p, ti);
        if (iend > ti) ty_end = iend;
    }
    if (!has_struct) {
        int te = (ty_end > 0) ? ty_end : peek_slice_elem_type_end(p, ti, p->n);
        if (te > ti && te < p->n && tok_eq(p->toks[te], TK_PUNCT, "[")) {
            int uniq = 0;
            int mend = peek_slice_brack_end(p, te, p->n, &uniq);
            if (mend > 0) {
                ty_end = mend;
                is_type_slice = 1;
                (void)uniq;
            }
        }
    }
    int name_i = (ty_end > 0) ? ty_end : (has_struct ? ti + 1 : ti + 1);
    int nstars = 0;
    while (name_i < p->n && tok_eq(p->toks[name_i], TK_PUNCT, "*")) {
        nstars++;
        name_i++;
    }
    if (name_i >= p->n || p->toks[name_i].kind != TK_IDENT) return NULL;
    int si = name_i + 1;
    /* `T x;` / `T *a, *b;` / `size_t n, m;` — multi-declarator beachhead
     * (levenshtein Result fns). Stars stay on each name when comma-separated
     * so emit prints `uint32_t *a, *b;`. */
    if (si < p->n &&
        (tok_eq(p->toks[si], TK_PUNCT, ";") ||
         tok_eq(p->toks[si], TK_PUNCT, ","))) {
        char tytxt[160];
        char names[256];
        size_t nl = 0;
        int multi = tok_eq(p->toks[si], TK_PUNCT, ",");
        int stars_left = nstars;
        Token name;
        if (has_struct) {
            p_next(p); /* struct */
            p_next(p); /* Tag */
            snprintf(tytxt, sizeof(tytxt), "struct %.*s", (int)ty.spell.len,
                     ty.spell.ptr);
        } else if (ty_end > 0) {
            if (!ast_spell_type_tokens(p, ty0, ty_end, tytxt, sizeof(tytxt)) &&
                !ast_spell_token_range(p, ty0, ty_end, tytxt, sizeof(tytxt)))
                return NULL;
            while (p->i < ty_end) p_next(p);
            shadow_seed_spelled_type_name(p, tytxt);
        } else {
            p_next(p); /* type */
            slice_to(tytxt, sizeof(tytxt), ty.spell);
        }
        if (!multi) {
            while (stars_left-- > 0) {
                size_t L = strlen(tytxt);
                if (L + 1 < sizeof(tytxt)) {
                    tytxt[L] = '*';
                    tytxt[L + 1] = 0;
                }
                p_next(p); /* * */
            }
            name = p_next(p);
            p_next(p); /* ; */
            {
                AstNode* n = ast_new(p, AST_VAR_DECL);
                if (!n) return NULL;
                snprintf(n->a, sizeof(n->a), "%s", tytxt);
                slice_to(n->b, sizeof(n->b), name.spell);
                return n;
            }
        }
        /* Multi: keep base type; pack `*a, *b` / `n, m` into n->b. */
        names[0] = 0;
        while (stars_left-- > 0) {
            if (nl + 1 >= sizeof(names)) {
                parser_fail(p, p_peek(p), "multi-declarator list too long");
                return NULL;
            }
            names[nl++] = '*';
            names[nl] = 0;
            p_next(p); /* * */
        }
        name = p_next(p);
        {
            size_t pl = name.spell.len;
            if (nl + pl + 1 >= sizeof(names)) {
                parser_fail(p, name, "multi-declarator list too long");
                return NULL;
            }
            memcpy(names + nl, name.spell.ptr, pl);
            nl += pl;
            names[nl] = 0;
        }
        while (p_accept(p, TK_PUNCT, ",")) {
            int st = 0;
            Token n2;
            size_t pl;
            while (tok_eq(p_peek(p), TK_PUNCT, "*")) {
                st++;
                p_next(p);
            }
            n2 = p_next(p);
            if (n2.kind != TK_IDENT) {
                parser_fail(p, n2, "expected declarator name after ','");
                return NULL;
            }
            if (nl + 1 + (size_t)st + n2.spell.len + 1 >= sizeof(names)) {
                parser_fail(p, n2, "multi-declarator list too long");
                return NULL;
            }
            names[nl++] = ',';
            while (st-- > 0) names[nl++] = '*';
            pl = n2.spell.len;
            memcpy(names + nl, n2.spell.ptr, pl);
            nl += pl;
            names[nl] = 0;
        }
        if (!p_accept(p, TK_PUNCT, ";")) {
            parser_fail(p, p_peek(p), "expected ';' after declaration");
            return NULL;
        }
        {
            AstNode* n = ast_new(p, AST_VAR_DECL);
            if (!n) return NULL;
            snprintf(n->a, sizeof(n->a), "%s", tytxt);
            snprintf(n->b, sizeof(n->b), "%s", names);
            return n;
        }
    }
    /* Type name [ expr ] ;  — or declarator `T name[:…];` */
    if (!has_struct && !is_type_slice && tok_eq(p->toks[si], TK_PUNCT, "[")) {
        int uniq = 0;
        int mend = peek_slice_brack_end(p, si, p->n, &uniq);
        int j = si + 1;
        int depth = 1;
        if (mend > 0 && mend < p->n && tok_eq(p->toks[mend], TK_PUNCT, ";")) {
            char tytxt[160];
            char elem[64];
            Token name;
            int e1 = (ty_end > 0) ? ty_end : peek_slice_elem_type_end(p, ti, name_i);
            if (e1 < 0) e1 = ti + 1;
            if (shadow_kw(ty) == SHADOW_KW_CHAR && e1 == ti + 1)
                snprintf(tytxt, sizeof(tytxt), "%s",
                         uniq ? "CCSliceUnique" : "CCSlice");
            else {
                ast_mangle_slice_elem(p, ti, e1, elem, sizeof(elem));
                snprintf(tytxt, sizeof(tytxt), "CCSlice_%s", elem);
            }
            while (p->i < name_i) p_next(p);
            name = p_next(p);
            while (p->i < mend) p_next(p);
            p_next(p); /* ; */
            {
                AstNode* n = ast_new(p, AST_VAR_DECL);
                if (!n) return NULL;
                snprintf(n->a, sizeof(n->a), "%s", tytxt);
                slice_to(n->b, sizeof(n->b), name.spell);
                if (uniq) snprintf(n->e, sizeof(n->e), "!");
                return n;
            }
        }
        while (j < p->n && depth > 0) {
            if (tok_eq(p->toks[j], TK_PUNCT, "[")) depth++;
            else if (tok_eq(p->toks[j], TK_PUNCT, "]")) depth--;
            j++;
        }
        /* `T a[N];` or multi `T a[N], b[M];`. */
        if (j >= p->n ||
            (!tok_eq(p->toks[j], TK_PUNCT, ";") &&
             !tok_eq(p->toks[j], TK_PUNCT, ",")))
            return NULL;
        if (ty_end > 0) return NULL; /* generic + C array dims unsupported here */
        {
            char tytxt[160];
            char dims[128];
            char names[256];
            Token name;
            int stars_left = nstars;
            int end_i;
            int multi = tok_eq(p->toks[j], TK_PUNCT, ",");
            p_next(p); /* type */
            slice_to(tytxt, sizeof(tytxt), ty.spell);
            while (stars_left-- > 0) {
                size_t L = strlen(tytxt);
                if (L + 1 < sizeof(tytxt)) {
                    tytxt[L] = '*';
                    tytxt[L + 1] = 0;
                }
                p_next(p); /* * */
            }
            if (!multi) {
                /* Single: name in b, dims in c (type_of constexpr fold). */
                name = p_next(p);
                if (!span_text(p, si, j, dims, sizeof(dims))) return NULL;
                while (p->i < j) p_next(p);
                p_next(p); /* ; */
                {
                    AstNode* n = ast_new(p, AST_VAR_DECL);
                    if (!n) return NULL;
                    snprintf(n->a, sizeof(n->a), "%s", tytxt);
                    slice_to(n->b, sizeof(n->b), name.spell);
                    snprintf(n->c, sizeof(n->c), "%s", dims);
                    return n;
                }
            }
            /* Multi: pack `key[32], val[32]` into b (c empty). */
            end_i = j;
            while (end_i < p->n && !tok_eq(p->toks[end_i], TK_PUNCT, ";")) {
                if (tok_eq(p->toks[end_i], TK_PUNCT, ",")) {
                    end_i++;
                    continue;
                }
                if (p->toks[end_i].kind != TK_IDENT) return NULL;
                end_i++;
                if (end_i >= p->n || !tok_eq(p->toks[end_i], TK_PUNCT, "["))
                    return NULL;
                depth = 1;
                end_i++;
                while (end_i < p->n && depth > 0) {
                    if (tok_eq(p->toks[end_i], TK_PUNCT, "[")) depth++;
                    else if (tok_eq(p->toks[end_i], TK_PUNCT, "]")) depth--;
                    end_i++;
                }
            }
            if (end_i >= p->n || !tok_eq(p->toks[end_i], TK_PUNCT, ";"))
                return NULL;
            if (!span_text(p, name_i, end_i, names, sizeof(names))) return NULL;
            while (p->i < end_i) p_next(p);
            p_next(p); /* ; */
            {
                AstNode* n = ast_new(p, AST_VAR_DECL);
                if (!n) return NULL;
                snprintf(n->a, sizeof(n->a), "%s", tytxt);
                snprintf(n->b, sizeof(n->b), "%s", names);
                return n;
            }
        }
    }
    return NULL;
}

/* while ( cond ) { stmts } | while ( cond ) stmt */
static AstNode* parse_while(Parser* p) {
    if (shadow_kw(p_peek(p)) != SHADOW_KW_WHILE) return NULL;
    p_next(p);
    if (!tok_eq(p_peek(p), TK_PUNCT, "(")) {
        parser_fail(p, p_peek(p), "expected '(' after while");
        return NULL;
    }
    int c0 = p->i + 1;
    if (!skip_parens(p)) {
        parser_fail(p, p_peek(p), "unterminated while (...)");
        return NULL;
    }
    int c1 = p->i - 1;
    AstNode* n = ast_new(p, AST_WHILE);
    if (!n) return NULL;
    if (c0 >= c1) n->a[0] = 0;
    else {
        AstNode* ue = parse_ufcs_expr_range(p, c0, c1);
        if (!ast_spell_token_range(p, c0, c1, n->a, sizeof(n->a)) &&
            !span_text(p, c0, c1, n->a, sizeof(n->a))) {
            parser_fail(p, p_peek(p), "while condition too long");
            return NULL;
        }
        if (ue) (void)ast_attach_ufcs_kid(p, n, ue);
    }
    if (tok_eq(p_peek(p), TK_PUNCT, "{")) {
        p_next(p);
        while (!tok_eq(p_peek(p), TK_PUNCT, "}") && p_peek(p).kind != TK_EOF &&
               !p->err) {
            AstNode* s = parse_stmt(p);
            if (!s) return NULL;
            if (n->nbody >= SHADOW_BODY_CAP) {
                parser_fail_body_cap(p, p_peek(p), "while body");
                return NULL;
            }
            n->body[n->nbody++] = s;
        }
        if (!p_accept(p, TK_PUNCT, "}")) {
            parser_fail(p, p_peek(p), "expected '}' to close while");
            return NULL;
        }
        return n;
    }
    {
        AstNode* s = parse_stmt(p);
        if (!s) return NULL;
        n->body[0] = s;
        n->nbody = 1;
        return n;
    }
}

/* static char name [] = "lit" ; */
static AstNode* parse_static_arr(Parser* p) {
    if (shadow_kw(p_peek(p)) != SHADOW_KW_STATIC) return NULL;
    if (p->i + 6 >= p->n) return NULL;
    if (shadow_kw(p->toks[p->i + 1]) != SHADOW_KW_CHAR) return NULL;
    if (p->toks[p->i + 2].kind != TK_IDENT) return NULL;
    if (!tok_eq(p->toks[p->i + 3], TK_PUNCT, "[") ||
        !tok_eq(p->toks[p->i + 4], TK_PUNCT, "]") ||
        !tok_eq(p->toks[p->i + 5], TK_PUNCT, "="))
        return NULL;
    p_next(p); /* static */
    p_next(p); /* char */
    Token name = p_next(p);
    p_next(p); p_next(p); p_next(p); /* [ ] = */
    Token lit = p_next(p);
    if (lit.kind != TK_STR) {
        parser_fail(p, lit, "expected string initializer");
        return NULL;
    }
    if (!p_accept(p, TK_PUNCT, ";")) { p->err = 1; return NULL; }
    AstNode* n = ast_new(p, AST_STATIC_ARR);
    if (!n) return NULL;
    slice_to(n->a, sizeof(n->a), name.spell);
    slice_to(n->b, sizeof(n->b), lit.spell);
    return n;
}

/* Grammar-engine type prefix? Used so user helpers like RedisReply_write are
 * structured-parsed (variant `.nil = {}`) while RespGBulk_write stays opaque
 * when the grammar engine emitted goto-heavy bodies. */
static int shadow_is_grammar_type_prefix(const char* name, size_t n) {
    extern int cc_grammar_pending_ufcs_type_count(void);
    extern const char* cc_grammar_pending_ufcs_type(int i);
    extern int cc_variant_schema_pending_count(void);
    extern const char* cc_variant_schema_pending_name(int i);
    int i, nt;
    if (!name || !n) return 0;
    nt = cc_grammar_pending_ufcs_type_count();
    for (i = 0; i < nt; i++) {
        const char* t = cc_grammar_pending_ufcs_type(i);
        if (t && strlen(t) == n && memcmp(t, name, n) == 0) return 1;
    }
    nt = cc_variant_schema_pending_count();
    for (i = 0; i < nt; i++) {
        const char* t = cc_variant_schema_pending_name(i);
        if (t && strlen(t) == n && memcmp(t, name, n) == 0) return 1;
    }
    return 0;
}

/* Grammar-engine statics: opaque tape (goto/labels / large bodies). */
static int shadow_is_grammar_static_helper(CCSlice name) {
    char buf[128];
    size_t n;
    size_t prefix;
    if (!name.ptr || name.len == 0) return 0;
    n = name.len < sizeof(buf) - 1 ? name.len : sizeof(buf) - 1;
    memcpy(buf, name.ptr, n);
    buf[n] = 0;
    /* Internal matchers / fills: Name__r_*, Name__m_*, Name__b_*,
     * Name__fill, Name__s__m_* / __s__b_* (skip-tier).  Any `__` was too
     * broad — user helpers like `lev__prep` became opaque tape and lost
     * UFCS.  Leading `__` (libc/CC) stays excluded. */
    {
        const char* dd = strstr(buf, "__");
        if (buf[0] != '_' && dd) {
            const char* after = dd + 2;
            if ((after[0] == 'r' || after[0] == 'm' || after[0] == 'b') &&
                after[1] == '_')
                return 1;
            if (strcmp(after, "fill") == 0 || strncmp(after, "fill_", 5) == 0)
                return 1;
            if (strncmp(after, "s__m_", 5) == 0 ||
                strncmp(after, "s__b_", 5) == 0 ||
                strncmp(after, "s__r_", 5) == 0)
                return 1;
        }
    }
    /* Public entry wrappers: Type_match / Type_parse / … — Type must be a
     * pending grammar/schema name. CapCase alone is too broad (RedisReply_write
     * carries Concurrent-C designators and must be structured). */
    if (!(buf[0] >= 'A' && buf[0] <= 'Z')) return 0;
    prefix = 0;
    if (n >= 6 && strcmp(buf + n - 6, "_match") == 0) prefix = n - 6;
    else if (n >= 6 && strcmp(buf + n - 6, "_parse") == 0) prefix = n - 6;
    else if (n >= 8 && strcmp(buf + n - 8, "_prepare") == 0) prefix = n - 8;
    else if (n >= 11 && strcmp(buf + n - 11, "_parse_args") == 0) prefix = n - 11;
    else if (n >= 9 && strcmp(buf + n - 9, "_try_read") == 0) prefix = n - 9;
    else if (n >= 7 && strcmp(buf + n - 7, "_reader") == 0) prefix = n - 7;
    else if (n >= 5 && strcmp(buf + n - 5, "_read") == 0) prefix = n - 5;
    else if (n >= 6 && strcmp(buf + n - 6, "_write") == 0) prefix = n - 6;
    else if (n >= 8 && strcmp(buf + n - 8, "_measure") == 0) prefix = n - 8;
    else if (n >= 7 && strcmp(buf + n - 7, "_to_str") == 0) prefix = n - 7;
    if (!prefix) return 0;
    return shadow_is_grammar_type_prefix(buf, prefix);
}

/* static [inline] [const] Ret[*] name ( params ) { body } — stmts or raw (d). */
static AstNode* parse_static_fn(Parser* p) {
    if (shadow_kw(p_peek(p)) != SHADOW_KW_STATIC) return NULL;
    if (p->i + 4 >= p->n) return NULL;
    int ri = p->i + 1;
    int has_inline = 0;
    int has_const = 0;
    if (ri < p->n && shadow_kw(p->toks[ri]) == SHADOW_KW_INLINE) {
        has_inline = 1;
        ri++;
    }
    if (ri < p->n && shadow_kw(p->toks[ri]) == SHADOW_KW_CONST) {
        has_const = 1;
        ri++;
    }
    if (ri >= p->n) return NULL;
    Token rty = p->toks[ri];
    if (rty.kind != TK_IDENT && shadow_kw(rty) != SHADOW_KW_INT &&
        shadow_kw(rty) != SHADOW_KW_CHAR && shadow_kw(rty) != SHADOW_KW_VOID &&
        shadow_kw(rty) != SHADOW_KW_BOOL && shadow_kw(rty) != SHADOW_KW_SIZE_T)
        return NULL;
    int ni = ri + 1;
    int is_long_long = 0;
    int is_slice = 0;
    int is_slice_unique = 0;
    if (rty.kind == TK_IDENT && spell_eq(rty.spell, "long") && ni < p->n &&
        p->toks[ni].kind == TK_IDENT && spell_eq(p->toks[ni].spell, "long")) {
        is_long_long = 1;
        ni++;
    }
    /* `Ret[:]` / `Ret[:!]` before the name (type-position slice sugar). */
    if (ni + 2 < p->n && tok_eq(p->toks[ni], TK_PUNCT, "[") &&
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
    int is_ptr = 0;
    if (ni < p->n && tok_eq(p->toks[ni], TK_PUNCT, "*")) {
        is_ptr = 1;
        ni++;
    }
    if (ni >= p->n || p->toks[ni].kind != TK_IDENT) return NULL;
    if (ni + 1 >= p->n || !tok_eq(p->toks[ni + 1], TK_PUNCT, "(")) return NULL;

    int fn_start = p->i;
    p_next(p); /* static */
    if (has_inline) p_next(p); /* inline */
    if (has_const) p_next(p); /* const */
    p_next(p); /* ret */
    if (is_long_long) p_next(p); /* long (second) */
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
    char params[512];
    if (p0 >= p->i) params[0] = 0;
    else if (!ast_spell_token_range(p, p0, p->i, params, sizeof(params))) {
        parser_fail(p, name, "static fn params too long");
        return NULL;
    }
    if (!p_accept(p, TK_PUNCT, ")")) {
        parser_fail(p, p_peek(p), "expected ')' after static fn params");
        return NULL;
    }
    /* Forward decl: `static T name(params);` — no body. */
    if (tok_eq(p_peek(p), TK_PUNCT, ";")) {
        AstNode* n = ast_new(p, AST_RAW_LINE);
        char line[640];
        size_t o = 0;
        p_next(p); /* ; */
        if (!n) return NULL;
        o += (size_t)snprintf(line + o, sizeof(line) - o, "static ");
        if (has_inline)
            o += (size_t)snprintf(line + o, sizeof(line) - o, "inline ");
        if (has_const)
            o += (size_t)snprintf(line + o, sizeof(line) - o, "const ");
        if (is_long_long)
            o += (size_t)snprintf(line + o, sizeof(line) - o, "long long");
        else
            o += (size_t)snprintf(line + o, sizeof(line) - o, "%.*s",
                                  (int)rty.spell.len, rty.spell.ptr);
        if (is_ptr)
            o += (size_t)snprintf(line + o, sizeof(line) - o, "*");
        o += (size_t)snprintf(line + o, sizeof(line) - o, " %.*s(%s);",
                              (int)name.spell.len, name.spell.ptr, params);
        (void)o;
        snprintf(n->a, sizeof(n->a), "%s", line);
        {
            char fname[64];
            slice_to(fname, sizeof(fname), name.spell);
            if (p->pending_fn_attrs)
                shadow_fn_attr_register(fname, p->pending_fn_attrs, 0);
        }
        return n;
    }
    if (!p_accept(p, TK_PUNCT, "{")) {
        parser_fail(p, p_peek(p), "expected ') {' after static fn params");
        return NULL;
    }
    /* Grammar matchers / fills / entry wrappers: whole fn as tape RAW_LINE
     * (caller). Avoid half-parsed STATIC_FN with dropped goto/label bodies. */
    if (shadow_is_grammar_static_helper(name.spell)) {
        p->i = fn_start;
        return NULL;
    }
    {
        int saved_nn = p->nn;
        int saved_nk = p->nkstore;
        int saved_soft = p->soft_stmt;
        AstNode* n = ast_new(p, AST_STATIC_FN);
        if (!n) return NULL;
        if (is_slice) {
            /* Keep sugar in a[]; emit rewrites via shadow_rewrite_slice_types. */
            if (is_slice_unique) {
                if (has_const)
                    snprintf(n->a, sizeof(n->a), "const %.*s[:!]%s",
                             (int)rty.spell.len, rty.spell.ptr,
                             is_ptr ? "*" : "");
                else
                    snprintf(n->a, sizeof(n->a), "%.*s[:!]%s",
                             (int)rty.spell.len, rty.spell.ptr,
                             is_ptr ? "*" : "");
            } else if (has_const) {
                snprintf(n->a, sizeof(n->a), "const %.*s[:]%s",
                         (int)rty.spell.len, rty.spell.ptr, is_ptr ? "*" : "");
            } else {
                snprintf(n->a, sizeof(n->a), "%.*s[:]%s", (int)rty.spell.len,
                         rty.spell.ptr, is_ptr ? "*" : "");
            }
        } else if (is_long_long) {
            if (has_const)
                snprintf(n->a, sizeof(n->a), "const long long%s", is_ptr ? "*" : "");
            else
                snprintf(n->a, sizeof(n->a), "long long%s", is_ptr ? "*" : "");
        } else if (has_const) {
            snprintf(n->a, sizeof(n->a), "const %.*s%s", (int)rty.spell.len, rty.spell.ptr,
                     is_ptr ? "*" : "");
        } else {
            slice_to(n->a, sizeof(n->a), rty.spell);
            if (is_ptr) {
                size_t al = strlen(n->a);
                if (al + 1 < sizeof(n->a)) { n->a[al] = '*'; n->a[al + 1] = 0; }
            }
        }
        slice_to(n->b, sizeof(n->b), name.spell);
        snprintf(n->c, sizeof(n->c), "%s", params);
        if (has_inline) snprintf(n->e, sizeof(n->e), "inline");
        if (p->pending_fn_attrs) {
            shadow_fn_attr_register(n->b, p->pending_fn_attrs, 1);
            p->pending_fn_attrs = 0;
        }

        /* Soft stmt parse; on any miss, whole-fn tape (no half body / no redef). */
        int saved_err = p->err;
        int body0 = p->i;
        n->kids = &p->kids_storage[p->nkstore];
        int ok_stmts = 1;
        p->soft_stmt = 1;
        while (!tok_eq(p_peek(p), TK_PUNCT, "}") && p_peek(p).kind != TK_EOF && !p->err) {
            AstNode* s = parse_static_arr(p);
            if (!s && !p->err) s = parse_stmt(p);
            if (!s) { ok_stmts = 0; break; }
            if (!ast_kids_push(p, s)) { ok_stmts = 0; break; }
            n->nkids++;
        }
        p->soft_stmt = saved_soft;
        if (ok_stmts && p_accept(p, TK_PUNCT, "}")) return n;

        /* Soft miss with Concurrent-C still in the body must not become raw
         * tape — that leaves !>/@/=> unlowered. Plain-C bodies may rewind. */
        if (!ok_stmts) {
            int j = body0;
            int d = 1;
            Token at = name;
            while (j < p->n && d > 0) {
                Token t = p->toks[j];
                if (tok_eq(t, TK_PUNCT, "{")) d++;
                else if (tok_eq(t, TK_PUNCT, "}")) d--;
                else if (tok_eq(t, TK_PUNCT, "!>") || tok_eq(t, TK_PUNCT, "?>") ||
                         tok_eq(t, TK_PUNCT, "=>") || tok_eq(t, TK_PUNCT, "@") ||
                         tok_eq(t, TK_PUNCT, "::")) {
                    at = t;
                    p->nn = saved_nn;
                    p->nkstore = saved_nk;
                    p->err = saved_err;
                    parser_fail(p, at,
                                "static fn soft-parse miss with Concurrent-C "
                                "tokens still in body (refusing whole-fn raw "
                                "tape)");
                    return NULL;
                }
                j++;
            }
        }

        /* Stmt subset miss — rewind; caller keeps one AST_RAW_LINE tape span.
         * Grammar primitive: `fallible` / PpStmt soft_unit (pp_stmt.rules). */
        p->i = fn_start;
        p->err = saved_err;
        p->nn = saved_nn;
        p->nkstore = saved_nk;
        return NULL;
    }
}

/* Bare <expr> ; — last-resort statement (e.g. `value * 3;` in send_task). */
static AstNode* parse_expr_stmt(Parser* p) {
    Token t = p_peek(p);
    int e0;
    int j;
    int depth = 0;
    AstNode* n;
    AstNode* ue;
    if (t.kind != TK_IDENT && t.kind != TK_NUM &&
        !tok_eq(t, TK_PUNCT, "(") && !tok_eq(t, TK_PUNCT, "*") &&
        !tok_eq(t, TK_PUNCT, "&") && !tok_eq(t, TK_PUNCT, "-") &&
        !tok_eq(t, TK_PUNCT, "+") && !tok_eq(t, TK_PUNCT, "!") &&
        !tok_eq(t, TK_PUNCT, "~"))
        return NULL;
    /* Statement keywords are handled above; don't reclaim them here. */
    if (shadow_kw(t) != SHADOW_KW_NONE) return NULL;
    e0 = p->i;
    j = p->i;
    while (j < p->n) {
        Token x = p->toks[j];
        if (tok_eq(x, TK_PUNCT, "(") || tok_eq(x, TK_PUNCT, "[") ||
            tok_eq(x, TK_PUNCT, "{"))
            depth++;
        else if (tok_eq(x, TK_PUNCT, ")") || tok_eq(x, TK_PUNCT, "]") ||
                 tok_eq(x, TK_PUNCT, "}")) {
            if (depth == 0) return NULL;
            depth--;
        } else if (depth == 0 && tok_eq(x, TK_PUNCT, ";"))
            break;
        j++;
    }
    if (j <= e0 || j >= p->n || !tok_eq(p->toks[j], TK_PUNCT, ";")) return NULL;
    n = ast_new(p, AST_EXPR_STMT);
    if (!n) return NULL;
    if (!ast_spell_token_range(p, e0, j, n->a, sizeof(n->a)) &&
        !span_text(p, e0, j, n->a, sizeof(n->a))) {
        parser_fail(p, p_peek(p), "expression statement too long");
        return NULL;
    }
    ue = parse_ufcs_expr_range(p, e0, j);
    if (ue) (void)ast_attach_ufcs_kid(p, n, ue);
    p->i = j;
    p_next(p); /* ; */
    return n;
}

/* Ident ( NUM ) ; */
static AstNode* parse_call_num(Parser* p) {
    Token callee = p_peek(p);
    if (callee.kind != TK_IDENT) return NULL;
    if (p->i + 4 >= p->n) return NULL;
    if (!tok_eq(p->toks[p->i + 1], TK_PUNCT, "(")) return NULL;
    if (p->toks[p->i + 2].kind != TK_NUM) return NULL;
    if (!tok_eq(p->toks[p->i + 3], TK_PUNCT, ")")) return NULL;
    if (!tok_eq(p->toks[p->i + 4], TK_PUNCT, ";")) return NULL;
    p_next(p);
    p_next(p); /* ( */
    Token num = p_next(p);
    p_next(p); /* ) */
    p_next(p); /* ; */
    AstNode* n = ast_new(p, AST_CALL_NUM);
    if (!n) return NULL;
    slice_to(n->a, sizeof(n->a), callee.spell);
    slice_to(n->b, sizeof(n->b), num.spell);
    return n;
}

static AstNode* parse_call_args(Parser* p) {
    Token callee = p_peek(p);
    if (callee.kind != TK_IDENT) return NULL;
    if (p->i + 2 >= p->n || !tok_eq(p->toks[p->i + 1], TK_PUNCT, "(")) return NULL;
    /* Prefer the tight NUM form when it matches (usleep beachhead). */
    if (p->i + 4 < p->n && p->toks[p->i + 2].kind == TK_NUM &&
        tok_eq(p->toks[p->i + 3], TK_PUNCT, ")") &&
        tok_eq(p->toks[p->i + 4], TK_PUNCT, ";"))
        return NULL;
    /* Lookahead: balanced parens then ';' or '!> ;' */
    int j = p->i + 1;
    int depth = 0;
    while (j < p->n) {
        Token t = p->toks[j];
        if (tok_eq(t, TK_PUNCT, "(")) depth++;
        else if (tok_eq(t, TK_PUNCT, ")")) {
            depth--;
            if (depth == 0) { j++; break; }
        }
        j++;
    }
    int is_bang = 0;
    if (j < p->n && tok_eq(p->toks[j], TK_PUNCT, "!>")) {
        is_bang = 1;
        j++;
    }
    if (j >= p->n || !tok_eq(p->toks[j], TK_PUNCT, ";")) return NULL;

    p_next(p);
    int a0 = p->i + 1;
    if (!skip_parens(p)) {
        parser_fail(p, p_peek(p), "unterminated call");
        return NULL;
    }
    int a1 = p->i - 1;
    if (is_bang) p_next(p); /* !> */
    p_next(p); /* ; */
    int after_semi = p->i; /* restore after closure reparse */
    AstNode* n = ast_new(p, AST_CALL_ARGS);
    if (!n) return NULL;
    slice_to(n->a, sizeof(n->a), callee.spell);
    if (a0 >= a1) {
        n->b[0] = 0;
    } else {
        AstNode* cl = NULL;
        char args[256];
        int had_cl = parse_call_arg_closure(p, a0, a1, args, sizeof(args),
                                            "callarg", &cl);
        if (p->err) return NULL;
        p->i = after_semi; /* closure scan rewinds; resume past ';' */
        if (had_cl) {
            size_t L = strlen(args);
            /* Helper closes a call expr with ')'; CALL_ARGS stores inner args. */
            if (L && args[L - 1] == ')') args[--L] = 0;
            while (L && (args[L - 1] == ' ' || args[L - 1] == '\t' ||
                         args[L - 1] == ','))
                args[--L] = 0;
            snprintf(n->b, sizeof(n->b), "%s", args);
            if (cl) {
                if (n->ndbody >= SHADOW_BODY_CAP) {
                    parser_fail(p, callee, "too many call-arg closure attachments");
                    return NULL;
                }
                /* Tag send_task closures so emit/safety pack Result payloads. */
                if (spell_eq(callee.spell, "cc_channel_send_task_hybrid"))
                    snprintf(cl->b, sizeof(cl->b), "send_task_hybrid");
                else if (spell_eq(callee.spell, "cc_channel_send_task"))
                    snprintf(cl->b, sizeof(cl->b), "send_task");
                spawn_infer_value_caps(cl);
                n->dbody[n->ndbody++] = cl;
            }
        } else {
            int need_spell = 0;
            int ti;
            for (ti = a0; ti < a1; ti++) {
                if (tok_eq(p->toks[ti], TK_PUNCT, "@")) {
                    need_spell = 1;
                    break;
                }
                if (p->toks[ti].kind == TK_IDENT && ti + 1 < a1 &&
                    tok_eq(p->toks[ti + 1], TK_PUNCT, ".")) {
                    need_spell = 1;
                    break;
                }
            }
            if (!need_spell &&
                span_text(p, a0, a1, n->b, sizeof(n->b))) {
                /* Plain args — preserve source bytes (string gaps, etc.). */
            } else if (!ast_spell_token_range(p, a0, a1, n->b, sizeof(n->b)) &&
                       !span_text(p, a0, a1, n->b, sizeof(n->b))) {
                parser_fail(p, callee, "call args too long");
                return NULL;
            }
            AstNode* ue = parse_ufcs_expr_range(p, a0, a1);
            if (ue) (void)ast_attach_ufcs_kid(p, n, ue);
        }
    }
    if (is_bang) snprintf(n->d, sizeof(n->d), "bang");
    return n;
}

#include "pp_ast_parse_unwrap.h"
#include "pp_ast_parse_spawn.h"

static AstNode* parse_stmt(Parser* p);
#include "pp_ast_parse_err.h"

/* Defined in pp_ast_parse_ext.cch (included after this file). */
static AstNode* parse_typedef_int(Parser* p);

/* Bare `enum { … };` / `enum Tag { … };` in stmt position — tape passthrough
 * (same shape as file-scope enum in pp_ast_parse_ext.cch). */
static AstNode* parse_enum_raw_stmt(Parser* p) {
    Token t = p_peek(p);
    int start;
    int depth = 0;
    int saw_brace = 0;
    size_t off0, off1;
    AstNode* n;
    Token t0;
    if (shadow_kw(t) != SHADOW_KW_ENUM) return NULL;
    start = p->i;
    t0 = p->toks[start];
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
    return n;
}

static AstNode* parse_stmt_inner(Parser* p) {
    Token t = p_peek(p);
    if (t.kind == TK_EOF) return NULL;
    AstNode* n;
    /* cpp-transparent `#if`/`#elif`/`#else`/`#endif` (stage-2 root tape).
     * File-scope parse_external_inner handles these; stmt bodies must too
     * or `#endif` is stolen as an expr/assign spanning to the next `;`. */
    if (t.kind == TK_IDENT && t.spell.len > 0 && t.spell.ptr[0] == '#') {
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
    if (shadow_kw(t) == SHADOW_KW_TYPEDEF) {
        n = parse_typedef_int(p);
        if (n || p->err) return n;
    }
    if (shadow_kw(t) == SHADOW_KW_ENUM) {
        n = parse_enum_raw_stmt(p);
        if (n || p->err) return n;
    }
    if (tok_eq(t, TK_PUNCT, "@")) {
        if (p->i + 2 < p->n && p->toks[p->i + 1].kind == TK_IDENT &&
            tok_eq(p->toks[p->i + 2], TK_PUNCT, "{")) {
            char ident[64];
            unsigned bits;
            slice_to(ident, sizeof(ident), p->toks[p->i + 1].spell);
            bits = shadow_attr_bits_from_ident(ident);
            if (bits && bits != SHADOW_FN_ASYNC) {
                p_next(p);
                p_next(p);
                n = parse_block(p);
                if (n) {
                    if (bits & SHADOW_FN_NOBLOCK)
                        snprintf(n->e, sizeof(n->e), "noblock");
                    else if (bits & SHADOW_FN_BLOCKING)
                        snprintf(n->e, sizeof(n->e), "blocking");
                }
                return n;
            }
        }
        unsigned site = 0;
        if (shadow_parser_peek_call_site_attr(p, &site)) {
            p_next(p);
            p_next(p);
            n = parse_call_args(p);
            if (n || p->err) {
                if (n && site & SHADOW_FN_NOBLOCK)
                    snprintf(n->e, sizeof(n->e), "noblock");
                else if (n && site & SHADOW_FN_BLOCKING)
                    snprintf(n->e, sizeof(n->e), "blocking");
                return n;
            }
        }
        n = parse_errhandler(p);
        if (n || p->err) return n;
        n = parse_err_delegate(p);
        if (n || p->err) return n;
        n = parse_err_fwd(p);
        if (n || p->err) return n;
        n = parse_defer(p);
        if (n || p->err) return n;
        n = parse_with_deadline(p);
        if (n || p->err) return n;
        /* `@string(...).println() !>;` before bare `@string` AT_STMT. */
        n = parse_ufcs_stmt(p);
        if (n || p->err) return n;
        return parse_at_stmt(p);
    }
    if (tok_eq(t, TK_PUNCT, "{")) return parse_block(p);
    if (tok_eq(t, TK_PUNCT, "(")) {
        n = parse_void_cast(p);
        if (n || p->err) return n;
    }
    if (shadow_kw(t) == SHADOW_KW_IF) return parse_if_stmt(p);
    if (shadow_kw(t) == SHADOW_KW_SWITCH) return parse_switch_stmt(p);
    if (shadow_kw(t) == SHADOW_KW_FOR) return parse_for(p);
    if (t.kind == TK_IDENT && spell_eq(t.spell, "CC_ARRAY_MAP_FOREACH")) {
        n = parse_array_map_foreach(p);
        if (n || p->err) return n;
    }
    if (shadow_kw(t) == SHADOW_KW_WHILE) return parse_while(p);
    if (shadow_kw(t) == SHADOW_KW_DO) return parse_do_while(p);
    if (shadow_kw(t) == SHADOW_KW_RETURN) return parse_return_int(p);
    if (shadow_kw(t) == SHADOW_KW_PRINTLN || shadow_kw(t) == SHADOW_KW_EPRINTLN)
        return parse_println_bang(p);
    if (shadow_kw(t) == SHADOW_KW_BREAK || shadow_kw(t) == SHADOW_KW_CONTINUE)
        return parse_break_continue(p);
    if (t.kind == TK_IDENT && spell_eq(t.spell, "goto")) {
        n = parse_goto(p);
        if (n || p->err) return n;
    }
    if (t.kind == TK_IDENT) {
        n = parse_label(p);
        if (n || p->err) return n;
    }
    if (shadow_kw(t) == SHADOW_KW_CONST) {
        n = parse_typed_init(p);
        if (n || p->err) return n;
    }
    if (shadow_kw(t) == SHADOW_KW_STATIC) {
        n = parse_static_arr(p);
        if (n || p->err) return n;
    }
    if (shadow_kw(t) == SHADOW_KW_INT) {
        n = parse_var_unwrap(p);
        if (n || p->err) return n;
        n = parse_chan_var(p);
        if (n || p->err) return n;
        n = parse_slice_var(p);
        if (n || p->err) return n;
    }
    if (shadow_kw(t) == SHADOW_KW_SIZE_T || shadow_kw(t) == SHADOW_KW_BOOL ||
        shadow_kw(t) == SHADOW_KW_CHAR || shadow_kw(t) == SHADOW_KW_VOID ||
        t.kind == TK_IDENT) {
        n = parse_chan_var(p);
        if (n || p->err) return n;
        n = parse_slice_init(p);
        if (n || p->err) return n;
        n = parse_var_unwrap(p);
        if (n || p->err) return n;
    }
    if (shadow_kw(t) == SHADOW_KW_CONST) {
        n = parse_ptr_unwrap(p);
        if (n || p->err) return n;
        n = parse_var_unwrap(p);
        if (n || p->err) return n;
    }
    if (peek_result_shape(p)) {
        n = parse_result_local(p);
        if (n || p->err) return n;
    }
    n = parse_ptr_unwrap(p);
    if (n || p->err) return n;
    n = parse_nursery_destroy(p);
    if (n || p->err) return n;
    n = parse_val_destroy(p);
    if (n || p->err) return n;
    n = parse_ptr_decl_stmt(p);
    if (n || p->err) return n;
    n = parse_ptr_init(p);
    if (n || p->err) return n;
    n = parse_typed_init(p);
    if (n || p->err) return n;
    n = parse_var_decl(p);
    if (n || p->err) return n;
    n = parse_spawn_closure(p);
    if (n || p->err) return n;
    n = parse_send_task_closure(p);
    if (n || p->err) return n;
    n = parse_ufcs_stmt(p);
    if (n || p->err) return n;
    n = parse_err_syntax_stmt(p);
    if (n || p->err) return n;
    /* Before assign: `call() !> (e) x = …` has `=` but is stmt-unwrap. */
    n = parse_stmt_unwrap(p);
    if (n || p->err) return n;
    n = parse_assign_stmt(p);
    if (n || p->err) return n;
    n = parse_inc(p);
    if (n || p->err) return n;
    n = parse_call_num(p);
    if (n || p->err) return n;
    n = parse_call_args(p);
    if (n || p->err) return n;
    n = parse_expr_stmt(p);
    if (n || p->err) return n;
    /* Empty statement `;` (e.g. `Lok1: ;` in grammar matchers). */
    if (tok_eq(t, TK_PUNCT, ";")) {
        p_next(p);
        n = ast_new(p, AST_EXPR_STMT);
        if (!n) return NULL;
        n->a[0] = 0;
        return n;
    }
    if (p->soft_stmt) return NULL;
    parser_fail(p, t, "unexpected statement (shadow subset)");
    return NULL;
}

static AstNode* parse_stmt(Parser* p) {
    int start = p->i;
    AstNode* n = parse_stmt_inner(p);
    if (n) shadow_attach_lead(p, n, start);
    return n;
}

/* int Name ( params ) { stmts } */
