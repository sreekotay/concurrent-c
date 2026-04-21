/*
 * Autoblock Pass: Automatically wraps blocking calls in @async functions.
 *
 * Inside @async functions, calls to non-async/non-noblock functions are wrapped
 * with `cc_run_blocking_task_intptr(...)` so they produce CCTaskIntptr values
 * that the async state machine can poll/await.
 *
 * Rewrite kinds:
 *   - Statement-form calls:  `blocking_fn(...)` -> `await cc_run_blocking_task_intptr(...)`
 *   - Return-value calls:    `return blocking_fn(...)` -> hoisted temp + await
 *   - Assignment calls:      `x = blocking_fn(...)` -> hoisted temp + await
 *   - Await operand calls:   `await chan_send(...)` -> wrapped to produce CCTaskIntptr
 *
 * Legacy/Transition Note (CC_AB_REWRITE_AWAIT_OPERAND_CALL):
 *   Channel operations (chan_send, chan_recv, etc.) are currently blocking runtime
 *   calls. When used with explicit `await`, they must be wrapped. The preferred
 *   future path is to use task-returning variants (cc_channel_send_task, cc_channel_recv_task)
 *   which return CCTaskIntptr directly, eliminating the need for this wrapping.
 *   See: spec-progress/CCTASKINTPTR_CHANNEL_OPS_DESIGN.md
 */

#include "pass_autoblock.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "comptime/symbols.h"
#include "result_spec.h"
#include "util/path.h"
#include "util/text.h"
#include "visitor/pass_common.h"

enum {
    CC_FN_ATTR_ASYNC = 1u << 0,
    CC_FN_ATTR_NOBLOCK = 1u << 1,
    CC_FN_ATTR_LATENCY_SENSITIVE = 1u << 2,
};

/* Alias shared types for local use */
typedef CCNodeView NodeView;

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

/* ---- pass-specific helpers ---- */

