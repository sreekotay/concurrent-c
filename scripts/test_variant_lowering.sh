#!/bin/sh
# @variant lowering shape assertions (spec/draft_variants.md §3, normative).
#
# The .ccs harness pins the SEMANTICS of @variant; this selftest pins the
# emitted C SHAPE — the lowered C is a first-class, ABI-stable surface
# (identical to the schema one-of shape), so drift in the enum spelling,
# the struct layout, the tag rewrite, or the projection rewrite is a
# contract break even if behavior happens to survive.
set -eu
ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT_DIR"
CCC=./cc/bin/ccc

fail() { echo "[test_variant_lowering] FAIL: $1" >&2; exit 1; }

SRC=tests/variant_decl_lowering_smoke.ccs
[ -f "$SRC" ] || fail "missing $SRC"

out_dir="$(mktemp -d)"
trap 'rm -rf "$out_dir"' EXIT
emitted="$out_dir/variant_decl_lowering_smoke.c"

"$CCC" build --no-cache --emit-c-only "$SRC" -o "$emitted" >/dev/null 2>&1 \
  || fail "emit-c-only build of $SRC failed"
[ -s "$emitted" ] || fail "no emitted C produced"

# (1) enum: Name_<arm> spelling, NameKind typedef (schema one-of naming).
grep -q 'typedef enum { RedisValue_txt, RedisValue_num } RedisValueKind;' "$emitted" \
  || fail "enum lowering shape drifted (expected 'typedef enum { RedisValue_txt, RedisValue_num } RedisValueKind;')"

# (2) struct: kind + union u, layout-compatible with schema one-of.
grep -q 'typedef struct RedisValue { RedisValueKind kind; union { const char\* txt; int64_t num; } u; } RedisValue;' "$emitted" \
  || fail "struct lowering shape drifted (kind + union u)"

# (3) designated init got the tag auto-filled.
grep -q '{ \.kind = RedisValue_num, \.u\.num = 42 }' "$emitted" \
  || fail "designated-init tag auto-fill missing"

# (4) raw @variant surface never leaks into the emitted C (comments in the
#     source may mention the token; a real declaration starts its own line).
if grep -qE '^[[:space:]]*@variant' "$emitted"; then
  fail "raw '@variant' declaration leaked into emitted C"
fi

# (5) subject-switch + designator labels lower to the tag dialect.
SRC2=tests/variant_subject_switch_smoke.ccs
emitted2="$out_dir/variant_subject_switch_smoke.c"
"$CCC" build --no-cache --emit-c-only "$SRC2" -o "$emitted2" >/dev/null 2>&1 \
  || fail "emit-c-only build of $SRC2 failed"
grep -q 'switch ((cell)->kind)' "$emitted2" \
  || fail "pointer subject-switch did not lower to (cell)->kind"
grep -q 'switch ((s)\.kind)' "$emitted2" \
  || fail "value subject-switch did not lower to (s).kind"
grep -q 'case RedisValue_num:' "$emitted2" \
  || fail "designator case label did not lower to the tag constant"
grep -q 'cell->u\.num += delta;' "$emitted2" \
  || fail "dominated pointer projection did not lower to ->u.num"

echo "[test_variant_lowering] OK"
