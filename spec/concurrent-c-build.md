# Concurrent-C Build Spec (Draft)

Status: draft. Purpose: define the `cc` driver behavior for C output, compilation, and linking, while staying compatible with existing C build systems.

## Goals
- “Default does the right thing”: minimal flags to go from `.cc` to a binary.
- Remain C-friendly: emit clean C; allow Make/CMake to own compilation if desired.
- Deterministic behavior; clear, inspectable outputs; easy overrides.

## Invocation Modes
- `cc [options] <input.cc>` — default end-to-end: emit C, compile, link.
- `cc --emit-c-only ...` — stop after emitting C.
- `cc --compile ...` — emit C and compile to object; no link.
- `cc --link ...` — emit C, compile, link (default).
- `cc build ...` — same as above but integrates `build.cc` consts (as today).

## Outputs and Defaults
- C output: `out/<stem>.c` by default (create `out/` if absent). Override with `-o` in `--emit-c-only` mode.
- Object output: `out/<stem>.o` by default in `--compile`/`--link` modes (override with `--obj-out`).
- Binary output: `out/<stem>` by default in `--link` mode (override with `-o`).
- `--keep-c` preserves the generated C even when compiling/linking; otherwise it’s retained in `out/`.

## Toolchain Selection
- C compiler: use `$CC` if set, else first of `cc`, `gcc`, `clang` in PATH. Override with `--cc-bin PATH`.
- Flags passthrough: honor `$CFLAGS`, `$CPPFLAGS`, `$LDFLAGS`, `$LDLIBS`. Additional flags via `--cc-flags "..."`, `--ld-flags "..."`.
- Target/cross: accept `--target TRIPLE` and forward to the C compiler; accept `--sysroot PATH` and forward.

## Runtime Linking
- Default: link against the bundled runtime (single TU or archive) automatically.
- `--no-runtime` to skip adding the runtime (for external linkage).
- `--libdir/--includedir` overrides where runtime headers/libs are found if separated.

## Build.cc Integration
- Discovery: input directory `build.cc` preferred, then CWD; `--build-file` overrides; `--no-build` disables.
- Const precedence: build.cc consts loaded first; CLI `-D` overrides with warning.
- `--dump-consts` to show merged consts; `--dry-run` to skip compile/link after resolution.

## Error Reporting
- On C compiler/linker failure: print the exact command, exit code, and keep generated C/object for inspection.
- On const binding overflow: clear error with max binding counts.
- Build.cc parse errors: show file/line and message; fall back to defaults only if no consts emitted and no fatal error.

## Compatibility Guidance
- For Make/CMake integration: use `cc --emit-c-only` to generate C and let the existing build drive compile/link, or `cc --compile` to produce objects. All outputs are deterministic under `out/` by default.
- Keep generated C stable to aid caching and diagnostics.
- Support `--verbose` to print all invoked commands.

## Future Extensions (non-normative)
- Incremental recompilation keyed on input + build.cc hash.
- Dependency emission (`-MMD` style) when invoking the C compiler.
- Configurable out dir (`--out-dir`) and per-artifact cleanup policy.

## Inspiration (non-normative): Zig `zig build`
- Zig models builds as a **DAG of named steps** (build/install/run/test/custom), with project options registered and exposed via `zig build --help`.
- We can mirror this shape in CC:
  - `cc build <step>` where `<step>` is `default`/`run`/`test` etc.
  - Project-specific settings as `-D` options (Zig’s pattern: `-Dtarget=...`, `-Doptimize=...`).

See `docs/zig-build-inspiration.md`.

