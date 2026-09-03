# Linux ILP32 Docker smoke

Build and run Concurrent-C **natively** inside a 32-bit Linux container so
pointer width, comptime `TARGET_PTR_WIDTH`, and the linked runtime all match
(`sizeof(void*) == 4`).

**When to run this:** before pushing a new `shadow_lower` `last-good`, or when
changing the cold build graph — not on every stdlib edit. See
[build-when.md](build-when.md).

Darwin 32-bit targets are not supported.

## Config

| Variable | Default | Meaning |
|----------|---------|---------|
| `CCC_HOST_CC` | `cc` | Host C compiler used to **build** `ccc` (and TinyCC first). Set to `tcc` for a TinyCC self-build of `ccc`. |
| `CCC_BACKEND_CC` | (matches host) | Host C compiler for `ccc build` / `ccc run` in the curated suite. When `CCC_HOST_CC=tcc` and this is unset, the suite uses the in-tree `third_party/tcc/tcc`. Set to `cc` or `tcc` to force. |
| `BUILD` | `debug` | `debug` or `release` |
| `CCC_ILP32_JOBS` | `nproc` | Parallel make jobs inside the sandbox |

```bash
./scripts/smoke_i386.sh                         # host+backend = system cc (gcc)
CCC_HOST_CC=tcc ./scripts/smoke_i386.sh         # host+backend = TinyCC
./scripts/smoke_arm32.sh                        # same matrix on ARM32
CCC_HOST_CC=tcc ./scripts/smoke_arm32.sh
```

## Latest receipt — 2026-08-30

**Host:** macOS (Darwin 25), arm64, Docker Desktop, QEMU user-mode  
**Seed:** `shadow_lower` last-good **0.3.4-259**  
**Suite:** hello, channel pipeline, fiber spawn, chan task, park/wake, nursery — expect `ELF 32-bit`

| Command | Host CC | Backend | Result |
|---------|---------|---------|--------|
| `./scripts/smoke_i386.sh` | gcc (`cc`) | gcc | **0 failures** |
| `CCC_HOST_CC=tcc ./scripts/smoke_i386.sh` | TinyCC | TinyCC | **0 failures** |
| `./scripts/smoke_arm32.sh` | gcc (`cc`) | gcc | **0 failures** |
| `CCC_HOST_CC=tcc ./scripts/smoke_arm32.sh` | TinyCC | TinyCC | **0 failures** |

## Pigz compare

**Scripts:** `./scripts/pigz_i386.sh`, `./scripts/pigz_arm32.sh` — `pigz.c`, `pigz_idiomatic` (chained dict), `pigz_cc`.  
**Input:** 20 MB Silesia concat; pigz / `pigz_cc` `-p 4`; `pigz_idiomatic` uses runtime cores (`CC_WORKERS` unset).  
**Compile:** original pigz `cc -O3`; `.ccs` via `ccc -O --release`. `CCC_HOST_CC=tcc` builds `ccc` and the product backend with TinyCC; `pigz.c` stays gcc.

All three binaries build and 4 MiB-gunzip on i386 and ARM32, gcc or TinyCC backend.

QEMU user-mode — relative ILP32 only; not comparable to host Darwin or across arches.

| Backend | i386 dump | ARM32 dump |
|---------|-----------|------------|
| gcc (`cc`) | [ilp32_i386_2026_08_30.txt](../real_projects/pigz/benchmarks/ilp32_i386_2026_08_30.txt) | [ilp32_arm32_2026_08_30.txt](../real_projects/pigz/benchmarks/ilp32_arm32_2026_08_30.txt) |
| TinyCC (`FORCE_TOOLCHAIN=1 CCC_HOST_CC=tcc`) | [ilp32_i386_tcc_2026_08_30.txt](../real_projects/pigz/benchmarks/ilp32_i386_tcc_2026_08_30.txt) | [ilp32_arm32_tcc_2026_09_02.txt](../real_projects/pigz/benchmarks/ilp32_arm32_tcc_2026_09_02.txt) |

### gcc backend

#### i386 (linux/386)

20 MB, avg of 2 runs:

| Implementation | Comp (s) | Comp (MB/s) | Ratio | Decomp (s) | Decomp (MB/s) |
|----------------|----------|-------------|-------|------------|---------------|
| pigz (pthread) | 1.355 | 14.1 | 46.0% | 0.506 | 37.7 |
| pigz_idiomatic (chained dict) | 0.696 | 27.4 | 46.0% | — | — |
| pigz_cc | 0.685 | 27.8 | 46.0% | 0.622 | 30.7 |

#### ARM32 (linux/arm/v7, gnueabihf)

20 MB, avg of 2 runs:

| Implementation | Comp (s) | Comp (MB/s) | Ratio | Decomp (s) | Decomp (MB/s) |
|----------------|----------|-------------|-------|------------|---------------|
| pigz (pthread) | 0.665 | 28.7 | 46.0% | 0.288 | 66.2 |
| pigz_idiomatic (chained dict) | 0.482 | 39.6 | 46.0% | — | — |
| pigz_cc | 0.480 | 39.7 | 46.0% | 0.299 | 63.7 |

### TCC backend

`FORCE_TOOLCHAIN=1 CCC_HOST_CC=tcc ./scripts/pigz_i386.sh` / `./scripts/pigz_arm32.sh` — original `pigz.c` still gcc; `ccc` host + product backend = TinyCC.

#### i386 (linux/386)

20 MB, avg of 2 runs:

