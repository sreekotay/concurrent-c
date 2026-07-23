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

  Escape hatch (Gap): raw `T*` / `@unsafe` outside tracked slices.
  Untracked `from_buffer` sends are covered by the same rule
  (SHAPE-T3-nonarena-borrow-send).
- **What would strengthen further:** Non-slice `T*` Send lattice.
