#!/usr/bin/env bash
set -euo pipefail

SRC_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

usage() {
  cat <<'EOF'
Install Concurrent-C Syntax into VS Code and/or Cursor.

Usage:
  install-local.sh [--vscode] [--cursor] [--both]

Defaults:
  --both

Installs to:
  VS Code : ~/.vscode/extensions/concurrent-c-syntax
  Cursor  : ~/.cursor/extensions/concurrent-c-syntax
EOF
}

INSTALL_VSCODE=0
INSTALL_CURSOR=0

if [[ $# -eq 0 ]]; then
  INSTALL_VSCODE=1
  INSTALL_CURSOR=1
else
  for arg in "$@"; do
    case "$arg" in
      --vscode) INSTALL_VSCODE=1 ;;
      --cursor) INSTALL_CURSOR=1 ;;
      --both) INSTALL_VSCODE=1; INSTALL_CURSOR=1 ;;
      -h|--help) usage; exit 0 ;;
      *)
        echo "Unknown arg: $arg" >&2
        usage >&2
        exit 2
        ;;
    esac
  done
fi

install_to() {
  local dest_root="$1"
  local dest_dir="${dest_root}/concurrent-c-syntax"

  mkdir -p "${dest_root}"
  rm -rf "${dest_dir}"
  cp -R "${SRC_DIR}" "${dest_dir}"

  echo "Installed Concurrent-C Syntax to:"
  echo "  ${dest_dir}"
}

if [[ "${INSTALL_VSCODE}" -eq 1 ]]; then
  install_to "${HOME}/.vscode/extensions"
  echo "Next steps (VS Code): Developer: Reload Window"
  echo
fi

if [[ "${INSTALL_CURSOR}" -eq 1 ]]; then
  install_to "${HOME}/.cursor/extensions"
  echo "Next steps (Cursor): Developer: Reload Window"
  echo
fi

echo "Open a .ccs or .cch file and confirm language mode is 'Concurrent-C'."


