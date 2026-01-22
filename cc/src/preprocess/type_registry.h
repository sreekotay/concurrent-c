/*
 * Type Registry for Generic Container UFCS Resolution
 *
 * Tracks variable -> type mappings during preprocessing so that UFCS calls
 * like v.push(x) can be resolved to the correct concrete function (e.g., Vec_int_push).
 *
 * Also tracks which generic type instantiations are used so that the compiler
 * can emit the necessary macro declarations (CC_VEC_DECL_ARENA, CC_DECL_OPTIONAL, etc).
 */
#ifndef CC_TYPE_REGISTRY_H
#define CC_TYPE_REGISTRY_H

#include <stddef.h>

/* Container kind for type instantiations */
typedef enum {
    CC_CONTAINER_VEC,
    CC_CONTAINER_MAP,
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

/* Generic type instantiation tracking (for emitting macro decls) */
int cc_type_registry_add_vec(CCTypeRegistry* reg, const char* elem_type, const char* mangled_name);
int cc_type_registry_add_map(CCTypeRegistry* reg, const char* key_type, const char* val_type, const char* mangled_name);
int cc_type_registry_add_optional(CCTypeRegistry* reg, const char* elem_type, const char* mangled_name);

/* Iterate over registered types for emitting declarations */
typedef struct {
    CCContainerKind kind;
    const char* mangled_name;  /* e.g., "Vec_int" or "Map_int_str" */
    const char* type1;         /* elem_type for Vec, key_type for Map */
    const char* type2;         /* NULL for Vec, val_type for Map */
} CCTypeInstantiation;

size_t cc_type_registry_vec_count(CCTypeRegistry* reg);
const CCTypeInstantiation* cc_type_registry_get_vec(CCTypeRegistry* reg, size_t idx);

size_t cc_type_registry_map_count(CCTypeRegistry* reg);
const CCTypeInstantiation* cc_type_registry_get_map(CCTypeRegistry* reg, size_t idx);

size_t cc_type_registry_optional_count(CCTypeRegistry* reg);
const CCTypeInstantiation* cc_type_registry_get_optional(CCTypeRegistry* reg, size_t idx);

/* Thread-local global registry for use during preprocessing */
CCTypeRegistry* cc_type_registry_get_global(void);
void cc_type_registry_set_global(CCTypeRegistry* reg);

#endif /* CC_TYPE_REGISTRY_H */
