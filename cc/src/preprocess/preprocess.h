#ifndef CC_PREPROCESS_H
#define CC_PREPROCESS_H

#include <stddef.h>

typedef struct CCSymbolTable CCSymbolTable;
typedef struct CCTypeRegistry CCTypeRegistry;

/* Compile-time struct-field reflection — the single field parser shared by
 * `@comptime for (f in type_of(T).fields)` and the `cc_reflect_field_*` host
 * callback.  Parses the declared members of the struct/typedef `type_name`
 * (a typedef name, or a `struct Tag`/`union Tag` spelling) from `src`.
 *
 * Models scalars/pointers, multi-declarators, arrays (incl. multi-dim),
 * function pointers, and named bitfields; the `type` spelling carries pointer
 * stars, array extents (`int[2][3]`), and function-pointer signatures
 * (`int (*)(int)`).
 *
 * Returns 1 and sets *out (free with cc_ct_free_fields) and *out_n on success.
 * Returns 0 if the type is not found OR any member uses a form not modeled
 * (inline anonymous/nested aggregate def, anonymous member, unnamed bitfield,
 * pointer-to-array): callers must treat 0 as "no reflection" — every field or
 * none, never a partial or guessed result. */
/* `params` is the method's parenthesized parameter list, verbatim, and `member`
 * the name it answers to after the dot; both NULL for a field.  Reflection
 * carries text, so the list is the source's own spelling. */
typedef struct CCCtField {
    char* name; char* type; int is_as; char* params; char* member;
} CCCtField;
int cc_ct_reflect_struct_fields(const char* src, size_t len, const char* type_name,
                                CCCtField** out, size_t* out_n);

/* Methods of `type_name`: the functions in `src` whose first parameter is `T`
 * or `T*`, in declaration order.  `name` is the function's, `type` its declared
 * return-type spelling (`!>(E)` included), `params` its parameter list verbatim.
 * Returns 0 when the type has no method, the same all-or-nothing posture the
 * field reader has. */
int cc_ct_reflect_type_methods(const char* src, size_t len, const char* type_name,
                               CCCtField** out, size_t* out_n);

/* One entry per declared parameter of a parenthesized list (`"(T self, int x)"`),
 * in order.  Returns 0 when an entry is unnamed or spelled in a form the
 * declarator parser cannot model — every parameter or none, since a dropped one
 * would silently renumber the rest. */
int cc_ct_reflect_param_list(const char* params, CCCtField** out, size_t* out_n);
/* Default literal for params[idx] (`pad = 1` → `"1"`).  0 = none; -1 = bad. */
int cc_ct_reflect_param_default(const char* params, int idx, char* buf, int buf_sz);
void cc_ct_free_fields(CCCtField* fields, size_t n);

/* The file's in-scope type definitions as a compilable prelude, for contexts
 * that must resolve a user type outside the merged TU — the `@comptime if`
 * layout evaluator and the generated-fragment validator.  Caller frees. */
char* cc_ct_extract_type_decls_prelude(const char* src, size_t n);

/* Enum reflection (edge-push #1).  Parses the enumerators of the enum `type_name`
 * (a typedef name, or an `enum Tag` spelling) from `src`, with C auto-increment
 * semantics (first = 0, each subsequent = prev + 1 unless an explicit value is
 * given).  Explicit values must be *integer literals* (decimal/hex/octal, with
 * an optional sign and integer suffix); a non-literal initializer
 * (`A | B`, `1 << 2`, `'c'`, …) makes the whole enum unreflectable — like the
 * struct reflector, the contract is every member or none, never a guess.
 *
 * Returns 1 and sets *out (free with cc_ct_free_enum_members) and *out_n on
 * success; 0 if the enum is not found or a member is not modeled. */
typedef struct CCCtEnumMember { char* name; long long value; } CCCtEnumMember;
int cc_ct_reflect_enum_members(const char* src, size_t len, const char* type_name,
                               CCCtEnumMember** out, size_t* out_n);
void cc_ct_free_enum_members(CCCtEnumMember* members, size_t n);

