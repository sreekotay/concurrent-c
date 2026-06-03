#!/usr/bin/env bash
set -euo pipefail

ROOT="$(git rev-parse --show-toplevel)"
cd "$ROOT"

if [ ! -f .gitmodules ]; then
    echo "OK: no .gitmodules"
    exit 0
fi

tmpdir="$(mktemp -d)"
cleanup() {
    rm -rf "$tmpdir"
}
trap cleanup EXIT

failures=0

while IFS= read -r line; do
    key="${line%% *}"
    path="${line#* }"
    name="${key#submodule.}"
    name="${name%.path}"
    url="$(git config -f .gitmodules --get "submodule.${name}.url" || true)"
    branch="$(git config -f .gitmodules --get "submodule.${name}.branch" || true)"
    sha="$(git ls-tree HEAD "$path" | { read -r mode type object rest || true; printf '%s' "${object:-}"; })"

    if [ -z "$sha" ]; then
        echo "SKIP $path: no gitlink in HEAD"
        continue
    fi
    if [ -z "$url" ]; then
        echo "FAIL $path: missing URL in .gitmodules"
        failures=$((failures + 1))
        continue
    fi

    repo="$tmpdir/${name//\//_}.git"
    git -c init.defaultBranch=main init --bare "$repo" >/dev/null
    if ! git -C "$repo" fetch --quiet "$url" \
        '+refs/heads/*:refs/remotes/origin/*' \
        '+refs/tags/*:refs/tags/*'; then
        echo "FAIL $path: could not fetch $url"
        failures=$((failures + 1))
        continue
    fi

    if git -C "$repo" cat-file -e "${sha}^{commit}" 2>/dev/null; then
        echo "OK   $path $sha"
        continue
    fi

    echo "FAIL $path $sha is not reachable from $url"
    if [ -d "$path/.git" ] || [ -f "$path/.git" ]; then
        if git -C "$path" cat-file -e "${sha}^{commit}" 2>/dev/null; then
            remote=""
            if git -C "$path" config --get remote.origin.url >/dev/null; then
                remote="origin"
            fi
            if [ -n "$remote" ] && [ -n "$branch" ]; then
                echo "     local commit exists; publish it with:"
                echo "       git -C $path push $remote $sha:$branch"
            elif [ -n "$remote" ]; then
                echo "     local commit exists; publish it to an appropriate branch on '$remote' or repoint the submodule."
            else
                echo "     local commit exists, but no submodule remote is configured; publish or repoint the submodule."
            fi
        fi
    fi
    failures=$((failures + 1))
done < <(git config -f .gitmodules --get-regexp '^submodule\..*\.path$')

if [ "$failures" -ne 0 ]; then
    echo
    echo "Submodule reachability check failed ($failures problem(s))."
    echo "Every gitlink committed by the superproject must be fetchable from the submodule URL."
    exit 1
fi

echo
echo "OK: all submodule gitlinks are reachable from their configured remotes."
