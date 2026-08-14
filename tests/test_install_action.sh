#!/bin/sh
set -eu

repo_root=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)

cleanup() {
    if [ -n "${tmp:-}" ]; then
        rm -rf -- "$tmp"
        tmp=
    fi
}

on_signal() {
    signal=$1
    trap '' HUP INT TERM
    cleanup
    trap - "$signal"
    kill -s "$signal" "$$"
    exit 1
}

tmp=$(mktemp -d "${TMPDIR:-/tmp}/vlc-autoscaler-install-test.XXXXXXXXXX")
trap cleanup EXIT
trap 'on_signal HUP' HUP
trap 'on_signal INT' INT
trap 'on_signal TERM' TERM

home="${tmp}/home % dollar\$ back\\slash tick\` quote\" space"
bin_dir="${tmp}/bin"
mkdir -p "$home" "$bin_dir"

cat > "${bin_dir}/vlc" <<'EOF'
#!/bin/sh
if [ "${1:-}" = "--list" ]; then
    modules=${FAKE_VLC_MODULES-"autoupscale x264 avcodec"}
    for module in $modules; do
        printf '  %s fake module\n' "$module"
    done
    exit 0
fi
if [ -n "${FAKE_VLC_ARGS_FILE:-}" ]; then
    printf '%s\n' "$@" > "$FAKE_VLC_ARGS_FILE"
fi
EOF
chmod 0755 "${bin_dir}/vlc"

cat > "${bin_dir}/whereis" <<'EOF'
#!/bin/sh
printf 'vlc: %s/vlc\n' "$(dirname -- "$0")"
EOF
chmod 0755 "${bin_dir}/whereis"

cat > "${bin_dir}/update-desktop-database" <<'EOF'
#!/bin/sh
exit 0
EOF
chmod 0755 "${bin_dir}/update-desktop-database"

cat > "${bin_dir}/desktop-file-validate" <<'EOF'
#!/bin/sh
test -f "$1"
EOF
chmod 0755 "${bin_dir}/desktop-file-validate"

PATH="${bin_dir}:$PATH" HOME="$home" \
    sh "${repo_root}/scripts/install-vlc-autoupscale-action.sh" >/dev/null

python3 - "$home" <<'PY'
import os
import sys

home = sys.argv[1]
wrapper = os.path.join(home, ".local", "bin", "vlc-autoupscale")
vlc = os.path.join(os.path.dirname(home), "bin", "vlc")
desktop = os.path.join(home, ".local", "share", "applications",
                       "vlc-autoupscale.desktop")
action = os.path.join(home, ".local", "share", "nemo", "actions",
                      "vlc-autoupscale.nemo_action")


def exec_value(path):
    with open(path, encoding="utf-8") as fh:
        for line in fh:
            if line.startswith("Exec="):
                return line.split("=", 1)[1].rstrip("\n")
    raise AssertionError(f"missing Exec= in {path}")


def decode_general(value):
    out = []
    escapes = {"s": " ", "n": "\n", "t": "\t", "r": "\r", "\\": "\\"}
    i = 0
    while i < len(value):
        if value[i] == "\\" and i + 1 < len(value):
            i += 1
            out.append(escapes.get(value[i], value[i]))
        else:
            out.append(value[i])
        i += 1
    return "".join(out)


def expand_field_codes(value):
    out = []
    i = 0
    while i < len(value):
        if value[i] == "%" and i + 1 < len(value):
            code = value[i + 1]
            if code == "%":
                out.append("%")
                i += 2
                continue
            out.append("%" + code)
            i += 2
            continue
        out.append(value[i])
        i += 1
    return "".join(out)


def parse_first_quoted_arg(value):
    value = decode_general(value)
    value = expand_field_codes(value)
    if not value.startswith('"'):
        raise AssertionError(f"Exec value is not quoted: {value!r}")
    out = []
    i = 1
    while i < len(value):
        char = value[i]
        if char == '"':
            return "".join(out), value[i + 1:].strip()
        if char == "\\" and i + 1 < len(value):
            i += 1
            out.append(value[i])
        else:
            out.append(char)
        i += 1
    raise AssertionError(f"unterminated quoted Exec value: {value!r}")


desktop_program, desktop_tail = parse_first_quoted_arg(exec_value(desktop))
if desktop_program != wrapper:
    raise AssertionError((desktop, desktop_program, wrapper))
if desktop_tail != "%U":
    raise AssertionError((desktop, desktop_tail, "%U"))

action_program, action_tail = parse_first_quoted_arg(exec_value(action))
if action_program != wrapper:
    raise AssertionError((action, action_program, wrapper))
if action_tail != "--transcode-display %F":
    raise AssertionError((action, action_tail, "--transcode-display %F"))

with open(wrapper, encoding="utf-8") as fh:
    wrapper_text = fh.read()
expected_vlc_assignment = f'VLC_BIN="{vlc}"'
if expected_vlc_assignment not in wrapper_text:
    raise AssertionError((expected_vlc_assignment, wrapper_text))

print("install action command and Exec escaping OK")
PY

wrapper="${home}/.local/bin/vlc-autoupscale"
args_file="${tmp}/transcode-args"
VLC_AUTOUPSCALE_ARGS='' FAKE_VLC_ARGS_FILE="$args_file" \
    "$wrapper" --transcode-display "clip one.mkv" "--clip-two.mkv"

python3 - "$args_file" <<'PY'
import sys

with open(sys.argv[1], encoding="utf-8") as fh:
    actual = fh.read().splitlines()

sout = (
    "--sout=#transcode{vcodec=h264,acodec=mp4a,vb=10000,ab=128,"
    "venc=x264{preset=ultrafast,tune=zerolatency},"
    "vfilter=autoupscale}:display"
)
expected = [
    "--no-one-instance",
    "--no-one-instance-when-started-from-file",
    "--avcodec-hw=none",
    "--autoupscale-target=2",
    "--autoupscale-algo=3",
    "--autoupscale-usm=20",
    sout,
    "--",
    "clip one.mkv",
    "--clip-two.mkv",
]
if actual != expected:
    raise AssertionError((actual, expected))
PY

direct_args_file="${tmp}/direct-args"
FAKE_VLC_ARGS_FILE="$direct_args_file" \
    "$wrapper" "clip one.mkv" "--clip-two.mkv"

python3 - "$direct_args_file" <<'PY'
import sys

with open(sys.argv[1], encoding="utf-8") as fh:
    actual = fh.read().splitlines()

expected = [
    "--no-one-instance",
    "--no-one-instance-when-started-from-file",
    "--video-filter=autoupscale",
    "--autoupscale-target=2",
    "--autoupscale-algo=3",
    "--autoupscale-usm=20",
    "--",
    "clip one.mkv",
    "--clip-two.mkv",
]
if actual != expected:
    raise AssertionError((actual, expected))
PY
echo "direct profile one-instance guard OK"

warning_log="${tmp}/missing-modules.log"
FAKE_VLC_MODULES='' PATH="${bin_dir}:$PATH" HOME="$home" \
    sh "${repo_root}/scripts/install-vlc-autoupscale-action.sh" \
    >/dev/null 2>"$warning_log"
grep -q "Missing VLC module 'autoupscale'" "$warning_log"
grep -q "Missing VLC module 'x264'" "$warning_log"
grep -q "Missing VLC module 'avcodec'" "$warning_log"
grep -q "action requirements are incomplete" "$warning_log"
echo "transcode profile and module checks OK"
