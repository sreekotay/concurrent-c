# SHAPE-T3 — arena / epoch borrow sent across a channel

- **Links:** Class exemplar (CWE-416 via handoff). Redis pressure:
  `redis_reply_stabilize_for_channel` in
  `real_projects/redis/redis_idiomatic.ccs`. CC evidence:
  `tests/channel_send_arena_borrow_fail.ccs` (channel-stable-borrow).
- **Product / versions:** (n/a — shape study)
- **CWE(s):** CWE-416
- **One paragraph:** A non-unique view into an arena (or request epoch)
  is placed on a channel; the receiver keeps using those bytes after the
  sender resets / frees / reuses the arena. In C this is a silent
  protocol bug. Safe Rust rejects sending `&[u8]` that does not outlive
  the channel message. Concurrent-C rejects sending a tracked arena
  slice borrow on a channel data send.
