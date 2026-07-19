#!/usr/bin/env bash
# Side-by-side: CC races; Rust refuses the unsafe shape (or proves the fix).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"

echo "========== Concurrent-C (compiles; races) =========="
set +e
./cc/bin/ccc build run examples/compare_rust_data_race/race_pointer_alias.ccs
cc_rc=$?
set -e
echo "(exit $cc_rc — non-zero means lost updates observed)"
echo

echo "========== Rust =========="
if ! command -v rustc >/dev/null 2>&1; then
  echo "rustc not installed — open race_pointer_alias.rs"
  echo "  • commented unsafe block: data race if enabled"
  echo "  • atomic_ok(): compiles and asserts exact count"
  exit 0
fi

rustc -O examples/compare_rust_data_race/race_pointer_alias.rs \
  -o /tmp/cc_rust_data_race_demo
/tmp/cc_rust_data_race_demo
