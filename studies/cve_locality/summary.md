# Summary

Update counts only from `corpus/*/verdict.md` primary fields.
Do not invent tallies from memory. Optional `parity: rust_unsafe` tags
mark still_expressible entries that safe Rust also leaves open.

| Verdict | Count | Entries |
|---------|------:|---------|
| prevented | 16 | CVE-2017-13245, CVE-2014-0160, CVE-2025-31115, CVE-2008-5038, CVE-2020-12387, CVE-2013-4153, CVE-2026-10653, CVE-2021-22945, SHAPE-T2-stack-escape, SHAPE-T8-use-after-move, SHAPE-T7-shared-mut-spawn, SHAPE-T10-inactive-arm, SHAPE-T3-channel-borrow-send, SHAPE-T3-nonarena-borrow-send, SHAPE-T5-ignored-result, SHAPE-T7-pointer-channel-send |
| mitigated | 4 | CVE-2024-38561, CVE-2025-39945, CVE-2016-5180, SHAPE-integer-overflow |
| still_expressible | 3 | SHAPE-T8-adopt-wrong-deleter (`parity: rust_unsafe`), SHAPE-T9-raw-index-oob, SHAPE-T7-bare-pointer-channel |
| n/a | 0 | — |
| **total** | **23** | |

Of which still_expressible with `parity: rust_unsafe`: **1** (not a claim-A backlog item).
Claim-A still_expressible (CC miss): **2** (raw index OOB write; bare `T*` channel send).

## Rust claim-A scorecard

Safe Rust would reject the buggy shape. How does idiomatic / enforced CC compare?

Score the **protected surface** (schema / unique / nursery / arena pin). Off-surface
escapes (raw `free`, hand `memcpy`, `@unsafe`) go in Gap — same pattern as Rust
`unsafe` vs safe idioms.

| Entry | Rust claim A | CC | Parity | Gap |
|-------|--------------|-----|--------|-----|
| SHAPE-T2-stack-escape | ill-formed | **prevented** | — | — |
| SHAPE-T8-use-after-move | ill-formed (move) | **prevented** (unique `T[:!]`) | — | raw `malloc`/`free` still open |
| CVE-2021-22945 | move/`Drop` once | **prevented** (unique move-once) | — | untracked `malloc` folklore |
| SHAPE-T7-shared-mut-spawn | `&mut` / non-`Sync` across threads | **prevented** (ref-capture mut + pointer-alias) | — | bare `T*` value-capture residual |
| SHAPE-T7-pointer-channel-send | non-`Send` / short-lived ref in message | **prevented** (pointer-channel-send-ban) | — | see bare-pointer residual |
| SHAPE-T7-bare-pointer-channel | non-`Send` raw ownership in message | **still_expressible** | — | bare `T*` handle send |
| SHAPE-T3-channel-borrow-send | short-lived `&[u8]` in message | **prevented** (channel-stable-borrow) | — | — |
| SHAPE-T3-nonarena-borrow-send | short-lived `&[u8]` in message | **prevented** (non-static non-unique ban) | — | — |
| SHAPE-T5-ignored-result | unused `Result` (`must_use`) | **prevented** (`T!>(E)` + strict unwrap) | — | default-off phase-1; `(void)`; C `int` APIs |
| SHAPE-integer-overflow | `checked_*` / debug overflow | **mitigated** (`cc_*_i64_checked`) | — | bare `+` still wraps |
| CVE-2016-5180 | bounds + checked size | **mitigated** (checked size + in-range write) | — | raw `ptr[i]=` (T9 residual) |
| SHAPE-T9-raw-index-oob | slice index panic / `get_mut` | **still_expressible** | — | `ptr[i]=` past `len` |
| CVE-2017-13245 | borrow vs realloc | **prevented** (epoch pin / reset ban) | — | — |
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

**Reading:** Claim-A misses reopened on purpose: raw OOB write and bare `T*`
channel handles. Integer overflow and c-ares length bugs are mitigated via
checked APIs, not language bans. Rust-parity miss: FFI wrong deleter.

## Language backlog

| needs_language slug | From | Note |
|---------------------|------|------|
| `bare-pointer-channel-send-ban` | SHAPE-T7-bare-pointer-channel | Ban non-branded `T*` send; keep redis handle exceptions |
| `checked-index-write` | SHAPE-T9-raw-index-oob / CVE-2016-5180 | Ban or Result-ize OOB `ptr[i]=` |
| `default-checked-arith` | SHAPE-integer-overflow | optional; bare `+` still wraps |
| Full non-`Send` type lattice | T7 | partial; bare `T*` open |
| Default-on strict Result unwrap | T5 | phase-1 env-gated (`CC_STRICT_RESULT_UNWRAP`) |
| Nursery-only task stop/join | CVE-2024-38561 / CVE-2025-39945 | would flip T6 toward prevented |

Shipped from this study (no longer backlog): `enforce-arena-provenance`,
`channel-stable-borrow` (arena + non-arena / untracked), `pointer-alias-as-ref-capture`,
`owned-buffer-child-free-ban`, destroy/detach under arena epoch pin,
`variant-raw-u-ban`, `atomic-shared-owner` (`CCArc`), `pointer-channel-send-ban`.

## Next on the Rust axis

| Direction | Expected CC |
|-----------|-------------|
| Implement `bare-pointer-channel-send-ban` | flip T7 residual → prevented (careful w/ redis) |
| Implement `checked-index-write` | flip T9 residual → prevented |
| Default-on checked arith / strict Result | harden mitigated Gaps |
| Nursery-only task stop | flip T6 mitigated pair → prevented |
