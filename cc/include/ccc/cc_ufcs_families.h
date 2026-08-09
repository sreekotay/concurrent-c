/*
 * Shared UFCS family-base predicates (host preprocess + shadow lower).
 * Pure C, no runtime deps — keep lists identical for both sides.
 */
#ifndef CC_UFCS_FAMILIES_H
#define CC_UFCS_FAMILIES_H

#include <string.h>

static inline int cc_ufcs_type_is_parser_vec(const char* type_name) {
    return type_name && strncmp(type_name, "__CC_VEC", 8) == 0;
}

static inline int cc_ufcs_type_is_parser_map(const char* type_name) {
    return type_name && (strncmp(type_name, "__CC_MAP", 8) == 0 ||
                         strncmp(type_name, "__CC_ARRAY_MAP", 14) == 0);
}

/* Canonical family / mangled spellings — never valid typedef *alias keys*.
 * Registry add_alias refuses these; rewrite sites chase aliases positively
 * (lookup hit → use; miss → keep spelling). Do not grow this list for UFCS
 * dispatch — that lives in shadow tables. */
static inline int cc_ufcs_type_is_known_family_base(const char* type_name) {
    if (!type_name || !type_name[0]) return 0;
    return strncmp(type_name, "CCVec_", 6) == 0 ||
           strncmp(type_name, "ArrayMap_", 9) == 0 ||
           strncmp(type_name, "Map_", 4) == 0 ||
           strcmp(type_name, "CCString") == 0 ||
           strcmp(type_name, "CCSlice") == 0 ||
           strcmp(type_name, "CCSliceUnique") == 0 ||
           strcmp(type_name, "CCSliceShared") == 0 ||
           strcmp(type_name, "CCArena") == 0 ||
           strcmp(type_name, "CCArenaPool") == 0 ||
           strcmp(type_name, "CCArenaCheckpoint") == 0 ||
           strcmp(type_name, "CCNursery") == 0 ||
           strcmp(type_name, "CCCommand") == 0 ||
           strcmp(type_name, "CCFile") == 0 ||
           strncmp(type_name, "CCResult_", 9) == 0 ||
           strncmp(type_name, "CCChanTx_", 9) == 0 ||
           strncmp(type_name, "CCChanRx_", 9) == 0 ||
           cc_ufcs_type_is_parser_vec(type_name) ||
           cc_ufcs_type_is_parser_map(type_name);
}

/* ##_ header suffix for mangled instance bases (registry lookups stay host-side). */
static inline const char* cc_ufcs_family_header_suffix(const char* base) {
    if (!base || !base[0]) return NULL;
    if (strncmp(base, "CCSlice_", 8) == 0) return "cc_slice.cch";
    if (strncmp(base, "CCVec_", 6) == 0) return "std/vec.cch";
    if (strncmp(base, "ArrayMap_", 9) == 0) return "std/array_map.cch";
    if (strncmp(base, "Map_", 4) == 0) return "std/map_impl.cch";
    if (strncmp(base, "CCResult_", 9) == 0) return "cc_result.cch";
    return NULL;
}

#endif /* CC_UFCS_FAMILIES_H */
