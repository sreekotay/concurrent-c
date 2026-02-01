#include "cccn/passes/passes.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

/*
 * Closure Lowering Pass
 *
 * Transforms closure literals `() => { body }` into:
 * 1. A struct for captured variables (env)
 * 2. An entry function that executes the body
 * 3. A make function that creates the closure
 * 4. Replace the literal with a call to the make function
 *
 * Output pattern:
 *   typedef struct __cc_closure_env_N { ... captures ... } __cc_closure_env_N;
 *   static void __cc_closure_env_N_drop(void* p) { if (p) free(p); }
 *   static void* __cc_closure_entry_N(void* __p) { ... body ... }
 *   static CCClosure0 __cc_closure_make_N(void) {
 *       return cc_closure0_make(__cc_closure_entry_N, NULL, NULL);
 *   }
 *
 * The closure literal is replaced with: __cc_closure_make_N()
 */

/* Counter for unique closure IDs */
static int g_closure_id = 0;

/* ========================================================================== */
/* Capture Analysis - Find free variables in closure body                     */
/* ========================================================================== */

/* Simple string set for tracking names */
typedef struct {
    char** items;
    int len;
    int cap;
} StringSet;

/* Simple string map for tracking variable types */
typedef struct {
    char** keys;
    char** values;
    int len;
    int cap;
} StringMap;

static void string_map_set(StringMap* map, const char* key, const char* value) {
    if (!key) return;
    /* Check if already present */
    for (int i = 0; i < map->len; i++) {
        if (strcmp(map->keys[i], key) == 0) {
            /* Update existing */
            free(map->values[i]);
            map->values[i] = value ? strdup(value) : NULL;
            return;
        }
    }
    /* Add new entry */
    if (map->len >= map->cap) {
        int new_cap = map->cap ? map->cap * 2 : 16;
        map->keys = realloc(map->keys, new_cap * sizeof(char*));
        map->values = realloc(map->values, new_cap * sizeof(char*));
        map->cap = new_cap;
    }
    map->keys[map->len] = strdup(key);
    map->values[map->len] = value ? strdup(value) : NULL;
    map->len++;
}

static const char* string_map_get(const StringMap* map, const char* key) {
    if (!key) return NULL;
    for (int i = 0; i < map->len; i++) {
        if (strcmp(map->keys[i], key) == 0) return map->values[i];
    }
    return NULL;
}

static void string_map_free(StringMap* map) {
    for (int i = 0; i < map->len; i++) {
        free(map->keys[i]);
        free(map->values[i]);
    }
    free(map->keys);
    free(map->values);
    map->keys = map->values = NULL;
    map->len = map->cap = 0;
}

/* Global type map - populated during traversal */
static StringMap g_type_map = {0};

static void string_set_add(StringSet* set, const char* name) {
    if (!name) return;
    /* Check if already present */
    for (int i = 0; i < set->len; i++) {
        if (strcmp(set->items[i], name) == 0) return;
    }
    /* Add new entry */
    if (set->len >= set->cap) {
        int new_cap = set->cap ? set->cap * 2 : 8;
        set->items = realloc(set->items, new_cap * sizeof(char*));
        set->cap = new_cap;
    }
    set->items[set->len++] = strdup(name);
}

static int string_set_contains(const StringSet* set, const char* name) {
    if (!name) return 0;
    for (int i = 0; i < set->len; i++) {
        if (strcmp(set->items[i], name) == 0) return 1;
    }
    return 0;
}

static void string_set_free(StringSet* set) {
    for (int i = 0; i < set->len; i++) {
        free(set->items[i]);
    }
    free(set->items);
    set->items = NULL;
    set->len = set->cap = 0;
}

/* Collect all referenced identifiers in a node */
static void collect_refs(CCNNode* node, StringSet* refs);
static void collect_refs_list(CCNNodeList* list, StringSet* refs) {
    if (!list) return;
    for (int i = 0; i < list->len; i++) {
        collect_refs(list->items[i], refs);
    }
}

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
            /* Include callee in refs - if it's a local variable (closure), it needs capture.
               If it's a global function, it won't be in local decls so won't be captured. */
            if (node->as.expr_call.callee && node->as.expr_call.callee->kind == CCN_EXPR_IDENT) {
                string_set_add(refs, node->as.expr_call.callee->as.expr_ident.name);
            }
            collect_refs_list(&node->as.expr_call.args, refs);
            break;
            
        case CCN_BLOCK:
            collect_refs_list(&node->as.block.stmts, refs);
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
static void collect_decls(CCNNode* node, StringSet* decls);
static void collect_decls_list(CCNNodeList* list, StringSet* decls) {
    if (!list) return;
    for (int i = 0; i < list->len; i++) {
        collect_decls(list->items[i], decls);
    }
}

