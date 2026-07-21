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
  # "Definition wins TU-locally" (@noblock): the lying-decl WARNING lands
  # on build stderr, which the .ccs harness never asserts for passing
  # tests — pinned here instead (behavior side is pinned by the
  # autoblock_noblock_*_smoke tid oracles).
  if ! sh scripts/test_autoblock_noblock_warn.sh; then
    echo "[test] autoblock noblock warning selftest FAILED"
    exit 1
  fi

  # @async state-machine #line accuracy on the real redis port: async poll
  # fns must be #line-mapped and no mapped coordinate may pass the source's
  # EOF (asserts on the emitted C; the diag_oracle corpus pins exact lines).
  if ! sh scripts/test_async_line_map.sh; then
    echo "[test] async line map selftest FAILED"
    exit 1
  fi

  # Warm-cache diagnostic replay: an emit that printed error diagnostics
  # must fail (and not cache), so reruns reprint the diagnostic instead of
  # riding the cache past the erroring pass.
  if ! sh scripts/test_diag_cache_replay.sh; then
    echo "[test] diag cache replay selftest FAILED"
    exit 1
  fi
fi

exec ./tools/cc_test --jobs 4 "$@"


