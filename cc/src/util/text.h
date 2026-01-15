/*
 * Shared text manipulation helpers for compiler passes.
 * 
 * These are static inline to avoid link-time coupling between passes
 * while eliminating code duplication.
 */
#ifndef CC_UTIL_TEXT_H
#define CC_UTIL_TEXT_H

#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- Identifier character predicates ---- */

static inline int cc_is_ident_start(char c) {
    return (c == '_') || ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'));
}

static inline int cc_is_ident_char(char c) {
    return cc_is_ident_start(c) || (c >= '0' && c <= '9');
}

/* ---- Whitespace helpers ---- */

static inline const char* cc_skip_ws(const char* s) {
    if (!s) return s;
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') s++;
    return s;
}

static inline size_t cc_skip_ws_len(const char* s, size_t len, size_t start) {
    while (start < len && (s[start] == ' ' || s[start] == '\t' || 
                           s[start] == '\r' || s[start] == '\n')) {
        start++;
    }
    return start;
}

/* ---- String builder (dynamic buffer) ---- */

static inline void cc_sb_append(char** buf, size_t* len, size_t* cap, const char* s, size_t n) {
    if (!buf || !len || !cap || !s || n == 0) return;
    size_t need = *len + n + 1;
    if (need > *cap) {
        size_t nc = (*cap ? *cap * 2 : 1024);
        while (nc < need) nc *= 2;
        char* nb = (char*)realloc(*buf, nc);
        if (!nb) return;
        *buf = nb;
        *cap = nc;
    }
    memcpy(*buf + *len, s, n);
    *len += n;
    (*buf)[*len] = 0;
}

static inline void cc_sb_append_cstr(char** buf, size_t* len, size_t* cap, const char* s) {
    if (!s) return;
    cc_sb_append(buf, len, cap, s, strlen(s));
}

static inline void cc_sb_append_vfmt(char** buf, size_t* len, size_t* cap, const char* fmt, va_list ap) {
    if (!buf || !len || !cap || !fmt) return;
    if (!*buf) {
        *cap = 256;
        *buf = (char*)malloc(*cap);
        if (!*buf) return;
        (*buf)[0] = 0;
        *len = 0;
    }
    for (;;) {
        size_t avail = (*cap > *len) ? (*cap - *len) : 0;
        va_list ap2;
        va_copy(ap2, ap);
        int n = vsnprintf(*buf + *len, avail, fmt, ap2);
        va_end(ap2);
        if (n < 0) return;
        if ((size_t)n < avail) { *len += (size_t)n; return; }
        *cap = (*cap ? *cap * 2 : 256) + (size_t)n + 16;
        *buf = (char*)realloc(*buf, *cap);
        if (!*buf) return;
    }
}

/* Note: cc_sb_append_fmt is a variadic wrapper - define in each file to avoid inline+variadic issues */
#define CC_DEFINE_SB_APPEND_FMT \
    static void cc_sb_append_fmt(char** buf, size_t* len, size_t* cap, const char* fmt, ...) { \
        va_list ap; \
        va_start(ap, fmt); \
        cc_sb_append_vfmt(buf, len, cap, fmt, ap); \
        va_end(ap); \
    }

/* ---- String duplication helpers ---- */

static inline char* cc_strndup(const char* s, size_t n) {
    if (!s) return NULL;
    char* out = (char*)malloc(n + 1);
    if (!out) return NULL;
    memcpy(out, s, n);
    out[n] = 0;
    return out;
}

static inline char* cc_strndup_trim_ws(const char* s, size_t n) {
    if (!s) return NULL;
    size_t i = 0;
    while (i < n && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) i++;
    size_t j = n;
    while (j > i && (s[j - 1] == ' ' || s[j - 1] == '\t' || s[j - 1] == '\n' || s[j - 1] == '\r')) j--;
    size_t m = (j > i) ? (j - i) : 0;
    char* out = (char*)malloc(m + 1);
    if (!out) return NULL;
    if (m) memcpy(out, s + i, m);
    out[m] = 0;
    return out;
}

static inline char* cc_dup_slice(const char* b, size_t start, size_t end) {
    if (!b || end <= start) return strdup("");
    return cc_strndup(b + start, end - start);
}

/* ---- Matching bracket/paren/brace helpers ---- */

/* Find matching ')' for '(' at position lpar. Returns 1 on success, 0 on failure. */
static inline int cc_find_matching_paren(const char* b, size_t bl, size_t lpar, size_t* out_rpar) {
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

/* Find matching '}' for '{' at position lbrace. Returns 1 on success, 0 on failure. */
static inline int cc_find_matching_brace(const char* b, size_t bl, size_t lbrace, size_t* out_rbrace) {
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

/* Find matching ']' for '[' at position lbrack. Returns 1 on success, 0 on failure. */
static inline int cc_find_matching_bracket(const char* b, size_t bl, size_t lbrack, size_t* out_rbrack) {
    if (!b || lbrack >= bl || b[lbrack] != '[') return 0;
    int brk = 1, par = 0, br = 0;
    int ins = 0; char q = 0;
    int in_lc = 0, in_bc = 0;
    for (size_t p = lbrack + 1; p < bl; p++) {
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
        else if (ch == '{') br++;
        else if (ch == '}') { if (br) br--; }
        else if (ch == '[') brk++;
        else if (ch == ']') { brk--; if (brk == 0) { if (out_rbrack) *out_rbrack = p; return 1; } }
    }
    return 0;
}

/* ---- Skip whitespace and comments ---- */

static inline size_t cc_skip_ws_and_comments(const char* src, size_t len, size_t i) {
    while (i < len) {
        char c = src[i];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') { i++; continue; }
        if (c == '/' && i + 1 < len) {
            if (src[i + 1] == '/') {
                i += 2;
                while (i < len && src[i] != '\n') i++;
                continue;
            }
            if (src[i + 1] == '*') {
                i += 2;
                while (i + 1 < len && !(src[i] == '*' && src[i + 1] == '/')) i++;
                if (i + 1 < len) i += 2;
                continue;
            }
        }
        break;
    }
    return i;
}

/* ---- Token checking ---- */

/* Check if a token exists in a range (word boundaries respected). */
static inline int cc_range_contains_token(const char* s, size_t n, const char* tok) {
    if (!s || !tok) return 0;
    size_t tn = strlen(tok);
    if (tn == 0 || n < tn) return 0;
    for (size_t i = 0; i + tn <= n; i++) {
        if (memcmp(s + i, tok, tn) != 0) continue;
        if (i > 0 && cc_is_ident_char(s[i - 1])) continue;
        if (i + tn < n && cc_is_ident_char(s[i + tn])) continue;
        return 1;
    }
    return 0;
}

#endif /* CC_UTIL_TEXT_H */
