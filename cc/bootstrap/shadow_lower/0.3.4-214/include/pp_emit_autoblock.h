/* Autoblock emit: wrap blocking call edges inside @async bodies.
 * Included from pp_emit_stmt.cch after pp_emit_spawn.cch. */
#pragma once

static int g_shadow_ab_id;

enum {
    SHADOW_AB_BATCH_MAX = 8,
};

typedef struct {
    AstNode* st;
    char callee[64];
    char args[288];
} ShadowAbItem;

typedef struct {
    int id;
    int n;
    ShadowAbItem items[SHADOW_AB_BATCH_MAX];
    int tail_kind; /* 0 none, 1 return-call, 2 assign-call */
    char tail_lhs[64];
    char tail_callee[64];
    char tail_args[288];
    int registered;
} ShadowAbBatch;

enum { SHADOW_AB_BATCH_CAP = 256 };
enum { SHADOW_AB_TAG_CAP = 256 };
static ShadowAbBatch g_shadow_ab_batches[SHADOW_AB_BATCH_CAP];

typedef struct {
    AstNode* st;
    char tag[16];
} ShadowAbNodeTag;
static ShadowAbNodeTag g_shadow_ab_tags[SHADOW_AB_TAG_CAP];
static int g_shadow_nab_tags;

static void shadow_ab_tag_set(AstNode* st, const char* tag) {
    int i;
    if (!st || !tag || !tag[0]) return;
    for (i = 0; i < g_shadow_nab_tags; i++) {
        if (g_shadow_ab_tags[i].st == st) {
            snprintf(g_shadow_ab_tags[i].tag, sizeof(g_shadow_ab_tags[i].tag),
                     "%s", tag);
            return;
        }
    }
    if (g_shadow_nab_tags >= SHADOW_AB_TAG_CAP) {
        shadow_table_full("autoblock_tags", SHADOW_AB_TAG_CAP, tag);
        return;
    }
    g_shadow_ab_tags[g_shadow_nab_tags].st = st;
    snprintf(g_shadow_ab_tags[g_shadow_nab_tags].tag,
             sizeof(g_shadow_ab_tags[g_shadow_nab_tags].tag), "%s", tag);
    g_shadow_nab_tags++;
}

static const char* shadow_ab_tag_get(AstNode* st) {
    int i;
    if (!st) return NULL;
    for (i = 0; i < g_shadow_nab_tags; i++) {
        if (g_shadow_ab_tags[i].st == st) return g_shadow_ab_tags[i].tag;
    }
    return NULL;
}

/* Unpack cast for `__e->__aN` in autoblock entry. Packing always stores
 * `(intptr_t)arg`; unpack must restore the callee parameter type — never
 * a blanket `(intptr_t*)` (wrong for `DB*`) or bare `(intptr_t)` for `&x`. */
static const char* shadow_ab_arg_cast_fn(const char* callee, int argi,
                                         const char* arg) {
    static char bufs[8][96];
    static unsigned bi;
    const ShadowFnParam* fp;
    const ShadowBind* lb;
    const char* p;
    char* slot;
    int i;
    size_t o;

    fp = callee ? shadow_fnparam_lookup(callee, argi) : NULL;
    if (fp && fp->base[0] && fp->stars > 0) {
        slot = bufs[bi++ & 7u];
        o = 0;
        o += (size_t)snprintf(slot + o, sizeof(bufs[0]) - o, "(%s", fp->base);
        for (i = 0; i < fp->stars && o + 1 < sizeof(bufs[0]); i++)
            slot[o++] = '*';
        if (o + 1 < sizeof(bufs[0])) {
            slot[o++] = ')';
            slot[o] = 0;
        } else {
            slot[sizeof(bufs[0]) - 1] = 0;
        }
        return slot;
    }

    if (!arg || !arg[0]) return "(intptr_t)";
    p = arg;
    while (*p == ' ' || *p == '\t') p++;
    if (*p >= '0' && *p <= '9') return "(intptr_t)";
    if (*p == '-' && p[1] >= '0' && p[1] <= '9') return "(intptr_t)";

    /* `&local` → `(T*)` from the referent's bind type. */
    if (p[0] == '&') {
        const char* name = p + 1;
        while (*name == ' ' || *name == '\t') name++;
        lb = shadow_bind_lookup(name);
        if (lb && lb->ty[0] && !strchr(lb->ty, '*')) {
            slot = bufs[bi++ & 7u];
            snprintf(slot, sizeof(bufs[0]), "(%s*)", lb->ty);
            return slot;
        }
        if (lb && lb->ty[0] && strchr(lb->ty, '*')) {
            slot = bufs[bi++ & 7u];
            snprintf(slot, sizeof(bufs[0]), "(%s)", lb->ty);
            return slot;
        }
        return "(void*)";
    }

    if (*p == '"') return "(const char*)";
    if (*p == '\'') return "(intptr_t)"; /* char literal is an int value */

    lb = shadow_bind_lookup(p);
    if (lb && lb->ty[0] && strchr(lb->ty, '*')) {
        slot = bufs[bi++ & 7u];
        snprintf(slot, sizeof(bufs[0]), "(%s)", lb->ty);
        return slot;
    }
    return "(intptr_t)";
}

static int shadow_autoblock_skip_callee(const char* name) {
    if (!name || !name[0]) return 1;
    if (strncmp(name, "cc_", 3) == 0) return 1;
    if (strcmp(name, "println") == 0 || strcmp(name, "eprintln") == 0 ||
        strcmp(name, "fprintln") == 0)
        return 1;
    return 0;
}

