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

#ifdef __cplusplus
}
#endif

#endif /* CC_UTIL_RESULT_FN_REGISTRY_H */
