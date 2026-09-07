# Worker and pipeline profiling

This evaluation measures the existing shared gate, worker execution, and frame
processing before considering more shared-nothing isolation. Profiling changes no
production scheduling, image partitioning, or pixel kernels. The profiling
executables compile private copies of the production backends; dispatch tracing
exists only in those executables.

## Evaluation: 7 September 2026

The existing per-worker ownership suits this workload. A broader shared-nothing
rewrite has no demonstrated latency benefit. The measurements instead identify
two distinct costs: substantial pixel-kernel work, and increasingly expensive
worker activation at high counts. Keep the private graphs, scratch buffers and
USM stripes; investigate coordination with a bounded prototype before changing
the architecture.

Host: Ryzen 9 7945HX, 16 physical cores / 32 logical CPUs, two L3 domains, one
NUMA node, `performance` governor and turbo enabled. Linux 7.0.0-31, GCC 13.3.0,
VLC 3.0.20 and zimg 3.0.5. Production sources are revision
`bcba62f92367c0dd9427f4ead0aa87ee3ad4c684`; the accompanying harness compiles
private copies of those sources. zimg uses `-O2 -g -march=native`, USM and the driver use
`-O3 -g -march=native`, with `-Werror` and no sanitizers or LTO in timed builds.
These match the standalone backend benchmark optimization levels, not the
plugin's complete LTO build.

The collection contains 870 timing runs: 475 baseline, 75 detailed, 200 empty,
30 paced and 90 observer-effect runs. Inspect the
[individual measurements](benchmarks/profiling-2026-09-07/runs.csv),
[aggregate statistics](benchmarks/profiling-2026-09-07/summary.csv), and
[environment and source hashes](benchmarks/profiling-2026-09-07/environment.json).

![Frame time by USM worker count, with individual runs and median run means](benchmarks/profiling-2026-09-07/worker-scaling.svg)

### Frame latency and worker count

The primary sweep contains 475 runs: five shuffled repetitions of 95 cases,
2,048 measured frames each. All cells below are medians of the five run means,
in microseconds per frame. zimg remains at 12 workers with an unchanged 12x1
graph grid; only USM concurrency changes.

| USM workers | 1280x720 | 1920x1080 | 3840x2160 |
|---|---:|---:|---:|
| 1 | 311.5 | 646.9 | 2497.1 |
| 2 | 208.9 | 418.3 | 1546.7 |
| 4 | 159.1 | 310.5 | 1066.8 |
| 8 | 135.3 | 266.9 | 898.7 |
| 12 | 136.7 | 258.9 | 875.0 |
| 16 | 146.8 | 265.5 | 805.0 |
| 24 | 153.7 | 273.4 | 883.7 |
| 32 | 173.7 | 284.9 | 958.9 |
| 48 | 202.4 | 320.3 | 877.7 |
| 64 | 233.3 | 346.7 | 920.9 |

The best sampled counts are 8, 12 and 16 respectively; 8 versus 12 at 720p is
only a 1% difference and does not justify a universal default change. At 1080p,
12 to 32 USM workers increases mean frame time by 10.1%, CPU time/frame from
2,226 to 3,128 us, and voluntary context switches/frame from 30.3 to 73.9.
At 720p, 64 USM workers take 72.4% longer than eight while consuming about
2.4x the CPU time. At 4K, 16 versus 12 workers reduces mean time by 8.0%, with
more CPU work. These are fixed-count results, not validation of the adaptive
controller's selection or convergence.

Changing zimg concurrency changes its graph grid. The 1080p zimg sweep with
12 USM workers reaches 248.3 us/frame at 16 zimg workers and 245.7 at 32, but
the corresponding median run p99 rises from 484.0 to 626.1 us and CPU/frame
from 2,453 to 3,702 us. This is not a quality-equivalent adaptive comparison.
Requests for 48 or 64 zimg workers produce only 32 on this host; differences
between these equivalent effective counts expose run-to-run variation.

The 4096x128 stress geometry changes from an 8x1 grid at eight zimg workers to
6x2 at 12 and 8x4 at 32. Its means are 88.5, 99.7 and 122.3 us/frame. More
tiles therefore do not automatically repay their coordination/copy cost, and
these different grids must also retain the seam-quality caveat.

### Placement and input sensitivity

With an unchanged 8x1 zimg grid and 12 USM workers at 1080p, restricting all
threads to eight physical cores in one L3 domain gives 278.9 us/frame versus
404.3 us across four physical cores in each of two L3 domains. The one-domain
placement is also faster at USM counts 4, 8, 16 and 32. This establishes
sensitivity to placement on this host; it does not isolate cache coherence
from scheduling, clock frequency, or placement of the coordinator. It supports
testing a bounded affinity policy, not shipping a topology heuristic from this
single synthetic workload.

