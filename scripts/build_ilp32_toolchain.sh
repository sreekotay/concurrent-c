#!/usr/bin/env bash
# build_ilp32_toolchain.sh — build patched TinyCC + ccc on native Linux ILP32.
# Used by smoke_ilp32.sh and pigz ILP32 compare. Expects cwd = repo root.
#
# Env:
#   CCC_HOST_CC   Host C compiler for building ccc (default: cc).
#                 Set to `tcc` to self-build ccc with the just-built
#                 third_party/tcc/tcc (Linux only).
#   BUILD         debug|release (default: debug)
#   CCC_ILP32_JOBS
set -euo pipefail

JOBS="${CCC_ILP32_JOBS:-$(nproc 2>/dev/null || echo 4)}"
HOST_CC="${CCC_HOST_CC:-cc}"

die() { printf 'build_ilp32_toolchain: %s\n' "$*" >&2; exit 1; }

[ "$(uname -s)" = "Linux" ] || die "must run on Linux"
# Probe ILP32 with the system compiler (always available in the image).
ptr_bytes="$(printf '#include <stdio.h>\nint main(void){printf("%%zu",sizeof(void*));return 0;}\n' \
  | cc -x c - -o /tmp/ccc_ilp32_ptrsz && /tmp/ccc_ilp32_ptrsz)"
[ "$ptr_bytes" = "4" ] || die "expected ILP32 sizeof(void*)==4, got $ptr_bytes"

export CFLAGS="-D_FILE_OFFSET_BITS=64 ${CFLAGS:-}"
export CPPFLAGS="-D_FILE_OFFSET_BITS=64 ${CPPFLAGS:-}"

host_is_tcc=0
case "$(basename "$HOST_CC")" in
  tcc) host_is_tcc=1 ;;
esac

printf '== submodules (tcc)\n'
git submodule update --init third_party/tcc
if [ ! -f third_party/tcc/configure ]; then
  git submodule update --checkout --force third_party/tcc
fi

printf '== clean prior build artifacts\n'
if [ -f third_party/tcc/Makefile ]; then
  make -C third_party/tcc clean >/dev/null 2>&1 || true
fi
make -C cc clean >/dev/null 2>&1 || true
rm -rf out/cc out/cc-tcc out/bin 2>/dev/null || true

printf '== TinyCC (patched, --config-cc_ext)\n'
./scripts/apply_tcc_patches.sh
# Linux configure omits CONFIG_TCC_SYSINCLUDEPATHS; in-tree tcc needs gcc's paths.
tcc_configure_extras=(--config-bcheck=no)
tcc_sysinc="$(gcc -Wp,-v -E -x c /dev/null 2>&1 \
  | awk '/^ /{gsub(/^ +/,""); if ($0 != "" && $0 !~ /search/) print}' \
  | paste -sd: - || true)"
tcc_inc_dir="$(pwd)/third_party/tcc/include"
if [ -n "$tcc_sysinc" ]; then
  tcc_configure_extras+=(--sysincludepaths="$tcc_inc_dir:$tcc_sysinc")
else
  tcc_configure_extras+=(--sysincludepaths="$tcc_inc_dir")
fi
(cd third_party/tcc && ./configure --config-cc_ext "${tcc_configure_extras[@]}" \
  && make libtcc.a tcc libtcc1.a -j"$JOBS")
nm third_party/tcc/libtcc.a >/dev/null \
  || die "libtcc.a missing after patched build"
grep -q 'CONFIG_cc_ext=yes' third_party/tcc/config.mak \
  || die "tcc config.mak missing CONFIG_cc_ext=yes"

if [ "$host_is_tcc" = 1 ]; then
  HOST_CC="$(pwd)/third_party/tcc/tcc"
  test -x "$HOST_CC" || die "expected tcc binary at $HOST_CC"
  printf '== ccc compiler (host CC=tcc self-build)\n'
else
  printf '== ccc compiler (host CC=%s)\n' "$HOST_CC"
fi

make -C cc CC="$HOST_CC" BUILD="${BUILD:-debug}" TCC_EXT=1 \
  TCC_INC=../third_party/tcc TCC_LIB=../third_party/tcc/libtcc.a -j"$JOBS"
test -x cc/bin/.ccc-bin || die "cc/bin/.ccc-bin not produced"
file_out="$(file -b cc/bin/.ccc-bin)"
printf '  ccc: %s\n' "$file_out"
echo "$file_out" | grep -q 'ELF 32-bit' || die "ccc is not ELF 32-bit: $file_out"
