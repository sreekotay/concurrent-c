#!/bin/sh
# lint_scanners.sh — RATCHET: no new hand-rolled comment/string state machines.
#
# The canonical scanners are util/text.h (bounded searches / matchers),
# util/text_scan.h (CCInertScan), preprocess.c's CCScannerState, and
# template_scan.c (template extents).  Every other `in_bc` / `in_block_comment`
# local is legacy debt being paid down (PASS_CLEANUP_PLAN phase 4).
#
# This lint counts scanner-state markers per file and FAILS LOUDLY when a file
# exceeds its recorded baseline (new state machine added) or a new file appears.
# Shrinking a count?  Lower its baseline here in the same commit — the ratchet
# only turns one way.
set -eu
cd "$(dirname "$0")/.."

PATTERN='in_bc\b|in_block_comment'

baseline() {
cat <<'EOF'
cc/src/comptime/executor.c 3
cc/src/comptime/hook_compile.c 15
cc/src/comptime/symbols.c 3
cc/src/header/lower_header.c 13
cc/src/ir/ir.c 12
cc/src/parser/parse.c 7
cc/src/preprocess/emit_plan.c 9
cc/src/preprocess/preprocess.c 72
cc/src/preprocess/template_scan.c 10
cc/src/preprocess/type_registry.c 3
cc/src/util/text.h 22
cc/src/util/text_scan.h 5
cc/src/visitor/async_ast.c 3
cc/src/visitor/pass_autoblock.c 4
cc/src/visitor/pass_create.c 4
cc/src/visitor/pass_defer_syntax.c 4
cc/src/visitor/pass_err_syntax.c 4
cc/src/visitor/pass_type_syntax.c 4
cc/src/visitor/visit_codegen.c 10
EOF
}

fail=0
current=$(grep -rlE "$PATTERN" cc/src --include='*.c' --include='*.h' | sort)
for f in $current; do
    n=$(grep -cE "$PATTERN" "$f")
    b=$(baseline | awk -v f="$f" '$1 == f { print $2 }')
    if [ -z "$b" ]; then
        echo "lint_scanners: NEW hand-rolled scanner state in $f ($n markers) — use util/text.h or CCInertScan" >&2
        fail=1
    elif [ "$n" -gt "$b" ]; then
        echo "lint_scanners: $f grew scanner-state markers ($b -> $n) — use util/text.h or CCInertScan" >&2
        fail=1
    fi
done
if [ "$fail" -ne 0 ]; then
    echo "lint_scanners: FAILED (see PASS_CLEANUP_PLAN.md phase 4)" >&2
    exit 1
fi
echo "lint_scanners: OK"
