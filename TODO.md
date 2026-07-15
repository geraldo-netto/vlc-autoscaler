# TODO — full-project audit findings

Full-project rescan of HEAD `1be0717` and its clean working tree on 2026-07-15.
Scope: all 248 tracked project files: production source/public headers, tests,
fuzzers, 148 corpus seeds, build/release files, workflows, scripts, documents,
and patches. Excluded only `.git`, ignored/generated build output, and caches.

Validation: parallel manual inspection across every category; GCC and Clang C11
unit/contract suites with `-Werror`, ASan, and UBSan; every deterministic
fuzz-smoke target; Clang/libFuzzer and benchmark builds; the 99.5% line / 157
function coverage gates; all 27 ASan/UBSan USM stress configurations; GCC and
Clang VLC ABI-layout checks; Clang Static Analyzer over the test-exposed
production paths; declared-interpreter shell syntax; relative Markdown links
and code fences; corpus sizes; unsafe-API, ownership, return-value, symbol, and
configuration-consumer searches; and review of every change since the preceding
scan. The analyzer's two real findings are recorded as UB-12. Local TSan aborts
before the test on an unsupported memory mapping. libswscale/zimg metadata is
absent, so the plugin and zimg integration/analyzer targets could not run.
cppcheck, clang-tidy, ShellCheck, actionlint, markdownlint, and Lizard were
unavailable; the preceding Lizard 1.17.31 result (924 functions, zero CCN > 10,
maximum 10) remains applicable because subsequent control-flow changes only
simplified an assertion. Row format:
`id | status | effort | description | notes`.

## security

| id | status | effort | description | notes |
|---|---|---|---|---|
| SEC-2 | open | M | CI installs an unpinned PyPI `lizard` package and downloads a mutable Sonar build-wrapper ZIP without integrity verification (`.github/workflows/ci.yml:37-41,243-252`). | Pin the Python package version and hash; pin the wrapper artifact and verify its published checksum/signature before extraction and execution. |
| SEC-5 | open | S | `tests/test_install_action.sh:5-37` creates a predictable `${TMPDIR:-/tmp}/vlc-autoscaler-install-test.$$` tree, recursively removes it, writes executable `PATH` stubs there, and invokes the installer. | In a shared temporary directory an attacker can pre-create or race components to redirect writes or swap the executed stub. Use `mktemp -d` (mode 0700), arm cleanup only after creation, and make signal traps clean up and exit. |

No additional security issue was found in the production media/configuration
paths: geometry is validated before pointer formation, user integers are
normalized or clamped, format strings are literals, and allocation dimensions
are bounded.

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
| SCAL-7 | open | S | The three-job CI workflow has no superseded-run cancellation (`.github/workflows/ci.yml:3-13`). | Rapid pushes retain stale build, analyzer, and sequential timed-fuzzer runs, consuming runner quota and delaying current feedback. Add ref/PR-scoped `concurrency` with `cancel-in-progress` for non-release runs. |

No additional unparked scalability finding. Deferred items remain in
**Open — parked**.

## concurrency

| id | status | effort | description | notes |
|---|---|---|---|---|

No open finding. Gate publication, the completion release sequence,
cancellation cleanup while blocked, partial spawn, and normal teardown showed
no data race or valid-state deadlock. The completion deadline is explicitly
not an end-to-end callback bound; recovery joins active callbacks before
releasing their storage.

## code complexity

| id | status | effort | description | notes |
|---|---|---|---|---|

No open finding. The most recent full `src/` + `tests/` Lizard analysis reports
924 functions, zero CCN violations, and a maximum CCN of 10.

## code duplication

| id | status | effort | description | notes |
|---|---|---|---|---|

No additional unparked duplication finding. Deferred items remain in
**Open — parked**.

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

No open finding.

## portability/standards conformance

| id | status | effort | description | notes |
|---|---|---|---|---|
| PORT-12 | open | S | The compatibility contract says Linux x86-64 with GCC or Clang, but all x86 test builds and `MULTIVERSION=1` require compiler support for `-march=x86-64-v3/v4` (`README.md:84-101,115-118`; `Makefile:216-229,1273-1280`). | Those options begin with GCC 11 and Clang 12; older compilers fail before the runtime CPUID fallback in `src/cpu_level.h` matters. Document and enforce minimum versions or feature-probe the flags and provide an explicit-feature fallback. |

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
visibility/final-link ISA gates remain coherent by inspection. Their actual
plugin link could not be repeated locally because the required SDKs are absent.

## build/toolchain hygiene

