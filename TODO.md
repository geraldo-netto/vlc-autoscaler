# TODO — full-project audit findings

Full production rescan of the working tree based on HEAD `9d279d0` on 2026-07-15.
Scope: all 43 tracked non-test/non-cache files, including production source and
public headers, build/release configuration, CI, scripts, documentation, and
patches. Excluded: `tests/**`, test corpora and fixtures, generated build output,
caches, ignored artifacts, and local untracked settings. Test paths named by
in-scope build files were considered only as wiring; no test source or fixture
was read or used as evidence. The working tree was clean at scan start.

Validation was production-only: GCC and Clang `MULTIVERSION=1` plugin builds
with `-Werror`; linked-symbol visibility and multiversion ISA gates; a GCC
`-fanalyzer` plugin build; source-only Lizard 1.23.0 with CCN <= 10 and at most
7 parameters; Bash syntax checks; ELF hardening inspection; and source-only
cppcheck 2.13.0 (limited by the real VLC macro headers). `clang-tidy`,
`scan-build`, ShellCheck, and actionlint were unavailable locally. No test was
run. Row format: `id | status | effort | description | notes`.

## security

| id | status | effort | description | notes |
|---|---|---|---|---|
| SEC-1 | open | S | `.github/workflows/ci.yml:1-23,101-107,184-193` has no explicit least-privilege token permissions, and all three checkout steps retain credentials. | Repository-default token rights remain available while repository code and downloaded tools execute. Set `permissions: contents: read` and `persist-credentials: false` unless a job proves it needs more. |
| SEC-2 | open | M | CI installs an unpinned PyPI `lizard` package and downloads a mutable Sonar build-wrapper ZIP without integrity verification (`.github/workflows/ci.yml:32-37,203-223`). | Pin the Python package version and hash; pin the wrapper artifact and verify its published checksum/signature before extraction and execution. |
| SEC-3 | open | S | The plugin link does not request immediate binding (`Makefile:95-110,288-298`); fresh ELF inspection found GNU RELRO but no `BIND_NOW`. | Lazy-binding GOT entries can remain writable after load. Add `-Wl,-z,relro,-z,now` and assert the dynamic tags in CI. This is hardening, not evidence of an existing exploit. |

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

No unparked finding: steady-state processing allocates no per-frame
backend state and does not rebuild graphs.

## scalability

| id | status | effort | description | notes |
|---|---|---|---|---|
| SCAL-1 | open | M | CPU capacity uses `sched_getaffinity` and host `sysconf` only (`src/threading.h:131-175,201-224`); neither observes cgroup v2 `cpu.max` or v1 CFS quota. | A container with a broad cpuset but a small CPU quota can create far more zimg/USM workers than it can run. Clamp the affinity count by the effective cgroup quota. |
| SCAL-2 | open | M | AUTO memory sizing uses host `sysinfo.totalram` (`src/autoupscale.c:386-403`), then selects 720p/1080p from that value (`src/upscale_logic.h:69-82`), ignoring cgroup memory limits. | Read cgroup v2 `memory.max` / v1 memory limit and use the smaller finite capacity. |
| SCAL-3 | open | M | Thread pinning stores the first allowed logical CPU IDs and assigns workers round-robin (`src/threading.h:100-116`, `src/scaler_zimg.c:780-789`), without physical-core, SMT, or NUMA topology. | The opt-in setting can pack workers onto siblings or cross NUMA nodes poorly. Prefer one logical CPU per physical core, then siblings, or document an explicit CPU ordering policy. |

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

No open finding. Source-only `lizard -C 10 -a 7 src/` reports 239 functions,
zero violations, and a maximum CCN of 10.

## code duplication

| id | status | effort | description | notes |
|---|---|---|---|---|

