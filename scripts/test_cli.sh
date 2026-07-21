#!/bin/sh
# CLI arg-parsing regressions for the ccc driver:
#
# 1) Flag-before-subcommand: `ccc --keep-c run f.ccs` must parse as run
#    mode.  It used to fall into default mode's legacy `cc <input> <output>`
#    form with an input literally named "run", which the debug-info
#    absolutization turned into <repo_root>/run — making build.cc discovery
#    find the root build.cc "twice" and die with a nonsense
#    "multiple build.cc files found" error.
#
# 2) choose_build_path must not report "multiple build.cc" when the cwd
#    candidate and the alongside-input candidate resolve to the SAME file
#    (an absolute input path in the repo root).
set -eu
ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT_DIR"
CCC=./cc/bin/ccc

fail() { echo "[test_cli] FAIL: $1" >&2; exit 1; }

out=$("$CCC" --keep-c run tests/string_template_verbatim_smoke.ccs 2>&1) \
  || fail "--keep-c run <file> exited nonzero: $out"
echo "$out" | grep -q "PASS" || fail "--keep-c run <file> did not run the program: $out"

tmp="$ROOT_DIR/.cli_selftest_tmp.ccs"
printf 'int main(void){ return 0; }\n' > "$tmp"
trap 'rm -f "$tmp"' EXIT
out=$("$CCC" --dry-run "$tmp" 2>&1) || fail "dry-run with repo-root absolute input failed: $out"
case "$out" in
  *"multiple build.cc"*) fail "same-file build.cc counted as multiple: $out" ;;
esac

echo "[test_cli] OK"
