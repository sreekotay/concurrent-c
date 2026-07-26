/*
 * variant_lower.h — `@variant` first-class tagged unions (spec/draft_variants.md)
 * and schema `one of` protected projection (spec/cc_serdes.md).
 *
 * Phase-1 canonical text passes:
 *   - cc_rewrite_variant_decls_text: `@variant Name { arm: Type; ... };` at
 *     file scope → `typedef enum {...} NameKind; typedef struct Name {...} Name;`
 *     (+ a Name__cc_drop helper when any arm type has a registered destructor),
 *     and populates the per-TU variant registry.  Also commits schema `one of`
 *     types queued via cc_variant_schema_pending_add (same surface rules).
 *   - cc_rewrite_variant_uses_text: the trapped consumption surface —
 *     designated-init construction (tag auto-fill), braced assignment,
 *     bare designators in comparison/assignment position, subject-switch and
 *     designator case labels (+ exhaustiveness), protected projection
 *     (domination / `!>` handler / `?>` fallback), read-only `.kind`,
 *     raw `.u` ban, transition drop, and scope-exit drop.
 *
 * Both return NULL (no change), (char*)-1 (diagnostics already printed),
 * or a fresh malloc'd buffer.  All rewrites are physical-line-neutral.
 */
#ifndef CC_VARIANT_LOWER_H
#define CC_VARIANT_LOWER_H

#include <stddef.h>

char* cc_rewrite_variant_decls_text(const char* src, size_t n, const char* input_path);
char* cc_rewrite_variant_uses_text(const char* src, size_t n, const char* input_path);

/* Number of tagged-union types in the per-TU registry after the decls pass
 * (@variant + committed schema one-of).  Uses pass is a no-op when zero. */
size_t cc_variant_registry_count(void);

/* Schema grammar emit queues `one of` types here; the variant decls pass
 * commits them into the registry (after any @variant decls).  Arm names are
 * copied.  is_void[i] != 0 marks a unit arm (literals only). */
void cc_variant_schema_pending_clear(void);
int cc_variant_schema_pending_add(const char* name, int narms,
                                  const char* const* arm_names,
                                  const int* is_void);

#endif /* CC_VARIANT_LOWER_H */
