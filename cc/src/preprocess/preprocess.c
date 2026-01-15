#include "preprocess.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "util/path.h"
#include "util/text.h"

/* Rewrite `@match { case <hdr>: <body> ... }` into valid C using cc_chan_match_select.
   This is intentionally text-based: the construct is not valid C, so TCC must see rewritten code.

   Supported (initial subset):
     - `case <rx>.recv(<out_ptr>): { ... }`
     - `case <tx>.send(<value_expr>): { ... }`
     - `case is_cancelled(): { ... }`  (routes immediately if cancelled)

   Notes:
     - In @async code, the emitted `cc_chan_match_select(...)` call will be auto-wrapped by the
       autoblock pass into an `await cc_run_blocking_task_intptr(...)`, making it cooperative. */
static char* cc__rewrite_match_syntax(const char* src, size_t n, const char* input_path) {
    if (!src || n == 0) return NULL;

    char* out = NULL;
    size_t out_len = 0, out_cap = 0;
    size_t i = 0;
    size_t last_emit = 0;

    int in_line_comment = 0;
    int in_block_comment = 0;
    int in_str = 0;
    int in_chr = 0;
    int line = 1;
    int col = 1;
    unsigned long counter = 0;

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

        /* Look for "@match" */
        if (c == '@') {
            size_t j = i + 1;
            while (j < n && (src[j] == ' ' || src[j] == '\t' || src[j] == '\r' || src[j] == '\n')) j++;
            const char* kw = "match";
            if (j + 5 <= n && memcmp(src + j, kw, 5) == 0) {
                char after = (j + 5 < n) ? src[j + 5] : 0;
                if (!after || !cc_is_ident_char(after)) {
                    size_t k = j + 5;
                    while (k < n && (src[k] == ' ' || src[k] == '\t' || src[k] == '\r' || src[k] == '\n')) k++;
                    if (k >= n || src[k] != '{') { i++; col++; continue; }

                    /* Find matching '}' for @match block. */
                    size_t body_s = k; /* points at '{' */
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
                                cc_path_rel_to_repo(input_path ? input_path : "<input>", rel, sizeof(rel)), line, col);
                        free(out);
                        return NULL;
                    }
                    size_t body_e = m + 1; /* after '}' */

                    /* Parse cases inside body_s..body_e. */
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
                        /* skip whitespace */
                        while (p < body_e && (src[p] == ' ' || src[p] == '\t' || src[p] == '\r' || src[p] == '\n')) p++;
                        if (p + 4 >= body_e) break;
                        if (memcmp(src + p, "case", 4) != 0 || (p > 0 && cc_is_ident_char(src[p - 1])) || (p + 4 < n && cc_is_ident_char(src[p + 4]))) {
                            p++;
                            continue;
                        }
                        p += 4;
                        while (p < body_e && (src[p] == ' ' || src[p] == '\t' || src[p] == '\r' || src[p] == '\n')) p++;

                        /* header: until ':' at depth 0 */
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

                        /* body: after ':' */
                        p = hdr_e + 1;
                        while (p < body_e && (src[p] == ' ' || src[p] == '\t' || src[p] == '\r' || src[p] == '\n')) p++;
                        if (p >= body_e) break;
                        size_t cb_s = p;
                        size_t cb_e = (size_t)-1;
                        if (src[p] == '{') {
                            /* balanced block */
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
                            /* single statement until ';' */
                            int par3 = 0, brk3 = 0, br3 = 0;
                            int ins3 = 0; char qq3 = 0;
                            for (size_t q = p; q < body_e; q++) {
                                char ch = src[q];
                                char ch2 = (q + 1 < body_e) ? src[q + 1] : 0;
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

                        /* interpret header */
                        {
                            /* trim hdr */
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
                                /* find ".recv(" or ".send(" */
                                const char* recv = strstr(hdr, ".recv");
                                const char* send = strstr(hdr, ".send");
                                const char* dot = recv ? recv : send;
                                int is_recv = (recv != NULL);
                                int is_send = (send != NULL);
                                if (!dot || (!is_recv && !is_send)) {
                                    char rel[1024];
                                    fprintf(stderr, "CC: error: @match case header must be <chan>.recv(ptr) or <chan>.send(value) or is_cancelled() at %s:%d:%d\n",
                                            cc_path_rel_to_repo(input_path ? input_path : "<input>", rel, sizeof(rel)), line, col);
                                    free(out);
                                    return NULL;
                                }
                                /* channel expr */
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
                                            cc_path_rel_to_repo(input_path ? input_path : "<input>", rel, sizeof(rel)), line, col);
                                    free(out);
                                    return NULL;
                                }
                                size_t an = (size_t)(rp - (lp + 1));
                                while (an > 0 && ((lp + 1)[0] == ' ' || (lp + 1)[0] == '\t')) { lp++; an--; }
                                while (an > 0 && ((lp + 1)[an - 1] == ' ' || (lp + 1)[an - 1] == '\t')) an--;
                                if (an >= sizeof(cases[case_n].arg_expr)) an = sizeof(cases[case_n].arg_expr) - 1;
                                memcpy(cases[case_n].arg_expr, lp + 1, an);
                                cases[case_n].arg_expr[an] = 0;

                                cases[case_n].kind = is_send ? 0 : 1;
                            }
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
                                           "  int __cc_match_rc_%lu = 0;\n",
                                           counter, counter, counter);

                    /* Prepare send temps and build cases array */
                    pn += (size_t)snprintf(pro + pn, sizeof(pro) - pn,
                                           "  CCChanMatchCase __cc_match_cases_%lu[%d];\n",
                                           counter, case_n);
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
                            /* cancelled() doesn't touch channels; leave entry unused */
                            pn += (size_t)snprintf(pro + pn, sizeof(pro) - pn,
                                                   "  __cc_match_cases_%lu[%d] = (CCChanMatchCase){0};\n",
                                                   counter, ci);
                        }
                    }

                    /* Select */
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

                    (void)pn; /* best-effort; if truncated, compilation will fail and point here */

                    /* Emit prefix up to @match, then prologue */
                    cc_sb_append(&out, &out_len, &out_cap, src + last_emit, i - last_emit);
                    cc_sb_append(&out, &out_len, &out_cap, pro, strlen(pro));

                    /* switch */
                    char sw[256];
                    snprintf(sw, sizeof(sw),
                             "  switch (__cc_match_idx_%lu) {\n",
                             counter);
                    cc_sb_append_cstr(&out, &out_len, &out_cap, sw);
                    for (int ci = 0; ci < case_n; ci++) {
                        char cs[64];
                        snprintf(cs, sizeof(cs), "    case %d:\n", ci);
                        cc_sb_append_cstr(&out, &out_len, &out_cap, cs);
                        cc_sb_append(&out, &out_len, &out_cap, src + cases[ci].body_off_s, cases[ci].body_off_e - cases[ci].body_off_s);
                        cc_sb_append_cstr(&out, &out_len, &out_cap, "\n      break;\n");
                    }
                    cc_sb_append_cstr(&out, &out_len, &out_cap,
                                       "    default: break;\n"
                                       "  }\n"
                                       "  (void)__cc_match_rc_");
                    char suf[64];
                    snprintf(suf, sizeof(suf), "%lu;\n", counter);
                    cc_sb_append_cstr(&out, &out_len, &out_cap, suf);
                    cc_sb_append_cstr(&out, &out_len, &out_cap, "} while(0);\n");

                    last_emit = body_e;
                    i = body_e;
                    continue;
                }
            }
        }

        i++; col++;
    }

    if (last_emit == 0) return NULL;
    if (last_emit < n) cc_sb_append(&out, &out_len, &out_cap, src + last_emit, n - last_emit);
    return out;
}

