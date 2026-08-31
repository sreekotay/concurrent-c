#include "strswitch_comptime.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util/text.h"

enum { CC_CT_STRSW_MAX_KEYS = 64, CC_CT_STRSW_KEY_CAP = 128, CC_CT_STRSW_MAX_ARMS = 64 };

typedef struct {
    char keys[CC_CT_STRSW_MAX_KEYS][CC_CT_STRSW_KEY_CAP];
    int nkeys;
    int is_default;
    size_t body_lo; /* inclusive offset into switch body text */
    size_t body_hi; /* exclusive */
} CCCtStrswArm;

typedef struct {
    char* data;
    size_t len;
    size_t cap;
} CCCtSb;

static int cc__sb_reserve(CCCtSb* sb, size_t need) {
    char* nb;
    size_t nc;
    if (!sb) return 0;
    if (sb->len + need + 1 <= sb->cap) return 1;
    nc = sb->cap ? sb->cap : 256;
    while (sb->len + need + 1 > nc) nc *= 2;
    nb = (char*)realloc(sb->data, nc);
    if (!nb) return 0;
    sb->data = nb;
    sb->cap = nc;
    return 1;
}

static int cc__sb_append(CCCtSb* sb, const char* p, size_t n) {
    if (!p || !n) return 1;
    if (!cc__sb_reserve(sb, n)) return 0;
    memcpy(sb->data + sb->len, p, n);
    sb->len += n;
    sb->data[sb->len] = 0;
    return 1;
}

static int cc__sb_append_cstr(CCCtSb* sb, const char* s) {
    return cc__sb_append(sb, s, s ? strlen(s) : 0);
}

static int cc__at_kw(const char* s, size_t len, size_t i, const char* kw) {
    return cc_match_ident_kw(s, len, i, kw);
}

/* Strip a trailing top-level `break;` from an arm body (switch break). */
static size_t cc__strip_trailing_break(const char* s, size_t lo, size_t hi) {
    size_t i;
    if (hi < lo || !s) return hi;
    i = hi;
    while (i > lo) {
        char c = s[i - 1];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            i--;
            continue;
        }
        break;
    }
    /* Match `break` `;` immediately before i. */
    if (i >= lo + 6) {
        size_t b = i;
        /* optional ';' */
        if (b > lo && s[b - 1] == ';') b--;
        else return i;
        while (b > lo && (s[b - 1] == ' ' || s[b - 1] == '\t')) b--;
        if (b >= lo + 5 && memcmp(s + b - 5, "break", 5) == 0) {
            size_t k = b - 5;
            if (k == lo || !cc_is_ident_char(s[k - 1])) {
                /* ensure not `breakx` */
                if (b < i && !cc_is_ident_char(s[b])) {
                    while (k > lo &&
                           (s[k - 1] == ' ' || s[k - 1] == '\t' ||
                            s[k - 1] == '\n' || s[k - 1] == '\r'))
                        k--;
                    return k;
                }
            }
        }
    }
    return i;
}

