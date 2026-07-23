# Verdict

- **Primary:** `still_expressible`
- **Family:** `locality`
- **needs_language:** `bare-pointer-channel-send-ban` — ban non-branded
  `T*` payloads (keep explicit handle / branded exceptions for redis)
- **Rationale:**
  `pointer-channel-send-ban` closed by-value structs with raw pointer
  fields. Bare `T*` send remains well-formed (intentional for pool-slot
  handles). Demo compiles: send `char*`, free, receiver still holds the
  value. Safe Rust claim A rejects the dangling case. This is Concurrent-C
  backlog on the non-slice Send lattice, not `rust_unsafe` parity.
- **What would change the verdict:** Ban bare non-branded pointer sends
  (or require unique/branded handles) → toward `prevented`.
