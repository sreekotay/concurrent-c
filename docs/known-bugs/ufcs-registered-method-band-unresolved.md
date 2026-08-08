# UFCS: registered-type method calls unresolved in one band of a large TU

> **Legacy-front bug writeup** (visitor reparse blanking). Default `ccc` is
> serdes; kept for archaeology.

**Status:** FIXED (root cause was not UFCS).  **Found:** 2026-07-21, redis
write-family refactor.  **Fixed:** 2026-07-21.

## Symptom

In `real_projects/redis/redis_idiomatic.ccs`, method spellings on a
registered type (`RedisConn*`) lowered correctly at some sites and were
left as raw C member accesses at others — the host compiler then failed
with `'RedisConn' has no member named 'flush_out'`.  The split was
POSITIONAL: the same method on the same receiver name/type failed in one
band of the file and lowered fine elsewhere.

## Root cause

`cc__sanitize_generated_unwrap_handlers_for_reparse` (visit_codegen.c)
blanks lowered `!>` handler bodies before feeding a buffer to a reparse.
The blanking loop is newline-preserving, but the ` (void)0;` placeholder
stamp was a fixed `memcpy` at `{`+1.  Multi-line handlers open with
`{\n`, so the stamp overwrote that newline — the reparse input shrank by
one line per multi-line handler.

TCC recorder lines from that reparse then ran N low versus the edit
buffer.  Where the exact-offset path was unavailable, the line-keyed
UFCS fallback probed the wrong physical line, found no span, and
silently skipped the node — the method call survived to the emitted C as
a raw member access.  The "band" was exactly the region where eaten
newlines had accumulated between the last `#line` anchor and the site.

## Fix (all in the same change)

- All placeholder stamps in the reparse sanitizers go through
  `cc__cg_stamp_pos()`, which finds a newline-free window — a multi-line
  handler keeps its line count.  Same hardening applied to the
  statement-unwrap sanitizer's `return 0` / `= {0}` / `= 0` stamps.
- `cc__check_sanitize_line_parity()` wraps every reparse-sanitizer call
  site: any line-count change is now a fatal internal error at compile
  time instead of a silent coordinate skew.
- The UFCS collect loop's no-span skip is a loud warning always, fatal
  under `CC_STRICT_OFFSETS=1` (CI runs with it).
- Pinned by `scripts/test_reparse_sanitize.sh` (asserts on the reparse
  dump of `tests/ufcs_below_multiline_unwrap_smoke.ccs` that multi-line
  handlers keep their `{`-adjacent newline) plus that smoke test.

## Lesson

The failure needed a large TU: small TUs lower UFCS on the initial AST
with exact offsets, so the skewed-reparse path never fires.  Line-count
neutrality of reparse rewrites is now enforced by the compiler itself
rather than by convention.
