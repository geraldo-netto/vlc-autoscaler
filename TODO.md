# TODO

Review findings from a full-project audit (2026-05-30) against the AGENTS.md
review categories. One table per category. Format: `id | status | effort | description | notes`.

2026-06-06 rescan: added two new review categories — **system design** and
**data governance** — and scanned the whole project for them.

2026-07-10 post-fix rescan: full repository, every category, four parallel
audit tracks covering production code, tests/fuzzers/benches, build/CI/scripts,
and documentation. New or reopened: REL-9, ERR-3, PORT-6..7,
BUILD-2, BUILD-11..12, BUILD-14..15,
OBS-6..8, DEAD-9; PORT-3 was expanded with related evidence. ASan/UBSan unit
tests and all deterministic smoke fuzzers pass; lizard is clean (658
functions, none above CCN 10).
`make analyze` is currently red (BUILD-14).

2026-07-10 second rescan (HEAD `42cac05`): full repository, every category,
five parallel audit tracks (UB/memory/security/resources/data-governance,
concurrency/reliability/error-handling, design/performance/duplication,
build/portability/wiring/dead-code, tests/observability). New rows: OBS-9,
REL-10..12, ERR-4, DUP-9, DUP-10, BUILD-16..20; BUILD-2 amended (wider
cppcheck exclusion list). No new finding in security, UB, memory management,
resource management, data governance, concurrency, performance, or
scalability survived refutation. Every previously-open row was re-verified
still valid at HEAD (stale line refs refreshed in place; none could be
closed). lizard clean; `make analyze` still red — the BUILD-14
`variableScope` finding drifted to `tests/test_usm_pool.c:250`.

Effort: S (small) / M (medium) / L (large). Remove a row once its fix is
implemented + tested + merged (`git log` is the durable record). Keep deferred
items under "Open — parked" with a why-not-now note; keep rejected audit picks
under "Audit picks deliberately rejected".

## security

| id | status | effort | description | notes |
|----|--------|--------|-------------|-------|
| (none new) | | | No new security-specific finding survived validation; arithmetic/aliasing issues remain tracked under UB. | |

## data governance

| id | status | effort | description | notes |
|----|--------|--------|-------------|-------|
| DG-2 | no-action | S | Plugin-owned scratch holding decoded frame pixels (`p->src`/`p->dst`/per-tile dst via `alloc_plane_buffer`, `scaler_zimg.c:672-683`; USM scratch `usm_pool_alloc_scratch`, `usm_pool.c:357-366`) is `free()`d without zeroing on close (`scaler_zimg.c:1359-1360`, `usm_pool.c:641`), so the last frame's content lingers in freed heap until reuse. | DECIDED no-action (2026-06-06): matches VLC's own picture pools (no scrub); decoded video is not treated as a secret anywhere in VLC, buffers never leave the process, and no log/error path ever emits buffer contents or addresses. Scrubbing every freed block on close adds cost for no real threat. Recorded so a future pass doesn't re-raise. |

## undefined behavior

| id | status | effort | description | notes |
|----|--------|--------|-------------|-------|
| UB-OVF1 | no-action | S | `autoupscale.c` `MaybeLogStats` computes `next_stats_ns = now_ns + OBS_STATS_INTERVAL_NS`; signed-overflow UB once `now_ns > INT64_MAX - 5e9` | ACCEPTED (theoretical, like PORT-3): `now_ns` is `CLOCK_MONOTONIC` nanoseconds, so reaching 2^63 ns needs ~292 years of uptime — unreachable. A saturating add would add a per-log branch for a case that cannot occur on a monotonic clock. Revisit only if the timestamp source ever changes to something that can approach INT64_MAX. Line refs at HEAD 42cac05: `MaybeLogStats` `autoupscale.c:914-937`, unguarded adds `:919`,`:924`. |

## memory management

| id | status | effort | description | notes |
|----|--------|--------|-------------|-------|
| (none open in production) | | | Production ownership/cleanup paths and `aligned_alloc` sizes remain sound. The benchmark-only invalid-size call found by this rescan is tracked under PORT-7. | |

## performance

| id | status | effort | description | notes |
|----|--------|--------|-------------|-------|
| (none new) | | | No new standalone hot-path regression survived validation. | |

## scalability

| id | status | effort | description | notes |
|----|--------|--------|-------------|-------|
| — | | | SCAL-2 remains resolved: broadcast wake plus a single counting barrier keeps per-frame main-thread dispatch O(1). | |

## concurrency

| id | status | effort | description | notes |
|----|--------|--------|-------------|-------|
| — | | | CON-2 remains resolved by the documented serial-per-instance `Filter()` contract. | |

## code complexity

| id | status | effort | description | notes |
|----|--------|--------|-------------|-------|

## code duplication

| id | status | effort | description | notes |
|----|--------|--------|-------------|-------|
| DUP-3 | keep | S | Worker-pool lifecycle (lazy-init flags, aligned_alloc + memset, sem_init, spawn loop with `constructed`, sticky `lazy_init_failed`) duplicated between `scaler_zimg.c` and `usm_pool.c`; per-worker `_Alignas(64)` struct + rationale comment copy-pasted | DECISION (2026-05-30): keep. A shared scaffold needs a type-erased pool (void* element + per-worker construct/destroy callbacks) since the worker structs and per-worker work differ (per-cell zimg graphs vs scratch rows). The common part is ~15 lines of alloc+memset+spawn; hiding it behind a callback interface across a module boundary adds indirection while the real work stays divergent — net clarity loss. Per AGENTS.md (SOLID only when it improves clarity). |
| DUP-5 | keep | S | Args-valid checks parallel: `usm_pool.c` (`usm_pool_validate_args`) vs `usm.h` (`up_usm__args_valid`) — both null dst/src + stride<width | Marginal: the pool variant also checks its own ptr and uses the stored `p->width`, while `up_usm__args_valid` validates full dims; not cleanly mergeable without threading width/height through. Keep. |
| DUP-8 | open | S | Stripe-partition math (`i*h/n`, last stripe absorbs remainder) computed twice in `usm_pool.c`: `usm_pool_spawn_worker` (:381-384) writes each worker's `y_start/y_end`, then `usm_pool_spawn_all` unconditionally calls `usm_pool_repartition_stripes` (call :440, fn :399-408) which recomputes and overwrites them before any worker can read them (workers block on `go` until the first apply). | The spawn-time assignment is dead code — always overwritten. Delete the `y_start/y_end` lines from `usm_pool_spawn_worker` and let `usm_pool_repartition_stripes` be the single partitioner (its comment already calls the spawn-time values a "redundant (idempotent) re-assignment"). Net −4 lines, one source of truth, no behavior change. Re-verified still-valid 2026-07-10 at HEAD 42cac05; line numbers refreshed. |
| DUP-9 | open | S | The `up_picture_region_t` literal mapping from `scaler_ctx_t` is hand-built at 6 sites across 3 files — src-shaped (coded dims + crop offsets + visible dims) and dst-shaped (dst dims + zero offsets) literals in `scaler_zimg.c:1284-1299`, `scaler_swscale.c:111-124`, and `autoupscale.c` (`RunProbe` :791-798, `ApplyUsmIfEnabled` :885-892). | bd4326c (crop origins) had to touch all six sites in lockstep; a future crop-semantics change that misses one silently desyncs backends/probe/USM — e.g. the probe samples a different window than the scaler scales. Fix: two `static inline` helpers in `scaler.h` (which already sees both types), e.g. `up_scaler_src_region()` / `up_scaler_dst_region()`, replacing the 6 literals; net negative lines, no indirection. |
| DUP-10 | open | S | The "column tiling requires source-direct reads → downgrade to rows-only grid" policy is implemented twice in `scaler_zimg.c` — option-driven at open (`zimg_honor_copy_in_grid` :1047-1062) and alignment-driven on the first frame (`zimg_prepare_first_frame_io` :1247-1266) — with the col_tiled⇒(src_zerocopy ∧ ¬dst_zerocopy) invariant enforced by a third set of scattered assignments (:1120-1123). The two copies have already drifted behaviorally: the open-time path (`zerocopy-src=0`) leaves `dst_zerocopy` at the user's setting (default on), but the first-frame path keeps the tile-forced `dst_zerocopy=false` even when the dst view IS aligned — a misaligned-src wide/short frame permanently pays the ~125 us/frame copy-out that the same configuration avoids via the option path. | Also a system-design smell (one policy, two owners). Fix: extract a single `zimg_apply_rows_only_grid()` helper used by both sites; in the first-frame path restore `dst_zerocopy` from the saved ctx option once col_tiled is dropped (dst alignment is already checked two lines above). Confirm the dst_zerocopy asymmetry is not intentional before changing behavior. |

