#!/bin/sh
set -euo pipefail

# Local test entrypoint without requiring "make test" (though building cc still uses make today).

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT_DIR"

if [ ! -x "./tools/cc_test" ]; then
  echo "[test] building tools/cc_test"
  cc -O2 -Wall -Wextra tools/cc_test.c -o tools/cc_test
fi

# D3.0: exercise the in-process constexpr seam (cc_tcc_eval_const_expr) — a
# compiler-internal that the .ccs/.c harness can't reach directly.
if [ -x "./cc/bin/ccc" ]; then
  if ! ./cc/bin/ccc __eval-const --selftest >/dev/null; then
    echo "[test] const-eval selftest FAILED"
    exit 1
  fi
  # Driver CLI arg-parsing regressions (flag-before-subcommand, build.cc
  # same-file dedup) — the .ccs harness can't reach argv parsing.
  if ! sh scripts/test_cli.sh; then
    echo "[test] CLI selftest FAILED"
    exit 1
  fi
  # Reparse-input sanitizer regressions (ctor-priority blanking): asserts
  # on the reparse dump, catching platform-dependent breakage (macOS SDK
  # vs glibc __attribute__ handling) that pass/fail alone can't see here.
  if ! sh scripts/test_reparse_sanitize.sh; then
    echo "[test] reparse sanitize selftest FAILED"
    exit 1
  fi
fi

exec ./tools/cc_test --jobs 4 "$@"


