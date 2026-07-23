# SHAPE-T8 — use-after-move / two owners of one allocation

- **Links:** Class exemplar (CWE-415 / CWE-416 adjacent). Rust: move
  invalidates the source; `Drop` runs once. CC evidence:
  `tests/slice_provenance_copy_fail.ccs`,
  `tests/slice_provenance_use_after_capture_fail.ccs`,
  `tests/slice_type_unique_copy_fail.ccs`.
- **Product / versions:** (n/a — shape study)
- **CWE(s):** CWE-415, CWE-416
- **One paragraph:** Two names both believe they own the same heap block
  (copy of a unique handle, or use after move). One drops/frees; the other
  still uses or frees again. Safe Rust makes the second use a compile
  error. Concurrent-C does the same for **unique** slices (`T[:!]` /
  unique provenance): copy without `cc_move` and use-after-move are
  rejected. Raw `malloc`/`free` aliases remain an escape hatch.
