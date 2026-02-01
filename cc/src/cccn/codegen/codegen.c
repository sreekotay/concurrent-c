#include "codegen.h"
#include <stdlib.h>
#include <string.h>

/* Forward declarations */
typedef struct ClosureEmitCtx ClosureEmitCtx;
static void emit_node(const CCNNode* node, FILE* out, int indent);
static void emit_node_ctx(const CCNNode* node, FILE* out, int indent, ClosureEmitCtx* ctx);

/* Track last emitted line for #line directive optimization */
typedef struct {
    const char* file;
    int line;
} EmitState;

/* Context for closure body emission - maps param names to __argN */
struct ClosureEmitCtx {
    const char** param_names;
    int param_count;
};

static EmitState g_emit_state = {0};

/* Current file being emitted (for closure type lookup) */
static const CCNFile* g_current_file = NULL;

/* Check if AST contains nursery statements (recursive helper) */
static int has_nursery_stmt(const CCNNode* node) {
    if (!node) return 0;
    if (node->kind == CCN_STMT_NURSERY) return 1;
    
    /* Check all possible child locations */
    switch (node->kind) {
        case CCN_FILE:
            for (int i = 0; i < node->as.file.items.len; i++) {
                if (has_nursery_stmt(node->as.file.items.items[i])) return 1;
            }
            break;
        case CCN_FUNC_DECL:
            if (has_nursery_stmt(node->as.func.body)) return 1;
            break;
        case CCN_BLOCK:
            for (int i = 0; i < node->as.block.stmts.len; i++) {
                if (has_nursery_stmt(node->as.block.stmts.items[i])) return 1;
            }
            break;
        case CCN_STMT_EXPR:
            if (has_nursery_stmt(node->as.stmt_expr.expr)) return 1;
            break;
        case CCN_STMT_IF:
            if (has_nursery_stmt(node->as.stmt_if.then_branch)) return 1;
            if (has_nursery_stmt(node->as.stmt_if.else_branch)) return 1;
            break;
        case CCN_STMT_FOR:
            if (has_nursery_stmt(node->as.stmt_for.body)) return 1;
            break;
        case CCN_STMT_WHILE:
            if (has_nursery_stmt(node->as.stmt_while.body)) return 1;
            break;
        case CCN_STMT_NURSERY:
            return 1;
        default:
            break;
    }
    return 0;
}

static void emit_indent(int indent, FILE* out) {
    for (int i = 0; i < indent; i++) fprintf(out, "  ");
}

/* Emit #line directive if line changed */
static void emit_line_directive(const CCNNode* node, FILE* out) {
    if (!node || !node->span.start.file) return;
    
    const char* file = node->span.start.file;
    int line = node->span.start.line;
    
    /* Skip if line/file unchanged */
    if (g_emit_state.file && strcmp(g_emit_state.file, file) == 0 && 
        g_emit_state.line == line) {
        return;
    }
    
    /* Skip if line is 0 (unknown) */
    if (line <= 0) return;
    
    fprintf(out, "#line %d \"%s\"\n", line, file);
    g_emit_state.file = file;
    g_emit_state.line = line;
}

/* Mark that we're in generated code (resets tracking) */
static void emit_generated_section(FILE* out, const char* name) {
    fprintf(out, "#line 1 \"<cc-generated:%s>\"\n", name);
    g_emit_state.file = NULL;
    g_emit_state.line = 0;
}

