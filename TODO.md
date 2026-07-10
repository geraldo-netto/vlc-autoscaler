# TODO

Review findings from a full-project audit (2026-05-30) against the AGENTS.md
review categories. One table per category. Format: `id | status | effort | description | notes`.

2026-06-06 rescan: added two new review categories — **system design** and
**data governance** — and scanned the whole project for them (SYS-1..3, DG-1..2).

2026-07-10 post-fix rescan: full repository, every category, four parallel
audit tracks covering production code, tests/fuzzers/benches, build/CI/scripts,
and documentation. New or reopened: ARCH-10, REL-6..9, ERR-3, PORT-6..8,
BUILD-2, BUILD-11..12, BUILD-14..15,
OBS-6..8, WIRE-5, DEAD-9; DG-1, PORT-3, and BUILD-10 were
expanded with related evidence. ASan/UBSan unit tests and all deterministic
smoke fuzzers pass; lizard is clean (538 functions, none above CCN 10).
`make analyze` is currently red (BUILD-14).

Effort: S (small) / M (medium) / L (large). Remove a row once its fix is
implemented + tested + merged (`git log` is the durable record). Keep deferred
items under "Open — parked" with a why-not-now note; keep rejected audit picks
under "Audit picks deliberately rejected".

## security

| id | status | effort | description | notes |
|----|--------|--------|-------------|-------|
| (none new) | | | No new security-specific finding survived validation; buffer-geometry defense gaps remain tracked under DG-1/DG-3 and arithmetic/aliasing issues under UB. | |

## data governance

| id | status | effort | description | notes |
|----|--------|--------|-------------|-------|
| DG-1 | open | S | zimg's picture pre-flight checks minimum pitch but not `i_lines`/`i_visible_lines`; the USM apply path likewise validates stride but writes the configured height (`usm_pool.c:451-459`). | Defense-in-depth under VLC's negotiated-format contract: a short plane can drive an OOB read/write. Require sufficient rows for luma/chroma on both paths. ARCH-4 now carries and tests independent U/V pitches, resolving the distinct-chroma-stride half of the original finding. |
| DG-2 | no-action | S | Plugin-owned scratch holding decoded frame pixels (`p->src`/`p->dst`/per-tile dst via `aligned_alloc`, `scaler_zimg.c:611-613`; USM scratch `usm_pool.c:521`) is `free()`d without zeroing on close (`zimg_close` ~1209), so the last frame's content lingers in freed heap until reuse. | DECIDED no-action (2026-06-06): matches VLC's own picture pools (no scrub); decoded video is not treated as a secret anywhere in VLC, buffers never leave the process, and no log/error path ever emits buffer contents or addresses. Scrubbing every freed block on close adds cost for no real threat. Recorded so a future pass doesn't re-raise. |
| DG-3 | open | S | `sws_process` (`scaler_swscale.c:94-123`) has NO picture-geometry pre-flight — the swscale twin of zimg's `zimg_pic_ok` guard. It builds `src_data`/`dst_data` from however many planes the pictures claim (capped at 4 but not floored at the chroma's required count), never checks `p_pixels != NULL` or `i_pitch >=` visible width, and passes Open-time `ctx->src_h` as the row count regardless of the incoming picture. A malformed/drifted picture (`i_planes < 3` for YUV420P → `src_data[1]`/`[2]` stay NULL; pitch < width; fewer rows than `src_h`) drives a null-deref / OOB read-write INSIDE libswscale — the exact failure class DG-1 records for zimg, except swscale checks nothing at all and is the fallback used precisely when zimg is absent (NV12/NV21/RGB always route here). | Defense-in-depth gap under the same trust contract as DG-1, not a live exploit. Asymmetry: zimg drops a malformed frame with a one-shot warn; swscale marches into the library. Direction: add a small pre-flight (planes >= required-for-chroma, non-NULL `p_pixels`, `i_pitch >=` visible width, rows >= `src_h`/`dst_h`) on both pictures in `sws_process`, mirroring `zimg_pic_ok`. Related: the USM apply path shares DG-1's height-trust (validates stride but writes `p->height` rows unconditionally, `usm_pool.c:451-459`) — fold into the DG-1 fix rather than a separate item. 2026-07-10 audit. |

