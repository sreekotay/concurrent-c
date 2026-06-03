# Concurrent-C Codegen Pipeline (authoritative)

**Last updated:** 2026-06-01 (drift audit vs code: phantom `cc_preprocess_for_light_reparse` / `cc_preprocess_simple`, orphan passes) — prior: 2026-05-28 (post M4.a: Phase-5 closure-lift gating)

Authoritative call-site map for `visit_codegen.c` and `parse.c`.

> **WHY does the pipeline have this shape?** See [`ARCHITECTURE.md`](../../docs/ARCHITECTURE.md) — the four constraints, three layers, and six ADRs that constrain every part of the pipeline. Read it before proposing structural changes.

Status summary: [COMPILER_CLEANUP_STATUS.md](../../docs/COMPILER_CLEANUP_STATUS.md).

## Reparse count (per translation unit)

| Stage | Function | Conditional? | Measured hit rate (461 smoke) |
|-------|----------|--------------|-------------------------------|
| Initial | `cc_tcc_bridge_parse_string_to_ast` | always | 461 |
| Phase3 pre-UFCS | `cc__reparse_source_to_ast` | only when `src_ufcs != src_all` after phase-2 | n/a |
| Phase3 stage 1 (UFCS) | `cc__reparse_source_to_ast` | only when stage 1 produced edits | counted in `phase3` (548 total across both stages) |
| Phase3 stage 2 (post-UFCS) | `cc__reparse_source_to_ast` | only when stage 2 produced edits | counted in `phase3` |
| Channel/type text | buffer + parse | text-only, no reparse | — |
| Statement-lowering (Phase 5 closure-lift) | `cc__reparse_source_to_ast` | **gated on `=>` token presence** (2026-05-28) | **155** (was 461 unconditional pre-M4.a) |
| Async SM | `cc__reparse_source_to_ast` | gated on `@async` / `await` token presence | 74 |
| Final UFCS sweep | `cc__reparse_source_to_ast` | always (38% of invocations produce ≥1 edit; see ARCHITECTURE.md §6 fossil audit) | 393 |

**Measured totals (461 smoke suite, 2026-05-28):**

| Stage | Reparses | Notes |
|-------|----------|-------|
| `phase3` (stages 1 + 2 combined) | 548 | conditional per stage |
| `statement-lowering` (Phase 5) | 155 | gated on `=>`; was 461 pre-M4.a (−306) |
| `async-lowering` | 74 | gated on `@async`/`await` |
| `final-UFCS` | 393 | always; 150/393 produce edits |
| **Total reparses** | **1170** | **was ~1476 pre-M4.a** (−306, −21%) |
| Initial parses | 461 | always |

Typical TUs hit 4 reparses (initial + final-UFCS + maybe phase3 + maybe Phase-5/async). Feature-heavy TUs hit 5–6.

## Phase 3 status

**Default (and only) pipeline:** two-stage batched.

1. **Stage 0 (text)** — `@blocking` / `@noblock` call-site markers rewritten in-buffer if present (no reparse, no AST dependency).
2. **Stage 1 (UFCS only)** — `cc__apply_batched_phase3_passes(..., CC_PHASE3_STAGE_UFCS_ONLY)` runs `cc__collect_ufcs_edits` into a fresh `CCEditBuffer`, applies, and reparses (if any edits). Required because UFCS *produces* new conventional call sites that the stage-2 passes are AST-driven against.
3. **Stage 2 (post-UFCS)** — `cc__apply_batched_phase3_passes(..., CC_PHASE3_STAGE_POST_UFCS)` runs `cc__collect_closure_calls_edits` + `cc__collect_autoblocking_edits` + `cc__collect_await_normalize_edits` into one `CCEditBuffer`, applies, and reparses (if any edits). These three target disjoint constructs (closure-typed CALL / blocking CALL under `@async` / `await EXPR`) and emit non-overlapping per-span edits.

**Reparse cost:** 2 max in Phase 3 (down from 4 in the legacy sequential path). Each stage skips its reparse if it produced no edits.

The legacy `cc__apply_coarse_codegen_pass()` per-collector path and the `CC_BATCH_PHASE3=1` env-var gate were removed on 2026-05-28; the four collectors are the same ones the old path used.

## Phase 5 status (M4.a)

**As of 2026-05-28:** the Phase-5 closure-literal lift (`cc__rewrite_closure_literals_with_nodes`) reparse + call are gated on `cc_contains_token_top_level(src_ufcs, ..., "=>")`. TUs without closure literals skip the reparse + buffer alloc entirely. 306/461 (66%) of smoke TUs benefit.

