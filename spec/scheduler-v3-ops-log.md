# Scheduler V3 Operational Log and Runbook (Crash-Safe)

This document preserves the active debugging and optimization workflow for Scheduler V3 so work can resume even if IDE/session state is lost.

It is intentionally practical: what we changed, why, where, how we test, and what remains.

---

## 1) Current Goal and Guardrails

Primary objective:
- Keep scheduler/user-facing behavior correct while driving `pigz` fiber runtime toward `pigz` pthread performance parity.

Current guardrails:
- Keep API/ABI stable; change internals in `cc/runtime/fiber_sched.c`.
- Prefer evidence-driven changes (counter deltas + stress outcomes), not wake/pop tuning by intuition alone.
- When a change improves counters but regresses stress liveness, revert and continue from last known-good baseline.

Current known state:
- `smoke`, `stress-check`, and `perf-check` are currently green in this worktree.
- Startup instability in `work_stealing_race` / `join_init_race` is resolved by
  explicit `STARTUP -> RUN` phase promotion.
- A follow-on flake in `stress/block_combinators_stress.ccs` was fixed in
  `cc/runtime/task.c` by correcting `cc_block_any` to be success-seeking
  (first non-negative result), not first-completion.

---

## 2) Files and Why They Matter

Primary edit surface:
- `cc/runtime/fiber_sched.c`
  - Worker sleep/wake loop, non-worker spawn routing, global pop paths, replacement worker behavior.
  - Diagnostic counters (`CC_WORKER_GAP_STATS`) and optional trace hooks.

Validation and benchmark surface:
- `stress/work_stealing_race.ccs`
  - Historical intermittent timeout reproducer (now used as regression guard).
- `stress/join_init_race.ccs`
  - Historical startup-race reproducer (now used as regression guard).
- `stress/block_combinators_stress.ccs`
  - Caught combinator semantic bug (`cc_block_any` accepting first completion).
- `stress/minimal_spawn_race.ccs`
  - Sensitive to startup/starvation mistakes.
- `tools/run_all.ccs`
  - Broad correctness regression pass.
- `real_projects/pigz/Makefile`
  - Builds `pigz_idiomatic` and `pigz_pthread` used for throughput/counter comparison.
- `real_projects/pigz/testdata/text_200mb.bin`
  - Standard input payload used in comparative perf/counter runs.

Reference docs:
- `spec/scheduler-v3.md` (formal design/spec narrative)
- This file (operational history + reproducible loops)

---

## 3) Flags You Can Use (Scheduler + Debug)

The runtime has many environment knobs in `fiber_sched.c`. For this effort, these are the key ones.

### 3.1 Core Scheduler Diagnostics

- `CC_WORKER_GAP_STATS=1`
  - Enables in-memory gap counter collection.
- `CC_WORKER_GAP_STATS_DUMP=1`
  - Dumps `CC_WORKER_GAP_STATS` summary output.
- `CC_WORKER_GAP_STATS_LIVE=1`
  - Emits live periodic gap lines (`[gap-live]`) while running.
- `CC_WORKER_GAP_STATS_TRACE=1`
  - Enables verbose trace events.
- `CC_WORKER_GAP_TRACE_EVERY=<N>`
  - Trace sampling cadence control.

### 3.2 Related Counters Often Used Together

- `CC_TASK_WAIT_STATS=1`
- `CC_TASK_WAIT_STATS_DUMP=1`
  - Task wait/park related companion counters (used with pigz counter runs).

### 3.3 Runtime/Behavior Controls Sometimes Relevant

- `CC_WORKERS=<N>` (worker count override)
- `CC_SHARDED_RUNQ=<0|1>`
- `CC_UNTENDED_FIRST_STEAL=<...>`
- `CC_SPIN_FAST_ITERS=<N>`
- `CC_SPIN_YIELD_ITERS=<N>`

### 3.4 Misc Debug Flags Present in Runtime

- `CC_DEBUG_INBOX`, `CC_DEBUG_INBOX_DUMP`
- `CC_DEBUG_JOIN`, `CC_DEBUG_WAKE`
- `CC_DEBUG_DEADLOCK_RUNTIME`, `CC_DEADLOCK_ABORT`
- `CC_DEBUG_STALL`, `CC_DEBUG_SYSMON`, `CC_SYSMON`
- `CC_FIBER_STATS`, `CC_SPAWN_TIMING`

Notes:
- Use only the minimum flag set needed for a given run; too many verbose flags can alter timing and obscure race signatures.
- For flaky timeout hunting, prefer `CC_WORKER_GAP_STATS(_DUMP|_LIVE)` first.

---

## 4) Test Loops We Actually Used

The project converged using two repeated loop families:
- **A) Throughput/counter loops** (`pigz`) for performance direction.
- **B) Flake loops** (`work_stealing_race`) for liveness/correctness under stress.

### 4.1 Build Baseline Before Any Perf/Stress Loop

Always rebuild runtime first:

```sh
make -C cc
```

Then rebuild pigz binaries when used:

```sh
make -C real_projects/pigz -B pigz_idiomatic pigz_pthread
```

Reason:
- Avoid stale runtime link artifacts (`STALE RUNTIME DETECTED` surfaced previously when skipping runtime rebuild).

### 4.2 Pigz Counter Comparison Loop (Speed + Internal Behavior)

Canonical pattern used repeatedly:

```sh
cp real_projects/pigz/testdata/text_200mb.bin /tmp/pigz_idio.bin
CC_WORKER_GAP_STATS=1 CC_WORKER_GAP_STATS_DUMP=1 \
CC_TASK_WAIT_STATS=1 CC_TASK_WAIT_STATS_DUMP=1 \
./real_projects/pigz/out/pigz_idiomatic /tmp/pigz_idio.bin \
  >/tmp/pigz_idio.out 2>/tmp/pigz_idio.err

cp real_projects/pigz/testdata/text_200mb.bin /tmp/pigz_thr.bin
CC_WORKER_GAP_STATS=1 CC_WORKER_GAP_STATS_DUMP=1 \
CC_TASK_WAIT_STATS=1 CC_TASK_WAIT_STATS_DUMP=1 \
./real_projects/pigz/out/pigz_pthread /tmp/pigz_thr.bin \
  >/tmp/pigz_thr.out 2>/tmp/pigz_thr.err
```

What this loop answers:
- Are scheduler changes reducing unnecessary waits/pops in real workload?
- Are we closing distance to pthread behavior without introducing regressions?

Primary fields watched:
- `sleep_wait_calls`
- `global_pop_attempts`
- `spawn_push_nonworker_global` (often referenced as nonworker-global pushes)
- Wake-source and replacement-pop split counters

### 4.3 Stress Correctness Loop (Broad)

Used for overall safety checks:

```sh
./out/cc/bin/ccc run --release --timeout 180 tools/run_all.ccs -- examples stress
```

And targeted stress files directly as needed.

### 4.4 Flake Repro Loop (Critical, historical and regression use)

Key loop used for startup-liveness diagnosis (and now as a regression guard):

```sh
for i in $(seq 1 50); do
  echo "run $i"
  ./out/cc/bin/ccc run --timeout 10 ./stress/work_stealing_race.ccs || break
done
```

Diagnostic variant:

```sh
CC_WORKER_GAP_STATS=1 CC_WORKER_GAP_STATS_DUMP=1 CC_WORKER_GAP_STATS_LIVE=1 \
./out/cc/bin/ccc run --timeout 10 ./stress/work_stealing_race.ccs
```

Interpretation:
- Pass-rate (e.g. `46/50`, `49/50`) is used as the immediate quality signal.
- Any timeout requires preserving full stderr/stats for pass-vs-timeout comparison.

### 4.5 Startup/Spawn Race Guard Check

Used after routing/wake-policy changes:

```sh
./out/cc/bin/ccc run --timeout 10 ./stress/minimal_spawn_race.ccs
```

This catches startup starvation quickly when non-worker routing is too aggressive.

---

## 5) What We Changed (Iteration History)

All changes below are in `cc/runtime/fiber_sched.c` unless noted.

