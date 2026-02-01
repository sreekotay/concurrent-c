#include "cccn/ast/ast.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ========================================================================== */
/* Memory Allocation                                                          */
/* ========================================================================== */

CCNNode* ccn_node_new(CCNKind kind) {
    CCNNode* node = (CCNNode*)calloc(1, sizeof(CCNNode));
    if (node) {
        node->kind = kind;
    }
    return node;
}

/* ========================================================================== */
/* List Operations                                                            */
/* ========================================================================== */

void ccn_list_push(CCNNodeList* list, CCNNode* node) {
    if (!list) return;
    if (list->len >= list->cap) {
        int new_cap = list->cap == 0 ? 4 : list->cap * 2;
        CCNNode** new_items = (CCNNode**)realloc(list->items, new_cap * sizeof(CCNNode*));
        if (!new_items) return;
        list->items = new_items;
        list->cap = new_cap;
    }
    list->items[list->len++] = node;
}

void ccn_list_free(CCNNodeList* list) {
    if (!list) return;
    for (int i = 0; i < list->len; i++) {
        ccn_node_free(list->items[i]);
    }
    free(list->items);
    list->items = NULL;
    list->len = 0;
    list->cap = 0;
}

/* ========================================================================== */
/* Node Constructors                                                          */
/* ========================================================================== */

CCNNode* ccn_make_ident(const char* name, CCNSpan span) {
    CCNNode* node = ccn_node_new(CCN_EXPR_IDENT);
    if (node) {
        node->span = span;
        node->as.expr_ident.name = name ? strdup(name) : NULL;  /* owned copy */
    }
    return node;
}

CCNNode* ccn_make_int_lit(int64_t value, CCNSpan span) {
    CCNNode* node = ccn_node_new(CCN_EXPR_LITERAL_INT);
    if (node) {
        node->span = span;
        node->as.expr_int.value = value;
    }
    return node;
}

CCNNode* ccn_make_string_lit(const char* value, size_t len, CCNSpan span) {
    CCNNode* node = ccn_node_new(CCN_EXPR_LITERAL_STRING);
    if (node) {
        node->span = span;
        node->as.expr_string.value = value ? strndup(value, len) : NULL;  /* owned */
        node->as.expr_string.len = len;
    }
    return node;
}

CCNNode* ccn_make_call(CCNNode* callee, CCNNodeList args, CCNSpan span) {
    CCNNode* node = ccn_node_new(CCN_EXPR_CALL);
    if (node) {
        node->span = span;
        node->as.expr_call.callee = callee;
        node->as.expr_call.args = args;
    }
    return node;
}

CCNNode* ccn_make_method(CCNNode* receiver, const char* method, CCNNodeList args, CCNSpan span) {
    CCNNode* node = ccn_node_new(CCN_EXPR_METHOD);
    if (node) {
        node->span = span;
        node->as.expr_method.receiver = receiver;
        node->as.expr_method.method = method ? strdup(method) : NULL;  /* owned */
        node->as.expr_method.args = args;
    }
    return node;
}

CCNNode* ccn_make_block(CCNNodeList stmts, CCNSpan span) {
    CCNNode* node = ccn_node_new(CCN_BLOCK);
    if (node) {
        node->span = span;
        node->as.block.stmts = stmts;
    }
    return node;
}

CCNNode* ccn_make_return(CCNNode* value, CCNSpan span) {
    CCNNode* node = ccn_node_new(CCN_STMT_RETURN);
    if (node) {
        node->span = span;
        node->as.stmt_return.value = value;
    }
    return node;
}

/* ========================================================================== */
/* Deep Clone                                                                 */
/* ========================================================================== */

static CCNNodeList ccn_list_clone(CCNNodeList* list) {
    CCNNodeList result = {0};
    if (!list) return result;
    for (int i = 0; i < list->len; i++) {
        ccn_list_push(&result, ccn_node_clone(list->items[i]));
    }
    return result;
}

/* Helper to clone string (returns NULL if input is NULL) */
static char* clone_str(const char* s) {
    return s ? strdup(s) : NULL;
}

