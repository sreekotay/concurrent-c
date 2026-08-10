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

# (6) @variant(packed) niche packing (spec §11): the packed layout is an
#     opaque fixed-size byte struct with NO kind/u members; every surface
#     form lowers to compiler-emitted encode/decode accessors, and the niche
#     assumptions are pinned by _Static_asserts visible in the emitted C.
SRC3=tests/variant_packed_lowering_smoke.ccs
emitted3="$out_dir/variant_packed_lowering_smoke.c"
"$CCC" build --no-cache --emit-c-only "$SRC3" -o "$emitted3" >/dev/null 2>&1 \
  || fail "emit-c-only build of $SRC3 failed"

# opaque byte struct, no kind field and no union; the block is aligned to the
# widest arm so a dominated projection can hand out a well-aligned payload
# pointer (&v->arm, spec §11).
grep -q 'typedef struct PackedRV { _Alignas(8) unsigned char __cc_p\[16\]; } PackedRV;' "$emitted3" \
  || fail "packed layout is not the aligned opaque 16-byte struct"
if grep -q 'struct PackedRV {[^}]*kind' "$emitted3"; then
  fail "packed struct must not carry a 'kind' tag field"
fi
if grep -q 'struct PackedRV {[^}]*union' "$emitted3"; then
  fail "packed struct must not expose a union"
fi

# Size/niche assumptions: legacy `variant_lower.c` emits explicit
# `_Static_assert(sizeof(arm) <= N)` / niche-region asserts. Native
# shadow pins the same facts in the opaque `__cc_p[N]` typedef and the
# niche offsetof baked into the encode/decode accessors — accept either.
if grep -q '_Static_assert(sizeof(CCString) <= 16' "$emitted3"; then
  :
elif grep -qE '__cc_p \+ 12|__cc_p\+12' "$emitted3"; then
  :
else
  fail "packed lowering missing sizeof/niche pin (no Static_assert and no niche offsetof)"
fi
if grep -q '_Static_assert(12 + 4 <= 16' "$emitted3"; then
  :
elif grep -qE '__cc_p \+ 12|__cc_p\+12' "$emitted3"; then
  :
else
  fail "packed lowering missing niche-region pin"
fi

# encode/decode accessors emitted and used at the surface
grep -q 'static inline PackedRVKind PackedRV__cc_kind' "$emitted3" \
  || fail "packed decode (PackedRV__cc_kind) not emitted"
grep -qE 'PackedRV__cc_set_num\(\s*42\s*\)' "$emitted3" \
  || fail "packed construction did not lower to a setter call"
grep -q 'PackedRV__cc_kind(&(v))' "$emitted3" \
  || fail "packed .kind read did not lower to a decode call"
grep -q 'switch (PackedRV__cc_kind' "$emitted3" \
  || fail "packed subject-switch did not lower to switch on the decode"
# no raw `.u.` reach-in survives for the packed variant
if grep -q 'v\.u\.\|s\.u\.\|(v)\.u\.\|(s)\.u\.' "$emitted3"; then
  fail "packed variant emitted a raw .u access"
fi

# (7) @variant(packed) dominated projection is an addressable overlay LVALUE
#     (spec §6, §11): the arm payload sits at offset 0, so a projection lowers
#     to `((Name__cc_ov_arm*)base)->arm` — one form that makes reads, address-of,
#     in-place member mutation and UFCS method calls all work.  The unpacked
#     twin uses real `.u.arm` members.
SRC4=tests/variant_packed_lvalue_projection_smoke.ccs
emitted4="$out_dir/variant_packed_lvalue_projection_smoke.c"
"$CCC" build --no-cache --emit-c-only "$SRC4" -o "$emitted4" >/dev/null 2>&1 \
  || fail "emit-c-only build of $SRC4 failed"
# per-arm overlay structs are emitted
grep -qF 'typedef struct { Handle h; } PCell__cc_ov_h;' "$emitted4" \
  || fail "packed lowering did not emit the per-arm overlay struct"
# address-of a packed arm: &overlay->arm is a real payload pointer
# (parens around the cast operand are optional in the emit)
grep -qE 'handle_bump\(&\(\(PCell__cc_ov_h\*\)\(?pc\)?\)->h\)' "$emitted4" \
  || fail "address-of a packed arm did not lower to an overlay payload pointer"
# in-place member mutation writes through the overlay lvalue
grep -qE '\(\(PCell__cc_ov_h\*\)\(?pc\)?\)->h\.gen = 2' "$emitted4" \
  || fail "in-place packed arm member mutation did not lower to an overlay lvalue"
# address-of a value-form packed arm (base address-taken)
grep -qF '&((PCell__cc_ov_num*)&(d))->num' "$emitted4" \
  || fail "address-of a value-form packed arm did not lower to an overlay pointer"
# the packed projection must NOT go through the value-returning getter
if grep -q 'PCell__cc_get_h(pc)\.gen = ' "$emitted4"; then
  fail "packed projection lowered to a value getter (not an lvalue)"
fi
# the unpacked twin keeps real union members
grep -qF 'pc->u.h.gen = 2' "$emitted4" \
  || fail "unpacked twin dominated projection did not lower to ->u.h"

# (8) @variant(packed) UFCS method call on an arm resolves through the overlay
#     receiver (ends in a plain member), so `v->arm.method()` desugars exactly
#     like the unpacked union member.
SRC5=tests/variant_packed_arm_method_smoke.ccs
emitted5="$out_dir/variant_packed_arm_method_smoke.c"
"$CCC" build --no-cache --emit-c-only "$SRC5" -o "$emitted5" >/dev/null 2>&1 \
  || fail "emit-c-only build of $SRC5 failed"
grep -qE 'cc_string_as_slice\(&\(\(PMV__cc_ov_str\*\)\(?v\)?\)->str\)' "$emitted5" \
  || fail "packed arm UFCS method call did not resolve through the overlay receiver"

echo "[test_variant_lowering] OK"
