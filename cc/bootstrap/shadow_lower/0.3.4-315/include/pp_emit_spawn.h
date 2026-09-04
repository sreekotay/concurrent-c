/* Emit: closure make, spawn collect, nursery create, defer epilogue.
 * Included from pp_emit_stmt.cch. */
#pragma once

/* Top-level arity of `args…)` (comma count + 1 when non-empty). */
#include "pp_emit_core.h"
#include "pp_tape.h"
static int shadow_create_arg_count(const char* args_with_close) {
    const char* p = args_with_close;
    int depth = 0, n = 0, any = 0;
    if (!p) return 0;
    while (*p == ' ' || *p == '\t') p++;
    if (*p == ')') return 0;
    for (; *p; p++) {
        if (*p == '(' || *p == '[' || *p == '{') depth++;
        else if (*p == ')' || *p == ']' || *p == '}') {
            if (depth == 0 && *p == ')') break;
            if (depth > 0) depth--;
        } else if (*p == ',' && depth == 0) {
            n++;
            any = 1;
        } else if (*p != ' ' && *p != '\t' && *p != '\n')
            any = 1;
    }
    return any ? n + 1 : 0;
}

static int shadow_create_arg0_is_str(const char* args_with_close) {
    const char* p = args_with_close;
    while (p && (*p == ' ' || *p == '\t')) p++;
    return p && *p == '"';
}

static int shadow_create_arg0_is_int(const char* args_with_close) {
    const char* p = args_with_close;
    while (p && (*p == ' ' || *p == '\t')) p++;
    if (!p) return 0;
    if (*p == '-' || *p == '+') p++;
    return *p >= '0' && *p <= '9';
}

/* Strip trailing `*` / spaces from a dest type (`Map_int_int*` → `Map_int_int`). */
static void shadow_type_base_nostar(const char* ty, char* dst, size_t cap) {
    size_t n;
    if (!dst || !cap) return;
    dst[0] = 0;
    if (!ty) return;
    snprintf(dst, cap, "%s", ty);
    n = strlen(dst);
    while (n && (dst[n - 1] == ' ' || dst[n - 1] == '\t' || dst[n - 1] == '*'))
        dst[--n] = 0;
}

static int shadow_type_has_new_factory(const char* base) {
    char trynew[160];
    if (!base || !base[0]) return 0;
    snprintf(trynew, sizeof(trynew), "%s_new", base);
    if (shadow_ufn_exists(trynew) || shadow_hdr_fn_exists(trynew)) return 1;
    if (strncmp(base, "CCVec_", 6) == 0) return 1;
    if (strncmp(base, "Map_", 4) == 0) return 1;
    if (strncmp(base, "ArrayMap_", 9) == 0) return 1;
    return 0;
}

/* Type → snake_case prefix (CreateToy → create_toy; CCArena → arena). */
static void shadow_type_snake(const char* ty, char* snake, size_t cap) {
    size_t si = 0, ti = 0;
    if (!ty || !snake || !cap) return;
    snake[0] = 0;
    if (strncmp(ty, "CC", 2) == 0 && ty[2] >= 'A' && ty[2] <= 'Z')
        ti = 2; /* CCArena → Arena → arena */
    for (; ty[ti] && si + 2 < cap; ti++) {
        char c = ty[ti];
        if (c >= 'A' && c <= 'Z') {
            if (si > 0) snake[si++] = '_';
            snake[si++] = (char)(c - 'A' + 'a');
        } else
            snake[si++] = c;
    }
    snake[si] = 0;
}

#define SHADOW_CREATE_DECL_TAG "decl:"

static int shadow_create_hook_is_decl(const char* hook) {
    return hook && strncmp(hook, SHADOW_CREATE_DECL_TAG, 5) == 0;
}

static const char* shadow_create_hook_callee(const char* hook) {
    if (shadow_create_hook_is_decl(hook)) return hook + 5;
    return hook;
}

/* `callee(args)` → `callee(name, args)` for declaration-form create. */
static int shadow_create_insert_binder(char* expr, size_t cap, const char* name) {
    char tmp[2048];
    const char* lp;
    if (!expr || !name || !name[0]) return 0;
    lp = strchr(expr, '(');
    if (!lp) return 0;
    snprintf(tmp, sizeof(tmp), "%.*s(%s, %s", (int)(lp - expr), expr, name,
             lp + 1);
    if (strlen(tmp) >= cap) return 0;
    snprintf(expr, cap, "%s", tmp);
    return 1;
}

/* Resolve `__cc_at_create(…)` from the declared dest type.
 * Returns 1 when expr is not a create binder, or when a real callee was
 * chosen. Returns 0 when `T name@(args)` has no .create hook / _new /
 * folklore callee — never invents arena or nursery.
 * `out_decl`: 1 when the chosen hook is declaration-form (`decl:callee`). */
static int shadow_resolve_at_create(char* expr, size_t cap, const char* ty,
                                    int is_ptr, int with_closure, int* out_decl) {
    char rest[256];
    char snake[96];
    char callee[128];
    const char* resolved;
    int arity;
    int decl = 0;
    const size_t prefix_len = sizeof("__cc_at_create(") - 1; /* 15 */
    callee[0] = 0;
    if (out_decl) *out_decl = 0;
    if (!expr) return 0;
    if (strncmp(expr, "__cc_at_create(", prefix_len) != 0) return 1;
    if (!ty || !ty[0]) {
        shadow_err(NULL, g_shadow_expr_site,
                   "type: @(...) create needs a declared type");
        return 0;
    }
    snprintf(rest, sizeof(rest), "%s", expr + prefix_len);
    {
        size_t rl = strlen(rest);
        while (rl && (rest[rl - 1] == ' ' || rest[rl - 1] == '\t')) rl--;
        /* Ensure create-arg text is a closed paren list for snprintf below. */
        if (!rl || rest[rl - 1] != ')') {
            if (rl + 1 < sizeof(rest)) {
                rest[rl++] = ')';
                rest[rl] = 0;
            }
        }
    }
    arity = shadow_create_arg_count(rest);
    resolved = shadow_td_alias_resolve(ty);
    if (!resolved || !resolved[0]) resolved = ty;
    shadow_type_snake(resolved, snake, sizeof(snake));
    if (strcmp(resolved, "CCNursery") == 0)
        /* `n@()` self-owned; `n@(parent)` create_child; closure → spawn. */
        snprintf(callee, sizeof(callee), "%s",
                 (with_closure || arity >= 2)
                     ? "cc_nursery_spawn_child_closure0"
                     : (arity >= 1) ? "cc_nursery_create_child"
                                    : "cc_nursery_create");
    else if (strcmp(resolved, "CCChan") == 0)
        snprintf(callee, sizeof(callee), "cc_channel_pair");
    else {
        char base[128];
        const char* reg;
        shadow_type_base_nostar(resolved, base, sizeof(base));
        reg = shadow_create_hook_for_arity(resolved, arity);
        if ((!reg || !reg[0]) && base[0] && strcmp(base, resolved) != 0)
            reg = shadow_create_hook_for_arity(base, arity);
        if (reg && reg[0]) {
            decl = shadow_create_hook_is_decl(reg);
            snprintf(callee, sizeof(callee), "%s",
                     shadow_create_hook_callee(reg));
        }
        if (!callee[0] && shadow_type_has_new_factory(base))
            snprintf(callee, sizeof(callee), "%s_new", base);
        if (!callee[0] && snake[0] && !is_ptr) {
        /* Registered create beats naming folklore (Holder → holder_make). */
        if (reg && reg[0] && arity == 0)
            snprintf(callee, sizeof(callee), "%s", reg);
        /* CreateToy name@(4) → create_toy_from_int; name@("x") → _from_cstr. */
        if (!callee[0] && arity == 1 && shadow_create_arg0_is_str(rest))
            snprintf(callee, sizeof(callee), "%s_from_cstr", snake);
        else if (!callee[0] && arity == 1 && shadow_create_arg0_is_int(rest))
            snprintf(callee, sizeof(callee), "%s_from_int", snake);
        /* `@typehooks` / `cc_type_create_call` wins over invented from_* when
         * that folklore symbol is not declared. */
        if (reg && reg[0] &&
            (!callee[0] || !shadow_ufn_exists(callee)))
            snprintf(callee, sizeof(callee), "%s", reg);
        if (!callee[0] || (!reg && !shadow_ufn_exists(callee))) {
            char tryc[128];
            snprintf(tryc, sizeof(tryc), "%s_create", snake);
            if (shadow_ufn_exists(tryc))
                snprintf(callee, sizeof(callee), "%s", tryc);
            else if (!callee[0]) {
                /* CCStdio → cc_stdio_create (header create hook / CC* naming). */
                snprintf(tryc, sizeof(tryc), "cc_%s_create", snake);
                if (reg && reg[0] && strcmp(reg, tryc) == 0)
                    snprintf(callee, sizeof(callee), "%s", reg);
                else if (shadow_ufn_exists(tryc))
                    snprintf(callee, sizeof(callee), "%s", tryc);
                else if (reg && reg[0])
                    snprintf(callee, sizeof(callee), "%s", reg);
                else
                    callee[0] = 0;
            } else if (!reg)
                callee[0] = 0;
        }
        }
    }
    if (!callee[0]) {
        {
            char __diag[192];
            snprintf(__diag, sizeof(__diag),
                     "type: '%s%s' has no .create hook", ty,
                     is_ptr ? "*" : "");
            shadow_err(NULL, g_shadow_expr_site, __diag);
        }
        return 0;
    }
    if (decl) {
        if (is_ptr) {
            shadow_err(NULL, g_shadow_expr_site,
                       "type: decl-form @create cannot bind a pointer");
            return 0;
        }
        if (!out_decl) {
            shadow_err(NULL, g_shadow_expr_site,
                       "type: decl-form @create needs a value binder");
            return 0;
        }
        *out_decl = 1;
    }
    snprintf(expr, cap, "%s(%s", callee, rest);
    /* Same dest-cast / lit wrap as a written `cc_file_open(path)` call. */
    shadow_rewrite_slice_lit_call_args(expr, cap);
    return 1;
}

