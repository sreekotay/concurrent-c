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
const char* cc_type_registry_lookup_field(CCTypeRegistry* reg,
                                          const char* struct_name,
                                          const char* field_name);
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
