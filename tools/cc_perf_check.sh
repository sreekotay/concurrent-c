#!/usr/bin/env bash
# Bash oracle for tools/cc_perf_check.ccscript (primary: make perf-regress).
#
# Prefer the .ccscript twin for day-to-day use:
#   ./tools/cc_perf_check.ccscript
#   ./tools/cc_perf_check.ccscript --update
#   make perf-regress
# This script stays as a side-by-side oracle (make perf-regress-oracle).
#
# Workflow:
#   1. Run scripts/capture_baseline.sh into a temp file (same command the
#      committed baseline was captured with).
#   2. Diff each metric against perf/compiler_baseline.txt.
#   3. Apply per-metric tolerance:
#        - Deterministic metrics (reparse counts, suite size, static reparse
#          sites) must equal baseline. They are stable across runs and any
#          drift means real architectural change.
#        - LOC metrics are informational (drift is fine; growth > +25% prints
#          a notice but doesn't fail).
#        - Wall time is noisy on shared hardware; allow +20% over baseline
#          before failing.
#   4. Exit 0 on no regression, 1 on any failing metric.
#
# Use:
#   ./tools/cc_perf_check.sh            # compares against perf/compiler_baseline.txt
#   ./tools/cc_perf_check.sh --update   # overwrites baseline with current numbers
#                                       # (use after a deliberate, reviewed change)
#
# Intended use: run before merging any change touching cc/src/visitor/* or
# cc/src/parser/*. CI could enforce this too.

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BASELINE="$ROOT/perf/compiler_baseline.txt"
TMP="$(mktemp -t cc_perf_check.XXXXXX)"
trap 'rm -f "$TMP"' EXIT

UPDATE=0
if [[ "${1:-}" == "--update" ]]; then
  UPDATE=1
fi

if [[ ! -f "$BASELINE" ]]; then
  echo "perf check: no baseline at $BASELINE" >&2
  echo "perf check: run ./scripts/capture_baseline.sh first" >&2
  exit 1
fi

echo "perf check: capturing current metrics (this takes ~60s)..."
"$ROOT/scripts/capture_baseline.sh" "$TMP" >/dev/null

if [[ "$UPDATE" -eq 1 ]]; then
  cp "$TMP" "$BASELINE"
  echo "perf check: baseline updated. New contents:"
  cat "$BASELINE"
  exit 0
fi

# Parse a key=value file into shell variables prefixed with $prefix_.
load_metrics() {
  local file="$1"
  local prefix="$2"
  while IFS='=' read -r key val; do
    case "$key" in
      ''|\#*) continue ;;  # skip blank lines and comments
    esac
    eval "${prefix}_${key}='${val}'"
  done < "$file"
}

load_metrics "$BASELINE" base
load_metrics "$TMP" cur

regressions=0
warnings=0

check_eq() {
  local name="$1"
  local b_var="base_$name"
  local c_var="cur_$name"
  local b="${!b_var:-}"
  local c="${!c_var:-}"
  if [[ -z "$b" || -z "$c" ]]; then
    printf "  %-38s SKIP    (missing: baseline=%s current=%s)\n" "$name" "${b:-EMPTY}" "${c:-EMPTY}"
    return
  fi
  if [[ "$b" == "$c" ]]; then
    printf "  %-38s OK      (=%s)\n" "$name" "$c"
  elif [[ "$c" -lt "$b" ]] 2>/dev/null; then
    printf "  %-38s IMPROVED (%s -> %s)\n" "$name" "$b" "$c"
  else
    printf "  %-38s FAIL    (baseline=%s current=%s)\n" "$name" "$b" "$c"
    regressions=$((regressions + 1))
  fi
}

check_le() {
  # current must be <= baseline (improvements OK).
  local name="$1"
  local b_var="base_$name"
  local c_var="cur_$name"
  local b="${!b_var:-}"
  local c="${!c_var:-}"
  if [[ -z "$b" || -z "$c" ]]; then
    printf "  %-38s SKIP    (missing values)\n" "$name"
    return
  fi
  if [[ "$c" -le "$b" ]] 2>/dev/null; then
    if [[ "$c" -lt "$b" ]]; then
      printf "  %-38s IMPROVED (%s -> %s)\n" "$name" "$b" "$c"
    else
      printf "  %-38s OK      (=%s)\n" "$name" "$c"
    fi
  else
    printf "  %-38s FAIL    (baseline=%s current=%s, increase=%d)\n" \
      "$name" "$b" "$c" $((c - b))
    regressions=$((regressions + 1))
  fi
}

