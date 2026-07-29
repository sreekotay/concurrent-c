/*
 * CCEmitPlan — unified instantiation anchors and splice scheduling (track A2).
 *
 * Replaces ad-hoc insert_pos / container_pos / delayed Vec-Map-Result logic
 * duplicated in preprocess.c and visit_codegen.c.  Feeds the comptime seam:
 * every monomorph emission names an explicit CCEmitAnchor.
 *
 * See cc/docs/COMPTIME_INSTANTIATION_SEAM.md.
 */
#ifndef CC_EMIT_PLAN_H
#define CC_EMIT_PLAN_H

#include <stddef.h>
#include <stdio.h>

#include "preprocess/type_graph.h"
#include "preprocess/type_registry.h"
#include "result_spec.h"

#define CC_EMIT_PLAN_MAX_DELAYED 512
#define CC_EMIT_PLAN_MAX_COMPTIME_FRAGMENTS 64

typedef enum CCEmitAnchor {
    CC_EMIT_AFTER_PRELUDE = 0,
    CC_EMIT_BEFORE_FIRST_USE = 1,
    CC_EMIT_AT_COMPTIME_SITE = 2,
} CCEmitAnchor;

typedef struct CCEmitPlanComptimeSchedule {
    size_t n;
    size_t pos[CC_EMIT_PLAN_MAX_COMPTIME_FRAGMENTS];
    size_t frag_index[CC_EMIT_PLAN_MAX_COMPTIME_FRAGMENTS];
} CCEmitPlanComptimeSchedule;

/* --- source scan (shared preprocess + visit_codegen) --- */
size_t cc_emit_plan_line_start_before(const char* src, size_t pos);
size_t cc_emit_plan_find_ident_top_level(const char* src, size_t start, size_t len,
                                         const char* ident);
/* Any-scope identifier-boundary find (comment/string-aware); len when absent. */
size_t cc_emit_plan_find_ident_any(const char* src, size_t len, const char* ident);
size_t cc_emit_plan_type_decl_end_top_level(const char* src, size_t len,
                                            const char* type_name);

/* After leading #include / # / blank lines; walks typedef/struct blocks. */
size_t cc_emit_plan_compute_prelude_insert_pos(const char* src, size_t len);

/* CC_EMIT_AFTER_PRELUDE for Vec/Map: stop before first decl referencing containers. */
size_t cc_emit_plan_compute_container_anchor(const char* src, size_t len);

size_t cc_emit_plan_compute_before_first_use(const char* src, size_t len,
                                             size_t anchor_pos,
                                             const char* payload_type,
                                             const char* mangled_name);

typedef struct CCEmitPlanContainerSchedule {
    size_t anchor_pos;
    size_t n_vec;
    size_t n_map;
    /* Typed slice instances from the session spec table (indices align
     * with cc_slice_spec_get). slice_emit marks instances the TU must
     * declare (non-prebaked element, no hand-written declaration). */
    size_t n_slice;
    unsigned char vec_delayed[CC_EMIT_PLAN_MAX_DELAYED];
    size_t vec_pos[CC_EMIT_PLAN_MAX_DELAYED];
    unsigned char map_delayed[CC_EMIT_PLAN_MAX_DELAYED];
    size_t map_pos[CC_EMIT_PLAN_MAX_DELAYED];
    unsigned char slice_emit[CC_EMIT_PLAN_MAX_DELAYED];
    unsigned char slice_delayed[CC_EMIT_PLAN_MAX_DELAYED];
    size_t slice_pos[CC_EMIT_PLAN_MAX_DELAYED];
} CCEmitPlanContainerSchedule;

typedef struct CCEmitPlanResultDelay {
    unsigned char delayed[CC_EMIT_PLAN_MAX_DELAYED];
    size_t pos[CC_EMIT_PLAN_MAX_DELAYED];
} CCEmitPlanResultDelay;

void cc_emit_plan_build_container_schedule(const char* src, size_t len,
                                           CCTypeGraph* graph,
                                           CCEmitPlanContainerSchedule* out);

void cc_emit_plan_build_result_delays(const char* src, size_t len,
                                      const CCResultSpecTable* specs,
                                      size_t prelude_insert_pos,
                                      CCEmitPlanResultDelay* out);

