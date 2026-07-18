# TODO — full-project audit findings

Full-project rescan of HEAD `9069eef` and its clean starting tree on 2026-07-18.
Scope: all 259 tracked project files: production source/public headers, tests,
fuzzers, 148 corpus seeds, build/release files, workflows, scripts, documents,
and patches. Excluded `.git`, ignored/generated build outputs, and cache
directories/files.

Validation: manual review across every category, every production source/header,
test/fuzzer, build/workflow/script/doc/patch, and every change since `6062482`;
GCC C11 unit/contract and deterministic fuzz-smoke suites with `-Werror`, ASan,
and UBSan; zimg integration and seam fuzz-smoke with ASan/UBSan; USM and zimg
ThreadSanitizer stress under disabled ASLR; all 148 curated seeds replayed under
Clang libFuzzer; GCC and Clang single-/multi-version plugin, ABI, visibility,
hardening, and linked-ISA checks; 98.8% gated production line coverage with all
177 tracked functions above 80%, plus 95.0% informational zimg coverage;
clean Cppcheck and ShellCheck runs; Lizard 1.23.0 over 966 functions (zero CCN
above 10, maximum 10); declared-interpreter shell syntax and workflow YAML
parsing; relative Markdown-link/fragment, corpus size/hash, unsafe-API,
ownership, return-value, symbol, and configuration-consumer searches.
`scan-build`, actionlint, rumdl, lychee, and clang-tidy were unavailable;
their gaps are retained where material. Row format:
`id | status | effort | description | notes`.

## security

| id | status | effort | description | notes |
|---|---|---|---|---|
| SEC-5 | open | S | `tests/test_install_action.sh:5-14` creates the predictable `${TMPDIR:-/tmp}/vlc-autoscaler-install-test.$$` tree, recursively removes it, writes executable `PATH` stubs there, and invokes the installer. | This finding was removed from the tracker without an implementation. In a shared temporary directory, another user can pre-create or race components to redirect writes or replace an executed stub. Use `mktemp -d`, arm cleanup only after successful creation, and make signal traps clean up and exit. |

No additional production-path security issue was found: geometry is validated
before pointer formation, user integers are normalized or clamped, format
strings are literals, and allocation dimensions are bounded.

## undefined behavior

| id | status | effort | description | notes |
|---|---|---|---|---|

No open UB finding after tracing shifts, allocation arithmetic, crop and
plane bounds, fixed-point rounding, worker lifetimes, atomics, and in-place USM
halo ownership.

## memory management

| id | status | effort | description | notes |
|---|---|---|---|---|

No open finding. Normal, partial-construction, lazy-failure, poison, fallback,
and close paths retain explicit ownership and paired releases.

## performance

| id | status | effort | description | notes |
|---|---|---|---|---|

No additional unparked finding: steady-state processing allocates no per-frame
backend state and does not rebuild graphs.

## scalability

| id | status | effort | description | notes |
|---|---|---|---|---|

No additional scalability finding.

## concurrency

| id | status | effort | description | notes |
|---|---|---|---|---|

No open finding. Gate publication, the completion release sequence,
cancellation cleanup while blocked, partial spawn, and normal teardown showed
no data race or valid-state deadlock. The completion deadline is explicitly
not an end-to-end callback bound; recovery joins active callbacks before
releasing their storage. USM's 27 configurations and the zimg invariant harness
also passed locally under ThreadSanitizer with ASLR disabled.

## code complexity

| id | status | effort | description | notes |
|---|---|---|---|---|

No open finding. The current full `src/` + `tests/` Lizard analysis reports
966 functions, zero CCN violations, and a maximum CCN of 10.

## code duplication

| id | status | effort | description | notes |
|---|---|---|---|---|

No additional duplication finding.

## architecture/modularity/SOLID

| id | status | effort | description | notes |
|---|---|---|---|---|

No open finding. Backend strategy, shared worker lifecycle, chroma descriptor,
and SIMD dispatch boundaries remain coherent.

## decoupling

| id | status | effort | description | notes |
|---|---|---|---|---|

No open finding. VLC-facing orchestration, pure helpers, scaler backends, and
worker-pool lifecycle remain appropriately separated at their current seams.

## business/design patterns/DDD

| id | status | effort | description | notes |
|---|---|---|---|---|

No open finding. The scaler strategy table and load-time SIMD dispatch table fit
this technical domain; another business/domain pattern would not clarify it.

## reliability/correctness

