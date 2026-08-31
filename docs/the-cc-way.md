# How to Think about CC-Lang

[README](../README.md) · [Getting Started](getting-started.md) ·
[Language Concepts](language-concepts.md) · [Cheatsheet](cheatsheet.md)

CC asks two questions repeatedly: What is the smallest fact that actually changes what may happen? Where is the narrowest place that fact becomes known? Express and enforce that fact there; do not promote unrelated relationships into ownership, scheduling, revocation, or policy.

- **[UFCS](language-concepts.md#3-methods-are-ordinary-functions)** — Attach vocabulary locally without making the operation belong to the type.
- **[Typeviews](typehooks-typeviews.md#2-typeview--faces-and-allow-lists)** — Same-object authority lenses; narrow what a caller may do without wrappers or new allocation.
- **[Arenas](language-concepts.md#4-slices-remember-where-bytes-live)** — Name lifetimes and owners; allocation strategy is secondary.
- **[Walk / extent](cheatsheet.md#walk-for-in)** — The bound is the live extent. Walk it; do not reconstruct `i < .len`. `.len` / `.access` are naked; the mut walk's write-time bound is the Result (`!>`). Slice fields are read-only. `CCString` is an owner: `as_slice()` / dest `char[:] v = s` / `cstr()`, not `.data`. Recipe: [recipe_walk.ccs](../examples/recipe_walk.ccs).
- **Named pointers (`CCBox`)** — A named, nullable pointer handle represented as `{ H *p }`. Copies refer to the same object; `p == NULL` is the canonical dead state (never born or consumed). Ordinary sites use `is_live` / `host`, not `.p`.
- **[Arena-last](cheatsheet.md#keep-pass-the-arena-to-live-on)** — When an operation creates owned output (i.e. allocates memory), make the destination lifetime explicit at that point.
- **[Results / `!>` / `?>`](language-concepts.md#2-errors-map-to-a-value-or-to-control-flow)** — Make fallible transitions force acknowledgement without forcing local policy. Do not make fallible operations appear infallible; keep errors distinct from successful values.
- **[`@defer` / `@defer(err)` / `@defer(ok)`](language-concepts.md#1-cleanup-binds-to-a-place)** — Attach cleanup and recovery obligations to the scope where they arise.
- **[Move / dead-state](cheatsheet.md#lifetime-parents-attach--adopt--create_)** — `cc_move` transfers; the source is empty for teardown. User use after the move is a compile error, not a runtime zero-check.
- **[Single-shot closures](language-concepts.md#5-closures-carry-captures)** — Represent one remaining action or obligation without inventing a larger task object.
- **[Turnstiles / gates](cheatsheet.md#pipeline-turnstile-ccturnstile)** — Express named local admission predicates; not locks, not DAGs.
- **[Tickets](cheatsheet.md#parallel)** — Names for relationships and admission, not counters or schedule positions.
- **Local sigils** — Mark consequential decision boundaries directly in source.
- **[Top-level owners + views](getting-started.md#locality-owned-or-view)** — Prefer one real owner with non-owning views over manufactured shared ownership.

The CC programming mental model:
TUTORIAL=IDIOMATIC=PERFORMANT=PRODUCTION
- **One path** — Tutorial code uses the same idioms and APIs as production code; performance is not a separate programming model.
- **Ownership is binary** — Memory is owned, or it is not. Views, borrows, slices, provenance, authority, and lifetime constraints describe non-owner access; they should not be promoted into additional ownership states.
- **Sharing is not shared ownership** — First look for the actual owner. Introduce transfer only when the application really creates a new independent lifetime.
- **Materialization at boundaries** — Borrow or view while representation is sufficient; copy, serialize, or promote only when the boundary actually requires it.
- **Local predicates** — Independence is primarily a knowledge problem. Ask for the smallest fact that must be true for an operation to proceed, then shrink what it needs to know until “may I proceed?” becomes locally decidable.
- **Relations do not inherit consequences** — Being a child, sharing a value, holding a view, being cancelled, or observing failure does not by itself imply ownership, revocation, destruction, or policy. Couple those facts only where the application actually couples them.
- **Preserve the weakest fact that is actually known** — Don’t compensate for missing application knowledge with ambient world knowledge.
