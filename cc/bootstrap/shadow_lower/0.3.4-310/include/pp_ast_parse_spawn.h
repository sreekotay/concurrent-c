/* Parse: closure / capture infer (nursery spawn and send_task are UFCS).
 * Included from pp_ast_parse_stmt.cch. */
#pragma once

static AstNode* parse_nursery_destroy(Parser* p) {
    Token ty = p_peek(p);
    if (ty.kind != TK_IDENT) return NULL;
    if (p->i + 12 >= p->n) return NULL;
    if (!tok_eq(p->toks[p->i + 1], TK_PUNCT, "*")) return NULL;
    if (p->toks[p->i + 2].kind != TK_IDENT) return NULL;
    if (!tok_eq(p->toks[p->i + 3], TK_PUNCT, "=")) return NULL;
    if (p->toks[p->i + 4].kind != TK_IDENT) return NULL;
    if (!tok_eq(p->toks[p->i + 5], TK_PUNCT, "(")) return NULL;
    /* Find ) !> @ destroy { */
    int j = p->i + 6;
    if (j >= p->n || p->toks[j].kind != TK_IDENT) return NULL; /* NULL / arg */
    j++;
    if (j >= p->n || !tok_eq(p->toks[j], TK_PUNCT, ")")) return NULL;
    j++;
    if (j >= p->n || !tok_eq(p->toks[j], TK_PUNCT, "!>")) return NULL;
    j++;
    if (j >= p->n || !tok_eq(p->toks[j], TK_PUNCT, "@")) return NULL;
    j++;
    if (j >= p->n || shadow_kw(p->toks[j]) != SHADOW_KW_DESTROY) return NULL;
    j++;
    if (j >= p->n || !tok_eq(p->toks[j], TK_PUNCT, "{")) return NULL;

    p_next(p); /* type */
    p_next(p); /* * */
    Token name = p_next(p);
    p_next(p); /* = */
    Token callee = p_next(p);
    p_next(p); /* ( */
    Token arg = p_next(p);
    (void)arg;
    p_next(p); /* ) */
    p_next(p); /* !> */
    p_next(p); /* @ */
    p_next(p); /* destroy */
    p_next(p); /* { */

    AstNode* n = ast_new(p, AST_NURSERY_DESTROY);
    if (!n) return NULL;
    n->a = ast_arena_slice(p, ty.spell);
    n->b = ast_arena_slice(p, name.spell);
    n->c = ast_arena_slice(p, callee.spell);
    while (!tok_eq(p_peek(p), TK_PUNCT, "}") && p_peek(p).kind != TK_EOF && !p->err) {
        AstNode* s = parse_stmt(p);
        if (!s) return NULL;
        if (n->nbody >= SHADOW_BODY_CAP) {
            parser_fail_body_cap(p, p_peek(p), "@destroy body");
            return NULL;
        }
        n->body[n->nbody++] = s;
    }
    if (!p_accept(p, TK_PUNCT, "}")) {
        parser_fail(p, p_peek(p), "expected '}' to close @destroy");
        return NULL;
    }
    if (!p_accept(p, TK_PUNCT, ";")) {
        parser_fail(p, p_peek(p), "expected ';' after @destroy { ... }");
        return NULL;
    }
    return n;
}

/* True for `=> {` / `=> [caps] {` / `=> [caps] expr` / `=> expr`
 * (and `= >` digraph forms). *out_brace = `{` index, or -1 for expr body. */
static int shadow_is_arrow_brace(Parser* p, int i, int end, int* out_brace) {
    int k;
    if (!p || i < 0 || i >= end) return 0;
    k = i;
    if (tok_eq(p->toks[k], TK_PUNCT, "=>")) {
        k++;
    } else if (tok_eq(p->toks[k], TK_PUNCT, "=") && k + 1 < end &&
               tok_eq(p->toks[k + 1], TK_PUNCT, ">")) {
        k += 2;
    } else {
        return 0;
    }
    if (k < end && tok_eq(p->toks[k], TK_PUNCT, "[")) {
        int depth = 0;
        for (; k < end; k++) {
            if (tok_eq(p->toks[k], TK_PUNCT, "["))
                depth++;
            else if (tok_eq(p->toks[k], TK_PUNCT, "]")) {
                depth--;
                if (depth == 0) {
                    k++;
                    break;
                }
            }
        }
    }
    if (k < end && tok_eq(p->toks[k], TK_PUNCT, "{")) {
        if (out_brace) *out_brace = k;
        return 1;
    }
    /* Expression body: anything but `,` / `)` starter (call-arg / send_task). */
    if (k < end && !tok_eq(p->toks[k], TK_PUNCT, ",") &&
        !tok_eq(p->toks[k], TK_PUNCT, ")") &&
        !tok_eq(p->toks[k], TK_PUNCT, ";")) {
        if (out_brace) *out_brace = -1;
        return 1;
    }
    return 0;
}

/* Find `(` of `(params) => {` or bare `x => {` in toks[c0..end).
 * Returns param-`(` index; *out_rp = `)` index, or -2 for bare ident form. */
