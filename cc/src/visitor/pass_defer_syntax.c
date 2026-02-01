#include "pass_defer_syntax.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util/text.h"
#include "visitor/edit_buffer.h"

typedef enum {
    DEFER_ALWAYS = 0,  /* @defer - always runs */
    DEFER_ON_ERR,      /* @defer(err) - only on error return */
    DEFER_ON_OK        /* @defer(ok) - only on success return */
} CCDeferCondition;

typedef struct {
    int line_no;
    char* stmt; /* includes trailing ';' */
    CCDeferCondition cond;
} CCDeferStmt;

static void cc__free_defer_list(CCDeferStmt* xs, int n) {
    if (!xs) return;
    for (int i = 0; i < n; i++) free(xs[i].stmt);
    free(xs);
}

/* Local aliases for the shared helpers */
#define cc__is_ident_start cc_is_ident_start
#define cc__is_ident_char cc_is_ident_char
#define cc__append_n cc_sb_append
#define cc__append_str cc_sb_append_cstr

static void cc__ensure_line_start(char** out, size_t* out_len, size_t* out_cap) {
    if (!out || !out_len || !out_cap) return;
    if (*out_len == 0) return;
    char last = (*out)[*out_len - 1];
    if (last != '\n') cc__append_n(out, out_len, out_cap, "\n", 1);
}

static int cc__token_is(const char* s, size_t len, size_t i, const char* tok) {
    size_t tn = strlen(tok);
    if (i + tn > len) return 0;
    if (memcmp(s + i, tok, tn) != 0) return 0;
    if (i > 0 && cc__is_ident_char(s[i - 1])) return 0;
    if (i + tn < len && cc__is_ident_char(s[i + tn])) return 0;
    return 1;
}

static int cc__is_if_controlled_return(const char* s, size_t len, size_t ret_i) {
    (void)len;
    if (!s || ret_i == 0) return 0;
    /* Heuristic: detect `if (...) return ...;` without braces by looking backward for a ')'
       immediately before the `return` token (ignoring whitespace), and checking for `if`
       before the matching '('. */
    size_t j = ret_i;
    while (j > 0 && (s[j - 1] == ' ' || s[j - 1] == '\t' || s[j - 1] == '\r' || s[j - 1] == '\n')) j--;
    if (j == 0 || s[j - 1] != ')') return 0;

    int par = 0;
    size_t k = j - 1;
    while (k > 0) {
        char ch = s[k - 1];
        if (ch == ')') par++;
        else if (ch == '(') {
            if (par == 0) break;
            par--;
        }
        k--;
    }
    if (k == 0) return 0;

    size_t t = k - 1;
    while (t > 0 && (s[t - 1] == ' ' || s[t - 1] == '\t' || s[t - 1] == '\r' || s[t - 1] == '\n')) t--;
    if (t < 2) return 0;
    if (s[t - 2] != 'i' || s[t - 1] != 'f') return 0;
    if (t > 2 && cc__is_ident_char(s[t - 3])) return 0; /* word boundary */
    return 1;
}

static int cc__scan_stmt_end_semicolon(const char* s, size_t len, size_t i, size_t* out_end_off) {
    int par = 0, brk = 0, br = 0;
    int in_str = 0;
    char qch = 0;
    int in_line_comment = 0;
    int in_block_comment = 0;
    for (; i < len; i++) {
        char ch = s[i];
        if (in_line_comment) {
            if (ch == '\n') in_line_comment = 0;
            continue;
        }
        if (in_block_comment) {
            if (ch == '*' && i + 1 < len && s[i + 1] == '/') { in_block_comment = 0; i++; }
            continue;
        }
        if (in_str) {
            if (ch == '\\' && i + 1 < len) { i++; continue; }
            if (ch == qch) in_str = 0;
            continue;
        }
        if (ch == '/' && i + 1 < len && s[i + 1] == '/') { in_line_comment = 1; i++; continue; }
        if (ch == '/' && i + 1 < len && s[i + 1] == '*') { in_block_comment = 1; i++; continue; }
        if (ch == '"' || ch == '\'') { in_str = 1; qch = ch; continue; }

        if (ch == '(') par++;
        else if (ch == ')') { if (par) par--; }
        else if (ch == '[') brk++;
        else if (ch == ']') { if (brk) brk--; }
        else if (ch == '{') br++;
        else if (ch == '}') { if (br) br--; }
        else if (ch == ';' && par == 0 && brk == 0 && br == 0) {
            if (out_end_off) *out_end_off = i + 1;
            return 1;
        }
    }
    return 0;
}

