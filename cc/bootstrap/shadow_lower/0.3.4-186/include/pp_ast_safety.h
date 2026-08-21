/* Post-parse safety/diag beachhead for the SERDES front.
 * Slice move, unique provenance, arena borrow free/reset/spawn-pin,
 * unwrap diverge / bare unhandled, closure capture mutation,
 * stack-slice escape, homemade last-drop, unknown map keys.
 * Requires pp_ast_core.cch (AstNode / TapeCache). */
#pragma once

enum {
    SHADOW_SAFE_PIN_CAP = 24,
    SHADOW_SAFE_EH_CAP = 16,
    SHADOW_SAFE_VARIANT_CAP = 32,
    SHADOW_SAFE_VARM_CAP = 16,
    SHADOW_SAFE_TYPE_CAP = 128,
    SHADOW_SAFE_DOM_CAP = 16,
    SHADOW_SAFE_CHAN_TD_CAP = 32,
    SHADOW_SAFE_GLOBAL_CAP = 64,
    SHADOW_SAFE_MAPKEY_CAP = 64
};

typedef struct {
    char name[64];
    char ty[64];    /* declared type spelling (aggregate send bans) */
    int is_slice;
    int is_arena;
    int is_arena_ptr;
    int is_ptr;     /* any pointer local */
    int is_variant;     /* local typed as a registered @variant value */
    int is_variant_ptr; /* pointer to a registered @variant */
    int unproven_ptr; /* pointer init with no proven provenance */
    int moved;
    int unique;     /* move-only (unique bit / [:!] / CCSlice!) */
    int is_static_slice; /* cc_slice_from_static — channel-stable */
    int is_stack_array; /* local `T name[N]` — stack storage */
    int is_stack_slice_view; /* cc_slice_from_buffer/parts over stack array */
    int is_scratch_string; /* CCString / view from @scratch */
    int arena_view; /* non-owning slice into arena epoch */
    int nonstable_slice; /* aggregate init copied an arena-view slice */
    int is_chan;    /* 1 = tx (>), 2 = rx (<) */
    char arena[64]; /* owning arena ident (view / ptr) */
    char alias_of[64]; /* if ptr init from &local */
} ShadowSafeVar;

typedef struct {
    char name[64];
    char arms[SHADOW_SAFE_VARM_CAP][32];
    int narm;
    int is_schema; /* grammar `one of` — production diagnostic wording */
    int is_packed; /* @variant(packed) — different raw .u wording */
} ShadowSafeVariant;

/* Syntactic domination (spec §11.5): projection of root.arm is legal while
 * this frame is active (if-guard then-arm or switch case region). */
typedef struct {
    char root[64];
    char arm[32];
} ShadowSafeDom;

enum {
    SHADOW_SAFE_STRUCT_CAP = 64,
    SHADOW_SAFE_FIELD_CAP = 24,
    SHADOW_SAFE_AS_SEEN_CAP = 32
};

typedef struct {
    char name[64]; /* typedef alias */
    char fty[SHADOW_SAFE_FIELD_CAP][64];
    char fname[SHADOW_SAFE_FIELD_CAP][64];
    int is_as[SHADOW_SAFE_FIELD_CAP];
    int nf;
} ShadowSafeStruct;

typedef struct {
    char arena[64];
    char nursery[64]; /* spawn receiver; pin clears when that nursery leaves scope */
} ShadowSafePin;

typedef struct {
    char name[64];
    char err[64];
} ShadowSafeRfn;

typedef struct {
    /* Locals grow on safe_ar (check-frame arena). Each var is arena-owned
     * so find() pointers stay valid across later adds. No fixed CAP. */
    ShadowSafeVar** vars;
    int nvars;
    int var_cap;
    ShadowSafePin pinned[SHADOW_SAFE_PIN_CAP];
    int npin;
    AstNode* eh_stack[SHADOW_SAFE_EH_CAP];
    int eh_scopes[SHADOW_SAFE_EH_CAP];
    int neh;
    int eh_scope;
    /* Result-fn roster grows on safe_ar (check-frame arena). No fixed CAP. */
    ShadowSafeRfn* rfns;
    int nrfn;
    int rfn_cap;
    CCArena* safe_ar;
    int strict_unhandled; /* CC_STRICT_RESULT_UNWRAP (default on) */
    int in_async;         /* walking an @async fn body */
    int in_send_task;     /* send_task[_hybrid] closure packs Result payload */
    AstNode* cur_fn;      /* enclosing fn while walking body (last-drop) */
    TapeCache* cache;
    int err;
    ShadowSafeStruct* structs; /* file-scope table owned by check */
    int nstructs;
    ShadowSafeVariant* variants; /* file-scope table owned by check */
    int nvariants;
    char type_names[SHADOW_SAFE_TYPE_CAP][64]; /* typedef / variant / primitives */
    int ntypes;
    /* `typedef long long i64;` → resolve i64 to long long for POD checks. */
    char td_alias[SHADOW_SAFE_TYPE_CAP][64];
    char td_base[SHADOW_SAFE_TYPE_CAP][64];
    int ntd;
    /* Typedef channel aliases: `typedef int[~N >] Tx;` → Tx is role 1. */
    char chan_td_name[SHADOW_SAFE_CHAN_TD_CAP][64];
    int chan_td_role[SHADOW_SAFE_CHAN_TD_CAP]; /* 1=tx 2=rx */
    int nchan_td;
    char globals[SHADOW_SAFE_GLOBAL_CAP][64]; /* file-scope names */
    int nglobals;
    char map_keys[SHADOW_SAFE_MAPKEY_CAP][64]; /* installed cc_map_key_hash_* */
    int nmap_keys;
    ShadowSafeDom dom[SHADOW_SAFE_DOM_CAP];
    int ndom;
    /* Unique slice value-captures deferred until after closure body walk. */
    char pending_moves[16][64];
    int npending_moves;
} ShadowSafeCtx;

static int shadow_safe_chan_td_role(ShadowSafeCtx* ctx, const char* ty) {
    int i;
    if (!ctx || !ty || !ty[0]) return 0;
    for (i = 0; i < ctx->nchan_td; i++) {
        if (strcmp(ctx->chan_td_name[i], ty) == 0) return ctx->chan_td_role[i];
    }
    return 0;
}

static void shadow_safe_add_chan_td(ShadowSafeCtx* ctx, const char* alias,
                                    int role) {
    if (!ctx || !alias || !alias[0] || (role != 1 && role != 2)) return;
    if (shadow_safe_chan_td_role(ctx, alias)) return;
    if (ctx->nchan_td >= SHADOW_SAFE_CHAN_TD_CAP) return;
    snprintf(ctx->chan_td_name[ctx->nchan_td],
             sizeof(ctx->chan_td_name[0]), "%s", alias);
    ctx->chan_td_role[ctx->nchan_td] = role;
    ctx->nchan_td++;
}

static void shadow_safe_err_at_col(ShadowSafeCtx* ctx, AstNode* st, int force_col,
                                   const char* msg) {
    FileTape* ft;
    int line = 1, col = 1;
    if (!ctx || !msg) return;
    /* Sticky: keep the first (most specific) diagnostic. */
    if (ctx->err) return;
    ctx->err = 1;
    if (!st || !ctx->cache || !st->file_id) {
        fprintf(stderr, "error: %s\n", msg);
        return;
    }
    ft = tape_by_id(ctx->cache, st->file_id);
    if (!ft || !ft->bytes) {
        fprintf(stderr, "error: %s\n", msg);
        return;
    }
    offset_to_linecol(ft, st->tok_off, &line, &col);
    tape_logical_at(ft, st->tok_off, NULL, 0, &line);
    if (force_col > 0) col = force_col;
    fprintf(stderr, "%s:%d:%d: error: %s\n", ft->path, line, col, msg);
}

static void shadow_safe_err_at(ShadowSafeCtx* ctx, AstNode* st, const char* msg) {
    shadow_safe_err_at_col(ctx, st, 0, msg);
}

static void shadow_safe_check_async_chan_text(ShadowSafeCtx* ctx, AstNode* st,
                                             const char* text);
static void shadow_safe_on_chan_send_text(ShadowSafeCtx* ctx, AstNode* st,
                                         const char* text);
static ShadowSafeStruct* shadow_safe_find_struct(ShadowSafeCtx* ctx,
                                                const char* name);
static ShadowSafeVariant* shadow_safe_find_variant(ShadowSafeCtx* ctx,
                                                  const char* name);
static void shadow_safe_strip_ty(const char* in, char* out, size_t cap);

static int shadow_safe_ident_eq(const char* a, const char* b) {
    return a && b && a[0] && strcmp(a, b) == 0;
}

static int shadow_safe_var_reserve(ShadowSafeCtx* ctx, int need) {
    ShadowSafeVar** nbuf;
    int ncap;
    if (!ctx || !ctx->safe_ar) return 0;
    if (need <= ctx->var_cap) return 1;
    ncap = ctx->var_cap ? ctx->var_cap * 2 : 32;
    while (ncap < need) ncap *= 2;
    nbuf = (ShadowSafeVar**)cc_arena_alloc(ctx->safe_ar,
                                           (size_t)ncap * sizeof(ShadowSafeVar*),
                                           _Alignof(ShadowSafeVar*));
    if (!nbuf) return 0;
    if (ctx->vars && ctx->nvars > 0)
        memcpy(nbuf, ctx->vars, (size_t)ctx->nvars * sizeof(ShadowSafeVar*));
    ctx->vars = nbuf;
    ctx->var_cap = ncap;
    return 1;
}

static ShadowSafeVar* shadow_safe_find(ShadowSafeCtx* ctx, const char* name) {
    int i;
    if (!ctx || !name || !name[0] || !ctx->vars) return NULL;
    for (i = ctx->nvars - 1; i >= 0; i--) {
        if (ctx->vars[i] && strcmp(ctx->vars[i]->name, name) == 0)
            return ctx->vars[i];
    }
    return NULL;
}

static ShadowSafeVar* shadow_safe_add(ShadowSafeCtx* ctx, const char* name) {
    ShadowSafeVar* v;
    if (!ctx || !name || !name[0]) return NULL;
    if (!shadow_safe_var_reserve(ctx, ctx->nvars + 1)) {
        shadow_safe_err_at(ctx, NULL, "safety var table grow failed");
        return NULL;
    }
    v = (ShadowSafeVar*)cc_arena_alloc(ctx->safe_ar, sizeof(ShadowSafeVar),
                                       _Alignof(ShadowSafeVar));
    if (!v) {
        shadow_safe_err_at(ctx, NULL, "safety var table grow failed");
        return NULL;
    }
    memset(v, 0, sizeof(*v));
    snprintf(v->name, sizeof(v->name), "%s", name);
    ctx->vars[ctx->nvars++] = v;
    return v;
}

static int shadow_safe_is_word(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_';
}

/* True if `text` contains ident as a whole word. */
static int shadow_safe_text_uses(const char* text, const char* ident) {
    const char* p;
    size_t n;
    if (!text || !ident || !ident[0]) return 0;
    n = strlen(ident);
    p = text;
    while ((p = strstr(p, ident)) != NULL) {
        char before = (p > text) ? p[-1] : 0;
        char after = p[n];
        if (!shadow_safe_is_word(before) && !shadow_safe_is_word(after))
            return 1;
        p += n;
    }
    return 0;
}

/* Skip uses inside cc_move(ident) — the move site itself is not UAF. */
static int shadow_safe_text_uses_outside_move(const char* text,
                                             const char* ident) {
    const char* p;
    size_t n;
    if (!text || !ident || !ident[0]) return 0;
    n = strlen(ident);
    p = text;
    while ((p = strstr(p, ident)) != NULL) {
        char before = (p > text) ? p[-1] : 0;
        char after = p[n];
        if (!shadow_safe_is_word(before) && !shadow_safe_is_word(after)) {
            const char* q = p;
            while (q > text && (q[-1] == ' ' || q[-1] == '\t')) q--;
            if (q > text && q[-1] == '(') {
                const char* m = q - 1;
                const char* key = "cc_move";
                size_t kn = 7;
                while (m > text && (m[-1] == ' ' || m[-1] == '\t')) m--;
                if ((size_t)(m - text) >= kn &&
                    memcmp(m - kn, key, kn) == 0 &&
                    (m - kn == text || !shadow_safe_is_word(m[-kn - 1]))) {
                    p += n;
                    continue;
                }
            }
            return 1;
        }
        p += n;
    }
    return 0;
}

static int shadow_safe_plain_ident(const char* text, char* out, size_t cap) {
    size_t i = 0, n;
    if (!text || !out || cap < 2) return 0;
    while (text[i] == ' ' || text[i] == '\t') i++;
    if (!shadow_safe_is_word(text[i]) || (text[i] >= '0' && text[i] <= '9'))
        return 0;
    n = 0;
    while (shadow_safe_is_word(text[i])) {
        if (n + 1 >= cap) return 0;
        out[n++] = text[i++];
    }
    while (text[i] == ' ' || text[i] == '\t') i++;
    if (text[i] != 0) return 0;
    out[n] = 0;
    return n > 0;
}

static int shadow_safe_amp_ident(const char* text, char* out, size_t cap) {
    size_t i = 0;
    size_t n = 0;
    if (!text || !out || cap < 2) return 0;
    while (text[i] == ' ' || text[i] == '\t') i++;
    if (text[i] == '&') {
        i++;
        while (text[i] == ' ' || text[i] == '\t') i++;
    }
    /* Bare ident, or `obj.field` / `obj->field` lvalues (`&h.arena`). */
    if (!shadow_safe_is_word(text[i]) || (text[i] >= '0' && text[i] <= '9'))
        return 0;
    while (text[i] && n + 1 < cap) {
        char c = text[i];
        if (shadow_safe_is_word(c)) {
            out[n++] = c;
            i++;
            continue;
        }
        if (c == '.' ||
            (c == '-' && text[i + 1] == '>')) {
            if (c == '-') {
                if (n + 2 >= cap) return 0;
                out[n++] = '-';
                out[n++] = '>';
                i += 2;
            } else {
                out[n++] = '.';
                i++;
            }
            if (!shadow_safe_is_word(text[i]) ||
                (text[i] >= '0' && text[i] <= '9'))
                return 0;
            continue;
        }
        break;
    }
    while (text[i] == ' ' || text[i] == '\t') i++;
    if (text[i] != 0) return 0;
    out[n] = 0;
    return n > 0;
}

/* First top-level comma-separated arg as ident or &ident. */
static int shadow_safe_first_arg_ident(const char* args, char* out, size_t cap) {
    char buf[160];
    size_t n = 0;
    int depth = 0;
    size_t i = 0;
    if (!args) return 0;
    while (args[i] == ' ' || args[i] == '\t') i++;
    while (args[i] && n + 1 < sizeof(buf)) {
        char c = args[i];
        if (c == '(' || c == '[' || c == '{') depth++;
        else if (c == ')' || c == ']' || c == '}') {
            if (depth) depth--;
        } else if (c == ',' && depth == 0)
            break;
        buf[n++] = c;
        i++;
    }
    buf[n] = 0;
    return shadow_safe_amp_ident(buf, out, cap);
}

/* Parse `cc_move(...)` starting at `p` (points at 'c' of cc_move). */
static int shadow_safe_parse_cc_move_at(const char* p, char* src, size_t cap) {
    char args[160];
    size_t n;
    int depth;
    if (!p || strncmp(p, "cc_move(", 8) != 0) return 0;
    p += 8;
    depth = 1;
    n = 0;
    while (*p && n + 1 < sizeof(args)) {
        if (*p == '(') depth++;
        else if (*p == ')') {
            depth--;
            if (depth == 0) break;
        }
        args[n++] = *p++;
    }
    if (depth != 0) return 0;
    args[n] = 0;
    return shadow_safe_amp_ident(args, src, cap);
}

static int shadow_safe_parse_cc_move(const char* expr, char* src, size_t cap) {
    const char* p;
    if (!expr) return 0;
    p = strstr(expr, "cc_move(");
    if (!p) return 0;
    if (p > expr && shadow_safe_is_word(p[-1])) return 0;
    return shadow_safe_parse_cc_move_at(p, src, cap);
}

/* Commit every parseable cc_move in text; refuse when unprovable. */
static void shadow_safe_commit_moves_in_text(ShadowSafeCtx* ctx, AstNode* st,
                                            const char* text) {
    const char* p;
    char src[64], msg[160];
    ShadowSafeVar* srcv;
    if (!ctx || !text || ctx->err) return;
    p = text;
    while ((p = strstr(p, "cc_move(")) != NULL) {
        if (p > text && shadow_safe_is_word(p[-1])) {
            p += 7;
            continue;
        }
        if (!shadow_safe_parse_cc_move_at(p, src, sizeof(src))) {
            shadow_safe_err_at(ctx, st, "unparseable cc_move(...)");
            return;
        }
        srcv = shadow_safe_find(ctx, src);
        if (!srcv || !srcv->is_slice) {
            snprintf(msg, sizeof(msg), "cannot prove move of '%s'", src);
            shadow_safe_err_at(ctx, st, msg);
            return;
        }
        if (srcv->moved) {
            snprintf(msg, sizeof(msg), "use of moved slice '%s'", src);
            shadow_safe_err_at(ctx, st, msg);
            fprintf(stderr,
                    "  note: after cc_move(x), the source variable is no longer "
                    "valid\n");
            return;
        }
        srcv->moved = 1;
        p += 8;
    }
}

static int shadow_safe_expr_unique_lit(const char* expr) {
    /* Beachhead: cc_slice_make_id(..., true, ...) marks unique. */
    return expr && strstr(expr, "cc_slice_make_id(") != NULL &&
           strstr(expr, ", true,") != NULL;
}

static int shadow_safe_expr_static_lit(const char* expr) {
    return expr && strstr(expr, "cc_slice_from_static(") != NULL;
}

static int shadow_safe_expr_heap_alloc(const char* expr) {
    if (!expr) return 0;
    return strstr(expr, "malloc(") != NULL || strstr(expr, "calloc(") != NULL ||
           strstr(expr, "realloc(") != NULL;
}

static int shadow_safe_expr_nullish(const char* expr) {
    char id[16];
    if (!expr) return 0;
    while (*expr == ' ' || *expr == '\t' || *expr == '(') expr++;
    if (strcmp(expr, "0") == 0 || strncmp(expr, "0)", 2) == 0) return 1;
    if (shadow_safe_plain_ident(expr, id, sizeof(id)) &&
        (strcmp(id, "NULL") == 0 || strcmp(id, "nullptr") == 0))
        return 1;
    if (strstr(expr, "NULL") != NULL && strchr(expr, '(') == NULL)
        return 1;
    return 0;
}

static void shadow_safe_add_type_name(ShadowSafeCtx* ctx, const char* name) {
    int i;
    if (!ctx || !name || !name[0]) return;
    for (i = 0; i < ctx->ntypes; i++)
        if (strcmp(ctx->type_names[i], name) == 0) return;
    if (ctx->ntypes >= SHADOW_SAFE_TYPE_CAP) {
        shadow_safe_err_at(ctx, NULL,
                           "safety type-name table capacity exceeded (128)");
        return;
    }
    snprintf(ctx->type_names[ctx->ntypes++], sizeof(ctx->type_names[0]), "%s",
             name);
}

static void shadow_safe_add_td_alias(ShadowSafeCtx* ctx, const char* alias,
                                    const char* base) {
    int i;
    if (!ctx || !alias || !alias[0] || !base || !base[0]) return;
    for (i = 0; i < ctx->ntd; i++) {
        if (strcmp(ctx->td_alias[i], alias) == 0) {
            snprintf(ctx->td_base[i], sizeof(ctx->td_base[0]), "%s", base);
            return;
        }
    }
    if (ctx->ntd >= SHADOW_SAFE_TYPE_CAP) return;
    snprintf(ctx->td_alias[ctx->ntd], sizeof(ctx->td_alias[0]), "%s", alias);
    snprintf(ctx->td_base[ctx->ntd], sizeof(ctx->td_base[0]), "%s", base);
    ctx->ntd++;
}

static const char* shadow_safe_resolve_td(ShadowSafeCtx* ctx, const char* ty) {
    int guard;
    if (!ctx || !ty || !ty[0]) return ty;
    for (guard = 0; guard < 8; guard++) {
        int i;
        int hit = 0;
        for (i = 0; i < ctx->ntd; i++) {
            if (strcmp(ctx->td_alias[i], ty) == 0) {
                ty = ctx->td_base[i];
                hit = 1;
                break;
            }
        }
        if (!hit) break;
    }
    return ty;
}

static int shadow_safe_type_name_known(ShadowSafeCtx* ctx, const char* name,
                                      size_t nlen) {
    int i;
    if (!ctx || !name || !nlen) return 0;
    for (i = 0; i < ctx->ntypes; i++) {
        if (strlen(ctx->type_names[i]) == nlen &&
            memcmp(ctx->type_names[i], name, nlen) == 0)
            return 1;
    }
    return 0;
}

/* Seed primitives + prebaked Vecs (aligned with pass_check_type_of.c). */
static void shadow_safe_seed_type_names(ShadowSafeCtx* ctx) {
    static const char* const prims[] = {
        "int", "char", "short", "long", "float", "double", "size_t",
        "intptr_t", "bool", "void", "int32_t", "uint32_t", "int64_t",
        "uint64_t", "CCSlice", "CCArena", "CCError", "CCChan", NULL};
    static const char* const vecs[] = {
        "CCVec_int", "CCVec_char", "CCVec_size_t", "CCVec_float",
        "CCVec_double", "CCVec_voidptr", "CCVec_charptr", "CCVec_intptr",
        NULL};
    int i;
    for (i = 0; prims[i]; i++) shadow_safe_add_type_name(ctx, prims[i]);
    for (i = 0; vecs[i]; i++) shadow_safe_add_type_name(ctx, vecs[i]);
}

/* Refuse unregistered type_of(T).  cc_type_of("…") is a runtime lookup and
 * may intentionally probe unknown names (NULL = not registered). */
static void shadow_safe_check_type_of_text(ShadowSafeCtx* ctx, AstNode* st,
                                          const char* text) {
    const char* p;
    char msg[192];
    if (!ctx || !text || ctx->err) return;
    p = text;
    while ((p = strstr(p, "type_of(")) != NULL) {
        const char* q;
        size_t n = 0;
        if (p > text && shadow_safe_is_word(p[-1])) {
            p += 7;
            continue;
        }
        /* Skip cc_type_of(…). */
        if (p >= text + 3 && memcmp(p - 3, "cc_", 3) == 0) {
            p += 7;
            continue;
        }
        q = p + 8;
        while (*q == ' ' || *q == '\t') q++;
        if (!shadow_safe_is_word(*q) || (*q >= '0' && *q <= '9')) {
            p = q;
            continue;
        }
        while (shadow_safe_is_word(q[n])) n++;
        if (!shadow_safe_type_name_known(ctx, q, n)) {
            snprintf(msg, sizeof(msg),
                     "type_of(%.*s): type '%.*s' has no registered "
                     "cc_type_info",
                     (int)n, q, (int)n, q);
            shadow_safe_err_at(ctx, st, msg);
            fprintf(stderr,
                    "  hint: register with CC_TYPE_INFO_BEGIN(%.*s) / "
                    "CC_TYPE_INFO_END, or use a known primitive/Vec\n",
                    (int)n, q);
            return;
        }
        p = q + n;
    }
}

static int shadow_safe_arena_from_alloc_call(const char* expr, char* arena,
                                            size_t cap) {
    const char* p;
    const char* keys[] = {
        "cc_arena_alloc_slice_bytes(", "cc_arena_alloc_T_count(",
        "cc_arena_alloc_T(", "cc_arena_alloc(", NULL};
    int k;
    if (!expr) return 0;
    for (k = 0; keys[k]; k++) {
        p = strstr(expr, keys[k]);
        if (!p) continue;
        p += strlen(keys[k]);
        /* slice_bytes: (&arena, n) — arena first.
         * alloc_T_count: (T, &arena, n) — arena second. */
        if (strcmp(keys[k], "cc_arena_alloc_T_count(") == 0 ||
            strcmp(keys[k], "cc_arena_alloc_T(") == 0) {
            const char* comma = strchr(p, ',');
            if (!comma) continue;
            p = comma + 1;
        }
        if (shadow_safe_first_arg_ident(p, arena, cap)) return 1;
    }
    return 0;
}

