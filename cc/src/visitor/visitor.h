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
    /* M1: when nonzero, the visitor's working `src_all` is itself the
     * pre-expanded buffer (i.e. M1.1's src-buffer unification is active).
     * Reparses use this to skip steps that re-prepend or re-process
     * system-header content that's already inlined in the buffer
     * (`cc__prepend_reparse_prelude`, .cch → .h rewriting, container
     * include prepends).  When zero, reparses behave exactly as they
     * did pre-M1 (full prelude + preprocess chain). */
    int         src_is_pre_expanded;
    // TODO: add type tables, provenance tracking, arena/async context, codegen state.
} CCVisitorCtx;

// Run the main visitor and emit C to output_path.
int cc_visit(const CCASTRoot* root, CCVisitorCtx* ctx, const char* output_path);

#endif // CC_VISITOR_VISITOR_H

