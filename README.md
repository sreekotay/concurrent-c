### Concurrent-C (CC) — a strict C11-superset language + toolchain

**Version:** 0.3.0 (`ccc --version`)

Concurrent‑C is a **strict C11-superset preprocessor**: `.ccs` lowers to
plain C and compiles with your **host C compiler**. Structured concurrency,
results, comptime, UFCS, slices/arenas, and a header-first runtime ship with the
language.

**License:** Dual-licensed under [MIT](LICENSE-MIT) or [Apache 2.0](LICENSE-APACHE), at your option.

Toolchain:
- A `ccc` driver (`out/cc/bin/ccc` or wrapper `cc/bin/ccc`) that lowers `.ccs` → C (with `#line` sourcemaps) and then compiles/links with the host C compiler (`cc`, `clang`, …).
- A light/statically linked runtime/stdlib (header-first, prefixed APIs) under `cc/include/ccc` and `cc/runtime`.
- A test runner (`tools/cc_test`) that drives `cc/bin/ccc` end-to-end.
- Vendored TinyCC runs **comptime** (`CONFIG_CC_EXT`); it can also be selected as a host-C backend if desired.

```c
#include <ccc/cc_runtime.cch>      // core runtime
#include <ccc/std/prelude.cch>     // standard library (channels, arena, etc.)
```

```c
#include <ccc/cc_runtime.cch>
#include <stdio.h>

int main(void) {
    @errhandler(CCError e) cc_error_exit(e);
    CCNursery* n = cc_nursery_create(NULL) !> @destroy;
    n->spawn(() => printf("Hello from task A!\n"));
    n->spawn(() => printf("Hello from task B!\n"));
    return 0;
}
```

Fuller example (error policy + compose): [examples/hello.ccs](examples/hello.ccs).

### Docs

