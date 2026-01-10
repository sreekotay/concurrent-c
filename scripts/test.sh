#!/bin/sh
set -euo pipefail

# Local test entrypoint without requiring "make test" (though building cc still uses make today).

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT_DIR"

if [ ! -x "./tools/cc_test" ]; then
  echo "[test] building tools/cc_test"
  cc -O2 -Wall -Wextra tools/cc_test.c -o tools/cc_test
fi

exec ./tools/cc_test --jobs 4 "$@"


