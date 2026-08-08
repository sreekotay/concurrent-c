# Linux ILP32 Docker smoke

Build and run Concurrent-C **natively** inside a 32-bit Linux container so
pointer width, comptime `TARGET_PTR_WIDTH`, and the linked runtime all match
(`sizeof(void*) == 4`).

**When to run this:** before pushing a new `shadow_lower` `last-good`, or when
changing the cold build graph — not on every stdlib edit. See
[build-when.md](build-when.md).

Darwin 32-bit targets are not supported.

## i386 (supported)

Requires Docker with `linux/386` (QEMU on Apple Silicon is fine; slower).

```bash
# One shot (builds image, runs harness)
./scripts/smoke_i386.sh
```

`smoke_i386.sh` mounts the repo at `/src` **read-only** and syncs it into an
anonymous writable `/work` volume before building. Host `cc/`, `out/`, and
`third_party/tcc` build products are not modified.

ILP32 builds define `_FILE_OFFSET_BITS=64` so `readdir` works on Docker volumes
and other filesystems with 64-bit inodes (plain 32-bit `readdir` returns
`EOVERFLOW` there).

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
```

- Dockerfile: `scripts/docker/Dockerfile.arm32` (`arm32v7/debian:bookworm`)
- Platform: `linux/arm/v7`
- ABI: **gnueabihf** / armhf (hard-float), matching TCC’s default arm target

Optional pigz compare (named volume `ccc-arm32-work`):

```bash
./scripts/pigz_arm32.sh
```

Manual equivalent:

```bash
docker build --platform linux/arm/v7 -f scripts/docker/Dockerfile.arm32 -t ccc-arm32 .
docker run --rm --platform linux/arm/v7 \
  -v "$PWD":/src:ro -v /work \
  -e CCC_ILP32_ARCH=arm \
  ccc-arm32 /src/scripts/docker/ilp32_entrypoint.sh
```