/* Rewrite `with_deadline(expr) { ... }` into:
     { CCDeadline __cc_dlN = cc_deadline_after_ms((uint64_t)(expr));
       CCDeadline* __cc_prevN = cc_deadline_push(&__cc_dlN);
       @defer cc_deadline_pop(__cc_prevN);
       { ... } }

   This is intentionally text-based: the construct is not valid C, so TCC must see rewritten code. */
static char* cc__rewrite_with_deadline_syntax(const char* src, size_t n) {
    if (!src || n == 0) return NULL;
    char* out = NULL;
    size_t out_len = 0, out_cap = 0;
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
            cc_sb_append(&out, &out_len, &out_cap, &c, 1);
            if (c == '\n') in_line_comment = 0;
            i++;
            continue;
        }
        if (in_block_comment) {
            cc_sb_append(&out, &out_len, &out_cap, &c, 1);
            if (c == '*' && c2 == '/') {
                cc_sb_append(&out, &out_len, &out_cap, &c2, 1);
                i += 2;
                in_block_comment = 0;
                continue;
            }
            i++;
            continue;
        }
        if (in_str) {
            cc_sb_append(&out, &out_len, &out_cap, &c, 1);
            if (c == '\\' && i + 1 < n) {
                cc_sb_append(&out, &out_len, &out_cap, &c2, 1);
                i += 2;
                continue;
            }
            if (c == '"') in_str = 0;
            i++;
            continue;
        }
        if (in_chr) {
            cc_sb_append(&out, &out_len, &out_cap, &c, 1);
            if (c == '\\' && i + 1 < n) {
                cc_sb_append(&out, &out_len, &out_cap, &c2, 1);
                i += 2;
                continue;
            }
            if (c == '\'') in_chr = 0;
            i++;
            continue;
        }

        if (c == '/' && c2 == '/') {
            cc_sb_append(&out, &out_len, &out_cap, &c, 1);
            cc_sb_append(&out, &out_len, &out_cap, &c2, 1);
            i += 2;
            in_line_comment = 1;
            continue;
        }
        if (c == '/' && c2 == '*') {
            cc_sb_append(&out, &out_len, &out_cap, &c, 1);
            cc_sb_append(&out, &out_len, &out_cap, &c2, 1);
            i += 2;
            in_block_comment = 1;
            continue;
        }
        if (c == '"') {
            cc_sb_append(&out, &out_len, &out_cap, &c, 1);
            i++;
            in_str = 1;
            continue;
        }
        if (c == '\'') {
            cc_sb_append(&out, &out_len, &out_cap, &c, 1);
            i++;
            in_chr = 1;
            continue;
        }

        /* Allow `@with_deadline(...) { ... }` as an alias for `with_deadline(...) { ... }`.
           This must happen in preprocessing because `@...` is not valid C and would be rejected
           by the patched TCC parser before later visitor rewrites run. */
        if (c == '@') {
            size_t j = i + 1;
            while (j < n && (src[j] == ' ' || src[j] == '\t' || src[j] == '\r' || src[j] == '\n')) j++;
            const char* kw = "with_deadline";
            size_t kw_len = strlen(kw);
            if (j + kw_len <= n && memcmp(src + j, kw, kw_len) == 0) {
                char after = (j + kw_len < n) ? src[j + kw_len] : 0;
                if (!after || !cc_is_ident_char(after)) {
                    /* Drop the '@' and continue scanning at the keyword. */
                    i = j;
                    continue;
                }
            }
            /* Not our alias; keep the '@' verbatim. */
            cc_sb_append(&out, &out_len, &out_cap, &c, 1);
            i++;
            continue;
        }

        if (cc_is_ident_start(c)) {
            size_t s0 = i;
            i++;
            while (i < n && cc_is_ident_char(src[i])) i++;
            size_t sl = i - s0;
            int is_wd = (sl == strlen("with_deadline") && memcmp(src + s0, "with_deadline", sl) == 0);
            if (!is_wd) {
                cc_sb_append(&out, &out_len, &out_cap, src + s0, sl);
                continue;
            }
            /* Ensure token boundary before. */
            if (s0 > 0 && cc_is_ident_char(src[s0 - 1])) {
                cc_sb_append(&out, &out_len, &out_cap, src + s0, sl);
                continue;
            }

            /* Skip whitespace then expect '(' */
            size_t j = i;
            while (j < n && (src[j] == ' ' || src[j] == '\t' || src[j] == '\r' || src[j] == '\n')) j++;
            if (j >= n || src[j] != '(') {
                /* Just an identifier occurrence. */
                cc_sb_append(&out, &out_len, &out_cap, src + s0, sl);
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
                else if (ch == ')') {
                    par--;
                    if (par == 0) break;
                }
            }
            if (k >= n || par != 0) {
                /* Give up; emit original token. */
                cc_sb_append(&out, &out_len, &out_cap, src + s0, sl);
                i = j;
                continue;
            }
            size_t expr_r = k; /* points at ')' */
            size_t after_paren = expr_r + 1;
            while (after_paren < n && (src[after_paren] == ' ' || src[after_paren] == '\t' || src[after_paren] == '\r' || src[after_paren] == '\n')) after_paren++;
            if (after_paren >= n || src[after_paren] != '{') {
                /* Not a block form; emit original token sequence. */
                cc_sb_append(&out, &out_len, &out_cap, src + s0, sl);
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
                else if (ch == '}') {
                    br--;
                    if (br == 0) { m++; break; }
                }
            }
            if (m > n || br != 0) {
                cc_sb_append(&out, &out_len, &out_cap, src + s0, sl);
                i = j;
                continue;
            }
            size_t body_e = m; /* points just after '}' */

            counter++;
            char hdr[512];
            snprintf(hdr, sizeof(hdr),
                     "{ CCDeadline __cc_dl%lu = cc_deadline_after_ms((uint64_t)(%.*s)); "
                     "CCDeadline* __cc_prev%lu = cc_deadline_push(&__cc_dl%lu); "
                     "@defer cc_deadline_pop(__cc_prev%lu); ",
                     counter,
                     (int)(expr_r - expr_l), src + expr_l,
                     counter, counter, counter);
            cc_sb_append_cstr(&out, &out_len, &out_cap, hdr);
            cc_sb_append(&out, &out_len, &out_cap, src + body_s, body_e - body_s);
            cc_sb_append_cstr(&out, &out_len, &out_cap, " }");

            i = body_e;
            continue;
        }

        /* default: copy */
        cc_sb_append(&out, &out_len, &out_cap, &c, 1);
        i++;
    }

    return out;
}

