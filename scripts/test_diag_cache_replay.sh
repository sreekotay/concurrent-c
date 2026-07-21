#!/bin/sh
# Warm-cache diagnostic replay.
#
# A pass that prints an error but "recovers" (leaves the construct
# unlowered) used to still count as a successful emit: the .c landed in
# the incremental cache, and a warm rerun skipped the diag-printing pass
# entirely — the diagnostic vanished and the build failed somewhere else
# (or not at all).  This deterministically flaked
# m0_5_diag_channel_pair_origin_fail on any second consecutive cached run.
#
# Pinned here: two consecutive CACHED builds of the same failing input
# must BOTH fail and BOTH print the diagnostic.
set -eu
ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT_DIR"
CCC=./cc/bin/ccc
FIXTURE=tests/m0_5_diag_channel_pair_origin_fail.ccs
DIAG_SUBSTR='error: channel: cc_channel_pair'

fail() { echo "[test_diag_cache_replay] FAIL: $1" >&2; exit 1; }

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
mkdir -p "$work/out" "$work/bin"

run_build() {
  # cache ON deliberately (the bug only exists warm)
  "$CCC" build --out-dir "$work/out" --bin-dir "$work/bin" --link \
      "$FIXTURE" -o "$work/bin/m0" >"$work/stderr.$1" 2>&1 && return 0 || return 1
}

if run_build 1; then
  fail "run 1: build unexpectedly succeeded for a _fail fixture"
fi
grep -q "$DIAG_SUBSTR" "$work/stderr.1" \
  || fail "run 1 (cold): diagnostic missing from stderr"

if run_build 2; then
  fail "run 2 (warm): build unexpectedly succeeded — erroring emit was cached"
fi
grep -q "$DIAG_SUBSTR" "$work/stderr.2" \
  || fail "run 2 (warm cache): diagnostic missing — erroring emit rode the cache"

echo "[test_diag_cache_replay] OK"
