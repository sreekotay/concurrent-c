# fuzz_findings

Reproducers and triage notes captured by the fuzzers in `tools/`:

- `tools/fuzz_comment_insert.shcc` — behavior-preservation fuzzer.  Inserts a
  block comment `/*x*/` at a random code position in a known-green test and
  expects identical build/run behavior.  Divergences land here as
  `comment_*.ccs` mutants plus a line in `comment_findings.log`.
- `tools/fuzz_mutate.shcc` — crash-oracle fuzzer.  Applies byte-level
  mutations (span delete/duplicate, byte replace, nasty-token insert,
  template-interior edits) to any test and expects the compiler to either
  accept the input or die with an ordinary diagnostic.  Crashes, hangs,
  internal-error paths, and accepted-but-invalid emitted C land here as
  `mutate_*.ccs` mutants plus a line in `mutate_findings.log`.

`FINDINGS.md` describes the triaged, minimized reproducers.

Nothing in this directory is a test: the test runner (`tools/cc_test.c`)
only scans `tests/`.  Files here are inputs for compiler triage; do not
"fix" them into passing tests without understanding the underlying bug.
