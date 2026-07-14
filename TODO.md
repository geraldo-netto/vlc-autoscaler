# TODO — full-project audit findings

Full rescan of HEAD `d754be4` on 2026-07-14 (6 parallel category audits, every src/ file read in
full, tests/ + Makefile + CI + sonar config + scripts/ + docs/ inspected; findings verified against
actual code paths, several with measurements on a 32-core box). IDs restart at this rescan — they do
not correlate with pre-rescan IDs in git history. Row format: `id | status | effort | description | notes`.

## security

No open findings. Re-verified: config ints saturated (`InheritIntSat`) + clamped (`ClampConfig`);
every frame-geometry entry point (zimg, swscale, probe, USM) is behind `up_picture_view_init`, which
bounds each plane against the *allocated* `i_pitch`/`i_lines` (not declared `i_visible_*`), so a lying
`video_format_t` cannot form an OOB pointer; allocation-size arithmetic overflow-checked at every seam;
`>> sub_w/sub_h` shifts clamped `< 8`; all format strings literal; `up_cli_parse_long` checks
`errno`/endptr/range.

## undefined behavior

| id | status | effort | description | notes |
|---|---|---|---|---|

No open findings. Re-verified this pass: the in-place USM halo protocol traced row-by-row (no worker
ever reads a row another worker wrote, at either stripe edge); `aligned_alloc` size proven a non-zero
multiple of the alignment at all five sites; all four zimg graph dims and `active_region.left/width`
provably even on every subsampled axis; `up_copy_plane`'s contiguous fast path proven unreachable for
column tiles; `atomic_fetch_sub` driving `pending` negative during a poisoned drain is well-defined.

## memory management

| id | status | effort | description | notes |
|---|---|---|---|---|

No open findings. Re-verified: `release_worker_resources` idempotent and `thread_started` cleared
before the second `zimg_close` pass (no double join/free, including barrier-failure →
`TryBackendFallback` → `zimg_close`); the failed `try_spawn_one_worker` slot sits outside
`teardown_constructed_workers`'s range but is already all-NULL; gate init partial failure unwinds
under `cv_inited`/`sem_inited`; both barrier-failure paths join before `picture_Release`.
(Retention-after-permanent-failure is real but tracked under *resource management*: RES-1, RES-2.)

## performance

| id | status | effort | description | notes |
|---|---|---|---|---|
| PERF-1 | open | S | No `n_threads == 1` fast path in either pool (`src/usm_pool.c:502-512`, `src/scaler_zimg.c:1141-1150`, gate `src/threading.h:320-373`): with a single worker the frame still arms the barrier, broadcasts, and blocks in `sem_wait` for a full thread round-trip. | Measured gate cost 6.79 µs/dispatch at N=1 (essentially N-independent, 6.2-8.5 µs for N=1..8) — ~13-14 µs/frame of pure sync across the two pools for zero parallelism, plus 2 needlessly spawned threads per filter instance. `up_threads_decide(AUTO, cores)` returns 1 for every core count ≤ 7, i.e. the whole mainstream desktop. Fix: run the single worker's fn inline on the main thread and skip the spawn. |
| PERF-2 | open | S | USM pool's only work cap is `height / USM_STRIPE_MIN_ROWS(8)` = 135 workers at 1080p (`src/usm_pool.c:88-90,431-433`), so it takes whatever `up_threads_decide` gives; the pass is memory-bandwidth-bound and stops scaling ~4× earlier. | Measured 1080p in-place USM: N=1 322 µs, N=8 58.6, **N=10 51.1 (knee)**, N=14 59.3, N=30 76.5. 4K plateaus at N=8..12. Auto picks 14 on a 32-core box (+16% vs optimum) and 30 on a 64-core box (+50%) while spawning 3× the threads. Fix: raise the effective USM stripe floor (~96-128 rows) or cap USM threads at ~12. |

Otherwise clean: zero steady-state per-frame allocations; no per-frame graph rebuilds; default aligned
row-stripe path copies no planes; hot USM TU is `-O3`; false sharing padded in both pools. Correction
to the previous pass: there are **no** alignment hints anywhere in `src/` (GCC emits `vmovdqu`), but
the measured cost is nil (0.217/0.218/0.219 ms at scratch offsets 0/16/32) — not a finding.

## scalability

