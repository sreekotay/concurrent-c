#!/usr/bin/env bash
# smoke_i386.sh — run the ILP32 runtime smoke for Linux i386 via Docker.
#
# From a macOS/Linux host with Docker:
#   ./scripts/smoke_i386.sh
#   CCC_HOST_CC=tcc ./scripts/smoke_i386.sh   # self-build ccc with TinyCC
#
# Mounts the repo read-only and builds in an anonymous /work volume so host
# cc/out/tcc artifacts are not replaced with i386 objects.
#
# Inside an already-running i386 container with a writable tree:
#   CCC_ILP32_ARCH=i386 ./scripts/smoke_ilp32.sh
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT_DIR"

IMAGE="${CCC_I386_IMAGE:-ccc-i386}"
DOCKERFILE="scripts/docker/Dockerfile.i386"
PLATFORM="linux/386"

if [ "$(uname -s)" = "Linux" ] && [ "$(getconf LONG_BIT 2>/dev/null || echo 0)" = "32" ]; then
  # Already on native ILP32 Linux — skip Docker.
  export CCC_ILP32_ARCH=i386
  exec ./scripts/smoke_ilp32.sh
fi

command -v docker >/dev/null 2>&1 || {
  echo "smoke_i386: docker not found; install Docker or run smoke_ilp32.sh on linux/386" >&2
  exit 1
}

echo "== docker build ($PLATFORM) $IMAGE"
docker build --platform "$PLATFORM" -f "$DOCKERFILE" -t "$IMAGE" .

echo "== docker run ($PLATFORM) RO /src + sandbox /work"
exec docker run --rm --platform "$PLATFORM" \
  -v "$ROOT_DIR":/src:ro \
  -v /work \
  -e CCC_ILP32_ARCH=i386 \
  -e CCC_ILP32_SRC=/src \
  -e CCC_ILP32_WORK=/work \
  -e BUILD="${BUILD:-debug}" \
  -e CCC_HOST_CC="${CCC_HOST_CC:-cc}" \
  "$IMAGE" /src/scripts/docker/ilp32_entrypoint.sh
