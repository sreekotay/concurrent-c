# SHAPE-T3 — non-arena / untracked borrow sent on a channel

- **Links:** Class residual of T3 after arena `channel-stable-borrow`
  (see SHAPE-T3-channel-borrow-send). Proved by compile of untracked
  `cc_slice_from_buffer` + `cc_channel_send` (no fail oracle — that is
  the point).
- **Product / versions:** (n/a — shape study)
- **CWE(s):** CWE-416
- **One paragraph:** Channel-stable-borrow rejects **arena-backed**
  non-unique slice views. A heap or stack buffer wrapped with
  `cc_slice_from_buffer` / untracked `T[:]` is still sendable; the sender
  can free or leave the frame while the receiver holds `.ptr`. Safe Rust
  rejects putting a short-lived `&[u8]` into a message. Concurrent-C does
  not yet — this is a claim-A miss on the Send lattice.
