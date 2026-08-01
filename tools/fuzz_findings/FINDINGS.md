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

## Fixed — `@destroy` ran on a declaration that never executed

Function-scope cleanups all run from one `__cc_cleanup_N:` label that every
early exit jumps to, and the label ran every registered cleanup — including
ones whose declaration sat after the exit and had never executed, destroying
an uninitialized value. Being an uninitialized read it was layout-dependent:
`py_unreached_destroy_after_early_return_crash.shcc` faulted releasing a
`CCPyObj`, while the same shape with a `CCArena` survived.

Fixed with a per-function high-water mark: each function-scope cleanup stamps
`__cc_defer_hw_N` at its declaration, and the label runs cleanup k only when
the mark passed it. Function-scope declarations execute in source order, so
reaching the k-th implies the first k-1 also ran. Pinned by
`tests/destroy_unreached_decl_smoke.ccs`.

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

## Fixed — a discarded `@emit` fragment was silent

Inside a `@comptime` block, `@emit(ANCHOR, `...`)` splices while
`@emit(`...`, arena)` yields a fragment — the factory form. Writing the
factory form as a bare statement built the fragment and dropped it with no
diagnostic; the code it was meant to emit simply never appeared, surfacing
later as an implicit-function-declaration on the missing symbol.

The yielded form at statement position is now an error naming both fixes (the
anchor form for blocks, `return @emit(...)` for factories). Pinned by
`tests/comptime_emit_discarded_fail.ccs`.

## Known limitation

`cc_emit_error` from a factory body attributes to the enclosing `@comptime`
block's line (block-level attribution, see `cc__diag_origin_for_pos`), so the
first diagnostic line points at the top of the file; the second line names the
use site and the instance.
