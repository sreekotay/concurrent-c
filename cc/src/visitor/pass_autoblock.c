/* Extracted from the working implementation in `cc/src/visitor/visitor.c`.
   Goal: keep semantics identical while shrinking visitor.c over time.

   This pass is intentionally self-contained (it duplicates a few small text helpers)
   so it doesn't depend on `static` helpers inside visitor.c.
*/

#include "pass_autoblock.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "comptime/symbols.h"
#include "visitor/text_span.h"
#include "visitor/visitor.h"

enum {
    CC_FN_ATTR_ASYNC = 1u << 0,
    CC_FN_ATTR_NOBLOCK = 1u << 1,
    CC_FN_ATTR_LATENCY_SENSITIVE = 1u << 2,
};

typedef struct {
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
} NodeView;

static int cc__node_is_descendant_of_kind(const CCASTRoot* root,
                                         const NodeView* n,
                                         int idx,
                                         int kind) {
    if (!root || !n) return 0;
    for (int cur = idx; cur >= 0 && cur < root->node_count; cur = n[cur].parent) {
        if (n[cur].kind == kind) return 1;
    }
    return 0;
}

/* ---- small shared helpers (duplicated from visitor.c) ---- */

static const char* cc__basename(const char* path) {
    if (!path) return NULL;
    const char* last = path;
    for (const char* p = path; *p; p++) {
        if (*p == '/' || *p == '\\') last = p + 1;
    }
    return last;
}

/* Return pointer to a stable suffix (last 2 path components) inside `path`.
   If `path` has fewer than 2 components, returns basename. */
static const char* cc__path_suffix2(const char* path) {
    if (!path) return NULL;
    const char* end = path + strlen(path);
    int seps = 0;
    for (const char* p = end; p > path; ) {
        p--;
        if (*p == '/' || *p == '\\') {
            seps++;
            if (seps == 2) return p + 1;
        }
    }
    return cc__basename(path);
}

static int cc__same_source_file(const char* a, const char* b) {
    if (!a || !b) return 0;
    if (strcmp(a, b) == 0) return 1;

    const char* a_base = cc__basename(a);
    const char* b_base = cc__basename(b);
    if (!a_base || !b_base || strcmp(a_base, b_base) != 0) return 0;

    /* Prefer 2-component suffix match (handles duplicate basenames across dirs). */
    const char* a_suf = cc__path_suffix2(a);
    const char* b_suf = cc__path_suffix2(b);
    if (a_suf && b_suf && strcmp(a_suf, b_suf) == 0) return 1;

    /* Fallback: basename-only match. */
    return 1;
}

static int cc__node_file_matches_this_tu(const CCASTRoot* root,
                                        const CCVisitorCtx* ctx,
                                        const char* node_file) {
    if (!ctx || !ctx->input_path || !node_file) return 0;
    if (cc__same_source_file(ctx->input_path, node_file)) return 1;
    if (root && root->lowered_path && cc__same_source_file(root->lowered_path, node_file)) return 1;
    return 0;
}

static int cc__lookup_func_attrs(const CCASTRoot* root,
                                 const CCVisitorCtx* ctx,
                                 const char* name,
                                 unsigned int* out_attrs) {
    if (out_attrs) *out_attrs = 0;
    if (!root || !name) return 0;
    const NodeView* n = (const NodeView*)root->nodes;
    for (int i = 0; i < root->node_count; i++) {
        if (n[i].kind != 17) continue; /* CC_AST_NODE_FUNC */
        if (!n[i].aux_s1 || strcmp(n[i].aux_s1, name) != 0) continue;
        if (!cc__node_file_matches_this_tu(root, ctx, n[i].file)) continue;
        if (out_attrs) *out_attrs = (unsigned int)n[i].aux1;
        return 1;
    }
    return 0;
}


static int cc__append_str(char** buf, size_t* len, size_t* cap, const char* s) {
    if (!buf || !len || !cap || !s) return 0;
    size_t n = strlen(s);
    size_t need = *len + n + 1;
    if (need > *cap) {
        size_t nc = *cap ? *cap * 2 : 1024;
        while (nc < need) nc *= 2;
        char* nb = (char*)realloc(*buf, nc);
        if (!nb) return 0;
        *buf = nb;
        *cap = nc;
    }
    memcpy(*buf + *len, s, n);
    *len += n;
    (*buf)[*len] = '\0';
    return 1;
}

static int cc__append_n(char** buf, size_t* len, size_t* cap, const char* s, size_t n) {
    if (!buf || !len || !cap || !s) return 0;
    if (n == 0) return 1;
    size_t need = *len + n + 1;
    if (need > *cap) {
        size_t nc = *cap ? *cap * 2 : 1024;
        while (nc < need) nc *= 2;
        char* nb = (char*)realloc(*buf, nc);
        if (!nb) return 0;
        *buf = nb;
        *cap = nc;
    }
    memcpy(*buf + *len, s, n);
    *len += n;
    (*buf)[*len] = '\0';
    return 1;
}

static int cc__append_fmt(char** buf, size_t* len, size_t* cap, const char* fmt, ...) {
    if (!buf || !len || !cap || !fmt) return 0;
    char tmp[4096];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n <= 0) return 0;
    tmp[sizeof(tmp) - 1] = '\0';
    return cc__append_str(buf, len, cap, tmp);
}

static int cc__ab_only_ws_comments(const char* s, size_t a, size_t b) {
    if (!s) return 0;
    size_t i = a;
    while (i < b) {
        char c = s[i];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') { i++; continue; }
        if (c == '/' && i + 1 < b && s[i + 1] == '/') {
            i += 2;
            while (i < b && s[i] != '\n') i++;
            continue;
        }
        if (c == '/' && i + 1 < b && s[i + 1] == '*') {
            i += 2;
            while (i + 1 < b && !(s[i] == '*' && s[i + 1] == '/')) i++;
            if (i + 1 < b) i += 2;
            continue;
        }
        return 0;
    }
    return 1;
}

/* ---- end small helpers ---- */