static int shadow_ab_is_user_fn(const char* name) {
    /* TU def, or decl-shaped `name(` in a lowered local .cch — not a
     * std-header spelling or function-like macro. */
    return name && name[0] &&
           (shadow_ufn_exists(name) || cc_lowered_local_declares_fn(name));
}

static int shadow_ty_is_structy(const char* ty) {
    if (!ty || !ty[0]) return 0;
    if (strstr(ty, "struct ") || strstr(ty, "struct{")) return 1;
    if (strcmp(ty, "int") == 0 || strcmp(ty, "void") == 0 ||
        strcmp(ty, "char") == 0 || strcmp(ty, "bool") == 0 ||
        strcmp(ty, "size_t") == 0 || strcmp(ty, "intptr_t") == 0 ||
        strcmp(ty, "float") == 0 || strcmp(ty, "double") == 0)
        return 0;
    return 1;
}

static int shadow_parse_call_expr(const char* expr, char* fn, size_t fncap,
                                  char* args, size_t argcap) {
    const char* p;
    const char* lp;
    const char* rp;
    size_t fl;
    if (!expr || !fn || !args) return 0;
    fn[0] = args[0] = 0;
    p = expr;
    while (*p == ' ' || *p == '\t') p++;
    lp = strchr(p, '(');
    if (!lp) return 0;
    fl = (size_t)(lp - p);
    while (fl && (p[fl - 1] == ' ' || p[fl - 1] == '\t')) fl--;
    if (fl == 0 || fl >= fncap) return 0;
    memcpy(fn, p, fl);
    fn[fl] = 0;
    rp = strrchr(lp, ')');
    if (!rp || rp <= lp) return 0;
    {
        size_t al = (size_t)(rp - lp - 1);
        if (al >= argcap) al = argcap - 1;
        memcpy(args, lp + 1, al);
        args[al] = 0;
    }
    return 1;
}

static void shadow_ab_batch_save(const ShadowAbBatch* b) {
    if (!b || b->id < 0) return;
    if (b->id >= SHADOW_AB_BATCH_CAP) {
        shadow_table_full("autoblock_batches", SHADOW_AB_BATCH_CAP, NULL);
        return;
    }
    g_shadow_ab_batches[b->id] = *b;
    g_shadow_ab_batches[b->id].registered = 1;
}

static const ShadowAbBatch* shadow_ab_batch_get(int id) {
    if (id < 0 || id >= SHADOW_AB_BATCH_CAP) return NULL;
    if (!g_shadow_ab_batches[id].registered) return NULL;
    return &g_shadow_ab_batches[id];
}

static int shadow_ab_split_args(const char* args, char out[][128], int cap) {
    int n = 0;
    int depth = 0;
    size_t start = 0;
    const char* p;
    if (!args) args = "";
    while (*args == ' ' || *args == '\t') args++;
    if (!args[0]) return 0;
    for (p = args; ; p++) {
        char c = *p;
        /* Commas / brackets inside string and char literals are not
         * separators; skip the literal body (escape-aware). */
        if (c == '"' || c == '\'') {
            char q = c;
            p++;
            while (*p) {
                if (*p == '\\' && p[1]) { p += 2; continue; }
                if (*p == q) break;
                p++;
            }
            if (!*p) p--; /* unterminated: let the NUL case flush */
            continue;
        }
        if (c == '(' || c == '[' || c == '{') depth++;
        else if (c == ')' || c == ']' || c == '}') depth--;
        else if ((c == ',' && depth == 0) || c == 0) {
            size_t len;
            const char* a;
            if (n >= cap) return n;
            a = args + start;
            len = (size_t)(p - (args + start));
            while (len && (a[0] == ' ' || a[0] == '\t')) { a++; len--; }
            while (len && (a[len - 1] == ' ' || a[len - 1] == '\t')) len--;
            if (len >= 128) len = 127;
            memcpy(out[n], a, len);
            out[n][len] = 0;
            n++;
            if (c == 0) break;
            start = (size_t)(p - args) + 1;
        }
    }
    return n;
}

/* Aggregates that cannot travel through (intptr_t) casts. */
static int shadow_ab_ty_by_value_agg(const char* ty) {
    if (!ty || !ty[0] || strchr(ty, '*')) return 0;
    if (strcmp(ty, "CCSlice") == 0 || strncmp(ty, "CCSlice_", 8) == 0 ||
        strcmp(ty, "CCSliceUnique") == 0 || strcmp(ty, "CCSliceShared") == 0 ||
        strcmp(ty, "CCSlicePacked") == 0)
        return 1;
    if (strcmp(ty, "CCError") == 0 || strcmp(ty, "CCIoError") == 0)
        return 1;
    if (strstr(ty, "struct ") || strstr(ty, "struct{")) return 1;
    return 0;
}

/* 1 = packable, 0 = known aggregate (leave in-thread), -1 = untyped
 * (would emit a guessed (intptr_t) cast). */
static int shadow_ab_arg_pack_kind(const char* callee, int argi,
                                   const char* arg) {
    const ShadowFnParam* fp;
    const ShadowBind* lb;
    const char* p;
    if (!arg) return -1;
    p = arg;
    while (*p == ' ' || *p == '\t') p++;
    if (!*p || *p == '&') return 1;
    if (*p >= '0' && *p <= '9') return 1;
    if (*p == '-' && p[1] >= '0' && p[1] <= '9') return 1;
    if (strcmp(p, "NULL") == 0) return 1;
    /* Literals carry their own pack type: a string literal is a pointer to
     * static storage (unpacked as the fnparam type, else `(const char*)`),
     * a char literal is an int value. Both travel. */
    if (*p == '"' || *p == '\'') return 1;
    fp = callee ? shadow_fnparam_lookup(callee, argi) : NULL;
    if (fp && fp->base[0]) {
        if (fp->stars > 0) return 1;
        if (shadow_ab_ty_by_value_agg(fp->base) || shadow_ty_is_structy(fp->base))
            return 0;
        return 1;
    }
    lb = shadow_bind_lookup(p);
    if (lb && lb->ty[0]) {
        if (strchr(lb->ty, '*')) return 1;
        if (shadow_ab_ty_by_value_agg(lb->ty) || shadow_ty_is_structy(lb->ty))
            return 0;
        return 1;
    }
    return -1;
}

