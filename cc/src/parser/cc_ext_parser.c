#include "tcc_bridge.h"

#ifdef CC_TCC_EXT_AVAILABLE
#include <tcc.h>
#include <string.h>

/* Access TCC internals */
extern int tok;
extern int tok_col;
extern CValue tokc;
extern struct BufferedFile *file;
extern int nocode_wanted;

/* Use raw error function to avoid macro issues */
#undef tcc_error
#define tcc_error(fmt, ...) do { tcc_enter_state(tcc_state); _tcc_error_noabort(fmt, ##__VA_ARGS__); } while(0)

/* AST recording functions (defined in tccgen.c) */
extern void cc_ast_record_start(int kind);
extern void cc_ast_record_end(void);

/* TCC parsing functions */
extern void next(void);
extern void skip(int c);
extern const char *get_tok_str(int v, CValue *cv);
extern int tok_alloc_const(const char *str);
extern char *tcc_strdup(const char *str);
extern void *tcc_mallocz(unsigned long size);
extern void tcc_free(void *ptr);
extern void cstr_new(CString *cstr);
extern void cstr_free(CString *cstr);
extern void cstr_cat(CString *cstr, const char *str, int len);
extern void cstr_ccat(CString *cstr, int ch);
extern void expr_eq(void);
extern void vpop(void);
extern void vpushi(int v);
extern void block(int flags);
extern void unget_tok(int last_tok);

/* Forward declarations */
static int cc_try_cc_decl(void);
static int cc_try_cc_stmt(void);
static int cc_try_cc_at_stmt(void);
static int cc_try_cc_unary(void);
static int cc_try_thread_spawn(void);
static int cc_try_cc_closure(void);
static int cc_try_cc_closure_single_param(int ident_tok, int start_line, int start_col);

const struct TCCExtParser cc_ext_parser = {
    .try_cc_decl = cc_try_cc_decl,
    .try_cc_stmt = cc_try_cc_stmt,
    .try_cc_at_stmt = cc_try_cc_at_stmt,
    .try_cc_unary = cc_try_cc_unary,
    .try_cc_spawn = cc_try_thread_spawn,
    .try_cc_closure = cc_try_cc_closure,
    .try_cc_closure_single_param = cc_try_cc_closure_single_param,
};

static int cc_try_cc_decl(void) { return 0; }
static int cc_try_cc_stmt(void) { return 0; }
static int cc_try_cc_unary(void) { return 0; }

/* Helper: parse optional captures [x, y, ...] after => and closure body.
 * New syntax: () => [captures] { body }  or  () => [captures] expr
 * Captures are now parsed AFTER the arrow, not before the params.
 */
/* Skip closure body by counting braces/parens, for closures with typed params
 * where TCC can't type-check without knowing the param types. */
static void cc_skip_closure_body(int is_block) {
    if (is_block) {
        /* Skip { ... } by matching braces */
        int depth = 1;
        next(); /* consume '{' */
        while (tok != TOK_EOF && depth > 0) {
            if (tok == '{') depth++;
            else if (tok == '}') depth--;
            if (depth > 0) next();
        }
        if (tok == '}') next(); /* consume final '}' */
    } else {
        /* Skip expression: stop at , or ) or ; at depth 0 */
        int paren = 0, brace = 0, bracket = 0;
        while (tok != TOK_EOF) {
            if (tok == '(') paren++;
            else if (tok == ')') { if (paren == 0) break; paren--; }
            else if (tok == '{') brace++;
            else if (tok == '}') { if (brace == 0) break; brace--; }
            else if (tok == '[') bracket++;
            else if (tok == ']') { if (bracket == 0) break; bracket--; }
            else if (tok == ',' && paren == 0 && brace == 0 && bracket == 0) break;
            else if (tok == ';' && paren == 0 && brace == 0 && bracket == 0) break;
            next();
        }
    }
}

