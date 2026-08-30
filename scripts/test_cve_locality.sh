#!/bin/sh
# Build and run every studies/cve_locality/corpus/*/idiomatic.ccs.
# Skip _template. Pin the count so a dropped entry is a miss.
set -eu
ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT_DIR"
CCC="${CCC:-$ROOT_DIR/cc/bin/ccc}"
EXPECTED=27

fail() { echo "[test_cve_locality] FAIL: $1" >&2; exit 1; }

[ -x "$CCC" ] || fail "missing $CCC"

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

n=0
fail_n=0
for src in studies/cve_locality/corpus/*/idiomatic.ccs; do
    dir=$(basename "$(dirname "$src")")
    [ "$dir" = "_template" ] && continue
    n=$((n + 1))
    bin="$work/$dir"
    if ! "$CCC" build "$src" -o "$bin" >"$work/$dir.log" 2>&1; then
        echo "[test_cve_locality] FAIL build $dir"
        tail -20 "$work/$dir.log" >&2
        fail_n=$((fail_n + 1))
        continue
    fi
    if ! "$bin" >"$work/$dir.run" 2>&1; then
        echo "[test_cve_locality] FAIL run $dir"
        tail -20 "$work/$dir.run" >&2
        fail_n=$((fail_n + 1))
        continue
    fi
done

[ "$n" = "$EXPECTED" ] || fail "expected $EXPECTED corpus demos, found $n"
[ "$fail_n" = 0 ] || fail "$fail_n of $n demos failed"
echo "[test_cve_locality] $n demos ok"