static int find_arrow_closure(Parser* p, int c0, int end, int* out_rp) {
    int i;
    if (out_rp) *out_rp = -1;
    if (!p || c0 < 0 || end > p->n || c0 + 2 >= end) return -1;
    for (i = c0; i + 3 < end; i++) {
        int j;
        int depth;
        int brace;
        if (!tok_eq(p->toks[i], TK_PUNCT, "(")) continue;
        depth = 0;
        for (j = i; j < end; j++) {
            Token t = p->toks[j];
            if (tok_eq(t, TK_PUNCT, "(") || tok_eq(t, TK_PUNCT, "[") ||
                tok_eq(t, TK_PUNCT, "{"))
                depth++;
            else if (tok_eq(t, TK_PUNCT, ")") || tok_eq(t, TK_PUNCT, "]") ||
                     tok_eq(t, TK_PUNCT, "}")) {
                depth--;
                if (depth == 0) {
                    if (j + 1 < end &&
                        shadow_is_arrow_brace(p, j + 1, end, &brace)) {
                        if (out_rp) *out_rp = j;
                        return i;
                    }
                    break;
                }
            }
        }
    }
    /* Bare `ident => {` (arity-1 untyped). */
    for (i = c0; i + 2 < end; i++) {
        int brace;
        if (p->toks[i].kind == TK_IDENT &&
            shadow_is_arrow_brace(p, i + 1, end, &brace)) {
            if (out_rp) *out_rp = -2; /* bare marker */
            return i;
        }
    }
    return -1;
}

/* Parse beachhead formals into dst; return arity or -1.
 * Typed: `int x` / `void* p` / `int x, int y`. Untyped: `x` → `intptr_t`. */
static int parse_closure_formals_text(Parser* p, int lp, int rp, char* dst,
                                      size_t cap) {
    int i;
    int arity = 0;
    size_t o = 0;
    if (!p || !dst || !cap || lp < 0 || rp <= lp) return -1;
    dst[0] = 0;
    i = lp + 1;
    while (i < rp) {
        Token t0;
        Token t1;
        char tbuf[32];
        char nbuf[32];
        size_t need;
        if (arity) {
            if (!tok_eq(p->toks[i], TK_PUNCT, ",")) return -1;
            i++;
        }
        if (i >= rp) break;
        t0 = p->toks[i];
        if (t0.kind != TK_IDENT) return -1;
        i++;
        if (arity >= 2) return -1;
        /* `void* p` / `T* name` — pointer formal (owned-channel destroy/reset). */
        if (i < rp && tok_eq(p->toks[i], TK_PUNCT, "*") && i + 1 < rp &&
            p->toks[i + 1].kind == TK_IDENT) {
            char base[24];
            slice_to(base, sizeof(base), t0.spell);
            snprintf(tbuf, sizeof(tbuf), "%s*", base);
            i++; /* * */
            t1 = p->toks[i];
            i++;
            slice_to(nbuf, sizeof(nbuf), t1.spell);
        } else if (i < rp && p->toks[i].kind == TK_IDENT) {
            t1 = p->toks[i];
            i++;
            slice_to(tbuf, sizeof(tbuf), t0.spell);
            slice_to(nbuf, sizeof(nbuf), t1.spell);
        } else {
            /* Untyped formal — ABI packs args as intptr_t. */
            snprintf(tbuf, sizeof(tbuf), "intptr_t");
            slice_to(nbuf, sizeof(nbuf), t0.spell);
        }
        need = strlen(tbuf) + 1 + strlen(nbuf) + (arity ? 2 : 0);
        if (o + need + 1 >= cap) return -1;
        if (arity) {
            dst[o++] = ',';
            dst[o++] = ' ';
        }
        o += (size_t)snprintf(dst + o, cap - o, "%s %s", tbuf, nbuf);
        arity++;
    }
    return arity;
}

static void spawn_caps_add(Parser* p, char** caps_slot, const char* item);

/* One `[...]` capture item → "x" / "&x" / "@safe&x" / "p=&x".
 * `@safe` is only legal with `&` (intentional shared binding). */
static int parse_spawn_capture_item(Parser* p, char* item, size_t item_cap,
                                    int allow_init) {
    int is_safe = 0;
    int is_ref = 0;
    Token id;
    char name[64];
    if (!p || !item || !item_cap) return 0;
    item[0] = 0;
    if (tok_eq(p_peek(p), TK_PUNCT, "@") && p->i + 1 < p->n &&
        p->toks[p->i + 1].kind == TK_IDENT &&
        spell_eq(p->toks[p->i + 1].spell, "safe")) {
        is_safe = 1;
        p_next(p); /* @ */
        p_next(p); /* safe */
    }
    if (tok_eq(p_peek(p), TK_PUNCT, "&")) {
        is_ref = 1;
        p_next(p);
    }
    if (is_safe && !is_ref) {
        parser_fail(p, p_peek(p), "@safe requires '&' capture");
        return 0;
    }
    /* Capture-all `[&]` / `[=]` is banned. */
    if (is_ref && tok_eq(p_peek(p), TK_PUNCT, "]")) {
        parser_fail(p, p_peek(p), "capture-all [&] is not allowed");
        return 0;
    }
    if (!is_ref && tok_eq(p_peek(p), TK_PUNCT, "=") &&
        p->i + 1 < p->n && tok_eq(p->toks[p->i + 1], TK_PUNCT, "]")) {
        parser_fail(p, p_peek(p), "capture-all [=] is not allowed");
        return 0;
    }
    id = p_next(p);
    if (id.kind != TK_IDENT) {
        parser_fail(p, id, "expected capture name");
        return 0;
    }
    slice_to(name, sizeof(name), id.spell);
    if (allow_init && !is_ref && tok_eq(p_peek(p), TK_PUNCT, "=")) {
        int e0, e1, depth;
        p_next(p); /* = */
        e0 = p->i;
        depth = 0;
        while (p->i < p->n) {
            Token x = p_peek(p);
            if (tok_eq(x, TK_PUNCT, "(") || tok_eq(x, TK_PUNCT, "[") ||
                tok_eq(x, TK_PUNCT, "{"))
                depth++;
            else if (tok_eq(x, TK_PUNCT, ")") || tok_eq(x, TK_PUNCT, "]") ||
                     tok_eq(x, TK_PUNCT, "}")) {
                if (depth == 0) break;
                depth--;
            } else if (depth == 0 &&
                       (tok_eq(x, TK_PUNCT, ",") || tok_eq(x, TK_PUNCT, "]")))
                break;
            p_next(p);
        }
        e1 = p->i;
        {
            char init[64];
            if (!span_text(p, e0, e1, init, sizeof(init))) {
                parser_fail(p, id, "init-capture expr too long");
                return 0;
            }
            snprintf(item, item_cap, "%s=%s", name, init);
        }
    } else if (is_ref) {
        if (is_safe)
            snprintf(item, item_cap, "@safe&%s", name);
        else
            snprintf(item, item_cap, "&%s", name);
    } else {
        snprintf(item, item_cap, "%s", name);
    }
    return 1;
}

