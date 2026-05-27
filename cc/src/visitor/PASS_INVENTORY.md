# CCC Pass Inventory

This document maps all compilation passes and preprocessing transforms, with consolidation candidates.

**Last updated**: 2026-05-26 (post M0–M5.5 — see [COMPILER_CLEANUP_STATUS.md](../../docs/COMPILER_CLEANUP_STATUS.md), [PIPELINE.md](PIPELINE.md))

## Invariants for new text scanners (MUST follow)

These are *non-negotiable* for any new text-rewriting pass added to
`cc/src/visitor/pass_*.c`, `cc/src/preprocess/preprocess.c`, or
anywhere else that walks the source buffer looking for CC syntax
patterns.  Every single inert-region bug we've hit (commits `43e0ebc`,
`842dd8c`, `23ea0a5`, `22f2896`, `2aa5ad3`) is a violation of one of
these rules.

1. **No inline state machines.**  Do NOT declare local
   `int in_str` / `char qch` / `int in_line_comment` /
   `int in_block_comment` / `int in_pp` / `int at_line_start`
   variables in a new scanner.  Use `CCInertScan` from
   `cc/src/util/text_scan.h` instead.  It is the visitor-side
   shared scanner state — comment / string / char-literal /
   preprocessor-directive body / `#line` directive tracking, all
   in one place.
   Migration template: see `pass_result_unwrap.c::cc__find_bang_token_from`
   (commit `23ea0a5`) and the brace/paren finders in
   `visit_codegen.c` (commit `2aa5ad3`).

2. **No reinventing skip helpers.**  `cc/src/util/text.h` already
   exposes comment-aware substring / identifier / matching-bracket
   finders (`cc_find_substr_top_level`, `cc_find_ident_top_level`,
   `cc_find_matching_paren`, `cc_find_matching_brace`,
   `cc_find_matching_bracket`, `cc_rfind_char_top_level`,
   `cc_contains_token_top_level`).  Use them.  When extending them,
   add the new behavior to `text.h` so every caller benefits at
   once.  Do not copy-paste a parallel implementation into a pass.

3. **Every CC syntax token family MUST have an inert-region smoke
   test.**  When you add a new CC syntax pattern (a new `@keyword`,
   a new operator, a new bracket form), add a
   `tests/inert_<family>_tokens_smoke.ccs` that puts the new
   token inside a block comment, a line comment, a string literal,
   a char literal, AND a `#define` body alongside real working
   code.  The existing `tests/inert_*_tokens_smoke.ccs` files are
   the templates.  Without this, the next refactor of your scanner
   will silently regress in one of these contexts.

4. **`#line` directives are first-class.**  Any scanner that
   operates on the post-preprocess buffer (most visitor passes do)
   must respect `CCInertScan.in_user_file` before acting on a
   token.  In the M1 final form this filters out matches that came
   from inlined CC runtime headers (`<ccc/cc_channel.cch>`,
   `<ccc/std/vec.cch>`, etc.).  Today the flag is always 1 because
   `src_all` is still the raw user file, but writing pass code
   that *ignores* the flag now creates a latent bug that bites the
   moment the M1 swap lands.

5. **Reparses are expensive — fewer is better.**  Before adding
   another `cc__reparse_source_to_ast_ctx` call site, ask whether
   the same effect can be achieved via `CCEditBuffer` (in-place
   edits) or by extending an existing reparse to do more work.
   Today's count is documented under "Current Stats" below; any
   PR that adds a reparse call site must update that count and
   justify the addition.

6. **Type-registry ratchet.**  Do NOT add new
   `cc_type_registry_get_global()` / `cc_type_registry_set_global()`
   callers.  The global registry is a thread-local ambient
   context with ~50 existing callers — the goal is to ratchet
   that count DOWN, not let it grow.  New code paths should
   accept an explicit `CCTypeRegistry*` parameter.  When you
   genuinely need to scope a *temporary* registry around a call
   (e.g. comptime preprocess isolation), use the
   `cc_type_registry_scope_push/pop` helpers from
   `cc/src/preprocess/type_registry.h` instead of the open-coded
   save / new / set / restore / free dance — it's wrong on at
   least three of the ~9 existing sites in subtle leak-on-error
   ways.

