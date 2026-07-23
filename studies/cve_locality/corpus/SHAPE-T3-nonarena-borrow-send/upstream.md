# SHAPE-T3 — non-arena / untracked borrow sent on a channel

- **Links:** Class residual of T3 after the first arena-only
  `channel-stable-borrow` rule. Closed by extending that rule to all
  non-unique, non-static slices. Evidence:
  `tests/channel_send_untracked_borrow_fail.ccs`.
- **Product / versions:** (n/a — shape study)
- **CWE(s):** CWE-416
- **One paragraph:** A heap or stack buffer wrapped with
  `cc_slice_from_buffer` was still sendable after arena views were banned;
  the sender could free or leave the frame while the receiver held `.ptr`.
  Concurrent-C now rejects that send at compile time — same claim-A bar as
  safe Rust for short-lived borrows in messages.