int cc__rewrite_defer_syntax(const CCVisitorCtx* ctx,
                            const char* in_src,
                            size_t in_len,
                            char** out_src,
                            size_t* out_len) {
    if (!ctx || !in_src || !out_src || !out_len) return 0;
    *out_src = NULL;
    *out_len = 0;

    /* Defer stacks by brace depth. */
    CCDeferStmt* defers[256];
    int defer_counts[256];
    int defer_caps[256];
    for (int d = 0; d < 256; d++) { defers[d] = NULL; defer_counts[d] = 0; defer_caps[d] = 0; }

    char* out = NULL;
    size_t outl = 0, outc = 0;

    int depth = 0;
    int line_no = 1;
    int in_str = 0;
    char qch = 0;
    int in_line_comment = 0;
    int in_block_comment = 0;

    int changed = 0;

    for (size_t i = 0; i < in_len; i++) {
        char ch = in_src[i];

        if (ch == '\n') line_no++;

        if (in_line_comment) {
            cc__append_n(&out, &outl, &outc, &ch, 1);
            if (ch == '\n') in_line_comment = 0;
            continue;
        }
        if (in_block_comment) {
            cc__append_n(&out, &outl, &outc, &ch, 1);
            if (ch == '*' && i + 1 < in_len && in_src[i + 1] == '/') {
                cc__append_n(&out, &outl, &outc, &in_src[i + 1], 1);
                i++;
                in_block_comment = 0;
            }
            continue;
        }
        if (in_str) {
            cc__append_n(&out, &outl, &outc, &ch, 1);
            if (ch == '\\' && i + 1 < in_len) {
                cc__append_n(&out, &outl, &outc, &in_src[i + 1], 1);
                i++;
                continue;
            }
            if (ch == qch) in_str = 0;
            continue;
        }

        if (ch == '/' && i + 1 < in_len && in_src[i + 1] == '/') {
            cc__append_n(&out, &outl, &outc, &in_src[i], 2);
            i++;
            in_line_comment = 1;
            continue;
        }
        if (ch == '/' && i + 1 < in_len && in_src[i + 1] == '*') {
            cc__append_n(&out, &outl, &outc, &in_src[i], 2);
            i++;
            in_block_comment = 1;
            continue;
        }
        if (ch == '"' || ch == '\'') {
            cc__append_n(&out, &outl, &outc, &ch, 1);
            in_str = 1;
            qch = ch;
            continue;
        }

        /* `cancel ...;` is not implemented: hard error. */
        if (cc__token_is(in_src, in_len, i, "cancel")) {
            const char* f = ctx->input_path ? ctx->input_path : "<input>";
            fprintf(stderr, "%s:%d:1: error: CC: 'cancel' is not implemented (use structured scopes instead)\n", f, line_no);
            for (int d = 0; d < 256; d++) cc__free_defer_list(defers[d], defer_counts[d]);
            free(out);
            return -1;
        }

        /* `return ...;` should execute all active defers (current scope and outers). */
        if (cc__token_is(in_src, in_len, i, "return")) {
            size_t stmt_end = 0;
            if (!cc__scan_stmt_end_semicolon(in_src, in_len, i, &stmt_end)) {
                const char* f = ctx->input_path ? ctx->input_path : "<input>";
                fprintf(stderr, "%s:%d:1: error: CC: malformed 'return' (expected ';')\n", f, line_no);
                for (int d = 0; d < 256; d++) cc__free_defer_list(defers[d], defer_counts[d]);
                free(out);
                return -1;
            }
            int has_defers = 0;
            for (int d = depth; d >= 0; d--) {
                int dd = d;
                if (dd < 0) dd = 0;
                if (dd >= 256) dd = 255;
                if (defer_counts[dd] > 0) { has_defers = 1; break; }
            }

            if (!has_defers) {
                /* No active defers: keep return as-is. */
                cc__append_n(&out, &outl, &outc, in_src + i, stmt_end - i);
                /* Skip original text (keep line_no tracking correct via the main loop's '\n' handler). */
                for (size_t k = i; k < stmt_end; k++) {
                    if (in_src[k] == '\n') line_no++;
                }
                i = stmt_end - 1;
                continue;
            }

            int is_if_ctl = cc__is_if_controlled_return(in_src, in_len, i);
            
            /* Check if we have any conditional defers */
            int has_conditional = 0;
            for (int d = depth; d >= 0 && !has_conditional; d--) {
                int dd = (d < 0) ? 0 : (d >= 256 ? 255 : d);
                for (int k = 0; k < defer_counts[dd]; k++) {
                    if (defers[dd][k].cond != DEFER_ALWAYS) {
                        has_conditional = 1;
                        break;
                    }
                }
            }
            
            /* Extract return expression (between 'return' and ';') */
            size_t ret_kw_end = i + 6; /* strlen("return") */
            while (ret_kw_end < stmt_end && (in_src[ret_kw_end] == ' ' || in_src[ret_kw_end] == '\t')) ret_kw_end++;
            size_t expr_start = ret_kw_end;
            size_t expr_end = stmt_end - 1; /* exclude ';' */
            while (expr_end > expr_start && (in_src[expr_end - 1] == ' ' || in_src[expr_end - 1] == '\t' || in_src[expr_end - 1] == '\n')) expr_end--;
            int has_expr = (expr_end > expr_start);
            
            if (is_if_ctl) {
                cc__append_str(&out, &outl, &outc, "{\n");
            }
            
            if (has_conditional && has_expr) {
                /* Emit: { __typeof__(expr) __cc_ret = (expr); bool __cc_ret_err = !__cc_ret.ok; ... } */
                cc__append_str(&out, &outl, &outc, "{ __typeof__(");
                cc__append_n(&out, &outl, &outc, in_src + expr_start, expr_end - expr_start);
                cc__append_str(&out, &outl, &outc, ") __cc_ret = (");
                cc__append_n(&out, &outl, &outc, in_src + expr_start, expr_end - expr_start);
                cc__append_str(&out, &outl, &outc, "); int __cc_ret_err = !__cc_ret.ok;\n");
                
                /* Emit defers with conditions.
                   NOTE: We do NOT clear the defer lists here - they need to remain
                   active for subsequent return statements in the same scope.
                   The defers are only cleared at scope exit ('}') for DEFER_ALWAYS. */
                for (int d = depth; d >= 0; d--) {
                    int dd = (d < 0) ? 0 : (d >= 256 ? 255 : d);
                    for (int k = defer_counts[dd] - 1; k >= 0; k--) {
                        if (defers[dd][k].cond == DEFER_ALWAYS) {
                            cc__append_str(&out, &outl, &outc, defers[dd][k].stmt);
                            cc__append_n(&out, &outl, &outc, "\n", 1);
                        } else if (defers[dd][k].cond == DEFER_ON_ERR) {
                            cc__append_str(&out, &outl, &outc, "if (__cc_ret_err) { ");
                            cc__append_str(&out, &outl, &outc, defers[dd][k].stmt);
                            cc__append_str(&out, &outl, &outc, " }\n");
                        } else if (defers[dd][k].cond == DEFER_ON_OK) {
                            cc__append_str(&out, &outl, &outc, "if (!__cc_ret_err) { ");
                            cc__append_str(&out, &outl, &outc, defers[dd][k].stmt);
                            cc__append_str(&out, &outl, &outc, " }\n");
                        }
                    }
                    /* Don't clear defers here - they apply to all returns in this scope */
                }
                cc__append_str(&out, &outl, &outc, "return __cc_ret; }");
            } else {
                /* No conditional defers or no expression - use original logic.
                   Still emit DEFER_ALWAYS defers at every return, but don't clear the list
                   so that subsequent returns also get the defers. */
                for (int d = depth; d >= 0; d--) {
                    int dd = (d < 0) ? 0 : (d >= 256 ? 255 : d);
                    for (int k = defer_counts[dd] - 1; k >= 0; k--) {
                        cc__append_str(&out, &outl, &outc, defers[dd][k].stmt);
                        cc__append_n(&out, &outl, &outc, "\n", 1);
                    }
                    /* Don't clear defers here - they apply to all returns in this scope */
                }
                cc__ensure_line_start(&out, &outl, &outc);
                cc__append_n(&out, &outl, &outc, in_src + i, stmt_end - i);
            }
            
            if (is_if_ctl) {
                cc__append_str(&out, &outl, &outc, "\n}");
            }
            changed = 1;

            /* Skip original text (keep line_no tracking correct via the main loop's '\n' handler). */
            for (size_t k = i; k < stmt_end; k++) {
                if (in_src[k] == '\n') line_no++;
            }
            i = stmt_end - 1;
            continue;
        }

        /* `@defer ...;` or `@defer(err) ...;` or `@defer(ok) ...;` */
        if (cc__token_is(in_src, in_len, i, "@defer")) {
            int defer_line = line_no;
            int defer_depth = depth;
            CCDeferCondition cond = DEFER_ALWAYS;

            size_t j = i + 6;
            while (j < in_len && (in_src[j] == ' ' || in_src[j] == '\t')) j++;

            /* Check for (err) or (ok) condition */
            if (j < in_len && in_src[j] == '(') {
                size_t paren_start = j;
                j++;
                while (j < in_len && (in_src[j] == ' ' || in_src[j] == '\t')) j++;
                if (j + 3 <= in_len && strncmp(in_src + j, "err", 3) == 0 && 
                    (j + 3 >= in_len || !cc__is_ident_char(in_src[j + 3]))) {
                    cond = DEFER_ON_ERR;
                    j += 3;
                } else if (j + 2 <= in_len && strncmp(in_src + j, "ok", 2) == 0 &&
                           (j + 2 >= in_len || !cc__is_ident_char(in_src[j + 2]))) {
                    cond = DEFER_ON_OK;
                    j += 2;
                }
                while (j < in_len && (in_src[j] == ' ' || in_src[j] == '\t')) j++;
                if (j < in_len && in_src[j] == ')') {
                    j++;
                } else {
                    /* Not a valid condition, reset */
                    j = paren_start;
                    cond = DEFER_ALWAYS;
                }
                while (j < in_len && (in_src[j] == ' ' || in_src[j] == '\t')) j++;
            }

            /* Optional name: identifier ':' */
            size_t name_start = j;
            if (j < in_len && cc__is_ident_start(in_src[j])) {
                j++;
                while (j < in_len && cc__is_ident_char(in_src[j])) j++;
                size_t save = j;
                while (j < in_len && (in_src[j] == ' ' || in_src[j] == '\t')) j++;
                if (j < in_len && in_src[j] == ':') {
                    j++;
                    while (j < in_len && (in_src[j] == ' ' || in_src[j] == '\t')) j++;
                } else {
                    /* not a name: rewind */
                    (void)name_start;
                    j = name_start;
                }
                (void)save;
            }

            size_t stmt_start = j;
            size_t stmt_end = 0;
            if (!cc__scan_stmt_end_semicolon(in_src, in_len, stmt_start, &stmt_end)) {
                const char* f = ctx->input_path ? ctx->input_path : "<input>";
                fprintf(stderr, "%s:%d:1: error: CC: malformed '@defer' (expected ';')\n", f, defer_line);
                for (int d = 0; d < 256; d++) cc__free_defer_list(defers[d], defer_counts[d]);
                free(out);
                return -1;
            }

            size_t stmt_len = stmt_end - stmt_start;
            char* stmt = (char*)malloc(stmt_len + 1);
            if (!stmt) {
                for (int d = 0; d < 256; d++) cc__free_defer_list(defers[d], defer_counts[d]);
                free(out);
                return -1;
            }
            memcpy(stmt, in_src + stmt_start, stmt_len);
            stmt[stmt_len] = 0;

            if (defer_depth < 0) defer_depth = 0;
            if (defer_depth >= 256) defer_depth = 255;

            if (defer_counts[defer_depth] + 1 > defer_caps[defer_depth]) {
                int nc = defer_caps[defer_depth] ? defer_caps[defer_depth] * 2 : 8;
                CCDeferStmt* nb = (CCDeferStmt*)realloc(defers[defer_depth], (size_t)nc * sizeof(CCDeferStmt));
                if (!nb) {
                    free(stmt);
                    for (int d = 0; d < 256; d++) cc__free_defer_list(defers[d], defer_counts[d]);
                    free(out);
                    return -1;
                }
                defers[defer_depth] = nb;
                defer_caps[defer_depth] = nc;
            }
            defers[defer_depth][defer_counts[defer_depth]++] = (CCDeferStmt){ .line_no = defer_line, .stmt = stmt, .cond = cond };

            cc__append_str(&out, &outl, &outc, "/* @defer recorded */");
            changed = 1;

            /* Skip original text (keep line_no tracking correct via the main loop's '\n' handler). */
            for (size_t k = i; k < stmt_end; k++) {
                if (in_src[k] == '\n') line_no++;
            }
            i = stmt_end - 1;
            continue;
        }

        if (ch == '}') {
            /* Emit defers for this scope before the brace.
               Only emit DEFER_ALWAYS defers at }; conditional defers only run at return. */
            int d = depth;
            if (d < 0) d = 0;
            if (d >= 256) d = 255;
            if (defer_counts[d] > 0) {
                for (int k = defer_counts[d] - 1; k >= 0; k--) {
                    if (defers[d][k].cond == DEFER_ALWAYS) {
                        cc__append_str(&out, &outl, &outc, defers[d][k].stmt);
                        if (defers[d][k].stmt[0] != 0) {
                            size_t sl = strlen(defers[d][k].stmt);
                            if (sl == 0 || defers[d][k].stmt[sl - 1] != '\n') cc__append_str(&out, &outl, &outc, "\n");
                        }
                    }
                    /* Note: conditional defers are NOT emitted at } - only at return.
                       We still free them to avoid leaks. */
                    free(defers[d][k].stmt);
                    defers[d][k].stmt = NULL;
                }
                defer_counts[d] = 0;
            }
            if (depth > 0) depth--;
            cc__append_n(&out, &outl, &outc, &ch, 1);
            continue;
        }

        if (ch == '{') {
            depth++;
            cc__append_n(&out, &outl, &outc, &ch, 1);
            continue;
        }

        cc__append_n(&out, &outl, &outc, &ch, 1);
    }

    for (int d = 0; d < 256; d++) cc__free_defer_list(defers[d], defer_counts[d]);

    if (!changed) {
        free(out);
        return 0;
    }

    *out_src = out;
    *out_len = outl;
    return 1;
}

/* NEW: Collect @defer edits into EditBuffer without applying.
   NOTE: Due to the complexity of defer semantics (scope tracking, multiple injection points),
   this function uses the existing rewrite function and adds a single whole-file edit.
   Returns number of edits added (>= 0), or -1 on error. */
int cc__collect_defer_edits(const CCVisitorCtx* ctx, CCEditBuffer* eb) {
    if (!ctx || !eb || !eb->src) return 0;

    char* rewritten = NULL;
    size_t rewritten_len = 0;
    int r = cc__rewrite_defer_syntax(ctx, eb->src, eb->src_len, &rewritten, &rewritten_len);
    
    if (r < 0) {
        /* Error already printed */
        return -1;
    }
    if (r == 0 || !rewritten) {
        /* No changes */
        return 0;
    }    /* Add a whole-file replacement edit */
    int edits_added = 0;
    if (cc_edit_buffer_add(eb, 0, eb->src_len, rewritten, 40, "defer") == 0) {
        edits_added = 1;
    }
    free(rewritten);
    return edits_added;
}