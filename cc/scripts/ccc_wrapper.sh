#!/bin/sh
# ccc wrapper: ensures lowered headers (.cch -> .h) are up to date before
# dispatching to the real compiler binary.  Safe to run from parallel make
# invocations: header lowering is gated by a filesystem mkdir-lock.
#
# Layout expected in a source tree:
#   cc/bin/ccc             -> this script
#   cc/bin/.ccc-bin        -> compiled ccc binary
#   cc/include/**/*.cch    -> source headers
#   cc/runtime/*.{c,h}     -> runtime sources
#   out/include/**/*.h     -> lowered headers (rewritten by lower_headers)
#   out/runtime/*.{c,h}    -> rewritten runtime translation units
#   out/cc/bin/lower_headers           -> header-lowering tool
#   out/include/.headers_lowered.stamp -> mtime barrier
#
# In an installed tree (no sibling cc/include source dir), the script detects
# the missing source tree and transparently delegates to .ccc-bin without
# attempting to lower anything.
#
# Env knobs:
#   CCC_SKIP_HEADER_CHECK=1  skip the staleness check entirely (hot-path opt).
#   CCC_LOWER_QUIET=1        suppress the "regenerating lowered headers" line.
#   CCC_LOWER_VERBOSE=1      print paths + reason for lowering.
set -e

SELF_DIR="$(cd "$(dirname "$0")" && pwd)"
REAL_BIN="$SELF_DIR/.ccc-bin"
CC_DIR="$(cd "$SELF_DIR/.." && pwd)"
REPO_ROOT="$(cd "$CC_DIR/.." && pwd)"

CCH_DIR="$CC_DIR/include"
RUNTIME_SRC="$CC_DIR/runtime"
OUT_INCLUDE="$REPO_ROOT/out/include"
OUT_RUNTIME="$REPO_ROOT/out/runtime"
LOWER_BIN="$REPO_ROOT/out/cc/bin/lower_headers"
STAMP="$OUT_INCLUDE/.headers_lowered.stamp"
LOCK_DIR="$OUT_INCLUDE/.headers_lowered.lock"

# Returns 0 when lowered headers are stale or missing.
ccc_headers_stale() {
    [ ! -f "$STAMP" ] && return 0
    if [ -d "$CCH_DIR" ]; then
        hit=$(find "$CCH_DIR" -name '*.cch' -type f -newer "$STAMP" -print -quit 2>/dev/null)
        [ -n "$hit" ] && { [ "$CCC_LOWER_VERBOSE" = "1" ] && printf 'ccc: stale .cch: %s\n' "$hit" >&2; return 0; }
    fi
    if [ -d "$RUNTIME_SRC" ]; then
        hit=$(find "$RUNTIME_SRC" \( -name '*.c' -o -name '*.h' \) -type f -newer "$STAMP" -print -quit 2>/dev/null)
        [ -n "$hit" ] && { [ "$CCC_LOWER_VERBOSE" = "1" ] && printf 'ccc: stale runtime: %s\n' "$hit" >&2; return 0; }
    fi
    return 1
}

# Only run the check when we can actually fix staleness (source tree + tool
# both present).  Everything else no-ops cleanly.
if [ "$CCC_SKIP_HEADER_CHECK" != "1" ] \
   && [ -x "$LOWER_BIN" ] \
   && [ -d "$CCH_DIR" ] \
   && ccc_headers_stale; then
    mkdir -p "$OUT_INCLUDE" "$OUT_RUNTIME"

    # Spin up to ~30s waiting for a concurrent ccc to finish lowering.
    i=0
    while [ "$i" -lt 300 ] && ! mkdir "$LOCK_DIR" 2>/dev/null; do
        sleep 0.1 2>/dev/null || sleep 1
        i=$((i + 1))
    done

    if [ "$i" -lt 300 ]; then
        # Release lock on any exit path.
        trap 'rmdir "$LOCK_DIR" 2>/dev/null || true' EXIT INT TERM

        # Re-check under lock: another ccc may have already done the work.
        if ccc_headers_stale; then
            if [ "$CCC_LOWER_QUIET" != "1" ]; then
                printf 'ccc: regenerating lowered headers (.cch newer than stamp)\n' >&2
            fi
            "$LOWER_BIN" "$CCH_DIR" "$OUT_INCLUDE" \
                --rewrite-includes "$RUNTIME_SRC" "$OUT_RUNTIME" >/dev/null
            touch "$STAMP"
        fi

        rmdir "$LOCK_DIR" 2>/dev/null || true
        trap - EXIT INT TERM
    fi
    # If the lock couldn't be acquired, proceed anyway: whichever holder
    # finishes will have written an updated stamp, and the worst case is
    # one more rebuild of the caller on the next invocation.
fi

exec "$REAL_BIN" "$@"
