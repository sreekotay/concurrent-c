# Concurrent-C Build Spec (Draft)

Status: draft. Purpose: define the `ccc` driver behavior for C output, compilation, and linking, while staying compatible with existing C build systems.

## Goals
- “Default does the right thing”: minimal flags to go from `.ccs` to a binary.
- Remain C-friendly: emit clean C; allow Make/CMake to own compilation if desired.
- Deterministic behavior; clear, inspectable outputs; easy overrides.

## Design Philosophy

**Declarative sugar over imperative comptime.**

Like Zig's `build.zig`, CC's `build.cc` should ultimately be real CC code that runs at compile time. However, most builds are simple and don't need imperative logic. Therefore:

1. **Declarative lines** (`CC_TARGET`, `CC_CONST`, etc.) handle the 80% case — simple, scannable, no logic required.
2. **Comptime blocks** (`@comptime { ... }`) handle the 20% case — conditionals, loops, platform detection, dynamic target generation.
3. **Declarative desugars to comptime** — they are not separate systems. `CC_TARGET main exe main.ccs` is syntactic sugar for equivalent comptime API calls.

This matches C's own philosophy: simple things are simple (`int x = 5;`), complex things are possible (`__attribute__`, inline asm).

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

### Output Layout

```
out/
  <stem>.c          # generated C
  <stem>.o          # object file
  <stem>.d          # dependency file (for incremental builds)
  .cc-build/        # cache metadata (hidden)
bin/
  <stem>            # linked binary
```

The `<stem>` is the basename of the input file without extension: `examples/hello.ccs` → `hello`.

### Name Collision Note

Output paths use basename only, so files with the same name in different directories will collide:

```bash
./cc/bin/ccc a/test.ccs   # → out/test.c, bin/test
./cc/bin/ccc b/test.ccs   # → overwrites out/test.c, bin/test
```

**Workarounds:**
- Use `-o` to specify distinct output paths: `ccc a/test.ccs -o bin/a_test`
- Use `build.cc` with explicit target names (recommended for multi-file projects)
- Use `--out-dir` / `--bin-dir` to isolate outputs per project

This flat naming is intentional for simplicity in single-file workflows. Multi-file projects should use `build.cc` for explicit control.

`ccc` automatically detects when multiple inputs share a basename and will widen the stem (by incorporating directory segments) so `out/` and `bin/` files no longer overwrite one another.  For finer control or to impose a custom identity, pass `--out-stem NAME`.

### Output naming customization
Automated builds can still conflict when legacy trees contain repeated basenames. Use `--out-stem NAME` to override the stem used for the generated `out/<stem>.*` and `bin/<stem>` outputs.

```bash
./cc/bin/ccc --out-stem examples_main build examples/main.ccs
```

now emits `out/examples_main.c` / `bin/examples_main`. This lets teams keep the simple layout while disambiguating outputs from legacy directories.

## Output directories
- `--out-dir DIR` / `CC_OUT_DIR`: override where generated C and objects are written (default: `<repo>/out`).
- `--bin-dir DIR` / `CC_BIN_DIR`: override where linked executables are written (default: `<repo>/bin`).
- `--out-stem NAME`: override the basename/stem used for generated files (auto disambiguation keeps derived names unique).

## Incremental cache
- Default behavior: `ccc build` uses a lightweight cache under `out/.cc-build/` to skip redundant emit/compile/link work when inputs and flags are unchanged.
- Disable: `--no-cache` or `CC_NO_CACHE=1`.

## Toolchain Selection
- C compiler: use `$CC` if set, else first of `cc`, `gcc`, `clang` in PATH. Override with `--cc-bin PATH`.
- Flags passthrough: honor `$CFLAGS`, `$CPPFLAGS`, `$LDFLAGS`, `$LDLIBS`. Additional flags via `--cc-flags "..."`, `--ld-flags "..."`.
- Target/cross: accept `--target TRIPLE` and forward to the C compiler; accept `--sysroot PATH` and forward.

### Supported Compilers

| Compiler | Support | Notes |
|----------|---------|-------|
| **Clang** | ✅ Full | Recommended on macOS |
| **GCC** | ✅ Full | Recommended on Linux |
| **TCC** | ⚠️ Partial | See limitations below |
| **MSVC** | 🚧 Planned | Not yet tested |

