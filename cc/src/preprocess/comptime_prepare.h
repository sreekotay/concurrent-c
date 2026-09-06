#ifndef CC_COMPTIME_PREPARE_H
#define CC_COMPTIME_PREPARE_H

#include <stddef.h>

/* Which of the prepare passes to run. The clean lowerer resolves
 * type-scoped calls and string templates itself, from its own index and
 * AST: running them here too would hand it C where it expects the
 * language, and its own steps would then have nothing to lower. */
#define CC_PREPARE_GRAMMAR      1u
#define CC_PREPARE_MODULE_EXPORT 2u
#define CC_PREPARE_TYPE_SCOPED   4u
#define CC_PREPARE_FACTORY_SUGAR 8u
#define CC_PREPARE_STATIC_MAP    16u
#define CC_PREPARE_COMPTIME      32u
#define CC_PREPARE_TEMPLATES     64u
#define CC_PREPARE_ALL           (CC_PREPARE_GRAMMAR | CC_PREPARE_MODULE_EXPORT | \
                                  CC_PREPARE_TYPE_SCOPED | CC_PREPARE_FACTORY_SUGAR | \
                                  CC_PREPARE_STATIC_MAP | CC_PREPARE_COMPTIME | \
                                  CC_PREPARE_TEMPLATES)

int cc_comptime_prepare_source_ex(char** inout_buf, size_t* inout_len,
                                  const char* input_path, unsigned passes);

/* Resolve `@comptime if/for`, then lower `@emit` / `@string` templates.
 * Updates *inout_buf / *inout_len in place (frees the prior buffer on change).
 * Returns 0 on success, -1 on hard error ((char*)-1 from a sub-pass). */
int cc_comptime_prepare_source(char** inout_buf, size_t* inout_len,
                               const char* input_path);

/* 1 = route @comptime if/for predicate + field load through libtcc (default).
 * Set CC_COMPTIME_UNIFIED_EXEC=0 to use the legacy structural resolver only. */
int cc_comptime_unified_exec_enabled(void);

#endif /* CC_COMPTIME_PREPARE_H */
