#include "preprocess.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdarg.h>

#include "preprocess/type_registry.h"
#include "util/path.h"
#include "util/text.h"

/* ========================================================================== */
/* Diagnostic helpers (gcc/clang compatible format)                           */
/* Format: file:line:col: error: category: message                            */
/* Categories: syntax, channel, type, async, closure, slice                   */
/* ========================================================================== */

static void cc_pp_error_cat(const char* file, int line, int col, 
                            const char* category, const char* fmt, ...) {
    const char* f = file ? file : "<input>";
    int l = (line > 0) ? line : 1;
    int c = (col > 0) ? col : 1;
    fprintf(stderr, "%s:%d:%d: error: %s: ", f, l, c, category);
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fprintf(stderr, "\n");
}

/* ========================================================================== */
/* Scanner helper for skipping comments and strings                           */
/* Reduces code duplication across cc__rewrite_* functions                    */
/* ========================================================================== */

typedef struct {
    int in_line_comment;
    int in_block_comment;
    int in_str;
    int in_chr;
    int line;  /* Current line (1-based), updated if track_pos is true */
    int col;   /* Current column (1-based), updated if track_pos is true */
} CCScannerState;

/* Initialize scanner state */
static void cc_scanner_init(CCScannerState* s) {
    s->in_line_comment = 0;
    s->in_block_comment = 0;
    s->in_str = 0;
    s->in_chr = 0;
    s->line = 1;
    s->col = 1;
}

/* Process current character and advance past non-code (comments, strings).
 * Returns 1 if we're in a comment/string (caller should continue to next char).
 * Returns 0 if we're at actual code (caller should process the character).
 * Updates *pos to skip multi-char sequences.
 * Updates s->line and s->col to track position.
 */
static int cc_scanner_skip_non_code(CCScannerState* s, const char* src, size_t n, size_t* pos) {
    size_t i = *pos;
    if (i >= n) return 0;
    
    char c = src[i];
    char c2 = (i + 1 < n) ? src[i + 1] : 0;
    
    /* Track newlines for position */
    if (c == '\n') { s->line++; s->col = 1; } else { s->col++; }
    
    /* Inside line comment */
    if (s->in_line_comment) {
        if (c == '\n') s->in_line_comment = 0;
        (*pos)++;
        return 1;
    }
    
    /* Inside block comment */
    if (s->in_block_comment) {
        if (c == '*' && c2 == '/') {
            s->in_block_comment = 0;
            *pos += 2;
            s->col++;  /* Account for '/' */
        } else {
            (*pos)++;
        }
        return 1;
    }
    
    /* Inside string literal */
    if (s->in_str) {
        if (c == '\\' && i + 1 < n) {
            *pos += 2;  /* Skip escape sequence */
            s->col++;   /* Account for escaped char */
        } else {
            if (c == '"') s->in_str = 0;
            (*pos)++;
        }
        return 1;
    }
    
    /* Inside char literal */
    if (s->in_chr) {
        if (c == '\\' && i + 1 < n) {
            *pos += 2;  /* Skip escape sequence */
            s->col++;   /* Account for escaped char */
        } else {
            if (c == '\'') s->in_chr = 0;
            (*pos)++;
        }
        return 1;
    }
    
    /* Check for start of comment/string */
    if (c == '/' && c2 == '/') { s->in_line_comment = 1; *pos += 2; s->col++; return 1; }
    if (c == '/' && c2 == '*') { s->in_block_comment = 1; *pos += 2; s->col++; return 1; }
    if (c == '"') { s->in_str = 1; (*pos)++; return 1; }
    if (c == '\'') { s->in_chr = 1; (*pos)++; return 1; }
    
    /* At actual code - undo the col++ since caller will handle this char */
    if (c != '\n') s->col--;
    
    return 0;
}

/* ========================================================================== */
/* End scanner helper                                                         */
/* ========================================================================== */

/* ========================================================================== */
/* Pass chain helper - tracks allocations for cleanup                         */
/* ========================================================================== */

#define CC_PASS_CHAIN_MAX 32

typedef struct {
    const char* src;      /* Current source buffer */
    size_t len;           /* Current source length */
    char* allocs[CC_PASS_CHAIN_MAX];  /* Tracked allocations */
    int n_allocs;         /* Number of tracked allocations */
} CCPassChain;

/* Initialize chain with source buffer (buffer is NOT owned by chain) */
static inline void cc_pass_chain_init(CCPassChain* c, const char* src, size_t len) {
    c->src = src;
    c->len = len;
    c->n_allocs = 0;
}

/* Apply a pass result to the chain.
 * - If result is NULL: no change, chain continues with current buffer
 * - If result is (char*)-1: error, returns -1 (caller should cleanup)
 * - Otherwise: result becomes new source, tracked for later cleanup
 * Returns 0 on success, -1 on error.
 */
static inline int cc_pass_chain_apply(CCPassChain* c, char* result) {
    if (result == (char*)-1) return -1;  /* Error sentinel */
    if (result) {
        if (c->n_allocs < CC_PASS_CHAIN_MAX) {
            c->allocs[c->n_allocs++] = result;
        }
        c->src = result;
        c->len = strlen(result);
    }
    return 0;
}

/* Free all tracked allocations */
static inline void cc_pass_chain_free(CCPassChain* c) {
    for (int i = 0; i < c->n_allocs; i++) {
        free(c->allocs[i]);
    }
    c->n_allocs = 0;
}

/* Convenience macros for common patterns */
#define CC_CHAIN(c, call) \
    do { if (cc_pass_chain_apply(&(c), call) < 0) goto chain_cleanup; } while(0)

/* ========================================================================== */
/* End pass chain helper                                                      */
/* ========================================================================== */

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

    CCScannerState scanner;
    cc_scanner_init(&scanner);
    int line = 1;
    int col = 1;
    unsigned long counter = 0;

    while (i < n) {
        char c = src[i];
        if (c == '\n') { line++; col = 1; }

        /* Skip comments and strings using helper */
        size_t old_i = i;
        if (cc_scanner_skip_non_code(&scanner, src, n, &i)) {
            col += (int)(i - old_i);
            continue;
        }

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
                        cc_pp_error_cat(cc_path_rel_to_repo(input_path ? input_path : "<input>", rel, sizeof(rel)),
                                    line, col, "syntax", "unterminated @match block");
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
                                    cc_pp_error_cat(cc_path_rel_to_repo(input_path ? input_path : "<input>", rel, sizeof(rel)),
                                                line, col, "syntax", "@match case header must be <chan>.recv(ptr) or <chan>.send(value) or is_cancelled()");
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
                                    cc_pp_error_cat(cc_path_rel_to_repo(input_path ? input_path : "<input>", rel, sizeof(rel)),
                                                line, col, "syntax", "malformed @match case header");
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
                                           counter, counter);

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

/* Helper: wrap a closure with typed parameter for CCClosure1.
   Transforms: [captures](Type param) => body
   Into:       [captures](intptr_t __arg) => { Type param = (Type)__arg; body }
   
   If param is already intptr_t, returns the original unchanged. */
static void cc__wrap_typed_closure1(const char* closure, char* out, size_t out_cap) {
    if (!closure || !out || out_cap == 0) return;
    out[0] = 0;
    
    /* Find '[' (captures start) */
    const char* p = closure;
    while (*p && *p != '[') p++;
    if (!*p) { strncpy(out, closure, out_cap - 1); out[out_cap - 1] = 0; return; }
    
    /* Find '](' to get to params */
    const char* cap_start = p;
    while (*p && !(*p == ']' && *(p + 1) == '(')) p++;
    if (!*p) { strncpy(out, closure, out_cap - 1); out[out_cap - 1] = 0; return; }
    
    size_t cap_len = p - cap_start + 1;  /* include ] */
    p++;  /* skip ] */
    if (*p != '(') { strncpy(out, closure, out_cap - 1); out[out_cap - 1] = 0; return; }
    p++;  /* skip ( */
    
    /* Skip whitespace */
    while (*p && (*p == ' ' || *p == '\t')) p++;
    
    /* Extract parameter type */
    const char* type_start = p;
    /* Scan type - handle pointers like "CCArena*" */
    while (*p && *p != ')' && *p != ' ' && *p != '\t') {
        if (*p == '*') { p++; break; }  /* pointer type ends at * */
        p++;
    }
    size_t type_len = p - type_start;
    
    /* Check if it's already intptr_t */
    if (type_len == 8 && strncmp(type_start, "intptr_t", 8) == 0) {
        strncpy(out, closure, out_cap - 1);
        out[out_cap - 1] = 0;
        return;
    }
    
    /* Skip whitespace to parameter name */
    while (*p && (*p == ' ' || *p == '\t')) p++;
    
    /* Extract parameter name */
    const char* name_start = p;
    while (*p && *p != ')' && *p != ' ' && *p != '\t') p++;
    size_t name_len = p - name_start;
    
    if (name_len == 0) {
        /* No param name, type might BE the name (e.g., just "r") - don't wrap */
        strncpy(out, closure, out_cap - 1);
        out[out_cap - 1] = 0;
        return;
    }
    
    /* Find => and body */
    while (*p && !(*p == '=' && *(p + 1) == '>')) p++;
    if (!*p) { strncpy(out, closure, out_cap - 1); out[out_cap - 1] = 0; return; }
    p += 2;  /* skip => */
    while (*p && (*p == ' ' || *p == '\t')) p++;
    
    /* Rest is body */
    const char* body = p;
    
    /* Build wrapped closure:
       [captures](intptr_t __arg) => { Type name = (Type)__arg; body } */
    char type_buf[128], name_buf[64];
    if (type_len >= sizeof(type_buf)) type_len = sizeof(type_buf) - 1;
    if (name_len >= sizeof(name_buf)) name_len = sizeof(name_buf) - 1;
    memcpy(type_buf, type_start, type_len);
    type_buf[type_len] = 0;
    memcpy(name_buf, name_start, name_len);
    name_buf[name_len] = 0;
    
    /* Check if body is already a block */
    int body_is_block = (*body == '{');
    
    if (body_is_block) {
        /* Body is { ... }, insert declaration after { */
        snprintf(out, out_cap, "%.*s(intptr_t __arg) => { %s %s = (%s)__arg; %s",
                 (int)cap_len, cap_start, type_buf, name_buf, type_buf, body + 1);
    } else {
        /* Body is expression, wrap in block */
        snprintf(out, out_cap, "%.*s(intptr_t __arg) => { %s %s = (%s)__arg; return %s; }",
                 (int)cap_len, cap_start, type_buf, name_buf, type_buf, body);
    }
}

static char* cc__rewrite_chan_handle_types(const char* src, size_t n, const char* input_path) {
    if (!src || n == 0) return NULL;
    char* out = NULL;
    size_t out_len = 0, out_cap = 0;

    size_t i = 0;
    size_t last_emit = 0;
    CCScannerState scanner;
    cc_scanner_init(&scanner);

    while (i < n) {
        /* Skip comments and strings using shared helper */
        if (cc_scanner_skip_non_code(&scanner, src, n, &i)) continue;

        char c = src[i];
        if (c == '[') {
            size_t j = i + 1;
            while (j < n && (src[j] == ' ' || src[j] == '\t')) j++;
            if (j < n && src[j] == '~') {
                /* Check for 'owned' keyword - skip owned channels, they're handled by later pass */
                int is_owned = 0;
                {
                    size_t scan = j + 1;
                    /* Skip capacity expression - can be:
                     * - digits: 4, 16
                     * - expressions: (cap + 2), cap * 2
                     * - identifiers: my_cap
                     * Stop when we hit 'owned', '>', '<', or ']' */
                    while (scan < n && src[scan] != ']') {
                        char sc = src[scan];
                        /* Skip whitespace */
                        if (sc == ' ' || sc == '\t') { scan++; continue; }
                        /* Check for 'owned' keyword before processing identifiers */
                        if (scan + 5 <= n && memcmp(src + scan, "owned", 5) == 0) {
                            char next = (scan + 5 < n) ? src[scan + 5] : 0;
                            int is_ident = (next == '_' || (next >= 'A' && next <= 'Z') || 
                                           (next >= 'a' && next <= 'z') || (next >= '0' && next <= '9'));
                            if (!is_ident) break;  /* Found 'owned' keyword */
                        }
                        /* Skip digits, identifiers, operators */
                        if ((sc >= '0' && sc <= '9') || sc == '_' ||
                            (sc >= 'a' && sc <= 'z') || (sc >= 'A' && sc <= 'Z') ||
                            sc == '+' || sc == '-' || sc == '*' || sc == '/') { scan++; continue; }
                        /* Skip parenthesized expressions */
                        if (sc == '(') {
                            int depth = 1;
                            scan++;
                            while (scan < n && depth > 0) {
                                if (src[scan] == '(') depth++;
                                else if (src[scan] == ')') depth--;
                                scan++;
                            }
                            continue;
                        }
                        break;  /* Hit something else (like '>' or '<') */
                    }
                    while (scan < n && (src[scan] == ' ' || src[scan] == '\t')) scan++;
                    if (scan + 5 <= n && memcmp(src + scan, "owned", 5) == 0) {
                        char next = (scan + 5 < n) ? src[scan + 5] : 0;
                        int is_ident = (next == '_' || (next >= 'A' && next <= 'Z') || (next >= 'a' && next <= 'z') || (next >= '0' && next <= '9'));
                        if (!is_ident) {
                            is_owned = 1;
                        }
                    }
                }
                
                if (is_owned) {
                    /* Transform owned channel: T[~N owned { ... }] varname; */
                    /* Skip capacity expression (may include parentheses) to find 'owned' */
                    size_t scan = j + 1;
                    while (scan < n && src[scan] != ']') {
                        char sc = src[scan];
                        if (sc == ' ' || sc == '\t') { scan++; continue; }
                        /* Check for 'owned' keyword */
                        if (scan + 5 <= n && memcmp(src + scan, "owned", 5) == 0) {
                            char next = (scan + 5 < n) ? src[scan + 5] : 0;
                            int is_kw = !(next == '_' || (next >= 'A' && next <= 'Z') || 
                                         (next >= 'a' && next <= 'z') || (next >= '0' && next <= '9'));
                            if (is_kw) break;
                        }
                        if ((sc >= '0' && sc <= '9') || sc == '_' ||
                            (sc >= 'a' && sc <= 'z') || (sc >= 'A' && sc <= 'Z') ||
                            sc == '+' || sc == '-' || sc == '*' || sc == '/') { scan++; continue; }
                        if (sc == '(') {
                            int depth = 1;
                            scan++;
                            while (scan < n && depth > 0) {
                                if (src[scan] == '(') depth++;
                                else if (src[scan] == ')') depth--;
                                scan++;
                            }
                            continue;
                        }
                        break;
                    }
                    while (scan < n && (src[scan] == ' ' || src[scan] == '\t')) scan++;
                    scan += 5;  /* Skip "owned" */
                    while (scan < n && (src[scan] == ' ' || src[scan] == '\t')) scan++;
                    
                    if (scan >= n || src[scan] != '{') {
                        char rel[1024];
                        cc_pp_error_cat(cc_path_rel_to_repo(input_path ? input_path : "<input>", rel, sizeof(rel)),
                                scanner.line, scanner.col, "channel", "owned channel requires { ... } block");
                        free(out);
                        return NULL;
                    }
                    
                    /* Find matching '}' for the owned block */
                    size_t brace_start = scan;
                    int brace_depth = 0;
                    int in_str2 = 0, in_chr2 = 0;
                    size_t brace_end = scan;
                    for (size_t bi = scan; bi < n; bi++) {
                        char bc = src[bi];
                        if (in_str2) { if (bc == '\\' && bi + 1 < n) { bi++; continue; } if (bc == '"') in_str2 = 0; continue; }
                        if (in_chr2) { if (bc == '\\' && bi + 1 < n) { bi++; continue; } if (bc == '\'') in_chr2 = 0; continue; }
                        if (bc == '"') { in_str2 = 1; continue; }
                        if (bc == '\'') { in_chr2 = 1; continue; }
                        if (bc == '{') brace_depth++;
                        else if (bc == '}') {
                            brace_depth--;
                            if (brace_depth == 0) { brace_end = bi; break; }
                        }
                    }
                    
                    if (brace_depth != 0) {
                        char rel[1024];
                        cc_pp_error_cat(cc_path_rel_to_repo(input_path ? input_path : "<input>", rel, sizeof(rel)),
                                scanner.line, scanner.col, "channel", "unterminated owned block");
                        free(out);
                        return NULL;
                    }
                    
                    /* Find ']' after the owned block */
                    size_t k = brace_end + 1;
                    while (k < n && (src[k] == ' ' || src[k] == '\t')) k++;
                    if (k >= n || src[k] != ']') {
                        char rel[1024];
                        cc_pp_error_cat(cc_path_rel_to_repo(input_path ? input_path : "<input>", rel, sizeof(rel)),
                                scanner.line, scanner.col, "channel", "expected ']' after owned block");
                        free(out);
                        return NULL;
                    }
                    
                    /* Find variable name after ] */
                    size_t var_start = k + 1;
                    while (var_start < n && (src[var_start] == ' ' || src[var_start] == '\t')) var_start++;
                    size_t var_end = var_start;
                    while (var_end < n && (src[var_end] == '_' || (src[var_end] >= 'A' && src[var_end] <= 'Z') ||
                           (src[var_end] >= 'a' && src[var_end] <= 'z') || (src[var_end] >= '0' && src[var_end] <= '9'))) var_end++;
                    
                    if (var_end == var_start) {
                        char rel[1024];
                        cc_pp_error_cat(cc_path_rel_to_repo(input_path ? input_path : "<input>", rel, sizeof(rel)),
                                scanner.line, scanner.col, "channel", "expected variable name after owned channel type");
                        free(out);
                        return NULL;
                    }
                    
                    /* Find semicolon */
                    size_t semi = var_end;
                    while (semi < n && src[semi] != ';') semi++;
                    if (semi >= n) {
                        char rel[1024];
                        cc_pp_error_cat(cc_path_rel_to_repo(input_path ? input_path : "<input>", rel, sizeof(rel)),
                                scanner.line, scanner.col, "channel", "expected ';' after owned channel declaration");
                        free(out);
                        return NULL;
                    }
                    
                    /* Extract element type (before [) */
                    size_t ty_start = cc__scan_back_to_delim(src, i);
                    char elem_ty[256];
                    size_t elem_len = i - ty_start;
                    if (elem_len >= sizeof(elem_ty)) elem_len = sizeof(elem_ty) - 1;
                    memcpy(elem_ty, src + ty_start, elem_len);
                    elem_ty[elem_len] = 0;
                    while (elem_len > 0 && (elem_ty[elem_len - 1] == ' ' || elem_ty[elem_len - 1] == '\t')) elem_ty[--elem_len] = 0;
                    
                    /* Extract capacity (between ~ and owned) - handles expressions like (cap + 2) */
                    char cap_expr[128] = "0";
                    {
                        size_t cs = j + 1;
                        while (cs < brace_start && (src[cs] == ' ' || src[cs] == '\t')) cs++;
                        size_t ce = cs;
                        /* Scan capacity expression, handling parentheses */
                        int paren_depth = 0;
                        while (ce < brace_start) {
                            char ec = src[ce];
                            if (ec == '(') { paren_depth++; ce++; continue; }
                            if (ec == ')') { paren_depth--; ce++; continue; }
                            if (paren_depth > 0) { ce++; continue; }  /* Inside parens, include everything */
                            /* Outside parens: stop at whitespace before 'owned' */
                            if (ec == ' ' || ec == '\t') {
                                size_t peek = ce;
                                while (peek < brace_start && (src[peek] == ' ' || src[peek] == '\t')) peek++;
                                if (peek + 5 <= brace_start && memcmp(src + peek, "owned", 5) == 0) break;
                            }
                            if (memcmp(src + ce, "owned", 5) == 0) break;
                            ce++;
                        }
                        /* Trim trailing whitespace */
                        while (ce > cs && (src[ce - 1] == ' ' || src[ce - 1] == '\t')) ce--;
                        if (ce > cs) {
                            size_t cl = ce - cs;
                            if (cl >= sizeof(cap_expr)) cl = sizeof(cap_expr) - 1;
                            memcpy(cap_expr, src + cs, cl);
                            cap_expr[cl] = 0;
                        }
                    }
                    
                    /* Extract variable name */
                    char var_name[128];
                    size_t vlen = var_end - var_start;
                    if (vlen >= sizeof(var_name)) vlen = sizeof(var_name) - 1;
                    memcpy(var_name, src + var_start, vlen);
                    var_name[vlen] = 0;
                    
                    /* Extract owned block content (closures) - keep it for closure pass */
                    char owned_content[4096];
                    size_t owned_len = brace_end - brace_start - 1;  /* Exclude { and } */
                    if (owned_len >= sizeof(owned_content)) owned_len = sizeof(owned_content) - 1;
                    memcpy(owned_content, src + brace_start + 1, owned_len);
                    owned_content[owned_len] = 0;
                    
                    /* Generate transformed code:
                     * Note: We can't fully expand the closures here, but we can generate
                     * a form that's valid C and will be processed by later passes. */
                    
                    /* For now, emit a comment and let pass_channel_syntax handle it later.
                     * But we need to emit something valid for TCC to parse...
                     * 
                     * Actually, the cleanest is to emit the manual API form:
                     * CCChan* varname = cc_chan_create_owned(cap, sizeof(elem), create, destroy, reset);
                     * But we need to extract the closures...
                     *
                     * Simplest working approach: emit placeholders that will compile
                     * and mark with a special comment for later pass.
                     */
                    
                    /* Emit up to the owned channel */
                    if (ty_start > last_emit) {
                        cc_sb_append(&out, &out_len, &out_cap, src + last_emit, ty_start - last_emit);
                    }
                    
                    /* Emit transformed owned channel as a placeholder that compiles.
                     * The closures are embedded and will be processed by closure pass. */
                    char buf[8192];
                    static int owned_id = 0;
                    int id = owned_id++;
                    
                    /* Extract closures from owned_content by looking for .create, .destroy, .reset */
                    char create_c[2048] = "{0}";
                    char destroy_c[2048] = "{0}";
                    char reset_c[2048] = "{0}";
                    
                    /* Simple extraction: find ".field = " and copy until next "," or "}" at same depth */
                    const char* p = owned_content;
                    while (*p) {
                        while (*p && (*p == ' ' || *p == '\t' || *p == '\n' || *p == ',')) p++;
                        if (*p != '.') { p++; continue; }
                        p++;
                        char field[32];
                        size_t fn = 0;
                        while (*p && fn + 1 < sizeof(field) && ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || *p == '_')) {
                            field[fn++] = *p++;
                        }
                        field[fn] = 0;
                        while (*p && (*p == ' ' || *p == '\t')) p++;
                        if (*p != '=') continue;
                        p++;
                        while (*p && (*p == ' ' || *p == '\t')) p++;
                        
                        /* Find end of closure */
                        const char* closure_start = p;
                        int depth = 0;
                        int in_s = 0;
                        while (*p) {
                            if (in_s) { if (*p == '\\' && *(p+1)) p++; else if (*p == '"') in_s = 0; p++; continue; }
                            if (*p == '"') { in_s = 1; p++; continue; }
                            if (*p == '(' || *p == '[' || *p == '{') depth++;
                            else if (*p == ')' || *p == ']' || *p == '}') depth--;
                            if (depth < 0 || (depth == 0 && *p == ',')) break;
                            p++;
                        }
                        
                        size_t clen = p - closure_start;
                        char* dest = NULL;
                        size_t dcap = 0;
                        if (strcmp(field, "create") == 0) { dest = create_c; dcap = sizeof(create_c); }
                        else if (strcmp(field, "destroy") == 0) { dest = destroy_c; dcap = sizeof(destroy_c); }
                        else if (strcmp(field, "reset") == 0) { dest = reset_c; dcap = sizeof(reset_c); }
                        
                        if (dest && clen < dcap) {
                            memcpy(dest, closure_start, clen);
                            dest[clen] = 0;
                            /* Trim trailing whitespace */
                            while (clen > 0 && (dest[clen-1] == ' ' || dest[clen-1] == '\t' || dest[clen-1] == '\n')) dest[--clen] = 0;
                        }
                    }
                    
                    /* Wrap destroy/reset closures to handle typed parameters.
                       Converts [](CCArena* a) => ... to [](intptr_t __arg) => { CCArena* a = (CCArena*)__arg; ... } */
                    char destroy_wrapped[2048], reset_wrapped[2048];
                    cc__wrap_typed_closure1(destroy_c, destroy_wrapped, sizeof(destroy_wrapped));
                    cc__wrap_typed_closure1(reset_c, reset_wrapped, sizeof(reset_wrapped));
                    
                    /* Generate code with closure variables and channel creation */
                    snprintf(buf, sizeof(buf),
                             "/* owned channel %s */\n"
                             "CCClosure0 __cc_owned_%d_create = %s;\n"
                             "CCClosure1 __cc_owned_%d_destroy = %s;\n"
                             "CCClosure1 __cc_owned_%d_reset = %s;\n"
                             "CCChan* %s = cc_chan_create_owned(%s, sizeof(%s), "
                             "__cc_owned_%d_create, __cc_owned_%d_destroy, __cc_owned_%d_reset)",
                             var_name,
                             id, create_c,
                             id, destroy_wrapped,
                             id, reset_wrapped,
                             var_name, cap_expr, elem_ty,
                             id, id, id);
                    cc_sb_append_cstr(&out, &out_len, &out_cap, buf);
                    
                    /* Advance past the semicolon */
                    last_emit = semi;  /* Leave ; to be emitted */
                    while (i <= semi) {
                        if (src[i] == '\n') { scanner.line++; scanner.col = 1; }
                        else scanner.col++;
                        i++;
                    }
                    continue;
                }
                
                /* Find ']' (same line, best-effort) */
                size_t k = j + 1;
                while (k < n && src[k] != ']' && src[k] != '\n') k++;
                if (k >= n || src[k] != ']') {
                    char rel[1024];
                    cc_pp_error_cat(cc_path_rel_to_repo(input_path ? input_path : "<input>", rel, sizeof(rel)),
                            scanner.line, scanner.col, "channel", "unterminated channel handle type (missing ']')");
                    free(out);
                    return NULL;
                }

                int saw_gt = 0, saw_lt = 0, saw_ordered = 0;
                for (size_t t = j; t < k; t++) {
                    if (src[t] == '>') saw_gt = 1;
                    if (src[t] == '<') saw_lt = 1;
                    /* Check for 'ordered' keyword */
                    if (t + 7 <= k && memcmp(src + t, "ordered", 7) == 0) {
                        char before = (t == j) ? ' ' : src[t - 1];
                        char after = (t + 7 < k) ? src[t + 7] : ' ';
                        /* Inline ident check to avoid forward declaration */
                        int before_is_ident = (before == '_' || (before >= 'A' && before <= 'Z') || 
                                               (before >= 'a' && before <= 'z') || (before >= '0' && before <= '9'));
                        int after_is_ident = (after == '_' || (after >= 'A' && after <= 'Z') || 
                                              (after >= 'a' && after <= 'z') || (after >= '0' && after <= '9'));
                        if (!before_is_ident && !after_is_ident) {
                            saw_ordered = 1;
                        }
                    }
                }
                if (saw_gt && saw_lt) {
                    char rel[1024];
                    cc_pp_error_cat(cc_path_rel_to_repo(input_path ? input_path : "<input>", rel, sizeof(rel)),
                            scanner.line, scanner.col, "channel", "channel handle type cannot be both send ('>') and recv ('<')");
                    free(out);
                    return NULL;
                }
                if (!saw_gt && !saw_lt) {
                    char rel[1024];
                    cc_pp_error_cat(cc_path_rel_to_repo(input_path ? input_path : "<input>", rel, sizeof(rel)),
                            scanner.line, scanner.col, "channel", "channel handle type requires direction: use 'T[~ ... >]' or 'T[~ ... <]'");
                    free(out);
                    return NULL;
                }
                /* Validate: 'ordered' only allowed on rx channels */
                if (saw_ordered && saw_gt) {
                    char rel[1024];
                    cc_pp_error_cat(cc_path_rel_to_repo(input_path ? input_path : "<input>", rel, sizeof(rel)),
                            scanner.line, scanner.col, "channel", "'ordered' modifier only allowed on receive (<) channel");
                    free(out);
                    return NULL;
                }

                size_t ty_start = cc__scan_back_to_delim(src, i);
                if (ty_start < last_emit) {
                    /* overlapping/odd context; just ignore and continue */
                } else {
                    cc_sb_append(&out, &out_len, &out_cap, src + last_emit, ty_start - last_emit);
                    /* Emit CCChanTx for send, CCChanRx for recv (ordered is a flag, not a type) */
                    cc_sb_append_cstr(&out, &out_len, &out_cap, saw_gt ? "CCChanTx" : "CCChanRx");
                    last_emit = k + 1; /* skip past ']' */
                }

                /* advance to k+1 */
                while (i < k + 1) {
                    if (src[i] == '\n') { scanner.line++; scanner.col = 1; }
                    else scanner.col++;
                    i++;
                }
                continue;
            }
        }

        i++; scanner.col++;
    }

    if (last_emit < n) {
        cc_sb_append(&out, &out_len, &out_cap, src + last_emit, n - last_emit);
    }
    return out;
}

