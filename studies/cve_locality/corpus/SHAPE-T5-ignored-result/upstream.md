# SHAPE-T5 — ignored fallible result / half-updated state

- **Links:** Class exemplar (CWE-252 unchecked return / CWE-391). CC
  evidence: `tests/result_unwrap_unhandled_bare_fail.ccs`,
  `tests/err_syntax_no_handler_fail.ccs`.
- **Product / versions:** (n/a — shape study)
- **CWE(s):** CWE-252, CWE-391
- **One paragraph:** A fallible operation returns an error (or Result),
  the caller discards it, and execution continues with half-updated
  resources or invalid state. In C this is ignoring `int` / `errno`. In
  safe Rust an unused `Result` is a hard error (`must_use`). Concurrent-C
  forces consumption of `T!>(E)` on the strict Result surface via `!>` /
  `?>` / `@errhandler` (bare statement-position calls are
  `unhandled-result`).
