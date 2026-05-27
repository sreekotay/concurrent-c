/*
 * pass_check_type_of.h — compile-time diagnostic for unregistered
 * `type_of(T)` / `cc_type_of("T")` calls.
 *
 * At runtime, `cc_type_of("Foo")` returns NULL if no
 * `cc_type_info` was ever registered under the mangled name
 * "Foo".  That's a sensible runtime contract, but it leaves users
 * staring at a NULL deref far from the typo.  This pass scans the
 * user TU at compile time and emits a `CC_DIAG_WARNING` for each
 * `type_of(T)` call whose `T` is none of:
 *
 *   - one of the 9 hardcoded primitives (int, char, …);
 *   - one of the 8 hardcoded pre-baked Vec typedefs (CCVec_int, …);
 *   - a generic container instantiation in this TU's
 *     `CCTypeRegistry` (Vec<T> → CCVec_T, Map<K,V> → Map_K_V);
 *   - a struct registered in this TU via
 *     CC_TYPE_INFO_BEGIN(T) / CC_TYPE_INFO_END(T, ...).
 *
 * Both the user-facing `type_of(T)` macro form AND its post-CPP
 * expansion `cc_type_of("T")` are scanned, since users may write
 * either.  Inert regions (comments, string/char literals,
 * preprocessor-directive bodies) are skipped via `CCInertScan`,
 * and matches in inlined headers (per `#line` directive) are
 * ignored — we only warn on the user's own TU.
 *
 * Tuning:
 *   CC_TYPE_OF_CHECK=0    Disable the check entirely.  Default on.
 *   CC_TYPE_OF_STRICT=1   Upgrade warnings to `CC_DIAG_ERROR`,
 *                         which makes the build fail.  Use in CI.
 *
 * Returns the number of diagnostics emitted (zero on clean input
 * or when the check is disabled).  The caller decides whether
 * `CC_DIAG_ERROR`-level diagnostics should abort the build.
 */
#ifndef CC_PASS_CHECK_TYPE_OF_H
#define CC_PASS_CHECK_TYPE_OF_H

#include <stddef.h>

typedef struct CCTypeRegistry CCTypeRegistry;

size_t cc__check_type_of_calls(const char* src,
                               size_t      n,
                               const char* input_path,
                               CCTypeRegistry* reg);

#endif /* CC_PASS_CHECK_TYPE_OF_H */
