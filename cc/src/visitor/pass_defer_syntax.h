#ifndef CC_PASS_DEFER_SYNTAX_H
#define CC_PASS_DEFER_SYNTAX_H

#include <stddef.h>

#include "visitor/visitor.h"
#include "visitor/edit_buffer.h"

/* Rewrite `@defer ...;` constructs into plain C by injecting the deferred statements
   immediately before the closing brace of the scope they were registered in.

   Also hard-errors on `cancel ...;` (not implemented).

   Returns:
   - 1 if rewritten and out_* is set
   - 0 if no changes
   - -1 on hard error (diagnostic already printed) */
int cc__rewrite_defer_syntax(const CCVisitorCtx* ctx,
                            const char* in_src,
                            size_t in_len,
                            char** out_src,
                            size_t* out_len);

/* NEW: Collect @defer edits into EditBuffer without applying.
   Returns number of edits added (>= 0), or -1 on error. */
int cc__collect_defer_edits(const CCVisitorCtx* ctx, CCEditBuffer* eb);

#endif /* CC_PASS_DEFER_SYNTAX_H */

