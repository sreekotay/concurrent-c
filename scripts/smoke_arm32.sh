#!/usr/bin/env bash
# smoke_arm32.sh — run the ILP32 runtime smoke for Linux ARM32 (armv7 / gnueabihf) via Docker.
#
# From a macOS/Linux host with Docker:
#   ./scripts/smoke_arm32.sh
#
# Mounts the repo read-only and builds in an anonymous /work volume so host
# cc/out/tcc artifacts are not replaced with arm32 objects.
#
# Inside an already-running arm32 container with a writable tree:
#   CCC_ILP32_ARCH=arm ./scripts/smoke_ilp32.sh
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT_DIR"

IMAGE="${CCC_ARM32_IMAGE:-ccc-arm32}"
DOCKERFILE="scripts/docker/Dockerfile.arm32"
PLATFORM="linux/arm/v7"

if [ "$(uname -s)" = "Linux" ] && [ "$(getconf LONG_BIT 2>/dev/null || echo 0)" = "32" ]; then
  machine="$(uname -m)"
  case "$machine" in
    arm|armv6l|armv7l|armhf)
      export CCC_ILP32_ARCH=arm
      exec ./scripts/smoke_ilp32.sh
      ;;
  esac
fi

command -v docker >/dev/null 2>&1 || {
  echo "smoke_arm32: docker not found; install Docker or run smoke_ilp32.sh on linux/arm/v7" >&2
  exit 1
}

echo "== docker build ($PLATFORM) $IMAGE"
docker build --platform "$PLATFORM" -f "$DOCKERFILE" -t "$IMAGE" .

echo "== docker run ($PLATFORM) RO /src + sandbox /work"
exec docker run --rm --platform "$PLATFORM" \
  -v "$ROOT_DIR":/src:ro \
  -v /work \
  -e CCC_ILP32_ARCH=arm \
  -e CCC_ILP32_SRC=/src \
  -e CCC_ILP32_WORK=/work \
  -e BUILD="${BUILD:-debug}" \
  "$IMAGE" /src/scripts/docker/ilp32_entrypoint.sh
