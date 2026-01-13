#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "visitor/visitor.h"

#ifndef CC_TCC_EXT_AVAILABLE
#error "CC_TCC_EXT_AVAILABLE is required (patched TCC stub-AST required)."
#endif

/* Stub AST kinds from patched TCC (see third_party/tcc/tcc.h). */
enum {
    CC_AST_NODE_UNKNOWN = 0,
    CC_AST_NODE_DECL = 1,
    CC_AST_NODE_BLOCK = 2,
    CC_AST_NODE_STMT = 3,
    CC_AST_NODE_ARENA = 4,
    CC_AST_NODE_CALL = 5,
    CC_AST_NODE_AWAIT = 6,
    CC_AST_NODE_SEND_TAKE = 7,
    CC_AST_NODE_SUBSLICE = 8,
    CC_AST_NODE_CLOSURE = 9,
    CC_AST_NODE_IDENT = 10,
    CC_AST_NODE_CONST = 11,
    CC_AST_NODE_DECL_ITEM = 12,
    CC_AST_NODE_MEMBER = 13,
    CC_AST_NODE_ASSIGN = 14,
    CC_AST_NODE_RETURN = 15,
    CC_AST_NODE_PARAM = 16,
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

static const char* cc__basename(const char* path) {
    if (!path) return NULL;
    const char* p = strrchr(path, '/');
    return p ? p + 1 : path;
}

static int cc__same_source_file(const char* a, const char* b) {
    if (!a || !b) return 0;
    const char* as = cc__basename(a);
    const char* bs = cc__basename(b);
    return strcmp(a, b) == 0 || strcmp(as, bs) == 0;
}

static int cc__node_in_this_tu(const CCASTRoot* root, const CCVisitorCtx* ctx, const char* node_file) {
    if (!ctx || !ctx->input_path || !node_file) return 0;
    if (cc__same_source_file(ctx->input_path, node_file)) return 1;
    if (root && root->lowered_path && cc__same_source_file(root->lowered_path, node_file)) return 1;
    return 0;
}

static size_t cc__offset_of_line_1based(const char* s, size_t n, int line_1based) {
    if (!s) return 0;
    if (line_1based <= 1) return 0;
    int line = 1;
    for (size_t i = 0; i < n; i++) {
        if (line == line_1based) return i;
        if (s[i] == '\n') line++;
    }
    return n;
}

static size_t cc__offset_of_line_col_1based(const char* s, size_t n, int line_1based, int col_1based) {
    size_t ls = cc__offset_of_line_1based(s, n, line_1based);
    if (ls >= n) return n;
    if (col_1based <= 1) return ls;
    size_t p = ls + (size_t)(col_1based - 1);
    if (p > n) p = n;
    return p;
}

static size_t cc__node_start_off(const char* src, size_t len, const NodeView* nd) {
    if (!nd || nd->line_start <= 0) return 0;
    return cc__offset_of_line_col_1based(src, len, nd->line_start, nd->col_start > 0 ? nd->col_start : 1);
}

static size_t cc__node_end_off(const char* src, size_t len, const NodeView* nd) {
    if (!nd || nd->line_end <= 0) return 0;
    return cc__offset_of_line_col_1based(src, len, nd->line_end, nd->col_end > 0 ? nd->col_end : 1);
}

static int cc__is_ident_start(char c) {
    return (c == '_') || ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'));
}

static int cc__is_ident_char(char c) {
    return cc__is_ident_start(c) || (c >= '0' && c <= '9');
}

static const char* cc__skip_ws(const char* p) {
    while (p && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
    return p;
}

static void cc__sb_append_vfmt(char** io_s, size_t* io_len, size_t* io_cap, const char* fmt, va_list ap) {
    if (!io_s || !io_len || !io_cap || !fmt) return;
    if (!*io_s) {
        *io_cap = 256;
        *io_s = (char*)malloc(*io_cap);
        if (!*io_s) return;
        (*io_s)[0] = 0;
        *io_len = 0;
    }
    for (;;) {
        size_t avail = (*io_cap > *io_len) ? (*io_cap - *io_len) : 0;
        va_list ap2;
        va_copy(ap2, ap);
        int n = vsnprintf(*io_s + *io_len, avail, fmt, ap2);
        va_end(ap2);
        if (n < 0) return;
        if ((size_t)n < avail) { *io_len += (size_t)n; return; }
        *io_cap = (*io_cap ? *io_cap * 2 : 256) + (size_t)n + 16;
        *io_s = (char*)realloc(*io_s, *io_cap);
        if (!*io_s) return;
    }
}

static void cc__sb_append_fmt(char** io_s, size_t* io_len, size_t* io_cap, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    cc__sb_append_vfmt(io_s, io_len, io_cap, fmt, ap);
    va_end(ap);
}

static void cc__sb_append_cstr(char** io_s, size_t* io_len, size_t* io_cap, const char* s) {
    if (!s) return;
    cc__sb_append_fmt(io_s, io_len, io_cap, "%s", s);
}

static char* cc__dup_slice(const char* b, size_t s, size_t e) {
    if (!b || e <= s) return strdup("");
    size_t n = e - s;
    char* out = (char*)malloc(n + 1);
    if (!out) return NULL;
    memcpy(out, b + s, n);
    out[n] = 0;
    return out;
}

static int cc__find_matching_paren(const char* b, size_t bl, size_t lpar, size_t* out_rpar) {
    if (!b || lpar >= bl || b[lpar] != '(') return 0;
    int par = 1, brk = 0, br = 0;
    int ins = 0; char q = 0;
    int in_lc = 0, in_bc = 0;
    for (size_t p = lpar + 1; p < bl; p++) {
        char ch = b[p];
        char ch2 = (p + 1 < bl) ? b[p + 1] : 0;
        if (in_lc) { if (ch == '\n') in_lc = 0; continue; }
        if (in_bc) { if (ch == '*' && ch2 == '/') { in_bc = 0; p++; } continue; }
        if (ins) { if (ch == '\\' && p + 1 < bl) { p++; continue; } if (ch == q) ins = 0; continue; }
        if (ch == '/' && ch2 == '/') { in_lc = 1; p++; continue; }
        if (ch == '/' && ch2 == '*') { in_bc = 1; p++; continue; }
        if (ch == '"' || ch == '\'') { ins = 1; q = ch; continue; }
        if (ch == '(') par++;
        else if (ch == ')') { par--; if (par == 0) { if (out_rpar) *out_rpar = p; return 1; } }
        else if (ch == '[') brk++;
        else if (ch == ']') { if (brk) brk--; }
        else if (ch == '{') br++;
        else if (ch == '}') { if (br) br--; }
    }
    return 0;
}

static int cc__find_matching_brace(const char* b, size_t bl, size_t lbrace, size_t* out_rbrace) {
    if (!b || lbrace >= bl || b[lbrace] != '{') return 0;
    int br = 1, par = 0, brk = 0;
    int ins = 0; char q = 0;
    int in_lc = 0, in_bc = 0;
    for (size_t p = lbrace + 1; p < bl; p++) {
        char ch = b[p];
        char ch2 = (p + 1 < bl) ? b[p + 1] : 0;
        if (in_lc) { if (ch == '\n') in_lc = 0; continue; }
        if (in_bc) { if (ch == '*' && ch2 == '/') { in_bc = 0; p++; } continue; }
        if (ins) { if (ch == '\\' && p + 1 < bl) { p++; continue; } if (ch == q) ins = 0; continue; }
        if (ch == '/' && ch2 == '/') { in_lc = 1; p++; continue; }
        if (ch == '/' && ch2 == '*') { in_bc = 1; p++; continue; }
        if (ch == '"' || ch == '\'') { ins = 1; q = ch; continue; }
        if (ch == '(') par++;
        else if (ch == ')') { if (par) par--; }
        else if (ch == '[') brk++;
        else if (ch == ']') { if (brk) brk--; }
        else if (ch == '{') br++;
        else if (ch == '}') { br--; if (br == 0) { if (out_rbrace) *out_rbrace = p; return 1; } }
    }
    return 0;
}

static char* cc__rewrite_idents(const char* s, const char* const* names, const char* const* repls, int n) {
    if (!s) return NULL;
    if (n <= 0) return strdup(s);
    size_t sl = strlen(s);
    size_t cap = sl * 3 + 64;
    char* out = (char*)malloc(cap);
    if (!out) return NULL;
    size_t ol = 0;
    for (size_t i = 0; i < sl; ) {
        if (cc__is_ident_start(s[i])) {
            size_t j = i + 1;
            while (j < sl && cc__is_ident_char(s[j])) j++;
            int did = 0;
            for (int k = 0; k < n; k++) {
                size_t nl = strlen(names[k]);
                if (nl == (j - i) && memcmp(s + i, names[k], nl) == 0) {
                    const char* r = repls[k];
                    size_t rl = strlen(r);
                    if (ol + rl + 2 >= cap) { cap = cap * 2 + rl + 64; out = (char*)realloc(out, cap); if (!out) return NULL; }
                    memcpy(out + ol, r, rl);
                    ol += rl;
                    did = 1;
                    break;
                }
            }
            if (!did) {
                size_t tl = j - i;
                if (ol + tl + 2 >= cap) { cap = cap * 2 + tl + 64; out = (char*)realloc(out, cap); if (!out) return NULL; }
                memcpy(out + ol, s + i, tl);
                ol += tl;
            }
            i = j;
            continue;
        }
        if (ol + 2 >= cap) { cap = cap * 2 + 64; out = (char*)realloc(out, cap); if (!out) return NULL; }
        out[ol++] = s[i++];
    }
    out[ol] = 0;
    return out;
}

typedef enum {
    ST_SEMI,
    ST_BLOCK,
    ST_IF,
    ST_WHILE,
    ST_FOR,
    ST_BREAK,
    ST_CONTINUE,
    ST_RETURN,
} StKind;

typedef struct Stmt Stmt;
struct Stmt {
    StKind kind;
    char* text;     /* for SEMI/RETURN: statement text without trailing ';' */
    char* cond;     /* if/while/for cond */
    char* for_init;
    char* for_post;
    Stmt* then_st;
    int then_n;
    Stmt* else_st;
    int else_n;
};

static void cc__free_stmt_list(Stmt* st, int n) {
    if (!st) return;
    for (int i = 0; i < n; i++) {
        free(st[i].text);
        free(st[i].cond);
        free(st[i].for_init);
        free(st[i].for_post);
        cc__free_stmt_list(st[i].then_st, st[i].then_n);
        cc__free_stmt_list(st[i].else_st, st[i].else_n);
        free(st[i].then_st);
        free(st[i].else_st);
    }
}

static void cc__debug_dump_stmt_list(const char* label, const Stmt* st, int n, int indent) {
    if (!getenv("CC_DEBUG_ASYNC_AST")) return;
    if (!st || n <= 0) {
        fprintf(stderr, "CC: async_ast: %*s%s: (empty)\n", indent, "", label ? label : "<list>");
        return;
    }
    fprintf(stderr, "CC: async_ast: %*s%s: n=%d\n", indent, "", label ? label : "<list>", n);
    for (int i = 0; i < n; i++) {
        const Stmt* s = &st[i];
        const char* k = "semi";
        if (s->kind == ST_BLOCK) k = "block";
        else if (s->kind == ST_IF) k = "if";
        else if (s->kind == ST_WHILE) k = "while";
        else if (s->kind == ST_FOR) k = "for";
        else if (s->kind == ST_BREAK) k = "break";
        else if (s->kind == ST_CONTINUE) k = "continue";
        else if (s->kind == ST_RETURN) k = "return";
        fprintf(stderr, "CC: async_ast: %*s- [%d] kind=%s\n", indent, "", i, k);
        if (s->text && (s->kind == ST_SEMI || s->kind == ST_RETURN)) {
            fprintf(stderr, "CC: async_ast: %*s  text: %s\n", indent, "", s->text);
        }
        if (s->cond && (s->kind == ST_IF || s->kind == ST_WHILE || s->kind == ST_FOR)) {
            fprintf(stderr, "CC: async_ast: %*s  cond: %s\n", indent, "", s->cond);
        }
        if (s->for_init && s->kind == ST_FOR) fprintf(stderr, "CC: async_ast: %*s  init: %s\n", indent, "", s->for_init);
        if (s->for_post && s->kind == ST_FOR) fprintf(stderr, "CC: async_ast: %*s  post: %s\n", indent, "", s->for_post);
        if (s->then_st && s->then_n) cc__debug_dump_stmt_list("then", s->then_st, s->then_n, indent + 2);
        if (s->else_st && s->else_n) cc__debug_dump_stmt_list("else", s->else_st, s->else_n, indent + 2);
    }
}

static void cc__collect_decl_names_from_stmt_list(const Stmt* st, int n, char** out_names, int* io_n, int cap) {
    if (!st || !out_names || !io_n) return;
    for (int i = 0; i < n; i++) {
        const Stmt* s = &st[i];
        if (s->kind == ST_BLOCK || s->kind == ST_IF || s->kind == ST_WHILE || s->kind == ST_FOR) {
            cc__collect_decl_names_from_stmt_list(s->then_st, s->then_n, out_names, io_n, cap);
            cc__collect_decl_names_from_stmt_list(s->else_st, s->else_n, out_names, io_n, cap);
            continue;
        }
        if (s->kind != ST_SEMI) continue;
        const char* p = cc__skip_ws(s->text ? s->text : "");
        if (strncmp(p, "int ", 4) == 0) p += 4;
        else if (strncmp(p, "intptr_t ", 9) == 0) p += 9;
        else continue;
        p = cc__skip_ws(p);
        if (!cc__is_ident_start(*p)) continue;
        const char* ns = p++;
        while (cc__is_ident_char(*p)) p++;
        size_t nn = (size_t)(p - ns);
        if (nn == 0 || nn >= 128) continue;
        char name[128];
        memcpy(name, ns, nn);
        name[nn] = 0;
        int dup = 0;
        for (int k = 0; k < *io_n; k++) if (out_names[k] && strcmp(out_names[k], name) == 0) dup = 1;
        if (dup) continue;
        if (*io_n < cap) out_names[(*io_n)++] = strdup(name);
    }
}

/* forward decls (used by text-based helpers below) */
static int cc__trim_trailing_semicolon(char* s);
static int cc__split_top_level_semis(const char* s, char*** out_parts, int* out_n);
static size_t cc__skip_ws_and_comments_bounded(const char* src, size_t len, size_t i, size_t end);

static size_t cc__skip_ws_and_comments_bounded(const char* src, size_t len, size_t i, size_t end) {
    (void)len;
    while (i < end && src && src[i]) {
        char c = src[i];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') { i++; continue; }
        if (c == '/' && i + 1 < end && src[i + 1] == '/') {
            i += 2;
            while (i < end && src[i] && src[i] != '\n') i++;
            continue;
        }
        if (c == '/' && i + 1 < end && src[i + 1] == '*') {
            i += 2;
            while (i + 1 < end && src[i] && !(src[i] == '*' && src[i + 1] == '/')) i++;
            if (i + 1 < end && src[i] == '*' && src[i + 1] == '/') i += 2;
            continue;
        }
        break;
    }
    return i;
}

static int cc__node_is_descendant_of(const NodeView* n, int node_idx, int anc_idx) {
    int p = node_idx;
    while (p >= 0) {
        if (p == anc_idx) return 1;
        p = n[p].parent;
    }
    return 0;
}

static void cc__trim_ws_bounds(const char* s, size_t* io_s, size_t* io_e) {
    if (!s || !io_s || !io_e) return;
    size_t a = *io_s, b = *io_e;
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\n' || s[a] == '\r')) a++;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\n' || s[b - 1] == '\r')) b--;
    *io_s = a;
    *io_e = b;
}