### 5.1 Instrumentation Foundation

Added/extended counters and dumps:
- Sleep wake classification:
  - `sleep_wake_no_work`
  - `sleep_wake_timeout_no_progress`
  - `sleep_wake_changed_no_work`
- Productive wake source split:
  - `sleep_wake_with_work_local`
  - `sleep_wake_with_work_inbox`
  - `sleep_wake_with_work_global`
- Non-worker publish context:
  - `spawn_nw_ready_active_zero`
  - `spawn_nw_target_sleep_lifecycle`
- Replacement/global pop diagnostics:
  - `global_pop_repl_nohint`
  - `global_pop_repl_hint_miss`
  - `global_pop_repl_spin_deferred`

Why:
- We needed to identify exactly where waits and pops came from, and whether wake activity was productive.

### 5.2 Startup Safety Fix (Important)

Change:
- Gated non-worker inbox publish readiness on active workers:
  - `pool_ready_for_nonworker_inbox_publish()` returns `active > 0`.

Why:
- Prevented startup path from publishing work into paths that were not yet progressing.
- Fixed `minimal_spawn_race` timeouts introduced by overly aggressive non-worker routing.

### 5.3 Wait-Reduction Pass

Changes:
- Reduced unconditional wakes for non-worker non-global publish path.
- Conditional wake now depends on edge/strict-idle context.
- Tightened strict-idle condition to include `spinning == 0`.

Why:
- `sleep_wait_calls` was dominated by wake churn where woken workers found no work.

Outcome:
- Major drop in wait-call volume.
- Required follow-on tuning to keep liveness robust.

### 5.4 Pop-Reduction Pass (Replacement Worker)

Changes:
- Added no-hint fast-idle behavior for replacement worker.
- Throttled replacement global hint/pop checks in spin loop.

Why:
- `global_pop_attempts` remained high due to replacement probing, often without useful work.

Outcome:
- Significant reduction in global pop churn.

### 5.5 Liveness Flake Investigations (`work_stealing_race`)

Attempted changes:
- Allowing additional wake paths for specific global-edge contexts.
- Sparse wake nudges for persistent sleepers.
- Opportunistic pre-sleep steal probe.
- Targeted stall-signature rescue wake.

Observed:
- Some ideas improved flake rate temporarily.
- Time-based persistent-sleeper nudge regressed flake behavior and wake traffic.
- Rescue-signature counters showed non-trigger (`0`) in tested failures, indicating wrong signature.

Action taken:
- Reverted failed/time-regressive rescue direction(s) and retained better baseline behavior.

---

## 6) How We Measured Performance vs Correctness

### Performance loop (pigz)

Decision metric:
- Not just wall-clock; also scheduler behavior counters under realistic throughput load.

Why pigz:
- High-concurrency, mixed spawn/sync workload exposing scheduler wake/pop behavior under pressure.

Cycle:
1. Rebuild runtime + pigz.
2. Run idiomatic and pthread variants on same input size (typically 200MB).
3. Capture `CC_WORKER_GAP_STATS` and `CC_TASK_WAIT_STATS`.
4. Compare:
   - waits (sleep churn)
   - pops (probe churn)
   - non-worker global spill behavior
5. Keep only changes that improve counters without stress regressions.

### Correctness loop (stress)

Decision metric:
- Timeout/no-timeout across repeated runs, not single-pass success.

Cycle:
1. Targeted repro loops (10s timeout) for `work_stealing_race`.
2. Repeat counts (`20`, `30`, `50` runs) to estimate flake rate.
3. Cross-check other stress tests and broad suite (`run_all`) for collateral regressions.
4. Revert any change that trades widespread correctness risk for benchmark counter gains.

---

## 7) What Is Left

### 7.1 Immediate unresolved issue

- Rare timeout in `stress/work_stealing_race.ccs` still exists.
- This appears to be a specific liveness edge, not broad scheduler throughput collapse.

### 7.2 Next high-value step (data capture first, then patch)

Run a controlled pass-vs-timeout evidence capture:
1. Execute repeated `work_stealing_race` attempts with:
   - `CC_WORKER_GAP_STATS=1`
   - `CC_WORKER_GAP_STATS_DUMP=1`
   - `CC_WORKER_GAP_STATS_LIVE=1`
2. Save one clean PASS sample and one TIMEOUT sample (stdout/stderr).
3. Diff these fields directly:
   - sleep enters/waits/wait-average
   - wake exits breakdown
   - global pop attempts/hits + callsite split
   - replacement nohint/hint_miss/spin_deferred
   - wake-source lines and nonworker publish lines
   - last `[gap-live]` lines before timeout
4. Patch only the mechanism implicated by this differential signature.

This avoids another blind tuning loop.

### 7.3 Acceptance criteria for next patch

- `work_stealing_race` materially better pass ratio across 50+ runs.
- No regression on `minimal_spawn_race`.
- Pigz counters remain in current low-wait/low-pop envelope.
- `tools/run_all.ccs` stress coverage remains green.

---

## 8) Practical Resume Checklist

If session crashes and you need to restart fast:

1. Rebuild:
   - `make -C cc`
   - `make -C real_projects/pigz -B pigz_idiomatic pigz_pthread`
2. Run one pigz counter pair and archive outputs in `/tmp`.
3. Run `work_stealing_race` loop (`50x`, timeout 10s) with gap stats enabled.
4. If timeout occurs, preserve first timeout artifacts before rerunning.
5. Compare pass vs timeout signatures before editing code.
6. Apply minimal targeted patch in `cc/runtime/fiber_sched.c`.
7. Re-run:
   - `stress/minimal_spawn_race.ccs`
   - `stress/work_stealing_race.ccs` loop
   - `tools/run_all.ccs` stress path
   - pigz counter pair

---

## 9) Notes on Scope Discipline

Touching for this effort should remain concentrated in:
- `cc/runtime/fiber_sched.c` (scheduler behavior + instrumentation)

Avoid widening scope unless evidence requires it:
- Do not mix unrelated refactors while chasing liveness flakes.
- Keep one behavioral hypothesis per patch to preserve rollback clarity.

This discipline is what made iteration loops fast and reversible.

---

## 10) Current Diff Triage (Keep vs Revert)

This section reflects the currently observed dirty diff in `cc/runtime/fiber_sched.c`.

### 10.1 Keep (high signal, low risk, or required for diagnosis)

- Instrumentation counters and dump formatting additions:
  - sleep productive source split (`local/inbox/global`)
  - sleep no-work split (`timeout_no_progress` vs `changed_no_work`)
  - replacement pop diagnostics (`repl_nohint`, `repl_hint_miss`, `repl_spin_deferred`)
  - non-worker publish attempt/result counters
- Live/trace plumbing:
  - `CC_WORKER_GAP_STATS_LIVE`
  - `CC_WORKER_GAP_STATS_TRACE`
  - `CC_WORKER_GAP_TRACE_EVERY`
- Startup guard:
  - non-worker inbox publish only when `active > 0`
- Strict-idle gate including spinner state:
  - `sleeping > 0 && active == 0 && spinning == 0`

Reason:
- These changes gave actionable visibility and fixed known startup starvation without broad semantic risk.

### 10.2 Keep with Caution (behavioral, but currently useful)

- Replacement worker global-pop throttling/no-hint fast-idle behavior.
- Conditional wake adjustments around global-edge/non-global paths.
- Sparse streak-based non-worker nudge.

Reason:
- These affect liveness/perf balance directly. Keep while collecting evidence, but treat as tuning knobs subject to immediate revert if timeout signature worsens.

### 10.3 Revert Candidates First if Flake Worsens

- Any wake-policy broadening that increases unproductive wake churn.
- Any new periodic/time-based nudge replacing streak-gated sparse nudge.
- Any rescue path that does not fire in failing signatures (counter remains zero) but still adds wake traffic.

Operational rule:
- Revert the smallest behavior delta first; do not remove instrumentation while debugging.

---

## 11) Durable Data Capture Recipe (Single-Directory Artifacts)