static int shadow_safe_from_parts_base(const char* expr, char* base,
                                      size_t cap) {
    const char* p;
    if (!expr) return 0;
    p = strstr(expr, "cc_slice_from_parts(");
    if (!p) return 0;
    p += strlen("cc_slice_from_parts(");
    return shadow_safe_first_arg_ident(p, base, cap);
}

static int shadow_safe_from_buffer_base(const char* expr, char* base,
                                       size_t cap) {
    const char* p;
    if (!expr) return 0;
    p = strstr(expr, "cc_slice_from_buffer(");
    if (!p) return 0;
    p += strlen("cc_slice_from_buffer(");
    return shadow_safe_first_arg_ident(p, base, cap);
}

static int shadow_safe_is_global(ShadowSafeCtx* ctx, const char* name) {
    int i;
    if (!ctx || !name || !name[0]) return 0;
    for (i = 0; i < ctx->nglobals; i++)
        if (strcmp(ctx->globals[i], name) == 0) return 1;
    return 0;
}

static void shadow_safe_add_global(ShadowSafeCtx* ctx, const char* name) {
    if (!ctx || !name || !name[0]) return;
    if (shadow_safe_is_global(ctx, name)) return;
    if (ctx->nglobals >= SHADOW_SAFE_GLOBAL_CAP) return;
    snprintf(ctx->globals[ctx->nglobals++], sizeof(ctx->globals[0]), "%s",
             name);
}

static int shadow_safe_map_key_installed(ShadowSafeCtx* ctx, const char* key) {
    int i;
    if (!key || !key[0]) return 0;
    if (strcmp(key, "int") == 0) return 1;
    if (strcmp(key, "long") == 0 || strcmp(key, "long_long") == 0) return 1;
    if (strcmp(key, "size_t") == 0 || strcmp(key, "ptrdiff_t") == 0) return 1;
    if (strcmp(key, "unsigned_long") == 0 ||
        strcmp(key, "unsigned_long_long") == 0)
        return 1;
    if (strcmp(key, "CCSliceHdr") == 0) return 1;
    if (strcmp(key, "CCSlicePacked") == 0) return 1;
    if (strcmp(key, "CCSlice") == 0 || strcmp(key, "charslice") == 0) return 1;
    if (strstr(key, "slice") != NULL || strstr(key, "Slice") != NULL) return 1;
    if (strstr(key, "64") != NULL) return 1;
    if (!ctx) return 0;
    for (i = 0; i < ctx->nmap_keys; i++)
        if (strcmp(ctx->map_keys[i], key) == 0) return 1;
    return 0;
}

static void shadow_safe_note_map_key(ShadowSafeCtx* ctx, const char* key) {
    if (!ctx || !key || !key[0] || ctx->nmap_keys >= SHADOW_SAFE_MAPKEY_CAP)
        return;
    if (shadow_safe_map_key_installed(ctx, key)) return;
    snprintf(ctx->map_keys[ctx->nmap_keys++], sizeof(ctx->map_keys[0]), "%s",
             key);
}

/* Refuse Map_/ArrayMap_ with an uninstalled key type (articulate oracle). */
static void shadow_safe_check_map_key_text(ShadowSafeCtx* ctx, AstNode* st,
                                          const char* text) {
    const char* p;
    if (!ctx || !text || ctx->err) return;
    p = text;
    while (*p) {
        const char* hit = NULL;
        size_t skip = 0;
        char compact[96];
        char kbuf[64];
        char msg[192];
        size_t n = 0;
        if (strncmp(p, "ArrayMap_", 9) == 0) {
            hit = p + 9;
            skip = 9;
        } else if (strncmp(p, "array_map_new_count_", 20) == 0) {
            hit = p + 20;
            skip = 20;
        } else if (strncmp(p, "array_map_new_", 14) == 0) {
            hit = p + 14;
            skip = 14;
        } else if (strncmp(p, "Map_", 4) == 0) {
            /* Do not treat the Map_ inside ArrayMap_ as a Map:: instance. */
            if (p >= text + 5 && memcmp(p - 5, "Array", 5) == 0) {
                p++;
                continue;
            }
            hit = p + 4;
            skip = 4;
        } else {
            p++;
            continue;
        }
        (void)skip;
        while (hit[n] && shadow_safe_is_word(hit[n]) && n + 1 < sizeof(compact)) {
            compact[n] = hit[n];
            n++;
        }
        compact[n] = 0;
        if (!compact[0]) {
            p = hit;
            continue;
        }
        {
            int klen = 0;
            const char* us;
            size_t kn;
            /* Prefer the longest installed key prefix, then underscored C
             * types (`size_t`), then the first `_`. */
            for (us = compact; *us; us++) {
                char tmp[64];
                if (*us != '_') continue;
                kn = (size_t)(us - compact);
                if (!kn || kn >= sizeof(tmp)) continue;
                memcpy(tmp, compact, kn);
                tmp[kn] = 0;
                if (shadow_safe_map_key_installed(ctx, tmp) && (int)kn > klen)
                    klen = (int)kn;
            }
            if (!klen) klen = shadow_kv_compact_key_len(compact);
            if (!klen) {
                p = hit + n;
                continue;
            }
            kn = (size_t)klen;
            if (kn >= sizeof(kbuf)) kn = sizeof(kbuf) - 1;
            memcpy(kbuf, compact, kn);
            kbuf[kn] = 0;
        }
        if (!shadow_safe_map_key_installed(ctx, kbuf)) {
            snprintf(msg, sizeof(msg),
                     "map key type '%s' has no installed hash/eq", kbuf);
            shadow_safe_err_at(ctx, st, msg);
            fprintf(stderr,
                    "declare cc_map_key_hash_%s and cc_map_key_eq_%s to "
                    "install it\n",
                    kbuf, kbuf);
            return;
        }
        p = hit + n;
    }
}

static int shadow_safe_refcount_field_at(const char* p) {
    if (!p) return 0;
    if (strncmp(p, "ref_count", 9) == 0 && !shadow_safe_is_word(p[9])) return 1;
    if (strncmp(p, "refcount", 8) == 0 && !shadow_safe_is_word(p[8])) return 1;
    if (strncmp(p, "ref", 3) == 0 && !shadow_safe_is_word(p[3])) return 1;
    return 0;
}

/* Homemade Arc folklore: `--x.ref` / `x->ref--` near free (CVE-2026-10653). */
static int shadow_safe_text_has_refcount_dec(const char* text) {
    const char* p;
    if (!text) return 0;
    for (p = text; *p; p++) {
        if (p[0] == '-' && p[1] == '-') {
            const char* q = p + 2;
            int i;
            while (*q == ' ' || *q == '\t') q++;
            for (i = 0; i < 96 && q[i]; i++) {
                if (q[i] == '.' && shadow_safe_refcount_field_at(q + i + 1))
                    return 1;
                if (q[i] == '-' && q[i + 1] == '>' &&
                    shadow_safe_refcount_field_at(q + i + 2))
                    return 1;
                if (q[i] == ';' || q[i] == '{' || q[i] == '}') break;
            }
        }
        if (p[0] == '.' && shadow_safe_refcount_field_at(p + 1)) {
            const char* f = p + 1;
            while (shadow_safe_is_word(*f)) f++;
            while (*f == ' ' || *f == '\t') f++;
            if (f[0] == '-' && f[1] == '-') return 1;
        }
        if (p[0] == '-' && p[1] == '>' && shadow_safe_refcount_field_at(p + 2)) {
            const char* f = p + 2;
            while (shadow_safe_is_word(*f)) f++;
            while (*f == ' ' || *f == '\t') f++;
            if (f[0] == '-' && f[1] == '-') return 1;
        }
    }
    return 0;
}

static int shadow_safe_node_has_refcount_dec(AstNode* st) {
    int k;
    if (!st) return 0;
    if (shadow_safe_text_has_refcount_dec(st->a)) return 1;
    if (shadow_safe_text_has_refcount_dec(st->b)) return 1;
    if (shadow_safe_text_has_refcount_dec(st->c)) return 1;
    if (shadow_safe_text_has_refcount_dec(st->d)) return 1;
    if (shadow_safe_text_has_refcount_dec(st->e)) return 1;
    for (k = 0; k < st->nbody; k++)
        if (shadow_safe_node_has_refcount_dec(st->body[k])) return 1;
    for (k = 0; k < st->ndbody; k++)
        if (shadow_safe_node_has_refcount_dec(st->dbody[k])) return 1;
    for (k = 0; k < st->nkids; k++)
        if (shadow_safe_node_has_refcount_dec(st->kids[k])) return 1;
    return 0;
}

static int shadow_safe_fn_has_refcount_dec(AstNode* fn) {
    int k;
    if (!fn) return 0;
    if (fn->kind == AST_STATIC_FN && fn->d[0] && fn->nkids == 0 &&
        shadow_safe_text_has_refcount_dec(fn->d))
        return 1;
    for (k = 0; k < fn->nbody; k++)
        if (shadow_safe_node_has_refcount_dec(fn->body[k])) return 1;
    for (k = 0; k < fn->nkids; k++)
        if (shadow_safe_node_has_refcount_dec(fn->kids[k])) return 1;
    return 0;
}

/* Hand release / consume of a CCPyObj local somewhere in the enclosing fn. */
static int shadow_safe_text_releases_py_obj(const char* text, const char* name) {
    char needle[96];
    const char* p;
    size_t nlen;
    if (!text || !name || !name[0]) return 0;
    nlen = strlen(name);
    if (nlen + 24 >= sizeof(needle)) return 0;
    snprintf(needle, sizeof(needle), "cc_py_obj_release(&%s)", name);
    if (strstr(text, needle)) return 1;
    snprintf(needle, sizeof(needle), "cc_py_obj_release(&%s )", name);
    if (strstr(text, needle)) return 1;
    /* `return name;` / `return name` as a whole-word consume. */
    p = text;
    while ((p = strstr(p, "return")) != NULL) {
        const char* q = p + 6;
        while (*q == ' ' || *q == '\t' || *q == '\n' || *q == '\r') q++;
        if (strncmp(q, name, nlen) == 0 && !shadow_safe_is_word(q[nlen]))
            return 1;
        p += 6;
    }
    return 0;
}

static int shadow_safe_args_target_py_obj(const char* args, const char* name) {
    const char* p;
    size_t nlen;
    if (!args || !name || !name[0]) return 0;
    nlen = strlen(name);
    p = args;
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '&') {
        p++;
        while (*p == ' ' || *p == '\t') p++;
        if (strncmp(p, name, nlen) == 0 && !shadow_safe_is_word(p[nlen]))
            return 1;
    }
    return 0;
}

static int shadow_safe_node_releases_py_obj(AstNode* st, const char* name) {
    int k;
    if (!st || !name) return 0;
    /* AST_CALL_ARGS: a=callee, b=args — not one contiguous spelling. */
    if ((st->kind == AST_CALL_ARGS || st->kind == AST_CALL_NUM) &&
        strcmp(st->a, "cc_py_obj_release") == 0 &&
        shadow_safe_args_target_py_obj(st->b, name))
        return 1;
    if (shadow_safe_text_releases_py_obj(st->a, name) ||
        shadow_safe_text_releases_py_obj(st->b, name) ||
        shadow_safe_text_releases_py_obj(st->c, name) ||
        shadow_safe_text_releases_py_obj(st->d, name) ||
        shadow_safe_text_releases_py_obj(st->e, name))
        return 1;
    for (k = 0; k < st->nbody; k++)
        if (shadow_safe_node_releases_py_obj(st->body[k], name)) return 1;
    for (k = 0; k < st->nkids; k++)
        if (shadow_safe_node_releases_py_obj(st->kids[k], name)) return 1;
    return 0;
}

static int shadow_safe_fn_releases_py_obj(AstNode* fn, const char* name) {
    int k;
    if (!fn || !name || !name[0]) return 0;
    if (fn->kind == AST_STATIC_FN && fn->d[0] && fn->nkids == 0 &&
        shadow_safe_text_releases_py_obj(fn->d, name))
        return 1;
    for (k = 0; k < fn->nbody; k++)
        if (shadow_safe_node_releases_py_obj(fn->body[k], name)) return 1;
    for (k = 0; k < fn->nkids; k++)
        if (shadow_safe_node_releases_py_obj(fn->kids[k], name)) return 1;
    return 0;
}

static int shadow_safe_type_is_slice(const char* ty) {
    if (!ty) return 0;
    if (strcmp(ty, "CCSlice") == 0) return 1;
    if (strncmp(ty, "CCSlice_", 8) == 0) return 1;
    if (strstr(ty, "Slice") != NULL) return 1;
    return 0;
}

/* AST_TYPED_INIT.e == "!" records parse-time `T[:!]` (emit ignores bare !). */
static int shadow_safe_node_unique_slice(AstNode* st) {
    if (!st) return 0;
    if (st->e[0] == '!' && st->e[1] == 0) return 1;
    if (strstr(st->a, "[:!]") != NULL) return 1;
    return 0;
}

/* Channel-stable: unique or immortal/static bytes. */
static int shadow_safe_slice_channel_stable(ShadowSafeVar* v) {
    return v && v->is_slice && (v->unique || v->is_static_slice);
}

static int shadow_safe_type_is_arena(const char* ty) {
    return ty && (strcmp(ty, "CCArena") == 0 || strcmp(ty, "CCArena*") == 0);
}

static void shadow_safe_pin_arena(ShadowSafeCtx* ctx, const char* arena,
                                 const char* nursery) {
    int i;
    if (!ctx || !arena || !arena[0]) return;
    for (i = 0; i < ctx->npin; i++) {
        if (strcmp(ctx->pinned[i].arena, arena) == 0 &&
            ((!nursery || !nursery[0])
                 ? !ctx->pinned[i].nursery[0]
                 : strcmp(ctx->pinned[i].nursery, nursery) == 0))
            return;
    }
    if (ctx->npin >= SHADOW_SAFE_PIN_CAP) {
        shadow_safe_err_at(ctx, NULL,
                           "safety arena-pin table capacity exceeded (24)");
        return;
    }
    snprintf(ctx->pinned[ctx->npin].arena, sizeof(ctx->pinned[0].arena), "%s",
             arena);
    snprintf(ctx->pinned[ctx->npin].nursery, sizeof(ctx->pinned[0].nursery),
             "%s", nursery ? nursery : "");
    ctx->npin++;
}

static void shadow_safe_unpin_nursery(ShadowSafeCtx* ctx, const char* nursery) {
    int i, w;
    if (!ctx || !nursery || !nursery[0]) return;
    w = 0;
    for (i = 0; i < ctx->npin; i++) {
        if (strcmp(ctx->pinned[i].nursery, nursery) == 0) continue;
        if (w != i) ctx->pinned[w] = ctx->pinned[i];
        w++;
    }
    ctx->npin = w;
}

static int shadow_safe_arena_pinned(ShadowSafeCtx* ctx, const char* arena) {
    int i;
    if (!ctx || !arena) return 0;
    for (i = 0; i < ctx->npin; i++) {
        if (strcmp(ctx->pinned[i].arena, arena) == 0) return 1;
    }
    return 0;
}

static int shadow_safe_type_is_nursery(const char* ty) {
    return ty && strstr(ty, "Nursery") != NULL;
}

static int shadow_safe_live_borrow_on(ShadowSafeCtx* ctx, const char* arena) {
    int i;
    if (!ctx || !arena) return 0;
    /* Epoch-end vs in-scope slice views only — arena pointers may outlive
     * reset (sibling-block / after-scope smokes); spawn pins cover captures. */
    for (i = 0; i < ctx->nvars; i++) {
        ShadowSafeVar* v = ctx->vars[i];
        if (!v || v->moved || !v->arena_view) continue;
        if (shadow_safe_ident_eq(v->arena, arena)) return 1;
    }
    return 0;
}

static void shadow_safe_check_moved_use(ShadowSafeCtx* ctx, AstNode* st,
                                       const char* text) {
    int i;
    char msg[160];
    if (!ctx || !text) return;
    for (i = 0; i < ctx->nvars; i++) {
        ShadowSafeVar* v = ctx->vars[i];
        if (!v || !v->is_slice || !v->moved) continue;
        if (!shadow_safe_text_uses_outside_move(text, v->name)) continue;
        snprintf(msg, sizeof(msg), "use of moved slice '%s'", v->name);
        shadow_safe_err_at(ctx, st, msg);
        fprintf(stderr,
                "  note: after cc_move(x), the source variable is no longer "
                "valid\n");
        return;
    }
}

