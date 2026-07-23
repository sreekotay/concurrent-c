# Verdict

- **Primary:** `still_expressible`
- **Family:** `locality`
- **needs_language:** `checked-index-write` — Result-ize or ban OOB
  writes through slice pointers; optional debug trap
- **Rationale:**
  Schema `bytes len` and Heartbleed-shaped parses are prevented. Soft
  `CCSlice.at` avoids OOB *reads* by returning 0. The residual write shape
  still compiles:

      char[:] s = …;
      ((char*)s.ptr)[s.len] = 'x';   /* still well-formed */

  Safe Rust claim A rejects that. Concurrent-C backlog, not `rust_unsafe`
  parity.
- **What would change the verdict:** Checked slice write API as the only
  well-formed path (or debug/compiler ban on raw indexed stores) →
  toward `prevented` / `mitigated`.