/* Lift `(…) => { stmts }` as call/create arg or bare typed-init RHS in toks[c0..end).
 * Call-arg form writes closed call prefix into dst and consumes trailing `)`.
 * Bare RHS (closure is the whole span) leaves dst empty.
 * Attaches SPAWN_CLOSURE via *out_cl (b=callarg|callarg1|callarg2|tag).
 * On success leaves p->i at end. Returns 1 if a closure was found. */
static int parse_call_arg_closure(Parser* p, int c0, int end, char* dst,
                                  size_t cap, const char* tag,
                                  AstNode** out_cl) {
    int cl = -1;
    int rp = -1;
    int bare;
    int arity;
    int is_unsafe = 0;
    char formals[96];
    if (out_cl) *out_cl = NULL;
    if (!p || !dst || !cap || c0 < 0 || end > p->n || c0 >= end) return 0;
    dst[0] = 0;
    cl = find_arrow_closure(p, c0, end, &rp);
    /* rp==-2 is bare-ident form (valid). */
    if (cl < 0 || (rp < 0 && rp != -2)) return 0;
    {
        /* `@unsafe () =>` — the unsafe mark is on the literal, not a call arg. */
        int cl0 = cl;
        if (rp != -2 && cl >= c0 + 2 &&
            tok_eq(p->toks[cl - 2], TK_PUNCT, "@") &&
            p->toks[cl - 1].kind == TK_IDENT &&
            spell_eq(p->toks[cl - 1].spell, "unsafe")) {
            cl0 = cl - 2;
            is_unsafe = 1;
        }
        bare = (cl0 == c0);
    }
    if (!bare) {
        int pre_end = is_unsafe ? cl - 2 : cl;
        size_t L;
        while (pre_end > c0 && tok_eq(p->toks[pre_end - 1], TK_PUNCT, ","))
            pre_end--;
        if (!ast_spell_token_range(p, c0, pre_end, dst, cap) &&
            !span_text(p, c0, pre_end, dst, cap)) {
            parser_fail(p, p->toks[c0], "call-arg closure prefix too long");
            return 0;
        }
        L = strlen(dst);
        while (L && (dst[L - 1] == ' ' || dst[L - 1] == '\t' || dst[L - 1] == ','))
            dst[--L] = 0;
        /* Leave the call open at '(' — closure splice fills the first arg. */
    }
    if (rp == -2) {
        /* Bare `x => {` */
        char nbuf[32];
        slice_to(nbuf, sizeof(nbuf), p->toks[cl].spell);
        snprintf(formals, sizeof(formals), "intptr_t %s", nbuf);
        arity = 1;
    } else {
        arity = parse_closure_formals_text(p, cl, rp, formals, sizeof(formals));
        if (arity < 0) {
            parser_fail(p, p->toks[cl],
                        "closure params too complex for shadow beachhead");
            return 0;
        }
    }
    p->i = cl;
    if (rp == -2) {
        p_next(p); /* ident */
        if (tok_eq(p_peek(p), TK_PUNCT, "=>"))
            p_next(p);
        else {
            p_next(p); /* = */
            p_next(p); /* > */
        }
    } else {
        p_next(p); /* ( */
        while (p->i < rp) p_next(p);
        p_next(p); /* ) */
        if (tok_eq(p_peek(p), TK_PUNCT, "=>"))
            p_next(p);
        else {
            p_next(p); /* = */
            p_next(p); /* > */
        }
    }
    {
        AstNode* n = ast_new(p, AST_SPAWN_CLOSURE);
        if (!n) return 0;
        if (arity == 0)
            n->b = ast_arena_cstr(p, tag ? tag : "callarg");
        else if (arity == 1)
            n->b = ast_arena_cstr(p, "callarg1");
        else
            n->b = ast_arena_cstr(p, "callarg2");
        if (formals[0])
            n->a = ast_arena_cstr(p, formals);
        else
            n->a = NULL;
        /* Call prefix for send_into formal inference (e.g. tx.try_send_into). */
        if (!bare && dst[0])
            n->c = ast_arena_cstr(p, dst);
        else
            n->c = NULL;
        n->e = NULL;
        if (is_unsafe) n->f = ast_arena_cstr(p, "unsafe");
        /* Optional explicit captures: `=> [rx, @safe &db, &tx] { … }` */
        if (tok_eq(p_peek(p), TK_PUNCT, "[")) {
            p_next(p);
            while (!tok_eq(p_peek(p), TK_PUNCT, "]") &&
                   p_peek(p).kind != TK_EOF && !p->err) {
                char item[80];
                if (!parse_spawn_capture_item(p, item, sizeof(item), 1))
                    return 0;
                spawn_caps_add(p, &n->e, item);
                if (tok_eq(p_peek(p), TK_PUNCT, ",")) p_next(p);
            }
            if (!p_accept(p, TK_PUNCT, "]")) {
                parser_fail(p, p_peek(p), "expected ']' after capture list");
                return 0;
            }
        }
        if (tok_eq(p_peek(p), TK_PUNCT, "{")) {
            p_next(p); /* { */
            while (!tok_eq(p_peek(p), TK_PUNCT, "}") && p_peek(p).kind != TK_EOF &&
                   !p->err) {
                AstNode* s = parse_stmt(p);
                if (!s) return 0;
                if (n->nbody >= SHADOW_BODY_CAP) {
                    parser_fail_body_cap(p, p_peek(p), "() => body");
                    return 0;
                }
                n->body[n->nbody++] = s;
            }
            if (!p_accept(p, TK_PUNCT, "}")) {
                parser_fail(p, p_peek(p), "expected '}' after () => { ... }");
                return 0;
            }
        } else {
            /* Expression body: `() => [caps] expr` up to call's `)`. */
            int e0 = p->i;
            int depth = bare ? 0 : 1;
            while (p->i < end) {
                Token t = p_peek(p);
                if (tok_eq(t, TK_PUNCT, "(") || tok_eq(t, TK_PUNCT, "[") ||
                    tok_eq(t, TK_PUNCT, "{"))
                    depth++;
                else if (tok_eq(t, TK_PUNCT, ")") || tok_eq(t, TK_PUNCT, "]") ||
                         tok_eq(t, TK_PUNCT, "}")) {
                    if (!bare && tok_eq(t, TK_PUNCT, ")") && depth == 1) break;
                    if (depth > 0) depth--;
                }
                p_next(p);
            }
            {
                char expr[256];
                if (!span_text(p, e0, p->i, expr, sizeof(expr))) {
                    parser_fail(p, p_peek(p), "() => expression body too long");
                    return 0;
                }
                /* Prefer CALL_ARGS when `name(args)`; else EXPR_STMT. */
                const char* lp = strchr(expr, '(');
                const char* rp = expr[0] ? strrchr(expr, ')') : NULL;
                if (lp && rp && rp > lp && n->nbody < 8) {
                    AstNode* s = ast_new(p, AST_CALL_ARGS);
                    size_t nl, al;
                    if (!s) return 0;
                    nl = (size_t)(lp - expr);
                    while (nl > 0 &&
                           (expr[nl - 1] == ' ' || expr[nl - 1] == '\t'))
                        nl--;
                    {
                        char nbuf[512], abuf[512];
                        if (nl >= sizeof(nbuf)) nl = sizeof(nbuf) - 1;
                        memcpy(nbuf, expr, nl);
                        nbuf[nl] = 0;
                        s->a = ast_arena_cstr(p, nbuf);
                        al = (size_t)(rp - lp - 1);
                        if (al >= sizeof(abuf)) al = sizeof(abuf) - 1;
                        memcpy(abuf, lp + 1, al);
                        abuf[al] = 0;
                        s->b = ast_arena_cstr(p, abuf);
                    }
                    n->body[n->nbody++] = s;
                } else if (n->nbody < 8) {
                    AstNode* s = ast_new(p, AST_EXPR_STMT);
                    if (!s) return 0;
                    s->a = ast_arena_cstr(p, expr);
                    n->body[n->nbody++] = s;
                } else {
                    parser_fail_body_cap(p, p_peek(p), "() => expression body");
                    return 0;
                }
            }
        }
        if (p->i != end) {
            /* `(a, b) => { … }, NULL` — closure is first of several call args. */
            char trail[256];
            int t0;
            if (tok_eq(p_peek(p), TK_PUNCT, ","))
                p_next(p);
            t0 = p->i;
            if (!span_text(p, t0, end, trail, sizeof(trail))) {
                parser_fail(p, p_peek(p),
                            "call-arg closure trailing args too long");
                return 0;
            }
            {
                size_t tl = strlen(trail);
                while (tl && (trail[tl - 1] == ' ' || trail[tl - 1] == '\t' ||
                              trail[tl - 1] == '\n' || trail[tl - 1] == '\r'))
                    trail[--tl] = 0;
                while (tl && trail[tl - 1] == ')')
                    trail[--tl] = 0;
            }
            if (!bare) {
                size_t L = strlen(dst);
                const char* t;
                while (L && (dst[L - 1] == ' ' || dst[L - 1] == '\t'))
                    L--;
                t = trail;
                while (*t == ' ' || *t == '\t' || *t == '\n' || *t == '\r')
                    t++;
                if (*t && strcmp(t, ")") != 0) {
                    if (L + strlen(trail) + 4 >= cap) {
                        parser_fail(p, p->toks[c0],
                                    "call-arg closure prefix too long");
                        return 0;
                    }
                    /* Empty first-arg slot: `(closure, rest…)`. */
                    snprintf(dst + L, cap - L, ", %s)", trail);
                }
            } else if (trail[0]) {
                const char* t = trail;
                while (*t == ' ' || *t == '\t') t++;
                if (*t && strcmp(t, ")") != 0)
                    snprintf(dst, cap, ", %s", trail);
            }
            p->i = end;
        }
        if (p->i != end) {
            parser_fail(p, p_peek(p), "trailing tokens after () => closure arg");
            return 0;
        }
        if (out_cl) *out_cl = n;
    }
    return 1;
}

