# Ownership / concurrency shape

- **Taxonomy class(es):** **T3** Borrow across handoff (residual)
- **Actors:** Sender (owns heap/stack buffer) vs channel receiver (holds
  a copy of the slice header / pointer)
- **The mistake in one sentence:** A non-unique, non-arena view crossed
  a channel without materialize; lifetime ended on the sender side.
- **Rust angle (claim A):** Safe Rust cannot send `&[u8]` that does not
  outlive the consumer — ill-formed. Not `unsafe` parity.
- **Likely CC verdict:** `still_expressible` — arena path is prevented;
  untracked / `from_buffer` path still compiles.
