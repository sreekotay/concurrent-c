# CCC Pass Inventory

This document maps all compilation passes and preprocessing transforms, with consolidation candidates.

**Last updated**: 2026-07-20 (invariant #8 rewritten — closure markers are now the ONLY identity) — prior: 2026-06-01 (drift audit vs code: line counts, phantom/retired passes, omitted passes), 2026-05-28 (post Phase-3 two-stage batched flip — see [COMPILER_CLEANUP_STATUS.md](../../docs/COMPILER_CLEANUP_STATUS.md), [PIPELINE.md](PIPELINE.md))

> **2026-07 span-anchored-passes cycle:** per-pass line counts and fragility
> notes below are the 2026-06-01 snapshot; the July cycle (recorder byte
> offsets on stub nodes, the reparse-diet exact-offset invariant
> `parse_src_shift`/`parse_src_valid_from`, marker-only closure identity,
> shared offset helpers in `pass_common.h`, scanner-debt paydown +
> `make lint-scanners` ratchet, `make test-strict`) is chronicled in
> [`PASS_CLEANUP_PLAN.md`](../../docs/PASS_CLEANUP_PLAN.md), which wins on
> conflict.

> **WHY this many passes? WHY this split between text and AST?** See [`ARCHITECTURE.md`](../../docs/ARCHITECTURE.md) §2 (the four constraints) and §3 (the three architectural layers). The Phase 1–9 numbering below is an artifact of how the code grew; the three layers in ARCHITECTURE.md are the right mental model.

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

7. **User-facing surface ratchet.**  A CC pipeline papercut
   (TCC stub-AST rejects standard C, parse-time vs codegen-time
   phase ordering, packed storage on public structs) MUST land
   INSIDE the compiler/runtime, NOT leak into user source or
   smoke tests.  Adding things like `(uint32_t)` casts,
   `__builtin_X` substitutions, magic-literal workarounds for a
   missing helper, or `#ifdef CC_PARSER_MODE` branches to
   user-facing headers is a code smell — the *user* should write
   idiomatic C with CC extensions; constraints that force
   unidiomatic spellings belong on our side of the line.
   When you bump into one of these, the options are (in order of
   preference): (a) hide it inside a macro/header/runtime helper
   so user code stays clean (see `kilobytes` in `cc_arena.cch`,
   the typed accessors `cc_ti_kind`/`cc_ti_size`/... in
   `cc_type.cch`); (b) add an entry to milestone "L2 — pre-parse
   rewrite pass for standard C idioms" in
   `COMPILER_CLEANUP_STATUS.md`; (c) escalate to the TCC stub-AST
   audit (milestone L3).  Adding the workaround to a test file
   without one of (a–c) requires explicit justification in the
   commit message.
   Concrete pattern: `__attribute__((constructor(N)))` with a
   priority argument trips TCC's stub-AST — drop the priority
   and rely on the registry being insertion-order-agnostic.

8. **Closure identity IS the `/*CC_CLO:N*/` marker.**  (Landed:
   the old best-effort resolver and its recovery branch are
   deleted — 2026-07-20.)  Every closure-literal producer MUST
   emit a marker (parse-build does it for user closures;
   autoblock marks its synthesized wrappers), and after
   `#if`-skip pruning the marker count MUST equal the in-TU
   closure-node count — a mismatch is a hard error, never a
   heuristic retry.  Do NOT add coordinate-matching or text-scan
   fallbacks for closure identity, and do NOT add a disable knob
   (`CC_NO_CLOSURE_MARKERS` was removed as a footgun).  New
   closure producers must either emit a marker or fail loudly.

If your change can't follow these rules, the right move is to
update the helpers (`text_scan.h`, `text.h`, `type_registry.h`)
so it CAN, not to work around them in a single pass.

----

## Current Stats

- **Total lines**: ~21k across pass files
- **TCC reparses**: ≤6 call sites in `visit_codegen.c` + 1 initial parse (down from ~9 pre-2026-05-28). Both Phase-3 stage reparses skip when their edit buffer is empty.
- **Text-based passes in preprocess.c**: ~24 transforms (2026-06-01 audit — the numbered table below is the historical subset; the actual phase-1 canonical + phase-3 host-lowering buckets in `cc__apply_phase1_canonical_passes` / `cc__apply_phase3_host_lowering_passes` run more, e.g. `cc__resolve_comptime_if`, `cc__rewrite_string_templates`, `cc__rewrite_free_call_families`, `cc__rewrite_channel_pair_pass`, `cc__lower_type_of_constexpr`, `cc__rewrite_result_field_sugar_pass`, `cc__rewrite_async_void_ret`, `cc__rewrite_at_call_site_mode`, `cc__rewrite_at_await`)
- **AST-based passes**: 8

## Preprocessing (preprocess.c) — ~10,803 lines

Text transforms applied BEFORE TCC parsing. Listed in execution order:

| # | Function | Transform | Lines | Notes |
|---|----------|-----------|-------|-------|
| P1 | cc__canonicalize_with_deadline_syntax (phase-1) + cc__lower_with_deadline_syntax (phase-3) | `with_deadline(ms)` → CCDeadline scope | ~240 | Control flow. **Corrected 2026-06-01:** lowering lives in `preprocess.c`; the visitor `pass_with_deadline_syntax.c` was a confirmed-orphan and has been deleted (2026-06-01). |
| P2 | ~~cc__rewrite_match_syntax~~ → cc__reject_match_syntax | **Removed (2026-07):** the `@match` construct was dropped from the language; the phase-1 slot now rejects the reserved token with a migration error. Callers use `cc_chan_match_select` directly. | ~50 | Channel select (reserved keyword) |
| P3 | cc__rewrite_slice_types | `T[:]` → CCSlice_T | ~110 | Type syntax |
| P4 | cc__rewrite_chan_handle_types | `int[~4 >]` → CCChanTx_int | ~510 | Channel types |
| P5 | cc_rewrite_generic_containers | `CCVec::[T]` → CCVec_T | ~250 | Generic types |
| P6 | cc__rewrite_optional_types | `T?` → hard-error (optionals retired); diagnostic-only guardrail | ~60 | Type syntax (emits error) |
| P7 | cc__rewrite_inferred_result_ctors | `cc_ok(v)` → `cc_ok_CCResult_T_E(v)` | ~260 | Constructor inference ⚠️ BEFORE P10 |
| P8 | cc__rewrite_result_types | `T!>(E)` → CCResult_T_E | ~155 | Type syntax |
| ~~P9~~ | ~~cc__rewrite_result_constructors~~ | ~~`cc_ok_CCResult_T_E(v)` → macro~~ | — | **Retired (2026-06-01 audit):** typed Result ctors now parse/type-check as-is; removed from the phase-3 chain (see `// (retired) cc__rewrite_result_constructors` in preprocess.c). |
| P10 | cc__rewrite_result_star_unwrap | `*res` → `cc_unwrap(res)` for CCResult | ~180 | Result unwrap |
| ~~P11~~ | ~~cc__rewrite_closing_annotation~~ | ~~`@closing(ch)` → sub-nursery~~ | — | **Phantom/retired (2026-06-01 audit):** no such function exists; `@closing(...)` is retired and now hard-errors in `cc/src/parser/cc_ext_parser.c` ("use `@create(...) @destroy { chan.close(); }`"). |
| P12 | cc__rewrite_cc_concurrent | `cc_concurrent { }` → closure exec | ~70 | Concurrency |
| P13 | cc__rewrite_link_directives | `@link("lib")` → linker comment | ~460 | Link directives |

**Note**: P8 must run before P10 (needs to see `T!>(E)` syntax for type inference).

**Consolidation candidates:**
- P3+P4+P5 → single "type syntax pass" (potential, needs analysis)

## visit_codegen.c Pipeline — ~4,886 lines

### Phase 1: Early Text (before TCC parse)

| # | Pass File | Function | Transform |
|---|-----------|----------|-----------|
| 1 | — | ~~cc__rewrite_closing_annotation~~ | **Phantom (2026-06-01 audit):** function does not exist; `@closing(...)` is retired and hard-errors in `cc_ext_parser.c`. |

### Phase 2: Text Passes (batched)

| # | Pass File | Lines | Transform |
|---|-----------|-------|-----------|
| 3 | ~~pass_with_deadline_syntax.c~~ | — | **Deleted (2026-06-01):** confirmed-orphan visitor pass removed (zero callers); `with_deadline` is lowered in `preprocess.c` (P1). |
| 4 | ~~pass_match_syntax.c~~ | — | **Deleted (2026-06-01):** confirmed-orphan visitor pass removed (zero callers). (`@match` itself was removed from the language 2026-07; the preprocess slot P2 now only rejects the reserved token.) |

### Phase 3: Initial AST Passes (EditBuffer; two-stage batched, 2 reparses max)

| # | Pass File | Lines | Transform | Stage |
|---|-----------|-------|-----------|-------|
| 5 | pass_ufcs.c | 1,110 | `x.method(y)` → `method(&x, y)` | 1 |
| 6 | pass_closure_calls.c | 750 | `c(x)` → `c.fn(c.env, x)` | 2 |
| 7 | pass_autoblock.c | 2,102 | Insert cc_block() wrappers | 2 |
| 8 | pass_await_normalize.c | 505 | `await expr` → temp binding | 2 |

**Pipeline status (2026-05-28):**
- **Default and only path:** `cc__apply_batched_phase3_passes()` runs in two stages — Stage 1 (UFCS only) and Stage 2 (closure_calls + autoblock + await_normalize batched into a single edit buffer). 461/461 smoke tests pass.
- Stage 1 is mandatory because UFCS *produces* new call sites that Stage 2's AST-driven passes must observe in the reparse.
- Stage 2's three passes target disjoint constructs and emit non-overlapping per-span edits, so they compose into one collect+apply+reparse cycle.
- Reparse cost: 2 max in Phase 3 (was 4 in the legacy sequential path). Stage reparses skip when their edit buffer is empty.
- The legacy sequential branch and `CC_BATCH_PHASE3=1` env-var gate were removed; `cc__apply_coarse_codegen_pass()` is deleted.

### Phase 4: Channel Syntax (text, REPARSE #2)

| # | Pass File | Lines | Transform |
|---|-----------|-------|-----------|
| 9 | pass_channel_syntax.c | 2,167 | channel_pair + `int[~4 >]` → CCChanTx |
| 10 | pass_type_syntax.c | 1,024 | slice/result type text rewrites plus retired-optional diagnostics |

**Attribution note (2026-06-01 audit):** these are *not* called from `visit_codegen.c`. `cc__rewrite_channel_pair_calls_text` runs in `preprocess.c`; `cc__rewrite_chan_send_task_text` runs in `visit_codegen.c`; `cc__rewrite_result_types_text` runs from `pass_closure_literal_ast.c`. The bulk of slice/result/chan-handle type lowering is the static preprocess passes (P3/P4/P8).

### Phase 5: Closure Literals (REPARSE #3)

| # | Pass File | Lines | Transform |
|---|-----------|-------|-----------|
| 11 | pass_closure_literal_ast.c | 3,643 | `() => {...}` → `<base>_make()` (location-tagged: `cc_closure__N<id>__line<L>_col<C>` from `cc_diag_mangle_symbol`, 2026-05-28). |

### Phase 6: Structured Concurrency (batched, REPARSE #4)

| # | Pass File | Lines | Transform |
|---|-----------|-------|-----------|
| — | ~~pass_nursery_spawn_ast.c~~ | — | **Deleted 2026-05-28.** Was a 1,326-LOC orphan: all four exported entry points (`cc__rewrite_spawn_stmts_with_nodes`, `cc__rewrite_nursery_blocks_with_nodes`, `cc__collect_spawn_edits`, `cc__collect_nursery_edits`) had zero callers anywhere in the tree. Spawn/nursery lowering is handled by `preprocess.c` + `pass_closure_literal_ast.c`. |

### Phase 7: Defer (text)

| # | Pass File | Lines | Transform |
|---|-----------|-------|-----------|
| 14 | pass_defer_syntax.c | 1,341 | `@defer stmt;` → inject before } and return |

