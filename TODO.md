# TODO

Review findings from a full-project audit (2026-05-30) against the AGENTS.md
review categories. One table per category. Format: `id | status | effort | description | notes`.

2026-06-06 rescan: added two new review categories — **system design** and
**data governance** — and scanned the whole project for them (SYS-1..3, DG-1..2).

2026-07-10 rescan: full project, all categories, 5 parallel reviewers. New:
CON-3, PERF-7, DG-3, REL-4, REL-5, SYS-7, WIRE-3, ARCH-8, ARCH-9, DUP-9,
DEAD-8, PORT-5, BUILD-8, BUILD-10. Cross-confirmed by independent reviewers:
CON-3 (x3), SYS-7 (x2), DG-3 (x2). Lizard CCN gate clean (523 fns, avg 3.4, none >10).

Effort: S (small) / M (medium) / L (large). Remove a row once its fix is
implemented + tested + merged (`git log` is the durable record). Keep deferred
items under "Open — parked" with a why-not-now note; keep rejected audit picks
under "Audit picks deliberately rejected".

## security

| id | status | effort | description | notes |
|----|--------|--------|-------------|-------|

## data governance

| id | status | effort | description | notes |
|----|--------|--------|-------------|-------|
| DG-1 | open | S | `zimg_pic_ok` (`scaler_zimg.c:1113-1126`) validates the WIDTH axis only — `i_planes>=3`, non-null plane pointers, `i_pitch >= w/cw` — but never the picture's ROW count (`i_lines`/`i_visible_lines`). In zerocopy-src mode workers read VLC's source picture directly up to the Open-time `p->src_h` (derived from `fmt_in`, not from the incoming `src`). The pre-flight deliberately distrusts pitch but silently trusts height. | Defense-in-depth gap, not a live exploit: under VLC's format contract the decoder honors negotiated `fmt_in` and a real resolution change restarts the filter, so a short luma plane shouldn't occur. But a malformed/drifted pool picture below `src_h` would drive an OOB read — exactly the failure class the pitch guard exists to prevent. Asymmetric with `RunProbe` (line ~703), which derives height from the picture itself. Direction: also require `pic->p[iy].i_visible_lines >= p->src_h` (and chroma `>> sub_h`) in `zimg_pic_ok`. |
| DG-2 | no-action | S | Plugin-owned scratch holding decoded frame pixels (`p->src`/`p->dst`/per-tile dst via `aligned_alloc`, `scaler_zimg.c:611-613`; USM scratch `usm_pool.c:521`) is `free()`d without zeroing on close (`zimg_close` ~1209), so the last frame's content lingers in freed heap until reuse. | DECIDED no-action (2026-06-06): matches VLC's own picture pools (no scrub); decoded video is not treated as a secret anywhere in VLC, buffers never leave the process, and no log/error path ever emits buffer contents or addresses. Scrubbing every freed block on close adds cost for no real threat. Recorded so a future pass doesn't re-raise. |
| DG-3 | open | S | `sws_process` (`scaler_swscale.c:94-123`) has NO picture-geometry pre-flight — the swscale twin of zimg's `zimg_pic_ok` guard. It builds `src_data`/`dst_data` from however many planes the pictures claim (capped at 4 but not floored at the chroma's required count), never checks `p_pixels != NULL` or `i_pitch >=` visible width, and passes Open-time `ctx->src_h` as the row count regardless of the incoming picture. A malformed/drifted picture (`i_planes < 3` for YUV420P → `src_data[1]`/`[2]` stay NULL; pitch < width; fewer rows than `src_h`) drives a null-deref / OOB read-write INSIDE libswscale — the exact failure class DG-1 records for zimg, except swscale checks nothing at all and is the fallback used precisely when zimg is absent (NV12/NV21/RGB always route here). | Defense-in-depth gap under the same trust contract as DG-1, not a live exploit. Asymmetry: zimg drops a malformed frame with a one-shot warn; swscale marches into the library. Direction: add a small pre-flight (planes >= required-for-chroma, non-NULL `p_pixels`, `i_pitch >=` visible width, rows >= `src_h`/`dst_h`) on both pictures in `sws_process`, mirroring `zimg_pic_ok`. Related: the USM apply path shares DG-1's height-trust (validates stride but writes `p->height` rows unconditionally, `usm_pool.c:451-459`) — fold into the DG-1 fix rather than a separate item. 2026-07-10 audit. |

## undefined behavior

