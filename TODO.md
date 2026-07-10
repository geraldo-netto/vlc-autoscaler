# TODO

Fresh full-project audit on 2026-07-10 at `f4da1db`, covering every category
required by `AGENTS.md` plus the repository's established data-governance and
system-design categories. Deployment scope is Linux x86-64 with VLC 3.x;
non-Linux and non-x86 findings are out of scope.

Audit evidence: `make test`, `make fuzz-smoke`, `make test-zimg`, `make
coverage`, and `make complexity` pass. The coverage gate reports 844/846
tracked lines (99.8%) and 99 tracked functions, all at least 80%; lizard reports
675 functions with none above CCN 10. Clang Static Analyzer and GCC
`-fanalyzer` report no production-TU defect. `make analyze` remains red under
cppcheck and is tracked as BUILD-14. Green gates do not cover the UB, wiring,
and false-negative test cases recorded below.

Effort: S (small), M (medium), L (large). Remove a row once its fix is
implemented and tested; `git log` is the durable completion record.

## security

| id | status | effort | description | notes |
|----|--------|--------|-------------|-------|
| (none) | | | No new production security-specific finding survived validation. | CI supply-chain findings remain under BUILD-18 and BUILD-19; memory-safety findings are under undefined behavior. |

## data governance

| id | status | effort | description | notes |
|----|--------|--------|-------------|-------|
| (none) | | | No new data-governance finding. | The decoded-frame scratch scrubbing decision remains recorded under rejected pick DG-2. |

## undefined behavior

| id | status | effort | description | notes |
|----|--------|--------|-------------|-------|
| UB-8 | open | S | `up_copy_plane` does not reject negative or undersized strides; they reach `size_t` pointer arithmetic and potentially out-of-bounds `memcpy` despite the helper's negative-input no-op contract (`src/zimg_helpers.h:128-147`). | Return early when either stride is non-positive or smaller than `row_bytes`. Extend `fuzz_copy_plane.c`, which currently normalizes strides upward at lines 56-67, and the unit tests to cover signed and undersized strides. Production callers currently pass validated views. |
| UB-9 | open | S | Both benchmarks use `atoi` and five smoke runners use `atol`; out-of-range text has undefined behavior, negative iteration counts can report false-green zero randomized iterations, and `bench_scaler_zimg` accepts `INT_MAX` dimensions that overflow `zt_align_up(v + 63)` (`tests/zimg_test_util.h:41-44`). | Use one checked `strtol` parser with `errno`, end-pointer, and destination-range validation in the affected tools; cap zimg dimensions before alignment and reject non-positive iteration counts. Evidence: `bench_usm_pool.c:56-76`, `bench_scaler_zimg.c:36-60`, and the `atol` calls in `fuzz_copy_plane`, `fuzz_perfmon`, `fuzz_picture_view`, `fuzz_threading`, and `fuzz_stripe_bounds`. |

## memory management

| id | status | effort | description | notes |
|----|--------|--------|-------------|-------|
| (none) | | | No new production leak, double-free, ownership, or cleanup-path defect survived validation. | ASan lifetime, zimg, and fuzz harnesses pass. |

## performance

| id | status | effort | description | notes |
|----|--------|--------|-------------|-------|
| (none) | | | No new standalone hot-path regression survived validation. | DUP-10 retains an unnecessary copy-out consequence. |

## scalability

| id | status | effort | description | notes |
|----|--------|--------|-------------|-------|
| (none) | | | No new scalability finding. | Per-frame dispatch is O(1); aggregate multi-instance sizing remains the decided RES-2 tradeoff. |

## concurrency

| id | status | effort | description | notes |
|----|--------|--------|-------------|-------|
| (none) | | | No new race, deadlock, missed-wakeup, or lifetime finding survived the worker-lifecycle trace. | The serial-per-filter-instance contract and broadcast/counting-barrier happens-before paths remain sound. |

## code complexity

| id | status | effort | description | notes |
|----|--------|--------|-------------|-------|

## code duplication

