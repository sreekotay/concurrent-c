# ILP32 Docker smoke images

Native 32-bit Linux containers for Concurrent-C runtime smokes. The compiler
and runtime are built *inside* the container so `TARGET_PTR_WIDTH` matches
`sizeof(void*)`.

| Arch | Dockerfile | Platform | Status |
|------|------------|----------|--------|
| i386 | `Dockerfile.i386` | `linux/386` | supported |
| ARM32 | `Dockerfile.arm32` | `linux/arm/v7` (gnueabihf) | supported |

**Layout:** host repo → `/src` (read-only) → rsync → `/work` (writable sandbox) →
[`ilp32_entrypoint.sh`](ilp32_entrypoint.sh) → [`../smoke_ilp32.sh`](../smoke_ilp32.sh).

Host wrappers: [`../smoke_i386.sh`](../smoke_i386.sh), [`../smoke_arm32.sh`](../smoke_arm32.sh).

See [docs/ilp32-docker.md](../../docs/ilp32-docker.md).
