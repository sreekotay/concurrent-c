/* Emit: Result / bang / qmark / try_assign unwrap.
 * Included from pp_emit_stmt.cch. */
#pragma once

static int shadow_emit_try_call(CEmit* out, ShadowCtx* ctx, const char* indent,
                                const char* call, const char* site,
                                const char* bind, int discard_ok) {
    char i1[80], i2[80];
    AstNode* saved_eh = NULL;
    AstNode* matched = NULL;
    ShadowTplBind env[3];
    shadow_indent_nest(i1, sizeof(i1), indent, 1);
    shadow_indent_nest(i2, sizeof(i2), indent, 2);
    (void)discard_ok;
    /* Stacked @errhandler: dispatch by Result error type E when known. */
    if (ctx) {
        saved_eh = ctx->eh;
        matched = shadow_eh_for_call(ctx, call);
        if (matched) ctx->eh = matched;
        if (matched && matched->b[0]) bind = matched->b;
    }
    env[0] = (ShadowTplBind){ "indent", SHADOW_TPL_IDENT, indent };
    env[1] = (ShadowTplBind){ "i1", SHADOW_TPL_IDENT, i1 };
    env[2] = (ShadowTplBind){ "call", SHADOW_TPL_EXPR, call };
    if (!shadow_tpl_emit(out, k_tpl_try_call_open, env, 3)) {
        if (ctx) ctx->eh = saved_eh;
        return 0;
    }
    /* Resync so __FILE__/__LINE__ on the next line are the source site. */
    if (!shadow_resync_line(out, ctx, i2) ||
        !shadow_emit_ptr_err_at_bind(out, ctx, i2, bind, site) ||
        !shadow_emit_handler(out, ctx, bind, i2)) {
        if (ctx) ctx->eh = saved_eh;
        return 0;
    }
    if (ctx) ctx->eh = saved_eh;
    return shadow_tpl_emit(out, k_tpl_try_call_close, env, 3);
}

/* Fallible call → typed local (typed-template unwrap). */
static int shadow_emit_try_assign(CEmit* out, ShadowCtx* ctx, const char* indent,
                                  const char* ty_lhs, const char* name,
                                  const char* call, const char* site,
                                  const char* bind,
                                  const char* value_expr /* after ok */) {
    char i1[80], i2[80];
    CEmit errb = {0};
    ShadowTplBind env[8];
    shadow_indent_nest(i1, sizeof(i1), indent, 1);
    shadow_indent_nest(i2, sizeof(i2), indent, 2);
    /* Prefer .ok / field value — _Generic unwrap macros omit many Result arms.
     * value_expr is `__r.u.value` or a post-unwrap UFCS chain (bang_chain). */
    if (!value_expr || !value_expr[0]) value_expr = "__r.u.value";
    if (!shadow_resync_line(&errb, ctx, i2) ||
        !shadow_emit_err_at_bind(&errb, ctx, i2, bind, site) ||
        !shadow_emit_handler(&errb, ctx, bind, i2)) {
        free(errb.buf);
        return 0;
    }
    env[0] = (ShadowTplBind){ "indent", SHADOW_TPL_IDENT, indent };
    env[1] = (ShadowTplBind){ "i1", SHADOW_TPL_IDENT, i1 };
    env[2] = (ShadowTplBind){ "i2", SHADOW_TPL_IDENT, i2 };
    env[3] = (ShadowTplBind){ "ty_lhs", SHADOW_TPL_IDENT, ty_lhs };
    env[4] = (ShadowTplBind){ "name", SHADOW_TPL_IDENT, name };
    env[5] = (ShadowTplBind){ "call", SHADOW_TPL_EXPR, call };
    env[6] = (ShadowTplBind){ "value_expr", SHADOW_TPL_EXPR, value_expr };
    env[7] = (ShadowTplBind){ "err_body", SHADOW_TPL_STMTS,
                              errb.buf ? errb.buf : "" };
    if (!shadow_tpl_emit(out, k_tpl_try_assign, env, 8)) {
        free(errb.buf);
        return 0;
    }
    free(errb.buf);
    return 1;
}

static int shadow_emit_println_arg(CEmit* out, ShadowCtx* ctx, const char* arg_expr,
                                   const char* indent, int is_eprint) {
    char call[320];
    const char* fn = is_eprint ? "cc_eprintln" : "cc_println";
    snprintf(call, sizeof(call), "%s(%s)", fn, arg_expr);
    return shadow_emit_try_call(out, ctx, indent, call, fn, "e", 1);
}

static int shadow_emit_println(AstNode* st, CEmit* out, ShadowCtx* ctx,
                               const char* indent) {
    /* Bare `println(...);` — discard Result (no !>/handler). */
    if (st && strcmp(st->e, "bare") == 0) {
        const char* fn = st->d[0] == 'e' ? "cc_eprintln" : "cc_println";
        return shadow_tpl_kv(out, "${indent}(void)${fn}(${args});\n",
                             "indent", indent, "fn", fn, "args", st->a, NULL);
    }
    return shadow_emit_println_arg(out, ctx, st->a, indent, st->d[0] == 'e');
}

static int shadow_emit_println_tpl(AstNode* st, CEmit* out, ShadowCtx* ctx,
                                   const char* indent) {
    int is_eprint = st->d[0] == 'e';
    const char* fn = is_eprint ? "cc_eprintln" : "cc_println";
    const char* arena = (st->c[0]) ? st->c : "&__cc_str_scratch";
    char nested[80];
    (void)ctx;
    shadow_indent_nest(nested, sizeof(nested), indent, 1);
    {
        if (!cemit_fmt(out, "%s{\n", indent)) return 0;
        if (!shadow_emit_tpl_build(out, st->a, arena, nested, "__msg"))
            return 0;
    }
    char call[320];
    snprintf(call, sizeof(call), "%s(__msg)", fn);
    if (!shadow_emit_try_call(out, ctx, nested, call, fn, "e", 1))
        return 0;
    return cemit_fmt(out, "%s}\n", indent);
}

