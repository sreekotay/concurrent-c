/*
 * Type Registry Implementation
 *
 * Simple hash-less linear search for now; sufficient for typical file sizes.
 * Can be upgraded to hash tables if performance becomes an issue.
 */
#include "type_registry.h"

#include <stdlib.h>
#include <string.h>

/* Variable -> type entry */
typedef struct {
    char* var_name;
    char* type_name;
} CCVarTypeEntry;

/* Type instantiation storage (stores CCTypeInstantiation with owned strings) */
typedef struct {
    CCContainerKind kind;
    char* mangled_name;
    char* type1;
    char* type2;
} CCTypeInstEntry;

struct CCTypeRegistry {
    /* Variable types */
    CCVarTypeEntry* vars;
    size_t var_count;
    size_t var_capacity;

    /* Vec instantiations */
    CCTypeInstEntry* vecs;
    size_t vec_count;
    size_t vec_capacity;

    /* Map instantiations */
    CCTypeInstEntry* maps;
    size_t map_count;
    size_t map_capacity;

    /* Optional instantiations */
    CCTypeInstEntry* optionals;
    size_t opt_count;
    size_t opt_capacity;
};

/* Thread-local global registry */
static _Thread_local CCTypeRegistry* g_type_registry = NULL;

CCTypeRegistry* cc_type_registry_get_global(void) {
    return g_type_registry;
}

void cc_type_registry_set_global(CCTypeRegistry* reg) {
    g_type_registry = reg;
}

CCTypeRegistry* cc_type_registry_new(void) {
    CCTypeRegistry* reg = (CCTypeRegistry*)calloc(1, sizeof(CCTypeRegistry));
    return reg;
}

static void free_var_entries(CCVarTypeEntry* entries, size_t count) {
    for (size_t i = 0; i < count; i++) {
        free(entries[i].var_name);
        free(entries[i].type_name);
    }
}

static void free_inst_entries(CCTypeInstEntry* entries, size_t count) {
    for (size_t i = 0; i < count; i++) {
        free(entries[i].mangled_name);
        free(entries[i].type1);
        free(entries[i].type2);
    }
}

void cc_type_registry_free(CCTypeRegistry* reg) {
    if (!reg) return;
    free_var_entries(reg->vars, reg->var_count);
    free(reg->vars);
    free_inst_entries(reg->vecs, reg->vec_count);
    free(reg->vecs);
    free_inst_entries(reg->maps, reg->map_count);
    free(reg->maps);
    free_inst_entries(reg->optionals, reg->opt_count);
    free(reg->optionals);
    free(reg);
}

void cc_type_registry_clear(CCTypeRegistry* reg) {
    if (!reg) return;
    free_var_entries(reg->vars, reg->var_count);
    reg->var_count = 0;
    free_inst_entries(reg->vecs, reg->vec_count);
    reg->vec_count = 0;
    free_inst_entries(reg->maps, reg->map_count);
    reg->map_count = 0;
    free_inst_entries(reg->optionals, reg->opt_count);
    reg->opt_count = 0;
}

/* Ensure capacity for var entries */
static int ensure_var_capacity(CCTypeRegistry* reg, size_t needed) {
    if (reg->var_capacity >= needed) return 0;
    size_t new_cap = reg->var_capacity ? reg->var_capacity * 2 : 16;
    while (new_cap < needed) new_cap *= 2;
    CCVarTypeEntry* nv = (CCVarTypeEntry*)realloc(reg->vars, new_cap * sizeof(CCVarTypeEntry));
    if (!nv) return -1;
    reg->vars = nv;
    reg->var_capacity = new_cap;
    return 0;
}

int cc_type_registry_add_var(CCTypeRegistry* reg, const char* var_name, const char* type_name) {
    if (!reg || !var_name || !type_name) return -1;

    /* Check for existing entry and update */
    for (size_t i = 0; i < reg->var_count; i++) {
        if (strcmp(reg->vars[i].var_name, var_name) == 0) {
            free(reg->vars[i].type_name);
            reg->vars[i].type_name = strdup(type_name);
            return reg->vars[i].type_name ? 0 : -1;
        }
    }

    if (ensure_var_capacity(reg, reg->var_count + 1) != 0) return -1;
    reg->vars[reg->var_count].var_name = strdup(var_name);
    reg->vars[reg->var_count].type_name = strdup(type_name);
    if (!reg->vars[reg->var_count].var_name || !reg->vars[reg->var_count].type_name) {
        free(reg->vars[reg->var_count].var_name);
        free(reg->vars[reg->var_count].type_name);
        return -1;
    }
    reg->var_count++;
    return 0;
}

const char* cc_type_registry_lookup_var(CCTypeRegistry* reg, const char* var_name) {
    if (!reg || !var_name) return NULL;
    for (size_t i = 0; i < reg->var_count; i++) {
        if (strcmp(reg->vars[i].var_name, var_name) == 0) {
            return reg->vars[i].type_name;
        }
    }
    return NULL;
}

/* Ensure capacity for vec entries */
static int ensure_vec_capacity(CCTypeRegistry* reg, size_t needed) {
    if (reg->vec_capacity >= needed) return 0;
    size_t new_cap = reg->vec_capacity ? reg->vec_capacity * 2 : 8;
    while (new_cap < needed) new_cap *= 2;
    CCTypeInstEntry* nv = (CCTypeInstEntry*)realloc(reg->vecs, new_cap * sizeof(CCTypeInstEntry));
    if (!nv) return -1;
    reg->vecs = nv;
    reg->vec_capacity = new_cap;
    return 0;
}