/* Type-kind classifier (edge-push #2).  Classifies a type spelling so a
 * recursive serializer/serde generator can decide whether to recurse
 * (aggregate), table-map (enum), pointer-handle, or emit a scalar leaf.  Uses
 * the body-finders (not the field parser), so a struct with an unmodeled
 * member still classifies as STRUCT.  Leading const/volatile are tolerated;
 * cv-qualified aggregate spellings and typedef-to-primitive aliases are not
 * resolved (report UNKNOWN) — kept deliberately small for v1.  Keep these
 * values in sync with the CC_REFLECT_KIND_* constants in cc_instantiate.cch. */
typedef enum CCReflectKind {
    CC_REFLECT_KIND_UNKNOWN   = 0,
    CC_REFLECT_KIND_PRIMITIVE = 1,
    CC_REFLECT_KIND_POINTER   = 2,
    CC_REFLECT_KIND_STRUCT    = 3,  /* struct/union/typedef-aggregate */
    CC_REFLECT_KIND_ENUM      = 4,
} CCReflectKind;
int cc_ct_reflect_type_kind(const char* src, size_t len, const char* type_name);

/* Canonical generic name mangling (naming/composition).  Produces the exact
 * mangled name the compiler uses for `base::[args...]`, so libraries that mangle
 * privately can compose with each other (and with the built-ins) instead of
 * each inventing an incompatible scheme.  Recipe: join `base` and each
 * type arg with '_'; per arg, canonicalize the spelling then sanitize
 * (`*`->`ptr`, `[:]`->`slice`, `[ ] , < >`->`_`, drop interior whitespace,
 * trim trailing `_`).  See cc/docs/GENERIC_MANGLING.md.  Returns the written
 * length, or -1 on truncation/bad args. */
int cc_ct_canonical_name(const char* base, const char* const* args, int nargs,
                         char* out, size_t out_sz);

/* Tag-filtered declaration reflection (edge-push #3).  Opt-in, advertent
 * marker: a single-line comment carrying the token `@tag:NAME` (in either a
 * block or line comment) immediately preceding a top-level function definition
 * tags that function.  This scan
 * collects the names of all functions carrying `tag`, in source order, so a
 * @comptime block can build a registry / dispatch table from them.  No new
 * syntax or parser surface — purely a source-scan (surface ≡ lowering).
 * Returns 1 on success (out_names = strdup'd array, possibly empty), 0 on OOM. */
int cc_ct_reflect_tagged_fns(const char* src, size_t len, const char* tag,
                             char*** out_names, size_t* out_n);
void cc_ct_free_tagged_fns(char** names, size_t n);

// Preprocess a CC source file, rewriting CC syntax (e.g., UFCS) into
// plain C that TCC can parse. Writes to a temporary file path (returned via
// out_path), nul-terminated. Returns 0 on success; caller must unlink the
// temp file when done.
int cc_preprocess_file(const char* input_path, char* out_path, size_t out_path_sz);

// Preprocess source string to output string (no temp files).
// Returns malloc'd string on success, NULL on error. Caller must free().
char* cc_preprocess_to_string(const char* input, size_t input_len, const char* input_path);

// Same as cc_preprocess_to_string but with option to skip validation checks.
// Use skip_checks=1 for reparse passes where checks already ran on original source.
char* cc_preprocess_to_string_ex(const char* input, size_t input_len, const char* input_path, int skip_checks);

#include "source_pipeline.h"

/* Phase-1+3 only (no emit-plan splice). */
char* cc_preprocess_canonicalize(const char* input, size_t input_len, const char* input_path,
                                 int skip_checks, int skip_comptime_surface);
/* Emit-plan splice only; input must already be canonical. */
char* cc_preprocess_emit_splice(const char* input, size_t input_len, const char* input_path,
                               int skip_checks);
/* Coordinate accounting for the most recent emit-splice call (reparse diet):
 * insertions land at known input-coordinate anchors, so for input text AFTER
 * `last_anchor`, out_off = in_off + delta.  `user_rewritten` = the input
 * bytes themselves changed (system-include lowering) — anchors meaningless. */
void cc_pp_get_splice_coord_info(size_t* last_anchor, long* delta, int* user_rewritten);

