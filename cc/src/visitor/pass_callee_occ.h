/* One-pass index of word-bounded `name(` call sites (comment/string-safe).
 * Used by @as-arg and slice-literal coerce to avoid O(calls×src) rescans. */
#ifndef CC_VISITOR_PASS_CALLEE_OCC_H
#define CC_VISITOR_PASS_CALLEE_OCC_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef int (*CCCalleeIsDeclaratorFn)(const char* s, size_t len,
                                      size_t name_s, size_t name_n);

typedef struct CCCalleeOccIndex CCCalleeOccIndex;

/* Build from `src[0..len)`. `is_decl` may be NULL (treat none as declarators). */
CCCalleeOccIndex* cc_callee_occ_index_build(const char* src, size_t len,
                                            CCCalleeIsDeclaratorFn is_decl);
void cc_callee_occ_index_free(CCCalleeOccIndex* idx);

/* 1-based occurrence among indexed call sites for `name`. (size_t)-1 if missing. */
size_t cc_callee_occ_index_nth(const CCCalleeOccIndex* idx,
                               const char* name, int want_occ);

/* First indexed call site for `name` in [start, end). (size_t)-1 if none. */
size_t cc_callee_occ_index_first_in_range(const CCCalleeOccIndex* idx,
                                          const char* name,
                                          size_t start, size_t end);

#ifdef __cplusplus
}
#endif

#endif /* CC_VISITOR_PASS_CALLEE_OCC_H */
