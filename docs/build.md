# CC Build System Plan

## Goals
- One language for build and sources (CC); no shell glue required.
- Deterministic, target-aware consts available to `@comptime` without extra flags.
- Keep MVP simple: existing Makefile/build flow keeps working; build.cc is additive.

## CLI Modes
- `cc <src> -o <out>` — Phase 1 simple compilation, no build.cc.
- `cc build <target>` — Phase 2+ integrated build: executes build.cc, then compiles targets with exported consts in scope.

## build.cc Contract (Phase 2 target)
- **Discovery:** Look for `build.cc` in CWD or alongside the requested target.
- **Inputs:** Target description (`target.os`, `target.arch`, `target.abi`, `target.endian`, `target.ptr_width`), env (whitelisted), CLI options (e.g., `-D`), and maybe workspace root.
- **Outputs:** A set of const bindings `{name,value}` (e.g., `DEBUG`, `USE_TLS`, `NUM_WORKERS`). Later: optional target graph/deps.
- **Determinism:** Pure by default—no filesystem/network/clock unless explicitly allowed (`--allow-fs` later).
- **Error reporting:** Errors surface with file/line as normal compile diagnostics.
- **Precedence:** Decide early; recommendation: CLI `-D` overrides build.cc for predictability.

## Const Plumbing
- Driver preloads build.cc outputs into the comptime symbol table before Pass 0.
- Lookup order for `@comptime if`: predefined consts → user consts.
- Last writer wins within the predefined set.

## Caching & Incremental (Phase 3)
- Cache hash of build.cc + target inputs → const set.
- Reuse across compilations until inputs change; invalidate on build.cc edit or target/env change.

## Cross-Compilation
- Always pass target triple data to build.cc.
- Build.cc decides feature flags (`USE_TLS`, `HAS_PTHREADS`, etc.) consumed during comptime.

## Near-Term Work
- Keep Phase 1 build flow unchanged (Makefile/`cc` binary).
- Implement build.cc loader stub that returns a fixed const set to exercise the pipeline. ✅
- Add real build.cc execution with sandboxing and const export in Phase 2.

## Current Stub Behavior (Phase 1)
- CLI: `cc build [options] <input.cc> <output.c>`.
- Discovery: prefers `build.cc` alongside the input file; falls back to CWD; errors if both exist.
- Target info: host-detected `target.os/arch/abi/endian/ptr_width` is passed to the loader; `TARGET_PTR_WIDTH` and `TARGET_IS_LITTLE_ENDIAN` consts are emitted.
- build.cc can emit consts via lines like `CC_CONST NAME EXPR` (whitespace-separated). EXPR supports integer literals or target const symbols (e.g., `TARGET_PTR_WIDTH`). If none are found, a fixed default set is used: `DEBUG=1`, `USE_TLS=1`, `NUM_WORKERS=4`.
- CLI `-DNAME[=VALUE]` defines are applied after build.cc and override its consts (VALUE defaults to 1).
- If no build.cc is found, compilation proceeds with only CLI consts (or none), same as `cc <input.cc> <output.c>`.

