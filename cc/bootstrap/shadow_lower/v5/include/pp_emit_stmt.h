/* Statement / TU emit: shadow_emit_stmt_ctx, destroy, file product.
 * Requires pp_emit_core.cch + pp_emit_ufcs.cch. */
#pragma once

/* 1 = emit CCShadow* beachhead stubs; 0 = real CCSlice_* / CCChan* from runtime. */
static int g_shadow_slice_stub = 1;
static int g_shadow_chan_stub = 1;

static const char* shadow_slice_ty(const char* elem) {
    static char buf[96];
    char mangled[96];
    size_t i, o = 0;
    const char* e = (elem && elem[0]) ? elem : "int";
    /* `unsigned long` → `unsigned_long` for the CCSlice_* instance name. */
    for (i = 0; e[i] && o + 1 < sizeof(mangled); i++) {
        if (e[i] == ' ' || e[i] == '\t') {
            if (o && mangled[o - 1] != '_') mangled[o++] = '_';
        } else
            mangled[o++] = e[i];
    }
    mangled[o] = 0;
    if (g_shadow_slice_stub)
        snprintf(buf, sizeof(buf), "CCShadowSlice_%s", mangled);
    else
        snprintf(buf, sizeof(buf), "CCSlice_%s", mangled);
    return buf;
}
static const char* shadow_chan_ty(void) {
    return g_shadow_chan_stub ? "CCShadowChan_int" : "CCChan*";
}
/* Directed handle from AST_CHAN_VAR.c (">" / "<" / "owned"); else stub. */
static const char* shadow_chan_handle_ty(AstNode* n) {
    if (n && strcmp(n->c, "owned") == 0) return "CCChan*";
    if (n && n->c[0] == '<') return "CCChanRx";
    if (n && n->c[0] == '>') return "CCChanTx";
    return shadow_chan_ty();
}

static int shadow_emit_stmt_ctx(AstNode* st, CEmit* out, ShadowCtx* ctx,
                                const char* indent, int use_cleanup);
static int shadow_stmt_uses_scratch(AstNode* st);
static int shadow_emit_one_destroy(AstNode* st, CEmit* out, ShadowCtx* ctx,
                                   const char* indent);
static int shadow_stmt_is_destroy(AstNode* st);
/* Defined in pp_emit_tu.cch (included after this switch). */
static int shadow_emit_typedef_struct(AstNode* it, CEmit* out, TapeCache* cache,
                                      ShadowCtx* line_ctx);

static int shadow_emit_err_delegate(CEmit* out, ShadowCtx* ctx,
                                    const char* local_bind,
                                    const char* indent);
static void shadow_subst_bind_on_node(AstNode* st, const char* from,
                                      const char* to);

static int shadow_emit_handler(CEmit* out, ShadowCtx* ctx, const char* bind,
                               const char* indent) {
    int use_c = ctx && ctx->goto_cleanup;
    if (ctx && ctx->eh && ctx->eh->nbody > 0) {
        for (int k = 0; k < ctx->eh->nbody; k++) {
            AstNode eh_copy = *ctx->eh->body[k];
            if (ctx->eh->b[0] && bind && bind[0] &&
                strcmp(ctx->eh->b, bind) != 0)
                shadow_subst_bind_on_node(&eh_copy, ctx->eh->b, bind);
            char saved = eh_copy.indent[0];
            eh_copy.indent[0] = 0;
            eh_copy.lead_len = 0;
            int ok = shadow_emit_stmt_ctx(&eh_copy, out, ctx, indent, use_c);
            eh_copy.indent[0] = saved;
            if (!ok) return 0;
        }
        return 1;
    }
    const char* h = (ctx && ctx->eh && ctx->eh->c[0]) ? ctx->eh->c : "cc_error_exit";
    return cemit_fmt(out, "%s%s(%s);\n", indent, h, bind);
}

static int shadow_mode_has_destroy(const char* mode) {
    return mode && strstr(mode, "_D") != NULL;
}
static int shadow_mode_destroy_bare(const char* mode) {
    return mode && strstr(mode, "_Dbare") != NULL;
}

/* Extracted to pp_emit_unwrap.cch / pp_emit_spawn.cch */
static void shadow_resolve_at_create(char* expr, size_t cap, const char* ty,
                                     int is_ptr, int with_closure);
static int shadow_is_expr_closure(AstNode* kid);
static AstNode* shadow_expr_closure_kid(AstNode* st);
static int shadow_closure_arity(AstNode* cl);
static const char* shadow_closure_cc_ty(AstNode* cl);
static void shadow_fmt_closure_make(char* dst, size_t cap, AstNode* cl);
static void shadow_splice_closure_arg(char* expr, size_t cap, AstNode* cl);
static int shadow_emit_try_call(CEmit* out, ShadowCtx* ctx, const char* indent,
                                const char* call, const char* site,
                                const char* bind, int discard_ok);
static int shadow_emit_try_assign(CEmit* out, ShadowCtx* ctx, const char* indent,
                                  const char* ty_lhs, const char* name,
                                  const char* call, const char* site,
                                  const char* bind, const char* value_expr);
static int shadow_emit_println(AstNode* st, CEmit* out, ShadowCtx* ctx,
                               const char* indent);
static int shadow_emit_println_tpl(AstNode* st, CEmit* out, ShadowCtx* ctx,
                                   const char* indent);
static int shadow_emit_bang_bind(AstNode* st, CEmit* out, ShadowCtx* ctx,
                                 const char* indent);
static int shadow_emit_var_unwrap(AstNode* st, CEmit* out, ShadowCtx* ctx,
                                  const char* indent);
static int shadow_emit_err_syntax(AstNode* st, CEmit* out, ShadowCtx* ctx,
                                  const char* indent);
static int shadow_emit_err_delegate(CEmit* out, ShadowCtx* ctx,
                                    const char* local_bind,
                                    const char* indent);
static int shadow_emit_closure_def(AstNode* cl, CEmit* out, TapeCache* cache);
static int shadow_emit_defer_epilogue(CEmit* out, ShadowCtx* ctx);
static int shadow_collect_spawns(AstNode* st, AstNode** spawns, int* nspawns,
                                 int cap);

static int g_shadow_closure_id = 1;
static int g_shadow_async_id = 60000;

/* Resolve `__cc_at_create(…)` from declared dest type (create-hook beachhead).
 * with_closure: nursery create-arg was `() => {…}` → spawn_child_closure0. */
static int shadow_dbody_is_stmt(AstNode* s) {
    if (!s) return 0;
    if (s->kind == AST_UFCS_EXPR || s->kind == AST_UFCS_STMT) return 0;
    if (s->kind == AST_SPAWN_CLOSURE) return 0;
    return 1;
}

/* Parse `@string(...)` forms.
 * Returns: 1 = backtick template (`tpl`, arena);
 *          2 = from-arg (arg, arena);
 *          3 = policy (policy, `tpl`, arena) — policy written when non-NULL;
 *          0 = not @string. */
/* Closing ` of an @string template; skip `${{…}}` so inner ticks survive. */
static const char* shadow_tpl_close_tick(const char* tick0) {
    const char* p;
    if (!tick0 || *tick0 != '`') return NULL;
    p = tick0 + 1;
    while (*p) {
        if (p[0] == '$' && p[1] == '{' && p[2] == '{') {
            const char* vend = strstr(p + 3, "}}");
            if (!vend) return NULL;
            p = vend + 2;
            continue;
        }
        if (*p == '`') return p;
        p++;
    }
    return NULL;
}

static int shadow_parse_at_string_expr_ex(const char* src, char* tpl, size_t tcap,
                                          char* arena, size_t acap,
                                          char* policy, size_t pcap) {
    const char* p;
    const char* tick0;
    const char* tick1;
    const char* comma;
    const char* rp;
    const char* arg0;
    size_t n;
    int depth;
    char pol_tmp[128];
    char* pol = policy;
    size_t polcap = pcap;
    if (!src || strncmp(src, "@string(", 8) != 0) return 0;
    if (!pol || !polcap) {
        pol = pol_tmp;
        polcap = sizeof(pol_tmp);
    }
    p = src + 8;
    while (*p == ' ' || *p == '\t') p++;
    /* Policy form: @string(policy, `…`, arena) */
    if (*p != '`') {
        arg0 = p;
        depth = 0;
        comma = NULL;
        for (p = arg0; *p; p++) {
            if (*p == '(' || *p == '[' || *p == '{') depth++;
            else if (*p == ')' || *p == ']' || *p == '}') {
                if (depth == 0) break;
                if (depth > 0) depth--;
            } else if (*p == ',' && depth == 0) {
                comma = p;
                break;
            }
        }
        if (comma) {
            const char* q = comma + 1;
            while (*q == ' ' || *q == '\t') q++;
            if (*q == '`') {
                n = (size_t)(comma - arg0);
                while (n > 0 && (arg0[n - 1] == ' ' || arg0[n - 1] == '\t')) n--;
                if (n >= polcap) n = polcap - 1;
                memcpy(pol, arg0, n);
                pol[n] = 0;
                if (policy && pcap) {
                    if (n >= pcap) n = pcap - 1;
                    memcpy(policy, pol, n);
                    policy[n] = 0;
                }
                tick0 = q;
                tick1 = shadow_tpl_close_tick(tick0);
                if (!tick1) return 0;
                comma = tick1 + 1;
                while (*comma == ' ' || *comma == '\t') comma++;
                if (*comma != ',') return 0;
                comma++;
                while (*comma == ' ' || *comma == '\t') comma++;
                {
                    const char* q = comma;
                    int dep = 0;
                    rp = NULL;
                    for (; *q; q++) {
                        if (*q == '(') dep++;
                        else if (*q == ')') {
                            if (dep == 0) {
                                rp = q;
                                break;
                            }
                            dep--;
                        }
                    }
                }
                if (!rp || rp <= comma) return 0;
                n = (size_t)(tick1 - (tick0 + 1));
                if (n >= tcap) n = tcap - 1;
                memcpy(tpl, tick0 + 1, n);
                tpl[n] = 0;
                n = (size_t)(rp - comma);
                while (n > 0 && (comma[n - 1] == ' ' || comma[n - 1] == '\t')) n--;
                if (n >= acap) n = acap - 1;
                memcpy(arena, comma, n);
                arena[n] = 0;
                return 3;
            }
        }
        p = src + 8;
        while (*p == ' ' || *p == '\t') p++;
    }
    /* Backtick template: @string(`…`, arena) or arena-less @string(`…`) */
    if (*p == '`') {
        tick0 = p;
        tick1 = shadow_tpl_close_tick(tick0);
        if (!tick1) return 0;
        comma = tick1 + 1;
        while (*comma == ' ' || *comma == '\t' || *comma == '\n') comma++;
        n = (size_t)(tick1 - (tick0 + 1));
        if (n >= tcap) n = tcap - 1;
        memcpy(tpl, tick0 + 1, n);
        tpl[n] = 0;
        if (*comma == ')') {
            /* Arena-less stack form. */
            if (acap) arena[0] = 0;
            return 4;
        }
        if (*comma != ',') return 0;
        comma++;
        while (*comma == ' ' || *comma == '\t') comma++;
        /* Balanced `)` — do not use strrchr (callers may nest @string in
         * println(...)). */
        {
            const char* q = comma;
            int dep = 0;
            rp = NULL;
            for (; *q; q++) {
                if (*q == '(') dep++;
                else if (*q == ')') {
                    if (dep == 0) {
                        rp = q;
                        break;
                    }
                    dep--;
                }
            }
        }
        if (!rp || rp <= comma) return 0;
        n = (size_t)(rp - comma);
        while (n > 0 && (comma[n - 1] == ' ' || comma[n - 1] == '\t')) n--;
        if (n >= acap) n = acap - 1;
        memcpy(arena, comma, n);
        arena[n] = 0;
        return 1;
    }
    /* From-arg: @string(arg, arena) — not the 3-arg policy form. */
    arg0 = p;
    depth = 0;
    comma = NULL;
    for (p = arg0; *p; p++) {
        if (*p == '(' || *p == '[' || *p == '{') depth++;
        else if (*p == ')' || *p == ']' || *p == '}') {
            if (depth == 0 && *p == ')') {
                rp = p;
                break;
            }
            if (depth > 0) depth--;
        } else if (*p == ',' && depth == 0 && !comma) {
            comma = p;
        }
    }
    if (!comma || !rp || rp <= comma) return 0;
    /* Reject 3-arg policy `@string(fn, `tpl`, arena)` for now. */
    {
        const char* q = comma + 1;
        while (*q == ' ' || *q == '\t') q++;
        if (*q == '`') return 0;
        depth = 0;
        for (q = comma + 1; q < rp; q++) {
            if (*q == '(' || *q == '[' || *q == '{') depth++;
            else if (*q == ')' || *q == ']' || *q == '}') {
                if (depth > 0) depth--;
            } else if (*q == ',' && depth == 0)
                return 0; /* third arg */
        }
    }
    n = (size_t)(comma - arg0);
    while (n > 0 && (arg0[n - 1] == ' ' || arg0[n - 1] == '\t')) n--;
    if (n >= tcap) n = tcap - 1;
    memcpy(tpl, arg0, n);
    tpl[n] = 0;
    p = comma + 1;
    while (*p == ' ' || *p == '\t') p++;
    n = (size_t)(rp - p);
    while (n > 0 && (p[n - 1] == ' ' || p[n - 1] == '\t')) n--;
    if (n >= acap) n = acap - 1;
    memcpy(arena, p, n);
    arena[n] = 0;
    return 2;
}

static int shadow_parse_at_string_expr(const char* src, char* tpl, size_t tcap,
                                       char* arena, size_t acap) {
    return shadow_parse_at_string_expr_ex(src, tpl, tcap, arena, acap, NULL, 0);
}

/* Emit one literal run — escape `\` / `"` for a C string literal.
 * Chunk when escaped text would exceed the temp buffer (py_baseline @string
 * bodies are multi-KiB; a 512-byte esc buffer used to truncate silently). */
static int shadow_emit_tpl_lit(CEmit* out, const char* indent, const char* msg,
                               const char* lit, size_t n, const char* arena) {
    char buf[4096];
    size_t i = 0;
    if (!n) return 1;
    while (i < n) {
        size_t o = 0;
        size_t chunk0 = i;
        while (i < n && o + 2 < sizeof(buf)) {
            char c = lit[i];
            if (c == '\\' || c == '"') {
                buf[o++] = '\\';
                buf[o++] = c;
            } else if (c == '\n') {
                buf[o++] = '\\';
                buf[o++] = 'n';
            } else if (c == '\r') {
                buf[o++] = '\\';
                buf[o++] = 'r';
            } else if (c == '\t') {
                buf[o++] = '\\';
                buf[o++] = 't';
            } else {
                buf[o++] = c;
            }
            i++;
        }
        buf[o] = 0;
        if (!cemit_fmt(out, "%scc_string_push_buffer(&%s, \"%s\", %u, %s);\n",
                       indent, msg, buf, (unsigned)(i - chunk0), arena))
            return 0;
    }
    return 1;
}

static void shadow_tpl_dedent_inplace(char* tpl, size_t cap);

/* @string(`tpl`, arena) → CCString local (hand-lowered shape).
 * When policy_expr is set, `$~tag{expr}` / `${…}` use cc_string_push_policy.
 * `\${` / `\$~` / `\\` are literal (not slots). */
static int shadow_emit_tpl_build_ex(CEmit* out, const char* tpl,
                                   const char* arena_expr, const char* indent,
                                   const char* msg_name, const char* policy_expr) {
    char ded[4096];
    if (!cemit_fmt(out,
            "%sCCString %s = cc_string_new();\n",
            indent, msg_name))
        return 0;
    snprintf(ded, sizeof(ded), "%s", tpl ? tpl : "");
    shadow_tpl_dedent_inplace(ded, sizeof(ded));
    tpl = ded;
    const char* p = tpl ? tpl : "";
    const char* lit0 = p;
    while (*p) {
        /* Escaped dollar / backslash → literal. */
        if (p[0] == '\\' && p[1] == '\\') {
            if (!shadow_emit_tpl_lit(out, indent, msg_name, lit0,
                                    (size_t)(p - lit0), arena_expr))
                return 0;
            if (!shadow_emit_tpl_lit(out, indent, msg_name, "\\", 1, arena_expr))
                return 0;
            p += 2;
            lit0 = p;
            continue;
        }
        if (p[0] == '\\' && (p[1] == 'n' || p[1] == 'r' || p[1] == 't')) {
            char esc[2];
            if (!shadow_emit_tpl_lit(out, indent, msg_name, lit0,
                                    (size_t)(p - lit0), arena_expr))
                return 0;
            esc[0] = (p[1] == 'n') ? '\n' : (p[1] == 'r') ? '\r' : '\t';
            esc[1] = 0;
            if (!shadow_emit_tpl_lit(out, indent, msg_name, esc, 1, arena_expr))
                return 0;
            p += 2;
            lit0 = p;
            continue;
        }
        /* `${{…}}` verbatim span — raw bytes to first `}}`. */
        if (p[0] == '$' && p[1] == '{' && p[2] == '{') {
            const char* vend = strstr(p + 3, "}}");
            if (!shadow_emit_tpl_lit(out, indent, msg_name, lit0,
                                    (size_t)(p - lit0), arena_expr))
                return 0;
            if (!vend) {
                fprintf(stderr, "error: unterminated ${{...}} in @string\n");
                out->err = 1;
                return 0;
            }
            if (!shadow_emit_tpl_lit(out, indent, msg_name, p + 3,
                                    (size_t)(vend - (p + 3)), arena_expr))
                return 0;
            p = vend + 2;
            lit0 = p;
            continue;
        }
        if (p[0] == '\\' && p[1] == '$' && (p[2] == '{' || p[2] == '~')) {
            if (!shadow_emit_tpl_lit(out, indent, msg_name, lit0,
                                    (size_t)(p - lit0), arena_expr))
                return 0;
            /* emit `$` then rest of `{`/`~…` as literal via continuing */
            if (!shadow_emit_tpl_lit(out, indent, msg_name, "$", 1, arena_expr))
                return 0;
            p += 2; /* point at { or ~ */
            lit0 = p;
            continue;
        }
        if (p[0] == '$' && p[1] == '~') {
            const char* tag0 = p + 2;
            const char* brace = strchr(tag0, '{');
            const char* end;
            char tag[64], expr[128], uexpr[192];
            size_t tl, el;
            if (!shadow_emit_tpl_lit(out, indent, msg_name, lit0,
                                    (size_t)(p - lit0), arena_expr))
                return 0;
            if (!brace) {
                fprintf(stderr, "error: unterminated $~tag{...} in @string\n");
                out->err = 1;
                return 0;
            }
            tl = (size_t)(brace - tag0);
            if (tl >= sizeof(tag)) tl = sizeof(tag) - 1;
            memcpy(tag, tag0, tl);
            tag[tl] = 0;
            end = strchr(brace + 1, '}');
            if (!end) {
                fprintf(stderr, "error: unterminated $~tag{...} in @string\n");
                out->err = 1;
                return 0;
            }
            el = (size_t)(end - (brace + 1));
            if (el >= sizeof(expr)) el = sizeof(expr) - 1;
            memcpy(expr, brace + 1, el);
            expr[el] = 0;
            shadow_emit_text_ufcs(uexpr, sizeof(uexpr), expr, NULL);
            if (policy_expr && policy_expr[0]) {
                if (!cemit_fmt(out,
                        "%scc_string_push_policy(&%s, %s, %s, "
                        "cc_slice_from_cstr(\"%s\"), "
                        "cc__string_slot_from_int((%s), %s));\n",
                        indent, msg_name, policy_expr, arena_expr, tag, uexpr,
                        arena_expr))
                    return 0;
            } else if (!cemit_fmt(out,
                    "%scc__string_slot_push(&%s, (%s), %s);\n",
                    indent, msg_name, uexpr, arena_expr))
                return 0;
            p = end + 1;
            lit0 = p;
            continue;
        }
        if (p[0] == '$' && p[1] == '{') {
            const char* end = strchr(p + 2, '}');
            char expr[128], uexpr[192];
            size_t el;
            if (!shadow_emit_tpl_lit(out, indent, msg_name, lit0,
                                    (size_t)(p - lit0), arena_expr))
                return 0;
            if (!end) {
                fprintf(stderr, "error: unterminated ${...} in @string template\n");
                out->err = 1;
                return 0;
            }
            el = (size_t)(end - (p + 2));
            if (el >= sizeof(expr)) el = sizeof(expr) - 1;
            memcpy(expr, p + 2, el);
            expr[el] = 0;
            shadow_emit_text_ufcs(uexpr, sizeof(uexpr), expr, NULL);
            if (policy_expr && policy_expr[0]) {
                if (!cemit_fmt(out,
                        "%scc_string_push_policy(&%s, %s, %s, cc_slice_empty(), "
                        "cc__string_slot_arg((%s), %s));\n",
                        indent, msg_name, policy_expr, arena_expr, uexpr,
                        arena_expr))
                    return 0;
            } else if (!cemit_fmt(out,
                    "%scc__string_slot_push(&%s, (%s), %s);\n",
                    indent, msg_name, uexpr, arena_expr))
                return 0;
            p = end + 1;
            lit0 = p;
            continue;
        }
        p++;
    }
    return shadow_emit_tpl_lit(out, indent, msg_name, lit0, (size_t)(p - lit0),
                               arena_expr);
}

static int shadow_emit_tpl_build(CEmit* out, const char* tpl, const char* arena_expr,
                                 const char* indent, const char* msg_name) {
    return shadow_emit_tpl_build_ex(out, tpl, arena_expr, indent, msg_name, NULL);
}

/* Types with a `cc__string_stack_bound` / `_push` arm (string.cch). */
static int shadow_ty_is_stack_string_bounded(const char* ty) {
    char buf[128];
    const char* p;
    size_t n = 0;
    int sp = 0;
    if (!ty || !ty[0]) return 0;
    p = ty;
    while (*p == ' ' || *p == '\t') p++;
    if (strncmp(p, "const ", 6) == 0) {
        p += 6;
        while (*p == ' ' || *p == '\t') p++;
    }
    while (*p && n + 1 < sizeof(buf)) {
        if (*p == ' ' || *p == '\t') {
            sp = 1;
            p++;
            continue;
        }
        if (sp && n) buf[n++] = ' ';
        sp = 0;
        buf[n++] = *p++;
    }
    buf[n] = 0;
    while (n && buf[n - 1] == ' ') buf[--n] = 0;
    if (!buf[0]) return 0;
    if (strchr(buf, '*') || strchr(buf, '[') || strchr(buf, ':')) return 0;
    if (strcmp(buf, "CCString") == 0) return 0;
    if (strncmp(buf, "CCSlice", 7) == 0) return 0;
    if (strcmp(buf, "float") == 0 || strcmp(buf, "double") == 0 ||
        strcmp(buf, "long double") == 0)
        return 0;
    if (strcmp(buf, "char") == 0 || strcmp(buf, "bool") == 0 ||
        strcmp(buf, "_Bool") == 0)
        return 1;
    if (strcmp(buf, "signed char") == 0 || strcmp(buf, "unsigned char") == 0)
        return 1;
    if (strcmp(buf, "short") == 0 || strcmp(buf, "signed short") == 0 ||
        strcmp(buf, "unsigned short") == 0)
        return 1;
    if (strcmp(buf, "int") == 0 || strcmp(buf, "signed") == 0 ||
        strcmp(buf, "signed int") == 0 || strcmp(buf, "unsigned") == 0 ||
        strcmp(buf, "unsigned int") == 0)
        return 1;
    if (strcmp(buf, "long") == 0 || strcmp(buf, "signed long") == 0 ||
        strcmp(buf, "unsigned long") == 0 || strcmp(buf, "long long") == 0 ||
        strcmp(buf, "signed long long") == 0 ||
        strcmp(buf, "unsigned long long") == 0)
        return 1;
    if (strcmp(buf, "int8_t") == 0 || strcmp(buf, "uint8_t") == 0 ||
        strcmp(buf, "int16_t") == 0 || strcmp(buf, "uint16_t") == 0 ||
        strcmp(buf, "int32_t") == 0 || strcmp(buf, "uint32_t") == 0 ||
        strcmp(buf, "int64_t") == 0 || strcmp(buf, "uint64_t") == 0 ||
        strcmp(buf, "size_t") == 0 || strcmp(buf, "ptrdiff_t") == 0 ||
        strcmp(buf, "intptr_t") == 0 || strcmp(buf, "uintptr_t") == 0)
        return 1;
    /* Any other known type has no stack-bound _Generic arm. */
    return 0;
}