static void cc_parse_closure_body_ex(int has_typed_params) {
    int saved_ncw = nocode_wanted;
    int saved_last_member_tok = 0;
    int saved_last_member_flags = 0;
    int saved_last_member_line = 0;
    int saved_last_member_col = 0;
    int saved_ufcs_active = 0;
    int saved_ufcs_seq_line = 0;
    int saved_ufcs_seq_count = 0;
    char saved_last_recv_type[128] = {0};

    if (tcc_state) {
        saved_last_member_tok = tcc_state->cc_last_member_tok;
        saved_last_member_flags = tcc_state->cc_last_member_flags;
        saved_last_member_line = tcc_state->cc_last_member_line;
        saved_last_member_col = tcc_state->cc_last_member_col;
        saved_ufcs_active = tcc_state->cc_ufcs_active;
        saved_ufcs_seq_line = tcc_state->cc_ufcs_seq_line;
        saved_ufcs_seq_count = tcc_state->cc_ufcs_seq_count;
        memcpy(saved_last_recv_type, tcc_state->cc_last_recv_type, sizeof(saved_last_recv_type));

        /* UFCS state is expression-scoped. A closure body parsed as an argument must
           not inherit the surrounding call's receiver, and inner UFCS must not leak
           back out to the enclosing call after the closure literal finishes. */
        tcc_state->cc_last_member_tok = 0;
        tcc_state->cc_last_member_flags = 0;
        tcc_state->cc_last_member_line = 0;
        tcc_state->cc_last_member_col = 0;
        tcc_state->cc_last_recv_type[0] = '\0';
        tcc_state->cc_ufcs_active = 0;
        tcc_state->cc_ufcs_seq_line = 0;
        tcc_state->cc_ufcs_seq_count = 0;
    }

    ++nocode_wanted;
    if (tcc_state) tcc_state->cc_in_closure_body++;
    
    /* NEW: Parse optional captures [x, y, ...] after => */
    if (tok == '[') {
        /* Mark that this closure has captures (aux2 bit 3) */
        if (tcc_state && tcc_state->cc_nodes && tcc_state->cc_node_stack_top >= 0) {
            int idx = tcc_state->cc_node_stack[tcc_state->cc_node_stack_top];
            tcc_state->cc_nodes[idx].aux2 |= 8;  /* bit 3: has captures after arrow */
        }
        next(); /* consume '[' */
        int sq = 1;
        while (tok != TOK_EOF && sq > 0) {
            if (tok == '[') sq++;
            else if (tok == ']') sq--;
            if (sq > 0) next();
        }
        if (tok == ']') {
            next(); /* consume ']' */
        }
    }
    
    int is_block = (tok == '{');
    
    if (has_typed_params) {
        /* Skip body without type-checking - TCC doesn't know param types */
        cc_skip_closure_body(is_block);
        if (tcc_state && tcc_state->cc_nodes && tcc_state->cc_node_stack_top >= 0) {
            int idx = tcc_state->cc_node_stack[tcc_state->cc_node_stack_top];
            tcc_state->cc_nodes[idx].aux2 |= (is_block ? 1 : 2);
        }
    } else if (is_block) {
        block(0);
        if (tcc_state && tcc_state->cc_nodes && tcc_state->cc_node_stack_top >= 0) {
            int idx = tcc_state->cc_node_stack[tcc_state->cc_node_stack_top];
            tcc_state->cc_nodes[idx].aux2 |= 1; /* body is block */
        }
    } else {
        expr_eq();
        vpop();
        if (tcc_state && tcc_state->cc_nodes && tcc_state->cc_node_stack_top >= 0) {
            int idx = tcc_state->cc_node_stack[tcc_state->cc_node_stack_top];
            tcc_state->cc_nodes[idx].aux2 |= 2; /* body is expr */
        }
    }
    
    if (tcc_state) tcc_state->cc_in_closure_body--;
    nocode_wanted = saved_ncw;
    if (tcc_state) {
        tcc_state->cc_last_member_tok = saved_last_member_tok;
        tcc_state->cc_last_member_flags = saved_last_member_flags;
        tcc_state->cc_last_member_line = saved_last_member_line;
        tcc_state->cc_last_member_col = saved_last_member_col;
        memcpy(tcc_state->cc_last_recv_type, saved_last_recv_type, sizeof(saved_last_recv_type));
        tcc_state->cc_ufcs_active = saved_ufcs_active;
        tcc_state->cc_ufcs_seq_line = saved_ufcs_seq_line;
        tcc_state->cc_ufcs_seq_count = saved_ufcs_seq_count;
    }
}

