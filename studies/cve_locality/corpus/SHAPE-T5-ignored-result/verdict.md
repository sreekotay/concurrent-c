# Verdict

- **Primary:** `prevented`
- **Family:** `locality`
- **Rationale:**
  On the Concurrent-C Result surface, a bare statement-position call to a
  `T!>(E)` function is `unhandled-result`
  (`tests/result_unwrap_unhandled_bare_fail.ccs`). The scan is on by
  default; `CC_STRICT_RESULT_UNWRAP=0` opts out. Idiomatic code consumes
  with `!>` / `?>` under `@errhandler` (or an explicit binder). That
  matches safe Rust’s must-use Result for claim A on the protected surface.

  Escape hatch (Gap): explicit `(void)f()` still compiles; plain C `int`
  APIs are unchecked; the scan is conservative and does not flag
  label-prefixed or indirect calls. Those are off-surface, not a downgrade
  of the Result path.
- **What would strengthen further:** Lint or ban of `(void)` on Result
  calls in more modes; flagging indirect calls through function pointers.
