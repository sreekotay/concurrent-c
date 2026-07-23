# Ownership / concurrency shape

- **Taxonomy class(es):** **T8** Double free / wrong deleter
- **Actors:** Two bindings that both think they own one allocation
- **The mistake in one sentence:** Ownership was copied, not moved; both
  sides tore down the same block.
- **Rust angle (claim A):** Move-only types + destructive move; use of
  moved value is ill-formed. Not rewrite-luck.
- **Likely CC verdict:** `prevented` on the unique-slice path; raw
  `char*`/`malloc` twin stays expressible (document as escape, not the
  idiomatic score).
