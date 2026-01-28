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
#include "visitor/visitor.h"

/* Rewrite slice type syntax `T[:]` to `CCSlice_T`.
 * Returns newly allocated string, or NULL if no rewrites. */
char* cc__rewrite_slice_types_text(const CCVisitorCtx* ctx, const char* src, size_t n);

/* Rewrite optional type syntax `T?` to `CCOptional_T`.
 * Returns newly allocated string, or NULL if no rewrites. */
char* cc__rewrite_optional_types_text(const CCVisitorCtx* ctx, const char* src, size_t n);

/* Rewrite result type syntax `T!E` to `CCResult_T_E`.
 * Returns newly allocated string, or NULL if no rewrites. */
char* cc__rewrite_result_types_text(const CCVisitorCtx* ctx, const char* src, size_t n);

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
typedef struct {
    char mangled_ok[128];
    char mangled_err[128];
    char ok_type[128];
    char err_type[128];
} CCCodegenResultTypePair;

extern CCCodegenResultTypePair cc__cg_result_types[64];
extern size_t cc__cg_result_type_count;

#endif /* CC_PASS_TYPE_SYNTAX_H */