/* Backward-compatible wrapper for closures without typed params */
static void cc_parse_closure_body(void) {
    cc_parse_closure_body_ex(0);
    
    /* Pin end span */
    if (tcc_state && file && tcc_state->cc_nodes && tcc_state->cc_node_stack_top >= 0) {
        int idx = tcc_state->cc_node_stack[tcc_state->cc_node_stack_top];
        tcc_state->cc_nodes[idx].line_end = file->line_num;
        tcc_state->cc_nodes[idx].col_end = tok_col;
        tcc_state->cc_nodes[idx].aux2 |= (int)(1U << 31);
    }
}

/*
 * Parse closure literal. Handles:
 *   - () => body                          (when tok is ')')
 *   - () => [captures] body               (captures parsed by cc_parse_closure_body)
 *   - (x) => body, (x, y) => body         (when tok is identifier)
 *   - (int x) => body                     (when tok is type keyword)
 *   - @unsafe () => [captures] body       (when tok is '@')
 * 
 * NEW SYNTAX (v3): Captures come AFTER the arrow:
 *   - () => [x, y] { body }               (explicit captures)
 *   - (a) => [x] expr                     (one param with capture)
 *   - @unsafe () => [&x] { body }         (@unsafe allows mutation of ref captures)
 * 
 * Called after '(' has been consumed (tok is first token inside parens),
 * OR when tok is '@' for @unsafe closures.
 * 
 * Returns: 1 = closure parsed, 0 = not a closure
 */
