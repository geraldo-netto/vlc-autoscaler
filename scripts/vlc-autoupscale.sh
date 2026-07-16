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

TARGET_ARG="--autoupscale-target=2"
ALGO_ARG="--autoupscale-algo=3"
USM_ARG="--autoupscale-usm=20"
DEFAULT_ARGS="--video-filter=autoupscale ${TARGET_ARG} ${ALGO_ARG} ${USM_ARG}"
TRANSCODE_SOUT="#transcode{vcodec=h264,acodec=mp4a,vb=10000,ab=128,venc=x264{preset=ultrafast,tune=zerolatency},vfilter=autoupscale}:display"
TRANSCODE_MODULES="autoupscale x264 avcodec"
VLC_BIN=vlc

warn_missing_module() {
    case $1 in
        autoupscale)
            echo "Missing VLC module 'autoupscale'; build and install the plugin." >&2
            ;;
        x264)
            echo "Missing VLC module 'x264'; install VLC x264 encoder support." >&2
            ;;
        avcodec)
            echo "Missing VLC module 'avcodec'; install VLC FFmpeg encoder support." >&2
            ;;
    esac
}

check_transcode_display() {
    if ! module_list=$("$VLC_BIN" --list 2>/dev/null); then
        echo "Unable to inspect VLC modules with '$VLC_BIN --list'." >&2
        return 1
    fi

    missing=0
    for module in $TRANSCODE_MODULES; do
        if ! printf '%s\n' "$module_list" |
                grep -Eq "^[[:space:]]*${module}([[:space:]]|$)"; then
            warn_missing_module "$module"
            missing=1
        fi
    done
    return "$missing"
}

case "${1:-}" in
    --check-transcode-display)
        check_transcode_display
        exit 0
        ;;
    --transcode-display)
        shift
        exec "$VLC_BIN" --no-one-instance \
            --no-one-instance-when-started-from-file --avcodec-hw=none \
            "$TARGET_ARG" "$ALGO_ARG" "$USM_ARG" \
            "--sout=${TRANSCODE_SOUT}" -- "$@"
        ;;
esac

# SH-3: `-` not `:-`, so an explicitly empty VLC_AUTOUPSCALE_ARGS means "no
# flags" instead of silently re-injecting the defaults.
ARGS="${VLC_AUTOUPSCALE_ARGS-$DEFAULT_ARGS}"

# SH-2: the argument string must word-split (these are separate VLC flags) but
# must NOT glob — a flag like --sub-file=*.srt would otherwise expand against
# the current directory and hand VLC a bogus extra input.
set -f

# shellcheck disable=SC2086  # word splitting is intentional; globbing is off
exec "$VLC_BIN" ${ARGS} -- "$@"
