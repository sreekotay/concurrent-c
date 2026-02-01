/*
 * Closure Lowering Pass
 *
 * Transforms closure literals `(params) => { body }` into:
 * 1. A struct for captured variables (env)
 * 2. An entry function that executes the body
 * 3. A make function that creates the closure
 * 4. Replace the literal with a call to the make function
 *
 * Output pattern:
 *   typedef struct __cc_closure_env_N { ... captures ... } __cc_closure_env_N;
 *   static void __cc_closure_env_N_drop(void* p) { if (p) free(p); }
 *   static void* __cc_closure_entry_N(void* __p, args...) { ... body ... }
 *   static CCClosureN __cc_closure_make_N(captures...) { ... }
 *
 * The closure literal is replaced with: __cc_closure_make_N(captured_values...)
 */

#include "cccn/passes/passes.h"
#include "cccn/util/string_set.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

/* ========================================================================== */
/* Pass Context - All state for the closure pass                              */
/* ========================================================================== */

typedef struct {
    int next_id;           /* Counter for unique closure IDs */
    StringSet globals;     /* Global variables (excluded from captures) */
    StringMap type_map;    /* Variable name -> type string */
} ClosurePassCtx;

static void ctx_init(ClosurePassCtx* ctx) {
    ctx->next_id = 0;
    string_set_init(&ctx->globals);
    string_map_init(&ctx->type_map);
}

static void ctx_free(ClosurePassCtx* ctx) {
    string_set_free(&ctx->globals);
    string_map_free(&ctx->type_map);
}

/* ========================================================================== */
/* Closure Definition List                                                    */
/* ========================================================================== */

typedef struct {
    CCClosureDef* items;
    int len;
    int cap;
} CCClosureDefList;

static void def_list_push(CCClosureDefList* list, CCClosureDef def) {
    if (list->len >= list->cap) {
        int new_cap = list->cap ? list->cap * 2 : 8;
        list->items = realloc(list->items, new_cap * sizeof(CCClosureDef));
        list->cap = new_cap;
    }
    list->items[list->len++] = def;
}

/* ========================================================================== */
/* Closure Variable Tracking (for call transformation)                        */
/* ========================================================================== */

typedef struct {
    char* name;
    int param_count;
} ClosureVar;

typedef struct {
    ClosureVar* items;
    int len;
    int cap;
} ClosureVarList;

static void closure_var_add(ClosureVarList* list, const char* name, int param_count) {
    if (!name) return;
    if (list->len >= list->cap) {
        int new_cap = list->cap ? list->cap * 2 : 8;
        list->items = realloc(list->items, new_cap * sizeof(ClosureVar));
        list->cap = new_cap;
    }
    list->items[list->len].name = strdup(name);
    list->items[list->len].param_count = param_count;
    list->len++;
}

static int closure_var_find(const ClosureVarList* list, const char* name) {
    if (!name) return -1;
    for (int i = 0; i < list->len; i++) {
        if (strcmp(list->items[i].name, name) == 0) {
            return list->items[i].param_count;
        }
    }
    return -1;
}

static void closure_var_list_free(ClosureVarList* list) {
    for (int i = 0; i < list->len; i++) free(list->items[i].name);
    free(list->items);
    list->items = NULL;
    list->len = list->cap = 0;
}

/* ========================================================================== */
/* Phase 1: Capture Analysis                                                  */
/* ========================================================================== */

