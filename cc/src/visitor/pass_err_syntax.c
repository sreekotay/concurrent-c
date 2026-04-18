#include "pass_err_syntax.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util/path.h"
#include "util/text.h"
#include "visitor/pass_common.h"
#include "visitor/visitor.h"

#define cc__append_n cc_sb_append
#define cc__append_str cc_sb_append_cstr
CC_DEFINE_SB_APPEND_FMT

typedef struct {
    int reg_depth;
    char* body;
    size_t body_len;
    char param_decl[192];
    char param_name[64];
} CCErrFrame;

#define CC_ERR_STK_MAX 64

static void cc__err_frame_free(CCErrFrame* f) {
    if (!f) return;
    free(f->body);
    f->body = NULL;
    f->body_len = 0;
}

static int cc__token_is(const char* s, size_t len, size_t i, const char* tok) {
    size_t tn = strlen(tok);
    if (i + tn > len) return 0;
    if (memcmp(s + i, tok, tn) != 0) return 0;
    if (i > 0 && cc_is_ident_char(s[i - 1])) return 0;
    if (i + tn < len && cc_is_ident_char(s[i + tn])) return 0;
    return 1;
}

static int cc__at_err_postfix(const char* s, size_t len, size_t i) {
    if (i + 4 > len || s[i] != '@') return 0;
    if (s[i + 1] != 'e' || s[i + 2] != 'r' || s[i + 3] != 'r') return 0;
    if (i + 11 <= len && memcmp(s + i, "@errhandler", 11) == 0 &&
        (i + 11 >= len || !cc_is_ident_char(s[i + 11])))
        return 0;
    if (i + 4 < len && cc_is_ident_char(s[i + 4])) return 0;
    if (i > 0 && cc_is_ident_char(s[i - 1])) return 0;
    /* Skip the new-surface `@err(IDENT);` forwarding form: the legacy
     * `@err (DECL) { BODY }` requires `{` after the closing `)`, while the
     * new-surface `@err(IDENT);` uses `;`.  Peek past a balanced `(...)`
     * at `@err` and, if the next non-ws byte is `;`, leave it to
     * `pass_result_unwrap.c` (which handles `@err(IDENT);` inside `!>`
     * bodies as a structured control-flow forward). */
    {
        size_t k = i + 4;
        while (k < len && (s[k] == ' ' || s[k] == '\t' || s[k] == '\r' ||
                           s[k] == '\n'))
            k++;
        if (k < len && s[k] == '(') {
            int dp = 1;
            size_t j = k + 1;
            while (j < len && dp > 0) {
                char ch = s[j];
                if (ch == '(') dp++;
                else if (ch == ')') dp--;
                j++;
            }
            if (dp == 0) {
                size_t m = j;
                while (m < len && (s[m] == ' ' || s[m] == '\t' ||
                                   s[m] == '\r' || s[m] == '\n'))
                    m++;
                if (m < len && s[m] == ';') return 0;
            }
        }
    }
    return 1;
}

static int cc__scan_stmt_end_semicolon(const char* s, size_t len, size_t i, size_t* out_end) {
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
            if (ch == '*' && i + 1 < len && s[i + 1] == '/') {
                in_block_comment = 0;
                i++;
            }
            continue;
        }
        if (in_str) {
            if (ch == '\\' && i + 1 < len) {
                i++;
                continue;
            }
            if (ch == qch) in_str = 0;
            continue;
        }
        if (ch == '/' && i + 1 < len && s[i + 1] == '/') {
            in_line_comment = 1;
            i++;
            continue;
        }
        if (ch == '/' && i + 1 < len && s[i + 1] == '*') {
            in_block_comment = 1;
            i++;
            continue;
        }
        if (ch == '"' || ch == '\'') {
            in_str = 1;
            qch = ch;
            continue;
        }
        if (ch == '(') par++;
        else if (ch == ')') {
            if (par) par--;
        } else if (ch == '[')
            brk++;
        else if (ch == ']') {
            if (brk) brk--;
        } else if (ch == '{')
            br++;
        else if (ch == '}') {
            if (br) br--;
        } else if (ch == ';' && par == 0 && brk == 0 && br == 0) {
            if (out_end) *out_end = i + 1;
            return 1;
        }
    }
    return 0;
}

static int cc__find_matching_brace_text(const char* s, size_t len, size_t open_i, size_t* out_close) {
    int depth = 1;
    int in_str = 0;
    char qch = 0;
    int in_line_comment = 0;
    int in_block_comment = 0;
    if (!s || open_i >= len || s[open_i] != '{') return 0;
    for (size_t i = open_i + 1; i < len; i++) {
        char ch = s[i];
        if (in_line_comment) {
            if (ch == '\n') in_line_comment = 0;
            continue;
        }
        if (in_block_comment) {
            if (ch == '*' && i + 1 < len && s[i + 1] == '/') {
                in_block_comment = 0;
                i++;
            }
            continue;
        }
        if (in_str) {
            if (ch == '\\' && i + 1 < len) {
                i++;
                continue;
            }
            if (ch == qch) in_str = 0;
            continue;
        }
        if (ch == '/' && i + 1 < len && s[i + 1] == '/') {
            in_line_comment = 1;
            i++;
            continue;
        }
        if (ch == '/' && i + 1 < len && s[i + 1] == '*') {
            in_block_comment = 1;
            i++;
            continue;
        }
        if (ch == '"' || ch == '\'') {
            in_str = 1;
            qch = ch;
            continue;
        }
        if (ch == '{') depth++;
        else if (ch == '}') {
            depth--;
            if (depth == 0) {
                if (out_close) *out_close = i;
                return 1;
            }
        }
    }
    return 0;
}

