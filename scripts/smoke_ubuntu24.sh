#!/usr/bin/env bash
# smoke_ubuntu24.sh — Ubuntu 24.04 amd64 emit probe via Docker (raytext CI parity).
#
# Reproduce Linux-only shadow_lower / @for + .sub() failures on a Mac host.
#
#   ./scripts/smoke_ubuntu24.sh
#   RAYTEXT_ROOT=/path/to/raytext ./scripts/smoke_ubuntu24.sh
#   ./scripts/smoke_ubuntu24.sh --clone-main          # clone concurrent-c main (no local mount)
#   ./scripts/smoke_ubuntu24.sh --ref f443c2d0        # clone at commit
#   ./scripts/smoke_ubuntu24.sh --no-build            # reuse image
#
# Needs: Docker. On Apple Silicon, runs linux/amd64 (same as GitHub ubuntu-24.04).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

IMAGE="${CCC_UBUNTU24_IMAGE:-ccc-ubuntu24}"
DOCKERFILE="scripts/docker/Dockerfile.ubuntu24"
PLATFORM="${CCC_UBUNTU24_PLATFORM:-linux/amd64}"

RAYTEXT_ROOT="${RAYTEXT_ROOT:-}"
CCC_CLONE=0
CCC_REF="${CCC_REF:-main}"
DO_BUILD=1

while [ $# -gt 0 ]; do
  case "$1" in
    --clone-main) CCC_CLONE=1; shift ;;
    --ref)
      shift
      CCC_REF="${1:?--ref needs a commit or branch}"
      shift
      ;;
    --no-build) DO_BUILD=0; shift ;;
    -h|--help)
      sed -n '2,14p' "$0" | sed 's/^# \?//'
      exit 0
      ;;
    *)
      echo "unknown arg: $1" >&2
      exit 2
      ;;
  esac
done

if [ "$(uname -s)" = "Linux" ] && [ "$(uname -m)" = "x86_64" ]; then
  PLATFORM="linux/amd64"
fi

command -v docker >/dev/null 2>&1 || {
  echo "smoke_ubuntu24: docker not found" >&2
  exit 1
}
docker info >/dev/null 2>&1 || {
  echo "smoke_ubuntu24: docker daemon not running" >&2
  exit 1
}

if [ "$DO_BUILD" -eq 1 ]; then
  echo "== docker build ($PLATFORM) $IMAGE"
  docker build --platform "$PLATFORM" -f "$DOCKERFILE" -t "$IMAGE" .
fi

MOUNTS=(-v "$ROOT:/ccc:ro")
ENV=( -e "CCC_SRC=/ccc" -e "CCC_CLONE=$CCC_CLONE" -e "CCC_REF=$CCC_REF" )

if [ -n "$RAYTEXT_ROOT" ]; then
  RAYTEXT_ROOT="$(cd "$RAYTEXT_ROOT" && pwd)"
  MOUNTS+=(-v "$RAYTEXT_ROOT:/raytext:ro")
  ENV+=(-e "RAYTEXT_SRC=/raytext")
fi

echo "== docker run ($PLATFORM) ubuntu24 emit probe"
if [ "$CCC_CLONE" = "1" ]; then
  echo "   concurrent-c: clone $CCC_REF from GitHub"
else
  echo "   concurrent-c: $ROOT (ro -> /ccc)"
fi
if [ -n "$RAYTEXT_ROOT" ]; then
  echo "   raytext: $RAYTEXT_ROOT (ro -> /raytext)"
else
  echo "   raytext: skipped (set RAYTEXT_ROOT=... to include layout_measure_smoke)"
fi

exec docker run --rm --platform "$PLATFORM" \
  "${MOUNTS[@]}" \
  -v "$ROOT/scripts/docker/ubuntu24_emit_probe.sh:/usr/local/bin/ubuntu24_emit_probe.sh:ro" \
  "${ENV[@]}" \
  "$IMAGE"