static int shadow_emit_bang_bind(AstNode* st, CEmit* out, ShadowCtx* ctx,
                                 const char* indent) {
    char call[320];
    char i1[80], i2[80];
    shadow_indent_nest(i1, sizeof(i1), indent, 1);
    shadow_indent_nest(i2, sizeof(i2), indent, 2);
    snprintf(call, sizeof(call), "cc_println(%s)", st->a);
    if (!cemit_fmt(out,
            "%s{\n"
            "%s__typeof__(%s) __r = %s;\n"
            "%sif (__cc_uw_is_err(__r)) {\n",
            indent, i1, call, call, i1))
        return 0;
    if (!shadow_resync_line(out, ctx, i2)) return 0;
    if (!shadow_emit_ptr_err_at_bind(out, ctx, i2, st->b, "cc_println"))
        return 0;
    for (int k = 0; k < st->nbody; k++) {
        AstNode* b = st->body[k];
        if (b->kind == AST_PRINTLN_BANG) {
            if (!shadow_emit_println(b, out, ctx, i2)) return 0;
        } else if (b->kind == AST_ERR_FWD) {
            if (!shadow_emit_handler(out, ctx, b->a, i2)) return 0;
        } else {
            fprintf(stderr, "error: !>(...) body stmt %s not lowerable yet\n",
                    ast_kind_name(b->kind));
            out->err = 1;
            return 0;
        }
    }
    return cemit_fmt(out, "%s}\n%s}\n", i1, indent);
}

/* Lower Concurrent-C leftovers inside a variant `?>` fallback blob so the
 * embedded statement-expression is host C (char[:], UFCS, Result `!>`). */
static void shadow_lower_qmark_fallback_text(char* expr, size_t cap) {
    char cur[2048];
    char nxt[2048];
    int guard;
    if (!expr || !cap || !expr[0]) return;
    snprintf(cur, sizeof(cur), "%s", expr);
    /* `char[:]` surface → CCSlice (matches oracle / runtime). */
    {
        char* p = cur;
        char* o = nxt;
        size_t rem = sizeof(nxt) - 1;
        nxt[0] = 0;
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
        snprintf(cur, sizeof(cur), "%s", nxt);
    }
    shadow_rewrite_variant_expr(cur, sizeof(cur), NULL);
    shadow_emit_text_ufcs(nxt, sizeof(nxt), cur, NULL);
    snprintf(cur, sizeof(cur), "%s", nxt);
    /* `EXPR !> { … }` → Result statement-expression (inactive-handler form). */
    for (guard = 0; guard < 8; guard++) {
        const char* bang;
        const char* after;
        const char* body_open;
        const char* body_end;
        int ls, le, dep;
        size_t clen, blen;
        char piece[1536];
        char out[2048];
        bang = strstr(cur, "!>");
        if (!bang) break;
        after = bang + 2;
        while (*after == ' ' || *after == '\t' || *after == '\n' ||
               *after == '\r')
            after++;
        if (*after != '{') break;
        body_open = after;
        dep = 0;
        body_end = body_open;
        do {
            if (*body_end == '{') dep++;
            else if (*body_end == '}') dep--;
            body_end++;
        } while (*body_end && dep > 0);
        if (dep != 0) break;
        le = (int)(bang - cur);
        ls = shadow_expr_lhs_start(cur, le);
        if (ls < 0) break;
        clen = (size_t)(le - ls);
        while (clen && (cur[ls + (int)clen - 1] == ' ' ||
                        cur[ls + (int)clen - 1] == '\t' ||
                        cur[ls + (int)clen - 1] == '\n'))
            clen--;
        blen = (size_t)(body_end - body_open);
        if (!clen || clen >= 400 || blen >= 800) break;
        if (snprintf(piece, sizeof(piece),
                     "({ __typeof__(%.*s) __r = (%.*s); if (!__r.ok) %.*s "
                     "__r.u.value; })",
                     (int)clen, cur + ls, (int)clen, cur + ls, (int)blen,
                     body_open) >= (int)sizeof(piece))
            break;
        if (snprintf(out, sizeof(out), "%.*s%s%s", ls, cur, piece, body_end) >=
            (int)sizeof(out))
            break;
        snprintf(cur, sizeof(cur), "%s", out);
    }
    if (strlen(cur) < cap) snprintf(expr, cap, "%s", cur);
}

static int shadow_mode_is_bang_chain(const char* mode) {
    return mode && (strcmp(mode, "bang_chain") == 0 ||
                    strncmp(mode, "bang_chain:", 11) == 0);
}

static const char* shadow_bang_chain_lhs_ty(const char* mode) {
    if (mode && strncmp(mode, "bang_chain:", 11) == 0 && mode[11])
        return mode + 11;
    return NULL;
}

