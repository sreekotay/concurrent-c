# QuickJS Lifetime Boundary Study

**Status:** living study — not a product claim.  
**Question:** Can a systems program state the lifetime and concurrency
relationships that actually exist at a foreign runtime boundary without
allowing those relationships to reorganize unrelated program structure?

This folder is a feedback loop in the same spirit as
[`studies/cve_locality/`](../cve_locality/) and [`real_projects/`](../../real_projects/).

## Two complementary pressures

1. **Track A — hostile foreign-protocol containment**  
   Impose an awkward but fixed foreign lifetime protocol and ask whether
   CC can contain it. Rust/rquickjs receives the same contract.

2. **Track B — natural runtime pressure (scaffold)**  
   Take a real QuickJS-shaped subsystem (txiki.js timers + libuv) where
   long-lived claims arise from the work, and ask whether the CC port
   still reads like that work. Implementation comes later.

The hostile test asks whether CC can absorb somebody else’s ontology.  
The natural test asks whether CC invents unnecessary ontology of its own.

## Fixed substrate

| Component | Pin | Notes |
|-----------|-----|-------|
| Engine | quickjs-ng | Same commit for CC and Rust — see [`PINNED.md`](PINNED.md) |
| Rust binding | rquickjs | Against that engine |
| Track B (later) | txiki.js + libuv | Not implemented in this slice |

No implementation gets a different QuickJS build, GC policy, compiler
flags, or workload merely because it performs better under one
configuration.

**Design constraint:** Track A must **not** grow
[`ccc/script/quickjs.cch`](../../cc/include/ccc/script/quickjs.cch). The
stdlib header only bootstraps `JSRuntime` / `JSContext`. Hostile
semantics live in this study.

## Distinctions that must survive

```text
identity
≠ access
≠ temporary borrow
≠ lifetime claim
≠ weak observation
≠ destruction authority
```

A persistent claim means the host currently requires this JS value to
remain live. Releasing a claim means only that this host claim no longer
exists — not “destroy the JS object.”

## Progression

```text
1. hostile retain/release/weak API     ← Track A stage 1
2. hostile callback registry           ← Track A stage 2
3. txiki-style timers                  ← Track B (later)
4. timers + shutdown                   ← Track B (later)
5. one async I/O primitive             ← later
6. sockets/fetch/workers               ← only if earlier stages remain interesting
```

Every stage keeps its own results. Do not discard an earlier loss because
a later design change fixes it.

## Preregistered outcomes

All of these are legitimate conclusions before implementation:

| Outcome | Meaning |
|---------|---------|
| **A** | CC contains the pressure well — foreign protocol localized; code still reads as create/retain/fire/release |
| **B** | CC is readable but pays materially (memory or CPU) |
| **C** | CC requires pervasive machinery — infection of unrelated types |
| **D** | Rust contains the relation better |
| **E** | Upstream C (Track B) remains best |

## Layout

| Path | Role |
|------|------|
| [`PINNED.md`](PINNED.md) | Version pins |
| [`PROTOCOL.md`](PROTOCOL.md) | Checkpoint / oracle / receipt format |
| [`track_a/`](track_a/) | Hostile contract, CC + Rust, workload, shape review |
| [`track_b/`](track_b/) | Timer scaffold only |
| [`scripts/run_track_a.sh`](scripts/run_track_a.sh) | Build, run, oracle compare |

## How to run Track A

```sh
# Attach pinned quickjs-ng (see PINNED.md)
export CC_QUICKJS_SRC=/path/to/quickjs-ng

./studies/quickjs_lifetime/scripts/run_track_a.sh
```

Correctness is pass/fail and precedes performance. Shape review
([`track_a/shape_review.md`](track_a/shape_review.md)) is filled after
both implementations pass the oracle.

## Infection vs containment

A hostile protocol wrapped in one narrow layer is **containment**.  
A hostile protocol reorganizing the application around itself is
**infection**.

The thesis takes a hit if the implementation repeatedly requires
stronger or more pervasive facts than the work itself supplies.
