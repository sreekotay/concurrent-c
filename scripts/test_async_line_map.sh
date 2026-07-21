#!/bin/sh
# @async state-machine #line accuracy on a real project.
#
# The async lowering replaces each @async fn with a poll/frame/drop blob
# ~20x the source's line count.  Pre-fix the blob carried NO #line
# accounting, so host-compiler diagnostics and debugger lines inside it
# mapped to last-directive-above + physical drift — for
# redis_idiomatic.ccs (1717 lines) a diagnostic inside the owner-loop
# poll fn was reported at redis_idiomatic.ccs:1964, past EOF.  The
# emitter now drops a masked per-statement marker (async_ast.c,
# cc__emit_src_line_marker) and every coordinate-stamping pass computes
# USER lines via the buffer's #line/CC_LN ledger (util/text.h,
# cc_user_line_for_offset) instead of raw newline counts.
#
# This test pins the composed result on the emitted C of the real redis
# port:
#   1) the async poll functions carry #line directives at all (the
#      per-statement markers survived to the final write), and
#   2) NO #line-mapped coordinate for the .ccs exceeds the source file's
#      line count (the cpp mapping model: `#line N` makes the next
#      physical line N, +1 per line until the next directive).
set -eu
ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT_DIR"
CCC=./cc/bin/ccc

fail() { echo "[test_async_line_map] FAIL: $1" >&2; exit 1; }

command -v python3 >/dev/null 2>&1 || fail "python3 is required for the mapping check"

SRC=real_projects/redis/redis_idiomatic.ccs
[ -f "$SRC" ] || fail "missing $SRC"

out_dir="$(mktemp -d)"
trap 'rm -rf "$out_dir"' EXIT
emitted="$out_dir/redis_idiomatic.c"

"$CCC" build --no-cache --emit-c-only "$SRC" -o "$emitted" >/dev/null 2>&1 \
  || fail "emit-c-only build of $SRC failed"
[ -s "$emitted" ] || fail "no emitted C produced"

# (1) async poll fns exist and the machine region is #line-mapped.
grep -q '__cc_async_owner_loop_[0-9]*_poll' "$emitted" \
  || fail "owner_loop poll fn not found in emitted C (async lowering shape drifted)"
n_dirs=$(grep -c '#line [0-9][0-9]* ".*redis_idiomatic\.ccs"' "$emitted" || true)
[ "$n_dirs" -ge 50 ] \
  || fail "expected >=50 #line directives citing redis_idiomatic.ccs in emitted C, got $n_dirs (per-statement async markers missing?)"

# (2) no mapped coordinate exceeds the source's line count.
python3 - "$emitted" "$SRC" <<'PYEOF'
import re, sys
cfile, src = sys.argv[1], sys.argv[2]
src_lines = sum(1 for _ in open(src, errors="replace"))
srcbase = src.rsplit("/", 1)[-1]
cur_file = None
nxt = None  # mapped line of the next physical line
bad = 0
worst = (0, 0)
for i, ln in enumerate(open(cfile, errors="replace"), 1):
    m = re.match(r'\s*#\s*line\s+(\d+)(?:\s+"([^"]*)")?', ln)
    if m:
        if m.group(2) is not None:
            cur_file = m.group(2)
        nxt = int(m.group(1))
        continue
    if cur_file and cur_file.rsplit("/", 1)[-1] == srcbase and nxt is not None:
        if nxt > src_lines:
            bad += 1
            if nxt > worst[0]:
                worst = (nxt, i)
        nxt += 1
if bad:
    print(f"[test_async_line_map] FAIL: {bad} emitted line(s) map past "
          f"{srcbase} EOF ({src_lines} lines); worst mapped {worst[0]} at "
          f"{cfile}:{worst[1]}", file=sys.stderr)
    sys.exit(1)
PYEOF

echo "[test_async_line_map] OK"
