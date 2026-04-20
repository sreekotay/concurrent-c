/*
 * Process-level registry of function names whose declared return type is
 * a result type (`T !> (E)`).  Populated by the result-type preprocess
 * pass; consumed by the slice-7 unhandled-result diagnostic in
 * pass_result_unwrap.c.
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

/* Parallel registry of function names whose declared return type is a
 * plain pointer (e.g. `T* foo(...)`).  Consumed by the `!>` / `?>`
 * lowering to emit a NULL-check-based unwrap that synthesizes a
 * CCError{ .kind = CC_ERR_NULL } on NULL return.  Populated by a
 * per-rewrite-invocation scan in pass_result_unwrap.c. */
void cc_pointer_fn_registry_add(const char* name, size_t len);
int  cc_pointer_fn_registry_contains(const char* name, size_t len);
void cc_pointer_fn_registry_clear(void);

/* Scan `s[0..n)` for function declarations whose declared return type
 * contains a `*` (i.e. raw pointer-returning functions like
 * `FILE* fopen(const char*, const char*);` or
 * `CCNursery* cc_nursery_create(void);`) and register each such NAME
 * into the pointer-fn set.  Heuristic, comment/string-aware.  Does NOT
 * clear the set first; intended to be called once per TU plus once per
 * include-expanded stream so both local and library declarations land
 * in the registry. */
void cc_pointer_fn_registry_scan(const char* s, size_t n);

#ifdef __cplusplus
}
#endif

#endif /* CC_UTIL_RESULT_FN_REGISTRY_H */