/* Backward stmt start may be before a (lexically prior) @errhandler; skip those input spans. */
static size_t cc__stmt_start_after_leading_errhandlers(const char* s, size_t n, size_t stmt_start, size_t err_at) {
    size_t i = stmt_start;
    while (i < err_at) {
        size_t j = i;
        while (j < err_at && isspace((unsigned char)s[j])) j++;
        if (j >= err_at) break;
        /* Real statement starts at j; do not return stale i (still end of prior @errhandler). */
        if (!cc__token_is(s, n, j, "@errhandler")) return j;
        size_t k = j + 11;
        while (k < n && isspace((unsigned char)s[k])) k++;
        if (k >= n || s[k] != '(') break;
        int dp = 1;
        k++;
        while (k < n && dp > 0) {
            if (s[k] == '(') dp++;
            else if (s[k] == ')') dp--;
            k++;
        }
        if (dp != 0) break;
        while (k < n && isspace((unsigned char)s[k])) k++;
        if (k >= n || s[k] != '{') break;
        size_t bclose = 0;
        if (!cc__find_matching_brace_text(s, n, k, &bclose)) break;
        size_t stmt_end = bclose + 1;
        while (stmt_end < n && isspace((unsigned char)s[stmt_end])) stmt_end++;
        if (stmt_end < n && s[stmt_end] == ';') stmt_end++;
        i = stmt_end;
    }
    return i > err_at ? stmt_start : i;
}

/* Statement start: scan backward for ';' or block '{' at paren/bracket/brace depth 0 (strings skipped).
 * Brace depth: when scanning backward, '}' increases nesting (still inside that block); '{' decreases.
 * Without this, a ';' inside a preceding @errhandler { ... } is mistaken for the boundary before @err. */
static size_t cc__err_stmt_start_backward(const char* s, size_t err_at) {
    int par = 0, brk = 0;
    int br = 0;
    int in_str = 0;
    char qch = 0;
    size_t i = err_at;
    while (i > 0) {
        i--;
        char c = s[i];
        if (in_str) {
            if (c == '\\' && i > 0) {
                i--;
                continue;
            }
            if (c == qch) in_str = 0;
            continue;
        }
        if (c == '"' || c == '\'') {
            in_str = 1;
            qch = c;
            continue;
        }
        if (c == ')')
            par++;
        else if (c == '(') {
            if (par) par--;
        } else if (c == ']')
            brk++;
        else if (c == '[') {
            if (brk) brk--;
        } else if (par == 0 && brk == 0) {
            if (c == '}') {
                br++;
            } else if (c == '{') {
                if (br)
                    br--;
                else
                    return i + 1;
            } else if (c == ';' && br == 0) {
                return i + 1;
            }
        }
    }
    return 0;
}

static void cc__trim_slice(const char* s, size_t a, size_t b, size_t* out_a, size_t* out_b) {
    while (a < b && isspace((unsigned char)s[a])) a++;
    while (b > a && isspace((unsigned char)s[b - 1])) b--;
    *out_a = a;
    *out_b = b;
}

static void cc__extract_param_name(const char* decl, char* name, size_t nc) {
    size_t L = strnlen(decl, sizeof(((CCErrFrame*)0)->param_decl));
    while (L > 0 && isspace((unsigned char)decl[L - 1])) L--;
    size_t start = L;
    while (start > 0 && cc_is_ident_char(decl[start - 1])) start--;
    if (L > start && L - start < nc) {
        memcpy(name, decl + start, L - start);
        name[L - start] = 0;
    } else {
        name[0] = 0;
    }
}

static int cc__lhs_is_decl_like(const char* s, size_t a, size_t b, char* name, size_t nc) {
    size_t id_a = a, id_b = b;
    cc__trim_slice(s, a, b, &a, &b);
    if (a >= b || !cc_is_ident_char(s[b - 1])) return 0;
    id_b = b;
    id_a = b;
    while (id_a > a && cc_is_ident_char(s[id_a - 1])) id_a--;
    if (id_a == id_b) return 0;

    size_t q = id_a;
    while (q > a && isspace((unsigned char)s[q - 1])) q--;
    if (q == a) return 0;

    char prev = s[q - 1];
    if (prev == '.' || prev == '>' || prev == ']' || prev == '[' || prev == '(') return 0;

    int has_typeish_prefix = 0;
    for (size_t i = a; i < q; i++) {
        if (cc_is_ident_start(s[i])) {
            has_typeish_prefix = 1;
            break;
        }
    }
    if (!has_typeish_prefix) return 0;

    if (name) {
        size_t nl = id_b - id_a;
        if (nl >= nc) return 0;
        memcpy(name, s + id_a, nl);
        name[nl] = 0;
    }
    return 1;
}

static int cc__decl_slice_is_result_type(const char* s, size_t n) {
    if (!s || n == 0) return 0;
    for (size_t i = 0; i + 2 <= n; i++) {
        if (s[i] == '!' && s[i + 1] == '>') return 1;
    }
    for (size_t i = 0; i + 9 <= n; i++) {
        if (memcmp(s + i, "CCResult_", 9) == 0) return 1;
    }
    return 0;
}

static char* cc__subst_word(const char* body, size_t bl, const char* from, const char* to, size_t* out_len) {
    if (!from || !from[0] || !to) {
        char* r = (char*)malloc(bl + 1);
        if (!r) return NULL;
        memcpy(r, body, bl);
        r[bl] = 0;
        if (out_len) *out_len = bl;
        return r;
    }
    size_t fn = strlen(from), tn = strlen(to);
    char* r = NULL;
    size_t rl = 0, rc = 0;
    size_t i = 0;
    while (i < bl) {
        if (i + fn <= bl && memcmp(body + i, from, fn) == 0 &&
            (i == 0 || !cc_is_ident_char(body[i - 1])) && (i + fn >= bl || !cc_is_ident_char(body[i + fn]))) {
            cc__append_n(&r, &rl, &rc, to, tn);
            i += fn;
        } else {
            cc__append_n(&r, &rl, &rc, body + i, 1);
            i++;
        }
    }
    cc__append_n(&r, &rl, &rc, "", 1);
    rl--;
    if (out_len) *out_len = rl;
    return r;
}

