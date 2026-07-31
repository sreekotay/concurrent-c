/*
 * Type Registry Implementation
 *
 * Simple hash-less linear search for now; sufficient for typical file sizes.
 * Can be upgraded to hash tables if performance becomes an issue.
 */
#include "type_registry.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "result_spec.h"
#include "util/text.h"

/* Variable -> type entry */
typedef struct {
    char* var_name;
    char* type_name;
} CCVarTypeEntry;

typedef struct {
    char* alias_name;
    char* type_name;
} CCTypeAliasEntry;

typedef struct {
    char* struct_name;
    char* field_name;
    char* field_type;
    int is_as; /* 1 when declared `Type name @as;` */
} CCFieldTypeEntry;

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

    /* Typedef aliases */
    CCTypeAliasEntry* aliases;
    size_t alias_count;
    size_t alias_capacity;

    /* Struct field types */
    CCFieldTypeEntry* fields;
    size_t field_count;
    size_t field_capacity;

    /* Vec instantiations */
    CCTypeInstEntry* vecs;
    size_t vec_count;
    size_t vec_capacity;

    /* Map instantiations */
    CCTypeInstEntry* maps;
    size_t map_count;
    size_t map_capacity;

    /* Channel typed-wrapper instantiations */
    CCTypeInstEntry* channels;
    size_t chan_count;
    size_t chan_capacity;

    /* Types with a registered dynamic UFCS sink (.ufcs_dynamic) */
    char** dyn_types;
    size_t dyn_count;
    size_t dyn_capacity;
};

/* Thread-local global registry */
static _Thread_local CCTypeRegistry* g_type_registry = NULL;

static void cc__normalize_local_decl_type(char* out, size_t out_sz, const char* type_name);
static void cc__strip_type_spelling(const char* in, char* out, size_t out_sz);

CCTypeRegistry* cc_type_registry_get_global(void) {
    return g_type_registry;
}

void cc_type_registry_set_global(CCTypeRegistry* reg) {
    g_type_registry = reg;
}

int cc_type_registry_scope_push(CCTypeRegistryScope* scope) {
    if (!scope) return 0;
    scope->saved = NULL;
    scope->temp  = NULL;
    CCTypeRegistry* temp = cc_type_registry_new();
    if (!temp) return 0;
    scope->saved = g_type_registry;
    scope->temp  = temp;
    g_type_registry = temp;
    return 1;
}

