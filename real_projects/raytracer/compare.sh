#!/usr/bin/env bash
# compare.sh — Shirley weekend raytracer: C seq vs CC @parallel for vs Go
#
# Fairness contract:
#   * Same scene: book final random-sphere world, same camera
#     (lookfrom 13,2,3; vfov 20; defocus 0.6; focus 10)
#   * Same RNG: world LCG seed 1; per-pixel LCG from (x, y)
#   * Same image: RT_WIDTH / RT_SAMPLES / RT_DEPTH (16:9 height)
#   * Checksums of gamma-encoded RGB must match
#   * Rows:
#       c seq   — host cc -O2, one thread
#       cc seq  — RT_SEQ=1 ordinary for
#       cc par  — @parallel for over scanlines
#       go seq  — RT_SEQ=1 ordinary for
#       go par  — one goroutine per scanline
#
#   RT_SMOKE=1   48x27, 2 spp, depth 8
#   RT_PPM=dir   write <impl>.ppm into that directory
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
CCC="${CCC:-$REPO_ROOT/cc/bin/ccc}"
OUT="$SCRIPT_DIR/out"
mkdir -p "$OUT"

SMOKE=0
for arg in "$@"; do
  case "$arg" in
    --smoke) SMOKE=1 ;;
    -h|--help)
      cat <<'EOF'
usage: ./compare.sh [--smoke]

  (default)  RT_WIDTH=400 RT_SAMPLES=10 RT_DEPTH=20
  --smoke    RT_SMOKE=1 (48x27, 2 spp, depth 8)

Env knobs pass through: RT_WIDTH RT_SAMPLES RT_DEPTH RT_PPM.
EOF
      exit 0
      ;;
    *)
      echo "error: unknown arg: $arg" >&2
      exit 2
      ;;
  esac
done

if [[ "$SMOKE" -eq 1 ]]; then
  export RT_SMOKE=1
fi
: "${RT_WIDTH:=}"
: "${RT_SAMPLES:=}"
: "${RT_DEPTH:=}"

echo "================================================================="
echo "RAYTRACER: Shirley weekend scene"
echo "================================================================="
if [[ "${RT_SMOKE:-}" == "1" ]]; then
  echo "mode=smoke (defaults 48x27, 2 spp, depth 8)"
else
  echo "mode=bench (defaults 400x225, 10 spp, depth 20)"
fi
echo "RT_WIDTH=${RT_WIDTH:-} RT_SAMPLES=${RT_SAMPLES:-} RT_DEPTH=${RT_DEPTH:-}"
echo ""

echo "Building C..."
cc -O2 -o "$OUT/rt_c" "$SCRIPT_DIR/rt.c" -lm

echo "Building Concurrent-C..."
"$CCC" build --release "$SCRIPT_DIR/rt.ccs" -o "$OUT/rt_cc"

echo "Building Go..."
go build -o "$OUT/rt_go" "$SCRIPT_DIR/go/rt.go"
echo ""

run_one() {
  local label="$1"
  local bin="$2"
  shift 2
  echo "--- $label ---"
  "$@" "$bin" | tee "$OUT/${label// /_}.txt"
  echo ""
}

run_one "c seq" "$OUT/rt_c" env
run_one "cc seq" "$OUT/rt_cc" env RT_SEQ=1
run_one "cc par" "$OUT/rt_cc" env RT_SEQ=0
run_one "go seq" "$OUT/rt_go" env RT_SEQ=1
run_one "go par" "$OUT/rt_go" env RT_SEQ=0

pick() {
  local file="$1"
  local key="$2"
  # line: rt impl=... checksum=0x.. time_ms=..
  sed -n "s/.*${key}=\\([^ ]*\\).*/\\1/p" "$file" | tail -1
}

c_file="$OUT/c_seq.txt"
c_sum="$(pick "$c_file" checksum)"
c_ms="$(pick "$c_file" time_ms)"

echo "================================================================="
echo "SUMMARY"
echo "================================================================="
printf "  %-8s %10s %8s  %s\n" "row" "time_ms" "vs_c" "checksum"
fail=0
for label in "c seq" "cc seq" "cc par" "go seq" "go par"; do
  f="$OUT/${label// /_}.txt"
  ms="$(pick "$f" time_ms)"
  sum="$(pick "$f" checksum)"
  vs="—"
  if [[ "$label" != "c seq" && -n "$c_ms" && "$c_ms" != "0.00" ]]; then
    vs="$(awk -v a="$c_ms" -v b="$ms" 'BEGIN{ if (b+0==0) print "inf"; else printf "%.2fx", a/b }')"
  fi
  printf "  %-8s %10s %8s  %s\n" "$label" "$ms" "$vs" "$sum"
  if [[ -n "$c_sum" && "$sum" != "$c_sum" ]]; then
    echo "  checksum mismatch vs c seq" >&2
    fail=1
  fi
done
echo ""
if [[ "$fail" -ne 0 ]]; then
  echo "FAIL: images diverged" >&2
  exit 1
fi
echo "checksums match"
echo "Interpret with the fairness contract at the top of this script."