static int shadow_ab_args_pack_kind(const char* callee, const char* args) {
    char argv[8][128];
    int argc;
    int i;
    int worst = 1;
    argc = shadow_ab_split_args(args, argv, 8);
    for (i = 0; i < argc; i++) {
        int k = shadow_ab_arg_pack_kind(callee, i, argv[i]);
        if (k < worst) worst = k;
    }
    return worst;
}

static void shadow_ab_pack_diag(ShadowCtx* ctx, AstNode* st, CEmit* out,
                                const char* fn) {
    char msg[256];
    snprintf(msg, sizeof(msg),
             "cannot autoblock '%s': argument cannot travel through (intptr_t)",
             fn ? fn : "?");
    shadow_variant_err_loc(ctx, st, out, fn, 0, msg);
    g_shadow_eh_diag = 1;
}

static unsigned shadow_call_site_attrs(AstNode* st) {
    if (!st || !st->e[0]) return 0;
    if (strcmp(st->e, "noblock") == 0) return SHADOW_FN_NOBLOCK;
    if (strcmp(st->e, "blocking") == 0) return SHADOW_FN_BLOCKING;
    return 0;
}

static ShadowCallMode shadow_call_edge_for(AstNode* st, ShadowCtx* ctx,
                                           const char* fn) {
    unsigned owner = ctx ? ctx->owner_fn_attrs : 0;
    unsigned site = st ? shadow_call_site_attrs(st) : 0;
    unsigned block = ctx ? ctx->block_attrs : 0;
    unsigned callee = shadow_fn_attr_lookup(fn);
    if (!ctx || !(owner & SHADOW_FN_ASYNC)) return SHADOW_CALL_NOBLOCK;
    if (shadow_autoblock_skip_callee(fn)) return SHADOW_CALL_NOBLOCK;
    return shadow_resolve_call_edge_mode(owner, callee, site, block);
}

static int shadow_call_should_autoblock(AstNode* st, ShadowCtx* ctx,
                                        const char* fn) {
    return shadow_call_edge_for(st, ctx, fn) == SHADOW_CALL_BLOCKING &&
           shadow_ab_is_user_fn(fn);
}

static int shadow_call_should_autoblock_args(AstNode* st, ShadowCtx* ctx,
                                             CEmit* out, const char* fn,
                                             const char* args) {
    if (!shadow_call_should_autoblock(st, ctx, fn)) return 0;
    {
        int kind = shadow_ab_args_pack_kind(fn, args);
        if (kind == 1) return 1;
        if (kind < 0) shadow_ab_pack_diag(ctx, st, out, fn);
        return 0;
    }
}

static int shadow_emit_ab_scalar_helper(CEmit* out, int id, const char* callee,
                                        const char* args) {
    char argv[8][128];
    int argc = shadow_ab_split_args(args, argv, 8);
    int i;
    if (argc > 0) {
        if (!cemit_fmt(out, "typedef struct { intptr_t __a0")) return 0;
        for (i = 1; i < argc; i++) {
            if (!cemit_fmt(out, "; intptr_t __a%d", i)) return 0;
        }
        if (!cemit_fmt(out,
                "; } __cc_ab_env_%d;\n"
                "static void* __cc_ab_entry_%d(void* __p) {\n"
                "    __cc_ab_env_%d* __e = (__cc_ab_env_%d*)__p;\n"
                "    return (void*)(intptr_t)%s(",
                id, id, id, id, callee))
            return 0;
        for (i = 0; i < argc; i++) {
            if (!cemit_fmt(out, "%s%s__e->__a%d", i ? ", " : "",
                           shadow_ab_arg_cast_fn(callee, i, argv[i]), i))
                return 0;
        }
        if (!cemit_str(out, ");\n}\n")) return 0;
        if (!cemit_fmt(out,
                "static void __cc_ab_drop_%d(void* __p) { free(__p); }\n"
                "static CCClosure0 __cc_ab_make_%d(",
                id, id))
            return 0;
        for (i = 0; i < argc; i++) {
            if (!cemit_fmt(out, "%sintptr_t __a%d", i ? ", " : "", i))
                return 0;
        }
        if (!cemit_fmt(out,
                ") {\n"
                "    __cc_ab_env_%d* __e = (__cc_ab_env_%d*)calloc(1, "
                "sizeof(*__e));\n",
                id, id))
            return 0;
        for (i = 0; i < argc; i++) {
            if (!cemit_fmt(out, "    if (__e) __e->__a%d = __a%d;\n", i, i))
                return 0;
        }
        return cemit_fmt(out,
            "    return cc_closure0_make(__cc_ab_entry_%d, __e, "
            "__cc_ab_drop_%d);\n}\n\n",
            id, id);
    }
    return cemit_fmt(out,
        "static void* __cc_ab_entry_%d(void* __p) {\n"
        "    (void)__p;\n"
        "    return (void*)(intptr_t)%s();\n"
        "}\n"
        "static CCClosure0 __cc_ab_make_%d(void) {\n"
        "    return cc_closure0_make(__cc_ab_entry_%d, NULL, NULL);\n"
        "}\n\n",
        id, callee, id, id);
}