static int cc_is_ident_char_local(char c) {
    return (c == '_' || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'));
}

/* Scan backwards from pos to find the start of a member access chain (e.g., obj.field or ptr->field).
   Returns the start position of the full expression. Handles chains like a.b.c or a->b->c.
   Limitation: does not handle (expr).field, arr[i].field, or func().field patterns. */
static size_t cc_scan_back_for_member_access(const char* src, size_t pos, size_t limit) {
    if (pos == 0 || pos <= limit) return pos;
    
    size_t p = pos;
    /* Skip whitespace backwards */
    while (p > limit && (src[p-1] == ' ' || src[p-1] == '\t')) p--;
    
    /* Check for '.' or '->' before this position */
    int has_access = 0;
    if (p > limit && src[p-1] == '.') {
        has_access = 1;
        p--;
    } else if (p >= 2 + limit && src[p-1] == '>' && src[p-2] == '-') {
        has_access = 1;
        p -= 2;
    }
    
    if (!has_access) return pos;
    
    /* Scan back through identifier chain (a.b->c.d pattern) */
    while (p > limit && (src[p-1] == ' ' || src[p-1] == '\t')) p--;
    while (p > limit) {
        if (cc_is_ident_char(src[p-1])) {
            p--;
        } else if (src[p-1] == '.') {
            p--;
        } else if (p >= 2 + limit && src[p-1] == '>' && src[p-2] == '-') {
            p -= 2;
        } else {
            break;
        }
        while (p > limit && (src[p-1] == ' ' || src[p-1] == '\t')) p--;
    }
    return p;
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
/* Short name to CC-prefixed name mappings for stdlib types.
   The compiler resolves short names (e.g. "IoError") to their CC-prefixed
   forms (e.g. "CCIoError") in generated C code. */
static const struct { const char* short_name; const char* cc_name; } cc__preproc_type_aliases[] = {
    { "IoError",     "CCIoError" },
    { "IoErrorKind", "CCIoErrorKind" },
    { "Error",       "CCError" },
    { "ErrorKind",   "CCErrorKind" },
    { "NetError",    "CCNetError" },
    { "Arena",       "CCArena" },
    { "File",        "CCFile" },
    { "String",      "CCString" },
    { "Slice",       "CCSlice" },
    { NULL, NULL }
};

/* Normalize a mangled type name: map short aliases to CC-prefixed names */
static void cc__normalize_type_alias(char* name) {
    if (!name || !name[0]) return;
    for (int i = 0; cc__preproc_type_aliases[i].short_name; i++) {
        if (strcmp(name, cc__preproc_type_aliases[i].short_name) == 0) {
            /* Safe because CC names are always longer or equal */
            strcpy(name, cc__preproc_type_aliases[i].cc_name);
            return;
        }
    }
}

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
    
    /* Normalize short names to CC-prefixed names */
    cc__normalize_type_alias(out);
}

/* Collect optional types (T) for typedef generation. */
static char cc__optional_types[64][128];
static size_t cc__optional_type_count = 0;

static void cc__add_optional_type(const char* mangled) {
    /* Check for duplicates */
    for (size_t i = 0; i < cc__optional_type_count; i++) {
        if (strcmp(cc__optional_types[i], mangled) == 0) {
            return; /* Already have this type */
        }
    }
    if (cc__optional_type_count >= 64) {
        static int warned = 0;
        if (!warned) {
            fprintf(stderr, "warning: too many optional types (limit 64), some may be ignored\n");
            warned = 1;
        }
        return;
    }
    snprintf(cc__optional_types[cc__optional_type_count++], 128, "%s", mangled);
}

/* Check if an optional type is a "known" type that's pre-defined in parse stubs */
static int cc__is_known_optional_type(const char* mangled) {
    static const char* known[] = {
        "int", "bool", "char", "size_t", "voidptr", "charptr",
        "long", "short", "float", "double", "void", NULL
    };
    for (int i = 0; known[i]; i++) {
        if (strcmp(mangled, known[i]) == 0) return 1;
    }
    return 0;
}

/* Check if a token is a known C base type (for error detection) */
static int cc__is_known_base_type(const char* s, size_t len) {
    static const char* types[] = {
        "int", "char", "void", "bool", "float", "double",
        "long", "short", "unsigned", "signed", "size_t",
        "int8_t", "int16_t", "int32_t", "int64_t",
        "uint8_t", "uint16_t", "uint32_t", "uint64_t",
        "intptr_t", "uintptr_t", "ptrdiff_t", NULL
    };
    for (int i = 0; types[i]; i++) {
        if (strlen(types[i]) == len && strncmp(s, types[i], len) == 0) return 1;
    }
    return 0;
}

/* Rewrite optional types:
   - `T?` -> `CCOptional_T`
   The '?' must immediately follow a type name (no space).
   We detect: identifier? or )? or ]? or >? patterns. */
static char* cc__rewrite_optional_types(const char* src, size_t n, const char* input_path) {
    if (!src || n == 0) return NULL;
    
    /* Reset optional type tracking for each preprocessing call.
       This ensures inline typedefs are emitted fresh each time. */
    cc__optional_type_count = 0;
    
    char* out = NULL;
    size_t out_len = 0, out_cap = 0;
    
    size_t i = 0;
    size_t last_emit = 0;
    CCScannerState scan;
    cc_scanner_init(&scan);
    
    while (i < n) {
        /* Skip comments and strings using shared helper */
        if (cc_scanner_skip_non_code(&scan, src, n, &i)) continue;
        
        char c = src[i];
        char c2 = (i + 1 < n) ? src[i + 1] : 0;
        
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
                        
                        /* VALIDATION: Check if this looks like a variable, not a type.
                           Get just the identifier immediately before '?' (not the whole scanned type) */
                        size_t ident_end = i;
                        size_t ident_start = ident_end;
                        while (ident_start > 0 && cc_is_ident_char(src[ident_start - 1])) ident_start--;
                        size_t ident_len = ident_end - ident_start;
                        
                        /* Types typically: start uppercase, or are known types (int, char, etc.)
                           Variables typically: start lowercase, single word */
                        if (ident_len > 0 && ident_len < 32) {
                            char first_char = src[ident_start];
                            int looks_like_variable = (first_char >= 'a' && first_char <= 'z') &&
                                                      !cc__is_known_base_type(src + ident_start, ident_len);
                            
                            if (looks_like_variable) {
                                /* Emit helpful error */
                                char var_name[64];
                                size_t vlen = ident_len < sizeof(var_name) - 1 ? ident_len : sizeof(var_name) - 1;
                                memcpy(var_name, src + ident_start, vlen);
                                var_name[vlen] = '\0';
                                
                                char rel[1024];
                                cc_pp_error_cat(cc_path_rel_to_repo(input_path ? input_path : "<input>", rel, sizeof(rel)),
                                        scan.line, scan.col, "syntax",
                                        "'%s?' looks like optional unwrap of variable '%s'", var_name, var_name);
                                fprintf(stderr, "  hint: to unwrap an optional, use '*%s' instead of '%s?'\n", var_name, var_name);
                                fprintf(stderr, "  hint: '%s?' syntax is for declaring optional TYPES (e.g., 'int? maybe_value')\n", var_name);
                                free(out);
                                return NULL;
                            }
                        }
                        
                        char mangled[256];
                        cc__mangle_type_name(src + ty_start, ty_len, mangled, sizeof(mangled));
                        
                        if (mangled[0]) {
                            /* Check if we've already emitted this type */
                            int already_emitted = 0;
                            for (size_t ti = 0; ti < cc__optional_type_count; ti++) {
                                if (strcmp(cc__optional_types[ti], mangled) == 0) {
                                    already_emitted = 1;
                                    break;
                                }
                            }
                            
                            /* Collect for tracking */
                            cc__add_optional_type(mangled);
                            
                            /* Emit everything up to ty_start */
                            cc_sb_append(&out, &out_len, &out_cap, src + last_emit, ty_start - last_emit);
                            
                            /* For user types (not known types), emit inline struct definition on first use */
                            if (!already_emitted && !cc__is_known_optional_type(mangled)) {
                                /* Extract original type name (before mangling) */
                                char orig_type[256];
                                size_t orig_len = ty_len < sizeof(orig_type) - 1 ? ty_len : sizeof(orig_type) - 1;
                                memcpy(orig_type, src + ty_start, orig_len);
                                orig_type[orig_len] = '\0';
                                
                                /* Emit struct definition: typedef struct { int has; union { T value; } u; } CCOptional_T; */
                                cc_sb_append_cstr(&out, &out_len, &out_cap, "typedef struct { int has; union { ");
                                cc_sb_append_cstr(&out, &out_len, &out_cap, orig_type);
                                cc_sb_append_cstr(&out, &out_len, &out_cap, " value; } u; } CCOptional_");
                                cc_sb_append_cstr(&out, &out_len, &out_cap, mangled);
                                cc_sb_append_cstr(&out, &out_len, &out_cap, "; ");
                            }
                            
                            /* Emit CCOptional_T - type name */
                            cc_sb_append_cstr(&out, &out_len, &out_cap, "CCOptional_");
                            cc_sb_append_cstr(&out, &out_len, &out_cap, mangled);
                            last_emit = i + 1; /* skip past '?' */
                        }
                    }
                }
            }
        }
        
        i++;
    }
    
    if (last_emit < n) cc_sb_append(&out, &out_len, &out_cap, src + last_emit, n - last_emit);
    return out;
}

/* Rewrite optional constructors for parser mode:
   - `cc_some_CCOptional_T(v)` -> `__CC_OPTIONAL_SOME(T, v)`
   - `cc_none_CCOptional_T()` -> `__CC_OPTIONAL_NONE(T)`
   This allows custom types to work during TCC parsing. */
static char* cc__rewrite_optional_constructors(const char* src, size_t n) {
    if (!src || n == 0) return NULL;
    char* out = NULL;
    size_t out_len = 0, out_cap = 0;
    
    size_t i = 0;
    size_t last_emit = 0;
    CCScannerState scan;
    cc_scanner_init(&scan);
    
    while (i < n) {
        /* Skip comments and strings using shared helper */
        if (cc_scanner_skip_non_code(&scan, src, n, &i)) continue;
        
        /* Check for cc_some_CCOptional_ or cc_none_CCOptional_ patterns */
        const char* some_prefix = "cc_some_CCOptional_";
        const char* none_prefix = "cc_none_CCOptional_";
        size_t some_len = 19;  /* strlen("cc_some_CCOptional_") */
        size_t none_len = 19;  /* strlen("cc_none_CCOptional_") */
        
        int is_some = (i + some_len < n && strncmp(src + i, some_prefix, some_len) == 0);
        int is_none = (i + none_len < n && strncmp(src + i, none_prefix, none_len) == 0);
        
        if (is_some || is_none) {
            /* Make sure this isn't part of a longer identifier */
            if (i > 0 && cc_is_ident_char(src[i - 1])) {
                i++;
                continue;
            }
            
            size_t prefix_len = is_some ? some_len : none_len;
            size_t j = i + prefix_len;
            
            /* Extract the type name (identifier after prefix) */
            size_t type_start = j;
            while (j < n && cc_is_ident_char(src[j])) j++;
            size_t type_end = j;
            
            /* Skip whitespace to find '(' */
            while (j < n && (src[j] == ' ' || src[j] == '\t')) j++;
            if (j >= n || src[j] != '(') {
                i++;
                continue;
            }
            
            size_t paren_open = j;
            j++;  /* skip '(' */
            
            /* Find matching ')' */
            int paren_depth = 1;
            size_t arg_start = j;
            int in_s = 0, in_c = 0;
            while (j < n && paren_depth > 0) {
                char ch = src[j];
                if (in_s) { if (ch == '\\' && j + 1 < n) j++; else if (ch == '"') in_s = 0; j++; continue; }
                if (in_c) { if (ch == '\\' && j + 1 < n) j++; else if (ch == '\'') in_c = 0; j++; continue; }
                if (ch == '"') { in_s = 1; j++; continue; }
                if (ch == '\'') { in_c = 1; j++; continue; }
                if (ch == '(') paren_depth++;
                else if (ch == ')') paren_depth--;
                j++;
            }
            
            if (paren_depth != 0) {
                i++;
                continue;
            }
            
            size_t paren_close = j - 1;  /* position of ')' */
            size_t arg_end = paren_close;
            
            /* Extract type name */
            size_t type_len = type_end - type_start;
            if (type_len == 0 || type_len >= 256) {
                i++;
                continue;
            }
            
            char type_name[256];
            memcpy(type_name, src + type_start, type_len);
            type_name[type_len] = '\0';
            
            /* Emit everything up to this pattern */
            cc_sb_append(&out, &out_len, &out_cap, src + last_emit, i - last_emit);
            
            if (is_some) {
                /* cc_some_CCOptional_T(v) -> ((void)(v), (CCOptional_T){.has = 1})
                   Use comma expression that evaluates arg (for AST) and returns typed result */
                cc_sb_append_cstr(&out, &out_len, &out_cap, "((void)(");
                cc_sb_append(&out, &out_len, &out_cap, src + arg_start, arg_end - arg_start);
                cc_sb_append_cstr(&out, &out_len, &out_cap, "), (CCOptional_");
                cc_sb_append_cstr(&out, &out_len, &out_cap, type_name);
                cc_sb_append_cstr(&out, &out_len, &out_cap, "){.has = 1})");
            } else {
                /* cc_none_CCOptional_T() -> (CCOptional_T){.has = 0} */
                cc_sb_append_cstr(&out, &out_len, &out_cap, "(CCOptional_");
                cc_sb_append_cstr(&out, &out_len, &out_cap, type_name);
                cc_sb_append_cstr(&out, &out_len, &out_cap, "){.has = 0}");
            }
            
            last_emit = j;  /* skip past ')' */
            i = j;
            continue;
        }
        
        i++;
    }
    
    if (last_emit == 0) return NULL;  /* No rewrites done */
    if (last_emit < n) cc_sb_append(&out, &out_len, &out_cap, src + last_emit, n - last_emit);
    return out;
}

