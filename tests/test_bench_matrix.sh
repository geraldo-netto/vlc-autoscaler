#!/bin/sh
set -eu

repo_root=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
matrix="$repo_root/scripts/bench_matrix.sh"
tmp=$(mktemp -d "${TMPDIR:-/tmp}/vlc-autoscaler-bench-matrix.XXXXXX")
fake="$tmp/fake-benchmark"

cleanup() {
    rm -rf -- "$tmp"
}
trap 'cleanup; exit 129' HUP
trap 'cleanup; exit 130' INT
trap 'cleanup; exit 143' TERM
trap cleanup EXIT

fail() {
    echo "benchmark matrix test failed: $*" >&2
    exit 1
}

cat >"$fake" <<'EOF'
#!/bin/sh
set -eu

requested=$1
width=$2
height=$3
frames=$4
amount=$5
fill=$6
effective=$requested
if test "$effective" -gt 12; then
    effective=12
fi

state=${BENCH_FAKE_STATE:?}
count=0
if test -s "$state"; then
    read -r count <"$state"
fi
count=$((count + 1))
printf '%s\n' "$count" >"$state"

case ${BENCH_FAKE_MODE:-valid} in
    fail) exit 9 ;;
    malformed)
        printf '%s,%s\n' "$requested" "$effective"
        exit 0
        ;;
    multiple_lines)
        printf 'one\ntwo\n'
        exit 0
        ;;
    mismatch_requested) requested=$((requested + 1)) ;;
    mismatch_width) width=$((width + 1)) ;;
    mismatch_height) height=$((height + 1)) ;;
    mismatch_frames) frames=$((frames + 1)) ;;
    mismatch_amount) amount=$((amount + 1)) ;;
    mismatch_fill) fill=flat ;;
    noncanonical_effective) effective=01 ;;
    excessive_effective) effective=$((requested + 1)) ;;
    huge_effective) effective=9999999999999999999999999999999999999999 ;;
    invalid_timing) timing=not-a-number ;;
    invalid_third_timing)
        if test "$count" -eq 3; then
            timing=not-a-number
        fi
        ;;
    inconsistent_effective)
        if test "$requested" -eq 4 && test "$((count % 3))" -eq 2; then
            effective=3
        fi
        ;;
    valid) ;;
    *) exit 10 ;;
esac

if test -z "${timing:-}"; then
    case $((count % 3)) in
        1) timing=30.00 ;;
        2) timing=10.00 ;;
        0) timing=20.00 ;;
    esac
fi
printf '%s,%s,%s,%s,%s,%s,%s,%s\n' \
    "$requested" "$effective" "$width" "$height" "$frames" "$amount" \
    "$fill" "$timing"
EOF
chmod 0755 "$fake"

run_matrix() {
    mode=$1
    : >"$tmp/state"
    BENCH_FAKE_MODE=$mode BENCH_FAKE_STATE="$tmp/state" \
        "$matrix" "$fake" 7 19 rand
}

expect_failure() {
    mode=$1
    expected=$2
    set +e
    run_matrix "$mode" >"$tmp/stdout" 2>"$tmp/stderr"
    status=$?
    set -e
    test "$status" -ne 0 || fail "$mode unexpectedly succeeded"
    grep -Fq "$expected" "$tmp/stderr" || {
        cat "$tmp/stderr" >&2
        fail "$mode did not report '$expected'"
    }
}

run_matrix valid >"$tmp/valid.csv" 2>"$tmp/stderr" || {
    cat "$tmp/stderr" >&2
    fail "valid capped benchmark was rejected"
}
test ! -s "$tmp/stderr" || fail "valid benchmark wrote stderr"
test "$(wc -l <"$tmp/valid.csv")" -eq 72 || fail "expected 72 matrix rows"
expected_first_group='fake-benchmark,1,1,1280,720,7,19,rand,raw,1,30.00
fake-benchmark,1,1,1280,720,7,19,rand,raw,2,10.00
fake-benchmark,1,1,1280,720,7,19,rand,raw,3,20.00
fake-benchmark,1,1,1280,720,7,19,rand,median,0,20.00'
test "$(sed -n '1,4p' "$tmp/valid.csv")" = "$expected_first_group" ||
    fail "first matrix group is not in raw acquisition order followed by median"
awk -F, '
    NF != 11 { exit 1 }
    $2 == 16 && $3 != 12 { exit 1 }
    $2 == 20 && $3 != 12 { exit 1 }
    $6 != 7 || $7 != 19 || $8 != "rand" { exit 1 }
    $9 == "raw" && ($10 < 1 || $10 > 3) { exit 1 }
    $9 == "raw" { raw++ }
    $9 == "median" && $10 != 0 { exit 1 }
    $9 == "median" && $11 != "20.00" { exit 1 }
    $9 == "median" { median++ }
    $9 != "raw" && $9 != "median" { exit 1 }
    END { if (raw != 54 || median != 18) exit 1 }
' "$tmp/valid.csv" || fail "valid matrix output has the wrong schema or values"
grep -Fq 'fake-benchmark,16,12,1280,720,7,19,rand,raw,1,30.00' "$tmp/valid.csv" ||
    fail "capped 16-thread row was not labeled with 12 effective workers"
grep -Fq 'fake-benchmark,20,12,2560,1440,7,19,rand,median,0,20.00' "$tmp/valid.csv" ||
    fail "capped 20-thread row was not labeled with 12 effective workers"

expect_failure fail 'benchmark exited with status 9'
expect_failure malformed 'expected 8 CSV fields'
expect_failure multiple_lines 'benchmark emitted multiple lines'
expect_failure mismatch_requested 'requested_threads mismatch'
expect_failure mismatch_width 'width mismatch'
expect_failure mismatch_height 'height mismatch'
expect_failure mismatch_frames 'frames mismatch'
expect_failure mismatch_amount 'amount mismatch'
expect_failure mismatch_fill 'fill mismatch'
expect_failure noncanonical_effective 'invalid effective_threads'
expect_failure excessive_effective 'exceeds requested_threads'
expect_failure huge_effective 'exceeds requested_threads'
expect_failure invalid_timing 'invalid timing'
expect_failure invalid_third_timing 'invalid timing'
test ! -s "$tmp/stdout" || fail "failed group emitted partial output"
expect_failure inconsistent_effective 'effective_threads changed'

echo "benchmark matrix checks OK"
