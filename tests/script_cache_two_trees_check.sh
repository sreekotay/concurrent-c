#!/bin/sh
# Two trees' same-named scripts must not share a stage: intermediates are
# named by the stem (out/make.c, make.o), so a shared stage let concurrent
# builds swap each other's C between compile and link.
set -e
root=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
base=/tmp/cc-script-two-trees-$$
rm -rf "$base"
mkdir -p "$base/tmp" "$base/a" "$base/b"
trap 'rm -rf "$base"' EXIT
for t in a b; do
  printf '"tree %s".println();\nreturn 0;\n' "$t" > "$base/$t/make.shcc"
done
cd "$root"
for t in a b; do
  TMPDIR="$base/tmp" ./cc/bin/ccc "$base/$t/make.shcc" > "$base/$t.out" 2>&1
  grep -q "tree $t" "$base/$t.out" || { echo "tree $t ran: $(cat "$base/$t.out")" >&2; exit 1; }
done
n=$(find "$base/tmp" -path '*/out/make.c' | wc -l | tr -d ' ')
if [ "$n" != 2 ]; then
  echo "expected one staged make.c per tree, found $n" >&2
  find "$base/tmp" -path '*/out/make.c' >&2
  exit 1
fi
i=0
while [ $i -lt 5 ]; do
  touch "$base/a/make.shcc" "$base/b/make.shcc"
  TMPDIR="$base/tmp" ./cc/bin/ccc "$base/a/make.shcc" > "$base/a.out" 2>&1 &
  TMPDIR="$base/tmp" ./cc/bin/ccc "$base/b/make.shcc" > "$base/b.out" 2>&1 &
  wait
  for t in a b; do
    grep -q "tree $t" "$base/$t.out" || { echo "round $i: tree $t ran: $(cat "$base/$t.out")" >&2; exit 1; }
  done
  i=$((i + 1))
done
exit 0