static int cc__build_simple_stmt_from_text(const char* s, size_t ss, size_t se, Stmt* out) {
    if (!out || !s) return 0;
    memset(out, 0, sizeof(*out));
    cc__trim_ws_bounds(s, &ss, &se);
    if (se <= ss) { out->kind = ST_SEMI; out->text = strdup(""); return 1; }
    char* t = cc__dup_slice(s, ss, se);
    if (!t) t = strdup("");
    cc__trim_trailing_semicolon(t);
    const char* p = cc__skip_ws(t);
    if (strncmp(p, "return", 6) == 0 && !cc__is_ident_char(p[6])) {
        out->kind = ST_RETURN;
        out->text = strdup(p);
        free(t);
        return 1;
    }
    out->kind = ST_SEMI;
    out->text = t;
    return 1;
}

static int cc__parse_stmt_list_from_text_range(const char* src, size_t len, size_t ss, size_t se,
                                               Stmt** out_list, int* out_n);

static int cc__match_kw_at(const char* s, size_t i, size_t end, const char* kw) {
    size_t kl = strlen(kw);
    if (i + kl > end) return 0;
    if (memcmp(s + i, kw, kl) != 0) return 0;
    if (i > 0 && cc__is_ident_char(s[i - 1])) return 0;
    if (i + kl < end && cc__is_ident_char(s[i + kl])) return 0;
    return 1;
}

static size_t cc__scan_simple_stmt_end(const char* src, size_t i, size_t end) {
    int par = 0, brk = 0, br = 0;
    int ins = 0; char q = 0;
    int in_lc = 0, in_bc = 0;
    for (; i < end; i++) {
        char ch = src[i];
        char ch2 = (i + 1 < end) ? src[i + 1] : 0;
        if (in_lc) { if (ch == '\n') in_lc = 0; continue; }
        if (in_bc) { if (ch == '*' && ch2 == '/') { in_bc = 0; i++; } continue; }
        if (ins) { if (ch == '\\' && i + 1 < end) { i++; continue; } if (ch == q) ins = 0; continue; }
        if (ch == '/' && ch2 == '/') { in_lc = 1; i++; continue; }
        if (ch == '/' && ch2 == '*') { in_bc = 1; i++; continue; }
        if (ch == '"' || ch == '\'') { ins = 1; q = ch; continue; }
        if (ch == '(') par++;
        else if (ch == ')') { if (par) par--; }
        else if (ch == '[') brk++;
        else if (ch == ']') { if (brk) brk--; }
        else if (ch == '{') br++;
        else if (ch == '}') { if (br) br--; }
        if (par == 0 && brk == 0 && br == 0 && ch == ';') return i;
    }
    return end;
}

static int cc__parse_one_stmt_from_text(const char* src, size_t len, size_t ss, size_t se,
                                        Stmt* out, size_t* out_end);

/* Best-effort parse of `if (...) <stmt> [else <stmt>]` / `else if (...) ...` chains from a source slice. */
static int cc__parse_if_chain_from_text(const char* src, size_t len, size_t ss, size_t se, Stmt* out, size_t* out_end) {
    if (!src || !out || se <= ss) return 0;
    memset(out, 0, sizeof(*out));
    out->kind = ST_IF;
    size_t i = ss;
    /* find "if" */
    while (i + 1 < se) {
        if (src[i] == 'i' && src[i + 1] == 'f' && (i == ss || !cc__is_ident_char(src[i - 1])) && !cc__is_ident_char(src[i + 2])) break;
        i++;
    }
    if (i + 1 >= se) return 0;
    /* find '(' */
    while (i < se && src[i] != '(') i++;
    if (i >= se) return 0;
    size_t lpo = i, rpo = 0;
    if (!cc__find_matching_paren(src, len, lpo, &rpo)) return 0;
    out->cond = cc__dup_slice(src, lpo + 1, rpo);
    if (!out->cond) out->cond = strdup("0");
    i = rpo + 1;
    i = cc__skip_ws_and_comments_bounded(src, len, i, se);

    /* parse then stmt */
    if (i < se && src[i] == '{') {
        size_t rb = 0;
        if (!cc__find_matching_brace(src, len, i, &rb)) return 0;
        (void)cc__parse_stmt_list_from_text_range(src, len, i + 1, rb, &out->then_st, &out->then_n);
        i = rb + 1;
    } else {
        size_t end0 = cc__scan_simple_stmt_end(src, i, se);
        out->then_st = (Stmt*)calloc(1, sizeof(Stmt));
        out->then_n = 1;
        (void)cc__build_simple_stmt_from_text(src, i, end0, &out->then_st[0]);
        i = (end0 < se && src[end0] == ';') ? (end0 + 1) : end0;
    }

    /* optional else */
    i = cc__skip_ws_and_comments_bounded(src, len, i, se);
    if (cc__match_kw_at(src, i, se, "else")) {
        i += 4;
        i = cc__skip_ws_and_comments_bounded(src, len, i, se);
        /* else if */
        if (cc__match_kw_at(src, i, se, "if")) {
            out->else_st = (Stmt*)calloc(1, sizeof(Stmt));
            out->else_n = 1;
            size_t end_if = se;
            (void)cc__parse_if_chain_from_text(src, len, i, se, &out->else_st[0], &end_if);
            i = end_if;
        } else if (i < se && src[i] == '{') {
            size_t rb = 0;
            if (!cc__find_matching_brace(src, len, i, &rb)) return 0;
            (void)cc__parse_stmt_list_from_text_range(src, len, i + 1, rb, &out->else_st, &out->else_n);
            i = rb + 1;
        } else {
        size_t end0 = cc__scan_simple_stmt_end(src, i, se);
            out->else_st = (Stmt*)calloc(1, sizeof(Stmt));
            out->else_n = 1;
            (void)cc__build_simple_stmt_from_text(src, i, end0, &out->else_st[0]);
            i = (end0 < se && src[end0] == ';') ? (end0 + 1) : end0;
        }
    }
    if (out_end) *out_end = i;
    return 1;
}

static int cc__build_stmt_list_from_text_body(const char* src, size_t len, size_t lbrace, size_t rbrace,
                                              Stmt** out_list, int* out_n) {
    if (!src || !out_list || !out_n) return 0;
    *out_list = NULL;
    *out_n = 0;
    if (!(rbrace > lbrace + 1 && rbrace <= len)) return 0;
    return cc__parse_stmt_list_from_text_range(src, len, lbrace + 1, rbrace, out_list, out_n);
}

static int cc__parse_while_from_text(const char* src, size_t len, size_t ss, size_t se, Stmt* out, size_t* out_end) {
    if (!src || !out || se <= ss) return 0;
    memset(out, 0, sizeof(*out));
    out->kind = ST_WHILE;
    size_t i = ss;
    if (!cc__match_kw_at(src, i, se, "while")) return 0;
    while (i < se && src[i] != '(') i++;
    if (i >= se) return 0;
    size_t lpo = i, rpo = 0;
    if (!cc__find_matching_paren(src, len, lpo, &rpo)) return 0;
    out->cond = cc__dup_slice(src, lpo + 1, rpo);
    if (!out->cond) out->cond = strdup("0");
    i = cc__skip_ws_and_comments_bounded(src, len, rpo + 1, se);
    if (i < se && src[i] == '{') {
        size_t rb = 0;
        if (!cc__find_matching_brace(src, len, i, &rb)) return 0;
        (void)cc__parse_stmt_list_from_text_range(src, len, i + 1, rb, &out->then_st, &out->then_n);
        i = rb + 1;
    } else {
        out->then_st = (Stmt*)calloc(1, sizeof(Stmt));
        out->then_n = 1;
        size_t end0 = se;
        (void)cc__parse_one_stmt_from_text(src, len, i, se, &out->then_st[0], &end0);
        i = end0;
    }
    if (out_end) *out_end = i;
    return 1;
}

