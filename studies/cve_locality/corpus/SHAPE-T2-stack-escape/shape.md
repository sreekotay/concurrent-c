# Ownership / concurrency shape

- **Taxonomy class(es):** **T2** Capture escape
- **Actors:** Creating frame (owns stack buffer) vs longer-lived task/nursery
- **The mistake in one sentence:** A view into frame-local memory outlived
  the frame.
- **Rust angle (claim A):** Safe Rust rejects storing `&[u8]` / `&T` from a
  local into a `'static` / escaping task; scoped threads are the opt-in
  that keeps the borrow inside a join. Rewrite-luck (claim B) is irrelevant.
- **Likely CC verdict:** `prevented` — stack-slice capture into an escaping
  closure is already ill-formed (same checker family as return/store escape).
