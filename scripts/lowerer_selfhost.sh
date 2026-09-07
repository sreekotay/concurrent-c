#!/usr/bin/env sh
# The clean lowerer's self-hosting fixed point.
#
#   gen0  the lowerer as built by the bootstrap (shadow) path
#   gen1  the lowerer built from what gen0 lowered its own sources to
#
# gen1 lowering the same sources must produce the same bytes — the .c and
# every .h. A difference means gen0 lowered itself into a program that
# does not agree with it, which no corpus row can be relied on to catch.
#
#   scripts/lowerer_selfhost.sh [tool ...]      (default: all four)
set -e

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$root"

work=${CC_SELFHOST_DIR:-out/.cc-build/selfhost}
gen0=out/cc/bin/cclower_cc
tools=${*:-cclower cclex ccparse ccindex}

[ -x "$gen0" ] || { echo "selfhost: no $gen0 (make -C cc lower-cc)" >&2; exit 2; }

rm -rf "$work"
mkdir -p "$work"

lower() {  # lower <binary> <unit.ccs> <h-root> <out.c>
    "$1" --lower "cc/lower/$2.ccs" -I cc/include --root "$root" \
         --h-root "$root/$3" --quote-dir cc/lower -o "$4"
}

echo "selfhost: gen0 lowers cclower.ccs"
lower "$gen0" cclower "$work/h0" "$work/cclower0.c"

echo "selfhost: building gen1"
cc -std=c11 -D_DEFAULT_SOURCE -O2 -w \
   -I "$work/h0" -I out/include -I cc/include -I cc/lower \
   -c "$work/cclower0.c" -o "$work/gen1.o"
cc "$work/gen1.o" out/cc/obj/runtime/concurrent_c.o \
   -o "$work/cclower_gen1" -lpthread -lm

rc=0
for t in $tools; do
    if [ "$t" != cclower ]; then
        lower "$gen0" "$t" "$work/h0_$t" "$work/${t}0.c"
    fi
    a_h=$work/h0; a_c=$work/cclower0.c
    [ "$t" = cclower ] || { a_h=$work/h0_$t; a_c=$work/${t}0.c; }
    lower "$work/cclower_gen1" "$t" "$work/h1_$t" "$work/${t}1.c"
    if cmp -s "$a_c" "$work/${t}1.c" && diff -rq "$a_h" "$work/h1_$t" >/dev/null; then
        echo "selfhost: $t identical"
    else
        echo "selfhost: $t DIFFERS"
        cmp "$a_c" "$work/${t}1.c" || true
        diff -rq "$a_h" "$work/h1_$t" || true
        rc=1
    fi
done

[ $rc -eq 0 ] && echo "selfhost: fixed point"
exit $rc
