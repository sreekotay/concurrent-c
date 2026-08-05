#!/usr/bin/env bash
# Behavioral gate: shadow-lower real examples → host cc → link runtime → run.
# Parallel path — TCC not required for this product consume step.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../../../.." && pwd)"
SHADOW_SH="$ROOT/cc/shadow/shadow_lower.sh"
RT_O="$ROOT/out/cc/obj/runtime/concurrent_c.o"
TMP="${TMPDIR:-/tmp}/cc_serdes_recipes_$$"
mkdir -p "$TMP"

if [[ ! -f "$RT_O" ]]; then
  echo "FAIL: missing $RT_O (build cc runtime first)"
  exit 1
fi

# run_one [--link LIB]... [--bin-arg ARG]... SRC EXPECTED...
run_one() {
  local link_extra=()
  local bin_args=()
  while [[ $# -gt 0 ]]; do
    case "$1" in
      --link) shift; link_extra+=("-l$1"); shift ;;
      --bin-arg) shift; bin_args+=("$1"); shift ;;
      *) break ;;
    esac
  done
  local src="$1"
  shift
  local name
  name="$(basename "$src" .ccs)"
  "$SHADOW_SH" "$ROOT/$src" -o "$TMP/$name.c"
  cc -std=c11 -I"$ROOT/out/include" -c "$TMP/$name.c" -o "$TMP/$name.o"
  cc "$TMP/$name.o" "$RT_O" -o "$TMP/$name.bin" -lpthread -lm \
    ${link_extra[@]+"${link_extra[@]}"}
  # Behavioral: expected stdout markers are the oracle. Non-zero exit is OK
  # for network soft-fail (e.g. httpbin 503); crashes (rc>=128) still fail.
  local out rc
  set +e
  out="$("$TMP/$name.bin" ${bin_args[@]+"${bin_args[@]}"})"
  rc=$?
  set -e
  printf '%s\n' "$out"
  if [[ "$rc" -ge 128 ]]; then
    echo "FAIL: $name crashed rc=$rc"
    exit 1
  fi
  local need
  for need in "$@"; do
    if ! grep -Fq "$need" <<<"$out"; then
      echo "FAIL: $name missing expected line: $need"
      exit 1
    fi
  done
  echo "run_recipes_shadow: $name OK"
}

run_one examples/hello.ccs \
  "Hello from task A! (last)" \
  "Hello from task B!" \
  "Hello from task C!" \
  "All tasks completed."

run_one examples/recipe_result_error_handling.ccs \
  "total wait time: 90 seconds" \
  "missing key fell back to: -1" \
  "invalid key mapped to: 0" \
  "compose ok: timeout=30"

run_one examples/recipe_arena_scope.ccs \
  "Processing request 1 bytes: 4096" \
  "Processing request 2 bytes: 4096" \
  "Processing request 3 bytes: 4096"

run_one examples/recipe_explicit_capture.ccs \
  "=== value capture ===" \
  "task A sees snapshot = 42" \
  "=== reference capture (read-only) ===" \
  "task C reads counter = 100"

# Timing-sensitive: exact iteration counts vary; recipe itself asserts 1..10.
run_one examples/recipe_timeout.ccs \
  "iterations before deadline" \
  "checkpoints against handle"

run_one examples/recipe_defer_cleanup.ccs \
  "write_report (ok):" \
  "committed /tmp/report_ok.txt" \
  "write_report (err):" \
  "rolled back /tmp/report_err.txt" \
  "caught: injected failure"

run_one examples/recipe_unwrap_destroy_forms.ccs \
  "ok_short=hello" \
  "with_default=fallback" \
  "done"

run_one examples/recipe_channel_pipeline.ccs \
  "sum = 6 (expected 6)"

run_one examples/recipe_worker_pool.ccs \
  "feeder: sent 9 jobs" \
  "done, jobs_done = 9"

run_one examples/recipe_fanout_capture.ccs \
  "sum = 30 (expected 30)"

run_one examples/recipe_exclusive_named.ccs \
  "shared  = 4000 (expect 4000)" \
  "OK"

run_one examples/recipe_async_await.ccs \
  "pipeline result: 42 (expected 42)"

run_one examples/recipe_ufcs_forms.ccs \
  "len=3" \
  "median=3.0" \
  "halve=5.0" \
  "slice=2 y=4.0" \
  "ok=42" \
  "die=-1"

# Network soft-fails to SKIP when offline; Summary always prints.
run_one --link curl examples/recipe_http_get.ccs \
  "Fetching 3 URLs in parallel..." \
  "=== Summary ==="
run_one --bin-arg --test examples/recipe_tcp_echo.ccs \
  "listening on 127.0.0.1:9000 (test mode)" \
  "test client got 16 bytes: hello from test" \
  "server done"

run_one examples/recipe_ordered_parallel.ccs \
  "=== Ordered Parallel Processing ===" \
  "[consumer] item 1: 1^2 = 1" \
  "[consumer] item 8: 8^2 = 64" \
  "Done! Results printed in order despite out-of-order completion."

run_one examples/recipe_long_lived_store.ccs \
  "entries=2 logical_key_bytes=12 inserted=18 deleted=6"

run_one examples/recipe_user_generics.ccs \
  "head=3 tail=4.5" \
  "span=1.5" \
  "q=10,20" \
  "at=20"

echo "run_recipes_shadow: all OK"
rm -rf "$TMP"
