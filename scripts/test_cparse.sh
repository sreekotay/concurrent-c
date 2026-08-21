#!/bin/sh
# Step-1 cparse: preserve reprints both #if arms; evaluate marks live/dead
# without dropping the dead arm from the tree.
set -eu
ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT_DIR"
BIN=out/cc/bin/cparse-dump

fail() { echo "[test_cparse] FAIL: $1" >&2; exit 1; }

# grep -q on a pipe exits after the first hit. printf is still dumping the
# haystack and gets EPIPE ("Broken pipe") — that is a match, not a failure.
hay="$(mktemp)"
trap 'rm -f "$hay"' EXIT
has() {
    printf '%s\n' "$1" >"$hay"
    grep -q "$2" "$hay"
}

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
trap 'rm -f "$hay" "$presf" "$ev0" "$ev1"' EXIT
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
has "$ap" '__attribute__((unused))' || fail "preserve missing __attribute__((unused))"
has "$ap" 'name(' || fail "preserve missing name("

ae="$("$BIN" --evaluate tests/cparse/attr_unused.c)" || fail "evaluate attr_unused"
has "$ae" 'func name' || fail "evaluate missing func name"
has "$ae" '__attribute__((unused))' || fail "evaluate missing attr on func"
has "$ae" 'live=1' || fail "func should be live"

pf="$("$BIN" --preserve tests/cparse/plain_fn.c)" || fail "preserve plain_fn"
has "$pf" 'cparse_plain_add' || fail "preserve dropped cparse_plain_add"
has "$pf" 'cparse_plain_main' || fail "preserve dropped cparse_plain_main"
has "$pf" 'return x + y' || fail "preserve dropped add body"

# --- FileTape-shaped tokens: '#' is a punct, not a glued directive ---
toks="$("$BIN" --tokens tests/cparse/process_fields.c)" || fail "tokens process_fields"
has "$toks" '^PUNCT #$' || fail "expected PUNCT #"
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
has "$st" '\\\\' && fail "backslash survived splice"

sp="$("$BIN" --preserve tests/cparse/line_splice.c)" || fail "preserve line_splice"
has "$sp" '#ifdef _WIN32' || fail "spliced #ifdef _WIN32 missing in preserve"

# --- expand: macros, hide-set, # / ## / __VA_ARGS__, dead arm omitted ---
ex="$("$BIN" --expand tests/cparse/macros.c)" || fail "expand macros"
has "$ex" 'IDENT posix_pid' || fail "expand dropped else-arm posix_pid"
has "$ex" 'IDENT handle' && fail "expand kept dead _WIN32 arm"
has "$ex" 'IDENT posix' || fail "JOIN(pos,ix) did not paste to posix"
has "$ex" 'STR "hello"' || fail "STR(hello) did not stringify"
printf '%s\n' "$ex" | awk '
  $0=="IDENT f" { f=1; next }
  f && $0=="PUNCT (" { f=2; next }
  f==2 && $0=="NUM 1" { f=3; next }
  f==3 && $0=="PUNCT ," { f=4; next }
  f==4 && $0=="NUM 2" { found=1 }
  { f=0 }
  END { if (!found) exit 1 }
' || fail "WRAP(1, 2) did not expand to f(1, 2)"
has "$ex" 'IDENT E' || fail "hide-set #define E E should emit E"
has "$ex" 'IDENT once' || fail "include-guard body dropped"

exw="$("$BIN" --expand -D_WIN32 tests/cparse/macros.c)" || fail "expand -D_WIN32"
has "$exw" 'IDENT handle' || fail "-D_WIN32 should expand T handle"
has "$exw" 'IDENT posix_pid' && fail "-D_WIN32 kept posix_pid"

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
has "$ep" '#ifdef _WIN32' || fail "preserve elif dropped #ifdef"
has "$ep" '#elif defined(__linux__)' || fail "preserve dropped #elif"
has "$ep" 'linux_fd' || fail "preserve dropped linux_fd"
has "$ep" 'posix_pid' || fail "preserve dropped posix_pid"

