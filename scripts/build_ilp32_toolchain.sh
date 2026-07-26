#!/usr/bin/env bash
# build_ilp32_toolchain.sh — build patched TinyCC + ccc on native Linux ILP32.
# Used by smoke_ilp32.sh and pigz ILP32 compare. Expects cwd = repo root.
set -euo pipefail

JOBS="${CCC_ILP32_JOBS:-$(nproc 2>/dev/null || echo 4)}"

die() { printf 'build_ilp32_toolchain: %s\n' "$*" >&2; exit 1; }

[ "$(uname -s)" = "Linux" ] || die "must run on Linux"
ptr_bytes="$(printf '#include <stdio.h>\nint main(void){printf("%%zu",sizeof(void*));return 0;}\n' \
  | ${CC:-cc} -x c - -o /tmp/ccc_ilp32_ptrsz && /tmp/ccc_ilp32_ptrsz)"
[ "$ptr_bytes" = "4" ] || die "expected ILP32 sizeof(void*)==4, got $ptr_bytes"

export CFLAGS="-D_FILE_OFFSET_BITS=64 ${CFLAGS:-}"
export CXXFLAGS="-D_FILE_OFFSET_BITS=64 ${CXXFLAGS:-}"
export CPPFLAGS="-D_FILE_OFFSET_BITS=64 ${CPPFLAGS:-}"

printf '== submodules (tcc, xjb)\n'
git submodule update --init third_party/tcc third_party/xjb
if [ ! -f third_party/tcc/configure ]; then
  git submodule update --checkout --force third_party/tcc
fi

printf '== clean prior build artifacts\n'
if [ -f third_party/tcc/Makefile ]; then
  make -C third_party/tcc clean >/dev/null 2>&1 || true
fi
make -C cc clean >/dev/null 2>&1 || true
rm -rf out/cc out/bin 2>/dev/null || true

printf '== TinyCC (patched, --config-cc_ext)\n'
(cd third_party/tcc && ./configure --config-cc_ext && make libtcc.a tcc libtcc1.a -j"$JOBS")
nm third_party/tcc/libtcc.a | grep cc_ast_record >/dev/null \
  || die "libtcc.a missing CC hooks (cc_ast_record_*)"

printf '== ccc compiler\n'
make -C cc BUILD="${BUILD:-debug}" TCC_EXT=1 TCC_INC=../third_party/tcc TCC_LIB=../third_party/tcc/libtcc.a -j"$JOBS"
test -x cc/bin/.ccc-bin || die "cc/bin/.ccc-bin not produced"
file_out="$(file -b cc/bin/.ccc-bin)"
printf '  ccc: %s\n' "$file_out"
echo "$file_out" | grep -q 'ELF 32-bit' || die "ccc is not ELF 32-bit: $file_out"