Goal:
- Capture one PASS and one TIMEOUT sample for `work_stealing_race`, plus a compact extracted diff, in a durable folder under `tmp/wsr-capture/`.

### 11.1 Run Capture Script

From repo root:

```sh
STAMP=$(date +%Y%m%d-%H%M%S)
OUTDIR="tmp/wsr-capture/$STAMP"
mkdir -p "$OUTDIR"

cat > "$OUTDIR/run_capture.sh" <<'SH'
#!/usr/bin/env sh
set -eu

ROOT="$(pwd)"
OUTDIR="${1:?usage: run_capture.sh <outdir> [max_runs] [timeout_s]}"
MAX_RUNS="${2:-120}"
TIMEOUT_S="${3:-10}"

mkdir -p "$OUTDIR"
echo "outdir=$OUTDIR" > "$OUTDIR/summary.txt"
echo "max_runs=$MAX_RUNS timeout=${TIMEOUT_S}s" >> "$OUTDIR/summary.txt"

PASS_FOUND=0
TIMEOUT_FOUND=0

i=1
while [ "$i" -le "$MAX_RUNS" ]; do
  LOG="$OUTDIR/run_${i}.log"
  echo "=== run $i/$MAX_RUNS ===" | tee -a "$OUTDIR/summary.txt"

  if env CC_WORKER_GAP_STATS=1 CC_WORKER_GAP_STATS_DUMP=1 CC_WORKER_GAP_STATS_LIVE=1 \
      ./out/cc/bin/ccc run --timeout "$TIMEOUT_S" ./stress/work_stealing_race.ccs \
      >"$LOG" 2>&1; then
    if [ "$PASS_FOUND" -eq 0 ]; then
      cp "$LOG" "$OUTDIR/pass_sample.log"
      PASS_FOUND=1
      echo "captured PASS at run $i" | tee -a "$OUTDIR/summary.txt"
    fi
  else
    if [ "$TIMEOUT_FOUND" -eq 0 ]; then
      cp "$LOG" "$OUTDIR/timeout_sample.log"
      TIMEOUT_FOUND=1
      echo "captured TIMEOUT at run $i" | tee -a "$OUTDIR/summary.txt"
    fi
  fi

  if [ "$PASS_FOUND" -eq 1 ] && [ "$TIMEOUT_FOUND" -eq 1 ]; then
    break
  fi
  i=$((i + 1))
done

if [ "$PASS_FOUND" -eq 0 ]; then
  echo "WARNING: no PASS captured" | tee -a "$OUTDIR/summary.txt"
fi
if [ "$TIMEOUT_FOUND" -eq 0 ]; then
  echo "WARNING: no TIMEOUT captured" | tee -a "$OUTDIR/summary.txt"
fi

extract_metrics() {
  IN="$1"
  OUT="$2"
  if [ ! -f "$IN" ]; then
    return 0
  fi
  {
    echo "# from $IN"
    rg "sleep:|sleep exits|global pop|replacement pop probes|wake sources|spawn nonworker publish|\\[gap-live\\]" "$IN" || true
  } > "$OUT"
}

extract_metrics "$OUTDIR/pass_sample.log" "$OUTDIR/pass_metrics.txt"
extract_metrics "$OUTDIR/timeout_sample.log" "$OUTDIR/timeout_metrics.txt"

{
  echo "## pass vs timeout extracted metrics"
  echo
  echo "### pass"
  [ -f "$OUTDIR/pass_metrics.txt" ] && cat "$OUTDIR/pass_metrics.txt" || true
  echo
  echo "### timeout"
  [ -f "$OUTDIR/timeout_metrics.txt" ] && cat "$OUTDIR/timeout_metrics.txt" || true
} > "$OUTDIR/diff_report.txt"

echo "done" | tee -a "$OUTDIR/summary.txt"
SH

chmod +x "$OUTDIR/run_capture.sh"
"$OUTDIR/run_capture.sh" "$OUTDIR" 120 10
```

### 11.2 Expected Artifacts

- `summary.txt`
- `run_capture.sh`
- `run_<N>.log` files
- `pass_sample.log` (if observed)
- `timeout_sample.log` (if observed)
- `pass_metrics.txt`
- `timeout_metrics.txt`
- `diff_report.txt`

### 11.3 How to Use the Artifacts

- Treat `pass_sample.log` and `timeout_sample.log` as canonical evidence.
- Patch only where metrics diverge materially.
- Keep artifacts for each iteration; do not overwrite prior capture folders.

---

## 12) Latest Counter Snapshot (Fiber vs Pthread)

This snapshot preserves the latest apples-to-apples run captured in this session.

### 12.1 Run Setup

- Runtime and targets rebuilt before capture:
  - `make -C cc`
  - `make -C real_projects/pigz -B pigz_idiomatic pigz_pthread`
- Input size:
  - `real_projects/pigz/testdata/text_200mb.bin`
- Environment:
  - `CC_WORKER_GAP_STATS=1`
  - `CC_WORKER_GAP_STATS_DUMP=1`
  - `CC_TASK_WAIT_STATS=1`
  - `CC_TASK_WAIT_STATS_DUMP=1`
- Captured files:
  - Fiber: `/tmp/pigz_idio_current.err`
  - Pthread: `/tmp/pigz_thr_current.err`

Command pattern used:

```sh
cp real_projects/pigz/testdata/text_200mb.bin /tmp/pigz_idio_current.bin
CC_WORKER_GAP_STATS=1 CC_WORKER_GAP_STATS_DUMP=1 \
CC_TASK_WAIT_STATS=1 CC_TASK_WAIT_STATS_DUMP=1 \
./real_projects/pigz/out/pigz_idiomatic /tmp/pigz_idio_current.bin \
  >/tmp/pigz_idio_current.out 2>/tmp/pigz_idio_current.err

cp real_projects/pigz/testdata/text_200mb.bin /tmp/pigz_thr_current.bin
CC_WORKER_GAP_STATS=1 CC_WORKER_GAP_STATS_DUMP=1 \
CC_TASK_WAIT_STATS=1 CC_TASK_WAIT_STATS_DUMP=1 \
./real_projects/pigz/out/pigz_pthread /tmp/pigz_thr_current.bin \
  >/tmp/pigz_thr_current.out 2>/tmp/pigz_thr_current.err
```

### 12.2 Side-by-Side Counters

| Metric | `pigz_idiomatic` | `pigz_pthread` |
|---|---:|---:|
| sleep enters | 29 | 8 |
| sleep waits | 760 | 1103 |
| sleep wait avg | 513030.871 us | 516615.805 us |
| wake_with_work | 21 | 0 |
| wake_no_work | 738 | 1103 |
| global pop attempts/hits | 57 / 57 | 2 / 2 |
| global pop callsites | fairness 1/1, mainloop 7/7, replacement 853/49 | fairness 0/0, mainloop 2/2, replacement 0/0 |
| replacement probes | nohint 55524, hint_miss 0, spin_deferred 1802496 | all 0 |
| spawn nonworker publish | attempts 871, inbox_success 869, global_fallback 2 | attempts 2, inbox_success 0, global_fallback 2 |
| join_thread wake cond/fired | 2 / 2 | 2 / 1 |

### 12.3 Raw Excerpts (Preserved)

Fiber (`/tmp/pigz_idio_current.err`):
- `sleep: enters=29 waits=760 wait_total=389903.462 ms wait_avg=513030.871 us`
- `sleep exits: post_recheck_hits=0 wake_with_work=21 [local=0 inbox=21 global=3] wake_no_work=738 (timeout=691 changed=47)`
- `global pops: attempts=57 hits=57 hit_rate=100.0% hint_skips=0`
- `global pop callsites: fairness=1/1 mainloop=7/7 replacement=853/49`
- `replacement pop probes: nohint=55524 hint_miss=0 spin_deferred=1802496`
- `spawn nonworker publish: attempts=871 ready=869 blocked=2 inbox_success=869 global_fallback=2 forced_spill=0 wake(cond=6 uncond=0 all=0) ready_active0=0 target_sleep=0`

