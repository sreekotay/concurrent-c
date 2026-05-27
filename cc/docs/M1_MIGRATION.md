# M1 — Visitor refactor (migration tracker)

**Status:** Phase 1 (audit) complete — 2026-05-27.  Phase 2 in progress (17 / 18 batches landed — A, B, C, D1, D2, E, F1, F2, G, H1, I1, I2, J, J3, K1, K2, K3a).  Remaining: K3b, L, M.

This is the living artifact for the M1 visitor refactor.  Every M1 commit
updates the appropriate batch in this file: tick off the migrated sites,
move them from "pending" to "done", record any surprises in the post-mortem
section, adjust the next batch if needed.

See [COMPILER_CLEANUP_STATUS.md](./COMPILER_CLEANUP_STATUS.md) (M1 callout
at line 76 + "Recommended next work" item 2) for the strategic framing,
and [text_scan.h](../src/util/text_scan.h) for the migration helper.

---

## Goal

Three sub-pieces, in order:

| Piece | What | State |
|-------|------|-------|
| **(a)** Source-buffer unification | swap `src_all = file` → `src_all = root->parse_buffer` when pre-expand is on | **TODO** — blocked on (c) |
| **(b)** Reparse prelude awareness | `CCReparseFlags.src_is_pre_expanded` so reparse skips re-prepending headers | **DONE** (M7.C3 plumbing) |
| **(c)** `#line`-aware text scanners | every visitor pass that walks `src_all` filters by origin file via `CCInertScan` | **PARTIAL** — 101 sites migrated, 1 remaining (K3b: `cc__infer_closure_end_off`) |

Why it matters: M1 unblocks four otherwise-stalled items —
macro CC-syntax end-to-end (CHAN test stops being a curiosity), retiring
the `CC_PRE_EXPAND=0` opt-out, deleting redundant `_cch → _h` text
passes, and fixing the `m0_5_diag_origin_line_fail` source-map drift.
None of those move until (c) is complete enough to support (a)'s flip.

---

## Migration template

Canonical examples to mirror:

- **Find-only** scanner: `cc/src/visitor/pass_result_unwrap.c::cc__find_bang_token_from`.
  Replaces ~50 LOC of inline state machine with ~10 LOC of helper calls.
- **Rewrite + `in_user_file` filter**: `cc/src/visitor/pass_check_type_of.c`.
  Born `CCInertScan`-native; demonstrates the output-buffer + filter pattern.

The mechanical migration:

1. `#include "util/text_scan.h"` (or `#include "../util/text_scan.h"` depending on path).
2. Replace inline state vars (`in_lc`/`in_bc`/`in_str`/`qch`/`in_pp`/...) with one `CCInertScan scan; cc_inert_scan_init(&scan, ctx->input_path);`.
3. At loop top: `if (cc_inert_scan_step(&scan, src, n, &i)) { /* rewrite: copy verbatim; find-only: continue */; continue; }`.
4. Convert `for (i=...; i<n; i++)` → `while (i < n)` with explicit `i++`s where the original implicitly advanced.  `cc_inert_scan_step` advances `i` by 1 or 2+ bytes; a `for`-loop's post-increment would double-advance.
5. Optional: `if (!scan.in_user_file) { /* skip CC-token match */; i++; continue; }` — a no-op today (always 1) until Phase 4 flips.

**Watch-outs:**

- **`at_line_start = 0` after init** when the input is a mid-buffer slice rather than a full file (Batch A surprise — `cc__line_has_await_keyword` quirk).  A leading `#` in a slice is code, not a pp directive.
- **Newline tracking in inert regions** — for rewrites that maintain `last_line_off`, `line`, or `col`, sweep the consumed `[before, i)` range after each `cc_inert_scan_step` that returned 1 (`for (size_t k = before; k < i; k++) if (src[k] == '\n') ...`).
- **Code-path newline** — `cc_inert_scan_step` returns 0 for `\n` outside any inert region and leaves `i` pointing AT the newline.  Any rewrite that tracks `line` or `col` must also handle `if (c == '\n') { line++; col = 1; i++; continue; }` on the code-byte path (Batch C surprise — `m0_5_diag_channel_pair_origin_fail` regression).
- **Inner sub-scanners** in bounded regions (paren groups, expression tails) — usually str/chr-only "no comments" scanners.  Batch B left these inline deliberately; reasonable default.  Migrate if a regression surfaces.
- **Body-`continue;` infinite-loop trap** (Batch F1 — caught by smoke).  Per-keyword loops where the original used `for (i=0; i<n; ++i)` and the body had an `if (...) continue;` (e.g. "skip if this looks like a function call, not a decl") — in the original, `continue` triggered the for's `++i`.  After migrating to `while (i < n)` with a tail `i += type_len ;` advance, the `continue;` skips that advance and re-tests the same position forever.  Fix: invert the condition into an `if (!(...)) { ... rest of body ... }` guard so the tail advance always runs.  Audit the body BEFORE migrating any per-keyword loop.
- **Stale scanner state after big jumps** (Batch G — `cc__pu_find_outer_errhandler` post-match `i = rbrace + 1;`).  When the migrated loop jumps `i` past a body that the scanner never saw (e.g. `i = rbrace + 1` skips a whole `@errhandler { ... }` body), the scanner's `in_block_comment` / `in_pp` / `in_user_file` flags reflect state at the PRE-jump position.  If the body contained an unterminated comment (impossible by definition, since `cc_find_matching_brace` is comment-aware) or a `#line` directive (possible!), the scanner state is stale.  Safe today because no Batch-G site reads `scan.in_user_file` after a jump.  Watch-out for future passes: if a post-jump code path reads any `scan.*` flag, `cc_inert_scan_init(&scan, ...)` to reset before continuing.

Today: zero behavior change for happy-path rewrites.  After Phase 4: scanners ignore inert content AND header-origin tokens.  One incidental fix landed already: error-message column off-by-one in `cc__rewrite_channel_pair_calls_text` (test expectations updated in the same commit).

---

## Status snapshot (running tally)

Updated 2026-05-27 (post Batch K3a).

| Metric | Count |
|--------|-------|
| Total scanner sites (inline + migrated) | **102** |
| Already on `CCInertScan` | **101** (+3 from Batch K3a) |
| Remaining to migrate | **1** |
| — complex | 1 (K3b: `cc__infer_closure_end_off`) |

Plus 3 backward scanners (Batch L) and 1 special case (Batch M) outside the 102-site forward-scan count.

Smoke at last batch close: **461/461** full suite (default mode); 382/382 when filtered to `_smoke` subset (both modes).

---

## Per-file audit (Phase 1 deliverable)

State-var codes: `lc` = in_line_comment, `bc` = in_block_comment, `str` = in_str, `chr` = in_chr, `qch` = quote char, `pp` = in_pp, `ppc` = pp_continued, `als` = at_line_start.

### `pass_channel_syntax.c` (2215 LOC, 7 sites, now uses `text_scan.h`)

| Site | Shape | State | LOC | Complexity | Notes |
|------|-------|-------|-----|------------|-------|
| `cc__record_chan_handle_alias_decls` (~152) | find-only | {lc,bc,str,chr} | ~16 | trivial | **✓ Migrated (Batch C)** |
| `cc__scan_matching_brace` (~401) | find-only | {lc,bc,str,chr} | ~24 | trivial | **✓ Migrated (Batch C)** — `depth` stays alongside |
| `cc__rewrite_channel_pair_calls_text` (~944) | rewrite | {lc,bc,str,chr} | ~20 | trivial | **✓ Migrated (Batch C)** — `line`/`col` preserved (inert sweep + code-path `\n` handler); incidentally fixes a +1 column off-by-one (anchored test `m0_5_diag_channel_pair_origin_fail.compile_err` updated 21:19 → 21:18) |
| `cc__rewrite_chan_handle_types_text` (~1295) | rewrite | {lc,bc,str,chr,pp,ppc,als} | ~55 | medium | **✓ Migrated (Batch C)** — `CCInertScan` does the full pp trio natively, so this collapses to the same simple shape as the other rewrites |
| `cc__find_matching_delim` (~1715) | find-only | {lc,bc,str,chr} | ~18 | trivial | **✓ Migrated (Batch C)** — `depth` stays alongside |
| `cc__find_arrow` (~1737) | find-only | {lc,bc,str,chr} | ~16 | trivial | **✓ Migrated (Batch C)** |
| `cc__extract_block_tail_expr` (~1787) | find-only | {lc,bc,str,chr} | ~25 | medium | **✓ Migrated (Batch C)** — `par`/`sq`/`br` stay alongside; for-loop replaced with `while` to accommodate step's variable advance |

### `pass_closure_literal_ast.c` (3875 LOC, 12 sites, no `text_scan.h`)

| Site | Shape | State | LOC | Complexity | Notes |
|------|-------|-------|-----|------------|-------|
| `cc__scan_skip_string_comment` (~1696) | step helper | {str,chr,lc,bc} — no pp | ~35 | trivial | **Precursor**: migrate first; callers (arrow finders, mutation scan) then collapse |
| `cc__infer_closure_end_off` (~234) | find-only | dual nested | ~75 | **complex** | Expr-body (no comments!) + block-body (lc/bc).  Real `=>`-in-comment bug history |
| 4× anon loops in `cc__maybe_record_decl_stmt` (~578/600/628/658) | find-only | {lc,bc,str,chr} | ~12 each | trivial | Same shape, different cursor vars |
| `cc__maybe_record_decl` (~779) | find-only | {lc,bc,str,chr} | ~12 | trivial | Finds `;`/`{` |
| `cc__last_top_level_semi_offset` (~819) | find-only | {lc,bc,str,chr} | ~25 | trivial | |
| `cc__has_top_level_brace` (~863) | find-only | {str,chr} only | ~20 | medium | **Incomplete**: no comment skip; migration must not regress |
| `cc__src_strip_comments_and_strings` (~995) | rewrite | {lc,bc,str,chr} | ~60 | trivial | Offset-preserving blank-out — clean rewrite template |
| `cc__closure_proto_insert_off` (~2194) | find-only | {lc,bc,str,chr} | ~45 | medium | Also brace_depth, last_line_off; backward-ish from closure start |
| `cc__parse_closure_from_src` body-brace scan (~2586) | find-only | {str,qch,lc,bc} | ~30 | trivial | Also `br` |

Note: `cc__find_next_arrow_skipping_inert`, `cc__find_prev_arrow_skipping_inert`, `cc__amp_is_const_param_read`, `cc__find_mutation_in_body`, recovery forward scan ~2888 all DELEGATE to `cc__scan_skip_string_comment` — migrating the helper migrates them all.