| id | status | effort | description | notes |
|----|--------|--------|-------------|-------|
| DUP-9 | open | S | Source- and destination-shaped `up_picture_region_t` literals are rebuilt at six sites across `scaler_zimg.c`, `scaler_swscale.c`, and `autoupscale.c`. | Add `up_scaler_src_region()` and `up_scaler_dst_region()` helpers in `scaler.h`; crop-semantics changes then have one owner. Current sites: `scaler_zimg.c:1284-1299`, `scaler_swscale.c:111-124`, `autoupscale.c:806-813,900-907`. |
| DUP-10 | open | S | The column-tiling to rows-only downgrade policy is duplicated in `zimg_honor_copy_in_grid` and `zimg_prepare_first_frame_io`; scattered assignments separately enforce the grid/I/O invariant. | Extract one rows-only transition helper. The first-frame path currently leaves `dst_zerocopy=false` after dropping column tiling even when destination alignment permits direct output, unlike the option-driven path (`scaler_zimg.c:1047-1062,1120-1123,1247-1266`). |
| DUP-11 | open | S | The same eight-argument `apply_plane8` compatibility wrapper is copied into four test tools after the `up_usm_plane_io_t` refactor (`test_usm.c:15-21`, `test_usm_pool.c:33-39`, `fuzz_usm.c:22-28`, `stress_usm_pool.c:48-54`). | Construct the descriptor at call sites or share a test-only helper; retaining four flat wrappers recreates the interface duplication the descriptor removed. |

## architecture/modularity/SOLID

| id | status | effort | description | notes |
|----|--------|--------|-------------|-------|
| ARCH-11 | open | S | The USM amount contract conflates the normal user-derived Q8 range (0..512) with the defensive API clamp ceiling (4096): `usm.h:40-42` says values above 4096 are rejected although `up_usm__clamp_amount_q8` clamps them. Variant tests call 0..511 the full range and the variant fuzzer claims a different 0..256 clamp. | Document the normal 0..512 range separately from the callable API's 0..4096 defensive range, correct the reject/clamp wording, and add cross-variant cases for 512, 4096, above-max, and negative values (`test_usm_pool_variants.c:227-235`, `fuzz_usm_variants.c:90-104`). |
| ARCH-12 | open | M | Runtime and verification documentation drifted after the last sync: resolved REL-9 is still described as open; HOW_IT_WORKS still claims all USM pointers are `restrict` and per-function O3 pragmas; published test/coverage totals are stale. | Reconcile `README.md`, `Makefile`, and `docs/HOW_IT_WORKS.md` with current code. Prefer generated or non-volatile aggregate counts. Relevant HOW_IT_WORKS blocks include lines 572-590, 898-905, 929-938, and 1188-1195. PORT-6 owns its CPU-gating documentation. |

## system design

| id | status | effort | description | notes |
|----|--------|--------|-------------|-------|
| (none) | | | No new system-design finding survived validation. | The process-global SIMD variant name remains a write-once loader-time value; see rejected SYS-3. |

## decoupling

| id | status | effort | description | notes |
|----|--------|--------|-------------|-------|
| (none) | | | No new actionable coupling issue survived validation. | Backend fallback policy remains correctly owned by the plugin orchestrator. |

## business/design patterns/DDD

| id | status | effort | description | notes |
|----|--------|--------|-------------|-------|
| (none) | | | No pattern or DDD abstraction would improve the current code without adding indirection. | PAT-2 remains deliberately rejected below. |

## reliability/correctness

| id | status | effort | description | notes |
|----|--------|--------|-------------|-------|
| REL-13 | open | S | The randomized seam fuzzer zero-initializes both outputs and accepts deltas up to 64, so an unwritten low-valued gradient pixel can remain zero and pass; this contradicts the claim that an unwritten band always trips (`tests/fuzz_scaler_seam.c:35-47,102-110,157-169`). | Add an independent full-write oracle by processing identical geometry from complementary destination poisons and requiring identical results before applying the approximate seam comparison. |

## portability/standards conformance

