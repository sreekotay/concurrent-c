#!/bin/sh
# A build file has no fixed limit on its targets, their sources, their deps
# or the length of a line.
#
# The driver used to parse build.cc into a 64-entry table (sources: 64 per
# target; 2 KiB lines read with fgets), and the target build sized its
# needed / closure / cache tables at 64 and its link object list at 256.
# A project past 64 targets had to split its build file. Here:
#   - 200 obj targets, chained in runs of ten, and one exe that depends on
#     all of them on a single ~5 KiB CC_TARGET_DEPS line;
#   - the exe has 80 sources and 100 CC_TARGET_DEFINE values over two lines;
#   - it builds and runs serially (-j1) and with the parallel scheduler;
#   - `build list` shows every target and dep; a duplicate target is an error.
set -eu
ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
CCC="${CCC:-$ROOT_DIR/cc/bin/ccc}"

fail() { echo "[test_build_many_targets] FAIL: $1" >&2; exit 1; }

[ -x "$CCC" ] || fail "missing $CCC"

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
mkdir -p "$work/src"

ntargets=200
nextra=80
ndefs=100
bf="$work/build.cc"
: > "$bf"

i=0
deps=""
while [ "$i" -lt "$ntargets" ]; do
    name="many_targets_part_$i"
    printf 'int many_targets_part_%d(void) { return %d; }\n' "$i" "$i" > "$work/src/part_$i.c"
    printf 'CC_TARGET %s obj src/part_%d.c\n' "$name" "$i" >> "$bf"
    # Chains of ten: part_k depends on part_{k-1} within each run.
    if [ $((i % 10)) -ne 0 ]; then
        printf 'CC_TARGET_DEPS %s many_targets_part_%d\n' "$name" $((i - 1)) >> "$bf"
    fi
    deps="$deps $name"
    i=$((i + 1))
done

srcs="src/main.c"
k=0
while [ "$k" -lt "$nextra" ]; do
    printf 'int many_targets_extra_%d(void) { return 1; }\n' "$k" > "$work/src/extra_$k.c"
    srcs="$srcs src/extra_$k.c"
    k=$((k + 1))
done

{
    printf '#include <stdio.h>\n'
    i=0
    while [ "$i" -lt "$ntargets" ]; do
        printf 'int many_targets_part_%d(void);\n' "$i"
        i=$((i + 1))
    done
    k=0
    while [ "$k" -lt "$nextra" ]; do
        printf 'int many_targets_extra_%d(void);\n' "$k"
        k=$((k + 1))
    done
    printf 'int main(void) {\n    long sum = 0;\n    int extra = 0;\n'
    i=0
    while [ "$i" -lt "$ntargets" ]; do
        printf '    sum += many_targets_part_%d();\n' "$i"
        i=$((i + 1))
    done
    k=0
    while [ "$k" -lt "$nextra" ]; do
        printf '    extra += many_targets_extra_%d();\n' "$k"
        k=$((k + 1))
    done
    printf '    printf("many-targets sum=%%ld extra=%%d def=%%d\\n", sum, extra, MANY_TARGETS_DEF_%d);\n' $((ndefs - 1))
    printf '    return 0;\n}\n'
} > "$work/src/main.c"

defs1=""
defs2=""
d=0
while [ "$d" -lt "$ndefs" ]; do
    if [ "$d" -lt $((ndefs / 2)) ]; then
        defs1="$defs1 MANY_TARGETS_DEF_$d=$d"
    else
        defs2="$defs2 MANY_TARGETS_DEF_$d=$d"
    fi
    d=$((d + 1))
done

{
    printf 'CC_TARGET many_targets_app exe %s\n' "$srcs"
    printf 'CC_TARGET_DEPS many_targets_app%s\n' "$deps"
    printf 'CC_TARGET_DEFINE many_targets_app%s\n' "$defs1"
    printf 'CC_TARGET_DEFINE many_targets_app%s\n' "$defs2"
    printf 'CC_DEFAULT many_targets_app\n'
} >> "$bf"

deps_len=$(grep '^CC_TARGET_DEPS many_targets_app' "$bf" | wc -c)
[ "$deps_len" -gt 4096 ] || fail "the deps line is only $deps_len bytes; the test wants one past 4 KiB"

cd "$work"

# --- list: every target and every dep of the exe --------------------------
list="$("$CCC" build list --build-file build.cc 2>&1)" \
    || { printf '%s\n' "$list" >&2; fail "build list failed"; }
got=$(printf '%s\n' "$list" | grep -c '^target ')
[ "$got" -eq $((ntargets + 1)) ] || fail "build list shows $got targets, want $((ntargets + 1))"
app_deps=$(printf '%s\n' "$list" | awk '/^target many_targets_app /{f=1; next} /^target /{f=0} f && /^  deps:/' \
    | tr ' ' '\n' | grep -c '^many_targets_part_')
[ "$app_deps" -eq "$ntargets" ] || fail "many_targets_app lists $app_deps deps, want $ntargets"
app_srcs=$(printf '%s\n' "$list" | awk '/^target many_targets_app /{f=1; next} /^target /{f=0} f && /^  src:/' \
    | tr ' ' '\n' | grep -c '\.c$')
[ "$app_srcs" -eq $((nextra + 1)) ] || fail "many_targets_app lists $app_srcs sources, want $((nextra + 1))"

want="many-targets sum=$((ntargets * (ntargets - 1) / 2)) extra=$nextra def=$((ndefs - 1))"

# --- serial (-j1) and parallel builds link all 281 objects -----------------
for jobs in 1 8; do
    out="$("$CCC" --out-dir "$work/out$jobs" --bin-dir "$work/bin$jobs" \
            build -j"$jobs" --build-file build.cc run many_targets_app 2>&1)" \
        || { printf '%s\n' "$out" | tail -20 >&2; fail "build -j$jobs run failed"; }
    printf '%s\n' "$out" | grep -qF "$want" \
        || { printf '%s\n' "$out" | tail -20 >&2; fail "-j$jobs binary did not print: $want"; }
done

# --- a duplicate target is an error naming the line -----------------------
printf 'CC_TARGET many_targets_part_7 obj src/part_7.c\n' >> "$bf"
if out="$("$CCC" build list --build-file build.cc 2>&1)"; then
    fail "a duplicate CC_TARGET was accepted"
fi
printf '%s\n' "$out" | grep -q "build.cc:[0-9]*: duplicate CC_TARGET many_targets_part_7" \
    || { printf '%s\n' "$out" >&2; fail "duplicate CC_TARGET diagnostic"; }

echo "[test_build_many_targets] ok"
