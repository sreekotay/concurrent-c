# Macro-generated CC syntax tests

> **Mostly legacy-front archaeology.** Default `ccc` is native
> (`shadow_lower`). Notes below about pre-expand / visitor reparse apply to
> `--frontend=legacy`. Prefer native for new macro surface unless diagnosing
> the opt-out path.

**Status:** M7.A + M7.B + M7.C (partial) + M7.C3 (M1-lite plumbing)
shipped, and pre-expand is now the only initial-parse path on the legacy
front (2026-05-26 flip; `CC_PRE_EXPAND` later collapsed to inert). The earlier
`CC_PRE_EXPAND_REPARSE=1` opt-in was removed: CPP-expanding the
final reparse buffer broke AST coord alignment with the visitor's
working buffer in 4 unrelated tests (`async_chan_await_works_smoke`,
`async_channel_typed_lowered_smoke`, `call_site_noblock_smoke`,
`ufcs_nested_std_io_smoke`); the proper fix for that was the M1
visitor swap. The M7.C3 plumbing means the channel-pair scanner
CAN now resolve macro-generated chan handle decls (e.g.
`CHAN(int) tx;` → `int[~4 >] tx;`) by falling back to
`CCASTRoot.parse_buffer_pre_relower`; the remaining blocker for end-
to-end macro CHAN compile on legacy is the reparse path itself, which still
re-runs phase-1+phase-3 on the raw user source and trips on
unexpanded macros. See
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
tests in this folder still fail until full M1 lands):

```
cc/bin/ccc <your-file>.ccs              # pre-expand is now default
CC_PRE_EXPAND=0 cc/bin/ccc <your-file>.ccs  # legacy path
make smoke                              # 447/447 pass under default
CC_PRE_EXPAND=0 make smoke              # 447/447 pass under legacy
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
2. ~~Mirror of (1) inside `cc__reparse_source_to_ast`~~ — **removed
   2026-05-26**.  Was gated behind `CC_PRE_EXPAND_REPARSE=1`.  The
   root cause of the 4 regressions (`async_chan_await_works_smoke`,
   `async_channel_typed_lowered_smoke`, `call_site_noblock_smoke`,
   `ufcs_nested_std_io_smoke`) is structural: the CPP-expanded
   reparse buffer carries line/offset coordinates that do NOT line
   up with `src_ufcs` (which the outer visitor does NOT re-expand).
   AST walkers like `async_ast` then walk past their intended target
   nodes.  Fixing this properly is the M1 visitor swap: feed the
   visitor the pre-expanded buffer end-to-end so AST and source
   agree on coordinates.

## What landed in M7.C3 (M1-lite visitor plumbing + heap-safety fix)

1. `CCASTRoot` gained two owned text-buffer fields:
   - `parse_buffer` — exactly what TCC parsed (post-CPP-expand +
     post-relower).
   - `parse_buffer_pre_relower` — post-CPP but still has `[~ ... >]`
     chan brackets intact (useful for scanners that need bracket
     specs from macro-generated decls).
   Both are populated by `cc_build_parse_input` when
   `CC_PRE_EXPAND=1`, transferred to the root in `parse.c`, and freed
   by `cc_tcc_bridge_free_ast`.
2. `CCVisitorCtx` gained `pre_expanded_buf`/`pre_expanded_len`,
   populated from the root in `walk.c` (prefers `parse_buffer_pre_relower`
   so visitor span scanners get a view that still has chan brackets).
3. The channel-pair scanner's `cc__find_chan_decl_before` is now
   parameterized by `alt_buf`/`alt_len`. When the raw user source
   doesn't contain a `[~ ... >] name;` decl (e.g. the user wrote
   `CHAN(int) tx;`), the scanner falls back to the pre-expand buffer
   and the caller uses the matching buffer for
   `cc__parse_chan_bracket_spec`. This makes macro-generated chan
   handle decls resolvable end-to-end on the visitor side.
4. **Heap-safety fix in `cc_cpp_expand`.** On macOS, `open_memstream(3)`
   returns a buffer whose reserved capacity extends past its logical
   end. A subsequent `malloc()` can land inside that capacity, and
   when the caller writes to its own malloc'd chunk, it silently
   scribbles over `pp`'s trailing NUL. `cc__rewrite_chan_handle_types`
   then walks past `pp`'s logical end into the caller's chunk and
   doubles the buffer. `cc_cpp_expand` now re-packs its output into a
   fresh tight allocation before returning, retiring that footgun for
   all callers (was triggered by the M7.C3 pre-relower copy).

## Why the macro CHAN tests in this folder still fail

The M7.C3 plumbing fixes the visitor side (channel-pair scanner can
now resolve macro-generated `tx`/`rx`). The remaining blocker is the
reparse path: `cc__reparse_source_to_ast` re-runs phase-1+phase-3 on
the raw user source, so macros like `CHAN(int) tx;` are still opaque
to chan-handle lowering on the reparse side and TCC chokes on the
literal `CHAN(int)`. The proper fix is the M1 visitor swap (the
visitor consumes the pre-expanded buffer end-to-end). Tracked under
"Recommended next work" in
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

- ~~`CC_BATCH_PHASE3=1`~~ — removed 2026-05-28; two-stage batched Phase 3 (UFCS, then closure_calls + autoblock + await_normalize) is now the only path. See [cc/src/visitor/PIPELINE.md](../../cc/src/visitor/PIPELINE.md).
- `CC_PRE_EXPAND` — TCC-CPP pre-expand of the initial parse +
  post-expand re-lower.  **Default-on (2026-05-26).**  Disable with
  `CC_PRE_EXPAND=0` or `CC_PRE_EXPAND=` (empty).
- ~~`CC_PRE_EXPAND_REPARSE=1`~~ — removed 2026-05-26 (broke AST coord
  alignment for the reparse side; real fix is the M1 visitor swap).
- `CC_DEBUG_PRE_EXPAND=1` — verbose pre-expand diagnostics
