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
# must BOTH fail and BOTH print the diagnostic.  Case 2 pins the warning
# side: a warning-emitting build that hits the cache must replay the
# lowering warnings captured on the cold build.
set -eu
ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT_DIR"
CCC=./cc/bin/ccc
FIXTURE=tests/m0_5_diag_channel_pair_origin_fail.ccs
DIAG_SUBSTR='error: channel: cc_channel_pair'

fail() { echo "[test_diag_cache_replay] FAIL: $1" >&2; exit 1; }

work="$(mktemp -d)"
work2="$(mktemp -d)"
trap 'rm -rf "$work" "$work2"' EXIT
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

# Case 2: WARNINGS.  A successful emit that prints lowering warnings IS
# cached, so the warning bytes are persisted as a sidecar next to the
# emitted C and replayed on every cache hit — a warm rebuild must print
# the same warnings as the cold one.
WARN_FIXTURE=tests/as_arg_coerce_smoke.ccs
WARN_SUBSTR='skips as: path'

mkdir -p "$work2/out" "$work2/bin"

run_warn_build() {
  "$CCC" build --out-dir "$work2/out" --bin-dir "$work2/bin" --link \
      "$WARN_FIXTURE" -o "$work2/bin/t" >"$work2/stderr.$1" 2>&1
}

run_warn_build 1 || fail "warn run 1 (cold): build failed"
grep -q "$WARN_SUBSTR" "$work2/stderr.1" \
  || fail "warn run 1 (cold): warning missing from stderr"

run_warn_build 2 || fail "warn run 2 (warm): build failed"
grep -q "$WARN_SUBSTR" "$work2/stderr.2" \
  || fail "warn run 2 (warm cache): warning missing — lowering warning not replayed"

echo "[test_diag_cache_replay] OK"