/* Expr-level () => / (T x) => closure on dbody (create / typed-init). */
static int shadow_is_expr_closure(AstNode* kid) {
    if (!kid || kid->kind != AST_SPAWN_CLOSURE) return 0;
    if (strncmp(ast_slot(kid->b), "callarg", 7) == 0 || strcmp(ast_slot(kid->b), "create") == 0 ||
        strcmp(ast_slot(kid->b), "send_task") == 0 ||
        strcmp(ast_slot(kid->b), "send_task_hybrid") == 0)
        return 1;
    return 0;
}

/* Legacy spawn expr bodies store the call text in c and use cc_nursery_spawn_fn. */
static int shadow_closure_needs_codegen(AstNode* st) {
    if (!st || st->kind != AST_SPAWN_CLOSURE) return 0;
    if (strncmp(ast_slot(st->b), "callarg", 7) == 0 || strcmp(ast_slot(st->b), "create") == 0 ||
        strcmp(ast_slot(st->b), "send_task") == 0 ||
        strcmp(ast_slot(st->b), "send_task_hybrid") == 0)
        return 1;
    if (st->c && st->c[0]) return 0;
    return 1;
}

/* Infer `(slot, arena)` types for try_send_into / send_into callarg2 closures. */
static void shadow_infer_callarg2_send_into(AstNode* cl) {
    const char* ctx;
    const char* meth;
    char recv[128];
    char outer[64];
    char fld[32];
    char fty[96];
    const char* elem;
    const ShadowChanDecl* ch;
    const ShadowBind* b;
    size_t n;
    if (!cl || strcmp(ast_slot(cl->b), "callarg2") != 0 || !(cl->a && cl->a[0])) return;
    if (!strstr(ast_slot(cl->a), "intptr_t")) return;
    ctx = cl->c;
    if (!ctx || !ctx[0]) return;
    meth = strstr(ctx, ".try_send_into");
    if (!meth) meth = strstr(ctx, ".send_into");
    if (!meth) return;
    n = (size_t)(meth - ctx);
    if (n >= sizeof(recv)) n = sizeof(recv) - 1;
    memcpy(recv, ctx, n);
    recv[n] = 0;
    while (n && (recv[n - 1] == ' ' || recv[n - 1] == '\t')) recv[--n] = 0;
    if (!recv[0]) return;
    elem = NULL;
    ch = shadow_chan_find(recv);
    if (ch && ch->elem[0]) elem = ch->elem;
    if (!elem) {
        const char* arrow = strstr(recv, "->");
        const char* dot = strrchr(recv, '.');
        const char* fldp = NULL;
        if (arrow) {
            size_t on = (size_t)(arrow - recv);
            if (on >= sizeof(outer)) on = sizeof(outer) - 1;
            memcpy(outer, recv, on);
            outer[on] = 0;
            fldp = arrow + 2;
        } else if (dot) {
            size_t on = (size_t)(dot - recv);
            if (on >= sizeof(outer)) on = sizeof(outer) - 1;
            memcpy(outer, recv, on);
            outer[on] = 0;
            fldp = dot + 1;
        }
        if (fldp) {
            snprintf(fld, sizeof(fld), "%s", fldp);
            b = shadow_bind_lookup(outer);
            if (!b) b = shadow_bind_for_recv(outer);
            if (b && b->ty[0]) {
                char base[64];
                shadow_bind_base_ty(b, base, sizeof(base));
                if (shadow_field_ty_of(base, fld, fty, sizeof(fty))) {
                    ch = shadow_chan_find(fty);
                    if (ch && ch->elem[0]) elem = ch->elem;
                }
            }
        }
    }
    if (!elem) {
        b = shadow_bind_for_recv(recv);
        if (b && b->ty[0]) {
            ch = shadow_chan_find(b->ty);
            if (ch && ch->elem[0]) elem = ch->elem;
        }
    }
    if (!elem || !elem[0]) return;
    do { char __ast_tmp[4096]; snprintf(__ast_tmp, sizeof(__ast_tmp), "%s* slot, CCArena arena", elem); shadow_slot_set(&cl->a, __ast_tmp); } while (0);
}

static AstNode* shadow_expr_closure_kid(AstNode* st) {
    int k;
    if (!st) return NULL;
    for (k = 0; k < st->ndbody; k++) {
        if (shadow_is_expr_closure(st->dbody[k])) return st->dbody[k];
    }
    return NULL;
}

static int shadow_closure_arity(AstNode* cl) {
    if (!cl) return 0;
    if (strcmp(ast_slot(cl->b), "callarg1") == 0) return 1;
    if (strcmp(ast_slot(cl->b), "callarg2") == 0) return 2;
    return 0;
}

static const char* shadow_closure_cc_ty(AstNode* cl) {
    int a = shadow_closure_arity(cl);
    if (a == 1) return "CCClosure1";
    if (a == 2) return "CCClosure2";
    return "CCClosure0";
}

static int shadow_caps_call_args(const char* caps, char* dst, size_t cap);

/* Format cc_closure__Nid_make(caps…) into dst. */
static void shadow_fmt_closure_make(char* dst, size_t cap, AstNode* cl) {
    const char* id;
    char args[256];
    if (!dst || !cap || !cl) return;
    id = cl->d && cl->d[0] ? cl->d : "1";
    if (cl->e && cl->e[0] && shadow_caps_call_args(cl->e, args, sizeof(args)) && args[0])
        snprintf(dst, cap, "cc_closure__N%s_make(%s)", id, args);
    else
        snprintf(dst, cap, "cc_closure__N%s_make()", id);
}

/* Insert cc_closure__Nid_make(…) into a call expr. Handles:
 *   foo(args) / foo(args,) / foo(args  / foo(, rest)  */
