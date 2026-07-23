# Verdict

- **Primary:** `prevented`
- **Family:** `locality`
- **needs_language:** (none for the stack-slice case)
- **Rationale:**
  The C footgun is `pthread_create` / fire-and-forget with a pointer into a
  local buffer, then return. Safe Rust cannot express that without `unsafe`.
  Concurrent-C already rejects capturing a stack-backed `T[:]` into a
  closure that escapes (return, global store, or nursery that outlives the
  frame). Idiomatic fix is the same as Rust’s: own the bytes in an arena /
  heap (or keep the nursery inside the frame and join before return).

  Escape hatch: raw `char*` capture (untracked) — still expressible, not
  the idiomatic path. That parallels Rust `unsafe`.
- **What would change the verdict:** If untracked raw pointers into stack
  were also rejected when captured into escaping closures (stronger).