CCNNode* ccn_node_clone(CCNNode* node) {
    if (!node) return NULL;
    
    CCNNode* clone = ccn_node_new(node->kind);
    if (!clone) return NULL;
    
    clone->span = node->span;
    clone->type = node->type;  /* shallow copy of type info */
    
    switch (node->kind) {
        case CCN_FILE:
            clone->as.file.path = clone_str(node->as.file.path);
            clone->as.file.items = ccn_list_clone(&node->as.file.items);
            break;
            
        case CCN_FUNC_DECL:
            clone->as.func.name = clone_str(node->as.func.name);
            clone->as.func.return_type = ccn_node_clone(node->as.func.return_type);
            clone->as.func.params = ccn_list_clone(&node->as.func.params);
            clone->as.func.body = ccn_node_clone(node->as.func.body);
            clone->as.func.is_async = node->as.func.is_async;
            clone->as.func.is_static = node->as.func.is_static;
            clone->as.func.is_noblock = node->as.func.is_noblock;
            break;
            
        case CCN_VAR_DECL:
            clone->as.var.name = clone_str(node->as.var.name);
            clone->as.var.type_node = ccn_node_clone(node->as.var.type_node);
            clone->as.var.init = ccn_node_clone(node->as.var.init);
            clone->as.var.is_static = node->as.var.is_static;
            clone->as.var.is_const = node->as.var.is_const;
            break;
            
        case CCN_PARAM:
            clone->as.param.name = clone_str(node->as.param.name);
            clone->as.param.type_node = ccn_node_clone(node->as.param.type_node);
            break;
            
        case CCN_BLOCK:
            clone->as.block.stmts = ccn_list_clone(&node->as.block.stmts);
            break;
            
        case CCN_STMT_EXPR:
            clone->as.stmt_expr.expr = ccn_node_clone(node->as.stmt_expr.expr);
            break;
            
        case CCN_STMT_RETURN:
            clone->as.stmt_return.value = ccn_node_clone(node->as.stmt_return.value);
            break;
            
        case CCN_STMT_IF:
            clone->as.stmt_if.cond = ccn_node_clone(node->as.stmt_if.cond);
            clone->as.stmt_if.then_branch = ccn_node_clone(node->as.stmt_if.then_branch);
            clone->as.stmt_if.else_branch = ccn_node_clone(node->as.stmt_if.else_branch);
            break;
            
        case CCN_STMT_FOR:
            clone->as.stmt_for.init = ccn_node_clone(node->as.stmt_for.init);
            clone->as.stmt_for.cond = ccn_node_clone(node->as.stmt_for.cond);
            clone->as.stmt_for.incr = ccn_node_clone(node->as.stmt_for.incr);
            clone->as.stmt_for.body = ccn_node_clone(node->as.stmt_for.body);
            break;
            
        case CCN_STMT_WHILE:
            clone->as.stmt_while.cond = ccn_node_clone(node->as.stmt_while.cond);
            clone->as.stmt_while.body = ccn_node_clone(node->as.stmt_while.body);
            break;
            
        case CCN_STMT_NURSERY:
        case CCN_STMT_ARENA:
            clone->as.stmt_scope.name = clone_str(node->as.stmt_scope.name);
            clone->as.stmt_scope.size = ccn_node_clone(node->as.stmt_scope.size);
            clone->as.stmt_scope.body = ccn_node_clone(node->as.stmt_scope.body);
            clone->as.stmt_scope.closing = ccn_list_clone(&node->as.stmt_scope.closing);
            break;
            
        case CCN_STMT_SPAWN:
            clone->as.stmt_spawn.closure = ccn_node_clone(node->as.stmt_spawn.closure);
            break;
            
        case CCN_STMT_DEFER:
            clone->as.stmt_defer.stmt = ccn_node_clone(node->as.stmt_defer.stmt);
            break;
            
        case CCN_EXPR_IDENT:
            clone->as.expr_ident.name = clone_str(node->as.expr_ident.name);
            break;
            
        case CCN_EXPR_LITERAL_INT:
            clone->as.expr_int.value = node->as.expr_int.value;
            break;
            
        case CCN_EXPR_LITERAL_STRING:
            clone->as.expr_string.value = clone_str(node->as.expr_string.value);
            clone->as.expr_string.len = node->as.expr_string.len;
            break;
            
        case CCN_EXPR_CALL:
            clone->as.expr_call.callee = ccn_node_clone(node->as.expr_call.callee);
            clone->as.expr_call.args = ccn_list_clone(&node->as.expr_call.args);
            break;
            
        case CCN_EXPR_METHOD:
            clone->as.expr_method.receiver = ccn_node_clone(node->as.expr_method.receiver);
            clone->as.expr_method.method = clone_str(node->as.expr_method.method);
            clone->as.expr_method.receiver_type = clone_str(node->as.expr_method.receiver_type);
            clone->as.expr_method.args = ccn_list_clone(&node->as.expr_method.args);
            break;
            
        case CCN_EXPR_BINARY:
            clone->as.expr_binary.op = node->as.expr_binary.op;
            clone->as.expr_binary.lhs = ccn_node_clone(node->as.expr_binary.lhs);
            clone->as.expr_binary.rhs = ccn_node_clone(node->as.expr_binary.rhs);
            break;
            
        case CCN_EXPR_UNARY:
            clone->as.expr_unary.op = node->as.expr_unary.op;
            clone->as.expr_unary.operand = ccn_node_clone(node->as.expr_unary.operand);
            clone->as.expr_unary.is_postfix = node->as.expr_unary.is_postfix;
            break;
            
        case CCN_EXPR_CLOSURE:
            clone->as.expr_closure.params = ccn_list_clone(&node->as.expr_closure.params);
            clone->as.expr_closure.body = ccn_node_clone(node->as.expr_closure.body);
            clone->as.expr_closure.captures = ccn_list_clone(&node->as.expr_closure.captures);
            clone->as.expr_closure.is_unsafe = node->as.expr_closure.is_unsafe;
            break;
            
        case CCN_EXPR_AWAIT:
            clone->as.expr_await.expr = ccn_node_clone(node->as.expr_await.expr);
            break;
            
        case CCN_EXPR_TRY:
            clone->as.expr_try.expr = ccn_node_clone(node->as.expr_try.expr);
            break;
            
        case CCN_EXPR_FIELD:
            clone->as.expr_field.object = ccn_node_clone(node->as.expr_field.object);
            clone->as.expr_field.field = clone_str(node->as.expr_field.field);
            clone->as.expr_field.is_arrow = node->as.expr_field.is_arrow;
            break;
            
        case CCN_EXPR_INDEX:
            clone->as.expr_index.array = ccn_node_clone(node->as.expr_index.array);
            clone->as.expr_index.index = ccn_node_clone(node->as.expr_index.index);
            break;
            
        case CCN_EXPR_SIZEOF:
            clone->as.expr_sizeof.type_str = clone_str(node->as.expr_sizeof.type_str);
            clone->as.expr_sizeof.expr = ccn_node_clone(node->as.expr_sizeof.expr);
            break;
            
        case CCN_EXPR_COMPOUND:
            clone->as.expr_compound.values = ccn_list_clone(&node->as.expr_compound.values);
            break;
            
        case CCN_STRUCT_DECL:
            clone->as.struct_decl.name = clone_str(node->as.struct_decl.name);
            clone->as.struct_decl.fields = ccn_list_clone(&node->as.struct_decl.fields);
            clone->as.struct_decl.is_union = node->as.struct_decl.is_union;
            break;
            
        case CCN_STRUCT_FIELD:
            clone->as.struct_field.name = clone_str(node->as.struct_field.name);
            clone->as.struct_field.type_str = clone_str(node->as.struct_field.type_str);
            break;
            
        case CCN_ENUM_DECL:
            clone->as.enum_decl.name = clone_str(node->as.enum_decl.name);
            clone->as.enum_decl.values = ccn_list_clone(&node->as.enum_decl.values);
            break;
            
        case CCN_ENUM_VALUE:
            clone->as.enum_value.name = clone_str(node->as.enum_value.name);
            clone->as.enum_value.value = node->as.enum_value.value;
            break;
            
        case CCN_TYPEDEF:
            clone->as.typedef_decl.name = clone_str(node->as.typedef_decl.name);
            clone->as.typedef_decl.type_str = clone_str(node->as.typedef_decl.type_str);
            break;
            
        case CCN_INCLUDE:
            clone->as.include.path = clone_str(node->as.include.path);
            clone->as.include.is_system = node->as.include.is_system;
            break;
            
        case CCN_TYPE_NAME:
            clone->as.type_name.name = clone_str(node->as.type_name.name);
            break;
            
        case CCN_TYPE_PTR:
        case CCN_TYPE_OPTIONAL:
            clone->as.type_ptr.base = ccn_node_clone(node->as.type_ptr.base);
            break;
            
        default:
            /* For unhandled kinds, the shallow copy is already done */
            break;
    }
    
    return clone;
}