static void shadow_splice_closure_arg(char* expr, size_t cap, AstNode* cl) {
    size_t L;
    int empty;
    char make[512];
    char base[2048];
    char with_cl[2560];
    const char* p;
    if (!expr || !cap || !cl) return;
    shadow_fmt_closure_make(make, sizeof(make), cl);
    /* Prefer empty first-arg slot: `foo(, rest)`. */
    for (p = expr; *p; p++) {
        if (*p == '(') {
            const char* q = p + 1;
            while (*q == ' ' || *q == '\t') q++;
            if (*q == ',') {
                char tail[256];
                const char* rest = q + 1;
                size_t pre = (size_t)(p - expr) + 1;
                size_t tl;
                while (*rest == ' ' || *rest == '\t') rest++;
                snprintf(tail, sizeof(tail), "%s", rest);
                tl = strlen(tail);
                while (tl && (tail[tl - 1] == ' ' || tail[tl - 1] == '\t'))
                    tail[--tl] = 0;
                while (tl && tail[tl - 1] == ')') tail[--tl] = 0;
                if (pre + strlen(make) + strlen(tail) + 8 >= cap) return;
                if (tail[0])
                    snprintf(with_cl, sizeof(with_cl), "%.*s%s, %s)", (int)pre,
                             expr, make, tail);
                else
                    snprintf(with_cl, sizeof(with_cl), "%.*s%s)", (int)pre, expr,
                             make);
                snprintf(expr, cap, "%s", with_cl);
                return;
            }
        }
    }
    /* Empty middle slot: `foo(a, , rest)`. */
    for (p = expr; *p; p++) {
        if (*p != '(') continue;
        {
            const char* q = p + 1;
            int depth = 0;
            while (*q) {
                if (*q == '(' || *q == '[' || *q == '{') depth++;
                else if (*q == ')' || *q == ']' || *q == '}') {
                    if (depth == 0 && *q == ')') break;
                    if (depth > 0) depth--;
                } else if (*q == ',' && depth == 0) {
                    const char* r = q + 1;
                    while (*r == ' ' || *r == '\t') r++;
                    if (*r == ',') {
                        char tail[256];
                        const char* rest = r + 1;
                        size_t pre = (size_t)(q - expr) + 1;
                        size_t tl;
                        while (*rest == ' ' || *rest == '\t') rest++;
                        snprintf(tail, sizeof(tail), "%s", rest);
                        tl = strlen(tail);
                        while (tl && (tail[tl - 1] == ' ' || tail[tl - 1] == '\t'))
                            tail[--tl] = 0;
                        while (tl && tail[tl - 1] == ')') tail[--tl] = 0;
                        if (pre + strlen(make) + strlen(tail) + 8 >= cap) return;
                        if (tail[0])
                            snprintf(with_cl, sizeof(with_cl), "%.*s%s, %s)",
                                     (int)pre, expr, make, tail);
                        else
                            snprintf(with_cl, sizeof(with_cl), "%.*s%s)", (int)pre,
                                     expr, make);
                        snprintf(expr, cap, "%s", with_cl);
                        return;
                    }
                }
                q++;
            }
        }
    }
    snprintf(base, sizeof(base), "%s", expr);
    L = strlen(base);
    while (L && (base[L - 1] == ' ' || base[L - 1] == '\t')) L--;
    if (L && base[L - 1] == ')') L--;
    while (L && (base[L - 1] == ' ' || base[L - 1] == '\t' ||
                 base[L - 1] == ','))
        L--;
    base[L] = 0;
    if (!L || !strchr(base, '(')) return;
    empty = (base[L - 1] == '(');
    if (empty)
        snprintf(with_cl, sizeof(with_cl), "%s%s)", base, make);
    else
        snprintf(with_cl, sizeof(with_cl), "%s, %s)", base, make);
    snprintf(expr, cap, "%s", with_cl);
}

/* dbody holds real stmts (PTR_UNWRAP destroy) or attachments (UFCS / create cl). */

/* Caps item: "@safe&x" | "&x" | "x" | "alias=init". */
static int shadow_cap_item_is_ref(const char* item) {
    return item && (item[0] == '&' || strncmp(item, "@safe&", 6) == 0);
}

/* Fills name; init_out → init or NULL. */
static void shadow_cap_item_parts(const char* item, char* name, size_t ncap,
                                  const char** init_out) {
    const char* eq;
    if (init_out) *init_out = NULL;
    if (!item || !name || !ncap) return;
    name[0] = 0;
    if (strncmp(item, "@safe&", 6) == 0) {
        snprintf(name, ncap, "%s", item + 6);
        return;
    }
    if (item[0] == '&') {
        snprintf(name, ncap, "%s", item + 1);
        return;
    }
    eq = strchr(item, '=');
    if (eq) {
        size_t n = (size_t)(eq - item);
        if (n >= ncap) n = ncap - 1;
        memcpy(name, item, n);
        name[n] = 0;
        if (init_out) *init_out = eq + 1;
        return;
    }
    snprintf(name, ncap, "%s", item);
}

/* Resolve capture value type into dst. Returns 1 if this item is a real
 * value-capture; 0 if it should be omitted (inferred free-ident that is a
 * file/global/enumerator — body uses the name as-is; do not invent `int`). */
static int shadow_cap_val_type(const char* name, char* dst, size_t cap) {
    const ShadowBind* b;
    const char* init = NULL;
    char alias[64];
    size_t n;
    const char* lookup;
    if (dst && cap) dst[0] = 0;
    shadow_cap_item_parts(name, alias, sizeof(alias), &init);
    if (g_shadow_cap_overflow || g_shadow_table_overflow) {
        /* Overflow already diagnosed — do not invent a type. */
        snprintf(dst, cap, "int");
        g_shadow_ufcs_miss = 1;
        return 1;
    }
    if (init && init[0] == '&') {
        const char* base = init + 1;
        while (*base == ' ' || *base == '\t') base++;
        b = shadow_cap_bind_lookup(base);
        if (!b) b = shadow_bind_lookup(base);
        if (b && b->ty[0]) {
            snprintf(dst, cap, "%s*", b->ty);
            return 1;
        }
        snprintf(dst, cap, "void*");
        return 1;
    }
    if (init) {
        b = shadow_cap_bind_lookup(alias);
        if (!b) b = shadow_bind_lookup(alias);
        if (!b) b = shadow_bind_lookup(init);
        if (b && b->ty[0]) {
            snprintf(dst, cap, "%s", b->ty);
            return 1;
        }
        snprintf(dst, cap, "void*");
        return 1;
    }
    lookup = alias[0] ? alias : name;
    b = shadow_cap_bind_lookup(lookup);
    n = lookup ? strlen(lookup) : 0;
    if (!b) b = shadow_bind_lookup(lookup);
    if (b && b->ty[0]) {
        snprintf(dst, cap, "%s", b->ty);
        return 1;
    }
    if ((lookup && strcmp(lookup, "rx") == 0) ||
        (lookup && n >= 3 && strcmp(lookup + n - 3, "_rx") == 0)) {
        snprintf(dst, cap, "CCChanRx");
        return 1;
    }
    if ((lookup && strcmp(lookup, "tx") == 0) ||
        (lookup && n >= 3 && strcmp(lookup + n - 3, "_tx") == 0)) {
        snprintf(dst, cap, "CCChanTx");
        return 1;
    }
    if (lookup && lookup[0] && shadow_ufn_exists(lookup)) {
        snprintf(dst, cap, "__cc_thread_fn");
        return 1;
    }
    /* No silent int: omit inferred globals/enumerators from the capture
     * pack. Real missing locals surface as undeclared in the entry body. */
    return 0;
}

/* Format a capture type + name as a C declarator (fn-ptr needs infix name). */
static void shadow_cap_decl(const char* ty, const char* name, char* dst,
                            size_t cap) {
    if (ty && strcmp(ty, "__cc_thread_fn") == 0)
        snprintf(dst, cap, "void *(*%s)(void *)", name);
    else
        snprintf(dst, cap, "%s %s", ty ? ty : "int", name);
}

/* Ref capture: array → T* identity; atomic → cc_atomic_int*; else T* + deref macro. */
static int shadow_cap_ref_is_array(const char* name) {
    const ShadowBind* b = shadow_cap_bind_lookup(name);
    if (!b) b = shadow_bind_lookup(name);
    return b && (b->flags & SHADOW_BIND_ARRAY);
}

static void shadow_cap_ref_base_type(const char* name, char* dst, size_t cap) {
    const ShadowBind* b = shadow_cap_bind_lookup(name);
    if (!b) b = shadow_bind_lookup(name);
    if (b && (b->flags & SHADOW_BIND_ATOMIC)) {
        snprintf(dst, cap, "cc_atomic_int");
        return;
    }
    if (b && b->ty[0]) {
        shadow_bind_base_ty(b, dst, cap);
        return;
    }
    snprintf(dst, cap, "int");
}

/* Capture list helpers: "x,&y" / "p=&x" → proto / call / env / entry unpack. */
static int shadow_caps_proto(const char* caps, char* dst, size_t cap) {
    if (!caps || !caps[0]) {
        snprintf(dst, cap, "void");
        return 1;
    }
    dst[0] = 0;
    const char* p = caps;
    int first = 1;
    while (*p) {
        const char* c = strchr(p, ',');
        size_t n = c ? (size_t)(c - p) : strlen(p);
        char item[80];
        size_t cur;
        size_t need;
        char piece[96];
        char vty[80];
        char cname[64];
        const char* init = NULL;
        if (n >= sizeof(item)) n = sizeof(item) - 1;
        memcpy(item, p, n);
        item[n] = 0;
        /* Skip hex/noise tokens that infer sometimes picks up (`x1f`, `ffff`). */
        if (item[0] == 'x' || item[0] == 'X') {
            const char* q = item + 1;
            int hex = (*q != 0);
            while (*q) {
                char ch = *q++;
                if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') ||
                      (ch >= 'A' && ch <= 'F'))) {
                    hex = 0;
                    break;
                }
            }
            if (hex) {
                if (!c) break;
                p = c + 1;
                continue;
            }
        }
        shadow_cap_item_parts(item, cname, sizeof(cname), &init);
        if (shadow_cap_item_is_ref(item))
            snprintf(piece, sizeof(piece), "void* __cc_opaque_%s", cname);
        else {
            if (!shadow_cap_val_type(item, vty, sizeof(vty))) {
                if (!c) break;
                p = c + 1;
                continue;
            }
            shadow_cap_decl(vty, cname, piece, sizeof(piece));
        }
        cur = strlen(dst);
        need = strlen(piece) + (first ? 0 : 2);
        if (cur + need + 1 >= cap) return 0;
        if (!first) strcat(dst, ", ");
        strcat(dst, piece);
        first = 0;
        if (!c) break;
        p = c + 1;
    }
    return 1;
}