## architecture/modularity/SOLID

| id | status | effort | description | notes |
|----|--------|--------|-------------|-------|

## system design

| id | status | effort | description | notes |
|----|--------|--------|-------------|-------|
| SYS-3 | no-action | S | `up_usm_pool_variant_name` is a process-global mutable `const char*` (`usm_pool_dispatch.c:83`, constructor `:107-131`) set by an `__attribute__((constructor))` and read per-filter-instance in Open's engagement log (`autoupscale.c:708`). It is the one piece of cross-instance mutable global state. | Not a live bug — write-once at dlopen (constructor completes before any plugin entry point), read-only after, value never changes. Recorded as the SOLE global-mutable-state item for completeness; becomes a data race only if a future variant re-selects at runtime. DEC-1 already rejected an accessor on tradeoff grounds, so keep as-is (treat as write-once). |

## decoupling

| id | status | effort | description | notes |
|----|--------|--------|-------------|-------|
| (none open) | | | No new actionable coupling issue survived this rescan. | |

## business/design patterns/DDD

| id | status | effort | description | notes |
|----|--------|--------|-------------|-------|
| PAT-2 | keep | S | `zimg_process` is an implicit pipeline (point → dispatch) with the zerocopy variation handled per side | DECISION (2026-05-30): keep. The "scattered branches" no longer exist: `zimg_process` is a flat linear skeleton, and the zerocopy variation is a single offset ternary per side (`WORKER_SRC_OFF` vs `WORKER_VLC_SRC_OFF`) plus per-worker `copy_in`/`copy_out` flags consumed inside `worker_main`. A Template-Method vtable would add fn-pointer indirection for two static paths with no real branching to hide. Per AGENTS.md (patterns only when they improve clarity). Revisit if a third output mode appears. |

PAT-1 (group dispatch fn-pointers into a usm_pool_ops_t vtable) DONE — commit 0f92daa.

## reliability/correctness

| id | status | effort | description | notes |
|----|--------|--------|-------------|-------|
| REL-9 | open | S | The deterministic zimg seam fuzzer exceeds its documented smooth-content bound during an extended run: `480x16 -> 1166x42`, 6 workers, I420 reaches max delta 18 versus `SEAM_MAX_DELTA=16`. | Reproduces before the SCAL-6 selector change because both selectors choose the same 2x3 grid. The standard 400-iteration gate passes, but the comment's claimed 4000+ empirical envelope is stale. Preserve the failing seed, then decide whether the quality bound, minimum cell geometry, or independent-graph tiling needs adjustment. Re-verified still-open 2026-07-10 at HEAD b8cbcf6: `SEAM_MAX_DELTA` is still 16 (`fuzz_scaler_seam.c:39`); the in-file comment now honestly cites REL-9 instead of the stale 4000+ envelope, but the delta-18 exceedance itself is unaddressed. NOTE: the repro is currently unreachable from the fuzzer itself — see REL-10 (src_zerocopy=0 collapses every fuzzer config to rows-only grids). |
| REL-10 | open | S | Column-tiled path lost nearly all harness verification after commit 68d998b: `zt_ctx_init` (`zimg_test_util.h:118`) memsets `zimg.src_zerocopy` to 0, and `zimg_honor_copy_in_grid` (`scaler_zimg.c:1047-1062`) now collapses any cols>1 grid to rows-only whenever src_zerocopy==0. Every zt call site that never sets src_zerocopy runs rows-only: the whole seam fuzzer (`fuzz_scaler_seam.c` — its header still claims to gate column tiling), `test_scaler_zimg.c` full-write/determinism/zerocopy-vs-copyout/pin/seam-oracle (`run_zimg(...,src_zc=0,...)`, `run_zimg_threads_in:689`), and `bench_scaler_zimg.c:74` (which also cannot measure the production-default src zero-copy path at all — no CLI knob). | The only remaining tiled-processing check is `test_src_zerocopy_matches_copy` (`test_scaler_zimg.c:357-364`), which compares two runs of the SAME forced config (col_tiled forces src_zerocopy=1 + dst copy-out, `scaler_zimg.c:1120-1123`) — a determinism self-check that cannot detect an unwritten column band (both dsts pre-zeroed 0x00) or a seam-bound violation vs the untiled reference. A black-band or gross-seam regression in the shipped, default-on tiled path would pass the entire suite, fuzzer, and bench. Fix: default `zt_ctx_init` to src_zerocopy=1 (production default), or thread src_zc through the seam fuzzer/bench and restore tiled full-write + seam-oracle variants. |
| REL-11 | open | S | Four fuzzers signal invariant violations by returning 1 from `LLVMFuzzerTestOneInput` instead of aborting: `fuzz_perfmon.c:125`, `fuzz_threading.c:199`, `fuzz_copy_plane.c:156`, `fuzz_stripe_bounds.c:147`. libFuzzer does not treat a nonzero return as a failure (only crashes/sanitizer traps are), so `make fuzz` campaigns cannot fail on these fuzzers' own invariants — only the `*_smoke` mains (which tally `run_one`) can. | The other nine fuzzers `abort()`/`__builtin_trap()` on violation (e.g. `fuzz_decide_tile_grid.c:115`). Fix: abort in the libFuzzer entry or in the check helpers. |
| REL-12 | open | S | `test_geometry_edge_cases.c` `test_zero_hardware` (`:37-44`) asserts nothing (prints only — cannot fail), and `test_edge_dimensions`' bypass branches check nothing; the binary is gated in `make test` (`Makefile:190`) but only the 1x1/1920x1 assert arms can ever fail. | Add expected-outcome checks (0-core/0-RAM must still yield a valid plan or a bypass per `up_decide_target_height`'s contract), or fold into `test_upscale_logic.c` and delete the file. |

