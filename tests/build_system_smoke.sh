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
echo "OK"

