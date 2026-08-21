#!/bin/sh
# Step-1 cparse: preserve reprints both #if arms; evaluate marks live/dead
# without dropping the dead arm from the tree.
set -eu
ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT_DIR"
BIN=out/cc/bin/cparse-dump

fail() { echo "[test_cparse] FAIL: $1" >&2; exit 1; }

make -C cc/cparse -s || fail "make cc/cparse"
[ -x "$BIN" ] || fail "missing $BIN"

line_of() {
    grep -n "$2" "$1" | head -1 | cut -d: -f1
}

ord() {
    [ -n "$1" ] && [ -n "$2" ] || fail "$3 (missing line)"
    [ "$1" -lt "$2" ] || fail "$3"
}

# --- process_fields: preserve ---
pres="$("$BIN" --preserve tests/cparse/process_fields.c)" || fail "preserve process_fields"
presf="$(mktemp)"
ev0="$(mktemp)"
ev1="$(mktemp)"
trap 'rm -f "$presf" "$ev0" "$ev1"' EXIT
printf '%s\n' "$pres" >"$presf"

ifdef_w=$(line_of "$presf" '#ifdef _WIN32')
handle=$(line_of "$presf" 'handle')
else_w=$(line_of "$presf" '#else')
posix=$(line_of "$presf" 'posix_pid')
endif_w=$(line_of "$presf" '#endif')
stdin_fd=$(line_of "$presf" 'stdin_fd')
guard=$(line_of "$presf" '#define CC_STD_PROCESS_H')
ifndef_g=$(line_of "$presf" '#ifndef CC_STD_PROCESS_H')

[ -n "$ifdef_w" ] || fail "preserve dropped #ifdef _WIN32"
[ -n "$handle" ] || fail "preserve dropped handle"
[ -n "$else_w" ] || fail "preserve dropped #else"
[ -n "$posix" ] || fail "preserve dropped posix_pid"
[ -n "$endif_w" ] || fail "preserve dropped #endif"
[ -n "$stdin_fd" ] || fail "preserve dropped stdin_fd"
[ -n "$guard" ] || fail "preserve dropped object-like #define"
[ -n "$ifndef_g" ] || fail "preserve dropped #ifndef guard"

ord "$ifndef_g" "$guard" "guard #define precedes #ifndef"
ord "$ifdef_w" "$handle" "handle precedes #ifdef _WIN32"
ord "$handle" "$else_w" "#else precedes handle"
ord "$else_w" "$posix" "posix_pid precedes #else"
ord "$posix" "$endif_w" "#endif precedes posix_pid"
ord "$endif_w" "$stdin_fd" "stdin_fd precedes #endif (hoisted?)"

# --- evaluate without _WIN32: posix live, handle still in the tree ---
"$BIN" --evaluate tests/cparse/process_fields.c >"$ev0" || fail "evaluate (no _WIN32)"

grep -q 'field posix_pid live=1' "$ev0" || fail "posix_pid should be live without _WIN32"
grep -q 'field handle live=0' "$ev0" || fail "handle should be dead without _WIN32 (still in tree)"
grep -q 'field pid live=0' "$ev0" || fail "win pid should be dead without _WIN32"
grep -q 'field stdin_fd live=1' "$ev0" || fail "stdin_fd should be live"
grep -q '#ifdef _WIN32' "$ev0" || fail "evaluate dropped #ifdef (flattened?)"
grep -q '#else' "$ev0" || fail "evaluate dropped #else"
grep -q '#endif' "$ev0" || fail "evaluate dropped #endif"
grep -q 'define CC_STD_PROCESS_H live=1' "$ev0" || fail "include-guard #define should be live"

# --- evaluate -D_WIN32: handle live, posix dead, still in the tree ---
"$BIN" --evaluate -D_WIN32 tests/cparse/process_fields.c >"$ev1" || fail "evaluate -D_WIN32"

grep -q 'field handle live=1' "$ev1" || fail "handle should be live with _WIN32"
grep -q 'field pid live=1' "$ev1" || fail "win pid should be live with _WIN32"
grep -q 'field posix_pid live=0' "$ev1" || fail "posix_pid should be dead with _WIN32 (still in tree)"
grep -q 'field stdin_fd live=1' "$ev1" || fail "stdin_fd should stay live under _WIN32"

