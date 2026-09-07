# Performance and benchmarking

This is the canonical performance measurement guide. Measure on the deployment
host: compiler, flags, CPU affinity, governor, temperature, and background load
can outweigh small code differences. This repository therefore does not publish
fixed throughput claims.

## Run

```sh
make build-bench                 # compile every available benchmark
make bench
make bench-usm-halo
make bench-worker-pool
make bench-pipeline
make bench-flatskip
make bench-zimg
scripts/bench_matrix.sh build/bench_usm_pool
scripts/bench_zimg_pinning.sh build/bench_scaler_zimg
```

`bench`, `bench-usm-halo`, `bench-worker-pool`, and `bench-flatskip` need only
the normal compiler toolchain. `bench-zimg`, `bench-pipeline`, and the zimg
pinning script require the zimg and VLC development files. The pinning script
also requires `taskset`; review its built-in affinity masks before running it.

Run a benchmark binary without arguments to print its current interface (and
exit with status 2). The USM benchmark supports `rand`, `flat`, and `mixed`
input fills. The matrix script accepts optional frame-count, amount, and fill
arguments.

`bench-usm-halo` compares out-of-place (`out`) against in-place (`in`) runs at
the same shapes. Their delta includes the serial halo-row snapshots required by
in-place processing and the different cache/write traffic; use it as a trigger
for profiling, not as an isolated snapshot-time measurement.

To compare ISA variants without letting the runtime dispatcher hide their
individual costs, build the same benchmark three times and run the paired
matrix:

```sh
make BUILD=build_dev/isa-sse2 MARCH=x86-64 build-bench
make BUILD=build_dev/isa-avx2 MARCH=x86-64-v3 build-bench
make BUILD=build_dev/isa-avx512 MARCH=x86-64-v4 build-bench
scripts/bench_usm_isa.sh build_dev/isa-sse2/bench_usm_pool \
  build_dev/isa-avx2/bench_usm_pool build_dev/isa-avx512/bench_usm_pool
```

Set `BENCH_FRAMES` or `BENCH_AMOUNT` to override the matrix defaults. Run each
matrix repeatedly under the same governor and load before changing dispatch.
The ISA script prepends `isa` to the benchmark's eight fields:

```text
isa,requested_threads,effective_threads,width,height,frames,amount,fill,us_per_frame
```

`bench_usm_pool` emits:

```text
requested_threads,effective_threads,width,height,frames,amount,fill,us_per_frame
```

The matrix emits three validated raw samples and their median:

```text
variant,requested_threads,effective_threads,width,height,frames,amount,fill,row_type,run_index,us_per_frame
```

Filter `row_type=median` for comparisons, but retain the raw rows. Use
`effective_threads`, not the request, because geometry and startup can reduce
the pool size.

## Adaptive USM

Enable the experimental playback option with
`--autoupscale-threads=0 --autoupscale-adaptive-usm=1`. Sharpening must be active. Explicit worker counts
remain fixed. The objective is lower processing time, with no resolution or
algorithm change. This first implementation tunes USM; zimg's grid stays fixed
because repartitioning its graphs can change resampling seams.

The search tries 1, 2, 4, 8, 12, 16, 24, 32, 48 and 64 workers, clipped to CPU
and stripe limits. It skips two warmup frames after each switch and compares
16-frame medians. A candidate must beat both neighboring baseline windows by
5%. A trial whose first four measured frames have a median over 1.5 times the
baseline returns early. Settled operation lasts at least 256 measured frames;
three windows outside 65–135% of the settled timing trigger a new search.
Periodic exploration resumes after 16,384 measured frames. Samples cover both
scaling and sharpening on frames that execute USM, so a local USM improvement
must also improve the combined processing time. Playback pacing and idle time
do not contribute samples.

Pool creation/retirement is excluded from the controller's steady samples but
included in this benchmark's frame totals. Resource or clock failure stops
adaptation and retains the working pool. The mode is off by default because
short clips, noisy measurements and exploration costs can outweigh gains.

```sh
make build/bench_adaptive
build/bench_adaptive 12 4096 1920 1080 12
build/bench_adaptive -1 4096 1920 1080 12
```

Arguments: USM count (`-1` adaptive), frames (minimum 1024), output width,
output height, fixed zimg count. Input is half the output dimensions, I420,
with deterministic noise and 20% sharpening. Width/height must be multiples
of four. CSV columns:

```text
usm_request,frames,width,height,zimg_workers,effective_usm,first_settled_frame,changes,total_us,tail_us,zimg_us,usm_us
```

`total_us` includes lazy startup and all transitions. `tail_us` covers the final
512 frames, which may still include adaptation. The stage means include their
startup costs. These are processing measurements, excluding decoding, display,
audio and VLC output-picture allocation. Compare rotated repeated runs on the
deployment host; a final selected count alone does not prove an improvement.

### Experimental snapshot

On 2026-09-07, an AMD Ryzen 9 7945HX with GCC 13.3 (`-O3 -march=native`)
ran five rotated repetitions per count, each with 4096 frames at 960×540 to
1920×1080, I420, Spline36 and 20% USM. The zimg count stayed at the static
AUTO value for each affinity mask. These are medians of run means, including
startup and exploration, in microseconds per frame:

| Allowed CPUs | Fixed AUTO USM | Adaptive USM | Adaptive selected | Best tested fixed USM |
|---|---|---|---|---|
| 32 | 344.71 (12 workers) | 351.67 | 12 in all runs | 344.71 (12 workers) |
| 8 (CPU IDs 0–7) | 887.70 (2 workers) | 795.68 | 4 in all runs | 788.49 (4 workers) |