/* ========================================================================== */
/* Free                                                                       */
/* ========================================================================== */

void ccn_node_free(CCNNode* node) {
    if (!node) return;
    
    switch (node->kind) {
        case CCN_FILE:
            free((void*)node->as.file.path);
            ccn_list_free(&node->as.file.items);
            break;
            
        case CCN_FUNC_DECL:
            free((void*)node->as.func.name);
            ccn_node_free(node->as.func.return_type);
            ccn_list_free(&node->as.func.params);
            ccn_node_free(node->as.func.body);
            break;
            
        case CCN_VAR_DECL:
            free((void*)node->as.var.name);
            ccn_node_free(node->as.var.type_node);
            ccn_node_free(node->as.var.init);
            break;
            
        case CCN_PARAM:
            free((void*)node->as.param.name);
            ccn_node_free(node->as.param.type_node);
            break;
            
        case CCN_BLOCK:
            ccn_list_free(&node->as.block.stmts);
            break;
            
        case CCN_STMT_EXPR:
            ccn_node_free(node->as.stmt_expr.expr);
            break;
            
        case CCN_STMT_RETURN:
            ccn_node_free(node->as.stmt_return.value);
            break;
            
        case CCN_STMT_IF:
            ccn_node_free(node->as.stmt_if.cond);
            ccn_node_free(node->as.stmt_if.then_branch);
            ccn_node_free(node->as.stmt_if.else_branch);
            break;
            
        case CCN_STMT_FOR:
            ccn_node_free(node->as.stmt_for.init);
            ccn_node_free(node->as.stmt_for.cond);
            ccn_node_free(node->as.stmt_for.incr);
            ccn_node_free(node->as.stmt_for.body);
            break;
            
        case CCN_STMT_WHILE:
            ccn_node_free(node->as.stmt_while.cond);
            ccn_node_free(node->as.stmt_while.body);
            break;
            
        case CCN_EXPR_CALL:
            ccn_node_free(node->as.expr_call.callee);
            ccn_list_free(&node->as.expr_call.args);
            break;
            
        case CCN_EXPR_METHOD:
            ccn_node_free(node->as.expr_method.receiver);
            free((void*)node->as.expr_method.method);
            free((void*)node->as.expr_method.receiver_type);
            ccn_list_free(&node->as.expr_method.args);
            break;
            
        case CCN_EXPR_BINARY:
            ccn_node_free(node->as.expr_binary.lhs);
            ccn_node_free(node->as.expr_binary.rhs);
            break;
            
        case CCN_EXPR_UNARY:
            ccn_node_free(node->as.expr_unary.operand);
            break;
            
        case CCN_EXPR_CLOSURE:
            ccn_list_free(&node->as.expr_closure.params);
            ccn_node_free(node->as.expr_closure.body);
            ccn_list_free(&node->as.expr_closure.captures);
            break;
            
        case CCN_EXPR_AWAIT:
            ccn_node_free(node->as.expr_await.expr);
            break;
            
        case CCN_EXPR_TRY:
            ccn_node_free(node->as.expr_try.expr);
            break;
            
        case CCN_TYPE_PTR:
        case CCN_TYPE_OPTIONAL:
            ccn_node_free(node->as.type_ptr.base);
            break;
            
        case CCN_STRUCT_DECL:
            free((void*)node->as.struct_decl.name);
            ccn_list_free(&node->as.struct_decl.fields);
            break;
            
        case CCN_STRUCT_FIELD:
            free((void*)node->as.struct_field.name);
            free((void*)node->as.struct_field.type_str);
            break;
            
        case CCN_ENUM_DECL:
            free((void*)node->as.enum_decl.name);
            ccn_list_free(&node->as.enum_decl.values);
            break;
            
        case CCN_ENUM_VALUE:
            free((void*)node->as.enum_value.name);
            break;
            
        case CCN_TYPEDEF:
            free((void*)node->as.typedef_decl.name);
            free((void*)node->as.typedef_decl.type_str);
            break;
            
        case CCN_INCLUDE:
            free((void*)node->as.include.path);
            break;
            
        case CCN_EXPR_FIELD:
            ccn_node_free(node->as.expr_field.object);
            free((void*)node->as.expr_field.field);
            break;
            
        case CCN_EXPR_INDEX:
            ccn_node_free(node->as.expr_index.array);
            ccn_node_free(node->as.expr_index.index);
            break;
            
        case CCN_EXPR_SIZEOF:
            free((void*)node->as.expr_sizeof.type_str);
            ccn_node_free(node->as.expr_sizeof.expr);
            break;
            
        case CCN_EXPR_COMPOUND:
            ccn_list_free(&node->as.expr_compound.values);
            break;
            
        case CCN_EXPR_IDENT:
            free((void*)node->as.expr_ident.name);
            break;
            
        case CCN_EXPR_LITERAL_STRING:
            free((void*)node->as.expr_string.value);
            break;
            
        case CCN_STMT_NURSERY:
        case CCN_STMT_ARENA:
            free((void*)node->as.stmt_scope.name);
            ccn_node_free(node->as.stmt_scope.size);
            ccn_node_free(node->as.stmt_scope.body);
            ccn_list_free(&node->as.stmt_scope.closing);
            break;
            
        case CCN_STMT_SPAWN:
            ccn_node_free(node->as.stmt_spawn.closure);
            break;
            
        case CCN_STMT_DEFER:
            ccn_node_free(node->as.stmt_defer.stmt);
            break;
            
        case CCN_TYPE_NAME:
            free((void*)node->as.type_name.name);
            break;
            
        default:
            /* Leaf nodes without dynamic allocations */
            break;
    }
    
    free(node);
}

