#!/usr/bin/env bash
# pigz_i386.sh — build/compare original pigz vs pigz_idiomatic (and pigz_cc)
# inside linux/386 Docker.
#
# Host tree is mounted read-only; build artifacts stay in an anonymous /work volume.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT_DIR"

IMAGE="${CCC_I386_IMAGE:-ccc-i386}"
DOCKERFILE="scripts/docker/Dockerfile.i386"
PLATFORM="linux/386"

command -v docker >/dev/null 2>&1 || {
  echo "pigz_i386: docker not found" >&2
  exit 1
}

echo "== docker build ($PLATFORM) $IMAGE"
docker build --platform "$PLATFORM" -f "$DOCKERFILE" -t "$IMAGE" .

# Named volume keeps the ILP32 toolchain across runs; source is re-synced each time.
VOLUME="${CCC_I386_VOLUME:-ccc-ilp32-work}"

echo "== docker run pigz compare (RO /src + volume $VOLUME)"
exec docker run --rm --platform "$PLATFORM" \
  -v "$ROOT_DIR":/src:ro \
  -v "$VOLUME":/work \
  -e CCC_ILP32_ARCH=i386 \
  -e CCC_ILP32_SRC=/src \
  -e CCC_ILP32_WORK=/work \
  -e BUILD="${BUILD:-debug}" \
  -e SKIP_TOOLCHAIN="${SKIP_TOOLCHAIN:-0}" \
  -e FORCE_TOOLCHAIN="${FORCE_TOOLCHAIN:-0}" \
  -e CCC_HOST_CC="${CCC_HOST_CC:-cc}" \
  -e CCC_BACKEND_CC="${CCC_BACKEND_CC:-}" \
  -e PIGZ_BENCH_MB="${PIGZ_BENCH_MB:-20}" \
  -e PIGZ_BENCH_WORKERS="${PIGZ_BENCH_WORKERS:-4}" \
  -e PIGZ_BENCH_RUNS="${PIGZ_BENCH_RUNS:-2}" \
  "$IMAGE" /src/scripts/docker/ilp32_entrypoint.sh \
  ./real_projects/pigz/compare_ilp32.sh
