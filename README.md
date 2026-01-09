### Concurrent-C (CC) — prototype compiler + runtime

This repo is an early prototype of **Concurrent‑C (CC)** built by extending **Tiny C Compiler (TCC)** with small, upstream-friendly hooks (guarded behind `CONFIG_CC_EXT`).

At this stage CC is a “C-with-extensions” toolchain:
- A `cc` compiler (`cc/bin/cc`) that lowers `.cc` → C (with `#line` sourcemaps) and then optionally compiles/links using the host C compiler.
- A small runtime/stdlib (header-first, prefixed APIs) under `cc/include` and `cc/runtime`.
- A test runner (`tools/cc_test`) that drives `cc/bin/cc` end-to-end.

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

---

### Run examples

```bash
make example TCC_EXT=1 TCC_INC=third_party/tcc TCC_LIB=../third_party/tcc/libtcc.a
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
- Extend `cc build` / `build.cc` so CC projects can be compiled without external makefiles.
- Promote the test runner into a CC-driven build/test workflow (still keeping `tools/cc_test` small and generic).


