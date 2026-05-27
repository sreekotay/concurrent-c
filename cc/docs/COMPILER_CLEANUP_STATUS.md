# Compiler cleanup status (M0–M5.5)

**Last updated:** 2026-05-26  
**Smoke suite:** 436 tests passing (`make smoke`, default + `CC_PRE_EXPAND=1`)

This is the single source of truth for the compiler cleanup workstream (M0–M5.5). See also [PIPELINE.md](../src/visitor/PIPELINE.md), [PASS_INVENTORY.md](../src/visitor/PASS_INVENTORY.md), [DIAG_AUDIT.md](../src/diag/DIAG_AUDIT.md), [M6_DEFERRED.md](../src/visitor/M6_DEFERRED.md).

---

## Shipped (complete)

| Milestone | What landed |
|-----------|-------------|
| **M0** | `PIPELINE.md`, `DIAG_AUDIT.md`, `PASS_INVENTORY` audit, `perf/baseline_M0.txt`, `scripts/capture_baseline.sh`, orphan/unwired notes |
| **M0.5** | `cc/src/diag/` — `cc_diag_emit`, `CCSourceMap`, `cc_diag_mangle`, `CCEditBuffer` span fields, `DEBUG_VARS.md`, `--show-lowered=<phase>`, driver init |
| **M3** | `cc_preprocess_for_initial_parse`, `cc_preprocess_for_reparse`, `cc_preprocess_for_light_reparse` (phase-1 skip when safe) |
| **M5** | `third_party/tcc-patches/tcc_ext_api.h`, `cc/src/parser/tcc_ext_api.c` — versioned API, diag bridge stubs, comment-aware search wrappers, recognizer hook registration |

---

## Shipped (partial — code exists, not fully integrated)

| Milestone | Done | Remaining |
|-----------|------|-----------|
| **M1** | `cc_build_parse_input()` in `parse.c` | `visit_codegen.c` still duplicates prep; source map not threaded through codegen |
| **M2** | `cc__apply_batched_phase3_passes()` | **Default is sequential** (429 tests). Opt-in: `CC_BATCH_PHASE3=1` (experimental; had regressions when default-on) |
| **M4** | `mangle.h` included in closure pass | Whole-file closure lift unchanged; `cc_diag_mangle_symbol` not used for emitted names |
| **M5.5** | `cc_macro_recognizer.c` hooks registered at parse | **Token synthesis into TCC lexer not implemented** — `#define CHAN(T) T[~4 >]` still fails; see [tests/macro/README.md](../../tests/macro/README.md) |
| **Runtime R0** | `cc/runtime/cc_rt_diag.c` stubs in runtime | R1–R5 (async backtrace naming, channel deadlock text, `!>` source location, etc.) not implemented |

---

## Deferred

| Milestone | Notes |
|-----------|--------|
| **M6** | Pilot stub-AST for `T[~N >]`; retire P4 text pass. See [M6_DEFERRED.md](../src/visitor/M6_DEFERRED.md). **Likely superseded by M7 pre-expand.** |

## M7 — Pre-expand integration (in progress)

