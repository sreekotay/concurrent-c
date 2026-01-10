### Concurrent-C (CC) — prototype compiler + runtime

This repo is an early prototype of **Concurrent‑C (CC)** built by extending **Tiny C Compiler (TCC)** with small, upstream-friendly hooks (guarded behind `CONFIG_CC_EXT`).

At this stage CC is a “C-with-extensions” toolchain:
- A `ccc` compiler (`out/cc/bin/ccc` or wrapper `cc/bin/ccc`) that lowers `.ccs` → C (with `#line` sourcemaps) and then optionally compiles/links using the host C compiler.
- A small runtime/stdlib (header-first, prefixed APIs) under `cc/include` and `cc/runtime`.
- A test runner (`tools/cc_test`) that drives `cc/bin/ccc` end-to-end.

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

---

### Run examples

```bash
make example TCC_EXT=1 TCC_INC=third_party/tcc TCC_LIB=../third_party/tcc/libtcc.a
```

---

### `ccc build` recipes (copy/paste)

All commands below assume you’re in the repo root and have built the compiler (`make -C cc ...`).

#### Build + run a single file

```bash
./cc/bin/ccc build run examples/hello.ccs
```

Pass args to the produced binary:

```bash
./cc/bin/ccc build run examples/hello.ccs -- --help
```

#### Emit generated C only (let another build system compile it)

```bash
./cc/bin/ccc --emit-c-only examples/hello.ccs
ls -l out/hello.c
```

#### Use `build.cc` (targets + options + consts)

```bash
./cc/bin/ccc build --help --build-file examples/build_graph/build.cc
./cc/bin/ccc build --build-file examples/build_graph/build.cc --summary     # builds CC_DEFAULT
./cc/bin/ccc build multi --build-file examples/build_graph/build.cc --summary
```

#### Mixed CC + C sources

`ccc build` can link a target composed of both `.ccs` and `.c` translation units. Headers (`.h` / `.cch`) are included normally (not built as standalone artifacts).

```bash
./cc/bin/ccc build --build-file examples/mixed_c/build.cc --summary
./bin/hello
```

Set comptime integer consts:

```bash
./cc/bin/ccc build --dump-consts --dry-run -DDEBUG -DNUM_WORKERS=8 examples/hello.ccs
```

#### Override output directories

```bash
CC_OUT_DIR=out2 CC_BIN_DIR=bin2 ./cc/bin/ccc build run examples/hello.ccs --summary
```

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