/* Collect all referenced identifiers in a node */
static void collect_refs(CCNNode* node, StringSet* refs) {
    if (!node) return;
    
    switch (node->kind) {
        case CCN_EXPR_IDENT:
            string_set_add(refs, node->as.expr_ident.name);
            break;
            
        case CCN_EXPR_BINARY:
            collect_refs(node->as.expr_binary.lhs, refs);
            collect_refs(node->as.expr_binary.rhs, refs);
            break;
            
        case CCN_EXPR_UNARY:
            collect_refs(node->as.expr_unary.operand, refs);
            break;
            
        case CCN_EXPR_CALL:
            /* Include callee - if it's a local closure var, it needs capture */
            if (node->as.expr_call.callee && 
                node->as.expr_call.callee->kind == CCN_EXPR_IDENT) {
                string_set_add(refs, node->as.expr_call.callee->as.expr_ident.name);
            }
            for (int i = 0; i < node->as.expr_call.args.len; i++)
                collect_refs(node->as.expr_call.args.items[i], refs);
            break;
            
        case CCN_BLOCK:
            for (int i = 0; i < node->as.block.stmts.len; i++)
                collect_refs(node->as.block.stmts.items[i], refs);
            break;
            
        case CCN_STMT_EXPR:
            collect_refs(node->as.stmt_expr.expr, refs);
            break;
            
        case CCN_STMT_RETURN:
            collect_refs(node->as.stmt_return.value, refs);
            break;
            
        case CCN_STMT_IF:
            collect_refs(node->as.stmt_if.cond, refs);
            collect_refs(node->as.stmt_if.then_branch, refs);
            collect_refs(node->as.stmt_if.else_branch, refs);
            break;
            
        case CCN_STMT_FOR:
            collect_refs(node->as.stmt_for.init, refs);
            collect_refs(node->as.stmt_for.cond, refs);
            collect_refs(node->as.stmt_for.incr, refs);
            collect_refs(node->as.stmt_for.body, refs);
            break;
            
        case CCN_STMT_WHILE:
            collect_refs(node->as.stmt_while.cond, refs);
            collect_refs(node->as.stmt_while.body, refs);
            break;
            
        case CCN_VAR_DECL:
            collect_refs(node->as.var.init, refs);
            break;
            
        default:
            break;
    }
}

/* Collect all declared variables in a node (to exclude from captures) */
static void collect_decls(CCNNode* node, StringSet* decls) {
    if (!node) return;
    
    switch (node->kind) {
        case CCN_VAR_DECL:
            string_set_add(decls, node->as.var.name);
            collect_decls(node->as.var.init, decls);
            break;
            
        case CCN_BLOCK:
            for (int i = 0; i < node->as.block.stmts.len; i++)
                collect_decls(node->as.block.stmts.items[i], decls);
            break;
            
        case CCN_STMT_EXPR:
            collect_decls(node->as.stmt_expr.expr, decls);
            break;
            
        case CCN_STMT_IF:
            collect_decls(node->as.stmt_if.cond, decls);
            collect_decls(node->as.stmt_if.then_branch, decls);
            collect_decls(node->as.stmt_if.else_branch, decls);
            break;
            
        case CCN_STMT_FOR:
            collect_decls(node->as.stmt_for.init, decls);
            collect_decls(node->as.stmt_for.body, decls);
            break;
            
        case CCN_STMT_WHILE:
            collect_decls(node->as.stmt_while.body, decls);
            break;
            
        default:
            break;
    }
}

/* Analyze a closure and populate its captures list */
static void analyze_captures(CCNNode* closure, const ClosurePassCtx* ctx) {
    if (!closure || closure->kind != CCN_EXPR_CLOSURE) return;
    
    StringSet refs = {0}, decls = {0};
    
    /* Collect referenced and declared identifiers */
    collect_refs(closure->as.expr_closure.body, &refs);
    collect_decls(closure->as.expr_closure.body, &decls);
    
    /* Parameters are not captures */
    for (int i = 0; i < closure->as.expr_closure.params.len; i++) {
        CCNNode* p = closure->as.expr_closure.params.items[i];
        if (p && p->kind == CCN_PARAM && p->as.param.name)
            string_set_add(&decls, p->as.param.name);
    }
    
    /* Free variables = refs - decls - globals */
    for (int i = 0; i < refs.len; i++) {
        const char* name = refs.items[i];
        if (!string_set_contains(&decls, name) && 
            !string_set_contains(&ctx->globals, name)) {
            CCNNode* cap = ccn_node_new(CCN_EXPR_IDENT);
            if (cap) {
                cap->as.expr_ident.name = strdup(name);
                ccn_list_push(&closure->as.expr_closure.captures, cap);
            }
        }
    }
    
    string_set_free(&refs);
    string_set_free(&decls);
}

