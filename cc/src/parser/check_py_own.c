#include "check_py_own.h"

#include <stdio.h>
#include <string.h>

/* Self-contained lexical walk: comments, string/char literals, and
 * backtick templates are opaque.  Mirrors the registry scanner's state
 * machine rather than pulling preprocess helpers into the parser TU. */

static int cc__po_ident_start(char c) {
    return c == '_' || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}
static int cc__po_ident_char(char c) {
    return cc__po_ident_start(c) || (c >= '0' && c <= '9');
}

static void cc__po_line_col(const char* src, size_t off, int* line, int* col) {
    int l = 1, c = 1;
    for (size_t i = 0; i < off; i++) {
        if (src[i] == '\n') { l++; c = 1; } else { c++; }
    }
    *line = l;
    *col = c;
}

/* Advance past one opaque region if `i` sits at its opener; returns the
 * position after it, or `i` unchanged when not at an opener. */
static size_t cc__po_skip_opaque(const char* src, size_t n, size_t i) {
    char c = src[i];
    char c2 = (i + 1 < n) ? src[i + 1] : 0;
    if (c == '/' && c2 == '/') {
        while (i < n && src[i] != '\n') i++;
        return i;
    }
    if (c == '/' && c2 == '*') {
        i += 2;
        while (i + 1 < n && !(src[i] == '*' && src[i + 1] == '/')) i++;
        return (i + 1 < n) ? i + 2 : n;
    }
    if (c == '"' || c == '\'' || c == '`') {
        char q = c;
        i++;
        while (i < n) {
            if (q != '`' && src[i] == '\\' && i + 1 < n) { i += 2; continue; }
            if (src[i] == q) return i + 1;
            i++;
        }
        return n;
    }
    return i;
}

/* Consumed later?  Either `return <name>` (ownership moves to the
 * caller) or the manual spelling `cc_py_obj_release(&<name>)`. */
static int cc__po_consumed_later(const char* src, size_t n, size_t from,
                                 const char* name, size_t name_len) {
    size_t i = from;
    while (i < n) {
        size_t j = cc__po_skip_opaque(src, n, i);
        if (j != i) { i = j; continue; }
        if (src[i] == 'r' && i + 6 < n && memcmp(src + i, "return", 6) == 0 &&
            (i == 0 || !cc__po_ident_char(src[i - 1])) &&
            !cc__po_ident_char(src[i + 6])) {
            size_t k = i + 6;
            while (k < n && (src[k] == ' ' || src[k] == '\t')) k++;
            if (k + name_len <= n && memcmp(src + k, name, name_len) == 0 &&
                (k + name_len == n || !cc__po_ident_char(src[k + name_len])))
                return 1;
            i = k;
            continue;
        }
        if (src[i] == 'c' && i + 17 < n &&
            memcmp(src + i, "cc_py_obj_release", 17) == 0 &&
            (i == 0 || !cc__po_ident_char(src[i - 1])) &&
            !cc__po_ident_char(src[i + 17])) {
            size_t k = i + 17;
            while (k < n && (src[k] == ' ' || src[k] == '\t')) k++;
            if (k < n && src[k] == '(') {
                k++;
                while (k < n && (src[k] == ' ' || src[k] == '\t')) k++;
                if (k < n && src[k] == '&') {
                    k++;
                    while (k < n && (src[k] == ' ' || src[k] == '\t')) k++;
                    if (k + name_len <= n &&
                        memcmp(src + k, name, name_len) == 0 &&
                        (k + name_len == n ||
                         !cc__po_ident_char(src[k + name_len])))
                        return 1;
                }
            }
            i = k;
            continue;
        }
        i++;
    }
    return 0;
}

int cc_check_py_own(const char* src, size_t n, const char* input_path) {
    size_t i = 0;
    if (!src || n == 0) return 0;
    while (i < n) {
        size_t j = cc__po_skip_opaque(src, n, i);
        if (j != i) { i = j; continue; }
        if (!(src[i] == 'C' && i + 7 < n && memcmp(src + i, "CCPyObj", 7) == 0 &&
              (i == 0 || !cc__po_ident_char(src[i - 1])) &&
              !cc__po_ident_char(src[i + 7]))) {
            i++;
            continue;
        }
        {
            size_t decl = i;
            size_t k = i + 7;
            size_t name_s, name_e;
            while (k < n && (src[k] == ' ' || src[k] == '\t')) k++;
            /* `CCPyObj *p` is a borrow; `(CCPyObj)` a cast; a field or
             * parameter never matches the `= init` shape below. */
            if (k >= n || !cc__po_ident_start(src[k])) { i = k; continue; }
            name_s = k;
            while (k < n && cc__po_ident_char(src[k])) k++;
            name_e = k;
            while (k < n && (src[k] == ' ' || src[k] == '\t')) k++;
            if (k >= n || src[k] != '=' || (k + 1 < n && src[k + 1] == '=')) {
                i = name_e;
                continue;
            }
            /* Statement runs to the `;` at zero delimiter depth (an
             * inline `!>(e) { ... }` handler nests braces mid-statement). */
            {
                int depth = 0;
                size_t s = k + 1, stmt_e = n;
                int owned = 0;
                while (s < n) {
                    size_t o = cc__po_skip_opaque(src, n, s);
                    if (o != s) { s = o; continue; }
                    if (src[s] == '(' || src[s] == '{' || src[s] == '[') depth++;
                    else if (src[s] == ')' || src[s] == '}' || src[s] == ']') depth--;
                    else if (src[s] == ';' && depth <= 0) { stmt_e = s; break; }
                    else if (src[s] == '@' &&
                             ((s + 8 <= n && memcmp(src + s, "@destroy", 8) == 0) ||
                              (s + 7 <= n && memcmp(src + s, "@detach", 7) == 0)))
                        owned = 1;
                    s++;
                }
                if (!owned &&
                    !cc__po_consumed_later(src, n, stmt_e, src + name_s,
                                           name_e - name_s)) {
                    int line = 1, col = 1;
                    cc__po_line_col(src, decl, &line, &col);
                    fprintf(stderr,
                            "%s:%d:%d: error: ownership: `CCPyObj %.*s` holds a "
                            "Python reference: bind '@destroy' so the scope "
                            "releases it, consume it (`return %.*s`), or release "
                            "it by hand (cc_py_obj_release); a dropped handle "
                            "leaks the object with no diagnostic on either "
                            "side of the boundary\n",
                            input_path ? input_path : "<input>", line, col,
                            (int)(name_e - name_s), src + name_s,
                            (int)(name_e - name_s), src + name_s);
                    return -1;
                }
                i = stmt_e < n ? stmt_e + 1 : n;
            }
        }
    }
    return 0;
}
