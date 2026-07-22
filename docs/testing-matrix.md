# Test-coverage matrix: PRs #109–#130

Audit artifact (2026-07-22). For every major fix/feature merged in PRs #109–#130,
this matrix records: the change, its positive pin(s), its negative pin(s) where the
change has a rejection/diagnostic surface, the gap verdict at audit time, and what
the audit added. "N/A" in the negative column means the change has no
rejection/diagnostic surface to pin, with the reason. All paths are repo-relative.

Method: `git log` on main for `(#109)`..`(#130)` subjects; each squash commit's
message and diff were read and cross-checked against `tests/` and `scripts/`.

## Matrix

| PR | Change | Positive pin(s) | Negative pin(s) | Gap verdict / action |
|----|--------|-----------------|-----------------|----------------------|
| #109 | Reparse sanitizers made line-neutral (UFCS dead-band root cause); fatal parity guard | `tests/ufcs_below_multiline_unwrap_smoke.ccs` (+`.stdout`); `scripts/test_reparse_sanitize.sh` (wired in `scripts/test.sh`, verified failing on pre-fix compiler per PR) | N/A — the guarded failure is an internal coordinate-skew class; the parity guard aborts the compiler itself (FAIL LOUDLY), not a user-facing diagnostic that user code can trigger | No gap. NOTE: the smoke's oracle shipped as `.expected_stdout` (never compared); #110 renamed it to `.stdout` — mis-pin existed for one PR |
| #110 | UFCS `occ` counts same-method occurrences per line; phantom template-twin dedupe (tcc recorder patch) | `tests/ufcs_same_line_occ_smoke.ccs` (+`.stdout`): two methods on one line, same method twice, two methods in one `@string` template line (off=-1 occurrence-probe path) | N/A — a miscount has no rejection surface (the only diagnostic is the loud 128-entries/line recorder-table overflow warning, not reachable by reasonable user code); the skew class is caught structurally by #109's fatal parity guard | No gap |
| #111 | Redis idiomatic command surface (expiry, variadic, arrays) + functional smoke | `real_projects/redis/redis_smoke.py` (all commands, expiry, 1000-op pipeline, RST teardown storm) | error paths (non-integer INCR, bad arity, unknown command) are inside the smoke | **GAP: smoke was never gated** — `scripts/test.sh` never ran it; all command semantics rode manual runs. **Added `scripts/test_redis_functional.sh`** (wired): builds `redis_idiomatic` (ccc cache ok), runs the smoke once. Smoke measured at ~0.3 s, so no reduced-op knob was needed; port collisions made safe by a new `--port 0` free-ephemeral-port mode added to `redis_smoke.py` |
| #112 | Autoblock: definition wins TU-locally for `@noblock` (+ lying-decl warning) | `tests/autoblock_noblock_def_wins_smoke.ccs`, `..._def_wins_inline_smoke.ccs`, `..._decl_promises_def_breaks_smoke.ccs`, `..._decl_and_def_smoke.ccs` (all failing-first per PR) | `scripts/test_autoblock_noblock_warn.sh` (wired): warning text + both coords + exactly-once + no spurious warnings | No gap |
| #113 | Accurate `#line` coordinates inside `@async` state machines (ledger-aware user lines) | `scripts/test_async_line_map.sh` (wired): real redis port `--emit-c-only`, asserts async poll fns are `#line`-mapped and no coordinate passes source EOF (pre-fix: 927 lines past EOF) | `tests/diag_oracle_async_body_fail.ccs` (+`.compile_err`): exact file:line for a host-compiler error inside an `@async` body | No gap |
| #114 | Emits that printed error diagnostics fail and are never cached | `scripts/test_diag_cache_replay.sh` (wired): two consecutive CACHED builds of the failing fixture must both fail and both print the diagnostic; verified failing pre-fix per PR | the script IS the negative oracle (asserts failure + diagnostic text) | No gap |
| #115 | WRITE projection for `one of` tagged-union schemas | `tests/grammar_schema_union_write_smoke.ccs` (+`.stdout`): five RESP variants, nested arrays, round-trip, cap overflow, invalid kind | `tests/grammar_schema_union_write_unsupported_fail.ccs` (+`.compile_err`): directive-items in a variant refuse with per-variant diagnostic | No gap |
| #116 | `scripts/apply_tcc_patches.sh` auto-resets stale patch state | **none** ("Exercised against current/stale/pristine states" was manual only) | n/a (script behavior) | **GAP: no test. Added `scripts/test_tcc_patch_apply.sh`** (wired): (1) current tree → no-op, (2) STALE — the previous patch version from git history (`5d12b81` at audit time; resolved dynamically) applied to a pristine tree, script must auto-reset and apply cleanly, (3) pristine → applies; asserts reverse-check cleanliness after each leg; `trap` restores the correct tree state regardless of outcome; no network (git history only, with a synthetic-stale fallback for shallow clones). Failing-first verified: with `apply_tcc_patches.sh` from `af5ecec~1`, the stale leg fails ("apply on STALE tree exited nonzero") |
| #117 | RESP reply encoding through generated schema writers | `[wire parity]` section of `redis_smoke.py` (byte-exact replies: nil, empty bulk, empty array, mixed MGET, CONFIG GET) + `tests/grammar_schema_union_*` | invalid-kind → loud 0-return pinned in `grammar_schema_union_write_smoke` | **GAP: wire-parity section not gated** (same as #111) — closed by `scripts/test_redis_functional.sh` |
| #118 | Spec draft: `@variant` design | N/A — spec-only (`spec/draft_variants.md`) | N/A | No code surface |
| #119 | Spec draft v2: drop `@match`, protected projection + checked switch | N/A — spec-only | N/A | No code surface |
| #120 | Remove `@match` statement; reserve keyword with migration error | migrated `tests/match_recv_smoke.ccs`, `tests/match_send_smoke.ccs`, `tests/chan_select_cancel_close_stale_smoke.ccs` (direct `cc_chan_match_select`) | `tests/match_removed_fail.ccs` (+`.compile_err`) pins the removal diagnostic | No gap |
| #121 | Multi-declarator decls no longer corrupt UFCS receiver types | `tests/chan_multi_declarator_ufcs_smoke.ccs` (+`.stdout`), failing-first per PR | N/A — fix removes a spurious compile error on valid code; there is no new rejection surface | No gap |
| #122 | Delete dead `CC_STRICT_DEADLOCK` knob; align docs | `scripts/test_cli.sh` (wired): `--help` must not advertise the knob | same script: `--strict-deadlock` must be REJECTED like any unknown flag (fails on pre-deletion driver) | No gap |
| #123 | sched_v2 fiber-recycle races: completer UAF (saved_nursery read after done=1) + free-list ABA + join re-publish | `tests/hybrid_run_to_completion_smoke.ccs` (the original carrier, ~5 fiber lifecycles/run — repro was 1/200 isolated, 86/1200 only under external 4-way suite load); rewritten `tests/nursery_spawn_placement_workers4_smoke.ccs` (load-robust placement residue) | N/A — runtime race, no diagnostic surface | **GAP: the gate ran no load**, so the fixes were effectively unpinned in CI. **Added `tests/fiber_recycle_hybrid_stress_smoke.ccs`** (+`.stdout`): 3 pthread loops × 20,000 rounds of the hybrid shape (2 `cc_block_on` task frees immediately before 3 nursery `spawnhybrid` recycles) with 8 workers oversubscribing a 4-core host = 300,000 fiber lifecycles (~60,000× the carrier's exposure) in ~1.2 s. Failing-first VERIFIED empirically: with `cc/runtime/sched_v2.c` rebuilt from `add6223~1` (pre-#123; the file has no other changes since), 5/5 runs failed inside the 20 s timeout with the original signature ("[sched_v2] BUG: got fiber in state 0/4", then hang) |
| #124 | Spec: variant constructors, type-scoped dot form | N/A — spec-only | N/A | No code surface |
| #125 | Spec: variants v3, designated-init construction | N/A — spec-only | N/A | No code surface |
| #126 | Spec: withdraw `@fmt` | N/A — spec-only | N/A | No code surface (its surviving residue — fixed-arena exhaustion pin — landed in #129) |
| #127 | Spec: arena-less `@string` bounded-template stack form | N/A — spec-only (implementation + pins landed in #129) | N/A | No code surface |
| #128 | Spec: no value-yielding `!>` | N/A — spec-only | N/A | No code surface |
| #129 | Stdlib+compiler riders: `to_i64` family, checked i64 math, arena-less `@string` | `tests/slice_to_i64_smoke.ccs` (incl. parse/range error Results), `tests/checked_math_i64_smoke.ccs` (incl. overflow errors), `tests/string_tpl_stack_smoke.ccs`, `tests/string_tpl_fixed_arena_exhaustion_smoke.ccs` (sticky-poison contract) | `tests/string_tpl_stack_unbounded_fail.ccs` (+`.compile_err`) — but it pinned ONLY the slice interpolation type | **GAP (partial): the unbounded-diagnostic oracle covered one of the four arena-legal-but-stack-illegal types. Added** `tests/string_tpl_stack_unbounded_float_fail.ccs`, `..._ccstring_fail.ccs`, `..._ptr_fail.ccs` (+`.compile_err` each; all verified to emit the "has no statically bounded width … pass an arena" diagnostic) **and the positive counterpart** `tests/string_tpl_arena_unbounded_types_smoke.ccs` (+`.stdout`): the arena form accepts slice, CCString, double/float, and `const char*` — "pass an arena" is a real fix, not a dead end |
| #130 | Buffered channel: gate direct handoff on empty ring (redis pipeline reply reorder) | `tests/channel_buffered_handoff_fifo_smoke.ccs` (+`.stdout`), failed 5/5 pre-fix per PR; end-to-end shape now also gated by `scripts/test_redis_functional.sh` (1000-op pipeline) | N/A — runtime ordering fix, no diagnostic surface | No gap in the unit pin; the end-to-end redis pipeline pin was unguarded until the #111/#117 gap fix |

