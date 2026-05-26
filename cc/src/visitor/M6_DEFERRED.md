# M6 — Type-syntax AST tolerance (deferred)

**Status:** Not started. See [COMPILER_CLEANUP_STATUS.md](../../docs/COMPILER_CLEANUP_STATUS.md).

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

**Why not flipped on by default yet:**

1. Reparses (`cc__reparse_source_to_ast`) operate on edited buffers
   built from the original source; they need either (a) pre-expand
   applied once and propagated through edits, or (b) restructure of
   `src_ufcs` plumbing in `visit_codegen.c` (~lines 3540-3700).
2. Many visitor passes carry "original-source-span" assumptions; after
   pre-expand all spans must round-trip through `CCSourceMap`. M0.5
   landed the map; passes need to be wired to consult it.
3. TCC's `# N "file" flags` GCC-style line markers in the expanded
   output trip TCC's own parser when re-fed (see "incompatible
   redefinition of `__mbstate_t`" on Apple SDK headers): pre-expand
   inlines system headers and the second-pass TCC re-encounters
   builtin-style decls.

**Suggested follow-up (M7):** plan a pre-expand integration milestone
that replaces M5.5/M6:

- Filter or rewrite `# N "..." flags` lines to bare `#line` before
  the second-pass parse (or use `-fno-canonical-prefixes`-style flag
  on TCC's CPP).
- Skip the `_cch → _h` include rewrites when pre-expand is enabled
  (CPP resolves them itself).
- Run pre-expand once; reparses use the same expanded buffer.
- Then retire `pass_channel_syntax` text scan and other forms that
  pre-expand made the parser able to handle.

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