## error handling

| id | status | effort | description | notes |
|----|--------|--------|-------------|-------|
| ERR-4 | open | S | `fuzz_scaler_seam.c` leaks the out-picture planes on failed resamples: `run_one` returns without `zt_pic_free(&ref)` when the reference resample fails after allocating `ref` (`:128-129`), and leaks `tiled` when the threaded resample fails (`:130-139` frees only on the OK path); `resample` itself allocates `out` before open/process can fail (`:78-88`). | Under the libFuzzer+ASan build (default leak detection), any legitimately failing iteration (graph-build failure on degenerate geometry, OOM) turns into a harness leak report that aborts/pollutes extended campaigns — the very runs REL-9 depends on. Free the out-param on non-OK inside `resample` or at both call sites. |

## portability/standards conformance

| id | status | effort | description | notes |
|----|--------|--------|-------------|-------|
| PORT-3 | no-action | S | Output-affecting arithmetic right shifts of negative signed values occur in the perfmon EWMA (`perfmon.h:96`) and USM sharpen term (`usm.h:212`). | The C standard leaves these implementation-defined; the perfmon comment's C11 “well-defined” claim is incorrect. ACCEPTED because every supported GCC/Clang target defines arithmetic shift here. Revisit if the compiler/target support set expands. |
| PORT-6 | open | M | Runtime SIMD guards prove only a subset of the ISA used by the compiled baseline: Open checks AVX2 or AVX512F, while the dispatcher/tests check AVX2 or AVX512F+BW (`autoupscale.c:625-639`, `usm_pool_dispatch.c:107-131`; both sites carry "tracked as PORT-6" comments). `-march=x86-64-v3` additionally requires BMI/BMI2/F16C/FMA/LZCNT/MOVBE, and v4 adds AVX512CD/DQ/VL; the generated v4 USM object contains EVEX 256-bit instructions requiring VL. | Gate explicit variants with `__builtin_cpu_supports("x86-64-v3")` / `("x86-64-v4")` in baseline-safe code, or check every required feature. Otherwise a CPU can pass the partial guard and later SIGILL. Keep the documented `native` build explicitly host-bound. |
| PORT-7 | open | S | `bench_usm_pool` accepts arbitrary dimensions >=8 but passes raw `width * height` to `aligned_alloc(64, ...)` (`bench_usm_pool.c:137-140`); e.g. 65x65 produces 4225 bytes, violating C11's size-multiple requirement. | Round the allocation size to 64 with overflow checking, or use a suitable `posix_memalign` wrapper. This corrects the old memory-management claim that every allocation seam was conformant. |
| — | | | PORT-4 (`_Alignas` keyword cleanup) remains resolved. | |

## resource management

| id | status | effort | description | notes |
|----|--------|--------|-------------|-------|
| RES-2 | decided | M | `scaler_zimg.c` + `usm_pool.c` each filter instance spawns up to `UP_THREADS_MAX` (64) zimg workers + up to 64 USM workers + multi-MB scratch, with no process-wide cap across concurrent AutoUpscale instances | DECISION (2026-05-30): per-instance bound is intentional; no aggregate budget added. The two pools run sequentially per frame (rejected SCAL-1), and each pool further clamps thread count to `dst_h / stripe_min_lines` and core count, so the only cross-instance cost is idle-thread address space (untouched stacks), not CPU or RSS. Typical VLC runs one filter instance per pipeline; a global atomic budget would add shared mutable state + a new open-time failure mode for no real gain. Revisit only if a concurrent-many-instance use case appears. |

## API/ABI stability

| id | status | effort | description | notes |
|----|--------|--------|-------------|-------|

## build/toolchain hygiene