static size_t cc__scan_back_to_delim(const char* s, size_t from) {
    size_t i = from;
    while (i > 0) {
        char p = s[i - 1];
        if (p == ';' || p == '{' || p == '}' || p == ',' || p == '(' || p == ')' || p == '\n') break;
        i--;
    }
    while (s[i] && (s[i] == ' ' || s[i] == '\t')) i++;
    return i;
}

/* Rewrite channel handle types (surface syntax) into runtime handle structs.

   - `T[~ ... >] name` -> `CCChanTx name`
   - `T[~ ... <] name` -> `CCChanRx name`

   Requires explicit direction ('>' or '<'). Hard errors otherwise.
   Text-based: not valid C, so TCC must see rewritten code. */
static char* cc__rewrite_chan_handle_types(const char* src, size_t n, const char* input_path) {
    if (!src || n == 0) return NULL;
    char* out = NULL;
    size_t out_len = 0, out_cap = 0;

    size_t i = 0;
    size_t last_emit = 0;
    int in_line_comment = 0;
    int in_block_comment = 0;
    int in_str = 0;
    int in_chr = 0;
    int line = 1;
    int col = 1;

    while (i < n) {
        char c = src[i];
        char c2 = (i + 1 < n) ? src[i + 1] : 0;

        if (c == '\n') { line++; col = 1; }

        if (in_line_comment) {
            if (c == '\n') in_line_comment = 0;
            i++; col++;
            continue;
        }
        if (in_block_comment) {
            if (c == '*' && c2 == '/') { in_block_comment = 0; i += 2; col += 2; continue; }
            i++; col++;
            continue;
        }
        if (in_str) {
            if (c == '\\' && i + 1 < n) { i += 2; col += 2; continue; }
            if (c == '"') in_str = 0;
            i++; col++;
            continue;
        }
        if (in_chr) {
            if (c == '\\' && i + 1 < n) { i += 2; col += 2; continue; }
            if (c == '\'') in_chr = 0;
            i++; col++;
            continue;
        }

        if (c == '/' && c2 == '/') { in_line_comment = 1; i += 2; col += 2; continue; }
        if (c == '/' && c2 == '*') { in_block_comment = 1; i += 2; col += 2; continue; }
        if (c == '"') { in_str = 1; i++; col++; continue; }
        if (c == '\'') { in_chr = 1; i++; col++; continue; }

        if (c == '[') {
            size_t j = i + 1;
            while (j < n && (src[j] == ' ' || src[j] == '\t')) j++;
            if (j < n && src[j] == '~') {
                /* Find ']' (same line, best-effort) */
                size_t k = j + 1;
                while (k < n && src[k] != ']' && src[k] != '\n') k++;
                if (k >= n || src[k] != ']') {
                    char rel[1024];
                    fprintf(stderr, "CC: error: unterminated channel handle type (missing ']') at %s:%d:%d\n",
                            cc_path_rel_to_repo(input_path ? input_path : "<input>", rel, sizeof(rel)), line, col);
                    free(out);
                    return NULL;
                }

                int saw_gt = 0, saw_lt = 0;
                for (size_t t = j; t < k; t++) {
                    if (src[t] == '>') saw_gt = 1;
                    if (src[t] == '<') saw_lt = 1;
                }
                if (saw_gt && saw_lt) {
                    char rel[1024];
                    fprintf(stderr, "CC: error: channel handle type cannot be both send ('>') and recv ('<') at %s:%d:%d\n",
                            cc_path_rel_to_repo(input_path ? input_path : "<input>", rel, sizeof(rel)), line, col);
                    free(out);
                    return NULL;
                }
                if (!saw_gt && !saw_lt) {
                    char rel[1024];
                    fprintf(stderr, "CC: error: channel handle type requires direction: use `T[~ ... >]` or `T[~ ... <]` at %s:%d:%d\n",
                            cc_path_rel_to_repo(input_path ? input_path : "<input>", rel, sizeof(rel)), line, col);
                    free(out);
                    return NULL;
                }

                size_t ty_start = cc__scan_back_to_delim(src, i);
                if (ty_start < last_emit) {
                    /* overlapping/odd context; just ignore and continue */
                } else {
                    cc_sb_append(&out, &out_len, &out_cap, src + last_emit, ty_start - last_emit);
                    cc_sb_append_cstr(&out, &out_len, &out_cap, saw_gt ? "CCChanTx" : "CCChanRx");
                    last_emit = k + 1; /* skip past ']' */
                }

                /* advance scan to k+1 */
                while (i < k + 1) {
                    if (src[i] == '\n') { line++; col = 1; }
                    else col++;
                    i++;
                }
                continue;
            }
        }

        i++; col++;
    }

    if (last_emit < n) {
        cc_sb_append(&out, &out_len, &out_cap, src + last_emit, n - last_emit);
    }
    return out;
}

