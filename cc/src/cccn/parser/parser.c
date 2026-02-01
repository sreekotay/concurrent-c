#include "cccn/parser/parser.h"
#include "parser/tcc_bridge.h"
#include "preprocess/preprocess.h"
#include "comptime/symbols.h"
#include "visitor/pass_common.h"
#include "visitor/visitor_fileutil.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/*
 * Convert TCC stub-AST (flat array) into CCNNode tree.
 *
 * Strategy:
 * 1. TCC gives us a flat array of nodes with parent indices
 * 2. We do a single pass to build CCNNode objects
 * 3. Then link children to parents based on parent indices
 *
 * TCC node kinds (from pass_common.h):
 *   CC_AST_NODE_FUNC = 17  - function declaration
 *   CC_AST_NODE_PARAM = 16 - parameter
 *   CC_AST_NODE_BLOCK = 2  - compound statement
 *   CC_AST_NODE_STMT = 3   - statement (spawn, defer, nursery marker)
 *   CC_AST_NODE_ARENA = 4  - @arena block
 *   CC_AST_NODE_CALL = 5   - function/method call
 *   CC_AST_NODE_AWAIT = 6  - await expression
 *   CC_AST_NODE_CLOSURE = 9 - closure literal
 *   CC_AST_NODE_IDENT = 10 - identifier
 *   CC_AST_NODE_RETURN = 15 - return statement
 */

/* Convert operator string to CCNBinaryOp enum */
static CCNBinaryOp str_to_binary_op(const char* s) {
    if (!s) return CCN_OP_ASSIGN;
    if (strcmp(s, "=") == 0) return CCN_OP_ASSIGN;
    if (strcmp(s, "+") == 0) return CCN_OP_ADD;
    if (strcmp(s, "-") == 0) return CCN_OP_SUB;
    if (strcmp(s, "*") == 0) return CCN_OP_MUL;
    if (strcmp(s, "/") == 0) return CCN_OP_DIV;
    if (strcmp(s, "%") == 0) return CCN_OP_MOD;
    if (strcmp(s, "==") == 0) return CCN_OP_EQ;
    if (strcmp(s, "!=") == 0) return CCN_OP_NE;
    if (strcmp(s, "<") == 0) return CCN_OP_LT;
    if (strcmp(s, "<=") == 0) return CCN_OP_LE;
    if (strcmp(s, ">") == 0) return CCN_OP_GT;
    if (strcmp(s, ">=") == 0) return CCN_OP_GE;
    if (strcmp(s, "&&") == 0) return CCN_OP_LAND;
    if (strcmp(s, "||") == 0) return CCN_OP_LOR;
    if (strcmp(s, "&") == 0) return CCN_OP_BAND;
    if (strcmp(s, "|") == 0) return CCN_OP_BOR;
    if (strcmp(s, "^") == 0) return CCN_OP_BXOR;
    if (strcmp(s, "<<") == 0) return CCN_OP_SHL;
    if (strcmp(s, ">>") == 0) return CCN_OP_SHR;
    if (strcmp(s, "+=") == 0) return CCN_OP_ADD_ASSIGN;
    if (strcmp(s, "-=") == 0) return CCN_OP_SUB_ASSIGN;
    if (strcmp(s, "*=") == 0) return CCN_OP_MUL_ASSIGN;
    if (strcmp(s, "/=") == 0) return CCN_OP_DIV_ASSIGN;
    if (strcmp(s, "%=") == 0) return CCN_OP_MOD_ASSIGN;
    return CCN_OP_ASSIGN;  /* Default */
}

/* Build span from TCC node */
static CCNSpan span_from_tcc(const CCNodeView* n, const char* file) {
    CCNSpan span = {0};
    span.start.file = file;
    span.start.line = n->line_start;
    span.start.col = n->col_start;
    span.end.file = file;
    span.end.line = n->line_end > 0 ? n->line_end : n->line_start;
    span.end.col = n->col_end;
    return span;
}

/* Helper to copy string (returns NULL if input is NULL) */
static char* copy_str(const char* s) {
    return s ? strdup(s) : NULL;
}