| id | status | effort | description | notes |
|---|---|---|---|---|
| SCAL-1 | open | M | `up_detect_cpu_topology_with` (`src/threading.h:127-171`) derives the CPU budget from `sched_getaffinity` + `sysconf`, which see a **cpuset** but never a cgroup CPU **quota** (v2 `cpu.max`, v1 `cpu.cfs_quota_us`). | Docker `--cpus=2` / K8s `limits.cpu: 2` on a 64-core node: affinity still reports 64, auto picks 30, and the plugin spawns 30 zimg + 30 USM workers against 2 CPUs of quota. The per-frame barrier is `max()` over 30 threads timesliced through 2 CPUs and CFS's 100 ms throttle lands mid-frame — frame latency inflates ~15× with burst stalls. Fix: read `cpu.max`/`cfs_quota` and clamp `allowed_count`. |
| SCAL-2 | open | M | `--autoupscale-pin-threads=1` pins worker `i` to `pin_ids[i % pin_count]`, the first N set bits of the affinity mask, with no SMT/NUMA awareness (`src/scaler_zimg.c:117-130,792-797`, `src/threading.h:96-114`). | On any host whose CPU enumeration interleaves SMT siblings (`--cpuset-cpus=0,1,2,3` = 2 physical cores × 2 threads; several AMD/BIOS enumerations), the first N IDs stack 2 workers per physical core while sibling-free cores idle — up to 2× zimg stripe latency, so the option halves the throughput it advertises. Compounding: the USM pool is never pinned and floats onto the cores the zimg workers just claimed. Fix: consult `topology/thread_siblings_list`, prefer one worker per physical core. |

Otherwise clean: stripe/tile partitions balanced to ±1-2 rows and even-aligned; grid search one-time
O(64); `UP_THREADS_MAX=64` cap; per-resolution memory bounded; lazy init keeps speculative chain probes
at ~zero cost; no O(n²) patterns.

## concurrency

| id | status | effort | description | notes |
|---|---|---|---|---|
| CONC-1 | open | S | `up_pool_gate_worker_done` (`src/threading.h:358-363`): if the last finisher's `sem_post` fails it is not retried and **no** post reaches the semaphore, so `up_pool_gate_wait_all`'s `sem_wait` (`:369`) blocks forever on VLC's video thread — the `post_failed` flag it sets can only be read after a wait that never returns. | Not reachable today (the comment's EOVERFLOW-only claim holds: count at `SEM_VALUE_MAX` ⇒ the wait cannot block), but the "recovery" is a deadlock, not a recovery. A future sem re-init path or a libc returning another errno turns it into a hard playback hang with no diagnostic. Cheap hardening: retry the post in the finisher, or use `sem_timedwait`. Related: ERR-1. |

Otherwise sound (full re-trace of every atomic order, both lifecycles, both drain paths): gate wake
publication ordered by the go-lock; acq_rel `fetch_sub` + release sequence + sem barrier publishes
worker writes; spawn-before-partition ordered by the mutex; both drain paths join before
`picture_Release`; partial-spawn teardown joins only started threads; the two pools share no mutable
state; perfmon is main-thread only.

## code complexity

| id | status | effort | description | notes |
|---|---|---|---|---|

Lizard 1.23.0 over `src` + `tests` at `-C 10`: exit 0, zero warnings. Highest is `ChromaToAVFmt`
(CCN 10, exempt flat switch). Every production function is at ≤7 params.

## code duplication

| id | status | effort | description | notes |
|---|---|---|---|---|
| DUP-1 | open | M | `zimg_wake_all_for_exit`/`zimg_stop_workers` (`src/scaler_zimg.c:836-858`) and `usm_pool_wake_all_for_exit`/`usm_pool_stop_workers` (`src/usm_pool.c:567-588`) are line-for-line the same protocol: gate-ready check → per-worker `should_exit` under lock → broadcast → join loop over `thread_started`. | The earlier pass factored out only the gate primitives; the exit/join half stayed two copies, so a protocol fix must land twice. Fix: hoist `should_exit` into `up_pool_gate_t`, add `up_pool_gate_request_exit()`; `up_pool_gate_wait_for_go` then drops its `should_exit` param and both wake-for-exit functions disappear. |
| DUP-3 | open | S | `tests/bench_usm_pool.c:22-31` — `xs32`/`fill_xs` reimplement `tests/prng.h`'s `up_xs32`/`up_fill_random` byte-for-byte (same 13/17/5 stream) without including `prng.h`. | Missed by the previous duplication pass, which only tracked the two content-probe files. A bench has no stream-sensitive assertions, so the DUP-5 blocker does not apply — adoptable now. |
| DUP-4 | open | S | `tests/test_usm_pool_variants.c:49` and `tests/fuzz_usm_variants.c:101` — two further private LCG buffer fills (`fill_pattern`, `fill_source`) instead of `prng.h`. | Both feed cross-variant byte-identity comparisons, so the stream is irrelevant to the assertions: no risk, unlike DUP-5. |
| DUP-5 | open | S | Two xorshift32 buffer fills remain inline in `tests/fuzz_content_probe.c:61` and `tests/test_content_probe.c:381` (same 13/17/5 stream now in `tests/prng.h`). | Carried over from the previous pass: they feed content-probe *metric* assertions, so a stream change could shift expected values. Adopt `prng.h` only after confirming those assertions are stream-agnostic. |

