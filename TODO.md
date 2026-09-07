# TODO — full-project audit findings

## security

| id | status | effort | description | notes |
|---|---|---|---|---|

## undefined behavior

| id | status | effort | description | notes |
|---|---|---|---|---|

## memory management

| id | status | effort | description | notes |
|---|---|---|---|---|

## performance

| id | status | effort | description | notes |
|---|---|---|---|---|
| PERF-10 | blocked | M | Align adaptive worker selection with the requested minimum average time per frame. | The user objective and `docs/BENCHMARKS.md` compare total processing time, but `src/worker_tuner.h:54-66,93-118` selects and monitors only medians. A deterministic ASan/UBSan probe starting at 2 workers selects 4 and remains settled: baseline frames cost 100 us; each candidate window has eleven 50-us frames followed by five 1000-us frames, giving a 50-us median but a 346.88-us mean. The four-sample early abort and median drift checks both miss the recurring stalls. Unblock by reconciling the selection statistic with total time/frame and an explicit tail-latency policy; verify both sustained gains and this recurring-stall counterexample. |
| PERF-11 | open | S | Make the adaptive benchmark's always-on sharpening prerequisite explicit and validate sustained playback gains. | `tests/bench_adaptive.c` applies USM to deterministic noise for every frame without the production content probe. The exact 960x540 fixture has `lap_mean=108893`, above the default 3500 cutoff; `RunProbe` in `src/autoupscale.c` disables USM, destroys its pools and stops tuning at frame 60 for this input. The 4096-frame experiment therefore models sustained sharpening only with a threshold override, not the same input under the normal sharpness gate. Document `--autoupscale-usm-sharp-threshold=0` for a matching synthetic playback reproduction and add representative video measurements where sharpening remains active. |
| PERF-12 | blocked | S | Complete cache-line attribution before choosing a shared-nothing coordination prototype. | The privileged stat and scheduler captures succeeded; [870 timing runs and hardware results](docs/PROFILING.md) quantify wake delay and reduced high-count efficiency. The first `perf c2c` command rejected the invalid `ibs_op//` selector, and the 32-worker 499-Hz CPU capture lost 14 samples. `perf c2c record -e list` confirms `mem-ldst` is available, but the normal account still cannot open its PMU (`perf_event_paranoid=4`). Unblock by running the corrected `scripts/profile_perf.sh` with its `attribution` mode and a fresh output directory, then correlating cache-line addresses with gate/pixel access sites and checking sample loss. Generic cache misses and IPC alone cannot establish false sharing or DRAM bandwidth saturation. |
| PERF-13 | open | M | Compare bounded USM affinity and stage-to-stage locality policies on representative paced input. | With the same 8x1 zimg grid and 12 USM workers at 1080p, five-run median means are 278.9 us on eight physical cores sharing one L3 versus 404.3 us across four physical cores in each of two L3 domains; the local placement also wins at USM counts 4/8/16/32. [Raw results and limits](docs/PROFILING.md) retain the identical-grid hashes and actual CPU masks. This changes whole-process placement and does not isolate cache effects from scheduler/coordinator/frequency effects. Compare a benchmark-only USM placement policy with the existing scheduler-managed pool before introducing production topology discovery, preserving pixels and measuring mean/tails/CPU on multiple hosts; no universal affinity policy is established. |
| PERF-15 | blocked | M | Reconcile the adopted static USM presets' continuous-loop gains with paced-playback regressions. | Twenty additional shuffled runs at 60 fps (five repeats/configuration, 240 measured frames, 128 warmup frames, Spline36/I420/20% USM, fixed 12-worker zimg) give median run means of 348.70 versus 291.35 us/frame for 720p eight versus twelve workers, and 1255.30 versus 1190.25 us/frame for 4K sixteen versus twelve. These are slower by 19.7% and 5.5%, contrary to extrapolating the earlier continuous-loop gains to playback; p95/p99 also worsen in this sample. [Measurements and scope](docs/PROFILING.md#adopted-usm-auto-presets) retain both results. The user authorized the fixed 8/12/16 policy provisionally. Unblock with representative paced-video and CPU-budget comparisons, then retain or revise the affected presets based on combined mean/tails and CPU cost; no universal or regression-free playback gain is established. |

## scalability

