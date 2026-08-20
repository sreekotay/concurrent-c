#ifndef CC_VISITOR_VISITOR_H
#define CC_VISITOR_VISITOR_H

#include "ast/ast.h"
#include "comptime/symbols.h"

// Placeholder structures for type/context state used during visiting.
typedef struct CCVisitorCtx {
    CCSymbolTable* symbols;
    const char* input_path; // used for #line source mapping
    /* M7.C3 / M1-lite: post-pre-expand text buffer (from the AST root) that
     * span-based scanners can consult as a fallback when the raw user source
     * doesn't contain the pattern they're looking for. NULL when pre-expand
     * is off.  Borrowed; owned by the AST root. */
    const char* pre_expanded_buf;
    size_t      pre_expanded_len;
} CCVisitorCtx;

/* Product emit is `shadow_lower`. These visitor passes stay linked into
 * libshadow_comptime (header/factory text). There is no `cc_visit` front. */

#endif // CC_VISITOR_VISITOR_H