## undefined behavior

| id | status | effort | description | notes |
|----|--------|--------|-------------|-------|
| UB-OVF1 | no-action | S | `autoupscale.c` `MaybeLogStats` computes `next_stats_ns = now_ns + OBS_STATS_INTERVAL_NS`; signed-overflow UB once `now_ns > INT64_MAX - 5e9` | ACCEPTED (theoretical, like PORT-3): `now_ns` is `CLOCK_MONOTONIC` nanoseconds, so reaching 2^63 ns needs ~292 years of uptime — unreachable. A saturating add would add a per-log branch for a case that cannot occur on a monotonic clock. Revisit only if the timestamp source ever changes to something that can approach INT64_MAX. |

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
| DUP-3 | keep | S | Worker-pool lifecycle (lazy-init flags, aligned_alloc + memset, sem_init, spawn loop with `constructed`, sticky `lazy_init_failed`) duplicated between `scaler_zimg.c` and `usm_pool.c`; per-worker `_Alignas(64)` struct + rationale comment copy-pasted | DECISION (2026-05-30): keep. A shared scaffold needs a type-erased pool (void* element + per-worker construct/destroy callbacks) since the worker structs and per-worker work differ (per-stripe zimg graphs vs scratch rows). The common part is ~15 lines of alloc+memset+spawn; hiding it behind a callback interface across a module boundary adds indirection while the real work stays divergent — net clarity loss. Per AGENTS.md (SOLID only when it improves clarity). |
| DUP-5 | keep | S | Args-valid checks parallel: `usm_pool.c` (`usm_pool_validate_args`) vs `usm.h` (`up_usm__args_valid`) — both null dst/src + stride<width | Marginal: the pool variant also checks its own ptr and uses the stored `p->width`, while `up_usm__args_valid` validates full dims; not cleanly mergeable without threading width/height through. Keep. |
| DUP-8 | open | S | Stripe-partition math (`i*h/n`, last stripe absorbs remainder) computed twice in `usm_pool.c`: `usm_pool_spawn_worker` (:310-313) writes each worker's `y_start/y_end`, then `usm_pool_spawn_all` unconditionally calls `usm_pool_repartition_stripes` (:340-345) which recomputes and overwrites them before any worker can read them (workers block on `go` until the first apply). | The spawn-time assignment is dead code — always overwritten. Delete the `y_start/y_end` lines from `usm_pool_spawn_worker` and let `usm_pool_repartition_stripes` be the single partitioner (its comment already calls the spawn-time values a "redundant (idempotent) re-assignment"). Net −4 lines, one source of truth, no behavior change. |

## architecture/modularity/SOLID

| id | status | effort | description | notes |
|----|--------|--------|-------------|-------|
| ARCH-10 | open | M | Public/internal documentation describes several pre-refactor paths and contradicts current code: the module string calls Lanczos the default although Spline36 is configured (`autoupscale.c:88-94,235-240`); README gives `skip-above` range `1+` although 0 disables it and its suite/fuzzer counts lag the Makefile; HOW_IT_WORKS still describes eager zimg allocation, a swscale-only Filter path, two-pass/per-worker-semaphore USM dispatch; and `content_probe.h:5-9` claims the observe-only probe bypasses output. | Refresh the option defaults/ranges, generated target counts, and the Open, Filter, zimg-lifecycle, USM-pool, and probe sections as one consistency pass. Later sections already document parts of the current lazy/broadcast behavior, making the file self-contradictory. |

## system design

