# Summary

Update counts only from `corpus/*/verdict.md` primary fields.
Do not invent tallies from memory.

| Verdict | Count | CVEs |
|---------|------:|------|
| prevented | 0 | — |
| mitigated | 2 | CVE-2025-31115 (locality), CVE-2014-0160 (serdes) |
| still_expressible | 2 | CVE-2017-13245 (locality), CVE-2024-23452 (serdes) |
| n/a | 0 | — |
| **total** | **4** | |

## By family

| Family | Count | CVEs |
|--------|------:|------|
| locality | 2 | CVE-2025-31115, CVE-2017-13245 |
| serdes | 2 | CVE-2014-0160, CVE-2024-23452 |
| variant | 0 | — (need a T10 specimen; redis `RedisValue` is the design driver) |

## Language backlog (from verdicts)

| needs_language slug | From | Note |
|---------------------|------|------|
| owned-buffer-child-free-ban | CVE-2025-31115 | Weak; structure mitigates, raw free still possible |
| enforce-arena-provenance / invalidate-borrows-on-arena-reset | CVE-2017-13245 | **Primary locality miss** — provenance ids + reset bump already exist; neither comptime nor use-path checks them. Same family as redis stabilize-as-rule |
| http-schema-reject-cl-te | CVE-2024-23452 | **Primary serdes miss** — ambiguous framing unrepresentable |

## Meta note (harness)

The first draft of `CVE-2025-31115/idiomatic.ccs` deadlocked (close-after-join).
`ccc` caught it at **runtime** (sysmon dump, exit 124) — detector evidence,
not a CVE prevented count.

## Next candidates (suggested)

| Direction | Why |
|-----------|-----|
| Stack pointer into fiber (T2) | Expect locality `prevented` / strong `mitigated` |
| FFI `adopt` double-free (T8) | Likely `still_expressible` |
| Ignored fallible read (T5) | Result / `@errhandler` |
| Inactive-arm / wrong tag use (T10) | Expect `@variant` mitigate once shipped; good control for draft |
| True `n/a` crypto/authz | Keep ≥1 control that is not wire/ownership |
| Another Heartbleed-twin with hand parser in idiomatic.ccs | Show serdes mitigate + raw escape side-by-side |