static char* cc__expand_delegations(const char* body,
                                    size_t blen,
                                    const char* inner_param,
                                    const CCErrFrame* outer,
                                    size_t* out_len,
                                    int* ok) {
    if (!body) {
        *ok = 0;
        return NULL;
    }
    char* acc = NULL;
    size_t al = 0, ac = 0;
    int in_str = 0;
    char qch = 0;
    int in_line_comment = 0;
    int in_block_comment = 0;
    *ok = 1;
    for (size_t i = 0; i < blen; i++) {
        char ch = body[i];
        if (in_line_comment) {
            cc__append_n(&acc, &al, &ac, body + i, 1);
            if (ch == '\n') in_line_comment = 0;
            continue;
        }
        if (in_block_comment) {
            cc__append_n(&acc, &al, &ac, body + i, 1);
            if (ch == '*' && i + 1 < blen && body[i + 1] == '/') {
                cc__append_n(&acc, &al, &ac, body + i + 1, 1);
                i++;
                in_block_comment = 0;
            }
            continue;
        }
        if (in_str) {
            cc__append_n(&acc, &al, &ac, body + i, 1);
            if (ch == '\\' && i + 1 < blen) {
                cc__append_n(&acc, &al, &ac, body + i + 1, 1);
                i++;
                continue;
            }
            if (ch == qch) in_str = 0;
            continue;
        }
        if (ch == '/' && i + 1 < blen && body[i + 1] == '/') {
            cc__append_n(&acc, &al, &ac, body + i, 2);
            i++;
            in_line_comment = 1;
            continue;
        }
        if (ch == '/' && i + 1 < blen && body[i + 1] == '*') {
            cc__append_n(&acc, &al, &ac, body + i, 2);
            i++;
            in_block_comment = 1;
            continue;
        }
        if (ch == '"' || ch == '\'') {
            cc__append_n(&acc, &al, &ac, body + i, 1);
            in_str = 1;
            qch = ch;
            continue;
        }

        if (ch == '@' && i + 11 <= blen && memcmp(body + i, "@errhandler", 11) == 0 &&
            (i + 11 >= blen || !cc_is_ident_char(body[i + 11]))) {
            size_t j = i + 11;
            while (j < blen && isspace((unsigned char)body[j])) j++;
            if (j < blen && body[j] == '(') {
                size_t close = j;
                int d = 1;
                close++;
                while (close < blen && d > 0) {
                    if (body[close] == '(') d++;
                    else if (body[close] == ')')
                        d--;
                    close++;
                }
                if (d != 0) {
                    *ok = 0;
                    free(acc);
                    return NULL;
                }
                while (close < blen && isspace((unsigned char)body[close])) close++;
                if (close < blen && body[close] == ';') {
                    close++;
                    if (!inner_param || !inner_param[0]) {
                        *ok = 0;
                        free(acc);
                        return NULL;
                    }
                    if (!outer) {
                        *ok = 0;
                        free(acc);
                        return NULL;
                    }
                    char* obody = cc__subst_word(outer->body, outer->body_len, outer->param_name, inner_param, NULL);
                    if (!obody) {
                        *ok = 0;
                        free(acc);
                        return NULL;
                    }
                    cc__append_str(&acc, &al, &ac, "{ ");
                    cc__append_str(&acc, &al, &ac, obody);
                    cc__append_str(&acc, &al, &ac, " } ");
                    free(obody);
                    i = close - 1;
                    continue;
                }
            }
        }
        cc__append_n(&acc, &al, &ac, body + i, 1);
    }
    if (out_len) *out_len = al;
    return acc;
}

static int cc__rewrite_colon_defaults(const CCVisitorCtx* ctx, const char* s, size_t n, char** out, size_t* out_len);
static int cc__rewrite_err_core(const CCVisitorCtx* ctx, const char* in, size_t n, char** out, size_t* out_len);

int cc__rewrite_err_syntax(const CCVisitorCtx* ctx, const char* in_src, size_t in_len, char** out_src,
                           size_t* out_len) {
    if (!out_src || !out_len) return 0;
    *out_src = NULL;
    *out_len = 0;
    if (!in_src || in_len == 0) return 0;

    char* cur = (char*)malloc(in_len + 1);
    if (!cur) return -1;
    memcpy(cur, in_src, in_len);
    cur[in_len] = 0;
    size_t curlen = in_len;
    int any = 0;
    for (int round = 0; round < 8; round++) {
        char* next = NULL;
        size_t nl = 0;
        int r = cc__rewrite_colon_defaults(ctx, cur, curlen, &next, &nl);
        if (r < 0) {
            free(cur);
            return -1;
        }
        if (r > 0 && next) {
            free(cur);
            cur = next;
            curlen = nl;
            any = 1;
            continue;
        }
        r = cc__rewrite_err_core(ctx, cur, curlen, &next, &nl);
        if (r < 0) {
            free(cur);
            return -1;
        }
        if (r > 0 && next) {
            free(cur);
            cur = next;
            curlen = nl;
            any = 1;
            continue;
        }
        break;
    }
    if (!any) {
        free(cur);
        return 0;
    }
    *out_src = cur;
    *out_len = curlen;
    return 1;
}

static void cc__line_from_pos(const char* s, size_t pos, int* line) {
    int ln = 1;
    for (size_t i = 0; i < pos && s[i]; i++) {
        if (s[i] == '\n') ln++;
    }
    *line = ln;
}

