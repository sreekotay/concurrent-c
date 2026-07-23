# Verdict

- **Primary:** `prevented`
- **Family:** `locality`
- **Rationale:**
  On the Concurrent-C Result surface, a bare statement-position call to a
  `T!>(E)` function is `unhandled-result` when
  `CC_STRICT_RESULT_UNWRAP=1` (`tests/result_unwrap_unhandled_bare_fail.ccs`).
  Idiomatic code consumes with `!>` / `?>` under `@errhandler` (or an
  explicit binder). That matches safe Rust’s must-use Result for claim A
  on the protected surface.

  Escape hatch (Gap): phase-1 strict unwrap is env-gated (default-on is
  planned); explicit `(void)f()` still compiles; plain C `int` APIs are
  unchecked. Those are off-surface, not a downgrade of the Result path.
- **What would strengthen further:** Default-on strict unwrap; lint or
  ban of `(void)` on Result calls in more modes.
