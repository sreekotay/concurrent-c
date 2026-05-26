#include "mangle.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

static char* cc__sanitize_ident(const char* s) {
    if (!s || !s[0]) return strdup("anon");
    size_t n = strlen(s);
    char* out = malloc(n + 1);
    if (!out) return NULL;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        out[i] = (isalnum((unsigned char)c) || c == '_') ? c : '_';
    }
    out[n] = '\0';
    return out;
}

char* cc_diag_mangle_symbol(CCConstructKind kind,
                            const char* user_ident,
                            int line,
                            int col) {
    const char* cname = cc_construct_kind_name(kind);
    char* ident = cc__sanitize_ident(user_ident);
    if (!ident) return NULL;
    size_t cap = strlen(cname) + strlen(ident) + 64;
    char* out = malloc(cap);
    if (!out) {
        free(ident);
        return NULL;
    }
    snprintf(out, cap, "cc_%s__%s__line%d_col%d", cname, ident, line, col);
    free(ident);
    return out;
}
