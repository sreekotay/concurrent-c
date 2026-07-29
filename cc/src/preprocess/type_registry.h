/*
 * Type Registry for Generic Container UFCS Resolution
 *
 * Tracks variable -> type mappings during preprocessing so that UFCS calls
 * like v.push(x) can be resolved to the correct concrete function (e.g., CCVec_int_push).
 *
 * Also tracks which generic type instantiations are used so that the compiler
 * can emit the necessary macro declarations (CC_VEC_DECL_ARENA, CC_MAP_DECL_ARENA, etc).
 */
#ifndef CC_TYPE_REGISTRY_H
#define CC_TYPE_REGISTRY_H

#include <stddef.h>

/* Generic family kind for emitted type instantiations */
typedef enum {
    CC_CONTAINER_VEC,
    CC_CONTAINER_MAP,
    CC_CONTAINER_CHANNEL,
} CCContainerKind;

/* Type registry opaque handle */
typedef struct CCTypeRegistry CCTypeRegistry;

/* Create/destroy registry */
CCTypeRegistry* cc_type_registry_new(void);
void cc_type_registry_free(CCTypeRegistry* reg);

/* Clear all entries (for reuse between files) */
void cc_type_registry_clear(CCTypeRegistry* reg);

/* Variable type tracking */
int cc_type_registry_add_var(CCTypeRegistry* reg, const char* var_name, const char* type_name);
const char* cc_type_registry_lookup_var(CCTypeRegistry* reg, const char* var_name);
int cc_type_registry_add_alias(CCTypeRegistry* reg, const char* alias_name, const char* type_name);
const char* cc_type_registry_lookup_alias(CCTypeRegistry* reg, const char* alias_name);
size_t cc_type_registry_alias_count(CCTypeRegistry* reg);
const char* cc_type_registry_alias_name_at(CCTypeRegistry* reg, size_t idx);
const char* cc_type_registry_alias_type_at(CCTypeRegistry* reg, size_t idx);
int cc_type_registry_add_field(CCTypeRegistry* reg,
                               const char* struct_name,
                               const char* field_name,
                               const char* field_type);
/* Like add_field; when is_as!=0 marks the field as `Type name @as`.
 * Rejects a second @as of the same field_type on the same struct (-2). */
int cc_type_registry_add_field_ex(CCTypeRegistry* reg,
                                  const char* struct_name,
                                  const char* field_name,
                                  const char* field_type,
                                  int is_as);
const char* cc_type_registry_lookup_field(CCTypeRegistry* reg,
                                          const char* struct_name,
                                          const char* field_name);
/* Count / index @as fields of struct_name in declaration order. */
size_t cc_type_registry_as_field_count(CCTypeRegistry* reg, const char* struct_name);
int cc_type_registry_as_field_at(CCTypeRegistry* reg,
                                 const char* struct_name,
                                 size_t idx,
                                 const char** out_field_name,
                                 const char** out_field_type);
/* Nonzero if struct_name has any @as field. */
int cc_type_registry_has_as_field(CCTypeRegistry* reg, const char* struct_name);

/* Dynamic UFCS sink (.ufcs_dynamic registrations): mark / query types whose
 * unresolved methods lower through a registered sink callee. Session-global
 * (survives registry rebuilds across pipeline stages). */
int cc_type_registry_set_dynamic_sink(CCTypeRegistry* reg, const char* type_name,
                                      const char* callee, const char* wrap);
int cc_type_registry_get_dynamic_sink(CCTypeRegistry* reg, const char* type_name,
                                      const char** out_callee,
                                      const char** out_wrap);
int cc_type_registry_has_dynamic_sink(CCTypeRegistry* reg, const char* type_name);
/* .ufcs_dynamic2: destination-aware sink — wherever a typed destination
 * is visible (`T name = recv.method(…)`, an assignment to a resolvable
 * lvalue, or a cast `(T)recv.method(…)`) the callee composes as
 * <callee>_<mangled dest> when declared. */
