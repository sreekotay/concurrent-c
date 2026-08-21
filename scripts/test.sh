#!/usr/bin/env bash
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
#   --O0 / -O0         host-compile harness bins with -O0 (faster cold builds)
#   CC_TEST_O0=1       same as --O0
#   --native           no-op (ccc is native-only)
#   --legacy / --compare-front  removed (exit 2)

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT_DIR"

# Harness binary lives under out/ with other arch-scoped products (ILP32 Docker
# rsync excludes /out/; a Mach-O at tools/cc_test used to clobber ARM volumes).
CC_TEST_BIN="${CC_TEST_BIN:-$ROOT_DIR/out/tools/cc_test}"
mkdir -p "$(dirname "$CC_TEST_BIN")"
if [ ! -x "$CC_TEST_BIN" ] || { [ -f "./tools/cc_test.c" ] && [ "./tools/cc_test.c" -nt "$CC_TEST_BIN" ]; }; then
  echo "[test] building $CC_TEST_BIN"
  cc -O2 -Wall -Wextra -D_FILE_OFFSET_BITS=64 tools/cc_test.c -o "$CC_TEST_BIN"
fi

# Default: quick. Full is opt-in. Front default: native (ccc default).
quick=1
full=0
opt_o0=0
case "${CC_TEST_FULL:-0}" in
  1|yes|true|TRUE|Yes) full=1 ;;
esac
case "${CC_TEST_QUICK:-}" in
  0|no|false|FALSE|No) full=1 ;;
esac
case "${CC_TEST_O0:-0}" in
  1|yes|true|TRUE|Yes) opt_o0=1 ;;
esac
case "${CC_TEST_FRONTEND:-}" in
  ''|native) ;;
  *)
    echo "[test] CC_TEST_FRONTEND=${CC_TEST_FRONTEND}: ccc is native-only" >&2
    exit 2
    ;;
esac
# Strip front flags before handing argv to cc_test; keep --quick/--full/--O0.
args=()
for a in "$@"; do
  case "$a" in
    --quick) quick=1; full=0; args+=("$a") ;;
    --full)  full=1; args+=("$a") ;;
    --O0|-O0) opt_o0=1; args+=("--O0") ;;
    --native) ;; # no-op
    --legacy|--compare-front)
      echo "[test] $a removed: ccc is native-only (shadow_lower)" >&2
      exit 2
      ;;
    *) args+=("$a") ;;
  esac
done
# Empty "${args[@]}" trips `set -u` on some bash builds — expand safely.
if [ "${#args[@]}" -gt 0 ]; then
  set -- "${args[@]}"
else
  set --
fi
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
if [ "$opt_o0" = 1 ]; then
  has_o0=0
  for a in "$@"; do
    if [ "$a" = "--O0" ] || [ "$a" = "-O0" ]; then has_o0=1; break; fi
  done
  if [ "$has_o0" = 0 ]; then
    extra="$extra --O0"
  fi
fi
if [ "$quick" = 1 ]; then
  has_quick=0
  for a in "$@"; do
    if [ "$a" = "--quick" ]; then has_quick=1; break; fi
  done
  if [ "$has_quick" = 0 ]; then
    extra="$extra --quick"
  fi
  echo "[test] default (quick): skipping heavy preambles + stress/race tests (jobs=$jobs${opt_o0:+, -O0})"
  echo "[test] tip: CC_TEST_FULL=1 or --full for redis line-map / functional / tcc-patch / stress"
else
  has_full=0
  for a in "$@"; do
    if [ "$a" = "--full" ]; then has_full=1; break; fi
  done
  if [ "$has_full" = 0 ]; then
    extra="$extra --full"
  fi
  echo "[test] full mode (jobs=$jobs${opt_o0:+, -O0})"
fi

# Rebuild the compiler when missing, broken, or stale vs this checkout
# (git pull of cc/src or tcc-patches while yesterday's ccc still runs).
need_cc=0
if [ ! -x "./cc/bin/.ccc-bin" ]; then need_cc=1; fi
if [ ! -x "./out/cc/bin/shadow_lower" ] && [ ! -x "./cc/bin/shadow_lower" ]; then
  need_cc=1
