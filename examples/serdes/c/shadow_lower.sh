#!/usr/bin/env bash
# Wrapper: experiment shadow lowerer beside ccc (never replaces lower_header).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
CCC="${CCC:-$ROOT/out/cc/bin/ccc}"
exec "$CCC" run --no-cache "$ROOT/examples/serdes/c/shadow_lower.ccs" -- "$@"
