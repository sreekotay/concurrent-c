# Concurrent-C Codegen Pipeline (authoritative)

**Last updated:** 2026-05-26 (post M0–M5.5 ship)

Authoritative call-site map for `visit_codegen.c` and `parse.c`. Status summary: [COMPILER_CLEANUP_STATUS.md](../../docs/COMPILER_CLEANUP_STATUS.md).

## Reparse count (per translation unit)

| Stage | Function | Notes |
|-------|----------|-------|
| Initial | `cc_tcc_bridge_parse_string_to_ast` | `parse.c` / codegen entry; full preprocess |
| Phase3 pre-UFCS | `cc__reparse_source_to_ast` | When `src_ufcs != src_all` after phase-2 |
| Phase3 post-UFCS | `cc__reparse_source_to_ast` | After UFCS apply (sequential path) |
| Phase3 post-closure-calls | `cc__reparse_source_to_ast` | After closure-call rewrite |
| Phase3 post-call-site-mode | `cc__reparse_source_to_ast` | `@blocking` / `@noblock` markers |
| Phase3 post-autoblock | `cc__reparse_source_to_ast` | After autoblock |
| Phase3 batched | `cc__reparse_source_to_ast` | **Only if `CC_BATCH_PHASE3=1`** — single reparse after batched apply |
| Channel/type text | buffer + parse | Channel pair + type syntax |
| Closure literal | `cc__reparse_source_to_ast` | Whole-file closure lift |
| Async SM | `cc__reparse_source_to_ast` | State machine |

**Current total:** ~9 `cc__reparse_source_to_ast` call sites (sequential default) + 1 initial parse.

**Target:** 3–4 (initial + post-Phase-3 + post-closure + post-async) after M2 default batch + M4 fine-grained edits.

## Phase 3 status

- **Default:** UFCS → reparse → closure_calls → reparse → (call-site mode) → autoblock → reparse → await_normalize (sequential). Uses `cc__apply_coarse_codegen_pass` per collector.
- **Experimental:** `CC_BATCH_PHASE3=1` runs `cc__apply_batched_phase3_passes()` (all four collectors, one apply, one reparse). Not default — caused test regressions when enabled without further AST merge work.

## Preprocess entry points (M3)

| API | Use |
|-----|-----|
| `cc_preprocess_for_initial_parse` | First parse of a TU |
| `cc_preprocess_for_reparse` | Reparse; skips validation checks (legacy `skip_checks=1`) |
| `cc_preprocess_for_light_reparse` | Reparse when phase-1 type-syntax bucket already applied |

`cc__reparse_source_to_ast` uses `cc_preprocess_for_reparse` (not light) unless stage is explicitly batched.

## Canonical prep (M1)

- **`cc_build_parse_input()`** — shared prep in `cc/src/parser/build_parse_input.c`
- **Wired in:** `parse.c`
- **Not yet wired in:** `visit_codegen.c` (still duplicates include/comptime/nursery/preprocess steps)

## Orphan / unwired components

| Component | Status |
|-----------|--------|
| `pass_nursery_spawn_ast.c` | Compiled; `cc__collect_nursery_edits` unwired — nursery in `preprocess.c` + `pass_closure_literal_ast.c` |
| `cc_preprocess_simple()` | Declared; never invoked — experimental AST path only |

## Diagnostics (M0.5)

- Module: `cc/src/diag/`
- Driver: `cc_diag_init` / `cc_diag_print_all` on compile failure
- See [DEBUG_VARS.md](../diag/DEBUG_VARS.md)
