# Ownership / concurrency shape

- **Taxonomy class(es):** **T9** Wire / length vs buffer
- **Actors:** Holder of a `CCSlice` / buffer writes or reads at an
  untrusted index
- **The mistake in one sentence:** Index not proven `< len` before a
  raw pointer access.
- **Rust angle (claim A):** `slice[i]` is bounds-checked; silent OOB is
  ill-formed in safe Rust.
- **Likely CC verdict:** `mitigated` — Result-checked `at`/`set` on the
  surface; raw `.ptr[i]` remains Gap.