static int cc_try_cc_closure(void) {
    /* Use saved position from before '(' was consumed, or current position for '@' */
    int start_line = (tok == '@') ? (file ? file->line_num : 0) : tcc_state->cc_paren_start_line;
    int start_col = (tok == '@') ? tok_col : tcc_state->cc_paren_start_col;
    
    /* Handle @unsafe prefix: @unsafe () => [captures] body */
    if (tok == '@') {
        next(); /* consume '@' */
        const char* kw = get_tok_str(tok, NULL);
        if (!kw || strcmp(kw, "unsafe") != 0) {
            tcc_error("unexpected '@' in expression (expected '@unsafe')");
            return 0;
        }
        next(); /* consume 'unsafe' */
        
        /* Now expect '(' for the closure params */
        if (tok != '(') {
            tcc_error("expected '(' after '@unsafe' in closure");
            return 0;
        }
        next(); /* consume '(' */
        
        /* Parse the closure params (empty or with params) */
        int cap_param_count = 0;
        int par = 1;
        int saw_content = 0;
        while (tok != TOK_EOF && par > 0) {
            if (tok == '(') par++;
            else if (tok == ')') { par--; if (par > 0) saw_content = 1; }
            else if (tok == ',' && par == 1) { cap_param_count++; saw_content = 0; }
            else saw_content = 1;
            if (par > 0) next();
        }
        if (saw_content) cap_param_count++;
        if (tok != ')') {
            tcc_error("unmatched '(' in closure parameter list");
            return 0;
        }
        next(); /* consume ')' */
        
        if (tok != TOK_CC_ARROW) {
            tcc_error("expected '=>' after closure parameters");
            return 0;
        }
        
        cc_ast_record_start(CC_AST_NODE_CLOSURE);
        if (tcc_state && tcc_state->cc_nodes && tcc_state->cc_node_stack_top >= 0) {
            int idx = tcc_state->cc_node_stack[tcc_state->cc_node_stack_top];
            tcc_state->cc_nodes[idx].line_start = start_line;
            tcc_state->cc_nodes[idx].col_start = start_col;
            tcc_state->cc_nodes[idx].aux1 = cap_param_count;
            tcc_state->cc_nodes[idx].aux_s1 = tcc_strdup("closure_unsafe");
        }
        next(); /* consume '=>' */
        
        cc_parse_closure_body();
        cc_ast_record_end();
        vpushi(0);
        return 1;
    }
    
    /* OLD SYNTAX DEPRECATION: [captures]() => body is no longer supported */
    if (tok == '[') {
        tcc_error("closure syntax changed: use '() => [captures] body' instead of '[captures]() => body'");
        return 0;
    }
    
    /* Case 2: () => body (empty params, tok is ')' after '(' was consumed) */
    if (tok == ')') {
        next(); /* consume ')' */
        if (tok == TOK_CC_ARROW) {
            cc_ast_record_start(CC_AST_NODE_CLOSURE);
            if (tcc_state && tcc_state->cc_nodes && tcc_state->cc_node_stack_top >= 0) {
                int idx = tcc_state->cc_node_stack[tcc_state->cc_node_stack_top];
                tcc_state->cc_nodes[idx].line_start = start_line;
                tcc_state->cc_nodes[idx].col_start = start_col;
                tcc_state->cc_nodes[idx].aux1 = 0;
                tcc_state->cc_nodes[idx].aux_s1 = tcc_strdup("closure");
            }
            next(); /* consume '=>' */
            cc_parse_closure_body();
            cc_ast_record_end();
            vpushi(0);
            return 1;
        }
        /* Not a closure - but () is invalid in C */
        tcc_error("expected '=>' after '()' for closure");
        return 0;
    }
    
    /* Case 3: (x) => body, (x, y) => body, (int x) => body */
    if (tok >= TOK_UIDENT || tok == TOK_INT || tok == TOK_CHAR || tok == TOK_SHORT || tok == TOK_LONG || tok == TOK_VOID) {
        int cc_consumed[64];
        int cc_consumed_n = 0;
        int cc_param_tok[2] = {0, 0};
        char* cc_param_ty[2] = {0, 0};
        int cc_param_line[2] = {0, 0};
        int cc_param_col[2] = {0, 0};
        int cc_param_n = 0;
        int cc_is_arrow = 0;

#define CC_CONSUME_TOK() do { if (cc_consumed_n < 64) cc_consumed[cc_consumed_n++] = tok; next(); } while (0)

        /* Parse up to 2 params */
        for (int pi = 0; pi < 2; pi++) {
            if (tok == TOK_INT || tok == TOK_CHAR || tok == TOK_SHORT || tok == TOK_LONG || tok == TOK_VOID) {
                char tybuf[64];
                tybuf[0] = 0;
                const char* tnm = get_tok_str(tok, NULL);
                if (tnm) strncpy(tybuf, tnm, sizeof(tybuf) - 1);
                tybuf[sizeof(tybuf) - 1] = 0;
                CC_CONSUME_TOK();
                while (tok == '*') {
                    size_t L = strlen(tybuf);
                    if (L + 1 < sizeof(tybuf)) { tybuf[L] = '*'; tybuf[L + 1] = 0; }
                    CC_CONSUME_TOK();
                }
                if (tok < TOK_UIDENT) break;
                cc_param_tok[pi] = tok;
                cc_param_line[pi] = file ? file->line_num : 0;
                cc_param_col[pi] = tok_col;
                cc_param_ty[pi] = tcc_strdup(tybuf);
                cc_param_n = pi + 1;
                CC_CONSUME_TOK();
            } else if (tok >= TOK_UIDENT) {
                /* Could be: (x) untyped param, or (Type* x) typed param, or (Type x) typed param */
                int first_tok = tok;
                int first_line = file ? file->line_num : 0;
                int first_col = tok_col;
                const char* first_name = get_tok_str(tok, NULL);
                CC_CONSUME_TOK();
                
                /* Check if this is a type name: look for '*' or another identifier */
                if (tok == '*' || tok >= TOK_UIDENT) {
                    /* This is a type name - build type string */
                    char tybuf[128];
                    tybuf[0] = 0;
                    if (first_name) strncpy(tybuf, first_name, sizeof(tybuf) - 1);
                    tybuf[sizeof(tybuf) - 1] = 0;
                    
                    /* Consume pointer stars */
                    while (tok == '*') {
                        size_t L = strlen(tybuf);
                        if (L + 1 < sizeof(tybuf)) { tybuf[L] = '*'; tybuf[L + 1] = 0; }
                        CC_CONSUME_TOK();
                    }
                    
                    /* Now expect parameter name */
                    if (tok >= TOK_UIDENT) {
                        cc_param_tok[pi] = tok;
                        cc_param_line[pi] = file ? file->line_num : 0;
                        cc_param_col[pi] = tok_col;
                        cc_param_ty[pi] = tcc_strdup(tybuf);
                        cc_param_n = pi + 1;
                        CC_CONSUME_TOK();
                    } else {
                        /* Type without param name - not valid for closure */
                        break;
                    }
                } else {
                    /* Just an identifier - untyped param name */
                    cc_param_tok[pi] = first_tok;
                    cc_param_line[pi] = first_line;
                    cc_param_col[pi] = first_col;
                    cc_param_ty[pi] = NULL;
                    cc_param_n = pi + 1;
                }
            } else {
                break;
            }

            if (pi == 0 && tok == ',') {
                CC_CONSUME_TOK();
                continue;
            }
            break;
        }

        if (tok == ')') {
            CC_CONSUME_TOK();
            if (tok == TOK_CC_ARROW) {
                cc_is_arrow = 1;
                CC_CONSUME_TOK();
            } else if (tok == '=') {
                CC_CONSUME_TOK();
                if (tok == '>') {
                    cc_is_arrow = 1;
                    CC_CONSUME_TOK();
                }
            }
        }

        if (cc_is_arrow && cc_param_n > 0) {
            cc_ast_record_start(CC_AST_NODE_CLOSURE);
            if (tcc_state && tcc_state->cc_nodes && tcc_state->cc_node_stack_top >= 0) {
                int cidx = tcc_state->cc_node_stack[tcc_state->cc_node_stack_top];
                tcc_state->cc_nodes[cidx].line_start = start_line;
                tcc_state->cc_nodes[cidx].col_start = start_col;
                tcc_state->cc_nodes[cidx].aux1 = cc_param_n;
                tcc_state->cc_nodes[cidx].aux_s1 = tcc_strdup("closure");
            }

            /* Check if any param has a type annotation */
            int has_typed_params = 0;
            for (int pi = 0; pi < cc_param_n; pi++) {
                if (cc_param_ty[pi]) has_typed_params = 1;
                cc_ast_record_start(CC_AST_NODE_PARAM);
                if (tcc_state && tcc_state->cc_nodes && tcc_state->cc_node_stack_top >= 0) {
                    int pidx = tcc_state->cc_node_stack[tcc_state->cc_node_stack_top];
                    const char *pname = get_tok_str(cc_param_tok[pi], NULL);
                    tcc_state->cc_nodes[pidx].aux1 = cc_param_tok[pi];
                    tcc_state->cc_nodes[pidx].aux_s1 = pname ? tcc_strdup(pname) : NULL;
                    tcc_state->cc_nodes[pidx].aux_s2 = cc_param_ty[pi];
                    tcc_state->cc_nodes[pidx].line_start = cc_param_line[pi];
                    tcc_state->cc_nodes[pidx].col_start = cc_param_col[pi];
                    if (pname) tcc_state->cc_nodes[pidx].col_end = cc_param_col[pi] + (int)strlen(pname);
                }
                cc_ast_record_end();
            }

            cc_parse_closure_body_ex(has_typed_params);
            cc_ast_record_end();
            vpushi(0);
            return 1;
        }

        /* Not a closure: restore tokens */
        for (int pi = 0; pi < 2; pi++) if (cc_param_ty[pi]) tcc_free(cc_param_ty[pi]);
        for (int i = cc_consumed_n - 1; i >= 0; i--) unget_tok(cc_consumed[i]);

#undef CC_CONSUME_TOK
    }
    
    return 0;
}

