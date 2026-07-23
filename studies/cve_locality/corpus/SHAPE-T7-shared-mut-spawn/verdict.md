# Verdict

- **Primary:** `prevented`
- **Family:** `locality`
- **needs_language:** (optional strengthen) treat value-captured raw
  pointers that alias outer locals like ref captures — toward fuller
  Send/Sync
- **Rationale:**
  Earlier scorecard guess said “mitigated / weak.” That undersells the
  checker: reference-capture mutation in spawn closures is already
  ill-formed (`closure_ref_capture_mutation_fail.ccs`). Safe wrappers
  are the idiomatic share path; `@unsafe` is the explicit hatch — same
  shape as Rust’s safe vs `unsafe` split for this rule.

  Still not a full Rust `Send`/`Sync` lattice: capturing `int* p = &x`
  by value and writing `*p` is a remaining smuggle (like raw `free`
  beside unique slices). Score the *named* claim-A shape (shared ref
  mut across spawn) as **prevented**; pointer smuggling is the gap.
- **What would strengthen further:** Pointer-alias / non-sendable type
  tracking beyond ref-capture mutation.
