#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

echo "bench_simple.sh scaffold"
echo
echo "Planned role:"
echo "  - build upstream Redis and local Concurrent-C targets"
echo "  - run low-friction redis-benchmark workloads"
echo "  - start with PING/SET/GET/INCR"
echo
echo "Not implemented yet."
echo "Expected future flow:"
echo "  cd \"$SCRIPT_DIR\""
echo "  make upstream redis_tutorial redis_idiomatic redis_cc"