/* 1 if slot is a bare identifier (bind-lookupable). */
static int shadow_stack_slot_ident(const char* slot, char* id, size_t cap) {
    const char* p = slot ? slot : "";
    size_t n = 0;
    while (*p == ' ' || *p == '\t') p++;
    if (!shadow_is_id(*p)) return 0;
    while (shadow_is_id(*p) && n + 1 < cap) id[n++] = *p++;
    while (*p == ' ' || *p == '\t') p++;
    if (*p) return 0;
    id[n] = 0;
    return n > 0;
}

/* Diagnose unbounded arena-less interpolations (oracle: compile_err). */
static int shadow_stack_slot_bounded_or_diag(const char* slot) {
    char id[128];
    const ShadowBind* b;
    const char* spell = slot ? slot : "";
    while (*spell == ' ' || *spell == '\t') spell++;
    if (!shadow_stack_slot_ident(slot, id, sizeof(id))) return 1;
    b = shadow_bind_lookup(id);
    if (!b || !b->ty[0]) return 1;
    if (shadow_ty_is_stack_string_bounded(b->ty)) return 1;
    fprintf(stderr,
            "error: arena-less @string: interpolation '${%s}' has no "
            "statically bounded width (allowed: ${int}/${i64}/${u64}/"
            "${bool}/${char}); pass an arena: @string(`...`, arena)\n",
            spell);
    g_shadow_string_stack_diag = 1;
    return 0;
}

/* Arena-less `@string(`tpl`)` → cc__string_stack_* expression (CCSlice). */
static int shadow_fmt_string_stack_expr(char* dst, size_t cap, const char* tpl) {
    typedef struct { char kind; char buf[128]; size_t n; } Piece;
    Piece pcs[32];
    int np = 0;
    const char* p = tpl ? tpl : "";
    char bound[512];
    char expr[1536];
    size_t bl, el;
    int i;
    bound[0] = 0;
    bl = 0;
    while (*p && np < 32) {
        /* `${{…}}` verbatim (raw) before `${…}` slots. */
        if (p[0] == '$' && p[1] == '{' && p[2] == '{') {
            const char* vend = strstr(p + 3, "}}");
            const char* vs;
            size_t vn;
            if (!vend) return 0;
            vs = p + 3;
            vn = (size_t)(vend - vs);
            if (!(np && pcs[np - 1].kind == 'L')) {
                pcs[np].kind = 'L';
                pcs[np].n = 0;
                np++;
            }
            while (vn && pcs[np - 1].n + 1 < sizeof(pcs[0].buf)) {
                pcs[np - 1].buf[pcs[np - 1].n++] = *vs++;
                vn--;
            }
            p = vend + 2;
            continue;
        }
        /* Bare `$` is literal unless followed by `{` / `~`. */
        if (p[0] == '$' && p[1] == '{') {
            const char* e = strchr(p + 2, '}');
            size_t elen;
            if (!e) return 0;
            elen = (size_t)(e - (p + 2));
            if (elen >= sizeof(pcs[0].buf)) elen = sizeof(pcs[0].buf) - 1;
            pcs[np].kind = 'S';
            memcpy(pcs[np].buf, p + 2, elen);
            pcs[np].buf[elen] = 0;
            pcs[np].n = elen;
            if (!shadow_stack_slot_bounded_or_diag(pcs[np].buf)) return 0;
            np++;
            p = e + 1;
            continue;
        }
        if (p[0] == '\\' && (p[1] == 'n' || p[1] == 'r' || p[1] == 't' ||
                              p[1] == '\\' || p[1] == '`')) {
            char c = (p[1] == 'n') ? '\n' : (p[1] == 'r') ? '\r'
                     : (p[1] == 't') ? '\t' : p[1];
            if (np && pcs[np - 1].kind == 'L') {
                if (pcs[np - 1].n + 1 < sizeof(pcs[0].buf))
                    pcs[np - 1].buf[pcs[np - 1].n++] = c;
            } else {
                pcs[np].kind = 'L';
                pcs[np].buf[0] = c;
                pcs[np].n = 1;
                np++;
            }
            p += 2;
            continue;
        }
        {
            if (!(np && pcs[np - 1].kind == 'L')) {
                pcs[np].kind = 'L';
                pcs[np].n = 0;
                np++;
            }
            if (pcs[np - 1].n + 1 < sizeof(pcs[0].buf))
                pcs[np - 1].buf[pcs[np - 1].n++] = *p;
            p++;
        }
    }
    /* Bound: lit bytes + sum(stack_bound(slot)). */
    bl = (size_t)snprintf(bound, sizeof(bound), "0u");
    for (i = 0; i < np; i++) {
        if (pcs[i].kind == 'L') {
            bl += (size_t)snprintf(bound + bl, sizeof(bound) - bl, " + %uu",
                                   (unsigned)pcs[i].n);
        } else {
            bl += (size_t)snprintf(bound + bl, sizeof(bound) - bl,
                                   " + cc__string_stack_bound((%s))",
                                   pcs[i].buf);
        }
    }
    el = (size_t)snprintf(expr, sizeof(expr),
                          "cc__string_stack_new((char[%s]){0}, %s)", bound,
                          bound);
    for (i = 0; i < np; i++) {
        char nxt[1536];
        if (pcs[i].kind == 'L') {
            char esc[256];
            size_t eo = 0, k;
            for (k = 0; k < pcs[i].n && eo + 2 < sizeof(esc); k++) {
                char c = pcs[i].buf[k];
                if (c == '\\' || c == '"') {
                    esc[eo++] = '\\';
                    esc[eo++] = c;
                } else if (c == '\n') {
                    esc[eo++] = '\\';
                    esc[eo++] = 'n';
                } else if (c == '\r') {
                    esc[eo++] = '\\';
                    esc[eo++] = 'r';
                } else if (c == '\t') {
                    esc[eo++] = '\\';
                    esc[eo++] = 't';
                } else {
                    esc[eo++] = c;
                }
            }
            esc[eo] = 0;
            snprintf(nxt, sizeof(nxt),
                     "cc__string_stack_lit(%s, \"%s\", %u)", expr, esc,
                     (unsigned)pcs[i].n);
        } else {
            snprintf(nxt, sizeof(nxt), "cc__string_stack_push(%s, (%s))", expr,
                     pcs[i].buf);
        }
        snprintf(expr, sizeof(expr), "%s", nxt);
        el = strlen(expr);
        (void)el;
    }
    return snprintf(dst, cap, "cc__string_stack_slice(%s)", expr) < (int)cap;
}

/* `@string(`tpl`, arena)` → GNU statement-expr yielding CCString. */
/* Closer-anchored dedent (matches cc_tpl_dedent_text): when the last line is
 * only whitespace, that margin is stripped from every content line and the
 * closer line is dropped. One-liners / empty margin → no-op.
 * Work on a copy — never leave `tpl` half-split on early return. */
static void shadow_tpl_dedent_inplace(char* tpl, size_t cap) {
    char src[4096];
    char* lines[256];
    int nlines = 0;
    char* p;
    int i;
    int mlen;
    char out[4096];
    size_t o = 0;
    const char* last;
    if (!tpl || !tpl[0] || !cap) return;
    snprintf(src, sizeof(src), "%s", tpl);
    p = src;
    {
        int ended_nl = 0;
        while (*p && nlines < 256) {
            lines[nlines++] = p;
            while (*p && *p != '\n') p++;
            if (*p == '\n') {
                *p++ = 0;
                ended_nl = 1;
            } else
                ended_nl = 0;
        }
        /* Trailing `\n` yields an empty closer-margin line (Python split). */
        if (ended_nl && nlines < 256) lines[nlines++] = p;
    }
    if (nlines < 2) return; /* one-liner / empty */
    last = lines[nlines - 1];
    mlen = 0;
    while (last[mlen] == ' ' || last[mlen] == '\t') mlen++;
    if (last[mlen]) return; /* closer shares line with content */
    /* Drop opener remainder (empty first line after opening tick+newline)
     * and the closer's margin line. Empty margin (mlen==0) still drops those
     * bookends so column-0 twins match indented templates. */
    i = 0;
    if (nlines > 0 && lines[0][0] == 0) i = 1;
    for (; i < nlines - 1 && o + 1 < sizeof(out); i++) {
        const char* s = lines[i];
        int ind = 0;
        int k;
        int margin_ok = 1;
        while (s[ind] == ' ' || s[ind] == '\t') ind++;
        if (!s[ind]) {
            /* Blank line passes through as newline only. */
            out[o++] = '\n';
            continue;
        }
        if (ind < mlen) {
            ind = 0;
            margin_ok = 0;
        } else {
            for (k = 0; k < mlen; k++) {
                if (s[k] != last[k]) {
                    margin_ok = 0;
                    break;
                }
            }
            ind = margin_ok ? mlen : 0;
        }
        {
            size_t rest = strlen(s + ind);
            if (o + rest + 2 >= sizeof(out)) break;
            memcpy(out + o, s + ind, rest);
            o += rest;
            out[o++] = '\n';
        }
    }
    out[o] = 0;
    snprintf(tpl, cap, "%s", out);
}

static int shadow_fmt_string_arena_expr(char* dst, size_t cap, const char* tpl,
                                        const char* arena) {
    CEmit side;
    char name[] = "__cc_tpl";
    char ded[4096];
    memset(&side, 0, sizeof(side));
    snprintf(ded, sizeof(ded), "%s", tpl ? tpl : "");
    shadow_tpl_dedent_inplace(ded, sizeof(ded));
    tpl = ded;
    /* Build pushes into side without the CCString decl line — wrap ourselves. */
    if (!cemit_fmt(&side, "({ CCArena* __cc_tpl_arena = (%s); "
                          "CCString __cc_tpl = cc_string_new(); ",
                   arena)) {
        free(side.buf);
        return 0;
    }
    /* Reuse tpl walker by emitting pushes targeting __cc_tpl. */
    {
        const char* p = tpl ? tpl : "";
        const char* lit0 = p;
        while (*p) {
            if (p[0] == '\\' && p[1] == '\\') {
                if (p > lit0 &&
                    !shadow_emit_tpl_lit(&side, "", name, lit0,
                                        (size_t)(p - lit0), "__cc_tpl_arena")) {
                    free(side.buf);
                    return 0;
                }
                if (!shadow_emit_tpl_lit(&side, "", name, "\\", 1,
                                         "__cc_tpl_arena")) {
                    free(side.buf);
                    return 0;
                }
                p += 2;
                lit0 = p;
                continue;
            }
            if (p[0] == '\\' && (p[1] == 'n' || p[1] == 'r' || p[1] == 't')) {
                char esc[2];
                if (p > lit0 &&
                    !shadow_emit_tpl_lit(&side, "", name, lit0,
                                        (size_t)(p - lit0), "__cc_tpl_arena")) {
                    free(side.buf);
                    return 0;
                }
                esc[0] = (p[1] == 'n') ? '\n' : (p[1] == 'r') ? '\r' : '\t';
                esc[1] = 0;
                if (!shadow_emit_tpl_lit(&side, "", name, esc, 1,
                                         "__cc_tpl_arena")) {
                    free(side.buf);
                    return 0;
                }
                p += 2;
                lit0 = p;
                continue;
            }
            if (p[0] == '$' && p[1] == '{' && p[2] == '{') {
                const char* vend = strstr(p + 3, "}}");
                if (p > lit0 &&
                    !shadow_emit_tpl_lit(&side, "", name, lit0,
                                        (size_t)(p - lit0), "__cc_tpl_arena")) {
                    free(side.buf);
                    return 0;
                }
                if (!vend) {
                    free(side.buf);
                    return 0;
                }
                if (!shadow_emit_tpl_lit(&side, "", name, p + 3,
                                        (size_t)(vend - (p + 3)),
                                        "__cc_tpl_arena")) {
                    free(side.buf);
                    return 0;
                }
                p = vend + 2;
                lit0 = p;
                continue;
            }
            if (p[0] == '\\' && p[1] == '$' &&
                (p[2] == '{' || p[2] == '~')) {
                if (p > lit0 &&
                    !shadow_emit_tpl_lit(&side, "", name, lit0,
                                        (size_t)(p - lit0), "__cc_tpl_arena")) {
                    free(side.buf);
                    return 0;
                }
                if (!shadow_emit_tpl_lit(&side, "", name, "$", 1,
                                         "__cc_tpl_arena")) {
                    free(side.buf);
                    return 0;
                }
                p += 2;
                lit0 = p;
                continue;
            }
            if (p[0] == '$' && p[1] == '{') {
                const char* end = strchr(p + 2, '}');
                char ex[128];
                size_t el;
                if (p > lit0 &&
                    !shadow_emit_tpl_lit(&side, "", name, lit0,
                                        (size_t)(p - lit0), "__cc_tpl_arena")) {
                    free(side.buf);
                    return 0;
                }
                if (!end) {
                    free(side.buf);
                    return 0;
                }
                el = (size_t)(end - (p + 2));
                if (el >= sizeof(ex)) el = sizeof(ex) - 1;
                memcpy(ex, p + 2, el);
                ex[el] = 0;
                if (!cemit_fmt(&side, "cc__string_slot_push(&__cc_tpl, (%s), "
                                     "__cc_tpl_arena); ",
                               ex)) {
                    free(side.buf);
                    return 0;
                }
                p = end + 1;
                lit0 = p;
                continue;
            }
            p++;
        }
        if (p > lit0 &&
            !shadow_emit_tpl_lit(&side, "", name, lit0, (size_t)(p - lit0),
                                 "__cc_tpl_arena")) {
            free(side.buf);
            return 0;
        }
    }
    if (!cemit_str(&side, "__cc_tpl; })")) {
        free(side.buf);
        return 0;
    }
    /* Strip newlines from multi-stmt tpl_lit emit for expr context. */
    {
        size_t i, o = 0;
        for (i = 0; side.buf && i < side.len && o + 1 < cap; i++) {
            char c = side.buf[i];
            if (c == '\n') continue;
            dst[o++] = c;
        }
        dst[o] = 0;
    }
    free(side.buf);
    return dst[0] != 0;
}

static int shadow_arena_is_scratch(const char* arena);

/* Rewrite @string(...) / println(...) occurrences in leftover expr text. */
static void shadow_rewrite_print_and_string(char* expr, size_t cap) {
    /* Expanded @string statement-exprs grow past the input; keep room for
     * opaque switch bodies with multiple templates (matches AST_SWITCH body). */
    char out[8192];
    const char* p;
    char* o;
    size_t rem;
    if (!expr || !cap) return;
    p = expr;
    o = out;
    rem = sizeof(out) - 1;
    out[0] = 0;
    while (*p && rem > 0) {
        /* Aliases are println/eprintln/fprintln only — bare `eprint` /
         * `print` are user-bindable (comment_discipline_smoke). */
        if ((strncmp(p, "println(", 8) == 0 || strncmp(p, "eprintln(", 9) == 0) &&
            (p == expr || !shadow_is_id(p[-1]))) {
            const char* fn = (strncmp(p, "eprintln", 8) == 0)
                                 ? "cc_eprintln"
                                 : "cc_println";
            const char* args = strchr(p, '(');
            int n = snprintf(o, rem, "%s", fn);
            if (n < 0 || (size_t)n >= rem) break;
            o += n;
            rem -= (size_t)n;
            p = args;
            continue;
        }
        if (strncmp(p, "@string(", 8) == 0) {
            char tpl[4096], arena[128], built[8192];
            int sk = shadow_parse_at_string_expr_ex(p, tpl, sizeof(tpl), arena,
                                                    sizeof(arena), NULL, 0);
            const char* end = p;
            int dep = 0;
            while (*end) {
                if (*end == '(') dep++;
                else if (*end == ')') {
                    dep--;
                    if (dep == 0) {
                        end++;
                        break;
                    }
                }
                end++;
            }
            built[0] = 0;
            if (sk == 4) {
                if (!shadow_fmt_string_stack_expr(built, sizeof(built), tpl)) {
                    g_shadow_string_stack_diag = 1;
                    built[0] = 0;
                }
            } else if (sk == 1 || sk == 2) {
                char aexpr[160];
                if (sk == 2)
                    snprintf(built, sizeof(built), "cc_string_from((%s), (%s))",
                             tpl, arena);
                else {
                    if (shadow_arena_is_scratch(arena))
                        snprintf(aexpr, sizeof(aexpr), "&__cc_str_scratch");
                    else
                        snprintf(aexpr, sizeof(aexpr), "%s", arena);
                    (void)shadow_fmt_string_arena_expr(built, sizeof(built), tpl,
                                                       aexpr);
                }
            }
            if (built[0]) {
                size_t bl = strlen(built);
                if (bl >= rem) break;
                memcpy(o, built, bl);
                o += bl;
                rem -= bl;
                p = end;
                continue;
            }
        }
        *o++ = *p++;
        rem--;
    }
    *o = 0;
    if (strlen(out) + 1 <= cap) snprintf(expr, cap, "%s", out);
}

static int shadow_arena_is_scratch(const char* arena) {
    return arena && strncmp(arena, "@scratch", 8) == 0 &&
           (arena[8] == '\0' || arena[8] == '(' || arena[8] == ' ' ||
            arena[8] == '\t');
}

/* Param piece → (ty, name). Shared by bind + async frame emit. */
typedef struct {
    char ty[160];
    char name[64];
} ShadowParam;

/* Peel a declaration-style `= <literal>` default from a single param piece.
 * Leaves call-style `x = 1` alone (no type on the left). */
static void shadow_peel_param_default(char* piece, size_t* n_io) {
    size_t n, i, depth, eq;
    char prev, next;
    if (!piece || !n_io) return;
    n = *n_io;
    depth = 0;
    eq = n;
    for (i = 0; i < n; i++) {
        char c = piece[i];
        if (c == '(' || c == '[' || c == '{') depth++;
        else if (c == ')' || c == ']' || c == '}') {
            if (depth) depth--;
        } else if (c == '=' && depth == 0) {
            prev = i > 0 ? piece[i - 1] : 0;
            next = i + 1 < n ? piece[i + 1] : 0;
            if (next != '=' && prev != '<' && prev != '>' && prev != '!' &&
                prev != '=' && prev != '+' && prev != '-' && prev != '*' &&
                prev != '/' && prev != '%' && prev != '&' && prev != '|' &&
                prev != '^') {
                eq = i;
                break;
            }
        }
    }
    if (eq >= n) return;
    /* Need a typed declarator on the left (at least one space before the name). */
    {
        size_t le = eq;
        int saw_space = 0, saw_ident = 0;
        while (le > 0 && (piece[le - 1] == ' ' || piece[le - 1] == '\t')) le--;
        if (le == 0) return;
        for (i = 0; i < le; i++) {
            if (piece[i] == ' ' || piece[i] == '\t') saw_space = 1;
            else if ((piece[i] >= 'A' && piece[i] <= 'Z') ||
                     (piece[i] >= 'a' && piece[i] <= 'z') || piece[i] == '_')
                saw_ident = 1;
        }
        if (!saw_space || !saw_ident) return;
    }
    while (eq > 0 && (piece[eq - 1] == ' ' || piece[eq - 1] == '\t')) eq--;
    piece[eq] = 0;
    *n_io = eq;
}

static int shadow_parse_params(const char* params, ShadowParam* out, int cap) {
    const char* p = params;
    int nout = 0;
    if (!p || !p[0] || !out || cap <= 0) return 0;
    while (*p && nout < cap) {
        char piece[192];
        const char* comma;
        const char* s;
        const char* e;
        size_t n, ti, ni;
        int depth = 0;
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        /* Split on ',' only at depth 0 — `Map::[K, V]` keeps its comma. */
        comma = NULL;
        for (s = p; *s; s++) {
            if (*s == '(' || *s == '[' || *s == '{') depth++;
            else if (*s == ')' || *s == ']' || *s == '}') {
                if (depth > 0) depth--;
            } else if (*s == ',' && depth == 0) {
                comma = s;
                break;
            }
        }
        n = comma ? (size_t)(comma - p) : strlen(p);
        if (n >= sizeof(piece)) n = sizeof(piece) - 1;
        memcpy(piece, p, n);
        piece[n] = 0;
        while (n > 0 && (piece[n - 1] == ' ' || piece[n - 1] == '\t'))
            piece[--n] = 0;
        shadow_peel_param_default(piece, &n);
        if (!n || strcmp(piece, "void") == 0) {
            if (!comma) break;
            p = comma + 1;
            continue;
        }
        e = piece + n;
        while (e > piece &&
               ((e[-1] >= 'A' && e[-1] <= 'Z') || (e[-1] >= 'a' && e[-1] <= 'z') ||
                (e[-1] >= '0' && e[-1] <= '9') || e[-1] == '_'))
            e--;
        s = e;
        while (s > piece && (s[-1] == ' ' || s[-1] == '\t' || s[-1] == '*')) s--;
        ni = 0;
        while (*e && ni + 1 < sizeof(out[nout].name))
            out[nout].name[ni++] = *e++;
        out[nout].name[ni] = 0;
        ti = 0;
        {
            const char* q = piece;
            while (q < s &&
                   (*q == ' ' || *q == '\t' || *q == '\n' || *q == '\r'))
                q++;
            for (; q < s && ti + 1 < sizeof(out[nout].ty); q++)
                out[nout].ty[ti++] = *q;
        }
        while (ti > 0 &&
               (out[nout].ty[ti - 1] == ' ' || out[nout].ty[ti - 1] == '\t' ||
                out[nout].ty[ti - 1] == '\n' || out[nout].ty[ti - 1] == '\r'))
            ti--;
        {
            const char* star = s;
            while (star < e && (*star == ' ' || *star == '\t')) star++;
            while (star < e && *star == '*' && ti + 1 < sizeof(out[nout].ty))
                out[nout].ty[ti++] = *star++;
        }
        out[nout].ty[ti] = 0;
        if (out[nout].name[0] && out[nout].ty[0]) nout++;
        if (!comma) break;
        p = comma + 1;
    }
    return nout;
}

/* `Elem[~N [ordered] [sched] >|<]` / `Elem*[~…>]` → CCChanTx / CCChanRx. */
static void shadow_normalize_chan_param_ty(char* ty, size_t cap) {
    char* br;
    char* end;
    char dir = 0;
    if (!ty || !cap) return;
    br = strstr(ty, "[~");
    if (!br) return;
    end = strchr(br, ']');
    if (!end) return;
    {
        const char* q = end;
        while (q > br) {
            q--;
            if (*q == '>' || *q == '<') {
                dir = *q;
                break;
            }
            if (*q != ' ' && *q != '\t' &&
                !((*q >= 'a' && *q <= 'z') || (*q >= 'A' && *q <= 'Z') ||
                  (*q >= '0' && *q <= '9') || *q == '_'))
                break;
        }
    }
    if (dir == '>')
        snprintf(ty, cap, "CCChanTx");
    else if (dir == '<')
        snprintf(ty, cap, "CCChanRx");
}

/* `Ok !>(Err)` → `CCResult_Ok_Err` (param / local type sugar). */
static void shadow_normalize_result_param_ty(char* ty, size_t cap) {
    char* bang;
    char ok[128], err[128], rname[256], rest[128];
    size_t ol, el, rl;
    const char* p;
    int depth;
    if (!ty || !cap) return;
    bang = strstr(ty, "!>(");
    if (!bang) return;
    ol = (size_t)(bang - ty);
    if (ol == 0 || ol >= sizeof(ok)) return;
    memcpy(ok, ty, ol);
    ok[ol] = 0;
    while (ol && (ok[ol - 1] == ' ' || ok[ol - 1] == '\t')) ok[--ol] = 0;
    if (!ol) return;
    p = bang + 3;
    depth = 1;
    el = 0;
    err[0] = 0;
    while (*p && depth > 0) {
        if (*p == '(') depth++;
        else if (*p == ')') {
            depth--;
            if (depth == 0) {
                p++;
                break;
            }
        }
        if (depth > 0) {
            if (el + 1 >= sizeof(err)) return;
            err[el++] = *p;
        }
        p++;
    }
    err[el] = 0;
    while (el && (err[el - 1] == ' ' || err[el - 1] == '\t')) err[--el] = 0;
    if (!el || depth != 0) return;
    rl = 0;
    while (*p && rl + 1 < sizeof(rest)) rest[rl++] = *p++;
    rest[rl] = 0;
    shadow_result_name(ok, err, rname, sizeof(rname));
    snprintf(ty, cap, "%s%s", rname, rest);
}

