# SHAPE — integer overflow → undersized buffer (realpath-class)

- **Links:** CVE-2018-11236 (glibc `realpath`) as exemplar; class is
  broader than one patch
- **Product / versions:** (n/a — shape study)
- **CWE(s):** CWE-190, CWE-787
- **One paragraph:** Size arithmetic wraps (`n + len` near `SIZE_MAX`),
  the allocation is too small, and a later copy/write treats the wrapped
  sum as honest. Safe Rust makes wrapping explicit
  (`wrapping_add` / `checked_add`); silent wrap-into-alloc is not the
  default happy path.
