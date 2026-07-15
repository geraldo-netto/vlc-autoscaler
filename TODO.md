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
| PERF-P1c | parked | M | If PERF-P1a confirms a bottleneck, prototype worker-owned halo copies with a snapshot-ready phase before any worker writes in place. | Do not fold this into the existing go gate: readiness must prevent neighbour overwrite. Require unit fault-injection for every new wait failure, in-place byte-equivalence regression tests, ASan/UBSan/TSan stress, and `fuzz_usm_variants` coverage. |
| PERF-P2a | parked | S | Benchmark SSE2, AVX2, and AVX-512 USM variants independently on representative widths, worker counts, and amounts (`src/usm_pool_dispatch.c:87-121`). | Still applicable at HEAD, but no host regression is known. Reuse variant entry points and record CPU model/governor; do not alter selection without evidence. |
| PERF-P2b | parked | S | Define a deterministic max-ISA selection policy only if PERF-P2a finds a repeatable wider-ISA regression. | The cheapest safe policy is a load-time maximum variant override, leaving current widest-supported selection as default; avoid an adaptive runtime tuner. |
| PERF-P2c | parked | S | Implement and document the confirmed max-ISA override in the dispatcher, rejecting unknown/unsupported values with a safe fallback. | Blocked on PERF-P2a/PERF-P2b. Extend dispatcher table tests for every override/capability combination, add a subprocess integration test for load-time selection, preserve cross-variant regression tests, and seed the selector parser fuzzer. |
| SCAL-1a | parked | S | Add bounded pure parsers for cgroup v2 `cpu.max` and v1 CFS quota/period, including unlimited and malformed inputs. | Still applicable at HEAD. Keep filesystem discovery out of the parser; table-driven unit/regression tests and a byte-input fuzzer must cover zero, negative, overflow, whitespace, truncation, and unlimited forms. |
| SCAL-1b | parked | M | Discover the process's effective cgroup CPU controller paths from `/proc/self/cgroup` and mount roots from `/proc/self/mountinfo`. | Parsing only fixed `/sys/fs/cgroup` paths cuts too many correctness corners for nested/delegated containers. Use size-capped reads; add fixture-based v1/v2/hybrid integration tests and fuzz both line parsers. |
| SCAL-1c | parked | S | Combine affinity capacity with a finite cgroup CPU quota, rounding fractional quotas conservatively and clamping through the existing CPU bounds. | Blocked on SCAL-1a/SCAL-1b. Keep explicit user thread counts subject to the same hard availability ceiling; unit-test precedence and add a fake-filesystem integration regression for affinity greater/less than quota. |
| SCAL-1d | parked | S | Fall back unchanged when cgroup files are absent, unlimited, unreadable, racing, or malformed. | Implement with a thin injectable reader rather than a filesystem abstraction. Add error-path regression tests and sanitizer fuzz-smoke; capacity discovery failure must never prevent plugin Open. |
| SCAL-2a | parked | S | Add bounded pure parsers for cgroup v2 `memory.max` and v1 `memory.limit_in_bytes`, including unlimited sentinels and overflow. | Still applicable at HEAD. Share only generic bounded-file helpers with SCAL-1; table-driven unit/regression tests and fuzzing must cover malformed, huge, zero, whitespace, and truncated values. |
| SCAL-2b | parked | S | Reuse SCAL-1b controller discovery to locate the process's effective memory-limit file in v1, v2, and hybrid layouts. | Do not create a second mountinfo/cgroup parser. Extend its fixtures with split CPU/memory controller mounts and nested paths. |
| SCAL-2c | parked | S | Compute AUTO memory as the minimum finite value of host RAM and cgroup memory limit before the existing 720p/1080p decision (`src/autoupscale.c:394-410`). | Blocked on SCAL-2a/SCAL-2b. Keep the existing decision function unchanged; unit-test min/unlimited precedence and add an Open/config integration regression with injected host and cgroup values. |
| SCAL-2d | parked | S | Preserve host-RAM behavior when cgroup memory discovery fails or reports unlimited. | Use the same injectable bounded reader as SCAL-1d. Add unreadable/racing/malformed fixture regressions and sanitizer fuzz-smoke. |
| SCAL-3a | parked | S | Benchmark current first-allowed-CPU round-robin pinning against physical-core-first and NUMA-spread orderings on SMT and multi-node hosts (`src/threading.h:99-116`, `src/scaler_zimg.c:784-796`). | Still applicable at HEAD, but no losing topology is established. Do not add platform discovery until a policy wins repeatably. |
| SCAL-3b | parked | M | If SCAL-3a justifies it, discover Linux CPU sibling and NUMA-node topology only for CPUs already allowed by `sched_getaffinity`. | Prefer bounded sysfs reads and deterministic fallback to existing ID order; unit-test sparse/offline/missing topology fixtures and fuzz numeric/list parsers. |
| SCAL-3c | parked | S | Build a pure stable CPU-ordering helper for the selected physical-core/SMT/NUMA policy. | Blocked on SCAL-3a. Fuzz ordering invariants: no duplicates, no disallowed IDs, stable output, complete allowed-prefix coverage, and bounds at `UP_THREADS_MAX`. |
| SCAL-3d | parked | S | Wire the selected order into opt-in zimg pinning while retaining round-robin fallback and current success accounting. | Blocked on SCAL-3b/SCAL-3c. Extend wrapped-affinity integration tests for exact worker-to-CPU mapping and failures; run zimg seam regression/fuzz plus ASan/UBSan/TSan stress. |
| SCAL-P1a | parked | S | Instrument benchmarks to report per-worker completion skew for USM and zimg static cells under controlled heterogeneous load (`src/worker_pool.h:364-384`). | Still applicable at HEAD, but the barrier alone does not prove actionable skew. Benchmark instrumentation stays test-only. |
| SCAL-P1b | parked | M | If SCAL-P1a confirms stable skew, prototype weighted static stripe/cell sizing before considering dynamic work stealing. | This is the main corner cut: preserve one dispatch and one completion barrier. Add pure partition unit tests and fuzz invariants for non-empty, contiguous, chroma-aligned, full-frame coverage. |
| SCAL-P1c | parked | M | Integrate a winning weighted-static policy without changing worker ownership or graph lifetime. | Blocked on SCAL-P1b outperforming equal partitions. Require byte/seam regression tests against single-worker output, zimg integration fuzz, USM variant fuzz, and sanitizer/TSan stress. |
| SCAL-P1d | parked | L | Consider bounded dynamic work queues only if weighted static partitioning fails under measured production-like contention. | Requires a design review first. Tests must cover exactly-once cell ownership, cancellation/failure draining, deterministic output, fault injection, integration fuzz, and ASan/UBSan/TSan stress. |
| SCAL-P2a | parked | S | Measure zimg graph/tmp/tile memory and lazy-build latency by geometry, chroma, zero-copy mode, and worker count (`src/scaler_zimg.c:183-214,713-739,797-805,823-859`). | Still applicable at HEAD; linear ownership is intentional and bounded. Add benchmark reporting before changing resource topology. |
| SCAL-P2b | parked | S | Define an evidence-based per-instance zimg memory/build budget and derive a lower worker cap when SCAL-P2a crosses it. | Cheapest safe mitigation: cap independent workers rather than share graphs or buffers. Unit-test saturating estimates and cap monotonicity; fuzz dimensions/counts for overflow and bounds. |
| SCAL-P2c | parked | S | Apply the resource cap before grid construction, preserving at least one worker and current geometry floors. | Blocked on SCAL-P2a/P2b. Add lazy-open integration regressions for capped/uncapped cases, output/seam equivalence, allocation-failure coverage, zimg fuzz, and sanitizers. |
| SCAL-P2d | parked | L | Investigate graph or temporary-buffer sharing only if the worker cap causes a measured throughput regression. | zimg thread-safety and per-call temporary ownership must be proven first; require concurrency-focused integration tests and TSan before any shared resource ships. |
| SCAL-P3a | parked | S | Measure combined persistent thread count, memory, startup, and frame latency when zimg and USM are both enabled (`src/autoupscale.c:578-600,972-1009,1076-1092,1188-1210`). | Still applicable at HEAD, but sequential passes do not establish that duplicate pools matter. Extend end-to-end benchmarks only. |
| SCAL-P3b | parked | S | First cap the two independent pools to one shared CPU budget when both are enabled. | This is the cheapest mitigation and avoids lifecycle coupling. Add pure allocation-policy unit/fuzz tests plus Open integration regressions for zimg-only, USM-only, both, and explicit thread preferences. |
| SCAL-P3c | parked | M | Define a backend-neutral job descriptor and prove both existing worker callbacks can run through it without moving resource ownership. | Blocked on SCAL-P3a showing that SCAL-P3b is insufficient. Add compile-time/API tests and worker-pool unit/fault-injection coverage; no production pool sharing yet. |
| SCAL-P3d | parked | L | Move zimg and USM onto one persistent worker lifecycle while keeping graphs/scratch owned by their backends. | Blocked on SCAL-P3c. Require transition regressions for lazy init, partial spawn, poison, join quarantine, close, backend fallback, and alternating jobs; add integration fuzz and ASan/UBSan/TSan stress. |
| SCAL-P4a | parked | S | Benchmark dispatch wake-to-run latency and completion skew by worker count for the current condition-variable broadcast (`src/threading.h:389-402,449-501,513-548`). | Still applicable at HEAD, but serialization has not been measured. Add test-only timestamps; keep noisy timing out of correctness CI. |
| SCAL-P4b | parked | S | Reduce worker count through existing geometry/resource caps where that beats waking low-value workers. | Cheapest mitigation; blocked on SCAL-P4a. Cover selection with pure unit/fuzz tests and output-equivalence integration tests. |
| SCAL-P4c | parked | M | If wake overhead remains material, prototype a bounded fan-out/tree wake while retaining the existing completion barrier. | Avoid Linux-only futexes unless portability scope changes. Unit-test every partial-init/wake/stop failure with wrappers; stress repeated generations and cancellation under TSan. |
| SCAL-P4d | parked | M | Integrate a winning wake strategy behind the shared worker-pool gate without changing owner callbacks. | Blocked on SCAL-P4c outperforming broadcast. Require worker-pool regression/fault tests, generation-wrap and lost-wake stress, USM/zimg integration fuzz, ASan/UBSan/TSan, and clean fallback to broadcast. |

## Audit picks deliberately rejected

Recorded so future full-project rescans do not repeatedly promote the same
non-findings:

- Adding a CI threshold for in-place USM halo snapshots: the paired alias-mode
  benchmark found no consistent in-place regression on the measured host, and
  cache/write differences prevent the comparison from isolating copy cost.
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
