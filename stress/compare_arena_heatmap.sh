#!/usr/bin/env bash
# compare_arena_heatmap.sh — root × block_max grid for CC (optional C).
#
# Fixed live bulk workload; sweep ARENA_MEM_BULK_ROOT × block_max.
# Shows where tier-3 overflow dies (bulk_ovf→0) and how bulk_ms moves.
#
# Env:
#   ARENA_HEAT_ROOTS      "65536 262144 1048576 4194304 16777216 67108864"
#   ARENA_HEAT_POLICIES   "1 2 4 8 0"
#   ARENA_HEAT_LANGS      "cc" or "cc c"
#   ARENA_MEM_BULK        live alloc count (default 800000 ≈ ~108MiB)
#   ARENA_MEM_TRIALS      default 1 (grid is already wide)
#   ARENA_MEM_QUICK=1     smaller roots + lighter bulk
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
OUT="$SCRIPT_DIR/out"
CCC="${CCC:-}"
if [ -z "$CCC" ]; then
  if [ -x "$REPO_ROOT/cc/bin/ccc" ]; then CCC="$REPO_ROOT/cc/bin/ccc"
  elif [ -x "$REPO_ROOT/out/cc/bin/ccc" ]; then CCC="$REPO_ROOT/out/cc/bin/ccc"
  else echo "error: ccc not found" >&2; exit 1; fi
fi

TRIALS="${ARENA_MEM_TRIALS:-1}"
LANGS="${ARENA_HEAT_LANGS:-cc}"
POLICIES="${ARENA_HEAT_POLICIES:-1 2 4 8 0}"

mkdir -p "$OUT"

# Tip/ovf/churn nearly off — grid is about bulk under (root, N).
export ARENA_MEM_TIP_ROUNDS="${ARENA_MEM_TIP_ROUNDS:-5}"
export ARENA_MEM_TIP_STEPS="${ARENA_MEM_TIP_STEPS:-200}"
export ARENA_MEM_TIP_ROOT="${ARENA_MEM_TIP_ROOT:-$((1024 * 1024))}"
export ARENA_MEM_OVF="${ARENA_MEM_OVF:-0}"
export ARENA_MEM_CHURN="${ARENA_MEM_CHURN:-10}"
export ARENA_MEM_CHURN_EACH="${ARENA_MEM_CHURN_EACH:-32}"

if [ "${ARENA_MEM_QUICK:-0}" = "1" ]; then
  ROOTS="${ARENA_HEAT_ROOTS:-262144 1048576 4194304 16777216}"
  export ARENA_MEM_BULK="${ARENA_MEM_BULK:-400000}"
else
  ROOTS="${ARENA_HEAT_ROOTS:-65536 262144 1048576 4194304 16777216 67108864}"
  export ARENA_MEM_BULK="${ARENA_MEM_BULK:-800000}"
fi

# Expected live ≈ bulk * avg(16 + (i*17)%240) ≈ bulk * 135.5
LIVE_EST=$(awk -v n="$ARENA_MEM_BULK" 'BEGIN { printf "%.1f", n * 135.5 / (1024*1024) }')

human() {
  local b=$1
  if [ "$b" -ge $((1024 * 1024)) ]; then
    awk -v b="$b" 'BEGIN { printf "%gMiB", b/(1024*1024) }'
  elif [ "$b" -ge 1024 ]; then
    awk -v b="$b" 'BEGIN { printf "%gKiB", b/1024 }'
  else
    echo "${b}B"
  fi
}

# Theoretical slab capacity for N blocks with 1.5× growth from root (matches grow heuristic).
slab_cap() {
  local root=$1 n=$2
  if [ "$n" = "0" ]; then
    echo "inf"
    return
  fi
  awk -v r="$root" -v n="$n" 'BEGIN {
    cap = 0; c = r
    for (i = 0; i < n; i++) {
      cap += c
      c = c + int(c/2)
      if (c < 4096) c = 4096
    }
    printf "%.1f", cap/(1024*1024)
  }'
}

echo "================================================================="
echo "ARENA HEATMAP — root × block_max (ovf collapse / bulk_ms)"
echo "================================================================="
echo "bulk_n=$ARENA_MEM_BULK  live≈${LIVE_EST}MiB  trials=$TRIALS  langs=$LANGS"
echo "policies: $POLICIES"
echo -n "roots:"
for r in $ROOTS; do echo -n " $(human "$r")"; done
echo ""
echo ""
echo "Theoretical slab capacity MiB (1.5× chain) vs live≈${LIVE_EST}MiB:"
printf "%10s" "root\\N"
for bm in $POLICIES; do printf " %8s" "N=$bm"; done
echo ""
for r in $ROOTS; do
  printf "%10s" "$(human "$r")"
  for bm in $POLICIES; do
    printf " %8s" "$(slab_cap "$r" "$bm")"
  done
  echo ""
done
echo "================================================================="
echo ""

echo "Building..."
need_cc=0 need_c=0
for lang in $LANGS; do
  case "$lang" in
    cc) need_cc=1 ;;
    c) need_c=1 ;;
    *) echo "error: unknown lang $lang" >&2; exit 1 ;;
  esac