/* Rewrite result constructors for parser mode:
   - `cc_ok_CCResult_T_E(v)` -> `__CC_RESULT_OK(0, 0, v)`
   - `cc_err_CCResult_T_E(e)` -> `__CC_RESULT_ERR(0, 0, e)`
   This avoids typed constructors during TCC parsing while preserving arg checking. */
static char* cc__rewrite_result_constructors(const char* src, size_t n) {
    if (!src || n == 0) return NULL;
    char* out = NULL;
    size_t out_len = 0, out_cap = 0;
    
    size_t i = 0;
    size_t last_emit = 0;
    CCScannerState scan;
    cc_scanner_init(&scan);
    
    while (i < n) {
        /* Skip comments and strings using shared helper */
        if (cc_scanner_skip_non_code(&scan, src, n, &i)) continue;
        
        const char* ok_prefix = "cc_ok_CCResult_";
        const char* err_prefix = "cc_err_CCResult_";
        size_t ok_len = 15;  /* strlen("cc_ok_CCResult_") */
        size_t err_len = 16; /* strlen("cc_err_CCResult_") */
        
        int is_ok = (i + ok_len < n && strncmp(src + i, ok_prefix, ok_len) == 0);
        int is_err = (i + err_len < n && strncmp(src + i, err_prefix, err_len) == 0);
        
        if (is_ok || is_err) {
            /* Ensure not part of a longer identifier */
            if (i > 0 && cc_is_ident_char(src[i - 1])) {
                i++;
                continue;
            }
            
            size_t prefix_len = is_ok ? ok_len : err_len;
            size_t j = i + prefix_len;
            
            /* Skip mangled type name */
            while (j < n && cc_is_ident_char(src[j])) j++;
            
            /* Skip whitespace to find '(' */
            while (j < n && (src[j] == ' ' || src[j] == '\t')) j++;
            if (j >= n || src[j] != '(') {
                i++;
                continue;
            }
            
            /* Parse argument list */
            size_t args_start = j + 1;
            size_t k = args_start;
            int depth = 1;
            int in_s = 0, in_c = 0;
            while (k < n && depth > 0) {
                char ch = src[k];
                if (in_s) { if (ch == '\\' && k + 1 < n) k++; else if (ch == '"') in_s = 0; k++; continue; }
                if (in_c) { if (ch == '\\' && k + 1 < n) k++; else if (ch == '\'') in_c = 0; k++; continue; }
                if (ch == '"') { in_s = 1; k++; continue; }
                if (ch == '\'') { in_c = 1; k++; continue; }
                if (ch == '(') depth++;
                else if (ch == ')') depth--;
                if (depth > 0) k++;
            }
            if (depth != 0) {
                i++;
                continue;
            }
            
            size_t args_end = k;
            size_t call_end = k + 1;
            
            /* Emit everything up to function name.
               Use comma expression: ((void)(arg), (__CCResultGeneric){.ok=1/0})
               This evaluates the arg (so it appears in TCC's AST for codegen to find)
               while returning the generic stub type. Visitor pass will rewrite output. */
            cc_sb_append(&out, &out_len, &out_cap, src + last_emit, i - last_emit);
            cc_sb_append_cstr(&out, &out_len, &out_cap, "((void)(");
            cc_sb_append(&out, &out_len, &out_cap, src + args_start, args_end - args_start);
            if (is_ok) {
                cc_sb_append_cstr(&out, &out_len, &out_cap, "), ((__CCResultGeneric){.ok = 1}))");
            } else {
                cc_sb_append_cstr(&out, &out_len, &out_cap, "), ((__CCResultGeneric){.ok = 0}))");
            }
            last_emit = call_end;
            i = call_end;
            continue;
        }
        
        i++;
    }
    
    if (last_emit < n) cc_sb_append(&out, &out_len, &out_cap, src + last_emit, n - last_emit);
    return out;
}

/* ============================================================================
 * Generic container syntax lowering: Vec<T> -> Vec_T, Map<K,V> -> Map_K_V
 * Also: vec_new<T>(...) -> Vec_T_init(...), map_new<K,V>(...) -> Map_K_V_init(...)
 * ============================================================================ */

/* Mangle a type parameter for container names (int -> int, char[:] -> charslice, etc.) */
static void cc__mangle_container_type_param(const char* src, size_t len, char* out, size_t out_sz) {
    if (!src || len == 0 || !out || out_sz == 0) { if (out && out_sz > 0) out[0] = 0; return; }
    /* Trim whitespace */
    size_t i = 0;
    while (i < len && (src[i] == ' ' || src[i] == '\t' || src[i] == '\n' || src[i] == '\r')) i++;
    size_t e = len;
    while (e > i && (src[e - 1] == ' ' || src[e - 1] == '\t' || src[e - 1] == '\n' || src[e - 1] == '\r')) e--;
    size_t j = 0;
    for (size_t k = i; k < e && j < out_sz - 1; k++) {
        char c = src[k];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') continue;
        if (c == '*') { 
            if (j + 3 < out_sz) { out[j++] = 'p'; out[j++] = 't'; out[j++] = 'r'; }
        } else if (c == '[') {
            if (j + 5 < out_sz && k + 2 < e && src[k + 1] == ':' && src[k + 2] == ']') {
                out[j++] = 's'; out[j++] = 'l'; out[j++] = 'i'; out[j++] = 'c'; out[j++] = 'e';
                k += 2;
            } else {
                out[j++] = '_';
            }
        } else if (c == ']' || c == ',' || c == '<' || c == '>') {
            out[j++] = '_';
        } else {
            out[j++] = c;
        }
    }
    while (j > 0 && out[j - 1] == '_') j--;
    out[j] = 0;
}

/* Find matching '>' for '<' at position langle. Returns 1 on success. */
static int cc__find_matching_angle(const char* b, size_t bl, size_t langle, size_t* out_rangle) {
    if (!b || langle >= bl || b[langle] != '<') return 0;
    int ang = 1, par = 0, brk = 0, br = 0;
    int ins = 0; char q = 0;
    int in_lc = 0, in_bc = 0;
    for (size_t p = langle + 1; p < bl; p++) {
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
        else if (ch == '}') { if (br) br--; }
        else if (ch == '<' && par == 0 && brk == 0 && br == 0) ang++;
        else if (ch == '>' && par == 0 && brk == 0 && br == 0) {
            ang--;
            if (ang == 0) { if (out_rangle) *out_rangle = p; return 1; }
        }
    }
    return 0;
}

/* Rewrite generic container syntax:
   - Vec<T> -> __CC_VEC(T_mangled)  (parser-safe macro)
   - Map<K, V> -> __CC_MAP(K_mangled, V_mangled)*  (parser-safe macro)
   - vec_new<T>(...) -> __CC_VEC_INIT(T_mangled, ...)
   - map_new<K, V>(...) -> __CC_MAP_INIT(K_mangled, V_mangled, ...)
   Also tracks variable declarations for UFCS resolution. */
char* cc_rewrite_generic_containers(const char* src, size_t n, const char* input_path) {
    if (!src || n == 0) return NULL;
    
    char* out = NULL;
    size_t out_len = 0, out_cap = 0;
    size_t i = 0;
    size_t last_emit = 0;
    CCScannerState scanner;
    cc_scanner_init(&scanner);
    
    CCTypeRegistry* reg = cc_type_registry_get_global();
    
    while (i < n) {
        /* Skip comments and strings using shared helper */
        if (cc_scanner_skip_non_code(&scanner, src, n, &i)) continue;
        
        /* Look for Vec< or Map< or vec_new< or map_new< */
        int is_vec_type = 0, is_map_type = 0, is_vec_new = 0, is_map_new = 0;
        size_t kw_start = i;
        size_t kw_len = 0;
        
        if (i + 4 <= n && memcmp(src + i, "Vec<", 4) == 0) {
            /* Check word boundary before */
            if (i == 0 || !cc_is_ident_char(src[i - 1])) {
                is_vec_type = 1;
                kw_len = 3;
            }
        } else if (i + 4 <= n && memcmp(src + i, "Map<", 4) == 0) {
            if (i == 0 || !cc_is_ident_char(src[i - 1])) {
                is_map_type = 1;
                kw_len = 3;
            }
        } else if (i + 8 <= n && memcmp(src + i, "vec_new<", 8) == 0) {
            if (i == 0 || !cc_is_ident_char(src[i - 1])) {
                is_vec_new = 1;
                kw_len = 7;
            }
        } else if (i + 8 <= n && memcmp(src + i, "map_new<", 8) == 0) {
            if (i == 0 || !cc_is_ident_char(src[i - 1])) {
                is_map_new = 1;
                kw_len = 7;
            }
        }
        
        if (is_vec_type || is_map_type || is_vec_new || is_map_new) {
            size_t angle_start = kw_start + kw_len;
            size_t angle_end = 0;
            
            if (!cc__find_matching_angle(src, n, angle_start, &angle_end)) {
                /* ERROR: unclosed generic type */
                const char* what = is_vec_type ? "Vec<" : is_map_type ? "Map<" : is_vec_new ? "vec_new<" : "map_new<";
                char rel[1024];
                cc_pp_error_cat(cc_path_rel_to_repo(input_path ? input_path : "<input>", rel, sizeof(rel)),
                        scanner.line, scanner.col, "type", "unclosed '%s' - missing '>'", what);
                fprintf(stderr, "  hint: generic syntax is '%sType>' e.g., 'Vec<int>'\n", what);
                free(out);
                return NULL;
            }
            
            /* Extract type parameters */
            const char* params = src + angle_start + 1;
            size_t params_len = angle_end - angle_start - 1;
            
            char mangled[256] = {0};
            char elem_type[128] = {0};
            char key_type[128] = {0};
            char val_type[128] = {0};
            char orig_elem_type[128] = {0};  /* Original type for macro emission */
            char orig_key_type[128] = {0};
            char orig_val_type[128] = {0};
            
            if (is_vec_type || is_vec_new) {
                /* Single type parameter - save both original and mangled */
                /* Original (trimmed): copy params as-is for macro */
                size_t ti = 0;
                while (ti < params_len && (params[ti] == ' ' || params[ti] == '\t')) ti++;
                size_t te = params_len;
                while (te > ti && (params[te-1] == ' ' || params[te-1] == '\t')) te--;
                size_t orig_len = te - ti;
                if (orig_len >= sizeof(orig_elem_type)) orig_len = sizeof(orig_elem_type) - 1;
                memcpy(orig_elem_type, params + ti, orig_len);
                orig_elem_type[orig_len] = 0;
                
                cc__mangle_container_type_param(params, params_len, elem_type, sizeof(elem_type));
                snprintf(mangled, sizeof(mangled), "Vec_%s", elem_type);
                
                if (reg) {
                    cc_type_registry_add_vec(reg, orig_elem_type, mangled);
                }
            } else {
                /* Two type parameters: K, V */
                const char* comma = NULL;
                int depth = 0;
                for (size_t k = 0; k < params_len; k++) {
                    char pc = params[k];
                    if (pc == '<') depth++;
                    else if (pc == '>') depth--;
                    else if (pc == ',' && depth == 0) {
                        comma = params + k;
                        break;
                    }
                }
                
                if (!comma) {
                    i++;
                    continue;
                }
                
                size_t k_len = (size_t)(comma - params);
                size_t v_start = (size_t)(comma - params) + 1;
                size_t v_len = params_len - v_start;
                
                /* Save original key type (trimmed) */
                {
                    size_t ti = 0;
                    while (ti < k_len && (params[ti] == ' ' || params[ti] == '\t')) ti++;
                    size_t te = k_len;
                    while (te > ti && (params[te-1] == ' ' || params[te-1] == '\t')) te--;
                    size_t orig_len = te - ti;
                    if (orig_len >= sizeof(orig_key_type)) orig_len = sizeof(orig_key_type) - 1;
                    memcpy(orig_key_type, params + ti, orig_len);
                    orig_key_type[orig_len] = 0;
                }
                
                /* Save original val type (trimmed) */
                {
                    const char* vparams = params + v_start;
                    size_t ti = 0;
                    while (ti < v_len && (vparams[ti] == ' ' || vparams[ti] == '\t')) ti++;
                    size_t te = v_len;
                    while (te > ti && (vparams[te-1] == ' ' || vparams[te-1] == '\t')) te--;
                    size_t orig_len = te - ti;
                    if (orig_len >= sizeof(orig_val_type)) orig_len = sizeof(orig_val_type) - 1;
                    memcpy(orig_val_type, vparams + ti, orig_len);
                    orig_val_type[orig_len] = 0;
                }
                
                cc__mangle_container_type_param(params, k_len, key_type, sizeof(key_type));
                cc__mangle_container_type_param(params + v_start, v_len, val_type, sizeof(val_type));
                snprintf(mangled, sizeof(mangled), "Map_%s_%s", key_type, val_type);
                
                if (reg) {
                    cc_type_registry_add_map(reg, orig_key_type, orig_val_type, mangled);
                }
            }
            
            /* Emit everything up to this point */
            cc_sb_append(&out, &out_len, &out_cap, src + last_emit, kw_start - last_emit);
            
            if (is_vec_type || is_map_type) {
                /* Emit parser-safe macro call instead of bare type name.
                   In parser mode: __CC_VEC(T) -> __CCVecGeneric
                   In real mode:   __CC_VEC(T) -> Vec_T */
                if (is_vec_type) {
                    cc_sb_append_cstr(&out, &out_len, &out_cap, "__CC_VEC(");
                    cc_sb_append_cstr(&out, &out_len, &out_cap, elem_type);
                    cc_sb_append_cstr(&out, &out_len, &out_cap, ")");
                } else {
                    /* Map uses pointers (khashl returns ptr from init) */
                    cc_sb_append_cstr(&out, &out_len, &out_cap, "__CC_MAP(");
                    cc_sb_append_cstr(&out, &out_len, &out_cap, key_type);
                    cc_sb_append_cstr(&out, &out_len, &out_cap, ", ");
                    cc_sb_append_cstr(&out, &out_len, &out_cap, val_type);
                    cc_sb_append_cstr(&out, &out_len, &out_cap, ")*");
                }
                last_emit = angle_end + 1;
                
                /* Try to extract variable name for type registry */
                if (reg) {
                    size_t j = cc_skip_ws_and_comments(src, n, angle_end + 1);
                    if (j < n && (cc_is_ident_start(src[j]) || src[j] == '*')) {
                        /* Skip any * for pointer types */
                        while (j < n && src[j] == '*') j++;
                        j = cc_skip_ws_and_comments(src, n, j);
                        if (j < n && cc_is_ident_start(src[j])) {
                            size_t var_start = j;
                            while (j < n && cc_is_ident_char(src[j])) j++;
                            char var_name[128];
                            size_t vn_len = j - var_start;
                            if (vn_len < sizeof(var_name)) {
                                memcpy(var_name, src + var_start, vn_len);
                                var_name[vn_len] = 0;
                                cc_type_registry_add_var(reg, var_name, mangled);
                            }
                        }
                    }
                }
            } else {
                /* vec_new<T>(...) or map_new<K,V>(...) -> __CC_VEC_INIT/__CC_MAP_INIT macro */
                /* Find the opening paren and extract args */
                size_t j = cc_skip_ws_and_comments(src, n, angle_end + 1);
                if (j < n && src[j] == '(') {
                    /* Find the closing paren */
                    size_t paren_end = 0;
                    if (cc_find_matching_paren(src, n, j, &paren_end)) {
                        /* Emit macro call with type param first, then original args */
                        if (is_vec_new) {
                            cc_sb_append_cstr(&out, &out_len, &out_cap, "__CC_VEC_INIT(");
                            cc_sb_append_cstr(&out, &out_len, &out_cap, elem_type);
                            cc_sb_append_cstr(&out, &out_len, &out_cap, ", ");
                            /* Emit the arena argument(s) */
                            cc_sb_append(&out, &out_len, &out_cap, src + j + 1, paren_end - j - 1);
                            cc_sb_append_cstr(&out, &out_len, &out_cap, ")");
                        } else {
                            cc_sb_append_cstr(&out, &out_len, &out_cap, "__CC_MAP_INIT(");
                            cc_sb_append_cstr(&out, &out_len, &out_cap, key_type);
                            cc_sb_append_cstr(&out, &out_len, &out_cap, ", ");
                            cc_sb_append_cstr(&out, &out_len, &out_cap, val_type);
                            cc_sb_append_cstr(&out, &out_len, &out_cap, ", ");
                            /* Emit the arena argument */
                            cc_sb_append(&out, &out_len, &out_cap, src + j + 1, paren_end - j - 1);
                            cc_sb_append_cstr(&out, &out_len, &out_cap, ")");
                        }
                        last_emit = paren_end + 1;
                        i = paren_end + 1;
                        continue;
                    }
                }
                /* Fallback if no paren found */
                if (is_vec_new) {
                    cc_sb_append_cstr(&out, &out_len, &out_cap, "__CC_VEC_INIT(");
                    cc_sb_append_cstr(&out, &out_len, &out_cap, elem_type);
                    cc_sb_append_cstr(&out, &out_len, &out_cap, ", NULL)");
                } else {
                    cc_sb_append_cstr(&out, &out_len, &out_cap, "__CC_MAP_INIT(");
                    cc_sb_append_cstr(&out, &out_len, &out_cap, key_type);
                    cc_sb_append_cstr(&out, &out_len, &out_cap, ", ");
                    cc_sb_append_cstr(&out, &out_len, &out_cap, val_type);
                    cc_sb_append_cstr(&out, &out_len, &out_cap, ", NULL)");
                }
                last_emit = angle_end + 1;
            }
            
            i = angle_end + 1;
            continue;
        }
        
        i++; scanner.col++;
    }
    
    if (last_emit < n) cc_sb_append(&out, &out_len, &out_cap, src + last_emit, n - last_emit);
    return out;
}

/* Generic UFCS rewrite for typed variables:
   v.push(x)    -> Vec_int_push(&v, x)      (Vec methods take pointer)
   m.get(k)     -> Map_K_V_get(m, k)        (Map by value)
   r.unwrap()   -> CCResult_T_E_unwrap(r)   (Result by value)
   r.is_ok()    -> CCResult_T_E_is_ok(r)
   
   Any type registered in the type registry gets UFCS support.
   The type registry is populated during type lowering passes
   (Vec<T>, Map<K,V>, T!E, T?, etc.). */
