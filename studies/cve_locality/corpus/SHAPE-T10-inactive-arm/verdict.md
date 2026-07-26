# Verdict

- **Primary:** `prevented`
- **Family:** `variant`
- **Rationale:**
  Safe Rust’s enum match / refuseing makes inactive-arm use ill-formed.
  Concurrent-C `@variant` matches that on the protected surface: sugar
  projection requires kind/switch/`!>`/`?>` domination, and user-written
  raw `.u` reach-in is a compile-time error for every `@variant` (packed
  and unpacked). Schema `one of` shares the same surface; generated
  fill/write helpers retain raw layout access.

  Redis’s idiomatic `RedisValue` path already uses portable projections
  only — no change required there.
- **What would strengthen further:** Flow-sensitive domination across
  `&&` / early return; struct-field bases for projection checking.
