/* IR differential verifier.
 *
 * During the phase-1 port of `pass_result_unwrap` (and every
 * subsequent per-pass port), we want to run both the legacy
 * text-rewrite path and the new IR-based path, and fail loudly if
 * they disagree.  This is the scaffolding for that check.
 *
 * Activation:
 *   CC_VERIFY_IR=1           (enable the check; default off)
 *   CC_VERIFY_IR_DUMP=<dir>  (also dump both outputs to <dir> on mismatch)
 *
 * Policy:
 *   - default off, so users never see extra cost;
 *   - CI flips it on for the pass-under-port only;
 *   - mismatch abort()s in debug builds and prints a diagnostic,
 *     never silently accepts either output.
 *
 * This header is deliberately tiny.  Passes that want to participate
 * call `cc_ir_verify_active()` once to gate the dual-path work, then
 * hand both outputs to `cc_ir_verify_diff()`. */
#ifndef CC_IR_VERIFIER_H
#define CC_IR_VERIFIER_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Returns non-zero iff CC_VERIFY_IR=1 is set in the environment.  Cached
 * after the first call so passes can call this freely. */
int cc_ir_verify_active(void);

/* Compare two pass outputs byte-for-byte.  `pass_name` is used in the
 * diagnostic (e.g. "result_unwrap").  `a` / `b` are the two candidate
 * outputs, `a_len` / `b_len` their lengths.
 *
 * Return value:
 *    0   outputs match (or verifier inactive — no-op)
 *   -1   outputs differ; diagnostic printed to stderr, and if
 *        CC_VERIFY_IR_DUMP is set, both outputs are written there.
 *
 * The caller decides what to do with -1.  During phase 1 the unwrap
 * pass treats it as fatal (abort in debug, EINVAL in release). */
int cc_ir_verify_diff(const char* pass_name,
                      const char* a, size_t a_len,
                      const char* b, size_t b_len);

#ifdef __cplusplus
}
#endif

#endif /* CC_IR_VERIFIER_H */
