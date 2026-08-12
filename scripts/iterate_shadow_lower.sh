#!/usr/bin/env bash
# One-liner for the shadow_lower edit loop (lowerer faces only).
# For stdlib / cold smoke / first checkout, see docs/build-when.md.
#
# Default (iterate): rebuild lowerer from cc/shadow/*.ccs via the live seed.
#   ./scripts/iterate_shadow_lower.sh
#
# Ship a new bootstrap seed (no commit):
#   ./scripts/iterate_shadow_lower.sh --ship           # ccs → snapshot → promote
#   ./scripts/iterate_shadow_lower.sh --ship --smoke   # + host-cc hello smoke
#
# Does not commit. After --ship: git add last-good + the new MAJOR.MINOR.PATCH-N/; then cold-check.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

SHIP=0
SMOKE=0
for arg in "$@"; do
  case "$arg" in
    --ship) SHIP=1 ;;
    --smoke) SMOKE=1 ;;
    -h|--help)
      cat <<'EOF'
usage: ./scripts/iterate_shadow_lower.sh [--ship] [--smoke]

  (default)  make -C cc SHADOW_LOWER_SOURCE=ccs …/shadow_lower
  --ship     then snapshot + promote (new MAJOR.MINOR.PATCH-N, flip last-good)
  --smoke    with --ship: snapshot --smoke (host-cc + hello emit)

  Stdlib edits → make -C cc lower-headers (not this script).
  When to run what → docs/build-when.md

EOF
      exit 0
      ;;
    *)
      echo "error: unknown arg: $arg (try --help)" >&2
      exit 2
      ;;
  esac
done

if [[ "$SMOKE" -eq 1 && "$SHIP" -eq 0 ]]; then
  echo "error: --smoke only applies with --ship" >&2
  exit 2
fi

echo "[iterate] rebuild shadow_lower from cc/shadow (SHADOW_LOWER_SOURCE=ccs)"
make -C cc SHADOW_LOWER_SOURCE=ccs ../out/cc/bin/shadow_lower

if [[ "$SHIP" -eq 0 ]]; then
  echo "[iterate] done (live binary: out/cc/bin/shadow_lower)"
  echo "[iterate] tip: --ship when the face should freeze into bootstrap"
  exit 0
fi

SNAP=(./scripts/snapshot_shadow_lower.sh)
if [[ "$SMOKE" -eq 1 ]]; then
  SNAP+=(--smoke)
fi
echo "[iterate] snapshot ${SNAP[*]}"
"${SNAP[@]}"

echo "[iterate] promote"
./scripts/promote_shadow_bootstrap.sh

echo "[iterate] host-cc from new last-good"
make -C cc SHADOW_LOWER_SOURCE=bootstrap ../out/cc/bin/shadow_lower

VER="$(cat cc/bootstrap/shadow_lower/last-good)"
echo "[iterate] shipped $VER (not committed)"
echo "  git add cc/bootstrap/shadow_lower/last-good cc/bootstrap/shadow_lower/$VER"
