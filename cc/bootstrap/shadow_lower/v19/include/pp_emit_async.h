/* Emit: @async poll-task beachhead.
 * Included from pp_emit_stmt.cch. */
#pragma once

/* Replace whole-word param names with `__f->__p_<name>` (fiber-hot shape).
 * Skip `.name` / `->name` — Result bang lowers to `__r.u.value`, and a param
 * named `value` must not become `__r.u.__f->__p_value`. */
static void shadow_async_rewrite_params(char* text, size_t cap, ShadowParam* ps,
                                        int np) {
    char out[8192];
    size_t o = 0;
    const char* p;
    int i;
    if (!text || !cap || !ps || np <= 0) return;
    p = text;
    out[0] = 0;
    while (*p && o + 1 < sizeof(out)) {
        if (((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') ||
             *p == '_')) {
            const char* s = p;
            size_t n = 0;
            int hit = -1;
            int member = 0;
            while ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') ||
                   (*p >= '0' && *p <= '9') || *p == '_') {
                p++;
                n++;
            }
            if (s > text && s[-1] == '.')
                member = 1;
            else if (s > text + 1 && s[-2] == '-' && s[-1] == '>')
                member = 1;
            for (i = 0; i < np; i++) {
                size_t pl = strlen(ps[i].name);
                if (pl == n && memcmp(s, ps[i].name, n) == 0) {
                    hit = i;
                    break;
                }
            }
            if (hit >= 0 && !member) {
                int w = snprintf(out + o, sizeof(out) - o, "__f->__p_%s",
                                 ps[hit].name);
                if (w < 0 || (size_t)w >= sizeof(out) - o) return;
                o += (size_t)w;
            } else {
                if (o + n >= sizeof(out)) return;
                memcpy(out + o, s, n);
                o += n;
                out[o] = 0;
            }
        } else {
            out[o++] = *p++;
            out[o] = 0;
        }
    }
    out[o] = 0;
    if (o + 1 < cap) snprintf(text, cap, "%s", out);
}