static int shadow_emit_var_unwrap(AstNode* st, CEmit* out, ShadowCtx* ctx,
                                  const char* indent) {
    const char* call = st->b;
    const char* mode = st->c;
    /* Variant `arm ?>` before expr lowering rewrites projection to `.u.arm`. */
    if ((strcmp(mode, "qmark") == 0 || strncmp(mode, "qmark", 5) == 0) && st->b[0]) {
        char qbuf[2048];
        char fb[1024];
        const char* qty = (strcmp(mode, "qmark") == 0 && st->d[0]) ? st->d : "int";
        snprintf(fb, sizeof(fb), "%s", st->e);
        shadow_lower_qmark_fallback_text(fb, sizeof(fb));
        snprintf(qbuf, sizeof(qbuf), "%s ?> %s", st->b, fb);
        if (shadow_rewrite_variant_qmark(qbuf, sizeof(qbuf))) {
            if (ctx && ctx->rname && ctx->rname[0])
                shadow_rewrite_result_ctors(qbuf, sizeof(qbuf), ctx->rname);
            return cemit_fmt(out, "%s%s %s = %s;\n", indent, qty, st->a, qbuf);
        }
    }
    /* Variant `arm !> {…}` — inactive-arm handler, not Result unwrap. */
    if ((strncmp(mode, "bang_stmt", 9) == 0 ||
         strncmp(mode, "bang_block", 10) == 0) &&
        st->b[0]) {
        char base[128], arm[64];
        int is_arrow = 0;
        ShadowVariant* v =
            shadow_variant_parse_proj(st->b, base, sizeof(base), arm,
                                     sizeof(arm), &is_arrow);
        if (v) {
            const char* ty = st->e[0] ? st->e : "int";
            const char* acc = is_arrow ? "->" : ".";
            char i1[80], i2[80];
            shadow_indent_nest(i1, sizeof(i1), indent, 1);
            shadow_indent_nest(i2, sizeof(i2), indent, 2);
            if (v->is_packed) {
                if (is_arrow) {
                    if (!cemit_fmt(out,
                            "%s%s %s = ({\n"
                            "%sif (%s__cc_kind(%s) != %s_%s) {\n",
                            indent, ty, st->a, i1, v->name, base, v->name,
                            arm))
                        return 0;
                } else {
                    if (!cemit_fmt(out,
                            "%s%s %s = ({\n"
                            "%sif (%s__cc_kind(&(%s)) != %s_%s) {\n",
                            indent, ty, st->a, i1, v->name, base, v->name,
                            arm))
                        return 0;
                }
            } else {
                if (!cemit_fmt(out,
                        "%s%s %s = ({\n"
                        "%sif ((%s)%skind != %s_%s) {\n",
                        indent, ty, st->a, i1, base, acc, v->name, arm))
                    return 0;
            }
            if (strcmp(mode, "bang_stmt") == 0) {
                if (st->nbody > 0 &&
                    !shadow_emit_stmt_ctx(st->body[0], out, ctx, i2, 0))
                    return 0;
            } else {
                int k;
                for (k = 0; k < st->nbody; k++) {
                    if (!shadow_emit_stmt_ctx(st->body[k], out, ctx, i2, 0))
                        return 0;
                }
            }
            if (v->is_packed) {
                if (is_arrow)
                    return cemit_fmt(out,
                            "%s}\n"
                            "%s%s__cc_get_%s(%s);\n"
                            "%s});\n",
                            i1, i1, v->name, arm, base, indent);
                return cemit_fmt(out,
                        "%s}\n"
                        "%s%s__cc_get_%s(&(%s));\n"
                        "%s});\n",
                        i1, i1, v->name, arm, base, indent);
            }
            return cemit_fmt(out,
                    "%s}\n"
                    "%s(%s)%su.%s;\n"
                    "%s});\n",
                    i1, i1, base, acc, arm, indent);
        }
    }
    /* e holds lhs type for bang/bang_stmt/bang_block; bang_chain parks
     * the type on mode (`bang_chain:CCSlice`) because e is the chain. */
    const char* ty = "int";
    if ((strcmp(mode, "bang") == 0 || strncmp(mode, "bang_stmt", 9) == 0 ||
         strncmp(mode, "bang_block", 10) == 0) &&
        st->e[0])
        ty = st->e;
    else if (shadow_mode_is_bang_chain(mode) && shadow_bang_chain_lhs_ty(mode))
        ty = shadow_bang_chain_lhs_ty(mode);
    /* Large: unwrap call may embed @string(`…`) / @slice("…") args. */
    char callbuf[8192];
    /* bang_chain Result check must see the SOURCE call (before UFCS rewrite
     * turns `m.get("r")` into `cc_py_obj_callm(...)`). */
    const char* src_call = call ? call : "";
    if (shadow_emit_channel_pair_expr(callbuf, sizeof(callbuf), call))
        call = callbuf;
    else {
        /* Pass lhs ty so .ufcs_dynamic2 can pick cc_py_obj_callm_<dest>. */
        shadow_emit_expr_text(st, call, callbuf, sizeof(callbuf),
                              (ty && ty[0] && strcmp(ty, "int") != 0) ? ty
                                                                      : NULL);
        /* Belt-and-suspenders: UFCS early-return paths historically skipped
         * @slice/@string rewrite; always normalize before host C. */
        shadow_rewrite_print_and_string(callbuf, sizeof(callbuf));
        {
            const char* pat = "__cc_tpl; }).as_slice()";
            const char* repl = "cc_string_as_slice(&__cc_tpl); })";
            char* hit;
            while ((hit = strstr(callbuf, pat)) != NULL) {
                char tmp[8192];
                size_t pre = (size_t)(hit - callbuf);
                if (pre + strlen(repl) + strlen(hit + strlen(pat)) + 1 >=
                    sizeof(tmp))
                    break;
                snprintf(tmp, sizeof(tmp), "%.*s%s%s", (int)pre, callbuf, repl,
                         hit + strlen(pat));
                snprintf(callbuf, sizeof(callbuf), "%s", tmp);
            }
        }
        shadow_rewrite_at_slice(callbuf, sizeof(callbuf));
        shadow_rewrite_generic_types_text(callbuf, sizeof(callbuf));
        call = callbuf;
    }
    if (strcmp(mode, "bang") == 0 || shadow_mode_is_bang_chain(mode)) {
        const char* val = "(__r).u.value";
        char chained[512];
        /* bang_chain: producer must be typable as a Result (declared fn /
         * Result spelling). Undeclared ghosts fail loud — never unwrap. */
        if (shadow_mode_is_bang_chain(mode)) {
            char callee[64];
            size_t ni = 0;
            const char* p = src_call;
            int ok = 0;
            while (*p == ' ' || *p == '\t' || *p == '(') p++;
            while (((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') ||
                    (*p >= '0' && *p <= '9') || *p == '_') &&
                   ni + 1 < sizeof(callee))
                callee[ni++] = *p++;
            callee[ni] = 0;
            if (strstr(src_call, "CCResult_") || strstr(call, "CCResult_"))
                ok = 1;
            /* UFCS hop `recv.meth(...)` / `recv->meth(...)` — Result-ness is
             * the method's; do not require the receiver ident to be a fn. */
            else if (strchr(src_call, '.') != NULL ||
                     strstr(src_call, "->") != NULL)
                ok = 1;
            else if (callee[0] &&
                     (strncmp(callee, "cc_ok", 5) == 0 ||
                      strncmp(callee, "cc_err", 6) == 0 ||
                      strncmp(callee, "cc_channel_", 11) == 0 ||
                      strncmp(callee, "cc_chan_", 8) == 0 ||
                      strncmp(callee, "cc_py_", 6) == 0 ||
                      shadow_ufn_exists(callee)))
                ok = 1;
            else if (call[0] && shadow_ufn_exists(call))
                ok = 1;
            if (!ok) {
                const char* path = NULL;
                int line = 0;
                if (shadow_site_loc(ctx ? ctx->cache : NULL, st, &path, &line) &&
                    path) {
                    fprintf(stderr,
                            "%s:%d:1: error: type: '!>' links a chain hop here, "
                            "but the hop's producer could not be typed as a "
                            "Result\n",
                            path, line);
                } else {
                    fprintf(stderr,
                            "error: type: '!>' links a chain hop here, but the "
                            "hop's producer could not be typed as a Result\n");
                }
                g_shadow_ufcs_miss = 1;
                out->err = 1;
                return 0;
            }
        }
        /* Form P: bare `!>;` at expression position needs @errhandler. */
        if (!ctx || !ctx->eh) {
            const char* path = NULL;
            int line = 0;
            if (shadow_site_loc(ctx ? ctx->cache : NULL, st, &path, &line) &&
                path) {
                fprintf(stderr,
                        "%s:%d:1: error: syntax: '!>;' at expression position "
                        "requires an enclosing '@errhandler' in scope\n",
                        path, line);
            } else {
                fprintf(stderr,
                        "error: syntax: '!>;' at expression position requires "
                        "an enclosing '@errhandler' in scope\n");
            }
            out->err = 1;
            return 0;
        }
        if (shadow_mode_is_bang_chain(mode) && st->e[0]) {
            /* Multi-hop: `.a()!>.b()!.c()` → nested Result unwraps + final UFCS.
             * Optional first-hop handler body (from `!>(e){…}.next()!>`). */
            if (strstr(st->e, "!>") || st->nbody > 0) {
                char i1[80], i2[80];
                const char* p = st->e;
                int hop = 0;
                const char* bind0 = st->d[0] ? st->d : "e";
                shadow_indent_nest(i1, sizeof(i1), indent, 1);
                shadow_indent_nest(i2, sizeof(i2), indent, 2);
                if (!cemit_fmt(out, "%s%s %s;\n%s{\n", indent, ty, st->a,
                               indent))
                    return 0;
                if (!cemit_fmt(out, "%s__typeof__(%s) __r0 = %s;\n", i1, call,
                               call))
                    return 0;
                if (!cemit_fmt(out, "%sif (!__r0.ok) {\n", i1)) return 0;
                if (!shadow_resync_line(out, ctx, i2)) return 0;
                if (st->nbody > 0) {
                    if (!cemit_fmt(out,
                                   "%s__typeof__(__r0.u.error) %s = "
                                   "__r0.u.error;\n",
                                   i2, bind0))
                        return 0;
                    {
                        int k;
                        for (k = 0; k < st->nbody; k++) {
                            if (!shadow_emit_stmt_ctx(st->body[k], out, ctx, i2,
                                                      0))
                                return 0;
                        }
                    }
                } else {
                    if (!shadow_emit_err_at_bind_tmp(out, ctx, i2, "e", "unwrap",
                                                     "__r0"))
                        return 0;
                    if (!shadow_emit_handler(out, ctx, "e", i2)) return 0;
                }
                if (!cemit_fmt(out, "%s}\n", i1)) return 0;
                while (*p) {
                    char hop_txt[256];
                    char recv[320];
                    char lowered[1024];
                    char rtmp[16];
                    const char* bang;
                    size_t n;
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
                    if (n >= sizeof(hop_txt)) n = sizeof(hop_txt) - 1;
                    memcpy(hop_txt, p, n);
                    hop_txt[n] = 0;
                    if (hop == 0)
                        snprintf(recv, sizeof(recv), "((__r0).u.value)%s",
                                 hop_txt);
                    else
                        snprintf(recv, sizeof(recv), "((__r%d).u.value)%s", hop,
                                 hop_txt);
                    /* Prefer a bound/receiver-shaped hint over the lhs ty so
                     * hops like `m.get("r")!>.as_i64()!>` lower via CCPyObj
                     * (not inventing `cc_<lhs>_as_i64`). Scalar lhs still
                     * works for `.step` / `.as_int` when no bind matches. */
                    {
                        const char* hop_ty = ty;
                        if (strstr(recv, "__r") &&
                            (strstr(hop_txt, "as_i64") ||
                             strstr(hop_txt, "as_slice") ||
                             strstr(hop_txt, "as_f64") ||
                             strstr(hop_txt, "as_bool") ||
                             strstr(hop_txt, "as_cstr")))
                            hop_ty = "CCPyObj";
                        else if (!strstr(ty, "CCPy") &&
                                 (strstr(call, "cc_py_") ||
                                  strstr(call, "CCPyObj") ||
                                  strstr(src_call, ".get(") ||
                                  strstr(src_call, "->get(")))
                            hop_ty = "CCPyObj";
                        shadow_emit_text_ufcs(lowered, sizeof(lowered), recv,
                                              hop_ty);
                    }
                    if (fallible) {
                        snprintf(rtmp, sizeof(rtmp), "__r%d", hop + 1);
                        if (!cemit_fmt(out,
                                       "%s__typeof__(%s) %s = %s;\n"
                                       "%sif (!%s.ok) {\n",
                                       i1, lowered, rtmp, lowered, i1, rtmp))
                            return 0;
                        if (!shadow_resync_line(out, ctx, i2)) return 0;
                        if (!shadow_emit_err_at_bind_tmp(out, ctx, i2, "e",
                                                         "unwrap", rtmp))
                            return 0;
                        if (!shadow_emit_handler(out, ctx, "e", i2)) return 0;
                        if (!cemit_fmt(out, "%s}\n", i1)) return 0;
                        hop++;
                        p = bang + 2;
                    } else {
                        if (!cemit_fmt(out, "%s%s = %s;\n%s}\n", i1, st->a,
                                       lowered, indent))
                            return 0;
                        return 1;
                    }
                }
                if (!cemit_fmt(out, "%s%s = __r%d.u.value;\n%s}\n", i1, st->a,
                               hop, indent))
                    return 0;
                return 1;
            }
            char tmp[320];
            const char* hop_ty = NULL;
            snprintf(tmp, sizeof(tmp), "((__r).u.value)%s", st->e);
            if (strstr(st->e, "as_i64") || strstr(st->e, "as_slice") ||
                strstr(st->e, "as_f64") || strstr(st->e, "as_bool") ||
                strstr(st->e, "as_cstr") || strstr(call, "cc_py_") ||
                strstr(src_call, ".get(") || strstr(src_call, "->get("))
                hop_ty = "CCPyObj";
            shadow_emit_text_ufcs(chained, sizeof(chained), tmp, hop_ty);
            val = chained;
        }
        /* Multi-declarator Form-P: `int a = f() !>, b = 3;` — d holds `, b = 3`. */
        if (st->d[0] == ',') {
            if (!shadow_emit_try_assign(out, ctx, indent, ty, st->a, call,
                                        "unwrap", "e", val))
                return 0;
            return cemit_fmt(out, "%s%s %s;\n", indent, ty, st->d + 1);
        }
        return shadow_emit_try_assign(out, ctx, indent, ty, st->a, call,
                                      "unwrap", "e", val);
    }
    if (strncmp(mode, "bang_stmt", 9) == 0 ||
        strncmp(mode, "bang_block", 10) == 0) {
        const char* bind = st->d[0] ? st->d : "e";
        char i1[80], i2[80];
        ShadowTplBind env[7];
        shadow_indent_nest(i1, sizeof(i1), indent, 1);
        shadow_indent_nest(i2, sizeof(i2), indent, 2);
        /* Direct Result field access — works for http/net Results not listed
         * in the host-cc _Generic unwrap macros. */
        env[0] = (ShadowTplBind){ "indent", SHADOW_TPL_IDENT, indent };
        env[1] = (ShadowTplBind){ "i1", SHADOW_TPL_IDENT, i1 };
        env[2] = (ShadowTplBind){ "i2", SHADOW_TPL_IDENT, i2 };
        env[3] = (ShadowTplBind){ "ty_lhs", SHADOW_TPL_IDENT, ty };
        env[4] = (ShadowTplBind){ "name", SHADOW_TPL_IDENT, st->a };
        env[5] = (ShadowTplBind){ "call", SHADOW_TPL_EXPR, call };
        env[6] = (ShadowTplBind){ "bind", SHADOW_TPL_IDENT, bind };
        if (!shadow_tpl_emit(out, k_tpl_bang_assign_open, env, 7)) return 0;
        if (!shadow_resync_line(out, ctx, i2)) return 0;
        {
            int saved_via = ctx ? ctx->err_via_bang : 0;
            if (ctx) ctx->err_via_bang = 1;
            if (strncmp(mode, "bang_stmt", 9) == 0 &&
                !shadow_mode_has_destroy(mode)) {
                if (st->nbody > 0 &&
                    !shadow_emit_stmt_ctx(st->body[0], out, ctx, i2, 0)) {
                    if (ctx) ctx->err_via_bang = saved_via;
                    return 0;
                }
            } else {
                for (int k = 0; k < st->nbody; k++) {
                    if (!shadow_emit_stmt_ctx(st->body[k], out, ctx, i2, 0)) {
                        if (ctx) ctx->err_via_bang = saved_via;
                        return 0;
                    }
                }
            }
            if (ctx) ctx->err_via_bang = saved_via;
        }
        return shadow_tpl_emit(out, k_tpl_bang_assign_close, env, 7);
    }
    if (strcmp(mode, "qmark") == 0) {
        char i1[80];
        const char* qty = st->d[0] ? st->d : "int";
        shadow_indent_nest(i1, sizeof(i1), indent, 1);
        return cemit_fmt(out,
            "%s%s %s;\n"
            "%s{\n"
            "%s__typeof__(%s) __r = %s;\n"
            "%s%s = !__cc_uw_is_err(__r) ? __cc_uw_value(__r) : (%s);\n"
            "%s}\n",
            indent, qty, st->a, indent, i1, call, call, i1, st->a, st->e, indent);
    }
    if (strcmp(mode, "qmark_bind") == 0 ||
        strncmp(mode, "qmark_bind:", 11) == 0) {
        char i1[80], i2[80];
        const char* qty =
            (strncmp(mode, "qmark_bind:", 11) == 0 && mode[11]) ? mode + 11
                                                               : "int";
        shadow_indent_nest(i1, sizeof(i1), indent, 1);
        shadow_indent_nest(i2, sizeof(i2), indent, 2);
        if (!cemit_fmt(out,
                "%s%s %s;\n"
                "%s{\n"
                "%s__typeof__(%s) __r = %s;\n"
                "%sif (__r.ok) {\n"
                "%s%s = __r.u.value;\n"
                "%s} else {\n"
                "%s__typeof__(__r.u.error) %s = __r.u.error;\n",
                indent, qty, st->a, indent, i1, call, call, i1, i2, st->a, i1,
                i2, st->d))
            return 0;
        if (!shadow_resync_line(out, ctx, i2)) return 0;
        return cemit_fmt(out,
            "%s%s = (%s);\n"
            "%s}\n"
            "%s}\n",
            i2, st->a, st->e, i1, indent);
    }
    fprintf(stderr, "error: unknown unwrap mode %s\n", mode);
    out->err = 1;
    return 0;
}

static int shadow_lhs_is_result_decl(const char* lhs) {
    return lhs && lhs[0] && strstr(lhs, "!>") != NULL;
}

static int shadow_lhs_is_plain_decl(const char* lhs) {
    const char* p;
    if (!lhs || !lhs[0] || shadow_lhs_is_result_decl(lhs)) return 0;
    /* Field/index lvalues are not decls even if span_text inserts spaces. */
    if (strchr(lhs, '.') || strchr(lhs, '[') || strchr(lhs, '>')) return 0;
    p = lhs;
    while (*p == ' ' || *p == '\t') p++;
    if (!*p) return 0;
    while (*p && *p != ' ' && *p != '\t') p++;
    while (*p == ' ' || *p == '\t') p++;
    return *p != 0;
}

static void shadow_err_assignee_from_lhs(const char* lhs, char* out, size_t cap) {
    if (!out || !cap) return;
    out[0] = 0;
    if (!lhs || !lhs[0]) return;
    if (strstr(lhs, "!>")) {
        const char* p = strstr(lhs, "!>");
        if (!p) return;
        p += 2;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '(') {
            p = strchr(p, ')');
            if (p) p++;
        }
        while (*p == ' ' || *p == '\t') p++;
        const char* start = p;
        while (*p && *p != ' ' && *p != '\t' && *p != '[') p++;
        size_t n = (size_t)(p - start);
        if (n >= cap) n = cap - 1;
        memcpy(out, start, n);
        out[n] = 0;
    } else if (shadow_lhs_is_plain_decl(lhs)) {
        /* `bool avail` — trailing binder name. */
        const char* end = lhs + strlen(lhs);
        const char* p;
        size_t n;
        while (end > lhs && (end[-1] == ' ' || end[-1] == '\t')) end--;
        p = end;
        while (p > lhs &&
               ((p[-1] >= 'A' && p[-1] <= 'Z') || (p[-1] >= 'a' && p[-1] <= 'z') ||
                (p[-1] >= '0' && p[-1] <= '9') || p[-1] == '_'))
            p--;
        n = (size_t)(end - p);
        if (n >= cap) n = cap - 1;
        memcpy(out, p, n);
        out[n] = 0;
    } else {
        /* Field / index / bare var — keep full lhs (incl. `items[ idx ]`). */
        snprintf(out, cap, "%s", lhs);
    }
}

