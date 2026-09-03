# Summary

Update counts only from `corpus/*/verdict.md` primary fields.
Do not invent tallies from memory. Optional `parity: rust_unsafe` tags
mark still_expressible entries that safe Rust also leaves open.

| Verdict | Count | Entries |
|---------|------:|---------|
| prevented | 21 | CVE-2017-13245, CVE-2014-0160, CVE-2025-31115, CVE-2008-5038, CVE-2020-12387, CVE-2013-4153, CVE-2026-10653, CVE-2021-22945, CVE-2015-0286, CVE-2013-4559, CVE-2015-7547, CVE-2024-38561, CVE-2025-39945, SHAPE-T2-stack-escape, SHAPE-T8-use-after-move, SHAPE-T7-shared-mut-spawn, SHAPE-T10-inactive-arm, SHAPE-T3-channel-borrow-send, SHAPE-T3-nonarena-borrow-send, SHAPE-T5-ignored-result, SHAPE-T7-pointer-channel-send |
| mitigated | 5 | CVE-2016-5180, CVE-2023-54235, SHAPE-integer-overflow, SHAPE-T9-raw-index-oob, CVE-2005-2088 (off-axis) |
| still_expressible | 2 | SHAPE-T8-adopt-wrong-deleter (`parity: rust_unsafe`), SHAPE-T7-bare-pointer-channel |
| n/a | 0 | — |
| **total** | **28** | |

Of which still_expressible with `parity: rust_unsafe`: **1** (not a claim-A backlog item).
Claim-A still_expressible (CC miss): **1** (bare `T*` channel send).

Of the five mitigated, **4** are on the Rust claim-A axis and **1**
(CVE-2005-2088) is off-axis: safe Rust does not close dual-header CL+TE.

Corpus health: all **28** `idiomatic.ccs` demos build and run
(`scripts/test_cve_locality.sh`, wired into `scripts/test.sh`).

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
| SHAPE-T5-ignored-result | unused `Result` (`must_use`) | **prevented** (`T!>(E)` + strict unwrap, default-on) | — | `(void)`; C `int` APIs |
| CVE-2013-4559 | `unsafe` for raw `setuid`; must-use wrapper | **prevented** (fallible binding + strict unwrap) | — | `int`-returning bindings; `(void)` |
| SHAPE-integer-overflow | `checked_*` / debug overflow | **mitigated** (`cc_*_i64_checked`) | — | bare `+` still wraps |
| CVE-2016-5180 | bounds + checked size | **mitigated** (checked size + `at`/`set`) | — | raw `ptr[i]=` Gap |
| SHAPE-T9-raw-index-oob | slice index panic / `get_mut` | **mitigated** (`at`/`set` Result, all builds) | — | raw `.ptr[i]=` |
| CVE-2015-7547 | fat `&mut [u8]`; no writable len | **prevented** (slice-len-write-ban / unnamed `@typeview` on CCSlice) | — | first-arg trusted bodies; `@unsafe` |
| CVE-2017-13245 | borrow vs realloc | **prevented** (epoch pin / reset ban) | — | — |
| CVE-2014-0160 | bounds / checked slice | **prevented** (serdes `bytes len`) | — | hand `memcpy(wire_len)` |
| CVE-2008-5038 | no shared mut free | **prevented** (child-free ban + pin) | — | untracked heap dual-free |
| CVE-2020-12387 | join before drop | **prevented** (destroy/detach under pin) | — | untracked / `@unsafe` |
| CVE-2025-31115 | owner-only free | **prevented** (owned-buffer-child-free-ban) | — | untracked `malloc`+`free` |
| CVE-2013-4153 | Drop once | **prevented** (arena tree + child-free ban) | — | malloc tree without provenance |
| CVE-2026-10653 | `Arc` or don’t share | **prevented** (`CCArc` + homemade last-drop ban; redis drain idiom) | — | `@unsafe` / untracked `malloc` folklore |
| CVE-2024-38561 | join/cancel ownership | **prevented** (nursery owns join; cancel cannot skip wait) | — | raw pthread / `kthread_stop` |
| CVE-2025-39945 | sync cancel before free | **prevented** (nursery cancel+join; no fire-and-forget free) | — | external workqueues |
| CVE-2023-54235 | `thread::scope` joins before borrow ends | **mitigated** (nursery join is the protocol) | — | outer-scope nursery + `&`-captured scalar |
| SHAPE-T8-adopt-wrong-deleter | n/a (`unsafe` / `from_raw`) | **still_expressible** | `rust_unsafe` | deleter↔allocator trusted |
| SHAPE-T10-inactive-arm | enum match / inactive ban | **prevented** (protected projection + raw `.u` ban; schema `one of` shares the surface) | — | `@unsafe`; compound-literal `.kind`/`.u` interop |
| CVE-2015-0286 | non-exhaustive `match` | **prevented** (exhaustive subject-switch + domination) | — | `@unsafe`; compound-literal `.kind`/`.u` interop |

Off the Rust axis (not in the claim-A counts above):

| Entry | Rust claim A | CC | Parity | Gap |
|-------|--------------|-----|--------|-----|
| CVE-2005-2088 | does not close (two `Option`s + precedence) | **mitigated** (`Framing!>(E)` rejects; overwrite still compiles) | — | lenient peer / tokenizer TE |

**Reading:** Checked index, default-on strict Result, and slice `.len`/`.ptr`
store ban shipped. Claim-A miss left: bare `T*` channel handles.
Rust-parity miss: FFI wrong deleter.

## Language backlog

| needs_language slug | From | Note |
|---------------------|------|------|
| `bare-pointer-channel-send-ban` | SHAPE-T7-bare-pointer-channel | Ban non-branded `T*` send; keep redis handle exceptions |
| Full non-`Send` type lattice | T7 | partial; bare `T*` open |
| `escaping-ref-capture-frame-check` | CVE-2023-54235 | Extend escaping-closure frame analysis from stack slices to `&`-captured locals |
| `default-checked-arith` | SHAPE-integer-overflow | optional; bare `+` still wraps |

Implemented rules: `enforce-arena-provenance`,
`channel-stable-borrow` (arena + non-arena / untracked), `pointer-alias-as-ref-capture`,
`owned-buffer-child-free-ban`, destroy/detach under arena epoch pin,
nursery join-before-free (T6 cancel/stop on the nursery surface),
`variant-raw-u-ban`, `atomic-shared-owner` (`CCArc`), `pointer-channel-send-ban`,
`checked-index-write` (Result `at`/`set`, all builds), default-on strict Result
unwrap (opt out with `CC_STRICT_RESULT_UNWRAP=0`),
`slice-len-write-ban` (unnamed `@typeview` on slice family).

## Next on the Rust axis

| Direction | Expected CC |
|-----------|-------------|
| Implement `bare-pointer-channel-send-ban` | flip T7 residual → prevented (careful w/ redis) |
| Implement `escaping-ref-capture-frame-check` | flip T2 scalar residual → prevented (CVE-2023-54235) |
| Default-on checked arith | harden the remaining arithmetic Gap |