static int shadow_emit_ab_void_helper(CEmit* out, int id, const char* callee,
                                      const char* args) {
    char argv[8][128];
    int argc = shadow_ab_split_args(args, argv, 8);
    int i;
    if (argc > 0) {
        if (!cemit_fmt(out, "typedef struct { intptr_t __a0")) return 0;
        for (i = 1; i < argc; i++) {
            if (!cemit_fmt(out, "; intptr_t __a%d", i)) return 0;
        }
        if (!cemit_fmt(out,
                "; } __cc_ab_env_%d;\n"
                "static void* __cc_ab_entry_%d(void* __p) {\n"
                "    __cc_ab_env_%d* __e = (__cc_ab_env_%d*)__p;\n"
                "    %s(",
                id, id, id, id, callee))
            return 0;
        for (i = 0; i < argc; i++) {
            if (!cemit_fmt(out, "%s%s__e->__a%d", i ? ", " : "",
                           shadow_ab_arg_cast_fn(callee, i, argv[i]), i))
                return 0;
        }
        if (!cemit_str(out, ");\n    return NULL;\n}\n")) return 0;
        if (!cemit_fmt(out,
                "static void __cc_ab_drop_%d(void* __p) { free(__p); }\n"
                "static CCClosure0 __cc_ab_make_%d(",
                id, id))
            return 0;
        for (i = 0; i < argc; i++) {
            if (!cemit_fmt(out, "%sintptr_t __a%d", i ? ", " : "", i))
                return 0;
        }
        if (!cemit_fmt(out,
                ") {\n"
                "    __cc_ab_env_%d* __e = (__cc_ab_env_%d*)calloc(1, "
                "sizeof(*__e));\n",
                id, id))
            return 0;
        for (i = 0; i < argc; i++) {
            if (!cemit_fmt(out, "    if (__e) __e->__a%d = __a%d;\n", i, i))
                return 0;
        }
        return cemit_fmt(out,
            "    return cc_closure0_make(__cc_ab_entry_%d, __e, "
            "__cc_ab_drop_%d);\n}\n\n",
            id, id);
    }
    return cemit_fmt(out,
        "static void* __cc_ab_entry_%d(void* __p) {\n"
        "    (void)__p;\n"
        "    %s();\n"
        "    return NULL;\n"
        "}\n"
        "static CCClosure0 __cc_ab_make_%d(void) {\n"
        "    return cc_closure0_make(__cc_ab_entry_%d, NULL, NULL);\n"
        "}\n\n",
        id, callee, id, id);
}

