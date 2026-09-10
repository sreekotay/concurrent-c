#!/usr/bin/env sh
# The clean lowerer's self-hosting fixed point.
#
#   gen0  the lowerer as built by the bootstrap (shadow) path
#   gen1  the lowerer built from what gen0 lowered its own sources to
#
# gen1 lowering the same sources must produce the same bytes: the C of
# every tool, the module unit lower_cch.c and every .h. A difference means
# gen0 lowered itself into a program that does not agree with it, which no
# corpus row can be relied on to catch.
#
# The four tools include the face of module `lower` (cc/lower/lower.cch),
# whose members lower once into lower_cch.c. The driver does that staging
# and the link, with CC_CLEAN_TOOL naming the lowerer of the generation;
# each generation writes under its own out dir, so nothing carries over.
# The one line that legitimately differs is a `#line` naming the staged
# copy a generation lowered from, under its own out dir; it is normalised
# before the compare.
#
#   scripts/lowerer_selfhost.sh [tool ...]      (default: all four)
set -e

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$root"

work=${CC_SELFHOST_DIR:-out/.cc-build/selfhost}
gen0=$root/out/cc/bin/cclower_cc
tools=${*:-cclower cclex ccparse ccindex}

[ -x "$gen0" ] || { echo "selfhost: no $gen0 (make -C cc lower-cc)" >&2; exit 2; }

rm -rf "$work"
mkdir -p "$work"
work=$(CDPATH= cd -- "$work" && pwd)

# the lowerer calls the comptime executor; running compile-time code needs
# a C compiler, so a generation links what `make -C cc lower-cc` links
comptime_libs="$root/out/cc/obj/libshadow_comptime.a $root/${CC_TCC_LIB:-third_party/tcc/libtcc.a} -ldl"

build() {  # build <lowerer> <out dir> <binary>: cclower built through the driver
    CC_CLEAN_TOOL=$1 CC_OUT_DIR=$2 out/cc/bin/ccc build --lowerer=clean --no-cache \
        cc/lower/cclower.ccs --ld-flags "$comptime_libs" -o "$3"
}
emit() {  # emit <lowerer> <out dir> <tool> <out.c>: the C of a tool
    CC_CLEAN_TOOL=$1 CC_OUT_DIR=$2 out/cc/bin/ccc --lowerer=clean --no-cache --emit-c-only \
        "cc/lower/$3.ccs" -o "$4"
}
norm() {  # norm <file> <out>: the staged copy a generation lowered from, unnamed
    sed -E 's#^(\#line [0-9]+ ").*/\.cc-build/(clean_comptime|modules)/[A-Za-z0-9_.]+\.ccs"#\1<stage>"#' \
        "$1" > "$2"
}
same() {  # same <a> <b>: byte-equal after norm
    norm "$1" "$work/a.norm"
    norm "$2" "$work/b.norm"
    cmp -s "$work/a.norm" "$work/b.norm"
}

echo "selfhost: gen0 builds gen1"
build "$gen0" "$work/o0" "$work/cclower_gen1"
echo "selfhost: gen1 builds gen2"
build "$work/cclower_gen1" "$work/o1" "$work/cclower_gen2"

rc=0
for t in $tools; do
    emit "$gen0" "$work/o0" "$t" "$work/${t}0.c"
    emit "$work/cclower_gen1" "$work/o1" "$t" "$work/${t}1.c"
    if same "$work/${t}0.c" "$work/${t}1.c"; then
        echo "selfhost: $t identical"
    else
        echo "selfhost: $t DIFFERS"
        cmp "$work/a.norm" "$work/b.norm" || true
        rc=1
    fi
done

# the module unit and every lowered .h, as each generation wrote them
h0=$work/o0/.cc-build/clean/cc/lower
h1=$work/o1/.cc-build/clean/cc/lower
for f in "$h0"/*.h "$h0"/lower_cch.c; do
    b=$(basename -- "$f")
    if [ ! -f "$h1/$b" ]; then
        echo "selfhost: $b MISSING from gen1"
        rc=1
    elif same "$f" "$h1/$b"; then
        echo "selfhost: $b identical"
    else
        echo "selfhost: $b DIFFERS"
        cmp "$work/a.norm" "$work/b.norm" || true
        rc=1
    fi
done

[ $rc -eq 0 ] && echo "selfhost: fixed point"
exit $rc