/* Convert a single TCC node to CCNNode */
static CCNNode* convert_tcc_node(const CCNodeView* n, const char* file) {
    CCNNode* node = NULL;
    CCNSpan span = span_from_tcc(n, file);
    
    switch (n->kind) {
        case CC_AST_NODE_FUNC: {
            node = ccn_node_new(CCN_FUNC_DECL);
            if (node) {
                node->span = span;
                node->as.func.name = copy_str(n->aux_s1);  /* function name - owned copy */
                node->as.func.is_async = (n->aux1 & 1) != 0;
                node->as.func.is_static = (n->aux1 & 2) != 0;
                node->as.func.is_noblock = (n->aux1 & 4) != 0;
                /* aux_s2 contains return type string */
                if (n->aux_s2) {
                    CCNNode* ret_type = ccn_node_new(CCN_TYPE_NAME);
                    if (ret_type) {
                        ret_type->as.type_name.name = copy_str(n->aux_s2);
                        node->as.func.return_type = ret_type;
                    }
                }
            }
            break;
        }
        
        case CC_AST_NODE_PARAM: {
            node = ccn_node_new(CCN_PARAM);
            if (node) {
                node->span = span;
                node->as.param.name = copy_str(n->aux_s1);
                /* aux_s2 contains parameter type string */
                if (n->aux_s2) {
                    CCNNode* param_type = ccn_node_new(CCN_TYPE_NAME);
                    if (param_type) {
                        param_type->as.type_name.name = copy_str(n->aux_s2);
                        node->as.param.type_node = param_type;
                    }
                }
            }
            break;
        }
        
        case CC_AST_NODE_BLOCK: {
            node = ccn_node_new(CCN_BLOCK);
            if (node) {
                node->span = span;
            }
            break;
        }
        
        case CC_AST_NODE_STMT: {
            /* aux_s1 contains statement type: "spawn", "defer", "nursery", etc.
             * Note: "return" is just a marker - actual return is CC_AST_NODE_RETURN. */
            if (n->aux_s1) {
                if (strcmp(n->aux_s1, "spawn") == 0) {
                    node = ccn_node_new(CCN_STMT_SPAWN);
                } else if (strcmp(n->aux_s1, "defer") == 0) {
                    node = ccn_node_new(CCN_STMT_DEFER);
                } else if (strcmp(n->aux_s1, "nursery") == 0) {
                    node = ccn_node_new(CCN_STMT_NURSERY);
                }
                /* "return" is a marker only - actual return is CC_AST_NODE_RETURN,
                 * so we don't create a node here, just let it pass through as STMT_EXPR */
            }
            if (!node) {
                node = ccn_node_new(CCN_STMT_EXPR);
            }
            if (node) {
                node->span = span;
            }
            break;
        }
        
        case CC_AST_NODE_ARENA: {
            node = ccn_node_new(CCN_STMT_ARENA);
            if (node) {
                node->span = span;
                node->as.stmt_scope.name = copy_str(n->aux_s1);  /* arena name - owned */
                /* aux_s2 may contain size expression */
            }
            break;
        }
        
        case CC_AST_NODE_CALL: {
            /* aux2 & 2 means UFCS (method call), otherwise regular function call.
             * aux_s1 contains the function/method name for both cases. */
            int is_ufcs = (n->aux2 & 2) != 0;
            if (is_ufcs) {
                node = ccn_node_new(CCN_EXPR_METHOD);
                if (node) {
                    node->span = span;
                    node->as.expr_method.method = copy_str(n->aux_s1);  /* method - owned */
                    /* aux_s2 contains receiver type if known */
                }
            } else {
                node = ccn_node_new(CCN_EXPR_CALL);
                if (node) {
                    node->span = span;
                    /* Create callee as IDENT node */
                    if (n->aux_s1) {
                        CCNNode* callee = ccn_node_new(CCN_EXPR_IDENT);
                        if (callee) {
                            callee->as.expr_ident.name = copy_str(n->aux_s1);
                        }
                        node->as.expr_call.callee = callee;
                    }
                }
            }
            break;
        }
        
        case CC_AST_NODE_AWAIT: {
            node = ccn_node_new(CCN_EXPR_AWAIT);
            if (node) {
                node->span = span;
            }
            break;
        }
        
        case CC_AST_NODE_TRY: {
            node = ccn_node_new(CCN_EXPR_TRY);
            if (node) {
                node->span = span;
            }
            break;
        }
        
        case CC_AST_NODE_CLOSURE: {
            node = ccn_node_new(CCN_EXPR_CLOSURE);
            if (node) {
                node->span = span;
                node->as.expr_closure.is_unsafe = (n->aux1 & 1) != 0;
                /* aux1 also contains param count in upper bits */
            }
            break;
        }
        
        case CC_AST_NODE_IDENT: {
            node = ccn_node_new(CCN_EXPR_IDENT);
            if (node) {
                node->span = span;
                node->as.expr_ident.name = copy_str(n->aux_s1);  /* owned */
            }
            break;
        }
        
        case CC_AST_NODE_RETURN: {
            node = ccn_node_new(CCN_STMT_RETURN);
            if (node) {
                node->span = span;
            }
            break;
        }
        
        case CC_AST_NODE_DECL:
            /* DECL is a wrapper around one or more DECL_ITEMs.
             * We don't create a node for it - the items will link directly
             * to the enclosing block. */
            break;
            
        case CC_AST_NODE_DECL_ITEM: {
            node = ccn_node_new(CCN_VAR_DECL);
            if (node) {
                node->span = span;
                node->as.var.name = copy_str(n->aux_s1);  /* owned */
                /* aux_s2 contains the type string */
                if (n->aux_s2) {
                    CCNNode* type_node = ccn_node_new(CCN_TYPE_NAME);
                    if (type_node) {
                        type_node->as.type_name.name = copy_str(n->aux_s2);
                    }
                    node->as.var.type_node = type_node;
                }
            }
            break;
        }
        
        case CC_AST_NODE_ASSIGN: {
            /* Assignment: aux_s1 contains LHS identifier name, aux_s2 contains operator.
             * We create a binary expression with the appropriate operator and
             * pre-set the LHS to the identifier from aux_s1. */
            node = ccn_node_new(CCN_EXPR_BINARY);
            if (node) {
                node->span = span;
                /* Determine operator from aux_s2 */
                if (n->aux_s2) {
                    if (strcmp(n->aux_s2, "+=") == 0) node->as.expr_binary.op = CCN_OP_ADD_ASSIGN;
                    else if (strcmp(n->aux_s2, "-=") == 0) node->as.expr_binary.op = CCN_OP_SUB_ASSIGN;
                    else if (strcmp(n->aux_s2, "*=") == 0) node->as.expr_binary.op = CCN_OP_MUL_ASSIGN;
                    else if (strcmp(n->aux_s2, "/=") == 0) node->as.expr_binary.op = CCN_OP_DIV_ASSIGN;
                    else if (strcmp(n->aux_s2, "%=") == 0) node->as.expr_binary.op = CCN_OP_MOD_ASSIGN;
                    else node->as.expr_binary.op = CCN_OP_ASSIGN;
                } else {
                    node->as.expr_binary.op = CCN_OP_ASSIGN;
                }
                /* Create LHS identifier from aux_s1 */
                if (n->aux_s1) {
                    node->as.expr_binary.lhs = ccn_make_ident(n->aux_s1, span);
                }
                /* RHS will be linked as a child in the tree-building pass */
            }
            break;
        }
        
        case CC_AST_NODE_CONST: {
            /* Constant literal: aux_s1 contains the value string */
            /* Determine type from the value */
            if (n->aux_s1 && n->aux_s1[0] == '"') {
                node = ccn_node_new(CCN_EXPR_LITERAL_STRING);
                if (node) {
                    node->span = span;
                    /* Strip quotes from string */
                    size_t len = strlen(n->aux_s1);
                    if (len >= 2) {
                        char* s = malloc(len - 1);
                        memcpy(s, n->aux_s1 + 1, len - 2);
                        s[len - 2] = '\0';
                        node->as.expr_string.value = s;
                        node->as.expr_string.len = len - 2;
                    }
                }
            } else {
                /* Assume integer for now */
                node = ccn_node_new(CCN_EXPR_LITERAL_INT);
                if (node) {
                    node->span = span;
                    node->as.expr_int.value = n->aux_s1 ? atoll(n->aux_s1) : 0;
                }
            }
            break;
        }
        
        case CC_AST_NODE_MEMBER: {
            /* Member access: aux_s1 contains member name, aux2 = 1 if arrow (->) */
            node = ccn_node_new(CCN_EXPR_FIELD);
            if (node) {
                node->span = span;
                node->as.expr_field.field = copy_str(n->aux_s1);  /* owned */
                node->as.expr_field.is_arrow = (n->aux2 & 1) ? 1 : 0;
            }
            break;
        }
        
        case CC_AST_NODE_IF: {
            /* If statement: children are cond expr, then STMT, [else STMT] */
            node = ccn_node_new(CCN_STMT_IF);
            if (node) {
                node->span = span;
            }
            break;
        }
        
        case CC_AST_NODE_FOR: {
            /* For loop: children are init, cond, incr, body */
            node = ccn_node_new(CCN_STMT_FOR);
            if (node) {
                node->span = span;
            }
            break;
        }
        
        case CC_AST_NODE_WHILE: {
            /* While loop: children are cond, body */
            node = ccn_node_new(CCN_STMT_WHILE);
            if (node) {
                node->span = span;
            }
            break;
        }
        
        case CC_AST_NODE_BINARY: {
            /* Binary expression: aux1 contains operator token, aux_s1 is operator string */
            node = ccn_node_new(CCN_EXPR_BINARY);
            if (node) {
                node->span = span;
                /* Convert operator string to enum */
                const char* op = n->aux_s1;
                if (!op) op = "+";  /* Default */
                /* Arithmetic */
                if (strcmp(op, "+") == 0) node->as.expr_binary.op = CCN_OP_ADD;
                else if (strcmp(op, "-") == 0) node->as.expr_binary.op = CCN_OP_SUB;
                else if (strcmp(op, "*") == 0) node->as.expr_binary.op = CCN_OP_MUL;
                else if (strcmp(op, "/") == 0) node->as.expr_binary.op = CCN_OP_DIV;
                else if (strcmp(op, "%") == 0) node->as.expr_binary.op = CCN_OP_MOD;
                /* Comparison */
                else if (strcmp(op, "==") == 0) node->as.expr_binary.op = CCN_OP_EQ;
                else if (strcmp(op, "!=") == 0) node->as.expr_binary.op = CCN_OP_NE;
                else if (strcmp(op, "<") == 0) node->as.expr_binary.op = CCN_OP_LT;
                else if (strcmp(op, ">") == 0) node->as.expr_binary.op = CCN_OP_GT;
                else if (strcmp(op, "<=") == 0) node->as.expr_binary.op = CCN_OP_LE;
                else if (strcmp(op, ">=") == 0) node->as.expr_binary.op = CCN_OP_GE;
                /* Logical */
                else if (strcmp(op, "&&") == 0) node->as.expr_binary.op = CCN_OP_LAND;
                else if (strcmp(op, "||") == 0) node->as.expr_binary.op = CCN_OP_LOR;
                /* Bitwise */
                else if (strcmp(op, "&") == 0) node->as.expr_binary.op = CCN_OP_BAND;
                else if (strcmp(op, "|") == 0) node->as.expr_binary.op = CCN_OP_BOR;
                else if (strcmp(op, "^") == 0) node->as.expr_binary.op = CCN_OP_BXOR;
                else if (strcmp(op, "<<") == 0) node->as.expr_binary.op = CCN_OP_SHL;
                else if (strcmp(op, ">>") == 0) node->as.expr_binary.op = CCN_OP_SHR;
                else node->as.expr_binary.op = CCN_OP_ADD;  /* Fallback */
            }
            break;
        }
        
        case CC_AST_NODE_UNARY: {
            /* Unary expression: aux_s1 is operator string, aux1=1 if postfix */
            node = ccn_node_new(CCN_EXPR_UNARY);
            if (node) {
                node->span = span;
                const char* op = n->aux_s1;
                int is_postfix = n->aux1 ? 1 : 0;
                if (!op) op = "++";
                /* Store operator info - use postfix/prefix specific enums */
                if (strcmp(op, "++") == 0) {
                    node->as.expr_unary.op = is_postfix ? CCN_OP_POST_INC : CCN_OP_PRE_INC;
                } else if (strcmp(op, "--") == 0) {
                    node->as.expr_unary.op = is_postfix ? CCN_OP_POST_DEC : CCN_OP_PRE_DEC;
                } else if (strcmp(op, "!") == 0) {
                    node->as.expr_unary.op = CCN_OP_NOT;
                } else if (strcmp(op, "~") == 0) {
                    node->as.expr_unary.op = CCN_OP_BNOT;
                } else if (strcmp(op, "-") == 0) {
                    node->as.expr_unary.op = CCN_OP_NEG;
                } else if (strcmp(op, "&") == 0) {
                    node->as.expr_unary.op = CCN_OP_ADDR;
                } else if (strcmp(op, "*") == 0) {
                    node->as.expr_unary.op = CCN_OP_DEREF;
                } else {
                    node->as.expr_unary.op = CCN_OP_POST_INC;
                }
                node->as.expr_unary.is_postfix = is_postfix;
            }
            break;
        }
        
        case CC_AST_NODE_SIZEOF: {
            /* sizeof expression: aux_s1 contains type string */
            node = ccn_node_new(CCN_EXPR_SIZEOF);
            if (node) {
                node->span = span;
                node->as.expr_sizeof.type_str = copy_str(n->aux_s1);
                node->as.expr_sizeof.expr = NULL;  /* Not tracking expression sizeof for now */
            }
            break;
        }
        
        case CC_AST_NODE_STRUCT: {
            /* struct/union definition: aux_s1=name, aux1=VT_STRUCT/VT_UNION */
            node = ccn_node_new(CCN_STRUCT_DECL);
            if (node) {
                node->span = span;
                node->as.struct_decl.name = copy_str(n->aux_s1);
                node->as.struct_decl.is_union = (n->aux1 == 2);  /* VT_UNION = 2 */
                /* fields list is zero-initialized by calloc */
            }
            break;
        }
        
        case CC_AST_NODE_STRUCT_FIELD: {
            /* struct field: aux_s1=name, aux_s2=type */
            node = ccn_node_new(CCN_STRUCT_FIELD);
            if (node) {
                node->span = span;
                node->as.struct_field.name = copy_str(n->aux_s1);
                node->as.struct_field.type_str = copy_str(n->aux_s2);
            }
            break;
        }
        
        case CC_AST_NODE_TYPEDEF: {
            /* typedef: aux_s1=name, aux_s2=type */
            node = ccn_node_new(CCN_TYPEDEF);
            if (node) {
                node->span = span;
                node->as.typedef_decl.name = copy_str(n->aux_s1);
                node->as.typedef_decl.type_str = copy_str(n->aux_s2);
            }
            break;
        }
        
        case CC_AST_NODE_INDEX: {
            /* Array index: children are array expr and index expr */
            node = ccn_node_new(CCN_EXPR_INDEX);
            if (node) {
                node->span = span;
            }
            break;
        }
        
        case CC_AST_NODE_ENUM: {
            /* enum def: aux_s1=name */
            node = ccn_node_new(CCN_ENUM_DECL);
            if (node) {
                node->span = span;
                node->as.enum_decl.name = copy_str(n->aux_s1);
            }
            break;
        }
        
        case CC_AST_NODE_ENUM_VALUE: {
            /* enum value: aux_s1=name, aux2=value */
            node = ccn_node_new(CCN_ENUM_VALUE);
            if (node) {
                node->span = span;
                node->as.enum_value.name = copy_str(n->aux_s1);
                node->as.enum_value.value = n->aux2;
            }
            break;
        }
        
        default:
            /* Unknown node type - skip or create error node */
            break;
    }
    
    return node;
}

