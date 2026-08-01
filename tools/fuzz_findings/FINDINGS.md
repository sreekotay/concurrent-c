# Findings

Triaged reproducers live beside this file; the logs record every hit.

## Fixed

Three comment-inertness bugs found by `fuzz_comment_insert.shcc` at seed 1,
all pinned by `tests/comment_inert_boundaries_smoke.ccs`:

- `?>` / `!>` binder scan skipped whitespace only, so `mk(41) ?>/*x*/ (e) 0`
  lost the binder and failed to parse.
- The inferred-Result-constructor pass looked for a function body's `{` with a
  whitespace-only scan, so `bool !>(CCError) f(int v) /*x*/{` left the
  enclosing function's ok/err types unset and `cc_ok(...)` stayed generic.
- The closure-literal pass looked for the capture list after `=>` with a
  whitespace-only scan, so `() =>/*x*/ [wp] { … }` was reported as the retired
  `[captures]() =>` syntax.

One emit-block hole found while probing factory error paths, pinned by
`tests/comptime_factory_arg_oob_fail.ccs`:

- `arg(i)` past the type-argument count interpolated nothing and the build
  SUCCEEDED with a malformed fragment. `arg(i)` is now bounds-checked and
  reports through `cc_emit_error`; the factory site fails the build.

## Open — compiler crash (py lowering)

`py_import_after_early_return_crash.shcc` segfaults the compiler, including
under `--emit-c-only`, so it is a lowering crash rather than a runtime one.
The crashing statement (`CCPyObj json = py.import("json") !> @destroy;`) is
unreachable at runtime — an earlier handler returns first — yet removing it
makes the file compile. Found by hand during the py interop stress round;
the mutation fuzzer has not reproduced it (seed 7, 1200 iterations, clean).

## Open — comment-insertion soak (seed 7, 800 iterations)

42 findings over 800 iterations (0 output divergences, 0 timeouts, 0 crashes
— every one is a compile-or-run failure, i.e. a comment that should have been
inert changed whether the program builds). Reproducers are the
`comment_*.ccs` files beside this document; each is the corpus file with one
`/*x*/` inserted at the recorded offset.

Recurring shapes, by insertion context:

- before a body's `{` (largest cluster)
- inside a result-type annotation: `/*x*/int !>(CCError) r`,
  `!>(/*x*/CCIoError) r`
- inside a type-argument list: `Pair::[int/*x*/, double]`, `Vec::[Pt/*x*/]`
- before a closure arrow, a `@comptime for` head, a declarator, a call's `(`,
  a trailing `@as`

Not yet triaged individually — the three already-fixed classes were all
whitespace-only scans, and these shapes suggest the same.

## Open — a discarded `@emit` fragment is silent

Inside a plain `@comptime { }` block, `@emit(ANCHOR, `...`)` emits, while
`@emit(`...`, arena)` *returns* a fragment — the factory form. Writing the
factory form as a bare statement in a block therefore builds the fragment and
throws it away, with no diagnostic: the code it was meant to emit simply never
appears, and the failure surfaces later as an implicit-function-declaration on
the missing symbol.

Repro: a block containing ``@emit(`static int good_fn(void) { return 7; }`,
arena);`` and a `main` that calls `good_fn()`.

Both spellings are legitimate in their own context, so the fix is a diagnostic
on the discarded value, pointing at the anchor form for blocks and at
`return @emit(...)` for factories.

## Known limitation

`cc_emit_error` from a factory body attributes to the enclosing `@comptime`
block's line (block-level attribution, see `cc__diag_origin_for_pos`), so the
first diagnostic line points at the top of the file; the second line names the
use site and the instance.