Pthread (`/tmp/pigz_thr_current.err`):
- `sleep: enters=8 waits=1103 wait_total=569827.233 ms wait_avg=516615.805 us`
- `sleep exits: post_recheck_hits=0 wake_with_work=0 [local=0 inbox=0 global=0] wake_no_work=1103 (timeout=1098 changed=5)`
- `global pops: attempts=2 hits=2 hit_rate=100.0% hint_skips=0`
- `global pop callsites: fairness=0/0 mainloop=2/2 replacement=0/0`
- `replacement pop probes: nohint=0 hint_miss=0 spin_deferred=0`
- `spawn nonworker publish: attempts=2 ready=0 blocked=2 inbox_success=0 global_fallback=2 forced_spill=0 wake(cond=0 uncond=0 all=0) ready_active0=0 target_sleep=0`

### 12.4 Interpretation (Current)

- Fiber path remains significantly more scheduler-active than pthread, especially in replacement-worker probing (`nohint` + `spin_deferred` volume).
- Successful global pop count is modest (`57`) despite high probe traffic, which indicates probe overhead/traffic concentration remains a primary optimization target.
- This snapshot should be treated as a point-in-time baseline for the next iteration; compare against future runs using the same 200MB input and flags.

---

## 13) Current Regression: `join_init_race` Timeouts

This is an active blocker and not a one-off harness artifact.

### 13.1 Reproduction Evidence

Observed in `run_all`:
- `stress/join_init_race.ccs ... FAIL`
- stderr: `cc: run timed out after 10 seconds`

Standalone flake checks (same binary/worktree):
- `./out/cc/bin/ccc run --timeout 10 ./stress/join_init_race.ccs`
  - `20` runs => `7 pass / 13 fail`
- `./out/cc/bin/ccc run --timeout 30 ./stress/join_init_race.ccs`
  - `10` runs => `3 pass / 7 fail`

Interpretation:
- This is not just “too close to 10s”; failures persist at 30s.
- Strong indication of liveness/race issue.

### 13.2 Captured Failure Signature (Live Gap Stats)

Capture command:

```sh
env CC_WORKER_GAP_STATS=1 CC_WORKER_GAP_STATS_DUMP=1 CC_WORKER_GAP_STATS_LIVE=1 \
  ./out/cc/bin/ccc run --timeout 30 ./stress/join_init_race.ccs
```

Failing sample (`tmp/wsr-capture/join-init-live/run_2.err`):

- `[gap-live] tag=spawn_nw pending=0 active=0 spinning=6 sleeping=0 sleep_wait_calls=0 ... global_pop_attempts=0 nw(attempts=1 inbox=0 global=0 wake_cond=0 wake_uncond=0 active0=0 target_sleep=0)`
- `cc: run timed out after 30 seconds`

Why this matters:
- Timeout occurred with no end-of-run counter dump (killed by harness timeout).
- The only live state line suggests early startup/progress pocket:
  - no pending work seen
  - no active workers
  - some workers marked spinning
  - first non-worker publish attempt not yet reflected as inbox/global success

### 13.3 Contrasting Passing Sample (Same Diagnostic Mode)

Passing sample (`tmp/wsr-capture/join-init-live/run_1.err`) did emit full stats:
- `spawn nonworker publish: attempts=18000 ready=954 blocked=17046 inbox_success=954 global_fallback=17046 ... ready_active0=8`
- `global pop callsites: fairness=54/56 mainloop=9/9 replacement=4550/4545`
- `sleep exits: ... wake_with_work=1817 ...`

Takeaway:
- Passing runs show high, continuous publish/pop activity and eventual completion.
- Failing run appears to stall in an early non-worker spawn/publish phase.

### 13.4 Immediate Debug Direction

- Focus on startup/non-worker publish progress when `active==0` but workers are spinning.
- Capture additional failing runs with `CC_WORKER_GAP_STATS_LIVE=1` and preserve first few `[gap-live]` lines.
- Compare failing early-state signatures against passing runs in first 100ms window.

---

## 14. Startup State-Machine Latch (Latest Iteration)

Date: 2026-02-12

### 14.1 Change

Implemented a one-way scheduler startup latch in `cc/runtime/fiber_sched.c` for non-worker spawn routing:

- New scheduler field: `_Atomic int nonworker_inbox_phase` (`0=bootstrap`, `1=run`).
- `pool_ready_for_nonworker_inbox_publish()` now behaves as:
  - return `0` while in bootstrap and `active==0` (force non-worker publishes to global path),
  - atomically promote to run phase once `active>0` is observed,
  - never demote back to bootstrap (one-way transition).

Rationale:
- Makes startup behavior explicit and monotonic.
- Removes threshold-driven/oscillatory readiness checks during early startup storm windows.
- Keeps launch-order-friendly global fallback until workers are demonstrably executing.

### 14.2 Validation

Build:

```sh
make -C cc
```

Primary loops (responsive per-iteration runs):

```sh
# join_init_race @ 3s
for i in $(seq 1 15); do ./out/cc/bin/ccc run --timeout 3 ./stress/join_init_race.ccs; done

# work_stealing_race @ 10s
for i in $(seq 1 12); do ./out/cc/bin/ccc run --timeout 10 ./stress/work_stealing_race.ccs; done
```

Observed:
- `join_init_race` (3s): `15/15` pass
- `work_stealing_race` (10s): `12/12` pass
- `minimal_spawn_race` (10s): pass

Confidence expansion:
- `join_init_race` (3s): `30/30` pass
- `work_stealing_race` (10s): `20/20` pass
- `minimal_spawn_race` (10s): pass

### 14.3 Notes

- A prior variant that forced unconditional wakes for bootstrap global publishes regressed `work_stealing_race` and was reverted.
- The current version keeps wake policy unchanged; only readiness state handling was changed.

---

## 15. Wake Churn Root Cause + Reduction

Date: 2026-02-12

### 15.1 Where the extra waits/wakes came from

From `pigz_idiomatic` counters (pre-change):

- `wake sources`:
  - `spawn_non_global cond=758 fired=751`
- `spawn nonworker publish`:
  - `attempts=790 inbox_success=789 global_fallback=1 wake(cond=758 uncond=0)`
- `sleep exits`:
  - `wake_no_work=575`

Interpretation:
- Most wake traffic was from non-worker inbox publications (`spawn_non_global`) and mostly led to empty wake exits.
- This was wake amplification from publish-time policy, not from global pops.

### 15.2 Change

File: `cc/runtime/fiber_sched.c`

In `cc_fiber_spawn`, non-worker non-global wake policy was narrowed:

- Track publish target worker (`nonworker_target_worker`).
- For `nonworker_nonglobal_publish`, issue wake only when:
  - publish created an edge **and target worker lifecycle is `SLEEP`**, or
  - pool is in strict idle (`sleeping>0 && active==0 && spinning==0`).

This suppresses wakes when publishing to already-active targets.

### 15.3 Counter impact (same pigz capture procedure)

`pigz_idiomatic` before -> after:

- `spawn_non_global cond/fired`: `758/751` -> `0/0`
- `spawn nonworker publish wake(cond/uncond)`: `758/0` -> `0/2`
- `sleep waits`: `617` -> `43`
- `sleep wake_no_work`: `575` -> `36`

`pigz_pthread` remains low-activity and unchanged directionally.

### 15.4 Correctness/flake checks after reduction

- `join_init_race` @3s:
  - `15/15` pass
  - expanded: `30/30` pass
- `work_stealing_race` @10s:
  - `12/12` pass
  - expanded: `20/20` pass
- `minimal_spawn_race` @10s: pass

### 15.5 Regression found and rollback note

During broader smoke execution (`./scripts/test.sh`), `chan_buffered_wakeup_smoke` timed out at 10s.

Observed signature:
- test stuck at `iter 0/1000`
- live gap stats repeatedly showed startup/idle pocket with `active=0` and sleepers present.

Root cause:
- The one-way `active>0` startup phase latch (`nonworker_inbox_phase`) introduced a liveness regression for this buffered channel wakeup scenario.

Action taken:
- Reverted to the prior startup readiness gate:
  - require scheduler presence (`active>0 || spinning>0`)
  - and while `active==0`, require `pending>=workers` before non-worker inbox publish
