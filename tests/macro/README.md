# Macro-generated CC syntax tests

**Status:** Two parallel tracks for `#define`-generated CC syntax.

- M5.5 hooks (TCC parse-time recognizer) — stubs only; not in CI.
- **Pre-expand spike (M7 candidate)** — works for the leverage case;
  see [`../../cc/src/visitor/M6_DEFERRED.md`](../../cc/src/visitor/M6_DEFERRED.md).

## Files in this folder

- `macro_chan_capacity_macro_smoke.ccs` — uses `#define CHAN(T) T[~4 >]`
  followed by `CHAN(int) tx;`. Demonstrates a real macro use case.
- `macro_chan_minimal_smoke.ccs` — same idea with no prelude. Smallest
  reproducer that fails on `cc/bin/ccc` without `CC_PRE_EXPAND`.

Neither is in `make smoke` yet; they need the pre-expand pipeline
(see "Pipeline integration" below) or M5.5 token synthesis.

## How to run the spike (today)

The standalone probe runs TCC's preprocessor on a `.ccs` file and dumps
the expanded text:

```
make -C cc -j8
cc cc/src/tools/cpp_expand_probe.c \
   out/cc/obj/src/preprocess/cpp_expand.o \
   -I third_party/tcc -DCC_TCC_EXT_AVAILABLE \
   -L third_party/tcc -ltcc -o /tmp/cpp_probe
/tmp/cpp_probe tests/macro/macro_chan_minimal_smoke.ccs | tail -15
```

Expected: `CHAN(int) tx;` expands to `int[~4 >] tx;` with `#line`
markers preserved.

For the end-to-end pipeline run (compiles successfully through the
initial parse only — reparses don't yet inherit pre-expand):

```
CC_PRE_EXPAND=1 cc/bin/ccc tests/macro/macro_chan_minimal_smoke.ccs
```

## Pipeline integration (M7 — TODO)

Full integration requires:

1. Skip the local/system `.cch → .h` rewrites under `CC_PRE_EXPAND`
   (CPP resolves them itself).
2. Filter or rewrite the GCC-style `# N "file" flags` line markers
   that TCC's preprocessor emits, so the second-pass parser doesn't
   trip on builtin redefinitions (e.g. `__mbstate_t` from Apple SDK).
3. Apply pre-expand once at the input boundary and propagate the
   expanded buffer through all reparses, instead of re-reading the
   original source for each `cc__reparse_source_to_ast` call site.
4. Wire `CCSourceMap` (M0.5) into visitor passes that currently
   carry "original-source-span" assumptions.

## M5.5 (TCC fork) — still relevant if pre-expand turns out to be infeasible

After TCC fork wires `tcc_ext_set_type_position_recognizer` to push
lowered channel/result/slice tokens, these tests become viable without
pre-expand:

- `macro_chan_out_syntax_smoke.ccs` — `#define CHAN(T) T[~4 >]`
- `macro_chan_in_syntax_smoke.ccs`, `macro_slice_syntax_smoke.ccs`,
  `macro_result_syntax_smoke.ccs`
- `macro_postfix_unwrap_smoke.ccs`, `macro_expansion_chain_smoke.ccs`

## Related

- `CC_BATCH_PHASE3=1` — experimental Phase 3 batching (off by default)
- `CC_PRE_EXPAND=1` — experimental TCC-CPP pre-expand (off by default)
- `CC_DEBUG_PRE_EXPAND=1` — verbose pre-expand diagnostics
