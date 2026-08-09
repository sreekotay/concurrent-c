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

## Ship a new `shadow_lower` bootstrap seed

Only when a lowerer face change should stick in committed `last-good` (cold clones):

```bash
./scripts/iterate_shadow_lower.sh --ship --smoke   # ccs → snapshot → promote → host-cc from new vN
git add cc/bootstrap/shadow_lower/last-good cc/bootstrap/shadow_lower/vN
# then verify cold (below) before push
```

Do **not** hand-edit `vN/` or `out/include/cc/shadow/`. Details: [bootstrap README](../cc/bootstrap/shadow_lower/README.md).

## Verify cold / second platform

Run these when you changed the **build graph**, **bootstrap seed**, or before pushing a promote — not on every stdlib edit.

| Goal | Command | When |
|------|---------|------|
| Wipe local `out/` and rebuild from `last-good` | `./scripts/smoke_bootstrap_fresh.sh` | after seed promote, or “does cold make still work?” |
| Clean Linux i386 (Docker) | `./scripts/smoke_i386.sh` | before pushing a new `last-good`; catches GNU ld / Darwin-only seeds |
| Same, host-cc = TinyCC | `CCC_HOST_CC=tcc ./scripts/smoke_i386.sh` | Linux only |

`smoke_i386.sh` mounts the repo **read-only** and builds in `/work` — it does not replace your host `out/`.

## Quick “which binary?”

| Binary | Role | Rebuilt by |
|--------|------|------------|
| `cc/bin/ccc` | Driver (calls `shadow_lower`, then host `cc`) | `make cc` |
| `out/cc/bin/shadow_lower` | Native lowerer | `iterate_shadow_lower.sh` or `make cc` (from `last-good`) |
| `out/include/ccc/**/*.h` | Lowered stdlib | `make -C cc lower-headers` |

Default `make cc` / `make all` builds `shadow_lower` from **committed `last-good`**, not from unpromoted `.cch` edits. Use `iterate_shadow_lower.sh` while iterating on the lowerer.