static int shadow_emit_ab_batch_helper(CEmit* out, const ShadowAbBatch* b) {
    int i, j;
    int id = b->id;
    int total_args = 0;
    char argv[SHADOW_AB_BATCH_MAX][8][128];
    int argc[SHADOW_AB_BATCH_MAX];
    if (!b || b->n <= 0) return 0;
    for (i = 0; i < b->n; i++)
        argc[i] = shadow_ab_split_args(b->items[i].args, argv[i], 8);
    if (b->tail_kind == 2)
        total_args = shadow_ab_split_args(b->tail_args, argv[b->n], 8);
    else if (b->tail_kind == 1)
        total_args = shadow_ab_split_args(b->tail_args, argv[b->n], 8);
    (void)total_args;

    if (!cemit_fmt(out, "typedef struct {")) return 0;
    {
        int slot = 0;
        for (i = 0; i < b->n; i++) {
            for (j = 0; j < argc[i]; j++) {
                if (!cemit_fmt(out, "%s intptr_t __a%d", slot ? ";" : "", slot))
                    return 0;
                slot++;
            }
        }
        if (b->tail_kind == 2) {
            if (!cemit_fmt(out, "%s intptr_t* __ylhs", slot ? ";" : ""))
                return 0;
            slot++;
            for (j = 0; j < shadow_ab_split_args(b->tail_args, argv[b->n], 8); j++) {
                if (!cemit_fmt(out, "; intptr_t __ta%d", j)) return 0;
            }
        } else if (b->tail_kind == 1) {
            for (j = 0; j < shadow_ab_split_args(b->tail_args, argv[b->n], 8);
                 j++) {
                if (!cemit_fmt(out, "%s intptr_t __ta%d", slot ? ";" : "", j))
                    return 0;
                slot++;
            }
        }
    }
    if (!cemit_fmt(out,
            "; } __cc_ab_env_%d;\n"
            "static void* __cc_ab_entry_%d(void* __p) {\n"
            "    __cc_ab_env_%d* __e = (__cc_ab_env_%d*)__p;\n",
            id, id, id, id))
        return 0;
    {
        int slot = 0;
        for (i = 0; i < b->n; i++) {
            if (!cemit_fmt(out, "    %s(", b->items[i].callee)) return 0;
            for (j = 0; j < argc[i]; j++) {
                if (!cemit_fmt(out, "%s%s__e->__a%d", j ? ", " : "",
                               shadow_ab_arg_cast_fn(b->items[i].callee, j,
                                                     argv[i][j]), slot))
                    return 0;
                slot++;
            }
            if (!cemit_str(out, ");\n")) return 0;
        }
        if (b->tail_kind == 1) {
            if (!cemit_fmt(out, "    return (void*)(intptr_t)%s(",
                           b->tail_callee))
                return 0;
            for (j = 0; j < shadow_ab_split_args(b->tail_args, argv[b->n], 8);
                 j++) {
                if (!cemit_fmt(out, "%s%s__e->__ta%d", j ? ", " : "",
                               shadow_ab_arg_cast_fn(b->tail_callee, j,
                                                     argv[b->n][j]), j))
                    return 0;
            }
            if (!cemit_str(out, ");\n")) return 0;
        } else if (b->tail_kind == 2) {
            if (!cemit_fmt(out, "    *(__e->__ylhs) = %s(", b->tail_callee))
                return 0;
            for (j = 0; j < shadow_ab_split_args(b->tail_args, argv[b->n], 8);
                 j++) {
                if (!cemit_fmt(out, "%s%s__e->__ta%d", j ? ", " : "",
                               shadow_ab_arg_cast_fn(b->tail_callee, j,
                                                     argv[b->n][j]), j))
                    return 0;
            }
            if (!cemit_str(out, ");\n    return NULL;\n")) return 0;
        } else if (!cemit_str(out, "    return NULL;\n")) {
            return 0;
        }
    }
    if (!cemit_fmt(out, "}\n"
            "static void __cc_ab_drop_%d(void* __p) { free(__p); }\n"
            "static CCClosure0 __cc_ab_make_%d(",
            id, id))
        return 0;
    {
        int slot = 0;
        int first = 1;
        for (i = 0; i < b->n; i++) {
            for (j = 0; j < argc[i]; j++) {
                if (!cemit_fmt(out, "%sintptr_t __a%d", first ? "" : ", ",
                               slot))
                    return 0;
                first = 0;
                slot++;
            }
        }
        if (b->tail_kind == 2) {
            if (!cemit_fmt(out, "%sintptr_t __ylhs", first ? "" : ", "))
                return 0;
            first = 0;
            for (j = 0; j < shadow_ab_split_args(b->tail_args, argv[b->n], 8);
                 j++) {
                if (!cemit_fmt(out, ", intptr_t __ta%d", j)) return 0;
            }
        } else if (b->tail_kind == 1) {
            for (j = 0; j < shadow_ab_split_args(b->tail_args, argv[b->n], 8);
                 j++) {
                if (!cemit_fmt(out, "%sintptr_t __ta%d", first ? "" : ", ", j))
                    return 0;
                first = 0;
            }
        }
    }
    if (!cemit_fmt(out,
            ") {\n"
            "    __cc_ab_env_%d* __e = (__cc_ab_env_%d*)calloc(1, "
            "sizeof(*__e));\n",
            id, id))
        return 0;
    {
        int slot = 0;
        for (i = 0; i < b->n; i++) {
            for (j = 0; j < argc[i]; j++) {
                if (!cemit_fmt(out, "    if (__e) __e->__a%d = __a%d;\n",
                               slot, slot))
                    return 0;
                slot++;
            }
        }
        if (b->tail_kind == 2) {
            if (!cemit_fmt(out, "    if (__e) __e->__ylhs = (intptr_t*)__ylhs;\n"))
                return 0;
            for (j = 0; j < shadow_ab_split_args(b->tail_args, argv[b->n], 8);
                 j++) {
                if (!cemit_fmt(out, "    if (__e) __e->__ta%d = __ta%d;\n", j, j))
                    return 0;
            }
        } else if (b->tail_kind == 1) {
            for (j = 0; j < shadow_ab_split_args(b->tail_args, argv[b->n], 8);
                 j++) {
                if (!cemit_fmt(out, "    if (__e) __e->__ta%d = __ta%d;\n", j, j))
                    return 0;
            }
        }
    }
    return cemit_fmt(out,
        "    return cc_closure0_make(__cc_ab_entry_%d, __e, "
        "__cc_ab_drop_%d);\n}\n\n",
        id, id);
}

static int shadow_emit_ab_struct_helper(CEmit* out, int id, const char* callee,
                                        const char* args, const char* ret_ty) {
    char argv[8][128];
    int argc = shadow_ab_split_args(args, argv, 8);
    int i;
    if (!cemit_fmt(out, "typedef struct { %s* __ret", ret_ty ? ret_ty : "void"))
        return 0;
    for (i = 0; i < argc; i++) {
        if (!cemit_fmt(out, "; intptr_t __a%d", i)) return 0;
    }
    if (!cemit_fmt(out,
            "; } __cc_ab_env_%d;\n"
            "static void* __cc_ab_entry_%d(void* __p) {\n"
            "    __cc_ab_env_%d* __e = (__cc_ab_env_%d*)__p;\n"
            "    *(__e->__ret) = %s(",
            id, id, id, id, callee))
        return 0;
    for (i = 0; i < argc; i++) {
        if (!cemit_fmt(out, "%s%s__e->__a%d", i ? ", " : "",
                       shadow_ab_arg_cast_fn(callee, i, argv[i]), i))
            return 0;
    }
    if (!cemit_str(out, ");\n    return NULL;\n}\n")) return 0;
    if (!cemit_fmt(out,
            "static void __cc_ab_drop_%d(void* __p) { free(__p); }\n"
            "static CCClosure0 __cc_ab_make_%d(%s* __ret",
            id, id, ret_ty ? ret_ty : "void"))
        return 0;
    for (i = 0; i < argc; i++) {
        if (!cemit_fmt(out, ", intptr_t __a%d", i)) return 0;
    }
    if (!cemit_fmt(out,
            ") {\n"
            "    __cc_ab_env_%d* __e = (__cc_ab_env_%d*)calloc(1, "
            "sizeof(*__e));\n"
            "    if (__e) __e->__ret = __ret;\n",
            id, id))
        return 0;
    for (i = 0; i < argc; i++) {
        if (!cemit_fmt(out, "    if (__e) __e->__a%d = __a%d;\n", i, i))
            return 0;
    }
    return cemit_fmt(out,
        "    return cc_closure0_make(__cc_ab_entry_%d, __e, "
        "__cc_ab_drop_%d);\n}\n\n",
        id, id);
}

