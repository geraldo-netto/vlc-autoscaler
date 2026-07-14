#!/bin/sh
# Launch VLC with the autoupscale video filter preconfigured.
# Installed as ~/.local/bin/vlc-autoupscale by install-vlc-autoupscale-action.sh.
#
# Override the filter tuning without editing this file:
#   VLC_AUTOUPSCALE_ARGS="--autoupscale-target=4 --autoupscale-algo=2" vlc-autoupscale clip.mkv
set -eu

DEFAULT_ARGS="--video-filter=autoupscale --autoupscale-target=2 --autoupscale-algo=3 --autoupscale-usm=20"

# Word-splitting of the argument string is intentional: these are VLC flags.
# shellcheck disable=SC2086
exec vlc ${VLC_AUTOUPSCALE_ARGS:-$DEFAULT_ARGS} -- "$@"
