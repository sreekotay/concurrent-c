# Concurrent-C Build Spec (Draft)

Status: draft. Purpose: define the `ccc` driver behavior for C output, compilation, and linking, while staying compatible with existing C build systems.

## Goals
- “Default does the right thing”: minimal flags to go from `.ccs` to a binary.
- Remain C-friendly: emit clean C; allow Make/CMake to own compilation if desired.
- Deterministic behavior; clear, inspectable outputs; easy overrides.

## Invocation Modes
- `ccc [options] <input.ccs>` — default end-to-end: emit C, compile, link.
- `ccc --emit-c-only ...` — stop after emitting C.
- `ccc --compile ...` — emit C and compile to object; no link.
- `ccc --link ...` — emit C, compile, link (default).
- `ccc build ...` — integrates `build.cc` (consts + targets).

Build steps:
- `ccc build` (default)
- `ccc build run ...`
- `ccc build test ...`
- `ccc build list ...`
- `ccc build graph ...`
- `ccc build install ...`

## Outputs and Defaults
- C output: `out/<stem>.c` by default. Override with `-o` in `--emit-c-only` mode.
- Object output: `out/<stem>.o` by default in `--compile`/`--link` modes (override with `--obj-out`).
- Binary output: `bin/<stem>` by default in `--link` mode (override with `-o`).
- `--keep-c` preserves the generated C even when compiling/linking; otherwise it’s retained in `out/`.

## Output directories
- `--out-dir DIR` / `CC_OUT_DIR`: override where generated C and objects are written (default: `<repo>/out`).
- `--bin-dir DIR` / `CC_BIN_DIR`: override where linked executables are written (default: `<repo>/bin`).

## Incremental cache
- Default behavior: `ccc build` uses a lightweight cache under `out/.cc-build/` to skip redundant emit/compile/link work when inputs and flags are unchanged.
- Disable: `--no-cache` or `CC_NO_CACHE=1`.

## Toolchain Selection
- C compiler: use `$CC` if set, else first of `cc`, `gcc`, `clang` in PATH. Override with `--cc-bin PATH`.
- Flags passthrough: honor `$CFLAGS`, `$CPPFLAGS`, `$LDFLAGS`, `$LDLIBS`. Additional flags via `--cc-flags "..."`, `--ld-flags "..."`.
- Target/cross: accept `--target TRIPLE` and forward to the C compiler; accept `--sysroot PATH` and forward.

## Runtime Linking
- Default: link against the bundled runtime automatically.
- `--no-runtime` to skip adding the runtime (for external linkage).
- `--libdir/--includedir` overrides where runtime headers/libs are found if separated.

## Build.cc Integration
### Discovery
- Prefer a `build.cc` **alongside the first input** (`<input_dir>/build.cc`), else fall back to `./build.cc` (CWD).
- If **both** exist, `ccc build` errors (ambiguous).
- `--build-file PATH` overrides discovery.
- `--no-build` disables build.cc integration even if present.

### Consts (current stub format)
`build.cc` may declare compile-time integer consts:
- `CC_CONST <NAME> <EXPR>`

Where `<EXPR>` is currently one token:
- An integer literal parsed by `strtoll(..., base=0)` (so `123`, `0xff`, etc), or
- One of the target const symbols:
  - `TARGET_PTR_WIDTH`
  - `TARGET_IS_LITTLE_ENDIAN` (0/1)

Const precedence:
- Target consts are injected first.
- `CC_CONST` lines are loaded next.
- CLI `-DNAME[=VALUE]` are applied last and override prior values (with a warning if overriding a build.cc const).

### Options (help-only today)
`build.cc` may declare project options for help output:
- `CC_OPTION <NAME> <HELP...>`

These are printed by `ccc build --help` when a `build.cc` is in scope (or when `--build-file` is provided).

### Targets (target graph)
`build.cc` may declare buildable targets:
- `CC_DEFAULT <TARGET_NAME>`
- `CC_TARGET <TARGET_NAME> exe <SRC...>`
- `CC_TARGET <TARGET_NAME> obj <SRC...>`

Notes:
- `<SRC...>` is a whitespace-separated list of source files.
- Source paths are resolved **relative to the directory containing `build.cc`**.
- Sources may be a mix of `.ccs` (CC, lowered first) and `.c` (compiled directly).

