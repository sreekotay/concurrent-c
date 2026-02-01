#include "cccn/passes/passes.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/*
 * UFCS Lowering Pass
 *
 * Transforms: receiver.method(args) → method(receiver, args)
 *
 * This converts CCN_EXPR_METHOD nodes into CCN_EXPR_CALL nodes,
 * prepending the receiver as the first argument.
 */

/* Forward declarations */
static CCNNode* lower_node(CCNNode* node);
static void lower_list(CCNNodeList* list);

/* Extract type name from TCC type string.
 * "struct Point *" -> "Point"
 * "struct Point"   -> "Point"
 * "Point *"        -> "Point"
 * "int *"          -> NULL (primitives not UFCS'd)
 */
static const char* extract_type_name(const char* type_str) {
    if (!type_str) return NULL;
    
    /* Skip "struct " prefix */
    const char* p = type_str;
    if (strncmp(p, "struct ", 7) == 0) p += 7;
    else if (strncmp(p, "union ", 6) == 0) p += 6;
    
    /* Skip leading whitespace */
    while (*p == ' ') p++;
    
    /* Find end of type name (stop at space or *) */
    const char* end = p;
    while (*end && *end != ' ' && *end != '*') end++;
    
    if (end == p) return NULL;
    
    /* Check for primitive types - don't UFCS these */
    size_t len = end - p;
    if (len <= 8 && (
        strncmp(p, "int", len) == 0 ||
        strncmp(p, "char", len) == 0 ||
        strncmp(p, "void", len) == 0 ||
        strncmp(p, "float", len) == 0 ||
        strncmp(p, "double", len) == 0 ||
        strncmp(p, "long", len) == 0 ||
        strncmp(p, "short", len) == 0)) {
        return NULL;
    }
    
    /* Return owned copy */
    char* name = malloc(len + 1);
    memcpy(name, p, len);
    name[len] = '\0';
    return name;
}

/* Check if type string indicates a pointer type */
static int is_pointer_type(const char* type_str) {
    return type_str && strchr(type_str, '*') != NULL;
}

/* Convert a method call to a regular function call */
static CCNNode* lower_method_to_call(CCNNode* method) {
    if (!method || method->kind != CCN_EXPR_METHOD) return method;
    
    /* Create new call node */
    CCNNode* call = ccn_node_new(CCN_EXPR_CALL);
    if (!call) return method;
    
    call->span = method->span;
    call->type = method->type;
    
    /* Build function name: TypeName_method or just method */
    const char* type_name = extract_type_name(method->as.expr_method.receiver_type);
    const char* method_name = method->as.expr_method.method;
    
    if (type_name && method_name) {
        /* Generate TypeName_method */
        size_t len = strlen(type_name) + 1 + strlen(method_name) + 1;
        char* full_name = malloc(len);
        snprintf(full_name, len, "%s_%s", type_name, method_name);
        call->as.expr_call.callee = ccn_make_ident(full_name, method->span);
        free(full_name);
        free((void*)type_name);
    } else {
        /* Just use method name */
        call->as.expr_call.callee = ccn_make_ident(method_name, method->span);
    }
    
    /* First argument is the receiver (lowered recursively) */
    CCNNode* receiver = lower_node(method->as.expr_method.receiver);
    
    /* If receiver is not already a pointer, take its address */
    if (receiver && !is_pointer_type(method->as.expr_method.receiver_type)) {
        CCNNode* addr = ccn_node_new(CCN_EXPR_UNARY);
        if (addr) {
            addr->as.expr_unary.op = CCN_OP_ADDR;
            addr->as.expr_unary.operand = receiver;
            addr->span = receiver->span;
            receiver = addr;
        }
    }
    
    /* Build new args list: [receiver, ...original_args] */
    CCNNodeList new_args = {0};
    
    /* Add receiver as first arg (if present) */
    if (receiver) {
        ccn_list_push(&new_args, receiver);
    }
    method->as.expr_method.receiver = NULL;  /* Transferred ownership */
    
    /* Add remaining args (lowered) */
    for (int i = 0; i < method->as.expr_method.args.len; i++) {
        CCNNode* arg = lower_node(method->as.expr_method.args.items[i]);
        if (arg) {
            ccn_list_push(&new_args, arg);
        }
    }
    
    /* Clear old args list without freeing items (ownership transferred) */
    free(method->as.expr_method.args.items);
    method->as.expr_method.args.items = NULL;
    method->as.expr_method.args.len = 0;
    method->as.expr_method.args.cap = 0;
    
    call->as.expr_call.args = new_args;
    
    /* Free the old method node shell (not its children, they're transferred) */
    free(method);
    
    return call;
}

