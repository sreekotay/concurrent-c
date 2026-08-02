#!/usr/bin/env bash
# First-class shadow lowerer CLI (parallel path — does not replace ccc).
# Installed to cc/bin/shadow_lower and out/cc/bin/shadow_lower by the Makefile.
set -euo pipefail
SELF_DIR="$(cd "$(dirname "$0")" && pwd)"
# cc/bin/shadow_lower → repo root is ../..
# out/cc/bin/shadow_lower → repo root is ../../..
# cc/scripts/shadow_lower.sh (dev) → repo root is ../..
if [[ -f "$SELF_DIR/../../examples/serdes/c/shadow_lower.ccs" ]]; then
  ROOT="$(cd "$SELF_DIR/../.." && pwd)"
elif [[ -f "$SELF_DIR/../../../examples/serdes/c/shadow_lower.ccs" ]]; then
  ROOT="$(cd "$SELF_DIR/../../.." && pwd)"
else
  echo "shadow_lower: cannot locate examples/serdes/c/shadow_lower.ccs" >&2
  exit 1
fi
CCC="${CCC:-$ROOT/out/cc/bin/ccc}"
if [[ ! -x "$CCC" ]]; then
  CCC="$ROOT/cc/bin/ccc"
fi
exec "$CCC" run --no-cache "$ROOT/examples/serdes/c/shadow_lower.ccs" -- "$@"