- Removed the `nonworker_inbox_phase` field/initialization.

Verification:
- `chan_buffered_wakeup_smoke` passes again under `cc_test` with 10s timeout.

---

## 16. Channel-Path Startup Bridge Iteration

Date: 2026-02-12

### 16.1 Objective

- Continue the startup state-machine direction while avoiding scheduler-wide readiness changes that repeatedly regressed buffered channel startup.
- Test a channel-specific bridge instead of changing non-worker spawn routing semantics.

### 16.2 Attempt A (rejected): monotonic scheduler progress token

File:
- `cc/runtime/fiber_sched.c`

Change:
- Added a one-way startup latch (`startup_progress_seen`) and made `pool_ready_for_nonworker_inbox_publish()` permanently ready after first successful worker-run.
- Kept startup gate behavior unchanged prior to first observed worker progress.

Result:
- `chan_buffered_wakeup_smoke`: `0/8` pass (hard regression)
- `join_init_race` @3s: `15/15` pass
- `work_stealing_race` @10s: `15/15` pass

Conclusion:
- Scheduler-level monotonic readiness still breaks buffered-channel startup behavior in this workload shape, even when startup preconditions are preserved.
- This variant was reverted.

### 16.3 Attempt B: channel wake bridge only

Files:
- `cc/runtime/channel.c`
- `cc/runtime/fiber_sched_boundary.h`
- `cc/runtime/fiber_sched_boundary.c`
- `cc/runtime/fiber_internal.h`
- `cc/runtime/fiber_sched.c`

Change:
- Added channel-specific wake boundary:
  - `cc_sched_fiber_wake_channel()`
  - `cc__fiber_unpark_channel()`
- Routed channel wake-batch flush through this path only.
- In unpark wake stage, channel path applies a narrow startup bridge:
  - if pool is in startup spin/no-sleep pocket (`active==0 && spinning>0 && sleeping==0`),
  - issue `wake_one_if_sleeping_unconditional(...)` to bump wake generation and avoid missed early handoff transitions.

Rationale:
- Keep scheduler spawn/readiness policy unchanged.
- Add a startup bridge only where channel handoff wake admission occurs.

Validation:
- `chan_buffered_wakeup_smoke`: `8/8` pass
- `join_init_race` @3s: `13/15` pass
- `work_stealing_race` @10s: `13/15` pass

Interpretation:
- Channel startup liveness is preserved.
- Race tests remain flaky (not solved), but this path avoids the catastrophic channel regression from scheduler-latch attempts.

### 16.4 Channel bridge attribution counters

Added counters in `CC_WORKER_GAP_STATS`:
- `unpark_chan_bridge_calls`
- `unpark_chan_bridge_startup_pocket`
- `unpark_chan_bridge_startup_sleepers`

Dump line:
- `channel startup bridge: calls=... startup_pocket=... startup_pocket_sleepers=...`

Readout snapshots:
- `tests/chan_buffered_wakeup_smoke.ccs`:
  - `channel startup bridge: calls=1827 startup_pocket=0 startup_pocket_sleepers=0`
- `stress/join_init_race.ccs`:
  - `channel startup bridge: calls=0 startup_pocket=0 startup_pocket_sleepers=0`
- `stress/work_stealing_race.ccs` (passing run):
  - `channel startup bridge: calls=0 startup_pocket=0 startup_pocket_sleepers=0`

Interpretation:
- Channel wake-path unparks are active (`calls` non-zero on channel smoke).
- The startup-pocket predicate has not fired in these captures, so the current bridge is mostly a no-op with respect to unconditional startup wake nudges.

### 16.5 Bridge behavior removal (keep attribution)

Change:
- Removed the channel bridge unconditional wake behavior in `cc__fiber_unpark`:
  - deleted `wake_one_if_sleeping_unconditional(...)` under the channel startup-pocket branch.
- Kept channel-path routing and attribution counters intact.

Validation:
- `chan_buffered_wakeup_smoke`: `8/8` pass
- `join_init_race` @3s: `12/15` pass
- `work_stealing_race` @10s: `10/15` pass

Counter snapshots:
- `join_init_race`:
  - `channel startup bridge: calls=0 startup_pocket=0 startup_pocket_sleepers=0`
- `chan_buffered_wakeup_smoke`:
  - `channel startup bridge: calls=4736 startup_pocket=0 startup_pocket_sleepers=0`

Conclusion:
- Removing bridge wake behavior did not introduce a channel regression.
- Startup-pocket path remains unfired in sampled runs, reinforcing that this was not an active control point for observed failures.

### 16.6 Remove wrapper path, keep counters

Change:
- Removed channel-specific wake wrapper plumbing:
  - `channel.c` wake batch now calls `cc_sched_fiber_wake(...)` directly.
  - removed `cc_sched_fiber_wake_channel(...)` declarations/definitions.
  - removed `cc__fiber_unpark_channel(...)` declaration/definition and TLS bridge flag.
- Kept bridge attribution counters by adding a counter-only hook:
  - `cc__fiber_unpark_channel_attrib()` called from channel wake batch path before wake dispatch.
  - No wake policy behavior changed by this hook.

Validation:
- `chan_buffered_wakeup_smoke`: `8/8` pass
- `join_init_race` @3s: `11/15` pass
- `work_stealing_race` @10s: `8/15` pass

Counter snapshots:
- `tests/chan_buffered_wakeup_smoke.ccs`:
  - `channel startup bridge: calls=19997996 startup_pocket=0 startup_pocket_sleepers=0`
- `stress/join_init_race.ccs` (passing sample):
  - `channel startup bridge: calls=0 startup_pocket=0 startup_pocket_sleepers=0`

Interpretation:
- The wrapper path is gone; counters still report from channel wake callsite.
- Startup-pocket still does not fire in sampled runs (`startup_pocket=0`), so bridge-startup predicate remains non-participating.

### 16.7 Startup uncond wake storm reduction

Change (`cc/runtime/fiber_sched.c`):
- Kept startup non-worker global liveness path, but constrained it:
  - emit unconditional wake only in startup spin/no-sleep pocket,
  - gate to one emission per observed wake generation,
  - and only while startup backlog is small (`pending <= workers`).
- Kept channel attribution counters and changed their implementation to low-overhead batched TLS flushes.

Why:
- `spawn nonworker publish ... wake(uncond=...)` remained high in failing runs and looked like startup wake storm pressure rather than productive wakeups.

Observed validation:
- `chan_buffered_wakeup_smoke`: consistently `8/8` pass during this iteration.
- stress matrix sample:
  - `join_init_race` @3s: `12/15` (also saw `14/15` in adjacent run)
  - `work_stealing_race` @10s: `13/15` (adjacent run showed `12/15`)

Current counter snapshots:
- `join_init_race`:
  - `spawn nonworker publish: ... wake(cond=2055 uncond=96 ...)`
- `work_stealing_race`:
  - `spawn nonworker publish: ... wake(cond=80464 uncond=337 ...)`
- `channel startup bridge: calls=0 startup_pocket=0 startup_pocket_sleepers=0`

Interpretation:
- Startup unconditional wake volume dropped substantially vs earlier high-thousands captures.
- Channel behavior stayed stable.
- Flake is reduced but not eliminated; remaining issue likely elsewhere in startup/routing/lifecycle partitioning.

### 16.8 Conditional wake split + run-phase target-sleep gate

Change (`cc/runtime/fiber_sched.c`):
- Added split counters for non-worker conditional wakes:
  - `spawn_nw_wake_cond_edge_only`
  - `spawn_nw_wake_cond_strict_idle`
  - `spawn_nw_wake_cond_target_sleep`
- Updated live/dump prints to include `cond` breakdown.
- Tightened non-worker non-global conditional wake policy:
  - when `active > 0`, edge-driven wake now requires target worker lifecycle `SLEEP`;
  - strict-idle wake remains as backstop.

Validation:
- `chan_buffered_wakeup_smoke`: `8/8` pass
- `join_init_race` @3s: `11/15` pass
- `work_stealing_race` @10s: `11/15` pass

