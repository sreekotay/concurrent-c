# Compiler cleanup status (M0–M5.5)

**Last updated:** 2026-05-26  
**Smoke suite:** 429 tests passing (`make smoke`)

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
| **M7 (proposed)** | Replace M5.5+M6 with a pre-expand pass that runs TCC's `-E` once at the input boundary. Spike landed in `cc/src/preprocess/cpp_expand.{c,h}` + `CC_PRE_EXPAND=1` flag; standalone probe at `cc/src/tools/cpp_expand_probe.c` confirms `#define CHAN(T) T[~4 >]` expands cleanly with CC channel tokens preserved. Full pipeline needs reparse plumbing + `# N "file" flags` line-marker handling. See [M6_DEFERRED.md](../src/visitor/M6_DEFERRED.md). |

---

## Reparse count (current)

- **~9** `cc__reparse_source_to_ast` sites in `visit_codegen.c` + 1 initial parse
- **Target** (after M2 default batch + M4 fine-grained): 3–4

---

## Recommended next work

Two top recommendations, given the spike result:

1. **M7 (pre-expand integration)** — replaces M5.5 + M6 if it works
   end-to-end. Pipeline plumbing: skip `.cch → .h` rewrite when on, filter
   `# N "file" flags` markers, propagate expanded buffer through reparses.
   Win: zero TCC fork changes; macro-generated CC syntax works for free.

2. **Closure-literal refactor** — re-use existing `cc__collect_closure_edits`
   path (EditBuffer-based, places protos at `find_protos_insertion_point`)
   in `visit_codegen.c` instead of the older `cc__rewrite_closure_literals_with_nodes`
   path. Fixes the two pre-existing capture-variant failures
   (`examples/recipe_tcp_echo.ccs`, `stress/syscall_kidnap.ccs`) and removes
   the brittle in-buffer offset walk in `cc__closure_proto_insert_off`.

Then in priority order:

3. **Doc sync** — this file; keep PIPELINE/PASS_INVENTORY aligned (ongoing)
4. **`tests/diag/` harness** — `EXPECT-DIAG` parsing; 3–5 smoke tests (protects I1–I8)
5. **M1 finish** — `visit_codegen.c` → `cc_build_parse_input`; thread `CCSourceMap` on reparse
6. **M2 finish** — fix AST ordering so `CC_BATCH_PHASE3=1` is safe by default
7. **M4** — fine-grained closure `EditBuffer` + use `cc_diag_mangle_symbol` for entry names
8. **Runtime R1+** — consume serialized `.ccs.map` from compile
9. **M5.5 fallback** — only if M7 pre-expand is blocked; otherwise drop

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
| `CC_PRE_EXPAND=1` | Experimental: run TCC `-E` (CPP) before text passes. Spike — initial parse only |
| `CC_DEBUG_PRE_EXPAND=1` | Log pre-expand attempts and TCC errors during CPP |

Full list: [DEBUG_VARS.md](../src/diag/DEBUG_VARS.md).

Baseline capture: `scripts/capture_baseline.sh` → `perf/baseline_M0.txt`.
