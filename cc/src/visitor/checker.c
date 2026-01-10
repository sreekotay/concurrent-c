#include "checker.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

// Slice flag tracking scaffold. As the parser starts emitting CC AST nodes,
// populate flags on expressions and enforce send_take eligibility.

typedef enum {
    CC_SLICE_UNKNOWN = 0,
    CC_SLICE_UNIQUE = 1 << 0,
    CC_SLICE_TRANSFERABLE = 1 << 1,
    CC_SLICE_SUBSLICE = 1 << 2,
} CCSliceFlags;

typedef struct {
    const char* name; /* borrowed */
    int is_slice;
    int is_array;
    int is_stack_slice_view;
    int move_only;
    int moved;
    int pending_move;
    int decl_line;
    int decl_col;
} CCSliceVar;

typedef struct {
    CCSliceVar* vars;
    int vars_len;
    int vars_cap;
} CCScope;

static CCSliceVar* cc__scope_find(CCScope* sc, const char* name) {
    if (!sc || !name) return NULL;
    for (int i = 0; i < sc->vars_len; i++) {
        if (sc->vars[i].name && strcmp(sc->vars[i].name, name) == 0)
            return &sc->vars[i];
    }
    return NULL;
}

static CCSliceVar* cc__scopes_lookup(CCScope* scopes, int n, const char* name) {
    for (int i = n - 1; i >= 0; i--) {
        CCSliceVar* v = cc__scope_find(&scopes[i], name);
        if (v) return v;
    }
    return NULL;
}

static CCSliceVar* cc__scope_add(CCScope* sc, const char* name) {
    if (!sc || !name) return NULL;
    CCSliceVar* ex = cc__scope_find(sc, name);
    if (ex) return ex;
    if (sc->vars_len == sc->vars_cap) {
        int nc = sc->vars_cap ? sc->vars_cap * 2 : 32;
        CCSliceVar* nv = (CCSliceVar*)realloc(sc->vars, (size_t)nc * sizeof(CCSliceVar));
        if (!nv) return NULL;
        sc->vars = nv;
        sc->vars_cap = nc;
    }
    CCSliceVar* v = &sc->vars[sc->vars_len++];
    memset(v, 0, sizeof(*v));
    v->name = name;
    return v;
}

static void cc__scope_free(CCScope* sc) {
    if (!sc) return;
    free(sc->vars);
    sc->vars = NULL;
    sc->vars_len = 0;
    sc->vars_cap = 0;
}

static void cc__commit_pending_moves(CCScope* scopes, int scope_n) {
    if (!scopes || scope_n <= 0) return;
    for (int i = 0; i < scope_n; i++) {
        CCScope* sc = &scopes[i];
        for (int j = 0; j < sc->vars_len; j++) {
            if (sc->vars[j].pending_move) {
                sc->vars[j].moved = 1;
                sc->vars[j].pending_move = 0;
            }
        }
    }
}

typedef struct StubNodeView {
    int kind;
    int parent;
    const char* file;
    int line_start;
    int line_end;
    int col_start;
    int col_end;
    int aux1;
    int aux2;
    const char* aux_s1;
    const char* aux_s2;
} StubNodeView;

enum {
    CC_STUB_DECL = 1,
    CC_STUB_BLOCK = 2,
    CC_STUB_STMT = 3,
    CC_STUB_ARENA = 4,
    CC_STUB_CALL = 5,
    CC_STUB_AWAIT = 6,
    CC_STUB_SEND_TAKE = 7,
    CC_STUB_SUBSLICE = 8,
    CC_STUB_CLOSURE = 9,
    CC_STUB_IDENT = 10,
    CC_STUB_CONST = 11,
    CC_STUB_DECL_ITEM = 12,
    CC_STUB_MEMBER = 13,
    CC_STUB_ASSIGN = 14,
    CC_STUB_RETURN = 15,
    CC_STUB_PARAM = 16,
};

typedef struct {
    int* child;
    int len;
    int cap;
} ChildList;