### TCC Compatibility

TCC (Tiny C Compiler) is used internally by `ccc` for fast preprocessing and parsing. It can also be used as the final C compiler via `CC=./third_party/tcc/tcc`, but with limitations:

**What works:**
- TCC as preprocessor (always used by ccc internally)
- `cc_atomic.cch` portable atomics (detects TCC, uses fallbacks)
- Compiler flag detection (ccc skips unsupported flags like `-MMD`, `-ffunction-sections`)

**Platform limitations:**

| Platform | TCC as Final Compiler |
|----------|----------------------|
| **Linux** | ✅ Works (TCC supports `__thread` TLS) |
| **macOS** | ❌ Not supported (TCC lacks `__thread` TLS on macOS) |
| **Windows** | 🚧 Untested |

**Why macOS doesn't work:** The CC runtime uses `__thread` for thread-local storage (nursery tracking, deadline scopes). TCC on macOS doesn't implement the Mach-O TLS ABI (`_tlv_bootstrap`, TLV sections). This is a fundamental TCC limitation, not a ccc bug.

**Alternatives considered:**
- Patching TCC for macOS TLS: ~1 week effort, high complexity, not worth it
- Using `pthread_getspecific` everywhere: ~10x slower TLS access, penalizes 99% of users
- Single-threaded mode for TCC: Possible but defeats the purpose of concurrent code

**Recommendation:** Use Clang or GCC on macOS. TCC-as-final-compiler is only for Linux or niche use cases.

### Build Flavors

Build flavor flags control optimization and debugging:

```bash
./cc/bin/ccc build foo.ccs -O          # Release: -O2 -DNDEBUG + dead-stripping
./cc/bin/ccc build foo.ccs -g          # Debug: -O0 -g
./cc/bin/ccc build foo.ccs --release   # Same as -O
./cc/bin/ccc build foo.ccs --debug     # Same as -g
```

| Flag | Compiler Flags | Linker Flags |
|------|---------------|--------------|
| `-O` / `--release` | `-O2 -DNDEBUG -ffunction-sections -fdata-sections` | `-Wl,-dead_strip` (macOS) / `-Wl,--gc-sections` (Linux) |
| `-g` / `--debug` | `-O0 -g` | (none) |

**Note:** When both `-O` and `-g` are specified, debug wins (safe default).

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

### End-State Vision: Comptime Build Scripts

The end goal is `build.cc` as real CC code with comptime evaluation:

```c
// build.cc — CC code, evaluated at compile time
@comptime {
    CCTarget* exe = cc_exe("main");
    exe.sources("main.ccs", "utils.ccs");
    
    if (cc_option_bool("debug")) {
        exe.define("DEBUG", "1");
        exe.cflags("-fsanitize=address");
    }
    
    if (TARGET_OS == OS_WINDOWS) {
        exe.libs("ws2_32");
    } else {
        exe.libs("pthread");
    }
    
    cc_default(exe);
}
```

This is imperative (matches C thinking), uses the same language (no DSL to learn), and handles complex builds naturally.

### Comptime helper utilities (non-normative)
Some lightweight helper utilities may be added to support legacy build script patterns, but they are not specified or required for this draft. Today the build integration supports declarative `CC_*` lines plus `--dump-comptime`/`ccc build graph` introspection.

### Declarative Syntax (Current & Permanent)

For simple builds, declarative lines are more convenient. These desugar to equivalent comptime API calls:

| Declarative | Equivalent Comptime |
|-------------|---------------------|
| `CC_TARGET main exe main.ccs` | `cc_exe("main").sources("main.ccs")` |
| `CC_TARGET_LIBS main pthread` | `cc_target("main").libs("pthread")` |
| `CC_CONST DEBUG 1` | `cc_const("DEBUG", 1)` |
| `CC_DEFAULT main` | `cc_default(cc_target("main"))` |

Both forms coexist in the same `build.cc`. Use declarative for simple cases, drop into `@comptime` when you need logic.

### Consts

Compile-time integer constants:
- `CC_CONST <NAME> <EXPR>`