static int cc_is_ident_char_local(char c) {
    return (c == '_' || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'));
}

static size_t cc__strip_leading_cv_qual(const char* s, size_t ty_start, char* out_qual, size_t out_cap) {
    /* Returns the new start offset after consuming leading qualifiers; writes qualifiers (with trailing space) into out_qual. */
    if (!s || !out_qual || out_cap == 0) return ty_start;
    out_qual[0] = 0;
    size_t p = ty_start;
    while (s[p] == ' ' || s[p] == '\t') p++;
    for (;;) {
        int matched = 0;
        if (strncmp(s + p, "const", 5) == 0 && !cc_is_ident_char_local(s[p + 5])) {
            strncat(out_qual, "const ", out_cap - strlen(out_qual) - 1);
            p += 5;
            while (s[p] == ' ' || s[p] == '\t') p++;
            matched = 1;
        } else if (strncmp(s + p, "volatile", 8) == 0 && !cc_is_ident_char_local(s[p + 8])) {
            strncat(out_qual, "volatile ", out_cap - strlen(out_qual) - 1);
            p += 8;
            while (s[p] == ' ' || s[p] == '\t') p++;
            matched = 1;
        }
        if (!matched) break;
    }
    return p;
}

/* Mangle a type name for use in CCOptional_T or CCResult_T_E.
   - Strips leading/trailing whitespace
   - Replaces spaces with underscores
   - Replaces '*' with 'ptr'
   - Replaces '[' and ']' with '_' */
static void cc__mangle_type_name(const char* src, size_t len, char* out, size_t out_sz) {
    if (!src || len == 0 || !out || out_sz == 0) { if (out && out_sz > 0) out[0] = 0; return; }
    
    /* Skip leading whitespace */
    while (len > 0 && (*src == ' ' || *src == '\t')) { src++; len--; }
    /* Skip trailing whitespace */
    while (len > 0 && (src[len - 1] == ' ' || src[len - 1] == '\t')) len--;
    
    size_t j = 0;
    for (size_t i = 0; i < len && j < out_sz - 1; i++) {
        char c = src[i];
        if (c == ' ' || c == '\t') {
            if (j > 0 && out[j - 1] != '_') out[j++] = '_';
        } else if (c == '*') {
            if (j + 3 < out_sz - 1) { out[j++] = 'p'; out[j++] = 't'; out[j++] = 'r'; }
        } else if (c == '[' || c == ']') {
            if (j > 0 && out[j - 1] != '_') out[j++] = '_';
        } else if (c == '<' || c == '>' || c == ',') {
            if (j > 0 && out[j - 1] != '_') out[j++] = '_';
        } else {
            out[j++] = c;
        }
    }
    /* Remove trailing underscore */
    while (j > 0 && out[j - 1] == '_') j--;
    out[j] = 0;
}

/* Rewrite optional types:
   - `T?` -> `CCOptional_T`
   The '?' must immediately follow a type name (no space).
   We detect: identifier? or )? or ]? or >? patterns. */
static char* cc__rewrite_optional_types(const char* src, size_t n, const char* input_path) {
    if (!src || n == 0) return NULL;
    char* out = NULL;
    size_t out_len = 0, out_cap = 0;
    
    size_t i = 0;
    size_t last_emit = 0;
    int in_line_comment = 0;
    int in_block_comment = 0;
    int in_str = 0;
    int in_chr = 0;
    int line = 1;
    int col = 1;
    
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
        
        /* Detect T? pattern: identifier followed by '?' (not '?:' ternary) */
        if (c == '?' && c2 != ':' && c2 != '?') {
            /* Check what precedes the '?' */
            if (i > 0) {
                char prev = src[i - 1];
                /* Valid type-ending chars: identifier char, ')', ']', '>' */
                if (cc_is_ident_char(prev) || prev == ')' || prev == ']' || prev == '>') {
                    /* Scan back to find the type start */
                    size_t ty_start = cc__scan_back_to_delim(src, i);
                    if (ty_start < i) {
                        /* Extract the type name */
                        size_t ty_len = i - ty_start;
                        char mangled[256];
                        cc__mangle_type_name(src + ty_start, ty_len, mangled, sizeof(mangled));
                        
                        if (mangled[0]) {
                            /* Emit everything up to ty_start */
                            cc_sb_append(&out, &out_len, &out_cap, src + last_emit, ty_start - last_emit);
                            /* Emit CCOptional_T */
                            cc_sb_append_cstr(&out, &out_len, &out_cap, "CCOptional_");
                            cc_sb_append_cstr(&out, &out_len, &out_cap, mangled);
                            last_emit = i + 1; /* skip past '?' */
                        }
                    }
                }
            }
        }
        
        i++; col++;
    }
    
    if (last_emit < n) cc_sb_append(&out, &out_len, &out_cap, src + last_emit, n - last_emit);
    return out;
}

/* Collect result types (T, E) pairs and generate declarations. */
typedef struct {
    char ok_type[128];
    char err_type[128];
    char mangled_ok[128];
    char mangled_err[128];
} CCResultTypePair;

static CCResultTypePair cc__result_types[64];
static size_t cc__result_type_count = 0;

static void cc__add_result_type(const char* ok, size_t ok_len, const char* err, size_t err_len,
                                const char* mangled_ok, const char* mangled_err) {
    /* Check for duplicates */
    for (size_t i = 0; i < cc__result_type_count; i++) {
        if (strcmp(cc__result_types[i].mangled_ok, mangled_ok) == 0 &&
            strcmp(cc__result_types[i].mangled_err, mangled_err) == 0) {
            return; /* Already have this type */
        }
    }
    if (cc__result_type_count >= sizeof(cc__result_types)/sizeof(cc__result_types[0])) return;
    CCResultTypePair* p = &cc__result_types[cc__result_type_count++];
    if (ok_len >= sizeof(p->ok_type)) ok_len = sizeof(p->ok_type) - 1;
    if (err_len >= sizeof(p->err_type)) err_len = sizeof(p->err_type) - 1;
    memcpy(p->ok_type, ok, ok_len);
    p->ok_type[ok_len] = 0;
    memcpy(p->err_type, err, err_len);
    p->err_type[err_len] = 0;
    snprintf(p->mangled_ok, sizeof(p->mangled_ok), "%s", mangled_ok);
    snprintf(p->mangled_err, sizeof(p->mangled_err), "%s", mangled_err);
}

/* Rewrite result types:
   - `T!E` -> `CCResult_T_E`
   The '!' must immediately follow a type name (no space).
   We detect: identifier!identifier patterns.
   Also collects unique (T, E) pairs for later emission of CC_DECL_RESULT_SPEC calls. */
static char* cc__rewrite_result_types(const char* src, size_t n, const char* input_path) {
    if (!src || n == 0) return NULL;
    char* out = NULL;
    size_t out_len = 0, out_cap = 0;
    
    /* Reset result type collection */
    cc__result_type_count = 0;
    
    size_t i = 0;
    size_t last_emit = 0;
    int in_line_comment = 0;
    int in_block_comment = 0;
    int in_str = 0;
    int in_chr = 0;
    int line = 1;
    int col = 1;
    
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
        
        /* Detect T!E pattern: type followed by '!' followed by error type */
        if (c == '!' && c2 != '=') {  /* != is not result type */
            /* Check what precedes the '!' */
            if (i > 0) {
                char prev = src[i - 1];
                /* Valid type-ending chars: identifier char, ')', ']', '>' */
                if (cc_is_ident_char(prev) || prev == ')' || prev == ']' || prev == '>') {
                    /* Check what follows the '!' - must be an identifier (error type) */
                    size_t j = i + 1;
                    while (j < n && (src[j] == ' ' || src[j] == '\t')) j++;
                    if (j < n && cc_is_ident_start(src[j])) {
                        /* Found error type start */
                        size_t err_start = j;
                        while (j < n && cc_is_ident_char(src[j])) j++;
                        size_t err_end = j;
                        
                        /* Scan back to find the ok type start */
                        size_t ty_start = cc__scan_back_to_delim(src, i);
                        if (ty_start < i) {
                            size_t ty_len = i - ty_start;
                            size_t err_len = err_end - err_start;
                            
                            char mangled_ok[256];
                            char mangled_err[256];
                            cc__mangle_type_name(src + ty_start, ty_len, mangled_ok, sizeof(mangled_ok));
                            cc__mangle_type_name(src + err_start, err_len, mangled_err, sizeof(mangled_err));
                            
                            if (mangled_ok[0] && mangled_err[0]) {
                                /* Collect this type pair for later declaration emission */
                                cc__add_result_type(src + ty_start, ty_len, src + err_start, err_len,
                                                    mangled_ok, mangled_err);
                                
                                /* Emit everything up to ty_start */
                                cc_sb_append(&out, &out_len, &out_cap, src + last_emit, ty_start - last_emit);
                                /* Emit CCResult_T_E */
                                cc_sb_append_cstr(&out, &out_len, &out_cap, "CCResult_");
                                cc_sb_append_cstr(&out, &out_len, &out_cap, mangled_ok);
                                cc_sb_append_cstr(&out, &out_len, &out_cap, "_");
                                cc_sb_append_cstr(&out, &out_len, &out_cap, mangled_err);
                                last_emit = err_end;
                                i = err_end;
                                continue;
                            }
                        }
                    }
                }
            }
        }
        
        i++; col++;
    }
    
    if (last_emit < n) cc_sb_append(&out, &out_len, &out_cap, src + last_emit, n - last_emit);
    
    /* Result type declarations are NOT emitted by the preprocessor.
       Instead, they are handled by the visitor/codegen pass which runs after TCC parsing.
       TCC parses using CC_PARSER_MODE which provides placeholder types.
       The visitor then emits proper CC_DECL_RESULT_SPEC macros in the generated C.
       
       This avoids forward declaration ordering issues where the error type may not
       be defined yet at the point where we'd need to emit the CC_DECL_RESULT_SPEC. */
    (void)cc__result_type_count; /* Result types collected but not emitted here */
    
    (void)input_path;
    return out;
}

