#ifndef CC_PASS_SLICE_LITERAL_COERCE_H
#define CC_PASS_SLICE_LITERAL_COERCE_H

#include <stddef.h>

#include "visitor/visitor.h"
#include "visitor/edit_buffer.h"

/* Wrap string-literal call args whose corresponding parameter is CCSlice /
 * char[:0] (by value) with const_char_to_slice(...).  Literals only — no
 * char[N] / char* variable coercion.
 *
 * Runs as Phase-3 stage 1.5 (after UFCS, before autoblock) so lowered
 * method calls see slice-typed params and autoblock does not overlap
 * arg-span edits.
 *
 * Returns the number of edits added (>= 0), or -1 on error. */
int cc__collect_slice_literal_coerce_edits(const CCASTRoot* root,
                                           const CCVisitorCtx* ctx,
                                           CCEditBuffer* eb);

#endif /* CC_PASS_SLICE_LITERAL_COERCE_H */
