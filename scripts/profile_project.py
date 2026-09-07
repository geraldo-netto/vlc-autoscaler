#!/usr/bin/env python3
"""Run isolated, repeated worker/pipeline profiles and retain raw run summaries."""

import argparse
import csv
import json
import os
from pathlib import Path
import platform
import random
import statistics
import subprocess
import time


def command_output(args):
    result = subprocess.run(args, text=True, capture_output=True, check=False)
    return {"command": args, "status": result.returncode,
            "stdout": result.stdout, "stderr": result.stderr}


def last_cache(base, cpu):
    entries = [p for p in (base / "cache").glob("index*")
               if (p / "type").read_text().strip() in ["Unified", "Data"]]
    if not entries:
        return f"cpu{cpu}"
    cache = max(entries, key=lambda p: int((p / "level").read_text()))
    return (cache / "shared_cpu_list").read_text().strip()


def masks():
    allowed = sorted(os.sched_getaffinity(0))
    cores, caches = {}, {}
    for cpu in allowed:
        base = Path(f"/sys/devices/system/cpu/cpu{cpu}")
        core = (base / "topology/core_id").read_text().strip()
        socket = (base / "topology/physical_package_id").read_text().strip()
        cores.setdefault((socket, core), cpu)
        cache = last_cache(base, cpu)
        caches.setdefault(cache, []).append(cpu)
    physical = sorted(cores.values())
    local = sorted(set(next(iter(caches.values()))) & set(physical))[:8]
    split = sorted(cpu for group in caches.values()
                   for cpu in sorted(set(group) & set(physical))[:4])[:8]
    return {"all": allowed, "physical": physical, "local8": local, "split8": split}


def pipeline_case(family, z, u, width=1920, height=1080, **changes):
    case = dict(family=family, kind="pipeline", mask="all", z=z, u=u,
                width=width, height=height, pin=1, detail=0, content=1, period=0)
    case.update(changes)
    return case


def baseline_cases():
    counts = [1, 2, 4, 8, 12, 16, 24, 32, 48, 64]
    sizes = [(1280, 720), (1920, 1080), (3840, 2160)]
    result = []
    for width, height in sizes:
        result.extend(pipeline_case("usm", 12, n, width, height) for n in counts)
        result.extend(pipeline_case("zimg", n, 12, width, height) for n in counts)
        result.extend(pipeline_case("balanced", n, n, width, height) for n in [32, 64])
    result.extend(pipeline_case("wide", n, 12, 4096, 128) for n in counts)
    return result


def placement_cases():
    result = []
    for mask in ["physical", "local8", "split8"]:
        result.extend(pipeline_case("placement", 8, n, mask=mask)
                      for n in [4, 8, 12, 16, 32])
    for n in [12, 32]:
        result.append(pipeline_case("pin", n, n, pin=0))
        result.append(pipeline_case("noise", n, n, content=0))
    return result


def detail_cases():
    result = []
    for width, height in [(1280, 720), (1920, 1080), (3840, 2160)]:
        for z, u in [(12, 4), (12, 12), (32, 32), (32, 64)]:
            result.append(pipeline_case("trace", z, u, width, height, detail=1))
        result.append(pipeline_case("cpu_trace", 12, 12, width, height, detail=2))
    return result


def empty_cases():
    return [dict(family="empty", kind="empty", mask="all", workers=n,
                 pin=pin, detail=detail)
            for n in [1, 2, 4, 8, 12, 16, 24, 32, 48, 64]
            for pin in [0, 1] for detail in [0, 1]]


def paced_cases():
    return [pipeline_case("paced", 12, n, width, height, period=16667)
            for width, height in [(1280, 720), (1920, 1080)] for n in [4, 12, 32]]


def observer_cases():
    return [pipeline_case("observer", n, n, width, height, detail=detail)
            for width, height in [(1280, 720), (1920, 1080), (3840, 2160)]
            for n in [12, 32] for detail in [0, 1, 2]]


def choose_cases(group):
    groups = {"baseline": baseline_cases() + placement_cases(),
              "detail": detail_cases(), "empty": empty_cases(), "paced": paced_cases(),
              "observer": observer_cases()}
    return sum(groups.values(), []) if group == "all" else groups[group]