static int shadow_caps_call_args(const char* caps, char* dst, size_t cap) {
    const char* p;
    int first = 1;
    if (!caps || !caps[0]) {
        dst[0] = 0;
        return 1;
    }
    dst[0] = 0;
    p = caps;
    while (*p) {
        const char* c = strchr(p, ',');
        size_t n = c ? (size_t)(c - p) : strlen(p);
        char item[80];
        char cname[64];
        const char* init = NULL;
        char piece[96];
        if (n >= sizeof(item)) n = sizeof(item) - 1;
        memcpy(item, p, n);
        item[n] = 0;
        shadow_cap_item_parts(item, cname, sizeof(cname), &init);
        if (shadow_cap_item_is_ref(item))
            snprintf(piece, sizeof(piece), "&%s", cname);
        else if (init)
            snprintf(piece, sizeof(piece), "%s", init);
        else {
            char vty[80];
            /* Keep call args aligned with proto — omit non-captures. */
            if (!shadow_cap_val_type(item, vty, sizeof(vty))) {
                if (!c) break;
                p = c + 1;
                continue;
            }
            snprintf(piece, sizeof(piece), "%s", cname);
        }
        {
            size_t cur = strlen(dst);
            size_t need = strlen(piece) + (first ? 0 : 2);
            if (cur + need + 1 >= cap) return 0;
            if (!first) strcat(dst, ", ");
            strcat(dst, piece);
        }
        first = 0;
        if (!c) break;
        p = c + 1;
    }
    return 1;
}

static int shadow_collect_scope_defers(AstNode** body, int nbody,
                                       AstNode** defers, int* n, int cap);

static int shadow_emit_closure_body(AstNode* cl, CEmit* out, TapeCache* cache) {
    char body_ind[64];
    AstNode* defers[16];
    AstNode* destroys[32];
    int ndefers = 0;
    int ndestroys = 0;
    int has_life = 0;
    int bind_mark;
    shadow_pick_body_indent(cl->body, cl->nbody, body_ind, sizeof(body_ind));
    ShadowCtx ctx = { .cache = cache, .body_indent = body_ind };
    ctx.send_task_ret = (cl->b && cl->b[0] && (strcmp(ast_slot(cl->b), "send_task") == 0 ||
                                      strcmp(ast_slot(cl->b), "send_task_hybrid") == 0));
    for (int k = 0; k < cl->nbody; k++) {
        if (!shadow_collect_scope_defers(&cl->body[k], 1, defers, &ndefers,
                                         16)) {
            shadow_err(NULL, cl, "too many @defer in closure body");
            out->err = 1;
            return 0;
        }
        /* Closure-level @destroy (same ledger as fn-level; §5.1 / §4.2.2). */
        if (shadow_stmt_is_destroy(cl->body[k])) {
            if (ndestroys >= 32) {
                shadow_err(NULL, cl, "too many @destroy in closure body");
                out->err = 1;
                return 0;
            }
            destroys[ndestroys++] = cl->body[k];
        }
    }
    ctx.defers = defers;
    ctx.ndefers = ndefers;
    ctx.destroys = destroys;
    ctx.ndestroys = ndestroys;
    has_life = (ndefers > 0 || ndestroys > 0);
    /* Soft-return epilogue: entry is void*; fallthrough returns NULL. */
    ctx.soft_ret_ty = "void*";
    ctx.goto_cleanup = has_life;
    ctx.defer_cleanup = ndefers > 0;
    /* Capture names are unpacked in the entry prologue (not AST locals). Bind
     * them for UFCS before walking the body (`cap->arena.free()` etc.). */
    bind_mark = g_shadow_nbinds;
    {
        const char* p = cl->e;
        while (p && *p) {
            const char* c = strchr(p, ',');
            size_t n = c ? (size_t)(c - p) : strlen(p);
            char item[80];
            char cname[64];
            const char* init = NULL;
            char vty[80];
            int flags = 0;
            if (n >= sizeof(item)) n = sizeof(item) - 1;
            memcpy(item, p, n);
            item[n] = 0;
            shadow_cap_item_parts(item, cname, sizeof(cname), &init);
            if (shadow_cap_item_is_ref(item)) {
                /* Entry rewrites bare uses to `(*__cc_ref_name)` — UFCS sees T. */
                shadow_cap_ref_base_type(cname, vty, sizeof(vty));
                if (shadow_cap_ref_is_array(cname)) {
                    char ptr_ty[96];
                    snprintf(ptr_ty, sizeof(ptr_ty), "%s*", vty);
                    shadow_bind_name(cname, ptr_ty, flags);
                } else {
                    shadow_bind_name(cname, vty, flags);
                }
            } else {
                if (!shadow_cap_val_type(item, vty, sizeof(vty))) {
                    if (!c) break;
                    p = c + 1;
                    continue;
                }
                if (strcmp(vty, "__cc_thread_fn") != 0)
                    shadow_bind_name(cname, vty, flags);
            }
            if (!c) break;
            p = c + 1;
        }
    }
    (void)shadow_push_block_binds(cl->body, cl->nbody);
    {
        int need_scratch = 0;
        for (int k = 0; k < cl->nbody; k++)
            need_scratch |= shadow_stmt_uses_scratch(cl->body[k]);
        if (need_scratch) {
            if (!cemit_fmt(out, "%scc_arena_stack(__cc_str_scratch, 1024);\n",
                           body_ind)) {
                shadow_pop_fn_binds(bind_mark);
                return 0;
            }
        }
        if (has_life && !shadow_emit_destroy_soft_vars(out, &ctx, body_ind)) {
            shadow_pop_fn_binds(bind_mark);
            return 0;
        }
        for (int k = 0; k < cl->nbody; k++) {
            AstNode* st = cl->body[k];
            /* AST_DEFER: emit_stmt is a no-op; hw_bump stamps registration. */
            if (!shadow_emit_stmt_ctx(st, out, &ctx, body_ind, has_life)) {
                shadow_pop_fn_binds(bind_mark);
                return 0;
            }
            if (has_life && !shadow_emit_hw_bump(out, &ctx, st, body_ind)) {
                shadow_pop_fn_binds(bind_mark);
                return 0;
            }
        }
        if (!shadow_eh_emit_trailers(out, &ctx, body_ind,
                                    shadow_fn_body_already_left(cl->body,
                                                                cl->nbody))) {
            shadow_pop_fn_binds(bind_mark);
            return 0;
        }
        if (ndefers > 0 && has_life) {
            if (!shadow_emit_defer_epilogue(out, &ctx)) {
                shadow_pop_fn_binds(bind_mark);
                return 0;
            }
        } else if (has_life) {
            if (!shadow_emit_destroy_cleanup(out, &ctx, destroys, ndestroys,
                                             body_ind)) {
                shadow_pop_fn_binds(bind_mark);
                return 0;
            }
        }
        shadow_pop_fn_binds(bind_mark);
    }
    return 1;
}

/* Emit `Type name = (Type)__argN;` for callarg1/2 formals in cl->a.
 * (Spawn/create keep recv/empty in a — not formals.) */
