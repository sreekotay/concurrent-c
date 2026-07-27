#include "util/result_fn_registry.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "result_spec.h"
#include "util/text.h"

/* Small additive set keyed by function name. */
typedef struct {
    char**  names;
    size_t  len;
    size_t  cap;
} cc_name_set;

/* Parallel map from result-fn name -> {err_type, result_type}.
 * Kept separate from `g_result_fns` so the existing unhandled-result scan
 * (slice-7) can keep using the name-only set without paying for strings
 * when they are not needed. */
typedef struct {
    char**  names;
    char**  err_types;
    char**  result_types;
    size_t  len;
    size_t  cap;
} cc_name_meta_map;

static cc_name_set g_result_fns = { NULL, 0, 0 };
static cc_name_meta_map g_result_fn_meta = { NULL, NULL, NULL, 0, 0 };

static int cc__is_ident_start(char c) {
    return (c == '_' ||
            (c >= 'A' && c <= 'Z') ||
            (c >= 'a' && c <= 'z'));
}

static int cc__is_ident_char(char c) {
    return cc__is_ident_start(c) || (c >= '0' && c <= '9');
}

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

static void cc__name_meta_map_clear(cc_name_meta_map* m) {
    for (size_t i = 0; i < m->len; i++) {
        free(m->names[i]);
        free(m->err_types[i]);
        free(m->result_types[i]);
    }
    free(m->names);
    free(m->err_types);
    free(m->result_types);
    m->names = NULL;
    m->err_types = NULL;
    m->result_types = NULL;
    m->len = 0;
    m->cap = 0;
}

static size_t cc__name_meta_map_find(const cc_name_meta_map* m,
                                      const char* name, size_t len) {
    if (!name || len == 0) return (size_t)-1;
    for (size_t i = 0; i < m->len; i++) {
        const char* n = m->names[i];
        if (!n) continue;
        size_t sl = strlen(n);
        if (sl == len && memcmp(n, name, len) == 0) return i;
    }
    return (size_t)-1;
}

static char* cc__dup_slice(const char* s, size_t len) {
    char* d;
    if (!s || len == 0) return NULL;
    d = (char*)malloc(len + 1);
    if (!d) return NULL;
    memcpy(d, s, len);
    d[len] = 0;
    return d;
}

static void cc__name_meta_map_set(cc_name_meta_map* m,
                                   const char* name, size_t name_len,
                                   const char* err, size_t err_len,
                                   const char* result, size_t result_len) {
    size_t idx;
    if (!name || name_len == 0) return;
    if ((!err || err_len == 0) && (!result || result_len == 0)) return;

    idx = cc__name_meta_map_find(m, name, name_len);
    if (idx != (size_t)-1) {
        /* Fill missing fields only; keep the first association. */
        if (err && err_len > 0 && !m->err_types[idx]) {
            m->err_types[idx] = cc__dup_slice(err, err_len);
        }
        if (result && result_len > 0 && !m->result_types[idx]) {
            m->result_types[idx] = cc__dup_slice(result, result_len);
        }
        return;
    }

    if (m->len == m->cap) {
        size_t nc = m->cap ? m->cap * 2 : 16;
        char** nn = (char**)realloc(m->names, nc * sizeof(char*));
        char** ne = (char**)realloc(m->err_types, nc * sizeof(char*));
        char** nr = (char**)realloc(m->result_types, nc * sizeof(char*));
        if (!nn || !ne || !nr) {
            free(nn); free(ne); free(nr);
            return;
        }
        m->names = nn;
        m->err_types = ne;
        m->result_types = nr;
        m->cap = nc;
    }
    {
        char* dn = cc__dup_slice(name, name_len);
        char* de = (err && err_len > 0) ? cc__dup_slice(err, err_len) : NULL;
        char* dr = (result && result_len > 0) ? cc__dup_slice(result, result_len) : NULL;
        if (!dn) {
            free(de); free(dr);
            return;
        }
        m->names[m->len] = dn;
        m->err_types[m->len] = de;
        m->result_types[m->len] = dr;
        m->len++;
    }
}

static int cc__copy_out(const char* src, char* out_buf, size_t out_sz) {
    size_t el;
    if (!out_buf || out_sz == 0) return 0;
    out_buf[0] = 0;
    if (!src || !src[0]) return 0;
    el = strlen(src);
    if (el + 1 > out_sz) return 0;
    memcpy(out_buf, src, el);
    out_buf[el] = 0;
    return 1;
}