## architecture/modularity/SOLID

| id | status | effort | description | notes |
|---|---|---|---|---|
| ARCH-2 | open | L | Each pool independently reimplements the same worker-pool lifecycle — sticky `lazy_init_done/failed`, `pool_broken` poison, aligned worker-array alloc, partial-spawn/teardown, dispatch-and-wait (`src/usm_pool.c:405-411,502-512` vs `src/scaler_zimg.c:976-1006,1141-1177`). | Only the gate was factored out; the surrounding lifecycle is two parallel state machines with identical semantics, so every reliability fix lands twice (see RES-2, ERR-1, CONC-1 — each currently fixed in one pool only). Not the parked SCAL-P3 (that shares *threads at runtime* for perf) — this is the structural duplication beneath it, and doing it first makes SCAL-P3 cheap. |

## decoupling

| id | status | effort | description | notes |
|---|---|---|---|---|

## business/design patterns/DDD

No open findings. `usm_pool_ops_t` (typeof-derived from the `usm_pool_variants.h` X-macro, load-time
bound, unit-driven through all three arms) and the `scaler_backend_t` strategy vtable are already the
right patterns; the new dispatcher adds no pattern debt. No further pattern proposal survives the
no-speculative-pattern-work bar.

## reliability/correctness

| id | status | effort | description | notes |
|---|---|---|---|---|
| REL-3 | open | S | `tests/test_picture_view.c:54` builds its test `plane_t` **from the layout table it is testing** (`i_pixel_pitch = layout->pixel_pitch[i]`), so the NV12 assertion at `:142` is circular and cannot catch upstream VLC drift. | The table is correct against today's `vlc_fourcc_GetChromaDescription()` (verified), and `tests/abi_assert.c` checks field names/types, not these *values*. If VLC's `pixel_size` for a chroma ever changed, every frame of that chroma would silently fail `up_picture_plane_storage_ok` (all frames dropped, TRANSIENT). Fix: `_Static_assert` the table's `pixel_pitch[]` against `vlc_fourcc_GetChromaDescription(c)->pixel_size` in the VLC-linked TU, beside the existing fourcc asserts at `autoupscale.c:48-53`. |
| SH-1 | open | S | `scripts/install-vlc-autoupscale-action.sh:43,59` — the generated `Exec=` lines interpolate `${WRAPPER}` (a `$HOME`-derived path) unquoted, but Desktop-Entry/Nemo-Action `Exec` is parsed with shell-like word splitting. | `HOME=/home/j smith` → `Exec=/home/j smith/.local/bin/vlc-autoupscale %U`; the launcher execs `/home/j` and both the *Open With* entry and the right-click action silently do nothing. Same for `$`, backtick or backslash in the path. Fix: `Exec="${WRAPPER}" %U` / `%F`. |
| SH-2 | open | S | `scripts/vlc-autoupscale.sh:13` — the intentional word-split of `${VLC_AUTOUPSCALE_ARGS:-$DEFAULT_ARGS}` is also exposed to pathname expansion; `set -f` is never set and the SC2086 suppression only acknowledges the splitting. | `VLC_AUTOUPSCALE_ARGS='… --sub-file=*.srt'` run in a directory with two `.srt` files: the glob expands to two words, so VLC gets a bogus trailing input. Add `set -f` before the `exec`. |
| SH-3 | open | S | `scripts/vlc-autoupscale.sh:13` uses `:-`, which treats an **empty** `VLC_AUTOUPSCALE_ARGS` as unset. | `VLC_AUTOUPSCALE_ARGS= vlc-autoupscale clip.mkv` — the natural way to say "this wrapper, no filter tuning", per the file's own header — silently re-injects the whole default flag chain. Use `${VLC_AUTOUPSCALE_ARGS-$DEFAULT_ARGS}`. |

## portability/standards conformance

No open findings. Verified against the declared target (Linux x86-64, `-std=c11` pinned for every
build flavour): no char-signedness dependence; shift exponents bounded; negative right-shift avoided;
`mem_mb` widened to `uint64_t` and saturated for 32-bit hosts; every compiler extension in use
(`__typeof__`, `constructor`, `__builtin_cpu_*`, diagnostic pragmas) is GCC/Clang-x86-gated with
`usm_pool_dispatch.c:54` `#error`-ing off x86-64; `WORDS_BIGENDIAN` fallback pinned by `_Static_assert`s.

## error handling