| id | status | effort | description | notes |
|----|--------|--------|-------------|-------|
| SYS-3 | no-action | S | `up_usm_pool_variant_name` is a process-global mutable `const char*` (`usm_pool_dispatch.c:85`) set by an `__attribute__((constructor))` and read per-filter-instance in Open's engagement log (`autoupscale.c:616`). It is the one piece of cross-instance mutable global state. | Not a live bug — write-once at dlopen (constructor completes before any plugin entry point), read-only after, value never changes. Recorded as the SOLE global-mutable-state item for completeness; becomes a data race only if a future variant re-selects at runtime. DEC-1 already rejected an accessor on tradeoff grounds, so keep as-is (treat as write-once). |

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
| REL-6 | open | M | Non-zero VLC visible-area crop offsets are ignored. `ResolveInputDims` uses visible width/height, but zimg, swscale, and the content probe all start at raw `p_pixels` (`autoupscale.c:395-404,733-751`, `scaler_zimg.c:1092-1107`, `scaler_swscale.c:94-117`), so they scale/probe the top-left physical rectangle rather than the declared visible rectangle. | Centralize an offset-aware picture-plane view that accounts for chroma subsampling, packed/interleaved formats, and pixel pitch; validate offset plus extent. Add cropped I420/NV12/RGB tests. Distinct from DG-1/DG-3, which cover allocation geometry rather than logical crop origin. |
| REL-8 | open | S | AUTO mode does not try swscale when the preferred zimg backend fails during `open`; `Open()` logs and aborts immediately (`autoupscale.c:617-634`). This includes the zimg runtime ABI-major rejection, even though swscale is the documented universal fallback. | In AUTO only, select/open swscale after zimg open failure; forced-zimg must still fail. The previously rejected Open candidate covered cleanup ownership, not fallback behavior. |
| REL-9 | open | S | The deterministic zimg seam fuzzer exceeds its documented smooth-content bound during an extended run: `480x16 -> 1166x42`, 6 workers, I420 reaches max delta 18 versus `SEAM_MAX_DELTA=16`. | Reproduces before the SCAL-6 selector change because both selectors choose the same 2x3 grid. The standard 400-iteration gate passes, but the comment's claimed 4000+ empirical envelope is stale. Preserve the failing seed, then decide whether the quality bound, minimum cell geometry, or independent-graph tiling needs adjustment. |
| — | | | REL-3 (runtime zimg API major-version probe) remains implemented; REL-8 tracks the missing AUTO fallback around that probe. | |

## error handling

| id | status | effort | description | notes |
|----|--------|--------|-------------|-------|
| ERR-3 | open | S | Timing utilities ignore both `clock_gettime` results and then consume possibly uninitialized `timespec` values (`bench_usm_pool.c:117,123`, `bench_scaler_zimg.c:82,85`, `stress_usm_pool.c:215,222`). | Check both calls and fail the run/configuration with a clear timing error. This is distinct from production `monotonic_ns`, which handles clock failure deliberately. |

## portability/standards conformance