ee0="$("$BIN" --evaluate tests/cparse/elif_fields.c)" || fail "evaluate elif (none)"
has "$ee0" 'field handle live=0' || fail "handle should be dead with no -D"
has "$ee0" 'field linux_fd live=0' || fail "linux_fd should be dead with no -D"
has "$ee0" 'field posix_pid live=1' || fail "posix_pid should be live with no -D"
has "$ee0" '#elif' || fail "evaluate dropped #elif"

ee1="$("$BIN" --evaluate -D_WIN32 tests/cparse/elif_fields.c)" || fail "evaluate elif -D_WIN32"
has "$ee1" 'field handle live=1' || fail "handle should be live with _WIN32"
has "$ee1" 'field linux_fd live=0' || fail "linux_fd should be dead with _WIN32"
has "$ee1" 'field posix_pid live=0' || fail "posix_pid should be dead with _WIN32"

ee2="$("$BIN" --evaluate -D__linux__ tests/cparse/elif_fields.c)" || fail "evaluate elif -D__linux__"
has "$ee2" 'field handle live=0' || fail "handle should be dead with __linux__"
has "$ee2" 'field linux_fd live=1' || fail "linux_fd should be live with __linux__"
has "$ee2" 'field posix_pid live=0' || fail "posix_pid should be dead with __linux__"

elf="$("$BIN" --fields tests/cparse/elif_fields.c)" || fail "fields elif_fields"
has "$elf" 'ppdir.*#elif' || fail "fields missing #elif"
has "$elf" 'field linux_fd' || fail "fields missing linux_fd"

uf="$("$BIN" --preserve tests/cparse/union_field.c)" || fail "preserve union_field"
has "$uf" 'union' || fail "preserve dropped union"
has "$uf" 'bytes' || fail "preserve dropped union member bytes"
has "$uf" 'extra' || fail "preserve dropped extra after union"
uff="$("$BIN" --fields tests/cparse/union_field.c)" || fail "fields union_field"
has "$uff" 'union' || fail "fields missing union declarator"
has "$uff" 'field fat' || fail "fields missing fat (must not chop >512)"
has "$uff" 'field extra' || fail "fields missing extra"

cpf="$("$BIN" --fields tests/cparse/comma_ptrs.c)" || fail "fields comma_ptrs"
has "$cpf" 'field fa' || fail "fields missing fa"
has "$cpf" 'field fb' || fail "fields missing fb"
has "$cpf" 'int \*fb' || fail "fields dropped * on later comma name fb"
has "$cpf" 'field s' || fail "fields missing s"
has "$cpf" 'const char \*e' || fail "fields dropped * on later comma name e"
has "$cpf" 'field p' || fail "fields missing p"
has "$cpf" 'field q' || fail "fields missing q"

# --- compound #if / #elif: || && ! == ---
cp="$("$BIN" --preserve tests/cparse/if_compound.c)" || fail "preserve if_compound"
has "$cp" 'defined(_WIN32) || defined(_WIN64)' \
    || fail "preserve dropped ||"
has "$cp" 'defined(__linux__) && !defined(__ANDROID__)' \
    || fail "preserve dropped &&"
has "$cp" 'UINTPTR_MAX == 0xFFFFFFFF' \
    || fail "preserve dropped =="
has "$cp" 'posix_pid' || fail "preserve dropped posix_pid"

ce0="$("$BIN" --evaluate tests/cparse/if_compound.c)" || fail "evaluate if_compound"
has "$ce0" 'field handle live=0' || fail "handle dead with no -D"
has "$ce0" 'field linux_fd live=0' || fail "linux_fd dead with no -D"
has "$ce0" 'field ilp32 live=0' || fail "ilp32 dead (0==0xFFFFFFFF is false)"
has "$ce0" 'field posix_pid live=1' || fail "posix_pid live with no -D"

ce1="$("$BIN" --evaluate -D_WIN32 tests/cparse/if_compound.c)" \
    || fail "evaluate if_compound -D_WIN32"
has "$ce1" 'field handle live=1' || fail "handle live with _WIN32"
has "$ce1" 'field linux_fd live=0' || fail "linux_fd dead with _WIN32"
has "$ce1" 'field posix_pid live=0' || fail "posix_pid dead with _WIN32"

