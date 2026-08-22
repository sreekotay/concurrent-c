# Concurrent-C Build

This specification defines the implemented `ccc` driver contract for C emission, compilation, linking, cached builds, declarative target graphs, and Make integration.

## Invocation

The driver accepts:

- `ccc [options] <input> [output]`
- `ccc run <input> [-- <args...>]`
- `ccc build [step] [options] [<input> ...]`
- `ccc clean [--out-dir DIR] [--bin-dir DIR] [--all]`
- `ccc portable-install DIR`

Unit kind is the first-line header (`#!ccc ccs|cch`, or a `ccc` OS shebang for
scripts; see the complete spec §1.7). A `.ccs` / `.cch` / `.shcc` suffix is the
fallback when the header is absent. `version=MAJOR.MINOR` (usual) or a
tighter `MAJOR.MINOR.PATCH[-SEED]` (or `--ccc-version=`) pins the lowerer
to a bootstrap folder the pin prefixes.

The default pipeline emits C, compiles an object, and links an executable. `run` performs that pipeline and executes the result.

The build steps are:

- default: build the selected target or input files;
- `run`: build and execute an executable;
- `test`: run the repository test suite;
- `list`: print declarations from `build.cc`;
- `graph`: print the target graph;
- `install`: build and copy the selected executable to its `CC_INSTALL` destination;
- `export-make`: write a Makefile fragment for declared targets.

## Modes and outputs

- `--emit-c-only` stops after C emission.
- `--compile` stops after object compilation.
- `--link` selects the default emit, compile, and link pipeline.
- `--emit-c-inspect[=PATH]` requests a best-effort merged translation-unit dump while the selected pipeline still runs.
- `--print-cflags` prints the required Concurrent-C include flags.
- `--print-libs` prints `-DCC_ENABLE_ASYNC`, the runtime source, `-lpthread`, and `-lm`.
- `--cccportable DIR` / `CCCPORTABLE` makes `--print-cflags` / `--print-libs` name that consumer tree (one `-I$DIR/include`). It does not remap lowerer faces. Using `--cccportable` with emit, compile, or link is an error.
- `--no-line` omits `#line` and `CC_LN` from emitted C.
- `--sysroot PATH` is forwarded to the host C compiler (cross-compile). It is not a Concurrent-C snapshot.

`ccc portable-install DIR` copies lowered `include/ccc/**/*.h` and the rewritten runtime from the resolved toolchain (`g_cc_lowered_include` / `g_cc_runtime_c`) into `DIR`. The lowered include root is a complete host-C tree: every face-tree `*.h` (including `ccc/vendor/ffc.h`) is copied through as a real file. The snapshot is a consumer host-C bundle, not a compiler sysroot (no `.cch`). `DIR` must not exist, be empty, or already contain `CCCPORTABLE.txt`. A stamp mismatch on install or `--print-*` is an error. Re-emit is a human step.

`--print-cflags` and `--print-libs` are author-side helpers. A consumer Makefile does not invoke `ccc`; it hardcodes the vendor paths or a snippet generated once. Host-cc of `--emit-c-only` output must not define `CC_PARSER_MODE`: that define is only for `ccc` parse / reparse / comptime. Headers still contain `#ifdef CC_PARSER_MODE` stubs; defining it on the consumer compile takes those stubs.

For an input stem `NAME`, default single-input paths are:

```
out/NAME.c
out/.cc-build/host/<host-fp>/NAME.o
out/.cc-build/host/<host-fp>/NAME.d
bin/NAME
out/.cc-build/
out/include/   # lowered .h headers (shared; not keyed by host CC)
```

`<host-fp>` is a fingerprint of the resolved host C compiler (`$CC` / `--cc-bin`:
path, mtime, size, profile schema). Host-native objects, depfiles, runtime
objects built by the driver, and compile/link cache metas live under
`out/.cc-build/host/<host-fp>/` so switching `CC=tcc` cannot reuse clang
Mach-O objects (or the reverse). Emitted `.c` and lowered headers stay shared.

`-o PATH` selects the terminal output for the current mode. In link mode, `--obj-out PATH` independently selects the intermediate object. Generated C is retained; `--keep-c` is accepted and is therefore idempotent.

