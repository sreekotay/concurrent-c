#!/bin/sh
# ============================================================================
# Safe, watchdog'd probe for the work_stealing_efficiency blowup.
#
# Runs a (reduced) .ccs under CC_V2_STATS=1 while an external sampler watches
# RSS + thread count and HARD-KILLS the process the instant it crosses a safe
# ceiling — so we can characterise the memory curve WITHOUT risking a reboot.
#
# We deliberately only run small task counts and EXTRAPOLATE to 100k; we never
# actually run the dangerous size.
#
# Usage:   sh perf/run_coro_probe.sh <path-to.ccs>
# Knobs:   RSS_CEIL_KB (default 3145728 = 3 GB)
#          THR_CEIL    (default 300)
#          TIMEOUT     (default 60 wall seconds)
#          CPUSEC      (default 90, ulimit -t backstop)
# ============================================================================
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"; cd "$ROOT" || exit 2
CCC="cc/bin/ccc"
SRC="${1:?usage: run_coro_probe.sh <file.ccs>}"
base="$(basename "$SRC")"
OUT="perf/.coro_probe_${base}.log"
RSS_CEIL_KB="${RSS_CEIL_KB:-3145728}"
THR_CEIL="${THR_CEIL:-300}"
TIMEOUT="${TIMEOUT:-60}"
CPUSEC="${CPUSEC:-90}"

echo "=== probe $base  (RSS_ceil=${RSS_CEIL_KB}KB thr_ceil=${THR_CEIL} timeout=${TIMEOUT}s) ==="
( ulimit -t "$CPUSEC" 2>/dev/null; exec env CC_V2_STATS=1 "$CCC" run "$SRC" ) >"$OUT" 2>&1 &
pid=$!

start=$(date +%s)
peak=0
verdict="EXIT"
while kill -0 "$pid" 2>/dev/null; do
  rss="$(ps -o rss= -p "$pid" 2>/dev/null | tr -d ' ')"
  [ -z "$rss" ] && break
  [ "$rss" -gt "$peak" ] && peak="$rss"
  thr="$(ps -M -p "$pid" 2>/dev/null | grep -c .)"
  now=$(date +%s)
  if [ "$rss" -gt "$RSS_CEIL_KB" ]; then
    echo "!! RSS ceiling hit: ${rss}KB > ${RSS_CEIL_KB}KB — KILL -9"; kill -9 "$pid"; verdict="RSS_KILL"; break
  fi
  if [ -n "$thr" ] && [ "$thr" -gt "$THR_CEIL" ]; then
    echo "!! thread ceiling hit: ${thr} > ${THR_CEIL} — KILL -9"; kill -9 "$pid"; verdict="THR_KILL"; break
  fi
  if [ "$((now - start))" -gt "$TIMEOUT" ]; then
    echo "!! wall timeout ${TIMEOUT}s — KILL -9"; kill -9 "$pid"; verdict="TIMEOUT"; break
  fi
  sleep 0.05
done
wait "$pid" 2>/dev/null; rc=$?
echo "RESULT $base  peak_rss_kb=$peak  rc=$rc  verdict=$verdict"
grep -E 'coro_pool|target_active|wake:' "$OUT" 2>/dev/null || echo "(no stats dump — likely killed before atexit)"
echo
