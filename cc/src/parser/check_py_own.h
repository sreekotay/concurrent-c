#ifndef CC_CHECK_PY_OWN_H
#define CC_CHECK_PY_OWN_H

#include <stddef.h>

/* CCPyObj ownership check, locals only: a local declaration
 * `CCPyObj name = init;` must take ownership with `@destroy` or be
 * consumed by a later `return name`.  A dropped handle leaks a Python
 * reference with no diagnostic on either side of the boundary, so the
 * leak is promoted to a compile error at the declaration.
 *
 * Runs on the RAW USER FILE (before include splice), so header-internal
 * CCPyObj plumbing is never in view and positions are the user's own.
 * Pointer declarations (`CCPyObj *p`) are borrows and exempt; struct
 * fields and parameters don't match the `= init` shape and are exempt
 * by construction.
 *
 * Returns 0 when clean, -1 after printing a diagnostic. */
int cc_check_py_own(const char* src, size_t n, const char* input_path);

#endif