| Phase | Status | What landed |
|-------|--------|-------------|
| **M7.A** (opt-in, no regressions) | **Shipped** | `cc_cpp_expand()` runs TCC's CPP after `cc_preprocess_for_initial_parse` so the prepended container/result-type `#include` lines resolve. GCC-style `# N "file" flags` markers normalized to bare C99 `#line` to prevent TCC's parser from re-triggering system-header inclusion. Opt-in via `CC_PRE_EXPAND=1`. **429/429 smoke pass; examples/stress baselines unchanged (same 2 pre-existing failures: `recipe_tcp_echo.ccs`, `syscall_kidnap.ccs`).** |
| **M7.B** (`#define`-aware scanner) | **Shipped** | `CCScannerState` now tracks `in_pp` and treats any `#`-led line (with backslash-newline continuations) as non-code, so all 13 phase-1 passes that use `cc_scanner_skip_non_code` (`cc__rewrite_chan_handle_types`, `cc_rewrite_slice_types`, `cc_rewrite_generic_containers`, etc.) no longer rewrite tokens inside `#define`/`#include`/`#if` bodies. The visitor-side `cc__rewrite_chan_handle_types_text` in `pass_channel_syntax.c` (which has its own ad-hoc scanner) was also taught the same `in_pp`/`pp_continued`/`at_line_start` plumbing — covered by `tests/m7b_define_chan_body_unused_smoke.ccs` which previously failed with "too many basic types" when an unused `#define LOOKS_LIKE_CHAN(T) T[~4 >]` was present. **436/436 smoke pass; CC_PRE_EXPAND=1 still parity with baseline.** The CHAN macro definition now survives intact through phase-1 (verified via debug dump); CPP correctly expands `CHAN(int)` to `int[~4 >]`. |
| **M7.C** (post-expand re-lower + reparse plumbing) | **Partially shipped** | **(a) Registry-preserving re-lower** (`cc_relower_cc_type_syntax_preserving_registry`) added in `preprocess.{c,h}` — wraps the same four header-safe lowerings as `cc_rewrite_header_type_syntax_shared` but deliberately does NOT call `cc_type_registry_clear`, so it is safe to run after the main preprocess has populated the registry. Wired into `cc_build_parse_input` right after `cc_cpp_expand`: the initial parse now compiles macro-generated CC type syntax (e.g. `int[~4 >]` from `#define CHAN(T) T[~4 >]`) into `CCChanTx_T` without disturbing existing Result/Vec/Map registrations. **(b) Reparse pre-expand** wired into `cc__reparse_source_to_ast` just before `cc_tcc_bridge_parse_string_to_ast`, gated behind a separate `CC_PRE_EXPAND_REPARSE=1` env (see below). 429/429 smoke pass with `CC_PRE_EXPAND=1` (M7.A behavior preserved). Full macro CHAN end-to-end still needs the reparse path to also run pre-expand without regressions; tracked under M7.C2 caveat below. |
| **M7.C2** caveat | Opt-in only | `CC_PRE_EXPAND_REPARSE=1` runs CPP over the FINAL reparse buffer (after `cc_preprocess_for_reparse` + `cc__prepend_reparse_prelude` + parser-helper rewrites — earlier placement causes `__mbstate_t` double-decl). Validated end-to-end pipeline but regresses 4 smoke tests (`async_chan_await_works_smoke`, `async_channel_typed_lowered_smoke`, `call_site_noblock_smoke`, `ufcs_nested_std_io_smoke`) because CPP-expanded reparse output changes AST shapes in ways that confuse the async-AST and a few UFCS passes. Kept opt-in so it can be unblocked one pass at a time without disturbing the default. |
| **M7.C3** (M1-lite visitor plumbing + heap bug fix) | **Shipped** | **(a) AST root carries the pre-expand text.** `CCASTRoot` gained two owned fields: `parse_buffer` (post-CPP-expand + post-relower, the exact text TCC parsed) and `parse_buffer_pre_relower` (post-CPP but still with `[~ ... >]` chan brackets intact). `cc_build_parse_input` now copies the pre-relower buffer before running the in-place re-lower; `parse.c` transfers ownership of both to the AST root, and `cc_tcc_bridge_free_ast` frees them. **(b) Visitor ctx exposes the buffer.** `CCVisitorCtx` gained `pre_expanded_buf`/`pre_expanded_len` (NULL when pre-expand off). `walk.c` populates them from the root, preferring the pre-relower copy so bracket-based scanners get a view that still has `[~ ... >]` even when the AST sees `CCChanTx_int`. **(c) Channel-pair scanner fallback.** `cc__find_chan_decl_before` is now parameterized by an `alt_buf`/`alt_len` fallback. When the raw user source doesn't contain a `[~ ... >] name;` decl (e.g. the user wrote `CHAN(int) tx;`), the scanner searches the pre-expand buffer too; on hit, the caller uses the matching buffer for `cc__parse_chan_bracket_spec`. **(d) `cc_cpp_expand` heap-safety fix.** On macOS, `open_memstream(3)` returns a buffer whose reserved capacity extends past its logical end — a later `malloc()` can land inside that capacity and silently scribble over the trailing NUL when the caller writes its new allocation. `cc__rewrite_chan_handle_types` then scanned past the original end into the caller's chunk and produced a buffer ~2× the expected size. `cc_cpp_expand` now re-packs its output into a fresh tight allocation before returning, which permanently retires that footgun for all callers. **436/436 smoke pass, both default and `CC_PRE_EXPAND=1`.** Full end-to-end macro CHAN compile still blocked on `CC_PRE_EXPAND_REPARSE` regressions (M7.C2). |

