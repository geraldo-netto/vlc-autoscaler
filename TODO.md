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

| id | status | effort | description | why not now |
|---|---|---|---|---|
| PERF-P1 | parked | M | In-place USM snapshots up to two halo rows per worker serially before each dispatch (`src/usm_pool.c:448-491`). | Folding snapshots into workers needs another readiness phase; profile the current copy cost before adding synchronization. |
| PERF-P2 | parked | M | Multiversion dispatch always chooses the widest supported ISA (`src/usm_pool_dispatch.c:87-121`), although AVX-512 frequency effects and memory-bound throughput are host-specific. | No deployment-host regression has been measured, and a single-baseline build is an available workaround. Benchmark first; if confirmed, add a force/max-variant override or a measured selection policy. |
| SCAL-1 | parked | M | CPU capacity uses `sched_getaffinity` and host `sysconf` only (`src/threading.h:121-174`); neither observes cgroup v2 `cpu.max` or v1 CFS quota. | Deferred by project prioritization; robust handling needs cgroup v1/v2 discovery and validation across nested or delegated controller layouts. |
| SCAL-2 | parked | M | AUTO memory sizing uses host `sysinfo.totalram` (`src/autoupscale.c:394-410`), then selects 720p/1080p from that value (`src/upscale_logic.h:69-82`), ignoring cgroup memory limits. | Deferred by project prioritization; robust handling needs cgroup v1/v2 limit discovery, unlimited-value handling, and container validation. |
| SCAL-3 | parked | M | Thread pinning stores the first allowed logical CPU IDs and assigns workers round-robin (`src/threading.h:99-116`, `src/scaler_zimg.c:784-796`), without physical-core, SMT, or NUMA topology. | Deferred by project prioritization; choosing a topology ordering policy needs cross-SMT/NUMA measurements and platform-specific discovery. |
| SCAL-P1 | parked | L | Static equal-work cells plus a full-frame barrier make latency the slowest worker on heterogeneous or contended CPUs (`src/usm_pool.c:357-365,475-532`; `src/scaler_zimg.c:823-843,1042-1056`; `src/worker_pool.h:364-384`). | Dynamic stealing or heterogeneous sizing substantially complicates a correctness-sensitive frame path; require production profile evidence. |
| SCAL-P2 | parked | M | Zimg memory and lazy graph-build time grow linearly with worker count because each cell owns a graph/tmp buffer and some own tile scratch (`src/scaler_zimg.c:183-214,713-739,797-805,823-859`). | Independent graphs keep processing lock-free and resources remain capped; revisit if high-thread memory profiles justify sharing. |
| SCAL-P3 | parked | L | Zimg and USM keep separate persistent pools derived from the same CPU budget even though their passes run sequentially (`src/autoupscale.c:578-600,972-1009,1076-1092,1188-1210`; `src/scaler_zimg.c:1096-1115`). | Sharing threads is structurally possible through `worker_pool.h`, but remains a large lifecycle change without a measured end-to-end bottleneck. |
| SCAL-P4 | parked | M | Every dispatch wakes all workers through one condition-variable broadcast (`src/threading.h:389-402,449-501,513-548`; `src/worker_pool.h:367-384`), serializing startup at high worker counts. | A tree/atomic wake redesign changes the most failure-sensitive synchronization code; re-profile current production before promotion. |

## Audit picks deliberately rejected

Recorded so future full-project rescans do not repeatedly promote the same
non-findings:

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