static int cc__parse_for_from_text(const char* src, size_t len, size_t ss, size_t se, Stmt* out, size_t* out_end) {
    if (!src || !out || se <= ss) return 0;
    memset(out, 0, sizeof(*out));
    out->kind = ST_FOR;
    size_t i = ss;
    if (!cc__match_kw_at(src, i, se, "for")) return 0;
    while (i < se && src[i] != '(') i++;
    if (i >= se) return 0;
    size_t lpo = i, rpo = 0;
    if (!cc__find_matching_paren(src, len, lpo, &rpo)) return 0;
    /* split init;cond;post within parens (top-level) */
    char* hdr = cc__dup_slice(src, lpo + 1, rpo);
    if (!hdr) hdr = strdup("");
    size_t hl = strlen(hdr);
    size_t a = 0;
    int found = 0;
    for (size_t k = 0; k <= hl; k++) {
        if (k == hl || hdr[k] == ';') {
            if (found == 0) out->for_init = cc__dup_slice(hdr, a, k);
            else if (found == 1) out->cond = cc__dup_slice(hdr, a, k);
            else if (found == 2) out->for_post = cc__dup_slice(hdr, a, k);
            found++;
            a = k + 1;
        }
    }
    free(hdr);
    if (!out->cond) out->cond = strdup("1");
    i = cc__skip_ws_and_comments_bounded(src, len, rpo + 1, se);
    if (i < se && src[i] == '{') {
        size_t rb = 0;
        if (!cc__find_matching_brace(src, len, i, &rb)) return 0;
        (void)cc__parse_stmt_list_from_text_range(src, len, i + 1, rb, &out->then_st, &out->then_n);
        i = rb + 1;
    } else {
        out->then_st = (Stmt*)calloc(1, sizeof(Stmt));
        out->then_n = 1;
        size_t end0 = se;
        (void)cc__parse_one_stmt_from_text(src, len, i, se, &out->then_st[0], &end0);
        i = end0;
    }
    if (out_end) *out_end = i;
    return 1;
}

static int cc__parse_one_stmt_from_text(const char* src, size_t len, size_t ss, size_t se,
                                        Stmt* out, size_t* out_end) {
    if (!out || !src) return 0;
    size_t i = cc__skip_ws_and_comments_bounded(src, len, ss, se);
    if (i >= se) { memset(out, 0, sizeof(*out)); out->kind = ST_SEMI; out->text = strdup(""); if (out_end) *out_end = se; return 1; }
    if (src[i] == '{') {
        size_t rb = 0;
        if (!cc__find_matching_brace(src, len, i, &rb)) return 0;
        memset(out, 0, sizeof(*out));
        out->kind = ST_BLOCK;
        (void)cc__parse_stmt_list_from_text_range(src, len, i + 1, rb, &out->then_st, &out->then_n);
        if (out_end) *out_end = rb + 1;
        return 1;
    }
    if (cc__match_kw_at(src, i, se, "if")) {
        size_t end_if = se;
        int ok = cc__parse_if_chain_from_text(src, len, i, se, out, &end_if);
        if (out_end) *out_end = end_if;
        return ok;
    }
    if (cc__match_kw_at(src, i, se, "while")) {
        size_t end_w = se;
        int ok = cc__parse_while_from_text(src, len, i, se, out, &end_w);
        if (out_end) *out_end = end_w;
        return ok;
    }
    if (cc__match_kw_at(src, i, se, "for")) {
        size_t end_f = se;
        int ok = cc__parse_for_from_text(src, len, i, se, out, &end_f);
        if (out_end) *out_end = end_f;
        return ok;
    }
    if (cc__match_kw_at(src, i, se, "break")) {
        memset(out, 0, sizeof(*out));
        out->kind = ST_BREAK;
        size_t e0 = cc__scan_simple_stmt_end(src, i, se);
        if (out_end) *out_end = (e0 < se && src[e0] == ';') ? (e0 + 1) : e0;
        return 1;
    }
    if (cc__match_kw_at(src, i, se, "continue")) {
        memset(out, 0, sizeof(*out));
        out->kind = ST_CONTINUE;
        size_t e0 = cc__scan_simple_stmt_end(src, i, se);
        if (out_end) *out_end = (e0 < se && src[e0] == ';') ? (e0 + 1) : e0;
        return 1;
    }
    size_t e0 = cc__scan_simple_stmt_end(src, i, se);
    (void)cc__build_simple_stmt_from_text(src, i, e0, out);
    if (out_end) *out_end = (e0 < se && src[e0] == ';') ? (e0 + 1) : e0;
    return 1;
}

static int cc__parse_stmt_list_from_text_range(const char* src, size_t len, size_t ss, size_t se,
                                               Stmt** out_list, int* out_n) {
    if (!src || !out_list || !out_n) return 0;
    *out_list = NULL;
    *out_n = 0;
    int cap = 8;
    int n = 0;
    Stmt* st = (Stmt*)calloc((size_t)cap, sizeof(Stmt));
    if (!st) return 0;
    size_t i = ss;
    while (1) {
        i = cc__skip_ws_and_comments_bounded(src, len, i, se);
        if (i >= se) break;
        if (n == cap) {
            cap *= 2;
            st = (Stmt*)realloc(st, (size_t)cap * sizeof(Stmt));
            if (!st) return 0;
            memset(&st[n], 0, (size_t)(cap - n) * sizeof(Stmt));
        }
        size_t end0 = se;
        if (!cc__parse_one_stmt_from_text(src, len, i, se, &st[n], &end0)) { cc__free_stmt_list(st, n); free(st); return 0; }
        n++;
        if (end0 <= i) break;
        i = end0;
    }
    *out_list = st;
    *out_n = n;
    return 1;
}

static int cc__collect_child(const CCASTRoot* root, const CCVisitorCtx* ctx, const NodeView* n, int parent_idx, int kind, int* out, int out_cap) {
    int c = 0;
    for (int i = 0; i < root->node_count && c < out_cap; i++) {
        if (n[i].kind != kind) continue;
        if (n[i].parent != parent_idx) continue;
        if (!cc__node_in_this_tu(root, ctx, n[i].file)) continue;
        out[c++] = i;
    }
    return c;
}

static int cc__build_stmt_list_from_block(const CCASTRoot* root,
                                         const CCVisitorCtx* ctx,
                                         const NodeView* n,
                                         const char* src,
                                         size_t src_len,
                                         int block_idx,
                                         Stmt** out_list,
                                         int* out_n);

static int cc__trim_trailing_semicolon(char* s) {
    if (!s) return 0;
    size_t tl = strlen(s);
    while (tl > 0 && (s[tl - 1] == ' ' || s[tl - 1] == '\t' || s[tl - 1] == '\n' || s[tl - 1] == '\r')) tl--;
    if (tl > 0 && s[tl - 1] == ';') tl--;
    while (tl > 0 && (s[tl - 1] == ' ' || s[tl - 1] == '\t')) tl--;
    s[tl] = 0;
    return 1;
}

static void cc__truncate_at_first_semicolon0(char* s) {
    if (!s) return;
    size_t sl = strlen(s);
    int par = 0, brk = 0, br = 0;
    int ins = 0; char q = 0;
    int in_lc = 0, in_bc = 0;
    for (size_t i = 0; i < sl; i++) {
        char ch = s[i];
        char ch2 = (i + 1 < sl) ? s[i + 1] : 0;
        if (in_lc) { if (ch == '\n') in_lc = 0; continue; }
        if (in_bc) { if (ch == '*' && ch2 == '/') { in_bc = 0; i++; } continue; }
        if (ins) { if (ch == '\\' && i + 1 < sl) { i++; continue; } if (ch == q) ins = 0; continue; }
        if (ch == '/' && ch2 == '/') { in_lc = 1; i++; continue; }
        if (ch == '/' && ch2 == '*') { in_bc = 1; i++; continue; }
        if (ch == '"' || ch == '\'') { ins = 1; q = ch; continue; }
        if (ch == '(') par++;
        else if (ch == ')') { if (par) par--; }
        else if (ch == '[') brk++;
        else if (ch == ']') { if (brk) brk--; }
        else if (ch == '{') br++;
        else if (ch == '}') { if (br) br--; }
        if (par == 0 && brk == 0 && br == 0 && ch == ';') { s[i] = 0; return; }
    }
}

static void cc__trim_ws_inplace(char* s) {
    if (!s) return;
    size_t n = strlen(s);
    size_t i = 0;
    while (i < n && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) i++;
    size_t j = n;
    while (j > i && (s[j - 1] == ' ' || s[j - 1] == '\t' || s[j - 1] == '\n' || s[j - 1] == '\r')) j--;
    if (i > 0) memmove(s, s + i, j - i);
    s[j - i] = 0;
}

static int cc__split_top_level_semis(const char* s, char*** out_parts, int* out_n) {
    if (!out_parts || !out_n) return 0;
    *out_parts = NULL;
    *out_n = 0;
    if (!s) return 1;

    size_t sl = strlen(s);
    int par = 0, brk = 0, br = 0;
    int ins = 0; char q = 0;
    int in_lc = 0, in_bc = 0;
    size_t seg_s = 0;

    char** parts = NULL;
    int pn = 0, pc = 0;

    for (size_t i = 0; i < sl; i++) {
        char ch = s[i];
        char ch2 = (i + 1 < sl) ? s[i + 1] : 0;
        if (in_lc) { if (ch == '\n') in_lc = 0; continue; }
        if (in_bc) { if (ch == '*' && ch2 == '/') { in_bc = 0; i++; } continue; }
        if (ins) { if (ch == '\\' && i + 1 < sl) { i++; continue; } if (ch == q) ins = 0; continue; }
        if (ch == '/' && ch2 == '/') { in_lc = 1; i++; continue; }
        if (ch == '/' && ch2 == '*') { in_bc = 1; i++; continue; }
        if (ch == '"' || ch == '\'') { ins = 1; q = ch; continue; }
        if (ch == '(') par++;
        else if (ch == ')') { if (par) par--; }
        else if (ch == '[') brk++;
        else if (ch == ']') { if (brk) brk--; }
        else if (ch == '{') br++;
        else if (ch == '}') { if (br) br--; }

        if (par == 0 && brk == 0 && br == 0 && ch == ';') {
            char* part = cc__dup_slice(s, seg_s, i);
            if (!part) continue;
            cc__trim_ws_inplace(part);
            if (part[0]) {
                if (pn == pc) {
                    pc = pc ? pc * 2 : 8;
                    parts = (char**)realloc(parts, (size_t)pc * sizeof(char*));
                    if (!parts) { free(part); return 0; }
                }
                parts[pn++] = part;
            } else {
                free(part);
            }
            seg_s = i + 1;
        }
    }
    if (seg_s < sl) {
        char* part = cc__dup_slice(s, seg_s, sl);
        if (part) {
            cc__trim_ws_inplace(part);
            if (part[0]) {
                if (pn == pc) {
                    pc = pc ? pc * 2 : 8;
                    parts = (char**)realloc(parts, (size_t)pc * sizeof(char*));
                    if (!parts) { free(part); return 0; }
                }
                parts[pn++] = part;
            } else {
                free(part);
            }
        }
    }
    *out_parts = parts;
    *out_n = pn;
    return 1;
}