/* ========================================================================== */
/* Debug Printing                                                             */
/* ========================================================================== */

static const char* ccn_kind_name(CCNKind kind) {
    switch (kind) {
        case CCN_ERROR: return "ERROR";
        case CCN_FILE: return "FILE";
        case CCN_FUNC_DECL: return "FUNC_DECL";
        case CCN_VAR_DECL: return "VAR_DECL";
        case CCN_PARAM: return "PARAM";
        case CCN_BLOCK: return "BLOCK";
        case CCN_STMT_EXPR: return "STMT_EXPR";
        case CCN_STMT_RETURN: return "STMT_RETURN";
        case CCN_STMT_IF: return "STMT_IF";
        case CCN_STMT_WHILE: return "STMT_WHILE";
        case CCN_STMT_FOR: return "STMT_FOR";
        case CCN_STMT_NURSERY: return "STMT_NURSERY";
        case CCN_STMT_ARENA: return "STMT_ARENA";
        case CCN_STMT_DEFER: return "STMT_DEFER";
        case CCN_STMT_SPAWN: return "STMT_SPAWN";
        case CCN_STMT_MATCH: return "STMT_MATCH";
        case CCN_EXPR_IDENT: return "EXPR_IDENT";
        case CCN_EXPR_LITERAL_INT: return "EXPR_INT";
        case CCN_EXPR_LITERAL_STRING: return "EXPR_STRING";
        case CCN_EXPR_CALL: return "EXPR_CALL";
        case CCN_EXPR_METHOD: return "EXPR_METHOD";
        case CCN_EXPR_BINARY: return "EXPR_BINARY";
        case CCN_EXPR_UNARY: return "EXPR_UNARY";
        case CCN_EXPR_CLOSURE: return "EXPR_CLOSURE";
        case CCN_EXPR_AWAIT: return "EXPR_AWAIT";
        case CCN_EXPR_TRY: return "EXPR_TRY";
        case CCN_TYPE_NAME: return "TYPE_NAME";
        case CCN_TYPE_PTR: return "TYPE_PTR";
        default: return "?";
    }
}