/*
 * Parse single-parameter closure `x => expr` or `x => { block }`
 * Called after identifier has been consumed and next() called.
 * ident_tok is the identifier token, start_line/col are its position.
 * Returns: 1 = closure parsed, 0 = not a closure
 */
static int cc_try_cc_closure_single_param(int ident_tok, int start_line, int start_col) {
    int cc_is_arrow = 0;
    
    if (tok == TOK_CC_ARROW) {
        cc_is_arrow = 1;
    } else if (tok == '=') {
        next();
        if (tok == '>') {
            cc_is_arrow = 1;
        } else {
            unget_tok('=');
            return 0;
        }
    }
    
    if (!cc_is_arrow)
        return 0;
    
    cc_ast_record_start(CC_AST_NODE_CLOSURE);
    if (tcc_state && tcc_state->cc_nodes && tcc_state->cc_node_stack_top >= 0) {
        int cidx = tcc_state->cc_node_stack[tcc_state->cc_node_stack_top];
        tcc_state->cc_nodes[cidx].line_start = start_line;
        tcc_state->cc_nodes[cidx].col_start = start_col;
        tcc_state->cc_nodes[cidx].aux1 = 1;
        tcc_state->cc_nodes[cidx].aux_s1 = tcc_strdup("closure");
    }

    cc_ast_record_start(CC_AST_NODE_PARAM);
    if (tcc_state && tcc_state->cc_nodes && tcc_state->cc_node_stack_top >= 0) {
        int pidx = tcc_state->cc_node_stack[tcc_state->cc_node_stack_top];
        const char *pname = get_tok_str(ident_tok, NULL);
        tcc_state->cc_nodes[pidx].aux1 = ident_tok;
        tcc_state->cc_nodes[pidx].aux_s1 = pname ? tcc_strdup(pname) : NULL;
        tcc_state->cc_nodes[pidx].line_start = start_line;
        tcc_state->cc_nodes[pidx].col_start = start_col;
        if (pname) tcc_state->cc_nodes[pidx].col_end = start_col + (int)strlen(pname);
    }
    cc_ast_record_end();

    /* consume => */
    if (tok == TOK_CC_ARROW)
        next();
    else
        next(); /* tok is '>' */
    
    cc_parse_closure_body();
    cc_ast_record_end();
    vpushi(0);
    return 1;
}