Per-target properties (optional):
- `CC_TARGET_INCLUDE <TARGET_NAME> <DIR...>`
  - Adds per-target include directories (each resolved relative to `build.cc` dir).
- `CC_TARGET_DEFINE <TARGET_NAME> <NAME[=VALUE]...>`
  - Adds per-target compile defines (equivalent to `-DNAME[=VALUE]`).
- `CC_TARGET_CFLAGS <TARGET_NAME> <FLAGS...>`
  - Extra per-target compile flags (appended before CLI `--cc-flags`).
- `CC_TARGET_LDFLAGS <TARGET_NAME> <FLAGS...>`
  - Extra per-target link flags (appended before CLI `--ld-flags`).
- `CC_TARGET_LIBS <TARGET_NAME> <LIB...>`
  - Adds per-target link libraries.
  - Tokens starting with `-` are passed through as-is; otherwise they are treated as a library name and lowered to `-l<LIB>`.
- `CC_TARGET_DEPS <TARGET_NAME> <DEP_TARGET...>`
  - Declares dependencies on other `CC_TARGET` names.
  - Semantics: deps are built first (compiled into objects under `out/obj/<target>/`), and dependents link those objects.
  - Dependency `ldflags`/`libs` are included in the final link.
  - Cycles and unknown target names are errors.
- `CC_TARGET_OUT <TARGET_NAME> <BIN_NAME>`
  - Sets the default output binary name (under `bin/`) for an `exe` target.
- `CC_TARGET_TARGET <TARGET_NAME> <TRIPLE>`
  - Per-target `--target` override for compilation and linking.
- `CC_TARGET_SYSROOT <TARGET_NAME> <PATH>`
  - Per-target `--sysroot` override for compilation and linking.
- `CC_INSTALL <TARGET_NAME> <DEST>`
  - Used by `ccc build install <target>` to copy the produced binary to `<DEST>`.
  - `<DEST>` is resolved relative to repo root unless it is an absolute path.

### Introspection
- `ccc build list` prints the declared target graph (and key per-target properties).
- `ccc build graph` prints the target graph as `--format json` (default) or `--format dot`.
- `--dump-consts` prints merged const bindings then compiles.
- `--dry-run` resolves consts / prints commands, and skips compile/link.

## Error Reporting
- On C compiler/linker failure: print the exact command, exit code, and keep generated C/object for inspection.
- On const binding overflow: clear error with max binding counts.
- Build.cc parse errors: show file/line and message; fall back to defaults only if no consts emitted and no fatal error.

## Compatibility Guidance
- For Make/CMake integration: use `ccc --emit-c-only` to generate C and let the existing build drive compile/link, or `ccc --compile` to produce objects.
- Keep generated C stable to aid caching and diagnostics.
- Support `--verbose` to print all invoked commands.

## Future Extensions (non-normative)
- More build graph features (explicit outputs/inputs, richer option types).
- Depfile integration beyond per-translation-unit (`-MMD` style) as needed.

## Inspiration (non-normative): Zig `zig build`
- Zig models builds as a **DAG of named steps** (build/install/run/test/custom), with project options registered and exposed via `zig build --help`.
- We can mirror this shape in CC:
  - `ccc build <step>` where `<step>` is `default`/`run`/`test` etc.
  - Project-specific settings as `-D` options (Zig’s pattern: `-Dtarget=...`, `-Doptimize=...`).

See `docs/zig-build-inspiration.md`.

## Recipes (copy/paste)

Assuming you built the compiler (`make -C cc ...`) and are in the repo root.

### Build + run a single file

```bash
./cc/bin/ccc build run examples/hello.ccs
```

### Emit generated C only (let your build system compile it)

```bash
./cc/bin/ccc --emit-c-only examples/hello.ccs
ls -l out/hello.c
```

### Use a target graph

```bash
./cc/bin/ccc build --build-file examples/mixed_c/build.cc --summary
./cc/bin/ccc build list --build-file examples/mixed_c/build.cc
./cc/bin/ccc build graph --format dot --build-file examples/mixed_c/build.cc
```

### Install a target binary

```bash
./cc/bin/ccc build install hello --build-file examples/mixed_c/build.cc --summary
```

### Disable the incremental cache

```bash
./cc/bin/ccc build --no-cache run --summary examples/hello.ccs
```