fi
if [ "$need_cc" = 0 ] && [ -x "./cc/bin/ccc" ]; then
  if ! ./cc/bin/ccc __eval-const --selftest >/dev/null 2>&1; then
    need_cc=1
  fi
fi
if [ "$need_cc" = 0 ] && [ -e "./cc/bin/.ccc-bin" ]; then
  if find cc/src cc/Makefile third_party/tcc-patches -type f \
       \( -name '*.c' -o -name '*.h' -o -name 'Makefile' -o -name '*.patch' \) \
       -newer ./cc/bin/.ccc-bin 2>/dev/null | head -1 | grep -q .; then
    echo "[test] compiler sources newer than cc/bin/.ccc-bin; rebuilding"
    need_cc=1
  fi
fi
if [ "$need_cc" = 0 ] && [ -f "./third_party/tcc/libtcc.a" ]; then
  if find third_party/tcc-patches -type f -name '*.patch' \
       -newer ./third_party/tcc/libtcc.a 2>/dev/null | head -1 | grep -q .; then
    echo "[test] tcc patches newer than libtcc.a; rebuilding"
    need_cc=1
  fi
fi
if [ "$need_cc" = 1 ]; then
  echo "[test] building compiler (make -C cc)"
  make -C cc all
fi

if [ ! -x "./out/cc/bin/shadow_lower" ] && [ ! -x "./cc/bin/shadow_lower" ]; then
  echo "[test] FAIL: needs shadow_lower (make -C cc failed?)"
  exit 1
fi

# Refuse an incomplete checkout before ~1000 tests fail as compile_err noise.
bash "$ROOT_DIR/scripts/check_patched_tcc.sh"

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
  # Quote #include "….h" passthrough (ordinary C headers are host-cc's).
  if ! sh scripts/test_quote_h_passthrough.sh; then
    echo "[test] quote-.h passthrough selftest FAILED"
    exit 1
  fi
  # Checkout ccc: relative --out-dir / default out is cwd, not the compiler repo.
  if ! sh scripts/test_out_dir_cwd.sh; then
    echo "[test] out-dir cwd selftest FAILED"
    exit 1
  fi
  # #!ccc units: quoted #include of project .cch resolves from the source dir.
  if ! sh scripts/test_unit_header_quote_include.sh; then
    echo "[test] unit-header quote-include selftest FAILED"
    exit 1
  fi
  # Root-tape `#ifdef` / `#ifndef` emit is a clean copy (host cpp selects).
  if ! sh scripts/test_ifdef_passthrough.sh; then
    echo "[test] ifdef passthrough selftest FAILED"
    exit 1
  fi
  # Step-1 owned C parser: preserve + evaluate on fixtures (not shadow).
  if ! sh scripts/test_cparse.sh; then
    echo "[test] cparse step-1 selftest FAILED"
    exit 1
  fi
  # Tutorial fences: docs/typehooks-typeviews.md is the source of truth.
  if ! bash scripts/test_doc_fences.sh; then
    echo "[test] doc fence smoke FAILED"
    exit 1
  fi
  # FileTape `#line` / CC_LN index + emit-cache replay of remapped loci.
  if ! sh scripts/test_tape_line_index.sh; then
    echo "[test] tape line index cache selftest FAILED"
    exit 1
  fi
  # Full SERDES goldens/recipes: scripts/test_shadow.sh (not this gate).

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
    if ! sh scripts/test_shadow_real_projects.sh; then
      echo "[test] native real-projects smoke FAILED"
      exit 1
    fi
  else
    echo "[test] quick: skipped async_line_map / diag_cache / variant_shape / tcc_patch / redis_functional / native_real"
  fi
fi

unset CC_TEST_FRONTEND
# Reject stale env that would make ccc exit 2 mid-harness.
case "${CC_FRONTEND:-}" in
  ''|native) ;;
  *)
    echo "[test] CC_FRONTEND=${CC_FRONTEND}: ccc is native-only" >&2
    exit 2
    ;;
esac

set +e
# shellcheck disable=SC2086
"$CC_TEST_BIN" $extra "$@"
rc=$?
set -e
ver="$(./cc/bin/ccc --v 2>/dev/null | head -1)"
echo "[test] ccc: ${ver:-unknown} (native)"
exit "$rc"
