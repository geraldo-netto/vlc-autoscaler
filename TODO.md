# TODO — full-project audit findings

Full-project rescan of the working tree based on HEAD `236ab77` on 2026-07-15.
Scope: all 248 tracked project files, including production source/public headers,
tests and fuzzers, all 154 binary corpus seeds, build/release configuration, CI,
scripts, documentation, and patches. Corpus payload sizes and formats were
checked against their harness parsers. Excluded only `.git`, ignored/generated
build output, and cache files/directories; no tracked project file was excluded.
The working tree was clean at scan start.

Validation: full manual inspection across every category; GCC C11 unit/contract
tests with `-Werror`, ASan, and UBSan; all deterministic fuzz-smoke targets with
`CLANG=gcc`; benchmark builds; declared-shell syntax checks; relative Markdown
link validation; and Lizard 1.17.31 over `src/` plus `tests/` (924 functions,
zero CCN > 10). Repeated sanitizer runs reproduced process-wide thread-count
flakes, and GCC `-fanalyzer` corroborated test OOM paths. The mandatory coverage
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
| UB-10 | open | S | Test allocations are checked nonfatally or not checked before unconditional buffer dereferences (`tests/test_lifetime.c:87-97,141-152,216-232,254-267,325-338`; `tests/test_zimg_helpers.c:249-256`). | OOM makes the ASan/UBSan suite null-dereference instead of reporting a clean setup failure. Add fatal/guarded allocation helpers with cleanup; GCC `-fanalyzer` independently reached several lifetime null paths. |
| UB-11 | open | S | `tests/test_scaler_zimg.c:687-702,853-876,948-962` short-circuits a second allocation into an uninitialized `zt_pic_t dst`, then unconditionally frees it through `tests/zimg_test_util.h:48-50`. | If the first allocation fails, `dst` was never initialized and cleanup reads/frees indeterminate pointers. Zero-initialize both ownership structs or allocate sequentially. |

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
| PERF-8 | open | S | The USM benchmark labels requested rather than effective worker counts: the default pool caps at 12 (`src/usm_pool.c:104-107,409-426`), but `tests/bench_usm_pool.c:175,194-195` prints the request and `scripts/bench_matrix.sh:18-66` publishes 16/20-worker rows. | The 16- and 20-thread rows are actually at most 12-worker runs, so scaling results are mislabeled. Emit requested and `up_usm_pool_effective_threads()` values after warmup, and make the matrix validate/use the effective count. |

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
| DUP-12 | open | S | Six test/benchmark files carry private deterministic PRNG implementations instead of the canonical `tests/prng.h`: `tests/bench_usm_pool.c:22-31`, `tests/fuzz_content_probe.c:57-66`, `tests/test_content_probe.c:374-383`, `tests/test_usm_pool_variants.c:49-56`, `tests/fuzz_usm_variants.c:101-111`, and `tests/test_lifetime.c:41-48`. | Their comparisons do not depend on distinct streams. Use `up_xs32` / `up_fill_random` so generator fixes and intent remain centralized. |

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
| REL-14 | open | S | `src/chroma_classify.h:262-276` clamps an unrepresentable aligned start backward while retaining a positive extent. | `dim=3, off=UINT_MAX` becomes `dim=2, off=UINT_MAX-1`, including a pixel before the input and violating the inward-window invariant at `tests/fuzz_scaler_chroma.c:294-314`. Produce an empty interval/preserve the offset or return failure; add the boundary test and let the fuzzer decode full-width offsets. Picture-view guards prevent OOB, but malformed metadata can stay engaged and drop every frame. |
| REL-15 | open | S | `tests/test_usm_pool.c:363-402` and `tests/test_worker_pool.c:204-226,438-483` assert exact process-wide `/proc/self/task` deltas around owned workers. | ASan/runtime helper threads may appear or retire between snapshots; fresh repeated runs failed in both suites, including worker-pool expected 2/actual 1. Assert wrapped `pthread_create`/join accounting or owned pool state rather than global task counts. |
| REL-16 | open | S | Documentation cross-references are stale: `docs/CINNAMON-DESKTOP-ACTIONS.md:117-127` sends users to `docs/USAGE.md` for a “full option table” although the complete 14-option table is at `README.md:109-127`; the README tree and corpus inventory (`README.md:520-529`; `docs/HOW_IT_WORKS.md:935-947`) omit the tracked/CI-run `corpus_scaler_seam`. | Point tuning readers at the actual option table and include the seam corpus/harness in the project and corpus inventories. |

