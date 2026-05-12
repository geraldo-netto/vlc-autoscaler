#!/usr/bin/env bash
# Per-function coverage gate.
#
# Reads $COV_DIR/functions.txt (produced by `gcov -f` runs in the
# coverage Make target) and fails if any function in a tracked source
# file is below $THRESHOLD% line coverage.
#
# Aggregation: each test binary emits its own gcov stream, so the same
# function appears multiple times. We keep the BEST (max) coverage
# across runs - a function is "tested" if at least one test exercises
# it adequately, regardless of which test it is.
#
# Usage: COV_DIR=build/cov THRESHOLD=80 scripts/coverage_per_function.sh

set -u
COV_DIR="${COV_DIR:-build/cov}"
THRESHOLD="${THRESHOLD:-80}"
INPUT="$COV_DIR/functions.txt"

if [[ ! -f "$INPUT" ]]; then
    echo "ERROR: $INPUT not found. Run 'make coverage' first." >&2
    exit 2
fi

# Tracked files - same set as the per-file gate (scripts/coverage_report.sh).
TRACKED="upscale_logic.h usm.h perfmon.h threading.h zimg_helpers.h \
chroma_classify.h content_probe.h scaler_pick_logic.h usm_pool.h usm_pool.c"

python3 - "$INPUT" "$THRESHOLD" "$TRACKED" <<'PY'
import sys, re
input_path, threshold, tracked_str = sys.argv[1], float(sys.argv[2]), sys.argv[3]
tracked = set(tracked_str.split())

with open(input_path) as fh:
    lines = fh.read().splitlines()

pending = []      # (name, pct, total) awaiting a File line
best = {}         # (basename, name) -> (pct, total)
i = 0
while i < len(lines):
    m = re.match(r"Function '?([^'\n]+)'?\s*$", lines[i])
    if m and i + 1 < len(lines):
        pm = re.match(r"Lines executed:([\d.]+)% of (\d+)", lines[i + 1])
        if pm:
            pending.append((m.group(1), float(pm.group(1)), int(pm.group(2))))
            i += 2
            continue
    m = re.match(r"File '?([^'\n]+)'?\s*$", lines[i])
    if m:
        base = m.group(1).split("/")[-1]
        if base in tracked:
            for (n, p, t) in pending:
                key = (base, n)
                if key not in best or best[key][0] < p:
                    best[key] = (p, t)
        pending = []
    i += 1

under = [(f, n, p, t) for (f, n), (p, t) in best.items() if p < threshold]

# Two-column report: file, function, pct, total exec lines.
print(f"{'file':<25} {'function':<48} {'pct':>7} {'lines':>8}")
print(f"{'-' * 25} {'-' * 48} {'-' * 7} {'-' * 8}")
for (f, n), (p, t) in sorted(best.items()):
    flag = "  <-- BELOW" if p < threshold else ""
    print(f"{f:<25} {n:<48} {p:6.1f}% {t:>8}{flag}")

print()
print(f"{len(best)} tracked functions, {len(under)} below {threshold:g}%")

if under:
    print(f"\nFAIL: at least one function is below {threshold:g}% coverage.",
          file=sys.stderr)
    sys.exit(1)
print(f"OK: all tracked functions >= {threshold:g}% coverage.")
PY
