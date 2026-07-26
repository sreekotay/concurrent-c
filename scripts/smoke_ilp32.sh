#!/usr/bin/env bash
# smoke_ilp32.sh — build Concurrent-C natively on Linux ILP32 and run a
# curated fiber/channel/atomic runtime suite.
#
# Intended to run on a writable Linux ILP32 tree (native host, or the /work
# sandbox created by scripts/docker/ilp32_entrypoint.sh). Prefer
# scripts/smoke_i386.sh from macOS — that mounts the repo read-only.
#
# Env:
#   CCC_ILP32_ARCH   i386 | arm
#   BUILD            debug|release (default: debug)
#   CCC_ILP32_JOBS   parallel make jobs (default: nproc or 4)
#
# Host wrappers: scripts/smoke_i386.sh, scripts/smoke_arm32.sh
#
# Exits nonzero on any unexpected failure.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT_DIR"

ARCH="${CCC_ILP32_ARCH:-}"

step()  { printf '\n== %s\n' "$*"; }
ok()    { printf '  OK   %s\n' "$*"; }
fail()  { printf '  FAIL %s\n' "$*"; }

die() {
  printf 'smoke_ilp32: %s\n' "$*" >&2
  exit 1
}

case "$ARCH" in
  i386|arm) ;;
  "") die "CCC_ILP32_ARCH unset (expected i386 or arm)" ;;
  *) die "unsupported CCC_ILP32_ARCH='$ARCH' (expected i386 or arm)" ;;
esac

uname_s="$(uname -s)"
[ "$uname_s" = "Linux" ] || die "must run on Linux (got $uname_s); use scripts/docker/"

machine="$(uname -m)"
case "$ARCH" in
  i386)
    case "$machine" in
      i386|i686|x86) ;;
      *) die "CCC_ILP32_ARCH=i386 but uname -m is '$machine'" ;;
    esac
    ;;
  arm)
    case "$machine" in
      arm|armv6l|armv7l|armhf) ;;
      *) die "CCC_ILP32_ARCH=arm but uname -m is '$machine'" ;;
    esac
    ;;
esac

step "ILP32 host ($ARCH, $machine)"
./scripts/build_ilp32_toolchain.sh

failures=0

run_one() {
  local f="$1"
  if ./cc/bin/ccc run --no-cache "$f"; then
    ok "run $f"
  else
    fail "run $f"
    failures=$((failures + 1))
  fi
}

step "assert linked program is ELF 32-bit"
rm -f out/hello bin/hello 2>/dev/null || true
./cc/bin/ccc build --no-cache examples/hello.ccs
hello_bin=""
for cand in out/hello bin/hello; do
  if [ -x "$cand" ]; then hello_bin="$cand"; break; fi
done
[ -n "$hello_bin" ] || die "hello binary not found after build"
hello_file="$(file -b "$hello_bin")"
printf '  %s: %s\n' "$hello_bin" "$hello_file"
echo "$hello_file" | grep -q 'ELF 32-bit' || die "hello is not ELF 32-bit: $hello_file"
ok "ELF 32-bit $hello_bin"

step "curated runtime suite"
SUITE=(
  examples/hello.ccs
  examples/recipe_channel_pipeline.ccs
  tests/fiber_spawn_task_smoke.ccs
  tests/chan_task_smoke.ccs
  tests/chan_pre_park_wake_smoke.ccs
  tests/nursery_create_destroy_proto_smoke.ccs
)
for f in "${SUITE[@]}"; do
  run_one "$f"
done

printf '\nsmoke_ilp32 (%s): %d unexpected failure(s)\n' "$ARCH" "$failures"
exit "$failures"