static void cc_skip_retired_stmt_tail(void) {
    int par = 0, br = 0, sq = 0;
    while (tok && tok != TOK_EOF) {
        if (tok == '(') par++;
        else if (tok == ')' && par > 0) par--;
        else if (tok == '{') br++;
        else if (tok == '}' && br > 0) br--;
        else if (tok == '[') sq++;
        else if (tok == ']' && sq > 0) sq--;
        else if (par == 0 && br == 0 && sq == 0) {
            if (tok == ';') {
                next();
                break;
            }
            if (tok == '}') {
                break;
            }
        }
        next();
    }
}

static void cc_skip_retired_block(void) {
    if (tok != '{') return;
    int depth = 1;
    next();
    while (tok && tok != TOK_EOF && depth > 0) {
        if (tok == '{') depth++;
        else if (tok == '}') depth--;
        next();
    }
}

/*
 * Parse spawn statement: spawn(expr, ...); or spawn into(channel)? () => expr;
 * Returns: 1 = handled, 0 = not handled
 */
static int cc_try_thread_spawn(void) {
    if (tok < TOK_IDENT || strcmp(get_tok_str(tok, NULL), "spawn") != 0)
        return 0;

    next(); /* consume 'spawn' */
    if (tok >= TOK_IDENT && strcmp(get_tok_str(tok, NULL), "into") == 0) {
        tcc_error("async: `spawn into(...)` is retired; create/send `CCTask` handles explicitly");
    } else {
        tcc_error("async: `spawn(...)` statement is retired; use `n->spawn(...)` or explicit task APIs");
    }
    cc_skip_retired_stmt_tail();
    return 1;
}

/*
 * Parse @ statements: retired @arena/@arena_init/@nursery plus active @defer, @match, @with_deadline, @comptime
 * Returns:
 *   0 = not handled (tok unchanged)
 *   1 = handled, caller should go to next statement
 *   2 = handled as block statement, caller should fall through with t='{'
 */