| id | status | effort | description | notes |
|---|---|---|---|---|
| ERR-1 | open | M | `pthread_cond_broadcast`'s and `pthread_mutex_lock`'s return values are ignored throughout the gate (`src/threading.h:313,326-330,343,369`), and the only recovery path is an **untimed** `sem_wait` — so a failed broadcast is unrecoverable by construction. | If `pthread_cond_broadcast` ever returns non-zero (EINVAL on a corrupted cond), no worker wakes, `pending` never reaches 0, nobody posts `all_done`, and `up_pool_gate_wait_all` blocks forever on VLC's video-output thread: playback hangs instead of dropping a frame and falling back. The test harness proves the gap — `tests/barrier_fault_inject.h:46-59` must pair its suppressed broadcast with an *injected `sem_wait` failure*, or the suite would hang exactly like production. Same root as CONC-1. |
| ERR-2 | open | S | `worker_emit_output()` runs unconditionally after `zimg_filter_graph_process()` fails (`src/scaler_zimg.c:508-513`); the failure is only inspected later, on the main thread. | Column-tiled grid (`n_cols > 1`): `w->tile_dst` is `aligned_alloc`'d and never zeroed, so a first-frame graph failure still `memcpy`s a tile-sized block of **indeterminate heap** into VLC's destination picture (a mid-graph failure splatters a half-resampled tile). The frame is dropped afterwards so it never reaches the screen, but it is a frame-sized read of uninitialized memory plus a wasted write. Gate on `w->result == 0`. |
| SH-4 | open | S | `scripts/install-vlc-autoupscale-action.sh:10-16,23-27` — `set -eu` catches an *unset* `HOME` but not an *empty* one, so every derived path collapses to root-relative `/.local/...` and uninstall reports success having removed nothing. | `HOME= …/install-vlc-autoupscale-action.sh --uninstall` → `rm -f` on three nonexistent root-relative paths, exits 0, prints "Removed …" while the real files under the user's home are untouched. Guard with `: "${HOME:?}"`. |

Otherwise clean: `pthread_create`, `aligned_alloc`, `clock_gettime`, `filter_NewPicture`, `zimg_*`,
`sem_init`, `sched_getaffinity`, `sws_getContext` returns all checked; `sem_wait` EINTR-retried; sticky
lazy-init/`pool_broken`/`fallback_tried` states propagate honest TRANSIENT-vs-FATAL codes.

## resource management

| id | status | effort | description | notes |
|---|---|---|---|---|
| RES-1 | open | S | The USM pool is never torn down when USM is **permanently** disabled — neither on the grainy-source skip (`src/autoupscale.c:915-919`) nor on the pool-failure path (`:980-1003`) — so its threads and scratch stay resident for the rest of playback with zero possible use. | Grainy 1080p source: USM runs from frame 1, so the pool lazily spawns up to 64 threads and allocates `5·dst_w·N` bytes (~10 MB at 4K/64). At frame 60 the probe window closes, `up_should_skip_usm_for_sharpness` trips, and `ApplyUsmIfEnabled` returns at its first line for every later frame — while the parked threads and scratch live until `Close()`. Same for the `usm_amount_q8 = 0` disable at `:1001`. |
| RES-2 | open | S | The zimg worker-failure poison path sets `pool_broken` and returns FATAL **without** stopping/joining the workers (`src/scaler_zimg.c:1165-1175`) — unlike the barrier-failure path ten lines above, which calls `zimg_stop_workers()`. | `--autoupscale-backend=1` + a fatal graph failure: `TryBackendFallback` (`autoupscale.c:1079-1088`) logs and returns *without* closing the backend, so up to 64 worker threads, 64 zimg graphs, 64 tmp buffers and (column grid) 64 tile scratch buffers stay parked on the gate for the rest of playback while `zimg_process` short-circuits every frame to FATAL. Retention, not a leak — but exactly the unbounded-hold-after-permanent-failure this category exists for. |

## API/ABI stability

