# `ordered` on a DATA channel: accepted at parse, first plain send fails at runtime

**Status:** OPEN.  **Found:** 2026-07-23, redis @variant conversion
(declaring the reply/request channels `ordered` per the channel spec's
delivery-order rule).

## Symptom

The channel spec (spec/concurrent-c-channel.md "Ordered channels",
spec/concurrent-c-spec-complete.md §7.2/§7.4) defines `ordered` for
**data channels** as per-sender FIFO.  The parser accepts `ordered` on a
data-channel rx endpoint (`T[~N 1:1 ordered <]`, `T*[~N N:1 ordered <]`
— element types other than CCTask), but the very first plain typed send
on the paired tx fails at runtime.  Nothing is delivered; a server built
this way answers every request with its owner-unavailable error.

## Minimized repro

```c
#include <ccc/std/prelude.cch>
#include <stdio.h>

int main(void) {
    int x = 1;
    int*[~8 N:1 >] tx;
    int*[~8 N:1 ordered <] rx;
    cc_channel_pair(&tx, &rx);
    CCResult_bool_CCIoError sr = cc_channel_send(tx, &x);   /* fails */
    if (cc_is_err(sr) || !sr.u.value) { printf("send failed\n"); return 2; }
    return 0;
}
```

Prints `send failed` (same with a by-value struct payload and
`cc_channel_raw_try_send_into`, which returns EINVAL explicitly for
ordered channels — runtime channel.c `cc_chan_try_send_into`).

## Root cause (located, not fixed — compiler/runtime change required)

The shipped `ordered` implementation is task-handle-only:

- `cc/src/visitor/pass_channel_syntax.c` (pair-creation lowering):
  "Ordered channels store CCTask values internally, regardless of the
  declared element type" — when `rx_ordered` it overrides the pair's
  elem size to `sizeof(CCTask)` and clears `allow_take`.
- A plain typed data send then calls into the channel with
  `sizeof(T) != sizeof(CCTask)` and is rejected (elem-size mismatch),
  so the first send fails.
- The ordered recv helper (`cc__chan_recv_ordered`,
  cc/include/ccc/cc_channel.cch) treats every received value as a
  CCTask and awaits it, so even a size-matched payload would be
  misinterpreted.

The data-channel arm of the spec'd `ordered` contract ("per-sender
FIFO") is unimplemented; only task-handle channels
(`cc_channel_send_task`) work.  Either the runtime should implement the
data-channel flag (per-sender FIFO is already the de-facto behavior of
the current backend), or the pair lowering should reject `ordered` on a
non-CCTask element type at compile time instead of breaking the channel
at runtime.

## Workaround

`real_projects/redis/redis_idiomatic.ccs` keeps its reply/request
channels undeclared (default) with a comment citing this bug: the
current backend happens to preserve per-sender FIFO on buffered data
channels (the handoff path is gated by
`cc__chan_buffered_handoff_would_reorder`), but per the spec that is an
implementation property, not a contract.