/* Classify switch body: 1 = string switch, 0 = not, -1 = hard error. */
static int cc__classify_cases(const char* body, size_t len, char* err,
                              size_t errcap) {
    size_t i = 0;
    int in_dq = 0, in_sq = 0, in_bt = 0;
    int depth = 0;
    int saw_string = 0, saw_nonstring = 0;
    while (i < len) {
        char c = body[i];
        if (in_bt) {
            if (c == '\\' && i + 1 < len) {
                i += 2;
                continue;
            }
            if (c == '`') in_bt = 0;
            i++;
            continue;
        }
        if (in_dq) {
            if (c == '\\' && i + 1 < len) {
                i += 2;
                continue;
            }
            if (c == '"') in_dq = 0;
            i++;
            continue;
        }
        if (in_sq) {
            if (c == '\\' && i + 1 < len) {
                i += 2;
                continue;
            }
            if (c == '\'') in_sq = 0;
            i++;
            continue;
        }
        if (c == '/' && i + 1 < len && body[i + 1] == '/') {
            i += 2;
            while (i < len && body[i] != '\n') i++;
            continue;
        }
        if (c == '/' && i + 1 < len && body[i + 1] == '*') {
            i += 2;
            while (i + 1 < len && !(body[i] == '*' && body[i + 1] == '/')) i++;
            if (i + 1 < len) i += 2;
            continue;
        }
        if (c == '`') {
            in_bt = 1;
            i++;
            continue;
        }
        if (c == '"') {
            in_dq = 1;
            i++;
            continue;
        }
        if (c == '\'') {
            in_sq = 1;
            i++;
            continue;
        }
        if (c == '{') {
            depth++;
            i++;
            continue;
        }
        if (c == '}') {
            if (depth) depth--;
            i++;
            continue;
        }
        if (depth != 0) {
            i++;
            continue;
        }
        if (cc__at_kw(body, len, i, "default")) {
            i += 7;
            continue;
        }
        if (cc__at_kw(body, len, i, "case")) {
            size_t j = cc_skip_ws_and_comments(body, len, i + 4);
            if (j < len && body[j] == '"') {
                size_t k = j + 1;
                while (k < len && body[k] != '"') {
                    if (body[k] == '\\') {
                        if (err && errcap)
                            snprintf(err, errcap,
                                     "string switch: case labels must be "
                                     "simple string literals (no escapes)");
                        return -1;
                    }
                    k++;
                }
                if (k >= len || body[k] != '"') {
                    if (err && errcap)
                        snprintf(err, errcap,
                                 "string switch: unterminated case label");
                    return -1;
                }
                saw_string = 1;
                i = k + 1;
                continue;
            }
            saw_nonstring = 1;
            i = j;
            continue;
        }
        i++;
    }
    if (saw_string && saw_nonstring) {
        if (err && errcap)
            snprintf(err, errcap,
                     "string switch: cannot mix string and non-string case "
                     "labels");
        return -1;
    }
    return saw_string ? 1 : 0;
}

/* Parse string-switch arms from body (interior of `{…}`). Returns arm count
 * or -1 on error. */
