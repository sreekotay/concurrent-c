#include "symbols.h"

#include "preprocess/type_registry.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util/text.h"

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
    char* cast_call;
    char* len_kind;
    char* len_name;
    char* access_kind;
    char* access_name;
    char* ufcs_dynamic_callee;
    char* ufcs_dynamic_wrap;
    int has_niche;
    unsigned niche_size;
    unsigned niche_align;
    unsigned niche_offset;
    unsigned niche_width;
    unsigned long long niche_sentinel;
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
        size_t prefix_len = plen - 1;
        /* `CCChanTx_*` must match bare `CCChanTx` as well as `CCChanTx_int`. */
        if (prefix_len > 0 && pattern[prefix_len - 1] == '_') {
            size_t base_len = prefix_len - 1;
            if (strncmp(type_name, pattern, base_len) == 0 &&
                (type_name[base_len] == '\0' || type_name[base_len] == '_')) {
                return 1;
            }
        }
        return strncmp(type_name, pattern, prefix_len) == 0;
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
        free(t->types[i].cast_call);
        free(t->types[i].len_kind);
        free(t->types[i].len_name);
        free(t->types[i].access_kind);
        free(t->types[i].access_name);
        free(t->types[i].ufcs_dynamic_callee);
        free(t->types[i].ufcs_dynamic_wrap);
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

int cc_symbols_set_type_cast_call(CCSymbolTable* t, const char* type_name,
                                  const char* callee) {
    CCTypeEntry* entry = NULL;
    char* copy = NULL;
    int err;
    if (!t || !type_name || !callee) return EINVAL;
    err = cc__ensure_type_entry(t, type_name, &entry);
    if (err != 0) return err;
    copy = strdup(callee);
    if (!copy) return ENOMEM;
    free(entry->cast_call);
    entry->cast_call = copy;
    return 0;
}

int cc_symbols_lookup_type_cast_call(CCSymbolTable* t, const char* type_name,
                                     const char** out_callee) {
    CCTypeEntry* best_entry = NULL;
    size_t best_len = 0;
    if (!t || !type_name || !out_callee) return EINVAL;
    *out_callee = NULL;
    for (size_t ti = 0; ti < t->type_count; ++ti) {
        CCTypeEntry* entry = &t->types[ti];
        size_t plen;
        size_t score;
        if (!entry->cast_call || !entry->cast_call[0]) continue;
        if (!cc__ufcs_pattern_matches(entry->type_name, type_name)) continue;
        plen = strlen(entry->type_name);
        score = (plen > 0 && entry->type_name[plen - 1] == '*') ? plen - 1 : plen;
        if (!best_entry || score > best_len) {
            best_entry = entry;
            best_len = score;
            *out_callee = entry->cast_call;
        }
    }
    return best_entry ? 0 : ENOENT;
}

int cc_symbols_set_type_len(CCSymbolTable* t, const char* type_name,
                            const char* kind, const char* name) {
    CCTypeEntry* entry = NULL;
    char* ck = NULL;
    char* cn = NULL;
    int err;
    if (!t || !type_name || !kind || !name) return EINVAL;
    err = cc__ensure_type_entry(t, type_name, &entry);
    if (err != 0) return err;
    ck = strdup(kind);
    cn = strdup(name);
    if (!ck || !cn) {
        free(ck);
        free(cn);
        return ENOMEM;
    }
    free(entry->len_kind);
    free(entry->len_name);
    entry->len_kind = ck;
    entry->len_name = cn;
    return 0;
}

int cc_symbols_lookup_type_len(CCSymbolTable* t, const char* type_name,
                               const char** out_kind, const char** out_name) {
    CCTypeEntry* best_entry = NULL;
    size_t best_len = 0;
    if (!t || !type_name || !out_kind || !out_name) return EINVAL;
    *out_kind = NULL;
    *out_name = NULL;
    for (size_t ti = 0; ti < t->type_count; ++ti) {
        CCTypeEntry* entry = &t->types[ti];
        size_t plen;
        size_t score;
        if (!entry->len_name || !entry->len_name[0]) continue;
        if (!cc__ufcs_pattern_matches(entry->type_name, type_name)) continue;
        plen = strlen(entry->type_name);
        score = (plen > 0 && entry->type_name[plen - 1] == '*') ? plen - 1 : plen;
        if (!best_entry || score > best_len) {
            best_entry = entry;
            best_len = score;
            *out_kind = entry->len_kind;
            *out_name = entry->len_name;
        }
    }
    return best_entry ? 0 : ENOENT;
}