| id | status | effort | description | notes |
|----|--------|--------|-------------|-------|
| PORT-6 | open (reopened) | M | Full x86-64-v3/v4 gating remains incomplete within the supported Linux x86-64 scope. The old-compiler v4 fallback omits required v3 features inherited by v4; `fuzz_usm_variants` still uses headline bits before invoking full-level objects; and `Open` infers a full level from `__AVX2__`/`__AVX512F__` while being compiled in the same high-ISA object it is meant to guard. | `cpu_level.h:25-48` omits at least F16C, LZCNT, and MOVBE in its fallback. `fuzz_usm_variants.c:50-57` bypasses that helper. A native KNL build defines AVX512F/CD but not full v4 and is rejected on its own host. Require direct level probes or enumerate the full inherited levels, use the common helper everywhere, move deployment gating into a baseline-safe wrapper keyed from the requested build level, and update the related README/Makefile/HOW_IT_WORKS/dispatcher/test descriptions. |

## error handling

| id | status | effort | description | notes |
|----|--------|--------|-------------|-------|
| ERR-5 | open | S | `scripts/bench_matrix.sh` lacks `pipefail` and does not require exactly three successful numeric samples; `cut` can mask a failed benchmark in the sampling pipeline, leaving two samples and still emitting a median. | Use `set -euo pipefail`, collect each run only after a successful benchmark, validate one numeric CSV field per run, and assert exactly three samples before sorting. |

## resource management

| id | status | effort | description | notes |
|----|--------|--------|-------------|-------|
| (none) | | | No new fd, thread, semaphore, mutex, or allocation-bound finding survived validation. | The per-instance resource ceiling remains the accepted RES-2 decision below. |

## API/ABI stability

| id | status | effort | description | notes |
|----|--------|--------|-------------|-------|
| ABI-4 | open | S | The plugin has no hidden-visibility policy or export map, so seven internal scaler/USM definitions appear in its dynamic ABI alongside VLC entry metadata; no corresponding headers are installed. | `nm -D --defined-only build/libautoupscale_plugin.so` exposes `scaler_pick`, both backend objects, three pool functions, and `up_usm_pool_variant_name`. Use hidden visibility or a version script and add an `nm`/`readelf` allow-list check that preserves only VLC-required exports. |

## build/toolchain hygiene

