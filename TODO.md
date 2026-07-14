# TODO — full-project audit findings

Full production rescan of the working tree based on HEAD `5cb7db1` on 2026-07-14.
Scope: all 43 tracked non-test files, including production source and public headers,
build/release configuration, CI, scripts, documentation, and patches. Excluded:
`tests/**`, test corpora, generated build output, caches, ignored artifacts, and local
untracked settings. The current uncommitted production edits were included in the scan.

Validation: GCC and Clang plugin builds with `-Werror`; GCC `MULTIVERSION=1` build;
linked-symbol visibility and multiversion ISA checks; `ldd -r`; cppcheck 2.13.0 over
production code (test stubs used only as parser headers); Lizard 1.23.0 over `src/`
with CCN <= 10 and at most 7 parameters; Bash syntax checks; and `git diff --check`.
`scan-build` was unavailable locally. Test files were neither audited nor used as
evidence for findings. Row format: `id | status | effort | description | notes`.

## security

| id | status | effort | description | notes |
|---|---|---|---|---|
| SEC-1 | open | S | `.github/workflows/ci.yml:1-23,189-195` has no explicit least-privilege token permissions, and both checkout steps retain credentials. | Repository-default token rights therefore remain available while repository code and downloaded tools execute. Set `permissions: contents: read` and `persist-credentials: false` unless a job proves it needs more. |
| SEC-2 | open | M | CI executes an unpinned PyPI `lizard` package and an unversioned Sonar build-wrapper ZIP without integrity verification (`.github/workflows/ci.yml:32-37,208-228`). | Pin the Python package/version and hash; pin a wrapper release and verify its published checksum/signature before extraction and execution. |
| SEC-3 | open | S | The plugin link has partial RELRO but no `BIND_NOW` (`Makefile:108,268-277`), leaving lazy-binding GOT entries writable after load. | Add `-Wl,-z,relro,-z,now` and assert the dynamic flags in CI. This is hardening, not evidence of an existing exploit. |

Production media/config paths otherwise had no new security finding: geometry is
validated before pointer formation, user integers are normalized/clamped, format
strings are literals, and allocation dimensions are bounded.

## undefined behavior

| id | status | effort | description | notes |
|---|---|---|---|---|

No other open UB finding after tracing shifts, allocation arithmetic, crop/plane
bounds, worker lifetimes, atomics, and in-place USM halo ownership.

## memory management

| id | status | effort | description | notes |
|---|---|---|---|---|

No separate open finding. Normal and partial-start teardown pairs every allocation
with an owner release. Permanent-failure retention is tracked under RES-1 and RES-2.

## performance

| id | status | effort | description | notes |
|---|---|---|---|---|

No other unparked finding: steady-state processing allocates no per-frame backend
state and does not rebuild graphs.

## scalability

| id | status | effort | description | notes |
|---|---|---|---|---|
| SCAL-1 | open | M | CPU capacity uses `sched_getaffinity` and host `sysconf` only (`src/threading.h:128-153,167-221`); neither observes cgroup v2 `cpu.max` or v1 CFS quota. | A container with a broad cpuset but a small CPU quota can create far more zimg/USM workers than it can run. Clamp the affinity count by the effective cgroup quota. |
| SCAL-2 | open | M | AUTO memory sizing uses host `sysinfo.totalram` (`src/autoupscale.c:383-399`), then selects 720p/1080p from that value (`src/upscale_logic.h:69-82`), ignoring cgroup memory limits. | Read cgroup v2 `memory.max` / v1 memory limit and use the smaller finite capacity. |
| SCAL-3 | open | M | Thread pinning stores the first allowed logical CPU IDs and assigns workers round-robin (`src/threading.h:97-113`, `src/scaler_zimg.c:792-800`), without physical-core, SMT, or NUMA topology. | The opt-in setting can pack workers onto siblings or cross NUMA nodes poorly. Prefer one logical CPU per physical core, then siblings, or document an explicit CPU ordering policy. |

## concurrency

| id | status | effort | description | notes |
|---|---|---|---|---|

The gate shutdown failure that can hang joins is tracked as ERR-1 rather than
duplicated here.

## code complexity

| id | status | effort | description | notes |
|---|---|---|---|---|

No open finding. Source-only `lizard -C 10 -a 7 src/` reports zero violations;
the maximum CCN is 10.

## code duplication

| id | status | effort | description | notes |
|---|---|---|---|---|

## architecture/modularity/SOLID

| id | status | effort | description | notes |
|---|---|---|---|---|

No separate open finding. Backend and worker-pool ownership boundaries remain clear;
the chroma source-of-truth issue is tracked as DUP-1.

## decoupling

| id | status | effort | description | notes |
|---|---|---|---|---|

No open finding. VLC-facing orchestration, scaler backends, pure decision helpers,
and worker-pool lifecycle remain separable at their current seams.

## business/design patterns/DDD