Where `<EXPR>` is:
- An integer literal (`123`, `0xff`, etc), or
- A target const: `TARGET_PTR_WIDTH`, `TARGET_IS_LITTLE_ENDIAN`, `TARGET_OS`

Precedence (later overrides earlier):
1. Target consts (injected by compiler)
2. `CC_CONST` lines
3. CLI `-DNAME[=VALUE]`

### Options

Project options for `--help` output:
- `CC_OPTION <NAME> <HELP...>`

End-state: options become typed and queryable in comptime:
```c
@comptime {
    int workers = cc_option_int("workers", 4);  // default 4
    bool debug = cc_option_bool("debug");
}
```

### Targets

Declarative target declarations:
- `CC_DEFAULT <TARGET_NAME>`
- `CC_TARGET <TARGET_NAME> exe <SRC...>`
- `CC_TARGET <TARGET_NAME> obj <SRC...>`

Notes:
- `<SRC...>` is whitespace-separated; paths relative to `build.cc` directory.
- Sources may mix `.ccs` (lowered) and `.c` (compiled directly).

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

#### Target grouping
`CC_TARGET_GROUP <GROUP_NAME>` collects multiple declarative directives into a logical block that shares configuration. Inside a group you can omit repeated `CC_TARGET` names and the group name prefixes generated targets. Group-level directives that set include paths, defines, libs, or deps are applied to every target declared within the block, reducing boilerplate for legacy suites that define many similar binaries. The group desugars to per-target calls such as:

```c
@comptime {
    CCTarget* exe = cc_exe("server-foo");
    exe.include("server/include");
    cc_default(exe);
}
```

where `server-foo` is the concatenation of `CC_TARGET_GROUP` plus the nested `CC_TARGET` name.

### Introspection
- `ccc build list` prints the declared target graph (and key per-target properties).
- `ccc build graph` prints the target graph as `--format json` (default) or `--format dot`.
- `--graph-out PATH` writes the graph output (json/dot) to PATH for downstream build systems.
- `--dump-consts` prints merged const bindings then compiles.
- `--dump-comptime` prints the computed const bindings, targets, and option bindings produced by `build.cc` before any emit/compile work.
- `--dry-run` resolves consts / prints commands, and skips compile/link.

### Legacy build integration
`ccc` can emit self-describing fragments so existing Make/CMake graphs can keep driving compilation while `ccc` owns C emission. The `ccc build export-make <target>` (alias: `ccc build export-ninja <target>`) subcommand writes a build-system fragment listing:

```
CCC_TARGET_<TARGET>_SRCS := main.ccs utils.c
CCC_TARGET_<TARGET>_CFLAGS := $(CCC_FLAGS) -Ilegacy/include
CCC_TARGET_<TARGET>_LDFLAGS := -lm
CCC_TARGET_<TARGET>_OUT := bin/legacy
```

These fragments include dependencies, per-target outputs, computed libs, and the `CCC_TARGET_<TARGET>_COMPTIME` bool that signals whether the target came from a `@comptime` block or declarative syntax. Legacy Makefiles can `include` the fragment and then run `$(CCC_TARGET_<TARGET>_SRCS)` through their existing rules, while still calling `ccc build` for C emission and caching. This keeps the declarative metadata in sync with `build.cc` without duplicating logic.

## Testing Infrastructure

### Stress Tests

Stress tests in `stress/` exercise concurrency patterns under load:

| Test | What it stresses | Correctness check |
|------|------------------|-------------------|
| `spawn_storm` | 1000 concurrent task spawns | All tasks complete |
| `channel_flood` | 10 producers × 1000 msgs | Message count matches |
| `closure_capture_storm` | Many closures with captures | Sum formula verification |
| `fanout_fanin` | Scatter-gather pattern | Sum of squares formula |
| `nursery_deep` | 20 levels of nested nurseries | All levels complete |
| `pipeline_long` | 4-stage channel pipeline | Sum formula verification |
| `worker_pool_heavy` | 8 workers × 500 jobs | Sum formula verification |
| `deadline_race` | Tasks with tight deadlines | Total = completed + timed_out |

### Sanitizer Testing

Run stress tests with ThreadSanitizer (race detection) and AddressSanitizer (memory errors):

