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

struct CCSymbolTable {
    CCConstEntry* entries;
    size_t count;
    size_t capacity;

    CCFuncAttrEntry* fn_attrs;
    size_t fn_count;
    size_t fn_capacity;
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
    free(t->entries);
    free(t->fn_attrs);
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