static int shadow_emit_ab_site(CEmit* out, const char* indent, int id,
                               const char* args) {
    char argv[8][128];
    int argc = shadow_ab_split_args(args, argv, 8);
    int i;
    /* Match production shape: `t = (cc_run_blocking_task_intptr(...));`
     * then block so body-helper async still runs the work synchronously. */
    if (!cemit_fmt(out,
                   "%s{ CCTask __cc_ab_t = (cc_run_blocking_task_intptr(",
                   indent))
        return 0;
    if (argc > 0) {
        if (!cemit_fmt(out, "__cc_ab_make_%d(", id)) return 0;
        for (i = 0; i < argc; i++) {
            if (!cemit_fmt(out, "%s(intptr_t)(%s)", i ? ", " : "", argv[i]))
                return 0;
        }
        return cemit_fmt(out,
                         "))); (void)cc_block_on_intptr(__cc_ab_t); }\n");
    }
    return cemit_fmt(out,
                     "__cc_ab_make_%d())); (void)cc_block_on_intptr(__cc_ab_t); "
                     "}\n",
                     id);
}

static int shadow_emit_ab_batch_site(CEmit* out, const char* indent,
                                     const ShadowAbBatch* b) {
    char argv[SHADOW_AB_BATCH_MAX][8][128];
    int argc[SHADOW_AB_BATCH_MAX + 1];
    int i, j, slot = 0;
    if (!b) return 0;
    for (i = 0; i < b->n; i++)
        argc[i] = shadow_ab_split_args(b->items[i].args, argv[i], 8);
    if (b->tail_kind)
        argc[b->n] = shadow_ab_split_args(b->tail_args, argv[b->n], 8);
    if (b->tail_kind == 1) {
        if (!cemit_fmt(out, "%sreturn (int)(intptr_t)cc_run_blocking_closure0_ptr(",
                       indent))
            return 0;
    } else if (b->tail_kind == 2) {
        if (!cemit_fmt(out, "%s(void)cc_run_blocking_closure0(", indent))
            return 0;
    } else if (!cemit_fmt(out, "%s(void)cc_run_blocking_closure0(", indent)) {
        return 0;
    }
    if (!cemit_fmt(out, "__cc_ab_make_%d(", b->id)) return 0;
    for (i = 0; i < b->n; i++) {
        for (j = 0; j < argc[i]; j++) {
            if (!cemit_fmt(out, "%s(intptr_t)(%s)", slot ? ", " : "", argv[i][j]))
                return 0;
            slot++;
        }
    }
    if (b->tail_kind == 2) {
        if (!cemit_fmt(out, "%s(intptr_t)(&%s)", slot ? ", " : "", b->tail_lhs))
            return 0;
        for (j = 0; j < argc[b->n]; j++) {
            if (!cemit_fmt(out, ", (intptr_t)(%s)", argv[b->n][j])) return 0;
        }
    } else if (b->tail_kind == 1) {
        for (j = 0; j < argc[b->n]; j++) {
            if (!cemit_fmt(out, "%s(intptr_t)(%s)", slot ? ", " : "",
                           argv[b->n][j]))
                return 0;
        }
    }
    return cemit_fmt(out, "));\n");
}

static int shadow_ab_fill_item(AstNode* st, ShadowCtx* ctx, CEmit* out,
                               ShadowAbItem* it) {
    if (!st || !it) return 0;
    it->st = st;
    snprintf(it->callee, sizeof(it->callee), "%s", st->a);
    shadow_emit_expr_text(st, st->b, it->args, sizeof(it->args), NULL);
    return shadow_call_should_autoblock_args(st, ctx, out, st->a, it->args);
}

static int shadow_ab_try_tail(AstNode* st, ShadowCtx* ctx, CEmit* out,
                              ShadowAbBatch* b) {
    char fn[64], args[288];
    if (!st || !b) return 0;
    if (st->kind == AST_RETURN_CC && st->b[0]) {
        if (!shadow_parse_call_expr(st->b, fn, sizeof(fn), args, sizeof(args)))
            return 0;
        if (!shadow_call_should_autoblock_args(st, ctx, out, fn, args))
            return 0;
        b->tail_kind = 1;
        snprintf(b->tail_callee, sizeof(b->tail_callee), "%s", fn);
        snprintf(b->tail_args, sizeof(b->tail_args), "%s", args);
        snprintf(st->d, sizeof(st->d), "ab:skip");
        return 1;
    }
    if (st->kind == AST_RETURN_EXPR && st->a[0] &&
        (!st->e[0] || strcmp(st->e, "bang") != 0)) {
        if (!shadow_parse_call_expr(st->a, fn, sizeof(fn), args, sizeof(args)))
            return 0;
        if (!shadow_call_should_autoblock_args(st, ctx, out, fn, args))
            return 0;
        b->tail_kind = 1;
        snprintf(b->tail_callee, sizeof(b->tail_callee), "%s", fn);
        snprintf(b->tail_args, sizeof(b->tail_args), "%s", args);
        snprintf(st->d, sizeof(st->d), "ab:skip");
        return 1;
    }
    if (st->kind == AST_ASSIGN && st->b[0] &&
        (!st->c[0] || strcmp(st->c, "=") == 0)) {
        if (!shadow_parse_call_expr(st->b, fn, sizeof(fn), args, sizeof(args)))
            return 0;
        if (!shadow_call_should_autoblock_args(st, ctx, out, fn, args))
            return 0;
        b->tail_kind = 2;
        snprintf(b->tail_lhs, sizeof(b->tail_lhs), "%s", st->a);
        snprintf(b->tail_callee, sizeof(b->tail_callee), "%s", fn);
        snprintf(b->tail_args, sizeof(b->tail_args), "%s", args);
        snprintf(st->d, sizeof(st->d), "ab:skip");
        return 1;
    }
    return 0;
}

