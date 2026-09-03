#!/bin/bash
# compare_arena_lifetime.sh - Per-operation cost of the arena lifetime model
# against plain C, and optionally against another git ref's headers.
#
# Always builds and runs perf/arena_lifetime_bench.ccs (--release) and the
# plain-C twin perf/arena_lifetime_c_baseline.c (gcc -O2), then prints the
# two side by side with the CC/C ratio per row. With a git ref as the first
# argument, also builds the CC benchmark against that ref's headers in a
# scratch worktree and prints current vs ref.
#
#   perf/compare_arena_lifetime.sh            # current vs plain C
#   perf/compare_arena_lifetime.sh HEAD~1     # ... plus current vs HEAD~1
#   ROUNDS=9 perf/compare_arena_lifetime.sh   # more rounds (best-of)

set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
CCC="$REPO_ROOT/cc/bin/ccc"
ROUNDS="${ROUNDS:-5}"
OUT="$SCRIPT_DIR/out"
mkdir -p "$OUT"

# Side-by-side table: rows matched by name, ratio of left over right.
side_by_side() {
    local left="$1" right="$2" lname="$3" rname="$4"
    printf "%-46s %10s %10s %8s\n" "row" "$lname" "$rname" "ratio"
    awk -v L="$left" -v R="$right" '
        function key(s) { sub(/ +[0-9.]+ ns\/op.*$/, "", s); return s }
        function val(s) { match(s, /[0-9.]+ ns\/op/); return substr(s, RSTART, RLENGTH - 6) + 0 }
        FILENAME == R && /[0-9] ns\/op/ { r[key($0)] = val($0); next }
        FILENAME == L && /[0-9] ns\/op/ {
            k = key($0); c = val($0);
            if (k in r && r[k] > 0) printf "%-46s %10.1f %10.1f %7.1fx\n", substr(k, 1, 46), c, r[k], c / r[k];
            else printf "%-46s %10.1f %10s\n", substr(k, 1, 46), c, "-";
        }' "$right" "$left"
}

echo "--- current (CC) ---"
"$CCC" build --release -DROUNDS="$ROUNDS" "$SCRIPT_DIR/arena_lifetime_bench.ccs" -o "$OUT/arena_lifetime_bench"
"$OUT/arena_lifetime_bench" | tee "$OUT/arena_lifetime_current.txt"
echo ""

echo "--- plain C ---"
gcc -O2 -std=c11 -DROUNDS="$ROUNDS" "$SCRIPT_DIR/arena_lifetime_c_baseline.c" -o "$OUT/arena_lifetime_c_baseline"
"$OUT/arena_lifetime_c_baseline" | tee "$OUT/arena_lifetime_c.txt"
echo ""

echo "--- CC vs plain C (ns/op) ---"
side_by_side "$OUT/arena_lifetime_current.txt" "$OUT/arena_lifetime_c.txt" "CC" "C"
echo ""

if [ -n "$1" ]; then
    REF="$1"
    WT="$OUT/wt_$(echo "$REF" | tr '/~^' '___')"
    rm -rf "$WT"
    git -C "$REPO_ROOT" worktree add --detach "$WT" "$REF" >/dev/null
    trap 'git -C "$REPO_ROOT" worktree remove --force "$WT" >/dev/null 2>&1 || true' EXIT
    if [ ! -x "$WT/cc/bin/ccc" ]; then
        echo "note: $REF has no built compiler; building it (make cc) in the worktree"
        (cd "$WT" && ./scripts/fetch_submodules.sh >/dev/null && ./scripts/apply_tcc_patches.sh >/dev/null \
            && (cd third_party/tcc && ./configure --config-cc_ext >/dev/null && make -j"$(nproc)" libtcc.a tcc libtcc1.a >/dev/null) \
            && make cc -j"$(nproc)" >/dev/null)
    fi
    cp "$SCRIPT_DIR/arena_lifetime_bench.ccs" "$WT/perf/arena_lifetime_bench.ccs"
    "$WT/cc/bin/ccc" build --release -DROUNDS="$ROUNDS" "$WT/perf/arena_lifetime_bench.ccs" -o "$OUT/arena_lifetime_ref"
    echo "--- $REF ---"
    "$OUT/arena_lifetime_ref" | tee "$OUT/arena_lifetime_ref.txt"
    echo ""
    echo "--- current vs $REF (ns/op) ---"
    side_by_side "$OUT/arena_lifetime_current.txt" "$OUT/arena_lifetime_ref.txt" "current" "$REF"
fi
