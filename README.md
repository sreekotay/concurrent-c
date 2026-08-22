### Concurrent-C (CC) — a strict C11-superset language + toolchain

[Getting Started](docs/getting-started.md) ·
[The CC Way](docs/the-cc-way.md) · [Cheatsheet](docs/cheatsheet.md)

**Version:** 0.3.4-189 (`ccc --version`)

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
#!ccc ccs
#include <ccc/cc_runtime.cch>
#include <stdio.h>

int main(void) {
    @errhandler(CCError e) cc_error_exit(e);
    CCNursery n = cc_nursery_create() !> @destroy;
    n.spawn(() => printf("Hello from task A!\n"));
    n.spawn(() => printf("Hello from task B!\n"));
    return 0;
}
```

Fuller example (error policy + compose): [examples/hello.ccs](examples/hello.ccs).

### Docs

- [Getting Started](docs/getting-started.md) — install, first program, concurrency
- [The CC Way](docs/the-cc-way.md) — idioms for ownership, fallibility, and coordination
- [cccportable](docs/cccportable.md) — ship emitted C so teammates compile with host `cc` only (`portable-install`, `CCCPORTABLE`, file-start pragmas)
- [Language Concepts](docs/language-concepts.md) — defer, results, UFCS, slices/arenas (arena = lifetime; alloc = policy), closures
- [@typehooks / @typeview](docs/typehooks-typeviews.md) — lifecycle hooks, is-a faces, allow-list views
- [Cheatsheet](docs/cheatsheet.md)
- [Language spec](spec/concurrent-c-spec-complete.md)
- [Stdlib](spec/concurrent-c-stdlib-spec.md)
- [Sanitizers / fuzzing](docs/sanitizers.md) — ASan/TSan receipts (runtime, real_projects, bridge)
- [Performance](perf/) — benches, [interop baselines](perf/baselines/), [compiler baseline](perf/compiler_baseline.txt), Neckbeard gauntlets
- [Backwards compatibility](docs/backwards_compatibility.md) — unit headers, version pins, bootstrap seeds
- [Docs index](docs/README.md) 

### Python & JavaScript interop

- [JS / Python interop](docs/js-py-modules.md) — host Python ([`pydemo.shcc`](examples/py/pydemo.shcc)) or JavaScript (`cc_js_new(isolated, &a)`: in-process libnode or N node children, [`jsdemo.shcc`](examples/js/jsdemo.shcc)) from CC, or export one file → `.node` + `.abi3.so` (40-90ns); plus the process bridges below
- [`npm/cc-python`](npm/cc-python) (`concurrent-c-python`) — Python from Node: in-process zero-copy or isolated N×numpy domains; mode costs in [`benchmarks/modes_bench.js`](npm/cc-python/benchmarks/modes_bench.js)
- [`pypi/cc-node`](pypi/cc-node) (`concurrent-c-node`) — JavaScript and npm packages from Python, same domain model mirrored

### Real CC Software

| Project | What it is |
|---------|------------|
| [cc-python](https://github.com/sreekotay/cc-python) | npm package to run Python in JS - any pip module — zero copy numpy etc, full speed, fully featured |
| [cc-node](https://github.com/sreekotay/cc-node) | pip package to run Node at full speed in Python, including first-class Jupyter/notebook support |
| [cctext](https://github.com/sreekotay/cctext) | Open multi-GB text, CSV, code, JSON and navigate and edit in ms. Includes deep-search file explorer (TUI and GUI). Open and edit an 8GB file with <5MB memory usage in under 2ms. |
| [rlsw-cc](https://github.com/sreekotay/rlsw-cc) | Update to raylib software rendering, running at 1.5x to 2x speed |

### Real programs, measured

Full bench suite: **[perf/](perf/)**. Dated JS/Python receipts:
**[perf/baselines/](perf/baselines/)** ([catalog](perf/baselines/README.md)).

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
  CC vs upstream Levenshtein 0.27: ahead on short and long distance/ratio
  rows (e.g. 94ns vs 359ns on short words; see the specimen README).
- [**raytracer**](real_projects/raytracer/) — Shirley weekend final scene
  in C, CC `@parallel for` over scanlines, and Go; same LCG, matching
  checksums (`./real_projects/raytracer/compare.sh`).
- [**The Neckbeard Challenges**](perf/run_neckbeard_challenges.sh) — six
  cross-language robustness gauntlets (syscall kidnapping, wake storms,
  fairness, named locks) run head-to-head against pthreads, Go, and Zig,
  each sub-benchmark's verdict printed verbatim:
  [latest record](perf/benchmarks/neckbeard_2026_08_14.txt).
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

> **VS Code / Cursor syntax.** Search **Concurrent-C** in the Extensions view, or:
>
> ```bash
> code --install-extension sreekotay.concurrent-c-syntax
> cursor --install-extension sreekotay.concurrent-c-syntax
> ```
>
> `./cc-install.sh` also copies the in-tree package. From a clone without the store:
>
> ```bash
> ./vscode/ccs-syntax/install-local.sh --both   # then: Developer → Reload Window
> ```
>
> Open a `.ccs` / `.cch` / `.shcc` — language mode should be **Concurrent-C**. See [getting started](docs/getting-started.md#vs-code--cursor-syntax-required-for-a-good-edit-experience).

```bash
ccc run hello.ccs
```

Build outputs: `./out` and `./bin` relative to the working directory (`--out-dir` / `--bin-dir`). Never into `$PREFIX`. Override the install tree with `CC_HOME=/opt/ccc` if needed.

### Ship emitted C without `ccc`

Authors who have `ccc` (Homebrew or `cc-install.sh` above) drop a host-C
snapshot. Teammates and CI compile that tree with host `cc` and never
invoke the toolchain. It is not `--sysroot`.

```bash
ccc portable-install vendor/cccportable
ccc --emit-c-only --no-line myfile.ccs -o include/generated/myfile.c
ccc --cccportable vendor/cccportable --print-cflags
ccc --cccportable vendor/cccportable --print-libs
# same print helpers: CCCPORTABLE=vendor/cccportable ccc --print-cflags
```

Paste the flags into the consumer Makefile once. Do not write
`$(ccc --print-cflags)` there.

Independent of the snapshot: `#pragma(@prelude) off` skips the automatic
prolog; `#pragma(@linenumbers) off` / `--no-line` omit `#line`. A
comptime-only script can emit with `--no-runtime` and need no portable
tree.

