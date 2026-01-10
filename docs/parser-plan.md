# Parser/Compiler Bring-up Plan (TCC Overlay)

## Goals (Phase 1)
- Parse CC syntax via TCC overlay with minimal, rebasing-friendly hooks.
- Run smoke tests through `cc/bin/ccc` (replace host `cc`).
- Enable UFCS rewrite and const-pass feeding a real AST.

## Steps
1) **Apply TCC hooks**  
   - Patches in `third_party/tcc-patches/000{1..4}-*.patch`.  
   - Expect to expose: lexer token registration, parser entry point for CC mode, constexpr API, AST side-table access.
   - Script: `./scripts/apply_tcc_patches.sh` (to be run from repo root).

2) **Build patched TCC**  
   - Inside `third_party/tcc`: `./configure && make -j` (keep local-only; no repo changes in vendor tree besides patches).
   - Optional: install or point `cc` at the built `tcc` via `--cc-bin`.

3) **Bridge layer (`cc/src/parser/tcc_bridge.c`)**  
   - Implement `cc_tcc_parse_to_ast`/`cc_tcc_free_ast` using the new TCC APIs.
   - Enable via `make -C cc TCC_EXT=1 TCC_INC=../third_party/tcc TCC_LIB=../third_party/tcc` (adds `-DCC_TCC_EXT_AVAILABLE` and links `-ltcc`).
   - Map TCC AST nodes into our `CCASTRoot` opaque handle; stash side-table pointers for later passes.

4) **Const-pass plumbing**  
   - Populate `CCSymbolTable` from literals/const decls encountered during parse (using TCC constexpr hook).
   - Feed these into the main pass for `@comptime if` and generic instantiation.

5) **UFCS + main pass**  
   - Walk the parsed AST to perform UFCS desugaring, basic type/semantics checks, and C emission with `#line`.
   - Preserve source spans from TCC side-table for diagnostics and source maps.

6) **cc-driven tests**  
   - Convert smoke tests to CC sources and run via `cc/bin/ccc --emit-c-only ... && cc ...` until we add direct link mode.
   - Add a `make smoke-cc` (or `scripts/run_smoke.sh`) that exercises hello + map/vec/io in CC.

## Open Questions
- Should we gate CC mode in TCC with a CLI flag vs. env var? (Recommend a `-cc` or `--cc-mode` hook exposed by patched TCC.)
- Side-table format: stable enough to read directly, or wrap with small helper functions in TCC?

## Risks
- Divergence from upstream TCC; keep patches minimal and well-scoped.
- AST mapping churn if upstream changes node layouts; mitigate via side-table indirection.

