#include "pass_defer_syntax.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    int line_no;
    char* stmt; /* includes trailing ';' */
} CCDeferStmt;

static void cc__free_defer_list(CCDeferStmt* xs, int n) {
    if (!xs) return;
    for (int i = 0; i < n; i++) free(xs[i].stmt);
    free(xs);
}

static int cc__is_ident_start(char c) { return (c == '_' || isalpha((unsigned char)c)); }
static int cc__is_ident_char(char c) { return (c == '_' || isalnum((unsigned char)c)); }

static void cc__append_n(char** out, size_t* out_len, size_t* out_cap, const char* s, size_t n) {
    if (!out || !out_len || !out_cap || !s) return;
    if (*out_len + n + 1 > *out_cap) {
        size_t nc = *out_cap ? (*out_cap * 2) : 1024;
        while (nc < *out_len + n + 1) nc *= 2;
        char* nb = (char*)realloc(*out, nc);
        if (!nb) return;
        *out = nb;
        *out_cap = nc;
    }
    memcpy(*out + *out_len, s, n);
    *out_len += n;
    (*out)[*out_len] = 0;
}

static void cc__append_str(char** out, size_t* out_len, size_t* out_cap, const char* s) {
    if (!s) return;
    cc__append_n(out, out_len, out_cap, s, strlen(s));
}

static int cc__token_is(const char* s, size_t len, size_t i, const char* tok) {
    size_t tn = strlen(tok);
    if (i + tn > len) return 0;
    if (memcmp(s + i, tok, tn) != 0) return 0;
    if (i > 0 && cc__is_ident_char(s[i - 1])) return 0;
    if (i + tn < len && cc__is_ident_char(s[i + tn])) return 0;
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

        /* `@defer ...;` */
        if (cc__token_is(in_src, in_len, i, "@defer")) {
            int defer_line = line_no;
            int defer_depth = depth;

            size_t j = i + 6;
            while (j < in_len && (in_src[j] == ' ' || in_src[j] == '\t')) j++;

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
            defers[defer_depth][defer_counts[defer_depth]++] = (CCDeferStmt){ .line_no = defer_line, .stmt = stmt };

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
            /* Emit defers for this scope before the brace. */
            int d = depth;
            if (d < 0) d = 0;
            if (d >= 256) d = 255;
            if (defer_counts[d] > 0) {
                const char* f = ctx->input_path ? ctx->input_path : "<input>";
                for (int k = defer_counts[d] - 1; k >= 0; k--) {
                    char ln[256];
                    int nn = snprintf(ln, sizeof(ln), "#line %d \"%s\"\n", defers[d][k].line_no, f);
                    if (nn > 0 && (size_t)nn < sizeof(ln)) {
                        cc__append_n(&out, &outl, &outc, ln, (size_t)nn);
                    }
                    cc__append_str(&out, &outl, &outc, defers[d][k].stmt);
                    if (defers[d][k].stmt[0] != 0) {
                        size_t sl = strlen(defers[d][k].stmt);
                        if (sl == 0 || defers[d][k].stmt[sl - 1] != '\n') cc__append_str(&out, &outl, &outc, "\n");
                    }
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

