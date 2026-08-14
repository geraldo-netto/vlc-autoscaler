#!/usr/bin/env python3
"""Run curated mutations against high-value control-plane unit tests."""

from __future__ import annotations

import os
import shlex
import shutil
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path


@dataclass(frozen=True)
class Mutant:
    name: str
    source: str
    suite: str
    old: str
    new: str


MUTANTS = (
    Mutant(
        "auto_four_core_boundary",
        "src/upscale_logic.h",
        "tests/test_upscale_logic.c",
        "int can_1080p = cores >= 4 && ratio_ok_for_1080p;",
        "int can_1080p = cores > 4 && ratio_ok_for_1080p;",
    ),
    Mutant(
        "auto_exact_ratio_boundary",
        "src/upscale_logic.h",
        "tests/test_upscale_logic.c",
        "&& (src_h * UP_MAX_RATIO >= 1080);",
        "&& (src_h * UP_MAX_RATIO > 1080);",
    ),
    Mutant(
        "ratio_cap_direction",
        "src/upscale_logic.h",
        "tests/test_upscale_logic.c",
        "if (target_h > cap)",
        "if (target_h < cap)",
    ),
    Mutant(
        "never_downscale_direction",
        "src/upscale_logic.h",
        "tests/test_upscale_logic.c",
        "if (target_h < src_h)",
        "if (target_h > src_h)",
    ),
    Mutant(
        "skip_gate_affects_explicit_presets",
        "src/upscale_logic.h",
        "tests/test_upscale_logic.c",
        "if (preset == UP_TARGET_AUTO && skip_above > 0 && src_h >= skip_above)",
        "if (preset >= UP_TARGET_AUTO && skip_above > 0 && src_h >= skip_above)",
    ),
    Mutant(
        "skip_gate_excludes_equal_height",
        "src/upscale_logic.h",
        "tests/test_upscale_logic.c",
        "if (preset == UP_TARGET_AUTO && skip_above > 0 && src_h >= skip_above)",
        "if (preset == UP_TARGET_AUTO && skip_above > 0 && src_h > skip_above)",
    ),
    Mutant(
        "preferred_open_inverts_success",
        "src/scaler_pick_logic.h",
        "tests/test_scaler_pick.c",
        "if (open_backend(context, preferred_handle) == 0)",
        "if (open_backend(context, preferred_handle) != 0)",
    ),
    Mutant(
        "fallback_permission_inverted",
        "src/scaler_pick_logic.h",
        "tests/test_scaler_pick.c",
        "if (!allow_fallback || fallback_handle == NULL ||",
        "if (allow_fallback || fallback_handle == NULL ||",
    ),
    Mutant(
        "fallback_alias_guard_inverted",
        "src/scaler_pick_logic.h",
        "tests/test_scaler_pick.c",
        "fallback_handle == preferred_handle)",
        "fallback_handle != preferred_handle)",
    ),
    Mutant(
        "auto_backend_priority_inverted",
        "src/scaler_pick_logic.h",
        "tests/test_scaler_pick.c",
        "if (z)\n        return z;",
        "if (!z)\n        return z;",
    ),
    Mutant(
        "sharpness_cutoff_becomes_inclusive",
        "src/content_probe.h",
        "tests/test_content_probe.c",
        "lap_mean > (uint64_t)threshold",
        "lap_mean >= (uint64_t)threshold",
    ),
    Mutant(
        "minimum_probe_frames_becomes_exclusive",
        "src/content_probe.h",
        "tests/test_content_probe.c",
        "if (a->frames < UP_PROBE_MIN_FRAMES) return 0;",
        "if (a->frames <= UP_PROBE_MIN_FRAMES) return 0;",
    ),
    Mutant(
        "minimum_laplacian_samples_becomes_exclusive",
        "src/content_probe.h",
        "tests/test_content_probe.c",
        "if (a->lap_samples  < UP_PROBE_MIN_SAMPLES_PER_KIND) return 0;",
        "if (a->lap_samples  <= UP_PROBE_MIN_SAMPLES_PER_KIND) return 0;",
    ),
    Mutant(
        "minimum_edge_samples_becomes_exclusive",
        "src/content_probe.h",
        "tests/test_content_probe.c",
        "if (a->edge_samples < UP_PROBE_MIN_SAMPLES_PER_KIND) return 0;",
        "if (a->edge_samples <= UP_PROBE_MIN_SAMPLES_PER_KIND) return 0;",
    ),
    Mutant(
        "soft_cutoff_becomes_inclusive",
        "src/content_probe.h",
        "tests/test_content_probe.c",
        "lap_mean  < (uint64_t)UP_PROBE_THRESH_SOFT_LAP_MEAN",
        "lap_mean  <= (uint64_t)UP_PROBE_THRESH_SOFT_LAP_MEAN",
    ),
    Mutant(
        "blocky_cutoff_becomes_inclusive",
        "src/content_probe.h",
        "tests/test_content_probe.c",
        "edge_mean > (uint64_t)UP_PROBE_THRESH_BLOCKY_EDGE_MEAN",
        "edge_mean >= (uint64_t)UP_PROBE_THRESH_BLOCKY_EDGE_MEAN",
    ),
    Mutant(
        "advisory_accepts_only_one_bad_metric",
        "src/content_probe.h",
        "tests/test_content_probe.c",
        "very_soft && very_blocky",
        "very_soft || very_blocky",
    ),
    Mutant(
        "zero_sharpness_threshold_enables_gate",
        "src/content_probe.h",
        "tests/test_content_probe.c",
        "if (threshold <= 0) return 0;",
        "if (threshold < 0) return 0;",
    ),
)


