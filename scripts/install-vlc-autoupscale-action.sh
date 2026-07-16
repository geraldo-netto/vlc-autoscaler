#!/bin/sh
# Install (or remove) a "VLC (AutoUpscale)" launcher, an "Open With" desktop
# entry, and a Nemo right-click action. The stock VLC entry is never touched:
# these are separate files with their own IDs.
#
#   scripts/install-vlc-autoupscale-action.sh              # install
#   scripts/install-vlc-autoupscale-action.sh --uninstall   # remove
set -eu

# SH-4: `set -u` catches an UNSET HOME but not an empty one, which would make
# every derived path root-relative (/.local/...) — uninstall would then rm -f
# three paths that do not exist, exit 0, and report success while the real
# files under the user's home were untouched.
: "${HOME:?HOME must be set to a non-empty path}"

BIN_DIR="${HOME}/.local/bin"
APP_DIR="${HOME}/.local/share/applications"
ACTION_DIR="${HOME}/.local/share/nemo/actions"

WRAPPER="${BIN_DIR}/vlc-autoupscale"
DESKTOP="${APP_DIR}/vlc-autoupscale.desktop"
ACTION="${ACTION_DIR}/vlc-autoupscale.nemo_action"

SRC_DIR=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)

find_vlc_binary() {
    command -v whereis >/dev/null 2>&1 || return 1
    locations=$(whereis -b vlc) || return 1
    for candidate in $locations; do
        [ "$candidate" = "vlc:" ] && continue
        if [ -x "$candidate" ]; then
            printf '%s\n' "$candidate"
            return 0
        fi
    done
    return 1
}

desktop_exec_quote() {
    cr=$(printf '\r')
    case $1 in
        *'
'*|*"$cr"*)
            echo "error: desktop Exec paths cannot contain newlines" >&2
            exit 1
            ;;
    esac
    # shellcheck disable=SC2016
    escaped=$(printf '%s' "$1" | sed \
        -e 's/\\/\\\\\\\\/g' \
        -e 's/"/\\\\"/g' \
        -e 's/`/\\\\`/g' \
        -e 's/[$]/\\\\$/g' \
        -e 's/%/%%/g')
    printf '"%s"' "$escaped"
}

# Desktop Entry string decoding runs before Exec command-line parsing. Emit
# both layers: doubled backslashes survive the general-string pass, backslash
# escapes protect the command parser inside quotes, and %% prevents field-code
# expansion inside the literal wrapper path.
WRAPPER_EXEC_PATH=$(desktop_exec_quote "$WRAPPER")

VIDEO_MIMES="video/mp4;video/x-matroska;video/x-msvideo;video/quicktime;video/webm;video/mpeg;video/x-ms-wmv;video/x-flv;video/mp2t;"
VIDEO_EXTS="mp4;mkv;avi;mov;webm;m4v;ts;mpg;mpeg;wmv;flv;"

uninstall() {
    rm -f "$WRAPPER" "$DESKTOP" "$ACTION"
    if [ -d "$APP_DIR" ]; then
        update-desktop-database "$APP_DIR" 2>/dev/null || true
    fi
    echo "Removed VLC AutoUpscale launcher, desktop entry and Nemo action."
}

install_wrapper() {
    mkdir -p "$BIN_DIR"
    escaped_vlc_bin=$(printf '%s' "$VLC_BIN" | sed 's/[\\&|]/\\&/g')
    sed "s|^VLC_BIN=vlc$|VLC_BIN=\"${escaped_vlc_bin}\"|" \
        "${SRC_DIR}/vlc-autoupscale.sh" > "$WRAPPER"
    chmod 0755 "$WRAPPER"
}

install_desktop() {
    mkdir -p "$APP_DIR"
    cat > "$DESKTOP" <<EOF
[Desktop Entry]
Type=Application
Version=1.0
Name=VLC (AutoUpscale)
GenericName=Media Player
Comment=Play media in VLC with the autoupscale video filter enabled
Exec=${WRAPPER_EXEC_PATH} %U
Icon=vlc
Terminal=false
Categories=AudioVideo;Player;Video;
MimeType=${VIDEO_MIMES}
StartupNotify=true
EOF
    command -v desktop-file-validate >/dev/null 2>&1 &&
        desktop-file-validate "$DESKTOP"
    update-desktop-database "$APP_DIR" 2>/dev/null || true
}

install_action() {
    mkdir -p "$ACTION_DIR"
    cat > "$ACTION" <<EOF
[Nemo Action]
Name=Play with VLC (AutoUpscale 1080p)
Comment=Upscale through VLC's compatible transcode display path to preserve the larger frame size
Exec=${WRAPPER_EXEC_PATH} --transcode-display %F
Icon-Name=vlc
Selection=notnone
Extensions=${VIDEO_EXTS}
Quote=double
Dependencies=vlc;
EOF
}

check_action_profile() {
    if ! "$WRAPPER" --check-transcode-display; then
        echo "Warning: VLC AutoUpscale action requirements are incomplete." >&2
        echo "         Install the modules listed above, then rerun this installer." >&2
    fi
}

case "${1:-}" in
    --uninstall|-u)
        uninstall
        exit 0
        ;;
    "") ;;
    *)
        echo "usage: $0 [--uninstall]" >&2
        exit 2
        ;;
esac

VLC_BIN=$(find_vlc_binary) || {
    echo "error: whereis did not find an executable VLC binary" >&2
    exit 1
}

install_wrapper
install_desktop
install_action
check_action_profile

echo "Installed:"
echo "  ${WRAPPER}"
echo "  ${DESKTOP}"
echo "  ${ACTION}"
echo "The stock VLC entry is unchanged; both appear side by side in 'Open With'."
