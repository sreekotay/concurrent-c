### Concurrent-C (CC) — prototype compiler + runtime

This repo is an early prototype of **Concurrent‑C (CC)** built by extending **Tiny C Compiler (TCC)** with small, upstream-friendly hooks (guarded behind `CONFIG_CC_EXT`).

**License:** Dual-licensed under [MIT](LICENSE-MIT) or [Apache 2.0](LICENSE-APACHE), at your option.

At this stage CC is a “C-with-extensions” toolchain:
- A `ccc` compiler (`out/cc/bin/ccc` or wrapper `cc/bin/ccc`) that lowers `.ccs` → C (with `#line` sourcemaps) and then optionally compiles/links using the host C compiler.
- A light/statically linked runtime/stdlib (header-first, prefixed APIs) under `cc/include/ccc` and `cc/runtime`.
- A test runner (`tools/cc_test`) that drives `cc/bin/ccc` end-to-end.

**Headers use the `<ccc/...>` namespace** to avoid collisions with your project:
```c
#include <ccc/cc_runtime.cch>      // core runtime
#include <ccc/std/prelude.cch>     // standard library (channels, arena, etc.)
```

---

### Type Family: Arrays, Slices, Channels

Concurrent-C uses `[...]` syntax for all container types:

| Type | Meaning |
|------|---------|
| `T[n]` | Fixed array of `n` elements |
| `T[:]` | Slice — variable-length view |
| `T[~n >]` | Channel send handle, buffer size `n` |
| `T[~n <]` | Channel receive handle, buffer size `n` |

The `~` means "channel" (a queue, not inline storage). The `>/<` indicate direction.

---

### Generic Containers: Vec and Map

CC provides arena-backed generic containers with UFCS method syntax:

```c
#include <ccc/std/prelude.cch>

int main(void) {
    CCArena arena = cc_arena_heap(kilobytes(4));
    
    // CCVec::[T] - dynamic array
    CCVec::[int] numbers = cc_vec_new::[int](&arena);
    numbers.push(10);
    numbers.push(20);
    numbers.push(30);
    
    int* val = numbers.get(1);   // Nullable pointer: &20
    int* oob = numbers.get(100); // Nullable pointer: NULL

    // Map::[K, V] - hash table
    Map::[int, char*] names = map_new::[int, char*](&arena);
    names.insert(1, "Alice");
    names.insert(2, "Bob");

    char** name = names.get(1);  // Nullable pointer: &"Alice"
    
    cc_arena_free(&arena);
    return 0;
}
```

The `Name::[args]` instantiation and `v.method(...)` UFCS syntax is lowered by the compiler:
- `CCVec::[int]` → `CCVec_int`
- `cc_vec_new::[int](&arena)` → `CCVec_int_init(&arena, CC_VEC_INITIAL_CAP)`
- `v.push(x)` → `CCVec_int_push(&v, x)`

The angle-bracket spellings (`Vec<T>`, `vec_new<T>`) are retired; `Name::[args]` is the
single instantiation surface for both built-in containers and user generic factories.

A registered factory can also be called in member position. `recv.member::[T](args)`
resolves to the factory `<snake(RecvType)>_<member>` and passes the receiver as its
first argument, so these are two spellings of one call and lower to one monomorph:

```c
py.expose::[Counter]("counter", &seed) !>;        // member spelling
py_expose::[Counter](&py, "counter", &seed) !>;   // free-name spelling
```

See the [stdlib spec](spec/concurrent-c-stdlib-spec.md) for full API documentation.

---

### Compiler architecture & cleanup status

The compiler pipeline was refactored in 2026 (M0–M5.5): diagnostics core, shared prep API, preprocess reparse modes, TCC ext API boundary, and experimental macro-syntax hooks.

