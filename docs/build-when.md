# When to run what (compiler / stdlib builds)

Pick the row that matches what you are doing. Do not stack unrelated steps.

## Install (use the language)

| Goal | Command |
|------|---------|
| Install `ccc` (Homebrew) | `brew install --HEAD sreekotay/concurrent-c/ccc` |
| Install from source | `PREFIX="$HOME/.local" ./cc-install.sh` |

Then: `ccc run hello.ccs`. You do **not** need the hacking scripts below.

## First checkout build (hack on this repo)

Run once after clone (or when `third_party/tcc` is missing / unclean):

```bash
./scripts/fetch_submodules.sh
./scripts/apply_tcc_patches.sh
jobs="$(getconf _NPROCESSORS_ONLN 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)"
(cd third_party/tcc && ./configure --config-cc_ext && make -j"$jobs" libtcc.a tcc libtcc1.a)
make cc -j"$jobs"
./cc/bin/ccc run examples/hello.ccs
```

Produces: `cc/bin/ccc`, the lowerer's tools `out/cc/bin/*_cc` from their seed, lowered `out/include/`.

## Day-to-day edit loops

| You changed… | Run | Do **not** run |
|--------------|-----|----------------|
| Your own `.ccs` / app only | `./cc/bin/ccc run …` / rebuild that target | anything under `scripts/` for the compiler |
| Stdlib / runtime (`cc/include/ccc/**`, `cc/runtime/**`) | `make -C cc lower-headers` then rebuild your program (`make cc` also works) | `lower-cc` |
| Driver / TCC glue (`cc/src/**`) | `make cc -jN` | snapshot / promote |
| The lowerer (`cc/lower/*.cch` / `*.ccs`) | `CC_NO_CACHE=1 make -C cc lower-cc`, then `./scripts/lowerer_selfhost.sh`; `./scripts/ship_seed.sh --promote` when the change should reach a fresh checkout | a snapshot before the selfhost gate holds |
| The comptime seam (`cc/src/comptime/**`, `cc/src/preprocess/**`) | `make -C cc` **then** `CC_NO_CACHE=1 make -C cc lower-cc` — `cclower_cc` links `libshadow_comptime.a`, so a stale archive is a stale lowerer | linking the lowerer against a `libshadow_comptime.a` the driver did not just rebuild |

`lowerer_selfhost.sh` builds the lowerer from what it lowered its own
sources to and has that build lower them again: the `.c` and every `.h`
must come back byte for byte. It runs in about a minute and catches what
the corpus cannot — a lowering that is wrong only about the lowerer.
`make -C cc lower-cc` rebuilds the four tools from source with the tools in
hand, so a lowerer edit is live in `ccc` as soon as it links.

`./scripts/test.sh` / `make test` refuse a missing, unpatched, or stale TinyCC
(`CC_TCC_EXT_ABI`, `libtcc.a` vs patch files, comptime `@emit`/`#line`
contract). After `git pull`, `test.sh` rebuilds `ccc` if `cc/src` or the
patches are newer than the binaries — you do not have to remember `make cc`
for the suite, but you still do for ad-hoc `./cc/bin/ccc` use.

## Ship a new bootstrap seed

Only when a lowerer change should stick in committed `last-good` (cold clones):

```bash
./scripts/lowerer_selfhost.sh                      # the freeze check: fixed point
./scripts/ship_seed.sh --promote                   # snapshot → promote → last-good
git add cc/bootstrap/lowerer/last-good cc/bootstrap/lowerer/MAJOR.MINOR.PATCH-SEED
# then verify cold (below) before push
```

Do **not** hand-edit a pin or `out/.cc-build/clean/cc/lower/`. Details: [bootstrap README](../cc/bootstrap/lowerer/README.md). Why pins exist and how they match: [backwards compatibility](backwards_compatibility.md).

## Verify cold / second platform

Run these when you changed the **build graph**, **bootstrap seed**, or before pushing a promote — not on every stdlib edit.

| Goal | Command | When |
|------|---------|------|
| Wipe local `out/` and rebuild from `last-good` | `./scripts/smoke_bootstrap_fresh.sh` | after seed promote, or “does cold make still work?” |
| Clean Linux i386 (Docker) | `./scripts/smoke_i386.sh` | before pushing a new `last-good`; catches GNU ld / Darwin-only seeds |
| Same, host+backend = TinyCC | `CCC_HOST_CC=tcc ./scripts/smoke_i386.sh` | Linux / Docker ILP32 |
| Clean Linux ARM32 (Docker) | `./scripts/smoke_arm32.sh` | same gate on `linux/arm/v7` (gnueabihf) |
| Same, host+backend = TinyCC | `CCC_HOST_CC=tcc ./scripts/smoke_arm32.sh` | Linux / Docker ILP32 |

`smoke_i386.sh` / `smoke_arm32.sh` mount the repo **read-only** and build in `/work` — they do not replace your host `out/`. Env and latest receipt: [ilp32-docker.md](ilp32-docker.md).

Optional **large-TU emit stress** (after lowerer changes that touch stmt / walk / UFCS emit, or before promoting when the curated smoke passed but pigz-scale risk remains):

| Tier | What | Command / note |
|------|------|----------------|
| Gated | `pigz_cc` (~1.3k lines) on ARM32 TCC self-build | `FORCE_TOOLCHAIN=1 CCC_HOST_CC=tcc ./scripts/pigz_arm32.sh` |
| Candidates | Other big `.ccs` worth spot-checking with `ccc build --no-cache` | `npm/cc-python/src/cc_python.ccs` (~4.4k), `real_projects/stylo-cc/engine/stylebench_cc.ccs` (~2k), `vscode/cc-lsp/cc_lsp.ccs` (~1.3k), `real_projects/redis/redis_owner.ccs` (~1.2k), `perf/wstore5.ccs` (~1.5k) |
| Pattern smokes | `@for (&… in …)` / zip / grower shrink | `tests/for_in_mut_*`, `tests/for_in_mut_walk_peel_smoke.c` |

Sensitive config: **`CCC_HOST_CC=tcc` on ARM32** — TCC host-compiles the bootstrap seed. Split `CCC_HOST_CC=cc CCC_BACKEND_CC=tcc` isolates product TCC codegen from lowerer host codegen. See [ilp32-docker.md](ilp32-docker.md#pigz-compare).

## Quick “which binary?”

| Binary | Role | Rebuilt by |
|--------|------|------------|
| `cc/bin/ccc` | Driver (runs the lowerer, then host `cc` and the link) | `make cc` |
| `out/cc/bin/cclower_cc` and `cclex_cc`, `ccparse_cc`, `ccindex_cc` | The lowerer's tools | `make -C cc lower-cc` from source, or `make cc` from `last-good` |
| `out/include/ccc/**/*.h` | Lowered stdlib | `make -C cc lower-headers` |

Default `make cc` / `make all` builds the tools from **committed `last-good`**, not from unpromoted `cc/lower` edits. Use `make -C cc lower-cc` while iterating on the lowerer.