/* Rewrite channel / Result sugar inside a param list string (fn signatures).
 * Also rebuilds when `=` is present so declaration-style defaults strip for C. */
static void shadow_normalize_chan_params(char* params, size_t cap) {
    ShadowParam ps[16];
    int np;
    int i;
    size_t n;
    char buf[512];
    if (!params || !cap) return;
    if (!strstr(params, "[~") && !strstr(params, "!>") && !strchr(params, '='))
        return;
    np = shadow_parse_params(params, ps, 16);
    if (np <= 0) return;
    buf[0] = 0;
    n = 0;
    for (i = 0; i < np; i++) {
        shadow_normalize_chan_param_ty(ps[i].ty, sizeof(ps[i].ty));
        shadow_normalize_result_param_ty(ps[i].ty, sizeof(ps[i].ty));
        if (i) {
            if (n + 2 >= sizeof(buf)) return;
            buf[n++] = ',';
            buf[n++] = ' ';
            buf[n] = 0;
        }
        {
            int wr = snprintf(buf + n, sizeof(buf) - n, "%s %s", ps[i].ty,
                              ps[i].name);
            if (wr < 0 || (size_t)wr >= sizeof(buf) - n) return;
            n += (size_t)wr;
        }
    }
    if (n + 1 > cap) return;
    memcpy(params, buf, n + 1);
}

/* Bind `Type name` / `Type* name` pieces from a parameter list. */
static void shadow_bind_param_list(const char* params) {
    ShadowParam ps[32];
    char buf[512];
    char* p;
    char* o;
    size_t rem;
    int np;
    int i;
    if (!params) return;
    /* `char[:]` → CCSlice before Map:: rewrite (compact uses CCSlice). */
    buf[0] = 0;
    p = (char*)params;
    o = buf;
    rem = sizeof(buf) - 1;
    while (*p && rem > 0) {
        if (strncmp(p, "char[:]", 7) == 0) {
            int n = snprintf(o, rem, "CCSlice");
            if (n < 0 || (size_t)n >= rem) break;
            o += n;
            rem -= (size_t)n;
            p += 7;
            continue;
        }
        *o++ = *p++;
        rem--;
    }
    *o = 0;
    shadow_rewrite_generic_types_text(buf, sizeof(buf));
    np = shadow_parse_params(buf, ps, 32);
    for (i = 0; i < np; i++) {
        int flags = 0;
        shadow_normalize_chan_param_ty(ps[i].ty, sizeof(ps[i].ty));
        shadow_normalize_result_param_ty(ps[i].ty, sizeof(ps[i].ty));
        if (strstr(ps[i].ty, "cc_atomic_int") || strstr(ps[i].ty, "atomic"))
            flags |= SHADOW_BIND_ATOMIC;
        shadow_bind_name(ps[i].name, ps[i].ty, flags);
        shadow_cap_bind_name(ps[i].name, ps[i].ty, flags);
    }
}

/* Scan text for Map_/CCVec_/Family_* emit spellings and register. */
static void shadow_register_from_text(const char* s);

/* Strip container/factory method suffixes so `CCVec_int_init` → elem `int`. */
static size_t shadow_strip_method_suffix(char* compact, size_t n) {
    static const char* suf[] = {
        "_insert_entry", "_key_logical_live_bytes", "_as_slice", "_get_ptr",
        "_nfields", "_make", "_init", "_push", "_pop", "_get", "_set", "_len",
        "_new", "_free", "_insert", "_delete", "_remove", "_del", "_clear",
        "_destroy", "_put", "_head", "_tail", "_marker", "_is_int", NULL};
    int changed = 1;
    while (changed && n > 0) {
        int i;
        changed = 0;
        for (i = 0; suf[i]; i++) {
            size_t sl = strlen(suf[i]);
            if (n > sl && strcmp(compact + n - sl, suf[i]) == 0) {
                n -= sl;
                compact[n] = 0;
                changed = 1;
                break;
            }
        }
    }
    return n;
}

/* Register Map_/CCVec_/Family_* from parse-time emit spellings (no tape prescan). */
static void shadow_register_mangled_ty(const char* ty) {
    const char* p = ty;
    char compact[96];
    size_t n;
    int fi;
    if (!p) return;
    while (*p == ' ' || *p == '\t') p++;
    if (strncmp(p, "const ", 6) == 0) p += 6;
    while (*p == ' ' || *p == '\t') p++;
    if (strncmp(p, "ArrayMap_", 9) == 0) {
        p += 9;
        n = 0;
        while (*p && *p != '*' && *p != ' ' && *p != '(' && *p != ')' &&
               *p != ',' && *p != ';' && n + 1 < sizeof(compact))
            compact[n++] = *p++;
        compact[n] = 0;
        n = shadow_strip_method_suffix(compact, n);
        if (compact[0]) shadow_amap_need(compact);
        return;
    }
    if (strncmp(p, "array_map_new_count_", 20) == 0 ||
        strncmp(p, "array_map_new_", 14) == 0) {
        p += (strncmp(p, "array_map_new_count_", 20) == 0) ? 20 : 14;
        n = 0;
        while (*p && *p != '*' && *p != ' ' && *p != '(' && n + 1 < sizeof(compact))
            compact[n++] = *p++;
        compact[n] = 0;
        if (compact[0]) shadow_amap_need(compact);
        return;
    }
    if (strncmp(p, "Map_", 4) == 0) {
        p += 4;
        n = 0;
        while (*p && *p != '*' && *p != ' ' && *p != '(' && *p != ')' &&
               *p != ',' && *p != ';' && n + 1 < sizeof(compact))
            compact[n++] = *p++;
        compact[n] = 0;
        n = shadow_strip_method_suffix(compact, n);
        if (compact[0]) shadow_map_need(compact);
        return;
    }
    if (strncmp(p, "CCVec_", 6) == 0) {
        p += 6;
        n = 0;
        while (*p && *p != '*' && *p != ' ' && *p != '(' && *p != ')' &&
               *p != ',' && *p != ';' && n + 1 < sizeof(compact))
            compact[n++] = *p++;
        compact[n] = 0;
        n = shadow_strip_method_suffix(compact, n);
        if (compact[0]) shadow_vec_need(compact);
        return;
    }
    for (fi = 0; fi < g_shadow_ngfac; fi++) {
        const char* fam = g_shadow_gfac[fi].family;
        size_t fl = strlen(fam);
        if (strncmp(p, fam, fl) == 0 && p[fl] == '_') {
            const char* rest = p + fl + 1;
            n = 0;
            while (rest[n] && rest[n] != '*' && rest[n] != ' ' &&
                   rest[n] != '(' && n + 1 < sizeof(compact)) {
                compact[n] = rest[n];
                n++;
            }
            compact[n] = 0;
            n = shadow_strip_method_suffix(compact, n);
            if (compact[0]) shadow_ginst_need(fam, compact);
            return;
        }
    }
    /* Compiled factories registered via comptime cc_generic_register. */
    {
        char fams[512];
        int nf = cc_emit_plan_generic_factory_names_csv(fams, sizeof(fams));
        const char* s;
        const char* best = NULL;
        size_t best_fl = 0;
        if (nf <= 0) return;
        for (s = fams; *s;) {
            const char* e = s;
            size_t fl;
            while (*e && *e != ',') e++;
            fl = (size_t)(e - s);
            while (fl && (s[fl - 1] == ' ' || s[fl - 1] == '\t')) fl--;
            while (fl && (*s == ' ' || *s == '\t')) {
                s++;
                fl--;
            }
            if (fl && strncmp(p, s, fl) == 0 && p[fl] == '_' && fl > best_fl) {
                best = s;
                best_fl = fl;
            }
            if (*e == ',') {
                s = e + 1;
                while (*s == ' ') s++;
            } else
                break;
        }
        if (best && best_fl) {
            char fam[64];
            const char* rest;
            if (best_fl >= sizeof(fam)) best_fl = sizeof(fam) - 1;
            memcpy(fam, best, best_fl);
            fam[best_fl] = 0;
            rest = p + best_fl + 1;
            n = 0;
            while (rest[n] && rest[n] != '*' && rest[n] != ' ' &&
                   rest[n] != '(' && n + 1 < sizeof(compact)) {
                compact[n] = rest[n];
                n++;
            }
            compact[n] = 0;
            n = shadow_strip_method_suffix(compact, n);
            if (compact[0]) shadow_ginst_need(fam, compact);
        }
    }
}

static void shadow_register_from_text(const char* s) {
    int fi;
    const char* h;
    char buf[2048];
    char* w;
    /* Strip "…" / '…' so mangled names inside strcmp("Pair_…") are not
     * mistaken for live instantiations (corrupts factory emit). */
    if (!s || !s[0]) return;
    w = buf;
    h = s;
    while (*h && (size_t)(w - buf) + 1 < sizeof(buf)) {
        if (*h == '"' || *h == '\'') {
            char q = *h++;
            while (*h) {
                if (*h == '\\' && h[1]) {
                    h += 2;
                    continue;
                }
                if (*h == q) {
                    h++;
                    break;
                }
                h++;
            }
            continue;
        }
        *w++ = *h++;
    }
    *w = 0;
    s = buf;
    shadow_register_mangled_ty(s);
    h = s;
    while ((h = strstr(h, "ArrayMap_")) != NULL) {
        shadow_register_mangled_ty(h);
        h += 9;
    }
    h = s;
    while ((h = strstr(h, "array_map_new")) != NULL) {
        shadow_register_mangled_ty(h);
        h += 13;
    }
    h = s;
    while ((h = strstr(h, "Map_")) != NULL) {
        /* Do not treat the `Map_` inside `ArrayMap_` as a Map:: instance. */
        if (h >= s + 5 && memcmp(h - 5, "Array", 5) == 0) {
            h += 4;
            continue;
        }
        if (h > s) {
            char prev = h[-1];
            if ((prev >= 'A' && prev <= 'Z') || (prev >= 'a' && prev <= 'z') ||
                (prev >= '0' && prev <= '9') || prev == '_') {
                h += 4;
                continue;
            }
        }
        shadow_register_mangled_ty(h);
        h += 4;
    }
    h = s;
    while ((h = strstr(h, "CCVec_")) != NULL) {
        shadow_register_mangled_ty(h);
        h += 6;
    }
    for (fi = 0; fi < g_shadow_ngfac; fi++) {
        char pat[80];
        snprintf(pat, sizeof(pat), "%s_", g_shadow_gfac[fi].family);
        h = s;
        while ((h = strstr(h, pat)) != NULL) {
            shadow_register_mangled_ty(h);
            h += strlen(pat);
        }
    }
    /* Surface `family::[Args]` — produce mangled `family_Args` instances. */
    h = s;
    while ((h = strstr(h, "::[")) != NULL) {
        const char* name = h;
        char fam[96];
        char compact[96];
        size_t fl = 0, ci = 0;
        const char* br;
        const char* r;
        int depth = 1;
        while (name > s &&
               ((name[-1] >= 'A' && name[-1] <= 'Z') ||
                (name[-1] >= 'a' && name[-1] <= 'z') ||
                (name[-1] >= '0' && name[-1] <= '9') || name[-1] == '_'))
            name--;
        fl = (size_t)(h - name);
        if (!fl || fl >= sizeof(fam)) {
            h += 3;
            continue;
        }
        memcpy(fam, name, fl);
        fam[fl] = 0;
        br = h + 2;
        r = br + 1;
        while (*r && depth > 0) {
            if (*r == '[') depth++;
            else if (*r == ']') {
                depth--;
                if (depth == 0) break;
            }
            r++;
        }
        if (depth != 0 || *r != ']') {
            h += 3;
            continue;
        }
        {
            const char* a = br + 1;
            while (a < r && ci + 1 < sizeof(compact)) {
                if (*a == ' ' || *a == '\t') {
                    a++;
                    continue;
                }
                if (*a == ',') {
                    if (ci && compact[ci - 1] != '_') compact[ci++] = '_';
                    a++;
                    continue;
                }
                compact[ci++] = *a++;
            }
            compact[ci] = 0;
        }
        /* Member spelling `py.expose::[T]` scans as family `expose` — resolve
         * to registered factory `py_expose` (suffix `_expose`). */
        if (compact[0] && !cc_emit_plan_has_generic_factory(fam) &&
            fam[0] >= 'a' && fam[0] <= 'z') {
            char names[512];
            if (cc_emit_plan_generic_factory_names_csv(names, sizeof(names)) >
                0) {
                char* tok = names;
                size_t fl = strlen(fam);
                while (tok && *tok) {
                    char* comma = strchr(tok, ',');
                    size_t nl;
                    if (comma) *comma = 0;
                    while (*tok == ' ') tok++;
                    nl = strlen(tok);
                    if (nl > fl + 1 && tok[nl - fl - 1] == '_' &&
                        strcmp(tok + nl - fl, fam) == 0) {
                        snprintf(fam, sizeof(fam), "%s", tok);
                        break;
                    }
                    tok = comma ? comma + 1 : NULL;
                }
            }
        }
        if (compact[0] &&
            (cc_emit_plan_has_generic_factory(fam) ||
             (fam[0] >= 'A' && fam[0] <= 'Z') ||
             strncmp(fam, "Map", 3) == 0 || strncmp(fam, "CCVec", 5) == 0 ||
             strncmp(fam, "ArrayMap", 8) == 0 || strncmp(fam, "Vec", 3) == 0 ||
             (strlen(fam) > 5 && strcmp(fam + strlen(fam) - 5, "_make") == 0))) {
            char mangled[160];
            snprintf(mangled, sizeof(mangled), "%s_%s", fam, compact);
            shadow_register_mangled_ty(mangled);
            if (cc_emit_plan_has_generic_factory(fam) ||
                (strlen(fam) > 5 &&
                 strcmp(fam + strlen(fam) - 5, "_make") == 0))
                shadow_ginst_need(fam, compact);
        }
        h = r + 1;
    }
}

/* 1 = recurse into fn bodies for binds (emit-time). 0 = TU prepass: register
 * types inside fns but do not keep param/local name→type entries. */
static int g_shadow_collect_fn_locals;

/* Walk AST: fill name→type map; register generics from structured spellings. */
static void shadow_collect_binds_node(AstNode* st) {
    int k;
    if (!st) return;
    /* Register generic typedef aliases before var binds that use them. */
    if (st->kind == AST_TYPEDEF_INT && st->b[0] &&
        (strncmp(st->b, "ArrayMap_", 9) == 0 || strncmp(st->b, "Map_", 4) == 0 ||
         strncmp(st->b, "CCVec_", 6) == 0)) {
        shadow_register_mangled_ty(st->b);
        shadow_td_alias_register(st->a, st->b);
    }
    if (st->kind == AST_TYPED_INIT || st->kind == AST_VAL_DESTROY ||
        st->kind == AST_VAR_DECL) {
        char bind_ty[168];
        int flags = 0;
        shadow_register_from_text(st->a);
        if (st->kind != AST_VAR_DECL) shadow_register_from_text(st->c);
        if (st->d[0])
            snprintf(bind_ty, sizeof(bind_ty), "%s%s", st->a, st->d);
        else
            snprintf(bind_ty, sizeof(bind_ty), "%s", st->a);
        if (st->kind == AST_TYPED_INIT && st->e[0] == '[')
            flags |= SHADOW_BIND_ARRAY;
        if (strstr(bind_ty, "cc_atomic_int") || strstr(bind_ty, "atomic"))
            flags |= SHADOW_BIND_ATOMIC;
        if (strncmp(bind_ty, "CCSlice_", 8) == 0)
            shadow_slice_need(bind_ty + 8);
        shadow_bind_name(st->b, bind_ty, flags);
    } else if (st->kind == AST_RAW_LINE) {
        shadow_slice_note_raw(st->a);
    } else if (st->kind == AST_TYPEDEF_STRUCT || st->kind == AST_STRUCT) {
        int i;
        const char* outer_alias = (st->kind == AST_TYPEDEF_STRUCT && st->b[0])
                                      ? st->b
                                      : NULL;
        const char* outer_tag = st->a[0] ? st->a : NULL;
        for (i = 0; i < st->nkids; i++) {
            AstNode* f = st->kids[i];
            char fname[64];
            char* comma;
            char target[64];
            if (!f) continue;
            if (f->kind == AST_FIELD_INT) {
                if (outer_alias && f->a[0]) shadow_field_register(outer_alias, f->a);
                if (outer_tag && f->a[0]) shadow_field_register(outer_tag, f->a);
                continue;
            }
            if (f->kind == AST_CHAN_VAR) {
                const char* cty = shadow_chan_handle_ty(f);
                if (outer_alias && f->a[0])
                    shadow_field_register_ex(outer_alias, f->a, cty);
                if (outer_tag && f->a[0])
                    shadow_field_register_ex(outer_tag, f->a, cty);
                continue;
            }
            if (f->kind != AST_FIELD_SIMPLE) continue;
            if (f->e[0] && strcmp(f->e, "raw") == 0) {
                if (outer_alias && f->a[0])
                    shadow_field_register_raw(outer_alias, f->a);
                if (outer_tag && f->a[0])
                    shadow_field_register_raw(outer_tag, f->a);
                continue;
            }
            if (outer_alias && f->b[0])
                shadow_field_register_ex(outer_alias, f->b, f->a[0] ? f->a : NULL);
            if (outer_tag && f->b[0])
                shadow_field_register_ex(outer_tag, f->b, f->a[0] ? f->a : NULL);
            if (!f->e[0] || strcmp(f->e, "as") != 0) continue;
            snprintf(fname, sizeof(fname), "%s", f->b);
            comma = strchr(fname, ',');
            if (comma) *comma = 0;
            shadow_ty_base_stars(f->a, target, sizeof(target), NULL);
            if (fname[0] && target[0]) {
                if (outer_alias) shadow_as_register(outer_alias, fname, target);
                if (outer_tag) shadow_as_register(outer_tag, fname, target);
            }
        }
    } else if (st->kind == AST_FIELD_SIMPLE) {
        shadow_register_from_text(st->a);
    } else if (st->kind == AST_ASSIGN) {
        shadow_register_from_text(st->b);
    } else if (st->kind == AST_RESULT_LOCAL) {
        char rname[256];
        shadow_result_name(st->c, st->b, rname, sizeof(rname));
        shadow_bind_name(st->a, rname, 0);
    } else if (st->kind == AST_CHAN_VAR) {
        /* ">" / "<" / "owned" / bare — same as shadow_chan_handle_ty. */
        const char* ty = shadow_chan_handle_ty(st);
        if (ty) {
            const char* p = st->a;
            char name[64];
            size_t ni;
            while (*p) {
                while (*p == ' ' || *p == '\t' || *p == ',') p++;
                if (!*p) break;
                ni = 0;
                while (*p && *p != ',' && *p != ' ' && *p != '\t' &&
                       ni + 1 < sizeof(name))
                    name[ni++] = *p++;
                name[ni] = 0;
                if (name[0]) shadow_bind_name(name, ty, 0);
            }
        }
    } else if (st->kind == AST_TYPEDEF_CHAN) {
        shadow_chan_register_node(st);
    } else if (st->kind == AST_PTR_INIT || st->kind == AST_PTR_UNWRAP) {
        char ty[160];
        shadow_register_from_text(st->a);
        if (st->kind == AST_PTR_INIT) shadow_register_from_text(st->c);
        snprintf(ty, sizeof(ty), "%s*", st->a);
        shadow_bind_name(st->b, ty, 0);
    } else if (st->kind == AST_SLICE_INIT) {
        if (strcmp(st->a, "char") == 0)
            shadow_bind_name(st->b, "CCSlice", 0);
        else {
            shadow_slice_need(st->a);
            shadow_bind_name(st->b, shadow_slice_ty(st->a), 0);
        }
    } else if (st->kind == AST_RESULT_FN || st->kind == AST_STATIC_FN ||
               st->kind == AST_ASYNC_FN || st->kind == AST_FN) {
        const char* src = NULL;
        const char* fname = NULL;
        if (st->kind == AST_RESULT_FN) {
            src = st->d;
            fname = st->a;
        } else if (st->kind == AST_FN) {
            src = st->b;
            fname = st->a;
        } else if (st->kind == AST_STATIC_FN) {
            src = st->c[0] ? st->c : "";
            fname = st->b;
        } else {
            src = st->c[0] ? st->c : "";
            fname = st->b;
        }
        if (src && src[0] && strcmp(src, "void") != 0) {
            shadow_register_from_text(src);
            /* Param name→type binds are emit-scoped (see shadow_push_fn_binds). */
            if (g_shadow_collect_fn_locals) shadow_bind_param_list(src);
        }
        if (fname && fname[0]) {
            ShadowParam ps[8];
            int np = shadow_parse_params(src ? src : "", ps, 8);
            int pi;
            if (np > 0) {
                char first[192];
                if (ps[0].name[0])
                    snprintf(first, sizeof(first), "%s %s", ps[0].ty,
                             ps[0].name);
                else
                    snprintf(first, sizeof(first), "%s", ps[0].ty);
                shadow_ufn_register(fname, first);
            } else
                shadow_ufn_register(fname, "");
            for (pi = 0; pi < np; pi++) {
                char base[64];
                int stars = 0;
                shadow_ty_base_stars(ps[pi].ty, base, sizeof(base), &stars);
                if (base[0])
                    shadow_fnparam_register(fname, pi, base, stars);
            }
        }
        if (!g_shadow_collect_fn_locals) {
            /* TU prepass: still walk the body for type/generic registration,
             * but freeze binds so locals cannot last-wins across functions. */
            int prev = g_shadow_binds_frozen;
            g_shadow_binds_frozen = 1;
            for (k = 0; k < st->nbody; k++) shadow_collect_binds_node(st->body[k]);
            for (k = 0; k < st->ndbody; k++)
                shadow_collect_binds_node(st->dbody[k]);
            if (st->kids) {
                for (k = 0; k < st->nkids; k++)
                    shadow_collect_binds_node(st->kids[k]);
            }
            g_shadow_binds_frozen = prev;
            return;
        }
    } else if (st->kind == AST_VAR_UNWRAP) {
        shadow_register_from_text(st->b);
        if (st->e[0]) {
            shadow_register_from_text(st->e);
            shadow_bind_name(st->a, st->e, 0);
        }
    } else if (st->kind == AST_STMT_UNWRAP) {
        /* `py_expose::[T](…) !>;` — factory instance must be produced. */
        shadow_register_from_text(st->a);
    } else if (st->kind == AST_UFCS_STMT || st->kind == AST_UFCS_EXPR) {
        shadow_register_from_text(st->a);
        shadow_register_from_text(st->c);
        /* Free-name `py_expose::[T]` lands as UFCS with empty meth when… */
        if (st->e[0]) shadow_register_from_text(st->e);
        /* Member `py.expose::[Math]` stores targs in e as `::Math` (no
         * brackets) — synthesize `expose::[Math]` so ginst/produce runs. */
        {
            const char* ta = shadow_ufcs_e_targs(st->e);
            if (ta && ta[0] && st->b[0]) {
                char syn[192];
                snprintf(syn, sizeof(syn), "%s::[%s]", st->b, ta);
                shadow_register_from_text(syn);
            }
        }
    } else if (st->kind == AST_IF || st->kind == AST_WHILE ||
               st->kind == AST_DO_WHILE || st->kind == AST_FOR) {
        /* Free-name `lru_cache_make::[T](…)` may appear only in conditions. */
        shadow_register_from_text(st->a);
    } else if (st->kind == AST_CALL_ARGS || st->kind == AST_EXPR_STMT) {
        shadow_register_from_text(st->a);
        shadow_register_from_text(st->b);
    } else if (st->kind == AST_RETURN_EXPR || st->kind == AST_RETURN_CC) {
        shadow_register_from_text(st->a);
        shadow_register_from_text(st->b);
    }
    for (k = 0; k < st->nbody; k++) shadow_collect_binds_node(st->body[k]);
    for (k = 0; k < st->ndbody; k++) shadow_collect_binds_node(st->dbody[k]);
    if (st->kids) {
        for (k = 0; k < st->nkids; k++) shadow_collect_binds_node(st->kids[k]);
    }
}

