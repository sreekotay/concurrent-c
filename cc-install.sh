#!/bin/sh
# Bootstrap, build, and install Concurrent-C.
# - If run from the repo root, use the current checkout.
# - Otherwise, clone into $PWD/concurrent-c (or $CC_REPO_DIR) first.
set -e

usage() {
  cat <<'EOF'
Usage:
  cc-install.sh [--add-to-path] [--no-add-to-path] [--no-editor-tools]

Options:
  --add-to-path     Add PREFIX/bin to your shell startup file without prompting.
  --no-add-to-path  Never modify shell startup files.
  --no-editor-tools Skip VS Code / Cursor syntax and debugger setup.
  -h, --help        Show this help.

Default behavior:
  If running interactively in a terminal, prompt to add PREFIX/bin to PATH.
  Install Concurrent-C syntax support for VS Code and Cursor.
  Attempt to install the CodeLLDB debugger extension when the editor CLI is available.
  If running non-interactively, do not modify shell config.
EOF
}

die() {
  echo "Error: $1" >&2
  exit 1
}

need_cmd() {
  if ! command -v "$1" >/dev/null 2>&1; then
    die "required command not found: $1"
  fi
}

choose_shell_rc() {
  shell_name="$(basename "${SHELL:-}")"
  case "$shell_name" in
    zsh) printf '%s\n' "$HOME/.zshrc" ;;
    bash) printf '%s\n' "$HOME/.bashrc" ;;
    *) printf '%s\n' "$HOME/.profile" ;;
  esac
}

add_prefix_to_path_file() {
  rc_file="$1"
  path_line="export PATH=\"$PREFIX/bin:\$PATH\""

  mkdir -p "$(dirname "$rc_file")"
  [ -f "$rc_file" ] || : > "$rc_file"

  if grep -F "$path_line" "$rc_file" >/dev/null 2>&1; then
    echo "PATH already configured in $rc_file"
    return 0
  fi

  {
    printf '\n'
    printf '# Added by cc-install.sh\n'
    printf '%s\n' "$path_line"
  } >> "$rc_file"

  echo "Added $PREFIX/bin to PATH in $rc_file"
  echo "Run: source \"$rc_file\""
}

maybe_add_to_path() {
  case "$ADD_TO_PATH_MODE" in
    never)
      return 0
      ;;
    always)
      rc_file="$(choose_shell_rc)"
      add_prefix_to_path_file "$rc_file"
      return 0
      ;;
  esac

  if [ ! -t 0 ] || [ ! -t 1 ]; then
    return 0
  fi

  rc_file="$(choose_shell_rc)"
  printf 'Add %s/bin to PATH in %s? [Y/n] ' "$PREFIX" "$rc_file"
  IFS= read -r reply || reply=""
  case "$reply" in
    ""|y|Y|yes|YES|Yes)
      add_prefix_to_path_file "$rc_file"
      ;;
    *)
      echo "Skipping PATH update."
      ;;
  esac
}

install_editor_syntax_to() {
  dest_root="$1"
  editor_name="$2"
  src_dir="$ROOT_DIR/vscode/ccs-syntax"
  dest_dir="${dest_root}/concurrent-c-syntax"

  if [ ! -d "$src_dir" ]; then
    echo "Skipping ${editor_name} syntax install: ${src_dir} not found."
    return 0
  fi

  mkdir -p "$dest_root"
  rm -rf "$dest_dir"
  cp -R "$src_dir" "$dest_dir"

  echo "Installed Concurrent-C syntax for ${editor_name}: ${dest_dir}"
}

maybe_install_lldb_extension() {
  editor_cmd="$1"
  editor_name="$2"

  if ! command -v "$editor_cmd" >/dev/null 2>&1; then
    echo "Skipping ${editor_name} debugger extension: '${editor_cmd}' CLI not found."
    return 0
  fi

  if "$editor_cmd" --install-extension vadimcn.vscode-lldb >/dev/null 2>&1; then
    echo "Installed CodeLLDB for ${editor_name}."
  else
    echo "Warning: failed to install CodeLLDB for ${editor_name} via '${editor_cmd}'." >&2
  fi
}

install_editor_tools() {
  if [ "$INSTALL_EDITOR_TOOLS" != "1" ]; then
    return 0
  fi

  install_editor_syntax_to "$HOME/.vscode/extensions" "VS Code"
  install_editor_syntax_to "$HOME/.cursor/extensions" "Cursor"
  maybe_install_lldb_extension code "VS Code"
  maybe_install_lldb_extension cursor "Cursor"
}

