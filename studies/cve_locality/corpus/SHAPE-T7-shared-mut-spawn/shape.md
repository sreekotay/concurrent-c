# Ownership / concurrency shape

- **Taxonomy class(es):** **T7** Shared mutable without safe owner
- **Actors:** Parent fiber holding `x`; child spawn capturing `&x` and
  writing
- **The mistake in one sentence:** Shared mutable state crossed a
  concurrency boundary without a synchronized owner.
- **Rust angle (claim A):** `&mut T` / non-`Sync` interior mutability
  across threads is ill-formed in safe Rust.
- **CC angle:** `n.spawn(() => [&x] { x++; })` is a compile error
  (enforced). Idiomatic fix: `Atomic` / `Mutex` / channel single-writer.
  Gap (escape, not the scored shape): value-capture a raw `T*` that
  aliases `x`, or `@unsafe`.