static void shadow_collect_binds(AstNode** items, int n) {
    int i;
    for (i = 0; i < n; i++) shadow_collect_binds_node(items[i]);
    shadow_as_resolve_transitive();
}

/* Emit-time: collect this fn's params+locals, return mark for pop. */
static int shadow_push_fn_binds(AstNode* fn) {
    int mark = g_shadow_nbinds;
    g_shadow_collect_fn_locals = 1;
    shadow_collect_binds_node(fn);
    g_shadow_collect_fn_locals = 0;
    return mark;
}

static void shadow_pop_fn_binds(int mark) {
    g_shadow_nbinds = mark;
}

/* Bind decls in a compound body (skips nested AST_BLOCK via collect). */
static int shadow_push_block_binds(AstNode** body, int n) {
    int mark = g_shadow_nbinds;
    int prev = g_shadow_collect_fn_locals;
    int i;
    g_shadow_collect_fn_locals = 1;
    for (i = 0; i < n; i++) shadow_collect_binds_node(body[i]);
    g_shadow_collect_fn_locals = prev;
    return mark;
}

/* Value-capture C type from AST+type bindings (fallback: chan suffix / int). */
static int shadow_args_has_comma(const char* args) {
    int depth = 0;
    for (const char* p = args ? args : ""; *p; p++) {
        if (*p == '(' || *p == '[' || *p == '{') depth++;
        else if (*p == ')' || *p == ']' || *p == '}') depth--;
        else if (*p == ',' && depth == 0) return 1;
    }
    return 0;
}

static int shadow_emit_return_cc(AstNode* st, CEmit* out, ShadowCtx* ctx,
                                 const char* indent) {
    const char* rname = ctx && ctx->rname ? ctx->rname : "CCResult_int_CCError";
    char assign[384];
    char args[288];
    /* Lower UFCS inside ctor args: cc_ok(res.value()) / cc_err(res.error()). */
    if (st->b[0])
        shadow_emit_expr_text(st, st->b, args, sizeof(args), NULL);
    else
        args[0] = 0;
    if (strcmp(st->a, "ok") == 0) {
        if (args[0])
            snprintf(assign, sizeof(assign), "cc_ok_%s(%s)", rname, args);
        else
            snprintf(assign, sizeof(assign), "cc_ok_%s()", rname);
        if (ctx && ctx->defer_cleanup) {
            /* One compound stmt so `if (c) return cc_ok(...);` stays gated. */
            return cemit_fmt(out,
                "%s{ __cc_retval = %s; __cc_ret_set = 1; __cc_ret_err = 0; "
                "goto __cc_cleanup; }\n",
                indent, assign);
        }
        return cemit_fmt(out, "%sreturn %s;\n", indent, assign);
    }
    if (shadow_args_has_comma(args))
        snprintf(assign, sizeof(assign), "cc_err_%s(CC_ERROR(%s))", rname, args);
    else
        snprintf(assign, sizeof(assign), "cc_err_%s(%s)", rname, args);
    if (ctx && ctx->defer_cleanup) {
        return cemit_fmt(out,
            "%s{ __cc_retval = %s; __cc_ret_set = 1; __cc_ret_err = 1; "
            "goto __cc_cleanup; }\n",
            indent, assign);
    }
    return cemit_fmt(out, "%sreturn %s;\n", indent, assign);
}

#include "pp_emit_unwrap.h"
#include "pp_emit_spawn.h"
#include "pp_emit_autoblock.h"

/* Call-arg text: skip UFCS peel when args are plain literals (preserve strings). */
static void shadow_emit_call_args_text(AstNode* st, char* dst, size_t cap) {
    /* Always run expr/UFCS lowering — a format-string `"` in args must not
     * skip `p.head()` / similar receivers in later arguments. */
    shadow_emit_expr_text(st, st->b, dst, cap, NULL);
}

