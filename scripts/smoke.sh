#!/usr/bin/env bash
# smoke.sh — fresh-clone smoke test: build the toolchain, compile every
# example, run a few end-to-end. Catches example rot and build bitrot.
#
# Usage: ./scripts/smoke.sh
# Exits nonzero on any unexpected failure.
#
# Known limitations honored here:
#  - third_party/liblfds is OPTIONAL (channel.c falls back to the native
#    ring queue backend; see CC_HAVE_LIBLFDS in cc/runtime/channel.c).
#  - recipe_http_get.ccs is skipped when curl/curl.h is unavailable.
#  - Known-broken examples are listed in XFAIL below; they report but do
#    not fail the run. Fix the example (or compiler), then remove the entry.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT_DIR"

# Examples expected to fail compilation right now (tracked breakage).
XFAIL="recipe_long_lived_store.ccs"   # 'missing ok type before !>' regression

step()  { printf '\n== %s\n' "$*"; }
ok()    { printf '  OK   %s\n' "$*"; }
fail()  { printf '  FAIL %s\n' "$*"; }

step "submodules (tcc)"
git submodule update --init third_party/tcc
# Work around partial clones leaving an empty worktree behind a valid .git link.
if [ ! -f third_party/tcc/configure ]; then
  git submodule update --checkout --force third_party/tcc
fi

step "TinyCC (patched, --config-cc_ext)"
# Fresh submodule checkouts are pristine; hooks ride the patch files.
./scripts/apply_tcc_patches.sh
# NB: grep -q would exit early and SIGPIPE nm, which pipefail turns into
# a failure — use full-consuming grep >/dev/null instead.
if ! nm third_party/tcc/libtcc.a 2>/dev/null | grep cc_ast_record >/dev/null; then
  (cd third_party/tcc && ./configure --config-cc_ext && make libtcc.a tcc libtcc1.a -j"$(nproc 2>/dev/null || echo 4)")
else
  # libtcc.a has hooks; make sure the runtime-support lib exists too.
  (cd third_party/tcc && make tcc libtcc1.a -j"$(nproc 2>/dev/null || echo 4)")
fi
nm third_party/tcc/libtcc.a | grep cc_ast_record >/dev/null || { echo "libtcc.a missing CC hooks (cc_ast_record_*)"; exit 1; }

step "ccc compiler"
make -C cc BUILD="${BUILD:-debug}" TCC_EXT=1 TCC_INC=../third_party/tcc TCC_LIB=../third_party/tcc/libtcc.a -j"$(nproc 2>/dev/null || echo 4)"
test -x cc/bin/ccc || { echo "cc/bin/ccc not produced"; exit 1; }

have_curl_h=0
echo '#include <curl/curl.h>' | ${CC:-cc} -E - >/dev/null 2>&1 && have_curl_h=1

step "compile all examples (--emit-c-only)"
failures=0
xfailed=0
for f in examples/*.ccs; do
  base="$(basename "$f")"
  if [ "$base" = "recipe_http_get.ccs" ] && [ "$have_curl_h" = "0" ]; then
    printf '  SKIP %s (no curl/curl.h)\n' "$f"
    continue
  fi
  if ./cc/bin/ccc --emit-c-only "$f" -o /dev/null >/dev/null 2>&1; then
    case " $XFAIL " in
      *" $base "*) printf '  XPASS %s (remove from XFAIL)\n' "$f" ;;
      *) ok "$f" ;;
    esac
  else
    case " $XFAIL " in
      *" $base "*) printf '  XFAIL %s (known breakage)\n' "$f"; xfailed=$((xfailed+1)) ;;
      *) fail "$f"; failures=$((failures+1)) ;;
    esac
  fi
done

step "build-system examples (--dry-run)"
for d in examples/*/; do
  if [ -f "$d/build.cc" ]; then
    if ./cc/bin/ccc build --build-file "$d/build.cc" --dry-run >/dev/null 2>&1; then
      ok "$d"
    else
      fail "$d"; failures=$((failures+1))
    fi
  fi
done

step "run examples end-to-end"
for f in examples/hello.ccs examples/recipe_channel_pipeline.ccs examples/recipe_worker_pool.ccs examples/recipe_result_error_handling.ccs; do
  if ./cc/bin/ccc run "$f" >/dev/null 2>&1; then
    ok "run $f"
  else
    fail "run $f"; failures=$((failures+1))
  fi
done

step "warm compile timing"
./cc/bin/ccc --emit-c-only examples/hello.ccs >/dev/null 2>&1 || true
t0=$(date +%s%N 2>/dev/null || echo 0)
./cc/bin/ccc --emit-c-only examples/hello.ccs >/dev/null 2>&1
t1=$(date +%s%N 2>/dev/null || echo 0)
if [ "$t0" != "0" ]; then
  printf '  hello.ccs -> C in %d ms (warm)\n' $(( (t1 - t0) / 1000000 ))
fi

printf '\nsmoke: %d unexpected failure(s), %d known-broken (xfail)\n' "$failures" "$xfailed"
exit "$failures"
