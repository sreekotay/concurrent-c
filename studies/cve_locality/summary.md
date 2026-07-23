# Summary

Update counts only from `corpus/*/verdict.md` primary fields.
Do not invent tallies from memory. Optional `parity: rust_unsafe` tags
mark still_expressible entries that safe Rust also leaves open.

| Verdict | Count | Entries |
|---------|------:|---------|
| prevented | 13 | CVE-2017-13245, CVE-2014-0160, CVE-2025-31115, CVE-2008-5038, CVE-2020-12387, CVE-2013-4153, CVE-2026-10653, SHAPE-T2-stack-escape, SHAPE-T8-use-after-move, SHAPE-T7-shared-mut-spawn, SHAPE-T10-inactive-arm, SHAPE-T3-channel-borrow-send, SHAPE-T5-ignored-result |
| mitigated | 2 | CVE-2024-38561, CVE-2025-39945 |
| still_expressible | 2 | SHAPE-T8-adopt-wrong-deleter (`parity: rust_unsafe`), SHAPE-T3-nonarena-borrow-send |
| n/a | 0 | — |
| **total** | **17** | |

Of which still_expressible with `parity: rust_unsafe`: **1** (not a claim-A backlog item).
Claim-A still_expressible (CC miss): **1** (SHAPE-T3-nonarena-borrow-send).

## Rust claim-A scorecard

Safe Rust would reject the buggy shape. How does idiomatic / enforced CC compare?

Score the **protected surface** (schema / unique / nursery / arena pin). Off-surface
escapes (raw `free`, hand `memcpy`, `@unsafe`) go in Gap — same pattern as Rust
`unsafe` vs safe idioms.

| Entry | Rust claim A | CC | Parity | Gap |
|-------|--------------|-----|--------|-----|
| SHAPE-T2-stack-escape | ill-formed | **prevented** | — | — |
| SHAPE-T8-use-after-move | ill-formed (move) | **prevented** (unique `T[:!]`) | — | raw `malloc`/`free` still open |
| SHAPE-T7-shared-mut-spawn | `&mut` / non-`Sync` across threads | **prevented** (ref-capture mut + pointer-alias) | — | `@unsafe`; heap `T*` still value-capturable |
| SHAPE-T3-channel-borrow-send | short-lived `&[u8]` in message | **prevented** (channel-stable-borrow) | — | see non-arena residual |
| SHAPE-T3-nonarena-borrow-send | short-lived `&[u8]` in message | **still_expressible** | — | untracked / `from_buffer` send |
| SHAPE-T5-ignored-result | unused `Result` (`must_use`) | **prevented** (`T!>(E)` + strict unwrap) | — | default-off phase-1; `(void)`; C `int` APIs |
| CVE-2017-13245 | borrow vs realloc | **prevented** (epoch pin / reset ban) | — | channel send of unique/static only |
| CVE-2014-0160 | bounds / checked slice | **prevented** (serdes `bytes len`) | — | hand `memcpy(wire_len)` |
| CVE-2008-5038 | no shared mut free | **prevented** (child-free ban + pin) | — | untracked heap dual-free |
| CVE-2020-12387 | join before drop | **prevented** (destroy/detach under pin) | — | untracked / `@unsafe` |
| CVE-2025-31115 | owner-only free | **prevented** (owned-buffer-child-free-ban) | — | untracked `malloc`+`free` |
| CVE-2013-4153 | Drop once | **prevented** (arena tree + child-free ban) | — | malloc tree without provenance |
| CVE-2026-10653 | `Arc` or don’t share | **prevented** (`CCArc` + homemade last-drop ban; redis drain idiom) | — | `@unsafe` / untracked `malloc` folklore |
| CVE-2024-38561 | join/cancel ownership | **mitigated** (nursery wait-before-free; cooperative deadline) | — | raw pthread / stop-outside-nursery |
| CVE-2025-39945 | sync cancel before free | **mitigated** (nursery cancel+join) | — | external workqueues / async cancel folklore |
| SHAPE-T8-adopt-wrong-deleter | n/a (`unsafe` / `from_raw`) | **still_expressible** | `rust_unsafe` | deleter↔allocator trusted |
| SHAPE-T10-inactive-arm | enum match / inactive ban | **prevented** (protected projection + raw `.u` ban) | — | `@unsafe`; schema wire `.u` |

**Reading:** Both CC and Rust win by staying on their surface. **Prevented** = surface plus an enforceable rule. **Mitigated** = idiomatic structure dissolves the class without ill-formedness. Claim-A miss: non-arena channel borrow send. Rust-parity miss: FFI wrong deleter.

## Language backlog

| needs_language slug | From | Note |
|---------------------|------|------|
| `channel-stable-borrow-nonarena` | SHAPE-T3-nonarena-borrow-send | Ban send of untracked / `from_buffer` non-unique views |
| Full non-`Send` type lattice | T7 / T3 | partial (arena channel-stable + pointer-alias + `CCArc`) |
| Default-on strict Result unwrap | T5 | phase-1 env-gated (`CC_STRICT_RESULT_UNWRAP`) |
| Nursery-only task stop/join | CVE-2024-38561 / CVE-2025-39945 | would flip T6 toward prevented |

Shipped from this study (no longer backlog): `enforce-arena-provenance`,
`channel-stable-borrow` (arena), `pointer-alias-as-ref-capture`,
`owned-buffer-child-free-ban`, destroy/detach under arena epoch pin,
`variant-raw-u-ban`, `atomic-shared-owner` (`CCArc`).

## Next on the Rust axis

| Direction | Expected CC |
|-----------|-------------|
| Implement `channel-stable-borrow-nonarena` | would flip SHAPE-T3 residual → prevented |
| Default-on strict Result unwrap | harden SHAPE-T5 Gap |
| Nursery-only task stop | flip T6 mitigated pair → prevented |
