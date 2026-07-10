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
# Usage: COV_DIR=build/cov THRESHOLD=80 scripts/coverage_per_function.sh

set -euo pipefail
COV_DIR="${COV_DIR:-build/cov}"
THRESHOLD="${THRESHOLD:-80}"
INPUT="$COV_DIR/gcov-json"

if [[ ! -d "$INPUT" ]]; then
    echo "ERROR: $INPUT not found. Run 'make coverage' first." >&2
    exit 2
fi

# Tracked files - same set as the per-file gate (scripts/coverage_report.sh).
TRACKED="upscale_logic.h usm.h perfmon.h threading.h zimg_helpers.h \
chroma_classify.h scaler_zimg_chroma.h content_probe.h \
scaler_pick_logic.h scaler_status.h scaler_swscale.c usm_pool.c"

python3 - "$INPUT" "$THRESHOLD" "$TRACKED" <<'PY'
import glob
import gzip
import json
import os
import sys

input_dir, threshold, tracked_str = sys.argv[1], float(sys.argv[2]), sys.argv[3]
tracked = set(tracked_str.split())
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
    for source in report.get("files", []):
        base = os.path.basename(source.get("file", ""))
        if base not in tracked:
            continue
        seen_files.add(base)
        for fn in source.get("functions", []):
            name = fn.get("demangled_name") or fn.get("name")
            if name:
                known_functions.add((base, name))
        for line in source.get("lines", []):
            name = line.get("function_name")
            number = line.get("line_number")
            if not name or number is None:
                continue
            key = (base, name, int(number))
            line_counts[key] = max(line_counts.get(key, 0),
                                   int(line.get("count", 0)))

missing = sorted(tracked - seen_files)
if missing:
    print("ERROR: missing gcov JSON for: " + ", ".join(missing),
          file=sys.stderr)
    sys.exit(2)

stats = {}
for base, name in known_functions:
    counts = [count for (f, n, _), count in line_counts.items()
              if f == base and n == name]
    if not counts:
        continue
    covered = sum(count > 0 for count in counts)
    total = len(counts)
    stats[(base, name)] = (100.0 * covered / total, total)

if not stats:
    print("ERROR: no tracked functions discovered", file=sys.stderr)
    sys.exit(2)

missing_line_data = sorted(known_functions - set(stats))
if missing_line_data:
    labels = [f"{base}:{name}" for base, name in missing_line_data]
    print("ERROR: no executable lines found for: " + ", ".join(labels),
          file=sys.stderr)
    sys.exit(2)

missing_functions = sorted(base for base in tracked
                           if not any(f == base for f, _ in stats))
if missing_functions:
    print("ERROR: no functions discovered for: " +
          ", ".join(missing_functions), file=sys.stderr)
    sys.exit(2)

under = [(f, n, p, t) for (f, n), (p, t) in stats.items()
         if p < threshold]

# Two-column report: file, function, pct, total exec lines.
print(f"{'file':<25} {'function':<48} {'pct':>7} {'lines':>8}")
print(f"{'-' * 25} {'-' * 48} {'-' * 7} {'-' * 8}")
for (f, n), (p, t) in sorted(stats.items()):
    flag = "  <-- BELOW" if p < threshold else ""
    print(f"{f:<25} {n:<48} {p:6.1f}% {t:>8}{flag}")

print()
print(f"{len(stats)} tracked functions, {len(under)} below {threshold:g}%")

if under:
    print(f"\nFAIL: at least one function is below {threshold:g}% coverage.",
          file=sys.stderr)
    sys.exit(1)
print(f"OK: all tracked functions >= {threshold:g}% coverage.")
PY