| id | status | effort | description | notes |
|---|---|---|---|---|
| REL-16 | open | M | VLC 3.0.20's `:display` stream-output path creates a private input resource and audio output, so the VLC GUI, hotkeys, and RC volume controls target the separate playlist-owned audio output and do not change the audible transcoded stream. | 2026-07-18 CLI matrix confirmed that RC `volume`/`voldown`, `--volume-step`, `--aout`, and `--sout-display-audio` do not repair routing; `--volume` is obsolete, and `--no-sout-display-audio` removes audio. `--gain=0.25` and `--audio-filter=gain --gain-value=0.25` each attenuated the audible stream by the expected 12 dB, while replay-gain also applied, so `--gain` is documented as a fixed startup-level workaround. Live control still requires the system mixer, upstream VLC integration, or an equivalent local patch. |

## portability/standards conformance

| id | status | effort | description | notes |
|---|---|---|---|---|

No other open finding. GNU/Linux-specific CPU affinity, dynamic loading,
and VLC plugin interfaces are isolated, while the supported compiler/CPU
fallbacks have explicit build and contract coverage.

## error handling

| id | status | effort | description | notes |
|---|---|---|---|---|

No open finding. Relevant production allocation, backend, picture, clock,
synchronization, and processing failures are propagated or deliberately
treated as invariant-only cases. The documented recovery contract preserves
worker storage until synchronous retirement completes.

## resource management

| id | status | effort | description | notes |
|---|---|---|---|---|

No open finding. Permanent USM/backend failures retire their pools and
resources. Normal safe retirement waits for active callbacks before releasing
their storage; a failed thread join quarantines the complete allocation island
instead of releasing worker-accessible state.

## API/ABI stability

| id | status | effort | description | notes |
|---|---|---|---|---|

No open finding. Shared variant declarations, the zimg ABI-major guard, and the
visibility/final-link ISA gates remain coherent by inspection and by successful
local GCC and Clang single- and multi-version plugin links.

## build/toolchain hygiene

| id | status | effort | description | notes |
|---|---|---|---|---|
| BUILD-34 | open | S | Sonar still excludes `src/autoupscale.c` from coverage and says its VLC lifecycle needs a full runtime mock, although `tests/test_autoupscale_lifecycle.c` now drives the real callbacks and the gated report measured 96.1% line coverage (`sonar-project.properties:25-36`; `Makefile:1033-1040,1119-1122`; `scripts/coverage_scope.txt:1-8`). | Remove `src/autoupscale.c` from `sonar.coverage.exclusions` and refresh the stale comments so Sonar exposes lifecycle coverage regressions instead of discarding the generated data. Keep the justified six-line `src/scaler.c` exclusion separate. |
| BUILD-35 | open | S | The zimg libFuzzer object and seam target bypass `EXTRA_CFLAGS` (`Makefile:830-837`), so `make fuzz EXTRA_CFLAGS=-Werror` emits VLC-header warnings and still succeeds while every other fuzzer honors the requested warning policy. | Thread `EXTRA_CFLAGS` through both rules (or derive them from a shared fuzz flag set) and apply the existing targeted VLC `_Generic` warning suppression. Keep the CI `-Werror` invocation as a regression gate for every built fuzzer. |

## observability

| id | status | effort | description | notes |
|---|---|---|---|---|
| OBS-12 | open | XS | README tells users that `make info` reports CPU configuration and zimg detection, but the target prints only plugin/library flags and compiler names (`README.md:40-41`; `Makefile:1286-1293`). | Print `MARCH`, `MULTIVERSION`, and resolved zimg state so the advertised pre-build diagnostic identifies artifact compatibility and backend selection. |

No additional production finding. Actionable degradation reasons and pinning
outcomes are visible without verbose logging.

## wiring gaps

| id | status | effort | description | notes |
|---|---|---|---|---|

No additional production wiring finding. All module options have consumers;
both scaler backends, runtime fallback, USM pool, and multiversion dispatcher
have real production call sites.

## unused functions/methods

| id | status | effort | description | notes |
|---|---|---|---|---|

No other unused static function, macro, field, or orphan call subtree was found.

## Audit picks deliberately rejected

Recorded so future full-project rescans do not repeatedly promote the same
non-findings:

- **SCAL-1a--SCAL-1d, cgroup CPU-aware worker sizing:** rejected. The existing
  affinity-aware count, 12-worker automatic cap, and explicit user override
  handle the supported desktop workload; robust v1/v2/hybrid cgroup discovery
  would add proc/sysfs parsing and failure policy with no measured need.
- **SCAL-2a--SCAL-2d, cgroup memory-aware AUTO target selection:** rejected.
  The current host-RAM heuristic remains intentionally simple, while the
  necessary cgroup discovery/parsing stack is rejected above and no memory
  pressure regression was reproduced within the tested bounded worker setup.
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