/* --- fragment emission (built-in container monomorphs today; cc_emit_cstr later) --- */
void cc_emit_plan_fprint_container_prelude(FILE* out, int use_cch,
                                           int need_vec, int need_map, int need_chan);
void cc_emit_plan_fprint_container_epilogue(FILE* out);
void cc_emit_plan_fprint_vec_decl(FILE* out, const CCTypeInstantiation* inst);
void cc_emit_plan_fprint_map_decl(FILE* out, const CCTypeInstantiation* inst);

/* D6.4: container declaration factories.  A factory emits the C declaration
 * text for one container instantiation (Vec/Map/...).  The built-in Vec/Map
 * emitters are registered by default, so the historic hardcoded
 * CC_VEC_DECL_ARENA(...) / CC_MAP_DECL_ARENA(...) emission is just the first
 * registered factory — `cc_emit_plan_fprint_vec_decl` / `_map_decl` dispatch
 * through this registry.  Libraries can register an additional container kind
 * (or override a built-in) through the same seam rather than the compiler
 * special-casing each one.  `kind` must outlive the registry (a literal or
 * a long-lived string); the last registration for a kind wins. */
typedef void (*CCContainerDeclFactory)(FILE* out, const CCTypeInstantiation* inst);
void cc_emit_plan_register_container_factory(const char* kind, CCContainerDeclFactory fn);
CCContainerDeclFactory cc_emit_plan_lookup_container_factory(const char* kind);
void cc_emit_plan_fprint_line_directive(FILE* out, const char* src, size_t offset,
                                        const char* input_path);

/* --- Result _Generic arm formatting (track A3) ---
 *
 * The unwrap primitives `__cc_uw_is_err` / `__cc_uw_value` / `__cc_uw_err_at`
 * expand to `_Generic((__x__), ...)` with one arm per concrete Result type.
 * Both the parser-mode emission (preprocess.c) and the final-compile emission
 * (visit_codegen.c) must use the *identical* cast-and-probe arm body or the
 * two parses disagree on field layout.  This single formatter owns that body
 * so the two call sites can keep their own control flow (which arms, defaults,
 * sink: FILE vs string buffer) without the arm format drifting. */
typedef enum CCResultArmKind {
    CC_RESULT_ARM_IS_ERR = 0,  /* (!((T*)&x)->ok)            */
    CC_RESULT_ARM_VALUE  = 1,  /* ((T*)&x)->u.value          */
    CC_RESULT_ARM_ERR    = 2,  /* ((T*)&x)->u.error          */
} CCResultArmKind;

/* Write one `_Generic` arm line ("    NAME: <body>, \\\n") for `concrete`.
 * For CC_RESULT_ARM_VALUE with ok_is_void the body is `((void)0)`.
 * For CC_RESULT_ARM_ERR with with_diag the body is wrapped in
 * `cc_rt_diag_record_unwrap_site(__f__, __l__)`.  Returns snprintf length. */
int cc_emit_plan_format_result_arm(char* out, size_t out_sz,
                                   const char* concrete,
                                   CCResultArmKind kind,
                                   int ok_is_void, int with_diag);

/* --- comptime explicit instantiation requests (track C1) ---
 *
 * `@comptime { cc_instantiate_vec("int"); cc_instantiate_map("int","int"); }`
 * lets a TU force a monomorph even if the type is never spelled as
 * `CCVec::[int]` / `Map::[int,int]` in source.  Collected before @comptime
 * blocks are blanked, then replayed into the per-TU graph at graph build. */
void cc_emit_plan_clear_comptime_instantiations(void);
size_t cc_emit_plan_comptime_instantiation_count(void);
void cc_emit_plan_collect_comptime_instantiations(const char* src, size_t len);
void cc_emit_plan_apply_comptime_instantiations(CCTypeGraph* graph);

