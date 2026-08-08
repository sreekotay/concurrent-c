#!/usr/bin/env bash
# smoke_bootstrap_fresh.sh — wipe build products and rebuild from the committed
# shadow_lower bootstrap (last-good). Canonical cold-tree check after TCC exists:
#   * fresh-clone graph: no warm out/include or stage-1 binary
#   * angle-include paths that need a seeded out/include/cc/shadow
#   * ODR clashes linking concurrent_c.o + libshadow_comptime.a on GNU ld
#   * stage-1 seed stamp must not recurse under make -j
#
# When: after seed promote / build-graph changes — not every stdlib edit.
# See docs/build-when.md.
#
# Usage (from repo root):
#   ./scripts/smoke_bootstrap_fresh.sh
#   ./scripts/smoke_i386.sh   # Linux ILP32 via Docker (preferred second leg)
#
# Env:
#   CCC_HOST_CC   host C compiler (default: cc). `tcc` on Linux ILP32 only.
#   BUILD         debug|release (default: debug)
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT_DIR"

die() { printf 'smoke_bootstrap_fresh: %s\n' "$*" >&2; exit 1; }

JOBS="${CCC_ILP32_JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)}"
HOST_CC="${CCC_HOST_CC:-cc}"
BUILD="${BUILD:-debug}"

printf '== wipe out/ cc/bin/ bin/ (fresh-clone simulation)\n'
rm -rf out cc/bin bin
# Keep third_party/tcc build if present — submodule fetch is separate.
# A true fresh clone still needs: ./scripts/fetch_submodules.sh && apply patches.
test -f third_party/tcc/libtcc.a || die "missing third_party/tcc/libtcc.a (fetch + configure + make tcc first)"

printf '== make -C cc (bootstrap shadow_lower from last-good)\n'
make -C cc CC="$HOST_CC" BUILD="$BUILD" TCC_EXT=1 \
  TCC_INC=../third_party/tcc TCC_LIB=../third_party/tcc/libtcc.a -j"$JOBS"

test -x cc/bin/.ccc-bin || die "cc/bin/.ccc-bin missing"
test -x out/cc/bin/shadow_lower -o -x cc/bin/shadow_lower || die "shadow_lower missing"

printf '== serdes hello\n'
printf '%s\n' '#include <stdio.h>' 'int main(void){ printf("bootstrap-fresh ok\n"); return 0; }' \
  > /tmp/ccc_bootstrap_fresh_hello.ccs
./cc/bin/ccc run --no-cache /tmp/ccc_bootstrap_fresh_hello.ccs
./cc/bin/ccc --v

printf 'smoke_bootstrap_fresh: ok (last-good=%s)\n' \
  "$(cat cc/bootstrap/shadow_lower/last-good 2>/dev/null || echo '?')"