| id | status | effort | description | notes |
|----|--------|--------|-------------|-------|
| BUILD-2 | open | S | The claimed static-analysis closure is not CI-gating: CI runs `make analyze`, not `make scan-build` (no scan-build string anywhere in ci.yml; analyze step `ci.yml:74-75`); the cppcheck file list (`Makefile:725-734`) excludes macro-heavy `autoupscale.c`/`scaler_zimg.c` AND also omits `scaler.c`, `scaler_swscale.c`, `usm_pool_dispatch.c`, `picture_view.h`, `scaler_status.h`, `scaler.h`; and the Sonar job installs deps without `libzimg-dev` (`ci.yml:143-149`), so its wrapped plugin build excludes `scaler_zimg.c`. | Reopened. Wire scan-build into CI and install zimg for the production Sonar build, or add an equivalent analyzer that actually sees both production TUs. Local “No bugs found” output does not close a CI coverage gap. Re-verified still-valid 2026-07-10 at HEAD 42cac05; refs refreshed, exclusion list widened. |
| BUILD-8 | open | S | No `-std=` pinned anywhere in the Makefile: every compile (plugin, tests, fuzz, coverage, bench) runs at the compiler's default dialect (gcc on this host: `gnu17` per `__STDC_VERSION__ 201710L`; newer gcc defaults to `gnu23`), while the cppcheck gate checks `--std=c11` — dialect drift between what is analyzed and what is compiled | Code targets C11 (`stdalign.h`, `aligned_alloc` C11 size contract). A default-dialect bump to C23 changes semantics (`bool`/`true`/`false` keywords, old-style declarations removed). Fix: add `-std=gnu11` to `COMMON_CFLAGS`, `TEST_CFLAGS`, `FUZZ_CFLAGS`, `SMOKE_CFLAGS`, `STRESS_CFLAGS_*`, `COV_CFLAGS`, `BENCH_CFLAGS`, `ZIMG_H_CFLAGS` (gnu not c: `_GNU_SOURCE`, `sysinfo`, semaphores in use). |
| BUILD-11 | open | S | Bare `make` builds only `build/usm_pool.o`, not the plugin: the conditional USM object rule is the first target in the file (`Makefile:137-153`), before `all` at line 160. `make -pn` confirms `.DEFAULT_GOAL := build/usm_pool.o`, contradicting README's primary build command. | Set `.DEFAULT_GOAL := all` before any conditional object rule, or move `all` first; add a dry-run/default-goal check. Re-verified still-valid 2026-07-10 via `make -pn` at HEAD 42cac05; line numbers refreshed. |
| BUILD-12 | open | M | Build configuration is not part of object freshness. Existing objects do not depend on `CC`, `MARCH`, `MULTIVERSION`, feature detection, or effective flags; after one build, changing `MARCH=x86-64` to `x86-64-v3` reports nothing to rebuild. A documented wider-ISA-compatible build can therefore silently retain native instructions. | Use configuration-keyed build directories or a generated command/flag stamp that every affected object depends on. Include `HAVE_ZIMG`/pkg-config changes as well as compiler and ISA flags. |
| BUILD-14 | open | S | The required static-analysis target is currently red: `make analyze` exits 2 because cppcheck reports `variableScope` on the `static uint8_t buf[STRIDE * H]` in `test_inplace_stride_mismatch_rejected` (`tests/test_usm_pool.c:250`, drifted from :249). CI invokes this same target. | Move the static buffer into its actual use scope or add a narrowly justified suppression; rerun the full analyzer gate. Re-verified still red 2026-07-10 (`make analyze` exit 2). |
| BUILD-15 | open | M | Non-plugin test/fuzz/stress/coverage/bench targets lack generated dependency files and several manual prerequisites omit included headers. For example `usm.h` includes `zimg_helpers.h` (usm.h:30), but `build/test_usm`'s deps (`Makefile:240-241`) omit it, so touching it leaves the binary "up to date"; the hand-maintained lists span `Makefile:234-580` (test/fuzz/smoke/stress/bench) and `:633-682` (coverage), while only `COMMON_CFLAGS` (`:68`) carries `-MMD -MP`. | Generate/include depfiles for every compiled target instead of maintaining incomplete transitive header lists by hand. The historical depfile fix covered plugin objects only. Re-verified still-valid 2026-07-10; refs refreshed. |
| BUILD-16 | open | S | When pkg-config fails to find zimg, `test-zimg`/`stress-zimg`/`bench-zimg`/`coverage-zimg` become echo stubs that exit 0 (`Makefile:542-545`), so the CI steps "zimg backend invariants" and "zimg backend stress" (`ci.yml:59-60,68-69`) go green having executed zero tests. | If the `libzimg-dev` install or the pkg-config name ever silently breaks, the entire zimg harness disappears from the gate without a red build. Make the stub exit 1 when CI opts in (e.g. `REQUIRE_ZIMG=1` set in ci.yml), or add a CI assertion that zimg was detected after install. |
| BUILD-17 | open | S | The advertised `MULTIVERSION=1` production build (README.md:544-574, docs/USAGE.md:636-637) is never compiled or linked by CI: the plugin is built only at MULTIVERSION=0 (ci.yml's own comment at `:19` confirms it), and the dispatcher TU is exercised only inside test binaries under TEST_CFLAGS. | The exact hazard `usm_pool.h:22-39` warns about (dispatcher/variant symbol drift → unresolved symbols at .so link/load) would surface only on a user's machine, never in CI. Add one CI step: `make clean && make plugin MARCH=x86-64-v3 MULTIVERSION=1 EXTRA_CFLAGS=-Werror`. |
| BUILD-18 | open | S | `actions/upload-artifact@v4` is mutable-tag-pinned (`ci.yml:124`) while checkout (`:23,:83,:138`) and sonarqube-scan-action (`:170`) are SHA-pinned. | Inconsistent supply-chain posture: a compromised v4 tag executes in a job holding the repo token. Pin to a full commit SHA like the other actions. |
| BUILD-19 | open | S | The SonarCloud build-wrapper zip is downloaded and executed with no checksum verification (`ci.yml:156-160`). | CI executes an unverified binary (mitigated: HTTPS, vendor endpoint). Record and verify a sha256 after download. |
| BUILD-20 | open | S | `coverage-zimg` (`Makefile:521-531`) emits `*.gcov` into the repo root and reaches its cleanup `rm -f *.gcov` only when the awk gate passes — and that blanket rm would also delete unrelated user `.gcov` files. The root currently holds 11 stale `.gcov` files (2026-07-10) plus a May-30 `a.out`, all gitignored but polluting the workspace. | Run gcov inside `build/covz` (cd into it) so nothing lands in the root; delete the stale root `a.out`/`*.gcov`. |

## observability

| id | status | effort | description | notes |
|----|--------|--------|-------------|-------|
| OBS-6 | open | S | `process_fail_logged` is filter-global and stays latched after a successful zimg-to-swscale replacement (`autoupscale.c:1022-1029`, fallback `:964-991`), so a later swscale processing failure drops frames without its own warning. | Reset the guard after successful fallback or track the last failed backend identity. Re-verified still-valid 2026-07-10; line numbers refreshed post-42cac05. |
| OBS-7 | open | S | The zimg engagement log reports tile-destination scratch as zero: `log_zimg_open` forces `dst_mb=0` whenever `col_tiled` (`scaler_zimg.c:957-960`), although every tiled worker owns allocated destination planes (`alloc_tile_dst` `:724`, called `:796`). | Sum the per-worker tile plane allocations so the logged scratch footprint matches actual memory use. Re-verified still-valid 2026-07-10; line numbers refreshed post-42cac05. |
| OBS-8 | open | S | Drop statistics are success-driven and incomplete. `filter_NewPicture` failure is not counted (`autoupscale.c:1009-1014`), while backend-null (`:999-1004`) and process failures (`:1030-1035`) increment `dropped_count` but return before `MaybeLogStats` (reached only via `RecordPerf` `:939-950`); a drop-only failure streak therefore never logs/refreshes the summary. | Centralize a drop-record path that increments the counter and services the periodic log/export without requiring a successful frame. Re-verified still-valid 2026-07-10; line numbers refreshed post-42cac05. |
| OBS-9 | open | S | The perfmon freezes after the one-shot advisory: `up_perfmon_record_ns` returns before updating the EWMA once `has_warned` is latched (`perfmon.h:78`), so from that frame on the OBS-3 periodic stats line and the OBS-5 exported `autoupscale-ewma-us` variable (`autoupscale.c:925-936`) report a stale, frozen EWMA for the rest of playback. | The long-run summary was added precisely for sustained-behavior visibility; after the advisory it reports the value at warn time forever (e.g. load later returning to normal is invisible). Decouple warn latching from accumulation: keep updating `ewma_ns` (and `samples_seen`) after `has_warned`, and only suppress the return-1. |
| — | | | OBS-4 remains obsolete because copy-out is already parallel and included in total frame timing. | |

## wiring gaps

| id | status | effort | description | notes |
|----|--------|--------|-------------|-------|
| — | | | WIRE-1 (flat-skip benchmark wiring) and WIRE-2 (single-threaded oracle documentation) remain resolved. | |

## unused functions/methods

| id | status | effort | description | notes |
|----|--------|--------|-------------|-------|
| DEAD-1,2,3,5 | keep | S | `up_usm_workspace_size`, `up_usm__pass1_hblur`, `up_usm__pass2_combine`, `up_usm__args_valid` (usm.h) are reachable only via `up_usm_apply_plane` | DECISION: keep. They are the single-threaded byte-identity TEST ORACLE for the threaded pool (documented at up_usm_apply_plane via WIRE-2, commit 0f92daa). Not dead — intentionally test-only. |
| DEAD-8 | open | S | Two write-only fields on `stripe_worker_t` (`scaler_zimg.c`): `src_x_start` (declared `:201`, written `:769`, never read — its comment "used by active_region" is FALSE: the active-region crop consumes the `src_x_start` PARAMETER at graph-build time in `build_worker_graph_and_tmp`, not the field) and `worker_id` (declared `:203`, written `:789`, never read — the pin call `:814` uses the parameter). | Verified by grep: no read sites for either field. Delete both fields and the misleading comment; net −4 lines, removes a false data-flow claim from the worker struct. Re-verified still-valid 2026-07-10 at HEAD 42cac05; line numbers refreshed. |
| DEAD-9 | open | S | `sws_priv_t.av_fmt` is assigned in `sws_open` but never read (`scaler_swscale.c:26`, written `:77`). | Delete the field and assignment; the local `fmt` already supplies the only use. Re-verified still-valid 2026-07-10; refs unchanged. |
| DEAD-7 | keep | S | `scaler_zimg.c` `construct_workers` partial-build retry + `teardown_constructed_workers` are unreachable in practice: `zimg_open` clamps stripes to >=16 dst rows and source-stripe degeneracy is all-or-nothing, so `0 < constructed < n` never occurs | DECISION: keep as a cheap defensive net against future stripe-bounds changes. Not a correctness bug. |

## Open — parked

Deferred deliberately; why-not-now recorded so a future pass doesn't treat
them as forgotten. The zimg test/bench harness blocker is now REMOVED
(commit 1c53e19: `make test-zimg` / `stress-zimg` / `bench-zimg`), so these
can now be implemented with ASan/UBSan/TSan + throughput validation. Parked
now only on scope, not on lack of a safety net.

| id | effort | description | why parked |
|----|--------|-------------|------------|

## Pending validation

| id | what | why it matters |
|----|------|----------------|
| VAL-1 | Real-VLC validation of source zero-copy now default-ON (commit 2236264). Run actual VLC across I420/YV12/I422/I444 sub-720p sources + the threads/zerocopy options; confirm no crash, no garbled output. Also exercise the 2026-07-10 runtime paths the harness cannot reach: SYS-2 swscale fallback (force a zimg first-frame failure, e.g. ulimit -v, and confirm playback continues on swscale), SYS-5 (`--autoupscale-zerocopy-src=0` on a wide/short source logs the rows-only warn), SYS-6 (USM pool failure warns once), and an odd-crop VP9/AV1 source engaging via the REL-4 even-align. | The harness proves byte-identity on malloc'd pictures but CANNOT reproduce the documented VLC-pool segfault history. Shared picture-view validation catches invalid crop/plane storage and first-frame alignment selects safe scratch I/O, but neither proves deeper pool/lifecycle behavior. If it misbehaves on a real VLC build, revert the default with `--autoupscale-zerocopy-src=0` (one-line flag flip) pending a fix. Dst zero-copy (also default-ON) shares the same caveat but has shipped longer. |

## Audit picks deliberately rejected

Kept here so future passes don't re-pick them.

| id | why rejected |
|----|--------------|
| PORT-5 | Non-x86 test/fuzz builds are outside the documented Linux x86-64 support contract. The x86-64/v3/v4 variant objects are intentional and exercise every supported SIMD level. Decided 2026-07-10. |
| PORT-8 | Process-local unnamed POSIX semaphores are available on the supported Linux target. macOS and other non-Linux behavior is outside the compatibility contract. Decided 2026-07-10. |
| SCAL-1 | "zimg pool + USM pool double the live thread count and oversubscribe." Premise is wrong: the two pools run SEQUENTIALLY within a frame — `Filter()` runs `scaler.process()` (zimg workers) to completion, THEN `ApplyUsmIfEnabled()` (USM workers). They never execute concurrently, so the idle pool's workers consume zero CPU and negligible RSS (untouched stacks). The suggested "subtract the USM count from the zimg budget" would halve each phase's parallelism for no benefit. A true single shared pool is a large refactor for ~nil gain. The only residual cost (idle-thread address space across many concurrent instances) is tracked under RES-2. Decided 2026-05-30. |
| DEC-1 | `up_usm_pool_variant_name` accessor would still require mode-specific bodies; swapping a global for a call adds no decoupling and complicates tests. Decided 2026-05-30. |
| DEC-2 | Link-time safety for shims is preferred over "clever" X-macro indirection per AGENTS.md guidelines. Decided 2026-05-30. |

### Full-project rescan 2026-05-30 — candidates investigated and rejected

Whole-project audit across every AGENTS.md category (parallel reviewers over
all `src/`). NO new actionable findings; every candidate raised was a false
positive or already-documented intentional design. Recorded so future passes
don't re-investigate:

| candidate | why rejected |
|----|--------------|
| `usm_pool.c` `usm_pool_lazy_init` "leaks scratch+workers if `init_done_sem` fails" | Not a leak: `lazy_init_failed` makes the failure sticky and `up_usm_pool_destroy` (called at filter Close) frees `p->scratch` and `p->workers` unconditionally. Same lazy-then-destroy ownership as the zimg pool. |
| `scaler_pick_logic.h` `up_pick_swscale_if_ok` "derefs `swscale_supports` with no NULL check" | Safe: the only public entry `up_scaler_pick_with` guards `swscale_handle == NULL || swscale_supports == NULL` before any dispatch. The zimg/swscale asymmetry is intentional (zimg optional + NULL-guarded per-call; swscale mandatory + guarded once at entry). |
| `autoupscale.c` Open `be->open` failure "leaves scaler priv dangling / Close never called" | Correct VLC contract: on Open returning `VLC_EGENERIC`, Close is never called and nothing is leaked — only `p_sys` is allocated before the check and it is `free()`d on the spot; the backend `open` frees its own priv on failure (it sets `ctx->priv` only on success). |
| `autoupscale.c` `monotonic_ns` returns 0 on `clock_gettime` failure "masks perf warnings" | Intentional + documented: a clock failure must NOT trigger a spurious perf advisory; perfmon ignores non-positive samples by design. |
| `autoupscale.c` `RunProbe` "pixel vs byte width unit mismatch (UB)" | Already handled + documented: width is taken from `format.i_visible_width` (pixels) and clamped to `i_pitch`; the probe is gated to 8-bit luma where 1 byte == 1 px. |
| `scaler_swscale.c` `sws_scale` "implicit pitch narrowing" | No narrowing: `i_pitch` is already `int`, assigned to the `int` stride array `sws_scale` expects; the short-return (`rc != dst_h`) is already checked (ERR-2). |

### Rescan of the SCAL-3 column-tiling + SCAL-4 code (2026-05-30) — rejected

Targeted re-audit of the new 2D-tiling / pinning code (active_region column
tiles, per-worker tile dst scratch, `up_decide_tile_grid`, CPU pinning). One
theoretical UB recorded above (UB-OVF1); the rest were false positives:

| candidate | why rejected |
|----|--------------|
| `scaler_zimg.c` `alloc_one_plane_set` "leaks earlier planes if a later `aligned_alloc` fails" | Not a leak: on partial failure `ps->y/u/v` keep their (partial) values and the OWNER frees them — a column tile's `w->dst` via `release_worker_resources` (gated on `w->col_tiled`, which is set before `alloc_tile_dst`), and the priv scratch via `zimg_close` (`free(p->src/dst.{y,u,v})`). Pre-existing pattern; no plane pointer is dropped. |
| `autoupscale.c` OBS-5 `var_Create` return unchecked | Benign: `var_Create` for a fixed-name INTEGER var effectively only fails on OOM; VLC's `var_Destroy` on an absent variable is a safe no-op (+debug log), so the unconditional `var_Destroy` in `Close()` cannot fault. Worst case the stat var simply isn't exported. |
| `autoupscale.c` `ClampConfig` no upper bound on `skip` | Not a bug: unlike preset/algo/backend/usm (which index tables, hence clamped to MAX), `skip-above` is a plain numeric height threshold compared by value — a large value just means "never skip", no OOB. VLC's `add_integer_with_range` also bounds it at the config layer. |
| `scaler_zimg.c` column-tile pointer math / per-tile dst race | Verified safe by the seam fuzzer (libFuzzer, geometry down to 2px + oversubscribed threads, no OOB), the ASan/UBSan + TSan harness (disjoint cells, no race), and the seam oracle (maxdelta<=6 vs single-graph). |

### UB/memory/security/data-governance rescan 2026-07-10 — rejected

These candidates were traced and rejected:

| candidate | why rejected |
|----|--------------|
| `scaler_zimg.c:494-498` `worker_main` "copies out uninitialized tile/dst scratch to VLC dst when `zimg_filter_graph_process` fails" | Copying indeterminate bytes via `uint8_t` memcpy is not UB, and the frame is unconditionally dropped: `zimg_dispatch_and_wait` returns -1 on any worker `result != 0`, so `Filter()` releases `p_out` — the garbage never leaves the plugin. |
| `usm_pool.c` / `scaler_zimg.c` unchecked `sem_wait` EINTR (UAF/null-deref via broken frame barrier) | Real, but already tracked as CON-3 — found independently this pass, verbatim the same three sites; not re-filed. |
| odd-height 4:2:0 source → zimg graph-build failure → sticky drop-every-frame | SUPERSEDED same day by REL-4 (filed open): the rejection's "conformant streams can't carry odd crops" premise holds only for H.264/HEVC 4:2:0 — VP9/AV1 permit odd frame dims with 4:2:0, and raw/container-cropped sources reach the filter too. See REL-4 for the fix. |
| BUILD-9 candidate: "stale `usm_pool.o`/`usm_pool_dispatch.o` committed to git at repo root" | Wrong: `git ls-files` does NOT list them and `.gitignore:4` (`*.o`, commit 1f7da23) covers them — they were untracked local leftovers, deleted from disk 2026-07-10. Nothing to fix in-tree. |
| `tests/test_zimg_helpers.c:244-245` unchecked `malloc` before writes (null-deref UB under OOM, test code) | Test-only, ~430 KB allocations, immediately crash-visible under the sanitizer harness that runs these tests; no product-code surface. Not worth a row. |

### UB/memory/security/resource/data-governance re-audit 2026-07-10 (post b8cbcf6, src/ only) — rejected

Second same-day pass over production `src/` after the SYS-2 fallback, SYS-5
rows-only, and REL-4 even-align commits. NO new finding in these categories
survived refutation. Candidates traced and rejected:

| candidate | why rejected |
|----|--------------|
| `scaler_zimg.c:748-750` `build_worker_graph_and_tmp` rounds `tmp_size` up to 64 without the wrap check its twin `usm_pool_alloc_scratch` (usm_pool.c:361-363) has — wrap would undersize `w->tmp` (heap overflow) | Unreachable: `tmp_size` comes from `zimg_filter_graph_get_tmp_size` for graphs whose dims are capped at UP_MAX_DIM (32768); real values are single-digit MBs, ~10 orders of magnitude below SIZE_MAX-63. A check would be dead code; recorded for symmetry only. |
| `picture_view.h:78-83` NV12/NV21 layout declares `pixel_pitch=1` for the interleaved UV plane; if a real VLC build reports `i_pixel_pitch=2` there, every NV12/NV21 frame is rejected (permanent drop via swscale TRANSIENT) | Fail-SAFE either way: a mismatch only makes `up_picture_plane_storage_ok` return false → clean frame drop, never OOB (the byte math `x_group_bytes=2 / pixel_pitch=1` covers the full UV row). VLC 3's picture_Setup assigns the chroma description's single pixel_size to every plane, matching 1. Would be a reliability (not memory-safety) issue; real-VLC NV12 playback is already on the VAL-1 checklist — noted there in spirit, no UB row. |
| `usm_pool.c:399-408` `usm_pool_repartition_stripes` writes already-spawned workers' `y_start`/`y_end` without holding `go_lock` (data-race candidate) | Not a race: a worker cannot read those fields until `*generation` advances, which happens under `go_lock` in `usm_pool_run` strictly after the writes; the worker re-acquires the same mutex, establishing happens-before. Same argument covers `usm_pool_set_per_frame` and zimg `point_workers_planes`. Overlaps DUP-8 (the spawn-time assignment is dead code anyway). |
| `autoupscale.c:964-991` `TryBackendFallback` closes the zimg backend while its worker threads might still hold plane pointers into the just-failed frame (UAF candidate) | Workers are quiescent before close on every FATAL path: a worker-result FATAL means the counting barrier completed (all plane writes done, workers re-blocked on the gate); a barrier-error FATAL runs `zimg_stop_workers` synchronously inside `zimg_dispatch_and_wait` before returning. `Filter()` releases the pictures only after `process()` returns. |
| `scaler_zimg.c:1191-1196` main thread reads `workers[i].result` (and usm dst contents) after `sem_wait` with no lock | Already documented at `stripe_worker_t`: each worker's acq_rel `fetch_sub` on `*pending` joins a release sequence; the final `sem_post(all_done)` → `sem_wait` publishes every worker's writes. TSan harness exercises this. |
| `var_InheritInteger` → `int` narrowing for the small tunables (`zimg-stripe-lines`, `usm-stripe-min-rows`, `usm-sharp-threshold`) — implementation-defined on out-of-range | VLC clamps `add_integer_with_range` options at the config layer, and every consumer degrades safely regardless (non-positive → compile-time default; huge minimum collapses the pool to 1 worker via `height/min -> 0 -> 1`). SEC-2/ClampConfig rationale already covers the pattern. |
| `autoupscale.c:825-827` USM-skip log divides by `probe_accum.lap_samples` (div-by-zero candidate) | Guarded: the branch is only reachable when `up_should_skip_usm_for_sharpness` returned 1, which returns 0 when `lap_samples == 0` (content_probe.h:293). |

### Concurrency/reliability/error-handling re-audit 2026-07-10 (post b8cbcf6, src/ only) — rejected

Full trace of the frame lifecycle (Open → probe/pick/process/USM/stats →
Close), worker lifecycle (lazy init, broadcast + counting-barrier dispatch,
repartition, fatal-dispatch drain per 0a54b62), the SYS-2 runtime swscale
fallback, and the bd4326c/REL-4 crop handling. NO new finding in these three
categories survived refutation. REL-9 and ERR-3 re-verified still-open (line
numbers refreshed in their rows). CON-2/CON-3 remain resolved at HEAD:
`up_sem_wait_nointr` guards both barrier waits (`scaler_zimg.c:1186`,
`usm_pool.c:545`) and a non-EINTR failure drains + joins before returning.
Candidates traced and rejected (beyond those in the tables above):

| candidate | why rejected |
|----|--------------|
| lost-wakeup candidate: a worker between its `fetch_sub` and re-locking `go_lock` misses the next dispatch's `pthread_cond_broadcast` | The wake condition is the `*generation != seen_gen` predicate checked under the mutex, not the signal itself: the late worker acquires `go_lock`, observes the already-bumped generation, and runs the frame without ever sleeping. No dispatch can be missed or double-run (`seen_gen` updated under the same lock). |
| fatal-drain leftover: after a barrier failure the last worker still posts `all_done`, leaving the sem at count 1 — a later dispatch's `sem_wait` would return before workers finish | Unreachable: both pools set `pool_broken` on the fatal path and gate every subsequent dispatch on it (`zimg_process:1318`, `up_usm_pool_apply:586`); the stale count only meets `sem_destroy`. |
| workers' `sem_post(all_done)` return unchecked — a failed post would hang the main thread's barrier wait forever | Only failure modes are EOVERFLOW (impossible: value bounded by 1 outstanding post per dispatch, +1 stale after a fatal drain that is never waited on) and EINVAL (the sem is valid for the pool's whole life; destroy is join-ordered behind it). |
| `zimg_prepare_first_frame_io:1260` regrids with `p->n_threads` (tiled rows*cols) instead of the original `up_threads_decide` budget its twin `zimg_honor_copy_in_grid:1055` uses | Provably identical outcome: tiling only engages when `row_limit < budget`, and the tiled `rows*cols >= row_limit` (rows-only is itself a candidate in the maximizing search), so the rows-only recompute lands on `row_limit` under either budget. Cosmetic inconsistency, not behavioral. |
| user `--autoupscale-zimg-stripe-lines=1` + tiny dst (dst_h under ~2x worker count, reachable only via the 4x ratio cap on a sub-32-row source) → `up_compute_stripe_bounds` even-alignment degenerates a stripe → all-or-nothing construct failure → sticky zimg lazy-init failure | Fails GRACEFULLY, not silently: lazy-init failure is FATAL, logged once (OBS-2), and AUTO swaps to swscale via SYS-2; strict zimg drops by explicit user choice. Needs a pathological override plus an absurd source; DEAD-7's defensive net already covers partial builds. |
| odd `i_x_offset`/`i_y_offset` crops on subsampled chromas: REL-4 even-aligns dims but not offsets, so the chroma view truncates (`extent.x = x_offset/2`, picture_view.h:113), shifting chroma half a luma pixel against luma | No OOB and no crash on any path: the extent checks keep every pointer in-bounds; on zimg the +1-byte luma base fails `zimg_view_aligned` → the first frame selects the aligned copy-in path. The residual half-sample chroma shift is inherent to odd-origin crops of 4:2:0/4:2:2 (no exact alignment exists). |
| `EvenAlignSrcDims` skips NV12/NV21 (not in `up_chroma_to_zimg`), so odd VP9/AV1 dims reach swscale unaligned | Deliberate asymmetry: only zimg rejects non-subsample-divisible dims (the REL-4 failure mode); libswscale handles odd 4:2:0 extents internally, and the picture view's ceil-based chroma extents stay in-bounds. |
| `RecordPerf`/`MaybeLogStats` with `monotonic_ns()` failure (`t_end == 0`): the stats tick is seeded as `0 + 5s`, so the first healthy clock read fires one early summary log | One mistimed msg_Dbg tick, self-healing (`next_stats_ns` re-anchors to the healthy `now_ns`); perfmon already ignores non-positive samples by design (documented monotonic_ns contract). |
| `pthread_mutex_lock` / `pthread_cond_broadcast` return values unchecked at the dispatch gates | Default (non-errorcheck, non-robust) process-private mutexes/condvars on the supported Linux target have no runtime failure mode absent API misuse; checking would add untestable dead branches to the hottest dispatch path. |

### Observability + test-harness re-audit 2026-07-10 (post-42cac05 tree) — rejected

Track covering production logging/stats and the tests/fuzz/stress/bench
harness. New rows filed: OBS-9, REL-10..12, ERR-4. Candidates rejected:

| candidate | why rejected |
|----|--------------|
| `scaler_zimg.c:1326-1327` alignment-drift frames return TRANSIENT with no backend-level log distinguishing them from geometry drops | Covered in practice: the first frame selects safe I/O modes (`zimg_prepare_first_frame_io`), a later drift still triggers the one-shot OBS-1 warn in `Filter()`, and `log_zimg_open` records the chosen src/dst modes. A persistent drift is not a realistic VLC pool behavior; adding a second one-shot warn is marginal. |
| `RunProbe` never closes the probe window if `up_picture_view_init` fails every frame (probe_active stays 1, no log) | If views fail persistently, zimg drops every frame too and OBS-1/OBS-2 warns fire; the probe staying silent is the least of the visible symptoms. |
| `autoupscale.c:825-827` USM-skip log divide-by-zero | Duplicate of the same-day UB-track rejection: `up_should_skip_usm_for_sharpness` returns 0 when `lap_samples == 0` (`content_probe.h:293`). |
| seam fuzzer `threads = pick(data + 9, ...)` reuses the dw bytes (correlated axes) | The two moduli (dw range vs 31) are nearly coprime, so the joint space is still covered as v sweeps 2^32; no dead region. |
| `run_zimg`/`zt_pic_free` on a never-initialized `zt_pic_t` after first-alloc failure (wild free) | OOM-only in test code, immediately crash-visible under ASan; same rationale as the earlier `test_zimg_helpers.c` rejection. |
| `test_usm_pool.c` `run_compare` ignores `up_usm_pool_apply`'s return (`:113`) | Fails in the right direction: src is random, so a failed apply leaves dst_mt zeroed and the byte-diff reports nonzero — the test still fails, only the printed reason is less precise. |
| `test_construction_pthread_fail` silently passes as root (RLIMIT_NPROC unenforced) | It fails loudly instead: rc1 would be OK and the `CHECK(rc1 == SCALER_PROCESS_FATAL)` trips — a wrong-environment signal, not a masked bug. |
| `fuzz_scaler_open.c` model mirrors the production logic (mirror oracle) | Accepted pattern for a pure sequencing helper; the model is independent code and the 256-case space is fully enumerated by the smoke main. |
| stress/bench in-place reference asymmetry (`stress_usm_pool.c:116-128` reference runs out-of-place vs pool in-place) | Intentional and correct: the pool's halo snapshots must make in-place output byte-identical to the out-of-place oracle — that asymmetry IS the assertion. |
| `test_scaler_swscale.c` stubs never run real swscale | By design a contract/forwarding test (mock at the FFmpeg boundary); real-pixel behavior is zimg-oracle + VAL-1 territory. |
| stub `vlc_picture.h` lacks refcount/date fields; zt pictures always have `i_lines == i_visible_lines`, offsets 0 | The backends only consume plane pointers/pitches plus negotiated-format crops, which `run_zimg_cropped`/picture_view tests exercise; refcounting stays in `Filter()`, untestable without real VLC (tracked by VAL-1). Padded-`i_lines` pictures are accepted-by-construction (`up_picture_plane_extent_ok` checks against `i_lines`, fuzz_picture_view varies extents). |

### Design/performance/duplication re-audit 2026-07-10 (post-42cac05, src/ only) — rejected

New rows filed: DUP-9, DUP-10. Keep/no-action decisions DUP-3, DUP-5, PAT-2,
SYS-3, RES-2 re-confirmed at HEAD (DUP-3's lifecycle duplication grew ~40
lines with the shared-gate wait/wake/stop trio but stays within the keep's
scope). Performance/scalability/complexity/architecture/decoupling tables
correctly stay empty. Candidates rejected:

| candidate | why rejected |
|----|--------------|
| Extract the shared dispatch-gate struct (`go_lock/go_cv/generation/pending/all_done` + wait/wake/stop trio) into `threading.h` | Covered by DUP-3's explicit keep on pool-scaffold sharing; each copy carries pool-specific memory-ordering commentary that a shared helper would flatten. |
| `point_workers_planes` called twice per frame (two cache-line passes over workers) | O(n_threads) pointer stores, negligible; design already blessed (DUP-7/PERF-6 rationale). |
| Per-frame in-place USM halo snapshots (2 rows × n workers serial memcpy, ~50 KB @1080p) | Documented SYS-4 design; already minimal. |
| `RunProbe` computes `up_block_edge_strength` even when only the sharpness gate needs metrics | First-60-frames only, subsampled; not steady-state hot path. |
| `sws_plane_index` vs `up_zimg_plane_idx` YV12-swap duplication | 3 lines each, differently keyed (chroma vs flag); merging needs a new include for negative clarity gain (DUP-5-style keep). |
| `EvenAlignSrcDims` uses zimg's chroma table even for strict-swscale configs | Even 4:2:0 dims are desirable for any YUV scaler; the 1-px crop is visually free and behaviorally consistent. |
| Broadcast thundering-herd on `go_lock` reacquire at 64 workers | Inherent to condvar broadcast; SCAL-2 resolved design, measured fine. |
| `alloc_plane_buffer` recomputes `plane_alloc_bytes` after the `plane_buffer_bytes` pre-check | 3 multiplications at lazy-init, not per-frame. |
| `zimg_open` re-runs CPU topology detection on every VLC probe open | One 1 KB `CPU_ALLOC` + scan; everything expensive is already deferred to lazy-init. |
| `autoupscale.c` `TryBackendFallback` knowing backend IDs (coupling candidate) | The plugin owns fallback policy per `scaler.h`'s documented contract; not a coupling defect. |

### Build/portability/wiring/dead-code re-audit 2026-07-10 (post-42cac05) — rejected

New rows filed: BUILD-16..20; BUILD-2 amended with the wider cppcheck
exclusion list. All open BUILD/PORT/DEAD rows re-verified still valid (refs
refreshed in place). Wiring sweep: all 14 config knobs added in
`autoupscale.c:244-281` are read at Open and dispatched (verified
`pin_cpus`, `min_stripe_lines`, `zerocopy`, `src_zerocopy` consumption in
`scaler_zimg.c:690-1333`); every `up_*` helper across the headers has
production or intra-header call sites. Candidates rejected:

| candidate | why rejected |
|----|--------------|
| `USM_POOL_CFLAGS := $(subst -O2,-O3,...)` mangles a user-supplied `EXTRA_CFLAGS=-O2` | Contrived invocation; last-flag-wins covers realistic overrides. |
| UP_FOURCC big-endian path relies on `WORDS_BIGENDIAN` never being defined in plugin builds | Consistent with VLC's own headers keyed on the same macro; the six `_Static_assert`s (`autoupscale.c:47-52`) catch any drift at compile time. |
| `threading.h` `CPU_ALLOC`/`CPU_ISSET_S`/`sched_getaffinity` glibc extensions | Inside the Linux x86-64 support contract (same grounds as rejected PORT-8). |
| `-include $(wildcard build/*.d)` parse-time wildcard | Standard idiom; fresh objects' depfiles exist by the rebuild that needs them. |
| `scan-build` target forcing `$(MAKE) clean` | Intentional for full-TU analysis; CI gating is BUILD-2's scope. |
| `sonar-project.properties` `sonar.exclusions` lists paths outside `sonar.sources=src` | Redundant but harmless. |
| CI pins `MARCH=x86-64-v3` for unit tests, so plain x86-64 as the *build* baseline is untested | The explicit `-march=x86-64` variant objects in `test_usm_pool_variants` exercise the SSE2 kernels; the residual gap is rejected-PORT-5 territory. |
| Single-call-site header helpers (`up_pick_auto`, `up_picture_view_layout`, `up__tile_axis_limit`, ...) as orphans | All have intra-header call sites; not dead. |

Coverage note: pure-logic files are 100% (gated). scaler_zimg.c is exercised
to ~94% by `make coverage-zimg` (was 0%); the rest needs a live VLC logger
(log_zimg_open) or fault injection and is intentionally ungated.
