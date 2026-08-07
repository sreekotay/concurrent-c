#!/usr/bin/env bash
# Prefer the native shadow_lower binary. Fallback: host the .ccs via default
# (native) ccc — never --frontend=legacy; that chicken-egg path is retired.
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

for cand in "$ROOT/out/cc/bin/shadow_lower" "$ROOT/cc/bin/shadow_lower"; do
  if [[ -x "$cand" ]]; then
    exec "$cand" "$@"
  fi
done

CCC="${CCC:-$ROOT/out/cc/bin/ccc}"
if [[ ! -x "$CCC" ]]; then
  CCC="$ROOT/cc/bin/ccc"
fi
if [[ ! -x "$CCC" ]]; then
  echo "shadow_lower: no binary (make -C cc) and no ccc" >&2
  exit 1
fi
exec "$CCC" run --no-cache \
  --cc-flags "-I$ROOT/cc/shadow -I$ROOT/third_party/tcc -DSHADOW_HAVE_LIBTCC=1" \
  "$ROOT/cc/shadow/shadow_lower.ccs" -- "$@"