- [Getting Started](docs/getting-started.md) — install, first program, concurrency
- [Language Concepts](docs/language-concepts.md) — defer, results, UFCS, slices/arenas, closures
- [Cheatsheet](docs/cheatsheet.md)
- [Performance](https://github.com/sreekotay/concurrent-c/tree/main/perf) — benches, baselines, Neckbeard gauntlets (`perf/`)
- [Docs index](docs/README.md) · [Language spec](spec/concurrent-c-spec-complete.md) · [Stdlib](spec/concurrent-c-stdlib-spec.md)

### Python & JavaScript interop

- [Native modules for Node and Python](docs/js-py-modules.md) — one CC (or C) file → a 26KB `.node` **and** a stable-ABI `.abi3.so`; 40-90ns calls, zero-copy buffers
- [`npm/cc-python`](npm/cc-python) (`concurrent-c-python`) — Python from Node: any module, zero-copy typed arrays, async lanes, N×numpy isolated domains
- [`pypi/cc-node`](pypi/cc-node) (`concurrent-c-node`) — JavaScript and npm packages from Python, same domain model mirrored

### Real programs, measured

Full bench suite and dated receipts: **[Performance](https://github.com/sreekotay/concurrent-c/tree/main/perf)**.

Specimens under [`real_projects/`](real_projects/) hold one bar for
tutorial, idiomatic, and production code — and race their upstreams
(dated baselines with full output live in each folder's `benchmarks/`):

- [**pigz**](real_projects/pigz/) — parallel gzip, feature-complete next
  to upstream C: **191 MB/s vs upstream pigz's 166 MB/s** on 100MB
  (fiber scheduler vs pthreads), plus an idiomatic pipeline file you can
  read in one sitting.
- [**Redis**](real_projects/redis/) — the data-plane subset benched with
  `redis-benchmark` against upstream: parity at one connection, **SET at
  P=16: 2.90M vs 2.38M rps**.
- [**levenshtein**](real_projects/levenshtein/) — a CPython extension in
  CC vs the upstream C extension: **1.4-1.9x faster** across
  distance/ratio/jaro (90ns vs 166ns on short words).
- [**The Neckbeard Challenges**](perf/run_neckbeard_challenges.sh) — six
  cross-language robustness gauntlets (syscall kidnapping, wake storms,
  fairness, named locks) run head-to-head against pthreads, Go, and Zig,
  each sub-benchmark's verdict printed verbatim:
  [latest record](perf/benchmarks/neckbeard_2026_07_26.txt).
- [**CVE locality study**](studies/cve_locality/) — 27 real CVE/shape
  reconstructions under idiomatic CC, pre-registered rules, misses
  counted as backlog: **19 prevented, 6 mitigated, 2 still expressible**
  — every demo builds and runs.

Compiler internals: [architecture](cc/docs/ARCHITECTURE.md), [shadow_lower ops / layout](cc/shadow/README.md), [bootstrap](cc/bootstrap/shadow_lower/README.md), [debug vars](cc/src/diag/DEBUG_VARS.md).

### Install

Homebrew:

```bash
brew tap sreekotay/concurrent-c https://github.com/sreekotay/concurrent-c.git
brew install --HEAD sreekotay/concurrent-c/ccc
```

Or from source (`git`, `make`, a C compiler):

```bash
git clone --filter=blob:none https://github.com/sreekotay/concurrent-c.git
cd concurrent-c
PREFIX="$HOME/.local" ./cc-install.sh
```

`cc-install.sh` also works from anywhere (clones into `$PWD/concurrent-c` or `$CC_REPO_DIR`), builds, installs to `$PREFIX/bin`, and compiles a small program to prove the install. Roughly half a minute on four cores and about 40M of disk.

```bash
./cc-install.sh                                   # from an existing checkout
PREFIX=/opt/ccc ./cc-install.sh                   # custom prefix
CC_REPO_DIR="$HOME/code/ccc" sh ./cc-install.sh   # override clone destination
```

It writes a repo-local `./ccc` launcher and (unless `--no-editor-tools`) installs the Concurrent-C syntax package for VS Code / Cursor plus CodeLLDB when those CLIs are present. Use `--add-to-path` / `--no-add-to-path` for the shell-rc edit.

```bash
ccc run hello.ccs
```

Build outputs: `./out` and `./bin` relative to the working directory (`--out-dir` / `--bin-dir`). Never into `$PREFIX`. Override the install tree with `CC_HOME=/opt/ccc` if needed.

### Hacking on the compiler

**When to run what:** [docs/build-when.md](docs/build-when.md)
(install / first checkout / stdlib / lowerer / ship seed / cold smoke).

```bash
./scripts/fetch_submodules.sh
./scripts/apply_tcc_patches.sh
jobs="$(getconf _NPROCESSORS_ONLN 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)"
(cd third_party/tcc && ./configure --config-cc_ext && make -j"$jobs" libtcc.a tcc libtcc1.a)
make cc -j"$jobs"
./cc/bin/ccc run examples/hello.ccs
```

Optional install from the tree: `make install PREFIX=/opt/ccc` and
`make install-check PREFIX=/opt/ccc`. Before pushing submodule pointer
changes: `make check-submodules`.

Installed layout:

```
$PREFIX/
├── bin/ccc
├── bin/shadow_lower
├── include/ccc/          # .cch and pre-lowered .h
└── lib/ccc/
    ├── runtime/          # pre-lowered runtime (compiled on first use)
    └── tcc/
```

Self-contained install; headers ship pre-lowered.

### Examples and tests

```bash
./cc/bin/ccc build --help
./cc/bin/ccc run examples/hello.ccs
./cc/bin/ccc build run recipe_async
./scripts/test.sh              # fast loop
./scripts/test.sh --full       # complete gate
./scripts/test.sh --filter ufcs
```

Or: `make test TCC_EXT=1 TCC_INC=third_party/tcc TCC_LIB=../third_party/tcc/libtcc.a`.

Test conventions: `tests/README.md`. Build driver / cache / outputs: [build spec](spec/concurrent-c-build.md). Channel close + deadlock patterns: `examples/recipe_channel_pipeline.ccs`, [getting started](docs/getting-started.md).

### Updating TCC

TinyCC is the **comptime** engine (and an optional host-C backend). Patch:
`third_party/tcc-patches/0001-cc-ext-hooks.patch`. Fork branch: `origin/mob`
on `https://github.com/sreekotay/tinycc.git`.

```bash
make tcc-patch-apply
make tcc-patch-regen
make tcc-update-check
```

`third_party/tcc` is usually detached HEAD (parent pins the commit). Push CC
TCC changes to `origin/mob`.

Upgrade loop: update submodule → `tcc-patch-apply` → fix → `tcc-patch-regen` →
push submodule to `origin/mob` → `tcc-update-check`.