## portability/standards conformance

| id | status | effort | description | notes |
|---|---|---|---|---|
| PORT-9 | open | S | README, design notes, and Makefile comments still say the old-compiler ISA fallback is incomplete (`README.md:611-617`; `docs/HOW_IT_WORKS.md:620-624`; `Makefile:84-86,156-160`). | `src/cpu_level.h:5-9,80-167` now checks the complete inherited x86-64 v2/v3/v4 feature sets plus OS state. Remove the obsolete deployment warning and stale `PORT-6` references. |
| PORT-10 | open | M | The old-compiler CPUID/XGETBV path is not exercised end-to-end: current compilers select builtins (`src/cpu_level.h:20-25,150-167`), while `tests/test_usm_pool_dispatch.c:76-135` tests only synthetic feature predicates. | A defect in `up_cpu_collect_x86_features`, inline `xgetbv`, or the fallback selection can ship unnoticed. Add a force-fallback test mode and compare its host result with the builtin, or keep an older-compiler CI lane. |

## error handling

| id | status | effort | description | notes |
|---|---|---|---|---|
| ERR-6 | open | M | Thread/gate tests record setup failures nonfatally, then continue with dependent state (`tests/test_threading.c:372-507,604-684`; `tests/test_worker_pool.c:497-510`). | Resource failures can use or destroy uninitialized mutex/condition objects, pthread IDs, or timestamps and make the suite hang/crash. Return setup status, guard dependent cleanup/actions, track successful spawns, and check clocks. |

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
| BUILD-9 | open | S | The shared coverage manifest omits shipped `plane_utils.h`, `cpu_level.h`, and `usm_pool_dispatch.c`, while listing non-production `cli_parse.h` (`scripts/coverage_scope.txt`; omitted logic at `src/plane_utils.h:30-205`, `src/cpu_level.h:27-168`, `src/usm_pool_dispatch.c:98-136`). | Those production units receive no per-file/function verdict. Add the omitted units to the manifest and document intentional exclusions. |
| BUILD-10 | open | S | Threaded coverage is compiled without an explicit atomic profile-update mode (`Makefile:805-811,858-875`), CI suppresses negative-hit parse errors (`.github/workflows/ci.yml:228-244`), and the local parser treats every non-sentinel count, including a negative one, as covered (`scripts/coverage_report.sh:112-123`). | Concurrent counter updates can make coverage vary or over-report. Use `-fprofile-update=atomic`, remove the suppression, and reject non-numeric/negative counts. |
| BUILD-11 | open | S | `PLUGIN_GOALS` omits the standalone `abi-layout-check` and `check-visibility` goals (`Makefile:50-59` versus `:283-325`). | Direct invocation without SDKs bypasses the friendly prerequisite check and fails deep in compilation. Add both goals to `PLUGIN_GOALS`. |
| BUILD-13 | open | S | `coverage-zimg` suppresses both the instrumented harness and `gcov` exit status (`Makefile:717-739`), and Sonar publishes the resulting data (`.github/workflows/ci.yml:228-244`). | Informational percentage reporting does not justify a green target after a crash or corrupt/missing coverage. Preserve the report if useful, then propagate the harness status and fail on a missing or malformed gcov artifact. |
| BUILD-14 | open | S | Zimg is optional (`Makefile:36-42`; `README.md:625-641`), but every CI job installs it and no plugin build forces `HAVE_ZIMG=` (`.github/workflows/ci.yml:25-31,112-120,198-226`); conversely, zimg verification targets exit green or become no-ops when it is absent (`Makefile:696-753`). | Both conditional graphs can rot or be silently skipped. Add a clean zimg-disabled plugin/visibility build and a CI-required zimg assertion that fails when the intended zimg gates are unavailable. |
| BUILD-15 | open | S | The libFuzzer failure upload and local ignore list omit the `oom-*` artifact class (`.github/workflows/ci.yml:127-185`, `.gitignore:21-25`). | OOM reproducers disappear when CI fails and dirty local worktrees. Retain/upload and ignore `oom-*`; consider uploading the already-ignored `slow-unit-*` class too. |
| BUILD-23 | open | S | Documented verification prerequisites do not match the Makefile (`README.md:628-646`): `fuzz-smoke` and `stress` unconditionally use `$(CLANG)` (`Makefile:540-624`), `make test` invokes Python, and coverage needs Python/gcov/GNU tooling. | A GCC/Python-only environment satisfies the stated generic-compiler requirement but the default smoke build fails; the same smoke targets pass with `CLANG=gcc`. Use `CC` where Clang is unnecessary or document/validate each target's actual dependencies. |
| BUILD-24 | open | S | CI builds libFuzzer targets with bare `make fuzz` (`.github/workflows/ci.yml:122-123`), while `FUZZ_CFLAGS` becomes warning-fatal only through `EXTRA_CFLAGS` (`Makefile:137-138`). | Code compiled only without `FUZZ_MAIN` can warn while smoke and plugin checks stay green. Pass `EXTRA_CFLAGS=-Werror` (plus any narrowly documented external-header suppression). |
| BUILD-25 | open | M | CI and `make analyze` have no semantic checks for repository shell or workflow code (`.github/workflows/ci.yml:25-36,98-102`; `Makefile:955-967`). | Syntax checks passed, but quoting, portability, and GitHub Actions expression errors lack a gate. Add pinned ShellCheck for `scripts/*.sh`/`tests/*.sh` and actionlint for workflow YAML. |
| BUILD-26 | open | S | The install-action test uses Python `assert` for its only generated `Exec=` equality checks (`tests/test_install_action.sh:111-114`). | `PYTHONOPTIMIZE=1` removes both checks and the test reports success without validating the result. Replace them with explicit conditionals that raise/exit on mismatch. |