/* ========================================================================== */
/* Phase 2: Closure Lowering                                                  */
/* ========================================================================== */

static CCNNode* lower_node(CCNNode* node, ClosurePassCtx* ctx, CCClosureDefList* defs);

static void lower_list(CCNNodeList* list, ClosurePassCtx* ctx, CCClosureDefList* defs) {
    if (!list) return;
    for (int i = 0; i < list->len; i++)
        list->items[i] = lower_node(list->items[i], ctx, defs);
}

/* Convert closure literal to __cc_closure_make_N(...) call */
static CCNNode* lower_closure(CCNNode* closure, ClosurePassCtx* ctx, CCClosureDefList* defs) {
    if (!closure || closure->kind != CCN_EXPR_CLOSURE) return closure;
    
    int id = ctx->next_id++;
    
    /* Build closure definition */
    CCClosureDef def = {0};
    def.id = id;
    def.param_count = closure->as.expr_closure.params.len;
    def.captures = closure->as.expr_closure.captures;
    def.body = closure->as.expr_closure.body;
    def.params = closure->as.expr_closure.params;
    
    /* Look up types for captures */
    if (def.captures.len > 0) {
        def.capture_types = malloc(def.captures.len * sizeof(char*));
        for (int i = 0; i < def.captures.len; i++) {
            CCNNode* cap = def.captures.items[i];
            const char* type = NULL;
            if (cap && cap->kind == CCN_EXPR_IDENT && cap->as.expr_ident.name)
                type = string_map_get(&ctx->type_map, cap->as.expr_ident.name);
            def.capture_types[i] = strdup(type ? type : "intptr_t");
        }
    }
    
    def_list_push(defs, def);
    
    /* Create: __cc_closure_make_N(captured_values...) */
    char make_name[64];
    snprintf(make_name, sizeof(make_name), "__cc_closure_make_%d", id);
    
    CCNNode* call = ccn_node_new(CCN_EXPR_CALL);
    if (!call) return closure;
    
    call->span = closure->span;
    call->as.expr_call.callee = ccn_make_ident(make_name, closure->span);
    
    /* Add captured values as arguments */
    for (int i = 0; i < def.captures.len; i++) {
        CCNNode* cap = def.captures.items[i];
        if (cap && cap->kind == CCN_EXPR_IDENT) {
            CCNNode* arg = ccn_node_new(CCN_EXPR_IDENT);
            if (arg) {
                arg->as.expr_ident.name = cap->as.expr_ident.name;  /* Share string */
                arg->span = closure->span;
                ccn_list_push(&call->as.expr_call.args, arg);
            }
        }
    }
    
    /* Transfer ownership, clear original */
    closure->as.expr_closure.captures.items = NULL;
    closure->as.expr_closure.captures.len = 0;
    closure->as.expr_closure.body = NULL;
    closure->as.expr_closure.params.items = NULL;
    closure->as.expr_closure.params.len = 0;
    free(closure);
    
    return call;
}

