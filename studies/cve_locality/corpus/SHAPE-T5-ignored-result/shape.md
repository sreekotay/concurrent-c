# Ownership / concurrency shape

- **Taxonomy class(es):** **T5** Error ignore / nonlocal
- **Actors:** Caller that must observe success/failure; callee that may
  leave resources half-live on Err
- **The mistake in one sentence:** A Result / error was discarded and
  the happy path continued.
- **Rust angle (claim A):** Safe Rust rejects unused `Result` (`must_use`);
  `?` / match forces handling.
- **Likely CC verdict:** `prevented` on the `T!>(E)` surface under strict
  unwrap. Gap: C `int` APIs and explicit `(void)f()` discard.
