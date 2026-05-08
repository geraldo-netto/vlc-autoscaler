#!/usr/bin/env bash
# Coverage summary for the vlc-autoscaler test suite.
#
# Reads .gcov files produced by `gcov` for the project's testable
# headers and TUs, prints per-file line coverage, and exits non-zero if
# any tracked file falls below the threshold.
#
# Usage: COV_DIR=build_dev/cov THRESHOLD=80 scripts/coverage_report.sh

set -u
COV_DIR="${COV_DIR:-build/cov}"
THRESHOLD="${THRESHOLD:-80}"

# The set of files we hold to the coverage bar. autoupscale.c and
# scaler_zimg.c are excluded because they require VLC headers (not
# directly unit-testable); their pure logic was extracted into the
# headers below precisely so it CAN be unit-tested.
TRACKED=(
    upscale_logic.h
    usm.h
    perfmon.h
    threading.h
    zimg_helpers.h
    chroma_classify.h
    content_probe.h
    scaler_pick_logic.h
    usm_pool.h
    usm_pool.c
)

if [ ! -d "$COV_DIR" ]; then
    echo "ERROR: $COV_DIR does not exist. Run 'make coverage' first." >&2
    exit 2
fi

# Aggregate executable lines and covered lines for each tracked file
# across ALL .gcov files (each test executable contributes one .gcov
# per included translation unit; we union them).
declare -A FILE_TOTAL FILE_COVERED

shopt -s nullglob
for f in "$COV_DIR"/*.gcov; do
    base=$(basename "$f" .gcov)
    keep=0
    for t in "${TRACKED[@]}"; do
        if [ "$base" = "$t" ]; then keep=1; break; fi
    done
    [ $keep -eq 1 ] || continue

    # gcov line format: "<count>:<lineno>:<source>"
    # count=='-' means non-executable (decl/comment/blank)
    # count=='#####' means uncovered
    # count=='=====' means uncovered (block flow)
    # count='<digit>+' means covered with that hit count
    runnable=$(awk -F: 'NF>=3 { c=$1; gsub(/^ +/,"",c);
        if (c != "-" && c != "") count++ } END { print count+0 }' "$f")
    covered=$(awk -F: 'NF>=3 { c=$1; gsub(/^ +/,"",c);
        if (c != "-" && c != "" && c != "#####" && c != "=====") count++ } END { print count+0 }' "$f")

    FILE_TOTAL[$base]=$(( ${FILE_TOTAL[$base]:-0} + runnable ))
    FILE_COVERED[$base]=$(( ${FILE_COVERED[$base]:-0} + covered ))
done

printf "%-30s %8s %8s %8s\n" "file" "lines" "covered" "pct"
printf "%-30s %8s %8s %8s\n" "------------------------------" "--------" "--------" "--------"

fail=0
total_lines=0
total_covered=0
for t in "${TRACKED[@]}"; do
    runnable=${FILE_TOTAL[$t]:-0}
    covered=${FILE_COVERED[$t]:-0}
    total_lines=$(( total_lines + runnable ))
    total_covered=$(( total_covered + covered ))
    if [ "$runnable" -gt 0 ]; then
        pct=$(awk "BEGIN{printf \"%.1f\", $covered*100.0/$runnable}")
    else
        pct="-"
    fi
    flag=""
    if [ "$runnable" -gt 0 ]; then
        below=$(awk "BEGIN{print ($covered*100.0/$runnable < $THRESHOLD)?1:0}")
        if [ "$below" = "1" ]; then flag=" <-- BELOW $THRESHOLD%"; fail=1; fi
    fi
    printf "%-30s %8d %8d %7s%%%s\n" "$t" "$runnable" "$covered" "$pct" "$flag"
done

if [ "$total_lines" -gt 0 ]; then
    overall=$(awk "BEGIN{printf \"%.1f\", $total_covered*100.0/$total_lines}")
else
    overall="-"
fi
printf "%-30s %8s %8s %8s\n" "------------------------------" "--------" "--------" "--------"
printf "%-30s %8d %8d %7s%%\n" "TOTAL (tracked)" "$total_lines" "$total_covered" "$overall"

if [ "$fail" -eq 1 ]; then
    echo
    echo "FAIL: at least one tracked file is below ${THRESHOLD}% coverage."
    exit 1
fi
echo
echo "OK: all tracked files >= ${THRESHOLD}% coverage."
exit 0