static int cc_try_cc_at_stmt(void) {
    if (tok != '@')
        return 0;
    
    next(); /* consume '@' */
    if (tok < TOK_UIDENT) {
        tcc_error("expected identifier after '@'");
        return 0;
    }
    
    const char *cc_at = get_tok_str(tok, NULL);
    
    /* Check for known @ keywords */
    if (strcmp(cc_at, "arena") != 0 &&
        strcmp(cc_at, "arena_init") != 0 &&
        strcmp(cc_at, "closing") != 0 &&
        strcmp(cc_at, "nursery") != 0 &&
        strcmp(cc_at, "defer") != 0 &&
        strcmp(cc_at, "match") != 0 &&
        strcmp(cc_at, "with_deadline") != 0 &&
        strcmp(cc_at, "comptime") != 0) {
        tcc_error("unknown '@%s' block", cc_at);
        return 0;
    }
    
    /* --- @closing(ch[, ...]) { ... } --- */
    if (strcmp(cc_at, "closing") == 0) {
        tcc_error("async: `@closing(...)` is retired; use explicit ownership with `@create(...) @destroy { chan.close(); }`");
        next(); /* consume 'closing' */
        if (tok != '(')
            return 1;
        int depth = 1;
        next();
        while (tok && tok != TOK_EOF && depth > 0) {
            if (tok == '(') depth++;
            else if (tok == ')') depth--;
            next();
        }
        if (tok == '{') cc_skip_retired_block();
        else cc_skip_retired_stmt_tail();
        return 1;
    }

    /* --- @defer stmt; --- */
    if (strcmp(cc_at, "defer") == 0) {
        cc_ast_record_start(CC_AST_NODE_STMT);
        if (tcc_state && tcc_state->cc_nodes && tcc_state->cc_node_stack_top >= 0) {
            int idx = tcc_state->cc_node_stack[tcc_state->cc_node_stack_top];
            tcc_state->cc_nodes[idx].aux_s1 = tcc_strdup("defer");
        }
        next(); /* consume 'defer' */
        
        /* Optional (err) for conditional defer */
        if (tok == '(') {
            int depth = 1;
            next();
            while (tok && tok != TOK_EOF && depth > 0) {
                if (tok == '(') depth++;
                else if (tok == ')') depth--;
                next();
            }
        }
        
        /* Consume tokens until ';' or end of block at depth 0 */
        int par = 0, br = 0, sq = 0;
        while (tok && tok != TOK_EOF) {
            if (tok == '(') par++;
            else if (tok == ')' && par > 0) par--;
            else if (tok == '{') br++;
            else if (tok == '}' && br > 0) br--;
            else if (tok == '[') sq++;
            else if (tok == ']' && sq > 0) sq--;
            else if (par == 0 && br == 0 && sq == 0) {
                if (tok == ';') {
                    next(); /* consume ';' */
                    break;
                }
                if (tok == '}') {
                    /* End of block - don't consume, caller will */
                    break;
                }
            }
            next();
        }
        cc_ast_record_end();
        return 1; /* handled, go again */
    }
    
    /* --- @nursery { ... } --- */
    if (strcmp(cc_at, "nursery") == 0) {
        tcc_error("async: `@nursery { ... }` is retired; use `CCNursery* n = @create(parent) @destroy` and `n->spawn(...)`");
        next(); /* consume 'nursery' */
        
        /* Optional 'closing(...)' clause */
        if (tok >= TOK_IDENT && strcmp(get_tok_str(tok, NULL), "closing") == 0) {
            next(); /* consume 'closing' */
            if (tok != '(')
                return 1;
            int depth = 1;
            next();
            while (tok && tok != TOK_EOF && depth > 0) {
                if (tok == '(') depth++;
                else if (tok == ')') depth--;
                next();
            }
        }
        if (tok == '{') cc_skip_retired_block();
        else cc_skip_retired_stmt_tail();
        return 1;
    }
    
    /* --- @match { ... } --- */
    if (strcmp(cc_at, "match") == 0) {
        cc_ast_record_start(CC_AST_NODE_STMT);
        if (tcc_state && tcc_state->cc_nodes && tcc_state->cc_node_stack_top >= 0) {
            int idx = tcc_state->cc_node_stack[tcc_state->cc_node_stack_top];
            tcc_state->cc_nodes[idx].aux_s1 = tcc_strdup("match");
        }
        next(); /* consume 'match' */
        if (tok != '{')
            tcc_error("expected '{' after @match");
        
        next(); /* consume '{' */
        /* Parse case arms inside @match */
        while (tok && tok != '}' && tok != TOK_EOF) {
            if (tok == TOK_CASE) {
                cc_ast_record_start(CC_AST_NODE_STMT);
                if (tcc_state && tcc_state->cc_nodes && tcc_state->cc_node_stack_top >= 0) {
                    int idx = tcc_state->cc_node_stack[tcc_state->cc_node_stack_top];
                    tcc_state->cc_nodes[idx].aux_s1 = tcc_strdup("case");
                }
                next(); /* consume 'case' */
                /* Consume until ':' */
                while (tok && tok != ':' && tok != TOK_EOF) {
                    next();
                }
                if (tok == ':') next();
                
                /* Parse case body (usually a block or stmt) */
                if (tok == '{') {
                    /* Fall through to TCC's block parser if we were returning 2, 
                       but here we are inside a loop. We need to handle nested blocks. */
                    int depth = 1;
                    next();
                    while (tok && tok != TOK_EOF && depth > 0) {
                        if (tok == '{') depth++;
                        else if (tok == '}') depth--;
                        next();
                    }
                } else {
                    /* Single statement case body - consume until ';' */
                    while (tok && tok != ';' && tok != TOK_EOF) {
                        next();
                    }
                    if (tok == ';') next();
                }
                cc_ast_record_end();
            } else if (tok == TOK_DEFAULT) {
                /* Similar to case */
                next();
                if (tok == ':') next();
                /* ... consume body ... */
            } else {
                next();
            }
        }
        if (tok == '}') next();
        
        cc_ast_record_end();
        return 1; /* handled as a full block now */
    }

    /* --- @comptime { ... } --- */
    if (strcmp(cc_at, "comptime") == 0) {
        cc_ast_record_start(CC_AST_NODE_STMT);
        if (tcc_state && tcc_state->cc_nodes && tcc_state->cc_node_stack_top >= 0) {
            int idx = tcc_state->cc_node_stack[tcc_state->cc_node_stack_top];
            tcc_state->cc_nodes[idx].aux_s1 = tcc_strdup("comptime");
        }
        next(); /* consume 'comptime' */
        if (tok != '{')
            tcc_error("expected '{' after @comptime");
        return 2; /* handled, fall through to block */
    }
    
    /* --- @with_deadline(expr) { ... } --- */
    if (strcmp(cc_at, "with_deadline") == 0) {
        cc_ast_record_start(CC_AST_NODE_STMT);
        if (tcc_state && tcc_state->cc_nodes && tcc_state->cc_node_stack_top >= 0) {
            int idx = tcc_state->cc_node_stack[tcc_state->cc_node_stack_top];
            tcc_state->cc_nodes[idx].aux_s1 = tcc_strdup("with_deadline");
        }
        next(); /* consume 'with_deadline' */
        
        /* Parse optional (expr) for deadline specification */
        if (tok == '(') {
            int depth = 1;
            next();
            while (tok && tok != TOK_EOF && depth > 0) {
                if (tok == '(') depth++;
                else if (tok == ')') depth--;
                next();
            }
        }
        if (tok != '{')
            tcc_error("expected '{' after @with_deadline");
        return 2; /* handled, fall through to block */
    }
    
    /* --- @arena or @arena_init { ... } --- */
    if (strcmp(cc_at, "arena") == 0 || strcmp(cc_at, "arena_init") == 0) {
        next();

        if (strcmp(cc_at, "arena_init") == 0) {
            tcc_error("async: `@arena_init(...) { ... }` is retired; use `CCArena a = @create(buf, size) @destroy` or `cc_arena_init(...)` directly");
        } else {
            tcc_error("async: `@arena(...) { ... }` is retired; use `CCArena a = @create(size) @destroy`");
        }

        if (tok == '(') {
            int depth = 1;
            next(); /* after '(' */
            while (tok && tok != TOK_EOF && depth > 0) {
                if (tok == '(') depth++;
                else if (tok == ')') depth--;
                next();
            }
        }

        if (tok == '{') cc_skip_retired_block();
        else cc_skip_retired_stmt_tail();
        return 1;
    }
    
    return 0;
}

#endif /* CC_TCC_EXT_AVAILABLE */