int cc_symbols_set_type_access(CCSymbolTable* t, const char* type_name,
                               const char* kind, const char* name) {
    CCTypeEntry* entry = NULL;
    char* ck = NULL;
    char* cn = NULL;
    int err;
    if (!t || !type_name || !kind || !name) return EINVAL;
    err = cc__ensure_type_entry(t, type_name, &entry);
    if (err != 0) return err;
    ck = strdup(kind);
    cn = strdup(name);
    if (!ck || !cn) {
        free(ck);
        free(cn);
        return ENOMEM;
    }
    free(entry->access_kind);
    free(entry->access_name);
    entry->access_kind = ck;
    entry->access_name = cn;
    return 0;
}

int cc_symbols_lookup_type_access(CCSymbolTable* t, const char* type_name,
                                  const char** out_kind, const char** out_name) {
    CCTypeEntry* best_entry = NULL;
    size_t best_len = 0;
    if (!t || !type_name || !out_kind || !out_name) return EINVAL;
    *out_kind = NULL;
    *out_name = NULL;
    for (size_t ti = 0; ti < t->type_count; ++ti) {
        CCTypeEntry* entry = &t->types[ti];
        size_t plen;
        size_t score;
        if (!entry->access_name || !entry->access_name[0]) continue;
        if (!cc__ufcs_pattern_matches(entry->type_name, type_name)) continue;
        plen = strlen(entry->type_name);
        score = (plen > 0 && entry->type_name[plen - 1] == '*') ? plen - 1 : plen;
        if (!best_entry || score > best_len) {
            best_entry = entry;
            best_len = score;
            *out_kind = entry->access_kind;
            *out_name = entry->access_name;
        }
    }
    return best_entry ? 0 : ENOENT;
}

int cc_symbols_set_type_ufcs_dynamic(CCSymbolTable* t, const char* type_name,
                                     const char* callee, const char* wrap) {
    CCTypeEntry* entry = NULL;
    char* c = NULL;
    char* w = NULL;
    int err;
    if (!t || !type_name || !callee || !wrap) return EINVAL;
    err = cc__ensure_type_entry(t, type_name, &entry);
    if (err != 0) return err;
    c = strdup(callee);
    w = strdup(wrap);
    if (!c || !w) {
        free(c);
        free(w);
        return ENOMEM;
    }
    free(entry->ufcs_dynamic_callee);
    free(entry->ufcs_dynamic_wrap);
    entry->ufcs_dynamic_callee = c;
    entry->ufcs_dynamic_wrap = w;
    return 0;
}