static void collect_decls(CCNNode* node, StringSet* decls) {
    if (!node) return;
    
    switch (node->kind) {
        case CCN_VAR_DECL:
            string_set_add(decls, node->as.var.name);
            collect_decls(node->as.var.init, decls);
            break;
            
        case CCN_BLOCK:
            collect_decls_list(&node->as.block.stmts, decls);
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

/* Global variables set (populated at start of pass) */
static StringSet g_globals = {0};

/* Collect global variable names from file-level items */
static void collect_globals(CCNNode* file_node) {
    if (!file_node || file_node->kind != CCN_FILE) return;
    
    for (int i = 0; i < file_node->as.file.items.len; i++) {
        CCNNode* item = file_node->as.file.items.items[i];
        if (item && item->kind == CCN_VAR_DECL && item->as.var.name) {
            string_set_add(&g_globals, item->as.var.name);
        }
    }
}

/* Analyze a closure and populate its captures list */
static void analyze_captures(CCNNode* closure) {
    if (!closure || closure->kind != CCN_EXPR_CLOSURE) return;
    
    StringSet refs = {0};
    StringSet decls = {0};
    
    /* Collect all referenced identifiers in the body */
    collect_refs(closure->as.expr_closure.body, &refs);
    
    /* Collect declared variables within the body */
    collect_decls(closure->as.expr_closure.body, &decls);
    
    /* Add closure parameters to decls (they're not captures) */
    for (int i = 0; i < closure->as.expr_closure.params.len; i++) {
        CCNNode* p = closure->as.expr_closure.params.items[i];
        if (p && p->kind == CCN_PARAM && p->as.param.name) {
            string_set_add(&decls, p->as.param.name);
        }
    }
    
    /* Free variables = refs - decls - globals */
    for (int i = 0; i < refs.len; i++) {
        const char* name = refs.items[i];
        if (!string_set_contains(&decls, name) && 
            !string_set_contains(&g_globals, name)) {
            /* This is a capture - add IDENT node to captures list */
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

/* List of closure definitions (CCClosureDef is defined in ast.h) */
typedef struct {
    CCClosureDef* items;
    int len;
    int cap;
} CCClosureDefList;

static void closure_def_list_push(CCClosureDefList* list, CCClosureDef def) {
    if (list->len >= list->cap) {
        int new_cap = list->cap ? list->cap * 2 : 8;
        list->items = realloc(list->items, new_cap * sizeof(CCClosureDef));
        list->cap = new_cap;
    }
    list->items[list->len++] = def;
}

/* Forward declarations */
static CCNNode* lower_node(CCNNode* node, CCClosureDefList* defs);
static void lower_list(CCNNodeList* list, CCClosureDefList* defs);

/* Convert a closure literal to a call to the generated make function */
static CCNNode* lower_closure(CCNNode* closure, CCClosureDefList* defs) {
    if (!closure || closure->kind != CCN_EXPR_CLOSURE) return closure;
    
    int id = g_closure_id++;
    
    /* Record the closure definition for later emission */
    CCClosureDef def = {0};
    def.id = id;
    def.param_count = closure->as.expr_closure.params.len;
    def.captures = closure->as.expr_closure.captures;
    def.body = closure->as.expr_closure.body;
    def.params = closure->as.expr_closure.params;
    
    /* Look up types for each capture */
    if (def.captures.len > 0) {
        def.capture_types = malloc(def.captures.len * sizeof(char*));
        for (int i = 0; i < def.captures.len; i++) {
            CCNNode* cap = def.captures.items[i];
            if (cap && cap->kind == CCN_EXPR_IDENT && cap->as.expr_ident.name) {
                const char* type = string_map_get(&g_type_map, cap->as.expr_ident.name);
                def.capture_types[i] = type ? strdup(type) : strdup("intptr_t");
            } else {
                def.capture_types[i] = strdup("intptr_t");
            }
        }
    }
    
    closure_def_list_push(defs, def);
    
    /* If this closure is assigned to a variable, update g_type_map with correct closure type.
       We do this by noting what __cc_closure_make_N generates - it will be assigned to a var. */
    /* Note: The actual type mapping happens when collect_closure_vars runs, but we need
       the info available for nested closures. We'll track closure IDs to param counts. */
    
    /* Create call to __cc_closure_make_N(captured_values...) */
    char make_name[64];
    snprintf(make_name, sizeof(make_name), "__cc_closure_make_%d", id);
    
    CCNNode* call = ccn_node_new(CCN_EXPR_CALL);
    if (!call) return closure;
    
    call->span = closure->span;
    call->as.expr_call.callee = ccn_make_ident(make_name, closure->span);
    
    /* Add captured values as arguments to make function */
    for (int i = 0; i < def.captures.len; i++) {
        CCNNode* cap = def.captures.items[i];
        if (cap && cap->kind == CCN_EXPR_IDENT) {
            /* Clone the capture IDENT as an argument */
            CCNNode* arg = ccn_node_new(CCN_EXPR_IDENT);
            if (arg) {
                arg->as.expr_ident.name = cap->as.expr_ident.name;  /* Share string */
                arg->span = closure->span;
                ccn_list_push(&call->as.expr_call.args, arg);
            }
        }
    }
    
    /* Clear captures/body/params from original (ownership transferred to def) */
    closure->as.expr_closure.captures.items = NULL;
    closure->as.expr_closure.captures.len = 0;
    closure->as.expr_closure.body = NULL;
    closure->as.expr_closure.params.items = NULL;
    closure->as.expr_closure.params.len = 0;
    
    /* Free old closure shell */
    free(closure);
    
    return call;
}

/* Recursively lower a node */
static CCNNode* lower_node(CCNNode* node, CCClosureDefList* defs) {
    if (!node) return NULL;
    
    /* Convert closure literals */
    if (node->kind == CCN_EXPR_CLOSURE) {
        /* First lower the body recursively (closures can be nested) */
        node->as.expr_closure.body = lower_node(node->as.expr_closure.body, defs);
        
        /* Analyze captures - find free variables */
        analyze_captures(node);
        
        return lower_closure(node, defs);
    }
    
    switch (node->kind) {
        case CCN_FILE:
            lower_list(&node->as.file.items, defs);
            break;
            
        case CCN_FUNC_DECL:
            node->as.func.body = lower_node(node->as.func.body, defs);
            break;
            
        case CCN_VAR_DECL:
            /* Track variable type for capture type lookup */
            if (node->as.var.name && node->as.var.type_node &&
                node->as.var.type_node->kind == CCN_TYPE_NAME &&
                node->as.var.type_node->as.type_name.name) {
                string_map_set(&g_type_map, node->as.var.name,
                               node->as.var.type_node->as.type_name.name);
            }
            node->as.var.init = lower_node(node->as.var.init, defs);
            /* If init was lowered to __cc_closure_make_N, update type to correct CCClosureN */
            if (node->as.var.name && node->as.var.init &&
                node->as.var.init->kind == CCN_EXPR_CALL &&
                node->as.var.init->as.expr_call.callee &&
                node->as.var.init->as.expr_call.callee->kind == CCN_EXPR_IDENT) {
                const char* callee = node->as.var.init->as.expr_call.callee->as.expr_ident.name;
                if (callee && strncmp(callee, "__cc_closure_make_", 18) == 0) {
                    int id = atoi(callee + 18);
                    /* Find param count for this closure */
                    for (int i = 0; i < defs->len; i++) {
                        if (defs->items[i].id == id) {
                            int pc = defs->items[i].param_count;
                            const char* ctype = pc == 0 ? "CCClosure0" :
                                               pc == 1 ? "CCClosure1" : "CCClosure2";
                            string_map_set(&g_type_map, node->as.var.name, ctype);
                            break;
                        }
                    }
                }
            }
            break;
            
        case CCN_BLOCK:
            lower_list(&node->as.block.stmts, defs);
            break;
            
        case CCN_STMT_EXPR:
            node->as.stmt_expr.expr = lower_node(node->as.stmt_expr.expr, defs);
            break;
            
        case CCN_STMT_RETURN:
            node->as.stmt_return.value = lower_node(node->as.stmt_return.value, defs);
            break;
            
        case CCN_STMT_IF:
            node->as.stmt_if.cond = lower_node(node->as.stmt_if.cond, defs);
            node->as.stmt_if.then_branch = lower_node(node->as.stmt_if.then_branch, defs);
            node->as.stmt_if.else_branch = lower_node(node->as.stmt_if.else_branch, defs);
            break;
            
        case CCN_STMT_FOR:
            node->as.stmt_for.init = lower_node(node->as.stmt_for.init, defs);
            node->as.stmt_for.cond = lower_node(node->as.stmt_for.cond, defs);
            node->as.stmt_for.incr = lower_node(node->as.stmt_for.incr, defs);
            node->as.stmt_for.body = lower_node(node->as.stmt_for.body, defs);
            break;
            
        case CCN_STMT_WHILE:
            node->as.stmt_while.cond = lower_node(node->as.stmt_while.cond, defs);
            node->as.stmt_while.body = lower_node(node->as.stmt_while.body, defs);
            break;
            
        case CCN_STMT_NURSERY:
        case CCN_STMT_ARENA:
            node->as.stmt_scope.body = lower_node(node->as.stmt_scope.body, defs);
            break;
            
        case CCN_STMT_DEFER:
            node->as.stmt_defer.stmt = lower_node(node->as.stmt_defer.stmt, defs);
            break;
            
        case CCN_STMT_SPAWN:
            node->as.stmt_spawn.closure = lower_node(node->as.stmt_spawn.closure, defs);
            break;
            
        case CCN_EXPR_CALL:
            node->as.expr_call.callee = lower_node(node->as.expr_call.callee, defs);
            lower_list(&node->as.expr_call.args, defs);
            break;
            
        case CCN_EXPR_AWAIT:
            node->as.expr_await.expr = lower_node(node->as.expr_await.expr, defs);
            break;
            
        case CCN_EXPR_BINARY:
            node->as.expr_binary.lhs = lower_node(node->as.expr_binary.lhs, defs);
            node->as.expr_binary.rhs = lower_node(node->as.expr_binary.rhs, defs);
            break;
            
        case CCN_EXPR_UNARY:
            node->as.expr_unary.operand = lower_node(node->as.expr_unary.operand, defs);
            break;
            
        default:
            break;
    }
    
    return node;
}

/* Lower all nodes in a list */
static void lower_list(CCNNodeList* list, CCClosureDefList* defs) {
    if (!list) return;
    
    for (int i = 0; i < list->len; i++) {
        list->items[i] = lower_node(list->items[i], defs);
    }
}

/* Generate the closure support code (env struct, entry fn, make fn) */
static CCNNode* generate_closure_defs(CCClosureDefList* defs) {
    if (!defs || defs->len == 0) return NULL;
    
    /* For now, we'll store the generated code as a special node.
     * In a full implementation, this would generate actual AST nodes
     * for the struct, drop function, entry function, and make function.
     * 
     * TODO: Generate proper AST nodes for:
     * - typedef struct __cc_closure_env_N { ... } __cc_closure_env_N;
     * - static void __cc_closure_env_N_drop(void* p) { ... }
     * - static void* __cc_closure_entry_N(void* __p) { ... body ... }
     * - static CCClosure0 __cc_closure_make_N(void) { ... }
     */
    
    return NULL;  /* Placeholder - codegen will handle this specially */
}

/* ========================================================================== */
/* Closure Call Transformation - Transform fn(x) to cc_closure_call(fn, x)   */
/* ========================================================================== */

/* Track which variables are closures */
typedef struct {
    char* name;
    int param_count;  /* 0, 1, or 2 */
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
    return -1;  /* Not a closure */
}

static void closure_var_list_free(ClosureVarList* list) {
    for (int i = 0; i < list->len; i++) {
        free(list->items[i].name);
    }
    free(list->items);
    list->items = NULL;
    list->len = list->cap = 0;
}

/* Forward declaration */
static void transform_closure_calls(CCNNode* node, ClosureVarList* closure_vars);

/* Collect closure variables from VAR_DECLs */
static void collect_closure_vars(CCNNode* node, ClosureVarList* list, const CCClosureDefList* defs) {
    if (!node) return;
    
    if (node->kind == CCN_VAR_DECL && node->as.var.init) {
        CCNNode* init = node->as.var.init;
        if (init->kind == CCN_EXPR_CALL && init->as.expr_call.callee &&
            init->as.expr_call.callee->kind == CCN_EXPR_IDENT) {
            const char* callee = init->as.expr_call.callee->as.expr_ident.name;
            if (callee && strncmp(callee, "__cc_closure_make_", 18) == 0) {
                /* Extract closure ID from name */
                int id = atoi(callee + 18);
                /* Look up param count from defs */
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
            /* Check if callee is a closure variable */
            CCNNode* callee = node->as.expr_call.callee;
            if (callee && callee->kind == CCN_EXPR_IDENT) {
                int param_count = closure_var_find(closure_vars, callee->as.expr_ident.name);
                if (param_count >= 0) {
                    /* Transform fn(args) to cc_closureN_call(fn, args) */
                    /* Move current callee to be first arg */
                    CCNNode* old_callee = callee;
                    
                    /* Create new callee: cc_closureN_call */
                    char fn_name[32];
                    snprintf(fn_name, sizeof(fn_name), "cc_closure%d_call", param_count);
                    CCNNode* new_callee = ccn_node_new(CCN_EXPR_IDENT);
                    new_callee->as.expr_ident.name = strdup(fn_name);
                    
                    /* Prepend old callee to args, wrap other args in (intptr_t) cast */
                    CCNNodeList new_args = {0};
                    ccn_list_push(&new_args, old_callee);
                    for (int i = 0; i < node->as.expr_call.args.len; i++) {
                        CCNNode* arg = node->as.expr_call.args.items[i];
                        /* Wrap arg in (intptr_t) cast for closure call interface */
                        CCNNode* cast = ccn_node_new(CCN_EXPR_CAST);
                        if (cast) {
                            CCNNode* type = ccn_node_new(CCN_TYPE_NAME);
                            if (type) {
                                type->as.type_name.name = strdup("intptr_t");
                                cast->as.expr_cast.type_node = type;
                            }
                            cast->as.expr_cast.expr = arg;
                            ccn_list_push(&new_args, cast);
                        } else {
                            ccn_list_push(&new_args, arg);
                        }
                    }
                    
                    /* Replace callee and args */
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

/* Entry point */
int cc_pass_lower_closures(CCNFile* file) {
    if (!file || !file->root) return -1;
    
    /* Reset closure counter for this file */
    g_closure_id = 0;
    
    /* Collect global variable names (to exclude from captures) */
    string_set_free(&g_globals);  /* Clear any previous */
    collect_globals(file->root);
    
    CCClosureDefList defs = {0};
    
    /* Lower all closure literals */
    file->root = lower_node(file->root, &defs);
    
    /* Store collected closure definitions in the file for codegen */
    file->closure_count = defs.len;
    file->closure_defs = defs.items;  /* Transfer ownership */
    
    /* Pass 2: Collect closure variables and transform calls */
    if (defs.len > 0) {
        ClosureVarList closure_vars = {0};
        collect_closure_vars(file->root, &closure_vars, &defs);
        transform_closure_calls(file->root, &closure_vars);
        
        /* Also transform calls inside closure bodies.
           If a closure captures another closure variable, transform calls to it. */
        for (int i = 0; i < defs.len; i++) {
            CCClosureDef* def = &defs.items[i];
            if (def->body && def->captures.len > 0) {
                /* Build list of captured closure variables */
                ClosureVarList captured_closures = {0};
                for (int j = 0; j < def->captures.len; j++) {
                    CCNNode* cap = def->captures.items[j];
                    if (cap && cap->kind == CCN_EXPR_IDENT) {
                        /* Check if this capture is a closure variable */
                        int pc = closure_var_find(&closure_vars, cap->as.expr_ident.name);
                        if (pc >= 0) {
                            closure_var_add(&captured_closures, cap->as.expr_ident.name, pc);
                        }
                    }
                }
                /* Transform calls in the closure body */
                if (captured_closures.len > 0) {
                    transform_closure_calls(def->body, &captured_closures);
                }
                closure_var_list_free(&captured_closures);
            }
        }
        
        closure_var_list_free(&closure_vars);
    }
    
    /* Clean up globals set and type map */
    string_set_free(&g_globals);
    string_map_free(&g_type_map);
    
    return 0;
}