ce2="$("$BIN" --evaluate -D__linux__ tests/cparse/if_compound.c)" \
    || fail "evaluate if_compound -D__linux__"
has "$ce2" 'field handle live=0' || fail "handle dead with __linux__"
has "$ce2" 'field linux_fd live=1' || fail "linux_fd live with __linux__"
has "$ce2" 'field posix_pid live=0' || fail "posix_pid dead with __linux__"

cf="$("$BIN" --fields tests/cparse/if_compound.c)" || fail "fields if_compound"
has "$cf" '||' || fail "fields missing ||"
has "$cf" 'field linux_fd' || fail "fields missing linux_fd"

oracle "" tests/cparse/if_compound.c
oracle "-D_WIN32" tests/cparse/if_compound.c
oracle "-D__linux__" tests/cparse/if_compound.c

# --- evaluate expands macros in #if (same algorithm as --expand) ---
iep="$("$BIN" --preserve tests/cparse/if_expand.c)" || fail "preserve if_expand"
has "$iep" '#if FOO' || fail "preserve dropped #if FOO"
ie="$("$BIN" --evaluate tests/cparse/if_expand.c)" || fail "evaluate if_expand"
has "$ie" 'field foo_live live=0' \
    || fail "#define FOO 0 then #if FOO must be false (not defined-or-1)"
has "$ie" 'field foo_dead live=1' || fail "#else of FOO 0"
has "$ie" 'field n_ok live=1' || fail "#define N 2 then #if N == 2"
has "$ie" 'field foo_defined live=1' || fail "defined(FOO) after #define"
has "$ie" 'field bar_live live=0' || fail "undefined BAR is 0"
has "$ie" 'field after live=1' || fail "after should stay live"
ied="$("$BIN" --evaluate -DFOO=0 tests/cparse/if_expand.c)" \
    || fail "evaluate if_expand -DFOO=0"
has "$ied" 'field foo_live live=0' \
    || fail "-DFOO=0 must expand to 0"
oracle "" tests/cparse/if_expand.c
oracle "-DFOO=0" tests/cparse/if_expand.c

# --- full #if ICE + __has_* ---
fp="$("$BIN" --preserve tests/cparse/if_full.c)" || fail "preserve if_full"
has "$fp" '1 ? 1 : 0' || fail "preserve dropped ?:"
has "$fp" '__has_feature' || fail "preserve dropped __has_feature"
has "$fp" '__has_builtin' || fail "preserve dropped __has_builtin"
has "$fp" '__has_include' || fail "preserve dropped __has_include"

fe="$("$BIN" --evaluate tests/cparse/if_full.c)" || fail "evaluate if_full"
has "$fe" 'field ternary_live live=1' || fail "?: then arm"
has "$fe" 'field ternary_else live=1' || fail "?: else arm (skipped 1/0)"
has "$fe" 'field arith_live live=1' || fail "arith / shift / bitwise"
has "$fe" 'field tsan live=0' || fail "tsan dead without -D"
has "$fe" 'field no_tsan live=1' || fail "no_tsan live without -D"
has "$fe" 'field has_addovf live=1' || fail "__has_builtin add_overflow"
has "$fe" 'field has_quote live=1' || fail "__has_include quote"
has "$fe" 'field missing live=0' || fail "missing include is 0"
has "$fe" 'field after live=1' || fail "after should stay live"

fe1="$("$BIN" --evaluate -D__SANITIZE_THREAD__ tests/cparse/if_full.c)" \
    || fail "evaluate if_full tsan"
has "$fe1" 'field tsan live=1' || fail "tsan live with __SANITIZE_THREAD__"
has "$fe1" 'field no_tsan live=0' || fail "no_tsan dead with tsan"

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
has "$hp" '#pragma once' || fail "preserve dropped #pragma once"
has "$hp" '#define ID(x) x' || fail "preserve dropped function-like #define"
has "$hp" '#undef Z' || fail "preserve dropped #undef"
has "$hp" 'typedef int dead_z' || fail "preserve dropped dead typedef"
has "$hp" 'typedef int live_z' || fail "preserve dropped live typedef"
has "$hp" 'header_unit_id' || fail "preserve dropped header_unit_id"

