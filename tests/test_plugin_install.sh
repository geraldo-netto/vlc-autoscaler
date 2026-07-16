#!/bin/sh
set -eu

repo_root=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
tmp=$(mktemp -d "${TMPDIR:-/tmp}/vlc-autoscaler-plugin-install.XXXXXX")
build="$repo_root/build/plugin-install-test.$$"
dest="$tmp/staging path"
base="/vlc plugins"
plugin=libautoupscale_plugin.so

cleanup() {
    rm -rf -- "$tmp" "$build"
}
trap cleanup EXIT HUP INT TERM

make -s -C "$repo_root" BUILD="$build" PLUGIN_OBJS= \
    VLC_LIBS=dummy SWS_LIBS=dummy "$build/.build-config"
printf 'plugin fixture\n' > "$build/$plugin"

make -s -C "$repo_root" BUILD="$build" PLUGIN_OBJS= \
    VLC_LIBS=dummy SWS_LIBS=dummy VLC_PLUGIN_BASE="$base" DESTDIR="$dest" \
    install
test -f "$dest$base/video_filter/$plugin"

make -s -C "$repo_root" BUILD="$build" \
    VLC_PLUGIN_BASE="$base" DESTDIR="$dest" uninstall
test ! -e "$dest$base/video_filter/$plugin"

if make -s -C "$repo_root" BUILD="$build" PLUGIN_OBJS= \
    VLC_LIBS=dummy SWS_LIBS=dummy VLC_PLUGIN_BASE= DESTDIR="$dest" \
    install >/dev/null 2>&1; then
    echo "empty VLC_PLUGIN_BASE install unexpectedly succeeded" >&2
    exit 1
fi
if make -s -C "$repo_root" VLC_PLUGIN_BASE= DESTDIR="$dest" \
    uninstall >/dev/null 2>&1; then
    echo "empty VLC_PLUGIN_BASE uninstall unexpectedly succeeded" >&2
    exit 1
fi

echo "plugin install path checks OK"