| id | status | effort | description | notes |
|----|--------|--------|-------------|-------|
| UB-OVF1 | no-action | S | `autoupscale.c` `MaybeLogStats` computes `next_stats_ns = now_ns + OBS_STATS_INTERVAL_NS`; signed-overflow UB once `now_ns > INT64_MAX - 5e9` | ACCEPTED (theoretical, like PORT-3): `now_ns` is `CLOCK_MONOTONIC` nanoseconds, so reaching 2^63 ns needs ~292 years of uptime — unreachable. A saturating add would add a per-log branch for a case that cannot occur on a monotonic clock. Revisit only if the timestamp source ever changes to something that can approach INT64_MAX. |

## memory management

| id | status | effort | description | notes |
|----|--------|--------|-------------|-------|
| (none found) | | | All `aligned_alloc` seams verified C11-conformant: sizes are multiples of alignment by construction — `up_round_up_pitch` rounds pitches to `UP_PITCH_ALIGN` (so `lines*pitch` is a multiple), scratch/worker blocks round explicitly or via `_Alignas(64)`, and VLC picture pitches never reach `aligned_alloc`. | |

## performance

| id | status | effort | description | notes |
|----|--------|--------|-------------|-------|
| PERF-7 | open | S | `RecordPerf` (`autoupscale.c:810-813`) calls `var_SetInteger` twice on EVERY frame ("autoupscale-ewma-us", "autoupscale-frames"). Each call takes the VLC object's variable lock and does a string-keyed variable lookup + callback scan — serial main-thread work on the per-frame hot path that no consumer reads at frame granularity. | Minor (sub-µs each vs a 16 ms budget) but pure waste: the stats are observability values, not per-frame contracts. Fix: move both `var_SetInteger` calls into the existing OBS-3 5-second tick (`MaybeLogStats`) so the hot path pays nothing between ticks; frame_count++ stays per-frame. |

## scalability

| id | status | effort | description | notes |
|----|--------|--------|-------------|-------|
| (none open) | | | SCAL-2 FULLY RESOLVED. Per-frame main-thread dispatch is now O(1) syscalls both ways: WAKE side bumps a shared `generation` under a mutex + one `pthread_cond_broadcast` (was N `sem_post(go)`); DONE side is a counting barrier — workers decrement an atomic `pending`, the last posts a single `all_done` the main waits on once (was N `sem_wait`). `should_exit` is now mutex-protected (set + broadcast in `zimg_wake_all_for_exit`). Validated race-free + byte-identical under the ASan/UBSan and TSan harnesses; bench-zimg shows near-linear 1->16 thread scaling. |

## concurrency

| id | status | effort | description | notes |
|----|--------|--------|-------------|-------|
| CON-3 | open | S | Unchecked `sem_wait` return at all three semaphore-barrier sites: `usm_pool.c:252` (worker `sem_wait(&w->go)`), `usm_pool.c:447-448` (main `for(i<n) sem_wait(&p->done)`), `scaler_zimg.c:1099` (main `sem_wait(&p->all_done)`). `sem_wait` fails with `EINTR` when a signal handler interrupts it; each failure silently breaks the frame barrier. Worker-side: an EINTR'd `go` wait makes the USM worker run a SPURIOUS frame against the PREVIOUS frame's `src`/`dst` pointers — a picture VLC has already released — i.e. a use-after-free write into pool memory, then a stray `sem_post(done)` that permanently skews the counting barrier by one (main thereafter returns from `usm_pool_run` while one worker is still writing → per-frame-field data race + writes to released pictures every frame). Main-side: an EINTR'd `done`/`all_done` wait returns early while workers are still writing the (default zero-copy) VLC dst picture, which `Filter()` then hands downstream / releases. | The zimg WORKER side is immune (`pthread_cond_wait` predicate loop, no EINTR), so only these three sites need it. Not reproducible under TSan/stress (no signal delivered mid-wait), plausible in production: libvlc embedders routinely install non-`SA_RESTART` handlers and worker threads inherit an open signal mask. Fix (S): wrap each site in the standard retry loop `while (sem_wait(s) == -1 && errno == EINTR) {}` (needs `<errno.h>`); on any other error, fail the dispatch rather than proceed. Mirrors the defensive posture the rest of the codebase already applies. |
| (none open beyond CON-3) | | | CON-2 RESOLVED by contract doc: the non-atomic `lazy_init_done`/`lazy_init_failed` check-then-act is sound under VLC's serial-per-instance `Filter()` contract, now stated at the field declaration with the pthread_once/`_Atomic` escape hatch if the scaler is ever shared across threads within one instance. | |

## code complexity

| id | status | effort | description | notes |
|----|--------|--------|-------------|-------|
| (none found) | | | `lizard src -C 10` reports no thresholds exceeded; max measured CCN is 10 (`ChromaToAVFmt@scaler_swscale.c:29`). Gate passes clean. | |

## code duplication