| id | status | effort | description | notes |
|---|---|---|---|---|
| SCAL-10 | blocked | M | Reconcile BBR reference contradictions before using them to justify an ML or loss-feedback extension. | [TUM survey §3.4.1](https://www.net.in.tum.de/fileadmin/TUM/NET/NET-2025-05-1/NET-2025-05-1_17.pdf) describes BBRv1 as completely ignoring loss; [Google IETF 101 slide 17](https://www.ietf.org/proceedings/101/slides/slides-101-iccrg-an-update-on-bbr-work-at-google-00.pdf) explicitly says both v1 and v2 use it. [NJIT §3.2](https://arxiv.org/pdf/2312.11790) evaluates the neural model on the same data, whereas §4 attributes accuracy to validation data. Unblock by identifying the exact BBR version and signal path in primary code/specification, and obtaining a reproducible held-out ML evaluation plus end-to-end control measurements. Conservative measured USM trials do not depend on either disputed claim. |
| SCAL-11 | blocked | L | Extend adaptive tuning to zimg without changing output pixels. | `src/scaler_zimg.c` couples one worker to each precomputed graph/grid cell; changing the worker preference can change seams, conflicting with stable quality during tuning. The USM prototype keeps this grid fixed. Unblock with scheduling that varies active workers over an unchanged graph grid, ownership/failure tests, byte-identity checks and combined scaler/USM latency benchmarks; existing rejection of dynamic queues must be revisited if that implementation is proposed. |

## concurrency

| id | status | effort | description | notes |
|---|---|---|---|---|

## code complexity

| id | status | effort | description | notes |
|---|---|---|---|---|

## code duplication

| id | status | effort | description | notes |
|---|---|---|---|---|

## architecture/modularity/SOLID

| id | status | effort | description | notes |
|---|---|---|---|---|
| ARCH-13 | blocked | S | Correct the documented owner of backend fallback. | `docs/ARCHITECTURE.md:47` says `scaler.c` owns selection and fallback; `src/scaler.c` only selects a supported backend, while `OpenScalerOrFallback` and `TryBackendFallback` in `src/autoupscale.c:235,652` own open/runtime recovery. Unblock by aligning the backend-contract description with those production call sites. |
| ARCH-14 | blocked | M | Benchmark a bounded notification/completion alternative before extending shared-nothing isolation. | [Profiling](docs/PROFILING.md) finds the current private graphs/scratch/stripes suitable: at 1080p, 12-to-32-worker zimg last-start delay rises from 23.45 to 68.77 us and USM from 14.31 to 79.54 us, while final handoff remains about 5 us. Unpinned empty dispatch grows from 19.99 to 50.58 to 108.56 us at 12/32/64 workers. Complete PERF-12, then compare per-worker notification/completion with the existing gate at identical grid, pixels, worker counts and kernels; require mean/p95/p99, CPU cost, paced input and shutdown/failure checks. These measurements do not establish removable overhead or a shared-nothing speedup. Per-worker ownership already exists; a process/distributed rewrite has no demonstrated single-frame latency benefit and adds IPC/buffering. Revisit prior alternate-wake rejections only against this new high-count evidence. |

## decoupling

| id | status | effort | description | notes |
|---|---|---|---|---|

## business/design patterns/DDD

| id | status | effort | description | notes |
|---|---|---|---|---|

## reliability/correctness

| id | status | effort | description | notes |
|---|---|---|---|---|
| REL-16 | blocked | M | Restore live VLC volume controls for the transcode-display profile; displayed control changes conflict with the unchanged audible stream. | Confirmed product limitation for the Nemo profile, not an upscaler DSP defect: [VLC 3.0.20 display.c:102,150](https://github.com/videolan/vlc/blob/3.0.20/modules/stream_out/display.c#L102) gives its decoder a private resource; [playlist/aout.c:43-46,65-74](https://github.com/videolan/vlc/blob/3.0.20/src/playlist/aout.c#L43) controls the playlist resource instead. The earlier CLI matrix reproduced unchanged audio under RC volume changes; core/audio-filter gain at 0.25 attenuated it by 12 dB. Unblock by choosing requirements: system mixer plus documented fixed `--gain` is a workaround; a normal receiving VLC process restores the correct ownership but live piping adds process/lifecycle work and loses ordinary file seeking (pre-transcoding preserves seeking at storage/startup cost); a VLC control-routing patch can preserve native controls, one process, and seeking but needs maintained integration and lifetime/feedback tests. Direct playback avoids private audio ownership, but preserving enlarged video then needs separate VLC vout integration. Changing `--aout`, volume step, or disabling sout audio does not repair ownership. |
| REL-21 | blocked | M | Honor the configured upscale target while respecting VLC's output-format permission. | User decision: the plugin configuration is authoritative; a successful upscale must deliver that target, without silently substituting the caller's dimensions. `src/autoupscale.c:373,416-468` overwrites output geometry without reading `b_allow_fmt_out_change`; the sanitizer lifecycle probe accepts 320x180 to 1280x720 with the flag false, contradicting the [chain API](https://github.com/videolan/vlc/blob/3.0.20/include/vlc_filter.h#L320). Resolve by accepting an already matching requested output or an authorized change, and explicitly rejecting incompatible fixed-output requests before allocation or state mutation. Keep the permission flag owner-controlled. [Normal video-filter chains](https://github.com/videolan/vlc/blob/3.0.20/src/video_output/video_output.c#L1509) and [transcode user filters](https://github.com/videolan/vlc/blob/3.0.20/modules/stream_out/transcode/video.c#L346) pass true; delivering the configured dimensions through the final display also requires a compatible path, since direct playback can resize afterward. The policy decision is settled; unblock by implementing and verifying the contract, including both flag values, matching/mismatching requested outputs, crop/chroma mismatches, and actual direct/transcode output dimensions. |

## portability/standards conformance

| id | status | effort | description | notes |
|---|---|---|---|---|

## error handling

| id | status | effort | description | notes |
|---|---|---|---|---|

## resource management

| id | status | effort | description | notes |
|---|---|---|---|---|

## API/ABI stability

| id | status | effort | description | notes |
|---|---|---|---|---|

## build/toolchain hygiene

| id | status | effort | description | notes |
|---|---|---|---|---|
| BUILD-40 | open | S | Preserve or deliberately clear GNU Make jobserver state in shell regression fixtures. | `make -j4 check` reports jobserver-unavailable warnings from nested Make invocations in the install/benchmark shell tests. Mark recursive recipes appropriately or sanitize inherited jobserver flags for isolated fixtures; verify parallel checks without these warnings. |
| BUILD-38 | blocked | S | Run `fuzz_plane_buffer` in the CI libFuzzer job. | `Makefile:666` includes `plane_buffer` in `FUZZ_TARGET_NAMES`, but `.github/workflows/ci.yml:183-247` never executes it, contradicting the workflow's claim that every built fuzzer receives a pass. Deterministic smoke coverage exists, but mutation-guided CI coverage is missing. Unblock by adding it to the pure-logic run list and verifying the built/run target sets match. |
| BUILD-39 | blocked | S | Align the per-function coverage aggregation description with its actual gate. | `scripts/coverage_per_function.sh:7-10` promises the best coverage from one binary, but lines 127-150 union covered lines across binaries before computing each function's percentage. Complementary partial tests can therefore pass although neither binary meets the documented threshold alone. Unblock by selecting the intended aggregation contract and updating the description or implementation, with a complementary-coverage fixture. |

## observability

| id | status | effort | description | notes |
|---|---|---|---|---|
| OBS-13 | blocked | S | Qualify source-zero-copy output equivalence in VLC's option help and test descriptions. | `src/autoupscale_module.c:108-121` claims byte identity with copy-in, while `README.md`'s zero-copy option and `src/scaler_zimg.c:30-34` restrict that guarantee to unchanged grids. `tests/test_scaler_zimg.c:511-540` retains an unconditional identity title but explicitly exempts column-tiled cases from copy/direct equality. Unblock by stating the same-grid condition and the grid-change seam caveat consistently. |
| OBS-14 | blocked | S | Correct stale pinning comments that contradict the shipped default. | `src/scaler_zimg.c:107-108,690` describes pinning as opt-in, while the `pin-threads` registration in `src/autoupscale_module.c` enables it by default. Unblock by synchronizing the comments with the runtime option default. |
| OBS-15 | blocked | S | Include allocated column-tile destination buffers in the zimg scratch diagnostic. | `src/scaler_zimg.c:802-826` reports zero destination scratch whenever column tiling is active, omitting the buffers allocated at lines 624-630. An ASan/UBSan backend probe for I420 4096x32 to 16384x128, 32 workers, source/destination zero-copy enabled produced an 8x4 grid with 5,242,880 allocated tile bytes but logged `scratch 0 MB (src zero-copy, dst tiled+copy), graph-tmp 2 MB`. Unblock by including per-worker `tile_dst` bytes and testing the reported total against the allocated layouts. |
| OBS-16 | open | S | Report when the adaptive benchmark falls back to a fixed pool. | `tests/bench_adaptive.c:72-110` checks frame success but never reports `adaptive.stopped` or `adaptive.enabled`. Link-wrap fault injection rejecting the second `up_usm_pool_create` call produces exit 0 and an ordinary `-1` adaptive CSV row despite adaptation stopping before its first trial. `first_settled_frame=0` is ambiguous with unfinished exploration and cannot reveal a failure after settling. Add explicit run outcome/stop reason to the CSV and distinguish fallback runs in comparisons; test failures both before and after settling. |
| OBS-17 | blocked | S | Reconcile the adaptive benchmark's actual profile with its published measurements and playback defaults. | `docs/BENCHMARKS.md:120-123` labels the committed experiment Spline36, but `tests/bench_adaptive.c:52-53` calls `zt_ctx_init`, which sets `UP_ALGO_LANCZOS` and leaves `pin_cpus=0`; neither value is overridden. The plugin defaults are Spline36 and pinning enabled (`src/autoupscale_module.c:219,235`). The CSV omits both settings, so the reported 10.4% gain does not establish the result for that claimed/default profile. Unblock by exposing and recording algorithm/pinning, correcting the existing experiment label, and repeating comparisons for the intended playback profile. |

## wiring gaps

| id | status | effort | description | notes |
|---|---|---|---|---|

## unused functions/methods

| id | status | effort | description | notes |
|---|---|---|---|---|
| UNUSED-6 | open | S | Remove obsolete variable-mutation shims from the lifecycle VLC stub. | `tests/lifecycle_stubs/vlc_common.h:10-12,19-43` declares `lifecycle_var_create/destroy/set_integer` and defines forwarding helpers/macros for `var_Create`, `var_Destroy`, and `var_SetInteger`, but repository-wide searches find no consumers or implementations. Only the inheritance shim is used by the current lifecycle code. |

## Audit picks deliberately rejected

Recorded so future full-project rescans do not repeatedly promote the same
non-findings:

- **SCAL-1a--SCAL-1d, cgroup CPU-aware worker sizing:** rejected. The existing
  affinity-aware count, 12-worker automatic cap, and explicit user override
  handle the supported desktop workload; robust v1/v2/hybrid cgroup discovery
  would add proc/sysfs parsing and failure policy with no measured need.
- **SCAL-2a--SCAL-2d, cgroup memory-aware AUTO target selection:** rejected.
  AUTO deliberately does not inspect host or cgroup RAM: user configuration
  selects quality, and allocation failures follow the established graceful
  failure path. A memory-derived target would violate that runtime policy; no
  memory-pressure regression was reproduced within the bounded worker setup.
- **SCAL-3b--SCAL-3d, topology-aware pin ordering:** rejected. Pinning the
  existing allowed-CPU order is byte-equivalent and improved the measured host;
  sibling/NUMA discovery would be Linux-specific policy without a demonstrated
  production contention benefit.
- **SCAL-P1d, dynamic work queues:** rejected. Completion skew occurs even with
  no partitioned work, identifying scheduler wake delay rather than unequal
  tile cost; a queue would add synchronization and failure paths without a
  measured problem it can solve.
- **REL-17, distinct AutoUpscale launcher icon:** rejected. The entry is
  intentionally a visibly named VLC launch profile, keeps VLC's icon to identify
  the actual player, and uses a separate desktop ID without changing stock VLC
  or MIME defaults. A custom icon would add packaging and maintenance for a
  cosmetic ambiguity that the `VLC (AutoUpscale)` name already distinguishes.
- Adding a CI threshold for in-place USM halo snapshots: the paired alias-mode
  benchmark found no consistent in-place regression on the measured host, and
  cache/write differences prevent the comparison from isolating copy cost.
- Moving USM halo snapshots into workers: PERF-P1a found no repeatable
  bottleneck, so an extra readiness phase would add synchronization risk
  without measured benefit.
- Defining a max-ISA dispatch policy: repeated Ryzen 9 7945HX matrices found
  AVX-512 faster than AVX2 in 17 of 18 medians and effectively tied in the
  other; widest-supported dispatch remains the evidence-backed policy.
- Adding a load-time max-ISA override: no wider-ISA regression was confirmed,
  while single-baseline builds already provide a diagnostic workaround.
- Adding a zimg resource budget: measured 1--16-worker 720p runs stayed below
  9 MiB process RSS; lazy startup rose from about 3 ms to 13 ms but is a
  bounded one-time cost, not evidence for reducing steady-state parallelism.
- Applying a zimg resource-derived worker cap: the measured resource budget
  did not cross a material threshold, so no cap value has a valid basis.
- Sharing zimg graphs or temporary buffers: the prerequisite worker cap was
  rejected, and measured memory remains bounded without concurrency coupling.
- Splitting one worker budget between sequential zimg and USM pools: combined
  pipeline RSS stayed below 8 MiB, while dividing the measured 12-worker knee
  would reduce each stage's available parallelism without lowering frame work.
- Adding a backend-neutral worker job descriptor: the independent-pool
  measurements found no material resource pressure requiring lifecycle reuse.
- Sharing one persistent worker lifecycle between zimg and USM: its job
  descriptor prerequisite was rejected, and the lifecycle/failure coupling
  would be disproportionate to the measured sub-8-MiB combined footprint.
- Adding weighted static partitions for worker skew: the no-work completion
  benchmark reproduced skew, identifying wake/scheduling delay rather than
  unequal stripe cost; changing geometry cannot make late workers start sooner.
- Integrating weighted static partitions: the prototype prerequisite was
  rejected because measured completion skew exists without partitioned work.
- Replacing condition broadcast with tree wake: after the 12-worker cap, the
  measured pipeline still gained about 28 us/frame from 8 to 12 workers while
  the empty-dispatch cost was about 17 us; current wake remains net-positive
  without adding another failure-sensitive synchronization topology.
- Integrating an alternate wake strategy: no prototype beat broadcast, so
  there is no winning strategy to place behind the shared pool gate.
- Changing the zimg automatic stripe minimum from 16 to 24: on the tiny
  64x64-to-128x128, eight-worker I420 case, 16 keeps copy and source-direct
  paths at an 8x1 grid, while 24 changes them to 5x1 and 4x2 respectively.
  Their independent graph phases differ in 20,515 visible bytes (maximum delta
  255 on noise); source-direct and full-zero-copy remain byte-identical. A
  current five-pair, interleaved 600-frame pipeline sample also made 24 slower
  (median 150.44 versus 141.39 us/frame), so neither quality nor performance
  supports changing the default.
- Unifying `worker_copy_in_stripe` / `worker_copy_out_stripe` /
  `worker_copy_out_tile`: their direction and offset invariants differ; a generic
  helper would require a wide parameter surface.
- Adding a pool-wide cleanup hook solely because `prepare` is a pool callback:
  shared allocations are explicitly owner-owned and both owners release them;
  permanent-failure paths now initiate retirement immediately.
- Treating `lazy_done` / `lazy_failed` as an atomicity defect: VLC's video-filter
  callback contract serializes the lifecycle; revisit only if that contract
  changes.
- Removing per-worker boundary-row re-blur: the small duplicate computation is
  the invariant that permits barrier-free, race-free in-place USM.
- Adding alignment assumptions to USM vector loops: valid row alignment depends
  on width/stride, and prior measurements found no actionable gain over
  unaligned moves.
- Treating zimg temporary-buffer alignment as an overflow seam: zimg 3.0.5 uses
  checked internal size arithmetic, graph dimensions are capped at 32768, and
  returned x86 temporary sizes are already 64-byte aligned; the value cannot
  approach `SIZE_MAX - 63` on a reachable production graph.
- **ARCH-R1, full Rust rewrite:** rejected for the current implementation. The
  VLC 3 descriptor, picture-plane access, zimg/libswscale calls, CPU-feature
  dispatch, affinity, and persistent worker publication would retain a material
  `unsafe` FFI/concurrency surface, while replacing a mature 6.5k-line
  production implementation inside a 22k-line source-and-test system. The
  2026-08-20 evaluation found no open UB, memory-management, or concurrency
  defect and re-ran `make check plugin check-hardening check-visibility
  BUILD=build-rust-eval EXTRA_CFLAGS=-Werror` successfully. Revisit only for a
  measured hot-path win, recurring C safety defects, a Rust-first maintainer
  base, or a broader portability goal; use a small differential-tested kernel
  prototype before considering migration.