7. **Closure-identity ratchet.**  Do NOT add new callers of
   `cc__closure_start_off_best_effort` or the descriptor
   "recovery" branch in `pass_closure_literal_ast.c`.  Both are
   heuristic fallbacks for a deeper problem: closures are
   matched between passes by `(line_start, line_end, col_start)`,
   which drifts whenever a reparse pulls in a header with
   `#line` directives.  The right fix is stable closure-IDs
   injected as `/*CC_CLO:N*/` markers before TCC parses — see
   the "Stable closure-IDs" milestone (#4b) in
   `COMPILER_CLEANUP_STATUS.md`.  Until that ships, new closure-
   walking code should compare full `(file, line, col, end)`
   tuples (never a single coord) and bail loudly on ambiguous
   matches rather than picking the "best" one.

If your change can't follow these rules, the right move is to
update the helpers (`text_scan.h`, `text.h`, `type_registry.h`)
so it CAN, not to work around them in a single pass.

----

## Current Stats

- **Total lines**: ~21k across pass files
- **TCC reparses**: ~9 call sites in `visit_codegen.c` + 1 initial parse (not 4)
- **Text-based passes in preprocess.c**: 19 functions
- **AST-based passes**: 8

## Preprocessing (preprocess.c) — ~4,800 lines

Text transforms applied BEFORE TCC parsing. Listed in execution order:

| # | Function | Transform | Lines | Notes |
|---|----------|-----------|-------|-------|
| P1 | cc__rewrite_with_deadline_syntax | `with_deadline(ms)` → CCDeadline scope | ~240 | Control flow |
| P2 | cc__rewrite_match_syntax | `@match` → switch + cc_chan_match_select | ~310 | Channel select |
| P3 | cc__rewrite_slice_types | `T[:]` → CCSlice_T | ~110 | Type syntax |
| P4 | cc__rewrite_chan_handle_types | `int[~4 >]` → CCChanTx_int | ~510 | Channel types |
| P5 | cc_rewrite_generic_containers | `Vec<T>` → Vec_T | ~250 | Generic types |
| P6 | cc__rewrite_optional_types | `T?` → diagnostic (retired) | ~60 | Type syntax (emits error) |
| P7 | cc__rewrite_inferred_result_ctors | `cc_ok(v)` → `cc_ok_CCResult_T_E(v)` | ~260 | Constructor inference ⚠️ BEFORE P10 |
| P8 | cc__rewrite_result_types | `T!>(E)` → CCResult_T_E | ~155 | Type syntax |
| P9 | cc__rewrite_result_constructors | `cc_ok_CCResult_T_E(v)` → macro | ~70 | Parse stub |
| P10 | cc__normalize_if_try_syntax | `if @try (` → `if (try ` | ~25 | Syntax normalize |
| P11 | cc__rewrite_try_binding | `if (try T x = expr)` → expanded | ~150 | Result unwrap |
| P12 | cc__rewrite_try_exprs | `try expr` → `cc_try(expr)` | ~95 | Result unwrap |
| P13 | cc__rewrite_optional_unwrap | `cc_try(r)` for CCResult (optional arm retired) | ~180 | Result unwrap |
| P14 | cc__rewrite_closing_annotation | `@closing(ch)` → sub-nursery | ~150 | Channel lifecycle |
| P15 | cc__rewrite_cc_concurrent | `cc_concurrent { }` → closure exec | ~70 | Concurrency |
| P16 | cc__rewrite_link_directives | `@link("lib")` → linker comment | ~460 | Link directives |

**Note**: P8 must run before P10 (needs to see `T!>(E)` syntax for type inference).

**Consolidation candidates:**
- P8+P9+P16 → single "optional pass" (3 scans → 1)
- P11+P12 → single "result types pass" (2 scans → 1, P10 stays separate due to ordering)
- P3+P4+P5 → single "type syntax pass" (potential, needs analysis)

## visit_codegen.c Pipeline — 1,214 lines

### Phase 1: Early Text (before TCC parse)

| # | Pass File | Function | Transform |
|---|-----------|----------|-----------|
| 1 | visit_codegen.c | cc__rewrite_closing_annotation | `@closing(ch)` → sub-nursery |
| 2 | visit_codegen.c | cc__rewrite_if_try_syntax | `if try` → result unwrap |

### Phase 2: Text Passes (batched)

| # | Pass File | Lines | Transform |
|---|-----------|-------|-----------|
| 3 | pass_with_deadline_syntax.c | 430 | `with_deadline(ms)` → CCDeadline + @defer |
| 4 | pass_match_syntax.c | 600 | `@match` → switch + cc_chan_match_select |

### Phase 3: Initial AST Passes (EditBuffer; sequential reparse by default)

| # | Pass File | Lines | Transform |
|---|-----------|-------|-----------|
| 5 | pass_ufcs.c | 432 | `x.method(y)` → `method(&x, y)` |
| 6 | pass_closure_calls.c | 748 | `c(x)` → `c.fn(c.env, x)` |
| 7 | pass_autoblock.c | 1,260 | Insert cc_block() wrappers |
| 8 | pass_await_normalize.c | 529 | `await expr` → temp binding |

**M2 status (2026-05-26):**
- **Default:** sequential `cc__apply_coarse_codegen_pass` + reparse between collectors (429 smoke tests).
- **Experimental:** `CC_BATCH_PHASE3=1` → `cc__apply_batched_phase3_passes()` (one apply, one reparse). Not default until AST merge ordering is fixed.

### Phase 4: Channel Syntax (text, REPARSE #2)

| # | Pass File | Lines | Transform |
|---|-----------|-------|-----------|
| 9 | pass_channel_syntax.c | 894 | channel_pair + `int[~4 >]` → CCChanTx |
| 10 | pass_type_syntax.c | 1,319 | slice/optional/result type text rewrites |

### Phase 5: Closure Literals (REPARSE #3)

| # | Pass File | Lines | Transform |
|---|-----------|-------|-----------|
| 11 | pass_closure_literal_ast.c | 2,094 | `() => {...}` → __cc_closure_make_N |

### Phase 6: Structured Concurrency (batched, REPARSE #4)

| # | Pass File | Lines | Transform |
|---|-----------|-------|-----------|
| 12 | pass_nursery_spawn_ast.c | 1,071 | spawn/nursery lowering (**ORPHAN**: `cc__collect_nursery_edits` unwired; nursery in preprocess + closure_literal) |

### Phase 7: Defer (text)

| # | Pass File | Lines | Transform |
|---|-----------|-------|-----------|
| 14 | pass_defer_syntax.c | 494 | `@defer stmt;` → inject before } and return |

