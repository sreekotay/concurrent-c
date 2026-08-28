#!/usr/bin/env bash
# compare_ilp32.sh — build original pigz, pigz_idiomatic, and pigz_cc on Linux ILP32.
#
# Headline compare is pigz.c vs pigz_idiomatic (PIGZ_DICT=1). pigz_cc is built
# when the backend can; a TinyCC backend may skip it.
#
# Run via host wrapper:
#   ./scripts/pigz_i386.sh
#   CCC_HOST_CC=tcc ./scripts/pigz_i386.sh
#   ./scripts/pigz_arm32.sh
# Or inside an ILP32 sandbox (after toolchain build):
#   ./real_projects/pigz/compare_ilp32.sh
#
# Env:
#   PIGZ_BENCH_MB      input size in MB (default: 20)
#   PIGZ_BENCH_WORKERS pigz -p workers (default: 4)
#   PIGZ_BENCH_RUNS    timed runs (default: 2)
#   PIGZ_ORIG_CC       compiler for Adler pigz.c (default: cc / gcc)
#   CCC_HOST_CC / CCC_BACKEND_CC  as in smoke_ilp32.sh
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

backend_want="${CCC_BACKEND_CC:-}"
if [ -z "$backend_want" ]; then
  case "$(basename "${CCC_HOST_CC:-cc}")" in
    tcc) backend_want=tcc ;;
  esac
fi
if [ -n "$backend_want" ]; then
  case "$(basename "$backend_want")" in
    tcc) export CC="$ROOT_DIR/third_party/tcc/tcc" ;;
    *) export CC="$backend_want" ;;
  esac
elif [ -z "${CC:-}" ]; then
  export CC=cc
fi
echo "== ccc backend CC=$CC"

echo "== build original pigz + pigz_idiomatic (+ pigz_cc)"
make -C "$SCRIPT_DIR" clean
# Original pigz stays on gcc unless PIGZ_ORIG_CC is set — TinyCC is the
# ccc backend for .ccs, not a second host compile of Adler's tree.
make -C "$SCRIPT_DIR" pigz CC="${PIGZ_ORIG_CC:-cc}" \
  CFLAGS="-O3 -Wall -Wextra -pthread -D_FILE_OFFSET_BITS=64"
make -C "$SCRIPT_DIR" pigz_idiomatic CCC="$CCC" \
  CFLAGS="-O3 -Wall -Wextra -pthread -D_FILE_OFFSET_BITS=64"
if make -C "$SCRIPT_DIR" pigz_cc CCC="$CCC" \
  CFLAGS="-O3 -Wall -Wextra -pthread -D_FILE_OFFSET_BITS=64"; then
  :
else
  echo "SKIP pigz_cc (backend build failed)"
fi

PIGZ="$SCRIPT_DIR/out/pigz"
PIGZ_CC="$SCRIPT_DIR/out/pigz_cc"
PIGZ_IDIOMATIC="$SCRIPT_DIR/out/pigz_idiomatic"
test -x "$PIGZ" || die "missing $PIGZ"
test -x "$PIGZ_IDIOMATIC" || die "missing $PIGZ_IDIOMATIC"

echo "  pigz:           $(file -b "$PIGZ")"
echo "  pigz_idiomatic: $(file -b "$PIGZ_IDIOMATIC")"
file -b "$PIGZ" | grep -q 'ELF 32-bit' || die "pigz is not ELF 32-bit"
file -b "$PIGZ_IDIOMATIC" | grep -q 'ELF 32-bit' || die "pigz_idiomatic is not ELF 32-bit"
if [ -x "$PIGZ_CC" ]; then
  echo "  pigz_cc:   $(file -b "$PIGZ_CC")"
  file -b "$PIGZ_CC" | grep -q 'ELF 32-bit' || die "pigz_cc is not ELF 32-bit"
fi

echo "== correctness (compress + gunzip round-trip)"
TMP="$SCRIPT_DIR/.compare_ilp32_tmp"
rm -rf "$TMP"
mkdir -p "$TMP"
dd if=/dev/urandom of="$TMP/in.bin" bs=1M count=4 status=none

"$PIGZ" -k -c -p "$WORKERS" "$TMP/in.bin" > "$TMP/orig.gz"
cp "$TMP/in.bin" "$TMP/idio.bin"
PIGZ_DICT=1 "$PIGZ_IDIOMATIC" "$TMP/idio.bin"
gunzip -c "$TMP/orig.gz" > "$TMP/orig.out"
gunzip -c "$TMP/idio.bin.gz" > "$TMP/idio.out"
cmp "$TMP/in.bin" "$TMP/orig.out" && echo "  OK   original pigz round-trip"
cmp "$TMP/in.bin" "$TMP/idio.out" && echo "  OK   pigz_idiomatic PIGZ_DICT=1 round-trip"
if [ -x "$PIGZ_CC" ]; then
  "$PIGZ_CC" -k -c -p "$WORKERS" "$TMP/in.bin" > "$TMP/cc.gz"
  gunzip -c "$TMP/cc.gz" > "$TMP/cc.out"
  cmp "$TMP/in.bin" "$TMP/cc.out" && echo "  OK   pigz_cc round-trip"
  printf '  sizes: orig=%s idiomatic=%s cc=%s bytes\n' \
    "$(wc -c < "$TMP/orig.gz")" "$(wc -c < "$TMP/idio.bin.gz")" "$(wc -c < "$TMP/cc.gz")"
else
  printf '  sizes: orig=%s idiomatic=%s bytes\n' \
    "$(wc -c < "$TMP/orig.gz")" "$(wc -c < "$TMP/idio.bin.gz")"
fi
rm -rf "$TMP"

echo "== benchmark (${SIZE_MB} MB, pigz -p ${WORKERS}, ${RUNS} runs; pigz_idiomatic uses runtime cores)"
# Do not export CC_WORKERS — oversubscribe vs pigz -p N (see wait_dict_parity receipt).
unset CC_WORKERS || true
cd "$SCRIPT_DIR"
./benchmark.sh "$SIZE_MB" "$WORKERS" "$RUNS"
