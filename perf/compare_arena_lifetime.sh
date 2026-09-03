#!/bin/bash
# compare_arena_lifetime.sh - Per-operation cost of the arena lifetime model.
#
# Builds perf/arena_lifetime_bench.ccs with --release and runs it. With a
# git ref as the first argument, also builds the same benchmark against that
# ref's arena headers in a scratch worktree (the compiler binary is shared;
# only cc/include and cc/runtime differ) and prints both tables side by side
# so a header change can be priced before it lands.
#
#   perf/compare_arena_lifetime.sh            # current tree
#   perf/compare_arena_lifetime.sh HEAD~1     # current tree vs HEAD~1 headers
#   ROUNDS=9 perf/compare_arena_lifetime.sh   # more rounds (best-of)

set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
CCC="$REPO_ROOT/cc/bin/ccc"
ROUNDS="${ROUNDS:-5}"
mkdir -p "$SCRIPT_DIR/out"

build_and_run() {
    local label="$1" out="$2"
    "$CCC" build --release -DROUNDS="$ROUNDS" "$SCRIPT_DIR/arena_lifetime_bench.ccs" -o "$out"
    echo "--- $label ---"
    "$out" | tee "$SCRIPT_DIR/out/arena_lifetime_${label}.txt"
    echo ""
}

build_and_run current "$SCRIPT_DIR/out/arena_lifetime_bench"

if [ -n "$1" ]; then
    REF="$1"
    WT="$SCRIPT_DIR/out/wt_$(echo "$REF" | tr '/~^' '___')"
    rm -rf "$WT"
    git -C "$REPO_ROOT" worktree add --detach "$WT" "$REF" >/dev/null
    trap 'git -C "$REPO_ROOT" worktree remove --force "$WT" >/dev/null 2>&1 || true' EXIT
    # Point the same compiler at the ref's headers and runtime. `ccc` resolves
    # its include roots from the repo it lives in, so run the ref's own
    # wrapper only if the ref built one; otherwise borrow ours.
    if [ -x "$WT/cc/bin/ccc" ]; then
        REF_CCC="$WT/cc/bin/ccc"
    else
        echo "note: $REF has no built compiler; building it (make cc) in the worktree"
        (cd "$WT" && ./scripts/fetch_submodules.sh >/dev/null && ./scripts/apply_tcc_patches.sh >/dev/null \
            && (cd third_party/tcc && ./configure --config-cc_ext >/dev/null && make -j"$(nproc)" libtcc.a tcc libtcc1.a >/dev/null) \
            && make cc -j"$(nproc)" >/dev/null)
        REF_CCC="$WT/cc/bin/ccc"
    fi
    cp "$SCRIPT_DIR/arena_lifetime_bench.ccs" "$WT/perf/arena_lifetime_bench.ccs"
    "$REF_CCC" build --release -DROUNDS="$ROUNDS" "$WT/perf/arena_lifetime_bench.ccs" -o "$SCRIPT_DIR/out/arena_lifetime_ref"
    echo "--- $REF ---"
    "$SCRIPT_DIR/out/arena_lifetime_ref" | tee "$SCRIPT_DIR/out/arena_lifetime_ref.txt"
    echo ""
    echo "--- delta (current vs $REF, ns/op) ---"
    paste -d'|' "$SCRIPT_DIR/out/arena_lifetime_current.txt" "$SCRIPT_DIR/out/arena_lifetime_ref.txt" \
        | awk -F'|' '/ns\/op/ { split($1,a," ns/op"); split($2,b," ns/op");
            n=length(a[1]); cur=substr(a[1], n-10); ref=substr(b[1], n-10);
            printf "%-46s %10s %10s %+8.1f%%\n", substr($1,1,46), cur+0, ref+0, (cur-ref)/ref*100 }'
fi