static void cc__child_push(ChildList* cl, int idx) {
    if (!cl) return;
    if (cl->len == cl->cap) {
        int nc = cl->cap ? cl->cap * 2 : 8;
        int* nv = (int*)realloc(cl->child, (size_t)nc * sizeof(int));
        if (!nv) return;
        cl->child = nv;
        cl->cap = nc;
    }
    cl->child[cl->len++] = idx;
}

static void cc__emit_err(const CCCheckerCtx* ctx, const StubNodeView* n, const char* msg) {
    const char* path = (ctx && ctx->input_path) ? ctx->input_path : (n && n->file ? n->file : "<src>");
    int line = n ? n->line_start : 0;
    int col = n ? n->col_start : 0;
    if (col <= 0) col = 1;
    fprintf(stderr, "%s:%d:%d: error: %s\n", path, line, col, msg);
}

static int cc__call_has_unique_flag(const StubNodeView* nodes, const ChildList* kids, int call_idx) {
    if (!nodes || !kids) return 0;
    const ChildList* cl = &kids[call_idx];
    /* We care about the 2nd argument to cc_slice_make_id(alloc_id, unique, transferable, is_sub). */
    int arg_pos = 0;
    for (int i = 0; i < cl->len; i++) {
        const StubNodeView* c = &nodes[cl->child[i]];
        if (c->kind == CC_STUB_CONST && c->aux_s1) {
            arg_pos++;
            if (arg_pos == 2 && strcmp(c->aux_s1, "1") == 0) return 1;
        }
        if (c->kind == CC_STUB_IDENT && c->aux_s1) {
            arg_pos++;
            if (arg_pos == 2 && strcmp(c->aux_s1, "true") == 0) return 1;
            if (strcmp(c->aux_s1, "CC_SLICE_ID_UNIQUE") == 0) return 1;
        }
    }
    return 0;
}

static int cc__subtree_has_call_named(const StubNodeView* nodes,
                                      const ChildList* kids,
                                      int idx,
                                      const char* name) {
    if (!nodes || !kids || !name) return 0;
    const StubNodeView* n = &nodes[idx];
    if (n->kind == CC_STUB_CALL && n->aux_s1 && strcmp(n->aux_s1, name) == 0) return 1;
    const ChildList* cl = &kids[idx];
    for (int i = 0; i < cl->len; i++) {
        if (cc__subtree_has_call_named(nodes, kids, cl->child[i], name)) return 1;
    }
    return 0;
}

static int cc__subtree_find_first_ident_matching_scope(const StubNodeView* nodes,
                                                       const ChildList* kids,
                                                       int idx,
                                                       CCScope* scopes,
                                                       int scope_n,
                                                       const char* exclude_name,
                                                       const char** out_name) {
    if (!nodes || !kids || !scopes || !out_name) return 0;
    const StubNodeView* n = &nodes[idx];
    if (n->kind == CC_STUB_IDENT && n->aux_s1) {
        if (!exclude_name || strcmp(n->aux_s1, exclude_name) != 0) {
            CCSliceVar* v = cc__scopes_lookup(scopes, scope_n, n->aux_s1);
            if (v) { *out_name = n->aux_s1; return 1; }
        }
    }
    const ChildList* cl = &kids[idx];
    for (int i = 0; i < cl->len; i++) {
        if (cc__subtree_find_first_ident_matching_scope(nodes, kids, cl->child[i], scopes, scope_n, exclude_name, out_name))
            return 1;
    }
    return 0;
}

static int cc__subtree_has_unique_make_id(const StubNodeView* nodes,
                                          const ChildList* kids,
                                          int idx) {
    if (!nodes || !kids) return 0;
    const StubNodeView* n = &nodes[idx];
    if (n->kind == CC_STUB_CALL && n->aux_s1 && strcmp(n->aux_s1, "cc_slice_make_id") == 0) {
        return cc__call_has_unique_flag(nodes, kids, idx);
    }
    if (n->kind == CC_STUB_IDENT && n->aux_s1 && strcmp(n->aux_s1, "CC_SLICE_ID_UNIQUE") == 0) return 1;
    const ChildList* cl = &kids[idx];
    for (int i = 0; i < cl->len; i++) {
        if (cc__subtree_has_unique_make_id(nodes, kids, cl->child[i])) return 1;
    }
    return 0;
}

