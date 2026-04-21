#include "symbols.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char* name;
    long long value;
} CCConstEntry;

typedef struct {
    char* name;
    unsigned int attrs;
} CCFuncAttrEntry;

typedef struct {
    char* method;
    char* callee;
} CCTypeUfcsEntry;

typedef struct {
    char* type_name;
    char* create_calls[5];
    const void* create_callable;
    void* create_owner;
    CCOwnedResourceFreeFn create_owner_free;
    char* pre_destroy_call;
    char* destroy_call;
    const void* ufcs_callable;
    int ufcs_callable_is_legacy_compat;
    void* ufcs_owner;
    CCOwnedResourceFreeFn ufcs_owner_free;
    CCTypeUfcsEntry* ufcs;
    size_t ufcs_count;
    size_t ufcs_capacity;
} CCTypeEntry;

struct CCSymbolTable {
    CCConstEntry* entries;
    size_t count;
    size_t capacity;

    CCFuncAttrEntry* fn_attrs;
    size_t fn_count;
    size_t fn_capacity;

    CCTypeEntry* types;
    size_t type_count;
    size_t type_capacity;
};

static int ensure_capacity(CCSymbolTable* t, size_t needed) {
    if (t->capacity >= needed) return 0;
    size_t new_cap = t->capacity ? t->capacity * 2 : 8;
    while (new_cap < needed) {
        new_cap *= 2;
    }
    CCConstEntry* new_entries = (CCConstEntry*)realloc(t->entries, new_cap * sizeof(CCConstEntry));
    if (!new_entries) {
        return ENOMEM;
    }
    t->entries = new_entries;
    t->capacity = new_cap;
    return 0;
}

static int ensure_fn_capacity(CCSymbolTable* t, size_t needed) {
    if (t->fn_capacity >= needed) return 0;
    size_t new_cap = t->fn_capacity ? t->fn_capacity * 2 : 8;
    while (new_cap < needed) new_cap *= 2;
    CCFuncAttrEntry* nv = (CCFuncAttrEntry*)realloc(t->fn_attrs, new_cap * sizeof(CCFuncAttrEntry));
    if (!nv) return ENOMEM;
    t->fn_attrs = nv;
    t->fn_capacity = new_cap;
    return 0;
}

static int ensure_type_capacity(CCSymbolTable* t, size_t needed) {
    if (t->type_capacity >= needed) return 0;
    size_t new_cap = t->type_capacity ? t->type_capacity * 2 : 8;
    while (new_cap < needed) new_cap *= 2;
    CCTypeEntry* nv = (CCTypeEntry*)realloc(t->types, new_cap * sizeof(CCTypeEntry));
    if (!nv) return ENOMEM;
    t->types = nv;
    t->type_capacity = new_cap;
    return 0;
}

static int cc__ufcs_pattern_matches(const char* pattern, const char* type_name) {
    size_t plen;
    if (!pattern || !type_name) return 0;
    plen = strlen(pattern);
    if (plen > 0 && pattern[plen - 1] == '*') {
        return strncmp(type_name, pattern, plen - 1) == 0;
    }
    return strcmp(pattern, type_name) == 0;
}

static CCTypeEntry* cc__find_type_entry(CCSymbolTable* t, const char* type_name) {
    if (!t || !type_name) return NULL;
    for (size_t i = 0; i < t->type_count; ++i) {
        if (strcmp(t->types[i].type_name, type_name) == 0) {
            return &t->types[i];
        }
    }
    return NULL;
}

static int cc__ensure_type_entry(CCSymbolTable* t, const char* type_name, CCTypeEntry** out_entry) {
    CCTypeEntry* entry = NULL;
    char* copy = NULL;
    int err;
    if (!t || !type_name || !out_entry) return EINVAL;
    entry = cc__find_type_entry(t, type_name);
    if (entry) {
        *out_entry = entry;
        return 0;
    }
    err = ensure_type_capacity(t, t->type_count + 1);
    if (err != 0) return err;
    copy = strdup(type_name);
    if (!copy) return ENOMEM;
    entry = &t->types[t->type_count];
    memset(entry, 0, sizeof(*entry));
    entry->type_name = copy;
    t->type_count += 1;
    *out_entry = entry;
    return 0;
}

static int cc__ensure_type_ufcs_capacity(CCTypeEntry* entry, size_t needed) {
    if (!entry) return EINVAL;
    if (entry->ufcs_capacity >= needed) return 0;
    size_t new_cap = entry->ufcs_capacity ? entry->ufcs_capacity * 2 : 4;
    while (new_cap < needed) new_cap *= 2;
    CCTypeUfcsEntry* nv = (CCTypeUfcsEntry*)realloc(entry->ufcs, new_cap * sizeof(CCTypeUfcsEntry));
    if (!nv) return ENOMEM;
    entry->ufcs = nv;
    entry->ufcs_capacity = new_cap;
    return 0;
}