static int cc__parse_arms(const char* body, size_t len, CCCtStrswArm* arms,
                          int max_arms, char* err, size_t errcap) {
    size_t i = 0;
    int in_dq = 0, in_sq = 0, in_bt = 0;
    int depth = 0;
    int narm = 0;
    int collecting = 0; /* gathering case labels for current arm */

    if (err && errcap) err[0] = 0;

    while (i < len) {
        char c = body[i];
        if (in_bt) {
            if (c == '\\' && i + 1 < len) {
                i += 2;
                continue;
            }
            if (c == '`') in_bt = 0;
            i++;
            continue;
        }
        if (in_dq) {
            if (c == '\\' && i + 1 < len) {
                i += 2;
                continue;
            }
            if (c == '"') in_dq = 0;
            i++;
            continue;
        }
        if (in_sq) {
            if (c == '\\' && i + 1 < len) {
                i += 2;
                continue;
            }
            if (c == '\'') in_sq = 0;
            i++;
            continue;
        }
        if (c == '/' && i + 1 < len && body[i + 1] == '/') {
            i += 2;
            while (i < len && body[i] != '\n') i++;
            continue;
        }
        if (c == '/' && i + 1 < len && body[i + 1] == '*') {
            i += 2;
            while (i + 1 < len && !(body[i] == '*' && body[i + 1] == '/')) i++;
            if (i + 1 < len) i += 2;
            continue;
        }
        if (c == '`') {
            in_bt = 1;
            i++;
            continue;
        }
        if (c == '"') {
            in_dq = 1;
            i++;
            continue;
        }
        if (c == '\'') {
            in_sq = 1;
            i++;
            continue;
        }
        if (c == '{') {
            depth++;
            i++;
            continue;
        }
        if (c == '}') {
            if (depth) depth--;
            i++;
            continue;
        }

        if (depth == 0 &&
            (cc__at_kw(body, len, i, "case") ||
             cc__at_kw(body, len, i, "default"))) {
            int is_def = cc__at_kw(body, len, i, "default");
            /* Close previous arm body at this label. */
            if (narm > 0 && !collecting) {
                arms[narm - 1].body_hi = i;
            }
            if (!collecting) {
                if (narm >= max_arms) {
                    if (err && errcap)
                        snprintf(err, errcap, "string switch: too many arms");
                    return -1;
                }
                memset(&arms[narm], 0, sizeof(arms[narm]));
                narm++;
                collecting = 1;
            }
            if (is_def) {
                size_t j;
                if (arms[narm - 1].nkeys > 0) {
                    if (err && errcap)
                        snprintf(err, errcap,
                                 "string switch: default after case labels in "
                                 "same arm");
                    return -1;
                }
                arms[narm - 1].is_default = 1;
                j = cc_skip_ws_and_comments(body, len, i + 7);
                if (j >= len || body[j] != ':') {
                    if (err && errcap)
                        snprintf(err, errcap,
                                 "string switch: expected ':' after default");
                    return -1;
                }
                arms[narm - 1].body_lo = j + 1;
                collecting = 0;
                i = j + 1;
                continue;
            }
            /* case "lit": */
            {
                size_t j = cc_skip_ws_and_comments(body, len, i + 4);
                char key[CC_CT_STRSW_KEY_CAP];
                size_t kn = 0;
                size_t k;
                int di;
                if (j >= len || body[j] != '"') {
                    if (err && errcap)
                        snprintf(err, errcap,
                                 "string switch: expected string case label");
                    return -1;
                }
                k = j + 1;
                while (k < len && body[k] != '"') {
                    if (body[k] == '\\') {
                        if (err && errcap)
                            snprintf(err, errcap,
                                     "string switch: case labels must be "
                                     "simple string literals (no escapes)");
                        return -1;
                    }
                    if (kn + 1 >= sizeof(key)) {
                        if (err && errcap)
                            snprintf(err, errcap,
                                     "string switch: case label too long");
                        return -1;
                    }
                    key[kn++] = body[k++];
                }
                if (k >= len || body[k] != '"') {
                    if (err && errcap)
                        snprintf(err, errcap,
                                 "string switch: unterminated case label");
                    return -1;
                }
                key[kn] = 0;
                for (di = 0; di < arms[narm - 1].nkeys; di++) {
                    if (strcmp(arms[narm - 1].keys[di], key) == 0) {
                        if (err && errcap)
                            snprintf(err, errcap,
                                     "string switch: duplicate case \"%s\"",
                                     key);
                        return -1;
                    }
                }
                /* Also reject duplicates across arms. */
                {
                    int a;
                    for (a = 0; a < narm - 1; a++) {
                        for (di = 0; di < arms[a].nkeys; di++) {
                            if (strcmp(arms[a].keys[di], key) == 0) {
                                if (err && errcap)
                                    snprintf(err, errcap,
                                             "string switch: duplicate case "
                                             "\"%s\"",
                                             key);
                                return -1;
                            }
                        }
                    }
                }
                if (arms[narm - 1].nkeys >= CC_CT_STRSW_MAX_KEYS) {
                    if (err && errcap)
                        snprintf(err, errcap,
                                 "string switch: too many case labels");
                    return -1;
                }
                memcpy(arms[narm - 1].keys[arms[narm - 1].nkeys], key, kn + 1);
                arms[narm - 1].nkeys++;
                j = cc_skip_ws_and_comments(body, len, k + 1);
                if (j >= len || body[j] != ':') {
                    if (err && errcap)
                        snprintf(err, errcap,
                                 "string switch: expected ':' after case "
                                 "label");
                    return -1;
                }
                arms[narm - 1].body_lo = j + 1;
                /* Peek: another `case` soon? keep collecting labels. */
                {
                    size_t peek = cc_skip_ws_and_comments(body, len, j + 1);
                    if (peek < len && cc__at_kw(body, len, peek, "case")) {
                        collecting = 1;
                        i = j + 1;
                        continue;
                    }
                }
                collecting = 0;
                i = j + 1;
                continue;
            }
        }
        i++;
    }
    if (narm > 0) arms[narm - 1].body_hi = len;
    return narm;
}

