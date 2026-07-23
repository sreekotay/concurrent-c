# Ownership / concurrency shape

- **Taxonomy class(es):** **T9** Wire / length vs buffer (size arithmetic)
- **Actors:** Caller builds `need = a + b` then `alloc(need)` and fills
  `a+b` bytes of payload
- **The mistake in one sentence:** Wrapping addition produced a small
  `need` while the fill still assumed the mathematical sum.
- **Rust angle (claim A):** `checked_add` / debug overflow checks; silent
  wrap-to-alloc is not idiomatic safe Rust.
- **Likely CC verdict:** `mitigated` — `cc_*_i64_checked` + Result force
  the honest path; C `+` still wraps.
