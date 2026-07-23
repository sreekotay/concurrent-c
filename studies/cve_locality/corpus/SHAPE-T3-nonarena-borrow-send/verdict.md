# Verdict

- **Primary:** `prevented`
- **Family:** `locality`
- **Rationale:**
  Channel-stable-borrow now rejects **any** non-unique, non-static slice
  on a data send — arena views, stack views, and untracked
  `cc_slice_from_buffer` alike (`tests/channel_send_untracked_borrow_fail.ccs`,
  `tests/channel_send_arena_borrow_fail.ccs`). Idiomatic rewrite:
  `cc_slice_from_static`, unique `T[:!]` / `send_take`, or copy into a
  stable arena before send.

  Escape hatch (Gap): raw `T*` / `@unsafe` / hand-built channel payloads
  outside tracked slices.
- **What would strengthen further:** Fuller non-`Send` lattice for
  pointer types beyond slices.