CCSymbolTable* cc_symbols_new(void) {
    CCSymbolTable* t = (CCSymbolTable*)calloc(1, sizeof(CCSymbolTable));
    return t;
}

void cc_symbols_free(CCSymbolTable* t) {
    if (!t) return;
    for (size_t i = 0; i < t->count; ++i) {
        free(t->entries[i].name);
    }
    for (size_t i = 0; i < t->fn_count; ++i) {
        free(t->fn_attrs[i].name);
    }
    for (size_t i = 0; i < t->type_count; ++i) {
        free(t->types[i].type_name);
        for (size_t a = 0; a < sizeof(t->types[i].create_calls) / sizeof(t->types[i].create_calls[0]); ++a) {
            free(t->types[i].create_calls[a]);
        }
        if (t->types[i].create_owner && t->types[i].create_owner_free) {
            t->types[i].create_owner_free(t->types[i].create_owner);
        }
        free(t->types[i].pre_destroy_call);
        free(t->types[i].destroy_call);
        if (t->types[i].ufcs_owner && t->types[i].ufcs_owner_free) {
            t->types[i].ufcs_owner_free(t->types[i].ufcs_owner);
        }
        for (size_t u = 0; u < t->types[i].ufcs_count; ++u) {
            free(t->types[i].ufcs[u].method);
            free(t->types[i].ufcs[u].callee);
        }
        free(t->types[i].ufcs);
    }
    free(t->entries);
    free(t->fn_attrs);
    free(t->types);
    free(t);
}

int cc_symbols_add_const(CCSymbolTable* t, const char* name, long long value) {
    if (!t || !name) {
        return EINVAL;
    }
    // Last writer wins: override existing entry if present.
    for (size_t i = 0; i < t->count; ++i) {
        if (strcmp(t->entries[i].name, name) == 0) {
            t->entries[i].value = value;
            return 0;
        }
    }
    int err = ensure_capacity(t, t->count + 1);
    if (err != 0) {
        return err;
    }
    char* copied = strdup(name);
    if (!copied) {
        return ENOMEM;
    }
    t->entries[t->count].name = copied;
    t->entries[t->count].value = value;
    t->count += 1;
    return 0;
}

int cc_symbols_add_predefined(CCSymbolTable* t, const CCConstBinding* bindings, size_t count) {
    if (!t || (!bindings && count > 0)) {
        return EINVAL;
    }
    for (size_t i = 0; i < count; ++i) {
        const CCConstBinding* b = &bindings[i];
        if (!b->name) {
            return EINVAL;
        }
        int err = cc_symbols_add_const(t, b->name, b->value);
        if (err != 0) {
            return err;
        }
    }
    return 0;
}

int cc_symbols_lookup_const(CCSymbolTable* t, const char* name, long long* out_value) {
    if (!t || !name || !out_value) {
        return EINVAL;
    }
    for (size_t i = 0; i < t->count; ++i) {
        if (strcmp(t->entries[i].name, name) == 0) {
            *out_value = t->entries[i].value;
            return 0;
        }
    }
    return ENOENT;
}

int cc_symbols_set_fn_attrs(CCSymbolTable* t, const char* name, unsigned int attrs) {
    if (!t || !name) return EINVAL;
    for (size_t i = 0; i < t->fn_count; ++i) {
        if (strcmp(t->fn_attrs[i].name, name) == 0) {
            t->fn_attrs[i].attrs = attrs;
            return 0;
        }
    }
    int err = ensure_fn_capacity(t, t->fn_count + 1);
    if (err != 0) return err;
    char* copied = strdup(name);
    if (!copied) return ENOMEM;
    t->fn_attrs[t->fn_count].name = copied;
    t->fn_attrs[t->fn_count].attrs = attrs;
    t->fn_count += 1;
    return 0;
}

int cc_symbols_lookup_fn_attrs(CCSymbolTable* t, const char* name, unsigned int* out_attrs) {
    if (!t || !name || !out_attrs) return EINVAL;
    for (size_t i = 0; i < t->fn_count; ++i) {
        if (strcmp(t->fn_attrs[i].name, name) == 0) {
            *out_attrs = t->fn_attrs[i].attrs;
            return 0;
        }
    }
    return ENOENT;
}

int cc_symbols_add_type_create_call(CCSymbolTable* t, const char* type_name, int arity, const char* callee) {
    CCTypeEntry* entry = NULL;
    char* copy = NULL;
    int err;
    if (!t || !type_name || !callee || arity < 0 || arity >= 5) return EINVAL;
    err = cc__ensure_type_entry(t, type_name, &entry);
    if (err != 0) return err;
    copy = strdup(callee);
    if (!copy) return ENOMEM;
    free(entry->create_calls[arity]);
    entry->create_calls[arity] = copy;
    return 0;
}