- **Architecture (WHY the pipeline looks this way — read before proposing restructures):** [cc/docs/ARCHITECTURE.md](cc/docs/ARCHITECTURE.md)
- **Status (what shipped / what's next):** [cc/docs/COMPILER_CLEANUP_STATUS.md](cc/docs/COMPILER_CLEANUP_STATUS.md)
- **Debug flags:** `CC_DEBUG_REPARSE`, `CC_DEBUG_DIAG`, `CC_DEBUG_LOWER`, `CC_DEBUG_SPANS`; CLI `--show-lowered=<phase>`
- **Details:** [cc/src/diag/DEBUG_VARS.md](cc/src/diag/DEBUG_VARS.md)

---

### Install

Requires `git`, `make`, and a C compiler.

```bash
git clone --filter=blob:none https://github.com/sreekotay/concurrent-c.git
cd concurrent-c
PREFIX="$HOME/.local" ./cc-install.sh
```

One pass: fetch submodules, patch and build TinyCC, build `ccc` and
`shadow_lower` (default serdes front), install both to `$PREFIX/bin`, then
compile a program against the installed prefix to prove it works. Roughly half
a minute on four cores and about 40M of disk.

`cc-install.sh` works in two modes:
- Run from the repo root, it uses the current checkout.
- Run anywhere else, it clones into `$PWD/concurrent-c` (or `$CC_REPO_DIR`) first.

```bash
./cc-install.sh                                   # from an existing checkout
PREFIX=/opt/ccc sh ./cc-install.sh                # from anywhere, custom prefix
CC_REPO_DIR="$HOME/code/ccc" sh ./cc-install.sh   # override clone destination
```

It also writes a repo-local `./ccc` launcher for that checkout, installs the
Concurrent-C syntax package for VS Code and Cursor, and installs the CodeLLDB
debugger extension when the `code` / `cursor` CLIs are present. Pass
`--no-editor-tools` to skip the editor setup, `--add-to-path` /
`--no-add-to-path` to control the shell-rc edit.

Homebrew:
```bash
brew tap sreekotay/concurrent-c https://github.com/sreekotay/concurrent-c.git
brew install sreekotay/concurrent-c/ccc
```

### Build

#### Build the compiler

From repo root:

```bash
./scripts/fetch_submodules.sh
./scripts/apply_tcc_patches.sh
jobs="$(getconf _NPROCESSORS_ONLN 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)"
(cd third_party/tcc && ./configure --config-cc_ext && make -j"$jobs")
make cc -j"$jobs"                    # or: make cc BUILD=debug -j"$jobs"
```

Notes:
- The build links against the patched `libtcc.a` so CC's AST hooks are available.
- `scripts/fetch_submodules.sh` fetches only what the build needs: all of
  `third_party/tcc`, and `third_party/zmij` sparse (`zmij.c` / `zmij-c.h`).
  `third_party/liblfds` is an optional channel backend — pass
  `--with-liblfds` to include it.
- Today the compiler build uses `make`. The longer-term goal is to move project builds under `cc` itself.
- Compiler outputs:
  - `cc/bin/.ccc-bin` (real binary)
  - `cc/bin/ccc` (wrapper that refreshes lowered headers, then execs the real binary)
  - `out/cc/bin/shadow_lower` (native serdes lowerer; also copied to `cc/bin/`)
- `shadow_lower` is host-cc'd from a committed bootstrap snapshot
  (`cc/bootstrap/shadow_lower/$(cat last-good)/` — lowered C checked into git).
  Source of truth remains `examples/serdes/c/*.ccs`; promote a new snapshot after
  meaningful serdes changes (`scripts/snapshot_shadow_lower.sh` +
  `scripts/promote_shadow_bootstrap.sh`).
- Host-TCC self-build: on Linux you can compile `ccc` itself with the just-built
  TinyCC (`CCC_HOST_CC=tcc ./scripts/smoke_i386.sh`, or the same env inside an
  ILP32 container). Darwin cannot — host TCC emits ELF while the in-tree
  `libtcc.a` is Mach-O. TinyCC also does not honor `__attribute__((weak))`;
  production `shadow_lower` omits weak emit-plan stubs when
  `SHADOW_HAVE_LIBTCC` is set so the link stays clean.

#### Install to a prefix

```bash
make install                     # install to /usr/local
make install PREFIX=/opt/ccc     # install to custom prefix
make install DESTDIR=/tmp/pkg    # staged install (for packaging)
make install-check PREFIX=/opt/ccc   # compile a program against the install
```

Installed layout:
```
$PREFIX/
├── bin/ccc                      # compiler driver (default front: serdes)
├── bin/shadow_lower             # serdes lowerer (required beside ccc)
├── include/ccc/                 # headers, as .cch and pre-lowered .h
│   ├── cc_runtime.cch, cc_runtime.h, ...
│   ├── std/                     # prelude.cch, vec.cch, map.cch, ...
│   ├── script/                  # stdio.cch, file.cch, sh.cch, ...
│   └── vendor/
└── lib/ccc/
    ├── runtime/                 # pre-lowered runtime source (compiled on first use)
    │   ├── concurrent_c.c
    │   ├── float_format_zmij.c
    │   └── vendor/zmij.c
    └── tcc/                     # TinyCC builtin headers + libtcc1.a
```

An installed tree is self-contained — it never reads back into a checkout.
Headers ship pre-lowered because lowering is a build-tree step. The driver
resolves includes, runtime sources, and `shadow_lower` from the prefix.
`ccc` writes build outputs to `./out` and `./bin` relative to the working
directory (use `--out-dir` / `--bin-dir` to redirect), never into `$PREFIX`.

The compiler auto-detects its include/runtime paths from the binary location. You can also override with `CC_HOME`:
```bash
CC_HOME=/opt/ccc ccc run myfile.ccs
```

Before pushing a commit that changes submodule pointers, run `make check-submodules`
or `npm run check:submodules`. This catches unreachable local-only submodule
commits, especially `third_party/tcc`, before they break fresh clones.

**Homebrew tap:** a *tap* is a Homebrew source of formulas. Adding this repo as a tap lets anyone install the head version without cloning:
```bash
brew tap sreekotay/concurrent-c https://github.com/sreekotay/concurrent-c.git
brew install sreekotay/concurrent-c/ccc
```
After the tap is added, later updates are `brew update && brew upgrade sreekotay/concurrent-c/ccc`. Plain `brew install ccc` would only work if the formula were in Homebrew’s main repo (homebrew-core); this tap keeps the formula in your repo instead.

---

### Run examples, stress tests, and benchmarks

The repo has a root `build.cc` with targets for all examples, stress tests, and perf benchmarks:

```bash
# List all targets
./cc/bin/ccc build --help

# Run examples
./cc/bin/ccc build run hello              # hello world
./cc/bin/ccc build run recipe_async       # async/await recipe
./cc/bin/ccc build run recipe_pipeline    # channel pipeline

# Run stress tests
./cc/bin/ccc build run stress_spawn       # spawn storm (1000 tasks)
./cc/bin/ccc build run stress_channel     # channel flood

# Run benchmarks
./cc/bin/ccc build run perf_channel       # channel throughput
./cc/bin/ccc build run perf_async         # async overhead
```

Or run a single file directly:

```bash
./cc/bin/ccc run examples/hello.ccs
./cc/bin/ccc run stress/spawn_storm.ccs
```

---

### `ccc build` recipes (copy/paste)

All commands below assume you’re in the repo root and have built the compiler (`make -C cc ...`).

#### Pass args to the produced binary

```bash
./cc/bin/ccc run examples/hello.ccs -- --help
```

#### Emit generated C only (let another build system compile it)

```bash
./cc/bin/ccc --emit-c-only examples/hello.ccs
ls -l out/hello.c
```

#### Custom `build.cc` files

Multi-file projects use `build.cc` to define targets. The root `build.cc` is auto-discovered; use `--build-file` for others:

```bash
./cc/bin/ccc build --help --build-file examples/build_graph/build.cc
./cc/bin/ccc build --build-file examples/build_graph/build.cc --summary
./cc/bin/ccc build multi --build-file examples/build_graph/build.cc
```

#### Mixed CC + C sources

`ccc build` can link a target composed of both `.ccs` and `.c` translation units. Headers (`.h` / `.cch`) are included normally (not built as standalone artifacts).

```bash
./cc/bin/ccc build --build-file examples/mixed_c/build.cc --summary
./bin/hello
```

You can also attach target-local build settings in `build.cc`:
- `CC_TARGET_INCLUDE <target> <dir...>`
- `CC_TARGET_DEFINE <target> <NAME[=VALUE]...>`
- `CC_TARGET_CFLAGS <target> <flags...>`
- `CC_TARGET_LDFLAGS <target> <flags...>`
- `CC_TARGET_LIBS <target> <lib...>`
- `CC_TARGET_DEPS <target> <dep_target...>`
- `CC_TARGET_OUT <target> <bin_name>`
- `CC_TARGET_TARGET <target> <triple>`
- `CC_TARGET_SYSROOT <target> <path>`
- `CC_INSTALL <target> <dest>`

Target kinds:
- `CC_TARGET <name> exe <src...>`
- `CC_TARGET <name> obj <src...>`

Set comptime integer consts:

```bash
./cc/bin/ccc build --dump-consts --dry-run -DDEBUG -DNUM_WORKERS=8 examples/hello.ccs
```

#### Override output directories

```bash
CC_OUT_DIR=out2 CC_BIN_DIR=bin2 ./cc/bin/ccc build run examples/hello.ccs --summary
```

---

### Deadlock Detection

Concurrent-C detects real deadlocks at **runtime**. Compile-time coverage is
deliberately narrow — two specific checks, nothing more:

- The retired `@closing(...)` construct (the old guaranteed-deadlock foot-gun)
  is a hard compile error with a migration hint.
- `cc_block_on` of a function that has channel ops in a loop and is not marked
  `@nonblocking` produces a "may deadlock" **warning** (heuristic, not a proof).

There is **no** general compile-time deadlock analysis. In particular, the
common foot-gun below compiles cleanly and is only caught when it actually
deadlocks at runtime:

```c
CCNursery* producer = cc_nursery_create(NULL) !> @destroy;
(void)producer->close_on(tx);          // tx closes once producer's tasks finish

producer->spawn(() => [rx] {
    int v;
    while (cc_io_avail(rx.recv(&v))) { /* ... */ }  // deadlocks at RUNTIME
});
// Consumer waits for close, but close happens AFTER the owning nursery's tasks exit
```

Fix: Move the consumer **outside** the owning nursery scope (see `examples/recipe_channel_pipeline.ccs`).

#### Runtime: Real deadlock detection

Concurrent-C includes a runtime deadlock detector that triggers when all worker threads are blocked and no progress is being made.

When a deadlock is detected:
- Prints detailed diagnostics (which fibers are parked, why, and where)
- Exits with code 124 (like `timeout`) by default

Configure via environment variables:
- `CC_DEADLOCK_ABORT=0` — warn but don't exit (continues hanging)
- `CC_WORKERS=n` — set number of worker threads (default: CPU count)

Opt-in runtime guard for the closing-nursery drain pattern:
- `CC_NURSERY_CLOSING_RUNTIME_GUARD=1` — instead of deadlocking, a recv that would wait forever on a channel whose `close_on` owner is the current nursery fails with `EDEADLK` (pinned by `tests/nursery_closing_deadlock_runtime_guard_smoke.ccs`)

---

### Tests (preferred)

#### Build + run the test runner

```bash
make test TCC_EXT=1 TCC_INC=third_party/tcc TCC_LIB=../third_party/tcc/libtcc.a
```

Or without `make` (builds only the runner; the compiler still needs to be built):

```bash
./scripts/test.sh              # default: fast loop (skips redis preambles + stress)
./scripts/test.sh --full       # complete gate
./scripts/test.sh --filter ufcs
./scripts/test.sh --list
```

Test conventions are documented in `tests/README.md`.

---

### Output layout (defaults)

- **Generated C + objects**: `out/` (e.g. `out/foo.c`, `out/foo.o`)
- **Linked executables**: `bin/` (e.g. `bin/foo`)

Override:
- `--out-dir DIR` or `CC_OUT_DIR=DIR`
- `--bin-dir DIR` or `CC_BIN_DIR=DIR`

### Incremental cache

`ccc build` maintains a lightweight incremental cache under `out/.cc-build/` to skip re-emitting C, recompiling objects, and relinking when inputs/flags haven’t changed.

- Disable: `--no-cache` or `CC_NO_CACHE=1`

The content-addressed caches (`~/.cache/concurrent-c/`, `out/ccc-cache/`) are
capped and trim themselves oldest-first; `CC_CACHE_MAX_MB` sets the
per-directory budget and `make -C cc clean-cache` empties them.

---

### Updating TCC (patch workflow)

We keep CC’s TCC modifications in a fetchable forked `third_party/tcc` history and snapshot that delta as a patch against a mirrored upstream base:

- Patch file: `third_party/tcc-patches/0001-cc-ext-hooks.patch`
- Submodule fork: `https://github.com/sreekotay/tinycc.git`
- Push branch for CC TCC changes: `origin/mob`
- Apply patches (idempotent): `make tcc-patch-apply`
- Regenerate patch from current `third_party/tcc` working tree diff: `make tcc-patch-regen`
- One-button check after upgrading TCC: `make tcc-update-check`

Important: `third_party/tcc` is usually checked out in detached-HEAD state because the parent repo pins a specific submodule commit. If you make changes there, do not guess where to push them. Push the resulting commit to the fork branch `origin/mob`, or first check out a local branch that tracks `origin/mob`.

Typical upgrade loop:

1. Update `third_party/tcc` to a new upstream commit (clean tree).
2. `make tcc-patch-apply`
3. Fix any conflicts / adjust hooks.
4. `make tcc-patch-regen`
5. Push the submodule commit to `origin/mob`
6. `make tcc-update-check`

---

### Roadmap: “no make; cc is the build system”

Near-term steps:
- Extend `ccc build` / `build.cc` so CC projects can be compiled without external makefiles.
- Promote the test runner into a CC-driven build/test workflow (still keeping `tools/cc_test` small and generic).