/* Type[*] name = expr @destroy [ { body } ];  or  … @detach;
 * d="*"|"", e="_Dbare"|"_D"|"_detach"; UFCS on dbody; body stmts on body[]. */

static int spawn_is_ident_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_';
}

static int spawn_is_ident(const char* s) {
    if (!s || !*s) return 0;
    if (!((s[0] >= 'a' && s[0] <= 'z') || (s[0] >= 'A' && s[0] <= 'Z') ||
          s[0] == '_'))
        return 0;
    for (const char* p = s + 1; *p; p++) {
        if (!spawn_is_ident_char(*p)) return 0;
    }
    return 1;
}

/* Alias name of a caps item: "@safe&x"/"&x"→x, "p=&x"→p, "y"→y. */
static void spawn_cap_alias(const char* item, char* out, size_t cap) {
    const char* eq;
    if (!item || !out || !cap) return;
    out[0] = 0;
    if (strncmp(item, "@safe&", 6) == 0) {
        snprintf(out, cap, "%s", item + 6);
        return;
    }
    if (item[0] == '&') {
        snprintf(out, cap, "%s", item + 1);
        return;
    }
    eq = strchr(item, '=');
    if (eq) {
        size_t n = (size_t)(eq - item);
        if (n >= cap) n = cap - 1;
        memcpy(out, item, n);
        out[n] = 0;
        return;
    }
    snprintf(out, cap, "%s", item);
}