### Phase 8: Async State Machine (REPARSE #5)

| # | Pass File | Lines | Transform |
|---|-----------|-------|-----------|
| 15 | async_ast.c | 3,713 | @async fn → state machine |

### Other

| # | Pass File | Lines | Transform |
|---|-----------|-------|-----------|
| 16 | pass_strip_markers.c | 71 | Strip @async/@noblock/@blocking/@latency_sensitive (`cc__strip_cc_decl_markers`, called from visit_codegen.c) |
| 17 | checker.c | 1,286 | Semantic checks (slice move, provenance) |

### Passes added by 2026-06-01 audit (existed in code, previously omitted)

| # | Pass File | Lines | Transform |
|---|-----------|-------|-----------|
| 18 | pass_result_unwrap.c | 2,869 | `!>` / `?>` result-unwrap operators → inline expansion (`cc__rewrite_result_unwrap`; preprocess.c phase-3 + visit_codegen.c late text passes) |
| 19 | pass_unwrap_destroy.c | 732 | lift `@destroy { body }` suffix off `!>`/`?>` stmts → standalone `@defer` (`cc__rewrite_unwrap_destroy_suffix`; preprocess.c phase-3 + visit_codegen.c) |
| 20 | pass_err_syntax.c | 1,357 | `@err` / `@errhandler` / `=<!` / `<?` error-handling sugar (`cc__rewrite_err_syntax`; preprocess.c phase-3 + visit_codegen.c) |
| 21 | pass_create.c | 543 | registered-type `@create(...)` / `@destroy` lowering (`cc_rewrite_registered_type_create_destroy`; build_parse_input.c, pre-parse) |
| 22 | pass_check_type_of.c | 409 | compile-time diagnostic for unregistered `type_of(T)` / `cc_type_of("T")` (`cc__check_type_of_calls`; build_parse_input.c, pre-parse) |