int cc_type_registry_set_dynamic_sink2(CCTypeRegistry* reg, const char* type_name,
                                       const char* callee, const char* wrap);
int cc_type_registry_dynamic_sink_dest_aware(CCTypeRegistry* reg,
                                             const char* type_name);
/* Typed slice instances (CC_DECL_SLICE_SPEC family). The rewriter
 * registers every instance it names; lookup recovers the member prefix
 * (the instance name itself: CCSlice_double_at, the same NAME##_
 * convention as Vec/Map) and element spelling. Session-global. Returns
 * 0 on hit, -1 otherwise; trailing `*` on type_name is ignored. */
int cc_slice_spec_register(const char* name, const char* prefix, const char* elem);
int cc_slice_spec_lookup(const char* type_name,
                         const char** out_prefix, const char** out_elem);
size_t cc_slice_spec_count(void);
int cc_slice_spec_get(size_t i, const char** out_name, const char** out_elem);
/* 1 when `elem` is one of the scalar pre-instances cc_slice.cch always
 * declares (so no TU splice is needed). */
int cc_slice_spec_elem_is_prebaked(const char* elem);
/* Slice instance for a container element type: "double" ->
 * "CCSlice_double" when that instance is known declared (a scalar
 * pre-instance of cc_slice.cch, or a session-registered user spec).
 * Returns 0 and writes the instance name, -1 otherwise. */
int cc_slice_spec_instance_for_elem(const char* elem,
                                    char* name_out, size_t name_sz);
/* Unique `@as` field of `outer` whose field type matches `want_type`
 * (cv/`struct ` stripped; pointer stars on want_type ignored).
 * Direct embeds only. Returns 0 and sets *out_field_name; -1 none; -2 ambiguous. */
int cc_type_registry_as_field_for_type(CCTypeRegistry* reg,
                                       const char* outer_type,
                                       const char* want_type,
                                       const char** out_field_name);
/* Transitive `@as` path from `outer` to `want_type` (dotted, e.g. `mid.file`).
 * Returns 0 unique; -1 none; -2 ambiguous; -3 cycle in the @as graph. */
int cc_type_registry_as_path_for_type(CCTypeRegistry* reg,
                                      const char* outer_type,
                                      const char* want_type,
                                      char* path_out,
                                      size_t path_cap);
/* Nonzero if `outer` has any @as field transitively (cycle-safe). */
int cc_type_registry_has_as_field_transitive(CCTypeRegistry* reg,
                                             const char* outer_type);
/* Decl-time check: from each type with `@as` fields, every reachable embed
 * type must appear via exactly one path; cycles are ill-formed. Pointer
 * `@as` fields are ill-formed. Prints `file:line:col: error: type: …`
 * diagnostics (line best-effort from src). Returns 0 if clean, -1 if any
 * diagnostic was emitted. */
int cc_type_registry_validate_as_graphs(CCTypeRegistry* reg,
                                        const char* file,
                                        const char* src,
                                        size_t n);
/* Nonzero when `src` may need `@as` arg-position coerce: local comment-marker
 * `@as`, live `@as`, or a registered `@as` outer type name appears in `src`. */
int cc_type_registry_may_need_as_arg_coerce(CCTypeRegistry* reg,
                                            const char* src,
                                            size_t n);
/* Nonzero when `field_name` is the first registered field of `struct_name`
 * (declaration-order proxy for offset-zero). */
int cc_type_registry_field_is_first(CCTypeRegistry* reg,
                                    const char* struct_name,
                                    const char* field_name);
/* Scan `typedef struct { ... } Name;` bodies in src and register fields,
 * including `Type name @as` / comment-marker form. Safe to call repeatedly. */
void cc_type_registry_ingest_struct_fields(CCTypeRegistry* reg,
                                           const char* src,
                                           size_t n);
/* If `field_name` appears on exactly one registered struct (or all hits
 * share one field type), return that field type; otherwise NULL. */
const char* cc_type_registry_lookup_unique_field_type(CCTypeRegistry* reg,
                                                     const char* field_name);