Counter snapshots:
- `join_init_race`:
  - `wake(cond=1917 edge=1917 idle=0 tsleep=0 uncond=248 ...)`
- `work_stealing_race`:
  - `wake(cond=36503 edge=36466 idle=37 tsleep=0 uncond=97 ...)`

Interpretation:
- Conditional wake volume dropped vs previous captures, especially in `work_stealing_race`.
- `target_sleep` path remains effectively unused (`0` in samples), indicating lifecycle-based target-sleep wake eligibility is rarely met in these runs.

### 16.9 Startup edge-active0 budget attempt (reverted)

Attempt:
- Added `edge_active0`/`edge_active_gt0` split counters.
- Tried gating non-worker non-global `edge_active0` wakes by:
  - `pending <= workers`
  - and one-per-wake-generation admission.

Outcome:
- `chan_buffered_wakeup_smoke`: stayed `8/8`
- `join_init_race`: regressed (`10/15` in sample loop)
- `work_stealing_race`: no clear gain (`11/15` in sample loop)
- Counter signature showed over-suppression of conditional edge wakes with compensating unconditional wake growth in startup-heavy runs.

Action:
- Reverted the `edge_active0` budget gate logic.
- Kept the new split counters, as they remain useful for attribution.

### 16.10 Wake-policy startup latch retry (reverted)

Attempt:
- Reintroduced a one-way startup latch, but scoped it to wake policy only (no routing changes):
  - suppress startup-specific non-worker wake branches after first successful worker-run.
- Added temporary `startup_suppressed` attribution for suppressed wake cases.

Observed:
- `chan_buffered_wakeup_smoke`: remained stable (`8/8`).
- `join_init_race` improved in one sample (`13/15`) but `work_stealing_race` regressed badly in the same run (`9/15`) and stayed noisy across follow-up samples.
- Snapshot showed extreme suppression profile (very low cond/uncond wake with huge suppressed count), indicating over-gating.

Action:
- Reverted this startup-latch wake gating and removed temporary startup-suppressed instrumentation.
- Returned to prior wake policy with split counters retained.

### 16.11 Publish->first-run latency attribution (new evidence)

Change (`cc/runtime/fiber_sched.c`):
- Added spawn publish metadata on `fiber_task`:
  - publish timestamp (`spawn_publish_tsc`)
  - publish route (`local` / `inbox` / `global`)
  - publish phase (`active==0` vs `active>0`)
- Added first-run capture in `worker_run_fiber` for first execution of a spawned fiber:
  - samples / total cycles / max cycles / missing metadata
  - per-route + per-phase sample/cycle aggregates
- Added dump lines:
  - `spawn->first-run latency: samples=... avg=... max=... missing_meta=...`
  - `route/phase: local[...] inbox[...] global[...]`

Observed snapshots:
- `join_init_race` passing snapshot:
  - `avg=21741 cyc`, `max=298051`, `missing_meta=0`
  - `inbox[a0=1851@2309]`, `global[a0=15668@24675]`
- `work_stealing_race` passing snapshot:
  - `avg=47270 cyc`, `max=10580016`, `missing_meta=54`
  - `inbox[a0=51716@58583]`, `inbox[a>0=302984@50991]`
- `work_stealing_race` stressed/live snapshot (still passing, but degraded):
  - `avg=100054 cyc`, `max=11358122`, `missing_meta=980`
  - `inbox[a0=25970@400816]` (very high)
  - `inbox[a>0=300507@95173]`
- `work_stealing_race` timeout snapshot:
  - only initial live line (`pending=0 active=0 spinning=8 sleeping=0`) then timeout.

Interpretation:
- Evidence now points at publish->run lag, primarily on `inbox` route during/near startup (`active==0`) and in degraded runs generally.
- Timeouts can occur before steady live counters advance, consistent with an early startup/run admission stall rather than pure wake-count imbalance.

### 16.12 Startup inbox budget + deterministic global spill

Change (`cc/runtime/fiber_sched.c`):
- Added startup cap for non-worker inbox admission:
  - when `nw_ready && active==0 && pending > workers`, force deterministic global fallback.
- Reused existing counter `spawn_nw_global_forced_spill` to track these forced diversions.

Validation:
- `chan_buffered_wakeup_smoke`: `8/8`
- `join_init_race` @3s: `13/15`
- `work_stealing_race` @10s: `11/15`

Counter/latency snapshots:
- `join_init_race`:
  - `spawn nonworker publish ... forced_spill=0`
  - `spawn->first-run latency avg=7390 cyc`
  - `inbox[a0=1745@2489]`, `global[a0=15710@8119]`
- `work_stealing_race`:
  - `spawn nonworker publish ... forced_spill=35128`
  - `spawn->first-run latency avg=57816 cyc`
  - `inbox[a0=5599@6300]` (much lower startup inbox lag)
  - `global[a0=117219@102468]` (startup load shifted to expensive global route)

Interpretation:
- Startup inbox stall pressure was reduced (clear drop in inbox `a0` latency).
- But the spill is large enough that startup load migrates to high-latency global handling; net flake improves but is not solved.

### 16.13 Where exactly (current evidence)

From fresh pass/degraded captures with the latency split:

- Passing `work_stealing_race` sample:
  - `global[a0] split: forced=28331@11197.6 natural=84826@2560.7`
- Degraded/near-timeout `work_stealing_race` sample:
  - `global[a0] split: forced=30582@42923.0 natural=84524@2303.6`
  - `forced spill context: pending_avg=116.6 pending_max=1597`

What this localizes:
- The expensive leg is specifically **forced global startup traffic** (`global a0 forced`), not global traffic in general (`global a0 natural` stays low).
- Forced global startup lane can jump to ~16-19x the natural global latency in bad runs.

Failure signature:
- Timeout captures can occur right after the first startup publish live line (`pending=0 active=0 spinning=8 sleeping=0`) with no follow-on progress lines.
- With full trace enabled, failures were not reproduced in 20 attempts (timing-sensitive startup race behavior).

### 16.14 Global a0 phase split (publish->pop vs pop->run)

Change:
- Added split counters for `global[a0]` latency buckets:
  - forced spill: `publish->pop` and `pop->run`
  - natural global: `publish->pop` and `pop->run`
- Hooked timestamp at global pop time and measured phase deltas on first run.

Observed `work_stealing_race` passing snapshot:
- `global[a0] split: forced=20763@9932.1 natural=89424@2300.2`
- `global[a0] phase split: forced[publish->pop=5231@7048.8 pop->run=5231@179.0] natural[publish->pop=69552@1712.9 pop->run=69552@61.7]`

Observed `join_init_race` snapshot:
- `global[a0] phase split: natural[publish->pop=10240@7732.4 pop->run=10240@37.7]`

Interpretation:
- The dominant cost is now clearly **publish->global-pop service latency**.
- `pop->run` is comparatively tiny in both forced and natural lanes.
- So the bottleneck is queue service/selection cadence for startup-global traffic, not post-pop worker claim/run handoff.

### 16.15 Forced-spill shard distribution attempt

Change:
- For `startup_force_global` spills, changed fallback shard preference from pinned `0` to target-affine shard (`global_preferred=target`) to avoid single-shard collapse.

Validation:
- `chan_buffered_wakeup_smoke`: `8/8`
- `join_init_race` @3s: `15/15`
- `work_stealing_race` @10s: `12/15`

Counter/latency snapshot (`work_stealing_race`):
- `forced_spill=40668`, `pending_avg=300.6`, `pending_max=1999`
- `global[a0] split: forced=40482@615376.4 natural=77171@4872.2`
- `global[a0] phase split: forced[publish->pop=14086@1113705.7 pop->run=14086@55.9] natural[publish->pop=53650@3267.9 pop->run=53650@81.6]`

Interpretation:
- Distribution tweak improved `join_init_race` but can catastrophically inflate forced-lane `publish->pop` latency under heavy forced spill pressure.
- Root cause remains forced startup spill volume + service lag (publish->pop), not pop->run.

### 16.16 Startup spill token/hysteresis shaping