/* Append "name" / "&name" / "alias=init" if that alias is not already present.
 * Rebuilds on parse_ar (unbounded); OOM → parser_fail. */
static void spawn_caps_add(Parser* p, char** caps_slot, const char* item) {
    char alias[64];
    char existing[64];
    char tmp[4096];
    const char* caps;
    if (!caps_slot || !item || !item[0]) return;
    caps = *caps_slot ? *caps_slot : "";
    spawn_cap_alias(item, alias, sizeof(alias));
    if (!alias[0]) return;
    {
        const char* curp = caps;
        size_t il = strlen(item);
        while (*curp) {
            const char* c = strchr(curp, ',');
            size_t n = c ? (size_t)(c - curp) : strlen(curp);
            char cur[80];
            if (n >= sizeof(cur)) n = sizeof(cur) - 1;
            memcpy(cur, curp, n);
            cur[n] = 0;
            if (n == il && memcmp(curp, item, il) == 0) return;
            spawn_cap_alias(cur, existing, sizeof(existing));
            if (existing[0] && strcmp(existing, alias) == 0) return;
            if (!c) break;
            curp = c + 1;
        }
    }
    if (caps[0]) {
        if (snprintf(tmp, sizeof(tmp), "%s,%s", caps, item) >= (int)sizeof(tmp)) {
            if (p)
                parser_fail(p, p_peek(p), "spawn capture list too long");
            else {
                char __diag[256];
                snprintf(__diag, sizeof(__diag),
                         "spawn capture list too long; cannot add '%s'", item);
                diag_err_loc(NULL, 0, 0, __diag);
            }
            return;
        }
    } else {
        snprintf(tmp, sizeof(tmp), "%s", item);
    }
    if (!p) {
        char __diag[256];
        snprintf(__diag, sizeof(__diag),
                 "spawn_caps_add without parser; cannot add '%s'", item);
        diag_err_loc(NULL, 0, 0, __diag);
        return;
    }
    *caps_slot = ast_arena_cstr(p, tmp);
}

static int spawn_stmt_declares(AstNode* s, const char* name) {
    int k;
    if (!s || !name || !name[0]) return 0;
    if ((s->kind == AST_TYPED_INIT || s->kind == AST_PTR_INIT ||
         s->kind == AST_VAR_DECL || s->kind == AST_VAL_DESTROY ||
         s->kind == AST_NURSERY_DESTROY) &&
        strcmp(ast_slot(s->b), name) == 0)
        return 1;
    /* VAR_UNWRAP / PTR_UNWRAP: binder name is a (not b). */
    if ((s->kind == AST_VAR_UNWRAP || s->kind == AST_PTR_UNWRAP) &&
        strcmp(ast_slot(s->a), name) == 0)
        return 1;
    /* RESULT_LOCAL: name lives in a (ok/err types in c/b). */
    if (s->kind == AST_RESULT_LOCAL && strcmp(ast_slot(s->a), name) == 0)
        return 1;
    /* `for (int i = 0; …)` — loop var is body-local. */
    if (s->kind == AST_FOR && s->a && s->a[0]) {
        const char* h = s->a;
        char id[64];
        size_t ni = 0;
        while (*h == ' ' || *h == '\t') h++;
        if ((strncmp(h, "int", 3) == 0 && !spawn_is_ident_char(h[3])) ||
            (strncmp(h, "size_t", 6) == 0 && !spawn_is_ident_char(h[6])) ||
            (strncmp(h, "long", 4) == 0 && !spawn_is_ident_char(h[4])) ||
            (strncmp(h, "bool", 4) == 0 && !spawn_is_ident_char(h[4]))) {
            while (*h && spawn_is_ident_char(*h)) h++;
            while (*h == ' ' || *h == '\t') h++;
            while (*h && spawn_is_ident_char(*h) && ni + 1 < sizeof(id))
                id[ni++] = *h++;
            id[ni] = 0;
            if (id[0] && strcmp(id, name) == 0) return 1;
        }
    }
    if (s->kind == AST_IF || s->kind == AST_WHILE || s->kind == AST_FOR ||
        s->kind == AST_BLOCK || s->kind == AST_DO_WHILE) {
        for (k = 0; k < s->nbody; k++)
            if (spawn_stmt_declares(s->body[k], name)) return 1;
        for (k = 0; k < s->ndbody; k++)
            if (spawn_stmt_declares(s->dbody[k], name)) return 1;
        if (s->kids) {
            for (k = 0; k < s->nkids; k++)
                if (spawn_stmt_declares(s->kids[k], name)) return 1;
        }
    }
    return 0;
}

