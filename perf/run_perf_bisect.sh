#!/bin/sh
# ============================================================================
# Crash-resilient perf-test bisector.
#
# Background: at least one perf/*.ccs test has hard-rebooted the machine
# (suspected kernel-level resource exhaustion, e.g. an unbounded OS-thread
# spawn). This runner is designed so that a hard reboot is RECOVERABLE and
# INFORMATIVE rather than mysterious:
#
#   1. Each test is bracketed by a `START <t>` / `PASS|FAIL <t>` line that is
#      flushed to disk with `sync` BEFORE the test launches. If the box panics
#      mid-test, the on-disk log's dangling `START <t>` names the culprit.
#   2. On the next run we detect that dangling START and QUARANTINE that test
#      (never re-run it), so re-running after each reboot CONVERGES and the
#      quarantined set == the crashers.
#   3. Each test runs inside a `ulimit` cage (capped threads + CPU seconds) so
#      a runaway spawn/alloc test fails with EAGAIN instead of taking the
#      kernel down. This is what makes it (relatively) safe to run down.
#   4. A wall-clock timeout (perl alarm) kills hangs (e.g. ping_server).
#
# Usage:
#     make -C cc                       # ensure runtime is fresh first
#     sh perf/run_perf_bisect.sh       # run; if it reboots, just run it again
#     cat perf/.perf_bisect_progress.log
#
# Env knobs:
#     PERF_TIMEOUT   wall-clock seconds per test     (default 120)
#     PERF_MAXPROC   ulimit -u thread/proc cap       (default 1024)
#     PERF_CPUSEC    ulimit -t CPU-seconds cap        (default 150)
#     PERF_RESET=1   wipe progress log and start over
# ============================================================================
set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT" || exit 2

CCC="cc/bin/ccc"
LOG="perf/.perf_bisect_progress.log"
TIMEOUT="${PERF_TIMEOUT:-120}"
MAXPROC="${PERF_MAXPROC:-1024}"
CPUSEC="${PERF_CPUSEC:-150}"

[ "${PERF_RESET:-0}" = "1" ] && rm -f "$LOG"
touch "$LOG"

flush() { printf '%s\n' "$*" >> "$LOG"; sync; }

# Has this test reached a terminal verdict already?
settled() { grep -q "^\(PASS\|FAIL\|QUARANTINE\|SKIP\) $1\( \|\$\)" "$LOG" 2>/dev/null; }

# Find a test that was STARTed but never reached an END line in a PRIOR run
# (i.e. the machine died while it was running).
dangling() {
  awk '
    /^START /            { s=$2; open=1 }
    /^(PASS|FAIL|QUARANTINE|SKIP) / { if ($2==s) open=0 }
    END { if (open) print s }
  ' "$LOG"
}

c="$(dangling)"
if [ -n "$c" ]; then
  flush "QUARANTINE $c (dangling START from a prior boot — suspected crasher)"
  echo ">>> QUARANTINED previous-boot crasher: $c"
fi

echo "=== perf bisect: timeout=${TIMEOUT}s ulimit -u ${MAXPROC} -t ${CPUSEC}s ==="
for f in perf/*.ccs; do
  base="$(basename "$f")"
  # ping_server never terminates by design.
  case "$base" in ping_server.ccs) settled "$base" || flush "SKIP $base"; echo "  SKIP  $base (non-terminating server)"; continue;; esac
  if settled "$base"; then echo "  ----  $base (already settled)"; continue; fi

  echo "  RUN   $base"
  flush "START $base"
  # Subshell: cage resources, then run under a wall-clock alarm.
  (
    ulimit -u "$MAXPROC" 2>/dev/null
    ulimit -t "$CPUSEC"  2>/dev/null
    exec perl -e 'alarm shift @ARGV; exec @ARGV' "$TIMEOUT" "$CCC" run "$f"
  ) >"perf/.perf_out_${base}.log" 2>&1
  rc=$?
  if [ "$rc" -eq 0 ]; then
    flush "PASS $base"
    echo "        PASS"
  else
    flush "FAIL $base rc=$rc"
    echo "        FAIL rc=$rc (see perf/.perf_out_${base}.log)"
  fi
done

echo "=== complete ==="
echo "PASS:       $(grep -c '^PASS ' "$LOG")"
echo "FAIL:       $(grep -c '^FAIL ' "$LOG")"
echo "QUARANTINE: $(grep -c '^QUARANTINE ' "$LOG")"
grep '^QUARANTINE ' "$LOG" || true
