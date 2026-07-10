#!/usr/bin/env bash
# Coverage summary for the vlc-autoscaler test suite.
#
# Reads .gcov files produced by `gcov` for the project's testable
# headers and TUs, prints per-file line coverage, and exits non-zero if
# any tracked file falls below the threshold.
#
# Usage: COV_DIR=build_dev/cov THRESHOLD=80 scripts/coverage_report.sh

set -euo pipefail
COV_DIR="${COV_DIR:-build/cov}"
THRESHOLD="${THRESHOLD:-80}"

# The set of files we hold to the coverage bar. autoupscale.c and
# scaler_zimg.c are excluded because they require VLC headers (not
# directly unit-testable); their pure logic was extracted into the
# headers below precisely so it CAN be unit-tested.
TRACKED=(
    upscale_logic.h
    cli_parse.h
    usm.h
    perfmon.h
    threading.h
    zimg_helpers.h
    chroma_classify.h
    scaler_zimg_chroma.h
    content_probe.h
    scaler_pick_logic.h
    scaler_status.h
    scaler_swscale.c
    picture_view.h
    usm_pool.c
)

if [[ ! -d "$COV_DIR" ]]; then
    echo "ERROR: $COV_DIR does not exist. Run 'make coverage' first." >&2
    exit 2
fi

# Aggregate executable/covered lines for each tracked file across ALL
# .gcov files. A header compiled into N test binaries produces N same-named
# .gcov files (kept in per-binary subdirs by `make coverage`); a line is
# COVERED if any binary covered it and RUNNABLE if any binary marked it
# executable. We therefore union PER LINE (keyed by line number), not by
# summing counts — summing would multiply-count a header's lines once per
# binary and report bogus totals.
declare -A FILE_TOTAL FILE_COVERED
missing=0

shopt -s nullglob globstar
for t in "${TRACKED[@]}"; do
    # Gather every .gcov for this source: legacy flat layout + per-binary
    # subdirs written by the Makefile.
    files=( "$COV_DIR/$t.gcov" "$COV_DIR"/gcov/*/"$t.gcov" )
    present=()
    for f in "${files[@]}"; do [[ -f "$f" ]] && present+=("$f"); done
    if [[ ${#present[@]} -eq 0 ]]; then
        echo "ERROR: no coverage artifact for $t" >&2
        missing=1
        continue
    fi

    # gcov line format: "<count>:<lineno>:<source>"
    #   '-'             non-executable (decl/comment/blank)
    #   '#####'/'=====' executable but uncovered
    #   '<digit>+'      covered with that hit count
    if ! counts=$(awk -F: '
        NF>=3 {
            ln=$2; gsub(/^ +/,"",ln);
            c=$1;  gsub(/^ +/,"",c);
            if (c=="-" || c=="") next;
            run[ln]=1;
            if (c!="#####" && c!="=====") cov[ln]=1;
        }
        END {
            t=0; cv=0;
            for (l in run) { t++; if (l in cov) cv++; }
            print t, cv;
        }' "${present[@]}"); then
        echo "ERROR: failed to aggregate coverage for $t" >&2
        missing=1
        continue
    fi
    read -r runnable covered <<< "$counts"
    if [[ ! "$runnable" =~ ^[0-9]+$ || ! "$covered" =~ ^[0-9]+$ ]]; then
        echo "ERROR: malformed coverage totals for $t" >&2
        missing=1
        continue
    fi

    FILE_TOTAL[$t]=$(( runnable + 0 ))
    FILE_COVERED[$t]=$(( covered + 0 ))
    if [[ "$runnable" -eq 0 ]]; then
        echo "ERROR: no executable lines found for $t" >&2
        missing=1
    fi
done

printf "%-30s %8s %8s %8s\n" "file" "lines" "covered" "pct"
printf "%-30s %8s %8s %8s\n" "------------------------------" "--------" "--------" "--------"

fail=$missing
total_lines=0
total_covered=0
for t in "${TRACKED[@]}"; do
    runnable=${FILE_TOTAL[$t]:-0}
    covered=${FILE_COVERED[$t]:-0}
    total_lines=$(( total_lines + runnable ))
    total_covered=$(( total_covered + covered ))
    if [[ "$runnable" -gt 0 ]]; then
        pct=$(awk "BEGIN{printf \"%.1f\", $covered*100.0/$runnable}")
    else
        pct="-"
    fi
    flag=""
    if [[ "$runnable" -gt 0 ]]; then
        below=$(awk "BEGIN{print ($covered*100.0/$runnable < $THRESHOLD)?1:0}")
        if [[ "$below" == "1" ]]; then flag=" <-- BELOW $THRESHOLD%"; fail=1; fi
    fi
    printf "%-30s %8d %8d %7s%%%s\n" "$t" "$runnable" "$covered" "$pct" "$flag"
done

if [[ "$total_lines" -gt 0 ]]; then
    overall=$(awk "BEGIN{printf \"%.1f\", $total_covered*100.0/$total_lines}")
else
    overall="-"
fi
printf "%-30s %8s %8s %8s\n" "------------------------------" "--------" "--------" "--------"
printf "%-30s %8d %8d %7s%%\n" "TOTAL (tracked)" "$total_lines" "$total_covered" "$overall"

if [[ "$fail" -eq 1 ]]; then
    echo
    echo "FAIL: at least one tracked file is below ${THRESHOLD}% coverage."
    exit 1
fi
echo
echo "OK: all tracked files >= ${THRESHOLD}% coverage."
exit 0
