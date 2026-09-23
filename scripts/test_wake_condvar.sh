#!/bin/bash
# Condition waits on the wake primitive's condvar fallback.
#
# Linux and macOS park OS threads on futex / ulock; the pthread condvar
# fallback is what every other platform gets, and nothing else here builds
# it. -DCC_WAKE_FORCE_CONDVAR selects it and gives each primitive a heap
# token from init to destroy, so a wait node whose retire is skipped on
# some exit leaks under LeakSanitizer.
#
# Usage:
#   scripts/test_wake_condvar.sh          # plain build
#   scripts/test_wake_condvar.sh asan     # AddressSanitizer + leak check
#   scripts/test_wake_condvar.sh tsan     # ThreadSanitizer (CC=clang)
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT_DIR"
CCC="${CCC:-$ROOT_DIR/cc/bin/ccc}"
mode="${1:-plain}"

TESTS=(
    tests/exclusive_cond_exit_paths_smoke.ccs
    tests/exclusive_cond_wake_frame_smoke.ccs
)
# Its handshake flags are plain ints (a test-side race TSan reports).
[ "$mode" = tsan ] || TESTS+=(tests/exclusive_acquire_when_smoke.ccs)

cc_flags="-DCC_WAKE_FORCE_CONDVAR"
ld_flags=""
env_extra=()
case "$mode" in
    plain) ;;
    asan)
        cc_flags="$cc_flags -fsanitize=address -fno-omit-frame-pointer -g"
        ld_flags="-fsanitize=address"
        env_extra=(ASAN_OPTIONS="${ASAN_OPTIONS:+${ASAN_OPTIONS}:}detect_leaks=1")
        ;;
    tsan)
        cc_flags="$cc_flags -fsanitize=thread -g"
        ld_flags="-fsanitize=thread"
        supp="$ROOT_DIR/scripts/tsan_fiber.supp"
        env_extra=(CC="${CC:-clang}"
                   TSAN_OPTIONS="${TSAN_OPTIONS:+${TSAN_OPTIONS}:}suppressions=${supp}:halt_on_error=0")
        # shellcheck source=tsan_fiber_fp.sh
        . "$ROOT_DIR/scripts/tsan_fiber_fp.sh"
        ;;
    *) echo "usage: $0 [plain|asan|tsan]" >&2; exit 2 ;;
esac

failed=0
for t in "${TESTS[@]}"; do
    name="$(basename "$t" .ccs)"
    printf '  %-40s ' "$name [condvar $mode]"
    rc=0
    args=(run "$t" --no-cache --cc-flags "$cc_flags")
    [ -n "$ld_flags" ] && args+=(--ld-flags "$ld_flags")
    out="$(env "${env_extra[@]+"${env_extra[@]}"}" "$CCC" "${args[@]}" 2>&1)" || rc=$?
    if printf '%s\n' "$out" | grep -qE "AddressSanitizer|LeakSanitizer"; then
        echo "FAIL (sanitizer)"
        printf '%s\n' "$out" | grep -E "SUMMARY|ERROR|#[0-9] " | head -12
        failed=$((failed + 1))
    elif printf '%s\n' "$out" | grep -qE "ThreadSanitizer" \
         && ! { [ "$mode" = tsan ] && tsan_output_only_fiber_teardown_fp "$out"; }; then
        echo "FAIL (race)"
        printf '%s\n' "$out" | grep -A8 "WARNING: ThreadSanitizer" | head -24
        failed=$((failed + 1))
    elif [ "$rc" -ne 0 ]; then
        echo "FAIL (exit $rc)"
        printf '%s\n' "$out" | tail -5
        failed=$((failed + 1))
    else
        echo "OK"
    fi
done

if [ "$failed" -ne 0 ]; then
    echo "[test_wake_condvar] $failed failed ($mode)"
    exit 1
fi
echo "[test_wake_condvar] ok ($mode)"
