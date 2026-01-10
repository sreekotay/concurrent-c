# Concurrent-C Runtime/Stdlib Status (WIP tracking)

This file tracks implementation vs. spec without modifying the spec docs.

## Channels
- Current: pthread-backed ring buffer; fixed capacity; elem size locked on first send; blocking/try/timed + `CCDeadline` helpers; typed wrappers (pointer/value); nursery-aware send/recv; pointer-only `send_take` gated by allow flag; backpressure modes (block, drop-new, drop-old); nursery auto-closes registered channels; async send/recv via executor offload; non-blocking match helper (poll-style); blocking select and async select (channel handle + future wrapper); native async/state-machine channel variant (`cc_async_chan_*`) for executor-free awaits; convenience macros to build match cases; smoke tests for async select (ready/timeout/closed) and send_take rejection on value channels.
- Missing: richer zero-copy eligibility rules (unique slices), deeper @match integration on the async/state-machine path, and spec-level alignment for async select/future helpers.

## Scheduler/Nursery
- Current: pthread spawn/join, sleep; cooperative `CCDeadline`; nursery cancel flag + deadline helpers.
- Missing: executor/offload for async I/O, automatic deadline propagation to blocking ops, closing-channels-on-exit behavior, error propagation semantics matching spec.

## I/O
- Current: header-only sync `open/close/read_all/read/line/read(n)/write/seek/tell/sync` with Result/IoError; executor-backed async wrappers (offload via CCExec + channel handle) with deadline-aware submit/run and cancellation flag checked in jobs and pre-submit; path helpers (sep/is_abs/join/dirname/basename); buffered reader/writer helpers; errno mapping includes Busy/ConnectionClosed.
- Missing: forceful mid-flight cancellation (jobs check flag but no thread interruption), platform backends (io_uring/kqueue), fuller IoError mapping/platform nuances.

## Executor / Async offload
- Current: portable pthread-backed executor (`cc_exec`) with bounded queue; async I/O wrappers (`std/io.cch` + `std/async_io.cch`) offload work and signal via channel-based handle; plugin-ready async backend interface with poll-based backend stub and lazy auto-probe (env `CC_RUNTIME_BACKEND`) defaulting poll→executor; future wrapper (`CCFuture`) for async ops and async channel select; smoke coverage includes async channel select.
- Missing: richer futures/promise ergonomics beyond the basic wrapper; higher-performance platform backends (io_uring/kqueue) and mid-flight cancellation improvements.

## Collections
- Current: arena-backed dynamic Vec/Map with growth until arena exhaustion; optional heap variants (tool-only); prelude exports Vec/Chan/Map; map foreach helper and slice hash helpers; Vec foreach macro; hash helpers for slice/u64; convenience map decls for slices/u64.
- Missing: growth limit knobs, more ergonomic iteration helpers if needed, default hash/eq for other common types.

## Strings
- Current: header-only slice helpers (trim, split_all, replace, parse i64/u64/f64/bool), arena-backed builder with append/format; many core methods covered.
- Missing: full UTF-8/grapheme handling (deferred per spec), any remaining slice UFCS methods not yet added.

## Spec alignment
- Spec remains authoritative; this file tracks current implementation gaps. No spec edits made for interim behavior.

## Next suggested steps
- Channels: add non-blocking timed helpers using `CCDeadline` (done) and implement backpressure/zero-copy send_take; closing(ch) on nursery exit.
- Scheduler: add executor offload for async I/O and deadline propagation hooks.
- I/O: implement async offload, path utilities, better IoError mapping.
- Collections: add iterators and hash helpers; clarify Map opt-in in docs/readme if desired.

