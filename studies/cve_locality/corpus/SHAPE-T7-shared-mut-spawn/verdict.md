# Verdict

- **Primary:** `prevented`
- **Family:** `locality`
- **needs_language:** (done) `pointer-alias-as-ref-capture`
- **Rationale:**
  Reference-capture mutation in spawn closures is ill-formed
  (`closure_ref_capture_mutation_fail.ccs`). Value-captured
  `T* p = &local` then writing `*p` / `p->` is also ill-formed
  (`closure_ptr_alias_value_capture_mutation_fail.ccs`) — the former
  smuggle path. Safe wrappers remain the idiomatic share path; `@unsafe`
  is the explicit hatch.

  Not a full Rust `Send`/`Sync` lattice: heap-backed `T*` value capture
  and other non-local aliases stay open. Score the *named* claim-A shape
  as **prevented**.
- **What would strengthen further:** Broader non-sendable type tracking
  beyond stack-local pointer aliases.