static int cc__find_loop_body_stmt(const CCASTRoot* root,
                                  const CCVisitorCtx* ctx,
                                  const NodeView* n,
                                  int loop_internal_block_idx) {
    /* Loops in stub-AST have a BLOCK child that contains header expr nodes under a DECL node,
       and the loop body as the *last* statement under that DECL. */
    int decl_idx = -1;
    for (int i = 0; i < root->node_count; i++) {
        if (n[i].kind == CC_AST_NODE_DECL && n[i].parent == loop_internal_block_idx) { decl_idx = i; break; }
    }

    int best = -1;
    size_t best_start = 0;

    /* Prefer stmt children of the DECL node (avoids picking nested inner statements). */
    if (decl_idx >= 0) {
        for (int i = 0; i < root->node_count; i++) {
            if (n[i].kind != CC_AST_NODE_STMT) continue;
            if (n[i].parent != decl_idx) continue;
            if (!cc__node_in_this_tu(root, ctx, n[i].file)) continue;
            /* stable ordering: prefer later statements (higher line_start) */
            size_t k = (size_t)(n[i].line_start > 0 ? n[i].line_start : 0);
            if (best < 0 || k > best_start || (k == best_start && i > best)) {
                best_start = k;
                best = i;
            }
        }
        if (best >= 0) return best;
    }

    /* Fallback: stmt children directly under the block. */
    for (int i = 0; i < root->node_count; i++) {
        if (n[i].kind != CC_AST_NODE_STMT) continue;
        if (n[i].parent != loop_internal_block_idx) continue;
        if (!cc__node_in_this_tu(root, ctx, n[i].file)) continue;
        size_t k = (size_t)(n[i].line_start > 0 ? n[i].line_start : 0);
        if (best < 0 || k > best_start || (k == best_start && i > best)) {
            best_start = k;
            best = i;
        }
    }
    return best;
}

static int cc__build_stmt_from_stmt_node(const CCASTRoot* root,
                                        const CCVisitorCtx* ctx,
                                        const NodeView* n,
                                        const char* src,
                                        size_t src_len,
                                        int stmt_idx,
                                        Stmt* out) {
    if (!out) return 0;
    memset(out, 0, sizeof(*out));

    /* IMPORTANT: earlier visitor passes may rewrite within the same line while preserving line counts.
       Column spans from the stub-AST can drift after those rewrites. For statement extraction we therefore
       default to a whole-line slice (line_start..line_end), and only later do small token parsing within it. */
    int ls = n[stmt_idx].line_start > 0 ? n[stmt_idx].line_start : 1;
    int le = n[stmt_idx].line_end > 0 ? n[stmt_idx].line_end : ls;
    size_t ss = cc__offset_of_line_1based(src, src_len, ls);
    size_t se = (le + 1 > le) ? cc__offset_of_line_1based(src, src_len, le + 1) : src_len;
    if (se > src_len) se = src_len;
    if (se < ss) se = ss;
    char* full = cc__dup_slice(src, ss, se);
    if (!full) full = strdup("");
    const char* kw = n[stmt_idx].aux_s1;

    /* Bare compound statement `{ ... }` (recorded as a STMT with a BLOCK child) */
    if (!kw || kw[0] == 0) {
        int bb[2];
        int bbn = cc__collect_child(root, ctx, n, stmt_idx, CC_AST_NODE_BLOCK, bb, 2);
        if (bbn > 0) {
            out->kind = ST_BLOCK;
            (void)cc__build_stmt_list_from_block(root, ctx, n, src, src_len, bb[0], &out->then_st, &out->then_n);
            free(full);
            return 1;
        }
    }

    /* classify */
    if (kw && strcmp(kw, "if") == 0) {
        out->kind = ST_IF;
        /* parse condition from full */
        const char* p = strstr(full, "if");
        if (!p) p = full;
        const char* lp = strchr(p, '(');
        if (!lp) { free(full); return 0; }
        size_t lpo = (size_t)(lp - full);
        size_t rpo = 0;
        if (!cc__find_matching_paren(full, strlen(full), lpo, &rpo)) { free(full); return 0; }
        out->cond = cc__dup_slice(full, lpo + 1, rpo);

        /* In stub-AST, if bodies are recorded as STMT nodes tagged "then"/"else" inside the if-stmt's internal BLOCK. */
        int then_node = -1, else_node = -1;
        int ib[2];
        int ibn = cc__collect_child(root, ctx, n, stmt_idx, CC_AST_NODE_BLOCK, ib, 2);
        if (ibn > 0) {
            int decl_idx = -1;
            for (int k = 0; k < root->node_count; k++) {
                if (n[k].kind == CC_AST_NODE_DECL && n[k].parent == ib[0]) { decl_idx = k; break; }
            }
            for (int k = 0; k < root->node_count; k++) {
                if (n[k].kind != CC_AST_NODE_STMT) continue;
                if (!(n[k].parent == ib[0] || (decl_idx >= 0 && n[k].parent == decl_idx))) continue;
                if (!n[k].aux_s1) continue;
                if (strcmp(n[k].aux_s1, "then") == 0) then_node = k;
                else if (strcmp(n[k].aux_s1, "else") == 0) else_node = k;
            }
        }
        if (then_node >= 0) {
            /* if then_node has a block child, use it; otherwise single stmt */
            int tb[2];
            int tbn = cc__collect_child(root, ctx, n, then_node, CC_AST_NODE_BLOCK, tb, 2);
            if (tbn > 0) {
                (void)cc__build_stmt_list_from_block(root, ctx, n, src, src_len, tb[0], &out->then_st, &out->then_n);
            } else {
                /* then_node is a wrapper (aux_s1="then"); actual stmt is typically its child */
                int child = -1;
                for (int k = 0; k < root->node_count; k++) {
                    if (n[k].kind != CC_AST_NODE_STMT) continue;
                    if (n[k].parent != then_node) continue;
                    child = k;
                    break;
                }
                out->then_st = (Stmt*)calloc(1, sizeof(Stmt));
                out->then_n = 1;
                (void)cc__build_stmt_from_stmt_node(root, ctx, n, src, src_len, (child >= 0 ? child : then_node), &out->then_st[0]);
            }
        }
        if (else_node >= 0) {
            int eb[2];
            int ebn = cc__collect_child(root, ctx, n, else_node, CC_AST_NODE_BLOCK, eb, 2);
            if (ebn > 0) {
                (void)cc__build_stmt_list_from_block(root, ctx, n, src, src_len, eb[0], &out->else_st, &out->else_n);
            } else {
                /* else_node is a wrapper (aux_s1="else"); actual stmt is typically its child.
                   This is critical for `else if (...) { ... }` chains. */
                int child = -1;
                for (int k = 0; k < root->node_count; k++) {
                    if (n[k].kind != CC_AST_NODE_STMT) continue;
                    if (n[k].parent != else_node) continue;
                    child = k;
                    break;
                }
                out->else_st = (Stmt*)calloc(1, sizeof(Stmt));
                out->else_n = 1;
                (void)cc__build_stmt_from_stmt_node(root, ctx, n, src, src_len, (child >= 0 ? child : else_node), &out->else_st[0]);
            }
        }
        if (getenv("CC_DEBUG_ASYNC_AST")) {
            fprintf(stderr, "CC: async_ast: if stmt idx=%d then_n=%d else_n=%d\n", stmt_idx, out->then_n, out->else_n);
        }
        /* Fallback: if the stub-AST couldn't structure an `else if` chain, it often appears as raw text
           in else_st as `} else if (...) { ... }`. Detect and rebuild the entire chain via text parse. */
        if (out->else_n > 0 && out->else_st && out->else_st[0].kind == ST_SEMI && out->else_st[0].text &&
            strstr(out->else_st[0].text, "else if") != NULL) {
            cc__free_stmt_list(out->then_st, out->then_n);
            cc__free_stmt_list(out->else_st, out->else_n);
            free(out->then_st); out->then_st = NULL; out->then_n = 0;
            free(out->else_st); out->else_st = NULL; out->else_n = 0;
            free(out->cond); out->cond = NULL;
            Stmt tmp;
            memset(&tmp, 0, sizeof(tmp));
            size_t end_if = strlen(full);
            (void)cc__parse_if_chain_from_text(full, strlen(full), 0, strlen(full), &tmp, &end_if);
            /* move tmp into out */
            *out = tmp;
        }
        free(full);
        return 1;
    }

    if (kw && strcmp(kw, "while") == 0) {
        out->kind = ST_WHILE;
        const char* p = strstr(full, "while");
        if (!p) p = full;
        const char* lp = strchr(p, '(');
        if (!lp) { free(full); return 0; }
        size_t lpo = (size_t)(lp - full);
        size_t rpo = 0;
        if (!cc__find_matching_paren(full, strlen(full), lpo, &rpo)) { free(full); return 0; }
        out->cond = cc__dup_slice(full, lpo + 1, rpo);
        /* loop header/body are stored under a BLOCK child; pick the actual body stmt inside that block. */
        int bb[2];
        int bbn = cc__collect_child(root, ctx, n, stmt_idx, CC_AST_NODE_BLOCK, bb, 2);
        if (bbn > 0) {
            int body_stmt = cc__find_loop_body_stmt(root, ctx, n, bb[0]);
            if (getenv("CC_DEBUG_ASYNC_AST")) {
                fprintf(stderr, "CC: async_ast: while stmt idx=%d block=%d body_stmt=%d\n", stmt_idx, bb[0], body_stmt);
            }
            if (body_stmt >= 0) {
                int body_blk = -1;
                for (int j = 0; j < root->node_count; j++) {
                    if (n[j].kind == CC_AST_NODE_BLOCK && n[j].parent == body_stmt) { body_blk = j; break; }
                }
                if (body_blk >= 0) {
                    (void)cc__build_stmt_list_from_block(root, ctx, n, src, src_len, body_blk, &out->then_st, &out->then_n);
                    if (getenv("CC_DEBUG_ASYNC_AST")) {
                        fprintf(stderr, "CC: async_ast: for body block=%d stmt_count=%d\n", body_blk, out->then_n);
                    }
                }
            }
        }
        free(full);
        return 1;
    }

    if (kw && strcmp(kw, "for") == 0) {
        out->kind = ST_FOR;
        const char* p = strstr(full, "for");
        if (!p) p = full;
        const char* lp = strchr(p, '(');
        if (!lp) { free(full); return 0; }
        size_t lpo = (size_t)(lp - full);
        size_t rpo = 0;
        if (!cc__find_matching_paren(full, strlen(full), lpo, &rpo)) { free(full); return 0; }
        char* header = cc__dup_slice(full, lpo + 1, rpo);
        if (!header) header = strdup("");
        /* split by two top-level semicolons */
        size_t hl = strlen(header);
        int parx = 0, brkx = 0, brx = 0;
        int insx = 0; char qx = 0;
        int in_lc = 0, in_bc = 0;
        int semi_n = 0;
        size_t semi1 = 0, semi2 = 0;
        for (size_t k = 0; k < hl; k++) {
            char ch = header[k];
            char ch2 = (k + 1 < hl) ? header[k + 1] : 0;
            if (in_lc) { if (ch == '\n') in_lc = 0; continue; }
            if (in_bc) { if (ch == '*' && ch2 == '/') { in_bc = 0; k++; } continue; }
            if (insx) { if (ch == '\\' && k + 1 < hl) { k++; continue; } if (ch == qx) insx = 0; continue; }
            if (ch == '/' && ch2 == '/') { in_lc = 1; k++; continue; }
            if (ch == '/' && ch2 == '*') { in_bc = 1; k++; continue; }
            if (ch == '"' || ch == '\'') { insx = 1; qx = ch; continue; }
            if (ch == '(') parx++;
            else if (ch == ')') { if (parx) parx--; }
            else if (ch == '[') brkx++;
            else if (ch == ']') { if (brkx) brkx--; }
            else if (ch == '{') brx++;
            else if (ch == '}') { if (brx) brx--; }
            else if (ch == ';' && parx == 0 && brkx == 0 && brx == 0) {
                semi_n++;
                if (semi_n == 1) semi1 = k;
                else if (semi_n == 2) semi2 = k;
            }
        }
        if (semi_n == 2) {
            out->for_init = cc__dup_slice(header, 0, semi1);
            out->cond = cc__dup_slice(header, semi1 + 1, semi2);
            out->for_post = cc__dup_slice(header, semi2 + 1, hl);
        } else {
            out->for_init = strdup("");
            out->cond = strdup("1");
            out->for_post = strdup("");
        }
        free(header);

        /* for header/body are stored under a BLOCK child; pick the actual body stmt inside that block. */
        int bb[2];
        int bbn = cc__collect_child(root, ctx, n, stmt_idx, CC_AST_NODE_BLOCK, bb, 2);
        if (bbn > 0) {
            int body_stmt = cc__find_loop_body_stmt(root, ctx, n, bb[0]);
            if (getenv("CC_DEBUG_ASYNC_AST")) {
                fprintf(stderr, "CC: async_ast: for stmt idx=%d block=%d body_stmt=%d\n", stmt_idx, bb[0], body_stmt);
            }
            if (body_stmt >= 0) {
                int body_blk = -1;
                for (int j = 0; j < root->node_count; j++) {
                    if (n[j].kind == CC_AST_NODE_BLOCK && n[j].parent == body_stmt) { body_blk = j; break; }
                }
                if (body_blk >= 0) {
                    (void)cc__build_stmt_list_from_block(root, ctx, n, src, src_len, body_blk, &out->then_st, &out->then_n);
                    if (getenv("CC_DEBUG_ASYNC_AST")) {
                        fprintf(stderr, "CC: async_ast: for body block=%d stmt_count=%d\n", body_blk, out->then_n);
                    }
                } else if (getenv("CC_DEBUG_ASYNC_AST")) {
                    fprintf(stderr, "CC: async_ast: for body_stmt=%d has no direct BLOCK child\n", body_stmt);
                }
            }
        }

        free(full);
        return 1;
    }

    if (kw && strcmp(kw, "break") == 0) {
        out->kind = ST_BREAK;
        free(full);
        return 1;
    }
    if (kw && strcmp(kw, "continue") == 0) {
        out->kind = ST_CONTINUE;
        free(full);
        return 1;
    }

    /* Return statement as text-based (await handled later) */
    if (kw && strcmp(kw, "return") == 0) {
        out->kind = ST_RETURN;
        cc__truncate_at_first_semicolon0(full);
        cc__trim_trailing_semicolon(full);
        out->text = full;
        return 1;
    }

    /* Everything else: treat as semicolon statement text */
    out->kind = ST_SEMI;
    cc__truncate_at_first_semicolon0(full);
    cc__trim_trailing_semicolon(full);
    out->text = full;
    return 1;
}