Adaptation reduced total processing time by 10.4% with eight allowed CPUs,
and added 2.0% when the 32-CPU static default was already the best tested count.
It remained about 0.9% slower than the best tested fixed count in the eight-CPU
case. This supports an opt-in experiment, not a universal speedup claim.
[Raw repeated measurements](benchmarks/adaptive-2026-09-07.csv) preserve the
stage means and final-window timings. No network BBR implementation or untuned
PID was benchmarked; these results do not establish superiority over either.

### BBR research and transfer limits

The [original Google BBR paper](https://web.stanford.edu/class/cs244/papers/bbr.pdf)
separates bandwidth and propagation-delay estimation, excludes misleading
application-limited samples and alternates probing with steady operation.
For this serial frame pipeline, completed work per processing second is useful;
display FPS is capped by the source and is a poor capacity signal. There is no
independent network propagation delay or in-flight byte window from which to
derive a worker count. Minimum frame time is also not a reliable estimate of
typical processing cost under cache, scheduler and content variation. Medians
and confirmation are deliberate choices here, rather than BBR's extrema filters.

[Google's IETF 101 update](https://www.ietf.org/proceedings/101/slides/slides-101-iccrg-an-update-on-bbr-work-at-google-00.pdf)
motivates bounded probing, time to recover after probes and avoiding synchronized
probes among competing flows. The prototype borrows bounded trials and cooldown;
it does not implement TCP BBR or claim its published network gains. Randomized
probe timing would need validation with multiple simultaneous playback instances.

The [TUM 2025 survey](https://www.net.in.tum.de/fileadmin/TUM/NET/NET-2025-05-1/NET-2025-05-1_17.pdf)
maps the evolution through v3. Implementation details should come from primary
sources such as the [BBRv3 draft](https://datatracker.ietf.org/doc/draft-ietf-ccwg-bbr/06/),
which separates probing/cruising phases and short/long-term bounds. Those ideas
justify testing recovery and changing load; their gains and time constants are
network-specific and are not copied into the worker tuner.

The [NJIT ML paper](https://arxiv.org/pdf/2312.11790) concerns inter-protocol
fairness and latency classification. Its reported classifier accuracy does not
establish better closed-loop worker control. A trained model would add dataset,
generalization and inference-cost requirements without demonstrated benefit to
this plugin. Contradictory loss and evaluation descriptions in the references
are recorded in `TODO.md` as SCAL-10; zimg adaptation is tracked as SCAL-11.

## Compare

For each result, record:

- commit and worktree state;
- compiler version and complete flags;
- CPU model, affinity mask, governor, and kernel;
- library versions and selected SIMD variant;
- every raw sample and relevant system load.

Compare the same workload on the same idle host. Repeat runs until the ordering
is stable; treat small differences as noise. Frame time must fit alongside
decode, scaling, display/encode, and other pipeline work.

More workers are not necessarily faster. Dispatch dominates small stripes;
memory bandwidth dominates large frames. Compare compiler and SIMD builds in
clean build directories.

The engagement log reports `simd=default`, `sse2`, `avx2`, or `avx512`. A
multiversion build can verify retained instruction sets with:

```sh
make MARCH=x86-64 MULTIVERSION=1 check-multiversion-isa
```

`bench-flatskip` is experimental and not byte-identical to the production
kernel for all content. Do not combine its numbers with production claims.
Cross-variant production output equivalence is enforced by
`tests/test_usm_pool_variants.c`.

The pinning matrix compares scheduler placement with current first-allowed-CPU
pinning under all-logical-CPU and one-thread-per-core affinity masks. Its
default masks fit the 32-thread/16-core reference host; edit them to match the
measured machine. Repeat the matrix before changing the default or a
deployment's pinning setting.

The zimg benchmark also reports first-frame lazy initialization in
microseconds and process maximum resident set in KiB before steady-state frame
time. Compare separate process runs by geometry, chroma, zero-copy mode, and
worker count; `ru_maxrss` includes the harness and libraries, so compare deltas
rather than treating it as backend-only allocation.

`bench-zimg` emits:

```text
threads,chroma,src,dst,frames,zc,pin,lazy_us,max_rss_kb,us_per_frame
```

`src` and `dst` are `WIDTHxHEIGHT`. `zc` controls direct destination writes;
the harness always reads the source directly. `pin` controls best-effort zimg
worker pinning.

`bench-worker-pool` times the shared dispatch gate with deliberately tiny
callbacks, exposing the upper bound of wake/barrier overhead by worker count
without mixing in scaler work. Its benchmark-only completion hook also reports
the last dispatch's earliest-to-latest worker completion skew. It is a
microbenchmark, not an end-to-end frame latency result.

`bench-pipeline` runs zimg followed by in-place USM, matching their sequential
production order while keeping both persistent pools alive. It reports the
combined first-frame initialization, process RSS, and steady frame time by
worker count. Its interface is `<threads> [frames] [pin] [zimg-lines]
[usm-lines]`; zero for either stripe value selects its production default.
It emits:

```text
threads,frames,pin,zimg_lines,usm_lines,lazy_us,max_rss_kb,us_per_frame
```

Changing zimg stripe lines can change the row/column grid, including whether
source-direct and copy-in modes use the same independent graphs. Compare output
quality as well as timing before changing the validated 16-line default.