char* cc_rewrite_ufcs_container_calls(const char* src, size_t n, const char* input_path) {
    (void)input_path;
    if (!src || n == 0) return NULL;
    
    CCTypeRegistry* reg = cc_type_registry_get_global();
    if (!reg) return NULL;
    
    char* out = NULL;
    size_t out_len = 0, out_cap = 0;
    size_t i = 0;
    size_t last_emit = 0;
    CCScannerState scanner;
    cc_scanner_init(&scanner);
    
    while (i < n) {
        /* Skip comments and strings using shared helper */
        if (cc_scanner_skip_non_code(&scanner, src, n, &i)) continue;
        
        char c = src[i];
        /* Look for identifier followed by '.' or '->' - potential UFCS call */
        if (cc_is_ident_start(c)) {
            size_t ident_start = i;
            while (i < n && cc_is_ident_char(src[i])) i++;
            size_t ident_end = i;
            size_t ident_len = ident_end - ident_start;
            
            /* Check for '.' or '->' after identifier (skip whitespace) */
            size_t j = cc_skip_ws_and_comments(src, n, ident_end);
            int is_arrow = 0;
            if (j < n && src[j] == '.') {
                j = cc_skip_ws_and_comments(src, n, j + 1);
            } else if (j + 1 < n && src[j] == '-' && src[j + 1] == '>') {
                is_arrow = 1;
                j = cc_skip_ws_and_comments(src, n, j + 2);
            } else {
                /* Not a UFCS pattern, continue */
                continue;
            }
            
            /* Found 'ident.' or 'ident->' - check if it's a method call */
            /* Look for method name */
            if (j < n && cc_is_ident_start(src[j])) {
                size_t method_start = j;
                while (j < n && cc_is_ident_char(src[j])) j++;
                size_t method_end = j;
                size_t method_len = method_end - method_start;
                
                /* Check for '(' after method name */
                size_t k = cc_skip_ws_and_comments(src, n, method_end);
                if (k < n && src[k] == '(') {
                    /* Extract identifier name and look up in registry */
                    char var_name[128];
                    if (ident_len < sizeof(var_name)) {
                        memcpy(var_name, src + ident_start, ident_len);
                        var_name[ident_len] = 0;
                        
                        const char* type_name = cc_type_registry_lookup_var(reg, var_name);
                        if (type_name) {
                            /* Generic UFCS: rewrite var.method(...) to Type_method(...).
                               Receiver passing convention based on type:
                               - Vec with . : pass &var (needs pointer)
                               - All others (Map, Result, etc.): pass by value or as-is
                               - Pointer receivers (->): pass as-is
                               Handle chained access like obj.field or ptr->field by scanning back.
                               Limitation: doesn't handle (expr).field, arr[i].field, or func().field. */
                            int is_vec = strncmp(type_name, "Vec_", 4) == 0;
                            size_t recv_start = cc_scan_back_for_member_access(src, ident_start, last_emit);
                            
                            char full_recv[256];
                            size_t recv_len = ident_end - recv_start;
                            if (recv_len >= sizeof(full_recv)) recv_len = sizeof(full_recv) - 1;
                            memcpy(full_recv, src + recv_start, recv_len);
                            full_recv[recv_len] = 0;
                            
                            char method_name[64];
                            if (method_len < sizeof(method_name)) {
                                memcpy(method_name, src + method_start, method_len);
                                method_name[method_len] = 0;
                                
                                /* Find matching close paren */
                                size_t paren_end = 0;
                                if (cc_find_matching_paren(src, n, k, &paren_end)) {
                                    /* Emit everything up to recv_start (not ident_start) */
                                    cc_sb_append(&out, &out_len, &out_cap, src + last_emit, recv_start - last_emit);
                                    
                                    /* Emit: Type_method(...) with appropriate receiver handling */
                                    cc_sb_append_cstr(&out, &out_len, &out_cap, type_name);
                                    cc_sb_append_cstr(&out, &out_len, &out_cap, "_");
                                    cc_sb_append_cstr(&out, &out_len, &out_cap, method_name);
                                    
                                    /* Receiver handling:
                                       - Vec with . needs & (methods take pointer)
                                       - Everything else: pass by value (arrow already pointer) */
                                    if (is_vec && !is_arrow) {
                                        cc_sb_append_cstr(&out, &out_len, &out_cap, "(&");
                                    } else {
                                        cc_sb_append_cstr(&out, &out_len, &out_cap, "(");
                                    }
                                    cc_sb_append_cstr(&out, &out_len, &out_cap, full_recv);
                                    
                                    /* Extract args (content between parens) */
                                    size_t args_start = k + 1;
                                    size_t args_end = paren_end;
                                    size_t args_len = args_end - args_start;
                                    
                                    /* Skip leading whitespace in args */
                                    while (args_start < args_end && (src[args_start] == ' ' || src[args_start] == '\t' || src[args_start] == '\n')) {
                                        args_start++;
                                        args_len--;
                                    }
                                    
                                    if (args_len > 0) {
                                        cc_sb_append_cstr(&out, &out_len, &out_cap, ", ");
                                        cc_sb_append(&out, &out_len, &out_cap, src + args_start, args_len);
                                    }
                                    cc_sb_append_cstr(&out, &out_len, &out_cap, ")");
                                    
                                    last_emit = paren_end + 1;
                                    i = paren_end + 1;
                                    continue;
                                }
                            }
                        }
                    }
                }
            }
            /* Not a UFCS call, continue from after identifier */
            continue;
        }
        
        i++;
    }
    
    if (last_emit == 0) {
        /* No changes made */
        return NULL;
    }
    
    if (last_emit < n) cc_sb_append(&out, &out_len, &out_cap, src + last_emit, n - last_emit);
    return out;
}

/* Rewrite cc_std_out.write() and cc_std_err.write() UFCS patterns.
   These are synthetic receivers (not real variables), so need special handling.
   
   Rewrites:
   - cc_std_out.write("literal") -> cc_std_out_write(cc_slice_from_buffer("literal", sizeof("literal") - 1))
   - cc_std_out.write(s) -> cc_std_out_write_string(&s) (for CCString variables)
   - cc_std_out.write(expr) -> cc_std_out_write(expr) (for other expressions)
   - cc_std_err.write(...) similarly
*/
char* cc_rewrite_std_io_ufcs(const char* src, size_t n) {
    if (!src || n == 0) return NULL;
    
    char* out = NULL;
    size_t out_len = 0, out_cap = 0;
    size_t i = 0;
    size_t last_emit = 0;
    CCScannerState scanner;
    cc_scanner_init(&scanner);
    
    while (i < n) {
        /* Skip comments and strings using shared helper */
        if (cc_scanner_skip_non_code(&scanner, src, n, &i)) continue;
        
        /* Look for cc_std_out or cc_std_err */
        if (i + 10 <= n && (strncmp(src + i, "cc_std_out", 10) == 0 || strncmp(src + i, "cc_std_err", 10) == 0)) {
            int is_err = (src[i + 7] == 'e');  /* cc_std_err vs cc_std_out */
            size_t kw_start = i;
            i += 10;
            
            /* Check for .write( pattern */
            size_t j = cc_skip_ws_and_comments(src, n, i);
            if (j < n && src[j] == '.') {
                j = cc_skip_ws_and_comments(src, n, j + 1);
                if (j + 5 <= n && strncmp(src + j, "write", 5) == 0) {
                    j += 5;
                    size_t k = cc_skip_ws_and_comments(src, n, j);
                    if (k < n && src[k] == '(') {
                        /* Found cc_std_out.write( or cc_std_err.write( */
                        size_t paren_end = 0;
                        if (cc_find_matching_paren(src, n, k, &paren_end)) {
                            /* Extract argument */
                            size_t arg_start = k + 1;
                            size_t arg_end = paren_end;
                            
                            /* Skip whitespace in arg */
                            while (arg_start < arg_end && (src[arg_start] == ' ' || src[arg_start] == '\t' || src[arg_start] == '\n'))
                                arg_start++;
                            while (arg_end > arg_start && (src[arg_end - 1] == ' ' || src[arg_end - 1] == '\t' || src[arg_end - 1] == '\n'))
                                arg_end--;
                            
                            size_t arg_len = arg_end - arg_start;
                            
                            /* Check for nested UFCS BEFORE emitting anything */
                            int has_ufcs = 0;
                            if (arg_len > 0 && src[arg_start] != '"') {
                                for (size_t x = arg_start; x < arg_end; x++) {
                                    char ac = src[x];
                                    if (ac == '.' || (ac == '-' && x + 1 < arg_end && src[x + 1] == '>')) {
                                        has_ufcs = 1;
                                        break;
                                    }
                                }
                            }
                            
                            if (has_ufcs) {
                                /* Nested UFCS in argument (e.g., cc_std_out.write(s.as_slice())).
                                   Rewrite the inner UFCS first, then wrap with cc_std_out_write.
                                   We handle common patterns like ident.as_slice() here. */
                                
                                /* Extract the argument as a string for analysis */
                                char arg_buf[512];
                                if (arg_len < sizeof(arg_buf)) {
                                    memcpy(arg_buf, src + arg_start, arg_len);
                                    arg_buf[arg_len] = 0;
                                    
                                    /* Normalize: trim all whitespace to find the pattern */
                                    char normalized[512];
                                    char* np = normalized;
                                    int in_ws = 0;
                                    for (char* p = arg_buf; *p; p++) {
                                        if (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') {
                                            in_ws = 1;
                                        } else {
                                            if (in_ws && np > normalized) *np++ = ' ';
                                            in_ws = 0;
                                            *np++ = *p;
                                        }
                                    }
                                    *np = 0;
                                    
                                    /* Check for ident.as_slice() pattern */
                                    char* dot = strchr(normalized, '.');
                                    if (dot && strcmp(dot, ".as_slice()") == 0) {
                                        /* Extract receiver identifier */
                                        *dot = 0;
                                        char* recv = normalized;
                                        
                                        /* Check if receiver is a simple identifier */
                                        int recv_is_ident = (*recv != 0);
                                        for (char* p = recv; *p && recv_is_ident; p++) {
                                            if (!cc_is_ident_char(*p)) recv_is_ident = 0;
                                        }
                                        
                                        if (recv_is_ident) {
                                            /* Emit: cc_std_out_write(cc_string_as_slice(&recv)) */
                                            cc_sb_append(&out, &out_len, &out_cap, src + last_emit, kw_start - last_emit);
                                            cc_sb_append_cstr(&out, &out_len, &out_cap, is_err ? "cc_std_err_write(cc_string_as_slice(&" : "cc_std_out_write(cc_string_as_slice(&");
                                            cc_sb_append_cstr(&out, &out_len, &out_cap, recv);
                                            cc_sb_append_cstr(&out, &out_len, &out_cap, "))");
                                            
                                            last_emit = paren_end + 1;
                                            i = paren_end + 1;
                                            continue;
                                        }
                                    }
                                }
                                
                                /* Fallback: skip and let TCC/visitor handle it */
                                i = kw_start + 10;
                                continue;
                            }
                            
                            /* Emit everything up to cc_std_out/cc_std_err */
                            cc_sb_append(&out, &out_len, &out_cap, src + last_emit, kw_start - last_emit);
                            
                            /* Determine what kind of argument we have */
                            if (arg_len > 0 && src[arg_start] == '"') {
                                /* String literal: cc_std_out_write(cc_slice_from_buffer("lit", sizeof("lit") - 1)) */
                                cc_sb_append_cstr(&out, &out_len, &out_cap, is_err ? "cc_std_err_write(cc_slice_from_buffer(" : "cc_std_out_write(cc_slice_from_buffer(");
                                cc_sb_append(&out, &out_len, &out_cap, src + arg_start, arg_len);
                                cc_sb_append_cstr(&out, &out_len, &out_cap, ", sizeof(");
                                cc_sb_append(&out, &out_len, &out_cap, src + arg_start, arg_len);
                                cc_sb_append_cstr(&out, &out_len, &out_cap, ") - 1))");
                            } else if (arg_len > 0) {
                                /* Check if it's a simple identifier */
                                int is_simple_ident = 1;
                                for (size_t x = arg_start; x < arg_end; x++) {
                                    char ac = src[x];
                                    if (!cc_is_ident_char(ac) && ac != ' ' && ac != '\t' && ac != '\n') {
                                        is_simple_ident = 0;
                                        break;
                                    }
                                }
                                
                                if (is_simple_ident) {
                                    /* Simple identifier - assume it's a String, use _string variant */
                                    cc_sb_append_cstr(&out, &out_len, &out_cap, is_err ? "cc_std_err_write_string(&" : "cc_std_out_write_string(&");
                                    cc_sb_append(&out, &out_len, &out_cap, src + arg_start, arg_len);
                                    cc_sb_append_cstr(&out, &out_len, &out_cap, ")");
                                } else {
                                    /* Complex expression without nested UFCS - wrap with cc_std_out_write */
                                    cc_sb_append_cstr(&out, &out_len, &out_cap, is_err ? "cc_std_err_write(" : "cc_std_out_write(");
                                    cc_sb_append(&out, &out_len, &out_cap, src + arg_start, arg_len);
                                    cc_sb_append_cstr(&out, &out_len, &out_cap, ")");
                                }
                            } else {
                                /* Empty args - just emit cc_std_out_write() */
                                cc_sb_append_cstr(&out, &out_len, &out_cap, is_err ? "cc_std_err_write()" : "cc_std_out_write()");
                            }
                            
                            last_emit = paren_end + 1;
                            i = paren_end + 1;
                            continue;
                        }
                    }
                }
            }
            /* Not a match, continue from after cc_std_out/cc_std_err */
            continue;
        }
        
        i++;
    }
    
    if (last_emit == 0) {
        return NULL;
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
    if (cc__result_type_count >= sizeof(cc__result_types)/sizeof(cc__result_types[0])) {
        static int warned = 0;
        if (!warned) {
            fprintf(stderr, "warning: too many result types (limit 64), some may be ignored\n");
            warned = 1;
        }
        return;
    }
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
   - `T!>(E)` -> `__CC_RESULT(T_mangled, E_mangled)`
   The '!>' sigil is followed by error type in parentheses.
   This syntax is unambiguous and easy to parse.
   Also collects unique (T, E) pairs for later emission of CC_DECL_RESULT_SPEC calls.
   
   NOTE: Do NOT reset cc__result_type_count here - cc__rewrite_inferred_result_ctors
   may have already registered types from function signatures, and we must keep those. */
static char* cc__rewrite_result_types(const char* src, size_t n, const char* input_path) {
    if (!src || n == 0) return NULL;
    char* out = NULL;
    size_t out_len = 0, out_cap = 0;
    
    size_t i = 0;
    size_t last_emit = 0;
    CCScannerState scan;
    cc_scanner_init(&scan);
    
    while (i < n) {
        /* Skip comments and strings using shared helper */
        if (cc_scanner_skip_non_code(&scan, src, n, &i)) continue;
        
        char c = src[i];
        char c2 = (i + 1 < n) ? src[i + 1] : 0;
        
        /* Detect `T!>(E)` pattern: type followed by '!>' followed by '(' error type ')' */
        if (c == '!' && c2 == '>') {
            /* Found '!>' sigil - now find the error type in parentheses */
            size_t sigil_pos = i;
            size_t j = i + 2;  /* skip '!>' */
            
            /* Skip whitespace */
            while (j < n && (src[j] == ' ' || src[j] == '\t' || src[j] == '\n' || src[j] == '\r')) j++;
            
            /* Must find '(' - VALIDATE */
            if (j >= n || src[j] != '(') {
                /* ERROR: !> without error type */
                char rel[1024];
                cc_pp_error_cat(cc_path_rel_to_repo(input_path ? input_path : "<input>", rel, sizeof(rel)),
                        scan.line, scan.col, "type", "'!>' requires error type in parentheses");
                fprintf(stderr, "  hint: use 'T !> (ErrorType)' syntax, e.g., 'int !> (Error)'\n");
                fprintf(stderr, "  hint: for simple error results, use 'int !> (int)' for error codes\n");
                free(out);
                return NULL;
            }
            if (j < n && src[j] == '(') {
                size_t paren_open = j;
                j++;  /* skip '(' */
                
                /* Skip whitespace inside parens */
                while (j < n && (src[j] == ' ' || src[j] == '\t' || src[j] == '\n' || src[j] == '\r')) j++;
                
                /* Find matching ')' - track nesting for complex types like Error<A, B> */
                size_t err_start = j;
                int paren_depth = 1;
                int in_s = 0, in_c = 0;
                while (j < n && paren_depth > 0) {
                    char ch = src[j];
                    if (in_s) { if (ch == '\\' && j + 1 < n) j++; else if (ch == '"') in_s = 0; j++; continue; }
                    if (in_c) { if (ch == '\\' && j + 1 < n) j++; else if (ch == '\'') in_c = 0; j++; continue; }
                    if (ch == '"') { in_s = 1; j++; continue; }
                    if (ch == '\'') { in_c = 1; j++; continue; }
                    if (ch == '(') paren_depth++;
                    else if (ch == ')') paren_depth--;
                    if (paren_depth > 0) j++;
                }
                
                if (paren_depth != 0) {
                    /* ERROR: unclosed paren in error type */
                    char rel[1024];
                    cc_pp_error_cat(cc_path_rel_to_repo(input_path ? input_path : "<input>", rel, sizeof(rel)),
                            scan.line, scan.col, "type", "unclosed '(' in result error type");
                    fprintf(stderr, "  hint: result type syntax is 'T !> (ErrorType)'\n");
                    free(out);
                    return NULL;
                }
                
                if (paren_depth == 0) {
                    /* Found matching ')' at position j */
                    size_t err_end = j;
                    
                    /* Trim trailing whitespace from error type */
                    while (err_end > err_start && (src[err_end - 1] == ' ' || src[err_end - 1] == '\t' ||
                                                    src[err_end - 1] == '\n' || src[err_end - 1] == '\r')) {
                        err_end--;
                    }
                    
                    /* VALIDATE: error type cannot be empty */
                    if (err_end <= err_start) {
                        char rel[1024];
                        cc_pp_error_cat(cc_path_rel_to_repo(input_path ? input_path : "<input>", rel, sizeof(rel)),
                                scan.line, scan.col, "type", "empty error type in '!> ()'");
                        fprintf(stderr, "  hint: specify an error type, e.g., 'int !> (Error)' or 'int !> (int)'\n");
                        free(out);
                        return NULL;
                    }
                    
                    size_t paren_close = j;
                    (void)paren_close;  /* unused but kept for clarity */
                    j++;  /* skip ')' */
                    
                    /* Scan back from '!>' to find the ok type start */
                    /* First skip any whitespace before '!>' */
                    size_t ty_end = sigil_pos;
                    while (ty_end > 0 && (src[ty_end - 1] == ' ' || src[ty_end - 1] == '\t')) ty_end--;
                    
                    size_t ty_start = cc__scan_back_to_delim(src, ty_end);
                    
                    /* VALIDATE: ok type cannot be empty */
                    if (ty_start >= ty_end) {
                        char rel[1024];
                        cc_pp_error_cat(cc_path_rel_to_repo(input_path ? input_path : "<input>", rel, sizeof(rel)),
                                scan.line, scan.col, "type", "missing ok type before '!>'");
                        fprintf(stderr, "  hint: result type syntax is 'T !> (ErrorType)', where T is the success type\n");
                        free(out);
                        return NULL;
                    }
                    
                    if (ty_start < ty_end && err_start < err_end) {
                        size_t ty_len = ty_end - ty_start;
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
                            /* Emit CCResult_T_E - real type name */
                            cc_sb_append_cstr(&out, &out_len, &out_cap, "CCResult_");
                            cc_sb_append_cstr(&out, &out_len, &out_cap, mangled_ok);
                            cc_sb_append_cstr(&out, &out_len, &out_cap, "_");
                            cc_sb_append_cstr(&out, &out_len, &out_cap, mangled_err);
                            last_emit = j;  /* skip past ')' */
                            
                            /* Register result-typed variable for UFCS.
                               Look for variable name after the type (skip ws and *). */
                            CCTypeRegistry* reg = cc_type_registry_get_global();
                            if (reg) {
                                size_t v = cc_skip_ws_and_comments(src, n, j);
                                /* Skip pointer modifiers */
                                while (v < n && src[v] == '*') v++;
                                v = cc_skip_ws_and_comments(src, n, v);
                                if (v < n && cc_is_ident_start(src[v])) {
                                    size_t var_start = v;
                                    while (v < n && cc_is_ident_char(src[v])) v++;
                                    char var_name[128];
                                    char type_name[256];
                                    size_t vn_len = v - var_start;
                                    if (vn_len < sizeof(var_name)) {
                                        memcpy(var_name, src + var_start, vn_len);
                                        var_name[vn_len] = 0;
                                        /* Type name is CCResult_T_E */
                                        snprintf(type_name, sizeof(type_name), "CCResult_%s_%s",
                                                 mangled_ok, mangled_err);
                                        cc_type_registry_add_var(reg, var_name, type_name);
                                    }
                                }
                            }
                            
                            i = j;
                            continue;
                        }
                    }
                }
            }
        }
        
        i++;
    }
    
    if (last_emit < n) cc_sb_append(&out, &out_len, &out_cap, src + last_emit, n - last_emit);
    
    /* Result types are collected for codegen to emit CC_DECL_RESULT_SPEC declarations.
       Parser mode uses the __CC_RESULT(T, E) macro which expands to __CCResultGeneric,
       so no explicit typedefs needed here - the macro in cc_result.cch handles it. */
    (void)cc__result_type_count;
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
    CCScannerState scan;
    cc_scanner_init(&scan);

    while (i < n) {
        /* Skip comments and strings using shared helper */
        if (cc_scanner_skip_non_code(&scan, src, n, &i)) continue;
        
        char c = src[i];

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
                    cc_pp_error_cat(cc_path_rel_to_repo(input_path ? input_path : "<input>", rel, sizeof(rel)),
                            scan.line, scan.col, "type", "unterminated slice type (missing ']')");
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
                i = k + 1;
                continue;
            }
        }

        i++;
    }

    if (last_emit < n) cc_sb_append(&out, &out_len, &out_cap, src + last_emit, n - last_emit);
    return out;
}

/* Simple pass: convert `if @try (` to `if (try ` so the main rewriter handles both syntaxes */
static char* cc__normalize_if_try_syntax(const char* src, size_t n) {
    if (!src || n == 0) return NULL;
    char* out = NULL;
    size_t out_len = 0, out_cap = 0;
    size_t i = 0, last_emit = 0;
    
    while (i + 9 < n) {  /* "if @try (" is 9 chars minimum */
        /* Look for "if" followed by whitespace, "@try", whitespace, "(" */
        if (src[i] == 'i' && src[i+1] == 'f' && 
            (i == 0 || !cc_is_ident_char(src[i-1])) &&
            (src[i+2] == ' ' || src[i+2] == '\t' || src[i+2] == '\n')) {
            size_t j = i + 2;
            while (j < n && (src[j] == ' ' || src[j] == '\t' || src[j] == '\n')) j++;
            if (j + 4 < n && src[j] == '@' && src[j+1] == 't' && src[j+2] == 'r' && src[j+3] == 'y') {
                size_t k = j + 4;
                while (k < n && (src[k] == ' ' || src[k] == '\t' || src[k] == '\n')) k++;
                if (k < n && src[k] == '(') {
                    /* Found "if @try (" - convert to "if (try " */
                    cc_sb_append(&out, &out_len, &out_cap, src + last_emit, i - last_emit);
                    cc_sb_append_cstr(&out, &out_len, &out_cap, "if (try ");
                    last_emit = k + 1;  /* Skip past the '(' */
                    i = k + 1;
                    continue;
                }
            }
        }
        i++;
    }
    if (last_emit == 0) return NULL;
    if (last_emit < n) cc_sb_append(&out, &out_len, &out_cap, src + last_emit, n - last_emit);
    return out;
}