/* Rewrite slice types:
   - `T[:]`  -> `CCSlice`
   - `T[:!]` -> `CCSliceUnique`
   Requires a closing ']' and a ':' after '['. */
static char* cc__rewrite_slice_types(const char* src, size_t n, const char* input_path) {
    if (!src || n == 0) return NULL;
    char* out = NULL;
    size_t out_len = 0, out_cap = 0;

    size_t i = 0;
    size_t last_emit = 0;
    int in_line_comment = 0;
    int in_block_comment = 0;
    int in_str = 0;
    int in_chr = 0;
    int line = 1;
    int col = 1;

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

        if (c == '[') {
            size_t j = i + 1;
            while (j < n && (src[j] == ' ' || src[j] == '\t')) j++;
            if (j < n && src[j] == ':') {
                size_t k = j + 1;
                while (k < n && (src[k] == ' ' || src[k] == '\t')) k++;
                int is_unique = 0;
                if (k < n && src[k] == '!') { is_unique = 1; k++; }
                while (k < n && (src[k] == ' ' || src[k] == '\t')) k++;
                if (k >= n || src[k] != ']') {
                    char rel[1024];
                    fprintf(stderr, "CC: error: unterminated slice type (missing ']') at %s:%d:%d\n",
                            cc_path_rel_to_repo(input_path ? input_path : "<input>", rel, sizeof(rel)), line, col);
                    free(out);
                    return NULL;
                }

                size_t ty_start = cc__scan_back_to_delim(src, i);
                if (ty_start < last_emit) { /* odd overlap */ }
                else {
                    char quals[64];
                    size_t qual_start = ty_start;
                    size_t after_qual = cc__strip_leading_cv_qual(src, qual_start, quals, sizeof(quals));
                    /* Emit everything up to qual_start, keep qualifiers, then emit CCSlice-ish type. */
                    cc_sb_append(&out, &out_len, &out_cap, src + last_emit, qual_start - last_emit);
                    cc_sb_append_cstr(&out, &out_len, &out_cap, quals);
                    cc_sb_append_cstr(&out, &out_len, &out_cap, is_unique ? "CCSliceUnique" : "CCSlice");
                    (void)after_qual; /* intentionally drop element type tokens */
                    last_emit = k + 1; /* skip past ']' */
                }

                /* advance scan to after ']' */
                size_t end = k + 1;
                while (i < end) { if (src[i] == '\n') { line++; col = 1; } else col++; i++; }
                continue;
            }
        }

        i++; col++;
    }

    if (last_emit < n) cc_sb_append(&out, &out_len, &out_cap, src + last_emit, n - last_emit);
    return out;
}

/* Rewrite try expressions: try expr -> cc_try(expr) */
static char* cc__rewrite_try_exprs(const char* src, size_t n) {
    if (!src || n == 0) return NULL;
    char* out = NULL;
    size_t out_len = 0, out_cap = 0;
    
    size_t i = 0;
    size_t last_emit = 0;
    int in_line_comment = 0, in_block_comment = 0, in_str = 0, in_chr = 0;
    
    while (i < n) {
        char c = src[i];
        char c2 = (i + 1 < n) ? src[i + 1] : 0;
        
        if (in_line_comment) { if (c == '\n') in_line_comment = 0; i++; continue; }
        if (in_block_comment) { if (c == '*' && c2 == '/') { in_block_comment = 0; i += 2; continue; } i++; continue; }
        if (in_str) { if (c == '\\' && i + 1 < n) { i += 2; continue; } if (c == '"') in_str = 0; i++; continue; }
        if (in_chr) { if (c == '\\' && i + 1 < n) { i += 2; continue; } if (c == '\'') in_chr = 0; i++; continue; }
        
        if (c == '/' && c2 == '/') { in_line_comment = 1; i += 2; continue; }
        if (c == '/' && c2 == '*') { in_block_comment = 1; i += 2; continue; }
        if (c == '"') { in_str = 1; i++; continue; }
        if (c == '\'') { in_chr = 1; i++; continue; }
        
        /* Detect 'try' keyword followed by expression (not try { block } form) */
        if (c == 't' && i + 2 < n && src[i+1] == 'r' && src[i+2] == 'y') {
            /* Check word boundary before */
            int word_start = (i == 0) || !cc_is_ident_char(src[i-1]);
            /* Check word boundary after */
            int word_end = (i + 3 >= n) || !cc_is_ident_char(src[i+3]);
            
            if (word_start && word_end) {
                size_t after_try = i + 3;
                /* Skip whitespace */
                while (after_try < n && (src[after_try] == ' ' || src[after_try] == '\t' || src[after_try] == '\n')) after_try++;
                
                /* Check it's not try { block } form */
                if (after_try < n && src[after_try] == '{') {
                    /* try { ... } block form - skip, not handled here */
                } else if (after_try < n && (cc_is_ident_start(src[after_try]) || src[after_try] == '(')) {
                    /* 'try expr' form - find end of expression */
                    size_t expr_start = after_try;
                    size_t expr_end = expr_start;
                    
                    /* Scan expression with balanced parens/braces */
                    int paren = 0, brace = 0, bracket = 0;
                    int in_s = 0, in_c = 0;
                    while (expr_end < n) {
                        char ec = src[expr_end];
                        
                        if (in_s) { if (ec == '\\' && expr_end + 1 < n) { expr_end += 2; continue; } if (ec == '"') in_s = 0; expr_end++; continue; }
                        if (in_c) { if (ec == '\\' && expr_end + 1 < n) { expr_end += 2; continue; } if (ec == '\'') in_c = 0; expr_end++; continue; }
                        if (ec == '"') { in_s = 1; expr_end++; continue; }
                        if (ec == '\'') { in_c = 1; expr_end++; continue; }
                        
                        if (ec == '(' ) { paren++; expr_end++; continue; }
                        if (ec == ')' ) { if (paren > 0) { paren--; expr_end++; continue; } else break; }
                        if (ec == '{' ) { brace++; expr_end++; continue; }
                        if (ec == '}' ) { if (brace > 0) { brace--; expr_end++; continue; } else break; }
                        if (ec == '[' ) { bracket++; expr_end++; continue; }
                        if (ec == ']' ) { if (bracket > 0) { bracket--; expr_end++; continue; } else break; }
                        
                        /* End expression at ';', ',', or unbalanced ')' */
                        if (paren == 0 && brace == 0 && bracket == 0) {
                            if (ec == ';' || ec == ',') break;
                        }
                        
                        expr_end++;
                    }
                    
                    /* Only rewrite if we found a valid expression */
                    if (expr_end > expr_start) {
                        /* Emit: everything up to 'try', then 'cc_try(', then expr, then ')' */
                        cc_sb_append(&out, &out_len, &out_cap, src + last_emit, i - last_emit);
                        cc_sb_append_cstr(&out, &out_len, &out_cap, "cc_try(");
                        cc_sb_append(&out, &out_len, &out_cap, src + expr_start, expr_end - expr_start);
                        cc_sb_append_cstr(&out, &out_len, &out_cap, ")");
                        last_emit = expr_end;
                        i = expr_end;
                        continue;
                    }
                }
            }
        }
        
        i++;
    }
    
    if (last_emit < n) cc_sb_append(&out, &out_len, &out_cap, src + last_emit, n - last_emit);
    return out;
}