const char* cc_type_registry_resolve_receiver_expr(CCTypeRegistry* reg,
                                                   const char* recv_expr,
                                                   int* out_recv_is_ptr);
const char* cc_type_registry_resolve_receiver_expr_at(CCTypeRegistry* reg,
                                                      const char* recv_expr,
                                                      const char* source_text,
                                                      size_t use_offset,
                                                      int* out_recv_is_ptr);
const char* cc_type_registry_resolve_expr_type(CCTypeRegistry* reg, const char* expr);
const char* cc_type_registry_lookup_channel_elem_type(CCTypeRegistry* reg, const char* handle_type_name);
const char* cc_type_registry_canonicalize_type_name(CCTypeRegistry* reg,
                                                    const char* type_name,
                                                    char* out,
                                                    size_t out_sz);

/* Generic type instantiation tracking (for emitting macro decls) */
int cc_type_registry_add_vec(CCTypeRegistry* reg, const char* elem_type, const char* mangled_name);
int cc_type_registry_add_map(CCTypeRegistry* reg, const char* key_type, const char* val_type, const char* mangled_name);
int cc_type_registry_add_channel(CCTypeRegistry* reg, const char* elem_type, const char* mangled_name);

/* Iterate over registered types for emitting declarations */
typedef struct {
    CCContainerKind kind;
    const char* mangled_name;  /* e.g., "CCVec_int" or "Map_int_str" */
    const char* type1;         /* elem_type for Vec, key_type for Map */
    const char* type2;         /* NULL for Vec, val_type for Map */
} CCTypeInstantiation;

size_t cc_type_registry_vec_count(CCTypeRegistry* reg);
const CCTypeInstantiation* cc_type_registry_get_vec(CCTypeRegistry* reg, size_t idx);

size_t cc_type_registry_map_count(CCTypeRegistry* reg);
const CCTypeInstantiation* cc_type_registry_get_map(CCTypeRegistry* reg, size_t idx);

size_t cc_type_registry_channel_count(CCTypeRegistry* reg);
const CCTypeInstantiation* cc_type_registry_get_channel(CCTypeRegistry* reg, size_t idx);

/* Thread-local global registry for use during preprocessing.
 *
 * DEPRECATED for NEW code (2026-05-26): do not add new
 * `cc_type_registry_get_global()` callers.  Thread an explicit
 * `CCTypeRegistry*` parameter through the caller's plumbing
 * instead.  See the "type-registry ratchet" rule in
 * `cc/src/visitor/PASS_INVENTORY.md`.  These functions stay
 * because ~50 existing call sites depend on them; the goal is to
 * ratchet that number down, not let it grow.
 *
 * The implicit stack semantics: `cc_type_registry_set_global`
 * REPLACES the current registry; callers wanting nested scoping
 * must save+restore the prior pointer themselves OR use the
 * `cc_type_registry_scope_push/pop` helpers below. */
CCTypeRegistry* cc_type_registry_get_global(void);
void cc_type_registry_set_global(CCTypeRegistry* reg);

/* Scoped helpers for the global registry.
 *
 * Consolidates the save / new / set / restore / free dance that
 * was open-coded in ~9 call sites.  `_push` allocates a fresh
 * registry, saves the prior global into `scope`, and installs the
 * new one.  `_pop` restores the prior and frees the temp.  Both
 * are safe to call when allocation fails (push returns 0, pop is
 * a no-op).
 *
 * Typical use:
 *
 *     CCTypeRegistryScope scope;
 *     if (cc_type_registry_scope_push(&scope)) {
 *         // ... call code that uses the (now temp) global ...
 *         cc_type_registry_scope_pop(&scope);
 *     }
 */
typedef struct {
    CCTypeRegistry* saved;
    CCTypeRegistry* temp;
} CCTypeRegistryScope;

int  cc_type_registry_scope_push(CCTypeRegistryScope* scope);
void cc_type_registry_scope_pop(CCTypeRegistryScope* scope);

#endif /* CC_TYPE_REGISTRY_H */
