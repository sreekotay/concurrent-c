# Contributor guidance

- Spec register: text under `spec/` never contains metanarration — no "RATIFIED"/"superseded"/"earlier drafts", dates or change markers, or design-review archaeology; write minimal, clear, complete, present-tense normative prose (status banners like "draft — not implemented" are allowed). Git history holds the reasoning.

- Prefer UFCS at call sites (`db.maps.count()`, `hold.release()`, `sh->get(k)`) over free-function snake names (`cc_shard_map_count(&db.maps)`, …). See `.cursor/rules/prefer-ufcs.mdc`.
- How to use the language (ownership, arenas, results, locality): [`docs/the-cc-way.md`](docs/the-cc-way.md).

- No silent degradation. A path that gives up must not be indistinguishable from a path that had nothing to do. Returning `NULL`/`0`/unchanged input to mean "couldn't" is only safe when the caller can tell that apart from "no work needed" — otherwise say so, at the position that caused it. Four instances so far, each found by accident and each expensive: an FFI symbol declared with the wrong return ABI degraded into a polite "unsupported"; local header lowering does nothing at all outside the repo root; one unbalanced `@comptime` block makes the header stripper skip every block in the file; the parse session quietly runs at a different C version than the host, so headers take branches the real compile never sees. A fallback is a good place to hide a bug precisely because it looks like success.

- When to run which build/smoke command: `docs/build-when.md`.
- `shadow_lower` bootstrap: **fixes always land in `cc/shadow/*.ccs` / `*.cch` first** — that is the only source of truth. `out/include/cc/shadow/*.h` and committed `MAJOR.MINOR.PATCH-N/` are regenerate-only products; never edit them to “fix” a bug (`make -C cc` recopies `last-good` over `out/include`). Iterate: `./scripts/iterate_shadow_lower.sh`. Ship a seed: `./scripts/iterate_shadow_lower.sh --ship --smoke` (ccs → snapshot → promote → bootstrap build). Do not patch `last-good`'s tree in place; do not `cp` raw `.cch` onto `out/include`. Details: `cc/bootstrap/shadow_lower/README.md`, `.cursor/rules/shadow-bootstrap.mdc`.
