#!/bin/sh
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
tmp=${TMPDIR:-/tmp}/vlc-autoscaler-install-test.$$
home="${tmp}/home % dollar\$ back\\slash tick\` quote\" space"
bin_dir="${tmp}/bin"

cleanup() {
    rm -rf "$tmp"
}
trap cleanup EXIT HUP INT TERM

mkdir -p "$home" "$bin_dir"

cat > "${bin_dir}/vlc" <<'EOF'
#!/bin/sh
if [ "${1:-}" = "--list" ]; then
    echo autoupscale
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


for path, field_code in ((desktop, "%U"), (action, "%F")):
    program, tail = parse_first_quoted_arg(exec_value(path))
    if program != wrapper:
        raise AssertionError((path, program, wrapper))
    if tail != field_code:
        raise AssertionError((path, tail, field_code))

with open(wrapper, encoding="utf-8") as fh:
    wrapper_text = fh.read()
expected_vlc_assignment = f'VLC_BIN="{vlc}"'
if expected_vlc_assignment not in wrapper_text:
    raise AssertionError((expected_vlc_assignment, wrapper_text))

print("install action Exec escaping OK")
PY
