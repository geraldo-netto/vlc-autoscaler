# TODO — full-project audit findings

Full-project rescan of the working tree based on HEAD `33b67fe` on 2026-07-15.
Scope: all 261 tracked project files, including production source/public headers,
tests and fuzzers, all 162 tracked corpus seeds, build/release configuration, CI,
scripts, documentation, and patches. Corpus payload sizes and formats were
checked against their harness parsers. Excluded only `.git`, ignored/generated
build output, and cache files/directories; no tracked project file was excluded.
The working tree was clean at scan start.

Validation: full manual inspection across every category; GCC C11 unit/contract
tests with `-Werror`, ASan, and UBSan; all deterministic fuzz-smoke targets with
`CLANG=gcc`; benchmark builds; declared-shell syntax checks; relative Markdown
link validation; and Lizard 1.17.31 over `src/` plus `tests/` (924 functions,
zero CCN > 10) in the preceding full scan. The 2026-07-15 refresh reran the
complete GCC unit/contract suite with `-Werror` and declared-interpreter shell
syntax checks; both passed. Lizard was unavailable during the refresh, so its
recorded result could not be independently repeated. Repeated sanitizer runs
reproduced process-wide thread-count flakes, and GCC `-fanalyzer` corroborated
test OOM paths. The mandatory coverage
target currently fails one per-function gate. The default fuzz-smoke invocation
also cannot start without Clang, while the same targets pass with GCC. Local
plugin and zimg verification could not start because their SDK metadata is
absent. GCC-built ASan/UBSan stress passed all 27 configurations; its TSan
binary compiled but the local runtime aborted on an unsupported memory mapping.
Clang-specific checks, cppcheck, clang-tidy/scan-build, ShellCheck, and actionlint
were unavailable. Row format:
`id | status | effort | description | notes`.

## security

| id | status | effort | description | notes |
|---|---|---|---|---|
| SEC-1 | open | S | `.github/workflows/ci.yml:1-23,104-110,187-196` has no explicit least-privilege token permissions, and all three checkout steps retain credentials. | Repository-default token rights remain available while repository code and downloaded tools execute. Set `permissions: contents: read` and `persist-credentials: false` unless a job proves it needs more. |
| SEC-2 | open | M | CI installs an unpinned PyPI `lizard` package and downloads a mutable Sonar build-wrapper ZIP without integrity verification (`.github/workflows/ci.yml:32-36,206-226`). | Pin the Python package version and hash; pin the wrapper artifact and verify its published checksum/signature before extraction and execution. |
| SEC-3 | open | S | The plugin link does not request immediate binding (`Makefile:96-110,289-298`); `PLUGIN_LDFLAGS` omits `-z now`. | Lazy-binding GOT entries can remain writable after load. Add `-Wl,-z,relro,-z,now` and assert the dynamic tags in CI. A fresh plugin link was unavailable in this scan; this is a hardening gap, not evidence of an existing exploit. |
| SEC-4 | open | S | The troubleshooting recipe writes verbose VLC output to the fixed path `/tmp/autoupscale.log` (`docs/USAGE.md:423-430`). | A normally created log is world-readable and the predictable name permits symlink or concurrent clobbering; verbose output can expose media paths and URL credentials. Create a mode-0600 file with `mktemp`, store its path in a variable, and reuse that variable in both commands. |
| SEC-5 | open | S | `tests/test_install_action.sh:5-37` creates a predictable `${TMPDIR:-/tmp}/vlc-autoscaler-install-test.$$` tree, recursively removes it, writes executable `PATH` stubs there, and invokes the installer. | In a shared temporary directory an attacker can pre-create or race components to redirect writes or swap the executed stub. Use `mktemp -d` (mode 0700), arm cleanup only after creation, and make signal traps clean up and exit. |

No additional security issue was found in the production media/configuration
paths: geometry is validated before pointer formation, user integers are
normalized or clamped, format strings are literals, and allocation dimensions
are bounded.

## undefined behavior

| id | status | effort | description | notes |
|---|---|---|---|---|

No additional UB finding after tracing shifts, allocation arithmetic, crop and
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
| SCAL-7 | open | S | The three-job CI workflow has no superseded-run cancellation (`.github/workflows/ci.yml:3-10,104-185`). | Rapid pushes retain stale build, analyzer, and sequential timed-fuzzer runs, consuming runner quota and delaying current feedback. Add ref/PR-scoped `concurrency` with `cancel-in-progress` for non-release runs. |

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

No open finding. Full `src/` + `tests/` Lizard analysis reports 924 functions,
zero CCN violations, and a maximum CCN of 10.

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

## portability/standards conformance

| id | status | effort | description | notes |
|---|---|---|---|---|

No additional open finding. GNU/Linux-specific CPU affinity, dynamic loading,
and VLC plugin interfaces are isolated, while the supported compiler/CPU
fallbacks have explicit build and contract coverage.

