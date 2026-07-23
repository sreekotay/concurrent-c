# Summary

Update counts only from `corpus/*/verdict.md` primary fields.
Do not invent tallies from memory. Optional `parity: rust_unsafe` tags
mark still_expressible entries that safe Rust also leaves open.

| Verdict | Count | Entries |
|---------|------:|---------|
| prevented | 4 | CVE-2017-13245, SHAPE-T2-stack-escape, SHAPE-T8-use-after-move, SHAPE-T7-shared-mut-spawn |
| mitigated | 6 | CVE-2025-31115, CVE-2014-0160, CVE-2008-5038, CVE-2020-12387, CVE-2013-4153, CVE-2026-10653 |
| still_expressible | 1 | SHAPE-T8-adopt-wrong-deleter (`parity: rust_unsafe`) |
| n/a | 0 | — |
| **total** | **11** | |

Of which still_expressible with `parity: rust_unsafe`: **1** (not a claim-A backlog item).

## Rust claim-A scorecard

Safe Rust would reject the buggy shape. How does idiomatic / enforced CC compare?

| Entry | Rust claim A | CC | Parity | Gap |
|-------|--------------|-----|--------|-----|
| SHAPE-T2-stack-escape | ill-formed | **prevented** | — | — |
| SHAPE-T8-use-after-move | ill-formed (move) | **prevented** (unique `T[:!]`) | — | raw `malloc`/`free` still open |
| SHAPE-T7-shared-mut-spawn | `&mut` / non-`Sync` across threads | **prevented** (ref-capture mut ban) | — | value-captured `T*` alias; `@unsafe` |
| CVE-2017-13245 | borrow vs realloc | **prevented** (epoch pin / reset ban) | — | — |
| CVE-2008-5038 | no shared mut free | **mitigated** | — | raw alias+`free` |
| CVE-2020-12387 | join before drop | **mitigated** | — | detach / raw |
| CVE-2025-31115 | owner-only free | **mitigated** | — | child `free` |
| CVE-2014-0160 | (serdes / bounds) | **mitigated** | — | hand `memcpy` |
| CVE-2013-4153 | Drop once | **mitigated** (arena tree) | — | raw malloc parent+child `free` |
| CVE-2026-10653 | `Arc` or don’t share | **mitigated** (redis: closures+drain) | — | homemade `ref--` still compiles |
| SHAPE-T8-adopt-wrong-deleter | n/a (`unsafe` / `from_raw`) | **still_expressible** | `rust_unsafe` | deleter↔allocator trusted |

**Reading:** Checker-hard prevents: stack escape, unique move, shared-ref mutation across spawn, arena epoch pin. Structure mitigations (redis/arenas/join/serdes) cover the CVE bulk. The only `still_expressible` entry is FFI adopt with a wrong deleter — **Rust parity** (`parity: rust_unsafe`), not a locality backlog miss.

## Language backlog

| needs_language slug | From | Note |
|---------------------|------|------|
| owned-buffer-child-free-ban | xz, CVE-2008-5038 | Toward Rust’s alias-free bar |
| channel-stable-borrow | CVE-2017-13245 sibling | Stabilize as rule |
| pointer-alias-as-ref-capture | SHAPE-T7 gap | Value-captured `T*` smuggle |
| (stdlib) atomic shared owner | CVE-2026-10653 | Optional; redis path is “don’t share” |

## Next on the Rust axis

| Direction | Expected CC |
|-----------|-------------|
| `@variant` inactive arm (T10) | TBD when exercised |
| Full non-`Send` type lattice | weak / backlog (`pointer-alias-as-ref-capture`) |
