#!/bin/sh
# Install (or remove) a "VLC (AutoUpscale)" launcher, an "Open With" desktop
# entry, and a Nemo right-click action. The stock VLC entry is never touched:
# these are separate files with their own IDs.
#
#   scripts/install-vlc-autoupscale-action.sh              # install
#   scripts/install-vlc-autoupscale-action.sh --uninstall   # remove
set -eu

BIN_DIR="${HOME}/.local/bin"
APP_DIR="${HOME}/.local/share/applications"
ACTION_DIR="${HOME}/.local/share/nemo/actions"

WRAPPER="${BIN_DIR}/vlc-autoupscale"
DESKTOP="${APP_DIR}/vlc-autoupscale.desktop"
ACTION="${ACTION_DIR}/vlc-autoupscale.nemo_action"

SRC_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

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
Exec=${WRAPPER} %U
Icon=vlc
Terminal=false
Categories=AudioVideo;Player;Video;
MimeType=${VIDEO_MIMES}
StartupNotify=true
EOF
    update-desktop-database "$APP_DIR" 2>/dev/null || true
}

install_action() {
    mkdir -p "$ACTION_DIR"
    cat > "$ACTION" <<EOF
[Nemo Action]
Name=Play with VLC (AutoUpscale)
Comment=Play the selected media with the autoupscale video filter enabled
Exec=${WRAPPER} %F
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