/* Rewrite *opt -> cc_unwrap_opt(opt) for variables declared with CCOptional_* type.
   Two-pass approach:
   1. Scan for CCOptional_<T> <varname> declarations
   2. Rewrite *varname to cc_unwrap_opt(varname)
*/
static char* cc__rewrite_optional_unwrap(const char* src, size_t n) {
    if (!src || n == 0) return NULL;
    
    /* Pass 1: Collect optional variable names */
    #define MAX_OPT_VARS 256
    char* opt_vars[MAX_OPT_VARS];
    int opt_var_count = 0;
    
    size_t i = 0;
    int in_line_comment = 0, in_block_comment = 0, in_str = 0, in_chr = 0;
    
    while (i < n && opt_var_count < MAX_OPT_VARS) {
        char c = src[i];
        char c2 = (i + 1 < n) ? src[i + 1] : 0;
        
        if (in_line_comment) { if (c == '\n') in_line_comment = 0; i++; continue; }
        if (in_block_comment) { if (c == '*' && c2 == '/') { in_block_comment = 0; i += 2; continue; } i++; continue; }
        if (in_str) { if (c == '\\' && i + 1 < n) { i += 2; continue; } if (c == '"') in_str = 0; i++; continue; }
        if (in_chr) { if (c == '\\' && i + 1 < n) { i += 2; continue; } if (c == '\'') in_chr = 0; i++; continue; }
        
        if (c == '/' && c2 == '/') { in_line_comment = 1; i += 2; continue; }
        if (c == '/' && c2 == '*') { in_block_comment = 1; i += 2; continue; }
        if (c == '"') { in_str = 1; i++; continue; }
        if (c == '\'') { in_chr = 1; i++; continue; }
        
        /* Look for CCOptional_ type declarations */
        if (c == 'C' && i + 10 < n && strncmp(src + i, "CCOptional_", 11) == 0) {
            /* Skip to end of type name */
            size_t type_start = i;
            i += 11;
            while (i < n && cc_is_ident_char(src[i])) i++;
            /* Skip whitespace */
            while (i < n && (src[i] == ' ' || src[i] == '\t' || src[i] == '\n')) i++;
            /* Check for variable name (not function) */
            if (i < n && cc_is_ident_start(src[i])) {
                size_t var_start = i;
                while (i < n && cc_is_ident_char(src[i])) i++;
                size_t var_len = i - var_start;
                /* Skip whitespace */
                while (i < n && (src[i] == ' ' || src[i] == '\t')) i++;
                /* If followed by '=' or ';', it's a variable declaration */
                if (i < n && (src[i] == '=' || src[i] == ';' || src[i] == ',')) {
                    char* varname = (char*)malloc(var_len + 1);
                    if (varname) {
                        memcpy(varname, src + var_start, var_len);
                        varname[var_len] = 0;
                        opt_vars[opt_var_count++] = varname;
                    }
                }
            }
            continue;
        }
        
        i++;
    }
    
    /* If no optional vars found, nothing to rewrite */
    if (opt_var_count == 0) return NULL;
    
    /* Pass 2: Rewrite *varname to cc_unwrap_opt(varname) */
    char* out = NULL;
    size_t out_len = 0, out_cap = 0;
    
    i = 0;
    size_t last_emit = 0;
    in_line_comment = 0; in_block_comment = 0; in_str = 0; in_chr = 0;
    
    while (i < n) {
        char c = src[i];
        char c2 = (i + 1 < n) ? src[i + 1] : 0;
        
        if (in_line_comment) { if (c == '\n') in_line_comment = 0; i++; continue; }
        if (in_block_comment) { if (c == '*' && c2 == '/') { in_block_comment = 0; i += 2; continue; } i++; continue; }
        if (in_str) { if (c == '\\' && i + 1 < n) { i += 2; continue; } if (c == '"') in_str = 0; i++; continue; }
        if (in_chr) { if (c == '\\' && i + 1 < n) { i += 2; continue; } if (c == '\'') in_chr = 0; i++; continue; }
        
        if (c == '/' && c2 == '/') { in_line_comment = 1; i += 2; continue; }
        if (c == '/' && c2 == '*') { in_block_comment = 1; i += 2; continue; }
        if (c == '"') { in_str = 1; i++; continue; }
        if (c == '\'') { in_chr = 1; i++; continue; }
        
        /* Look for * followed by an optional variable name */
        if (c == '*') {
            size_t star_pos = i;
            i++;
            /* Skip whitespace */
            while (i < n && (src[i] == ' ' || src[i] == '\t')) i++;
            /* Check for identifier */
            if (i < n && cc_is_ident_start(src[i])) {
                size_t var_start = i;
                while (i < n && cc_is_ident_char(src[i])) i++;
                size_t var_len = i - var_start;
                
                /* Check if this identifier is in our opt_vars list */
                int is_opt = 0;
                for (int j = 0; j < opt_var_count; j++) {
                    if (strlen(opt_vars[j]) == var_len && strncmp(opt_vars[j], src + var_start, var_len) == 0) {
                        is_opt = 1;
                        break;
                    }
                }
                
                if (is_opt) {
                    /* Rewrite *varname to cc_unwrap_opt(varname) */
                    cc_sb_append(&out, &out_len, &out_cap, src + last_emit, star_pos - last_emit);
                    cc_sb_append_cstr(&out, &out_len, &out_cap, "cc_unwrap_opt(");
                    cc_sb_append(&out, &out_len, &out_cap, src + var_start, var_len);
                    cc_sb_append_cstr(&out, &out_len, &out_cap, ")");
                    last_emit = i;
                }
            }
            continue;
        }
        
        i++;
    }
    
    /* Free opt_vars */
    for (int j = 0; j < opt_var_count; j++) {
        free(opt_vars[j]);
    }
    
    if (last_emit == 0) {
        /* No rewrites done */
        return NULL;
    }
    
    if (last_emit < n) cc_sb_append(&out, &out_len, &out_cap, src + last_emit, n - last_emit);
    return out;
}