[cccportable](docs/cccportable.md) — install rules, one `-I`, consumer
recipe (`-DCC_ENABLE_ASYNC`). Host-cc of `--emit-c-only` output must not
define `CC_PARSER_MODE` (that is `ccc`'s parse session). Spec:
[§1.8 file-start pragmas](spec/concurrent-c-spec-complete.md#18-file-start-pragmas),
[build](spec/concurrent-c-build.md).

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

Needs the hacking checkout (patched TinyCC), not just an installed `ccc`.
`./scripts/test.sh` / `make test` refuse an unpatched or stale tree and
rebuild `ccc` when sources are newer than the binaries.

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

**Linux ILP32 (last verified 2026-08-19).** Docker cold smokes on i386 and
`linux/arm/v7` (gnueabihf / armhf, QEMU on Apple Silicon), `shadow_lower`
last-good **0.3.3-173**: curated suite green for host+backend **gcc** and
**TinyCC** (`./scripts/smoke_i386.sh`, `./scripts/smoke_arm32.sh`, and the
same with `CCC_HOST_CC=tcc`). pigz compare on i386 and ARM32 (**0.3.3-174**):
`pigz_wait` / `pigz_cc` / original all ELF 32-bit and gunzip-clean;
numbers in [docs/ilp32-docker.md](docs/ilp32-docker.md). Earlier full
`cc_test` on ARM32: **787 / 787** (`0.3.2-108`).

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