## error handling

| id | status | effort | description | notes |
|---|---|---|---|---|

No additional open finding. Relevant production allocation, backend, picture, clock,
synchronization, and processing failures are propagated or deliberately
treated as invariant-only cases. The documented recovery contract preserves
worker storage until synchronous retirement completes.

## resource management

| id | status | effort | description | notes |
|---|---|---|---|---|

No separate open finding. Permanent USM/backend failures retire their pools and
resources. Safe retirement waits for active callbacks before releasing their
storage; that deliberately has no hard end-to-end deadline.

## API/ABI stability

| id | status | effort | description | notes |
|---|---|---|---|---|

No open finding. Shared variant declarations, the zimg ABI-major guard, and the
visibility/final-link ISA gates remain coherent by inspection. Their actual
plugin link could not be repeated locally because the required SDKs are absent.

## build/toolchain hygiene

| id | status | effort | description | notes |
|---|---|---|---|---|
| BUILD-1 | open | S | CI's Clang step builds only `MULTIVERSION=0` and omits Clang visibility verification (`.github/workflows/ci.yml:47-73`), while README claims Clang support for SIMD variants (`README.md:625-626`). | Build and link both Clang configurations, then run visibility and linked-ISA gates under Clang too. |
| BUILD-6 | open | S | Empty `VLC_PLUGIN_BASE` becomes non-empty `/video_filter` (`Makefile:44-45`), while install/uninstall validate only the derived value and use unquoted paths (`Makefile:998-1009`). | Validate a non-empty base in both targets before deriving the subdirectory, and quote every destination. |
| BUILD-7 | open | S | The Sonar job runs for every pull request but requires `SONAR_TOKEN` (`.github/workflows/ci.yml:3-8,187-255`); fork pull requests do not receive repository secrets. | Gate Sonar to pushes/internal pull requests while retaining token-free build checks for forks. |
| BUILD-11 | open | S | `PLUGIN_GOALS` omits the standalone `abi-layout-check` and `check-visibility` goals (`Makefile:50-59` versus `:283-325`). | Direct invocation without SDKs bypasses the friendly prerequisite check and fails deep in compilation. Add both goals to `PLUGIN_GOALS`. |
| BUILD-14 | open | S | Zimg is optional (`Makefile:36-42`; `README.md:625-641`), but every CI job installs it and no plugin build forces `HAVE_ZIMG=` (`.github/workflows/ci.yml:25-31,112-120,198-226`); conversely, zimg verification targets exit green or become no-ops when it is absent (`Makefile:696-753`). | Both conditional graphs can rot or be silently skipped. Add a clean zimg-disabled plugin/visibility build and a CI-required zimg assertion that fails when the intended zimg gates are unavailable. |
| BUILD-25 | open | M | CI and `make analyze` have no semantic checks for repository shell or workflow code (`.github/workflows/ci.yml:25-36,98-102`; `Makefile:955-967`). | Syntax checks passed, but quoting, portability, and GitHub Actions expression errors lack a gate. Add pinned ShellCheck for `scripts/*.sh`/`tests/*.sh` and actionlint for workflow YAML. |

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
| UNUSED-1 | open | S | `ALIGN_DOWN_2` occurs only at `src/scaler_zimg.c:98`. | Delete the alias. |
| UNUSED-2 | open | S | Production retains unused or redundant fields: `sws_priv_t.av_fmt` (`src/scaler_swscale.c:27,93`), `stripe_worker_t.worker_id` (`src/scaler_zimg.c:195,746-768`), and picture-view `pixel_pitch/width/height/row_bytes` (`src/picture_view.h:18-26,120-140`) are write-only; `filter_sys_t.probe.enabled` is assigned and read only immediately to initialize `probe.active` (`src/autoupscale.c:328-343,624-630`). | Remove the first two and assign the probe predicate directly to `active`, updating its stale comment. Remove the four view metadata fields or isolate their non-runtime diagnostic purpose; production consumers read only `pixels` and `pitch`. |
| UNUSED-3 | open | S | Nine `static inline` functions have no production-rooted caller: the USM reference subtree (`src/usm.h:66-74,175-186,237-255,273-308`) and the content reference subtree (`src/content_probe.h:71-111,134-193`). | Move intentional reference helpers and support types to a clearly named support header. |
| UNUSED-4 | open | S | `barrier_fault_inject_lose_next_wake()` and its suppression state/branches (`tests/barrier_fault_inject.h:15,49-51,72-76,113-114`) have no caller. | Delete the dormant lost-wake injection subtree or restore an explicit contract test that consumes it. |
| UNUSED-5 | open | S | Warning capture in `tests/test_scaler_swscale.c:9-36` is unused: included production code emits only Info/Dbg, while `g_warn_calls == 0` assertions at `:216,222,258` are vacuous. | Remove the `msg_Warn` apparatus/assertions or add a real warning contract. |

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