| Implementation | Comp (s) | Comp (MB/s) | Ratio | Decomp (s) | Decomp (MB/s) |
|----------------|----------|-------------|-------|------------|---------------|
| pigz (pthread) | 1.696 | 11.2 | 46.0% | 0.771 | 24.7 |
| pigz_idiomatic (chained dict) | 1.152 | 16.6 | 46.0% | — | — |
| pigz_cc | 1.346 | 14.2 | 46.0% | 0.952 | 20.0 |

#### ARM32 (linux/arm/v7, gnueabihf)

4 MiB urandom + gunzip for `pigz`, `pigz_idiomatic`, and `pigz_cc`. 20 MB, 1 run:

| Implementation | Comp (s) | Comp (MB/s) | Ratio |
|----------------|----------|-------------|-------|
| pigz (pthread) | 0.786 | 24.3 | 46.0% |
| pigz_idiomatic (chained dict) | 0.503 | 37.9 | 46.0% |

## i386 (supported)

Requires Docker with `linux/386` (QEMU on Apple Silicon is fine; slower).

```bash
# One shot (builds image, runs harness)
./scripts/smoke_i386.sh
CCC_HOST_CC=tcc ./scripts/smoke_i386.sh   # self-build ccc + suite backend=tcc
```

`smoke_i386.sh` mounts the repo at `/src` **read-only** and syncs it into an
anonymous writable `/work` volume before building. Host trees are not modified.

**Arch boundary (how products stay separated):**

- `/work` is the ILP32 sandbox; host Darwin/arm64 objects never share that tree.
- Entrypoint rsync uses `--delete` so a named volume cannot keep stale
  `tests/` (or other sources) removed on the host. Excluded paths stay on
  dest: `/out/`, `/bin/`, `cc/bin/.ccc-bin`, `tools/cc_test` (legacy in-tree
  harness), TinyCC artifacts (`config.mak`, `*.a`/`*.o`, `tcc`), and the
  ARM32 smoke progress files. Omitting the excludes lets a volume pick up
  Darwin `libtcc1.a` (breaks ARM comptime `__aeabi_*`) or a Mach-O
  `tools/cc_test` (`Exec format error` mid-suite).
- Inside a tree, `ccc` further shards host objects under
  `out/.cc-build/host/<fingerprint>/`.
- Prefer the harness at `out/tools/cc_test` (`scripts/test.sh` builds it there)
  so it lives with the rest of `out/` and is naturally excluded from sync.

ILP32 builds define `_FILE_OFFSET_BITS=64` so `readdir` works on Docker volumes
and other filesystems with 64-bit inodes (plain 32-bit `readdir` returns
`EOVERFLOW` there). The toolchain export covers compiling `ccc` itself; `ccc`
also injects `-D_FILE_OFFSET_BITS=64` into Linux ILP32 host compiles of user
programs (and `cc_test` passes the same flag) so runtime `cc_glob` / dir walks
see matches instead of empty results.

Manual equivalent:

```bash
docker build --platform linux/386 -f scripts/docker/Dockerfile.i386 -t ccc-i386 .
docker run --rm --platform linux/386 \
  -v "$PWD":/src:ro -v /work \
  -e CCC_ILP32_ARCH=i386 \
  ccc-i386 /src/scripts/docker/ilp32_entrypoint.sh
```

The harness (inside `/work`):

1. Wipes prior products and rebuilds patched TinyCC + `ccc` from
   `last-good` (true cold tree — same graph as a fresh clone)
2. Checks `ccc` and a linked `hello` binary with `file` — expect `ELF 32-bit`
3. Asserts `libshadow_comptime.a` omits `arena_state.o` (GNU ld ODR)
4. Runs a curated runtime suite (hello, channels, fibers, atomics, nursery)

Use this before promoting a new `shadow_lower` bootstrap seed.

On a native 32-bit Linux host you can run the harness in-tree:

```bash
CCC_ILP32_ARCH=i386 ./scripts/smoke_ilp32.sh
```

That path writes build products into the repo (same as a normal local build).

## ARM32 (supported)

Same harness (`scripts/smoke_ilp32.sh` with `CCC_ILP32_ARCH=arm`) and the same
RO `/src` + `/work` entrypoint pattern:

```bash
./scripts/smoke_arm32.sh
CCC_HOST_CC=tcc ./scripts/smoke_arm32.sh   # self-build ccc + suite backend=tcc
```

- Dockerfile: `scripts/docker/Dockerfile.arm32` (`arm32v7/debian:bookworm`)
- Platform: `linux/arm/v7`
- ABI: **gnueabihf** / armhf (hard-float), matching TCC’s default arm target

Optional pigz compare (named volumes `ccc-ilp32-work` / `ccc-arm32-work`):
`pigz.c`, `pigz_idiomatic` (chained dict), and `pigz_cc`. Numbers:
[Pigz compare](#pigz-compare).

```bash
./scripts/pigz_i386.sh
CCC_HOST_CC=tcc ./scripts/pigz_i386.sh   # ccc backend = TinyCC; pigz.c still gcc
./scripts/pigz_arm32.sh
CCC_HOST_CC=tcc ./scripts/pigz_arm32.sh
```

Manual equivalent:

```bash
docker build --platform linux/arm/v7 -f scripts/docker/Dockerfile.arm32 -t ccc-arm32 .
docker run --rm --platform linux/arm/v7 \
  -v "$PWD":/src:ro -v /work \
  -e CCC_ILP32_ARCH=arm \
  -e CCC_HOST_CC=cc \
  ccc-arm32 /src/scripts/docker/ilp32_entrypoint.sh
```
