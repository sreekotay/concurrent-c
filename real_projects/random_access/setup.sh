#!/usr/bin/env bash
# Fetch Chapel HPCC RandomAccess sources into chapel/.
set -euo pipefail
cd "$(dirname "$0")/chapel"
BASE=https://raw.githubusercontent.com/chapel-lang/chapel/main/test/release/examples/benchmarks/hpcc
echo "Downloading Chapel HPCC RA into chapel/..."
curl -fsSL -o ra.chpl "$BASE/ra.chpl"
curl -fsSL -o ra-atomics.chpl "$BASE/ra-atomics.chpl"
curl -fsSL -o RARandomStream.chpl "$BASE/RARandomStream.chpl"
curl -fsSL -o HPCCProblemSize.chpl "$BASE/HPCCProblemSize.chpl"
git ls-remote https://github.com/chapel-lang/chapel.git refs/heads/main \
  | awk '{print $1}' > COMMIT.txt
echo "Pinned chapel main $(cat COMMIT.txt)"
ls -l ra.chpl ra-atomics.chpl RARandomStream.chpl HPCCProblemSize.chpl
