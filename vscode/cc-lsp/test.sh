#!/usr/bin/env bash
# Starter + stress tests for cc-lsp over JSON-RPC stdio.
# Does not reload the editor. Rebuilds the binary if sources are newer.
set -euo pipefail

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "${DIR}/../.." && pwd)"
CCC="${CCC:-${REPO}/cc/bin/ccc}"
BIN="${DIR}/bin/cc-lsp"

if [[ ! -x "${CCC}" ]]; then
  echo "ccc not found at ${CCC}" >&2
  exit 2
fi

need_build=0
if [[ ! -x "${BIN}" ]]; then
  need_build=1
elif [[ "${DIR}/cc_lsp.ccs" -nt "${BIN}" || "${DIR}/cc_lsp_hover.cch" -nt "${BIN}" ]]; then
  need_build=1
fi

if [[ "${need_build}" -eq 1 ]]; then
  "${DIR}/install-local.sh" --build-only
fi

"${BIN}" --smoke

node "${DIR}/test-suite.js" "$@"