static int cc__build_stmt_list_from_block(const CCASTRoot* root,
                                         const CCVisitorCtx* ctx,
                                         const NodeView* n,
                                         const char* src,
                                         size_t src_len,
                                         int block_idx,
                                         Stmt** out_list,
                                         int* out_n) {
    if (!out_list || !out_n) return 0;
    *out_list = NULL;
    *out_n = 0;

    typedef struct { int kind; int idx; size_t start; } NodeRef;
    NodeRef refs[768];
    int ref_n = 0;

    /* 1) statements directly under BLOCK */
    for (int i = 0; i < root->node_count && ref_n < 768; i++) {
        if (n[i].kind != CC_AST_NODE_STMT) continue;
        if (n[i].parent != block_idx) continue;
        if (!cc__node_in_this_tu(root, ctx, n[i].file)) continue;
        refs[ref_n++] = (NodeRef){ .kind = CC_AST_NODE_STMT, .idx = i, .start = cc__node_start_off(src, src_len, &n[i]) };
    }

    /* 2) contents under BLOCK's DECL child: decl-items + stmts */
    int decls[8];
    int dn = cc__collect_child(root, ctx, n, block_idx, CC_AST_NODE_DECL, decls, 8);
    for (int di = 0; di < dn && ref_n < 768; di++) {
        int d = decls[di];
        for (int i = 0; i < root->node_count && ref_n < 768; i++) {
            if (n[i].parent != d) continue;
            if (!cc__node_in_this_tu(root, ctx, n[i].file)) continue;
            if (n[i].kind == CC_AST_NODE_STMT) {
                refs[ref_n++] = (NodeRef){ .kind = CC_AST_NODE_STMT, .idx = i, .start = cc__node_start_off(src, src_len, &n[i]) };
            } else if (n[i].kind == CC_AST_NODE_DECL_ITEM) {
                /* local var decls are stored as decl items; include as pseudo-stmts so we can hoist/init them */
                refs[ref_n++] = (NodeRef){ .kind = CC_AST_NODE_DECL_ITEM, .idx = i, .start = cc__node_start_off(src, src_len, &n[i]) };
            }
        }
    }

    /* sort by start */
    for (int a = 0; a < ref_n; a++) {
        for (int b = a + 1; b < ref_n; b++) {
            if (refs[b].start < refs[a].start) { NodeRef t = refs[a]; refs[a] = refs[b]; refs[b] = t; }
        }
    }

    /* Fallback: some nested blocks currently don't record CC_AST_NODE_STMT children (only expr nodes).
       In that case, recover by slicing the block text and splitting on top-level semicolons. */
    if (ref_n == 0) {
        /* Prefer exact column spans when available (reparse stub-AST matches current rewritten source). */
        size_t ss = cc__node_start_off(src, src_len, &n[block_idx]);
        size_t se = cc__node_end_off(src, src_len, &n[block_idx]);
        if (!(se > ss && se <= src_len)) {
            int ls = n[block_idx].line_start > 0 ? n[block_idx].line_start : 1;
            int le = n[block_idx].line_end > 0 ? n[block_idx].line_end : ls;
            ss = cc__offset_of_line_1based(src, src_len, ls);
            se = cc__offset_of_line_1based(src, src_len, le + 1);
        }
        if (se > src_len) se = src_len;
        if (se < ss) se = ss;
        char* full = cc__dup_slice(src, ss, se);
        if (!full) full = strdup("");
        /* Find first '{' and matching '}' and use inner text. */
        size_t bl = strlen(full);
        size_t lb = 0;
        while (lb < bl && full[lb] != '{') lb++;
        size_t rb = 0;
        if (lb < bl && cc__find_matching_brace(full, bl, lb, &rb) && rb > lb) {
            Stmt* st = NULL;
            int sn = 0;
            if (!cc__parse_stmt_list_from_text_range(full, bl, lb + 1, rb, &st, &sn)) { free(full); return 0; }
            free(full);
            *out_list = st;
            *out_n = sn;
            return 1;
        }
        /* No braces: treat whole slice as a single semi-like stmt */
        {
            Stmt* st = (Stmt*)calloc(1, sizeof(Stmt));
            if (!st) { free(full); return 0; }
            (void)cc__build_simple_stmt_from_text(full, 0, strlen(full), &st[0]);
            free(full);
            *out_list = st;
            *out_n = 1;
            return 1;
        }
    }

    Stmt* st = (Stmt*)calloc((size_t)ref_n, sizeof(Stmt));
    if (!st) return 0;
    for (int i = 0; i < ref_n; i++) {
        if (refs[i].kind == CC_AST_NODE_STMT) {
            if (!cc__build_stmt_from_stmt_node(root, ctx, n, src, src_len, refs[i].idx, &st[i])) {
                cc__free_stmt_list(st, ref_n);
                free(st);
                return 0;
            }
        } else {
            /* DECL_ITEM pseudo stmt: slice its line-range and truncate at ';' */
            memset(&st[i], 0, sizeof(st[i]));
            st[i].kind = ST_SEMI;
            int ls = n[refs[i].idx].line_start > 0 ? n[refs[i].idx].line_start : 1;
            int le = n[refs[i].idx].line_end > 0 ? n[refs[i].idx].line_end : ls;
            size_t ss = cc__offset_of_line_1based(src, src_len, ls);
            size_t se = (le + 1 > le) ? cc__offset_of_line_1based(src, src_len, le + 1) : src_len;
            if (se > src_len) se = src_len;
            if (se < ss) se = ss;
            char* full = cc__dup_slice(src, ss, se);
            if (!full) full = strdup("");
            cc__truncate_at_first_semicolon0(full);
            cc__trim_trailing_semicolon(full);
            st[i].text = full;
        }
    }
    *out_list = st;
    *out_n = ref_n;
    return 1;
}

typedef struct {
    char** out;
    size_t* out_len;
    size_t* out_cap;
    const char* const* map_names;
    const char* const* map_repls;
    int map_n;
    int* cur_state;
    int* next_state;
    int* task_idx;
    int ret_is_void;
    int* finished;
    int loop_depth;
    int break_state[64];
    int cont_state[64];
} Emit;

static int cc__emit_stmt_list(Emit* e, const Stmt* st, int n);

static int cc__alloc_state(Emit* e) {
    if (!e || !e->next_state) return 0;
    int st = (*e->next_state)++;
    if (st <= 0) st = (*e->next_state)++;
    return st;
}

static int cc__emit_open_case(Emit* e, int st) {
    if (!e || !e->out) return 0;
    cc__sb_append_fmt(e->out, e->out_len, e->out_cap, "case %d:{\n", st);
    if (e->cur_state) *e->cur_state = st;
    return 1;
}

static void cc__emit_close_case(Emit* e) {
    cc__sb_append_cstr(e->out, e->out_len, e->out_cap, "}\n");
}

static int cc__emit_await(Emit* e, const char* task_expr, const char* assign_to /* rewritten */) {
    if (!e || !task_expr || !e->task_idx || !e->cur_state || !e->next_state) return 0;
    if (*e->task_idx >= 16) return 0;
    int t = (*e->task_idx)++;
    int poll_state = cc__alloc_state(e);
    int cont_state = cc__alloc_state(e);

    char* ex = cc__rewrite_idents(task_expr, e->map_names, e->map_repls, e->map_n);
    if (!ex) return 0;

    cc__sb_append_fmt(e->out, e->out_len, e->out_cap,
                      "__f->__t%d=(%s);__f->__st=%d;return CC_FUTURE_PENDING;\n",
                      t, ex, poll_state);
    free(ex);
    cc__emit_close_case(e);

    cc__emit_open_case(e, poll_state);
    cc__sb_append_fmt(e->out, e->out_len, e->out_cap,
                      "intptr_t __v=0;int __err=0;CCFutureStatus __st=cc_task_intptr_poll(&__f->__t%d,&__v,&__err);"
                      "if(__st==CC_FUTURE_PENDING)return CC_FUTURE_PENDING;cc_task_intptr_free(&__f->__t%d);",
                      t, t);
    if (assign_to && assign_to[0]) {
        cc__sb_append_fmt(e->out, e->out_len, e->out_cap, "%s=(intptr_t)__v;", assign_to);
    } else {
        cc__sb_append_cstr(e->out, e->out_len, e->out_cap, "(void)__v;");
    }
    cc__sb_append_fmt(e->out, e->out_len, e->out_cap, "__f->__st=%d;return CC_FUTURE_PENDING;\n", cont_state);
    cc__emit_close_case(e);
    cc__emit_open_case(e, cont_state);
    return 1;
}

/* Very small "await in expr" expander: rewrites occurrences of `await <expr>` into temp idents.
   Emits awaits first (in source order), storing results into __f->__cc_awK fields (pre-hoisted).
   Returns newly allocated expression string where await occurrences are replaced by __cc_awK.

   This is intentionally conservative (best-effort parsing). */
