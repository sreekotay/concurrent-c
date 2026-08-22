#!/usr/bin/env sh
set -euo pipefail

# Build-system / driver smoke tests.
# These are intentionally simple integration checks that run the built compiler
# against tiny inputs and validate behavior via stdout/text.

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CCC="$ROOT/cc/bin/ccc"

if [ ! -x "$CCC" ]; then
  echo "ccc not found at $CCC (run: make cc)" >&2
  exit 1
fi

tmpdir="$(mktemp -d)"
trap 'rm -rf "$tmpdir"' EXIT

echo "== build system smoke =="

###############################################################################
# 1) --dump-comptime prints merged consts + target metadata
###############################################################################
echo "-- dump comptime"
out="$("$CCC" build --build-file "$ROOT/examples/build_stub/build.cc" --dump-comptime --dry-run 2>&1 || true)"
printf "%s\n" "$out" | grep -F "COMPTIME consts" >/dev/null

###############################################################################
# 2) build graph can write to a file (--graph-out) in both json and dot modes
###############################################################################
echo "-- graph out (json)"
json_out="$tmpdir/graph.json"
"$CCC" build graph --build-file "$ROOT/examples/build_stub/build.cc" --graph-out "$json_out" --format json >/dev/null
test -s "$json_out"

echo "-- graph out (dot)"
dot_out="$tmpdir/graph.dot"
"$CCC" build graph --build-file "$ROOT/examples/build_stub/build.cc" --graph-out "$dot_out" --format dot >/dev/null
test -s "$dot_out"

###############################################################################
# 3) Multi-input stem disambiguation: two same-basename files should not collide
###############################################################################
echo "-- multi-input stem disambiguation"
mkdir -p "$tmpdir/a" "$tmpdir/b" "$tmpdir/c"
cat >"$tmpdir/a/test.ccs" <<'EOF'
int a_fn(void) { return 1; }
EOF
cat >"$tmpdir/b/test.ccs" <<'EOF'
int b_fn(void) { return 2; }
EOF
cat >"$tmpdir/c/other.ccs" <<'EOF'
int main(void) { return 0; }
EOF

out_dir="$tmpdir/out"
bin_out="$tmpdir/bin/test_multi"
mkdir -p "$out_dir" "$tmpdir/bin"
"$CCC" --out-dir "$out_dir" --emit-c-only "$tmpdir/a/test.ccs" "$tmpdir/b/test.ccs" "$tmpdir/c/other.ccs" >/dev/null