int cc_symbols_lookup_type_create_call(CCSymbolTable* t, const char* type_name, int arity, const char** out_callee) {
    CCTypeEntry* entry = NULL;
    if (!t || !type_name || !out_callee || arity < 0 || arity >= 5) return EINVAL;
    entry = cc__find_type_entry(t, type_name);
    if (!entry || !entry->create_calls[arity]) return ENOENT;
    *out_callee = entry->create_calls[arity];
    return 0;
}

int cc_symbols_set_type_create_callable(CCSymbolTable* t,
                                        const char* type_name,
                                        const void* fn_ptr,
                                        void* owner,
                                        CCOwnedResourceFreeFn owner_free) {
    CCTypeEntry* entry = NULL;
    int err;
    if (!t || !type_name || !fn_ptr) return EINVAL;
    err = cc__ensure_type_entry(t, type_name, &entry);
    if (err != 0) return err;
    if (entry->create_owner && entry->create_owner_free) {
        entry->create_owner_free(entry->create_owner);
    }
    entry->create_callable = fn_ptr;
    entry->create_owner = owner;
    entry->create_owner_free = owner_free;
    return 0;
}

int cc_symbols_lookup_type_create_callable(CCSymbolTable* t,
                                           const char* type_name,
                                           const void** out_fn_ptr) {
    CCTypeEntry* entry = NULL;
    if (!t || !type_name || !out_fn_ptr) return EINVAL;
    entry = cc__find_type_entry(t, type_name);
    if (!entry || !entry->create_callable) return ENOENT;
    *out_fn_ptr = entry->create_callable;
    return 0;
}

int cc_symbols_set_type_destroy_call(CCSymbolTable* t, const char* type_name, const char* callee) {
    CCTypeEntry* entry = NULL;
    char* copy = NULL;
    int err;
    if (!t || !type_name || !callee) return EINVAL;
    err = cc__ensure_type_entry(t, type_name, &entry);
    if (err != 0) return err;
    copy = strdup(callee);
    if (!copy) return ENOMEM;
    free(entry->destroy_call);
    entry->destroy_call = copy;
    return 0;
}

int cc_symbols_set_type_pre_destroy_call(CCSymbolTable* t, const char* type_name, const char* callee) {
    CCTypeEntry* entry = NULL;
    char* copy = NULL;
    int err;
    if (!t || !type_name || !callee) return EINVAL;
    err = cc__ensure_type_entry(t, type_name, &entry);
    if (err != 0) return err;
    copy = strdup(callee);
    if (!copy) return ENOMEM;
    free(entry->pre_destroy_call);
    entry->pre_destroy_call = copy;
    return 0;
}

int cc_symbols_lookup_type_pre_destroy_call(CCSymbolTable* t, const char* type_name, const char** out_callee) {
    CCTypeEntry* entry = NULL;
    if (!t || !type_name || !out_callee) return EINVAL;
    entry = cc__find_type_entry(t, type_name);
    if (!entry || !entry->pre_destroy_call) return ENOENT;
    *out_callee = entry->pre_destroy_call;
    return 0;
}

int cc_symbols_lookup_type_destroy_call(CCSymbolTable* t, const char* type_name, const char** out_callee) {
    CCTypeEntry* entry = NULL;
    if (!t || !type_name || !out_callee) return EINVAL;
    entry = cc__find_type_entry(t, type_name);
    if (!entry || !entry->destroy_call) return ENOENT;
    *out_callee = entry->destroy_call;
    return 0;
}

int cc_symbols_add_type_ufcs_value(CCSymbolTable* t, const char* type_name, const char* method, const char* callee) {
    CCTypeEntry* entry = NULL;
    char* method_copy = NULL;
    char* callee_copy = NULL;
    int err;
    if (!t || !type_name || !method || !callee) return EINVAL;
    err = cc__ensure_type_entry(t, type_name, &entry);
    if (err != 0) return err;
    for (size_t i = 0; i < entry->ufcs_count; ++i) {
        if (strcmp(entry->ufcs[i].method, method) == 0) {
            callee_copy = strdup(callee);
            if (!callee_copy) return ENOMEM;
            free(entry->ufcs[i].callee);
            entry->ufcs[i].callee = callee_copy;
            return 0;
        }
    }
    err = cc__ensure_type_ufcs_capacity(entry, entry->ufcs_count + 1);
    if (err != 0) return err;
    method_copy = strdup(method);
    callee_copy = strdup(callee);
    if (!method_copy || !callee_copy) {
        free(method_copy);
        free(callee_copy);
        return ENOMEM;
    }
    entry->ufcs[entry->ufcs_count].method = method_copy;
    entry->ufcs[entry->ufcs_count].callee = callee_copy;
    entry->ufcs_count += 1;
    return 0;
}

