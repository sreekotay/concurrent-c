# M6 — Type-syntax AST tolerance (deferred)

**Status:** Not started. See [COMPILER_CLEANUP_STATUS.md](../../docs/COMPILER_CLEANUP_STATUS.md).

## Goal

Promote **one** CC type construct from text rewrite / M5.5 token synthesis to a **real TCC stub-AST node** carrying `CCSourceSpan`, then retire the matching preprocess text pass.

**Recommended pilot:** channel out-handle `T[~N >]` (P4 `cc__rewrite_chan_handle_types`).

## Steps (when funded)

1. Extend TCC declarator grammar to record a stub node for `T[~N >]` / `T[~N <]`.
2. Attach `CCSourceSpan` on the node (invariant I5).
3. Lower from AST in visit_codegen (or TCC emission) instead of text scan.
4. Remove P4 text pass for that construct only.
5. Prove on:
   - `tests/channel_pair_syntax_smoke.ccs`
   - `tests/owned_channel_syntax_smoke.ccs`
   - `real_projects/pigz/pigz_idiomatic.ccs`
6. Only then consider `T[:]`, `T!>(E)`, etc.

## Prerequisites

- M5.5 macro-aware recognizer **validated** (token synthesis + macro tests green)
- M0.5 source map stable across reparses in `visit_codegen.c`
- M5 `tcc_ext_api.h` frozen for the node-kind enum extension

## Why deferred

Multi-week TCC fork work; risks reopening merge churn right after M5. M0–M5.5 infrastructure ships without M6.