static int spawn_body_declares(AstNode* n, const char* name) {
    for (int k = 0; k < n->nbody; k++) {
        if (spawn_stmt_declares(n->body[k], name)) return 1;
    }
    return 0;
}

/* Formal names from SPAWN_CLOSURE.a (`int x` / `int x, int y`). */
static int spawn_name_is_formal(AstNode* n, const char* name) {
    const char* p;
    if (!n || !name || !name[0] || !(n->a && n->a[0])) return 0;
    p = n->a;
    while (*p) {
        char ty[32];
        char id[32];
        size_t ti = 0, ni = 0;
        while (*p == ' ') p++;
        while (*p && *p != ' ' && *p != ',' && ti + 1 < sizeof(ty)) ty[ti++] = *p++;
        ty[ti] = 0;
        while (*p == ' ') p++;
        while (*p && *p != ',' && *p != ' ' && ni + 1 < sizeof(id)) id[ni++] = *p++;
        id[ni] = 0;
        (void)ty;
        if (id[0] && strcmp(id, name) == 0) return 1;
        while (*p && *p != ',') p++;
        if (*p == ',') p++;
    }
    return 0;
}

/* Locals typed as CCClosure* in the enclosing fn — call-form `inc(x)` captures. */
enum { SHADOW_SPAWN_CL_LOCAL_CAP = 256 };
static char g_spawn_cl_locals[SHADOW_SPAWN_CL_LOCAL_CAP][64];
static int g_spawn_n_cl_locals;

static void spawn_note_closure_local(const char* name) {
    int i;
    if (!name || !name[0]) return;
    if (g_spawn_n_cl_locals >= SHADOW_SPAWN_CL_LOCAL_CAP) {
        shadow_table_full("spawn_cl_locals", SHADOW_SPAWN_CL_LOCAL_CAP, name);
        return;
    }
    for (i = 0; i < g_spawn_n_cl_locals; i++)
        if (strcmp(g_spawn_cl_locals[i], name) == 0) return;
    snprintf(g_spawn_cl_locals[g_spawn_n_cl_locals++],
             sizeof(g_spawn_cl_locals[0]), "%s", name);
}

static int spawn_is_closure_local(const char* name) {
    int i;
    if (!name || !name[0]) return 0;
    for (i = 0; i < g_spawn_n_cl_locals; i++)
        if (strcmp(g_spawn_cl_locals[i], name) == 0) return 1;
    return 0;
}

static void spawn_reset_closure_locals(void) { g_spawn_n_cl_locals = 0; }

/* File-scope names: bare uses inside closures must not become captures. */
enum { SHADOW_SPAWN_GLOBAL_CAP = 256 };
static char g_spawn_globals[SHADOW_SPAWN_GLOBAL_CAP][64];
static int g_spawn_nglobals;

static void spawn_note_global(const char* name) {
    int i;
    if (!name || !name[0]) return;
    if (g_spawn_nglobals >= SHADOW_SPAWN_GLOBAL_CAP) {
        shadow_table_full("spawn_globals", SHADOW_SPAWN_GLOBAL_CAP, name);
        return;
    }
    for (i = 0; i < g_spawn_nglobals; i++)
        if (strcmp(g_spawn_globals[i], name) == 0) return;
    snprintf(g_spawn_globals[g_spawn_nglobals++],
             sizeof(g_spawn_globals[0]), "%s", name);
}

static int spawn_is_file_global(const char* name) {
    int i;
    if (!name || !name[0]) return 0;
    for (i = 0; i < g_spawn_nglobals; i++)
        if (strcmp(g_spawn_globals[i], name) == 0) return 1;
    return 0;
}

/* Skip C/CC noise tokens that look like idents in casts / keywords. */
static int spawn_is_noise_ident(const char* id) {
    static const char* kw[] = {
        "int", "void", "char", "long", "short", "signed", "unsigned", "const",
        "static", "struct", "enum", "typedef", "return", "if", "else", "while",
        "for", "do", "switch", "case", "default", "break", "continue", "sizeof",
        "NULL", "true", "false", "bool", "size_t", "intptr_t", "uintptr_t",
        "ptrdiff_t", "uint8_t", "uint16_t", "uint32_t", "uint64_t", "int8_t",
        "int16_t", "int32_t", "int64_t", "float", "double", "auto", "register",
        "volatile", "restrict", "inline", "typeof", "__typeof__", "abort",
        "malloc", "free", "printf", "fprintf", "snprintf", "memcpy", "memset",
        "strlen", "strcmp", "CCSlice", "CCNursery", "CCChan", "CCChanTx",
        "CCChanRx", "CCTask", "CCError", "CCIoError", "chan_send", "chan_recv",
        "chan_try_send", "chan_try_recv", "chan_close", "chan_free",
        "cc_channel_send", "cc_channel_recv", "cc_channel_pair", "cc_chan_free",
        "cc_nursery_wait", "cc_nursery_free", "cleanup_fn", "usleep", "sleep",
        "nanosleep", "pthread_create", "pthread_join", "stderr", "stdout",
        "stdin", "fprintf", "FILE", "NULL", "cc_io_avail", "cc_atomic_fetch_add",
        "cc_atomic_load", "abort", NULL};
    int i;
    if (!id || !id[0]) return 1;
    /* Type-ish tokens (Item, CCResult_…) are not value captures. */
    if (id[0] >= 'A' && id[0] <= 'Z') return 1;
    /* Generated temps / macros (`__cc_nursery47`) stay unbound. */
    if (strncmp(id, "__cc_", 5) == 0 || strncmp(id, "__CC_", 5) == 0)
        return 1;
    /* C11 atomic memory_order_* enumerators — not locals. */
    if (strncmp(id, "memory_order_", 13) == 0) return 1;
    for (i = 0; kw[i]; i++)
        if (strcmp(id, kw[i]) == 0) return 1;
    return 0;
}