/* Derive Err from a concrete `CCResult_<Ok>_<Err>` name.  Prefer the
 * stdlib-predeclared table; fall back to the last `_` segment. */
static int cc__err_from_result_name(const char* result_name,
                                     char* out, size_t out_sz) {
    const CCStdlibPredeclaredResult* pre;
    const char* p;
    const char* last_us;
    size_t len;
    if (!result_name || !out || out_sz == 0) return 0;
    out[0] = 0;
    if (strncmp(result_name, "CCResult_", 9) != 0) return 0;
    pre = cc_result_spec_lookup_stdlib_predeclared(result_name);
    if (pre && pre->err_type) {
        return cc__copy_out(pre->err_type, out, out_sz);
    }
    p = result_name + 9;
    last_us = strrchr(p, '_');
    if (!last_us || last_us[1] == 0) return 0;
    len = strlen(last_us + 1);
    if (len + 1 > out_sz) return 0;
    memcpy(out, last_us + 1, len);
    out[len] = 0;
    return 1;
}

void cc_result_fn_registry_clear(void) {
    cc__name_set_clear(&g_result_fns);
    cc__name_meta_map_clear(&g_result_fn_meta);
}
int  cc_result_fn_registry_contains(const char* n, size_t l) {
    return cc__name_set_contains(&g_result_fns, n, l);
}
void cc_result_fn_registry_add(const char* n, size_t l) {
    cc__name_set_add(&g_result_fns, n, l);
}

void cc_result_fn_registry_add_typed(const char* name, size_t name_len,
                                      const char* err_type, size_t err_len,
                                      const char* result_type, size_t result_len) {
    /* Keep the name-only set in sync so slice-7 diagnostics still see this
     * fn even when callers only go through the typed path. */
    cc__name_set_add(&g_result_fns, name, name_len);
    cc__name_meta_map_set(&g_result_fn_meta, name, name_len,
                           err_type, err_len, result_type, result_len);
}

int cc_result_fn_registry_get_err_type(const char* name, size_t name_len,
                                         char* out_buf, size_t out_sz) {
    size_t idx;
    if (!out_buf || out_sz == 0) return 0;
    out_buf[0] = 0;
    idx = cc__name_meta_map_find(&g_result_fn_meta, name, name_len);
    if (idx == (size_t)-1) return 0;
    if (g_result_fn_meta.err_types[idx] && g_result_fn_meta.err_types[idx][0]) {
        return cc__copy_out(g_result_fn_meta.err_types[idx], out_buf, out_sz);
    }
    /* Derive from stored result type when err was not recorded. */
    if (g_result_fn_meta.result_types[idx]) {
        return cc__err_from_result_name(g_result_fn_meta.result_types[idx],
                                         out_buf, out_sz);
    }
    return 0;
}

int cc_result_fn_registry_get_result_type(const char* name, size_t name_len,
                                            char* out_buf, size_t out_sz) {
    size_t idx;
    if (!out_buf || out_sz == 0) return 0;
    out_buf[0] = 0;
    idx = cc__name_meta_map_find(&g_result_fn_meta, name, name_len);
    if (idx == (size_t)-1) return 0;
    return cc__copy_out(g_result_fn_meta.result_types[idx], out_buf, out_sz);
}

static size_t cc__skip_ws(const char* s, size_t n, size_t i) {
    while (i < n && isspace((unsigned char)s[i])) i++;
    return i;
}

static size_t cc__scan_back_type_start(const char* s, size_t end) {
    size_t i = end;
    while (i > 0) {
        char c = s[i - 1];
        if (cc__is_ident_char(c) || c == '*' || c == ' ' || c == '\t') {
            i--;
            continue;
        }
        break;
    }
    while (i < end && isspace((unsigned char)s[i])) i++;
    return i;
}