/* Emit closure definitions (env struct, drop fn, entry fn, make fn) */
static void emit_closure_defs(const CCNFile* file, FILE* out) {
    if (!file || file->closure_count <= 0 || !file->closure_defs) return;
    
    fprintf(out, "/* ===== Generated Closure Definitions ===== */\n\n");
    
    for (int i = 0; i < file->closure_count; i++) {
        const CCClosureDef* def = &file->closure_defs[i];
        int id = def->id;
        int has_captures = def->captures.len > 0;
        
        /* Determine closure type based on param count */
        const char* closure_type;
        const char* make_fn;
        switch (def->param_count) {
            case 0:  closure_type = "CCClosure0"; make_fn = "cc_closure0_make"; break;
            case 1:  closure_type = "CCClosure1"; make_fn = "cc_closure1_make"; break;
            default: closure_type = "CCClosure2"; make_fn = "cc_closure2_make"; break;
        }
        
        if (has_captures) {
            /* Emit env struct */
            fprintf(out, "typedef struct __cc_closure_env_%d {\n", id);
            for (int j = 0; j < def->captures.len; j++) {
                CCNNode* cap = def->captures.items[j];
                if (cap && cap->kind == CCN_EXPR_IDENT) {
                    /* Use intptr_t to store captures (works for both ints and pointers) */
                    fprintf(out, "  intptr_t %s;\n", cap->as.expr_ident.name);
                }
            }
            fprintf(out, "} __cc_closure_env_%d;\n\n", id);
            
            /* Emit drop function */
            fprintf(out, "static void __cc_closure_env_%d_drop(void* p) { if (p) free(p); }\n\n", id);
        }
        
        /* Emit entry function prototype */
        switch (def->param_count) {
            case 0:
                fprintf(out, "static void* __cc_closure_entry_%d(void* __p);\n", id);
                break;
            case 1:
                fprintf(out, "static void* __cc_closure_entry_%d(void* __p, intptr_t __arg0);\n", id);
                break;
            default:
                fprintf(out, "static void* __cc_closure_entry_%d(void* __p, intptr_t __arg0, intptr_t __arg1);\n", id);
                break;
        }
        
        /* Emit make function */
        if (has_captures) {
            fprintf(out, "static %s __cc_closure_make_%d(", closure_type, id);
            /* Parameters for captured values - use intptr_t */
            for (int j = 0; j < def->captures.len; j++) {
                CCNNode* cap = def->captures.items[j];
                if (cap && cap->kind == CCN_EXPR_IDENT) {
                    if (j > 0) fprintf(out, ", ");
                    fprintf(out, "intptr_t _cap_%s", cap->as.expr_ident.name);
                }
            }
            fprintf(out, ") {\n");
            fprintf(out, "  __cc_closure_env_%d* __env = (__cc_closure_env_%d*)malloc(sizeof(__cc_closure_env_%d));\n", id, id, id);
            for (int j = 0; j < def->captures.len; j++) {
                CCNNode* cap = def->captures.items[j];
                if (cap && cap->kind == CCN_EXPR_IDENT) {
                    fprintf(out, "  __env->%s = _cap_%s;\n", cap->as.expr_ident.name, cap->as.expr_ident.name);
                }
            }
            fprintf(out, "  return %s(__cc_closure_entry_%d, __env, __cc_closure_env_%d_drop);\n", make_fn, id, id);
            fprintf(out, "}\n\n");
        } else {
            /* No captures - simpler make function */
            fprintf(out, "static %s __cc_closure_make_%d(void) {\n", closure_type, id);
            fprintf(out, "  return %s(__cc_closure_entry_%d, NULL, NULL);\n", make_fn, id);
            fprintf(out, "}\n\n");
        }
        
        /* Emit entry function definition */
        switch (def->param_count) {
            case 0:
                fprintf(out, "static void* __cc_closure_entry_%d(void* __p) {\n", id);
                break;
            case 1:
                fprintf(out, "static void* __cc_closure_entry_%d(void* __p, intptr_t __arg0) {\n", id);
                break;
            default:
                fprintf(out, "static void* __cc_closure_entry_%d(void* __p, intptr_t __arg0, intptr_t __arg1) {\n", id);
                break;
        }
        
        if (has_captures) {
            fprintf(out, "  __cc_closure_env_%d* __env = (__cc_closure_env_%d*)__p;\n", id, id);
            /* Define local aliases for captured values.
               We use #define to avoid type mismatch issues - the macro expands
               to the env access, preserving type semantics for user code. */
            for (int j = 0; j < def->captures.len; j++) {
                CCNNode* cap = def->captures.items[j];
                if (cap && cap->kind == CCN_EXPR_IDENT) {
                    fprintf(out, "  #define %s (__env->%s)\n", cap->as.expr_ident.name, cap->as.expr_ident.name);
                }
            }
        } else {
            fprintf(out, "  (void)__p;\n");  /* Suppress unused param warning */
        }
        
        /* Emit closure body with #line directive */
        if (def->body) {
            /* Point back to original closure location for debugging */
            if (def->body->span.start.file && def->body->span.start.line > 0) {
                fprintf(out, "#line %d \"%s\"\n", 
                        def->body->span.start.line, 
                        def->body->span.start.file);
            }
            
            /* Build param context for substitution */
            ClosureEmitCtx ctx = {0};
            if (def->param_count > 0) {
                const char** param_names = malloc(def->param_count * sizeof(char*));
                for (int j = 0; j < def->param_count && j < def->params.len; j++) {
                    CCNNode* p = def->params.items[j];
                    param_names[j] = (p && p->kind == CCN_PARAM) ? p->as.param.name : NULL;
                }
                ctx.param_names = param_names;
                ctx.param_count = def->param_count;
            }
            
            /* Emit as return statement if body is an expression */
            if (def->body->kind >= CCN_EXPR_IDENT && def->body->kind <= CCN_EXPR_TRY) {
                fprintf(out, "  return (void*)(intptr_t)(");
                emit_node_ctx(def->body, out, 1, &ctx);
                fprintf(out, ");\n");
            } else {
                emit_node_ctx(def->body, out, 1, &ctx);
                fprintf(out, "\n");
            }
            
            if (ctx.param_names) free((void*)ctx.param_names);
            
            /* If body was an expression, we already emitted return */
            if (def->body->kind >= CCN_EXPR_IDENT && def->body->kind <= CCN_EXPR_TRY) {
                fprintf(out, "}\n\n");
                continue;
            }
        }
        
        fprintf(out, "  return NULL;\n");
        fprintf(out, "}\n\n");
    }
    
    fprintf(out, "/* ===== End Closure Definitions ===== */\n\n");
}

