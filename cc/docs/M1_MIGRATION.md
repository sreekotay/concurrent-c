# M1 — Visitor refactor (migration tracker)

**Status:** Phase 1 (audit) complete — 2026-05-27.  Phase 2 in progress (10 / 14 batches landed — A, B, C, D1, D2, E, F1, F2, G, H1).

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
| **(c)** `#line`-aware text scanners | every visitor pass that walks `src_all` filters by origin file via `CCInertScan` | **PARTIAL** — 65 sites migrated, 37 remaining (this doc) |

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

Updated 2026-05-27 (post Batch H1).

| Metric | Count |
|--------|-------|
| Total scanner sites (inline + migrated) | **102** |
| Already on `CCInertScan` | **65** (+6 from Batch H1) |
| Remaining to migrate | **37** |
| — trivial | 28 |
| — medium | 4 |
| — complex | 5 (or 7 counting full-file rewrites) |

Smoke at last batch close: **461/461** both pre-expand-on (default) and `CC_PRE_EXPAND=0`.

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

### `pass_with_deadline_syntax.c` (412 LOC, 5 inline + 1 migrated outer, has `text_scan.h`)

**Migrated:** outer loop of `cc__collect_with_deadline_edits` (~274).

| Site | Shape | State | LOC | Complexity | Notes |
|------|-------|-------|-----|------------|-------|
| `cc__rewrite_with_deadline_syntax` main loop (~11) | rewrite | {lc,bc,str,chr} | ~85 | medium | Legacy path; contains 2 nested scanners |
| anon loop in `cc__rewrite_with_deadline_syntax` (~136) | find-only | {str,qch,lc,bc} | ~15 | trivial | |
| anon loop in `cc__rewrite_with_deadline_syntax` (~191) | find-only | {str,qch,lc,bc} | ~15 | trivial | |
| anon loop in `cc__collect_with_deadline_edits` (~321) | find-only | {str,qch,lc,bc} | ~15 | trivial | Nested INSIDE migrated outer |
| anon loop in `cc__collect_with_deadline_edits` (~355) | find-only | {str,qch,lc,bc} | ~15 | trivial | |

### `pass_err_syntax.c` (1468 LOC, 6 inline + 1 migrated, has `text_scan.h`)

**Migrated:** `cc__find_matching_brace_text` (~213).

| Site | Shape | State | LOC | Complexity | Notes |
|------|-------|-------|-----|------------|-------|
| `cc__scan_stmt_end_semicolon` (~152) | find-only | {str,qch,lc,bc} | ~35 | trivial | Duplicate of defer's |
| `cc__err_pos_in_line_comment` (~290) | find-only | {str,qch} | ~16 | trivial | |
| `cc__err_stmt_start_backward` (~311) | find-only | hybrid backward | ~40 | **complex** | **Backward** — see cross-cutting risk #1 |
| `cc__expand_delegations` (~523) | rewrite | {str,qch,lc,bc} | ~55 | medium | |
| `cc__rewrite_colon_defaults` (~700) | rewrite | {str,qch,lc,bc} | ~45 | medium | |
| `cc__rewrite_err_core` main loop (~859) | rewrite | {str,qch,lc,bc,pp,als} | ~100 | **complex** | Full pass + `ito[]` offset map.  Highest-risk in file |

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

### Batch I — pass_with_deadline_syntax + pass_err_syntax forward sites (8 sites, 1–2 commits)

I1 — with_deadline finish (5 sites):
- [ ] `cc__rewrite_with_deadline_syntax` main loop (medium) + its 2 nested
- [ ] `cc__collect_with_deadline_edits` 2 nested inside migrated outer

I2 — err forward sites (4 sites):
- [ ] `pass_err_syntax.c::cc__scan_stmt_end_semicolon`
- [ ] `pass_err_syntax.c::cc__err_pos_in_line_comment`
- [ ] `pass_err_syntax.c::cc__expand_delegations` (medium)
- [ ] `pass_err_syntax.c::cc__rewrite_colon_defaults` (medium)

I3 — err complex full-pass rewrite (separate commit):
- [ ] `cc__rewrite_err_core` main loop (complex — `ito[]` offset map)

### Batch J — async_ast forward sites (11 sites, 2 commits; complex separate)

J1 (8 sites, mostly trivial-medium):
- [ ] `cc__find_matching_paren`
- [ ] `cc__find_matching_brace`
- [ ] `cc__rewrite_idents` (medium — "no comments" flag)
- [ ] `cc__rewrite_typed_chan_await_expr` (medium — "no comments" flag)
- [ ] `cc__scan_simple_stmt_end`
- [ ] `cc__truncate_at_first_semicolon0`
- [ ] `cc__split_top_level_semis` (medium)
- [ ] `cc__rhs_has_top_level_comma`

J2 (3 sites, mixed):
- [ ] `cc__parse_loop_from_text` anon (medium)
- [ ] `cc__normalize_result_generic_bool_calls` (medium — "no comments" flag)
- [ ] `cc_async_rewrite_state_machine_ast` anon (medium)

J3 (complex, separate commit):
- [ ] `cc__emit_awaits_in_expr` (complex — nested rewrite + operand scanner)

### Batch K — closure literal pass (11 sites + helper, 2–3 commits)

K1 — precursor helper + auto-collapse:
- [ ] `cc__scan_skip_string_comment` helper migration; verify `cc__find_*_arrow_skipping_inert`, `cc__amp_is_const_param_read`, `cc__find_mutation_in_body`, recovery scan all collapse / simplify automatically.

K2 — trivial bulk (7 sites):
- [ ] 4× anon in `cc__maybe_record_decl_stmt`
- [ ] `cc__maybe_record_decl`
- [ ] `cc__last_top_level_semi_offset`
- [ ] `cc__src_strip_comments_and_strings` (rewrite template)

K3 — medium + complex (4 sites):
- [ ] `cc__has_top_level_brace` (medium — "no comments" deliberate?  audit first)
- [ ] `cc__closure_proto_insert_off` (medium — backward-ish)
- [ ] `cc__parse_closure_from_src` body-brace scan
- [ ] `cc__infer_closure_end_off` (complex — dual nested, real bug history)

> Optional: **interleave with 4b (stable closure-IDs)** here.  If `cc__infer_closure_end_off` migration proves painful, 4b deletes 150+ LOC of the same surface — could be cheaper to do 4b first.  Decision point at K3.

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