static int shadow_parse_result_decl_lhs(const char* lhs, char* rname, size_t rncap,
                                        char* name, size_t ncap) {
    const char* bang;
    const char* after;
    const char* close;
    const char* start;
    char ok[64];
    char err[64];
    if (!lhs || !rname || !name || rncap == 0 || ncap == 0) return 0;
    bang = strstr(lhs, "!>");
    if (!bang) return 0;
    start = lhs;
    while (*start == ' ' || *start == '\t') start++;
    {
        size_t n = (size_t)(bang - start);
        while (n > 0 && (start[n - 1] == ' ' || start[n - 1] == '\t')) n--;
        if (n >= sizeof(ok)) n = sizeof(ok) - 1;
        memcpy(ok, start, n);
        ok[n] = 0;
    }
    after = bang + 2;
    while (*after == ' ' || *after == '\t') after++;
    if (*after == '(') {
        after++;
        close = strchr(after, ')');
        if (!close) return 0;
        {
            size_t n = (size_t)(close - after);
            while (n > 0 && (after[n - 1] == ' ' || after[n - 1] == '\t')) n--;
            if (n >= sizeof(err)) n = sizeof(err) - 1;
            memcpy(err, after, n);
            err[n] = 0;
        }
        after = close + 1;
    } else {
        const char* end = after;
        while (*end && *end != ' ' && *end != '\t' && *end != '[') end++;
        {
            size_t n = (size_t)(end - after);
            if (n >= sizeof(err)) n = sizeof(err) - 1;
            memcpy(err, after, n);
            err[n] = 0;
        }
        after = end;
    }
    while (*after == ' ' || *after == '\t') after++;
    start = after;
    while (*after && *after != ' ' && *after != '\t' && *after != '[') after++;
    {
        size_t n = (size_t)(after - start);
        if (n >= ncap) n = ncap - 1;
        memcpy(name, start, n);
        name[n] = 0;
    }
    shadow_result_name(ok, err, rname, rncap);
    return name[0] != 0;
}