| id | status | effort | description | notes |
|---|---|---|---|---|
| ABI-1 | open | S | `make check-visibility` (`Makefile:269-276`) asserts only that no *extra* symbols are exported; it never asserts the `vlc_entry*` symbols **exist**. | Empirically confirmed: a `.so` with **zero** `vlc_entry` exports passes verbatim — `nm -D --defined-only` lists nothing, `grep -v '^vlc_entry'` yields an empty `bad`, and the target prints "check-visibility OK: only vlc_entry* exported". A visibility regression would make VLC's loader silently skip the plugin (never appears in `vlc --list`) with CI green. |
| ABI-2 | open | S | `PLUGIN_LDFLAGS := -shared -flto $(EXTRA_LDFLAGS)` (`Makefile:102,260`) carries neither `$(WARN)` nor `$(EXTRA_CFLAGS)`, so CI's `-Werror` stops at the compile step and **every LTO link-time diagnostic is non-fatal**. | Reproduced on gcc 13.3: two TUs with a mismatched parameter type both compile clean under `-Werror`; `gcc -shared -flto` then prints `warning: type of 'pool_process' does not match original declaration [-Wlto-type-mismatch]` and **exits 0**. `ci.yml:63-66` claims the MULTIVERSION link gates "prototype/symbol drift" — true only for *unresolved* symbols. Same hole swallows LTO-only `-Wstringop-overflow`/`-Warray-bounds` from cross-TU inlining of the USM kernels. |
| ABI-3 | open | S | `check-multiversion-isa` (`Makefile:209-228`) inspects objects **it compiles itself** (`-O3 -march=… -c src/usm_pool.c`, no `-flto`, not `$(PLUGIN_CFLAGS)`); the shipped LTO-linked `.so` is never checked for ymm/zmm. `make check-simd` (`Makefile:217`) likewise builds without `-flto`. | Gate gap, not a live bug: gcc 13.3 *does* preserve per-TU `-march` through LTO (verified by disassembling the linked `.so`: xmm-only / 165 ymm / 208 ymm + 152 zmm). But a clang-LTO build or a compiler regression that collapses the avx512 variant to baseline would still print `[ok] avx512: AVX-512` (fresh objects), and the byte-identical-output variant test passes *by design* — an SSE2 kernel ships while `LogEngaged` reports `simd=avx512` (2-4× on the hottest kernel). One-line fix: `objdump -d $(BUILD)/$(PLUGIN).so \| grep -c zmm` after the MULTIVERSION=1 build. |

## build/toolchain hygiene

| id | status | effort | description | notes |
|---|---|---|---|---|
| BUILD-1 | open | S | `make check-visibility` runs after the gcc build (`ci.yml:45`) and the MULTIVERSION build (`:71`) but **not** for the clang plugin step (`:51-56`, which builds then `make clean`s). | clang-specific symbol-visibility/dispatch link regressions slip through. Add a `check-visibility` to the clang job. (Note ABI-1: the check itself is also too weak.) |
| BUILD-2 | open | S | `Makefile:817` defines a `scan-build` target (clang static analyzer, `--status-bugs`) that no CI job invokes, so the clang-analyzer path can rot silently. | May be a deliberate cost tradeoff — flagged as a coverage gap. Wire into CI or note as intentionally manual. |
| BUILD-3 | open | M | `sonar-project.properties:32` — the new `sonar.coverage.exclusions=src/autoupscale.c` removes the **last** gate on that file: it is already excluded from cppcheck (`Makefile:875-877`), absent from the local 90% gate's `TRACKED` list (`scripts/coverage_report.sh:18-34`), has no unit test, and `scan-build` isn't in CI (BUILD-2). | A new branch in `TryBackendFallback`/`Filter` (`autoupscale.c:1070-1168` — a stateful backend-swap + 4-way drop machine that `picture_Release`s on each arm, *not* "glue") is measured by nothing; a NULL-backend or double-release bug ships green. The properties comment ("exercising the glue needs a full VLC runtime mock") overstates it: `tests/stubs/vlc_common.h` + `vlc_picture.h` already fake `picture_t`/`plane_t`/`msg_*` well enough to unit-test `scaler_swscale.c`; adding `filter_t`, `var_Inherit/Create/SetInteger` and `filter_NewPicture` is an incremental extension. |
| BUILD-4 | open | S | The sonarcloud job never blocks (`ci.yml:248-253`): no `sonar.qualitygate.wait=true`, no `args:`, no gate step. | Combined with BUILD-3, Sonar is now the only analyzer that sees `autoupscale.c` — **and its verdict cannot fail the build**. Sonar raises a blocker, CI is green, the PR merges. |
| BUILD-5 | open | S | `TRACKED` (`scripts/coverage_report.sh:18-34`, `scripts/coverage_per_function.sh:25-28`) omits `usm_pool_dispatch.c`, `plane_utils.h` and `cpu_level.h` — all production, all shipped — while it *does* gate `cli_parse.h`, a `tests/`-only helper. | Commit `d01ef2a` added the dispatch test to `COV_TESTS` (`Makefile:753`) "so Sonar sees its coverage" but never added the file to either gate. `usm_pool_select_ops` — the table deciding which SIMD kernel every frame runs in the shipped MULTIVERSION=1 build — can regress to 0% coverage with `make coverage` still exiting 0. Same for `plane_utils.h`, which holds the overflow-checked allocation-size arithmetic. |
| BUILD-6 | open | S | `--gcov-ignore-parse-errors=negative_hits.warn_once_per_file` (`ci.yml:240`) makes gcovr clamp gcc's bogus negative counts to **0**, i.e. an *executed* line in `usm_pool.c`/`scaler_zimg.c` is reported to Sonar as **uncovered**. | Direction is safe but non-deterministic (GCC 68080 fires only on certain interleavings): an unchanged PR can flip Sonar's coverage-on-new-code red, or a real drop gets dismissed as "the gcov bug again". Worse, the two gates now disagree in *opposite* directions on the same lines — `scripts/coverage_report.sh:68-75` treats any count that isn't `-`/`#####`/`=====` as **covered**, so a negative count reads as covered locally and uncovered in Sonar. The warning goes to the job log and is never asserted on. |
| BUILD-7 | open | S | build-wrapper only captures `make plugin MULTIVERSION=1` (`ci.yml:228`, `sonar-project.properties:35`), so Sonar's cfamily analysis only ever sees the `#ifdef USM_VARIANT` arm of `usm_pool.c`/`usm_pool.h`. | The **default shipped** build is `MULTIVERSION=0` (`Makefile:146`), whose arm (`src/usm_pool.c:85`, all un-suffixed symbol definitions) is never Sonar-analysed: a Sonar-detectable defect confined to it ships in the configuration nearly every user builds. cppcheck covers that arm — partial mitigation, narrower ruleset. |
| BUILD-8 | open | S | `ci.yml:100` names the step "Per-file and per-function coverage gates (minimum 80%)" while the recipe passes `THRESHOLD=90` (`Makefile:858-859`). | A contributor relaxes a file to 85% believing it is in-bounds; the job fails on a number the step name contradicts. |

