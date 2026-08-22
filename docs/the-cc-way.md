# How to Use CC-Lang

[README](../README.md) · [Getting Started](getting-started.md) ·
[Cheatsheet](cheatsheet.md) · [Stdlib Design](plans/stdlib_properly_cc_plan.md)

CC asks two questions repeatedly: What is the smallest fact that actually changes what may happen? Where is the narrowest place that fact becomes known? Express and enforce that fact there; do not promote unrelated relationships into ownership, scheduling, revocation, or policy.

- **UFCS** — Attach vocabulary locally without making the operation belong to the type.
- **Typeviews** — Same-object authority lenses; narrow what a caller may do without wrappers or new allocation.
- **Arenas** — Name lifetimes and owners; allocation strategy is secondary.
- **Arena-last** — When an operation creates owned output (i.e. allocates memory), make the destination lifetime explicit at that point.
- **Results / `!>` / `?>`** — Make fallible transitions force acknowledgement without forcing local policy. Refuse making fallible things appear infallible and separate errors from results.
- **`@defer` / `@defer(err)` / `@defer(ok)`** — Attach cleanup and recovery obligations to the scope where they arise
- **Move / dead-state** — Use transfer semantics only where transfer itself is consequential. See cc_arena_adopt
- **Single-shot closures** — Represent one remaining action or obligation without inventing a larger task object.
- **Turnstiles / gates** — Express named local admission predicates; not locks, not DAGs.
- **Tickets** — Names for relationships and admission, not counters or schedule positions.
- **Local sigils** — Mark consequential decision boundaries directly in source.
- **Top-level owners + views** — Prefer one real owner with non-owning views over manufactured shared ownership.



The CC programming mental model:
- **TUTORIAL=IDIOMATIC=PERFORMANT+PRODUCTON**
- **Ownership is binary** — Memory is owned, or it is not. Views, borrows, slices, provenance, authority, and lifetime constraints describe non-owner access; they should not be promoted into additional ownership states.
- **Sharing is not shared ownership** — First look for the actual owner. Introduce transfer only when the application really creates a new independent lifetime.
- **Materialization at boundaries** — Borrow or view while representation is sufficient; copy, serialize, or promote only when the boundary actually requires it.
- **Predicates** — Ask for the smallest fact that must be true for an operation to proceed independently. If that fact can be stated locally, concurrency and coordination should be built around that predicate rather than around a global schedule or world-state.
- **Predicated execution** — Independence is primarily a knowledge problem: shrink what an operation needs to know until “may I proceed?” becomes locally decidable.
- **Relations do not inherit consequences** — Being a child, sharing a value, holding a view, being cancelled, or observing failure does not by itself imply ownership, revocation, destruction, or policy. Couple those facts only where the application actually couples them.
- **Preserve the weakest fact that is actually known** Don’t compensate for missing application knowledge with ambient world knowledge.