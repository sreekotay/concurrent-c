# Verdict

- **Primary:** `still_expressible`
- **Family:** `locality`
- **parity:** `rust_unsafe`
- **needs_language:** none — proving deleter↔allocator match is out of band
  (same as Rust `unsafe` / `Box::from_raw`)
- **Rationale:**
  Shipped as `cc_adopt` + `s.destroy()` / `@destroy` → `cc_slice_destroy`.
  Unique move rules stop *aliasing* an adopted owner; they do not stop:

      CCSliceUnique s = cc_adopt(mmap_ptr, n, free) @destroy; /* wrong */

  Safe Rust has the same trust boundary (`from_raw` / lying `Drop`). This is
  Rust-parity, not a Concurrent-C claim-A gap.
- **What would change the verdict:** Typed allocator tags / paired
  alloc-free brands (research; not planned). Until then this stays
  `still_expressible` by design.
