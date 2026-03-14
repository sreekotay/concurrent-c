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
    char* pattern;
    char* prefix;
    const void* fn_ptr;
    void* owner;
    CCOwnedResourceFreeFn owner_free;
} CCUfcsEntry;

typedef struct {
    char* method;
    char* callee;
} CCTypeUfcsEntry;

typedef struct {
    char* type_name;
    char* create_calls[5];
    char* destroy_call;
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

    CCUfcsEntry* ufcs;
    size_t ufcs_count;
    size_t ufcs_capacity;

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

static int ensure_ufcs_capacity(CCSymbolTable* t, size_t needed) {
    if (t->ufcs_capacity >= needed) return 0;
    size_t new_cap = t->ufcs_capacity ? t->ufcs_capacity * 2 : 8;
    while (new_cap < needed) new_cap *= 2;
    CCUfcsEntry* nv = (CCUfcsEntry*)realloc(t->ufcs, new_cap * sizeof(CCUfcsEntry));
    if (!nv) return ENOMEM;
    t->ufcs = nv;
    t->ufcs_capacity = new_cap;
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
    for (size_t i = 0; i < t->ufcs_count; ++i) {
        free(t->ufcs[i].pattern);
        free(t->ufcs[i].prefix);
        if (t->ufcs[i].owner && t->ufcs[i].owner_free) {
            t->ufcs[i].owner_free(t->ufcs[i].owner);
        }
    }
    for (size_t i = 0; i < t->type_count; ++i) {
        free(t->types[i].type_name);
        for (size_t a = 0; a < sizeof(t->types[i].create_calls) / sizeof(t->types[i].create_calls[0]); ++a) {
            free(t->types[i].create_calls[a]);
        }
        free(t->types[i].destroy_call);
        for (size_t u = 0; u < t->types[i].ufcs_count; ++u) {
            free(t->types[i].ufcs[u].method);
            free(t->types[i].ufcs[u].callee);
        }
        free(t->types[i].ufcs);
    }
    free(t->entries);
    free(t->fn_attrs);
    free(t->ufcs);
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

int cc_symbols_add_ufcs_prefix(CCSymbolTable* t, const char* pattern, const char* prefix) {
    if (!t || !pattern || !prefix) return EINVAL;
    for (size_t i = 0; i < t->ufcs_count; ++i) {
        if (strcmp(t->ufcs[i].pattern, pattern) == 0) {
            char* copy = strdup(prefix);
            if (!copy) return ENOMEM;
            free(t->ufcs[i].prefix);
            t->ufcs[i].prefix = copy;
            t->ufcs[i].fn_ptr = NULL;
            return 0;
        }
    }
    int err = ensure_ufcs_capacity(t, t->ufcs_count + 1);
    if (err != 0) return err;
    char* pattern_copy = strdup(pattern);
    char* prefix_copy = strdup(prefix);
    if (!pattern_copy || !prefix_copy) {
        free(pattern_copy);
        free(prefix_copy);
        return ENOMEM;
    }
    t->ufcs[t->ufcs_count].pattern = pattern_copy;
    t->ufcs[t->ufcs_count].prefix = prefix_copy;
    t->ufcs[t->ufcs_count].fn_ptr = NULL;
    t->ufcs[t->ufcs_count].owner = NULL;
    t->ufcs[t->ufcs_count].owner_free = NULL;
    t->ufcs_count += 1;
    return 0;
}

int cc_symbols_lookup_ufcs_prefix(CCSymbolTable* t, const char* pattern, const char** out_prefix) {
    if (!t || !pattern || !out_prefix) return EINVAL;
    size_t best_len = 0;
    const char* best_prefix = NULL;
    for (size_t i = 0; i < t->ufcs_count; ++i) {
        if (t->ufcs[i].prefix && cc__ufcs_pattern_matches(t->ufcs[i].pattern, pattern)) {
            size_t plen = strlen(t->ufcs[i].pattern);
            size_t score = (plen > 0 && t->ufcs[i].pattern[plen - 1] == '*') ? plen - 1 : plen;
            if (!best_prefix || score > best_len) {
                best_prefix = t->ufcs[i].prefix;
                best_len = score;
            }
        }
    }
    if (best_prefix) {
        *out_prefix = best_prefix;
        return 0;
    }
    return ENOENT;
}

int cc_symbols_add_ufcs_callable(CCSymbolTable* t,
                                 const char* pattern,
                                 const void* fn_ptr,
                                 void* owner,
                                 CCOwnedResourceFreeFn owner_free) {
    if (!t || !pattern || !fn_ptr) return EINVAL;
    for (size_t i = 0; i < t->ufcs_count; ++i) {
        if (strcmp(t->ufcs[i].pattern, pattern) == 0) {
            if (t->ufcs[i].owner && t->ufcs[i].owner_free) {
                t->ufcs[i].owner_free(t->ufcs[i].owner);
            }
            t->ufcs[i].fn_ptr = fn_ptr;
            t->ufcs[i].owner = owner;
            t->ufcs[i].owner_free = owner_free;
            return 0;
        }
    }
    {
        int err = ensure_ufcs_capacity(t, t->ufcs_count + 1);
        char* pattern_copy;
        if (err != 0) return err;
        pattern_copy = strdup(pattern);
        if (!pattern_copy) return ENOMEM;
        t->ufcs[t->ufcs_count].pattern = pattern_copy;
        t->ufcs[t->ufcs_count].prefix = NULL;
        t->ufcs[t->ufcs_count].fn_ptr = fn_ptr;
        t->ufcs[t->ufcs_count].owner = owner;
        t->ufcs[t->ufcs_count].owner_free = owner_free;
        t->ufcs_count += 1;
    }
    return 0;
}

int cc_symbols_lookup_ufcs_callable(CCSymbolTable* t, const char* pattern, const void** out_fn_ptr) {
    if (!t || !pattern || !out_fn_ptr) return EINVAL;
    size_t best_len = 0;
    const void* best_fn = NULL;
    for (size_t i = 0; i < t->ufcs_count; ++i) {
        if (t->ufcs[i].fn_ptr && cc__ufcs_pattern_matches(t->ufcs[i].pattern, pattern)) {
            size_t plen = strlen(t->ufcs[i].pattern);
            size_t score = (plen > 0 && t->ufcs[i].pattern[plen - 1] == '*') ? plen - 1 : plen;
            if (!best_fn || score > best_len) {
                best_fn = t->ufcs[i].fn_ptr;
                best_len = score;
            }
        }
    }
    if (best_fn) {
        *out_fn_ptr = best_fn;
        return 0;
    }
    return ENOENT;
}

size_t cc_symbols_ufcs_count(CCSymbolTable* t) {
    return t ? t->ufcs_count : 0;
}

const char* cc_symbols_ufcs_pattern(CCSymbolTable* t, size_t idx) {
    if (!t || idx >= t->ufcs_count) return NULL;
    return t->ufcs[idx].pattern;
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
    CCTypeEntry* entry = NULL;
    if (!t || !type_name || !method || !out_callee) return EINVAL;
    entry = cc__find_type_entry(t, type_name);
    if (!entry) return ENOENT;
    for (size_t i = 0; i < entry->ufcs_count; ++i) {
        if (strcmp(entry->ufcs[i].method, method) == 0) {
            *out_callee = entry->ufcs[i].callee;
            return 0;
        }
    }
    return ENOENT;
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

static int cc__parse_type_hooks_object(CCSymbolTable* t,
                                       const char* input_path,
                                       const char* type_name,
                                       const char* src,
                                       size_t obj_l,
                                       size_t obj_r) {
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
            p += strlen("destroy");
            p = cc__skip_ws_reg(src, obj_r, p);
            if (p >= obj_r || src[p] != '=') return -1;
            p++;
            if (!cc__parse_helper_call_1(src, obj_r, &p, "cc_type_destroy_call", destroy_callee, sizeof(destroy_callee))) return -1;
            if (cc_symbols_set_type_destroy_call(t, type_name, destroy_callee) != 0) return -1;
            continue;
        }
        if (cc__match_kw_reg(src, obj_r, p, "ufcs")) {
            size_t depth = 0;
            int in_str = 0, in_chr = 0;
            p += strlen("ufcs");
            p = cc__skip_ws_reg(src, obj_r, p);
            if (p >= obj_r || src[p] != '=') return -1;
            p = cc__skip_ws_reg(src, obj_r, p + 1);
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
            continue;
        }
        fprintf(stderr, "%s: error: unsupported cc_type_register hook field for '%s'\n",
                input_path ? input_path : "<input>", type_name);
        return -1;
    }
    return 0;
}

int cc_symbols_collect_type_registrations(CCSymbolTable* t,
                                          const char* input_path,
                                          const char* src,
                                          size_t n) {
    int in_lc = 0, in_bc = 0, in_str = 0, in_chr = 0;
    if (!t || !src) return 0;
    for (size_t i = 0; i < n; ++i) {
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
                if (cc__parse_type_hooks_object(t, input_path, type_name, src, obj_l, obj_r) != 0) {
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
