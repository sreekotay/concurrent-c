#ifndef CC_COMPTIME_EXECUTOR_H
#define CC_COMPTIME_EXECUTOR_H

#include <stddef.h>
#include <stdint.h>

#define CC_COMPTIME_FN_NAME_MAX 64

/* Stage 0 comptime executor: compile a C-subset @comptime block body with
 * libtcc (TCC_OUTPUT_MEMORY), inject host API symbols, run __cc_ct_entry().
 * Emits and instantiations reach the compiler via cc_emit_plan_host_* callbacks.
 * Returns 0 on success, -1 on error (err_buf optional). Never aborts. */
typedef struct CCComptimeExecOpts {
    const char* input_path;
    size_t      site_pos;       /* source offset of the @comptime block (anchors) */
    const char* type_prelude;   /* optional expanded type defs for host cc_type_of */
} CCComptimeExecOpts;

/* Scan top-level `@comptime` function definitions from `src` (not blocks). */
void cc_comptime_fn_registry_clear(void);
int  cc_comptime_fn_registry_scan(const char* src, size_t len);
const char* cc_comptime_fn_registry_scan_error(void);
int  cc_comptime_fn_is_registered(const char* name);
const char* cc_comptime_fn_registry_defs(void);
const char* cc_comptime_fn_registry_lookup_def(const char* name);
int cc_comptime_fn_registry_lookup_line(const char* name);

int cc_comptime_exec_block_body(const char* body, size_t body_len,
                                const CCComptimeExecOpts* opts,
                                char* err_buf, size_t err_sz);

/* Evaluate `expr` with registered @comptime fn defs in scope; writes integer
 * result to *out on success.  Used for result marshaling from @comptime code. */
int cc_comptime_exec_eval_int(const char* expr,
                              const CCComptimeExecOpts* opts,
                              int64_t* out,
                              char* err_buf, size_t err_sz);

/* Validate a generated C fragment (a generic factory/template definition) in
 * isolation, before it is spliced into the merged translation unit.  Compiles
 * `fragment` (preceded by a minimal header prelude) with libtcc, no relocate.
 *
 * Returns:
 *    0  fragment is syntactically well-formed, OR the failure could not be
 *       attributed to the fragment itself (e.g. it references a type/symbol
 *       defined elsewhere in the user TU) — the caller must not block.
 *   -1  the fragment contains a definite syntax error.  `err_buf` holds the
 *       first compiler message and `*out_frag_line` (if non-NULL) holds the
 *       1-based line within the fragment that failed (0 if unknown). */
int cc_comptime_validate_c_fragment(const char* fragment,
                                    int* out_frag_line,
                                    char* err_buf, size_t err_sz);

#endif /* CC_COMPTIME_EXECUTOR_H */
