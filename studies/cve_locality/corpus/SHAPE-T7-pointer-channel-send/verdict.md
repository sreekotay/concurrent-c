# Verdict

- **Primary:** `prevented`
- **Family:** `locality`
- **needs_language:** —
- **Rationale:**
  By-value channel send of an aggregate with a raw pointer field is
  ill-formed (`pointer-channel-send-ban`;
  `tests/channel_send_pointer_field_fail.ccs`). Idiomatic path sends
  owned / static slice bytes instead. Bare `T*` handle payloads remain
  outside this rule (redis `RedisRequest*` join protocol).
- **What would change the verdict:** Broader non-`Send` lattice that also
  bans bare non-branded `T*` would shrink Gap further; not required for
  this shape's claim-A miss.