static int shadow_emit_closure_formals(AstNode* cl, CEmit* out) {
    const char* p;
    int ai = 0;
    if (!cl || !(cl->a && cl->a[0]) || shadow_closure_arity(cl) == 0) return 1;
    p = cl->a;
    while (*p) {
        char ty[32];
        char name[32];
        size_t ti = 0, ni = 0;
        while (*p == ' ') p++;
        while (*p && *p != ' ' && *p != ',' && ti + 1 < sizeof(ty)) ty[ti++] = *p++;
        ty[ti] = 0;
        while (*p == ' ') p++;
        while (*p && *p != ',' && *p != ' ' && ni + 1 < sizeof(name))
            name[ni++] = *p++;
        name[ni] = 0;
        if (!ty[0] || !name[0]) return 0;
        /* Runtime passes handle formals as intptr_t &handle. A C cast
         * `(CCArena)__arg` is invalid; reload the handle from that slot. */
        if (shadow_ty_is_box_alias(ty) || shadow_ty_is_box_instance(ty)) {
            if (!cemit_fmt(out, "    %s %s = *(%s*)__arg%d;\n", ty, name, ty, ai))
                return 0;
        } else if (!cemit_fmt(out, "    %s %s = (%s)__arg%d;\n", ty, name, ty, ai)) {
            return 0;
        }
        ai++;
        while (*p && *p != ',') p++;
        if (*p == ',') p++;
    }
    return 1;
}

static int shadow_emit_closure_def_raw(AstNode* cl, CEmit* out,
                                       TapeCache* cache);

static int shadow_emit_closure_def(AstNode* cl, CEmit* out, TapeCache* cache) {
    CEmit syn = {0};
    ShadowCtx pin = { .cache = cache, .site = cl };
    int ok = shadow_emit_closure_def_raw(cl, &syn, cache);
    if (ok && syn.buf && syn.buf[0])
        ok = shadow_emit_pinned_block(out, &pin, cl, "", syn.buf);
    free(syn.buf);
    return ok;
}

static int shadow_emit_closure_def_raw(AstNode* cl, CEmit* out, TapeCache* cache) {
    const char* id = cl->d && cl->d[0] ? cl->d : "1";
    const char* caps = cl->e;
    const char* ccty = shadow_closure_cc_ty(cl);
    int arity = shadow_closure_arity(cl);
    /* Cap-deep-collect this body before nested make() protos (emitted later)
     * resolve capture types — e.g. `CCSocket client` in a serve callback
     * captured by an inner spawn. */
    shadow_cap_collect_closure_locals(cl);
    shadow_infer_callarg2_send_into(cl);
    const char* make_fn =
        arity == 1 ? "cc_closure1_make" :
        arity == 2 ? "cc_closure2_make" : "cc_closure0_make";
    char proto[256];
    char entry_sig[96];
    char refs[32][64];
    int nrefs = 0;
    if (!shadow_caps_proto(caps, proto, sizeof(proto))) return 0;
    if (arity == 1)
        snprintf(entry_sig, sizeof(entry_sig), "void* __p, intptr_t __arg0");
    else if (arity == 2)
        snprintf(entry_sig, sizeof(entry_sig),
                 "void* __p, intptr_t __arg0, intptr_t __arg1");
    else
        snprintf(entry_sig, sizeof(entry_sig), "void* __p");

    if (!caps || !caps[0]) {
        if (arity == 0) {
            ShadowTplBind cenv[1];
            cenv[0] = (ShadowTplBind){ "id", SHADOW_TPL_IDENT, id };
            if (!shadow_tpl_emit(out, k_tpl_closure0_nocaps, cenv, 1))
                return 0;
        } else {
            if (!cemit_fmt(out, "\n/* CC closure %s */\n", id)) return 0;
            if (!cemit_fmt(out,
                    "static %s cc_closure__N%s_make(void) {\n"
                    "    return %s(cc_closure__N%s_entry, NULL, NULL);\n"
                    "}\n"
                    "static void* cc_closure__N%s_entry(%s) {\n"
                    "    (void)__p;\n",
                    ccty, id, make_fn, id, id, entry_sig))
                return 0;
            if (!shadow_emit_closure_formals(cl, out)) return 0;
        }
        if (!shadow_emit_closure_body(cl, out, cache)) return 0;
        return cemit_str(out, "    return NULL;\n}\n");
    }

    if (!cemit_fmt(out, "\n/* CC closure %s */\n", id)) return 0;
    /* env struct */
    if (!cemit_fmt(out, "typedef struct cc_closure__N%s_env {\n", id)) return 0;
    {
        const char* p = caps;
        while (*p) {
            const char* c = strchr(p, ',');
            size_t n = c ? (size_t)(c - p) : strlen(p);
            char item[80];
            char cname[64];
            const char* init = NULL;
            char vty[80];
            char decl[96];
            if (n >= sizeof(item)) n = sizeof(item) - 1;
            memcpy(item, p, n);
            item[n] = 0;
            shadow_cap_item_parts(item, cname, sizeof(cname), &init);
            if (shadow_cap_item_is_ref(item)) {
                if (!cemit_fmt(out, "    void* %s;\n", cname)) return 0;
            } else {
                if (!shadow_cap_val_type(item, vty, sizeof(vty))) {
                    if (!c) break;
                    p = c + 1;
                    continue;
                }
                shadow_cap_decl(vty, cname, decl, sizeof(decl));
                if (!cemit_fmt(out, "    %s;\n", decl)) return 0;
            }
            if (!c) break;
            p = c + 1;
        }
    }
    if (!cemit_fmt(out,
            "} cc_closure__N%s_env;\n"
            "static void cc_closure__N%s_env_drop(void* p) { if (p) cc__heap_free(p); }\n"
            "static %s cc_closure__N%s_make(%s) {\n"
            "    cc_closure__N%s_env* __env = (cc_closure__N%s_env*)cc__heap_alloc(sizeof(*__env));\n",
            id, id, ccty, id, proto, id, id))
        return 0;
    {
        const char* p = caps;
        while (*p) {
            const char* c = strchr(p, ',');
            size_t n = c ? (size_t)(c - p) : strlen(p);
            char item[80];
            char cname[64];
            const char* init = NULL;
            if (n >= sizeof(item)) n = sizeof(item) - 1;
            memcpy(item, p, n);
            item[n] = 0;
            shadow_cap_item_parts(item, cname, sizeof(cname), &init);
            if (shadow_cap_item_is_ref(item)) {
                if (!cemit_fmt(out, "    __env->%s = __cc_opaque_%s;\n",
                               cname, cname))
                    return 0;
            } else {
                char vty[80];
                if (!shadow_cap_val_type(item, vty, sizeof(vty))) {
                    if (!c) break;
                    p = c + 1;
                    continue;
                }
                if (!cemit_fmt(out, "    __env->%s = %s;\n", cname, cname))
                    return 0;
            }
            if (!c) break;
            p = c + 1;
        }
    }
    if (!cemit_fmt(out,
            "    return %s(cc_closure__N%s_entry, __env, "
            "cc_closure__N%s_env_drop);\n"
            "}\n"
            "static void* cc_closure__N%s_entry(%s) {\n"
            "    cc_closure__N%s_env* __env = (cc_closure__N%s_env*)__p;\n",
            make_fn, id, id, id, entry_sig, id, id))
        return 0;
    {
        const char* p = caps;
        while (*p) {
            const char* c = strchr(p, ',');
            size_t n = c ? (size_t)(c - p) : strlen(p);
            char item[80];
            char cname[64];
            const char* init = NULL;
            if (n >= sizeof(item)) n = sizeof(item) - 1;
            memcpy(item, p, n);
            item[n] = 0;
            shadow_cap_item_parts(item, cname, sizeof(cname), &init);
            if (shadow_cap_item_is_ref(item)) {
                char rty[80];
                shadow_cap_ref_base_type(cname, rty, sizeof(rty));
                if (shadow_cap_ref_is_array(cname)) {
                    if (!cemit_fmt(out, "    %s* %s = (%s*)__env->%s;\n",
                                   rty, cname, rty, cname))
                        return 0;
                } else {
                    /* Bare uses of the name in the body are redirected by
                     * shadow_refs_rewrite_emit below; member names and
                     * literals are untouched. */
                    if (!cemit_fmt(out, "    %s* __cc_ref_%s = (%s*)__env->%s;\n",
                                   rty, cname, rty, cname))
                        return 0;
                    if (nrefs < (int)(sizeof(refs) / sizeof(refs[0]))) {
                        snprintf(refs[nrefs], sizeof(refs[nrefs]), "%s",
                                 cname);
                        nrefs++;
                    } else {
                        shadow_err(NULL, cl,
                                   "closure reference capture list too long");
                        return 0;
                    }
                }
            } else {
                char vty[80];
                char decl[96];
                if (!shadow_cap_val_type(item, vty, sizeof(vty))) {
                    if (!c) break;
                    p = c + 1;
                    continue;
                }
                if (strcmp(vty, "__cc_thread_fn") == 0) {
                    if (!cemit_fmt(out,
                            "    void *(*%s)(void *) = __env->%s;\n",
                            cname, cname))
                        return 0;
                } else {
                    shadow_cap_decl(vty, cname, decl, sizeof(decl));
                    (void)decl;
                    if (!cemit_fmt(out, "    %s %s = __env->%s;\n", vty, cname,
                                   cname))
                        return 0;
                }
            }
            if (!c) break;
            p = c + 1;
        }
    }
    {
        /* Formals and body go through the reference rewrite: bare uses
         * of a `&`-captured name become derefs of its `__cc_ref_` spill;
         * member names (`p->x`) stay untouched. */
        CEmit t = {0};
        if (!shadow_emit_closure_formals(cl, &t)) {
            free(t.buf);
            return 0;
        }
        if (!shadow_emit_closure_body(cl, &t, cache)) {
            free(t.buf);
            return 0;
        }
        if (t.buf && t.len &&
            !shadow_refs_rewrite_emit(out, t.buf, t.len, refs, nrefs)) {
            free(t.buf);
            return 0;
        }
        free(t.buf);
    }
    return cemit_str(out, "    return NULL;\n}\n");
}

