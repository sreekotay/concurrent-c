# Verdict

- **Primary:** `still_expressible`
- **Family:** `locality`
- **needs_language:** `pointer-channel-send-ban` / fuller non-`Send`
  lattice for `T*` and structs containing pointers
- **Rationale:**
  Slice sends of non-unique views are ill-formed
  (SHAPE-T3-*). Stack-local `T*` mutation-via-alias in spawn is
  ill-formed (SHAPE-T7-shared-mut-spawn). The residual shape still
  compiles:

      typedef struct Msg { char* p; size_t n; } Msg;
      Msg m = { .p = malloc(n), .n = n };
      cc_channel_send(tx, m);   /* still compiles */
      free(m.p);                /* receiver may still hold got.p */

  Heap `T*` value-capture into spawn is similarly open. Safe Rust claim
  A rejects both. This is Concurrent-C backlog, not Rust `unsafe`
  parity.
- **What would change the verdict:** Ban channel send / escaping capture
  of types that contain raw pointers (unless branded owned / unique) →
  toward `prevented`.