# --- attr fixture ---
ap="$("$BIN" --preserve tests/cparse/attr_unused.c)" || fail "preserve attr_unused"
printf '%s\n' "$ap" | grep -q '__attribute__((unused))' || fail "preserve missing __attribute__((unused))"
printf '%s\n' "$ap" | grep -q 'name(' || fail "preserve missing name("

ae="$("$BIN" --evaluate tests/cparse/attr_unused.c)" || fail "evaluate attr_unused"
printf '%s\n' "$ae" | grep -q 'func name' || fail "evaluate missing func name"
printf '%s\n' "$ae" | grep -q '__attribute__((unused))' || fail "evaluate missing attr on func"
printf '%s\n' "$ae" | grep -q 'live=1' || fail "func should be live"

pf="$("$BIN" --preserve tests/cparse/plain_fn.c)" || fail "preserve plain_fn"
printf '%s\n' "$pf" | grep -q 'cparse_plain_add' || fail "preserve dropped cparse_plain_add"
printf '%s\n' "$pf" | grep -q 'cparse_plain_main' || fail "preserve dropped cparse_plain_main"
printf '%s\n' "$pf" | grep -q 'return x + y' || fail "preserve dropped add body"

# --- FileTape-shaped tokens: '#' is a punct, not a glued directive ---
toks="$("$BIN" --tokens tests/cparse/process_fields.c)" || fail "tokens process_fields"
printf '%s\n' "$toks" | grep -q '^PUNCT #$' || fail "expected PUNCT #"
printf '%s\n' "$toks" | awk '
  $0=="PUNCT #" { hash=1; next }
  hash && $0=="IDENT ifdef" { found=1 }
  { hash=0 }
  END { if (!found) exit 1 }
' || fail "#ifdef is not PUNCT # + IDENT ifdef (glued DIR?)"
printf '%s\n' "$toks" | awk '
  $0=="PUNCT #" { hash=1; next }
  hash && $0=="IDENT define" { found=1 }
  { hash=0 }
  END { if (!found) exit 1 }
' || fail "#define is not PUNCT # + IDENT define"

# Line splice before lex (FileTape): `\` + newline disappears.
st="$("$BIN" --tokens tests/cparse/line_splice.c)" || fail "tokens line_splice"
printf '%s\n' "$st" | awk '
  $0=="PUNCT #" { hash=1; next }
  hash && $0=="IDENT ifdef" { hash=2; next }
  hash==2 && $0=="IDENT _WIN32" { found=1 }
  { hash=0 }
  END { if (!found) exit 1 }
' || fail "line splice did not yield # ifdef _WIN32"
printf '%s\n' "$st" | grep -q '\\\\' && fail "backslash survived splice"

sp="$("$BIN" --preserve tests/cparse/line_splice.c)" || fail "preserve line_splice"
printf '%s\n' "$sp" | grep -q '#ifdef _WIN32' || fail "spliced #ifdef _WIN32 missing in preserve"

# --- expand: macros, hide-set, # / ## / __VA_ARGS__, dead arm omitted ---
ex="$("$BIN" --expand tests/cparse/macros.c)" || fail "expand macros"
printf '%s\n' "$ex" | grep -q 'IDENT posix_pid' || fail "expand dropped else-arm posix_pid"
printf '%s\n' "$ex" | grep -q 'IDENT handle' && fail "expand kept dead _WIN32 arm"
printf '%s\n' "$ex" | grep -q 'IDENT posix' || fail "JOIN(pos,ix) did not paste to posix"
printf '%s\n' "$ex" | grep -q 'STR "hello"' || fail "STR(hello) did not stringify"
printf '%s\n' "$ex" | awk '
  $0=="IDENT f" { f=1; next }
  f && $0=="PUNCT (" { f=2; next }
  f==2 && $0=="NUM 1" { f=3; next }
  f==3 && $0=="PUNCT ," { f=4; next }
  f==4 && $0=="NUM 2" { found=1 }
  { f=0 }
  END { if (!found) exit 1 }
' || fail "WRAP(1, 2) did not expand to f(1, 2)"
printf '%s\n' "$ex" | grep -q 'IDENT E' || fail "hide-set #define E E should emit E"
printf '%s\n' "$ex" | grep -q 'IDENT once' || fail "include-guard body dropped"