/* Scan C-ish text for free idents → value captures (skip formals / locals). */
static void spawn_infer_caps_from_text(Parser* p, AstNode* n, const char* text) {
    const char* cur = text ? text : "";
    const char* start = cur;
    while (*cur) {
        /* Skip string / char literals so "spawned task\n" is not captures. */
        if (*cur == '"' || *cur == '\'') {
            char q = *cur++;
            while (*cur && *cur != q) {
                if (*cur == '\\' && cur[1]) cur++;
                cur++;
            }
            if (*cur == q) cur++;
            continue;
        }
        /* Skip numeric literals so `0xffff` is not captured as `xffff`. */
        if (*cur >= '0' && *cur <= '9') {
            if (cur[0] == '0' && (cur[1] == 'x' || cur[1] == 'X')) {
                cur += 2;
                while ((*cur >= '0' && *cur <= '9') || (*cur >= 'a' && *cur <= 'f') ||
                       (*cur >= 'A' && *cur <= 'F'))
                    cur++;
            } else {
                while (*cur >= '0' && *cur <= '9') cur++;
            }
            while (*cur == 'u' || *cur == 'U' || *cur == 'l' || *cur == 'L') cur++;
            continue;
        }
        if (((*cur >= 'a' && *cur <= 'z') || (*cur >= 'A' && *cur <= 'Z') || *cur == '_')) {
            char id[64];
            size_t nlen = 0;
            const char* id0 = cur;
            int after_dot = 0;
            /* Skip struct/union field names: `s.ptr` / `s->len`. */
            if (id0 > start) {
                const char* b = id0 - 1;
                while (b > start && (*b == ' ' || *b == '\t')) b--;
                if (*b == '.')
                    after_dot = 1;
                else if (*b == '>' && b > start && b[-1] == '-')
                    after_dot = 1;
            }
            while (spawn_is_ident_char(*cur)) {
                if (nlen + 1 < sizeof(id)) id[nlen++] = *cur;
                cur++;
            }
            id[nlen] = 0;
            {
                const char* after = cur;
                int is_call = 0;
                while (*after == ' ' || *after == '\t') after++;
                /* `usleep(…)` / `printf(…)` are callees, not captures. */
                if (*after == '(') is_call = 1;
                /* `&g_worker_count` / `&local` — address-of is not a value
             * capture (globals must stay unbound; refs use explicit `&name`). */
            int after_amp = 0;
            if (id0 > start) {
                const char* b = id0 - 1;
                while (b > start && (*b == ' ' || *b == '\t')) b--;
                if (*b == '&') after_amp = 1;
            }
            /* Call-form: skip callees unless a CCClosure local (`inc(x)`).
             * File-scope names (incl. global arrays) need no capture. */
            if (!after_dot && !after_amp &&
                !(is_call && !spawn_is_closure_local(id)) &&
                spawn_is_ident(id) && !spawn_is_noise_ident(id) &&
                !spawn_is_file_global(id) &&
                !spawn_body_declares(n, id) && !spawn_name_is_formal(n, id))
                    spawn_caps_add(p, &n->e, id);
            }
            continue;
        }
        cur++;
    }
}

static int spawn_looks_like_chan(const char* id) {
    size_t n;
    if (!id || !id[0]) return 0;
    /* Channel *handles* only — never callees (cc_chan_send) or globals
     * that merely end in `_done` (g_done is an atomic, not a chan). */
    if (strcmp(id, "tx") == 0 || strcmp(id, "rx") == 0) return 1;
    if (strcmp(id, "block_done") == 0) return 1;
    n = strlen(id);
    if (n >= 3 && (strcmp(id + n - 3, "_tx") == 0 || strcmp(id + n - 3, "_rx") == 0))
        return 1;
    if (n >= 5 && strcmp(id + n - 5, "_chan") == 0) return 1;
    return 0;
}

/* Chan-handle idents in text → value captures (spawn/create beachhead). */
static void spawn_infer_chan_caps_from_text(Parser* p, AstNode* n, const char* text) {
    const char* cur = text ? text : "";
    while (*cur) {
        if (((*cur >= 'a' && *cur <= 'z') || (*cur >= 'A' && *cur <= 'Z') || *cur == '_')) {
            char id[64];
            size_t nlen = 0;
            while ((*cur >= 'a' && *cur <= 'z') || (*cur >= 'A' && *cur <= 'Z') ||
                   (*cur >= '0' && *cur <= '9') || *cur == '_') {
                if (nlen + 1 < sizeof(id)) id[nlen++] = *cur;
                cur++;
            }
            id[nlen] = 0;
            if (spawn_looks_like_chan(id) && !spawn_body_declares(n, id) &&
                !spawn_name_is_formal(n, id))
                spawn_caps_add(p, &n->e, id);
            continue;
        }
        cur++;
    }
}

