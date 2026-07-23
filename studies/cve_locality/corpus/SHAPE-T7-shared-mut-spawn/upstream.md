# SHAPE-T7 — shared mutation across spawn (Send / data-race analogue)

- **Links:** Spec §6.1 reference-capture mutation rules;
  `tests/closure_ref_capture_mutation_fail.ccs`;
  `tests/closure_unsafe_ref_mutation_smoke.ccs` (`@unsafe` hatch)
- **Product / versions:** Language shape (Rust claim A: shared `&mut`
  / non-`Sync` mutation across threads is ill-formed in safe code)
- **CWE(s):** CWE-362 (race), CWE-366
- **One paragraph:** Spawning a nursery/thread closure with an explicit
  reference capture `[&x]` and then mutating `x` is a classic data race
  if another fiber also touches `x`. Concurrent-C rejects that mutation
  at compile time unless the type is a safe wrapper (`Atomic`, `Mutex`,
  channel handle) or the closure is `@unsafe`. That is the Send/Sync
  *mutation* half of claim A, enforced today.
