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

### Build

#### Build the compiler

From repo root:

```bash
make -C cc BUILD=debug TCC_EXT=1 TCC_INC=../third_party/tcc TCC_LIB=../third_party/tcc/libtcc.a
```

Notes:
- `TCC_EXT=1` builds `cc` against the patched `libtcc.a` so CC’s AST hooks are available.
- Today the compiler build uses `make`. The longer-term goal is to move project builds under `cc` itself.
 - Compiler outputs:
   - `out/cc/bin/ccc` (real binary)
   - `cc/bin/ccc` (thin wrapper that execs the real binary)

#### Install the compiler

```bash
make install                     # install to /usr/local
make install PREFIX=/opt/ccc     # install to custom prefix
make install DESTDIR=/tmp/pkg    # staged install (for packaging)
```

Installed layout:
```
$PREFIX/
├── bin/ccc                      # compiler binary
├── include/ccc/                 # headers
│   ├── cc_runtime.cch
│   ├── std/prelude.cch
│   └── vendor/khashl.h
└── lib/ccc/runtime/             # runtime source
    └── concurrent_c.c
```

The compiler auto-detects its include/runtime paths from the binary location. You can also override with `CC_HOME`:
```bash
CC_HOME=/opt/ccc ccc run myfile.ccs
```

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

Concurrent-C detects deadlocks at **compile time** (for guaranteed patterns) and **runtime** (for real deadlocks).

#### Compile-time: 100% guaranteed deadlocks → ERROR

```c
@nursery closing(ch) {
    spawn([rx]() => {
        while (chan_recv(rx, &v) == 0) { ... }  // ❌ ERROR: deadlock
    });
}
// Consumer waits for close, but close happens AFTER children exit
```

Fix: Move consumer **outside** the nursery.

#### Runtime: Real deadlock detection

Enable runtime deadlock detection for fuzzy patterns:

```bash
CC_DEADLOCK_DETECT=0 ./my_program   # disable watchdog (default is enabled)
```

When all threads are blocked with no progress for 10+ seconds:
- Prints detailed diagnostics (which threads are blocked, why)
- Exits with code 124 (like `timeout`)

Configure:
- `CC_DEADLOCK_TIMEOUT=5` — detect after 5 seconds (default: 10)
- `CC_DEADLOCK_ABORT=0` — warn but don't exit

Escape hatch for compile-time check:
- `CC_ALLOW_NURSERY_CLOSING_DRAIN=1`

---

### Tests (preferred)

#### Build + run the test runner

```bash
make test TCC_EXT=1 TCC_INC=third_party/tcc TCC_LIB=../third_party/tcc/libtcc.a
```

Or without `make` (builds only the runner; the compiler still needs to be built):

```bash
./scripts/test.sh
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

---

### Updating TCC (patch workflow)

We keep CC’s TCC modifications as an **uncommitted diff** on top of a pinned upstream commit, and store that diff as a patch file:

- Patch file: `third_party/tcc-patches/0001-cc-ext-hooks.patch`
- Apply patches (idempotent): `make tcc-patch-apply`
- Regenerate patch from current `third_party/tcc` working tree diff: `make tcc-patch-regen`
- One-button check after upgrading TCC: `make tcc-update-check`

Typical upgrade loop:

1. Update `third_party/tcc` to a new upstream commit (clean tree).
2. `make tcc-patch-apply`
3. Fix any conflicts / adjust hooks.
4. `make tcc-patch-regen`
5. `make tcc-update-check`

---

### Roadmap: “no make; cc is the build system”

Near-term steps:
- Extend `ccc build` / `build.cc` so CC projects can be compiled without external makefiles.
- Promote the test runner into a CC-driven build/test workflow (still keeping `tools/cc_test` small and generic).