def show_failure(label: str, result: subprocess.CompletedProcess[str]) -> None:
    print(f"  [FAIL] {label} (exit {result.returncode})", file=sys.stderr)
    output = (result.stdout + result.stderr).strip()
    if output:
        print(output, file=sys.stderr)


def compile_suite(
    compiler: list[str], flags: list[str], workspace: Path, suite: str, output: Path
) -> subprocess.CompletedProcess[str]:
    command = compiler + flags + [str(workspace / suite), "-o", str(output)]
    return subprocess.run(command, capture_output=True, text=True, check=False)


def run_binary(binary: Path) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [str(binary)], capture_output=True, text=True, check=False, timeout=30
    )


def prepare_workspace(repo: Path, workspace: Path) -> None:
    shutil.copytree(repo / "src", workspace / "src")
    (workspace / "tests").mkdir()
    for header in (repo / "tests").glob("*.h"):
        shutil.copy2(header, workspace / "tests" / header.name)
    for suite in {mutant.suite for mutant in MUTANTS}:
        source = repo / suite
        shutil.copy2(source, workspace / suite)


def verify_baselines(
    compiler: list[str], flags: list[str], workspace: Path, build: Path
) -> bool:
    for index, suite in enumerate(sorted({mutant.suite for mutant in MUTANTS})):
        binary = build / f"baseline-{index}"
        compiled = compile_suite(compiler, flags, workspace, suite, binary)
        if compiled.returncode != 0:
            show_failure(f"baseline compile: {suite}", compiled)
            return False
        result = run_binary(binary)
        if result.returncode != 0:
            show_failure(f"baseline test: {suite}", result)
            return False
    return True


def run_mutant(
    mutant: Mutant,
    compiler: list[str],
    flags: list[str],
    workspace: Path,
    build: Path,
    originals: dict[str, str],
) -> bool:
    path = workspace / mutant.source
    original = originals[mutant.source]
    occurrences = original.count(mutant.old)
    if occurrences != 1:
        print(
            f"  [FAIL] {mutant.name}: source pattern occurs {occurrences} times",
            file=sys.stderr,
        )
        return False

    path.write_text(original.replace(mutant.old, mutant.new), encoding="utf-8")
    binary = build / mutant.name
    compiled = compile_suite(compiler, flags, workspace, mutant.suite, binary)
    path.write_text(original, encoding="utf-8")
    if compiled.returncode != 0:
        show_failure(f"{mutant.name}: mutant did not compile", compiled)
        return False

    result = run_binary(binary)
    if result.returncode == 1:
        print(f"  [killed] {mutant.name}")
        return True
    if result.returncode == 0:
        print(f"  [SURVIVED] {mutant.name}", file=sys.stderr)
        return False
    show_failure(f"{mutant.name}: test crashed", result)
    return False


def main() -> int:
    repo = Path(__file__).resolve().parent.parent
    compiler = shlex.split(os.environ.get("CC", "cc"))
    flags = shlex.split(
        os.environ.get(
            "MUTATION_CFLAGS",
            "-O2 -g -std=c11 -D_GNU_SOURCE -Wall -Wextra -Werror",
        )
    )

    with tempfile.TemporaryDirectory(prefix="vlc-autoscaler-mutation-") as temp:
        workspace = Path(temp)
        build = workspace / "build"
        build.mkdir()
        prepare_workspace(repo, workspace)
        originals = {
            source: (workspace / source).read_text(encoding="utf-8")
            for source in {mutant.source for mutant in MUTANTS}
        }

        if not verify_baselines(compiler, flags, workspace, build):
            return 1

        print(f"Running {len(MUTANTS)} curated mutants...")
        killed = sum(
            run_mutant(mutant, compiler, flags, workspace, build, originals)
            for mutant in MUTANTS
        )

    print(f"Mutation score: {killed}/{len(MUTANTS)} killed")
    return 0 if killed == len(MUTANTS) else 1


if __name__ == "__main__":
    sys.exit(main())