/* Rewrite if (try T x = expr) { ... } else { ... } to expanded form */
static char* cc__rewrite_try_binding(const char* src, size_t n) {
    if (!src || n == 0) return NULL;
    char* out = NULL;
    size_t out_len = 0, out_cap = 0;
    size_t i = 0, last_emit = 0;
    int in_lc = 0, in_bc = 0, in_str = 0, in_chr = 0;
    
    while (i < n) {
        char c = src[i], c2 = (i+1 < n) ? src[i+1] : 0;
        if (in_lc) { if (c == '\n') in_lc = 0; i++; continue; }
        if (in_bc) { if (c == '*' && c2 == '/') { in_bc = 0; i += 2; continue; } i++; continue; }
        if (in_str) { if (c == '\\' && i+1 < n) { i += 2; continue; } if (c == '"') in_str = 0; i++; continue; }
        if (in_chr) { if (c == '\\' && i+1 < n) { i += 2; continue; } if (c == '\'') in_chr = 0; i++; continue; }
        if (c == '/' && c2 == '/') { in_lc = 1; i += 2; continue; }
        if (c == '/' && c2 == '*') { in_bc = 1; i += 2; continue; }
        if (c == '"') { in_str = 1; i++; continue; }
        if (c == '\'') { in_chr = 1; i++; continue; }
        
        /* Detect if (try ...) */
        if (c == 'i' && c2 == 'f') {
            int ws = (i == 0) || !cc_is_ident_char(src[i-1]);
            int we = (i+2 >= n) || !cc_is_ident_char(src[i+2]);
            if (ws && we) {
                size_t if_start = i, j = i + 2;
                while (j < n && (src[j] == ' ' || src[j] == '\t' || src[j] == '\n')) j++;
                if (j < n && src[j] == '(') {
                    j++;
                    while (j < n && (src[j] == ' ' || src[j] == '\t' || src[j] == '\n')) j++;
                    /* Check for 'try' */
                    if (j+3 <= n && src[j] == 't' && src[j+1] == 'r' && src[j+2] == 'y' &&
                        (j+3 >= n || !cc_is_ident_char(src[j+3]))) {
                        size_t after_try = j + 3;
                        while (after_try < n && (src[after_try] == ' ' || src[after_try] == '\t' || src[after_try] == '\n')) after_try++;
                        
                        /* Find closing ')' */
                        size_t cond_end = after_try;
                        int paren = 1, in_s = 0, in_c = 0;
                        while (cond_end < n && paren > 0) {
                            char ec = src[cond_end];
                            if (in_s) { if (ec == '\\' && cond_end+1 < n) cond_end++; else if (ec == '"') in_s = 0; cond_end++; continue; }
                            if (in_c) { if (ec == '\\' && cond_end+1 < n) cond_end++; else if (ec == '\'') in_c = 0; cond_end++; continue; }
                            if (ec == '"') { in_s = 1; cond_end++; continue; }
                            if (ec == '\'') { in_c = 1; cond_end++; continue; }
                            if (ec == '(') paren++;
                            else if (ec == ')') { paren--; if (paren == 0) break; }
                            cond_end++;
                        }
                        if (paren != 0) { i++; continue; }
                        
                        /* Find '=' */
                        size_t eq = after_try;
                        while (eq < cond_end && src[eq] != '=') eq++;
                        if (eq >= cond_end) { i++; continue; }
                        
                        /* Type and var: after_try to eq, var is last ident */
                        size_t tv_end = eq;
                        while (tv_end > after_try && (src[tv_end-1] == ' ' || src[tv_end-1] == '\t' || src[tv_end-1] == '\n')) tv_end--;
                        size_t var_end = tv_end, var_start = var_end;
                        while (var_start > after_try && cc_is_ident_char(src[var_start-1])) var_start--;
                        if (var_start >= var_end) { i++; continue; }
                        
                        size_t type_end = var_start;
                        while (type_end > after_try && (src[type_end-1] == ' ' || src[type_end-1] == '\t' || src[type_end-1] == '\n')) type_end--;
                        size_t type_start = after_try;
                        while (type_start < type_end && (src[type_start] == ' ' || src[type_start] == '\t' || src[type_start] == '\n')) type_start++;
                        if (type_start >= type_end) { i++; continue; }
                        
                        /* Expr: eq+1 to cond_end */
                        size_t expr_start = eq + 1;
                        while (expr_start < cond_end && (src[expr_start] == ' ' || src[expr_start] == '\t' || src[expr_start] == '\n')) expr_start++;
                        size_t expr_end = cond_end;
                        while (expr_end > expr_start && (src[expr_end-1] == ' ' || src[expr_end-1] == '\t' || src[expr_end-1] == '\n')) expr_end--;
                        if (expr_start >= expr_end) { i++; continue; }
                        
                        /* Find then-block */
                        size_t k = cond_end + 1;
                        while (k < n && (src[k] == ' ' || src[k] == '\t' || src[k] == '\n')) k++;
                        if (k >= n || src[k] != '{') { i++; continue; }
                        
                        size_t then_start = k;
                        int brace = 1; k++; in_s = 0; in_c = 0;
                        while (k < n && brace > 0) {
                            char ec = src[k];
                            if (in_s) { if (ec == '\\' && k+1 < n) k++; else if (ec == '"') in_s = 0; k++; continue; }
                            if (in_c) { if (ec == '\\' && k+1 < n) k++; else if (ec == '\'') in_c = 0; k++; continue; }
                            if (ec == '"') { in_s = 1; k++; continue; }
                            if (ec == '\'') { in_c = 1; k++; continue; }
                            if (ec == '{') brace++; else if (ec == '}') brace--;
                            k++;
                        }
                        size_t then_end = k;
                        
                        /* Check for else */
                        size_t else_start = 0, else_end = 0, m = k;
                        while (m < n && (src[m] == ' ' || src[m] == '\t' || src[m] == '\n')) m++;
                        if (m+4 <= n && src[m] == 'e' && src[m+1] == 'l' && src[m+2] == 's' && src[m+3] == 'e' &&
                            (m+4 >= n || !cc_is_ident_char(src[m+4]))) {
                            m += 4;
                            while (m < n && (src[m] == ' ' || src[m] == '\t' || src[m] == '\n')) m++;
                            if (m < n && src[m] == '{') {
                                else_start = m; brace = 1; m++; in_s = 0; in_c = 0;
                                while (m < n && brace > 0) {
                                    char ec = src[m];
                                    if (in_s) { if (ec == '\\' && m+1 < n) m++; else if (ec == '"') in_s = 0; m++; continue; }
                                    if (in_c) { if (ec == '\\' && m+1 < n) m++; else if (ec == '\'') in_c = 0; m++; continue; }
                                    if (ec == '"') { in_s = 1; m++; continue; }
                                    if (ec == '\'') { in_c = 1; m++; continue; }
                                    if (ec == '{') brace++; else if (ec == '}') brace--;
                                    m++;
                                }
                                else_end = m;
                            }
                        }
                        
                        /* Emit expansion */
                        cc_sb_append(&out, &out_len, &out_cap, src + last_emit, if_start - last_emit);
                        cc_sb_append_cstr(&out, &out_len, &out_cap, "{ __typeof__(");
                        cc_sb_append(&out, &out_len, &out_cap, src + expr_start, expr_end - expr_start);
                        cc_sb_append_cstr(&out, &out_len, &out_cap, ") __cc_try_bind = (");
                        cc_sb_append(&out, &out_len, &out_cap, src + expr_start, expr_end - expr_start);
                        cc_sb_append_cstr(&out, &out_len, &out_cap, "); if (__cc_try_bind.ok) { ");
                        cc_sb_append(&out, &out_len, &out_cap, src + type_start, type_end - type_start);
                        cc_sb_append_cstr(&out, &out_len, &out_cap, " ");
                        cc_sb_append(&out, &out_len, &out_cap, src + var_start, var_end - var_start);
                        cc_sb_append_cstr(&out, &out_len, &out_cap, " = __cc_try_bind.u.value; ");
                        cc_sb_append(&out, &out_len, &out_cap, src + then_start + 1, then_end - then_start - 2);
                        cc_sb_append_cstr(&out, &out_len, &out_cap, " }");
                        if (else_end > else_start) {
                            cc_sb_append_cstr(&out, &out_len, &out_cap, " else ");
                            cc_sb_append(&out, &out_len, &out_cap, src + else_start, else_end - else_start);
                        }
                        cc_sb_append_cstr(&out, &out_len, &out_cap, " }");
                        
                        last_emit = (else_end > 0) ? else_end : then_end;
                        i = last_emit;
                        continue;
                    }
                }
            }
        }
        i++;
    }
    if (last_emit == 0) return NULL;
    if (last_emit < n) cc_sb_append(&out, &out_len, &out_cap, src + last_emit, n - last_emit);
    return out;
}

/* Simple pass: rewrite @defer(err) -> @defer */
static char* cc__rewrite_defer_syntax(const char* src, size_t n) {
    if (!src || n == 0) return NULL;
    char* out = NULL;
    size_t out_len = 0, out_cap = 0;
    size_t i = 0, last_emit = 0;
    
    while (i + 6 < n) {
        if (src[i] == '@' && strncmp(src + i + 1, "defer", 5) == 0) {
            size_t j = i + 6;
            while (j < n && (src[j] == ' ' || src[j] == '\t' || src[j] == '\n')) j++;
            if (j < n && src[j] == '(') {
                /* Found @defer( - skip parenthesized part */
                size_t k = j + 1;
                int paren = 1;
                while (k < n && paren > 0) {
                    if (src[k] == '(') paren++;
                    else if (src[k] == ')') paren--;
                    k++;
                }
                if (paren == 0) {
                    /* Rewrite @defer(...) -> @defer */
                    cc_sb_append(&out, &out_len, &out_cap, src + last_emit, i - last_emit);
                    cc_sb_append_cstr(&out, &out_len, &out_cap, "@defer ");
                    last_emit = k;
                    i = k;
                    continue;
                }
            }
        }
        i++;
    }
    
    if (last_emit == 0) return NULL;
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
    CCScannerState scan;
    cc_scanner_init(&scan);
    
    while (i < n) {
        /* Skip comments and strings using shared helper */
        if (cc_scanner_skip_non_code(&scan, src, n, &i)) continue;
        
        char c = src[i];
        
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
    
    /* Pass 1: Collect optional and result variable names */
    #define MAX_UNWRAP_VARS 256
    typedef struct {
        char* name;
        int is_optional;  /* 1 for Optional, 0 for Result */
    } UnwrapVar;
    UnwrapVar vars[MAX_UNWRAP_VARS];
    int var_count = 0;
    
    size_t i = 0;
    CCScannerState scan;
    cc_scanner_init(&scan);
    
    while (i < n && var_count < MAX_UNWRAP_VARS) {
        /* Skip comments and strings using shared helper */
        if (cc_scanner_skip_non_code(&scan, src, n, &i)) continue;
        
        char c = src[i];
        
        /* Look for Optional types: CCOptional_ or __CC_OPTIONAL( */
        int is_cc_optional = (c == 'C' && i + 10 < n && strncmp(src + i, "CCOptional_", 11) == 0);
        int is_macro_optional = (c == '_' && i + 14 < n && strncmp(src + i, "__CC_OPTIONAL(", 14) == 0);
        
        if (is_cc_optional || is_macro_optional) {
            /* Skip to end of type name */
            size_t type_end = i;
            if (is_cc_optional) {
                type_end += 11;
                while (type_end < n && cc_is_ident_char(src[type_end])) type_end++;
            } else {
                /* __CC_OPTIONAL(T) - skip to closing paren */
                type_end += 14;
                int paren_depth = 1;
                while (type_end < n && paren_depth > 0) {
                    if (src[type_end] == '(') paren_depth++;
                    else if (src[type_end] == ')') paren_depth--;
                    type_end++;
                }
            }
            /* Skip whitespace */
            size_t ws_end = type_end;
            while (ws_end < n && (src[ws_end] == ' ' || src[ws_end] == '\t' || src[ws_end] == '\n')) ws_end++;
            /* Check for variable name (not function) */
            if (ws_end < n && cc_is_ident_start(src[ws_end])) {
                size_t var_start = ws_end;
                while (ws_end < n && cc_is_ident_char(src[ws_end])) ws_end++;
                size_t var_len = ws_end - var_start;
                /* Skip whitespace */
                size_t after_ws = ws_end;
                while (after_ws < n && (src[after_ws] == ' ' || src[after_ws] == '\t')) after_ws++;
                /* If followed by '=' or ';' or ',', it's a variable declaration */
                if (after_ws < n && (src[after_ws] == '=' || src[after_ws] == ';' || src[after_ws] == ',')) {
                    char* varname = (char*)malloc(var_len + 1);
                    if (varname) {
                        memcpy(varname, src + var_start, var_len);
                        varname[var_len] = 0;
                        vars[var_count].name = varname;
                        vars[var_count].is_optional = 1;
                        var_count++;
                    }
                }
            }
            i = type_end;
            continue;
        }
        
        /* Look for Result types: CCResult_ or __CC_RESULT( */
        int is_cc_result = (c == 'C' && i + 8 < n && strncmp(src + i, "CCResult_", 9) == 0);
        int is_macro_result = (c == '_' && i + 13 < n && strncmp(src + i, "__CC_RESULT(", 12) == 0);
        
        if (is_cc_result || is_macro_result) {
            /* Skip to end of type name */
            size_t type_end = i;
            if (is_cc_result) {
                type_end += 9;
                /* CCResult_T_E - skip through identifier and underscore and another identifier */
                while (type_end < n && (cc_is_ident_char(src[type_end]) || src[type_end] == '_')) type_end++;
            } else {
                /* __CC_RESULT(T, E) - skip to closing paren */
                type_end += 12;
                int paren_depth = 1;
                while (type_end < n && paren_depth > 0) {
                    if (src[type_end] == '(') paren_depth++;
                    else if (src[type_end] == ')') paren_depth--;
                    type_end++;
                }
            }
            /* Skip whitespace */
            size_t ws_end = type_end;
            while (ws_end < n && (src[ws_end] == ' ' || src[ws_end] == '\t' || src[ws_end] == '\n')) ws_end++;
            /* Check for variable name (not function) */
            if (ws_end < n && cc_is_ident_start(src[ws_end])) {
                size_t var_start = ws_end;
                while (ws_end < n && cc_is_ident_char(src[ws_end])) ws_end++;
                size_t var_len = ws_end - var_start;
                /* Skip whitespace */
                size_t after_ws = ws_end;
                while (after_ws < n && (src[after_ws] == ' ' || src[after_ws] == '\t')) after_ws++;
                /* If followed by '=' or ';' or ',', it's a variable declaration */
                if (after_ws < n && (src[after_ws] == '=' || src[after_ws] == ';' || src[after_ws] == ',')) {
                    char* varname = (char*)malloc(var_len + 1);
                    if (varname) {
                        memcpy(varname, src + var_start, var_len);
                        varname[var_len] = 0;
                        vars[var_count].name = varname;
                        vars[var_count].is_optional = 0;
                        var_count++;
                    }
                }
            }
            i = type_end;
            continue;
        }
        
        i++;
    }
    
    /* If no vars found, nothing to rewrite */
    if (var_count == 0) return NULL;
    
    /* Pass 2: Rewrite *varname to unwrap call */
    char* out = NULL;
    size_t out_len = 0, out_cap = 0;
    
    i = 0;
    size_t last_emit = 0;
    cc_scanner_init(&scan);  /* Reset scanner for pass 2 */
    
    while (i < n) {
        /* Skip comments and strings using shared helper */
        if (cc_scanner_skip_non_code(&scan, src, n, &i)) continue;
        
        char c = src[i];
        
        /* Look for * followed by an optional/result variable name */
        if (c == '*') {
            size_t star_pos = i;
            
            /* Skip if this looks like a pointer type declaration (preceded by identifier).
               E.g. "S* res" should NOT be rewritten, but "*res" or "= *res" should be. */
            int is_ptr_type_decl = 0;
            if (star_pos > 0) {
                size_t prev = star_pos - 1;
                /* Skip whitespace before the * */
                while (prev > 0 && (src[prev] == ' ' || src[prev] == '\t')) prev--;
                /* If preceded by identifier char, it's likely a type like "S*" or "int*" */
                if (prev < n && cc_is_ident_char(src[prev])) {
                    is_ptr_type_decl = 1;
                }
            }
            
            if (is_ptr_type_decl) {
                i++;
                continue;
            }
            
            i++;
            /* Skip whitespace */
            while (i < n && (src[i] == ' ' || src[i] == '\t')) i++;
            /* Check for identifier */
            if (i < n && cc_is_ident_start(src[i])) {
                size_t var_start = i;
                while (i < n && cc_is_ident_char(src[i])) i++;
                size_t var_len = i - var_start;
                
                /* Check if this identifier is in our vars list */
                int found_idx = -1;
                for (int j = 0; j < var_count; j++) {
                    if (strlen(vars[j].name) == var_len && strncmp(vars[j].name, src + var_start, var_len) == 0) {
                        found_idx = j;
                        break;
                    }
                }
                
                if (found_idx >= 0) {
                    /* Rewrite *varname to unwrap call */
                    cc_sb_append(&out, &out_len, &out_cap, src + last_emit, star_pos - last_emit);
                    if (vars[found_idx].is_optional) {
                        cc_sb_append_cstr(&out, &out_len, &out_cap, "cc_unwrap_opt(");
                    } else {
                        cc_sb_append_cstr(&out, &out_len, &out_cap, "cc_unwrap(");
                    }
                    cc_sb_append(&out, &out_len, &out_cap, src + var_start, var_len);
                    cc_sb_append_cstr(&out, &out_len, &out_cap, ")");
                    last_emit = i;
                    continue;
                }
            }
            /* Not a match, continue normally */
            i = star_pos + 1;
            continue;
        }
        
        i++;
    }
    
    /* Free vars */
    for (int j = 0; j < var_count; j++) {
        free(vars[j].name);
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
                    cc_pp_error_cat(rel, line, col, "async", "channel operation '%s' must be awaited in @async function", op_name);
                    fprintf(stderr, "  hint: add 'await' before this call\n");
                    errors++;
                }
                i += op_len - 1; /* Skip past the op name */
            }
        }
    }

    return errors > 0 ? -1 : 0;
}

/* Track @async functions for @nonblocking inference. */
typedef struct {
    char name[128];
    int is_explicit_nonblocking;  /* Has @nonblocking attribute */
    int has_loop_with_chan_op;    /* Contains loop with channel ops */
} CCAsyncFnInfo;

#define CC_MAX_ASYNC_FNS 256
static CCAsyncFnInfo cc__async_fns[CC_MAX_ASYNC_FNS];
static int cc__async_fn_count = 0;

/* Check if an @async function is nonblocking (either explicit or inferred).
   Note: Reserved for future cross-file analysis. */
__attribute__((unused))
static int cc__fn_is_nonblocking(const char* name) {
    for (int i = 0; i < cc__async_fn_count; i++) {
        if (strcmp(cc__async_fns[i].name, name) == 0) {
            if (cc__async_fns[i].is_explicit_nonblocking) return 1;
            if (!cc__async_fns[i].has_loop_with_chan_op) return 1;
            return 0;
        }
    }
    /* Unknown function - assume nonblocking (could be from another file) */
    return 1;
}

/* Collect @async function info and check cc_block_on calls.
   Warns if cc_block_on is called with a function that has loops with channel ops.
   Returns number of warnings. */
