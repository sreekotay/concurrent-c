# Macro-generated CC syntax tests

**Status:** M7.A + M7.B + M7.C (partial) shipped (pre-expand opt-in,
`#define`-aware phase-1 scanner, registry-preserving post-pre-expand
re-lower, and opt-in reparse pre-expand). 429/429 smoke with
`CC_PRE_EXPAND=1`; the additional `CC_PRE_EXPAND_REPARSE=1` flag
exercises the reparse-side CPP path but is opt-in because it changes
AST shapes in 4 unrelated smoke tests. Full macro end-to-end compile
is blocked on the M1 visitor refactor (visitor consuming
`cc_build_parse_input`'s buffer instead of re-reading the original
file) so that span-based passes like the channel-pair scanner see the
expanded form. See
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

## What landed in M7.B

`CCScannerState` now tracks `in_pp` and treats any `#`-led line as
non-code (with backslash-newline continuation handling). The 13 phase-1
passes that share `cc_scanner_skip_non_code` no longer rewrite tokens
inside `#define`/`#include`/`#if` bodies. Verified via
`CC_DEBUG_PRE_EXPAND_DUMP`: the `#define CHAN(T) T[~4 >]` line survives
phase-1 verbatim, and CPP correctly expands `CHAN(int)` to `int[~4 >]`
during pre-expand.

## What landed in M7.C (partial)

1. `cc_relower_cc_type_syntax_preserving_registry` in
   `cc/src/preprocess/preprocess.{c,h}` — runs the same four header-safe
   lowerings as `cc_rewrite_header_type_syntax_shared`
   (`cc__rewrite_string_templates`, `cc__rewrite_chan_handle_types`,
   `cc__rewrite_slice_types`, `cc_rewrite_generic_containers`) but
   deliberately does NOT call `cc_type_registry_clear`, so Result/Vec/Map
   registrations from the main preprocess survive. Called by
   `cc_build_parse_input` immediately after `cc_cpp_expand`: macro-
   generated CC type syntax like `int[~4 >]` (from `#define CHAN(T)
   T[~4 >]`) is now lowered to `CCChanTx_int` and TCC's initial parse
   succeeds.
2. Mirror of (1) inside `cc__reparse_source_to_ast` in
   `visit_codegen.c`, but gated behind a separate env var
   (`CC_PRE_EXPAND_REPARSE=1`) because CPP-expanding the full reparse
   buffer (post-prelude) regresses 4 unrelated smoke tests
   (`async_chan_await_works_smoke`, `async_channel_typed_lowered_smoke`,
   `call_site_noblock_smoke`, `ufcs_nested_std_io_smoke`) by changing
   AST shapes the async-AST and a few UFCS passes depend on. Useful
   for validating the end-to-end CPP-through-reparse pipeline
   without disturbing the default.

## Why the macro CHAN tests in this folder still fail

Even with both flags on, `cc_channel_pair(&tx, &rx)` errors with
`could not find declarations for 'tx' and 'rx'`. That pass scans the
visitor's `src_ufcs` buffer — which is still the raw un-expanded user
source read from disk in `visit_codegen.c`. The expanded form lives
only in `cc_build_parse_input`'s output (used for the initial parse)
and in the reparse path (when `CC_PRE_EXPAND_REPARSE=1`). Threading
the pre-expanded buffer through the visitor's text-pass pipeline is
the M1 visitor refactor; tracked under M1 in
[`../../cc/docs/COMPILER_CLEANUP_STATUS.md`](../../cc/docs/COMPILER_CLEANUP_STATUS.md).

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
- `CC_PRE_EXPAND=1` — experimental TCC-CPP pre-expand of the initial
  parse + post-expand re-lower (off by default; 429/429 smoke)
- `CC_PRE_EXPAND_REPARSE=1` — also pre-expand the reparse buffer
  (off by default; opt-in, regresses 4 smoke tests today)
- `CC_DEBUG_PRE_EXPAND=1` — verbose pre-expand diagnostics
