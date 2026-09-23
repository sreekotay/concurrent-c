#!/bin/sh
# A link line longer than any fixed buffer must link the binary it names.
#
# The driver used to build its host-cc commands in fixed buffers and strncat
# into them: past the end, `-o <bin>` was cut to a prefix, the host cc wrote
# a binary under that prefix and returned 0, and `build run` then failed with
# `execv: No such file or directory`. Three shapes reach it:
#   - build.cc: many objects, linked by cc__link_many (4 KiB);
#   - one unit: long --ld-flags (a 1 KiB copy, then a 2 KiB link line);
#   - one unit under a deep --out-dir: the 2 KiB compile line.
set -eu
ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
CCC="${CCC:-$ROOT_DIR/cc/bin/ccc}"

fail() { echo "[test_long_link_line] FAIL: $1" >&2; exit 1; }

[ -x "$CCC" ] || fail "missing $CCC"

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

# A ~100-byte project directory: every object path on the link line
# carries it, and the compile lines still fit the old 2 KiB.
proj="$work/aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa/bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"
deep="$proj"
mkdir -p "$deep/src"

# --- build.cc: 40 objects, each path ~200 bytes -> link line ~8 KiB --------
n=40
srcs=""
i=0
while [ "$i" -lt "$n" ]; do
    printf 'int long_link_part_%d(void) { return %d; }\n' "$i" "$i" \
        > "$deep/src/part_$i.ccs"
    srcs="$srcs src/part_$i.ccs"
    i=$((i + 1))
done
{
    printf '#include <stdio.h>\n'
    i=0
    while [ "$i" -lt "$n" ]; do
        printf 'int long_link_part_%d(void);\n' "$i"
        i=$((i + 1))
    done
    printf 'int main(void) {\n    int sum = 0;\n'
    i=0
    while [ "$i" -lt "$n" ]; do
        printf '    sum += long_link_part_%d();\n' "$i"
        i=$((i + 1))
    done
    printf '    printf("long-link sum=%%d\\n", sum);\n    return 0;\n}\n'
} > "$deep/src/main.ccs"
printf 'CC_TARGET long_link_probe exe src/main.ccs%s\nCC_DEFAULT long_link_probe\n' \
    "$srcs" > "$deep/build.cc"

expect_sum=$((n * (n - 1) / 2))
cd "$deep"
out="$("$CCC" --out-dir "$deep/out" --bin-dir "$deep/bin" \
        build --build-file build.cc run long_link_probe 2>&1)" \
    || { printf '%s\n' "$out" >&2; fail "build.cc run with a long link line failed"; }
printf '%s\n' "$out" | grep -q "long-link sum=$expect_sum" \
    || { printf '%s\n' "$out" >&2; fail "build.cc binary did not run (sum=$expect_sum)"; }
[ -x "$deep/bin/long_link_probe" ] \
    || fail "expected $deep/bin/long_link_probe"

# --- one unit: --ld-flags past 1 KiB and the old 2 KiB link buffer ----------
ld=""
i=0
while [ "$i" -lt 20 ]; do
    ld="$ld -L$deep/lib_search_dir_$i"
    i=$((i + 1))
done
printf 'int main(void) { return 0; }\n' > "$deep/one.ccs"
mkdir -p "$deep/bin1"
"$CCC" --out-dir "$deep/out1" --ld-flags "$ld" build "$deep/one.ccs" \
    -o "$deep/bin1/one_unit_probe" >/dev/null 2>&1 \
    || fail "single-unit build with long --ld-flags failed"
[ -x "$deep/bin1/one_unit_probe" ] \
    || fail "expected $deep/bin1/one_unit_probe"
"$deep/bin1/one_unit_probe" || fail "single-unit binary did not run"

# --- one unit under a ~400-byte --out-dir: the compile line ---------------
deep="$proj"
for seg in cccccccccccccccccccccccccccccccccccccccccccc \
           dddddddddddddddddddddddddddddddddddddddddddd \
           eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee \
           ffffffffffffffffffffffffffffffffffffffffffff; do
    deep="$deep/$seg"
done
mkdir -p "$deep/bin2"
cp "$proj/one.ccs" "$deep/one.ccs"
"$CCC" --out-dir "$deep/out2" build "$deep/one.ccs" \
    -o "$deep/bin2/deep_unit_probe" >/dev/null 2>&1 \
    || fail "single-unit build under a deep --out-dir failed"
"$deep/bin2/deep_unit_probe" || fail "deep-dir binary did not run"

echo "[test_long_link_line] ok"