static int cc__check_block_on_nonblocking(const char* src, size_t n, const char* input_path) {
    if (!src || n == 0) return 0;

    cc__async_fn_count = 0;
    int warnings = 0;

    /* Pass 1: Collect @async functions and determine if they're @nonblocking */
    int in_lc = 0, in_bc = 0, in_str = 0, in_chr = 0;
    int line = 1, col = 1;

    for (size_t i = 0; i < n; i++) {
        char c = src[i];
        char c2 = (i + 1 < n) ? src[i + 1] : 0;

        if (c == '\n') { line++; col = 1; } else { col++; }

        if (in_lc) { if (c == '\n') in_lc = 0; continue; }
        if (in_bc) { if (c == '*' && c2 == '/') { in_bc = 0; i++; col++; } continue; }
        if (in_str) { if (c == '\\' && i + 1 < n) { i++; col++; continue; } if (c == '"') in_str = 0; continue; }
        if (in_chr) { if (c == '\\' && i + 1 < n) { i++; col++; continue; } if (c == '\'') in_chr = 0; continue; }
        if (c == '/' && c2 == '/') { in_lc = 1; i++; col++; continue; }
        if (c == '/' && c2 == '*') { in_bc = 1; i++; col++; continue; }
        if (c == '"') { in_str = 1; continue; }
        if (c == '\'') { in_chr = 1; continue; }

        /* Look for @async or @async @nonblocking */
        if (c == '@' && i + 5 < n && memcmp(src + i + 1, "async", 5) == 0 && !cc_is_ident_char(src[i + 6])) {
            size_t j = i + 6;
            int is_explicit_nb = 0;

            /* Skip whitespace and check for @nonblocking */
            while (j < n && (src[j] == ' ' || src[j] == '\t' || src[j] == '\n')) j++;
            if (j + 12 < n && src[j] == '@' && memcmp(src + j + 1, "nonblocking", 11) == 0 && !cc_is_ident_char(src[j + 12])) {
                is_explicit_nb = 1;
                j += 12;
            }

            /* Skip to function name: skip return type and get identifier before '(' */
            while (j < n && src[j] != '(' && src[j] != '{' && src[j] != ';') j++;
            if (j >= n || src[j] != '(') continue;

            /* Back up to find function name */
            size_t paren = j;
            j--;
            while (j > i && (src[j] == ' ' || src[j] == '\t')) j--;
            if (j <= i || !cc_is_ident_char(src[j])) continue;

            size_t name_end = j + 1;
            while (j > i && cc_is_ident_char(src[j - 1])) j--;
            size_t name_start = j;
            size_t name_len = name_end - name_start;

            if (name_len == 0 || name_len >= 127) continue;

            /* Find function body */
            j = paren + 1;
            int depth = 1;
            while (j < n && depth > 0) {
                if (src[j] == '(') depth++;
                else if (src[j] == ')') depth--;
                j++;
            }
            while (j < n && src[j] != '{' && src[j] != ';') j++;
            if (j >= n || src[j] != '{') continue;

            size_t body_start = j;
            depth = 1;
            j++;
            while (j < n && depth > 0) {
                if (src[j] == '{') depth++;
                else if (src[j] == '}') depth--;
                j++;
            }
            size_t body_end = j;

            /* Check for loops with channel ops in the body */
            int has_loop_with_chan = 0;

            for (size_t k = body_start; k < body_end && !has_loop_with_chan; k++) {
                /* Simple check: look for 'for' or 'while' keyword */
                int is_for = (k + 3 <= body_end && memcmp(src + k, "for", 3) == 0);
                int is_while = (k + 5 <= body_end && memcmp(src + k, "while", 5) == 0);
                if (!is_for && !is_while) continue;
                if (k > 0 && cc_is_ident_char(src[k - 1])) continue;
                size_t kw_len = is_for ? 3 : 5;
                if (k + kw_len < body_end && cc_is_ident_char(src[k + kw_len])) continue;

                /* Found a loop keyword - scan to find its body, handling parentheses in for() header */
                size_t loop_start = k + kw_len;
                int paren_depth = 0;
                while (loop_start < body_end) {
                    char lc = src[loop_start];
                    if (lc == '(') paren_depth++;
                    else if (lc == ')') paren_depth--;
                    else if (lc == '{' && paren_depth == 0) break;  /* Found loop body */
                    else if (lc == ';' && paren_depth == 0) break;  /* Single-statement loop (no braces) */
                    loop_start++;
                }
                if (loop_start >= body_end || src[loop_start] != '{') continue; /* Skip single-line loops for now */

                int ld = 1;
                size_t loop_end = loop_start + 1;
                while (loop_end < body_end && ld > 0) {
                    if (src[loop_end] == '{') ld++;
                    else if (src[loop_end] == '}') ld--;
                    loop_end++;
                }

                /* Check for channel ops in loop body - look for .send( or .recv( */
                for (size_t m = loop_start; m < loop_end; m++) {
                    if (m + 5 <= loop_end && memcmp(src + m, ".send", 5) == 0) {
                        has_loop_with_chan = 1;
                        break;
                    }
                    if (m + 5 <= loop_end && memcmp(src + m, ".recv", 5) == 0) {
                        has_loop_with_chan = 1;
                        break;
                    }
                    /* Also check for chan_send/chan_recv macro forms */
                    if (m + 9 <= loop_end && memcmp(src + m, "chan_send", 9) == 0) {
                        has_loop_with_chan = 1;
                        break;
                    }
                    if (m + 9 <= loop_end && memcmp(src + m, "chan_recv", 9) == 0) {
                        has_loop_with_chan = 1;
                        break;
                    }
                }
            }

            /* Store function info */
            if (cc__async_fn_count < CC_MAX_ASYNC_FNS) {
                memcpy(cc__async_fns[cc__async_fn_count].name, src + name_start, name_len);
                cc__async_fns[cc__async_fn_count].name[name_len] = '\0';
                cc__async_fns[cc__async_fn_count].is_explicit_nonblocking = is_explicit_nb;
                cc__async_fns[cc__async_fn_count].has_loop_with_chan_op = has_loop_with_chan;
                cc__async_fn_count++;
            }

            i = body_end - 1;
            continue;
        }
    }

    /* Pass 2: Check cc_block_on calls */
    in_lc = in_bc = in_str = in_chr = 0;
    line = 1; col = 1;

    for (size_t i = 0; i < n; i++) {
        char c = src[i];
        char c2 = (i + 1 < n) ? src[i + 1] : 0;

        if (c == '\n') { line++; col = 1; } else { col++; }

        if (in_lc) { if (c == '\n') in_lc = 0; continue; }
        if (in_bc) { if (c == '*' && c2 == '/') { in_bc = 0; i++; col++; } continue; }
        if (in_str) { if (c == '\\' && i + 1 < n) { i++; col++; continue; } if (c == '"') in_str = 0; continue; }
        if (in_chr) { if (c == '\\' && i + 1 < n) { i++; col++; continue; } if (c == '\'') in_chr = 0; continue; }
        if (c == '/' && c2 == '/') { in_lc = 1; i++; col++; continue; }
        if (c == '/' && c2 == '*') { in_bc = 1; i++; col++; continue; }
        if (c == '"') { in_str = 1; continue; }
        if (c == '\'') { in_chr = 1; continue; }

        /* Look for cc_block_on( */
        if (c == 'c' && i + 11 < n && memcmp(src + i, "cc_block_on", 11) == 0 && src[i + 11] == '(') {
            int call_line = line, call_col = col;
            size_t j = i + 12;

            /* Skip the type argument */
            while (j < n && src[j] != ',') j++;
            if (j >= n) continue;
            j++; /* Skip comma */

            /* Skip whitespace */
            while (j < n && (src[j] == ' ' || src[j] == '\t' || src[j] == '\n')) j++;

            /* Extract the function name being called */
            if (j >= n || !cc_is_ident_start(src[j])) continue;
            size_t fn_start = j;
            while (j < n && cc_is_ident_char(src[j])) j++;
            size_t fn_len = j - fn_start;

            if (fn_len == 0 || fn_len >= 127) continue;

            char fn_name[128];
            memcpy(fn_name, src + fn_start, fn_len);
            fn_name[fn_len] = '\0';

            /* Check if this function is known and not @nonblocking */
            for (int fi = 0; fi < cc__async_fn_count; fi++) {
                if (strcmp(cc__async_fns[fi].name, fn_name) == 0) {
                    if (!cc__async_fns[fi].is_explicit_nonblocking && cc__async_fns[fi].has_loop_with_chan_op) {
                        char rel[1024];
                        cc_path_rel_to_repo(input_path, rel, sizeof(rel));
                        fprintf(stderr, "%s:%d:%d: warning: cc_block_on with '%s' may deadlock\n",
                                rel, call_line, call_col, fn_name);
                        fprintf(stderr, "%s:%d:%d: note: '%s' has channel ops in a loop; consider @nursery for concurrency or larger buffer\n",
                                rel, call_line, call_col, fn_name);
                        warnings++;
                    }
                    break;
                }
            }

            i = j - 1;
        }
    }

    return warnings;
}

/* Infer result constructor types from enclosing function signature.
   Within a function returning `T!E`:
     cc_ok(v)   -> cc_ok(T, v)     for T!CCError
     cc_ok(v)   -> cc_ok(T, E, v)  for T!E (custom error)
     cc_err(e)  -> cc_err(T, e)    for T!CCError  
     cc_err(e)  -> cc_err(T, E, e) for T!E (custom error)
   
   This allows users to write just `cc_ok(42)` and have the compiler infer the type. */
static char* cc__rewrite_inferred_result_ctors(const char* src, size_t n) {
    if (!src || n == 0) return NULL;
    
    char* out = NULL;
    size_t out_len = 0, out_cap = 0;
    size_t last_emit = 0;
    
    /* Track current function's result type */
    char current_ok_type[128] = {0};   /* e.g., "int" */
    char current_err_type[128] = {0};  /* e.g., "CCError" or custom */
    int brace_depth = 0;
    int fn_brace_depth = -1;
    
    int in_lc = 0, in_bc = 0, in_str = 0, in_chr = 0;
    
    for (size_t i = 0; i < n; ) {
        char c = src[i];
        char c2 = (i + 1 < n) ? src[i + 1] : 0;
        
        /* Skip comments and strings */
        if (in_lc) { if (c == '\n') in_lc = 0; i++; continue; }
        if (in_bc) { if (c == '*' && c2 == '/') { in_bc = 0; i += 2; continue; } i++; continue; }
        if (in_str) { if (c == '\\' && i + 1 < n) { i += 2; continue; } if (c == '"') in_str = 0; i++; continue; }
        if (in_chr) { if (c == '\\' && i + 1 < n) { i += 2; continue; } if (c == '\'') in_chr = 0; i++; continue; }
        
        if (c == '/' && c2 == '/') { in_lc = 1; i += 2; continue; }
        if (c == '/' && c2 == '*') { in_bc = 1; i += 2; continue; }
        if (c == '"') { in_str = 1; i++; continue; }
        if (c == '\'') { in_chr = 1; i++; continue; }
        
        /* Track braces */
        if (c == '{') {
            brace_depth++;
            i++;
            continue;
        }
        if (c == '}') {
            brace_depth--;
            if (fn_brace_depth >= 0 && brace_depth < fn_brace_depth) {
                current_ok_type[0] = 0;
                current_err_type[0] = 0;
                fn_brace_depth = -1;
            }
            i++;
            continue;
        }
        
        /* Detect function returning T!E or T!>(E) - look for pattern: T!E name( or T!>(E) name( 
           Handle space before ! (e.g., "MyData !> (MyError)" or "MyData*!>(MyError)") */
        if (c == '!' && c2 != '=' && fn_brace_depth < 0 && i > 0) {
            /* Skip backwards over whitespace to find the type */
            size_t prev_idx = i - 1;
            while (prev_idx > 0 && (src[prev_idx] == ' ' || src[prev_idx] == '\t')) prev_idx--;
            char prev = src[prev_idx];
            /* Valid chars before !: identifier chars, closing brackets, pointer star */
            if (cc_is_ident_char(prev) || prev == ')' || prev == ']' || prev == '>' || prev == '*') {
                /* Check for error type after ! - two forms:
                   1. T!E (simple form)
                   2. T!>(E) (arrow form with parentheses) */
                size_t j = i + 1;
                size_t err_start = 0, err_end = 0;
                
                /* Check for !> arrow form */
                if (j < n && src[j] == '>') {
                    j++; /* skip '>' */
                    while (j < n && (src[j] == ' ' || src[j] == '\t')) j++;
                    if (j < n && src[j] == '(') {
                        j++; /* skip '(' */
                        while (j < n && (src[j] == ' ' || src[j] == '\t')) j++;
                        err_start = j;
                        while (j < n && cc_is_ident_char(src[j])) j++;
                        err_end = j;
                        while (j < n && (src[j] == ' ' || src[j] == '\t')) j++;
                        if (j < n && src[j] == ')') j++; /* skip ')' */
                    }
                } else {
                    /* Simple !E form */
                    while (j < n && (src[j] == ' ' || src[j] == '\t')) j++;
                    if (j < n && cc_is_ident_start(src[j])) {
                        err_start = j;
                        while (j < n && cc_is_ident_char(src[j])) j++;
                        err_end = j;
                    }
                }
                
                if (err_start < err_end) {
                    
                    /* Skip whitespace, then check for identifier followed by '(' */
                    while (j < n && (src[j] == ' ' || src[j] == '\t' || src[j] == '\n' || src[j] == '*')) j++;
                    if (j < n && cc_is_ident_start(src[j])) {
                        size_t name_start = j;
                        while (j < n && cc_is_ident_char(src[j])) j++;
                        (void)name_start; /* function name, not needed */
                        while (j < n && (src[j] == ' ' || src[j] == '\t')) j++;
                        if (j < n && src[j] == '(') {
                            /* Skip params to find '{' */
                            int pdepth = 1;
                            j++;
                            while (j < n && pdepth > 0) {
                                if (src[j] == '(') pdepth++;
                                else if (src[j] == ')') pdepth--;
                                else if (src[j] == '"') { j++; while (j < n && src[j] != '"') { if (src[j] == '\\' && j+1<n) j++; j++; } }
                                else if (src[j] == '\'') { j++; while (j < n && src[j] != '\'') { if (src[j] == '\\' && j+1<n) j++; j++; } }
                                j++;
                            }
                            while (j < n && (src[j] == ' ' || src[j] == '\t' || src[j] == '\n')) j++;
                            if (j < n && src[j] == '{') {
                                /* Found function definition! Extract ok and err types */
                                size_t ty_start = cc__scan_back_to_delim(src, i);
                                if (ty_start < i) {
                                    size_t ty_len = i - ty_start;
                                    if (ty_len < sizeof(current_ok_type) - 1) {
                                        /* Trim whitespace */
                                        while (ty_len > 0 && (src[ty_start + ty_len - 1] == ' ' || src[ty_start + ty_len - 1] == '\t')) ty_len--;
                                        memcpy(current_ok_type, src + ty_start, ty_len);
                                        current_ok_type[ty_len] = 0;
                                    }
                                    size_t err_len = err_end - err_start;
                                    if (err_len < sizeof(current_err_type) - 1) {
                                        memcpy(current_err_type, src + err_start, err_len);
                                        current_err_type[err_len] = 0;
                                    }
                                    fn_brace_depth = brace_depth;
                                    
                                    /* Register this result type so it gets declared in parse stubs.
                                       This is critical for pointer result types that are declared first. */
                                    char mangled_ok[128], mangled_err[128];
                                    cc__mangle_type_name(current_ok_type, strlen(current_ok_type), mangled_ok, sizeof(mangled_ok));
                                    cc__mangle_type_name(current_err_type, strlen(current_err_type), mangled_err, sizeof(mangled_err));
                                    cc__add_result_type(current_ok_type, strlen(current_ok_type),
                                                        current_err_type, strlen(current_err_type),
                                                        mangled_ok, mangled_err);
                                }
                            }
                        }
                    }
                }
            }
            i++;
            continue;
        }
        
        /* Rewrite cc_ok(...) and cc_err(...) when inside result-returning function */
        if (current_ok_type[0] && c == 'c' && i + 5 < n) {
            int is_ok = (memcmp(src + i, "cc_ok(", 6) == 0);
            int is_err = (memcmp(src + i, "cc_err(", 7) == 0);
            
            if (is_ok || is_err) {
                /* Check word boundary */
                int word_start = (i == 0) || !cc_is_ident_char(src[i - 1]);
                if (word_start) {
                    size_t macro_start = i;
                    size_t paren_pos = i + (is_ok ? 5 : 6);
                    
                    /* Count args to determine if short form */
                    size_t args_start = paren_pos + 1;
                    size_t j = args_start;
                    int depth = 1;
                    int comma_count = 0;
                    int in_s = 0, in_c = 0;
                    while (j < n && depth > 0) {
                        char ch = src[j];
                        if (in_s) { if (ch == '\\' && j+1<n) j++; else if (ch == '"') in_s = 0; j++; continue; }
                        if (in_c) { if (ch == '\\' && j+1<n) j++; else if (ch == '\'') in_c = 0; j++; continue; }
                        if (ch == '"') { in_s = 1; j++; continue; }
                        if (ch == '\'') { in_c = 1; j++; continue; }
                        if (ch == '(') depth++;
                        else if (ch == ')') { depth--; if (depth == 0) break; }
                        else if (ch == ',' && depth == 1) comma_count++;
                        j++;
                    }
                    
                    /* Short form: cc_ok(v) has 0 commas, cc_err(e) has 0 commas
                       Long form: cc_ok(T,v) has 1+ commas
                       Shorthand: cc_err(CC_ERR_*, "msg") has 1 comma - expand to cc_error() */
                    int is_short = (comma_count == 0);
                    
                    /* Check for cc_err shorthand: cc_err(CC_ERR_*) or cc_err(CC_ERR_*, "msg") */
                    int is_err_shorthand = 0;
                    int is_err_shorthand_no_msg = 0;
                    if (is_err && strcmp(current_err_type, "CCError") == 0) {
                        /* Check if first arg starts with CC_ERR_ */
                        size_t k = args_start;
                        while (k < j && (src[k] == ' ' || src[k] == '\t')) k++;
                        if (k + 7 < j && memcmp(src + k, "CC_ERR_", 7) == 0) {
                            is_err_shorthand = 1;
                            is_err_shorthand_no_msg = (comma_count == 0);
                        }
                    }
                    
                    if (is_err_shorthand && depth == 0) {
                        /* Rewrite cc_err(CC_ERR_*) -> cc_err_CCResult_T_E(cc_error(CC_ERR_*, NULL))
                           Rewrite cc_err(CC_ERR_*, "msg") -> cc_err_CCResult_T_E(cc_error(CC_ERR_*, "msg"))
                           Type names must be mangled (e.g., MyData* -> MyDataptr) */
                        char mangled_ok[128], mangled_err[128];
                        cc__mangle_type_name(current_ok_type, strlen(current_ok_type), mangled_ok, sizeof(mangled_ok));
                        cc__mangle_type_name(current_err_type, strlen(current_err_type), mangled_err, sizeof(mangled_err));
                        
                        cc_sb_append(&out, &out_len, &out_cap, src + last_emit, macro_start - last_emit);
                        cc_sb_append_cstr(&out, &out_len, &out_cap, "cc_err_CCResult_");
                        cc_sb_append_cstr(&out, &out_len, &out_cap, mangled_ok);
                        cc_sb_append_cstr(&out, &out_len, &out_cap, "_");
                        cc_sb_append_cstr(&out, &out_len, &out_cap, mangled_err);
                        cc_sb_append_cstr(&out, &out_len, &out_cap, "(cc_error(");
                        /* Copy args */
                        cc_sb_append(&out, &out_len, &out_cap, src + args_start, j - args_start);
                        if (is_err_shorthand_no_msg) {
                            cc_sb_append_cstr(&out, &out_len, &out_cap, ", NULL");
                        }
                        cc_sb_append_cstr(&out, &out_len, &out_cap, "))");
                        last_emit = j + 1;
                        i = j + 1;
                        continue;
                    }
                    
                    if (is_short && depth == 0) {
                        /* Rewrite short form to typed constructor call:
                           cc_ok(x) -> cc_ok_CCResult_T_E(x)
                           cc_err(e) -> cc_err_CCResult_T_E(e)
                           Type names must be mangled (e.g., MyData* -> MyDataptr) */
                        char mangled_ok[128], mangled_err[128];
                        cc__mangle_type_name(current_ok_type, strlen(current_ok_type), mangled_ok, sizeof(mangled_ok));
                        cc__mangle_type_name(current_err_type, strlen(current_err_type), mangled_err, sizeof(mangled_err));
                        
                        cc_sb_append(&out, &out_len, &out_cap, src + last_emit, macro_start - last_emit);
                        
                        if (is_ok) {
                            cc_sb_append_cstr(&out, &out_len, &out_cap, "cc_ok_CCResult_");
                        } else {
                            cc_sb_append_cstr(&out, &out_len, &out_cap, "cc_err_CCResult_");
                        }
                        cc_sb_append_cstr(&out, &out_len, &out_cap, mangled_ok);
                        cc_sb_append_cstr(&out, &out_len, &out_cap, "_");
                        cc_sb_append_cstr(&out, &out_len, &out_cap, mangled_err);
                        cc_sb_append_cstr(&out, &out_len, &out_cap, "(");
                        /* Copy argument */
                        cc_sb_append(&out, &out_len, &out_cap, src + args_start, j - args_start);
                        cc_sb_append_cstr(&out, &out_len, &out_cap, ")");
                        last_emit = j + 1;
                        i = j + 1;
                        continue;
                    }
                }
            }
        }
        
        i++;
    }
    
    if (last_emit == 0) return NULL;
    if (last_emit < n) cc_sb_append(&out, &out_len, &out_cap, src + last_emit, n - last_emit);
    return out;
}

/* Rewrite @closing(ch) spawn(...) or @closing(ch) { ... } syntax.
   This transforms the annotation into a spawned sub-nursery:
   
   @closing(ch) spawn(task);
   becomes:
   spawn(() => { @nursery closing(ch) { spawn(task); } });
   
   @closing(ch) { for (w) spawn(worker); }
   becomes:
   spawn(() => { @nursery closing(ch) { for (w) spawn(worker); } });
   
   This allows flat visual structure while maintaining correct channel close semantics. */