## observability

| id | status | effort | description | notes |
|---|---|---|---|---|
| OBS-1 | open | S | `MaybeLogStats` — and with it the **entire** stats-var export, including `autoupscale-dropped` — is reachable only from `RecordPerf` (`src/autoupscale.c:1055`), which runs only on the success path (`:1163`). All three drop paths (`:1117`, `:1135`, `:1156`) do `dropped_count++` then `return NULL` early. | `--autoupscale-backend=1` + a zimg lazy-init failure: `TryBackendFallback` logs once and leaves `scaler.backend == NULL`, so *every* later frame takes the drop path — `dropped_count` climbs into the thousands while `MaybeLogStats` never runs again. The periodic `frames=… dropped=…` line stops and `var_SetInteger("autoupscale-dropped", …)` is never called: an embedder polling it (README:190) reads **0** while 100% of frames drop. The counter is blind in exactly the scenario it exists for. |
| OBS-2 | open | S | Every frame-dropping diagnostic sits at a suppressed log level: pool exhaustion `msg_Warn` (`autoupscale.c:1131`), backend process failure (`:1149`), USM pool death (`:998`), runtime swscale fallback (`:1104`), zimg alignment drift (`scaler_zimg.c:1270,1288`) — and the only place `dropped=` is printed is `msg_Dbg` (`:1027`). | The code itself establishes at `:842-845` that "VLC 3.x's default verbosity suppresses level-2 warnings" and promotes the *perf advisory* to `msg_Info` for that reason. Net effect: on a default `vlc file.mkv` a user whose playback is stuttering from pool exhaustion sees **nothing**, while the one cosmetic tuning hint (which they can silence) is the one thing that prints. All these latches are already one-shot, so promoting them to `msg_Info` carries no spam risk. |
| OBS-3 | open | S | `--autoupscale-target-fps=0` is documented (`autoupscale.c:879`) as the way to "silence this warning", but `up_perfmon_init` maps `target_fps <= 0` → `enabled = 0`, which makes `up_perfmon_record_ns` return immediately and `up_perfmon_ewma_us` return a hard `0` (`src/perfmon.h:64-78,91-93,126-131`). | An operator who silences the advisory then polls `autoupscale-ewma-us` (README:188) or reads the periodic `ewma=…µs` field gets a constant `0` — indistinguishable from "frames take 0 µs" — and nothing logs that telemetry was disabled. The kill switch for one *advisory* silently kills the whole *perf telemetry surface*. perfmon already separates tracking from warning (`has_warned` latch), so keeping the EWMA live at `fps=0` is a two-line change. |

No log-spam risk on per-frame paths: perf advisory, process-fail, alignment-drift, bad-geometry, probe
verdict and USM-pool-failure messages are all one-shot latched; periodic stats are `msg_Dbg` on a 5 s
tick; engagement/backend/grid selection is logged once at Open (`log_zimg_open` now also reports
`graph-tmp MB`).

## wiring gaps