| id | status | effort | description | notes |
|----|--------|--------|-------------|-------|
| BUILD-2 | open | S | Static-analysis coverage is not closed: CI runs cppcheck but not `scan-build`; cppcheck omits macro-heavy production TUs and several other source files; the Sonar job does not install `libzimg-dev`, so it can exclude `scaler_zimg.c` while remaining green. | Gate a production analyzer over all enabled TUs and install zimg in the Sonar job, or explicitly verify the captured compile database contains every production source. |
| BUILD-8 | open | S | No compile path pins `-std=` while cppcheck analyzes as C11; compiler-default dialect changes can alter semantics independently of the analysis gate. | Add `-std=gnu11` consistently to plugin, test, fuzz, smoke, stress, coverage, benchmark, and zimg-harness flags. |
| BUILD-11 | open | S | Bare `make` builds only `build/usm_pool.o`: the conditional object rule precedes `all`, and `.DEFAULT_GOAL` resolves to that object despite README calling `make` the primary plugin build. | Set `.DEFAULT_GOAL := all` before conditional rules and add a dry-run/default-goal regression check. Reverified with `make -pn`. |
| BUILD-12 | open | M | Object freshness ignores compiler, flags, `MARCH`, `MULTIVERSION`, and detected optional libraries, so configuration changes can silently reuse incompatible objects. | Use configuration-keyed build directories or command/feature stamp prerequisites for every affected object. |
| BUILD-14 | open | S | The required `make analyze` gate exits 2. Current cppcheck diagnostics include `test_picture_view.c:86`, `scaler_swscale.c:106`, `test_scaler_swscale.c:13,51,205`, and `test_usm_pool.c:262`; the old row named only the final `variableScope` warning. | Resolve or narrowly suppress each justified diagnostic, then keep the exact CI target green. |
| BUILD-15 | open | M | Non-plugin test, fuzz, stress, coverage, and benchmark targets lack generated depfiles and rely on incomplete hand-maintained transitive header prerequisites. | Enable `-MMD -MP` and include depfiles for every compiled target instead of duplicating header dependency lists. |
| BUILD-16 | open | S | If zimg is not detected, `test-zimg`, `stress-zimg`, `bench-zimg`, and `coverage-zimg` are success-returning echo stubs, so a CI dependency/detection regression can execute zero zimg tests while staying green. | Add `REQUIRE_ZIMG=1` behavior for CI or assert zimg detection before the opted-in jobs. |
| BUILD-17 | open | S | CI never compiles or links the advertised `MULTIVERSION=1` production plugin; dispatcher tests do not catch a shared-object link/load mismatch. | Add a clean `MARCH=x86-64 MULTIVERSION=1` plugin build under `-Werror`. |
| BUILD-18 | open | S | `actions/upload-artifact@v4` is mutable-tag-pinned while the other third-party actions are full-SHA pinned. | Pin upload-artifact to a reviewed commit SHA. |
| BUILD-19 | open | S | CI downloads and executes the mutable latest SonarCloud C/C++ build wrapper without checksum or signature verification. | Replace the latest-wrapper download with a SHA-pinned official installer or a trusted compilation-database generator. |
| BUILD-20 | open | S | `coverage-zimg` emits `*.gcov` into the repository root, cleans them only after a successful gate, and uses blanket `rm -f *.gcov`; ignored stale `.gcov` files are present now. | Run gcov entirely inside `$(BUILD)/covz`, clean only owned paths, and remove the stale root artifacts. |
| BUILD-21 | open | S | The new pure `cpu_level.h` helpers are absent from both fail-closed coverage tracked lists, and `test_usm_pool_variants` is absent from `COV_TESTS`, so the 80%-per-function gate can pass without discovering either critical CPU-level helper. | Add a focused CPU-level test and `cpu_level.h` to both coverage lists; explicitly exercise or separately compile the legacy fallback branch. Evidence: `Makefile:600-628`, `scripts/coverage_report.sh:14-31`, `scripts/coverage_per_function.sh:24-27`. |

## observability

| id | status | effort | description | notes |
|----|--------|--------|-------------|-------|
| OBS-6 | open | S | `process_fail_logged` stays latched after successful zimg-to-swscale replacement, so a later swscale failure drops frames without a backend-specific warning (`autoupscale.c:979-1006,1035-1049`). | Reset the guard after fallback or track the backend identity associated with the last warning. |
| OBS-7 | open | S | `log_zimg_open` reports tiled destination scratch as zero although every column-tiled worker owns allocated destination planes (`scaler_zimg.c:734-742,953-970`). | Sum per-worker tile allocations in the engagement log. |
| OBS-8 | open | S | Drop statistics are success-driven: output-allocation failures are not counted, and backend/process failures return before `MaybeLogStats`, so a drop-only streak never publishes or refreshes the summary (`autoupscale.c:1013-1050,929-965`). | Centralize a drop-record path that increments the counter and services periodic logging/export without requiring a successful frame. |

## wiring gaps

| id | status | effort | description | notes |
|----|--------|--------|-------------|-------|
| WIRE-6 | open | S | The 80-file curated seam corpus, including `rel9_480x16_to_1166x42_t6`, has no automated consumer. `test-zimg`'s seam-smoke leg runs 400 PRNG-generated cases, `make fuzz` omits the seam libFuzzer target, and CI never references `tests/corpus_scaler_seam`; seed `d14052f14b6ef8abc1de88574e138129f4352576` is only 11 bytes and would be skipped by the fuzzer's `size < 16` guard. | Replay every valid committed seed in `test-zimg`/CI, either through a bounded libFuzzer run or a smoke-harness seed-file mode, and reject or repair undersized seeds. Evidence: `Makefile:485-504`, `tests/fuzz_scaler_seam.c:181-200`, `.github/workflows/ci.yml`. |

## unused functions/methods

