# True if TSan printed data-race warnings and every one is a nursery
# teardown false positive (see tsan_fiber.supp). Linux reports the
# same alive_count-ordered free as an atomic load of end_state in
# notify_child_done / last_exit, not only as cc_nursery_release.
tsan_fiber_teardown_fp_re() {
    printf '%s' 'cc_nursery_release|cc_nursery_free|cc_nursery_last_exit|cc_nursery_notify_child_done|cc_nursery_abandon'
}

tsan_output_only_fiber_teardown_fp() {
    local out="$1"
    local w r
    echo "$out" | grep -qE "ThreadSanitizer.*data race" || return 1
    w=$(printf '%s\n' "$out" | grep -c "WARNING: ThreadSanitizer: data race" || true)
    r=$(printf '%s\n' "$out" | grep -cE "$(tsan_fiber_teardown_fp_re)" || true)
    [ "$w" -gt 0 ] && [ "$w" -le "$r" ]
}