int cc_symbols_lookup_type_ufcs_dynamic(CCSymbolTable* t, const char* type_name,
                                        const char** out_callee,
                                        const char** out_wrap) {
    CCTypeEntry* entry = NULL;
    if (!t || !type_name || !out_callee || !out_wrap) return EINVAL;
    entry = cc__find_type_entry(t, type_name);
    if (!entry || !entry->ufcs_dynamic_callee || !entry->ufcs_dynamic_wrap)
        return ENOENT;
    *out_callee = entry->ufcs_dynamic_callee;
    *out_wrap = entry->ufcs_dynamic_wrap;
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

int cc_symbols_set_type_niche(CCSymbolTable* t, const char* type_name,
                              unsigned size, unsigned align, unsigned offset,
                              unsigned width, unsigned long long sentinel) {
    CCTypeEntry* entry = NULL;
    int err;
    if (!t || !type_name) return EINVAL;
    if (width == 0 || width > 8 || offset + width > size) return EINVAL;
    err = cc__ensure_type_entry(t, type_name, &entry);
    if (err != 0) return err;
    entry->has_niche = 1;
    entry->niche_size = size;
    entry->niche_align = align;
    entry->niche_offset = offset;
    entry->niche_width = width;
    entry->niche_sentinel = sentinel;
    return 0;
}

int cc_symbols_lookup_type_niche(CCSymbolTable* t, const char* type_name,
                                 unsigned* out_size, unsigned* out_align,
                                 unsigned* out_offset, unsigned* out_width,
                                 unsigned long long* out_sentinel) {
    CCTypeEntry* entry = NULL;
    if (!t || !type_name) return EINVAL;
    entry = cc__find_type_entry(t, type_name);
    if (!entry || !entry->has_niche) return ENOENT;
    if (out_size) *out_size = entry->niche_size;
    if (out_align) *out_align = entry->niche_align;
    if (out_offset) *out_offset = entry->niche_offset;
    if (out_width) *out_width = entry->niche_width;
    if (out_sentinel) *out_sentinel = entry->niche_sentinel;
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

/* Single source of truth lives in util/text.h; this remains as a local name
 * so the many call sites below read unchanged. */
static int cc__match_kw_reg(const char* src, size_t n, size_t pos, const char* kw) {
    return cc_match_ident_kw(src, n, pos, kw);
}

/* Single source of truth lives in util/text.h (comment/string-aware ws skip). */
static size_t cc__skip_ws_reg(const char* src, size_t n, size_t i) {
    return cc_skip_ws_and_comments(src, n, i);
}

/* Single source of truth lives in util/text.h.  symbols.c only ever matches
 * `(`/`{` pairs; the close_ch arg is implied by the opener and kept for the
 * existing call-site signatures. */
static int cc__find_matching_reg(const char* src, size_t n, size_t open_idx, char open_ch, char close_ch, size_t* out_close) {
    (void)close_ch;
    if (open_ch == '{') return cc_find_matching_brace(src, n, open_idx, out_close);
    if (open_ch == '(') return cc_find_matching_paren(src, n, open_idx, out_close);
    if (open_ch == '[') return cc_find_matching_bracket(src, n, open_idx, out_close);
    return 0;
}

/* Single source of truth lives in util/text.h (cc_parse_c_string_literal),
 * which additionally decodes \r \\ \" \0 and truncates overlong content
 * rather than failing — neither matters for the short type-name / callee
 * literals parsed here, all of which are escape-free. */
static int cc__parse_string_literal_reg(const char* src, size_t n, size_t* io_pos, char* out, size_t out_sz) {
    return cc_parse_c_string_literal(src, n, io_pos, out, out_sz);
}

/* Create-callee arg: `"foo"` or `CC_TYPE_CREATE_DECL("foo")` → `decl:foo`. */
static int cc__parse_create_callee_reg(const char* src, size_t n, size_t* io_pos,
                                      char* out, size_t out_sz) {
    size_t p;
    size_t lpar = 0, rpar = 0;
    char inner[240];
    if (!src || !io_pos || !out || !out_sz) return 0;
    p = cc__skip_ws_reg(src, n, *io_pos);
    if (cc__parse_string_literal_reg(src, n, &p, out, out_sz)) {
        *io_pos = p;
        return 1;
    }
    if (!cc__match_kw_reg(src, n, p, "CC_TYPE_CREATE_DECL")) return 0;
    p += strlen("CC_TYPE_CREATE_DECL");
    p = cc__skip_ws_reg(src, n, p);
    if (p >= n || src[p] != '(') return 0;
    lpar = p;
    if (!cc__find_matching_reg(src, n, lpar, '(', ')', &rpar)) return 0;
    p = cc__skip_ws_reg(src, rpar, lpar + 1);
    if (!cc__parse_string_literal_reg(src, rpar, &p, inner, sizeof(inner))) return 0;
    p = cc__skip_ws_reg(src, rpar, p);
    if (p != rpar) return 0;
    if (strncmp(inner, "decl:", 5) == 0)
        snprintf(out, out_sz, "%s", inner);
    else
        snprintf(out, out_sz, "decl:%s", inner);
    *io_pos = rpar + 1;
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

static int cc__parse_helper_call_2_create(const char* src,
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
    if (!cc__parse_create_callee_reg(src, rpar, &p, out_a, out_a_sz)) return 0;
    p = cc__skip_ws_reg(src, rpar, p);
    if (p >= rpar || src[p] != ',') return 0;
    p = cc__skip_ws_reg(src, rpar, p + 1);
    if (!cc__parse_create_callee_reg(src, rpar, &p, out_b, out_b_sz)) return 0;
    p = cc__skip_ws_reg(src, rpar, p);
    if (p != rpar) return 0;
    *io_pos = rpar + 1;
    return 1;
}

static int cc__parse_ident_call_1_reg(const char* src,
                                      size_t n,
                                      size_t* io_pos,
                                      const char* callee,
                                      char* out_ident,
                                      size_t out_ident_sz) {
    size_t p = io_pos ? *io_pos : 0;
    size_t lpar = 0;
    size_t rpar = 0;
    size_t ident_start;
    size_t ident_end;
    if (!src || !io_pos || !callee || !out_ident || out_ident_sz == 0) return 0;
    out_ident[0] = '\0';
    if (!cc__match_kw_reg(src, n, p, callee)) return 0;
    p += strlen(callee);
    p = cc__skip_ws_reg(src, n, p);
    if (p >= n || src[p] != '(') return 0;
    lpar = p;
    if (!cc__find_matching_reg(src, n, lpar, '(', ')', &rpar)) return 0;
    p = cc__skip_ws_reg(src, rpar, lpar + 1);
    ident_start = p;
    if (p >= rpar || !(src[p] == '_' || isalpha((unsigned char)src[p]))) return 0;
    p++;
    while (p < rpar && (src[p] == '_' || isalnum((unsigned char)src[p]))) p++;
    ident_end = p;
    p = cc__skip_ws_reg(src, rpar, p);
    if (p != rpar) return 0;
    if (ident_end - ident_start >= out_ident_sz) return 0;
    memcpy(out_ident, src + ident_start, ident_end - ident_start);
    out_ident[ident_end - ident_start] = '\0';
    *io_pos = rpar + 1;
    return 1;
}

/* Parse one integer literal (decimal or 0x-hex, optional u/l suffixes) from
 * src[*io_pos..n), skipping leading ws.  Returns 1 and advances on success. */
static int cc__parse_uint_reg(const char* src, size_t n, size_t* io_pos,
                              unsigned long long* out) {
    size_t p = cc__skip_ws_reg(src, n, *io_pos);
    unsigned long long v = 0;
    int any = 0;
    if (p >= n) return 0;
    if (p + 1 < n && src[p] == '0' && (src[p + 1] == 'x' || src[p + 1] == 'X')) {
        p += 2;
        while (p < n && isxdigit((unsigned char)src[p])) {
            char c = src[p];
            unsigned d = (c <= '9') ? (unsigned)(c - '0')
                        : (unsigned)((c | 0x20) - 'a' + 10);
            v = v * 16 + d;
            p++;
            any = 1;
        }
    } else {
        while (p < n && isdigit((unsigned char)src[p])) {
            v = v * 10 + (unsigned long long)(src[p] - '0');
            p++;
            any = 1;
        }
    }
    if (!any) return 0;
    while (p < n && (src[p] == 'u' || src[p] == 'U' || src[p] == 'l' || src[p] == 'L')) p++;
    *io_pos = p;
    *out = v;
    return 1;
}

/* Parse `cc_type_niche(size, align, offset, width, sentinel)` at *io_pos. */
static int cc__parse_niche_call(const char* src, size_t n, size_t* io_pos,
                                unsigned long long out[5]) {
    size_t p = cc__skip_ws_reg(src, n, *io_pos);
    size_t lpar = 0, rpar = 0;
    if (!cc__match_kw_reg(src, n, p, "cc_type_niche")) return 0;
    p += strlen("cc_type_niche");
    p = cc__skip_ws_reg(src, n, p);
    if (p >= n || src[p] != '(') return 0;
    lpar = p;
    if (!cc__find_matching_reg(src, n, lpar, '(', ')', &rpar)) return 0;
    p = lpar + 1;
    for (int a = 0; a < 5; a++) {
        if (!cc__parse_uint_reg(src, rpar, &p, &out[a])) return 0;
        p = cc__skip_ws_reg(src, rpar, p);
        if (a < 4) {
            if (p >= rpar || src[p] != ',') return 0;
            p++;
        }
    }
    if (p != rpar) return 0;
    *io_pos = rpar + 1;
    return 1;
}

static int cc__record_map_decl_ufcs(CCSymbolTable* t, const char* map_name) {
    static const char* methods[] = {
        "insert", "put", "get", "get_ptr", "remove", "del", "clear", "destroy",
        "len", "cap", "live_bytes", "at_ptr", "key_ptr"
    };
    char pattern[256];
    char callee[256];
    if (!t || !map_name || !map_name[0]) return EINVAL;
    /* Exact name + `Name*` so both value and pointer receivers hit the table
     * (NestedUfcsMap / ArrayMap_int_int / ArrayMap_int_int*). */
    if (snprintf(pattern, sizeof(pattern), "%s", map_name) >= (int)sizeof(pattern)) return ENAMETOOLONG;
    for (size_t i = 0; i < sizeof(methods) / sizeof(methods[0]); ++i) {
        if (snprintf(callee, sizeof(callee), "%s_%s", map_name, methods[i]) >= (int)sizeof(callee)) {
            return ENAMETOOLONG;
        }
        if (cc_symbols_add_type_ufcs_value(t, pattern, methods[i], callee) != 0) return ENOMEM;
    }
    if (snprintf(pattern, sizeof(pattern), "%s*", map_name) >= (int)sizeof(pattern)) return ENAMETOOLONG;
    for (size_t i = 0; i < sizeof(methods) / sizeof(methods[0]); ++i) {
        if (snprintf(callee, sizeof(callee), "%s_%s", map_name, methods[i]) >= (int)sizeof(callee)) {
            return ENAMETOOLONG;
        }
        if (cc_symbols_add_type_ufcs_value(t, pattern, methods[i], callee) != 0) return ENOMEM;
    }
    return 0;
}

int cc_symbols_register_map_ufcs(CCSymbolTable* t, const char* map_name) {
    return cc__record_map_decl_ufcs(t, map_name);
}

int cc_symbols_register_map_ufcs_alias(CCSymbolTable* t,
                                       const char* alias_name,
                                       const char* mangled_name) {
    static const char* methods[] = {
        "insert", "put", "get", "get_ptr", "remove", "del", "clear", "destroy",
        "len", "cap", "live_bytes", "at_ptr", "key_ptr"
    };
    char pattern[256];
    char callee[256];
    if (!t || !alias_name || !alias_name[0] || !mangled_name || !mangled_name[0]) return EINVAL;
    if (snprintf(pattern, sizeof(pattern), "%s", alias_name) >= (int)sizeof(pattern)) return ENAMETOOLONG;
    for (size_t i = 0; i < sizeof(methods) / sizeof(methods[0]); ++i) {
        if (snprintf(callee, sizeof(callee), "%s_%s", mangled_name, methods[i]) >= (int)sizeof(callee))
            return ENAMETOOLONG;
        if (cc_symbols_add_type_ufcs_value(t, pattern, methods[i], callee) != 0) return ENOMEM;
    }
    if (snprintf(pattern, sizeof(pattern), "%s*", alias_name) >= (int)sizeof(pattern)) return ENAMETOOLONG;
    for (size_t i = 0; i < sizeof(methods) / sizeof(methods[0]); ++i) {
        if (snprintf(callee, sizeof(callee), "%s_%s", mangled_name, methods[i]) >= (int)sizeof(callee))
            return ENAMETOOLONG;
        if (cc_symbols_add_type_ufcs_value(t, pattern, methods[i], callee) != 0) return ENOMEM;
    }
    return 0;
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
            if (cc__parse_helper_call_2_create(src, expr_e, &parse_pos, "cc_type_create_overloads",
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
            if (expr_s < expr_e && cc_is_ident_start(src[expr_s])) {
                size_t id_e = expr_s;
                while (id_e < expr_e && cc_is_ident_char(src[id_e])) id_e++;
                if (id_e == expr_e && (id_e - expr_s) < sizeof(create_callee)) {
                    memcpy(create_callee, src + expr_s, id_e - expr_s);
                    create_callee[id_e - expr_s] = 0;
                    if (cc_symbols_add_type_create_call(t, type_name, 1, create_callee) != 0) return -1;
                    p = scan;
                    continue;
                }
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
            if (cc__parse_helper_call_1(src, obj_r, &p, "cc_type_destroy_call", destroy_callee, sizeof(destroy_callee))) {
                if (cc_symbols_set_type_destroy_call(t, type_name, destroy_callee) != 0) return -1;
                continue;
            }
            {
                size_t id = cc__skip_ws_reg(src, obj_r, p);
                size_t id_e = id;
                size_t after;
                if (id < obj_r && cc_is_ident_start(src[id])) {
                    while (id_e < obj_r && cc_is_ident_char(src[id_e])) id_e++;
                    after = cc__skip_ws_reg(src, obj_r, id_e);
                    if ((after >= obj_r || src[after] == ',' || src[after] == '}') &&
                        (id_e - id) > 0 && (id_e - id) < sizeof(destroy_callee)) {
                        memcpy(destroy_callee, src + id, id_e - id);
                        destroy_callee[id_e - id] = 0;
                        if (cc_symbols_set_type_destroy_call(t, type_name, destroy_callee) != 0) return -1;
                        p = after;
                        continue;
                    }
                }
            }
            return -1;
        }
        if (cc__match_kw_reg(src, obj_r, p, "niche")) {
            unsigned long long args[5];
            p += strlen("niche");
            p = cc__skip_ws_reg(src, obj_r, p);
            if (p >= obj_r || src[p] != '=') return -1;
            p = cc__skip_ws_reg(src, obj_r, p + 1);
            if (!cc__parse_niche_call(src, obj_r, &p, args)) {
                fprintf(stderr, "%s: error: malformed .niche = cc_type_niche(...) for '%s'\n",
                        input_path ? input_path : "<input>", type_name);
                return -1;
            }
            if (cc_symbols_set_type_niche(t, type_name, (unsigned)args[0], (unsigned)args[1],
                                          (unsigned)args[2], (unsigned)args[3], args[4]) != 0) {
                fprintf(stderr, "%s: error: invalid niche descriptor for '%s'\n",
                        input_path ? input_path : "<input>", type_name);
                return -1;
            }
            continue;
        }
        /* `.ufcs_sink` is the field. `.ufcs_dynamic` / `.ufcs_dynamic2`
         * are accepted spellings of the same dest-aware sink. */
        {
            size_t sink_kw = 0;
            if (cc__match_kw_reg(src, obj_r, p, "ufcs_sink"))
                sink_kw = strlen("ufcs_sink");
            else if (cc__match_kw_reg(src, obj_r, p, "ufcs_dynamic2"))
                sink_kw = strlen("ufcs_dynamic2");
            else if (cc__match_kw_reg(src, obj_r, p, "ufcs_dynamic"))
                sink_kw = strlen("ufcs_dynamic");
            if (sink_kw) {
                char dyn_callee[256];
                char dyn_wrap[256];
                p += sink_kw;
                p = cc__skip_ws_reg(src, obj_r, p);
                if (p >= obj_r || src[p] != '=') return -1;
                p++;
                if (!cc__parse_helper_call_2(src, obj_r, &p, "cc_type_dynamic_call",
                                             dyn_callee, sizeof(dyn_callee),
                                             dyn_wrap, sizeof(dyn_wrap))) {
                    fprintf(stderr,
                            "%s: error: malformed .ufcs_sink = "
                            "cc_type_dynamic_call(\"callee\", \"argwrap\") for '%s'\n",
                            input_path ? input_path : "<input>", type_name);
                    return -1;
                }
                if (cc_symbols_set_type_ufcs_dynamic(t, type_name, dyn_callee,
                                                     dyn_wrap) != 0)
                    return -1;
                (void)cc_type_registry_set_dynamic_sink(
                    cc_type_registry_get_global(), type_name, dyn_callee,
                    dyn_wrap);
                continue;
            }
        }
        if (cc__match_kw_reg(src, obj_r, p, "cast")) {
            size_t expr_s = 0;
            size_t expr_e = 0;
            size_t depth = 0;
            int in_str = 0, in_chr = 0;
            char handler[256];
            size_t hn = 0;
            p += strlen("cast");
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
            if (expr_e <= expr_s) return -1;
            if (!cc_is_ident_start(src[expr_s])) {
                fprintf(stderr,
                        "%s: error: .cast handler for '%s' must be a function ident\n",
                        input_path ? input_path : "<input>", type_name);
                return -1;
            }
            hn = 0;
            while (expr_s + hn < expr_e && cc_is_ident_char(src[expr_s + hn]) &&
                   hn + 1 < sizeof(handler))
                hn++;
            if (expr_s + hn != expr_e) {
                fprintf(stderr,
                        "%s: error: .cast handler for '%s' must be a function ident\n",
                        input_path ? input_path : "<input>", type_name);
                return -1;
            }
            memcpy(handler, src + expr_s, hn);
            handler[hn] = 0;
            if (cc_symbols_set_type_cast_call(t, type_name, handler) != 0)
                return -1;
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
        if (cc__match_kw_reg(src, obj_r, p, "len")) {
            char name[256];
            p += strlen("len");
            p = cc__skip_ws_reg(src, obj_r, p);
            if (p >= obj_r || src[p] != '=') return -1;
            p++;
            if (cc__parse_helper_call_1(src, obj_r, &p, "cc_type_len_field",
                                       name, sizeof(name))) {
                if (cc_symbols_set_type_len(t, type_name, "field", name) != 0)
                    return -1;
                continue;
            }
            if (cc__parse_helper_call_1(src, obj_r, &p, "cc_type_len_call",
                                       name, sizeof(name))) {
                if (cc_symbols_set_type_len(t, type_name, "call", name) != 0)
                    return -1;
                continue;
            }
            fprintf(stderr,
                    "%s: error: malformed .len = cc_type_len_field/call(\"...\" ) "
                    "for '%s'\n",
                    input_path ? input_path : "<input>", type_name);
            return -1;
        }
        if (cc__match_kw_reg(src, obj_r, p, "access")) {
            char name[256];
            p += strlen("access");
            p = cc__skip_ws_reg(src, obj_r, p);
            if (p >= obj_r || src[p] != '=') return -1;
            p++;
            if (cc__parse_helper_call_1(src, obj_r, &p, "cc_type_access_load",
                                       name, sizeof(name))) {
                if (cc_symbols_set_type_access(t, type_name, "load", name) != 0)
                    return -1;
                continue;
            }
            if (cc__parse_helper_call_1(src, obj_r, &p, "cc_type_access_call",
                                       name, sizeof(name))) {
                if (cc_symbols_set_type_access(t, type_name, "call", name) != 0)
                    return -1;
                continue;
            }
            fprintf(stderr,
                    "%s: error: malformed .access = cc_type_access_load/call(\"...\" ) "
                    "for '%s'\n",
                    input_path ? input_path : "<input>", type_name);
            return -1;
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
        if (i + strlen("__cc_map_decl_ufcs__") < n &&
            memcmp(src + i, "__cc_map_decl_ufcs__", strlen("__cc_map_decl_ufcs__")) == 0 &&
            (i == 0 || !isalnum((unsigned char)src[i - 1])) && (i == 0 || src[i - 1] != '_')) {
            char map_name[256];
            size_t p = i + strlen("__cc_map_decl_ufcs__");
            size_t s = p;
            while (p < n && (src[p] == '_' || isalnum((unsigned char)src[p]))) p++;
            if (p > s && p - s < sizeof(map_name)) {
                int rc;
                memcpy(map_name, src + s, p - s);
                map_name[p - s] = '\0';
                rc = cc__record_map_decl_ufcs(t, map_name);
                if (rc != 0) {
                    fprintf(stderr, "%s: error: failed to record map UFCS declaration for '%s'\n",
                            input_path ? input_path : "<input>", map_name);
                    return -1;
                }
                i = p;
                continue;
            }
        }
        if (i + strlen("__cc_array_map_decl_ufcs__") < n &&
            memcmp(src + i, "__cc_array_map_decl_ufcs__", strlen("__cc_array_map_decl_ufcs__")) == 0 &&
            (i == 0 || !isalnum((unsigned char)src[i - 1])) && (i == 0 || src[i - 1] != '_')) {
            char map_name[256];
            size_t p = i + strlen("__cc_array_map_decl_ufcs__");
            size_t s = p;
            while (p < n && (src[p] == '_' || isalnum((unsigned char)src[p]))) p++;
            if (p > s && p - s < sizeof(map_name)) {
                int rc;
                memcpy(map_name, src + s, p - s);
                map_name[p - s] = '\0';
                rc = cc__record_map_decl_ufcs(t, map_name);
                if (rc != 0) {
                    fprintf(stderr, "%s: error: failed to record array-map UFCS declaration for '%s'\n",
                            input_path ? input_path : "<input>", map_name);
                    return -1;
                }
                i = p;
                continue;
            }
        }
        if (cc__match_kw_reg(src, n, i, "CC_MAP_DECL_UFCS")) {
            char map_name[256];
            size_t p = i;
            if (cc__parse_ident_call_1_reg(src, n, &p, "CC_MAP_DECL_UFCS", map_name, sizeof(map_name))) {
                int rc = cc__record_map_decl_ufcs(t, map_name);
                if (rc != 0) {
                    fprintf(stderr, "%s: error: failed to record map UFCS declaration for '%s'\n",
                            input_path ? input_path : "<input>", map_name);
                    return -1;
                }
                i = p;
                continue;
            }
        }
        if (cc__match_kw_reg(src, n, i, "CC_ARRAY_MAP_DECL_UFCS")) {
            char map_name[256];
            size_t p = i;
            if (cc__parse_ident_call_1_reg(src, n, &p, "CC_ARRAY_MAP_DECL_UFCS", map_name, sizeof(map_name))) {
                int rc = cc__record_map_decl_ufcs(t, map_name);
                if (rc != 0) {
                    fprintf(stderr, "%s: error: failed to record array-map UFCS declaration for '%s'\n",
                            input_path ? input_path : "<input>", map_name);
                    return -1;
                }
                i = p;
                continue;
            }
        }
        if (c != '@') continue;
        {
            size_t body_l = 0, body_r = 0;
            /* Shared recognizer (util/text.h): same "what is a @comptime block"
             * definition used by emit_plan.c's intrinsic enumerator. */
            if (!cc_match_comptime_block(src, n, i, &body_l, &body_r)) continue;
            for (size_t j = body_l + 1; j < body_r; ++j) {
                char type_name[256];
                size_t lpar = 0, rpar = 0, p = 0, obj_l = 0, obj_r = 0;
                const char* reg_kw = NULL;
                /* Accept both the legacy `cc_type_register` and the
                 * forward-looking `cc_type_define` spelling (B3).  Both take an
                 * identical ("name", CCTypeHooks{...}) shape, so the parse below
                 * is unchanged — only the matched keyword length differs. */
                if (cc__match_kw_reg(src, body_r, j, "cc_type_register")) {
                    reg_kw = "cc_type_register";
                } else if (cc__match_kw_reg(src, body_r, j, "cc_type_define")) {
                    reg_kw = "cc_type_define";
                }
                if (!reg_kw) continue;
                lpar = cc__skip_ws_reg(src, body_r, j + strlen(reg_kw));
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