static int shadow_emit_err_local(AstNode* st, CEmit* out, ShadowCtx* ctx,
                                 const char* indent, const char* bind) {
    if (st->nbody > 0) {
        for (int k = 0; k < st->nbody; k++) {
            if (!shadow_emit_stmt_ctx(st->body[k], out, ctx, indent, 0))
                return 0;
        }
        return 1;
    }
    return shadow_emit_handler(out, ctx, bind, indent);
}

static int shadow_emit_err_branch(AstNode* st, CEmit* out, ShadowCtx* ctx,
                                  const char* indent, const char* bind,
                                  const char* err_tmp) {
    char i2[80];
    shadow_indent_nest(i2, sizeof(i2), indent, 1);
    if (!err_tmp || !err_tmp[0]) err_tmp = "__r";
    if (!shadow_resync_line(out, ctx, i2)) return 0;
    if (st->nbody > 0 || st->d[0]) {
        if (st->d[0]) {
            char ty[128];
            char bname[64];
            const char* sp = strchr(st->d, ' ');
            if (sp) {
                size_t tl = (size_t)(sp - st->d);
                if (tl >= sizeof(ty)) tl = sizeof(ty) - 1;
                memcpy(ty, st->d, tl);
                ty[tl] = 0;
                snprintf(bname, sizeof(bname), "%s", sp + 1);
                if (!cemit_fmt(out, "%s%s %s = (%s).u.error;\n", i2, ty, bname,
                               err_tmp))
                    return 0;
                bind = bname;
            } else {
                if (!shadow_emit_ptr_err_at_bind(out, ctx, i2, st->d, "unwrap"))
                    return 0;
                bind = st->d;
            }
        } else if (!shadow_emit_ptr_err_at_bind(out, ctx, i2, bind, "unwrap"))
            return 0;
        return shadow_emit_err_local(st, out, ctx, i2, bind);
    }
    if (!shadow_emit_ptr_err_at_bind(out, ctx, i2, bind, "unwrap")) return 0;
    return shadow_emit_handler(out, ctx, bind, i2);
}

