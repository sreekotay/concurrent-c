#include "result_spec.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static _Thread_local CCResultSpecTable* g_result_spec_table = NULL;

static const struct {
    const char* short_name;
    const char* cc_name;
} cc__type_aliases[] = {
    { "IoError", "CCIoError" },
    { "IoErrorKind", "CCIoErrorKind" },
    { "Error", "CCError" },
    { "ErrorKind", "CCErrorKind" },
    { "NetError", "CCNetError" },
    { "Arena", "CCArena" },
    { "File", "CCFile" },
    { "String", "CCString" },
    { "Slice", "CCSlice" },
    { NULL, NULL }
};

static const char* cc__core_builtin_result_types[] = {
    "int_CCError",
    "bool_CCError",
    "size_t_CCError",
    "voidptr_CCError",
    "charptr_CCError",
    "void_CCError",
    NULL
};

static const char* cc__stdlib_predeclared_result_types[] = {
    "CCResult_CCSlice_CCIoError",
    "CCResult_size_t_CCIoError",
    "CCResult_CCOptional_CCSlice_CCIoError",
    "CCResult_CCDirIterptr_CCIoError",
    "CCResult_CCDirEntry_CCIoError",
    "CCResult_bool_CCIoError",
    "CCResult_int64_t_CC_I64ParseError",
    "CCResult_uint64_t_CC_U64ParseError",
    "CCResult_double_CC_F64ParseError",
    "CCResult_bool_CC_BoolParseError",
    NULL
};

static void cc__normalize_type_name(char* name) {
    int i;
    if (!name || !name[0]) return;
    for (i = 0; cc__type_aliases[i].short_name; i++) {
        if (strcmp(name, cc__type_aliases[i].short_name) == 0) {
            snprintf(name, 128, "%s", cc__type_aliases[i].cc_name);
            return;
        }
    }
}

void cc_result_spec_mangle_type(const char* src, size_t len, char* out, size_t out_sz) {
    size_t i;
    size_t j = 0;
    if (!src || len == 0 || !out || out_sz == 0) {
        if (out && out_sz > 0) out[0] = '\0';
        return;
    }

    while (len > 0 && (*src == ' ' || *src == '\t' || *src == '\n' || *src == '\r')) {
        src++;
        len--;
    }
    while (len > 0 &&
           (src[len - 1] == ' ' || src[len - 1] == '\t' ||
            src[len - 1] == '\n' || src[len - 1] == '\r')) {
        len--;
    }

    for (i = 0; i < len && j < out_sz - 1; i++) {
        char c = src[i];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            if (j > 0 && out[j - 1] != '_') out[j++] = '_';
        } else if (c == '*') {
            if (j + 3 < out_sz - 1) {
                out[j++] = 'p';
                out[j++] = 't';
                out[j++] = 'r';
            }
        } else if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                   (c >= '0' && c <= '9') || c == '_') {
            out[j++] = c;
        } else if (c == '[' || c == ']' || c == '<' || c == '>' || c == ',' ||
                   c == '(' || c == ')' || c == '!' || c == ':' || c == '?') {
            if (j > 0 && out[j - 1] != '_') out[j++] = '_';
        } else {
            if (j > 0 && out[j - 1] != '_') out[j++] = '_';
        }
    }
    while (j > 0 && out[j - 1] == '_') j--;
    out[j] = '\0';
    cc__normalize_type_name(out);
}

void cc_result_spec_format_name(const char* mangled_ok,
                                const char* mangled_err,
                                char* out,
                                size_t out_sz) {
    if (!out || out_sz == 0) return;
    snprintf(out, out_sz, "CCResult_%s_%s",
             mangled_ok ? mangled_ok : "",
             mangled_err ? mangled_err : "");
}

static int cc__result_key_matches(const char* const* list,
                                  const char* mangled_ok,
                                  const char* mangled_err,
                                  int prefixed) {
    char key[256];
    int i;
    if (!mangled_ok || !mangled_err) return 0;
    if (prefixed) cc_result_spec_format_name(mangled_ok, mangled_err, key, sizeof(key));
    else snprintf(key, sizeof(key), "%s_%s", mangled_ok, mangled_err);
    for (i = 0; list[i]; i++) {
        if (strcmp(list[i], key) == 0) return 1;
    }
    return 0;
}

int cc_result_spec_is_core_builtin(const char* mangled_ok, const char* mangled_err) {
    return cc__result_key_matches(cc__core_builtin_result_types, mangled_ok, mangled_err, 0);
}