`--out-dir DIR` or `CC_OUT_DIR` replaces `out`. `--bin-dir DIR` or `CC_BIN_DIR` replaces `bin`. Command-line values take precedence over environment values. Relative `--out-dir` / `--bin-dir` and the default `out` / `bin` resolve against the process cwd in every layout (checkout binary and prefix install). Absolute paths are unchanged. Toolchain files (includes, runtime, lowerer) still come from the checkout or prefix.

### Stem identity and collisions

A single input uses its basename without extension. Separate invocations with equal basenames therefore address the same default output paths.

Within one multi-input invocation, the driver detects repeated basenames and derives wider stems from each repository-relative path. It then appends numeric suffixes if derived names still collide. This makes outputs unique within that invocation.

`--out-stem NAME` overrides the derived stem for a single input. It is an error with multiple inputs. Explicit `-o`, `--obj-out`, `--out-dir`, and `--bin-dir` remain available when identities must be isolated across separate invocations.

Declarative target builds do not depend on flat source stems. For the default
output root, each target uses:

```
out/c/<build-id>/<target>/<source-stem>__<source-path-hash>.c
out/.cc-build/host/<host-fp>/obj/<build-id>/<target>/<source-stem>__<source-path-hash>.o
out/.cc-build/host/<host-fp>/obj/<build-id>/<target>/<source-stem>__<source-path-hash>.d
```

`<build-id>` is a stable hash of the build-file directory. The unit suffix is
a stable hash of the source path relative to that directory, or of its
absolute path when it is outside the directory. The selected output root
replaces `out`.

## Driver options

The build parser implements:

- `-DNAME[=VALUE]`: define a compile-time integer constant and pass the same definition to the C compiler; omitted value means `1`.
- `--build-file PATH`: select an explicit build file.
- `--no-build`: disable build-file integration.
- `--dump-consts`: print merged constant bindings.
- `--dump-comptime`: print constants and target declarations; implies `--dump-consts`.
- `--dry-run`: resolve build state and print commands without compiling or linking.
- `--cc-bin PATH`: select the final C compiler.
- `--cc-flags FLAGS` and `--ld-flags FLAGS`: append compile and link flags.
- `--target TRIPLE` and `--sysroot PATH`: forward target settings to compile and link commands.
- `--no-runtime`: omit the bundled runtime from linking.
- `--no-cache` or `CC_NO_CACHE=1`: disable incremental caching.
- `--verbose`: print invoked commands.
- `--summary`: print whether pipeline outputs were built or reused.
- `--timeout SECONDS`: bound a `run` or `test` step.
- `-O` or `--release`: add `-O2 -DNDEBUG` and enable supported section dead-stripping.
- `-g` or `--debug`: add `-O0 -g`.

With neither flag, the driver adds `-O2` (asserts kept). If both flavor
flags are present, debug wins. The driver honors `CC`, `CFLAGS`, `CPPFLAGS`, `LDFLAGS`, and the runtime build's applicable C++ flags. `--cc-bin` overrides `CC`; otherwise the driver selects an available `cc`, `gcc`, or `clang`.

The bundled runtime is linked unless `--no-runtime` is present. The driver may reuse a compatible runtime object or build one under the output directory.

## Incremental cache

Cache metadata lives under `<out-dir>/.cc-build/`. Emit metas (`.meta`) are
shared there. Object and link metas (`.obj`, `.link`) live under
`<out-dir>/.cc-build/host/<host-fp>/` with the host-native objects. The driver
independently caches C emission, object compilation, runtime compilation, and
linking.

Cache keys include the relevant source and build-file signatures, compiler inputs, target and sysroot, command-line and environment flags, compile-time bindings, runtime signatures, declared `#pragma cc_depends` content, and the contents of `#include`d `.cch` files (quoted and angled, resolved on the compile include path). Object and link keys also fold the host-compiler fingerprint. Object reuse also checks dependency files. Host `.d` files rebuild objects from already-emitted C; they do not re-lower a `.ccs`.

Failed emission is not cached. A diagnostic-producing emit fails the build, and a later invocation reruns it even when prior cache metadata exists. `--no-cache` and `CC_NO_CACHE=1` force each selected phase to execute.

## Cache eviction

Three content-addressed caches sit outside the build cache: `incexp/` and
`comptime-hooks/` under `$HOME/.cache/concurrent-c/`, and `comptime/` under
`<out-dir>/ccc-cache/`. Their keys fold input mtimes and toolchain
fingerprints, so an edited source mints a new entry and the previous one is
unreachable.

