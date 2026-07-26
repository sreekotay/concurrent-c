#!/usr/bin/env bash
# compare_ilp32.sh — build original pigz + pigz_cc on Linux ILP32 and compare.
#
# Run via host wrapper:
#   ./scripts/pigz_i386.sh
#   ./scripts/pigz_arm32.sh
# Or inside an ILP32 sandbox (after toolchain build):
#   ./real_projects/pigz/compare_ilp32.sh
#
# Env:
#   PIGZ_BENCH_MB      input size in MB (default: 20)
#   PIGZ_BENCH_WORKERS pigz -p workers (default: 4)
#   PIGZ_BENCH_RUNS    timed runs (default: 2)
#   SKIP_TOOLCHAIN=1   skip ccc rebuild if already present
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
cd "$ROOT_DIR"

SIZE_MB="${PIGZ_BENCH_MB:-20}"
WORKERS="${PIGZ_BENCH_WORKERS:-4}"
RUNS="${PIGZ_BENCH_RUNS:-2}"

die() { printf 'compare_ilp32: %s\n' "$*" >&2; exit 1; }

[ "$(uname -s)" = "Linux" ] || die "must run on Linux ILP32 (use ./scripts/pigz_i386.sh or ./scripts/pigz_arm32.sh)"

export CFLAGS="-D_FILE_OFFSET_BITS=64 ${CFLAGS:-}"
export CXXFLAGS="-D_FILE_OFFSET_BITS=64 ${CXXFLAGS:-}"
export CPPFLAGS="-D_FILE_OFFSET_BITS=64 ${CPPFLAGS:-}"

# Rebuild ccc when SKIP_TOOLCHAIN is unset/0, or when the binary is missing.
# Set FORCE_TOOLCHAIN=1 to rebuild even when SKIP_TOOLCHAIN=1 (e.g. driver fix).
if [ "${FORCE_TOOLCHAIN:-0}" = "1" ] || [ "${SKIP_TOOLCHAIN:-0}" != "1" ] || [ ! -x cc/bin/.ccc-bin ]; then
  ./scripts/build_ilp32_toolchain.sh
else
  echo "== reuse existing cc/bin/.ccc-bin"
  file cc/bin/.ccc-bin
fi

# Makefile defaults to ../../out/cc/bin/ccc; prefer the wrapper.
export CCC="${CCC:-$ROOT_DIR/cc/bin/ccc}"
test -x "$CCC" || die "ccc not executable: $CCC"

echo "== build original pigz + pigz_cc"
make -C "$SCRIPT_DIR" clean
make -C "$SCRIPT_DIR" pigz pigz_cc CCC="$CCC" CC="${CC:-cc}" \
  CFLAGS="-O3 -Wall -Wextra -pthread -D_FILE_OFFSET_BITS=64"

PIGZ="$SCRIPT_DIR/out/pigz"
PIGZ_CC="$SCRIPT_DIR/out/pigz_cc"
test -x "$PIGZ" || die "missing $PIGZ"
test -x "$PIGZ_CC" || die "missing $PIGZ_CC"

echo "  pigz:    $(file -b "$PIGZ")"
echo "  pigz_cc: $(file -b "$PIGZ_CC")"
file -b "$PIGZ" | grep -q 'ELF 32-bit' || die "pigz is not ELF 32-bit"
file -b "$PIGZ_CC" | grep -q 'ELF 32-bit' || die "pigz_cc is not ELF 32-bit"

echo "== correctness (compress + gunzip round-trip)"
TMP="$SCRIPT_DIR/.compare_ilp32_tmp"
rm -rf "$TMP"
mkdir -p "$TMP"
dd if=/dev/urandom of="$TMP/in.bin" bs=1M count=4 status=none

"$PIGZ" -k -c -p "$WORKERS" "$TMP/in.bin" > "$TMP/orig.gz"
"$PIGZ_CC" -k -c -p "$WORKERS" "$TMP/in.bin" > "$TMP/cc.gz"
gunzip -c "$TMP/orig.gz" > "$TMP/orig.out"
gunzip -c "$TMP/cc.gz" > "$TMP/cc.out"
cmp "$TMP/in.bin" "$TMP/orig.out" && echo "  OK   original pigz round-trip"
cmp "$TMP/in.bin" "$TMP/cc.out" && echo "  OK   pigz_cc round-trip"
# Compressed streams need not be byte-identical; sizes for visibility:
printf '  sizes: orig=%s cc=%s bytes\n' "$(wc -c < "$TMP/orig.gz")" "$(wc -c < "$TMP/cc.gz")"
rm -rf "$TMP"

echo "== benchmark (${SIZE_MB} MB, -p ${WORKERS}, ${RUNS} runs)"
# Use existing testdata when present; benchmark.sh will fetch Silesia if needed.
cd "$SCRIPT_DIR"
./benchmark.sh "$SIZE_MB" "$WORKERS" "$RUNS"
