# SHAPE-T9 — raw index OOB write (Rust claim-A miss)

- **Links:** Class residual after schema `bytes len` (CVE-2014-0160) and
  soft `CCSlice.at` (returns 0 on OOB read). No single CVE required.
- **Product / versions:** (n/a — shape study)
- **CWE(s):** CWE-787
- **One paragraph:** Safe Rust rejects or panics on out-of-bounds slice
  indexing. Concurrent-C’s schema path prevents over-length parses, and
  `s.at(i)` soft-fails on read, but `s.ptr[i] = …` with `i >= s.len` is
  still well-formed C. That is the write-side claim-A gap.
