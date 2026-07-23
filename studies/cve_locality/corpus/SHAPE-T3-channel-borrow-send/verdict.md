# Verdict

- **Primary:** `prevented`
- **Family:** `locality`
- **Rationale:**
  The buggy shape is sending a non-unique arena slice view on a channel
  while the arena epoch can still advance. Concurrent-C’s
  `channel-stable-borrow` checker makes that ill-formed
  (`tests/channel_send_arena_borrow_fail.ccs`). Idiomatic rewrite matches
  redis: materialize (unique / static / copy into a reply or batch arena)
  before send, then the receiver owns stable bytes.

  Escape hatch (Gap): non-arena borrows, untracked slices, and raw `T*`
  are not covered by the same rule — fuller Send lattice remains backlog.
- **What would strengthen further:** Ban send of any non-unique /
  non-`'static` slice view, not only arena-backed ones.
