#ifndef CC_COMPTIME_SYMBOLS_H
#define CC_COMPTIME_SYMBOLS_H

#include <stddef.h>

// Minimal symbol table API for comptime constants/functions.
// This will grow into the real comptime environment; for now it
// supports predefined const bindings (e.g. future build.cc outputs).

typedef struct CCSymbolTable CCSymbolTable;
typedef void (*CCOwnedResourceFreeFn)(void*);

// Simple name/value binding used to preload consts from the driver.
typedef struct {
    const char* name;
    long long value;
} CCConstBinding;

CCSymbolTable* cc_symbols_new(void);
void cc_symbols_free(CCSymbolTable* t);

// Add a const integer value (last writer wins on duplicate names).
int cc_symbols_add_const(CCSymbolTable* t, const char* name, long long value);

// Bulk-add predefined consts; convenience for driver/build integration.
int cc_symbols_add_predefined(CCSymbolTable* t, const CCConstBinding* bindings, size_t count);

// Lookup const; returns 0 on success, non-zero on miss.
int cc_symbols_lookup_const(CCSymbolTable* t, const char* name, long long* out_value);

/* Function decl attributes (parsed from CC decl annotations like @async/@noblock). */
int cc_symbols_set_fn_attrs(CCSymbolTable* t, const char* name, unsigned int attrs);
int cc_symbols_lookup_fn_attrs(CCSymbolTable* t, const char* name, unsigned int* out_attrs);

/* Minimal compile-time UFCS registry (exact-match or simple prefix* patterns).
   Callable entries back real comptime execution for named handlers and
   non-capturing lambdas through the active comptime backend. Prefix entries
   remain as a compatibility fallback for older lowering paths. */
int cc_symbols_add_ufcs_prefix(CCSymbolTable* t, const char* pattern, const char* prefix);
int cc_symbols_lookup_ufcs_prefix(CCSymbolTable* t, const char* pattern, const char** out_prefix);
int cc_symbols_add_ufcs_callable(CCSymbolTable* t,
                                 const char* pattern,
                                 const void* fn_ptr,
                                 void* owner,
                                 CCOwnedResourceFreeFn owner_free);
int cc_symbols_lookup_ufcs_callable(CCSymbolTable* t, const char* pattern, const void** out_fn_ptr);
size_t cc_symbols_ufcs_count(CCSymbolTable* t);
const char* cc_symbols_ufcs_pattern(CCSymbolTable* t, size_t idx);

 /* Exact-type hooks for declaration lifecycle and declarative UFCS lowering. */
 int cc_symbols_add_type_create_call(CCSymbolTable* t, const char* type_name, int arity, const char* callee);
 int cc_symbols_lookup_type_create_call(CCSymbolTable* t, const char* type_name, int arity, const char** out_callee);
 int cc_symbols_set_type_destroy_call(CCSymbolTable* t, const char* type_name, const char* callee);
 int cc_symbols_lookup_type_destroy_call(CCSymbolTable* t, const char* type_name, const char** out_callee);
 int cc_symbols_add_type_ufcs_value(CCSymbolTable* t, const char* type_name, const char* method, const char* callee);
 int cc_symbols_lookup_type_ufcs_value(CCSymbolTable* t,
                                      const char* type_name,
                                      const char* method,
                                      const char** out_callee);
 int cc_symbols_collect_type_registrations(CCSymbolTable* t,
                                           const char* input_path,
                                           const char* src,
                                           size_t n);

#endif // CC_COMPTIME_SYMBOLS_H

