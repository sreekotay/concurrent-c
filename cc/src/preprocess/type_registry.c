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

    /* Optional instantiations */
    CCTypeInstEntry* optionals;
    size_t opt_count;
    size_t opt_capacity;

    /* Channel typed-wrapper instantiations */
    CCTypeInstEntry* channels;
    size_t chan_count;
    size_t chan_capacity;
};

/* Thread-local global registry */
static _Thread_local CCTypeRegistry* g_type_registry = NULL;

static void cc__normalize_local_decl_type(char* out, size_t out_sz, const char* type_name);

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
    free_inst_entries(reg->optionals, reg->opt_count);
    free(reg->optionals);
    free_inst_entries(reg->channels, reg->chan_count);
    free(reg->channels);
    free(reg);
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
    free_inst_entries(reg->optionals, reg->opt_count);
    reg->opt_count = 0;
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

int cc_type_registry_add_field(CCTypeRegistry* reg,
                               const char* struct_name,
                               const char* field_name,
                               const char* field_type) {
    if (!reg || !struct_name || !field_name || !field_type) return -1;
    for (size_t i = 0; i < reg->field_count; i++) {
        if (strcmp(reg->fields[i].struct_name, struct_name) == 0 &&
            strcmp(reg->fields[i].field_name, field_name) == 0) {
            if (!cc__is_parser_placeholder_type(reg->fields[i].field_type) &&
                cc__is_parser_placeholder_type(field_type)) {
                return 0;
            }
            if (!cc__is_parser_placeholder_type(reg->fields[i].field_type) &&
                strcmp(reg->fields[i].field_type, field_type) != 0) {
                return 0;
            }
            free(reg->fields[i].field_type);
            reg->fields[i].field_type = strdup(field_type);
            return reg->fields[i].field_type ? 0 : -1;
        }
    }
    if (ensure_field_capacity(reg, reg->field_count + 1) != 0) return -1;
    reg->fields[reg->field_count].struct_name = strdup(struct_name);
    reg->fields[reg->field_count].field_name = strdup(field_name);
    reg->fields[reg->field_count].field_type = strdup(field_type);
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

static void cc__copy_type_base(char* out, size_t out_sz, const char* type_name) {
    size_t len = 0;
    if (!out || out_sz == 0) return;
    out[0] = '\0';
    if (!type_name) return;
    len = strlen(type_name);
    while (len > 0 && (type_name[len - 1] == ' ' || type_name[len - 1] == '\t')) len--;
    while (len > 0 && type_name[len - 1] == '*') len--;
    while (len > 0 && (type_name[len - 1] == ' ' || type_name[len - 1] == '\t')) len--;
    if (len >= out_sz) len = out_sz - 1;
    memcpy(out, type_name, len);
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

    if ((strncmp(work, "Vec::[", 6) == 0 && work[strlen(work) - 1] == ']') ||
        (strncmp(work, "Vec<", 4) == 0 && work[strlen(work) - 1] == '>') ||
        (strncmp(work, "CCVec::[", 8) == 0 && work[strlen(work) - 1] == ']') ||
        (strncmp(work, "CCVec<", 6) == 0 && work[strlen(work) - 1] == '>') ||
        (strncmp(work, "__CC_VEC(", 9) == 0 && work[strlen(work) - 1] == ')')) {
        size_t prefix = (strncmp(work, "__CC_VEC(", 9) == 0) ? 9 :
                        ((strncmp(work, "CCVec::[", 8) == 0) ? 8 :
                         ((strncmp(work, "CCVec<", 6) == 0) ? 6 :
                          ((work[3] == ':') ? 6 : 4)));
        size_t inner_len = strlen(work) - prefix - 1;
        if (inner_len >= sizeof(inner)) inner_len = sizeof(inner) - 1;
        memcpy(inner, work + prefix, inner_len);
        inner[inner_len] = '\0';
        cc__normalize_registry_container_param_name(reg, inner, inner, sizeof(inner));
        cc_result_spec_mangle_type(inner, strlen(inner), mangled_inner, sizeof(mangled_inner));
        if (mangled_inner[0]) snprintf(work, sizeof(work), "CCVec_%s", mangled_inner);
    } else if ((strncmp(work, "Map::[", 6) == 0 && work[strlen(work) - 1] == ']') ||
               (strncmp(work, "Map<", 4) == 0 && work[strlen(work) - 1] == '>') ||
               (strncmp(work, "__CC_MAP(", 9) == 0 && work[strlen(work) - 1] == ')')) {
        const char* params = work + ((strncmp(work, "__CC_MAP(", 9) == 0) ? 9 : ((work[3] == ':') ? 6 : 4));
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
                snprintf(work, sizeof(work), "Map_%s_%s", mangled_key, mangled_val);
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
    }

    if ((strncmp(work, "Vec::[", 6) == 0 && work[strlen(work) - 1] == ']') ||
        (strncmp(work, "Vec<", 4) == 0 && work[strlen(work) - 1] == '>') ||
        (strncmp(work, "CCVec::[", 8) == 0 && work[strlen(work) - 1] == ']') ||
        (strncmp(work, "CCVec<", 6) == 0 && work[strlen(work) - 1] == '>') ||
        (strncmp(work, "__CC_VEC(", 9) == 0 && work[strlen(work) - 1] == ')')) {
        size_t prefix = (strncmp(work, "__CC_VEC(", 9) == 0) ? 9 :
                        ((strncmp(work, "CCVec::[", 8) == 0) ? 8 :
                         ((strncmp(work, "CCVec<", 6) == 0) ? 6 :
                          ((work[3] == ':') ? 6 : 4)));
        size_t inner_len = strlen(work) - prefix - 1;
        if (inner_len >= sizeof(inner)) inner_len = sizeof(inner) - 1;
        memcpy(inner, work + prefix, inner_len);
        inner[inner_len] = '\0';
        cc__normalize_registry_container_param_name(reg, inner, inner, sizeof(inner));
        cc_result_spec_mangle_type(inner, strlen(inner), mangled_inner, sizeof(mangled_inner));
        if (mangled_inner[0]) snprintf(work, sizeof(work), "CCVec_%s", mangled_inner);
    } else if ((strncmp(work, "Map::[", 6) == 0 && work[strlen(work) - 1] == ']') ||
               (strncmp(work, "Map<", 4) == 0 && work[strlen(work) - 1] == '>') ||
               (strncmp(work, "__CC_MAP(", 9) == 0 && work[strlen(work) - 1] == ')')) {
        const char* params = work + ((strncmp(work, "__CC_MAP(", 9) == 0) ? 9 : ((work[3] == ':') ? 6 : 4));
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
                snprintf(work, sizeof(work), "Map_%s_%s", mangled_key, mangled_val);
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

static const char* cc__lookup_scoped_local_var_type(const char* src,
                                                    size_t limit,
                                                    const char* var_name,
                                                    char* out_type,
                                                    size_t out_type_sz) {
    typedef struct {
        int scope_id;
        char type_name[256];
    } LocalDecl;
    enum { MAX_DECLS = 256, MAX_SCOPES = 256 };
    LocalDecl decls[MAX_DECLS];
    int decl_count = 0;
    int scope_stack[MAX_SCOPES];
    int scope_depth = 1;
    int next_scope_id = 1;
    int in_lc = 0, in_bc = 0, in_str = 0, in_chr = 0;
    size_t stmt_start = 0;
    size_t i = 0;
    if (!src || !var_name || !var_name[0] || !out_type || out_type_sz == 0) return NULL;
    out_type[0] = '\0';
    scope_stack[0] = 0;
    while (i < limit) {
        char c = src[i];
        char c2 = (i + 1 < limit) ? src[i + 1] : 0;
        if (in_lc) { if (c == '\n') in_lc = 0; i++; continue; }
        if (in_bc) { if (c == '*' && c2 == '/') { in_bc = 0; i += 2; continue; } i++; continue; }
        if (in_str) { if (c == '\\' && i + 1 < limit) { i += 2; continue; } if (c == '"') in_str = 0; i++; continue; }
        if (in_chr) { if (c == '\\' && i + 1 < limit) { i += 2; continue; } if (c == '\'') in_chr = 0; i++; continue; }
        if (c == '/' && c2 == '/') { in_lc = 1; i += 2; continue; }
        if (c == '/' && c2 == '*') { in_bc = 1; i += 2; continue; }
        if (c == '"') { in_str = 1; i++; continue; }
        if (c == '\'') { in_chr = 1; i++; continue; }
        if (c == '{') {
            if (scope_depth < MAX_SCOPES) scope_stack[scope_depth++] = next_scope_id++;
            stmt_start = i + 1;
            i++;
            continue;
        }
        if (c == '}') {
            int closing_scope = (scope_depth > 1) ? scope_stack[scope_depth - 1] : 0;
            while (decl_count > 0 && decls[decl_count - 1].scope_id == closing_scope) decl_count--;
            if (scope_depth > 1) scope_depth--;
            stmt_start = i + 1;
            i++;
            continue;
        }
        if (c == ';') {
            char decl_name[128];
            char decl_type[256];
            cc_parse_decl_name_and_type(src + stmt_start, src + i,
                                               decl_name, sizeof(decl_name),
                                               decl_type, sizeof(decl_type));
            if (decl_name[0] &&
                strcmp(decl_name, var_name) == 0 &&
                !cc_is_non_decl_stmt_type(decl_type) &&
                decl_count < MAX_DECLS) {
                decls[decl_count].scope_id = scope_stack[scope_depth - 1];
                cc__normalize_local_decl_type(decls[decl_count].type_name,
                                              sizeof(decls[decl_count].type_name),
                                              decl_type);
                decl_count++;
            }
            stmt_start = i + 1;
        }
        i++;
    }
    if (decl_count == 0) return NULL;
    strncpy(out_type, decls[decl_count - 1].type_name, out_type_sz - 1);
    out_type[out_type_sz - 1] = '\0';
    return out_type;
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
    while (*p) {
        char field_name[128];
        char base_type[256];
        size_t fn = 0;
        int is_arrow = 0;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '.') {
            p++;
        } else if (*p == '-' && p[1] == '>') {
            is_arrow = 1;
            p += 2;
        } else {
            break;
        }
        while (*p == ' ' || *p == '\t') p++;
        if (!(isalnum((unsigned char)*p) || *p == '_') || !(((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || *p == '_')))
            return NULL;
        while (*p && (isalnum((unsigned char)*p) || *p == '_')) {
            if (fn + 1 < sizeof(field_name)) field_name[fn] = *p;
            fn++;
            p++;
        }
        field_name[fn < sizeof(field_name) ? fn : sizeof(field_name) - 1] = '\0';
        cc__copy_type_base(base_type, sizeof(base_type), resolved_type);
        if (is_arrow && base_type[0] == '\0') return NULL;
        type_name = cc_type_registry_lookup_field(reg, base_type, field_name);
        if (!type_name) return NULL;
        cc__normalize_registry_type_name(reg, type_name, resolved_type, sizeof(resolved_type));
    }
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
    while (*p) {
        char field_name[128];
        char base_type[256];
        size_t fn = 0;
        int is_arrow = 0;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '.') {
            p++;
        } else if (*p == '-' && p[1] == '>') {
            is_arrow = 1;
            p += 2;
        } else {
            break;
        }
        while (*p == ' ' || *p == '\t') p++;
        if (!(isalnum((unsigned char)*p) || *p == '_') || !(((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || *p == '_')))
            return NULL;
        while (*p && (isalnum((unsigned char)*p) || *p == '_')) {
            if (fn + 1 < sizeof(field_name)) field_name[fn] = *p;
            fn++;
            p++;
        }
        field_name[fn < sizeof(field_name) ? fn : sizeof(field_name) - 1] = '\0';
        cc__copy_type_base(base_type, sizeof(base_type), resolved_type);
        if (is_arrow && base_type[0] == '\0') return NULL;
        type_name = cc_type_registry_lookup_field(reg, base_type, field_name);
        if (!type_name) return NULL;
        cc__normalize_registry_type_name(reg, type_name, resolved_type, sizeof(resolved_type));
    }
    if (out_recv_is_ptr) *out_recv_is_ptr = recv_is_ptr || strchr(resolved_type, '*') != NULL;
    return resolved_type;
}

const char* cc_type_registry_resolve_expr_type(CCTypeRegistry* reg, const char* expr) {
    static _Thread_local char resolved_type[256];
    const char* p = expr;
    size_t len = 0;
    int recv_is_ptr = 0;
    const char* resolved = NULL;
    if (!reg || !expr) return NULL;
    while (*p && isspace((unsigned char)*p)) p++;
    len = strlen(p);
    while (len > 0 && isspace((unsigned char)p[len - 1])) len--;
    if (len == 0 || len >= sizeof(resolved_type)) return NULL;

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
        int saw_dot = 0;
        int saw_exp = 0;
        for (size_t i = 0; i < len; ++i) {
            char c = p[i];
            if (c == '.') saw_dot = 1;
            else if (c == 'e' || c == 'E') saw_exp = 1;
        }
        return (saw_dot || saw_exp) ? "double" : "int";
    }

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

size_t cc_type_registry_optional_count(CCTypeRegistry* reg) {
    return reg ? reg->opt_count : 0;
}

const CCTypeInstantiation* cc_type_registry_get_optional(CCTypeRegistry* reg, size_t idx) {
    if (!reg || idx >= reg->opt_count) return NULL;
    return (const CCTypeInstantiation*)&reg->optionals[idx];
}

size_t cc_type_registry_channel_count(CCTypeRegistry* reg) {
    return reg ? reg->chan_count : 0;
}

const CCTypeInstantiation* cc_type_registry_get_channel(CCTypeRegistry* reg, size_t idx) {
    if (!reg || idx >= reg->chan_count) return NULL;
    return (const CCTypeInstantiation*)&reg->channels[idx];
}