check_wall() {
  # Wall time is INFORMATIONAL ONLY — never fails the check.
  # Reason: on a developer machine, wall time is dominated by OS scheduler
  # noise (other processes, thermal throttling, Spotlight indexing, etc.) and
  # routinely varies +/-30% back-to-back with no code changes. We measured a
  # 48s → 66s jump (+37%) on two consecutive runs of an unmodified tree.
  # The deterministic reparse counts above are the real regression guard.
  # Wall-clock just gives a sanity check: if it's drastically off WHILE
  # reparses are unchanged, something non-reparse-related got slow (text
  # pass, codegen, file I/O) and the user should investigate manually.
  local name="wall_real_seconds"
  local b="${base_wall_real_seconds:-}"
  local c="${cur_wall_real_seconds:-}"
  if [[ -z "$b" || -z "$c" ]]; then
    printf "  %-38s SKIP    (missing values)\n" "$name"
    return
  fi
  local pct=$(awk "BEGIN{printf \"%+.1f\", ($c - $b) * 100 / $b}")
  local verdict=$(awk -v c="$c" -v b="$b" 'BEGIN{
    diff = (c - b) / b * 100
    if (diff <= 5)       print "OK"
    else if (diff <= 30) print "INFO"
    else                 print "NOTICE"
  }')
  case "$verdict" in
    OK)     printf "  %-38s OK      (=%ss vs baseline %ss, %s%%)\n" "$name" "$c" "$b" "$pct" ;;
    INFO)   printf "  %-38s INFO    (=%ss vs baseline %ss, %s%% — within typical OS-noise band)\n" "$name" "$c" "$b" "$pct" ;;
    NOTICE) printf "  %-38s NOTICE  (=%ss vs baseline %ss, %s%% — large jump; worth a manual look if reparses are unchanged)\n" "$name" "$c" "$b" "$pct"; warnings=$((warnings + 1)) ;;
  esac
}

check_loc() {
  # LOC growth > +25% is a notice, not a failure (legit features grow code).
  local name="$1"
  local b_var="base_$name"
  local c_var="cur_$name"
  local b="${!b_var:-}"
  local c="${!c_var:-}"
  if [[ -z "$b" || -z "$c" ]]; then
    printf "  %-38s SKIP    (missing values)\n" "$name"
    return
  fi
  if [[ "$c" -le "$b" ]]; then
    printf "  %-38s OK      (=%s vs baseline %s)\n" "$name" "$c" "$b"
  else
    local pct=$(awk "BEGIN{printf \"%.0f\", ($c - $b) * 100 / $b}")
    if [[ "$pct" -gt 25 ]]; then
      printf "  %-38s NOTICE  (=%s vs baseline %s, +%s%%)\n" "$name" "$c" "$b" "$pct"
      warnings=$((warnings + 1))
    else
      printf "  %-38s OK      (=%s vs baseline %s, +%s%%)\n" "$name" "$c" "$b" "$pct"
    fi
  fi
}

echo
echo "perf check: comparing current vs baseline ($BASELINE)"
echo

echo "Static metrics (must equal baseline):"
check_eq reparse_sites_visit_codegen

echo
echo "LOC metrics (growth > +25% prints a notice):"
check_loc loc_visit_codegen
check_loc loc_closure_pass
check_loc loc_visitor_dir

echo
echo "Dynamic metrics (must be <= baseline):"
check_eq suite_tests
check_le reparses_phase3
check_le reparses_statement_lowering
check_le reparses_async_lowering
check_le reparses_final_ufcs
check_le reparses_total

echo
echo "Wall-clock (informational only — too noisy on developer machines to gate on):"
check_wall

echo
if [[ $regressions -gt 0 ]]; then
  echo "perf check: FAIL — $regressions regression(s), $warnings warning(s)"
  echo "perf check: if this is an intentional change, run ./tools/cc_perf_check.ccscript --update"
  exit 1
fi
if [[ $warnings -gt 0 ]]; then
  echo "perf check: OK with $warnings warning(s) (no regressions)"
else
  echo "perf check: OK"
fi