static int cc__lookup_func_attrs(const CCASTRoot* root,
                                 const CCVisitorCtx* ctx,
                                 const char* name,
                                 unsigned int* out_attrs) {
    if (out_attrs) *out_attrs = 0;
    if (!root || !name) return 0;
    const NodeView* n = (const NodeView*)root->nodes;
    for (int i = 0; i < root->node_count; i++) {
        if (n[i].kind != 12) continue; /* CC_AST_NODE_DECL_ITEM (functions) */
        if (!n[i].aux_s1 || strcmp(n[i].aux_s1, name) != 0) continue;
        if (!cc_pass_node_in_tu(root, ctx, n[i].file)) continue;
        if (out_attrs) *out_attrs = (unsigned int)n[i].aux2;
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

static int cc__append_autoblock_capture_name_fmt(char** buf,
                                                 size_t* len,
                                                 size_t* cap,
                                                 int* first,
                                                 const char* fmt,
                                                 ...) {
    char tmp[256];
    va_list ap;
    if (!buf || !len || !cap || !first || !fmt) return 0;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n <= 0) return 0;
    tmp[sizeof(tmp) - 1] = '\0';
    if (!*first && !cc__append_str(buf, len, cap, ", ")) return 0;
    *first = 0;
    return cc__append_str(buf, len, cap, tmp);
}

/* Emit the `__cc_ab_l%d_arg%d` capture-list entry for one positional arg,
 * unless the arg was identified as a pure string literal — in which case
 * it's inlined into the closure body directly (no temp captured). */
static void cc__ab_emit_arg_cap(char** buf, size_t* len, size_t* cap,
                                int* first, const int* arg_is_str_lit,
                                int line_start, int ai) {
    if (arg_is_str_lit && arg_is_str_lit[ai]) return;
    cc__append_autoblock_capture_name_fmt(buf, len, cap, first,
                                          "__cc_ab_l%d_arg%d", line_start, ai);
}

/* Emit the body-side reference to one positional arg: the captured
 * temp name, or the original source literal for string-literal args. */
static void cc__ab_emit_arg_use(char** buf, size_t* len, size_t* cap,
                                const int* arg_is_str_lit,
                                const char* call_txt,
                                const size_t* arg_starts,
                                const size_t* arg_ends,
                                int line_start, int ai) {
    if (arg_is_str_lit && arg_is_str_lit[ai]) {
        cc__append_n(buf, len, cap,
                     call_txt + arg_starts[ai],
                     arg_ends[ai] - arg_starts[ai]);
    } else {
        cc__append_fmt(buf, len, cap, "__cc_ab_l%d_arg%d", line_start, ai);
    }
}

/* Return 1 if the source slice `s[a..b)` is a pure string literal — possibly
 * adjacent-concatenated (C joins `"foo" "bar"`), possibly prefixed with an
 * encoding marker (`L"..."`, `u"..."`, `u8"..."`, `U"..."`), with only
 * whitespace and C-style comments between segments.  The autoblock pass
 * uses this to avoid lifting format-string arguments through a captured
 * variable: the original `fprintf(stderr, "msg %d\n", x)` stays format-aware
 * in the synthesized closure body, so `-Wformat-security` does not fire and
 * the generated code reads like the source.  Anything even slightly more
 * complex than a chain of string literals (a cast, a macro, a concatenated
 * non-literal identifier) falls back to the capture path — correctness over
 * cleverness. */
static int cc__ab_arg_is_string_literal(const char* s, size_t a, size_t b) {
    if (!s || a >= b) return 0;
    size_t i = a;
    int segments = 0;
    for (;;) {
        while (i < b) {
            char c = s[i];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') { i++; continue; }
            if (c == '/' && i + 1 < b && s[i + 1] == '/') {
                i += 2; while (i < b && s[i] != '\n') i++; continue;
            }
            if (c == '/' && i + 1 < b && s[i + 1] == '*') {
                i += 2;
                while (i + 1 < b && !(s[i] == '*' && s[i + 1] == '/')) i++;
                if (i + 1 < b) i += 2;
                continue;
            }
            break;
        }
        if (i >= b) break;
        if (i + 2 < b && s[i] == 'u' && s[i + 1] == '8' && s[i + 2] == '"') i += 2;
        else if (i + 1 < b && (s[i] == 'L' || s[i] == 'u' || s[i] == 'U') && s[i + 1] == '"') i += 1;
        if (i >= b || s[i] != '"') return 0;
        i++;
        while (i < b && s[i] != '"') {
            if (s[i] == '\\' && i + 1 < b) { i += 2; continue; }
            i++;
        }
        if (i >= b) return 0;
        i++;
        segments++;
    }
    return segments > 0;
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

static int cc__is_typed_chan_send_wrapper(const char* name) {
    size_t n;
    if (!name) return 0;
    if (strncmp(name, "CCChanTx_", 9) != 0) return 0;
    n = strlen(name);
    return n > 5 && strcmp(name + n - 5, "_send") == 0;
}

static int cc__is_typed_chan_recv_wrapper(const char* name) {
    size_t n;
    if (!name) return 0;
    if (strncmp(name, "CCChanRx_", 9) != 0) return 0;
    n = strlen(name);
    return n > 5 && strcmp(name + n - 5, "_recv") == 0;
}

static int cc__is_intptr_compatible_type(const char* type_str) {
    char buf[256];
    size_t n = 0;
    size_t i = 0;
    if (!type_str) return 0;
    while (*type_str == ' ' || *type_str == '\t') type_str++;
    while (type_str[i] && n + 1 < sizeof(buf)) {
        char c = type_str[i++];
        if (c == ' ' || c == '\t') {
            if (n > 0 && buf[n - 1] != ' ') buf[n++] = ' ';
            continue;
        }
        buf[n++] = c;
    }
    while (n > 0 && buf[n - 1] == ' ') n--;
    buf[n] = '\0';
    if (buf[0] == '\0') return 0;

    if (strcmp(buf, "char") == 0 ||
        strcmp(buf, "signed char") == 0 ||
        strcmp(buf, "unsigned char") == 0 ||
        strcmp(buf, "short") == 0 ||
        strcmp(buf, "short int") == 0 ||
        strcmp(buf, "unsigned short") == 0 ||
        strcmp(buf, "unsigned short int") == 0 ||
        strcmp(buf, "int") == 0 ||
        strcmp(buf, "unsigned") == 0 ||
        strcmp(buf, "unsigned int") == 0 ||
        strcmp(buf, "long") == 0 ||
        strcmp(buf, "long int") == 0 ||
        strcmp(buf, "unsigned long") == 0 ||
        strcmp(buf, "unsigned long int") == 0 ||
        strcmp(buf, "long long") == 0 ||
        strcmp(buf, "long long int") == 0 ||
        strcmp(buf, "unsigned long long") == 0 ||
        strcmp(buf, "unsigned long long int") == 0 ||
        strcmp(buf, "intptr_t") == 0 ||
        strcmp(buf, "uintptr_t") == 0 ||
        strcmp(buf, "size_t") == 0 ||
        strcmp(buf, "ssize_t") == 0 ||
        strcmp(buf, "ptrdiff_t") == 0 ||
        strcmp(buf, "bool") == 0) {
        return 1;
    }
    if (strncmp(buf, "enum ", 5) == 0) return 1;
    return 0;
}

static const char* cc__normalize_autoblock_decl_type(const char* type_str, char* buf, size_t buf_cap) {
    const char* p = NULL;
    size_t prefix_len = 0;
    if (!type_str || !buf || buf_cap == 0) return type_str;
    p = strstr(type_str, "struct __CCGenericError");
    if (!p) p = strstr(type_str, "__CCGenericError");
    if (!p) return type_str;
    prefix_len = (size_t)(p - type_str);
    if (prefix_len + strlen("CCError") + strlen(p) + 1 >= buf_cap) return type_str;
    memcpy(buf, type_str, prefix_len);
    buf[prefix_len] = '\0';
    strcat(buf, "CCError");
    if (strncmp(p, "struct __CCGenericError", 23) == 0) strcat(buf, p + 23);
    else strcat(buf, p + 16);
    return buf;
}

static const char* cc__known_autoblock_param_type(const char* callee, int arg_index) {
    if (!callee) return NULL;
    if (arg_index == 0 &&
        (strcmp(callee, "cc_run_blocking_task") == 0 ||
         strcmp(callee, "cc_run_blocking_closure0") == 0 ||
         strcmp(callee, "cc_run_blocking_closure0_ptr") == 0 ||
         strcmp(callee, "cc_thread_spawn_closure0") == 0 ||
         strcmp(callee, "cc_fiber_spawn_closure0") == 0)) {
        return "CCClosure0";
    }
    if (arg_index == 1 &&
        (strcmp(callee, "cc_nursery_spawn_closure0") == 0 ||
         strcmp(callee, "cc_nursery_spawn_child_closure0") == 0 ||
         strcmp(callee, "cc_thread_spawn_closure0_legacy") == 0)) {
        return "CCClosure0";
    }
    return NULL;
}

static const char* cc__canonicalize_autoblock_value_type(const char* type_str, char* buf, size_t buf_cap) {
    const char* bang = NULL;
    const char* ok_s = NULL;
    const char* ok_e = NULL;
    const char* err_s = NULL;
    const char* err_e = NULL;
    char mangled_ok[128];
    char mangled_err[128];
    char norm_buf[256];
    const char* norm = cc__normalize_autoblock_decl_type(type_str, norm_buf, sizeof(norm_buf));
    if (!norm || !buf || buf_cap == 0) return norm;

    /* (retired) The trailing `?` -> `CCOptional_T` autoblock canonicalization
     * used to live here. Optionals are gone; stray `T?` spellings are rejected
     * earlier by the preprocess diagnostic. */

    bang = strchr(norm, '!');
    if (!bang || bang[1] == '=') return norm;
    ok_s = norm;
    ok_e = bang;
    while (ok_s < ok_e && isspace((unsigned char)*ok_s)) ok_s++;
    while (ok_e > ok_s && isspace((unsigned char)ok_e[-1])) ok_e--;

    err_s = bang + 1;
    while (*err_s == ' ' || *err_s == '\t') err_s++;
    if (*err_s == '>') {
        err_s++;
        while (*err_s == ' ' || *err_s == '\t') err_s++;
        if (*err_s == '(') {
            err_s++;
            err_e = strchr(err_s, ')');
            if (!err_e) err_e = err_s + strlen(err_s);
        } else {
            err_e = err_s + strlen(err_s);
        }
    } else {
        err_e = err_s;
        while (*err_e && (isalnum((unsigned char)*err_e) || *err_e == '_')) err_e++;
    }
    while (err_s < err_e && isspace((unsigned char)*err_s)) err_s++;
    while (err_e > err_s && isspace((unsigned char)err_e[-1])) err_e--;
    if (ok_e <= ok_s || err_e <= err_s) return norm;
    cc_result_spec_mangle_type(ok_s, (size_t)(ok_e - ok_s), mangled_ok, sizeof(mangled_ok));
    cc_result_spec_mangle_type(err_s, (size_t)(err_e - err_s), mangled_err, sizeof(mangled_err));
    if (!mangled_ok[0] || !mangled_err[0]) return norm;
    snprintf(buf, buf_cap, "CCResult_%s_%s", mangled_ok, mangled_err);
    return buf;
}

static const char* cc__select_autoblock_box_type(const char* decl_type, const char* ret_type) {
    if (decl_type && (strstr(decl_type, "!>") != NULL || strchr(decl_type, '?') != NULL)) {
        return decl_type;
    }
    return ret_type ? ret_type : decl_type;
}

static size_t cc__scan_call_paren_end(const char* src, size_t len, size_t lparen) {
    int par = 0, brk = 0, br = 0;
    int ins = 0, in_chr = 0, in_lc = 0, in_bc = 0;
    char q = 0;
    if (!src || lparen >= len || src[lparen] != '(') return 0;
    for (size_t i = lparen + 1; i < len; i++) {
        char ch = src[i];
        char ch2 = (i + 1 < len) ? src[i + 1] : 0;
        if (in_lc) { if (ch == '\n') in_lc = 0; continue; }
        if (in_bc) { if (ch == '*' && ch2 == '/') { in_bc = 0; i++; } continue; }
        if (ins) {
            if (ch == '\\' && i + 1 < len) { i++; continue; }
            if (ch == q) ins = 0;
            continue;
        }
        if (in_chr) {
            if (ch == '\\' && i + 1 < len) { i++; continue; }
            if (ch == '\'') in_chr = 0;
            continue;
        }
        if (ch == '/' && ch2 == '/') { in_lc = 1; i++; continue; }
        if (ch == '/' && ch2 == '*') { in_bc = 1; i++; continue; }
        if (ch == '"') { ins = 1; q = ch; continue; }
        if (ch == '\'') { in_chr = 1; continue; }
        if (ch == '(') par++;
        else if (ch == ')') {
            if (par == 0 && brk == 0 && br == 0) return i + 1;
            if (par) par--;
        } else if (ch == '[') brk++;
        else if (ch == ']') { if (brk) brk--; }
        else if (ch == '{') br++;
        else if (ch == '}') { if (br) br--; }
    }
    return 0;
}

static int cc__relocate_call_span_on_line(const char* src,
                                          size_t len,
                                          size_t line_start,
                                          size_t line_end,
                                          size_t hint,
                                          const char* callee,
                                          size_t* out_start,
                                          size_t* out_end) {
    size_t best_start = 0;
    size_t best_end = 0;
    size_t best_dist = (size_t)-1;
    size_t callee_n = 0;
    if (!src || !callee || !callee[0] || !out_start || !out_end) return 0;
    if (line_start >= len) return 0;
    if (line_end > len) line_end = len;
    if (line_end <= line_start) return 0;
    callee_n = strlen(callee);
    for (size_t i = line_start; i + callee_n <= line_end; i++) {
        size_t after = i + callee_n;
        size_t lparen = 0;
        size_t call_end = 0;
        size_t dist = 0;
        if (memcmp(src + i, callee, callee_n) != 0) continue;
        if (i > line_start && (isalnum((unsigned char)src[i - 1]) || src[i - 1] == '_')) continue;
        if (after < len && (isalnum((unsigned char)src[after]) || src[after] == '_')) continue;
        while (after < len && (src[after] == ' ' || src[after] == '\t' || src[after] == '\r' || src[after] == '\n')) after++;
        if (after >= len || src[after] != '(') continue;
        lparen = after;
        call_end = cc__scan_call_paren_end(src, len, lparen);
        if (!call_end) continue;
        dist = (i > hint) ? (i - hint) : (hint - i);
        if (!best_end || dist < best_dist) {
            best_start = i;
            best_end = call_end;
            best_dist = dist;
        }
    }
    if (!best_end) return 0;
    *out_start = best_start;
    *out_end = best_end;
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
        CC_AB_REWRITE_DECL_INIT_CALL = 8,
        /* LEGACY: For `await chan_*` - wraps the blocking call to produce CCTaskIntptr.
           Future: Use cc_channel_send_task/cc_channel_recv_task which return CCTaskIntptr directly,
           eliminating the need for this wrapping. See: CCTASKINTPTR_CHANNEL_OPS_DESIGN.md */
        CC_AB_REWRITE_AWAIT_OPERAND_CALL = 9,
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
        char* lhs_name; /* owned; for assign/decl-init */
        char* decl_type; /* owned; for decl-init */
        CCAutoBlockRewriteKind kind;
        int raw_call; /* reserved */
        int is_chan_recv; /* 1 if chan_recv, 0 if chan_send */
        int argc;
        int ret_is_ptr;
        int ret_is_void;
        int ret_is_structy;
        char* ret_type; /* owned */
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
        int is_ufcs = (n[i].aux2 & 4) != 0;
        if (is_ufcs) continue;
        if (!n[i].aux_s1) continue; /* callee name */
        if (!cc_pass_node_in_tu(root, ctx, n[i].file)) continue;
        /* Nested closure literals are lowered by a later pass; rewriting sync calls
           inside them here can emit `await` into non-async closure bodies. */
        if (cc__node_is_descendant_of_kind(root, n, i, 9 /* CC_AST_NODE_CLOSURE */)) continue;

        /* Calls inside `await ...` are async call sites; skip unless it's a channel op.
           Channel ops return CCTaskIntptr via cc_chan_*_task, so they must be awaited.
           Note: chan_send/chan_recv are macros that expand to cc_chan_send/cc_chan_recv. */
        int is_under_await = cc__node_is_descendant_of_kind(root, n, i, 6 /* CC_AST_NODE_AWAIT */);
        int is_chan_op = (strcmp(n[i].aux_s1, "chan_send") == 0 ||
                          strcmp(n[i].aux_s1, "chan_recv") == 0 ||
                          strcmp(n[i].aux_s1, "chan_send_take") == 0 ||
                          strcmp(n[i].aux_s1, "chan_send_take_ptr") == 0 ||
                          strcmp(n[i].aux_s1, "chan_send_take_slice") == 0 ||
                          /* Macro expansions: */
                          strcmp(n[i].aux_s1, "cc_chan_send") == 0 ||
                          strcmp(n[i].aux_s1, "cc_chan_recv") == 0 ||
                          strcmp(n[i].aux_s1, "cc_chan_send_take") == 0 ||
                          strcmp(n[i].aux_s1, "cc_chan_send_take_slice") == 0 ||
                          cc__is_typed_chan_send_wrapper(n[i].aux_s1) ||
                          cc__is_typed_chan_recv_wrapper(n[i].aux_s1));
        if (is_under_await && !is_chan_op) continue;
        /* In async context, chan ops MUST be awaited - they return CCTaskIntptr. */
        int is_chan_recv = (strcmp(n[i].aux_s1, "chan_recv") == 0 ||
                           strcmp(n[i].aux_s1, "cc_chan_recv") == 0 ||
                           cc__is_typed_chan_recv_wrapper(n[i].aux_s1));

        /* Find enclosing function decl-item and check @async attr. */
        int cur = n[i].parent;
        const char* owner = NULL;
        unsigned int owner_attrs = 0;
        while (cur >= 0 && cur < root->node_count) {
            if (n[cur].kind == 12 && n[cur].aux_s1 && n[cur].aux_s2 && strchr(n[cur].aux_s2, '(') &&
                cc_pass_node_in_tu(root, ctx, n[cur].file)) {
                owner = n[cur].aux_s1;
                owner_attrs = (unsigned int)n[cur].aux2;
                break;
            }
            cur = n[cur].parent;
        }
        if (getenv("CC_DEBUG_AUTOBLOCK_CALLS")) {
            fprintf(stderr, "autoblock call node line=%d callee=%s owner=%s owner_async=%d\n",
                    n[i].line_start,
                    n[i].aux_s1 ? n[i].aux_s1 : "<null>",
                    owner ? owner : "<null>",
                    (owner_attrs & CC_FN_ATTR_ASYNC) != 0);
        }
        if (!owner || ((owner_attrs & CC_FN_ATTR_ASYNC) == 0)) continue;

        /* NOTE: Channel ops in @async are wrapped automatically by autoblock,
           making them cooperative without explicit await. This matches spec behavior.
           Future: Consider requiring explicit await for clarity (requires better macro detection). */

        /* Only skip known-nonblocking callees; if we don't know attrs, assume blocking. */
        unsigned int callee_attrs = 0;
        if (!cc__lookup_func_attrs(root, ctx, n[i].aux_s1, &callee_attrs)) {
            (void)cc_symbols_lookup_fn_attrs(ctx->symbols, n[i].aux_s1, &callee_attrs);
        }
        if (getenv("CC_DEBUG_AUTOBLOCK_CALLS")) {
            fprintf(stderr, "  attrs async=%d noblock=%d under_await=%d chan=%d\n",
                    (callee_attrs & CC_FN_ATTR_ASYNC) != 0,
                    (callee_attrs & CC_FN_ATTR_NOBLOCK) != 0,
                    is_under_await,
                    is_chan_op);
        }
        if (callee_attrs & CC_FN_ATTR_ASYNC) continue;
        if (callee_attrs & CC_FN_ATTR_NOBLOCK) continue;
        if (strcmp(n[i].aux_s1, "cc_channel_pair") == 0) continue;

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

        /* is_ufcs already handled above */

        /* Line + indent info */
        size_t lb = cc__offset_of_line_1based(in_src, in_len, n[i].line_start);
        size_t first = lb;
        while (first < in_len && (in_src[first] == ' ' || in_src[first] == '\t')) first++;
        size_t indent_start = lb;
        size_t indent_len = first > lb ? (first - lb) : 0;
        {
            size_t line_end = cc__offset_of_line_1based(in_src, in_len, n[i].line_start + 1);
            if (line_end <= lb || line_end > in_len) line_end = in_len;
            if (!cc__relocate_call_span_on_line(in_src, in_len, lb, line_end, call_start, n[i].aux_s1, &call_start, &call_end)) {
                size_t search_start = lb;
                size_t search_end = line_end;
                while (search_end < in_len && search_end > search_start &&
                       in_src[search_end - 1] != ';' && in_src[search_end - 1] != '{' && in_src[search_end - 1] != '}') {
                    size_t next = cc__offset_of_line_1based(in_src, in_len, n[i].line_start + 2);
                    if (next <= search_end || next > in_len) break;
                    search_end = next;
                }
                (void)cc__relocate_call_span_on_line(in_src, in_len, search_start, search_end, call_start, n[i].aux_s1, &call_start, &call_end);
            }
        }
        /* Next non-ws token after relocated call. */
        size_t j = call_end;
        while (j < in_len && (in_src[j] == ' ' || in_src[j] == '\t' || in_src[j] == '\n' || in_src[j] == '\r')) j++;
        int is_stmt_form = (j < in_len && in_src[j] == ';') ? 1 : 0;
        size_t stmt_end = is_stmt_form ? (j + 1) : call_end;
        if (getenv("CC_DEBUG_AUTOBLOCK_CALLS")) {
            size_t dbg_n = call_end > call_start ? (call_end - call_start) : 0;
            if (dbg_n > 120) dbg_n = 120;
            fprintf(stderr, "  span='%.*s' stmt=%d next='%c'\n",
                    (int)dbg_n, in_src + call_start, is_stmt_form,
                    (j < in_len ? in_src[j] : '?'));
        }

        /* Prefer FUNC/PARAM stub nodes; fallback to DECL_ITEM text signature. */
        const char* ret_str = NULL;
        char* param_types[16] = {0};
        int paramc = 0;
        int ret_is_ptr = 0, ret_is_void = 0, ret_is_structy = 0;

        int found_func = 0;
        for (int k = 0; k < root->node_count; k++) {
            if (n[k].kind != 17) continue; /* CC_AST_NODE_FUNC */
            if (!n[k].aux_s1 || strcmp(n[k].aux_s1, n[i].aux_s1) != 0) continue;
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
            if (strchr(ret_str, '*')) ret_is_ptr = 1;
            if (!ret_is_ptr &&
                (strstr(ret_str, "struct") ||
                 strstr(ret_str, "union") ||
                 strstr(ret_str, "CCSlice") ||
                 !cc__is_intptr_compatible_type(ret_str))) {
                ret_is_structy = 1;
            }
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
                            if (!ret_is_void) {
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
        }
        if (kind == CC_AB_REWRITE_STMT_CALL && is_stmt_form) {
            /* declaration initializer: `TYPE lhs = call(...);` */
            size_t decl_first = first;
            while (decl_first > 0) {
                size_t p = decl_first;
                while (p > 0) {
                    char ch = in_src[p - 1];
                    if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n') {
                        p--;
                        continue;
                    }
                    break;
                }
                if (p == 0) {
                    decl_first = 0;
                    break;
                }
                char prev = in_src[p - 1];
                if (prev == ';' || prev == '{' || prev == '}') break;
                size_t prev_line = p;
                while (prev_line > 0 && in_src[prev_line - 1] != '\n') prev_line--;
                if (prev_line == decl_first) break;
                decl_first = prev_line;
            }
            size_t eq = call_start;
            while (eq > decl_first) {
                eq--;
                if (in_src[eq] == '=') break;
            }
            if (eq > decl_first && in_src[eq] == '=') {
                size_t after_eq = eq + 1;
                while (after_eq < call_start &&
                       (in_src[after_eq] == ' ' || in_src[after_eq] == '\t' ||
                        in_src[after_eq] == '\r' || in_src[after_eq] == '\n')) after_eq++;
                if (after_eq == call_start) {
                    size_t lhs_end = eq;
                    while (lhs_end > decl_first && (in_src[lhs_end - 1] == ' ' || in_src[lhs_end - 1] == '\t')) lhs_end--;
                    size_t lhs_start = lhs_end;
                    while (lhs_start > decl_first &&
                           (isalnum((unsigned char)in_src[lhs_start - 1]) || in_src[lhs_start - 1] == '_')) lhs_start--;
                    if (lhs_start < lhs_end) {
                        int type_ok = 1;
                        size_t type_end = lhs_start;
                        while (type_end > decl_first && (in_src[type_end - 1] == ' ' || in_src[type_end - 1] == '\t')) type_end--;
                        if (type_end > decl_first) {
                            for (size_t tk = decl_first; tk < type_end; tk++) {
                                char ch = in_src[tk];
                                if (!(isalnum((unsigned char)ch) || ch == '_' || ch == ' ' || ch == '\t' ||
                                      ch == '*' || ch == '!' || ch == '>' || ch == '<' || ch == '(' ||
                                      ch == ')' || ch == ',' || ch == '[' || ch == ']' || ch == ':' ||
                                      ch == '?' || ch == '~')) {
                                    type_ok = 0;
                                    break;
                                }
                            }
                            if (type_ok) {
                                size_t lhs_n = lhs_end - lhs_start;
                                size_t type_n = type_end - decl_first;
                                char* lhs_dup = (char*)malloc(lhs_n + 1);
                                char* type_dup = (char*)malloc(type_n + 1);
                                if (lhs_dup && type_dup) {
                                    memcpy(lhs_dup, in_src + lhs_start, lhs_n);
                                    lhs_dup[lhs_n] = 0;
                                    memcpy(type_dup, in_src + decl_first, type_n);
                                    type_dup[type_n] = 0;
                                    lhs_name = lhs_dup;
                                    kind = CC_AB_REWRITE_DECL_INIT_CALL;
                                    stmt_start = decl_first;
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
                                        .lhs_name = lhs_dup,
                                        .decl_type = type_dup,
                                        .kind = kind,
                                        .raw_call = 0,
                                        .is_chan_recv = is_chan_recv,
                                        .argc = paramc,
                                        .ret_is_ptr = ret_is_ptr,
                                        .ret_is_void = ret_is_void,
                                        .ret_is_structy = ret_is_structy,
                                        .ret_type = ret_str ? strdup(ret_str) : NULL,
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
                                    if (getenv("CC_DEBUG_AUTOBLOCK_MATCHES")) {
                                        fprintf(stderr, "autoblock decl-init line=%d callee=%s decl='%s' lhs='%s'\n",
                                                n[i].line_start,
                                                n[i].aux_s1 ? n[i].aux_s1 : "<null>",
                                                type_dup,
                                                lhs_dup);
                                    }
                                    rep_n++;
                                    continue;
                                }
                                free(lhs_dup);
                                free(type_dup);
                            }
                        }
                    }
                }
            }
        }

        if (kind == CC_AB_REWRITE_STMT_CALL) {
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
            kind != CC_AB_REWRITE_DECL_INIT_CALL &&
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
            .lhs_name = lhs_name ? strdup(lhs_name) : NULL,
            .decl_type = NULL,
            .kind = kind,
            .raw_call = 0,
            .is_chan_recv = is_chan_recv,
            .argc = paramc,
            .ret_is_ptr = ret_is_ptr,
            .ret_is_void = ret_is_void,
            .ret_is_structy = ret_is_structy,
            .ret_type = ret_str ? strdup(ret_str) : NULL,
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
        if (getenv("CC_DEBUG_AUTOBLOCK_MATCHES")) {
            fprintf(stderr, "autoblock kind=%d line=%d callee=%s lhs=%s\n",
                    kind,
                    n[i].line_start,
                    n[i].aux_s1 ? n[i].aux_s1 : "<null>",
                    lhs_name ? lhs_name : "<null>");
        }
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

                /* Comment/string-aware `(` scan so a block comment
                 * containing a `(` doesn't pull the scan off the real
                 * call-opening paren. */
                size_t lpo = cc_find_char_top_level(call_txt, 0, call_len, '(');
                const char* lpar = (lpo < call_len) ? (call_txt + lpo) : NULL;
                const char* rpar = lpar ? strrchr(call_txt, ')') : NULL;
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
                        size_t lpo2 = cc_find_char_top_level(call_txt, 0, call_len, '(');
                        const char* lpar = (lpo2 < call_len) ? (call_txt + lpo2) : NULL;
                        const char* rpar = lpar ? strrchr(call_txt, ')') : NULL;
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
            cc__append_fmt(&repl, &repl_len, &repl_cap, "%s  CCClosure0 __cc_ab_c_l%d = () => [", I, reps[ri].line_start);
            {
                int first_cap = 1;
                for (int bi = 0; bi < reps[ri].batch_n; bi++) {
                    const CCAutoBlockBatchItem* it = &reps[ri].batch[bi];
                    for (int ai = 0; ai < it->argc; ai++) {
                        cc__append_autoblock_capture_name_fmt(&repl, &repl_len, &repl_cap, &first_cap,
                                                              "__cc_ab_l%d_b%d_a%d",
                                                              reps[ri].line_start, bi, ai);
                    }
                }
                for (int ai = 0; ai < reps[ri].tail_argc; ai++) {
                    cc__append_autoblock_capture_name_fmt(&repl, &repl_len, &repl_cap, &first_cap,
                                                          "__cc_ab_l%d_t_a%d",
                                                          reps[ri].line_start, ai);
                }
            }
            cc__append_str(&repl, &repl_len, &repl_cap, "] {\n");
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

        /* Find args inside call_txt — comment/string-aware. */
        size_t lpo3 = cc_find_char_top_level(call_txt, 0, call_len, '(');
        const char* lpar = (lpo3 < call_len) ? (call_txt + lpo3) : NULL;
        const char* rpar = lpar ? strrchr(call_txt, ')') : NULL;
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

        /* Trim whitespace on each arg in-place and note whether the payload
         * is a pure string literal.  Literal args are inlined into the
         * synthesized closure body instead of flowing through a captured
         * temporary: this keeps format-string arguments to `printf`/`fprintf`
         * visible to the compiler so `-Wformat`/-Wformat-security/ don't
         * fire on generated autoblock wrappers. */
        int arg_is_str_lit[16] = {0};
        for (int ai = 0; ai < argc; ai++) {
            size_t a = arg_starts[ai];
            size_t e = arg_ends[ai];
            while (a < e && (call_txt[a] == ' ' || call_txt[a] == '\t' ||
                             call_txt[a] == '\n' || call_txt[a] == '\r')) a++;
            while (e > a && (call_txt[e - 1] == ' ' || call_txt[e - 1] == '\t' ||
                             call_txt[e - 1] == '\n' || call_txt[e - 1] == '\r')) e--;
            arg_starts[ai] = a;
            arg_ends[ai] = e;
            arg_is_str_lit[ai] = cc__ab_arg_is_string_literal(call_txt, a, e);
        }

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
            size_t arg_s = arg_starts[ai];
            size_t arg_e = arg_ends[ai];
            /* String-literal args are inlined into the closure body, so
             * they don't need a captured temporary. */
            if (arg_is_str_lit[ai]) continue;
            {
                char norm_type[256];
                const char* forced_type = cc__known_autoblock_param_type(reps[ri].callee, ai);
                const char* decl_type = NULL;
                if (forced_type) {
                    decl_type = forced_type;
                } else if (ai < reps[ri].argc && reps[ri].param_types[ai]) {
                    decl_type = cc__normalize_autoblock_decl_type(reps[ri].param_types[ai], norm_type, sizeof(norm_type));
                }
                if (decl_type) {
                    if (getenv("CC_DEBUG_AUTOBLOCK_PARAM_TYPES")) {
                        fprintf(stderr, "autoblock callee=%s arg=%d raw='%s' norm='%s'\n",
                                reps[ri].callee ? reps[ri].callee : "<null>",
                                ai,
                                reps[ri].param_types[ai] ? reps[ri].param_types[ai] : "<null>",
                                decl_type ? decl_type : "<null>");
                    }
                    cc__append_fmt(&repl, &repl_len, &repl_cap, "%s%s __cc_ab_l%d_arg%d = ",
                                   I, decl_type, reps[ri].line_start, ai);
                    cc__append_n(&repl, &repl_len, &repl_cap, call_txt + arg_s, arg_e - arg_s);
                    cc__append_str(&repl, &repl_len, &repl_cap, ";\n");
                } else {
                    cc__append_fmt(&repl, &repl_len, &repl_cap, "%sCCAbIntptr __cc_ab_l%d_arg%d = (CCAbIntptr)(",
                                   I, reps[ri].line_start, ai);
                    cc__append_n(&repl, &repl_len, &repl_cap, call_txt + arg_s, arg_e - arg_s);
                    cc__append_str(&repl, &repl_len, &repl_cap, ");\n");
                }
            }
        }
        if (reps[ri].kind == CC_AB_REWRITE_RETURN_EXPR_CALL || reps[ri].kind == CC_AB_REWRITE_ASSIGN_EXPR_CALL) {
            char tmp_name[96];
            snprintf(tmp_name, sizeof(tmp_name), "__cc_ab_expr_l%d", reps[ri].line_start);

            /* Emit a CCClosure0 value first (avoid embedding closure literal directly in `await <expr>`),
               then await the task into an intptr temp, then emit the original statement with the call
               replaced by that temp. */
            cc__append_fmt(&repl, &repl_len, &repl_cap, "%sCCClosure0 __cc_ab_c_l%d = () => [", I, reps[ri].line_start);
            {
                int first_cap = 1;
                for (int ai = 0; ai < argc; ai++) {
                    cc__ab_emit_arg_cap(&repl, &repl_len, &repl_cap, &first_cap,
                                        arg_is_str_lit, reps[ri].line_start, ai);
                }
            }
            cc__append_str(&repl, &repl_len, &repl_cap, "] { return ");
            if (!reps[ri].ret_is_ptr) cc__append_str(&repl, &repl_len, &repl_cap, "(void*)(intptr_t)");
            else cc__append_str(&repl, &repl_len, &repl_cap, "(void*)");
            cc__append_str(&repl, &repl_len, &repl_cap, reps[ri].callee);
            cc__append_str(&repl, &repl_len, &repl_cap, "(");
            for (int ai = 0; ai < argc; ai++) {
                if (ai) cc__append_str(&repl, &repl_len, &repl_cap, ", ");
                cc__ab_emit_arg_use(&repl, &repl_len, &repl_cap, arg_is_str_lit,
                                    call_txt, arg_starts, arg_ends, reps[ri].line_start, ai);
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
            /* For `await chan_*` forms, rewrite the call to cc_channel_*_task() which returns CCTaskIntptr.
               The await keyword is preserved; async lowering handles the CCTaskIntptr await.
               - chan_send(ch, val)    -> cc_channel_send_task(ch, &val, sizeof(val))
               - chan_recv(ch, &out)   -> cc_channel_recv_task(ch, out, sizeof(*out))
             */
            /* Emit arg temporaries - they may be complex expressions. */
            for (int ai = 0; ai < argc; ai++) {
                size_t arg_s = arg_starts[ai];
                size_t arg_e = arg_ends[ai];
                /* Trim whitespace */
                while (arg_s < arg_e && (call_txt[arg_s] == ' ' || call_txt[arg_s] == '\t')) arg_s++;
                while (arg_e > arg_s && (call_txt[arg_e - 1] == ' ' || call_txt[arg_e - 1] == '\t')) arg_e--;
                if (ai < reps[ri].argc && reps[ri].param_types[ai]) {
                    cc__append_fmt(&repl, &repl_len, &repl_cap, "%s%s __cc_ab_l%d_arg%d = ", I, reps[ri].param_types[ai], reps[ri].line_start, ai);
                } else {
                    /* Use __auto_type for C11 type inference */
                    cc__append_fmt(&repl, &repl_len, &repl_cap, "%s__auto_type __cc_ab_l%d_arg%d = ", I, reps[ri].line_start, ai);
                }
                cc__append_n(&repl, &repl_len, &repl_cap, call_txt + arg_s, arg_e - arg_s);
                cc__append_str(&repl, &repl_len, &repl_cap, ";\n");
            }

            /* Emit the task-returning call in place of the original call. */
            cc__append_n(&repl, &repl_len, &repl_cap, stmt_txt, call_s);
            if (cc__is_typed_chan_send_wrapper(reps[ri].callee) ||
                cc__is_typed_chan_recv_wrapper(reps[ri].callee)) {
                cc__append_str(&repl, &repl_len, &repl_cap,
                               reps[ri].is_chan_recv ? "cc_channel_recv_task(("
                                                     : "cc_channel_send_task((");
                if (argc >= 1) {
                    cc__append_fmt(&repl, &repl_len, &repl_cap, "__cc_ab_l%d_arg0", reps[ri].line_start);
                }
                cc__append_str(&repl, &repl_len, &repl_cap, ").raw, ");
                if (argc >= 2) {
                    cc__append_fmt(&repl, &repl_len, &repl_cap, "__cc_ab_l%d_arg1", reps[ri].line_start);
                }
                if (reps[ri].is_chan_recv) {
                    cc__append_str(&repl, &repl_len, &repl_cap, ", sizeof(*(");
                    if (argc >= 2) {
                        cc__append_fmt(&repl, &repl_len, &repl_cap, "__cc_ab_l%d_arg1", reps[ri].line_start);
                    }
                    cc__append_str(&repl, &repl_len, &repl_cap, ")))");
                } else {
                    cc__append_str(&repl, &repl_len, &repl_cap, ", &(");
                    if (argc >= 2) {
                        cc__append_fmt(&repl, &repl_len, &repl_cap, "__cc_ab_l%d_arg1", reps[ri].line_start);
                    }
                    cc__append_str(&repl, &repl_len, &repl_cap, "), sizeof(");
                    if (argc >= 2) {
                        cc__append_fmt(&repl, &repl_len, &repl_cap, "__cc_ab_l%d_arg1", reps[ri].line_start);
                    }
                    cc__append_str(&repl, &repl_len, &repl_cap, "))");
                }
            } else if (reps[ri].is_chan_recv) {
                /* chan_recv(ch, &out) -> cc_channel_recv_task(ch, out, sizeof(*out)) */
                cc__append_str(&repl, &repl_len, &repl_cap, "cc_channel_recv_task(");
                if (argc >= 1) {
                    cc__append_fmt(&repl, &repl_len, &repl_cap, "__cc_ab_l%d_arg0", reps[ri].line_start);
                }
                cc__append_str(&repl, &repl_len, &repl_cap, ", ");
                if (argc >= 2) {
                    cc__append_fmt(&repl, &repl_len, &repl_cap, "__cc_ab_l%d_arg1", reps[ri].line_start);
                }
                cc__append_str(&repl, &repl_len, &repl_cap, ", sizeof(*(");
                if (argc >= 2) {
                    cc__append_fmt(&repl, &repl_len, &repl_cap, "__cc_ab_l%d_arg1", reps[ri].line_start);
                }
                cc__append_str(&repl, &repl_len, &repl_cap, ")))");
            } else {
                /* chan_send(ch, val) -> cc_channel_send_task(ch, &val, sizeof(val)) */
                cc__append_str(&repl, &repl_len, &repl_cap, "cc_channel_send_task(");
                if (argc >= 1) {
                    cc__append_fmt(&repl, &repl_len, &repl_cap, "__cc_ab_l%d_arg0", reps[ri].line_start);
                }
                cc__append_str(&repl, &repl_len, &repl_cap, ", &(");
                if (argc >= 2) {
                    cc__append_fmt(&repl, &repl_len, &repl_cap, "__cc_ab_l%d_arg1", reps[ri].line_start);
                }
                cc__append_str(&repl, &repl_len, &repl_cap, "), sizeof(");
                if (argc >= 2) {
                    cc__append_fmt(&repl, &repl_len, &repl_cap, "__cc_ab_l%d_arg1", reps[ri].line_start);
                }
                cc__append_str(&repl, &repl_len, &repl_cap, "))");
            }
            cc__append_n(&repl, &repl_len, &repl_cap, stmt_txt + call_e, stmt_len - call_e);
            if (repl_len > 0 && repl[repl_len - 1] != '\n') cc__append_str(&repl, &repl_len, &repl_cap, "\n");
        } else if (reps[ri].kind == CC_AB_REWRITE_STMT_CALL) {
            cc__append_fmt(&repl, &repl_len, &repl_cap, "%s  CCClosure0 __cc_ab_c_l%d = () => [", I, reps[ri].line_start);
            {
                int first_cap = 1;
                for (int ai = 0; ai < argc; ai++) {
                    cc__ab_emit_arg_cap(&repl, &repl_len, &repl_cap, &first_cap,
                                        arg_is_str_lit, reps[ri].line_start, ai);
                }
            }
            cc__append_str(&repl, &repl_len, &repl_cap, "] { ");
            cc__append_str(&repl, &repl_len, &repl_cap, reps[ri].callee);
            cc__append_str(&repl, &repl_len, &repl_cap, "(");
            for (int ai = 0; ai < argc; ai++) {
                if (ai) cc__append_str(&repl, &repl_len, &repl_cap, ", ");
                cc__ab_emit_arg_use(&repl, &repl_len, &repl_cap, arg_is_str_lit,
                                    call_txt, arg_starts, arg_ends, reps[ri].line_start, ai);
            }
            cc__append_str(&repl, &repl_len, &repl_cap, "); return NULL; };\n");
            cc__append_fmt(&repl, &repl_len, &repl_cap, "%s  await cc_run_blocking_task_intptr(__cc_ab_c_l%d);\n", I, reps[ri].line_start);
        } else {
            if (reps[ri].kind == CC_AB_REWRITE_RETURN_CALL) {
                cc__append_fmt(&repl, &repl_len, &repl_cap, "%s  CCClosure0 __cc_ab_c_l%d = () => [", I, reps[ri].line_start);
                {
                    int first_cap = 1;
                    for (int ai = 0; ai < argc; ai++) {
                        cc__ab_emit_arg_cap(&repl, &repl_len, &repl_cap, &first_cap,
                                            arg_is_str_lit, reps[ri].line_start, ai);
                    }
                }
                cc__append_str(&repl, &repl_len, &repl_cap, "] { return ");
                if (!reps[ri].ret_is_ptr) cc__append_str(&repl, &repl_len, &repl_cap, "(void*)(intptr_t)");
                else cc__append_str(&repl, &repl_len, &repl_cap, "(void*)");
                cc__append_str(&repl, &repl_len, &repl_cap, reps[ri].callee);
                cc__append_str(&repl, &repl_len, &repl_cap, "(");
                for (int ai = 0; ai < argc; ai++) {
                    if (ai) cc__append_str(&repl, &repl_len, &repl_cap, ", ");
                    cc__ab_emit_arg_use(&repl, &repl_len, &repl_cap, arg_is_str_lit,
                                    call_txt, arg_starts, arg_ends, reps[ri].line_start, ai);
                }
                cc__append_str(&repl, &repl_len, &repl_cap, "); };\n");
                cc__append_fmt(&repl, &repl_len, &repl_cap, "%s  return await cc_run_blocking_task_intptr(__cc_ab_c_l%d);\n", I, reps[ri].line_start);
            } else if (reps[ri].kind == CC_AB_REWRITE_ASSIGN_CALL && reps[ri].lhs_name) {
                if (reps[ri].ret_is_structy) {
                    char box_type_buf[256];
                    const char* box_type = cc__canonicalize_autoblock_value_type(
                        reps[ri].ret_type ? reps[ri].ret_type : "void*",
                        box_type_buf, sizeof(box_type_buf));
                    cc__append_fmt(&repl, &repl_len, &repl_cap, "%s  %s cc_ab_ret_l%d;\n",
                                   I, box_type ? box_type : "void*", reps[ri].line_start);
                    cc__append_fmt(&repl, &repl_len, &repl_cap, "%s  %s* cc_ab_retp_l%d = &cc_ab_ret_l%d;\n",
                                   I, box_type ? box_type : "void*", reps[ri].line_start, reps[ri].line_start);
                    cc__append_fmt(&repl, &repl_len, &repl_cap, "%s  CCClosure0 __cc_ab_c_l%d = () => [",
                                   I, reps[ri].line_start);
                    {
                        int first_cap = 1;
                        cc__append_autoblock_capture_name_fmt(&repl, &repl_len, &repl_cap, &first_cap,
                                                              "cc_ab_retp_l%d",
                                                              reps[ri].line_start);
                        for (int ai = 0; ai < argc; ai++) {
                            cc__ab_emit_arg_cap(&repl, &repl_len, &repl_cap, &first_cap,
                                                arg_is_str_lit, reps[ri].line_start, ai);
                        }
                    }
                    cc__append_fmt(&repl, &repl_len, &repl_cap, "] { *cc_ab_retp_l%d = ",
                                   reps[ri].line_start);
                    cc__append_str(&repl, &repl_len, &repl_cap, reps[ri].callee);
                    cc__append_str(&repl, &repl_len, &repl_cap, "(");
                    for (int ai = 0; ai < argc; ai++) {
                        if (ai) cc__append_str(&repl, &repl_len, &repl_cap, ", ");
                        cc__ab_emit_arg_use(&repl, &repl_len, &repl_cap, arg_is_str_lit,
                                    call_txt, arg_starts, arg_ends, reps[ri].line_start, ai);
                    }
                    cc__append_str(&repl, &repl_len, &repl_cap, "); return NULL; };\n");
                    cc__append_fmt(&repl, &repl_len, &repl_cap,
                                   "%s  await cc_run_blocking_task_intptr(__cc_ab_c_l%d);\n",
                                   I, reps[ri].line_start, reps[ri].line_start);
                    cc__append_fmt(&repl, &repl_len, &repl_cap,
                                   "%s  %s = cc_ab_ret_l%d;\n",
                                   I, reps[ri].lhs_name,
                                   reps[ri].line_start);
                } else {
                    cc__append_fmt(&repl, &repl_len, &repl_cap, "%s  CCClosure0 __cc_ab_c_l%d = () => [", I, reps[ri].line_start);
                    {
                        int first_cap = 1;
                        for (int ai = 0; ai < argc; ai++) {
                            cc__ab_emit_arg_cap(&repl, &repl_len, &repl_cap, &first_cap,
                                                arg_is_str_lit, reps[ri].line_start, ai);
                        }
                    }
                    cc__append_str(&repl, &repl_len, &repl_cap, "] { return ");
                    if (!reps[ri].ret_is_ptr) cc__append_str(&repl, &repl_len, &repl_cap, "(void*)(intptr_t)");
                    else cc__append_str(&repl, &repl_len, &repl_cap, "(void*)");
                    cc__append_str(&repl, &repl_len, &repl_cap, reps[ri].callee);
                    cc__append_str(&repl, &repl_len, &repl_cap, "(");
                    for (int ai = 0; ai < argc; ai++) {
                        if (ai) cc__append_str(&repl, &repl_len, &repl_cap, ", ");
                        cc__ab_emit_arg_use(&repl, &repl_len, &repl_cap, arg_is_str_lit,
                                    call_txt, arg_starts, arg_ends, reps[ri].line_start, ai);
                    }
                    cc__append_str(&repl, &repl_len, &repl_cap, "); };\n");
                    cc__append_fmt(&repl, &repl_len, &repl_cap, "%s  %s = ", I, reps[ri].lhs_name);
                    if (reps[ri].ret_is_ptr) {
                        cc__append_str(&repl, &repl_len, &repl_cap, "(void*)");
                    } else if (reps[ri].ret_type) {
                        cc__append_fmt(&repl, &repl_len, &repl_cap, "(%s)", reps[ri].ret_type);
                    }
                    cc__append_fmt(&repl, &repl_len, &repl_cap, "await cc_run_blocking_task_intptr(__cc_ab_c_l%d);\n",
                                   reps[ri].line_start);
                }
            } else if (reps[ri].kind == CC_AB_REWRITE_DECL_INIT_CALL && reps[ri].lhs_name && reps[ri].decl_type) {
                if (reps[ri].ret_is_structy) {
                    char box_type_buf[256];
                    char decl_emit_type_buf[256];
                    const char* box_src = cc__select_autoblock_box_type(reps[ri].decl_type, reps[ri].ret_type);
                    const char* box_type = cc__canonicalize_autoblock_value_type(box_src, box_type_buf, sizeof(box_type_buf));
                    const char* decl_emit_type = cc__canonicalize_autoblock_value_type(reps[ri].decl_type, decl_emit_type_buf, sizeof(decl_emit_type_buf));
                    cc__append_fmt(&repl, &repl_len, &repl_cap, "%s  %s cc_ab_ret_l%d;\n",
                                   I, box_type ? box_type : reps[ri].decl_type, reps[ri].line_start);
                    cc__append_fmt(&repl, &repl_len, &repl_cap, "%s  %s* cc_ab_retp_l%d = &cc_ab_ret_l%d;\n",
                                   I, box_type ? box_type : reps[ri].decl_type, reps[ri].line_start, reps[ri].line_start);
                    cc__append_fmt(&repl, &repl_len, &repl_cap, "%s  CCClosure0 __cc_ab_c_l%d = () => [",
                                   I, reps[ri].line_start);
                    {
                        int first_cap = 1;
                        cc__append_autoblock_capture_name_fmt(&repl, &repl_len, &repl_cap, &first_cap,
                                                              "cc_ab_retp_l%d",
                                                              reps[ri].line_start);
                        for (int ai = 0; ai < argc; ai++) {
                            cc__ab_emit_arg_cap(&repl, &repl_len, &repl_cap, &first_cap,
                                                arg_is_str_lit, reps[ri].line_start, ai);
                        }
                    }
                    cc__append_fmt(&repl, &repl_len, &repl_cap, "] { *cc_ab_retp_l%d = ",
                                   reps[ri].line_start);
                    cc__append_str(&repl, &repl_len, &repl_cap, reps[ri].callee);
                    cc__append_str(&repl, &repl_len, &repl_cap, "(");
                    for (int ai = 0; ai < argc; ai++) {
                        if (ai) cc__append_str(&repl, &repl_len, &repl_cap, ", ");
                        cc__ab_emit_arg_use(&repl, &repl_len, &repl_cap, arg_is_str_lit,
                                    call_txt, arg_starts, arg_ends, reps[ri].line_start, ai);
                    }
                    cc__append_str(&repl, &repl_len, &repl_cap, "); return NULL; };\n");
                    cc__append_fmt(&repl, &repl_len, &repl_cap,
                                   "%s  await cc_run_blocking_task_intptr(__cc_ab_c_l%d);\n",
                                   I, reps[ri].line_start, reps[ri].line_start);
                    cc__append_fmt(&repl, &repl_len, &repl_cap,
                                   "%s  %s %s = cc_ab_ret_l%d;\n",
                                   I, decl_emit_type ? decl_emit_type : reps[ri].decl_type, reps[ri].lhs_name,
                                   reps[ri].line_start);
                } else {
                    cc__append_fmt(&repl, &repl_len, &repl_cap, "%s  CCClosure0 __cc_ab_c_l%d = () => [", I, reps[ri].line_start);
                    {
                        int first_cap = 1;
                        for (int ai = 0; ai < argc; ai++) {
                            cc__ab_emit_arg_cap(&repl, &repl_len, &repl_cap, &first_cap,
                                                arg_is_str_lit, reps[ri].line_start, ai);
                        }
                    }
                    cc__append_str(&repl, &repl_len, &repl_cap, "] { return ");
                    if (!reps[ri].ret_is_ptr) cc__append_str(&repl, &repl_len, &repl_cap, "(void*)(intptr_t)");
                    else cc__append_str(&repl, &repl_len, &repl_cap, "(void*)");
                    cc__append_str(&repl, &repl_len, &repl_cap, reps[ri].callee);
                    cc__append_str(&repl, &repl_len, &repl_cap, "(");
                    for (int ai = 0; ai < argc; ai++) {
                        if (ai) cc__append_str(&repl, &repl_len, &repl_cap, ", ");
                        cc__ab_emit_arg_use(&repl, &repl_len, &repl_cap, arg_is_str_lit,
                                    call_txt, arg_starts, arg_ends, reps[ri].line_start, ai);
                    }
                    cc__append_str(&repl, &repl_len, &repl_cap, "); };\n");
                    cc__append_fmt(&repl, &repl_len, &repl_cap, "%s  %s %s = ", I, reps[ri].decl_type, reps[ri].lhs_name);
                    if (reps[ri].ret_is_ptr && reps[ri].ret_type) {
                        cc__append_fmt(&repl, &repl_len, &repl_cap, "(%s)(void*)", reps[ri].ret_type);
                    } else if (reps[ri].ret_type && !reps[ri].ret_is_ptr) {
                        cc__append_fmt(&repl, &repl_len, &repl_cap, "(%s)", reps[ri].ret_type);
                    }
                    cc__append_fmt(&repl, &repl_len, &repl_cap, "await cc_run_blocking_task_intptr(__cc_ab_c_l%d);\n",
                                   reps[ri].line_start);
                }
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
        free(reps[i].lhs_name);
        free(reps[i].decl_type);
        free(reps[i].ret_type);
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

/* NEW: Collect autoblocking edits into EditBuffer.
   NOTE: This pass has complex batching and nesting logic.
   For now, this function runs the rewrite and uses a coarse-grained edit.
   Future: refactor to collect edits directly. */
int cc__collect_autoblocking_edits(const CCASTRoot* root,
                                   const CCVisitorCtx* ctx,
                                   CCEditBuffer* eb) {
    if (!root || !ctx || !ctx->symbols || !eb || !eb->src) return 0;

    char* rewritten = NULL;
    size_t rewritten_len = 0;
    int r = cc__rewrite_autoblocking_calls_with_nodes(root, ctx, eb->src, eb->src_len, &rewritten, &rewritten_len);
    if (r <= 0 || !rewritten) return 0;

    if (rewritten_len != eb->src_len || memcmp(rewritten, eb->src, eb->src_len) != 0) {
        if (cc_edit_buffer_add(eb, 0, eb->src_len, rewritten, 80, "autoblock") == 0) {
            free(rewritten);
            return 1;
        }
    }
    free(rewritten);
    return 0;
}