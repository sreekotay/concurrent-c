# Verdict

- **Primary:** `mitigated`
- **Family:** `locality`
- **needs_language:** —
- **Rationale:**
  Protected byte-slice index ops (`at` / `get_checked` / `set`) return
  `CC_ERR_INVALID_ARG` on out-of-bounds in all builds — no soft-zero `at`,
  no debug/release split. That matches safe Rust’s checked-index claim-A
  spirit on the idiomatic surface. Raw `s.ptr[i] =` past `len` still
  compiles (Gap), same class as Rust `get_unchecked` / `unsafe`.
- **What would change the verdict:** Checker ban on indexed stores through
  slice `.ptr` → toward `prevented`.