## observability

| id | status | effort | description | notes |
|---|---|---|---|---|
| OBS-10 | open | S | `scripts/bench_matrix.sh:23-66` discards all three raw timings after emitting only their median. | `docs/BENCHMARKS.md:32-39` requires every raw sample for publishable/reproducible results. Emit run-indexed raw rows plus a derived median, or preserve a raw sidecar. |

No additional production finding. Actionable degradation reasons and pinning outcomes are
visible without verbose logging, and advisory-disable wording preserves the
distinction from active EWMA/stat telemetry.

## wiring gaps

| id | status | effort | description | notes |
|---|---|---|---|---|
| WIRE-7 | open | S | `fuzz_worker_pool` is built and smoke-run (`Makefile:413,449-450,496-512`) but omitted from every coverage-guided CI run despite the “every built fuzzer” claim (`.github/workflows/ci.yml:125-175`); Makefile's printed target inventory also omits it and conditional `fuzz_scaler_seam` (`Makefile:414-427`). | Add worker-pool to the short libFuzzer pass and derive or update the printed inventory. |
| WIRE-8 | open | S | `tests/fuzz_scaler_seam.c:145-157` rejects inputs shorter than 16 bytes but consumes only bytes 0-9; the tracked 11-byte seed `tests/corpus_scaler_seam/d14052f14b6ef8abc1de88574e138129f4352576` is therefore a no-op, while bytes 10-15 of the other 79 seeds are inert. | Align the minimum/parser/corpus on one format: accept the 10 consumed bytes and replay the short seed, or deliberately consume 16 bytes and regenerate/minimize every curated seed. |
| WIRE-9 | open | S | All 21 curated `tests/corpus_scaler_chroma/*` seeds are the legacy 8-byte format, while crop-window invariants activate only at 12 bytes (`tests/fuzz_scaler_chroma.c:264-280,358-375`). | Corpus replay gives the new alignment logic no structured starting cases; only later mutations or deterministic smoke reach it. Add 12+ byte seeds for odd/even, empty, boundary, and full-width-offset cases, and update the documented format. |
| WIRE-10 | open | S | All 16 `tests/corpus/*` upscale seeds are 32 bytes and `tests/fuzz_upscale_logic.c:152-169,231-239` copies up to 32, but only bytes 0-23 affect an input. | The zero suffix is inert and mutations in it cannot change coverage. Make the parser/comment and curated corpus a 24-byte format, or deliberately consume/document the remaining bytes. |

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
