#!/bin/sh
# `#ifdef` / `#ifndef` emit is a clean copy (not half-evaluated), including
# inside struct field lists.
#
# The lowerer must keep the `#ifdef` / `#else` / `#endif` tree and the
# `#define` / `#include` lines that sit inside it. Choosing a side and
# leaving leftover `#else` is `#else without #if` at host-cc.
set -eu
ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT_DIR"
CCC=./cc/bin/ccc

fail() { echo "[test_ifdef_passthrough] FAIL: $1" >&2; exit 1; }

[ -x "$CCC" ] || fail "missing $CCC"
[ -f tests/ifdef_passthrough_smoke.ccs ] || fail "missing smoke source"

out_dir="$(mktemp -d)"
trap 'rm -rf "$out_dir"' EXIT

emitted="$out_dir/ifdef_passthrough_smoke.c"
"$CCC" build --no-cache --emit-c-only tests/ifdef_passthrough_smoke.ccs \
    -o "$emitted" >/dev/null 2>&1 \
    || fail "emit-c-only of ifdef_passthrough_smoke.ccs failed"
[ -s "$emitted" ] || fail "no emitted C produced"

line_of() {
    # first matching line number, or 0
    grep -n "$1" "$emitted" | head -1 | cut -d: -f1
}

ifdef_gai=$(line_of '#ifdef HAVE_GETADDRINFO')
arm1=$(line_of 'RESOLVER_ENOMEM  1')
else_gai=$(line_of '#else')
arm2=$(line_of 'RESOLVER_ENOMEM  2')
endif_one=$(line_of '#endif')
ifdef_net=$(line_of '#ifdef HAVE_NETDB_H')
inc_net=$(line_of '#include <stdbool.h>')
ifdef_ares=$(line_of '#ifdef USE_ARES')
ares_fn=$(line_of 'ares_probe')
ifdef_verb=$(line_of '#ifdef CURLVERBOSE')
ifndef_wake=$(line_of '#ifndef ENABLE_WAKEUP')
wake_def=$(line_of 'async_thrdd_event')
ifdef_win=$(line_of '#ifdef HAVE_WIN_HANDLE')
win_h=$(line_of 'win_handle')
posix_p=$(line_of 'posix_pid')

[ -n "$ifdef_gai" ] || fail "dropped #ifdef HAVE_GETADDRINFO"
[ -n "$arm1" ] || fail "dropped true-arm #define"
[ -n "$else_gai" ] || fail "dropped #else"
[ -n "$arm2" ] || fail "dropped else-arm #define"
[ -n "$endif_one" ] || fail "dropped #endif"
[ -n "$ifdef_net" ] || fail "dropped #ifdef HAVE_NETDB_H"
[ -n "$inc_net" ] || fail "dropped #include inside #ifdef"
[ -n "$ifdef_ares" ] || fail "dropped #ifdef USE_ARES"
[ -n "$ares_fn" ] || fail "dropped USE_ARES body"
[ -n "$ifdef_verb" ] || fail "dropped #ifdef CURLVERBOSE"
[ -n "$ifndef_wake" ] || fail "dropped #ifndef ENABLE_WAKEUP"
[ -n "$wake_def" ] || fail "dropped ENABLE_WAKEUP #define"
[ -n "$ifdef_win" ] || fail "dropped #ifdef HAVE_WIN_HANDLE in struct"
[ -n "$win_h" ] || fail "dropped win_handle field"
[ -n "$posix_p" ] || fail "dropped posix_pid field"

# Balanced tree: opening #ifdef before its #define, #else, other #define.
# Hoisting the chosen #define above #ifdef is the half-evaluated bug.
ord() {
    [ "$1" -lt "$2" ] || fail "$3"
}

ord "$ifdef_gai" "$arm1" "true-arm #define precedes #ifdef (hoisted?)"
ord "$arm1" "$else_gai" "#else precedes true-arm #define"
ord "$else_gai" "$arm2" "else-arm #define precedes #else"
ord "$arm2" "$endif_one" "#endif precedes else-arm #define"
ord "$ifdef_net" "$inc_net" "include hoisted out of #ifdef HAVE_NETDB_H"
ord "$ifdef_ares" "$ares_fn" "ares_probe precedes #ifdef USE_ARES"
ord "$ifndef_wake" "$wake_def" "async_thrdd_event hoisted out of #ifndef"
ord "$ifdef_win" "$win_h" "win_handle precedes #ifdef HAVE_WIN_HANDLE"
ord "$win_h" "$posix_p" "posix_pid precedes win_handle (else arm lost?)"

# Host-cc selects the GETADDRINFO / quiet / no-ares side.
host_c="$out_dir/host.c"
cp "$emitted" "$host_c"
cc -c -o "$out_dir/host.o" \
    -DHAVE_GETADDRINFO \
    "$host_c" >/dev/null 2>"$out_dir/host.err" \
    || fail "host-cc rejected balanced emit: $(cat "$out_dir/host.err")"

echo "[test_ifdef_passthrough] ok"
