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
- `ccc build ...` — same as above but integrates `build.cc` consts (as today).

## Outputs and Defaults
- C output: `out/<stem>.c` by default (create `out/` if absent). Override with `-o` in `--emit-c-only` mode.
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
- Default: link against the bundled runtime (single TU or archive) automatically.
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

Notes:
- `<SRC...>` is a whitespace-separated list of source files.
- Source paths are resolved **relative to the directory containing `build.cc`**.

### Introspection
- `--dump-consts` prints merged const bindings then compiles.
- `--dry-run` resolves consts / prints commands, and skips compile/link.

## Error Reporting
- On C compiler/linker failure: print the exact command, exit code, and keep generated C/object for inspection.
- On const binding overflow: clear error with max binding counts.
- Build.cc parse errors: show file/line and message; fall back to defaults only if no consts emitted and no fatal error.

## Compatibility Guidance
- For Make/CMake integration: use `ccc --emit-c-only` to generate C and let the existing build drive compile/link, or `ccc --compile` to produce objects. All outputs are deterministic under `out/` by default.
- Keep generated C stable to aid caching and diagnostics.
- Support `--verbose` to print all invoked commands.

## Future Extensions (non-normative)
- Incremental recompilation keyed on input + build.cc hash.
- Dependency emission (`-MMD` style) when invoking the C compiler.
- Configurable out dir (`--out-dir`) and per-artifact cleanup policy.

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

Pass args to the produced binary:

```bash
./cc/bin/ccc build run examples/hello.ccs -- --help
```

### Emit generated C only (let your build system compile it)

```bash
./cc/bin/ccc --emit-c-only examples/hello.ccs
ls -l out/hello.c
```

### Override output directories

```bash
CC_OUT_DIR=out2 CC_BIN_DIR=bin2 ./cc/bin/ccc build run examples/hello.ccs --summary
```

### Use a target graph (`CC_TARGET` / `CC_DEFAULT`)

```bash
./cc/bin/ccc build --build-file examples/build_graph/build.cc --summary
./cc/bin/ccc build multi --build-file examples/build_graph/build.cc --summary
./cc/bin/ccc build run multi --build-file examples/build_graph/build.cc --summary
```

### See declared project options / targets

```bash
./cc/bin/ccc build --help --build-file examples/build_graph/build.cc
```

### Define comptime consts from the CLI

```bash
./cc/bin/ccc build --dump-consts --dry-run -DDEBUG -DNUM_WORKERS=8 examples/hello.ccs
```

### Disable the incremental cache

```bash
./cc/bin/ccc build --no-cache run --summary examples/hello.ccs
```

