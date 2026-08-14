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
(229 functions, zero CCN above 10, average 3.8) and a local ShellCheck pass
(clean). Findings dated 2026-08-14 below are its output: CONC-3, DUP-9,
DUP-10, ARCH-3, ARCH-4, REL-18, REL-19, REL-20, PORT-2, BUILD-10, OBS-3.
All other categories re-verified clean.

## security

| id | status | effort | description | notes |
|---|---|---|---|---|

No open finding.

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
971 functions, zero CCN violations, and a maximum CCN of 10.

## code duplication

| id | status | effort | description | notes |
|---|---|---|---|---|
| DUP-9 | open | S | `scaler_zimg.c` builds the 9-field `up_zimg_io_req_t` twice (`zimg_open` ~963-973 and `zimg_prepare_first_frame_io` ~1149-1161); a new field must be added at both sites or the open-time and first-frame plans silently diverge. Extract one `zimg_build_io_req()` helper taking the two alignment-permitted flags. | 2026-08-14 audit. The resolver's fixed-point property depends on both requests being built identically. |
| DUP-10 | open | S | `up_pool_gate_request_exit` (threading.h ~499-503) re-implements the broadcast+unlock+sync_failed tail of `up_pool_gate_unlock_broadcast` (~397-404) verbatim; a fix to the failure path must land twice in a concurrency-critical file. Replace the tail with `return up_pool_gate_unlock_broadcast(g);` after setting `exit_requested`. | 2026-08-14 audit. Same both-rc/latch-on-failure semantics; behavior-preserving. |

No additional duplication finding.

## architecture/modularity/SOLID

| id | status | effort | description | notes |
|---|---|---|---|---|
| ARCH-3 | open | M | `threading.h` (553 lines) carries two independent concerns: CPU-topology/thread-count policy (lines ~39-225) and the pthread pool gate (lines ~227-553). `autoupscale.c` and `autoupscale_module.c` consume only the policy half yet pull in the whole synchronization layer; the gate half is consumed only via `worker_pool.h`. Split at the existing section boundary: `up_detect_cpu_topology*`, `up_threads_decide`, `up_detect_cores`, `UP_THREADS_*`/`UP_CPU_*` constants → `src/thread_policy.h`; `up_pool_gate_*`, `up__deadline_after_ms`, `UP_POOL_BARRIER_TIMEOUT_MS` → `src/pool_gate.h` (included by `worker_pool.h`). | 2026-08-14 audit. Mechanical move + include updates in src and tests; no behavior change. |
| ARCH-4 | open | M | `scaler_zimg.c` (1279 lines) embeds a self-contained, zimg-API-free plane-buffer subsystem: `plane_view_t`/`plane_layout_t`/`plane_buffer_t` (~136-152) plus `plane_alloc_bytes`, `plane_buffer_view`, `free_plane_buffer`, `init_plane_layout`, `plane_buffer_bytes`, `alloc_plane_buffer` (~612-675). Move them to a new `src/plane_buffer.h` (depends only on `zimg_helpers.h` sizing helpers), matching the project's pure-logic-in-headers testability layout and cutting ~120 lines from the backend TU. The rest of the file is coherent backend logic; no further split recommended. | 2026-08-14 audit. Enables direct unit/fuzz coverage of the overflow-checked alloc seam without libzimg. |

Backend strategy, shared worker lifecycle, chroma descriptor,
and SIMD dispatch boundaries otherwise remain coherent.

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
| REL-18 | open | S | README.md:150 claims "The wrapper validates its required VLC modules" for the `--transcode-display` profile, but the launch path performs no module validation — the check exists only behind the separate `--check-transcode-display` flag, run once by the installer. | 2026-08-14 audit. `--transcode-display)` in scripts/vlc-autoupscale.sh:56-62 execs VLC directly without calling `check_transcode_display`; if x264/avcodec are removed after install, the action fails with only VLC's own errors. USAGE.md:28-29 and DESKTOP_INTEGRATION.md:62-68 describe the check as a separate command, so README's sentence is the outlier. Fix the README sentence or run the check (or a cheap subset) on launch. |
| REL-19 | open | S | The direct-mode wrapper path (used by the `VLC (AutoUpscale)` desktop/Open With entry) omits `--no-one-instance`, so with VLC's one-instance preference enabled the file is enqueued into an already-running plain VLC and every `--video-filter=autoupscale`/`--autoupscale-*` flag is silently dropped. | 2026-08-14 audit. scripts/vlc-autoupscale.sh:75 versus the transcode path's deliberate guard at :58-59 (`--no-one-instance --no-one-instance-when-started-from-file`, rationale in DESKTOP_INTEGRATION.md:52). Only affects users who enabled one-instance mode (off by default on Linux). Add the same guard to the direct exec. |
| REL-20 | open | S | src/scaler.h:46-49 documents `pin_cpus` as opt-in with "0 = let the scheduler place threads (default)", but the shipped default is 1 (pinning on) — the backend contract header contradicts the actual option default. | 2026-08-14 audit. `add_integer_with_range( UP_CFG_PREFIX "pin-threads", 1, 0, 1, ...)` in src/autoupscale_module.c:225, its longtext ("1 = on (default)"), and the README options table all say on-by-default. Fix the stale header comment. |

## portability/standards conformance

| id | status | effort | description | notes |
|---|---|---|---|---|
| PORT-2 | open | M | `make test`/`check`/`fuzz-smoke` cannot run on non-x86 hosts: `CC_LEVEL_GOALS` attaches `check-cc-x86-level-flags` to `BUILD_CONFIG` without an `IS_X86` gate, and `test`/`fuzz-smoke` unconditionally build and run the x86-only `test_usm_pool_variants`/`fuzz_usm_variants_smoke` (`-march=x86-64*` compiles), so the recipe's own "skipped: non-x86 host" branch is unreachable. | 2026-08-14 build/scripts audit. Makefile:54-57, 355-357, 541/592, 719/758 versus the dead non-x86 branch at Makefile:594-599; dispatch tests are correctly `$(if $(IS_X86),...)`-gated, the variants pair is not. Gate the level check and the variant binaries on `IS_X86` the same way. |

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
| BUILD-10 | open | S | `bench-usm-halo`, `bench-worker-pool`, and `bench-pipeline` are the only recipe-only targets not declared `.PHONY`; a same-named file in the repo root would silently satisfy them and skip the benchmark run. | 2026-08-14 build/scripts audit. Makefile:1093/1096/1100; every sibling target is covered by the `.PHONY` declarations at Makefile:284/342/372/411/426/857/1233/1280. Add the three names to the line-411 list. |

No other open finding.

## observability

| id | status | effort | description | notes |
|---|---|---|---|---|
| OBS-3 | open | S | The open-time "zimg backend open failed; using swscale fallback" notice in src/autoupscale.c:250-255 uses `msg_Warn`, which VLC 3.x suppresses at default verbosity, so the *reason* for the silent quality downgrade is invisible — contrary to the file's own convention of using `msg_Info` for actionable degradations. | 2026-08-14 audit. Comments at autoupscale.c:596, 680, 713 and scaler_zimg.c:1193 state "msg_Warn is suppressed by default" and use msg_Info; the engagement banner does show `backend=swscale`, so the outcome (not the cause) is visible — hence S. The zimg calloc-failure open path emits nothing visible at all; only the ABI-mismatch path has its own msg_Err. Switch to one-shot `msg_Info`. |

No additional production finding. All other actionable degradation reasons and
pinning outcomes are visible at default verbosity.

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
