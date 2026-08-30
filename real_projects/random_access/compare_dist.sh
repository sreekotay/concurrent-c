#!/usr/bin/env bash
# compare_dist.sh — multi-locale Chapel HPCC RA vs Concurrent-C
#
# Two placement facts: who generates stream[u], who owns T[i].
# Chapel: -nl N, ra.chpl useOn / ra-atomics remote xor.
# CC:     ra_dist.ccs  dist / dist-atomic (N:1 hop of r).
#
# CC hops on in-process channels. Chapel hops on CHPL_COMM. Same
# sentence; not the same wire. If this Chapel is CHPL_COMM=none,
# -nl N is not a locale grid and we skip those rows.
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
usage: ./compare_dist.sh [--smoke]

  chapel ra -nl N          ↔  cc dist
  chapel ra-atomics -nl N  ↔  cc dist-atomic

  (default)  TABLE_BITS=20 LOCALES=2 WORKERS=1
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
  : "${TABLE_BITS:=20}"
fi
: "${LOCALES:=2}"
: "${WORKERS:=1}"
export TABLE_BITS LOCALES WORKERS
NUPDATE=$((4 << TABLE_BITS))

HAVE_CHPL=0
CHPL_MULTI=0
if [[ -x "$CHPL" ]] || command -v "$CHPL" >/dev/null 2>&1; then
  HAVE_CHPL=1
  comm="$("$CHPL" --print-chpl-settings 2>/dev/null | awk -F= '/^CHPL_COMM=/ {print $2; exit}' | tr -d '[:space:]')"
  if [[ -n "$comm" && "$comm" != "none" ]]; then
    CHPL_MULTI=1
  fi
elif [[ "$SMOKE" -eq 0 ]]; then
  echo "note: chpl not on PATH — CC dist only"
fi

echo "================================================================="
echo "RANDOM ACCESS: Chapel ra -nl $LOCALES ↔ CC dist"
echo "================================================================="
echo "TABLE_BITS=$TABLE_BITS N_U=$NUPDATE LOCALES=$LOCALES WORKERS=$WORKERS"
if [[ "$CHPL_MULTI" -eq 1 ]]; then
  echo "Chapel CHPL_COMM is not none — will run -nl $LOCALES"
else
  echo "Chapel multi-locale skipped (CHPL_COMM=none or no chpl)"
fi
echo ""

echo "Building Concurrent-C dist..."
"$CCC" build --release "$SCRIPT_DIR/ra_dist.ccs" -o "$OUT/ra_cc_dist"

if [[ "$CHPL_MULTI" -eq 1 ]]; then
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
  "$bin" -nl "$LOCALES" --n="$TABLE_BITS" --N_U="$NUPDATE" \
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
  echo "gups impl=chapel mode=$mode table_bits=$TABLE_BITS nupdate=$n seconds=$t locales=$LOCALES workers=$WORKERS errors=$e error_frac=$frac gups=$g" \
    | tee "$OUT/${label// /_}.txt"
  echo ""
}

if [[ "$CHPL_MULTI" -eq 1 ]]; then
  run_chapel "chapel ra nl$LOCALES" "$OUT/ra_chpl" ra
  run_chapel "chapel ra-atomics nl$LOCALES" "$OUT/ra_chpl_atomic" ra-atomics
elif [[ "$HAVE_CHPL" -eq 1 && "$SMOKE" -eq 0 ]]; then
  echo "note: this Chapel cannot run -nl $LOCALES (CHPL_COMM=none)"
  echo ""
fi
run_one "cc dist" "$OUT/ra_cc_dist" env MODE=dist
run_one "cc dist-atomic" "$OUT/ra_cc_dist" env MODE=dist-atomic

pick() {
  local file="$1"
  local key="$2"
  sed -n "s/.*${key}=\\([^ ]*\\).*/\\1/p" "$file" | tail -1
}

chpl_gups=""
if [[ -f "$OUT/chapel_ra_nl${LOCALES}.txt" ]]; then
  chpl_gups="$(pick "$OUT/chapel_ra_nl${LOCALES}.txt" gups)"
fi
chpl_at_gups=""
if [[ -f "$OUT/chapel_ra-atomics_nl${LOCALES}.txt" ]]; then
  chpl_at_gups="$(pick "$OUT/chapel_ra-atomics_nl${LOCALES}.txt" gups)"
fi

echo "================================================================="
echo "SUMMARY"
echo "================================================================="
printf "  %-24s %10s %10s %10s %10s\n" "row" "gups" "errors" "nupdate" "vs_chpl"
fail=0

rows=("cc dist" "cc dist-atomic")
if [[ "$CHPL_MULTI" -eq 1 ]]; then
  rows=("chapel ra nl$LOCALES" "cc dist" "chapel ra-atomics nl$LOCALES" "cc dist-atomic")
fi

for label in "${rows[@]}"; do
  f="$OUT/${label// /_}.txt"
  g="$(pick "$f" gups)"
  e="$(pick "$f" errors)"
  n="$(pick "$f" nupdate)"
  vs="—"
  base=""
  if [[ "$label" == "cc dist" ]]; then
    base="$chpl_gups"
  elif [[ "$label" == "cc dist-atomic" ]]; then
    base="$chpl_at_gups"
  fi
  if [[ -n "$base" && "$base" != "0" && "$base" != "0.000000" ]]; then
    vs="$(awk -v a="$base" -v b="$g" 'BEGIN{ if (b+0==0) print "inf"; else printf "%.2fx", b/a }')"
  fi
  printf "  %-24s %10s %10s %10s %10s\n" "$label" "$g" "$e" "$n" "$vs"
done

dist_frac="$(pick "$OUT/cc_dist.txt" error_frac)"
atomic_err="$(pick "$OUT/cc_dist-atomic.txt" errors)"

if [[ "$atomic_err" != "0" ]]; then
  echo "FAIL: dist-atomic must have 0 errors" >&2
  fail=1
fi
if ! awk -v f="$dist_frac" 'BEGIN{ exit (f+0 <= 0.01) ? 0 : 1 }'; then
  echo "FAIL: dist error_frac=$dist_frac > 0.01" >&2
  fail=1
fi

echo ""
if [[ "$fail" -ne 0 ]]; then
  echo "FAIL" >&2
  exit 1
fi
echo "dist within 1%; dist-atomic identity"