| id | status | effort | description | notes |
|----|--------|--------|-------------|-------|
| DUP-3 | keep | S | Worker-pool lifecycle (lazy-init flags, aligned_alloc + memset, sem_init, spawn loop with `constructed`, sticky `lazy_init_failed`) duplicated between `scaler_zimg.c` and `usm_pool.c`; per-worker `_Alignas(64)` struct + rationale comment copy-pasted | DECISION (2026-05-30): keep. A shared scaffold needs a type-erased pool (void* element + per-worker construct/destroy callbacks) since the worker structs and per-worker work differ (per-stripe zimg graphs vs scratch rows). The common part is ~15 lines of alloc+memset+spawn; hiding it behind a callback interface across a module boundary adds indirection while the real work stays divergent — net clarity loss. Per AGENTS.md (SOLID only when it improves clarity). |
| DUP-5 | keep | S | Args-valid checks parallel: `usm_pool.c` (`usm_pool_validate_args`) vs `usm.h` (`up_usm__args_valid`) — both null dst/src + stride<width | Marginal: the pool variant also checks its own ptr and uses the stored `p->width`, while `up_usm__args_valid` validates full dims; not cleanly mergeable without threading width/height through. Keep. |
| DUP-8 | open | S | Stripe-partition math (`i*h/n`, last stripe absorbs remainder) computed twice in `usm_pool.c`: `usm_pool_spawn_worker` (:310-313) writes each worker's `y_start/y_end`, then `usm_pool_spawn_all` unconditionally calls `usm_pool_repartition_stripes` (:340-345) which recomputes and overwrites them before any worker can read them (workers block on `go` until the first apply). | The spawn-time assignment is dead code — always overwritten. Delete the `y_start/y_end` lines from `usm_pool_spawn_worker` and let `usm_pool_repartition_stripes` be the single partitioner (its comment already calls the spawn-time values a "redundant (idempotent) re-assignment"). Net −4 lines, one source of truth, no behavior change. |
| DUP-9 | open | S | Online-CPU detection duplicated with divergent clamps, contradicting `threading.h:7-10`'s claim "Single source of truth — both DetectHardware (autoupscale.c) and the zimg backend call this so they can't drift": `zimg_open` DOES call `up_detect_cores()` for the thread count (`scaler_zimg.c:1002`) but then re-implements raw `sysconf(_SC_NPROCESSORS_ONLN)` with its own clamp for `cpus_online` (`:1040-1044`, cap `UP_THREADS_MAX*64` = 4096 vs `up_detect_cores`'s `UP_THREADS_MAX*4` = 256). | The drift the header warns about has already happened, just on the SCAL-4 pin path. Fix: `p->cpus_online = up_detect_cores();` — identical behavior on any real machine (both caps far above physical counts), −5 lines, restores the single source of truth. |

## architecture/modularity/SOLID

| id | status | effort | description | notes |
|----|--------|--------|-------------|-------|
| ARCH-4 | keep | S | `plane_set_t` on `stripe_worker_t` carries three roles: scratch geometry (`.lines_*`, priv-level only), the worker's scratch view (`src`/`dst`), and per-frame VLC picture pointers (`vlc_src`/`vlc_dst`) | DECISION (2026-05-30): keep. A `plane_geom_t`{pitch,lines} / `plane_ptrs_t`{y,u,v,pitch} split duplicates `pitch` across both types and ripples through `point_workers_planes`, the copy helpers, scratch alloc, and every worker field — for an overload whose only cost is two unused `int`s (`lines_*`) carried in the worker views. Net more types/code, marginal clarity. Per AGENTS.md (SOLID only when it helps). Revisit if a third consumer with different geometry needs appears. |
| ARCH-7 | open | S | `UP_PROBE_WINDOW_FRAMES` (60) lives in `autoupscale.c:336-339` while its coupled counterpart `UP_PROBE_MIN_FRAMES` (10) and all other probe thresholds live in `content_probe.h:256-259`. If the window were ever shrunk below MIN_FRAMES, `up_should_bypass_for_content` would silently never fire — and that coupling is invisible from autoupscale.c. | Fix: move the #define next to UP_PROBE_MIN_FRAMES in content_probe.h with a one-line "must be >= MIN_FRAMES" note (or a `_Static_assert`). One-line move; puts constants that must stay consistent side by side. |
| ARCH-8 | open | S | Zero-copy default/opt-in doc drift, including an INTERNAL contradiction inside `scaler_zimg.c`: the `dst_zerocopy` field comment (`:271-277`) ends "Opt-in via the autoupscale-zerocopy-dst module option; default off." while the `src_zerocopy` comment five lines below (`:281-283`) says both sides are "ON by default". Both options default to 1 (`autoupscale.c:249-252`, README table). `scaler.h:49-53` likewise still labels `zerocopy` "(opt-in, not everywhere)" and `src_zerocopy` "(experimental, opt-in)". | Same doc-drift class as ARCH-5/6, new instance: a maintainer reading the interface header or the dst field comment learns a default that flipped when zero-copy shipped default-ON (VAL-1). Fix: reword the three comments to "default ON, option is the copy fallback". Pure comment edit. |
| ARCH-9 | open | S | Module-option longtexts contradict the declared defaults/ranges in the same `vlc_module_begin` block (`autoupscale.c`): (a) `usm` longtext `:107` says "Default 30 (subtle)" but the declared default is `UP_USM_AMOUNT_DEFAULT` = 20 (`usm.h:37`, README says 20); (b) `zimg-stripe-lines` longtext `:180` says "Default 16. Range 4..128" but the option is declared 0..128 default 0, the 0-sentinel is undocumented, and `up_zimg_stripe_min_lines` (`zimg_helpers.h:45-48`) passes 1..3 through as-is so the advertised floor of 4 is never enforced; (c) `usm-stripe-min-rows` longtext `:174` says "Range 1..256" but is declared 0..256 default 0 (0 = compile-time 8). | User-facing help (`vlc --help`, qt prefs) shows the longtext; it currently misstates one default and two ranges. Fix: correct the three longtexts (state the 0 = auto sentinel like README:111 does), and either clamp `zimg-stripe-lines` 1..3 up to 4 in `up_zimg_stripe_min_lines` or drop the "4.." claim. |