// Rewrite @link("lib") directives into marker comments that the linker phase can extract.
// Input:  @link("curl")
// Output: a comment containing __CC_LINK__ curl
char* cc__rewrite_link_directives(const char* src, size_t n) {
    if (!src || n == 0) return NULL;
    char* out = NULL;
    size_t out_len = 0, out_cap = 0;
    size_t i = 0;
    size_t last_emit = 0;
    int in_line_comment = 0;
    int in_block_comment = 0;
    int in_str = 0;
    int in_chr = 0;

    while (i < n) {
        char c = src[i];
        char c2 = (i + 1 < n) ? src[i + 1] : 0;

        /* Track comment/string state */
        if (in_line_comment) {
            if (c == '\n') in_line_comment = 0;
            i++;
            continue;
        }
        if (in_block_comment) {
            if (c == '*' && c2 == '/') {
                i += 2;
                in_block_comment = 0;
                continue;
            }
            i++;
            continue;
        }
        if (in_str) {
            if (c == '\\' && i + 1 < n) { i += 2; continue; }
            if (c == '"') in_str = 0;
            i++;
            continue;
        }
        if (in_chr) {
            if (c == '\\' && i + 1 < n) { i += 2; continue; }
            if (c == '\'') in_chr = 0;
            i++;
            continue;
        }

        /* Check for comment/string start */
        if (c == '/' && c2 == '/') { in_line_comment = 1; i += 2; continue; }
        if (c == '/' && c2 == '*') { in_block_comment = 1; i += 2; continue; }
        if (c == '"') { in_str = 1; i++; continue; }
        if (c == '\'') { in_chr = 1; i++; continue; }

        /* Look for @link("...") */
        if (c == '@' && i + 5 < n && strncmp(src + i, "@link(", 6) == 0) {
            size_t start = i;
            i += 6;  /* skip @link( */
            
            /* Skip whitespace */
            while (i < n && (src[i] == ' ' || src[i] == '\t')) i++;
            
            /* Expect " */
            if (i < n && src[i] == '"') {
                i++;  /* skip opening " */
                size_t lib_start = i;
                
                /* Find closing " */
                while (i < n && src[i] != '"' && src[i] != '\n') i++;
                
                if (i < n && src[i] == '"') {
                    size_t lib_end = i;
                    i++;  /* skip closing " */
                    
                    /* Skip whitespace and closing ) */
                    while (i < n && (src[i] == ' ' || src[i] == '\t')) i++;
                    if (i < n && src[i] == ')') {
                        i++;  /* skip ) */
                        
                        /* Success! Emit up to @link, then emit marker comment */
                        cc_sb_append(&out, &out_len, &out_cap, src + last_emit, start - last_emit);
                        
                        // Emit marker: __CC_LINK__ libname 
                        cc_sb_append_cstr(&out, &out_len, &out_cap, "/* __CC_LINK__ ");
                        cc_sb_append(&out, &out_len, &out_cap, src + lib_start, lib_end - lib_start);
                        cc_sb_append_cstr(&out, &out_len, &out_cap, " */");
                        
                        last_emit = i;
                        continue;
                    }
                }
            }
            /* If parsing failed, continue from after @ */
            i = start + 1;
        }

        i++;
    }

    if (last_emit == 0) return NULL;  /* No rewrites */
    if (last_emit < n) cc_sb_append(&out, &out_len, &out_cap, src + last_emit, n - last_emit);
    return out;
}

/* Check that channel ops inside @async functions are awaited.
   Returns 0 if OK, -1 if errors were emitted.
   This runs BEFORE rewrites so we can see the original source forms. */
static int cc__check_async_chan_await(const char* src, size_t n, const char* input_path) {
    if (!src || n == 0) return 0;

    int errors = 0;
    int line = 1, col = 1;
    int in_async_fn = 0;      /* 1 if inside @async function body */
    int async_brace_depth = 0; /* Brace depth when we entered the async function */

    /* Comment/string tracking */
    int in_lc = 0, in_bc = 0, in_str = 0, in_chr = 0;
    int brace_depth = 0;

    for (size_t i = 0; i < n; i++) {
        char c = src[i];
        char c2 = (i + 1 < n) ? src[i + 1] : 0;

        /* Track position */
        if (c == '\n') { line++; col = 1; } else { col++; }

        /* Skip comments and strings */
        if (in_lc) { if (c == '\n') in_lc = 0; continue; }
        if (in_bc) { if (c == '*' && c2 == '/') { in_bc = 0; i++; col++; } continue; }
        if (in_str) { if (c == '\\' && i + 1 < n) { i++; col++; continue; } if (c == '"') in_str = 0; continue; }
        if (in_chr) { if (c == '\\' && i + 1 < n) { i++; col++; continue; } if (c == '\'') in_chr = 0; continue; }
        if (c == '/' && c2 == '/') { in_lc = 1; i++; col++; continue; }
        if (c == '/' && c2 == '*') { in_bc = 1; i++; col++; continue; }
        if (c == '"') { in_str = 1; continue; }
        if (c == '\'') { in_chr = 1; continue; }

        /* Track braces */
        if (c == '{') brace_depth++;
        else if (c == '}') {
            brace_depth--;
            if (in_async_fn && brace_depth < async_brace_depth) {
                in_async_fn = 0; /* Exited async function */
            }
        }

        /* Detect @async function start */
        if (c == '@') {
            size_t j = i + 1;
            while (j < n && (src[j] == ' ' || src[j] == '\t')) j++;
            if (j + 5 <= n && memcmp(src + j, "async", 5) == 0 && (j + 5 >= n || !cc_is_ident_char(src[j + 5]))) {
                /* Found @async - find the function body */
                j += 5;
                /* Skip to opening brace */
                int found_brace = 0;
                while (j < n) {
                    if (src[j] == '{') { found_brace = 1; break; }
                    if (src[j] == ';') break; /* Declaration, not definition */
                    j++;
                }
                if (found_brace) {
                    in_async_fn = 1;
                    async_brace_depth = brace_depth + 1; /* We'll hit the '{' soon */
                }
            }
            continue;
        }

        /* Inside @async function, check for channel ops.
           Note: We only check chan_send/chan_recv here, not UFCS form (.send/.recv).
           UFCS form is handled by the UFCS pass which correctly emits task variants in await context. */
        if (in_async_fn) {
            /* Check for chan_send, chan_recv (macro forms) */
            int is_chan_op = 0;
            const char* op_name = NULL;
            size_t op_len = 0;

            if (i + 9 <= n && memcmp(src + i, "chan_send", 9) == 0 && (i + 9 >= n || !cc_is_ident_char(src[i + 9]))) {
                /* Make sure it's not preceded by identifier char (to avoid matching cc_chan_send) */
                if (i > 0 && cc_is_ident_char(src[i - 1])) { /* skip */ }
                else { is_chan_op = 1; op_name = "chan_send"; op_len = 9; }
            } else if (i + 9 <= n && memcmp(src + i, "chan_recv", 9) == 0 && (i + 9 >= n || !cc_is_ident_char(src[i + 9]))) {
                if (i > 0 && cc_is_ident_char(src[i - 1])) { /* skip */ }
                else { is_chan_op = 1; op_name = "chan_recv"; op_len = 9; }
            }

            if (is_chan_op) {
                /* Check if preceded by "await" (skipping whitespace/comments backwards) */
                int has_await = 0;
                size_t k = i;
                while (k > 0) {
                    k--;
                    char ck = src[k];
                    if (ck == ' ' || ck == '\t' || ck == '\n' || ck == '\r') continue;
                    /* Check for "await" ending at position k */
                    if (k >= 4 && memcmp(src + k - 4, "await", 5) == 0) {
                        /* Ensure "await" is not part of a larger identifier */
                        if (k >= 5 && cc_is_ident_char(src[k - 5])) break;
                        has_await = 1;
                    }
                    break;
                }

                if (!has_await) {
                    char rel[1024];
                    cc_path_rel_to_repo(input_path, rel, sizeof(rel));
                    fprintf(stderr, "%s:%d:%d: error: channel operation '%s' must be awaited in @async function\n",
                            rel, line, col, op_name);
                    fprintf(stderr, "%s:%d:%d: note: add 'await' before this call\n", rel, line, col);
                    errors++;
                }
                i += op_len - 1; /* Skip past the op name */
            }
        }
    }

    return errors > 0 ? -1 : 0;
}

