/* UFCS lower: structured parts, leftover text peel, expr/cond helpers.
 * Peel is leftover-text only — not a beachhead pipeline.
 * Requires pp_emit_core.cch. */
#pragma once

/* Defined in pp_emit_stmt.cch / pp_emit_spawn.cch (after this include). */
static void shadow_rewrite_print_and_string(char* expr, size_t cap);
static AstNode* shadow_expr_closure_kid(AstNode* st);
static void shadow_splice_closure_arg(char* expr, size_t cap, AstNode* cl);

/* Extract receiver immediately before `.` / `->` at `op`.
 * Accepts ident, a->b.c chains, parenthesized expr, or call ending in `)`. */
static int shadow_ufcs_recv(const char* src, const char* op, char* recv, size_t rcap) {
    const char* r = op;
    while (r > src && (r[-1] == ' ' || r[-1] == '\t')) r--;
    const char* end = r;
    for (;;) {
        if (r > src && r[-1] == ')') {
            int depth = 0;
            while (r > src) {
                r--;
                if (*r == ')') depth++;
                else if (*r == '(') {
                    depth--;
                    if (depth == 0) break;
                }
            }
            if (depth != 0) return 0;
            while (r > src && (r[-1] == ' ' || r[-1] == '\t')) r--;
            while (r > src) {
                char c = r[-1];
                if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                    (c >= '0' && c <= '9') || c == '_')
                    r--;
                else
                    break;
            }
        } else {
            while (r > src) {
                char c = r[-1];
                if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                    (c >= '0' && c <= '9') || c == '_')
                    r--;
                else
                    break;
            }
        }
        /* continue through `.` / `->` field access */
        while (r > src && (r[-1] == ' ' || r[-1] == '\t')) r--;
        if (r > src && r[-1] == '.') {
            r--;
            while (r > src && (r[-1] == ' ' || r[-1] == '\t')) r--;
            continue;
        }
        if (r > src + 1 && r[-1] == '>' && r[-2] == '-') {
            r -= 2;
            while (r > src && (r[-1] == ' ' || r[-1] == '\t')) r--;
            continue;
        }
        break;
    }
    if (r == end) return 0;
    size_t n = (size_t)(end - r);
    if (n >= rcap) return 0;
    memcpy(recv, r, n);
    recv[n] = 0;
    return 1;
}

/* Balanced args starting just after `(`. */
static int shadow_ufcs_args(const char* open_paren, char* args, size_t acap,
                            const char** after_close) {
    if (!open_paren || *open_paren != '(') return 0;
    int depth = 0;
    const char* p = open_paren;
    for (; *p; p++) {
        if (*p == '(') depth++;
        else if (*p == ')') {
            depth--;
            if (depth == 0) {
                size_t n = (size_t)(p - (open_paren + 1));
                if (n >= acap) n = acap - 1;
                memcpy(args, open_paren + 1, n);
                args[n] = 0;
                if (after_close) *after_close = p + 1;
                return 1;
            }
        }
    }
    return 0;
}

/* Lower one UFCS call from structured pieces → C spelling in dst.
 * meth_name is bare (no parens); args is inside-parens text (may be empty).
 * elem_ty is the member type formal (from ::[T] or typed destination). */
/* Spec: trailing capital T marks a type-formal member; block_on / py map
 * helpers are registered by name. */
static int shadow_meth_has_type_formal(const char* meth) {
    size_t n;
    if (!meth || !meth[0]) return 0;
    if (strcmp(meth, "block_on") == 0) return 1;
    if (strcmp(meth, "map") == 0 || strcmp(meth, "as_map") == 0 ||
        strcmp(meth, "as_list") == 0)
        return 1;
    n = strlen(meth);
    return meth[n - 1] == 'T';
}

static int shadow_ufcs_recv_is_ident(const char* recv);

/* e encoding: "" | "->" | "::T" | "->::T" */
static int shadow_ufcs_e_arrow(const char* e) {
    return e && e[0] == '-' && e[1] == '>';
}
static const char* shadow_ufcs_e_targs(const char* e) {
    const char* p;
    if (!e || !e[0]) return NULL;
    p = e;
    if (p[0] == '-' && p[1] == '>') p += 2;
    if (p[0] == ':' && p[1] == ':') return p + 2;
    return NULL;
}

/* After method name in text: optional `::[…]` then `(`. Returns `(` or NULL. */
static const char* shadow_ufcs_text_paren_after_meth(const char* meth_end,
                                                    char* targs, size_t tcap) {
    const char* q = meth_end;
    int depth;
    const char* br;
    const char* close;
    if (targs && tcap) targs[0] = 0;
    while (*q == ' ' || *q == '\t') q++;
    if (*q == '(') return q;
    if (q[0] != ':' || q[1] != ':') return NULL;
    q += 2;
    while (*q == ' ' || *q == '\t') q++;
    if (*q != '[') return NULL;
    br = q + 1;
    depth = 0;
    for (close = q; *close; close++) {
        if (*close == '[') depth++;
        else if (*close == ']') {
            depth--;
            if (depth == 0) {
                if (targs && tcap) {
                    size_t n = (size_t)(close - br);
                    while (n && (br[n - 1] == ' ' || br[n - 1] == '\t')) n--;
                    while (n && (*br == ' ' || *br == '\t')) {
                        br++;
                        n--;
                    }
                    if (n >= tcap) n = tcap - 1;
                    memcpy(targs, br, n);
                    targs[n] = 0;
                }
                q = close + 1;
                while (*q == ' ' || *q == '\t') q++;
                return (*q == '(') ? q : NULL;
            }
        }
    }
    return NULL;
}

/* Normalize a C type spelling for UFCS first-param compare. */
static void shadow_ufcs_norm_ty(const char* in, char* out, size_t cap) {
    const char* s = in ? in : "";
    size_t o = 0;
    while (*s == ' ' || *s == '\t') s++;
    while (*s && o + 1 < cap) {
        if (*s == ' ' || *s == '\t') {
            if (o > 0 && out[o - 1] != ' ' && out[o - 1] != '*') out[o++] = ' ';
            s++;
            continue;
        }
        if (*s == '*' && o > 0 && out[o - 1] == ' ') o--;
        out[o++] = *s++;
    }
    while (o > 0 && out[o - 1] == ' ') o--;
    out[o] = 0;
}

static int shadow_ufcs_ty_eq(const char* a, const char* b) {
    char na[160], nb[160];
    shadow_ufcs_norm_ty(a, na, sizeof(na));
    shadow_ufcs_norm_ty(b, nb, sizeof(nb));
    return na[0] && strcmp(na, nb) == 0;
}

/* Strip trailing param name from "Pair* p1" / "Pair p1" → type for match. */
static void shadow_ufcs_param_ty_only(const char* in, char* out, size_t cap) {
    const char* e;
    size_t n;
    if (!out || !cap) return;
    out[0] = 0;
    if (!in || !in[0]) return;
    e = in + strlen(in);
    while (e > in &&
           ((e[-1] >= 'A' && e[-1] <= 'Z') || (e[-1] >= 'a' && e[-1] <= 'z') ||
            (e[-1] >= '0' && e[-1] <= '9') || e[-1] == '_'))
        e--;
    while (e > in && (e[-1] == ' ' || e[-1] == '\t')) e--;
    n = (size_t)(e - in);
    if (n >= cap) n = cap - 1;
    memcpy(out, in, n);
    out[n] = 0;
}

/* Universal bare-name tier: meth(recv[, args]) when first param matches. */
/* Dynamic sink: callee(&recv, "meth", N, WRAP(a1), …). */
static int shadow_ufcs_emit_dyn_sink(const char* recv, const char* meth_name,
                                    const char* args, int ptr_recv,
                                    const char* callee, const char* wrap,
                                    char* dst, size_t cap) {
    char argbuf[512];
    size_t ao = 0;
    int argc = 0;
    if (!recv || !meth_name || !callee || !wrap || !dst || !cap) return 0;
    argbuf[0] = 0;
    if (args && args[0]) {
        const char* p = args;
        const char* seg = p;
        int depth = 0;
        int in_str = 0, in_chr = 0;
        for (;; p++) {
            char c = *p;
            int at_end = (c == '\0');
            if (!at_end && in_str) {
                if (c == '\\' && p[1]) p++;
                else if (c == '"') in_str = 0;
                continue;
            }
            if (!at_end && in_chr) {
                if (c == '\\' && p[1]) p++;
                else if (c == '\'') in_chr = 0;
                continue;
            }
            if (!at_end) {
                if (c == '"') {
                    in_str = 1;
                    continue;
                }
                if (c == '\'') {
                    in_chr = 1;
                    continue;
                }
                if (c == '(' || c == '[' || c == '{') {
                    depth++;
                    continue;
                }
                if (c == ')' || c == ']' || c == '}') {
                    depth--;
                    continue;
                }
                if (!(c == ',' && depth == 0)) continue;
            }
            {
                size_t seg_len = (size_t)(p - seg);
                while (seg_len > 0 && (seg[0] == ' ' || seg[0] == '\t')) {
                    seg++;
                    seg_len--;
                }
                while (seg_len > 0 &&
                       (seg[seg_len - 1] == ' ' || seg[seg_len - 1] == '\t'))
                    seg_len--;
                if (seg_len > 0) {
                    int n = snprintf(argbuf + ao, sizeof(argbuf) - ao,
                                     ", %s(%.*s)", wrap, (int)seg_len, seg);
                    if (n < 0 || (size_t)n >= sizeof(argbuf) - ao) return 0;
                    ao += (size_t)n;
                    argc++;
                }
            }
            if (at_end) break;
            seg = p + 1;
        }
    }
    if (ptr_recv)
        snprintf(dst, cap, "%s((%s), \"%s\", %d%s)", callee, recv, meth_name,
                 argc, argbuf);
    else
        snprintf(dst, cap, "%s(&(%s), \"%s\", %d%s)", callee, recv, meth_name,
                 argc, argbuf);
    return 1;
}

/* Mangle dest type for `<sink>_<mangled>` (cv-stripped). */
static void shadow_sink_mangle_dest(const char* dest, char* out, size_t cap) {
    char buf[96];
    size_t i, o = 0;
    if (!dest || !out || !cap) return;
    out[0] = 0;
    while (*dest == ' ' || *dest == '\t') dest++;
    if (strncmp(dest, "const ", 6) == 0) dest += 6;
    while (*dest == ' ' || *dest == '\t') dest++;
    snprintf(buf, sizeof(buf), "%s", dest);
    /* Drop trailing stars / spaces. */
    {
        size_t L = strlen(buf);
        while (L && (buf[L - 1] == '*' || buf[L - 1] == ' ' || buf[L - 1] == '\t'))
            buf[--L] = 0;
    }
    for (i = 0; buf[i] && o + 1 < cap; i++) {
        char c = buf[i];
        if (c == ' ' || c == '\t') c = '_';
        out[o++] = c;
    }
    out[o] = 0;
    /* Collapse __ from "long  long". */
    {
        char* p;
        while ((p = strstr(out, "__")) != NULL) memmove(p, p + 1, strlen(p));
    }
}

/* Try registered .ufcs_dynamic sink for receiver base type. */
static int shadow_ufcs_try_dyn_sink(const char* recv, const char* meth_name,
                                   const char* args, int ptr_recv,
                                   const char* ty, char* dst, size_t cap) {
    const ShadowDynSink* ds;
    char base[96];
    const char* callee;
    /* Function-scoped: callee may point here across the emit_dyn_sink call.
     * A block-local tryc dangles into emit_dyn_sink's argbuf (same stack
     * slot) and the sink name becomes `, WRAP(...)` — compiling-but-wrong. */
    char tryc[160];
    size_t n;
    if (!ty || !ty[0]) return 0;
    while (*ty == ' ' || *ty == '\t') ty++;
    if (strncmp(ty, "struct ", 7) == 0) ty += 7;
    n = 0;
    while (ty[n] && ty[n] != ' ' && ty[n] != '*' && n + 1 < sizeof(base)) {
        base[n] = ty[n];
        n++;
    }
    base[n] = 0;
    ds = shadow_dyn_sink_for(base);
    if (!ds) return 0;
    callee = ds->callee;
    tryc[0] = 0;
    if (ds->dest_aware && g_shadow_sink_dest[0]) {
        char mangled[96];
        shadow_sink_mangle_dest(g_shadow_sink_dest, mangled, sizeof(mangled));
        if (mangled[0]) {
            int scalar_dest =
                strcmp(mangled, "int") == 0 || strcmp(mangled, "long_long") == 0 ||
                strcmp(mangled, "int64_t") == 0 || strcmp(mangled, "int32_t") == 0 ||
                strcmp(mangled, "uint64_t") == 0 ||
                strcmp(mangled, "double") == 0 || strcmp(mangled, "float") == 0 ||
                strcmp(mangled, "bool") == 0 || strcmp(mangled, "size_t") == 0 ||
                strcmp(mangled, "char") == 0;
            snprintf(tryc, sizeof(tryc), "%s_%s", ds->callee, mangled);
            /* Header static-inline `callm_<dest>` may miss the local ufn table.
             * Still invent for scalar dests; never invent `ts2_call_long_long`
             * when undeclared — fall back to plain sink. */
            if (shadow_ufn_exists(tryc) ||
                (scalar_dest && strncmp(ds->callee, "cc_py_obj_", 10) == 0))
                callee = tryc;
        }
    }
    return shadow_ufcs_emit_dyn_sink(recv, meth_name, args, ptr_recv, callee,
                                    ds->wrap, dst, cap);
}

/* Build `recv.meth(args)` / `recv->meth(args)` surface for whole-RHS checks. */
static void shadow_ufcs_surface(AstNode* u, char* dst, size_t cap) {
    const char* uop;
    const char* uta;
    if (!u || !dst || !cap) return;
    dst[0] = 0;
    uop = shadow_ufcs_e_arrow(u->e) ? "->" : ".";
    uta = shadow_ufcs_e_targs(u->e);
    if (uta && uta[0] && u->c[0])
        snprintf(dst, cap, "%s%s%s::[%s](%s)", u->a, uop, u->b, uta, u->c);
    else if (uta && uta[0])
        snprintf(dst, cap, "%s%s%s::[%s]()", u->a, uop, u->b, uta);
    else if (u->c[0])
        snprintf(dst, cap, "%s%s%s(%s)", u->a, uop, u->b, u->c);
    else
        snprintf(dst, cap, "%s%s%s()", u->a, uop, u->b);
}

static int shadow_text_eq_trim(const char* a, const char* b) {
    if (!a || !b) return 0;
    while (*a == ' ' || *a == '\t' || *a == '\n') a++;
    while (*b == ' ' || *b == '\t' || *b == '\n') b++;
    {
        size_t na = strlen(a), nb = strlen(b);
        while (na && (a[na - 1] == ' ' || a[na - 1] == '\t' || a[na - 1] == '\n'))
            na--;
        while (nb && (b[nb - 1] == ' ' || b[nb - 1] == '\t' || b[nb - 1] == '\n'))
            nb--;
        if (na != nb) return 0;
        return na == 0 || memcmp(a, b, na) == 0;
    }
}

