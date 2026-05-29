#ifndef CC_COMPTIME_EXECUTOR_H
#define CC_COMPTIME_EXECUTOR_H

#include <stddef.h>

/* Stage 0 comptime executor: compile a C-subset @comptime block body with
 * libtcc (TCC_OUTPUT_MEMORY), inject host API symbols, run __cc_ct_entry().
 * Emits and instantiations reach the compiler via cc_emit_plan_host_* callbacks.
 * Returns 0 on success, -1 on error (err_buf optional). Never aborts. */
typedef struct CCComptimeExecOpts {
    const char* input_path;
    size_t      site_pos;       /* source offset of the @comptime block (anchors) */
    const char* type_prelude;   /* optional expanded type defs for host cc_type_of */
} CCComptimeExecOpts;

int cc_comptime_exec_block_body(const char* body, size_t body_len,
                                const CCComptimeExecOpts* opts,
                                char* err_buf, size_t err_sz);

#endif /* CC_COMPTIME_EXECUTOR_H */
