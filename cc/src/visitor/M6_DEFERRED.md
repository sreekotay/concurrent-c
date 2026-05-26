# M6 — Type-syntax AST tolerance (deferred)

**Status:** Superseded by **M7**. M7.A landed (opt-in pre-expand, zero
regressions on 429/429 smoke + parity with baseline on examples/stress).
M7.B (text-pass `#define`-awareness) is the remaining piece needed for
the macro CC-syntax case. See
[COMPILER_CLEANUP_STATUS.md](../../docs/COMPILER_CLEANUP_STATUS.md) for
the current M7 plan.

## 2026-05-26 update — pre-expand spike supersedes M5.5/M6 for macros

A small spike (`cc/src/preprocess/cpp_expand.{c,h}`, opt-in via
`CC_PRE_EXPAND=1`) confirmed the **"invert the order"** pattern works for
the leverage case (macro-generated CC syntax):

```
src → TCC -E (cpp only)  →  text passes  →  TCC (lex+parse)
```

Tested with `#define CHAN(T) T[~4 >]` followed by `CHAN(int) tx;`. TCC's
preprocessor expands the macro to `int[~4 >]` while preserving channel
tokens (the CC lexer prints them back verbatim during `-E`), and `#line`
markers map diagnostics back to user source. See the standalone probe:

```
cc cc/src/tools/cpp_expand_probe.c \
   out/cc/obj/src/preprocess/cpp_expand.o \
   -L third_party/tcc -ltcc -o /tmp/cpp_probe
/tmp/cpp_probe tests/macro/macro_chan_capacity_macro_smoke.ccs
```

**Implication:** the planned M5.5 (TCC fork: post-CPP token recognizer)
and M6 (TCC fork: stub-AST nodes for CC type syntax) can both be
replaced by:

- a single `cc_cpp_expand()` call early in `cc_build_parse_input`
- removing the redundant text passes for forms now handled by CPP

**What landed in M7.A:**

- `cc_cpp_expand()` runs TCC's CPP **after** `cc_preprocess_for_initial_parse`,
  so the prepended container/result-type `#include` lines resolve before
  TCC's second-pass parser sees them.
- GCC-style `# N "file" flags` line markers are normalized to bare C99
  `#line` form to prevent TCC from re-triggering system-header include
  logic (which was the `__mbstate_t` redefinition seen in the spike).
- Opt-in via `CC_PRE_EXPAND=1`. 429/429 smoke pass; examples/stress
  baseline unchanged (same 2 pre-existing closure-capture failures).

**Why CHAN-macro tests still don't compile under `CC_PRE_EXPAND=1`:**

Phase-1 text passes (esp. P4 `cc__rewrite_chan_handle_types`) scan
text token-by-token without skipping `#define` bodies. For
`#define CHAN(T) T[~4 >]`, P4 matches `T[~4 >]` **inside** the macro
body and rewrites it to `CCChanTx`, leaving a dangling `CCChanTx`
identifier that breaks the parse before CPP ever runs. M7.B is the
follow-up: teach `CCScannerState` to skip strings/comments/`#define`
bodies; then pre-expand sees an intact `#define` and the rest works.

**M7.C — eventual cleanups (after M7.B):**

- Flip `CC_PRE_EXPAND=1` to default.
- Retire `pass_channel_syntax` text scan and `cc__rewrite_chan_handle_types`
  in favor of the lexer-recognized form.
- Retire the local/system `_cch → _h` include rewrites (CPP handles them).
- Propagate the expanded buffer through reparses (`cc__reparse_source_to_ast`)
  instead of re-reading the original source.
- Wire `CCSourceMap` into visitor passes that carry "original-source-span"
  assumptions so diagnostics still point to user source after pre-expand.

## Goal (original, M6)

Promote **one** CC type construct from text rewrite / M5.5 token synthesis to a **real TCC stub-AST node** carrying `CCSourceSpan`, then retire the matching preprocess text pass.

**Recommended pilot:** channel out-handle `T[~N >]` (P4 `cc__rewrite_chan_handle_types`).

## Steps (when funded)

1. Extend TCC declarator grammar to record a stub node for `T[~N >]` / `T[~N <]`.
2. Attach `CCSourceSpan` on the node (invariant I5).
3. Lower from AST in visit_codegen (or TCC emission) instead of text scan.
4. Remove P4 text pass for that construct only.
5. Prove on:
   - `tests/channel_pair_syntax_smoke.ccs`
   - `tests/owned_channel_syntax_smoke.ccs`
   - `real_projects/pigz/pigz_idiomatic.ccs`
6. Only then consider `T[:]`, `T!>(E)`, etc.

## Prerequisites

- M5.5 macro-aware recognizer **validated** (token synthesis + macro tests green)
- M0.5 source map stable across reparses in `visit_codegen.c`
- M5 `tcc_ext_api.h` frozen for the node-kind enum extension

## Why deferred

Multi-week TCC fork work; risks reopening merge churn right after M5. M0–M5.5 infrastructure ships without M6.