void cc_result_fn_registry_scan_source(const char* src, size_t n) {
    size_t i = 0;
    int in_lc = 0, in_bc = 0, in_str = 0, in_chr = 0;
    if (!src || n == 0) return;

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

        /* `CCResult_<Ok>_<Err> name(` */
        if (c == 'C' && i + 9 < n && memcmp(src + i, "CCResult_", 9) == 0 &&
            (i == 0 || !cc__is_ident_char(src[i - 1]))) {
            size_t ty_a = i;
            size_t j = i + 9;
            char result_type[256];
            char err_type[128];
            size_t ty_len;
            while (j < n && cc__is_ident_char(src[j])) j++;
            ty_len = j - ty_a;
            if (ty_len > 9 && ty_len + 1 <= sizeof(result_type)) {
                size_t k = cc__skip_ws(src, n, j);
                while (k < n && src[k] == '*') {
                    k++;
                    k = cc__skip_ws(src, n, k);
                }
                if (k < n && cc__is_ident_start(src[k])) {
                    size_t nm_a = k;
                    size_t nm_b;
                    while (k < n && cc__is_ident_char(src[k])) k++;
                    nm_b = k;
                    k = cc__skip_ws(src, n, k);
                    if (k < n && src[k] == '(' && nm_b > nm_a) {
                        memcpy(result_type, src + ty_a, ty_len);
                        result_type[ty_len] = 0;
                        err_type[0] = 0;
                        (void)cc__err_from_result_name(result_type, err_type, sizeof(err_type));
                        cc_result_fn_registry_add_typed(src + nm_a, nm_b - nm_a,
                                                         err_type[0] ? err_type : NULL,
                                                         err_type[0] ? strlen(err_type) : 0,
                                                         result_type, ty_len);
                    }
                }
            }
            i = j;
            continue;
        }

        /* `Ok !>(Err) name(` surface sugar */
        if (c == '!' && c2 == '>' && i > 0) {
            size_t j = i + 2;
            j = cc__skip_ws(src, n, j);
            if (j < n && src[j] == '(') {
                size_t rp = 0;
                if (cc_find_matching_paren(src, n, j, &rp) && rp < n) {
                    size_t err_a = j + 1, err_b = rp;
                    size_t k, ok_a, ok_b;
                    char mangled_ok[128], mangled_err[128], result_type[256];
                    while (err_a < err_b && isspace((unsigned char)src[err_a])) err_a++;
                    while (err_b > err_a && isspace((unsigned char)src[err_b - 1])) err_b--;
                    k = cc__skip_ws(src, n, rp + 1);
                    if (err_b > err_a && k < n && cc__is_ident_start(src[k])) {
                        size_t nm_a = k, nm_b;
                        while (k < n && cc__is_ident_char(src[k])) k++;
                        nm_b = k;
                        k = cc__skip_ws(src, n, k);
                        if (k < n && src[k] == '(' && nm_b > nm_a) {
                            ok_b = i;
                            while (ok_b > 0 && isspace((unsigned char)src[ok_b - 1])) ok_b--;
                            ok_a = cc__scan_back_type_start(src, ok_b);
                            /* Strip leading declspecs that are not the ok type. */
                            while (ok_a < ok_b) {
                                static const char* specs[] = {
                                    "static", "inline", "extern", "const",
                                    "volatile", "register", "_Noreturn", NULL
                                };
                                size_t si;
                                int matched = 0;
                                for (si = 0; specs[si]; si++) {
                                    size_t sl = strlen(specs[si]);
                                    if (ok_a + sl <= ok_b &&
                                        memcmp(src + ok_a, specs[si], sl) == 0 &&
                                        (ok_a + sl == ok_b ||
                                         !cc__is_ident_char(src[ok_a + sl]))) {
                                        ok_a += sl;
                                        while (ok_a < ok_b &&
                                               isspace((unsigned char)src[ok_a]))
                                            ok_a++;
                                        matched = 1;
                                        break;
                                    }
                                }
                                if (!matched) break;
                            }
                            if (ok_b > ok_a) {
                                cc_result_spec_mangle_type(src + ok_a, ok_b - ok_a,
                                                            mangled_ok, sizeof(mangled_ok));
                                cc_result_spec_mangle_type(src + err_a, err_b - err_a,
                                                            mangled_err, sizeof(mangled_err));
                                if (mangled_ok[0] && mangled_err[0]) {
                                    int wrote = snprintf(result_type, sizeof(result_type),
                                                         "CCResult_%s_%s",
                                                         mangled_ok, mangled_err);
                                    if (wrote > 0 && (size_t)wrote < sizeof(result_type)) {
                                        cc_result_fn_registry_add_typed(
                                            src + nm_a, nm_b - nm_a,
                                            src + err_a, err_b - err_a,
                                            result_type, (size_t)wrote);
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }

        i++;
    }
}
