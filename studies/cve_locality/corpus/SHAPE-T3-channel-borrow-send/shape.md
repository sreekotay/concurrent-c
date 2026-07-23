# Ownership / concurrency shape

- **Taxonomy class(es):** **T3** Borrow across handoff
- **Actors:** Arena/request owner (allocates, may reset) vs channel
  receiver (uses bytes after send returns)
- **The mistake in one sentence:** A non-unique arena view outlived its
  epoch by crossing a channel.
- **Rust angle (claim A):** Safe Rust cannot put a short-lived `&[u8]`
  into a message whose consumer is not bound by that lifetime.
- **Likely CC verdict:** `prevented` for arena-backed non-unique slices
  (`channel-stable-borrow`). Gap: untracked / non-arena borrows and raw
  `T*` may still send.
