#!/usr/bin/env bash
# Wall + peak RSS: bash (capture / pipes / awk) vs CC (slurp / sync / chan).
#
#   ./perf/bench_line_scan_vs_bash.sh [path] [samples] [passes]
#
# Timed CC runs use --timed (no cold verify / warm inside the wall clock).
# Verify + warm happen once outside measure().
#
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PATH_FILE="${1:-$ROOT/cc/src/visitor/pass_result_unwrap.c}"
SAMPLES="${2:-5}"
PASSES="${3:-8}"
CCC="${CCC:-$ROOT/cc/bin/ccc}"
BIN="$ROOT/bin/bench_line_scan"
CTX='cc__reparse_source_to_ast_ctx('
EX='cc__reparse_source_to_ast_ex('

if [[ ! -f "$PATH_FILE" ]]; then
  echo "bench_line_scan_vs_bash: missing $PATH_FILE" >&2
  exit 1
fi
BYTES=$(wc -c < "$PATH_FILE" | tr -d ' ')
echo "bench_line_scan_vs_bash: path=$PATH_FILE bytes=$BYTES samples=$SAMPLES passes=$PASSES"

# BSD /usr/bin/time -l  (macOS)  or  GNU time -v  (/usr/bin/time on Linux, gtime on macOS)
TIME_BIN=""
TIME_STYLE=""
if command -v gtime >/dev/null 2>&1 && gtime --version >/dev/null 2>&1; then
  TIME_BIN=$(command -v gtime)
  TIME_STYLE=gnu
elif [[ -x /usr/bin/time ]] && /usr/bin/time --version >/dev/null 2>&1; then
  TIME_BIN=/usr/bin/time
  TIME_STYLE=gnu
elif [[ -x /usr/bin/time ]] && /usr/bin/time -l true >/dev/null 2>&1; then
  TIME_BIN=/usr/bin/time
  TIME_STYLE=bsd
else
  echo "bench_line_scan_vs_bash: need BSD /usr/bin/time -l or GNU time -v (install gnu-time / use Linux)" >&2
  exit 1
fi
echo "bench_line_scan_vs_bash: timer=$TIME_BIN ($TIME_STYLE)"

echo "[build] $BIN"
"$CCC" build --link "$ROOT/perf/bench_line_scan.ccs" -o "$BIN" >/dev/null

TIME_ERR=$(mktemp)
RESULTS=$(mktemp)
cleanup() { rm -f "$TIME_ERR" "$RESULTS"; }
trap cleanup EXIT

parse_time_rss() {
  local real rss ms
  if [[ "$TIME_STYLE" == bsd ]]; then
    if ! grep -q 'maximum resident set size' "$TIME_ERR"; then
      echo "bench_line_scan_vs_bash: BSD time -l output missing RSS" >&2
      cat "$TIME_ERR" >&2
      return 1
    fi
    real=$(awk '/ real / {print $1; exit}' "$TIME_ERR")
    rss=$(awk '/maximum resident set size/ {print $1; exit}' "$TIME_ERR")
  else
    if ! grep -q 'Maximum resident set size' "$TIME_ERR"; then
      echo "bench_line_scan_vs_bash: GNU time -v output missing RSS" >&2
      cat "$TIME_ERR" >&2
      return 1
    fi
    real=$(awk -F: '/Elapsed \(wall clock\)/ {
      gsub(/^[ \t]+/,"",$2)
      n=split($2,a,/:/)
      if (n==3) t=a[1]*3600+a[2]*60+a[3]
      else if (n==2) t=a[1]*60+a[2]
      else t=$2
      printf "%.6f\n", t; exit
    }' "$TIME_ERR")
    rss=$(awk -F: '/Maximum resident set size/ {
      gsub(/[^0-9]/,"",$2); print $2*1024; exit
    }' "$TIME_ERR")
  fi
  if [[ -z "${real:-}" || -z "${rss:-}" ]]; then
    echo "bench_line_scan_vs_bash: failed to parse wall/RSS" >&2
    cat "$TIME_ERR" >&2
    return 1
  fi
  ms=$(awk -v r="$real" 'BEGIN{printf "%.3f", (r+0)*1000}')
  echo "$ms $rss"
}

measure() {
  local label=$1
  shift
  if [[ "$TIME_STYLE" == bsd ]]; then
    "$TIME_BIN" -l "$@" >/dev/null 2>"$TIME_ERR"
  else
    "$TIME_BIN" -v "$@" >/dev/null 2>"$TIME_ERR"
  fi
  echo "$label $(parse_time_rss)" | tee -a "$RESULTS"
}

# Export for child bash -c bodies
export PATH_FILE CTX EX PASSES

echo "[verify]"
"$BIN" "$PATH_FILE" 0 1 all >/dev/null