```bash
./scripts/stress_sanitize.sh              # default compiler only
./scripts/stress_sanitize.sh tsan         # ThreadSanitizer
./scripts/stress_sanitize.sh asan         # AddressSanitizer
./scripts/stress_sanitize.sh sanitizers   # Both TSan and ASan
./scripts/stress_sanitize.sh compilers    # Test with clang, gcc, (tcc on Linux)
./scripts/stress_sanitize.sh all          # Everything
```

**Note:** Sanitizers require Clang or GCC. TSan and ASan are mutually exclusive (run separately).

### Portable Atomics

For user code needing thread-safe counters or lock-free structures, include `cc_atomic.cch`:

```c
#include <ccc/cc_atomic.cch>

cc_atomic_int counter = 0;

void increment(void) {
    cc_atomic_fetch_add(&counter, 1);
}
```

The header auto-detects the compiler and uses:
- C11 `<stdatomic.h>` on GCC/Clang
- `__sync_*` builtins on older compilers
- Non-atomic fallback on TCC (not thread-safe, best-effort only)

See `<cc_atomic.cch>` documentation in the stdlib spec for full API.

## Error Reporting
- On C compiler/linker failure: print the exact command, exit code, and keep generated C/object for inspection.
- On const binding overflow: clear error with max binding counts.
- Build.cc parse errors: show file/line and message; fall back to defaults only if no consts emitted and no fatal error.

## Compatibility Guidance
- For Make/CMake integration: use `ccc --emit-c-only` to generate C and let the existing build drive compile/link, or `ccc --compile` to produce objects.
- Keep generated C stable to aid caching and diagnostics.
- Support `--verbose` to print all invoked commands.

## Future Extensions (non-normative)

### Comptime Build API (priority)
- Full comptime evaluation in `build.cc`
- Typed option queries: `cc_option_int()`, `cc_option_bool()`, `cc_option_string()`
- Target constants: `TARGET_OS`, `TARGET_ARCH`, `TARGET_PTR_WIDTH`
- Conditional target generation in `@comptime` blocks

### Build Graph Enhancements
- Explicit outputs/inputs for custom steps
- Parallel step execution
- Richer depfile integration

### Standard Options
Following Zig's pattern, standardize common options:
- `-Doptimize=Debug|ReleaseSafe|ReleaseFast|ReleaseSmall`
- `-Dtarget=<triple>`

### Legacy Integration Helpers
- **Comptime helper library** — expose a small set of `comptime` utilities (path normalization, simple JSON/TOML readers, flag expansion, etc.) so messy legacy build scripts can be re‑expressed inside `@comptime` blocks without hand‑rolling low-level parsing logic.
- **Automatic stem disambiguation** — add `--out-stem <prefix>` (or similar `build.cc` hook) that prefixes generated files with their relative directories when basenames collide, letting legacy trees with reused filenames drop into the new output layout without accidental overwrites.
- **Declarative → Make/Ninja bridge** — provide a way to emit target metadata (`cc_target` names, sources, deps, flags) as Makefile or Ninja fragments that legacy `Makefile`s can include, so `ccc build` can coexist with existing dependency graphs instead of replacing them wholesale.
- **`--dump-comptime` mode** — add a build flag that prints the resolved `@comptime` state (constants, generated targets, selected branches) before running the compiler; this helps users debug why the comptime script produced a particular graph in a messy repo.
- **Target grouping sugar** — allow `CC_TARGET_GROUP <name> { ... }` (or similar) so a block can declare multiple related `CC_TARGET` entries without repeating boilerplate when importing sprawling legacy target lists.

## Inspiration: Zig `zig build`

Zig models builds as a **DAG of named steps** with `build.zig` as imperative Zig code. CC mirrors this:

| Zig | CC |
|-----|-----|
| `build.zig` (Zig code) | `build.cc` (CC code with comptime) |
| `zig build <step>` | `ccc build <step>` |
| `-Dname=value` | `-Dname=value` |
| `b.addExecutable(...)` | `cc_exe(...)` in `@comptime` |

Key difference: CC keeps declarative syntax (`CC_TARGET`) as sugar for simple cases, avoiding boilerplate for the common path.

For more details, see `docs/zig-build-inspiration.md`.

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

