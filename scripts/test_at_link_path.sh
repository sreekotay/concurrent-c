#!/bin/sh
# @link path vs short name. shadow_add_lib_flag must match host
# cc__is_lib_path: slash or .a/.so/.dylib is a path (no -l prefix).
# ccc --dry-run returns before the host link, so inspect --verbose link lines.
set -eu
ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT_DIR"
CCC=./cc/bin/ccc

fail() { echo "[test_at_link_path] FAIL: $1" >&2; exit 1; }

[ -x "$CCC" ] || fail "missing $CCC"

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

link_line() {
    # Last `cc: cc …` line is the host link command.
    printf '%s\n' "$1" | grep '^cc: cc ' | tail -1
}

# Short name → -lcurl (link may fail if curl is absent; the flag must still land).
printf '%s\n' '@link("curl")' 'int main(void) { return 0; }' > "$work/name.ccs"
name_err="$("$CCC" --verbose --no-cache --out-dir "$work/out" --bin-dir "$work/bin" \
    build --link "$work/name.ccs" -o "$work/bin/name" 2>&1)" || true
name_ln="$(link_line "$name_err")"
[ -n "$name_ln" ] || fail "name form: no host link line: $name_err"
printf '%s\n' "$name_ln" | grep -F -q -- '-lcurl' \
    || fail "name form missing -lcurl: $name_ln"

# Relative archive path must not become -lpath/to/lib.a.
printf '%s\n' '@link("../../third_party/bearssl/build/libbearssl.a")' \
    'int main(void) { return 0; }' > "$work/rel.ccs"
rel_err="$("$CCC" --verbose --no-cache --out-dir "$work/out" --bin-dir "$work/bin" \
    build --link "$work/rel.ccs" -o "$work/bin/rel" 2>&1)" || true
rel_ln="$(link_line "$rel_err")"
[ -n "$rel_ln" ] || fail "rel path: no host link line: $rel_err"
printf '%s\n' "$rel_ln" | grep -F -q -- '-l../../third_party/bearssl/build/libbearssl.a' \
    && fail "rel path became -lPATH: $rel_ln"
printf '%s\n' "$rel_ln" | grep -F -q -- '../../third_party/bearssl/build/libbearssl.a' \
    || fail "rel path missing on link line: $rel_ln"

# Suffix-only archive name (no slash).
printf '%s\n' '@link("libghost.a")' 'int main(void) { return 0; }' > "$work/suf.ccs"
suf_err="$("$CCC" --verbose --no-cache --out-dir "$work/out" --bin-dir "$work/bin" \
    build --link "$work/suf.ccs" -o "$work/bin/suf" 2>&1)" || true
suf_ln="$(link_line "$suf_err")"
[ -n "$suf_ln" ] || fail "suffix .a: no host link line: $suf_err"
printf '%s\n' "$suf_ln" | grep -F -q -- '-llibghost.a' \
    && fail "suffix .a became -llibghost.a: $suf_ln"
printf '%s\n' "$suf_ln" | grep -F -q -- 'libghost.a' \
    || fail "suffix .a missing on link line: $suf_ln"

# End-to-end: a real archive on the link line, then run.
printf '%s\n' 'int at_link_path_answer(void) { return 42; }' > "$work/ans.c"
cc -c -o "$work/ans.o" "$work/ans.c" || fail "host cc -c ans.c"
ar rcs "$work/libatlinkpath.a" "$work/ans.o" || fail "ar libatlinkpath.a"
printf '%s\n' \
    "@link(\"$work/libatlinkpath.a\")" \
    "int at_link_path_answer(void);" \
    "int main(void) {" \
    "    if (at_link_path_answer() != 42) return 1;" \
    "    return 0;" \
    "}" > "$work/e2e.ccs"
e2e_err="$("$CCC" --verbose --no-cache --out-dir "$work/out" --bin-dir "$work/bin" \
    build --link "$work/e2e.ccs" -o "$work/bin/e2e" 2>&1)" \
    || fail "e2e link failed: $e2e_err"