## system design

| id | status | effort | description | notes |
|----|--------|--------|-------------|-------|
| SYS-1 | open | M | The per-frame dispatch model diverged between the two structurally-parallel worker pools. zimg was migrated (SCAL-2) to a single mutex+`generation`+`cond_broadcast` wake plus an atomic-`pending` counting barrier — O(1) syscalls each way. The USM pool was left on the OLD O(N) model: `usm_pool_run` does N×`sem_post(go)` + N×`sem_wait(done)` every frame (`usm_pool.c:445-448`), one `sem_t go` per worker. | SCAL-2 is logged "RESOLVED" but only for zimg; the USM pool runs once per USM-enabled frame, so the 2N-syscall cost is real, just smaller (single-pass, ~6-30 workers). Two near-identical pools now carry DIFFERENT concurrency designs, undercutting DUP-3's "keep — they're parallel" rationale and creating a maintenance/divergence trap. Direction: port the broadcast+barrier to the USM pool (harness already validates it for zimg), OR document why USM intentionally stays on per-worker sems. |
| SYS-2 | open | M | No runtime backend fallback after a successful `Open()`. Backend selection (`scaler_pick`) runs once at Open; zimg's heavy setup (worker spawn, scratch alloc, per-stripe graph build) is deferred to the FIRST frame (`zimg_ensure_lazy_init`/`zimg_lazy_init`). If that deferred init fails, `lazy_init_failed` is sticky (`scaler_zimg.c:266-267,925-926`) and `zimg_process` returns -1 for the filter's whole lifetime → `Filter()` drops EVERY frame. swscale opens eagerly in `sws_open`, so its failure is caught at Open and lets VLC try another filter. | The `scaler_pick`/`scaler_backend_t` seam makes zimg→swscale failover look designed-in, but it only operates pre-Open. A first-frame zimg failure (OOM under pressure, graph-build edge case) is unrecoverable degradation to frozen/black output instead of the universal fallback the swscale backend exists to provide. Distinct from the rejected "Open `be->open` failure" item (that was Open-time; this is post-Open first-frame). Direction: on sticky lazy-init failure re-`scaler_pick(SWSCALE)`+`be->open` once and swap `ctx->backend`, OR force eager zimg init at Open when `backend=auto` so the failure surfaces while VLC can still pick another filter. |
| SYS-3 | no-action | S | `up_usm_pool_variant_name` is a process-global mutable `const char*` (`usm_pool_dispatch.c:85`) set by an `__attribute__((constructor))` and read per-filter-instance in Open's engagement log (`autoupscale.c:616`). It is the one piece of cross-instance mutable global state. | Not a live bug — write-once at dlopen (constructor completes before any plugin entry point), read-only after, value never changes. Recorded as the SOLE global-mutable-state item for completeness; becomes a data race only if a future variant re-selects at runtime. DEC-1 already rejected an accessor on tradeoff grounds, so keep as-is (treat as write-once). |
| SYS-4 | open | M | Cross-module CONTRACT VIOLATION: `ApplyUsmIfEnabled` (`autoupscale.c:770-775`) calls `up_usm_pool_apply` IN-PLACE (`y->p_pixels` as both dst and src) with `amount_q8 > 0`, but `usm_pool.h:69-70` permits aliasing only for "the identity/amount=0 case", and the fused sweep's correctness argument (`usm_pool.c:20-24`: boundary rows "re-hblurred locally" from an assumed-immutable src) depends on dst != src. With n_threads >= 2 and dst == src, worker i's boundary reads of src rows `y_start-1` (`usm_pool.c:222-223`) and `y_end` (`:227-228`) race with neighbor workers concurrently WRITING those same rows as dst → data race + nondeterministic stripe-boundary output; additionally `up_usm__combine_row` (`usm.h:192-198`) takes `restrict`-qualified dst_row/src_row that production aliases to the same row (UB). Single-threaded pools (auto on <=8 cores) and the test harness (separate buffers) never hit it — which is why TSan/byte-identity gates stayed green. | Also belongs in the concurrency + UB tables; root cause filed here because it is a caller/callee contract mismatch, not a pool bug. Fix directions: (a) make Filter() pass a distinct src (needs a persistent luma-sized scratch: memory cost), or (b) have the pool support in-place by snapshotting each stripe's two boundary halo rows into per-worker scratch on the MAIN thread in `usm_pool_set_per_frame` (serial, 2 rows/worker, cheap) before dispatch, and drop/scope the `restrict` on `combine_row`'s dst/src, then update the header contract. (b) keeps the zero-extra-copy hot path and makes the documented API match the only production call. Validate with the TSan harness driven IN-PLACE. |
| SYS-5 | open | S | The documented zero-copy SAFETY FALLBACK is silently ignored on the column-tiled path: `zimg_open` forces `src_zerocopy = true` whenever `col_tiled` (`scaler_zimg.c:1029-1032`), overriding `--autoupscale-zerocopy-src=0` — the exact knob the option longtext (`autoupscale.c:155-166`) and VAL-1's mitigation plan ("revert the default with --autoupscale-zerocopy-src=0, one-line flag flip") sell as the escape hatch for VLC-pool instability. On wide/short frames the escape hatch does nothing and nothing is logged. | The tiled design genuinely needs full-width source reads (active_region halo), so honoring copy-in would mean either disabling column tiling when the user sets zerocopy-src=0 (falls back to rows-only grid — safe, fewer workers) or copying the full source to scratch for tiled workers. Minimum honest fix: prefer "zerocopy-src=0 => cols=1" (the user asked for the safe path; give it to them), else at least msg_Warn the override so VAL-1's documented mitigation isn't a silent no-op. |
| SYS-6 | open | S | Lazy-init failure observability diverges between the two pools: zimg's sticky first-frame init failure logs once via msg_Err (`scaler_zimg.c:1144-1153`, OBS-2) and dropped frames are counted; the USM pool's sticky lazy-init failure is fully SILENT — `up_usm_pool_apply` returns -1 but `ApplyUsmIfEnabled` (`autoupscale.c:764-776`) discards the return, so a user who set `--autoupscale-usm=30` gets no sharpening for the whole playback with zero indication, every frame, forever. | Same lifecycle model, opposite failure visibility (lifecycle-model coherence gap). Fix: check the return in ApplyUsmIfEnabled and msg_Warn once (mirror `process_fail_logged`), optionally clearing `usm_amount_q8` so subsequent frames skip the dead call. ~6 lines in autoupscale.c; brings the USM pool up to the OBS-2 standard the zimg pool already meets. |
| SYS-7 | open | M | Unit-convention mismatch between the two consumers of the same probe accumulator. `up_should_bypass_for_content` (`content_probe.h:302-303`) squares its softness threshold (`lap_mean < 400² = 160000`) while `up_should_skip_usm_for_sharpness` (`:280-288`) compares the SAME `lap_sum/lap_samples` value linearly against 3500 — documented (`:261-269`) as "the same units as lap_sum/lap_samples" and calibrated against the field-measured band in this very file (`:252-255`: clean grainy ≈ 800-2000, blocky ≈ 200-400). Under those measured magnitudes every real source satisfies `lap_mean < 160000`, so `very_soft` is effectively ALWAYS true and the bypass verdict degenerates to `edge_mean > 6` alone — contradicting the documented decision matrix ("don't bypass on blockiness alone", `:246-248`). | tests/test_content_probe.c encodes the squared convention with synthetic means of `100²`/`200²`/`1500²` (2.25M for "sharp") — three orders of magnitude above the file's own measured band, so the blocky-but-sharp no-bypass case never occurs on real content. One of the two conventions is miscalibrated. Direction: pick the unit (mean-of-squares is what the code computes AND logs in RunProbe), re-measure the soft cutoff in those units, drop the `THRESH²` squaring, and rewrite the tests with realistic magnitudes. |

