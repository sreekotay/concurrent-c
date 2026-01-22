#ifndef CC_VISITOR_UFCS_H
#define CC_VISITOR_UFCS_H

#include <stddef.h>

#include "ast/ast.h"

// UFCS rewrite: transforms x.method(a, b) -> method(&x, a, b) (with small
// built-in mappings for std string and stdout helpers). Operates on source text
// until the real AST is available via TCC hooks.
int cc_ufcs_rewrite(CCASTRoot* root);

// Rewrite a single source line in-place into the out buffer (UTF-8 C text).
// Returns 0 on success; falls back to copying input when no UFCS patterns are
// present. Safe for nested method calls.
int cc_ufcs_rewrite_line(const char* in, char* out, size_t out_cap);

// Rewrite UFCS with await context flag. When is_await=1, channel ops (send/recv)
// emit task-returning variants (cc_chan_*_task) for use in @async functions.
int cc_ufcs_rewrite_line_await(const char* in, char* out, size_t out_cap, int is_await);

// Extended rewrite with type info. recv_type_is_ptr=1 when the receiver's resolved
// type is a pointer (from TCC), enabling correct dispatch for ptr.free() vs handle.free().
int cc_ufcs_rewrite_line_ex(const char* in, char* out, size_t out_cap, int is_await, int recv_type_is_ptr);

// Full UFCS rewrite with receiver type from TCC. If recv_type is non-NULL,
// generates TypeName_method(&recv, ...) for struct types.
int cc_ufcs_rewrite_line_full(const char* in, char* out, size_t out_cap, 
                              int is_await, int recv_type_is_ptr, const char* recv_type);

#endif // CC_VISITOR_UFCS_H

