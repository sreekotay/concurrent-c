/*
 * lower_header.h - Header lowering infrastructure
 *
 * Converts .cch (CC Header) files to .h (C Header) files by:
 * - Rewriting T!>(E) -> CCResult_T_E + guarded CC_DECL_RESULT_SPEC
 * - Rewriting T? -> CCOptional_T + guarded CC_DECL_OPTIONAL
 * - Other CC syntax transformations as needed
 *
 * This allows .cch files to use CC syntax while outputting standard C headers
 * that can be included by generated code.
 */
#ifndef CC_LOWER_HEADER_H
#define CC_LOWER_HEADER_H

#include <stddef.h>

/*
 * Lower a .cch file to a .h file.
 *
 * @param cch_path  Path to input .cch file
 * @param h_path    Path to output .h file
 * @return 0 on success, non-zero on error
 *
 * The output .h file contains:
 * - All original content with CC syntax transformed to C
 * - Guarded type declarations (CC_DECL_RESULT_SPEC, CC_DECL_OPTIONAL)
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
 * Result type pair collected during lowering.
 * Used to emit CC_DECL_RESULT_SPEC declarations.
 */
typedef struct CCLowerResultType {
    char ok_type[128];      /* Raw ok type: "int", "MyData*" */
    char err_type[128];     /* Raw error type: "CCError", "CCIoError" */
    char mangled_ok[128];   /* Mangled ok type: "int", "MyDataptr" */
    char mangled_err[128];  /* Mangled error type: "CCError", "CCIoError" */
} CCLowerResultType;

/*
 * Optional type collected during lowering.
 * Used to emit CC_DECL_OPTIONAL declarations.
 */
typedef struct CCLowerOptionalType {
    char raw_type[128];     /* Raw type: "MyData*" */
    char mangled_type[128]; /* Mangled type: "MyDataptr" */
} CCLowerOptionalType;

/*
 * State for header lowering, tracking collected types.
 */
typedef struct CCLowerState {
    CCLowerResultType result_types[64];
    size_t result_type_count;
    CCLowerOptionalType optional_types[64];
    size_t optional_type_count;
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
 * Add an optional type to the lowering state.
 */
void cc_lower_state_add_optional(CCLowerState* state,
                                  const char* raw_type, size_t raw_len,
                                  const char* mangled_type);

/*
 * Generate type declarations for all collected types.
 * Returns allocated string with guarded CC_DECL_* macros.
 * Caller must free() the returned string.
 */
char* cc_lower_state_emit_decls(const CCLowerState* state);

#endif /* CC_LOWER_HEADER_H */