static int shadow_emit_stmt_ctx(AstNode* st, CEmit* out, ShadowCtx* ctx,
                                const char* indent, int use_cleanup) {
    if (!st) return 0;
    /* Markers: next stmt's lead already covers the gap (avoid double blanks). */
    if (st->kind == AST_ERRHANDLER || st->kind == AST_DEFER)
        return 1;
    if (!shadow_emit_preamble(st, out, ctx ? ctx->cache : NULL)) return 0;
    indent = shadow_stmt_indent(st, indent);
    if (ctx && !shadow_emit_line(out, ctx, st, indent)) return 0;
    switch (st->kind) {
    case AST_RETURN_INT:
        /* `@err`→errhandler return inside bang + @async body helper: match
         * legacy poll-escape (task result stays 0). */
        if (ctx && ctx->err_via_bang &&
            (ctx->owner_fn_attrs & SHADOW_FN_ASYNC))
            return cemit_fmt(out, "%sreturn 0;\n", indent);
        if (use_cleanup) {
            /* Braced so unbraced `if (c) return N;` stays a single stmt. */
            return cemit_fmt(out,
                "%s{ __cc_retval = %s; __cc_ret_set = 1; goto __cc_cleanup; }\n",
                indent, st->a);
        }
        if (ctx && ctx->nvdrop_marks > 0 &&
            ctx->vdrop_marks[0] < ctx->nvdrops) {
            char nested[80];
            int from = ctx->vdrop_marks[0];
            /* Brace drops+return so `if (c) return N;` stays one statement. */
            if (!cemit_fmt(out, "%s{\n", indent)) return 0;
            shadow_indent_nest(nested, sizeof(nested), indent, 1);
            if (!shadow_emit_variant_block_drops(out, ctx, nested, from,
                                                 ctx->nvdrops))
                return 0;
            return cemit_fmt(out, "%sreturn %s;\n%s}\n", nested, st->a, indent);
        }
        return cemit_fmt(out, "%sreturn %s;\n", indent, st->a);
    case AST_RETURN_EXPR: {
        char expr[512];
        char bang[640];
        AstNode* ret_cl = shadow_expr_closure_kid(st);
        if (st->d[0] && strcmp(st->d, "ab:skip") == 0)
            return 1;
        if (ret_cl)
            shadow_fmt_closure_make(expr, sizeof(expr), ret_cl);
        else
            shadow_emit_expr_text(st, st->a, expr, sizeof(expr), NULL);
        /* Result-fn bodies: rewrite bare cc_ok/cc_err in ternaries etc. */
        if (ctx && ctx->rname && ctx->rname[0])
            shadow_rewrite_result_ctors(expr, sizeof(expr), ctx->rname);
        /* `return expr !>(bind)? {…}|stmt;` — structured unwrap + return value. */
        if (strcmp(st->e, "bang_block") == 0 || strcmp(st->e, "bang_stmt") == 0) {
            const char* bind = st->d[0] ? st->d : "e";
            char i1[80], i2[80];
            shadow_indent_nest(i1, sizeof(i1), indent, 1);
            shadow_indent_nest(i2, sizeof(i2), indent, 2);
            if (!cemit_fmt(out,
                    "%s{\n"
                    "%s__typeof__(%s) __r = %s;\n"
                    "%sif (!__r.ok) {\n"
                    "%s__typeof__(__r.u.error) %s = __r.u.error;\n",
                    indent, i1, expr, expr, i1, i2, bind))
                return 0;
            if (!shadow_resync_line(out, ctx, i2)) return 0;
            {
                int saved_via = ctx ? ctx->err_via_bang : 0;
                if (ctx) ctx->err_via_bang = 1;
                if (strcmp(st->e, "bang_stmt") == 0) {
                    if (st->nbody > 0 &&
                        !shadow_emit_stmt_ctx(st->body[0], out, ctx, i2, 0)) {
                        if (ctx) ctx->err_via_bang = saved_via;
                        return 0;
                    }
                } else {
                    int k;
                    for (k = 0; k < st->nbody; k++) {
                        if (!shadow_emit_stmt_ctx(st->body[k], out, ctx, i2,
                                                  0)) {
                            if (ctx) ctx->err_via_bang = saved_via;
                            return 0;
                        }
                    }
                }
                if (ctx) ctx->err_via_bang = saved_via;
            }
            if (use_cleanup) {
                return cemit_fmt(out,
                    "%s}\n"
                    "%s{ __cc_retval = (__r).u.value; __cc_ret_set = 1; "
                    "goto __cc_cleanup; }\n"
                    "%s}\n",
                    i1, i1, indent);
            }
            return cemit_fmt(out,
                "%s}\n"
                "%sreturn (__typeof__((__r).u.value))(__r).u.value;\n"
                "%s}\n",
                i1, i1, indent);
        }
        /* `return expr !>;` — bang flag set at parse. */
        if (strcmp(st->e, "bang") == 0) {
            shadow_emit_return_bang_unwrap(bang, sizeof(bang), expr);
            snprintf(expr, sizeof(expr), "%s", bang);
        }
        if (!expr[0]) {
            if (use_cleanup) {
                return cemit_fmt(out,
                    "%s{ __cc_ret_set = 1; goto __cc_cleanup; }\n",
                    indent);
            }
            return cemit_fmt(out, "%sreturn;\n", indent);
        }
        if (ctx && ctx->send_task_ret) {
            return cemit_fmt(out,
                "%s{\n"
                "%s    __typeof__(%s)* __cc_st_r = (__typeof__(%s)*)"
                "cc_task_result_ptr(sizeof(*__cc_st_r));\n"
                "%s    if (!__cc_st_r) return NULL;\n"
                "%s    *__cc_st_r = (%s);\n"
                "%s    return __cc_st_r;\n"
                "%s}\n",
                indent, indent, expr, expr, indent, indent, expr, indent,
                indent);
        }
        if (use_cleanup) {
            return cemit_fmt(out,
                "%s{ __cc_retval = %s; __cc_ret_set = 1; goto __cc_cleanup; }\n",
                indent, expr);
        }
        if (ctx && ctx->nvdrop_marks > 0 &&
            ctx->vdrop_marks[0] < ctx->nvdrops) {
            char nested[80];
            int from = ctx->vdrop_marks[0];
            if (!cemit_fmt(out, "%s{\n", indent)) return 0;
            shadow_indent_nest(nested, sizeof(nested), indent, 1);
            if (!shadow_emit_variant_block_drops(out, ctx, nested, from,
                                                 ctx->nvdrops))
                return 0;
            return cemit_fmt(out, "%sreturn %s;\n%s}\n", nested, expr, indent);
        }
        return cemit_fmt(out, "%sreturn %s;\n", indent, expr);
    }
    case AST_RETURN_CC:
        return shadow_emit_return_cc(st, out, ctx, indent);
    case AST_PRINTLN_BANG:
        if (st->nbody > 0) {
            int is_eprint = st->d[0] == 'e';
            const char* fn = is_eprint ? "cc_eprintln" : "cc_println";
            char call[320];
            snprintf(call, sizeof(call), "%s(%s)", fn, st->a);
            char i1[80], i2[80];
            shadow_indent_nest(i1, sizeof(i1), indent, 1);
            shadow_indent_nest(i2, sizeof(i2), indent, 2);
            if (!cemit_fmt(out,
                    "%s{\n%s__typeof__(%s) __r = %s;\n%sif (__cc_uw_is_err(__r)) {\n",
                    indent, i1, call, call, i1))
                return 0;
            if (!shadow_resync_line(out, ctx, i2)) return 0;
            if (!shadow_emit_ptr_err_at_bind(out, ctx, i2, "e", fn))
                return 0;
            for (int k = 0; k < st->nbody; k++) {
                if (!shadow_emit_stmt_ctx(st->body[k], out, ctx, i2, use_cleanup))
                    return 0;
            }
            return cemit_fmt(out, "%s}\n%s}\n", i1, indent);
        }
        return shadow_emit_println(st, out, ctx, indent);
    case AST_PRINTLN_TPL:
        if (st->nbody > 0) {
            int is_eprint = st->d[0] == 'e';
            const char* fn = is_eprint ? "cc_eprintln" : "cc_println";
            char nested[80], i2[80];
            shadow_indent_nest(nested, sizeof(nested), indent, 1);
            shadow_indent_nest(i2, sizeof(i2), indent, 2);
            if (!cemit_fmt(out, "%s{\n", indent)) return 0;
            {
                if (!shadow_emit_tpl_build(out, st->a, "&__cc_str_scratch", nested,
                                           "__msg"))
                    return 0;
            }
            char call[320];
            snprintf(call, sizeof(call), "%s(__msg)", fn);
            if (!cemit_fmt(out,
                    "%s{\n%s__typeof__(%s) __r = %s;\n%sif (__cc_uw_is_err(__r)) {\n",
                    nested, i2, call, call, i2))
                return 0;
            char i3[80];
            shadow_indent_nest(i3, sizeof(i3), indent, 3);
            if (!shadow_resync_line(out, ctx, i3)) return 0;
            if (!shadow_emit_ptr_err_at_bind(out, ctx, i3, "e", fn))
                return 0;
            for (int k = 0; k < st->nbody; k++) {
                if (!shadow_emit_stmt_ctx(st->body[k], out, ctx, i3, use_cleanup))
                    return 0;
            }
            if (!cemit_fmt(out, "%s}\n%s}\n", i2, nested)) return 0;
            return cemit_fmt(out, "%s}\n", indent);
        }
        return shadow_emit_println_tpl(st, out, ctx, indent);
    case AST_PRINTLN_BANG_BIND:
        return shadow_emit_bang_bind(st, out, ctx, indent);
    case AST_ERR_FWD:
        if (strcmp(st->c, "delegate") == 0)
            return shadow_emit_err_delegate(out, ctx, st->a, indent);
        return shadow_emit_handler(out, ctx, st->a, indent);
    case AST_ERR_SYNTAX:
        return shadow_emit_err_syntax(st, out, ctx, indent);
    case AST_SWITCH: {
        /* Body can exceed cemit_fmt's 512 temp — emit in pieces. */
        char expr[288];
        char body[8192];
        ShadowVariant* v = NULL;
        int have_desig = (st->d[0] && strstr(st->d, "case .") != NULL);
        /* Structured body (case labels + stmts) — preferred for !> unwrap. */
        if (st->nbody > 0 && !st->d[0]) {
            char nested[80];
            shadow_emit_expr_text(st, st->a, expr, sizeof(expr), NULL);
            if (!cemit_fmt(out, "%sswitch (%s) {\n", indent, expr)) return 0;
            shadow_indent_nest(nested, sizeof(nested), indent, 1);
            for (int k = 0; k < st->nbody; k++) {
                AstNode* kid = st->body[k];
                if (!kid) continue;
                if (kid->kind == AST_RAW_LINE) {
                    if (!cemit_fmt(out, "%s%s\n", nested, kid->a)) return 0;
                    continue;
                }
                if (!shadow_emit_stmt_ctx(kid, out, ctx, nested, use_cleanup))
                    return 0;
            }
            return cemit_fmt(out, "%s}\n", indent);
        }
        shadow_emit_expr_text(st, st->a, expr, sizeof(expr), NULL);
        /* Subject-switch: resolve via bind — never assume the sole @variant. */
        if (g_shadow_nvariants && have_desig) {
            char subj[128];
            const char* s = expr;
            size_t n = 0;
            while (*s == '(' || *s == ' ' || *s == '\t') s++;
            while (s[n] && shadow_is_id(s[n]) && n + 1 < sizeof(subj)) n++;
            memcpy(subj, s, n);
            subj[n] = 0;
            v = shadow_variant_for_recv(subj);
            if (shadow_variant_diag_switch(ctx, st, out, expr, st->d, v))
                return 0;
            {
                int ptr = 0;
                const ShadowBind* b = subj[0] ? shadow_bind_lookup(subj) : NULL;
                if (b && b->ty[0] && strchr(b->ty, '*')) ptr = 1;
                if (!ptr && strstr(st->d, "->") != NULL) ptr = 1;
                if (v) {
                    char kinded[320];
                    if (v->is_packed) {
                        if (ptr)
                            snprintf(kinded, sizeof(kinded), "%s__cc_kind(%s)",
                                     v->name, expr);
                        else
                            snprintf(kinded, sizeof(kinded),
                                     "%s__cc_kind(&(%s))", v->name, expr);
                    } else if (ptr)
                        snprintf(kinded, sizeof(kinded), "(%s)->kind", expr);
                    else
                        snprintf(kinded, sizeof(kinded), "(%s).kind", expr);
                    snprintf(expr, sizeof(expr), "%s", kinded);
                }
            }
        }
        if (!cemit_fmt(out, "%sswitch (%s) {\n", indent, expr)) return 0;
        if (st->d[0]) {
            char tmp[8192];
            char* p;
            snprintf(body, sizeof(body), "%s", st->d);
            /* Opaque case body: peel leftover UFCS / char slice sugar / @create. */
            while ((p = strstr(body, "char[:0!]")) != NULL) {
                size_t off = (size_t)(p - body);
                snprintf(tmp, sizeof(tmp), "%.*sCCSlice%s", (int)off, body,
                         p + 9);
                snprintf(body, sizeof(body), "%s", tmp);
            }
            while ((p = strstr(body, "char[0:]")) != NULL) {
                size_t off = (size_t)(p - body);
                snprintf(tmp, sizeof(tmp), "%.*sCCSlice%s", (int)off, body,
                         p + 8);
                snprintf(body, sizeof(body), "%s", tmp);
            }
            while ((p = strstr(body, "char[:0]")) != NULL) {
                size_t off = (size_t)(p - body);
                snprintf(tmp, sizeof(tmp), "%.*sCCSlice%s", (int)off, body,
                         p + 8);
                snprintf(body, sizeof(body), "%s", tmp);
            }
            while ((p = strstr(body, "char[:]")) != NULL) {
                size_t off = (size_t)(p - body);
                snprintf(tmp, sizeof(tmp), "%.*sCCSlice%s", (int)off, body,
                         p + 7);
                snprintf(body, sizeof(body), "%s", tmp);
            }
            while ((p = strstr(body, "@create(")) != NULL) {
                const char* args = p + 8;
                const char* q = args;
                int dep = 1;
                const char* dest;
                size_t off = (size_t)(p - body);
                while (*q && dep > 0) {
                    if (*q == '(') dep++;
                    else if (*q == ')') dep--;
                    q++;
                }
                dest = q;
                while (*dest == ' ' || *dest == '\t') dest++;
                if (strncmp(dest, "@destroy", 8) == 0) dest += 8;
                snprintf(tmp, sizeof(tmp), "%.*scc_arena_create(%.*s)%s",
                         (int)off, body, (int)((q - 1) - args), args, dest);
                snprintf(body, sizeof(body), "%s", tmp);
            }
            shadow_rewrite_defer_in_opaque(body, sizeof(body));
            shadow_rewrite_print_and_string(body, sizeof(body));
            shadow_emit_text_ufcs(tmp, sizeof(tmp), body, NULL);
            snprintf(body, sizeof(body), "%s", tmp);
            shadow_rewrite_variant_expr(body, sizeof(body),
                                        v ? v->name : NULL);
            /* Opaque subject-switch bodies never walk AST_STMT_UNWRAP kids —
             * lower `!>` / `?>` in the text so host-C refuse does not fire. */
            if (!shadow_rewrite_bang_exprs(body, sizeof(body), ctx)) return 0;
            if (ctx && ctx->rname && ctx->rname[0])
                shadow_rewrite_result_ctors(body, sizeof(body), ctx->rname);
            if (!cemit_str(out, body)) return 0;
            if (!cemit_str(out, "\n")) return 0;
        }
        return cemit_fmt(out, "%s}\n", indent);
    }
    case AST_IF: {
        char cond[2048];
        if (st->e[0] && strcmp(st->e, "spawn_cond") == 0) {
            AstNode* sp = NULL;
            int k;
            for (k = 0; k < st->ndbody; k++) {
                AstNode* kid = st->dbody[k];
                if (kid && kid->kind == AST_SPAWN_CLOSURE &&
                    (strcmp(kid->b, "spawn") == 0 ||
                     strcmp(kid->b, "spawn_unsafe") == 0 ||
                     strcmp(kid->b, "spawnhybrid") == 0 ||
                     strcmp(kid->b, "spawnhybrid_unsafe") == 0)) {
                    sp = kid;
                    break;
                }
            }
            if (!sp) return 0;
            {
                char args[256];
                const char* nur_spawn =
                    (sp->b[0] && (strcmp(sp->b, "spawnhybrid") == 0 ||
                                  strcmp(sp->b, "spawnhybrid_unsafe") == 0))
                        ? "cc_nursery_spawnhybrid_closure0"
                        : "cc_nursery_spawn_closure0";
                if (!shadow_caps_call_args(sp->e, args, sizeof(args))) return 0;
                if (args[0])
                    snprintf(cond, sizeof(cond),
                             "%s(%s, cc_closure__N%s_make(%s))%s", nur_spawn,
                             sp->a, sp->d[0] ? sp->d : "1", args, st->a);
                else
                    snprintf(cond, sizeof(cond),
                             "%s(%s, cc_closure__N%s_make())%s", nur_spawn,
                             sp->a, sp->d[0] ? sp->d : "1", st->a);
            }
        } else {
            if (!shadow_emit_cond_text(st, st->a, cond, sizeof(cond), ctx, out))
                return 0;
            if (!shadow_rewrite_bang_exprs(cond, sizeof(cond), ctx)) return 0;
        }
        /* Newline before then-stmt so `#line` from the body cannot sit
         * mid-`if (cond) #line N` (invalid C). */
        if (!cemit_fmt(out, "%sif (%s)\n", indent, cond)) return 0;
        if (st->nbody < 1) return 0;
        /* Route blocks through AST_BLOCK so vdrop marks push/pop correctly. */
        if (st->body[0]->kind == AST_BLOCK) {
            if (!shadow_emit_stmt_ctx(st->body[0], out, ctx, indent,
                                      use_cleanup))
                return 0;
        } else {
            char then_ind[80];
            shadow_indent_nest(then_ind, sizeof(then_ind), indent, 1);
            if (!shadow_emit_stmt_ctx(st->body[0], out, ctx, then_ind,
                                      use_cleanup))
                return 0;
        }
        if (st->nbody >= 2) {
            if (!cemit_fmt(out, "%selse\n", indent)) return 0;
            if (st->body[1]->kind == AST_BLOCK || st->body[1]->kind == AST_IF)
                return shadow_emit_stmt_ctx(st->body[1], out, ctx, indent,
                                            use_cleanup);
            {
                char then_ind[80];
                shadow_indent_nest(then_ind, sizeof(then_ind), indent, 1);
                return shadow_emit_stmt_ctx(st->body[1], out, ctx, then_ind,
                                            use_cleanup);
            }
        }
        return 1;
    }
    case AST_BLOCK: {
        unsigned saved_block = ctx ? ctx->block_attrs : 0;
        int bind_mark = shadow_push_block_binds(st->body, st->nbody);
        if (ctx && st->e[0]) {
            if (strcmp(st->e, "noblock") == 0)
                ctx->block_attrs |= SHADOW_FN_NOBLOCK;
            else if (strcmp(st->e, "blocking") == 0)
                ctx->block_attrs |= SHADOW_FN_BLOCKING;
        }
        if (!cemit_fmt(out, "%s{\n", indent)) {
            shadow_pop_fn_binds(bind_mark);
            return 0;
        }
        char nested[80];
        shadow_indent_nest(nested, sizeof(nested), indent, 1);
        AstNode* bdest[32];
        int nbdest = 0;
        int k;
        for (k = 0; k < st->nbody; k++) {
            if (shadow_stmt_is_destroy(st->body[k]) && nbdest < 32)
                bdest[nbdest++] = st->body[k];
        }
        if (ctx) shadow_vdrop_mark_push(ctx);
        if (ctx) {
            if (!shadow_emit_compound_body(st->body, st->nbody, out, ctx, nested,
                                           use_cleanup, 0)) {
                shadow_pop_fn_binds(bind_mark);
                return 0;
            }
        } else {
            for (k = 0; k < st->nbody; k++) {
                if (!shadow_emit_stmt_ctx(st->body[k], out, ctx, nested,
                                          use_cleanup)) {
                    shadow_pop_fn_binds(bind_mark);
                    return 0;
                }
            }
        }
        if (ctx && !shadow_vdrop_mark_pop_emit(out, ctx, nested)) {
            shadow_pop_fn_binds(bind_mark);
            return 0;
        }
        if (ctx && st->e[0]) ctx->block_attrs = saved_block;
        /* Block-scoped @destroy runs at block exit (not fn __cc_cleanup). */
        for (int d = nbdest - 1; d >= 0; d--) {
            if (!shadow_emit_one_destroy(bdest[d], out, ctx, nested)) {
                shadow_pop_fn_binds(bind_mark);
                return 0;
            }
        }
        shadow_pop_fn_binds(bind_mark);
        return cemit_fmt(out, "%s}\n", indent);
    }
    case AST_VAR_UNWRAP:
        return shadow_emit_var_unwrap(st, out, ctx, indent);
    case AST_PTR_INIT: {
        char expr[288];
        if (!shadow_emit_channel_pair_expr(expr, sizeof(expr), st->c))
            shadow_emit_expr_text(st, st->c, expr, sizeof(expr), st->a);
        return cemit_fmt(out, "%s%s* %s = %s;\n", indent, st->a, st->b, expr);
    }
    case AST_STMT_UNWRAP: {
        char call[8192];
        char renamed[8192];
        const char* src = st->a;
        char i1[80], i2[80];
        /* Naked print helpers (also handled on AST_CALL_ARGS). */
        if (strncmp(src, "fprintln(", 9) == 0)
            snprintf(renamed, sizeof(renamed), "cc_fprintln(%s", src + 9);
        else if (strncmp(src, "eprintln(", 9) == 0)
            snprintf(renamed, sizeof(renamed), "cc_eprintln(%s", src + 9);
        else if (strncmp(src, "println(", 8) == 0)
            snprintf(renamed, sizeof(renamed), "cc_println(%s", src + 8);
        else
            snprintf(renamed, sizeof(renamed), "%s", src);
        if (!shadow_emit_channel_pair_expr(call, sizeof(call), renamed))
            shadow_emit_expr_text(st, renamed, call, sizeof(call), NULL);
        shadow_rewrite_print_and_string(call, sizeof(call));
        {
            const char* pat = "__cc_tpl; }).as_slice()";
            const char* repl = "cc_string_as_slice(&__cc_tpl); })";
            char* hit;
            while ((hit = strstr(call, pat)) != NULL) {
                char tmp[8192];
                size_t pre = (size_t)(hit - call);
                if (pre + strlen(repl) + strlen(hit + strlen(pat)) + 1 >=
                    sizeof(tmp))
                    break;
                snprintf(tmp, sizeof(tmp), "%.*s%s%s", (int)pre, call, repl,
                         hit + strlen(pat));
                snprintf(call, sizeof(call), "%s", tmp);
            }
        }
        shadow_rewrite_at_slice(call, sizeof(call));
        shadow_rewrite_generic_types_text(call, sizeof(call));
        if (strcmp(st->c, "bang_block") == 0 ||
            strcmp(st->c, "bang_stmt") == 0) {
            const char* bind = st->d[0] ? st->d : "e";
            /* Bare `!>` / `@destroy` bodies use enclosing @errhandler (null +
             * Result). Explicit `!>(e){…}` keeps field-typed binder so
             * CCIoError stays CCIoError (not __cc_uw_err_at → CCError). */
            int use_eh = (st->nbody == 0) ||
                         (st->nbody > 0 && st->body[0] &&
                          st->body[0]->kind == AST_RAW_LINE &&
                          strstr(st->body[0]->a, "@destroy") != NULL);
            shadow_indent_nest(i1, sizeof(i1), indent, 1);
            shadow_indent_nest(i2, sizeof(i2), indent, 2);
            if (use_eh || !st->d[0]) {
                if (!cemit_fmt(out,
                        "%s{\n"
                        "%s__typeof__(%s) __r = %s;\n"
                        "%sif (__cc_uw_is_err(__r)) {\n",
                        indent, i1, call, call, i1))
                    return 0;
            if (!shadow_resync_line(out, ctx, i2)) return 0;
            if (!shadow_emit_ptr_err_at_bind(out, ctx, i2, bind, "unwrap"))
                return 0;
            {
                int saved_via = ctx ? ctx->err_via_bang : 0;
                if (ctx) ctx->err_via_bang = 1;
                if (use_eh && st->nbody == 0) {
                    if (!shadow_emit_handler(out, ctx, bind, i2)) {
                        if (ctx) ctx->err_via_bang = saved_via;
                        return 0;
                    }
                } else if (strcmp(st->c, "bang_stmt") == 0) {
                    if (st->nbody > 0 &&
                        !shadow_emit_stmt_ctx(st->body[0], out, ctx, i2,
                                              use_cleanup)) {
                        if (ctx) ctx->err_via_bang = saved_via;
                        return 0;
                    }
                } else {
                    for (int k = 0; k < st->nbody; k++) {
                        if (!shadow_emit_stmt_ctx(st->body[k], out, ctx, i2,
                                                  use_cleanup)) {
                            if (ctx) ctx->err_via_bang = saved_via;
                            return 0;
                        }
                    }
                }
                if (ctx) ctx->err_via_bang = saved_via;
            }
                if (st->e[0])
                    return cemit_fmt(out,
                                     "%s}\n%s%s = __cc_uw_value(__r);\n%s}\n",
                                     i1, i1, st->e, indent);
                return cemit_fmt(out, "%s}\n%s}\n", i1, indent);
            }
            /* Field-typed binder keeps CCIoError. */
            if (!cemit_fmt(out,
                    "%s{\n"
                    "%s__typeof__(%s) __r = %s;\n"
                    "%sif (!__r.ok) {\n"
                    "%s__typeof__(__r.u.error) %s = __r.u.error;\n",
                    indent, i1, call, call, i1, i2, bind))
                return 0;
            if (!shadow_resync_line(out, ctx, i2)) return 0;
            {
                int saved_via = ctx ? ctx->err_via_bang : 0;
                if (ctx) ctx->err_via_bang = 1;
                if (strcmp(st->c, "bang_stmt") == 0) {
                    if (st->nbody > 0 &&
                        !shadow_emit_stmt_ctx(st->body[0], out, ctx, i2,
                                              use_cleanup)) {
                        if (ctx) ctx->err_via_bang = saved_via;
                        return 0;
                    }
                } else {
                    for (int k = 0; k < st->nbody; k++) {
                        if (!shadow_emit_stmt_ctx(st->body[k], out, ctx, i2,
                                                  use_cleanup)) {
                            if (ctx) ctx->err_via_bang = saved_via;
                            return 0;
                        }
                    }
                }
                if (ctx) ctx->err_via_bang = saved_via;
            }
            if (st->e[0])
                return cemit_fmt(out, "%s}\n%s%s = __r.u.value;\n%s}\n", i1, i1,
                                 st->e, indent);
            return cemit_fmt(out, "%s}\n%s}\n", i1, indent);
        }
        return shadow_emit_try_call(out, ctx, indent, call, "unwrap", "e", 1);
    }
    case AST_RESULT_LOCAL: {
        char rname[256];
        char init[512];
        AstNode* cl = shadow_expr_closure_kid(st);
        shadow_result_name(st->c, st->b, rname, sizeof(rname));
        if (!st->d[0])
            return cemit_fmt(out, "%s%s %s%s;\n", indent, rname, st->a,
                             st->e[0] ? st->e : "");
        shadow_emit_expr_text(st, st->d, init, sizeof(init), NULL);
        if (cl) shadow_splice_closure_arg(init, sizeof(init), cl);
        shadow_rewrite_result_ctors(init, sizeof(init), rname);
        return cemit_fmt(out, "%s%s %s%s = %s;\n", indent, rname, st->a,
                         st->e[0] ? st->e : "", init);
    }
    case AST_VOID_CAST: {
        char expr[288];
        AstNode* cl = shadow_expr_closure_kid(st);
        shadow_emit_expr_text(st, st->a, expr, sizeof(expr), NULL);
        if (cl) shadow_splice_closure_arg(expr, sizeof(expr), cl);
        return cemit_fmt(out, "%s(void)%s;\n", indent, expr);
    }
    case AST_TYPEDEF_STRUCT:
        return shadow_emit_typedef_struct(st, out, ctx ? ctx->cache : NULL, ctx);
    case AST_FIELD_SIMPLE:
    case AST_FIELD_INT:
        /* Nested typedef struct fields share kids_storage with the enclosing
         * fn body; emit is owned by AST_TYPEDEF_STRUCT. */
        return 1;
    case AST_TYPEDEF_INT:
        if (st->c[0] && st->d[0]) {
            char rname[256];
            shadow_result_name(st->c, st->d, rname, sizeof(rname));
            return cemit_fmt(out, "%stypedef %s %s;\n", indent, rname, st->a);
        }
        if (st->b[0]) {
            /* Generic typedef: register ArrayMap_/Map_ instance + alias. */
            shadow_register_mangled_ty(st->b);
            if (strncmp(st->b, "ArrayMap_", 9) == 0 ||
                strncmp(st->b, "Map_", 4) == 0 ||
                strncmp(st->b, "CCVec_", 6) == 0)
                shadow_td_alias_register(st->a, st->b);
            return cemit_fmt(out, "%stypedef %s %s;\n", indent, st->b, st->a);
        }
        return cemit_fmt(out, "%stypedef int %s;\n", indent, st->a);
    case AST_VAR_DECL: {
        /* Typedef channel alias → register the variable for cc_channel_pair. */
        const ShadowChanDecl* alias = shadow_chan_find(st->a);
        if (alias && st->b[0])
            shadow_chan_register_ex(st->b, alias->cap, alias->ordered,
                                    alias->elem[0] ? alias->elem : NULL,
                                    alias->topo[0] ? alias->topo : NULL,
                                    alias->is_sync, alias->bp_mode);
        if (st->c[0]) {
            char dims[160];
            snprintf(dims, sizeof(dims), "%s", st->c);
            shadow_lower_type_of_constexpr(dims, sizeof(dims));
            return cemit_fmt(out, "%s%s %s%s;\n", indent, st->a, st->b, dims);
        }
        return cemit_fmt(out, "%s%s %s;\n", indent, st->a, st->b);
    }
    case AST_WHILE: {
        char cond[2048];
        if (!shadow_emit_cond_text(st, st->a, cond, sizeof(cond), ctx, out))
            return 0;
        if (!shadow_rewrite_bang_exprs(cond, sizeof(cond), ctx)) return 0;
        if (!cemit_fmt(out, "%swhile (%s) {\n", indent, cond)) return 0;
        char nested[80];
        shadow_indent_nest(nested, sizeof(nested), indent, 1);
        /* Use compound_body so nested `@errhandler` scopes subsequent stmts
         * (same as AST_FOR / AST_BLOCK) — bare stmt walk drops eh. */
        if (ctx) {
            if (!shadow_emit_compound_body(st->body, st->nbody, out, ctx, nested,
                                           use_cleanup, 1))
                return 0;
        } else {
            for (int k = 0; k < st->nbody; k++) {
                if (!shadow_emit_stmt_ctx(st->body[k], out, ctx, nested,
                                          use_cleanup))
                    return 0;
            }
        }
        return cemit_fmt(out, "%s}\n", indent);
    }
    case AST_INC:
        return cemit_fmt(out, "%s%s++;\n", indent, st->a);
    case AST_EXPR_STMT: {
        char expr[2048];
        shadow_emit_expr_text(st, st->a, expr, sizeof(expr), NULL);
        if (!shadow_rewrite_bang_exprs(expr, sizeof(expr), ctx)) return 0;
        /* send_task body: bare expr is the packed task result. */
        if (ctx && ctx->send_task_ret) {
            return cemit_fmt(out,
                "%s{\n"
                "%s    __typeof__(%s)* __cc_st_r = (__typeof__(%s)*)"
                "cc_task_result_ptr(sizeof(*__cc_st_r));\n"
                "%s    if (!__cc_st_r) return NULL;\n"
                "%s    *__cc_st_r = (%s);\n"
                "%s    return __cc_st_r;\n"
                "%s}\n",
                indent, indent, expr, expr, indent, indent, expr, indent,
                indent);
        }
        return cemit_fmt(out, "%s%s;\n", indent, expr);
    }
    case AST_BREAK: {
        int to = shadow_defer_exit_depth(ctx, 1);
        int from = (ctx && ctx->defer_depth > 0) ? ctx->defer_depth - 1 : -1;
        if (to >= 0 && from >= to) {
            char nested[80];
            /* Brace so `if (c) break;` keeps defer+break as one statement. */
            if (!cemit_fmt(out, "%s{\n", indent)) return 0;
            shadow_indent_nest(nested, sizeof(nested), indent, 1);
            if (!shadow_emit_defers_exiting(ctx, out, nested, from, to))
                return 0;
            return cemit_fmt(out, "%sbreak;\n%s}\n", nested, indent);
        }
        return cemit_fmt(out, "%sbreak;\n", indent);
    }
    case AST_CONTINUE: {
        int to = shadow_defer_exit_depth(ctx, 1);
        int from = (ctx && ctx->defer_depth > 0) ? ctx->defer_depth - 1 : -1;
        if (to >= 0 && from >= to) {
            char nested[80];
            /* Brace so `if (c) continue;` keeps defer+continue as one stmt. */
            if (!cemit_fmt(out, "%s{\n", indent)) return 0;
            shadow_indent_nest(nested, sizeof(nested), indent, 1);
            if (!shadow_emit_defers_exiting(ctx, out, nested, from, to))
                return 0;
            return cemit_fmt(out, "%scontinue;\n%s}\n", nested, indent);
        }
        return cemit_fmt(out, "%scontinue;\n", indent);
    }
    case AST_GOTO:
        return cemit_fmt(out, "%sgoto %s;\n", indent, st->a);
    case AST_LABEL:
        /* Labels are not indented (C allows; keeps jumps readable). */
        return cemit_fmt(out, "%s:\n", st->a);
    case AST_WITH_DEADLINE: {
        int id = ctx && ctx->deadline_i ? (*ctx->deadline_i)++ : 0;
        char i1[80];
        shadow_indent_nest(i1, sizeof(i1), indent, 1);
        if (!cemit_fmt(out, "%s{\n", indent)) return 0;
        if (!cemit_fmt(out,
                "%sCCDeadline __cc_dl%d = cc_deadline_after_ms((uint64_t)(%s));\n",
                i1, id, st->a))
            return 0;
        if (st->b[0]) {
            if (!cemit_fmt(out,
                    "%sCCDeadline* %s = &__cc_dl%d;\n"
                    "%sCCDeadline* __cc_prev%d = cc_deadline_push(%s);\n",
                    i1, st->b, id, i1, id, st->b))
                return 0;
        } else {
            if (!cemit_fmt(out,
                    "%sCCDeadline* __cc_prev%d = cc_deadline_push(&__cc_dl%d);\n",
                    i1, id, id))
                return 0;
        }
        for (int k = 0; k < st->nbody; k++) {
            /* Body sits at +1 nest (same as a hand-lowered block body). */
            char saved = st->body[k]->indent[0];
            st->body[k]->indent[0] = 0;
            int ok = shadow_emit_stmt_ctx(st->body[k], out, ctx, i1, use_cleanup);
            st->body[k]->indent[0] = saved;
            if (!ok) return 0;
        }
        return cemit_fmt(out,
                "%scc_deadline_pop(__cc_prev%d);\n"
                "%s}\n",
                i1, id, indent);
    }
    case AST_DO_WHILE: {
        if (!cemit_fmt(out, "%sdo {\n", indent)) return 0;
        char nested[80];
        shadow_indent_nest(nested, sizeof(nested), indent, 1);
        if (ctx) {
            if (!shadow_emit_compound_body(st->body, st->nbody, out, ctx, nested,
                                           use_cleanup, 1))
                return 0;
        } else {
            for (int k = 0; k < st->nbody; k++) {
                if (!shadow_emit_stmt_ctx(st->body[k], out, ctx, nested,
                                          use_cleanup))
                    return 0;
            }
        }
        char cond[2048];
        if (!shadow_emit_cond_text(st, st->a, cond, sizeof(cond), ctx, out))
            return 0;
        if (!shadow_rewrite_bang_exprs(cond, sizeof(cond), ctx)) return 0;
        return cemit_fmt(out, "%s} while (%s);\n", indent, cond);
    }
    case AST_STATIC_ARR:
        return cemit_fmt(out, "%sstatic char %s[] = %s;\n", indent, st->a, st->b);
    case AST_CHAN_VAR:
        shadow_chan_register_node(st);
        if (strcmp(st->c, "owned") == 0) {
            /* T[~N owned { .create/.destroy/.reset = closures }] name; */
            static int owned_id;
            int id = owned_id++;
            char create_m[96] = "{0}";
            char destroy_m[96] = "{0}";
            char reset_m[96] = "{0}";
            const char* elem = st->d[0] ? st->d : "void*";
            const char* cap = st->b[0] ? st->b : "0";
            int k;
            for (k = 0; k < st->ndbody; k++) {
                AstNode* cl = st->dbody[k];
                char make[96];
                const char* field = NULL;
                if (!cl || cl->kind != AST_SPAWN_CLOSURE) continue;
                if (strcmp(cl->b, "create") == 0) field = "create";
                else if (strncmp(cl->c, "owned:", 6) == 0) field = cl->c + 6;
                else if (strcmp(cl->b, "destroy") == 0) field = "destroy";
                else if (strcmp(cl->b, "reset") == 0) field = "reset";
                if (!field) continue;
                shadow_fmt_closure_make(make, sizeof(make), cl);
                if (strcmp(field, "create") == 0)
                    snprintf(create_m, sizeof(create_m), "%s", make);
                else if (strcmp(field, "destroy") == 0)
                    snprintf(destroy_m, sizeof(destroy_m), "%s", make);
                else if (strcmp(field, "reset") == 0)
                    snprintf(reset_m, sizeof(reset_m), "%s", make);
            }
            if (!cemit_fmt(out,
                    "%s/* owned channel %s */\n"
                    "%sCCClosure0 __cc_owned_%d_create = %s;\n"
                    "%sCCClosure1 __cc_owned_%d_destroy = %s;\n"
                    "%sCCClosure1 __cc_owned_%d_reset = %s;\n"
                    "%sCCChan* %s = cc_chan_create_owned(%s, sizeof(%s), "
                    "__cc_owned_%d_create, __cc_owned_%d_destroy, "
                    "__cc_owned_%d_reset);\n",
                    indent, st->a, indent, id, create_m, indent, id, destroy_m,
                    indent, id, reset_m, indent, st->a, cap, elem, id, id, id))
                return 0;
            return 1;
        }
        {
            const char* init = NULL;
            const char* pe = st->e;
            while (pe && (pe = strstr(pe, ";=")) != NULL) {
                init = pe + 2;
                break;
            }
            if (init && init[0])
                return cemit_fmt(out, "%s%s %s = %s;\n", indent,
                                 shadow_chan_handle_ty(st), st->a, init);
            return cemit_fmt(out, "%s%s %s;\n", indent,
                             shadow_chan_handle_ty(st), st->a);
        }
    case AST_TYPEDEF_CHAN:
        shadow_chan_register_node(st);
        if (st->c[0] == '<')
            return cemit_fmt(out, "%stypedef CCChanRx %s;\n", indent, st->a);
        return cemit_fmt(out, "%stypedef CCChanTx %s;\n", indent, st->a);
    case AST_VAL_DESTROY: {
        char init[288];
        char raw[288];
        int is_ptr = (st->d[0] == '*');
        int has_bang = 0;
        AstNode* cl = shadow_expr_closure_kid(st);
        /* Dest type supplies type formals (block_on) and pointer elems (allocT). */
        const char* formal = st->a[0] ? st->a : NULL;
        snprintf(raw, sizeof(raw), "%s", st->c);
        /* Strip trailing `!>` kept in init text by parse_val_destroy. */
        {
            size_t n = strlen(raw);
            while (n > 0 && (raw[n - 1] == ' ' || raw[n - 1] == '\t'))
                raw[--n] = 0;
            if (n >= 2 && raw[n - 2] == '!' && raw[n - 1] == '>') {
                raw[n - 2] = 0;
                has_bang = 1;
                n -= 2;
                while (n > 0 && (raw[n - 1] == ' ' || raw[n - 1] == '\t'))
                    raw[--n] = 0;
            }
        }
        shadow_emit_expr_text(st, raw, init, sizeof(init), formal);
        shadow_resolve_at_create(init, sizeof(init), st->a, is_ptr, cl != NULL);
        if (cl) shadow_splice_closure_arg(init, sizeof(init), cl);
        if (has_bang) {
            /* Result: field unwrap + @errhandler. Plain values (create hook,
             * cc_adopt): identity — bang is vacuous / null-check folklore. */
            char ty_lhs[160];
            const char* chook = shadow_create_hook_for(st->a);
            int plain = 0;
            if (is_ptr)
                snprintf(ty_lhs, sizeof(ty_lhs), "%s*", st->a);
            else
                snprintf(ty_lhs, sizeof(ty_lhs), "%s", st->a);
            if (chook && chook[0] && strstr(init, chook) != NULL) plain = 1;
            if (strstr(init, "cc_adopt(") != NULL) plain = 1;
            if (plain)
                return cemit_fmt(out, "%s%s %s = %s;\n", indent, ty_lhs, st->b,
                                 init);
            return shadow_emit_try_assign(out, ctx, indent, ty_lhs, st->b, init,
                                         "unwrap", "e", "(__r).u.value");
        }
        if (is_ptr)
            return cemit_fmt(out, "%s%s* %s = %s;\n", indent, st->a, st->b, init);
        /* Value @destroy — cleanup via registered destroy hook at scope exit. */
        return cemit_fmt(out, "%s%s %s = %s;\n", indent, st->a, st->b, init);
    }
    case AST_FOR: {
        char header[768];
        snprintf(header, sizeof(header), "%s", st->a);
        shadow_rewrite_variant_expr(header, sizeof(header), NULL);
        if (!cemit_fmt(out, "%sfor (%s) {\n", indent, header)) return 0;
        char nested[80];
        shadow_indent_nest(nested, sizeof(nested), indent, 1);
        if (ctx) {
            if (!shadow_emit_compound_body(st->body, st->nbody, out, ctx, nested,
                                           use_cleanup, 1))
                return 0;
        } else {
            for (int k = 0; k < st->nbody; k++) {
                if (!shadow_emit_stmt_ctx(st->body[k], out, ctx, nested,
                                          use_cleanup))
                    return 0;
            }
        }
        return cemit_fmt(out, "%s}\n", indent);
    }
    case AST_SLICE_INIT:
        /* Type name[:] = {…} → buffer + CCSlice_T_from_buffer (or stub view).
         * Designated `{ .ptr = …, .len = … }` is already a CCSlice value. */
        {
            int empty = (strcmp(st->c, "{}") == 0 || strcmp(st->c, "{ }") == 0);
            int is_char = (strcmp(st->a, "char") == 0);
            int is_desig = (strstr(st->c, ".ptr") != NULL ||
                            strstr(st->c, ".len") != NULL);
            if (is_char && empty && !g_shadow_slice_stub)
                return cemit_fmt(out, "%sCCSlice %s = cc_slice_empty();\n",
                                 indent, st->b);
            if (!is_char && empty && !g_shadow_slice_stub) {
                const char* sty = shadow_slice_ty(st->a);
                return cemit_fmt(out, "%s%s %s = %s_from_buffer(0, 0);\n",
                                 indent, sty, st->b, sty);
            }
            if (is_desig) {
                if (is_char)
                    return cemit_fmt(out, "%sCCSlice %s = %s;\n", indent, st->b,
                                     st->c);
                {
                    const char* sty = shadow_slice_ty(st->a);
                    return cemit_fmt(out, "%s%s %s = %s;\n", indent, sty, st->b,
                                     st->c);
                }
            }
            if (g_shadow_slice_stub) {
                return cemit_fmt(out,
                    "%s%s __cc_sl_%s[] = %s;\n"
                    "%sstruct { %s* ptr; size_t len; } %s = { "
                    "__cc_sl_%s, sizeof(__cc_sl_%s)/sizeof(__cc_sl_%s[0]) };\n",
                    indent, st->a, st->b, st->c, indent, st->a, st->b, st->b,
                    st->b, st->b);
            }
            if (is_char) {
                return cemit_fmt(out,
                    "%schar __cc_sl_%s[] = %s;\n"
                    "%sCCSlice %s = cc_slice_from_buffer(__cc_sl_%s, "
                    "sizeof(__cc_sl_%s)/sizeof(__cc_sl_%s[0]));\n",
                    indent, st->b, st->c, indent, st->b, st->b, st->b, st->b);
            }
            {
                const char* sty = shadow_slice_ty(st->a);
                return cemit_fmt(out,
                    "%s%s __cc_sl_%s[] = %s;\n"
                    "%s%s %s = %s_from_buffer(__cc_sl_%s, "
                    "sizeof(__cc_sl_%s)/sizeof(__cc_sl_%s[0]));\n",
                    indent, st->a, st->b, st->c, indent, sty, st->b, sty, st->b,
                    st->b, st->b);
            }
        }
    case AST_TYPED_INIT: {
        char expr[512];
        char tpl[256];
        char arena[128];
        AstNode* cl = shadow_expr_closure_kid(st);
        /* Type already parse/bind-normalized — emit as stored. */
        const char* ty = st->a;
        /* CCString key = @string(`…`|arg|policy, …); — only when RHS is the
         * @string call alone (no `.meth` chain). Chained forms fall through to
         * expr_text so UFCS can peel after @string rewrite. */
        {
            char policy[128];
            int sk = shadow_parse_at_string_expr_ex(st->c, tpl, sizeof(tpl), arena,
                                                    sizeof(arena), policy,
                                                    sizeof(policy));
            if (sk) {
                const char* rest = st->c;
                int dep = 0;
                while (*rest) {
                    if (*rest == '(') dep++;
                    else if (*rest == ')') {
                        dep--;
                        if (dep == 0) {
                            rest++;
                            break;
                        }
                    }
                    rest++;
                }
                while (*rest == ' ' || *rest == '\t' || *rest == '\n') rest++;
                if (*rest == '\0') {
                    char arena_expr[160];
                    if (shadow_arena_is_scratch(arena)) {
                        snprintf(arena_expr, sizeof(arena_expr),
                                 "&__cc_str_scratch");
                    } else {
                        snprintf(arena_expr, sizeof(arena_expr), "%s", arena);
                    }
                    if (sk == 1)
                        return shadow_emit_tpl_build(out, tpl, arena_expr, indent,
                                                    st->b);
                    if (sk == 3)
                        return shadow_emit_tpl_build_ex(out, tpl, arena_expr,
                                                       indent, st->b, policy);
                    if (sk == 4) {
                        char sexpr[1536];
                        if (!shadow_fmt_string_stack_expr(sexpr, sizeof(sexpr),
                                                         tpl))
                            return 0;
                        return cemit_fmt(out, "%s%s %s = %s;\n", indent, ty,
                                         st->b, sexpr);
                    }
                    /* from-arg → cc_string_from((arg), (arena)) */
                    return cemit_fmt(
                        out, "%s%s %s = cc_string_from((%s), (%s));\n", indent,
                        ty, st->b, tpl, arena_expr);
                }
            }
        }
        /* Dest type as type-formal hint (allocT peels *; block_on uses full ty). */
        const char* formal = ty && ty[0] ? ty : NULL;
        if (cl && (!st->c[0] || strncmp(cl->b, "callarg", 7) == 0) &&
            !strchr(st->c, '(')) {
            /* Bare RHS: `CCClosureN c = (T x) => {…};` */
            shadow_fmt_closure_make(expr, sizeof(expr), cl);
        } else {
            char raw[512];
            snprintf(raw, sizeof(raw), "%s", st->c);
            if (cl) shadow_splice_closure_arg(raw, sizeof(raw), cl);
            if (!shadow_emit_channel_pair_expr(expr, sizeof(expr), raw))
                shadow_emit_expr_text(st, raw, expr, sizeof(expr), formal);
            if (!shadow_rewrite_bang_exprs(expr, sizeof(expr), ctx)) return 0;
            shadow_resolve_at_create(expr, sizeof(expr), ty, st->d[0] == '*',
                                     cl != NULL && strcmp(cl->b, "create") == 0);
        }
        if (ty && strncmp(ty, "CCResult_", 9) == 0)
            shadow_rewrite_result_ctors(expr, sizeof(expr), ty);
        else if (ctx && ctx->rname && ctx->rname[0])
            shadow_rewrite_result_ctors(expr, sizeof(expr), ctx->rname);
        else
            shadow_rewrite_result_ctors_for_var(expr, sizeof(expr), st->b);
        {
            const char* vty = ty;
            char vbuf[96];
            size_t tl = ty ? strlen(ty) : 0;
            if (tl > 4 && strcmp(ty + tl - 4, "Kind") == 0) {
                size_t nl = tl - 4;
                if (nl >= sizeof(vbuf)) nl = sizeof(vbuf) - 1;
                memcpy(vbuf, ty, nl);
                vbuf[nl] = 0;
                vty = vbuf;
            }
            if (shadow_variant_diag_ctor(ctx, st, out, vty, expr, NULL))
                return 0;
            if (shadow_variant_diag_bare_desig(ctx, st, out, expr)) return 0;
            shadow_rewrite_variant_expr(expr, sizeof(expr), vty);
        }
        /* e=="!" is a safety-only unique marker from `T[:!]`, not a C declarator. */
        if (st->e[0] && !(st->e[0] == '!' && st->e[1] == 0)) {
            if (st->d[0])
                return cemit_fmt(out, "%s%s%s %s%s = %s;\n", indent, ty, st->d,
                                 st->b, st->e, expr);
            return cemit_fmt(out, "%s%s %s%s = %s;\n", indent, ty, st->b, st->e,
                             expr);
        }
        if (st->d[0])
            return cemit_fmt(out, "%s%s%s %s = %s;\n", indent, ty, st->d, st->b,
                             expr);
        if (shadow_emit_decl_init_maybe_autoblock(st, out, ctx, indent, ty))
            return 1;
        if (ctx && ty && ty[0]) {
            const char* vty = ty;
            char vbuf[96];
            size_t tl = strlen(ty);
            if (tl > 4 && strcmp(ty + tl - 4, "Kind") == 0) {
                size_t nl = tl - 4;
                if (nl < sizeof(vbuf)) {
                    memcpy(vbuf, ty, nl);
                    vbuf[nl] = 0;
                    vty = vbuf;
                }
            }
            shadow_variant_track_drop(ctx, st->b, vty);
        }
        /* Slice inits: string lit → const_char_to_slice; `{}` → empty. */
        if (ty && (strcmp(ty, "CCSlice") == 0 ||
                   strcmp(ty, "CCSliceUnique") == 0 ||
                   strcmp(ty, "const CCSlice") == 0)) {
            size_t el = strlen(expr);
            if (strcmp(expr, "{}") == 0 || strcmp(expr, "{ }") == 0)
                snprintf(expr, sizeof(expr), "cc_slice_empty()");
            else if (!shadow_slc_already_wrapped(expr, el) &&
                     shadow_slc_is_string_lit(expr, el)) {
                char wrap[540];
                snprintf(wrap, sizeof(wrap), "const_char_to_slice(%s)", expr);
                snprintf(expr, sizeof(expr), "%s", wrap);
            }
        } else if (ty && strncmp(ty, "CCSlice_", 8) == 0 &&
                   (strcmp(expr, "{}") == 0 || strcmp(expr, "{ }") == 0)) {
            snprintf(expr, sizeof(expr), "%s_from_buffer(0, 0)", ty);
        }
        return cemit_fmt(out, "%s%s %s = %s;\n", indent, ty, st->b, expr);
    }
    case AST_UFCS_STMT: {
        /* Room for `@string(...).println()` / long @slice args. */
        char call[8192];
        if (!shadow_emit_ufcs_to_buf(st, call, sizeof(call), NULL)) {
            /* Loud miss for instance/handle/scalar types, type-formal fails,
             * and @as retries. Other failures (fnptr members) keep a surface
             * call so host-cc can decide. */
            const ShadowBind* rb = shadow_bind_for_recv(st->a);
            char vty[128];
            int ai, as_n = 0;
            int instance = 0;
            const char* path = NULL;
            int line = 0;
            if (g_shadow_ufcs_miss) return 0;
            shadow_bind_base_ty(rb, vty, sizeof(vty));
            if (!vty[0])
                (void)shadow_ufcs_recv_slice_ty(st->a, vty, sizeof(vty));
            if (!vty[0]) snprintf(vty, sizeof(vty), "%s", st->a);
            instance =
                (strncmp(vty, "CCSlice_", 8) == 0 || strcmp(vty, "CCSlice") == 0 ||
                 strncmp(vty, "CCVec_", 6) == 0 || strncmp(vty, "Map_", 4) == 0 ||
                 strncmp(vty, "ArrayMap_", 9) == 0 ||
                 strncmp(vty, "CCChan", 6) == 0 ||
                 strncmp(vty, "CCResult_", 9) == 0 ||
                 strncmp(vty, "Pair_", 5) == 0 ||
                 shadow_ufcs_is_scalar_ty(vty) ||
                 strcmp(vty, "CCArena") == 0);
            for (ai = 0; ai < g_shadow_nas; ai++) {
                if (strcmp(g_shadow_as[ai].outer, vty) != 0) continue;
                if (strchr(g_shadow_as[ai].field, '.')) continue; /* transitive */
                if (!as_n)
                    fprintf(stderr, "@as retry also failed\n");
                fprintf(stderr, "@as field: %s %s\n", g_shadow_as[ai].target,
                        g_shadow_as[ai].field);
                as_n++;
            }
            if (instance || as_n) {
                if (ctx && ctx->cache)
                    (void)shadow_site_loc(ctx->cache, st, &path, &line);
                shadow_ufcs_diagnose_miss_at(st->a, st->b, path, line);
                return 0;
            }
            /* Surface form: recv.meth(args) / recv->meth(args). */
            if (shadow_ufcs_e_arrow(st->e))
                snprintf(call, sizeof(call), "%s->%s(%s)", st->a, st->b, st->c);
            else
                snprintf(call, sizeof(call), "%s.%s(%s)", st->a, st->b, st->c);
        }
        if (strncmp(st->d, "bang_block:", 11) == 0) {
            const char* bind = st->d + 11;
            char i1[80], i2[80];
            shadow_indent_nest(i1, sizeof(i1), indent, 1);
            shadow_indent_nest(i2, sizeof(i2), indent, 2);
            if (!cemit_fmt(out,
                    "%s{\n"
                    "%s__typeof__(%s) __r = %s;\n"
                    "%sif (__cc_uw_is_err(__r)) {\n",
                    indent, i1, call, call, i1))
                return 0;
            if (!shadow_resync_line(out, ctx, i2)) return 0;
            if (!shadow_emit_ptr_err_at_bind(out, ctx, i2, bind, st->b))
                return 0;
            {
                int saved_via = ctx ? ctx->err_via_bang : 0;
                if (ctx) ctx->err_via_bang = 1;
                for (int k = 0; k < st->nbody; k++) {
                    if (!shadow_emit_stmt_ctx(st->body[k], out, ctx, i2,
                                              use_cleanup)) {
                        if (ctx) ctx->err_via_bang = saved_via;
                        return 0;
                    }
                }
                if (ctx) ctx->err_via_bang = saved_via;
            }
            return cemit_fmt(out, "%s}\n%s}\n", i1, indent);
        }
        if (strcmp(st->d, "bang") == 0) {
            /* `a()!>.b()!>.c()!>` — recv holds prior hops; meth is the last. */
            if (strstr(st->a, "!>")) {
                char i1[80], i2[80];
                const char* p = st->a;
                int hop = 0;
                char first[2048];
                char lowered[8192];
                const char* bang;
                size_t n;
                shadow_indent_nest(i1, sizeof(i1), indent, 1);
                shadow_indent_nest(i2, sizeof(i2), indent, 2);
                bang = strstr(p, "!>");
                n = (size_t)(bang - p);
                while (n > 0 && (p[n - 1] == ' ' || p[n - 1] == '\t')) n--;
                if (n >= sizeof(first)) n = sizeof(first) - 1;
                memcpy(first, p, n);
                first[n] = 0;
                shadow_emit_expr_text(st, first, lowered, sizeof(lowered), NULL);
                shadow_rewrite_print_and_string(lowered, sizeof(lowered));
                shadow_rewrite_at_slice(lowered, sizeof(lowered));
                shadow_rewrite_generic_types_text(lowered, sizeof(lowered));
                if (!cemit_fmt(out, "%s{\n", indent)) return 0;
                if (!cemit_fmt(out, "%s__typeof__(%s) __r0 = %s;\n", i1, lowered,
                               lowered))
                    return 0;
                if (!cemit_fmt(out, "%sif (!__r0.ok) {\n", i1)) return 0;
                if (!shadow_resync_line(out, ctx, i2)) return 0;
                if (!shadow_emit_err_at_bind_tmp(out, ctx, i2, "e", st->b,
                                                 "__r0"))
                    return 0;
                if (!shadow_emit_handler(out, ctx, "e", i2)) return 0;
                if (!cemit_fmt(out, "%s}\n", i1)) return 0;
                p = bang + 2;
                while (*p) {
                    char hop_txt[512];
                    char recv[640];
                    char rtmp[16];
                    int fallible;
                    while (*p == ' ' || *p == '\t') p++;
                    if (!*p) break;
                    bang = strstr(p, "!>");
                    if (bang) {
                        n = (size_t)(bang - p);
                        fallible = 1;
                    } else {
                        n = strlen(p);
                        fallible = 0;
                    }
                    while (n > 0 && (p[n - 1] == ' ' || p[n - 1] == '\t')) n--;
                    if (!n) break;
                    if (n >= sizeof(hop_txt)) n = sizeof(hop_txt) - 1;
                    memcpy(hop_txt, p, n);
                    hop_txt[n] = 0;
                    snprintf(recv, sizeof(recv), "((__r%d).u.value)%s", hop,
                             hop_txt);
                    {
                        const char* hop_ty = "CCPy*";
                        shadow_emit_text_ufcs(lowered, sizeof(lowered), recv,
                                              hop_ty);
                    }
                    shadow_rewrite_generic_types_text(lowered, sizeof(lowered));
                    if (fallible) {
                        snprintf(rtmp, sizeof(rtmp), "__r%d", hop + 1);
                        if (!cemit_fmt(out,
                                       "%s__typeof__(%s) %s = %s;\n"
                                       "%sif (!%s.ok) {\n",
                                       i1, lowered, rtmp, lowered, i1, rtmp))
                            return 0;
                        if (!shadow_resync_line(out, ctx, i2)) return 0;
                        if (!shadow_emit_err_at_bind_tmp(out, ctx, i2, "e",
                                                         st->b, rtmp))
                            return 0;
                        if (!shadow_emit_handler(out, ctx, "e", i2)) return 0;
                        if (!cemit_fmt(out, "%s}\n", i1)) return 0;
                        hop++;
                        p = bang + 2;
                    } else {
                        /* Non-bang trailing hop — shouldn't appear; assign. */
                        if (!cemit_fmt(out, "%s(void)%s;\n%s}\n", i1, lowered,
                                       indent))
                            return 0;
                        return 1;
                    }
                }
                /* Final method on the last unwrapped value. */
                {
                    char surface[8192];
                    const char* targs = shadow_ufcs_e_targs(st->e);
                    char rtmp[16];
                    if (targs && targs[0] && st->c[0])
                        snprintf(surface, sizeof(surface),
                                 "((__r%d).u.value).%s::[%s](%s)", hop, st->b,
                                 targs, st->c);
                    else if (targs && targs[0])
                        snprintf(surface, sizeof(surface),
                                 "((__r%d).u.value).%s::[%s]()", hop, st->b,
                                 targs);
                    else if (st->c[0])
                        snprintf(surface, sizeof(surface),
                                 "((__r%d).u.value).%s(%s)", hop, st->b, st->c);
                    else
                        snprintf(surface, sizeof(surface),
                                 "((__r%d).u.value).%s()", hop, st->b);
                    /* Lower @string first so Python bodies are C string lits
                     * before UFCS peel (also skips `…` in peel_left). */
                    shadow_rewrite_print_and_string(surface, sizeof(surface));
                    {
                        const char* pat = "__cc_tpl; }).as_slice()";
                        const char* repl = "cc_string_as_slice(&__cc_tpl); })";
                        char* hit;
                        while ((hit = strstr(surface, pat)) != NULL) {
                            char tmp[8192];
                            size_t pre = (size_t)(hit - surface);
                            if (pre + strlen(repl) + strlen(hit + strlen(pat)) +
                                    1 >=
                                sizeof(tmp))
                                break;
                            snprintf(tmp, sizeof(tmp), "%.*s%s%s", (int)pre,
                                     surface, repl, hit + strlen(pat));
                            snprintf(surface, sizeof(surface), "%s", tmp);
                        }
                    }
                    shadow_rewrite_at_slice(surface, sizeof(surface));
                    /* Hint CCPy* so `.exec` → `cc_py_exec` (tag strips `*`). */
                    shadow_emit_text_ufcs(lowered, sizeof(lowered), surface,
                                          "CCPy*");
                    shadow_rewrite_print_and_string(lowered, sizeof(lowered));
                    {
                        const char* pat = "__cc_tpl; }).as_slice()";
                        const char* repl = "cc_string_as_slice(&__cc_tpl); })";
                        char* hit;
                        while ((hit = strstr(lowered, pat)) != NULL) {
                            char tmp[8192];
                            size_t pre = (size_t)(hit - lowered);
                            if (pre + strlen(repl) + strlen(hit + strlen(pat)) +
                                    1 >=
                                sizeof(tmp))
                                break;
                            snprintf(tmp, sizeof(tmp), "%.*s%s%s", (int)pre,
                                     lowered, repl, hit + strlen(pat));
                            snprintf(lowered, sizeof(lowered), "%s", tmp);
                        }
                    }
                    shadow_rewrite_at_slice(lowered, sizeof(lowered));
                    shadow_rewrite_generic_types_text(lowered, sizeof(lowered));
                    snprintf(rtmp, sizeof(rtmp), "__r%d", hop + 1);
                    if (!cemit_fmt(out,
                                   "%s__typeof__(%s) %s = %s;\n"
                                   "%sif (!%s.ok) {\n",
                                   i1, lowered, rtmp, lowered, i1, rtmp))
                        return 0;
                    if (!shadow_resync_line(out, ctx, i2)) return 0;
                    if (!shadow_emit_err_at_bind_tmp(out, ctx, i2, "e", st->b,
                                                     rtmp))
                        return 0;
                    if (!shadow_emit_handler(out, ctx, "e", i2)) return 0;
                    if (!cemit_fmt(out, "%s}\n%s}\n", i1, indent)) return 0;
                    return 1;
                }
            }
            return shadow_emit_try_call(out, ctx, indent, call, st->b, "e", 1);
        }
        return cemit_fmt(out, "%s%s;\n", indent, call);
    }
    case AST_ASSIGN: {
        char lhs[288];
        char rhs[2048];
        char pair[768];
        const ShadowBind* lb;
        const char* vty = NULL;
        char vbuf[96];
        const char* sink_formal = NULL;
        AstNode* cl = shadow_expr_closure_kid(st);
        snprintf(lhs, sizeof(lhs), "%s", st->a);
        /* Plain `=` only — compound assign is not a sink destination. */
        lb = shadow_bind_lookup(st->a);
        if ((!st->c[0] || strcmp(st->c, "=") == 0) && lb && lb->ty[0])
            sink_formal = lb->ty;
        if (cl && (!st->c[0] || strcmp(st->c, "=") == 0)) {
            shadow_fmt_closure_make(rhs, sizeof(rhs), cl);
        } else {
            shadow_emit_expr_text(st, st->b, rhs, sizeof(rhs), sink_formal);
            if (shadow_emit_channel_pair_expr(pair, sizeof(pair), rhs))
                snprintf(rhs, sizeof(rhs), "%s", pair);
            if (!shadow_rewrite_bang_exprs(rhs, sizeof(rhs), ctx)) return 0;
            shadow_rewrite_result_ctors_for_var(rhs, sizeof(rhs), st->a);
        }
        /* Resolve type from bare lhs name when assign target is `v.kind`. */
        if (!lb) {
            char recv[64];
            const char* acc = strstr(lhs, "->kind");
            if (!acc) acc = strstr(lhs, ".kind");
            if (acc && shadow_recv_name_before(lhs, acc, recv, sizeof(recv)))
                lb = shadow_bind_lookup(recv);
        }
        /* `*cell = …` — look up the pointer bind and peel one `*`. */
        if (!lb) {
            const char* p = lhs;
            char recv[64];
            size_t n = 0;
            while (*p == ' ' || *p == '\t' || *p == '(') p++;
            if (*p == '*') {
                p++;
                while (*p == ' ' || *p == '\t') p++;
                while (p[n] && ((p[n] >= 'A' && p[n] <= 'Z') ||
                                (p[n] >= 'a' && p[n] <= 'z') ||
                                (p[n] >= '0' && p[n] <= '9') || p[n] == '_') &&
                       n + 1 < sizeof(recv))
                    n++;
                if (n) {
                    memcpy(recv, p, n);
                    recv[n] = 0;
                    lb = shadow_bind_lookup(recv);
                }
            }
        }
        if (lb && lb->ty[0]) {
            size_t tl = strlen(lb->ty);
            if (tl > 4 && strcmp(lb->ty + tl - 4, "Kind") == 0) {
                size_t nl = tl - 4;
                if (nl >= sizeof(vbuf)) nl = sizeof(vbuf) - 1;
                memcpy(vbuf, lb->ty, nl);
                vbuf[nl] = 0;
                vty = vbuf;
            } else if (tl > 1 && lb->ty[tl - 1] == '*' &&
                       lhs[0] == '*') {
                /* `*p` assign: bind is `T*`, variant value type is `T`. */
                size_t nl = tl - 1;
                while (nl && (lb->ty[nl - 1] == ' ' || lb->ty[nl - 1] == '\t'))
                    nl--;
                if (nl >= sizeof(vbuf)) nl = sizeof(vbuf) - 1;
                memcpy(vbuf, lb->ty, nl);
                vbuf[nl] = 0;
                vty = vbuf;
            } else {
                vty = lb->ty;
            }
        }
        if ((!st->c[0] || strcmp(st->c, "=") == 0) &&
            shadow_variant_diag_kind_write(ctx, st, out, lhs))
            return 0;
        if ((!st->c[0] || strcmp(st->c, "=") == 0) &&
            shadow_variant_diag_ctor(ctx, st, out, vty, rhs, st->a))
            return 0;
        if (shadow_variant_diag_bare_desig(ctx, st, out, rhs)) return 0;
        shadow_rewrite_variant_expr(lhs, sizeof(lhs), vty);
        shadow_rewrite_variant_expr(rhs, sizeof(rhs), vty);
        {
            ShadowVariant* vv = vty ? shadow_variant_find(vty) : NULL;
            if (vv && vv->has_drop && (!st->c[0] || strcmp(st->c, "=") == 0)) {
                int id = ++g_shadow_va_tmp_id;
                return cemit_fmt(out,
                        "%s{ %s __cc_vt%d = %s; %s__cc_drop(&(%s)); %s = __cc_vt%d; }\n",
                        indent, vv->name, id, rhs, vv->name, lhs, lhs, id);
            }
        }
        if (shadow_emit_assign_maybe_autoblock(st, out, ctx, indent))
            return 1;
        return cemit_fmt(out, "%s%s %s %s;\n", indent, lhs,
                         st->c[0] ? st->c : "=", rhs);
    }
    case AST_PTR_UNWRAP: {
        /* Pointer/null default arms — keep host `__cc_uw_*` (Result temps use fields). */
        const char* mode = st->d;
        char callbuf[512];
        const char* call;
        int use_c = ctx && ctx->goto_cleanup;
        char ty[160];
        char i1[80], i2[80];
        if (shadow_emit_channel_pair_expr(callbuf, sizeof(callbuf), st->c))
            call = callbuf;
        else {
            shadow_emit_expr_text(st, st->c, callbuf, sizeof(callbuf), NULL);
            call = callbuf;
        }
        snprintf(ty, sizeof(ty), "%s*", st->a);
        shadow_indent_nest(i1, sizeof(i1), indent, 1);
        shadow_indent_nest(i2, sizeof(i2), indent, 2);
        if (strncmp(mode, "qmark_bind:", 11) == 0) {
            const char* bind = mode + 11;
            if (!cemit_fmt(out,
                    "%s%s %s;\n"
                    "%s{\n"
                    "%s__typeof__(%s) __r = %s;\n"
                    "%sif (!__cc_uw_is_err(__r)) {\n"
                    "%s%s = __cc_uw_value(__r);\n"
                    "%s} else {\n",
                    indent, ty, st->b, indent, i1, call, call, i1, i2, st->b,
                    i1))
                return 0;
            if (!shadow_resync_line(out, ctx, i2)) return 0;
            if (!shadow_emit_ptr_err_at_bind(out, ctx, i2, bind, call))
                return 0;
            return cemit_fmt(out,
                "%s%s = (%s);\n"
                "%s}\n"
                "%s}\n",
                i2, st->b, st->e, i1, indent);
        }
        if (strncmp(mode, "qmark", 5) == 0) {
            /* Variant `arm ?>` before pointer/null Result lowering. */
            {
                char qbuf[512];
                snprintf(qbuf, sizeof(qbuf), "%s ?> %s", st->c, st->e);
                if (shadow_rewrite_variant_qmark(qbuf, sizeof(qbuf)))
                    return cemit_fmt(out, "%s%s %s = %s;\n", indent, ty, st->b,
                                     qbuf);
            }
            return cemit_fmt(out,
                "%s%s %s;\n"
                "%s{\n"
                "%s__typeof__(%s) __r = %s;\n"
                "%s%s = !__cc_uw_is_err(__r) ? __cc_uw_value(__r) : (%s);\n"
                "%s}\n",
                indent, ty, st->b, indent, i1, call, call, i1, st->b, st->e,
                indent);
        }
        if (!cemit_fmt(out,
                "%s%s %s;\n"
                "%s{\n"
                "%s__typeof__(%s) __r = %s;\n"
                "%sif (__cc_uw_is_err(__r)) {\n",
                indent, ty, st->b, indent, i1, call, call, i1))
            return 0;
        if (strncmp(mode, "bang_eh", 7) == 0 ||
            (strncmp(mode, "bang_nobind", 11) == 0 && st->nbody == 0)) {
            /* `!> @destroy` / bare `!>;` — enclosing @errhandler (typed). */
            AstNode* saved_eh = ctx ? ctx->eh : NULL;
            AstNode* matched = shadow_eh_for_call(ctx, call);
            const char* bind;
            if (ctx && matched) ctx->eh = matched;
            bind = shadow_eh_bind(ctx);
            if (!shadow_resync_line(out, ctx, i2) ||
                !shadow_emit_ptr_err_at_bind(out, ctx, i2, bind, call) ||
                !shadow_emit_handler(out, ctx, bind, i2)) {
                if (ctx) ctx->eh = saved_eh;
                return 0;
            }
            if (ctx) ctx->eh = saved_eh;
        } else if (strncmp(mode, "bang_block", 10) == 0) {
            int saved_via = ctx ? ctx->err_via_bang : 0;
            if (!shadow_resync_line(out, ctx, i2)) return 0;
            /* Site is the callee expr so __cc_err_null_at embeds it. */
            if (!shadow_emit_ptr_err_at_bind(out, ctx, i2,
                                             st->e[0] ? st->e : "e", call))
                return 0;
            if (ctx) ctx->err_via_bang = 1;
            for (int k = 0; k < st->nbody; k++) {
                if (!shadow_emit_stmt_ctx(st->body[k], out, ctx, i2, use_c)) {
                    if (ctx) ctx->err_via_bang = saved_via;
                    return 0;
                }
            }
            if (ctx) ctx->err_via_bang = saved_via;
        } else {
            for (int k = 0; k < st->nbody; k++) {
                if (!shadow_emit_stmt_ctx(st->body[k], out, ctx, i2, use_c))
                    return 0;
            }
        }
        return cemit_fmt(out,
                "%s}\n"
                "%s%s = __cc_uw_value(__r);\n"
                "%s}\n",
                i1, i1, st->b, indent);
    }
    case AST_NURSERY_DESTROY: {
        char call[160];
        snprintf(call, sizeof(call), "%s(NULL)", st->c);
        char ty[160];
        snprintf(ty, sizeof(ty), "%s*", st->a);
        return shadow_emit_try_assign(out, ctx, indent, ty, st->b, call, call,
                                      "e", "(__r).u.value");
    }
    case AST_SPAWN_CLOSURE: {
        if (st->c[0])
            return cemit_fmt(out, "%scc_nursery_spawn_fn(%s, %s);\n",
                             indent, st->a, st->c);
        char args[256];
        char i1[80];
        if (!shadow_caps_call_args(st->e, args, sizeof(args))) return 0;
        if (st->b[0] && (strcmp(st->b, "send_task") == 0 ||
                         strcmp(st->b, "send_task_hybrid") == 0)) {
            const char* spawn_fn =
                strcmp(st->b, "send_task_hybrid") == 0
                    ? "cc_fiber_spawn_closure0_v2"
                    : "cc_fiber_spawn_closure0";
            shadow_indent_nest(i1, sizeof(i1), indent, 1);
            if (args[0])
                return cemit_fmt(out,
                    "%sdo {\n"
                    "%sCCClosure0 __cc_st_c = cc_closure__N%s_make(%s);\n"
                    "%sCCTask __cc_st_t = %s(__cc_st_c);\n"
                    "%sint __cc_st_e = cc_chan_send((%s).raw, &__cc_st_t, "
                    "sizeof(__cc_st_t));\n"
                    "%sif (__cc_st_e != 0) { (void)cc_block_on_intptr(__cc_st_t); }\n"
                    "%s} while (0);\n",
                    indent, i1, st->d[0] ? st->d : "1", args, i1, spawn_fn, i1,
                    st->a, i1, indent);
            return cemit_fmt(out,
                "%sdo {\n"
                "%sCCClosure0 __cc_st_c = cc_closure__N%s_make();\n"
                "%sCCTask __cc_st_t = %s(__cc_st_c);\n"
                "%sint __cc_st_e = cc_chan_send((%s).raw, &__cc_st_t, "
                "sizeof(__cc_st_t));\n"
                "%sif (__cc_st_e != 0) { (void)cc_block_on_intptr(__cc_st_t); }\n"
                "%s} while (0);\n",
                indent, i1, st->d[0] ? st->d : "1", i1, spawn_fn, i1, st->a, i1,
                indent);
        }
        {
            const char* nur_spawn =
                (st->b[0] && (strcmp(st->b, "spawnhybrid") == 0 ||
                              strcmp(st->b, "spawnhybrid_unsafe") == 0))
                    ? "cc_nursery_spawnhybrid_closure0"
                    : "cc_nursery_spawn_closure0";
            if (args[0])
                return cemit_fmt(out,
                    "%s%s(%s, cc_closure__N%s_make(%s));\n", indent, nur_spawn,
                    st->a, st->d[0] ? st->d : "1", args);
            return cemit_fmt(out, "%s%s(%s, cc_closure__N%s_make());\n", indent,
                             nur_spawn, st->a, st->d[0] ? st->d : "1");
        }
    }
    case AST_CALL_NUM: {
        const ShadowBind* nb = shadow_bind_lookup(st->a);
        if (nb && strcmp(nb->ty, "CCClosure1") == 0)
            return cemit_fmt(out, "%scc_closure1_call(%s, (intptr_t)(%s));\n",
                             indent, st->a, st->b);
        if (nb && strcmp(nb->ty, "CCClosure2") == 0)
            return cemit_fmt(out,
                             "%scc_closure2_call(%s, (intptr_t)(%s), (intptr_t)0);\n",
                             indent, st->a, st->b);
        return cemit_fmt(out, "%s%s(%s);\n", indent, st->a, st->b);
    }
    case AST_CALL_ARGS: {
        /* Long printf/fprintf arg lists exceed the old 288 beachhead. */
        char call[1536];
        char fn[96];
        const ShadowBind* cb = shadow_bind_lookup(st->a);
        AstNode* st_cl = shadow_expr_closure_kid(st);
        int is_send_task =
            strcmp(st->a, "cc_channel_send_task") == 0 ||
            strcmp(st->a, "cc_channel_send_task_hybrid") == 0;
        /* Naked print helpers: println/eprintln/fprintln → cc_* */
        if (strcmp(st->a, "println") == 0)
            snprintf(fn, sizeof(fn), "cc_println");
        else if (strcmp(st->a, "eprintln") == 0)
            snprintf(fn, sizeof(fn), "cc_eprintln");
        else if (strcmp(st->a, "fprintln") == 0)
            snprintf(fn, sizeof(fn), "cc_fprintln");
        else
            snprintf(fn, sizeof(fn), "%s", st->a);
        /* cc_channel_send_task(tx, () => …) → fiber-spawn + chan_send(CCTask). */
        if (is_send_task && st_cl) {
            char tx[128];
            char i1[80];
            const char* spawn_fn =
                strcmp(st->a, "cc_channel_send_task_hybrid") == 0
                    ? "cc_fiber_spawn_closure0_v2"
                    : "cc_fiber_spawn_closure0";
            char caps_args[256];
            snprintf(st_cl->b, sizeof(st_cl->b), "%s",
                     strcmp(st->a, "cc_channel_send_task_hybrid") == 0
                         ? "send_task_hybrid"
                         : "send_task");
            /* First arg is the tx handle (text before spliced closure). */
            {
                const char* src = st->b;
                const char* comma = NULL;
                int depth = 0;
                const char* p;
                size_t n0;
                tx[0] = 0;
                for (p = src; p && *p; p++) {
                    if (*p == '(' || *p == '[' || *p == '{') depth++;
                    else if (*p == ')' || *p == ']' || *p == '}') depth--;
                    else if (*p == ',' && depth == 0) {
                        comma = p;
                        break;
                    }
                }
                if (comma) {
                    n0 = (size_t)(comma - src);
                    if (n0 >= sizeof(tx)) n0 = sizeof(tx) - 1;
                    memcpy(tx, src, n0);
                    tx[n0] = 0;
                    while (n0 && (tx[n0 - 1] == ' ' || tx[n0 - 1] == '\t'))
                        tx[--n0] = 0;
                } else {
                    snprintf(tx, sizeof(tx), "%s", src);
                }
            }
            if (!shadow_caps_call_args(st_cl->e, caps_args, sizeof(caps_args)))
                return 0;
            shadow_indent_nest(i1, sizeof(i1), indent, 1);
            if (caps_args[0])
                return cemit_fmt(out,
                    "%sdo {\n"
                    "%sCCClosure0 __cc_st_c = cc_closure__N%s_make(%s);\n"
                    "%sCCTask __cc_st_t = %s(__cc_st_c);\n"
                    "%sint __cc_st_e = cc_chan_send((%s).raw, &__cc_st_t, "
                    "sizeof(__cc_st_t));\n"
                    "%sif (__cc_st_e != 0) { (void)cc_block_on_intptr(__cc_st_t); }\n"
                    "%s} while (0);\n",
                    indent, i1, st_cl->d[0] ? st_cl->d : "1", caps_args, i1,
                    spawn_fn, i1, tx, i1, indent);
            return cemit_fmt(out,
                "%sdo {\n"
                "%sCCClosure0 __cc_st_c = cc_closure__N%s_make();\n"
                "%sCCTask __cc_st_t = %s(__cc_st_c);\n"
                "%sint __cc_st_e = cc_chan_send((%s).raw, &__cc_st_t, "
                "sizeof(__cc_st_t));\n"
                "%sif (__cc_st_e != 0) { (void)cc_block_on_intptr(__cc_st_t); }\n"
                "%s} while (0);\n",
                indent, i1, st_cl->d[0] ? st_cl->d : "1", i1, spawn_fn, i1, tx,
                i1, indent);
        }
        {
            char args[1024];
            char full[1280];
            shadow_emit_call_args_text(st, args, sizeof(args));
            /* Rewrite full `cc_channel_pair(&tx,&rx)` (needs callee name). */
            snprintf(full, sizeof(full), "%s(%s)", fn, args);
            if (shadow_emit_channel_pair_expr(call, sizeof(call), full)) {
                /* call already set */
            } else
            /* `c(args)` on CCClosure1/2 → cc_closureN_call */
            if (cb && strcmp(cb->ty, "CCClosure1") == 0) {
                snprintf(call, sizeof(call),
                         "cc_closure1_call(%s, (intptr_t)(%s))", st->a, args);
            } else if (cb && strcmp(cb->ty, "CCClosure2") == 0) {
                char a0[128], a1[128];
                const char* comma = NULL;
                int depth = 0;
                const char* p;
                a0[0] = a1[0] = 0;
                for (p = args; *p; p++) {
                    if (*p == '(' || *p == '[' || *p == '{') depth++;
                    else if (*p == ')' || *p == ']' || *p == '}') depth--;
                    else if (*p == ',' && depth == 0) {
                        comma = p;
                        break;
                    }
                }
                if (comma) {
                    size_t n0 = (size_t)(comma - args);
                    if (n0 >= sizeof(a0)) n0 = sizeof(a0) - 1;
                    memcpy(a0, args, n0);
                    a0[n0] = 0;
                    while (n0 && (a0[n0 - 1] == ' ' || a0[n0 - 1] == '\t'))
                        a0[--n0] = 0;
                    snprintf(a1, sizeof(a1), "%s", comma + 1);
                    while (a1[0] == ' ' || a1[0] == '\t')
                        memmove(a1, a1 + 1, strlen(a1));
                    snprintf(call, sizeof(call),
                             "cc_closure2_call(%s, (intptr_t)(%s), (intptr_t)(%s))",
                             st->a, a0, a1);
                } else {
                    snprintf(call, sizeof(call),
                             "cc_closure2_call(%s, (intptr_t)(%s), (intptr_t)0)",
                             st->a, args);
                }
            } else {
                snprintf(call, sizeof(call), "%s(%s)", fn, args);
            }
            /* CALL_ARGS.b is inner args only — splice onto the full call. */
            if (st_cl) shadow_splice_closure_arg(call, sizeof(call), st_cl);
            shadow_rewrite_slice_lit_call_args(call, sizeof(call));
            shadow_rewrite_as_call_args(call, sizeof(call));
            if (!shadow_rewrite_bang_exprs(call, sizeof(call), ctx)) return 0;
        }
        if (strcmp(st->d, "bang") == 0)
            return shadow_emit_try_call(out, ctx, indent, call, fn, "e", 1);
        if (ctx && ctx->send_task_ret) {
            return cemit_fmt(out,
                "%s{\n"
                "%s    __typeof__(%s)* __cc_st_r = (__typeof__(%s)*)"
                "cc_task_result_ptr(sizeof(*__cc_st_r));\n"
                "%s    if (!__cc_st_r) return NULL;\n"
                "%s    *__cc_st_r = (%s);\n"
                "%s    return __cc_st_r;\n"
                "%s}\n",
                indent, indent, call, call, indent, indent, call, indent,
                indent);
        }
        {
            char args[1024];
            shadow_emit_call_args_text(st, args, sizeof(args));
            return shadow_emit_call_maybe_autoblock(st, out, ctx, indent, call,
                                                    fn, args);
        }
    }
    case AST_AT_STMT:
        if (strcmp(st->a, "await") == 0 && st->c[0]) {
            char lowered[384];
            shadow_emit_text_ufcs(lowered, sizeof(lowered), st->c, NULL);
            return cemit_fmt(out, "%s%s;\n", indent, lowered);
        }
        if (strcmp(st->a, "grammar") == 0 && st->b[0]) {
            /* Beachhead @grammar(cli) T { flag --test … } */
            return cemit_fmt(out,
                "%stypedef struct %s { int test; } %s;\n"
                "%sstatic inline int %s_parse_args(int argc, char** argv, "
                "CCArena* __a, %s* __o) {\n"
                "%s    (void)__a;\n"
                "%s    if (!__o) return 0;\n"
                "%s    __o->test = 0;\n"
                "%s    for (int __i = 1; __i < argc; __i++) {\n"
                "%s        if (argv[__i] && strcmp(argv[__i], \"--test\") == 0)\n"
                "%s            __o->test = 1;\n"
                "%s    }\n"
                "%s    return 1;\n"
                "%s}\n",
                indent, st->b, st->b, indent, st->b, st->b, indent, indent,
                indent, indent, indent, indent, indent, indent, indent);
        }
        if (strcmp(st->a, "variant") == 0 && st->b[0] && st->c[0]) {
            /* `@variant[(packed)] Name { arm: Type; … }` → Kind enum + layout. */
            char arms[8][64];
            char tys[8][96];
            int narm = 0;
            int is_packed = (st->d[0] && strcmp(st->d, "packed") == 0);
            const char* p = st->c;
            while (*p && narm < 8) {
                while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '{' ||
                       *p == '}')
                    p++;
                if (!*p) break;
                const char* an = p;
                while (*p && *p != ':' && *p != ' ' && *p != '\t') p++;
                size_t al = (size_t)(p - an);
                if (al == 0 || al >= sizeof(arms[0])) break;
                memcpy(arms[narm], an, al);
                arms[narm][al] = 0;
                {
                    int ai;
                    for (ai = 0; ai < narm; ai++) {
                        if (strcmp(arms[ai], arms[narm]) == 0) {
                            const char* path = NULL;
                            int line = 1, col = 1;
                            int first_line = 1;
                            if (ctx && ctx->cache && st->file_id) {
                                FileTape* ft = tape_by_id(ctx->cache, st->file_id);
                                if (ft && ft->bytes) {
                                    size_t off = st->tok_off;
                                    const char* src = ft->bytes;
                                    size_t slen = ft->len;
                                    size_t i;
                                    int seen = 0;
                                    for (i = st->tok_off; i + al + 1 < slen; i++) {
                                        if (memcmp(src + i, arms[narm], al) == 0 &&
                                            src[i + al] == ':') {
                                            if (seen == 0) {
                                                int fl = 1, fc = 1;
                                                offset_to_linecol(ft->bytes,
                                                                  ft->len, i,
                                                                  &fl, &fc);
                                                first_line = fl;
                                            }
                                            if (seen == 1) {
                                                off = i;
                                                break;
                                            }
                                            seen++;
                                        }
                                    }
                                    offset_to_linecol(ft->bytes, ft->len, off,
                                                      &line, &col);
                                    path = ft->path;
                                }
                            }
                            fprintf(stderr,
                                    "%s:%d:%d: error: @variant '%s': duplicate "
                                    "arm name '%s' (first declared at line %d)\n",
                                    path && path[0] ? path : "<input>", line,
                                    col, st->b, arms[narm], first_line);
                            out->err = 1;
                            return 0;
                        }
                    }
                }
                if (shadow_is_c_keyword(arms[narm])) {
                    const char* path = NULL;
                    int line = 1, col = 1;
                    if (ctx && ctx->cache && st->file_id) {
                        FileTape* ft = tape_by_id(ctx->cache, st->file_id);
                        if (ft && ft->bytes) {
                            /* Point at the arm name inside the variant body. */
                            size_t off = st->tok_off;
                            const char* hit = strstr(st->c, arms[narm]);
                            if (hit && ft->bytes) {
                                /* Prefer source occurrence of `arm:` after `{`. */
                                const char* src = ft->bytes;
                                size_t slen = ft->len;
                                size_t i;
                                for (i = st->tok_off; i + al + 1 < slen; i++) {
                                    if (memcmp(src + i, arms[narm], al) == 0 &&
                                        src[i + al] == ':') {
                                        off = i;
                                        break;
                                    }
                                }
                            }
                            (void)hit;
                            offset_to_linecol(ft->bytes, ft->len, off, &line,
                                              &col);
                            path = ft->path;
                        }
                    }
                    fprintf(stderr,
                            "%s:%d:%d: error: @variant '%s': arm name '%s' is a "
                            "C keyword\npick another spelling\n",
                            path && path[0] ? path : "<input>", line, col, st->b,
                            arms[narm]);
                    out->err = 1;
                    return 0;
                }
                while (*p == ' ' || *p == '\t') p++;
                if (*p != ':') break;
                p++;
                while (*p == ' ' || *p == '\t') p++;
                const char* tn = p;
                while (*p && *p != ';') p++;
                size_t tl = (size_t)(p - tn);
                while (tl > 0 && (tn[tl - 1] == ' ' || tn[tl - 1] == '\t'))
                    tl--;
                if (tl == 0 || tl >= sizeof(tys[0])) break;
                memcpy(tys[narm], tn, tl);
                tys[narm][tl] = 0;
                /* Recursive by-value arm: type names this variant without * / [:]. */
                if (strcmp(tys[narm], "void") != 0 &&
                    strstr(tys[narm], st->b) &&
                    !strchr(tys[narm], '*') && !strstr(tys[narm], "[:")) {
                    char msg[320];
                    const char* path = "<input>";
                    int line = 1, col = 1;
                    size_t al = strlen(arms[narm]);
                    size_t tl2 = strlen(tys[narm]);
                    snprintf(msg, sizeof(msg),
                             "@variant '%s': arm '%s' contains '%s' by value — "
                             "recursive arms must go through a pointer or "
                             "slice (e.g. '%s*' or '%s[:]')",
                             st->b, arms[narm], st->b, st->b, st->b);
                    if (ctx && ctx->cache && st->file_id) {
                        FileTape* ft = tape_by_id(ctx->cache, st->file_id);
                        if (ft && ft->bytes) {
                            size_t off = st->tok_off;
                            size_t i;
                            for (i = st->tok_off; i + al + 1 + tl2 < ft->len;
                                 i++) {
                                if (memcmp(ft->bytes + i, arms[narm], al) == 0 &&
                                    ft->bytes[i + al] == ':') {
                                    size_t t = i + al + 1;
                                    while (t < ft->len &&
                                           (ft->bytes[t] == ' ' ||
                                            ft->bytes[t] == '\t'))
                                        t++;
                                    if (t + tl2 <= ft->len &&
                                        memcmp(ft->bytes + t, tys[narm],
                                               tl2) == 0) {
                                        off = t;
                                        break;
                                    }
                                }
                            }
                            offset_to_linecol(ft->bytes, ft->len, off, &line,
                                              &col);
                            tape_logical_at(ft->bytes, ft->len, off, ft->path,
                                            NULL, 0, &line);
                            if (ft->path && ft->path[0]) path = ft->path;
                        }
                    }
                    fprintf(stderr, "%s:%d:%d: error: %s\n", path, line, col,
                            msg);
                    out->err = 1;
                    return 0;
                }
                if (*p == ';') p++;
                narm++;
            }
            if (narm <= 0)
                return cemit_fmt(out, "%s/* @variant %s */\n", indent, st->b);
            if (!cemit_fmt(out, "%stypedef enum { ", indent)) return 0;
            for (int i = 0; i < narm; i++) {
                if (!cemit_fmt(out, "%s%s_%s%s", i ? ", " : "", st->b, arms[i],
                               ""))
                    return 0;
            }
            if (!cemit_fmt(out, " } %sKind;\n", st->b)) return 0;
            if (is_packed) {
                ShadowVariant pv;
                int ai, d;
                const char* nut;
                memset(&pv, 0, sizeof(pv));
                snprintf(pv.name, sizeof(pv.name), "%s", st->b);
                pv.narm = narm;
                pv.is_packed = 1;
                for (ai = 0; ai < narm; ai++) {
                    snprintf(pv.arms[ai], sizeof(pv.arms[0]), "%s", arms[ai]);
                    snprintf(pv.tys[ai], sizeof(pv.tys[0]), "%s", tys[ai]);
                    pv.is_void[ai] = (strcmp(tys[ai], "void") == 0);
                }
                shadow_variant_compute_packed(&pv);
                if (pv.donor_arm < 0) {
                    fprintf(stderr,
                            "error: @variant(packed) '%s' cannot be niche-packed\n",
                            st->b);
                    if (narm == 2) {
                        unsigned sz0 = shadow_variant_type_size(tys[0]);
                        unsigned sz1 = shadow_variant_type_size(tys[1]);
                        fprintf(stderr,
                                "  arm '%s' (%s, %u bytes) and arm '%s' (%s, %u bytes)\n"
                                "  neither donates a free niche\n",
                                arms[0], tys[0], sz0, arms[1], tys[1], sz1);
                    }
                    out->err = 1;
                    return 0;
                }
                nut = (pv.niche_width == 1) ? "uint8_t"
                      : (pv.niche_width == 2) ? "uint16_t"
                      : (pv.niche_width == 4) ? "uint32_t" : "uint64_t";
                if (!cemit_fmt(out,
                        "%stypedef struct %s { _Alignas(%u) unsigned char __cc_p[%u]; } %s;\n",
                        indent, st->b, pv.packed_align, pv.packed_size, st->b))
                    return 0;
                for (ai = 0; ai < narm; ai++) {
                    if (pv.is_void[ai]) continue;
                    if (!cemit_fmt(out,
                            "%stypedef struct { %s %s; } %s__cc_ov_%s;\n",
                            indent, tys[ai], arms[ai], st->b, arms[ai]))
                        return 0;
                }
                if (!cemit_fmt(out,
                        "%sstatic inline %sKind %s__cc_kind(const %s* __v) {\n",
                        indent, st->b, st->b, st->b))
                    return 0;
                d = pv.donor_arm;
                if (narm == 1) {
                    if (!cemit_fmt(out, "%s    (void)__v; return %s_%s;\n",
                                   indent, st->b, arms[0]))
                        return 0;
                } else {
                    int o = 1 - d;
                    if (!cemit_fmt(out,
                            "%s    %s __n; __builtin_memcpy(&__n, __v->__cc_p + %u, %u);\n"
                            "%s    return __n == (%s)%lluULL ? %s_%s : %s_%s;\n",
                            indent, nut, pv.niche_off, pv.niche_width, indent, nut,
                            pv.niche_sentinel, st->b, arms[o], st->b, arms[d]))
                        return 0;
                }
                if (!cemit_fmt(out, "%s}\n", indent)) return 0;
                for (ai = 0; ai < narm; ai++) {
                    if (pv.is_void[ai]) {
                        if (!cemit_fmt(out,
                                "%sstatic inline %s %s__cc_set_%s(void) {\n"
                                "%s    %s __r; __builtin_memset(__r.__cc_p, 0, sizeof(%s)); return __r; }\n",
                                indent, st->b, st->b, arms[ai], indent, st->b,
                                st->b))
                            return 0;
                    } else {
                        if (!cemit_fmt(out,
                                "%sstatic inline %s %s__cc_set_%s(%s __x) {\n"
                                "%s    %s __r; __builtin_memset(__r.__cc_p, 0, sizeof(%s));\n"
                                "%s    __builtin_memcpy(__r.__cc_p, &__x, sizeof(__x));\n",
                                indent, st->b, st->b, arms[ai], tys[ai], indent,
                                st->b, st->b, indent))
                            return 0;
                        if (ai != d && narm == 2) {
                            if (!cemit_fmt(out,
                                    "%s    %s __s = (%s)%lluULL; __builtin_memcpy(__r.__cc_p + %u, &__s, %u);\n",
                                    indent, nut, nut, pv.niche_sentinel,
                                    pv.niche_off, pv.niche_width))
                                return 0;
                        }
                        if (!cemit_fmt(out, "%s    return __r; }\n", indent))
                            return 0;
                    }
                }
                for (ai = 0; ai < narm; ai++) {
                    if (pv.is_void[ai]) continue;
                    if (!cemit_fmt(out,
                            "%sstatic inline %s %s__cc_get_%s(const %s* __v) {\n"
                            "%s    %s __x; __builtin_memcpy(&__x, __v->__cc_p, sizeof(%s)); return __x; }\n",
                            indent, tys[ai], st->b, arms[ai], st->b, indent,
                            tys[ai], tys[ai]))
                        return 0;
                }
                shadow_variant_register_full(st->b, arms, tys, pv.is_void, narm, 1);
            } else {
                if (!cemit_fmt(out,
                        "%stypedef struct %s { %sKind kind; union { ",
                        indent, st->b, st->b))
                    return 0;
                {
                    int is_void[8];
                    int npayload = 0;
                    for (int i = 0; i < narm; i++) {
                        is_void[i] = (strcmp(tys[i], "void") == 0);
                        if (is_void[i]) continue;
                        if (!cemit_fmt(out, "%s %s; ", tys[i], arms[i])) return 0;
                        npayload++;
                    }
                    if (npayload == 0 &&
                        !cemit_fmt(out, "char __cc_variant_empty; "))
                        return 0;
                    if (!cemit_fmt(out, "} u; } %s;\n", st->b)) return 0;
                    shadow_variant_register_full(st->b, arms, tys, is_void, narm,
                                                 0);
                }
            }
            {
                ShadowVariant* vv = shadow_variant_find(st->b);
                int ai;
                if (vv) shadow_variant_update_drop(vv);
                if (vv && vv->has_drop) {
                    if (vv->is_packed) {
                        if (!cemit_fmt(out,
                                "%sstatic inline void %s__cc_drop(%s* __v) {\n"
                                "%s    switch (%s__cc_kind(__v)) {\n",
                                indent, st->b, st->b, indent, st->b))
                            return 0;
                    } else if (!cemit_fmt(out,
                            "%sstatic inline void %s__cc_drop(%s* __v) {\n"
                            "%s    switch (__v->kind) {\n",
                            indent, st->b, st->b, indent))
                        return 0;
                    for (ai = 0; ai < vv->narm; ai++) {
                        const char* hook;
                        if (vv->is_void[ai]) continue;
                        hook = shadow_destroy_hook_for(vv->tys[ai]);
                        if (!hook || !hook[0]) continue;
                        if (vv->is_packed) {
                            if (!cemit_fmt(out,
                                    "%s    case %s_%s: { %s __t = %s__cc_get_%s(__v); %s(&__t); } break;\n",
                                    indent, st->b, vv->arms[ai], vv->tys[ai],
                                    st->b, vv->arms[ai], hook))
                                return 0;
                        } else {
                            if (!cemit_fmt(out,
                                    "%s    case %s_%s: { %s __t = __v->u.%s; %s(&__t); } break;\n",
                                    indent, st->b, vv->arms[ai], vv->tys[ai],
                                    vv->arms[ai], hook))
                                return 0;
                        }
                    }
                    if (!cemit_fmt(out, "%s    default: break;\n%s    }\n%s}\n",
                                   indent, indent, indent))
                        return 0;
                }
            }
            return 1;
        }
        return cemit_fmt(out, "%s/* @%s */\n", indent, st->a);
    case AST_RAW_LINE:
        /* Stmt-position tape passthrough (local `enum { … };`, etc.). */
        if (st->e[0] && strcmp(st->e, "tape") == 0 && ctx && ctx->cache) {
            FileTape* ft = tape_by_id(ctx->cache, st->file_id);
            size_t o0 = 0, o1 = 0;
            char* endp = NULL;
            if (!ft || !ft->bytes) {
                fprintf(stderr, "error: stmt raw tape span missing file\n");
                out->err = 1;
                return 0;
            }
            o0 = (size_t)strtoull(st->a, &endp, 10);
            o1 = (size_t)strtoull(st->b, &endp, 10);
            if (o1 > ft->len) o1 = ft->len;
            if (o0 > o1) o0 = o1;
            if (o1 > o0) {
                if (indent && indent[0] && !cemit_str(out, indent)) return 0;
                if (!cemit_buf(out, ft->bytes + o0, o1 - o0)) return 0;
                if (ft->bytes[o1 - 1] != '\n' && !cemit_str(out, "\n"))
                    return 0;
            }
            return 1;
        }
        if (st->a[0])
            return cemit_fmt(out, "%s%s\n", indent, st->a);
        return 1;
    default:
        fprintf(stderr, "error: stmt %s not lowerable\n", ast_kind_name(st->kind));
        out->err = 1;
        return 0;
    }
}