static int shadow_emit_err_assign_ok(CEmit* out, const char* indent,
                                     const char* lhs, const char* assignee,
                                     int lhs_result_decl, int lhs_plain_decl,
                                     const char* tmpv) {
    char i1[80];
    const char* val;
    shadow_indent_nest(i1, sizeof(i1), indent, 1);
    (void)lhs;
    if (lhs_plain_decl)
        val = "__cc_uw_value(__r)";
    else
        val = tmpv;
    return cemit_fmt(out, "%s} else {\n%s%s = %s;\n%s}\n%s}\n",
                     i1, i1, assignee, val, i1, indent);
}

static void shadow_subst_ident_word(char* buf, size_t cap,
                                    const char* from, const char* to) {
    char tmp[512];
    size_t fl, tl, bl, i, o;
    if (!buf || !cap || !from || !from[0] || !to) return;
    fl = strlen(from);
    tl = strlen(to);
    bl = strlen(buf);
    if (bl >= sizeof(tmp)) return;
    o = 0;
    for (i = 0; i < bl;) {
        if (i + fl <= bl && memcmp(buf + i, from, fl) == 0 &&
            (i == 0 ||
             !(isalnum((unsigned char)buf[i - 1]) || buf[i - 1] == '_')) &&
            (i + fl == bl ||
             !(isalnum((unsigned char)buf[i + fl]) || buf[i + fl] == '_'))) {
            if (o + tl >= cap) return;
            memcpy(tmp + o, to, tl);
            o += tl;
            i += fl;
        } else {
            tmp[o++] = buf[i++];
        }
    }
    tmp[o] = 0;
    snprintf(buf, cap, "%s", tmp);
}

