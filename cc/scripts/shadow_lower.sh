#!/usr/bin/env bash
# Fallback shadow lowerer via `ccc run` (parallel path — does not replace ccc).
# Primary install is a native libtcc-linked binary from `make -C cc`.
# This script remains for bootstrap / when the native binary is absent.
set -euo pipefail
SELF_DIR="$(cd "$(dirname "$0")" && pwd)"
# cc/bin/shadow_lower → repo root is ../..
# out/cc/bin/shadow_lower → repo root is ../../..
# cc/scripts/shadow_lower.sh (dev) → repo root is ../..
if [[ -f "$SELF_DIR/../../cc/shadow/shadow_lower.ccs" ]]; then
  ROOT="$(cd "$SELF_DIR/../.." && pwd)"
elif [[ -f "$SELF_DIR/../../../cc/shadow/shadow_lower.ccs" ]]; then
  ROOT="$(cd "$SELF_DIR/../../.." && pwd)"
else
  echo "shadow_lower: cannot locate cc/shadow/shadow_lower.ccs" >&2
  exit 1
fi
CCC="${CCC:-$ROOT/out/cc/bin/ccc}"
if [[ ! -x "$CCC" ]]; then
  CCC="$ROOT/cc/bin/ccc"
fi
# Chicken-egg only: no native binary yet, so run the .ccs through the legacy
# front. Prefer the installed/built shadow_lower binary for normal use.
exec env CC_FRONTEND=legacy "$CCC" --frontend=legacy run --no-cache \
  "$ROOT/cc/shadow/shadow_lower.ccs" -- "$@"