// Expand a source file through the host C preprocessor so local and stdlib
// headers appear in one include-expanded stream with line markers preserved.
// Process-local memo + disk cache (~/.cache/concurrent-c/incexp/); disable
// disk with CC_INCEXP_NO_CACHE=1. Returns malloc'd string on success, NULL on
// error. Caller must free().
char* cc_preprocess_include_expanded(const char* input_path);

// Build the include-expanded CC source buffer consumed by comptime registration
// discovery (type registrations, UFCS hooks). Line markers are preserved.
//
// This intentionally does NOT run phase-1 sugar lowering: registrations live in
// already-lowered header harvest / user @comptime text. The main TU still gets
// full phase-1 via build_parse_input.
//
// Returns malloc'd string on success, NULL on error. Caller must free().
char* cc_preprocess_comptime_source(const char* input_path);

// Rewrite quoted local .cch includes to stable lowered .h files under
// out/include/ so parser and final host C compilation do not see raw project
// headers with CC-only syntax such as @comptime blocks. Returns malloc'd
// string on success or NULL when no rewrite was needed. Caller must free().
char* cc_rewrite_local_cch_includes_to_lowered_headers(const char* src,
                                                       size_t input_len,
                                                       const char* input_path);
char* cc_rewrite_system_cch_includes_to_lowered_headers(const char* src,
                                                        size_t input_len);
void cc_reset_included_cch_sources(void);

/* Enumerate .cch paths registered for this TU (harvest / include rewrite).
 * Paths are absolute; valid until the next cc_reset_included_cch_sources. */
size_t cc_included_cch_source_count(void);
const char* cc_included_cch_source_path(size_t i);

/* Ingest `typedef struct …` fields (including `Type name @as`) from every
 * .cch registered for this TU into `reg`.  Headers stay as `#include` in the
 * host buffer, so destroy/UFCS must pull @as metadata from this side channel. */
void cc_ingest_included_cch_struct_fields(CCTypeRegistry* reg);

/* Nonzero if any registered included .cch contains callable `name(`. */
int cc_included_cch_contains_fn(const char* name);
/* First parameter type of a decl-shaped `name(` occurrence in an
 * included cch header, whitespace-normalized. Returns 0/1. */
int cc_included_cch_fn_first_param(const char* name, char* out, size_t out_sz);
/* Family member sets derived from a declaration-form macro's body: the
 * `##_<member>` tokens in the included cch header whose path ends with
 * `header_suffix`. Membership test and comma-separated enumeration. */
int cc_family_header_has_member(const char* header_suffix, const char* method);
const char* cc_family_header_members(const char* header_suffix);
/* UFCS resolution package (member trust): single oracle for
 * generic-family instances (slice/vec/map/result/channel). Text gates
 * and AST composed-spelling trust both call these — do not add a second
 * allowlist. header_for → family .cch suffix (NULL for handle allowlists);
 * has_member → derived members only; accepts → member or declared
 * Base_method extension (includes; use _in_tu when TU text is in hand);
 * members_for → CSV for strict-ladder notes. */
const char* cc_ufcs_family_header_for(const char* base);
int cc_ufcs_family_has_member(const char* base, const char* method);
const char* cc_ufcs_family_members_for(const char* base);
int cc_ufcs_family_accepts(const char* base, const char* method);
int cc_ufcs_family_accepts_in_tu(const char* base, const char* method,
                                 const char* src, size_t n);
/* True when `base` is a generic-factory instance emitted this TU (member
 * set derived from the fragment; see cc_emit_plan_note_generic_instance). */
int cc_ufcs_generic_instance_known(const char* base);
/* Reset per-TU dest-trap dedup (also cleared with included-cch sources). */
void cc_ufcs_reset_dest_trap_dedup(void);
/* Map key hash/eq selection: declared convention
 * (cc_map_key_hash_<mangled K> / cc_map_key_eq_<mangled K>) outranks
 * the built-in table; unknown keys error articulately. Returns 0 on
 * an installed pair, 1 on the error fallback. */
