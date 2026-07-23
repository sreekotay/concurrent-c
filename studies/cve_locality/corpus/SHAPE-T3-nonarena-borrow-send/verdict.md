# Verdict

- **Primary:** `still_expressible`
- **Family:** `locality`
- **needs_language:** `channel-stable-borrow-nonarena` (extend Send lattice
  beyond arena views)
- **Rationale:**
  Arena-backed non-unique sends are already ill-formed
  (`SHAPE-T3-channel-borrow-send` / `channel_send_arena_borrow_fail`).
  The residual bug shape uses untracked / `cc_slice_from_buffer` views:

      char* p = malloc(n);
      char[:] v = cc_slice_from_buffer(p, n);
      cc_channel_send(tx, v);   /* still compiles */
      free(p);                  /* receiver may still hold v.ptr */

  Safe Rust claim A rejects that. Concurrent-C does not — intentional
  backlog for a fuller channel-stable / non-`Send` rule, not Rust
  `unsafe` parity.
- **What would change the verdict:** Ban send of any non-unique /
  non-static slice (or require unique/`send_take` / materialize) →
  toward `prevented`.