/* Recursively lower nodes */
static CCNNode* lower_node(CCNNode* node, ClosurePassCtx* ctx, CCClosureDefList* defs) {
    if (!node) return NULL;
    
    /* Handle closure literals specially */
    if (node->kind == CCN_EXPR_CLOSURE) {
        node->as.expr_closure.body = lower_node(node->as.expr_closure.body, ctx, defs);
        analyze_captures(node, ctx);
        return lower_closure(node, ctx, defs);
    }
    
    /* Track variable types */
    if (node->kind == CCN_VAR_DECL && node->as.var.name) {
        if (node->as.var.type_node && node->as.var.type_node->kind == CCN_TYPE_NAME &&
            node->as.var.type_node->as.type_name.name) {
            string_map_set(&ctx->type_map, node->as.var.name,
                           node->as.var.type_node->as.type_name.name);
        }
    }
    
    /* Recurse into children */
    switch (node->kind) {
        case CCN_FILE:
            lower_list(&node->as.file.items, ctx, defs);
            break;
        case CCN_FUNC_DECL:
            node->as.func.body = lower_node(node->as.func.body, ctx, defs);
            break;
        case CCN_VAR_DECL:
            node->as.var.init = lower_node(node->as.var.init, ctx, defs);
            /* Update type if lowered to closure make */
            if (node->as.var.name && node->as.var.init &&
                node->as.var.init->kind == CCN_EXPR_CALL &&
                node->as.var.init->as.expr_call.callee &&
                node->as.var.init->as.expr_call.callee->kind == CCN_EXPR_IDENT) {
                const char* callee = node->as.var.init->as.expr_call.callee->as.expr_ident.name;
                if (callee && strncmp(callee, "__cc_closure_make_", 18) == 0) {
                    int id = atoi(callee + 18);
                    for (int i = 0; i < defs->len; i++) {
                        if (defs->items[i].id == id) {
                            int pc = defs->items[i].param_count;
                            const char* t = pc == 0 ? "CCClosure0" :
                                           pc == 1 ? "CCClosure1" : "CCClosure2";
                            string_map_set(&ctx->type_map, node->as.var.name, t);
                            break;
                        }
                    }
                }
            }
            break;
        case CCN_BLOCK:
            lower_list(&node->as.block.stmts, ctx, defs);
            break;
        case CCN_STMT_EXPR:
            node->as.stmt_expr.expr = lower_node(node->as.stmt_expr.expr, ctx, defs);
            break;
        case CCN_STMT_RETURN:
            node->as.stmt_return.value = lower_node(node->as.stmt_return.value, ctx, defs);
            break;
        case CCN_STMT_IF:
            node->as.stmt_if.cond = lower_node(node->as.stmt_if.cond, ctx, defs);
            node->as.stmt_if.then_branch = lower_node(node->as.stmt_if.then_branch, ctx, defs);
            node->as.stmt_if.else_branch = lower_node(node->as.stmt_if.else_branch, ctx, defs);
            break;
        case CCN_STMT_FOR:
            node->as.stmt_for.init = lower_node(node->as.stmt_for.init, ctx, defs);
            node->as.stmt_for.cond = lower_node(node->as.stmt_for.cond, ctx, defs);
            node->as.stmt_for.incr = lower_node(node->as.stmt_for.incr, ctx, defs);
            node->as.stmt_for.body = lower_node(node->as.stmt_for.body, ctx, defs);
            break;
        case CCN_STMT_WHILE:
            node->as.stmt_while.cond = lower_node(node->as.stmt_while.cond, ctx, defs);
            node->as.stmt_while.body = lower_node(node->as.stmt_while.body, ctx, defs);
            break;
        case CCN_STMT_NURSERY:
        case CCN_STMT_ARENA:
            node->as.stmt_scope.body = lower_node(node->as.stmt_scope.body, ctx, defs);
            break;
        case CCN_STMT_DEFER:
            node->as.stmt_defer.stmt = lower_node(node->as.stmt_defer.stmt, ctx, defs);
            break;
        case CCN_STMT_SPAWN:
            node->as.stmt_spawn.closure = lower_node(node->as.stmt_spawn.closure, ctx, defs);
            break;
        case CCN_EXPR_CALL:
            node->as.expr_call.callee = lower_node(node->as.expr_call.callee, ctx, defs);
            lower_list(&node->as.expr_call.args, ctx, defs);
            break;
        case CCN_EXPR_AWAIT:
            node->as.expr_await.expr = lower_node(node->as.expr_await.expr, ctx, defs);
            break;
        case CCN_EXPR_BINARY:
            node->as.expr_binary.lhs = lower_node(node->as.expr_binary.lhs, ctx, defs);
            node->as.expr_binary.rhs = lower_node(node->as.expr_binary.rhs, ctx, defs);
            break;
        case CCN_EXPR_UNARY:
            node->as.expr_unary.operand = lower_node(node->as.expr_unary.operand, ctx, defs);
            break;
        default:
            break;
    }
    
    return node;
}

/* ========================================================================== */
/* Phase 3: Closure Call Transformation                                       */
/* ========================================================================== */