/* Build tree from flat TCC stub array */
static CCNNode* build_tree_from_tcc(const CCASTRoot* root, const char* filename) {
    if (!root || !root->nodes || root->node_count <= 0) {
        fprintf(stderr, "build_tree_from_tcc: no TCC nodes (count=%d)\n", 
                root ? root->node_count : -1);
        return NULL;
    }
    
    const CCNodeView* nodes = (const CCNodeView*)root->nodes;
    int count = root->node_count;
    
    if (getenv("CC_DEBUG_TCC_NODES")) {
        fprintf(stderr, "build_tree_from_tcc: %d TCC nodes from %s\n", count, filename);
        /* Debug: dump nodes from the source file */
        for (int i = 0; i < count; i++) {
            if (nodes[i].file && strstr(nodes[i].file, filename)) {
                fprintf(stderr, "  [%d] kind=%d parent=%d aux2=%d line=%d-%d aux_s1=%s aux_s2=%s\n",
                        i, nodes[i].kind, nodes[i].parent, nodes[i].aux2,
                        nodes[i].line_start, nodes[i].line_end,
                        nodes[i].aux_s1 ? nodes[i].aux_s1 : "<null>",
                        nodes[i].aux_s2 ? nodes[i].aux_s2 : "<null>");
            }
        }
    }
    
    /* Allocate array to hold converted nodes */
    CCNNode** converted = (CCNNode**)calloc(count, sizeof(CCNNode*));
    if (!converted) return NULL;
    
    /* Pass 1: Convert all TCC nodes to CCNNode */
    for (int i = 0; i < count; i++) {
        converted[i] = convert_tcc_node(&nodes[i], filename);
    }
    
    /* Pass 1.5: Fix misparented nodes.
     * TCC's node recording can misparent nodes when control flow statements
     * are followed by other statements. If a node's parent is a STMT with a
     * control-flow keyword (if/while/for/etc), and the node is a DECL or STMT
     * that's NOT the body of that control flow, re-parent it to the STMT's parent.
     * 
     * Body nodes have aux_s1="then"/"else" for if, or are BLOCKs.
     * Misparented siblings are DECLs or STMTs with other keywords like "return".
     */
    for (int i = 0; i < count; i++) {
        int parent_idx = nodes[i].parent;
        if (parent_idx < 0 || parent_idx >= count) continue;
        
        const CCNodeView* parent = &nodes[parent_idx];
        /* Check if parent is a STMT with control-flow keyword */
        if (parent->kind == CC_AST_NODE_STMT && parent->aux_s1) {
            const char* kw = parent->aux_s1;
            if (strcmp(kw, "if") == 0 || strcmp(kw, "while") == 0 ||
                strcmp(kw, "for") == 0 || strcmp(kw, "switch") == 0) {
                /* Check if this node is a sibling (not "then"/"else" body) */
                int my_kind = nodes[i].kind;
                const char* my_kw = nodes[i].aux_s1;
                
                /* Skip if this is the body of the control flow */
                if (my_kind == CC_AST_NODE_BLOCK) continue;
                if (my_kind == CC_AST_NODE_STMT && my_kw && 
                    (strcmp(my_kw, "then") == 0 || strcmp(my_kw, "else") == 0)) continue;
                
                /* DECLs and STMTs with other keywords are siblings */
                if (my_kind == CC_AST_NODE_DECL || my_kind == CC_AST_NODE_STMT ||
                    my_kind == CC_AST_NODE_RETURN) {
                    /* Re-parent to grandparent */
                    int grandparent_idx = parent->parent;
                    if (grandparent_idx >= 0 && grandparent_idx < count) {
                        ((CCNodeView*)&nodes[i])->parent = grandparent_idx;
                    }
                }
            }
        }
        
        /* Note: RETURN nodes stay in their enclosing block (including if/while bodies).
         * Previous code incorrectly lifted all returns to function body. */
    }
    
    /* Pass 2: Link children to parents.
     * We keep track of transferred nodes separately instead of nulling them,
     * so parents are still available when their children are processed. */
    int* transferred_flags = (int*)calloc(count, sizeof(int));
    if (!transferred_flags) { free(converted); return NULL; }
    
    for (int i = 0; i < count; i++) {
        int parent_idx = nodes[i].parent;
        if (parent_idx < 0 || parent_idx >= count) continue;
        if (!converted[i]) continue;
        
        /* If parent wasn't converted (e.g., DECL wrapper), try grandparent */
        while (parent_idx >= 0 && parent_idx < count && !converted[parent_idx]) {
            parent_idx = nodes[parent_idx].parent;
        }
        if (parent_idx < 0 || parent_idx >= count || !converted[parent_idx]) continue;
        
        CCNNode* child = converted[i];
        CCNNode* parent = converted[parent_idx];
        
        /* Add child to appropriate list based on parent kind.
         * Set transferred=1 if we actually attach the child. */
        int transferred = 0;
        switch (parent->kind) {
            case CCN_FUNC_DECL:
                if (child->kind == CCN_PARAM) {
                    ccn_list_push(&parent->as.func.params, child);
                    transferred = 1;
                } else if (child->kind == CCN_BLOCK) {
                    parent->as.func.body = child;
                    transferred = 1;
                }
                break;
                
            case CCN_BLOCK:
                /* Unwrap STMT_EXPR that just wraps a BLOCK containing NURSERY.
                 * TCC creates these wrapper STMTs; we want to add the children directly. */
                if (child->kind == CCN_STMT_EXPR && child->as.stmt_expr.expr &&
                    child->as.stmt_expr.expr->kind == CCN_BLOCK) {
                    CCNNode* inner_block = child->as.stmt_expr.expr;
                    /* Check if this inner block contains nursery */
                    int has_nursery = 0;
                    for (int j = 0; j < inner_block->as.block.stmts.len; j++) {
                        CCNNode* s = inner_block->as.block.stmts.items[j];
                        if (s && s->kind == CCN_STMT_NURSERY) {
                            has_nursery = 1;
                            break;
                        }
                    }
                    if (has_nursery) {
                        /* Add inner block's statements directly to parent block */
                        for (int j = 0; j < inner_block->as.block.stmts.len; j++) {
                            CCNNode* s = inner_block->as.block.stmts.items[j];
                            if (s) ccn_list_push(&parent->as.block.stmts, s);
                        }
                        /* Free the wrapper (but not its children which we transferred) */
                        inner_block->as.block.stmts.len = 0;
                        ccn_node_free(child);
                        converted[i] = NULL;
                        continue;
                    }
                }
                
                /* Skip integer literals that are array dimensions.
                 * These appear as siblings before VAR_DECL with array types. */
                if (child->kind == CCN_EXPR_LITERAL_INT) {
                    /* Look ahead for next sibling - use original TCC parent for comparison */
                    int orig_parent = nodes[i].parent;
                    int next_idx = -1;
                    for (int j = i + 1; j < count; j++) {
                        if (converted[j] && nodes[j].parent == orig_parent) {
                            next_idx = j;
                            break;
                        }
                    }
                    if (next_idx >= 0 && converted[next_idx] && 
                        converted[next_idx]->kind == CCN_VAR_DECL &&
                        converted[next_idx]->as.var.type_node &&
                        converted[next_idx]->as.var.type_node->kind == CCN_TYPE_NAME &&
                        converted[next_idx]->as.var.type_node->as.type_name.name &&
                        strchr(converted[next_idx]->as.var.type_node->as.type_name.name, '[')) {
                        /* This is an array dimension - skip it */
                        ccn_node_free(child);
                        converted[i] = NULL;
                        continue;
                    }
                }
                
                /* If child is BINARY, check if last stmt is a stray expr (its LHS) */
                if (child->kind == CCN_EXPR_BINARY && parent->as.block.stmts.len > 0) {
                    CCNNode* last = parent->as.block.stmts.items[parent->as.block.stmts.len - 1];
                    /* If last stmt is a simple expr that should be LHS */
                    if (last && (last->kind == CCN_EXPR_IDENT || 
                                 last->kind == CCN_EXPR_LITERAL_INT ||
                                 last->kind == CCN_EXPR_FIELD ||
                                 last->kind == CCN_EXPR_INDEX ||
                                 last->kind == CCN_EXPR_CALL ||
                                 last->kind == CCN_EXPR_BINARY)) {
                        if (!child->as.expr_binary.lhs) {
                            /* No LHS yet - use the preceding expr */
                            child->as.expr_binary.lhs = last;
                            parent->as.block.stmts.len--;
                        } else if (last->kind == CCN_EXPR_FIELD && 
                                   child->as.expr_binary.lhs->kind == CCN_EXPR_IDENT) {
                            /* FIELD as LHS: p.x = 10 - replace LHS IDENT with FIELD */
                            /* Check if FIELD's object matches the LHS */
                            CCNNode* field_obj = last->as.expr_field.object;
                            const char* lhs_name = child->as.expr_binary.lhs->as.expr_ident.name;
                            if (field_obj && field_obj->kind == CCN_EXPR_IDENT &&
                                lhs_name && field_obj->as.expr_ident.name &&
                                strcmp(lhs_name, field_obj->as.expr_ident.name) == 0) {
                                /* Replace the IDENT LHS with the FIELD */
                                ccn_node_free(child->as.expr_binary.lhs);
                                child->as.expr_binary.lhs = last;
                                parent->as.block.stmts.len--;
                            }
                        } else if (last->kind == CCN_EXPR_INDEX && 
                                   child->as.expr_binary.lhs->kind == CCN_EXPR_IDENT) {
                            /* INDEX as LHS: arr[i] = 10 - replace LHS IDENT with INDEX */
                            CCNNode* idx_arr = last->as.expr_index.array;
                            const char* lhs_name = child->as.expr_binary.lhs->as.expr_ident.name;
                            if (idx_arr && idx_arr->kind == CCN_EXPR_IDENT &&
                                lhs_name && idx_arr->as.expr_ident.name &&
                                strcmp(lhs_name, idx_arr->as.expr_ident.name) == 0) {
                                /* Replace the IDENT LHS with the INDEX */
                                ccn_node_free(child->as.expr_binary.lhs);
                                child->as.expr_binary.lhs = last;
                                parent->as.block.stmts.len--;
                            }
                        } else if (child->as.expr_binary.lhs->kind == CCN_EXPR_IDENT &&
                                   last->kind == CCN_EXPR_IDENT) {
                            /* BINARY already has LHS - check if last is a duplicate */
                            const char* lhs_name = child->as.expr_binary.lhs->as.expr_ident.name;
                            const char* last_name = last->as.expr_ident.name;
                            if (lhs_name && last_name && strcmp(lhs_name, last_name) == 0) {
                                /* Duplicate - remove the preceding IDENT */
                                ccn_node_free(last);
                                parent->as.block.stmts.len--;
                            }
                        }
                    }
                }
                /* If child is CALL, check if last stmt is a duplicate of the callee */
                if (child->kind == CCN_EXPR_CALL && parent->as.block.stmts.len > 0) {
                    CCNNode* last = parent->as.block.stmts.items[parent->as.block.stmts.len - 1];
                    if (last && last->kind == CCN_EXPR_IDENT && 
                        child->as.expr_call.callee && child->as.expr_call.callee->kind == CCN_EXPR_IDENT) {
                        const char* callee_name = child->as.expr_call.callee->as.expr_ident.name;
                        const char* last_name = last->as.expr_ident.name;
                        if (callee_name && last_name && strcmp(callee_name, last_name) == 0) {
                            /* Duplicate - remove the preceding IDENT */
                            ccn_node_free(last);
                            parent->as.block.stmts.len--;
                        }
                    }
                }
                /* If child is UNARY with no operand, adopt last stmt (e.g., i++) */
                if (child->kind == CCN_EXPR_UNARY && !child->as.expr_unary.operand &&
                    parent->as.block.stmts.len > 0) {
                    CCNNode* last = parent->as.block.stmts.items[parent->as.block.stmts.len - 1];
                    if (last && last->kind == CCN_EXPR_IDENT) {
                        child->as.expr_unary.operand = last;
                        parent->as.block.stmts.len--;
                    }
                }
                /* If child is FIELD with no object, adopt last stmt (e.g., p.x) */
                if (child->kind == CCN_EXPR_FIELD && !child->as.expr_field.object &&
                    parent->as.block.stmts.len > 0) {
                    CCNNode* last = parent->as.block.stmts.items[parent->as.block.stmts.len - 1];
                    if (last && (last->kind == CCN_EXPR_IDENT || 
                                 last->kind == CCN_EXPR_FIELD ||
                                 last->kind == CCN_EXPR_CALL)) {
                        child->as.expr_field.object = last;
                        parent->as.block.stmts.len--;
                    }
                }
                /* If child is INDEX with no array, adopt last stmt (e.g., arr[0]) */
                if (child->kind == CCN_EXPR_INDEX && !child->as.expr_index.array &&
                    parent->as.block.stmts.len > 0) {
                    CCNNode* last = parent->as.block.stmts.items[parent->as.block.stmts.len - 1];
                    if (last && (last->kind == CCN_EXPR_IDENT || 
                                 last->kind == CCN_EXPR_INDEX ||
                                 last->kind == CCN_EXPR_FIELD)) {
                        child->as.expr_index.array = last;
                        parent->as.block.stmts.len--;
                    }
                }
                ccn_list_push(&parent->as.block.stmts, child);
                transferred = 1;
                break;
                
            case CCN_STMT_NURSERY:
            case CCN_STMT_ARENA:
                /* Body should be BLOCK or STMT (wrapper). Skip VAR_DECL. */
                if (child->kind == CCN_BLOCK || 
                    child->kind == CCN_STMT_EXPR ||
                    child->kind == CCN_STMT_SPAWN ||
                    child->kind == CCN_STMT_DEFER) {
                    if (!parent->as.stmt_scope.body) {
                        parent->as.stmt_scope.body = child;
                        transferred = 1;
                    }
                }
                /* VAR_DECL in nursery is the implicit nursery variable - skip for now */
                break;
                
            case CCN_STMT_SPAWN:
                if (child->kind == CCN_EXPR_CLOSURE) {
                    parent->as.stmt_spawn.closure = child;
                    transferred = 1;
                } else if (!parent->as.stmt_spawn.closure) {
                    /* Could be wrapped - accept as expr */
                    parent->as.stmt_spawn.closure = child;
                    transferred = 1;
                }
                break;
            
            case CCN_STMT_EXPR:
                /* Expression statements can wrap blocks or expressions.
                 * TCC wraps compound blocks in STMT nodes. */
                if (!parent->as.stmt_expr.expr) {
                    parent->as.stmt_expr.expr = child;
                    transferred = 1;
                } else {
                    /* STMT_EXPR already has expr - overflow.
                     * This happens with @nursery where TCC wraps the nursery block AND
                     * subsequent statements in a single STMT. Reparent to grandparent BLOCK. */
                    int gp_idx = nodes[parent_idx].parent;
                    while (gp_idx >= 0 && gp_idx < count && !converted[gp_idx]) {
                        gp_idx = nodes[gp_idx].parent;
                    }
                    if (gp_idx >= 0 && gp_idx < count && converted[gp_idx] &&
                        converted[gp_idx]->kind == CCN_BLOCK) {
                        ccn_list_push(&converted[gp_idx]->as.block.stmts, child);
                        transferred = 1;
                    }
                }
                break;
                
            case CCN_STMT_DEFER:
                /* Defer wraps a statement */
                if (!parent->as.stmt_defer.stmt) {
                    parent->as.stmt_defer.stmt = child;
                    transferred = 1;
                }
                break;
                
            case CCN_VAR_DECL:
                /* Variable declarations can have expression initializers.
                 * Skip FUNC_DECL, BLOCK, VAR_DECL, STRUCT_DECL, STRUCT_FIELD, TYPEDEF, ENUM_DECL, ENUM_VALUE
                 * (these should go to the enclosing block/file, not nest inside). */
                if (child->kind != CCN_FUNC_DECL && 
                    child->kind != CCN_BLOCK &&
                    child->kind != CCN_VAR_DECL &&
                    child->kind != CCN_STRUCT_DECL &&
                    child->kind != CCN_STRUCT_FIELD &&
                    child->kind != CCN_TYPEDEF &&
                    child->kind != CCN_ENUM_DECL &&
                    child->kind != CCN_ENUM_VALUE) {
                    /* Special case: CALL replaces existing IDENT init (callee was recorded separately) */
                    if (child->kind == CCN_EXPR_CALL && parent->as.var.init &&
                        parent->as.var.init->kind == CCN_EXPR_IDENT) {
                        /* The existing IDENT was the callee - don't duplicate it */
                        parent->as.var.init = child;
                        transferred = 1;
                    }
                    /* Special case: METHOD - existing init becomes receiver (UFCS) */
                    else if (child->kind == CCN_EXPR_METHOD && parent->as.var.init &&
                             parent->as.var.init->kind == CCN_EXPR_IDENT) {
                        /* The existing IDENT is the UFCS receiver */
                        if (!child->as.expr_method.receiver) {
                            child->as.expr_method.receiver = parent->as.var.init;
                        }
                        parent->as.var.init = child;
                        transferred = 1;
                    }
                    /* Special case: Chained METHOD - existing METHOD becomes receiver (a.f().g()) */
                    else if (child->kind == CCN_EXPR_METHOD && parent->as.var.init &&
                             parent->as.var.init->kind == CCN_EXPR_METHOD) {
                        /* Chain: the previous METHOD result is the receiver for this METHOD */
                        if (!child->as.expr_method.receiver) {
                            child->as.expr_method.receiver = parent->as.var.init;
                        }
                        parent->as.var.init = child;
                        transferred = 1;
                    }
                    /* Special case: BINARY expr - existing init becomes LHS */
                    else if (child->kind == CCN_EXPR_BINARY && parent->as.var.init) {
                        if (!child->as.expr_binary.lhs) {
                            child->as.expr_binary.lhs = parent->as.var.init;
                        }
                        parent->as.var.init = child;
                        transferred = 1;
                    } else if (!parent->as.var.init) {
                        parent->as.var.init = child;
                        transferred = 1;
                    }
                    /* Multiple literal values - create/extend compound literal for struct init */
                    else if ((child->kind == CCN_EXPR_LITERAL_INT || 
                              child->kind == CCN_EXPR_LITERAL_FLOAT ||
                              child->kind == CCN_EXPR_LITERAL_STRING ||
                              child->kind == CCN_EXPR_IDENT) &&
                             (parent->as.var.init->kind == CCN_EXPR_LITERAL_INT ||
                              parent->as.var.init->kind == CCN_EXPR_LITERAL_FLOAT ||
                              parent->as.var.init->kind == CCN_EXPR_LITERAL_STRING ||
                              parent->as.var.init->kind == CCN_EXPR_IDENT ||
                              parent->as.var.init->kind == CCN_EXPR_COMPOUND)) {
                        /* Convert init to compound if needed */
                        if (parent->as.var.init->kind != CCN_EXPR_COMPOUND) {
                            CCNNode* compound = ccn_node_new(CCN_EXPR_COMPOUND);
                            if (compound) {
                                compound->span = parent->as.var.init->span;
                                ccn_list_push(&compound->as.expr_compound.values, parent->as.var.init);
                                parent->as.var.init = compound;
                            }
                        }
                        /* Add child to compound */
                        if (parent->as.var.init->kind == CCN_EXPR_COMPOUND) {
                            ccn_list_push(&parent->as.var.init->as.expr_compound.values, child);
                            transferred = 1;
                        }
                    }
                }
                break;
                
            case CCN_EXPR_CALL:
                /* If child is BINARY with no LHS, and last arg is a simple expr, 
                 * move last arg to be BINARY's LHS (e.g., factorial(n - 1)) */
                if (child->kind == CCN_EXPR_BINARY && !child->as.expr_binary.lhs &&
                    parent->as.expr_call.args.len > 0) {
                    CCNNode* last = parent->as.expr_call.args.items[parent->as.expr_call.args.len - 1];
                    if (last && (last->kind == CCN_EXPR_IDENT ||
                                 last->kind == CCN_EXPR_LITERAL_INT ||
                                 last->kind == CCN_EXPR_CALL)) {
                        child->as.expr_binary.lhs = last;
                        parent->as.expr_call.args.len--;
                    }
                }
                /* If child is CALL and last arg is a duplicate of its callee, remove it */
                if (child->kind == CCN_EXPR_CALL && parent->as.expr_call.args.len > 0 &&
                    child->as.expr_call.callee && child->as.expr_call.callee->kind == CCN_EXPR_IDENT) {
                    CCNNode* last = parent->as.expr_call.args.items[parent->as.expr_call.args.len - 1];
                    if (last && last->kind == CCN_EXPR_IDENT) {
                        const char* callee_name = child->as.expr_call.callee->as.expr_ident.name;
                        const char* last_name = last->as.expr_ident.name;
                        if (callee_name && last_name && strcmp(callee_name, last_name) == 0) {
                            ccn_node_free(last);
                            parent->as.expr_call.args.len--;
                        }
                    }
                }
                /* If child is METHOD and last arg is IDENT, use it as receiver */
                if (child->kind == CCN_EXPR_METHOD && !child->as.expr_method.receiver &&
                    parent->as.expr_call.args.len > 0) {
                    CCNNode* last = parent->as.expr_call.args.items[parent->as.expr_call.args.len - 1];
                    if (last && last->kind == CCN_EXPR_IDENT) {
                        child->as.expr_method.receiver = last;
                        parent->as.expr_call.args.len--;
                    }
                    /* Chained METHOD: if last arg is METHOD, chain them */
                    else if (last && last->kind == CCN_EXPR_METHOD) {
                        child->as.expr_method.receiver = last;
                        parent->as.expr_call.args.len--;
                    }
                }
                /* If child is FIELD and last arg is IDENT, use it as object */
                if (child->kind == CCN_EXPR_FIELD && !child->as.expr_field.object &&
                    parent->as.expr_call.args.len > 0) {
                    CCNNode* last = parent->as.expr_call.args.items[parent->as.expr_call.args.len - 1];
                    if (last && (last->kind == CCN_EXPR_IDENT ||
                                 last->kind == CCN_EXPR_FIELD ||
                                 last->kind == CCN_EXPR_CALL)) {
                        child->as.expr_field.object = last;
                        parent->as.expr_call.args.len--;
                    }
                }
                /* If child is INDEX and last arg is IDENT, use it as array */
                if (child->kind == CCN_EXPR_INDEX && !child->as.expr_index.array &&
                    parent->as.expr_call.args.len > 0) {
                    CCNNode* last = parent->as.expr_call.args.items[parent->as.expr_call.args.len - 1];
                    if (last && (last->kind == CCN_EXPR_IDENT ||
                                 last->kind == CCN_EXPR_INDEX ||
                                 last->kind == CCN_EXPR_FIELD)) {
                        child->as.expr_index.array = last;
                        parent->as.expr_call.args.len--;
                    }
                }
                ccn_list_push(&parent->as.expr_call.args, child);
                transferred = 1;
                break;
                
            case CCN_EXPR_METHOD:
                /* First child is receiver, rest are args */
                if (!parent->as.expr_method.receiver) {
                    parent->as.expr_method.receiver = child;
                } else {
                    ccn_list_push(&parent->as.expr_method.args, child);
                }
                transferred = 1;
                break;
                
            case CCN_EXPR_AWAIT:
                /* UFCS: TCC puts receiver IDENT and METHOD/CALL as siblings under AWAIT.
                 * When we see a METHOD/CALL, move any existing IDENT to be its receiver.
                 * For regular (non-UFCS) calls, the prev IDENT is just the callee - don't prepend. */
                if (child->kind == CCN_EXPR_METHOD || child->kind == CCN_EXPR_CALL) {
                    CCNNode* prev = parent->as.expr_await.expr;
                    /* If previous expr was an IDENT, it might be the UFCS receiver */
                    if (prev && prev->kind == CCN_EXPR_IDENT) {
                        if (child->kind == CCN_EXPR_METHOD && !child->as.expr_method.receiver) {
                            /* METHOD always needs a receiver */
                            child->as.expr_method.receiver = prev;
                        } else if (child->kind == CCN_EXPR_CALL) {
                            /* Only prepend as receiver if this is UFCS (callee name differs from prev).
                             * If CALL's callee matches prev IDENT name, it's the same thing - skip. */
                            int is_same = 0;
                            if (child->as.expr_call.callee && 
                                child->as.expr_call.callee->kind == CCN_EXPR_IDENT &&
                                child->as.expr_call.callee->as.expr_ident.name &&
                                prev->as.expr_ident.name) {
                                is_same = (strcmp(child->as.expr_call.callee->as.expr_ident.name,
                                                  prev->as.expr_ident.name) == 0);
                            }
                            if (!is_same) {
                                /* Different name - prepend as receiver (UFCS) */
                                CCNNodeList new_args = {0};
                                ccn_list_push(&new_args, prev);
                                for (int j = 0; j < child->as.expr_call.args.len; j++) {
                                    ccn_list_push(&new_args, child->as.expr_call.args.items[j]);
                                }
                                free(child->as.expr_call.args.items);
                                child->as.expr_call.args = new_args;
                            }
                        }
                    }
                    parent->as.expr_await.expr = child;
                    transferred = 1;
                } else if (!parent->as.expr_await.expr) {
                    parent->as.expr_await.expr = child;
                    transferred = 1;
                }
                break;
                
            case CCN_EXPR_TRY:
                /* try expr - inner expression becomes expr field.
                 * CALL replaces existing IDENT (callee was recorded separately). */
                if (child->kind == CCN_EXPR_CALL && parent->as.expr_try.expr &&
                    parent->as.expr_try.expr->kind == CCN_EXPR_IDENT) {
                    parent->as.expr_try.expr = child;
                    transferred = 1;
                } else if (!parent->as.expr_try.expr) {
                    parent->as.expr_try.expr = child;
                    transferred = 1;
                }
                break;
                
            case CCN_EXPR_CLOSURE:
                if (child->kind == CCN_PARAM) {
                    ccn_list_push(&parent->as.expr_closure.params, child);
                    transferred = 1;
                } else if (child->kind == CCN_EXPR_BINARY && !child->as.expr_binary.lhs &&
                           parent->as.expr_closure.body) {
                    /* BINARY needs to adopt the current body as LHS */
                    child->as.expr_binary.lhs = parent->as.expr_closure.body;
                    parent->as.expr_closure.body = child;
                    transferred = 1;
                } else if (child->kind == CCN_BLOCK || 
                           (child->kind >= CCN_EXPR_IDENT && child->kind <= CCN_EXPR_TRY)) {
                    parent->as.expr_closure.body = child;
                    transferred = 1;
                }
                break;
                
            case CCN_STMT_RETURN:
                /* If child is CALL and current value is duplicate of callee, replace it */
                if (child->kind == CCN_EXPR_CALL && parent->as.stmt_return.value &&
                    parent->as.stmt_return.value->kind == CCN_EXPR_IDENT &&
                    child->as.expr_call.callee && child->as.expr_call.callee->kind == CCN_EXPR_IDENT) {
                    const char* val_name = parent->as.stmt_return.value->as.expr_ident.name;
                    const char* callee_name = child->as.expr_call.callee->as.expr_ident.name;
                    if (val_name && callee_name && strcmp(val_name, callee_name) == 0) {
                        ccn_node_free(parent->as.stmt_return.value);
                        parent->as.stmt_return.value = child;
                        transferred = 1;
                        break;
                    }
                }
                /* If child is BINARY and we already have a value, that value is the LHS */
                if (child->kind == CCN_EXPR_BINARY && parent->as.stmt_return.value) {
                    if (!child->as.expr_binary.lhs) {
                        child->as.expr_binary.lhs = parent->as.stmt_return.value;
                    }
                    parent->as.stmt_return.value = child;
                    transferred = 1;
                } else if (!parent->as.stmt_return.value) {
                    parent->as.stmt_return.value = child;
                    transferred = 1;
                }
                break;
                
            case CCN_STMT_IF:
                /* If statement: first non-stmt child is cond, then branches
                 * Check aux_s1 of STMT children to distinguish then/else */
                if (child->kind == CCN_STMT_EXPR || child->kind == CCN_BLOCK) {
                    /* Look at original TCC node's aux_s1 to determine role */
                    /* For now, assign based on order */
                    if (!parent->as.stmt_if.then_branch) {
                        parent->as.stmt_if.then_branch = child;
                        transferred = 1;
                    } else if (!parent->as.stmt_if.else_branch) {
                        parent->as.stmt_if.else_branch = child;
                        transferred = 1;
                    }
                } else if (child->kind == CCN_EXPR_BINARY && parent->as.stmt_if.cond) {
                    /* Binary arrived after simple cond - cond becomes BINARY's LHS */
                    if (!child->as.expr_binary.lhs) {
                        child->as.expr_binary.lhs = parent->as.stmt_if.cond;
                    }
                    parent->as.stmt_if.cond = child;
                    transferred = 1;
                } else if (child->kind == CCN_EXPR_FIELD && parent->as.stmt_if.cond) {
                    /* Field access after simple cond (e.g., if (opt.has)) */
                    if (!child->as.expr_field.object) {
                        child->as.expr_field.object = parent->as.stmt_if.cond;
                    }
                    parent->as.stmt_if.cond = child;
                    transferred = 1;
                } else if (!parent->as.stmt_if.cond) {
                    /* First expression child is the condition */
                    parent->as.stmt_if.cond = child;
                    transferred = 1;
                }
                break;
            
            case CCN_STMT_FOR: {
                /* For loop: children are init (VAR_DECL or expr), cond, incr, body */
                if (child->kind == CCN_STMT_EXPR || child->kind == CCN_BLOCK) {
                    /* Body arrives last */
                    parent->as.stmt_for.body = child;
                    transferred = 1;
                } else if (child->kind == CCN_VAR_DECL) {
                    /* Variable declaration is the init */
                    parent->as.stmt_for.init = child;
                    transferred = 1;
                } else if (child->kind == CCN_EXPR_BINARY) {
                    /* Binary expressions: order is cond, then incr */
                    if (!parent->as.stmt_for.cond) {
                        /* Handle case where cond's LHS arrives as earlier sibling */
                        if (!child->as.expr_binary.lhs && parent->as.stmt_for.incr &&
                            (parent->as.stmt_for.incr->kind == CCN_EXPR_IDENT ||
                             parent->as.stmt_for.incr->kind == CCN_EXPR_UNARY)) {
                            /* "incr" slot was actually holding the cond LHS */
                            child->as.expr_binary.lhs = parent->as.stmt_for.incr;
                            parent->as.stmt_for.incr = NULL;
                        }
                        parent->as.stmt_for.cond = child;
                        transferred = 1;
                    } else if (!parent->as.stmt_for.incr) {
                        /* This is the incr (e.g., i = i + 1 or i += 1) */
                        parent->as.stmt_for.incr = child;
                        transferred = 1;
                    }
                } else if (child->kind == CCN_EXPR_UNARY) {
                    /* Unary expression (like i++ for incr) - adopt preceding IDENT as operand */
                    if (!child->as.expr_unary.operand && parent->as.stmt_for.incr &&
                        parent->as.stmt_for.incr->kind == CCN_EXPR_IDENT) {
                        /* The incr slot was holding the operand */
                        child->as.expr_unary.operand = parent->as.stmt_for.incr;
                        parent->as.stmt_for.incr = NULL;
                    }
                    if (parent->as.stmt_for.cond && !parent->as.stmt_for.incr) {
                        parent->as.stmt_for.incr = child;
                        transferred = 1;
                    } else if (!parent->as.stmt_for.incr) {
                        parent->as.stmt_for.incr = child;
                        transferred = 1;
                    }
                } else if (!parent->as.stmt_for.cond && parent->as.stmt_for.init) {
                    /* Simple expression after init - might be cond LHS, store in incr temporarily */
                    parent->as.stmt_for.incr = child;
                    transferred = 1;
                } else if (!parent->as.stmt_for.init) {
                    /* Simple expression as init */
                    parent->as.stmt_for.init = child;
                    transferred = 1;
                } else if (!parent->as.stmt_for.incr) {
                    /* Simple expression as incr */
                    parent->as.stmt_for.incr = child;
                    transferred = 1;
                }
                break;
            }
            
            case CCN_STMT_WHILE:
                /* While loop: children are cond, body */
                if (child->kind == CCN_STMT_EXPR || child->kind == CCN_BLOCK) {
                    parent->as.stmt_while.body = child;
                    transferred = 1;
                } else if (child->kind == CCN_EXPR_BINARY && parent->as.stmt_while.cond) {
                    /* Binary arrived after simple cond - cond becomes BINARY's LHS */
                    if (!child->as.expr_binary.lhs) {
                        child->as.expr_binary.lhs = parent->as.stmt_while.cond;
                    }
                    parent->as.stmt_while.cond = child;
                    transferred = 1;
                } else if (!parent->as.stmt_while.cond) {
                    parent->as.stmt_while.cond = child;
                    transferred = 1;
                }
                break;
                
            case CCN_EXPR_BINARY:
                /* Binary: children are the RHS. LHS comes from sibling.
                 * For ASSIGN nodes, LHS may already be set from aux_s1. */
                if (!parent->as.expr_binary.rhs) {
                    parent->as.expr_binary.rhs = child;
                    transferred = 1;
                } else if (child->kind == CCN_EXPR_INDEX && !child->as.expr_index.array &&
                           parent->as.expr_binary.rhs && parent->as.expr_binary.rhs->kind == CCN_EXPR_IDENT) {
                    /* INDEX coming after IDENT - IDENT is the array */
                    child->as.expr_index.array = parent->as.expr_binary.rhs;
                    parent->as.expr_binary.rhs = child;
                    transferred = 1;
                } else if (child->kind == CCN_EXPR_BINARY) {
                    /* Nested binary: e.g., "x = y + 2" where RHS is already "y"
                     * Move existing RHS to be BINARY's LHS */
                    if (!child->as.expr_binary.lhs) {
                        child->as.expr_binary.lhs = parent->as.expr_binary.rhs;
                    }
                    parent->as.expr_binary.rhs = child;
                    transferred = 1;
                } else if (child->kind == CCN_EXPR_CALL) {
                    /* CALL as RHS: e.g., "n * factorial(n-1)" 
                     * If RHS is just an IDENT matching the callee, replace it */
                    if (parent->as.expr_binary.rhs && 
                        parent->as.expr_binary.rhs->kind == CCN_EXPR_IDENT &&
                        child->as.expr_call.callee &&
                        child->as.expr_call.callee->kind == CCN_EXPR_IDENT) {
                        const char* rhs_name = parent->as.expr_binary.rhs->as.expr_ident.name;
                        const char* callee_name = child->as.expr_call.callee->as.expr_ident.name;
                        if (rhs_name && callee_name && strcmp(rhs_name, callee_name) == 0) {
                            ccn_node_free(parent->as.expr_binary.rhs);
                            parent->as.expr_binary.rhs = child;
                            transferred = 1;
                        }
                    }
                } else if (child->kind == CCN_EXPR_METHOD) {
                    /* METHOD as RHS (UFCS): e.g., "a.add(1) + b.mul(2)"
                     * If RHS is an IDENT, use it as the METHOD's receiver */
                    if (parent->as.expr_binary.rhs && 
                        parent->as.expr_binary.rhs->kind == CCN_EXPR_IDENT &&
                        !child->as.expr_method.receiver) {
                        child->as.expr_method.receiver = parent->as.expr_binary.rhs;
                        parent->as.expr_binary.rhs = child;
                        transferred = 1;
                    }
                }
                break;
                
            case CCN_EXPR_FIELD:
                /* Field access: child is the object expression */
                if (!parent->as.expr_field.object) {
                    parent->as.expr_field.object = child;
                    transferred = 1;
                }
                break;
                
            case CCN_EXPR_UNARY:
                /* Unary expression: child is the operand */
                if (!parent->as.expr_unary.operand) {
                    parent->as.expr_unary.operand = child;
                    transferred = 1;
                }
                break;
                
            case CCN_STRUCT_DECL:
                /* Struct definition: children are fields */
                if (child->kind == CCN_STRUCT_FIELD) {
                    ccn_list_push(&parent->as.struct_decl.fields, child);
                    transferred = 1;
                }
                break;
                
            case CCN_ENUM_DECL:
                /* Enum definition: children are values */
                if (child->kind == CCN_ENUM_VALUE) {
                    ccn_list_push(&parent->as.enum_decl.values, child);
                    transferred = 1;
                }
                break;
                
            case CCN_EXPR_INDEX:
                /* Array index: first child is array, second is index */
                if (!parent->as.expr_index.array) {
                    parent->as.expr_index.array = child;
                    transferred = 1;
                } else if (!parent->as.expr_index.index) {
                    parent->as.expr_index.index = child;
                    transferred = 1;
                }
                break;
                
            default:
                /* Parent type doesn't accept this child - leave child orphaned */
                break;
        }
        
        /* Mark child as transferred only if we actually attached it */
        if (transferred) {
            transferred_flags[i] = 1;
        }
    }
    
    /* Pass 3: TCC puts FUNC and BLOCK as siblings under DECL_ITEM.
     * Find FUNC nodes without bodies and look for sibling BLOCKs. */
    for (int i = 0; i < count; i++) {
        if (transferred_flags[i]) continue;
        if (!converted[i]) continue;
        if (converted[i]->kind != CCN_FUNC_DECL) continue;
        if (converted[i]->as.func.body) continue;  /* Already has body */
        
        int parent_idx = nodes[i].parent;
        if (parent_idx < 0) continue;
        
        /* Look for sibling BLOCK with same parent */
        for (int j = 0; j < count; j++) {
            if (i == j) continue;
            if (transferred_flags[j]) continue;
            if (!converted[j]) continue;
            if (nodes[j].parent != parent_idx) continue;
            if (converted[j]->kind != CCN_BLOCK) continue;
            
            /* Found sibling BLOCK - attach as body */
            converted[i]->as.func.body = converted[j];
            transferred_flags[j] = 1;
            break;
        }
    }
    
    /* Find root nodes from the source file.
     * TCC nests user declarations under prelude declarations, so we look for
     * FUNC (kind=17), STRUCT (kind=25), etc. nodes whose file matches ours.
     */
    CCNNode* file_node = ccn_node_new(CCN_FILE);
    if (file_node) {
        file_node->as.file.path = filename;
        
        for (int i = 0; i < count; i++) {
            /* Only look at file-level nodes from the source file */
            int kind = nodes[i].kind;
            if (kind != CC_AST_NODE_FUNC && 
                kind != CC_AST_NODE_STRUCT &&
                kind != CC_AST_NODE_TYPEDEF &&
                kind != CC_AST_NODE_ENUM &&
                kind != CC_AST_NODE_DECL_ITEM) continue;
            if (!nodes[i].file) continue;
            
            /* Skip parse stub declarations (names starting with CC or __CC) */
            if (kind == CC_AST_NODE_DECL_ITEM && nodes[i].aux_s1) {
                const char* name = nodes[i].aux_s1;
                if (strncmp(name, "__CC", 4) == 0 ||
                    strncmp(name, "CC", 2) == 0 ||
                    strncmp(name, "__cc", 4) == 0) {
                    continue;
                }
            }
            
            /* Match source file (by basename since paths may differ) */
            const char* node_file = nodes[i].file;
            const char* node_base = strrchr(node_file, '/');
            node_base = node_base ? node_base + 1 : node_file;
            
            const char* src_base = strrchr(filename, '/');
            src_base = src_base ? src_base + 1 : filename;
            
            if (strcmp(node_base, src_base) != 0) continue;
            
            /* Check if this node was converted and not yet transferred */
            if (!converted[i]) continue;
            if (transferred_flags[i]) continue;
            
            ccn_list_push(&file_node->as.file.items, converted[i]);
            transferred_flags[i] = 1;
        }
    }
    
    /* Free any orphaned nodes (not transferred to any parent) */
    for (int i = 0; i < count; i++) {
        if (converted[i] && !transferred_flags[i]) {
            ccn_node_free(converted[i]);
        }
    }
    free(converted);
    free(transferred_flags);
    
    return file_node;
}

