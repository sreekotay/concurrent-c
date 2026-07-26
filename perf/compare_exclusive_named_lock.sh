#!/usr/bin/env bash
# compare_exclusive_named_lock.sh — CC vs Go vs Rust vs Zig named exclusive sections
#
# Fairness contract (read before interpreting numbers):
#   * Same env:
#       CC_EXCL_WORKERS / CC_EXCL_ITERS / CC_EXCL_TRIALS
#       CC_EXCL_WORK (default 32) / CC_EXCL_KEYS (64) / CC_EXCL_ZIPF_S (1.0)
#   * Same workload:
#       Zipf: lock BY NAME each iter: zipf-pick + directory lookup + RMW under
#             lock (CC_EXCL_WORK applies here — directory+lock+scheduler
#             product throughput; no pre-resolved handles)
#       Serial fast path: one resolved mutex; one caller performs W*I pairs;
#                         always cs_spins=0; no parallel scheduler
#   * Identical Zipf RNG (xorshift64) + CDF across languages
#   * No start-gun: wall time is spawn → work → join; overlap is earned
#   * Warmup + median of timed trials; correctness checks on counters
#   * Idiomatic named locks per language (concurrent-read directory):
#       CC:   excl->lock(name) (lock-free probe + CAS + fiber park) + nursery
#       Go:   mutexFor(name) via sync.Map, sync.Mutex + goroutines
#       Rust: mutex_for(name) via RwLock<HashMap>+Arc, std Mutex + OS threads
#       Zig:  mutexFor(name) via Io.RwLock+HashMap, Io.Mutex + OS threads
#   * Zipf intentionally includes each runtime's directory + scheduler
#     behavior. Serial fast path intentionally excludes both.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
CCC="${CCC:-$REPO_ROOT/cc/bin/ccc}"
OUT="$SCRIPT_DIR/out"
mkdir -p "$OUT"

: "${CC_EXCL_WORKERS:=8}"
: "${CC_EXCL_ITERS:=200000}"
: "${CC_EXCL_TRIALS:=7}"
: "${CC_EXCL_WORK:=32}"
: "${CC_EXCL_KEYS:=64}"
: "${CC_EXCL_ZIPF_S:=1.0}"
export CC_EXCL_WORKERS CC_EXCL_ITERS CC_EXCL_TRIALS CC_EXCL_WORK CC_EXCL_KEYS CC_EXCL_ZIPF_S

echo "================================================================="
echo "NAMED EXCLUSIVE LOCK COMPARISON"
echo "================================================================="
echo "workers=$CC_EXCL_WORKERS iters=$CC_EXCL_ITERS trials=$CC_EXCL_TRIALS"
echo "work=$CC_EXCL_WORK keys=$CC_EXCL_KEYS zipf_s=$CC_EXCL_ZIPF_S"
echo ""

echo "Building Concurrent-C (release)..."
"$CCC" build --release "$SCRIPT_DIR/exclusive_named_lock.ccs" -o "$OUT/exclusive_named_lock_cc"

echo "Building Go..."
go build -o "$OUT/exclusive_named_lock_go" "$SCRIPT_DIR/go/exclusive_named_lock.go"

HAVE_RUST=0
if command -v rustc >/dev/null 2>&1; then
  echo "Building Rust..."
  rustc -O -C target-cpu=native "$SCRIPT_DIR/rust/exclusive_named_lock.rs" \
    -o "$OUT/exclusive_named_lock_rust"
  HAVE_RUST=1
else
  echo "Skipping Rust (rustc not on PATH)"
fi

HAVE_ZIG=0
if command -v zig >/dev/null 2>&1; then
  echo "Building Zig..."
  zig build-exe "$SCRIPT_DIR/zig/exclusive_named_lock.zig" -O ReleaseFast -lc \
    -femit-bin="$OUT/exclusive_named_lock_zig"
  HAVE_ZIG=1
else
  echo "Skipping Zig (zig not on PATH)"
fi
echo ""

run_one() {
  local label="$1"
  local bin="$2"
  local log="$OUT/excl_${label}.txt"
  echo "--- $label ---"
  "$bin" | tee "$log"
  echo ""
}

run_one "cc" "$OUT/exclusive_named_lock_cc"
run_one "go" "$OUT/exclusive_named_lock_go"
if [ "$HAVE_RUST" -eq 1 ]; then
  run_one "rust" "$OUT/exclusive_named_lock_rust"
fi
if [ "$HAVE_ZIG" -eq 1 ]; then
  run_one "zig" "$OUT/exclusive_named_lock_zig"
fi

echo "================================================================="
echo "SUMMARY (median lock-ops/s; higher is better)"
echo "================================================================="
printf "%-14s %18s %22s\n" "lang" "zipf_product_ops/s" "serial_fastpath_ops/s"
for label in cc go rust zig; do
  log="$OUT/excl_${label}.txt"
  [ -f "$log" ] || continue
  line=$(grep '^RESULT ' "$log" || true)
  [ -n "$line" ] || continue
  z=$(echo "$line" | awk '{for(i=1;i<=NF;i++) if($i ~ /^zipf_ops_s=/) {split($i,a,"="); print a[2]}}')
  f=$(echo "$line" | awk '{for(i=1;i<=NF;i++) if($i ~ /^serial_fastpath_ops_s=/) {split($i,a,"="); print a[2]}}')
  printf "%-14s %18s %22s\n" "$label" "$z" "$f"
done
echo ""
echo "Interpret with the fairness contract at the top of this script."