| id | status | effort | description | notes |
|----|--------|--------|-------------|-------|
| PORT-3 | no-action | S | Output-affecting arithmetic right shifts of negative signed values occur in the perfmon EWMA (`perfmon.h:96`) and USM sharpen term (`usm.h:212`). | The C standard leaves these implementation-defined; the perfmon comment's C11 “well-defined” claim is incorrect. ACCEPTED because every supported GCC/Clang target defines arithmetic shift here. Revisit if the compiler/target support set expands. |
| PORT-5 | open | S | `make test` / `make fuzz` cannot build on non-x86 hosts: the SIMD-variant object rules hardcode `-march=x86-64` / `x86-64-v3` / `x86-64-v4` literals (Makefile:695-702, 293-298, 378-383), so the whole `test` target (which depends on `test_usm_pool_variants`) fails on aarch64 even though `plugin` builds fine there via `MARCH=native` | Also needs gcc>=11/clang>=12 for `x86-64-v4`. If x86-only is intentional, gate the variant targets behind an arch check and skip with a message instead of a compile error; otherwise fall back to a single-baseline variant test off-x86. |
| PORT-6 | open | M | Runtime SIMD guards prove only a subset of the ISA used by the compiled baseline: Open checks AVX2 or AVX512F, while the dispatcher/tests check AVX2 or AVX512F+BW (`autoupscale.c:573-586`, `usm_pool_dispatch.c:103-133`). `-march=x86-64-v3` additionally requires BMI/BMI2/F16C/FMA/LZCNT/MOVBE, and v4 adds AVX512CD/DQ/VL; the generated v4 USM object contains EVEX 256-bit instructions requiring VL. | Gate explicit variants with `__builtin_cpu_supports("x86-64-v3")` / `("x86-64-v4")` in baseline-safe code, or check every required feature. Otherwise a CPU can pass the partial guard and later SIGILL. Keep the documented `native` build explicitly host-bound. |
| PORT-7 | open | S | `bench_usm_pool` accepts arbitrary dimensions >=8 but passes raw `width * height` to `aligned_alloc(64, ...)` (`bench_usm_pool.c:130-141`); e.g. 65x65 produces 4225 bytes, violating C11's size-multiple requirement. | Round the allocation size to 64 with overflow checking, or use a suitable `posix_memalign` wrapper. This corrects the old memory-management claim that every allocation seam was conformant. |
| PORT-8 | open | M | Both worker pools unconditionally depend on process-local unnamed POSIX semaphores (`usm_pool.c:328-335`, `scaler_zimg.c:952-954`). Platforms such as macOS expose the API but do not implement `sem_init`, so USM disables at first use; AUTO zimg falls back to swscale, while forced-zimg drops frames despite other code/docs discussing non-Linux operation. | Replace the one-count completion semaphore with a mutex/condition-variable counter already compatible with the broadcast design, or explicitly scope/document the plugin as requiring unnamed-semaphore support. |
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
| BUILD-2 | open | S | The claimed static-analysis closure is not CI-gating: CI runs `make analyze`, not `make scan-build`; cppcheck explicitly excludes macro-heavy `autoupscale.c`/`scaler_zimg.c`; and the Sonar job omits `libzimg-dev`, so its wrapped plugin build excludes `scaler_zimg.c` (`Makefile:633-647`, `.github/workflows/ci.yml:26-37,72-76,144-168`). | Reopened. Wire scan-build into CI and install zimg for the production Sonar build, or add an equivalent analyzer that actually sees both production TUs. Local “No bugs found” output does not close a CI coverage gap. |
| BUILD-8 | open | S | No `-std=` pinned anywhere in the Makefile: every compile (plugin, tests, fuzz, coverage, bench) runs at the compiler's default dialect (gcc on this host: `gnu17` per `__STDC_VERSION__ 201710L`; newer gcc defaults to `gnu23`), while the cppcheck gate checks `--std=c11` — dialect drift between what is analyzed and what is compiled | Code targets C11 (`stdalign.h`, `aligned_alloc` C11 size contract). A default-dialect bump to C23 changes semantics (`bool`/`true`/`false` keywords, old-style declarations removed). Fix: add `-std=gnu11` to `COMMON_CFLAGS`, `TEST_CFLAGS`, `FUZZ_CFLAGS`, `SMOKE_CFLAGS`, `STRESS_CFLAGS_*`, `COV_CFLAGS`, `BENCH_CFLAGS`, `ZIMG_H_CFLAGS` (gnu not c: `_GNU_SOURCE`, `sysinfo`, semaphores in use). |
| BUILD-10 | open | S | Makefile/CI nits: (a) `stress` is absent from `.PHONY`; (b) the `build-bench:` rule splits its surrounding comment sentence (`Makefile:511-520`); (c) the `fuzz` echo list omits the tile-grid fuzzer it builds; (d) CI says local builds default to x86-64-v4 although `MARCH ?= native` (`ci.yml:14-20`, `Makefile:63`). | Four localized documentation/target-list fixes. |
| BUILD-11 | open | S | Bare `make` builds only `build/usm_pool.o`, not the plugin: the conditional USM object rule is the first target in the file (`Makefile:135-151`), before `all` at line 158. `make -pn` confirms `.DEFAULT_GOAL := build/usm_pool.o`, contradicting README's primary build command. | Set `.DEFAULT_GOAL := all` before any conditional object rule, or move `all` first; add a dry-run/default-goal check. |
| BUILD-12 | open | M | Build configuration is not part of object freshness. Existing objects do not depend on `CC`, `MARCH`, `MULTIVERSION`, feature detection, or effective flags; after one build, changing `MARCH=x86-64` to `x86-64-v3` reports nothing to rebuild. A documented “portable” build can therefore silently retain native instructions. | Use configuration-keyed build directories or a generated command/flag stamp that every affected object depends on. Include `HAVE_ZIMG`/pkg-config changes as well as compiler and ISA flags. |
| BUILD-14 | open | S | The required static-analysis target is currently red: `make analyze` exits 2 because cppcheck reports `tests/test_usm_pool.c:249` `variableScope`. CI invokes this same target. | Move the static buffer into its actual use scope or add a narrowly justified suppression; rerun the full analyzer gate. |
| BUILD-15 | open | M | Non-plugin test/fuzz/stress/coverage/bench targets lack generated dependency files and several manual prerequisites omit included headers. For example `usm.h` includes `zimg_helpers.h`, but touching it leaves `build/test_usm` up to date; `threading.h` likewise does not rebuild stress/pool binaries (`Makefile:79-86,226-594,679-720`). | Generate/include depfiles for every compiled target instead of maintaining incomplete transitive header lists by hand. The historical depfile fix covered plugin objects only. |