echo "[warm]"
bash -c 'for ((i=0;i<PASSES;i++)); do grep -c "$CTX" "$PATH_FILE"||true; grep -c "$EX" "$PATH_FILE"||true; wc -l <"$PATH_FILE"|tr -d " "; done >/dev/null'
bash -c 'for ((i=0;i<PASSES;i++)); do cat "$PATH_FILE"|grep -c "$CTX"||true; cat "$PATH_FILE"|grep -c "$EX"||true; cat "$PATH_FILE"|wc -l|tr -d " "; done >/dev/null'
bash -c 'for ((i=0;i<PASSES;i++)); do awk -v ctx="$CTX" -v ex="$EX" "
  function count(s, lit,    n, p) {
    n=0
    while ((p = index(s, lit)) > 0) {
      n++
      s = substr(s, p + length(lit))
    }
    return n
  }
  { lines++; c+=count(\$0, ctx); e+=count(\$0, ex) }
  END { print lines+0, c+0, e+0 }
" "$PATH_FILE"; done >/dev/null'
"$BIN" --timed "$PATH_FILE" 1 "$PASSES" slurp >/dev/null
"$BIN" --timed "$PATH_FILE" 1 "$PASSES" sync >/dev/null
"$BIN" --timed "$PATH_FILE" 1 "$PASSES" chan >/dev/null

echo "[samples]"
for ((s = 1; s <= SAMPLES; s++)); do
  echo "  sample $s"
  measure bash_capture bash -c \
    'for ((i=0;i<PASSES;i++)); do
       grep -c "$CTX" "$PATH_FILE" || true
       grep -c "$EX" "$PATH_FILE" || true
       wc -l <"$PATH_FILE" | tr -d " "
     done >/dev/null'

  measure bash_pipes bash -c \
    'for ((i=0;i<PASSES;i++)); do
       cat "$PATH_FILE" | grep -c "$CTX" || true
       cat "$PATH_FILE" | grep -c "$EX" || true
       cat "$PATH_FILE" | wc -l | tr -d " "
     done >/dev/null'

  measure bash_awk bash -c \
    'for ((i=0;i<PASSES;i++)); do
       awk -v ctx="$CTX" -v ex="$EX" "
         function count(s, lit,    n, p) {
           n=0
           while ((p = index(s, lit)) > 0) {
             n++
             s = substr(s, p + length(lit))
           }
           return n
         }
         { lines++; c+=count(\$0, ctx); e+=count(\$0, ex) }
         END { print lines+0, c+0, e+0 }
       " "$PATH_FILE"
     done >/dev/null'

  # --timed: wall covers only PASSES scans (verify/warm already done above)
  measure cc_slurp "$BIN" --timed "$PATH_FILE" 1 "$PASSES" slurp
  measure cc_sync "$BIN" --timed "$PATH_FILE" 1 "$PASSES" sync
  measure cc_chan "$BIN" --timed "$PATH_FILE" 1 "$PASSES" chan
done

echo "---"
# BSD awk: no true multi-dim arrays — pack samples as "ms,rss" strings.
awk '
  {
    k = $1
    n[k]++
    ms[k, n[k]] = $2 + 0
    rss[k, n[k]] = $3 + 0
    cnt[k] = n[k]
  }
  function med(key, field,   i, j, t, b, nn) {
    nn = cnt[key]
    for (i = 1; i <= nn; i++) b[i] = (field == "ms") ? ms[key, i] : rss[key, i]
    for (i = 1; i <= nn; i++)
      for (j = i + 1; j <= nn; j++)
        if (b[j] < b[i]) { t = b[i]; b[i] = b[j]; b[j] = t }
    return b[int((nn + 1) / 2)]
  }
  END {
    bash = med("bash_capture", "ms")
    split("bash_capture bash_pipes bash_awk cc_slurp cc_sync cc_chan", ord, " ")
    printf "%-14s  %10s  %12s  %8s\n", "mode", "median_ms", "median_RSS", "vs_bash_capture"
    for (i = 1; i <= 6; i++) {
      m = ord[i]
      if (!(m in cnt)) continue
      mms = med(m, "ms")
      mr = med(m, "rss")
      rel = (bash > 0) ? mms / bash : 0
      printf "%-14s  %8.2f ms  %8.2f MB  %6.2fx\n", m, mms, mr / 1024 / 1024, rel
    }
  }
' "$RESULTS"

echo "notes:"
echo "  bash_capture = capture_baseline static (grep -c = matching lines)"
echo "  bash_pipes   = same work through cat|grep / cat|wc"
echo "  bash_awk     = one-pass occurrence counts (fair vs CC)"
echo "  cc_*         = occurrence counts; timed with --timed (PASSES only)"
