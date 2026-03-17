/*
 * Type Registry Implementation
 *
 * Simple hash-less linear search for now; sufficient for typical file sizes.
 * Can be upgraded to hash tables if performance becomes an issue.
 */
#include "type_registry.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

/* Variable -> type entry */
typedef struct {
    char* var_name;
    char* type_name;
} CCVarTypeEntry;

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

int cc_type_registry_add_field(CCTypeRegistry* reg,
                               const char* struct_name,
                               const char* field_name,
                               const char* field_type) {
    if (!reg || !struct_name || !field_name || !field_type) return -1;
    for (size_t i = 0; i < reg->field_count; i++) {
        if (strcmp(reg->fields[i].struct_name, struct_name) == 0 &&
            strcmp(reg->fields[i].field_name, field_name) == 0) {
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
    strncpy(resolved_type, type_name, sizeof(resolved_type) - 1);
    resolved_type[sizeof(resolved_type) - 1] = '\0';
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
        strncpy(resolved_type, type_name, sizeof(resolved_type) - 1);
        resolved_type[sizeof(resolved_type) - 1] = '\0';
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
