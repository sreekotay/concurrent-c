#!/bin/sh
# lint_soft_return_emit.sh — emit-shape ratchet for §5.1 soft-return / watermark.
#
# Spec §5.1 Conformance: in any function with registered function-scope cleanup,
# every return lowers to the soft-goto form that reaches the cleanup epilogue,
# and every cleanup statement in that epilogue sits under its registration-
# threshold guard. Bare `return` / unguarded hooks in such functions are
# non-conforming.
#
#   ./scripts/lint_soft_return_emit.sh
#   make lint-soft-return-emit
#   ./tools/dev.shcc @lint_soft_return_emit
set -eu
ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT_DIR"

CCC="${CCC:-./out/cc/bin/ccc}"
if [ ! -x "$CCC" ]; then
  CCC=./cc/bin/ccc
fi
[ -x "$CCC" ] || { echo "lint_soft_return_emit: missing ccc at $CCC" >&2; exit 2; }

FIXTURES="
tests/defer_registration_hw_smoke.ccs
tests/defer_bang_handler_return_smoke.ccs
tests/defer_scope_soft_return_smoke.ccs
tests/scratch_soft_return_restore_smoke.ccs
tests/cancel_parked_destroy_smoke.ccs
"

out_dir="$(mktemp -d)"
trap 'rm -rf "$out_dir"' EXIT
fail=0

for src in $FIXTURES; do
  if [ ! -f "$src" ]; then
    echo "lint_soft_return_emit: missing $src" >&2
    fail=1
    continue
  fi
  stem="$(basename "$src" .ccs)"
  emitted="$out_dir/$stem.c"
  if ! "$CCC" build --no-cache --emit-c-only "$src" -o "$emitted" \
      >/dev/null 2>"$out_dir/$stem.err"; then
    echo "lint_soft_return_emit: emit failed for $src" >&2
    cat "$out_dir/$stem.err" >&2 || true
    fail=1
    continue
  fi
  if ! python3 "$ROOT_DIR/scripts/lint_soft_return_emit.py" "$emitted" "$src"; then
    fail=1
    continue
  fi
  echo "lint_soft_return_emit: ok $src"
done

if [ "$fail" -ne 0 ]; then
  echo "lint_soft_return_emit: FAILED (spec §5.1 conformance)" >&2
  exit 1
fi
echo "lint_soft_return_emit: OK"
