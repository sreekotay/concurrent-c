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
| **M7.C** (post-expand re-lower + reparse plumbing) | **Partially shipped** | **(a) Registry-preserving re-lower** (`cc_relower_cc_type_syntax_preserving_registry`) added in `preprocess.{c,h}` — wraps the same four header-safe lowerings as `cc_rewrite_header_type_syntax_shared` but deliberately does NOT call `cc_type_registry_clear`, so it is safe to run after the main preprocess has populated the registry. Wired into `cc_build_parse_input` right after `cc_cpp_expand`: the initial parse now compiles macro-generated CC type syntax (e.g. `int[~4 >]` from `#define CHAN(T) T[~4 >]`) into `CCChanTx_T` without disturbing existing Result/Vec/Map registrations. **(b) Reparse pre-expand** wired into `cc__reparse_source_to_ast` just before `cc_tcc_bridge_parse_string_to_ast`, gated behind a separate `CC_PRE_EXPAND_REPARSE=1` env (see below). 429/429 smoke pass with `CC_PRE_EXPAND=1` (M7.A behavior preserved). Full macro CHAN end-to-end still needs the visitor refactor (consuming `cc_build_parse_input`'s buffer instead of re-reading from disk) so that the channel-pair scanner and other AST/text span passes see the expanded form; tracked under M1 visitor work. |
| **M7.C2** caveat | Opt-in only | `CC_PRE_EXPAND_REPARSE=1` runs CPP over the FINAL reparse buffer (after `cc_preprocess_for_reparse` + `cc__prepend_reparse_prelude` + parser-helper rewrites — earlier placement causes `__mbstate_t` double-decl). Validated end-to-end pipeline but regresses 4 smoke tests (`async_chan_await_works_smoke`, `async_channel_typed_lowered_smoke`, `call_site_noblock_smoke`, `ufcs_nested_std_io_smoke`) because CPP-expanded reparse output changes AST shapes in ways that confuse the async-AST and a few UFCS passes. Kept opt-in so it can be unblocked one pass at a time without disturbing the default. |

---

## Reparse count (current)

- **~9** `cc__reparse_source_to_ast` sites in `visit_codegen.c` + 1 initial parse
- **Target** (after M2 default batch + M4 fine-grained): 3–4

---

## Recommended next work

1. **M1 visitor refactor → finish macro CC-syntax**: M7.C parts (a)/(b)
   landed, but the visitor still re-reads the raw user source for
   span-based passes (channel-pair scanner, etc.).  Threading
   `cc_build_parse_input`'s pre-expanded buffer all the way through
   `visit_codegen.c` is what's needed to make
   `tests/macro/macro_chan_capacity_macro_smoke.ccs` compile end-to-end
   under `CC_PRE_EXPAND=1`.  Should also fix the source-map drift
   surfaced by `tests/m0_5_diag_origin_line_fail.ccs` under
   `CC_PRE_EXPAND=1` (currently the test is pinned to default via an
   `.env` sidecar; remove the override once M1 lands).
   After that, flip `CC_PRE_EXPAND=1` to default and begin retiring
   redundant `_cch → _h` rewrites (CPP handles them).

2. **Closure-literal refactor**: re-use existing `cc__collect_closure_edits`
   (EditBuffer-based; places protos at `find_protos_insertion_point`)
   in `visit_codegen.c` instead of the older
   `cc__rewrite_closure_literals_with_nodes`. Fixes the two pre-existing
   capture-variant failures (`recipe_tcp_echo.ccs`, `syscall_kidnap.ccs`)
   and removes the brittle in-buffer offset walk in
   `cc__closure_proto_insert_off`.

Then in priority order:

3. **Doc sync** — this file; keep PIPELINE/PASS_INVENTORY aligned (ongoing)
4. **`tests/diag/` harness** — `EXPECT-DIAG` parsing; 3–5 smoke tests (protects I1–I8)
5. **M1 finish** — `visit_codegen.c` → `cc_build_parse_input`; thread `CCSourceMap` on reparse
6. **M2 finish** — fix AST ordering so `CC_BATCH_PHASE3=1` is safe by default
7. **M4** — fine-grained closure `EditBuffer` + use `cc_diag_mangle_symbol` for entry names
8. **Runtime R1+** — consume serialized `.ccs.map` from compile
9. **M7.C** — flip pre-expand default; retire redundant `.cch` rewrites
10. **M5.5 fallback** — only if M7 turns out to need TCC-side help after all; otherwise drop

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
| `CC_DEBUG_PRE_EXPAND=1` | Log pre-expand attempts and TCC errors during CPP |
| `CC_DEBUG_PRE_EXPAND_DUMP=/path` | Dump the post-expand buffer to a file (M7.A debugging) |

Full list: [DEBUG_VARS.md](../src/diag/DEBUG_VARS.md).

Baseline capture: `scripts/capture_baseline.sh` → `perf/baseline_M0.txt`.