int cc_symbols_lookup_type_ufcs_value(CCSymbolTable* t,
                                      const char* type_name,
                                      const char* method,
                                      const char** out_callee) {
    CCTypeEntry* best_entry = NULL;
    size_t best_len = 0;
    if (!t || !type_name || !method || !out_callee) return EINVAL;
    for (size_t ti = 0; ti < t->type_count; ++ti) {
        CCTypeEntry* entry = &t->types[ti];
        size_t plen;
        size_t score;
        if (!entry->ufcs_count) continue;
        if (!cc__ufcs_pattern_matches(entry->type_name, type_name)) continue;
        plen = strlen(entry->type_name);
        score = (plen > 0 && entry->type_name[plen - 1] == '*') ? plen - 1 : plen;
        for (size_t i = 0; i < entry->ufcs_count; ++i) {
            if (strcmp(entry->ufcs[i].method, method) != 0) continue;
            if (!best_entry || score > best_len) {
                best_entry = entry;
                best_len = score;
                *out_callee = entry->ufcs[i].callee;
            }
        }
    }
    return best_entry ? 0 : ENOENT;
}

int cc_symbols_set_type_ufcs_callable(CCSymbolTable* t,
                                      const char* type_name,
                                      const void* fn_ptr,
                                      void* owner,
                                      CCOwnedResourceFreeFn owner_free) {
    CCTypeEntry* entry = NULL;
    int err;
    if (!t || !type_name || !fn_ptr) return EINVAL;
    err = cc__ensure_type_entry(t, type_name, &entry);
    if (err != 0) return err;
    if (entry->ufcs_owner && entry->ufcs_owner_free) {
        entry->ufcs_owner_free(entry->ufcs_owner);
    }
    entry->ufcs_callable = fn_ptr;
    entry->ufcs_callable_is_legacy_compat = 0;
    entry->ufcs_owner = owner;
    entry->ufcs_owner_free = owner_free;
    return 0;
}

int cc_symbols_add_legacy_type_ufcs_callable(CCSymbolTable* t,
                                             const char* type_name,
                                             const void* fn_ptr,
                                             void* owner,
                                             CCOwnedResourceFreeFn owner_free) {
    CCTypeEntry* entry = NULL;
    int err;
    if (!t || !type_name || !fn_ptr) return EINVAL;
    err = cc__ensure_type_entry(t, type_name, &entry);
    if (err != 0) return err;
    if (entry->ufcs_callable && !entry->ufcs_callable_is_legacy_compat) {
        if (owner && owner_free) owner_free(owner);
        return 0;
    }
    if (entry->ufcs_owner && entry->ufcs_owner_free) {
        entry->ufcs_owner_free(entry->ufcs_owner);
    }
    entry->ufcs_callable = fn_ptr;
    entry->ufcs_callable_is_legacy_compat = 1;
    entry->ufcs_owner = owner;
    entry->ufcs_owner_free = owner_free;
    return 0;
}

int cc_symbols_lookup_type_ufcs_callable_ex(CCSymbolTable* t,
                                            const char* type_name,
                                            const void** out_fn_ptr,
                                            size_t* out_score) {
    CCTypeEntry* best_entry = NULL;
    size_t best_len = 0;
    if (!t || !type_name || !out_fn_ptr) return EINVAL;
    for (size_t ti = 0; ti < t->type_count; ++ti) {
        CCTypeEntry* entry = &t->types[ti];
        size_t plen;
        size_t score;
        if (!entry->ufcs_callable) continue;
        if (!cc__ufcs_pattern_matches(entry->type_name, type_name)) continue;
        plen = strlen(entry->type_name);
        score = (plen > 0 && entry->type_name[plen - 1] == '*') ? plen - 1 : plen;
        if (!best_entry || score > best_len) {
            best_entry = entry;
            best_len = score;
            *out_fn_ptr = entry->ufcs_callable;
        }
    }
    if (out_score) *out_score = best_len;
    return best_entry ? 0 : ENOENT;
}

int cc_symbols_lookup_type_ufcs_callable(CCSymbolTable* t,
                                         const char* type_name,
                                         const void** out_fn_ptr) {
    return cc_symbols_lookup_type_ufcs_callable_ex(t, type_name, out_fn_ptr, NULL);
}

size_t cc_symbols_type_count(CCSymbolTable* t) {
    return t ? t->type_count : 0;
}

const char* cc_symbols_type_name(CCSymbolTable* t, size_t idx) {
    if (!t || idx >= t->type_count) return NULL;
    return t->types[idx].type_name;
}

static int cc__match_kw_reg(const char* src, size_t n, size_t pos, const char* kw) {
    size_t klen = strlen(kw);
    if (!src || !kw || pos + klen > n) return 0;
    if (memcmp(src + pos, kw, klen) != 0) return 0;
    if (pos > 0 && (isalnum((unsigned char)src[pos - 1]) || src[pos - 1] == '_')) return 0;
    if (pos + klen < n && (isalnum((unsigned char)src[pos + klen]) || src[pos + klen] == '_')) return 0;
    return 1;
}