/* Recursively lower a node, returning the (possibly new) node */
static CCNNode* lower_node(CCNNode* node) {
    if (!node) return NULL;
    
    /* Convert method calls to function calls */
    if (node->kind == CCN_EXPR_METHOD) {
        return lower_method_to_call(node);
    }
    
    switch (node->kind) {
        case CCN_FILE:
            lower_list(&node->as.file.items);
            break;
            
        case CCN_FUNC_DECL:
            node->as.func.return_type = lower_node(node->as.func.return_type);
            lower_list(&node->as.func.params);
            node->as.func.body = lower_node(node->as.func.body);
            break;
            
        case CCN_VAR_DECL:
            node->as.var.type_node = lower_node(node->as.var.type_node);
            node->as.var.init = lower_node(node->as.var.init);
            break;
            
        case CCN_PARAM:
            node->as.param.type_node = lower_node(node->as.param.type_node);
            break;
            
        case CCN_BLOCK:
            lower_list(&node->as.block.stmts);
            break;
            
        case CCN_STMT_EXPR:
            node->as.stmt_expr.expr = lower_node(node->as.stmt_expr.expr);
            break;
            
        case CCN_STMT_RETURN:
            node->as.stmt_return.value = lower_node(node->as.stmt_return.value);
            break;
            
        case CCN_STMT_IF:
            node->as.stmt_if.cond = lower_node(node->as.stmt_if.cond);
            node->as.stmt_if.then_branch = lower_node(node->as.stmt_if.then_branch);
            node->as.stmt_if.else_branch = lower_node(node->as.stmt_if.else_branch);
            break;
            
        case CCN_STMT_WHILE:
        case CCN_STMT_FOR_AWAIT:
            node->as.stmt_while.cond = lower_node(node->as.stmt_while.cond);
            node->as.stmt_while.body = lower_node(node->as.stmt_while.body);
            break;
            
        case CCN_STMT_FOR:
            node->as.stmt_for.init = lower_node(node->as.stmt_for.init);
            node->as.stmt_for.cond = lower_node(node->as.stmt_for.cond);
            node->as.stmt_for.incr = lower_node(node->as.stmt_for.incr);
            node->as.stmt_for.body = lower_node(node->as.stmt_for.body);
            break;
            
        case CCN_STMT_NURSERY:
        case CCN_STMT_ARENA:
            node->as.stmt_scope.size = lower_node(node->as.stmt_scope.size);
            node->as.stmt_scope.body = lower_node(node->as.stmt_scope.body);
            lower_list(&node->as.stmt_scope.closing);
            break;
            
        case CCN_STMT_DEFER:
            node->as.stmt_defer.stmt = lower_node(node->as.stmt_defer.stmt);
            break;
            
        case CCN_STMT_SPAWN:
            node->as.stmt_spawn.closure = lower_node(node->as.stmt_spawn.closure);
            break;
            
        case CCN_STMT_MATCH:
            lower_list(&node->as.stmt_match.arms);
            break;
            
        case CCN_MATCH_ARM:
            node->as.match_arm.pattern = lower_node(node->as.match_arm.pattern);
            node->as.match_arm.body = lower_node(node->as.match_arm.body);
            break;
            
        case CCN_EXPR_CALL:
            node->as.expr_call.callee = lower_node(node->as.expr_call.callee);
            lower_list(&node->as.expr_call.args);
            break;
            
        case CCN_EXPR_FIELD:
            node->as.expr_field.object = lower_node(node->as.expr_field.object);
            break;
            
        case CCN_EXPR_INDEX:
            node->as.expr_index.array = lower_node(node->as.expr_index.array);
            node->as.expr_index.index = lower_node(node->as.expr_index.index);
            break;
            
        case CCN_EXPR_UNARY:
            node->as.expr_unary.operand = lower_node(node->as.expr_unary.operand);
            break;
            
        case CCN_EXPR_BINARY:
            node->as.expr_binary.lhs = lower_node(node->as.expr_binary.lhs);
            node->as.expr_binary.rhs = lower_node(node->as.expr_binary.rhs);
            break;
            
        case CCN_EXPR_TERNARY:
            node->as.expr_ternary.cond = lower_node(node->as.expr_ternary.cond);
            node->as.expr_ternary.then_expr = lower_node(node->as.expr_ternary.then_expr);
            node->as.expr_ternary.else_expr = lower_node(node->as.expr_ternary.else_expr);
            break;
            
        case CCN_EXPR_CAST:
            node->as.expr_cast.type_node = lower_node(node->as.expr_cast.type_node);
            node->as.expr_cast.expr = lower_node(node->as.expr_cast.expr);
            break;
            
        case CCN_EXPR_CLOSURE:
            lower_list(&node->as.expr_closure.params);
            node->as.expr_closure.body = lower_node(node->as.expr_closure.body);
            lower_list(&node->as.expr_closure.captures);
            break;
            
        case CCN_EXPR_AWAIT:
            node->as.expr_await.expr = lower_node(node->as.expr_await.expr);
            break;
            
        case CCN_EXPR_OK:
        case CCN_EXPR_ERR:
        case CCN_EXPR_SOME:
            node->as.expr_result.value = lower_node(node->as.expr_result.value);
            break;
            
        case CCN_EXPR_TRY:
            node->as.expr_try.expr = lower_node(node->as.expr_try.expr);
            break;
            
        case CCN_TYPE_PTR:
        case CCN_TYPE_OPTIONAL:
            node->as.type_ptr.base = lower_node(node->as.type_ptr.base);
            break;
            
        case CCN_TYPE_ARRAY:
        case CCN_TYPE_SLICE:
            node->as.type_array.elem = lower_node(node->as.type_array.elem);
            node->as.type_array.size = lower_node(node->as.type_array.size);
            break;
            
        case CCN_TYPE_CHAN_TX:
        case CCN_TYPE_CHAN_RX:
            node->as.type_chan.elem = lower_node(node->as.type_chan.elem);
            node->as.type_chan.capacity = lower_node(node->as.type_chan.capacity);
            break;
            
        case CCN_TYPE_RESULT:
            node->as.type_result.ok_type = lower_node(node->as.type_result.ok_type);
            node->as.type_result.err_type = lower_node(node->as.type_result.err_type);
            break;
            
        default:
            /* Leaf nodes: nothing to do */
            break;
    }
    
    return node;
}

/* Lower all nodes in a list */
static void lower_list(CCNNodeList* list) {
    if (!list) return;
    
    for (int i = 0; i < list->len; i++) {
        CCNNode* node = list->items[i];
        if (!node) continue;
        
        /* lower_node handles method conversion now */
        list->items[i] = lower_node(node);
    }
}

/* Entry point: lower UFCS in the entire file */
int cc_pass_lower_ufcs(CCNFile* file) {
    if (!file || !file->root) return -1;
    
    file->root = lower_node(file->root);
    return 0;
}