static void shadow_subst_bind_on_node(AstNode* st, const char* from,
                                      const char* to) {
    if (!st || !from || !from[0] || !to) return;
    shadow_subst_ident_word(st->a, sizeof(st->a), from, to);
    shadow_subst_ident_word(st->b, sizeof(st->b), from, to);
    shadow_subst_ident_word(st->c, sizeof(st->c), from, to);
    shadow_subst_ident_word(st->d, sizeof(st->d), from, to);
    shadow_subst_ident_word(st->e, sizeof(st->e), from, to);
}

static int shadow_emit_err_delegate(CEmit* out, ShadowCtx* ctx,
                                    const char* local_bind,
                                    const char* indent) {
    AstNode eh_copy;
    int k;
    if (!ctx || !ctx->eh) {
        fprintf(stderr, "error: @errhandler delegation with no outer handler\n");
        out->err = 1;
        return 0;
    }
    if (ctx->eh->nbody == 0) {
        const char* h = ctx->eh->c[0] ? ctx->eh->c : "cc_error_exit";
        return cemit_fmt(out, "%s%s(%s);\n", indent, h, local_bind);
    }
    for (k = 0; k < ctx->eh->nbody; k++) {
        eh_copy = *ctx->eh->body[k];
        if (ctx->eh->b[0] && local_bind[0] &&
            strcmp(ctx->eh->b, local_bind) != 0)
            shadow_subst_bind_on_node(&eh_copy, ctx->eh->b, local_bind);
        if (!shadow_emit_stmt_ctx(&eh_copy, out, ctx, indent, 0))
            return 0;
    }
    return 1;
}

static int shadow_emit_err_syntax(AstNode* st, CEmit* out, ShadowCtx* ctx,
                                  const char* indent) {
    char callbuf[512];
    char defbuf[512];
    char assignee[128];
    char i1[80];
    const char* call = st->b;
    const char* bind = "e";
    int has_assign = st->a[0] != 0;
    int has_colon = strcmp(st->c, "colon") == 0;
    int lhs_result = has_assign && shadow_lhs_is_result_decl(st->a);
    int lhs_plain = has_assign && shadow_lhs_is_plain_decl(st->a);
    int lhs_decl = lhs_result || lhs_plain;

    shadow_emit_expr_text(st, call, callbuf, sizeof(callbuf), NULL);
    call = callbuf;
    shadow_indent_nest(i1, sizeof(i1), indent, 1);

    if (has_assign) {
        shadow_err_assignee_from_lhs(st->a, assignee, sizeof(assignee));
        if (lhs_result) {
            char rname[128];
            char vname[64];
            if (!shadow_parse_result_decl_lhs(st->a, rname, sizeof(rname),
                                              vname, sizeof(vname))) {
                fprintf(stderr, "error: malformed result decl lhs in @err assign\n");
                out->err = 1;
                return 0;
            }
            if (!cemit_fmt(out, "%s%s %s;\n", indent, rname, vname)) return 0;
            snprintf(assignee, sizeof(assignee), "%s", vname);
        } else if (lhs_plain) {
            if (!cemit_fmt(out, "%s%s;\n", indent, st->a)) return 0;
        }
    }

    if (has_colon) {
        shadow_emit_expr_text(st, st->e, defbuf, sizeof(defbuf), NULL);
        if (!cemit_fmt(out,
                "%s{\n"
                "%s__typeof__(%s) __r = %s;\n"
                "%sif (!__cc_uw_is_err(__r)) {\n",
                indent, i1, call, call, i1))
            return 0;
        if (has_assign) {
            const char* val = lhs_result ? "__r" :
                              lhs_plain ? "__cc_uw_value(__r)" : "__r";
            if (!cemit_fmt(out, "%s%s = %s;\n", i1, assignee, val)) return 0;
        }
        if (!cemit_fmt(out,
                "%s} else {\n"
                "%s__typeof__(%s) __r_d = %s;\n"
                "%sif (__cc_uw_is_err(__r_d)) {\n",
                i1, i1, defbuf, defbuf, i1))
            return 0;
        if (!shadow_emit_err_branch(st, out, ctx, i1, bind, "__r_d")) return 0;
        if (!cemit_fmt(out, "%s} else {\n", i1)) return 0;
        if (has_assign) {
            const char* val = lhs_plain ? "__cc_uw_value(__r_d)" : "__r_d";
            if (!cemit_fmt(out, "%s%s = %s;\n", i1, assignee, val)) return 0;
        }
        return cemit_fmt(out, "%s}\n%s}\n%s}\n", i1, i1, indent);
    }

    if (!cemit_fmt(out,
            "%s{\n"
            "%s__typeof__(%s) __r = %s;\n"
            "%sif (__cc_uw_is_err(__r)) {\n",
            indent, i1, call, call, i1))
        return 0;
    if (!shadow_emit_err_branch(st, out, ctx, i1, bind, "__r")) return 0;
    if (has_assign)
        return shadow_emit_err_assign_ok(out, indent, st->a, assignee,
                                         lhs_result, lhs_plain, "__r");
    return cemit_fmt(out, "%s} else { (void)__r; }\n%s}\n", i1, indent);
}