### Phase 8: Async State Machine (REPARSE #5)

| # | Pass File | Lines | Transform |
|---|-----------|-------|-----------|
| 15 | async_ast.c | 2,524 | @async fn → state machine |

### Other

| # | Pass File | Lines | Transform |
|---|-----------|-------|-----------|
| 16 | pass_strip_markers.c | 104 | Strip @async/@noblock/@latency_sensitive |
| 17 | checker.c | 1,657 | Semantic checks (slice move, provenance) |

## Consolidation Opportunities

### High Value (reduces reparses)

1. **Merge Phase 3 passes** — UFCS, closure_calls, autoblock, await_normalize
   - **Partial (2026-05-26):** `cc__apply_batched_phase3_passes()` behind `CC_BATCH_PHASE3=1`
   - **Default path:** still sequential reparses (see PIPELINE.md)
   - **Next:** make batching safe by default (fix AST/type state between collectors)

2. **Merge closure_literals + spawn/nursery** — share one reparse
   - BLOCKED: closure_literals uses coarse-grained whole-file edit
   - Would require refactoring to fine-grained edits
   - Could save: 1 reparse (3→2)

3. **Refactor Phase 6** — spawn/nursery lowering is now the only remaining pass there
   - Arena block lowering was retired with the surface syntax
   - Savings are now from simplification rather than fewer reparses

### Medium Value (reduces complexity)

4. **Consolidate preprocessor type transforms** (P8+P9+P16, P11+P12)
   - Currently: 19 separate text scans
   - Target: ~12 scans (merge related passes)
   - Note: P10 must stay separate (ordering constraint)

5. ✅ **Clean up pass chaining** in cc_preprocess_to_string_ex
   - Done: CCPassChain helper + CC_CHAIN macro (2026-02-01)

### Low Value (cleanup)

6. **Inline small passes** — strip_markers (104 lines) into visit_codegen.c
7. **Extract reparse helper** — common pattern used 5 times

## Completed Improvements