static const char* shadow_safe_skip_ws(const char* p) {
    while (p && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
    return p;
}

static void shadow_safe_dom_push(ShadowSafeCtx* ctx, const char* root,
                                 const char* arm) {
    if (!ctx || !root || !root[0] || !arm || !arm[0]) return;
    if (ctx->ndom >= SHADOW_SAFE_DOM_CAP) return;
    snprintf(ctx->dom[ctx->ndom].root, sizeof(ctx->dom[0].root), "%s", root);
    snprintf(ctx->dom[ctx->ndom].arm, sizeof(ctx->dom[0].arm), "%s", arm);
    ctx->ndom++;
}

static void shadow_safe_dom_pop(ShadowSafeCtx* ctx) {
    if (ctx && ctx->ndom > 0) ctx->ndom--;
}

static int shadow_safe_dom_has(ShadowSafeCtx* ctx, const char* root,
                               const char* arm) {
    int i;
    if (!ctx || !root || !arm) return 0;
    for (i = 0; i < ctx->ndom; i++) {
        if (strcmp(ctx->dom[i].root, root) == 0 &&
            strcmp(ctx->dom[i].arm, arm) == 0)
            return 1;
    }
    return 0;
}

/* Parse `root.arm` / `root->arm` (last projection in text) for a tracked var. */
static int shadow_safe_parse_proj_arm(ShadowSafeCtx* ctx, const char* text,
                                     char* root, size_t rcap, char* arm,
                                     size_t acap) {
    int vi, ai;
    const char* best = NULL;
    size_t best_nlen = 0;
    int best_ai = -1;
    int best_vi = -1;
    if (!ctx || !text || !root || !arm || !rcap || !acap) return 0;
    for (vi = 0; vi < ctx->nvars; vi++) {
        ShadowSafeVar* v = ctx->vars[vi];
        ShadowSafeVariant* vt;
        size_t nlen;
        const char* p;
        if ((!v->is_variant && !v->is_variant_ptr) || !v->name[0]) continue;
        vt = shadow_safe_find_variant(ctx, v->ty);
        if (!vt) continue;
        nlen = strlen(v->name);
        p = text;
        while ((p = strstr(p, v->name)) != NULL) {
            char before = (p > text) ? p[-1] : 0;
            const char* after = p + nlen;
            if (shadow_safe_is_word(before) || shadow_safe_is_word(*after)) {
                p = after;
                continue;
            }
            if (after[0] == '-' && after[1] == '>') after += 2;
            else if (after[0] == '.') after += 1;
            else {
                p = p + nlen;
                continue;
            }
            for (ai = 0; ai < vt->narm; ai++) {
                size_t want = strlen(vt->arms[ai]);
                if (want && strncmp(after, vt->arms[ai], want) == 0 &&
                    !shadow_safe_is_word(after[want])) {
                    if (!best || p >= best) {
                        best = p;
                        best_nlen = nlen;
                        best_ai = ai;
                        best_vi = vi;
                    }
                    break;
                }
            }
            p = p + nlen;
        }
    }
    (void)best_nlen;
    if (best_vi < 0 || best_ai < 0) return 0;
    {
        ShadowSafeVar* v = ctx->vars[best_vi];
        ShadowSafeVariant* vt = shadow_safe_find_variant(ctx, v->ty);
        if (!vt) return 0;
        snprintf(root, rcap, "%s", v->name);
        snprintf(arm, acap, "%s", vt->arms[best_ai]);
        return 1;
    }
}

/* Two-arm complement: the inactive arm when `named` is the projected one. */
static int shadow_safe_complement_arm(ShadowSafeCtx* ctx, const char* root,
                                     const char* named, char* out, size_t ocap) {
    ShadowSafeVar* v;
    ShadowSafeVariant* vt;
    int ai;
    if (!ctx || !root || !named || !out || !ocap) return 0;
    v = shadow_safe_find(ctx, root);
    if (!v) return 0;
    vt = shadow_safe_find_variant(ctx, v->ty);
    if (!vt || vt->narm != 2) return 0;
    for (ai = 0; ai < vt->narm; ai++) {
        if (strcmp(vt->arms[ai], named) == 0) {
            snprintf(out, ocap, "%s", vt->arms[ai == 0 ? 1 : 0]);
            return 1;
        }
    }
    return 0;
}

/* Map `.arm` / `Name_arm` / bare `arm` to a registered arm of root's variant. */
static int shadow_safe_arm_from_tag(ShadowSafeCtx* ctx, const char* root,
                                   const char* tag, size_t tlen, char* arm,
                                   size_t acap) {
    ShadowSafeVar* v;
    ShadowSafeVariant* vt;
    int ai;
    const char* t;
    size_t tl;
    if (!ctx || !root || !tag || !arm || acap == 0 || tlen == 0) return 0;
    v = shadow_safe_find(ctx, root);
    if (!v || (!v->is_variant && !v->is_variant_ptr)) return 0;
    vt = shadow_safe_find_variant(ctx, v->ty);
    if (!vt) return 0;
    t = tag;
    tl = tlen;
    while (tl && (*t == ' ' || *t == '\t')) {
        t++;
        tl--;
    }
    while (tl && (t[tl - 1] == ' ' || t[tl - 1] == '\t')) tl--;
    if (!tl) return 0;
    if (t[0] == '.') {
        t++;
        tl--;
        while (tl && (*t == ' ' || *t == '\t')) {
            t++;
            tl--;
        }
        for (ai = 0; ai < vt->narm; ai++) {
            size_t want = strlen(vt->arms[ai]);
            if (tl == want && memcmp(t, vt->arms[ai], tl) == 0) {
                snprintf(arm, acap, "%s", vt->arms[ai]);
                return 1;
            }
        }
        return 0;
    }
    for (ai = 0; ai < vt->narm; ai++) {
        char want[96];
        size_t wlen;
        size_t alen = strlen(vt->arms[ai]);
        if (tl == alen && memcmp(t, vt->arms[ai], tl) == 0) {
            snprintf(arm, acap, "%s", vt->arms[ai]);
            return 1;
        }
        snprintf(want, sizeof(want), "%s_%s", vt->name, vt->arms[ai]);
        wlen = strlen(want);
        if (tl == wlen && memcmp(t, want, tl) == 0) {
            snprintf(arm, acap, "%s", vt->arms[ai]);
            return 1;
        }
    }
    return 0;
}

/* `if (root.kind == Tag)` / `if (root->kind == Tag)` — == only (then-arm). */
static int shadow_safe_parse_kind_eq_guard(ShadowSafeCtx* ctx, const char* cond,
                                          char* root, size_t rcap, char* arm,
                                          size_t acap) {
    const char* kind;
    const char* eq;
    const char* rs;
    const char* re;
    const char* tag;
    size_t rlen, tlen;
    int arrow = 0;
    if (!ctx || !cond || !root || !arm) return 0;
    kind = strstr(cond, "->kind");
    if (kind) {
        arrow = 1;
    } else {
        kind = strstr(cond, ".kind");
        if (!kind) return 0;
    }
    /* Root: trim leading spaces/`(` before kind token. */
    rs = cond;
    while (*rs == ' ' || *rs == '\t' || *rs == '(') rs++;
    re = kind;
    while (re > rs && (re[-1] == ' ' || re[-1] == '\t')) re--;
    rlen = (size_t)(re - rs);
    if (rlen == 0 || rlen >= rcap) goto try_reversed;
    memcpy(root, rs, rlen);
    root[rlen] = 0;
    /* Peel wrapping parens on root. */
    while (root[0] == '(') {
        size_t L = strlen(root);
        if (L < 2 || root[L - 1] != ')') break;
        memmove(root, root + 1, L - 2);
        root[L - 2] = 0;
    }
    eq = kind + (arrow ? 6 : 5);
    eq = shadow_safe_skip_ws(eq);
    if (eq[0] != '=' || eq[1] != '=') goto try_reversed;
    tag = shadow_safe_skip_ws(eq + 2);
    tlen = 0;
    if (tag[0] == '.') {
        tlen = 1;
        while (shadow_safe_is_word(tag[tlen])) tlen++;
    } else {
        while (shadow_safe_is_word(tag[tlen])) tlen++;
    }
    if (!tlen) goto try_reversed;
    return shadow_safe_arm_from_tag(ctx, root, tag, tlen, arm, acap);

try_reversed:
    /* Reversed: `Tag == root.kind` / `.arm == root->kind` */
    {
        const char* eqp = strstr(cond, "==");
        const char* kp;
        const char* ts;
        const char* te;
        if (!eqp || eqp >= kind) return 0;
        kp = eqp + 2;
        while (*kp == ' ' || *kp == '\t') kp++;
        /* kp should point to root, then .kind / ->kind */
        re = kind;
        while (re > kp && (re[-1] == ' ' || re[-1] == '\t')) re--;
        rlen = (size_t)(re - kp);
        if (rlen == 0 || rlen >= rcap) return 0;
        memcpy(root, kp, rlen);
        root[rlen] = 0;
        /* Tag is on the left of `==` */
        ts = cond;
        while (*ts == ' ' || *ts == '\t' || *ts == '(') ts++;
        te = eqp;
        while (te > ts && (te[-1] == ' ' || te[-1] == '\t')) te--;
        tlen = (size_t)(te - ts);
        if (!tlen || tlen >= acap) return 0;
        return shadow_safe_arm_from_tag(ctx, root, ts, tlen, arm, acap);
    }
}

/* Switch subject → root ident (`v.kind` / `p->kind` / `v` / `p`). */
static int shadow_safe_switch_root(const char* expr, char* root, size_t rcap) {
    char buf[128];
    size_t n;
    const char* s;
    const char* e;
    if (!expr || !root || rcap == 0) return 0;
    s = shadow_safe_skip_ws(expr);
    e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t')) e--;
    while (e - s >= 2 && s[0] == '(' && e[-1] == ')') {
        s++;
        e--;
        while (s < e && (*s == ' ' || *s == '\t')) s++;
        while (e > s && (e[-1] == ' ' || e[-1] == '\t')) e--;
    }
    n = (size_t)(e - s);
    if (n == 0 || n >= sizeof(buf)) return 0;
    memcpy(buf, s, n);
    buf[n] = 0;
    if (n > 6 && strcmp(buf + n - 6, "->kind") == 0) {
        n -= 6;
        while (n && (buf[n - 1] == ' ' || buf[n - 1] == '\t')) n--;
        buf[n] = 0;
    } else if (n > 5 && strcmp(buf + n - 5, ".kind") == 0) {
        n -= 5;
        while (n && (buf[n - 1] == ' ' || buf[n - 1] == '\t')) n--;
        buf[n] = 0;
    }
    if (!n || n >= rcap) return 0;
    for (s = buf; *s; s++) {
        if (!shadow_safe_is_word(*s)) return 0;
    }
    snprintf(root, rcap, "%s", buf);
    return 1;
}

static const char* shadow_safe_find_kw(const char* s, const char* kw) {
    size_t klen;
    const char* p;
    if (!s || !kw || !kw[0]) return NULL;
    klen = strlen(kw);
    p = s;
    while ((p = strstr(p, kw)) != NULL) {
        char b = (p > s) ? p[-1] : 0;
        char a = p[klen];
        if (!shadow_safe_is_word(b) && !shadow_safe_is_word(a)) return p;
        p += klen;
    }
    return NULL;
}

static int shadow_safe_proj_protected(const char* after_arm) {
    const char* p = shadow_safe_skip_ws(after_arm);
    if (!p) return 0;
    if (p[0] == '!' && p[1] == '>') return 1;
    if (p[0] == '?' && p[1] == '>') return 1;
    return 0;
}

/* Refuse unprotected @variant arm projection and raw `.u` reach-in.
 * Projection is legal under syntactic domination, or with `!>` / `?>`. */
static void shadow_safe_check_variant_text(ShadowSafeCtx* ctx, AstNode* st,
                                          const char* text) {
    int vi, ai;
    char msg[192];
    if (!ctx || !text || ctx->err) return;
    for (vi = 0; vi < ctx->nvars; vi++) {
        ShadowSafeVar* v = ctx->vars[vi];
        ShadowSafeVariant* vt;
        const char* p;
        size_t nlen;
        int arrow_form;
        if ((!v->is_variant && !v->is_variant_ptr) || !v->name[0]) continue;
        nlen = strlen(v->name);
        vt = shadow_safe_find_variant(ctx, v->ty);
        if (!vt) continue;
        arrow_form = v->is_variant_ptr ? 1 : 0;
        p = text;
        while ((p = strstr(p, v->name)) != NULL) {
            char before = (p > text) ? p[-1] : 0;
            const char* after = p + nlen;
            int use_arrow;
            if (shadow_safe_is_word(before) || shadow_safe_is_word(*after)) {
                p = after;
                continue;
            }
            /* Raw `.u` / `->u` — never dominated. */
            if (after[0] == '-' && after[1] == '>' && after[2] == 'u' &&
                (after[3] == '.' || after[3] == 0 ||
                 !shadow_safe_is_word(after[3]))) {
                if (vt->is_schema)
                    snprintf(msg, sizeof(msg),
                             "cannot reach into schema union '%s' via '.u'",
                             v->ty);
                else if (vt->is_packed)
                    snprintf(msg, sizeof(msg),
                             "variant '%s' is @variant(packed): it has no exposed '.u' union",
                             vt->name);
                else
                    snprintf(msg, sizeof(msg),
                             "cannot reach into variant '%s' via '.u'",
                             vt->name);
                shadow_safe_err_at(ctx, st, msg);
                if (!vt->is_schema && !vt->is_packed)
                    fprintf(stderr,
                            "  note: access inactive or packed union payload via "
                            "a dominating check, '!>' handler, or '?>' fallback\n");
                return;
            }
            if (after[0] == '.' && after[1] == 'u' &&
                (after[2] == '.' || after[2] == 0 ||
                 !shadow_safe_is_word(after[2]))) {
                if (vt->is_schema)
                    snprintf(msg, sizeof(msg),
                             "cannot reach into schema union '%s' via '.u'",
                             v->ty);
                else if (vt->is_packed)
                    snprintf(msg, sizeof(msg),
                             "variant '%s' is @variant(packed): it has no exposed '.u' union",
                             vt->name);
                else
                    snprintf(msg, sizeof(msg),
                             "cannot reach into variant '%s' via '.u'",
                             vt->name);
                shadow_safe_err_at(ctx, st, msg);
                if (!vt->is_schema && !vt->is_packed)
                    fprintf(stderr,
                            "  note: access inactive or packed union payload via "
                            "a dominating check, '!>' handler, or '?>' fallback\n");
                return;
            }
            use_arrow = 0;
            if (after[0] == '-' && after[1] == '>') {
                if (!arrow_form && !v->is_variant) {
                    p = after;
                    continue;
                }
                use_arrow = 1;
                after += 2;
            } else if (after[0] == '.') {
                if (arrow_form && !v->is_variant) {
                    /* Pointer local: only `->` projects. */
                    p = after;
                    continue;
                }
                after += 1;
            } else {
                p = p + nlen;
                continue;
            }
            {
                const char* arm = after;
                size_t al = 0;
                while (shadow_safe_is_word(arm[al])) al++;
                if (al == 4 && memcmp(arm, "kind", 4) == 0) {
                    const char* eq = arm + al;
                    while (*eq == ' ' || *eq == '\t') eq++;
                    if (*eq == '=' && eq[1] != '=') {
                        /* Prefer the dedicated AST_ASSIGN handler (full
                         * oracle text + `.kind` column). Skip here. */
                        p = arm;
                        continue;
                    }
                    p = arm;
                    continue;
                }
                if (al == 1 && arm[0] == 'u') {
                    p = arm;
                    continue;
                }
                for (ai = 0; ai < vt->narm; ai++) {
                    size_t want = strlen(vt->arms[ai]);
                    if (al == want && memcmp(arm, vt->arms[ai], al) == 0) {
                        const char* eq = arm + al;
                        while (*eq == ' ' || *eq == '\t') eq++;
                        /* Designated init `.arm =` is not a projection. */
                        if (*eq == '=' && eq[1] != '=') break;
                        if (shadow_safe_proj_protected(eq)) break;
                        if (shadow_safe_dom_has(ctx, v->name, vt->arms[ai]))
                            break;
                        (void)use_arrow;
                        snprintf(msg, sizeof(msg),
                                 "projection of arm '%s' is not dominated "
                                 "by a kind check and has no !> handler",
                                 vt->arms[ai]);
                        /* Point at the arm identifier when possible. */
                        if (st && ctx->cache && st->file_id && arm) {
                            FileTape* ft = tape_by_id(ctx->cache, st->file_id);
                            if (ft && ft->bytes && st->tok_off < ft->len) {
                                size_t off = st->tok_off;
                                size_t i;
                                for (i = st->tok_off; i + al < ft->len; i++) {
                                    if (memcmp(ft->bytes + i, arm, al) == 0 &&
                                        (i == 0 ||
                                         !shadow_safe_is_word(ft->bytes[i - 1])) &&
                                        !shadow_safe_is_word(ft->bytes[i + al])) {
                                        /* Prefer the `.` / `->` of the projection. */
                                        if (i > 0 && ft->bytes[i - 1] == '.') {
                                            off = i - 1;
                                            break;
                                        }
                                        if (i > 1 && ft->bytes[i - 2] == '-' &&
                                            ft->bytes[i - 1] == '>') {
                                            off = i - 2;
                                            break;
                                        }
                                    }
                                }
                                {
                                    int line = 1, col = 1;
                                    if (!ctx->err) {
                                        ctx->err = 1;
                                        offset_to_linecol(ft, off, &line, &col);
                                        tape_logical_at(ft, off, NULL, 0, &line);
                                        fprintf(stderr, "%s:%d:%d: error: %s\n",
                                                ft->path, line, col, msg);
                                        fprintf(stderr,
                                                "note: protect it: 'if (%s.kind == "
                                                "%s_%s) { ... }' or 'case .%s:' in "
                                                "a switch on '%s', or handle the "
                                                "other arms: '%s.%s !> { "
                                                "...diverge... }' / '%s.%s ?> "
                                                "fallback'\n",
                                                v->name, vt->name, vt->arms[ai],
                                                vt->arms[ai], v->name, v->name,
                                                vt->arms[ai], v->name,
                                                vt->arms[ai]);
                                        return;
                                    }
                                }
                            }
                        }
                        shadow_safe_err_at(ctx, st, msg);
                        fprintf(stderr,
                                "note: protect it: 'if (%s.kind == %s_%s) { ... }' "
                                "or 'case .%s:' in a switch on '%s', or handle "
                                "the other arms: '%s.%s !> { ...diverge... }' / "
                                "'%s.%s ?> fallback'\n",
                                v->name, vt->name, vt->arms[ai], vt->arms[ai],
                                v->name, v->name, vt->arms[ai], v->name,
                                vt->arms[ai]);
                        return;
                    }
                }
            }
            p = p + nlen;
        }
    }
}

/* Per-case domination over opaque switch body text. */
static void shadow_safe_check_switch_variant_body(ShadowSafeCtx* ctx, AstNode* st,
                                                 const char* root,
                                                 const char* body) {
    const char* p;
    if (!ctx || !st || !root || !body || ctx->err) return;
    /* Leading text before the first case is unchecked for projections
     * under a case arm (declarations only in well-formed switches). */
    p = body;
    while (*p && !ctx->err) {
        const char* ccase = shadow_safe_find_kw(p, "case");
        const char* cdef = shadow_safe_find_kw(p, "default");
        const char* lab;
        const char* colon;
        const char* region;
        const char* next;
        char arm[32];
        char chunk[4096];
        size_t rlen;
        int is_default = 0;
        int pushed = 0;
        if (!ccase && !cdef) break;
        if (ccase && (!cdef || ccase < cdef)) {
            lab = ccase + 4;
            is_default = 0;
        } else {
            lab = cdef + 7;
            is_default = 1;
        }
        colon = strchr(lab, ':');
        if (!colon) break;
        region = colon + 1;
        next = shadow_safe_find_kw(region, "case");
        {
            const char* nd = shadow_safe_find_kw(region, "default");
            if (nd && (!next || nd < next)) next = nd;
        }
        rlen = next ? (size_t)(next - region) : strlen(region);
        if (rlen >= sizeof(chunk)) {
            shadow_safe_err_at(ctx, st, "switch case body too long for safety");
            return;
        }
        memcpy(chunk, region, rlen);
        chunk[rlen] = 0;
        arm[0] = 0;
        if (!is_default) {
            size_t tlen = (size_t)(colon - lab);
            if (shadow_safe_arm_from_tag(ctx, root, lab, tlen, arm,
                                        sizeof(arm))) {
                shadow_safe_dom_push(ctx, root, arm);
                pushed = 1;
            }
        }
        shadow_safe_check_variant_text(ctx, st, chunk);
        if (pushed) shadow_safe_dom_pop(ctx);
        p = next ? next : region + strlen(region);
    }
}

static void shadow_safe_scan_texts(ShadowSafeCtx* ctx, AstNode* st) {
    if (!ctx || !st || ctx->err) return;
    /* Commit moves before use-after-move scan so `f(cc_move(s), s)` fails. */
    shadow_safe_commit_moves_in_text(ctx, st, st->a);
    if (ctx->err) return;
    shadow_safe_commit_moves_in_text(ctx, st, st->b);
    if (ctx->err) return;
    shadow_safe_commit_moves_in_text(ctx, st, st->c);
    if (ctx->err) return;
    shadow_safe_commit_moves_in_text(ctx, st, st->d);
    if (ctx->err) return;
    shadow_safe_commit_moves_in_text(ctx, st, st->e);
    if (ctx->err) return;
    shadow_safe_check_moved_use(ctx, st, st->a);
    if (ctx->err) return;
    shadow_safe_check_moved_use(ctx, st, st->b);
    if (ctx->err) return;
    shadow_safe_check_moved_use(ctx, st, st->c);
    if (ctx->err) return;
    shadow_safe_check_moved_use(ctx, st, st->d);
    if (ctx->err) return;
    shadow_safe_check_moved_use(ctx, st, st->e);
    if (ctx->err) return;
    shadow_safe_check_variant_text(ctx, st, st->a);
    if (ctx->err) return;
    shadow_safe_check_variant_text(ctx, st, st->b);
    if (ctx->err) return;
    shadow_safe_check_variant_text(ctx, st, st->c);
    if (ctx->err) return;
    shadow_safe_check_variant_text(ctx, st, st->d);
    if (ctx->err) return;
    shadow_safe_check_variant_text(ctx, st, st->e);
    if (ctx->err) return;
    shadow_safe_check_type_of_text(ctx, st, st->a);
    if (ctx->err) return;
    shadow_safe_check_type_of_text(ctx, st, st->b);
    if (ctx->err) return;
    shadow_safe_check_type_of_text(ctx, st, st->c);
    if (ctx->err) return;
    shadow_safe_check_type_of_text(ctx, st, st->d);
    if (ctx->err) return;
    shadow_safe_check_type_of_text(ctx, st, st->e);
    if (ctx->err) return;
    if (ctx->in_async) {
        shadow_safe_check_async_chan_text(ctx, st, st->a);
        if (ctx->err) return;
        shadow_safe_check_async_chan_text(ctx, st, st->b);
        if (ctx->err) return;
        shadow_safe_check_async_chan_text(ctx, st, st->c);
    }
    if (ctx->err) return;
    shadow_safe_on_chan_send_text(ctx, st, st->a);
    if (ctx->err) return;
    shadow_safe_on_chan_send_text(ctx, st, st->c);
}

static void shadow_safe_on_typed_init(ShadowSafeCtx* ctx, AstNode* st) {
    ShadowSafeVar* v;
    ShadowSafeVar* srcv;
    char src[64], arena[64], base[64], msg[192];
    const char* ty;
    const char* name;
    const char* expr;
    int stars;
    if (!ctx || !st || ctx->err) return;
    ty = st->a;
    name = st->b;
    expr = st->c;
    /* AST_PTR_INIT stores Type without '*'; TYPED_INIT/VAL_DESTROY use d="*". */
    stars = (st->d[0] == '*') || (st->kind == AST_PTR_INIT) ||
            (ty && strchr(ty, '*') != NULL);
    shadow_safe_check_moved_use(ctx, st, expr);
    if (ctx->err) return;

    v = shadow_safe_add(ctx, name);
    if (!v) return;
    if (ty) snprintf(v->ty, sizeof(v->ty), "%s", ty);
    /* `T buf[N] = …` — dims live in e (unless e is the unique `!` marker). */
    if (st->e[0] == '[') v->is_stack_array = 1;
    if (ty) shadow_safe_check_map_key_text(ctx, st, ty);
    if (ctx->err) return;
    if (expr) shadow_safe_check_map_key_text(ctx, st, expr);
    if (ctx->err) return;
    if (!stars && ty) {
        char leaf[64];
        shadow_safe_strip_ty(ty, leaf, sizeof(leaf));
        if (shadow_safe_find_variant(ctx, leaf)) {
            v->is_variant = 1;
            snprintf(v->ty, sizeof(v->ty), "%s", leaf);
        }
    }
    /* Aggregate with CCSlice and no sibling CCArena: if init mentions an
     * arena-view slice local, mark non-stable for channel-send ban. */
    if (!stars && ty && expr) {
        char leaf[64];
        ShadowSafeStruct* s;
        int fi, has_slice = 0, has_arena = 0, vi;
        shadow_safe_strip_ty(ty, leaf, sizeof(leaf));
        s = shadow_safe_find_struct(ctx, leaf);
        if (s) {
            for (fi = 0; fi < s->nf; fi++) {
                if (strcmp(s->fty[fi], "CCSlice") == 0) has_slice = 1;
                if (strcmp(s->fty[fi], "CCArena") == 0) has_arena = 1;
            }
            if (has_slice && !has_arena) {
                for (vi = 0; vi < ctx->nvars; vi++) {
                    if (ctx->vars[vi] && ctx->vars[vi]->arena_view &&
                        shadow_safe_text_uses(expr, ctx->vars[vi]->name)) {
                        v->nonstable_slice = 1;
                        break;
                    }
                }
            }
        }
    }

    if (shadow_safe_type_is_arena(ty) && !stars) {
        v->is_arena = 1;
        return;
    }
    if (shadow_safe_type_is_slice(ty) && !stars) {
        v->is_slice = 1;
        if (shadow_safe_node_unique_slice(st)) v->unique = 1;
        if (strstr(expr, "cc_move(") != NULL) {
            if (!shadow_safe_parse_cc_move(expr, src, sizeof(src))) {
                shadow_safe_err_at(ctx, st, "unparseable cc_move(...)");
                return;
            }
            srcv = shadow_safe_find(ctx, src);
            if (!srcv || !srcv->is_slice) {
                snprintf(msg, sizeof(msg), "cannot prove move of '%s'", src);
                shadow_safe_err_at(ctx, st, msg);
                return;
            }
            if (srcv->moved) {
                snprintf(msg, sizeof(msg), "use of moved slice '%s'", src);
                shadow_safe_err_at(ctx, st, msg);
                return;
            }
            srcv->moved = 1;
            v->unique = srcv->unique;
            v->is_static_slice = srcv->is_static_slice;
            v->arena_view = srcv->arena_view;
            snprintf(v->arena, sizeof(v->arena), "%s", srcv->arena);
            return;
        }
        if (shadow_safe_plain_ident(expr, src, sizeof(src))) {
            srcv = shadow_safe_find(ctx, src);
            if (srcv && srcv->is_slice) {
                if (srcv->moved) {
                    snprintf(msg, sizeof(msg), "use of moved slice '%s'", src);
                    shadow_safe_err_at(ctx, st, msg);
                    return;
                }
                if (srcv->unique) {
                    shadow_safe_err_at(ctx, st, "cannot copy unique slice");
                    fprintf(stderr,
                            "  hint: unique slices have move-only semantics; "
                            "use cc_move(x) to transfer ownership\n");
                    return;
                }
                v->unique = srcv->unique;
                v->is_static_slice = srcv->is_static_slice;
                v->arena_view = srcv->arena_view;
                snprintf(v->arena, sizeof(v->arena), "%s", srcv->arena);
                return;
            }
        }
        if (shadow_safe_expr_unique_lit(expr)) v->unique = 1;
        if (shadow_safe_expr_static_lit(expr)) v->is_static_slice = 1;
        if (expr && (strstr(expr, "@scratch") != NULL ||
                     strstr(expr, "__cc_str_scratch") != NULL))
            v->is_scratch_string = 1;
        if (shadow_safe_arena_from_alloc_call(expr, arena, sizeof(arena))) {
            v->arena_view = 1;
            snprintf(v->arena, sizeof(v->arena), "%s", arena);
        } else if (shadow_safe_from_parts_base(expr, base, sizeof(base)) ||
                   shadow_safe_from_buffer_base(expr, base, sizeof(base))) {
            srcv = shadow_safe_find(ctx, base);
            if (srcv && srcv->is_stack_array)
                v->is_stack_slice_view = 1;
            if (srcv && (srcv->is_arena_ptr || srcv->arena_view) &&
                srcv->arena[0]) {
                v->arena_view = 1;
                snprintf(v->arena, sizeof(v->arena), "%s", srcv->arena);
            }
        }
        return;
    }
    if (stars) {
        char leaf[64];
        v->is_ptr = 1;
        if (ty) {
            shadow_safe_strip_ty(ty, leaf, sizeof(leaf));
            if (shadow_safe_find_variant(ctx, leaf)) {
                v->is_variant_ptr = 1;
                snprintf(v->ty, sizeof(v->ty), "%s", leaf);
            }
        }
        if (shadow_safe_arena_from_alloc_call(expr, arena, sizeof(arena))) {
            v->is_arena_ptr = 1;
            snprintf(v->arena, sizeof(v->arena), "%s", arena);
            return;
        }
        if (shadow_safe_expr_heap_alloc(expr) || shadow_safe_expr_nullish(expr))
            return;
        if (expr[0] == '&' &&
            shadow_safe_amp_ident(expr, src, sizeof(src))) {
            snprintf(v->alias_of, sizeof(v->alias_of), "%s", src);
            srcv = shadow_safe_find(ctx, src);
            if (srcv && srcv->is_variant) {
                v->is_variant_ptr = 1;
                snprintf(v->ty, sizeof(v->ty), "%s", srcv->ty);
            }
            return;
        }
        if (shadow_safe_plain_ident(expr, src, sizeof(src))) {
            srcv = shadow_safe_find(ctx, src);
            if (srcv && srcv->is_arena_ptr) {
                v->is_arena_ptr = 1;
                snprintf(v->arena, sizeof(v->arena), "%s", srcv->arena);
                return;
            }
            if (srcv && srcv->is_variant_ptr) {
                v->is_variant_ptr = 1;
                snprintf(v->ty, sizeof(v->ty), "%s", srcv->ty);
                return;
            }
            if (srcv && srcv->unproven_ptr) {
                v->unproven_ptr = 1;
                return;
            }
            if (srcv && srcv->alias_of[0]) {
                snprintf(v->alias_of, sizeof(v->alias_of), "%s",
                         srcv->alias_of);
                return;
            }
            if (srcv && srcv->is_ptr) return; /* proven non-arena ptr alias */
            /* Unknown ident as pointer RHS — refuse free later. */
            v->unproven_ptr = 1;
            return;
        }
        /* Callee(...) heap owners (malloc wrappers / factories) — free ok. */
        {
            size_t ei = 0;
            char callee[64];
            size_t cn = 0;
            while (expr[ei] == ' ' || expr[ei] == '\t') ei++;
            if (shadow_safe_is_word(expr[ei]) &&
                !(expr[ei] >= '0' && expr[ei] <= '9')) {
                while (shadow_safe_is_word(expr[ei]) && cn + 1 < sizeof(callee))
                    callee[cn++] = expr[ei++];
                callee[cn] = 0;
                while (expr[ei] == ' ' || expr[ei] == '\t') ei++;
                if (expr[ei] == '(' && cn > 0)
                    return; /* proven: function result */
            }
        }
        /* `(T*)handle` reconstitutes a heap pointer (owned-channel destroy).
         * Cast of a proven heap ptr keeps provenance; arena / unproven
         * sources stay unproven. Nested casts / call results are ok. */
        if (expr[0] == '(') {
            const char* rp = strchr(expr, ')');
            char src2[64];
            const char* after;
            if (rp) {
                after = rp + 1;
                while (*after == ' ' || *after == '\t') after++;
                /* Peel one nested cast layer: `(T*)(U*)x` / `(T*)(intptr_t)y`. */
                if (after[0] == '(') {
                    const char* rp2 = strchr(after, ')');
                    if (rp2) {
                        after = rp2 + 1;
                        while (*after == ' ' || *after == '\t') after++;
                    }
                }
                if (shadow_safe_plain_ident(after, src2, sizeof(src2))) {
                    ShadowSafeVar* sv = shadow_safe_find(ctx, src2);
                    if (!sv) return; /* intptr / unknown reconstitutes */
                    if (sv->is_arena_ptr || sv->unproven_ptr) {
                        /* Cast drops arena-borrow identity → unproven free
                         * target (not "non-owning borrow", which is for
                         * direct aliases like `char* p = bytes`). */
                        v->unproven_ptr = 1;
                        return;
                    }
                    if (sv->is_ptr || sv->is_slice) return; /* proven alias */
                    return; /* non-ptr reconstitutes */
                }
                /* `(T*)cc_block_on_intptr(task)` / other call forms. */
                {
                    size_t ai = 0;
                    while (after[ai] == ' ' || after[ai] == '\t') ai++;
                    if (shadow_safe_is_word(after[ai])) {
                        while (shadow_safe_is_word(after[ai])) ai++;
                        while (after[ai] == ' ' || after[ai] == '\t') ai++;
                        if (after[ai] == '(') return;
                    }
                }
                /* `(T*)(intptr_t)call(...)` after peel — non-ident heap. */
                if (after[0] == 0 || after[0] == ';') return;
                return;
            }
        }
        /* Casts / field addresses / other exprs: provenance unproven. */
        v->unproven_ptr = 1;
    }
}

static void shadow_safe_on_assign(ShadowSafeCtx* ctx, AstNode* st) {
    ShadowSafeVar* lhs;
    ShadowSafeVar* rhs;
    char lname[64], rname[64], src[64], msg[160];
    if (!ctx || !st || ctx->err) return;
    shadow_safe_check_moved_use(ctx, st, st->a);
    if (ctx->err) return;
    shadow_safe_check_moved_use(ctx, st, st->b);
    if (ctx->err) return;
    if (!shadow_safe_plain_ident(st->a, lname, sizeof(lname))) return;
    lhs = shadow_safe_find(ctx, lname);
    if (!lhs || !lhs->is_slice) return;
    if (strstr(st->b, "cc_move(") != NULL) {
        if (!shadow_safe_parse_cc_move(st->b, src, sizeof(src))) {
            shadow_safe_err_at(ctx, st, "unparseable cc_move(...)");
            return;
        }
        rhs = shadow_safe_find(ctx, src);
        if (!rhs || !rhs->is_slice) {
            snprintf(msg, sizeof(msg), "cannot prove move of '%s'", src);
            shadow_safe_err_at(ctx, st, msg);
            return;
        }
        if (rhs->moved) {
            snprintf(msg, sizeof(msg), "use of moved slice '%s'", src);
            shadow_safe_err_at(ctx, st, msg);
            return;
        }
        rhs->moved = 1;
        lhs->unique = rhs->unique;
        lhs->is_static_slice = rhs->is_static_slice;
        lhs->moved = 0;
        return;
    }
    if (shadow_safe_plain_ident(st->b, rname, sizeof(rname))) {
        rhs = shadow_safe_find(ctx, rname);
        if (rhs && rhs->is_slice) {
            if (rhs->moved) {
                snprintf(msg, sizeof(msg), "use of moved slice '%s'", rname);
                shadow_safe_err_at(ctx, st, msg);
                return;
            }
            if (rhs->unique || lhs->unique) {
                shadow_safe_err_at(ctx, st, "cannot copy unique slice");
                fprintf(stderr,
                        "  hint: unique slices have move-only semantics; use "
                        "cc_move(x) to transfer ownership\n");
            }
        }
    }
}

/* True when `s` is `@string(..., @scratch)` as the whole expression
 * (optional wrapping parens). `f(@string(..., @scratch))` is false — the
 * product is consumed in the callee, not returned. */
static int shadow_safe_expr_is_scratch_at_string(const char* s) {
    const char* p;
    int parens = 0;
    int depth;
    int in_tick;
    if (!s || !s[0]) return 0;
    p = s;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    while (*p == '(') {
        parens++;
        p++;
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    }
    if (strncmp(p, "@string", 7) != 0) return 0;
    p += 7;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    if (*p != '(') return 0;
    depth = 0;
    in_tick = 0;
    for (; *p; p++) {
        if (*p == '`') {
            in_tick = !in_tick;
            continue;
        }
        if (in_tick) continue;
        if (*p == '(')
            depth++;
        else if (*p == ')') {
            depth--;
            if (depth == 0) {
                const char* end = p + 1;
                const char* span = s;
                /* Arena operand is after the last top-level comma. */
                if (strstr(span, "@scratch") == NULL &&
                    strstr(span, "__cc_str_scratch") == NULL)
                    return 0;
                p = end;
                while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
                    p++;
                while (parens > 0 && *p == ')') {
                    parens--;
                    p++;
                    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
                        p++;
                }
                return parens == 0 && *p == 0;
            }
        }
    }
    return 0;
}

static void shadow_safe_scratch_return(ShadowSafeCtx* ctx, AstNode* st,
                                       const char* expr) {
    ShadowSafeVar* v;
    char name[64];
    if (!ctx || !st || ctx->err || !expr || !expr[0]) return;
    if (shadow_safe_plain_ident(expr, name, sizeof(name))) {
        v = shadow_safe_find(ctx, name);
        if (v && v->is_scratch_string)
            shadow_safe_err_at(ctx, st, "@scratch string escapes scope");
        return;
    }
    if (shadow_safe_expr_is_scratch_at_string(expr))
        shadow_safe_err_at(ctx, st, "@scratch string escapes scope");
}

static void shadow_safe_on_return(ShadowSafeCtx* ctx, AstNode* st) {
    ShadowSafeVar* v;
    char name[64];
    if (!ctx || !st || ctx->err) return;
    shadow_safe_check_moved_use(ctx, st, st->a);
    if (ctx->err) return;
    if (shadow_safe_parse_cc_move(st->a, name, sizeof(name))) return;
    if (shadow_safe_plain_ident(st->a, name, sizeof(name))) {
        v = shadow_safe_find(ctx, name);
        if (v && v->is_scratch_string) {
            shadow_safe_err_at(ctx, st, "@scratch string escapes scope");
            return;
        }
        if (v && v->is_slice && v->unique) {
            shadow_safe_err_at(ctx, st, "cannot return unique slice");
            fprintf(stderr,
                    "  hint: unique slices (T[:!]) require explicit ownership "
                    "transfer; use: return cc_move(x)\n");
        }
        return;
    }
    /* `return @string(..., @scratch)` — call-local arena dies with the fn.
     * `return f(@string(..., @scratch))` is a consumed arg, not an escape. */
    shadow_safe_scratch_return(ctx, st, st->a);
    if (!ctx->err && st->kind == AST_RETURN_CC)
        shadow_safe_scratch_return(ctx, st, st->b);
}

static void shadow_safe_on_deleter(ShadowSafeCtx* ctx, AstNode* st,
                                  const char* callee, const char* args) {
    char name[64], msg[192];
    ShadowSafeVar* v;
    if (!ctx || !callee || ctx->err) return;
    if (strcmp(callee, "free") != 0 && strcmp(callee, "cfree") != 0 &&
        strcmp(callee, "cc_slice_destroy") != 0 &&
        strcmp(callee, "CCSlice_destroy") != 0)
        return;
    /* Homemade shared last-drop: `--x.ref` + free/cfree in one fn body. */
    if ((strcmp(callee, "free") == 0 || strcmp(callee, "cfree") == 0) &&
        ctx->cur_fn && shadow_safe_fn_has_refcount_dec(ctx->cur_fn)) {
        shadow_safe_err_at(
            ctx, st,
            "homemade shared last-drop (refcount field '--' + free); use CCArc");
        fprintf(stderr,
                "  note: non-atomic int ref / last-drop races under concurrency "
                "(CVE-2026-10653 class)\n");
        fprintf(stderr,
                "  hint: cc_arc_from_ptr / cc_arc_clone / cc_arc_drop, or don't "
                "share (redis-style drain/join)\n");
        return;
    }
    if (!shadow_safe_first_arg_ident(args, name, sizeof(name))) {
        /* Bare C `free(expr)` is common; only refuse unparseable slice destroy. */
        if (strcmp(callee, "cc_slice_destroy") == 0 ||
            strcmp(callee, "CCSlice_destroy") == 0) {
            shadow_safe_err_at(
                ctx, st,
                "cannot prove slice destroy target (need bare ident)");
        }
        return;
    }
    v = shadow_safe_find(ctx, name);
    if (!v) return;
    if ((v->is_slice && v->arena_view) || v->is_arena_ptr) {
        snprintf(msg, sizeof(msg),
                 "cannot free/destroy non-owning borrow '%s'", name);
        shadow_safe_err_at(ctx, st, msg);
        return;
    }
    if (v->unproven_ptr &&
        (strcmp(callee, "free") == 0 || strcmp(callee, "cfree") == 0)) {
        snprintf(msg, sizeof(msg),
                 "cannot prove free target provenance for '%s'", name);
        shadow_safe_err_at(ctx, st, msg);
        fprintf(stderr,
                "  hint: free only heap owners (malloc/calloc) or let the "
                "arena own cleanup; casts of arena pointers are refused\n");
    }
}

static void shadow_safe_on_arena_epoch(ShadowSafeCtx* ctx, AstNode* st,
                                      const char* callee, const char* args) {
    char arena[64], msg[256];
    const char* op;
    if (!ctx || !callee || ctx->err) return;
    if (strcmp(callee, "cc_arena_reset") == 0)
        op = "cc_arena_reset";
    else if (strcmp(callee, "cc_arena_free") == 0)
        op = "cc_arena_free";
    else if (strcmp(callee, "cc_arena_destroy") == 0)
        op = "cc_arena_destroy";
    else
        return;
    if (!shadow_safe_first_arg_ident(args, arena, sizeof(arena))) {
        shadow_safe_err_at(
            ctx, st, "cannot prove arena argument for epoch op (need bare ident)");
        return;
    }
    if (shadow_safe_arena_pinned(ctx, arena)) {
        snprintf(msg, sizeof(msg),
                 "cannot %s arena '%s' while a nursery task holds a borrow "
                 "from it",
                 op, arena);
        shadow_safe_err_at(ctx, st, msg);
        fprintf(stderr,
                "  note: capturing an arena borrow into a spawn pins that "
                "arena epoch until the nursery scope ends\n");
        return;
    }
    if (strcmp(op, "cc_arena_reset") == 0 &&
        shadow_safe_live_borrow_on(ctx, arena)) {
        snprintf(msg, sizeof(msg),
                 "cannot cc_arena_reset arena '%s' while a derived slice "
                 "borrow is still in scope",
                 arena);
        shadow_safe_err_at(ctx, st, msg);
        fprintf(stderr,
                "  note: arena provenance epoch would invalidate live borrows "
                "(CVE-2017-13245 class)\n");
    }
}

static ShadowSafeStruct* shadow_safe_find_struct(ShadowSafeCtx* ctx,
                                                const char* name) {
    int i;
    if (!ctx || !name || !name[0] || !ctx->structs) return NULL;
    for (i = 0; i < ctx->nstructs; i++)
        if (strcmp(ctx->structs[i].name, name) == 0) return &ctx->structs[i];
    return NULL;
}

static void shadow_safe_strip_ty(const char* in, char* out, size_t cap) {
    const char* p = in;
    size_t n = 0;
    if (!out || !cap) return;
    out[0] = 0;
    if (!p) return;
    while (*p == ' ' || *p == '\t') p++;
    if (strncmp(p, "const ", 6) == 0) p += 6;
    while (*p == ' ' || *p == '\t') p++;
    if (strncmp(p, "struct ", 7) == 0) p += 7;
    while (*p && *p != '*' && *p != ' ' && *p != '\t' && n + 1 < cap)
        out[n++] = *p++;
    out[n] = 0;
}

/* `@typeview on T { as: field; }` — mark matching struct fields so cycle /
 * ambiguous / reflect is_as see the same faces as UFCS (g_shadow_as). */
static int shadow_typeview_body_as_names(const char* body, char names[][64],
                                         int cap) {
    const char* p = body ? body : "";
    int in_as = 0;
    int n = 0;
    if (!names || cap <= 0) return 0;
    while (*p) {
        char tok[64];
        int i = 0;
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' ||
               *p == '{' || *p == '}' || *p == ';' || *p == ',' ||
               *p == '(' || *p == ')')
            p++;
        if (!*p) break;
        if (*p == ':') {
            p++;
            continue;
        }
        while (*p && (shadow_safe_is_word(*p) || *p == '*')) {
            if (i + 1 < (int)sizeof(tok)) tok[i++] = *p;
            p++;
        }
        tok[i] = 0;
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
        if (*p == ':') {
            p++;
            in_as = (strcmp(tok, "as") == 0);
            continue;
        }
        if (in_as && tok[0] && n < cap) {
            snprintf(names[n], 64, "%s", tok);
            n++;
        }
    }
    return n;
}