static int cc__emit_arm_cond(CCCtSb* out, const CCCtStrswArm* arm) {
    int k;
    if (!arm || arm->is_default) return 0;
    for (k = 0; k < arm->nkeys; k++) {
        if (k) {
            if (!cc__sb_append_cstr(out, " || ")) return 0;
        }
        if (!cc__sb_append_cstr(out, "CCSlice_eq_cstr(&__cc_sw, \"")) return 0;
        if (!cc__sb_append_cstr(out, arm->keys[k])) return 0;
        if (!cc__sb_append_cstr(out, "\")")) return 0;
    }
    return 1;
}

static int cc__rewrite_one_switch(CCCtSb* out, const char* src, size_t sw_kw,
                                  size_t rbrace, char* err, size_t errcap) {
    size_t after_kw, lpar, rpar, lbrace;
    size_t subj_lo, subj_hi, body_lo, body_hi;
    CCCtStrswArm arms[CC_CT_STRSW_MAX_ARMS];
    int narm, a, first_if, cls;
    const char* body;
    size_t blen;

    after_kw = cc_skip_ws_and_comments(src, rbrace + 1, sw_kw + 6);
    if (after_kw >= rbrace + 1 || src[after_kw] != '(') return 0;
    lpar = after_kw;
    if (!cc_find_matching_paren(src, rbrace + 1, lpar, &rpar)) return 0;
    subj_lo = lpar + 1;
    subj_hi = rpar;
    lbrace = cc_skip_ws_and_comments(src, rbrace + 1, rpar + 1);
    if (lbrace >= rbrace + 1 || src[lbrace] != '{') return 0;
    body_lo = lbrace + 1;
    body_hi = rbrace;
    body = src + body_lo;
    blen = body_hi - body_lo;

    cls = cc__classify_cases(body, blen, err, errcap);
    if (cls < 0) return -1;
    if (cls == 0) {
        /* Not a string switch — copy verbatim. */
        if (!cc__sb_append(out, src + sw_kw, rbrace + 1 - sw_kw)) return -1;
        return 1;
    }

    narm = cc__parse_arms(body, blen, arms, CC_CT_STRSW_MAX_ARMS, err, errcap);
    if (narm < 0) return -1;

    if (!cc__sb_append_cstr(out, "{\nCCSlice __cc_sw = (")) return -1;
    if (!cc__sb_append(out, src + subj_lo, subj_hi - subj_lo)) return -1;
    if (!cc__sb_append_cstr(out, ");\n")) return -1;

    first_if = 1;
    for (a = 0; a < narm; a++) {
        size_t blo = arms[a].body_lo;
        size_t bhi = cc__strip_trailing_break(body, blo, arms[a].body_hi);
        if (arms[a].is_default) {
            if (!cc__sb_append_cstr(out, "else {\n")) return -1;
            if (bhi > blo && !cc__sb_append(out, body + blo, bhi - blo))
                return -1;
            if (!cc__sb_append_cstr(out, "\n}\n")) return -1;
            continue;
        }
        if (first_if) {
            if (!cc__sb_append_cstr(out, "if (")) return -1;
            first_if = 0;
        } else {
            if (!cc__sb_append_cstr(out, "else if (")) return -1;
        }
        if (!cc__emit_arm_cond(out, &arms[a])) return -1;
        if (!cc__sb_append_cstr(out, ") {\n")) return -1;
        if (bhi > blo && !cc__sb_append(out, body + blo, bhi - blo)) return -1;
        if (!cc__sb_append_cstr(out, "\n}\n")) return -1;
    }
    if (!cc__sb_append_cstr(out, "}\n")) return -1;
    return 1;
}