int cc_preprocess_file(const char* input_path, char* out_path, size_t out_path_sz) {
    if (!input_path || !out_path || out_path_sz == 0) return -1;
    char tmp_path[] = "/tmp/cc_pp_XXXXXX.c";
    int fd = mkstemps(tmp_path, 2); /* keep .c suffix */
    if (fd < 0) return -1;

    FILE *in = fopen(input_path, "r");
    FILE *out = fdopen(fd, "w");
    if (!in || !out) {
        if (in) fclose(in);
        if (out) fclose(out);
        close(fd);
        unlink(tmp_path);
        return -1;
    }

    /* Read full file so we can rewrite constructs that are not valid C syntax. */
    fseek(in, 0, SEEK_END);
    long in_n = ftell(in);
    fseek(in, 0, SEEK_SET);
    if (in_n < 0 || in_n > (1 << 22)) { /* 4MB cap for now */
        fclose(in);
        fclose(out);
        unlink(tmp_path);
        return -1;
    }
    char* buf = (char*)malloc((size_t)in_n + 1);
    if (!buf) {
        fclose(in);
        fclose(out);
        unlink(tmp_path);
        return -1;
    }
    size_t got = fread(buf, 1, (size_t)in_n, in);
    buf[got] = 0;

    /* Check for unawaited channel ops in @async functions (before rewrites).
       Skip this check for temp/reparse files (they have already been checked). */
    const char* basename = strrchr(input_path, '/');
    basename = basename ? basename + 1 : input_path;
    int is_temp_file = (strncmp(basename, "cc_reparse_", 11) == 0 ||
                        strncmp(basename, "cc_pp_", 6) == 0 ||
                        strncmp(input_path, "/tmp/", 5) == 0);
    if (!is_temp_file) {
        int chan_err = cc__check_async_chan_await(buf, got, input_path);
        if (chan_err != 0) {
            fclose(in);
            fclose(out);
            free(buf);
            unlink(tmp_path);
            return -1;
        }
    }

    char* rewritten_deadline = cc__rewrite_with_deadline_syntax(buf, got);
    const char* cur = rewritten_deadline ? rewritten_deadline : buf;
    size_t cur_n = rewritten_deadline ? strlen(rewritten_deadline) : got;

    char* rewritten_match = cc__rewrite_match_syntax(cur, cur_n, input_path);
    const char* cur_m = rewritten_match ? rewritten_match : cur;
    size_t cur_m_n = rewritten_match ? strlen(rewritten_match) : cur_n;

    char* rewritten_slice = cc__rewrite_slice_types(cur_m, cur_m_n, input_path);
    const char* cur2 = rewritten_slice ? rewritten_slice : cur_m;
    size_t cur2_n = rewritten_slice ? strlen(rewritten_slice) : cur_m_n;

    char* rewritten_chan = cc__rewrite_chan_handle_types(cur2, cur2_n, input_path);
    const char* cur3 = rewritten_chan ? rewritten_chan : cur2;
    size_t cur3_n = rewritten_chan ? strlen(rewritten_chan) : cur2_n;

    /* Rewrite T? -> CCOptional_T */
    char* rewritten_opt = cc__rewrite_optional_types(cur3, cur3_n, input_path);
    const char* cur4 = rewritten_opt ? rewritten_opt : cur3;
    size_t cur4_n = rewritten_opt ? strlen(rewritten_opt) : cur3_n;

    /* Rewrite T!E -> CCResult_T_E */
    char* rewritten_res = cc__rewrite_result_types(cur4, cur4_n, input_path);
    const char* cur5 = rewritten_res ? rewritten_res : cur4;
    size_t cur5_n = rewritten_res ? strlen(rewritten_res) : cur4_n;

    /* Rewrite try expr -> cc_try(expr) */
    char* rewritten_try = cc__rewrite_try_exprs(cur5, cur5_n);
    const char* cur6 = rewritten_try ? rewritten_try : cur5;
    size_t cur6_n = rewritten_try ? strlen(rewritten_try) : cur5_n;

    /* Rewrite *opt -> cc_unwrap_opt(opt) for optional variables */
    char* rewritten_unwrap = cc__rewrite_optional_unwrap(cur6, cur6_n);
    const char* cur7 = rewritten_unwrap ? rewritten_unwrap : cur6;
    size_t cur7_n = rewritten_unwrap ? strlen(rewritten_unwrap) : cur6_n;

    // Rewrite @link("lib") -> marker comments for linker
    char* rewritten_link = cc__rewrite_link_directives(cur7, cur7_n);
    const char* use = rewritten_link ? rewritten_link : cur7;

    char rel[1024];
    fprintf(out, "#line 1 \"%s\"\n", cc_path_rel_to_repo(input_path, rel, sizeof(rel)));
    fputs(use, out);

    free(rewritten_link);
    free(rewritten_unwrap);
    free(rewritten_try);
    free(rewritten_res);
    free(rewritten_opt);
    free(rewritten_chan);
    free(rewritten_slice);
    free(rewritten_match);
    free(rewritten_deadline);
    free(buf);

    fclose(in);
    fclose(out);

    strncpy(out_path, tmp_path, out_path_sz - 1);
    out_path[out_path_sz - 1] = '\0';
    return 0;
}

