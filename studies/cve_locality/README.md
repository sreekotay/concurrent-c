# CVE locality / serdes / variant study

**Status:** living study — not a product claim.  
**Question:** which real CVEs would idiomatic Concurrent-C have prevented, mitigated, left expressible, or ruled out of scope — counting **locality**, **SERDES**, and **`@variant`** — and which misses should become language work?

This folder is a feedback loop, same spirit as `real_projects/`:

- **Hits** support the thesis (ownership seams *or* truthful wire *or* data-model sums).  
- **Misses** are backlog (`channel-stable-borrow`, schema rejects, shipped `@variant`, …).  
- **N/A** is for bugs with no ownership / wire-framing / representation angle.

## Inclusion rules (pre-registered)

Include a CVE if the root cause is primarily one of:

1. Use-after-free / double-free tied to ownership or teardown order  
2. Concurrent access to memory whose lifetime ended (worker vs owner)  
3. Stack / request buffer captured into a longer-lived task  
4. Ignored or nonlocal error leaving resources half-live  
5. Cancel / shutdown races (use after stop)  
6. Wire/buffer length or framing abuse (Heartbleed-shaped, HTTP smuggling-shaped)  
7. Wrong in-memory representation that invites encode/decode or inactive-arm bugs (`@variant` / schema `one of`)

**Out of scope (mark N/A):**

- Crypto breaks, authz/logic bugs, XSS/SQLi with no framing/ownership angle  
- Speculative execution / side channels  
- “Would need a full product HTTP stack” alone is not enough for `n/a` — score the **shape** against `@grammar` / schema rejects even if no HTTP library ships yet  

When in doubt between `still_expressible` and `n/a`: prefer `still_expressible` + `needs_language:` if SERDES or variants *could* close it with a concrete rule.

## Verdict rubric

Exactly one primary verdict per CVE:

| Verdict | Meaning |
|---------|---------|
| `prevented` | Idiomatic CC makes the buggy shape ill-formed (compile / type / schema rule). |
| `mitigated` | Idiomatic locality **or** idiomatic serdes/variant use makes the safe path natural; footguns remain via raw/`unsafe`/hand parsers. |
| `still_expressible` | A competent CC port can still ship the same bug. **Language / schema backlog.** |
| `n/a` | No ownership, wire-framing, or representation angle. |

Optional tags: `needs_language:` (e.g. `channel-stable-borrow`, `http-schema-reject-cl-te`).

In `verdict.md`, name which family drove the score: `locality` | `serdes` | `variant` | `mixed`.

## How to add a CVE

```bash
cp -R studies/cve_locality/corpus/_template \
      studies/cve_locality/corpus/CVE-YYYY-NNNNN
```

Fill every file in the template. Keep reconstructions **shape-minimal**.
Then update `summary.md` from `verdict.md` files only.

## Layout

```text
studies/cve_locality/
  README.md
  taxonomy.md               # T1–T10 including serdes + variant
  summary.md
  corpus/
    _template/
    CVE-…/
```

## Relation to real projects

`real_projects/pigz` and `real_projects/redis` prove idiomatic style under load
(redis already exercises `@grammar` RESP). This study asks whether that style —
including SERDES and (draft) variants — would have blocked historical failures.

## Intentionally include misses

A corpus that only contains `mitigated`/`prevented` is marketing. Prefer a
mix of locality hits, serdes mitigations, **still_expressible** backlog, and
true `n/a` controls (crypto/authz). Misses feed language work the same way
redis feeds `@variant` / stabilize.

Compile-time + runtime detectors are allowed evidence — they do not inflate
`prevented` without a language or schema rule.
