# Real projects

Subdirectories here are **specimens and stress tests**: Concurrent-C used the way we want production code to read, without hiding that **it is still C**.

## Principles

**Tutorial, idiomatic, production, and performance are the same bar.** A demo should not be a toy dialect: it should compile and run with the same seriousness as the rest of the tree, and it should stay honest about cost.

**Explicit beats magic.** Prefer constructs you could audit from lowered C (`--emit-c`): visible lifetimes, obvious channel payloads, errors you can branch on. Concurrency adds mechanism, not mystery.

**Scope is where “destructor-like” behavior lives.** `@defer`, `@destroy`, and related forms tie cleanup to **lexical structure**, not to hidden runtime policy. That matches the language spec: memory and task shape follow the skeleton; channels are the graph on top, with provenance kept explicit.

**Language gaps are acceptable; they are also data.** Some gaps we close in the compiler or stdlib, some we document and live with. The point is to learn from friction without turning specimens into clever workarounds that a C reader cannot follow.

## Layout pattern (pigz and Redis)

Each serious domain tends to ship **two layers**:

1. **`*_idiomatic.ccs`** — thin, readable **pipeline and vocabulary** for the subsystem (ordered tasks, ownership seams, one error story on the wire). This is the file you read first.
2. **`*/<name>_cc/` or parity binary** — **feature-complete** Concurrent-C next to upstream C: same algorithms and flags, more surface area. It should still **read like C for the domain** and like CC for structure, not like a second hidden language.

Keeping that split clear preserves headroom: the idiomatic file teaches the skeleton; the full port proves the skeleton scales.

Normative definitions of the primitives and lowering live in `spec/concurrent-c-spec-complete.md`.

**Related:** `studies/cve_locality/` reconstructs historical CVEs under idiomatic CC to test locality, SERDES, and `@variant` — hits and misses both welcome.

## Raytracer

[`raytracer/`](raytracer/) is the Shirley *Ray Tracing in One Weekend*
cover scene: sequential C, Concurrent-C `@parallel for` over scanlines,
and Go with one goroutine per row. Same camera, same per-pixel LCG,
checksum of the framebuffer. `./raytracer/compare.sh` (or `--smoke`).

## Sanitizers / fuzz

```bash
./scripts/real_projects_sanitize.sh asan
./scripts/real_projects_sanitize.sh tsan
./scripts/real_projects_sanitize.sh fuzz
./scripts/real_projects_sanitize.sh all
```

Covers the main specimens (`pigz_idiomatic`, `pigz_cc` build, `redis_idiomatic` +
smoke, `levenshtein`). Darwin auto-uses Docker for runtime (host ASan/TSan +
fibers hang). Latest ASan + TSan receipts:
[`docs/sanitizers.md`](../docs/sanitizers.md).