int cc_type_registry_add_vec(CCTypeRegistry* reg, const char* elem_type, const char* mangled_name) {
    if (!reg || !elem_type || !mangled_name) return -1;

    /* Check for duplicate */
    for (size_t i = 0; i < reg->vec_count; i++) {
        if (strcmp(reg->vecs[i].mangled_name, mangled_name) == 0) {
            return 0; /* Already registered */
        }
    }

    if (ensure_vec_capacity(reg, reg->vec_count + 1) != 0) return -1;
    reg->vecs[reg->vec_count].kind = CC_CONTAINER_VEC;
    reg->vecs[reg->vec_count].mangled_name = strdup(mangled_name);
    reg->vecs[reg->vec_count].type1 = strdup(elem_type);
    reg->vecs[reg->vec_count].type2 = NULL;
    if (!reg->vecs[reg->vec_count].mangled_name || !reg->vecs[reg->vec_count].type1) {
        free(reg->vecs[reg->vec_count].mangled_name);
        free(reg->vecs[reg->vec_count].type1);
        return -1;
    }
    reg->vec_count++;
    return 0;
}

/* Ensure capacity for map entries */
static int ensure_map_capacity(CCTypeRegistry* reg, size_t needed) {
    if (reg->map_capacity >= needed) return 0;
    size_t new_cap = reg->map_capacity ? reg->map_capacity * 2 : 8;
    while (new_cap < needed) new_cap *= 2;
    CCTypeInstEntry* nv = (CCTypeInstEntry*)realloc(reg->maps, new_cap * sizeof(CCTypeInstEntry));
    if (!nv) return -1;
    reg->maps = nv;
    reg->map_capacity = new_cap;
    return 0;
}

int cc_type_registry_add_map(CCTypeRegistry* reg, const char* key_type, const char* val_type, const char* mangled_name) {
    if (!reg || !key_type || !val_type || !mangled_name) return -1;

    /* Check for duplicate */
    for (size_t i = 0; i < reg->map_count; i++) {
        if (strcmp(reg->maps[i].mangled_name, mangled_name) == 0) {
            return 0; /* Already registered */
        }
    }

    if (ensure_map_capacity(reg, reg->map_count + 1) != 0) return -1;
    reg->maps[reg->map_count].kind = CC_CONTAINER_MAP;
    reg->maps[reg->map_count].mangled_name = strdup(mangled_name);
    reg->maps[reg->map_count].type1 = strdup(key_type);
    reg->maps[reg->map_count].type2 = strdup(val_type);
    if (!reg->maps[reg->map_count].mangled_name || !reg->maps[reg->map_count].type1 || !reg->maps[reg->map_count].type2) {
        free(reg->maps[reg->map_count].mangled_name);
        free(reg->maps[reg->map_count].type1);
        free(reg->maps[reg->map_count].type2);
        return -1;
    }
    reg->map_count++;
    return 0;
}

/* Ensure capacity for optional entries */
static int ensure_opt_capacity(CCTypeRegistry* reg, size_t needed) {
    if (reg->opt_capacity >= needed) return 0;
    size_t new_cap = reg->opt_capacity ? reg->opt_capacity * 2 : 8;
    while (new_cap < needed) new_cap *= 2;
    CCTypeInstEntry* nv = (CCTypeInstEntry*)realloc(reg->optionals, new_cap * sizeof(CCTypeInstEntry));
    if (!nv) return -1;
    reg->optionals = nv;
    reg->opt_capacity = new_cap;
    return 0;
}

int cc_type_registry_add_optional(CCTypeRegistry* reg, const char* elem_type, const char* mangled_name) {
    if (!reg || !elem_type || !mangled_name) return -1;

    /* Check for duplicate */
    for (size_t i = 0; i < reg->opt_count; i++) {
        if (strcmp(reg->optionals[i].mangled_name, mangled_name) == 0) {
            return 0; /* Already registered */
        }
    }

    if (ensure_opt_capacity(reg, reg->opt_count + 1) != 0) return -1;
    reg->optionals[reg->opt_count].kind = CC_CONTAINER_VEC; /* Not used for optionals */
    reg->optionals[reg->opt_count].mangled_name = strdup(mangled_name);
    reg->optionals[reg->opt_count].type1 = strdup(elem_type);
    reg->optionals[reg->opt_count].type2 = NULL;
    if (!reg->optionals[reg->opt_count].mangled_name || !reg->optionals[reg->opt_count].type1) {
        free(reg->optionals[reg->opt_count].mangled_name);
        free(reg->optionals[reg->opt_count].type1);
        return -1;
    }
    reg->opt_count++;
    return 0;
}

/* Iteration helpers - return views into internal storage */
size_t cc_type_registry_vec_count(CCTypeRegistry* reg) {
    return reg ? reg->vec_count : 0;
}

const CCTypeInstantiation* cc_type_registry_get_vec(CCTypeRegistry* reg, size_t idx) {
    if (!reg || idx >= reg->vec_count) return NULL;
    return (const CCTypeInstantiation*)&reg->vecs[idx];
}

size_t cc_type_registry_map_count(CCTypeRegistry* reg) {
    return reg ? reg->map_count : 0;
}

const CCTypeInstantiation* cc_type_registry_get_map(CCTypeRegistry* reg, size_t idx) {
    if (!reg || idx >= reg->map_count) return NULL;
    return (const CCTypeInstantiation*)&reg->maps[idx];
}

size_t cc_type_registry_optional_count(CCTypeRegistry* reg) {
    return reg ? reg->opt_count : 0;
}

const CCTypeInstantiation* cc_type_registry_get_optional(CCTypeRegistry* reg, size_t idx) {
    if (!reg || idx >= reg->opt_count) return NULL;
    return (const CCTypeInstantiation*)&reg->optionals[idx];
}
