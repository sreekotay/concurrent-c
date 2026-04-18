#!/bin/bash
# run_neckbeard_challenges.sh - Run all robustness and fairness comparisons
#
# Runs the five "Neckbeard" benchmarks end-to-end and prints each sub-script's
# native per-language results + verdict block verbatim.  The per-language
# metadata (kidnappers drained, wake primitive, peak threads, allocation time,
# message counts, etc.) matters and was being stripped by the old single-scalar
# summary tables, so we just forward the sub-script output now.
#
# The sub-scripts compare_syscall.sh / compare_herd.sh /
# compare_contention_stability.sh / compare_preemption.sh all run Pthread, CC,
# Go and Zig internally, so the harness does not re-invoke them.
# compare_arena.sh only covers Pthread + CC, so Go/Zig are still run here.

set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
CCC="$REPO_ROOT/out/cc/bin/ccc"

mkdir -p "$SCRIPT_DIR/out"

echo "================================================================="
echo "CONCURRENT-C: THE NECKBEARD CHALLENGES"
echo "================================================================="
echo "Running all robustness and fairness comparisons..."
echo ""

SKIP_CC=0
SKIP_GO=0
SKIP_ZIG=0

if ! command -v "$CCC" &>/dev/null && [ ! -x "$CCC" ]; then
    echo "WARNING: CCC compiler not found at $CCC"
    echo "         CC and Pthread tests will be skipped."
    echo "         Build with: make -C $REPO_ROOT"
    echo ""
    SKIP_CC=1
fi

if ! command -v go &>/dev/null; then
    echo "WARNING: Go not found on PATH (arena Go row will be N/A)"
    echo ""
    SKIP_GO=1
fi

if ! command -v zig &>/dev/null; then
    echo "WARNING: Zig not found on PATH (arena Zig row will be N/A)"
    echo ""
    SKIP_ZIG=1
fi

build_zig() {
    local src=$1
    local out=$2
    zig build-exe "$src" -O ReleaseFast -lc -femit-bin="$out" >/dev/null
}

# Print the interesting tail of a sub-script log: everything from the first
# '--- Running' line through EOF, minus the machine-readable DATA_ tags (they
# are redundant next to the verdict block that already prints the same info
# in a human table).
print_results() {
    local f="$1"
    if [ ! -f "$f" ]; then
        echo "(no output captured)"
        return
    fi
    awk '
        /^--- Running/ { go = 1 }
        go && !/^DATA_/ { print }
    ' "$f"
}

TMPDIR_HARNESS="$(mktemp -d "${TMPDIR:-/tmp}/neckbeard.XXXXXX")"
trap 'rm -rf "$TMPDIR_HARNESS"' EXIT

# ---------------------------------------------------------------------------
# 1. Syscall Kidnapping
# ---------------------------------------------------------------------------
echo "[1/5] Syscall Kidnapping Challenge..."
if [ "$SKIP_CC" -eq 0 ]; then
    if "$SCRIPT_DIR/compare_syscall.sh" > "$TMPDIR_HARNESS/syscall.out" 2>&1; then
        print_results "$TMPDIR_HARNESS/syscall.out"
    else
        echo "  [WARN] compare_syscall.sh failed (exit $?). Last lines:"
        tail -30 "$TMPDIR_HARNESS/syscall.out"
    fi
else
    echo "  (skipped — CCC not available)"
fi
echo ""

# ---------------------------------------------------------------------------
# 2. Thundering Herd
# ---------------------------------------------------------------------------
echo "[2/5] Thundering Herd Challenge..."
if [ "$SKIP_CC" -eq 0 ]; then
    if "$SCRIPT_DIR/compare_herd.sh" > "$TMPDIR_HARNESS/herd.out" 2>&1; then
        print_results "$TMPDIR_HARNESS/herd.out"
    else
        echo "  [WARN] compare_herd.sh failed (exit $?). Last lines:"
        tail -30 "$TMPDIR_HARNESS/herd.out"
    fi
else
    echo "  (skipped — CCC not available)"
fi
echo ""

# ---------------------------------------------------------------------------
# 3. Channel Isolation
# ---------------------------------------------------------------------------
echo "[3/5] Channel Isolation Challenge..."
if [ "$SKIP_CC" -eq 0 ]; then
    if "$SCRIPT_DIR/compare_contention_stability.sh" 5 \
            > "$TMPDIR_HARNESS/contention.out" 2>&1; then
        # This script has its own min/mean/max table; print from that header.
        sed -n '/^min \/ mean \/ max/,$p' "$TMPDIR_HARNESS/contention.out"
    else
        echo "  [WARN] compare_contention_stability.sh failed (exit $?). Last lines:"
        tail -30 "$TMPDIR_HARNESS/contention.out"
    fi
else
    echo "  (skipped — CCC not available)"
fi
echo ""

# ---------------------------------------------------------------------------
# 4. Noisy Neighbor (preemption)
# ---------------------------------------------------------------------------
echo "[4/5] Noisy Neighbor Challenge..."
if [ "$SKIP_CC" -eq 0 ]; then
    if "$SCRIPT_DIR/compare_preemption.sh" > "$TMPDIR_HARNESS/preempt.out" 2>&1; then
        print_results "$TMPDIR_HARNESS/preempt.out"
    else
        echo "  [WARN] compare_preemption.sh failed (exit $?). Last lines:"
        tail -30 "$TMPDIR_HARNESS/preempt.out"
    fi
else
    echo "  (skipped — CCC not available)"
fi
echo ""

# ---------------------------------------------------------------------------
# 5. Arena Contention (CC+Pthread from sub-script, Go+Zig run separately)
# ---------------------------------------------------------------------------
echo "[5/5] Arena Contention Challenge..."
if [ "$SKIP_CC" -eq 0 ]; then
    if "$SCRIPT_DIR/compare_arena.sh" > "$TMPDIR_HARNESS/arena.out" 2>&1; then
        print_results "$TMPDIR_HARNESS/arena.out"
    else
        echo "  [WARN] compare_arena.sh failed (exit $?). Last lines:"
        tail -30 "$TMPDIR_HARNESS/arena.out"
    fi
fi

# Go + Zig arena (not covered by compare_arena.sh)
if [ "$SKIP_GO" -eq 0 ]; then
    echo ""
    echo "--- Running Go (mcache) ---"
    if go run "$SCRIPT_DIR/go/arena_contention.go" > "$TMPDIR_HARNESS/arena_go.out" 2>&1; then
        # The Go program emits a short results block; surface it verbatim.
        cat "$TMPDIR_HARNESS/arena_go.out"
    else
        echo "  [WARN] Go arena_contention failed (exit $?)."
        tail -20 "$TMPDIR_HARNESS/arena_go.out"
    fi
fi
if [ "$SKIP_ZIG" -eq 0 ]; then
    echo ""
    echo "--- Running Zig ---"
    if build_zig "$SCRIPT_DIR/zig/arena_contention.zig" "$SCRIPT_DIR/out/zig_arena_contention" \
        && "$SCRIPT_DIR/out/zig_arena_contention" > "$TMPDIR_HARNESS/arena_zig.out" 2>&1; then
        cat "$TMPDIR_HARNESS/arena_zig.out"
    else
        echo "  [WARN] Zig arena_contention failed (exit $?)."
        tail -20 "$TMPDIR_HARNESS/arena_zig.out"
    fi
fi
echo ""

echo "================================================================="
if [ "$SKIP_CC" -eq 1 ]; then
    echo "COMPLETED (partial — CCC compiler not found)"
else
    echo "ALL CHALLENGES COMPLETED"
fi
echo "================================================================="