| id | status | effort | description | notes |
|---|---|---|---|---|

No open finding. The scaler strategy table and load-time SIMD dispatch table fit the
problem; another business/domain pattern would add structure without a domain need.

## reliability/correctness

| id | status | effort | description | notes |
|---|---|---|---|---|

## portability/standards conformance

| id | status | effort | description | notes |
|---|---|---|---|---|
| PORT-1 | open | M | The GCC < 12 / Clang < 17 fallback in `src/cpu_level.h:25-48` treats AVX2+FMA+BMI+BMI2 as full x86-64-v3/v4 support, omitting required features such as F16C, LZCNT, MOVBE, and the full inherited v2 level. The selected objects are compiled with full `-march=x86-64-v3/v4` (`Makefile:195-205`). | A masked VM CPU can pass the fallback and then execute an unsupported instruction. Require a compiler with full-level builtins for multiversion builds or implement complete CPUID/XGETBV level checks. |

## error handling

| id | status | effort | description | notes |
|---|---|---|---|---|

No separate row for the unchecked `pthread_join` result: with internally created,
joinable workers its specified failures require an invariant violation. Any ERR-1
recovery change must nevertheless avoid freeing storage unless termination is proven.

## resource management

| id | status | effort | description | notes |
|---|---|---|---|---|

## API/ABI stability

| id | status | effort | description | notes |
|---|---|---|---|---|

No open finding. Current linked artifacts expose only the expected VLC entry
point, `ldd -r` found no live unresolved symbol, and the final-link gate verifies
multiversion vector-ISA use.

## build/toolchain hygiene

| id | status | effort | description | notes |
|---|---|---|---|---|
| BUILD-1 | open | S | CI's Clang step builds only `MULTIVERSION=0` and omits `check-visibility` before cleaning (`.github/workflows/ci.yml:47-72`), while README claims Clang support for SIMD variants (`README.md:598-599`). | Build/link `MULTIVERSION=1` under Clang and run visibility plus ISA checks for both Clang configurations. |
| BUILD-2 | open | M | The cppcheck target explicitly excludes the main orchestration TU and analyzes only a subset of production sources (`Makefile:926-935`); the fuller `scan-build` target at `:937-940` is not run in CI. `autoupscale.c` is therefore seen in CI only by Sonar, whose verdict does not block (BUILD-3). | Analyze the real plugin compile database with clang's analyzer/cppcheck, including `autoupscale.c`, `scaler.c`, `scaler_swscale.c`, and dispatcher code. |
| BUILD-3 | open | S | The Sonar scan action has no quality-gate wait/check (`.github/workflows/ci.yml:248-253`). | Set `sonar.qualitygate.wait=true` or add the official quality-gate action so analyzer failures block CI. |
| BUILD-4 | open | S | `PLUGIN_LDFLAGS` omits `-Wl,-z,defs` (`Makefile:108,268-277`). A shared-object link can therefore succeed with a missing internal dispatcher/API symbol and fail only at `dlopen`. | Link with `-Wl,-z,defs` and optionally assert no undefined project-prefixed symbols. |
| BUILD-5 | open | M | Object and plugin targets do not depend on the effective compiler/configuration (`Makefile:89-114,195-212,268-281`). Reusing one build directory after changing `CC`, `MARCH`, `EXTRA_CFLAGS`, `MULTIVERSION`, or zimg availability can silently reuse incompatible objects/plugin output. | Add a configuration-hash stamp as an object/link dependency or make the build directory configuration-specific. Reproduced: changing GCC/x86-64 to Clang/x86-64-v4 plus a new define performed no compile or link. |
| BUILD-6 | open | S | The install guard checks `VLC_PLUGIN_DIR`, but an empty `VLC_PLUGIN_BASE` produces non-empty `/video_filter` (`Makefile:43-44,943-953`). Install/uninstall can therefore target a root-level directory instead of failing. | Validate non-empty `VLC_PLUGIN_BASE` in both targets before deriving the subdirectory, and quote destination paths. |
| BUILD-7 | open | S | The Sonar job runs on all pull requests but requires `SONAR_TOKEN` (`.github/workflows/ci.yml:3-8,189-253`); fork pull requests do not receive repository secrets. | Gate the job to branch/internal PR events, while leaving the token-free build job available to forks. |
| BUILD-8 | open | S | CI labels its coverage gate “minimum 80%” (`.github/workflows/ci.yml:100`), while the invoked recipes enforce 90% (`Makefile:909-910`). | Make the displayed threshold and enforced threshold share one value so contributors get an accurate failure contract. |

## observability

