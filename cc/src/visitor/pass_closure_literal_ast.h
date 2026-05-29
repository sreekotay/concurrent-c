#ifndef CC_PASS_CLOSURE_LITERAL_AST_H
#define CC_PASS_CLOSURE_LITERAL_AST_H

#include <stddef.h>

#include "visitor/visitor.h"
#include "visitor/edit_buffer.h"

/* Rewrite closure literals (`() => { ... }`, `x => expr`, etc.) using stub-AST spans.
   Emits top-level closure env/entry/make definitions into out_protos/out_defs;
   the caller is responsible for placing `*out_protos` somewhere visible to all
   call sites (typically at top-of-file, after `#include`s — see
   `find_protos_insertion_point` in `edit_buffer.c`).

   Returns:
   - 1 if rewritten (out_src/out_len set; out_protos/out_defs may be set)
   - 0 if no changes
   - -1 on hard error (diagnostic already printed)

   The pre-2026-05-28 `_ex` variant accepted a `skip_inline_protos` parameter
   to choose between in-source-walker forward-decl placement and file-scope
   placement.  Only the file-scope path remains; the walker
   (`cc__closure_proto_insert_off`) and the non-`_ex` wrapper were deleted. */
int cc__rewrite_closure_literals_with_nodes(const CCASTRoot* root,
                                            const CCVisitorCtx* ctx,
                                            const char* in_src,
                                            size_t in_len,
                                            char** out_src,
                                            size_t* out_len,
                                            char** out_protos,
                                            size_t* out_protos_len,
                                            char** out_defs,
                                            size_t* out_defs_len);

/* `cc__collect_closure_edits` was deleted 2026-05-28 — it was a fake
   per-span CCEditBuffer wrapper around `cc__rewrite_closure_literals_with_nodes`
   that had no callers and emitted a wholesale `[0, src_len)` edit anyway.
   The whole-file API above is the only supported closure-literal entry point. */

#endif /* CC_PASS_CLOSURE_LITERAL_AST_H */

