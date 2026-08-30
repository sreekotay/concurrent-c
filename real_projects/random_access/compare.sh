#!/usr/bin/env bash
# compare.sh — single-locale Chapel HPCC RA vs Concurrent-C
#
# Two races only (-nl 1):
#   chapel ra.chpl          ↔  cc smp          (unlocked XOR, one table)
#   chapel ra-atomics.chpl  ↔  cc smp-atomic   (atomic xor)
# -suseLCG=false: HPCC LFSR, same as ra.h.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
CCC="${CCC:-$REPO_ROOT/cc/bin/ccc}"
CHPL="${CHPL:-}"
if [[ -z "$CHPL" ]]; then
  if command -v chpl >/dev/null 2>&1; then
    CHPL="$(command -v chpl)"
  elif [[ -x /opt/homebrew/bin/chpl ]]; then
    CHPL=/opt/homebrew/bin/chpl
  elif [[ -x /usr/local/bin/chpl ]]; then
    CHPL=/usr/local/bin/chpl
  else
    CHPL=chpl
  fi
fi
OUT="$SCRIPT_DIR/out"
mkdir -p "$OUT"

SMOKE=0
for arg in "$@"; do
  case "$arg" in
    --smoke) SMOKE=1 ;;
    -h|--help)
      cat <<'EOF'
usage: ./compare.sh [--smoke]

  chapel ra -nl 1          ↔  cc smp
  chapel ra-atomics -nl 1  ↔  cc smp-atomic

  (default)  TABLE_BITS=26 WORKERS=4   # 512 MiB table; needs chpl
  --smoke    TABLE_BITS=16; Chapel optional
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
  export RA_SMOKE=1
  : "${TABLE_BITS:=16}"
else
  : "${TABLE_BITS:=26}"
fi
: "${WORKERS:=4}"
export TABLE_BITS WORKERS
NUPDATE=$((4 << TABLE_BITS))

HAVE_CHPL=0
if [[ -x "$CHPL" ]] || command -v "$CHPL" >/dev/null 2>&1; then
  HAVE_CHPL=1
elif [[ "$SMOKE" -eq 0 ]]; then
  echo "error: chpl not on PATH. brew install chapel" >&2
  exit 2
else
  echo "note: chpl not on PATH — CC identity only"
fi

echo "================================================================="
echo "RANDOM ACCESS: Chapel ra -nl 1 ↔ CC smp"
echo "================================================================="
echo "TABLE_BITS=$TABLE_BITS N_U=$NUPDATE WORKERS=$WORKERS locales=1"
echo ""

echo "Building Concurrent-C..."
"$CCC" build --release "$SCRIPT_DIR/ra_idiomatic.ccs" -o "$OUT/ra_cc"

if [[ "$HAVE_CHPL" -eq 1 ]]; then
  if [[ ! -f "$SCRIPT_DIR/chapel/ra.chpl" ]]; then
    echo "Fetching Chapel HPCC RA sources..."
    "$SCRIPT_DIR/setup.sh"
  fi
  echo "Building Chapel RA (-suseLCG=false)..."
  (
    cd "$SCRIPT_DIR/chapel"
    "$CHPL" --fast -suseLCG=false ra.chpl -o "$OUT/ra_chpl"
    "$CHPL" --fast -suseLCG=false ra-atomics.chpl -o "$OUT/ra_chpl_atomic"
  )
fi
echo ""

run_one() {
  local label="$1"
  local bin="$2"
  shift 2
  echo "--- $label ---"
  "$@" "$bin" | tee "$OUT/${label// /_}.txt"
  echo ""
}

run_chapel() {
  local label="$1"
  local bin="$2"
  local mode="$3"
  echo "--- $label ---"
  local raw="$OUT/${label// /_}.raw"
  "$bin" -nl 1 --n="$TABLE_BITS" --N_U="$NUPDATE" \
    --dataParTasksPerLocale="$WORKERS" --verify=true \
    --printParams=true --printStats=true | tee "$raw"
  local g e n t
  g="$(sed -n 's/^Performance (GUPS) = //p' "$raw" | tail -1)"
  e="$(sed -n 's/^Number of errors is: //p' "$raw" | tail -1)"
  n="$(sed -n 's/^Number of updates = //p' "$raw" | tail -1)"
  t="$(sed -n 's/^Execution time = //p' "$raw" | tail -1)"
  : "${e:=0}"
  : "${n:=$NUPDATE}"
  : "${t:=0}"
  : "${g:=0}"
  local frac
  frac="$(awk -v e="$e" -v n="$n" 'BEGIN{ if (n+0==0) print 0; else printf "%.6f", e/n }')"
  echo "gups impl=chapel mode=$mode table_bits=$TABLE_BITS nupdate=$n seconds=$t workers=$WORKERS errors=$e error_frac=$frac gups=$g" \
    | tee "$OUT/${label// /_}.txt"
  echo ""
}