int cc_map_key_hasheq(const char* key_type, char* hash_out, size_t hs,
                      char* eq_out, size_t es);
/* _ex: *out_tu_static = -1 when the pair is not TU-declared; else the
 * definition's staticness, for the forward prototypes the container
 * decl must emit above itself (convention: size_t hash(K), int
 * eq(K, K)). */
int cc_map_key_hasheq_ex(const char* key_type, char* hash_out, size_t hs,
                         char* eq_out, size_t es, int* out_tu_static);
/* Note TU-declared key pairs (decl-shaped hash+eq twins) so selection
 * sees pairs declared in the translation unit itself. */
void cc_note_tu_map_key_pairs(const char* src, size_t n);
/* Nonzero when the TU must splice the declaration for slice instance
 * `name` (element `elem`): non-prebaked element and no hand-written
 * CC_DECL_SLICE_SPEC/CC_DECL_SLICE in the TU or an included cch. */
int cc_slice_spec_tu_needs_decl(const char* src, size_t n,
                                const char* name, const char* elem);

/* Splice known local lowered headers (`out/include/.../*.h` from quoted .cch)
 * into the codegen/UFCS buffer so phase3 sees their bodies with parent-TU
 * symbols.  Returns malloc'd text, or NULL when unchanged. Caller frees. */
char* cc_splice_local_lowered_headers_for_codegen(const char* src, size_t n);

/* After phase3 UFCS: write marked splice regions back to their .h files and
 * collapse each region to `#include "path"`.  Updates *src/*n in place
 * (may realloc).  Returns 0 on success, -1 on I/O failure. */
int cc_writeback_local_lowered_headers_from_codegen(char** src, size_t* n);

// Map an emitted lowered-header path (out/include/.../X.h) back to the .cch
// source the user actually wrote, using the local-header lowering registry.
// Returns a borrowed pointer (owned by the registry) or NULL if `lowered_path`
// is not a known lowered local header. Used to restore source provenance in
// `#line`/`# N "file"` markers so diagnostics in header content blame the .cch.
const char* cc_lowered_header_source_for(const char* lowered_path);

/* ---- Header comptime harvest --------------------------------------------
 * lower_header blanks `@comptime` / CC_GENERIC_FACTORY from `.cch` → `.h` so
 * the lowered header stays host-C. The including TU re-appends the raw forms
 * (with `#line` back to the `.cch`) before comptime prepare/exec so factories,
 * `@comptime` functions, and emit-producing `@comptime { }` blocks (e.g.
 * `static_map`) still run. Call after local `.cch` includes are rewritten /
 * registered. Each returns malloc'd text to append, or NULL. Caller frees.
 *
 *   factories  — local headers only (g_lowered_local_headers)
 *   functions  — local + system included .cch (g_included_cch_sources)
 *   blocks     — local headers only; skips `@comptime if/for` and functions
 * ------------------------------------------------------------------------ */

char* cc_harvest_local_header_factories(void);
char* cc_harvest_header_comptime_functions(void);
char* cc_harvest_local_header_comptime_blocks(void);

// Shared header-safe type-syntax lowering used by both preprocessing and
// `.cch -> .h` lowering. Rewrites syntax that must not leak into plain C
// headers, such as slice, typed channel handles, and generic container types.
char* cc_rewrite_header_type_syntax_shared(const char* src,
                                           size_t input_len,
                                           const char* input_path);

// Rewrite `@slice(...)`, `@string(...)`, and backtick template literals in a
// source fragment used by later text-based lowering/codegen passes.
char* cc_rewrite_string_templates_text(const char* src, size_t n, const char* input_path);
/* `@string(...)` receivers: capture the template and its first member call
 * into a typed temp so the ident receiver resolves on the normal UFCS
 * rails in every position. Run before template lowering. */
char* cc_normalize_template_recv_chains_text(const char* src, size_t n);

/* Arena-less `@string(`...`)` slot registry: the template rewrite records
 * each lowered slot's (file, line, text); the TCC stderr replay uses the
 * lookup to rewrite "does not match any association" errors into the
 * bounded-template diagnostic naming the interpolation and suggesting an
 * arena (spec/draft_variants.md §9.2).  Lookup returns a joined
 * "'${a}' / '${b}'" list for that file basename + line, or NULL. */
