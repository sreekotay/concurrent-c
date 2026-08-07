/* @noblock / @blocking / @async fn attrs + call-edge resolution (spec §8.2).
 * Included from pp_ast_core.cch after Parser is defined. */
#pragma once

enum {
    SHADOW_FN_ASYNC    = 1u << 0,
    SHADOW_FN_NOBLOCK  = 1u << 1,
    SHADOW_FN_BLOCKING = 1u << 2,
};

typedef struct {
    char name[64];
    unsigned decl_attrs;
    unsigned def_attrs;
    int has_def;
} ShadowFnAttr;

enum { SHADOW_FN_ATTR_CAP = 128 };
static ShadowFnAttr g_shadow_fn_attrs[SHADOW_FN_ATTR_CAP];
static int g_shadow_nfn_attrs;

static void shadow_fn_attr_reset(void) {
    g_shadow_nfn_attrs = 0;
}

static unsigned shadow_attr_bits_from_ident(const char* ident) {
    if (!ident || !ident[0]) return 0;
    if (strcmp(ident, "noblock") == 0 || strcmp(ident, "nonblocking") == 0)
        return SHADOW_FN_NOBLOCK;
    if (strcmp(ident, "blocking") == 0) return SHADOW_FN_BLOCKING;
    if (strcmp(ident, "async") == 0) return SHADOW_FN_ASYNC;
    return 0;
}

static ShadowFnAttr* shadow_fn_attr_find(const char* name) {
    int i;
    if (!name || !name[0]) return NULL;
    for (i = 0; i < g_shadow_nfn_attrs; i++) {
        if (strcmp(g_shadow_fn_attrs[i].name, name) == 0)
            return &g_shadow_fn_attrs[i];
    }
    return NULL;
}

static void shadow_fn_attr_register(const char* name, unsigned attrs, int is_def) {
    ShadowFnAttr* e;
    if (!name || !name[0] || !attrs) return;
    e = shadow_fn_attr_find(name);
    if (!e) {
        if (g_shadow_nfn_attrs >= SHADOW_FN_ATTR_CAP) return;
        e = &g_shadow_fn_attrs[g_shadow_nfn_attrs++];
        memset(e, 0, sizeof(*e));
        snprintf(e->name, sizeof(e->name), "%s", name);
    }
    if (is_def) {
        e->def_attrs |= attrs;
        e->has_def = 1;
    } else {
        e->decl_attrs |= attrs;
    }
}

static unsigned shadow_fn_attr_lookup(const char* name) {
    ShadowFnAttr* e = shadow_fn_attr_find(name);
    unsigned mode;
    if (!e) return 0;
    /* Def-wins TU-locally for @noblock / @blocking (spec §8.2.2). */
    mode = e->has_def ? e->def_attrs : e->decl_attrs;
    /* Preserve @async from either decl or def. */
    if (e->decl_attrs & SHADOW_FN_ASYNC) mode |= SHADOW_FN_ASYNC;
    if (e->def_attrs & SHADOW_FN_ASYNC) mode |= SHADOW_FN_ASYNC;
    return mode;
}

static int shadow_is_cc_storage_kw(Token t) {
    ShadowKwKind k = shadow_kw(t);
    if (k == SHADOW_KW_STATIC || k == SHADOW_KW_INLINE || k == SHADOW_KW_CONST)
        return 1;
    if (t.kind == TK_IDENT &&
        (spell_eq(t.spell, "extern") || spell_eq(t.spell, "volatile")))
        return 1;
    return 0;
}

/* Consume leading `@IDENT` attrs + C storage-class keywords; OR bits into *attrs. */
static void shadow_parser_skip_decl_specs(Parser* p, unsigned* attrs) {
    int guard = 0;
    if (!p) return;
    while (p->i < p->n && guard++ < 32) {
        Token t = p_peek(p);
        if (tok_eq(t, TK_PUNCT, "@") && p->i + 1 < p->n &&
            p->toks[p->i + 1].kind == TK_IDENT) {
            char ident[64];
            slice_to(ident, sizeof(ident), p->toks[p->i + 1].spell);
            if (strcmp(ident, "async") == 0) break;
            if (attrs) *attrs |= shadow_attr_bits_from_ident(ident);
            p_next(p);
            p_next(p);
            continue;
        }
        if (shadow_is_cc_storage_kw(t)) {
            p_next(p);
            continue;
        }
        if (t.kind == TK_IDENT && spell_eq(t.spell, "volatile")) {
            p_next(p);
            continue;
        }
        break;
    }
}

static int shadow_parser_peek_call_site_attr(Parser* p, unsigned* site_attrs) {
    if (!p || p->i + 2 >= p->n) return 0;
    if (!tok_eq(p_peek(p), TK_PUNCT, "@")) return 0;
    if (p->toks[p->i + 1].kind != TK_IDENT) return 0;
    {
        char ident[64];
        unsigned bits;
        slice_to(ident, sizeof(ident), p->toks[p->i + 1].spell);
        bits = shadow_attr_bits_from_ident(ident);
        if (!bits || bits == SHADOW_FN_ASYNC) return 0;
        if (p->toks[p->i + 2].kind != TK_IDENT) return 0;
        if (site_attrs) *site_attrs = bits;
        return 1;
    }
}

typedef enum {
    SHADOW_CALL_ASYNC,
    SHADOW_CALL_NOBLOCK,
    SHADOW_CALL_BLOCKING,
} ShadowCallMode;

static ShadowCallMode shadow_resolve_call_edge_mode(unsigned owner_attrs,
                                                    unsigned callee_attrs,
                                                    unsigned site_attrs,
                                                    unsigned block_attrs) {
    if (site_attrs & SHADOW_FN_BLOCKING) return SHADOW_CALL_BLOCKING;
    if (site_attrs & SHADOW_FN_NOBLOCK) return SHADOW_CALL_NOBLOCK;
    if (callee_attrs & SHADOW_FN_ASYNC) return SHADOW_CALL_ASYNC;
    if (callee_attrs & SHADOW_FN_NOBLOCK) return SHADOW_CALL_NOBLOCK;
    if (callee_attrs & SHADOW_FN_BLOCKING) return SHADOW_CALL_BLOCKING;
    if (block_attrs & SHADOW_FN_NOBLOCK) return SHADOW_CALL_NOBLOCK;
    if (block_attrs & SHADOW_FN_BLOCKING) return SHADOW_CALL_BLOCKING;
    if (owner_attrs & SHADOW_FN_NOBLOCK) return SHADOW_CALL_NOBLOCK;
    if (owner_attrs & SHADOW_FN_BLOCKING) return SHADOW_CALL_BLOCKING;
    return SHADOW_CALL_BLOCKING;
}
