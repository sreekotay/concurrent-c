/* Legacy comptime prepare/exec/splice bridge for shadow_lower.
 * Runs the production emit-plan path (libtcc) then splices fragments
 * into the SERDES emit buffer. */
#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Lower-then-TCC type pass: read input, rewrite includes, harvest, space-blank
 * all `@comptime` constructs (blocks / for / if) so whitelist parse+emit can
 * populate `cc_ct_field_reg_*` from `__cc_rf_*` tables before comptime exec.
 * Returns NULL (skip type pass) when the harvested TU has no `@comptime`.
 * Caller frees. */
char* shadow_comptime_type_pass_src(const char* input_path, size_t* out_n);

/* Field registry (libshadow_comptime) — filled during type-pass emit. */
void cc_ct_field_reg_clear(void);
int cc_ct_field_reg_put(const char* type_name, const char* const* names,
                        const char* const* types, const int* is_as, int n);
void cc_ct_field_reg_set_lowered_c(char* owned_c);

/* Prepare + execute @comptime blocks for `input_path` (reads the file).
 * Populates the process-global emit-plan fragment table.
 * On success, if `out_stage1_src` is non-NULL, fills it with a malloc'd
 * whitelist stage1 buffer: original include/factory spelling, `@comptime if`
 * / value-position resolved, `@comptime` blocks space-blanked. Caller
 * transfers ownership into the FileTape (or frees). Diags keep `input_path`.
 * Returns 0 on success, -1 on hard comptime/diagnostic failure. */
int shadow_comptime_exec_file(const char* input_path, char** out_stage1_src,
                              size_t* out_stage1_len);

/* Splice collected comptime fragments into an emit buffer in place.
 * No-op when the fragment table is empty. Returns 0 on success. */
int shadow_comptime_splice_emit(char** emit_buf, size_t* emit_len,
                                const char* input_path);

/* Harvested / rewritten .cch paths from the last shadow_comptime_exec_file
 * (same process; cleared on the next exec). For emit-cache dep sidecars. */
size_t cc_included_cch_source_count(void);
const char* cc_included_cch_source_path(size_t i);

/* Emit-plan hooks used by whitelist instantiate (linked via libshadow_comptime).
 * CCGenProduceStatus is declared in pp_emit_core.cch when building shadow_lower. */
int cc_emit_plan_generic_factory_names_csv(char* out, size_t cap);
int cc_emit_plan_has_generic_factory(const char* name);
void cc_emit_plan_snake_name(const char* name, char* out, size_t cap);
size_t cc_emit_plan_comptime_fragment_count(void);
const char* cc_emit_plan_comptime_fragment_text(size_t frag_index);

#ifdef __cplusplus
}
#endif
