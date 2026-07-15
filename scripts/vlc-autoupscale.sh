#!/bin/sh
# Launch VLC with the autoupscale video filter preconfigured.
# Installed as ~/.local/bin/vlc-autoupscale by install-vlc-autoupscale-action.sh.
#
# VLC_AUTOUPSCALE_ARGS replaces the whole flag set, so it must carry
# --video-filter=autoupscale itself or the filter is never loaded (WIRE-2):
#   VLC_AUTOUPSCALE_ARGS="--video-filter=autoupscale --autoupscale-target=4" \
#       vlc-autoupscale clip.mkv
# Setting it to the empty string launches plain VLC with no filter at all.
set -eu

DEFAULT_ARGS="--video-filter=autoupscale --autoupscale-target=2 --autoupscale-algo=3 --autoupscale-usm=20"
VLC_BIN=vlc

# SH-3: `-` not `:-`, so an explicitly empty VLC_AUTOUPSCALE_ARGS means "no
# flags" instead of silently re-injecting the defaults.
ARGS="${VLC_AUTOUPSCALE_ARGS-$DEFAULT_ARGS}"

# SH-2: the argument string must word-split (these are separate VLC flags) but
# must NOT glob — a flag like --sub-file=*.srt would otherwise expand against
# the current directory and hand VLC a bogus extra input.
set -f

# shellcheck disable=SC2086  # word splitting is intentional; globbing is off
exec "$VLC_BIN" ${ARGS} -- "$@"
