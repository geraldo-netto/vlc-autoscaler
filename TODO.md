# TODO — full-project audit findings

Latest rescan: 2026-09-07, HEAD `5d17c0c`, all 269 tracked files across all
review categories, excluding build outputs and cache files/directories.
Added 13 findings; all eight contradiction findings are `blocked` with source
evidence and an unblocking condition. Fresh verification passed GCC/Clang
builds, ASan/UBSan suites, USM/zimg TSan stress, 148 corpus-seed replays and
short mutation-guided passes for all 15 fuzz targets, 18/18 mutation checks,
Cppcheck, Clang static analysis, ShellCheck, actionlint, rumdl, and offline
Markdown-link/fragment checks. Gated line coverage: 98.8%, all 185 tracked
functions at least 80%; informational zimg coverage: 94.4%. Lizard checked
994 C functions with no CCN above 10. Earlier audit provenance follows.

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

Post-audit implementation through `11da8c6` added three tracked files. Its
integrated verification reported 98.9% gated production line coverage
(1417/1433) with all 179 tracked functions above 80%, and Lizard covered 970
functions with zero CCN above 10 and a maximum of 10. The original rescan scope
and validation above remain the provenance for the open findings.

A second full rescan on 2026-08-14 at HEAD `f7f806b` covered all production
source/headers, Makefile, scripts, workflows, docs, and patches (excluding
tests, build outputs, and caches), with explicit passes for architecture and
code-to-merge/code-to-split seams. Method: five parallel adversarial
code-trace reviews across all categories, plus a fresh Lizard run over `src/`
and a local ShellCheck pass (clean). It produced eleven findings (CONC-3,
DUP-9, DUP-10, ARCH-3, ARCH-4, REL-18, REL-19, REL-20, PORT-2, BUILD-10,
OBS-3), all since implemented, tested, and committed — see `git log` — and
re-verified every other category clean. Post-fix verification: full unit,
shell, and 14 fuzz-smoke suites green; zimg integration + seam smoke green;
gated coverage 185 tracked functions with none below 80% (plane_buffer.h at
100%); Lizard clean over 993 functions (max CCN 10).

## security

| id | status | effort | description | notes |
|---|---|---|---|---|

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

## scalability

| id | status | effort | description | notes |
|---|---|---|---|---|
| SCAL-8 | blocked | S | Reconcile explicit worker preferences with the USM pool's hidden 12-worker cap. | `src/autoupscale_module.c:74-84` describes explicit 1..64 preferences as CPU/geometry-capped, but `src/usm_pool.c:409-425` also caps them at 12 whenever stripe rows are automatic. A sanitizer-built probe of `up_usm_pool_create(32,1920,1080,0)` reports 12; changing only the stripe value to its documented default, 8, reports 32. `tests/test_usm_pool.c:334-374` intentionally enforces this difference. Unblock by deciding whether explicit worker preference should bypass the cap or documenting the additional cap and stripe-option interaction in both option help and README. |

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
994 functions, zero CCN violations, and a maximum CCN of 10.

## code duplication

| id | status | effort | description | notes |
|---|---|---|---|---|

No additional duplication finding.

## architecture/modularity/SOLID

| id | status | effort | description | notes |
|---|---|---|---|---|
| ARCH-13 | blocked | S | Correct the documented owner of backend fallback. | `docs/ARCHITECTURE.md:47` says `scaler.c` owns selection and fallback; `src/scaler.c` only selects a supported backend, while `OpenScalerOrFallback` and `TryBackendFallback` in `src/autoupscale.c:235,652` own open/runtime recovery. Unblock by aligning the backend-contract description with those production call sites. |

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
| REL-21 | blocked | M | Respect VLC's output-format-change permission before accepting an upscale. | `src/autoupscale.c:382-460` changes output geometry without reading `filter_t.b_allow_fmt_out_change`. A sanitizer-built lifecycle probe returned success with `allow_change=0`, changing 320x180 to 1280x720; the current lifecycle tests also initialize the flag false. [VLC 3.0.20's deinterlacer](https://github.com/videolan/vlc/blob/3.0.20/modules/video_filter/deinterlace/deinterlace.c#L590) rejects geometry changes in this case. A fixed-format caller can therefore receive unexpected geometry or undersized output buffers that our views reject. Unblock by defining the refusal/allowed-change behavior and testing both flag values against the requested output format. |

## portability/standards conformance

| id | status | effort | description | notes |
|---|---|---|---|---|

No other open finding. GNU/Linux-specific CPU affinity, dynamic loading,
and VLC plugin interfaces are isolated, while the supported compiler/CPU
fallbacks have explicit build and contract coverage.

## error handling

| id | status | effort | description | notes |
|---|---|---|---|---|

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
| BUILD-38 | blocked | S | Run `fuzz_plane_buffer` in the CI libFuzzer job. | `Makefile:666` includes `plane_buffer` in `FUZZ_TARGET_NAMES`, but `.github/workflows/ci.yml:183-247` never executes it, contradicting the workflow's claim that every built fuzzer receives a pass. Deterministic smoke coverage exists, but mutation-guided CI coverage is missing. Unblock by adding it to the pure-logic run list and verifying the built/run target sets match. |
| BUILD-39 | blocked | S | Align the per-function coverage aggregation description with its actual gate. | `scripts/coverage_per_function.sh:7-10` promises the best coverage from one binary, but lines 127-150 union covered lines across binaries before computing each function's percentage. Complementary partial tests can therefore pass although neither binary meets the documented threshold alone. Unblock by selecting the intended aggregation contract and updating the description or implementation, with a complementary-coverage fixture. |

## observability

| id | status | effort | description | notes |
|---|---|---|---|---|
| OBS-13 | blocked | S | Qualify source-zero-copy output equivalence in VLC's option help and test descriptions. | `src/autoupscale_module.c:108-121` claims byte identity with copy-in, while `README.md`'s zero-copy option and `src/scaler_zimg.c:30-34` restrict that guarantee to unchanged grids. `tests/test_scaler_zimg.c:511-540` retains an unconditional identity title but explicitly exempts column-tiled cases from copy/direct equality. Unblock by stating the same-grid condition and the grid-change seam caveat consistently. |
| OBS-14 | blocked | S | Correct stale worker-policy comments that contradict the shipped defaults. | `src/thread_policy.h:15-17` describes AUTO as capped at `UP_THREADS_MAX` (64), but line 205 caps it at `UP_THREADS_AUTO_MAX` (12). `src/scaler_zimg.c:107-108,690` describes pinning as opt-in, while `src/autoupscale_module.c:88,225` enables it by default. Unblock by synchronizing both comments with the existing runtime constants and option defaults. |
| OBS-15 | blocked | S | Include allocated column-tile destination buffers in the zimg scratch diagnostic. | `src/scaler_zimg.c:802-826` reports zero destination scratch whenever column tiling is active, omitting the buffers allocated at lines 624-630. An ASan/UBSan backend probe for I420 4096x32 to 16384x128, 32 workers, source/destination zero-copy enabled produced an 8x4 grid with 5,242,880 allocated tile bytes but logged `scratch 0 MB (src zero-copy, dst tiled+copy), graph-tmp 2 MB`. Unblock by including per-worker `tile_dst` bytes and testing the reported total against the allocated layouts. |

## wiring gaps

| id | status | effort | description | notes |
|---|---|---|---|---|

No additional production wiring finding. All module options have consumers;
both scaler backends, runtime fallback, USM pool, and multiversion dispatcher
have real production call sites.

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
