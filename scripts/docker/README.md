# ILP32 Docker smoke images

Native 32-bit Linux containers for Concurrent-C runtime smokes. The compiler
and runtime are built *inside* the container so `TARGET_PTR_WIDTH` matches
`sizeof(void*)`.

| Arch | Dockerfile | Platform | Status |
|------|------------|----------|--------|
| i386 | `Dockerfile.i386` | `linux/386` | supported |
| ARM32 | `Dockerfile.arm32` | `linux/arm/v7` (gnueabihf) | supported |
| Ubuntu 24.04 amd64 | `Dockerfile.ubuntu24` | `linux/amd64` | emit / raytext CI parity |

**ILP32 layout:** host repo → `/src` (read-only) → rsync → `/work` (writable sandbox) →
[`ilp32_entrypoint.sh`](ilp32_entrypoint.sh) → [`../smoke_ilp32.sh`](../smoke_ilp32.sh).

Host wrappers: [`../smoke_i386.sh`](../smoke_i386.sh), [`../smoke_arm32.sh`](../smoke_arm32.sh).

**Ubuntu 24.04 (linux/amd64):** reproduces raytext / cctext dist and `@for` + `.sub()` emit on Linux from a Mac host. Host wrapper: [`../smoke_ubuntu24.sh`](../smoke_ubuntu24.sh).

```bash
# Local concurrent-c checkout + optional raytext tree
RAYTEXT_ROOT=/path/to/raytext ./scripts/smoke_ubuntu24.sh

# Same as GitHub Actions (clone main, no local edits)
./scripts/smoke_ubuntu24.sh --clone-main

# Pin a commit
./scripts/smoke_ubuntu24.sh --clone-main --ref f443c2d0
```

See [docs/ilp32-docker.md](../../docs/ilp32-docker.md).