/* First `!>` outside strings/comments/ticks, or NULL. Avoids Form-P rewrite
 * re-entering on `"…!>;" ` prose (sigils smoke / flatten traps). */
static const char* shadow_find_bang_live(const char* s) {
    int in_line = 0, in_block = 0, in_dq = 0, in_sq = 0, in_bt = 0;
    if (!s) return NULL;
    for (; *s; s++) {
        char c = *s;
        if (in_line) {
            if (c == '\n') in_line = 0;
            continue;
        }
        if (in_block) {
            if (c == '*' && s[1] == '/') {
                in_block = 0;
                s++;
            }
            continue;
        }
        if (in_dq) {
            if (c == '\\' && s[1]) {
                s++;
                continue;
            }
            if (c == '"') in_dq = 0;
            continue;
        }
        if (in_sq) {
            if (c == '\\' && s[1]) {
                s++;
                continue;
            }
            if (c == '\'') in_sq = 0;
            continue;
        }
        if (in_bt) {
            if (c == '`') in_bt = 0;
            continue;
        }
        if (c == '/' && s[1] == '/') {
            in_line = 1;
            s++;
            continue;
        }
        if (c == '/' && s[1] == '*') {
            in_block = 1;
            s++;
            continue;
        }
        if (c == '"') {
            in_dq = 1;
            continue;
        }
        if (c == '\'') {
            in_sq = 1;
            continue;
        }
        if (c == '`') {
            in_bt = 1;
            continue;
        }
        if (c == '!' && s[1] == '>') return s;
    }
    return NULL;
}

/* Form P: bare `expr !>` before an infix/terminator → GNU stmt-expr value.
 * Matches production expression-position bang (leaves `!= 7` etc. in place).
 * Returns 1 if rewritten, 0 if no Form-P bang, -1 on hard failure. */
static int shadow_rewrite_one_bang_expr(char* expr, size_t cap, ShadowCtx* ctx) {
    char out[8192];
    const char* p;
    int op, ls, rs;
    const char* bind;
    CEmit hb = {0};
    int n;
    if (!expr || !cap) return 0;
    p = shadow_find_bang_live(expr);
    if (!p) return 0;
    op = (int)(p - expr);
    ls = shadow_expr_lhs_start(expr, op);
    if (ls < 0) return 0;
    rs = op + 2;
    while (expr[rs] == ' ' || expr[rs] == '\t' || expr[rs] == '\n') rs++;
    /* Binder / chain / statement-body — other emit paths. */
    if (expr[rs] == '(' || expr[rs] == '{' || expr[rs] == '.') return 0;
    if (expr[rs] && expr[rs] != ')' && expr[rs] != ',' && expr[rs] != ';' &&
        expr[rs] != ']' &&
        !(expr[rs] == '!' && expr[rs + 1] == '=') &&
        !(expr[rs] == '=' && expr[rs + 1] == '=') &&
        expr[rs] != '<' && expr[rs] != '>' &&
        !(expr[rs] == '&' && expr[rs + 1] == '&') &&
        !(expr[rs] == '|' && expr[rs + 1] == '|') &&
        expr[rs] != '+' && expr[rs] != '-' && expr[rs] != '*' &&
        expr[rs] != '/' && expr[rs] != '%' && expr[rs] != '?')
        return 0;
    if (!ctx || !ctx->eh) {
        fprintf(stderr,
                "error: syntax: '!>' at expression position requires an "
                "enclosing '@errhandler' in scope\n");
        return -1;
    }
    bind = ctx->eh->b[0] ? ctx->eh->b : "e";
    if (!shadow_emit_err_at_bind(&hb, ctx, "        ", bind, "unwrap") ||
        !shadow_emit_handler(&hb, ctx, bind, "        ")) {
        free(hb.buf);
        return -1;
    }
    n = snprintf(out, sizeof(out),
                 "%.*s({ __typeof__(%.*s) __r = (%.*s); if (!__r.ok) {\n%s"
                 "        } __r.u.value; })%s",
                 ls, expr, op - ls, expr + ls, op - ls, expr + ls,
                 hb.buf ? hb.buf : "", expr + rs);
    free(hb.buf);
    if (n < 0 || (size_t)n >= sizeof(out) || (size_t)n >= cap) {
        fprintf(stderr,
                "error: expression-position '!>' rewrite overflowed "
                "(need %d bytes, dest cap %zu)\n",
                n, cap);
        return -1;
    }
    snprintf(expr, cap, "%s", out);
    return 1;
}

static int shadow_rewrite_bang_exprs(char* expr, size_t cap, ShadowCtx* ctx) {
    int guard = 0;
    int rc;
    if (!expr || !cap) return 1;
    while (guard++ < 8) {
        rc = shadow_rewrite_one_bang_expr(expr, cap, ctx);
        if (rc < 0) return 0;
        if (rc == 0) break;
    }
    return 1;
}