static void shadow_prescan_autoblock_body(AstNode** body, int nbody,
                                          ShadowCtx* ctx, CEmit* out) {
    int i = 0;
    while (i < nbody) {
        AstNode* st = body[i];
        if (!st) {
            i++;
            continue;
        }
        if (st->kind == AST_BLOCK) {
            unsigned saved = ctx ? ctx->block_attrs : 0;
            if (strcmp(st->e, "noblock") == 0)
                ctx->block_attrs |= SHADOW_FN_NOBLOCK;
            else if (strcmp(st->e, "blocking") == 0)
                ctx->block_attrs |= SHADOW_FN_BLOCKING;
            shadow_prescan_autoblock_body(st->body, st->nbody, ctx, out);
            if (ctx) ctx->block_attrs = saved;
            i++;
            continue;
        }
        if (st->kind == AST_DEFER || st->kind == AST_CANCEL_DEFER) {
            i++;
            continue;
        }
        if (st->kind == AST_CALL_ARGS && !st->d[0]) {
            ShadowAbBatch batch = {0};
            int j = i;
            batch.id = g_shadow_ab_id;
            if (batch.id >= SHADOW_AB_BATCH_CAP) {
                shadow_table_full("autoblock_batches", SHADOW_AB_BATCH_CAP, NULL);
                i++;
                continue;
            }
            while (j < nbody && batch.n < SHADOW_AB_BATCH_MAX) {
                ShadowAbItem it = {0};
                if (!shadow_ab_fill_item(body[j], ctx, out, &it)) break;
                batch.items[batch.n++] = it;
                j++;
            }
            if (batch.n >= 2 ||
                (batch.n == 1 && j < nbody &&
                 shadow_ab_try_tail(body[j], ctx, out, &batch))) {
                if (shadow_emit_ab_batch_helper(out, &batch)) {
                    shadow_ab_batch_save(&batch);
                    snprintf(batch.items[0].st->d,
                             sizeof(batch.items[0].st->d), "ab:b:%d", batch.id);
                    for (int k = 1; k < batch.n; k++)
                        snprintf(batch.items[k].st->d,
                                 sizeof(batch.items[k].st->d), "ab:skip");
                    g_shadow_ab_id++;
                    i = j + (batch.tail_kind ? 1 : 0);
                    continue;
                }
            }
            if (batch.n == 1) {
                if (shadow_emit_ab_void_helper(out, g_shadow_ab_id,
                                               batch.items[0].callee,
                                               batch.items[0].args)) {
                    snprintf(st->d, sizeof(st->d), "ab:%d", g_shadow_ab_id++);
                }
                i++;
                continue;
            }
        }
        if (st->kind == AST_TYPED_INIT && !shadow_ab_tag_get(st)) {
            char fn[64], args[288];
            char tag[16];
            if (shadow_parse_call_expr(st->c, fn, sizeof(fn), args,
                                       sizeof(args)) &&
                shadow_call_should_autoblock_args(st, ctx, out, fn, args)) {
                int id = g_shadow_ab_id++;
                if (shadow_ty_is_structy(st->a)) {
                    if (shadow_emit_ab_struct_helper(out, id, fn, args, st->a)) {
                        snprintf(tag, sizeof(tag), "di:%d", id);
                        shadow_ab_tag_set(st, tag);
                    }
                } else if (shadow_emit_ab_scalar_helper(out, id, fn, args)) {
                    snprintf(tag, sizeof(tag), "di:%d", id);
                    shadow_ab_tag_set(st, tag);
                }
            }
        } else if (st->kind == AST_ASSIGN && !shadow_ab_tag_get(st) &&
                   st->b[0] && (!st->c[0] || strcmp(st->c, "=") == 0)) {
            char fn[64], args[288];
            char tag[16];
            const ShadowBind* lb;
            if (shadow_parse_call_expr(st->b, fn, sizeof(fn), args,
                                       sizeof(args)) &&
                shadow_call_should_autoblock_args(st, ctx, out, fn, args)) {
                int id = g_shadow_ab_id++;
                lb = shadow_bind_lookup(st->a);
                if (lb && shadow_ty_is_structy(lb->ty)) {
                    if (shadow_emit_ab_struct_helper(out, id, fn, args, lb->ty)) {
                        snprintf(tag, sizeof(tag), "as:%d", id);
                        shadow_ab_tag_set(st, tag);
                    }
                } else if (shadow_emit_ab_scalar_helper(out, id, fn, args)) {
                    snprintf(tag, sizeof(tag), "as:%d", id);
                    shadow_ab_tag_set(st, tag);
                }
            }
        }
        if (st->nbody > 0)
            shadow_prescan_autoblock_body(st->body, st->nbody, ctx, out);
        if (st->nkids > 0)
            shadow_prescan_autoblock_body(st->kids, st->nkids, ctx, out);
        i++;
    }
}

static void __attribute__((unused)) shadow_prescan_autoblock_stmt(
    AstNode* st, ShadowCtx* ctx, CEmit* out) {
    if (!st) return;
    if (st->kind == AST_DEFER || st->kind == AST_CANCEL_DEFER) return;
    if (st->kind == AST_BLOCK) {
        shadow_prescan_autoblock_body(&st, 1, ctx, out);
        return;
    }
    if (st->nbody > 0) {
        shadow_prescan_autoblock_body(st->body, st->nbody, ctx, out);
        return;
    }
    shadow_prescan_autoblock_body(&st, 1, ctx, out);
}

static int shadow_emit_autoblock_prescan(AstNode** kids, int nkids,
                                         ShadowCtx* ctx, CEmit* out) {
    g_shadow_nab_tags = 0;
    shadow_prescan_autoblock_body(kids, nkids, ctx, out);
    return !out->err;
}