| id | status | effort | description | notes |
|---|---|---|---|---|
| OBS-1 | open | S | Requested zimg thread pinning silently discards CPU-set allocation and `pthread_setaffinity_np` failures (`src/scaler_zimg.c:107-130,792-800`), and the post-start log reports no pin result (`:874-900`). | Count successful/failed pins and emit one post-start diagnostic when the opt-in request is only partially applied. |
| OBS-2 | open | S | User-facing help says `target-fps=0` disables performance monitoring (`src/autoupscale.c:120-124`, `README.md:113,174-176`), but the implementation intentionally keeps EWMA/stat monitoring active and disables only the advisory (`src/perfmon.h:61-71,102-122`). | Change help/docs to “disable the performance warning/advisory”; keep telemetry behavior explicit. |
| OBS-3 | open | S | Detailed swscale geometry/short-output failures use `msg_Warn` (`src/scaler_swscale.c:35-41,130-166`), although this project already treats that level as hidden by VLC 3's default verbosity; the outer path emits only a generic one-shot failure. | Use a one-shot visible level for the specific reason, consistent with zimg and output-pool failures. |

## wiring gaps

| id | status | effort | description | notes |
|---|---|---|---|---|

All 14 module options otherwise have a production consumer; both scaler backends,
runtime fallback, stats exports, and the multiversion dispatcher have real call sites.

## unused functions/methods

| id | status | effort | description | notes |
|---|---|---|---|---|
| UNUSED-1 | open | S | `ALIGN_DOWN_2` at `src/scaler_zimg.c:98` has no use. | Delete the alias. |
| UNUSED-2 | open | S | Several fields are write-only in production: `sws_priv_t.av_fmt` (`src/scaler_swscale.c:23-31,84-92`), `stripe_worker_t.worker_id` (`src/scaler_zimg.c:194,754-776`), and `up_picture_plane_view_t`'s `pixel_pitch/width/height/row_bytes` (`src/picture_view.h:16-24,173-178`). | Remove the first two. Either remove the view metadata from production or document/isolate its non-runtime diagnostic purpose. |
| UNUSED-3 | open | S | Ten `static inline` functions have no production-rooted caller: the `up_usm_apply_plane` reference subtree (five functions), the Laplacian/block-edge reference subtree (four), and `up_worker_pool_inline` (`src/usm.h:66,175-301`, `src/content_probe.h:71-193`, `src/worker_pool.h:159-162`). | They emit no code unless used. Move intentional reference helpers to a clearly named support header or document the roots uniformly; delete `up_worker_pool_inline` if no public diagnostic contract needs it. |

No other unused production static function or macro was found.

## Open — parked

| id | status | effort | description | why not now |
|---|---|---|---|---|
| PERF-P1 | parked | M | In-place USM snapshots up to two halo rows per worker serially before each dispatch (`src/usm_pool.c:437-480`). | Folding snapshots into workers needs another readiness phase; profile the current copy cost before adding synchronization. |
| SCAL-P1 | parked | L | Static equal-work stripes plus a full-frame barrier make latency the slowest worker on heterogeneous or contended CPUs (`src/usm_pool.c:346-371,497-518`, `src/scaler_zimg.c:1034-1070`). | Dynamic stealing or heterogeneous sizing substantially complicates a correctness-sensitive frame path; require production profile evidence. |
| SCAL-P2 | parked | M | Zimg memory and lazy graph-build time grow linearly with worker count because each cell owns a graph/tmp buffer and some own tile scratch (`src/scaler_zimg.c:738-856`). | Independent graphs keep processing lock-free and resources remain capped; revisit if high-thread memory profiles justify sharing. |
| SCAL-P3 | parked | L | Zimg and USM keep separate persistent pools derived from the same CPU budget even though their passes run sequentially (`src/autoupscale.c:564-584` and `src/scaler_zimg.c:864-988`). | Sharing threads is now structurally easier through `worker_pool.h`, but it remains a large lifecycle change without a measured end-to-end bottleneck. |
| SCAL-P4 | parked | M | Every dispatch wakes all workers through one condition-variable broadcast (`src/threading.h:364-377`), serializing startup at high worker counts. | Previous microbenchmarks indicate a material small-pass cost, but a tree/atomic wake redesign changes the most failure-sensitive synchronization code. Re-profile current production before promotion. |

## Audit picks deliberately rejected

Recorded so production-only rescans do not repeatedly promote the same non-findings:

- Unifying `worker_copy_in_stripe` / `worker_copy_out_stripe` /
  `worker_copy_out_tile`: their direction and offset invariants differ; a generic
  helper would require a wide parameter surface.
- Adding a pool-wide cleanup hook solely because `prepare` is a pool callback:
  shared allocations are explicitly owner-owned and both owners already release
  them. Permanent-failure retirement is the real gap and is tracked by RES-1/RES-2.
- Treating `lazy_done` / `lazy_failed` as an atomicity defect: VLC's video-filter
  callback contract serializes the lifecycle; revisit only if that contract changes.
- Removing per-worker boundary-row re-blur: the small duplicate computation is the
  invariant that permits barrier-free, race-free in-place USM.
- Adding alignment assumptions to USM vector loops: valid row alignment depends on
  width/stride, and prior measurements found no actionable gain over unaligned moves.