/* Check if name matches a closure param and return index, or -1 */
static int find_param_index(const char* name, ClosureEmitCtx* ctx) {
    if (!ctx || !name) return -1;
    for (int i = 0; i < ctx->param_count; i++) {
        if (ctx->param_names[i] && strcmp(name, ctx->param_names[i]) == 0) {
            return i;
        }
    }
    return -1;
}

static void emit_node_ctx(const CCNNode* node, FILE* out, int indent, ClosureEmitCtx* ctx);

static void emit_node(const CCNNode* node, FILE* out, int indent) {
    emit_node_ctx(node, out, indent, NULL);
}

static void emit_node_ctx(const CCNNode* node, FILE* out, int indent, ClosureEmitCtx* ctx) {
    if (!node) return;

    switch (node->kind) {
        case CCN_FUNC_DECL:
            /* Skip internal stub functions from parse-time stubs and CC runtime */
            if (node->as.func.name) {
                const char* fn = node->as.func.name;
                if (strncmp(fn, "__cc_", 5) == 0 ||
                    strcmp(fn, "cc_try") == 0 ||
                    strcmp(fn, "cc_some") == 0 ||
                    strcmp(fn, "cc_none") == 0 ||
                    strcmp(fn, "cc_ok") == 0 ||
                    strcmp(fn, "cc_err") == 0 ||
                    strcmp(fn, "cc_error") == 0 ||
                    strncmp(fn, "__CC", 4) == 0) {
                    break;
                }
            }
            fprintf(out, "\n");
            emit_line_directive(node, out);
            if (node->as.func.is_async) fprintf(out, "/* @async */ ");
            /* Emit return type - handle TCC's type representation */
            if (node->as.func.return_type && node->as.func.return_type->kind == CCN_TYPE_NAME) {
                const char* ret = node->as.func.return_type->as.type_name.name;
                /* TCC shows generic stubs as "struct <anonymous>" - use auto for now */
                if (ret && strstr(ret, "<anonymous>")) {
                    fprintf(out, "auto ");
                } else if (ret && strncmp(ret, "struct __CC", 11) == 0) {
                    /* Strip "struct " from __CC* types - they're typedef'd */
                    fprintf(out, "%s ", ret + 7);
                } else {
                    fprintf(out, "%s ", ret);
                }
            } else {
                fprintf(out, "void ");
            }
            fprintf(out, "%s(", node->as.func.name);
            for (int i = 0; i < node->as.func.params.len; i++) {
                emit_node_ctx(node->as.func.params.items[i], out, 0, ctx);
                if (i < node->as.func.params.len - 1) fprintf(out, ", ");
            }
            fprintf(out, ") ");
            emit_node_ctx(node->as.func.body, out, indent, ctx);
            fprintf(out, "\n");
            break;

        case CCN_BLOCK:
            fprintf(out, "{\n");
            for (int i = 0; i < node->as.block.stmts.len; i++) {
                CCNNode* stmt = node->as.block.stmts.items[i];
                if (!stmt) continue;
                /* Skip implicit forward declarations (function types inside blocks) */
                if (stmt->kind == CCN_VAR_DECL && stmt->as.var.type_node &&
                    stmt->as.var.type_node->kind == CCN_TYPE_NAME &&
                    stmt->as.var.type_node->as.type_name.name &&
                    strstr(stmt->as.var.type_node->as.type_name.name, "(") != NULL) {
                    continue;
                }
                /* Skip empty STMT_EXPR (just wraps NULL or empty block) */
                if (stmt->kind == CCN_STMT_EXPR) {
                    CCNNode* expr = stmt->as.stmt_expr.expr;
                    if (!expr) continue;
                    if (expr->kind == CCN_BLOCK && expr->as.block.stmts.len == 0) continue;
                }
                emit_indent(indent + 1, out);
                emit_node_ctx(stmt, out, indent + 1, ctx);
                fprintf(out, ";\n");
            }
            emit_indent(indent, out);
            fprintf(out, "}");
            break;

        case CCN_EXPR_CALL:
            emit_node_ctx(node->as.expr_call.callee, out, 0, ctx);
            fprintf(out, "(");
            for (int i = 0; i < node->as.expr_call.args.len; i++) {
                emit_node_ctx(node->as.expr_call.args.items[i], out, 0, ctx);
                if (i < node->as.expr_call.args.len - 1) fprintf(out, ", ");
            }
            fprintf(out, ")");
            break;

        case CCN_EXPR_METHOD:
            /* After UFCS lowering, this becomes a regular call */
            fprintf(out, "%s(", node->as.expr_method.method);
            emit_node_ctx(node->as.expr_method.receiver, out, 0, ctx);
            if (node->as.expr_method.args.len > 0) fprintf(out, ", ");
            for (int i = 0; i < node->as.expr_method.args.len; i++) {
                emit_node_ctx(node->as.expr_method.args.items[i], out, 0, ctx);
                if (i < node->as.expr_method.args.len - 1) fprintf(out, ", ");
            }
            fprintf(out, ")");
            break;

        case CCN_EXPR_IDENT: {
            /* Check for closure param substitution */
            int param_idx = find_param_index(node->as.expr_ident.name, ctx);
            if (param_idx >= 0) {
                /* Don't cast - let user code handle type conversion.
                   __argN is intptr_t which can hold both ints and pointers. */
                fprintf(out, "__arg%d", param_idx);
            } else {
                fprintf(out, "%s", node->as.expr_ident.name);
            }
            break;
        }

        case CCN_EXPR_LITERAL_INT:
            fprintf(out, "%lld", (long long)node->as.expr_int.value);
            break;

        case CCN_EXPR_LITERAL_STRING: {
            /* Emit string with proper escaping */
            fprintf(out, "\"");
            const char* s = node->as.expr_string.value;
            if (s) {
                for (; *s; s++) {
                    switch (*s) {
                        case '\n': fprintf(out, "\\n"); break;
                        case '\r': fprintf(out, "\\r"); break;
                        case '\t': fprintf(out, "\\t"); break;
                        case '\\': fprintf(out, "\\\\"); break;
                        case '"':  fprintf(out, "\\\""); break;
                        default:
                            if (*s >= 32 && *s < 127) {
                                fputc(*s, out);
                            } else {
                                fprintf(out, "\\x%02x", (unsigned char)*s);
                            }
                    }
                }
            }
            fprintf(out, "\"");
            break;
        }

        case CCN_PARAM:
            /* Emit parameter with type */
            if (node->as.param.type_node && node->as.param.type_node->kind == CCN_TYPE_NAME) {
                fprintf(out, "%s %s", 
                        node->as.param.type_node->as.type_name.name,
                        node->as.param.name ? node->as.param.name : "");
            } else {
                fprintf(out, "/* untyped */ %s", node->as.param.name ? node->as.param.name : "?");
            }
            break;
            
        case CCN_VAR_DECL: {
            /* Check if init is a closure make call */
            const char* closure_type = NULL;
            if (node->as.var.init && node->as.var.init->kind == CCN_EXPR_CALL &&
                node->as.var.init->as.expr_call.callee &&
                node->as.var.init->as.expr_call.callee->kind == CCN_EXPR_IDENT) {
                const char* callee = node->as.var.init->as.expr_call.callee->as.expr_ident.name;
                if (callee && strncmp(callee, "__cc_closure_make_", 18) == 0) {
                    /* Extract closure ID and look up param count from g_current_file */
                    int id = atoi(callee + 18);
                    int param_count = 1;  /* Default */
                    if (g_current_file && g_current_file->closure_defs) {
                        for (int i = 0; i < g_current_file->closure_count; i++) {
                            if (g_current_file->closure_defs[i].id == id) {
                                param_count = g_current_file->closure_defs[i].param_count;
                                break;
                            }
                        }
                    }
                    switch (param_count) {
                        case 0: closure_type = "CCClosure0"; break;
                        case 1: closure_type = "CCClosure1"; break;
                        default: closure_type = "CCClosure2"; break;
                    }
                }
            }
            
            /* Emit type */
            if (closure_type) {
                fprintf(out, "%s %s", closure_type, node->as.var.name);
            } else if (node->as.var.type_node && node->as.var.type_node->kind == CCN_TYPE_NAME &&
                node->as.var.type_node->as.type_name.name) {
                const char* type_str = node->as.var.type_node->as.type_name.name;
                /* Strip "struct " from __CC* types - they're typedef'd */
                const char* emit_type = type_str;
                if (strncmp(type_str, "struct __CC", 11) == 0) {
                    emit_type = type_str + 7;  /* Skip "struct " */
                }
                /* Check for array type (contains '[') */
                const char* bracket = strchr(emit_type, '[');
                if (bracket) {
                    /* Array type: emit "basetype name[dim]" */
                    size_t base_len = bracket - emit_type;
                    /* Remove trailing space from base type */
                    while (base_len > 0 && emit_type[base_len - 1] == ' ') base_len--;
                    fprintf(out, "%.*s %s%s", (int)base_len, emit_type, node->as.var.name, bracket);
                } else {
                    fprintf(out, "%s %s", emit_type, node->as.var.name);
                }
            } else {
                fprintf(out, "auto %s", node->as.var.name);
            }
            if (node->as.var.init) {
                fprintf(out, " = ");
                emit_node_ctx(node->as.var.init, out, 0, ctx);
            }
            break;
        }

        case CCN_STMT_RETURN:
            fprintf(out, "return");
            if (node->as.stmt_return.value) {
                fprintf(out, " ");
                emit_node_ctx(node->as.stmt_return.value, out, 0, ctx);
            }
            break;

        case CCN_STMT_IF:
            fprintf(out, "if (");
            emit_node_ctx(node->as.stmt_if.cond, out, 0, ctx);
            fprintf(out, ") ");
            emit_node_ctx(node->as.stmt_if.then_branch, out, indent, ctx);
            if (node->as.stmt_if.else_branch) {
                fprintf(out, " else ");
                emit_node_ctx(node->as.stmt_if.else_branch, out, indent, ctx);
            }
            break;

        case CCN_STMT_FOR:
            fprintf(out, "for (");
            if (node->as.stmt_for.init) {
                emit_node_ctx(node->as.stmt_for.init, out, 0, ctx);
            }
            fprintf(out, "; ");
            if (node->as.stmt_for.cond) {
                emit_node_ctx(node->as.stmt_for.cond, out, 0, ctx);
            }
            fprintf(out, "; ");
            if (node->as.stmt_for.incr) {
                emit_node_ctx(node->as.stmt_for.incr, out, 0, ctx);
            }
            fprintf(out, ") ");
            emit_node_ctx(node->as.stmt_for.body, out, indent, ctx);
            break;

        case CCN_STMT_WHILE:
            fprintf(out, "while (");
            emit_node_ctx(node->as.stmt_while.cond, out, 0, ctx);
            fprintf(out, ") ");
            emit_node_ctx(node->as.stmt_while.body, out, indent, ctx);
            break;

        case CCN_STMT_EXPR:
            emit_node_ctx(node->as.stmt_expr.expr, out, indent, ctx);
            break;

        case CCN_STMT_NURSERY:
            /* Lower @nursery { body } to:
             *   { CCNursery __ccn_nursery; cc_nursery_open(&__ccn_nursery, NULL);
             *     body;
             *     cc_nursery_close(&__ccn_nursery); }
             */
            emit_line_directive(node, out);
            fprintf(out, "{\n");
            emit_indent(indent + 1, out);
            fprintf(out, "CCNursery __ccn_nursery;\n");
            emit_indent(indent + 1, out);
            fprintf(out, "cc_nursery_open(&__ccn_nursery, NULL);\n");
            if (node->as.stmt_scope.body) {
                /* Emit body statements directly (unwrap the block) */
                CCNNode* body = node->as.stmt_scope.body;
                if (body->kind == CCN_BLOCK) {
                    for (int i = 0; i < body->as.block.stmts.len; i++) {
                        emit_indent(indent + 1, out);
                        emit_node_ctx(body->as.block.stmts.items[i], out, indent + 1, ctx);
                        fprintf(out, ";\n");
                    }
                } else {
                    emit_indent(indent + 1, out);
                    emit_node_ctx(body, out, indent + 1, ctx);
                    fprintf(out, ";\n");
                }
            }
            emit_indent(indent + 1, out);
            fprintf(out, "cc_nursery_close(&__ccn_nursery);\n");
            emit_indent(indent, out);
            fprintf(out, "}");
            break;

        case CCN_STMT_SPAWN:
            /* Lower spawn(closure) to cc_nursery_spawn_closure0(&__ccn_nursery, closure) */
            fprintf(out, "cc_nursery_spawn_closure0(&__ccn_nursery, ");
            emit_node_ctx(node->as.stmt_spawn.closure, out, 0, ctx);
            fprintf(out, ")");
            break;

        case CCN_EXPR_BINARY: {
            static const char* op_strs[] = {
                "+", "-", "*", "/", "%",
                "&", "|", "^", "<<", ">>",
                "&&", "||",
                "==", "!=", "<", "<=", ">", ">=",
                "=", "+=", "-=", "*=", "/=", "%=",
                ","
            };
            int op = node->as.expr_binary.op;
            const char* op_str = (op >= 0 && op < (int)(sizeof(op_strs)/sizeof(op_strs[0]))) 
                                 ? op_strs[op] : "?";
            fprintf(out, "(");
            emit_node_ctx(node->as.expr_binary.lhs, out, 0, ctx);
            fprintf(out, " %s ", op_str);
            emit_node_ctx(node->as.expr_binary.rhs, out, 0, ctx);
            fprintf(out, ")");
            break;
        }

        case CCN_EXPR_UNARY: {
            /* Convert unary op enum to string */
            const char* op_str = "?";
            int is_postfix = 0;
            switch (node->as.expr_unary.op) {
                case CCN_OP_POST_INC: op_str = "++"; is_postfix = 1; break;
                case CCN_OP_POST_DEC: op_str = "--"; is_postfix = 1; break;
                case CCN_OP_PRE_INC:  op_str = "++"; break;
                case CCN_OP_PRE_DEC:  op_str = "--"; break;
                case CCN_OP_NOT:     op_str = "!"; break;
                case CCN_OP_BNOT:    op_str = "~"; break;
                case CCN_OP_NEG:     op_str = "-"; break;
                case CCN_OP_ADDR:    op_str = "&"; break;
                case CCN_OP_DEREF:   op_str = "*"; break;
            }
            if (is_postfix) {
                emit_node_ctx(node->as.expr_unary.operand, out, 0, ctx);
                fprintf(out, "%s", op_str);
            } else {
                fprintf(out, "%s", op_str);
                emit_node_ctx(node->as.expr_unary.operand, out, 0, ctx);
            }
            break;
        }

        case CCN_EXPR_AWAIT:
            /* For now, just emit the expression (async lowering will transform this later) */
            fprintf(out, "/* await */ ");
            emit_node_ctx(node->as.expr_await.expr, out, 0, ctx);
            break;

        case CCN_EXPR_TRY:
            /* try expr -> cc_try(expr) - unwrap Result or propagate error */
            fprintf(out, "cc_try(");
            if (node->as.expr_try.expr) {
                emit_node_ctx(node->as.expr_try.expr, out, 0, ctx);
            }
            fprintf(out, ")");
            break;

        case CCN_EXPR_FIELD:
            /* Field access: object.field or object->field */
            emit_node_ctx(node->as.expr_field.object, out, 0, ctx);
            if (node->as.expr_field.is_arrow) {
                fprintf(out, "->");
            } else {
                fprintf(out, ".");
            }
            fprintf(out, "%s", node->as.expr_field.field ? node->as.expr_field.field : "???");
            break;

        case CCN_EXPR_INDEX:
            /* Array index: array[index] */
            emit_node_ctx(node->as.expr_index.array, out, 0, ctx);
            fprintf(out, "[");
            emit_node_ctx(node->as.expr_index.index, out, 0, ctx);
            fprintf(out, "]");
            break;

        case CCN_EXPR_COMPOUND:
            /* Compound literal: {val1, val2, ...} */
            fprintf(out, "{");
            for (int i = 0; i < node->as.expr_compound.values.len; i++) {
                if (i > 0) fprintf(out, ", ");
                emit_node_ctx(node->as.expr_compound.values.items[i], out, 0, ctx);
            }
            fprintf(out, "}");
            break;

        case CCN_EXPR_SIZEOF:
            /* sizeof(type) or sizeof(expr) */
            if (node->as.expr_sizeof.type_str) {
                fprintf(out, "sizeof(%s)", node->as.expr_sizeof.type_str);
            } else if (node->as.expr_sizeof.expr) {
                fprintf(out, "sizeof(");
                emit_node_ctx(node->as.expr_sizeof.expr, out, 0, ctx);
                fprintf(out, ")");
            } else {
                fprintf(out, "sizeof(/* ??? */)");
            }
            break;

        case CCN_ENUM_DECL:
            /* enum definition */
            /* Skip anonymous enums (generated by TCC) */
            if (node->as.enum_decl.name && strchr(node->as.enum_decl.name, '.')) {
                break;  /* Skip internal names */
            }
            emit_line_directive(node, out);
            fprintf(out, "enum");
            if (node->as.enum_decl.name) {
                fprintf(out, " %s", node->as.enum_decl.name);
            }
            fprintf(out, " {\n");
            for (int i = 0; i < node->as.enum_decl.values.len; i++) {
                CCNNode* val = node->as.enum_decl.values.items[i];
                if (val && val->kind == CCN_ENUM_VALUE) {
                    emit_indent(indent + 1, out);
                    fprintf(out, "%s = %d", 
                            val->as.enum_value.name ? val->as.enum_value.name : "???",
                            val->as.enum_value.value);
                    if (i < node->as.enum_decl.values.len - 1) {
                        fprintf(out, ",");
                    }
                    fprintf(out, "\n");
                }
            }
            emit_indent(indent, out);
            fprintf(out, "};\n");
            break;

        case CCN_STRUCT_DECL:
            /* struct/union definition */
            /* Skip anonymous structs (generated by TCC for internal use) */
            if (node->as.struct_decl.name && strchr(node->as.struct_decl.name, '.')) {
                break;  /* Skip L.1, L.2 etc anonymous structs */
            }
            /* Skip parse stub structs (defined in real headers) */
            if (node->as.struct_decl.name && 
                (strncmp(node->as.struct_decl.name, "__CC", 4) == 0 ||
                 strcmp(node->as.struct_decl.name, "CCChan") == 0)) {
                break;
            }
            emit_line_directive(node, out);
            if (node->as.struct_decl.is_union) {
                fprintf(out, "union");
            } else {
                fprintf(out, "struct");
            }
            if (node->as.struct_decl.name) {
                fprintf(out, " %s", node->as.struct_decl.name);
            }
            fprintf(out, " {\n");
            for (int i = 0; i < node->as.struct_decl.fields.len; i++) {
                CCNNode* field = node->as.struct_decl.fields.items[i];
                if (field && field->kind == CCN_STRUCT_FIELD) {
                    emit_indent(indent + 1, out);
                    fprintf(out, "%s %s;\n", 
                            field->as.struct_field.type_str ? field->as.struct_field.type_str : "int",
                            field->as.struct_field.name ? field->as.struct_field.name : "???");
                }
            }
            emit_indent(indent, out);
            fprintf(out, "};\n");
            break;

        case CCN_TYPEDEF:
            /* typedef declaration */
            /* Skip parse stub typedefs - either with <anonymous> type or __CC prefix */
            if (node->as.typedef_decl.type_str &&
                strstr(node->as.typedef_decl.type_str, "<anonymous>")) {
                break;  /* Skip anonymous struct typedefs */
            }
            if (node->as.typedef_decl.name &&
                strncmp(node->as.typedef_decl.name, "__CC", 4) == 0) {
                break;  /* Skip __CC* internal types */
            }
            /* Skip typedef of __CC* types (parse stubs) */
            if (node->as.typedef_decl.type_str &&
                strstr(node->as.typedef_decl.type_str, "__CC")) {
                break;
            }
            /* Skip forward declarations of parse stub types */
            if (node->as.typedef_decl.name &&
                (strncmp(node->as.typedef_decl.name, "Vec_", 4) == 0 ||
                 strncmp(node->as.typedef_decl.name, "Map_", 4) == 0 ||
                 strncmp(node->as.typedef_decl.name, "CCChan", 6) == 0 ||
                 strncmp(node->as.typedef_decl.name, "CCOptional_", 11) == 0 ||
                 strncmp(node->as.typedef_decl.name, "CCResult_", 9) == 0)) {
                break;
            }
            emit_line_directive(node, out);
            if (node->as.typedef_decl.type_str && node->as.typedef_decl.name) {
                /* For anonymous struct typedefs, we need the struct body.
                 * For now, emit the type as-is (may need struct definition elsewhere). */
                fprintf(out, "typedef %s %s;\n", 
                        node->as.typedef_decl.type_str,
                        node->as.typedef_decl.name);
            }
            break;

        case CCN_INCLUDE: {
            const char* path = node->as.include.path;
            if (path) {
                /* Skip CC runtime includes - we emit cc_runtime.cch at the top */
                if (strstr(path, "ccc/cc_") || strstr(path, "ccc/std/")) {
                    break;  /* Already included via cc_runtime.cch */
                }
                if (node->as.include.is_system) {
                    fprintf(out, "#include <%s>\n", path);
                } else {
                    fprintf(out, "#include \"%s\"\n", path);
                }
            }
            break;
        }

        default:
            fprintf(out, "/* TODO: node kind %d */", node->kind);
            break;
    }
}