int cc__rewrite_autoblocking_calls_with_nodes(const CCASTRoot* root,
                                                     const CCVisitorCtx* ctx,
                                                     const char* in_src,
                                                     size_t in_len,
                                                     char** out_src,
                                                     size_t* out_len) {
    if (!root || !ctx || !ctx->symbols || !in_src || !out_src || !out_len) return 0;
    *out_src = NULL;
    *out_len = 0;
    if (!root->nodes || root->node_count <= 0) return 0;

    const NodeView* n = (const NodeView*)root->nodes;

    typedef enum {
        CC_AB_REWRITE_STMT_CALL = 0,
        CC_AB_REWRITE_RETURN_CALL = 1,
        CC_AB_REWRITE_ASSIGN_CALL = 2,
        CC_AB_REWRITE_BATCH_STMT_CALLS = 3,
        CC_AB_REWRITE_BATCH_STMTS_THEN_RETURN = 4,
        CC_AB_REWRITE_BATCH_STMTS_THEN_ASSIGN = 5,
        CC_AB_REWRITE_RETURN_EXPR_CALL = 6,
        CC_AB_REWRITE_ASSIGN_EXPR_CALL = 7,
        /* Special-case: inside `await ...`, rewrite the call expression to a blocking-task operand.
           Used for chan_* operations (which are currently blocking runtime calls). */
        CC_AB_REWRITE_AWAIT_OPERAND_CALL = 8,
    } CCAutoBlockRewriteKind;

    typedef struct {
        size_t call_start;
        size_t call_end;
        int line_start;
        const char* callee;
        int argc;
        char* param_types[16]; /* owned */
    } CCAutoBlockBatchItem;

    typedef struct {
        size_t start;     /* statement start */
        size_t end;       /* statement end (inclusive of ';') */
        size_t call_start;
        size_t call_end;  /* end of ')' */
        int line_start;
        const char* callee;
        const char* lhs_name; /* for assign */
        CCAutoBlockRewriteKind kind;
        int argc;
        int ret_is_ptr;
        int ret_is_void;
        int ret_is_structy;
        char* param_types[16]; /* owned */
        size_t indent_start;
        size_t indent_len;
        CCAutoBlockBatchItem* batch;
        int batch_n;
        /* Optional trailing value-producing call to fold into the same dispatch. */
        int tail_kind; /* 0 none, 1 return, 2 assign */
        size_t tail_call_start;
        size_t tail_call_end;
        const char* tail_callee;
        const char* tail_lhs_name;
        int tail_argc;
        int tail_ret_is_ptr;
        char* tail_param_types[16]; /* owned */
    } Replace;
    Replace* reps = NULL;
    int rep_n = 0;
    int rep_cap = 0;

    for (int i = 0; i < root->node_count; i++) {
        if (n[i].kind != 5) continue; /* CALL */
        int is_ufcs = (n[i].aux2 & 2) != 0;
        if (is_ufcs) continue;
        if (!n[i].aux_s1) continue; /* callee name */
        if (!cc__node_file_matches_this_tu(root, ctx, n[i].file)) continue;

        /* Calls inside `await ...` are usually async call sites; never autoblock them.
           Exception: channel ops are currently blocking (chan_*), so `await tx.send(...)` / `await rx.recv(...)`
           must be rewritten into a blocking-task operand so async lowering sees a CCTaskIntptr. */
        int is_under_await = cc__node_is_descendant_of_kind(root, n, i, 6 /* CC_AST_NODE_AWAIT */);
        int is_chan_op = (strcmp(n[i].aux_s1, "chan_send") == 0 ||
                          strcmp(n[i].aux_s1, "chan_recv") == 0 ||
                          strcmp(n[i].aux_s1, "chan_send_take") == 0 ||
                          strcmp(n[i].aux_s1, "chan_send_take_ptr") == 0 ||
                          strcmp(n[i].aux_s1, "chan_send_take_slice") == 0);
        if (is_under_await && !is_chan_op) continue;

        /* Find enclosing function decl-item and check @async attr. */
        int cur = n[i].parent;
        const char* owner = NULL;
        unsigned int owner_attrs = 0;
        while (cur >= 0 && cur < root->node_count) {
            if (n[cur].kind == 12 && n[cur].aux_s1 && n[cur].aux_s2 && strchr(n[cur].aux_s2, '(') &&
                cc__node_file_matches_this_tu(root, ctx, n[cur].file)) {
                owner = n[cur].aux_s1;
                owner_attrs = (unsigned int)n[cur].aux2;
                break;
            }
            cur = n[cur].parent;
        }
        if (!owner || ((owner_attrs & CC_FN_ATTR_ASYNC) == 0)) continue;

        /* Only skip known-nonblocking callees; if we don't know attrs, assume blocking. */
        unsigned int callee_attrs = 0;
        if (!cc__lookup_func_attrs(root, ctx, n[i].aux_s1, &callee_attrs)) {
            (void)cc_symbols_lookup_fn_attrs(ctx->symbols, n[i].aux_s1, &callee_attrs);
        }
        if (callee_attrs & CC_FN_ATTR_ASYNC) continue;
        if (callee_attrs & CC_FN_ATTR_NOBLOCK) continue;

        /* Compute span offsets in the CURRENT input buffer using line/col. */
        if (n[i].line_start <= 0 || n[i].col_start <= 0) continue;
        if (n[i].line_end <= 0 || n[i].col_end <= 0) continue;
        size_t call_start = cc__offset_of_line_col_1based(in_src, in_len, n[i].line_start, n[i].col_start);
        size_t call_end = cc__offset_of_line_col_1based(in_src, in_len, n[i].line_end, n[i].col_end);
        if (call_start >= call_end || call_end > in_len) continue;

        /* Some TCC call spans report col_start at '(' rather than the callee identifier.
           Expand start leftwards to include a preceding identifier token. */
        {
            size_t s2 = call_start;
            while (s2 > 0 && (in_src[s2 - 1] == ' ' || in_src[s2 - 1] == '\t')) s2--;
            while (s2 > 0 && (isalnum((unsigned char)in_src[s2 - 1]) || in_src[s2 - 1] == '_')) s2--;
            if (s2 < call_start) call_start = s2;
        }

        /* Next non-ws token after call. */
        size_t j = call_end;
        while (j < in_len && (in_src[j] == ' ' || in_src[j] == '\t' || in_src[j] == '\n' || in_src[j] == '\r')) j++;
        int is_stmt_form = (j < in_len && in_src[j] == ';') ? 1 : 0;
        size_t stmt_end = is_stmt_form ? (j + 1) : call_end;

        /* Line + indent info */
        size_t lb = cc__offset_of_line_1based(in_src, in_len, n[i].line_start);
        size_t first = lb;
        while (first < in_len && (in_src[first] == ' ' || in_src[first] == '\t')) first++;
        size_t indent_start = lb;
        size_t indent_len = first > lb ? (first - lb) : 0;

        /* Prefer FUNC/PARAM stub nodes; fallback to DECL_ITEM text signature. */
        const char* ret_str = NULL;
        char* param_types[16] = {0};
        int paramc = 0;
        int ret_is_ptr = 0, ret_is_void = 0, ret_is_structy = 0;

        int found_func = 0;
        for (int k = 0; k < root->node_count; k++) {
            if (n[k].kind != 17) continue; /* CC_AST_NODE_FUNC */
            if (!n[k].aux_s1 || strcmp(n[k].aux_s1, n[i].aux_s1) != 0) continue;
            if (!cc__node_file_matches_this_tu(root, ctx, n[k].file)) continue;
            found_func = 1;
            ret_str = n[k].aux_s2;
            /* collect params */
            for (int p = 0; p < root->node_count && paramc < 16; p++) {
                if (n[p].parent != k || n[p].kind != 16) continue; /* PARAM */
                if (n[p].aux_s2) param_types[paramc++] = strdup(n[p].aux_s2);
            }
            break;
        }

        const char* param_list_from_decl = NULL;
        if (!found_func) {
            const char* callee_sig = NULL;
            for (int k = 0; k < root->node_count; k++) {
                if (n[k].kind != 12) continue; /* DECL_ITEM */
                if (!n[k].aux_s1 || !n[k].aux_s2) continue;
                if (!cc__node_file_matches_this_tu(root, ctx, n[k].file)) continue;
                if (strcmp(n[k].aux_s1, n[i].aux_s1) != 0) continue;
                if (!strchr(n[k].aux_s2, '(')) continue;
                callee_sig = n[k].aux_s2;
                break;
            }
            if (!callee_sig) continue;
            const char* l = strchr(callee_sig, '(');
            if (!l) continue;
            size_t pre_n = (size_t)(l - callee_sig);
            if (pre_n > 255) pre_n = 255;
            static char pre[256];
            memcpy(pre, callee_sig, pre_n);
            pre[pre_n] = 0;
            size_t a = 0;
            while (pre[a] == ' ' || pre[a] == '\t') a++;
            size_t b = strlen(pre);
            while (b > a && (pre[b - 1] == ' ' || pre[b - 1] == '\t')) b--;
            pre[b] = 0;
            ret_str = pre + a;
            const char* ps = strchr(callee_sig, '(');
            const char* pe = strrchr(callee_sig, ')');
            if (!ps || !pe || pe <= ps) continue;
            ps++; while (ps < pe && (*ps == ' ' || *ps == '\t')) ps++;
            while (pe > ps && (pe[-1] == ' ' || pe[-1] == '\t')) pe--;
            size_t ncp = (size_t)(pe - ps);
            char* param_buf = (char*)malloc(ncp + 1);
            if (!param_buf) continue;
            memcpy(param_buf, ps, ncp);
            param_buf[ncp] = 0;
            param_list_from_decl = param_buf;
        }

        if (ret_str) {
            if (strstr(ret_str, "struct") || strstr(ret_str, "CCSlice")) ret_is_structy = 1;
            if (strchr(ret_str, '*')) ret_is_ptr = 1;
            if (!ret_is_ptr && !ret_is_structy) {
                const char* v = strstr(ret_str, "void");
                if (v) {
                    const char* endt = ret_str + strlen(ret_str);
                    while (endt > ret_str && (endt[-1] == ' ' || endt[-1] == '\t')) endt--;
                    if (endt - ret_str >= 4 && memcmp(endt - 4, "void", 4) == 0) ret_is_void = 1;
                }
            }
        }

        if (!found_func && param_list_from_decl) {
            if (strlen(param_list_from_decl) == 0 || strcmp(param_list_from_decl, "void") == 0) {
                paramc = 0;
            } else {
                char* pcur = (char*)param_list_from_decl;
                while (*pcur && paramc < 16) {
                    while (*pcur == ' ' || *pcur == '\t') pcur++;
                    char* startp = pcur;
                    while (*pcur && *pcur != ',') pcur++;
                    char* endp = pcur;
                    while (endp > startp && (endp[-1] == ' ' || endp[-1] == '\t')) endp--;
                    size_t ln = (size_t)(endp - startp);
                    if (ln > 0) {
                        char* ty = (char*)malloc(ln + 1);
                        if (!ty) break;
                        memcpy(ty, startp, ln);
                        ty[ln] = 0;
                        param_types[paramc++] = ty;
                    }
                    if (*pcur == ',') { pcur++; continue; }
                    break;
                }
            }
            free((void*)param_list_from_decl);
        }

        /* Determine rewrite kind + statement start + validity checks for return/assign roots. */
        CCAutoBlockRewriteKind kind = CC_AB_REWRITE_STMT_CALL;
        const char* lhs_name = NULL;
        size_t stmt_start = lb;
        if (is_under_await && is_chan_op) {
            /* We rewrite the operand expression (call span) in-place, keeping the existing `await`. */
            kind = CC_AB_REWRITE_AWAIT_OPERAND_CALL;
        }

        /* Check for nearest RETURN or ASSIGN ancestor.
           NOTE: For CC_AB_REWRITE_AWAIT_OPERAND_CALL, we intentionally skip this and only replace the call span. */
        int assign_idx = -1;
        int ret_idx = -1;
        if (kind != CC_AB_REWRITE_AWAIT_OPERAND_CALL) {
            for (int cur2 = n[i].parent; cur2 >= 0 && cur2 < root->node_count; cur2 = n[cur2].parent) {
                if (n[cur2].kind == 15) { ret_idx = cur2; break; } /* RETURN */
                if (n[cur2].kind == 14) { assign_idx = cur2; break; } /* ASSIGN */
            }
        }

        if (ret_idx >= 0 && n[ret_idx].line_start == n[i].line_start && is_stmt_form) {
            /* return <call>; */
            size_t rs = lb;
            while (rs < in_len && (in_src[rs] == ' ' || in_src[rs] == '\t')) rs++;
            if (rs + 6 <= in_len && memcmp(in_src + rs, "return", 6) == 0) {
                size_t after = rs + 6;
                while (after < in_len && (in_src[after] == ' ' || in_src[after] == '\t')) after++;
                /* require call is the expression root */
                size_t after_call = call_end;
                while (after_call < in_len && (in_src[after_call] == ' ' || in_src[after_call] == '\t')) after_call++;
                if (after == call_start && after_call == j) {
                    if (!ret_is_void && !ret_is_structy) {
                        kind = CC_AB_REWRITE_RETURN_CALL;
                        stmt_start = lb;
                    }
                }
            }
            if (kind != CC_AB_REWRITE_RETURN_CALL) {
                /* Not a root `return <call>;` -> rewrite the whole return statement as
                   `tmp = await run_blocking(...); return ...tmp...;` */
                if (!ret_is_void && !ret_is_structy) {
                    /* Find statement end ';' at top-level from the start of the line. */
                    size_t endp = lb;
                    int par = 0, brk = 0, br = 0;
                    int ins = 0; char q = 0;
                    int in_lc = 0, in_bc = 0;
                    for (size_t k = lb; k < in_len; k++) {
                        char ch = in_src[k];
                        char ch2 = (k + 1 < in_len) ? in_src[k + 1] : 0;
                        if (in_lc) { if (ch == '\n') in_lc = 0; continue; }
                        if (in_bc) { if (ch == '*' && ch2 == '/') { in_bc = 0; k++; } continue; }
                        if (ins) { if (ch == '\\' && k + 1 < in_len) { k++; continue; } if (ch == q) ins = 0; continue; }
                        if (ch == '/' && ch2 == '/') { in_lc = 1; k++; continue; }
                        if (ch == '/' && ch2 == '*') { in_bc = 1; k++; continue; }
                        if (ch == '"' || ch == '\'') { ins = 1; q = ch; continue; }
                        if (ch == '(') par++;
                        else if (ch == ')') { if (par) par--; }
                        else if (ch == '[') brk++;
                        else if (ch == ']') { if (brk) brk--; }
                        else if (ch == '{') br++;
                        else if (ch == '}') { if (br) br--; }
                        else if (ch == ';' && par == 0 && brk == 0 && br == 0) { endp = k + 1; break; }
                    }
                    if (endp > lb && endp <= in_len) {
                        kind = CC_AB_REWRITE_RETURN_EXPR_CALL;
                        stmt_start = lb;
                        stmt_end = endp;
                    }
                }
            }
        } else if (assign_idx >= 0 && n[assign_idx].line_start == n[i].line_start && is_stmt_form) {
            /* <lhs> = <call>; */
            const char* op = n[assign_idx].aux_s2;
            if (op && strcmp(op, "=") == 0 && n[assign_idx].aux_s1) {
                lhs_name = n[assign_idx].aux_s1;
                /* require statement starts with lhs_name */
                size_t lhs_len = strlen(lhs_name);
                if (lhs_len > 0 && first + lhs_len <= in_len && memcmp(in_src + first, lhs_name, lhs_len) == 0) {
                    size_t p = first + lhs_len;
                    while (p < in_len && (in_src[p] == ' ' || in_src[p] == '\t')) p++;
                    if (p < in_len && in_src[p] == '=') {
                        p++;
                        while (p < in_len && (in_src[p] == ' ' || in_src[p] == '\t')) p++;
                        size_t after_call = call_end;
                        while (after_call < in_len && (in_src[after_call] == ' ' || in_src[after_call] == '\t')) after_call++;
                        if (p == call_start && after_call == j) {
                            if (!ret_is_void && !ret_is_structy) {
                                kind = CC_AB_REWRITE_ASSIGN_CALL;
                                stmt_start = lb;
                            }
                        }
                    }
                }
            }
            if (kind != CC_AB_REWRITE_ASSIGN_CALL) {
                if (!ret_is_void && !ret_is_structy) {
                    /* Find statement end ';' at top-level from the start of the line. */
                    size_t endp = lb;
                    int par = 0, brk = 0, br = 0;
                    int ins = 0; char q = 0;
                    int in_lc = 0, in_bc = 0;
                    for (size_t k = lb; k < in_len; k++) {
                        char ch = in_src[k];
                        char ch2 = (k + 1 < in_len) ? in_src[k + 1] : 0;
                        if (in_lc) { if (ch == '\n') in_lc = 0; continue; }
                        if (in_bc) { if (ch == '*' && ch2 == '/') { in_bc = 0; k++; } continue; }
                        if (ins) { if (ch == '\\' && k + 1 < in_len) { k++; continue; } if (ch == q) ins = 0; continue; }
                        if (ch == '/' && ch2 == '/') { in_lc = 1; k++; continue; }
                        if (ch == '/' && ch2 == '*') { in_bc = 1; k++; continue; }
                        if (ch == '"' || ch == '\'') { ins = 1; q = ch; continue; }
                        if (ch == '(') par++;
                        else if (ch == ')') { if (par) par--; }
                        else if (ch == '[') brk++;
                        else if (ch == ']') { if (brk) brk--; }
                        else if (ch == '{') br++;
                        else if (ch == '}') { if (br) br--; }
                        else if (ch == ';' && par == 0 && brk == 0 && br == 0) { endp = k + 1; break; }
                    }
                    if (endp > lb && endp <= in_len) {
                        kind = CC_AB_REWRITE_ASSIGN_EXPR_CALL;
                        stmt_start = lb;
                        stmt_end = endp;
                    }
                }
            }
        } else if (kind != CC_AB_REWRITE_AWAIT_OPERAND_CALL) {
            /* plain statement call: require call begins statement token */
            if (is_stmt_form) {
                int ok = 1;
                for (size_t k = first; k < call_start; k++) {
                    char ch = in_src[k];
                    if (ch == ' ' || ch == '\t') continue;
                    ok = 0;
                    break;
                }
                if (ok) {
                    kind = CC_AB_REWRITE_STMT_CALL;
                    stmt_start = lb;
                } else {
                    /* Don't try to rewrite general expression contexts yet (e.g. for-loop headers). */
                    for (int pi = 0; pi < paramc; pi++) free(param_types[pi]);
                    continue;
                }
            } else {
                /* Don't try to rewrite general expression contexts yet (e.g. for-loop headers). */
                for (int pi = 0; pi < paramc; pi++) free(param_types[pi]);
                continue;
            }
        }

        /* If we didn't select a kind (due to conservative checks), skip. */
        if (kind != CC_AB_REWRITE_STMT_CALL &&
            kind != CC_AB_REWRITE_RETURN_CALL &&
            kind != CC_AB_REWRITE_ASSIGN_CALL &&
            kind != CC_AB_REWRITE_RETURN_EXPR_CALL &&
            kind != CC_AB_REWRITE_ASSIGN_EXPR_CALL &&
            kind != CC_AB_REWRITE_AWAIT_OPERAND_CALL) {
            for (int pi = 0; pi < paramc; pi++) free(param_types[pi]);
            continue;
        }
        /* If return/assign checks failed, fall back only if it was a plain stmt-call (already handled above). */
        if (kind == CC_AB_REWRITE_RETURN_CALL || kind == CC_AB_REWRITE_ASSIGN_CALL || kind == CC_AB_REWRITE_STMT_CALL) {
            /* ok */
        }

        if (rep_n == rep_cap) {
            rep_cap = rep_cap ? rep_cap * 2 : 64;
            reps = (Replace*)realloc(reps, (size_t)rep_cap * sizeof(*reps));
            if (!reps) return 0;
        }
        reps[rep_n] = (Replace){
            .start = stmt_start,
            .end = stmt_end,
            .call_start = call_start,
            .call_end = call_end,
            .line_start = n[i].line_start,
            .callee = n[i].aux_s1,
            .lhs_name = lhs_name,
            .kind = kind,
            .argc = paramc,
            .ret_is_ptr = ret_is_ptr,
            .ret_is_void = ret_is_void,
            .ret_is_structy = ret_is_structy,
            .indent_start = indent_start,
            .indent_len = indent_len,
            .batch = NULL,
            .batch_n = 0,
            .tail_kind = 0,
            .tail_call_start = 0,
            .tail_call_end = 0,
            .tail_callee = NULL,
            .tail_lhs_name = NULL,
            .tail_argc = 0,
            .tail_ret_is_ptr = 0,
        };
        for (int pi = 0; pi < paramc; pi++) reps[rep_n].param_types[pi] = param_types[pi];
        for (int pi = paramc; pi < 16; pi++) reps[rep_n].param_types[pi] = NULL;
        for (int pi = 0; pi < 16; pi++) reps[rep_n].tail_param_types[pi] = NULL;
        rep_n++;
    }

    if (rep_n == 0) {
        free(reps);
        return 0;
    }

    /* Filter overlaps (keep outermost). */
    /* Sort ASC by start, tie-break by larger end first (outer spans first). */
    for (int i = 0; i < rep_n - 1; i++) {
        for (int k = i + 1; k < rep_n; k++) {
            if (reps[k].start < reps[i].start ||
                (reps[k].start == reps[i].start && reps[k].end > reps[i].end)) {
                Replace tmp = reps[i];
                reps[i] = reps[k];
                reps[k] = tmp;
            }
        }
    }

    int out_n = 0;
    for (int i = 0; i < rep_n; i++) {
        int overlap = 0;
        for (int j2 = 0; j2 < out_n; j2++) {
            if (!(reps[i].end <= reps[j2].start || reps[i].start >= reps[j2].end)) {
                overlap = 1;
                break;
            }
        }
        if (!overlap) reps[out_n++] = reps[i];
    }
    rep_n = out_n;

    /* Batch consecutive statement-form sync calls in @async (coalescing/batching).
       Only batches CC_AB_REWRITE_STMT_CALL nodes, and only when separated by whitespace/comments. */
    {
        /* Sort ASC for grouping */
        for (int i = 0; i < rep_n - 1; i++) {
            for (int k = i + 1; k < rep_n; k++) {
                if (reps[k].start < reps[i].start) {
                    Replace tmp = reps[i];
                    reps[i] = reps[k];
                    reps[k] = tmp;
                }
            }
        }

        Replace* out = (Replace*)calloc((size_t)rep_n, sizeof(Replace));
        if (out) {
            int on = 0;
            for (int i = 0; i < rep_n; ) {
                if (reps[i].kind != CC_AB_REWRITE_STMT_CALL) {
                    out[on++] = reps[i++];
                    continue;
                }
                int j = i + 1;
                while (j < rep_n &&
                       reps[j].kind == CC_AB_REWRITE_STMT_CALL &&
                       cc__ab_only_ws_comments(in_src, reps[j - 1].end, reps[j].start)) {
                    j++;
                }
                int group_n = j - i;
                int tail_idx = -1;
                if (j < rep_n &&
                    (reps[j].kind == CC_AB_REWRITE_RETURN_CALL || reps[j].kind == CC_AB_REWRITE_ASSIGN_CALL) &&
                    cc__ab_only_ws_comments(in_src, reps[j - 1].end, reps[j].start)) {
                    tail_idx = j;
                    j++;
                }
                if (group_n <= 1 && tail_idx < 0) {
                    out[on++] = reps[i++];
                    continue;
                }

                Replace r = reps[i];
                r.kind = (tail_idx >= 0 && reps[tail_idx].kind == CC_AB_REWRITE_RETURN_CALL)
                             ? CC_AB_REWRITE_BATCH_STMTS_THEN_RETURN
                             : (tail_idx >= 0 && reps[tail_idx].kind == CC_AB_REWRITE_ASSIGN_CALL)
                                   ? CC_AB_REWRITE_BATCH_STMTS_THEN_ASSIGN
                                   : CC_AB_REWRITE_BATCH_STMT_CALLS;
                r.start = reps[i].start;
                r.end = reps[(tail_idx >= 0) ? tail_idx : (j - 1)].end;
                r.call_start = reps[i].call_start;
                r.call_end = reps[(tail_idx >= 0) ? tail_idx : (j - 1)].call_end;
                /* Batch carries per-item metadata; do not keep per-rep param_types to avoid double-free. */
                r.argc = 0;
                for (int pi = 0; pi < 16; pi++) r.param_types[pi] = NULL;
                r.batch_n = group_n;
                r.batch = (CCAutoBlockBatchItem*)calloc((size_t)group_n, sizeof(CCAutoBlockBatchItem));
                if (!r.batch) {
                    out[on++] = reps[i++];
                    continue;
                }
                for (int bi = 0; bi < group_n; bi++) {
                    Replace* src = &reps[i + bi];
                    CCAutoBlockBatchItem* it = &r.batch[bi];
                    it->call_start = src->call_start;
                    it->call_end = src->call_end;
                    it->line_start = src->line_start;
                    it->callee = src->callee;
                    it->argc = src->argc;
                    for (int pi = 0; pi < src->argc; pi++) {
                        it->param_types[pi] = src->param_types[pi];
                        src->param_types[pi] = NULL; /* transfer ownership */
                    }
                }

                if (tail_idx >= 0) {
                    Replace* tail = &reps[tail_idx];
                    r.tail_kind = (tail->kind == CC_AB_REWRITE_RETURN_CALL) ? 1 : 2;
                    r.tail_call_start = tail->call_start;
                    r.tail_call_end = tail->call_end;
                    r.tail_callee = tail->callee;
                    r.tail_lhs_name = tail->lhs_name;
                    r.tail_argc = tail->argc;
                    r.tail_ret_is_ptr = tail->ret_is_ptr;
                    for (int pi = 0; pi < tail->argc; pi++) {
                        r.tail_param_types[pi] = tail->param_types[pi];
                        tail->param_types[pi] = NULL; /* transfer ownership */
                    }
                }
                out[on++] = r;
                i = j;
            }
            free(reps);
            reps = out;
            rep_n = on;
        } else {
            /* if we couldn't allocate, keep unbatched and leave reps as-is */
        }

        /* Sort DESC again for splicing */
        for (int i = 0; i < rep_n - 1; i++) {
            for (int k = i + 1; k < rep_n; k++) {
                if (reps[k].start > reps[i].start) {
                    Replace tmp = reps[i];
                    reps[i] = reps[k];
                    reps[k] = tmp;
                }
            }
        }
    }

    char* cur_src = (char*)malloc(in_len + 1);
    if (!cur_src) { free(reps); return 0; }
    memcpy(cur_src, in_src, in_len);
    cur_src[in_len] = 0;
    size_t cur_len = in_len;

    for (int ri = 0; ri < rep_n; ri++) {
        size_t s = reps[ri].start;
        size_t e = reps[ri].end;
        if (s >= e || e > cur_len) continue;

        /* Extract original statement text */
        size_t stmt_len = e - s;
        char* stmt_txt = (char*)malloc(stmt_len + 1);
        if (!stmt_txt) continue;
        memcpy(stmt_txt, cur_src + s, stmt_len);
        stmt_txt[stmt_len] = 0;

        /* Batched statement calls: collapse adjacent sync calls into one blocking dispatch. */
        if ((reps[ri].kind == CC_AB_REWRITE_BATCH_STMT_CALLS ||
             reps[ri].kind == CC_AB_REWRITE_BATCH_STMTS_THEN_RETURN ||
             reps[ri].kind == CC_AB_REWRITE_BATCH_STMTS_THEN_ASSIGN) &&
            reps[ri].batch && reps[ri].batch_n > 0) {
            char* ind = NULL;
            if (reps[ri].indent_len > 0 && reps[ri].indent_start + reps[ri].indent_len <= cur_len) {
                ind = (char*)malloc(reps[ri].indent_len + 1);
                if (ind) {
                    memcpy(ind, cur_src + reps[ri].indent_start, reps[ri].indent_len);
                    ind[reps[ri].indent_len] = 0;
                }
            }
            const char* I = ind ? ind : "";

            char* repl = NULL;
            size_t repl_len = 0;
            size_t repl_cap = 0;
            /* No extra block wrapper: we use per-line unique temp names to avoid collisions. */

            /* Bind all args in order (in the async context), then do one run_blocking dispatch. */
            for (int bi = 0; bi < reps[ri].batch_n; bi++) {
                const CCAutoBlockBatchItem* it = &reps[ri].batch[bi];
                size_t call_s = it->call_start - s;
                size_t call_e = it->call_end - s;
                if (call_e > stmt_len || call_s >= call_e) continue;
                size_t call_len = call_e - call_s;
                char* call_txt = (char*)malloc(call_len + 1);
                if (!call_txt) continue;
                memcpy(call_txt, stmt_txt + call_s, call_len);
                call_txt[call_len] = 0;

                const char* lpar = strchr(call_txt, '(');
                const char* rpar = strrchr(call_txt, ')');
                if (!lpar || !rpar || rpar <= lpar) { free(call_txt); continue; }
                size_t args_s = (size_t)(lpar - call_txt) + 1;
                size_t args_e = (size_t)(rpar - call_txt);

                size_t arg_starts[16];
                size_t arg_ends[16];
                int argc = 0;
                int par = 0, brk = 0, br = 0;
                int ins = 0; char q = 0;
                size_t cur_a = args_s;
                for (size_t k = args_s; k < args_e; k++) {
                    char ch = call_txt[k];
                    if (ins) {
                        if (ch == '\\' && k + 1 < args_e) { k++; continue; }
                        if (ch == q) ins = 0;
                        continue;
                    }
                    if (ch == '"' || ch == '\'') { ins = 1; q = ch; continue; }
                    if (ch == '(') par++;
                    else if (ch == ')') { if (par) par--; }
                    else if (ch == '[') brk++;
                    else if (ch == ']') { if (brk) brk--; }
                    else if (ch == '{') br++;
                    else if (ch == '}') { if (br) br--; }
                    else if (ch == ',' && par == 0 && brk == 0 && br == 0) {
                        if (argc < (int)(sizeof(arg_starts)/sizeof(arg_starts[0]))) {
                            arg_starts[argc] = cur_a;
                            arg_ends[argc] = k;
                            argc++;
                        }
                        cur_a = k + 1;
                    }
                }
                if (args_s == args_e) argc = 0;
                else if (argc < (int)(sizeof(arg_starts)/sizeof(arg_starts[0]))) {
                    arg_starts[argc] = cur_a;
                    arg_ends[argc] = args_e;
                    argc++;
                }

                for (int ai = 0; ai < argc; ai++) {
                    cc__append_fmt(&repl, &repl_len, &repl_cap, "%s  CCAbIntptr __cc_ab_l%d_b%d_a%d = (CCAbIntptr)(%.*s);\n",
                                   I, reps[ri].line_start, bi, ai,
                                   (int)(arg_ends[ai] - arg_starts[ai]), call_txt + arg_starts[ai]);
                }

                free(call_txt);
            }

            /* If we have a trailing return/assign, bind its args too. */
            if (reps[ri].tail_kind != 0 && reps[ri].tail_call_end > reps[ri].tail_call_start) {
                size_t call_s = reps[ri].tail_call_start - s;
                size_t call_e = reps[ri].tail_call_end - s;
                if (call_e <= stmt_len && call_s < call_e) {
                    size_t call_len = call_e - call_s;
                    char* call_txt = (char*)malloc(call_len + 1);
                    if (call_txt) {
                        memcpy(call_txt, stmt_txt + call_s, call_len);
                        call_txt[call_len] = 0;
                        const char* lpar = strchr(call_txt, '(');
                        const char* rpar = strrchr(call_txt, ')');
                        if (lpar && rpar && rpar > lpar) {
                            size_t args_s = (size_t)(lpar - call_txt) + 1;
                            size_t args_e = (size_t)(rpar - call_txt);
                            size_t arg_starts[16];
                            size_t arg_ends[16];
                            int argc = 0;
                            int par = 0, brk = 0, br = 0;
                            int ins = 0; char q = 0;
                            size_t cur_a = args_s;
                            for (size_t k = args_s; k < args_e; k++) {
                                char ch = call_txt[k];
                                if (ins) {
                                    if (ch == '\\' && k + 1 < args_e) { k++; continue; }
                                    if (ch == q) ins = 0;
                                    continue;
                                }
                                if (ch == '"' || ch == '\'') { ins = 1; q = ch; continue; }
                                if (ch == '(') par++;
                                else if (ch == ')') { if (par) par--; }
                                else if (ch == '[') brk++;
                                else if (ch == ']') { if (brk) brk--; }
                                else if (ch == '{') br++;
                                else if (ch == '}') { if (br) br--; }
                                else if (ch == ',' && par == 0 && brk == 0 && br == 0) {
                                    if (argc < (int)(sizeof(arg_starts)/sizeof(arg_starts[0]))) {
                                        arg_starts[argc] = cur_a;
                                        arg_ends[argc] = k;
                                        argc++;
                                    }
                                    cur_a = k + 1;
                                }
                            }
                            if (args_s == args_e) argc = 0;
                            else if (argc < (int)(sizeof(arg_starts)/sizeof(arg_starts[0]))) {
                                arg_starts[argc] = cur_a;
                                arg_ends[argc] = args_e;
                                argc++;
                            }
                            for (int ai = 0; ai < argc; ai++) {
                                cc__append_fmt(&repl, &repl_len, &repl_cap, "%s  CCAbIntptr __cc_ab_l%d_t_a%d = (CCAbIntptr)(%.*s);\n",
                                               I, reps[ri].line_start, ai,
                                               (int)(arg_ends[ai] - arg_starts[ai]), call_txt + arg_starts[ai]);
                            }
                        }
                        free(call_txt);
                    }
                }
            }

            /* Task-based auto-blocking: create a CCClosure0 value first, then await the task.
               This avoids embedding multiline `() => { ... }` directly inside `await <expr>` which
               interacts badly with later closure-elision + #line resync. */
            cc__append_fmt(&repl, &repl_len, &repl_cap, "%s  CCClosure0 __cc_ab_c_l%d = () => {\n", I, reps[ri].line_start);
            for (int bi = 0; bi < reps[ri].batch_n; bi++) {
                const CCAutoBlockBatchItem* it = &reps[ri].batch[bi];
                cc__append_fmt(&repl, &repl_len, &repl_cap, "%s    %s(", I, it->callee ? it->callee : "");
                for (int ai = 0; ai < it->argc; ai++) {
                    if (ai) cc__append_str(&repl, &repl_len, &repl_cap, ", ");
                    if (it->param_types[ai]) {
                        cc__append_fmt(&repl, &repl_len, &repl_cap, "(%s)__cc_ab_l%d_b%d_a%d", it->param_types[ai], reps[ri].line_start, bi, ai);
                    } else {
                        cc__append_fmt(&repl, &repl_len, &repl_cap, "__cc_ab_l%d_b%d_a%d", reps[ri].line_start, bi, ai);
                    }
                }
                cc__append_str(&repl, &repl_len, &repl_cap, ");\n");
            }
            if (reps[ri].tail_kind != 0) {
                cc__append_fmt(&repl, &repl_len, &repl_cap, "%s    return ", I);
                if (!reps[ri].tail_ret_is_ptr) cc__append_str(&repl, &repl_len, &repl_cap, "(void*)(intptr_t)");
                else cc__append_str(&repl, &repl_len, &repl_cap, "(void*)");
                cc__append_str(&repl, &repl_len, &repl_cap, reps[ri].tail_callee ? reps[ri].tail_callee : "");
                cc__append_str(&repl, &repl_len, &repl_cap, "(");
                for (int ai = 0; ai < reps[ri].tail_argc; ai++) {
                    if (ai) cc__append_str(&repl, &repl_len, &repl_cap, ", ");
                    if (reps[ri].tail_param_types[ai]) {
                        cc__append_fmt(&repl, &repl_len, &repl_cap, "(%s)__cc_ab_l%d_t_a%d", reps[ri].tail_param_types[ai], reps[ri].line_start, ai);
                    } else {
                        cc__append_fmt(&repl, &repl_len, &repl_cap, "__cc_ab_l%d_t_a%d", reps[ri].line_start, ai);
                    }
                }
                cc__append_str(&repl, &repl_len, &repl_cap, ");\n");
            } else {
                cc__append_fmt(&repl, &repl_len, &repl_cap, "%s    return NULL;\n", I);
            }
            cc__append_fmt(&repl, &repl_len, &repl_cap, "%s  };\n", I);
            if (reps[ri].tail_kind == 0) {
                cc__append_fmt(&repl, &repl_len, &repl_cap, "%s  await cc_run_blocking_task_intptr(__cc_ab_c_l%d);\n", I, reps[ri].line_start);
            } else if (reps[ri].tail_kind == 1) {
                cc__append_fmt(&repl, &repl_len, &repl_cap, "%s  return await cc_run_blocking_task_intptr(__cc_ab_c_l%d);\n", I, reps[ri].line_start);
            } else {
                cc__append_fmt(&repl, &repl_len, &repl_cap, "%s  %s = await cc_run_blocking_task_intptr(__cc_ab_c_l%d);\n",
                               I,
                               reps[ri].tail_lhs_name ? reps[ri].tail_lhs_name : "__cc_ab_lhs",
                               reps[ri].line_start);
            }

            free(stmt_txt);
            free(ind);
            if (!repl) continue;

            /* Splice */
            size_t new_len = cur_len - (e - s) + repl_len;
            char* next = (char*)malloc(new_len + 1);
            if (!next) { free(repl); continue; }
            memcpy(next, cur_src, s);
            memcpy(next + s, repl, repl_len);
            memcpy(next + s + repl_len, cur_src + e, cur_len - e);
            next[new_len] = 0;
            free(repl);
            free(cur_src);
            cur_src = next;
            cur_len = new_len;
            continue;
        }

        /* Extract original call text (from statement slice) for arg parsing */
        size_t call_s = reps[ri].call_start - s;
        size_t call_e = reps[ri].call_end - s;
        if (call_e > stmt_len || call_s >= call_e) { free(stmt_txt); continue; }
        size_t call_len = call_e - call_s;
        char* call_txt = (char*)malloc(call_len + 1);
        if (!call_txt) continue;
        memcpy(call_txt, stmt_txt + call_s, call_len);
        call_txt[call_len] = 0;

        /* Build replacement (indent-aware). */
        char* repl = NULL;
        size_t repl_len = 0;
        size_t repl_cap = 0;

        /* Find args inside call_txt. */
        const char* lpar = strchr(call_txt, '(');
        const char* rpar = strrchr(call_txt, ')');
        if (!lpar || !rpar || rpar <= lpar) {
            free(call_txt);
            free(stmt_txt);
            free(repl);
            continue;
        }
        size_t args_s = (size_t)(lpar - call_txt) + 1;
        size_t args_e = (size_t)(rpar - call_txt);

        /* Split args on top-level commas. */
        size_t arg_starts[16];
        size_t arg_ends[16];
        int argc = 0;
        int par = 0, brk = 0, br = 0;
        int ins = 0; char q = 0;
        size_t cur_a = args_s;
        for (size_t k = args_s; k < args_e; k++) {
            char ch = call_txt[k];
            if (ins) {
                if (ch == '\\' && k + 1 < args_e) { k++; continue; }
                if (ch == q) ins = 0;
                continue;
            }
            if (ch == '"' || ch == '\'') { ins = 1; q = ch; continue; }
            if (ch == '(') par++;
            else if (ch == ')') { if (par) par--; }
            else if (ch == '[') brk++;
            else if (ch == ']') { if (brk) brk--; }
            else if (ch == '{') br++;
            else if (ch == '}') { if (br) br--; }
            else if (ch == ',' && par == 0 && brk == 0 && br == 0) {
                if (argc < (int)(sizeof(arg_starts)/sizeof(arg_starts[0]))) {
                    arg_starts[argc] = cur_a;
                    arg_ends[argc] = k;
                    argc++;
                }
                cur_a = k + 1;
            }
        }
        /* Last arg (or empty) */
        if (argc < (int)(sizeof(arg_starts)/sizeof(arg_starts[0]))) {
            arg_starts[argc] = cur_a;
            arg_ends[argc] = args_e;
            argc++;
        }
        /* Handle empty-arg call. */
        if (args_s == args_e) argc = 0;

        /* Indent string from original line. */
        char* ind = NULL;
        if (reps[ri].indent_len > 0 && reps[ri].indent_start + reps[ri].indent_len <= cur_len) {
            ind = (char*)malloc(reps[ri].indent_len + 1);
            if (ind) {
                memcpy(ind, cur_src + reps[ri].indent_start, reps[ri].indent_len);
                ind[reps[ri].indent_len] = 0;
            }
        }
        const char* I = ind ? ind : "";

        for (int ai = 0; ai < argc; ai++) {
            cc__append_fmt(&repl, &repl_len, &repl_cap, "%sCCAbIntptr __cc_ab_l%d_arg%d = (CCAbIntptr)(%.*s);\n",
                           I,
                           reps[ri].line_start,
                           ai,
                           (int)(arg_ends[ai] - arg_starts[ai]), call_txt + arg_starts[ai]);
        }
        if (reps[ri].kind == CC_AB_REWRITE_RETURN_EXPR_CALL || reps[ri].kind == CC_AB_REWRITE_ASSIGN_EXPR_CALL) {
            char tmp_name[96];
            snprintf(tmp_name, sizeof(tmp_name), "__cc_ab_expr_l%d", reps[ri].line_start);

            /* Emit a CCClosure0 value first (avoid embedding closure literal directly in `await <expr>`),
               then await the task into an intptr temp, then emit the original statement with the call
               replaced by that temp. */
            cc__append_fmt(&repl, &repl_len, &repl_cap, "%sCCClosure0 __cc_ab_c_l%d = () => { return ", I, reps[ri].line_start);
            if (!reps[ri].ret_is_ptr) cc__append_str(&repl, &repl_len, &repl_cap, "(void*)(intptr_t)");
            else cc__append_str(&repl, &repl_len, &repl_cap, "(void*)");
            cc__append_str(&repl, &repl_len, &repl_cap, reps[ri].callee);
            cc__append_str(&repl, &repl_len, &repl_cap, "(");
            for (int ai = 0; ai < argc; ai++) {
                if (ai) cc__append_str(&repl, &repl_len, &repl_cap, ", ");
                if (ai < reps[ri].argc && reps[ri].param_types[ai]) {
                    cc__append_fmt(&repl, &repl_len, &repl_cap, "(%s)__cc_ab_l%d_arg%d", reps[ri].param_types[ai], reps[ri].line_start, ai);
                } else {
                    cc__append_fmt(&repl, &repl_len, &repl_cap, "__cc_ab_l%d_arg%d", reps[ri].line_start, ai);
                }
            }
            cc__append_str(&repl, &repl_len, &repl_cap, "); };\n");
            cc__append_fmt(&repl, &repl_len, &repl_cap, "%sintptr_t %s = 0;\n", I, tmp_name);
            cc__append_fmt(&repl, &repl_len, &repl_cap, "%s%s = await cc_run_blocking_task_intptr(__cc_ab_c_l%d);\n",
                           I, tmp_name, reps[ri].line_start);

            /* Original statement with call replaced by tmp. */
            cc__append_n(&repl, &repl_len, &repl_cap, stmt_txt, call_s);
            cc__append_str(&repl, &repl_len, &repl_cap, tmp_name);
            cc__append_n(&repl, &repl_len, &repl_cap, stmt_txt + call_e, stmt_len - call_e);
            if (repl_len > 0 && repl[repl_len - 1] != '\n') cc__append_str(&repl, &repl_len, &repl_cap, "\n");
        } else if (reps[ri].kind == CC_AB_REWRITE_AWAIT_OPERAND_CALL) {
            /* For `await chan_*` forms, keep the user's `await` but rewrite the call operand
               to `cc_run_blocking_task_intptr(__cc_ab_c_lN)` so async lowering sees a task. */
            cc__append_fmt(&repl, &repl_len, &repl_cap, "%sCCClosure0 __cc_ab_c_l%d = () => { return ", I, reps[ri].line_start);
            if (!reps[ri].ret_is_ptr) cc__append_str(&repl, &repl_len, &repl_cap, "(void*)(intptr_t)");
            else cc__append_str(&repl, &repl_len, &repl_cap, "(void*)");
            cc__append_str(&repl, &repl_len, &repl_cap, reps[ri].callee);
            cc__append_str(&repl, &repl_len, &repl_cap, "(");
            for (int ai = 0; ai < argc; ai++) {
                if (ai) cc__append_str(&repl, &repl_len, &repl_cap, ", ");
                if (ai < reps[ri].argc && reps[ri].param_types[ai]) {
                    cc__append_fmt(&repl, &repl_len, &repl_cap, "(%s)__cc_ab_l%d_arg%d", reps[ri].param_types[ai], reps[ri].line_start, ai);
                } else {
                    cc__append_fmt(&repl, &repl_len, &repl_cap, "__cc_ab_l%d_arg%d", reps[ri].line_start, ai);
                }
            }
            cc__append_str(&repl, &repl_len, &repl_cap, "); };\n");

            /* Original statement with call replaced by cc_run_blocking_task_intptr(__cc_ab_c_lN). */
            cc__append_n(&repl, &repl_len, &repl_cap, stmt_txt, call_s);
            cc__append_fmt(&repl, &repl_len, &repl_cap, "cc_run_blocking_task_intptr(__cc_ab_c_l%d)", reps[ri].line_start);
            cc__append_n(&repl, &repl_len, &repl_cap, stmt_txt + call_e, stmt_len - call_e);
            if (repl_len > 0 && repl[repl_len - 1] != '\n') cc__append_str(&repl, &repl_len, &repl_cap, "\n");
        } else if (reps[ri].kind == CC_AB_REWRITE_STMT_CALL) {
            cc__append_fmt(&repl, &repl_len, &repl_cap, "%s  CCClosure0 __cc_ab_c_l%d = () => { ", I, reps[ri].line_start);
            cc__append_str(&repl, &repl_len, &repl_cap, reps[ri].callee);
            cc__append_str(&repl, &repl_len, &repl_cap, "(");
            for (int ai = 0; ai < argc; ai++) {
                if (ai) cc__append_str(&repl, &repl_len, &repl_cap, ", ");
                if (ai < reps[ri].argc && reps[ri].param_types[ai]) {
                    cc__append_fmt(&repl, &repl_len, &repl_cap, "(%s)__cc_ab_l%d_arg%d", reps[ri].param_types[ai], reps[ri].line_start, ai);
                } else {
                    cc__append_fmt(&repl, &repl_len, &repl_cap, "__cc_ab_l%d_arg%d", reps[ri].line_start, ai);
                }
            }
            cc__append_str(&repl, &repl_len, &repl_cap, "); return NULL; };\n");
            cc__append_fmt(&repl, &repl_len, &repl_cap, "%s  await cc_run_blocking_task_intptr(__cc_ab_c_l%d);\n", I, reps[ri].line_start);
        } else {
            if (reps[ri].kind == CC_AB_REWRITE_RETURN_CALL) {
                cc__append_fmt(&repl, &repl_len, &repl_cap, "%s  CCClosure0 __cc_ab_c_l%d = () => { return ", I, reps[ri].line_start);
                if (!reps[ri].ret_is_ptr) cc__append_str(&repl, &repl_len, &repl_cap, "(void*)(intptr_t)");
                else cc__append_str(&repl, &repl_len, &repl_cap, "(void*)");
                cc__append_str(&repl, &repl_len, &repl_cap, reps[ri].callee);
                cc__append_str(&repl, &repl_len, &repl_cap, "(");
                for (int ai = 0; ai < argc; ai++) {
                    if (ai) cc__append_str(&repl, &repl_len, &repl_cap, ", ");
                    if (ai < reps[ri].argc && reps[ri].param_types[ai]) {
                        cc__append_fmt(&repl, &repl_len, &repl_cap, "(%s)__cc_ab_l%d_arg%d", reps[ri].param_types[ai], reps[ri].line_start, ai);
                    } else {
                        cc__append_fmt(&repl, &repl_len, &repl_cap, "__cc_ab_l%d_arg%d", reps[ri].line_start, ai);
                    }
                }
                cc__append_str(&repl, &repl_len, &repl_cap, "); };\n");
                cc__append_fmt(&repl, &repl_len, &repl_cap, "%s  return await cc_run_blocking_task_intptr(__cc_ab_c_l%d);\n", I, reps[ri].line_start);
            } else if (reps[ri].kind == CC_AB_REWRITE_ASSIGN_CALL && reps[ri].lhs_name) {
                cc__append_fmt(&repl, &repl_len, &repl_cap, "%s  CCClosure0 __cc_ab_c_l%d = () => { return ", I, reps[ri].line_start);
                if (!reps[ri].ret_is_ptr) cc__append_str(&repl, &repl_len, &repl_cap, "(void*)(intptr_t)");
                else cc__append_str(&repl, &repl_len, &repl_cap, "(void*)");
                cc__append_str(&repl, &repl_len, &repl_cap, reps[ri].callee);
                cc__append_str(&repl, &repl_len, &repl_cap, "(");
                for (int ai = 0; ai < argc; ai++) {
                    if (ai) cc__append_str(&repl, &repl_len, &repl_cap, ", ");
                    if (ai < reps[ri].argc && reps[ri].param_types[ai]) {
                        cc__append_fmt(&repl, &repl_len, &repl_cap, "(%s)__cc_ab_l%d_arg%d", reps[ri].param_types[ai], reps[ri].line_start, ai);
                    } else {
                        cc__append_fmt(&repl, &repl_len, &repl_cap, "__cc_ab_l%d_arg%d", reps[ri].line_start, ai);
                    }
                }
                cc__append_str(&repl, &repl_len, &repl_cap, "); };\n");
                cc__append_fmt(&repl, &repl_len, &repl_cap, "%s  %s = await cc_run_blocking_task_intptr(__cc_ab_c_l%d);\n",
                               I, reps[ri].lhs_name, reps[ri].line_start);
            }
        }

        free(call_txt);
        free(stmt_txt);
        free(ind);
        if (!repl) continue;

        /* Splice */
        size_t new_len = cur_len - (e - s) + repl_len;
        char* next = (char*)malloc(new_len + 1);
        if (!next) { free(repl); continue; }
        memcpy(next, cur_src, s);
        memcpy(next + s, repl, repl_len);
        memcpy(next + s + repl_len, cur_src + e, cur_len - e);
        next[new_len] = 0;
        free(repl);
        free(cur_src);
        cur_src = next;
        cur_len = new_len;
    }

    for (int i = 0; i < rep_n; i++) {
        for (int j = 0; j < reps[i].argc; j++) free(reps[i].param_types[j]);
        if ((reps[i].kind == CC_AB_REWRITE_BATCH_STMT_CALLS ||
             reps[i].kind == CC_AB_REWRITE_BATCH_STMTS_THEN_RETURN ||
             reps[i].kind == CC_AB_REWRITE_BATCH_STMTS_THEN_ASSIGN) &&
            reps[i].batch) {
            for (int bi = 0; bi < reps[i].batch_n; bi++) {
                for (int pj = 0; pj < reps[i].batch[bi].argc; pj++) {
                    free(reps[i].batch[bi].param_types[pj]);
                }
            }
            free(reps[i].batch);
            reps[i].batch = NULL;
        }
        if (reps[i].tail_kind != 0) {
            for (int pj = 0; pj < reps[i].tail_argc; pj++) free(reps[i].tail_param_types[pj]);
        }
    }
    free(reps);
    *out_src = cur_src;
    *out_len = cur_len;
    return 1;
}