he="$("$BIN" --evaluate tests/cparse/header_unit.h)" || fail "evaluate header_unit"
has "$he" 'dir pragma live=1' || fail "pragma should be live"
has "$he" 'define ID live=1' || fail "function-like ID should be live"
has "$he" 'typedef dead_z live=0' || fail "ID(Z) with Z=0: dead_z still in tree"
has "$he" 'typedef live_z live=1' || fail "ID(Z) with Z=0: live_z live"
has "$he" 'dir undef Z live=1' || fail "undef Z should be live"
has "$he" 'typedef unit_flag live=1' || fail "Z after #undef/#define 1"
has "$he" 'func header_unit_id live=1' || fail "header_unit_id should be live"
has "$hp" 'typedef struct UnitTag UnitTag' \
    || fail "preserve dropped incomplete typedef struct"
has "$hp" 'int header_unit_proto(int x);' \
    || fail "preserve dropped function prototype"
has "$he" 'typedef UnitTag live=1' || fail "incomplete typedef should be live"
has "$he" 'func header_unit_proto live=1' || fail "prototype should be live"
has "$hp" 'struct UnitFwd;' || fail "preserve dropped struct forward"
has "$hp" 'struct UnitRec' || fail "preserve dropped tagged struct"
has "$hp" 'rec_x' || fail "preserve dropped tagged struct field"
has "$he" 'struct UnitFwd live=1' || fail "struct forward should be live"
has "$he" 'struct UnitRec live=1' || fail "tagged struct should be live"
has "$hp" 'extern "C"' || fail "preserve dropped extern \"C\""
has "$he" 'func header_unit_c live=1' || fail "header_unit_c should be live"
has "$he" 'data cxx live=0' \
    || fail "extern \"C\" opener is in the dead __cplusplus arm"
has "$hp" 'enum { UNIT_LEAF' || fail "preserve dropped anonymous enum"
has "$hp" 'enum UnitKind' || fail "preserve dropped tagged enum"
has "$he" 'enum UNIT_LEAF live=1' || fail "anonymous enum should be live"
has "$he" 'enum UnitKind live=1' || fail "tagged enum should be live"
has "$hp" 'unit_tab\[4\]' || fail "preserve dropped file-scope array"
has "$hp" 'extern int unit_ext' || fail "preserve dropped extern object"
has "$he" 'data unit_tab live=1' || fail "file-scope array should be live"
has "$he" 'data unit_ext live=1' || fail "extern object should be live"
has "$hp" 'alignas(16) char unit_pad' || fail "preserve dropped alignas field"
has "$hp" 'template<typename ty>' || fail "preserve dropped C++ template arm"
has "$he" 'struct UnitAlign live=1' || fail "alignas struct should be live"
has "$hp" 'const UnitTag \*unit_ptr' || fail "preserve dropped const typedef field"
has "$hp" 'size_t a, b, c' || fail "preserve dropped comma field names"
has "$he" 'field a live=1' || fail "comma field a should be live"
has "$he" 'field b live=1' || fail "comma field b should be live"
has "$he" 'field c live=1' || fail "comma field c should be live"
has "$hp" 'union { int unit_u_a' || fail "preserve dropped nested union"
has "$he" 'field unit_u live=1' || fail "nested union field should be live"
has "$he" 'struct UnitNest live=1' || fail "nested union struct should be live"
has "$he" 'stmt return live=1' || fail "header_unit_id should own a return stmt"
has "$he" 'struct UnitPtr live=1' || fail "const typedef field struct should be live"
has "$hp" 'CCJ_ALIGNAS UnitTag unit_align_fld' \
    || fail "preserve dropped prefix-macro field"
has "$he" 'struct UnitAlignPrefix live=1' \
    || fail "prefix-macro field struct should be live"
has "$hp" '!>(int) unit_bang' || fail "preserve dropped !>(int) proto"
has "$he" 'func unit_bang live=1' || fail "!> proto should be live"
has "$hp" '@typeview on UnitRec' || fail "preserve dropped @typeview"
has "$he" 'data typeview live=1' || fail "@typeview should be live"
has "$hp" 'UNIT_DECL(unit_decl_t)' \
    || fail "preserve dropped semicolon-less macro invoke"