## Consolidation Opportunities

### High Value (reduces reparses)

1. ✅ **Merge Phase 3 passes** — UFCS, closure_calls, autoblock, await_normalize
   - **Done (2026-05-28):** two-stage batched is the only path. Phase-3 reparses: 4 → 2.
   - Stage 1 (UFCS) cannot fold into Stage 2 — UFCS produces new call sites the other AST-driven collectors must see post-reparse.
   - **Next:** see PIPELINE.md "Target" — fold Stage-1 reparse into the closure-literal reparse (M6_DEFERRED).

2. **Merge closure_literals + spawn/nursery** — share one reparse
   - BLOCKED: closure_literals uses coarse-grained whole-file edit
   - Would require refactoring to fine-grained edits
   - Could save: 1 reparse (3→2)

3. **Refactor Phase 6** — spawn/nursery lowering is now the only remaining pass there
   - Arena block lowering was retired with the surface syntax
   - Savings are now from simplification rather than fewer reparses

### Medium Value (reduces complexity)

4. **Consolidate preprocessor type transforms** (P8+P9+P13)
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
4. ✅ Phase 3 EditBuffer infrastructure (2026-02-01); batched apply optional via `CC_BATCH_PHASE3` (2026-05-26); **two-stage batched is now the only path** (2026-05-28) — all four collectors emit per-span edits; UFCS in stage 1, closure_calls+autoblock+await_normalize in stage 2; reparses 4 → 2.
5. ✅ Dynamic result-spec registry - `cc__cg_result_specs` is backed by
   `CCResultSpecTable` rather than fixed-size arrays, so there is no Result
   type count limit per compilation unit. (2026-03-09; updated after
   optional-type retirement)