## Summary

- 22 PRs audited; 6 are spec-only (no code surface): #118, #119, #124–#128.
- 16 code-bearing changes; 4 had gaps (#111+#117 counted as one gating gap, #116, #123, #129-partial).
- Added by this audit:
  - `scripts/test_tcc_patch_apply.sh` (wired into `scripts/test.sh`; skips loudly if the tcc submodule is uninitialized, restores tree state via trap)
  - `scripts/test_redis_functional.sh` (wired; `redis_smoke.py` gained `--port 0` collision-safe port picking; no reduced-op knob needed — the smoke runs in ~0.3 s, the only cost is the cached server build)
  - `tests/fiber_recycle_hybrid_stress_smoke.ccs` + `.stdout` (bounded #123 load pin; pre-fix runtime fails 5/5, fixed runtime 0 failures)
  - `tests/string_tpl_stack_unbounded_{float,ccstring,ptr}_fail.ccs` + `.compile_err`, `tests/string_tpl_arena_unbounded_types_smoke.ccs` + `.stdout`
- Failing-first status of the additions:
  - tcc patch-apply: verified failing against `scripts/apply_tcc_patches.sh` from `af5ecec~1` (stale leg fails).
  - fiber-recycle stress: verified failing against `cc/runtime/sched_v2.c` from `add6223~1` (5/5 fail, original signature).
  - redis functional gate: not failing-first-able as a whole (it gates previously-untested behavior; its components were verified against live bugs in #130: pre-fix, the pipeline leg flaked with shifted replies per that PR's 22/300 repro).
  - string-tpl fail siblings: negative oracles — verified to produce the pinned diagnostic today; the guarded behavior (the no-default `_Generic` + stderr-replay rewrite) landed in #129 and cannot be toggled without reverting it.
- Mis-pins / over-claims found:
  - #109 shipped its smoke oracle as `.expected_stdout`, which `tools/cc_test` never reads — the stdout comparison never engaged until #110 renamed it (also affected `inert_async_frame_comment_smoke`).
  - #116 claimed "Exercised against current/stale/pristine states" — true only as a manual, unrecorded exercise; nothing in the gate covered it until this audit.
  - #111/#117/#129/#130 all cite `redis_smoke.py` greens as gate evidence, but the smoke was never wired into `scripts/test.sh`.
  - #123's "gates green" claim was accurate but vacuous for the fix itself: the suite contained no load-shaped reproducer (the carrier test needed external 4-way suite load to trip at 86/1200).

## Gate cost

Baseline full suite before additions: 584 tests, ~44 s wall (4-core container,
warm compiler build). After additions: 589 tests + 2 new script hooks; the new
wall-time cost is ~5–7 s total (fiber-recycle stress ~1.2 s inside the parallel
harness; redis functional ~3 s warm-cache; tcc patch-apply ~2 s; the string-tpl
tests are ordinary compile smokes/fails).
