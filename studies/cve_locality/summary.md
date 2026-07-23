# Summary

Update counts only from `corpus/*/verdict.md` primary fields.
Do not invent tallies from memory. Optional `parity: rust_unsafe` tags
mark still_expressible entries that safe Rust also leaves open.

| Verdict | Count | Entries |
|---------|------:|---------|
| prevented | 5 | CVE-2017-13245, CVE-2014-0160, SHAPE-T2-stack-escape, SHAPE-T8-use-after-move, SHAPE-T7-shared-mut-spawn |
| mitigated | 5 | CVE-2025-31115, CVE-2008-5038, CVE-2020-12387, CVE-2013-4153, CVE-2026-10653 |
| still_expressible | 1 | SHAPE-T8-adopt-wrong-deleter (`parity: rust_unsafe`) |
| n/a | 0 | — |
| **total** | **11** | |

Of which still_expressible with `parity: rust_unsafe`: **1** (not a claim-A backlog item).

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
| CVE-2017-13245 | borrow vs realloc | **prevented** (epoch pin / reset ban) | — | channel send of unique/static only |
| CVE-2014-0160 | bounds / checked slice | **prevented** (serdes `bytes len`) | — | hand `memcpy(wire_len)` |
| CVE-2008-5038 | no shared mut free | **mitigated** | — | raw alias+`free` |
| CVE-2020-12387 | join before drop | **mitigated** | — | detach / raw |
| CVE-2025-31115 | owner-only free | **mitigated** | — | child `free` |
| CVE-2013-4153 | Drop once | **mitigated** (arena tree) | — | raw malloc parent+child `free` |
| CVE-2026-10653 | `Arc` or don’t share | **mitigated** (redis: closures+drain) | — | homemade `ref--` still compiles |
| SHAPE-T8-adopt-wrong-deleter | n/a (`unsafe` / `from_raw`) | **still_expressible** | `rust_unsafe` | deleter↔allocator trusted |

**Reading:** Both CC and Rust win by staying on their surface. **Prevented** = surface plus an enforceable rule (checker / schema). **Mitigated** = surface dissolves the bug class (arenas, join/drain) without yet making the C-shaped rewrite ill-formed. The only `still_expressible` entry is FFI wrong deleter — Rust parity, not a locality backlog miss.

## Language backlog

| needs_language slug | From | Note |
|---------------------|------|------|
| owned-buffer-child-free-ban | xz, CVE-2008-5038, CVE-2013-4153 | Toward Rust’s alias-free / non-freeable borrow bar |
| (stdlib) atomic shared owner | CVE-2026-10653 | Optional; redis path is “don’t share” |

Shipped from this study (no longer backlog): `enforce-arena-provenance`,
`channel-stable-borrow`, `pointer-alias-as-ref-capture`.

## Next on the Rust axis

| Direction | Expected CC |
|-----------|-------------|
| `@variant` inactive arm (T10) | TBD when exercised (redis `@variant RedisValue` candidate) |
| Full non-`Send` type lattice | partial (pointer-alias closes local-stack smuggle) |
| Detach / free while borrow live | would strengthen CVE-2020-12387 toward prevented |
