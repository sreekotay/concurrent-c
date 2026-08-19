# True if TSan printed data-race warnings and every one is the
# cc_nursery_release teardown false positive (see tsan_fiber.supp).
tsan_output_only_fiber_teardown_fp() {
    local out="$1"
    local w r
    echo "$out" | grep -qE "ThreadSanitizer.*data race" || return 1
    w=$(printf '%s\n' "$out" | grep -c "WARNING: ThreadSanitizer: data race" || true)
    r=$(printf '%s\n' "$out" | grep -c "cc_nursery_release" || true)
    [ "$w" -gt 0 ] && [ "$w" -le "$r" ]
}