exw="$("$BIN" --expand -D_WIN32 tests/cparse/macros.c)" || fail "expand -D_WIN32"
printf '%s\n' "$exw" | grep -q 'IDENT handle' || fail "-D_WIN32 should expand T handle"
printf '%s\n' "$exw" | grep -q 'IDENT posix_pid' && fail "-D_WIN32 kept posix_pid"

# Oracle: expanded token stream matches host cpp (-E -P -undef).
oracle() {
    defs="$1"
    src="$2"
    ours="$(mktemp)"
    hosti="$(mktemp)"
    hostt="$(mktemp)"
    # shellcheck disable=SC2086
    "$BIN" --expand $defs "$src" >"$ours" || {
        rm -f "$ours" "$hosti" "$hostt"
        fail "oracle expand $src"
    }
    # shellcheck disable=SC2086
    cc -E -P -undef -std=c11 $defs "$src" >"$hosti" || {
        rm -f "$ours" "$hosti" "$hostt"
        fail "oracle host cpp $src"
    }
    "$BIN" --tokens "$hosti" >"$hostt" || {
        rm -f "$ours" "$hosti" "$hostt"
        fail "oracle lex host $src"
    }
    if ! cmp -s "$ours" "$hostt"; then
        echo "[test_cparse] oracle mismatch $src $defs" >&2
        diff -u "$hostt" "$ours" >&2 || true
        rm -f "$ours" "$hosti" "$hostt"
        fail "expand != host cpp for $src"
    fi
    rm -f "$ours" "$hosti" "$hostt"
}

oracle "" tests/cparse/macros.c
oracle "-D_WIN32" tests/cparse/macros.c
oracle "" tests/cparse/process_fields.c
oracle "-D_WIN32" tests/cparse/process_fields.c
oracle "" tests/cparse/elif_fields.c
oracle "-D_WIN32" tests/cparse/elif_fields.c
oracle "-D__linux__" tests/cparse/elif_fields.c

# --- elif: preserve keeps the chain; evaluate marks one live arm ---
ep="$("$BIN" --preserve tests/cparse/elif_fields.c)" || fail "preserve elif_fields"
printf '%s\n' "$ep" | grep -q '#ifdef _WIN32' || fail "preserve elif dropped #ifdef"
printf '%s\n' "$ep" | grep -q '#elif defined(__linux__)' || fail "preserve dropped #elif"
printf '%s\n' "$ep" | grep -q 'linux_fd' || fail "preserve dropped linux_fd"
printf '%s\n' "$ep" | grep -q 'posix_pid' || fail "preserve dropped posix_pid"

ee0="$("$BIN" --evaluate tests/cparse/elif_fields.c)" || fail "evaluate elif (none)"
printf '%s\n' "$ee0" | grep -q 'field handle live=0' || fail "handle should be dead with no -D"
printf '%s\n' "$ee0" | grep -q 'field linux_fd live=0' || fail "linux_fd should be dead with no -D"
printf '%s\n' "$ee0" | grep -q 'field posix_pid live=1' || fail "posix_pid should be live with no -D"
printf '%s\n' "$ee0" | grep -q '#elif' || fail "evaluate dropped #elif"

ee1="$("$BIN" --evaluate -D_WIN32 tests/cparse/elif_fields.c)" || fail "evaluate elif -D_WIN32"
printf '%s\n' "$ee1" | grep -q 'field handle live=1' || fail "handle should be live with _WIN32"
printf '%s\n' "$ee1" | grep -q 'field linux_fd live=0' || fail "linux_fd should be dead with _WIN32"
printf '%s\n' "$ee1" | grep -q 'field posix_pid live=0' || fail "posix_pid should be dead with _WIN32"

ee2="$("$BIN" --evaluate -D__linux__ tests/cparse/elif_fields.c)" || fail "evaluate elif -D__linux__"
printf '%s\n' "$ee2" | grep -q 'field handle live=0' || fail "handle should be dead with __linux__"
printf '%s\n' "$ee2" | grep -q 'field linux_fd live=1' || fail "linux_fd should be live with __linux__"
printf '%s\n' "$ee2" | grep -q 'field posix_pid live=0' || fail "posix_pid should be dead with __linux__"

elf="$("$BIN" --fields tests/cparse/elif_fields.c)" || fail "fields elif_fields"
printf '%s\n' "$elf" | grep -q 'ppdir.*#elif' || fail "fields missing #elif"
printf '%s\n' "$elf" | grep -q 'field linux_fd' || fail "fields missing linux_fd"

