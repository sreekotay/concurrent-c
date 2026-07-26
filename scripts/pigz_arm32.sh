#!/usr/bin/env bash
# pigz_arm32.sh — build/compare original pigz vs pigz_cc inside linux/arm/v7 Docker.
#
# Host tree is mounted read-only; build artifacts stay in a named /work volume.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT_DIR"

IMAGE="${CCC_ARM32_IMAGE:-ccc-arm32}"
DOCKERFILE="scripts/docker/Dockerfile.arm32"
PLATFORM="linux/arm/v7"

command -v docker >/dev/null 2>&1 || {
  echo "pigz_arm32: docker not found" >&2
  exit 1
}

echo "== docker build ($PLATFORM) $IMAGE"
docker build --platform "$PLATFORM" -f "$DOCKERFILE" -t "$IMAGE" .

VOLUME="${CCC_ARM32_VOLUME:-ccc-arm32-work}"

echo "== docker run pigz compare (RO /src + volume $VOLUME)"
exec docker run --rm --platform "$PLATFORM" \
  -v "$ROOT_DIR":/src:ro \
  -v "$VOLUME":/work \
  -e CCC_ILP32_ARCH=arm \
  -e CCC_ILP32_SRC=/src \
  -e CCC_ILP32_WORK=/work \
  -e BUILD="${BUILD:-debug}" \
  -e SKIP_TOOLCHAIN="${SKIP_TOOLCHAIN:-0}" \
  -e FORCE_TOOLCHAIN="${FORCE_TOOLCHAIN:-0}" \
  -e PIGZ_BENCH_MB="${PIGZ_BENCH_MB:-20}" \
  -e PIGZ_BENCH_WORKERS="${PIGZ_BENCH_WORKERS:-4}" \
  -e PIGZ_BENCH_RUNS="${PIGZ_BENCH_RUNS:-2}" \
  "$IMAGE" /src/scripts/docker/ilp32_entrypoint.sh \
  ./real_projects/pigz/compare_ilp32.sh
