# Ownership / concurrency shape

- **Taxonomy class(es):** **T7** Shared mutable without owner,
  **T3** Borrow across handoff
- **Actors:** Sender frees heap behind `T*` after `cc_channel_send` of
  the pointer value; receiver still holds a copy
- **The mistake in one sentence:** A bare pointer crossed a channel
  without transferring ownership of the pointee.
- **Rust angle (claim A):** Safe Rust does not put short-lived raw
  ownership into a cross-thread message without `unsafe`.
- **Likely CC verdict:** `still_expressible` — aggregate pointer-*fields*
  banned; bare `T*` handle send open (redis).
