#!/bin/sh
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
helper="$repo_root/scripts/safe-rm-tree.sh"
tmp=$(mktemp -d "${TMPDIR:-/tmp}/vlc-autoscaler-safe-rm.XXXXXX")

cleanup() {
    rm -rf -- "$tmp"
}
trap 'cleanup; exit 129' HUP
trap 'cleanup; exit 130' INT
trap 'cleanup; exit 143' TERM
trap cleanup EXIT

"$helper" remove "$tmp/missing" marker "$repo_root"

marked="$tmp/marked path"
mkdir -p -- "$marked"
: > "$marked/marker"
"$helper" remove "$marked" marker "$repo_root"
test ! -e "$marked"

existing="$tmp/existing"
mkdir -p -- "$existing"
if "$helper" init "$existing" marker "$repo_root" 2>/dev/null; then
    echo "existing external root was armed" >&2
    exit 1
fi

created="$tmp/created"
"$helper" init "$created" marker "$repo_root"
test -f "$created/marker"
"$helper" remove "$created" marker "$repo_root"
test ! -e "$created"

unmarked="$tmp/unmarked"
mkdir -p -- "$unmarked"
if "$helper" remove "$unmarked" marker "$repo_root" 2>/dev/null; then
    echo "unmarked cleanup unexpectedly succeeded" >&2
    exit 1
fi
test -d "$unmarked"

if "$helper" remove "$repo_root" marker "$repo_root" 2>/dev/null; then
    echo "repository cleanup unexpectedly succeeded" >&2
    exit 1
fi
if "$helper" remove / marker "$repo_root" 2>/dev/null; then
    echo "filesystem root cleanup unexpectedly succeeded" >&2
    exit 1
fi
if "$helper" remove "$repo_root/src" marker "$repo_root" 2>/dev/null; then
    echo "tracked cleanup unexpectedly succeeded" >&2
    exit 1
fi
if "$helper" remove "$repo_root/.git" marker "$repo_root" 2>/dev/null; then
    echo "Git metadata cleanup unexpectedly succeeded" >&2
    exit 1
fi

ignored="$repo_root/build/safe-rm-test.$$"
"$helper" init "$ignored" marker "$repo_root"
test -f "$ignored/marker"
"$helper" remove "$ignored" marker "$repo_root"
test ! -e "$ignored"

fresh_repo="$tmp/fresh-repo"
mkdir -p -- "$fresh_repo"
git -C "$fresh_repo" init -q
printf 'build/\n' > "$fresh_repo/.gitignore"
git -C "$fresh_repo" add .gitignore
"$helper" init "$fresh_repo/build" marker "$fresh_repo"
test -f "$fresh_repo/build/marker"
"$helper" remove "$fresh_repo/build" marker "$fresh_repo"
test ! -e "$fresh_repo/build"

echo "safe cleanup root checks OK"
