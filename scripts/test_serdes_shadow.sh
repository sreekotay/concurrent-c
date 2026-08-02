#!/usr/bin/env bash
# SERDES hard-go + recipe behavioral gate (subset of scripts/test_serdes.sh).
# Not invoked from scripts/test.sh — production ccc stays separate.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
SH="$ROOT/examples/serdes/c/shadow"

bash "$SH/diff_stdlib_exec.sh"
bash "$SH/diff_stdlib_async_runtime.sh"
bash "$SH/diff_stdlib_nursery.sh"
bash "$SH/diff_stdlib_chan_handle.sh"
bash "$SH/diff_stdlib_io_error.sh"
bash "$SH/diff_stdlib_result_face.sh"
bash "$SH/run_recipes_shadow.sh"
bash "$SH/run_via_seam.sh" examples/hello.ccs | grep -Fq 'All tasks completed.'

echo "test_serdes_shadow: OK"