static int shadow_named_defer_flag_name(AstNode* d, char* buf, size_t cap) {
    if (!d || d->kind != AST_DEFER || !(d->a && d->a[0]) || !buf || cap < 16) return 0;
    snprintf(buf, cap, "__cc_dc_%zu", d->tok_off);
    return 1;
}

static int shadow_emit_named_defer_flag(CEmit* out, AstNode* d,
                                        const char* indent) {
    char fl[80];
    if (!shadow_named_defer_flag_name(d, fl, sizeof(fl))) return 1;
    if (!indent) indent = "    ";
    return cemit_fmt(out, "%sint %s = 1;\n", indent, fl);
}

static int shadow_defer_is_adopted(AstNode* d);
static int shadow_adopted_defer_flag_name(AstNode* d, char* buf, size_t cap);

static int shadow_emit_named_defer_flags(CEmit* out, AstNode** xs, int n,
                                         const char* indent) {
    int i;
    char fl[80];
    if (!out || !xs) return 1;
    if (!indent) indent = "    ";
    for (i = 0; i < n; i++) {
        if (!shadow_emit_named_defer_flag(out, xs[i], indent)) return 0;
        if (shadow_defer_is_adopted(xs[i]) &&
            shadow_adopted_defer_flag_name(xs[i], fl, sizeof(fl))) {
            if (!cemit_fmt(out, "%sint %s = 0;\n", indent, fl)) return 0;
        }
    }
    return 1;
}

static int shadow_defer_is_adopted(AstNode* d) {
    return d && d->kind == AST_DEFER && strcmp(ast_slot(d->e), "arm") == 0;
}

static int shadow_adopted_defer_flag_name(AstNode* d, char* buf, size_t cap) {
    if (!d || !buf || cap < 16) return 0;
    snprintf(buf, cap, "__cc_dreg_%zu", d->tok_off);
    return 1;
}

/* Braceless `if` / `else` `@defer` registers on the enclosing block.
 * Loop bodies are their own scope — do not lift those. A `{ }` then-arm
 * is also its own scope. */
static int shadow_collect_adopted_defers(AstNode* st, AstNode** defers, int* n,
                                         int cap) {
    int i;
    if (!st || !defers || !n) return 1;
    if (st->kind == AST_BLOCK) return 1;
    if (st->kind == AST_DEFER) {
        if (*n >= cap) return 0;
        shadow_slot_set(&st->e, "arm");
        defers[(*n)++] = st;
        return 1;
    }
    if (st->kind == AST_IF) {
        for (i = 0; i < st->nbody; i++) {
            if (!shadow_collect_adopted_defers(st->body[i], defers, n, cap))
                return 0;
        }
        return 1;
    }
    return 1;
}

static int shadow_collect_scope_defers(AstNode** body, int nbody,
                                       AstNode** defers, int* n, int cap) {
    int k;
    if (!body || !defers || !n) return 1;
    for (k = 0; k < nbody; k++) {
        if (!body[k]) continue;
        if (body[k]->kind == AST_DEFER) {
            if (*n >= cap) return 0;
            defers[(*n)++] = body[k];
        } else if (!shadow_collect_adopted_defers(body[k], defers, n, cap))
            return 0;
    }
    return 1;
}

static AstNode* shadow_named_defer_find(ShadowCtx* ctx, const char* name,
                                        size_t before) {
    int d, i;
    if (!ctx || !name || !name[0]) return NULL;
    for (d = ctx->defer_depth - 1; d >= 0; d--) {
        ShadowDeferScope* sc = &ctx->defer_stack[d];
        for (i = sc->nlife - 1; i >= 0; i--) {
            AstNode* n = sc->life[i];
            if (n && n->kind == AST_DEFER && n->a && n->a[0] &&
                strcmp(ast_slot(n->a), name) == 0 && n->tok_off < before)
                return n;
        }
    }
    for (i = ctx->ndefers - 1; i >= 0; i--) {
        AstNode* n = ctx->defers[i];
        if (n && n->kind == AST_DEFER && n->a && n->a[0] &&
            strcmp(ast_slot(n->a), name) == 0 && n->tok_off < before)
            return n;
    }
    return NULL;
}

static int shadow_emit_cancel(AstNode* st, CEmit* out, ShadowCtx* ctx,
                              const char* indent) {
    AstNode* d;
    char fl[80];
    if (!st || !(st->a && st->a[0])) {
        shadow_err(ctx, st, "@cancel_defer requires a name");
        if (out) out->err = 1;
        return 0;
    }
    d = shadow_named_defer_find(ctx, st->a, st->tok_off);
    if (!d || !shadow_named_defer_flag_name(d, fl, sizeof(fl))) {
        {
            char __diag[192];
            snprintf(__diag, sizeof(__diag),
                     "@cancel_defer '%s': no named @defer in scope", st->a);
            shadow_err(ctx, st, __diag);
        }
        if (out) out->err = 1;
        return 0;
    }
    if (!indent) indent = "    ";
    return cemit_fmt(out, "%s%s = 0;\n", indent, fl);
}

static int shadow_emit_one_defer(AstNode* d, CEmit* out, ShadowCtx* ctx,
                                  const char* indent, int always_only) {
    const char* mode;
    char i1[80];
    char gate_ind[80];
    char named_ind[80];
    char nested[80];
    char fl[80];
    char dreg[80];
    char dreg_ind[80];
    char adopt_close[80];
    const char* outer;
    const char* body_ind;
    const char* flag_ind;
    int hw_idx;
    int saved_goto;
    int named;
    int adopted;
    int k;
    int ok = 1;
    if (!d) return 1;
    mode = ast_slot(d->c);
    if (always_only && mode[0]) return 1;
    if (!indent) indent = (ctx && ctx->body_indent) ? ctx->body_indent : "    ";
    outer = indent;
    body_ind = indent;
    named = shadow_named_defer_flag_name(d, fl, sizeof(fl));
    adopted = shadow_defer_is_adopted(d);
    adopt_close[0] = 0;
    /* Adopted `if` then-arm sites use a private reach flag, not the
     * monotonic hw mark (a later taken arm must not enable a skipped one). */
    hw_idx = adopted ? -1 : shadow_lifecycle_index(ctx, d);
    saved_goto = ctx ? ctx->goto_cleanup : 0;
    if (ctx) ctx->goto_cleanup = 0;
    if (ctx) ctx->defer_emit = 1;
    if (hw_idx >= 0) {
        if (!cemit_fmt(out, "%sif (__cc_defer_hw > %d) {\n", outer, hw_idx)) {
            if (ctx) {
                ctx->defer_emit = 0;
                ctx->goto_cleanup = saved_goto;
            }
            return 0;
        }
        shadow_indent_nest(gate_ind, sizeof(gate_ind), outer, 1);
        body_ind = gate_ind;
    }
    if (adopted) {
        if (!shadow_adopted_defer_flag_name(d, dreg, sizeof(dreg))) {
            if (ctx) {
                ctx->defer_emit = 0;
                ctx->goto_cleanup = saved_goto;
            }
            return 0;
        }
        if (!cemit_fmt(out, "%sif (%s) {\n", body_ind, dreg)) {
            if (ctx) {
                ctx->defer_emit = 0;
                ctx->goto_cleanup = saved_goto;
            }
            return 0;
        }
        snprintf(adopt_close, sizeof(adopt_close), "%s", body_ind);
        shadow_indent_nest(dreg_ind, sizeof(dreg_ind), body_ind, 1);
        body_ind = dreg_ind;
    }
    flag_ind = body_ind;
    if (named) {
        if (!cemit_fmt(out, "%sif (%s) {\n", body_ind, fl)) {
            if (ctx) {
                ctx->defer_emit = 0;
                ctx->goto_cleanup = saved_goto;
            }
            return 0;
        }
        shadow_indent_nest(named_ind, sizeof(named_ind), body_ind, 1);
        body_ind = named_ind;
    }
    shadow_indent_nest(i1, sizeof(i1), body_ind, 1);
    shadow_indent_nest(nested, sizeof(nested), body_ind, 1);
    (void)nested;
    if (mode[0] == 0) {
        for (k = 0; k < d->nbody; k++) {
            if (!shadow_emit_stmt_ctx(d->body[k], out, ctx, body_ind, 0)) {
                ok = 0;
                break;
            }
        }
    } else if (strcmp(mode, "ok") == 0) {
        if (!cemit_fmt(out, "%sif (!__cc_ret_err) {\n", body_ind)) ok = 0;
        for (k = 0; ok && k < d->nbody; k++) {
            if (!shadow_emit_stmt_ctx(d->body[k], out, ctx, i1, 0)) ok = 0;
        }
        if (ok && !cemit_fmt(out, "%s}\n", body_ind)) ok = 0;
    } else if (strcmp(mode, "err") == 0) {
        if (!cemit_fmt(out, "%sif (__cc_ret_err) {\n", body_ind)) ok = 0;
        for (k = 0; ok && k < d->nbody; k++) {
            if (!shadow_emit_stmt_ctx(d->body[k], out, ctx, i1, 0)) ok = 0;
        }
        if (ok && !cemit_fmt(out, "%s}\n", body_ind)) ok = 0;
    }
    if (ok && named) {
        if (!cemit_fmt(out, "%s}\n", flag_ind)) ok = 0;
    }
    if (ok && adopted && adopt_close[0]) {
        if (!cemit_fmt(out, "%s}\n", adopt_close)) ok = 0;
    }
    if (ok && hw_idx >= 0) {
        if (!cemit_fmt(out, "%s}\n", outer)) ok = 0;
    }
    if (ctx) {
        ctx->defer_emit = 0;
        ctx->goto_cleanup = saved_goto;
    }
    return ok;
}