static int cc__subtree_collect_call_names(const StubNodeView* nodes,
                                         const ChildList* kids,
                                         int idx,
                                         const char** out_names,
                                         int* io_n,
                                         int cap) {
    if (!nodes || !kids || !out_names || !io_n) return 0;
    const StubNodeView* n = &nodes[idx];
    if (n->kind == CC_STUB_CALL && n->aux_s1) {
        int seen = 0;
        for (int i = 0; i < *io_n; i++) if (strcmp(out_names[i], n->aux_s1) == 0) seen = 1;
        if (!seen && *io_n < cap) out_names[(*io_n)++] = n->aux_s1;
    }
    const ChildList* cl = &kids[idx];
    for (int i = 0; i < cl->len; i++) {
        cc__subtree_collect_call_names(nodes, kids, cl->child[i], out_names, io_n, cap);
    }
    return 1;
}

static int cc__closure_allows_stack_capture(const StubNodeView* nodes, int closure_idx) {
    /* Allowed only when directly under a nursery `spawn (...)` statement. */
    int cur = nodes[closure_idx].parent;
    while (cur >= 0) {
        if (nodes[cur].kind == CC_STUB_STMT && nodes[cur].aux_s1 && strcmp(nodes[cur].aux_s1, "spawn") == 0)
            return 1;
        cur = nodes[cur].parent;
    }
    return 0;
}

static int cc__closure_is_escaping(const StubNodeView* nodes, int closure_idx) {
    /* Conservative: anything not a nursery spawn is treated as escaping for stack-slice purposes. */
    if (cc__closure_allows_stack_capture(nodes, closure_idx)) return 0;
    return 1;
}

static int cc__closure_has_illegal_stack_capture(int closure_idx,
                                                 const StubNodeView* nodes,
                                                 const ChildList* kids,
                                                 CCScope* scopes,
                                                 int scope_n) {
    /* If the closure is not escaping, allow. */
    if (!cc__closure_is_escaping(nodes, closure_idx)) return 0;

    /* Build set of local names declared inside the closure (decl items + params). */
    const char* locals[256];
    int locals_n = 0;
    int stack[512];
    int sp = 0;
    stack[sp++] = closure_idx;
    while (sp > 0) {
        int cur = stack[--sp];
        const StubNodeView* n = &nodes[cur];
        if ((n->kind == CC_STUB_DECL_ITEM || n->kind == CC_STUB_PARAM) && n->aux_s1) {
            int seen = 0;
            for (int i = 0; i < locals_n; i++) if (strcmp(locals[i], n->aux_s1) == 0) seen = 1;
            if (!seen && locals_n < (int)(sizeof(locals)/sizeof(locals[0]))) locals[locals_n++] = n->aux_s1;
        }
        const ChildList* cl = &kids[cur];
        for (int i = 0; i < cl->len && sp < (int)(sizeof(stack)/sizeof(stack[0])); i++) {
            stack[sp++] = cl->child[i];
        }
    }

    /* Collect call names so we can skip callee identifier tokens. */
    const char* call_names[64];
    int call_n = 0;
    cc__subtree_collect_call_names(nodes, kids, closure_idx, call_names, &call_n, 64);

    /* Scan ident uses in closure subtree. */
    sp = 0;
    stack[sp++] = closure_idx;
    while (sp > 0) {
        int cur = stack[--sp];
        const StubNodeView* n = &nodes[cur];
        if (n->kind == CC_STUB_IDENT && n->aux_s1) {
            const char* nm = n->aux_s1;
            int is_call = 0;
            for (int i = 0; i < call_n; i++) if (strcmp(call_names[i], nm) == 0) is_call = 1;
            if (!is_call) {
                int is_local = 0;
                for (int i = 0; i < locals_n; i++) if (strcmp(locals[i], nm) == 0) is_local = 1;
                if (!is_local) {
                    CCSliceVar* v = cc__scopes_lookup(scopes, scope_n, nm);
                    if (v && v->is_slice && v->is_stack_slice_view) return 1;
                }
            }
        }
        const ChildList* cl = &kids[cur];
        for (int i = 0; i < cl->len && sp < (int)(sizeof(stack)/sizeof(stack[0])); i++) {
            stack[sp++] = cl->child[i];
        }
    }

    return 0;
}

