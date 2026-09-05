#!/usr/bin/env bash
# Best-effort install of darkhttpd sources, nginx, and load tools.
# Does not fail the whole specimen if optional peers are missing.
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

DARKHTTPD_ONLY=0
if [[ "${1:-}" == "--darkhttpd-only" ]]; then
    DARKHTTPD_ONLY=1
fi

fetch_darkhttpd() {
    local dest="$SCRIPT_DIR/vendor/darkhttpd"
    if [[ -f "$dest/darkhttpd.c" ]]; then
        echo "darkhttpd sources present"
        return 0
    fi
    mkdir -p vendor
    rm -rf "$dest"
    echo "fetching darkhttpd..."
    git clone --depth 1 https://github.com/emikulic/darkhttpd.git "$dest"
}

install_brew_pkg() {
    local pkg="$1"
    if command -v "$pkg" >/dev/null 2>&1; then
        echo "$pkg already installed"
        return 0
    fi
    if ! command -v brew >/dev/null 2>&1; then
        echo "brew not found; skip $pkg"
        return 0
    fi
    echo "brew install $pkg ..."
    brew install "$pkg" || echo "warning: brew install $pkg failed"
}

fetch_darkhttpd
if [[ "$DARKHTTPD_ONLY" == "1" ]]; then
    exit 0
fi

install_brew_pkg nginx
install_brew_pkg wrk
# hey is optional
if ! command -v hey >/dev/null 2>&1; then
    if command -v brew >/dev/null 2>&1; then
        brew install hey 2>/dev/null || true
    fi
fi

mkdir -p out run
./gen_fixtures.sh
echo "setup done"