void cc_string_stack_tpl_note_slot(const char* file, int line, const char* expr, size_t expr_len);
const char* cc_string_stack_tpl_slots_for(const char* file, int line);

// Lower `CC_GENERIC_FACTORY(Name) { ... }` sugar into a `cc_generic_register`
// registration plus a `@comptime` factory function. Returns NULL when the
// source has no occurrence, or (char*)-1 on a malformed header.
char* cc_rewrite_generic_factory_text(const char* src, size_t n, const char* input_path);
/* @grammar(engine) Name {SENT...SENT} -> synthesized @comptime engine call.
 * NULL = no declarations; (char*)-1 = malformed (diagnostic printed). */
char* cc_rewrite_grammar_decls_text(const char* src, size_t n, const char* input_path);

/* `Type.method(args)` -> `Type_method(args)` (type-scoped calls). Textual and
 * pre-parse: a type name in expression position is a C syntax error, so the
 * UFCS pass never gets to see it. Rewrites only when the lowered Type_method
 * is visibly used/declared in this file's text. NULL = no changes. */
char* cc_rewrite_type_scoped_calls_text(const char* src, size_t n);

/* `static_map(name, entries, flags)` → typed internal call with inferred
 * value type + layout (sizeof / address offsets).  NULL = no 3-arg sites;
 * (char*)-1 = malformed (diagnostic printed). */
char* cc_rewrite_static_map_calls_text(const char* src, size_t n, const char* input_path);

/* Scan a backtick template literal starting at tick_pos (which must point at '`').
 * On success sets *tick_end_out to the closing backtick index and returns 0. */
int cc_scan_template_literal_end(const char* src, size_t n, size_t tick_pos, size_t* tick_end_out);

// Rewrite @link("lib") directives to marker comments for linker extraction.
// Returns newly allocated string, or NULL if no rewrites needed.
char* cc__rewrite_link_directives(const char* src, size_t n);

// Rewrite generic container syntax:
//   CCVec::[T] -> CCVec_T, Map<K,V> -> Map_K_V
//   cc_vec_new::[T](&arena) -> CCVec_T_init(&arena, CC_VEC_INITIAL_CAP)
//   map_new<K,V>(&arena) -> Map_K_V_init(&arena)
// Returns newly allocated string with rewrites, or NULL if no changes.
char* cc_rewrite_generic_containers(const char* src, size_t n, const char* input_path);

// Parser-only text rewrite for concrete generic family UFCS. The AST UFCS
// pass is authoritative for all family UFCS; this rewriter remains as a
// narrow parser-survival aid so the stub-AST parser sees lowered receiver
// forms for fragile nested contexts (Vec methods inside printf args,
// CCCommand/CCFile calls recorded inconsistently by TCC, etc.).
char* cc_rewrite_generic_family_ufcs_parser_safe(const char* src, size_t n,
                                                const char* input_path);

// [Removed] cc_rewrite_channel_ufcs_concrete: channel UFCS is now handled by
// the AST UFCS pass via the CCChanTx/CCChanRx registered hooks.  The AST
// dispatch also retains a small cc_ufcs_channel_callee helper for the raw
// CCChan/untyped-alias cases that still need sizeof(*out_ptr) insertion.

// Prototype rewrite for builtin nursery declarations:
//   CCNursery* n = @create(parent, closure) @destroy;
//   CCNursery* n = @create(parent, closure) @destroy { ... };
// Returns newly allocated string on change, NULL on no-op, (char*)-1 on error.
char* cc_rewrite_nursery_create_destroy_proto(const char* src, size_t n, const char* input_path);
char* cc_rewrite_nursery_create_destroy_proto_ex(const char* src,
                                                 size_t n,
                                                 const char* input_path,
                                                 CCSymbolTable* symbols);

// Rewrite @await fname(...) -> cc_block_on(ReturnType, fname(...)) in any context.
// Collects @async function return types from the source, then rewrites each
// @await call site to cc_block_on.  Unknown callees have @await stripped and
// the expression is emitted as-is.
// Returns newly allocated string on change, NULL if no @await present.
char* cc__rewrite_at_await(const char* src, size_t n);