uf="$("$BIN" --preserve tests/cparse/union_field.c)" || fail "preserve union_field"
printf '%s\n' "$uf" | grep -q 'union' || fail "preserve dropped union"
printf '%s\n' "$uf" | grep -q 'bytes' || fail "preserve dropped union member bytes"
printf '%s\n' "$uf" | grep -q 'extra' || fail "preserve dropped extra after union"
uff="$("$BIN" --fields tests/cparse/union_field.c)" || fail "fields union_field"
printf '%s\n' "$uff" | grep -q 'union' || fail "fields missing union declarator"
printf '%s\n' "$uff" | grep -q 'field extra' || fail "fields missing extra"

# --- compound #if / #elif: || && ! == ---
cp="$("$BIN" --preserve tests/cparse/if_compound.c)" || fail "preserve if_compound"
printf '%s\n' "$cp" | grep -q 'defined(_WIN32) || defined(_WIN64)' \
    || fail "preserve dropped ||"
printf '%s\n' "$cp" | grep -q 'defined(__linux__) && !defined(__ANDROID__)' \
    || fail "preserve dropped &&"
printf '%s\n' "$cp" | grep -q 'UINTPTR_MAX == 0xFFFFFFFF' \
    || fail "preserve dropped =="
printf '%s\n' "$cp" | grep -q 'posix_pid' || fail "preserve dropped posix_pid"

ce0="$("$BIN" --evaluate tests/cparse/if_compound.c)" || fail "evaluate if_compound"
printf '%s\n' "$ce0" | grep -q 'field handle live=0' || fail "handle dead with no -D"
printf '%s\n' "$ce0" | grep -q 'field linux_fd live=0' || fail "linux_fd dead with no -D"
printf '%s\n' "$ce0" | grep -q 'field ilp32 live=0' || fail "ilp32 dead (0==0xFFFFFFFF is false)"
printf '%s\n' "$ce0" | grep -q 'field posix_pid live=1' || fail "posix_pid live with no -D"

ce1="$("$BIN" --evaluate -D_WIN32 tests/cparse/if_compound.c)" \
    || fail "evaluate if_compound -D_WIN32"
printf '%s\n' "$ce1" | grep -q 'field handle live=1' || fail "handle live with _WIN32"
printf '%s\n' "$ce1" | grep -q 'field linux_fd live=0' || fail "linux_fd dead with _WIN32"
printf '%s\n' "$ce1" | grep -q 'field posix_pid live=0' || fail "posix_pid dead with _WIN32"

ce2="$("$BIN" --evaluate -D__linux__ tests/cparse/if_compound.c)" \
    || fail "evaluate if_compound -D__linux__"
printf '%s\n' "$ce2" | grep -q 'field handle live=0' || fail "handle dead with __linux__"
printf '%s\n' "$ce2" | grep -q 'field linux_fd live=1' || fail "linux_fd live with __linux__"
printf '%s\n' "$ce2" | grep -q 'field posix_pid live=0' || fail "posix_pid dead with __linux__"

cf="$("$BIN" --fields tests/cparse/if_compound.c)" || fail "fields if_compound"
printf '%s\n' "$cf" | grep -q '||' || fail "fields missing ||"
printf '%s\n' "$cf" | grep -q 'field linux_fd' || fail "fields missing linux_fd"

oracle "" tests/cparse/if_compound.c
oracle "-D_WIN32" tests/cparse/if_compound.c
oracle "-D__linux__" tests/cparse/if_compound.c

# --- evaluate expands macros in #if (same algorithm as --expand) ---
iep="$("$BIN" --preserve tests/cparse/if_expand.c)" || fail "preserve if_expand"
printf '%s\n' "$iep" | grep -q '#if FOO' || fail "preserve dropped #if FOO"
ie="$("$BIN" --evaluate tests/cparse/if_expand.c)" || fail "evaluate if_expand"
printf '%s\n' "$ie" | grep -q 'field foo_live live=0' \
    || fail "#define FOO 0 then #if FOO must be false (not defined-or-1)"
printf '%s\n' "$ie" | grep -q 'field foo_dead live=1' || fail "#else of FOO 0"
printf '%s\n' "$ie" | grep -q 'field n_ok live=1' || fail "#define N 2 then #if N == 2"
printf '%s\n' "$ie" | grep -q 'field foo_defined live=1' || fail "defined(FOO) after #define"
printf '%s\n' "$ie" | grep -q 'field bar_live live=0' || fail "undefined BAR is 0"
printf '%s\n' "$ie" | grep -q 'field after live=1' || fail "after should stay live"
ied="$("$BIN" --evaluate -DFOO=0 tests/cparse/if_expand.c)" \
    || fail "evaluate if_expand -DFOO=0"