if [[ "$HAVE_CHPL" -eq 1 ]]; then
  run_chapel "chapel ra" "$OUT/ra_chpl" ra
  run_chapel "chapel ra-atomics" "$OUT/ra_chpl_atomic" ra-atomics
fi
run_one "cc smp" "$OUT/ra_cc" env MODE=smp
run_one "cc smp-atomic" "$OUT/ra_cc" env MODE=smp-atomic

pick() {
  local file="$1"
  local key="$2"
  sed -n "s/.*${key}=\\([^ ]*\\).*/\\1/p" "$file" | tail -1
}

chpl_gups=""
chpl_nup=""
if [[ -f "$OUT/chapel_ra.txt" ]]; then
  chpl_gups="$(pick "$OUT/chapel_ra.txt" gups)"
  chpl_nup="$(pick "$OUT/chapel_ra.txt" nupdate)"
fi
chpl_at_gups=""
chpl_at_nup=""
if [[ -f "$OUT/chapel_ra-atomics.txt" ]]; then
  chpl_at_gups="$(pick "$OUT/chapel_ra-atomics.txt" gups)"
  chpl_at_nup="$(pick "$OUT/chapel_ra-atomics.txt" nupdate)"
fi

echo "================================================================="
echo "SUMMARY"
echo "================================================================="
printf "  %-18s %10s %10s %10s %10s\n" "row" "gups" "errors" "nupdate" "vs_chpl"
fail=0

rows=()
if [[ "$HAVE_CHPL" -eq 1 ]]; then
  rows+=("chapel ra" "cc smp" "chapel ra-atomics" "cc smp-atomic")
else
  rows+=("cc smp" "cc smp-atomic")
fi

for label in "${rows[@]}"; do
  f="$OUT/${label// /_}.txt"
  g="$(pick "$f" gups)"
  e="$(pick "$f" errors)"
  n="$(pick "$f" nupdate)"
  vs="—"
  base=""
  if [[ "$label" == "cc smp" ]]; then
    base="$chpl_gups"
  elif [[ "$label" == "cc smp-atomic" ]]; then
    base="$chpl_at_gups"
  fi
  if [[ -n "$base" && "$base" != "0" && "$base" != "0.000000" ]]; then
    vs="$(awk -v a="$base" -v b="$g" 'BEGIN{ if (b+0==0) print "inf"; else printf "%.2fx", b/a }')"
  fi
  printf "  %-18s %10s %10s %10s %10s\n" "$label" "$g" "$e" "$n" "$vs"
done

smp_frac="$(pick "$OUT/cc_smp.txt" error_frac)"
atomic_err="$(pick "$OUT/cc_smp-atomic.txt" errors)"
cc_nup="$(pick "$OUT/cc_smp.txt" nupdate)"

if [[ "$atomic_err" != "0" ]]; then
  echo "FAIL: smp-atomic must have 0 errors" >&2
  fail=1
fi
if ! awk -v f="$smp_frac" 'BEGIN{ exit (f+0 <= 0.01) ? 0 : 1 }'; then
  echo "FAIL: smp error_frac=$smp_frac > 0.01" >&2
  fail=1
fi
if [[ -n "$chpl_nup" && -n "$cc_nup" && "$chpl_nup" != "$cc_nup" ]]; then
  echo "FAIL: Chapel ra nupdate != cc smp" >&2
  fail=1
fi
if [[ -n "$chpl_at_nup" && -n "$cc_nup" && "$chpl_at_nup" != "$cc_nup" ]]; then
  echo "FAIL: Chapel ra-atomics nupdate != cc smp" >&2
  fail=1
fi

echo ""
if [[ "$fail" -ne 0 ]]; then
  echo "FAIL" >&2
  exit 1
fi
echo "smp within 1%; smp-atomic identity"