static char* cc__rewrite_closing_annotation(const char* src, size_t n) {
    if (!src || n == 0) return NULL;
    char* out = NULL;
    size_t out_len = 0, out_cap = 0;
    
    size_t i = 0;
    size_t last_emit = 0;
    int in_lc = 0, in_bc = 0, in_str = 0, in_chr = 0;
    
    while (i < n) {
        char c = src[i];
        char c2 = (i + 1 < n) ? src[i + 1] : 0;
        
        if (in_lc) { if (c == '\n') in_lc = 0; i++; continue; }
        if (in_bc) { if (c == '*' && c2 == '/') { in_bc = 0; i += 2; continue; } i++; continue; }
        if (in_str) { if (c == '\\' && i + 1 < n) { i += 2; continue; } if (c == '"') in_str = 0; i++; continue; }
        if (in_chr) { if (c == '\\' && i + 1 < n) { i += 2; continue; } if (c == '\'') in_chr = 0; i++; continue; }
        if (c == '/' && c2 == '/') { in_lc = 1; i += 2; continue; }
        if (c == '/' && c2 == '*') { in_bc = 1; i += 2; continue; }
        if (c == '"') { in_str = 1; i++; continue; }
        if (c == '\'') { in_chr = 1; i++; continue; }
        
        /* Look for @closing( */
        if (c == '@' && i + 8 < n && memcmp(src + i, "@closing", 8) == 0) {
            size_t start = i;
            size_t j = i + 8;
            
            /* Skip whitespace */
            while (j < n && (src[j] == ' ' || src[j] == '\t' || src[j] == '\n')) j++;
            
            if (j < n && src[j] == '(') {
                /* Found @closing( - parse the channel list */
                size_t paren_start = j;
                j++; /* skip '(' */
                int paren_depth = 1;
                while (j < n && paren_depth > 0) {
                    if (src[j] == '(') paren_depth++;
                    else if (src[j] == ')') paren_depth--;
                    j++;
                }
                if (paren_depth != 0) { i++; continue; } /* Malformed, skip */
                
                size_t paren_end = j - 1; /* position of ')' */
                
                /* Extract channel names from (ch1, ch2, ...) */
                size_t chans_start = paren_start + 1;
                size_t chans_end = paren_end;
                
                /* VALIDATION: check if channel list is empty */
                size_t cs = chans_start;
                while (cs < chans_end && (src[cs] == ' ' || src[cs] == '\t' || src[cs] == '\n')) cs++;
                if (cs >= chans_end) {
                    cc_pp_error_cat("<input>", 0, 0, "syntax", "@closing() requires channel argument(s)");
                    fprintf(stderr, "  hint: use '@closing(ch) { ... }' where ch is the channel to close\n");
                    free(out);
                    return NULL;
                }
                
                /* Skip whitespace after ) */
                while (j < n && (src[j] == ' ' || src[j] == '\t' || src[j] == '\n')) j++;
                
                if (j >= n) { i++; continue; }
                
                /* Now expect either 'spawn' or '{' */
                size_t body_start = j;
                size_t body_end;
                int is_block = 0;
                
                if (src[j] == '{') {
                    /* Block form: @closing(ch) { ... } */
                    is_block = 1;
                    int brace_depth = 1;
                    j++; /* skip '{' */
                    while (j < n && brace_depth > 0) {
                        if (src[j] == '{') brace_depth++;
                        else if (src[j] == '}') brace_depth--;
                        else if (src[j] == '"') {
                            j++;
                            while (j < n && src[j] != '"') {
                                if (src[j] == '\\' && j + 1 < n) j++;
                                j++;
                            }
                        } else if (src[j] == '\'') {
                            j++;
                            while (j < n && src[j] != '\'') {
                                if (src[j] == '\\' && j + 1 < n) j++;
                                j++;
                            }
                        }
                        j++;
                    }
                    body_end = j; /* after '}' */
                } else if (j + 5 < n && memcmp(src + j, "spawn", 5) == 0) {
                    /* Single spawn form: @closing(ch) spawn(...); */
                    is_block = 0;
                    /* Find the end of the spawn statement (semicolon or end of closure) */
                    int paren_d = 0;
                    int brace_d = 0;
                    while (j < n) {
                        if (src[j] == '(') paren_d++;
                        else if (src[j] == ')') paren_d--;
                        else if (src[j] == '{') brace_d++;
                        else if (src[j] == '}') {
                            brace_d--;
                            if (brace_d < 0) break; /* Hit outer brace */
                        }
                        else if (src[j] == ';' && paren_d == 0 && brace_d == 0) {
                            j++; /* include semicolon */
                            break;
                        }
                        else if (src[j] == '"') {
                            j++;
                            while (j < n && src[j] != '"') {
                                if (src[j] == '\\' && j + 1 < n) j++;
                                j++;
                            }
                        }
                        j++;
                    }
                    body_end = j;
                } else {
                    /* Unexpected - skip */
                    i++;
                    continue;
                }
                
                /* Emit everything up to start */
                cc_sb_append(&out, &out_len, &out_cap, src + last_emit, start - last_emit);
                
                /* Emit the transformed code:
                   spawn(() => { @nursery closing(channels) { body } }); */
                cc_sb_append_cstr(&out, &out_len, &out_cap, "spawn(() => { @nursery closing(");
                cc_sb_append(&out, &out_len, &out_cap, src + chans_start, chans_end - chans_start);
                cc_sb_append_cstr(&out, &out_len, &out_cap, ") ");
                
                if (is_block) {
                    /* Body already has braces */
                    cc_sb_append(&out, &out_len, &out_cap, src + body_start, body_end - body_start);
                } else {
                    /* Wrap single spawn in braces */
                    cc_sb_append_cstr(&out, &out_len, &out_cap, "{ ");
                    cc_sb_append(&out, &out_len, &out_cap, src + body_start, body_end - body_start);
                    cc_sb_append_cstr(&out, &out_len, &out_cap, " }");
                }
                
                cc_sb_append_cstr(&out, &out_len, &out_cap, " });");
                
                last_emit = body_end;
                i = body_end;
                continue;
            }
        }
        
        i++;
    }
    
    if (last_emit == 0) return NULL; /* No changes */
    if (last_emit < n) cc_sb_append(&out, &out_len, &out_cap, src + last_emit, n - last_emit);
    return out;
}

/* Check for cc_concurrent usage and emit error with migration guidance.
   cc_concurrent syntax is deprecated; use cc_block_all instead. */
static char* cc__rewrite_cc_concurrent(const char* src, size_t n) {
    if (!src || n == 0) return NULL;

    int in_lc = 0, in_bc = 0, in_str = 0, in_chr = 0;
    int line = 1;

    for (size_t i = 0; i < n; i++) {
        char c = src[i];
        char c2 = (i + 1 < n) ? src[i + 1] : 0;

        if (c == '\n') line++;

        if (in_lc) { if (c == '\n') in_lc = 0; continue; }
        if (in_bc) { if (c == '*' && c2 == '/') { in_bc = 0; i++; } continue; }
        if (in_str) { if (c == '\\' && i + 1 < n) { i++; continue; } if (c == '"') in_str = 0; continue; }
        if (in_chr) { if (c == '\\' && i + 1 < n) { i++; continue; } if (c == '\'') in_chr = 0; continue; }
        if (c == '/' && c2 == '/') { in_lc = 1; i++; continue; }
        if (c == '/' && c2 == '*') { in_bc = 1; i++; continue; }
        if (c == '"') { in_str = 1; continue; }
        if (c == '\'') { in_chr = 1; continue; }

        if (c == 'c' && i + 12 < n && memcmp(src + i, "cc_concurrent", 13) == 0) {
            if (i > 0 && cc_is_ident_char(src[i - 1])) continue;
            if (i + 13 < n && cc_is_ident_char(src[i + 13])) continue;

            cc_pp_error_cat("<input>", line, 1, "syntax", "'cc_concurrent' syntax is deprecated (use cc_block_all instead)");
            fprintf(stderr, "  note: use cc_block_all() instead:\n");
            fprintf(stderr, "    CCTaskIntptr tasks[] = { task1(), task2() };\n");
            fprintf(stderr, "    intptr_t results[2];\n");
            fprintf(stderr, "    cc_block_all(2, tasks, results);\n");
            return (char*)-1;
        }
    }

    return NULL;
}
int cc_preprocess_file(const char* input_path, char* out_path, size_t out_path_sz) {
    if (!input_path || !out_path || out_path_sz == 0) return -1;
    char tmp_path[] = "/tmp/cc_pp_XXXXXX.c";
    int fd = mkstemps(tmp_path, 2); /* keep .c suffix */
    if (fd < 0) return -1;

    /* Create type registry for this file (or reuse existing global) */
    CCTypeRegistry* reg = cc_type_registry_get_global();
    if (!reg) {
        reg = cc_type_registry_new();
        cc_type_registry_set_global(reg);
    }

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
        /* Check for cc_block_on with non-@nonblocking functions (warning only) */
        cc__check_block_on_nonblocking(buf, got, input_path);
    }

    /* --- Apply preprocessing passes using chain helper --- */
    CCPassChain chain;
    cc_pass_chain_init(&chain, buf, got);
    
    /* P1: with_deadline syntax */
    CC_CHAIN(chain, cc__rewrite_with_deadline_syntax(chain.src, chain.len));
    /* P2: @match syntax */
    CC_CHAIN(chain, cc__rewrite_match_syntax(chain.src, chain.len, input_path));
    /* P3: slice types T[:] -> CCSlice_T */
    CC_CHAIN(chain, cc__rewrite_slice_types(chain.src, chain.len, input_path));
    /* P4: channel handle types int[~4 >] -> CCChanTx_int */
    CC_CHAIN(chain, cc__rewrite_chan_handle_types(chain.src, chain.len, input_path));
    /* P5: generic containers Vec<T> -> Vec_T */
    CC_CHAIN(chain, cc_rewrite_generic_containers(chain.src, chain.len, input_path));
    /* P6: container UFCS v.push(x) -> Vec_T_push(&v, x) */
    CC_CHAIN(chain, cc_rewrite_ufcs_container_calls(chain.src, chain.len, input_path));
    /* P7: std I/O UFCS std_out.write() -> printf */
    CC_CHAIN(chain, cc_rewrite_std_io_ufcs(chain.src, chain.len));
    /* P8: optional types T? -> CCOptional_T */
    CC_CHAIN(chain, cc__rewrite_optional_types(chain.src, chain.len, input_path));
    /* P9: optional constructors for parser mode */
    CC_CHAIN(chain, cc__rewrite_optional_constructors(chain.src, chain.len));
    /* P10: inferred result constructors (MUST run before P11) */
    CC_CHAIN(chain, cc__rewrite_inferred_result_ctors(chain.src, chain.len));
    /* P11: result types T!>(E) -> CCResult_T_E */
    CC_CHAIN(chain, cc__rewrite_result_types(chain.src, chain.len, input_path));
    /* P12: result constructors for parser mode */
    CC_CHAIN(chain, cc__rewrite_result_constructors(chain.src, chain.len));
    /* P13: normalize if @try ( -> if (try */
    CC_CHAIN(chain, cc__normalize_if_try_syntax(chain.src, chain.len));
    /* P14: if (try T x = expr) -> expanded form */
    CC_CHAIN(chain, cc__rewrite_try_binding(chain.src, chain.len));
    /* P15: try expr -> cc_try(expr) */
    CC_CHAIN(chain, cc__rewrite_try_exprs(chain.src, chain.len));
    /* P16: optional unwrap *opt -> cc_unwrap_opt(opt) */
    CC_CHAIN(chain, cc__rewrite_optional_unwrap(chain.src, chain.len));
    /* P17: @closing(ch) -> sub-nursery */
    CC_CHAIN(chain, cc__rewrite_closing_annotation(chain.src, chain.len));
    /* P18: cc_concurrent { } -> closure exec */
    CC_CHAIN(chain, cc__rewrite_cc_concurrent(chain.src, chain.len));
    /* P19: @link("lib") -> linker comments */
    CC_CHAIN(chain, cc__rewrite_link_directives(chain.src, chain.len));
    
    const char* use = chain.src;

    /* Emit container type declarations from type registry */
    {
        size_t n_vec = cc_type_registry_vec_count(reg);
        size_t n_map = cc_type_registry_map_count(reg);
        
        if (n_vec > 0 || n_map > 0) {
            fprintf(out, "/* --- CC generic container declarations --- */\n");
            fprintf(out, "#include <ccc/std/vec.cch>\n");
            fprintf(out, "#include <ccc/std/map.cch>\n");
            /* Vec/Map declarations must be skipped in parser mode where they're
               already typedef'd to generic placeholders in vec.cch/map.cch */
            fprintf(out, "#ifndef CC_PARSER_MODE\n");
            
            /* Emit Vec declarations */
            for (size_t i = 0; i < n_vec; i++) {
                const CCTypeInstantiation* inst = cc_type_registry_get_vec(reg, i);
                if (inst && inst->type1 && inst->mangled_name) {
                    /* Extract mangled element name from Vec_xxx */
                    const char* mangled_elem = inst->mangled_name + 4; /* Skip "Vec_" */
                    
                    /* Skip Vec_char - it's predeclared in string.cch */
                    if (strcmp(mangled_elem, "char") == 0) {
                        continue;
                    }
                    
                    /* Check if type is complex (pointer, struct) - needs FULL macro with explicit optional */
                    int is_complex = (strchr(inst->type1, '*') != NULL || 
                                      strncmp(inst->type1, "struct ", 7) == 0 ||
                                      strncmp(inst->type1, "union ", 6) == 0);
                    if (is_complex) {
                        /* Check if this optional is already pre-declared in cc_optional.cch */
                        int opt_predeclared = (strcmp(mangled_elem, "charptr") == 0 ||
                                               strcmp(mangled_elem, "intptr") == 0 ||
                                               strcmp(mangled_elem, "voidptr") == 0);
                        if (!opt_predeclared) {
                            /* Emit optional type declaration first */
                            fprintf(out, "CC_DECL_OPTIONAL(CCOptional_%s, %s)\n", mangled_elem, inst->type1);
                        }
                        /* Use FULL macro with explicit optional type */
                        fprintf(out, "CC_VEC_DECL_ARENA_FULL(%s, %s, CCOptional_%s)\n", 
                                inst->type1, inst->mangled_name, mangled_elem);
                    } else {
                        fprintf(out, "CC_VEC_DECL_ARENA(%s, %s)\n", inst->type1, inst->mangled_name);
                    }
                }
            }
            
            /* Emit Map declarations (using default hash functions for known types) */
            for (size_t i = 0; i < n_map; i++) {
                const CCTypeInstantiation* inst = cc_type_registry_get_map(reg, i);
                if (inst && inst->type1 && inst->type2 && inst->mangled_name) {
                    /* Determine hash/eq functions based on key type */
                    const char* hash_fn = "cc_kh_hash_i32";
                    const char* eq_fn = "cc_kh_eq_i32";
                    if (strcmp(inst->type1, "int") == 0) {
                        hash_fn = "cc_kh_hash_i32"; eq_fn = "cc_kh_eq_i32";
                    } else if (strstr(inst->type1, "64") != NULL) {
                        hash_fn = "cc_kh_hash_u64"; eq_fn = "cc_kh_eq_u64";
                    } else if (strstr(inst->type1, "slice") != NULL || strcmp(inst->type1, "charslice") == 0) {
                        hash_fn = "cc_kh_hash_slice"; eq_fn = "cc_kh_eq_slice";
                    }
                    fprintf(out, "CC_MAP_DECL_ARENA(%s, %s, %s, %s, %s)\n", 
                            inst->type1, inst->type2, inst->mangled_name, hash_fn, eq_fn);
                }
            }
            
            fprintf(out, "#endif /* !CC_PARSER_MODE */\n");
            fprintf(out, "/* --- end container declarations --- */\n\n");
        }
    }

    /* If result types are used, include cc_result.cch with CC_PARSER_MODE
       so that __CC_RESULT(T, E) expands to __CCResultGeneric for TCC parsing. */
    if (cc__result_type_count > 0) {
        fprintf(out, "/* --- CC result type support --- */\n");
        fprintf(out, "#ifndef CC_PARSER_MODE\n");
        fprintf(out, "#define CC_PARSER_MODE 1\n");
        fprintf(out, "#endif\n");
        fprintf(out, "#include <ccc/cc_result.cch>\n");
        fprintf(out, "/* --- end result support --- */\n\n");
    }

    char rel[1024];
    fprintf(out, "#line 1 \"%s\"\n", cc_path_rel_to_repo(input_path, rel, sizeof(rel)));
    fputs(use, out);

    cc_pass_chain_free(&chain);
    free(buf);
    fclose(in);
    fclose(out);

    strncpy(out_path, tmp_path, out_path_sz - 1);
    out_path[out_path_sz - 1] = '\0';
    return 0;

chain_cleanup:
    /* Error path - cleanup allocations and return failure */
    cc_pass_chain_free(&chain);
    free(buf);
    fclose(in);
    fclose(out);
    unlink(tmp_path);
    return -1;
}