printf '%s\n' "$ied" | grep -q 'field foo_live live=0' \
    || fail "-DFOO=0 must expand to 0"
oracle "" tests/cparse/if_expand.c
oracle "-DFOO=0" tests/cparse/if_expand.c

# --- full #if ICE + __has_* ---
fp="$("$BIN" --preserve tests/cparse/if_full.c)" || fail "preserve if_full"
printf '%s\n' "$fp" | grep -q '1 ? 1 : 0' || fail "preserve dropped ?:"
printf '%s\n' "$fp" | grep -q '__has_feature' || fail "preserve dropped __has_feature"
printf '%s\n' "$fp" | grep -q '__has_builtin' || fail "preserve dropped __has_builtin"
printf '%s\n' "$fp" | grep -q '__has_include' || fail "preserve dropped __has_include"

fe="$("$BIN" --evaluate tests/cparse/if_full.c)" || fail "evaluate if_full"
printf '%s\n' "$fe" | grep -q 'field ternary_live live=1' || fail "?: then arm"
printf '%s\n' "$fe" | grep -q 'field ternary_else live=1' || fail "?: else arm (skipped 1/0)"
printf '%s\n' "$fe" | grep -q 'field arith_live live=1' || fail "arith / shift / bitwise"
printf '%s\n' "$fe" | grep -q 'field tsan live=0' || fail "tsan dead without -D"
printf '%s\n' "$fe" | grep -q 'field no_tsan live=1' || fail "no_tsan live without -D"
printf '%s\n' "$fe" | grep -q 'field has_addovf live=1' || fail "__has_builtin add_overflow"
printf '%s\n' "$fe" | grep -q 'field has_quote live=1' || fail "__has_include quote"
printf '%s\n' "$fe" | grep -q 'field missing live=0' || fail "missing include is 0"
printf '%s\n' "$fe" | grep -q 'field after live=1' || fail "after should stay live"

fe1="$("$BIN" --evaluate -D__SANITIZE_THREAD__ tests/cparse/if_full.c)" \
    || fail "evaluate if_full tsan"
printf '%s\n' "$fe1" | grep -q 'field tsan live=1' || fail "tsan live with __SANITIZE_THREAD__"
printf '%s\n' "$fe1" | grep -q 'field no_tsan live=0' || fail "no_tsan dead with tsan"

oracle "" tests/cparse/if_full.c

unk="$(mktemp)"
cat >"$unk" <<'EOF'
typedef struct Unk {
#if not_a_macro(1)
    int x;
#endif
} Unk;
EOF
if "$BIN" --expand "$unk" >/dev/null 2>&1; then
    rm -f "$unk"
    fail "unknown call in #if expand must fail loud"
fi
rm -f "$unk"

# --- header unit: function-like #define, #undef, #pragma, simple typedef ---
hp="$("$BIN" --preserve tests/cparse/header_unit.h)" || fail "preserve header_unit"
printf '%s\n' "$hp" | grep -q '#pragma once' || fail "preserve dropped #pragma once"
printf '%s\n' "$hp" | grep -q '#define ID(x) x' || fail "preserve dropped function-like #define"
printf '%s\n' "$hp" | grep -q '#undef Z' || fail "preserve dropped #undef"
printf '%s\n' "$hp" | grep -q 'typedef int dead_z' || fail "preserve dropped dead typedef"
printf '%s\n' "$hp" | grep -q 'typedef int live_z' || fail "preserve dropped live typedef"
printf '%s\n' "$hp" | grep -q 'header_unit_id' || fail "preserve dropped header_unit_id"

