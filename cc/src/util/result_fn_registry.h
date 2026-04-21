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

/* Optional per-fn error-type metadata.
 *
 * For each result-returning function `T !> (E) NAME(...)` recognized by the
 * type-syntax pass, record the *textual* error type `E` alongside the name.
 * Consumed by `pass_result_unwrap.c` when expanding `!>(e)` / `?>(e)`
 * binders: we emit `ErrType e = *(ErrType*)(void*)&(tmp.u.error);` so the
 * binder's type is the declared error type rather than the parser-mode
 * generic `__CCGenericError`.
 *
 * `err_type` should be the unmangled C type name (e.g. "CCIoError"), not
 * the mangled form.  Returns the stored length via `*out_len` on success;
 * `out_buf` receives a NUL-terminated copy.  Returns 0 if the fn has no
 * recorded err type (callers should fall back to the `__typeof__` path). */
void cc_result_fn_registry_add_typed(const char* name, size_t name_len,
                                      const char* err_type, size_t err_len);
int  cc_result_fn_registry_get_err_type(const char* name, size_t name_len,
                                         char* out_buf, size_t out_sz);

#ifdef __cplusplus
}
#endif

#endif /* CC_UTIL_RESULT_FN_REGISTRY_H */
