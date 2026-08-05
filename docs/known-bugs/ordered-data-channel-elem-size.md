# `ordered` on a DATA channel: accepted at parse, first plain send fails at runtime

> **Legacy-front bug writeup** (`pass_channel_syntax.c`). Default `ccc` is
> serdes; kept for archaeology.

**Status:** FIXED.  **Found:** 2026-07-23, redis @variant conversion
(declaring the reply/request channels `ordered` per the channel spec's
delivery-order rule).  **Fixed:** 2026-07-23.

## Symptom

The channel spec (spec/concurrent-c-channel.md "Ordered channels",
spec/concurrent-c-spec-complete.md §7.2/§7.4) defines `ordered` for
**data channels** as per-sender FIFO.  The parser accepted `ordered` on a
data-channel rx endpoint (`T[~N 1:1 ordered <]`, `T*[~N N:1 ordered <]`
— element types other than CCTask), but the very first plain typed send
on the paired tx failed at runtime.  Nothing was delivered; a server
built this way answered every request with its owner-unavailable error.

## Root cause

The shipped `ordered` implementation was task-handle-only.  The
pair-creation lowering (`cc__rewrite_channel_pair_calls_text`,
cc/src/visitor/pass_channel_syntax.c) treated EVERY `ordered` rx as a
task-handle channel: it overrode the pair's elem size to
`sizeof(CCTask)`, cleared `allow_take`, and set the runtime `is_ordered`
flag.  A plain typed data send then called into the channel with
`sizeof(T) != sizeof(CCTask)` and was rejected (`cc_chan_ensure_buf`
elem-size mismatch → EINVAL), so the first send failed;
`cc_chan_try_send_into` rejected ordered channels outright; and the
ordered recv helper (`cc__chan_recv_ordered`,
cc/include/ccc/cc_channel.cch) awaited every received value as a CCTask,
so even a size-matched payload would have been misinterpreted.

## Fix

`ordered` is one delivery-order property applied to the channel's
payload kind (spec "Ordered channels"), and the pair lowering now
selects the task machinery only for channels that actually carry task
handles:

- Task-handle channels — element type is `CCTask` itself, or the paired
  tx is fed via the send_task family in the TU
  (`cc_channel_send_task[_hybrid](tx, ...)` or UFCS
  `tx.send_task[_hybrid](...)`; a task channel's element type is spelled
  as the task's RESULT type, so the declaration alone cannot identify
  it) — keep the exact previous lowering: elem size `sizeof(CCTask)`,
  `allow_take = 0`, runtime `is_ordered = 1` (typed recv awaits each
  handle; FIFO on handles).
- Data channels lower to the NORMAL data-channel machinery: declared
  element size, plain send/recv, runtime `is_ordered = 0`.  The
  attribute is the per-sender FIFO contract marker; the backend's
  buffered path already delivers per-sender FIFO (the direct-handoff
  path is gated by `cc__chan_buffered_handoff_would_reorder`, pinned by
  `tests/channel_buffered_handoff_fifo_smoke.ccs`).

See `cc__ordered_elem_is_task` / `cc__chan_tx_has_task_send` and the
`ordered_task` selection in cc/src/visitor/pass_channel_syntax.c.

Pinned by `tests/chan_ordered_data_smoke.ccs` (struct payload send/recv
through `T[~N 1:1 ordered <]` incl. `cc_channel_raw_try_send_into`, the
minimized `int*[~N N:1 ordered <]` repro, and a 50k-item FIFO stream
assertion).  Task-handle `ordered` behavior is unchanged
(`tests/ordered_channel_smoke.ccs`,
`tests/ordered_channel_spawned_sender_smoke.ccs`,
`tests/send_task_hybrid_smoke.ccs` all pass as-is).
`real_projects/redis/redis_idiomatic.ccs` now declares its reply/request
rx endpoints `ordered` (the former workaround comments are gone).

## Lesson

An attribute that names a contract ("what flows arrives in order") must
not be welded to one implementation of that contract.  The lowering
keyed a runtime representation change (CCTask wire format) off a purely
declarative marker, so declaring the documented guarantee on a data
channel silently swapped its wire format and broke the first send.
