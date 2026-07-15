#!/bin/sh
set -eu

repo_root=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
text_parser="$repo_root/scripts/gcov_line_totals.awk"
function_parser="$repo_root/scripts/coverage_per_function.sh"
tmp=$(mktemp -d "${TMPDIR:-/tmp}/vlc-autoscaler-coverage-parser.XXXXXX")

cleanup() {
    rm -rf -- "$tmp"
}
trap 'cleanup; exit 129' HUP
trap 'cleanup; exit 130' INT
trap 'cleanup; exit 143' TERM
trap cleanup EXIT

fail() {
    echo "coverage parser test failed: $*" >&2
    exit 1
}

expect_failure() {
    expected_status=$1
    expected_stderr=$2
    shift 2

    set +e
    "$@" >"$tmp/stdout" 2>"$tmp/stderr"
    status=$?
    set -e

    test "$status" -eq "$expected_status" ||
        fail "expected status $expected_status, got $status"
    test ! -s "$tmp/stdout" || fail "failure wrote unexpected stdout"
    printf '%s\n' "$expected_stderr" >"$tmp/expected-stderr"
    cmp -s "$tmp/expected-stderr" "$tmp/stderr" || {
        echo "expected stderr:" >&2
        cat "$tmp/expected-stderr" >&2
        echo "actual stderr:" >&2
        cat "$tmp/stderr" >&2
        exit 1
    }
}

valid_text="$tmp/valid.gcov"
cat >"$valid_text" <<'EOF'
        -:    0:Source:src/upscale_logic.h
    #####:    1:int never_called;
    =====:    2:int exceptional_path;
        0:    3:int zero_hits;
     000*:    4:int starred_zero_hits;
       1*:    5:int starred_hit;
00000002:    6:int multiple_hits;
EOF
totals=$(awk -f "$text_parser" "$valid_text")
test "$totals" = "6 2" || fail "unexpected valid text totals: $totals"

for invalid_count in -1 +1 1.0 1x '1**' '#####*'; do
    invalid_text="$tmp/invalid.gcov"
    printf '%s:1:int corrupt;\n' "$invalid_count" >"$invalid_text"
    expect_failure 2 \
        "ERROR: malformed gcov count at $invalid_text:1: $invalid_count" \
        awk -f "$text_parser" "$invalid_text"
done

scope="$tmp/scope.txt"
cov_dir="$tmp/cov"
json_dir="$cov_dir/gcov-json/case"
json_path="$json_dir/case.gcov.json.gz"
mkdir -p -- "$json_dir"
printf '%s\n' 'src/upscale_logic.h' >"$scope"

write_json_case() {
    python3 - "$json_path" "$repo_root" "$1" <<'PY'
import gzip
import json
import sys

path, repo_root, case = sys.argv[1:]
values = {
    "valid": 1,
    "bool": True,
    "string": "1",
    "float": 1.0,
    "negative": -1,
}
line = {
    "function_name": "probe",
    "line_number": 1,
}
if case != "missing":
    line["count"] = values[case]
report = {
    "current_working_directory": repo_root,
    "files": [{
        "file": "src/upscale_logic.h",
        "functions": [{"name": "probe"}],
        "lines": [line],
    }],
}
with gzip.open(path, "wt", encoding="utf-8") as fh:
    json.dump(report, fh)
PY
}

write_json_case valid
env COV_DIR="$cov_dir" THRESHOLD=0 COVERAGE_SCOPE_FILE="$scope" \
    "$function_parser" >"$tmp/stdout" 2>"$tmp/stderr" ||
    fail "valid JSON count was rejected"
test ! -s "$tmp/stderr" || fail "valid JSON count wrote stderr"
grep -Fq 'OK: all tracked functions >= 0% coverage.' "$tmp/stdout" ||
    fail "valid JSON count did not produce the success summary"

expected_json_error='ERROR: invalid coverage count for src/upscale_logic.h:probe:1; expected a nonnegative integer'
for invalid_case in bool string float missing negative; do
    write_json_case "$invalid_case"
    expect_failure 2 "$expected_json_error" \
        env COV_DIR="$cov_dir" THRESHOLD=0 COVERAGE_SCOPE_FILE="$scope" \
        "$function_parser"
done

echo "coverage parser checks OK"