1. ✅ Compiler cleanup M0–M5.5 infrastructure (2026-05-26) — diag core, `cc_build_parse_input`, preprocess reparse APIs, `tcc_ext_api`, macro recognizer hooks; see [COMPILER_CLEANUP_STATUS.md](../../docs/COMPILER_CLEANUP_STATUS.md)
2. ✅ Update this inventory to match reality (2026-02-01)
3. ✅ Pass chaining helper in preprocess.c - CCPassChain + CC_CHAIN macro (2026-02-01)
4. ✅ Phase 3 EditBuffer infrastructure (2026-02-01); batched apply optional via `CC_BATCH_PHASE3` (2026-05-26)
5. ✅ Dynamic type registries - `cc__cg_result_types` and `cc__cg_optional_types` are
   now heap-allocated dynamic arrays (previously fixed [64]). No limit on Result/Optional
   type count per compilation unit. (2026-03-09)
6. ✅ Explicit registry reset - `cc__cg_reset_type_registries()` called once per
   compilation unit in visit_codegen.c. Scan functions now ACCUMULATE rather than
   implicitly resetting on each call. Previously the second call to any scan function
   within one compilation unit (e.g. from pass_closure_literal_ast.c) would silently
   discard types collected by the first call. (2026-03-09)
7. ✅ `spawn into` correctness - fixed `__spawn_into_thunk` to detect when the called
   function stores its result directly via cc_task_result_ptr (evidenced by returning
   the same buffer pointer). The thunk no longer overwrites the caller's structured
   result. The `spawn into(ch)?` form now uses discard-on-backpressure semantics
   (cc_task_free) instead of the incorrect cc_try propagation. (2026-03-09)

## AST Migration Investigation (2026-03-09)

### Why text-based rewrites are necessary for type syntax

The type passes (P3, P8, P11) rewrit `T[:]`, `T?`, and `T!>(E)` at the token level.
TCC's stub-AST does NOT emit AST nodes for these annotations — they exist only as
surface syntax tokens before TCC sees them. As a result:

- **Fully text-based is correct and necessary** for P3/P8/P11. No AST nodes to visit.
- The `CCScannerState` refactoring (all 12 passes) already extracted the common
  scanning boilerplate — this is the right level of abstraction.

### What CAN be migrated to AST-based

| Candidate | Blocker | Status |
|-----------|---------|--------|
| `spawn into` validation | Already AST-based via stub-AST `spawn_into` node | ✅ |
| Nursery nesting checks | Already AST-based via `@nursery` node matching | ✅ |
| Arena/await interaction check | Retired with `@arena` block syntax removal | ✅ |
| Result type collection | Only in text form (no AST nodes for `T!>E`) | BLOCKED |
| Try expr rewriting (P15) | Only in text form (no AST nodes for `try expr`) | BLOCKED |
| Closure literal lowering (P11) | Partially AST-based; whole-file edit blocks batching | MEDIUM |

### Reparse reduction: the real migration target

The `5 reparses` (AST passes that require TCC re-parsing) is the primary cost.
Reducing to 4 reparses requires making `pass_closure_literal_ast.c` use fine-grained
`CCEditBuffer` edits instead of whole-file text replacement. This would allow Phase 5
to batch with Phase 6 (spawn/nursery/arena). Effort: ~2-3 days. Not blocked technically,
only by refactoring risk.

## Known issues (closure literals in deeply-rewritten call sites)

Two pre-existing failures share a root cause area in `pass_closure_literal_ast.c`:

- `examples/recipe_tcp_echo.ccs` — `n->spawn(() => [sock] { handle_client(sock); })`
  inside `if (test_mode) { ... }` inside a nursery `{ ... } @destroy` block.
  **Layer 1 fixed (May 2026):** the block-scope `static __cc_closure_make_N(void);`
  decl that triggered `function without file scope cannot be static` was
  caused by `cc__closure_proto_insert_off` landing the legacy in-source
  forward decl inside the enclosing `if` block.  We now pass
  `skip_inline_protos=1` to `cc__rewrite_closure_literals_with_nodes_ex`
  from `visit_codegen.c` and place file-scope forward decls via the new
  `cc_find_first_func_def_offset` helper (just before the first
  top-level function definition, so user typedefs that appear between
  `#include`s and the first function — e.g. `HolReqTx` in
  `tests/redis_owner_reply_try_send_hol_smoke.ccs` — are still in scope).
  **Layer 2 fixed (May 2026 — verified after closure scanner fix):**
  the captured `sock` was previously reported as not unpacked from
  `__env`.  After the closure proto-placement refactor (Layer 1) plus
  the `=>`-scanner comment-skipping fix, `recipe_tcp_echo.ccs --test`
  builds AND runs end-to-end: the test client connects, sends
  `"hello from test"`, the server echoes 16 bytes back, both sides
  close cleanly, exit 0.  The underlying capture-emission code was
  always correct; what looked like a layer-2 bug was actually
  downstream fallout from layer-1's malformed forward decls and from
  the `=>`-in-comment scanner trap leaving the closure descriptor
  inconsistent.
