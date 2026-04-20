/* pass_type_syntax.h - Type syntax lowering passes.
 *
 * Handles:
 *   - T[:]  -> CCSlice_T (slice types)
 *   - T!E   -> CCResult_T_E (result types)
 *   - cc_ok(v) / cc_err(e) -> typed constructors (inferred result constructors)
 *   - try expr -> cc_try(expr) (try expressions)
 *
 * The retired optional surface (T?, CCOptional_T, *opt unwrap) used to live
 * here; see cc/include/ccc/DEPRECATIONS.md for the migration matrix.
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

/* Result type registry - used by codegen to emit CC_DECL_RESULT_SPEC declarations */
extern CCResultSpecTable cc__cg_result_specs;

/* Reset type registries to empty (retain allocated buffer capacity).
 * Call once at the start of each compilation unit in visit_codegen.c rather
 * than relying on the implicit reset inside each scan function. */
void cc__cg_reset_type_registries(void);

#endif /* CC_PASS_TYPE_SYNTAX_H */