he="$("$BIN" --evaluate tests/cparse/header_unit.h)" || fail "evaluate header_unit"
printf '%s\n' "$he" | grep -q 'dir pragma live=1' || fail "pragma should be live"
printf '%s\n' "$he" | grep -q 'define ID live=1' || fail "function-like ID should be live"
printf '%s\n' "$he" | grep -q 'typedef dead_z live=0' || fail "ID(Z) with Z=0: dead_z still in tree"
printf '%s\n' "$he" | grep -q 'typedef live_z live=1' || fail "ID(Z) with Z=0: live_z live"
printf '%s\n' "$he" | grep -q 'dir undef Z live=1' || fail "undef Z should be live"
printf '%s\n' "$he" | grep -q 'typedef unit_flag live=1' || fail "Z after #undef/#define 1"
printf '%s\n' "$he" | grep -q 'func header_unit_id live=1' || fail "header_unit_id should be live"
printf '%s\n' "$hp" | grep -q 'typedef struct UnitTag UnitTag' \
    || fail "preserve dropped incomplete typedef struct"
printf '%s\n' "$hp" | grep -q 'int header_unit_proto(int x);' \
    || fail "preserve dropped function prototype"
printf '%s\n' "$he" | grep -q 'typedef UnitTag live=1' || fail "incomplete typedef should be live"
printf '%s\n' "$he" | grep -q 'func header_unit_proto live=1' || fail "prototype should be live"
printf '%s\n' "$hp" | grep -q 'struct UnitFwd;' || fail "preserve dropped struct forward"
printf '%s\n' "$hp" | grep -q 'struct UnitRec' || fail "preserve dropped tagged struct"
printf '%s\n' "$hp" | grep -q 'rec_x' || fail "preserve dropped tagged struct field"
printf '%s\n' "$he" | grep -q 'struct UnitFwd live=1' || fail "struct forward should be live"
printf '%s\n' "$he" | grep -q 'struct UnitRec live=1' || fail "tagged struct should be live"
printf '%s\n' "$hp" | grep -q 'extern "C"' || fail "preserve dropped extern \"C\""
printf '%s\n' "$he" | grep -q 'func header_unit_c live=1' || fail "header_unit_c should be live"
printf '%s\n' "$he" | grep -q 'dir extern "C" live=0' \
    || fail "extern \"C\" opener is in the dead __cplusplus arm"

oracle "" tests/cparse/header_unit.h

# Real in-tree header: parse + preserve + evaluate. Not an oracle — #include
# is pass-through here; host -E follows stddef/stdbool.
compat=cc/include/ccc/cc_compat.cch
cpcomp="$("$BIN" --preserve "$compat")" || fail "preserve cc_compat.cch"
printf '%s\n' "$cpcomp" | grep -q '#pragma once' || fail "cc_compat: dropped #pragma once"
printf '%s\n' "$cpcomp" | grep -q '#include <stddef.h>' || fail "cc_compat: dropped #include"
printf '%s\n' "$cpcomp" | grep -q '#define __has_include(x) 0' \
    || fail "cc_compat: dropped function-like __has_include stub"
printf '%s\n' "$cpcomp" | grep -q 'cc_static_assert' || fail "cc_compat: dropped cc_static_assert"
printf '%s\n' "$cpcomp" | grep -q 'typedef int bool' || fail "cc_compat: dropped typedef int bool"

cecomp="$("$BIN" --evaluate "$compat")" || fail "evaluate cc_compat.cch"
printf '%s\n' "$cecomp" | grep -q 'define CC_COMPAT_H live=1' \
    || fail "cc_compat: include-guard #define should be live"
printf '%s\n' "$cecomp" | grep -q 'dir include live=1' || fail "cc_compat: #include should be live"
printf '%s\n' "$cecomp" | grep -q 'define cc_static_assert live=1' \
    || fail "cc_compat: cc_static_assert should be live"
printf '%s\n' "$cecomp" | grep -q 'define __has_include live=0' \
    || fail "cc_compat: #ifndef __has_include stub must be dead (engine builtin)"
printf '%s\n' "$cecomp" | grep -q 'typedef bool live=0' \
    || fail "cc_compat: typedef bool is in the dead #else of __has_include"

atomic=cc/include/ccc/cc_atomic.cch
ap="$("$BIN" --preserve "$atomic")" || fail "preserve cc_atomic.cch"
printf '%s\n' "$ap" | grep -q 'cc_atomic_fetch_add' || fail "cc_atomic: dropped fetch_add"
printf '%s\n' "$ap" | grep -q 'typedef _Atomic int cc_atomic_int' \
    || fail "cc_atomic: dropped C11 typedef"
ae="$("$BIN" --evaluate "$atomic")" || fail "evaluate cc_atomic.cch"
printf '%s\n' "$ae" | grep -q 'typedef cc_atomic_int live=1' \
    || fail "cc_atomic: C11 typedef should be live"
