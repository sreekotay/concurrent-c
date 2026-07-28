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

int cc_errhandler_types_equal(const char* a, const char* b);

/* Innermost frame with param_type == err_type; NULL if none. */
const CCErrHandlerFrame* cc_errhandler_stack_find(const CCErrHandlerStack* stk,
                                                  const char* err_type);

/*
 * Exact match, else unique @as path from err_type → frame param_type.
 * On success returns the frame; writes dotted path into out_as_path
 * (empty when exact). Returns NULL if none / ambiguous / cycle.
 * out_as_diag: -1 none, -2 ambiguous, -3 cycle (optional).
 */
const CCErrHandlerFrame* cc_errhandler_stack_find_with_as(
    const CCErrHandlerStack* stk,
    const char* err_type,
    char* out_as_path,
    size_t out_as_path_sz,
    int* out_as_diag);

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
 * Returns 0 if no match / unknown E / no handlers.
 * *out_have_handlers is 1 if any in-scope handler existed (for diagnostics).
 * *out_err_type receives resolved E when known (may be empty).
 */
int cc_errhandler_find_for_call(const char* s, size_t n, size_t pos,
                                size_t call_a, size_t call_b,
                                char* out_decl, size_t out_decl_sz,
                                size_t* out_decl_len,
                                const char** out_body, size_t* out_body_len,
                                size_t* out_decl_pos,
                                char* out_err_type, size_t out_err_type_sz,
                                char* out_as_path, size_t out_as_path_sz,
                                int* out_have_handlers);

#ifdef __cplusplus
}
#endif

#endif /* CC_VISITOR_ERRHANDLER_LOOKUP_H */