static int cc__subtree_should_apply_slice_copy_rule(const StubNodeView* nodes,
                                                    const ChildList* kids,
                                                    int idx,
                                                    const char* lhs_name,
                                                    const char* rhs_name) {
    if (!nodes || !kids || !rhs_name) return 0;
    const char* call_names[64];
    int call_n = 0;
    cc__subtree_collect_call_names(nodes, kids, idx, call_names, &call_n, 64);

    /* Count non-function identifier tokens in the subtree. If we see more than the rhs itself,
       this is likely a projection (e.g. `s.ptr`) rather than a slice copy. */
    int rhs_seen = 0;
    int other_ident = 0;
    int saw_member = 0;

    /* Simple DFS stack */
    int stack[256];
    int sp = 0;
    stack[sp++] = idx;
    while (sp > 0) {
        int cur = stack[--sp];
        const StubNodeView* n = &nodes[cur];
        if (n->kind == CC_STUB_MEMBER) {
            saw_member = 1;
        }
        if (n->kind == CC_STUB_IDENT && n->aux_s1) {
            const char* nm = n->aux_s1;
            if (lhs_name && strcmp(nm, lhs_name) == 0) {
                /* ignore lhs */
            } else {
                int is_call = 0;
                for (int i = 0; i < call_n; i++) if (strcmp(call_names[i], nm) == 0) is_call = 1;
                if (!is_call && strcmp(nm, "true") != 0 && strcmp(nm, "false") != 0 && strcmp(nm, "NULL") != 0) {
                    if (strcmp(nm, rhs_name) == 0) rhs_seen = 1;
                    else other_ident = 1;
                }
            }
        }
        const ChildList* cl = &kids[cur];
        for (int i = 0; i < cl->len && sp < (int)(sizeof(stack)/sizeof(stack[0])); i++) {
            stack[sp++] = cl->child[i];
        }
    }
    return rhs_seen && !other_ident && !saw_member;
}

static int cc__walk(int idx,
                    const StubNodeView* nodes,
                    const ChildList* kids,
                    CCScope* scopes,
                    int* io_scope_n,
                    CCCheckerCtx* ctx);

static int cc__walk_call(int idx,
                         const StubNodeView* nodes,
                         const ChildList* kids,
                         CCScope* scopes,
                         int* io_scope_n,
                         CCCheckerCtx* ctx) {
    const StubNodeView* n = &nodes[idx];
    if (!n->aux_s1) return 0;

    /* Move markers (parse-only): cc__move_marker_impl(&x) */
    if (strcmp(n->aux_s1, "cc__move_marker_impl") == 0) {
        const ChildList* cl = &kids[idx];
        CCSliceVar* to_move[16];
        int to_move_n = 0;
        for (int i = 0; i < cl->len; i++) {
            const StubNodeView* c = &nodes[cl->child[i]];
            if (c->kind == CC_STUB_IDENT && c->aux_s1) {
                /* The recorder also emits the callee as an IDENT child; ignore that and
                   mark any slice variable args as moved. */
                if (strcmp(c->aux_s1, "cc__move_marker_impl") == 0) continue;
                CCSliceVar* v = cc__scopes_lookup(scopes, *io_scope_n, c->aux_s1);
                if (v && v->is_slice && to_move_n < (int)(sizeof(to_move)/sizeof(to_move[0]))) {
                    to_move[to_move_n++] = v;
                }
            }
        }

        /* Walk children first: `cc_move(x)` should not report use-after-move of `x` inside the same expression. */
        for (int i = 0; i < cl->len; i++) {
            if (cc__walk(cl->child[i], nodes, kids, scopes, io_scope_n, ctx) != 0) return -1;
        }
        for (int i = 0; i < to_move_n; i++) {
            if (to_move[i]) to_move[i]->pending_move = 1;
        }
        return 0;
    }

    /* walk children */
    const ChildList* cl = &kids[idx];
    for (int i = 0; i < cl->len; i++) {
        if (cc__walk(cl->child[i], nodes, kids, scopes, io_scope_n, ctx) != 0) return -1;
    }
    return 0;
}