# Both generated C files should exist with different stems.
count_c=0
for f in "$out_dir"/*.c; do
  if [ -f "$f" ]; then
    count_c=$((count_c + 1))
  fi
done
test "$count_c" -ge 3

# Also ensure link works when explicitly naming output (no collision).
"$CCC" --out-dir "$out_dir" --bin-dir "$tmpdir/bin" "$tmpdir/a/test.ccs" "$tmpdir/b/test.ccs" "$tmpdir/c/other.ccs" -o "$bin_out" >/dev/null
test -x "$bin_out"

###############################################################################
# 4) --print-cflags and --print-libs
###############################################################################
echo "-- print cflags/libs"
"$CCC" --print-cflags | grep "\-I" >/dev/null
"$CCC" --print-libs | grep "\.c" >/dev/null

###############################################################################
# 5) ccc wrapper auto-re-lowers stale .cch headers on the next invocation.
#
# Regression: a .cch edit was silently ignored by downstream builds that
# called cc/bin/ccc directly because the headers-lowered stamp under
# out/include/ was only regenerated when someone remembered to run
# `make -C cc lower-headers`.  cc/bin/ccc is now a shell wrapper that
# checks .cch mtimes against the stamp and re-runs lower_headers on demand.
###############################################################################
echo "-- stale-header auto-lower"
STAMP="$ROOT/out/include/.headers_lowered.stamp"
LOWERED_ARENA="$ROOT/out/include/ccc/cc_arena.h"
CCH_ARENA="$ROOT/cc/include/ccc/cc_arena.cch"
if [ -f "$STAMP" ] && [ -f "$CCH_ARENA" ] && [ -f "$LOWERED_ARENA" ]; then
  # Age the stamp so the .cch is provably newer without modifying the
  # source file itself.
  touch -t 200001010000 "$STAMP"
  cp "$LOWERED_ARENA" "$tmpdir/cc_arena.h.before"

  # Use --print-cflags so ccc exits immediately; the wrapper check runs
  # regardless of the subcommand.
  CCC_LOWER_QUIET=1 "$CCC" --print-cflags >/dev/null

  # Stamp must have been refreshed past the 2000-01-01 age.
  stamp_epoch="$(stat -f %m "$STAMP" 2>/dev/null || stat -c %Y "$STAMP")"
  now_epoch="$(date +%s)"
  age=$((now_epoch - stamp_epoch))
  test "$age" -lt 120 || {
    echo "stamp not refreshed (age=${age}s)" >&2
    exit 1
  }

  # Subsequent invocation with no source change must NOT re-touch the stamp.
  fresh_stamp_epoch="$stamp_epoch"
  sleep 1
  CCC_LOWER_QUIET=1 "$CCC" --print-cflags >/dev/null
  stamp_epoch2="$(stat -f %m "$STAMP" 2>/dev/null || stat -c %Y "$STAMP")"
  test "$stamp_epoch2" = "$fresh_stamp_epoch" || {
    echo "stamp re-lowered on a clean run (before=$fresh_stamp_epoch after=$stamp_epoch2)" >&2
    exit 1
  }
else
  echo "(skipping: stamp or lowered header missing)" >&2
fi

###############################################################################
# 6) cccportable: install from resolved toolchain (cwd outside checkout)
###############################################################################
echo "-- cccportable"
foreign="$(mktemp -d /tmp/cccportable-smoke.XXXXXX)"
trap 'rm -rf "$tmpdir" "$foreign"' EXIT
(
  cd "$foreign"
  mkdir -p occupied && echo x >occupied/keep.txt
  if "$CCC" portable-install occupied; then
    echo "occupied unstamped DIR should fail" >&2
    exit 1
  fi
  "$CCC" portable-install vendor/cccportable
  test -f vendor/cccportable/CCCPORTABLE.txt
  test -f vendor/cccportable/include/ccc/cc_runtime.h
  test -f vendor/cccportable/runtime/concurrent_c.c
  if find vendor/cccportable -name '*.cch' | grep . >/dev/null; then
    echo "portable tree must not contain .cch" >&2
    exit 1
  fi
  "$CCC" portable-install vendor/cccportable
  cflags="$("$CCC" --cccportable vendor/cccportable --print-cflags)"
  printf '%s\n' "$cflags" | grep -F "vendor/cccportable/include" >/dev/null
  printf '%s\n' "$cflags" | grep -c -- '-I' | grep -qx 1
  libs="$("$CCC" --cccportable vendor/cccportable --print-libs)"
  printf '%s\n' "$libs" | grep -F -- "-DCC_ENABLE_ASYNC" >/dev/null
  printf '%s\n' "$libs" | grep -F -- "-lm" >/dev/null
  mkdir -p include/generated
  cat >emit_fail.ccs <<'EOF'
#!ccc ccs
int main(void) { return 0; }
EOF
  if "$CCC" --cccportable vendor/cccportable --emit-c-only emit_fail.ccs \
      -o include/generated/fail.c 2>/dev/null; then
    echo "--cccportable on emit should fail" >&2
    exit 1
  fi
  cat >bare.ccs <<'EOF'
#!ccc ccs
#pragma(@prelude) off
int add(int a, int b) { return a + b; }
int main(void) { return add(1, 2) == 3 ? 0 : 1; }
EOF
  "$CCC" --emit-c-only --no-runtime --no-line bare.ccs -o include/generated/bare.c
  test -f include/generated/bare.c
  grep -F "generated by Concurrent-C" include/generated/bare.c >/dev/null
  if grep -E '#include <stddef.h>|#include <stdint.h>|#include <stdlib.h>' \
      include/generated/bare.c >/dev/null; then
    echo "prelude-off should strip libc banner includes" >&2
    exit 1
  fi
  if grep -E '^#line |/\*CC_LN ' include/generated/bare.c >/dev/null; then
    echo "--no-line should strip #line" >&2
    exit 1
  fi
  cc -Ivendor/cccportable/include -c include/generated/bare.c -o bare.o
  cc -o bare bare.o
  ./bare
  cat >bare.shcc <<'EOF'
#!/usr/bin/env -S ccc --as=shcc
#pragma(@prelude) off
int main(void) { return 0; }
EOF
  "$CCC" build --as=shcc --emit-c-only --no-runtime --no-line bare.shcc \
      -o include/generated/bare_shcc.c
  if grep -F "ccc/script/prelude" include/generated/bare_shcc.c >/dev/null; then
    echo "prelude-off .shcc should not include script prelude" >&2
    exit 1
  fi
  grep -F "generated by Concurrent-C" include/generated/bare_shcc.c >/dev/null
  cat >lined.ccs <<'EOF'
#!ccc ccs
int main(void) { return 0; }
EOF
  warn="$("$CCC" --emit-c-only lined.ccs -o include/generated/lined.c 2>&1 || true)"
  printf '%s\n' "$warn" | grep -F "warning" >/dev/null
  printf '%s\n' "$warn" | grep -F -- "--no-line" >/dev/null
)
rm -rf "$foreign"
trap 'rm -rf "$tmpdir"' EXIT

###############################################################################
echo "OK"

