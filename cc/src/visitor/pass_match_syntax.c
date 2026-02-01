#include "pass_match_syntax.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util/path.h"
#include "util/text.h"
#include "visitor/edit_buffer.h"

int cc__rewrite_match_syntax(const CCVisitorCtx* ctx,
                            const char* src,
                            size_t n,
                            char** out_src,
                            size_t* out_len) {
    if (!out_src || !out_len) return -1;
    *out_src = NULL;
    *out_len = 0;
    if (!src || n == 0) return 0;

    char* out = NULL;
    size_t outl = 0, outc = 0;
    size_t i = 0;
    size_t last_emit = 0;

    int in_line_comment = 0;
    int in_block_comment = 0;
    int in_str = 0;
    int in_chr = 0;
    int line = 1;
    int col = 1;
    unsigned long counter = 0;

    const char* input_path = (ctx && ctx->input_path) ? ctx->input_path : "<input>";

    while (i < n) {
        char c = src[i];
        char c2 = (i + 1 < n) ? src[i + 1] : 0;
        if (c == '\n') { line++; col = 1; }

        if (in_line_comment) { if (c == '\n') in_line_comment = 0; i++; col++; continue; }
        if (in_block_comment) { if (c == '*' && c2 == '/') { in_block_comment = 0; i += 2; col += 2; continue; } i++; col++; continue; }
        if (in_str) { if (c == '\\' && i + 1 < n) { i += 2; col += 2; continue; } if (c == '"') in_str = 0; i++; col++; continue; }
        if (in_chr) { if (c == '\\' && i + 1 < n) { i += 2; col += 2; continue; } if (c == '\'') in_chr = 0; i++; col++; continue; }

        if (c == '/' && c2 == '/') { in_line_comment = 1; i += 2; col += 2; continue; }
        if (c == '/' && c2 == '*') { in_block_comment = 1; i += 2; col += 2; continue; }
        if (c == '"') { in_str = 1; i++; col++; continue; }
        if (c == '\'') { in_chr = 1; i++; col++; continue; }

        if (c == '@') {
            size_t j = i + 1;
            while (j < n && (src[j] == ' ' || src[j] == '\t' || src[j] == '\r' || src[j] == '\n')) j++;
            if (j + 5 <= n && memcmp(src + j, "match", 5) == 0) {
                char after = (j + 5 < n) ? src[j + 5] : 0;
                if (!after || !cc_is_ident_char(after)) {
                    size_t k = j + 5;
                    while (k < n && (src[k] == ' ' || src[k] == '\t' || src[k] == '\r' || src[k] == '\n')) k++;
                    if (k >= n || src[k] != '{') { i++; col++; continue; }

                    size_t body_s = k;
                    int br = 1;
                    int in_s2 = 0, in_lc2 = 0, in_bc2 = 0;
                    char q2 = 0;
                    size_t m = body_s + 1;
                    for (; m < n; m++) {
                        char ch = src[m];
                        char ch2 = (m + 1 < n) ? src[m + 1] : 0;
                        if (in_lc2) { if (ch == '\n') in_lc2 = 0; continue; }
                        if (in_bc2) { if (ch == '*' && ch2 == '/') { in_bc2 = 0; m++; } continue; }
                        if (in_s2) { if (ch == '\\' && m + 1 < n) { m++; continue; } if (ch == q2) in_s2 = 0; continue; }
                        if (ch == '/' && ch2 == '/') { in_lc2 = 1; m++; continue; }
                        if (ch == '/' && ch2 == '*') { in_bc2 = 1; m++; continue; }
                        if (ch == '"' || ch == '\'') { in_s2 = 1; q2 = ch; continue; }
                        if (ch == '{') br++;
                        else if (ch == '}') { br--; if (br == 0) break; }
                    }
                    if (m >= n || br != 0) {
                        char rel[1024];
                        fprintf(stderr, "CC: error: unterminated @match block at %s:%d:%d\n",
                                cc_path_rel_to_repo(input_path, rel, sizeof(rel)), line, col);
                        free(out);
                        return -1;
                    }
                    size_t body_e = m + 1;

                    typedef struct {
                        int kind; /* 0=send, 1=recv, 2=cancel */
                        char ch_expr[160];
                        char arg_expr[240];
                        size_t body_off_s;
                        size_t body_off_e;
                    } MatchCase;
                    MatchCase cases[32];
                    int case_n = 0;
                    int cancel_idx = -1;

                    size_t p = body_s + 1;
                    while (p < body_e) {
                        while (p < body_e && (src[p] == ' ' || src[p] == '\t' || src[p] == '\r' || src[p] == '\n')) p++;
                        if (p + 4 >= body_e) break;
                        if (memcmp(src + p, "case", 4) != 0 || (p > 0 && cc_is_ident_char(src[p - 1])) || (p + 4 < n && cc_is_ident_char(src[p + 4]))) {
                            p++;
                            continue;
                        }
                        p += 4;
                        while (p < body_e && (src[p] == ' ' || src[p] == '\t' || src[p] == '\r' || src[p] == '\n')) p++;

                        size_t hdr_s = p;
                        int par = 0, brk2 = 0, br2 = 0;
                        int ins = 0; char qq = 0;
                        size_t hdr_e = (size_t)-1;
                        for (size_t q = p; q < body_e; q++) {
                            char ch = src[q];
                            char ch2 = (q + 1 < body_e) ? src[q + 1] : 0;
                            if (ins) { if (ch == '\\' && q + 1 < body_e) { q++; continue; } if (ch == qq) ins = 0; continue; }
                            if (ch == '"' || ch == '\'') { ins = 1; qq = ch; continue; }
                            if (ch == '/' && ch2 == '/') { while (q < body_e && src[q] != '\n') q++; continue; }
                            if (ch == '/' && ch2 == '*') { q += 2; while (q + 1 < body_e && !(src[q] == '*' && src[q + 1] == '/')) q++; q++; continue; }
                            if (ch == '(') par++;
                            else if (ch == ')') { if (par) par--; }
                            else if (ch == '[') brk2++;
                            else if (ch == ']') { if (brk2) brk2--; }
                            else if (ch == '{') br2++;
                            else if (ch == '}') { if (br2) br2--; }
                            else if (ch == ':' && par == 0 && brk2 == 0 && br2 == 0) { hdr_e = q; break; }
                        }
                        if (hdr_e == (size_t)-1) break;

                        p = hdr_e + 1;
                        while (p < body_e && (src[p] == ' ' || src[p] == '\t' || src[p] == '\r' || src[p] == '\n')) p++;
                        if (p >= body_e) break;
                        size_t cb_s = p;
                        size_t cb_e = (size_t)-1;
                        if (src[p] == '{') {
                            int brr = 1;
                            int ins2 = 0; char qq2 = 0;
                            int lc = 0, bc = 0;
                            for (size_t q = p + 1; q < body_e; q++) {
                                char ch = src[q];
                                char ch2 = (q + 1 < body_e) ? src[q + 1] : 0;
                                if (lc) { if (ch == '\n') lc = 0; continue; }
                                if (bc) { if (ch == '*' && ch2 == '/') { bc = 0; q++; } continue; }
                                if (ins2) { if (ch == '\\' && q + 1 < body_e) { q++; continue; } if (ch == qq2) ins2 = 0; continue; }
                                if (ch == '/' && ch2 == '/') { lc = 1; q++; continue; }
                                if (ch == '/' && ch2 == '*') { bc = 1; q++; continue; }
                                if (ch == '"' || ch == '\'') { ins2 = 1; qq2 = ch; continue; }
                                if (ch == '{') brr++;
                                else if (ch == '}') { brr--; if (brr == 0) { cb_e = q + 1; break; } }
                            }
                        } else {
                            int par3 = 0, brk3 = 0, br3 = 0;
                            int ins3 = 0; char qq3 = 0;
                            for (size_t q = p; q < body_e; q++) {
                                char ch = src[q];
                                (void)(q + 1 < body_e ? src[q + 1] : 0); /* reserved for comment detection */
                                if (ins3) { if (ch == '\\' && q + 1 < body_e) { q++; continue; } if (ch == qq3) ins3 = 0; continue; }
                                if (ch == '"' || ch == '\'') { ins3 = 1; qq3 = ch; continue; }
                                if (ch == '(') par3++;
                                else if (ch == ')') { if (par3) par3--; }
                                else if (ch == '[') brk3++;
                                else if (ch == ']') { if (brk3) brk3--; }
                                else if (ch == '{') br3++;
                                else if (ch == '}') { if (br3) br3--; }
                                else if (ch == ';' && par3 == 0 && brk3 == 0 && br3 == 0) { cb_e = q + 1; break; }
                            }
                        }
                        if (cb_e == (size_t)-1) break;

                        if (case_n >= (int)(sizeof(cases)/sizeof(cases[0]))) break;
                        memset(&cases[case_n], 0, sizeof(cases[case_n]));
                        cases[case_n].body_off_s = cb_s;
                        cases[case_n].body_off_e = cb_e;

                        while (hdr_s < hdr_e && (src[hdr_s] == ' ' || src[hdr_s] == '\t')) hdr_s++;
                        while (hdr_e > hdr_s && (src[hdr_e - 1] == ' ' || src[hdr_e - 1] == '\t')) hdr_e--;
                        size_t hl = hdr_e > hdr_s ? (hdr_e - hdr_s) : 0;
                        if (hl >= 380) hl = 379;
                        char hdr[380];
                        memcpy(hdr, src + hdr_s, hl);
                        hdr[hl] = 0;

                        if (strncmp(hdr, "is_cancelled()", 14) == 0) {
                            cases[case_n].kind = 2;
                            cancel_idx = case_n;
                        } else {
                            const char* recv = strstr(hdr, ".recv");
                            const char* send = strstr(hdr, ".send");
                            const char* dot = recv ? recv : send;
                            int is_recv = (recv != NULL);
                            int is_send = (send != NULL);
                            if (!dot || (!is_recv && !is_send)) {
                                char rel[1024];
                                fprintf(stderr, "CC: error: @match case header must be <chan>.recv(ptr) or <chan>.send(value) or is_cancelled() at %s:%d:%d\n",
                                        cc_path_rel_to_repo(input_path, rel, sizeof(rel)), line, col);
                                free(out);
                                return -1;
                            }
                            size_t cn = (size_t)(dot - hdr);
                            while (cn > 0 && (hdr[cn - 1] == ' ' || hdr[cn - 1] == '\t')) cn--;
                            if (cn >= sizeof(cases[case_n].ch_expr)) cn = sizeof(cases[case_n].ch_expr) - 1;
                            memcpy(cases[case_n].ch_expr, hdr, cn);
                            cases[case_n].ch_expr[cn] = 0;

                            const char* lp = strchr(dot, '(');
                            const char* rp = lp ? strrchr(dot, ')') : NULL;
                            if (!lp || !rp || rp <= lp) {
                                char rel[1024];
                                fprintf(stderr, "CC: error: malformed @match case header at %s:%d:%d\n",
                                        cc_path_rel_to_repo(input_path, rel, sizeof(rel)), line, col);
                                free(out);
                                return -1;
                            }
                            size_t an = (size_t)(rp - (lp + 1));
                            if (an >= sizeof(cases[case_n].arg_expr)) an = sizeof(cases[case_n].arg_expr) - 1;
                            memcpy(cases[case_n].arg_expr, lp + 1, an);
                            cases[case_n].arg_expr[an] = 0;
                            cases[case_n].kind = is_send ? 0 : 1;
                        }

                        case_n++;
                        p = cb_e;
                    }

                    if (case_n == 0) { i++; col++; continue; }

                    counter++;
                    char pro[4096];
                    size_t pn = 0;
                    pn += (size_t)snprintf(pro + pn, sizeof(pro) - pn,
                                           "do { /* @match */\n"
                                           "  size_t __cc_match_idx_%lu = (size_t)-1;\n"
                                           "  int __cc_match_rc_%lu = 0;\n"
                                           "  CCChanMatchCase __cc_match_cases_%lu[%d];\n",
                                           counter, counter, counter, case_n);

                    for (int ci = 0; ci < case_n; ci++) {
                        if (cases[ci].kind == 0) {
                            pn += (size_t)snprintf(pro + pn, sizeof(pro) - pn,
                                                   "  __typeof__(%s) __cc_match_v_%lu_%d = (%s);\n"
                                                   "  __cc_match_cases_%lu[%d] = (CCChanMatchCase){ .ch = (%s).raw, .send_buf = &__cc_match_v_%lu_%d, .recv_buf = NULL, .elem_size = sizeof(__cc_match_v_%lu_%d), .is_send = true };\n",
                                                   cases[ci].arg_expr, counter, ci, cases[ci].arg_expr,
                                                   counter, ci, cases[ci].ch_expr, counter, ci, counter, ci);
                        } else if (cases[ci].kind == 1) {
                            pn += (size_t)snprintf(pro + pn, sizeof(pro) - pn,
                                                   "  __cc_match_cases_%lu[%d] = (CCChanMatchCase){ .ch = (%s).raw, .send_buf = NULL, .recv_buf = (void*)(%s), .elem_size = sizeof(*(%s)), .is_send = false };\n",
                                                   counter, ci, cases[ci].ch_expr, cases[ci].arg_expr, cases[ci].arg_expr);
                        } else {
                            pn += (size_t)snprintf(pro + pn, sizeof(pro) - pn,
                                                   "  __cc_match_cases_%lu[%d] = (CCChanMatchCase){0};\n",
                                                   counter, ci);
                        }
                    }
                    if (cancel_idx >= 0) {
                        pn += (size_t)snprintf(pro + pn, sizeof(pro) - pn,
                                               "  if (cc_is_cancelled()) {\n"
                                               "    __cc_match_idx_%lu = %d;\n"
                                               "  } else {\n"
                                               "    __cc_match_rc_%lu = cc_chan_match_select(__cc_match_cases_%lu, %d, &__cc_match_idx_%lu, cc_current_deadline());\n"
                                               "  }\n",
                                               counter, cancel_idx,
                                               counter, counter, case_n, counter);
                    } else {
                        pn += (size_t)snprintf(pro + pn, sizeof(pro) - pn,
                                               "  __cc_match_rc_%lu = cc_chan_match_select(__cc_match_cases_%lu, %d, &__cc_match_idx_%lu, cc_current_deadline());\n",
                                               counter, counter, case_n, counter);
                    }
                    (void)pn;

                    cc_sb_append(&out, &outl, &outc, src + last_emit, i - last_emit);
                    cc_sb_append(&out, &outl, &outc, pro, strlen(pro));

                    char sw[256];
                    snprintf(sw, sizeof(sw), "  switch (__cc_match_idx_%lu) {\n", counter);
                    cc_sb_append_cstr(&out, &outl, &outc, sw);
                    for (int ci = 0; ci < case_n; ci++) {
                        char cs[64];
                        snprintf(cs, sizeof(cs), "    case %d:\n", ci);
                        cc_sb_append_cstr(&out, &outl, &outc, cs);
                        cc_sb_append(&out, &outl, &outc, src + cases[ci].body_off_s, cases[ci].body_off_e - cases[ci].body_off_s);
                        cc_sb_append_cstr(&out, &outl, &outc, "\n      break;\n");
                    }
                    cc_sb_append_cstr(&out, &outl, &outc,
                                       "    default: break;\n"
                                       "  }\n"
                                       "  (void)__cc_match_rc_");
                    char suf[64];
                    snprintf(suf, sizeof(suf), "%lu;\n", counter);
                    cc_sb_append_cstr(&out, &outl, &outc, suf);
                    cc_sb_append_cstr(&out, &outl, &outc, "} while(0);\n");

                    last_emit = body_e;
                    i = body_e;
                    continue;
                }
            }
        }

        i++; col++;
    }

    if (last_emit == 0) return 0;
    if (last_emit < n) cc_sb_append(&out, &outl, &outc, src + last_emit, n - last_emit);
    *out_src = out;
    *out_len = outl;
    return 1;
}

