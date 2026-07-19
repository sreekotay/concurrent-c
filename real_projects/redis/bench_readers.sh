#!/bin/sh
# Head-to-head RESP reader benchmark: the @grammar (gen, default) reader vs
# the handrolled reader in redis_resp.cch, in otherwise-identical server
# builds.  Interleaved AND order-swapped rounds — single-position runs lie
# (the second server in a round reliably measures faster on a warm box).
#
#   ./bench_readers.sh [rounds] [requests] [pipeline]
#
# Defaults: 3 rounds, 200000 requests, -P 64.  Needs redis-benchmark on
# PATH (or redis_c/src/redis-benchmark from `make upstream`).
set -e
cd "$(dirname "$0")"

ROUNDS=${1:-3}
REQUESTS=${2:-200000}
PIPELINE=${3:-64}
GEN_PORT=${GEN_PORT:-6480}
HAND_PORT=${HAND_PORT:-6481}

BENCH=$(command -v redis-benchmark || echo "$REDIS_SRC_DIR/src/redis-benchmark")
[ -x "$BENCH" ] || { echo "redis-benchmark not found"; exit 1; }

make redis_idiomatic_gen redis_idiomatic_hand

./out/redis_idiomatic_gen  "$GEN_PORT"  >/dev/null 2>&1 & GEN_PID=$!
./out/redis_idiomatic_hand "$HAND_PORT" >/dev/null 2>&1 & HAND_PID=$!
trap 'kill $GEN_PID $HAND_PID 2>/dev/null' EXIT
sleep 0.7

rps() {  # rps <port> <test>
    "$BENCH" -p "$1" -t "$2" -n "$REQUESTS" -P "$PIPELINE" -c 50 -q 2>/dev/null \
        | tr '\r' '\n' | grep -o '[0-9.]* requests per second' | tail -1 \
        | grep -o '^[0-9.]*'
}

echo "rounds=$ROUNDS requests=$REQUESTS pipeline=$PIPELINE"
echo "round  test  order       gen(rps)      hand(rps)"
r=1
while [ "$r" -le "$ROUNDS" ]; do
    for t in set get; do
        # gen first...
        G1=$(rps "$GEN_PORT" "$t"); H1=$(rps "$HAND_PORT" "$t")
        printf '%-6s %-5s gen-first  %12s  %12s\n' "$r" "$t" "$G1" "$H1"
        # ...then hand first
        H2=$(rps "$HAND_PORT" "$t"); G2=$(rps "$GEN_PORT" "$t")
        printf '%-6s %-5s hand-first %12s  %12s\n' "$r" "$t" "$G2" "$H2"
    done
    r=$((r + 1))
done
echo "done — compare positions across rounds; the noise band here is ~10%"