static int shadow_stmt_uses_scratch(AstNode* st) {
    if (!st) return 0;
    if (st->kind == AST_PRINTLN_TPL) return 1;
    if (st->kind == AST_TYPED_INIT) {
        char tpl[256], arena[128];
        if (shadow_parse_at_string_expr(st->c, tpl, sizeof(tpl), arena,
                                        sizeof(arena)) &&
            shadow_arena_is_scratch(arena))
            return 1;
    }
    for (int k = 0; k < st->nbody; k++)
        if (shadow_stmt_uses_scratch(st->body[k])) return 1;
    for (int k = 0; k < st->ndbody; k++)
        if (shadow_stmt_uses_scratch(st->dbody[k])) return 1;
    if (st->kids) {
        for (int k = 0; k < st->nkids; k++)
            if (shadow_stmt_uses_scratch(st->kids[k])) return 1;
    }
    return 0;
}

static int shadow_stmt_is_destroy(AstNode* st) {
    if (!st) return 0;
    if (st->kind == AST_NURSERY_DESTROY) return 1;
    /* @detach transfers ownership — no scope-exit cleanup. */
    if (st->kind == AST_VAL_DESTROY) return strcmp(st->e, "_detach") != 0;
    if (st->kind == AST_VAR_UNWRAP && shadow_mode_has_destroy(st->c)) return 1;
    return st->kind == AST_PTR_UNWRAP && shadow_mode_has_destroy(st->d);
}