static int cc__rewrite_colon_defaults(const CCVisitorCtx* ctx, const char* s, size_t n, char** out, size_t* out_len) {
    (void)ctx;
    char* r = NULL;
    size_t rl = 0, rc = 0;
    size_t copy_from = 0;
    int changed = 0;
    int in_str = 0;
    char qch = 0;
    int in_line_comment = 0;
    int in_block_comment = 0;
    size_t i = 0;

    while (i < n) {
        char ch = s[i];
        if (in_line_comment) {
            if (ch == '\n') in_line_comment = 0;
            i++;
            continue;
        }
        if (in_block_comment) {
            if (ch == '*' && i + 1 < n && s[i + 1] == '/') {
                in_block_comment = 0;
                i += 2;
            } else
                i++;
            continue;
        }
        if (in_str) {
            if (ch == '\\' && i + 1 < n) {
                i += 2;
                continue;
            }
            if (ch == qch) in_str = 0;
            i++;
            continue;
        }
        if (ch == '/' && i + 1 < n && s[i + 1] == '/') {
            in_line_comment = 1;
            i += 2;
            continue;
        }
        if (ch == '/' && i + 1 < n && s[i + 1] == '*') {
            in_block_comment = 1;
            i += 2;
            continue;
        }
        if (ch == '"' || ch == '\'') {
            in_str = 1;
            qch = ch;
            i++;
            continue;
        }

        size_t op_at = 0;
        size_t op_len = 0;
        if (i + 3 <= n && s[i] == '=' && s[i + 1] == '<' && s[i + 2] == '!' && (i == 0 || s[i - 1] != '<')) {
            op_at = i;
            op_len = 3;
        } else if (i + 2 <= n && s[i] == '<' && s[i + 1] == '?' && (i == 0 || s[i - 1] != '<')) {
            op_at = i;
            op_len = 2;
        }
        if (op_len != 0) {
            size_t stmt_start = cc__err_stmt_start_backward(s, op_at);
            size_t semi = 0;
            if (!cc__scan_stmt_end_semicolon(s, n, op_at, &semi)) {
                i++;
                continue;
            }
            if (stmt_start < semi && memchr(s + stmt_start, '@', semi - stmt_start) &&
                strstr(s + stmt_start, "@err") && !strstr(s + stmt_start, "@errhandler")) {
                i++;
                continue;
            }

            size_t eq = op_at;
            size_t lhs_a, lhs_b, rest_a, rest_b;
            cc__trim_slice(s, stmt_start, eq, &lhs_a, &lhs_b);
            size_t after = eq + op_len;
            while (after < semi && isspace((unsigned char)s[after])) after++;

            int par = 0;
            size_t colon_pos = semi;
            for (size_t j = after; j < semi; j++) {
                if (s[j] == '(')
                    par++;
                else if (s[j] == ')' && par)
                    par--;
                else if (s[j] == ':' && par == 0) {
                    colon_pos = j;
                    break;
                }
            }
            if (colon_pos >= semi) {
                i++;
                continue;
            }

            cc__trim_slice(s, after, colon_pos, &rest_a, &rest_b);
            size_t def_a = colon_pos + 1, def_b = (semi > 0 ? semi - 1 : semi);
            cc__trim_slice(s, def_a, def_b, &def_a, &def_b);
            if (rest_b <= rest_a || def_b <= def_a) {
                i++;
                continue;
            }

            static int g_colon_id = 0;
            int cid = ++g_colon_id;
            cc__append_n(&r, &rl, &rc, s + copy_from, stmt_start - copy_from);
            {
                char lhs_one[256];
                char assignee[64] = {0};
                int lhs_is_decl = 0;
                size_t lhl = lhs_b - lhs_a;
                if (lhl < sizeof(lhs_one)) {
                    memcpy(lhs_one, s + lhs_a, lhl);
                    lhs_one[lhl] = 0;
                    lhs_is_decl = cc__lhs_is_decl_like(lhs_one, 0, lhl, assignee, sizeof(assignee));
                }
                if (lhs_is_decl) {
                    cc__append_n(&r, &rl, &rc, s + lhs_a, lhs_b - lhs_a);
                    cc__append_str(&r, &rl, &rc, "; ");
                    cc_sb_append_fmt(&r, &rl, &rc, "{ __typeof__(%.*s) __cc_ed_%d = (%.*s); %s = cc_is_ok(__cc_ed_%d) ? "
                                                "cc_value(__cc_ed_%d) : (%.*s); }",
                                     (int)(rest_b - rest_a), s + rest_a, cid, (int)(rest_b - rest_a), s + rest_a,
                                     assignee, cid, cid, (int)(def_b - def_a), s + def_a);
                } else {
                    cc_sb_append_fmt(&r, &rl, &rc, "{ __typeof__(%.*s) __cc_ed_%d = (%.*s); %.*s = cc_is_ok(__cc_ed_%d) ? "
                                                "cc_value(__cc_ed_%d) : (%.*s); }",
                                     (int)(rest_b - rest_a), s + rest_a, cid, (int)(rest_b - rest_a), s + rest_a,
                                     (int)(lhs_b - lhs_a), s + lhs_a, cid, cid, (int)(def_b - def_a), s + def_a);
                }
            }
            copy_from = semi;
            i = semi;
            changed = 1;
            continue;
        }
        i++;
    }
    if (!changed) {
        free(r);
        return 0;
    }
    cc__append_n(&r, &rl, &rc, s + copy_from, n - copy_from);
    *out = r;
    *out_len = rl;
    return 1;
}

