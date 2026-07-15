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

SRC_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

desktop_exec_quote() {
    cr=$(printf '\r')
    case $1 in
        *'
'*|*"$cr"*)
            echo "error: desktop Exec paths cannot contain newlines" >&2
            exit 1
            ;;
    esac
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
EXEC_PATH=$(desktop_exec_quote "$WRAPPER")

VIDEO_MIMES="video/mp4;video/x-matroska;video/x-msvideo;video/quicktime;video/webm;video/mpeg;video/x-ms-wmv;video/x-flv;video/mp2t;"
VIDEO_EXTS="mp4;mkv;avi;mov;webm;m4v;ts;mpg;mpeg;wmv;flv;"

uninstall() {
    rm -f "$WRAPPER" "$DESKTOP" "$ACTION"
    [ -d "$APP_DIR" ] && update-desktop-database "$APP_DIR" 2>/dev/null || true
    echo "Removed VLC AutoUpscale launcher, desktop entry and Nemo action."
}

install_wrapper() {
    mkdir -p "$BIN_DIR"
    install -m 0755 "${SRC_DIR}/vlc-autoupscale.sh" "$WRAPPER"
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
Exec=${EXEC_PATH} %U
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
Name=Play with VLC (AutoUpscale)
Comment=Play the selected media with the autoupscale video filter enabled
Exec=${EXEC_PATH} %F
Icon-Name=vlc
Selection=notnone
Extensions=${VIDEO_EXTS}
Quote=double
Dependencies=vlc;
EOF
}

check_plugin() {
    if ! command -v vlc >/dev/null 2>&1; then
        echo "Warning: vlc not found in PATH." >&2
        return
    fi
    if ! vlc --list 2>/dev/null | grep -q autoupscale; then
        echo "Warning: VLC does not list the 'autoupscale' module." >&2
        echo "         Build and install the plugin first (see README.md)." >&2
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

install_wrapper
install_desktop
install_action
check_plugin

echo "Installed:"
echo "  ${WRAPPER}"
echo "  ${DESKTOP}"
echo "  ${ACTION}"
echo "The stock VLC entry is unchanged; both appear side by side in 'Open With'."