static void shadow_mark_struct_as_fields(AstNode* st, char names[][64], int nn) {
    int i, k;
    if (!st || !names || nn <= 0) return;
    for (i = 0; i < st->nkids; i++) {
        AstNode* f = st->kids[i];
        if (!f || f->kind != AST_FIELD_SIMPLE || !f->b[0]) continue;
        {
            const char* np = f->b;
            char one[64];
            int hit = 0;
            while (!hit && shadow_field_next_name(&np, one, sizeof(one))) {
                char* br = strchr(one, '[');
                if (br) *br = 0;
                for (k = 0; k < nn; k++) {
                    if (strcmp(one, names[k]) == 0) {
                        snprintf(f->e, sizeof(f->e), "as");
                        hit = 1;
                        break;
                    }
                }
            }
        }
    }
}

static void shadow_apply_typeview_as_faces(AstNode** items, int n) {
    int i, j;
    if (!items) return;
    for (i = 0; i < n; i++) {
        AstNode* it = items[i];
        char names[32][64];
        int nn;
        if (!it) continue;
        if (it->kids && it->nkids > 0)
            shadow_apply_typeview_as_faces(it->kids, it->nkids);
        if (it->kind != AST_AT_STMT || strcmp(it->a, "typeview") != 0 || !it->d[0])
            continue;
        nn = shadow_typeview_body_as_names(it->c, names, 32);
        if (nn <= 0) continue;
        for (j = 0; j < n; j++) {
            AstNode* st = items[j];
            if (!st) continue;
            if (st->kind == AST_TYPEDEF_STRUCT &&
                ((st->b[0] && strcmp(st->b, it->d) == 0) ||
                 (st->a[0] && strcmp(st->a, it->d) == 0)))
                shadow_mark_struct_as_fields(st, names, nn);
            else if (st->kind == AST_STRUCT && st->a[0] &&
                     strcmp(st->a, it->d) == 0)
                shadow_mark_struct_as_fields(st, names, nn);
        }
    }
}

static int shadow_safe_register_struct(ShadowSafeCtx* ctx, AstNode* st) {
    ShadowSafeStruct* s;
    int i;
    if (!ctx || !st || st->kind != AST_TYPEDEF_STRUCT || !st->b[0]) return 0;
    if (ctx->nstructs >= SHADOW_SAFE_STRUCT_CAP) {
        shadow_safe_err_at(ctx, st,
                           "safety struct table capacity exceeded (64)");
        return 0;
    }
    if (shadow_safe_find_struct(ctx, st->b)) return 0;
    s = &ctx->structs[ctx->nstructs++];
    memset(s, 0, sizeof(*s));
    snprintf(s->name, sizeof(s->name), "%s", st->b);
    for (i = 0; i < st->nkids && s->nf < SHADOW_SAFE_FIELD_CAP; i++) {
        AstNode* f = st->kids[i];
        if (!f || f->kind != AST_FIELD_SIMPLE || !f->a[0]) continue;
        /* Anonymous / raw @as cannot be proven as a named embed. */
        if (strcmp(f->e, "raw") == 0 &&
            (strstr(f->a, "@as") != NULL || strstr(f->a, "/*@as*/") != NULL)) {
            shadow_safe_err_at(ctx, f, "type: anonymous as: field is ill-formed");
            return 0;
        }
        if (strcmp(f->e, "as") == 0 && !f->b[0]) {
            shadow_safe_err_at(ctx, f, "type: anonymous as: field is ill-formed");
            return 0;
        }
        if (!f->b[0]) continue;
        {
            const char* np = f->b;
            char one[64];
            while (s->nf < SHADOW_SAFE_FIELD_CAP &&
                   shadow_field_next_name(&np, one, sizeof(one))) {
                char* br = strchr(one, '[');
                if (br) *br = 0;
                snprintf(s->fname[s->nf], sizeof(s->fname[0]), "%s", one);
                snprintf(s->fty[s->nf], sizeof(s->fty[0]), "%s", f->a);
                s->is_as[s->nf] = (strcmp(f->e, "as") == 0);
                s->nf++;
            }
        }
    }
    return 1;
}

static ShadowSafeVariant* shadow_safe_find_variant(ShadowSafeCtx* ctx,
                                                  const char* name) {
    int i;
    if (!ctx || !name || !name[0] || !ctx->variants) return NULL;
    for (i = 0; i < ctx->nvariants; i++)
        if (strcmp(ctx->variants[i].name, name) == 0) return &ctx->variants[i];
    return NULL;
}

/* Parse `@variant` body text `arm: Type; …` into a safety table entry. */
static int shadow_safe_register_variant(ShadowSafeCtx* ctx, AstNode* st) {
    ShadowSafeVariant* v;
    const char* p;
    if (!ctx || !st || st->kind != AST_AT_STMT) return 0;
    if (strcmp(st->a, "variant") != 0 || !st->b[0]) return 0;
    if (shadow_safe_find_variant(ctx, st->b)) return 0;
    if (ctx->nvariants >= SHADOW_SAFE_VARIANT_CAP) {
        shadow_safe_err_at(ctx, st,
                           "safety variant table capacity exceeded (32)");
        return 0;
    }
    v = &ctx->variants[ctx->nvariants++];
    memset(v, 0, sizeof(*v));
    snprintf(v->name, sizeof(v->name), "%s", st->b);
    if (st->d[0] && strcmp(st->d, "packed") == 0)
        v->is_packed = 1;
    p = st->c;
    while (p && *p && v->narm < SHADOW_SAFE_VARM_CAP) {
        char arm[32];
        size_t n = 0;
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == '{' ||
               *p == '}')
            p++;
        if (!*p) break;
        if (!shadow_safe_is_word(*p) || (*p >= '0' && *p <= '9')) {
            while (*p && *p != '\n' && *p != ';') p++;
            if (*p == ';') p++;
            continue;
        }
        while (shadow_safe_is_word(*p) && n + 1 < sizeof(arm)) arm[n++] = *p++;
        arm[n] = 0;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == ':' && arm[0] && strcmp(arm, "kind") != 0 &&
            strcmp(arm, "u") != 0) {
            snprintf(v->arms[v->narm++], sizeof(v->arms[0]), "%s", arm);
        }
        while (*p && *p != ';' && *p != '\n') p++;
        if (*p == ';') p++;
    }
    return 1;
}

extern int cc_variant_schema_pending_count(void);
extern const char* cc_variant_schema_pending_name(int i);
extern int cc_variant_schema_pending_narms(int i);
extern const char* cc_variant_schema_pending_arm(int i, int a);

static void shadow_safe_register_schema_variants(ShadowSafeCtx* ctx) {
    int ni = cc_variant_schema_pending_count();
    int i, a;
    if (!ctx) return;
    for (i = 0; i < ni; i++) {
        ShadowSafeVariant* v;
        const char* name = cc_variant_schema_pending_name(i);
        int narm = cc_variant_schema_pending_narms(i);
        if (!name || !name[0] || narm <= 0) continue;
        if (shadow_safe_find_variant(ctx, name)) continue;
        if (ctx->nvariants >= SHADOW_SAFE_VARIANT_CAP) {
            shadow_safe_err_at(ctx, NULL,
                               "safety variant table capacity exceeded (32)");
            return;
        }
        v = &ctx->variants[ctx->nvariants++];
        memset(v, 0, sizeof(*v));
        snprintf(v->name, sizeof(v->name), "%s", name);
        v->is_schema = 1;
        for (a = 0; a < narm && a < SHADOW_SAFE_VARM_CAP; a++) {
            snprintf(v->arms[v->narm++], sizeof(v->arms[0]), "%s",
                     cc_variant_schema_pending_arm(i, a));
        }
        shadow_safe_add_type_name(ctx, name);
    }
}

typedef struct {
    char type[64];
    char path[128];
} ShadowSafeAsSeen;

static void shadow_safe_as_walk(ShadowSafeCtx* ctx, AstNode* at,
                                const char* root, const char* cur,
                                const char* path_prefix, ShadowSafeAsSeen* seen,
                                int* nseen) {
    ShadowSafeStruct* s;
    int i;
    char msg[256];
    if (!ctx || ctx->err || !cur || !cur[0] || !seen || !nseen) return;
    s = shadow_safe_find_struct(ctx, cur);
    if (!s) return;
    for (i = 0; i < s->nf; i++) {
        char leaf[64];
        char path[128];
        int k;
        if (!s->is_as[i]) continue;
        if (strchr(s->fty[i], '*') != NULL) {
            snprintf(msg, sizeof(msg),
                     "type: as: field '%s' on '%s' must be a value embed, "
                     "not a pointer",
                     s->fname[i], s->name);
            shadow_safe_err_at(ctx, at, msg);
            return;
        }
        shadow_safe_strip_ty(s->fty[i], leaf, sizeof(leaf));
        if (!leaf[0]) continue;
        if (path_prefix && path_prefix[0])
            snprintf(path, sizeof(path), "%s.%s", path_prefix, s->fname[i]);
        else
            snprintf(path, sizeof(path), "%s", s->fname[i]);
        /* Self-embed (or return to root) is a cycle, not path ambiguity. */
        if (root && strcmp(leaf, root) == 0) {
            shadow_safe_err_at(ctx, at, "cyclic as:");
            return;
        }
        for (k = 0; k < *nseen; k++) {
            if (strcmp(seen[k].type, leaf) == 0) {
                if (strcmp(seen[k].path, path) != 0) {
                    snprintf(msg, sizeof(msg),
                             "type: ambiguous as: embed of '%s' on '%s'", leaf,
                             root);
                    shadow_safe_err_at(ctx, at, msg);
                    return;
                }
                break;
            }
        }
        if (k == *nseen) {
            if (*nseen >= SHADOW_SAFE_AS_SEEN_CAP) {
                shadow_safe_err_at(
                    ctx, at, "safety @as embed table capacity exceeded (32)");
                return;
            }
            snprintf(seen[*nseen].type, sizeof(seen[0].type), "%s", leaf);
            snprintf(seen[*nseen].path, sizeof(seen[0].path), "%s", path);
            (*nseen)++;
        }
        shadow_safe_as_walk(ctx, at, root, leaf, path, seen, nseen);
        if (ctx->err) return;
    }
}

static void shadow_safe_check_as_embeds(ShadowSafeCtx* ctx, AstNode** items,
                                        int n) {
    int i;
    if (!ctx || !items) return;
    for (i = 0; i < n && !ctx->err; i++) {
        AstNode* it = items[i];
        ShadowSafeAsSeen seen[SHADOW_SAFE_AS_SEEN_CAP];
        int nseen = 0;
        if (!it || it->kind != AST_TYPEDEF_STRUCT || !it->b[0]) continue;
        shadow_safe_as_walk(ctx, it, it->b, it->b, "", seen, &nseen);
    }
}