int cc_emit_c(const CCNFile* file, FILE* out) {
    if (!file || !file->root || !out) return -1;

    /* Reset emit state */
    g_emit_state.file = NULL;
    g_emit_state.line = 0;
    g_current_file = file;  /* For closure type lookup */

    fprintf(out, "/* Generated by cccn (Concurrent-C Compiler) */\n");
    fprintf(out, "#define CCC_VERSION 4\n\n");
    /* Note: CC_PARSER_MODE is intentionally NOT defined - we want real runtime types */
    
    /* Check if CC runtime is needed (nursery, spawn, channels, etc.) */
    int has_nursery = has_nursery_stmt(file->root);
    int needs_runtime = has_nursery || file->closure_count > 0;
    
    if (needs_runtime) {
        fprintf(out, "#include <ccc/cc_runtime.cch>\n\n");
    }
    
    /* Include stdlib for malloc/free (used by closure env allocation) */
    if (file->closure_count > 0) {
        fprintf(out, "#include <stdlib.h>\n");
        fprintf(out, "#include <stdint.h>\n\n");
        /* Note: Closure types (CCClosure0/1/2, cc_closureN_make/call) are provided
           by cc_closure.cch which is included via cc_runtime.cch above. */
    }

    /* Emit generated closure definitions first (in generated section) */
    if (file->closure_count > 0) {
        emit_generated_section(out, "closures");
    }
    emit_closure_defs(file, out);
    
    /* Switch back to source file */
    if (file->filename) {
        fprintf(out, "#line 1 \"%s\"\n", file->filename);
        g_emit_state.file = file->filename;
        g_emit_state.line = 1;
    }

    CCNNode* root = file->root;
    if (root->kind == CCN_FILE) {
        for (int i = 0; i < root->as.file.items.len; i++) {
            CCNNode* item = root->as.file.items.items[i];
            if (!item) continue;
            
            /* Skip function-type declarations (forward decls from TCC) */
            if (item->kind == CCN_VAR_DECL && item->as.var.type_node &&
                item->as.var.type_node->kind == CCN_TYPE_NAME &&
                item->as.var.type_node->as.type_name.name &&
                strchr(item->as.var.type_node->as.type_name.name, '(') != NULL) {
                continue;
            }
            
            /* Skip parse stub type declarations (Vec_*, Map_*, CC*, __CC*) */
            if (item->kind == CCN_VAR_DECL && item->as.var.name &&
                (strncmp(item->as.var.name, "Vec_", 4) == 0 ||
                 strncmp(item->as.var.name, "Map_", 4) == 0 ||
                 strncmp(item->as.var.name, "CCChan", 6) == 0 ||
                 strncmp(item->as.var.name, "CCOptional_", 11) == 0 ||
                 strncmp(item->as.var.name, "CCResult_", 9) == 0 ||
                 strncmp(item->as.var.name, "__CC", 4) == 0)) {
                continue;
            }
            
            emit_node(item, out, 0);
            /* Add semicolon and newline for declarations that need it */
            if (item->kind == CCN_VAR_DECL) {
                fprintf(out, ";\n");
            }
        }
    }

    return 0;
}

