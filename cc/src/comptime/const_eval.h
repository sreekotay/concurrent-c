#ifndef CC_COMPTIME_CONST_EVAL_H
#define CC_COMPTIME_CONST_EVAL_H

#include <stddef.h>
#include <stdint.h>

/*
 * D3.0 — in-process constant-expression evaluation via libtcc.
 *
 * Uses only the public libtcc API (no vendored-TCC changes): it compiles
 *
 *     <prelude>
 *     long long __cc_ce_result = (long long)( <expr> );
 *
 * to memory (`TCC_OUTPUT_MEMORY`), relocates, and reads back the symbol.  This
 * is the foundational seam for layout-aware comptime: because the host backend
 * computes the value, `sizeof`/`_Alignof`/`__builtin_offsetof`/enum constants /
 * integer arithmetic all evaluate with full target-ABI fidelity — the thing the
 * self-contained D2 predicate evaluator deliberately could not do.
 *
 * `prelude` may carry the declarations the expression needs (e.g. a `struct`
 * definition so `sizeof(T)` resolves), or be NULL.  `expr` must be a C integer
 * constant expression.
 *
 * Returns 1 and writes *out on success; 0 if libtcc is unavailable in this
 * build, the expression is not a constant expression, or compilation /
 * relocation / symbol lookup fails.  Never aborts; safe to probe speculatively.
 */
int cc_tcc_eval_const_expr(const char* prelude, const char* expr, int64_t* out);

#endif /* CC_COMPTIME_CONST_EVAL_H */