/* `(T)call` / `((T)call)` → dest T; *out_inner points at call start. */
static const char* shadow_sink_cast_dest(const char* text, char* dest,
                                         size_t dcap) {
    const char* p;
    const char* ty0;
    const char* ty1;
    const char* call;
    size_t nparen = 0;
    if (!text || !dest || !dcap) return NULL;
    dest[0] = 0;
    p = text;
    while (*p == ' ' || *p == '\t' || *p == '\n') p++;
    while (*p == '(') {
        nparen++;
        p++;
        while (*p == ' ' || *p == '\t' || *p == '\n') p++;
    }
    if (nparen == 0) return NULL;
    /* Type tokens then `)`. */
    ty0 = p;
    if (!(((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || *p == '_') ||
          strncmp(p, "long", 4) == 0))
        return NULL;
    while (*p && *p != ')') p++;
    if (*p != ')') return NULL;
    ty1 = p;
    {
        size_t n = (size_t)(ty1 - ty0);
        while (n && (ty0[n - 1] == ' ' || ty0[n - 1] == '\t')) n--;
        if (!n || n + 1 >= dcap) return NULL;
        memcpy(dest, ty0, n);
        dest[n] = 0;
    }
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\n') p++;
    call = p;
    /* Trailing close-parens for `((T)call)`. */
    {
        const char* e = call + strlen(call);
        size_t need = nparen - 1;
        while (e > call && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\n'))
            e--;
        while (need && e > call && e[-1] == ')') {
            e--;
            need--;
        }
        if (need) return NULL;
        (void)e;
    }
    return call;
}

static int shadow_ufcs_emit_bare(const char* recv, const char* meth_name,
                                 const char* args, int recv_is_ptr,
                                 const char* recv_base, int recv_is_const,
                                 char* dst, size_t cap) {
    const char* param_full;
    char param[160];
    char cand[160];
    int matched = 0;
    int by_ptr = 0;
    if (!recv || !meth_name || !recv_base || !recv_base[0] || !dst || !cap)
        return 0;
    param_full = shadow_ufn_first_ty(meth_name);
    if (!param_full || !param_full[0]) return 0;
    shadow_ufcs_param_ty_only(param_full, param, sizeof(param));
    if (!param[0]) return 0;
    if (!recv_is_ptr) {
        const char* pv = param;
        if (strncmp(pv, "const ", 6) == 0) pv += 6;
        if (shadow_ufcs_ty_eq(pv, recv_base) && !strchr(pv, '*')) {
            matched = 1;
        } else {
            snprintf(cand, sizeof(cand), "%s*", recv_base);
            if (shadow_ufcs_ty_eq(param, cand)) {
                matched = 1;
                by_ptr = 1;
            } else {
                snprintf(cand, sizeof(cand), "const %s*", recv_base);
                if (shadow_ufcs_ty_eq(param, cand)) {
                    matched = 1;
                    by_ptr = 1;
                }
            }
        }
    } else {
        snprintf(cand, sizeof(cand), "const %s*", recv_base);
        if (shadow_ufcs_ty_eq(param, cand)) matched = 1;
        if (!matched && shadow_ufcs_ty_eq(param, "const void*")) matched = 1;
        if (!matched && !recv_is_const) {
            snprintf(cand, sizeof(cand), "%s*", recv_base);
            if (shadow_ufcs_ty_eq(param, cand)) matched = 1;
            if (!matched && shadow_ufcs_ty_eq(param, "void*")) matched = 1;
        }
    }
    if (!matched) return 0;
    {
        const char* amp = by_ptr ? "&" : "";
        if (args && args[0])
            snprintf(dst, cap, "%s(%s%s, %s)", meth_name, amp, recv, args);
        else
            snprintf(dst, cap, "%s(%s%s)", meth_name, amp, recv);
    }
    return 1;
}

/* Recover CCSlice_T from a prior typed-slice call: CCSlice_double_sub(...). */
static int shadow_ufcs_recv_slice_ty(const char* recv, char* sty, size_t cap) {
    const char* us = NULL;
    const char* q;
    size_t n;
    if (!recv || !sty || !cap) return 0;
    /* CCVec_T_as_slice(...) → CCSlice_T */
    if (strncmp(recv, "CCVec_", 6) == 0) {
        const char* p = recv + 6;
        const char* as = strstr(p, "_as_slice");
        if (as && as[9] == '(') {
            size_t el = (size_t)(as - p);
            if (el + 9 >= cap) return 0;
            snprintf(sty, cap, "CCSlice_%.*s", (int)el, p);
            return 1;
        }
    }
    if (strncmp(recv, "CCSlice_", 8) != 0) return 0;
    for (q = recv + 8; *q && *q != '('; q++) {
        if (*q == '_') us = q;
    }
    if (!us || us <= recv) return 0;
    n = (size_t)(us - recv);
    if (n + 1 >= cap) return 0;
    memcpy(sty, recv, n);
    sty[n] = 0;
    return 1;
}

/* True when recv is a plain C identifier (addressable lvalue). */
static int shadow_ufcs_recv_is_ident(const char* recv) {
    const char* p = recv;
    if (!p || !*p) return 0;
    if (!((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || *p == '_'))
        return 0;
    for (p++; *p; p++) {
        if (!((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') ||
              (*p >= '0' && *p <= '9') || *p == '_'))
            return 0;
    }
    return 1;
}

/* `recv.expose::[T]` → factory family `py_expose` (snake(CCPy)_expose).
 * Matches production cc__rewrite_member_generic_to_free_name. */
static int shadow_ufcs_factory_member_cand(const char* recv, const char* vty,
                                          const char* meth, char* cand,
                                          size_t ccap) {
    char base[192];
    char snake[192];
    size_t bl;
    size_t ml;
    if (!meth || !meth[0] || !cand || !ccap) return 0;
    cand[0] = 0;
    ml = strlen(meth);
    /* Simple-ident shortcut: `py.expose` → `py_expose`. */
    if (recv && recv[0] && shadow_ufcs_recv_is_ident(recv)) {
        snprintf(cand, ccap, "%s_%s", recv, meth);
        if (cc_emit_plan_has_generic_factory(cand)) return 1;
        cand[0] = 0;
    }
    if (vty && vty[0]) {
        snprintf(base, sizeof(base), "%s", vty);
        bl = strlen(base);
        while (bl > 0 && (base[bl - 1] == '*' || base[bl - 1] == ' '))
            base[--bl] = 0;
        if (bl > 2 && base[0] == 'C' && base[1] == 'C' && base[2] >= 'A' &&
            base[2] <= 'Z')
            memmove(base, base + 2, bl - 1);
        cc_emit_plan_snake_name(base, snake, sizeof(snake));
        if (snake[0]) {
            snprintf(cand, ccap, "%s_%s", snake, meth);
            if (cc_emit_plan_has_generic_factory(cand)) return 1;
            cand[0] = 0;
        }
    }
    /* Bang-chain hop `((__rN).u.value).expose::[T]` — recv type is unbindable;
     * resolve by factory suffix `_*meth`. */
    if (recv && strstr(recv, ".u.value")) {
        char names[512];
        if (cc_emit_plan_generic_factory_names_csv(names, sizeof(names)) > 0) {
            char* tok = names;
            while (tok && *tok) {
                char* comma = strchr(tok, ',');
                size_t nl;
                if (comma) *comma = 0;
                while (*tok == ' ') tok++;
                nl = strlen(tok);
                if (nl > ml + 1 && tok[nl - ml - 1] == '_' &&
                    strcmp(tok + nl - ml, meth) == 0) {
                    snprintf(cand, ccap, "%s", tok);
                    return 1;
                }
                tok = comma ? comma + 1 : NULL;
            }
        }
    }
    return 0;
}

/* Compact type formal `Counter` / `K_V` for factory monomorph name. */
static void shadow_ufcs_formal_compact(const char* formal, char* out,
                                      size_t ocap) {
    size_t o = 0;
    const char* p;
    if (!out || !ocap) return;
    out[0] = 0;
    if (!formal) return;
    p = formal;
    while (*p && o + 1 < ocap) {
        if (*p == ' ' || *p == '\t') {
            p++;
            continue;
        }
        if (*p == ',' || *p == '*') {
            if (o && out[o - 1] != '_') out[o++] = '_';
            p++;
            continue;
        }
        out[o++] = *p++;
    }
    out[o] = 0;
}

/* Emit CCSlice_T_meth(&recv), or meth((T[]){recv}) so rvalues decay to T*.
 * Declared extensions `cc_slice_<elem>_<meth>` beat the composed instance name. */
static int shadow_ufcs_emit_slice_meth(const char* sty, const char* meth,
                                       const char* recv, const char* args,
                                       char* dst, size_t cap) {
    char amp[384];
    char composed[160];
    char ext[160];
    const char* callee;
    if (!sty || !meth || !recv || !dst || !cap) return 0;
    snprintf(composed, sizeof(composed), "%s_%s", sty, meth);
    snprintf(ext, sizeof(ext), "cc_slice_%s_%s", sty + 8, meth);
    if (shadow_ufn_exists(ext) && !shadow_ufn_exists(composed))
        callee = ext;
    else
        callee = composed;
    if (shadow_ufcs_recv_is_ident(recv))
        snprintf(amp, sizeof(amp), "&%s", recv);
    else
        snprintf(amp, sizeof(amp), "((%s[]){%s})", sty, recv);
    if (args && args[0])
        snprintf(dst, cap, "%s(%s, %s)", callee, amp, args);
    else
        snprintf(dst, cap, "%s(%s)", callee, amp);
    return 1;
}

/* Peel a leading C cast `(T*)` / `(T)` so bind/UFCS see the real receiver.
 * Must not peel grouping / unary deref: `(*link)->key`, `(&x)->…`. */
static const char* shadow_ufcs_skip_cast(const char* recv, char* buf,
                                         size_t buf_cap) {
    const char* p;
    const char* rp;
    const char* in;
    int depth;
    int has_star = 0, has_ident = 0, has_call = 0;
    if (!recv || !buf || !buf_cap) return recv;
    p = recv;
    while (*p == ' ' || *p == '\t') p++;
    if (*p != '(') return recv;
    in = p + 1;
    while (*in == ' ' || *in == '\t') in++;
    /* Unary `*` / `&` / `!` / `~` / `+/-` → parenthesized expr, not a cast. */
    if (*in == '*' || *in == '&' || *in == '!' || *in == '~' || *in == '+' ||
        *in == '-')
        return recv;
    depth = 0;
    rp = NULL;
    for (; *p; p++) {
        if (*p == '(') depth++;
        else if (*p == ')') {
            depth--;
            if (depth == 0) {
                rp = p;
                break;
            }
        }
    }
    if (!rp) return recv;
    /* Cast if inside looks like a type (has ident + optional *), not a call. */
    for (; in < rp; in++) {
        if ((*in >= 'A' && *in <= 'Z') || (*in >= 'a' && *in <= 'z') ||
            *in == '_')
            has_ident = 1;
        if (*in == '*') has_star = 1;
        if (*in == '(') {
            has_call = 1;
            break;
        }
    }
    if (!has_ident || has_call) return recv;
    (void)has_star;
    p = rp + 1;
    while (*p == ' ' || *p == '\t') p++;
    if (!*p) return recv;
    snprintf(buf, buf_cap, "%s", p);
    return buf;
}

/* Map_/ArrayMap_ or CC_MAP_DECL_* type with installed Name_insert. */
static int shadow_ufcs_is_map_ty(const char* vty) {
    char tryc[160];
    if (!vty || !vty[0]) return 0;
    if (strncmp(vty, "Map_", 4) == 0 || strncmp(vty, "ArrayMap_", 9) == 0)
        return 1;
    snprintf(tryc, sizeof(tryc), "%s_insert", vty);
    return shadow_ufn_exists(tryc);
}

/* Exact type match: vty is `name` / `name*`, or bind base ty is `name`.
 * Prefer this over shadow_bind_ty_has (strstr) so CCArena ≠ CCArenaPool. */
static int shadow_ufcs_ty_is(const char* vty, const ShadowBind* rb,
                             const char* name) {
    char starred[128];
    char base[96];
    size_t n;
    if (!name || !name[0]) return 0;
    if (vty && vty[0]) {
        if (strcmp(vty, name) == 0) return 1;
        n = strlen(name);
        if (n + 2 < sizeof(starred)) {
            memcpy(starred, name, n);
            starred[n] = '*';
            starred[n + 1] = 0;
            if (strcmp(vty, starred) == 0) return 1;
        }
    }
    if (rb && rb->ty[0]) {
        shadow_bind_base_ty(rb, base, sizeof(base));
        if (strcmp(base, name) == 0) return 1;
    }
    return 0;
}

/* Strip trailing * / spaces from vty into out (for mangled Type_destroy). */
static void shadow_ufcs_vty_base(const char* vty, char* out, size_t cap) {
    size_t n;
    if (!out || !cap) return;
    out[0] = 0;
    if (!vty || !vty[0]) return;
    while (*vty == ' ' || *vty == '\t') vty++;
    snprintf(out, cap, "%s", vty);
    n = strlen(out);
    while (n && (out[n - 1] == '*' || out[n - 1] == ' ' || out[n - 1] == '\t'))
        out[--n] = 0;
}

/* Land `callee(amp recv[, args])` into dst.
 * amp is "" (by-value / pointer recv) or "&" (address-of value recv).
 * Use snprintf — `@string(${amp}…)` drops a bare `&` amp (template capture).
 * Overflow returns 0 — never a truncated call spelling. */
static int shadow_ufcs_fmt_call(char* dst, size_t cap, const char* callee,
                                const char* amp, const char* recv,
                                const char* args) {
    int n;
    if (!dst || !cap || !callee || !recv) return 0;
    if (!amp) amp = "";
    if (args && args[0])
        n = snprintf(dst, cap, "%s(%s%s, %s)", callee, amp, recv, args);
    else
        n = snprintf(dst, cap, "%s(%s%s)", callee, amp, recv);
    if (n < 0 || (size_t)n >= cap) {
        if (cap) dst[0] = 0;
        return 0;
    }
    return 1;
}

/* Bang-chain hop on Result ok value: `(__rN.u.value).step()` → cc_int_step /
 * `cc_size_t_as_int` when registered as a scalar-family fn. */
static int shadow_ufcs_try_result_value_hop(const char* recv,
                                           const char* meth_name,
                                           const char* args,
                                           const char* elem_ty, char* dst,
                                           size_t cap) {
    char tryc[160];
    const char* hint = elem_ty;
    if (!recv || !meth_name || !meth_name[0] || !dst || !cap) return 0;
    if (!strstr(recv, ".u.value")) return 0;
    if (hint && hint[0] &&
        (strcmp(hint, "int") == 0 || strcmp(hint, "size_t") == 0 ||
         strcmp(hint, "double") == 0 || strcmp(hint, "bool") == 0)) {
        snprintf(tryc, sizeof(tryc), "cc_%s_%s", hint, meth_name);
        if (shadow_ufn_exists(tryc))
            return shadow_ufcs_fmt_call(dst, cap, tryc, "", recv, args);
    }
    {
        static const char* scalars[] = {"int", "size_t", "double", "bool", NULL};
        int si;
        for (si = 0; scalars[si]; si++) {
            snprintf(tryc, sizeof(tryc), "cc_%s_%s", scalars[si], meth_name);
            if (!shadow_ufn_exists(tryc)) continue;
            return shadow_ufcs_fmt_call(dst, cap, tryc, "", recv, args);
        }
    }
    return 0;
}

static int shadow_ufcs_lower_parts(const char* recv, const char* meth_name,
                                   const char* args, int is_arrow,
                                   const char* elem_ty, char* dst, size_t cap) {
    /* Args may embed multiline @slice("…") / @string(`…`) payloads. */
    char a[4096];
    char recv_buf[192];
    const ShadowBind* rb;
    char vty[128];
    int recv_is_double;
    if (!recv || !meth_name || !dst || !cap) return 0;
    recv = shadow_ufcs_skip_cast(recv, recv_buf, sizeof(recv_buf));
    while (*recv == ' ' || *recv == '\t') recv++;
    a[0] = 0;
    if (args) snprintf(a, sizeof(a), "%s", args);
    while (a[0] == ' ' || a[0] == '\t') memmove(a, a + 1, strlen(a));
    recv_is_double =
        strstr(recv, "double") || strstr(recv, "halve") ||
        strstr(recv, "fabs") || (strchr(recv, '.') && !strchr(recv, '('));
    rb = shadow_bind_for_recv(recv);
    shadow_bind_base_ty(rb, vty, sizeof(vty));
    /* Member-generic factory before elem_ty→vty fallback: `::[T]` formal is the
     * monomorph arg, not the receiver type (`Counter` ≠ `CCPy`). */
    if (!is_arrow && elem_ty && elem_ty[0]) {
        char fam[160];
        char compact[128];
        if (shadow_ufcs_factory_member_cand(recv, vty, meth_name, fam,
                                           sizeof(fam))) {
            int need_amp = 1;
            size_t vl = strlen(vty);
            shadow_ufcs_formal_compact(elem_ty, compact, sizeof(compact));
            if (!compact[0]) return 0;
            if (vl && vty[vl - 1] == '*') need_amp = 0;
            if (recv[0] == '*' || strstr(recv, "->") ||
                strstr(recv, ".u.value"))
                need_amp = 0;
            if (a[0])
                snprintf(dst, cap, "%s_%s(%s%s, %s)", fam, compact,
                         need_amp ? "&" : "", recv, a);
            else
                snprintf(dst, cap, "%s_%s(%s%s)", fam, compact,
                         need_amp ? "&" : "", recv);
            return 1;
        }
    }
    /* Typed destination / bang-chain hop hint when the receiver has no bind
     * (e.g. `((__r0).u.value).as_i64()` with elem_ty = CCPyObj). */
    if (!vty[0] && elem_ty && elem_ty[0] && strchr(elem_ty, ' ') == NULL &&
        elem_ty[0] >= 'A' && elem_ty[0] <= 'Z')
        snprintf(vty, sizeof(vty), "%s", elem_ty);
    if (!is_arrow && !vty[0] &&
        shadow_ufcs_try_result_value_hop(recv, meth_name, a, elem_ty, dst, cap))
        return 1;
    /* Type-scoped statics: `Type.method(args)` → `Type_method(args)` (no recv
     * insert). Schema types are registered as ufns; rules-only grammars (Kv)
     * may not be — invent for the known grammar entry methods. */
    if (!is_arrow && !rb && shadow_ufcs_recv_is_ident(recv) &&
        recv[0] >= 'A' && recv[0] <= 'Z') {
        static const char* type_scoped[] = {
            "match", "parse", "read", "try_read", "write", "measure", "to_str",
            "reader", "get"
        };
        char composed[160];
        size_t mi;
        int known = 0;
        for (mi = 0; mi < sizeof(type_scoped) / sizeof(type_scoped[0]); mi++) {
            if (strcmp(meth_name, type_scoped[mi]) == 0) {
                known = 1;
                break;
            }
        }
        snprintf(composed, sizeof(composed), "%s_%s", recv, meth_name);
        if (known || shadow_ufn_exists(composed)) {
            if (a[0])
                snprintf(dst, cap, "%s(%s)", composed, a);
            else
                snprintf(dst, cap, "%s()", composed);
            return 1;
        }
    }
    /* Field path: db.entries.meth → type of entries, not Db.
     * path_is_ptr: final field is a pointer (ArrayMap_* *entries). */
    int path_is_ptr = 0;
    int walked_fields = 0;
    if (vty[0] && recv) {
        const char* p = recv;
        char outer[128];
        char fty[128];
        while (*p == '*' || *p == '&' || *p == ' ' || *p == '\t' || *p == '(')
            p++;
        while (*p && *p != '.' && *p != '-' && *p != '(' && *p != ' ' &&
               *p != '\t')
            p++;
        snprintf(outer, sizeof(outer), "%s", vty);
        while (*p == '.' || (*p == '-' && p[1] == '>')) {
            char fname[64];
            size_t ni = 0;
            int fptr = 0;
            if (*p == '-') p += 2;
            else p++;
            while (*p == ' ' || *p == '\t') p++;
            while (((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') ||
                    (*p >= '0' && *p <= '9') || *p == '_') &&
                   ni + 1 < sizeof(fname))
                fname[ni++] = *p++;
            fname[ni] = 0;
            if (!fname[0] || !shadow_field_ty_of(outer, fname, fty, sizeof(fty)))
                break;
            walked_fields = 1;
            if (strchr(fty, '*')) fptr = 1;
            /* Strip trailing * from field type for family spelling. */
            {
                size_t L = strlen(fty);
                while (L && (fty[L - 1] == '*' || fty[L - 1] == ' '))
                    fty[--L] = 0;
            }
            path_is_ptr = fptr;
            snprintf(outer, sizeof(outer), "%s", fty);
            snprintf(vty, sizeof(vty), "%s", fty);
        }
        /* Map/ArrayMap field sugar is a handle pointer even when spelled
         * without `*` on the member declarator. */
        if (walked_fields && !path_is_ptr && vty[0] &&
            (strncmp(vty, "ArrayMap_", 9) == 0 || strncmp(vty, "Map_", 4) == 0))
            path_is_ptr = 1;
    }
    /* Helper: pointer vs &recv for container methods.
 * Arrow call / final field pointer / already-& recv. Unresolved pointer bind
 * only when recv is a bare name — a field path (`p->out.clear`) must use
 * path_is_ptr from the walk, not the outer bind's `*`. */
#define SHADOW_UFCS_PTR_RECV()                                                   \
    (is_arrow || path_is_ptr ||                                                   \
     (!walked_fields && rb && strchr(rb->ty, '*') != NULL &&                      \
      strchr(recv, '.') == NULL && strstr(recv, "->") == NULL) ||                \
     recv[0] == '&')
    /* Chain continuation through typed-slice calls. */
    if (!vty[0])
        (void)shadow_ufcs_recv_slice_ty(recv, vty, sizeof(vty));

    /* @as field UFCS: Outer.open/write/close → cc_file_*( &Outer.field, … ).
     * Only plain Outer idents — `w.file.write` already names the embed. */
    if (!is_arrow && vty[0] && g_shadow_nas > 0 &&
        shadow_ufcs_recv_is_ident(recv) &&
        (strcmp(meth_name, "open") == 0 || strcmp(meth_name, "close") == 0 ||
         strcmp(meth_name, "write") == 0 || strcmp(meth_name, "read") == 0)) {
        int ai;
        for (ai = 0; ai < g_shadow_nas; ai++) {
            const ShadowAsEmbed* as = &g_shadow_as[ai];
            char arecv[192];
            int ptr = 0;
            if (strcmp(as->outer, vty) != 0) continue;
            if (strcmp(as->target, "CCFile") != 0) continue;
            if (rb && strchr(rb->ty, '*')) ptr = 1;
            if (ptr)
                snprintf(arecv, sizeof(arecv), "&%s->%s", recv, as->field);
            else
                snprintf(arecv, sizeof(arecv), "&%s.%s", recv, as->field);
            if (strcmp(meth_name, "open") == 0)
                snprintf(dst, cap, "cc_file_open(%s, %s)", arecv, a);
            else if (strcmp(meth_name, "close") == 0)
                snprintf(dst, cap, "cc_file_close(%s)", arecv);
            else if (strcmp(meth_name, "write") == 0)
                snprintf(dst, cap, "cc_file_write(%s, %s)", arecv, a);
            else
                snprintf(dst, cap, "cc_file_read(%s, %s)", arecv, a);
            return 1;
        }
    }
    /* Member path `w.file.write` — bind_for_recv stops at `w`; force CCFile. */
    if (!is_arrow && (strstr(recv, ".file") || strstr(recv, "->file")) &&
        (strcmp(meth_name, "open") == 0 || strcmp(meth_name, "close") == 0 ||
         strcmp(meth_name, "write") == 0 || strcmp(meth_name, "read") == 0))
        snprintf(vty, sizeof(vty), "CCFile");

    /* Closure-field sugar: `w->on_value(42)` → cc_closure1_call((w)->on_value, …).
     * Field name shadows a free `Type_meth` lookup when typed CCClosureN. */
    if (vty[0] && meth_name[0]) {
        char fty[96];
        char path[192];
        if (shadow_field_ty_of(vty, meth_name, fty, sizeof(fty))) {
            if (is_arrow)
                snprintf(path, sizeof(path), "(%s)->%s", recv, meth_name);
            else
                snprintf(path, sizeof(path), "(%s).%s", recv, meth_name);
            if (strcmp(fty, "CCClosure0") == 0) {
                snprintf(dst, cap, "cc_closure0_call(%s)", path);
                return 1;
            }
            if (strcmp(fty, "CCClosure1") == 0) {
                snprintf(dst, cap, "cc_closure1_call(%s, (intptr_t)(%s))", path,
                         a);
                return 1;
            }
            if (strcmp(fty, "CCClosure2") == 0) {
                char a0[128], a1[128];
                const char* comma = NULL;
                int depth = 0;
                const char* p;
                a0[0] = a1[0] = 0;
                for (p = a; *p; p++) {
                    if (*p == '(' || *p == '[' || *p == '{') depth++;
                    else if (*p == ')' || *p == ']' || *p == '}') depth--;
                    else if (*p == ',' && depth == 0) {
                        comma = p;
                        break;
                    }
                }
                if (comma) {
                    size_t n0 = (size_t)(comma - a);
                    if (n0 >= sizeof(a0)) n0 = sizeof(a0) - 1;
                    memcpy(a0, a, n0);
                    a0[n0] = 0;
                    while (n0 && (a0[n0 - 1] == ' ' || a0[n0 - 1] == '\t'))
                        a0[--n0] = 0;
                    snprintf(a1, sizeof(a1), "%s", comma + 1);
                    while (a1[0] == ' ' || a1[0] == '\t')
                        memmove(a1, a1 + 1, strlen(a1));
                    snprintf(dst, cap,
                             "cc_closure2_call(%s, (intptr_t)(%s), "
                             "(intptr_t)(%s))",
                             path, a0, a1);
                } else {
                    snprintf(dst, cap,
                             "cc_closure2_call(%s, (intptr_t)(%s), "
                             "(intptr_t)0)",
                             path, a);
                }
                return 1;
            }
        }
    }

    if (!is_arrow && strcmp(meth_name, "remaining") == 0) {
        snprintf(dst, cap, "cc_arena_remaining(&%s)", recv);
    } else if (!is_arrow && strcmp(meth_name, "checkpoint") == 0) {
        snprintf(dst, cap, "cc_arena_checkpoint(&%s)", recv);
    } else if (!is_arrow && strcmp(meth_name, "restore") == 0) {
        snprintf(dst, cap, "cc_arena_checkpoint_restore(&%s)", recv);
    } else if (!is_arrow && strcmp(meth_name, "load") == 0) {
        snprintf(dst, cap, "cc_atomic_load(&%s)", recv);
    } else if (!is_arrow && strcmp(meth_name, "open") == 0) {
        int need_amp = !SHADOW_UFCS_PTR_RECV();
        if (shadow_bind_ty_has(rb, "CCFile") ||
            (vty[0] && strcmp(vty, "CCFile") == 0))
            snprintf(dst, cap,
                     need_amp ? "cc_file_open(&%s, %s)" : "cc_file_open(%s, %s)",
                     recv, a);
        else
            return 0;
    } else if (!is_arrow && strcmp(meth_name, "close") == 0) {
        int need_amp = !SHADOW_UFCS_PTR_RECV();
        if (shadow_bind_ty_has(rb, "CCFile") ||
            (vty[0] && strcmp(vty, "CCFile") == 0))
            snprintf(dst, cap, need_amp ? "cc_file_close(&%s)" : "cc_file_close(%s)", recv);
        else if (shadow_bind_ty_has(rb, "CCListener"))
            snprintf(dst, cap, need_amp ? "cc_listener_close(&%s)" : "cc_listener_close(%s)", recv);
        else if (shadow_bind_ty_has(rb, "CCSocket"))
            snprintf(dst, cap, need_amp ? "cc_socket_close(&%s)" : "cc_socket_close(%s)", recv);
        else if (shadow_bind_ty_has(rb, "CCChanTx") &&
                 shadow_ufn_exists("tracked_tx_close"))
            snprintf(dst, cap, "tracked_tx_close(%s)", recv);
        else if (shadow_bind_ty_has(rb, "CCChan") ||
                 shadow_bind_ty_has(rb, "CCChanTx") ||
                 shadow_bind_ty_has(rb, "CCChanRx") ||
                 shadow_bind_ty_has(rb, "Chan") ||
                 strncmp(vty, "CCChan", 6) == 0)
            snprintf(dst, cap, "cc_channel_close(%s)", recv);
        else if (shadow_bind_ty_has(rb, "CCListener") ||
                 (vty[0] && strcmp(vty, "CCListener") == 0))
            snprintf(dst, cap, need_amp ? "cc_listener_close(&%s)" : "cc_listener_close(%s)", recv);
        else if (shadow_bind_ty_has(rb, "CCSocket") ||
                 (vty[0] && strcmp(vty, "CCSocket") == 0))
            snprintf(dst, cap, need_amp ? "cc_socket_close(&%s)" : "cc_socket_close(%s)", recv);
        else
            return 0; /* no ambient close — resolve from receiver type only */
    } else if (!is_arrow && strcmp(meth_name, "cancel") == 0) {
        if (shadow_bind_ty_has(rb, "CCChan") ||
            shadow_bind_ty_has(rb, "CCChanTx") ||
            shadow_bind_ty_has(rb, "CCChanRx") ||
            shadow_bind_ty_has(rb, "Chan") ||
            strncmp(vty, "CCChan", 6) == 0)
            snprintf(dst, cap, "cc_channel_cancel(%s)", recv);
        else
            return 0;
    } else if (!is_arrow && strcmp(meth_name, "twice") == 0) {
        /* Typed recv (Foo.twice) beats scalar cc_int_twice. */
        if (vty[0] && vty[0] >= 'A' && vty[0] <= 'Z' &&
            strcmp(vty, "CCSlice") != 0) {
            char tryc[160];
            snprintf(tryc, sizeof(tryc), "%s_twice", vty);
            if (shadow_ufn_exists(tryc))
                snprintf(dst, cap, "%s(&%s)", tryc, recv);
            else if (!shadow_ufcs_emit_bare(recv, meth_name, a, 0, vty, 0, dst,
                                           cap))
                snprintf(dst, cap,
                         recv_is_double ? "cc_double_twice(%s)"
                                        : "cc_int_twice(%s)",
                         recv);
        } else {
            snprintf(dst, cap,
                     recv_is_double ? "cc_double_twice(%s)" : "cc_int_twice(%s)",
                     recv);
        }
    } else if (!is_arrow && strcmp(meth_name, "halve") == 0) {
        snprintf(dst, cap, "cc_double_halve(%s)", recv);
    } else if (!is_arrow && strcmp(meth_name, "fabs") == 0) {
        snprintf(dst, cap, "fabs(%s)", recv);
    } else if (!is_arrow && strcmp(meth_name, "strlen") == 0) {
        snprintf(dst, cap, "strlen(%s)", recv);
    } else if (!is_arrow && strcmp(meth_name, "alloc") == 0 &&
               (strcmp(vty, "CCArenaPool") == 0 ||
                (rb && strcmp(rb->ty, "CCArenaPool") == 0) ||
                strstr(recv, "pool") != NULL ||
                strstr(recv, "_stack") != NULL)) {
        if (SHADOW_UFCS_PTR_RECV())
            snprintf(dst, cap, "cc_arena_pool_alloc(%s)", recv);
        else
            snprintf(dst, cap, "cc_arena_pool_alloc(&%s)", recv);
    } else if (!is_arrow && strcmp(meth_name, "detach_arena") == 0 &&
               (strcmp(vty, "CCArenaPool") == 0 ||
                (rb && strcmp(rb->ty, "CCArenaPool") == 0))) {
        if (SHADOW_UFCS_PTR_RECV())
            snprintf(dst, cap, "cc_arena_pool_detach_arena(%s)", recv);
        else
            snprintf(dst, cap, "cc_arena_pool_detach_arena(&%s)", recv);
    } else if (!is_arrow && strcmp(meth_name, "len") == 0) {
        if (vty[0] && strncmp(vty, "CCSlice_", 8) == 0)
            return shadow_ufcs_emit_slice_meth(vty, "len", recv, NULL, dst, cap);
        if (shadow_ufcs_ty_is(vty, rb, "CCShardMap")) {
            if (SHADOW_UFCS_PTR_RECV())
                snprintf(dst, cap, "cc_shard_map_len(%s)", recv);
            else
                snprintf(dst, cap, "cc_shard_map_len(&%s)", recv);
        } else if (shadow_ufcs_ty_is(vty, rb, "CCShard")) {
            if (SHADOW_UFCS_PTR_RECV())
                snprintf(dst, cap, "cc_shard_len(%s)", recv);
            else
                snprintf(dst, cap, "cc_shard_len(&%s)", recv);
        /* Maps before packed — ty may be ArrayMap_CCSlicePacked_int. */
        } else if (vty[0] && (strncmp(vty, "CCVec_", 6) == 0 ||
                       strncmp(vty, "Map_", 4) == 0 ||
                       strncmp(vty, "ArrayMap_", 9) == 0)) {
            if (SHADOW_UFCS_PTR_RECV())
                snprintf(dst, cap, "%s_len(%s)", vty, recv);
            else
                snprintf(dst, cap, "%s_len(&%s)", vty, recv);
        } else if (strcmp(vty, "CCSlicePacked") == 0) {
            snprintf(dst, cap, "cc_slice_packed_len(&%s)", recv);
        } else if (vty[0] && strcmp(vty, "CCString") == 0)
            snprintf(dst, cap, "cc_string_len(&%s)", recv);
        else if (strstr(recv, "__cc_ov_str") ||
                 (strlen(recv) >= 4 &&
                  strcmp(recv + strlen(recv) - 4, ".str") == 0) ||
                 (strlen(recv) >= 5 &&
                  strcmp(recv + strlen(recv) - 5, "->str") == 0))
            /* Packed variant overlay / `.str` field is CCString. */
            snprintf(dst, cap, "cc_string_len(&(%s))", recv);
        else if (strstr(recv, "__cc_tpl; })")) {
            /* Arena @string stmt-expr: fold .len() into the yield. */
            char tmp[4096];
            const char* mark = strstr(recv, "__cc_tpl; })");
            size_t pre = (size_t)(mark - recv);
            if (pre + 32 < sizeof(tmp)) {
                snprintf(tmp, sizeof(tmp), "%.*scc_string_len(&__cc_tpl); })",
                         (int)pre, recv);
                snprintf(dst, cap, "%s", tmp);
            } else
                snprintf(dst, cap,
                         "({ CCString __cc_ls = %s; cc_string_len(&__cc_ls); })",
                         recv);
        } else if (strstr(recv, "cc_string_") || strstr(recv, "CCString"))
            snprintf(dst, cap,
                     "({ CCString __cc_ls = %s; cc_string_len(&__cc_ls); })",
                     recv);
        else if (vty[0] && strcmp(vty, "CCSlice") == 0)
            snprintf(dst, cap, "(%s).len", recv);
        else if (vty[0] && strcmp(vty, "CCSliceArray") == 0)
            snprintf(dst, cap, "(%s).len", recv);
        else if (strstr(recv, "cc__string_stack") || strstr(recv, "cc_slice_") ||
                 strstr(recv, "cc_unwrap") || strstr(recv, "__cc_uw_value"))
            snprintf(dst, cap, "(%s).len", recv);
        else if (shadow_bind_ty_has(rb, "Slice") || shadow_bind_ty_has(rb, "slice") ||
                 strstr(recv, "sub(") || strstr(recv, "ps") || strstr(recv, "Pt"))
            snprintf(dst, cap, "(%s).len", recv);
        else if (vty[0] && strncmp(vty, "CC", 2) == 0 && vty[2] >= 'A' &&
                 vty[2] <= 'Z') {
            /* CCShardMap.len → cc_shard_map_len (stdlib ladder; no Vec invent). */
            char snake[160];
            char tryc[160];
            size_t si = 0, ti = 2;
            for (; vty[ti] && si + 2 < sizeof(snake); ti++) {
                char c = vty[ti];
                if (c >= 'A' && c <= 'Z') {
                    if (si > 0) snake[si++] = '_';
                    snake[si++] = (char)(c - 'A' + 'a');
                } else
                    snake[si++] = c;
            }
            snake[si] = 0;
            if (!snake[0]) return 0;
            snprintf(tryc, sizeof(tryc), "cc_%s_len", snake);
            if (SHADOW_UFCS_PTR_RECV())
                snprintf(dst, cap, "%s(%s)", tryc, recv);
            else
                snprintf(dst, cap, "%s(&%s)", tryc, recv);
        } else
            return 0;
    } else if (is_arrow && strcmp(meth_name, "len") == 0) {
        /* CharVec* → CCVec_char; avoid Map_* false fallback. */
        char rty[128];
        const char* resolved;
        snprintf(rty, sizeof(rty), "%s", vty);
        resolved = rty[0] ? shadow_td_alias_resolve(rty) : NULL;
        if (resolved && resolved[0]) snprintf(rty, sizeof(rty), "%s", resolved);
        if (rty[0] && strncmp(rty, "CCVec_", 6) == 0)
            snprintf(dst, cap, "%s_len(%s)", rty, recv);
        else if (rty[0] && (strncmp(rty, "Map_", 4) == 0 ||
                            strncmp(rty, "ArrayMap_", 9) == 0 ||
                            shadow_ufcs_is_map_ty(rty)))
            snprintf(dst, cap, "%s_len(%s)", rty, recv);
        else if (rty[0] && strncmp(rty, "CCSlice_", 8) == 0)
            return shadow_ufcs_emit_slice_meth(rty, "len", recv, NULL, dst, cap);
        else if (strcmp(rty, "CCSlicePacked") == 0)
            snprintf(dst, cap, "cc_slice_packed_len(%s)", recv);
        else if (strcmp(rty, "CCString") == 0)
            snprintf(dst, cap, "cc_string_len(%s)", recv);
        else if (rty[0] && strncmp(rty, "CC", 2) == 0 && rty[2] >= 'A' &&
                 rty[2] <= 'Z') {
            char snake[160];
            char tryc[160];
            size_t si = 0, ti = 2;
            for (; rty[ti] && si + 2 < sizeof(snake); ti++) {
                char c = rty[ti];
                if (c >= 'A' && c <= 'Z') {
                    if (si > 0) snake[si++] = '_';
                    snake[si++] = (char)(c - 'A' + 'a');
                } else if (c == '*')
                    break;
                else
                    snake[si++] = c;
            }
            snake[si] = 0;
            if (!snake[0]) return 0;
            snprintf(tryc, sizeof(tryc), "cc_%s_len", snake);
            snprintf(dst, cap, "%s(%s)", tryc, recv);
        } else
            /* Never invent Map_CCSliceHdr_int_* — wrong-family silent C. */
            return 0;
    } else if (!is_arrow && strcmp(meth_name, "cap") == 0 &&
               (strcmp(vty, "CCString") == 0 || shadow_bind_ty_has(rb, "CCString") ||
                strstr(recv, "cc_string_") || strstr(recv, "CCString") ||
                strstr(recv, "__cc_ov_str"))) {
        /* CCString only — Map_/ArrayMap_/CCVec_ fall through to family invent. */
        snprintf(dst, cap, "cc_string_cap(&%s)", recv);
    } else if (is_arrow && strcmp(meth_name, "cap") == 0 &&
               (strcmp(vty, "CCString") == 0 || shadow_bind_ty_has(rb, "CCString"))) {
        snprintf(dst, cap, "cc_string_cap(%s)", recv);
    } else if (!is_arrow && strcmp(meth_name, "median") == 0) {
        if (vty[0] && strncmp(vty, "CCVec_", 6) == 0)
            snprintf(dst, cap, "%s_median(&%s)", vty, recv);
        else
            snprintf(dst, cap, "CCVec_double_median(&%s)", recv);
    } else if (!is_arrow && strcmp(meth_name, "push") == 0) {
        if (vty[0] && strncmp(vty, "CCVec_", 6) == 0)
            snprintf(dst, cap, "%s_push(&%s, %s)", vty, recv, a);
        else
            snprintf(dst, cap, "CCVec_double_push(&%s, %s)", recv, a);
    } else if (!is_arrow && strcmp(meth_name, "mean") == 0 &&
               vty[0] && strncmp(vty, "CCVec_", 6) == 0) {
        snprintf(dst, cap, "%s_mean(&%s)", vty, recv);
    } else if (!is_arrow && strcmp(meth_name, "println") == 0) {
        /* Handle form `io.println(x)` prints args; `@string(...).println()` /
         * `s.println()` print the receiver. */
        if (a && a[0])
            snprintf(dst, cap, "cc_println(%s)", a);
        else
            snprintf(dst, cap, "cc_println(%s)", recv);
    } else if (!is_arrow && strcmp(meth_name, "eprintln") == 0) {
        if (a && a[0])
            snprintf(dst, cap, "cc_eprintln(%s)", a);
        else
            snprintf(dst, cap, "cc_eprintln(%s)", recv);
    } else if (!is_arrow && strcmp(meth_name, "sub") == 0) {
        if (vty[0] && strncmp(vty, "CCSlice_", 8) == 0)
            return shadow_ufcs_emit_slice_meth(vty, "sub", recv, a, dst, cap);
        if (strstr(recv, "ps") || strstr(recv, "__cc_sl_")) {
            char a0[64] = {0}, a1[64] = {0};
            const char* comma = strchr(a, ',');
            if (comma) {
                size_t n0 = (size_t)(comma - a);
                if (n0 >= sizeof(a0)) n0 = sizeof(a0) - 1;
                memcpy(a0, a, n0);
                a0[n0] = 0;
                while (a0[0] == ' ') memmove(a0, a0 + 1, strlen(a0));
                snprintf(a1, sizeof(a1), "%s", comma + 1);
                while (a1[0] == ' ') memmove(a1, a1 + 1, strlen(a1));
                snprintf(dst, cap,
                         "((__typeof__(%s)){ (%s).ptr + (%s), "
                         "(size_t)((%s) - (%s)) })",
                         recv, recv, a0, a1, a0);
            } else {
                snprintf(dst, cap, "cc_slice_sub(%s, %s)", recv, a);
            }
        } else {
            snprintf(dst, cap, "cc_slice_sub(%s, %s)", recv, a);
        }
    } else if (!is_arrow && strcmp(meth_name, "index_of") == 0) {
        snprintf(dst, cap, "cc_slice_index_of(%s, %s)", recv, a);
    } else if (!is_arrow &&
               (strcmp(meth_name, "to_i64") == 0 ||
                strcmp(meth_name, "to_u64") == 0 ||
                strcmp(meth_name, "to_f64") == 0)) {
        /* char[:] / CCSlice → cc_slice_to_*; takes const CCSlice*. */
        if (shadow_ufcs_recv_is_ident(recv))
            snprintf(dst, cap, "cc_slice_%s(&%s)", meth_name, recv);
        else
            snprintf(dst, cap, "cc_slice_%s(&(%s))", meth_name, recv);
    } else if (strcmp(meth_name, "at") == 0 &&
               shadow_ufcs_ty_is(vty, rb, "CCShardMap")) {
        if (SHADOW_UFCS_PTR_RECV() || is_arrow)
            snprintf(dst, cap, "cc_shard_map_at(%s, %s)", recv, a);
        else
            snprintf(dst, cap, "cc_shard_map_at(&%s, %s)", recv, a);
    } else if (!is_arrow && strcmp(meth_name, "at") == 0) {
        if (vty[0] && strncmp(vty, "CCSlice_", 8) == 0)
            return shadow_ufcs_emit_slice_meth(vty, "at", recv, a, dst, cap);
        if (vty[0] && strncmp(vty, "Pair_", 5) == 0)
            snprintf(dst, cap, "%s_at(&%s, %s)", vty, recv, a);
        else if (g_shadow_nginst > 0 && shadow_bind_ty_has(rb, "Pair"))
            snprintf(dst, cap, "Pair_int_int_at(&%s, %s)", recv, a);
        else if (SHADOW_UFCS_PTR_RECV())
            snprintf(dst, cap, "cc_slice_at(%s, %s)", recv, a);
        else
            snprintf(dst, cap, "cc_slice_at(&%s, %s)", recv, a);
    } else if (!is_arrow && strcmp(meth_name, "bytes") == 0) {
        if (vty[0] && strncmp(vty, "CCSlice_", 8) == 0)
            return shadow_ufcs_emit_slice_meth(vty, "bytes", recv, NULL, dst,
                                              cap);
        snprintf(dst, cap, "(%s)", recv);
    } else if (!is_arrow && strcmp(meth_name, "allocT") == 0) {
        /* Type-formal member: ::[T] or pointer-dest element type. */
        const char* et = elem_ty;
        char peeled[128];
        peeled[0] = 0;
        if (et && et[0]) {
            size_t el = strlen(et);
            /* If dest formal still has a trailing *, peel for the element. */
            if (el > 1 && et[el - 1] == '*') {
                size_t n = el - 1;
                while (n && et[n - 1] == ' ') n--;
                if (n && n < sizeof(peeled)) {
                    memcpy(peeled, et, n);
                    peeled[n] = 0;
                    et = peeled;
                }
            }
        }
        if (!et || !et[0]) {
            if (!g_shadow_ufcs_miss) {
                fprintf(stderr, "error: arena.allocT needs its element type\n");
                g_shadow_ufcs_miss = 1;
            }
            return 0;
        }
        if (a[0])
            snprintf(dst, cap, "cc_arena_alloc_T_count(%s, &%s, %s)", et, recv, a);
        else
            snprintf(dst, cap, "cc_arena_alloc_T(%s, &%s)", et, recv);
    } else if (!is_arrow && strcmp(meth_name, "block_on") == 0) {
        if (!elem_ty || !elem_ty[0]) return 0;
        snprintf(dst, cap, "cc_block_on(%s, %s)", elem_ty, recv);
    } else if (!is_arrow && strcmp(meth_name, "map") == 0 &&
               (strcmp(vty, "CCPyObj") == 0 ||
                shadow_bind_ty_has(rb, "CCPyObj") || strstr(recv, "__r"))) {
        /* `f.map::[T](&arena, cols...)` → cc_py_obj_map(T, &f, …). */
        if (!elem_ty || !elem_ty[0]) return 0;
        if (a[0])
            snprintf(dst, cap, "cc_py_obj_map(%s, &%s, %s)", elem_ty, recv, a);
        else
            snprintf(dst, cap, "cc_py_obj_map(%s, &%s)", elem_ty, recv);
    } else if (!is_arrow && strcmp(meth_name, "as_map") == 0 &&
               (strcmp(vty, "CCPyObj") == 0 ||
                shadow_bind_ty_has(rb, "CCPyObj"))) {
        /* `obj.as_map::[K,V](&arena, m)` — elem_ty holds "K,V" compact. */
        if (!elem_ty || !elem_ty[0] || !a[0]) return 0;
        {
            char k[64], v[64];
            const char* us = strchr(elem_ty, '_');
            if (!us) us = strchr(elem_ty, ',');
            if (!us) return 0;
            {
                size_t kl = (size_t)(us - elem_ty);
                if (kl >= sizeof(k)) kl = sizeof(k) - 1;
                memcpy(k, elem_ty, kl);
                k[kl] = 0;
                snprintf(v, sizeof(v), "%s", us + 1);
            }
            snprintf(dst, cap, "cc_py_obj_as_map(%s, %s, &%s, %s)", k, v, recv,
                     a);
        }
    } else if (strcmp(meth_name, "append") == 0) {
        /* Value recv → &s; pointer recv (sp->append) → sp. */
        if (SHADOW_UFCS_PTR_RECV())
            snprintf(dst, cap, "cc_string_append(%s, %s)", recv, a);
        else
            snprintf(dst, cap, "cc_string_append(&%s, %s)", recv, a);
    } else if (!is_arrow && strcmp(meth_name, "detach") == 0) {
        /* Value field chains like p->arena.detach() still need &arena. */
        if (recv[0] == '&' || recv[0] == '*')
            snprintf(dst, cap, "cc_arena_detach(%s)", recv);
        else
            snprintf(dst, cap, "cc_arena_detach(&%s)", recv);
    } else if (!is_arrow && strcmp(meth_name, "recv") == 0) {
        /* Raw CCChan* only — not CCChanRx/Tx (substring-safe). */
        int raw_chan = 0;
        if (rb && rb->ty[0]) {
            char bty[96];
            shadow_bind_base_ty(rb, bty, sizeof(bty));
            raw_chan = (strcmp(bty, "CCChan") == 0);
        }
        if (!raw_chan && vty[0] &&
            (strcmp(vty, "CCChan") == 0 || strcmp(vty, "CCChan*") == 0))
            raw_chan = 1;
        if (raw_chan) {
            snprintf(dst, cap,
                     "cc_chan_result_with((%s), cc_channel_raw_recv((%s), "
                     "(void*)(%s), sizeof(*(%s))), 1)",
                     recv, recv, a, a);
        } else {
            snprintf(dst, cap, "cc_channel_recv(%s, %s)", recv, a);
        }
    } else if (!is_arrow && strcmp(meth_name, "send") == 0) {
        snprintf(dst, cap, "cc_channel_send(%s, %s)", recv, a);
    } else if (!is_arrow && strcmp(meth_name, "as_list") == 0 &&
               elem_ty && elem_ty[0]) {
        /* `obj.as_list::[T](&arena)` → cc_py_obj_as_list(T, &obj, &arena).
         * Must not fall through to ufcs_dynamic callm + CC_PY_ARG(arena). */
        int need_amp = !SHADOW_UFCS_PTR_RECV();
        char compact[128];
        shadow_ufcs_formal_compact(elem_ty, compact, sizeof(compact));
        if (!compact[0]) return 0;
        if (a[0])
            snprintf(dst, cap, "cc_py_obj_as_list(%s, %s%s, %s)", compact,
                     need_amp ? "&" : "", recv, a);
        else
            snprintf(dst, cap, "cc_py_obj_as_list(%s, %s%s, NULL)", compact,
                     need_amp ? "&" : "", recv);
        return 1;
    } else if (!is_arrow &&
               (strcmp(meth_name, "try_send_into") == 0 ||
                strcmp(meth_name, "send_into") == 0)) {
        const char* cfn = (strcmp(meth_name, "try_send_into") == 0)
                              ? "cc_channel_try_send_into"
                              : "cc_channel_send_into";
        if (shadow_bind_ty_has(rb, "CCChanTx") ||
            shadow_bind_ty_has(rb, "CCChan") ||
            (vty[0] && strncmp(vty, "CCChan", 6) == 0) ||
            (vty[0] && shadow_chan_find(vty))) {
            snprintf(dst, cap, "%s(%s, %s)", cfn, recv, a);
            return 1;
        }
        return 0;
    } else if (!is_arrow && strcmp(meth_name, "fetch_add") == 0) {
        snprintf(dst, cap, "cc_atomic_fetch_add(&%s, %s)", recv, a);
    } else if (is_arrow && strcmp(meth_name, "wait") == 0) {
        if (!shadow_ufcs_ty_is(vty, rb, "CCNursery")) return 0;
        snprintf(dst, cap, "cc_nursery_wait(%s)", recv);
    } else if (is_arrow &&
               (strcmp(meth_name, "spawn") == 0 ||
                strcmp(meth_name, "spawn_async") == 0)) {
        /* One surface: n->spawn(...).
         *   () => {…}     → sync fiber (closure0)
         *   async_fn(…)   → @async task (named spawn_async)
         * spawn_async kept as an explicit alias for the task form. */
        char callee_name[96];
        size_t ci = 0, cj = 0;
        if (!shadow_ufcs_ty_is(vty, rb, "CCNursery")) return 0;
        if (strcmp(meth_name, "spawn") == 0 && strstr(a, "=>")) {
            snprintf(dst, cap, "cc_nursery_spawn_closure0(%s, %s)", recv, a);
            return 1;
        }
        while (a[ci] && (a[ci] == ' ' || a[ci] == '\t')) ci++;
        while (a[ci] && cj + 1 < sizeof(callee_name) &&
               ((a[ci] >= 'A' && a[ci] <= 'Z') || (a[ci] >= 'a' && a[ci] <= 'z') ||
                (a[ci] >= '0' && a[ci] <= '9') || a[ci] == '_' || a[ci] == ':')) {
            callee_name[cj++] = a[ci++];
        }
        callee_name[cj] = 0;
        if (cj == 0 || a[ci] != '(')
            snprintf(callee_name, sizeof(callee_name), "%s", "<async>");
        snprintf(dst, cap,
                 "cc_nursery_spawn_async_named(%s, %s, \"%s\", __FILE__, __LINE__)",
                 recv, a, callee_name);
    } else if (is_arrow && strcmp(meth_name, "spawnhybrid") == 0) {
        if (!shadow_ufcs_ty_is(vty, rb, "CCNursery")) return 0;
        snprintf(dst, cap, "cc_nursery_spawnhybrid_closure0(%s, %s)", recv, a);
    } else if (is_arrow && strcmp(meth_name, "close_on") == 0) {
        if (!shadow_ufcs_ty_is(vty, rb, "CCNursery")) return 0;
        snprintf(dst, cap, "cc_nursery_add_closing_tx(%s, %s)", recv, a);
    } else if (is_arrow && strcmp(meth_name, "mutex") == 0) {
        if (!shadow_ufcs_ty_is(vty, rb, "CCExclusive")) return 0;
        snprintf(dst, cap, "cc_exclusive_mutex(%s, %s)", recv, a);
    } else if (is_arrow && strcmp(meth_name, "destroy") == 0) {
        /* Pointer receivers: type-dispatch only. Unknown → fail loud (never
         * invent cc_exclusive_destroy — that miscompiled CCArena*). */
        char vbase[96];
        char dhook[96];
        shadow_ufcs_vty_base(vty, vbase, sizeof(vbase));
        if (shadow_ufcs_is_map_ty(vbase) || shadow_ufcs_is_map_ty(vty))
            snprintf(dst, cap, "%s_destroy(%s)",
                     vbase[0] ? vbase : vty, recv);
        else if (vbase[0] && (strncmp(vbase, "ArrayMap_", 9) == 0 ||
                              strncmp(vbase, "Map_", 4) == 0 ||
                              strncmp(vbase, "CCVec_", 6) == 0))
            snprintf(dst, cap, "%s_destroy(%s)", vbase, recv);
        else if (shadow_ufcs_ty_is(vty, rb, "CCArenaPool") ||
                 strstr(recv, "pool"))
            snprintf(dst, cap, "cc_arena_pool_destroy(%s)", recv);
        else if (shadow_ufcs_ty_is(vty, rb, "CCArena") ||
                 strstr(recv, "arena") || strstr(recv, "detached"))
            snprintf(dst, cap, "cc_arena_destroy(%s)", recv);
        else if (shadow_ufcs_ty_is(vty, rb, "CCExclusiveGuard"))
            snprintf(dst, cap, "cc_exclusive_guard_destroy(%s)", recv);
        else if (shadow_ufcs_ty_is(vty, rb, "CCNursery"))
            snprintf(dst, cap, "cc_nursery_free(%s)", recv);
        else if (shadow_ufcs_ty_is(vty, rb, "CCExclusive"))
            snprintf(dst, cap, "cc_exclusive_destroy(%s)", recv);
        else if (vbase[0] && vbase[0] >= 'A' && vbase[0] <= 'Z') {
            snprintf(dhook, sizeof(dhook), "%s_destroy", vbase);
            if (shadow_ufn_exists(dhook))
                snprintf(dst, cap, "%s(%s)", dhook, recv);
            else
                return 0;
        } else
            return 0;
    } else if (!is_arrow && strcmp(meth_name, "acquire") == 0) {
        if (!shadow_ufcs_ty_is(vty, rb, "CCExclusiveMutex")) return 0;
        snprintf(dst, cap, "cc_exclusive_mutex_acquire(&%s)", recv);
    } else if (is_arrow && strcmp(meth_name, "acquire") == 0) {
        /* CCExclusive* → cc_exclusive_acquire(excl, name). */
        if (!shadow_ufcs_ty_is(vty, rb, "CCExclusive")) return 0;
        snprintf(dst, cap, "cc_exclusive_acquire(%s, %s)", recv, a);
    } else if (!is_arrow && strcmp(meth_name, "release") == 0) {
        /* CCString.release(arena) vs CCExclusiveGuard.release(). */
        if (shadow_ufcs_ty_is(vty, rb, "CCString"))
            snprintf(dst, cap, "cc_string_release(&%s, %s)", recv, a);
        else if (shadow_ufcs_ty_is(vty, rb, "CCSlicePacked"))
            /* C twin is arena-first; UFCS keeps receiver-first / arena-last. */
            snprintf(dst, cap, "cc_slice_packed_release(%s, &%s)", a, recv);
        else if (shadow_ufcs_ty_is(vty, rb, "CCExclusiveGuard"))
            snprintf(dst, cap, "cc_exclusive_guard_release(&%s)", recv);
        else if (shadow_ufcs_ty_is(vty, rb, "CCShardHold"))
            snprintf(dst, cap, "cc_shard_hold_release(&%s)", recv);
        else
            return 0;
    } else if (is_arrow && strcmp(meth_name, "release") == 0) {
        if (shadow_ufcs_ty_is(vty, rb, "CCString"))
            snprintf(dst, cap, "cc_string_release(%s, %s)", recv, a);
        else if (shadow_ufcs_ty_is(vty, rb, "CCSlicePacked"))
            snprintf(dst, cap, "cc_slice_packed_release(%s, %s)", a, recv);
        else if (shadow_ufcs_ty_is(vty, rb, "CCExclusiveGuard"))
            snprintf(dst, cap, "cc_exclusive_guard_release(%s)", recv);
        else if (shadow_ufcs_ty_is(vty, rb, "CCShardHold"))
            snprintf(dst, cap, "cc_shard_hold_release(%s)", recv);
        else
            return 0;
    } else if (!is_arrow && strcmp(meth_name, "destroy") == 0 &&
               (strcmp(vty, "CCExclusiveGuard") == 0 ||
                (rb && strcmp(rb->ty, "CCExclusiveGuard") == 0))) {
        snprintf(dst, cap, "cc_exclusive_guard_destroy(&%s)", recv);
    } else if (!is_arrow && strcmp(meth_name, "destroy") == 0 &&
               (strcmp(vty, "CCArenaPool") == 0 ||
                (rb && strcmp(rb->ty, "CCArenaPool") == 0))) {
        snprintf(dst, cap, "cc_arena_pool_destroy(&%s)", recv);
    } else if (!is_arrow && strcmp(meth_name, "destroy") == 0 &&
               (strcmp(vty, "CCArena") == 0 ||
                (rb && strcmp(rb->ty, "CCArena") == 0) ||
                /* Field paths: store->map_arena — bind is Store*, not CCArena. */
                strstr(recv, "arena") || strstr(recv, "detached"))) {
        snprintf(dst, cap, "cc_arena_destroy(&%s)", recv);
    } else if (!is_arrow && strcmp(meth_name, "destroy") == 0 && vty[0] &&
               (strncmp(vty, "ArrayMap_", 9) == 0 ||
                strncmp(vty, "Map_", 4) == 0 ||
                strncmp(vty, "CCVec_", 6) == 0)) {
        if (SHADOW_UFCS_PTR_RECV())
            snprintf(dst, cap, "%s_destroy(%s)", vty, recv);
        else
            snprintf(dst, cap, "%s_destroy(&%s)", vty, recv);
    } else if (!is_arrow && strcmp(meth_name, "destroy") == 0 && vty[0] &&
               vty[0] >= 'A' && vty[0] <= 'Z') {
        /* draft_as §3: registered deltas + recursive @as flatten (no synth). */
        char body[768];
        size_t bo = 0;
        int have;
        char dhook[96];
        bo += (size_t)snprintf(body + bo, sizeof(body) - bo, "({ ");
        have = shadow_as_destroy_append(body, &bo, sizeof(body), vty, recv);
        /* Stdlib CCFoo.destroy → cc_<snake>_destroy (CCShardMap, …). */
        if (!have && strncmp(vty, "CC", 2) == 0 && vty[2] >= 'A' &&
            vty[2] <= 'Z') {
            char snake[160];
            size_t si = 0, ti = 2;
            for (; vty[ti] && si + 2 < sizeof(snake); ti++) {
                char c = vty[ti];
                if (c >= 'A' && c <= 'Z') {
                    if (si > 0) snake[si++] = '_';
                    snake[si++] = (char)(c - 'A' + 'a');
                } else
                    snake[si++] = c;
            }
            snake[si] = 0;
            if (snake[0]) {
                snprintf(dhook, sizeof(dhook), "cc_%s_destroy", snake);
                if (bo + 48 < sizeof(body)) {
                    bo += (size_t)snprintf(body + bo, sizeof(body) - bo,
                                           "%s(&%s); ", dhook, recv);
                    have = 1;
                }
            }
        }
        if (!have) {
            snprintf(dhook, sizeof(dhook), "%s_destroy", vty);
            if (shadow_ufn_exists(dhook) && bo + 48 < sizeof(body)) {
                bo += (size_t)snprintf(body + bo, sizeof(body) - bo,
                                       "%s(&%s); ", dhook, recv);
                have = 1;
            }
        }
        if (bo + 3 < sizeof(body))
            snprintf(body + bo, sizeof(body) - bo, "})");
        else
            return 0;
        if (have)
            snprintf(dst, cap, "%s", body);
        else
            return 0;
    } else if (!is_arrow && strcmp(meth_name, "reset") == 0 &&
               (strcmp(vty, "CCArena") == 0 ||
                shadow_bind_ty_has(rb, "CCArena"))) {
        /* Only CCArena.reset — user types (MyWidget.reset) use snake UFCS. */
        snprintf(dst, cap, "cc_arena_reset(&%s)", recv);
    } else if (!is_arrow && strcmp(meth_name, "store") == 0) {
        snprintf(dst, cap, "cc_atomic_store(&%s, %s)", recv, a);
    } else if (!is_arrow && strcmp(meth_name, "cas") == 0) {
        snprintf(dst, cap, "cc_atomic_cas(&%s, %s)", recv, a);
    } else if (!is_arrow && strcmp(meth_name, "peer_addr") == 0) {
        if (SHADOW_UFCS_PTR_RECV())
            snprintf(dst, cap, "cc_socket_peer_addr(%s, %s)", recv, a);
        else
            snprintf(dst, cap, "cc_socket_peer_addr(&%s, %s)", recv, a);
    } else if (!is_arrow && strcmp(meth_name, "read") == 0) {
        if (shadow_bind_ty_has(rb, "CCFile") ||
            (vty[0] && strcmp(vty, "CCFile") == 0)) {
            if (SHADOW_UFCS_PTR_RECV())
                snprintf(dst, cap, "cc_file_read(%s, %s)", recv, a);
            else
                snprintf(dst, cap, "cc_file_read(&%s, %s)", recv, a);
        } else if (shadow_bind_ty_has(rb, "CCSocket") ||
                   (vty[0] && strcmp(vty, "CCSocket") == 0)) {
            if (SHADOW_UFCS_PTR_RECV())
                snprintf(dst, cap, "cc_socket_read(%s, %s)", recv, a);
            else
                snprintf(dst, cap, "cc_socket_read(&%s, %s)", recv, a);
        } else
            return 0;
    } else if (!is_arrow && strcmp(meth_name, "write") == 0) {
        if (strcmp(recv, "cc_std_out") == 0 || strcmp(recv, "std_out") == 0)
            snprintf(dst, cap, "cc_std_out_write_auto(%s)", a);
        else if (strcmp(recv, "cc_std_err") == 0 || strcmp(recv, "std_err") == 0)
            snprintf(dst, cap, "cc_std_err_write_auto(%s)", a);
        else if (shadow_bind_ty_has(rb, "CCFile") ||
                 (vty[0] && strcmp(vty, "CCFile") == 0)) {
            if (SHADOW_UFCS_PTR_RECV())
                snprintf(dst, cap, "cc_file_write(%s, %s)", recv, a);
            else
                snprintf(dst, cap, "cc_file_write(&%s, %s)", recv, a);
        } else if (shadow_bind_ty_has(rb, "CCSocket") ||
                   (vty[0] && strcmp(vty, "CCSocket") == 0)) {
            if (SHADOW_UFCS_PTR_RECV())
                snprintf(dst, cap, "cc_socket_write(%s, %s)", recv, a);
            else
                snprintf(dst, cap, "cc_socket_write(&%s, %s)", recv, a);
        } else
            return 0; /* no ambient write by method name */
    } else if (strcmp(meth_name, "to_slice_n") == 0) {
        /* `p->to_slice_n(n)` — counted; no strlen. */
        const char* ty = rb ? rb->ty : vty;
        int is_unsigned = ty && strstr(ty, "unsigned char") != NULL;
        int is_signed = ty && strstr(ty, "signed char") != NULL && !is_unsigned;
        int is_const = ty && strstr(ty, "const") != NULL;
        const char* fn;
        if (is_unsigned && is_const) fn = "const_unsigned_char_to_slice_n";
        else if (is_unsigned) fn = "unsigned_char_to_slice_n";
        else if (is_signed && is_const) fn = "const_signed_char_to_slice_n";
        else if (is_signed) fn = "signed_char_to_slice_n";
        else if (is_const || (ty && strstr(ty, "char")))
            fn = is_const ? "const_char_to_slice_n" : "char_to_slice_n";
        else
            fn = is_arrow ? "const_char_to_slice_n" : "char_to_slice_n";
        snprintf(dst, cap, "%s(%s, %s)", fn, recv, a);
    } else if (!is_arrow && strcmp(meth_name, "read_into") == 0) {
        if (SHADOW_UFCS_PTR_RECV())
            snprintf(dst, cap, "cc_socket_read_into(%s, %s)", recv, a);
        else
            snprintf(dst, cap, "cc_socket_read_into(&%s, %s)", recv, a);
    } else if (!is_arrow && strcmp(meth_name, "accept") == 0) {
        if (SHADOW_UFCS_PTR_RECV())
            snprintf(dst, cap, "cc_listener_accept(%s)", recv);
        else
            snprintf(dst, cap, "cc_listener_accept(&%s)", recv);
    } else if (!is_arrow && strcmp(meth_name, "send_task") == 0) {
        if (strstr(a, "=>")) return 0;
        snprintf(dst, cap, "cc_channel_send_task(%s, %s)", recv, a);
    } else if (!is_arrow && strcmp(meth_name, "send_task_hybrid") == 0) {
        if (strstr(a, "=>")) return 0;
        snprintf(dst, cap, "cc_channel_send_task_hybrid(%s, %s)", recv, a);
    } else if (!is_arrow && strcmp(meth_name, "valid") == 0) {
        snprintf(dst, cap, "cc_arena_valid(&%s)", recv);
    } else if (!is_arrow && strcmp(meth_name, "set_heap_overflow") == 0 &&
               (strcmp(vty, "CCArena") == 0 ||
                shadow_bind_ty_has(rb, "CCArena") ||
                strstr(recv, "arena"))) {
        /* Field paths: sh->arena.set_heap_overflow(true). */
        snprintf(dst, cap, "cc_arena_set_heap_overflow(&%s, %s)", recv, a);
    } else if (is_arrow && strcmp(meth_name, "set_heap_overflow") == 0 &&
               (strcmp(vty, "CCArena") == 0 ||
                shadow_bind_ty_has(rb, "CCArena") ||
                strstr(recv, "arena"))) {
        snprintf(dst, cap, "cc_arena_set_heap_overflow(%s, %s)", recv, a);
    } else if (strcmp(meth_name, "init") == 0 || strcmp(meth_name, "put") == 0 ||
               strcmp(meth_name, "get") == 0 || strcmp(meth_name, "delete") == 0 ||
               strcmp(meth_name, "insert_entry") == 0 ||
               strcmp(meth_name, "key_logical_live_bytes") == 0) {
        /* Typed container methods beat the ArrayMap `store_*` family. */
        int ptr_recv = SHADOW_UFCS_PTR_RECV();
        if (vty[0] && (strncmp(vty, "CCVec_", 6) == 0 ||
                       strncmp(vty, "Map_", 4) == 0 ||
                       strncmp(vty, "ArrayMap_", 9) == 0)) {
            if (a[0]) {
                if (ptr_recv)
                    snprintf(dst, cap, "%s_%s(%s, %s)", vty, meth_name, recv, a);
                else
                    snprintf(dst, cap, "%s_%s(&%s, %s)", vty, meth_name, recv, a);
            } else if (ptr_recv) {
                snprintf(dst, cap, "%s_%s(%s)", vty, meth_name, recv);
            } else {
                snprintf(dst, cap, "%s_%s(&%s)", vty, meth_name, recv);
            }
        } else if (vty[0]) {
            /* Typed user struct: Foo_get / struct Foo_get — not store_*. */
            char tag[128];
            char tryc[160];
            const char* bt = vty;
            if (strncmp(bt, "struct ", 7) == 0) bt += 7;
            snprintf(tag, sizeof(tag), "%s", bt);
            if (tag[0] >= 'A' && tag[0] <= 'Z' && strncmp(tag, "CC", 2) != 0 &&
                strncmp(tag, "Map_", 4) != 0 &&
                strncmp(tag, "ArrayMap_", 9) != 0) {
                char snake[160];
                size_t si = 0, ti = 0;
                const char* callee = NULL;
                snprintf(tryc, sizeof(tryc), "%s_%s", tag, meth_name);
                if (shadow_ufn_exists(tryc))
                    callee = tryc;
                else if (shadow_ufn_exists(meth_name))
                    callee = meth_name;
                else {
                    /* Store.get → store_get (snake family, TU-declared). */
                    for (; tag[ti] && si + 2 < sizeof(snake); ti++) {
                        char c = tag[ti];
                        if (c >= 'A' && c <= 'Z') {
                            if (si > 0) snake[si++] = '_';
                            snake[si++] = (char)(c - 'A' + 'a');
                        } else
                            snake[si++] = c;
                    }
                    snake[si] = 0;
                    if (snake[0]) {
                        snprintf(tryc, sizeof(tryc), "%s_%s", snake, meth_name);
                        if (shadow_ufn_exists(tryc)) callee = tryc;
                    }
                }
                if (callee) {
                    if (a[0]) {
                        if (ptr_recv)
                            snprintf(dst, cap, "%s(%s, %s)", callee, recv, a);
                        else
                            snprintf(dst, cap, "%s(&%s, %s)", callee, recv, a);
                    } else if (ptr_recv) {
                        snprintf(dst, cap, "%s(%s)", callee, recv);
                    } else {
                        snprintf(dst, cap, "%s(&%s)", callee, recv);
                    }
                } else if (shadow_ufcs_emit_bare(recv, meth_name, a, ptr_recv,
                                                tag, 0, dst, cap)) {
                    /* bare */
                } else {
                    snprintf(tryc, sizeof(tryc), "%s_%s", tag, meth_name);
                    if (a[0]) {
                        if (ptr_recv)
                            snprintf(dst, cap, "%s(%s, %s)", tryc, recv, a);
                        else
                            snprintf(dst, cap, "%s(&%s, %s)", tryc, recv, a);
                    } else if (ptr_recv) {
                        snprintf(dst, cap, "%s(%s)", tryc, recv);
                    } else {
                        snprintf(dst, cap, "%s(&%s)", tryc, recv);
                    }
                }
            } else if (strncmp(tag, "CC", 2) == 0 && tag[2] >= 'A' &&
                       tag[2] <= 'Z') {
                char cc_meth[160];
                {
                    char ccs[128];
                    char base[128];
                    size_t ci = 3;
                    size_t ti;
                    const char* r;
                    /* CCShard* → cc_shard_get (strip pointer stars). */
                    shadow_ufcs_vty_base(tag, base, sizeof(base));
                    r = base[0] ? base + 2 : tag + 2;
                    ccs[0] = 'c'; ccs[1] = 'c'; ccs[2] = '_';
                    for (ti = 0; r[ti] && ci + 2 < sizeof(ccs); ti++) {
                        char c = r[ti];
                        if (c == '*') break;
                        if (c >= 'A' && c <= 'Z') {
                            if (ti > 0) ccs[ci++] = '_';
                            ccs[ci++] = (char)(c - 'A' + 'a');
                        } else if ((c >= 'a' && c <= 'z') ||
                                   (c >= '0' && c <= '9') || c == '_')
                            ccs[ci++] = c;
                    }
                    ccs[ci] = 0;
                    snprintf(cc_meth, sizeof(cc_meth), "%s_%s", ccs, meth_name);
                }
                if (a[0]) {
                    if (ptr_recv)
                        snprintf(dst, cap, "%s(%s, %s)", cc_meth, recv, a);
                    else
                        snprintf(dst, cap, "%s(&%s, %s)", cc_meth, recv, a);
                } else if (ptr_recv) {
                    snprintf(dst, cap, "%s(%s)", cc_meth, recv);
                } else {
                    snprintf(dst, cap, "%s(&%s)", cc_meth, recv);
                }
            } else {
                snprintf(tryc, sizeof(tryc), "store_%s", meth_name);
                if (a[0]) {
                    if (ptr_recv)
                        snprintf(dst, cap, "%s(%s, %s)", tryc, recv, a);
                    else
                        snprintf(dst, cap, "%s(&%s, %s)", tryc, recv, a);
                } else if (ptr_recv) {
                    snprintf(dst, cap, "%s(%s)", tryc, recv);
                } else {
                    snprintf(dst, cap, "%s(&%s)", tryc, recv);
                }
            }
        } else if (strcmp(meth_name, "init") == 0 ||
                   strcmp(meth_name, "put") == 0 ||
                   strcmp(meth_name, "get") == 0 ||
                   strcmp(meth_name, "delete") == 0 ||
                   strcmp(meth_name, "insert_entry") == 0 ||
                   strcmp(meth_name, "key_logical_live_bytes") == 0) {
            /* Untyped recv (opaque blobs): unique Type_meth / cc_*_meth only.
             * Never invent store_* — that hid missing binds as a fake symbol. */
            const char* u = shadow_ufn_unique_for_meth(meth_name);
            if (!u) return 0;
            if (a[0])
                snprintf(dst, cap,
                         ptr_recv ? "%s(%s, %s)" : "%s(&%s, %s)", u, recv, a);
            else
                snprintf(dst, cap, ptr_recv ? "%s(%s)" : "%s(&%s)", u, recv);
        }
    } else if (!is_arrow && strcmp(meth_name, "as_i64") == 0 &&
               (strcmp(vty, "CCPyObj") == 0 ||
                shadow_bind_ty_has(rb, "CCPyObj") ||
                strstr(recv, "__r") != NULL)) {
        snprintf(dst, cap, "cc_py_obj_as_i64(&%s)", recv);
    } else if (!is_arrow && strcmp(meth_name, "as_slice_into") == 0 &&
               (strcmp(vty, "CCPyObj") == 0 ||
                shadow_bind_ty_has(rb, "CCPyObj") || strstr(recv, "__r"))) {
        if (a[0])
            snprintf(dst, cap, "cc_py_obj_as_slice_into(&%s, %s)", recv, a);
        else
            snprintf(dst, cap, "cc_py_obj_as_slice_into(&%s)", recv);
    } else if (!is_arrow && strcmp(meth_name, "as_slice") == 0) {
        if (strcmp(vty, "CCSlicePacked") == 0)
            snprintf(dst, cap, "cc_slice_packed_as_slice(&%s)", recv);
        else if (vty[0] && strncmp(vty, "CCVec_", 6) == 0)
            snprintf(dst, cap, "%s_as_slice(&%s)", vty, recv);
        else if (strcmp(vty, "CCPyObj") == 0 ||
                 shadow_bind_ty_has(rb, "CCPyObj"))
            snprintf(dst, cap, "cc_py_obj_as_slice(&%s)", recv);
        else if (strstr(recv, "cc_string_new") || strstr(recv, "__cc_tpl") ||
                 (recv[0] == '(' && recv[1] == '{'))
            /* @string(`…`) lowered to a GNU stmt-expr yielding CCString. */
            snprintf(dst, cap,
                     "({ CCString __cc_as_s = %s; cc_string_as_slice(&__cc_as_s); })",
                     recv);
        else if (shadow_bind_ty_has(rb, "CCString") || strstr(recv, "string") ||
                 strstr(recv, "str") || strstr(recv, "CCString") ||
                 strcmp(vty, "CCString") == 0)
            snprintf(dst, cap, "cc_string_as_slice(&%s)", recv);
        else if (vty[0] && strncmp(vty, "CCSlice", 7) == 0)
            snprintf(dst, cap, "(%s)", recv);
        else
            snprintf(dst, cap, "cc_string_as_slice(&%s)", recv);
    } else if (is_arrow && strcmp(meth_name, "as_slice") == 0) {
        /* CCString* p; p->as_slice() → cc_string_as_slice(p) (same shape as
         * len/cap/release). Dot form takes &recv; arrow already has a pointer.
         * Unknown vty still defaults to CCString — opaque string-switch bodies
         * never register locals, so a hard miss would stall peel_left. */
        if (strcmp(vty, "CCSlicePacked") == 0)
            snprintf(dst, cap, "cc_slice_packed_as_slice(%s)", recv);
        else if (vty[0] && strncmp(vty, "CCVec_", 6) == 0)
            snprintf(dst, cap, "%s_as_slice(%s)", vty, recv);
        else if (strcmp(vty, "CCPyObj") == 0 ||
                 shadow_bind_ty_has(rb, "CCPyObj"))
            snprintf(dst, cap, "cc_py_obj_as_slice(%s)", recv);
        else if (vty[0] && strncmp(vty, "CCSlice", 7) == 0)
            snprintf(dst, cap, "(*(%s))", recv);
        else
            snprintf(dst, cap, "cc_string_as_slice(%s)", recv);
    } else if (!is_arrow && strcmp(meth_name, "is_inline") == 0 &&
               strcmp(vty, "CCSlicePacked") == 0) {
        snprintf(dst, cap, "cc_slice_packed_is_inline(&%s)", recv);
    } else if (!is_arrow && strcmp(meth_name, "eq_cstr") == 0) {
        /* slice.eq_cstr("…") or as_slice().eq_cstr — always cc_slice_eq_cstr. */
        if (strstr(recv, "cc_string_as_slice") || strstr(recv, "as_slice"))
            snprintf(dst, cap,
                     "({ CCSlice __cc_eq = %s; cc_slice_eq_cstr(&__cc_eq, %s); })",
                     recv, a);
        else
            snprintf(dst, cap, "cc_slice_eq_cstr(&%s, %s)", recv, a);
    } else if (!is_arrow && strcmp(meth_name, "to_str") == 0) {
        if (vty && vty[0] && strchr(vty, '*') == NULL) {
            char callee[128];
            snprintf(callee, sizeof(callee), "%s_to_str", vty);
            if (shadow_ufn_exists(callee)) {
                snprintf(dst, cap, "%s(&%s, %s)", callee, recv, a);
                return 1;
            }
        }
        /* cc_string_from _Generic arms take values (int/float/CCString/…),
         * not addresses — &recv only belongs on T_to_str helpers above. */
        snprintf(dst, cap, "cc_string_from((%s), (%s))", recv, a);
    } else if (strcmp(meth_name, "get_ptr") == 0) {
        int ptr_recv = SHADOW_UFCS_PTR_RECV();
        if (vty[0] && strncmp(vty, "CCVec_", 6) == 0) {
            if (ptr_recv)
                snprintf(dst, cap, "%s_get_ptr(%s, %s)", vty, recv, a);
            else
                snprintf(dst, cap, "%s_get_ptr(&%s, %s)", vty, recv, a);
        } else if (shadow_ufcs_is_map_ty(vty)) {
            if (ptr_recv)
                snprintf(dst, cap, "%s_get_ptr(%s, %s)", vty, recv, a);
            else
                snprintf(dst, cap, "%s_get_ptr(&%s, %s)", vty, recv, a);
        } else
            return 0;
    } else if (!is_arrow && strcmp(meth_name, "insert") == 0) {
        int ptr_recv = SHADOW_UFCS_PTR_RECV();
        char mapty[128];
        size_t mn;
        mapty[0] = 0;
        if (vty[0])
            snprintf(mapty, sizeof(mapty), "%s", vty);
        else if (rb && rb->ty[0])
            shadow_bind_base_ty(rb, mapty, sizeof(mapty));
        mn = strlen(mapty);
        while (mn && (mapty[mn - 1] == '*' || mapty[mn - 1] == ' ' ||
                      mapty[mn - 1] == '\t'))
            mapty[--mn] = 0;
        if (shadow_ufcs_is_map_ty(mapty)) {
            if (ptr_recv || (rb && strchr(rb->ty, '*')))
                snprintf(dst, cap, "%s_insert(%s, %s)", mapty, recv, a);
            else
                snprintf(dst, cap, "%s_insert(&%s, %s)", mapty, recv, a);
        } else
            return 0;
    } else if (strcmp(meth_name, "del") == 0 ||
               strcmp(meth_name, "remove") == 0) {
        int ptr_recv = SHADOW_UFCS_PTR_RECV();
        const char* meth =
            (strcmp(meth_name, "del") == 0) ? "del" : "remove";
        if (shadow_ufcs_is_map_ty(vty)) {
            if (ptr_recv)
                snprintf(dst, cap, "%s_%s(%s, %s)", vty, meth, recv, a);
            else
                snprintf(dst, cap, "%s_%s(&%s, %s)", vty, meth, recv, a);
        } else
            return 0;
    } else if (!is_arrow && strcmp(meth_name, "materialize_in") == 0) {
        /* Value field paths (`reply->data.materialize_in`) need &data. */
        int ptr_recv = SHADOW_UFCS_PTR_RECV();
        if (strcmp(vty, "CCString") == 0 || shadow_bind_ty_has(rb, "CCString")) {
            if (ptr_recv)
                snprintf(dst, cap, "cc_string_materialize_in(%s, %s)", recv, a);
            else
                snprintf(dst, cap, "cc_string_materialize_in(&(%s), %s)", recv, a);
        } else if (ptr_recv) {
            snprintf(dst, cap, "cc_slice_materialize_in(%s, %s)", recv, a);
        } else {
            snprintf(dst, cap, "cc_slice_materialize_in(&(%s), %s)", recv, a);
        }
    } else if (!is_arrow && strcmp(meth_name, "clear") == 0) {
        int ptr_recv = SHADOW_UFCS_PTR_RECV();
        if (strcmp(vty, "CCString") == 0 || shadow_bind_ty_has(rb, "CCString")) {
            if (ptr_recv)
                snprintf(dst, cap, "cc_string_clear(%s)", recv);
            else
                snprintf(dst, cap, "cc_string_clear(&%s)", recv);
        } else if (vty[0] && (strncmp(vty, "Map_", 4) == 0 ||
                       strncmp(vty, "ArrayMap_", 9) == 0 ||
                       strncmp(vty, "CCVec_", 6) == 0)) {
            if (ptr_recv)
                snprintf(dst, cap, "%s_clear(%s)", vty, recv);
            else
                snprintf(dst, cap, "%s_clear(&%s)", vty, recv);
        } else
            return 0;
    } else if (is_arrow && strcmp(meth_name, "clear") == 0) {
        char vbase[96];
        shadow_ufcs_vty_base(vty, vbase, sizeof(vbase));
        if (shadow_ufcs_ty_is(vty, rb, "CCString"))
            snprintf(dst, cap, "cc_string_clear(%s)", recv);
        else if (vbase[0] && (strncmp(vbase, "Map_", 4) == 0 ||
                              strncmp(vbase, "ArrayMap_", 9) == 0 ||
                              strncmp(vbase, "CCVec_", 6) == 0))
            snprintf(dst, cap, "%s_clear(%s)", vbase, recv);
        else
            return 0;
    } else if (strcmp(meth_name, "free") == 0) {
        /* Nursery*→free; ArenaPool.free(ptr); Arena.free()→destroy;
         * chan → cc_channel_free; Store→store_free. Unknown arrow → fail loud.
         * Note: check CCArenaPool before CCArena (exact ty_is). */
        if (shadow_ufcs_ty_is(vty, rb, "CCNursery")) {
            snprintf(dst, cap, "cc_nursery_free(%s)", recv);
        } else if (shadow_ufcs_ty_is(vty, rb, "CCChanTx") ||
                   shadow_ufcs_ty_is(vty, rb, "CCChanRx") ||
                   shadow_ufcs_ty_is(vty, rb, "CCChan") ||
                   (vty[0] && strncmp(vty, "CCChan", 6) == 0)) {
            snprintf(dst, cap, "cc_channel_free(%s)", recv);
        } else if (a[0] &&
                   (shadow_ufcs_ty_is(vty, rb, "CCArenaPool") ||
                    strstr(recv, "pool") != NULL ||
                    strstr(recv, "_stack") != NULL)) {
            if (is_arrow || SHADOW_UFCS_PTR_RECV())
                snprintf(dst, cap, "cc_arena_pool_free(%s, %s)", recv, a);
            else
                snprintf(dst, cap, "cc_arena_pool_free(&%s, %s)", recv, a);
        } else if (shadow_ufcs_ty_is(vty, rb, "CCArena") ||
                   strstr(recv, "arena") || strstr(recv, "detached")) {
            if (is_arrow || recv[0] == '&' || recv[0] == '*')
                snprintf(dst, cap, "cc_arena_destroy(%s)", recv);
            else
                snprintf(dst, cap, "cc_arena_destroy(&%s)", recv);
        } else if (shadow_ufcs_ty_is(vty, rb, "CCExclusiveMutex")) {
            if (is_arrow)
                snprintf(dst, cap, "cc_exclusive_mutex_free(%s)", recv);
            else
                snprintf(dst, cap, "cc_exclusive_mutex_free(&%s)", recv);
        } else if (shadow_ufcs_ty_is(vty, rb, "Store") ||
                   (!is_arrow && (strstr(recv, "store") != NULL))) {
            /* Recipe Store beachhead; arrow requires typed Store. */
            if (SHADOW_UFCS_PTR_RECV())
                snprintf(dst, cap, "store_free(%s)", recv);
            else
                snprintf(dst, cap, "store_free(&%s)", recv);
        } else if (is_arrow) {
            return 0; /* never invent store_free / exclusive for unknown * */
        } else {
            snprintf(dst, cap, "store_free(&%s)", recv);
        }
    } else if (!is_arrow && strcmp(meth_name, "head") == 0) {
        /* Prefer typed mangled callee when the receiver type is known
         * (declared twin or factory instance) — avoids huge _Generic and
         * leftover-buffer truncation in multi-UFCS printf lines. */
        if (vty[0] && (strncmp(vty, "Pair_", 5) == 0 ||
                       shadow_family_accepts(vty, "head") ||
                       shadow_ginst_has_mangled(vty))) {
            int ptr_recv = (rb && strchr(rb->ty, '*') != NULL);
            if (ptr_recv)
                snprintf(dst, cap, "%s_head(%s)", vty, recv);
            else
                snprintf(dst, cap, "%s_head(&%s)", vty, recv);
        } else if (g_shadow_nginst <= 0) {
            return 0;
        } else {
            char gen[384];
            size_t go = 0;
            int i;
            int n;
            go += (size_t)snprintf(gen + go, sizeof(gen) - go, "_Generic((%s)", recv);
            for (i = 0; i < g_shadow_nginst; i++) {
                if (go + 80 >= sizeof(gen)) return 0; /* fail-loud: truncated */
                n = snprintf(gen + go, sizeof(gen) - go, ", %s: %s_head",
                             g_shadow_ginst[i].mangled, g_shadow_ginst[i].mangled);
                if (n < 0 || (size_t)n >= sizeof(gen) - go) return 0;
                go += (size_t)n;
            }
            if (go + strlen(recv) + 8 >= cap) return 0;
            snprintf(dst, cap, "%s)(&%s)", gen, recv);
        }
    } else if (!is_arrow && strcmp(meth_name, "tail") == 0) {
        if (vty[0] && (strncmp(vty, "Pair_", 5) == 0 ||
                       shadow_family_accepts(vty, "tail") ||
                       shadow_ginst_has_mangled(vty))) {
            int ptr_recv = (rb && strchr(rb->ty, '*') != NULL);
            if (ptr_recv)
                snprintf(dst, cap, "%s_tail(%s)", vty, recv);
            else
                snprintf(dst, cap, "%s_tail(&%s)", vty, recv);
        } else if (g_shadow_nginst <= 0) {
            return 0;
        } else {
            char gen[384];
            size_t go = 0;
            int i;
            int n;
            go += (size_t)snprintf(gen + go, sizeof(gen) - go, "_Generic((%s)", recv);
            for (i = 0; i < g_shadow_nginst; i++) {
                if (go + 80 >= sizeof(gen)) return 0;
                n = snprintf(gen + go, sizeof(gen) - go, ", %s: %s_tail",
                             g_shadow_ginst[i].mangled, g_shadow_ginst[i].mangled);
                if (n < 0 || (size_t)n >= sizeof(gen) - go) return 0;
                go += (size_t)n;
            }
            if (go + strlen(recv) + 8 >= cap) return 0;
            snprintf(dst, cap, "%s)(&%s)", gen, recv);
        }
    } else if (!is_arrow && strcmp(meth_name, "span") == 0 &&
               (!vty[0] || strncmp(vty, "Pair_", 5) == 0)) {
        /* Pair beachhead only — typed ArrayMap_/TU extensions fall through. */
        if (vty[0] && strncmp(vty, "Pair_", 5) == 0)
            snprintf(dst, cap, "%s_span(&%s)", vty, recv);
        else
            snprintf(dst, cap, "Pair_int_double_span(&%s)", recv);
    } else if (!is_arrow && strcmp(meth_name, "hdr") == 0) {
        snprintf(dst, cap, "cc_slice_hdr(&%s)", recv);
    } else if (!is_arrow && strcmp(meth_name, "is_err") == 0) {
        snprintf(dst, cap, "cc_is_err(%s)", recv);
    } else if (!is_arrow && strcmp(meth_name, "is_ok") == 0) {
        snprintf(dst, cap, "cc_is_ok(%s)", recv);
    } else if (!is_arrow && strcmp(meth_name, "value") == 0) {
        snprintf(dst, cap, "cc_value(%s)", recv);
    } else if (!is_arrow && strcmp(meth_name, "unwrap_or") == 0) {
        snprintf(dst, cap, "(cc_is_ok(%s) ? cc_value(%s) : (%s))", recv, recv, a);
    } else if (!is_arrow && strcmp(meth_name, "error") == 0) {
        snprintf(dst, cap, "cc_error(%s)", recv);
    } else {
        /* Beachhead fallbacks: typed instance / scalar family / struct tag /
         * bare-name. Prefer bound receiver type when available. */
        char tag[96];
        char mty[96];
        /* Use field-walk PTR_RECV — `c->out.push_slice` is CCString value, not
         * Conn* just because the outer bind is a pointer. */
        int ptr_recv = SHADOW_UFCS_PTR_RECV();
        tag[0] = mty[0] = 0;
        /* Typed family instances: trust composed Name_meth when the method is
         * in the header-derived ##_ set or a TU-declared extension. */
        if (vty[0] && strncmp(vty, "CCSlice_", 8) == 0) {
            char erased[160];
            char pascal[160];
            if (shadow_family_accepts(vty, meth_name))
                return shadow_ufcs_emit_slice_meth(vty, meth_name, recv, a, dst,
                                                  cap);
            /* Inherited via `base @as` → erased CCSlice helpers only when
             * `cc_slice_<meth>` / `CCSlice_<meth>` is real — never invent
             * Type_nosuch / cc_slice_ghost for miss diagnostics. */
            snprintf(erased, sizeof(erased), "cc_slice_%s", meth_name);
            snprintf(pascal, sizeof(pascal), "CCSlice_%s", meth_name);
            if (!shadow_ufn_exists(erased) && !shadow_ufn_exists(pascal) &&
                !shadow_family_accepts("CCSlice", meth_name) &&
                !shadow_cc_slice_erased_has(meth_name))
                return 0;
            if (a[0]) {
                if (ptr_recv)
                    snprintf(dst, cap, "cc_slice_%s(&(%s)->base, %s)",
                             meth_name, recv, a);
                else
                    snprintf(dst, cap, "cc_slice_%s(&(%s).base, %s)", meth_name,
                             recv, a);
            } else if (ptr_recv) {
                snprintf(dst, cap, "cc_slice_%s(&(%s)->base)", meth_name, recv);
            } else {
                snprintf(dst, cap, "cc_slice_%s(&(%s).base)", meth_name, recv);
            }
            return 1;
        }
        if (vty[0] && (strncmp(vty, "CCVec_", 6) == 0 ||
                       strncmp(vty, "Map_", 4) == 0 ||
                       strncmp(vty, "ArrayMap_", 9) == 0 ||
                       strncmp(vty, "Pair_", 5) == 0)) {
            if (!shadow_family_accepts(vty, meth_name)) return 0;
            if (a[0]) {
                if (ptr_recv)
                    snprintf(dst, cap, "%s_%s(%s, %s)", vty, meth_name, recv, a);
                else
                    snprintf(dst, cap, "%s_%s(&%s, %s)", vty, meth_name, recv, a);
            } else if (ptr_recv) {
                snprintf(dst, cap, "%s_%s(%s)", vty, meth_name, recv);
            } else {
                snprintf(dst, cap, "%s_%s(&%s)", vty, meth_name, recv);
            }
            return 1;
        }
        /* CC_GENERIC_FACTORY monomorph: methods are emitted with the instance
         * (after UFCS). Trust Name_meth when this mangled type is scheduled. */
        if (vty[0] && shadow_ginst_has_mangled(vty)) {
            if (a[0]) {
                if (ptr_recv)
                    snprintf(dst, cap, "%s_%s(%s, %s)", vty, meth_name, recv, a);
                else
                    snprintf(dst, cap, "%s_%s(&%s, %s)", vty, meth_name, recv, a);
            } else if (ptr_recv) {
                snprintf(dst, cap, "%s_%s(%s)", vty, meth_name, recv);
            } else {
                snprintf(dst, cap, "%s_%s(&%s)", vty, meth_name, recv);
            }
            return 1;
        }
        /* CCResult_* members: prefer header-derived Name_meth; never invent
         * snake callees like cc_result_…_nosuch. By-value receivers. */
        if (vty[0] && strncmp(vty, "CCResult_", 9) == 0) {
            if (!shadow_family_accepts(vty, meth_name)) return 0;
            if (a[0])
                snprintf(dst, cap, "%s_%s(%s, %s)", vty, meth_name, recv, a);
            else
                snprintf(dst, cap, "%s_%s(%s)", vty, meth_name, recv);
            return 1;
        }
        /* Parenthesized scalar literals: `(2.0).scale(3.0)` / `(21LL).twice()`. */
        if (!vty[0] && recv[0] == '(') {
            const char* lit = recv + 1;
            size_t llen;
            int is_float = 0, is_double = 0, is_ll = 0, is_int = 0;
            while (*lit == ' ' || *lit == '\t' || *lit == '-') lit++;
            if ((*lit >= '0' && *lit <= '9') || *lit == '.') {
                const char* e = lit;
                while ((*e >= '0' && *e <= '9') || *e == '.' || *e == 'e' ||
                       *e == 'E' || *e == '+' || *e == '-')
                    e++;
                llen = (size_t)(e - lit);
                if (llen && (e[0] == 'L' || e[0] == 'l') &&
                    (e[1] == 'L' || e[1] == 'l'))
                    is_ll = 1;
                else if (llen && (e[0] == 'f' || e[0] == 'F'))
                    is_float = 1;
                else if (llen && strchr(lit, '.') != NULL)
                    is_double = 1;
                else if (llen)
                    is_int = 1;
                if (is_ll || is_float || is_double || is_int) {
                    const char* sty =
                        is_ll ? "long_long" : is_float ? "float"
                                         : is_double  ? "double"
                                                      : "int";
                    char composed[160];
                    snprintf(composed, sizeof(composed), "cc_%s_%s", sty,
                             meth_name);
                    if (shadow_ufn_exists(composed)) {
                        if (a[0])
                            snprintf(dst, cap, "%s(%s, %s)", composed, recv, a);
                        else
                            snprintf(dst, cap, "%s(%s)", composed, recv);
                        return 1;
                    }
                    if (shadow_ufcs_emit_bare(recv, meth_name, a, 0, sty, 0, dst,
                                              cap))
                        return 1;
                }
            }
        }
        /* Chain continuation: `cc_size_t_dbl(n).inc()` — recv is a prior
         * scalar-family call; recover the mangled type from the callee. */
        if (!vty[0] && strncmp(recv, "cc_", 3) == 0) {
            const char* s = recv + 3;
            const char* us = NULL;
            const char* p;
            for (p = s; *p && *p != '('; p++) {
                if (*p == '_') us = p;
            }
            if (us && us > s && (size_t)(us - s) < sizeof(mty)) {
                memcpy(mty, s, (size_t)(us - s));
                mty[us - s] = 0;
                if (strcmp(mty, "int") == 0 || strcmp(mty, "double") == 0 ||
                    strcmp(mty, "float") == 0 || strcmp(mty, "size_t") == 0 ||
                    strcmp(mty, "bool") == 0 || strcmp(mty, "char") == 0 ||
                    strcmp(mty, "long") == 0 || strcmp(mty, "long_long") == 0) {
                    char composed[160];
                    snprintf(composed, sizeof(composed), "cc_%s_%s", mty,
                             meth_name);
                    if (shadow_ufn_exists(composed)) {
                        if (a[0])
                            snprintf(dst, cap, "%s(%s, %s)", composed, recv, a);
                        else
                            snprintf(dst, cap, "%s(%s)", composed, recv);
                        return 1;
                    }
                    if (shadow_ufcs_emit_bare(recv, meth_name, a, 0, mty, 0, dst,
                                              cap))
                        return 1;
                }
            }
        }
        if (vty[0] &&
            (strcmp(vty, "int") == 0 || strcmp(vty, "double") == 0 ||
             strcmp(vty, "float") == 0 || strcmp(vty, "size_t") == 0 ||
             strcmp(vty, "bool") == 0 || strcmp(vty, "char") == 0 ||
             strcmp(vty, "long") == 0 || strcmp(vty, "long long") == 0 ||
             strcmp(vty, "long_long") == 0)) {
            size_t i, o = 0;
            char composed[160];
            int ris_const = rb && strncmp(rb->ty, "const ", 6) == 0;
            for (i = 0; vty[i] && o + 1 < sizeof(mty); i++) {
                char c = vty[i];
                if (c == ' ' || c == '\t') c = '_';
                mty[o++] = c;
            }
            mty[o] = 0;
            snprintf(composed, sizeof(composed), "cc_%s_%s", mty, meth_name);
            if (shadow_ufn_exists(composed))
                return shadow_ufcs_fmt_call(dst, cap, composed, "", recv, a);
            if (shadow_ufcs_emit_bare(recv, meth_name, a, ptr_recv, mty,
                                      ris_const, dst, cap))
                return 1;
            return 0;
        }
        if (vty[0] && strncmp(vty, "struct ", 7) == 0) {
            snprintf(tag, sizeof(tag), "%s", vty + 7);
        } else if (vty[0] && vty[0] >= 'A' && vty[0] <= 'Z') {
            /* Typedef / tag / CC* (CCArena.avail → arena_avail via snake). */
            snprintf(tag, sizeof(tag), "%s", vty);
        }
        /* `CCPy*` → tag CCPy + ptr_recv; never snake `py*` → `cc_py*_exec`. */
        {
            size_t tl = strlen(tag);
            while (tl && (tag[tl - 1] == '*' || tag[tl - 1] == ' ')) {
                if (tag[tl - 1] == '*') ptr_recv = 1;
                tag[--tl] = 0;
            }
        }
        if (!ptr_recv && recv && strstr(recv, ".u.value")) ptr_recv = 1;
        if (tag[0]) {
            char composed[160];
            char snake[160];
            char tryc[160];
            int ris_const = rb && strncmp(rb->ty, "const ", 6) == 0;
            size_t si = 0, ti = 0;
            int found = 0;
            if (shadow_type_has_callable_field(tag, meth_name)) {
                int ptr = is_arrow || (rb && strchr(rb->ty, '*') != NULL);
                if (a[0])
                    snprintf(dst, cap, ptr ? "%s->%s(%s)" : "%s.%s(%s)",
                             recv, meth_name, a);
                else
                    snprintf(dst, cap, ptr ? "%s->%s()" : "%s.%s()",
                             recv, meth_name);
                return 1;
            }
            /* CCArena → Arena for snake (drop CC prefix). */
            if (strncmp(tag, "CC", 2) == 0 && tag[2] >= 'A' && tag[2] <= 'Z')
                ti = 2;
            /* DefinedToy → defined_toy (matches cc_type_register ufcs prefixes). */
            for (; tag[ti] && si + 2 < sizeof(snake); ti++) {
                char c = tag[ti];
                if (c >= 'A' && c <= 'Z') {
                    if (si > 0) snake[si++] = '_';
                    snake[si++] = (char)(c - 'A' + 'a');
                } else
                    snake[si++] = c;
            }
            snake[si] = 0;
            composed[0] = 0;
            /* Prefer registered callees visible as static fns in the TU:
             *   Type_meth, snake_type_meth, snake_meth_{int,cstr},
             *   snake_meth, snake_legacy_meth, bare meth (arena_avail). */
            snprintf(tryc, sizeof(tryc), "%s_%s", tag, meth_name);
            if (shadow_ufn_exists(tryc)) {
                snprintf(composed, sizeof(composed), "%s", tryc);
                found = 1;
            }
            if (!found && snake[0]) {
                snprintf(tryc, sizeof(tryc), "%s_type_%s", snake, meth_name);
                if (shadow_ufn_exists(tryc)) {
                    snprintf(composed, sizeof(composed), "%s", tryc);
                    found = 1;
                }
            }
            if (!found && snake[0] && a[0]) {
                const char* ap = a;
                while (*ap == ' ' || *ap == '\t') ap++;
                if (*ap == '"')
                    snprintf(tryc, sizeof(tryc), "%s_%s_cstr", snake,
                             meth_name);
                else if ((*ap >= '0' && *ap <= '9') || *ap == '-' || *ap == '+')
                    snprintf(tryc, sizeof(tryc), "%s_%s_int", snake,
                             meth_name);
                else
                    tryc[0] = 0;
                if (tryc[0] && shadow_ufn_exists(tryc)) {
                    snprintf(composed, sizeof(composed), "%s", tryc);
                    found = 1;
                }
            }
            if (!found && snake[0]) {
                snprintf(tryc, sizeof(tryc), "%s_%s", snake, meth_name);
                if (shadow_ufn_exists(tryc)) {
                    snprintf(composed, sizeof(composed), "%s", tryc);
                    found = 1;
                }
            }
            if (!found && snake[0]) {
                snprintf(tryc, sizeof(tryc), "%s_legacy_%s", snake, meth_name);
                if (shadow_ufn_exists(tryc)) {
                    snprintf(composed, sizeof(composed), "%s", tryc);
                    found = 1;
                }
            }
            /* CCArena.avail → cc_arena_remaining (stdlib / header hook). */
            if (!found && strcmp(meth_name, "avail") == 0 &&
                (strcmp(tag, "CCArena") == 0 || strcmp(snake, "arena") == 0)) {
                snprintf(composed, sizeof(composed), "cc_arena_remaining");
                found = 1;
            }
            /* Stdlib CC* methods in headers: plain CCSlice → cc_slice_meth;
             * Unique/Shared slice markers share cc_slice_*; Tx/Rx → Type_meth.
             * Invent the composed callee for host/stdlib link (Nursery.spawn_async,
             * Arena.slice, …). Typed CCSlice_T / CCVec_T miss paths above do not
             * invent Type_nosuch. */
            if (!found && strncmp(tag, "CC", 2) == 0) {
                int strict_handle =
                    (strstr(tag, "Tx") != NULL || strstr(tag, "Rx") != NULL ||
                     ((strstr(tag, "Unique") || strstr(tag, "Shared")) &&
                      !strstr(tag, "Slice")));
                if ((strstr(tag, "Unique") || strstr(tag, "Shared")) &&
                    strstr(tag, "Slice")) {
                    snprintf(composed, sizeof(composed), "cc_slice_%s",
                             meth_name);
                    found = 1;
                } else if (strstr(tag, "Tx") || strstr(tag, "Rx") ||
                           strstr(tag, "Unique") || strstr(tag, "Shared")) {
                    char try_comp[160];
                    snprintf(try_comp, sizeof(try_comp), "%s_%s", tag,
                             meth_name);
                    /* Channel/handle allowlist — never invent Type_nosuch. */
                    if (shadow_ufn_exists(try_comp) ||
                        shadow_family_accepts(tag, meth_name)) {
                        snprintf(composed, sizeof(composed), "%s", try_comp);
                        found = 1;
                    }
                    (void)strict_handle;
                } else if (snake[0]) {
                    /* Compose cc_py_exec / cc_thing_area. Prefer a bare-name
                     * rival (peek_t) over inventing cc_thing_peek_t — that is
                     * the CC-prefixed ladder. Otherwise trust the composition
                     * for header methods not mirrored in the local ufn table.
                     * Never invent over a registered .ufcs_dynamic sink
                     * (CCPyObj → callm for unknown Python methods). */
                    snprintf(composed, sizeof(composed), "cc_%s_%s", snake,
                             meth_name);
                    if (shadow_ufn_exists(composed) ||
                        shadow_family_accepts(tag, meth_name))
                        found = 1;
                    else if (!shadow_ufn_exists(meth_name) &&
                             !shadow_dyn_sink_for(tag))
                        found = 1;
                    else
                        composed[0] = 0;
                } else {
                    snprintf(composed, sizeof(composed), "%s_%s", tag,
                             meth_name);
                    if (shadow_ufn_exists(composed) ||
                        shadow_family_accepts(tag, meth_name))
                        found = 1;
                    else if (!shadow_ufn_exists(meth_name) &&
                             !shadow_dyn_sink_for(tag))
                        found = 1;
                    else
                        composed[0] = 0;
                }
            }
            /* Dynamic sink outranks bare ambient names (poke etc.). */
            if (!found &&
                shadow_ufcs_try_dyn_sink(recv, meth_name, a, ptr_recv, tag, dst,
                                        cap))
                return 1;
            /* Bare local name: first-param match decides & vs by-value. */
            if (!found && shadow_ufn_exists(meth_name)) {
                if (shadow_ufcs_emit_bare(recv, meth_name, a, ptr_recv, tag,
                                          ris_const, dst, cap))
                    return 1;
            }
            /* Typed CCSlice_T inherits erased helpers via `base @as`. */
            if (!found && strncmp(tag, "CCSlice_", 8) == 0) {
                char pascal[160];
                snprintf(composed, sizeof(composed), "cc_slice_%s", meth_name);
                snprintf(pascal, sizeof(pascal), "CCSlice_%s", meth_name);
                if (!shadow_ufn_exists(composed) && !shadow_ufn_exists(pascal) &&
                    !shadow_family_accepts("CCSlice", meth_name) &&
                    !shadow_cc_slice_erased_has(meth_name))
                    return 0;
                if (a[0]) {
                    if (ptr_recv)
                        snprintf(dst, cap, "%s(&(%s)->base, %s)", composed, recv,
                                 a);
                    else
                        snprintf(dst, cap, "%s(&(%s).base, %s)", composed, recv,
                                 a);
                } else if (ptr_recv) {
                    snprintf(dst, cap, "%s(&(%s)->base)", composed, recv);
                } else {
                    snprintf(dst, cap, "%s(&(%s).base)", composed, recv);
                }
                return 1;
            }
            if (found)
                return shadow_ufcs_fmt_call(dst, cap, composed,
                                           ptr_recv ? "" : "&", recv, a);
            if (shadow_ufcs_try_dyn_sink(recv, meth_name, a, ptr_recv, tag, dst,
                                        cap))
                return 1;
            if (shadow_ufcs_emit_bare(recv, meth_name, a, ptr_recv, tag,
                                      ris_const, dst, cap))
                return 1;
            /* Fall through to typed-miss diagnostic below. */
        } else if (vty[0]) {
            /* Typed receiver with no family/tag spelling — bare-name only. */
            int ris_const = rb && strncmp(rb->ty, "const ", 6) == 0;
            if (shadow_ufcs_try_dyn_sink(recv, meth_name, a, ptr_recv, vty, dst,
                                        cap))
                return 1;
            if (shadow_ufcs_emit_bare(recv, meth_name, a, ptr_recv, vty,
                                      ris_const, dst, cap))
                return 1;
        }
        return 0;
    }
#undef SHADOW_UFCS_PTR_RECV
    return 1;
}

static int shadow_ufcs_is_scalar_ty(const char* vty) {
    return vty &&
           (strcmp(vty, "int") == 0 || strcmp(vty, "double") == 0 ||
            strcmp(vty, "float") == 0 || strcmp(vty, "size_t") == 0 ||
            strcmp(vty, "bool") == 0 || strcmp(vty, "char") == 0 ||
            strcmp(vty, "long") == 0 || strcmp(vty, "long long") == 0 ||
            strcmp(vty, "long_long") == 0);
}

/* Diagnose a typed UFCS miss once; used by structured emit + leftover scan.
 * Optional path/line attribute the site (spliced .cch oracles). */
static void shadow_ufcs_diagnose_miss_at(const char* recv, const char* meth,
                                        const char* path, int line) {
    const ShadowBind* rb;
    char vty[128];
    int is_scalar;
    int is_handle;
    if (!meth || !meth[0] || g_shadow_ufcs_miss) return;
    rb = shadow_bind_for_recv(recv);
    shadow_bind_base_ty(rb, vty, sizeof(vty));
    if (!vty[0])
        (void)shadow_ufcs_recv_slice_ty(recv, vty, sizeof(vty));
    if (!vty[0] && recv && recv[0] >= 'A' && recv[0] <= 'Z') {
        /* CCSlice_double_sub(...) → type prefix before '('. */
        const char* par = strchr(recv, '(');
        size_t n = par ? (size_t)(par - recv) : strlen(recv);
        if (n && n < sizeof(vty)) {
            memcpy(vty, recv, n);
            vty[n] = 0;
        } else
            snprintf(vty, sizeof(vty), "%s", recv);
    }
    if (!vty[0]) return;
    is_scalar = shadow_ufcs_is_scalar_ty(vty);
    is_handle =
        (strncmp(vty, "CCChan", 6) == 0 || strncmp(vty, "CCResult_", 9) == 0);
    /* Prefer `error: no UFCS…` for channel/result/scalars; keep
     * `error: type: no UFCS…` for user/typedef receivers (as_ufcs oracle). */
    if (path && path[0] && line > 0) {
        if (is_handle || is_scalar)
            fprintf(stderr,
                    "%s:%d: error: no UFCS method '%s' for receiver type '%s'\n",
                    path, line, meth, vty);
        else
            fprintf(stderr,
                    "%s:%d: error: type: no UFCS method '%s' for receiver type "
                    "'%s'\n",
                    path, line, meth, vty);
    } else if (is_handle || is_scalar) {
        fprintf(stderr, "error: no UFCS method '%s' for receiver type '%s'\n",
                meth, vty);
    } else {
        fprintf(stderr,
                "error: type: no UFCS method '%s' for receiver type '%s'\n", meth,
                vty);
    }
    if (is_scalar) {
        char cand[160];
        char fam[64];
        size_t i, o = 0;
        for (i = 0; vty[i] && o + 1 < sizeof(fam); i++) {
            char c = vty[i];
            fam[o++] = (c == ' ' || c == '\t') ? '_' : c;
        }
        fam[o] = 0;
        snprintf(cand, sizeof(cand), "cc_%s_%s", fam, meth);
        fprintf(stderr, "candidate %s (scalar family): not declared\n", cand);
        if (shadow_ufn_exists(meth)) {
            const char* pty = shadow_ufn_first_ty(meth);
            fprintf(stderr,
                    "candidate %s (bare): declared, but first parameter '%s' "
                    "does not take '%s'\n",
                    meth, pty && pty[0] ? pty : "?", vty);
        } else {
            fprintf(stderr, "candidate %s (bare): no visible declaration\n",
                    meth);
        }
    } else if (shadow_ufn_exists(meth)) {
        const char* pty = shadow_ufn_first_ty(meth);
        fprintf(stderr,
                "candidate %s (bare): declared, but first parameter '%s' does "
                "not take '%s'\n",
                meth, pty && pty[0] ? pty : "?", vty);
    } else if (strncmp(vty, "CCSlice_", 8) == 0 ||
               strncmp(vty, "CCVec_", 6) == 0 ||
               strncmp(vty, "Map_", 4) == 0 ||
               strncmp(vty, "ArrayMap_", 9) == 0 ||
               strncmp(vty, "Pair_", 5) == 0 ||
               strncmp(vty, "CCResult_", 9) == 0 ||
               strncmp(vty, "CCChan", 6) == 0 ||
               (vty[0] >= 'A' && vty[0] <= 'Z')) {
        char cand[160];
        char snake[160];
        size_t si = 0, ti = 0;
        snprintf(cand, sizeof(cand), "%s_%s", vty, meth);
        fprintf(stderr, "candidate %s (family spelling): not declared\n", cand);
        if (strncmp(vty, "CC", 2) == 0 && vty[2] >= 'A' && vty[2] <= 'Z')
            ti = 2;
        for (; vty[ti] && si + 2 < sizeof(snake); ti++) {
            char c = vty[ti];
            if (c >= 'A' && c <= 'Z') {
                if (si > 0) snake[si++] = '_';
                snake[si++] = (char)(c - 'A' + 'a');
            } else if (c == '_')
                snake[si++] = c;
            else
                snake[si++] = c;
        }
        snake[si] = 0;
        if (snake[0]) {
            snprintf(cand, sizeof(cand), "%s_%s", snake, meth);
            if (strcmp(cand, meth) != 0)
                fprintf(stderr,
                        "candidate %s (family spelling): not declared\n", cand);
        }
        if (!shadow_ufn_exists(meth))
            fprintf(stderr, "candidate %s (bare): no visible declaration\n",
                    meth);
    }
    {
        const char* mem = shadow_family_members_csv(vty);
        if (mem && mem[0])
            fprintf(stderr, "installed methods of %s: %s\n", vty, mem);
        else
            fprintf(stderr, "installed methods of %s:\n", vty);
    }
    g_shadow_ufcs_miss = 1;
}

static void shadow_ufcs_diagnose_miss(const char* recv, const char* meth) {
    shadow_ufcs_diagnose_miss_at(recv, meth, NULL, 0);
}

/* One left-to-right UFCS step → lower_parts + splice.
 * Leftmost so chains like d.halve().twice() peel correctly. Gated leftover
 * text (slots/chains) uses this; structured AST_UFCS_* use lower_parts only. */
static int shadow_ufcs_peel_left(char* dst, size_t cap, const char* src,
                                const char* elem_ty) {
    const char* op = NULL;
    int is_arrow = 0;
    char targs[128];
    targs[0] = 0;
    {
        const char* p = src;
        int in_dq = 0, in_sq = 0, in_bt = 0;
        while (*p && !op) {
            /* Do not peel UFCS inside "…" / '…' / `…` (@string Python bodies). */
            if (in_bt) {
                if (*p == '`') in_bt = 0;
                p++;
                continue;
            }
            if (in_dq) {
                if (*p == '\\' && p[1]) {
                    p += 2;
                    continue;
                }
                if (*p == '"') in_dq = 0;
                p++;
                continue;
            }
            if (in_sq) {
                if (*p == '\\' && p[1]) {
                    p += 2;
                    continue;
                }
                if (*p == '\'') in_sq = 0;
                p++;
                continue;
            }
            if (*p == '`') {
                in_bt = 1;
                p++;
                continue;
            }
            if (*p == '"') {
                in_dq = 1;
                p++;
                continue;
            }
            if (*p == '\'') {
                in_sq = 1;
                p++;
                continue;
            }
            if (p[0] == '-' && p[1] == '>' &&
                ((p[2] >= 'A' && p[2] <= 'Z') || (p[2] >= 'a' && p[2] <= 'z') ||
                 p[2] == '_')) {
                const char* m = p + 2;
                char ta[128];
                const char* par;
                while (*m == ' ' || *m == '\t') m++;
                const char* q = m;
                while ((*q >= 'A' && *q <= 'Z') || (*q >= 'a' && *q <= 'z') ||
                       (*q >= '0' && *q <= '9') || *q == '_')
                    q++;
                par = shadow_ufcs_text_paren_after_meth(q, ta, sizeof(ta));
                if (par) {
                    op = p;
                    is_arrow = 1;
                    snprintf(targs, sizeof(targs), "%s", ta);
                    break;
                }
                p += 2;
                continue;
            }
            if (p[0] == '.' &&
                ((p[1] >= 'A' && p[1] <= 'Z') || (p[1] >= 'a' && p[1] <= 'z') ||
                 p[1] == '_')) {
                const char* m = p + 1;
                char ta[128];
                const char* par;
                while (*m == ' ' || *m == '\t') m++;
                const char* q = m;
                while ((*q >= 'A' && *q <= 'Z') || (*q >= 'a' && *q <= 'z') ||
                       (*q >= '0' && *q <= '9') || *q == '_')
                    q++;
                par = shadow_ufcs_text_paren_after_meth(q, ta, sizeof(ta));
                if (par) {
                    op = p;
                    is_arrow = 0;
                    snprintf(targs, sizeof(targs), "%s", ta);
                    break;
                }
            }
            p++;
        }
    }
    if (!op) return 0;
    char recv[192];
    char meth_name[64];
    char args[4096];
    char repl[8192];
    const char* after = NULL;
    const char* meth;
    const char* par;
    const char* formal = targs[0] ? targs : elem_ty;
    size_t mi = 0;
    if (!shadow_ufcs_recv(src, op, recv, sizeof(recv))) return 0;
    meth = is_arrow ? op + 2 : op + 1;
    while (*meth == ' ' || *meth == '\t') meth++;
    while (meth[mi] && meth[mi] != '(' && meth[mi] != ':' &&
           mi + 1 < sizeof(meth_name)) {
        meth_name[mi] = meth[mi];
        mi++;
    }
    meth_name[mi] = 0;
    par = shadow_ufcs_text_paren_after_meth(meth + mi, targs, sizeof(targs));
    if (!par) return 0;
    if (targs[0]) formal = targs;
    if (!shadow_ufcs_args(par, args, sizeof(args), &after)) return 0;
    if (targs[0] && !shadow_meth_has_type_formal(meth_name)) {
        char fam[160];
        const ShadowBind* rb0 = shadow_bind_for_recv(recv);
        char vty0[128];
        shadow_bind_base_ty(rb0, vty0, sizeof(vty0));
        if (!shadow_ufcs_factory_member_cand(recv, vty0, meth_name, fam,
                                            sizeof(fam)))
            return 0;
    }
    {
        const char* rstart = op;
        size_t rn, pre;
        const char* cast_lp = NULL;
        char cast_ty[96];
        int saved_dest = g_shadow_sink_dest[0];
        cast_ty[0] = 0;
        while (rstart > src && (rstart[-1] == ' ' || rstart[-1] == '\t' ||
                                rstart[-1] == '\n'))
            rstart--;
        rn = strlen(recv);
        if ((size_t)(rstart - src) < rn) return 0;
        rstart -= rn;
        if (memcmp(rstart, recv, rn) != 0) return 0;
        /* `(T)recv.meth` — cast is a spelled sink destination (absorbed). */
        {
            const char* q = rstart;
            while (q > src && (q[-1] == ' ' || q[-1] == '\t' || q[-1] == '\n'))
                q--;
            if (q > src && q[-1] == ')') {
                const char* rp = q - 1;
                const char* lp = rp;
                int found = 0;
                while (lp > src) {
                    char c = lp[-1];
                    if (c == '(') {
                        lp--; /* points at '(' */
                        found = 1;
                        break;
                    }
                    if (c == ')' || c == ';' || c == '{' || c == '}') break;
                    lp--;
                }
                if (found) {
                    size_t tn = (size_t)(rp - (lp + 1));
                    const char* ty = lp + 1;
                    while (tn && (*ty == ' ' || *ty == '\t' || *ty == '\n')) {
                        ty++;
                        tn--;
                    }
                    while (tn && (ty[tn - 1] == ' ' || ty[tn - 1] == '\t' ||
                                  ty[tn - 1] == '\n'))
                        tn--;
                    if (tn && tn + 1 < sizeof(cast_ty) &&
                        (((ty[0] >= 'A' && ty[0] <= 'Z') ||
                          (ty[0] >= 'a' && ty[0] <= 'z') || ty[0] == '_') ||
                         strncmp(ty, "long", 4) == 0 ||
                         strncmp(ty, "const", 5) == 0)) {
                        memcpy(cast_ty, ty, tn);
                        cast_ty[tn] = 0;
                        cast_lp = lp;
                        snprintf(g_shadow_sink_dest, sizeof(g_shadow_sink_dest),
                                 "%s", cast_ty);
                    }
                }
            }
        }
        if (!shadow_ufcs_lower_parts(recv, meth_name, args, is_arrow, formal,
                                     repl, sizeof(repl))) {
            if (!saved_dest) g_shadow_sink_dest[0] = 0;
            return 0;
        }
        if (cast_lp && !saved_dest) g_shadow_sink_dest[0] = 0;
        /* Replace cast+call; keep any outer parens (`((T)call)` → `(lowered)`). */
        pre = (size_t)((cast_lp ? cast_lp : rstart) - src);
        {
            /* Opaque blobs sometimes start the match after eating the space
             * before `recv` (`return sh->m` → `returnShard_m`).  Re-insert. */
            int need_sp = 0;
            char prev;
            if (pre > 0 && repl[0] && repl[0] != ' ' && repl[0] != '\t' &&
                repl[0] != '(') {
                prev = src[pre - 1];
                if ((prev >= 'a' && prev <= 'z') ||
                    (prev >= 'A' && prev <= 'Z') ||
                    (prev >= '0' && prev <= '9') || prev == '_')
                    need_sp = 1;
            }
            if (pre + (size_t)need_sp + strlen(repl) +
                    strlen(after ? after : "") + 1 >=
                cap)
                return 0;
            if (need_sp)
                snprintf(dst, cap, "%.*s %s%s", (int)pre, src, repl,
                         after ? after : "");
            else
                snprintf(dst, cap, "%.*s%s%s", (int)pre, src, repl,
                         after ? after : "");
        }
    }
    return 1;
}

/* cc_channel_pair(&tx, &rx) → cc_channel_pair_create_named(...).
 * Looks up capacity/elem/topo/ordered from registered AST_CHAN_VAR decls. */
static int shadow_format_channel_pair(char* dst, size_t cap, const char* src) {
    const char* call = src ? strstr(src, "cc_channel_pair(") : NULL;
    const char* p;
    char tx[64], rx[64];
    char args[128];
    char repl[512];
    char outb[768];
    char elem_sz_buf[96];
    const ShadowChanDecl* dtx;
    const ShadowChanDecl* drx;
    const char* cap_expr = "1";
    const char* elem_ty = NULL;
    const char* topo = NULL;
    const char* topo_enum = "CC_CHAN_TOPO_DEFAULT";
    const char* bp_enum = "CC_CHAN_MODE_BLOCK";
    const char* elem_sz = "sizeof(int)";
    int ordered = 0;
    int allow_take = 0;
    int is_sync = 0;
    int bp_mode = 0;
    size_t pre;
    const char* after;
    if (!call) return 0;
    p = call + strlen("cc_channel_pair");
    if (!shadow_ufcs_args(p, args, sizeof(args), &after)) return 0;
    /* Expect &tx, &rx (optional whitespace). */
    {
        const char* a = args;
        while (*a == ' ' || *a == '\t') a++;
        if (*a != '&') return 0;
        a++;
        size_t i = 0;
        while (*a && *a != ',' && *a != ' ' && *a != '\t' && i + 1 < sizeof(tx))
            tx[i++] = *a++;
        tx[i] = 0;
        while (*a == ' ' || *a == '\t') a++;
        if (*a != ',') return 0;
        a++;
        while (*a == ' ' || *a == '\t') a++;
        if (*a != '&') return 0;
        a++;
        i = 0;
        while (*a && *a != ' ' && *a != '\t' && i + 1 < sizeof(rx))
            rx[i++] = *a++;
        rx[i] = 0;
        while (*a == ' ' || *a == '\t') a++;
        if (*a) return 0;
        if (!tx[0] || !rx[0]) return 0;
    }
    dtx = shadow_chan_find(tx);
    drx = shadow_chan_find(rx);
    if (dtx && dtx->cap[0]) cap_expr = dtx->cap;
    else if (drx && drx->cap[0]) cap_expr = drx->cap;
    if (dtx && dtx->elem[0]) elem_ty = dtx->elem;
    else if (drx && drx->elem[0]) elem_ty = drx->elem;
    if (dtx && dtx->topo[0]) topo = dtx->topo;
    else if (drx && drx->topo[0]) topo = drx->topo;
    if (dtx && dtx->is_sync) is_sync = 1;
    else if (drx && drx->is_sync) is_sync = 1;
    if (dtx && dtx->bp_mode) bp_mode = dtx->bp_mode;
    else if (drx && drx->bp_mode) bp_mode = drx->bp_mode;
    /* Decl `ordered` vs create_named ordered-TASK bit (production):
     * CCTask elem or a tx this TU feeds via send_task → task machinery;
     * ordered data channels keep their element size (chan_ordered_data_smoke). */
    {
        int decl_ordered =
            (dtx && dtx->ordered) || (drx && drx->ordered);
        int task_elem = 0;
        if (elem_ty && elem_ty[0]) {
            if (strcmp(elem_ty, "CCTask") == 0 ||
                strcmp(elem_ty, "CCTaskIntptr") == 0 ||
                strcmp(elem_ty, "Task") == 0)
                task_elem = 1;
        }
        ordered = (decl_ordered &&
                   (task_elem || shadow_chan_has_task_send(tx)))
                      ? 1
                      : 0;
    }
    if (elem_ty && elem_ty[0]) {
        if (strstr(elem_ty, "[:") || strstr(elem_ty, "CCSlice")) {
            elem_sz = "sizeof(CCSlice)";
            allow_take = 1;
        } else if (strchr(elem_ty, '*')) {
            elem_sz = "sizeof(void*)";
            allow_take = 1;
        } else {
            snprintf(elem_sz_buf, sizeof(elem_sz_buf), "sizeof(%s)", elem_ty);
            elem_sz = elem_sz_buf;
        }
    }
    if (ordered) {
        elem_sz = "sizeof(CCTask)";
        allow_take = 0;
    }
    if (topo) {
        if (strcmp(topo, "1:1") == 0) topo_enum = "CC_CHAN_TOPO_1_1";
        else if (strcmp(topo, "1:N") == 0) topo_enum = "CC_CHAN_TOPO_1_N";
        else if (strcmp(topo, "N:1") == 0) topo_enum = "CC_CHAN_TOPO_N_1";
        else if (strcmp(topo, "N:N") == 0) topo_enum = "CC_CHAN_TOPO_N_N";
    }
    if (bp_mode == 1) bp_enum = "CC_CHAN_MODE_DROP_NEW";
    else if (bp_mode == 2) bp_enum = "CC_CHAN_MODE_DROP_OLD";
    snprintf(repl, sizeof(repl),
             "cc_channel_pair_create_named(%s, %s, %d, %s, %d, "
             "%s, %d, &%s, &%s, \"%s,%s\", __FILE__, __LINE__)",
             cap_expr, bp_enum, allow_take, elem_sz, is_sync, topo_enum,
             ordered, tx, rx, tx, rx);
    pre = (size_t)(call - src);
    snprintf(outb, sizeof(outb), "%.*s%s%s", (int)pre, src, repl, after);
    snprintf(dst, cap, "%s", outb);
    return 1;
}

/* Typed 2-arg cc_channel_send/recv(...) != 0 → !(__cc_chkr).ok (Result API).
 * 3-arg raw forms return int errno — leave `== 0` / `!= 0` alone (pigz pool). */
static int shadow_chan_call_nargs(const char* args) {
    int depth = 0, n = 0, any = 0;
    const char* p = args ? args : "";
    while (*p == ' ' || *p == '\t') p++;
    if (!*p) return 0;
    for (; *p; p++) {
        if (*p == '(' || *p == '[' || *p == '{') depth++;
        else if (*p == ')' || *p == ']' || *p == '}') {
            if (depth > 0) depth--;
        } else if (*p == ',' && depth == 0) {
            n++;
            any = 1;
        } else if (*p != ' ' && *p != '\t' && *p != '\n')
            any = 1;
    }
    return any ? n + 1 : 0;
}

static int shadow_lower_chan_result_cmp(char* dst, size_t cap,
                                              const char* src) {
    const char* ops[] = { "cc_channel_send(", "cc_channel_recv(", NULL };
    int oi;
    for (oi = 0; ops[oi]; oi++) {
        const char* call = strstr(src, ops[oi]);
        const char* after;
        char args[256];
        char outb[768];
        size_t pre;
        const char* p;
        if (!call) continue;
        p = call + strlen(ops[oi]) - 1; /* at '(' */
        if (!shadow_ufcs_args(p, args, sizeof(args), &after)) continue;
        /* Raw `send/recv(ch, ptr, size)` returns int, not CCResult. */
        if (shadow_chan_call_nargs(args) >= 3) continue;
        while (*after == ' ' || *after == '\t') after++;
        if (strncmp(after, "!= 0", 4) == 0 || strncmp(after, "!=0", 3) == 0) {
            const char* rest = after + (after[2] == ' ' ? 4 : 3);
            pre = (size_t)(call - src);
            /* Bind call to a local so Result field access is on an lvalue. */
            snprintf(outb, sizeof(outb),
                     "%.*s({ __typeof__(%.*s) __cc_chkr = %.*s; "
                     "!(__cc_chkr).ok; })%s",
                     (int)pre, src, (int)(after - call), call,
                     (int)(after - call), call, rest);
            snprintf(dst, cap, "%s", outb);
            return 1;
        }
        if (strncmp(after, "== 0", 4) == 0 || strncmp(after, "==0", 3) == 0) {
            const char* rest = after + (after[2] == ' ' ? 4 : 3);
            pre = (size_t)(call - src);
            snprintf(outb, sizeof(outb),
                     "%.*s({ __typeof__(%.*s) __cc_chkr = %.*s; "
                     "(__cc_chkr).ok; })%s",
                     (int)pre, src, (int)(after - call), call,
                     (int)(after - call), call, rest);
            snprintf(dst, cap, "%s", outb);
            return 1;
        }
    }
    return 0;
}

static void shadow_rewrite_ufcs(char* dst, size_t cap, const char* src,
                                const char* elem_ty);
static void shadow_rewrite_slice_lit_call_args(char* expr, size_t cap);
static void shadow_rewrite_as_call_args(char* expr, size_t cap);
static void shadow_rewrite_slice_as_call_args(char* expr, size_t cap);

/* Structured UFCS → C via lower_parts (no text reconstruct on the call). */
static int shadow_emit_ufcs_to_buf(AstNode* st, char* dst, size_t cap,
                                  const char* elem_ty) {
    /* Large enough for expanded `@string(...).println()` stmt-exprs. */
    char recv[4096];
    char args[4096];
    const char* targs;
    const char* formal;
    int is_arrow;
    int i;
    if (!st || !dst || !cap) return 0;
    if (st->kind != AST_UFCS_STMT && st->kind != AST_UFCS_EXPR) return 0;
    is_arrow = shadow_ufcs_e_arrow(st->e);
    targs = shadow_ufcs_e_targs(st->e);
    /* Explicit ::[T] wins over typed-destination inference. */
    formal = (targs && targs[0]) ? targs : elem_ty;
    if (targs && targs[0] && !shadow_meth_has_type_formal(st->b)) {
        char fam[160];
        char fams[512];
        const ShadowBind* rb0 = shadow_bind_for_recv(st->a);
        char vty0[128];
        shadow_bind_base_ty(rb0, vty0, sizeof(vty0));
        if (!shadow_ufcs_factory_member_cand(st->a, vty0, st->b, fam,
                                            sizeof(fam))) {
            /* Match production wording so typo oracles (py_expse) can bind. */
            snprintf(fam, sizeof(fam), "%s_%s", st->a, st->b);
            fprintf(stderr,
                    "error: type: no type-formal member '%s' for receiver type "
                    "'%s' — `::[...]` binds a member that declares a type formal "
                    "(allocT, block_on, as_list, as_map, map) or a registered "
                    "generic factory named '%s'\n",
                    st->b, st->a[0] ? st->a : "?", fam);
            if (cc_emit_plan_generic_factory_names_csv(fams, sizeof(fams)) > 0)
                fprintf(stderr, "  note: registered generic factories: %s\n",
                        fams);
            g_shadow_ufcs_miss = 1;
            return 0;
        }
    }
    snprintf(recv, sizeof(recv), "%s", st->a);
    args[0] = 0;
    if (st->c[0]) snprintf(args, sizeof(args), "%s", st->c);
    /* `@string(...).println()` — lower the template recv before method emit. */
    if (strncmp(recv, "@string(", 8) == 0)
        shadow_rewrite_print_and_string(recv, sizeof(recv));
    /* Nested UFCS kids (recv/args) — structural first. */
    for (i = 0; i < st->ndbody; i++) {
        AstNode* u = st->dbody[i];
        char nested[320];
        char surface[288];
        const char* uop;
        const char* uta;
        if (!u || (u->kind != AST_UFCS_EXPR && u->kind != AST_UFCS_STMT))
            continue;
        if (!shadow_emit_ufcs_to_buf(u, nested, sizeof(nested), elem_ty))
            continue;
        uop = shadow_ufcs_e_arrow(u->e) ? "->" : ".";
        uta = shadow_ufcs_e_targs(u->e);
        if (uta && uta[0] && u->c[0])
            snprintf(surface, sizeof(surface), "%s%s%s::[%s](%s)", u->a, uop,
                     u->b, uta, u->c);
        else if (uta && uta[0])
            snprintf(surface, sizeof(surface), "%s%s%s::[%s]()", u->a, uop, u->b,
                     uta);
        else if (u->c[0])
            snprintf(surface, sizeof(surface), "%s%s%s(%s)", u->a, uop, u->b,
                     u->c);
        else
            snprintf(surface, sizeof(surface), "%s%s%s()", u->a, uop, u->b);
        if (strcmp(recv, surface) == 0)
            snprintf(recv, sizeof(recv), "%s", nested);
        if (args[0] && strcmp(args, surface) == 0)
            snprintf(args, sizeof(args), "%s", nested);
        else if (args[0] && surface[0]) {
            /* Piece-level nested UFCS inside a larger arg list. */
            const char* hit = strstr(args, surface);
            if (hit) {
                char rebuilt[288];
                size_t pre = (size_t)(hit - args);
                snprintf(rebuilt, sizeof(rebuilt), "%.*s%s%s", (int)pre, args,
                         nested, hit + strlen(surface));
                snprintf(args, sizeof(args), "%s", rebuilt);
            }
        }
    }
    if (!shadow_ufcs_lower_parts(recv, st->b, args, is_arrow, formal, dst, cap)) {
        /* Typed miss: return 0 so callers diagnose with site/@as (stmt path)
         * or leftover peel can keep fnptr field calls as surface C.
         * Do not diagnose here — that skipped `@as retry` lines and
         * `file:line:` oracles, and blocked FpT.cb() as a false miss. */
        return 0;
    }
    /* Args may still carry @string / @slice — lower before host C. */
    shadow_rewrite_print_and_string(dst, cap);
    {
        const char* pat = "__cc_tpl; }).as_slice()";
        const char* repl = "cc_string_as_slice(&__cc_tpl); })";
        char* hit;
        while ((hit = strstr(dst, pat)) != NULL) {
            char tmp[8192];
            size_t pre = (size_t)(hit - dst);
            if (pre + strlen(repl) + strlen(hit + strlen(pat)) + 1 >= sizeof(tmp) ||
                pre + strlen(repl) + strlen(hit + strlen(pat)) + 1 >= cap)
                break;
            snprintf(tmp, sizeof(tmp), "%.*s%s%s", (int)pre, dst, repl,
                     hit + strlen(pat));
            snprintf(dst, cap, "%s", tmp);
        }
    }
    shadow_rewrite_at_slice(dst, cap);
    shadow_rewrite_generic_types_text(dst, cap);
    /* Call-arg `=>` lifted onto dbody — splice make() into the lowered call. */
    {
        AstNode* cl = shadow_expr_closure_kid(st);
        if (cl) shadow_splice_closure_arg(dst, cap, cl);
    }
    /* Structured UFCS used to skip lit coerce (only the text-peel path ran
     * it) — `return c->err("…")` stayed a bare C string. */
    shadow_rewrite_slice_as_call_args(dst, cap);
    shadow_rewrite_slice_lit_call_args(dst, cap);
    shadow_rewrite_as_call_args(dst, cap);
    return 1;
}

/* If src is exactly one UFCS call (no prefix/suffix), lower via parts. */
static int shadow_emit_exact_ufcs_text(char* dst, size_t cap, const char* src,
                                      const char* elem_ty) {
    const char* op = NULL;
    int is_arrow = 0;
    const char* p;
    char recv[192];
    char meth_name[64];
    char args[4096];
    char targs[128];
    const char* after = NULL;
    const char* meth;
    const char* par;
    const char* formal;
    size_t mi = 0;
    targs[0] = 0;
    if (!src || !src[0]) return 0;
    {
        int in_dq = 0, in_sq = 0;
        for (p = src; *p && !op; p++) {
            if (in_dq) {
                if (*p == '\\' && p[1]) {
                    p++;
                    continue;
                }
                if (*p == '"') in_dq = 0;
                continue;
            }
            if (in_sq) {
                if (*p == '\\' && p[1]) {
                    p++;
                    continue;
                }
                if (*p == '\'') in_sq = 0;
                continue;
            }
            if (*p == '"') {
                in_dq = 1;
                continue;
            }
            if (*p == '\'') {
                in_sq = 1;
                continue;
            }
            if (p[0] == '-' && p[1] == '>' &&
                ((p[2] >= 'A' && p[2] <= 'Z') || (p[2] >= 'a' && p[2] <= 'z') ||
                 p[2] == '_')) {
                const char* m = p + 2;
                const char* q;
                char ta[128];
                while (*m == ' ' || *m == '\t') m++;
                q = m;
                while ((*q >= 'A' && *q <= 'Z') || (*q >= 'a' && *q <= 'z') ||
                       (*q >= '0' && *q <= '9') || *q == '_')
                    q++;
                if (shadow_ufcs_text_paren_after_meth(q, ta, sizeof(ta))) {
                    op = p;
                    is_arrow = 1;
                    snprintf(targs, sizeof(targs), "%s", ta);
                    break;
                }
                p += 2;
                continue;
            }
            if (p[0] == '.' &&
                ((p[1] >= 'A' && p[1] <= 'Z') || (p[1] >= 'a' && p[1] <= 'z') ||
                 p[1] == '_')) {
                const char* m = p + 1;
                const char* q;
                char ta[128];
                while (*m == ' ' || *m == '\t') m++;
                q = m;
                while ((*q >= 'A' && *q <= 'Z') || (*q >= 'a' && *q <= 'z') ||
                       (*q >= '0' && *q <= '9') || *q == '_')
                    q++;
                if (shadow_ufcs_text_paren_after_meth(q, ta, sizeof(ta))) {
                    op = p;
                    is_arrow = 0;
                    snprintf(targs, sizeof(targs), "%s", ta);
                    break;
                }
            }
        }
    }
    if (!op) return 0;
    if (!shadow_ufcs_recv(src, op, recv, sizeof(recv))) return 0;
    /* Recv must be the start of src (exact call, no leading junk). */
    {
        const char* rstart = op;
        size_t rn;
        while (rstart > src && (rstart[-1] == ' ' || rstart[-1] == '\t'))
            rstart--;
        rn = strlen(recv);
        if ((size_t)(rstart - src) < rn) return 0;
        rstart -= rn;
        if (rstart != src || memcmp(rstart, recv, rn) != 0) return 0;
    }
    meth = is_arrow ? op + 2 : op + 1;
    while (*meth == ' ' || *meth == '\t') meth++;
    while (meth[mi] && meth[mi] != '(' && meth[mi] != ':' &&
           mi + 1 < sizeof(meth_name)) {
        meth_name[mi] = meth[mi];
        mi++;
    }
    meth_name[mi] = 0;
    par = shadow_ufcs_text_paren_after_meth(meth + mi, targs, sizeof(targs));
    if (!par) return 0;
    if (!shadow_ufcs_args(par, args, sizeof(args), &after)) return 0;
    while (after && (*after == ' ' || *after == '\t')) after++;
    if (after && *after) return 0; /* trailing junk → not exact */
    formal = targs[0] ? targs : elem_ty;
    if (targs[0] && !shadow_meth_has_type_formal(meth_name)) {
        char fam[160];
        const ShadowBind* rb0 = shadow_bind_for_recv(recv);
        char vty0[128];
        shadow_bind_base_ty(rb0, vty0, sizeof(vty0));
        if (!shadow_ufcs_factory_member_cand(recv, vty0, meth_name, fam,
                                            sizeof(fam)))
            return 0;
    }
    /* Reject chains: recv/args must not still contain UFCS calls. */
    {
        const char* q;
        for (q = recv; *q; q++) {
            if (q[0] == '.' && ((q[1] >= 'A' && q[1] <= 'Z') ||
                                (q[1] >= 'a' && q[1] <= 'z') || q[1] == '_')) {
                const char* r = q + 1;
                char ta[8];
                while ((*r >= 'A' && *r <= 'Z') || (*r >= 'a' && *r <= 'z') ||
                       (*r >= '0' && *r <= '9') || *r == '_')
                    r++;
                if (shadow_ufcs_text_paren_after_meth(r, ta, sizeof(ta)))
                    return 0;
            }
            if (q[0] == '-' && q[1] == '>') return 0;
        }
        for (q = args; *q; q++) {
            if (q[0] == '.' && ((q[1] >= 'A' && q[1] <= 'Z') ||
                                (q[1] >= 'a' && q[1] <= 'z') || q[1] == '_')) {
                const char* r = q + 1;
                char ta[8];
                while ((*r >= 'A' && *r <= 'Z') || (*r >= 'a' && *r <= 'z') ||
                       (*r >= '0' && *r <= '9') || *r == '_')
                    r++;
                if (shadow_ufcs_text_paren_after_meth(r, ta, sizeof(ta)))
                    return 0;
            }
        }
    }
    return shadow_ufcs_lower_parts(recv, meth_name, args, is_arrow, formal, dst,
                                  cap);
}

/* Known callees: wrap string-literal args that expect CCSlice by value. */
static int shadow_slc_known_arg(const char* callee, int arg_index) {
    if (!callee || arg_index < 0) return 0;
    if (strcmp(callee, "cc_path_exists") == 0) return arg_index == 0;
    if (strcmp(callee, "cc_path_is_dir") == 0) return arg_index == 0;
    if (strcmp(callee, "cc_path_is_file") == 0) return arg_index == 0;
    if (strcmp(callee, "cc_file_open") == 0) return arg_index == 1;
    if (strcmp(callee, "cc_file_open_async") == 0) return arg_index == 2;
    if (strcmp(callee, "cc_dir_open") == 0) return arg_index == 1;
    if (strcmp(callee, "cc_glob") == 0) return arg_index == 0;
    if (strcmp(callee, "cc_command") == 0) return arg_index == 1;
    if (strcmp(callee, "cc_command_new") == 0) return arg_index == 1;
    if (strcmp(callee, "cc_path_join") == 0)
        return arg_index == 1 || arg_index == 2;
    if (strcmp(callee, "cc_file_read_path") == 0) return arg_index == 0;
    if (strcmp(callee, "cc_file_write_path") == 0) return arg_index == 0;
    if (strcmp(callee, "cc_file_write") == 0) return arg_index == 1;
    if (strcmp(callee, "cc_sh_run") == 0) return arg_index == 0;
    if (strcmp(callee, "cc_tcp_listen") == 0) return arg_index == 0;
    return 0;
}

static int shadow_slc_is_string_lit(const char* s, size_t n) {
    size_t i = 0;
    if (!s || !n) return 0;
    while (i < n && (s[i] == ' ' || s[i] == '\t')) i++;
    if (i < n && (s[i] == 'L' || s[i] == 'u' || s[i] == 'U')) i++;
    if (i >= n || s[i] != '"') return 0;
    i++;
    while (i < n) {
        if (s[i] == '\\' && i + 1 < n) {
            i += 2;
            continue;
        }
        if (s[i] == '"') {
            i++;
            while (i < n && (s[i] == ' ' || s[i] == '\t')) i++;
            return i == n;
        }
        i++;
    }
    return 0;
}

static int shadow_slc_already_wrapped(const char* s, size_t n) {
    static const char* wraps[] = {
        "cc_slice_from_static", "CC_SLICE_LIT", "cc_slice_cstr",
        "char_to_slice_n", "const_char_to_slice_n",
    };
    size_t i = 0, k;
    while (i < n && (s[i] == ' ' || s[i] == '\t')) i++;
    for (k = 0; k < sizeof(wraps) / sizeof(wraps[0]); k++) {
        size_t wn = strlen(wraps[k]);
        if (i + wn <= n && memcmp(s + i, wraps[k], wn) == 0) {
            size_t j = i + wn;
            while (j < n && (s[j] == ' ' || s[j] == '\t')) j++;
            if (j < n && s[j] == '(') return 1;
        }
    }
    return 0;
}

/* Copy one string/char literal from *pp into out; leave *pp after the closer.
 * Prevents call-arg rewriters from seeing `int (` inside `"int (*)"`. */
static int shadow_copy_quoted(const char** pp, char* out, size_t* o, size_t ocap) {
    const char* p;
    char q;
    if (!pp || !*pp || !out || !o) return 0;
    p = *pp;
    q = *p;
    if (q != '"' && q != '\'') return 0;
    if (*o + 1 >= ocap) return 0;
    out[(*o)++] = *p++;
    while (*p && *o + 1 < ocap) {
        out[(*o)++] = *p;
        if (*p == '\\' && p[1] && *o + 1 < ocap) {
            p++;
            out[(*o)++] = *p++;
            continue;
        }
        if (*p == q) {
            p++;
            *pp = p;
            return 1;
        }
        p++;
    }
    *pp = p;
    return 1;
}

/* Wrap string lits at known CCSlice params: cc_path_exists(".") etc. */
static void shadow_rewrite_slice_lit_call_args(char* expr, size_t cap) {
    char out[8192];
    size_t o = 0;
    const char* p;
    if (!expr || !cap) return;
    p = expr;
    while (*p && o + 1 < sizeof(out)) {
        if (*p == '"' || *p == '\'') {
            if (!shadow_copy_quoted(&p, out, &o, sizeof(out))) break;
            continue;
        }
        if (((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') ||
             *p == '_') &&
            (p == expr ||
             !((p[-1] >= 'A' && p[-1] <= 'Z') || (p[-1] >= 'a' && p[-1] <= 'z') ||
               (p[-1] >= '0' && p[-1] <= '9') || p[-1] == '_'))) {
            const char* name = p;
            size_t nl = 0;
            const char* q;
            char callee[64];
            while ((p[nl] >= 'A' && p[nl] <= 'Z') ||
                   (p[nl] >= 'a' && p[nl] <= 'z') ||
                   (p[nl] >= '0' && p[nl] <= '9') || p[nl] == '_')
                nl++;
            q = name + nl;
            while (*q == ' ' || *q == '\t') q++;
            if (*q == '(' && nl < sizeof(callee)) {
                const char* args = q + 1;
                const char* rp = NULL;
                int depth = 1;
                const char* r;
                for (r = args; *r; r++) {
                    if (*r == '(') depth++;
                    else if (*r == ')') {
                        depth--;
                        if (depth == 0) {
                            rp = r;
                            break;
                        }
                    }
                }
                if (rp) {
                    int argi = 0;
                    const char* cur = args;
                    const char* c;
                    int d = 0;
                    memcpy(callee, name, nl);
                    callee[nl] = 0;
                    o += (size_t)snprintf(out + o, sizeof(out) - o, "%.*s(",
                                          (int)nl, name);
                    for (c = args; c <= rp && o + 40 < sizeof(out); c++) {
                        if (c < rp && (*c == '(' || *c == '[' || *c == '{'))
                            d++;
                        else if (c < rp &&
                                 (*c == ')' || *c == ']' || *c == '}')) {
                            if (d > 0) d--;
                        } else if ((c == rp || (*c == ',' && d == 0))) {
                            size_t al = (size_t)(c - cur);
                            while (al && (cur[al - 1] == ' ' ||
                                          cur[al - 1] == '\t'))
                                al--;
                            {
                                size_t lead = 0;
                                while (lead < al &&
                                       (cur[lead] == ' ' || cur[lead] == '\t'))
                                    lead++;
                                {
                                    const ShadowFnParam* fp =
                                        shadow_fnparam_lookup(callee, argi);
                                    int want_slc =
                                        shadow_slc_known_arg(callee, argi) ||
                                        (fp && fp->stars == 0 &&
                                         (strcmp(fp->base, "CCSlice") == 0 ||
                                          strcmp(fp->base, "CCSliceUnique") ==
                                              0 ||
                                          strcmp(fp->base, "CCSliceShared") ==
                                              0 ||
                                          strcmp(fp->base, "char[:]") == 0 ||
                                          strcmp(fp->base, "char[:0]") == 0 ||
                                          strcmp(fp->base, "char[:!]") == 0 ||
                                          strcmp(fp->base, "char[:0!]") == 0 ||
                                          strcmp(fp->base, "char[::]") == 0));
                                    if (want_slc &&
                                        !shadow_slc_already_wrapped(
                                            cur + lead, al - lead) &&
                                        shadow_slc_is_string_lit(cur + lead,
                                                                 al - lead)) {
                                        o += (size_t)snprintf(
                                            out + o, sizeof(out) - o,
                                            "CC_SLICE_LIT(%.*s)",
                                            (int)(al - lead), cur + lead);
                                    } else {
                                        /* Nested calls (e.g. cc_is_err(f.write(
                                         * "x"))) must rewrite inner CCSlice
                                         * lit args — outer callee consumes the
                                         * whole call as one arg. */
                                        char abuf[4096];
                                        if (al + 1 < sizeof(abuf)) {
                                            memcpy(abuf, cur, al);
                                            abuf[al] = 0;
                                            shadow_rewrite_slice_lit_call_args(
                                                abuf, sizeof(abuf));
                                            o += (size_t)snprintf(
                                                out + o, sizeof(out) - o, "%s",
                                                abuf);
                                        } else {
                                            o += (size_t)snprintf(
                                                out + o, sizeof(out) - o,
                                                "%.*s", (int)al, cur);
                                        }
                                    }
                                }
                            }
                            if (c < rp) {
                                out[o++] = ',';
                                cur = c + 1;
                                argi++;
                            }
                            continue;
                        }
                    }
                    out[o++] = ')';
                    p = rp + 1;
                    continue;
                }
            }
        }
        out[o++] = *p++;
    }
    out[o] = 0;
    snprintf(expr, cap, "%s", out);
}

/* @as arg coerce: takes_file(&w) → takes_file(&w.file) when Outer has @as. */
static void shadow_rewrite_as_call_args(char* expr, size_t cap) {
    char out[4096];
    size_t o = 0;
    const char* p;
    if (!expr || !cap || g_shadow_nas <= 0) return;
    p = expr;
    while (*p && o + 1 < sizeof(out)) {
        if (*p == '"' || *p == '\'') {
            if (!shadow_copy_quoted(&p, out, &o, sizeof(out))) break;
            continue;
        }
        if (((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') ||
             *p == '_') &&
            (p == expr ||
             !((p[-1] >= 'A' && p[-1] <= 'Z') || (p[-1] >= 'a' && p[-1] <= 'z') ||
               (p[-1] >= '0' && p[-1] <= '9') || p[-1] == '_'))) {
            const char* name = p;
            size_t nl = 0;
            const char* q;
            char callee[64];
            while ((p[nl] >= 'A' && p[nl] <= 'Z') ||
                   (p[nl] >= 'a' && p[nl] <= 'z') ||
                   (p[nl] >= '0' && p[nl] <= '9') || p[nl] == '_')
                nl++;
            q = name + nl;
            while (*q == ' ' || *q == '\t') q++;
            memcpy(callee, name, nl);
            callee[nl] = 0;
            if (*q == '(' && nl < sizeof(callee) &&
                shadow_fnparam_lookup(callee, 0)) {
                const char* args = q + 1;
                const char* rp = NULL;
                int depth = 1;
                const char* r;
                for (r = args; *r; r++) {
                    if (*r == '(') depth++;
                    else if (*r == ')') {
                        depth--;
                        if (depth == 0) {
                            rp = r;
                            break;
                        }
                    }
                }
                if (rp) {
                    int argi = 0;
                    const char* cur = args;
                    const char* c;
                    int d = 0;
                    o += (size_t)snprintf(out + o, sizeof(out) - o, "%s(",
                                          callee);
                    for (c = args; c <= rp && o + 64 < sizeof(out); c++) {
                        if (c < rp && (*c == '(' || *c == '[' || *c == '{'))
                            d++;
                        else if (c < rp &&
                                 (*c == ')' || *c == ']' || *c == '}')) {
                            if (d > 0) d--;
                        } else if ((c == rp || (*c == ',' && d == 0))) {
                            size_t al = (size_t)(c - cur);
                            const ShadowFnParam* fp =
                                shadow_fnparam_lookup(callee, argi);
                            int rewritten = 0;
                            while (al && (cur[al - 1] == ' ' ||
                                          cur[al - 1] == '\t'))
                                al--;
                            if (fp && fp->stars == 1 && fp->base[0]) {
                                size_t lead = 0;
                                int has_amp = 0;
                                char ident[64];
                                size_t ii = 0;
                                const ShadowBind* b;
                                char obase[64];
                                int ostars = 0;
                                const ShadowAsEmbed* as;
                                while (lead < al &&
                                       (cur[lead] == ' ' || cur[lead] == '\t'))
                                    lead++;
                                /* Explicit cast `(T*)&x` / `(T*)x` — keep as
                                 * written; warn when Outer has an @as path. */
                                if (lead < al && cur[lead] == '(') {
                                    const char* cast_end = NULL;
                                    int cd = 0;
                                    size_t k;
                                    char cast_base[64];
                                    size_t cbn = 0;
                                    for (k = lead; k < al; k++) {
                                        if (cur[k] == '(') cd++;
                                        else if (cur[k] == ')') {
                                            cd--;
                                            if (cd == 0) {
                                                cast_end = cur + k;
                                                break;
                                            }
                                        }
                                    }
                                    if (cast_end) {
                                        const char* cs = cur + lead + 1;
                                        while (cs < cast_end &&
                                               (*cs == ' ' || *cs == '\t'))
                                            cs++;
                                        if (strncmp(cs, "const ", 6) == 0)
                                            cs += 6;
                                        while (cs < cast_end &&
                                               ((*cs >= 'A' && *cs <= 'Z') ||
                                                (*cs >= 'a' && *cs <= 'z') ||
                                                (*cs >= '0' && *cs <= '9') ||
                                                *cs == '_') &&
                                               cbn + 1 < sizeof(cast_base))
                                            cast_base[cbn++] = *cs++;
                                        cast_base[cbn] = 0;
                                        while (cs < cast_end &&
                                               (*cs == ' ' || *cs == '\t' ||
                                                *cs == '*'))
                                            cs++;
                                        if (cs == cast_end && cast_base[0] &&
                                            strcmp(cast_base, fp->base) == 0) {
                                            size_t after = (size_t)(cast_end -
                                                                   cur) +
                                                           1;
                                            int ca = 0;
                                            char cid[64];
                                            size_t ci = 0;
                                            while (after < al &&
                                                   (cur[after] == ' ' ||
                                                    cur[after] == '\t'))
                                                after++;
                                            if (after < al &&
                                                cur[after] == '&') {
                                                ca = 1;
                                                after++;
                                                while (after < al &&
                                                       (cur[after] == ' ' ||
                                                        cur[after] == '\t'))
                                                    after++;
                                            }
                                            while (after + ci < al &&
                                                   ((cur[after + ci] >= 'A' &&
                                                     cur[after + ci] <= 'Z') ||
                                                    (cur[after + ci] >= 'a' &&
                                                     cur[after + ci] <= 'z') ||
                                                    (cur[after + ci] >= '0' &&
                                                     cur[after + ci] <= '9') ||
                                                    cur[after + ci] == '_') &&
                                                   ci + 1 < sizeof(cid)) {
                                                cid[ci] = cur[after + ci];
                                                ci++;
                                            }
                                            cid[ci] = 0;
                                            if (ci && after + ci == al) {
                                                b = shadow_bind_lookup(cid);
                                                if (b) {
                                                    shadow_ty_base_stars(
                                                        b->ty, obase,
                                                        sizeof(obase),
                                                        &ostars);
                                                    as = shadow_as_lookup(
                                                        obase, fp->base);
                                                    if (as) {
                                                        fprintf(stderr,
                                                                "warning: "
                                                                "explicit cast "
                                                                "to '%s *' "
                                                                "from '%s' "
                                                                "skips @as "
                                                                "path\n",
                                                                fp->base,
                                                                obase);
                                                        (void)ca;
                                                    }
                                                }
                                            }
                                        }
                                    }
                                } else if (lead < al && cur[lead] == '&') {
                                    has_amp = 1;
                                    lead++;
                                    while (lead < al &&
                                           (cur[lead] == ' ' ||
                                            cur[lead] == '\t'))
                                        lead++;
                                }
                                if (!rewritten && lead < al &&
                                    cur[lead] != '(') {
                                    while (lead + ii < al &&
                                           ((cur[lead + ii] >= 'A' &&
                                             cur[lead + ii] <= 'Z') ||
                                            (cur[lead + ii] >= 'a' &&
                                             cur[lead + ii] <= 'z') ||
                                            (cur[lead + ii] >= '0' &&
                                             cur[lead + ii] <= '9') ||
                                            cur[lead + ii] == '_') &&
                                           ii + 1 < sizeof(ident)) {
                                        ident[ii] = cur[lead + ii];
                                        ii++;
                                    }
                                    ident[ii] = 0;
                                    /* Bare ident only — skip member forms. */
                                    if (ii && lead + ii == al) {
                                        b = shadow_bind_lookup(ident);
                                        if (b) {
                                            shadow_ty_base_stars(
                                                b->ty, obase, sizeof(obase),
                                                &ostars);
                                            as = shadow_as_lookup(obase,
                                                                  fp->base);
                                            if (as) {
                                                if (has_amp || ostars == 0)
                                                    o += (size_t)snprintf(
                                                        out + o,
                                                        sizeof(out) - o,
                                                        "&%s.%s", ident,
                                                        as->field);
                                                else
                                                    o += (size_t)snprintf(
                                                        out + o,
                                                        sizeof(out) - o,
                                                        "&%s->%s", ident,
                                                        as->field);
                                                rewritten = 1;
                                            }
                                        }
                                    }
                                }
                            }
                            if (!rewritten)
                                o += (size_t)snprintf(out + o, sizeof(out) - o,
                                                      "%.*s", (int)al, cur);
                            if (c < rp) {
                                out[o++] = ',';
                                cur = c + 1;
                                argi++;
                            }
                            continue;
                        }
                    }
                    out[o++] = ')';
                    p = rp + 1;
                    continue;
                }
            }
        }
        out[o++] = *p++;
    }
    out[o] = 0;
    snprintf(expr, cap, "%s", out);
}

/* CCSlice_T value → erased CCSlice via bytes() when passed to a call. */
static void shadow_rewrite_slice_as_call_args(char* expr, size_t cap) {
    char out[8192];
    size_t o = 0;
    const char* p;
    if (!expr || !cap) return;
    p = expr;
    while (*p && o + 1 < sizeof(out)) {
        if (*p == '"' || *p == '\'') {
            if (!shadow_copy_quoted(&p, out, &o, sizeof(out))) break;
            continue;
        }
        if (((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') ||
             *p == '_') &&
            (p == expr ||
             !((p[-1] >= 'A' && p[-1] <= 'Z') || (p[-1] >= 'a' && p[-1] <= 'z') ||
               (p[-1] >= '0' && p[-1] <= '9') || p[-1] == '_'))) {
            const char* name = p;
            size_t nl = 0;
            const char* q;
            char callee[64];
            char arg[64];
            const ShadowBind* b;
            while ((p[nl] >= 'A' && p[nl] <= 'Z') ||
                   (p[nl] >= 'a' && p[nl] <= 'z') ||
                   (p[nl] >= '0' && p[nl] <= '9') || p[nl] == '_')
                nl++;
            q = name + nl;
            while (*q == ' ' || *q == '\t') q++;
            if (*q == '(' && nl < sizeof(callee)) {
                const char* a0 = q + 1;
                size_t al = 0;
                while (*a0 == ' ' || *a0 == '\t') a0++;
                while ((a0[al] >= 'A' && a0[al] <= 'Z') ||
                       (a0[al] >= 'a' && a0[al] <= 'z') ||
                       (a0[al] >= '0' && a0[al] <= '9') || a0[al] == '_')
                    al++;
                {
                    const char* r = a0 + al;
                    while (*r == ' ' || *r == '\t') r++;
                    if (*r == ')' && al > 0 && al < sizeof(arg)) {
                        memcpy(callee, name, nl);
                        callee[nl] = 0;
                        memcpy(arg, a0, al);
                        arg[al] = 0;
                        b = shadow_bind_lookup(arg);
                        if (b && strncmp(b->ty, "CCSlice_", 8) == 0 &&
                            strcmp(callee, "CCSlice_double_len") != 0 &&
                            strncmp(callee, "CCSlice_", 8) != 0) {
                            int skip = 0;
                            int di;
                            const ShadowFnParam* fp =
                                shadow_fnparam_lookup(callee, 0);
                            /* Dyn-sink WRAP macros (_Generic on CCSlice_T)
                             * must see the typed instance, not erased bytes. */
                            for (di = 0; di < g_shadow_ndsinks; di++) {
                                if (strcmp(g_shadow_dsinks[di].wrap, callee) ==
                                    0) {
                                    skip = 1;
                                    break;
                                }
                            }
                            /* Typed CCSlice_T params keep the instance. */
                            if (!skip && fp &&
                                strncmp(fp->base, "CCSlice_", 8) == 0)
                                skip = 1;
                            /* Header helpers like cc__py_arg_ts_CCSlice_double
                             * are not in the fnparam table — keep the typed
                             * value when the callee name already names it. */
                            if (!skip && b->ty[0] && strstr(callee, b->ty) != NULL)
                                skip = 1;
                            if (!skip) {
                                char sty[96];
                                shadow_bind_base_ty(b, sty, sizeof(sty));
                                o += (size_t)snprintf(
                                    out + o, sizeof(out) - o,
                                    "%s(%s_bytes(&%s))", callee, sty, arg);
                                p = r + 1;
                                continue;
                            }
                        }
                    }
                }
            }
        }
        out[o++] = *p++;
    }
    out[o] = 0;
    snprintf(expr, cap, "%s", out);
}

/* Leftover expr text UFCS (slots / chains). Prefer exact parts; else peel. */
static void shadow_emit_text_ufcs(char* dst, size_t cap, const char* src,
                                 const char* elem_ty) {
    /* Opaque switch bodies with expanded @string can exceed 512. */
    char cur[8192];
    char nxt[8192];
    int i;
    if (!src) { if (cap) dst[0] = 0; return; }
    if (shadow_emit_exact_ufcs_text(dst, cap, src, elem_ty)) {
        shadow_rewrite_slice_as_call_args(dst, cap);
        shadow_rewrite_slice_lit_call_args(dst, cap);
        shadow_rewrite_as_call_args(dst, cap);
        return;
    }
    snprintf(cur, sizeof(cur), "%s", src);
    /* Opaque switch bodies (redis command tables) can have dozens of UFCS
     * sites; 8 peels left later calls as surface member access. */
    for (i = 0; i < 64; i++) {
        if (!shadow_ufcs_peel_left(nxt, sizeof(nxt), cur, elem_ty)) break;
        snprintf(cur, sizeof(cur), "%s", nxt);
    }
    shadow_rewrite_slice_as_call_args(cur, sizeof(cur));
    shadow_rewrite_slice_lit_call_args(cur, sizeof(cur));
    shadow_rewrite_as_call_args(cur, sizeof(cur));
    /* Leftover `recv.meth(` / `recv->meth(` after peel → typed miss.
     * Instance/handle types always; other bound types when neither
     * Type_meth nor bare meth is a successful lower (leftover remains). */
    {
        const char* p = cur;
        int in_dq = 0, in_sq = 0, in_bt = 0;
        while (*p && !g_shadow_ufcs_miss) {
            int is_arrow = 0;
            const char* mstart = NULL;
            if (in_bt) {
                if (*p == '`') in_bt = 0;
                p++;
                continue;
            }
            if (in_dq) {
                if (*p == '\\' && p[1]) {
                    p += 2;
                    continue;
                }
                if (*p == '"') in_dq = 0;
                p++;
                continue;
            }
            if (in_sq) {
                if (*p == '\\' && p[1]) {
                    p += 2;
                    continue;
                }
                if (*p == '\'') in_sq = 0;
                p++;
                continue;
            }
            if (*p == '`') {
                in_bt = 1;
                p++;
                continue;
            }
            if (*p == '"') {
                in_dq = 1;
                p++;
                continue;
            }
            if (*p == '\'') {
                in_sq = 1;
                p++;
                continue;
            }
            if (p[0] == '-' && p[1] == '>' &&
                ((p[2] >= 'A' && p[2] <= 'Z') || (p[2] >= 'a' && p[2] <= 'z') ||
                 p[2] == '_')) {
                is_arrow = 1;
                mstart = p + 2;
            } else if (p[0] == '.' &&
                       ((p[1] >= 'A' && p[1] <= 'Z') ||
                        (p[1] >= 'a' && p[1] <= 'z') || p[1] == '_')) {
                mstart = p + 1;
            }
            if (mstart) {
                char recv[160], meth[64], vty[128], composed[160];
                const char* m = mstart;
                size_t mi = 0;
                const char* par;
                const ShadowBind* rb;
                int instance = 0;
                if (shadow_ufcs_recv(cur, p, recv, sizeof(recv))) {
                    /* Method name is an identifier only — never scan into
                     * `s.len != 0` / field compares looking for a later `(`. */
                    while (((m[mi] >= 'A' && m[mi] <= 'Z') ||
                            (m[mi] >= 'a' && m[mi] <= 'z') ||
                            (m[mi] >= '0' && m[mi] <= '9') || m[mi] == '_') &&
                           mi + 1 < sizeof(meth)) {
                        meth[mi] = m[mi];
                        mi++;
                    }
                    meth[mi] = 0;
                    par = m + mi;
                    while (*par == ' ' || *par == '\t') par++;
                    /* Optional ::[T] before '('. */
                    if (par[0] == ':' && par[1] == ':') {
                        par = shadow_ufcs_text_paren_after_meth(m + mi, NULL, 0);
                        if (!par) {
                            p++;
                            continue;
                        }
                    }
                    rb = shadow_bind_for_recv(recv);
                    shadow_bind_base_ty(rb, vty, sizeof(vty));
                    if (!vty[0])
                        (void)shadow_ufcs_recv_slice_ty(recv, vty, sizeof(vty));
                    instance =
                        vty[0] &&
                        (strncmp(vty, "CCSlice_", 8) == 0 ||
                         strcmp(vty, "CCSlice") == 0 ||
                         strncmp(vty, "CCVec_", 6) == 0 ||
                         strncmp(vty, "Map_", 4) == 0 ||
                         strncmp(vty, "ArrayMap_", 9) == 0 ||
                         strncmp(vty, "CCChan", 6) == 0 ||
                         strncmp(vty, "CCResult_", 9) == 0 ||
                         strncmp(vty, "Pair_", 5) == 0 ||
                         strcmp(vty, "double") == 0 ||
                         strcmp(vty, "int") == 0 ||
                         strcmp(vty, "float") == 0 ||
                         strcmp(vty, "size_t") == 0);
                    /* Instance/handle leftovers always diagnose. Bare-name
                     * mismatch when meth is a known fn. Capital typedefs
                     * diagnose only when meth is not a struct field (fnptr
                     * members like t.cb() stay as C field calls). */
                    if (par && *par == '(' && meth[0] && vty[0]) {
                        int capital = vty[0] >= 'A' && vty[0] <= 'Z' &&
                                      strncmp(vty, "CC", 2) != 0;
                        int chain = strchr(recv, '.') != NULL ||
                                    strstr(recv, "->") != NULL;
                        snprintf(composed, sizeof(composed), "%s_%s", vty, meth);
                        /* Field/member chains (`hp->tx.try_send_into`) use the
                         * peeled C call or valid member access — outer bind ty
                         * is not the UFCS receiver. */
                        if (!chain &&
                            (instance || shadow_ufn_exists(meth) ||
                             (capital && !shadow_type_has_field(vty, meth) &&
                              !shadow_ufn_exists(composed))))
                            shadow_ufcs_diagnose_miss(recv, meth);
                    }
                }
                (void)is_arrow;
            }
            p++;
        }
    }
    snprintf(dst, cap, "%s", cur);
}

/* Rewrite `c(args)` → `cc_closureN_call(c, (intptr_t)…)` for bound closures. */
static void shadow_rewrite_closure_calls(char* expr, size_t cap) {
    char out[8192];
    size_t o = 0;
    const char* p;
    if (!expr || !cap) return;
    p = expr;
    out[0] = 0;
    while (*p && o + 8 < sizeof(out)) {
        if (((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || *p == '_')) {
            char name[64];
            size_t nl = 0;
            const char* start = p;
            while ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
                   (*p >= '0' && *p <= '9') || *p == '_') {
                if (nl + 1 < sizeof(name)) name[nl++] = *p;
                p++;
            }
            name[nl] = 0;
            if (*p == '(') {
                const ShadowBind* b = shadow_bind_lookup(name);
                int arity = 0;
                if (b && strcmp(b->ty, "CCClosure1") == 0) arity = 1;
                else if (b && strcmp(b->ty, "CCClosure2") == 0) arity = 2;
                if (arity) {
                    const char* args0 = p + 1;
                    const char* q = args0;
                    int depth = 1;
                    char args[256];
                    size_t al;
                    while (*q && depth) {
                        if (*q == '(' || *q == '[' || *q == '{') depth++;
                        else if (*q == ')' || *q == ']' || *q == '}') depth--;
                        if (depth) q++;
                    }
                    if (*q != ')') {
                        /* fall through copy */
                    } else {
                        al = (size_t)(q - args0);
                        if (al >= sizeof(args)) al = sizeof(args) - 1;
                        memcpy(args, args0, al);
                        args[al] = 0;
                        if (arity == 1) {
                            o += (size_t)snprintf(out + o, sizeof(out) - o,
                                    "cc_closure1_call(%s, (intptr_t)(%s))",
                                    name, args);
                        } else {
                            char a0[128], a1[128];
                            const char* comma = NULL;
                            int d2 = 0;
                            const char* r;
                            a0[0] = a1[0] = 0;
                            for (r = args; *r; r++) {
                                if (*r == '(' || *r == '[' || *r == '{') d2++;
                                else if (*r == ')' || *r == ']' || *r == '}') d2--;
                                else if (*r == ',' && d2 == 0) { comma = r; break; }
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
                                o += (size_t)snprintf(out + o, sizeof(out) - o,
                                        "cc_closure2_call(%s, (intptr_t)(%s), "
                                        "(intptr_t)(%s))",
                                        name, a0, a1);
                            } else {
                                o += (size_t)snprintf(out + o, sizeof(out) - o,
                                        "cc_closure2_call(%s, (intptr_t)(%s), "
                                        "(intptr_t)0)",
                                        name, args);
                            }
                        }
                        p = q + 1;
                        continue;
                    }
                }
            }
            while (start < p && o + 1 < sizeof(out)) out[o++] = *start++;
            continue;
        }
        out[o++] = *p++;
    }
    out[o] = 0;
    snprintf(expr, cap, "%s", out);
}

/* Defined in pp_emit_stmt.cch (after this include). */
static void shadow_rewrite_print_and_string(char* expr, size_t cap);

/* Prefer structured UFCS on dbody; else leftover text UFCS (exact / peel). */
static void shadow_emit_expr_text(AstNode* st, const char* text, char* dst,
                                  size_t cap, const char* elem_ty) {
    int i;
    char cur[8192];
    /* Boolean / ternary mixes are multi-call — peel text, don't trust one kid. */
    int multi = text && (strstr(text, "&&") || strstr(text, "||") ||
                         strchr(text, '?'));
    /* `await` / already-lowered `cc_block_on` must win over a structured
     * UFCS kid: parse attaches `tx.send(...)` even when the surface was
     * `await tx.send(...)`, and the kid would emit sync send. */
    if (text) {
        const char* tp = text;
        while (*tp == ' ' || *tp == '\t') tp++;
        if (strncmp(tp, "await", 5) == 0 &&
            !shadow_is_id(tp[5] ? tp[5] : 0))
            multi = 1;
        else if (strncmp(tp, "cc_block_on", 11) == 0 &&
                 !shadow_is_id(tp[11] ? tp[11] : 0))
            multi = 1;
    }
    if (st && !multi) {
        for (i = 0; i < st->ndbody; i++) {
            AstNode* u = st->dbody[i];
            if (u && (u->kind == AST_UFCS_EXPR || u->kind == AST_UFCS_STMT)) {
                char surface[288];
                char cast_dest[96];
                const char* cast_call;
                g_shadow_sink_dest[0] = 0;
                shadow_ufcs_surface(u, surface, sizeof(surface));
                /* Whole-RHS decl/assign destination. */
                if (elem_ty && elem_ty[0] && text &&
                    shadow_text_eq_trim(text, surface))
                    snprintf(g_shadow_sink_dest, sizeof(g_shadow_sink_dest),
                             "%s", elem_ty);
                /* Cast destination wrapping the call (absorbed). */
                cast_call = shadow_sink_cast_dest(text, cast_dest,
                                                  sizeof(cast_dest));
                if (cast_call && shadow_text_eq_trim(cast_call, surface))
                    snprintf(g_shadow_sink_dest, sizeof(g_shadow_sink_dest),
                             "%s", cast_dest);
                if (shadow_emit_ufcs_to_buf(u, dst, cap, elem_ty)) {
                    g_shadow_sink_dest[0] = 0;
                    shadow_rewrite_closure_calls(dst, cap);
                    shadow_rewrite_print_and_string(dst, cap);
                    shadow_rewrite_at_slice(dst, cap);
                    /* ufcs_to_buf already lit-coerces; keep as_call here for
                     * any print/string rewrite that reintroduced bare lits. */
                    shadow_rewrite_slice_lit_call_args(dst, cap);
                    shadow_rewrite_as_call_args(dst, cap);
                    if (elem_ty && strncmp(elem_ty, "CCResult_", 9) == 0)
                        shadow_rewrite_result_ctors(dst, cap, elem_ty);
                    return;
                }
                g_shadow_sink_dest[0] = 0;
            }
        }
    }
    /* Lower @string before UFCS peel so chains like `@string(`…`).sub()` /
     * `.index_of()` / `.len()` see a CCSlice / CCString receiver. */
    {
        const char* tp = text ? text : "";
        while (*tp == ' ' || *tp == '\t') tp++;
        /* Expr-site `@noblock` / `@blocking` — strip; autoblock honors fn attrs. */
        if (tp[0] == '@' &&
            (strncmp(tp + 1, "noblock", 7) == 0 ||
             strncmp(tp + 1, "blocking", 8) == 0)) {
            tp++;
            while (*tp && *tp != ' ' && *tp != '\t' && *tp != '(') tp++;
            while (*tp == ' ' || *tp == '\t') tp++;
        }
        snprintf(cur, sizeof(cur), "%s", tp);
    }
    shadow_rewrite_print_and_string(cur, sizeof(cur));
    /* `@string(…).as_slice()` after template lower: stmt-expr yields CCString. */
    {
        const char* pat = "__cc_tpl; }).as_slice()";
        const char* repl = "cc_string_as_slice(&__cc_tpl); })";
        char* hit;
        while ((hit = strstr(cur, pat)) != NULL) {
            char tmp[8192];
            size_t pre = (size_t)(hit - cur);
            if (pre + strlen(repl) + strlen(hit + strlen(pat)) + 1 >= sizeof(tmp))
                break;
            snprintf(tmp, sizeof(tmp), "%.*s%s%s", (int)pre, cur, repl,
                     hit + strlen(pat));
            snprintf(cur, sizeof(cur), "%s", tmp);
        }
    }
    /* Lower @slice before UFCS peel so dots inside the payload stay in a
     * C string literal (not mistaken for method calls). */
    shadow_rewrite_at_slice(cur, sizeof(cur));
    /* Exact UFCS / cast-wrapped UFCS: dest-aware sink. */
    {
        char cast_dest[96];
        const char* cast_call = shadow_sink_cast_dest(cur, cast_dest,
                                                     sizeof(cast_dest));
        char exact_buf[8192];
        g_shadow_sink_dest[0] = 0;
        if (cast_call) {
            snprintf(g_shadow_sink_dest, sizeof(g_shadow_sink_dest), "%s",
                     cast_dest);
            if (shadow_emit_exact_ufcs_text(exact_buf, sizeof(exact_buf),
                                           cast_call, elem_ty)) {
                g_shadow_sink_dest[0] = 0;
                snprintf(dst, cap, "%s", exact_buf);
                shadow_rewrite_closure_calls(dst, cap);
                shadow_emit_leftover_qmark_rewrite(dst, cap);
                shadow_rewrite_at_slice(dst, cap);
                shadow_rewrite_variant_expr(dst, cap, elem_ty);
                if (elem_ty && strncmp(elem_ty, "CCResult_", 9) == 0)
                    shadow_rewrite_result_ctors(dst, cap, elem_ty);
                return;
            }
            /* Keep sink dest for peel — bang/`!>` suffix breaks exact match
             * (`d = mod.f(x)!>` still needs callm_int64_t). */
        } else if (elem_ty && elem_ty[0]) {
            /* Whole-text exact call with typed destination. */
            snprintf(g_shadow_sink_dest, sizeof(g_shadow_sink_dest), "%s",
                     elem_ty);
            if (shadow_emit_exact_ufcs_text(exact_buf, sizeof(exact_buf), cur,
                                           elem_ty)) {
                g_shadow_sink_dest[0] = 0;
                snprintf(dst, cap, "%s", exact_buf);
                shadow_rewrite_closure_calls(dst, cap);
                shadow_emit_leftover_qmark_rewrite(dst, cap);
                shadow_rewrite_at_slice(dst, cap);
                shadow_rewrite_variant_expr(dst, cap, elem_ty);
                if (elem_ty && strncmp(elem_ty, "CCResult_", 9) == 0)
                    shadow_rewrite_result_ctors(dst, cap, elem_ty);
                return;
            }
            /* Keep sink dest only for bang suffix — subexpression calls
             * (e.g. `w.foo(1.0) + 100.0`) must not inherit the decl type. */
            if (!strstr(cur, "!>"))
                g_shadow_sink_dest[0] = 0;
        }
    }
    shadow_emit_text_ufcs(dst, cap, cur, elem_ty);
    g_shadow_sink_dest[0] = 0;
    shadow_rewrite_closure_calls(dst, cap);
    shadow_emit_leftover_qmark_rewrite(dst, cap);
    shadow_rewrite_at_slice(dst, cap);
    shadow_rewrite_variant_expr(dst, cap, elem_ty);
    if (elem_ty && strncmp(elem_ty, "CCResult_", 9) == 0)
        shadow_rewrite_result_ctors(dst, cap, elem_ty);
    shadow_rewrite_generic_types_text(dst, cap);
    shadow_lower_type_of_constexpr(dst, cap);
}

/* Emit-site channel_pair → create_named (chan table). */
static int shadow_emit_channel_pair_expr(char* dst, size_t cap, const char* src) {
    char cur[768];
    char nxt[768];
    int n = 0;
    if (!src) { if (cap) dst[0] = 0; return 0; }
    snprintf(cur, sizeof(cur), "%s", src);
    while (shadow_format_channel_pair(nxt, sizeof(nxt), cur)) {
        snprintf(cur, sizeof(cur), "%s", nxt);
        n = 1;
    }
    if (n) snprintf(dst, cap, "%s", cur);
    return n;
}

/* Structured return !> unwrap (expr already lowered). */
static void shadow_emit_return_bang_unwrap(char* dst, size_t cap,
                                          const char* expr) {
    snprintf(dst, cap,
             "({ __typeof__(%s) __r = %s; "
             "if (__cc_uw_is_err(__r)) { "
             "CCError e = __cc_uw_err_at(__r, \"unwrap\", __FILE__, \"0\"); "
             "(void)e; return -1; } "
             "__cc_uw_value(__r); })",
             expr, expr);
}

/* Leftover: bare `r.is_err()` / `r.is_ok()` when not structured UFCS. */
static void shadow_rewrite_cond(char* dst, size_t cap, const char* cond) {
    size_t n = cond ? strlen(cond) : 0;
    if (n >= 9 && memcmp(cond + n - 9, ".is_err()", 9) == 0) {
        snprintf(dst, cap, "cc_is_err(%.*s)", (int)(n - 9), cond);
        return;
    }
    if (n >= 8 && memcmp(cond + n - 8, ".is_ok()", 8) == 0) {
        snprintf(dst, cap, "cc_is_ok(%.*s)", (int)(n - 8), cond);
        return;
    }
    snprintf(dst, cap, "%s", cond ? cond : "");
}

/* Cond: UFCS via emit_expr_text; leftover is_err / chan send-recv checks. */
static int shadow_emit_cond_text(AstNode* st, const char* text, char* dst,
                                size_t cap, ShadowCtx* ctx, CEmit* out) {
    char cur[2048];
    char nxt[2048];
    /* Diagnose bare designators on the source text before expr rewrite
     * (which may resolve `.arm` via the sole-variant fallback). */
    if (shadow_variant_diag_bare_desig(ctx, st, out, text)) return 0;
    shadow_emit_expr_text(st, text, cur, sizeof(cur), NULL);
    shadow_rewrite_cond(nxt, sizeof(nxt), cur);
    snprintf(cur, sizeof(cur), "%s", nxt);
    while (shadow_lower_chan_result_cmp(nxt, sizeof(nxt), cur))
        snprintf(cur, sizeof(cur), "%s", nxt);
    if (shadow_variant_diag_bare_desig(ctx, st, out, cur)) return 0;
    snprintf(dst, cap, "%s", cur);
    return 1;
}

/* Opaque C passthrough for unparsed static/enum/fn one-liner bodies. */
static void shadow_rewrite_ufcs(char* dst, size_t cap, const char* src,
                                const char* elem_ty) {
    char cur[2048];
    if (!src) {
        if (cap) dst[0] = 0;
        return;
    }
    /* Peel UFCS in-place via a temp — dst/src may alias.
     * @string first so chains see a real receiver. */
    snprintf(cur, sizeof(cur), "%s", src);
    shadow_rewrite_print_and_string(cur, sizeof(cur));
    shadow_emit_text_ufcs(dst, cap, cur, elem_ty);
    shadow_rewrite_variant_expr(dst, cap, elem_ty);
}