printf '%s\n' "$ae" | grep -q 'define CC_ATOMIC_HAVE_REAL_ATOMICS live=1' \
    || fail "cc_atomic: HAVE_REAL_ATOMICS should be live on this host"
printf '%s\n' "$ae" | grep -q 'dir warning live=0' \
    || fail "cc_atomic: unknown-compiler #warning stays dead in the tree"

runtime=cc/include/ccc/cc_runtime.cch
rp="$("$BIN" --preserve "$runtime")" || fail "preserve cc_runtime.cch"
printf '%s\n' "$rp" | grep -q 'CCChan\* channel_pair' || fail "cc_runtime: dropped prototype"
printf '%s\n' "$rp" | grep -q 'typedef struct CCChan CCChan' \
    || fail "cc_runtime: dropped incomplete typedef"
re="$("$BIN" --evaluate "$runtime")" || fail "evaluate cc_runtime.cch"
printf '%s\n' "$re" | grep -q 'func channel_pair live=1' || fail "cc_runtime: channel_pair live"
printf '%s\n' "$re" | grep -q 'func cc_deadlock_suppress_enter live=1' \
    || fail "cc_runtime: deadlock proto live"
printf '%s\n' "$re" | grep -q 'typedef CCChan live=1' || fail "cc_runtime: CCChan typedef live"
printf '%s\n' "$re" | grep -q 'define cc_move live=1' || fail "cc_runtime: cc_move else-arm live"

chanh=cc/include/ccc/cc_chan_handle.cch
chp="$("$BIN" --preserve "$chanh")" || fail "preserve cc_chan_handle.cch"
printf '%s\n' "$chp" | grep -q 'struct CCChan;' || fail "cc_chan_handle: dropped struct forward"
printf '%s\n' "$chp" | grep -q 'CCChanTx' || fail "cc_chan_handle: dropped CCChanTx"
che="$("$BIN" --evaluate "$chanh")" || fail "evaluate cc_chan_handle.cch"
printf '%s\n' "$che" | grep -q 'struct CCChan live=1' || fail "cc_chan_handle: CCChan forward live"
printf '%s\n' "$che" | grep -q 'struct CCChanTx live=1' || fail "cc_chan_handle: CCChanTx live"

bh=cc/include/ccc/cc_build_helpers.cch
bhp="$("$BIN" --preserve "$bh")" || fail "preserve cc_build_helpers.cch"
printf '%s\n' "$bhp" | grep -q 'extern "C"' || fail "cc_build_helpers: dropped extern \"C\""
printf '%s\n' "$bhp" | grep -q 'cc_build_join_paths' || fail "cc_build_helpers: dropped join_paths"
bhe="$("$BIN" --evaluate "$bh")" || fail "evaluate cc_build_helpers.cch"
printf '%s\n' "$bhe" | grep -q 'func cc_build_join_paths live=1' \
    || fail "cc_build_helpers: join_paths live"
printf '%s\n' "$bhe" | grep -q 'dir extern "C" live=0' \
    || fail "cc_build_helpers: extern \"C\" opener dead without __cplusplus"
printf '%s\n' "$bhe" | grep -q 'dir extern } live=0' \
    || fail "cc_build_helpers: linkage close stays in the dead arm"

# --- overlay flatten of the struct-field span (same API shadow calls) ---
fl="$("$BIN" --fields tests/cparse/process_fields.c)" || fail "fields process_fields"
printf '%s\n' "$fl" | grep -q 'ppdir.*#ifdef _WIN32' || fail "fields missing #ifdef"
printf '%s\n' "$fl" | grep -q 'field handle' || fail "fields missing handle"
printf '%s\n' "$fl" | grep -q 'ppdir.*#else' || fail "fields missing #else"
printf '%s\n' "$fl" | grep -q 'field posix_pid' || fail "fields missing posix_pid"
printf '%s\n' "$fl" | grep -q 'ppdir.*#endif' || fail "fields missing #endif"
printf '%s\n' "$fl" | grep -q 'field stdin_fd' || fail "fields missing stdin_fd"

if [ -x out/cc/bin/shadow_lower ] &&
   nm -g out/cc/bin/shadow_lower 2>/dev/null | grep -q cparse_flat_fields; then
    sh scripts/test_cparse_overlay.sh || fail "overlay"
fi

echo "[test_cparse] ok"