/* Arena expr from lowered cc_arena_alloc_T*(_, ARENA, …) — beachhead stand-in
 * for registered destroy on arena-allocated pointers. */
static int shadow_arena_from_alloc_init(const char* init, char* arena,
                                       size_t cap) {
    const char* p;
    const char* comma;
    const char* a0;
    const char* a1;
    size_t n;
    if (!init || !arena || !cap) return 0;
    p = strstr(init, "cc_arena_alloc_T_count(");
    if (p) p += strlen("cc_arena_alloc_T_count(");
    else {
        p = strstr(init, "cc_arena_alloc_T(");
        if (!p) return 0;
        p += strlen("cc_arena_alloc_T(");
    }
    comma = strchr(p, ',');
    if (!comma) return 0;
    a0 = comma + 1;
    while (*a0 == ' ' || *a0 == '\t') a0++;
    a1 = a0;
    while (*a1 && *a1 != ',' && *a1 != ')') a1++;
    while (a1 > a0 && (a1[-1] == ' ' || a1[-1] == '\t')) a1--;
    n = (size_t)(a1 - a0);
    if (n == 0 || n >= cap) return 0;
    memcpy(arena, a0, n);
    arena[n] = 0;
    return 1;
}

/* Index of st in ctx->destroys, or -1 (block-scoped / unknown). */
static int shadow_destroy_index(ShadowCtx* ctx, AstNode* st) {
    int i;
    if (!ctx || !ctx->destroys || !st) return -1;
    for (i = 0; i < ctx->ndestroys; i++)
        if (ctx->destroys[i] == st) return i;
    return -1;
}