No open finding. Both coverage gates now consume one path-qualified manifest.

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
| REL-3 | open | M | Zimg alignment failover counts only consecutive unsafe frames and resets after every safe frame (`src/scaler_zimg.c:1166-1193,1209-1213`); only fatal status invokes fallback (`src/autoupscale.c:1179-1183`). | A picture pool alternating aligned and unaligned buffers can drop every incompatible frame forever without reaching the 30-frame threshold. Use a cumulative/sliding-window threshold, or rebuild/retire direct-I/O mode after recurring drift. |

## portability/standards conformance

| id | status | effort | description | notes |
|---|---|---|---|---|
| PORT-1 | open | M | The GCC < 12 / Clang < 17 fallback in `src/cpu_level.h:25-48` treats AVX2+FMA+BMI+BMI2 as full x86-64-v3/v4 support, omitting the full inherited v2 level and required features such as F16C, LZCNT, and MOVBE. Full-level objects are selected at `Makefile:197-207` and the whole plugin is gated at `src/autoupscale.c:680-700`. | A masked VM CPU can pass the fallback and then execute an unsupported instruction. Require a compiler with full-level builtins for multiversion builds or implement complete CPUID/XGETBV level checks. |

## error handling

| id | status | effort | description | notes |
|---|---|---|---|---|

No separate open finding. Relevant allocation, backend, picture, clock,
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

No open finding. Shared variant declarations, the zimg ABI-major guard, fresh
linked visibility checks, and final-link ISA checks remain coherent; the plugin
exports only the expected VLC entry points.

## build/toolchain hygiene

| id | status | effort | description | notes |
|---|---|---|---|---|
| BUILD-1 | open | S | CI's Clang step builds only `MULTIVERSION=0` and omits Clang visibility verification (`.github/workflows/ci.yml:47-73`), while README claims Clang support for SIMD variants (`README.md:617-618`). | Build and link both Clang configurations, then run visibility and linked-ISA gates under Clang too. |
| BUILD-5 | open | M | Compiler, ISA, feature, and flag values (`Makefile:24-44,67-115,165-216`) are not freshness dependencies of object/link rules at `:288-301`. | Reusing one build directory after changing `CC`, `MARCH`, `EXTRA_CFLAGS`, `MULTIVERSION`, or zimg availability can silently retain incompatible output. Add a configuration-hash stamp or configuration-specific build directory. |
| BUILD-6 | open | S | Empty `VLC_PLUGIN_BASE` becomes non-empty `/video_filter` (`Makefile:43-44`), while install/uninstall validate only the derived value and use unquoted paths (`Makefile:966-976`). | Validate a non-empty base in both targets before deriving the subdirectory, and quote every destination. |
| BUILD-7 | open | S | The Sonar job runs for every pull request but requires `SONAR_TOKEN` (`.github/workflows/ci.yml:3-8,184-248`); fork pull requests do not receive repository secrets. | Gate Sonar to pushes/internal pull requests while retaining token-free build checks for forks. |
| BUILD-9 | open | S | The shared coverage manifest omits shipped `plane_utils.h`, `cpu_level.h`, and `usm_pool_dispatch.c`, while listing non-production `cli_parse.h` (`scripts/coverage_scope.txt`; omitted logic at `src/plane_utils.h:30-205`, `src/cpu_level.h:25-48`, `src/usm_pool_dispatch.c:98-136`). | Those production units receive no per-file/function verdict. Add the omitted units to the manifest and document intentional exclusions. |
| BUILD-10 | open | S | Threaded coverage is compiled without an explicit atomic profile-update mode (`Makefile:801-802,849-866`), CI suppresses negative-hit parse errors (`.github/workflows/ci.yml:225-241`), and the local parser treats every non-sentinel count, including a negative one, as covered (`scripts/coverage_report.sh:65-75`). | Concurrent counter updates can make coverage vary or over-report. Use `-fprofile-update=atomic`, remove the suppression, and reject non-numeric/negative counts. |
| BUILD-11 | open | S | `PLUGIN_GOALS` omits the standalone `abi-layout-check` and `check-visibility` goals (`Makefile:46-58` versus `:282-285,303-324`). | Direct invocation without SDKs bypasses the friendly prerequisite check and fails deep in compilation. Add both goals to `PLUGIN_GOALS`. |