### `pass_type_syntax.c` (1152 LOC, 7 sites, now uses `text_scan.h`)

| Site | Shape | State | LOC | Complexity | Notes |
|------|-------|-------|-----|------------|-------|
| `cc__rewrite_slice_types_text` (~84) | rewrite | {lc,bc,str,chr} | ~20 | trivial | **✓ Migrated (Batch B)** — `line`/`col` preserved via post-step sweep; original had a small off-by-one on the column of the first char after a newline, migration incidentally fixes it |
| `cc__scan_for_existing_result_types` (~244) | find-only | {lc,bc,str,chr} | ~20 | trivial | **✓ Migrated (Batch B)** |
| `cc__rewrite_result_types_text` (~445) | rewrite | {lc,bc,str,chr} | ~20 | trivial | **✓ Migrated (Batch B)** outer scanner; inner str/chr-only paren-balanced scanner for error-type extraction left alone (bounded, no comments expected inside `!>(...)`) |
| `cc__rewrite_result_field_sugar_text` pass 1 (~584) | find-only | {lc,bc,str,chr} | ~20 | trivial | **✓ Migrated (Batch B)** |
| `cc__rewrite_result_field_sugar_text` pass 2 (~665) | rewrite | {lc,bc,str,chr} | ~20 | trivial | **✓ Migrated (Batch B)** |
| `cc__rewrite_inferred_result_constructors` (~782) | rewrite | {lc,bc,str,chr} | ~25 | medium | **✓ Migrated (Batch B)** outer scanner; `brace_depth`/`fn_brace_depth`/`current_result_type[256]` stayed alongside; inner str/chr-only `cc_ok(...)`/`cc_err(...)` arg-split scanner left alone (bounded paren scan, "no comments" pre-existing) |
| `cc__rewrite_try_exprs_text` (~1062) | rewrite | {lc,bc,str,chr} | ~20 | trivial | **✓ Migrated (Batch B)** outer scanner; inner str/chr-only expression-end scanner left alone (similar bounded "no comments" pattern) |

### `pass_create.c` (534 LOC, 3 sites, now uses `text_scan.h`)

| Site | Shape | State | LOC | Complexity | Notes |
|------|-------|-------|-----|------------|-------|
| `cc__create_find_top_level_comma` (~47) | find-only | {lc,bc,str,chr} | ~25 | trivial | **✓ Migrated (Batch D1)** — `par`/`brk`/`br` stay alongside; `for` → `while` |
| `cc__create_build_arg_slices` (~93) | find-only | {str,chr} | ~40 | medium | **✓ Migrated (Batch D1)** — **behavior change**: now comment-aware (was deliberately not).  Synthetic trailing-`,` trick (loop `i <= n` with `(i < n) ? c : ','`) replaced with explicit post-loop trailing-arg emit so `cc_inert_scan_step`'s `[0,n)` contract is honored.  `at_line_start = 0` for mid-buffer slice |
| `cc__create_seed_registered_var_types` (~179) | find-only | {lc,bc,str,chr} | ~20 | trivial | **✓ Migrated (Batch D1)** — `CCInertScan` re-init INSIDE per-type outer loop; loop-tail increment changed from `i += type_len - 1` (relying on `++i` from `for`) to `i += type_len` (explicit) |

### `async_ast.c` (3652 LOC, 12 sites, no `text_scan.h`)

| Site | Shape | State | LOC | Complexity | Notes |
|------|-------|-------|-----|------------|-------|
| `cc__find_matching_paren` (~203) | find-only | {str,qch,lc,bc} | ~22 | trivial | |
| `cc__find_matching_brace` (~227) | find-only | {str,qch,lc,bc} | ~22 | trivial | Mirror of paren helper |
| `cc__rewrite_idents` (~251) | rewrite | {str,chr} | ~45 | medium | **No comment skip** — flag for review |
| `cc__rewrite_typed_chan_await_expr` (~339) | find-only | {str,chr} | ~15 | medium | No comments; also `depth` |
| `cc__scan_simple_stmt_end` (~718) | find-only | {str,qch,lc,bc} | ~22 | trivial | |
| `cc__truncate_at_first_semicolon0` (~1032) | find-only | {str,qch,lc,bc} | ~22 | trivial | |
| `cc__split_top_level_semis` (~1068) | collect | {str,qch,lc,bc} | ~35 | medium | `@errhandler` tail interleave |
| anon loop in `cc__parse_loop_from_text` (~1389) | find-only | {str,qch,lc,bc} | ~25 | medium | For-header `;` splitter |
| `cc__rhs_has_top_level_comma` (~1645) | find-only | {str,qch,lc,bc} | ~22 | trivial | |
| `cc__emit_awaits_in_expr` (~1780) | rewrite | dual nested | ~80 | **complex** | Rewrite + per-`await` operand scanner.  Highest-risk site in file |
| `cc__normalize_result_generic_bool_calls` (~1876) | rewrite | {str,chr} | ~25 | medium | No comments |
| anon loop in `cc_async_rewrite_state_machine_ast` (~3170) | find-only | {lc,bc,str,chr} | ~25 | medium | Per-line type extraction |

### `pass_ufcs.c` (900 LOC, 1 site, now uses `text_scan.h`)

| Site | Shape | State | LOC | Complexity | Notes |
|------|-------|-------|-----|------------|-------|
| `cc__line_has_await_keyword` (~106) | find-only | {str,qch} + in_blk_cmt | ~28 | trivial | **✓ Migrated (Batch A)** |

### `pass_match_syntax.c` (712 LOC, 7+ sites, now uses `text_scan.h`)

| Site | Shape | State | LOC | Complexity | Notes |
|------|-------|-------|-----|------------|-------|
| `cc__parse_lowered_match_call` inner (~65) | find-only | {str,qch} + par/brk/brc | ~15 | medium | **✓ Migrated (Batch D1)** — **behavior change**: now comment-aware (was deliberately not).  Inner paren-scan inside hdr slice; `at_line_start = 0` |
| `cc__rewrite_match_syntax` main loop (~95) | rewrite | {lc,bc,str,chr} | ~25 | medium | **✓ Migrated (Batch D1)** — `line`/`col` preserved (inert sweep + code-path `\n` handler).  Re-counted: 4+ nested inner scanners (not 2 as audit said); migrated 3 of them, left the bounded "no comments" single-stmt `;` scanner inline (see Batch D1 below) |
| anon loop in `cc__rewrite_match_syntax` (~147) — body-brace matcher | find-only | {str,qch,lc,bc} | ~15 | trivial | **✓ Migrated (Batch D1)** — `br` stays alongside; `for` → `while` |
| anon loop in `cc__rewrite_match_syntax` (~195) — case header `:` scanner | find-only | {str,qch,lc,bc} | ~22 | trivial | **✓ Migrated (Batch D1)** — `par`/`brk2`/`br2` stay alongside; hand-rolled `//`/`/*` inner skip replaced by `CCInertScan` |
| anon loop in `cc__rewrite_match_syntax` (~223) — case body brace matcher | find-only | {str,qch,lc,bc} | ~12 | trivial | **✓ Migrated (Batch D1)** — `brr` stays alongside |
| anon loop in `cc__rewrite_match_syntax` (~238) — case body single-stmt `;` scanner | find-only | {str,chr} | ~12 | trivial | **Deferred** — bounded paren scan, no comments expected; mirrors Batch B's "inner scanner" decision |
| `cc__collect_match_edits` main loop (~415) | collect | {lc,bc,str,chr} | ~25 | medium | **✓ Migrated (Batch D2)** — `line`/`col` preserved (same pattern as the rewrite path); collect path stays semantically equivalent (no `out` buffer to manage) |
| anon loop in `cc__collect_match_edits` (~460) — body-brace matcher | find-only | {str,qch,lc,bc} | ~15 | trivial | **✓ Migrated (Batch D2)** — `br` stays alongside; mirror of rewrite ~147 |
| anon loop in `cc__collect_match_edits` (~507) — case header `:` scanner | find-only | {str,qch,lc,bc} | ~22 | trivial | **✓ Migrated (Batch D2)** — `par`/`brk2`/`br2` stay alongside; mirror of rewrite ~195 |
| anon loop in `cc__collect_match_edits` (~519) — case body brace matcher | find-only | {str,qch,lc,bc} | ~12 | trivial | **✓ Migrated (Batch D2)** — `brr` stays alongside; mirror of rewrite ~223 |
| anon loop in `cc__collect_match_edits` (~535) — case body single-stmt `;` scanner | find-only | {str,chr} | ~12 | trivial | **Deferred** — bounded paren scan, no comments expected; mirror of rewrite ~238 (same Batch B inner-scanner decision) |

### `pass_unwrap_destroy.c` (735 LOC, 2 sites, no `text_scan.h`)

| Site | Shape | State | LOC | Complexity | Notes |
|------|-------|-------|-----|------------|-------|
| `cc__ud_pos_in_line_comment` (~62) | find-only | {str,qch} | ~18 | trivial | Line-local forward rescan |
| inline scanner in `cc__ud_stmt_start_backward` (~181) | find-only | {str,qch} | ~12 | medium | **Backward** — see cross-cutting risk #1 |

### `pass_nursery_spawn_ast.c` (1342 LOC, 3 sites, now uses `text_scan.h`)

| Site | Shape | State | LOC | Complexity | Notes |
|------|-------|-------|-----|------------|-------|
| `cc__scan_matching_rbrace` (~38) | find-only | {str,qch,lc,bc} | ~35 | trivial | **✓ Migrated (Batch D2)** — `depth` stays alongside; `for` → `while` |
| `cc__split_top_level_commas` (~434) | find-only | {str,qch} | ~22 | medium | **✓ Migrated (Batch D2)** — **behavior change**: now comment-aware.  Mid-buffer slice — `at_line_start = 0` |
| `cc__infer_spawn_stmt_end_off` (~460) | find-only | {str,qch} | ~20 | medium | **✓ Migrated (Batch D2)** — **behavior change**: now comment-aware (post-`(`).  Linear walk to first `(` kept (caller-anchored, no comments expected in gap); `CCInertScan` kicks in for the paren-balanced walk |

Note: this whole file is ORPHAN (unwired, see header banner).  Migrated for consistency so the file-wide `CCInertScan` story is uniform when the orphan is wired up or deleted.

### `ufcs.c` (2252 LOC, 3 sites, now uses `text_scan.h`)