/* Emit one @destroy site (fn cleanup or block exit; caller iterates reverse).
 * Fn-level sites are gated by __cc_defer_hw so unreached decls are skipped. */
static int shadow_emit_one_destroy(AstNode* st, CEmit* out, ShadowCtx* ctx,
                                   const char* indent) {
    const char* name = NULL;
    const char* ty = NULL;
    int bare = 0;
    int is_ptr = 0;
    int hw_idx = -1;
    AstNode** dbody = NULL;
    int ndbody = 0;
    AstNode** body = NULL;
    int nbody = 0;
    if (!indent) indent = (ctx && ctx->body_indent) ? ctx->body_indent : "    ";
    if (st->kind == AST_NURSERY_DESTROY) {
        name = st->b;
        ty = st->a;
        dbody = st->body;
        ndbody = st->nbody;
    } else if (st->kind == AST_PTR_UNWRAP && shadow_mode_has_destroy(st->d)) {
        name = st->b;
        ty = st->a;
        is_ptr = 1;
        bare = shadow_mode_destroy_bare(st->d);
        dbody = st->dbody;
        ndbody = st->ndbody;
    } else if (st->kind == AST_VAR_UNWRAP && shadow_mode_has_destroy(st->c)) {
        /* `CCPyObj x = … !>(e){…} @destroy;` — value local + bare destroy. */
        name = st->a;
        ty = st->e[0] ? st->e : "int";
        is_ptr = 0;
        bare = shadow_mode_destroy_bare(st->c);
        dbody = st->dbody;
        ndbody = st->ndbody;
    } else if (st->kind == AST_VAL_DESTROY) {
        name = st->b;
        ty = st->a;
        is_ptr = (st->d[0] == '*');
        bare = (strcmp(st->e, "_D") != 0);
        body = st->body;
        nbody = st->nbody;
    } else {
        return 1;
    }
    hw_idx = shadow_destroy_index(ctx, st);
    int is_nur = strcmp(ty, "CCNursery") == 0;
    int is_chan = strcmp(ty, "CCChan") == 0;
    int is_arena = strcmp(ty, "CCArena") == 0;
    char gate_ind[80];
    char nested[80];
    const char* outer = indent;
    const char* body_ind = indent;
    /* Already inside cleanup — don't re-enter via soft returns. */
    int saved_goto = ctx ? ctx->goto_cleanup : 0;
    if (ctx) ctx->goto_cleanup = 0;
    int ok = 1;
    if (hw_idx >= 0) {
        if (!cemit_fmt(out, "%sif (__cc_defer_hw > %d) {\n", outer, hw_idx))
            return 0;
        shadow_indent_nest(gate_ind, sizeof(gate_ind), outer, 1);
        body_ind = gate_ind;
    }
    shadow_indent_nest(nested, sizeof(nested), body_ind, 1);
    /* Registered-type hooks first (nursery/chan/arena), then arena-pointer
     * release beachhead — order matters for CCNursery* … @destroy. */
    if (is_nur) {
        if (!cemit_fmt(out,
                "%s{\n"
                "%scc_nursery_wait(%s);\n",
                body_ind, nested, name))
            ok = 0;
        else if (!bare) {
            for (int k = 0; k < ndbody && ok; k++) {
                if (!shadow_dbody_is_stmt(dbody[k])) continue;
                if (!shadow_emit_stmt_ctx(dbody[k], out, ctx, nested, 0)) ok = 0;
            }
            for (int k = 0; k < nbody && ok; k++) {
                if (!shadow_emit_stmt_ctx(body[k], out, ctx, nested, 0)) ok = 0;
            }
        }
        if (ok && !cemit_fmt(out,
                "%scc_nursery_free(%s);\n"
                "%s}\n",
                nested, name, body_ind))
            ok = 0;
    } else if (is_chan) {
        if (!bare && (ndbody || nbody)) {
            if (!cemit_fmt(out, "%s{\n", body_ind)) ok = 0;
            for (int k = 0; k < ndbody && ok; k++) {
                if (!shadow_dbody_is_stmt(dbody[k])) continue;
                if (!shadow_emit_stmt_ctx(dbody[k], out, ctx, nested, 0)) ok = 0;
            }
            for (int k = 0; k < nbody && ok; k++) {
                if (!shadow_emit_stmt_ctx(body[k], out, ctx, nested, 0)) ok = 0;
            }
            if (ok && !cemit_fmt(out, "%s}\n", body_ind)) ok = 0;
        }
        if (ok && !cemit_fmt(out, "%scc_channel_free(%s);\n", body_ind, name))
            ok = 0;
    } else if (is_arena && !is_ptr) {
        /* Registered pre-destroy (mark_arena_pre_destroy) before user body. */
        if (shadow_ufn_exists("mark_arena_pre_destroy")) {
            if (!cemit_fmt(out, "%smark_arena_pre_destroy(&%s);\n", body_ind,
                           name))
                ok = 0;
        }
        if (ok && !bare) {
            for (int k = 0; k < nbody && ok; k++) {
                if (!shadow_emit_stmt_ctx(body[k], out, ctx, body_ind, 0)) ok = 0;
            }
        }
        if (ok && !cemit_fmt(out, "%scc_arena_destroy(&%s);\n", body_ind, name))
            ok = 0;
    } else if (is_ptr && (st->kind == AST_VAL_DESTROY ||
                          st->kind == AST_PTR_UNWRAP)) {
        /* Pointer @destroy: arena_release, else registered destroy(name). */
        char init[288];
        char arena[128];
        char keyed[160];
        const char* reg = NULL;
        const char* elem = st->a;
        init[0] = 0;
        if (st->c[0]) {
            shadow_emit_expr_text(st, st->c, init, sizeof(init), elem);
            shadow_resolve_at_create(init, sizeof(init), ty, is_ptr, 0);
        }
        if (!bare) {
            for (int k = 0; k < ndbody && ok; k++) {
                if (!shadow_dbody_is_stmt(dbody[k])) continue;
                if (!shadow_emit_stmt_ctx(dbody[k], out, ctx, body_ind, 0))
                    ok = 0;
            }
            for (int k = 0; k < nbody && ok; k++) {
                if (!shadow_emit_stmt_ctx(body[k], out, ctx, body_ind, 0)) ok = 0;
            }
        }
        if (ok && init[0] &&
            shadow_arena_from_alloc_init(init, arena, sizeof(arena))) {
            if (!cemit_fmt(out, "%s(void)cc_arena_release(%s, %s);\n", body_ind,
                           arena, name))
                ok = 0;
        } else if (ok && bare) {
            /* Prefer "Ty*" key (cc_type_register("Widget*", …)). */
            snprintf(keyed, sizeof(keyed), "%s*", ty);
            reg = shadow_destroy_hook_for(keyed);
            if (!reg) reg = shadow_destroy_hook_for(ty);
            if (reg && reg[0]) {
                if (!cemit_fmt(out, "%s%s(%s);\n", body_ind, reg, name))
                    ok = 0;
            } else {
                fprintf(stderr,
                        "error: bodyless @destroy on '%s* %s' needs a "
                        "registered destroy hook\n",
                        ty, name);
                ok = 0;
            }
        }
        /* Non-bare: user @destroy { … } body already emitted above. */
    } else if (!bare && (ndbody || nbody)) {
        if (!cemit_fmt(out, "%s{\n", body_ind)) ok = 0;
        for (int k = 0; k < ndbody && ok; k++) {
            if (!shadow_dbody_is_stmt(dbody[k])) continue;
            if (!shadow_emit_stmt_ctx(dbody[k], out, ctx, nested, 0)) ok = 0;
        }
        for (int k = 0; k < nbody && ok; k++) {
            if (!shadow_emit_stmt_ctx(body[k], out, ctx, nested, 0)) ok = 0;
        }
        if (ok && !cemit_fmt(out, "%s}\n", body_ind)) ok = 0;
    } else if (st->kind == AST_VAL_DESTROY && bare) {
        /* Slice ownership markers — ABI-identical to CCSlice. */
        if (strcmp(ty, "CCSliceUnique") == 0 || strcmp(ty, "CCSliceShared") == 0 ||
            strcmp(ty, "CCSlice") == 0) {
            if (!cemit_fmt(out, "%scc_slice_destroy(&%s);\n", body_ind, name))
                ok = 0;
        } else if (shadow_ufn_exists("CCString_destroy") &&
                   strcmp(ty, "CCString") == 0) {
            if (!cemit_fmt(out, "%sCCString_destroy(&%s);\n", body_ind, name))
                ok = 0;
        } else {
            /* draft_as §3: registered deltas + recursive @as flatten. */
            char chain[768];
            size_t bo = 0;
            int emitted;
            char dhook[96];
            chain[0] = 0;
            emitted = shadow_as_destroy_append(chain, &bo, sizeof(chain), ty,
                                              name);
            if (!emitted) {
                snprintf(dhook, sizeof(dhook), "%s_destroy", ty);
                if (shadow_ufn_exists(dhook)) {
                    snprintf(chain, sizeof(chain), "%s(&%s); ", dhook, name);
                    emitted = 1;
                }
            }
            if (emitted) {
                /* Split `hook(&x); hook2(&x.f); ` into one stmt per line. */
                const char* p = chain;
                while (ok && *p) {
                    const char* semi = strchr(p, ';');
                    char call[192];
                    size_t n;
                    while (*p == ' ' || *p == '\t') p++;
                    if (!*p) break;
                    if (!semi) semi = p + strlen(p);
                    n = (size_t)(semi - p);
                    if (n >= sizeof(call)) n = sizeof(call) - 1;
                    memcpy(call, p, n);
                    call[n] = 0;
                    while (n > 0 && (call[n - 1] == ' ' || call[n - 1] == '\t'))
                        call[--n] = 0;
                    if (call[0] &&
                        !cemit_fmt(out, "%s%s;\n", body_ind, call))
                        ok = 0;
                    p = *semi ? semi + 1 : semi;
                }
            } else {
                fprintf(stderr,
                        "error: bodyless @destroy on '%s %s' needs a registered "
                        "destroy hook\n",
                        ty, name);
                ok = 0;
            }
        }
    }
    if (ok && hw_idx >= 0) {
        if (!cemit_fmt(out, "%s}\n", outer)) ok = 0;
    }
    if (ctx) ctx->goto_cleanup = saved_goto;
    return ok;
}

/* Scratch arena if any stmt (or destroy body) uses @string/@scratch. */
static int shadow_emit_scratch_if_needed(AstNode** kids, int nkids,
                                        AstNode** destroys, int ndestroys,
                                        CEmit* out, const char* indent) {
    int need = 0;
    int k, d;
    for (k = 0; k < nkids; k++)
        need |= shadow_stmt_uses_scratch(kids[k]);
    for (d = 0; d < ndestroys; d++) {
        AstNode* dn = destroys[d];
        for (k = 0; k < dn->nbody; k++)
            need |= shadow_stmt_uses_scratch(dn->body[k]);
        for (k = 0; k < dn->ndbody; k++)
            need |= shadow_stmt_uses_scratch(dn->dbody[k]);
    }
    if (!need) return 1;
    return cemit_fmt(out, "%sCC_ARENA_STACK(__cc_str_scratch, 1024);\n", indent);
}

static int shadow_emit_destroy_soft_vars(CEmit* out, const char* indent) {
    return cemit_fmt(out,
            "%sint __cc_retval = 0;\n"
            "%sint __cc_ret_set = 0;\n"
            "%sint __cc_defer_hw = 0;\n",
            indent, indent, indent);
}

/* After emitting a reached @destroy decl, bump the watermark. */
static int shadow_emit_hw_bump(CEmit* out, ShadowCtx* ctx, AstNode* st,
                              const char* indent) {
    int di = shadow_destroy_index(ctx, st);
    if (di < 0) return 1;
    return cemit_fmt(out, "%s__cc_defer_hw = %d;\n", indent, di + 1);
}

/* `__cc_cleanup:` + reverse destroys + soft return. */
static int shadow_emit_destroy_cleanup(CEmit* out, ShadowCtx* ctx,
                                      AstNode** destroys, int ndestroys,
                                      const char* indent) {
    if (!cemit_str(out, "__cc_cleanup:\n")) return 0;
    for (int d = ndestroys - 1; d >= 0; d--) {
        if (!shadow_emit_one_destroy(destroys[d], out, ctx, indent)) return 0;
    }
    return cemit_fmt(out,
            "%sif (__cc_ret_set) return __cc_retval;\n"
            "%sreturn 0;\n",
            indent, indent);
}


#include "pp_emit_async.h"
#include "pp_emit_tu.h"
