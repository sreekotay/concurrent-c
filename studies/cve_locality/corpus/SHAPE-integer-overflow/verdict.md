# Verdict

- **Primary:** `mitigated`
- **Family:** `locality`
- **needs_language:** `default-checked-arith` — optional; would flip toward
  prevented if bare `+` on size paths became ill-formed
- **Rationale:**
  Idiomatic Concurrent-C sizes buffers with `cc_add_i64_checked` /
  `cc_mul_i64_checked` returning `T!>(E)` (see `tests/checked_math_i64_smoke.ccs`).
  Overflow is an error, not a tiny allocation. That matches safe Rust’s
  `checked_*` claim-A spirit. Bare C `size_t need = a + b` still compiles
  and wraps — structure/API mitigation, not a language ban.
- **What would change the verdict:** Default-on checked arithmetic for
  size_t/index math → toward `prevented`.
