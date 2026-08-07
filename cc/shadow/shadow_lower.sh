#!/usr/bin/env bash
# Thin wrapper → cc/scripts/shadow_lower.sh (prefer native binary).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
exec bash "$ROOT/cc/scripts/shadow_lower.sh" "$@"
