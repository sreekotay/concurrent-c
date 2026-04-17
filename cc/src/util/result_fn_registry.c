#include "util/result_fn_registry.h"

#include <stdlib.h>
#include <string.h>

/* Small additive set keyed by function name. */
static char**  g_names = NULL;
static size_t  g_len   = 0;
static size_t  g_cap   = 0;

void cc_result_fn_registry_clear(void) {
    for (size_t i = 0; i < g_len; i++) {
        free(g_names[i]);
    }
    free(g_names);
    g_names = NULL;
    g_len = 0;
    g_cap = 0;
}

int cc_result_fn_registry_contains(const char* name, size_t len) {
    if (!name || len == 0) return 0;
    for (size_t i = 0; i < g_len; i++) {
        const char* s = g_names[i];
        if (!s) continue;
        size_t sl = strlen(s);
        if (sl == len && memcmp(s, name, len) == 0) return 1;
    }
    return 0;
}

void cc_result_fn_registry_add(const char* name, size_t len) {
    if (!name || len == 0) return;
    if (cc_result_fn_registry_contains(name, len)) return;
    if (g_len == g_cap) {
        size_t nc = g_cap ? g_cap * 2 : 16;
        char** nb = (char**)realloc(g_names, nc * sizeof(char*));
        if (!nb) return;
        g_names = nb;
        g_cap = nc;
    }
    char* dup = (char*)malloc(len + 1);
    if (!dup) return;
    memcpy(dup, name, len);
    dup[len] = 0;
    g_names[g_len++] = dup;
}
