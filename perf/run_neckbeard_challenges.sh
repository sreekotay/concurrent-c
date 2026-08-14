#!/bin/bash
# run_neckbeard_challenges.sh - Run all robustness and fairness comparisons
#
# Runs the six "Neckbeard" benchmarks end-to-end and prints each sub-script's
# native per-language results + verdict block verbatim.  The per-language
# metadata (kidnappers drained, wake primitive, peak threads, allocation time,
# message counts, etc.) matters and was being stripped by the old single-scalar
# summary tables, so we just forward the sub-script output now.
#
# The sub-scripts compare_syscall.sh / compare_herd.sh /
# compare_contention_stability.sh / compare_preemption.sh /
# compare_exclusive_named_lock.sh all run the per-language variants
# internally, so the harness does not re-invoke them.
# compare_arena.sh only covers Pthread + CC, so Go/Zig are still run here.

set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
CCC="${CCC:-$REPO_ROOT/cc/bin/ccc}"
export CCC

SNAPSHOT=""
if [ "${1:-}" = "--snapshot" ]; then
    SNAPSHOT="${2:?--snapshot requires an output path}"
    shift 2
    mkdir -p "$(dirname "$SNAPSHOT")"
fi

mkdir -p "$SCRIPT_DIR/out"

print_manifest() {
    echo "=== Neckbeard Challenges (six cross-language) ==="
    echo "Date: $(date)"
    echo "Command: ./perf/run_neckbeard_challenges.sh${SNAPSHOT:+ --snapshot $SNAPSHOT}"
    echo ""
    echo "--- git ---"
    echo "HEAD: $(git -C "$REPO_ROOT" rev-parse HEAD)"
    echo "short: $(git -C "$REPO_ROOT" rev-parse --short HEAD)"
    # bin/ and out/ are build products; listing them swamps the receipt.
    local tracked untracked
    tracked=$(git -C "$REPO_ROOT" diff --name-only HEAD)
    untracked=$(git -C "$REPO_ROOT" ls-files --others --exclude-standard \
        | grep -vE '^(bin/|out/)' || true)
    if [ -z "$tracked" ] && [ -z "$untracked" ]; then
        echo "tree: clean (tracked + source untracked; bin/out ignored)"
    else
        echo "tree: DIRTY (this record is not a clean-commit receipt)"
        if [ -n "$tracked" ]; then
            echo "modified/tracked:"
            echo "$tracked" | sed 's/^/  /'
        fi
        if [ -n "$untracked" ]; then
            echo "untracked (excluding bin/ out/):"
            echo "$untracked" | sed 's/^/  /'
        fi
    fi
    echo ""
    echo "--- machine ---"
    echo "uname: $(uname -a)"
    if command -v sysctl >/dev/null 2>&1; then
        local brand ncpu mem
        brand=$(sysctl -n machdep.cpu.brand_string 2>/dev/null || true)
        ncpu=$(sysctl -n hw.ncpu 2>/dev/null || true)
        mem=$(sysctl -n hw.memsize 2>/dev/null || true)
        [ -n "$brand" ] && echo "cpu: $brand"
        [ -n "$ncpu" ] && echo "ncpu: $ncpu"
        [ -n "$mem" ] && echo "memsize: $mem"
    fi
    if [ -r /proc/cpuinfo ]; then
        awk -F: '/^model name/ { gsub(/^ +/, "", $2); print "cpu:", $2; exit }' /proc/cpuinfo
        echo "ncpu: $(getconf _NPROCESSORS_ONLN 2>/dev/null || echo '?')"
    fi
    echo "hostname: $(hostname 2>/dev/null || echo '?')"
    echo ""
    echo "--- compilers ---"
    if [ -x "$CCC" ]; then
        echo "ccc: $("$CCC" --version 2>/dev/null | head -1)  ($CCC)"
    else
        echo "ccc: not found ($CCC)"
    fi
    if command -v cc >/dev/null 2>&1; then
        echo "cc: $(cc --version 2>/dev/null | head -1)"
    fi
    if command -v go >/dev/null 2>&1; then
        echo "go: $(go version 2>/dev/null)"
    else
        echo "go: not found"
    fi
    if command -v zig >/dev/null 2>&1; then
        echo "zig: $(zig version 2>/dev/null)"
    else
        echo "zig: not found"
    fi
    if command -v rustc >/dev/null 2>&1; then
        echo "rustc: $(rustc --version 2>/dev/null)"
    else
        echo "rustc: not found"
    fi
    echo ""
}

if [ -n "$SNAPSHOT" ]; then
    exec > >(tee "$SNAPSHOT")
    exec 2>&1
fi

print_manifest

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
echo "[1/6] Syscall Kidnapping Challenge..."
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
echo "[2/6] Thundering Herd Challenge..."
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
echo "[3/6] Channel Isolation Challenge..."
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
echo "[4/6] Noisy Neighbor Challenge..."
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
echo "[5/6] Arena Contention Challenge..."
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
    echo "--- Running Go (non-escaping alloc, stack-promoted) ---"
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

# ---------------------------------------------------------------------------
# 6. Named Exclusive Lock (CC vs Go vs Rust vs Zig)
# ---------------------------------------------------------------------------
echo "[6/6] Named Exclusive Lock Challenge..."
if [ "$SKIP_CC" -eq 0 ]; then
    if "$SCRIPT_DIR/compare_exclusive_named_lock.sh" > "$TMPDIR_HARNESS/excl.out" 2>&1; then
        # The sub-script's summary table is the interesting part; the
        # per-language trial logs land in perf/out/excl_<lang>.txt.
        sed -n '/^SUMMARY (median/,$p' "$TMPDIR_HARNESS/excl.out"
    else
        echo "  [WARN] compare_exclusive_named_lock.sh failed (exit $?). Last lines:"
        tail -30 "$TMPDIR_HARNESS/excl.out"
    fi
else
    echo "  (skipped — CCC not available)"
fi
echo ""

echo "================================================================="
if [ "$SKIP_CC" -eq 1 ]; then
    echo "COMPLETED (partial — CCC compiler not found)"
else
    echo "ALL CHALLENGES COMPLETED"
fi
echo "================================================================="
