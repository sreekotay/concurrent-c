#!/usr/bin/env bash
set -euo pipefail

SRC_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "${SRC_DIR}/../.." && pwd)"
CCC="${CCC:-${REPO_DIR}/cc/bin/ccc}"

usage() {
  cat <<'EOF'
Build the Concurrent-C language server and install the editor extension.

Usage:
  install-local.sh [--vscode] [--cursor] [--both] [--build-only]

Defaults:
  --both

Iterate on cc_lsp.ccs without reloading the window:
  ./install-local.sh --build-only
  # or Command Palette: Concurrent-C: Rebuild Language Server

The installed extension prefers <workspace>/vscode/cc-lsp/bin/cc-lsp
and restarts the client when that binary changes. Reinstall + Reload
only when extension.js / package.json change.
EOF
}

INSTALL_VSCODE=0
INSTALL_CURSOR=0
BUILD_ONLY=0

if [[ $# -eq 0 ]]; then
  INSTALL_VSCODE=1
  INSTALL_CURSOR=1
else
  for arg in "$@"; do
    case "$arg" in
      --vscode) INSTALL_VSCODE=1 ;;
      --cursor) INSTALL_CURSOR=1 ;;
      --both) INSTALL_VSCODE=1; INSTALL_CURSOR=1 ;;
      --build-only) BUILD_ONLY=1 ;;
      -h|--help) usage; exit 0 ;;
      *)
        echo "Unknown arg: $arg" >&2
        usage >&2
        exit 2
        ;;
    esac
  done
fi

if [[ ! -x "${CCC}" ]]; then
  echo "ccc not found at ${CCC}" >&2
  exit 1
fi

mkdir -p "${SRC_DIR}/bin"
"${CCC}" build --release "${SRC_DIR}/cc_lsp.ccs" -o "${SRC_DIR}/bin/cc-lsp"
chmod +x "${SRC_DIR}/bin/cc-lsp"

if [[ ! -d "${SRC_DIR}/node_modules/vscode-languageclient" ]]; then
  (cd "${SRC_DIR}" && npm install --omit=dev)
fi

if [[ "${BUILD_ONLY}" -eq 1 ]]; then
  echo "Built ${SRC_DIR}/bin/cc-lsp"
  exit 0
fi

install_to() {
  local dest_root="$1"
  local dest_dir="${dest_root}/concurrent-c-lsp"

  mkdir -p "${dest_root}"
  rm -rf "${dest_dir}"
  mkdir -p "${dest_dir}/bin" "${dest_dir}/syntaxes"
  cp "${SRC_DIR}/package.json" "${dest_dir}/"
  cp "${SRC_DIR}/extension.js" "${dest_dir}/"
  cp "${SRC_DIR}/bin/cc-lsp" "${dest_dir}/bin/cc-lsp"
  if [[ -f "${SRC_DIR}/language-configuration.json" ]]; then
    cp "${SRC_DIR}/language-configuration.json" "${dest_dir}/"
  fi
  if [[ -f "${SRC_DIR}/syntaxes/concurrent-c.tmLanguage.json" ]]; then
    cp "${SRC_DIR}/syntaxes/concurrent-c.tmLanguage.json" "${dest_dir}/syntaxes/"
  fi
  if [[ -f "${SRC_DIR}/README.md" ]]; then
    cp "${SRC_DIR}/README.md" "${dest_dir}/"
  fi
  (cd "${dest_dir}" && npm install --omit=dev --silent)

  echo "Installed Concurrent-C Language Server to:"
  echo "  ${dest_dir}"
}

if [[ "${INSTALL_VSCODE}" -eq 1 ]]; then
  install_to "${HOME}/.vscode/extensions"
  echo "Next steps (VS Code): Developer → Reload Window"
  echo
fi

if [[ "${INSTALL_CURSOR}" -eq 1 ]]; then
  install_to "${HOME}/.cursor/extensions"
  echo "Next steps (Cursor): Developer → Reload Window"
  echo
fi