static size_t cc__skip_ws_reg(const char* src, size_t n, size_t i) {
    for (;;) {
        while (i < n && isspace((unsigned char)src[i])) i++;
        if (i + 1 < n && src[i] == '/' && src[i + 1] == '/') {
            i += 2;
            while (i < n && src[i] != '\n') i++;
            continue;
        }
        if (i + 1 < n && src[i] == '/' && src[i + 1] == '*') {
            i += 2;
            while (i + 1 < n && !(src[i] == '*' && src[i + 1] == '/')) i++;
            if (i + 1 < n) i += 2;
            continue;
        }
        break;
    }
    return i;
}

static int cc__find_matching_reg(const char* src, size_t n, size_t open_idx, char open_ch, char close_ch, size_t* out_close) {
    int depth = 0;
    int in_str = 0, in_chr = 0, in_lc = 0, in_bc = 0;
    if (!src || open_idx >= n || src[open_idx] != open_ch) return 0;
    for (size_t i = open_idx; i < n; ++i) {
        char c = src[i];
        char c2 = (i + 1 < n) ? src[i + 1] : 0;
        if (in_lc) { if (c == '\n') in_lc = 0; continue; }
        if (in_bc) { if (c == '*' && c2 == '/') { in_bc = 0; i++; } continue; }
        if (in_str) { if (c == '\\' && c2) { i++; continue; } if (c == '"') in_str = 0; continue; }
        if (in_chr) { if (c == '\\' && c2) { i++; continue; } if (c == '\'') in_chr = 0; continue; }
        if (c == '/' && c2 == '/') { in_lc = 1; i++; continue; }
        if (c == '/' && c2 == '*') { in_bc = 1; i++; continue; }
        if (c == '"') { in_str = 1; continue; }
        if (c == '\'') { in_chr = 1; continue; }
        if (c == open_ch) depth++;
        else if (c == close_ch) {
            depth--;
            if (depth == 0) {
                *out_close = i;
                return 1;
            }
        }
    }
    return 0;
}

static int cc__parse_string_literal_reg(const char* src, size_t n, size_t* io_pos, char* out, size_t out_sz) {
    size_t p = io_pos ? *io_pos : 0;
    size_t len = 0;
    if (!src || !io_pos || !out || out_sz == 0 || p >= n || src[p] != '"') return 0;
    p++;
    while (p < n && src[p] != '"') {
        if (src[p] == '\\' && p + 1 < n) {
            char c = src[p + 1];
            if (len + 1 >= out_sz) return 0;
            out[len++] = (c == 'n') ? '\n' : (c == 't') ? '\t' : c;
            p += 2;
            continue;
        }
        if (len + 1 >= out_sz) return 0;
        out[len++] = src[p++];
    }
    if (p >= n || src[p] != '"') return 0;
    out[len] = '\0';
    *io_pos = p + 1;
    return 1;
}

static int cc__parse_helper_call_1(const char* src,
                                   size_t n,
                                   size_t* io_pos,
                                   const char* helper,
                                   char* out_a,
                                   size_t out_a_sz) {
    size_t p = cc__skip_ws_reg(src, n, *io_pos);
    size_t lpar = 0, rpar = 0;
    if (!cc__match_kw_reg(src, n, p, helper)) return 0;
    p += strlen(helper);
    p = cc__skip_ws_reg(src, n, p);
    if (p >= n || src[p] != '(') return 0;
    lpar = p;
    if (!cc__find_matching_reg(src, n, lpar, '(', ')', &rpar)) return 0;
    p = cc__skip_ws_reg(src, rpar, lpar + 1);
    if (!cc__parse_string_literal_reg(src, rpar, &p, out_a, out_a_sz)) return 0;
    p = cc__skip_ws_reg(src, rpar, p);
    if (p != rpar) return 0;
    *io_pos = rpar + 1;
    return 1;
}

static int cc__parse_helper_call_2(const char* src,
                                   size_t n,
                                   size_t* io_pos,
                                   const char* helper,
                                   char* out_a,
                                   size_t out_a_sz,
                                   char* out_b,
                                   size_t out_b_sz) {
    size_t p = cc__skip_ws_reg(src, n, *io_pos);
    size_t lpar = 0, rpar = 0;
    if (!cc__match_kw_reg(src, n, p, helper)) return 0;
    p += strlen(helper);
    p = cc__skip_ws_reg(src, n, p);
    if (p >= n || src[p] != '(') return 0;
    lpar = p;
    if (!cc__find_matching_reg(src, n, lpar, '(', ')', &rpar)) return 0;
    p = cc__skip_ws_reg(src, rpar, lpar + 1);
    if (!cc__parse_string_literal_reg(src, rpar, &p, out_a, out_a_sz)) return 0;
    p = cc__skip_ws_reg(src, rpar, p);
    if (p >= rpar || src[p] != ',') return 0;
    p = cc__skip_ws_reg(src, rpar, p + 1);
    if (!cc__parse_string_literal_reg(src, rpar, &p, out_b, out_b_sz)) return 0;
    p = cc__skip_ws_reg(src, rpar, p);
    if (p != rpar) return 0;
    *io_pos = rpar + 1;
    return 1;
}