def summarize_empty(result):
    samples = result.pop("samples")
    walls = sorted(row[0] for row in samples)
    result.update(frame_mean=statistics.mean(walls), frame_p50=statistics.median(walls),
                  frame_p95=walls[(len(walls) - 1) * 95 // 100],
                  frame_p99=walls[(len(walls) - 1) * 99 // 100])
    result["trace"] = [statistics.mean(row[i] for row in samples) for i in range(1, 9)]


def run_case(case, args, affinity, repetition, index):
    frames = 240 if case.get("period") else args.frames
    prefix = ["taskset", "-c", ",".join(map(str, affinity[case["mask"]]))]
    if case["kind"] == "empty":
        cmd = [str(args.build / "profile_worker_pool"), str(case["workers"]),
               str(frames), str(case["pin"]), str(case["detail"])]
    else:
        values = [case[k] for k in ["z", "u", "width", "height"]] + [frames]
        values += [case[k] for k in ["pin", "detail", "content", "period"]]
        cmd = [str(args.build / "profile_pipeline"), *map(str, values)]
    completed = subprocess.run(prefix + cmd, text=True, capture_output=True, check=True, timeout=180)
    result = json.loads(completed.stdout)
    if case["kind"] == "empty":
        summarize_empty(result)
    result.update(case, repetition=repetition, order=index, frames=frames,
                  command=prefix + cmd, timestamp=time.time())
    return result


def validate_hashes(results):
    seen = {}
    for row in results:
        if row["kind"] != "pipeline":
            continue
        key = (row["width"], row["height"], row["content"], row["frames"] % 8,
               row["rows"], row["cols"], bool(row["u"]))
        previous = seen.setdefault(key, row["hash"])
        if previous != row["hash"]:
            raise RuntimeError(f"Output changed with an unchanged graph grid: {row}")


def metadata(args, affinity):
    commands = [["git", "rev-parse", "HEAD"], ["git", "status", "--short"],
                ["cc", "--version"], ["lscpu"], ["lscpu", "-e=CPU,CORE,SOCKET,NODE,CACHE"],
                ["pkg-config", "--modversion", "vlc-plugin", "zimg"],
                ["cat", "/proc/sys/kernel/perf_event_paranoid"],
                ["cat", "/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor"]]
    return {"platform": platform.platform(), "affinity": affinity,
            "args": {k: str(v) for k, v in vars(args).items()},
            "commands": [command_output(command) for command in commands]}


def write_csv(results, path):
    fields = sorted({key for row in results for key, value in row.items()
                     if not isinstance(value, list)})
    with path.open("x") as out:
        writer = csv.DictWriter(out, fieldnames=fields, extrasaction="ignore", lineterminator="\n")
        writer.writeheader()
        writer.writerows(results)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--group", choices=["all", "baseline", "detail", "empty", "paced", "observer"], default="all")
    parser.add_argument("--repetitions", type=int, default=5)
    parser.add_argument("--frames", type=int, default=2048)
    args = parser.parse_args()
    if not 1 <= args.repetitions <= 20 or not 1 <= args.frames <= 1000000:
        parser.error("repetitions must be 1..20 and frames 1..1000000")
    args.build = args.build.resolve()
    args.output.mkdir(parents=True, exist_ok=True)
    if (args.output / "runs.csv").exists():
        parser.error("output already contains runs.csv")
    affinity = masks()
    cases = choose_cases(args.group)
    results = []
    with (args.output / "runs.jsonl").open("x") as out:
        (args.output / "metadata.json").write_text(json.dumps(metadata(args, affinity), indent=2) + "\n")
        for repetition in range(args.repetitions):
            order = cases.copy()
            random.Random(20260907 + repetition).shuffle(order)
            for index, case in enumerate(order):
                result = run_case(case, args, affinity, repetition, index)
                results.append(result)
                out.write(json.dumps(result) + "\n")
                out.flush()
            print(f"Completed {repetition + 1}/{args.repetitions}: {len(results)} runs", flush=True)
    validate_hashes(results)
    write_csv(results, args.output / "runs.csv")
    print("All matching-grid output hashes agree.")


if __name__ == "__main__":
    main()
