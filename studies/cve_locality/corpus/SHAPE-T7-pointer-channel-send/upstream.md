# SHAPE-T7 — raw `T*` / pointer field across a channel

- **Links:** Class residual after slice `channel-stable-borrow` and
  stack-pointer alias bans (SHAPE-T3-*, SHAPE-T7-shared-mut-spawn).
  Closed by `pointer-channel-send-ban` (fail oracle:
  `tests/channel_send_pointer_field_fail.ccs`).
- **Product / versions:** (n/a — shape study)
- **CWE(s):** CWE-416
- **One paragraph:** Channel-stable-borrow tracks **slices**. A message
  carrying a raw `T*` field in a by-value struct previously sent by
  value; the sender could free while the receiver held the pointer.
  Concurrent-C rejects that aggregate send. Bare `T*` handle payloads
  remain a narrower Gap outside this shape's prevented path.