has "$he" 'data UNIT_DECL live=1' \
    || fail "semicolon-less macro invoke should be live"
has "$hp" '#if __cplusplus >= 202101L' \
    || fail "preserve dropped mid-expression #if in C++ arm"
has "$he" 'data cxx live=0' \
    || fail "C++ arm is a dead opaque span (__cplusplus is 0)"

oracle "" tests/cparse/header_unit.h

# Real in-tree header: parse + preserve + evaluate. Not an oracle — #include
# is pass-through here; host -E follows stddef/stdbool.
compat=cc/include/ccc/cc_compat.cch
cpcomp="$("$BIN" --preserve "$compat")" || fail "preserve cc_compat.cch"
has "$cpcomp" '#pragma once' || fail "cc_compat: dropped #pragma once"
has "$cpcomp" '#include <stddef.h>' || fail "cc_compat: dropped #include"
has "$cpcomp" '#define __has_include(x) 0' \
    || fail "cc_compat: dropped function-like __has_include stub"
has "$cpcomp" 'cc_static_assert' || fail "cc_compat: dropped cc_static_assert"
has "$cpcomp" 'typedef int bool' || fail "cc_compat: dropped typedef int bool"

cecomp="$("$BIN" --evaluate "$compat")" || fail "evaluate cc_compat.cch"
has "$cecomp" 'define CC_COMPAT_H live=1' \
    || fail "cc_compat: include-guard #define should be live"
has "$cecomp" 'dir include live=1' || fail "cc_compat: #include should be live"
has "$cecomp" 'define cc_static_assert live=1' \
    || fail "cc_compat: cc_static_assert should be live"
has "$cecomp" 'define __has_include live=0' \
    || fail "cc_compat: #ifndef __has_include stub must be dead (engine builtin)"
has "$cecomp" 'typedef bool live=0' \
    || fail "cc_compat: typedef bool is in the dead #else of __has_include"

atomic=cc/include/ccc/cc_atomic.cch
ap="$("$BIN" --preserve "$atomic")" || fail "preserve cc_atomic.cch"
has "$ap" 'cc_atomic_fetch_add' || fail "cc_atomic: dropped fetch_add"
has "$ap" 'typedef _Atomic int cc_atomic_int' \
    || fail "cc_atomic: dropped C11 typedef"
ae="$("$BIN" --evaluate "$atomic")" || fail "evaluate cc_atomic.cch"
has "$ae" 'typedef cc_atomic_int live=1' \
    || fail "cc_atomic: C11 typedef should be live"
has "$ae" 'define CC_ATOMIC_HAVE_REAL_ATOMICS live=1' \
    || fail "cc_atomic: HAVE_REAL_ATOMICS should be live on this host"
has "$ae" 'dir warning live=0' \
    || fail "cc_atomic: unknown-compiler #warning stays dead in the tree"

runtime=cc/include/ccc/cc_runtime.cch
rp="$("$BIN" --preserve "$runtime")" || fail "preserve cc_runtime.cch"
has "$rp" 'CCChan\* channel_pair' || fail "cc_runtime: dropped prototype"
has "$rp" 'typedef struct CCChan CCChan' \
    || fail "cc_runtime: dropped incomplete typedef"
re="$("$BIN" --evaluate "$runtime")" || fail "evaluate cc_runtime.cch"
has "$re" 'func channel_pair live=1' || fail "cc_runtime: channel_pair live"
has "$re" 'func cc_deadlock_suppress_enter live=1' \
    || fail "cc_runtime: deadlock proto live"
has "$re" 'typedef CCChan live=1' || fail "cc_runtime: CCChan typedef live"
has "$re" 'define cc_move live=1' || fail "cc_runtime: cc_move else-arm live"

chanh=cc/include/ccc/cc_chan_handle.cch
chp="$("$BIN" --preserve "$chanh")" || fail "preserve cc_chan_handle.cch"
has "$chp" 'struct CCChan;' || fail "cc_chan_handle: dropped struct forward"
has "$chp" 'CCChanTx' || fail "cc_chan_handle: dropped CCChanTx"
che="$("$BIN" --evaluate "$chanh")" || fail "evaluate cc_chan_handle.cch"
has "$che" 'struct CCChan live=1' || fail "cc_chan_handle: CCChan forward live"
has "$che" 'struct CCChanTx live=1' || fail "cc_chan_handle: CCChanTx live"