/* Collect closure variables from VAR_DECLs */
static void collect_closure_vars(CCNNode* node, ClosureVarList* list, 
                                  const CCClosureDefList* defs) {
    if (!node) return;
    
    if (node->kind == CCN_VAR_DECL && node->as.var.init) {
        CCNNode* init = node->as.var.init;
        if (init->kind == CCN_EXPR_CALL && init->as.expr_call.callee &&
            init->as.expr_call.callee->kind == CCN_EXPR_IDENT) {
            const char* callee = init->as.expr_call.callee->as.expr_ident.name;
            if (callee && strncmp(callee, "__cc_closure_make_", 18) == 0) {
                int id = atoi(callee + 18);
                int param_count = 1;  /* Default */
                for (int i = 0; i < defs->len; i++) {
                    if (defs->items[i].id == id) {
                        param_count = defs->items[i].param_count;
                        break;
                    }
                }
                closure_var_add(list, node->as.var.name, param_count);
            }
        }
    }
    
    /* Recurse */
    switch (node->kind) {
        case CCN_FILE:
            for (int i = 0; i < node->as.file.items.len; i++)
                collect_closure_vars(node->as.file.items.items[i], list, defs);
            break;
        case CCN_FUNC_DECL:
            collect_closure_vars(node->as.func.body, list, defs);
            break;
        case CCN_BLOCK:
            for (int i = 0; i < node->as.block.stmts.len; i++)
                collect_closure_vars(node->as.block.stmts.items[i], list, defs);
            break;
        case CCN_STMT_IF:
            collect_closure_vars(node->as.stmt_if.then_branch, list, defs);
            collect_closure_vars(node->as.stmt_if.else_branch, list, defs);
            break;
        case CCN_STMT_FOR:
            collect_closure_vars(node->as.stmt_for.init, list, defs);
            collect_closure_vars(node->as.stmt_for.body, list, defs);
            break;
        case CCN_STMT_WHILE:
            collect_closure_vars(node->as.stmt_while.body, list, defs);
            break;
        case CCN_STMT_EXPR:
            collect_closure_vars(node->as.stmt_expr.expr, list, defs);
            break;
        default:
            break;
    }
}

/* Transform fn(args) to cc_closureN_call(fn, args) for closure variables */
static void transform_closure_calls(CCNNode* node, ClosureVarList* closure_vars) {
    if (!node) return;
    
    switch (node->kind) {
        case CCN_FILE:
            for (int i = 0; i < node->as.file.items.len; i++)
                transform_closure_calls(node->as.file.items.items[i], closure_vars);
            break;
        case CCN_FUNC_DECL:
            transform_closure_calls(node->as.func.body, closure_vars);
            break;
        case CCN_BLOCK:
            for (int i = 0; i < node->as.block.stmts.len; i++)
                transform_closure_calls(node->as.block.stmts.items[i], closure_vars);
            break;
        case CCN_STMT_EXPR:
            transform_closure_calls(node->as.stmt_expr.expr, closure_vars);
            break;
        case CCN_STMT_IF:
            transform_closure_calls(node->as.stmt_if.cond, closure_vars);
            transform_closure_calls(node->as.stmt_if.then_branch, closure_vars);
            transform_closure_calls(node->as.stmt_if.else_branch, closure_vars);
            break;
        case CCN_STMT_FOR:
            transform_closure_calls(node->as.stmt_for.init, closure_vars);
            transform_closure_calls(node->as.stmt_for.cond, closure_vars);
            transform_closure_calls(node->as.stmt_for.incr, closure_vars);
            transform_closure_calls(node->as.stmt_for.body, closure_vars);
            break;
        case CCN_STMT_WHILE:
            transform_closure_calls(node->as.stmt_while.cond, closure_vars);
            transform_closure_calls(node->as.stmt_while.body, closure_vars);
            break;
        case CCN_STMT_RETURN:
            transform_closure_calls(node->as.stmt_return.value, closure_vars);
            break;
        case CCN_VAR_DECL:
            transform_closure_calls(node->as.var.init, closure_vars);
            break;
        case CCN_EXPR_BINARY:
            transform_closure_calls(node->as.expr_binary.lhs, closure_vars);
            transform_closure_calls(node->as.expr_binary.rhs, closure_vars);
            break;
            
        case CCN_EXPR_CALL: {
            CCNNode* callee = node->as.expr_call.callee;
            if (callee && callee->kind == CCN_EXPR_IDENT) {
                int pc = closure_var_find(closure_vars, callee->as.expr_ident.name);
                if (pc >= 0) {
                    /* Transform: fn(args) -> cc_closureN_call(fn, args) */
                    char fn_name[32];
                    snprintf(fn_name, sizeof(fn_name), "cc_closure%d_call", pc);
                    
                    CCNNode* new_callee = ccn_node_new(CCN_EXPR_IDENT);
                    new_callee->as.expr_ident.name = strdup(fn_name);
                    
                    CCNNodeList new_args = {0};
                    ccn_list_push(&new_args, callee);  /* Old callee becomes first arg */
                    
                    /* Wrap args in (intptr_t) cast */
                    for (int i = 0; i < node->as.expr_call.args.len; i++) {
                        CCNNode* arg = node->as.expr_call.args.items[i];
                        CCNNode* cast = ccn_node_new(CCN_EXPR_CAST);
                        if (cast) {
                            CCNNode* type = ccn_node_new(CCN_TYPE_NAME);
                            if (type) type->as.type_name.name = strdup("intptr_t");
                            cast->as.expr_cast.type_node = type;
                            cast->as.expr_cast.expr = arg;
                            ccn_list_push(&new_args, cast);
                        } else {
                            ccn_list_push(&new_args, arg);
                        }
                    }
                    
                    node->as.expr_call.callee = new_callee;
                    free(node->as.expr_call.args.items);
                    node->as.expr_call.args = new_args;
                }
            }
            /* Recurse into args */
            for (int i = 0; i < node->as.expr_call.args.len; i++)
                transform_closure_calls(node->as.expr_call.args.items[i], closure_vars);
            break;
        }
            
        default:
            break;
    }
}