## observability

| id | status | effort | description | notes |
|----|--------|--------|-------------|-------|
| OBS-6 | open | S | `process_fail_logged` is filter-global and stays latched after a successful zimg-to-swscale replacement (`autoupscale.c:892-918,946-956`), so a later swscale processing failure drops frames without its own warning. | Reset the guard after successful fallback or track the last failed backend identity. |
| OBS-7 | open | S | The zimg engagement log reports tile-destination scratch as zero: `log_zimg_open` forces `dst_mb=0` whenever `col_tiled`, although every tiled worker owns allocated destination planes (`scaler_zimg.c:669-676,740-741,888-907`). | Sum the per-worker tile plane allocations so the logged scratch footprint matches actual memory use. |
| OBS-8 | open | S | Drop statistics are success-driven and incomplete. `filter_NewPicture` failure is not counted, while backend-null/process failures increment `dropped_count` but return before `MaybeLogStats`; a drop-only failure streak therefore never logs/refreshes the summary (`autoupscale.c:839-877,921-960`). | Centralize a drop-record path that increments the counter and services the periodic log/export without requiring a successful frame. |
| — | | | OBS-4 remains obsolete because copy-out is already parallel and included in total frame timing. | |

## wiring gaps

| id | status | effort | description | notes |
|----|--------|--------|-------------|-------|
| WIRE-5 | open | S | Production clamping defeats the pure helpers' tested/documented invalid-enum fallback: `target=999` becomes explicit 8K rather than AUTO, and `backend=999` becomes forced swscale rather than AUTO (`autoupscale.c:558-566`, `upscale_logic.h:123-125`, `scaler_pick_logic.h:85-96`, `HOW_IT_WORKS.md:181-185`). | Normalize invalid target/backend enums to their AUTO sentinel. Keep endpoint clamping only for numeric quantities. This makes the production dispatcher exercise the forward-compatible behavior its tests assert. |
| — | | | WIRE-1 (flat-skip benchmark wiring) and WIRE-2 (single-threaded oracle documentation) remain resolved. | |

## unused functions/methods

