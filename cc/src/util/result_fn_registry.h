/*
 * Process-level registry of function names whose declared return type is
 * a result type (`T !> (E)` / `CCResult_T_E`).  Populated by the result-type
 * preprocess pass and by scanning included headers; consumed by
 * pass_result_unwrap.c for typed `!>` / `?>` lowering and the slice-7
 * unhandled-result diagnostic.
 *
 * The registry is a simple additive set.  We do not reset between
 * translation units because `ccc` runs as a fresh process per file in
 * practice, so state does not leak across user-visible builds.
 */
#ifndef CC_UTIL_RESULT_FN_REGISTRY_H
#define CC_UTIL_RESULT_FN_REGISTRY_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void cc_result_fn_registry_add(const char* name, size_t len);
int  cc_result_fn_registry_contains(const char* name, size_t len);
void cc_result_fn_registry_clear(void);

/* Optional per-fn metadata.
 *
 * For each result-returning function recognized by the type-syntax pass or
 * by `cc_result_fn_registry_scan_source`, record:
 *   - the *textual* error type `E` (e.g. "CCIoError") for typed binders
 *   - the concrete result type name (e.g. "CCResult_CCSlice_CCError") for
 *     expression-position `!>;` lowering that cannot see header decls
 *
 * Either metadata pointer may be NULL / len 0 when unknown.  Later calls
 * fill missing fields but do not overwrite an already-recorded value.
 *
 * `err_type` should be the unmangled C type name.  Getters return 0 if the
 * fn has no recorded value (callers fall back to `__typeof__` / source scan). */
void cc_result_fn_registry_add_typed(const char* name, size_t name_len,
                                      const char* err_type, size_t err_len,
                                      const char* result_type, size_t result_len);
int  cc_result_fn_registry_get_err_type(const char* name, size_t name_len,
                                         char* out_buf, size_t out_sz);
int  cc_result_fn_registry_get_result_type(const char* name, size_t name_len,
                                            char* out_buf, size_t out_sz);

/* Scan `src[0..n)` for result-returning function declarators and register
 * them.  Recognizes:
 *   - `CCResult_<Ok>_<Err> name(`
 *   - `Ok !>(Err) name(`  (surface sugar; mangles to CCResult_Ok_Err)
 * Safe to call on header bodies that are not spliced into the TU buffer. */
void cc_result_fn_registry_scan_source(const char* src, size_t n);

/* Enumerate scanned result-fn rows (name / textual E / CCResult_T_E). */
const char* cc_result_fn_registry_name_at(size_t i);
const char* cc_result_fn_registry_err_type_at(size_t i);
const char* cc_result_fn_registry_result_type_at(size_t i);
size_t cc_result_fn_registry_count(void);

#ifdef __cplusplus
}
#endif

#endif /* CC_UTIL_RESULT_FN_REGISTRY_H */