- ~~`stress/syscall_kidnap.ccs` — `nursery->spawnhybrid(() => [id] { ... })`
  inside a `for` loop. The capture-variant closure literal is **not
  detected at all** (no descriptor produced); the raw `() => [id] { ... }`
  text leaks to the host C compiler.~~  **FIXED (May 2026).**  Root cause
  was not the capture variant itself but the closure scanner's
  *recovery / validation* path: a `// Pattern: (cancelled && no_work) => exit`
  line comment in `kidnapper_fiber` was being walked by the byte-level
  `=>` scanner without comment/string skipping, latching a fake closure
  descriptor onto the comment's `=>` and starving the real heartbeat
  closure of its descriptor.  Fix in `pass_closure_literal_ast.c`:
  added `cc__find_next_arrow_skipping_inert` / `_prev_` helpers that
  route through the existing `cc__scan_skip_string_comment` machinery,
  and rewired the four raw `=>` scan loops (best-effort start/end,
  validation, recovery) to use them.  Regression guards live in
  `tests/inert_*_tokens_smoke.ccs` (one per CC scanner family).

The proto-placement layer (Layer 1) is fixed; the syscall_kidnap
detection layer is fixed; the `recipe_tcp_echo.ccs` capture-emission
layer (Layer 2 above) remains open.

The legacy `cc__closure_proto_insert_off` walker is still present in
`pass_closure_literal_ast.c` for backward compat with the default
(`skip_inline_protos=0`) path, but no in-tree caller exercises it.  It
can be deleted once we're sure no out-of-tree consumer depends on it.

## Next Steps

1. **Reparse reduction (5→4)**: Make closure_literals use CCEditBuffer (fine-grained edits)
   - Current: whole-file edit can't batch with spawn/nursery/arena
   - Effort: Medium (2-3 days)
   - Saves: 1 TCC reparse per compilation unit

2. **Optional/Result pass merging** (P8+P9, P11+P12): DEFERRED
   - Analysis: 19 → 17 scans = ~10% reduction, minimal real impact
   - Note: P16 must stay separate (runs after try passes)
   - Decision: Not worth the refactoring effort

3. **Dead code removed**: visitor_pipeline.c was deleted (2026-02-01)
   - The Phase 3 consolidation from there was already applied to visit_codegen.c

## Summary

The major consolidation wins have been achieved:
- ✅ CCPassChain helper (cleaner pass chaining)
- ✅ Phase 3 EditBuffer batching (4 passes → 1 apply)
- ✅ CCScannerState refactoring (5 passes converted)
- ✅ Documentation updated

### CCScannerState Refactoring Status

All suitable passes have been converted to use the shared `CCScannerState` helper:

**Converted (12 passes):**
- `cc__rewrite_optional_types` (P8)
- `cc__rewrite_result_types` (P11)
- `cc__rewrite_slice_types` (P3)
- `cc__rewrite_try_exprs` (P15)
- `cc__rewrite_optional_unwrap` (P16)
- `cc__rewrite_match_syntax` (P2)
- `cc__rewrite_optional_constructors` (P9)
- `cc__rewrite_result_constructors` (P12)
- `cc__rewrite_chan_handle_types` (P4) ✅ 2026-02-01
- `cc_rewrite_generic_containers` (P5) ✅ 2026-02-01
- Legacy preprocess UFCS passes removed; UFCS now lowers through the parser/TCC tolerance plus AST-aware UFCS path.

**Not suitable (1 instance):**
- `cc__rewrite_with_deadline_syntax` - streaming output architecture incompatible

Remaining opportunities are either blocked (reparse reduction) or low-value (pass merging).