6. ✅ Explicit registry reset - `cc__cg_reset_type_registries()` called once per
   compilation unit in visit_codegen.c. Scan functions now ACCUMULATE rather than
   implicitly resetting on each call. Previously the second call to any scan function
   within one compilation unit (e.g. from pass_closure_literal_ast.c) would silently
   discard types collected by the first call. (2026-03-09)
7. ✅ `spawn into` correctness - fixed `__spawn_into_thunk` to detect when the called
   function stores its result directly via cc_task_result_ptr (evidenced by returning
   the same buffer pointer). The thunk no longer overwrites the caller's structured
   result. The `spawn into(ch)` form now uses discard-on-backpressure semantics
   (cc_task_free) instead of incorrect result propagation. (2026-03-09)

## AST Migration Investigation (2026-03-09)

### Why text-based rewrites are necessary for type syntax

The type passes rewrite `T[:]` and `T!>(E)` at the token level; the retired
`T?` surface is handled by a diagnostic-only text guardrail. TCC's stub-AST
does NOT emit AST nodes for these annotations — they exist only as surface
syntax tokens before TCC sees them. As a result:

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
| Closure literal lowering (P11) | Partially AST-based; whole-file edit blocks batching | MEDIUM |

### Reparse reduction: the real migration target

**Update 2026-05-28:** the historical "make Phase 5 per-span to fold into Phase 3" plan
was reanalyzed and found to be a non-win. Closure-literal lift is a *producer* for
closure_calls (closure literals inside closure-typed call arg lists must be lowered
to `__cc_closure_make_N()` before `cc__emit_call_replacement` extracts the arg text).
Folding it into Phase 3 Stage 2 would require a 3rd Phase-3 stage with its own reparse
barrier — net zero reparse savings. See [`ARCHITECTURE.md` §6 "Targets that aren't
worth it"](../../docs/ARCHITECTURE.md).

**What actually shipped (M4.a, 2026-05-28):** the Phase-5 reparse + closure-pass call
were gated on `cc_contains_token_top_level(src_ufcs, ..., "=>")` in
`visit_codegen.c`. Phase-5 reparse went from **461 → 155** across the smoke suite
(−306, −66%). 70% of real TUs have no closure literals; they now skip Phase 5
entirely. The whole-file rewrite inside `cc__rewrite_closure_literals_with_nodes`
is preserved as-is because per-span migration alone wouldn't change any reparse
count.

**Historical context (pre-M4.a):** the 5-reparse total above was a worst case;
the actual smoke-suite total post-M4.a is 1170 reparses across 461 TUs, averaging
~2.5 reparses/TU (including the initial parse and unconditional final-UFCS sweep).

## Known issues (closure literals in deeply-rewritten call sites)

Two pre-existing failures share a root cause area in `pass_closure_literal_ast.c`:

- `examples/recipe_tcp_echo.ccs` — `n->spawn(() => [sock] { handle_client(sock); })`
  inside `if (test_mode) { ... }` inside a nursery `{ ... } @destroy` block.
  **Layer 1 fixed (May 2026):** the block-scope `static __cc_closure_make_N(void);`
  decl that triggered `function without file scope cannot be static` was
  caused by the legacy in-source walker (formerly named
  `cc__closure_proto_insert_off`) landing the forward decl inside the
  enclosing `if` block.  `visit_codegen.c` was switched to the
  file-scope-only path of `cc__rewrite_closure_literals_with_nodes`,
  placing decls via `cc_find_first_func_def_offset` (just before the
  first top-level function definition, so user typedefs that appear
  between `#include`s and the first function — e.g. `HolReqTx` in
  `tests/redis_owner_reply_try_send_hol_smoke.ccs` — are still in
  scope).  The walker and its `skip_inline_protos=0` opt-out were
  deleted entirely 2026-05-28.
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

The legacy `cc__closure_proto_insert_off` walker and its
`skip_inline_protos=0` opt-out were **deleted 2026-05-28** (fossil
audit confirmed both in-tree callers passed `=1`).  The non-`_ex`
wrapper was deleted at the same time; the surviving entry point
is `cc__rewrite_closure_literals_with_nodes` (file-scope-only).

## Next Steps

1. **Reparse reduction (5→4)**: Make closure_literals use CCEditBuffer (fine-grained edits)
   - Current: whole-file edit can't batch with spawn/nursery/arena
   - Effort: Medium (2-3 days)
   - Saves: 1 TCC reparse per compilation unit

2. **Optional/Result pass merging** (P8+P9): DEFERRED
   - Analysis: marginal scan reduction, minimal real impact
   - Decision: Not worth the refactoring effort

3. **Dead code removed**: visitor_pipeline.c was deleted (2026-02-01)
   - The Phase 3 consolidation from there was already applied to visit_codegen.c

## Summary

The major consolidation wins have been achieved:
- ✅ CCPassChain helper (cleaner pass chaining)
- ✅ Phase 3 EditBuffer two-stage batching (4 passes → 2 applies, 2 reparses) — default since 2026-05-28
- ✅ CCScannerState refactoring (5 passes converted)
- ✅ Documentation updated

### CCScannerState Refactoring Status

All suitable passes have been converted to use the shared `CCScannerState` helper:

**Converted (12 passes):**
- `cc__rewrite_optional_types` (P6, diagnostic-only guardrail)
- `cc__rewrite_result_types` (P8)
- `cc__rewrite_slice_types` (P3)
- `cc__rewrite_result_star_unwrap` (P10)
- `cc__reject_match_syntax` (P2)
- `cc__rewrite_chan_handle_types` (P4) ✅ 2026-02-01
- `cc_rewrite_generic_containers` (P5) ✅ 2026-02-01
- Legacy preprocess UFCS passes removed; UFCS now lowers through the parser/TCC tolerance plus AST-aware UFCS path.

Remaining opportunities are either blocked (reparse reduction) or low-value (pass merging).
