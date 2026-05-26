# Macro-generated CC syntax tests

**Status:** M7.A shipped (pre-expand opt-in, zero regressions, 429/429
smoke). M7.B (`#define`-aware text passes — required to actually compile
macros whose body contains CC syntax) not started. See
[`../../cc/docs/COMPILER_CLEANUP_STATUS.md`](../../cc/docs/COMPILER_CLEANUP_STATUS.md).

- M5.5 hooks (TCC parse-time recognizer) — stubs only; superseded by M7.
- Pre-expand path (`CC_PRE_EXPAND=1`) — resolves prepended `#include`
  lines through TCC's CPP after CC text passes run. Validated against
  the full smoke suite; same examples/stress baseline as default.

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

End-to-end build (works for non-macro CC files today — the macro
tests in this folder still fail until M7.B lands):

```
CC_PRE_EXPAND=1 cc/bin/ccc <your-file>.ccs
CC_PRE_EXPAND=1 make smoke   # 429/429 pass
```

## M7.B (TODO) — why the macro tests in this folder don't compile yet

Phase-1 text passes (esp. P4 `cc__rewrite_chan_handle_types`) scan the
raw source token-by-token without skipping `#define` directive bodies.
For `#define CHAN(T) T[~4 >]`, P4 sees the `T[~4 >]` pattern inside the
`#define` body and lowers it to `CCChanTx` mid-line, leaving a dangling
identifier that breaks the parse before CPP ever runs.

Fix path:

1. Teach `CCScannerState` (and per-pass scanners that don't use it) to
   detect `#define <name>(...)` and skip until the logical line end
   (handle backslash-newline continuations).
2. Verify on the two tests in this folder.
3. Then `CC_PRE_EXPAND=1` will actually compile macro-generated CC syntax
   end-to-end.

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
