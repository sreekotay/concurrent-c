#include "symbols.h"

#include <errno.h>
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

static int cc__ufcs_pattern_matches(const char* pattern, const char* type_name) {
    size_t plen;
    if (!pattern || !type_name) return 0;
    plen = strlen(pattern);
    if (plen > 0 && pattern[plen - 1] == '*') {
        return strncmp(type_name, pattern, plen - 1) == 0;
    }
    return strcmp(pattern, type_name) == 0;
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
    free(t->entries);
    free(t->fn_attrs);
    free(t->ufcs);
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
