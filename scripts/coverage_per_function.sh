#!/usr/bin/env bash
# Per-function coverage gate.
#
# Reads gcov JSON produced by the coverage Make target and fails if any
# function in a tracked source file is below $THRESHOLD% line coverage.
#
# Aggregation: each instrumented test/fuzz binary emits its own gcov data, so
# the same function appears multiple times. We keep the BEST (max) coverage
# across runs - a function is "tested" if at least one test exercises
# it adequately, regardless of which test it is.
#
# Usage: COV_DIR=build/cov THRESHOLD=90 scripts/coverage_per_function.sh

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
INPUT="$COV_DIR/gcov-json"

if [[ ! -d "$INPUT" ]]; then
    echo "ERROR: $INPUT not found. Run 'make coverage' first." >&2
    exit 2
fi

python3 - "$INPUT" "$THRESHOLD" "$SCOPE_FILE" "$REPO_ROOT" <<'PY'
import glob
import gzip
import json
import os
import sys

input_dir = sys.argv[1]
threshold = float(sys.argv[2])
scope_file = sys.argv[3]
repo_root = os.path.realpath(sys.argv[4])


def fail(message):
    print(f"ERROR: {message}", file=sys.stderr)
    sys.exit(2)


def repo_relative(path, base):
    absolute = path if os.path.isabs(path) else os.path.join(base, path)
    absolute = os.path.realpath(absolute)
    try:
        if os.path.commonpath((repo_root, absolute)) != repo_root:
            return None
    except ValueError:
        return None
    return os.path.relpath(absolute, repo_root)


if not os.path.isfile(scope_file):
    fail(f"coverage scope manifest not found: {scope_file}")

tracked_list = []
tracked = set()
tracked_basenames = {}
with open(scope_file, encoding="utf-8") as manifest:
    for raw in manifest:
        entry = raw.partition("#")[0].strip()
        if not entry:
            continue
        if os.path.isabs(entry):
            fail(f"coverage scope entry must be relative: {entry}")
        normalized = repo_relative(entry, repo_root)
        if normalized is None:
            fail(f"coverage scope entry escapes the repository: {entry}")
        if normalized in tracked:
            fail(f"duplicate coverage scope entry: {normalized}")
        basename = os.path.basename(normalized)
        if basename in tracked_basenames:
            fail("coverage scope basename collision: "
                 f"{tracked_basenames[basename]} and {normalized}")
        if not os.path.isfile(os.path.join(repo_root, normalized)):
            fail(f"coverage scope entry does not exist: {normalized}")
        tracked.add(normalized)
        tracked_basenames[basename] = normalized
        tracked_list.append(normalized)

if not tracked_list:
    fail(f"coverage scope manifest is empty: {scope_file}")

json_paths = glob.glob(os.path.join(input_dir, "**", "*.gcov.json.gz"),
                       recursive=True)
if not json_paths:
    print(f"ERROR: no gcov JSON found under {input_dir}", file=sys.stderr)
    sys.exit(2)

seen_files = set()
known_functions = set()
line_counts = {}
for path in json_paths:
    with gzip.open(path, "rt") as fh:
        report = json.load(fh)
    report_cwd = report.get("current_working_directory") or repo_root
    if not os.path.isabs(report_cwd):
        report_cwd = os.path.join(repo_root, report_cwd)
    for source in report.get("files", []):
        source_path = repo_relative(source.get("file", ""), report_cwd)
        if source_path not in tracked:
            continue
        seen_files.add(source_path)
        for fn in source.get("functions", []):
            name = fn.get("demangled_name") or fn.get("name")
            if name:
                known_functions.add((source_path, name))
        for line in source.get("lines", []):
            name = line.get("function_name")
            number = line.get("line_number")
            if not name or number is None:
                continue
            key = (source_path, name, int(number))
            line_counts[key] = max(line_counts.get(key, 0),
                                   int(line.get("count", 0)))

missing = sorted(tracked - seen_files)
if missing:
    print("ERROR: missing gcov JSON for: " + ", ".join(missing),
          file=sys.stderr)
    sys.exit(2)

stats = {}
for source_path, name in known_functions:
    counts = [count for (f, n, _), count in line_counts.items()
              if f == source_path and n == name]
    if not counts:
        continue
    covered = sum(count > 0 for count in counts)
    total = len(counts)
    stats[(source_path, name)] = (100.0 * covered / total, total)

if not stats:
    print("ERROR: no tracked functions discovered", file=sys.stderr)
    sys.exit(2)

missing_line_data = sorted(known_functions - set(stats))
if missing_line_data:
    labels = [f"{source_path}:{name}"
              for source_path, name in missing_line_data]
    print("ERROR: no executable lines found for: " + ", ".join(labels),
          file=sys.stderr)
    sys.exit(2)

missing_functions = sorted(source_path for source_path in tracked
                           if not any(f == source_path for f, _ in stats))
if missing_functions:
    print("ERROR: no functions discovered for: " +
          ", ".join(missing_functions), file=sys.stderr)
    sys.exit(2)

under = [(f, n, p, t) for (f, n), (p, t) in stats.items()
         if p < threshold]

# Two-column report: file, function, pct, total exec lines.
print(f"{'file':<32} {'function':<48} {'pct':>7} {'lines':>8}")
print(f"{'-' * 32} {'-' * 48} {'-' * 7} {'-' * 8}")
for (f, n), (p, t) in sorted(stats.items()):
    flag = "  <-- BELOW" if p < threshold else ""
    print(f"{f:<32} {n:<48} {p:6.1f}% {t:>8}{flag}")

print()
print(f"{len(stats)} tracked functions, {len(under)} below {threshold:g}%")

if under:
    print(f"\nFAIL: at least one function is below {threshold:g}% coverage.",
          file=sys.stderr)
    sys.exit(1)
print(f"OK: all tracked functions >= {threshold:g}% coverage.")
PY