bh=cc/include/ccc/cc_build_helpers.cch
bhp="$("$BIN" --preserve "$bh")" || fail "preserve cc_build_helpers.cch"
has "$bhp" 'extern "C"' || fail "cc_build_helpers: dropped extern \"C\""
has "$bhp" 'cc_build_join_paths' || fail "cc_build_helpers: dropped join_paths"
bhe="$("$BIN" --evaluate "$bh")" || fail "evaluate cc_build_helpers.cch"
has "$bhe" 'func cc_build_join_paths live=1' \
    || fail "cc_build_helpers: join_paths live"
has "$bhe" 'data cxx live=0' \
    || fail "cc_build_helpers: __cplusplus arm is a dead opaque span"

shape=cc/include/ccc/cc_shape.cch
shp="$("$BIN" --preserve "$shape")" || fail "preserve cc_shape.cch"
has "$shp" 'enum { CC_SHAPE_LEAF' || fail "cc_shape: dropped enum"
has "$shp" 'cc_shape_get' || fail "cc_shape: dropped cc_shape_get"
she="$("$BIN" --evaluate "$shape")" || fail "evaluate cc_shape.cch"
has "$she" 'enum CC_SHAPE_LEAF live=1' \
    || fail "cc_shape: anonymous enum should be live"
has "$she" 'struct CCShapeVal live=1' \
    || fail "cc_shape: CCShapeVal should be live"
has "$she" 'func cc_shape_get live=1' \
    || fail "cc_shape: cc_shape_get should be live"

json=cc/include/ccc/std/json.cch
jp="$("$BIN" --preserve "$json")" || fail "preserve json.cch"
has "$jp" 'jesc_tab\[256\]' || fail "json: dropped jesc_tab"
je="$("$BIN" --evaluate "$json")" || fail "evaluate json.cch"
has "$je" 'data jesc_tab live=1' || fail "json: jesc_tab should be live"
has "$je" 'func jstr live=1' || fail "json: jstr should be live"

arena=cc/include/ccc/cc_arena.cch
arp="$("$BIN" --preserve "$arena")" || fail "preserve cc_arena.cch"
has "$arp" 'cc_arena_prov_counter' \
    || fail "cc_arena: dropped extern counter"
are="$("$BIN" --evaluate "$arena")" || fail "evaluate cc_arena.cch"
has "$are" 'data cc_arena_prov_counter live=1' \
    || fail "cc_arena: extern counter should be live"
has "$are" 'func cc__align_up live=1' \
    || fail "cc_arena: cc__align_up should be live"

ctr=cc/include/ccc/cc_containers.cch
ctp="$("$BIN" --preserve "$ctr")" || fail "preserve cc_containers.cch"
has "$ctp" 'CC_MAKE_LVAL_COPY' || fail "cc_containers: dropped LVAL_COPY"
has "$ctp" 'template<typename ty>' \
    || fail "cc_containers: dropped C++ template arm"
has "$ctp" '#if __cplusplus >= 202101L' \
    || fail "cc_containers: dropped mid-expression #if in C++ arm"
cte="$("$BIN" --evaluate "$ctr")" || fail "evaluate cc_containers.cch"
has "$cte" 'data cxx live=0' \
    || fail "cc_containers: __cplusplus arm should be dead"
has "$cte" 'define CC_VEC live=1' \
    || fail "cc_containers: CC_VEC should be live"

dyn=cc/include/ccc/cc_dyn_vec.cch
dyp="$("$BIN" --preserve "$dyn")" || fail "preserve cc_dyn_vec.cch"
has "$dyp" 'const cc_type_info' || fail "cc_dyn_vec: dropped const typedef field"
dye="$("$BIN" --evaluate "$dyn")" || fail "evaluate cc_dyn_vec.cch"
has "$dye" 'struct cc_dyn_vec live=1' || fail "cc_dyn_vec: struct should be live"

