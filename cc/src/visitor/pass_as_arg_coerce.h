#ifndef CC_PASS_AS_ARG_COERCE_H
#define CC_PASS_AS_ARG_COERCE_H

#include <stddef.h>

#include "visitor/visitor.h"
#include "visitor/edit_buffer.h"

/* Arg-position `@as` autocast: when a call expects `T*` and the argument is
 * `Outer` / `Outer*` with `T name @as`, rewrite to `&arg.name` / `&arg->name`.
 * Explicit C casts are left alone (optional warn if the @as field is not the
 * first member). By-value Outer→T is not performed.
 *
 * Phase-3 stage 1.5 sibling of slice-literal coerce (after UFCS, before
 * autoblock). Returns edits added (>= 0), or -1 on hard error. */
int cc__collect_as_arg_coerce_edits(const CCASTRoot* root,
                                    const CCVisitorCtx* ctx,
                                    CCEditBuffer* eb);

#endif /* CC_PASS_AS_ARG_COERCE_H */