/* NEW: Collect @match edits into EditBuffer without applying.
   Returns number of edits added (>= 0), or -1 on error. */
int cc__collect_match_edits(const CCVisitorCtx* ctx, CCEditBuffer* eb) {
    if (!ctx || !eb || !eb->src) return 0;

    const char* src = eb->src;
    size_t n = eb->src_len;
    if (n == 0) return 0;

    int edits_added = 0;
    size_t i = 0;

    int in_line_comment = 0;
    int in_block_comment = 0;
    int in_str = 0;
    int in_chr = 0;
    int line = 1;
    int col = 1;
    unsigned long counter = 0;

    const char* input_path = (ctx && ctx->input_path) ? ctx->input_path : "<input>";

    while (i < n) {
        char c = src[i];
        char c2 = (i + 1 < n) ? src[i + 1] : 0;
        if (c == '\n') { line++; col = 1; }

        if (in_line_comment) { if (c == '\n') in_line_comment = 0; i++; col++; continue; }
        if (in_block_comment) { if (c == '*' && c2 == '/') { in_block_comment = 0; i += 2; col += 2; continue; } i++; col++; continue; }
        if (in_str) { if (c == '\\' && i + 1 < n) { i += 2; col += 2; continue; } if (c == '"') in_str = 0; i++; col++; continue; }
        if (in_chr) { if (c == '\\' && i + 1 < n) { i += 2; col += 2; continue; } if (c == '\'') in_chr = 0; i++; col++; continue; }

        if (c == '/' && c2 == '/') { in_line_comment = 1; i += 2; col += 2; continue; }
        if (c == '/' && c2 == '*') { in_block_comment = 1; i += 2; col += 2; continue; }
        if (c == '"') { in_str = 1; i++; col++; continue; }
        if (c == '\'') { in_chr = 1; i++; col++; continue; }

        if (c == '@') {
            size_t j = i + 1;
            while (j < n && (src[j] == ' ' || src[j] == '\t' || src[j] == '\r' || src[j] == '\n')) j++;
            if (j + 5 <= n && memcmp(src + j, "match", 5) == 0) {
                char after = (j + 5 < n) ? src[j + 5] : 0;
                if (!after || !cc_is_ident_char(after)) {
                    size_t k = j + 5;
                    while (k < n && (src[k] == ' ' || src[k] == '\t' || src[k] == '\r' || src[k] == '\n')) k++;
                    if (k >= n || src[k] != '{') { i++; col++; continue; }

                    size_t body_s = k;
                    int br = 1;
                    int in_s2 = 0, in_lc2 = 0, in_bc2 = 0;
                    char q2 = 0;
                    size_t m = body_s + 1;
                    for (; m < n; m++) {
                        char ch = src[m];
                        char ch2 = (m + 1 < n) ? src[m + 1] : 0;
                        if (in_lc2) { if (ch == '\n') in_lc2 = 0; continue; }
                        if (in_bc2) { if (ch == '*' && ch2 == '/') { in_bc2 = 0; m++; } continue; }
                        if (in_s2) { if (ch == '\\' && m + 1 < n) { m++; continue; } if (ch == q2) in_s2 = 0; continue; }
                        if (ch == '/' && ch2 == '/') { in_lc2 = 1; m++; continue; }
                        if (ch == '/' && ch2 == '*') { in_bc2 = 1; m++; continue; }
                        if (ch == '"' || ch == '\'') { in_s2 = 1; q2 = ch; continue; }
                        if (ch == '{') br++;
                        else if (ch == '}') { br--; if (br == 0) break; }
                    }
                    if (m >= n || br != 0) {
                        char rel[1024];
                        fprintf(stderr, "CC: error: unterminated @match block at %s:%d:%d\n",
                                cc_path_rel_to_repo(input_path, rel, sizeof(rel)), line, col);
                        return -1;
                    }
                    size_t body_e = m + 1;

                    typedef struct {
                        int kind; /* 0=send, 1=recv, 2=cancel */
                        char ch_expr[160];
                        char arg_expr[240];
                        size_t body_off_s;
                        size_t body_off_e;
                    } MatchCase;
                    MatchCase cases[32];
                    int case_n = 0;
                    int cancel_idx = -1;

                    size_t p = body_s + 1;
                    while (p < body_e) {
                        while (p < body_e && (src[p] == ' ' || src[p] == '\t' || src[p] == '\r' || src[p] == '\n')) p++;
                        if (p + 4 >= body_e) break;
                        if (memcmp(src + p, "case", 4) != 0 || (p > 0 && cc_is_ident_char(src[p - 1])) || (p + 4 < n && cc_is_ident_char(src[p + 4]))) {
                            p++;
                            continue;
                        }
                        p += 4;
                        while (p < body_e && (src[p] == ' ' || src[p] == '\t' || src[p] == '\r' || src[p] == '\n')) p++;

                        size_t hdr_s = p;
                        int par = 0, brk2 = 0, br2 = 0;
                        int ins = 0; char qq = 0;
                        size_t hdr_e = (size_t)-1;
                        for (size_t q = p; q < body_e; q++) {
                            char ch = src[q];
                            char ch2 = (q + 1 < body_e) ? src[q + 1] : 0;
                            if (ins) { if (ch == '\\' && q + 1 < body_e) { q++; continue; } if (ch == qq) ins = 0; continue; }
                            if (ch == '"' || ch == '\'') { ins = 1; qq = ch; continue; }
                            if (ch == '/' && ch2 == '/') { while (q < body_e && src[q] != '\n') q++; continue; }
                            if (ch == '/' && ch2 == '*') { q += 2; while (q + 1 < body_e && !(src[q] == '*' && src[q + 1] == '/')) q++; q++; continue; }
                            if (ch == '(') par++;
                            else if (ch == ')') { if (par) par--; }
                            else if (ch == '[') brk2++;
                            else if (ch == ']') { if (brk2) brk2--; }
                            else if (ch == '{') br2++;
                            else if (ch == '}') { if (br2) br2--; }
                            else if (ch == ':' && par == 0 && brk2 == 0 && br2 == 0) { hdr_e = q; break; }
                        }
                        if (hdr_e == (size_t)-1) break;                        p = hdr_e + 1;
                        while (p < body_e && (src[p] == ' ' || src[p] == '\t' || src[p] == '\r' || src[p] == '\n')) p++;
                        if (p >= body_e) break;
                        size_t cb_s = p;
                        size_t cb_e = (size_t)-1;
                        if (src[p] == '{') {
                            int brr = 1;
                            int ins2 = 0; char qq2 = 0;
                            int lc = 0, bc = 0;
                            for (size_t q = p + 1; q < body_e; q++) {
                                char ch = src[q];
                                char ch2 = (q + 1 < body_e) ? src[q + 1] : 0;
                                if (lc) { if (ch == '\n') lc = 0; continue; }
                                if (bc) { if (ch == '*' && ch2 == '/') { bc = 0; q++; } continue; }
                                if (ins2) { if (ch == '\\' && q + 1 < body_e) { q++; continue; } if (ch == qq2) ins2 = 0; continue; }
                                if (ch == '/' && ch2 == '/') { lc = 1; q++; continue; }
                                if (ch == '/' && ch2 == '*') { bc = 1; q++; continue; }
                                if (ch == '"' || ch == '\'') { ins2 = 1; qq2 = ch; continue; }
                                if (ch == '{') brr++;
                                else if (ch == '}') { brr--; if (brr == 0) { cb_e = q + 1; break; } }
                            }
                        } else {
                            int par3 = 0, brk3 = 0, br3 = 0;
                            int ins3 = 0; char qq3 = 0;
                            for (size_t q = p; q < body_e; q++) {
                                char ch = src[q];
                                (void)(q + 1 < body_e ? src[q + 1] : 0);
                                if (ins3) { if (ch == '\\' && q + 1 < body_e) { q++; continue; } if (ch == qq3) ins3 = 0; continue; }
                                if (ch == '"' || ch == '\'') { ins3 = 1; qq3 = ch; continue; }
                                if (ch == '(') par3++;
                                else if (ch == ')') { if (par3) par3--; }
                                else if (ch == '[') brk3++;
                                else if (ch == ']') { if (brk3) brk3--; }
                                else if (ch == '{') br3++;
                                else if (ch == '}') { if (br3) br3--; }
                                else if (ch == ';' && par3 == 0 && brk3 == 0 && br3 == 0) { cb_e = q + 1; break; }
                            }
                        }
                        if (cb_e == (size_t)-1) break;

                        if (case_n >= (int)(sizeof(cases)/sizeof(cases[0]))) break;
                        memset(&cases[case_n], 0, sizeof(cases[case_n]));
                        cases[case_n].body_off_s = cb_s;
                        cases[case_n].body_off_e = cb_e;

                        while (hdr_s < hdr_e && (src[hdr_s] == ' ' || src[hdr_s] == '\t')) hdr_s++;
                        while (hdr_e > hdr_s && (src[hdr_e - 1] == ' ' || src[hdr_e - 1] == '\t')) hdr_e--;
                        size_t hl = hdr_e > hdr_s ? (hdr_e - hdr_s) : 0;
                        if (hl >= 380) hl = 379;
                        char hdr[380];
                        memcpy(hdr, src + hdr_s, hl);
                        hdr[hl] = 0;

                        if (strncmp(hdr, "is_cancelled()", 14) == 0) {
                            cases[case_n].kind = 2;
                            cancel_idx = case_n;
                        } else {
                            const char* recv = strstr(hdr, ".recv");
                            const char* send = strstr(hdr, ".send");
                            const char* dot = recv ? recv : send;
                            int is_recv = (recv != NULL);
                            int is_send = (send != NULL);
                            if (!dot || (!is_recv && !is_send)) {
                                char rel[1024];
                                fprintf(stderr, "CC: error: @match case header must be <chan>.recv(ptr) or <chan>.send(value) or is_cancelled() at %s:%d:%d\n",
                                        cc_path_rel_to_repo(input_path, rel, sizeof(rel)), line, col);
                                return -1;
                            }
                            size_t cn = (size_t)(dot - hdr);
                            while (cn > 0 && (hdr[cn - 1] == ' ' || hdr[cn - 1] == '\t')) cn--;
                            if (cn >= sizeof(cases[case_n].ch_expr)) cn = sizeof(cases[case_n].ch_expr) - 1;
                            memcpy(cases[case_n].ch_expr, hdr, cn);
                            cases[case_n].ch_expr[cn] = 0;

                            const char* lp = strchr(dot, '(');
                            const char* rp = lp ? strrchr(dot, ')') : NULL;
                            if (!lp || !rp || rp <= lp) {
                                char rel[1024];
                                fprintf(stderr, "CC: error: malformed @match case header at %s:%d:%d\n",
                                        cc_path_rel_to_repo(input_path, rel, sizeof(rel)), line, col);
                                return -1;
                            }
                            size_t an = (size_t)(rp - (lp + 1));
                            if (an >= sizeof(cases[case_n].arg_expr)) an = sizeof(cases[case_n].arg_expr) - 1;
                            memcpy(cases[case_n].arg_expr, lp + 1, an);
                            cases[case_n].arg_expr[an] = 0;
                            cases[case_n].kind = is_send ? 0 : 1;
                        }

                        case_n++;
                        p = cb_e;
                    }

                    if (case_n == 0) { i++; col++; continue; }

                    counter++;

                    /* Build replacement string */
                    char* repl = NULL;
                    size_t repl_len = 0, repl_cap = 0;

                    char pro[4096];
                    size_t pn = 0;
                    pn += (size_t)snprintf(pro + pn, sizeof(pro) - pn,
                                           "do { /* @match */\n"
                                           "  size_t __cc_match_idx_%lu = (size_t)-1;\n"
                                           "  int __cc_match_rc_%lu = 0;\n"
                                           "  CCChanMatchCase __cc_match_cases_%lu[%d];\n",
                                           counter, counter, counter, case_n);

                    for (int ci = 0; ci < case_n; ci++) {
                        if (cases[ci].kind == 0) {
                            pn += (size_t)snprintf(pro + pn, sizeof(pro) - pn,
                                                   "  __typeof__(%s) __cc_match_v_%lu_%d = (%s);\n"
                                                   "  __cc_match_cases_%lu[%d] = (CCChanMatchCase){ .ch = (%s).raw, .send_buf = &__cc_match_v_%lu_%d, .recv_buf = NULL, .elem_size = sizeof(__cc_match_v_%lu_%d), .is_send = true };\n",
                                                   cases[ci].arg_expr, counter, ci, cases[ci].arg_expr,
                                                   counter, ci, cases[ci].ch_expr, counter, ci, counter, ci);
                        } else if (cases[ci].kind == 1) {
                            pn += (size_t)snprintf(pro + pn, sizeof(pro) - pn,
                                                   "  __cc_match_cases_%lu[%d] = (CCChanMatchCase){ .ch = (%s).raw, .send_buf = NULL, .recv_buf = (void*)(%s), .elem_size = sizeof(*(%s)), .is_send = false };\n",
                                                   counter, ci, cases[ci].ch_expr, cases[ci].arg_expr, cases[ci].arg_expr);
                        } else {
                            pn += (size_t)snprintf(pro + pn, sizeof(pro) - pn,
                                                   "  __cc_match_cases_%lu[%d] = (CCChanMatchCase){0};\n",
                                                   counter, ci);
                        }
                    }
                    if (cancel_idx >= 0) {
                        pn += (size_t)snprintf(pro + pn, sizeof(pro) - pn,
                                               "  if (cc_is_cancelled()) {\n"
                                               "    __cc_match_idx_%lu = %d;\n"
                                               "  } else {\n"
                                               "    __cc_match_rc_%lu = cc_chan_match_select(__cc_match_cases_%lu, %d, &__cc_match_idx_%lu, cc_current_deadline());\n"
                                               "  }\n",
                                               counter, cancel_idx,
                                               counter, counter, case_n, counter);
                    } else {
                        pn += (size_t)snprintf(pro + pn, sizeof(pro) - pn,
                                               "  __cc_match_rc_%lu = cc_chan_match_select(__cc_match_cases_%lu, %d, &__cc_match_idx_%lu, cc_current_deadline());\n",
                                               counter, counter, case_n, counter);
                    }

                    cc_sb_append(&repl, &repl_len, &repl_cap, pro, pn);

                    char sw[256];
                    snprintf(sw, sizeof(sw), "  switch (__cc_match_idx_%lu) {\n", counter);
                    cc_sb_append_cstr(&repl, &repl_len, &repl_cap, sw);
                    for (int ci = 0; ci < case_n; ci++) {
                        char cs[64];
                        snprintf(cs, sizeof(cs), "    case %d:\n", ci);
                        cc_sb_append_cstr(&repl, &repl_len, &repl_cap, cs);
                        cc_sb_append(&repl, &repl_len, &repl_cap, src + cases[ci].body_off_s, cases[ci].body_off_e - cases[ci].body_off_s);
                        cc_sb_append_cstr(&repl, &repl_len, &repl_cap, "\n      break;\n");
                    }
                    cc_sb_append_cstr(&repl, &repl_len, &repl_cap,
                                       "    default: break;\n"
                                       "  }\n"
                                       "  (void)__cc_match_rc_");
                    char suf[64];
                    snprintf(suf, sizeof(suf), "%lu;\n", counter);
                    cc_sb_append_cstr(&repl, &repl_len, &repl_cap, suf);
                    cc_sb_append_cstr(&repl, &repl_len, &repl_cap, "} while(0);\n");

                    /* Add the edit */
                    if (cc_edit_buffer_add(eb, i, body_e, repl, 50, "match") == 0) {
                        edits_added++;
                    }
                    free(repl);

                    i = body_e;
                    continue;
                }
            }
        }

        i++; col++;
    }

    return edits_added;
}