int cc_result_spec_is_stdlib_predeclared(const char* mangled_ok, const char* mangled_err) {
    return cc__result_key_matches(cc__stdlib_predeclared_result_types, mangled_ok, mangled_err, 1);
}

int cc_result_spec_is_stdlib_predeclared_name(const char* concrete_name) {
    int i;
    if (!concrete_name) return 0;
    for (i = 0; cc__stdlib_predeclared_result_types[i]; i++) {
        if (strcmp(cc__stdlib_predeclared_result_types[i], concrete_name) == 0) return 1;
    }
    return 0;
}

void cc_result_spec_table_init(CCResultSpecTable* table) {
    if (!table) return;
    table->items = NULL;
    table->count = 0;
    table->capacity = 0;
}

void cc_result_spec_table_reset(CCResultSpecTable* table) {
    if (!table) return;
    table->count = 0;
}

void cc_result_spec_table_free(CCResultSpecTable* table) {
    if (!table) return;
    free(table->items);
    table->items = NULL;
    table->count = 0;
    table->capacity = 0;
}

const CCResultSpec* cc_result_spec_table_get(const CCResultSpecTable* table, size_t idx) {
    if (!table || idx >= table->count) return NULL;
    return &table->items[idx];
}

const CCResultSpec* cc_result_spec_table_find_by_name(const CCResultSpecTable* table,
                                                      const char* concrete_name) {
    size_t i;
    if (!table || !concrete_name) return NULL;
    for (i = 0; i < table->count; i++) {
        if (strcmp(table->items[i].concrete_name, concrete_name) == 0) return &table->items[i];
    }
    return NULL;
}

const CCResultSpec* cc_result_spec_table_find_by_mangled(const CCResultSpecTable* table,
                                                         const char* mangled_ok,
                                                         const char* mangled_err) {
    size_t i;
    if (!table || !mangled_ok || !mangled_err) return NULL;
    for (i = 0; i < table->count; i++) {
        if (strcmp(table->items[i].mangled_ok, mangled_ok) == 0 &&
            strcmp(table->items[i].mangled_err, mangled_err) == 0) {
            return &table->items[i];
        }
    }
    return NULL;
}

const CCResultSpec* cc_result_spec_table_add(CCResultSpecTable* table,
                                             const char* ok_type, size_t ok_len,
                                             const char* err_type, size_t err_len,
                                             const char* mangled_ok,
                                             const char* mangled_err) {
    CCResultSpec* spec;
    CCResultSpec* new_items;
    size_t new_cap;
    if (!table || !ok_type || !err_type || !mangled_ok || !mangled_err) return NULL;

    spec = (CCResultSpec*)cc_result_spec_table_find_by_mangled(table, mangled_ok, mangled_err);
    if (spec) return spec;

    if (table->count >= table->capacity) {
        new_cap = table->capacity ? table->capacity * 2 : 16;
        new_items = (CCResultSpec*)realloc(table->items, new_cap * sizeof(CCResultSpec));
        if (!new_items) return NULL;
        table->items = new_items;
        table->capacity = new_cap;
    }

    spec = &table->items[table->count++];
    if (ok_len >= sizeof(spec->ok_type)) ok_len = sizeof(spec->ok_type) - 1;
    if (err_len >= sizeof(spec->err_type)) err_len = sizeof(spec->err_type) - 1;
    memcpy(spec->ok_type, ok_type, ok_len);
    spec->ok_type[ok_len] = '\0';
    memcpy(spec->err_type, err_type, err_len);
    spec->err_type[err_len] = '\0';
    snprintf(spec->mangled_ok, sizeof(spec->mangled_ok), "%s", mangled_ok);
    snprintf(spec->mangled_err, sizeof(spec->mangled_err), "%s", mangled_err);
    cc_result_spec_format_name(mangled_ok, mangled_err,
                               spec->concrete_name, sizeof(spec->concrete_name));
    return spec;
}

int cc_result_spec_emit_decl(const CCResultSpec* spec, char* out, size_t out_sz) {
    if (!spec || !out || out_sz == 0) return -1;
    return snprintf(out, out_sz,
                    "#ifndef %s_DEFINED\n"
                    "#define %s_DEFINED 1\n"
                    "CC_DECL_RESULT_SPEC(%s, %s, %s)\n"
                    "#endif\n",
                    spec->concrete_name,
                    spec->concrete_name,
                    spec->concrete_name,
                    spec->ok_type,
                    spec->err_type);
}

CCResultSpecTable* cc_result_spec_table_get_global(void) {
    return g_result_spec_table;
}

void cc_result_spec_table_set_global(CCResultSpecTable* table) {
    g_result_spec_table = table;
}