/* ========================================================================== */
/* Entry Point                                                                */
/* ========================================================================== */

int cc_pass_lower_closures(CCNFile* file) {
    if (!file || !file->root) return -1;
    
    ClosurePassCtx ctx;
    ctx_init(&ctx);
    
    /* Collect global variable names (excluded from captures) */
    if (file->root->kind == CCN_FILE) {
        for (int i = 0; i < file->root->as.file.items.len; i++) {
            CCNNode* item = file->root->as.file.items.items[i];
            if (item && item->kind == CCN_VAR_DECL && item->as.var.name)
                string_set_add(&ctx.globals, item->as.var.name);
        }
    }
    
    CCClosureDefList defs = {0};
    
    /* Phase 1 & 2: Analyze captures and lower closures */
    file->root = lower_node(file->root, &ctx, &defs);
    
    /* Store definitions for codegen */
    file->closure_count = defs.len;
    file->closure_defs = defs.items;
    
    /* Phase 3: Transform closure calls */
    if (defs.len > 0) {
        ClosureVarList closure_vars = {0};
        collect_closure_vars(file->root, &closure_vars, &defs);
        transform_closure_calls(file->root, &closure_vars);
        
        /* Also transform calls inside closure bodies */
        for (int i = 0; i < defs.len; i++) {
            CCClosureDef* def = &defs.items[i];
            if (def->body && def->captures.len > 0) {
                ClosureVarList captured_closures = {0};
                for (int j = 0; j < def->captures.len; j++) {
                    CCNNode* cap = def->captures.items[j];
                    if (cap && cap->kind == CCN_EXPR_IDENT) {
                        int pc = closure_var_find(&closure_vars, cap->as.expr_ident.name);
                        if (pc >= 0)
                            closure_var_add(&captured_closures, cap->as.expr_ident.name, pc);
                    }
                }
                if (captured_closures.len > 0)
                    transform_closure_calls(def->body, &captured_closures);
                closure_var_list_free(&captured_closures);
            }
        }
        
        closure_var_list_free(&closure_vars);
    }
    
    ctx_free(&ctx);
    return 0;
}