/* Whole-word match; rejects `cc_chan_send` matching `chan_send`. */
static int shadow_safe_text_has_word(const char* text, const char* word) {
    const char* p;
    size_t n;
    if (!text || !word || !word[0]) return 0;
    n = strlen(word);
    p = text;
    while ((p = strstr(p, word)) != NULL) {
        char before = (p > text) ? p[-1] : 0;
        char after = p[n];
        if (!shadow_safe_is_word(before) && !shadow_safe_is_word(after))
            return 1;
        p += n;
    }
    return 0;
}

static void shadow_safe_check_async_chan_text(ShadowSafeCtx* ctx, AstNode* st,
                                             const char* text) {
    const char* op = NULL;
    char msg[160];
    if (!ctx || !ctx->in_async || !text || ctx->err) return;
    if (shadow_safe_text_has_word(text, "chan_send"))
        op = "chan_send";
    else if (shadow_safe_text_has_word(text, "chan_recv"))
        op = "chan_recv";
    else
        return;
    snprintf(msg, sizeof(msg),
             "async: channel operation '%s' must be awaited in @async function",
             op);
    shadow_safe_err_at(ctx, st, msg);
}

/* Second top-level call arg → text span (trimmed). Returns 1 on success. */
static int shadow_safe_call_arg1_span(const char* args, char* out, size_t cap) {
    const char* p;
    const char* start;
    const char* end;
    int depth = 0;
    size_t n = 0;
    if (!args || !out || !cap) return 0;
    out[0] = 0;
    p = args;
    while (*p) {
        if (*p == '(' || *p == '[' || *p == '{') depth++;
        else if (*p == ')' || *p == ']' || *p == '}') {
            if (depth > 0) depth--;
        } else if (*p == ',' && depth == 0) {
            p++;
            break;
        }
        p++;
    }
    while (*p == ' ' || *p == '\t') p++;
    /* Allow `&ident` stack aliases — proven below via payload_expr_ok /
     * amp-ident path (channel copies the pointer bits by value). */
    if (!*p) return 0;
    start = p;
    depth = 0;
    while (*p) {
        if (*p == '(' || *p == '[' || *p == '{') depth++;
        else if (*p == ')' || *p == ']' || *p == '}') {
            if (depth == 0 && *p == ')') break;
            if (depth > 0) depth--;
        } else if (*p == ',' && depth == 0)
            break;
        p++;
    }
    end = p;
    while (end > start && (end[-1] == ' ' || end[-1] == '\t')) end--;
    n = (size_t)(end - start);
    if (n == 0 || n + 1 > cap) return 0;
    memcpy(out, start, n);
    out[n] = 0;
    return 1;
}

/* Second top-level call arg → bare ident (beachhead). */
static int shadow_safe_call_arg1_ident(const char* args, char* out, size_t cap) {
    const char* p;
    size_t n;
    if (!shadow_safe_call_arg1_span(args, out, cap)) return 0;
    p = out;
    if (!shadow_safe_is_word(*p)) return 0;
    while (*p && shadow_safe_is_word(*p)) p++;
    if (*p) return 0; /* not a bare ident */
    n = strlen(out);
    return n > 0;
}

/* Scalar arithmetic / field payload: literals + tracked POD idents only.
 * Leading `&ident` is a stack alias (channel copies pointer bits by value). */
static int shadow_safe_payload_expr_ok(ShadowSafeCtx* ctx, const char* expr) {
    const char* p = expr ? expr : "";
    char id[64];
    size_t n;
    ShadowSafeVar* v;
    int addr_of = 0;
    if (!ctx || !expr || !expr[0]) return 0;
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '&') {
        addr_of = 1;
        p++;
        while (*p == ' ' || *p == '\t') p++;
    }
    while (*p) {
        if (*p == ' ' || *p == '\t' || *p == '+' || *p == '-' || *p == '*' ||
            *p == '/' || *p == '%' || *p == '(' || *p == ')' || *p == '>' ||
            *p == '<' || *p == '|' || *p == '&' || *p == '^' || *p == '~' ||
            *p == '!') {
            p++;
            continue;
        }
        if (*p >= '0' && *p <= '9') {
            p++;
            while ((*p >= '0' && *p <= '9') || *p == '.' || *p == 'u' ||
                   *p == 'U' || *p == 'l' || *p == 'L' || *p == 'e' ||
                   *p == 'E' || *p == '+' || *p == '-')
                p++;
            continue;
        }
        if (*p == '"' || *p == '\'') {
            char q = *p++;
            while (*p && *p != q) {
                if (*p == '\\' && p[1]) p++;
                p++;
            }
            if (*p == q) p++;
            continue;
        }
        if (shadow_safe_is_word(*p)) {
            n = 0;
            while (*p && shadow_safe_is_word(*p)) {
                if (n + 1 < sizeof(id)) id[n++] = *p;
                p++;
            }
            id[n] = 0;
            /* `ident->field` / `ident.field` — prove leaf via base var. */
            if (*p == '-' && p[1] == '>') {
                p += 2;
                while (*p && shadow_safe_is_word(*p)) p++;
                v = shadow_safe_find(ctx, id);
                if (!v) return 0;
                /* Scalar field through pointer/value receiver is channel-stable. */
                continue;
            }
            if (*p == '.') {
                p++;
                while (*p && shadow_safe_is_word(*p)) p++;
                v = shadow_safe_find(ctx, id);
                if (!v) return 0;
                continue;
            }
            v = shadow_safe_find(ctx, id);
            if (!v) return 0;
            /* `&ptr` / `&local` — POD address bits; bare ptr/slice still refuse. */
            if (!addr_of && (v->is_ptr || v->is_slice)) return 0;
            continue;
        }
        return 0;
    }
    return 1;
}

static void shadow_safe_emit_channel_stable_hints(const char* payload) {
    fprintf(stderr,
            "  note: non-unique slices (arena / stack / untracked "
            "from_buffer) may dangle after send (channel-stable-borrow)\n"
            "  hint: include a CCArena in the same message, use unique "
            "T[:!] / cc_slice_from_static, or use send_into / "
            "try_send_into\n");
    /* Match production checker: channel macros copy by value into __cc_tmp. */
    (void)payload;
    fprintf(stderr,
            "  note: '__cc_tmp' is the channel send macro's by-value copy of "
            "the payload\n");
}

static void shadow_safe_check_chan_send_payload(ShadowSafeCtx* ctx, AstNode* st,
                                               const char* callee,
                                               const char* args) {
    char payload[64];
    char leaf[64];
    char msg[256];
    ShadowSafeVar* v;
    ShadowSafeStruct* s;
    int i;
    int has_slice = 0;
    int has_arena = 0;
    if (!ctx || !st || ctx->err || !callee || !args) return;
    if (strcmp(callee, "cc_channel_send") != 0 &&
        strcmp(callee, "chan_send") != 0)
        return;
    /* Bare ident, or scalar arithmetic / field expr over tracked POD. */
    if (!shadow_safe_call_arg1_ident(args, payload, sizeof(payload))) {
        char span[128];
        if (shadow_safe_call_arg1_span(args, span, sizeof(span)) &&
            shadow_safe_payload_expr_ok(ctx, span))
            return;
        shadow_safe_err_at(
            ctx, st,
            "cannot prove channel-send payload (need bare identifier)");
        return;
    }
    /* Scalar / string literals are channel-stable POD (no provenance). */
    {
        const char* lit = payload;
        int is_num = 0;
        if (lit[0] == '-' || lit[0] == '+') lit++;
        if (lit[0] >= '0' && lit[0] <= '9') {
            const char* q = lit;
            while (*q >= '0' && *q <= '9') q++;
            if (*q == '.' || *q == 'e' || *q == 'E' || *q == 'u' || *q == 'U' ||
                *q == 'l' || *q == 'L' || *q == 0)
                is_num = 1;
        }
        if (is_num || ((payload[0] == '"' || payload[0] == '\'') &&
                       payload[strlen(payload) - 1] == payload[0]))
            return;
    }
    v = shadow_safe_find(ctx, payload);
    if (!v) {
        snprintf(msg, sizeof(msg),
                 "cannot send untracked value '%s' on a channel", payload);
        shadow_safe_err_at(ctx, st, msg);
        shadow_safe_emit_channel_stable_hints(payload);
        return;
    }
    if (v->is_ptr) {
        /* T* payload copies the pointer bits (ordered-data / take channels).
         * Refuse arena borrows and unproven provenance; stack aliases (`&x`)
         * are allowed as POD pointer values (lifetime is the caller's). */
        if (v->unproven_ptr || v->is_arena_ptr) {
            snprintf(msg, sizeof(msg),
                     "cannot send pointer '%s' on a channel", payload);
            shadow_safe_err_at(ctx, st, msg);
            return;
        }
        return;
    }
    /* Bare non-unique / non-static slice borrow — match production checker. */
    if (v->is_slice && !shadow_safe_slice_channel_stable(v)) {
        snprintf(msg, sizeof(msg),
                 "cannot send non-stable slice borrow '%s' on a channel",
                 payload);
        shadow_safe_err_at(ctx, st, msg);
        shadow_safe_emit_channel_stable_hints(payload);
        return;
    }
    if (!v->ty[0]) return;
    shadow_safe_strip_ty(shadow_safe_resolve_td(ctx, v->ty), leaf, sizeof(leaf));
    s = shadow_safe_find_struct(ctx, leaf);
    if (!s) {
        /* Scalars / slices already handled; unknown aggregates refuse. */
        if (strcmp(leaf, "int") == 0 || strcmp(leaf, "char") == 0 ||
            strcmp(leaf, "bool") == 0 || strcmp(leaf, "size_t") == 0 ||
            strcmp(leaf, "void") == 0 || strcmp(leaf, "long") == 0 ||
            strcmp(leaf, "short") == 0 || strcmp(leaf, "float") == 0 ||
            strcmp(leaf, "double") == 0 || strcmp(leaf, "int64_t") == 0 ||
            strcmp(leaf, "uint64_t") == 0 || strcmp(leaf, "int32_t") == 0 ||
            strcmp(leaf, "uint32_t") == 0 || strcmp(leaf, "unsigned") == 0 ||
            strcmp(leaf, "signed") == 0 || strcmp(leaf, "intptr_t") == 0 ||
            strcmp(leaf, "uintptr_t") == 0 ||
            /* Task handles are channel-stable POD (spawn-into / ordered). */
            strcmp(leaf, "CCTask") == 0 ||
            /* Typed Results are POD envelopes (ok + union). */
            strncmp(leaf, "CCResult_", 9) == 0 ||
            shadow_safe_type_is_slice(leaf) ||
            shadow_safe_type_is_arena(leaf) ||
            shadow_safe_find_variant(ctx, leaf))
            return;
        snprintf(msg, sizeof(msg),
                 "cannot prove channel-send type '%s'", leaf);
        shadow_safe_err_at(ctx, st, msg);
        return;
    }
    for (i = 0; i < s->nf; i++) {
        const char* fty = s->fty[i];
        if (strchr(fty, '*') != NULL) {
            shadow_safe_err_at(
                ctx, st,
                "cannot send value containing a raw pointer field on a channel");
            return;
        }
        if (strcmp(fty, "CCSlice") == 0) has_slice = 1;
        if (strcmp(fty, "CCArena") == 0) has_arena = 1;
    }
    /* Aggregate with CCSlice and no sibling CCArena: reject unless proven
     * unique/static (do not require the arena-view name heuristic). */
    if (has_slice && !has_arena &&
        (v->nonstable_slice || !v->unique)) {
        snprintf(msg, sizeof(msg),
                 "cannot send '%s' (type '%s'): non-stable slice field "
                 "without a CCArena",
                 payload, leaf[0] ? leaf : "struct");
        shadow_safe_err_at(ctx, st, msg);
        shadow_safe_emit_channel_stable_hints(payload);
        return;
    }
}

static void shadow_safe_check_channel_pair_text(ShadowSafeCtx* ctx, AstNode* st,
                                               const char* text) {
    const char* call;
    const char* a;
    char tx[64], rx[64];
    size_t i;
    ShadowSafeVar* vtx;
    ShadowSafeVar* vrx;
    FileTape* ft;
    size_t off;
    int line = 1, col = 1;
    if (!ctx || !st || !text || ctx->err) return;
    call = strstr(text, "cc_channel_pair(");
    if (!call) return;
    /* Do not match create_named / other helpers. */
    if (call > text && shadow_safe_is_word(call[-1])) return;
    a = call + strlen("cc_channel_pair(");
    while (*a == ' ' || *a == '\t') a++;
    if (*a != '&') return;
    a++;
    i = 0;
    while (*a && *a != ',' && *a != ' ' && *a != '\t' && i + 1 < sizeof(tx))
        tx[i++] = *a++;
    tx[i] = 0;
    while (*a == ' ' || *a == '\t') a++;
    if (*a != ',') return;
    a++;
    while (*a == ' ' || *a == '\t') a++;
    if (*a != '&') return;
    a++;
    i = 0;
    while (*a && *a != ')' && *a != ' ' && *a != '\t' && i + 1 < sizeof(rx))
        rx[i++] = *a++;
    rx[i] = 0;
    if (!tx[0] || !rx[0]) return;
    vtx = shadow_safe_find(ctx, tx);
    vrx = shadow_safe_find(ctx, rx);
    if (vtx && vtx->is_chan == 1 && vrx && vrx->is_chan == 2) return;
    /* Origin at `cc_channel_pair` spelling in the source tape. */
    ctx->err = 1;
    if (!ctx->cache || !st->file_id) {
        fprintf(stderr, "error: channel: cc_channel_pair\n");
        return;
    }
    ft = tape_by_id(ctx->cache, st->file_id);
    if (!ft || !ft->bytes) {
        fprintf(stderr, "error: channel: cc_channel_pair\n");
        return;
    }
    off = st->tok_off;
    if (off < ft->len) {
        const char* hit = strstr(ft->bytes + off, "cc_channel_pair");
        if (!hit && off > 0) {
            size_t back = off > 80 ? off - 80 : 0;
            hit = strstr(ft->bytes + back, "cc_channel_pair");
        }
        if (hit && (size_t)(hit - ft->bytes) < ft->len)
            off = (size_t)(hit - ft->bytes);
    }
    offset_to_linecol(ft, off, &line, &col);
    fprintf(stderr, "%s:%d:%d: error: channel: cc_channel_pair\n", ft->path,
            line, col);
}

static void shadow_safe_on_chan_send_text(ShadowSafeCtx* ctx, AstNode* st,
                                         const char* text) {
    const char* p;
    char callee[32];
    if (!ctx || !st || !text || ctx->err) return;
    shadow_safe_check_channel_pair_text(ctx, st, text);
    if (ctx->err) return;
    p = text;
    while (*p) {
        const char* open;
        size_t clen = 0;
        int depth;
        char args[192];
        size_t ai = 0;
        while (*p && !shadow_safe_is_word(*p)) p++;
        if (!*p) break;
        if (strncmp(p, "cc_channel_send", 15) == 0 &&
            !shadow_safe_is_word(p[15])) {
            snprintf(callee, sizeof(callee), "cc_channel_send");
            clen = 15;
        } else if (strncmp(p, "chan_send", 9) == 0 &&
                   !shadow_safe_is_word(p[9]) &&
                   (p == text || !shadow_safe_is_word(p[-1]))) {
            snprintf(callee, sizeof(callee), "chan_send");
            clen = 9;
        } else {
            while (shadow_safe_is_word(*p)) p++;
            continue;
        }
        open = p + clen;
        while (*open == ' ' || *open == '\t') open++;
        if (*open != '(') {
            p += clen;
            continue;
        }
        depth = 0;
        ai = 0;
        open++;
        while (*open && ai + 1 < sizeof(args)) {
            if (*open == '(') depth++;
            else if (*open == ')') {
                if (depth == 0) break;
                depth--;
            }
            args[ai++] = *open++;
        }
        args[ai] = 0;
        shadow_safe_check_chan_send_payload(ctx, st, callee, args);
        if (ctx->err) return;
        p = open;
    }
}

static void shadow_safe_on_call(ShadowSafeCtx* ctx, AstNode* st) {
    if (!ctx || !st || ctx->err) return;
    shadow_safe_scan_texts(ctx, st);
    if (ctx->err) return;
    shadow_safe_check_async_chan_text(ctx, st, st->a);
    if (ctx->err) return;
    shadow_safe_check_chan_send_payload(ctx, st, st->a, st->b);
    if (ctx->err) return;
    shadow_safe_on_deleter(ctx, st, st->a, st->b);
    if (ctx->err) return;
    shadow_safe_on_arena_epoch(ctx, st, st->a, st->b);
}

static void shadow_safe_on_ufcs(ShadowSafeCtx* ctx, AstNode* st) {
    char msg[192];
    char args[192];
    ShadowSafeVar* v;
    if (!ctx || !st || ctx->err) return;
    shadow_safe_scan_texts(ctx, st);
    if (ctx->err) return;
    /* UFCS channel send: recv.send(payload) — same oracle as cc_channel_send.
     * `!>` / `!>(e){…}` on the UFCS node (st->d starts with bang*) awaits. */
    if (strcmp(st->b, "send") == 0) {
        snprintf(args, sizeof(args), "%s, %s", st->a, st->c);
        shadow_safe_check_chan_send_payload(ctx, st, "cc_channel_send", args);
        if (ctx->err) return;
        if (ctx->in_async &&
            !(st->d[0] && strncmp(st->d, "bang", 4) == 0)) {
            shadow_safe_err_at(
                ctx, st,
                "async: channel operation 'chan_send' must be awaited in "
                "@async function");
        }
        return;
    }
    if (strcmp(st->b, "recv") == 0 && ctx->in_async &&
        !(st->d[0] && strncmp(st->d, "bang", 4) == 0)) {
        shadow_safe_err_at(
            ctx, st,
            "async: channel operation 'chan_recv' must be awaited in "
            "@async function");
        return;
    }
    if (strcmp(st->b, "destroy") != 0 && strcmp(st->b, "free") != 0 &&
        strcmp(st->b, "reset") != 0)
        return;
    v = shadow_safe_find(ctx, st->a);
    if (!v) return;
    if ((strcmp(st->b, "destroy") == 0 || strcmp(st->b, "free") == 0) &&
        ((v->is_slice && v->arena_view) || v->is_arena_ptr)) {
        snprintf(msg, sizeof(msg),
                 "cannot free/destroy non-owning borrow '%s'", st->a);
        shadow_safe_err_at(ctx, st, msg);
        return;
    }
    if (v->is_arena &&
        (strcmp(st->b, "reset") == 0 || strcmp(st->b, "free") == 0 ||
         strcmp(st->b, "destroy") == 0)) {
        snprintf(args, sizeof(args), "&%s", st->a);
        if (strcmp(st->b, "reset") == 0)
            shadow_safe_on_arena_epoch(ctx, st, "cc_arena_reset", args);
        else if (strcmp(st->b, "free") == 0)
            shadow_safe_on_arena_epoch(ctx, st, "cc_arena_free", args);
        else
            shadow_safe_on_arena_epoch(ctx, st, "cc_arena_destroy", args);
    }
}

static int shadow_safe_stmt_diverges(AstNode* st) {
    if (!st) return 0;
    if (st->kind == AST_EXPR_STMT && st->a[0]) {
        const char* p = st->a;
        while (*p == ' ' || *p == '\t') p++;
        if (strncmp(p, "goto", 4) == 0) {
            const char* q = p + 4;
            while (*q == ' ' || *q == '\t') q++;
            if (*q) return 1;
        }
    }
    if (st->kind == AST_RETURN_EXPR || st->kind == AST_RETURN_INT ||
        st->kind == AST_RETURN_CC || st->kind == AST_BREAK ||
        st->kind == AST_CONTINUE || st->kind == AST_GOTO ||
        st->kind == AST_ERR_FWD)
        return 1;
    if (st->kind == AST_CALL_ARGS || st->kind == AST_CALL_NUM) {
        if (strcmp(st->a, "exit") == 0 || strcmp(st->a, "abort") == 0 ||
            strcmp(st->a, "_exit") == 0 || strcmp(st->a, "longjmp") == 0 ||
            strcmp(st->a, "cc_error_exit") == 0 || strcmp(st->a, "goto") == 0)
            return 1;
    }
    if (st->kind == AST_BLOCK && st->nbody > 0)
        return shadow_safe_stmt_diverges(st->body[st->nbody - 1]);
    return 0;
}

static int shadow_safe_body_diverges(AstNode** body, int n) {
    if (!body || n <= 0) return 0;
    return shadow_safe_stmt_diverges(body[n - 1]);
}

static AstNode* shadow_safe_eh(ShadowSafeCtx* ctx) {
    if (!ctx || ctx->neh <= 0) return NULL;
    return ctx->eh_stack[ctx->neh - 1];
}

static void shadow_safe_push_eh(ShadowSafeCtx* ctx, AstNode* eh) {
    if (!ctx || !eh) return;
    if (ctx->neh >= SHADOW_SAFE_EH_CAP) {
        shadow_safe_err_at(ctx, eh,
                           "safety @errhandler stack capacity exceeded (16)");
        return;
    }
    ctx->eh_scopes[ctx->neh] = ctx->eh_scope;
    ctx->eh_stack[ctx->neh++] = eh;
}

static int shadow_safe_rfn_reserve(ShadowSafeCtx* ctx, int need) {
    ShadowSafeRfn* nbuf;
    int ncap;
    if (!ctx || !ctx->safe_ar) return 0;
    if (need <= ctx->rfn_cap) return 1;
    ncap = ctx->rfn_cap ? ctx->rfn_cap * 2 : 16;
    while (ncap < need) ncap *= 2;
    nbuf = (ShadowSafeRfn*)cc_arena_alloc(ctx->safe_ar,
                                         (size_t)ncap * sizeof(ShadowSafeRfn),
                                         _Alignof(ShadowSafeRfn));
    if (!nbuf) return 0;
    if (ctx->rfns && ctx->nrfn > 0)
        memcpy(nbuf, ctx->rfns, (size_t)ctx->nrfn * sizeof(ShadowSafeRfn));
    ctx->rfns = nbuf;
    ctx->rfn_cap = ncap;
    return 1;
}

static int shadow_safe_is_result_fn(ShadowSafeCtx* ctx, const char* name) {
    int i;
    if (!ctx || !name || !ctx->rfns) return 0;
    for (i = 0; i < ctx->nrfn; i++)
        if (strcmp(ctx->rfns[i].name, name) == 0) return 1;
    return 0;
}

static void shadow_safe_add_result_fn_err(ShadowSafeCtx* ctx, const char* name,
                                         const char* err_ty) {
    int i;
    if (!ctx || !name || !name[0]) return;
    for (i = 0; i < ctx->nrfn; i++) {
        if (strcmp(ctx->rfns[i].name, name) == 0) {
            if (err_ty && err_ty[0] && !ctx->rfns[i].err[0])
                snprintf(ctx->rfns[i].err, sizeof(ctx->rfns[i].err),
                         "%s", err_ty);
            return;
        }
    }
    if (!shadow_safe_rfn_reserve(ctx, ctx->nrfn + 1)) {
        shadow_safe_err_at(ctx, NULL,
                           "safety result-fn table grow failed");
        return;
    }
    snprintf(ctx->rfns[ctx->nrfn].name, sizeof(ctx->rfns[0].name), "%s",
             name);
    ctx->rfns[ctx->nrfn].err[0] = 0;
    if (err_ty && err_ty[0])
        snprintf(ctx->rfns[ctx->nrfn].err, sizeof(ctx->rfns[0].err), "%s",
                 err_ty);
    ctx->nrfn++;
}
static void __attribute__((unused)) shadow_safe_add_result_fn(ShadowSafeCtx* ctx,
                                                              const char* name) {
    shadow_safe_add_result_fn_err(ctx, name, NULL);
}
static const char* shadow_safe_result_fn_err(ShadowSafeCtx* ctx,
                                            const char* name);
static int shadow_safe_as_face_of(ShadowSafeCtx* ctx, const char* outer,
                                  const char* face);

