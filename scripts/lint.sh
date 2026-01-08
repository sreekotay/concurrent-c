#!/usr/bin/env bash
set -euo pipefail

if ! command -v clang-format >/dev/null 2>&1; then
  echo "clang-format not found; skipping lint."
  exit 0
fi

# Dry-run check formatting
find cc -type f \( -name "*.c" -o -name "*.h" \) \
  -not -path "*/third_party/*" \
  -print0 | xargs -0 clang-format --dry-run --Werror