// Preprocess source string to output string (no temp files).
// skip_checks: if true, skip validation checks (use for reparse passes).
char* cc_preprocess_to_string_ex(const char* input, size_t input_len, const char* input_path, int skip_checks) {
    if (!input || input_len == 0) return NULL;

    /* Create type registry for this file (or reuse existing global) */
    CCTypeRegistry* reg = cc_type_registry_get_global();
    if (!reg) {
        reg = cc_type_registry_new();
        cc_type_registry_set_global(reg);
    }

    /* Make a copy of input since some rewrites modify in-place */
    char* buf = (char*)malloc(input_len + 1);
    if (!buf) return NULL;
    memcpy(buf, input, input_len);
    buf[input_len] = 0;
    size_t got = input_len;

    /* Check for unawaited channel ops in @async functions (before rewrites).
       Skip if requested (reparse passes) or if path looks like a temp file. */
    if (!skip_checks && input_path) {
        const char* basename = strrchr(input_path, '/');
        basename = basename ? basename + 1 : input_path;
        int is_temp_file = basename && (strncmp(basename, "cc_reparse_", 11) == 0 ||
                            strncmp(basename, "cc_pp_", 6) == 0 ||
                            strncmp(input_path, "/tmp/", 5) == 0);
        if (!is_temp_file) {
            int chan_err = cc__check_async_chan_await(buf, got, input_path);
            if (chan_err != 0) {
                free(buf);
                return NULL;
            }
            /* Check for cc_block_on with non-@nonblocking functions (warning only) */
            cc__check_block_on_nonblocking(buf, got, input_path);
        }
    }

    /* --- Apply preprocessing passes using chain helper --- */
    CCPassChain chain;
    cc_pass_chain_init(&chain, buf, got);
    
    /* P1: with_deadline syntax */
    CC_CHAIN(chain, cc__rewrite_with_deadline_syntax(chain.src, chain.len));
    /* P2: @match syntax */
    CC_CHAIN(chain, cc__rewrite_match_syntax(chain.src, chain.len, input_path));
    /* P3: slice types T[:] -> CCSlice_T */
    CC_CHAIN(chain, cc__rewrite_slice_types(chain.src, chain.len, input_path));
    /* P4: channel handle types int[~4 >] -> CCChanTx_int */
    CC_CHAIN(chain, cc__rewrite_chan_handle_types(chain.src, chain.len, input_path));
    /* P5: generic containers Vec<T> -> Vec_T */
    CC_CHAIN(chain, cc_rewrite_generic_containers(chain.src, chain.len, input_path));
    /* P6: container UFCS v.push(x) -> Vec_T_push(&v, x) */
    CC_CHAIN(chain, cc_rewrite_ufcs_container_calls(chain.src, chain.len, input_path));
    /* P7: std I/O UFCS std_out.write() -> printf */
    CC_CHAIN(chain, cc_rewrite_std_io_ufcs(chain.src, chain.len));
    /* P8: optional types T? -> CCOptional_T */
    CC_CHAIN(chain, cc__rewrite_optional_types(chain.src, chain.len, input_path));
    /* P9: optional constructors for parser mode */
    CC_CHAIN(chain, cc__rewrite_optional_constructors(chain.src, chain.len));
    /* P10: inferred result constructors (MUST run before P11) */
    CC_CHAIN(chain, cc__rewrite_inferred_result_ctors(chain.src, chain.len));
    /* P11: result types T!>(E) -> CCResult_T_E */
    CC_CHAIN(chain, cc__rewrite_result_types(chain.src, chain.len, input_path));
    /* P12: result constructors for parser mode */
    CC_CHAIN(chain, cc__rewrite_result_constructors(chain.src, chain.len));
    /* P13: normalize if @try ( -> if (try */
    CC_CHAIN(chain, cc__normalize_if_try_syntax(chain.src, chain.len));
    /* P14: if (try T x = expr) -> expanded form */
    CC_CHAIN(chain, cc__rewrite_try_binding(chain.src, chain.len));
    /* P15: try expr -> cc_try(expr) */
    CC_CHAIN(chain, cc__rewrite_try_exprs(chain.src, chain.len));
    /* P16: optional unwrap *opt -> cc_unwrap_opt(opt) */
    CC_CHAIN(chain, cc__rewrite_optional_unwrap(chain.src, chain.len));
    /* P17: @closing(ch) -> sub-nursery */
    CC_CHAIN(chain, cc__rewrite_closing_annotation(chain.src, chain.len));
    /* P18: cc_concurrent { } -> closure exec */
    CC_CHAIN(chain, cc__rewrite_cc_concurrent(chain.src, chain.len));
    /* P19: @link("lib") -> linker comments */
    CC_CHAIN(chain, cc__rewrite_link_directives(chain.src, chain.len));
    
    const char* use = chain.src;
    (void)chain.len;  /* use_n not needed here */

    /* Build output string using open_memstream (POSIX) */
    char* out_buf = NULL;
    size_t out_size = 0;
    FILE* out = open_memstream(&out_buf, &out_size);
    if (!out) {
        cc_pass_chain_free(&chain);
        free(buf);
        return NULL;
    }

    /* Emit container type declarations from type registry */
    {
        size_t n_vec = cc_type_registry_vec_count(reg);
        size_t n_map = cc_type_registry_map_count(reg);
        
        if (n_vec > 0 || n_map > 0) {
            fprintf(out, "/* --- CC generic container declarations --- */\n");
            fprintf(out, "#include <ccc/std/vec.cch>\n");
            fprintf(out, "#include <ccc/std/map.cch>\n");
            fprintf(out, "#ifndef CC_PARSER_MODE\n");
            
            /* Emit Vec declarations */
            for (size_t i = 0; i < n_vec; i++) {
                const CCTypeInstantiation* inst = cc_type_registry_get_vec(reg, i);
                if (inst && inst->type1 && inst->mangled_name) {
                    const char* mangled_elem = inst->mangled_name + 4; /* Skip "Vec_" */
                    if (strcmp(mangled_elem, "char") == 0) continue;
                    int is_complex = (strchr(inst->type1, '*') != NULL || 
                                      strncmp(inst->type1, "struct ", 7) == 0 ||
                                      strncmp(inst->type1, "union ", 6) == 0);
                    if (is_complex) {
                        int opt_predeclared = (strcmp(mangled_elem, "charptr") == 0 ||
                                               strcmp(mangled_elem, "intptr") == 0 ||
                                               strcmp(mangled_elem, "voidptr") == 0);
                        if (!opt_predeclared) {
                            fprintf(out, "CC_DECL_OPTIONAL(CCOptional_%s, %s)\n", mangled_elem, inst->type1);
                        }
                        fprintf(out, "CC_VEC_DECL_ARENA_FULL(%s, %s, CCOptional_%s)\n", 
                                inst->type1, inst->mangled_name, mangled_elem);
                    } else {
                        fprintf(out, "CC_VEC_DECL_ARENA(%s, %s)\n", inst->type1, inst->mangled_name);
                    }
                }
            }
            
            /* Emit Map declarations */
            for (size_t i = 0; i < n_map; i++) {
                const CCTypeInstantiation* inst = cc_type_registry_get_map(reg, i);
                if (inst && inst->type1 && inst->type2 && inst->mangled_name) {
                    const char* hash_fn = "cc_kh_hash_i32";
                    const char* eq_fn = "cc_kh_eq_i32";
                    if (strcmp(inst->type1, "int") == 0) {
                        hash_fn = "cc_kh_hash_i32"; eq_fn = "cc_kh_eq_i32";
                    } else if (strstr(inst->type1, "64") != NULL) {
                        hash_fn = "cc_kh_hash_u64"; eq_fn = "cc_kh_eq_u64";
                    } else if (strstr(inst->type1, "slice") != NULL || strcmp(inst->type1, "charslice") == 0) {
                        hash_fn = "cc_kh_hash_slice"; eq_fn = "cc_kh_eq_slice";
                    }
                    fprintf(out, "CC_MAP_DECL_ARENA(%s, %s, %s, %s, %s)\n", 
                            inst->type1, inst->type2, inst->mangled_name, hash_fn, eq_fn);
                }
            }
            
            fprintf(out, "#endif /* !CC_PARSER_MODE */\n");
            fprintf(out, "/* --- end container declarations --- */\n\n");
        }
    }

    /* If result types are used, include cc_result.cch and emit parse-time stubs for custom types */
    if (cc__result_type_count > 0) {
        fprintf(out, "/* --- CC result type support --- */\n");
        fprintf(out, "#ifndef CC_PARSER_MODE\n");
        fprintf(out, "#define CC_PARSER_MODE 1\n");
        fprintf(out, "#endif\n");
        fprintf(out, "#include <ccc/cc_result.cch>\n");
        /* Emit parse-time stub typedefs and constructors for user-defined Result types.
           Uses __CCResultGeneric so TCC can parse before actual types are defined.
           Constructor macros cast away values and call generic helpers.
           Real type expansion happens in codegen via CC_DECL_RESULT_SPEC. */
        for (size_t i = 0; i < cc__result_type_count; i++) {
            const char* ok = cc__result_types[i].mangled_ok;
            const char* err = cc__result_types[i].mangled_err;
            /* Emit parse-time stub typedef for ALL result types encountered.
               Use guards to handle any pre-existing definitions from CC_DECL_RESULT_SPEC.
               Don't emit constructor macros - CC_DECL_RESULT_SPEC will provide functions. */
            fprintf(out, "#ifndef CCResult_%s_%s_DEFINED\n", ok, err);
            fprintf(out, "#define CCResult_%s_%s_DEFINED 1\n", ok, err);
            fprintf(out, "typedef __CCResultGeneric CCResult_%s_%s;\n", ok, err);
            fprintf(out, "#endif\n");
        }
        fprintf(out, "/* --- end result support --- */\n\n");
    }

    char rel[1024];
    fprintf(out, "#line 1 \"%s\"\n", cc_path_rel_to_repo(input_path ? input_path : "<string>", rel, sizeof(rel)));
    fputs(use, out);
    fclose(out);

    cc_pass_chain_free(&chain);
    free(buf);
    return out_buf;

chain_cleanup:
    /* Error path - cleanup allocations and return failure */
    cc_pass_chain_free(&chain);
    free(buf);
    return NULL;
}

// Wrapper that runs all checks (default behavior for initial parse).
char* cc_preprocess_to_string(const char* input, size_t input_len, const char* input_path) {
    return cc_preprocess_to_string_ex(input, input_len, input_path, 0);
}

/* Inline parse-time stubs - minimal definitions for TCC to accept CC types.
   Uses guard macros so real CC headers can skip their own definitions. */
static const char* cc__parse_stubs = 
    "#ifndef CC_PARSE_STUBS_INLINE\n"
    "#define CC_PARSE_STUBS_INLINE 1\n"
    "#define CC_PARSER_MODE 1\n"
    /* Guard macros to prevent redefinition when real headers are included */
    "#define __CC_OPTIONAL_GENERIC_VALUE_DEFINED 1\n"
    "#define __CC_OPTIONAL_GENERIC_DEFINED 1\n"
    "#define __CC_GENERIC_ERROR_DEFINED 1\n"
    "#define __CC_RESULT_GENERIC_DEFINED 1\n"
    /* Struct definitions compatible with real headers' field layout.
       Use unsigned long instead of size_t since stddef.h may not be included yet. */
    "typedef struct __CCOptionalGenericValue { long x,y,z,w; void* ptr; void* data; unsigned long len; int id,kind,code; long _scalar; } __CCOptionalGenericValue;\n"
    "typedef struct __CCOptionalGeneric { int has; union { __CCOptionalGenericValue value; long _scalar; } u; } __CCOptionalGeneric;\n"
    "typedef struct __CCGenericError { int kind; int os_errno; int os_code; const char* message; } __CCGenericError;\n"
    "typedef struct __CCResultGeneric { int ok; union { long value; __CCGenericError error; } u; } __CCResultGeneric;\n"
    /* Channel handle stubs */
    "#define __CC_CHAN_TX_DEFINED 1\n"
    "#define __CC_CHAN_RX_DEFINED 1\n"
    "typedef struct CCChan CCChan;\n"
    "typedef struct { CCChan* raw; } CCChanTx;\n"
    "typedef struct { CCChan* raw; } CCChanRx;\n"
    /* Generic container stubs - define guards so real headers skip.
       Field counts must match real headers to allow their initializers. */
    "#define __CC_VEC_GENERIC_DEFINED 1\n"
    "#define __CC_MAP_GENERIC_DEFINED 1\n"
    "typedef struct { unsigned long len; unsigned long cap; void* data; void* arena; } __CCVecGeneric;\n"
    "typedef struct { unsigned long count; void* keys; void* vals; void* arena; } __CCMapGeneric;\n"
    "#define __CC_VEC(T) __CCVecGeneric\n"
    "#define __CC_MAP(K, V) __CCMapGeneric\n"
    "#define __CC_VEC_INIT(T, arena) ((__CCVecGeneric){0, 0, 0})\n"
    "#define __CC_MAP_INIT(K, V, arena) ((__CCMapGeneric){0, 0})\n"
    /* Common Vec/Map types */
    "typedef __CCVecGeneric Vec_int;\n"
    "typedef __CCVecGeneric Vec_char;\n"
    "typedef __CCVecGeneric Vec_float;\n"
    "typedef __CCMapGeneric Map_int_int;\n"
    "typedef __CCMapGeneric Map_charptr_int;\n"
    /* Key macros: rewritten T? -> __CC_OPTIONAL(T), T!>(E) -> __CC_RESULT(T,E) */
    "#define __CC_OPTIONAL(T) __CCOptionalGeneric\n"
    "#define __CC_RESULT(T, E) __CCResultGeneric\n"
    /* Typed optional constructors for rewriting cc_some_CCOptional_T -> __CC_OPTIONAL_SOME
       These need to be function calls (not compound literals) so TCC records them in the AST.
       Using comma expressions to type-check the value while returning through the function. */
    "#ifndef __CC_TYPED_OPT_CTORS_DEFINED\n"
    "#define __CC_TYPED_OPT_CTORS_DEFINED\n"
    "#define __CC_OPTIONAL_SOME(T, ...) __cc_optional_some_impl(__VA_ARGS__)\n"
    "#define __CC_OPTIONAL_NONE(T) __cc_optional_none_impl()\n"
    "#endif\n"
    "/* Helper functions - extern (not inline) so TCC records the calls */\n"
    "__CCOptionalGeneric __cc_optional_some_impl(long v);\n"
    "__CCOptionalGeneric __cc_optional_none_impl(void);\n"
    "__CCResultGeneric __cc_result_ok_impl(long v);\n"
    "__CCResultGeneric __cc_result_err_impl(int kind, const char* msg);\n"
    /* Common optional types (direct names, for explicit CCOptional_T usage) */
    "typedef __CCOptionalGeneric CCOptional_int;\n"
    "typedef __CCOptionalGeneric CCOptional_bool;\n"
    "typedef __CCOptionalGeneric CCOptional_char;\n"
    "typedef __CCOptionalGeneric CCOptional_size_t;\n"
    "typedef __CCOptionalGeneric CCOptional_voidptr;\n"
    "typedef __CCOptionalGeneric CCOptional_charptr;\n"
    /* Common result types */
    "typedef __CCResultGeneric CCResult_int_CCError;\n"
    "typedef __CCResultGeneric CCResult_bool_CCError;\n"
    "typedef __CCResultGeneric CCResult_void_CCError;\n"
    "typedef __CCResultGeneric CCResult_size_t_CCError;\n"
    "typedef __CCResultGeneric CCResult_charptr_CCError;\n"
    "typedef __CCResultGeneric CCResult_voidptr_CCError;\n"
    /* CCError type */
    "#ifndef __CC_ERROR_TYPE_DEFINED\n"
    "#define __CC_ERROR_TYPE_DEFINED\n"
    "typedef struct { int kind; const char* message; } CCError;\n"
    "#endif\n"
    /* cc_error helper */
    "#ifndef __CC_ERROR_HELPER_DEFINED\n"
    "#define __CC_ERROR_HELPER_DEFINED\n"
    "static inline CCError cc_error(int k, const char* m) { CCError e = {k, m}; return e; }\n"
    "#endif\n"
    /* Result constructors - variadic macros to accept any type.
       We use a comma expression to evaluate the arg (so it appears in AST) then return generic.
       cccn handles the real types in codegen. */
    "#ifndef __CC_RESULT_CTORS_DEFINED\n"
    "#define __CC_RESULT_CTORS_DEFINED\n"
    "#define cc_ok(v) ((void)(v), (__CCResultGeneric){.ok = 1})\n"
    "#define cc_err(...) ((__CCResultGeneric){.ok = 0})\n"
    "#endif\n"
    /* Optional constructors - same approach */
    "#ifndef __CC_OPT_CTORS_DEFINED\n"
    "#define __CC_OPT_CTORS_DEFINED\n"
    "#define cc_some(v) ((void)(v), (__CCOptionalGeneric){.has = 1})\n"
    "#define cc_none() ((__CCOptionalGeneric){.has = 0})\n"
    "#endif\n"
    /* Accessor macros */
    "#define cc_is_some(opt) ((opt).has)\n"
    "#define cc_is_none(opt) (!(opt).has)\n"
    "#define cc_is_ok(res) ((res).ok)\n"
    "#define cc_is_err(res) (!(res).ok)\n"
    "#define cc_unwrap(res) ((res).u.value)\n"
    "#define cc_unwrap_as(res, T) (*(T*)(void*)&(res).u.value)\n"
    "#define cc_unwrap_err(res) ((res).u.error)\n"
    "#define cc_unwrap_err_as(res, T) (*(T*)(void*)&(res).u.error)\n"
    "#define cc_unwrap_opt(opt) ((opt).u.value)\n"
    "#endif\n";

// Simple preprocessing for cccn: rewrites type syntax and adds parse-time stubs.
// Type syntax (T?, T!>(E), T[~N>], Vec<T>) is rewritten to C-compatible names.
// Parse-time stubs provide placeholder definitions.
// Other CC syntax (try, await, closures, etc.) is handled by TCC hooks and AST passes.
char* cc_preprocess_simple(const char* input, size_t input_len, const char* input_path) {
    if (!input || input_len == 0) return NULL;
    
    /* Reset type collectors */
    cc__optional_type_count = 0;
    cc__result_type_count = 0;
    
    /* Create type registry for this file (or reuse existing global) */
    CCTypeRegistry* reg = cc_type_registry_get_global();
    if (!reg) {
        reg = cc_type_registry_new();
        cc_type_registry_set_global(reg);
    }
    
    /* Chain of preprocessing passes - each takes previous output */
    const char* cur = input;
    size_t cur_len = input_len;
    char* buffers[16] = {0};
    int buf_idx = 0;
    
    /* Pass 0: Rewrite @link("lib") -> marker comments for linker */
    buffers[buf_idx] = cc__rewrite_link_directives(cur, cur_len);
    if (buffers[buf_idx]) {
        cur = buffers[buf_idx];
        cur_len = strlen(cur);
        buf_idx++;
    }
    
    /* Pass 1: Rewrite channel handle types T[~N >] -> CCChanTx, T[~N <] -> CCChanRx */
    buffers[buf_idx] = cc__rewrite_chan_handle_types(cur, cur_len, input_path);
    if (buffers[buf_idx]) {
        cur = buffers[buf_idx];
        cur_len = strlen(cur);
        buf_idx++;
    }
    
    /* Pass 2: Rewrite T? -> CCOptional_T */
    buffers[buf_idx] = cc__rewrite_optional_types(cur, cur_len, input_path);
    if (buffers[buf_idx]) {
        cur = buffers[buf_idx];
        cur_len = strlen(cur);
        buf_idx++;
    }
    
    /* Pass 2b: Rewrite cc_some_CCOptional_T(v) -> __CC_OPTIONAL_SOME(T, v) */
    buffers[buf_idx] = cc__rewrite_optional_constructors(cur, cur_len);
    if (buffers[buf_idx]) {
        cur = buffers[buf_idx];
        cur_len = strlen(cur);
        buf_idx++;
    }
    
    /* Pass 3: Infer result constructor types from enclosing function:
       cc_ok(x) -> cc_ok_CCResult_T_E(x) when inside a function returning T!E
       This must run BEFORE type rewrite so we can still detect T!E syntax. */
    buffers[buf_idx] = cc__rewrite_inferred_result_ctors(cur, cur_len);
    if (buffers[buf_idx]) {
        cur = buffers[buf_idx];
        cur_len = strlen(cur);
        buf_idx++;
    }
    
    /* Pass 3b: Rewrite cc_ok_CCResult_T_E(v) -> __cc_result_ok_impl(v) */
    buffers[buf_idx] = cc__rewrite_result_constructors(cur, cur_len);
    if (buffers[buf_idx]) {
        cur = buffers[buf_idx];
        cur_len = strlen(cur);
        buf_idx++;
    }
    
    /* Pass 3c: Rewrite T!>(E) -> CCResult_T_E */
    buffers[buf_idx] = cc__rewrite_result_types(cur, cur_len, input_path);
    if (buffers[buf_idx]) {
        cur = buffers[buf_idx];
        cur_len = strlen(cur);
        buf_idx++;
    }
    
    /* Pass 4: Rewrite slice types T[:] -> CCSlice */
    buffers[buf_idx] = cc__rewrite_slice_types(cur, cur_len, input_path);
    if (buffers[buf_idx]) {
        cur = buffers[buf_idx];
        cur_len = strlen(cur);
        buf_idx++;
    }
    
    /* Pass 5: Normalize if @try ( -> if (try  */
    buffers[buf_idx] = cc__normalize_if_try_syntax(cur, cur_len);
    if (buffers[buf_idx]) {
        cur = buffers[buf_idx];
        cur_len = strlen(cur);
        buf_idx++;
    }
    
    /* Pass 6: Rewrite if (try T x = expr) to expanded form */
    buffers[buf_idx] = cc__rewrite_try_binding(cur, cur_len);
    if (buffers[buf_idx]) {
        cur = buffers[buf_idx];
        cur_len = strlen(cur);
        buf_idx++;
    }
    
    /* Note: try expressions are handled natively by TCC (CC_AST_NODE_TRY) */
    
    /* Pass 7: Rewrite @defer(err) -> @defer */
    buffers[buf_idx] = cc__rewrite_defer_syntax(cur, cur_len);
    if (buffers[buf_idx]) {
        cur = buffers[buf_idx];
        cur_len = strlen(cur);
        buf_idx++;
    }
    
    /* Pass 8: Rewrite *opt_var and *result_var to unwrap calls */
    buffers[buf_idx] = cc__rewrite_optional_unwrap(cur, cur_len);
    if (buffers[buf_idx]) {
        cur = buffers[buf_idx];
        cur_len = strlen(cur);
        buf_idx++;
    }
    
    /* Pass 10: Rewrite Vec<T>/Map<K,V> -> Vec_T/Map_K_V */
    buffers[buf_idx] = cc_rewrite_generic_containers(cur, cur_len, input_path);
    if (buffers[buf_idx]) {
        cur = buffers[buf_idx];
        cur_len = strlen(cur);
        buf_idx++;
    }
    
    /* Pass 11: Rewrite UFCS container calls (v.push(x) -> Vec_int_push(&v, x)) */
    buffers[buf_idx] = cc_rewrite_ufcs_container_calls(cur, cur_len, input_path);
    if (buffers[buf_idx]) {
        cur = buffers[buf_idx];
        cur_len = strlen(cur);
        buf_idx++;
    }
    
    /* Build output with stubs and #line directive */
    char* out_buf = NULL;
    size_t out_size = 0;
    FILE* out = open_memstream(&out_buf, &out_size);
    if (!out) {
        for (int i = 0; i < buf_idx; i++) {
            if (buffers[i]) free(buffers[i]);
        }
        return NULL;
    }
    
    /* Prepend parse-time stubs (only once) */
    fputs(cc__parse_stubs, out);
    
    /* Note: User-defined Optional types (CCOptional_Point etc.) are emitted inline
       in the source at first use, so they have access to the actual type definition.
       This allows field access like p.u.value.x to work correctly. */
    
    /* Emit typedefs and constructor macros for user-defined Result types.
       In parser mode, we emit macros that cast away the value/error and call generic helpers.
       This allows type-checking to pass even though the actual types aren't fully defined. */
    for (size_t i = 0; i < cc__result_type_count; i++) {
        const char* ok = cc__result_types[i].mangled_ok;
        const char* err = cc__result_types[i].mangled_err;
        /* Emit typedef for ALL result types - use guards to handle pre-existing definitions.
           Don't emit constructor macros - CC_DECL_RESULT_SPEC will provide functions. */
        fprintf(out, "#ifndef CCResult_%s_%s_DEFINED\n", ok, err);
        fprintf(out, "#define CCResult_%s_%s_DEFINED 1\n", ok, err);
        fprintf(out, "typedef __CCResultGeneric CCResult_%s_%s;\n", ok, err);
        fprintf(out, "#endif\n");
    }
    
    /* Add #line directive for source mapping */
    char rel[1024];
    fprintf(out, "#line 1 \"%s\"\n", cc_path_rel_to_repo(input_path ? input_path : "<string>", rel, sizeof(rel)));
    
    /* Output processed source */
    fwrite(cur, 1, cur_len, out);
    
    fclose(out);
    
    /* Clean up intermediate buffers */
    for (int i = 0; i < buf_idx; i++) {
        if (buffers[i]) free(buffers[i]);
    }
    
    return out_buf;
}