static char* cc__emit_awaits_in_expr(Emit* e, const char* expr, int* io_aw_next) {
    if (!e || !expr || !io_aw_next) return NULL;
    const char* s = expr;
    size_t sl = strlen(s);
    size_t out_cap = sl * 2 + 64;
    char* out = (char*)malloc(out_cap);
    if (!out) return NULL;
    size_t out_len = 0;

    int par = 0, brk = 0, br = 0;
    int ins = 0; char q = 0;
    int in_lc = 0, in_bc = 0;

    for (size_t i = 0; i < sl; ) {
        char ch = s[i];
        char ch2 = (i + 1 < sl) ? s[i + 1] : 0;
        if (in_lc) { if (ch == '\n') in_lc = 0; out[out_len++] = ch; i++; continue; }
        if (in_bc) { if (ch == '*' && ch2 == '/') { in_bc = 0; out[out_len++] = ch; out[out_len++] = ch2; i += 2; continue; } out[out_len++] = ch; i++; continue; }
        if (ins) { out[out_len++] = ch; if (ch == '\\' && i + 1 < sl) { out[out_len++] = s[i+1]; i += 2; continue; } if (ch == q) ins = 0; i++; continue; }
        if (ch == '/' && ch2 == '/') { in_lc = 1; out[out_len++] = ch; out[out_len++] = ch2; i += 2; continue; }
        if (ch == '/' && ch2 == '*') { in_bc = 1; out[out_len++] = ch; out[out_len++] = ch2; i += 2; continue; }
        if (ch == '"' || ch == '\'') { ins = 1; q = ch; out[out_len++] = ch; i++; continue; }
        if (ch == '(') par++;
        else if (ch == ')') { if (par) par--; }
        else if (ch == '[') brk++;
        else if (ch == ']') { if (brk) brk--; }
        else if (ch == '{') br++;
        else if (ch == '}') { if (br) br--; }

        /* look for await keyword at token boundary */
        if (ch == 'a' && i + 5 <= sl && memcmp(s + i, "await", 5) == 0) {
            char pre = (i == 0) ? 0 : s[i - 1];
            char post = (i + 5 < sl) ? s[i + 5] : 0;
            if ((i == 0 || !cc__is_ident_char(pre)) && (i + 5 == sl || !cc__is_ident_char(post))) {
                size_t j = i + 5;
                while (j < sl && (s[j] == ' ' || s[j] == '\t')) j++;
                size_t expr_s = j;
                /* read operand until a delimiter at depth0 */
                int ppar = 0, pbrk = 0, pbr = 0;
                int pins = 0; char pq = 0;
                int pin_lc = 0, pin_bc = 0;
                for (; j < sl; j++) {
                    char c = s[j];
                    char c2 = (j + 1 < sl) ? s[j + 1] : 0;
                    if (pin_lc) { if (c == '\n') pin_lc = 0; continue; }
                    if (pin_bc) { if (c == '*' && c2 == '/') { pin_bc = 0; j++; } continue; }
                    if (pins) { if (c == '\\' && j + 1 < sl) { j++; continue; } if (c == pq) pins = 0; continue; }
                    if (c == '/' && c2 == '/') { pin_lc = 1; j++; continue; }
                    if (c == '/' && c2 == '*') { pin_bc = 1; j++; continue; }
                    if (c == '"' || c == '\'') { pins = 1; pq = c; continue; }
                    if (c == '(') ppar++;
                    else if (c == ')') { if (ppar) ppar--; else break; }
                    else if (c == '[') pbrk++;
                    else if (c == ']') { if (pbrk) pbrk--; }
                    else if (c == '{') pbr++;
                    else if (c == '}') { if (pbr) pbr--; }
                    if (ppar == 0 && pbrk == 0 && pbr == 0) {
                        if (c == ',' || c == ';') break;
                        if (c == ']' || c == '}') break;
                    }
                }
                size_t expr_e = j;
                while (expr_e > expr_s && (s[expr_e - 1] == ' ' || s[expr_e - 1] == '\t')) expr_e--;
                char* operand = cc__dup_slice(s, expr_s, expr_e);
                if (!operand) { free(out); return NULL; }

                int aw = (*io_aw_next)++;
                char tmp[64];
                snprintf(tmp, sizeof(tmp), "__cc_aw%d", aw);
                char assign_to[128];
                snprintf(assign_to, sizeof(assign_to), "__f->%s", tmp);
                if (!cc__emit_await(e, operand, assign_to)) {
                    free(operand);
                    free(out);
                    return NULL;
                }
                free(operand);

                size_t tl = strlen(tmp);
                if (out_len + tl + 2 >= out_cap) { out_cap = out_cap * 2 + tl + 64; out = (char*)realloc(out, out_cap); if (!out) return NULL; }
                memcpy(out + out_len, tmp, tl);
                out_len += tl;
                i = expr_e; /* continue after operand */
                continue;
            }
        }

        if (out_len + 2 >= out_cap) { out_cap = out_cap * 2 + 64; out = (char*)realloc(out, out_cap); if (!out) return NULL; }
        out[out_len++] = ch;
        i++;
    }

    out[out_len] = 0;
    return out;
}

static int cc__emit_semi_like(Emit* e, const char* text) {
    if (!e || !text) return 1;
    const char* p = cc__skip_ws(text);
    if (p[0] == 0) return 1;

    /* Some stub STMT spans can cover multiple semicolon-terminated statements (especially after
       other rewrites inserted helper decls + await statements). Split and emit each. */
    {
        char** parts = NULL;
        int pn = 0;
        if (!cc__split_top_level_semis(p, &parts, &pn)) return 0;
        if (pn > 1) {
            for (int i = 0; i < pn; i++) {
                int ok = cc__emit_semi_like(e, parts[i]);
                free(parts[i]);
                if (!ok) { free(parts); return 0; }
            }
            free(parts);
            return 1;
        }
        if (pn == 1) {
            p = parts[0];
            /* fall through; free at end of function */
        } else {
            free(parts);
        }
        if (pn == 1) {
            /* keep p allocated until end */
        }
    }

    if (strncmp(p, "return", 6) == 0 && !cc__is_ident_char(p[6])) {
        const char* rp = cc__skip_ws(p + 6);
        if (e->ret_is_void && rp[0] == 0) {
            cc__sb_append_cstr(e->out, e->out_len, e->out_cap, "__f->__r=0;__f->__st=999;return CC_FUTURE_PENDING;\n");
            cc__emit_close_case(e);
            *e->finished = 1;
            return 0;
        }
        int aw_next = 0;
        char* expr2 = cc__emit_awaits_in_expr(e, rp, &aw_next);
        if (!expr2) return 0;
        char* ex3 = cc__rewrite_idents(expr2, e->map_names, e->map_repls, e->map_n);
        free(expr2);
        if (!ex3) return 0;
        cc__sb_append_fmt(e->out, e->out_len, e->out_cap, "__f->__r=(intptr_t)(%s);__f->__st=999;return CC_FUTURE_PENDING;\n", ex3);
        free(ex3);
        cc__emit_close_case(e);
        *e->finished = 1;
        return 0;
    }

    if (strncmp(p, "break", 5) == 0 && !cc__is_ident_char(p[5])) {
        if (e->loop_depth <= 0) return 0;
        int bs = e->break_state[e->loop_depth - 1];
        cc__sb_append_fmt(e->out, e->out_len, e->out_cap, "__f->__st=%d;return CC_FUTURE_PENDING;\n", bs);
        cc__emit_close_case(e);
        *e->finished = 1;
        return 0;
    }

    if (strncmp(p, "continue", 8) == 0 && !cc__is_ident_char(p[8])) {
        if (e->loop_depth <= 0) return 0;
        int cs = e->cont_state[e->loop_depth - 1];
        cc__sb_append_fmt(e->out, e->out_len, e->out_cap, "__f->__st=%d;return CC_FUTURE_PENDING;\n", cs);
        cc__emit_close_case(e);
        *e->finished = 1;
        return 0;
    }

    /* declaration-like (int/intptr_t/CCAbIntptr) -> hoisted; emit initializer as assignment */
    if (strncmp(p, "int ", 4) == 0 || strncmp(p, "intptr_t ", 9) == 0 || strncmp(p, "CCAbIntptr ", 10) == 0) {
        /* parse name */
        const char* q = p;
        if (strncmp(q, "int ", 4) == 0) q += 4;
        else if (strncmp(q, "intptr_t ", 9) == 0) q += 9;
        else q += 10;
        q = cc__skip_ws(q);
        const char* ns = q;
        if (!cc__is_ident_start(*q)) return 1;
        q++;
        while (cc__is_ident_char(*q)) q++;
        size_t nn = (size_t)(q - ns);
        char nm[128];
        if (nn == 0 || nn >= sizeof(nm)) return 1;
        memcpy(nm, ns, nn);
        nm[nn] = 0;

        /* Only rewrite as an assignment if this name is in the frame mapping.
           Rewriter-introduced temporaries (e.g. auto-blocking `CCAbIntptr __cc_ab_*`) are not present in the
           original stub-AST and should remain as locals within the current state. */
        int is_frame = 0;
        for (int k = 0; k < e->map_n; k++) {
            if (e->map_names[k] && strcmp(e->map_names[k], nm) == 0) { is_frame = 1; break; }
        }
        if (!is_frame) {
            /* fall through to generic handling (emit declaration as-is, with ident rewrites in initializer) */
        } else {
        q = cc__skip_ws(q);
        if (*q != '=') return 1;
        q++;
        q = cc__skip_ws(q);
        int aw_next = 0;
        char* init2 = cc__emit_awaits_in_expr(e, q, &aw_next);
        if (!init2) return 0;
        char* lhs2 = cc__rewrite_idents(nm, e->map_names, e->map_repls, e->map_n);
        char* rhs2 = cc__rewrite_idents(init2, e->map_names, e->map_repls, e->map_n);
        free(init2);
        if (lhs2 && rhs2) cc__sb_append_fmt(e->out, e->out_len, e->out_cap, "%s=(intptr_t)(%s);\n", lhs2, rhs2);
        free(lhs2);
        free(rhs2);
        return 1;
        }
    }

    /* Generic: emit awaits inside expression statement by rewriting and then output as-is */
    {
        int aw_next = 0;
        char* t2 = cc__emit_awaits_in_expr(e, p, &aw_next);
        if (!t2) return 0;
        char* t3 = cc__rewrite_idents(t2, e->map_names, e->map_repls, e->map_n);
        free(t2);
        if (t3) {
            cc__sb_append_fmt(e->out, e->out_len, e->out_cap, "%s;\n", t3);
            free(t3);
        }
    }
    /* free single-part allocation if we created it */
    if (p != text) free((void*)p);
    return 1;
}

