# SHAPE-T7 — raw `T*` / pointer field across a channel

- **Links:** Class residual after slice `channel-stable-borrow` and
  stack-pointer alias bans (SHAPE-T3-*, SHAPE-T7-shared-mut-spawn).
  Proved by compile of `struct { char* p; }` send (no fail oracle —
  that is the point).
- **Product / versions:** (n/a — shape study)
- **CWE(s):** CWE-416
- **One paragraph:** Channel-stable-borrow tracks **slices**. A message
  carrying a raw `T*` (or a struct field pointing at heap/stack) still
  sends by value; the sender can free while the receiver holds the
  pointer. Safe Rust rejects non-`Send` / short-lived references in
  messages. Concurrent-C does not yet — claim-A miss on the non-slice
  Send lattice.
