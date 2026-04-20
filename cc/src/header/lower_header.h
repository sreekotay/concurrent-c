/*
 * lower_header.h - Header lowering infrastructure
 *
 * Converts .cch (CC Header) files to .h (C Header) files by:
 * - Rewriting T!>(E) -> CCResult_T_E + guarded CC_DECL_RESULT_SPEC
 * - Other CC syntax transformations as needed
 *
 * (The retired T? -> CCOptional_T lowering used to live here;
 * see cc/include/ccc/DEPRECATIONS.md for the migration matrix.)
 *
 * This allows .cch files to use CC syntax while outputting standard C headers
 * that can be included by generated code.
 */
#ifndef CC_LOWER_HEADER_H
#define CC_LOWER_HEADER_H

#include <stddef.h>

#include "result_spec.h"

/*
 * Lower a .cch file to a .h file.
 *
 * @param cch_path  Path to input .cch file
 * @param h_path    Path to output .h file
 * @return 0 on success, non-zero on error
 *
 * The output .h file contains:
 * - All original content with CC syntax transformed to C
 * - Guarded type declarations (CC_DECL_RESULT_SPEC)
 */
int cc_lower_header(const char* cch_path, const char* h_path);

/*
 * Lower a .cch string to C header content.
 *
 * @param input     Input .cch content
 * @param input_len Length of input
 * @param input_path Path for error messages (may be NULL)
 * @return Allocated string with lowered content, or NULL on error
 *
 * Caller must free() the returned string.
 */
char* cc_lower_header_string(const char* input, size_t input_len, const char* input_path);

/*
 * State for header lowering, tracking collected types.
 */
typedef struct CCLowerState {
    CCResultSpecTable result_specs;
} CCLowerState;

/*
 * Initialize lowering state.
 */
void cc_lower_state_init(CCLowerState* state);

/*
 * Add a result type pair to the lowering state.
 */
void cc_lower_state_add_result(CCLowerState* state,
                                const char* ok_type, size_t ok_len,
                                const char* err_type, size_t err_len,
                                const char* mangled_ok,
                                const char* mangled_err);

/*
 * Generate type declarations for all collected types.
 * Returns allocated string with guarded CC_DECL_* macros.
 * Caller must free() the returned string.
 */
char* cc_lower_state_emit_decls(const CCLowerState* state);

#endif /* CC_LOWER_HEADER_H */