char* cc_comptime_strswitch_rewrite(const char* body, size_t body_len,
                                    char* err, size_t errcap) {
    CCCtSb out = {0};
    size_t i = 0;
    int in_dq = 0, in_sq = 0, in_bt = 0;

    if (err && errcap) err[0] = 0;
    if (!body) {
        if (err && errcap) snprintf(err, errcap, "string switch: null body");
        return NULL;
    }
    if (!cc__sb_reserve(&out, body_len + 64)) {
        if (err && errcap) snprintf(err, errcap, "string switch: OOM");
        return NULL;
    }
    out.data[0] = 0;

    while (i < body_len) {
        char c = body[i];
        if (in_bt) {
            if (!cc__sb_append(&out, &c, 1)) goto oom;
            if (c == '\\' && i + 1 < body_len) {
                if (!cc__sb_append(&out, &body[i + 1], 1)) goto oom;
                i += 2;
                continue;
            }
            if (c == '`') in_bt = 0;
            i++;
            continue;
        }
        if (in_dq) {
            if (!cc__sb_append(&out, &c, 1)) goto oom;
            if (c == '\\' && i + 1 < body_len) {
                if (!cc__sb_append(&out, &body[i + 1], 1)) goto oom;
                i += 2;
                continue;
            }
            if (c == '"') in_dq = 0;
            i++;
            continue;
        }
        if (in_sq) {
            if (!cc__sb_append(&out, &c, 1)) goto oom;
            if (c == '\\' && i + 1 < body_len) {
                if (!cc__sb_append(&out, &body[i + 1], 1)) goto oom;
                i += 2;
                continue;
            }
            if (c == '\'') in_sq = 0;
            i++;
            continue;
        }
        if (c == '/' && i + 1 < body_len && body[i + 1] == '/') {
            if (!cc__sb_append(&out, body + i, 2)) goto oom;
            i += 2;
            while (i < body_len && body[i] != '\n') {
                if (!cc__sb_append(&out, &body[i], 1)) goto oom;
                i++;
            }
            continue;
        }
        if (c == '/' && i + 1 < body_len && body[i + 1] == '*') {
            if (!cc__sb_append(&out, body + i, 2)) goto oom;
            i += 2;
            while (i + 1 < body_len &&
                   !(body[i] == '*' && body[i + 1] == '/')) {
                if (!cc__sb_append(&out, &body[i], 1)) goto oom;
                i++;
            }
            if (i + 1 < body_len) {
                if (!cc__sb_append(&out, body + i, 2)) goto oom;
                i += 2;
            }
            continue;
        }
        if (c == '`') {
            in_bt = 1;
            if (!cc__sb_append(&out, &c, 1)) goto oom;
            i++;
            continue;
        }
        if (c == '"') {
            in_dq = 1;
            if (!cc__sb_append(&out, &c, 1)) goto oom;
            i++;
            continue;
        }
        if (c == '\'') {
            in_sq = 1;
            if (!cc__sb_append(&out, &c, 1)) goto oom;
            i++;
            continue;
        }

        /* `@switch` is required for string cases. Consume `@` so a leftover
         * sigil does not sit in front of the rewritten if/else (hook TU
         * drops any chunk that still has `@`, which used to hide port_ufcs). */
        if (c == '@' && i + 1 < body_len &&
            cc__at_kw(body, body_len, i + 1, "switch")) {
            i++;
            c = body[i];
        }
        if (cc__at_kw(body, body_len, i, "switch")) {
            size_t after = cc_skip_ws_and_comments(body, body_len, i + 6);
            size_t rpar = 0, lbrace = 0, rbrace = 0;
            int rc;
            if (after < body_len && body[after] == '(' &&
                cc_find_matching_paren(body, body_len, after, &rpar)) {
                lbrace = cc_skip_ws_and_comments(body, body_len, rpar + 1);
                if (lbrace < body_len && body[lbrace] == '{' &&
                    cc_find_matching_brace(body, body_len, lbrace, &rbrace)) {
                    rc = cc__rewrite_one_switch(&out, body, i, rbrace, err,
                                                errcap);
                    if (rc < 0) {
                        free(out.data);
                        return NULL;
                    }
                    if (rc > 0) {
                        i = rbrace + 1;
                        continue;
                    }
                }
            }
            /* Fall through: copy `switch` as text. */
        }

        if (!cc__sb_append(&out, &c, 1)) goto oom;
        i++;
    }
    return out.data;

oom:
    free(out.data);
    if (err && errcap) snprintf(err, errcap, "string switch: OOM");
    return NULL;
}