## observability

| id | status | effort | description | notes |
|---|---|---|---|---|

No open finding. Actionable degradation reasons and pinning outcomes are
visible without verbose logging, and advisory-disable wording preserves the
distinction from active EWMA/stat telemetry.

## wiring gaps

| id | status | effort | description | notes |
|---|---|---|---|---|

No open finding. All 14 module options have production consumers; both scaler
backends, runtime fallback, stats lifecycle, USM pool, and multiversion
dispatcher have real production call sites.

## unused functions/methods

| id | status | effort | description | notes |
|---|---|---|---|---|
| UNUSED-1 | open | S | `ALIGN_DOWN_2` occurs only at `src/scaler_zimg.c:98`. | Delete the alias. |
| UNUSED-2 | open | S | Production retains unused or redundant fields: `sws_priv_t.av_fmt` (`src/scaler_swscale.c:27,93`), `stripe_worker_t.worker_id` (`src/scaler_zimg.c:193,743-764`), and picture-view `pixel_pitch/width/height/row_bytes` (`src/picture_view.h:18-26,120-140`) are write-only; `filter_sys_t.probe.enabled` is assigned and read only immediately to initialize `probe.active` (`src/autoupscale.c:320-337,610-615`). | Remove the first two and assign the probe predicate directly to `active`, updating its stale comment. Remove the four view metadata fields or isolate their non-runtime diagnostic purpose; production consumers read only `pixels` and `pitch`. |
| UNUSED-3 | open | S | Nine `static inline` functions have no production-rooted caller: the USM reference subtree (`src/usm.h:66-74,175-186,237-255,273-308`) and the content reference subtree (`src/content_probe.h:71-111,134-193`). | Move intentional reference helpers and support types to a clearly named support header. |

No other unused production static function, macro, field, or orphan call subtree
was found.

## Open — parked

| id | status | effort | description | why not now |
|---|---|---|---|---|
| PERF-P1 | parked | M | In-place USM snapshots up to two halo rows per worker serially before each dispatch (`src/usm_pool.c:448-491`). | Folding snapshots into workers needs another readiness phase; profile the current copy cost before adding synchronization. |
| SCAL-P1 | parked | L | Static equal-work cells plus a full-frame barrier make latency the slowest worker on heterogeneous or contended CPUs (`src/usm_pool.c:357-365,475-532`; `src/scaler_zimg.c:823-843,1042-1056`; `src/worker_pool.h:364-384`). | Dynamic stealing or heterogeneous sizing substantially complicates a correctness-sensitive frame path; require production profile evidence. |
| SCAL-P2 | parked | M | Zimg memory and lazy graph-build time grow linearly with worker count because each cell owns a graph/tmp buffer and some own tile scratch (`src/scaler_zimg.c:183-214,713-739,797-805,823-859`). | Independent graphs keep processing lock-free and resources remain capped; revisit if high-thread memory profiles justify sharing. |
| SCAL-P3 | parked | L | Zimg and USM keep separate persistent pools derived from the same CPU budget even though their passes run sequentially (`src/autoupscale.c:519-587,1053-1069,1167-1187`; `src/scaler_zimg.c:921-974`). | Sharing threads is structurally possible through `worker_pool.h`, but remains a large lifecycle change without a measured end-to-end bottleneck. |
| SCAL-P4 | parked | M | Every dispatch wakes all workers through one condition-variable broadcast (`src/threading.h:389-404,468-480,518-546`; `src/worker_pool.h:364-384`), serializing startup at high worker counts. | A tree/atomic wake redesign changes the most failure-sensitive synchronization code; re-profile current production before promotion. |

## Audit picks deliberately rejected

Recorded so production-only rescans do not repeatedly promote the same
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