#ifndef SHADOW_TOK_OFF_ALL
#define SHADOW_TOK_OFF_ALL 0x7fffffff
#endif

static int shadow_scope_life_index(const ShadowDeferScope* sc, AstNode* st) {
    int i;
    if (!sc || !st) return -1;
    for (i = 0; i < sc->nlife; i++)
        if (sc->life[i] == st) return i;
    return -1;
}

/* Reverse-order gated cleanups for one lexical scope.
 * before_tok_off: only sites with tok_off < cutoff (SHADOW_TOK_OFF_ALL = all).
 * Skips textually-later decls so early return/break does not name undeclared
 * locals. */
static int shadow_emit_scope_life(const ShadowDeferScope* sc, CEmit* out,
                                  ShadowCtx* ctx, const char* indent,
                                  int always_only, int before_tok_off) {
    int i;
    char gate[80];
    char line[96];
    if (!sc || sc->nlife <= 0) return 1;
    if (!indent) indent = "    ";
    for (i = sc->nlife - 1; i >= 0; i--) {
        AstNode* n = sc->life[i];
        int is_def = n && n->kind == AST_DEFER;
        if (!n) continue;
        if (before_tok_off != SHADOW_TOK_OFF_ALL &&
            n->tok_off >= (size_t)before_tok_off)
            continue;
        if (is_def && always_only && n->c && n->c[0]) continue;
        if (is_def && shadow_defer_is_adopted(n)) {
            char dreg[80];
            if (!shadow_adopted_defer_flag_name(n, dreg, sizeof(dreg)))
                return 0;
            snprintf(line, sizeof(line), "%sif (%s) {\n", indent, dreg);
            if (!shadow_emit_pinned_block(out, ctx, n, "", line)) return 0;
            shadow_indent_nest(gate, sizeof(gate), indent, 1);
            if (!shadow_pin_line(out, ctx, n, gate)) return 0;
            if (!shadow_emit_one_defer(n, out, ctx, gate, always_only))
                return 0;
            snprintf(line, sizeof(line), "%s}\n", indent);
            if (!shadow_emit_pinned_block(out, ctx, n, "", line)) return 0;
            continue;
        }
        /* Pin gate + body to the @destroy/@defer declaration site. */
        snprintf(line, sizeof(line), "%sif (__cc_shw%d > %d) {\n", indent,
                 sc->shw_id, i);
        if (!shadow_emit_pinned_block(out, ctx, n, "", line)) return 0;
        shadow_indent_nest(gate, sizeof(gate), indent, 1);
        if (!shadow_pin_line(out, ctx, n, gate)) return 0;
        if (is_def) {
            if (!shadow_emit_one_defer(n, out, ctx, gate, always_only))
                return 0;
        } else {
            if (!shadow_emit_one_destroy(n, out, ctx, gate)) return 0;
        }
        snprintf(line, sizeof(line), "%s}\n", indent);
        if (!shadow_emit_pinned_block(out, ctx, n, "", line)) return 0;
    }
    return 1;
}

static int shadow_emit_scopes_exiting(ShadowCtx* ctx, CEmit* out,
                                      const char* indent, int from_depth,
                                      int to_depth, int always_only,
                                      int before_tok_off) {
    int d;
    if (!ctx || from_depth < 0) return 1;
    if (to_depth < 0) to_depth = 0;
    for (d = from_depth; d >= to_depth; d--) {
        if (!shadow_emit_scope_life(&ctx->defer_stack[d], out, ctx, indent,
                                    always_only, before_tok_off))
            return 0;
    }
    return 1;
}

/* break/continue: unconditional @defer/@destroy only (skip @defer(ok|err)). */
static int __attribute__((unused)) shadow_emit_defers_exiting(
    ShadowCtx* ctx, CEmit* out, const char* indent, int from_depth,
    int to_depth) {
    return shadow_emit_scopes_exiting(ctx, out, indent, from_depth, to_depth,
                                      1, SHADOW_TOK_OFF_ALL);
}

static int shadow_defer_exit_depth(ShadowCtx* ctx, int for_break) {
    int d;
    if (!ctx || ctx->defer_depth <= 0) return -1;
    for (d = ctx->defer_depth - 1; d >= 0; d--) {
        if (ctx->defer_stack[d].is_loop) return d;
    }
    return for_break ? 0 : -1;
}

static int shadow_emit_scope_hw_bump(CEmit* out, const ShadowDeferScope* sc,
                                    AstNode* st, const char* indent) {
    int li = shadow_scope_life_index(sc, st);
    if (li < 0) return 1;
    return cemit_fmt(out, "%s__cc_shw%d = %d;\n", indent, sc->shw_id, li + 1);
}

