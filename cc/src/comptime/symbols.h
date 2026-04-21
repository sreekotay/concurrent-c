#ifndef CC_COMPTIME_SYMBOLS_H
#define CC_COMPTIME_SYMBOLS_H

#include <stddef.h>

// Minimal symbol table API for comptime constants/functions.
// This will grow into the real comptime environment; for now it
// supports predefined const bindings (e.g. future build.cc outputs).

typedef struct CCSymbolTable CCSymbolTable;
typedef void (*CCOwnedResourceFreeFn)(void*);
typedef int (*CCTypeRegistrationCreateCallback)(CCSymbolTable* t,
                                                const char* registration_input_path,
                                                const char* logical_file,
                                                const char* type_name,
                                                const char* expr_src,
                                                size_t expr_len,
                                                void* user_ctx);
typedef int (*CCTypeRegistrationUfcsCallback)(CCSymbolTable* t,
                                              const char* registration_input_path,
                                              const char* logical_file,
                                              const char* type_name,
                                              const char* expr_src,
                                              size_t expr_len,
                                              void* user_ctx);

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

/* Type hooks for declaration lifecycle and declarative UFCS lowering.
   Legacy `cc_ufcs_register(...)` compatibility is represented here as
   type-pattern UFCS callables too, so this remains the one authoritative UFCS
   registry. */
 int cc_symbols_add_type_create_call(CCSymbolTable* t, const char* type_name, int arity, const char* callee);
 int cc_symbols_lookup_type_create_call(CCSymbolTable* t, const char* type_name, int arity, const char** out_callee);
 int cc_symbols_set_type_create_callable(CCSymbolTable* t,
                                         const char* type_name,
                                         const void* fn_ptr,
                                         void* owner,
                                         CCOwnedResourceFreeFn owner_free);
 int cc_symbols_lookup_type_create_callable(CCSymbolTable* t,
                                            const char* type_name,
                                            const void** out_fn_ptr);
 int cc_symbols_set_type_pre_destroy_call(CCSymbolTable* t, const char* type_name, const char* callee);
 int cc_symbols_lookup_type_pre_destroy_call(CCSymbolTable* t, const char* type_name, const char** out_callee);
 int cc_symbols_set_type_destroy_call(CCSymbolTable* t, const char* type_name, const char* callee);
 int cc_symbols_lookup_type_destroy_call(CCSymbolTable* t, const char* type_name, const char** out_callee);
 int cc_symbols_add_type_ufcs_value(CCSymbolTable* t, const char* type_name, const char* method, const char* callee);
 int cc_symbols_lookup_type_ufcs_value(CCSymbolTable* t,
                                      const char* type_name,
                                      const char* method,
                                      const char** out_callee);
 int cc_symbols_set_type_ufcs_callable(CCSymbolTable* t,
                                      const char* type_name,
                                      const void* fn_ptr,
                                      void* owner,
                                      CCOwnedResourceFreeFn owner_free);
 int cc_symbols_add_legacy_type_ufcs_callable(CCSymbolTable* t,
                                             const char* type_name,
                                             const void* fn_ptr,
                                             void* owner,
                                             CCOwnedResourceFreeFn owner_free);
 int cc_symbols_lookup_type_ufcs_callable(CCSymbolTable* t,
                                         const char* type_name,
                                         const void** out_fn_ptr);
/* Same as cc_symbols_lookup_type_ufcs_callable, but also reports the
 * specificity of the matched pattern: the length of the literal prefix
 * before any trailing `*`.  A bare `*` wildcard scores 0; a type-name
 * pattern like `CC*` scores 2; an exact type name scores its full
 * length.  Callers use this to distinguish "user explicitly registered
 * a hook for this type" (score > 0) from "a global catch-all wildcard
 * caught this type by default" (score == 0), since the two cases imply
 * different authority over things like field-wins shadowing. */
 int cc_symbols_lookup_type_ufcs_callable_ex(CCSymbolTable* t,
                                             const char* type_name,
                                             const void** out_fn_ptr,
                                             size_t* out_score);
 size_t cc_symbols_type_count(CCSymbolTable* t);
 const char* cc_symbols_type_name(CCSymbolTable* t, size_t idx);
 int cc_symbols_collect_type_registrations(CCSymbolTable* t,
                                           const char* input_path,
                                           const char* src,
                                           size_t n);
 int cc_symbols_collect_type_registrations_ex(CCSymbolTable* t,
                                             const char* input_path,
                                             const char* src,
                                             size_t n,
                                             CCTypeRegistrationCreateCallback create_callback,
                                             void* create_user_ctx,
                                             CCTypeRegistrationUfcsCallback ufcs_callback,
                                             void* ufcs_user_ctx);

#endif // CC_COMPTIME_SYMBOLS_H