static int shadow_emit_call_maybe_autoblock(AstNode* st, CEmit* out,
                                            ShadowCtx* ctx, const char* indent,
                                            const char* call, const char* fn,
                                            const char* args) {
    int ab_id = -1;
    if (ctx && ctx->defer_emit)
        return cemit_fmt(out, "%s%s;\n", indent, call);
    if (st->d[0] && strcmp(st->d, "ab:skip") == 0)
        return 1;
    if (st->d[0] && strncmp(st->d, "ab:b:", 5) == 0) {
        const ShadowAbBatch* batch = shadow_ab_batch_get(atoi(st->d + 5));
        if (batch)
            return shadow_emit_ab_batch_site(out, indent, batch);
    }
    if (st->d[0] && strncmp(st->d, "ab:", 3) == 0)
        ab_id = atoi(st->d + 3);
    if (ab_id < 0 ||
        shadow_call_edge_for(st, ctx, fn) != SHADOW_CALL_BLOCKING) {
        return cemit_fmt(out, "%s%s;\n", indent, call);
    }
    return shadow_emit_ab_site(out, indent, ab_id, args);
}

static int shadow_emit_decl_init_maybe_autoblock(AstNode* st, CEmit* out,
                                                 ShadowCtx* ctx,
                                                 const char* indent,
                                                 const char* ty) {
    char fn[64], args[288];
    char argv[8][128];
    const char* tag;
    int id, argc;
    (void)ctx;
    if (!st) return 0;
    tag = shadow_ab_tag_get(st);
    if (!tag || strncmp(tag, "di:", 3) != 0) return 0;
    id = atoi(tag + 3);
    if (!shadow_parse_call_expr(st->c, fn, sizeof(fn), args, sizeof(args)))
        return 0;
    argc = shadow_ab_split_args(args, argv, 8);
    if (shadow_ty_is_structy(ty)) {
        /* Decl-init is always a block-item (never an unbraced if/for
         * then-body), so leave the introduced binding at this scope. */
        if (!cemit_fmt(out, "%s%s __cc_ab_ret_%d;\n", indent, ty, id)) return 0;
        if (!cemit_fmt(out, "%s(void)cc_run_blocking_closure0(", indent))
            return 0;
        if (!cemit_fmt(out, "__cc_ab_make_%d(&__cc_ab_ret_%d", id, id))
            return 0;
        for (int i = 0; i < argc; i++) {
            if (!cemit_fmt(out, ", (intptr_t)(%s)", argv[i])) return 0;
        }
        if (!cemit_fmt(out, "));\n")) return 0;
        return cemit_fmt(out, "%s%s %s = __cc_ab_ret_%d;\n", indent, ty, st->b,
                         id);
    }
    if (!cemit_fmt(out, "%s%s %s = ", indent, ty, st->b)) return 0;
    if (!cemit_str(out, "(intptr_t)cc_run_blocking_closure0_ptr(")) return 0;
    if (!cemit_fmt(out, "__cc_ab_make_%d(", id)) return 0;
    for (int i = 0; i < argc; i++) {
        if (!cemit_fmt(out, "%s(intptr_t)(%s)", i ? ", " : "", argv[i]))
            return 0;
    }
    return cemit_fmt(out, "));\n");
}

static int shadow_emit_assign_maybe_autoblock(AstNode* st, CEmit* out,
                                              ShadowCtx* ctx,
                                              const char* indent) {
    char fn[64], args[288];
    char argv[8][128];
    const ShadowBind* lb;
    const char* tag;
    int id, argc;
    (void)ctx;
    if (!st) return 0;
    if (st->d[0] && strcmp(st->d, "ab:skip") == 0) return 1;
    tag = shadow_ab_tag_get(st);
    if (!tag || strncmp(tag, "as:", 3) != 0) return 0;
    id = atoi(tag + 3);
    if (!shadow_parse_call_expr(st->b, fn, sizeof(fn), args, sizeof(args)))
        return 0;
    argc = shadow_ab_split_args(args, argv, 8);
    lb = shadow_bind_lookup(st->a);
    if (lb && shadow_ty_is_structy(lb->ty)) {
        /* Brace-wrap: multi-stmt expansion must be one statement for
         * unbraced if/for/while then-bodies (C forbids a bare decl there). */
        if (!cemit_fmt(out, "%s{\n", indent)) return 0;
        if (!cemit_fmt(out, "%s    %s __cc_ab_ret_%d;\n", indent, lb->ty, id))
            return 0;
        if (!cemit_fmt(out, "%s    (void)cc_run_blocking_closure0(", indent))
            return 0;
        if (!cemit_fmt(out, "__cc_ab_make_%d(&__cc_ab_ret_%d", id, id))
            return 0;
        for (int i = 0; i < argc; i++) {
            if (!cemit_fmt(out, ", (intptr_t)(%s)", argv[i])) return 0;
        }
        if (!cemit_fmt(out, "));\n")) return 0;
        if (!cemit_fmt(out, "%s    %s = __cc_ab_ret_%d;\n", indent, st->a, id))
            return 0;
        return cemit_fmt(out, "%s}\n", indent);
    }
    if (!cemit_fmt(out, "%s%s = (intptr_t)cc_run_blocking_closure0_ptr(",
                   indent, st->a))
        return 0;
    if (!cemit_fmt(out, "__cc_ab_make_%d(", id)) return 0;
    for (int i = 0; i < argc; i++) {
        if (!cemit_fmt(out, "%s(intptr_t)(%s)", i ? ", " : "", argv[i]))
            return 0;
    }
    return cemit_fmt(out, "));\n");
}