| Site | Shape | State | LOC | Complexity | Notes |
|------|-------|-------|-----|------------|-------|
| ~~`cc__ufcs_lookup_scoped_local_var_type` (~273)~~ | ~~find-only~~ | ~~{lc,bc,str,chr}~~ | ~~~25~~ | ~~medium~~ | **✓ Migrated (Batch E)** — scope-stack/decl-count algorithm preserved (paren_depth/bracket_depth/scope_stack/scope_depth/decl_count all stay alongside).  Still a near-copy of `cc__lookup_scoped_local_var_type_codegen` — see Batch E notes for the deferred dedup |
| ~~`cc__build_ufcs_arg_slices` (~702)~~ | ~~find-only~~ | ~~{str,chr}~~ | ~~~40~~ | ~~medium~~ | **✓ Migrated (Batch F2)** — **behavior change**: now comment-aware (was deliberately not).  Synthetic trailing-`,` trick (loop `i <= n` with `(i < n) ? c : ','`) replaced with explicit post-loop trailing-arg emit (same pattern as Batch D1's `cc__create_build_arg_slices`).  `at_line_start = 0` for mid-expression slice |
| ~~anon loop in `cc__emit_closure_field_call` (~1511)~~ | ~~find-only~~ | ~~{str,qch}~~ | ~~~18~~ | ~~trivial~~ | **✓ Migrated (Batch F2)** — **behavior change**: now comment-aware.  Pointer-walk converted to indexed walk via `strlen(args)` since `CCInertScan` requires `(buf, len, &i)` |

### `checker.c` (1314 LOC, 1 site, now uses `text_scan.h`)

| Site | Shape | State | LOC | Complexity | Notes |
|------|-------|-------|-----|------------|-------|
| anon loop in `cc_visit_checker` (~918) | find-only | {lc,bc,str,chr} | ~35 | trivial | **✓ Migrated (Batch A)** |

### `edit_buffer.c` (385 LOC, 1 site, now uses `text_scan.h`)

| Site | Shape | State | LOC | Complexity | Notes |
|------|-------|-------|-----|------------|-------|
| `cc_find_first_func_def_offset` (~174) | find-only | {lc,bc,str,chr} | ~45 | medium | **✓ Migrated (Batch A)** — `brace_depth` / `last_line_off` stayed alongside; ad-hoc `#`-skip replaced by `CCInertScan` in_pp; inert newlines tracked via post-step sweep |

### `visit_codegen.c` (5598 LOC, 1 inline + 11 migrated, has `text_scan.h`)

**Migrated:** `cc__find_matching_paren_codegen` (~1619), `cc__find_matching_brace_codegen` (~1642), `cc__lookup_scoped_local_var_type_codegen` (~2408 — Batch E), plus Batch F1: `cc__neutralize_comments_for_reparse` (~420), `cc__rewrite_parser_placeholder_ufcs_lowers` (~968), `cc__blank_comptime_blocks_preserve_layout` (~2127), `cc__register_ufcs_declared_vars_for_type` (~2162), `cc__lookup_enclosing_param_type_codegen` (~2499), `cc__collect_ufcs_field_and_var_types` (~3250), plus Batch F2: `cc__cg_type_decl_end_top_level` (~185), `cc__sanitize_statement_unwraps_for_reparse` (~586).

| Site | Shape | State | LOC | Complexity | Notes |
|------|-------|-------|-----|------------|-------|
| ~~anon loop in `cc__cg_type_decl_end_top_level` (~185)~~ | ~~find-only~~ | ~~{lc,bc,str,chr}~~ | ~~~20~~ | ~~medium~~ | **✓ Migrated (Batch F2)** — `brace_depth` stays alongside; `for` → `while` with explicit `q++` tail.  `c == '{'`/`c == '}'` arms use `q++; continue;` (body-`continue;` trap variant for brace counters) |
| ~~`cc__neutralize_comments_for_reparse` (~420)~~ | ~~rewrite~~ | ~~{lc,bc,str,chr}~~ | ~~~50~~ | ~~trivial~~ | **✓ Migrated (Batch F1)** — new "inert-kind discrimination" pattern: snapshot `scan.in_line_comment`/`scan.in_block_comment` BEFORE step, check AFTER; blank consumed range only if EITHER flag was set in the OR.  Strings/chars/pp left verbatim |
| ~~`cc__sanitize_statement_unwraps_for_reparse` (~586)~~ | ~~find-only~~ | ~~{lc,bc,str,chr}~~ | ~~~20~~ | ~~medium~~ | **✓ Migrated (Batch F2)** — `for` → `while`; mid-body `continue;` (early-skip patterns) converted to `{ i++; continue; }`; trailing `i = suffix_end - 1;` (relied on for's `++i`) merged with tail advance via `i = (suffix_end > 0) ? suffix_end : i + 1;` ternary |
| ~~`cc__rewrite_parser_placeholder_ufcs_lowers` (~968)~~ | ~~rewrite~~ | ~~{lc,bc,str,chr}~~ | ~~~25~~ | ~~trivial~~ | **✓ Migrated (Batch F1)** — clean drop-in; removed now-unused `c` local |
| `cc__collect_legacy_ufcs_registrations` (~2014) | find-only | {lc,bc,str,chr} + line_start | ~35 | **complex** | Custom `#line` parse — partial overlap with `CCInertScan`.  Consolidate carefully (Batch M) |
| ~~`cc__blank_comptime_blocks_preserve_layout` (~2127)~~ | ~~rewrite~~ | ~~{lc,bc,str,chr}~~ | ~~~30~~ | ~~trivial~~ | **✓ Migrated (Batch F1)** — `i = body_r;` becomes `i = body_r + 1;` (was relying on for's `++i`) |
| ~~`cc__register_ufcs_declared_vars_for_type` (~2162)~~ | ~~find-only~~ | ~~{lc,bc,str,chr}~~ | ~~~20~~ | ~~trivial~~ | **✓ Migrated (Batch F1)** — body-`continue;` trap fix: inverted `if (...) continue;` to `if (!(...)) { ... }` guard so tail `i += type_len` always runs.  See Watch-outs |
| ~~`cc__lookup_scoped_local_var_type_codegen` (~2408)~~ | ~~find-only~~ | ~~{lc,bc,str,chr}~~ | ~~~25~~ | ~~medium~~ | **✓ Migrated (Batch E)** |
| ~~`cc__lookup_enclosing_param_type_codegen` (~2499)~~ | ~~find-only~~ | ~~{lc,bc,str,chr}~~ | ~~~20~~ | ~~trivial~~ | **✓ Migrated (Batch F1)** — clean drop-in |
| ~~`cc__collect_ufcs_field_and_var_types` (~3250)~~ | ~~find-only~~ | ~~{lc,bc,str,chr}~~ | ~~~20~~ | ~~trivial~~ | **✓ Migrated (Batch F1)** — body `continue;` at ~3450 was safe (inner `while ... i++;` advanced before continue) |

### `pass_result_unwrap.c` (2964 LOC, 1 inline + 12 migrated, has `text_scan.h`)

**Migrated:** `cc__find_unwrap_token` (~299), `cc__find_rhs_end_forward` (~476), `cc__find_semi_forward` (~613), `cc__find_bang_token_from` (~932), plus Batch G: `cc__pos_in_line_comment` (~385), `cc__bang_lhs_looks_like_decl` (~974), `cc__pu_find_outer_errhandler` (~1153), `cc__pu_next_stmt` (~1298), `cc__pu_find_enclosing_brace_close` (~1393), `cc__pu_find_next_stmt_byte` (~1430), `cc__pu_process_bang_body` (~1474), `cc__strict_unhandled_scan` (~2529).

| Site | Shape | State | LOC | Complexity | Notes |
|------|-------|-------|-----|------------|-------|
| ~~`cc__pos_in_line_comment` (~385)~~ | ~~find-only~~ | ~~{str,qch}~~ | ~~~16~~ | ~~trivial~~ | **✓ Migrated (Batch G)** — new "post-step state probe" pattern: drive `cc_inert_scan_step` to consume bytes and check `scan.in_line_comment` after each step.  Returns 1 the instant the scanner crosses into line-comment mode.  Strict superset of original behavior (also detects `/* ... */ //` on same line, which the original missed since it didn't track block comments). |
| `cc__find_lhs_start_backward_raw` (~405) | find-only | hybrid backward | ~45 | **complex** | **Backward** — see cross-cutting risk #1 |
| ~~`cc__bang_lhs_looks_like_decl` (~974)~~ | ~~find-only~~ | ~~{str,qch,lc,bc}~~ | ~~~22~~ | ~~trivial~~ | **✓ Migrated (Batch G)** — clean drop-in; `for` → `while` with tail `i++` |
| ~~`cc__pu_find_outer_errhandler` (~1153)~~ | ~~find-only~~ | ~~{str,qch,lc,bc}~~ | ~~~25~~ | ~~medium~~ | **✓ Migrated (Batch G)** — body-`continue;` audit: 7 early-skip continues each got `{ i++; continue; }`; post-match jump `i = rbrace;` (relied on for `++i`) → `i = rbrace + 1;`.  CCInertScan state inside body is stale after jump but `in_user_file` isn't checked so safe |
| ~~`cc__pu_next_stmt` (~1298)~~ | ~~find-only~~ | ~~ad-hoc prefix + {str,qch,lc,bc}~~ | ~~~35~~ | ~~medium~~ | **✓ Migrated (Batch G)** — two-phase: Phase 1 (leading ws/comments skip) uses a separate scoped `CCInertScan skip;` since the original didn't track strings; Phase 2 (stmt body walk) uses a fresh `CCInertScan scan;`.  Brace/paren/bracket arms each got `i++; continue;` |
| ~~`cc__pu_find_enclosing_brace_close` (~1393)~~ | ~~find-only~~ | ~~{str,qch,lc,bc}~~ | ~~~22~~ | ~~trivial~~ | **✓ Migrated (Batch G)** — `for` → `while`; brace/paren/bracket arms each got `i++; continue;` (body-`continue;` audit) |
| ~~`cc__pu_find_next_stmt_byte` (~1430)~~ | ~~find-only~~ | ~~ad-hoc~~ | ~~~25~~ | ~~trivial~~ | **✓ Migrated (Batch G)** — comments-only ad-hoc skip replaced by `CCInertScan`; whitespace and label-skip logic preserved alongside; label-skip's `i = m + 1; continue;` already pre-advances so doesn't need `i++` |
| ~~`cc__pu_process_bang_body` (~1474)~~ | ~~rewrite~~ | ~~{str,qch,lc,bc}~~ | ~~~55~~ | ~~medium~~ | **✓ Migrated (Batch G)** — canonical rewrite template: snapshot `before = i;` before each `cc_inert_scan_step`, `cc__append_n(out, body + before, i - before)` after step returns 1.  Replaces ~50 LOC of interleaved `in_lc`/`in_bc`/`in_str` + `cc__append_n` boilerplate with the 5-line snapshot pattern |
| ~~`cc__strict_unhandled_scan` (~2529)~~ | ~~find-only~~ | ~~{str,qch,lc,bc}~~ | ~~~25~~ | ~~trivial~~ | **✓ Migrated (Batch G)** — already a `while` loop; cleanest of the eight: just replace the inert-handler prefix with `cc_inert_scan_step` |

### `pass_defer_syntax.c` (1453 LOC, 1 inline + 7 migrated, has `text_scan.h`)

**Migrated:** `cc__find_matching_brace_text` (~350), plus Batch H1: `cc__count_top_level_semicolons` (~144), `cc__last_stmt_terminator_before` (~260), `cc__scan_function_top_level_defer_info` (~377), `cc__match_result_ctor_prefix_arg` (~485), `cc__match_result_ctor_name_arg` (~557), `cc__scan_stmt_end_semicolon` (~665).

| Site | Shape | State | LOC | Complexity | Notes |
|------|-------|-------|-----|------------|-------|
| ~~`cc__count_top_level_semicolons` (~144)~~ | ~~find-only~~ | ~~{str,qch,lc,bc}~~ | ~~~35~~ | ~~trivial~~ | **✓ Migrated (Batch H1)** — `if/else if` counter chain naturally falls through to tail `i++` — no body-`continue;` audit needed |
| ~~`cc__last_stmt_terminator_before` (~260)~~ | ~~find-only~~ | ~~{lc,bc,str,chr}~~ | ~~~18~~ | ~~trivial~~ | **✓ Migrated (Batch H1)** — cleanest of the six, naked body with no counters or continues |
| ~~`cc__scan_function_top_level_defer_info` (~377)~~ | ~~find-only~~ | ~~{str,qch,lc,bc}~~ | ~~~25~~ | ~~medium~~ | **✓ Migrated (Batch H1)** — `rel_depth` stays alongside; `{`/`}` arms get `i++; continue;`; CCInertScan called with bound `close_i` (not `len`) since loop terminates at the matching brace |
| ~~`cc__match_result_ctor_prefix_arg` (~485)~~ | ~~find-only~~ | ~~{str,chr,lc,bc}~~ | ~~~35~~ | ~~medium~~ | **✓ Migrated (Batch H1)** — **near-duplicate consolidation**: both ~485 and ~557 had identical paren-balance inert-scan blocks; factored into shared helper `cc__match_ctor_close_paren` (~25 LOC).  Net −60 LOC across the pair |
| ~~`cc__match_result_ctor_name_arg` (~557)~~ | ~~find-only~~ | ~~{str,chr,lc,bc}~~ | ~~~35~~ | ~~medium~~ | **✓ Migrated (Batch H1)** — calls `cc__match_ctor_close_paren` |
| ~~`cc__scan_stmt_end_semicolon` (~665)~~ | ~~find-only~~ | ~~{str,qch,lc,bc}~~ | ~~~35~~ | ~~trivial~~ | **✓ Migrated (Batch H1)** — duplicate of pass_err_syntax's twin; identical migration shape |
| `cc__rewrite_defer_syntax` main loop (~704) | rewrite | {str,qch,lc,bc,pp,als} | ~90 | **complex** | Full defer-stack pass with pp tracking (Batch H2) |

### `pass_with_deadline_syntax.c` (412 LOC, 0 inline + 6 migrated, has `text_scan.h`)

**Migrated:** outer loop of `cc__collect_with_deadline_edits` (~274), plus Batch I1: `cc__rewrite_with_deadline_syntax` main loop (~11), 2 nested in rewrite (~136/~191), 2 nested in collect (~321/~355) — all 4 nested replaced with shared `cc_find_matching_paren`/`cc_find_matching_brace` helpers.

| Site | Shape | State | LOC | Complexity | Notes |
|------|-------|-------|-----|------------|-------|
| ~~`cc__rewrite_with_deadline_syntax` main loop (~11)~~ | ~~rewrite~~ | ~~{lc,bc,str,chr}~~ | ~~~85~~ | ~~medium~~ | **✓ Migrated (Batch I1)** — canonical rewrite template (snapshot before, append after) + nested scanners replaced with `cc_find_matching_paren`/`cc_find_matching_brace` |
| ~~anon loop in `cc__rewrite_with_deadline_syntax` (~136)~~ | ~~find-only~~ | ~~{str,qch,lc,bc}~~ | ~~~15~~ | ~~trivial~~ | **✓ Migrated (Batch I1)** — replaced wholesale with `cc_find_matching_paren(src, n, j, &expr_r)`; collapsed 16 LOC to 4 |
| ~~anon loop in `cc__rewrite_with_deadline_syntax` (~191)~~ | ~~find-only~~ | ~~{str,qch,lc,bc}~~ | ~~~15~~ | ~~trivial~~ | **✓ Migrated (Batch I1)** — replaced wholesale with `cc_find_matching_brace(src, n, body_s, &body_close); body_e = body_close + 1;` |
| ~~anon loop in `cc__collect_with_deadline_edits` (~321)~~ | ~~find-only~~ | ~~{str,qch,lc,bc}~~ | ~~~15~~ | ~~trivial~~ | **✓ Migrated (Batch I1)** — same `cc_find_matching_paren` substitution (twin of rewrite's ~136) |
| ~~anon loop in `cc__collect_with_deadline_edits` (~355)~~ | ~~find-only~~ | ~~{str,qch,lc,bc}~~ | ~~~15~~ | ~~trivial~~ | **✓ Migrated (Batch I1)** — same `cc_find_matching_brace` substitution (twin of rewrite's ~191) |

### `pass_err_syntax.c` (1468 LOC, 2 inline + 5 migrated, has `text_scan.h`)

**Migrated:** `cc__find_matching_brace_text` (~213), plus Batch I2: `cc__scan_stmt_end_semicolon` (~152), `cc__err_pos_in_line_comment` (~290), `cc__expand_delegations` (~523), `cc__rewrite_colon_defaults` (~700).

| Site | Shape | State | LOC | Complexity | Notes |
|------|-------|-------|-----|------------|-------|
| ~~`cc__scan_stmt_end_semicolon` (~152)~~ | ~~find-only~~ | ~~{str,qch,lc,bc}~~ | ~~~35~~ | ~~trivial~~ | **✓ Migrated (Batch I2)** — byte-for-byte twin of pass_defer's same-named function |
| ~~`cc__err_pos_in_line_comment` (~290)~~ | ~~find-only~~ | ~~{str,qch}~~ | ~~~16~~ | ~~trivial~~ | **✓ Migrated (Batch I2)** — twin of Batch G's `cc__pos_in_line_comment`; same post-step `scan.in_line_comment` probe |
| `cc__err_stmt_start_backward` (~311) | find-only | hybrid backward | ~40 | **complex** | **Backward** — see cross-cutting risk #1 |
| ~~`cc__expand_delegations` (~523)~~ | ~~rewrite~~ | ~~{str,qch,lc,bc}~~ | ~~~55~~ | ~~medium~~ | **✓ Migrated (Batch I2)** — canonical rewrite template (snapshot before, append after); body `i = close - 1; continue;` (relied on for `++i`) → `i = close; continue;`; tail needs explicit `i++` |
| ~~`cc__rewrite_colon_defaults` (~700)~~ | ~~rewrite~~ | ~~{str,qch,lc,bc}~~ | ~~~45~~ | ~~medium~~ | **✓ Migrated (Batch I2)** — `copy_from` block-copy bookkeeping means inert content isn't copied per-byte; just skip inert via `cc_inert_scan_step`.  No body-`continue;` audit needed (was already a `while` loop with explicit `i++` paths).  Dropped now-unused `char ch` local |
| `cc__rewrite_err_core` main loop (~859) | rewrite | {str,qch,lc,bc,pp,als} | ~100 | **complex** | Full pass + `ito[]` offset map.  Highest-risk in file (Batch I3) |

---

## Cross-cutting risk themes

1. **Backward scanners.**  `CCInertScan` is forward-only.
   `cc__find_lhs_start_backward_raw` (result_unwrap), `cc__err_stmt_start_backward` (err), `cc__ud_stmt_start_backward` (unwrap_destroy) all walk backward using companion helpers (`cc__skip_str_backward`, `cc__skip_block_comment_backward`, `cc__pos_in_line_comment`).
   **Decision needed**: do we extend `text_scan.h` with a `CCInertScanBackward` helper, or treat these as a separate workstream and leave the forward-side companions in place?
   **Recommendation**: defer backward to its own batch at the end of Phase 2.  Forward sites need this work too and we can size the helper investment then.

2. **Incomplete comment skip.**  Several sites skip strings/chars only and **deliberately don't** skip comments (e.g. `cc__has_top_level_brace`, `cc__create_build_arg_slices`, `cc__split_top_level_commas`, `cc__rewrite_idents`, `cc__rewrite_typed_chan_await_expr`).
   Migrating to `CCInertScan` would be a behavior change (now skipping comments).  **Each one needs a moment of thought**: is the omission deliberate (and we need a no-comment-skip variant of `CCInertScan`) or accidental (and migration is a free correctness improvement)?
   **Recommendation**: flag these in their batch's commit message; smoke is the safety net.

3. **Nested inline scanners inside migrated outer loops** (`pass_with_deadline_syntax.c` collect path).  Outer migrated, inner still inline.  Migrating the inner is mechanical but the partial state in the file is misleading until done.

4. **Duplicate scanners across files** — `cc__ufcs_lookup_scoped_local_var_type` (ufcs.c) and `cc__lookup_scoped_local_var_type_codegen` (visit_codegen.c) are near-copies.  Batch together for consistency.

5. **`cc__scan_skip_string_comment` is a precursor.**  Several scanner sites in `pass_closure_literal_ast.c` delegate to it.  Migrating the helper first, then collapsing its callers, gives the cleanest commit shape.

6. **Custom `#line` parsers** that overlap with `CCInertScan` but aren't identical (e.g. `cc__collect_legacy_ufcs_registrations` in visit_codegen.c).  Need to verify behavioral equivalence carefully.

---

## Phase 2 — batching plan

Each batch is one commit, with smoke verification (default + `CC_PRE_EXPAND=0`) before commit.

Ordering principle: **easy wins first to build muscle memory, then per-area sweeps, then complex/risky sites, then backward scanners last.**

### Batch A — single-site easy wins (3 sites, 1 commit) — **LANDED 2026-05-27**

> Goal: lowest-risk start; establishes that the migration pattern works across diverse pass files.

- [x] `pass_ufcs.c::cc__line_has_await_keyword` (~28 LOC, trivial)
- [x] `checker.c::cc_visit_checker` anon (~35 LOC, trivial)
- [x] `edit_buffer.c::cc_find_first_func_def_offset` (~45 LOC, medium — `brace_depth` + `last_line_off` stay alongside)

**Actual diff**: −101 / +37 (net −64).  Smoke 461/461 both modes.  Lowered C inspected on `r1_async_name_smoke` (rewrite paths produce identical `cc_nursery_spawn_async_named(...)` calls).

**Surprises:**
- For `cc__line_has_await_keyword`, the original ignored `#`-line directives (line is mid-buffer slice).  Migration sets `s.at_line_start = 0` after init to preserve exact behavior — otherwise `CCInertScan` would treat a leading `#` as a pp directive on subsequent lines (none reach this path in practice, but defensive).
- For `cc_find_first_func_def_offset`, newlines inside inert regions still need to advance `last_line_off`.  Solved by sweeping the consumed `[before, i)` range after each `cc_inert_scan_step` returns 1.  Pattern is reusable for any "find-only with line tracking" site (e.g. `cc__cg_type_decl_end_top_level` in F2).
- The ad-hoc `#`-directive skip in `cc_find_first_func_def_offset` was replaced by `CCInertScan`'s built-in `in_pp` handling, which is strictly more correct (handles line continuations + the at-line-start guard properly).

### Batch B — type-syntax sweep (7 sites, 1 commit) — **LANDED 2026-05-27**

> Goal: single-pass sweep of the most uniformly-trivial file in the audit.

- [x] `pass_type_syntax.c::cc__rewrite_slice_types_text` (post-step sweep for line/col)
- [x] `pass_type_syntax.c::cc__scan_for_existing_result_types`
- [x] `pass_type_syntax.c::cc__rewrite_result_types_text` (inner `!>(...)` paren scan kept inline)
- [x] `pass_type_syntax.c::cc__rewrite_result_field_sugar_text` (pass 1 & 2)
- [x] `pass_type_syntax.c::cc__rewrite_inferred_result_constructors` (`brace_depth`/`fn_brace_depth`/`current_result_type` stay alongside; inner `cc_ok(...)` paren scan kept inline)
- [x] `pass_type_syntax.c::cc__rewrite_try_exprs_text` (inner expression-end paren scan kept inline)

**Actual diff**: 42 / −97 (net **−55 LOC**).  Smoke 461/461 both modes.  Lowered C inspected on `result_custom_types_smoke.ccs`: all rewrites (`CCRes(T,E) → CCResult_T_E`, `cc_ok(v) → cc_ok_CCResult_..._...(v)`, `cc_err(...)`, `CCSlice[T]`, `cc_try(...)`) emit byte-for-byte identical output.

**Pattern emerging**: most "rewrite" sites have an **outer** scanner (now migrated) plus one or two **inner** scanners that walk a bounded region (a paren group, an expression-tail) using str/chr only.  We're deliberately leaving the inner scanners inline this batch because (a) they're correct enough for their bounded scope and (b) migrating them is a deeper change that adds little until Phase 4.  Recorded for future batches: if a regression ever surfaces in `!>(...)` with a comment inside, this is where to look.

### Batch C — channel-syntax sweep (7 sites, 1 commit) — **LANDED 2026-05-27**

> Goal: file we just edited for R2 — context is fresh.

- [x] `pass_channel_syntax.c::cc__record_chan_handle_alias_decls`
- [x] `pass_channel_syntax.c::cc__scan_matching_brace`
- [x] `pass_channel_syntax.c::cc__rewrite_channel_pair_calls_text`
- [x] `pass_channel_syntax.c::cc__find_matching_delim`
- [x] `pass_channel_syntax.c::cc__find_arrow`
- [x] `pass_channel_syntax.c::cc__extract_block_tail_expr`
- [x] `pass_channel_syntax.c::cc__rewrite_chan_handle_types_text` (drop-in win — `CCInertScan`'s native pp trio replaces the hand-rolled one, the comment about M7.B parity now says "see `text_scan.h`")

**Actual diff**: 67 / −124 (net **−57 LOC**).  Smoke 461/461 both modes.  Channel lowering inspected on `r2_channel_meta_smoke.ccs` — still produces clean `cc_channel_pair_create_named(...)` with `"tx,rx"`/`__FILE__`/`__LINE__` metadata.

**Surprises:**
- **Code-path `\n` bug**: latent in Batch B too.  `cc_inert_scan_step` returns 0 for `\n` outside any inert region and leaves `i` pointing AT the newline.  My initial Batch C migration of `cc__rewrite_channel_pair_calls_text` lost line tracking for code-line newlines and reported the `m0_5_diag_channel_pair_origin_fail.ccs` error at line 16 (prelude) instead of line 21 (user source).  Fix: add `if (c == '\n') { line++; col = 1; i++; continue; }` after the inert-step block.  Retroactively applied to `cc__rewrite_slice_types_text` (Batch B); no test there ever triggered the error path so it had been latent.
- **Off-by-one column** in original pre-migration code: pre-state `if (c == '\n') { line++; col = 1; }` followed by `i++; col++` overshot by 1 (reported the column of the byte AFTER the token, not the token itself).  My migration reports the correct column.  Test `m0_5_diag_channel_pair_origin_fail.compile_err` was anchored on the buggy 21:19 — updated to the now-correct 21:18 in the same commit.
- **`for` → `while`** loop conversion for two find-only sites (`cc__find_matching_delim`, `cc__extract_block_tail_expr`).  `cc_inert_scan_step` advances `i` by 1 or 2 bytes per call, which a `for (i=...; i<end; i++)` would double-advance.  Mechanical change but worth noting for batch authors who reach for `for` first.

**Pattern that should now go into the migration template**: any rewrite that tracks `line`/`col` needs BOTH the inert-region sweep AND a code-path `\n` handler.  This is recurring — at least 3 sites so far (`cc__rewrite_slice_types_text`, `cc__rewrite_channel_pair_calls_text`, `cc__rewrite_chan_handle_types_text`) and probably more in batches H/I/J.

### Batch D — create + nursery + match (13 sites, 2 commits)

Commit D1 (8 sites) — **LANDED 2026-05-27**:
- [x] `pass_create.c::cc__create_find_top_level_comma`
- [x] `pass_create.c::cc__create_build_arg_slices` — **behavior change**: now comment-aware (was deliberately not); synthetic trailing-`,` trick replaced by explicit post-loop emit
- [x] `pass_create.c::cc__create_seed_registered_var_types` — per-type re-init; loop-tail increment adjusted
- [x] `pass_match_syntax.c::cc__parse_lowered_match_call` inner (~65) — **behavior change**: now comment-aware
- [x] `pass_match_syntax.c::cc__rewrite_match_syntax` main loop (~95) — `line`/`col` preserved via post-step sweep + code-path `\n` handler
- [x] `pass_match_syntax.c::cc__rewrite_match_syntax` nested ~147 (body-brace matcher)
- [x] `pass_match_syntax.c::cc__rewrite_match_syntax` nested ~195 (case header `:` scanner)
- [x] `pass_match_syntax.c::cc__rewrite_match_syntax` nested ~223 (case body brace matcher)
- [ ] **Deferred**: `pass_match_syntax.c::cc__rewrite_match_syntax` nested ~238 (case body single-stmt `;` scanner — bounded, no comments).  Mirrors Batch B's inner-scanner decision.

**Actual diff**: 102 / −146 (net **−44 LOC**).  Smoke 461/461 both modes.  Lowered C inspected on `match_case_header_comment_bait_smoke.ccs`: still emits the expected `do { /* @match */ size_t __cc_match_idx_1 = ...; CCChanMatchCase __cc_match_cases_1[1]; ... cc_chan_match_select(...) ... switch (__cc_match_idx_1) { ... } } while(0);`.

**Surprises:**
- **Audit miscount in `pass_match_syntax.c`**: audit said "2 nested inline scanners" inside `cc__rewrite_match_syntax`; real count is 4 (body-brace ~147, case header ~195, case-body brace ~223, case-body single-stmt ~238).  Migrated 3, left the no-comments single-stmt scanner inline.  D2 will also touch the collect path's mirrored set.
- **Two "no comments" behavior changes** (`cc__create_build_arg_slices`, `cc__parse_lowered_match_call` inner).  Smoke green in both modes; effect is correct in practice (no real CC source embeds `/* */` inside `cc_type_create(int, "...")` arg-list strings or inside `<chan>.recv(/*...*/&x)` headers — and the latter the rewrite path catches *before* this inner scanner runs, via `cc_find_substr_top_level`).
- **Synthetic trailing-char trick** in `cc__create_build_arg_slices` had to go: the original looped `for (i=0; i <= n; ++i)` with `c = (i < n) ? args_src[i] : ','` to flush the last segment.  `cc_inert_scan_step` requires `*pos < n`, so the trick must be replaced with explicit post-loop trailing-arg emit.  Pattern to remember for any future "synthetic sentinel" loop.
- **Per-type scanner re-init** in `cc__create_seed_registered_var_types`: the outer loop iterates types and the inner scanner walks the full source per type.  `CCInertScan` state must reset per outer iteration (init inside the outer loop body).
- **Loop-tail increment** in `cc__create_seed_registered_var_types`: original `i += type_len - 1` relied on `for`'s `++i` to give total advance of `type_len`; new while-loop changes to `i += type_len`.

Commit D2 (6 sites) — **LANDED 2026-05-27**:
- [x] `pass_nursery_spawn_ast.c::cc__scan_matching_rbrace`
- [x] `pass_nursery_spawn_ast.c::cc__split_top_level_commas` — **behavior change**: now comment-aware
- [x] `pass_nursery_spawn_ast.c::cc__infer_spawn_stmt_end_off` — **behavior change**: now comment-aware (post-`(`); leading linear-walk-to-`(` kept (caller-anchored)
- [x] `pass_match_syntax.c::cc__collect_match_edits` main loop (~415) — `line`/`col` preserved via post-step sweep + code-path `\n` handler
- [x] `pass_match_syntax.c::cc__collect_match_edits` nested ~460 (body-brace matcher)
- [x] `pass_match_syntax.c::cc__collect_match_edits` nested ~507 (case header `:` scanner)
- [x] `pass_match_syntax.c::cc__collect_match_edits` nested ~519 (case body brace matcher)
- [ ] **Deferred**: `cc__collect_match_edits` nested ~535 (case body single-stmt `;` scanner) — mirrors rewrite ~238 deferral.

**Actual diff**: +72 / −127 (net **−55 LOC**).  Smoke 461/461 both modes.  Lowered C on `match_case_header_comment_bait_smoke` byte-for-byte identical to Batch D1 (collect path's expansion matches rewrite path's expansion).

**Surprises:** None — D2 was a near-mechanical mirror of D1's pass_match_syntax work, and pass_nursery_spawn_ast's three sites all followed the standard "find-only with depth counters" pattern from Batch C.  The two "no comments" behavior changes in pass_nursery_spawn_ast are theoretical only since the file is orphaned today; even so, smoke would have caught any real regression because the helpers are still link-reachable.

### Batch E — ufcs duplicate consolidation (2 sites in different files, 1 commit) — **LANDED 2026-05-27**

> Goal: kill the near-copy.  Either migrate both to `CCInertScan` AND introduce a shared scoped-local-var helper, OR just migrate each independently.  Decide at commit time based on how messy the consolidation looks.

- [x] `ufcs.c::cc__ufcs_lookup_scoped_local_var_type` — migrated independently
- [x] `visit_codegen.c::cc__lookup_scoped_local_var_type_codegen` — migrated independently

**Decision (commit-time)**: migrated each independently rather than consolidating.  The two functions share an identical scope-stack + decl-count body, but differ in **four** non-trivial places:
  1. Input parsing (`recv_expr` skip-`&`/whitespace/identifier-extract vs raw `var_name`).
  2. Helper-fn names called inside the body: parse-decl fallback (×2), normalize-decl-type (×2), canonicalize-type-alias (×2) — all per-file static.
  3. Non-decl-stmt check: hand-rolled list vs `cc_is_non_decl_stmt_type` helper.
  4. Post-loop fallback: NULL vs `cc__lookup_enclosing_param_type_codegen(...)`.

Consolidating would require a function-pointer dispatch table or harmonizing those four divergences first — net churn larger than the M1 scope.  Recorded as a **future cleanup item**: "deduplicate scoped-local-var-type lookup" — owner: anyone touching either file next.

**Actual diff**: +14 / −36 (net **−22 LOC**).  Smoke 461/461 both modes.  Spot-check on `ufcs_chained_methods_smoke.ccs` confirms receiver-type lookup still produces correct chained-method dispatch.

**Surprises:**
- **Incidental warning fix**: removing the unused `in_lc`/`in_bc`/`in_str`/`in_chr` locals dropped one of the build's 3 pre-existing `-Wunused-*` warnings to 2.  The remaining 2 (`cc__is_channel_tx_recv_type`, `cc__is_channel_rx_recv_type`) are unrelated dead static helpers — separate cleanup.
- **The audit's "batch together" suggestion was right but for the wrong reason.**  Yes, both files need the same migration; no, that doesn't mean they should be consolidated yet.  Migration-with-future-dedup-flag is the right tradeoff at this scale.

### Batch F — ufcs + visit_codegen trivial bulk (10 sites, 2 commits)

Commit F1 (6 trivial in visit_codegen.c) — **LANDED 2026-05-27**:
- [x] `cc__neutralize_comments_for_reparse` — new pattern: inert-kind discrimination via prev/post `scan.in_*` snapshots
- [x] `cc__rewrite_parser_placeholder_ufcs_lowers`
- [x] `cc__blank_comptime_blocks_preserve_layout`
- [x] `cc__register_ufcs_declared_vars_for_type` — body-`continue;` infinite-loop bug caught by smoke + fixed
- [x] `cc__lookup_enclosing_param_type_codegen`
- [x] `cc__collect_ufcs_field_and_var_types`

**Actual diff**: +44 / −108 (net **−64 LOC**).  Smoke 461/461 both modes.  Incidentally cleared 2 of 3 pre-existing `-Wunused-*` warnings (1 from F1, 1 from E).

**Surprises:**
- **New pattern — "inert-kind discrimination"**: `cc__neutralize_comments_for_reparse` actively *uses* the inert state to decide what to rewrite (blank comments, leave strings).  Migrated by snapshotting `scan.in_line_comment`/`scan.in_block_comment` BEFORE the step, then OR-ing with the same flags AFTER; if EITHER side is true, the step consumed comment bytes and we blank them.  Strings/chars/pp are left verbatim.  Generally useful pattern for any "rewrite by inert-region kind" pass.
- **Body-`continue;` infinite-loop bug** (caught by smoke!): `cc__register_ufcs_declared_vars_for_type` had `if (v < n && src[v] == '(') continue;` mid-body — the original for-loop's `continue` triggered `++i`; my while-loop's `continue` skipped the tail `i += type_len` advance and re-tested the same position forever.  4 tests hung at 300s build timeout (`unwrap_destroy_registered_type_smoke`, `arena_detach_destroy_return_smoke`, `nursery_create_detach_proto_smoke`, `comptime_type_create_arg_types_smoke`).  Fix: invert the condition to `if (!(...)) { ... rest of body ... }` so the tail advance always runs.  Added to Watch-outs as a pre-migration audit checklist item.
- **`cc__rewrite_parser_placeholder_ufcs_lowers`** had an unused `char c = src[i];` after the migration removed all `c`-references.  Just dropped it.
- **`cc__blank_comptime_blocks_preserve_layout`** had `i = body_r;` relying on the for-loop's `++i` to make total advance `body_r + 1`.  Changed to explicit `i = body_r + 1;`.

Commit F2 (4 medium sites) — **LANDED 2026-05-27**:
- [x] `visit_codegen.c::cc__cg_type_decl_end_top_level` anon — `brace_depth` stays alongside; `q++; continue;` on `{`/`}` arms, tail `q++` for fall-through
- [x] `visit_codegen.c::cc__sanitize_statement_unwraps_for_reparse` — body-`continue;` early-skip pattern: each `continue;` audited and converted to `{ i++; continue; }`; trailing `i = suffix_end - 1;` (relied on for's `++i`) folded into `i = (suffix_end > 0) ? suffix_end : i + 1;` ternary
- [x] `ufcs.c::cc__build_ufcs_arg_slices` — two-pass split, synthetic trailing-`,` → explicit post-loop trailing-arg emit (twin of Batch D1 `cc__create_build_arg_slices`)
- [x] `ufcs.c::cc__emit_closure_field_call` anon — pointer-walk (`for (const char* p = args; *p; p++)`) converted to indexed walk via `strlen(args)` since `CCInertScan` requires `(buf, len, &i)`

**Actual diff**: +60 / −98 (net **−38 LOC**).  Smoke 461/461 both modes.  No new pre-existing warnings cleared (the 2 unrelated unused-static helpers in `ufcs.c` remain).

**Surprises:**
- **No new patterns** — F2 was the cleanest batch yet.  Every site was a textbook application of an already-documented Watch-out:
  - `cc__cg_type_decl_end_top_level`: standard `for`→`while` with explicit tail `q++` plus `q++; continue;` on the brace-counter arms.
  - `cc__sanitize_statement_unwraps_for_reparse`: body-`continue;` audit before migration successfully spotted 3 early-skip continues (lines `!(c == '!' && c2 == '>')`, suffix-chain bail, decl-init bail).  Each got `i++` before the continue — no smoke regression.
  - `cc__build_ufcs_arg_slices`: identical to its `pass_create.c` twin; synthetic trailing-`,` trick has now appeared 3 times, so the "explicit post-loop emit" rewrite is muscle memory.
  - `cc__emit_closure_field_call`: pointer-walk had to become indexed.  No semantic change.
- **Pointer-walk conversion pattern**: when a scanner uses `for (const char* p = src; *p; p++)`, `CCInertScan` (which takes `(buf, len, &i)`) forces a switch to indexed walk.  Pattern: compute `size_t n = strlen(src);` once before the loop, replace `*p` with `src[i]`, replace `p[1]` with `src[i+1]` (with bounds check), replace `comma = p;` with `comma = src + i;` (or just save the index).

### Batch G — pass_result_unwrap forward sites (8 sites, 1 commit) — **LANDED 2026-05-27**

> Backward scanner `cc__find_lhs_start_backward_raw` deferred to Batch L.

- [x] `cc__pos_in_line_comment` — new pattern: post-step `scan.in_line_comment` probe
- [x] `cc__bang_lhs_looks_like_decl` — clean drop-in
- [x] `cc__pu_find_outer_errhandler` — 7 early-skip continues `{ i++; continue; }`'d; post-match jump `i = rbrace + 1;`
- [x] `cc__pu_next_stmt` — two scoped `CCInertScan` instances (one per phase)
- [x] `cc__pu_find_enclosing_brace_close` — body-`continue;` audit on brace counters
- [x] `cc__pu_find_next_stmt_byte` — comment-only scan replaced; label-skip preserved
- [x] `cc__pu_process_bang_body` — canonical rewrite template (snapshot before, append after)
- [x] `cc__strict_unhandled_scan` — already a while; trivial replacement

**Actual diff**: +69 / −189 (net **−120 LOC**, the biggest batch reduction yet).  Smoke 461/461 both modes.  No new warnings (pre-existing 3rd-party noise unchanged).

**Surprises:**
- **No bugs, no smoke regressions** — the body-`continue;` trap pattern was a known watch-out from Batch F1, so every early-skip continue got audited before the StrReplace ran.  This is what muscle memory looks like.
- **New pattern — "post-step state probe"**: `cc__pos_in_line_comment` doesn't try to MATCH a CC token; it just wants to know if position X sits inside a line comment.  Migration: drive `cc_inert_scan_step` over the line and check `scan.in_line_comment` after each step.  Returns 1 the moment the scanner crosses into line-comment mode.  Note this is strictly more correct than the original — the original only checked for `//` ignoring block comments, so `/* foo */ //` on the same line would still return 1 (correctly) by accident.  CCInertScan handles this for free.
- **`cc__pu_process_bang_body` is the canonical rewrite template now**: the snapshot-before/append-after pattern replaces ~50 LOC of interleaved `in_lc`/`in_bc`/`in_str` state + `cc__append_n` calls with 5 lines.  Pointing future rewrite-pass migrations here.
- **Two-phase functions like `cc__pu_next_stmt`**: easiest pattern is two scoped `CCInertScan` instances (one per phase), each freshly initialized.  Cheaper than trying to preserve state across the phase boundary and clearer for the reader.
- **Stale scanner state after big jumps** (in `cc__pu_find_outer_errhandler` post-match `i = rbrace + 1;`): scanner doesn't process the skipped bytes, so its `in_block_comment` / `in_pp` flags could be wrong if the body contained those.  Safe for this function (it doesn't check `in_user_file` or any other state-derived flag, just looks for `@errhandler` tokens at code position).  Note this as a Watch-out for future jumps: if a post-match jump is followed by code that reads `scan.in_user_file`, re-init the scanner.

### Batch H — pass_defer_syntax (6 forward sites + 1 complex, 2 commits)

H1 (6 trivial/medium) — **LANDED 2026-05-27**:
- [x] `cc__count_top_level_semicolons` — `if/else if` chain naturally falls through to tail `i++`
- [x] `cc__last_stmt_terminator_before` — cleanest drop-in
- [x] `cc__scan_function_top_level_defer_info` — `rel_depth` stays; `{`/`}` arms `i++; continue;`
- [x] `cc__match_result_ctor_prefix_arg` — consolidated with name_arg below
- [x] `cc__match_result_ctor_name_arg` — extracted `cc__match_ctor_close_paren` helper, net −60 LOC across the pair
- [x] `cc__scan_stmt_end_semicolon` — duplicate of pass_err_syntax's twin

**Actual diff**: +49 / −187 (net **−138 LOC**, new biggest batch reduction — beats Batch G by 18).  Smoke 461/461 both modes.

**Surprises:**
- **Near-duplicate consolidation worked this time** (unlike Batch E).  `cc__match_result_ctor_prefix_arg` and `cc__match_result_ctor_name_arg` had IDENTICAL paren-balance inert-scan blocks — the only difference was the prefix/name token-matching logic ABOVE the inert scan.  Extracted just the inert-scan-and-balanced-paren walk into a shared helper `cc__match_ctor_close_paren` (~25 LOC).  Replaces ~70 LOC of duplicated body in each function with a single call.  Generally useful pattern: **when two callers diverge only in the pre-scan setup but share an identical inert-scanner body, factor the inert-scanner body out.**
- **Why this is different from Batch E's "don't consolidate" decision**: in Batch E, the two functions diverged in FOUR places (input parsing + 3 different helper calls inside the body).  Here, the divergence is only in the leading prefix/name check.  The post-extraction call sites are 3 lines (`if (!cc__match_ctor_close_paren(...)) return 0;`).  Easy enough.
- **Counter `if/else if` chains don't need body-`continue;` audit** — sites 1 and 6 use `if (ch == '(') par++; else if (ch == ')') par--;` chains that naturally fall through to the loop tail.  No `continue;` in any arm means tail `i++` always runs.  Cheaper migration than sites with explicit `if (ch == '{') { rel_depth++; continue; }` (which need the `{ i++; continue; }` audit).

H2 (complex full-pass rewrite, separate commit):
- [ ] `cc__rewrite_defer_syntax` main loop (complex)

### Batch I — pass_with_deadline_syntax + pass_err_syntax forward sites (9 sites, 1 commit) — **LANDED 2026-05-27**

I1 — with_deadline finish (5 sites):
- [x] `cc__rewrite_with_deadline_syntax` main loop — canonical rewrite template + nested replaced
- [x] anon paren scanner ~136 — replaced with `cc_find_matching_paren`
- [x] anon brace scanner ~191 — replaced with `cc_find_matching_brace`
- [x] `cc__collect_with_deadline_edits` anon paren ~321 — replaced with `cc_find_matching_paren`
- [x] `cc__collect_with_deadline_edits` anon brace ~355 — replaced with `cc_find_matching_brace`

I2 — err forward sites (4 sites):
- [x] `cc__scan_stmt_end_semicolon` — twin of pass_defer; trivial drop-in
- [x] `cc__err_pos_in_line_comment` — twin of Batch G's post-step probe
- [x] `cc__expand_delegations` — canonical rewrite template
- [x] `cc__rewrite_colon_defaults` — `copy_from` block-copy means just skip inert

**Actual diff**: +73 / −213 (net **−140 LOC**, new biggest batch reduction — beats H1 by 2).  Smoke 461/461 both modes.

**Surprises:**
- **`cc_find_matching_paren` / `cc_find_matching_brace` in `util/text.h` already existed** and were sitting unused by these passes.  All 4 nested with_deadline scanners were reinventing the wheel — and identically wrong about brk/br tracking that the shared helpers do correctly.  Net effect: 64 LOC of duplicated inline state machine collapsed into 4 helper calls.  **Watch-out for future batches: GREP for nested paren/brace inline scanners; many will already have a shared helper.**
- **`cc_find_matching_paren/brace` aren't yet on CCInertScan themselves** — they have their own inlined inert state.  Migrating them is a separate cleanup (future Batch M).  Today's `cc_find_matching_*` callers still get correct inert handling; it's just not unified with `CCInertScan`.
- **`copy_from` block-copy rewrites are cheaper to migrate than canonical rewrites**: when the output is built via `cc__append_n(out, s + copy_from, ...)` jumps at rewrite points, the inert content is never per-byte copied.  No snapshot-before/append-after needed — just `if (cc_inert_scan_step) continue;` to skip inert.  Pattern: identify rewrite shape FIRST before choosing the migration template.

I3 — err complex full-pass rewrite (separate commit):
- [ ] `cc__rewrite_err_core` main loop (complex — `ito[]` offset map)

### Batch J — async_ast forward sites (11 sites, 1 combined commit; complex separate) — **LANDED 2026-05-27**

J1+J2 combined into a single commit (file didn't yet have `text_scan.h`, so all 11 forward sites were migrated at once for net diff clarity):

- [x] `cc__find_matching_paren` — **deleted local copy**, `#define`'d to `cc_find_matching_paren` from `util/text.h` (identical implementation; "Keep local implementations" comment removed)
- [x] `cc__find_matching_brace` — same as above
- [x] `cc__rewrite_idents` (rewrite, "no comments" → now comment-aware) — canonical rewrite template with the existing realloc/cap pattern preserved; fixed a latent realloc-leak (original returned NULL without freeing old `out` on realloc failure)
- [x] `cc__rewrite_typed_chan_await_expr` (find-only with depth + "no comments" → now comment-aware) — inner comma scanner only; outer find-`(` already uses `cc_find_char_top_level`
- [x] `cc__scan_simple_stmt_end` — clean drop-in
- [x] `cc__truncate_at_first_semicolon0` — clean drop-in (NULL-terminates in place)
- [x] `cc__split_top_level_semis` — collect-shape; `@errhandler` body-close split logic preserved
- [x] `cc__rhs_has_top_level_comma` — clean drop-in
- [x] `cc__parse_loop_from_text` anon (for-header `;` splitter on `header` slice)
- [x] `cc__normalize_result_generic_bool_calls` — rewrite with `last_emit` jumps; inner depth-scanner replaced with `cc__find_matching_paren`; outer adds CCInertScan (pattern matches are now skipped inside comments/strings)
- [x] `cc_async_rewrite_state_machine_ast` anon — per-line ident finder; `break` on `//` converges to `continue` on inert step (single-line slice, so behavior is equivalent)

**Actual diff**: +83 / −189 (net **−106 LOC**).  Full suite 461/461 (default mode); 382/382 smoke-filtered (both modes).

> **Watch-out (process)**: a transient `[FAIL] ... build failed` storm during one full-suite run turned out to be a parallelism flake (`--jobs 4` under system load — re-run was clean).  When investigating a wide failure pattern, always rerun once before bisecting.

**Surprises:**
- **Two identical local helpers shadowed `util/text.h`** with a `/* Keep local implementations ... to avoid subtle behavioral changes */` comment.  Byte-for-byte diff confirmed they were identical to the shared versions.  Replaced with `#define cc__find_matching_paren cc_find_matching_paren` (and twin for brace) — preserves all call sites with zero churn, gains the shared-helper status, and lets future Batch M migrate them in one place.  **Watch-out: any "Keep local — avoid behavioral changes" comment older than ~2 weeks is suspect; diff-confirm before trusting it.**
- **Existing helper reuse, second sighting**: `cc__normalize_result_generic_bool_calls`'s inner paren-depth scanner was a hand-rolled `int depth = 1, in_str = 0, in_chr = 0; for (; arg_end < n; arg_end++) {...}` walker — exactly `cc_find_matching_paren`'s contract.  Replaced 20 LOC with one call.  Combined with Batch I's `cc_find_matching_paren/brace` reuse for the `with_deadline` scanners, this is now an established pattern: **whenever you see a `depth=1; for (...){...}` walking forward from after a `(`, check for `cc_find_matching_paren` first.**
- **Per-line scanner with `break` on inert**: the anon at ~3170 had `if (in_lc) break;` instead of `continue;` — the slice was a single source line so `\n`-terminated comments effectively halted the search.  Migrated form uses `continue;` (the natural CCInertScan rewrite); because the slice still has no `\n`, the scanner stays in `in_line_comment` for the rest of the iterations, so `memcmp` never runs while inert.  Behaviorally equivalent on single-line slices, more correct on multi-line ones (which this caller doesn't pass).
- **Realloc-leak fix in `cc__rewrite_idents`**: the original `out = realloc(out, cap); if (!out) return NULL;` leaks the previous `out` if realloc fails.  My migration only changed one of the three realloc sites (the new inert-bulk-copy one); the other two still have the original pattern.  Recorded as a future cleanup item: "audit rewrite passes for realloc-leak-on-failure pattern".
- **No new patterns** otherwise — every site followed an already-documented template.

J3 (complex, separate commit) — **LANDED 2026-05-27**:
- [x] `cc__emit_awaits_in_expr` — canonical rewrite template (outer) + indexed-walk operand scanner (inner); outer scanner re-initialized after operand-skip to clear stale `in_*` state (the inner pscan tracked the operand's inert spans, but outer scan never saw them)

**Actual diff**: +35 / −24 (net **+11 LOC** — the operand scanner's `if/break` split for unbalanced delims slightly out-grew the original inline state machine).  Full suite 461/461 both modes.

**Surprises:**
- **Stale-outer-state pattern applied for the first time** (the watch-out from Batch G).  After the outer loop jumps `i = expr_e;` past the operand, the inner pscan saw all the operand's inert content, but the outer scan didn't — so its `in_block_comment` / `in_pp` flags could mismatch reality.  Fix: `cc_inert_scan_init(&scan, NULL); scan.at_line_start = 0;` right before `continue;`.  Cheap and unambiguous.  Pattern: **when an outer scanner jumps past a region another scanner walked, re-init the outer.**
- **Operand-scanner unbalanced-delim split**: the original collapsed `']'`/`'}'` unbalanced-terminator and depth-decrement into a single conditional (`else if (c == ']') { if (pbrk) pbrk--; }` then later `if (c == ']') break;` at depth-0).  Migrated into explicit per-character if/break/continue arms for clarity.  Same semantics, slightly more LOC.
- **Latent realloc-leak fixed** in the rewrite path: outer `realloc(out, out_cap); if (!out) return NULL;` leaks the old pointer; replaced with `tmp = realloc(...); if (!tmp) { free(out); return NULL; } out = tmp;`.  Same pattern as the J1 `cc__rewrite_idents` fix.

### Batch K — closure literal pass (11 sites + helper, 2–3 commits)

K1 — precursor helper + auto-collapse — **LANDED 2026-05-27**:
- [x] **Deleted `cc__scan_skip_string_comment` helper** (~35 LOC) plus both forward decls.  Was a stateless poor-man's CCInertScan that only handled strings/chars/comments (no `#line`, no pp directives, no stateful multi-line tracking).
- [x] `cc__find_next_arrow_skipping_inert` — drop-in replacement; the defensive `if (i == before) i++;` guard no longer needed (CCInertScan always advances)
- [x] `cc__find_prev_arrow_skipping_inert` — drop-in replacement; same defensive guard removed
- [x] `cc__addr_of_is_readonly_call` arg-index inner loop — `for` → `while` with `lim = min(amp_off, n)` upfront
- [x] `cc__find_mutation_in_body` — `for` → `while` with body-`continue;` audit: 6 early-skip continues each got `{ i++; continue; }`; final `i = j - 1;` (relied on for's `++i`) → `i = j;`
- [x] Recovery scan in main file (~2880) — drop-in replacement; defensive guard removed

**Actual diff**: +55 / −91 (net **−36 LOC**).  Full suite 461/461 both modes.

**Surprises:**
- **Defensive `if (i == before) i++;` guards become dead code**.  Old `cc__scan_skip_string_comment` could theoretically return 1 without advancing in pathological cases (e.g. lone `'` at EOF — actually it does advance, but the guards were preemptively defensive).  CCInertScan's contract guarantees `*pos` strictly advances when it returns 1.  Pattern: **migrating away from old per-pass scan helpers lets us drop defensive infinite-loop guards.**
- **Body-`continue;` audit was the hard part of K1**: `cc__find_mutation_in_body` had 6 early-skip continues (`!is_ident_start`, `is_ident_char`, bounds, `strncmp`, post-ident-char, struct field access).  Each needs `{ i++; continue; }`.  Skipped the body-continue trap (Batch F1) with no test failures because the pre-migration audit caught all 6.
- **K1 cost ≈ 5 sites of effort, K2+K3 cost not yet reduced** — the "auto-collapse" framing in the audit was optimistic.  None of the K2/K3 sites used `cc__scan_skip_string_comment`; they have their own inline state machines that still need migrating.  Net effect of K1: −36 LOC and one shared helper retired, but K2/K3 work is unchanged.

K2 — trivial bulk (7 sites) — **LANDED 2026-05-27**:
- [x] 4× anon in `cc__maybe_record_decl_stmt` — pointer-walks (`const char* cur_scan` over `[p, semi)`) converted to indexed walks using `stmt` as base; pointer outputs (`name_s`, `comma_pos[]`, `eq`) preserved via `stmt + i` at scan exit
- [x] `cc__maybe_record_decl` — outer pointer-walk on `line` converted to indexed; calls `cc__maybe_record_decl_stmt` with `line + stmt_off` / `line + semi_off` (the recorder's pointer interface unchanged)
- [x] `cc__last_top_level_semi_offset` — returns offset directly (was returning `(size_t)(last - line)` pointer subtraction)
- [x] `cc__src_strip_comments_and_strings` — **inert-kind discrimination** pattern (Batch F1 twin): snapshot pre-step `scan.in_str`/`scan.in_chr`, blank consumed `[before, i)` bytes EXCEPT (i) newlines (preserve line numbers) and (ii) string/char delimiters on entry/exit transitions

**Actual diff**: +128 / −208 (net **−80 LOC**).  Full suite 461/461 both modes.

**Surprises:**
- **Pointer-walk → indexed walk pattern, 2nd application** (after Batch F2's `cc__emit_closure_field_call`).  When the function's outputs are pointers but the inert scan is interior, convert offset ↔ pointer at the scan boundaries: `size_t p_off = p - base;` going in, `name_s = base + s_off;` coming out.  Keeps the function's public interface stable while the inner loop is properly indexed.
- **`cc__src_strip_comments_and_strings` behavior change for pp directives**: original didn't track `#line`/`#define`/etc. as inert, so a `#` byte leaked through as code; downstream decl scanners relied on the `if (*p == '#' || *p == '\0') return;` guard.  Migrated version blanks pp-directive bodies (CCInertScan's `in_pp` is on by default).  The guard is now mostly defensive — but a `#` at the START of a pp directive (the `#` byte itself) DOES get blanked.  This is a strict improvement: stripped decl scanners no longer encounter raw `#`s.
- **Delimiter-preservation works cleanly with snapshot pre/post**: `c == '"' && pre_str != post_str` catches BOTH the opening `"` (pre=0, post=1) and closing `"` (pre=1, post=0).  Same for `'` and `in_chr`.  Pattern: **when an inert-kind rewrite needs to preserve specific entry/exit bytes, snapshot pre-step flags and use XOR-like comparison.**

K3a — medium (3 sites) — **LANDED 2026-05-27**:
- [x] `cc__has_top_level_brace` — straight migration; callers pass comment-stripped text so behavior is unchanged in practice (now defensively comment-aware for free)
- [x] `cc__parse_closure_from_src` body-brace scan — replaced 22 LOC inline state machine with single `cc_find_matching_brace(s, n, body_start, &rbrace)` call.  3rd application of the "use util/text.h's matching helpers" pattern.
- [x] `cc__closure_proto_insert_off` — preserves `last_line_off` line tracking via the Batch C pattern (post-step `[before, i)` newline sweep).  Code-path `\n` handler still explicit at the top of the code-byte branch.

**Actual diff**: +37 / −114 (net **−77 LOC**).  Full suite 461/461 both modes.

**Surprises:**
- **`cc_find_matching_brace` 3rd sighting**: after Batches I and J each used it for nested scanners, K3a's `cc__parse_closure_from_src` body-brace site collapsed the same way.  This shared helper has now killed ~80 LOC of duplicated brace-balance inline state machines across 3 files.  **Standing recommendation: any inline `int br = 0; for (...) { if (ch == '{') br++; ... }` is a candidate.**
- **`last_line_off` tracking in `cc__closure_proto_insert_off`** worked cleanly with Batch C's post-step sweep — no surprises.  The function's `best_func_off = last_line_off;` assignment runs on the code-byte branch (after `{` is detected at depth 0), so the line tracking only needs to be CURRENT, not retroactively-perfect for inert bytes.

K3b — complex (1 site, deferred to next commit):
- [ ] `cc__infer_closure_end_off` (complex — dual nested, real bug history)

> **Pivot decision point**: the artifact suggested optionally interleaving with **4b (stable closure-IDs)** before K3b, since 4b deletes ~150 LOC of the same surface (the heuristic `=>` recovery path that `cc__infer_closure_end_off` services).  Recommendation: **do K3b first** anyway — it's bounded enough (1 function, ~80 LOC) and finishes M1 Phase 2.  4b can then proceed independently with one less stale scanner to worry about.

### Batch L — backward scanners (3 sites, 1 commit; possibly + helper)

> Goal: decide on the backward-scan helper investment, then sweep all three.

Sub-decision (commit-time): extend `text_scan.h` with `CCInertScanBackward` (similar API but walks backward), OR migrate each backward site independently using existing per-pass companions.

- [ ] `pass_result_unwrap.c::cc__find_lhs_start_backward_raw`
- [ ] `pass_err_syntax.c::cc__err_stmt_start_backward`
- [ ] `pass_unwrap_destroy.c::cc__ud_stmt_start_backward`

### Batch M — special cases (1–2 commits)

- [ ] `visit_codegen.c::cc__collect_legacy_ufcs_registrations` — verify behavioral equivalence with `CCInertScan`'s `#line` tracking; consolidate or keep separate

---

## Phase 3 — closure-ID cleanup (optional, interleavable with K)

If Batch K3 proves expensive, pivot to **4b** (stable closure-IDs) first — see `COMPILER_CLEANUP_STATUS.md` "Recommended next work" item **4b** at line ~349.  Deletes ~150 LOC from `pass_closure_literal_ast.c` and removes the heuristic `=>` recovery path entirely.  Net effect: K3's hardest sites shrink or disappear.

---

## Phase 4 — the flip

After all batches green:

- [ ] Add `tests/m1_basename_collision_smoke.ccs` — user TU named `cc_channel.ccs` to verify `CCInertScan.in_user_file` doesn't false-match a header with the same basename.  (Optional: extend `cc__inert_scan_path_is_user_tu` to compare a path component or two beyond basename.)
- [ ] In `visit_codegen.c`, swap the `src_all = cc__read_entire_file(ctx->input_path)` source to `src_all = strdup(root->parse_buffer)` (or equivalent) when `CC_PRE_EXPAND` is on.
- [ ] Run full smoke both modes.  Expect regressions — debug each by either (i) auditing the offending pass's `in_user_file` filter usage or (ii) extending `CCInertScan` if a real corner case shows up.
- [ ] Remove the `.env` pin on `tests/m0_5_diag_origin_line_fail.ccs` once green.
- [ ] Update `COMPILER_CLEANUP_STATUS.md` "Recommended next work" item 2 (M1 visitor refactor): mark (a)/(b)/(c) all DONE; promote to "Shipped (complete)".

---

## How to update this doc

When you finish a batch:

1. Tick the `- [ ]` boxes for migrated sites under that batch.
2. Update the **Status snapshot** table at the top:
   - Decrement "Remaining to migrate" by the batch size.
   - Increment "Already on `CCInertScan`" by the same.
   - Update smoke baseline if it changes (it shouldn't until Phase 4).
3. If you discovered a surprise (e.g. a site you thought was trivial was actually medium), add a one-line post-mortem in the relevant per-file table's Notes column.
4. If the batching order needs to change (e.g. K turned out to depend on G), update the batch order and note the dependency.
5. If you split a batch into two commits, list both with their hashes.
6. Commit the doc update alongside the migration commit (single commit per batch is the goal).

When you finish Phase 2:

- Move "Status snapshot" line "Remaining to migrate" to 0.
- Add a "Phase 2 close" date and link to the final batch commit.
- Proceed to Phase 4.

This doc is the **single source of truth for M1 progress**.  Keep it accurate; it tells the next person (or you, three weeks later) exactly where to pick up.
