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

Produces: `cc/bin/ccc`, `out/cc/bin/shadow_lower`, lowered `out/include/`.

## Day-to-day edit loops

| You changed… | Run | Do **not** run |
|--------------|-----|----------------|
| Your own `.ccs` / app only | `./cc/bin/ccc run …` / rebuild that target | anything under `scripts/` for the compiler |
| Stdlib / runtime (`cc/include/ccc/**`, `cc/runtime/**`) | `make -C cc lower-headers` then rebuild your program (`make cc` also works) | `iterate_shadow_lower.sh` |
| Driver / TCC glue (`cc/src/**`) | `make cc -jN` | snapshot / promote |
| Lowerer faces (`cc/shadow/*.cch` / `shadow_lower.ccs`) | `./scripts/iterate_shadow_lower.sh` | `make all` unless you also need `ccc`/stdlib rebuilt; **no** snapshot until you ship |

`iterate_shadow_lower.sh` rebuilds **`shadow_lower` only** (the native front). `ccc` already invokes that binary.

`./scripts/test.sh` / `make test` refuse a missing, unpatched, or stale TinyCC
(`CC_TCC_EXT_ABI`, `libtcc.a` vs patch files, comptime `@emit`/`#line`
contract). After `git pull`, `test.sh` rebuilds `ccc` if `cc/src` or the
patches are newer than the binaries — you do not have to remember `make cc`
for the suite, but you still do for ad-hoc `./cc/bin/ccc` use.

## Ship a new `shadow_lower` bootstrap seed

Only when a lowerer face change should stick in committed `last-good` (cold clones):

```bash
./scripts/iterate_shadow_lower.sh --ship --smoke   # ccs → snapshot → promote → host-cc from new pin
git add cc/bootstrap/shadow_lower/last-good cc/bootstrap/shadow_lower/MAJOR.MINOR.PATCH-N
# then verify cold (below) before push
```

Do **not** hand-edit `MAJOR.MINOR.PATCH-N/` or `out/include/cc/shadow/`. Details: [bootstrap README](../cc/bootstrap/shadow_lower/README.md). Why pins exist and how they match: [backwards compatibility](backwards_compatibility.md).

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
| Gated | `pigz_cc` (~1.3k lines) on ARM32 TCC self-build | `FORCE_TOOLCHAIN=1 CCC_HOST_CC=tcc ./scripts/pigz_arm32.sh` — caught a TCC-built `shadow_lower` crash fixed in 0.3.4-294 (`for_in_mut` / walk-peel lowering) |
| Host `--full` | redis / pigz_idiomatic / pigz_cc / levenshtein emit | `./scripts/test_shadow_real_projects.sh` (wired from `test.sh --full`) |
| Candidates | Other big `.ccs` worth spot-checking on `shadow_lower --no-cache` | `npm/cc-python/src/cc_python.ccs` (~4.4k), `real_projects/stylo-cc/engine/stylebench_cc.ccs` (~2k), `vscode/cc-lsp/cc_lsp.ccs` (~1.3k), `real_projects/redis/redis_owner.ccs` (~1.2k), `perf/wstore5.ccs` (~1.5k) |
| Pattern smokes | `@for (&… in …)` / zip / grower shrink | `tests/for_in_mut_*`, `tests/for_in_mut_walk_peel_smoke.c` |

Sensitive config: **`CCC_HOST_CC=tcc` on ARM32** — TCC host-compiles bootstrap `shadow_lower.c` (`out/cc-tcc/bin/shadow_lower`). Split `CCC_HOST_CC=cc CCC_BACKEND_CC=tcc` isolates product TCC codegen from lowerer host codegen. See [ilp32-docker.md](ilp32-docker.md#pigz-compare).

## Quick “which binary?”

| Binary | Role | Rebuilt by |
|--------|------|------------|
| `cc/bin/ccc` | Driver (calls `shadow_lower`, then host `cc`) | `make cc` |
| `out/cc/bin/shadow_lower` | Native lowerer | `iterate_shadow_lower.sh` or `make cc` (from `last-good`) |
| `out/include/ccc/**/*.h` | Lowered stdlib | `make -C cc lower-headers` |

Default `make cc` / `make all` builds `shadow_lower` from **committed `last-good`**, not from unpromoted `.cch` edits. Use `iterate_shadow_lower.sh` while iterating on the lowerer.