static int shadow_emit_async_fn(AstNode* it, CEmit* out, TapeCache* cache) {
    char params[512];
    char base[160];
    char body_ind[64];
    ShadowParam ps[16];
    int np;
    int id;
    int is_void;
    int is_result_ret;
    int k;
    int bind_mark;
    AstNode* spawns[16];
    int nspawns = 0;
    if (!it || !out) return 0;
    snprintf(params, sizeof(params), "%s", it->c[0] ? it->c : "void");
    shadow_normalize_chan_params(params, sizeof(params));
    np = shadow_parse_params(params, ps, 16);
    id = g_shadow_async_id++;
    snprintf(base, sizeof(base), "__cc_async_%s_%d", it->b, id);
    is_void = (strcmp(it->a, "void") == 0);
    is_result_ret = (strncmp(it->a, "CCResult_", 9) == 0) ||
                    (it->e[0] && strcmp(it->e, "result") == 0);

    if (!cemit_fmt(out, "typedef struct %s_frame {\n", base)) return 0;
    if (!cemit_str(out, "  int __st;\n  intptr_t __r;\n")) return 0;
    for (k = 0; k < np; k++) {
        if (!cemit_fmt(out, "  %s __p_%s;\n", ps[k].ty, ps[k].name)) return 0;
    }
    if (!cemit_fmt(out, "  CCTaskIntptr __t[1];\n} %s_frame;\n\n", base))
        return 0;

    /* Params+locals in scope for closure formal/capture typing. */
    bind_mark = shadow_push_fn_binds(it);

    for (k = 0; k < it->nkids; k++) {
        if (!shadow_collect_spawns(it->kids[k], spawns, &nspawns, 16)) {
            fprintf(stderr, "error: too many spawns in @async for shadow\n");
            out->err = 1;
            shadow_pop_fn_binds(bind_mark);
            return 0;
        }
    }
    for (k = 0; k < nspawns; k++) shadow_infer_callarg2_send_into(spawns[k]);

    /* Closures spawned inside @async need fwd decls before the body helper. */
    if (nspawns) {
        if (!cemit_str(out, "\n/* --- CC closure declarations --- */\n")) {
            shadow_pop_fn_binds(bind_mark);
            return 0;
        }
        for (int s = 0; s < nspawns; s++) {
            char proto[256];
            int ar = shadow_closure_arity(spawns[s]);
            const char* ccty = shadow_closure_cc_ty(spawns[s]);
            if (!shadow_caps_proto(spawns[s]->e, proto, sizeof(proto))) {
                shadow_pop_fn_binds(bind_mark);
                return 0;
            }
            if (ar == 1) {
                if (!cemit_fmt(out,
                        "static void* cc_closure__N%s_entry(void*, intptr_t);\n"
                        "static %s cc_closure__N%s_make(%s);\n",
                        spawns[s]->d, ccty, spawns[s]->d, proto)) {
                    shadow_pop_fn_binds(bind_mark);
                    return 0;
                }
            } else if (ar == 2) {
                if (!cemit_fmt(out,
                        "static void* cc_closure__N%s_entry(void*, intptr_t, intptr_t);\n"
                        "static %s cc_closure__N%s_make(%s);\n",
                        spawns[s]->d, ccty, spawns[s]->d, proto)) {
                    shadow_pop_fn_binds(bind_mark);
                    return 0;
                }
            } else if (!cemit_fmt(out,
                    "static void* cc_closure__N%s_entry(void*);\n"
                    "static CCClosure0 cc_closure__N%s_make(%s);\n",
                    spawns[s]->d, spawns[s]->d, proto)) {
                shadow_pop_fn_binds(bind_mark);
                return 0;
            }
        }
        if (!cemit_str(out, "/* --- end closure declarations --- */\n\n")) {
            shadow_pop_fn_binds(bind_mark);
            return 0;
        }
    }
    /* Autoblock helpers at file scope (before body fn). */
    if (it->nkids > 0) {
        ShadowCtx prescan = { .cache = cache,
                              .owner_fn_attrs = shadow_fn_attr_lookup(it->b) };
        if (!shadow_emit_autoblock_prescan(it->kids, it->nkids, &prescan, out)) {
            shadow_pop_fn_binds(bind_mark);
            return 0;
        }
    }
    /* Body takes the frame so recv/send use `__f->__p_*` (shape oracle). */
    if (!cemit_fmt(out, "static %s %s_body(%s_frame* __f) {\n", it->a, base,
                   base)) {
        shadow_pop_fn_binds(bind_mark);
        return 0;
    }
    {
        int bind_ok = 1;
        CEmit body = {0};
        if (it->d[0] && it->nkids == 0) {
            char text[2048];
            shadow_rewrite_ufcs(text, sizeof(text), it->d, NULL);
            shadow_async_rewrite_params(text, sizeof(text), ps, np);
            if (!cemit_fmt(out, "%s\n}\n\n", text)) bind_ok = 0;
        } else {
            shadow_pick_body_indent(it->kids, it->nkids, body_ind,
                                    sizeof(body_ind));
            ShadowCtx sctx = { .cache = cache, .body_indent = body_ind };
            AstNode* defers[16];
            int ndefers = 0;
            for (k = 0; bind_ok && k < it->nkids; k++) {
                if (it->kids[k]->kind == AST_ERRHANDLER)
                    sctx.eh = it->kids[k];
                if (it->kids[k]->kind == AST_DEFER) {
                    if (ndefers >= 16) {
                        fprintf(stderr,
                                "error: too many @defer in @async body\n");
                        out->err = 1;
                        bind_ok = 0;
                        break;
                    }
                    defers[ndefers++] = it->kids[k];
                }
            }
            if (bind_ok) {
                sctx.defers = defers;
                sctx.ndefers = ndefers;
                sctx.defer_cleanup = ndefers > 0;
                sctx.soft_ret_ty = is_void ? "void" : (it->a[0] ? it->a : "int");
                sctx.owner_fn_attrs = shadow_fn_attr_lookup(it->b);
                if (sctx.defer_cleanup) {
                    if (is_void) {
                        if (!cemit_fmt(&body,
                                "%sint __cc_ret_set = 0;\n"
                                "%sint __cc_ret_err = 0;\n",
                                body_ind, body_ind))
                            bind_ok = 0;
                    } else if (!cemit_fmt(&body,
                            "%sint __cc_retval = 0;\n"
                            "%sint __cc_ret_set = 0;\n"
                            "%sint __cc_ret_err = 0;\n",
                            body_ind, body_ind, body_ind))
                        bind_ok = 0;
                }
                if (bind_ok &&
                    !shadow_emit_scratch_if_needed(it->kids, it->nkids, NULL, 0,
                                                   &body, body_ind))
                    bind_ok = 0;
                for (k = 0; bind_ok && k < it->nkids; k++) {
                    if (!shadow_emit_stmt_ctx(it->kids[k], &body, &sctx,
                                              body_ind, sctx.defer_cleanup))
                        bind_ok = 0;
                }
                if (bind_ok && sctx.defer_cleanup) {
                    if (!shadow_emit_defer_epilogue(&body, &sctx)) bind_ok = 0;
                }
                if (bind_ok && body.buf) {
                    /* Rewrites grow names (`req_rx` → `__f->__p_req_rx`). */
                    if (!cemit_reserve(&body, body.len + (size_t)np * 16 + 64))
                        bind_ok = 0;
                    else {
                        shadow_async_rewrite_params(body.buf, body.cap, ps, np);
                        body.len = strlen(body.buf);
                        if (!cemit_str(out, body.buf)) bind_ok = 0;
                    }
                }
                if (bind_ok && !cemit_str(out, "}\n\n")) bind_ok = 0;
            }
        }
        free(body.buf);
        if (!bind_ok) {
            shadow_pop_fn_binds(bind_mark);
            return 0;
        }
        /* Closure defs while param/local binds remain. */
        if (nspawns) {
            if (!cemit_str(out, "\n/* --- CC generated closures --- */\n")) {
                shadow_pop_fn_binds(bind_mark);
                return 0;
            }
            for (int s = 0; s < nspawns; s++) {
                if (!shadow_emit_closure_def(spawns[s], out, cache)) {
                    shadow_pop_fn_binds(bind_mark);
                    return 0;
                }
            }
            if (!cemit_str(out, "\n")) {
                shadow_pop_fn_binds(bind_mark);
                return 0;
            }
        }
        shadow_pop_fn_binds(bind_mark);
    }

    if (!cemit_fmt(out,
            "static CCFutureStatus %s_poll(void* __p, intptr_t* __o, int* __e) {\n"
            "  (void)__e;\n"
            "  %s_frame* __f = (%s_frame*)__p;\n"
            "  if (!__f) return CC_FUTURE_ERR;\n",
            base, base, base))
        return 0;
    if (!cemit_str(out,
            "  for (;;) {\n"
            "  switch (__f->__st) {\n"
            "    case 0:\n"
            "      __f->__st = 1;\n"
            "      /* fallthrough */\n"
            "    case 1: {\n"))
        return 0;
    if (is_void) {
        if (!cemit_fmt(out, "      %s_body(__f);\n      __f->__r = 0;\n", base))
            return 0;
    } else if (is_result_ret) {
        /* Pack Result on the heap — POLL tasks have no fiber result_buf.
         * Await wrap copies out and frees (see ast_spell await). */
        if (!cemit_fmt(out,
                "      {\n"
                "        %s __cc_ar = %s_body(__f);\n"
                "        %s* __cc_arp = (%s*)malloc(sizeof(*__cc_arp));\n"
                "        if (__cc_arp) *__cc_arp = __cc_ar;\n"
                "        __f->__r = (intptr_t)(void*)__cc_arp;\n"
                "      }\n",
                it->a, base, it->a, it->a))
            return 0;
    } else if (!cemit_fmt(out, "      __f->__r = (intptr_t)%s_body(__f);\n",
                          base)) {
        return 0;
    }
    if (!cemit_str(out,
            "      __f->__st = 999;\n"
            "      continue;\n"
            "    }\n"
            "    case 999: {\n"
            "      if (__o) *__o = __f->__r;\n"
            "      return CC_FUTURE_READY;\n"
            "    }\n"
            "    default:\n"
            "      return CC_FUTURE_ERR;\n"
            "  }\n"
            "  }\n"
            "}\n\n"))
        return 0;
    if (!cemit_fmt(out, "static void %s_drop(void* __p) {\n", base)) return 0;
    if (!cemit_fmt(out, "  %s_frame* __f = (%s_frame*)__p;\n", base, base))
        return 0;
    if (!cemit_str(out,
            "  if (!__f) return;\n"
            "  for (int __i = 0; __i < 1; __i++) {\n"
            "    cc_task_intptr_free(&__f->__t[__i]);\n"
            "  }\n"
            "  free(__f);\n"
            "}\n\n"))
        return 0;
    if (!cemit_fmt(out, "CCTaskIntptr %s(%s) {\n", it->b, params)) return 0;
    if (!cemit_fmt(out,
            "  %s_frame* __f = (%s_frame*)calloc(1, sizeof(%s_frame));\n",
            base, base, base))
        return 0;
    if (!cemit_str(out,
            "  if (!__f) {\n"
            "    CCTaskIntptr __t;\n"
            "    memset(&__t, 0, sizeof(__t));\n"
            "    return __t;\n"
            "  }\n"
            "  __f->__st = 0;\n"))
        return 0;
    for (k = 0; k < np; k++) {
        if (!cemit_fmt(out, "  __f->__p_%s = %s;\n", ps[k].name, ps[k].name))
            return 0;
    }
    if (!cemit_fmt(out,
            "  return cc_task_intptr_make_poll_ex(%s_poll, NULL, __f, %s_drop);\n"
            "}\n\n",
            base, base))
        return 0;
    return 1;
}