/* --- user generic factories (compiled handlers) ---
 *
 * `cc_generic_register("Pair", handler)` — or the `CC_GENERIC_FACTORY(Pair){...}`
 * sugar that lowers to it — binds a library generic to a compiled @comptime
 * factory.  At a `Pair::[int,double]` use site the compiler computes a mangled
 * name, invokes the factory to produce the C definition, emits it once via the
 * comptime-fragment channel, and rewrites the use site to the mangled name.
 * Modeled on UFCS (library owns the C lowering; compiler owns the splice). */
void cc_emit_plan_register_generic_factory(const char* name, const char* handler_name);
/* Append an extension factory (CC_GENERIC_FACTORY_EXTEND); the base may register
 * before or after — the base requirement is enforced at the use site. */
void cc_emit_plan_register_generic_factory_extend(const char* name, const char* handler_name);
const void* cc_emit_plan_lookup_generic_factory(const char* name);
const char* cc_emit_plan_lookup_generic_factory_handler(const char* name);
/* True when `name` has any COMPILED registration (base or extension); the
 * use-site gate uses this so extend-only names still reach the base-required
 * diagnostic. */
int cc_emit_plan_has_generic_factory(const char* name);
int cc_emit_plan_compile_generic_factories(const char* src, size_t len,
                                           const char* input_path);
/* Compile a registered factory dylib on first use (errors attributed at call site). */
int cc_emit_plan_ensure_generic_factory(const char* generic_name, const char* input_path,
                                        char* err_buf, size_t err_sz);
int cc_emit_plan_invoke_generic_factory(const char* name, const char* mangled,
                                        const char type_args[8][128], int nargs,
                                        char* def_out, size_t def_cap);

/* Use-site production: ensure the registered factory dylib is compiled, then
 * invoke it to produce the C definition text for `mangled`.  On failure the
 * status names the failing stage and `err` holds the dynamic detail (e.g. the
 * factory compile error); the caller owns the use-site-attributed diagnostic. */
typedef enum CCGenProduceStatus {
    CC_GEN_PRODUCE_OK              = 0,
    CC_GEN_PRODUCE_ENSURE_FAILED   = 1, /* compiled factory failed to compile (err set) */
    CC_GEN_PRODUCE_INVOKE_FAILED   = 2, /* compiled factory returned empty / overflowed  */
} CCGenProduceStatus;

CCGenProduceStatus cc_emit_plan_produce_generic_def(
    const char* gname, const char* mangled, const char orig_args[8][128], int nargs,
    const char* reflect_src, size_t reflect_len, const char* input_path,
    char* def_out, size_t def_cap, char* err, size_t err_cap);
/* Emit `def_text` as an AFTER_PRELUDE fragment unless `mangled` was already
 * emitted this TU.  Returns 1 if newly added, 0 if a duplicate/full/failed. */
int cc_emit_plan_generic_def_emit_once(const char* mangled, const char* def_text);
void cc_emit_plan_clear_generic_factory_registrations(void);
/* Returns 1 the first time `mangled` is reported as producing invalid C this
 * TU, 0 thereafter — used to suppress duplicate emit-site diagnostics across
 * the preprocess and codegen rewrite passes. */
int cc_emit_plan_generic_invalid_report_once(const char* mangled);

/* --- comptime fragment buffer (track B2) --- */
void cc_emit_plan_clear_comptime_fragments(void);
size_t cc_emit_plan_comptime_fragment_count(void);
void cc_emit_plan_collect_comptime_emits(const char* src, size_t len);
void cc_emit_plan_build_comptime_schedule(const char* src, size_t len,
                                          const char* input_path,
                                          size_t insert_pos, size_t container_pos,
                                          CCEmitPlanComptimeSchedule* out);
void cc_emit_plan_fprint_comptime_fragment(FILE* out, size_t frag_index);
int cc_emit_plan_splice_comptime_fragments(char** src, size_t* len, const char* input_path);

/* Dup-emit-name detector (naming/composition): warn on duplicate top-level
 * function symbols across collected comptime fragments, naming both origins. */
void cc_emit_plan_warn_duplicate_symbols(const char* src, size_t len,
                                         const char* input_path);

/* --- comptime executor host API (Stage 0) ---
 * Injected into libtcc-compiled @comptime blocks via tcc_add_symbol. */
