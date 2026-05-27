#ifndef CC_L2_REWRITER_H
#define CC_L2_REWRITER_H

#include <stddef.h>

/* L2 prelude pre-parse rewriter.
 *
 * Converts a small, controlled set of *standard C* idioms that
 * TCC's stub-AST parser rejects into TCC-acceptable equivalents,
 * applied as the LAST text transform in `cc_build_parse_input`
 * just before the buffer is handed to the parser.
 *
 * The motivation is PASS_INVENTORY rule #7 ("user-facing surface
 * ratchet"): compiler back-end limitations should not leak into
 * user source code.  Without this rewriter, a user writing the
 * perfectly standard:
 *
 *     #include <stddef.h>
 *     ...
 *     size_t off = offsetof(MyStruct, field);
 *
 * sees a TCC parse error ("lvalue expected") because TCC's stub
 * AST builder doesn't recognize the libc `offsetof` macro inside
 * expression contexts.  Rewriting `offsetof(T,F)` to GCC's
 * `__builtin_offsetof(T,F)` (which TCC wires directly into the
 * expression parser) makes the standard form Just Work.
 *
 * Current idioms handled:
 *
 *   1. `offsetof(T, F)`                  -> `__builtin_offsetof(T, F)`
 *
 *   2. `__attribute__((constructor(N)))` -> `__attribute__((constructor))`
 *      (drops the priority arg; ordering across TUs is already
 *      unspecified for unprioritized constructors per the GCC
 *      docs, and CC's type registry uses content-addressed dedup
 *      so priority would be redundant noise even if TCC accepted
 *      it.  Same treatment for `destructor(N)`.)
 *
 * Both rewrites:
 *   - Respect string / char-literal / comment / preprocessor-body
 *     boundaries (via `CCInertScan` from `util/text_scan.h`).
 *   - Preserve newlines one-to-one within the replaced region so
 *     downstream `#line` directive and `(line, col)` diagnostics
 *     stay accurate.  Column numbers within a rewritten line may
 *     shift by a few bytes.
 *   - Are pure-textual; no parsing required.
 *
 * Returns a newly malloc'd buffer on rewrite, NULL on alloc
 * failure, or `NULL` (with `*out_len = src_len`) when no idiom
 * was matched (caller continues with the original buffer).  The
 * non-rewrite path is the fast common case for code that's
 * already TCC-friendly.
 *
 * Adding a new idiom: each rewrite is a single function in
 * `cc_l2_rewriter.c` invoked from `cc_l2_rewrite_all` — copy the
 * existing pattern, add a corresponding smoke test that USES the
 * standard form, and document it in the bullet list above. */
char* cc_l2_rewrite_all(const char* src, size_t src_len, size_t* out_len);

#endif /* CC_L2_REWRITER_H */
