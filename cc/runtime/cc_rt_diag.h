#ifndef CC_RT_DIAG_H
#define CC_RT_DIAG_H

#include <stdint.h>
#include <stdio.h>

typedef struct CCRuntimeSourceLoc {
    const char* file;
    int line;
    int col;
    const char* construct;
    const char* user_name;
} CCRuntimeSourceLoc;

/* R0: load serialized source map companion (.ccs.map) */
int cc_rt_diag_load_map(const char* map_path);

int cc_rt_resolve_pc(void* pc, CCRuntimeSourceLoc* out);

void cc_rt_diag_set_async_name(const char* cc_name, const char* file, int line);
void cc_rt_diag_set_channel_meta(const char* name, const char* topology,
                                 const char* file, int line);

/* ----------------------------------------------------------------------
 * R3 — `!>` source-location propagation chain.
 *
 * Every `!>` propagation site emits a call to `cc_rt_diag_record_unwrap_site`
 * (wired in `cc_result.cch`'s `__cc_uw_err_at` macro and the per-TU
 * enumerated `_Generic` arms in `visit_codegen.c`).  The chain records the
 * `(file, line)` pairs of the most recent N propagation sites in
 * chronological push order, so when an error bubbles up to an
 * `@errhandler` (or to `main`), the handler can walk the chain and show
 * the user *where* the error trickled through.
 *
 * Strings are not copied — callers pass C string literals embedded in the
 * lowered C (so they live for the program's lifetime).  No allocation,
 * no formatting on the hot propagation path; just two pointer stores.
 *
 * The chain is a process-global ring buffer for now.  Multi-threaded
 * programs will see interleaving; revisit with `_Thread_local` storage
 * once we have a real multi-thread test.  Single-thread fiber programs
 * (the common case today) get the natural chain.
 *
 * `line_str` is the line number as a C string literal (matches the
 * other unwrap-site macros which already produce a stringified line).
 * Storing it as a string avoids re-parsing/formatting at print time. */

#ifndef CC_RT_DIAG_UNWRAP_CHAIN_MAX
#define CC_RT_DIAG_UNWRAP_CHAIN_MAX 16
#endif

void cc_rt_diag_record_unwrap_site(const char* file, const char* line_str);
void cc_rt_diag_clear_unwrap_chain(void);
int  cc_rt_diag_unwrap_chain_len(void);
/* Returns 0 on success, -1 if i is out of range.  Either out_* pointer may be NULL. */
int  cc_rt_diag_unwrap_site(int i, const char** out_file, const char** out_line_str);
/* Print "  propagated through <file>:<line>\n" for each chain entry (oldest first).
 * No-op for an empty chain.  `fp` may be NULL (defaults to stderr). */
void cc_rt_diag_print_unwrap_chain(FILE* fp);

#endif
