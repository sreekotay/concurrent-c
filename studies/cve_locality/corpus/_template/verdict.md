# Verdict

- **Primary:** `prevented` | `mitigated` | `still_expressible` | `n/a`
- **Family:** `locality` | `serdes` | `variant` | `mixed`
- **needs_language:** (optional) short slug — only if `still_expressible` or weak `mitigated`
- **parity:** (optional) `rust_unsafe` — safe Rust also leaves this open (`unsafe` /
  `from_raw`); not a claim-A miss. Leave blank for CC-only backlog.
- **Rationale:** 3–8 sentences. Cite which CC seams apply (locality /
  SERDES / `@variant`) and which escape hatches remain.
- **What would change the verdict:** e.g. “channel send of borrowed slice
  becomes ill-formed”
