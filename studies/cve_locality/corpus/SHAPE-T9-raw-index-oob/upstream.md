# SHAPE-T9 — raw index OOB (checked surface)

- **Links:** Class residual after schema `bytes len` (CVE-2014-0160).
  Closed on the idiomatic path by `checked-index-write` (`at` /
  `get_checked` / `set` → `CCError` on OOB).
- **Product / versions:** (n/a — shape study)
- **CWE(s):** CWE-787
- **One paragraph:** Safe Rust rejects or panics on out-of-bounds slice
  indexing. Concurrent-C’s protected index ops now return `CCError` on
  OOB in all builds. Raw `s.ptr[i]=` remains well-formed C (Gap).