/* Opaque switch/for text: `@defer stmt;` → run stmt before jumps / block end. */
static void shadow_rewrite_defer_in_opaque(char* body, size_t cap) {
    char out[8192];
    char stmt[256];
    const char* p;
    int guard = 0;
    if (!body || !cap || !strstr(body, "@defer")) return;
    snprintf(out, sizeof(out), "%s", body);
    while (guard++ < 16 && (p = strstr(out, "@defer")) != NULL) {
        const char* s;
        const char* semi;
        size_t pre = (size_t)(p - out);
        size_t sl;
        char tmp[8192];
        char region[8192];
        /* Skip @defer(ok|err). */
        s = p + 6;
        while (*s == ' ' || *s == '\t') s++;
        if (*s == '(') break;
        semi = strchr(s, ';');
        if (!semi) break;
        sl = (size_t)(semi + 1 - s);
        if (sl == 0 || sl >= sizeof(stmt)) break;
        memcpy(stmt, s, sl);
        stmt[sl] = 0;
        /* Drop `@defer stmt;` from text. */
        snprintf(tmp, sizeof(tmp), "%.*s%s", (int)pre, out, semi + 1);
        snprintf(out, sizeof(out), "%s", tmp);
        /* Enclosing `{…}` after insertion point: inject before break/continue/
         * return and before the matching `}`. */
        {
            int depth = 0;
            size_t i = pre;
            size_t block_end = 0;
            size_t len = strlen(out);
            /* Find block end from current position (depth starts at 1 if inside). */
            depth = 1;
            for (; i < len; i++) {
                if (out[i] == '{') depth++;
                else if (out[i] == '}') {
                    depth--;
                    if (depth == 0) {
                        block_end = i;
                        break;
                    }
                }
            }
            if (!block_end) break;
            memcpy(region, out + pre, block_end - pre);
            region[block_end - pre] = 0;
            /* If the block jumps out, inject cleanup on those edges only;
             * otherwise run cleanup at the closing brace (fallthrough). */
            {
                char r2[8192];
                char* q = region;
                size_t ro = 0;
                int has_jump = (strstr(region, "break;") != NULL ||
                                strstr(region, "continue;") != NULL ||
                                strstr(region, "return") != NULL);
                r2[0] = 0;
                if (has_jump) {
                    while (*q && ro + 1 < sizeof(r2)) {
                        if ((q == region ||
                             !((q[-1] >= 'a' && q[-1] <= 'z') ||
                               (q[-1] >= 'A' && q[-1] <= 'Z') ||
                               (q[-1] >= '0' && q[-1] <= '9') ||
                               q[-1] == '_')) &&
                            (strncmp(q, "break;", 6) == 0 ||
                             strncmp(q, "continue;", 9) == 0 ||
                             strncmp(q, "return", 6) == 0)) {
                            const char* end = q;
                            if (strncmp(q, "break;", 6) == 0) end = q + 6;
                            else if (strncmp(q, "continue;", 9) == 0)
                                end = q + 9;
                            else {
                                end = q + 6;
                                while (*end && *end != ';') end++;
                                if (*end == ';') end++;
                            }
                            int n = snprintf(r2 + ro, sizeof(r2) - ro,
                                             "{ %s %.*s }", stmt,
                                             (int)(end - q), q);
                            if (n < 0) break;
                            ro += (size_t)n;
                            q = (char*)end;
                            continue;
                        }
                        r2[ro++] = *q++;
                    }
                    r2[ro] = 0;
                    snprintf(tmp, sizeof(tmp), "%.*s%s%s", (int)pre, out, r2,
                             out + block_end);
                } else {
                    snprintf(tmp, sizeof(tmp), "%.*s%s%s%s", (int)pre, out,
                             region, stmt, out + block_end);
                }
                snprintf(out, sizeof(out), "%s", tmp);
            }
        }
    }
    if (strlen(out) < cap) snprintf(body, cap, "%s", out);
}

static int shadow_emit_compound_body(AstNode** body, int nbody, CEmit* out,
                                     ShadowCtx* ctx, const char* indent,
                                     int use_cleanup, int is_loop) {
    int k;
    int depth;
    int nlife = 0;
    AstNode* life[SHADOW_DEFER_SCOPE_DEFERS];
    ShadowDeferScope* sc;
    if (!ctx || ctx->defer_depth >= SHADOW_DEFER_SCOPE_MAX) {
        shadow_err(ctx, ctx ? ctx->site : NULL, "defer scope depth exceeded");
        out->err = 1;
        return 0;
    }
    depth = ctx->defer_depth;
    memset(&ctx->defer_stack[depth], 0, sizeof(ctx->defer_stack[0]));
    sc = &ctx->defer_stack[depth];
    sc->is_loop = is_loop ? 1 : 0;
    sc->shw_id = depth;
    /* Collect @defer / @destroy in source order, including braceless
     * `if` / loop then-arm `@defer` (those register on this block). */
    for (k = 0; k < nbody; k++) {
        if (!body[k]) continue;
        if (body[k]->kind == AST_DEFER || shadow_stmt_is_destroy(body[k])) {
            if (nlife >= SHADOW_DEFER_SCOPE_DEFERS) {
                shadow_err(ctx, body[k], "too many @defer/@destroy in scope");
                out->err = 1;
                return 0;
            }
            life[nlife++] = body[k];
            continue;
        }
        if (!shadow_collect_adopted_defers(body[k], life, &nlife,
                                          SHADOW_DEFER_SCOPE_DEFERS)) {
            shadow_err(ctx, body[k], "too many @defer/@destroy in scope");
            out->err = 1;
            return 0;
        }
    }
    for (k = 0; k < nlife; k++) sc->life[k] = life[k];
    sc->nlife = nlife;
    if (nlife > 0) {
        if (!cemit_fmt(out, "%sint __cc_shw%d = 0;\n", indent, sc->shw_id))
            return 0;
        if (!shadow_emit_named_defer_flags(out, life, nlife, indent))
            return 0;
    }
    ctx->defer_depth++;
    int saved_nehs = ctx->nehs;
    AstNode* saved_eh = ctx->eh;
    int saved_eh_scope = ctx->eh_scope;
    ctx->eh_scope++;
    for (k = 0; k < nbody; k++) {
        if (body[k]->kind == AST_DEFER) {
            /* Registration only — body runs at scope exit / unwind. */
            if (!shadow_emit_scope_hw_bump(out, sc, body[k], indent)) {
                ctx->nehs = saved_nehs;
                ctx->eh = saved_eh;
                ctx->eh_scope = saved_eh_scope;
                ctx->defer_depth--;
                return 0;
            }
            continue;
        }
        if (!shadow_emit_stmt_ctx(body[k], out, ctx, indent, use_cleanup)) {
            ctx->nehs = saved_nehs;
            ctx->eh = saved_eh;
            ctx->eh_scope = saved_eh_scope;
            ctx->defer_depth--;
            return 0;
        }
        if (shadow_stmt_is_destroy(body[k]) &&
            !shadow_emit_scope_hw_bump(out, sc, body[k], indent)) {
            ctx->nehs = saved_nehs;
            ctx->eh = saved_eh;
            ctx->eh_scope = saved_eh_scope;
            ctx->defer_depth--;
            return 0;
        }
    }
    ctx->nehs = saved_nehs;
    ctx->eh = saved_eh;
    ctx->eh_scope = saved_eh_scope;
    ctx->defer_depth--;
    /* Fallthrough: run registered life (ok/err modes honored). */
    return shadow_emit_scope_life(sc, out, ctx, indent, 0, SHADOW_TOK_OFF_ALL);
}


static int shadow_emit_defer_epilogue(CEmit* out, ShadowCtx* ctx) {
    AstNode* ret_site;
    if (!ctx || !ctx->ndefers) return 1;
    const char* ind = ctx->body_indent ? ctx->body_indent : "    ";
    char i1[80];
    shadow_indent_nest(i1, sizeof(i1), ind, 1);
    /* Preserve the return site â defer bodies overwrite ctx->site. */
    ret_site = ctx->site;
    /* Cleanup label + soft-return pin to the return, not the last @defer.
     * Unwrap here inlines the handler: the trailer label lives above
     * this epilogue and may not exist if no body site used it. */
    ctx->eh_in_cleanup = 1;
    if (!shadow_emit_pinned_block(out, ctx, ret_site, "", "__cc_cleanup:\n"))
        return 0;
    /* Interleave @destroy and @defer in reverse declaration order (tok_off)
     * so later-declared resources are released before earlier ones. */
    {
        int di = (ctx->destroys && ctx->ndestroys > 0) ? ctx->ndestroys - 1 : -1;
        int fi = ctx->ndefers - 1;
        while (di >= 0 || fi >= 0) {
            int do_dest = 0;
            if (di >= 0 && fi >= 0)
                do_dest = ctx->destroys[di]->tok_off >= ctx->defers[fi]->tok_off;
            else
                do_dest = (di >= 0);
            if (do_dest) {
                if (!shadow_emit_one_destroy(ctx->destroys[di], out, ctx, ind))
                    return 0;
                di--;
            } else {
                if (!shadow_emit_one_defer(ctx->defers[fi], out, ctx, ind, 0))
                    return 0;
                fi--;
            }
        }
    }
    ctx->site = ret_site;
    {
        int ok = shadow_emit_soft_return(out, ctx, ind);
        ctx->eh_in_cleanup = 0;
        return ok;
    }
}

static int shadow_collect_spawns(AstNode* st, AstNode** spawns, int* nspawns,
                                int cap) {
    if (!st) return 1;
    if (shadow_closure_needs_codegen(st)) {
        if (*nspawns >= cap) return 0;
        do { char __ast_tmp[4096]; snprintf(__ast_tmp, sizeof(__ast_tmp), "%d", g_shadow_closure_id++); shadow_slot_set(&st->d, __ast_tmp); } while (0);
        spawns[(*nspawns)++] = st;
    }
    for (int k = 0; k < st->nbody; k++) {
        if (!shadow_collect_spawns(st->body[k], spawns, nspawns, cap)) return 0;
    }
    for (int k = 0; k < st->ndbody; k++) {
        if (!shadow_collect_spawns(st->dbody[k], spawns, nspawns, cap)) return 0;
    }
    if (st->kids) {
        for (int k = 0; k < st->nkids; k++) {
            if (!shadow_collect_spawns(st->kids[k], spawns, nspawns, cap)) return 0;
        }
    }
    return 1;
}

