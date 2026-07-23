# Ownership / concurrency shape

- **Taxonomy class(es):** **T7** Shared mutable without owner,
  **T3** Borrow across handoff (pointer form)
- **Actors:** Sender (owns heap buffer behind `T*`) vs channel receiver
  (holds a copy of the pointer)
- **The mistake in one sentence:** A raw pointer crossed a channel
  without transferring ownership of the pointee.
- **Rust angle (claim A):** Safe Rust cannot put `&T` / non-`Send` raw
  ownership into a cross-thread message — ill-formed. Not `unsafe`
  parity.
- **Likely CC verdict:** `still_expressible` — slices are prevented;
  `T*` / pointer fields are not.
