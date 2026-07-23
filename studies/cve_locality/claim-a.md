# Rust claim A — Concurrent-C scorecard

**Claim A:** safe Rust would reject the buggy ownership / concurrency /
bounds shape at compile time (or panic on checked index), without needing
`unsafe`.

This note summarizes [`summary.md`](summary.md) for that claim only.
Counts come from `corpus/*/verdict.md`. Gap means off-surface escape
(`malloc`/`free`, raw `.ptr`, `@unsafe`) — same role as Rust `unsafe`.

## Headline

| | Count |
|--|------:|
| Corpus entries | 23 |
| **prevented** (protected surface blocks the shape) | 16 |
| **mitigated** (idiom dissolves it; C-shaped rewrite still compiles) | 5 |
| **still_expressible** claim-A miss | 1 |
| **still_expressible** `parity: rust_unsafe` | 1 |

**Claim-A miss:** bare `T*` as a channel payload
([SHAPE-T7-bare-pointer-channel](corpus/SHAPE-T7-bare-pointer-channel/)).
Left open on purpose: redis-style pool handles are protocol lifetime, not
message ownership. Closing it needs an allowlist/brand we are not taking.

**Rust parity miss (not claim A):** wrong deleter on `cc_adopt` /
`from_raw` ([SHAPE-T8-adopt-wrong-deleter](corpus/SHAPE-T8-adopt-wrong-deleter/)).

## Where CC matches claim A (prevented)

Representative wins — full table in `summary.md`:

- **Unique / move-once** — use-after-move, curl-style keep-pointer-after-free
  (SHAPE-T8, CVE-2021-22945).
- **Nursery + arena epoch** — teardown vs in-flight, borrow vs reset
  (CVE-2025-31115, CVE-2017-13245, CVE-2020-12387, …).
- **Channel-stable borrow** — non-unique / untracked slice send (SHAPE-T3-*).
- **Pointer-field channel ban** — by-value struct with raw `T*` field
  (SHAPE-T7-pointer-channel-send).
- **Shared mut across spawn** — ref-capture mutation (SHAPE-T7-shared-mut-spawn).
- **SERDES length** — Heartbleed-shaped over-length (CVE-2014-0160).
- **Variant inactive arm** — raw `.u` / wrong projection (SHAPE-T10).
- **Must-consume Result** — ignored `T!>(E)` on the strict path (SHAPE-T5).

## Where CC is structure-only (mitigated)

Idiom works; escape hatch still compiles:

- Cancel / stop races outside nursery-only policy (CVE-2024-38561,
  CVE-2025-39945).
- Integer overflow into alloc size — `cc_*_i64_checked` vs bare `+`
  (SHAPE-integer-overflow).
- Length under-count / OOB write — checked size + `at`/`set` vs raw
  `ptr[i]=` (CVE-2016-5180, SHAPE-T9-raw-index-oob).

## Reading

Both languages win by staying on their surface. Concurrent-C’s forced seams
(unique, nursery join, channel-stable borrow, Result, schema `bytes len`,
checked index) cover most claim-A shapes in this corpus. Remaining honesty:

1. Bare pointer **handles** on channels — protocol, not Send lattice.
2. Raw `.ptr` / `malloc` — Gap, like `unsafe`.
3. T6 stop/join — mitigated until nursery-only stop is language policy.

Do not treat this folder as a product guarantee. It is a language instrument:
hits validate seams; misses feed backlog or conscious Gaps.
