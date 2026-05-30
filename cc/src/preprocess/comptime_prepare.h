#ifndef CC_COMPTIME_PREPARE_H
#define CC_COMPTIME_PREPARE_H

#include <stddef.h>

/* Resolve `@comptime if/for`, then lower `@emit` / `@string` templates.
 * Updates *inout_buf / *inout_len in place (frees the prior buffer on change).
 * Returns 0 on success, -1 on hard error ((char*)-1 from a sub-pass). */
int cc_comptime_prepare_source(char** inout_buf, size_t* inout_len,
                               const char* input_path);

/* 1 = route @comptime if/for predicate + field load through libtcc (default).
 * Set CC_COMPTIME_UNIFIED_EXEC=0 to use the legacy structural resolver only. */
int cc_comptime_unified_exec_enabled(void);

#endif /* CC_COMPTIME_PREPARE_H */