---

## Reparse count (current)

- **~9** `cc__reparse_source_to_ast` sites in `visit_codegen.c` + 1 initial parse
- **Target** (after M2 default batch + M4 fine-grained): 3–4

---

## Recommended next work

> **Central blocker risk callout:** M1 (the visitor refactor) is the
> load-bearing piece for four otherwise-stalled items: macro CC-syntax
> end-to-end, flipping `CC_PRE_EXPAND=1` to default, retiring redundant
> `_cch → _h` rewrites, and fixing the `m0_5_diag_origin_line_fail`
> source-map drift.  The earlier framing — "just unblock the four
> `CC_PRE_EXPAND_REPARSE` regressions and flip the flag" —
> understated the problem: those four passes fail because
> `visit_codegen.c` reads `src_all` from disk (small, user-source-
> shaped buffer) while the pre-expand reparse's AST stores
> `fn->lbrace/rbrace` as offsets into a much larger inlined-headers
> buffer.  No per-pass plumbing fixes that coordinate mismatch; we
> have to make the visitor's working buffer agree with the AST's
> parse buffer.  This is the actual M1 refactor.  Doing it
> incrementally is safe (the M7.C3 plumbing is already in place to
> support it), but it is bigger than one commit.

1. **Closure-literal refactor — DONE (proto-placement layer).**
   `visit_codegen.c` now calls `cc__rewrite_closure_literals_with_nodes_ex`
   with `skip_inline_protos=1`, bypassing the brittle in-source walker
   (`cc__closure_proto_insert_off`).  File-scope forward decls are placed
   via the new `cc_find_first_func_def_offset` helper (just before the
   first top-level function definition — past `#include`s AND user
   typedefs).  Fixes the block-scope `static` failure in
   `examples/recipe_tcp_echo.ccs`.  Smoke clean in both modes.
   **Remaining layers** (separate bugs, NOT addressed by this refactor):
   - `recipe_tcp_echo.ccs` layer 2: captured `sock` is not unpacked from
     `__env` — lives in capture-emission code in
     `pass_closure_literal_ast.c`.
   - `syscall_kidnap.ccs`: capture-variant closure inside a `for` loop
     is not detected at all — lives in capture-variant detection code in
     the same file.

2. **M1 visitor refactor** — bigger than originally framed.
   A spike (May 2026) attempted the naive form — swap
   `src_all = cc__read_entire_file(ctx->input_path)` →
   `src_all = strdup(root->parse_buffer)` when `CC_PRE_EXPAND=1` —
   to align the visitor's working buffer with the AST's coordinate
   space.  Smoke went from 436/436 to ~62/436 under
   `CC_PRE_EXPAND=1`.  The dominant failure mode is **not** the
   reparse prelude (that part is solvable; see below) but that
   visitor text scanners then see the inlined CC runtime headers
   (`<ccc/cc_channel.cch>`, `<ccc/std/vec.cch>`, `<ccc/cc_result.cch>`,
   etc.) as part of `src_all`.  Patterns those scanners look for —
   `cc_channel_pair(`, `[~ ... >]`, UFCS calls, `@async`, `!>`, etc.
   — are present in the runtime headers themselves, so scanners
   match against header content and emit spurious diagnostics or
   rewrites.
   The real M1 lift is therefore three pieces:
   - **(a) Source-buffer unification.** One-line swap of `src_all`
     to `root->parse_buffer` when pre-expand is on.
   - **(b) Reparse prelude awareness.**  `cc__reparse_source_to_ast`
     skips `cc_preprocess_for_reparse` + `cc__prepend_reparse_prelude`
     when its input is pre-expanded (system headers + container
     `.cch` files already inlined → re-prepending double-decls
     `__mbstate_t` etc.).  Plumbing for this is already in place:
     `CCReparseFlags.src_is_pre_expanded` + the
     `cc__reparse_source_to_ast_ctx` wrapper.  Confirmed end-to-end
     in the spike: with the swap on and the flag set, reparse made
     it past the `__mbstate_t` wall before hitting the next class
     of failures.
   - **(c) `#line`-aware text scanners.**  Each visitor pass that
     scans `src_all` for syntactic patterns needs to filter by
     origin file (the file the nearest preceding `#line N "..."`
     points to) so it only acts on tokens that originated in the
     user TU.  Likely shape: centralize this in `CCScannerState`
     (already exists; tracks comments/strings/preprocessor) by
     adding an `in_user_file` flag updated on every `#line`
     directive.  Then migrate passes one at a time, with smoke
     gating each step.  Probably 5–10 commits.
   - The M7.C3 plumbing (`CCASTRoot.parse_buffer*`,
     `CCVisitorCtx.pre_expanded_buf`,
     `cc__find_chan_decl_before` alt_buf pattern) is already there
     to support this work and remains useful as a fallback for
     scanners that aren't yet `#line`-aware.