/* Outermost call ident — `recv.method(` / `outer(inner())`. */
static void shadow_safe_callee_name(const char* call, char* dst, size_t cap) {
    const char* last = NULL;
    const char* s;
    size_t n = 0;
    int depth = 0;
    int in_dq = 0, in_sq = 0;
    dst[0] = 0;
    if (!call || cap < 2) return;
    for (s = call; *s; s++) {
        char c = *s;
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
        if (c == '"') {
            in_dq = 1;
            continue;
        }
        if (c == '\'') {
            in_sq = 1;
            continue;
        }
        if (c == '(' || c == '{') {
            depth++;
            continue;
        }
        if (c == ')' || c == '}') {
            if (depth > 0) depth--;
            continue;
        }
        if (depth != 0) continue;
        if (((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_') &&
            (s == call ||
             !((s[-1] >= 'A' && s[-1] <= 'Z') || (s[-1] >= 'a' && s[-1] <= 'z') ||
               (s[-1] >= '0' && s[-1] <= '9') || s[-1] == '_'))) {
            const char* id = s;
            size_t k = 0;
            const char* q;
            while ((id[k] >= 'A' && id[k] <= 'Z') ||
                   (id[k] >= 'a' && id[k] <= 'z') ||
                   (id[k] >= '0' && id[k] <= '9') || id[k] == '_')
                k++;
            q = id + k;
            while (*q == ' ' || *q == '\t') q++;
            if (*q == '(') last = id;
        }
    }
    if (!last) return;
    while ((last[n] >= 'A' && last[n] <= 'Z') ||
           (last[n] >= 'a' && last[n] <= 'z') ||
           (last[n] >= '0' && last[n] <= '9') || last[n] == '_') {
        if (n + 1 >= cap) break;
        dst[n] = last[n];
        n++;
    }
    dst[n] = 0;
}

/* Known Result E with in-scope handlers that do not match: ill-formed.
 * Returns 1 when a diagnostic was issued. */
static int shadow_safe_diag_eh_e(ShadowSafeCtx* ctx, AstNode* st,
                                 const char* call) {
    char callee[64];
    const char* ety;
    int exact = 0, i;
    int face_i = -1, face_scope = -1, same = 0;
    if (!ctx || !st || ctx->err || ctx->neh <= 0 || !call) return 0;
    shadow_safe_callee_name(call, callee, sizeof(callee));
    ety = shadow_safe_result_fn_err(ctx, callee);
    if (!ety || !ety[0]) return 0;
    for (i = ctx->neh - 1; i >= 0; i--) {
        AstNode* eh = ctx->eh_stack[i];
        if (eh && eh->a[0] && strcmp(eh->a, ety) == 0) exact++;
    }
    if (exact) return 0;
    for (i = ctx->neh - 1; i >= 0; i--) {
        AstNode* eh = ctx->eh_stack[i];
        if (!eh || !eh->a[0] || strcmp(eh->a, ety) == 0) continue;
        if (!shadow_safe_as_face_of(ctx, ety, eh->a)) continue;
        if (face_i < 0) {
            face_i = i;
            face_scope = ctx->eh_scopes[i];
            same = 1;
        } else if (ctx->eh_scopes[i] == face_scope) {
            same++;
        }
    }
    if (same == 1) return 0;
    if (same > 1) {
        char msg[192];
        snprintf(msg, sizeof(msg),
                 "syntax: ambiguous '@errhandler' for error "
                 "type '%s': multiple as: faces match "
                 "in-scope handlers",
                 ety);
        shadow_safe_err_at_col(ctx, st, 1, msg);
        return 1;
    }
    /* No exact / known face: do not guess. Header `@typeview` faces
     * (CCIoError → CCError) live in the emit `@as` registry. */
    return 0;
}

static int shadow_safe_rfn_index(ShadowSafeCtx* ctx, const char* name) {
    int i, hit = -1, n = 0;
    size_t nlen;
    if (!ctx || !name || !name[0] || !ctx->rfns) return -1;
    for (i = 0; i < ctx->nrfn; i++)
        if (strcmp(ctx->rfns[i].name, name) == 0) return i;
    nlen = strlen(name);
    for (i = 0; i < ctx->nrfn; i++) {
        const char* rn = ctx->rfns[i].name;
        size_t rlen = strlen(rn);
        if (rlen > nlen + 1 && rn[rlen - nlen - 1] == '_' &&
            strcmp(rn + rlen - nlen, name) == 0) {
            n++;
            hit = i;
        }
    }
    return n == 1 ? hit : -1;
}

static const char* shadow_safe_stdlib_err(const char* name) {
    if (!name || !name[0]) return NULL;
    if (strcmp(name, "cc_command_status") == 0 ||
        strcmp(name, "cc_command_output") == 0 ||
        strcmp(name, "cc_command_output_with_input") == 0 ||
        strcmp(name, "cc_process_spawn") == 0 ||
        strcmp(name, "cc_process_wait") == 0 ||
        strcmp(name, "cc_process_write") == 0)
        return "CCIoError";
    if (strcmp(name, "cc_println") == 0 ||
        strcmp(name, "cc_eprintln") == 0 ||
        strcmp(name, "cc_print") == 0 ||
        strcmp(name, "cc_eprint") == 0 ||
        strcmp(name, "cc_fprintln") == 0 ||
        strcmp(name, "cc_fprint") == 0 ||
        strncmp(name, "cc_slice_", 9) == 0 ||
        strncmp(name, "cc_string_", 10) == 0 ||
        strncmp(name, "cc_char_", 8) == 0 ||
        strncmp(name, "cc_const_char_", 14) == 0)
        return "CCError";
    if (strncmp(name, "cc_js_", 6) == 0) return "CCJsError";
    if (strncmp(name, "cc_py_", 6) == 0) return "CCPyError";
    return NULL;
}

static const char* shadow_safe_result_fn_err(ShadowSafeCtx* ctx,
                                            const char* name) {
    int i = shadow_safe_rfn_index(ctx, name);
    if (i >= 0 && ctx->rfns[i].err[0]) return ctx->rfns[i].err;
    return shadow_safe_stdlib_err(name);
}

static int shadow_safe_as_paths(ShadowSafeCtx* ctx, const char* outer,
                                const char* face, int depth) {
    int si, fi, n = 0;
    if (!ctx || !outer || !face || depth > 8) return 0;
    if (strcmp(outer, face) == 0) return 1;
    for (si = 0; si < ctx->nstructs; si++) {
        ShadowSafeStruct* s = &ctx->structs[si];
        if (strcmp(s->name, outer) != 0) continue;
        for (fi = 0; fi < s->nf; fi++) {
            char leaf[64];
            if (!s->is_as[fi]) continue;
            shadow_safe_strip_ty(s->fty[fi], leaf, sizeof(leaf));
            if (!leaf[0]) continue;
            if (strcmp(leaf, face) == 0) n++;
            else n += shadow_safe_as_paths(ctx, leaf, face, depth + 1);
        }
    }
    return n;
}

static int shadow_safe_as_face_of(ShadowSafeCtx* ctx, const char* outer,
                                  const char* face) {
    if (!ctx || !outer || !face) return 0;
    if (strcmp(outer, face) == 0) return 1;
    return shadow_safe_as_paths(ctx, outer, face, 0) == 1;
}

static void shadow_safe_check_err_fwds(ShadowSafeCtx* ctx, AstNode* site,
                                      AstNode** body, int nbody,
                                      const char* binder) {
    int i, j;
    AstNode* eh;
    (void)site;
    if (!ctx || !body || ctx->err) return;
    for (i = 0; i < nbody && !ctx->err; i++) {
        AstNode* st = body[i];
        if (!st || st->kind != AST_ERR_FWD) continue;
        if (st->c[0] && strcmp(st->c, "delegate") == 0) continue;
        if (!binder || !binder[0]) {
            char msg[160];
            snprintf(msg, sizeof(msg),
                     "'@err(%s)' requires an error binder",
                     st->a[0] ? st->a : "e");
            shadow_safe_err_at(ctx, st, msg);
            return;
        }
        if (st->a[0] && strcmp(st->a, binder) != 0) {
            char msg[192];
            snprintf(msg, sizeof(msg),
                     "@err(%s) forward references unknown binder '%s' "
                     "(expected '%s')",
                     st->a, st->a, binder);
            shadow_safe_err_at(ctx, st, msg);
            return;
        }
        eh = shadow_safe_eh(ctx);
        if (!eh) {
            shadow_safe_err_at(ctx, st,
                               "@err with no local handler requires @errhandler");
            return;
        }
        if (!shadow_safe_body_diverges(eh->body, eh->nbody)) {
            /* Named-stmt handlers: exact diverge callees only (no substring). */
            if (!(eh->c[0] &&
                  (strcmp(eh->c, "exit") == 0 || strcmp(eh->c, "abort") == 0 ||
                   strcmp(eh->c, "_exit") == 0 ||
                   strcmp(eh->c, "cc_error_exit") == 0 ||
                   strcmp(eh->c, "longjmp") == 0))) {
                shadow_safe_err_at(ctx, eh,
                                   "@errhandler body must visibly diverge");
                return;
            }
        }
        for (j = i + 1; j < nbody; j++) {
            if (!body[j]) continue;
            shadow_safe_err_at(ctx, body[j],
                               "unreachable code after '@err(e);'");
            return;
        }
    }
}

static int shadow_safe_unwrap_mode_known(const char* mode) {
    if (!mode || !mode[0]) return 0;
    if (strcmp(mode, "bang") == 0 || strcmp(mode, "bang_chain") == 0 ||
        strncmp(mode, "bang_chain:", 11) == 0 ||
        strcmp(mode, "bang_block") == 0 || strcmp(mode, "bang_stmt") == 0 ||
        strcmp(mode, "bang_nobind") == 0 || strcmp(mode, "qmark") == 0 ||
        strcmp(mode, "qmark_bind") == 0)
        return 1;
    if (strncmp(mode, "bang_eh", 7) == 0) return 1;
    if (strncmp(mode, "bang_nobind", 11) == 0) return 1;
    if (strncmp(mode, "bang_block", 10) == 0) return 1;
    if (strncmp(mode, "qmark_bind:", 11) == 0) return 1;
    if (strncmp(mode, "qmark", 5) == 0) return 1;
    return 0;
}

static void shadow_safe_check_bang_body(ShadowSafeCtx* ctx, AstNode* st,
                                       const char* mode, const char* binder,
                                       int expr_pos) {
    if (!ctx || !st || ctx->err) return;
    if (mode && (strcmp(mode, "bang_block") == 0 ||
                 strcmp(mode, "bang_stmt") == 0 ||
                 strncmp(mode, "bang_block:", 11) == 0)) {
        if (expr_pos && !shadow_safe_body_diverges(st->body, st->nbody)) {
            shadow_safe_err_at(
                ctx, st, "expression-position '!>' body must diverge");
            return;
        }
        shadow_safe_check_err_fwds(ctx, st, st->body, st->nbody, binder);
    }
}

static int shadow_safe_node_in_tree(AstNode* root, AstNode* needle) {
    int i;
    if (!root || !needle) return 0;
    if (root == needle) return 1;
    for (i = 0; i < root->nbody; i++)
        if (shadow_safe_node_in_tree(root->body[i], needle)) return 1;
    for (i = 0; i < root->nkids; i++)
        if (shadow_safe_node_in_tree(root->kids[i], needle)) return 1;
    return 0;
}

static void shadow_safe_diag_bare_bang_reenter(ShadowSafeCtx* ctx, AstNode* st) {
    int i;
    if (!ctx || !st || ctx->err) return;
    for (i = 0; i < ctx->neh; i++) {
        AstNode* eh = ctx->eh_stack[i];
        if (!eh) continue;
        if (shadow_safe_node_in_tree(eh, st) && st != eh) {
            shadow_safe_err_at_col(
                ctx, st, 1,
                "syntax: bare '!>;' inside matching '@errhandler' would "
                "re-enter the same handler; use a helper (cc_error_exit) or "
                "'!> { abort(); }' for reporting");
            return;
        }
    }
}

static void shadow_safe_require_eh(ShadowSafeCtx* ctx, AstNode* st,
                                  const char* stmt_msg) {
    if (!ctx || !st || ctx->err) return;
    if (shadow_safe_eh(ctx)) {
        /* Bare `!>;` lexically inside the matching handler body re-enters. */
        shadow_safe_diag_bare_bang_reenter(ctx, st);
        return;
    }
    if (stmt_msg) {
        shadow_safe_err_at(ctx, st, stmt_msg);
        return;
    }
    /* Match diag_oracle_ccfront: col 1 + `syntax:` prefix. */
    shadow_safe_err_at_col(
        ctx, st, 1,
        "syntax: '!>;' at expression position requires an enclosing "
        "'@errhandler' in scope");
}

static void shadow_safe_on_unwrap(ShadowSafeCtx* ctx, AstNode* st) {
    const char* mode;
    const char* binder;
    if (!ctx || !st || ctx->err) return;
    if (st->kind == AST_VAR_UNWRAP) {
        mode = st->c;
        binder = st->d;
        if (!shadow_safe_unwrap_mode_known(mode)) {
            shadow_safe_err_at(ctx, st, "unknown unwrap mode (refuse by default)");
            return;
        }
        if (strcmp(mode, "bang") == 0) {
            if (shadow_safe_diag_eh_e(ctx, st, st->a)) return;
            shadow_safe_require_eh(ctx, st, NULL);
        }
        else if (strcmp(mode, "bang_chain") == 0 ||
                 strncmp(mode, "bang_chain:", 11) == 0) {
            /* First hop may carry `!>(e){…}`; later bare `!>` need eh. */
            if (st->nbody > 0)
                shadow_safe_check_bang_body(ctx, st, "bang_block", binder, 1);
            if (st->nbody == 0 || (st->e[0] && strstr(st->e, "!>")) ) {
                if (shadow_safe_diag_eh_e(ctx, st, st->a)) return;
                shadow_safe_require_eh(ctx, st, NULL);
            }
        } else if ((strcmp(mode, "bang_stmt") == 0 ||
                  strcmp(mode, "bang_block") == 0) &&
                 st->nbody == 1 && st->body[0] &&
                 st->body[0]->kind == AST_AT_STMT &&
                 (strcmp(st->body[0]->a, "destroy") == 0 ||
                  strcmp(st->body[0]->a, "detach") == 0)) {
            /* `T x = call() !> @destroy` parsed as bang_stmt — delegate to eh. */
            if (shadow_safe_diag_eh_e(ctx, st, st->a)) return;
            shadow_safe_require_eh(ctx, st, NULL);
        } else
            shadow_safe_check_bang_body(ctx, st, mode, binder, 1);
        return;
    }
    if (st->kind == AST_STMT_UNWRAP) {
        mode = st->c;
        binder = st->d;
        if (!mode[0]) {
            /* `call() !>;` — unique-E / ambiguous @as across in-scope handlers. */
            if (shadow_safe_diag_eh_e(ctx, st, st->a)) return;
            /* `call() !>;` statement position */
            shadow_safe_require_eh(
                ctx, st, "@err with no local handler requires @errhandler");
            return;
        }
        if (!shadow_safe_unwrap_mode_known(mode)) {
            shadow_safe_err_at(ctx, st, "unknown unwrap mode (refuse by default)");
            return;
        }
        /* Statement-position bang_block/bang_stmt: diverge not required by
         * Form D/E legacy (only expr-pos). Still check @err forwards. */
        shadow_safe_check_bang_body(ctx, st, mode, binder, 0);
        return;
    }
    if (st->kind == AST_PTR_UNWRAP) {
        mode = st->d;
        binder = st->e;
        if (!shadow_safe_unwrap_mode_known(mode)) {
            shadow_safe_err_at(ctx, st, "unknown unwrap mode (refuse by default)");
            return;
        }
        /* bang_nobind + body is `!> { … }` (inline handler), not bare !>; */
        if (strncmp(mode, "bang_eh", 7) == 0) {
            if (shadow_safe_diag_eh_e(ctx, st, st->c[0] ? st->c : st->a))
                return;
            shadow_safe_require_eh(ctx, st, NULL);
        }
        else if (strncmp(mode, "bang_nobind", 11) == 0) {
            if (st->nbody > 0)
                shadow_safe_check_bang_body(ctx, st, "bang_block", binder, 1);
            else {
                if (shadow_safe_diag_eh_e(ctx, st, st->c[0] ? st->c : st->a))
                    return;
                shadow_safe_require_eh(ctx, st, NULL);
            }
        } else if (strncmp(mode, "bang_block", 10) == 0)
            shadow_safe_check_bang_body(ctx, st, "bang_block", binder, 1);
        return;
    }
    if (st->kind == AST_PRINTLN_BANG || st->kind == AST_PRINTLN_TPL) {
        if (st->nbody > 0)
            shadow_safe_check_err_fwds(ctx, st, st->body, st->nbody, NULL);
        else if (strcmp(st->e, "bare") != 0) {
            /* `println(...) !>;` needs @errhandler; bare `println(...);` does not. */
            if (shadow_safe_diag_eh_e(ctx, st, st->d[0] == 'e'
                                                ? "cc_eprintln("
                                                : "cc_println("))
                return;
            shadow_safe_require_eh(ctx, st, NULL);
        }
        return;
    }
    if (st->kind == AST_PRINTLN_BANG_BIND) {
        shadow_safe_check_err_fwds(ctx, st, st->body, st->nbody, st->b);
        return;
    }
}

static int shadow_safe_fn_returns_result(const AstNode* fn) {
    if (!fn) return 0;
    if (fn->kind == AST_RESULT_FN) return 1;
    if (fn->kind == AST_ASYNC_FN && strcmp(fn->e, "result") == 0) return 1;
    return 0;
}

static int shadow_safe_ufcs_result_consumed(const AstNode* st) {
    const char* d;
    if (!st) return 0;
    d = st->d;
    if (!d[0]) return 0;
    return strncmp(d, "bang", 4) == 0 || strncmp(d, "qmark", 5) == 0 ||
           strcmp(d, "await") == 0 || strncmp(d, "bang_await", 10) == 0;
}

static void shadow_safe_on_unhandled_call(ShadowSafeCtx* ctx, AstNode* st) {
    const char* callee;
    if (!ctx || !st || ctx->err || !ctx->strict_unhandled) return;
    /* send_task body packs the Result into the task payload. */
    if (ctx->in_send_task) return;
    if (st->kind == AST_UFCS_STMT) {
        if (shadow_safe_ufcs_result_consumed(st)) return;
        if (!shadow_safe_is_result_fn(ctx, st->b)) return;
        callee = st->b;
    } else if (st->kind == AST_CALL_ARGS || st->kind == AST_CALL_NUM) {
        if (!shadow_safe_is_result_fn(ctx, st->a)) return;
        callee = st->a;
    } else {
        return;
    }
    {
        /* Two-line oracle: emit as one stderr blob with newline. */
        shadow_safe_err_at(ctx, st, "unhandled-result");
        fprintf(stderr, "call to '%s'\n", callee);
    }
}

static int shadow_safe_body_mutates_ident(AstNode** body, int n,
                                         const char* name) {
    int i;
    if (!body || !name) return 0;
    for (i = 0; i < n; i++) {
        AstNode* st = body[i];
        char lname[64];
        if (!st) continue;
        if (st->kind == AST_INC && strcmp(st->a, name) == 0) return 1;
        if (st->kind == AST_ASSIGN) {
            if (shadow_safe_plain_ident(st->a, lname, sizeof(lname)) &&
                strcmp(lname, name) == 0)
                return 1;
            if (st->a[0] == '*' &&
                shadow_safe_plain_ident(st->a + 1, lname, sizeof(lname)) &&
                strcmp(lname, name) == 0)
                return 1;
        }
        if (st->nbody &&
            shadow_safe_body_mutates_ident(st->body, st->nbody, name))
            return 1;
        if (st->nkids &&
            shadow_safe_body_mutates_ident(st->kids, st->nkids, name))
            return 1;
    }
    return 0;
}

static int shadow_safe_body_passes_amp(AstNode** body, int n,
                                      const char* name, char* callee_out,
                                      size_t ccap) {
    int i;
    char amp[80];
    if (!body || !name) return 0;
    snprintf(amp, sizeof(amp), "&%s", name);
    for (i = 0; i < n; i++) {
        AstNode* st = body[i];
        if (!st) continue;
        if ((st->kind == AST_CALL_ARGS || st->kind == AST_CALL_NUM) &&
            shadow_safe_text_uses(st->b, amp)) {
            if (callee_out && ccap)
                snprintf(callee_out, ccap, "%s", st->a);
            return 1;
        }
        if (st->nbody &&
            shadow_safe_body_passes_amp(st->body, st->nbody, name, callee_out,
                                       ccap))
            return 1;
        if (st->nkids &&
            shadow_safe_body_passes_amp(st->kids, st->nkids, name, callee_out,
                                       ccap))
            return 1;
    }
    return 0;
}

enum { SHADOW_SAFE_CFN_CAP = 64 };
/* File-scope: filled in shadow_safety_check before walks. */
static char g_shadow_safe_const_fns[SHADOW_SAFE_CFN_CAP][64];
static int g_shadow_safe_nconst_fns;

static int shadow_safe_is_const_ptr_fn(const char* name) {
    int i;
    if (!name) return 0;
    for (i = 0; i < g_shadow_safe_nconst_fns; i++)
        if (strcmp(g_shadow_safe_const_fns[i], name) == 0) return 1;
    return 0;
}

static void shadow_safe_add_const_ptr_fn(const char* name) {
    if (!name || !name[0] || g_shadow_safe_nconst_fns >= SHADOW_SAFE_CFN_CAP)
        return;
    if (shadow_safe_is_const_ptr_fn(name)) return;
    snprintf(g_shadow_safe_const_fns[g_shadow_safe_nconst_fns++],
             sizeof(g_shadow_safe_const_fns[0]), "%s", name);
}

static int shadow_safe_amp_call_ok(const char* callee) {
    if (!callee || !callee[0]) return 0;
    if (shadow_safe_is_const_ptr_fn(callee)) return 1;
    /* Intentional shared mutators (atomics) — not a race diagnostic target. */
    if (strstr(callee, "atomic") != NULL) return 1;
    return 0;
}

/* Escaping closure capturing a stack slice / @scratch string. */
static void shadow_safe_check_escape_stack_slice(ShadowSafeCtx* ctx,
                                                AstNode* cl, int escapes) {
    const char* p;
    char item[64];
    size_t n;
    ShadowSafeVar* v;
    if (!ctx || !cl || !escapes || ctx->err) return;
    p = cl->e;
    while (p && *p) {
        while (*p == ' ' || *p == '\t' || *p == ',') p++;
        if (!*p) break;
        n = 0;
        /* Ref capture of stack slice still escapes (`&x` / `@safe&x`). */
        if (strncmp(p, "@safe&", 6) == 0) p += 6;
        else if (*p == '&') p++;
        while (*p && *p != ',' && n + 1 < sizeof(item)) item[n++] = *p++;
        item[n] = 0;
        if (!item[0]) continue;
        v = shadow_safe_find(ctx, item);
        if (v && ((v->is_slice && v->is_stack_slice_view) || v->is_scratch_string)) {
            shadow_safe_err_at(
                ctx, cl,
                "cannot capture stack slice or @scratch string in escaping "
                "closure");
            fprintf(stderr,
                    "  note: the closure outlives the stack / @scratch frame\n");
            fprintf(stderr,
                    "  hint: pass as parameter, or allocate with an explicit "
                    "heap/arena\n");
            return;
        }
    }
}

static void shadow_safe_on_spawn(ShadowSafeCtx* ctx, AstNode* st, int escapes) {
    /* caps in e: "view,base,&x" */
    const char* p;
    char item[64];
    size_t n;
    ShadowSafeVar* v;
    char callee[64];
    char msg[192];
    int is_unsafe;
    if (!ctx || !st || ctx->err) return;
    is_unsafe = (strcmp(st->b, "spawn_unsafe") == 0 ||
                 strcmp(st->b, "spawnhybrid_unsafe") == 0);
    /* Task-escaping only for value-captured pointer alias mutation (legacy). */
    if (ctx->in_send_task) escapes = 1;
    p = st->e;
    while (p && *p) {
        int is_ref = 0;
        int is_safe_cap = 0;
        while (*p == ' ' || *p == '\t' || *p == ',') p++;
        if (!*p) break;
        n = 0;
        if (strncmp(p, "@safe&", 6) == 0) {
            is_ref = 1;
            is_safe_cap = 1;
            p += 6;
        } else if (*p == '&') {
            is_ref = 1;
            p++;
        }
        while (*p && *p != ',' && n + 1 < sizeof(item)) item[n++] = *p++;
        item[n] = 0;
        if (!item[0]) continue;
        v = shadow_safe_find(ctx, item);
        if (v && v->arena[0] && (v->arena_view || v->is_arena_ptr))
            shadow_safe_pin_arena(ctx, v->arena, st->a);
        /* Value-capture of a unique slice is an implicit move into the
         * closure env. Defer marking until after the body walk so uses
         * inside the closure remain valid; outer scope sees the move. */
        if (!is_ref && v && v->is_slice && v->unique) {
            if (v->moved) {
                snprintf(msg, sizeof(msg), "use of moved slice '%s'", item);
                shadow_safe_err_at(ctx, st, msg);
                return;
            }
            if (item[0] && ctx->npending_moves < 16) {
                snprintf(ctx->pending_moves[ctx->npending_moves],
                         sizeof(ctx->pending_moves[0]), "%s", item);
                ctx->npending_moves++;
            }
        }
        if (is_unsafe) continue; /* @unsafe: skip mutation / amp races */
        if (is_ref) {
            if (shadow_safe_body_mutates_ident(st->body, st->nbody, item)) {
                snprintf(msg, sizeof(msg),
                         "mutation of shared reference '%s' in closure", item);
                shadow_safe_err_at(ctx, st, msg);
                return;
            }
            /* `@safe &x`: intentional share — amp-pass to callees is expected.
             * Direct mutation (`x++`) still rejected above. */
            callee[0] = 0;
            if (!is_safe_cap &&
                shadow_safe_body_passes_amp(st->body, st->nbody, item, callee,
                                            sizeof(callee)) &&
                !shadow_safe_amp_call_ok(callee)) {
                snprintf(msg, sizeof(msg),
                         "passing '&%s' to '%s' may mutate shared state", item,
                         callee[0] ? callee : "callee");
                shadow_safe_err_at(ctx, st, msg);
                return;
            }
        } else if (escapes && v && (v->is_ptr || v->is_arena_ptr) &&
                   v->alias_of[0]) {
            /* Sync CCClosure stores may write through T* p = &local. */
            if (shadow_safe_body_mutates_ident(st->body, st->nbody, item)) {
                snprintf(msg, sizeof(msg),
                         "mutation through value-captured pointer '%s' that "
                         "aliases an outer local",
                         item);
                shadow_safe_err_at(ctx, st, msg);
                return;
            }
        }
    }
}

static void shadow_safe_walk_stmt(ShadowSafeCtx* ctx, AstNode* st);
static void shadow_safe_check_field_suggest_text(ShadowSafeCtx* ctx, AstNode* st,
                                                const char* text);
static void shadow_safe_walk_list(ShadowSafeCtx* ctx, AstNode** xs, int n);

/* Walk dbody SPAWN_CLOSURE attachments (call-arg / assign / return). */
static void shadow_safe_walk_attached_closures(ShadowSafeCtx* ctx, AstNode* st,
                                              int escapes) {
    int k;
    if (!ctx || !st || ctx->err) return;
    for (k = 0; k < st->ndbody && !ctx->err; k++) {
        AstNode* cl = st->dbody[k];
        int prev_st;
        int saved_npend;
        if (!cl || cl->kind != AST_SPAWN_CLOSURE) continue;
        shadow_safe_check_escape_stack_slice(ctx, cl, escapes);
        if (ctx->err) return;
        prev_st = ctx->in_send_task;
        if (cl->b[0] && (strcmp(cl->b, "send_task") == 0 ||
                         strcmp(cl->b, "send_task_hybrid") == 0))
            ctx->in_send_task = 1;
        saved_npend = ctx->npending_moves;
        shadow_safe_on_spawn(ctx, cl, escapes);
        if (!ctx->err) shadow_safe_walk_list(ctx, cl->body, cl->nbody);
        /* Commit only moves recorded for this closure. */
        if (!ctx->err) {
            int i;
            for (i = saved_npend; i < ctx->npending_moves; i++) {
                ShadowSafeVar* v = shadow_safe_find(ctx, ctx->pending_moves[i]);
                if (v && v->is_slice) v->moved = 1;
            }
        }
        ctx->npending_moves = saved_npend;
        ctx->in_send_task = prev_st;
    }
}

static void shadow_safe_walk_list(ShadowSafeCtx* ctx, AstNode** xs, int n) {
    int i;
    for (i = 0; i < n && ctx && !ctx->err; i++)
        shadow_safe_walk_stmt(ctx, xs[i]);
}

static void shadow_safe_walk_stmt(ShadowSafeCtx* ctx, AstNode* st) {
    int saved_nvars;
    int saved_neh;
    int saved_eh_scope;
    if (!ctx || !st || ctx->err) return;
    /* Nested-field suggest while locals are still in scope. */
    shadow_safe_check_field_suggest_text(ctx, st, st->a);
    if (!ctx->err) shadow_safe_check_field_suggest_text(ctx, st, st->b);
    if (!ctx->err) shadow_safe_check_field_suggest_text(ctx, st, st->c);
    if (!ctx->err) shadow_safe_check_field_suggest_text(ctx, st, st->d);
    if (!ctx->err) shadow_safe_check_field_suggest_text(ctx, st, st->e);
    if (ctx->err) return;
    if (st->kind == AST_VAR_UNWRAP || st->kind == AST_TYPED_INIT) {
        /* Non-recursive: only this node (kids walked below).
         * VAR_UNWRAP: bang keeps lhs type in e; qmark parks type in d /
         * qmark_bind:Type (e is the fallback expr). */
        const char* ty = NULL;
        const char* name =
            (st->kind == AST_VAR_UNWRAP) ? st->a : st->b;
        const char* mode =
            (st->kind == AST_VAR_UNWRAP) ? st->c : "";
        if (st->kind == AST_TYPED_INIT)
            ty = st->a;
        else if (strcmp(mode, "qmark") == 0)
            ty = st->d[0] ? st->d : "int";
        else if (strncmp(mode, "qmark_bind:", 11) == 0)
            ty = mode[11] ? mode + 11 : "int";
        else if (strncmp(mode, "bang_chain:", 11) == 0 && mode[11])
            ty = mode + 11;
        else
            ty = st->e;
        if (ty && strcmp(ty, "CCPyObj") == 0 && name && name[0] &&
            !strstr(mode, "destroy") && !strstr(mode, "detach") &&
            !(mode[0] && strchr(mode, 'D')) &&
            !shadow_safe_fn_releases_py_obj(ctx->cur_fn, name)) {
            char msg[256];
            snprintf(msg, sizeof(msg),
                     "ownership: `CCPyObj %s` holds a Python reference: bind "
                     "'@destroy' so the scope releases it, consume it "
                     "(`return %s`), or release it by hand "
                     "(cc_py_obj_release); a dropped handle leaks the object "
                     "with no diagnostic on either side of the boundary",
                     name, name);
            shadow_safe_err_at(ctx, st, msg);
            return;
        }
    }
    switch (st->kind) {
    case AST_TYPEDEF_CHAN: {
        int role = 0;
        if (st->c[0] == '>') role = 1;
        else if (st->c[0] == '<') role = 2;
        if (st->a[0] && role) shadow_safe_add_chan_td(ctx, st->a, role);
        break;
    }
    case AST_CHAN_VAR: {
        const char* p = st->a;
        char name[64];
        size_t ni;
        int role = 0;
        if (strcmp(st->c, ">") == 0) role = 1;
        else if (strcmp(st->c, "<") == 0) role = 2;
        while (*p) {
            ShadowSafeVar* cv;
            while (*p == ' ' || *p == '\t' || *p == ',') p++;
            if (!*p) break;
            ni = 0;
            while (*p && *p != ',' && *p != ' ' && *p != '\t' &&
                   ni + 1 < sizeof(name))
                name[ni++] = *p++;
            name[ni] = 0;
            cv = name[0] ? shadow_safe_add(ctx, name) : NULL;
            if (cv) cv->is_chan = role;
        }
        break;
    }
    case AST_VAR_DECL: {
        /* `WorkerTx tx;` — typedef channel alias → mark endpoint role.
         * Also bind ordinary locals (`int v;`) so send(v+1) can prove POD. */
        int role = shadow_safe_chan_td_role(ctx, st->a);
        ShadowSafeVar* cv;
        if (st->a[0]) shadow_safe_check_map_key_text(ctx, st, st->a);
        if (st->b[0] && !ctx->err) {
            cv = shadow_safe_add(ctx, st->b);
            if (cv) {
                snprintf(cv->ty, sizeof(cv->ty), "%s", st->a);
                if (role) cv->is_chan = role;
                if (strchr(st->a, '*')) cv->is_ptr = 1;
                /* `int buf[N];` — dims in c. */
                if (st->c[0] == '[') cv->is_stack_array = 1;
            }
        }
        break;
    }
    case AST_TYPED_INIT:
    case AST_PTR_INIT:
        /* `@create` without `@destroy`/`@detach` — ownership required. */
        if (!ctx->err && st->c[0] &&
            (strstr(st->c, "@create") || strstr(st->c, "__cc_at_create"))) {
            char msg[192];
            int is_ptr = (st->kind == AST_PTR_INIT) ||
                         (st->d[0] == '*');
            snprintf(msg, sizeof(msg),
                     "type: `%s%s` created with '@(...)' requires explicit "
                     "ownership: use '@destroy' or '@detach'",
                     st->a, is_ptr ? "*" : "");
            shadow_safe_err_at(ctx, st, msg);
        }
        if (!ctx->err) shadow_safe_on_typed_init(ctx, st);
        if (!ctx->err) shadow_safe_check_variant_text(ctx, st, st->c);
        if (!ctx->err) shadow_safe_check_type_of_text(ctx, st, st->c);
        if (!ctx->err) shadow_safe_check_channel_pair_text(ctx, st, st->c);
        /* Local bind of a closure is not an escape by itself. */
        if (!ctx->err) shadow_safe_walk_attached_closures(ctx, st, 0);
        break;
    case AST_VAL_DESTROY:
        shadow_safe_on_typed_init(ctx, st);
        if (!ctx->err) shadow_safe_check_variant_text(ctx, st, st->c);
        /* `T x = call() !> @destroy` — bang uses enclosing @errhandler. */
        if (!ctx->err && st->c[0] && strstr(st->c, "!>") != NULL)
            shadow_safe_require_eh(ctx, st, NULL);
        if (!ctx->err) shadow_safe_walk_attached_closures(ctx, st, 0);
        break;
    case AST_ASSIGN:
        shadow_safe_on_assign(ctx, st);
        /* Detect `v.kind = ...` — .kind is read-only on variants. */
        if (!ctx->err && st->a[0]) {
            int vi2;
            for (vi2 = 0; vi2 < ctx->nvars && !ctx->err; vi2++) {
                ShadowSafeVar* sv = ctx->vars[vi2];
                ShadowSafeVariant* svt;
                size_t snl;
                const char* sp;
                if ((!sv->is_variant && !sv->is_variant_ptr) || !sv->name[0]) continue;
                snl = strlen(sv->name);
                svt = shadow_safe_find_variant(ctx, sv->ty);
                if (!svt) continue;
                sp = st->a;
                while ((sp = strstr(sp, sv->name)) != NULL) {
                    char sb = (sp > st->a) ? sp[-1] : 0;
                    const char* sa = sp + snl;
                    int is_arrow = 0;
                    if (shadow_safe_is_word(sb) || shadow_safe_is_word(*sa)) {
                        sp = sa; continue;
                    }
                    if (sa[0] == '.' && sa[1] == 'k' && sa[2] == 'i' &&
                        sa[3] == 'n' && sa[4] == 'd' &&
                        !shadow_safe_is_word(sa[5])) {
                        is_arrow = 0;
                    } else if (sa[0] == '-' && sa[1] == '>' && sa[2] == 'k' &&
                               sa[3] == 'i' && sa[4] == 'n' && sa[5] == 'd' &&
                               !shadow_safe_is_word(sa[6])) {
                        is_arrow = 1;
                    } else {
                        sp = sa;
                        continue;
                    }
                    {
                        char km[384];
                        /* Point at `.` / `->` of the kind write. */
                        int force_col = 0;
                        if (st && ctx->cache && st->file_id) {
                            FileTape* ft = tape_by_id(ctx->cache, st->file_id);
                            if (ft && ft->bytes) {
                                size_t i;
                                const char* want = is_arrow ? "->kind" : ".kind";
                                size_t wl = strlen(want);
                                for (i = st->tok_off; i + wl < ft->len; i++) {
                                    if (memcmp(ft->bytes + i, want, wl) == 0) {
                                        int fl = 1, fc = 1;
                                        offset_to_linecol(ft, i, &fl, &fc);
                                        force_col = fc;
                                        break;
                                    }
                                }
                            }
                        }
                        snprintf(km, sizeof(km),
                                 "variant tag '.kind' is read-only — the tag "
                                 "changes only through construction or "
                                 "whole-variant assignment ('%s = (%s){ .arm = "
                                 "... };')",
                                 sv->name, svt->name);
                        shadow_safe_err_at_col(ctx, st, force_col, km);
                    }
                    break;
                }
            }
        }
        if (!ctx->err) shadow_safe_check_variant_text(ctx, st, st->a);
        if (!ctx->err) shadow_safe_check_variant_text(ctx, st, st->b);
        if (!ctx->err) shadow_safe_check_map_key_text(ctx, st, st->a);
        if (!ctx->err) shadow_safe_check_map_key_text(ctx, st, st->b);
        /* Store to file-scope global / member lvalue escapes the closure. */
        if (!ctx->err) {
            char lhs[64];
            int escapes = 0;
            if (shadow_safe_plain_ident(st->a, lhs, sizeof(lhs)) &&
                shadow_safe_is_global(ctx, lhs))
                escapes = 1;
            else if (strstr(st->a, ".") != NULL || strstr(st->a, "->") != NULL)
                escapes = 1;
            shadow_safe_walk_attached_closures(ctx, st, escapes);
        }
        break;
    case AST_RETURN_EXPR:
        shadow_safe_on_return(ctx, st);
        if (!ctx->err) shadow_safe_check_variant_text(ctx, st, st->a);
        /* Returning a closure always escapes. */
        if (!ctx->err) shadow_safe_walk_attached_closures(ctx, st, 1);
        /* `return expr !>(e) {…}` — walk handler body. */
        if (!ctx->err && st->nbody > 0)
            shadow_safe_walk_list(ctx, st->body, st->nbody);
        break;
    case AST_RETURN_CC:
        shadow_safe_on_return(ctx, st);
        if (!ctx->err) shadow_safe_check_variant_text(ctx, st, st->b);
        if (!ctx->err) shadow_safe_walk_attached_closures(ctx, st, 1);
        break;
    case AST_CALL_ARGS:
    case AST_CALL_NUM:
        shadow_safe_on_call(ctx, st);
        if (!ctx->err) shadow_safe_on_unhandled_call(ctx, st);
        if (!ctx->err) shadow_safe_check_map_key_text(ctx, st, st->a);
        if (!ctx->err) shadow_safe_check_map_key_text(ctx, st, st->b);
        /* Pass-as-arg closure escapes (nursery spawn is AST_SPAWN_CLOSURE). */
        if (!ctx->err) shadow_safe_walk_attached_closures(ctx, st, 1);
        break;
    case AST_UFCS_STMT:
    case AST_UFCS_EXPR:
        shadow_safe_on_ufcs(ctx, st);
        if (!ctx->err && st->kind == AST_UFCS_STMT)
            shadow_safe_on_unhandled_call(ctx, st);
        if (!ctx->err) shadow_safe_check_map_key_text(ctx, st, st->a);
        if (!ctx->err) shadow_safe_check_map_key_text(ctx, st, st->c);
        if (!ctx->err) shadow_safe_walk_attached_closures(ctx, st, 1);
        break;
    case AST_SPAWN_CLOSURE: {
        int prev_st = ctx->in_send_task;
        int saved_npend = ctx->npending_moves;
        int is_nursery =
            strcmp(st->b, "spawn") == 0 || strcmp(st->b, "spawn_unsafe") == 0 ||
            strcmp(st->b, "spawnhybrid") == 0 ||
            strcmp(st->b, "spawnhybrid_unsafe") == 0;
        if (st->b[0] && (strcmp(st->b, "send_task") == 0 ||
                         strcmp(st->b, "send_task_hybrid") == 0))
            ctx->in_send_task = 1;
        /* Nursery spawn is scoped — stack-slice capture is allowed there. */
        if (!is_nursery)
            shadow_safe_check_escape_stack_slice(ctx, st, 1);
        /* Nursery/send_task still task-escape for value-captured ptr mutation. */
        if (!ctx->err) shadow_safe_on_spawn(ctx, st, 1);
        if (!ctx->err) shadow_safe_walk_list(ctx, st->body, st->nbody);
        if (!ctx->err) {
            int i;
            for (i = saved_npend; i < ctx->npending_moves; i++) {
                ShadowSafeVar* v = shadow_safe_find(ctx, ctx->pending_moves[i]);
                if (v && v->is_slice) v->moved = 1;
            }
        }
        ctx->npending_moves = saved_npend;
        ctx->in_send_task = prev_st;
        break;
    }
    case AST_ERRHANDLER:
        shadow_safe_push_eh(ctx, st);
        shadow_safe_walk_list(ctx, st->body, st->nbody);
        break;
    case AST_ERR_SYNTAX:
        if (st->nbody == 0 && !st->d[0] && !shadow_safe_eh(ctx)) {
            shadow_safe_err_at(ctx, st,
                               "@err with no local handler requires @errhandler");
            return;
        }
        if (st->nbody > 0) {
            int bi;
            const char* bind = st->d[0] ? strrchr(st->d, ' ') : NULL;
            if (bind) bind++;
            else bind = st->d[0] ? st->d : NULL;
            for (bi = 0; bi < st->nbody && !ctx->err; bi++) {
                AstNode* fwd = st->body[bi];
                if (!fwd || fwd->kind != AST_ERR_FWD ||
                    strcmp(fwd->c, "delegate") != 0)
                    continue;
                if (!bind || !bind[0] || strcmp(fwd->a, bind) != 0 ||
                    !shadow_safe_eh(ctx)) {
                    shadow_safe_err_at(
                        ctx, fwd,
                        "bad @errhandler(e); delegation in @err block");
                    return;
                }
            }
            shadow_safe_check_err_fwds(ctx, st, st->body, st->nbody, bind);
        }
        shadow_safe_walk_list(ctx, st->body, st->nbody);
        break;
    case AST_BLOCK:
    case AST_NURSERY_DESTROY: {
        int vi, k;
        saved_nvars = ctx->nvars;
        saved_neh = ctx->neh;
        saved_eh_scope = ctx->eh_scope;
        ctx->eh_scope++;
        shadow_safe_walk_list(ctx, st->body, st->nbody);
        shadow_safe_walk_list(ctx, st->kids, st->nkids);
        /* Nursery @destroy / block exit joins tasks — drop pins tied to
         * nursery locals that leave this scope (CVE-2017-13245 idiomatic).
         * PTR_UNWRAP `CCNursery* n = … !> @destroy` often never lands in
         * vars[]; scan the block body for nursery destroy sites too. */
        if (st->kind == AST_NURSERY_DESTROY && st->b[0])
            shadow_safe_unpin_nursery(ctx, st->b);
        for (vi = saved_nvars; vi < ctx->nvars; vi++) {
            if (ctx->vars[vi] &&
                shadow_safe_type_is_nursery(ctx->vars[vi]->ty))
                shadow_safe_unpin_nursery(ctx, ctx->vars[vi]->name);
        }
        for (k = 0; k < st->nbody; k++) {
            AstNode* s = st->body[k];
            const char* nname = NULL;
            const char* nty = NULL;
            if (!s) continue;
            if (s->kind == AST_NURSERY_DESTROY) {
                nname = s->b;
                nty = s->a;
            } else if (s->kind == AST_VAL_DESTROY) {
                nname = s->b;
                nty = s->a;
            } else if (s->kind == AST_PTR_UNWRAP &&
                       s->d[0] && strstr(s->d, "_D") != NULL) {
                nname = s->b;
                nty = s->a;
            }
            if (nname && nname[0] && shadow_safe_type_is_nursery(nty))
                shadow_safe_unpin_nursery(ctx, nname);
        }
        ctx->nvars = saved_nvars;
        ctx->neh = saved_neh;
        ctx->eh_scope = saved_eh_scope;
        break;
    }
    case AST_IF: {
        char root[64], arm[32];
        int pushed = 0;
        /* Condition only — then/else are separate stmts. */
        shadow_safe_scan_texts(ctx, st);
        if (ctx->err) return;
        if (shadow_safe_parse_kind_eq_guard(ctx, st->a, root, sizeof(root), arm,
                                           sizeof(arm))) {
            shadow_safe_dom_push(ctx, root, arm);
            pushed = 1;
        }
        if (st->nbody > 0) shadow_safe_walk_stmt(ctx, st->body[0]);
        if (pushed) shadow_safe_dom_pop(ctx);
        if (st->nbody > 1) shadow_safe_walk_stmt(ctx, st->body[1]);
        shadow_safe_walk_list(ctx, st->kids, st->nkids);
        break;
    }
    case AST_SWITCH: {
        char root[64];
        shadow_safe_commit_moves_in_text(ctx, st, st->a);
        if (ctx->err) return;
        shadow_safe_commit_moves_in_text(ctx, st, st->d);
        if (ctx->err) return;
        shadow_safe_check_moved_use(ctx, st, st->a);
        if (ctx->err) return;
        shadow_safe_check_moved_use(ctx, st, st->d);
        if (ctx->err) return;
        shadow_safe_check_variant_text(ctx, st, st->a);
        if (ctx->err) return;
        if (shadow_safe_switch_root(st->a, root, sizeof(root)))
            shadow_safe_check_switch_variant_body(ctx, st, root, st->d);
        else
            shadow_safe_check_variant_text(ctx, st, st->d);
        if (ctx->err) return;
        shadow_safe_check_type_of_text(ctx, st, st->a);
        if (ctx->err) return;
        shadow_safe_check_type_of_text(ctx, st, st->d);
        if (ctx->err) return;
        /* Structured switches keep case/stmt nodes on body[]. */
        shadow_safe_walk_list(ctx, st->body, st->nbody);
        break;
    }
    case AST_FOR: {
        /* Bind `for (int i = 0; …)` / `for (size_t n; …)` so chan_send(i)
         * sees a tracked scalar (not "untracked value"). */
        const char* h = st->a;
        char ty[32], name[64];
        size_t ti = 0, ni = 0;
        ShadowSafeVar* fv;
        while (h && (*h == ' ' || *h == '\t')) h++;
        if (h && ((strncmp(h, "int", 3) == 0 && !shadow_safe_is_word(h[3])) ||
                  (strncmp(h, "size_t", 6) == 0 && !shadow_safe_is_word(h[6])) ||
                  (strncmp(h, "long", 4) == 0 && !shadow_safe_is_word(h[4])) ||
                  (strncmp(h, "bool", 4) == 0 && !shadow_safe_is_word(h[4])) ||
                  (strncmp(h, "uint64_t", 8) == 0 && !shadow_safe_is_word(h[8])) ||
                  (strncmp(h, "int64_t", 7) == 0 && !shadow_safe_is_word(h[7])) ||
                  (strncmp(h, "uint32_t", 8) == 0 && !shadow_safe_is_word(h[8])) ||
                  (strncmp(h, "int32_t", 7) == 0 && !shadow_safe_is_word(h[7])))) {
            while (*h && shadow_safe_is_word(*h) && ti + 1 < sizeof(ty))
                ty[ti++] = *h++;
            ty[ti] = 0;
            while (*h == ' ' || *h == '\t') h++;
            if (*h && ((*h >= 'A' && *h <= 'Z') || (*h >= 'a' && *h <= 'z') ||
                       *h == '_')) {
                while (*h && shadow_safe_is_word(*h) && ni + 1 < sizeof(name))
                    name[ni++] = *h++;
                name[ni] = 0;
                fv = name[0] ? shadow_safe_add(ctx, name) : NULL;
                if (fv) snprintf(fv->ty, sizeof(fv->ty), "%s", ty);
            }
        }
        shadow_safe_scan_texts(ctx, st);
        if (ctx->err) return;
        saved_neh = ctx->neh;
        saved_eh_scope = ctx->eh_scope;
        ctx->eh_scope++;
        shadow_safe_walk_list(ctx, st->body, st->nbody);
        shadow_safe_walk_list(ctx, st->kids, st->nkids);
        ctx->neh = saved_neh;
        ctx->eh_scope = saved_eh_scope;
        break;
    }
    case AST_WHILE:
    case AST_DO_WHILE:
    case AST_DEFER:
    case AST_WITH_DEADLINE:
    case AST_PARALLEL:
    case AST_PARALLEL_FOR:
    case AST_SERIAL:
    case AST_STAGE:
        if (st->kind == AST_DEFER) {
            if (st->a[0]) {
                shadow_safe_err_at(ctx, st,
                                   "@defer name: requires @cancel (unsupported); "
                                   "use unnamed @defer");
                return;
            }
            if ((strcmp(st->c, "ok") == 0 || strcmp(st->c, "err") == 0) &&
                !shadow_safe_fn_returns_result(ctx->cur_fn)) {
                shadow_safe_err_at(
                    ctx, st,
                    "@defer(ok|err) is only valid in a Result-returning function");
                return;
            }
        }
        shadow_safe_scan_texts(ctx, st);
        if (ctx->err) return;
        /* `@parallel wait (gate) for` re-raises captured body/enter errors
         * after the brace — that dispatch needs a handler in scope.
         * Not under `#pragma(@parallel) off`: the sequential lowering has
         * no re-raise edge. */
        if (st->kind == AST_PARALLEL_FOR && st->e[0] && !st->forced_seq &&
            !shadow_pw_bang_eh(st) && !shadow_safe_eh(ctx)) {
            shadow_safe_err_at_col(
                ctx, st, 1,
                "syntax: '@parallel wait (...) for' requires an enclosing "
                "'@errhandler' in scope");
            return;
        }
        saved_neh = ctx->neh;
        saved_eh_scope = ctx->eh_scope;
        ctx->eh_scope++;
        shadow_safe_walk_list(ctx, st->body, st->nbody);
        shadow_safe_walk_list(ctx, st->kids, st->nkids);
        ctx->neh = saved_neh;
        ctx->eh_scope = saved_eh_scope;
        break;
    case AST_VOID_CAST:
        shadow_safe_scan_texts(ctx, st);
        break;
    case AST_VAR_UNWRAP: {
        /* a=name b=proj-expr c=mode — `!>`/`?>` peeled into mode, so the
         * projection text alone looks unprotected; trust bang/qmark modes.
         * Two-arm complement: the inactive-arm handler / `?>` fallback may
         * project the other arm (spec §5a/§10). */
        int prot = st->c[0] && (strncmp(st->c, "bang", 4) == 0 ||
                                strncmp(st->c, "qmark", 5) == 0);
        int complement_pushed = 0;
        char croot[64], carm[32], comp[32];
        shadow_safe_commit_moves_in_text(ctx, st, st->b);
        if (ctx->err) return;
        shadow_safe_check_moved_use(ctx, st, st->b);
        if (ctx->err) return;
        if (!prot) shadow_safe_check_variant_text(ctx, st, st->b);
        if (ctx->err) return;
        if (prot && shadow_safe_parse_proj_arm(ctx, st->b, croot, sizeof(croot),
                                              carm, sizeof(carm)) &&
            shadow_safe_complement_arm(ctx, croot, carm, comp, sizeof(comp))) {
            shadow_safe_dom_push(ctx, croot, comp);
            complement_pushed = 1;
        }
        shadow_safe_check_variant_text(ctx, st, st->e);
        if (ctx->err) {
            if (complement_pushed) shadow_safe_dom_pop(ctx);
            return;
        }
        shadow_safe_check_type_of_text(ctx, st, st->b);
        if (ctx->err) {
            if (complement_pushed) shadow_safe_dom_pop(ctx);
            return;
        }
        if (!ctx->err) shadow_safe_on_unwrap(ctx, st);
        if (!ctx->err) shadow_safe_walk_list(ctx, st->body, st->nbody);
        if (complement_pushed) shadow_safe_dom_pop(ctx);
        break;
    }
    case AST_PTR_UNWRAP: {
        int prot = st->d[0] && (strncmp(st->d, "bang", 4) == 0 ||
                                strncmp(st->d, "qmark", 5) == 0);
        shadow_safe_commit_moves_in_text(ctx, st, st->c);
        if (ctx->err) return;
        shadow_safe_check_moved_use(ctx, st, st->c);
        if (ctx->err) return;
        if (!prot) shadow_safe_check_variant_text(ctx, st, st->c);
        if (ctx->err) return;
        shadow_safe_check_variant_text(ctx, st, st->e);
        if (ctx->err) return;
        if (!ctx->err) shadow_safe_on_unwrap(ctx, st);
        if (!ctx->err) shadow_safe_walk_list(ctx, st->body, st->nbody);
        break;
    }
    case AST_STMT_UNWRAP: {
        int prot = !st->c[0] || strncmp(st->c, "bang", 4) == 0 ||
                   strncmp(st->c, "qmark", 5) == 0;
        shadow_safe_commit_moves_in_text(ctx, st, st->a);
        if (ctx->err) return;
        shadow_safe_check_moved_use(ctx, st, st->a);
        if (ctx->err) return;
        if (!prot) shadow_safe_check_variant_text(ctx, st, st->a);
        if (ctx->err) return;
        if (!ctx->err) shadow_safe_on_unwrap(ctx, st);
        if (!ctx->err) shadow_safe_walk_list(ctx, st->body, st->nbody);
        break;
    }
    case AST_PRINTLN_BANG:
    case AST_PRINTLN_TPL:
    case AST_PRINTLN_BANG_BIND:
        shadow_safe_scan_texts(ctx, st);
        if (!ctx->err) shadow_safe_on_unwrap(ctx, st);
        if (!ctx->err) shadow_safe_walk_list(ctx, st->body, st->nbody);
        break;
    case AST_RESULT_LOCAL:
        shadow_safe_scan_texts(ctx, st);
        shadow_safe_walk_list(ctx, st->body, st->nbody);
        break;
    case AST_RAW_LINE:
        /* Grammar/schema generated C — `.u` and arm stores are allowed. */
        break;
    default:
        shadow_safe_scan_texts(ctx, st);
        shadow_safe_walk_list(ctx, st->body, st->nbody);
        shadow_safe_walk_list(ctx, st->kids, st->nkids);
        break;
    }
}

/* Seed function parameters so channel-send / projections can see them.
 * @variant params get variant flags; others are tracked POD/pointer formals
 * (needed for `&param` pool-recycle sends like pigz `return_input_arena`). */
static void shadow_safe_seed_fn_params(ShadowSafeCtx* ctx, AstNode* fn) {
    const char* params = NULL;
    const char* p;
    if (!ctx || !fn) return;
    if (fn->kind == AST_FN)
        params = fn->b;
    else if (fn->kind == AST_STATIC_FN || fn->kind == AST_ASYNC_FN)
        params = fn->c;
    else if (fn->kind == AST_RESULT_FN)
        params = fn->d;
    if (!params || !params[0] || strcmp(params, "void") == 0) return;
    p = params;
    while (*p) {
        char piece[160];
        size_t n = 0;
        int depth = 0;
        ShadowSafeVariant* vt;
        ShadowSafeVar* v;
        char leaf[64];
        const char* ident;
        int stars = 0;
        while (*p && (*p == ' ' || *p == '\t' || *p == ',')) p++;
        if (!*p) break;
        while (*p && n + 1 < sizeof(piece)) {
            if (*p == '(') depth++;
            else if (*p == ')') {
                if (depth) depth--;
            } else if (*p == ',' && depth == 0)
                break;
            piece[n++] = *p++;
        }
        piece[n] = 0;
        while (n && (piece[n - 1] == ' ' || piece[n - 1] == '\t'))
            piece[--n] = 0;
        if (!n) continue;
        /* Trailing ident. */
        ident = piece + n;
        while (ident > piece && shadow_safe_is_word(ident[-1])) ident--;
        if (ident == piece + n || !shadow_safe_is_word(*ident)) continue;
        /* Stars immediately before ident. */
        {
            const char* q = ident;
            while (q > piece && (q[-1] == ' ' || q[-1] == '\t')) q--;
            while (q > piece && q[-1] == '*') {
                stars = 1;
                q--;
                while (q > piece && (q[-1] == ' ' || q[-1] == '\t')) q--;
            }
        }
        /* Type leaf: first ident in piece (skip const). */
        {
            const char* t = piece;
            size_t tl = 0;
            while (*t == ' ' || *t == '\t') t++;
            if (strncmp(t, "const", 5) == 0 &&
                (t[5] == ' ' || t[5] == '\t')) {
                t += 5;
                while (*t == ' ' || *t == '\t') t++;
            }
            while (shadow_safe_is_word(t[tl])) tl++;
            if (!tl || tl >= sizeof(leaf)) continue;
            memcpy(leaf, t, tl);
            leaf[tl] = 0;
        }
        v = shadow_safe_add(ctx, ident);
        if (!v) return;
        snprintf(v->ty, sizeof(v->ty), "%s", leaf);
        vt = shadow_safe_find_variant(ctx, leaf);
        if (stars) {
            v->is_ptr = 1;
            if (vt) v->is_variant_ptr = 1;
            /* Arena* formals are live handles — not arena-borrow sends. */
            if (shadow_safe_type_is_arena(leaf)) v->is_arena_ptr = 0;
        } else if (vt) {
            v->is_variant = 1;
        }
    }
}

static void shadow_safe_walk_fn(ShadowSafeCtx* ctx, AstNode* fn) {
    int saved_nvars;
    int saved_npin;
    int saved_neh;
    int saved_async;
    AstNode* saved_fn;
    if (!ctx || !fn || ctx->err) return;
    saved_nvars = ctx->nvars;
    saved_npin = ctx->npin;
    saved_neh = ctx->neh;
    saved_async = ctx->in_async;
    saved_fn = ctx->cur_fn;
    ctx->cur_fn = fn;
    if (fn->kind == AST_ASYNC_FN) ctx->in_async = 1;
    shadow_safe_seed_fn_params(ctx, fn);
    shadow_safe_walk_list(ctx, fn->kids, fn->nkids);
    shadow_safe_walk_list(ctx, fn->body, fn->nbody);
    /* Static fn raw-body fallback: scan opaque text under no case dom
     * except when it embeds a switch (handled only if parsed). */
    if (fn->kind == AST_STATIC_FN && fn->d[0] && fn->nkids == 0)
        shadow_safe_check_variant_text(ctx, fn, fn->d);
    ctx->nvars = saved_nvars;
    ctx->npin = saved_npin;
    ctx->neh = saved_neh;
    ctx->in_async = saved_async;
    ctx->cur_fn = saved_fn;
}

/* Missing field that lives one member deep → spell `.base.field`. */
static int shadow_safe_field_on(ShadowSafeStruct* s, const char* field) {
    int i;
    if (!s || !field) return 0;
    for (i = 0; i < s->nf; i++)
        if (strcmp(s->fname[i], field) == 0) return 1;
    return 0;
}

static void shadow_safe_check_field_suggest_text(ShadowSafeCtx* ctx, AstNode* st,
                                                const char* text) {
    const char* p;
    if (!ctx || !st || !text || ctx->err) return;
    p = text;
    while (*p && !ctx->err) {
        char recv[64], field[64];
        size_t ri = 0, fi = 0;
        ShadowSafeVar* v;
        ShadowSafeStruct* outer;
        int i;
        char nested_ty[64];
        ShadowSafeStruct* inner;
        if (!((p[0] >= 'A' && p[0] <= 'Z') || (p[0] >= 'a' && p[0] <= 'z') ||
              p[0] == '_')) {
            p++;
            continue;
        }
        while (((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') ||
                (*p >= '0' && *p <= '9') || *p == '_') &&
               ri + 1 < sizeof(recv))
            recv[ri++] = *p++;
        recv[ri] = 0;
        if (*p != '.') {
            continue;
        }
        p++;
        while (((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') ||
                (*p >= '0' && *p <= '9') || *p == '_') &&
               fi + 1 < sizeof(field))
            field[fi++] = *p++;
        field[fi] = 0;
        if (!recv[0] || !field[0]) continue;
        /* UFCS / method call `recv.meth(` — not a field miss. */
        if (*p == '(' || *p == ':') continue;
        v = NULL;
        for (i = 0; i < ctx->nvars; i++) {
            if (ctx->vars[i] && strcmp(ctx->vars[i]->name, recv) == 0) {
                v = ctx->vars[i];
                break;
            }
        }
        if (!v || !v->ty[0]) continue;
        outer = shadow_safe_find_struct(ctx, v->ty);
        if (!outer) {
            char strip[64];
            shadow_safe_strip_ty(v->ty, strip, sizeof(strip));
            outer = shadow_safe_find_struct(ctx, strip);
        }
        if (!outer || shadow_safe_field_on(outer, field)) continue;
        /* Only the nested-suggest case — bare misses stay host/UFCS. */
        for (i = 0; i < outer->nf; i++) {
            shadow_safe_strip_ty(outer->fty[i], nested_ty, sizeof(nested_ty));
            inner = shadow_safe_find_struct(ctx, nested_ty);
            if (!inner || !shadow_safe_field_on(inner, field)) continue;
            {
                char msg[192];
                snprintf(msg, sizeof(msg),
                         "field not found: %s — member '%s' has it; "
                         "spell '.%s.%s'",
                         field, outer->fname[i], outer->fname[i], field);
                shadow_safe_err_at(ctx, st, msg);
                return;
            }
        }
    }
}

/* Returns 1 on success, 0 if a safety diagnostic was emitted. */
static int shadow_safety_check(AstNode** items, int n, TapeCache* cache) {
    ShadowSafeCtx ctx;
    ShadowSafeStruct structs[SHADOW_SAFE_STRUCT_CAP];
    ShadowSafeVariant variants[SHADOW_SAFE_VARIANT_CAP];
    CCArena safe_ar;
    int i;
    int ok;
    const char* strict;
    memset(&ctx, 0, sizeof(ctx));
    memset(structs, 0, sizeof(structs));
    memset(variants, 0, sizeof(variants));
    safe_ar = cc_arena_heap(64 * 1024);
    ctx.safe_ar = safe_ar.base ? &safe_ar : NULL;
    ctx.cache = cache;
    ctx.structs = structs;
    ctx.nstructs = 0;
    ctx.variants = variants;
    ctx.nvariants = 0;
    shadow_safe_seed_type_names(&ctx);
    shadow_apply_typeview_as_faces(items, n);
    strict = getenv("CC_STRICT_RESULT_UNWRAP");
    /* Default on; only "0"/"false"/"off" disable. */
    ctx.strict_unhandled = 1;
    if (strict && (strcmp(strict, "0") == 0 || strcmp(strict, "false") == 0 ||
                   strcmp(strict, "off") == 0))
        ctx.strict_unhandled = 0;
    g_shadow_safe_nconst_fns = 0;
    /* Note user-installed map keys from tape sources (cc_map_key_hash_K). */
    if (cache) {
        int fi;
        for (fi = 0; fi < cache->n; fi++) {
            FileTape* ft = cache->items[fi];
            const char* bytes;
            size_t off;
            if (!ft || !ft->bytes || ft->len < 16) continue;
            bytes = ft->bytes;
            for (off = 0; off + 16 < ft->len; off++) {
                const char* pre = "cc_map_key_hash_";
                size_t pl = 16; /* strlen("cc_map_key_hash_") */
                size_t e, sl;
                char suf[64];
                char eqn[96];
                if (memcmp(bytes + off, pre, pl) != 0) continue;
                if (off > 0 && shadow_safe_is_word(bytes[off - 1])) continue;
                e = off + pl;
                while (e < ft->len && shadow_safe_is_word(bytes[e])) e++;
                sl = e - (off + pl);
                if (!sl || sl >= sizeof(suf)) continue;
                memcpy(suf, bytes + off + pl, sl);
                suf[sl] = 0;
                snprintf(eqn, sizeof(eqn), "cc_map_key_eq_%s", suf);
                if (strstr(bytes, eqn) != NULL)
                    shadow_safe_note_map_key(&ctx, suf);
            }
        }
    }
    /* Also note from AST fn names (tape scan can miss after heavy includes). */
    for (i = 0; i < n; i++) {
        AstNode* it = items[i];
        const char* fname = NULL;
        const char* pre = "cc_map_key_hash_";
        size_t pl = 16; /* strlen("cc_map_key_hash_") */
        if (!it) continue;
        if (it->kind == AST_STATIC_FN || it->kind == AST_FN ||
            it->kind == AST_FN_PROTO || it->kind == AST_RAW_LINE)
            fname = it->kind == AST_RAW_LINE ? it->a : it->b;
        if (!fname || strncmp(fname, pre, pl) != 0) {
            if (it->kind == AST_RAW_LINE && it->a[0]) {
                const char* hit = strstr(it->a, pre);
                if (hit && (hit == it->a || !shadow_safe_is_word(hit[-1])))
                    fname = hit;
                else
                    fname = NULL;
            } else
                fname = NULL;
        }
        if (fname && strncmp(fname, pre, pl) == 0) {
            char suf[64];
            size_t sl = 0;
            fname += pl;
            while (fname[sl] && shadow_safe_is_word(fname[sl]) &&
                   sl + 1 < sizeof(suf))
                sl++;
            if (sl) {
                memcpy(suf, fname, sl);
                suf[sl] = 0;
                shadow_safe_note_map_key(&ctx, suf);
            }
        }
    }
    for (i = 0; i < n; i++) {
        AstNode* it = items[i];
        const char* fname;
        const char* params;
        if (!it) continue;
        /* File-scope names for escaping-closure / store-global checks. */
        if (it->kind == AST_VAR_DECL || it->kind == AST_TYPED_INIT ||
            it->kind == AST_PTR_INIT || it->kind == AST_VAL_DESTROY ||
            it->kind == AST_SLICE_VAR || it->kind == AST_PTR_DECL ||
            it->kind == AST_STATIC_VAR || it->kind == AST_GLOBAL_ARR) {
            if (it->b[0]) shadow_safe_add_global(&ctx, it->b);
        } else if (it->kind == AST_CHAN_VAR || it->kind == AST_VAR_INT) {
            if (it->a[0]) shadow_safe_add_global(&ctx, it->a);
        }
        /* Container instances used in the TU are codegen-registered. */
        {
            const char* tys[4];
            int ti;
            tys[0] = it->a;
            tys[1] = it->b;
            tys[2] = it->c;
            tys[3] = it->e;
            for (ti = 0; ti < 4; ti++) {
                const char* t = tys[ti];
                char base[96];
                size_t bn = 0;
                if (!t || !t[0]) continue;
                if (strncmp(t, "CCVec_", 6) != 0 && strncmp(t, "Map_", 4) != 0 &&
                    strncmp(t, "ArrayMap_", 9) != 0)
                    continue;
                while (t[bn] && t[bn] != '*' && t[bn] != ' ' &&
                       bn + 1 < sizeof(base))
                    bn++;
                memcpy(base, t, bn);
                base[bn] = 0;
                if (base[0]) shadow_safe_add_type_name(&ctx, base);
            }
            if (it->kind == AST_FN || it->kind == AST_STATIC_FN ||
                it->kind == AST_ASYNC_FN || it->kind == AST_RESULT_FN) {
                int k;
                for (k = 0; k < it->nkids; k++) {
                    AstNode* kid = it->kids[k];
                    char base[96];
                    size_t bn = 0;
                    if (!kid || !kid->a[0]) continue;
                    if (strncmp(kid->a, "CCVec_", 6) != 0 &&
                        strncmp(kid->a, "Map_", 4) != 0 &&
                        strncmp(kid->a, "ArrayMap_", 9) != 0)
                        continue;
                    while (kid->a[bn] && kid->a[bn] != '*' &&
                           kid->a[bn] != ' ' && bn + 1 < sizeof(base))
                        bn++;
                    memcpy(base, kid->a, bn);
                    base[bn] = 0;
                    if (base[0]) shadow_safe_add_type_name(&ctx, base);
                }
            }
        }
        if (it->kind == AST_TYPEDEF_STRUCT) {
            shadow_safe_register_struct(&ctx, it);
            if (it->b[0]) shadow_safe_add_type_name(&ctx, it->b);
            if (it->a[0]) shadow_safe_add_type_name(&ctx, it->a);
        } else if (it->kind == AST_TYPEDEF_INT || it->kind == AST_TYPEDEF_ENUM ||
                   it->kind == AST_TYPEDEF_FN_PTR ||
                   it->kind == AST_TYPEDEF_CHAN) {
            if (it->kind == AST_TYPEDEF_CHAN && it->a[0]) {
                int role = 0;
                if (it->c[0] == '>') role = 1;
                else if (it->c[0] == '<') role = 2;
                shadow_safe_add_type_name(&ctx, it->a);
                if (role) shadow_safe_add_chan_td(&ctx, it->a, role);
            } else if (it->kind == AST_TYPEDEF_INT && it->a[0]) {
                shadow_safe_add_type_name(&ctx, it->a);
                if (it->b[0]) shadow_safe_add_td_alias(&ctx, it->a, it->b);
            } else if (it->b[0])
                shadow_safe_add_type_name(&ctx, it->b);
            else if (it->a[0])
                shadow_safe_add_type_name(&ctx, it->a);
            if (it->kind == AST_TYPEDEF_INT) {
                if (it->a[0]) shadow_safe_check_map_key_text(&ctx, it, it->a);
                if (it->b[0]) shadow_safe_check_map_key_text(&ctx, it, it->b);
            }
        } else if (it->kind == AST_STRUCT && it->a[0]) {
            shadow_safe_add_type_name(&ctx, it->a);
        }
        if (it->kind == AST_AT_STMT && strcmp(it->a, "variant") == 0) {
            shadow_safe_register_variant(&ctx, it);
            if (it->b[0]) shadow_safe_add_type_name(&ctx, it->b);
        }
        if (it->kind == AST_RAW_LINE && it->a[0]) {
            const char* line = it->a;
            while (*line == ' ' || *line == '\t') line++;
            if (strncmp(line, "CC_TYPE_INFO_BEGIN", 17) == 0) {
                const char* lp = strchr(line, '(');
                char nm[64];
                size_t ni = 0;
                if (lp) {
                    lp++;
                    while (*lp == ' ' || *lp == '\t') lp++;
                    while (*lp && *lp != ')' && *lp != ',' &&
                           ni + 1 < sizeof(nm))
                        nm[ni++] = *lp++;
                    nm[ni] = 0;
                    if (nm[0]) shadow_safe_add_type_name(&ctx, nm);
                }
            }
        }
        if (it->kind == AST_RESULT_FN && it->a[0])
            shadow_safe_add_result_fn_err(&ctx, it->a,
                                          it->b[0] ? it->b : NULL);
        fname = NULL;
        params = NULL;
        if (it->kind == AST_FN) {
            fname = it->a;
            params = it->c;
        } else if (it->kind == AST_STATIC_FN || it->kind == AST_FN_PROTO) {
            fname = it->b;
            params = it->c;
        } else if (it->kind == AST_RESULT_FN) {
            fname = it->a;
            params = it->d;
        }
        if (fname && params && strstr(params, "const") != NULL &&
            strchr(params, '*') != NULL)
            shadow_safe_add_const_ptr_fn(fname);
    }
    shadow_safe_register_schema_variants(&ctx);
    shadow_safe_check_as_embeds(&ctx, items, n);
    for (i = 0; i < n && !ctx.err; i++) {
        AstNode* it = items[i];
        if (!it) continue;
        if (it->kind == AST_FN || it->kind == AST_STATIC_FN ||
            it->kind == AST_ASYNC_FN || it->kind == AST_RESULT_FN)
            shadow_safe_walk_fn(&ctx, it);
        else
            shadow_safe_walk_stmt(&ctx, it);
    }
    ok = ctx.err ? 0 : 1;
    if (safe_ar.base) cc_arena_free(&safe_ar);
    return ok;
}