e2e_ln="$(link_line "$e2e_err")"
printf '%s\n' "$e2e_ln" | grep -F -q -- "-l$work/libatlinkpath.a" \
    && fail "e2e path became -lPATH: $e2e_ln"
"$work/bin/e2e" || fail "e2e binary exited nonzero"

# Two TUs: @link on the first file must reach cc__link_many (not only single-TU).
printf '%s\n' 'int two_tu_marker(void) { return 1; }' > "$work/e2e_b.ccs"
printf '%s\n' \
    "@link(\"$work/libatlinkpath.a\")" \
    "int at_link_path_answer(void);" \
    "int two_tu_marker(void);" \
    "int main(void) {" \
    "    if (at_link_path_answer() != 42) return 1;" \
    "    if (two_tu_marker() != 1) return 1;" \
    "    return 0;" \
    "}" > "$work/e2e_a.ccs"
two_err="$("$CCC" --verbose --no-cache --out-dir "$work/out" --bin-dir "$work/bin" \
    -o "$work/bin/e2e2" "$work/e2e_a.ccs" "$work/e2e_b.ccs" 2>&1)" \
    || fail "two-tu link failed: $two_err"
printf '%s\n' "$two_err" | grep -F -q -- "-l$work/libatlinkpath.a" \
    && fail "two-tu path became -lPATH: $two_err"
"$work/bin/e2e2" || fail "two-tu binary exited nonzero"

# A relative path resolves beside the file that wrote it, not the cwd:
# a face in lib/ names ../lib/libatlinkpath.a, the program in app/ includes
# the face, and the build runs from a third directory.
mkdir -p "$work/face" "$work/app" "$work/elsewhere"
cp "$work/libatlinkpath.a" "$work/face/libatlinkpath.a"
printf '%s\n' \
    '#ifndef AT_LINK_FACE_H' '#define AT_LINK_FACE_H' \
    '@link("../face/libatlinkpath.a")' \
    'int at_link_path_answer(void);' \
    '#endif' > "$work/face/at_link_face.cch"
printf '%s\n' \
    '#include "../face/at_link_face.cch"' \
    'int main(void) { return at_link_path_answer() == 42 ? 0 : 1; }' > "$work/app/rel_face.ccs"
face_err="$(cd "$work/elsewhere" && "$ROOT_DIR/cc/bin/ccc" --verbose --no-cache \
    --out-dir "$work/out" --bin-dir "$work/bin" \
    build --link "$work/app/rel_face.ccs" -o "$work/bin/rel_face" 2>&1)" \
    || fail "face-relative link failed: $face_err"
face_ln="$(link_line "$face_err")"
printf '%s\n' "$face_ln" | grep -F -q -- "$work/face/../face/libatlinkpath.a" \
    || fail "face-relative path not resolved beside the face: $face_ln"
"$work/bin/rel_face" || fail "face-relative binary exited nonzero"

# The stdlib header tool writes the marker with the path resolved beside the
# face, so a `<ccc/...>` face can state its own library.
LH="$ROOT_DIR/out/cc/bin/lower_headers"
if [ -x "$LH" ]; then
    mkdir -p "$work/lowered"
    "$LH" --ordered "$work/lowered" "$work/face/at_link_face.cch" >/dev/null 2>&1 \
        || fail "lower_headers refused the face"
    lowered_h="$(find "$work/lowered" -name 'at_link_face.h' | head -1)"
    [ -n "$lowered_h" ] || fail "lower_headers wrote no at_link_face.h"
    grep -F -q -- "__CC_LINK__ $work/face/../face/libatlinkpath.a" "$lowered_h" \
        || fail "lowered face lacks the resolved marker: $(grep -n LINK "$lowered_h")"
    grep -F -q -- '@link(' "$lowered_h" && fail "lowered face still carries raw @link"
fi

echo "[test_at_link_path] ok"
