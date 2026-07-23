# Ownership / concurrency shape

- **Taxonomy class(es):** **T3** Borrow across handoff
- **Actors:** Sender (owns heap/stack buffer) vs channel receiver (holds
  a copy of the slice header / pointer)
- **The mistake in one sentence:** A non-unique, non-static view crossed
  a channel without materialize; lifetime ended on the sender side.
- **Rust angle (claim A):** Safe Rust cannot send `&[u8]` that does not
  outlive the consumer — ill-formed.
- **Likely CC verdict:** `prevented` — channel-stable-borrow covers
  untracked / from_buffer as well as arena views.