Each directory is capped. A store trims its directory to a byte budget,
deleting entries in oldest-mtime-first order until the total is under budget.
Budgets default to 1024 MB for `incexp/` and 256 MB for the two dylib caches.
`CC_CACHE_MAX_MB` overrides the budget for every such directory; `0` removes
the cap. `CC_CACHE_EVICT_INTERVAL` sets the minimum seconds between sweeps of
one directory, default 60. A directory that cannot be trimmed reports on
stderr.

Evicting an entry costs a cache miss and never changes build output.

## Build-file discovery

Unless disabled or overridden, the driver looks for `build.cc` beside the first input and in the current directory. If both locations contain different build files, discovery is ambiguous and the build fails. `--build-file PATH` bypasses discovery.

Paths in target declarations are relative to the build-file directory. A target name passed to `ccc build` selects that target. Otherwise selection uses `CC_DEFAULT`, then a target named `default`, then the sole declared target. Multiple targets without one of those choices require an explicit target name.

## Declarative build file

The parser recognizes whitespace-separated directives. It does not evaluate an imperative build program.

### Constants and options

```
CC_CONST <NAME> <EXPR>
CC_OPTION <NAME> <HELP...>
```

`EXPR` is an integer literal, `TARGET_PTR_WIDTH`, or `TARGET_IS_LITTLE_ENDIAN`. Target constants are added first, `CC_CONST` declarations follow, and command-line `-D` bindings override equal names.

`CC_OPTION` contributes help text shown by build help; its value is supplied through `-DNAME[=VALUE]`.

### Targets

```
CC_DEFAULT <TARGET>
CC_TARGET <TARGET> exe <SRC...>
CC_TARGET <TARGET> obj <SRC...>
```

Every target has at least one source. `.ccs` sources are emitted before compilation; `.c` sources compile directly.

The parser attaches these optional properties:

```
CC_TARGET_DEPS <TARGET> <DEP_TARGET...>
CC_TARGET_OUT <TARGET> <BIN_NAME>
CC_TARGET_TARGET <TARGET> <TRIPLE>
CC_TARGET_SYSROOT <TARGET> <PATH>
CC_TARGET_INCLUDE <TARGET> <DIR...>
CC_TARGET_DEFINE <TARGET> <NAME[=VALUE]...>
CC_TARGET_CFLAGS <TARGET> <FLAGS...>
CC_TARGET_LDFLAGS <TARGET> <FLAGS...>
CC_TARGET_LIBS <TARGET> <LIB...>
CC_INSTALL <TARGET> <DEST>
```

Unknown property targets, unknown dependencies, and dependency cycles are errors. Dependencies build first and their object closure is linked into dependents. Dependency link flags and libraries propagate to the final link. Library tokens beginning with `-` pass through; other tokens become `-l<TOKEN>`.

`CC_TARGET_OUT` chooses an executable's name under the binary directory. Target and sysroot properties override the corresponding command-line values for that target. Include and install paths are resolved from the build-file directory or repository root as implemented by the driver.

## Graph inspection

`ccc build list` prints the selected build file, default target, and each target's parsed properties.

`ccc build graph` emits JSON by default. `--format dot` emits Graphviz DOT, and `--graph-out PATH` writes either format to a file. The graph contains target names, kinds, sources, dependency edges, and the default target.

`--dump-comptime` prints the same parsed declarative state before a build step; it does not imply an additional build language.

## Make export

`ccc build export-make` writes `<out-dir>/cc_targets.mk` by default or the path supplied by `-o`.

The fragment defines:

- `CC_BUILD_FILE`, `CC_OUT_DIR`, `CC_INCLUDE`, and `CC_RUNTIME_C`;
- `CC_TARGETS`;
- per-target kind, original sources, generated C paths, compile flags, link flags, and dependencies;
- a `cc-emit-c` helper target that invokes `ccc build --emit-c-only`.

Source and include paths are resolved so an including Makefile can compile the generated C or continue to own its existing compile and link graph.

## Failures

Parse errors identify the build file and line. Missing or ambiguous build files, malformed declarations, unknown targets, dependency cycles, invalid constants, unsafe legacy output arguments, and subprocess failures terminate the requested step with a nonzero status. Failed compiler and linker commands are reported with their return status; `--verbose` prints all invoked commands.