/* Scan source for #include directives and add to AST */
static void scan_includes(const char* source, CCNNode* file_node, const char* filename) {
    if (!source || !file_node || file_node->kind != CCN_FILE) return;
    
    CCNNodeList includes = {0};
    const char* p = source;
    int line = 1;
    
    while (*p) {
        /* Skip whitespace at start of line */
        while (*p && (*p == ' ' || *p == '\t')) p++;
        
        /* Check for #include */
        if (*p == '#') {
            const char* directive = p + 1;
            while (*directive == ' ' || *directive == '\t') directive++;
            
            if (strncmp(directive, "include", 7) == 0) {
                directive += 7;
                while (*directive == ' ' || *directive == '\t') directive++;
                
                char delim = *directive;
                int is_system = (delim == '<');
                if (delim == '"' || delim == '<') {
                    directive++;
                    const char* end = directive;
                    char end_delim = (delim == '<') ? '>' : '"';
                    while (*end && *end != end_delim && *end != '\n') end++;
                    
                    if (*end == end_delim) {
                        size_t len = end - directive;
                        char* path = malloc(len + 1);
                        if (path) {
                            memcpy(path, directive, len);
                            path[len] = '\0';
                            
                            /* Create include node */
                            CCNNode* inc = ccn_node_new(CCN_INCLUDE);
                            if (inc) {
                                inc->span.start.line = line;
                                inc->span.end.line = line;
                                inc->span.start.file = filename;
                                inc->as.include.path = path;
                                inc->as.include.is_system = is_system;
                                ccn_list_push(&includes, inc);
                            } else {
                                free(path);
                            }
                        }
                    }
                }
            }
        }
        
        /* Skip to end of line */
        while (*p && *p != '\n') p++;
        if (*p == '\n') { p++; line++; }
    }
    
    /* Prepend includes to file items (in order) */
    if (includes.len > 0) {
        CCNNodeList new_items = {0};
        for (int i = 0; i < includes.len; i++) {
            ccn_list_push(&new_items, includes.items[i]);
        }
        for (int i = 0; i < file_node->as.file.items.len; i++) {
            ccn_list_push(&new_items, file_node->as.file.items.items[i]);
        }
        free(includes.items);
        free(file_node->as.file.items.items);
        file_node->as.file.items = new_items;
    }
}

