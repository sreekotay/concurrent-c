/* pass_type_syntax.h - Type syntax lowering passes.
 *
 * Handles:
 *   - T[:]  -> CCSlice_T (slice types)
 *   - T?    -> CCOptional_T (optional types)
 *   - T!E   -> CCResult_T_E (result types)
 *   - cc_ok(v) / cc_err(e) -> typed constructors (inferred result constructors)
 *   - try expr -> cc_try(expr) (try expressions)
 *   - *opt_var -> unwrap (optional unwrap)
 *
 * Extracted from visit_codegen.c for maintainability.
 */

#ifndef CC_PASS_TYPE_SYNTAX_H
#define CC_PASS_TYPE_SYNTAX_H

#include <stddef.h>
#include "result_spec.h"
#include "visitor/visitor.h"

/* Rewrite slice type syntax `T[:]` to `CCSlice_T`.
 * Returns newly allocated string, or NULL if no rewrites. */
char* cc__rewrite_slice_types_text(const CCVisitorCtx* ctx, const char* src, size_t n);

/* Rewrite optional type syntax `T?` to `CCOptional_T`.
 * Returns newly allocated string, or NULL if no rewrites. */
char* cc__rewrite_optional_types_text(const CCVisitorCtx* ctx, const char* src, size_t n);

/* Rewrite result type syntax `T!>(E)` to `CCResult_T_E`.
 * Returns newly allocated string, or NULL if no rewrites. */
char* cc__rewrite_result_types_text(const CCVisitorCtx* ctx, const char* src, size_t n);

/* Rewrite result field sugar:
 *   res.value -> res.u.value
 *   res.error -> res.u.error
 * Applies only when `res` is declared with a `CCResult_*` type in the same unit.
 * Returns newly allocated string, or NULL if no rewrites. */
char* cc__rewrite_result_field_sugar_text(const CCVisitorCtx* ctx, const char* src, size_t n);

/* Rewrite inferred result constructors `cc_ok(v)` / `cc_err(e)` to typed versions.
 * Returns newly allocated string, or NULL if no rewrites. */
char* cc__rewrite_inferred_result_constructors(const char* src, size_t n);

/* Rewrite try expressions `try expr` to `cc_try(expr)`.
 * Returns newly allocated string, or NULL if no rewrites. */
char* cc__rewrite_try_exprs_text(const CCVisitorCtx* ctx, const char* src, size_t n);

/* Rewrite optional unwrap `*opt_var` to unwrap call.
 * Returns newly allocated string, or NULL if no rewrites. */
char* cc__rewrite_optional_unwrap_text(const CCVisitorCtx* ctx, const char* src, size_t n);

/* Result type registry - used by codegen to emit CC_DECL_RESULT_SPEC declarations */
extern CCResultSpecTable cc__cg_result_specs;

/* Optional type registry - used by codegen to emit CC_DECL_OPTIONAL declarations */
typedef struct {
    char mangled_type[128];  /* e.g., "Point" */
    char raw_type[128];      /* e.g., "Point" (original type name) */
} CCCodegenOptionalType;

/* Dynamic array - grows via realloc; count/cap managed in pass_type_syntax.c */
extern CCCodegenOptionalType* cc__cg_optional_types;
extern size_t cc__cg_optional_type_count;
extern size_t cc__cg_optional_type_cap;

/* Reset both type registries to empty (retain allocated buffer capacity).
 * Call once at the start of each compilation unit in visit_codegen.c rather
 * than relying on the implicit reset inside each scan function. */
void cc__cg_reset_type_registries(void);

#endif /* CC_PASS_TYPE_SYNTAX_H */