static int cc__emit_stmt_list(Emit* e, const Stmt* st, int n) {
    if (!e || !st) return 1;
    for (int i = 0; i < n && e->finished && !*e->finished; i++) {
        const Stmt* s = &st[i];
        if (s->kind == ST_BLOCK) {
            (void)cc__emit_stmt_list(e, s->then_st, s->then_n);
            continue;
        }
        if (s->kind == ST_SEMI || s->kind == ST_RETURN) {
            if (!cc__emit_semi_like(e, s->text ? s->text : "")) return 0;
            continue;
        }

        if (s->kind == ST_IF) {
            int aw_next = 0;
            char* cond2 = cc__emit_awaits_in_expr(e, s->cond ? s->cond : "0", &aw_next);
            if (!cond2) return 0;
            char* cond3 = cc__rewrite_idents(cond2, e->map_names, e->map_repls, e->map_n);
            free(cond2);
            if (!cond3) return 0;

            int then_state = cc__alloc_state(e);
            int else_state = cc__alloc_state(e);
            int after_state = cc__alloc_state(e);

            cc__sb_append_fmt(e->out, e->out_len, e->out_cap,
                              "int __cc_if_c%d=(%s);__f->__st=__cc_if_c%d?%d:%d;return CC_FUTURE_PENDING;\n",
                              then_state, cond3, then_state, then_state, (s->else_n ? else_state : after_state));
            free(cond3);
            cc__emit_close_case(e);

            cc__emit_open_case(e, then_state);
            {
                int done = 0;
                Emit sub = *e;
                sub.finished = &done;
                (void)cc__emit_stmt_list(&sub, s->then_st, s->then_n);
                if (!done) {
                    cc__sb_append_fmt(e->out, e->out_len, e->out_cap, "__f->__st=%d;return CC_FUTURE_PENDING;\n", after_state);
                    cc__emit_close_case(e);
                }
            }

            if (s->else_n) {
                cc__emit_open_case(e, else_state);
                {
                    int done = 0;
                    Emit sub = *e;
                    sub.finished = &done;
                    (void)cc__emit_stmt_list(&sub, s->else_st, s->else_n);
                    if (!done) {
                        cc__sb_append_fmt(e->out, e->out_len, e->out_cap, "__f->__st=%d;return CC_FUTURE_PENDING;\n", after_state);
                        cc__emit_close_case(e);
                    }
                }
            }

            cc__emit_open_case(e, after_state);
            continue;
        }

        if (s->kind == ST_WHILE) {
            int cond_state = cc__alloc_state(e);
            int body_state = cc__alloc_state(e);
            int after_state = cc__alloc_state(e);

            cc__sb_append_fmt(e->out, e->out_len, e->out_cap, "__f->__st=%d;return CC_FUTURE_PENDING;\n", cond_state);
            cc__emit_close_case(e);

            /* loop context */
            if (e->loop_depth < 64) {
                e->break_state[e->loop_depth] = after_state;
                e->cont_state[e->loop_depth] = cond_state;
                e->loop_depth++;
            }

            cc__emit_open_case(e, cond_state);
            {
                int aw_next = 0;
                char* cond2 = cc__emit_awaits_in_expr(e, s->cond ? s->cond : "0", &aw_next);
                if (!cond2) return 0;
                char* cond3 = cc__rewrite_idents(cond2, e->map_names, e->map_repls, e->map_n);
                free(cond2);
                if (!cond3) return 0;
                cc__sb_append_fmt(e->out, e->out_len, e->out_cap,
                                  "int __cc_wh_c%d=(%s);__f->__st=__cc_wh_c%d?%d:%d;return CC_FUTURE_PENDING;\n",
                                  cond_state, cond3, cond_state, body_state, after_state);
                free(cond3);
                cc__emit_close_case(e);
            }

            cc__emit_open_case(e, body_state);
            {
                int done = 0;
                Emit sub = *e;
                sub.finished = &done;
                (void)cc__emit_stmt_list(&sub, s->then_st, s->then_n);
                if (!done) {
                    cc__sb_append_fmt(e->out, e->out_len, e->out_cap, "__f->__st=%d;return CC_FUTURE_PENDING;\n", cond_state);
                    cc__emit_close_case(e);
                }
            }

            if (e->loop_depth > 0) e->loop_depth--;
            cc__emit_open_case(e, after_state);
            continue;
        }

        if (s->kind == ST_FOR) {
            int init_state = cc__alloc_state(e);
            int cond_state = cc__alloc_state(e);
            int body_state = cc__alloc_state(e);
            int post_state = cc__alloc_state(e);
            int after_state = cc__alloc_state(e);

            cc__sb_append_fmt(e->out, e->out_len, e->out_cap, "__f->__st=%d;return CC_FUTURE_PENDING;\n", init_state);
            cc__emit_close_case(e);

            /* loop context */
            if (e->loop_depth < 64) {
                e->break_state[e->loop_depth] = after_state;
                e->cont_state[e->loop_depth] = post_state; /* continue in for runs post */
                e->loop_depth++;
            }

            /* init */
            cc__emit_open_case(e, init_state);
            if (s->for_init && cc__skip_ws(s->for_init)[0]) {
                if (!cc__emit_semi_like(e, s->for_init)) return 0;
            }
            cc__sb_append_fmt(e->out, e->out_len, e->out_cap, "__f->__st=%d;return CC_FUTURE_PENDING;\n", cond_state);
            cc__emit_close_case(e);

            /* cond */
            cc__emit_open_case(e, cond_state);
            {
                int aw_next = 0;
                char* cond2 = cc__emit_awaits_in_expr(e, (s->cond && cc__skip_ws(s->cond)[0]) ? s->cond : "1", &aw_next);
                if (!cond2) return 0;
                char* cond3 = cc__rewrite_idents(cond2, e->map_names, e->map_repls, e->map_n);
                free(cond2);
                if (!cond3) return 0;
                cc__sb_append_fmt(e->out, e->out_len, e->out_cap,
                                  "int __cc_for_c%d=(%s);__f->__st=__cc_for_c%d?%d:%d;return CC_FUTURE_PENDING;\n",
                                  cond_state, cond3, cond_state, body_state, after_state);
                free(cond3);
                cc__emit_close_case(e);
            }

            /* body */
            cc__emit_open_case(e, body_state);
            {
                int done = 0;
                Emit sub = *e;
                sub.finished = &done;
                (void)cc__emit_stmt_list(&sub, s->then_st, s->then_n);
                if (!done) {
                    cc__sb_append_fmt(e->out, e->out_len, e->out_cap, "__f->__st=%d;return CC_FUTURE_PENDING;\n", post_state);
                    cc__emit_close_case(e);
                }
            }

            /* post */
            cc__emit_open_case(e, post_state);
            if (s->for_post && cc__skip_ws(s->for_post)[0]) {
                if (!cc__emit_semi_like(e, s->for_post)) return 0;
            }
            cc__sb_append_fmt(e->out, e->out_len, e->out_cap, "__f->__st=%d;return CC_FUTURE_PENDING;\n", cond_state);
            cc__emit_close_case(e);

            if (e->loop_depth > 0) e->loop_depth--;
            cc__emit_open_case(e, after_state);
            continue;
        }

        return 0;
    }
    return 1;
}