ioe=cc/include/ccc/cc_io_error.cch
iop="$("$BIN" --preserve "$ioe")" || fail "preserve cc_io_error.cch"
has "$iop" '@typeview on CCIoError' || fail "cc_io_error: dropped @typeview"
has "$iop" 'CC_DECL_RESULT_SPEC' || fail "cc_io_error: dropped CC_DECL"
ioee="$("$BIN" --evaluate "$ioe")" || fail "evaluate cc_io_error.cch"
has "$ioee" 'data typeview live=1' || fail "cc_io_error: @typeview should be live"

chan=cc/include/ccc/cc_channel.cch
cnp="$("$BIN" --preserve "$chan")" || fail "preserve cc_channel.cch"
has "$cnp" 'bool !>(CCIoError)' || fail "cc_channel: dropped !> proto"
cne="$("$BIN" --evaluate "$chan")" || fail "evaluate cc_channel.cch"
has "$cne" 'func cc_chan_result_from_errno live=1' \
    || fail "cc_channel: !> proto should be live"

smap=cc/include/ccc/std/static_map.cch
smp="$("$BIN" --preserve "$smap")" || fail "preserve static_map.cch"
has "$smp" '@comptime void static_map' \
    || fail "static_map: dropped @comptime"
sme="$("$BIN" --evaluate "$smap")" || fail "evaluate static_map.cch"
has "$sme" 'data comptime live=1' \
    || fail "static_map: @comptime should be live"

# Remaining stdlib .cch files must parse as units (preserve + evaluate).
for u in \
    cc/include/ccc/cc_arena_result.cch \
    cc/include/ccc/cc_exclusive_result.cch \
    cc/include/ccc/cc_grammar.cch \
    cc/include/ccc/cc_io_test.cch \
    cc/include/ccc/cc_nursery.cch \
    cc/include/ccc/cc_result.cch \
    cc/include/ccc/cc_select.cch \
    cc/include/ccc/cc_turnstile.cch \
    cc/include/ccc/cc_type.cch \
    cc/include/ccc/std/bufio.cch \
    cc/include/ccc/std/cli.cch \
    cc/include/ccc/std/dir.cch \
    cc/include/ccc/std/exec.cch \
    cc/include/ccc/std/http.cch \
    cc/include/ccc/std/io.cch \
    cc/include/ccc/std/mmap.cch \
    cc/include/ccc/std/net.cch \
    cc/include/ccc/std/process.cch \
    cc/include/ccc/std/slice.cch \
    cc/include/ccc/std/slice_packed.cch \
    cc/include/ccc/std/string.cch \
    cc/include/ccc/script/file.cch \
    cc/include/ccc/script/js.cch \
    cc/include/ccc/script/pathx.cch \
    cc/include/ccc/script/prelude.cch \
    cc/include/ccc/script/py.cch \
    cc/include/ccc/script/sh.cch \
    cc/include/ccc/script/stdio.cch \
    cc/include/ccc/script/temp.cch
do
    "$BIN" --preserve "$u" >/dev/null || fail "preserve $u"
    "$BIN" --evaluate "$u" >/dev/null || fail "evaluate $u"
done

# --- overlay flatten of the struct-field span (same API shadow calls) ---
fl="$("$BIN" --fields tests/cparse/process_fields.c)" || fail "fields process_fields"
has "$fl" 'ppdir.*#ifdef _WIN32' || fail "fields missing #ifdef"
has "$fl" 'field handle' || fail "fields missing handle"
has "$fl" 'ppdir.*#else' || fail "fields missing #else"
has "$fl" 'field posix_pid' || fail "fields missing posix_pid"
has "$fl" 'ppdir.*#endif' || fail "fields missing #endif"
has "$fl" 'field stdin_fd' || fail "fields missing stdin_fd"
has "$fl" 'field extra_a' || fail "fields missing extra_a"
has "$fl" 'field extra_b' || fail "fields missing extra_b"
has "$fl" 'field extra_c' || fail "fields missing extra_c"

if [ -x out/cc/bin/shadow_lower ] &&
   nm -g out/cc/bin/shadow_lower 2>/dev/null | grep -q cparse_flat_fields; then
    sh scripts/test_cparse_overlay.sh || fail "overlay"
fi

echo "[test_cparse] ok"
