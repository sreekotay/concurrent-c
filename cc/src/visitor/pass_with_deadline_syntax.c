#include "pass_with_deadline_syntax.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int cc__is_ident_start(char c) {
    return (c == '_' || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'));
}
static int cc__is_ident_char(char c) {
    return cc__is_ident_start(c) || (c >= '0' && c <= '9');
}

static void cc__sb_append(char** buf, size_t* len, size_t* cap, const char* s, size_t n) {
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

static void cc__sb_append_cstr(char** buf, size_t* len, size_t* cap, const char* s) {
    if (!s) return;
    cc__sb_append(buf, len, cap, s, strlen(s));
}

int cc__rewrite_with_deadline_syntax(const char* src, size_t n, char** out_src, size_t* out_len) {
    if (!out_src || !out_len) return -1;
    *out_src = NULL;
    *out_len = 0;
    if (!src || n == 0) return 0;

    char* out = NULL;
    size_t olen = 0, ocap = 0;
    size_t i = 0;
    int in_line_comment = 0;
    int in_block_comment = 0;
    int in_str = 0;
    int in_chr = 0;
    unsigned long counter = 0;

    while (i < n) {
        char c = src[i];
        char c2 = (i + 1 < n) ? src[i + 1] : 0;

        if (in_line_comment) {
            cc__sb_append(&out, &olen, &ocap, &c, 1);
            if (c == '\n') in_line_comment = 0;
            i++;
            continue;
        }
        if (in_block_comment) {
            cc__sb_append(&out, &olen, &ocap, &c, 1);
            if (c == '*' && c2 == '/') {
                cc__sb_append(&out, &olen, &ocap, &c2, 1);
                i += 2;
                in_block_comment = 0;
                continue;
            }
            i++;
            continue;
        }
        if (in_str) {
            cc__sb_append(&out, &olen, &ocap, &c, 1);
            if (c == '\\' && i + 1 < n) {
                cc__sb_append(&out, &olen, &ocap, &c2, 1);
                i += 2;
                continue;
            }
            if (c == '"') in_str = 0;
            i++;
            continue;
        }
        if (in_chr) {
            cc__sb_append(&out, &olen, &ocap, &c, 1);
            if (c == '\\' && i + 1 < n) {
                cc__sb_append(&out, &olen, &ocap, &c2, 1);
                i += 2;
                continue;
            }
            if (c == '\'') in_chr = 0;
            i++;
            continue;
        }

        if (c == '/' && c2 == '/') {
            cc__sb_append(&out, &olen, &ocap, &c, 1);
            cc__sb_append(&out, &olen, &ocap, &c2, 1);
            i += 2;
            in_line_comment = 1;
            continue;
        }
        if (c == '/' && c2 == '*') {
            cc__sb_append(&out, &olen, &ocap, &c, 1);
            cc__sb_append(&out, &olen, &ocap, &c2, 1);
            i += 2;
            in_block_comment = 1;
            continue;
        }
        if (c == '"') {
            cc__sb_append(&out, &olen, &ocap, &c, 1);
            i++;
            in_str = 1;
            continue;
        }
        if (c == '\'') {
            cc__sb_append(&out, &olen, &ocap, &c, 1);
            i++;
            in_chr = 1;
            continue;
        }

        if (cc__is_ident_start(c)) {
            size_t s0 = i;
            i++;
            while (i < n && cc__is_ident_char(src[i])) i++;
            size_t sl = i - s0;
            int is_wd = (sl == strlen("with_deadline") && memcmp(src + s0, "with_deadline", sl) == 0);
            if (!is_wd || (s0 > 0 && cc__is_ident_char(src[s0 - 1]))) {
                cc__sb_append(&out, &olen, &ocap, src + s0, sl);
                continue;
            }

            size_t j = i;
            while (j < n && (src[j] == ' ' || src[j] == '\t' || src[j] == '\r' || src[j] == '\n')) j++;
            if (j >= n || src[j] != '(') {
                cc__sb_append(&out, &olen, &ocap, src + s0, sl);
                i = j;
                continue;
            }

            size_t expr_l = j + 1;
            int par = 1;
            int in_s2 = 0, in_lc2 = 0, in_bc2 = 0;
            char q2 = 0;
            size_t k = expr_l;
            for (; k < n; k++) {
                char ch = src[k];
                char ch2 = (k + 1 < n) ? src[k + 1] : 0;
                if (in_lc2) { if (ch == '\n') in_lc2 = 0; continue; }
                if (in_bc2) { if (ch == '*' && ch2 == '/') { in_bc2 = 0; k++; } continue; }
                if (in_s2) {
                    if (ch == '\\' && k + 1 < n) { k++; continue; }
                    if (ch == q2) in_s2 = 0;
                    continue;
                }
                if (ch == '/' && ch2 == '/') { in_lc2 = 1; k++; continue; }
                if (ch == '/' && ch2 == '*') { in_bc2 = 1; k++; continue; }
                if (ch == '"' || ch == '\'') { in_s2 = 1; q2 = ch; continue; }
                if (ch == '(') par++;
                else if (ch == ')') { par--; if (par == 0) break; }
            }
            if (k >= n || par != 0) {
                cc__sb_append(&out, &olen, &ocap, src + s0, sl);
                i = j;
                continue;
            }

            size_t expr_r = k;
            size_t after_paren = expr_r + 1;
            while (after_paren < n && (src[after_paren] == ' ' || src[after_paren] == '\t' || src[after_paren] == '\r' || src[after_paren] == '\n')) after_paren++;
            if (after_paren >= n || src[after_paren] != '{') {
                cc__sb_append(&out, &olen, &ocap, src + s0, sl);
                i = j;
                continue;
            }

            size_t body_s = after_paren;
            int br = 1;
            int in_s3 = 0, in_lc3 = 0, in_bc3 = 0;
            char q3 = 0;
            size_t m = body_s + 1;
            for (; m < n; m++) {
                char ch = src[m];
                char ch2 = (m + 1 < n) ? src[m + 1] : 0;
                if (in_lc3) { if (ch == '\n') in_lc3 = 0; continue; }
                if (in_bc3) { if (ch == '*' && ch2 == '/') { in_bc3 = 0; m++; } continue; }
                if (in_s3) {
                    if (ch == '\\' && m + 1 < n) { m++; continue; }
                    if (ch == q3) in_s3 = 0;
                    continue;
                }
                if (ch == '/' && ch2 == '/') { in_lc3 = 1; m++; continue; }
                if (ch == '/' && ch2 == '*') { in_bc3 = 1; m++; continue; }
                if (ch == '"' || ch == '\'') { in_s3 = 1; q3 = ch; continue; }
                if (ch == '{') br++;
                else if (ch == '}') { br--; if (br == 0) { m++; break; } }
            }
            if (m > n || br != 0) {
                cc__sb_append(&out, &olen, &ocap, src + s0, sl);
                i = j;
                continue;
            }

            size_t body_e = m;

            counter++;
            char hdr[512];
            snprintf(hdr, sizeof(hdr),
                     "{ CCDeadline __cc_dl%lu = cc_deadline_after_ms((uint64_t)(%.*s)); "
                     "CCDeadline* __cc_prev%lu = cc_deadline_push(&__cc_dl%lu); "
                     "@defer cc_deadline_pop(__cc_prev%lu); ",
                     counter,
                     (int)(expr_r - expr_l), src + expr_l,
                     counter, counter, counter);
            cc__sb_append_cstr(&out, &olen, &ocap, hdr);
            cc__sb_append(&out, &olen, &ocap, src + body_s, body_e - body_s);
            cc__sb_append_cstr(&out, &olen, &ocap, " }");

            i = body_e;
            continue;
        }

        cc__sb_append(&out, &olen, &ocap, &c, 1);
        i++;
    }

    *out_src = out ? out : strdup(src);
    *out_len = out ? olen : n;
    return 0;
}

