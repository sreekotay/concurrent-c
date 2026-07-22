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

# 3) The strict-deadlock knob was deleted (it was write-only: the driver
#    setenv'd CC_STRICT_DEADLOCK but nothing ever read it, and the compile-time
#    deadlock heuristics it claimed to gate do not exist).  Pin that the driver
#    no longer advertises it and no longer silently accepts the flag.
if "$CCC" --help 2>&1 | grep -qi -e "strict-deadlock" -e "CC_STRICT_DEADLOCK"; then
  fail "--help still advertises the deleted strict-deadlock knob"
fi
if "$CCC" build --strict-deadlock "$tmp" >/dev/null 2>&1; then
  fail "deleted flag --strict-deadlock was silently accepted"
fi

echo "[test_cli] OK"