Disabling the existing zimg pinning changes the 12/12 1080p mean from 258.9 to
290.0 us/frame. Noise versus smooth input with sharpening forcibly active is
similar here: 258.5 versus 258.9 us at 12/12. This noise result does not model
the production sharpness probe, which can disable USM.

### Coordination and worker execution

The empty pool runs the production gate and increments one private byte per
worker. Five repeats of 2,048 dispatches, with callback tracing disabled:

| Workers | Unpinned mean us | Unpinned p99 us | Pinned mean us | Pinned p99 us |
|---|---:|---:|---:|---:|
| 4 | 10.39 | 15.55 | 10.21 | 13.92 |
| 8 | 11.11 | 17.05 | 9.27 | 28.72 |
| 12 | 19.99 | 26.87 | 18.57 | 34.88 |
| 16 | 23.39 | 33.57 | 24.52 | 40.58 |
| 32 | 50.58 | 70.19 | 70.22 | 303.61 |
| 64 | 108.56 | 180.92 | 192.80 | 840.34 |

One worker takes the synchronous inline path, so its approximately 0.02 us
empty result does not measure a thread wake. At 32 and 64 workers, strict
pinning makes empty-dispatch tails worse on this host. Conversely, zimg pinning
helps the busy pipeline. The empty workload cannot select a production affinity
policy or quantify the fraction of busy dispatch time that can be removed.

Detailed 1080p traces, five repeats, in us; each column is a median of run-level
means. Both stages use the indicated worker count:

| Stage / workers | Dispatch | First start | Last start | Shortest work | Longest work | Final handoff |
|---|---:|---:|---:|---:|---:|---:|
| zimg / 12 | 169.62 | 7.64 | 23.45 | 101.54 | 140.69 | 5.46 |
| zimg / 32 | 156.77 | 20.98 | 68.77 | 67.75 | 93.37 | 5.42 |
| USM / 12 | 91.93 | 7.74 | 14.31 | 40.92 | 77.57 | 5.07 |
| USM / 32 | 109.90 | 22.29 | 79.54 | 17.19 | 53.81 | 5.27 |

Extra workers shorten each USM callback but delay the last callback's start
enough that the dispatch grows. Completion handoff stays near 5 us. At 720p,
32-worker zimg has an 81.60 us last start and a 44.19 us longest callback,
making worker activation particularly prominent. At 4K and 12 workers, zimg's
longest callback is 482.84 us against a 28.60 us last start, so pixel execution
is the larger visible cost. These fields overlap; they must not be added or
subtracted to claim an architectural speedup.

Worker runqueue time at 1080p grows from 14.5/2.6 us per frame summed across
zimg/USM workers at 12/12 to 57.1/68.1 us at 32/32. Kernel scheduler captures
independently report average runnable delay of 0.004 ms over 120,565 schedule
events at 12/12, versus 0.006 ms over 375,414 events at 32/32; maxima are
1.716 and 3.523 ms. These captures include warmup/startup and tracing overhead,
and their event averages are not frame-latency quantiles.

### Hardware counters

`perf stat` collected three repetitions of 10,000 measured frames plus 128
warmup frames, allocation, teardown and the `runuser` launcher. Every requested
event reports 100% running time, with no multiplexing. Counts below are mean
whole-process counts per invocation, not counts isolated to the measured loop.

| Output / workers per stage | Cycles, billions | Instructions, billions | IPC | Generic cache misses, millions | Context switches | CPU migrations |
|---|---:|---:|---:|---:|---:|---:|
| 1080p / 12 | 104.33 | 282.05 | 2.70 | 379.03 | 270637 | 2938 |
| 1080p / 32 | 214.91 | 294.63 | 1.37 | 787.12 | 1034361 | 88712 |
| 4K / 12 | 368.98 | 1020.70 | 2.77 | 1594.13 | 274116 | 4277 |
| 4K / 32 | 719.43 | 1034.61 | 1.44 | 2474.84 | 1051017 | 116816 |

At 1080p, 32/32 executes only 4.5% more instructions but consumes 2.06x the
cycles and incurs 3.82x the context switches. IPC roughly halves at both
resolutions. Together with the wake-delay traces, this is evidence of declining
parallel efficiency. The changed zimg grid, SMT use, scheduling, cache behavior
and clock frequency remain mixed in this comparison. Generic cache events
must not be relabeled as measured DRAM bandwidth or proof of false sharing.

### Playback pacing