static int cc__walk_closure(int idx,
                            const StubNodeView* nodes,
                            const ChildList* kids,
                            CCScope* scopes,
                            int* io_scope_n,
                            CCCheckerCtx* ctx) {
    /* Walk closure body in a nested scope, collecting captures of move-only slices. */
    CCScope closure_scope = {0};
    scopes[(*io_scope_n)++] = closure_scope;

    /* Collect names declared inside the closure (decl items + params). */
    const char* locals[256];
    int locals_n = 0;
    const ChildList* cl0 = &kids[idx];
    for (int i = 0; i < cl0->len; i++) {
        const StubNodeView* c = &nodes[cl0->child[i]];
        if ((c->kind == CC_STUB_DECL_ITEM || c->kind == CC_STUB_PARAM) && c->aux_s1) {
            int seen = 0;
            for (int k = 0; k < locals_n; k++) if (strcmp(locals[k], c->aux_s1) == 0) seen = 1;
            if (!seen && locals_n < (int)(sizeof(locals)/sizeof(locals[0]))) locals[locals_n++] = c->aux_s1;
        }
    }

    /* Collect call names so we can skip callee identifier tokens. */
    const char* call_names[64];
    int call_n = 0;
    cc__subtree_collect_call_names(nodes, kids, idx, call_names, &call_n, 64);

    /* Collect identifier uses in the closure subtree (excluding locals/params and callees). */
    const char* used_names[256];
    int used_n = 0;
    {
        /* DFS over closure subtree */
        int stack[512];
        int sp = 0;
        stack[sp++] = idx;
        while (sp > 0) {
            int cur = stack[--sp];
            const StubNodeView* n = &nodes[cur];
            if (n->kind == CC_STUB_IDENT && n->aux_s1) {
                const char* nm = n->aux_s1;
                int is_call = 0;
                for (int i = 0; i < call_n; i++) if (strcmp(call_names[i], nm) == 0) is_call = 1;
                if (!is_call) {
                    int is_local = 0;
                    for (int i = 0; i < locals_n; i++) if (strcmp(locals[i], nm) == 0) is_local = 1;
                    if (!is_local) {
                        int seen = 0;
                        for (int k = 0; k < used_n; k++) if (strcmp(used_names[k], nm) == 0) seen = 1;
                        if (!seen && used_n < (int)(sizeof(used_names)/sizeof(used_names[0]))) used_names[used_n++] = nm;
                    }
                }
            }
            const ChildList* cl = &kids[cur];
            for (int i = 0; i < cl->len && sp < (int)(sizeof(stack)/sizeof(stack[0])); i++) {
                stack[sp++] = cl->child[i];
            }
        }
    }

    const ChildList* cl = &kids[idx];
    for (int i = 0; i < cl->len; i++) {
        int ch = cl->child[i];
        if (cc__walk(ch, nodes, kids, scopes, io_scope_n, ctx) != 0) return -1;
    }

    /* Apply implicit move for captured move-only slices (names used but not declared locally). */
    for (int i = 0; i < used_n; i++) {
        const char* nm = used_names[i];
        if (!nm) continue;
        CCSliceVar* local = cc__scope_find(&scopes[(*io_scope_n) - 1], nm);
        if (local) continue; /* local to closure */
        CCSliceVar* outer = cc__scopes_lookup(scopes, (*io_scope_n) - 1, nm);
        if (outer && outer->is_slice && outer->move_only) outer->moved = 1;
    }

    /* Pop closure scope */
    cc__scope_free(&scopes[(*io_scope_n) - 1]);
    (*io_scope_n)--;
    return 0;
}

