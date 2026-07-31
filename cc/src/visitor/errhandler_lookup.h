/*
 * Brace-scoped @errhandler registry + type-matched lookup.
 *
 * Dispatch prefers the innermost in-scope handler whose parameter type
 * exactly matches the unwrap's Result error type E. If none, the
 * innermost handler whose parameter type is reachable from E via a
 * unique `@as` embed path (same preference order as UFCS) wins; the
 * binder RHS projects through that path (by-value member selection).
 */
#ifndef CC_VISITOR_ERRHANDLER_LOOKUP_H
#define CC_VISITOR_ERRHANDLER_LOOKUP_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CC_ERRHANDLER_STK_MAX 64
#define CC_ERRHANDLER_AS_PATH_MAX 128

/* out_as_diag from cc_errhandler_stack_find_with_as / find_for_call. */
#define CC_ERRHANDLER_AS_EXACT  1  /* param_type == E */
#define CC_ERRHANDLER_AS_FACE   0  /* unique @as path E → handler type */
#define CC_ERRHANDLER_AS_NONE  -1
#define CC_ERRHANDLER_AS_AMBIG -2  /* distinct faces / multi-path to one face */
#define CC_ERRHANDLER_AS_CYCLE -3

typedef struct {
    int reg_depth;
    const char* body; /* aliases source; not owned */
    size_t body_len;
    char param_decl[192];
    char param_type[128];
    char param_name[64];
    size_t at_pos; /* byte offset of '@' in @errhandler */
} CCErrHandlerFrame;

typedef struct {
    CCErrHandlerFrame frames[CC_ERRHANDLER_STK_MAX];
    int n;
} CCErrHandlerStack;

void cc_errhandler_stack_init(CCErrHandlerStack* stk);

/* Split "CCIoError e" / "struct foo *err" → type prefix + trailing name. */
int cc_errhandler_split_param_decl(const char* decl,
                                   char* type_out, size_t type_sz,
                                   char* name_out, size_t name_sz);

/*
 * Parse `@errhandler(E e) { … }` or `@errhandler(E e) STMT;` starting at
 * `at` (byte of '@'). On success: param decl [decl_a,decl_b), body
 * [body_a,body_b), and stmt_end past the registration (after optional `;`
 * following a block, or after the stmt's terminating `;`).
 * Block bodies are the interior (no braces). Stmt bodies include the `;`.
 * Returns 1 on success, 0 if malformed.
 */
int cc_errhandler_parse_registration(const char* s, size_t n, size_t at,
                                     size_t* decl_a, size_t* decl_b,
                                     size_t* body_a, size_t* body_b,
                                     size_t* stmt_end);

int cc_errhandler_types_equal(const char* a, const char* b);

/* Innermost frame with param_type == err_type; NULL if none. */
const CCErrHandlerFrame* cc_errhandler_stack_find(const CCErrHandlerStack* stk,
                                                  const char* err_type);

/*
 * Exact match, else unique @as path from err_type → frame param_type.
 * On success returns the frame; writes dotted path into out_as_path
 * (empty when exact). Returns NULL if none / ambiguous / cycle.
 * Scans every in-scope handler before choosing: two distinct handler
 * types both reachable from E via @as → ambiguous (Rule 3), not the
 * innermost face. out_as_diag: CC_ERRHANDLER_AS_* (optional).
 */
const CCErrHandlerFrame* cc_errhandler_stack_find_with_as(
    const CCErrHandlerStack* stk,
    const char* err_type,
    char* out_as_path,
    size_t out_as_path_sz,
    int* out_as_diag);

/* When CC_DEBUG_ERRHANDLER_AS is set, log exact / @as / ambient dispatch. */
void cc_errhandler_debug_dispatch(const char* err_type, int ambient,
                                  const char* handler_type,
                                  const char* as_path, int as_diag);

/*
 * Scan s[0..pos) and build the in-scope handler stack at `pos`.
 * Bodies alias into `s`. Returns 0 on success, -1 on stack overflow /
 * malformed @errhandler that should already be diagnosed elsewhere
 * (skips unparseable registrations).
 */
int cc_errhandler_stack_build_at(const char* s, size_t n, size_t pos,
                                 CCErrHandlerStack* out);

/*
 * Resolve Result error type E for call span [call_a, call_b) using the
 * result-fn registry and buffer scans. Writes NUL-terminated type into
 * out. Returns 1 on success, 0 if unknown.
 */
int cc_errhandler_resolve_call_err_type(const char* s, size_t n,
                                        size_t call_a, size_t call_b,
                                        char* out, size_t out_sz);

/*
 * Build stack at `pos`, resolve E for the call, find matching handler
 * (exact, else @as). On success fills out_* (body aliases into s) and
 * returns 1. out_as_path is empty on exact match.
 * Returns 0 if no match / ambiguous / cycle / no handlers.
 * *out_have_handlers is 1 if any in-scope handler existed (for diagnostics).
 * *out_err_type receives resolved E (or "CCError" when ambient).
 * *out_as_diag receives CC_ERRHANDLER_AS_* (including AMBIG/CYCLE on miss).
 * *out_ambient is 1 when Result E could not be resolved and dispatch used
 * ambient CCError (optional).
 * Set CC_DEBUG_ERRHANDLER_AS to log exact / @as / ambient on success.
 */
int cc_errhandler_find_for_call(const char* s, size_t n, size_t pos,
                                size_t call_a, size_t call_b,
                                char* out_decl, size_t out_decl_sz,
                                size_t* out_decl_len,
                                const char** out_body, size_t* out_body_len,
                                size_t* out_decl_pos,
                                char* out_err_type, size_t out_err_type_sz,
                                char* out_as_path, size_t out_as_path_sz,
                                int* out_have_handlers,
                                int* out_as_diag,
                                int* out_ambient);

#ifdef __cplusplus
}
#endif

#endif /* CC_VISITOR_ERRHANDLER_LOOKUP_H */
