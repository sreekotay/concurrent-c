#!/bin/sh
set -euo pipefail

# Local test entrypoint without requiring "make test" (though building cc still uses make today).
#
# Default is the fast local loop (cheap preambles + no stress/race). Opt into
# the complete gate with --full / CC_TEST_FULL=1.
#
# Env / flags:
#   CC_TEST_JOBS=N     parallel harness jobs (default: ncpu, capped at 16)
#   CC_TEST_FULL=1     full preambles + stress/lostwake/race tests
#   --full             same as CC_TEST_FULL=1
#   CC_TEST_QUICK=0    same as --full (escape hatch if something sets QUICK=1)
#   --quick            explicit default (no-op unless paired with conflicting FULL)

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT_DIR"

if [ ! -x "./tools/cc_test" ]; then
  echo "[test] building tools/cc_test"
  cc -O2 -Wall -Wextra tools/cc_test.c -o tools/cc_test
fi

# Rebuild cc_test when the source is newer than the binary.
if [ -f "./tools/cc_test.c" ] && [ "./tools/cc_test.c" -nt "./tools/cc_test" ]; then
  echo "[test] rebuilding tools/cc_test"
  cc -O2 -Wall -Wextra tools/cc_test.c -o tools/cc_test
fi

# Default: quick. Full is opt-in.
quick=1
full=0
case "${CC_TEST_FULL:-0}" in
  1|yes|true|TRUE|Yes) full=1 ;;
esac
case "${CC_TEST_QUICK:-}" in
  0|no|false|FALSE|No) full=1 ;;
esac
for a in "$@"; do
  case "$a" in
    --quick) quick=1; full=0 ;;
    --full)  full=1 ;;
  esac
done
if [ "$full" = 1 ]; then
  quick=0
fi

has_jobs=0
for a in "$@"; do
  if [ "$a" = "--jobs" ]; then has_jobs=1; break; fi
done

jobs="${CC_TEST_JOBS:-}"
if [ -z "$jobs" ]; then
  jobs="$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || getconf _NPROCESSORS_ONLN 2>/dev/null || echo 6)"
fi
case "$jobs" in
  ''|*[!0-9]*) jobs=6 ;;
esac
if [ "$jobs" -lt 1 ]; then jobs=1; fi
if [ "$jobs" -gt 16 ]; then jobs=16; fi

extra=""
if [ "$has_jobs" = 0 ]; then
  extra="$extra --jobs $jobs"
fi
if [ "$quick" = 1 ]; then
  has_quick=0
  for a in "$@"; do
    if [ "$a" = "--quick" ]; then has_quick=1; break; fi
  done
  if [ "$has_quick" = 0 ]; then
    extra="$extra --quick"
  fi
  echo "[test] default (quick): skipping heavy preambles + stress/race tests (jobs=$jobs)"
  echo "[test] tip: CC_TEST_FULL=1 or --full for redis line-map / functional / tcc-patch / stress"
else
  has_full=0
  for a in "$@"; do
    if [ "$a" = "--full" ]; then has_full=1; break; fi
  done
  if [ "$has_full" = 0 ]; then
    extra="$extra --full"
  fi
  echo "[test] full mode (jobs=$jobs)"
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
  # SERDES parallel lowerer is NOT part of this gate — use scripts/test_serdes.sh.

  if [ "$quick" = 0 ]; then
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

    # @variant lowering shape (spec/draft_variants.md §3): the lowered C is a
    # normative ABI surface; assert the emitted enum/struct/tag/projection
    # shapes on the smoke sources (the .ccs harness pins only semantics).
    if ! sh scripts/test_variant_lowering.sh; then
      echo "[test] variant lowering shape selftest FAILED"
      exit 1
    fi

    # tcc patch-apply stale-state auto-reset (#116): current tree is a no-op,
    # a stale (old-patch) tree auto-resets and applies, a pristine tree
    # applies.  Needs the tcc submodule; restores the tree on exit.
    if [ -e third_party/tcc/.git ]; then
      if ! bash scripts/test_tcc_patch_apply.sh; then
        echo "[test] tcc patch apply selftest FAILED"
        exit 1
      fi
    else
      echo "[test] SKIP tcc patch apply selftest (tcc submodule not initialized)"
    fi

    # Redis functional smoke (#111 command semantics, #117 wire parity,
    # #130 pipeline reply ordering): builds the real redis port (cached) and
    # runs redis_smoke.py once on a collision-safe ephemeral port.
    if ! sh scripts/test_redis_functional.sh; then
      echo "[test] redis functional smoke FAILED"
      exit 1
    fi
  else
    echo "[test] quick: skipped async_line_map / diag_cache / variant_shape / tcc_patch / redis_functional"
  fi
fi

# shellcheck disable=SC2086
exec ./tools/cc_test $extra "$@"