| id | status | effort | description | notes |
|---|---|---|---|---|
| WIRE-1 | open | S | The whole desktop-integration feature (`scripts/vlc-autoupscale.sh`, `scripts/install-vlc-autoupscale-action.sh`, `docs/CINNAMON-DESKTOP-ACTIONS.md`) has **zero** inbound references anywhere in the repo, so a user has no discoverable path to it. | `grep -rn "vlc-autoupscale\.sh\|install-vlc-autoupscale-action\|CINNAMON-DESKTOP-ACTIONS" . --exclude-dir=.git -l` matches only the three files themselves. README.md:514-515 still lists `scripts/` as exactly `bench_matrix.sh` + `coverage_report.sh`; README.md:517-519 and the docs index at `docs/USAGE.md:732-734` omit the new doc. |
| WIRE-2 | open | S | `scripts/vlc-autoupscale.sh:6` — the script's own `VLC_AUTOUPSCALE_ARGS` example omits `--video-filter=autoupscale`, and since the variable **replaces** the whole default arg string, following the documented example launches VLC with the filter never loaded (every `--autoupscale-*` flag becomes a no-op). | `docs/CINNAMON-DESKTOP-ACTIONS.md:121` gets it right (keeps `--video-filter=autoupscale`), so the two disagree. Fix the header comment, or make the variable *append* to the defaults. |
| WIRE-3 | open | S | `scripts/coverage_per_function.sh` is wired into `make coverage` (`Makefile:859`) but missing from README's `scripts/` listing (README.md:514). | Pre-existing doc drift, same class as WIRE-1. |

Otherwise clean: all 14 `add_integer_with_range` options traced end-to-end to a concrete consumer
(re-verified this pass); the option names in README/docs match the 14 declared in code exactly, no
extras and none missing; the three exported VLC variables match `k_stats_vars[]`; both backends
reachable through `scaler_pick` with AUTO/open/runtime fallback wired; `src/usm_pool_dispatch.c` is
MULTIVERSION-only by documented design; no env vars anywhere. `USM_POOL_FLAT_SKIP` remains a
bench-target-only feature (`Makefile:631`), correctly excluded from the shipped plugin.

## unused functions/methods