3. **Flip `CC_PRE_EXPAND=1` to default** (post-M1). Once item #2 is
   green and the macro CHAN tests in `tests/macro/` compile end-to-end,
   make pre-expand the default. Remove the `.env` sidecar pinning
   `tests/m0_5_diag_origin_line_fail.ccs` to non-pre-expand mode (the
   source-map drift goes away when the visitor and AST agree on
   coordinates).

4. **Retire redundant text passes** (post-flip). With CPP handling
   `#include` resolution unconditionally, several legacy passes become
   no-ops or near-no-ops: the local/system `_cch → _h` rewriters,
   parts of phase-1 chan_handle/slice/Generic lowering that re-run on
   already-lowered text, etc. Audit and remove.

Then in priority order (independent of M1):

5. **Doc sync** — this file; keep PIPELINE/PASS_INVENTORY aligned (ongoing)
6. **`tests/diag/` harness** — `EXPECT-DIAG` parsing; 3–5 smoke tests (protects I1–I8)
7. **M2 finish** — fix AST ordering so `CC_BATCH_PHASE3=1` is safe by default
8. **M4** — fine-grained closure `EditBuffer` + use `cc_diag_mangle_symbol` for entry names
9. **Runtime R1+** — consume serialized `.ccs.map` from compile
10. **M5.5 fallback** — only if M7/M1 turns out to need TCC-side help after all; otherwise drop. Current evidence says drop.

---

## Compiler debugging (quick reference)

| Env / flag | Effect |
|------------|--------|
| `CC_DEBUG_REPARSE=1` | Log each reparse stage name |
| `CC_DEBUG_LOWER=1` | Log lowering / edit-buffer apply steps |
| `CC_DEBUG_SPANS=1` | Log source-map insertions |
| `CC_DEBUG_DIAG=1` | Log every `cc_diag_emit` |
| `CC_DEBUG_REPARSE_DUMP_DIR=...` | Write intermediate buffers per reparse |
| `--show-lowered=<phase>` | Dump post-phase buffer (e.g. `phase3`) |
| `CC_BATCH_PHASE3=1` | Experimental batched Phase 3 collectors |
| `CC_PRE_EXPAND=1` | M7.A: run TCC `-E` (CPP) after text passes so all `#include` directives resolve before TCC's second-pass parse. Zero-regression opt-in |
| `CC_PRE_EXPAND_REPARSE=1` | M7.C2: run CPP over the FINAL reparse buffer (after `cc_preprocess_for_reparse` + reparse prelude + parser-helper rewrites). Opt-in; see M7.C2 caveat above for the four regressing smoke tests |
| `CC_DEBUG_PRE_EXPAND=1` | Log pre-expand attempts and TCC errors during CPP |
| `CC_DEBUG_PRE_EXPAND_DUMP=/path` | Dump the post-expand buffer to a file (M7.A debugging) |

Full list: [DEBUG_VARS.md](../src/diag/DEBUG_VARS.md).

Baseline capture: `scripts/capture_baseline.sh` → `perf/baseline_M0.txt`.
