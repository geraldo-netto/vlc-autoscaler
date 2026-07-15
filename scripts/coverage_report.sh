#!/usr/bin/env bash
# Coverage summary for the vlc-autoscaler test suite.
#
# Reads .gcov files produced by `gcov` for the project's testable
# headers and TUs, prints per-file line coverage, and exits non-zero if
# any tracked file falls below the threshold.
#
# Usage: COV_DIR=build_dev/cov THRESHOLD=90 scripts/coverage_report.sh

set -euo pipefail
SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd -- "$SCRIPT_DIR/.." && pwd)"
SCOPE_FILE="${COVERAGE_SCOPE_FILE:-$SCRIPT_DIR/coverage_scope.txt}"
COV_DIR="${COV_DIR:-$REPO_ROOT/build/cov}"
if [[ -z "${THRESHOLD:-}" ]]; then
    echo "ERROR: THRESHOLD is required (normally set by make coverage)." >&2
    exit 2
fi

if [[ "$SCOPE_FILE" != /* ]]; then SCOPE_FILE="$REPO_ROOT/$SCOPE_FILE"; fi
if [[ "$COV_DIR" != /* ]]; then COV_DIR="$REPO_ROOT/$COV_DIR"; fi

if [[ ! -f "$SCOPE_FILE" ]]; then
    echo "ERROR: coverage scope manifest not found: $SCOPE_FILE" >&2
    exit 2
fi

TRACKED=()
declare -A SCOPE_SEEN SCOPE_BASENAME_SEEN
while IFS= read -r entry || [[ -n "$entry" ]]; do
    entry="${entry%%#*}"
    entry="${entry#"${entry%%[![:space:]]*}"}"
    entry="${entry%"${entry##*[![:space:]]}"}"
    [[ -z "$entry" ]] && continue
    if [[ "$entry" = /* ]]; then
        echo "ERROR: coverage scope entry must be relative: $entry" >&2
        exit 2
    fi
    normalized=$(realpath -m --relative-to="$REPO_ROOT" -- "$REPO_ROOT/$entry")
    if [[ "$normalized" == ".." || "$normalized" == ../* ]]; then
        echo "ERROR: coverage scope entry escapes the repository: $entry" >&2
        exit 2
    fi
    if [[ -n "${SCOPE_SEEN[$normalized]+present}" ]]; then
        echo "ERROR: duplicate coverage scope entry: $normalized" >&2
        exit 2
    fi
    base=${normalized##*/}
    if [[ -n "${SCOPE_BASENAME_SEEN[$base]+present}" ]]; then
        echo "ERROR: coverage scope basename collision: " \
             "${SCOPE_BASENAME_SEEN[$base]} and $normalized" >&2
        exit 2
    fi
    if [[ ! -f "$REPO_ROOT/$normalized" ]]; then
        echo "ERROR: coverage scope entry does not exist: $normalized" >&2
        exit 2
    fi
    SCOPE_SEEN[$normalized]=1
    SCOPE_BASENAME_SEEN[$base]=$normalized
    TRACKED+=("$normalized")
done < "$SCOPE_FILE"

if [[ ${#TRACKED[@]} -eq 0 ]]; then
    echo "ERROR: coverage scope manifest is empty: $SCOPE_FILE" >&2
    exit 2
fi

gcov_source_path()
{
    local gcov_file=$1 header source absolute
    header=$(awk 'index($0, ":Source:") { print; exit }' "$gcov_file")
    [[ -n "$header" ]] || return 1
    source=${header#*:Source:}
    if [[ "$source" = /* ]]; then absolute=$source; else absolute=$REPO_ROOT/$source; fi
    realpath -m --relative-to="$REPO_ROOT" -- "$absolute"
}

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
    # subdirs written by the Makefile. gcov names artifacts by basename, so
    # verify the embedded source path before accepting a candidate.
    base=${t##*/}
    files=( "$COV_DIR/$base.gcov" "$COV_DIR"/gcov/*/"$base.gcov" )
    present=()
    for f in "${files[@]}"; do
        [[ -f "$f" ]] || continue
        source=$(gcov_source_path "$f") || continue
        [[ "$source" == "$t" ]] && present+=("$f")
    done
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