Change (`cc/runtime/fiber_sched.c`):
- Replaced blunt startup spill trigger (`pending > workers`) with startup-only tokenized spill mode:
  - enter spill mode at `active==0 && pending > high_water` (`high_water ~= 2*workers`)
  - exit spill mode at `pending <= low_water` (`low_water = workers`)
  - when in spill mode, consume one token to force global fallback; no token means keep inbox route
  - token refill only on backlog relief (`pending` drop) or wake generation change (`wake_prim.value`), capped to worker-wave size
- Added spill-shaping counters:
  - `mode_enter`, `mode_exit`, `token_refill`, `suppressed_no_tokens`
- Kept all prior route/phase and global phase-split diagnostics unchanged.

Validation:
- `work_stealing_race` @10s: `12/15` (baseline in this session: `14/15`; recent bad prior config: `12/15`)
- `join_init_race` @3s: `13/15`
- `join_init_race` @10s stats run: PASS

Counter/latency snapshots:
- Baseline-before (same session, old blunt trigger):
  - `forced_spill=31209`
  - `global[a0] split: forced=30911@261174.7 natural=70920@4996.5`
  - `global[a0] phase split: forced[publish->pop=10838@193425.8 pop->run=10838@106.0]`
- After token/hysteresis shaping:
  - `forced_spill=1535(mode_enter=946 mode_exit=946 refill=234 no_tokens=3204)`
  - `global[a0] split: forced=1379@51291.1 natural=98385@3592.2`
  - `global[a0] phase split: forced[publish->pop=315@32456.1 pop->run=315@117.9]`

Interpretation:
- Token/hysteresis shaping dramatically reduced forced spill volume (about 20x) and improved forced-lane `publish->pop` service latency (about 6x improvement vs session baseline).
- The residual forced lane remains costlier than natural global, but it is now much less dominant.
- `work_stealing_race` pass-rate is not yet at a stable `15/15`; next iteration should tune watermarks/refill aggressiveness while watching `mode_enter/exit/refill/no_tokens` and forced `publish->pop`.

### 16.17 Spill-shaper aggressiveness tuning attempt (reverted)

Change attempt (`cc/runtime/fiber_sched.c`):
- Made spill mode more aggressive:
  - lower exit watermark (`low_water = workers/2`)
  - larger initial token grant (`workers`)
  - larger token cap (`2*workers`)
  - refill also when `pending > high_water`

Validation:
- `work_stealing_race` @10s: `11/15`
- `join_init_race` @3s: `12/15`

Outcome:
- Regressed both target stress tests versus the prior token/hysteresis settings.
- Reverted these aggressiveness changes.

Post-revert quick verify:
- `work_stealing_race` @10s: `10/10`
- `join_init_race` @3s: `10/10`

Post-revert counter snapshot (`work_stealing_race`):
- `forced_spill=2709(mode_enter=1744 mode_exit=1744 refill=208 no_tokens=5147)`
- `global[a0] split: forced=2465@78081.8 natural=98886@3607.0`
- `global[a0] phase split: forced[publish->pop=976@89994.2 pop->run=976@74.5] natural[publish->pop=81124@2915.0 pop->run=81124@74.5]`

Interpretation:
- Increasing spill aggressiveness directly worsened stability despite aiming to reduce inbox startup lag.
- The safer direction remains conservative spill shaping with lower forced volume, then tightening mode churn/entry behavior rather than broad token-rate increases.

### 16.18 Spill-controller race/churn follow-ups (mixed, not yet stable)

Attempts (`cc/runtime/fiber_sched.c`):
- Tried a wake-generation single-controller update for spill mode/tokens with atomic token consumption.
  - Result: regressed badly (`work_stealing_race 5/15`, `join_init_race 12/15`), with very low forced spill and high startup uncond wakes.
  - Reverted.
- Tried wake-generation-gated spill-mode entry.
  - Result: regressed (`work_stealing_race 10/15`, `join_init_race 13/15`).
  - Reverted.
- Tried anti-flap exit hysteresis (`low_streak`) so spill mode exits only after sustained low backlog.
  - Best observed sample in-session: `work_stealing_race 13/15`, `join_init_race 15/15`.
  - Subsequent samples were noisy (`work_stealing_race 7/10`, `join_init_race 8/10`), indicating instability remains.

Current state:
- Kept `low_streak` anti-flap variant as the active candidate.
- Scheduler is still not consistently stable across repeated matrices; issue not fully fixed.

### 16.19 Forced-spill shard pin correction re-check

Change:
- Restored forced startup spill fallback to shard `0` (`global_preferred=0`) for `startup_force_global`.
- Kept non-forced behavior unchanged (`nw_ready ? target : 0`).

Validation snapshot:
- `work_stealing_race` @10s: `11/15`
- `join_init_race` @3s: `12/15`

Interpretation:
- This correction is directionally safer (matches prior diagnosis), but by itself does not eliminate flakiness in the current matrix.
- Remaining instability is still driven by startup-phase publish/service dynamics, not a single shard-selection knob.

### 16.20 CAS-owned single-writer spill controller

Change (`cc/runtime/fiber_sched.c`):
- Added wake-generation controller ownership (`g_spawn_nw_startup_last_wakev_control`).
- In startup (`active==0`), spill mode/token state updates are now performed by a single controller winner per wake generation using CAS.
- Other non-worker spawns no longer rewrite spill control state; they only atomically consume available spill tokens.
- On `active>0`, spill controller state now fully resets (`mode/tokens/low_streak/last_pending/wakev markers`).
- Kept forced-spill shard pinning to `0` and existing diagnostics/counters.

Validation:
- `work_stealing_race` @10s: `13/15`
- `join_init_race` @3s: `13/15`
- Extended:
  - `work_stealing_race` @10s: `16/20`
  - `join_init_race` @3s: `19/20`

Counter snapshot (`work_stealing_race` PASS run):
- `forced_spill=6307(mode_enter=5262 mode_exit=5262 refill=5286 no_tokens=437)`
- `global[a0] split: forced=5579@167519.4 natural=66401@3868.7`
- `global[a0] phase split: forced[publish->pop=2187@153209.5 pop->run=2187@48.0] natural[publish->pop=53647@2686.0 pop->run=53647@73.1]`

Interpretation:
- Single-writer control reduced controller-state races and improved stability versus recent regressions, but does not yet deliver fully stable `15/15` behavior.
- Forced-lane publish->pop remains the dominant expensive leg during bad startup pressure windows.

### 16.21 Spill-mode re-entry cooldown (controller-side)

Change (`cc/runtime/fiber_sched.c`):
- Added startup spill-mode re-entry cooldown marker:
  - `g_spawn_nw_startup_last_wakev_enter`
- In controller update path, spill mode entry on `pending > high_water` now requires at least 2 wake generations since last entry.
- Added new counter:
  - `spawn_nw_forced_spill_suppressed_cooldown`
- Extended dump line to print `cooldown=...` in forced spill diagnostics.

Validation:
- `work_stealing_race` @10s: `14/15`
- `join_init_race` @3s: `14/15`
- Extended:
  - `work_stealing_race` @10s: `19/20`
  - `join_init_race` @3s: `19/20`

Counter snapshot (`work_stealing_race` PASS run):
- `forced_spill=11414(mode_enter=10124 mode_exit=10124 refill=10199 no_tokens=6615 cooldown=0)`
- `global[a0] split: forced=11095@422505.4 natural=21426@61719.3`
- `global[a0] phase split: forced[publish->pop=4727@590949.1 pop->run=4727@46.3] natural[publish->pop=18829@47565.6 pop->run=18829@70.2]`

Interpretation:
- Stability improved materially in this session and reached near-steady pass rates.
- In the sampled PASS dump, cooldown did not need to trigger (`cooldown=0`), suggesting the gain likely comes from the combined CAS-owned controller + re-entry guard behavior under flakier iterations.

### 16.22 Further tightening attempts (follow-up)

