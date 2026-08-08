/* Comptime string-switch lower: case "lit" → CCSlice_eq_cstr if/else chain.
 * Runtime/shadow keeps MPH/static-map lowering; this is executor-only. */
#ifndef CC_PREPROCESS_STRSWITCH_COMPTIME_H
#define CC_PREPROCESS_STRSWITCH_COMPTIME_H

#include <stddef.h>

/* Rewrite string-literal case switches in a `@comptime` block body.
 * Returns a newly allocated buffer (caller frees). On success with no
 * string switches, returns a copy of the input.
 * On hard error (mixed labels, bad escapes, etc.): fills err, returns NULL. */
char* cc_comptime_strswitch_rewrite(const char* body, size_t body_len,
                                    char* err, size_t errcap);

#endif /* CC_PREPROCESS_STRSWITCH_COMPTIME_H */
