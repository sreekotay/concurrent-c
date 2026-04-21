#include "util/result_fn_registry.h"

#include <stdlib.h>
#include <string.h>

/* Small additive set keyed by function name. */
typedef struct {
    char**  names;
    size_t  len;
    size_t  cap;
} cc_name_set;

/* Parallel map from result-fn name -> textual error type (e.g. "CCIoError").
 * Kept separate from `g_result_fns` so the existing unhandled-result scan
 * (slice-7) can keep using the name-only set without paying for err strings
 * when they are not needed. */
typedef struct {
    char**  names;
    char**  err_types;
    size_t  len;
    size_t  cap;
} cc_name_err_map;

static cc_name_set g_result_fns = { NULL, 0, 0 };
static cc_name_err_map g_result_fn_err = { NULL, NULL, 0, 0 };

static void cc__name_set_clear(cc_name_set* s) {
    for (size_t i = 0; i < s->len; i++) free(s->names[i]);
    free(s->names);
    s->names = NULL;
    s->len = 0;
    s->cap = 0;
}

static int cc__name_set_contains(const cc_name_set* s, const char* name, size_t len) {
    if (!name || len == 0) return 0;
    for (size_t i = 0; i < s->len; i++) {
        const char* n = s->names[i];
        if (!n) continue;
        size_t sl = strlen(n);
        if (sl == len && memcmp(n, name, len) == 0) return 1;
    }
    return 0;
}

static void cc__name_set_add(cc_name_set* s, const char* name, size_t len) {
    if (!name || len == 0) return;
    if (cc__name_set_contains(s, name, len)) return;
    if (s->len == s->cap) {
        size_t nc = s->cap ? s->cap * 2 : 16;
        char** nb = (char**)realloc(s->names, nc * sizeof(char*));
        if (!nb) return;
        s->names = nb;
        s->cap = nc;
    }
    char* dup = (char*)malloc(len + 1);
    if (!dup) return;
    memcpy(dup, name, len);
    dup[len] = 0;
    s->names[s->len++] = dup;
}

static void cc__name_err_map_clear(cc_name_err_map* m) {
    for (size_t i = 0; i < m->len; i++) {
        free(m->names[i]);
        free(m->err_types[i]);
    }
    free(m->names);
    free(m->err_types);
    m->names = NULL;
    m->err_types = NULL;
    m->len = 0;
    m->cap = 0;
}

static const char* cc__name_err_map_get(const cc_name_err_map* m,
                                         const char* name, size_t len) {
    if (!name || len == 0) return NULL;
    for (size_t i = 0; i < m->len; i++) {
        const char* n = m->names[i];
        if (!n) continue;
        size_t sl = strlen(n);
        if (sl == len && memcmp(n, name, len) == 0) return m->err_types[i];
    }
    return NULL;
}

static void cc__name_err_map_set(cc_name_err_map* m,
                                  const char* name, size_t name_len,
                                  const char* err, size_t err_len) {
    if (!name || name_len == 0 || !err || err_len == 0) return;
    for (size_t i = 0; i < m->len; i++) {
        const char* n = m->names[i];
        if (!n) continue;
        size_t sl = strlen(n);
        if (sl == name_len && memcmp(n, name, name_len) == 0) {
            /* Already recorded.  Keep the first association; typed decls
             * usually appear once per TU, and later include-expansion
             * passes may re-scan the same decl. */
            return;
        }
    }
    if (m->len == m->cap) {
        size_t nc = m->cap ? m->cap * 2 : 16;
        char** nn = (char**)realloc(m->names, nc * sizeof(char*));
        char** ne = (char**)realloc(m->err_types, nc * sizeof(char*));
        if (!nn || !ne) {
            free(nn); free(ne);
            return;
        }
        m->names = nn;
        m->err_types = ne;
        m->cap = nc;
    }
    char* dn = (char*)malloc(name_len + 1);
    char* de = (char*)malloc(err_len + 1);
    if (!dn || !de) {
        free(dn); free(de);
        return;
    }
    memcpy(dn, name, name_len); dn[name_len] = 0;
    memcpy(de, err, err_len);   de[err_len] = 0;
    m->names[m->len] = dn;
    m->err_types[m->len] = de;
    m->len++;
}

void cc_result_fn_registry_clear(void)                         {
    cc__name_set_clear(&g_result_fns);
    cc__name_err_map_clear(&g_result_fn_err);
}
int  cc_result_fn_registry_contains(const char* n, size_t l)   { return cc__name_set_contains(&g_result_fns, n, l); }
void cc_result_fn_registry_add(const char* n, size_t l)        { cc__name_set_add(&g_result_fns, n, l); }

void cc_result_fn_registry_add_typed(const char* name, size_t name_len,
                                      const char* err_type, size_t err_len) {
    /* Keep the name-only set in sync so slice-7 diagnostics still see this
     * fn even when callers only go through the typed path. */
    cc__name_set_add(&g_result_fns, name, name_len);
    cc__name_err_map_set(&g_result_fn_err, name, name_len, err_type, err_len);
}

int cc_result_fn_registry_get_err_type(const char* name, size_t name_len,
                                         char* out_buf, size_t out_sz) {
    if (!out_buf || out_sz == 0) return 0;
    out_buf[0] = 0;
    const char* e = cc__name_err_map_get(&g_result_fn_err, name, name_len);
    if (!e) return 0;
    size_t el = strlen(e);
    if (el + 1 > out_sz) return 0;
    memcpy(out_buf, e, el);
    out_buf[el] = 0;
    return 1;
}

