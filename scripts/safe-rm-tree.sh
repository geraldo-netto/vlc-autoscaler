#!/bin/sh
set -eu

if [ "$#" -ne 4 ]; then
    echo "usage: $0 init|remove ROOT MARKER REPOSITORY" >&2
    exit 2
fi

action=$1
root=$2
marker=$3
repository=$4

case "$action" in
    init|remove) ;;
    *)
        echo "unknown build-root action: '$action'" >&2
        exit 2
        ;;
esac

case "$root" in
    ''|-*)
        echo "refusing unsafe cleanup root: '$root'" >&2
        exit 2
        ;;
esac
case "$marker" in
    ''|.|..|*/*)
        echo "refusing invalid cleanup marker: '$marker'" >&2
        exit 2
        ;;
esac

repository_abs=$(realpath -e -- "$repository")
root_abs=$(realpath -m -- "$root")

if [ "$root_abs" = / ]; then
    echo "refusing filesystem root cleanup" >&2
    exit 2
fi

case "$repository_abs/" in
    "$root_abs/"*)
        echo "refusing repository or ancestor cleanup root: '$root_abs'" >&2
        exit 2
        ;;
esac

case "$root_abs/" in
    "$repository_abs/"*)
        relative=${root_abs#"$repository_abs"/}
        if git -C "$repository_abs" ls-files -- "$relative" | grep -q .; then
            echo "refusing tracked cleanup root: '$root_abs'" >&2
            exit 2
        fi
        if ! git -C "$repository_abs" check-ignore -q -- "$relative/"; then
            echo "refusing unignored cleanup root: '$root_abs'" >&2
            exit 2
        fi
        in_repository=1
        ;;
    *) in_repository=0 ;;
esac

if [ "$action" = init ]; then
    if [ -e "$root" ] || [ -L "$root" ]; then
        if [ ! -d "$root" ]; then
            echo "refusing non-directory build root: '$root_abs'" >&2
            exit 2
        fi
        if [ ! -f "$root/$marker" ] && [ "$in_repository" -eq 0 ]; then
            echo "refusing existing unmarked external build root: '$root_abs'" >&2
            exit 2
        fi
    else
        mkdir -p -- "$root"
    fi
    : > "$root/$marker"
    exit 0
fi

if [ ! -e "$root" ] && [ ! -L "$root" ]; then
    exit 0
fi
if [ ! -f "$root/$marker" ]; then
    echo "refusing unmarked cleanup root: '$root_abs'" >&2
    exit 2
fi
rm -rf -- "$root"
