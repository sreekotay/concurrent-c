# CVE locality / serdes / variant study

**Status:** living study — not a product claim.  
**Question:** which real CVEs would idiomatic Concurrent-C have prevented, mitigated, left expressible, or ruled out of scope — counting **locality**, **SERDES**, and **`@variant`** — and which misses should become language work?

## Selection axis

Corpus entries are shapes where **safe Rust claim A** closes (or would close) a
bug class that ordinary C left open. The scorecard compares Concurrent-C’s
protected surface on that axis — not how often CC wins among all CVEs.

Broaden within reach of idiomatic locality / SERDES / `@variant` seams.
Application-level policy and failures inside third-party libraries (opaque
APIs, product logic) are outside the instrument: omit them; Concurrent-C has
little normative claim there.

This folder is a feedback loop, same spirit as `real_projects/`:

- **Hits** support the thesis (ownership seams *or* truthful wire *or* data-model sums).  
- **Misses** are backlog (fuller Send lattice, FFI wrong deleter, …).  
- **N/A** is for bugs outside idiomatic language seams (crypto/authz, etc.).
  Prefer **omitting** protocol-product policy bugs (e.g. obfuscated-TE
  tokenizer smuggling) rather than keeping confusing `n/a` entries.

## Inclusion rules (pre-registered)

Include a CVE if the root cause is primarily one of:

1. Use-after-free / double-free tied to ownership or teardown order  
2. Concurrent access to memory whose lifetime ended (worker vs owner)  
3. Stack / request buffer captured into a longer-lived task  
4. Ignored or nonlocal error leaving resources half-live  
5. Cancel / shutdown races (use after stop)  
6. Wire length vs actual buffer (Heartbleed-shaped) — general serdes idiom  
7. Wrong in-memory representation that invites encode/decode or inactive-arm bugs (`@variant` / schema `one of`)

**Out of scope (omit from corpus, or mark N/A only if a control is useful):**

- Crypto breaks, authz/logic bugs, XSS/SQLi with no framing/ownership angle  
- Speculative execution / side channels  
- HTTP tokenizer / obfuscated-TE smuggling (HAProxy, Netty) — not a
  framing-representation question; omit. Dual-header CL+TE
  (CVE-2005-2088) is in as T10, off the Rust axis.

When in doubt between `still_expressible` and omit/`n/a`: prefer
`still_expressible` + `needs_language:` only if a **language or general serdes**
rule could close it; omit if only a full protocol product could.

## Verdict rubric

Exactly one primary verdict per CVE:

| Verdict | Meaning |
|---------|---------|
| `prevented` | On the protected surface, the buggy shape is ill-formed (compile / type / schema rule). Off-surface escapes belong in Gap (like Rust `unsafe`), not a downgrade — see SHAPE-T8 / CVE-2014-0160. |
| `mitigated` | Idiomatic locality **or** idiomatic serdes/variant use dissolves the bug class; the C-shaped rewrite is still well-formed (no rule yet). |
| `still_expressible` | A competent CC port can still ship the same bug. |
| `n/a` | Outside idiomatic language seams (crypto/authz, or protocol-product policy). |

Optional tags:

- `needs_language:` (e.g. `channel-stable-borrow`) — CC backlog that could close the miss  
- `parity:` — when set to `rust_unsafe`, safe Rust does not close it either (`unsafe` /
  `from_raw` / custom `Drop`). Not a claim-A gap; keep primary `still_expressible`.

In `verdict.md`, name which family drove the score: `locality` | `serdes` | `variant` | `mixed`.

## How to add a CVE (or shape exemplar)

```bash
cp -R studies/cve_locality/corpus/_template \
      studies/cve_locality/corpus/CVE-YYYY-NNNNN
# Textbook class with no clean single CVE:
cp -R studies/cve_locality/corpus/_template \
      studies/cve_locality/corpus/SHAPE-<short-name>
```

Fill every file in the template. Keep reconstructions **shape-minimal**.
In `shape.md`, when comparing to Rust, use **claim A** only (same shape
ill-formed in safe Rust) — not rewrite-luck. Then update `summary.md`
from `verdict.md` files only.

## Layout

```text
studies/cve_locality/
  README.md
  taxonomy.md               # T1–T10 including serdes + variant
  summary.md                # tallies + full scorecard table
  claim-a.md                # short Rust claim-A narrative
  corpus/
    _template/
    CVE-…/
    SHAPE-…/              # optional class exemplar (e.g. T2 stack escape)
```

See [claim-a.md](claim-a.md) for the claim-A headline without the full table.


## Relation to real projects

`real_projects/pigz` and `real_projects/redis` prove idiomatic style under load
(redis already exercises `@grammar` RESP). This study asks whether that style —
including SERDES and (draft) variants — would have blocked historical failures.

## Intentionally include misses

A corpus that only contains `mitigated`/`prevented` is marketing. Prefer a
mix of locality hits, serdes mitigations, **still_expressible** backlog, and
true `n/a` controls (crypto/authz) when useful. Misses feed language work the
same way redis feeds `@variant` / stabilize.

Compile-time + runtime detectors are allowed evidence — they do not inflate
`prevented` without a language or schema rule.
