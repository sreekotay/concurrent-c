# Verdict

- **Primary:** `prevented`
- **Family:** `locality`
- **needs_language:** (optional) push more APIs to return `T[:!]` by default
  so the protected path is the common one
- **Rationale:**
  Safe Rust’s “use after move / two owners” ban is claim A. Concurrent-C
  already implements the same rule for unique-provenance slices: cannot
  copy without `cc_move`, cannot use after move (including after capture
  move into a closure). That is a real prevent, not structure-only.

  The CVE-shaped bug with plain `malloc` + two `free`s is still writable —
  score that under escape hatches / still_expressible controls (see
  CVE-2013-4153), not as a failure of the unique-slice rule.
- **What would strengthen further:** Adopt/FFI paths that mint unique
  slices by default; ban double-`free` on raw pointers is out of scope
  without becoming a different language.