At 60 fps release intervals, five repeats of 240 measured frames give:

| Output | USM workers | Mean processing us | p95 us | p99 us |
|---|---:|---:|---:|---:|
| 720p | 4 | 313.9 | 447.1 | 1367.2 |
| 720p | 12 | 291.5 | 475.8 | 1327.5 |
| 720p | 32 | 346.1 | 537.1 | 1371.3 |
| 1080p | 4 | 528.3 | 734.2 | 919.0 |
| 1080p | 12 | 468.2 | 768.3 | 1068.6 |
| 1080p | 32 | 495.7 | 719.4 | 1570.6 |

zimg remains at 12 workers. Timing excludes the deliberate sleep. The 1080p
12/12 mean is 1.81x its continuous-loop result, consistent with a different
cache/power/scheduling regime after idle intervals; these captures do not
separate those causes. This is a synthetic paced pipeline, not a real decoded
clip or complete VLC playback. It demonstrates why controller acceptance must
include paced representative content, even when continuously saturated trials
show a gain. The means remain well below a 16.67 ms frame period, but the rest
of VLC's decode/display/audio work is outside this budget measurement.

### Observer effects and limits

The separate observer sweep shuffles all three detail modes within each of
five repetitions. For 1080p 12/12, median run means are 256.92 us without
callback tracing, 255.66 us with wall timestamps, and 271.10 us with thread CPU
clocks. Wall tracing's small negative difference is noise, not a speedup; CPU
clock tracing is 5.5% higher in this comparison. At 4K 32/32, the corresponding
means are 760.99, 912.26 and 774.68 us, with overlapping run ranges extending
above 1 ms. There is no stable overhead percentage to subtract from all traces.
Use detail mode 0 for performance decisions and traced modes for attribution.

The first CPU sample captures attribute 70.02% of 12/12 user-cycle samples to
libzimg and 28.99% to `usm_pool_run_worker`. At 32/32, libzimg accounts for
71.57%; libc rises from 0.59% to 1.76%. The latter capture reports 14 lost
samples and is provisional. zimg's installed library lacks internal symbols,
so individual zimg kernel names are unavailable. User-cycle percentages exclude
blocked time and kernel execution; they cannot establish the fraction of frame
latency spent in synchronization. The
[12-worker library report](benchmarks/profiling-2026-09-07/w12.cpu-dso.txt) and
[32-worker report](benchmarks/profiling-2026-09-07/w32.cpu-dso.txt) retain this scope.