CCNFile* cc_parse_file(const char* path) {
    if (!path) return NULL;
    
    /* Read source file */
    char* source = NULL;
    size_t source_len = 0;
    /* cc__read_entire_file returns 1 on success, 0 on failure */
    if (cc__read_entire_file(path, &source, &source_len) == 0 || !source) {
        fprintf(stderr, "cccn: failed to read %s\n", path);
        return NULL;
    }
    
    CCNFile* file = cc_parse_string(source, path);
    
    /* Scan source for includes (before preprocessing ate them) */
    if (file && file->root) {
        scan_includes(source, file->root, path);
    }
    
    free(source);
    return file;
}

CCNFile* cc_parse_string(const char* source, const char* filename) {
    if (!source) return NULL;
    if (!filename) filename = "<input>";
    
    /* Simple preprocessing - just add #line, no CC syntax rewrites.
     * All CC syntax (try, await, closures, etc.) handled by TCC hooks + AST passes. */
    char* preprocessed = cc_preprocess_simple(source, strlen(source), filename);
    if (!preprocessed) {
        fprintf(stderr, "cc: preprocessing failed for %s\n", filename);
        return NULL;
    }
    
    /* Parse with TCC to get stub AST */
    CCSymbolTable* symbols = cc_symbols_new();
    CCASTRoot* tcc_root = cc_tcc_bridge_parse_string_to_ast(
        preprocessed, filename, filename, symbols
    );
    free(preprocessed);
    
    if (!tcc_root) {
        fprintf(stderr, "cc: parsing failed for %s\n", filename);
        cc_symbols_free(symbols);
        return NULL;
    }
    
    /* Convert TCC stub AST to CCNNode tree */
    CCNNode* root_node = build_tree_from_tcc(tcc_root, filename);
    
    /* Free TCC AST (we've extracted what we need) */
    cc_tcc_bridge_free_ast(tcc_root);
    cc_symbols_free(symbols);
    
    if (!root_node) {
        fprintf(stderr, "cc: AST conversion failed for %s\n", filename);
        return NULL;
    }
    
    /* Create CCNFile wrapper */
    CCNFile* file = (CCNFile*)calloc(1, sizeof(CCNFile));
    if (!file) {
        ccn_node_free(root_node);
        return NULL;
    }
    
    file->filename = filename;
    file->root = root_node;
    
    return file;
}

void cc_file_free(CCNFile* file) {
    if (!file) return;
    if (file->root) {
        ccn_node_free(file->root);
    }
    free(file);
}