| id | status | effort | description | notes |
|---|---|---|---|---|
| BUILD-1 | open | S | CI's Clang step builds only `MULTIVERSION=0` and omits Clang visibility verification (`.github/workflows/ci.yml:72-81`), while the compatibility contract supports Clang (`README.md:115-118`). | Build and link both Clang configurations, then run visibility and linked-ISA gates under Clang too. |
| BUILD-6 | open | S | Empty `VLC_PLUGIN_BASE` becomes non-empty `/video_filter` (`Makefile:55-56`), while install/uninstall validate only the derived value and use unquoted paths (`Makefile:1209-1220`). | Validate a non-empty base in both targets before deriving the subdirectory, and quote every destination. |
| BUILD-7 | open | S | The Sonar job runs for every pull request but requires `SONAR_TOKEN` (`.github/workflows/ci.yml:3-8,221-290`); fork pull requests do not receive repository secrets. | Gate Sonar to pushes/internal pull requests while retaining token-free build checks for forks. |
| BUILD-11 | open | S | `PLUGIN_GOALS` omits the standalone `abi-layout-check` and `check-visibility` goals (`Makefile:58-70` versus `:348-405`). | Direct invocation without SDKs bypasses the friendly prerequisite check and fails deep in compilation. Add both goals to `PLUGIN_GOALS`. |
| BUILD-27 | open | S | The desktop-action installer inserts the `whereis` result into a shell double-quoted assignment after escaping only sed metacharacters (`scripts/install-vlc-autoupscale-action.sh:26-37,75-80`). | `$`, backticks, quotes, or newlines in a valid VLC path are expanded or can make the installed wrapper invalid; the current test varies only `HOME`. Emit a POSIX-safe shell literal (and reject line breaks), then execute the installed wrapper in a metacharacter-path regression test. |
| BUILD-31 | open | S | The repository has four user/developer Markdown guides, but CI validates only C, shell, and workflow files; no Markdown style or relative-link gate exists (`Makefile:1156-1162`; `.github/workflows/ci.yml:120-131`). | Add a pinned Markdown linter and local-link checker to `semantic-analysis`/CI, with configuration matching the existing tables and long command examples. |

## observability

| id | status | effort | description | notes |
|---|---|---|---|---|

No additional production finding. Actionable degradation reasons and pinning outcomes are
visible without verbose logging, and advisory-disable wording preserves the
distinction from active EWMA/stat telemetry.

## wiring gaps

| id | status | effort | description | notes |
|---|---|---|---|---|

No additional production wiring finding. All 14 module options have consumers; both scaler
backends, runtime fallback, stats lifecycle, USM pool, and multiversion
dispatcher have real production call sites.

## unused functions/methods

| id | status | effort | description | notes |
|---|---|---|---|---|

No other unused static function, macro, field, or orphan call subtree was found.

## Open — parked

Each implementation row below remains open until its stated unit/regression,
integration, fuzz, and sanitizer/stress coverage is implemented and passing;
measurement-only rows require a reproducible benchmark procedure and recorded
result. A prerequisite finding that measurements disprove is removed together
with its blocked implementation rows and recorded under deliberately rejected
picks so a later audit does not revive it without new evidence.