static int cc__rewrite_err_core(const CCVisitorCtx* ctx, const char* in_src, size_t in_len, char** out_src,
                                size_t* out_len) {
    CCErrFrame stk[CC_ERR_STK_MAX];
    int stk_n = 0;
    int depth = 0;
    int line_no = 1;
    int in_str = 0;
    char qch = 0;
    int in_line_comment = 0;
    int in_block_comment = 0;

    char* out = NULL;
    size_t ol = 0, oc = 0;
    int changed = 0;
    static int g_err_id = 0;
    size_t* ito = (size_t*)calloc(in_len + 1, sizeof(size_t));
    if (!ito) goto fail;

    for (size_t i = 0; i < in_len; i++) {
        ito[i] = ol;
        char ch = in_src[i];
        if (ch == '\n') line_no++;

        if (in_line_comment) {
            cc__append_n(&out, &ol, &oc, &ch, 1);
            if (ch == '\n') in_line_comment = 0;
            continue;
        }
        if (in_block_comment) {
            cc__append_n(&out, &ol, &oc, &ch, 1);
            if (ch == '*' && i + 1 < in_len && in_src[i + 1] == '/') {
                cc__append_n(&out, &ol, &oc, &in_src[i + 1], 1);
                i++;
                in_block_comment = 0;
            }
            continue;
        }
        if (in_str) {
            cc__append_n(&out, &ol, &oc, &ch, 1);
            if (ch == '\\' && i + 1 < in_len) {
                cc__append_n(&out, &ol, &oc, &in_src[i + 1], 1);
                i++;
                continue;
            }
            if (ch == qch) in_str = 0;
            continue;
        }
        if (ch == '/' && i + 1 < in_len && in_src[i + 1] == '/') {
            cc__append_n(&out, &ol, &oc, &in_src[i], 2);
            i++;
            in_line_comment = 1;
            continue;
        }
        if (ch == '/' && i + 1 < in_len && in_src[i + 1] == '*') {
            cc__append_n(&out, &ol, &oc, &in_src[i], 2);
            i++;
            in_block_comment = 1;
            continue;
        }
        if (ch == '"' || ch == '\'') {
            cc__append_n(&out, &ol, &oc, &ch, 1);
            in_str = 1;
            qch = ch;
            continue;
        }

        if (cc__token_is(in_src, in_len, i, "@errhandler")) {
            int eh_line = line_no;
            size_t j = i + 11;
            while (j < in_len && isspace((unsigned char)in_src[j])) j++;
            if (j >= in_len || in_src[j] != '(') {
                char rel[1024];
                const char* f =
                    cc_path_rel_to_repo(ctx && ctx->input_path ? ctx->input_path : "<input>", rel, sizeof(rel));
                cc_pass_error_cat(f, eh_line, 1, CC_ERR_SYNTAX, "expected '(' after @errhandler");
                goto fail;
            }
            size_t p0 = j;
            int dp = 1;
            j++;
            while (j < in_len && dp > 0) {
                if (in_src[j] == '(') dp++;
                else if (in_src[j] == ')')
                    dp--;
                j++;
            }
            if (dp != 0) {
                char rel[1024];
                const char* f =
                    cc_path_rel_to_repo(ctx && ctx->input_path ? ctx->input_path : "<input>", rel, sizeof(rel));
                cc_pass_error_cat(f, eh_line, 1, CC_ERR_SYNTAX, "unclosed '(' in @errhandler");
                goto fail;
            }
            size_t p1 = j - 1;
            while (j < in_len && isspace((unsigned char)in_src[j])) j++;
            if (j >= in_len || in_src[j] != '{') {
                char rel[1024];
                const char* f =
                    cc_path_rel_to_repo(ctx && ctx->input_path ? ctx->input_path : "<input>", rel, sizeof(rel));
                cc_pass_error_cat(f, eh_line, 1, CC_ERR_SYNTAX, "expected '{' after @errhandler(...)");
                goto fail;
            }
            size_t bopen = j;
            size_t bclose = 0;
            if (!cc__find_matching_brace_text(in_src, in_len, bopen, &bclose)) {
                char rel[1024];
                const char* f =
                    cc_path_rel_to_repo(ctx && ctx->input_path ? ctx->input_path : "<input>", rel, sizeof(rel));
                cc_pass_error_cat(f, eh_line, 1, CC_ERR_SYNTAX, "unclosed '{' in @errhandler body");
                goto fail;
            }
            size_t stmt_end = bclose + 1;
            while (stmt_end < in_len && isspace((unsigned char)in_src[stmt_end])) stmt_end++;
            if (stmt_end < in_len && in_src[stmt_end] == ';') stmt_end++;

            if (stk_n >= CC_ERR_STK_MAX) {
                char rel[1024];
                const char* f =
                    cc_path_rel_to_repo(ctx && ctx->input_path ? ctx->input_path : "<input>", rel, sizeof(rel));
                cc_pass_error_cat(f, eh_line, 1, CC_ERR_SYNTAX, "@errhandler stack overflow (too many nested)");
                goto fail;
            }

            CCErrFrame* fr = &stk[stk_n];
            memset(fr, 0, sizeof(*fr));
            fr->reg_depth = depth;
            {
                size_t decl_a = p0 + 1, decl_b = p1;
                cc__trim_slice(in_src, decl_a, decl_b, &decl_a, &decl_b);
                size_t dl = decl_b - decl_a;
                if (dl >= sizeof(fr->param_decl)) dl = sizeof(fr->param_decl) - 1;
                memcpy(fr->param_decl, in_src + decl_a, dl);
                fr->param_decl[dl] = 0;
                cc__extract_param_name(fr->param_decl, fr->param_name, sizeof(fr->param_name));
            }
            size_t inner_a = bopen + 1, inner_b = bclose;
            cc__trim_slice(in_src, inner_a, inner_b, &inner_a, &inner_b);
            fr->body_len = inner_b - inner_a;
            fr->body = (char*)malloc(fr->body_len + 1);
            if (!fr->body) goto fail;
            memcpy(fr->body, in_src + inner_a, fr->body_len);
            fr->body[fr->body_len] = 0;
            stk_n++;

            for (size_t k = i; k < stmt_end; k++) {
                ito[k] = ol;
                if (in_src[k] == '\n') {
                    cc__append_n(&out, &ol, &oc, "\n", 1);
                    line_no++;
                }
            }
            ito[stmt_end] = ol;
            changed = 1;
            i = stmt_end - 1;
            continue;
        }

        if (cc__at_err_postfix(in_src, in_len, i)) {
            size_t err_at = i;
            size_t stmt_start = cc__err_stmt_start_backward(in_src, err_at);
            stmt_start = cc__stmt_start_after_leading_errhandlers(in_src, in_len, stmt_start, err_at);
            int errl = 0;
            cc__line_from_pos(in_src, err_at, &errl);

            size_t k = err_at + 4;
            while (k < in_len && isspace((unsigned char)in_src[k])) k++;

            char local_param[64] = {0};
            char local_decl[192] = {0};
            char* local_body = NULL;
            size_t local_body_len = 0;
            int has_local = 0;

            if (k < in_len && in_src[k] == '(') {
                size_t p0 = k;
                int dp = 1;
                k++;
                while (k < in_len && dp > 0) {
                    if (in_src[k] == '(') dp++;
                    else if (in_src[k] == ')')
                        dp--;
                    k++;
                }
                if (dp != 0) {
                    char rel[1024];
                    const char* f =
                        cc_path_rel_to_repo(ctx && ctx->input_path ? ctx->input_path : "<input>", rel, sizeof(rel));
                    cc_pass_error_cat(f, errl, 1, CC_ERR_SYNTAX, "unclosed '(' after @err");
                    goto fail;
                }
                size_t p1 = k - 1;
                size_t decl_a = p0 + 1, decl_b = p1;
                cc__trim_slice(in_src, decl_a, decl_b, &decl_a, &decl_b);
                size_t dl = decl_b - decl_a;
                if (dl >= sizeof(local_decl)) dl = sizeof(local_decl) - 1;
                memcpy(local_decl, in_src + decl_a, dl);
                local_decl[dl] = 0;
                cc__extract_param_name(local_decl, local_param, sizeof(local_param));
                if (!local_param[0]) {
                    char rel[1024];
                    const char* f =
                        cc_path_rel_to_repo(ctx && ctx->input_path ? ctx->input_path : "<input>", rel, sizeof(rel));
                    cc_pass_error_cat(f, errl, 1, CC_ERR_SYNTAX, "expected parameter in @err(...)");
                    goto fail;
                }
                while (k < in_len && isspace((unsigned char)in_src[k])) k++;
                if (k >= in_len || in_src[k] != '{') {
                    char rel[1024];
                    const char* f =
                        cc_path_rel_to_repo(ctx && ctx->input_path ? ctx->input_path : "<input>", rel, sizeof(rel));
                    cc_pass_error_cat(f, errl, 1, CC_ERR_SYNTAX, "expected '{' after @err(...)");
                    goto fail;
                }
                size_t bo = k;
                size_t bc = 0;
                if (!cc__find_matching_brace_text(in_src, in_len, bo, &bc)) {
                    char rel[1024];
                    const char* f =
                        cc_path_rel_to_repo(ctx && ctx->input_path ? ctx->input_path : "<input>", rel, sizeof(rel));
                    cc_pass_error_cat(f, errl, 1, CC_ERR_SYNTAX, "unclosed body in @err(...){...}");
                    goto fail;
                }
                size_t ia = bo + 1, ib = bc;
                cc__trim_slice(in_src, ia, ib, &ia, &ib);
                local_body_len = ib - ia;
                local_body = (char*)malloc(local_body_len + 1);
                if (!local_body) goto fail;
                memcpy(local_body, in_src + ia, local_body_len);
                local_body[local_body_len] = 0;
                k = bc + 1;
                has_local = 1;
            } else if (k < in_len && in_src[k] == '{') {
                size_t bo = k;
                size_t bc = 0;
                if (!cc__find_matching_brace_text(in_src, in_len, bo, &bc)) {
                    char rel[1024];
                    const char* f =
                        cc_path_rel_to_repo(ctx && ctx->input_path ? ctx->input_path : "<input>", rel, sizeof(rel));
                    cc_pass_error_cat(f, errl, 1, CC_ERR_SYNTAX, "unclosed body in @err{...}");
                    goto fail;
                }
                size_t ia = bo + 1, ib = bc;
                cc__trim_slice(in_src, ia, ib, &ia, &ib);
                local_body_len = ib - ia;
                local_body = (char*)malloc(local_body_len + 1);
                if (!local_body) goto fail;
                memcpy(local_body, in_src + ia, local_body_len);
                local_body[local_body_len] = 0;
                k = bc + 1;
                has_local = 1;
            }

            size_t stmt_end = 0;
            if (!cc__scan_stmt_end_semicolon(in_src, in_len, k, &stmt_end)) {
                char rel[1024];
                const char* f =
                    cc_path_rel_to_repo(ctx && ctx->input_path ? ctx->input_path : "<input>", rel, sizeof(rel));
                cc_pass_error_cat(f, errl, 1, CC_ERR_SYNTAX, "expected ';' after @err");
                free(local_body);
                goto fail;
            }

            if (!has_local && k < stmt_end) {
                size_t sa = k, sb = stmt_end;
                cc__trim_slice(in_src, sa, sb, &sa, &sb);
                if (sb > sa && in_src[sa] != ';') {
                    local_body_len = sb - sa;
                    local_body = (char*)malloc(local_body_len + 1);
                    if (!local_body) goto fail;
                    memcpy(local_body, in_src + sa, local_body_len);
                    local_body[local_body_len] = 0;
                    has_local = 1;
                }
            }

            size_t expr_a = stmt_start, expr_b = err_at;
            size_t lhs_a = 0, lhs_b = 0;
            int has_assign = 0;
            cc__trim_slice(in_src, stmt_start, err_at, &expr_a, &expr_b);
            for (size_t t = expr_a; t + 2 <= expr_b; t++) {
                if (t + 3 <= expr_b && in_src[t] == '=' && in_src[t + 1] == '<' && in_src[t + 2] == '!' &&
                    (t == expr_a || !cc_is_ident_char(in_src[t - 1]))) {
                    lhs_a = expr_a;
                    lhs_b = t;
                    expr_a = t + 3;
                    cc__trim_slice(in_src, lhs_a, lhs_b, &lhs_a, &lhs_b);
                    cc__trim_slice(in_src, expr_a, expr_b, &expr_a, &expr_b);
                    has_assign = 1;
                    break;
                }
                if (in_src[t] == '<' && in_src[t + 1] == '?' &&
                    (t == expr_a || !cc_is_ident_char(in_src[t - 1]))) {
                    lhs_a = expr_a;
                    lhs_b = t;
                    expr_a = t + 2;
                    cc__trim_slice(in_src, lhs_a, lhs_b, &lhs_a, &lhs_b);
                    cc__trim_slice(in_src, expr_a, expr_b, &expr_a, &expr_b);
                    has_assign = 1;
                    break;
                }
            }

            if (expr_b <= expr_a) {
                char rel[1024];
                const char* f =
                    cc_path_rel_to_repo(ctx && ctx->input_path ? ctx->input_path : "<input>", rel, sizeof(rel));
                cc_pass_error_cat(f, errl, 1, CC_ERR_SYNTAX, "missing expression before @err");
                free(local_body);
                goto fail;
            }
            if (has_assign && !(lhs_b > lhs_a)) {
                char rel[1024];
                const char* f =
                    cc_path_rel_to_repo(ctx && ctx->input_path ? ctx->input_path : "<input>", rel, sizeof(rel));
                cc_pass_error_cat(f, errl, 1, CC_ERR_SYNTAX, "missing left-hand side before '<?' or '=<!' for @err");
                free(local_body);
                goto fail;
            }

            size_t rest_a = expr_a, rest_b = expr_b;
            size_t def_a = 0, def_b = 0;
            int has_colon_def = 0;
            if (has_assign) {
                int par = 0;
                for (size_t t = expr_a; t < expr_b; t++) {
                    if (in_src[t] == '(')
                        par++;
                    else if (in_src[t] == ')' && par)
                        par--;
                    else if (in_src[t] == ':' && par == 0) {
                        cc__trim_slice(in_src, expr_a, t, &rest_a, &rest_b);
                        cc__trim_slice(in_src, t + 1, expr_b, &def_a, &def_b);
                        if (rest_b > rest_a && def_b > def_a) has_colon_def = 1;
                        break;
                    }
                }
            }

            CCErrFrame* def = NULL;
            if (!has_local) {
                if (stk_n <= 0) {
                    char rel[1024];
                    const char* f =
                        cc_path_rel_to_repo(ctx && ctx->input_path ? ctx->input_path : "<input>", rel, sizeof(rel));
                    cc_pass_error_cat(f, errl, 1, CC_ERR_SYNTAX, "@err with no local handler requires @errhandler");
                    free(local_body);
                    goto fail;
                }
                def = &stk[stk_n - 1];
            }

            int id = ++g_err_id;
            char tmpv[48], tmpv_d[48];
            snprintf(tmpv, sizeof(tmpv), "__cc_er_r_%d", id);
            snprintf(tmpv_d, sizeof(tmpv_d), "__cc_er_d_%d", id);

            char assignee[64] = {0};
            int lhs_is_decl = 0;
            int lhs_decl_is_result = 0;
            if (has_assign) {
                char lhs_one[256];
                size_t lhl = lhs_b - lhs_a;
                if (lhl >= sizeof(lhs_one)) {
                    char rel[1024];
                    const char* f =
                        cc_path_rel_to_repo(ctx && ctx->input_path ? ctx->input_path : "<input>", rel, sizeof(rel));
                    cc_pass_error_cat(f, errl, 1, CC_ERR_SYNTAX, "'<?'/'=<!' left-hand side too long for @err");
                    free(local_body);
                    goto fail;
                }
                memcpy(lhs_one, in_src + lhs_a, lhl);
                lhs_one[lhl] = 0;
                lhs_is_decl = cc__lhs_is_decl_like(lhs_one, 0, lhl, assignee, sizeof(assignee));
                lhs_decl_is_result = lhs_is_decl && cc__decl_slice_is_result_type(lhs_one, lhl);
            }

            /* Rewind out to stmt start (input/output lengths differ when @errhandler emitted newlines only). */
            ol = ito[stmt_start];
            if (out) out[ol] = 0;

            for (size_t x = stmt_start; x < stmt_end; x++) {
                if (in_src[x] == '\n') cc__append_n(&out, &ol, &oc, "\n", 1);
            }

            if (has_assign && lhs_is_decl) {
                cc__append_n(&out, &ol, &oc, in_src + lhs_a, lhs_b - lhs_a);
                cc__append_str(&out, &ol, &oc, "; ");
            }

            if (has_assign && has_colon_def) {
                cc__append_str(&out, &ol, &oc, "{ __typeof__(");
                cc__append_n(&out, &ol, &oc, in_src + rest_a, rest_b - rest_a);
                cc_sb_append_fmt(&out, &ol, &oc, ") %s = (", tmpv);
                cc__append_n(&out, &ol, &oc, in_src + rest_a, rest_b - rest_a);
                cc_sb_append_fmt(&out, &ol, &oc, "); if (cc_is_ok(%s)) { ", tmpv);
                if (lhs_is_decl) {
                    if (lhs_decl_is_result)
                        cc_sb_append_fmt(&out, &ol, &oc, "%s = %s; ", assignee, tmpv);
                    else
                        cc_sb_append_fmt(&out, &ol, &oc, "%s = cc_value(%s); ", assignee, tmpv);
                }
                else {
                    cc__append_n(&out, &ol, &oc, in_src + lhs_a, lhs_b - lhs_a);
                    cc_sb_append_fmt(&out, &ol, &oc, " = %s; ", tmpv);
                }
                cc__append_str(&out, &ol, &oc, "} else { __typeof__(");
                cc__append_n(&out, &ol, &oc, in_src + def_a, def_b - def_a);
                cc_sb_append_fmt(&out, &ol, &oc, ") %s = (", tmpv_d);
                cc__append_n(&out, &ol, &oc, in_src + def_a, def_b - def_a);
                cc_sb_append_fmt(&out, &ol, &oc, "); if (cc_is_err(%s)) ", tmpv_d);
            } else {
                cc__append_str(&out, &ol, &oc, "{ __typeof__(");
                cc__append_n(&out, &ol, &oc, in_src + expr_a, expr_b - expr_a);
                cc_sb_append_fmt(&out, &ol, &oc, ") %s = (", tmpv);
                cc__append_n(&out, &ol, &oc, in_src + expr_a, expr_b - expr_a);
                cc_sb_append_fmt(&out, &ol, &oc, "); if (cc_is_err(%s)) ", tmpv);
            }

            {
                const char* err_tmp = (has_assign && has_colon_def) ? tmpv_d : tmpv;
                if (has_local) {
                    const CCErrFrame* outer_d = (stk_n > 0) ? &stk[stk_n - 1] : NULL;
                    int dg = 1;
                    char* lb_exp =
                        cc__expand_delegations(local_body, local_body_len, local_param, outer_d, NULL, &dg);
                    free(local_body);
                    local_body = NULL;
                    if (!dg || !lb_exp) {
                        char rel[1024];
                        const char* f =
                            cc_path_rel_to_repo(ctx && ctx->input_path ? ctx->input_path : "<input>", rel, sizeof(rel));
                        cc_pass_error_cat(f, errl, 1, CC_ERR_SYNTAX, "bad @errhandler(e); delegation in @err block");
                        goto fail;
                    }
                    cc__append_str(&out, &ol, &oc, "{ ");
                    if (local_decl[0])
                        cc_sb_append_fmt(&out, &ol, &oc, "%s = cc_error(%s); ", local_decl, err_tmp);
                    cc__append_str(&out, &ol, &oc, lb_exp);
                    cc__append_str(&out, &ol, &oc, " } ");
                    free(lb_exp);
                } else {
                    cc_sb_append_fmt(&out, &ol, &oc, "{ %s = cc_error(%s); ", def->param_decl, err_tmp);
                    cc__append_n(&out, &ol, &oc, def->body, def->body_len);
                    cc__append_str(&out, &ol, &oc, " } ");
                }
            }

            if (has_assign && has_colon_def) {
                cc__append_str(&out, &ol, &oc, "else { ");
                if (lhs_is_decl) {
                    if (lhs_decl_is_result)
                        cc_sb_append_fmt(&out, &ol, &oc, "%s = %s; ", assignee, tmpv_d);
                    else
                        cc_sb_append_fmt(&out, &ol, &oc, "%s = cc_value(%s); ", assignee, tmpv_d);
                }
                else {
                    cc__append_n(&out, &ol, &oc, in_src + lhs_a, lhs_b - lhs_a);
                    cc_sb_append_fmt(&out, &ol, &oc, " = %s; ", tmpv_d);
                }
                cc__append_str(&out, &ol, &oc, "} } } ");
            } else {
                cc_sb_append_fmt(&out, &ol, &oc, "else ");
                if (has_assign) {
                    cc__append_str(&out, &ol, &oc, "{ ");
                    if (lhs_is_decl) {
                        if (lhs_decl_is_result)
                            cc_sb_append_fmt(&out, &ol, &oc, "%s = %s; ", assignee, tmpv);
                        else
                            cc_sb_append_fmt(&out, &ol, &oc, "%s = cc_value(%s); ", assignee, tmpv);
                    }
                    else {
                        cc__append_n(&out, &ol, &oc, in_src + lhs_a, lhs_b - lhs_a);
                        cc_sb_append_fmt(&out, &ol, &oc, " = %s; ", tmpv);
                    }
                    cc__append_str(&out, &ol, &oc, "} ");
                } else {
                    cc_sb_append_fmt(&out, &ol, &oc, "{ (void)%s; } ", tmpv);
                }
                cc__append_str(&out, &ol, &oc, "} ");
            }

            for (size_t x = stmt_start; x < stmt_end; x++) {
                if (in_src[x] == '\n') line_no++;
            }
            changed = 1;
            i = stmt_end - 1;
            continue;
        }

        if (ch == '{') {
            depth++;
            cc__append_n(&out, &ol, &oc, &ch, 1);
            continue;
        }
        if (ch == '}') {
            while (stk_n > 0 && stk[stk_n - 1].reg_depth == depth) {
                stk_n--;
                cc__err_frame_free(&stk[stk_n]);
            }
            if (depth > 0) depth--;
            cc__append_n(&out, &ol, &oc, &ch, 1);
            continue;
        }

        cc__append_n(&out, &ol, &oc, &ch, 1);
    }
    ito[in_len] = ol;
    free(ito);
    ito = NULL;

    if (!changed) {
        free(out);
        for (int x = 0; x < stk_n; x++) cc__err_frame_free(&stk[x]);
        return 0;
    }
    *out_src = out;
    *out_len = ol;
    for (int x = 0; x < stk_n; x++) cc__err_frame_free(&stk[x]);
    return 1;

fail:
    free(ito);
    free(out);
    for (int x = 0; x < stk_n; x++) cc__err_frame_free(&stk[x]);
    return -1;
}
