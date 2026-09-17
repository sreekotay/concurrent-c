# The join's error and the dead-handler rule (seed 0.4.0-414)

Spec §8.11.1 since seed 413: an immediate-wait join raises `CCError`,
"so the frame needs an `@errhandler(CCError)` for the record to reach."
Seed 412's rule: a handler no unwrap in its scope reaches is refused.

| Probe | Result |
|-------|--------|
| `join_handler.ccs`: `@errhandler(CCError e) cc_error_exit(e);` plus `@parallel { a = leaf(); b = leaf(); } !>.wait()!>;` with arms that raise nothing | refused: "handles an error nothing in its scope raises" |
| `join_nohandler.ccs`: the same without the handler | compiles and runs |

So the join's own two unwraps (`CCParallel !>(CCError)` create and
`.wait()`) do not count as a raise of `CCError` for the dead-handler
rule, and without a handler they resolve to the construct's default
(`cc_error_exit`) rather than to "no matching handler." The two rules
disagree about whether a join raises. `perf/parallel_pow2.shcc` on
main does not compile on 414 for this reason (`pow2_par`, `pow2_cut`).