The initial cache-to-cache command used an invalid event selector and captured
no data. A corrected, lower-overhead CPU and cache-line collection is pending.
No false-sharing location, DRAM bandwidth saturation, or shared-nothing speedup
is claimed from the available counters. `perf c2c` selects AMD's IBS event
through its `mem-ldst` selector/default, as specified by the
[perf documentation](https://kernel.googlesource.com/pub/scm/linux/kernel/git/stable/linux-stable/+/master/tools/perf/Documentation/perf-c2c.txt).

### Architectural decision

Keep per-worker ownership in one process. The mutable image-processing state
is already private to each worker, and shared input can be borrowed read-only.
Introducing processes, full-frame copying or a distributed pipeline would add
communication and buffering without a measured benefit to this synchronous
single-frame objective.

The next useful comparison is a benchmark-only notification/completion
prototype, keeping graph geometry, pixels, worker counts and kernels fixed.
Compare it with the current gate at 12, 32 and 64 workers where supported,
including mean/p95/p99, CPU time, paced input and cancellation/shutdown tests.
Per-worker mailboxes could reduce shared-lock traffic but still require wakeups
and a completion join; spinning may shorten wake delay at substantial idle CPU
and power cost. A tree or batching can add hops or frame latency. None has an
established speedup here. Complete cache-line attribution before selecting the
mechanism.

Affinity is a separate bounded experiment suggested by the placement sweep.
Test consistent USM placement and stage-to-stage locality before introducing
Linux topology discovery into production policy. Any zimg tuning must preserve
the graph grid: a faster configuration with changed seams does not satisfy a
constant-quality tuning contract. `TODO.md` retains these follow-ups and the
existing mean-versus-median controller issue.

### Validation

The profiling binaries build with `-Werror`. Twenty-four ASan/UBSan cases and
24 TSan cases cover pipeline/empty dispatch, worker counts 1/12/32/64 and all
three detail modes. TSan needs `setarch x86_64 -R` on this host because its
default address layout causes a runtime mapping failure; no global ASLR setting
was changed. Final-frame hashes agree for matching graph/input/sharpening
configurations throughout the sweeps, and empty-pool worker counters verify
every dispatch.

`make check plugin check-hardening check-visibility`, clang static analysis of
the four profiling translation units, cppcheck, shell/Markdown/link checks and
the lizard complexity gate pass. All added C/Python functions remain at CCN 10
or below. The parallel regression recipes still emit the pre-existing GNU Make
jobserver warnings recorded as BUILD-40. These baseline measurements precede
the subsequent USM AUTO policy below.

## Adopted USM AUTO presets

USM AUTO now selects workers once at pool creation using output pixel area:
8 through 921,600 pixels, 12 through 2,073,600 pixels, and 16 above that.
Allowed CPUs and stripe geometry cap the count. The same thresholds cover
portrait, intermediate and wide/short output dimensions. Explicit preferences
still apply to both pools; zimg's AUTO policy and grid remain unchanged. Opt-in
adaptive USM starts from the new count. The USM policy does not apply zimg's
half-CPU reserve and can use every allowed CPU when fewer than the preset count
are available.

The presets were authorized provisionally from the continuous-loop results.
A subsequent 60 fps check produced these median run statistics in microseconds:

| Output | USM workers | Mean frame | p95 | p99 | CPU/frame |
|---|---:|---:|---:|---:|---:|
| 720p | 8 | 348.70 | 950.50 | 2020.79 | 1626.47 |
| 720p | 12 | 291.35 | 663.17 | 1135.76 | 1661.94 |
| 4K | 12 | 1190.25 | 1727.73 | 2516.27 | 10002.44 |
| 4K | 16 | 1255.30 | 2012.76 | 3257.95 | 10467.01 |

The [20 individual runs](benchmarks/usm-auto-paced-2026-09-07.csv) comprise five
shuffled repetitions per configuration, 128 warmup frames and 240 measured
frames, using the same baseline profile binary and unchanged pixel kernels.
All matching-grid final-frame hashes agree. Direct command per case:

```text
profile_pipeline 12 USM_WORKERS WIDTH HEIGHT 240 1 0 1 16667
```

These mean summaries are 19.7% slower for 720p eight versus twelve workers and
5.5% slower for 4K sixteen versus twelve. Eight loses four of five paired 720p
comparisons; 4K is mixed, with sixteen winning three of five pairs despite its
higher median run mean. Small samples and material host variation limit the
conclusion. The presets are not an established paced-playback improvement;
PERF-15 tracks this conflict with the continuous-loop evidence. Representative
decoded content and additional hosts remain needed before generalizing.

GCC and Clang policy/lifecycle tests cover the thresholds, portrait and unusual
geometry, invalid/large inputs, CPU caps, explicit preferences and the real
VLC open path. ASan/UBSan in-place comparisons at all three output resolutions
match the single-threaded USM oracle byte for byte. Clang uses the existing CI
suppression for the VLC SDK's `-Wunreachable-code-generic-assoc` warning.

## Reproduce

The harness requires Linux, the VLC and zimg development files, and the normal
project compiler dependencies. Run each collection separately on an otherwise
idle host. The runner records the allowed CPUs, physical-core/L3 groupings,
compiler, dependencies, governor, commands, and source revision.

```sh
make BUILD=build-profile EXTRA_CFLAGS=-Werror build-profile
python3 scripts/profile_project.py --build build-profile \
  --output build-profile/results --group all --repetitions 5 --frames 2048
```

The output directory must not already contain `runs.jsonl` or `runs.csv`.
`runs.jsonl` retains every command, run-level result, and trace aggregate;
`runs.csv` contains the scalar fields. The groups can also run separately:
`baseline`, `empty`, `detail`, `paced`, and `observer`. The last group shuffles
matching detail modes within each repetition to assess instrumentation effects.

Direct interfaces:

```text
profile_pipeline zworkers uworkers width height frames pin detail content period-us [samples.csv]
profile_worker_pool workers frames pin detail
```

`pin=1` enables the existing zimg affinity policy; USM remains scheduler-managed.
For the empty-pool executable, `pin=1` pins its synthetic workers to the allowed
CPU order. `detail=0` measures frame/stage time without callback tracing;
`detail=1` adds callback wall timestamps and per-thread scheduling snapshots;
`detail=2` also reads each callback's thread CPU clock. Clock and tracing costs
must be assessed against matching `detail=0` runs.

`content=1` uses eight deterministic smooth-pattern input pictures;
`content=0` uses eight deterministic noise pictures. Both use I420, 2x scaling,
Spline36, source/destination zero-copy where the backend permits it, and 20%
in-place USM. `uworkers=0` disables sharpening. A nonzero `period-us` supplies
absolute frame release times; frame timing excludes the deliberate pacing sleep.
The runner's paced cases use 240 measured frames at approximately 60 fps.

All runs discard 128 warmup frames. The input ring, output picture, pools and
sample storage are allocated before timing. Decoding, display, audio, VLC's
picture allocator, startup and adaptive exploration are outside these pipeline
measurements. Sharpening is deliberately active throughout: a matching VLC
noise experiment requires `--autoupscale-usm-sharp-threshold=0`.

## Interpret the measurements

The goal is lower total processing time per frame, with attention to p95/p99
latency and CPU consumption. Run means are total measured processing time
divided by frame count. Reported cross-run summaries use the median of repeated
run statistics, keeping individual results available; a median of run p99s is
not the p99 of a pooled population.

The production zimg backend still clips its requested count to the available
CPU budget and valid graph grid. The standalone USM pool and empty pool can
execute requests up to 64 even on this 32-logical-CPU host. Those oversubscribed
runs diagnose coordination limits; they do not represent 64 physical cores or
a 64-worker zimg production run.

Run variation is material. For example, the five 1080p 32/32 baseline means
range from 263.2 to 1414.1 us/frame, while 12/12 ranges from 251.9 to 269.1.
All runs are retained; medians describe typical repeated runs and do not erase
the outliers or establish statistical significance. Continuous temperature,
frequency and unrelated-host-load telemetry was not collected, so the outliers
cannot be assigned exclusively to the pool implementation.

The `usm` matrix holds the zimg grid fixed. The `zimg`, `balanced`, and `wide`
matrices can change that grid and therefore can change resampling seams. The
runner verifies that the final frame's visible-output hashes agree wherever input, geometry,
graph grid and sharpening state match. Different grids must not be ranked as
quality-equivalent configurations merely because one is faster.

Each trace array contains these values, in microseconds:

| Position | Measurement |
|---|---|
| 0 | Dispatch entry to completed dispatch return |
| 1 | Dispatch entry to first callback start |
| 2 | Dispatch entry to last callback start |
| 3 | Shortest callback wall duration |
| 4 | Longest callback wall duration |
| 5 | Mean callback wall duration |
| 6 | Last callback finish to completed dispatch return |
| 7 | Sum of callback thread CPU time; available only in detail mode 2 |

Callback start delay includes publication, waking, mutex reacquisition and
scheduling. Callback wall time includes any descheduling during its execution.
The final handoff includes completion accounting, signaling and the caller's
return to execution. These fields are not an additive critical-path breakdown:
the latest-starting callback need not be the latest-finishing callback.

Per-thread `/proc/self/task/<tid>/schedstat` deltas report CPU execution time,
runqueue wait time and scheduling slices. These are sums across workers, not
single-frame wall-time components. Their definitions follow the
[kernel scheduler documentation](https://www.kernel.org/doc/html/latest/scheduler/sched-stats.html#proc-pid-schedstat).
Context-switch counts and process CPU time include coordinator and worker work.
An empty dispatch establishes a coordination workload, not the removable
overhead inside a busy image-processing dispatch.

## Privileged collection

Hardware counters and kernel scheduling traces require access unavailable to
the normal account on the evaluation host. `scripts/profile_perf.sh` runs perf
with elevated privileges and the benchmark as the invoking normal user. It
does not change sysctls or install file capabilities. A ready-file coordinates
with other benchmark runs so they do not overlap.

```sh
touch build-profile/perf.ready
sudo bash scripts/profile_perf.sh "$(realpath build-profile)" \
  "$(realpath build-profile)/perf-results" "$(realpath build-profile)/perf.ready"
```

The script collects repeated hardware-event counts, sampled CPU call stacks,
scheduler latency traces, and an AMD IBS cache-to-cache attempt. An unsupported
IBS capture is retained as a diagnostic. Inspect event availability and running
percentages before interpreting rates. Cache misses alone do not establish
false sharing; cache-line addresses and access sites are needed, as described
in the [kernel false-sharing guide](https://www.kernel.org/doc/html/latest/kernel-hacking/false-sharing.html).

An optional fourth argument, `attribution`, repeats only CPU samples and
cache-to-cache captures in a fresh output directory. It skips hardware-stat and
scheduler recordings. The current script uses 199 Hz CPU sampling and a larger
perf buffer; the initial captures above used 499 Hz and the default buffer.

Kernel scheduler traces may contain other host tasks. Keep the raw trace local
and publish only the benchmark's extracted statistics. Syscall tracing and CPU
sampling are separate diagnostic runs; their elapsed times must not replace
the ordinary frame-latency measurements.