static int cc__parse_type_hooks_object(CCSymbolTable* t,
                                       const char* input_path,
                                       const char* logical_file,
                                       const char* type_name,
                                       const char* src,
                                       size_t obj_l,
                                       size_t obj_r,
                                       CCTypeRegistrationCreateCallback create_callback,
                                       void* create_user_ctx,
                                       CCTypeRegistrationUfcsCallback ufcs_callback,
                                       void* ufcs_user_ctx) {
    size_t p = obj_l + 1;
    while (p < obj_r) {
        char create_callee[256];
        char destroy_callee[256];
        p = cc__skip_ws_reg(src, obj_r, p);
        if (p >= obj_r) break;
        if (src[p] == ',') {
            p++;
            continue;
        }
        if (src[p] != '.') {
            fprintf(stderr, "%s: error: malformed cc_type_register(\"%s\", ...) hooks object\n",
                    input_path ? input_path : "<input>", type_name);
            return -1;
        }
        p++;
        if (cc__match_kw_reg(src, obj_r, p, "create")) {
            char create_callee2[256];
            size_t expr_s = 0;
            size_t expr_e = 0;
            size_t scan = 0;
            size_t depth = 0;
            int in_str = 0, in_chr = 0;
            size_t parse_pos = 0;
            p += strlen("create");
            p = cc__skip_ws_reg(src, obj_r, p);
            if (p >= obj_r || src[p] != '=') return -1;
            p = cc__skip_ws_reg(src, obj_r, p + 1);
            expr_s = p;
            scan = p;
            while (scan < obj_r) {
                char c = src[scan];
                char c2 = (scan + 1 < obj_r) ? src[scan + 1] : 0;
                if (in_str) {
                    if (c == '\\' && c2) { scan += 2; continue; }
                    if (c == '"') in_str = 0;
                    scan++;
                    continue;
                }
                if (in_chr) {
                    if (c == '\\' && c2) { scan += 2; continue; }
                    if (c == '\'') in_chr = 0;
                    scan++;
                    continue;
                }
                if (c == '"') { in_str = 1; scan++; continue; }
                if (c == '\'') { in_chr = 1; scan++; continue; }
                if (c == '(' || c == '{' || c == '[') { depth++; scan++; continue; }
                if (c == ')' || c == '}' || c == ']') {
                    if (depth == 0) break;
                    depth--;
                    scan++;
                    continue;
                }
                if (c == ',' && depth == 0) break;
                scan++;
            }
            expr_e = scan;
            while (expr_s < expr_e && isspace((unsigned char)src[expr_s])) expr_s++;
            while (expr_e > expr_s && isspace((unsigned char)src[expr_e - 1])) expr_e--;
            parse_pos = expr_s;
            if (cc__parse_helper_call_2(src, expr_e, &parse_pos, "cc_type_create_overloads",
                                        create_callee, sizeof(create_callee),
                                        create_callee2, sizeof(create_callee2)) &&
                parse_pos == expr_e) {
                if (cc_symbols_add_type_create_call(t, type_name, 1, create_callee) != 0) return -1;
                if (cc_symbols_add_type_create_call(t, type_name, 2, create_callee2) != 0) return -1;
                p = scan;
                continue;
            }
            parse_pos = expr_s;
            if (cc__parse_helper_call_1(src, expr_e, &parse_pos, "cc_type_create_call", create_callee, sizeof(create_callee)) &&
                parse_pos == expr_e) {
                if (cc_symbols_add_type_create_call(t, type_name, 1, create_callee) != 0) return -1;
                p = scan;
                continue;
            }
            if (cc__match_kw_reg(src, expr_e, expr_s, "cc_type_create_hook")) {
                size_t hook_pos = cc__skip_ws_reg(src, expr_e, expr_s + strlen("cc_type_create_hook"));
                size_t hook_rpar = 0;
                if (hook_pos < expr_e && src[hook_pos] == '(' &&
                    cc__find_matching_reg(src, expr_e, hook_pos, '(', ')', &hook_rpar)) {
                    size_t inner_s = cc__skip_ws_reg(src, hook_rpar, hook_pos + 1);
                    size_t inner_e = hook_rpar;
                    while (inner_e > inner_s && isspace((unsigned char)src[inner_e - 1])) inner_e--;
                    if (cc__skip_ws_reg(src, expr_e, hook_rpar + 1) == expr_e) {
                        expr_s = inner_s;
                        expr_e = inner_e;
                    }
                }
            }
            if (create_callback && expr_e > expr_s) {
                if (create_callback(t,
                                    input_path,
                                    logical_file,
                                    type_name,
                                    src + expr_s,
                                    expr_e - expr_s,
                                    create_user_ctx) != 0) {
                    return -1;
                }
                p = scan;
                continue;
            }
            p = scan;
            continue;
        }
        if (cc__match_kw_reg(src, obj_r, p, "create1")) {
            p += strlen("create1");
            p = cc__skip_ws_reg(src, obj_r, p);
            if (p >= obj_r || src[p] != '=') return -1;
            p++;
            if (!cc__parse_helper_call_1(src, obj_r, &p, "cc_type_create_call", create_callee, sizeof(create_callee))) return -1;
            if (cc_symbols_add_type_create_call(t, type_name, 1, create_callee) != 0) return -1;
            continue;
        }
        if (cc__match_kw_reg(src, obj_r, p, "create2")) {
            p += strlen("create2");
            p = cc__skip_ws_reg(src, obj_r, p);
            if (p >= obj_r || src[p] != '=') return -1;
            p++;
            if (!cc__parse_helper_call_1(src, obj_r, &p, "cc_type_create_call", create_callee, sizeof(create_callee))) return -1;
            if (cc_symbols_add_type_create_call(t, type_name, 2, create_callee) != 0) return -1;
            continue;
        }
        if (cc__match_kw_reg(src, obj_r, p, "destroy")) {
            char destroy_pre_callee[256];
            p += strlen("destroy");
            p = cc__skip_ws_reg(src, obj_r, p);
            if (p >= obj_r || src[p] != '=') return -1;
            p++;
            if (cc__parse_helper_call_2(src, obj_r, &p, "cc_type_destroy_hooks",
                                        destroy_pre_callee, sizeof(destroy_pre_callee),
                                        destroy_callee, sizeof(destroy_callee))) {
                if (cc_symbols_set_type_pre_destroy_call(t, type_name, destroy_pre_callee) != 0) return -1;
                if (cc_symbols_set_type_destroy_call(t, type_name, destroy_callee) != 0) return -1;
                continue;
            }
            if (cc__parse_helper_call_1(src, obj_r, &p, "cc_type_pre_destroy_call",
                                        destroy_pre_callee, sizeof(destroy_pre_callee))) {
                if (cc_symbols_set_type_pre_destroy_call(t, type_name, destroy_pre_callee) != 0) return -1;
                continue;
            }
            if (!cc__parse_helper_call_1(src, obj_r, &p, "cc_type_destroy_call", destroy_callee, sizeof(destroy_callee))) return -1;
            if (cc_symbols_set_type_destroy_call(t, type_name, destroy_callee) != 0) return -1;
            continue;
        }
        if (cc__match_kw_reg(src, obj_r, p, "ufcs")) {
            size_t expr_s = 0;
            size_t expr_e = 0;
            size_t depth = 0;
            int in_str = 0, in_chr = 0;
            p += strlen("ufcs");
            p = cc__skip_ws_reg(src, obj_r, p);
            if (p >= obj_r || src[p] != '=') return -1;
            p = cc__skip_ws_reg(src, obj_r, p + 1);
            expr_s = p;
            while (p < obj_r) {
                char c = src[p];
                char c2 = (p + 1 < obj_r) ? src[p + 1] : 0;
                if (in_str) {
                    if (c == '\\' && c2) { p += 2; continue; }
                    if (c == '"') in_str = 0;
                    p++;
                    continue;
                }
                if (in_chr) {
                    if (c == '\\' && c2) { p += 2; continue; }
                    if (c == '\'') in_chr = 0;
                    p++;
                    continue;
                }
                if (c == '"') { in_str = 1; p++; continue; }
                if (c == '\'') { in_chr = 1; p++; continue; }
                if (c == '(' || c == '{' || c == '[') { depth++; p++; continue; }
                if (c == ')' || c == '}' || c == ']') {
                    if (depth == 0) break;
                    depth--;
                    p++;
                    continue;
                }
                if (c == ',' && depth == 0) {
                    p++;
                    break;
                }
                p++;
            }
            expr_e = p;
            while (expr_s < expr_e && isspace((unsigned char)src[expr_s])) expr_s++;
            while (expr_e > expr_s && isspace((unsigned char)src[expr_e - 1])) expr_e--;
            if (expr_e > expr_s && src[expr_e - 1] == ',') expr_e--;
            while (expr_e > expr_s && isspace((unsigned char)src[expr_e - 1])) expr_e--;
            if (ufcs_callback && expr_e > expr_s) {
                if (ufcs_callback(t,
                                  input_path,
                                  logical_file,
                                  type_name,
                                  src + expr_s,
                                  expr_e - expr_s,
                                  ufcs_user_ctx) != 0) {
                    return -1;
                }
            }
            continue;
        }
        fprintf(stderr, "%s: error: unsupported cc_type_register hook field for '%s'\n",
                input_path ? input_path : "<input>", type_name);
        return -1;
    }
    return 0;
}

