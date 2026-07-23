# SHAPE-T8 — FFI adopt with wrong deleter

- **Links:** Spec §10.2 / Appendix A.2 (`adopt::[T]`); Rust analogue
  `Box::from_raw` / custom `Drop` lying about allocator
- **Product / versions:** Language shape (not a single CVE). Common in
  C FFI: `malloc`/`free` vs `mmap`/`munmap`, pool free vs `free`,
  `LocalFree` vs `HeapFree`, etc.
- **CWE(s):** CWE-762 (mismatched memory management), CWE-415/416
- **One paragraph:** Concurrent-C’s intended FFI seam mints a unique
  slice from a foreign pointer plus a user-supplied deleter
  (`adopt(ptr, count, deleter)`). The language trusts that deleter the
  way safe Rust trusts `unsafe` when constructing an owned value from a
  raw pointer. Passing the wrong free function, or also calling free by
  hand beside `@destroy`/deleter, is not a type error — it is a
  programmer trust failure at the FFI boundary.