install_local_ccc_wrapper() {
  cat > "$ROOT_DIR/ccc" <<'EOF'
#!/bin/sh
set -e
SELF_DIR="$(CDPATH= cd -- "$(dirname "$0")" && pwd)"
exec "$SELF_DIR/cc/bin/ccc" "$@"
EOF
  chmod +x "$ROOT_DIR/ccc"
}

print_path_hint() {
  echo "Repo-local command (from this checkout): ./ccc"
  case ":$PATH:" in
    *":$PREFIX/bin:"*)
      echo "Global command available on PATH: ccc"
      ;;
    *)
      echo "Global command not on PATH yet: ccc"
      echo "Installed binary: $PREFIX/bin/ccc"
      echo "Add $PREFIX/bin to PATH if you want bare 'ccc' to work."
      ;;
  esac
}

print_editor_tool_hints() {
  if [ "$INSTALL_EDITOR_TOOLS" != "1" ]; then
    return 0
  fi

  echo "Workspace debug configs: $ROOT_DIR/.vscode/tasks.json and $ROOT_DIR/.vscode/launch.json"
  echo "Reload VS Code / Cursor after install so the syntax extension is picked up."
}

check_prefix_writable() {
  target="$1"

  if [ -d "$target" ]; then
    test -w "$target" && return 0
  else
    parent="$(dirname "$target")"
    while [ ! -d "$parent" ] && [ "$parent" != "/" ]; do
      parent="$(dirname "$parent")"
    done
    test -w "$parent" && return 0
  fi

  cat >&2 <<EOF
Error: install prefix is not writable: $target

Try one of:
  PREFIX="\$HOME/.local" ./cc-install.sh
  PREFIX="/opt/ccc" ./cc-install.sh

Or rerun with elevated privileges if you really want a system-wide install.
EOF
  exit 1
}

need_cmd git
need_cmd make

BUILD="${BUILD:-release}"
PREFIX="${PREFIX:-/usr/local}"
CC_REPO_URL="${CC_REPO_URL:-https://github.com/sreekotay/concurrent-c.git}"
CC_REPO_DIR="${CC_REPO_DIR:-$PWD/concurrent-c}"
ADD_TO_PATH_MODE="ask"
INSTALL_EDITOR_TOOLS="1"

while [ $# -gt 0 ]; do
  case "$1" in
    --add-to-path)
      ADD_TO_PATH_MODE="always"
      ;;
    --no-add-to-path)
      ADD_TO_PATH_MODE="never"
      ;;
    --no-editor-tools)
      INSTALL_EDITOR_TOOLS="0"
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      usage >&2
      die "unknown argument: $1"
      ;;
  esac
  shift
done

if [ -f "$PWD/Formula/ccc.rb" ] && [ -f "$PWD/Makefile" ] && [ -d "$PWD/cc" ]; then
  ROOT_DIR="$PWD"
  echo "Using existing concurrent-c checkout in $ROOT_DIR"
else
  ROOT_DIR="$CC_REPO_DIR"
  if [ -d "$ROOT_DIR/.git" ]; then
    echo "Using existing concurrent-c checkout in $ROOT_DIR"
  elif [ -e "$ROOT_DIR" ]; then
    die "target exists and is not a concurrent-c checkout: $ROOT_DIR"
  else
    echo "Cloning concurrent-c into $ROOT_DIR..."
    git clone "$CC_REPO_URL" "$ROOT_DIR"
  fi
fi

cd "$ROOT_DIR"

check_prefix_writable "$PREFIX"

echo "Initializing required submodules..."
git submodule sync -- third_party/tcc third_party/liblfds >/dev/null 2>&1 || true
git submodule update --init third_party/tcc third_party/liblfds

if [ -f third_party/tcc/cc_ast_record.h ]; then
  echo "TCC patches already applied."
else
  ./scripts/apply_tcc_patches.sh
fi

echo "Configuring TinyCC..."
(
  cd third_party/tcc
  ./configure --config-cc_ext
)

if [ -n "${JOBS:-}" ]; then
  jobs="$JOBS"
else
  jobs="$(getconf _NPROCESSORS_ONLN 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)"
fi

echo "Building TinyCC..."
make -C third_party/tcc -j"$jobs"

echo "Building Concurrent-C..."
make cc BUILD="$BUILD"
install_local_ccc_wrapper

echo "Installing Concurrent-C to $PREFIX..."
make install BUILD="$BUILD" PREFIX="$PREFIX"
install_editor_tools
maybe_add_to_path

echo "Install complete."
echo "Checkout: $ROOT_DIR"
print_path_hint
print_editor_tool_hints