/* Infer value captures from one stmt (recurses into if/loop/block). */
static void spawn_infer_value_caps_stmt(Parser* p, AstNode* n, AstNode* s) {
    int k;
    if (!n || !s) return;
    if (s->kind == AST_TYPED_INIT && s->d && s->d[0] != '*' && spawn_is_ident(s->c) &&
        !spawn_is_noise_ident(s->c) && !spawn_body_declares(n, s->c) &&
        !spawn_name_is_formal(n, s->c)) {
        spawn_caps_add(p, &n->e, s->c);
    }
    /* Callarg: capture free idents from assign RHS (`*gp = x` → x), not bare
     * assign LHS (globals like `g = 1` must stay unbound). */
    if (s->kind == AST_ASSIGN && strncmp(ast_slot(n->b), "callarg", 7) == 0) {
        spawn_infer_caps_from_text(p, n, s->b);
        if ((s->a && s->a[0] == '*'))
            spawn_infer_caps_from_text(p, n, s->a);
    }
    /* Spawn/create/callarg: free idents in conds / inits / UFCS / calls.
     * Skip assign LHS/RHS full scan — globals like `g = 9` must stay unbound. */
    if (strcmp(ast_slot(n->b), "spawn") == 0 || strcmp(ast_slot(n->b), "spawnhybrid") == 0 ||
        strcmp(ast_slot(n->b), "spawnhybrid_unsafe") == 0 ||
        strcmp(ast_slot(n->b), "create") == 0 ||
        strcmp(ast_slot(n->b), "send_task") == 0 ||
        strncmp(ast_slot(n->b), "callarg", 7) == 0) {
        if (s->kind == AST_CALL_ARGS)
            spawn_infer_caps_from_text(p, n, s->b);
        if (s->kind == AST_VOID_CAST)
            spawn_infer_caps_from_text(p, n, s->a);
        if (s->kind == AST_UFCS_STMT || s->kind == AST_UFCS_EXPR) {
            spawn_infer_caps_from_text(p, n, s->a);
            spawn_infer_caps_from_text(p, n, s->c);
        }
        /* CALL_NUM.a is the callee (usleep) — never a capture. */
        if (s->kind == AST_EXPR_STMT)
            spawn_infer_caps_from_text(p, n, s->a);
        /* `return inc(x);` inside () => — capture closure callees. */
        if (s->kind == AST_RETURN_EXPR || s->kind == AST_RETURN_CC)
            spawn_infer_caps_from_text(p, n, s->a);
        if (s->kind == AST_IF || s->kind == AST_WHILE)
            spawn_infer_caps_from_text(p, n, s->a);
        if (s->kind == AST_TYPED_INIT || s->kind == AST_PTR_INIT)
            spawn_infer_caps_from_text(p, n, s->c);
        if (s->kind == AST_ASSIGN && strncmp(ast_slot(n->b), "callarg", 7) != 0) {
            spawn_infer_chan_caps_from_text(p, n, s->a);
            spawn_infer_chan_caps_from_text(p, n, s->b);
        }
    }
    if (s->kind == AST_PRINTLN_TPL) {
        const char* cur = s->a;
        while (cur && *cur) {
            const char* slot = strstr(cur, "${");
            if (!slot) break;
            const char* end = strchr(slot + 2, '}');
            if (!end) break;
            char id[64];
            size_t el = (size_t)(end - (slot + 2));
            if (el < sizeof(id)) {
                memcpy(id, slot + 2, el);
                id[el] = 0;
                if (spawn_is_ident(id) && !spawn_body_declares(n, id) &&
                    !spawn_name_is_formal(n, id))
                    spawn_caps_add(p, &n->e, id);
            }
            cur = end + 1;
        }
    }
    if (s->kind == AST_IF || s->kind == AST_WHILE || s->kind == AST_FOR ||
        s->kind == AST_BLOCK || s->kind == AST_DO_WHILE) {
        for (k = 0; k < s->nbody; k++) spawn_infer_value_caps_stmt(p, n, s->body[k]);
        for (k = 0; k < s->ndbody; k++) spawn_infer_value_caps_stmt(p, n, s->dbody[k]);
        if (s->kids) {
            for (k = 0; k < s->nkids; k++)
                spawn_infer_value_caps_stmt(p, n, s->kids[k]);
        }
    }
}

/* Walk body text for channel-shaped free names only (tx, rx, star_tx, star_rx). */
static void spawn_infer_chan_caps_stmt(Parser* p, AstNode* n, AstNode* s) {
    int k;
    if (!n || !s) return;
    spawn_infer_chan_caps_from_text(p, n, s->a);
    spawn_infer_chan_caps_from_text(p, n, s->b);
    spawn_infer_chan_caps_from_text(p, n, s->c);
    for (k = 0; k < s->nbody; k++) spawn_infer_chan_caps_stmt(p, n, s->body[k]);
    for (k = 0; k < s->ndbody; k++) spawn_infer_chan_caps_stmt(p, n, s->dbody[k]);
    if (s->kids) {
        for (k = 0; k < s->nkids; k++) spawn_infer_chan_caps_stmt(p, n, s->kids[k]);
    }
}

/* Infer value captures from `int x = outer;`, assigns, and `${outer}` in body.
 * Explicit `[caps]` are a floor: still pull in channel endpoints used in the
 * body (`=> [n] { chan_send(tx,…) }` → capture `tx`), but do not re-merge
 * general value idents (that slurps body locals / pigz `g`). */
static void spawn_infer_value_caps(Parser* p, AstNode* n) {
    if (!n) return;
    if (n->e && n->e[0]) {
        for (int k = 0; k < n->nbody; k++)
            spawn_infer_chan_caps_stmt(p, n, n->body[k]);
        return;
    }
    for (int k = 0; k < n->nbody; k++)
        spawn_infer_value_caps_stmt(p, n, n->body[k]);
}

/* recv.spawn(() => …) and recv.send_task(() => …) are ordinary UFCS
 * plus a call-arg closure. !>; / !>(e){…} is the UFCS unwrap path. */