Tried variants:
- Deterministic re-entry cooldown counter (`cooldown_left`) replacing wake-age gate:
  - Regressed (e.g. `work_stealing_race 14/20`, `join_init_race 18/20`) and cooldown remained effectively inactive in sampled runs.
  - Reverted.
- Reduced forced-spill strength:
  - `init=workers/8`, `cap=workers/4`:
    - Best sample hit `work_stealing_race 20/20`, `join_init_race 18/20`, but follow-up samples were noisier.
  - `init=workers/8`, `cap=workers/2`:
    - `work_stealing_race 15/20`, `join_init_race 18/20`.
- Startup unconditional wake window `3*workers`:
  - Hurt `join_init_race` in this session; reverted to `2*workers`.
- Dynamic wake budget (`3x` only while spill mode active):
  - Regressed overall (`work_stealing_race 16/20`, `join_init_race 17/20`); reverted.

Current retained profile:
- CAS-owned controller update per wake generation.
- Wake-age re-entry gate retained.
- Startup unconditional wake window at `2*workers`.
- Higher spill strength (`init=workers/2`, `cap=workers`) retained for stability.

Latest sanity after re-locking profile:
- `work_stealing_race` @10s: `5/5`
- `join_init_race` @3s: `5/5`

### 16.23 Deterministic startup protocol pass (structural)

Intent:
- Replace heuristic startup tuning with explicit startup invariants.

Changes (`cc/runtime/fiber_sched.c`):
- **Admission invariant (startup, non-worker spawns):**
  - Added deterministic wake-wave inbox quota:
    - `g_spawn_nw_startup_admit_wakev`
    - `g_spawn_nw_startup_admit_remaining`
  - On wake-generation change, reset remaining admits to `num_workers`.
  - Non-worker startup spawns consume one admit token for inbox path; when exhausted, deterministically spill to global (forced).
  - Added diagnostics:
    - `spawn_nw_startup_admit_wave_reset`
    - `spawn_nw_startup_admit_granted`
    - `spawn_nw_startup_admit_denied`
- **Service invariant (startup worker loop):**
  - Added deterministic startup pre-pass in worker loop:
    - probe own inbox once, then probe global once, before bulk local-drain path.
  - This bounds startup lag for both inbox and global lanes.
- **Wake invariant (startup global publish):**
  - Removed startup unconditional wake pocket branch from global publish path.
  - Startup now relies on edge/strict-idle conditional wake path only.

Validation:
- Quick matrix:
  - `work_stealing_race` @10s: `18/20`
  - `join_init_race` @3s: `16/20`
- Definitive soak:
  - `work_stealing_race` @10s: `32/50`
  - `join_init_race` @3s: `42/50`

Counter snapshot characteristics:
- `startup_admit(reset=..., grant=..., deny=...)` active and coherent.
- Forced spills become explicit startup quota denials (not spill-mode oscillation).
- Startup global `publish->pop` forced lane remains the expensive leg when degraded.

Interpretation:
- Structural protocol improved over prior failed-soak baseline (`21/50`, `38/50`) but still fails acceptance.
- Deterministic admission/service reduced policy jitter, yet startup service capacity remains insufficient in worst windows.

### 16.24 Explicit STARTUP->RUN phase protocol (acceptance pass)

Change (`cc/runtime/fiber_sched.c`):
- Added explicit scheduler phase state:
  - `startup_phase` (`STARTUP` / `RUN`)
  - `startup_run_count`
  - `startup_target_runs` (initialized to `num_workers * 8`)
- Promotion rule:
  - workers increment `startup_run_count` on each completed fiber run;
  - scheduler transitions once to `RUN` when `startup_run_count >= startup_target_runs`.
- Startup policy is now keyed off explicit phase (not `active==0`):
  - `pool_ready_for_nonworker_inbox_publish()` returns `1` in `RUN`;
  - startup-only deterministic admission quota remains in `STARTUP`.
- Startup service remains deterministic:
  - worker loop probes inbox once + global once before bulk local drain while in `STARTUP`.
- Startup global unconditional wake pocket remains removed.

Validation:
- Quick matrix:
  - `work_stealing_race` @10s: `20/20`
  - `join_init_race` @3s: `20/20`
- Acceptance soak:
  - `work_stealing_race` @10s: `50/50`
  - `join_init_race` @3s: `50/50`

Counter characteristics:
- `startup_admit(reset/grant/deny)` active only in early startup waves, then effectively quiescent after phase promotion.
- `spawn_non_global` dominates in steady state; forced global startup spill volume collapses.

Interpretation:
- This resolves the intermittent startup instability by replacing repeated pseudo-startup re-entry with a single deterministic startup phase and a one-way promotion to run phase.

### 16.25 Post-startup fix follow-through: peer-inbox rescue generalization

Change (`cc/runtime/fiber_sched.c`):
- Generalized peer-inbox rescue checks from startup-only to all phases.
- Applied both in the pre-sleep "no local/inbox/global" path and in the
  immediate post-sleep recheck path.

Why:
- `stress/fiber_spawn_join_tight.ccs` exposed stranded inbox work in `RUN`
  phase: a worker could wake, miss its own inbox, and return to sleep while
  work sat in a peer inbox.
- The same stranded-inbox pattern affected several channel smoke tests.

Validation:
- `stress/fiber_spawn_join_tight.ccs`: passes after change.
- `make smoke`: all pass.
- `make perf-check`: all pass.

Interpretation:
- The rescue path is not only a startup concern; it is a general liveness guard
  for inbox-routed work under wake timing races.

### 16.26 Block combinator stress fix + current baseline snapshot

Change (`cc/runtime/task.c`):
- Corrected `cc_block_any()` semantics to wait for first successful completion
  (current convention: non-negative result), instead of treating first completion
  as success.
- If all tasks fail, function now returns `ECANCELED` with `winner=-1` and
  `result=-1`.

Repro + fix confirmation:
- Before fix (`stress/block_combinators_stress.ccs` loop): `5/20` pass.
- After fix (`stress/block_combinators_stress.ccs` loop): `20/20` pass.

Current suite status (latest run):
- `make smoke` -> pass
- `make stress-check` -> pass
- `make perf-check` -> pass

Latest pigz sanity pair (200MB, same counter flags):
- Fiber vs pthread:
  - `sleep enters`: `20` vs `9`
  - `sleep waits`: `27` vs `15`
  - `wake_with_work`: `12` vs `1`
  - `wake_no_work`: `21` vs `15`
  - `global pops attempts/hits`: `25/25` vs `1/1`
  - `spawn nonworker publish attempts`: `1196` vs `2`

Interpretation:
- Runtime remains more scheduler-active than pthread in this snapshot, but
  correctness/liveness baseline is currently green across smoke+stress+perf.

### 16.27 Scheduler knob classification (cleanup guide)

Intent:
- Separate structural invariants ("fixes") from calibration knobs ("tuning") and
  identify legacy residue for removal.

Class A - contract-defining (keep):
- Explicit startup protocol (`startup_phase`, `startup_run_count`,
  `startup_target_runs`).
- Startup wake-wave admission (`g_spawn_nw_startup_admit_wakev`,
  `g_spawn_nw_startup_admit_remaining`).
- Scheduler-thread context split (`tls_sched_worker_ctx`) and replacement/non-worker
  routing separation.
- Stale-affinity unpark reroute (peer inbox first, global fallback only).

Class B - calibration knobs (keep but document rationale/range):
- `SYSMON_CHECK_US`
- `ORPHAN_THRESHOLD_CYCLES`
- `ORPHAN_COOLDOWN_CYCLES`
- `TEMP_IDLE_TIMEOUT_ITERS`
- `FAIRNESS_CHECK_INTERVAL`
- Replacement probe cadence constants (`&3`, `spin<64`, `(spin&7)`).

Class C - legacy/residue (candidate removal):
- Startup spill-mode state/counters still threaded through spawn path but no
  longer policy-defining under deterministic startup admission.
- Dead helper utilities in sysmon path that are not referenced by runtime logic.

Immediate cleanup applied:
- Removed dead helper functions from `cc/runtime/fiber_sched.c`:
  - `sysmon_has_global_pending()`
  - `sysmon_count_pending()`