done
if [ "$need_cc" = 1 ]; then
  "$CCC" build --release "$SCRIPT_DIR/arena_memory_bench.ccs" -o "$OUT/arena_memory_bench_cc" >/dev/null
fi
if [ "$need_c" = 1 ]; then
  cc -O2 -std=c11 "$SCRIPT_DIR/c/arena_memory_bench.c" -o "$OUT/arena_memory_bench_c"
fi
echo "Done."
echo ""

RAW="$OUT/arena_heatmap_results.raw"
: > "$RAW"

for ((t = 1; t <= TRIALS; t++)); do
  echo "--- trial $t/$TRIALS ---"
  for lang in $LANGS; do
    bin="$OUT/arena_memory_bench_${lang}"
    for root in $ROOTS; do
      for bm in $POLICIES; do
        echo -n "  $lang root=$(human "$root") N=$bm ... "
        if ! out=$(ARENA_MEM_BULK_ROOT="$root" ARENA_MEM_BLOCK_MAX="$bm" "$bin" 2>&1); then
          echo "FAIL"
          echo "$out"
          exit 1
        fi
        line=$(echo "$out" | grep '^RESULT ' || true)
        if [ -z "$line" ]; then
          echo "no RESULT"
          echo "$out"
          exit 1
        fi
        # normalize: ensure root= is present for awk
        echo "trial=$t root=$root $line" >> "$RAW"
        bovf=$(echo "$line" | sed -n 's/.*bulk_ovf=\([0-9]*\).*/\1/p')
        bms=$(echo "$line" | sed -n 's/.*bulk_ms=\([0-9.]*\).*/\1/p')
        if [ "${bovf:-1}" = "0" ]; then
          echo "clean bulk_ms=$bms"
        else
          awk -v o="$bovf" -v m="$bms" 'BEGIN { printf "ovf=%.1fMiB bulk_ms=%s\n", o/(1024*1024), m }'
        fi
      done
    done
  done
  echo ""
done

emit_grid() {
  local lang=$1 field=$2 title=$3
  echo "$title ($lang)"
  # header
  printf "%10s" "root\\N"
  for bm in $POLICIES; do printf " %10s" "N=$bm"; done
  echo ""
  for root in $ROOTS; do
    printf "%10s" "$(human "$root")"
    for bm in $POLICIES; do
      val=$(awk -v lang="$lang" -v root="$root" -v bm="$bm" -v field="$field" '
        BEGIN { n=0; s=0 }
        $0 ~ "lang="lang && $0 ~ "root="root" " {
          # block_max may be absent for older lines; match exactly
          ok = 0
          for (i = 1; i <= NF; i++) {
            if ($i == ("block_max=" bm)) ok = 1
            if ($i ~ ("^" field "=")) { split($i, a, "="); v = a[2]+0 }
          }
          if (ok) { n++; s += v }
        }
        END {
          if (n == 0) { print "-"; exit }
          if (field == "bulk_ovf") printf "%.1f", s/n/(1024*1024)
          else printf "%.2f", s/n
        }
      ' "$RAW")
      printf " %10s" "$val"
    done
    echo ""
  done
  echo ""
}

echo "================================================================="
echo "GRIDS (averages over $TRIALS trial(s); ovf in MiB, times in ms)"
echo "================================================================="
for lang in $LANGS; do
  emit_grid "$lang" bulk_ovf "bulk_ovf MiB — 0 = stayed in slabs"
  emit_grid "$lang" bulk_ms  "bulk_ms (alloc+reclaim)"
  emit_grid "$lang" reset_ms "reset_ms (reclaim split)"
done

echo "Draw line: for each root, smallest listed N with bulk_ovf=0 (else spill):"
for lang in $LANGS; do
  echo -n "  $lang:"
  for root in $ROOTS; do
    pick=spill
    for bm in $POLICIES; do
      clean=$(awk -v lang="$lang" -v root="$root" -v bm="$bm" '
        BEGIN { n=0; dirty=0 }
        {
          ok_lang=0; ok_root=0; ok_bm=0; ovf=-1
          for (i = 1; i <= NF; i++) {
            if ($i == ("lang=" lang)) ok_lang=1
            if ($i == ("root=" root)) ok_root=1
            if ($i == ("block_max=" bm)) ok_bm=1
            if ($i ~ /^bulk_ovf=/) { split($i, a, "="); ovf=a[2]+0 }
          }
          if (ok_lang && ok_root && ok_bm) {
            n++
            if (ovf != 0) dirty=1
          }
        }
        END { if (n>0 && dirty==0) print "yes"; else print "no" }
      ' "$RAW")
      if [ "$clean" = "yes" ]; then
        pick=$bm
        break
      fi
    done
    echo -n " $(human "$root")→$pick"
  done
  echo ""
done

echo ""
echo "raw: $RAW"
echo "Rule of thumb: default N=4 is justified when typical request roots"
echo "put live under the N=4 capacity column; else raise root or N."
echo "================================================================="
