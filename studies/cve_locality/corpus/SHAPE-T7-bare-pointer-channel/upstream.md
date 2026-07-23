# SHAPE-T7 — bare `T*` channel send (residual after pointer-field ban)

- **Links:** Residual of SHAPE-T7-pointer-channel-send after
  `pointer-channel-send-ban` (by-value aggregates with raw pointer fields).
- **Product / versions:** (n/a — shape study)
- **CWE(s):** CWE-416
- **One paragraph:** Sending `char*` / `int*` / `Foo*` by value on a
  channel still compiles (handle protocols such as redis
  `RedisRequest*`). The pointee can be freed while the receiver holds
  the pointer. Safe Rust’s non-`Send` / ownership rules reject the
  dangling case; Concurrent-C has not banned bare pointer payloads.