int cc_async_rewrite_state_machine_ast(const CCASTRoot* root,
                                       const CCVisitorCtx* ctx,
                                       const char* in_src,
                                       size_t in_len,
                                       char** out_src,
                                       size_t* out_len) {
    fprintf(stderr, "CC: async_ast: starting async lowering, root->node_count=%d\n", root ? root->node_count : 0);
    if (!root || !ctx || !in_src || !out_src || !out_len) return 0;
    *out_src = NULL;
    *out_len = 0;
    if (!root->nodes || root->node_count <= 0) return 0;

    const NodeView* n = (const NodeView*)root->nodes;

    typedef struct {
        int decl_item_idx;
        int body_block_idx;
        size_t start;
        size_t end;
        size_t lbrace;
        size_t rbrace;
        char name[128];
        int ret_is_void;
    } AF;

    AF fns[256];
    int fn_n = 0;

    for (int i = 0; i < root->node_count && fn_n < 256; i++) {
        if (n[i].kind != CC_AST_NODE_DECL_ITEM) continue;
        if (!cc__node_in_this_tu(root, ctx, n[i].file)) continue;
        if (((unsigned int)n[i].aux2 & (1u << 0)) == 0) continue; /* async */
        if (!n[i].aux_s1) continue;

        int body_block = -1;

        /* Compute span by brace matching in the *current* source.
           Stub-AST block node spans can be short for function bodies (decls are tracked under a child DECL node),
           so we avoid using `body_block`'s end span for replacement. */
        int ls = n[i].line_start > 0 ? n[i].line_start : 1;
        size_t s0 = cc__offset_of_line_1based(in_src, in_len, ls);
        size_t scan = s0;
        /* Find '@async' near the declaration line (best-effort), otherwise start at line. */
        for (size_t t = s0; t + 6 < in_len && t < s0 + 512; t++) {
            if (in_src[t] == '@') {
                size_t u = t + 1;
                while (u < in_len && (in_src[u] == ' ' || in_src[u] == '\t')) u++;
                if (u + 5 < in_len && memcmp(in_src + u, "async", 5) == 0) { scan = t; break; }
            }
        }
        size_t lbrace = 0, rbrace = 0;
        /* Find first '{' after the function name. */
        const char* nm = n[i].aux_s1;
        const char* hit = nm ? strstr(in_src + scan, nm) : NULL;
        size_t p = hit ? (size_t)(hit - in_src) : scan;
        for (; p < in_len; p++) {
            if (in_src[p] == '{') { lbrace = p; break; }
        }
        size_t e = scan;
        if (lbrace && cc__find_matching_brace(in_src, in_len, lbrace, &rbrace)) {
            e = rbrace + 1;
            while (e < in_len && in_src[e] != '\n') e++;
            if (e < in_len) e++;
        } else {
            /* Fallback: use decl line only. */
            e = cc__offset_of_line_1based(in_src, in_len, ls + 1);
        }
        size_t s = scan;

        /* Find the best body block: scan all BLOCK nodes in the function's subtree and pick the one
           whose span encloses the function braces with the tightest span. */
        if (lbrace && rbrace) {
            int best = -1;
            size_t best_span = (size_t)-1;
            for (int b = 0; b < root->node_count; b++) {
                if (n[b].kind != CC_AST_NODE_BLOCK) continue;
                if (!cc__node_in_this_tu(root, ctx, n[b].file)) continue;
                if (!cc__node_is_descendant_of(n, b, i)) continue;
                size_t bs = cc__node_start_off(in_src, in_len, &n[b]);
                size_t be = cc__node_end_off(in_src, in_len, &n[b]);
                if (!(be > bs && be <= in_len)) {
                    int bls = n[b].line_start > 0 ? n[b].line_start : 1;
                    int ble = n[b].line_end > 0 ? n[b].line_end : bls;
                    bs = cc__offset_of_line_1based(in_src, in_len, bls);
                    be = cc__offset_of_line_1based(in_src, in_len, ble + 1);
                }
                if (be > in_len) be = in_len;
                if (be < bs) be = bs;
                if (bs <= lbrace && be >= (rbrace + 1)) {
                    size_t span = be - bs;
                    if (span < best_span) { best = b; best_span = span; }
                }
            }
            if (best >= 0) body_block = best;
        }

        AF fn;
        memset(&fn, 0, sizeof(fn));
        fn.decl_item_idx = i;
        fn.body_block_idx = body_block;
        fn.start = s;
        fn.end = e;
        fn.lbrace = lbrace;
        fn.rbrace = rbrace;
        strncpy(fn.name, n[i].aux_s1, sizeof(fn.name) - 1);
        fn.ret_is_void = 0;
        if (n[i].aux_s2 && strstr(n[i].aux_s2, "void") == n[i].aux_s2) fn.ret_is_void = 1;
        fns[fn_n++] = fn;
    }

    if (fn_n == 0) return 0;

    if (getenv("CC_DEBUG_ASYNC_AST")) {
        fprintf(stderr, "CC: async_ast: found %d @async functions in reparse stub-AST\n", fn_n);
        for (int i = 0; i < fn_n; i++) fprintf(stderr, "CC: async_ast:   - %s\n", fns[i].name);
    }

    char* cur = (char*)malloc(in_len + 1);
    if (!cur) return -1;
    memcpy(cur, in_src, in_len);
    cur[in_len] = 0;
    size_t cur_len = in_len;

    static int g_async_id = 60000;

    for (int fi = fn_n - 1; fi >= 0; fi--) {
        AF* fn = &fns[fi];
        int id = g_async_id++;

        /* Build structured stmt list from body block. */
        Stmt* st = NULL;
        int st_n = 0;
        if (fn->body_block_idx >= 0) {
            if (!cc__build_stmt_list_from_block(root, ctx, n, cur, cur_len, fn->body_block_idx, &st, &st_n)) {
                fprintf(stderr, "CC: async_ast: failed to build statement list for @async function '%s' (body block idx=%d)\n",
                        fn->name, fn->body_block_idx);
                free(cur);
                return -1;
            }
        } else {
            if (!cc__build_stmt_list_from_text_body(cur, cur_len, fn->lbrace, fn->rbrace, &st, &st_n)) {
                fprintf(stderr, "CC: async_ast: failed to parse statement list for @async function '%s' (text body fallback)\n", fn->name);
                free(cur);
                return -1;
            }
        }
        cc__debug_dump_stmt_list(fn->name, st, st_n, 0);

        /* Collect locals names using DECL_ITEM nodes in function subtree. */
        char* locals[256];
        int local_n = 0;
        memset(locals, 0, sizeof(locals));
        for (int i = 0; i < root->node_count && local_n < 256; i++) {
            if (n[i].kind != CC_AST_NODE_DECL_ITEM) continue;
            if (!cc__node_in_this_tu(root, ctx, n[i].file)) continue;
            if (!n[i].aux_s1) continue;
            /* Only hoist scalar locals we currently model in the frame as intptr_t. */
            if (!n[i].aux_s2) continue;
            if (!(strcmp(n[i].aux_s2, "int") == 0 || strcmp(n[i].aux_s2, "intptr_t") == 0)) continue;
            /* Avoid hoisting compiler-introduced temporaries / closure locals; keep them as locals in the current state. */
            if (strncmp(n[i].aux_s1, "__cc_ab_", 7) == 0) continue;
            if (i == fn->decl_item_idx) continue;
            /* ensure in subtree */
            int p = n[i].parent;
            int ok = 0;
            while (p >= 0) { if (p == fn->decl_item_idx) { ok = 1; break; } p = n[p].parent; }
            if (!ok) continue;
            int dup = 0;
            for (int k = 0; k < local_n; k++) if (locals[k] && strcmp(locals[k], n[i].aux_s1) == 0) dup = 1;
            if (dup) continue;
            locals[local_n++] = strdup(n[i].aux_s1);
        }

        /* Also collect declaration-like names from the already-built statement list.
           This picks up rewrite-introduced temps like `intptr_t __cc_ab_expr_*` / `intptr_t __cc_aw_l*_N`
           that are not present in the stub-AST DECL_ITEM stream but must live in the frame across awaits. */
        cc__collect_decl_names_from_stmt_list(st, st_n, locals, &local_n, 256);

        /* Count awaits in subtree; add __cc_awN temps */
        int aw_total = 0;
        for (int i = 0; i < root->node_count; i++) {
            if (n[i].kind != CC_AST_NODE_AWAIT) continue;
            if (!cc__node_in_this_tu(root, ctx, n[i].file)) continue;
            int p = n[i].parent;
            int ok = 0;
            while (p >= 0) { if (p == fn->decl_item_idx) { ok = 1; break; } p = n[p].parent; }
            if (ok) aw_total++;
        }
        if (aw_total > 64) aw_total = 64;
        char* aw_names[64];
        memset(aw_names, 0, sizeof(aw_names));
        for (int i = 0; i < aw_total; i++) {
            char nm[64];
            snprintf(nm, sizeof(nm), "__cc_aw%d", i);
            aw_names[i] = strdup(nm);
        }

        /* Parse params from source slice (best-effort): find name(...). */
        char* params_text = NULL;
        {
            const char* fn_pos = strstr(cur + fn->start, fn->name);
            if (!fn_pos) fn_pos = cur + fn->start;
            const char* lp = strchr(fn_pos, '(');
            if (lp) {
                size_t lpo = (size_t)(lp - cur);
                size_t rpo = 0;
                if (cc__find_matching_paren(cur, cur_len, lpo, &rpo)) {
                    params_text = cc__dup_slice(cur, lpo + 1, rpo);
                }
            }
        }

        /* Extract param names very conservatively: last ident in each comma-separated chunk. */
        char* param_names[64];
        int param_n = 0;
        memset(param_names, 0, sizeof(param_names));
        if (params_text) {
            int par = 0, brk = 0, br = 0;
            int ins = 0; char q = 0;
            int in_lc = 0, in_bc = 0;
            const char* last_ident = NULL;
            size_t last_ident_len = 0;
            size_t pl = strlen(params_text);
            for (size_t i = 0; i <= pl; i++) {
                char ch = params_text[i];
                char ch2 = (i + 1 <= pl) ? params_text[i + 1] : 0;
                int at_end = (ch == 0);
                if (!at_end) {
                    if (in_lc) { if (ch == '\n') in_lc = 0; continue; }
                    if (in_bc) { if (ch == '*' && ch2 == '/') { in_bc = 0; i++; } continue; }
                    if (ins) { if (ch == '\\' && i + 1 < pl) { i++; continue; } if (ch == q) ins = 0; continue; }
                    if (ch == '/' && ch2 == '/') { in_lc = 1; i++; continue; }
                    if (ch == '/' && ch2 == '*') { in_bc = 1; i++; continue; }
                    if (ch == '"' || ch == '\'') { ins = 1; q = ch; continue; }
                    if (ch == '(') par++;
                    else if (ch == ')') { if (par) par--; }
                    else if (ch == '[') brk++;
                    else if (ch == ']') { if (brk) brk--; }
                    else if (ch == '{') br++;
                    else if (ch == '}') { if (br) br--; }
                    if (par || brk || br) continue;
                    if (cc__is_ident_start(ch)) {
                        const char* s0 = &params_text[i];
                        size_t j = i + 1;
                        while (j < pl && cc__is_ident_char(params_text[j])) j++;
                        last_ident = s0;
                        last_ident_len = j - i;
                        i = j - 1;
                        continue;
                    }
                }
                if (at_end || (ch == ',' && par == 0 && brk == 0 && br == 0)) {
                    if (last_ident && last_ident_len && last_ident_len < 128 && param_n < 64) {
                        char* nm = (char*)malloc(last_ident_len + 1);
                        if (nm) {
                            memcpy(nm, last_ident, last_ident_len);
                            nm[last_ident_len] = 0;
                            if (strcmp(nm, "void") != 0) param_names[param_n++] = nm;
                            else free(nm);
                        }
                    }
                    last_ident = NULL;
                    last_ident_len = 0;
                }
            }
        }

        /* Build ident map: locals + await temps + params */
        const char* map_names[512];
        const char* map_repls[512];
        int map_n = 0;
        for (int k = 0; k < local_n && map_n < 512; k++) {
            map_names[map_n] = locals[k];
            char* r = (char*)malloc(strlen(locals[k]) + 8);
            if (!r) continue;
            sprintf(r, "__f->%s", locals[k]);
            map_repls[map_n++] = r;
        }
        for (int k = 0; k < aw_total && map_n < 512; k++) {
            map_names[map_n] = aw_names[k];
            char* r = (char*)malloc(strlen(aw_names[k]) + 8);
            if (!r) continue;
            sprintf(r, "__f->%s", aw_names[k]);
            map_repls[map_n++] = r;
        }
        for (int k = 0; k < param_n && map_n < 512; k++) {
            map_names[map_n] = param_names[k];
            char* r = (char*)malloc(strlen(param_names[k]) + 16);
            if (!r) continue;
            sprintf(r, "__f->__p_%s", param_names[k]);
            map_repls[map_n++] = r;
        }

        /* Emit */
        char* repl = NULL;
        size_t repl_len = 0, repl_cap = 0;

        cc__sb_append_fmt(&repl, &repl_len, &repl_cap, "typedef struct{int __st; intptr_t __r;");
        for (int k = 0; k < local_n; k++) cc__sb_append_fmt(&repl, &repl_len, &repl_cap, " intptr_t %s;", locals[k]);
        for (int k = 0; k < aw_total; k++) cc__sb_append_fmt(&repl, &repl_len, &repl_cap, " intptr_t %s;", aw_names[k]);
        for (int k = 0; k < param_n; k++) cc__sb_append_fmt(&repl, &repl_len, &repl_cap, " intptr_t __p_%s;", param_names[k]);
        for (int k = 0; k < 16; k++) cc__sb_append_fmt(&repl, &repl_len, &repl_cap, " CCTaskIntptr __t%d;", k);
        cc__sb_append_fmt(&repl, &repl_len, &repl_cap, "}__cc_af%d_f;", id);

        cc__sb_append_fmt(&repl, &repl_len, &repl_cap,
                          "static CCFutureStatus __cc_af%d_poll(void*__p,intptr_t*__o,int*__e){__cc_af%d_f*__f=(__cc_af%d_f*)__p;if(!__f)return CC_FUTURE_ERR;switch(__f->__st){case 0:__f->__st=1;/*fall*/case 1:{\n",
                          id, id, id);

        int cur_state = 1;
        int next_state = 2;
        int task_idx = 0;
        int finished = 0;
        Emit em = {
            .out = &repl, .out_len = &repl_len, .out_cap = &repl_cap,
            .map_names = map_names, .map_repls = map_repls, .map_n = map_n,
            .cur_state = &cur_state, .next_state = &next_state, .task_idx = &task_idx,
            .ret_is_void = fn->ret_is_void,
            .finished = &finished,
            .loop_depth = 0,
        };
        (void)cc__emit_stmt_list(&em, st, st_n);

        if (!finished) {
            cc__sb_append_cstr(&repl, &repl_len, &repl_cap, "__f->__r=0;__f->__st=999;return CC_FUTURE_PENDING;\n");
            cc__emit_close_case(&em);
        }
        cc__sb_append_cstr(&repl, &repl_len, &repl_cap, "case 999:{if(__o)*__o=__f->__r;return CC_FUTURE_READY;}default:return CC_FUTURE_ERR;}return CC_FUTURE_ERR;}\n");

        cc__sb_append_fmt(&repl, &repl_len, &repl_cap,
                          "static void __cc_af%d_drop(void*__p){__cc_af%d_f*__f=(__cc_af%d_f*)__p;if(!__f)return;",
                          id, id, id);
        for (int k = 0; k < 16; k++) cc__sb_append_fmt(&repl, &repl_len, &repl_cap, "cc_task_intptr_free(&__f->__t%d);", k);
        cc__sb_append_cstr(&repl, &repl_len, &repl_cap, "free(__f);}\n");

        /* Emit function signature as `CCTaskIntptr name(<params>)` */
        cc__sb_append_fmt(&repl, &repl_len, &repl_cap, "CCTaskIntptr %s(%s){", fn->name,
                          (params_text && strlen(params_text) > 0) ? params_text : "void");
        cc__sb_append_fmt(&repl, &repl_len, &repl_cap,
                          "__cc_af%d_f*__f=(__cc_af%d_f*)calloc(1,sizeof(__cc_af%d_f));if(!__f){CCTaskIntptr __t;memset(&__t,0,sizeof(__t));return __t;}__f->__st=0;",
                          id, id, id);
        for (int k = 0; k < param_n; k++) {
            cc__sb_append_fmt(&repl, &repl_len, &repl_cap, "__f->__p_%s=(intptr_t)(%s);", param_names[k], param_names[k]);
        }
        cc__sb_append_fmt(&repl, &repl_len, &repl_cap,
                          "return cc_task_intptr_make_poll_ex(__cc_af%d_poll,NULL,__f,__cc_af%d_drop);}\n",
                          id, id);

        /* Replace original span */
        size_t rs = fn->start;
        size_t re = fn->end;
        if (rs > cur_len) rs = cur_len;
        if (re > cur_len) re = cur_len;
        if (re < rs) re = rs;
        size_t old_len = re - rs;
        size_t new_len = cur_len - old_len + repl_len;
        char* next = (char*)malloc(new_len + 1);
        if (!next) {
            cc__free_stmt_list(st, st_n);
            free(st);
            free(repl);
            free(cur);
            return -1;
        }
        memcpy(next, cur, rs);
        memcpy(next + rs, repl, repl_len);
        memcpy(next + rs + repl_len, cur + re, cur_len - re);
        next[new_len] = 0;
        free(cur);
        cur = next;
        cur_len = new_len;

        cc__free_stmt_list(st, st_n);
        free(st);

        for (int k = 0; k < local_n; k++) free(locals[k]);
        for (int k = 0; k < aw_total; k++) free(aw_names[k]);
        for (int k = 0; k < param_n; k++) free(param_names[k]);
        for (int k = 0; k < map_n; k++) free((void*)map_repls[k]);
        free(params_text);
        free(repl);
    }

    *out_src = cur;
    *out_len = cur_len;
    return 1;
}