| id | status | effort | description | notes |
|---|---|---|---|---|
| UNUSED-1 | open | S | `src/scaler_zimg.c:97` — local macro `ALIGN_DOWN_2(x)` aliases `UP_ALIGN_DOWN_2` and has zero uses; the file reaches even-align logic only through plane_utils helpers. | `grep -rn '\bALIGN_DOWN_2\b' src/ tests/` matches only the `#define` itself. Delete the line. |
| UNUSED-2 | open | S | `src/picture_view.h:19-23` — `up_picture_plane_view_t` fields `width`/`height`/`row_bytes`/`pixel_pitch` are populated by `up_picture_plane_view_init` but production reads only `.pixels` and `.pitch`; the four extra fields are read only in `tests/`. | Not dead (they're a validation byproduct), but production carries them unused. Add a comment noting they're computed for test/validation assertions only. |
| UNUSED-3 | open | S | The production-dead header set is **9** functions, not the 3 the previous pass documented as test-only oracles. Additions: `up_usm_workspace_size` (`src/usm.h:66`), `up_usm__args_valid`/`up_usm__pass1_hblur`/`up_usm__pass2_combine` (`usm.h:175,230,266` — reachable only from the `up_usm_apply_plane` oracle), `up_block_edge_vertical`/`up_block_edge_horizontal` (`content_probe.h:134,152` — reachable only from the `up_block_edge_strength` oracle). | Verified by grep: nothing in `src/` calls `up_usm_workspace_size` (the pool sizes its own scratch via `usm_pool_scratch_bytes`, `usm_pool.c:329`) — yet TODO's security section previously cited it as a *production* overflow-checked seam. All 9 are `static inline` in headers, so nothing is emitted into the shipped `.so`: a bookkeeping/doc-accuracy gap, not bloat. Fix: extend the in-source test-only-oracle documentation to the full set. |

No other unused functions/macros: every `#define` in `src/` has ≥1 use except `ALIGN_DOWN_2`; every
static function in `src/*.c` has ≥1 caller (`up_usm_pool_dispatch_init` is the ELF constructor); no
unused statics or macros in `tests/`; the `UP_USM_POOL_VARIANT_LIST` X-macro is consumed in both
`src/usm_pool_variants.h:42` and `tests/test_usm_pool_dispatch.c:33`.

## Open — parked

| id | status | effort | description | why not now |
|---|---|---|---|---|
| PERF-P1 | parked | M | `src/usm_pool.c:476-516` — in-place USM does 2×n_threads serial main-thread halo-row memcpys per frame before dispatch (~53-115 KB/frame at 1080p, ~0.5 MB worst case at 4K/64). | Folding snapshots into workers needs an extra ready-barrier — significant complexity for a cost that hasn't shown up in a profile. Measure first. |
| TEST-P1 | parked | S | `up_usm_pool_effective_threads`: the mutant `n_threads`→`n_threads_pref` survives the suite — the fields diverge only under a deterministic partial spawn, and the RLIMIT_NPROC test asserts bounds only. | Killing it needs pthread_create fault-injection infra (new `--wrap`) for a diagnostics-only getter; not worth the infra now. |
| SCAL-P1 | parked | L | Static equal-work stripe partition + full per-frame completion barrier (`usm_pool.c:530-542`, `scaler_zimg.c:1167-1195`) makes frame latency `max` over workers; on hybrid P/E-core CPUs or decoder-shared cores, fast workers idle at the barrier every frame. | Partition math is balanced (±1-2 rows); the fix is dynamic stripe stealing or heterogeneity-aware sizing — a large change against a deliberately simple, verified-correct design. Revisit with profile evidence on hybrid hardware. |
| SCAL-P2 | parked | M | Per-worker zimg graph + tmp buffer (+ per-tile dst scratch) grows memory and graph-build time linearly with thread count, up to 64 graphs (`scaler_zimg.c:732-759`). | Independent graphs are what makes the frame path lock-free; bounded by thread caps and stripe floors. Inherent design cost. |
| SCAL-P3 | parked | L | zimg pool and USM pool are separate persistent pools sized from the same budget (`autoupscale.c:585`, `scaler_zimg.c:774`): up to 2×64 threads per instance that only ever run sequentially within a frame. Auto policy on a 32-core box spawns 14+14 = 28 persistent threads, and the USM pass is a second cache-cold full read+write of the luma with its own broadcast/barrier — ~0.1-0.16 ms (~0.6-1%) at 1080p, ~0.3-0.8 ms (~2-5%) at 4K, plus 2× wake/barrier cycles per frame, per filter instance. | A shared pool (share the gate + worker threads, keep separate run fns) would halve the thread herd even without fusing the passes; full fusion removes one dispatch per frame and keeps stripes L2-warm. Effort M (share pool) to L (fuse). Large restructuring for a cost that hasn't surfaced in a profile — measure first. **Do ARCH-2 first**: it factors out the shared lifecycle and makes this cheap. |
| SCAL-P4 | parked | M | `src/threading.h:326-350` broadcast wake serializes worker startup through one mutex: glibc `pthread_cond_broadcast` requeues waiters one futex-handoff at a time, so at N=64 the last worker starts ~64-128 µs after the first — twice per frame (both pools). | **Revisit trigger now met.** This pass measured per-dispatch gate cost at 6.2-8.5 µs against a 51 µs optimal 1080p USM pass — 12-17% of the pass it gates, at *auto* thread counts, not only explicit high `--autoupscale-threads`. The old "~0.2-0.4% of budget" estimate compared the ramp against the whole-frame budget rather than the pass it serializes. Still parked only because PERF-1/PERF-2 (cheaper, same symptom) should land first and may change the picture. Fix direction: per-worker atomic-generation check outside the lock, or tree wake. |

## Audit picks deliberately rejected

Recorded so future passes don't re-pick them:

- **Unifying `worker_copy_in_stripe`/`worker_copy_out_stripe`/`worker_copy_out_tile`** (`scaler_zimg.c:373-455`) — shapes rhyme but direction and offset math differ; a unified helper needs ~8 params and reads worse.
- **`min_plane_dims` in `tests/fuzz_scaler_seam.c` re-encoding subsample knowledge** from `scaler_zimg_chroma.h` — 8 lines, too small to matter. (The *production*-side duplicate of that knowledge is a real finding: ARCH-1.)
- **First-frame rows-only fallback passing reduced `n_threads` as budget** (`scaler_zimg.c:1255-1256`) — verified it cannot change the resulting grid.
- **`lazy_init_done/failed` as plain bools** — race-free under VLC's documented serial `pf_video_filter` contract; documented in-source. Revisit only if that contract changes.
- **Per-worker 2-boundary-row re-hblur** in the USM pool — deliberate trade (<1% of USM work at 1080p) that buys barrier-free operation.
- **RunProbe fusion beyond PERF-1** / SIMD for probe sweeps — probe is 60-frame bounded and grid-subsampled; not worth kernel complexity.
- **Unifying the stripe-invariant checkers** (`check_stripe`/`check_alignment` vs `check_stripe_step`/`check_stripe_bounds_range`) — the invariants each asserts differ, and merging produces exactly the ~10-param mega-helper the guide warns against.
- **Unifying `run_sse2`/`run_avx2`/`run_avx512`** (`test_usm_pool_variants.c:60`) — each names distinct per-ISA symbols that only exist in that build; unifying needs a code-gen macro, disfavored by the "prefer explicit" stance.
- **64B-padding the per-worker USM scratch blocks** — adjacent workers do share one boundary cache line, but it holds a halo row written once per frame and read twice: ~1-2 coherence misses per frame. Measured nil.
- **Alignment hints on the USM kernels** — the rolling scratch rows are only 64B-aligned when `width % 64 == 0`, and GCC emits `vmovdqu` throughout; measured 1080p fused sweep is 0.217/0.218/0.219 ms at scratch offsets 0/16/32. No cost to recover.
