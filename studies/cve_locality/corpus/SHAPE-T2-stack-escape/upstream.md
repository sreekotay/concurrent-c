# SHAPE-T2 — stack / frame buffer captured into a longer-lived task

- **Links:** Class exemplar (CWE-562 use-after-return / CWE-416). No single
  canonical CVE — the shape is the textbook “Rust borrow checker vs C
  pthread” case. CC evidence:
  `tests/closure_capture_stack_slice_return_fail.ccs`,
  nursery-escape probe (stack slice + return nursery → compile error).
- **Product / versions:** (n/a — shape study)
- **CWE(s):** CWE-562, CWE-416
- **One paragraph:** A stack (or frame-local) buffer is handed to a thread /
  task / escaping closure; the creating frame returns while the task still
  runs, so the task uses dead stack memory. In C this is a silent footgun
  (`pthread_create(..., &local)`). In safe Rust it does not compile. In
  Concurrent-C, capturing a stack slice into an escaping closure is a
  compile-time error.