## decoupling

| (none open) | | | | |

## business/design patterns/DDD

| id | status | effort | description | notes |
|----|--------|--------|-------------|-------|
| PAT-2 | keep | S | `zimg_process` is an implicit pipeline (point → dispatch) with the zerocopy variation handled per side | DECISION (2026-05-30): keep. The "scattered branches" no longer exist: `zimg_process` is a flat linear skeleton, and the zerocopy variation is a single offset ternary per side (`WORKER_SRC_OFF` vs `WORKER_VLC_SRC_OFF`) plus per-worker `copy_in`/`copy_out` flags consumed inside `worker_main`. A Template-Method vtable would add fn-pointer indirection for two static paths with no real branching to hide. Per AGENTS.md (patterns only when they improve clarity). Revisit if a third output mode appears. |

PAT-1 (group dispatch fn-pointers into a usm_pool_ops_t vtable) DONE — commit 0f92daa.

## reliability/correctness

| id | status | effort | description | notes |
|----|--------|--------|-------------|-------|
| REL-4 | open | S | ODD source visible dimensions are never even-aligned, and the zimg backend cannot represent them: `ResolveInputDims` (`autoupscale.c:380-389`) takes `i_visible_width/height` raw, `up_plan_upscale` even-rounds only the DST (`up__clamp_even` in `up_compute_target_dims`), and `zimg_open`/`init_priv_geometry` pass the raw src dims into every per-cell `build_stripe_graph` (`scaler_zimg.c:528-529`: `src_fmt.width = src_full_w`, `height = src_stripe_h`). zimg rejects image dims not divisible by the subsample factor (I420/YV12: both axes; I422: width), so an odd-visible-dim source (e.g. 853x479 I420 crop) passes Open, then EVERY graph build fails at first frame → `zimg_lazy_init` fails → sticky `lazy_init_failed` → every frame dropped for the whole playback (frozen/black output, SYS-2's unrecoverable path). tests/test_scaler_zimg.c:66-69 documents the even-dim requirement but claims "production always satisfies via up__clamp_even" — true only for dst, false for src. | Deterministic trigger for SYS-2's sticky-failure hole, hit by any odd container crop. Fix (S): even-align (`& ~1`) src_w/src_h in `ResolveInputDims`/`ConfigureScaler` when the chroma is subsampled (scaling from 852x478 instead of 853x479 is visually free), or have `zimg_open` return -1 on odd dims so Open falls back cleanly while VLC can still build a chain. Secondary: even if zimg accepted odd dims, `worker_copy_in_stripe`'s chroma row count floors (`(src_y_end >> sub_h) - cs`, `scaler_zimg.c:354-355`) and would skip the last ceil'd chroma row → uninitialized scratch read; even-aligning src fixes that too. 2026-07-10 audit. |
| REL-5 | open | S | The documented "disable" value of `--autoupscale-skip-above` is unreachable and inverts into "never engage": the longtext (`autoupscale.c:95-102`) promises "Values <= 0 disable the gate (AUTO engages on any sub-target source)" and `up__plan_inputs_ok` (`upscale_logic.h:210`) implements `skip_above > 0` as gate-off, but the option is declared `add_integer_with_range(..., 720, 1, 8192, ...)` (`:234-235`) — VLC's config layer clamps a user's 0 up to the range minimum 1, and `skip_above == 1` makes `src_h >= 1` true for every source, so AUTO bypasses ALWAYS. A user following the built-in help to disable the gate instead disables the entire filter. | ClampConfig already handles <=0 correctly (`:529`), only the declared range blocks it. Fix: change the range minimum from 1 to 0 (one character); optionally note 0 in the longtext's first line. Distinct from ARCH-9 (that item is longtext/default drift on usm + stripe options; this one flips behavior). 2026-07-10 audit. |
| — | | | REL-3 (runtime zimg API major-version probe in `zimg_open`, fails graceful on ABI mismatch) DONE — commit pending. | |

## error handling

| id | status | effort | description | notes |
|----|--------|--------|-------------|-------|
| (none found) | | | | |

## portability/standards conformance

| id | status | effort | description | notes |
|----|--------|--------|-------------|-------|
| PORT-3 | no-action | S | `perfmon.h:96` EWMA update right-shifts a signed `diff` | Well-defined arithmetic shift on every twos-complement target (all real ABIs); already documented in the file. No change unless a non-twos-complement target appears. Kept as a known, accepted item. |
| PORT-5 | open | S | `make test` / `make fuzz` cannot build on non-x86 hosts: the SIMD-variant object rules hardcode `-march=x86-64` / `x86-64-v3` / `x86-64-v4` literals (Makefile:695-702, 293-298, 378-383), so the whole `test` target (which depends on `test_usm_pool_variants`) fails on aarch64 even though `plugin` builds fine there via `MARCH=native` | Also needs gcc>=11/clang>=12 for `x86-64-v4`. If x86-only is intentional, gate the variant targets behind an arch check and skip with a message instead of a compile error; otherwise fall back to a single-baseline variant test off-x86. |
| (none open beyond PORT-3/PORT-5) | | | PORT-4 (`<stdalign.h>` + `alignas` over the `_Alignas` keyword in `usm_pool.c`/`scaler_zimg.c`) DONE — commit pending. | |

## resource management

| id | status | effort | description | notes |
|----|--------|--------|-------------|-------|
| RES-2 | decided | M | `scaler_zimg.c` + `usm_pool.c` each filter instance spawns up to `UP_THREADS_MAX` (64) zimg workers + up to 64 USM workers + multi-MB scratch, with no process-wide cap across concurrent AutoUpscale instances | DECISION (2026-05-30): per-instance bound is intentional; no aggregate budget added. The two pools run sequentially per frame (rejected SCAL-1), and each pool further clamps thread count to `dst_h / stripe_min_lines` and core count, so the only cross-instance cost is idle-thread address space (untouched stacks), not CPU or RSS. Typical VLC runs one filter instance per pipeline; a global atomic budget would add shared mutable state + a new open-time failure mode for no real gain. Revisit only if a concurrent-many-instance use case appears. |

## API/ABI stability

| id | status | effort | description | notes |
|----|--------|--------|-------------|-------|
| (none open) | | | | |

## build/toolchain hygiene

| id | status | effort | description | notes |
|----|--------|--------|-------------|-------|
| BUILD-8 | open | S | No `-std=` pinned anywhere in the Makefile: every compile (plugin, tests, fuzz, coverage, bench) runs at the compiler's default dialect (gcc on this host: `gnu17` per `__STDC_VERSION__ 201710L`; newer gcc defaults to `gnu23`), while the cppcheck gate checks `--std=c11` — dialect drift between what is analyzed and what is compiled | Code targets C11 (`stdalign.h`, `aligned_alloc` C11 size contract). A default-dialect bump to C23 changes semantics (`bool`/`true`/`false` keywords, old-style declarations removed). Fix: add `-std=gnu11` to `COMMON_CFLAGS`, `TEST_CFLAGS`, `FUZZ_CFLAGS`, `SMOKE_CFLAGS`, `STRESS_CFLAGS_*`, `COV_CFLAGS`, `BENCH_CFLAGS`, `ZIMG_H_CFLAGS` (gnu not c: `_GNU_SOURCE`, `sysinfo`, semaphores in use). |
| BUILD-10 | open | S | Makefile nits: (a) `stress` target (line 415) missing from `.PHONY` — a file/dir named `stress` would silently mask it; (b) the `build-bench:` rule (line 513) sits mid-sentence inside the bench comment block (lines 511-520), splitting "matches the production / USM_POOL_CFLAGS optimization level" in two; (c) the `fuzz` target's echo list omits `build/fuzz_decide_tile_grid` though it is built | Three one-line fixes. |
| — | | | BUILD-2 RESOLVED: the cppcheck gap on `autoupscale.c`/`scaler_zimg.c` (cppcheck can't parse VLC's macro headers) is now covered by `make scan-build` — the clang static analyzer DOES parse VLC headers and runs over those exact TUs with `--status-bugs` (CI-gating). Verified clean ("No bugs found", 2026-05-30). Combined with gcc+clang `-Werror` and the ASan/UBSan/TSan harness, the `.so` analysis gap is closed. `-fanalyzer` stays deferred (noisy on VLC headers; scan-build supersedes the need). |

## observability

| id | status | effort | description | notes |
|----|--------|--------|-------------|-------|
| (none open) | | | OBS-4 OBSOLETE: it assumed a SERIAL copy-out tail, but PERF-5 (commit b4b86b3) made copy-out PARALLEL — each worker copies its own stripe inside `worker_main`, folded into the per-frame dispatch the workers all complete before the barrier. There is no separable serial copy-out cost to surface; total per-frame time (incl. parallel copy-out) is already visible via OBS-3's EWMA. Measuring it would need per-worker hot-path timing + a max-reduction for negligible value. Closed, not deferred. |

## wiring gaps

| id | status | effort | description | notes |
|----|--------|--------|-------------|-------|
| WIRE-3 | open | S | `--autoupscale-usm-sharp-threshold` is silently inert when `--autoupscale-content-probe=0` (or on non-planar chromas): `usm_skip_sharp` is set ONLY inside `RunProbe` (`autoupscale.c:716-719`), which runs only while `probe_active`, which is gated on the `content-probe` option (`:498-501`). Neither option's longtext (`:182-191`, `:193-205`) mentions the coupling — the sharp-threshold help even implies its own independent off-switch ("0 = feature off"). | A user who disables the probe (documented as a pure diagnostic: "DIAGNOSTIC only") also unknowingly disables the USM grain-skip feature, which is NOT diagnostic — it changes pixel output. Fix: document the dependency in both longtexts (cheapest), or decouple by letting the sharpness accumulation run independent of the advisory probe flag. |
| — | | | WIRE-1 (flat-skip wired + exercised via `make bench-flatskip`, commit cc71221) and WIRE-2 (oracle documented, commit 0f92daa) resolved. | |

## unused functions/methods

| id | status | effort | description | notes |
|----|--------|--------|-------------|-------|
| DEAD-1,2,3,5 | keep | S | `up_usm_workspace_size`, `up_usm__pass1_hblur`, `up_usm__pass2_combine`, `up_usm__args_valid` (usm.h) are reachable only via `up_usm_apply_plane` | DECISION: keep. They are the single-threaded byte-identity TEST ORACLE for the threaded pool (documented at up_usm_apply_plane via WIRE-2, commit 0f92daa). Not dead — intentionally test-only. |
| DEAD-8 | open | S | Two write-only fields on `stripe_worker_t` (`scaler_zimg.c`): `src_x_start` (declared `:179`, written `:708`, never read — its comment "used by active_region" is FALSE: the active-region crop consumes the `src_x_start` PARAMETER at graph-build time in `build_worker_graph_and_tmp`, not the field) and `worker_id` (declared `:181`, written `:728`, never read — the pin call `:753` uses the parameter). | Verified by grep: no read sites for either field. Delete both fields and the misleading comment; net −4 lines, removes a false data-flow claim from the worker struct. |
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
| VAL-1 | Real-VLC validation of source zero-copy now default-ON (commit 2236264). Run actual VLC across I420/YV12/I422/I444 sub-720p sources + the threads/zerocopy options; confirm no crash, no garbled output. | The harness proves byte-identity on malloc'd pictures but CANNOT reproduce the documented VLC-pool segfault history. The pre-flight guard (zimg_pic_ok) catches null/bad-pitch geometry, not deeper pool/lifecycle issues. If it misbehaves on a real VLC build, revert the default with `--autoupscale-zerocopy-src=0` (one-line flag flip) pending a fix. Dst zero-copy (also default-ON) shares the same caveat but has shipped longer. |

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
| `scaler_zimg.c:104-114` `pin_worker_to_cpu` "CPU_SET with cpu >= CPU_SETSIZE (1024) is documented-undefined" (cpus_online capped at 4096) | Unreachable: `cpu = worker_id % cpus_online` and `worker_id < n_threads <= UP_THREADS_MAX (64)`, so cpu <= 63 < CPU_SETSIZE regardless of the 4096 cap on `cpus_online`. |
| `scaler_zimg.c:494-498` `worker_main` "copies out uninitialized tile/dst scratch to VLC dst when `zimg_filter_graph_process` fails" | Copying indeterminate bytes via `uint8_t` memcpy is not UB, and the frame is unconditionally dropped: `zimg_dispatch_and_wait` returns -1 on any worker `result != 0`, so `Filter()` releases `p_out` — the garbage never leaves the plugin. |
| `usm_pool.c` / `scaler_zimg.c` unchecked `sem_wait` EINTR (UAF/null-deref via broken frame barrier) | Real, but already tracked as CON-3 — found independently this pass, verbatim the same three sites; not re-filed. |
| odd-height 4:2:0 source → zimg graph-build failure → sticky drop-every-frame | SUPERSEDED same day by REL-4 (filed open): the rejection's "conformant streams can't carry odd crops" premise holds only for H.264/HEVC 4:2:0 — VP9/AV1 permit odd frame dims with 4:2:0, and raw/container-cropped sources reach the filter too. See REL-4 for the fix. |
| BUILD-9 candidate: "stale `usm_pool.o`/`usm_pool_dispatch.o` committed to git at repo root" | Wrong: `git ls-files` does NOT list them and `.gitignore:4` (`*.o`, commit 1f7da23) covers them — they were untracked local leftovers, deleted from disk 2026-07-10. Nothing to fix in-tree. |
| `tests/test_zimg_helpers.c:244-245` unchecked `malloc` before writes (null-deref UB under OOM, test code) | Test-only, ~430 KB allocations, immediately crash-visible under the sanitizer harness that runs these tests; no product-code surface. Not worth a row. |

Coverage note: pure-logic files are 100% (gated). scaler_zimg.c is exercised
to ~94% by `make coverage-zimg` (was 0%); the rest needs a live VLC logger
(log_zimg_open) or fault injection and is intentionally ungated.