| id | status | effort | description | why not now |
|---|---|---|---|---|
| SCAL-1a | parked | S | Add bounded pure parsers for cgroup v2 `cpu.max` and v1 CFS quota/period, including unlimited and malformed inputs. | Still applicable at HEAD. Keep filesystem discovery out of the parser; table-driven unit/regression tests and a byte-input fuzzer must cover zero, negative, overflow, whitespace, truncation, and unlimited forms. |
| SCAL-1b | parked | M | Discover the process's effective cgroup CPU controller paths from `/proc/self/cgroup` and mount roots from `/proc/self/mountinfo`. | Parsing only fixed `/sys/fs/cgroup` paths cuts too many correctness corners for nested/delegated containers. Use size-capped reads; add fixture-based v1/v2/hybrid integration tests and fuzz both line parsers. |
| SCAL-1c | parked | S | Combine affinity capacity with a finite cgroup CPU quota, rounding fractional quotas conservatively and clamping through the existing CPU bounds. | Blocked on SCAL-1a/SCAL-1b. Keep explicit user thread counts subject to the same hard availability ceiling; unit-test precedence and add a fake-filesystem integration regression for affinity greater/less than quota. |
| SCAL-1d | parked | S | Fall back unchanged when cgroup files are absent, unlimited, unreadable, racing, or malformed. | Implement with a thin injectable reader rather than a filesystem abstraction. Add error-path regression tests and sanitizer fuzz-smoke; capacity discovery failure must never prevent plugin Open. |
| SCAL-2a | parked | S | Add bounded pure parsers for cgroup v2 `memory.max` and v1 `memory.limit_in_bytes`, including unlimited sentinels and overflow. | Still applicable at HEAD. Share only generic bounded-file helpers with SCAL-1; table-driven unit/regression tests and fuzzing must cover malformed, huge, zero, whitespace, and truncated values. |
| SCAL-2b | parked | S | Reuse SCAL-1b controller discovery to locate the process's effective memory-limit file in v1, v2, and hybrid layouts. | Do not create a second mountinfo/cgroup parser. Extend its fixtures with split CPU/memory controller mounts and nested paths. |
| SCAL-2c | parked | S | Compute AUTO memory as the minimum finite value of host RAM and cgroup memory limit before the existing 720p/1080p decision (`src/autoupscale.c:394-410`). | Blocked on SCAL-2a/SCAL-2b. Keep the existing decision function unchanged; unit-test min/unlimited precedence and add an Open/config integration regression with injected host and cgroup values. |
| SCAL-2d | parked | S | Preserve host-RAM behavior when cgroup memory discovery fails or reports unlimited. | Use the same injectable bounded reader as SCAL-1d. Add unreadable/racing/malformed fixture regressions and sanitizer fuzz-smoke. |
| SCAL-3b | parked | M | If SCAL-3a justifies it, discover Linux CPU sibling and NUMA-node topology only for CPUs already allowed by `sched_getaffinity`. | Prefer bounded sysfs reads and deterministic fallback to existing ID order; unit-test sparse/offline/missing topology fixtures and fuzz numeric/list parsers. |
| SCAL-3c | parked | S | Build a pure stable CPU-ordering helper for the selected physical-core/SMT/NUMA policy. | Blocked on SCAL-3a. Fuzz ordering invariants: no duplicates, no disallowed IDs, stable output, complete allowed-prefix coverage, and bounds at `UP_THREADS_MAX`. |
| SCAL-3d | parked | S | Wire the selected order into opt-in zimg pinning while retaining round-robin fallback and current success accounting. | Blocked on SCAL-3b/SCAL-3c. Extend wrapped-affinity integration tests for exact worker-to-CPU mapping and failures; run zimg seam regression/fuzz plus ASan/UBSan/TSan stress. |
| SCAL-P1a | parked | S | Instrument benchmarks to report per-worker completion skew for USM and zimg static cells under controlled heterogeneous load (`src/worker_pool.h:364-384`). | Still applicable at HEAD, but the barrier alone does not prove actionable skew. Benchmark instrumentation stays test-only. |
| SCAL-P1b | parked | M | If SCAL-P1a confirms stable skew, prototype weighted static stripe/cell sizing before considering dynamic work stealing. | This is the main corner cut: preserve one dispatch and one completion barrier. Add pure partition unit tests and fuzz invariants for non-empty, contiguous, chroma-aligned, full-frame coverage. |
| SCAL-P1c | parked | M | Integrate a winning weighted-static policy without changing worker ownership or graph lifetime. | Blocked on SCAL-P1b outperforming equal partitions. Require byte/seam regression tests against single-worker output, zimg integration fuzz, USM variant fuzz, and sanitizer/TSan stress. |
| SCAL-P1d | parked | L | Consider bounded dynamic work queues only if weighted static partitioning fails under measured production-like contention. | Requires a design review first. Tests must cover exactly-once cell ownership, cancellation/failure draining, deterministic output, fault injection, integration fuzz, and ASan/UBSan/TSan stress. |
| SCAL-P3c | parked | M | Define a backend-neutral job descriptor and prove both existing worker callbacks can run through it without moving resource ownership. | Blocked on SCAL-P3a showing that SCAL-P3b is insufficient. Add compile-time/API tests and worker-pool unit/fault-injection coverage; no production pool sharing yet. |
| SCAL-P3d | parked | L | Move zimg and USM onto one persistent worker lifecycle while keeping graphs/scratch owned by their backends. | Blocked on SCAL-P3c. Require transition regressions for lazy init, partial spawn, poison, join quarantine, close, backend fallback, and alternating jobs; add integration fuzz and ASan/UBSan/TSan stress. |
| SCAL-P4c | parked | M | If wake overhead remains material, prototype a bounded fan-out/tree wake while retaining the existing completion barrier. | Avoid Linux-only futexes unless portability scope changes. Unit-test every partial-init/wake/stop failure with wrappers; stress repeated generations and cancellation under TSan. |
| SCAL-P4d | parked | M | Integrate a winning wake strategy behind the shared worker-pool gate without changing owner callbacks. | Blocked on SCAL-P4c outperforming broadcast. Require worker-pool regression/fault tests, generation-wrap and lost-wake stress, USM/zimg integration fuzz, ASan/UBSan/TSan, and clean fallback to broadcast. |

## Audit picks deliberately rejected

Recorded so future full-project rescans do not repeatedly promote the same
non-findings:

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
- Extending the performance timer over `RunProbe()`: the first 60-frame probe is
  temporary diagnostic work, while the advisory deliberately measures the
  steady scaler + USM settings it recommends changing. Including the probe in
  the EWMA could produce a permanent warning for an overhead that has ended.
- Treating zimg temporary-buffer alignment as an overflow seam: zimg 3.0.5 uses
  checked internal size arithmetic, graph dimensions are capped at 32768, and
  returned x86 temporary sizes are already 64-byte aligned; the value cannot
  approach `SIZE_MAX - 63` on a reachable production graph.