static int cc__subtree_has_kind(const StubNodeView* nodes,
                                const ChildList* kids,
                                int idx,
                                int kind) {
    if (!nodes || !kids) return 0;
    if (nodes[idx].kind == kind) return 1;
    const ChildList* cl = &kids[idx];
    for (int i = 0; i < cl->len; i++) {
        if (cc__subtree_has_kind(nodes, kids, cl->child[i], kind)) return 1;
    }
    return 0;
}

static int cc__walk_assign(int idx,
                           const StubNodeView* nodes,
                           const ChildList* kids,
                           CCScope* scopes,
                           int* io_scope_n,
                           CCCheckerCtx* ctx) {
    const StubNodeView* n = &nodes[idx];
    const char* lhs = n->aux_s1; /* best-effort from TCC recorder */
    const char* rhs = NULL;

    (void)cc__subtree_find_first_ident_matching_scope(nodes, kids, idx, scopes, *io_scope_n, lhs, &rhs);

    if (lhs && rhs && strcmp(lhs, rhs) != 0) {
        CCSliceVar* lhs_v = cc__scopes_lookup(scopes, *io_scope_n, lhs);
        CCSliceVar* rhs_v = cc__scopes_lookup(scopes, *io_scope_n, rhs);
        int has_move_marker = cc__subtree_has_call_named(nodes, kids, idx, "cc__move_marker_impl");
        int saw_member = cc__subtree_has_kind(nodes, kids, idx, CC_STUB_MEMBER);

        if (rhs_v && rhs_v->is_slice) {
            /* Overwrite clears moved-from status for lhs. */
            if (lhs_v) lhs_v->moved = 0;

            /* If we assign from a slice var, treat lhs as a slice var too. */
            if (lhs_v) lhs_v->is_slice = 1;

            /* Only treat as a slice copy/move when RHS isn't being projected via member access. */
            if (!saw_member) {
                if (rhs_v->move_only && !has_move_marker) {
                    cc__emit_err(ctx, n, "CC: cannot copy move-only slice (use cc_move(x))");
                    ctx->errors++;
                    return -1;
                }
                if (rhs_v->move_only && has_move_marker) {
                    rhs_v->moved = 1;
                    if (lhs_v) lhs_v->move_only = 1;
                } else if (lhs_v && !has_move_marker) {
                    lhs_v->move_only = 0;
                }
            }
        }
    }

    const ChildList* cl = &kids[idx];
    for (int i = 0; i < cl->len; i++) {
        if (cc__walk(cl->child[i], nodes, kids, scopes, io_scope_n, ctx) != 0) return -1;
    }
    /* Commit pending moves at full-expression boundary. */
    cc__commit_pending_moves(scopes, *io_scope_n);
    return 0;
}

static int cc__walk_return(int idx,
                           const StubNodeView* nodes,
                           const ChildList* kids,
                           CCScope* scopes,
                           int* io_scope_n,
                           CCCheckerCtx* ctx) {
    const StubNodeView* n = &nodes[idx];
    const char* name = NULL;
    (void)cc__subtree_find_first_ident_matching_scope(nodes, kids, idx, scopes, *io_scope_n, NULL, &name);
    if (name) {
        CCSliceVar* v = cc__scopes_lookup(scopes, *io_scope_n, name);
        if (v && v->is_slice) {
            int saw_member = cc__subtree_has_kind(nodes, kids, idx, CC_STUB_MEMBER);
            int has_move_marker = cc__subtree_has_call_named(nodes, kids, idx, "cc__move_marker_impl");
            if (v->move_only && !has_move_marker && !saw_member) {
                cc__emit_err(ctx, n, "CC: cannot return move-only slice (use cc_move(x))");
                ctx->errors++;
                return -1;
            }
            if (v->move_only && has_move_marker) v->moved = 1;
        }
    }

    const ChildList* cl = &kids[idx];
    for (int i = 0; i < cl->len; i++) {
        if (cc__walk(cl->child[i], nodes, kids, scopes, io_scope_n, ctx) != 0) return -1;
    }
    /* Commit pending moves at full-expression boundary. */
    cc__commit_pending_moves(scopes, *io_scope_n);
    return 0;
}

