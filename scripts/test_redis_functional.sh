#!/bin/sh
set -eu

# Gate the redis functional smoke (#111 command semantics, #117 wire parity,
# #130 pipeline ordering).  Before this hook, scripts/test.sh never ran
# redis_smoke.py — all redis command/wire/pipeline correctness rode manual
# runs only.
#
# Cost: the smoke itself is ~0.3s (measured; no reduced-op knob needed) —
# the only real cost is building real_projects/redis/redis_idiomatic.ccs,
# which uses ccc's incremental cache (warm rebuild is seconds).  Port
# collisions with parallel sessions/CI are avoided by `--port 0` (the smoke
# picks a free ephemeral port).

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT_DIR"

fail() { echo "[redis-functional] FAIL: $*" >&2; exit 1; }

[ -x ./cc/bin/ccc ] || fail "cc/bin/ccc not built"
command -v python3 >/dev/null 2>&1 || fail "python3 not available"

echo "[redis-functional] building redis_idiomatic (cached ok)"
./cc/bin/ccc build real_projects/redis/redis_idiomatic.ccs -o out/redis_idiomatic \
  || fail "redis_idiomatic build failed"

echo "[redis-functional] running redis_smoke.py"
out="$(python3 real_projects/redis/redis_smoke.py --port 0 2>&1)" || {
  echo "$out" | tail -40 >&2
  fail "redis_smoke.py exited nonzero"
}
case "$out" in
  *"SMOKE OK"*) : ;;
  *) echo "$out" | tail -40 >&2; fail "smoke did not print SMOKE OK" ;;
esac

echo "[redis-functional] OK"
