#ifndef CC_RESULT_SPEC_H
#define CC_RESULT_SPEC_H

#include <stddef.h>

typedef struct {
    char ok_type[128];
    char err_type[128];
    char mangled_ok[128];
    char mangled_err[128];
    char concrete_name[256];
} CCResultSpec;

typedef struct {
    CCResultSpec* items;
    size_t count;
    size_t capacity;
} CCResultSpecTable;

void cc_result_spec_mangle_type(const char* src, size_t len, char* out, size_t out_sz);
void cc_result_spec_format_name(const char* mangled_ok,
                                const char* mangled_err,
                                char* out,
                                size_t out_sz);

int cc_result_spec_is_core_builtin(const char* mangled_ok, const char* mangled_err);
int cc_result_spec_is_stdlib_predeclared(const char* mangled_ok, const char* mangled_err);
int cc_result_spec_is_stdlib_predeclared_name(const char* concrete_name);

void cc_result_spec_table_init(CCResultSpecTable* table);
void cc_result_spec_table_reset(CCResultSpecTable* table);
void cc_result_spec_table_free(CCResultSpecTable* table);

const CCResultSpec* cc_result_spec_table_get(const CCResultSpecTable* table, size_t idx);
const CCResultSpec* cc_result_spec_table_find_by_name(const CCResultSpecTable* table,
                                                      const char* concrete_name);
const CCResultSpec* cc_result_spec_table_find_by_mangled(const CCResultSpecTable* table,
                                                         const char* mangled_ok,
                                                         const char* mangled_err);
const CCResultSpec* cc_result_spec_table_add(CCResultSpecTable* table,
                                             const char* ok_type, size_t ok_len,
                                             const char* err_type, size_t err_len,
                                             const char* mangled_ok,
                                             const char* mangled_err);

int cc_result_spec_emit_decl(const CCResultSpec* spec, char* out, size_t out_sz);

CCResultSpecTable* cc_result_spec_table_get_global(void);
void cc_result_spec_table_set_global(CCResultSpecTable* table);

#endif