void cc_emit_plan_host_ctx_begin(size_t site_pos);
void cc_emit_plan_host_ctx_end(void);
void cc_emit_plan_host_emit_raw(int anchor, const char* ptr, size_t len);
void cc_emit_plan_host_emit_raw_at(int anchor, const char* file, int line,
                                   const char* ptr, size_t len);
void cc_emit_plan_host_instantiate_vec(const char* elem);
void cc_emit_plan_host_instantiate_map(const char* key, const char* val);
void cc_emit_plan_host_instantiate_result(const char* ok, const char* err);
void cc_emit_plan_host_instantiate_chan(const char* elem);
const void* cc_emit_plan_host_type_of(const char* name);

/* --- comptime reflection host API (D6.3) ---
 *
 * Structured type info crosses the user-space bind point as bytes only:
 * callers pass a type name (C string) and read field name / type-spelling
 * strings back into their own buffers.  No compiler-internal pointer (e.g.
 * `cc_type_info*`) ever escapes, so the narrow waist that keeps `@comptime`
 * code and compiled factories decoupled from compiler internals stays intact.
 * The same three symbols are injected into the libtcc executor and resolved by
 * compiled factory dylibs (via `-undefined dynamic_lookup`), so executed
 * comptime code and library factories share one reflection contract.
 *
 * Backed by an on-demand scan of the current source buffer's struct
 * definitions: the global type registry is not yet populated with struct
 * fields at the point @comptime blocks / generic factories run (see the
 * ordering in cc_build_parse_input), so reflection parses the named struct's
 * fields from source text.  `type` spellings can be fed straight back into
 * `cc_reflect_field_count` to recurse into nested struct fields.
 *
 * Returns: field count (>=0) or -1 for an unknown type; for name/type the
 * number of bytes written (excluding NUL) or -1 if idx is out of range. */
void cc_emit_plan_set_reflect_source(const char* src, size_t len);
int cc_reflect_field_count(const char* type_name);
int cc_reflect_field_name(const char* type_name, int idx, char* buf, int buf_sz);
int cc_reflect_field_type(const char* type_name, int idx, char* buf, int buf_sz);

/* Enum reflection (edge-push #1): enumerator count, name (bytes), and value
 * (scalar out-param).  Same bytes-only contract and source-scan backing as the
 * field reflection above; injected into the executor and resolved by dylib
 * factories.  Returns -1 for an unknown/unreflectable enum or out-of-range idx. */
int cc_reflect_enum_count(const char* enum_name);
int cc_reflect_enum_name(const char* enum_name, int idx, char* buf, int buf_sz);
int cc_reflect_enum_value(const char* enum_name, int idx, long long* out);

/* Type-kind classifier (edge-push #2): CC_REFLECT_KIND_* code for a type
 * spelling (recursive serde: recurse aggregates, table-map enums, leaf scalars). */
int cc_reflect_kind(const char* type_name);

/* Canonical generic mangling (naming/composition): the mangled name the
 * compiler uses for base::[args...] so library generics compose interoperably. */
int cc_canonical_name(const char* base, const char** args, int nargs,
                      char* out, int out_sz);

/* Tag-filtered reflection (edge-push #3): count / name functions carrying a
 * `@tag:NAME` marker, for building registries and dispatch tables. */
int cc_reflect_tagged_count(const char* tag);
int cc_reflect_tagged_name(const char* tag, int idx, char* buf, int buf_sz);

/* Custom domain diagnostics (edge-push #4): the dual of cc_emit_raw.  A
 * @comptime block or compiled factory raises a compiler diagnostic for a
 * constraint it checks itself; cc_emit_error fails the build, cc_emit_warning
 * does not.  Attributed to the enclosing @comptime block's source line. */
void cc_emit_error(const char* msg);
void cc_emit_warning(const char* msg);
/* Explicit-origin variants (edge-push #5): author supplies file:line. */
void cc_emit_error_at(const char* file, int line, const char* msg);
void cc_emit_warning_at(const char* file, int line, const char* msg);

/* Execute @comptime {} blocks that need the libtcc executor (control flow or
 * calls to registered @comptime functions). */
int cc_emit_plan_exec_comptime_blocks(const char* src, size_t len, const char* input_path);

#endif /* CC_EMIT_PLAN_H */
