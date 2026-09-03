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
| Corpus entries | 28 |
| **prevented** (protected surface blocks the shape) | 21 |
| **mitigated** (idiom dissolves it; C-shaped rewrite still compiles) | 4 |
| **mitigated** off-axis (Rust does not close the shape) | 1 |
| **still_expressible** claim-A miss | 1 |
| **still_expressible** `parity: rust_unsafe` | 1 |

**Claim-A miss:** bare `T*` as a channel payload
([SHAPE-T7-bare-pointer-channel](corpus/SHAPE-T7-bare-pointer-channel/)).
Left open on purpose: redis-style pool handles are protocol lifetime, not
message ownership. Closing it needs an allowlist/brand we are not taking.

**Closed this round:** writable slice `.len`
([CVE-2015-7547](corpus/CVE-2015-7547/)) — unnamed `@typeview` on the
slice family denies `.len` / `.ptr` stores at ordinary sites
(`slice-len-write-ban`). Grow returns a new `char[:]`; the desync
assignment is ill-formed.

**Rust parity miss (not claim A):** wrong deleter on `cc_adopt` /
`from_raw` ([SHAPE-T8-adopt-wrong-deleter](corpus/SHAPE-T8-adopt-wrong-deleter/)).

## Where CC matches claim A (prevented)

Representative wins — full table in `summary.md`:

- **Unique / move-once** — use-after-move, curl-style keep-pointer-after-free
  (SHAPE-T8, CVE-2021-22945).
- **Nursery + arena epoch** — teardown vs in-flight, borrow vs reset,
  cancel/stop join-before-free (CVE-2025-31115, CVE-2017-13245,
  CVE-2020-12387, CVE-2024-38561, CVE-2025-39945, …).
- **Channel-stable borrow** — non-unique / untracked slice send (SHAPE-T3-*).
- **Pointer-field channel ban** — by-value struct with raw `T*` field
  (SHAPE-T7-pointer-channel-send).
- **Shared mut across spawn** — ref-capture mutation (SHAPE-T7-shared-mut-spawn).
- **SERDES length** — Heartbleed-shaped over-length (CVE-2014-0160).
- **Variant inactive arm** — raw `.u` / wrong projection, and the missing
  arm itself: a non-exhaustive subject-switch names the arm OpenSSL forgot
  (SHAPE-T10, CVE-2015-0286).
- **Must-consume Result** — ignored `T!>(E)`, now default-on; a refused
  privilege drop cannot fall through to the serve path
  (SHAPE-T5, CVE-2013-4559).

## Where CC is structure-only (mitigated)

Idiom works; a CC spelling of the bug still compiles:

- Integer overflow into alloc size — `cc_*_i64_checked` vs bare `+`
  (SHAPE-integer-overflow).
- Length under-count / OOB write — checked size + `at`/`set` vs
  `as_ptr()[i]=` (CVE-2016-5180, SHAPE-T9-raw-index-oob).
- Task state in a frame the task outlives — nursery join replaces the
  hand-rolled completion flag, but escaping-frame analysis covers stack
  slices and not `&`-captured scalars (CVE-2023-54235). That is a real
  CC spelling (outer nursery + `&` to inner-frame state), not a pthread Gap.

**Off the Rust axis:** dual-header HTTP CL+TE
([CVE-2005-2088](corpus/CVE-2005-2088/)) — `@variant Framing` plus
must-handle `!>` makes reject the natural path; overwriting the arm
(TE-wins / CL-wins) still compiles and is the CVE. Safe Rust does not
reject two `Option`s plus a precedence pick. Tokenizer / obfuscated-TE
smuggling stays out of corpus.

## Reading

Both languages win by staying on their surface. Concurrent-C’s forced seams
(unique, nursery join, channel-stable borrow, Result, schema `bytes len`,
checked index, slice len/ptr store ban, exhaustive variant switch) cover
most claim-A shapes in this corpus. Remaining honesty:

1. Bare pointer **handles** on channels — protocol, not Send lattice.
2. `as_ptr()` / `malloc` — Gap, like `unsafe`.
3. T2 frame escape — tracked for stack slices, not for `&`-captured scalars
   (CVE-2023-54235).

Do not treat this folder as a product guarantee. It is a language instrument:
hits validate seams; misses feed backlog or conscious Gaps.
