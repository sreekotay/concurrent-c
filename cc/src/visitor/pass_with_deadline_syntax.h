#ifndef CC_VISITOR_PASS_WITH_DEADLINE_SYNTAX_H
#define CC_VISITOR_PASS_WITH_DEADLINE_SYNTAX_H

#include <stddef.h>

#include "visitor/edit_buffer.h"

/* Rewrite `with_deadline(expr) { ... }` into a CCDeadline scope using @defer.
   Returns 0 on success and allocates *out_src (caller frees). */
int cc__rewrite_with_deadline_syntax(const char* in_src,
                                     size_t in_len,
                                     char** out_src,
                                     size_t* out_len);

/* NEW: Collect with_deadline edits into EditBuffer without applying.
   Returns number of edits added (>= 0), or -1 on error. */
int cc__collect_with_deadline_edits(CCEditBuffer* eb);

#endif