**Why not fold Phase 5 into Phase 3 Stage 2?** Closure-literal lift is a *producer* for closure_calls (closure literals inside closure-typed call arg lists must be lowered to `__cc_closure_make_N()` before `cc__emit_call_replacement` extracts the arg text). That's the same producer/consumer pattern UFCS has — folding would require a 3rd Phase-3 stage with its own reparse barrier, net zero reparse savings. See [`ARCHITECTURE.md` §6 "Targets that aren't worth it"](../../docs/ARCHITECTURE.md).

## Cached flattened reparse prelude (2026-05-30)

Every reparse prepends a fixed prelude (`cc__prepend_reparse_prelude` in
`visitor_fileutil.c`) that `#include`s the standard + CC system headers so the
intermediate, partially-lowered source parses. Profiling (`CC_PROFILE_REPARSE`)
showed the prelude's CPP work (`cc_preprocess_emit_splice`) is negligible
(~0.04 ms) but the **TCC parse is the fixed cost (~10 ms/reparse)** — almost
entirely re-reading and re-preprocessing those headers (≈8.5k lines) on *every*
reparse, independent of user-code size.

**Optimization:** flatten the prelude **once** with `-dD` semantics
(`cc_cpp_expand_ex(..., keep_defines=1)` → `s->dflag = 3`), which expands all
`#include`s while *retaining* macro definitions and include guards, and persist
the result to a disk cache keyed on a hash of the prelude text. Subsequent
reparses prepend the already-expanded text; the retained include guards make
the user body's own `#include`s **self-skip**, so TCC never re-reads the
headers. The `#line 1 "<input>"` directive emitted after the prelude resets
coordinates for the user body, so the (much larger) flattened prelude does not
perturb AST→source mapping.

- **Per-reparse cost:** ~10 ms → ~6 ms (≈41% faster). Cold (first process)
  pays the ~12 ms flatten once; every later process/file runs warm.
- **Invalidation:** the cache filename embeds the prelude-text hash (prelude
  edits miss); the cache is rejected if any header it was built from (recovered
  from the flatten's own `#line` markers, stored in a `.deps` sidecar) is newer
  than the cache file (header edits invalidate).
- **`-dD` variadic fixup:** TCC serializes a variadic macro's parameter list as
  `(__VA_ARGS__)` instead of `(...)`. `cc__fixup_flat_va_args` repairs the
  parameter list of each `#define` (body untouched) so e.g. `__API_AVAILABLE`
  round-trips. Without this, SDK headers like `math.h` fail with "macro used
  with too many args".
- **Escape hatch:** `CC_NO_PRELUDE_CACHE=1` disables caching (uses the raw
  prelude); `CC_PRELUDE_CACHE_DIR` overrides the cache directory (default
  `$TMPDIR`).

## Preprocess entry points (M3)

| API | Use |
|-----|-----|
| `cc_preprocess_canonicalize` | Phase-1/3 canonicalization without emit-plan splice |
| `cc_preprocess_emit_splice` | Emit-plan splice for canonical buffers |
| `cc_preprocess_to_string_ex` | Full canonicalize + splice compatibility entry point |
| ~~`cc_preprocess_for_initial_parse` / `cc_preprocess_for_reparse`~~ | **Deleted (2026-06-01):** dead thin wrappers after `cc_build_parse_input` and reparses moved to the explicit canonicalize/splice APIs. |
| ~~`cc_preprocess_for_light_reparse`~~ | **Phantom (2026-06-01 audit):** never implemented. |

`cc_build_parse_input()` owns initial canonicalization. `cc__reparse_source_to_ast_ex` calls `cc_preprocess_emit_splice(..., /*skip_checks=*/1)` directly on already-canonical buffers.

## Canonical prep (M1)

- **`cc_build_parse_input()`** — shared prep in `cc/src/parser/build_parse_input.c`
- **Wired in:** `parse.c`
- **Not yet wired in:** `visit_codegen.c` (still duplicates include/comptime/nursery/preprocess steps)

## Orphan / unwired components

| Component | Status |
|-----------|--------|
| ~~`cc_preprocess_simple()`~~ | **Deleted (2026-06-01 audit):** no longer in the tree (was an unused experimental AST path). |
| ~~`cc/src/visitor/pass_with_deadline_syntax.c`~~ | **Deleted (2026-06-01):** confirmed-orphan visitor pass removed; `with_deadline` is lowered in `preprocess.c` (`cc__canonicalize_with_deadline_syntax` + `cc__lower_with_deadline_syntax`). |
| ~~`cc/src/visitor/pass_match_syntax.c`~~ | **Deleted (2026-06-01):** confirmed-orphan visitor pass removed; `@match` is lowered by the static `cc__rewrite_match_syntax` in `preprocess.c`. |

## Diagnostics (M0.5)

- Module: `cc/src/diag/`
- Driver: `cc_diag_init` / `cc_diag_print_all` on compile failure
- See [DEBUG_VARS.md](../diag/DEBUG_VARS.md)
