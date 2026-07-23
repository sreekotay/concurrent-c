/*
 * variant_lower.h — `@variant` first-class tagged unions (spec/draft_variants.md).
 *
 * Phase-1 canonical text passes:
 *   - cc_rewrite_variant_decls_text: `@variant Name { arm: Type; ... };` at
 *     file scope → `typedef enum {...} NameKind; typedef struct Name {...} Name;`
 *     (+ a Name__cc_drop helper when any arm type has a registered destructor),
 *     and populates the per-TU variant registry.
 *   - cc_rewrite_variant_uses_text: the trapped consumption surface —
 *     designated-init construction (tag auto-fill), braced assignment,
 *     bare designators in comparison/assignment position, subject-switch and
 *     designator case labels (+ exhaustiveness), protected projection
 *     (domination / `!>` handler / `?>` fallback), read-only `.kind`,
 *     transition drop, and scope-exit drop.
 *
 * Both return NULL (no change), (char*)-1 (diagnostics already printed),
 * or a fresh malloc'd buffer.  All rewrites are physical-line-neutral.
 */
#ifndef CC_VARIANT_LOWER_H
#define CC_VARIANT_LOWER_H

#include <stddef.h>

char* cc_rewrite_variant_decls_text(const char* src, size_t n, const char* input_path);
char* cc_rewrite_variant_uses_text(const char* src, size_t n, const char* input_path);

/* Number of @variant declarations collected by the most recent decls pass
 * (per-thread).  The uses pass is a no-op when this is zero. */
size_t cc_variant_registry_count(void);

#endif /* CC_VARIANT_LOWER_H */