| id | status | effort | description | notes |
|----|--------|--------|-------------|-------|
| DEAD-8 | open | S | `stripe_worker_t.src_x_start` and `.worker_id` are write-only; the graph consumes the constructor parameter directly and CPU pinning uses the local parameter (`scaler_zimg.c:201,203,763-815`). | Delete both fields, their assignments, and the misleading `src_x_start` comment. |
| DEAD-9 | open | S | `sws_priv_t.av_fmt` is assigned in `sws_open` but never read (`scaler_swscale.c:23-27,70-78`). | Delete the field and assignment; the local `fmt` supplies the only use. |
| DEAD-10 | open | S | `filter_sys_t.probe_enabled` is stored only to initialize `probe_active`, while `advice_logged` guards a verdict call already made one-shot by clearing `probe_active` before its sole invocation (`autoupscale.c:329-332,602-608,826-845,1021-1022`). | Assign `probe_active` directly, remove `advice_logged` and its redundant branch, and update the state comment. |

## Open — parked

| id | status | effort | description | notes |
|----|--------|--------|-------------|-------|
| VAL-1 | parked | M | Validate source/destination zero-copy, negotiated crops, runtime zimg-to-swscale fallback, and USM failure behavior in a real VLC 3 picture pool. | The malloc-backed harness proves geometry and byte invariants but cannot reproduce VLC pool recycling/refcount behavior. Exercise I420/YV12/I422/I444 and odd-crop VP9/AV1 inputs plus forced first-frame allocation failure before treating zero-copy lifecycle coverage as complete. |

## Audit picks deliberately rejected

| id | status | effort | description | notes |
|----|--------|--------|-------------|-------|
| DG-2 | no-action | S | Scrub decoded-frame scratch before `free`. | Decoded video is not treated as secret, buffers remain process-local, VLC picture pools do not scrub, and per-close scrubbing adds cost without a threat model. |
| UB-OVF1 | no-action | S | Saturate `CLOCK_MONOTONIC` nanosecond scheduling additions. | Signed overflow requires roughly 292 years of continuous uptime; revisit only if the time source changes. |
| PORT-3 | no-action | S | Replace arithmetic right shifts of negative signed values in EWMA/USM. | Implementation-defined by C, but defined as arithmetic shift by every supported Linux x86-64 GCC/Clang ABI; revisit only if compiler/target scope expands. |
| PORT-5 | rejected | S | Add non-x86 build coverage. | Non-x86 is outside the documented deployment contract. |
| PORT-8 | rejected | S | Replace process-local unnamed POSIX semaphores for macOS. | Non-Linux is outside the documented deployment contract. |
| SCAL-1 | rejected | L | Merge zimg and USM pools to prevent oversubscription. | The pools execute sequentially within a frame and idle workers consume no CPU; merging would reduce clarity for negligible gain. |
| DUP-3 | keep | M | Share the zimg and USM worker-pool lifecycle scaffold. | Worker state/payload and failure cleanup differ; a type-erased callback scaffold adds indirection while leaving the substantive code separate. |
| SYS-3 | no-action | S | Replace the mutable global SIMD variant name. | It is written once by the load-time constructor before external entry and read-only thereafter. |
| PAT-2 | keep | M | Introduce Template Method/strategy objects for the zimg processing pipeline. | The current pipeline is flat; an extra vtable would obscure two static I/O variations. |
| RES-2 | decided | M | Add a process-wide aggregate thread/memory budget across filter instances. | Per-instance counts are bounded by cores, destination height, and hard maxima; typical VLC use has one instance, and a global budget adds shared failure state. |
| DEAD-1,2,3,5 | keep | S | Delete single-threaded USM helpers reachable only from `up_usm_apply_plane`. | They are the byte-identity oracle for threaded/SIMD tests, intentionally test-only rather than dead. |
| DEAD-7 | keep | S | Delete zimg partial-worker construction retry/cleanup as unreachable today. | It is a small defensive net against future stripe-bound changes. |
| DEC-1 | rejected | S | Replace `up_usm_pool_variant_name` with an accessor. | Both build modes would still need separate bodies; the call adds indirection without reducing ownership. |
| DEC-2 | rejected | S | Generate dispatcher declarations/shims through X-macros. | Explicit declarations fail loudly at link time on drift and are clearer than macro indirection. |
