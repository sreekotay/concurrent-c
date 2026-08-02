#!/usr/bin/env bash
# Wrapper: prefer first-class shadow_lower binary; fall back to ccc run.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
if [[ -x "$ROOT/out/cc/bin/shadow_lower" ]]; then
  exec "$ROOT/out/cc/bin/shadow_lower" "$@"
fi
if [[ -x "$ROOT/cc/bin/shadow_lower" ]]; then
  exec "$ROOT/cc/bin/shadow_lower" "$@"
fi
CCC="${CCC:-$ROOT/out/cc/bin/ccc}"
exec "$CCC" run --no-cache "$ROOT/examples/serdes/c/shadow_lower.ccs" -- "$@"