/* Generate include guard name from filename */
static void make_guard_name(const char* filename, char* guard, size_t guard_size) {
    if (!filename || !guard || guard_size < 2) return;
    
    /* Extract basename */
    const char* base = strrchr(filename, '/');
    base = base ? base + 1 : filename;
    
    /* Convert to uppercase, replace non-alnum with underscore */
    size_t i = 0;
    for (const char* p = base; *p && i < guard_size - 1; p++) {
        if ((*p >= 'a' && *p <= 'z')) {
            guard[i++] = *p - 'a' + 'A';
        } else if ((*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9')) {
            guard[i++] = *p;
        } else {
            guard[i++] = '_';
        }
    }
    guard[i] = '\0';
}

int cc_emit_h(const CCNFile* file, FILE* out, const char* guard_name) {
    if (!file || !file->root || !out) return -1;

    /* Reset emit state */
    g_emit_state.file = NULL;
    g_emit_state.line = 0;

    /* Generate guard name if not provided */
    char generated_guard[256];
    if (!guard_name && file->filename) {
        make_guard_name(file->filename, generated_guard, sizeof(generated_guard));
        guard_name = generated_guard;
    }
    if (!guard_name) guard_name = "CC_HEADER_H";

    /* Emit header with #pragma once (preferred) and traditional guards (fallback) */
    fprintf(out, "/* Generated by cccn */\n");
    fprintf(out, "#pragma once\n");
    fprintf(out, "#ifndef %s\n", guard_name);
    fprintf(out, "#define %s\n\n", guard_name);
    fprintf(out, "#include <ccc/cc_runtime.cch>\n\n");

    /* Emit generated closure definitions if any */
    if (file->closure_count > 0) {
        emit_generated_section(out, "closures");
        emit_closure_defs(file, out);
    }
    
    /* Switch back to source file */
    if (file->filename) {
        fprintf(out, "#line 1 \"%s\"\n", file->filename);
        g_emit_state.file = file->filename;
        g_emit_state.line = 1;
    }

    /* Emit declarations */
    CCNNode* root = file->root;
    if (root->kind == CCN_FILE) {
        for (int i = 0; i < root->as.file.items.len; i++) {
            emit_node(root->as.file.items.items[i], out, 0);
        }
    }

    /* Close guard */
    fprintf(out, "\n#endif /* %s */\n", guard_name);

    return 0;
}