int cc_symbols_collect_type_registrations_ex(CCSymbolTable* t,
                                             const char* input_path,
                                             const char* src,
                                             size_t n,
                                             CCTypeRegistrationCreateCallback create_callback,
                                             void* create_user_ctx,
                                             CCTypeRegistrationUfcsCallback ufcs_callback,
                                             void* ufcs_user_ctx) {
    int in_lc = 0, in_bc = 0, in_str = 0, in_chr = 0, line_start = 1;
    char logical_file[1024] = {0};
    if (!t || !src) return 0;
    for (size_t i = 0; i < n; ++i) {
        char c = src[i];
        char c2 = (i + 1 < n) ? src[i + 1] : 0;
        if (line_start && c == '#') {
            size_t p = i + 1;
            while (p < n && isspace((unsigned char)src[p]) && src[p] != '\n') p++;
            while (p < n && isdigit((unsigned char)src[p])) p++;
            while (p < n && isspace((unsigned char)src[p]) && src[p] != '\n') p++;
            if (p < n && src[p] == '"') {
                size_t q = p + 1;
                size_t out = 0;
                while (q < n && src[q] && src[q] != '"' && out + 1 < sizeof(logical_file)) {
                    logical_file[out++] = src[q++];
                }
                logical_file[out] = '\0';
            }
            while (i < n && src[i] != '\n') i++;
            line_start = 1;
            continue;
        }
        if (in_lc) { if (c == '\n') in_lc = 0; continue; }
        if (in_bc) { if (c == '*' && c2 == '/') { in_bc = 0; i++; } continue; }
        if (in_str) { if (c == '\\' && c2) { i++; continue; } if (c == '"') in_str = 0; continue; }
        if (in_chr) { if (c == '\\' && c2) { i++; continue; } if (c == '\'') in_chr = 0; continue; }
        if (c == '/' && c2 == '/') { in_lc = 1; i++; continue; }
        if (c == '/' && c2 == '*') { in_bc = 1; i++; continue; }
        if (c == '"') { in_str = 1; continue; }
        if (c == '\'') { in_chr = 1; continue; }
        line_start = (c == '\n');
        if (c != '@' || !cc__match_kw_reg(src, n, i + 1, "comptime")) continue;
        {
            size_t body_l = cc__skip_ws_reg(src, n, i + 1 + strlen("comptime"));
            size_t body_r = 0;
            if (body_l >= n || src[body_l] != '{') continue;
            if (!cc__find_matching_reg(src, n, body_l, '{', '}', &body_r)) continue;
            for (size_t j = body_l + 1; j < body_r; ++j) {
                char type_name[256];
                size_t lpar = 0, rpar = 0, p = 0, obj_l = 0, obj_r = 0;
                if (!cc__match_kw_reg(src, body_r, j, "cc_type_register")) continue;
                lpar = cc__skip_ws_reg(src, body_r, j + strlen("cc_type_register"));
                if (lpar >= body_r || src[lpar] != '(') continue;
                if (!cc__find_matching_reg(src, body_r, lpar, '(', ')', &rpar)) continue;
                p = cc__skip_ws_reg(src, body_r, lpar + 1);
                if (!cc__parse_string_literal_reg(src, body_r, &p, type_name, sizeof(type_name))) {
                    fprintf(stderr, "%s: error: unsupported cc_type_register type key form\n",
                            input_path ? input_path : "<input>");
                    return -1;
                }
                p = cc__skip_ws_reg(src, body_r, p);
                if (p >= body_r || src[p] != ',') {
                    fprintf(stderr, "%s: error: malformed cc_type_register(...) for '%s'\n",
                            input_path ? input_path : "<input>", type_name);
                    return -1;
                }
                p = cc__skip_ws_reg(src, body_r, p + 1);
                if (p < body_r && src[p] == '(') {
                    size_t cast_r = 0;
                    if (!cc__find_matching_reg(src, body_r, p, '(', ')', &cast_r)) return -1;
                    p = cc__skip_ws_reg(src, body_r, cast_r + 1);
                }
                if (p >= body_r || src[p] != '{') {
                    fprintf(stderr, "%s: error: cc_type_register(\"%s\", ...) expects a hooks object literal\n",
                            input_path ? input_path : "<input>", type_name);
                    return -1;
                }
                obj_l = p;
                if (!cc__find_matching_reg(src, body_r, obj_l, '{', '}', &obj_r)) return -1;
                if (cc__parse_type_hooks_object(t,
                                                input_path,
                                                logical_file[0] ? logical_file : NULL,
                                                type_name,
                                                src,
                                                obj_l,
                                                obj_r,
                                                create_callback,
                                                create_user_ctx,
                                                ufcs_callback,
                                                ufcs_user_ctx) != 0) {
                    fprintf(stderr, "%s: error: failed to record type hooks for '%s'\n",
                            input_path ? input_path : "<input>", type_name);
                    return -1;
                }
                j = rpar;
            }
            i = body_r;
        }
    }
    return 0;
}

int cc_symbols_collect_type_registrations(CCSymbolTable* t,
                                          const char* input_path,
                                          const char* src,
                                          size_t n) {
    return cc_symbols_collect_type_registrations_ex(t, input_path, src, n, NULL, NULL, NULL, NULL);
}