| id | status | effort | description | notes |
|----|--------|--------|-------------|-------|
| DEAD-1,2,3,5 | keep | S | `up_usm_workspace_size`, `up_usm__pass1_hblur`, `up_usm__pass2_combine`, `up_usm__args_valid` (usm.h) are reachable only via `up_usm_apply_plane` | DECISION: keep. They are the single-threaded byte-identity TEST ORACLE for the threaded pool (documented at up_usm_apply_plane via WIRE-2, commit 0f92daa). Not dead — intentionally test-only. |
| DEAD-8 | open | S | Two write-only fields on `stripe_worker_t` (`scaler_zimg.c`): `src_x_start` (declared `:179`, written `:714`, never read — its comment "used by active_region" is FALSE: the active-region crop consumes the `src_x_start` PARAMETER at graph-build time in `build_worker_graph_and_tmp`, not the field) and `worker_id` (declared `:181`, written `:734`, never read — the pin call `:759` uses the parameter). | Verified by grep: no read sites for either field. Delete both fields and the misleading comment; net −4 lines, removes a false data-flow claim from the worker struct. |
| DEAD-9 | open | S | `sws_priv_t.av_fmt` is assigned in `sws_open` but never read (`scaler_swscale.c:22-26,69-84`). | Delete the field and assignment; the local `fmt` already supplies the only use. |
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
| VAL-1 | Real-VLC validation of source zero-copy now default-ON (commit 2236264). Run actual VLC across I420/YV12/I422/I444 sub-720p sources + the threads/zerocopy options; confirm no crash, no garbled output. Also exercise the 2026-07-10 runtime paths the harness cannot reach: SYS-2 swscale fallback (force a zimg first-frame failure, e.g. ulimit -v, and confirm playback continues on swscale), SYS-5 (`--autoupscale-zerocopy-src=0` on a wide/short source logs the rows-only warn), SYS-6 (USM pool failure warns once), and an odd-crop VP9/AV1 source engaging via the REL-4 even-align. | The harness proves byte-identity on malloc'd pictures but CANNOT reproduce the documented VLC-pool segfault history. The pre-flight guard (zimg_pic_ok) catches null/bad-pitch geometry, not deeper pool/lifecycle issues. If it misbehaves on a real VLC build, revert the default with `--autoupscale-zerocopy-src=0` (one-line flag flip) pending a fix. Dst zero-copy (also default-ON) shares the same caveat but has shipped longer. |

## Audit picks deliberately rejected

Kept here so future passes don't re-pick them.

| id | why rejected |
|----|--------------|
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

One new finding recorded (DG-3); these candidates were traced and rejected:

| candidate | why rejected |
|----|--------------|
| `scaler_zimg.c:494-498` `worker_main` "copies out uninitialized tile/dst scratch to VLC dst when `zimg_filter_graph_process` fails" | Copying indeterminate bytes via `uint8_t` memcpy is not UB, and the frame is unconditionally dropped: `zimg_dispatch_and_wait` returns -1 on any worker `result != 0`, so `Filter()` releases `p_out` — the garbage never leaves the plugin. |
| `usm_pool.c` / `scaler_zimg.c` unchecked `sem_wait` EINTR (UAF/null-deref via broken frame barrier) | Real, but already tracked as CON-3 — found independently this pass, verbatim the same three sites; not re-filed. |
| odd-height 4:2:0 source → zimg graph-build failure → sticky drop-every-frame | SUPERSEDED same day by REL-4 (filed open): the rejection's "conformant streams can't carry odd crops" premise holds only for H.264/HEVC 4:2:0 — VP9/AV1 permit odd frame dims with 4:2:0, and raw/container-cropped sources reach the filter too. See REL-4 for the fix. |
| BUILD-9 candidate: "stale `usm_pool.o`/`usm_pool_dispatch.o` committed to git at repo root" | Wrong: `git ls-files` does NOT list them and `.gitignore:4` (`*.o`, commit 1f7da23) covers them — they were untracked local leftovers, deleted from disk 2026-07-10. Nothing to fix in-tree. |
| `tests/test_zimg_helpers.c:244-245` unchecked `malloc` before writes (null-deref UB under OOM, test code) | Test-only, ~430 KB allocations, immediately crash-visible under the sanitizer harness that runs these tests; no product-code surface. Not worth a row. |

Coverage note: pure-logic files are 100% (gated). scaler_zimg.c is exercised
to ~94% by `make coverage-zimg` (was 0%); the rest needs a live VLC logger
(log_zimg_open) or fault injection and is intentionally ungated.