// Rewrite call-site `@blocking callee(args)` / `@noblock callee(args)` /
// `@nonblocking callee(args)` to
// leave a survivable marker comment for pass_autoblock:
//   @blocking callee(args)  -> CC_SITE=blocking marker + callee(args)
//   @noblock callee(args)   -> CC_SITE=noblock marker + callee(args)
//   @nonblocking { ... }    -> CC_BLOCK=noblock marker + { ... }
// This only fires at call-expression positions; decl-level `@blocking` /
// `@noblock` (e.g. `@blocking int foo(...)`) is left intact so the TCC
// cc-ext hook can record the bit on the function decl node. Decl-level
// `@nonblocking` is canonicalized to `@noblock` for the same hook.
// Returns newly allocated string on change, NULL if no rewrite applied.
char* cc__rewrite_at_call_site_mode(const char* src, size_t n);

// M7.C: re-run header-safe CC type-syntax lowerings (channel handle types,
// slice types, generic containers, string templates) on a buffer that has
// already passed through the main preprocess. Unlike
// `cc_rewrite_header_type_syntax_shared`, this variant does NOT clear or
// reset the global type registry, so it is safe to call after phase-1+phase-3
// preprocessing or after `cc_cpp_expand` to mop up CC syntax produced by
// macro expansion (e.g. `#define CHAN(T) T[~4 >]` invoked as `CHAN(int) tx;`
// becoming `int[~4 >] tx;` post-CPP). Returns malloc'd string on changes,
// NULL when no rewrite was needed. Caller must free().
char* cc_relower_cc_type_syntax_preserving_registry(const char* src,
                                                    size_t input_len,
                                                    const char* input_path);

// Rewrite `@async [<attrs>] void <name>(...)` to `@async [<attrs>] CCAsyncVoidRet <name>(...)`.
// Keeps the function visible to phase-3 reparse as a CCTaskIntptr-returning
// function (needed so that call-sites like `n->spawn_async(fn(args))` type-check
// against `cc_nursery_spawn_async(CCNursery*, CCTask)`). The async lowering in
// `async_ast.c` recognises the `CCAsyncVoidRet` marker as an originally
// `@async void` declaration and keeps bare `return;` valid in the body.
// Returns newly allocated string on change, NULL if no rewrite applied.
char* cc__rewrite_async_void_ret(const char* src, size_t n);

// D1.0 — constexpr `type_of(T)` view: fold `type_of(T).size` -> `sizeof(T)` and
// `type_of(T).align` -> `_Alignof(T)` (each `(size_t)`-cast), so they are usable
// as integer constant expressions (static_assert / array dims / @comptime if).
// The bare value form `type_of(T)` and the pointer form `type_of(T)->m` are
// left untouched (runtime `cc_type_of` semantics).  Must run on BOTH the
// preprocess-for-parse path and the visit_codegen emit path (the emitted .c is
// produced by the latter).  Returns malloc'd string on change, NULL otherwise.
char* cc__lower_type_of_constexpr(const char* src, size_t n);

// D2.0: resolve `@comptime if (PRED) { ... } [else { ... }]` by evaluating the
// compile-time-constant predicate and splicing the taken branch in place (the
// rest dropped, newline-padded for stable line numbers).  Returns a
// malloc'd string on change, NULL if no `@comptime if` present, or (char*)-1
// on a hard error (non-constant predicate / unsupported `else if`), after
// printing a diagnostic to stderr.
char* cc__resolve_comptime_if(const char* src, size_t n, const char* input_path);

// Value-position `@comptime(expr)`: evaluate `expr` at compile time and splice
// its value as a C constant-expression literal in place (integers, floating
// point, bool, strings).  Returns a malloc'd string on change, NULL if no value
// form is present, or (char*)-1 on a hard error (after printing a diagnostic).
char* cc__resolve_comptime_value(const char* src, size_t n, const char* input_path);

#include "preprocess/comptime_prepare.h"

#endif // CC_PREPROCESS_H