void ccn_node_dump(CCNNode* node, int indent) {
    if (!node) {
        printf("%*s(null)\n", indent * 2, "");
        return;
    }
    
    printf("%*s%s", indent * 2, "", ccn_kind_name(node->kind));
    
    switch (node->kind) {
        case CCN_FILE:
            printf(" path=%s items=%d\n", 
                   node->as.file.path ? node->as.file.path : "<null>",
                   node->as.file.items.len);
            for (int i = 0; i < node->as.file.items.len; i++) {
                ccn_node_dump(node->as.file.items.items[i], indent + 1);
            }
            break;
            
        case CCN_FUNC_DECL:
            printf(" name=%s async=%d\n", node->as.func.name, node->as.func.is_async);
            for (int i = 0; i < node->as.func.params.len; i++) {
                ccn_node_dump(node->as.func.params.items[i], indent + 1);
            }
            ccn_node_dump(node->as.func.body, indent + 1);
            break;
            
        case CCN_VAR_DECL:
            printf(" name=%s\n", node->as.var.name);
            if (node->as.var.init) {
                ccn_node_dump(node->as.var.init, indent + 1);
            }
            break;
            
        case CCN_PARAM:
            printf(" name=%s\n", node->as.param.name);
            break;
            
        case CCN_BLOCK:
            printf(" stmts=%d\n", node->as.block.stmts.len);
            for (int i = 0; i < node->as.block.stmts.len; i++) {
                ccn_node_dump(node->as.block.stmts.items[i], indent + 1);
            }
            break;
            
        case CCN_STMT_NURSERY:
        case CCN_STMT_ARENA:
            printf(" name=%s\n", node->as.stmt_scope.name ? node->as.stmt_scope.name : "<anon>");
            if (node->as.stmt_scope.body) {
                ccn_node_dump(node->as.stmt_scope.body, indent + 1);
            }
            break;
            
        case CCN_STMT_SPAWN:
            printf("\n");
            if (node->as.stmt_spawn.closure) {
                ccn_node_dump(node->as.stmt_spawn.closure, indent + 1);
            }
            break;
            
        case CCN_STMT_DEFER:
            printf("\n");
            if (node->as.stmt_defer.stmt) {
                ccn_node_dump(node->as.stmt_defer.stmt, indent + 1);
            }
            break;
            
        case CCN_STMT_EXPR:
            printf("\n");
            if (node->as.stmt_expr.expr) {
                ccn_node_dump(node->as.stmt_expr.expr, indent + 1);
            }
            break;
            
        case CCN_STMT_RETURN:
            printf("\n");
            if (node->as.stmt_return.value) {
                ccn_node_dump(node->as.stmt_return.value, indent + 1);
            }
            break;
            
        case CCN_STMT_IF:
            printf("\n");
            printf("%*scond:\n", (indent + 1) * 2, "");
            ccn_node_dump(node->as.stmt_if.cond, indent + 2);
            printf("%*sthen:\n", (indent + 1) * 2, "");
            ccn_node_dump(node->as.stmt_if.then_branch, indent + 2);
            if (node->as.stmt_if.else_branch) {
                printf("%*selse:\n", (indent + 1) * 2, "");
                ccn_node_dump(node->as.stmt_if.else_branch, indent + 2);
            }
            break;
            
        case CCN_STMT_FOR:
            printf("\n");
            printf("%*sinit:\n", (indent + 1) * 2, "");
            ccn_node_dump(node->as.stmt_for.init, indent + 2);
            printf("%*scond:\n", (indent + 1) * 2, "");
            ccn_node_dump(node->as.stmt_for.cond, indent + 2);
            printf("%*sincr:\n", (indent + 1) * 2, "");
            ccn_node_dump(node->as.stmt_for.incr, indent + 2);
            printf("%*sbody:\n", (indent + 1) * 2, "");
            ccn_node_dump(node->as.stmt_for.body, indent + 2);
            break;
            
        case CCN_STMT_WHILE:
            printf("\n");
            printf("%*scond:\n", (indent + 1) * 2, "");
            ccn_node_dump(node->as.stmt_while.cond, indent + 2);
            printf("%*sbody:\n", (indent + 1) * 2, "");
            ccn_node_dump(node->as.stmt_while.body, indent + 2);
            break;
            
        case CCN_EXPR_CLOSURE:
            printf(" params=%d\n", node->as.expr_closure.params.len);
            for (int i = 0; i < node->as.expr_closure.params.len; i++) {
                ccn_node_dump(node->as.expr_closure.params.items[i], indent + 1);
            }
            if (node->as.expr_closure.body) {
                ccn_node_dump(node->as.expr_closure.body, indent + 1);
            }
            break;
            
        case CCN_EXPR_IDENT:
            printf(" name=%s\n", node->as.expr_ident.name);
            break;
            
        case CCN_EXPR_LITERAL_INT:
            printf(" value=%lld\n", (long long)node->as.expr_int.value);
            break;
            
        case CCN_EXPR_LITERAL_STRING:
            printf(" value=\"%s\"\n", node->as.expr_string.value);
            break;
            
        case CCN_EXPR_CALL:
            printf("\n");
            ccn_node_dump(node->as.expr_call.callee, indent + 1);
            for (int i = 0; i < node->as.expr_call.args.len; i++) {
                ccn_node_dump(node->as.expr_call.args.items[i], indent + 1);
            }
            break;
            
        case CCN_EXPR_METHOD:
            printf(" method=%s\n", node->as.expr_method.method);
            ccn_node_dump(node->as.expr_method.receiver, indent + 1);
            for (int i = 0; i < node->as.expr_method.args.len; i++) {
                ccn_node_dump(node->as.expr_method.args.items[i], indent + 1);
            }
            break;
            
        case CCN_EXPR_AWAIT:
            printf("\n");
            if (node->as.expr_await.expr) {
                ccn_node_dump(node->as.expr_await.expr, indent + 1);
            }
            break;
            
        case CCN_EXPR_TRY:
            printf("\n");
            if (node->as.expr_try.expr) {
                ccn_node_dump(node->as.expr_try.expr, indent + 1);
            }
            break;
            
        case CCN_EXPR_BINARY: {
            static const char* op_names[] = {
                "+", "-", "*", "/", "%",
                "&", "|", "^", "<<", ">>",
                "&&", "||",
                "==", "!=", "<", "<=", ">", ">=",
                "=", "+=", "-=", "*=", "/=", "%=",
                ","
            };
            int op = node->as.expr_binary.op;
            const char* op_str = (op >= 0 && op < (int)(sizeof(op_names)/sizeof(op_names[0]))) 
                                 ? op_names[op] : "?";
            printf(" op=%s\n", op_str);
            ccn_node_dump(node->as.expr_binary.lhs, indent + 1);
            ccn_node_dump(node->as.expr_binary.rhs, indent + 1);
            break;
        }
            
        case CCN_TYPE_NAME:
            printf(" name=%s\n", node->as.type_name.name);
            break;
            
        default:
            printf("\n");
            break;
    }
}
