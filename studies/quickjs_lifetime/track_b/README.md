# Track B — natural timer pressure (scaffold)

**Status:** scaffold only — not implemented in this slice.

## Goal

Port a txiki.js-shaped timer slice (setTimeout / clearTimeout + shutdown)
onto Concurrent-C and Rust, then compare against an upstream txiki C
reference build. Lifetime facts come from the work itself: libuv handle
lifetime ≠ logical timer ≠ JS callback claim.

## Why not quickjs-libc timers

QuickJS-ng’s `quickjs-libc` timers are not txiki’s libuv integration.
Track B requires new libuv glue — not [`js.cch`](../../../cc/include/ccc/script/js.cch)
(Node host loop).

## Pins

See [`../PINNED.md`](../PINNED.md) for txiki.js + libuv SHAs (fill before
implementation).

## Slice definition (later)

1. Extract timer module + shutdown hooks from pinned txiki.
2. Shared workload: [`workload/timers.js`](workload/timers.js).
3. Three implementations:
   - `c/` — upstream txiki reference build instructions
   - `cc/` — Concurrent-C port
   - `rust/` — Rust + libuv (or txiki-compatible) binding
4. Oracle: txiki upstream timer tests + study checkpoints.

## Slots

```text
track_b/c/      # empty — reference build notes land here
track_b/cc/     # empty — CC timer host later
track_b/rust/   # empty — Rust twin later
```

Do not add libuv integration in this scaffold slice.