void cc_type_registry_scope_pop(CCTypeRegistryScope* scope) {
    if (!scope || !scope->temp) return;
    g_type_registry  = scope->saved;
    cc_type_registry_free(scope->temp);
    scope->saved = NULL;
    scope->temp  = NULL;
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

static void free_alias_entries(CCTypeAliasEntry* entries, size_t count) {
    for (size_t i = 0; i < count; i++) {
        free(entries[i].alias_name);
        free(entries[i].type_name);
    }
}

static void free_field_entries(CCFieldTypeEntry* entries, size_t count) {
    for (size_t i = 0; i < count; i++) {
        free(entries[i].struct_name);
        free(entries[i].field_name);
        free(entries[i].field_type);
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
    free_alias_entries(reg->aliases, reg->alias_count);
    free(reg->aliases);
    free_field_entries(reg->fields, reg->field_count);
    free(reg->fields);
    free_inst_entries(reg->vecs, reg->vec_count);
    free(reg->vecs);
    free_inst_entries(reg->maps, reg->map_count);
    free(reg->maps);
    free_inst_entries(reg->channels, reg->chan_count);
    free(reg->channels);
    for (size_t i = 0; i < reg->dyn_count; ++i) free(reg->dyn_types[i]);
    free(reg->dyn_types);
    free(reg);
}

/* Session-global (thread-local) sink-type list: registrations are parsed
 * from header text, but later pipeline stages rebuild fresh registry
 * instances from spliced buffers whose @comptime blocks are already
 * consumed — the fact "type T has a dynamic sink" must survive those
 * rebuilds. */
typedef struct {
    char* type_name;
    char* callee;
    char* wrap;
    int dest_aware; /* .ufcs_dynamic2: sink callee composes with dest type */
} CCDynSinkEntry;
static _Thread_local CCDynSinkEntry* g_dyn_sinks = NULL;
static _Thread_local size_t g_dyn_count = 0;
static _Thread_local size_t g_dyn_capacity = 0;

int cc_type_registry_set_dynamic_sink(CCTypeRegistry* reg, const char* type_name,
                                      const char* callee, const char* wrap) {
    char stripped[256];
    (void)reg;
    if (!type_name || !type_name[0] || !callee || !wrap) return -1;
    cc__strip_type_spelling(type_name, stripped, sizeof(stripped));
    if (!stripped[0]) return -1;
    for (size_t i = 0; i < g_dyn_count; ++i) {
        if (strcmp(g_dyn_sinks[i].type_name, stripped) == 0) return 0;
    }
    if (g_dyn_count == g_dyn_capacity) {
        size_t ncap = g_dyn_capacity ? g_dyn_capacity * 2 : 8;
        CCDynSinkEntry* nt = (CCDynSinkEntry*)realloc(
            g_dyn_sinks, ncap * sizeof(CCDynSinkEntry));
        if (!nt) return -1;
        g_dyn_sinks = nt;
        g_dyn_capacity = ncap;
    }
    g_dyn_sinks[g_dyn_count].type_name = strdup(stripped);
    g_dyn_sinks[g_dyn_count].callee = strdup(callee);
    g_dyn_sinks[g_dyn_count].wrap = strdup(wrap);
    g_dyn_sinks[g_dyn_count].dest_aware = 0;
    if (!g_dyn_sinks[g_dyn_count].type_name || !g_dyn_sinks[g_dyn_count].callee ||
        !g_dyn_sinks[g_dyn_count].wrap)
        return -1;
    g_dyn_count++;
    return 0;
}

int cc_type_registry_get_dynamic_sink(CCTypeRegistry* reg, const char* type_name,
                                      const char** out_callee,
                                      const char** out_wrap) {
    char stripped[256];
    (void)reg;
    if (!type_name || !type_name[0]) return -1;
    cc__strip_type_spelling(type_name, stripped, sizeof(stripped));
    for (size_t i = 0; i < g_dyn_count; ++i) {
        if (strcmp(g_dyn_sinks[i].type_name, stripped) == 0) {
            if (out_callee) *out_callee = g_dyn_sinks[i].callee;
            if (out_wrap) *out_wrap = g_dyn_sinks[i].wrap;
            return 0;
        }
    }
    return -1;
}

int cc_type_registry_has_dynamic_sink(CCTypeRegistry* reg, const char* type_name) {
    return cc_type_registry_get_dynamic_sink(reg, type_name, NULL, NULL) == 0;
}

int cc_type_registry_set_dynamic_sink2(CCTypeRegistry* reg, const char* type_name,
                                       const char* callee, const char* wrap) {
    char stripped[256];
    int rc = cc_type_registry_set_dynamic_sink(reg, type_name, callee, wrap);
    if (rc != 0) return rc;
    cc__strip_type_spelling(type_name, stripped, sizeof(stripped));
    for (size_t i = 0; i < g_dyn_count; ++i) {
        if (strcmp(g_dyn_sinks[i].type_name, stripped) == 0) {
            g_dyn_sinks[i].dest_aware = 1;
            return 0;
        }
    }
    return 0;
}

int cc_type_registry_dynamic_sink_dest_aware(CCTypeRegistry* reg,
                                             const char* type_name) {
    char stripped[256];
    (void)reg;
    if (!type_name || !type_name[0]) return 0;
    cc__strip_type_spelling(type_name, stripped, sizeof(stripped));
    for (size_t i = 0; i < g_dyn_count; ++i) {
        if (strcmp(g_dyn_sinks[i].type_name, stripped) == 0)
            return g_dyn_sinks[i].dest_aware;
    }
    return 0;
}

/* Typed slice instances: `T[:]` with a non-char element type lowers to
 * `CCSlice_<mangled T>`, an open generic family declared by
 * CC_DECL_SLICE_SPEC (cc_slice.cch). Session-global registration
 * (survives per-stage registry rebuilds, like the dyn-sink list): the
 * rewriter records every instance it names so downstream passes can
 * recover the member prefix (NAME##_, same convention as Vec/Map) and
 * element spelling. */
typedef struct {
    char* name;   /* CCSlice_double */
    char* prefix; /* CCSlice_double — member fns are prefix##_<member> */
    char* elem;   /* double */
} CCSliceSpecEntry;
static _Thread_local CCSliceSpecEntry* g_slice_specs = NULL;
static _Thread_local size_t g_slice_spec_count = 0;
static _Thread_local size_t g_slice_spec_capacity = 0;

int cc_slice_spec_register(const char* name, const char* prefix, const char* elem) {
    if (!name || !name[0] || !prefix || !prefix[0] || !elem || !elem[0]) return -1;
    for (size_t i = 0; i < g_slice_spec_count; ++i) {
        if (strcmp(g_slice_specs[i].name, name) == 0) return 0;
    }
    if (g_slice_spec_count == g_slice_spec_capacity) {
        size_t ncap = g_slice_spec_capacity ? g_slice_spec_capacity * 2 : 16;
        CCSliceSpecEntry* nt = (CCSliceSpecEntry*)realloc(
            g_slice_specs, ncap * sizeof(CCSliceSpecEntry));
        if (!nt) return -1;
        g_slice_specs = nt;
        g_slice_spec_capacity = ncap;
    }
    g_slice_specs[g_slice_spec_count].name = strdup(name);
    g_slice_specs[g_slice_spec_count].prefix = strdup(prefix);
    g_slice_specs[g_slice_spec_count].elem = strdup(elem);
    if (!g_slice_specs[g_slice_spec_count].name ||
        !g_slice_specs[g_slice_spec_count].prefix ||
        !g_slice_specs[g_slice_spec_count].elem)
        return -1;
    g_slice_spec_count++;
    return 0;
}

int cc_slice_spec_lookup(const char* type_name,
                         const char** out_prefix, const char** out_elem) {
    char stripped[256];
    if (!type_name || !type_name[0]) return -1;
    cc__strip_type_spelling(type_name, stripped, sizeof(stripped));
    for (size_t i = 0; i < g_slice_spec_count; ++i) {
        if (strcmp(g_slice_specs[i].name, stripped) == 0) {
            if (out_prefix) *out_prefix = g_slice_specs[i].prefix;
            if (out_elem) *out_elem = g_slice_specs[i].elem;
            return 0;
        }
    }
    return -1;
}

size_t cc_slice_spec_count(void) { return g_slice_spec_count; }

int cc_slice_spec_get(size_t i, const char** out_name, const char** out_elem) {
    if (i >= g_slice_spec_count) return -1;
    if (out_name) *out_name = g_slice_specs[i].name;
    if (out_elem) *out_elem = g_slice_specs[i].elem;
    return 0;
}

int cc_slice_spec_elem_is_prebaked(const char* elem) {
    /* The scalar pre-instances cc_slice.cch always declares (mangled
     * spellings). */
    static const char* const builtins[] = {
        "short", "int", "long", "long_long",
        "int16_t", "int32_t", "int64_t", "float", "double", NULL,
    };
    char stripped[256];
    char mangled[256];
    if (!elem || !elem[0]) return 0;
    cc__strip_type_spelling(elem, stripped, sizeof(stripped));
    cc_result_spec_mangle_type(stripped, strlen(stripped), mangled, sizeof(mangled));
    for (size_t i = 0; builtins[i]; ++i) {
        if (strcmp(mangled, builtins[i]) == 0) return 1;
    }
    return 0;
}

int cc_slice_spec_instance_for_elem(const char* elem,
                                    char* name_out, size_t name_sz) {
    /* Prebaked scalars always exist; anything else must have been
     * registered by the rewriter, which happens for every instance the
     * source names — the compiler splices the declaration. */
    char stripped[256];
    char mangled[256];
    if (!elem || !elem[0] || !name_out || name_sz == 0) return -1;
    cc__strip_type_spelling(elem, stripped, sizeof(stripped));
    cc_result_spec_mangle_type(stripped, strlen(stripped), mangled, sizeof(mangled));
    if (!mangled[0]) return -1;
    if ((size_t)snprintf(name_out, name_sz, "CCSlice_%s", mangled) >= name_sz)
        return -1;
    if (!cc_slice_spec_elem_is_prebaked(elem) &&
        cc_slice_spec_lookup(name_out, NULL, NULL) != 0)
        return -1;
    return 0;
}

void cc_type_registry_clear(CCTypeRegistry* reg) {
    if (!reg) return;
    free_var_entries(reg->vars, reg->var_count);
    reg->var_count = 0;
    free_alias_entries(reg->aliases, reg->alias_count);
    reg->alias_count = 0;
    free_field_entries(reg->fields, reg->field_count);
    reg->field_count = 0;
    free_inst_entries(reg->vecs, reg->vec_count);
    reg->vec_count = 0;
    free_inst_entries(reg->maps, reg->map_count);
    reg->map_count = 0;
    free_inst_entries(reg->channels, reg->chan_count);
    reg->chan_count = 0;
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

static int ensure_field_capacity(CCTypeRegistry* reg, size_t needed) {
    if (reg->field_capacity >= needed) return 0;
    size_t new_cap = reg->field_capacity ? reg->field_capacity * 2 : 32;
    while (new_cap < needed) new_cap *= 2;
    CCFieldTypeEntry* nv = (CCFieldTypeEntry*)realloc(reg->fields, new_cap * sizeof(CCFieldTypeEntry));
    if (!nv) return -1;
    reg->fields = nv;
    reg->field_capacity = new_cap;
    return 0;
}

static int ensure_alias_capacity(CCTypeRegistry* reg, size_t needed) {
    if (reg->alias_capacity >= needed) return 0;
    size_t new_cap = reg->alias_capacity ? reg->alias_capacity * 2 : 16;
    while (new_cap < needed) new_cap *= 2;
    CCTypeAliasEntry* nv = (CCTypeAliasEntry*)realloc(reg->aliases, new_cap * sizeof(CCTypeAliasEntry));
    if (!nv) return -1;
    reg->aliases = nv;
    reg->alias_capacity = new_cap;
    return 0;
}

static int cc__is_parser_placeholder_type(const char* type_name) {
    return type_name &&
           (strcmp(type_name, "__CCVecGeneric") == 0 ||
            strcmp(type_name, "__CCVecGeneric*") == 0 ||
            strcmp(type_name, "__CCMapGeneric") == 0 ||
            strcmp(type_name, "__CCMapGeneric*") == 0 ||
            strcmp(type_name, "__CCResultGeneric") == 0 ||
            strcmp(type_name, "__CCResultGeneric*") == 0);
}

int cc_type_registry_add_var(CCTypeRegistry* reg, const char* var_name, const char* type_name) {
    if (!reg || !var_name || !type_name) return -1;

    /* Check for existing entry and update */
    for (size_t i = 0; i < reg->var_count; i++) {
        if (strcmp(reg->vars[i].var_name, var_name) == 0) {
            if (((strncmp(reg->vars[i].type_name, "CCChanTx_", 9) == 0 &&
                  strcmp(type_name, "CCChanTx") == 0) ||
                 (strncmp(reg->vars[i].type_name, "CCChanRx_", 9) == 0 &&
                  strcmp(type_name, "CCChanRx") == 0))) {
                return 0;
            }
            /* This registry is intentionally scope-less, so "last writer wins"
               is unsafe once lowering passes introduce alternate concrete views
               (e.g. CCString later appearing as CCSlice). Allow upgrading
               parser placeholders to concrete types, but otherwise keep the
               first concrete declaration we saw for a name. */
            if (!cc__is_parser_placeholder_type(reg->vars[i].type_name) &&
                strcmp(reg->vars[i].type_name, type_name) != 0) {
                return 0;
            }
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

int cc_type_registry_add_alias(CCTypeRegistry* reg, const char* alias_name, const char* type_name) {
    if (!reg || !alias_name || !type_name) return -1;
    for (size_t i = 0; i < reg->alias_count; i++) {
        if (strcmp(reg->aliases[i].alias_name, alias_name) == 0) {
            if (strcmp(reg->aliases[i].type_name, type_name) == 0) return 0;
            free(reg->aliases[i].type_name);
            reg->aliases[i].type_name = strdup(type_name);
            return reg->aliases[i].type_name ? 0 : -1;
        }
    }
    if (ensure_alias_capacity(reg, reg->alias_count + 1) != 0) return -1;
    reg->aliases[reg->alias_count].alias_name = strdup(alias_name);
    reg->aliases[reg->alias_count].type_name = strdup(type_name);
    if (!reg->aliases[reg->alias_count].alias_name || !reg->aliases[reg->alias_count].type_name) {
        free(reg->aliases[reg->alias_count].alias_name);
        free(reg->aliases[reg->alias_count].type_name);
        return -1;
    }
    reg->alias_count++;
    return 0;
}

const char* cc_type_registry_lookup_alias(CCTypeRegistry* reg, const char* alias_name) {
    if (!reg || !alias_name) return NULL;
    for (size_t i = 0; i < reg->alias_count; i++) {
        if (strcmp(reg->aliases[i].alias_name, alias_name) == 0) {
            return reg->aliases[i].type_name;
        }
    }
    return NULL;
}

size_t cc_type_registry_alias_count(CCTypeRegistry* reg) {
    return reg ? reg->alias_count : 0;
}

const char* cc_type_registry_alias_name_at(CCTypeRegistry* reg, size_t idx) {
    if (!reg || idx >= reg->alias_count) return NULL;
    return reg->aliases[idx].alias_name;
}

const char* cc_type_registry_alias_type_at(CCTypeRegistry* reg, size_t idx) {
    if (!reg || idx >= reg->alias_count) return NULL;
    return reg->aliases[idx].type_name;
}

int cc_type_registry_add_field_ex(CCTypeRegistry* reg,
                                  const char* struct_name,
                                  const char* field_name,
                                  const char* field_type,
                                  int is_as) {
    if (!reg || !struct_name || !field_name || !field_type) return -1;
    /* Duplicate `@as` of the same embed type is still recorded so
     * `cc_type_registry_validate_as_graphs` can diagnose both paths; do not
     * silently drop the second field. */
    for (size_t i = 0; i < reg->field_count; i++) {
        if (strcmp(reg->fields[i].struct_name, struct_name) == 0 &&
            strcmp(reg->fields[i].field_name, field_name) == 0) {
            if (!cc__is_parser_placeholder_type(reg->fields[i].field_type) &&
                cc__is_parser_placeholder_type(field_type)) {
                if (is_as) reg->fields[i].is_as = 1;
                return 0;
            }
            if (!cc__is_parser_placeholder_type(reg->fields[i].field_type) &&
                strcmp(reg->fields[i].field_type, field_type) != 0) {
                return 0;
            }
            free(reg->fields[i].field_type);
            reg->fields[i].field_type = strdup(field_type);
            if (is_as) reg->fields[i].is_as = 1;
            return reg->fields[i].field_type ? 0 : -1;
        }
    }
    if (ensure_field_capacity(reg, reg->field_count + 1) != 0) return -1;
    reg->fields[reg->field_count].struct_name = strdup(struct_name);
    reg->fields[reg->field_count].field_name = strdup(field_name);
    reg->fields[reg->field_count].field_type = strdup(field_type);
    reg->fields[reg->field_count].is_as = is_as ? 1 : 0;
    if (!reg->fields[reg->field_count].struct_name ||
        !reg->fields[reg->field_count].field_name ||
        !reg->fields[reg->field_count].field_type) {
        free(reg->fields[reg->field_count].struct_name);
        free(reg->fields[reg->field_count].field_name);
        free(reg->fields[reg->field_count].field_type);
        return -1;
    }
    reg->field_count++;
    return 0;
}

int cc_type_registry_add_field(CCTypeRegistry* reg,
                               const char* struct_name,
                               const char* field_name,
                               const char* field_type) {
    return cc_type_registry_add_field_ex(reg, struct_name, field_name, field_type, 0);
}

size_t cc_type_registry_as_field_count(CCTypeRegistry* reg, const char* struct_name) {
    size_t n = 0;
    if (!reg || !struct_name) return 0;
    for (size_t i = 0; i < reg->field_count; i++) {
        if (reg->fields[i].is_as &&
            strcmp(reg->fields[i].struct_name, struct_name) == 0)
            n++;
    }
    return n;
}

int cc_type_registry_as_field_at(CCTypeRegistry* reg,
                                 const char* struct_name,
                                 size_t idx,
                                 const char** out_field_name,
                                 const char** out_field_type) {
    size_t n = 0;
    if (!reg || !struct_name) return -1;
    for (size_t i = 0; i < reg->field_count; i++) {
        if (!reg->fields[i].is_as ||
            strcmp(reg->fields[i].struct_name, struct_name) != 0)
            continue;
        if (n == idx) {
            if (out_field_name) *out_field_name = reg->fields[i].field_name;
            if (out_field_type) *out_field_type = reg->fields[i].field_type;
            return 0;
        }
        n++;
    }
    return -1;
}

int cc_type_registry_has_as_field(CCTypeRegistry* reg, const char* struct_name) {
    return cc_type_registry_as_field_count(reg, struct_name) > 0;
}

static void cc__strip_type_spelling(const char* in, char* out, size_t out_sz) {
    size_t n = 0;
    size_t i = 0;
    if (!out || out_sz == 0) return;
    out[0] = '\0';
    if (!in) return;
    while (in[i] && n + 1 < out_sz) {
        char c = in[i++];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            if (n > 0 && out[n - 1] != ' ') out[n++] = ' ';
            continue;
        }
        out[n++] = c;
    }
    while (n > 0 && out[n - 1] == ' ') n--;
    out[n] = '\0';
    {
        char* p = out;
        for (;;) {
            if (strncmp(p, "const ", 6) == 0) { p += 6; continue; }
            if (strncmp(p, "volatile ", 9) == 0) { p += 9; continue; }
            if (strncmp(p, "restrict ", 9) == 0) { p += 9; continue; }
            if (strncmp(p, "struct ", 7) == 0) { p += 7; continue; }
            if (strncmp(p, "union ", 6) == 0) { p += 6; continue; }
            break;
        }
        if (p != out) memmove(out, p, strlen(p) + 1);
    }
    /* Drop trailing pointer stars for matching Outer.field_type to T* params. */
    n = strlen(out);
    while (n > 0) {
        while (n > 0 && out[n - 1] == ' ') out[--n] = '\0';
        if (n > 0 && out[n - 1] == '*') {
            out[--n] = '\0';
            continue;
        }
        break;
    }
}

int cc_type_registry_as_field_for_type(CCTypeRegistry* reg,
                                       const char* outer_type,
                                       const char* want_type,
                                       const char** out_field_name) {
    char want[256];
    char outer[256];
    const char* hit_name = NULL;
    size_t hits = 0;
    size_t i;
    if (out_field_name) *out_field_name = NULL;
    if (!reg || !outer_type || !want_type) return -1;
    cc__strip_type_spelling(want_type, want, sizeof(want));
    cc__strip_type_spelling(outer_type, outer, sizeof(outer));
    if (!want[0] || !outer[0]) return -1;
    for (i = 0; i < reg->field_count; i++) {
        char fty[256];
        if (!reg->fields[i].is_as) continue;
        if (strcmp(reg->fields[i].struct_name, outer) != 0) continue;
        cc__strip_type_spelling(reg->fields[i].field_type, fty, sizeof(fty));
        if (strcmp(fty, want) != 0) continue;
        hits++;
        hit_name = reg->fields[i].field_name;
    }
    if (hits == 0) return -1;
    if (hits > 1) return -2;
    if (out_field_name) *out_field_name = hit_name;
    return 0;
}

typedef struct {
    char path[256];
    size_t hits;
    int cycle;
    char want[256];
} CCAsPathSearch;

static int cc__as_path_search(CCTypeRegistry* reg,
                              const char* type_key,
                              char* path,
                              size_t path_len,
                              size_t path_cap,
                              const char** visited,
                              size_t visited_n,
                              size_t visited_cap,
                              CCAsPathSearch* st) {
    size_t nas;
    size_t vi;
    size_t ai;
    if (!reg || !type_key || !type_key[0] || !st) return -1;
    for (vi = 0; vi < visited_n; vi++) {
        if (visited[vi] && strcmp(visited[vi], type_key) == 0) {
            st->cycle = 1;
            return -3;
        }
    }
    if (visited_n >= visited_cap) return -1;
    visited[visited_n] = type_key;
    nas = cc_type_registry_as_field_count(reg, type_key);
    for (ai = 0; ai < nas; ai++) {
        const char* field_name = NULL;
        const char* field_type = NULL;
        char fkey[256];
        size_t saved_len = path_len;
        size_t fn_len;
        int sub;
        if (cc_type_registry_as_field_at(reg, type_key, ai, &field_name, &field_type) != 0)
            continue;
        if (!field_name || !field_type) continue;
        cc__strip_type_spelling(field_type, fkey, sizeof(fkey));
        if (!fkey[0]) continue;
        fn_len = strlen(field_name);
        if (path_len > 0) {
            if (path_len + 1 + fn_len >= path_cap) continue;
            path[path_len++] = '.';
        } else if (fn_len >= path_cap) {
            continue;
        }
        memcpy(path + path_len, field_name, fn_len);
        path_len += fn_len;
        path[path_len] = '\0';
        if (strcmp(fkey, st->want) == 0) {
            st->hits++;
            if (st->hits == 1) {
                size_t copy = path_len < sizeof(st->path) - 1 ? path_len : sizeof(st->path) - 1;
                memcpy(st->path, path, copy);
                st->path[copy] = '\0';
            }
        }
        sub = cc__as_path_search(reg, fkey, path, path_len, path_cap,
                                 visited, visited_n + 1, visited_cap, st);
        path_len = saved_len;
        path[path_len] = '\0';
        if (sub == -3) return -3;
    }
    return 0;
}

int cc_type_registry_as_path_for_type(CCTypeRegistry* reg,
                                      const char* outer_type,
                                      const char* want_type,
                                      char* path_out,
                                      size_t path_cap) {
    char outer[256];
    char path[256];
    const char* visited[32];
    CCAsPathSearch st;
    if (path_out && path_cap) path_out[0] = '\0';
    if (!reg || !outer_type || !want_type || !path_out || path_cap == 0) return -1;
    memset(&st, 0, sizeof(st));
    cc__strip_type_spelling(want_type, st.want, sizeof(st.want));
    cc__strip_type_spelling(outer_type, outer, sizeof(outer));
    if (!st.want[0] || !outer[0]) return -1;
    path[0] = '\0';
    if (cc__as_path_search(reg, outer, path, 0, sizeof(path),
                           visited, 0, 32, &st) == -3 || st.cycle)
        return -3;
    if (st.hits == 0) return -1;
    if (st.hits > 1) return -2;
    if (strlen(st.path) + 1 > path_cap) return -1;
    memcpy(path_out, st.path, strlen(st.path) + 1);
    return 0;
}

int cc_type_registry_has_as_field_transitive(CCTypeRegistry* reg,
                                             const char* outer_type) {
    /* Entry point for expanders: only the root needs a direct @as; nested
     * embeds are discovered while walking. */
    return cc_type_registry_has_as_field(reg, outer_type);
}

typedef struct {
    char type[128];
    char path[256];
} CCAsReach;

/* True when a field type spelling is a pointer (has `*`). `@as` requires a
 * value embed; pointer stars are not stripped here (unlike path matching). */
static int cc__as_field_type_is_pointer(const char* field_type) {
    size_t i = 0;
    size_t n;
    if (!field_type || !field_type[0]) return 0;
    n = strlen(field_type);
    while (i < n) {
        if (field_type[i] == '/' && i + 1 < n && field_type[i + 1] == '/') {
            i += 2;
            while (i < n && field_type[i] != '\n') i++;
            continue;
        }
        if (field_type[i] == '/' && i + 1 < n && field_type[i + 1] == '*') {
            i += 2;
            while (i + 1 < n && !(field_type[i] == '*' && field_type[i + 1] == '/')) i++;
            if (i + 1 < n) i += 2;
            continue;
        }
        if (field_type[i] == '*') return 1;
        i++;
    }
    return 0;
}

static int cc__line_for_typedef_name(const char* src, size_t n, const char* name) {
    size_t i;
    size_t nlen;
    int line = 1;
    int best = 1;
    if (!src || !name || !name[0]) return 1;
    nlen = strlen(name);
    for (i = 0; i + nlen < n; i++) {
        if (src[i] == '\n') {
            line++;
            continue;
        }
        /* Prefer `} Name` (typedef struct body close) or bare `Name` after
         * whitespace following `}`. */
        if (src[i] == '}' ) {
            size_t j = i + 1;
            while (j < n && (src[j] == ' ' || src[j] == '\t' || src[j] == '\n' ||
                             src[j] == '\r')) {
                if (src[j] == '\n') line++;
                j++;
            }
            if (j + nlen <= n && memcmp(src + j, name, nlen) == 0 &&
                (j + nlen == n || !cc_is_ident_char(src[j + nlen]))) {
                best = line;
            }
        }
    }
    return best;
}

static int cc__as_validate_walk(CCTypeRegistry* reg,
                                const char* root,
                                const char* type_key,
                                char* path,
                                size_t path_len,
                                size_t path_cap,
                                const char** stack,
                                size_t stack_n,
                                size_t stack_cap,
                                CCAsReach* seen,
                                size_t* seen_n,
                                size_t seen_cap,
                                const char* file,
                                int line,
                                int* err_count) {
    size_t nas;
    size_t si;
    size_t ai;
    if (!reg || !root || !type_key || !type_key[0] || !err_count) return -1;
    for (si = 0; si < stack_n; si++) {
        if (stack[si] && strcmp(stack[si], type_key) == 0) {
            fprintf(stderr,
                    "%s:%d:1: error: type: cyclic @as embed graph on '%s' "
                    "(revisits '%s')\n",
                    file ? file : "<input>", line > 0 ? line : 1, root, type_key);
            fprintf(stderr,
                    "%s:%d:1: note: each @as edge must form a DAG; break the "
                    "cycle or drop one embed\n",
                    file ? file : "<input>", line > 0 ? line : 1);
            (*err_count)++;
            return -3;
        }
    }
    if (stack_n >= stack_cap) return -1;
    stack[stack_n] = type_key;
    nas = cc_type_registry_as_field_count(reg, type_key);
    for (ai = 0; ai < nas; ai++) {
        const char* field_name = NULL;
        const char* field_type = NULL;
        char fkey[256];
        char fkey_store[256];
        size_t saved_len = path_len;
        size_t fn_len;
        size_t k;
        int sub;
        if (cc_type_registry_as_field_at(reg, type_key, ai, &field_name, &field_type) != 0)
            continue;
        if (!field_name || !field_type) continue;
        /* Pointer `@as` is diagnosed separately; do not walk it as an edge. */
        if (cc__as_field_type_is_pointer(field_type)) continue;
        cc__strip_type_spelling(field_type, fkey, sizeof(fkey));
        if (!fkey[0]) continue;
        fn_len = strlen(field_name);
        if (path_len > 0) {
            if (path_len + 1 + fn_len >= path_cap) continue;
            path[path_len++] = '.';
        } else if (fn_len >= path_cap) {
            continue;
        }
        memcpy(path + path_len, field_name, fn_len);
        path_len += fn_len;
        path[path_len] = '\0';
        for (k = 0; k < *seen_n; k++) {
            if (strcmp(seen[k].type, fkey) == 0) {
                if (strcmp(seen[k].path, path) != 0) {
                    fprintf(stderr,
                            "%s:%d:1: error: type: ambiguous @as embed of '%s' "
                            "on '%s'\n",
                            file ? file : "<input>", line > 0 ? line : 1, fkey,
                            root);
                    fprintf(stderr,
                            "%s:%d:1: note: path '%s' and path '%s' both reach "
                            "'%s'; keep a single @as path\n",
                            file ? file : "<input>", line > 0 ? line : 1,
                            seen[k].path, path, fkey);
                    (*err_count)++;
                }
                break;
            }
        }
        if (k == *seen_n) {
            if (*seen_n >= seen_cap) {
                fprintf(stderr,
                        "%s:%d:1: error: type: @as embed graph on '%s' exceeds "
                        "validation capacity (%zu types); simplify the embed DAG\n",
                        file ? file : "<input>", line > 0 ? line : 1, root, seen_cap);
                (*err_count)++;
                path_len = saved_len;
                path[path_len] = '\0';
                return -1;
            }
            snprintf(seen[*seen_n].type, sizeof(seen[*seen_n].type), "%s", fkey);
            snprintf(seen[*seen_n].path, sizeof(seen[*seen_n].path), "%s", path);
            (*seen_n)++;
        }
        /* Recurse with a stable key string — fkey is stack-local. */
        snprintf(fkey_store, sizeof(fkey_store), "%s", fkey);
        /* Use seen[].type storage when possible for stack pointers. */
        {
            const char* next_key = fkey_store;
            for (k = 0; k < *seen_n; k++) {
                if (strcmp(seen[k].type, fkey) == 0) {
                    next_key = seen[k].type;
                    break;
                }
            }
            sub = cc__as_validate_walk(reg, root, next_key, path, path_len, path_cap,
                                       stack, stack_n + 1, stack_cap, seen, seen_n,
                                       seen_cap, file, line, err_count);
        }
        path_len = saved_len;
        path[path_len] = '\0';
        if (sub == -3) return -3;
        if (sub < 0) return sub;
    }
    return 0;
}

static int cc__as_diagnose_anon_fields(const char* file, const char* src, size_t n);

int cc_type_registry_validate_as_graphs(CCTypeRegistry* reg,
                                        const char* file,
                                        const char* src,
                                        size_t n) {
    char** roots = NULL;
    size_t root_count = 0;
    size_t root_cap = 0;
    size_t i;
    int err_count = 0;
    if (!reg) return 0;
    err_count += cc__as_diagnose_anon_fields(file, src, n);
    /* Pointer `@as` is ill-formed (value-embed layout / `&recv.field`). */
    for (i = 0; i < reg->field_count; i++) {
        int line;
        if (!reg->fields[i].is_as || !reg->fields[i].struct_name) continue;
        if (!cc__as_field_type_is_pointer(reg->fields[i].field_type)) continue;
        line = cc__line_for_typedef_name(src, n, reg->fields[i].struct_name);
        fprintf(stderr,
                "%s:%d:1: error: type: @as field '%s' on '%s' must be a value "
                "embed, not a pointer (got '%s')\n",
                file ? file : "<input>", line > 0 ? line : 1,
                reg->fields[i].field_name ? reg->fields[i].field_name : "?",
                reg->fields[i].struct_name,
                reg->fields[i].field_type ? reg->fields[i].field_type : "?");
        fprintf(stderr,
                "%s:%d:1: note: declare 'T %s @as;' (by value); pointer @as is "
                "not supported\n",
                file ? file : "<input>", line > 0 ? line : 1,
                reg->fields[i].field_name ? reg->fields[i].field_name : "field");
        err_count++;
    }
    for (i = 0; i < reg->field_count; i++) {
        size_t r;
        int have = 0;
        if (!reg->fields[i].is_as || !reg->fields[i].struct_name) continue;
        /* Pointer embeds already diagnosed; omit them from the DAG walk so
         * a pointer cycle does not also report "cyclic @as". */
        if (cc__as_field_type_is_pointer(reg->fields[i].field_type)) continue;
        for (r = 0; r < root_count; r++) {
            if (strcmp(roots[r], reg->fields[i].struct_name) == 0) {
                have = 1;
                break;
            }
        }
        if (have) continue;
        if (root_count + 1 > root_cap) {
            size_t nc = root_cap ? root_cap * 2 : 16;
            char** nr = (char**)realloc(roots, nc * sizeof(char*));
            if (!nr) {
                free(roots);
                return -1;
            }
            roots = nr;
            root_cap = nc;
        }
        roots[root_count] = reg->fields[i].struct_name;
        root_count++;
    }
    for (i = 0; i < root_count; i++) {
        CCAsReach seen[64];
        size_t seen_n = 0;
        const char* stack[32];
        char path[256];
        int line = cc__line_for_typedef_name(src, n, roots[i]);
        path[0] = '\0';
        {
            int walk_rc = cc__as_validate_walk(reg, roots[i], roots[i], path, 0,
                                               sizeof(path), stack, 0, 32, seen,
                                               &seen_n, 64, file, line, &err_count);
            /* Capacity / OOM aborts already increment err_count when diagnosed;
             * treat any negative walk result as failure. */
            if (walk_rc < 0 && err_count == 0) err_count++;
        }
    }
    free(roots);
    return err_count > 0 ? -1 : 0;
}

int cc_type_registry_may_need_as_arg_coerce(CCTypeRegistry* reg,
                                            const char* src,
                                            size_t n) {
    size_t i;
    if (!src || n == 0) return 0;
    /* Emit-ready buffer keeps the comment-marker form after preprocess. */
    for (i = 0; i + 7 <= n; i++) {
        if (memcmp(src + i, "/*@as*/", 7) == 0) return 1;
    }
    if (cc_contains_token_top_level(src, n, "@as")) return 1;
    if (!reg) return 0;
    /* Header-registered embeds (e.g. CCTempFile): fire when an outer type
     * that has `@as` appears in the TU text. */
    for (i = 0; i < reg->field_count; i++) {
        const char* sn;
        if (!reg->fields[i].is_as) continue;
        sn = reg->fields[i].struct_name;
        if (!sn || !sn[0]) continue;
        if (cc_contains_token_top_level(src, n, sn)) return 1;
    }
    return 0;
}

int cc_type_registry_field_is_first(CCTypeRegistry* reg,
                                    const char* struct_name,
                                    const char* field_name) {
    size_t i;
    char outer[256];
    if (!reg || !struct_name || !field_name) return 0;
    cc__strip_type_spelling(struct_name, outer, sizeof(outer));
    for (i = 0; i < reg->field_count; i++) {
        if (strcmp(reg->fields[i].struct_name, outer) != 0 &&
            strcmp(reg->fields[i].struct_name, struct_name) != 0)
            continue;
        return strcmp(reg->fields[i].field_name, field_name) == 0;
    }
    return 0;
}

static int cc__as_stmt_has_marker(const char* src, size_t a, size_t b) {
    size_t i = a;
    if (!src || a >= b) return 0;
    while (i < b) {
        if (src[i] == '/' && i + 1 < b && src[i + 1] == '/') {
            i += 2;
            while (i < b && src[i] != '\n') i++;
            continue;
        }
        if (src[i] == '/' && i + 1 < b && src[i + 1] == '*') {
            if (i + 7 <= b && memcmp(src + i, "/*@as*/", 7) == 0) return 1;
            i += 2;
            while (i + 1 < b && !(src[i] == '*' && src[i + 1] == '/')) i++;
            if (i + 1 < b) i += 2;
            continue;
        }
        if (src[i] == '"' || src[i] == '\'') {
            char q = src[i++];
            while (i < b) {
                if (src[i] == '\\' && i + 1 < b) { i += 2; continue; }
                if (src[i] == q) { i++; break; }
                i++;
            }
            continue;
        }
        if (src[i] == '@' && i + 3 <= b && src[i + 1] == 'a' && src[i + 2] == 's' &&
            (i + 3 == b || !cc_is_ident_char(src[i + 3])))
            return 1;
        i++;
    }
    return 0;
}

/* When the statement at [stmt_off, semi_off) is an anonymous
 * `union { ... };` / `struct { ... };` member, set *out_body_l/r to its
 * brace span and return 1. C11 flattens anonymous members into the
 * enclosing struct, so ingestion recurses with the same struct name. */
static int cc__stmt_is_anon_member(const char* src, size_t stmt_off, size_t semi_off,
                                   size_t* out_body_l, size_t* out_body_r) {
    size_t p = stmt_off;
    size_t rb = 0;
    while (p < semi_off && (src[p] == ' ' || src[p] == '\t' ||
                            src[p] == '\n' || src[p] == '\r'))
        p++;
    if (p + 5 < semi_off && memcmp(src + p, "union", 5) == 0 &&
        !cc_is_ident_char(src[p + 5]))
        p += 5;
    else if (p + 6 < semi_off && memcmp(src + p, "struct", 6) == 0 &&
             !cc_is_ident_char(src[p + 6]))
        p += 6;
    else
        return 0;
    while (p < semi_off && (src[p] == ' ' || src[p] == '\t' ||
                            src[p] == '\n' || src[p] == '\r'))
        p++;
    if (p >= semi_off || src[p] != '{') return 0;
    if (!cc_find_matching_brace(src, semi_off, p, &rb)) return 0;
    {
        size_t q = rb + 1;
        while (q < semi_off && (src[q] == ' ' || src[q] == '\t' ||
                                src[q] == '\n' || src[q] == '\r'))
            q++;
        if (q != semi_off) return 0; /* named member — not anonymous */
    }
    if (out_body_l) *out_body_l = p;
    if (out_body_r) *out_body_r = rb;
    return 1;
}

static void cc__ingest_struct_body_fields(CCTypeRegistry* reg,
                                          const char* src,
                                          size_t body_l,
                                          size_t body_r,
                                          const char* struct_name) {
    const char* stmt;
    const char* body_end;
    if (!reg || !src || !struct_name || !struct_name[0] || body_l >= body_r) return;
    stmt = src + body_l + 1;
    body_end = src + body_r;
    while (stmt < body_end) {
        size_t stmt_off = (size_t)(stmt - src);
        size_t end_off = (size_t)(body_end - src);
        size_t semi_off = cc_find_char_top_level(src, stmt_off, end_off, ';');
        size_t anon_l = 0, anon_r = 0;
        char field_name[128];
        char field_type[256];
        int field_is_as = 0;
        if (semi_off >= end_off) break;
        if (cc__stmt_is_anon_member(src, stmt_off, semi_off, &anon_l, &anon_r)) {
            cc__ingest_struct_body_fields(reg, src, anon_l, anon_r, struct_name);
            stmt = src + semi_off + 1;
            continue;
        }
        cc_parse_decl_name_and_type_ex(src + stmt_off, src + semi_off,
                                       field_name, sizeof(field_name),
                                       field_type, sizeof(field_type),
                                       &field_is_as);
        if (field_name[0] && field_type[0]) {
            (void)cc_type_registry_add_field_ex(reg, struct_name, field_name,
                                                field_type, field_is_as);
        }
        stmt = src + semi_off + 1;
    }
}

static int cc__as_diagnose_anon_in_body(const char* file,
                                        const char* src,
                                        size_t body_l,
                                        size_t body_r,
                                        const char* struct_name,
                                        int line) {
    const char* stmt;
    const char* body_end;
    int errs = 0;
    if (!src || !struct_name || body_l >= body_r) return 0;
    stmt = src + body_l + 1;
    body_end = src + body_r;
    while (stmt < body_end) {
        size_t stmt_off = (size_t)(stmt - src);
        size_t end_off = (size_t)(body_end - src);
        size_t semi_off = cc_find_char_top_level(src, stmt_off, end_off, ';');
        size_t anon_l = 0, anon_r = 0;
        char field_name[128];
        char field_type[256];
        int field_is_as = 0;
        if (semi_off >= end_off) break;
        if (!cc__as_stmt_has_marker(src, stmt_off, semi_off)) {
            stmt = src + semi_off + 1;
            continue;
        }
        if (cc__stmt_is_anon_member(src, stmt_off, semi_off, &anon_l, &anon_r)) {
            /* Anonymous union/struct member: fields flatten into the
             * enclosing struct; check the nested body instead. */
            errs += cc__as_diagnose_anon_in_body(file, src, anon_l, anon_r,
                                                 struct_name, line);
            stmt = src + semi_off + 1;
            continue;
        }
        cc_parse_decl_name_and_type_ex(src + stmt_off, src + semi_off,
                                       field_name, sizeof(field_name),
                                       field_type, sizeof(field_type),
                                       &field_is_as);
        /* Named form yields both name and type; anything else with an @as
         * marker is anonymous / ill-formed. */
        if (!(field_name[0] && field_type[0] && field_is_as)) {
            fprintf(stderr,
                    "%s:%d:1: error: type: anonymous @as field on '%s' is "
                    "ill-formed; declare 'Type name @as;'\n",
                    file ? file : "<input>", line > 0 ? line : 1, struct_name);
            errs++;
        }
        stmt = src + semi_off + 1;
    }
    return errs;
}

static int cc__as_diagnose_anon_fields(const char* file,
                                       const char* src,
                                       size_t n) {
    size_t i = 0;
    int errs = 0;
    if (!src || n == 0) return 0;
    while (i < n) {
        size_t body_l = 0, body_r = 0;
        char struct_name[128];
        size_t sn = 0;
        int line = 1;
        int is_typedef = 0;
        size_t struct_kw;
        /* Skip comments/strings so prose like `struct Tag { T @as; }` in a
         * file comment cannot be mistaken for a real declaration. */
        if (src[i] == '/' && i + 1 < n && src[i + 1] == '/') {
            i += 2;
            while (i < n && src[i] != '\n') i++;
            continue;
        }
        if (src[i] == '/' && i + 1 < n && src[i + 1] == '*') {
            i += 2;
            while (i + 1 < n && !(src[i] == '*' && src[i + 1] == '/')) i++;
            if (i + 1 < n) i += 2;
            continue;
        }
        if (src[i] == '"' || src[i] == '\'') {
            char q = src[i++];
            while (i < n) {
                if (src[i] == '\\' && i + 1 < n) { i += 2; continue; }
                if (src[i] == q) { i++; break; }
                i++;
            }
            continue;
        }
        if (i + 7 <= n && memcmp(src + i, "typedef", 7) == 0 &&
            (i == 0 || !cc_is_ident_char(src[i - 1])) &&
            (i + 7 >= n || !cc_is_ident_char(src[i + 7]))) {
            is_typedef = 1;
            struct_kw = cc_skip_ws_and_comments(src, n, i + 7);
        } else if (i + 6 <= n && memcmp(src + i, "struct", 6) == 0 &&
                   (i == 0 || !cc_is_ident_char(src[i - 1])) &&
                   (i + 6 >= n || !cc_is_ident_char(src[i + 6]))) {
            struct_kw = i;
        } else {
            i++;
            continue;
        }
        if (struct_kw + 6 > n || memcmp(src + struct_kw, "struct", 6) != 0 ||
            (struct_kw + 6 < n && cc_is_ident_char(src[struct_kw + 6]))) {
            i++;
            continue;
        }
        body_l = cc_skip_ws_and_comments(src, n, struct_kw + 6);
        if (body_l < n && cc_is_ident_start(src[body_l])) {
            size_t tag_end = body_l;
            while (tag_end < n && cc_is_ident_char(src[tag_end]) &&
                   sn + 1 < sizeof(struct_name)) {
                struct_name[sn++] = src[tag_end++];
            }
            struct_name[sn] = '\0';
            body_l = cc_skip_ws_and_comments(src, n, tag_end);
        }
        if (body_l >= n || src[body_l] != '{' ||
            !cc_find_matching_brace(src, n, body_l, &body_r)) {
            i++;
            continue;
        }
        if (is_typedef) {
            size_t name_pos = cc_skip_ws_and_comments(src, n, body_r + 1);
            sn = 0;
            if (name_pos < n && cc_is_ident_start(src[name_pos])) {
                while (name_pos < n && cc_is_ident_char(src[name_pos]) &&
                       sn + 1 < sizeof(struct_name)) {
                    struct_name[sn++] = src[name_pos++];
                }
                struct_name[sn] = '\0';
            }
        }
        if (struct_name[0]) {
            line = cc__line_for_typedef_name(src, n, struct_name);
            errs += cc__as_diagnose_anon_in_body(file, src, body_l, body_r,
                                                 struct_name, line);
        }
        i = body_r + 1;
    }
    return errs;
}

/* Early text scan for `.ufcs_dynamic` registrations so the pre-splice
 * text-UFCS pass can defer sink receivers before the comptime scan runs.
 * A false positive (registration text inside a comment) only defers that
 * type's lowering to the AST pass — fail-closed. */
static void cc__scan_dynamic_sink_registrations(CCTypeRegistry* reg,
                                                const char* src, size_t n) {
    size_t i = 0;
    static const char kw[] = "cc_type_register";
    static const char field[] = ".ufcs_dynamic";
    if (!reg || !src) return;
    while (i + sizeof(kw) - 1 < n) {
        const char* hit = (const char*)memmem(src + i, n - i, kw, sizeof(kw) - 1);
        size_t p, name_a, name_b, depth;
        char type_name[128];
        if (!hit) return;
        p = (size_t)(hit - src) + sizeof(kw) - 1;
        i = p;
        while (p < n && isspace((unsigned char)src[p])) p++;
        if (p >= n || src[p] != '(') continue;
        p++;
        while (p < n && isspace((unsigned char)src[p])) p++;
        if (p >= n || src[p] != '"') continue;
        p++;
        name_a = p;
        while (p < n && src[p] != '"') p++;
        if (p >= n) return;
        name_b = p;
        if (name_b <= name_a || name_b - name_a >= sizeof(type_name)) continue;
        memcpy(type_name, src + name_a, name_b - name_a);
        type_name[name_b - name_a] = '\0';
        /* Scan the registration's argument list for the field name. */
        depth = 1;
        p++;
        {
            size_t body_a = p;
            while (p < n && depth > 0) {
                char c = src[p];
                if (c == '(') depth++;
                else if (c == ')') depth--;
                else if (c == '"') {
                    p++;
                    while (p < n && src[p] != '"') {
                        if (src[p] == '\\' && p + 1 < n) p++;
                        p++;
                    }
                }
                p++;
            }
            {
                const char* fhit = (const char*)memmem(src + body_a, p - body_a,
                                                       field, sizeof(field) - 1);
                if (fhit) {
                    int f_dest = (fhit + sizeof(field) - 1 < src + p &&
                                  fhit[sizeof(field) - 1] == '2');
                    /* Extract cc_type_dynamic_call("callee", "wrap"). */
                    static const char call[] = "cc_type_dynamic_call";
                    const char* chit = (const char*)memmem(
                        fhit, (size_t)(src + p - fhit), call, sizeof(call) - 1);
                    char callee[256];
                    char wrapm[256];
                    callee[0] = wrapm[0] = '\0';
                    if (chit) {
                        size_t q = (size_t)(chit - src) + sizeof(call) - 1;
                        int which = 0;
                        while (q < p && which < 2) {
                            if (src[q] == '"') {
                                size_t sa = ++q;
                                while (q < p && src[q] != '"') q++;
                                if (q >= p) break;
                                {
                                    size_t len = q - sa;
                                    char* dst = which == 0 ? callee : wrapm;
                                    if (len < 256) {
                                        memcpy(dst, src + sa, len);
                                        dst[len] = '\0';
                                    }
                                }
                                which++;
                            }
                            q++;
                        }
                    }
                    if (callee[0] && wrapm[0]) {
                        if (f_dest)
                            (void)cc_type_registry_set_dynamic_sink2(reg, type_name,
                                                                     callee, wrapm);
                        else
                            (void)cc_type_registry_set_dynamic_sink(reg, type_name,
                                                                    callee, wrapm);
                    }
                }
            }
        }
        i = p;
    }
}

void cc_type_registry_ingest_struct_fields(CCTypeRegistry* reg,
                                           const char* src,
                                           size_t n) {
    size_t i = 0;
    if (!reg || !src || n == 0) return;
    cc__scan_dynamic_sink_registrations(reg, src, n);
    while (i < n) {
        size_t body_l = 0, body_r = 0;
        char struct_name[128];
        size_t sn = 0;
        int is_typedef = 0;
        size_t struct_kw;
        if (src[i] == '/' && i + 1 < n && src[i + 1] == '/') {
            i += 2;
            while (i < n && src[i] != '\n') i++;
            continue;
        }
        if (src[i] == '/' && i + 1 < n && src[i + 1] == '*') {
            i += 2;
            while (i + 1 < n && !(src[i] == '*' && src[i + 1] == '/')) i++;
            if (i + 1 < n) i += 2;
            continue;
        }
        if (src[i] == '"' || src[i] == '\'') {
            char q = src[i++];
            while (i < n) {
                if (src[i] == '\\' && i + 1 < n) { i += 2; continue; }
                if (src[i] == q) { i++; break; }
                i++;
            }
            continue;
        }
        if (i + 7 <= n && memcmp(src + i, "typedef", 7) == 0 &&
            (i == 0 || !cc_is_ident_char(src[i - 1])) &&
            (i + 7 >= n || !cc_is_ident_char(src[i + 7]))) {
            is_typedef = 1;
            struct_kw = cc_skip_ws_and_comments(src, n, i + 7);
        } else if (i + 6 <= n && memcmp(src + i, "struct", 6) == 0 &&
                   (i == 0 || !cc_is_ident_char(src[i - 1])) &&
                   (i + 6 >= n || !cc_is_ident_char(src[i + 6]))) {
            struct_kw = i;
        } else {
            i++;
            continue;
        }
        if (struct_kw + 6 > n || memcmp(src + struct_kw, "struct", 6) != 0 ||
            (struct_kw + 6 < n && cc_is_ident_char(src[struct_kw + 6]))) {
            i++;
            continue;
        }
        body_l = cc_skip_ws_and_comments(src, n, struct_kw + 6);
        /* Tagged form: struct Tag { ... } — keep the tag as the registry key. */
        if (body_l < n && cc_is_ident_start(src[body_l])) {
            size_t tag_end = body_l;
            while (tag_end < n && cc_is_ident_char(src[tag_end]) &&
                   sn + 1 < sizeof(struct_name)) {
                struct_name[sn++] = src[tag_end++];
            }
            struct_name[sn] = '\0';
            body_l = cc_skip_ws_and_comments(src, n, tag_end);
        }
        if (body_l >= n || src[body_l] != '{' ||
            !cc_find_matching_brace(src, n, body_l, &body_r)) {
            i++;
            continue;
        }
        if (is_typedef) {
            size_t name_pos = cc_skip_ws_and_comments(src, n, body_r + 1);
            /* Prefer the typedef alias name when present. */
            if (name_pos < n && cc_is_ident_start(src[name_pos])) {
                sn = 0;
                while (name_pos < n && cc_is_ident_char(src[name_pos]) &&
                       sn + 1 < sizeof(struct_name)) {
                    struct_name[sn++] = src[name_pos++];
                }
                struct_name[sn] = '\0';
            }
        }
        if (struct_name[0])
            cc__ingest_struct_body_fields(reg, src, body_l, body_r, struct_name);
        i = body_r + 1;
    }
}

const char* cc_type_registry_lookup_field(CCTypeRegistry* reg,
                                          const char* struct_name,
                                          const char* field_name) {
    if (!reg || !struct_name || !field_name) return NULL;
    for (size_t i = 0; i < reg->field_count; i++) {
        if (strcmp(reg->fields[i].struct_name, struct_name) == 0 &&
            strcmp(reg->fields[i].field_name, field_name) == 0) {
            return reg->fields[i].field_type;
        }
    }
    return NULL;
}

const char* cc_type_registry_lookup_unique_field_type(CCTypeRegistry* reg,
                                                     const char* field_name) {
    const char* found = NULL;
    if (!reg || !field_name || !field_name[0]) return NULL;
    for (size_t i = 0; i < reg->field_count; i++) {
        if (strcmp(reg->fields[i].field_name, field_name) != 0) continue;
        if (!found) {
            found = reg->fields[i].field_type;
            continue;
        }
        if (strcmp(found, reg->fields[i].field_type) != 0) return NULL;
    }
    return found;
}

static void cc__copy_type_base(char* out, size_t out_sz, const char* type_name) {
    size_t len = 0;
    size_t start = 0;
    if (!out || out_sz == 0) return;
    out[0] = '\0';
    if (!type_name) return;
    len = strlen(type_name);
    while (len > 0 && (type_name[len - 1] == ' ' || type_name[len - 1] == '\t')) len--;
    while (len > 0 && type_name[len - 1] == '*') len--;
    while (len > 0 && (type_name[len - 1] == ' ' || type_name[len - 1] == '\t')) len--;
    /* Drop leading cv-qualifiers so `const Db*` field lookup hits `Db`. */
    while (start < len) {
        size_t rest = len - start;
        size_t kw = 0;
        if (rest >= 5 && memcmp(type_name + start, "const", 5) == 0) kw = 5;
        else if (rest >= 8 && memcmp(type_name + start, "volatile", 8) == 0) kw = 8;
        else if (rest >= 8 && memcmp(type_name + start, "restrict", 8) == 0) kw = 8;
        if (kw == 0) break;
        if (start + kw < len &&
            (isalnum((unsigned char)type_name[start + kw]) || type_name[start + kw] == '_')) {
            break;
        }
        start += kw;
        while (start < len && (type_name[start] == ' ' || type_name[start] == '\t')) start++;
    }
    if (start >= len) return;
    len -= start;
    if (len >= out_sz) len = out_sz - 1;
    memcpy(out, type_name + start, len);
    out[len] = '\0';
}

static void cc__trim_type_text(char* text) {
    size_t len;
    size_t start = 0;
    if (!text) return;
    len = strlen(text);
    while (start < len && isspace((unsigned char)text[start])) start++;
    while (len > start && isspace((unsigned char)text[len - 1])) len--;
    if (start > 0 && len > start) memmove(text, text + start, len - start);
    if (len >= start) text[len - start] = '\0';
}

static void cc__normalize_registry_container_param_name(CCTypeRegistry* reg,
                                                        const char* type_name,
                                                        char* out,
                                                        size_t out_sz) {
    char work[256];
    char inner[256];
    char key[256];
    char val[256];
    char mangled_inner[128];
    char mangled_key[128];
    char mangled_val[128];
    size_t len = 0;
    int ptr_count = 0;
    if (!out || out_sz == 0) return;
    out[0] = '\0';
    if (!type_name || !type_name[0]) return;

    cc__normalize_local_decl_type(work, sizeof(work), type_name);
    if (!work[0]) snprintf(work, sizeof(work), "%s", type_name);
    cc__trim_type_text(work);
    len = strlen(work);
    while (len > 0 && work[len - 1] == '*') {
        ptr_count++;
        work[--len] = '\0';
        cc__trim_type_text(work);
        len = strlen(work);
    }

    if (strcmp(work, "CCString") == 0) {
        snprintf(work, sizeof(work), "CCVec_char");
    }

    if ((strncmp(work, "CCVec::[", 8) == 0 && work[strlen(work) - 1] == ']') ||
        (strncmp(work, "__CC_VEC(", 9) == 0 && work[strlen(work) - 1] == ')')) {
        size_t prefix = (strncmp(work, "__CC_VEC(", 9) == 0) ? 9 :
                        8;
        size_t inner_len = strlen(work) - prefix - 1;
        if (inner_len >= sizeof(inner)) inner_len = sizeof(inner) - 1;
        memcpy(inner, work + prefix, inner_len);
        inner[inner_len] = '\0';
        cc__normalize_registry_container_param_name(reg, inner, inner, sizeof(inner));
        cc_result_spec_mangle_type(inner, strlen(inner), mangled_inner, sizeof(mangled_inner));
        if (mangled_inner[0]) snprintf(work, sizeof(work), "CCVec_%s", mangled_inner);
    } else if ((strncmp(work, "ArrayMap::[", 11) == 0 && work[strlen(work) - 1] == ']') ||
               (strncmp(work, "__CC_ARRAY_MAP(", 15) == 0 && work[strlen(work) - 1] == ')') ||
               (strncmp(work, "Map::[", 6) == 0 && work[strlen(work) - 1] == ']') ||
               (strncmp(work, "Map<", 4) == 0 && work[strlen(work) - 1] == '>') ||
               (strncmp(work, "__CC_MAP(", 9) == 0 && work[strlen(work) - 1] == ')')) {
        int is_array = (strncmp(work, "ArrayMap::[", 11) == 0) ||
                       (strncmp(work, "__CC_ARRAY_MAP(", 15) == 0);
        const char* params = work + (is_array
                                         ? ((strncmp(work, "__CC_ARRAY_MAP(", 15) == 0) ? 15 : 11)
                                         : ((strncmp(work, "__CC_MAP(", 9) == 0)
                                                ? 9
                                                : ((work[3] == ':') ? 6 : 4)));
        size_t params_len = strlen(work) - (size_t)(params - work) - 1;
        int depth = 0;
        const char* comma = NULL;
        if (params_len >= sizeof(inner)) params_len = sizeof(inner) - 1;
        memcpy(inner, params, params_len);
        inner[params_len] = '\0';
        for (size_t i = 0; inner[i]; ++i) {
            char c = inner[i];
            if (c == '<' || c == '[' || c == '(' || c == '{') depth++;
            else if (c == '>' || c == ']' || c == ')' || c == '}') depth--;
            else if (c == ',' && depth == 0) {
                comma = inner + i;
                break;
            }
        }
        if (comma) {
            size_t key_len = (size_t)(comma - inner);
            size_t val_len = strlen(comma + 1);
            if (key_len >= sizeof(key)) key_len = sizeof(key) - 1;
            if (val_len >= sizeof(val)) val_len = sizeof(val) - 1;
            memcpy(key, inner, key_len);
            key[key_len] = '\0';
            memcpy(val, comma + 1, val_len);
            val[val_len] = '\0';
            cc__normalize_registry_container_param_name(reg, key, key, sizeof(key));
            cc__normalize_registry_container_param_name(reg, val, val, sizeof(val));
            cc_result_spec_mangle_type(key, strlen(key), mangled_key, sizeof(mangled_key));
            cc_result_spec_mangle_type(val, strlen(val), mangled_val, sizeof(mangled_val));
            if (mangled_key[0] && mangled_val[0]) {
                snprintf(work, sizeof(work), "%s_%s_%s", is_array ? "ArrayMap" : "Map",
                         mangled_key, mangled_val);
            }
        }
    }

    snprintf(out, out_sz, "%s", work);
    while (ptr_count-- > 0 && strlen(out) + 1 < out_sz) strcat(out, "*");
}

static void cc__normalize_registry_type_name(CCTypeRegistry* reg,
                                             const char* type_name,
                                             char* out,
                                             size_t out_sz) {
    char work[256];
    char inner[256];
    char key[256];
    char val[256];
    char mangled_inner[128];
    char mangled_key[128];
    char mangled_val[128];
    const char* alias = NULL;
    size_t len = 0;
    int ptr_count = 0;
    if (!out || out_sz == 0) return;
    out[0] = '\0';
    if (!type_name || !type_name[0]) return;

    cc__normalize_local_decl_type(work, sizeof(work), type_name);
    if (!work[0]) {
        snprintf(work, sizeof(work), "%s", type_name);
    }
    cc__trim_type_text(work);
    len = strlen(work);
    while (len > 0 && work[len - 1] == '*') {
        ptr_count++;
        work[--len] = '\0';
        cc__trim_type_text(work);
        len = strlen(work);
    }

    for (int hop = 0; reg && hop < 8; ++hop) {
        alias = cc_type_registry_lookup_alias(reg, work);
        if (!alias || !alias[0] || strcmp(alias, work) == 0) break;
        snprintf(work, sizeof(work), "%s", alias);
        cc__trim_type_text(work);
        len = strlen(work);
        /* Alias targets may themselves carry `*` (Map/ArrayMap pointer
         * typedefs).  Fold those into ptr_count so later expansion of
         * `__CC_MAP(...)` / `__CC_ARRAY_MAP(...)` still matches. */
        while (len > 0 && work[len - 1] == '*') {
            ptr_count++;
            work[--len] = '\0';
            cc__trim_type_text(work);
            len = strlen(work);
        }
    }
    if ((strncmp(work, "CCVec::[", 8) == 0 && work[strlen(work) - 1] == ']') ||
        (strncmp(work, "__CC_VEC(", 9) == 0 && work[strlen(work) - 1] == ')')) {
        size_t prefix = (strncmp(work, "__CC_VEC(", 9) == 0) ? 9 :
                        8;
        size_t inner_len = strlen(work) - prefix - 1;
        if (inner_len >= sizeof(inner)) inner_len = sizeof(inner) - 1;
        memcpy(inner, work + prefix, inner_len);
        inner[inner_len] = '\0';
        cc__normalize_registry_container_param_name(reg, inner, inner, sizeof(inner));
        cc_result_spec_mangle_type(inner, strlen(inner), mangled_inner, sizeof(mangled_inner));
        if (mangled_inner[0]) snprintf(work, sizeof(work), "CCVec_%s", mangled_inner);
    } else if ((strncmp(work, "ArrayMap::[", 11) == 0 && work[strlen(work) - 1] == ']') ||
               (strncmp(work, "__CC_ARRAY_MAP(", 15) == 0 && work[strlen(work) - 1] == ')') ||
               (strncmp(work, "Map::[", 6) == 0 && work[strlen(work) - 1] == ']') ||
               (strncmp(work, "Map<", 4) == 0 && work[strlen(work) - 1] == '>') ||
               (strncmp(work, "__CC_MAP(", 9) == 0 && work[strlen(work) - 1] == ')')) {
        int is_array = (strncmp(work, "ArrayMap::[", 11) == 0) ||
                       (strncmp(work, "__CC_ARRAY_MAP(", 15) == 0);
        const char* params = work + (is_array
                                         ? ((strncmp(work, "__CC_ARRAY_MAP(", 15) == 0) ? 15 : 11)
                                         : ((strncmp(work, "__CC_MAP(", 9) == 0)
                                                ? 9
                                                : ((work[3] == ':') ? 6 : 4)));
        size_t params_len = strlen(work) - (size_t)(params - work) - 1;
        int depth = 0;
        const char* comma = NULL;
        if (params_len >= sizeof(inner)) params_len = sizeof(inner) - 1;
        memcpy(inner, params, params_len);
        inner[params_len] = '\0';
        for (size_t i = 0; inner[i]; ++i) {
            char c = inner[i];
            if (c == '<' || c == '[' || c == '(' || c == '{') depth++;
            else if (c == '>' || c == ']' || c == ')' || c == '}') depth--;
            else if (c == ',' && depth == 0) {
                comma = inner + i;
                break;
            }
        }
        if (comma) {
            size_t key_len = (size_t)(comma - inner);
            size_t val_len = strlen(comma + 1);
            if (key_len >= sizeof(key)) key_len = sizeof(key) - 1;
            if (val_len >= sizeof(val)) val_len = sizeof(val) - 1;
            memcpy(key, inner, key_len);
            key[key_len] = '\0';
            memcpy(val, comma + 1, val_len);
            val[val_len] = '\0';
            cc__normalize_registry_container_param_name(reg, key, key, sizeof(key));
            cc__normalize_registry_container_param_name(reg, val, val, sizeof(val));
            cc_result_spec_mangle_type(key, strlen(key), mangled_key, sizeof(mangled_key));
            cc_result_spec_mangle_type(val, strlen(val), mangled_val, sizeof(mangled_val));
            if (mangled_key[0] && mangled_val[0]) {
                snprintf(work, sizeof(work), "%s_%s_%s", is_array ? "ArrayMap" : "Map",
                         mangled_key, mangled_val);
            }
        }
    }

    snprintf(out, out_sz, "%s", work);
    while (ptr_count-- > 0 && strlen(out) + 1 < out_sz) strcat(out, "*");
}

const char* cc_type_registry_canonicalize_type_name(CCTypeRegistry* reg,
                                                    const char* type_name,
                                                    char* out,
                                                    size_t out_sz) {
    if (!out || out_sz == 0) return NULL;
    cc__normalize_registry_type_name(reg, type_name, out, out_sz);
    return out[0] ? out : NULL;
}

/* cc__parse_decl_name_and_type_local — now delegated to cc_parse_decl_name_and_type in util/text.h */

/* cc__is_non_decl_stmt_type — now cc_is_non_decl_stmt_type in util/text.h */

static void cc__trim_type_span(const char** start, const char** end) {
    while (*start < *end && isspace((unsigned char)**start)) (*start)++;
    while (*end > *start && isspace((unsigned char)(*end)[-1])) (*end)--;
}

static void cc__normalize_local_decl_type(char* out, size_t out_sz, const char* type_name) {
    const char* bang;
    const char* chan_l;
    const char* chan_r;
    const char* ok_s;
    const char* ok_e;
    const char* err_s;
    const char* err_e;
    const char* elem_s;
    const char* elem_e;
    char mangled_ok[128];
    char mangled_err[128];
    char mangled_elem[128];
    int chan_is_tx = 0;
    int chan_is_rx = 0;
    if (!out || out_sz == 0) return;
    out[0] = '\0';
    if (!type_name || !type_name[0]) return;
    chan_l = strchr(type_name, '[');
    if (chan_l) {
        chan_r = strchr(chan_l, ']');
        if (chan_r && memchr(chan_l, '~', (size_t)(chan_r - chan_l)) != NULL) {
            if (memchr(chan_l, '>', (size_t)(chan_r - chan_l)) != NULL) chan_is_tx = 1;
            if (memchr(chan_l, '<', (size_t)(chan_r - chan_l)) != NULL) chan_is_rx = 1;
            if (chan_is_tx || chan_is_rx) {
                elem_s = type_name;
                elem_e = chan_l;
                cc__trim_type_span(&elem_s, &elem_e);
                if (elem_e > elem_s) {
                    cc_result_spec_mangle_type(elem_s, (size_t)(elem_e - elem_s),
                                               mangled_elem, sizeof(mangled_elem));
                    if (mangled_elem[0]) {
                        snprintf(out, out_sz, "%s_%s",
                                 chan_is_tx ? "CCChanTx" : "CCChanRx", mangled_elem);
                        return;
                    }
                }
            }
        }
    }
    bang = strchr(type_name, '!');
    if (!bang || bang[1] == '=') {
        strncpy(out, type_name, out_sz - 1);
        out[out_sz - 1] = '\0';
        return;
    }
    ok_s = type_name;
    ok_e = bang;
    cc__trim_type_span(&ok_s, &ok_e);
    err_s = bang + 1;
    while (*err_s == ' ' || *err_s == '\t') err_s++;
    if (*err_s == '>') {
        err_s++;
        while (*err_s == ' ' || *err_s == '\t') err_s++;
        if (*err_s == '(') {
            err_s++;
            err_e = strchr(err_s, ')');
            if (!err_e) err_e = err_s + strlen(err_s);
        } else {
            err_e = err_s + strlen(err_s);
        }
    } else {
        err_e = err_s;
        while (*err_e && (isalnum((unsigned char)*err_e) || *err_e == '_')) err_e++;
    }
    cc__trim_type_span(&err_s, &err_e);
    if (ok_e <= ok_s || err_e <= err_s) {
        strncpy(out, type_name, out_sz - 1);
        out[out_sz - 1] = '\0';
        return;
    }
    cc_result_spec_mangle_type(ok_s, (size_t)(ok_e - ok_s), mangled_ok, sizeof(mangled_ok));
    cc_result_spec_mangle_type(err_s, (size_t)(err_e - err_s), mangled_err, sizeof(mangled_err));
    if (!mangled_ok[0] || !mangled_err[0]) {
        strncpy(out, type_name, out_sz - 1);
        out[out_sz - 1] = '\0';
        return;
    }
    snprintf(out, out_sz, "CCResult_%s_%s", mangled_ok, mangled_err);
}

/* One-pass scoped decl index for registry receiver resolution. The old
 * per-call walk rescanned src[0..use_offset) on every resolve. */
typedef struct {
    size_t pos; /* ';' of the decl, or '{' for a param binding */
    int scope_id;
    char name[128];
    char type[256];
} CCRegIdxDecl;

typedef struct {
    const char* src;
    size_t n;
    CCRegIdxDecl* decls;
    size_t n_decls;
    size_t decls_cap;
    size_t* scope_close;
    size_t n_scopes;
    size_t scopes_cap;
    int ready;
} CCRegScopeIndex;

static CCRegScopeIndex g_reg_scope_idx;

static void cc__reg_scope_idx_reset(void) {
    free(g_reg_scope_idx.decls);
    free(g_reg_scope_idx.scope_close);
    memset(&g_reg_scope_idx, 0, sizeof(g_reg_scope_idx));
}

static int cc__reg_scope_idx_ensure_scopes(size_t need) {
    size_t* nv;
    size_t cap;
    size_t i;
    if (g_reg_scope_idx.scopes_cap >= need) return 0;
    cap = g_reg_scope_idx.scopes_cap ? g_reg_scope_idx.scopes_cap : 64;
    while (cap < need) cap *= 2;
    nv = (size_t*)realloc(g_reg_scope_idx.scope_close, cap * sizeof(*nv));
    if (!nv) return -1;
    for (i = g_reg_scope_idx.scopes_cap; i < cap; i++) nv[i] = (size_t)-1;
    g_reg_scope_idx.scope_close = nv;
    g_reg_scope_idx.scopes_cap = cap;
    return 0;
}

static int cc__reg_scope_idx_push_decl(size_t pos, int scope_id,
                                       const char* name, const char* type) {
    CCRegIdxDecl* slot;
    if (!name || !name[0] || !type || !type[0]) return 0;
    if (g_reg_scope_idx.n_decls == g_reg_scope_idx.decls_cap) {
        size_t cap = g_reg_scope_idx.decls_cap ? g_reg_scope_idx.decls_cap * 2 : 128;
        CCRegIdxDecl* nv =
            (CCRegIdxDecl*)realloc(g_reg_scope_idx.decls, cap * sizeof(*nv));
        if (!nv) return -1;
        g_reg_scope_idx.decls = nv;
        g_reg_scope_idx.decls_cap = cap;
    }
    slot = &g_reg_scope_idx.decls[g_reg_scope_idx.n_decls++];
    slot->pos = pos;
    slot->scope_id = scope_id;
    snprintf(slot->name, sizeof(slot->name), "%s", name);
    cc__normalize_local_decl_type(slot->type, sizeof(slot->type), type);
    return 0;
}

static void cc__reg_scope_idx_build(const char* src, size_t n) {
    enum { MAX_SCOPES = 256 };
    int scope_stack[MAX_SCOPES];
    int scope_depth = 1;
    int next_scope_id = 1;
    int in_lc = 0, in_bc = 0, in_str = 0, in_chr = 0;
    size_t stmt_start = 0;
    size_t i = 0;
    cc__reg_scope_idx_reset();
    if (!src || n == 0) return;
    g_reg_scope_idx.src = src;
    g_reg_scope_idx.n = n;
    scope_stack[0] = 0;
    if (cc__reg_scope_idx_ensure_scopes(1) != 0) return;
    g_reg_scope_idx.n_scopes = 1;
    g_reg_scope_idx.scope_close[0] = (size_t)-1;
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
        if (c == '{') {
            int new_scope_id = next_scope_id++;
            if (scope_depth < MAX_SCOPES) {
                scope_stack[scope_depth++] = new_scope_id;
                if (cc__reg_scope_idx_ensure_scopes((size_t)new_scope_id + 1) != 0)
                    return;
                if ((size_t)new_scope_id + 1 > g_reg_scope_idx.n_scopes)
                    g_reg_scope_idx.n_scopes = (size_t)new_scope_id + 1;
                g_reg_scope_idx.scope_close[new_scope_id] = (size_t)-1;
            }
            {
                size_t close = i;
                while (close > 0 && isspace((unsigned char)src[close - 1])) close--;
                if (close > 0 && src[close - 1] == ')') {
                    size_t open = close - 1;
                    int depth = 1;
                    while (open > 0 && depth > 0) {
                        open--;
                        if (src[open] == ')') depth++;
                        else if (src[open] == '(') depth--;
                    }
                    if (depth == 0) {
                        size_t param_start = open + 1;
                        size_t p = param_start;
                        int par = 0, br = 0;
                        while (p <= close - 1) {
                            if (p == close - 1 ||
                                (src[p] == ',' && par == 0 && br == 0)) {
                                char decl_name[128];
                                char decl_type[256];
                                cc_parse_decl_name_and_type(
                                    src + param_start, src + p, decl_name,
                                    sizeof(decl_name), decl_type, sizeof(decl_type));
                                if (decl_name[0] && strcmp(decl_type, "void") != 0 &&
                                    !cc_is_non_decl_stmt_type(decl_type)) {
                                    if (cc__reg_scope_idx_push_decl(
                                            i, new_scope_id, decl_name, decl_type) != 0)
                                        return;
                                }
                                param_start = p + 1;
                            } else if (src[p] == '(') par++;
                            else if (src[p] == ')' && par > 0) par--;
                            else if (src[p] == '[') br++;
                            else if (src[p] == ']' && br > 0) br--;
                            p++;
                        }
                    }
                }
            }
            stmt_start = i + 1;
            i++;
            continue;
        }
        if (c == '}') {
            if (scope_depth > 1) {
                int closing_scope = scope_stack[scope_depth - 1];
                if ((size_t)closing_scope < g_reg_scope_idx.scopes_cap)
                    g_reg_scope_idx.scope_close[closing_scope] = i;
                scope_depth--;
            }
            stmt_start = i + 1;
            i++;
            continue;
        }
        if (c == ';') {
            char decl_name[128];
            char decl_type[256];
            cc_parse_decl_name_and_type(src + stmt_start, src + i, decl_name,
                                         sizeof(decl_name), decl_type,
                                         sizeof(decl_type));
            if (decl_name[0] && !cc_is_non_decl_stmt_type(decl_type)) {
                if (cc__reg_scope_idx_push_decl(i, scope_stack[scope_depth - 1],
                                                decl_name, decl_type) != 0)
                    return;
            }
            stmt_start = i + 1;
        }
        i++;
    }
    g_reg_scope_idx.ready = 1;
}

static void cc__reg_scope_idx_ensure(const char* src, size_t n) {
    if (!src || n == 0) return;
    if (g_reg_scope_idx.ready && g_reg_scope_idx.src == src &&
        g_reg_scope_idx.n == n)
        return;
    cc__reg_scope_idx_build(src, n);
}

static const char* cc__lookup_scoped_local_var_type(const char* src,
                                                    size_t limit,
                                                    const char* var_name,
                                                    char* out_type,
                                                    size_t out_type_sz) {
    size_t di;
    const CCRegIdxDecl* best = NULL;
    size_t n;
    if (!src || !var_name || !var_name[0] || !out_type || out_type_sz == 0) return NULL;
    out_type[0] = '\0';
    n = strlen(src);
    if (limit > n) limit = n;
    cc__reg_scope_idx_ensure(src, n);
    if (!g_reg_scope_idx.ready || g_reg_scope_idx.src != src) return NULL;
    for (di = 0; di < g_reg_scope_idx.n_decls; di++) {
        const CCRegIdxDecl* d = &g_reg_scope_idx.decls[di];
        size_t close;
        if (d->pos >= limit) break;
        if (strcmp(d->name, var_name) != 0) continue;
        if ((size_t)d->scope_id >= g_reg_scope_idx.n_scopes) continue;
        close = g_reg_scope_idx.scope_close[d->scope_id];
        if (close < limit) continue;
        best = d;
    }
    if (!best) return NULL;
    strncpy(out_type, best->type, out_type_sz - 1);
    out_type[out_type_sz - 1] = '\0';
    return out_type;
}

/* After `expr[i]`, peel one array/pointer level from a registry type spelling.
 * `T*` / `T[n]` / `T[]` become `T`.  When normalization already decayed a
 * pointer field (`Shard*` → `Shard`), peeling is a no-op and still succeeds. */
static int cc__peel_indexed_type(char* type_buf) {
    size_t len;
    if (!type_buf) return 0;
    len = strlen(type_buf);
    while (len > 0 && (type_buf[len - 1] == ' ' || type_buf[len - 1] == '\t')) len--;
    if (len > 0 && type_buf[len - 1] == '*') {
        len--;
        while (len > 0 && (type_buf[len - 1] == ' ' || type_buf[len - 1] == '\t')) len--;
        type_buf[len] = '\0';
        return 1;
    }
    if (len > 0 && type_buf[len - 1] == ']') {
        size_t i = len - 1;
        int depth = 1;
        while (i > 0 && depth > 0) {
            i--;
            if (type_buf[i] == ']') depth++;
            else if (type_buf[i] == '[') depth--;
        }
        if (depth != 0) return 0;
        while (i > 0 && (type_buf[i - 1] == ' ' || type_buf[i - 1] == '\t')) i--;
        type_buf[i] = '\0';
        return 1;
    }
    return 1;
}

/* Walk `.` / `->` / `[index]` suffixes, updating resolved_type in place.
 * Returns 0 on malformed / unresolved field access. */
static int cc__walk_receiver_postfix(CCTypeRegistry* reg,
                                     const char** io_p,
                                     char* resolved_type,
                                     size_t resolved_cap) {
    const char* p;
    if (!reg || !io_p || !resolved_type) return 0;
    p = *io_p;
    while (*p) {
        char field_name[128];
        char base_type[256];
        const char* type_name;
        size_t fn = 0;
        int is_arrow = 0;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '[') {
            /* Keep the subscript as part of the receiver; index into arrays
             * / pointers so `arr[i].field` / `p->arr[i].field` resolve. */
            int depth = 1;
            p++;
            while (*p && depth > 0) {
                if (*p == '"' || *p == '\'') {
                    char q = *p++;
                    while (*p) {
                        if (*p == '\\' && p[1]) { p += 2; continue; }
                        if (*p == q) { p++; break; }
                        p++;
                    }
                    continue;
                }
                if (*p == '[') depth++;
                else if (*p == ']') depth--;
                p++;
            }
            if (depth != 0) return 0;
            if (!cc__peel_indexed_type(resolved_type)) return 0;
            continue;
        }
        if (*p == '.') {
            p++;
        } else if (*p == '-' && p[1] == '>') {
            is_arrow = 1;
            p += 2;
        } else {
            break;
        }
        while (*p == ' ' || *p == '\t') p++;
        if (!(isalnum((unsigned char)*p) || *p == '_') ||
            !(((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || *p == '_')))
            return 0;
        while (*p && (isalnum((unsigned char)*p) || *p == '_')) {
            if (fn + 1 < sizeof(field_name)) field_name[fn] = *p;
            fn++;
            p++;
        }
        field_name[fn < sizeof(field_name) ? fn : sizeof(field_name) - 1] = '\0';
        cc__copy_type_base(base_type, sizeof(base_type), resolved_type);
        if (is_arrow && base_type[0] == '\0') return 0;
        type_name = cc_type_registry_lookup_field(reg, base_type, field_name);
        if (!type_name) return 0;
        cc__normalize_registry_type_name(reg, type_name, resolved_type, resolved_cap);
    }
    *io_p = p;
    return 1;
}

const char* cc_type_registry_resolve_receiver_expr(CCTypeRegistry* reg,
                                                   const char* recv_expr,
                                                   int* out_recv_is_ptr) {
    static _Thread_local char resolved_type[256];
    char expr[256];
    char root[128];
    const char* p;
    const char* type_name;
    int recv_is_ptr = 0;
    size_t len;
    if (out_recv_is_ptr) *out_recv_is_ptr = 0;
    if (!reg || !recv_expr) return NULL;
    while (*recv_expr == ' ' || *recv_expr == '\t' || *recv_expr == '\n' || *recv_expr == '\r') recv_expr++;
    len = strlen(recv_expr);
    while (len > 0 &&
           (recv_expr[len - 1] == ' ' || recv_expr[len - 1] == '\t' ||
            recv_expr[len - 1] == '\n' || recv_expr[len - 1] == '\r')) len--;
    if (len == 0 || len >= sizeof(expr)) return NULL;
    memcpy(expr, recv_expr, len);
    expr[len] = '\0';
    p = expr;
    if (*p == '&') {
        recv_is_ptr = 1;
        p++;
        while (*p == ' ' || *p == '\t') p++;
    }
    if (!(isalnum((unsigned char)*p) || *p == '_') || !(((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || *p == '_')))
        return NULL;
    {
        size_t rn = 0;
        while (p[rn] && (isalnum((unsigned char)p[rn]) || p[rn] == '_')) rn++;
        if (rn >= sizeof(root)) rn = sizeof(root) - 1;
        memcpy(root, p, rn);
        root[rn] = '\0';
        p += rn;
    }
    type_name = cc_type_registry_lookup_var(reg, root);
    if (!type_name) return NULL;
    cc__normalize_registry_type_name(reg, type_name, resolved_type, sizeof(resolved_type));
    if (!cc__walk_receiver_postfix(reg, &p, resolved_type, sizeof(resolved_type))) return NULL;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    /* Require full consumption so `rc == 0 ? …` is not typed as `rc`. */
    if (*p) return NULL;
    if (out_recv_is_ptr) *out_recv_is_ptr = recv_is_ptr || strchr(resolved_type, '*') != NULL;
    return resolved_type;
}

const char* cc_type_registry_resolve_receiver_expr_at(CCTypeRegistry* reg,
                                                      const char* recv_expr,
                                                      const char* source_text,
                                                      size_t use_offset,
                                                      int* out_recv_is_ptr) {
    static _Thread_local char resolved_type[256];
    char local_type[256];
    char expr[256];
    char root[128];
    const char* p;
    const char* type_name;
    int recv_is_ptr = 0;
    size_t len;
    if (out_recv_is_ptr) *out_recv_is_ptr = 0;
    if (!reg || !recv_expr) return NULL;
    while (*recv_expr == ' ' || *recv_expr == '\t' || *recv_expr == '\n' || *recv_expr == '\r') recv_expr++;
    len = strlen(recv_expr);
    while (len > 0 &&
           (recv_expr[len - 1] == ' ' || recv_expr[len - 1] == '\t' ||
            recv_expr[len - 1] == '\n' || recv_expr[len - 1] == '\r')) len--;
    if (len == 0 || len >= sizeof(expr)) return NULL;
    memcpy(expr, recv_expr, len);
    expr[len] = '\0';
    p = expr;
    if (*p == '&') {
        recv_is_ptr = 1;
        p++;
        while (*p == ' ' || *p == '\t') p++;
    }
    if (!(isalnum((unsigned char)*p) || *p == '_') || !(((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || *p == '_')))
        return NULL;
    {
        size_t rn = 0;
        while (p[rn] && (isalnum((unsigned char)p[rn]) || p[rn] == '_')) rn++;
        if (rn >= sizeof(root)) rn = sizeof(root) - 1;
        memcpy(root, p, rn);
        root[rn] = '\0';
        p += rn;
    }
    type_name = NULL;
    if (source_text) {
        type_name = cc__lookup_scoped_local_var_type(source_text, use_offset, root,
                                                     local_type, sizeof(local_type));
    }
    if (!type_name) type_name = cc_type_registry_lookup_var(reg, root);
    if (!type_name) return NULL;
    cc__normalize_registry_type_name(reg, type_name, resolved_type, sizeof(resolved_type));
    if (getenv("CC_DEBUG_RESOLVE_RECV"))
        fprintf(stderr, "resolve_recv_at: root='%s' decl='%s' normalized='%s'\n",
                root, type_name, resolved_type);
    if (!cc__walk_receiver_postfix(reg, &p, resolved_type, sizeof(resolved_type))) return NULL;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    if (*p) return NULL;
    if (out_recv_is_ptr) *out_recv_is_ptr = recv_is_ptr || strchr(resolved_type, '*') != NULL;
    return resolved_type;
}

const char* cc_type_registry_resolve_expr_type(CCTypeRegistry* reg, const char* expr) {
    static _Thread_local char resolved_type[256];
    const char* p = expr;
    size_t len = 0;
    int recv_is_ptr = 0;
    const char* resolved = NULL;
    if (!expr) return NULL;
    while (*p && isspace((unsigned char)*p)) p++;
    len = strlen(p);
    while (len > 0 && isspace((unsigned char)p[len - 1])) len--;
    if (len == 0 || len >= sizeof(resolved_type)) return NULL;

    /* Literals do not need a registry; keep these working even if the global
     * type graph has not been installed yet (e.g. pre-canonicalize create). */
    if (p[0] == '"' || (len > 1 && p[0] == 'L' && p[1] == '"')) return "const char*";
    if (p[0] == '\'') return "char";
    if ((len == 4 && memcmp(p, "true", 4) == 0) ||
        (len == 5 && memcmp(p, "false", 5) == 0)) {
        return "bool";
    }
    if (len == 4 && memcmp(p, "NULL", 4) == 0) return "void*";
    if (len >= 7 && memcmp(p, "sizeof(", 7) == 0) return "size_t";

    if ((isdigit((unsigned char)p[0])) ||
        ((p[0] == '+' || p[0] == '-') && len > 1 && isdigit((unsigned char)p[1]))) {
        /* Pure numeric literal only — reject ternaries / comparisons that
         * merely begin with a digit (`0 ? "OK" : "FAIL"`). */
        size_t i = (p[0] == '+' || p[0] == '-') ? 1 : 0;
        int saw_dot = 0;
        int saw_exp = 0;
        for (; i < len; ++i) {
            char c = p[i];
            if (isdigit((unsigned char)c)) continue;
            if (c == '.' && !saw_dot && !saw_exp) {
                saw_dot = 1;
                continue;
            }
            if ((c == 'e' || c == 'E') && !saw_exp) {
                saw_exp = 1;
                if (i + 1 < len && (p[i + 1] == '+' || p[i + 1] == '-')) i++;
                continue;
            }
            if (c == 'f' || c == 'F' || c == 'u' || c == 'U' || c == 'l' ||
                c == 'L')
                continue;
            return NULL;
        }
        return (saw_dot || saw_exp) ? "double" : "int";
    }

    if (!reg) return NULL;

    resolved = cc_type_registry_resolve_receiver_expr(reg, p, &recv_is_ptr);
    if (resolved && resolved[0]) return resolved;

    if (p[0] == '&') {
        const char* inner = cc_type_registry_resolve_expr_type(reg, p + 1);
        size_t inner_len = inner ? strlen(inner) : 0;
        if (!inner || inner_len == 0 || inner_len + 1 >= sizeof(resolved_type)) return NULL;
        memcpy(resolved_type, inner, inner_len);
        resolved_type[inner_len] = '*';
        resolved_type[inner_len + 1] = '\0';
        return resolved_type;
    }

    return NULL;
}

const char* cc_type_registry_lookup_channel_elem_type(CCTypeRegistry* reg, const char* handle_type_name) {
    const char* mangled = NULL;
    if (!reg || !handle_type_name) return NULL;

    if (strncmp(handle_type_name, "CCChanTx_", 9) == 0) {
        mangled = handle_type_name + 9;
    } else if (strncmp(handle_type_name, "CCChanRx_", 9) == 0) {
        mangled = handle_type_name + 9;
    } else {
        return NULL;
    }

    if (!*mangled) return NULL;
    for (size_t i = 0; i < reg->chan_count; i++) {
        if (strcmp(reg->channels[i].mangled_name, mangled) == 0) {
            return reg->channels[i].type1;
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

/* (retired) The CCOptional_* type registry used to live here. */

static int ensure_chan_capacity(CCTypeRegistry* reg, size_t needed) {
    if (reg->chan_capacity >= needed) return 0;
    size_t new_cap = reg->chan_capacity ? reg->chan_capacity * 2 : 8;
    while (new_cap < needed) new_cap *= 2;
    CCTypeInstEntry* nv = (CCTypeInstEntry*)realloc(reg->channels, new_cap * sizeof(CCTypeInstEntry));
    if (!nv) return -1;
    reg->channels = nv;
    reg->chan_capacity = new_cap;
    return 0;
}

int cc_type_registry_add_channel(CCTypeRegistry* reg, const char* elem_type, const char* mangled_name) {
    if (!reg || !elem_type || !mangled_name) return -1;

    for (size_t i = 0; i < reg->chan_count; i++) {
        if (strcmp(reg->channels[i].mangled_name, mangled_name) == 0) {
            return 0;
        }
    }

    if (ensure_chan_capacity(reg, reg->chan_count + 1) != 0) return -1;
    reg->channels[reg->chan_count].kind = CC_CONTAINER_CHANNEL;
    reg->channels[reg->chan_count].mangled_name = strdup(mangled_name);
    reg->channels[reg->chan_count].type1 = strdup(elem_type);
    reg->channels[reg->chan_count].type2 = NULL;
    if (!reg->channels[reg->chan_count].mangled_name || !reg->channels[reg->chan_count].type1) {
        free(reg->channels[reg->chan_count].mangled_name);
        free(reg->channels[reg->chan_count].type1);
        return -1;
    }
    reg->chan_count++;
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

size_t cc_type_registry_channel_count(CCTypeRegistry* reg) {
    return reg ? reg->chan_count : 0;
}

const CCTypeInstantiation* cc_type_registry_get_channel(CCTypeRegistry* reg, size_t idx) {
    if (!reg || idx >= reg->chan_count) return NULL;
    return (const CCTypeInstantiation*)&reg->channels[idx];
}