static int cc__walk(int idx,
                    const StubNodeView* nodes,
                    const ChildList* kids,
                    CCScope* scopes,
                    int* io_scope_n,
                    CCCheckerCtx* ctx) {
    const StubNodeView* n = &nodes[idx];

    if (n->kind == CC_STUB_DECL_ITEM && n->aux_s1 && n->aux_s2) {
        CCScope* cur = &scopes[(*io_scope_n) - 1];
        CCSliceVar* v = cc__scope_add(cur, n->aux_s1);
        if (!v) return -1;
        v->decl_line = n->line_start;
        v->decl_col = n->col_start;
        v->is_slice = (strstr(n->aux_s2, "CCSlice") != NULL);
        if (strchr(n->aux_s2, '[') && strchr(n->aux_s2, ']')) {
            v->is_array = 1;
        }

        /* Determine move_only from initializer subtree */
        {
            const ChildList* cl = &kids[idx];
            const char* copy_from = NULL;
            int saw_slice_ctor = 0;

            for (int i = 0; i < cl->len; i++) {
                const StubNodeView* c = &nodes[cl->child[i]];
                if (c->kind == CC_STUB_CALL && c->aux_s1) {
                    if (strncmp(c->aux_s1, "cc_slice_", 9) == 0) saw_slice_ctor = 1;
                }
            }

            /* If initializer is a known slice constructor, treat as slice even if the type string
               prints as 'struct <anonymous>' (CCSlice is a typedef of an anonymous struct). */
            if (saw_slice_ctor) v->is_slice = 1;

            if (v->is_slice) {
                /* move-only by provenance: detect unique-id construction anywhere under initializer */
                if (cc__subtree_has_unique_make_id(nodes, kids, idx)) v->move_only = 1;

                /* Stack-slice view detection (best-effort): if init uses cc_slice_from_buffer/parts with a local array. */
                int uses_buf = cc__subtree_has_call_named(nodes, kids, idx, "cc_slice_from_buffer");
                int uses_parts = cc__subtree_has_call_named(nodes, kids, idx, "cc_slice_from_parts");
                if (uses_buf || uses_parts) {
                    int st[256];
                    int sp = 0;
                    st[sp++] = idx;
                    while (sp > 0) {
                        int curi = st[--sp];
                        const StubNodeView* nn = &nodes[curi];
                        if (nn->kind == CC_STUB_IDENT && nn->aux_s1) {
                            CCSliceVar* maybe = cc__scopes_lookup(scopes, *io_scope_n, nn->aux_s1);
                            if (maybe && maybe->is_array) {
                                v->is_stack_slice_view = 1;
                                break;
                            }
                        }
                        const ChildList* k = &kids[curi];
                        for (int j = 0; j < k->len && sp < (int)(sizeof(st)/sizeof(st[0])); j++)
                            st[sp++] = k->child[j];
                    }
                }
            }

            /* Find a candidate RHS identifier in the initializer (best-effort). */
            (void)cc__subtree_find_first_ident_matching_scope(nodes, kids, idx, scopes, *io_scope_n, v->name, &copy_from);

            /* Copy rule for decl initializers: `CCSlice t = s;` */
            if (copy_from && copy_from != v->name) {
                CCSliceVar* rhs = cc__scopes_lookup(scopes, *io_scope_n, copy_from);
                /* If we see assignment from an existing slice var, treat this decl as slice too
                   (CCSlice prints as 'struct <anonymous>' in type_to_str). */
                if (rhs && rhs->is_slice) v->is_slice = 1;
                int has_move_marker = cc__subtree_has_call_named(nodes, kids, idx, "cc__move_marker_impl");
                int is_simple_copy = cc__subtree_should_apply_slice_copy_rule(nodes, kids, idx, v->name, copy_from);
                if (rhs && rhs->is_slice && rhs->move_only && !has_move_marker && is_simple_copy) {
                    cc__emit_err(ctx, n, "CC: cannot copy move-only slice (use cc_move(x))");
                    ctx->errors++;
                    return -1;
                }
                if (rhs && rhs->is_slice && rhs->move_only && has_move_marker) {
                    /* Moving a move-only slice produces a move-only slice value. */
                    v->move_only = 1;
                }
            }
        }
    }

    if (n->kind == CC_STUB_IDENT && n->aux_s1) {
        CCSliceVar* v = cc__scopes_lookup(scopes, *io_scope_n, n->aux_s1);
        if (v && v->is_slice && v->moved) {
            cc__emit_err(ctx, n, "CC: use after move of slice");
            ctx->errors++;
            return -1;
        }
    }

    if (n->kind == CC_STUB_CALL) {
        return cc__walk_call(idx, nodes, kids, scopes, io_scope_n, ctx);
    }

    if (n->kind == CC_STUB_CLOSURE) {
        if (cc__walk_closure(idx, nodes, kids, scopes, io_scope_n, ctx) != 0) return -1;
        if (cc__closure_has_illegal_stack_capture(idx, nodes, kids, scopes, *io_scope_n)) {
            cc__emit_err(ctx, n, "CC: cannot capture stack slice in escaping closure");
            ctx->errors++;
            return -1;
        }
        return 0;
    }

    if (n->kind == CC_STUB_ASSIGN) {
        return cc__walk_assign(idx, nodes, kids, scopes, io_scope_n, ctx);
    }

    if (n->kind == CC_STUB_RETURN) {
        return cc__walk_return(idx, nodes, kids, scopes, io_scope_n, ctx);
    }

    /* default: recurse */
    const ChildList* cl = &kids[idx];
    for (int i = 0; i < cl->len; i++) {
        if (cc__walk(cl->child[i], nodes, kids, scopes, io_scope_n, ctx) != 0) return -1;
    }
    if (n->kind == CC_STUB_DECL_ITEM) {
        /* Commit pending moves at full-expression boundary of an initializer. */
        cc__commit_pending_moves(scopes, *io_scope_n);
    }
    return 0;
}

int cc_check_ast(const CCASTRoot* root, CCCheckerCtx* ctx) {
    if (!root || !ctx) return -1;
    ctx->errors = 0;

    if (!root->nodes || root->node_count <= 0) {
        /* Transitional: no stub nodes, skip. */
        return 0;
    }
    const StubNodeView* nodes = (const StubNodeView*)root->nodes;
    int n = root->node_count;

    ChildList* kids = (ChildList*)calloc((size_t)n, sizeof(ChildList));
    if (!kids) return -1;
    for (int i = 0; i < n; i++) {
        int p = nodes[i].parent;
        if (p >= 0 && p < n) cc__child_push(&kids[p], i);
    }

    CCScope scopes[256];
    int scope_n = 0;
    memset(scopes, 0, sizeof(scopes));
    scopes[scope_n++] = (CCScope){0};

    for (int i = 0; i < n; i++) {
        if (nodes[i].parent != -1) continue;
        if (cc__walk(i, nodes, kids, scopes, &scope_n, ctx) != 0) break;
    }

    for (int i = 0; i < n; i++) free(kids[i].child);
    free(kids);
    for (int i = 0; i < scope_n; i++) cc__scope_free(&scopes[i]);

    return ctx->errors ? -1 : 0;
}

