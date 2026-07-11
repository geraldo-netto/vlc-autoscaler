# TODO — full-project audit findings

Full rescan of HEAD `5fbfa58` on 2026-07-11 (7 parallel category audits, every src/ file read
in full, tests/ + Makefile + CI + stubs inspected; all findings verified against actual code
paths). IDs restart at this rescan — they do not correlate with pre-rescan IDs in git history.
Row format: `id | status | effort | description | notes`.

## security

No open findings. Verified: config ints saturated (`InheritIntSat`) + clamped (`ClampConfig`);
every frame-geometry entry point (zimg, swscale, probe, USM) is behind `up_picture_view_init`
before any pixel access; allocation-size arithmetic overflow-checked at every seam
(`plane_alloc_bytes`, `plane_buffer_bytes`, `usm_pool_scratch_bytes`, `up_usm_workspace_size`,
`up_round_up_*`); `>> sub_w/sub_h` shifts clamped `< 8`; all format strings literal;
`up_cli_parse_long` checks `errno`/endptr/range.

## undefined behavior

| id | status | effort | description | notes |
|---|---|---|---|---|

No open findings. Verified clean: all chroma shifts operand-range-checked; every `aligned_alloc`
size is a proven multiple of the alignment and non-zero; target/aspect math widened to `int64_t`
with pre-multiply guards; negative right-shift deliberately avoided (`up_perfmon__ewma_step`);
Laplacian/edge kernels bounded so `lap*lap` fits `int`; picture-view offset/extent arithmetic
overflow-checked before pointer formation.

## memory management

| id | status | effort | description | notes |
|---|---|---|---|---|

No open findings. Verified: `Filter()` releases `p_in` on every one of its four exit paths with
no double-release; `zimg_lazy_init` partial failure retains state in `priv` and reclaims exactly
once via NULL-ing frees (idempotent under the second `zimg_close`); `TryBackendFallback` frees
zimg state before opening swscale; stats vars created/destroyed under the same gate; USM
partial-spawn shrink keeps every scratch offset in-bounds.

## performance

No open rows (see "Open — parked" PERF-P1, and SCAL-P3 which now carries the quantified
two-pool cost). Verified clean: zero steady-state per-frame allocations; no per-frame graph
rebuilds (graphs built once in lazy init, only buffer descriptors rebuilt per dispatch); default
aligned row-stripe path copies no planes; copy-in/out is opt-in and parallelized in-dispatch;
`restrict`/alignment hints correct on the hot USM kernels; hot USM TU is `-O3` while non-pixel
TUs stay `-O2`; false sharing padded in both pools.

## scalability

No open-table rows — see "Open — parked" (SCAL-P1..P4). Verified clean: stripe/tile partitions
balanced to ±1-2 rows and even-aligned; grid search is one-time O(64); `UP_THREADS_MAX=64` cap
and stripe/col floors prevent degenerate geometry; per-resolution memory is bounded; lazy init
keeps speculative filter-chain probes at ~zero cost; done-barrier is one atomic `fetch_sub` +
one sem round-trip per frame; no O(n²) patterns.

## concurrency

| id | status | effort | description | notes |
|---|---|---|---|---|

No open findings (full re-audit: every atomic order, both thread lifecycles, both drain paths
traced). Verified sound: gate wake-side publication ordered by the go-lock (the one relaxed
`pending` store is covered by the mutex, not the atomic); acq_rel `fetch_sub` + release-sequence
+ sem completion barrier publishes worker writes before main can overwrite next-frame state;
SYS-4 in-place halo protocol reads only main-thread snapshots; drain-under-failure joins before
returning FATAL (no use-after-free of the released picture); partial-`pthread_create` teardown
joins only started threads; EINTR retried, non-EINTR poisons the pool; perfmon is main-thread
only; the two pools share no mutable state and run strictly sequentially; test harnesses race-free.

## code complexity

| id | status | effort | description | notes |
|---|---|---|---|---|

Lizard 1.23.0 at HEAD: zero CCN>10 with the project threshold; every production function is at
≤7 params (only test helpers reach 8-10, inherent in/out tuples — deliberately not restructured).
`ChromaToAVFmt` (CCN 10) is an exempt flat switch.

## code duplication

| id | status | effort | description | notes |
|---|---|---|---|---|
| DUP-1 | open | S | The `stripe_bounds4` compat shim (10 lines, identical body + comment) is copy-pasted across `tests/fuzz_frame_shape.c:59`, `tests/fuzz_stripe_bounds.c:30`, `tests/test_zimg_helpers.c:15`. | Hoist once into a shared header (`tests/stripe_bounds_compat.h` or existing `tests/test_harness.h`). Verified byte-identical across the three sites. |
| DUP-2 | open | S | Four+ scattered deterministic-PRNG fillers with divergent constants: `fill_pseudorandom` (test_usm_pool.c:80, xorshift64), `fill_xorshift`/`xs32` (stress_usm_pool.c:51), `zt_pic_fill` (zimg_test_util.h:100), inline LCGs (test_usm.c:192, test_upscale_logic.c:689). | Add `tests/prng.h` exposing one `up_xs32`/`up_fill_random`; `zt_pic_fill` keeps its geometry loop and wraps the scalar. `tests/fuzz_smoke.h` already shares the smoke-main PRNG, so the pattern exists. Values shift once — verify no hardcoded expected outputs depend on the stream. |

## architecture/modularity/SOLID

| id | status | effort | description | notes |
|---|---|---|---|---|

No open findings. Verified: header-heavy pure-logic split is the deliberate documented
convention; autoupscale.c is the VLC entry TU (Open/Filter/Close + glue) with reusable logic
already extracted to headers — not a god file; both backends sit cleanly behind the
`scaler_backend_t` vtable; layering (autoupscale → backend → helpers) is one-directional.

## decoupling

| id | status | effort | description | notes |
|---|---|---|---|---|

No open findings. Verified: every config knob is read only in `Open` at the boundary
(`InheritIntSat`/`CFG_PREFIX` appear nowhere deeper), clamped once, then passed as plain
values/structs downward; backends are injected as `scaler_backend_t*`, not reached into; tests
exercise production logic through public header entry points. (The vtable-restates-prototypes
risk is tracked as ABI-2, not here.)

## business/design patterns/DDD

No open findings. The codebase already uses the right patterns (strategy vtables, table-driven
mappings, flat switches). Proposing more (factories, visitors over chroma formats) would be the
speculative pattern-work the project explicitly rejects.

## reliability/correctness

| id | status | effort | description | notes |
|---|---|---|---|---|

## portability/standards conformance

No open findings.

## error handling

| id | status | effort | description | notes |
|---|---|---|---|---|

No open findings. Verified: `pthread_create`, `aligned_alloc`, `clock_gettime`,
`filter_NewPicture`, `zimg_filter_graph_build`/`get_tmp_size`, `sem_init`, `pthread_*_init`,
`sched_getaffinity`, `sws_getContext` returns all checked; `sem_post` failure recorded via
`post_failed` and surfaced by `up_pool_gate_wait_all`; `sem_wait` EINTR-retried; sticky
lazy-init/`pool_broken`/`fallback_tried`/`drift.streak→FATAL` states propagate honest
TRANSIENT-vs-FATAL codes that gate the swscale fallback; best-effort ignores documented.

## resource management

| id | status | effort | description | notes |
|---|---|---|---|---|

No open findings in src/. Verified: gate init partial-failure unwinds under `cv_inited`/
`sem_inited` guards; `stop_workers` is safe to call twice (dispatch-failure + Close) and joins
only `thread_started` slots; barrier-failure drains + joins before returning FATAL;
`up_detect_cpu_topology_with` and `pin_worker_to_cpu` pair `CPU_ALLOC`/`CPU_FREE` on all
branches; no fds/sockets opened; allocation growth bounded everywhere. Test-harness allocations
are all balanced on every path.

## API/ABI stability

| id | status | effort | description | notes |
|---|---|---|---|---|
| ABI-1 | open | M | `tests/stubs/` hand-duplicate VLC struct layouts (`plane_t`/`video_format_t`/`picture_t`, `vlc_fourcc_t`, `VLC_CODEC_*`, swscale API) and `tests/test_scaler_swscale.c:8` `#include`s the production `.c` compiled against those fake headers (`-Itests/stubs`). Nothing pins the stub field names/types to real VLC 3.0. | Current stubs match VLC 3.0 field-for-field — this is drift *risk*, not a present mismatch: an upstream rename keeps unit/coverage green while the shipped `.so` diverges. Mitigate with a tiny TU compiled against real VLC headers that `_Static_assert`s `sizeof`/`offsetof` of the touched fields. |
| ABI-2 | open | S | `src/usm_pool_dispatch.c:70` restates the four variant signatures a second time as `usm_pool_ops_t` fn-pointer types, independent of the `usm_pool_variants.h` X-macro single-source-of-truth. | Presently guarded (CI builds `MULTIVERSION=1 -Werror`, so an incompatible-pointer assignment fails), but the vtable would be more robust deriving its member types from the shared header than restating them. |

## build/toolchain hygiene

| id | status | effort | description | notes |
|---|---|---|---|---|
| BUILD-1 | open | S | `make check-visibility` runs after the gcc build (ci.yml:45) and the MULTIVERSION build (:71) but **not** for the clang plugin step (:51-56, which builds then `make clean`s). clang-specific symbol-visibility/dispatch link regressions slip through. | Add a `check-visibility` to the clang job. |
| BUILD-2 | open | S | The SonarCloud job builds `make plugin` at default `MULTIVERSION=0` (ci.yml:218), so `PLUGIN_OBJS` is `usm_pool.o` only — `src/usm_pool_dispatch.c` and the per-`-march` variant compilations are never captured, and `sonar-project.properties:15` reads only `bw-output`. SonarCloud never analyzes the dispatch/multiversion code. | Add a MULTIVERSION build under the build-wrapper, or a second analyzed configuration. |
| BUILD-3 | open | S | `Makefile:817` defines a `scan-build` (clang static analyzer, `--status-bugs`) target that no CI job invokes, so the clang-analyzer path can rot silently. | May be a deliberate cost tradeoff — flagged as a coverage gap. Wire into CI or note as intentionally manual. |
| BUILD-4 | open | M | The multiversion variants are `-flto` objects at distinct `-march` levels linked `-flto`; the mechanism depends on the LTO link preserving per-TU target attributes, and the cross-variant test asserts **byte-identical** output, which masks an accidental ISA downgrade (nothing would catch a variant collapsing to baseline). | Codegen/perf-integrity gap, not a correctness bug. Verifying needs an objdump/ISA check on the built variant symbols in the shipped `.so`. |

## observability

| id | status | effort | description | notes |
|---|---|---|---|---|
| OBS-1 | open | M | `worker_main` (`scaler_zimg.c:497`) collapses the `zimg_error_code_e rc` to `result = (rc==0)?0:-1`, and `zimg_dispatch_and_wait` returns FATAL with **no log** on both the worker-error and barrier-break paths (`:1133-1142`). Only the generic dispatcher one-shot in `Filter` surfaces — no zimg error code, no barrier-vs-graph distinction. | Add a one-shot `msg_Err` at the failure site carrying `rc`; safe from spam because `pool_broken` latches. |
| OBS-3 | open | S | swscale backend failure paths are all silent: `SCALER_PROCESS_TRANSIENT` geometry reject (`scaler_swscale.c:118`), short-`sws_scale` line-count reject (`:144`, returned count discarded), `!priv` FATAL (`:111`). zimg has a one-shot `zimg_warn_bad_geometry`; the fallback everyone lands on has no counterpart. | Add a one-shot geometry warn symmetric with zimg's. |
| OBS-5 | open | S | `log_zimg_open` (`scaler_zimg.c:917`) reports only plane-scratch MB, not the per-worker `zimg_filter_graph` tmp buffers (`build_worker_graph_and_tmp`, ~0.5-2 MB each, up to 64 workers → tens of MB, and column-tile graphs keep `src_full_w` so tmp doesn't shrink). The dominant memory term is unaccounted. | Sum `w->tmp_size` into the open log; optionally cap worker count by a tmp budget. |

No log-spam risk on per-frame paths: perf advisory, process-fail, alignment-drift, bad-geometry,
probe verdict, and USM-pool-failure messages are all one-shot latched; periodic stats are
`msg_Dbg` on a 5 s tick; engagement/backend/grid selection is logged once at Open.

## wiring gaps

| id | status | effort | description | notes |
|---|---|---|---|---|
| WIRE-1 | open | S | Exported VLC stat variables `autoupscale-ewma-us` / `autoupscale-frames` (`autoupscale.c:657,981`) are write-only — no in-tree consumer reads them (by design, for external monitoring), but the var names are documented nowhere a user would discover them. | Add a one-line README note listing the exported var names (and `autoupscale-dropped` if OBS-4 lands), or drop if unused. |

All 14 `add_integer_with_range` options traced end-to-end to a concrete consumer; both backends
reachable through `scaler_pick` with AUTO/open/runtime fallback wired; no env vars anywhere.
Documented-and-accepted exceptions: `USM_POOL_FLAT_SKIP` is a bench-target-only feature
(`Makefile:631`), correctly excluded from the shipped plugin for byte-identity.

## unused functions/methods

| id | status | effort | description | notes |
|---|---|---|---|---|
| UNUSED-1 | open | S | `src/scaler_zimg.c:97` — local macro `ALIGN_DOWN_2(x)` aliases `UP_ALIGN_DOWN_2` but has zero uses in the file or anywhere; the file reaches even-align logic only through plane_utils helpers. | Verified: `grep -rn '\bALIGN_DOWN_2\b' src/ tests/` matches only the `#define` itself. Delete the line. |
| UNUSED-2 | open | S | `src/picture_view.h:19-23` — `up_picture_plane_view_t` fields `width`/`height`/`row_bytes`/`pixel_pitch` are populated by `up_picture_plane_view_init` but production reads only `.pixels` and `.pitch` (RunProbe, ApplyUsmIfEnabled, `point_workers_planes`, swscale); the four extra fields are read only in `tests/`. | Not dead (they're a validation byproduct), but production carries them unused. Add a comment noting they're computed for test/validation assertions only. |

No other unused functions/macros: all 85 header inline helpers have ≥1 caller (single-caller
helpers are complexity-split extractions); all non-static `.c` symbols are wired; the test-only
oracles (`up_usm_apply_plane`, `up_laplacian_variance`, `up_block_edge_strength`) are documented
in-source.

## Open — parked

| id | status | effort | description | why not now |
|---|---|---|---|---|
| PERF-P1 | parked | M | `src/usm_pool.c:476-516` — in-place USM does 2×n_threads serial main-thread halo-row memcpys per frame before dispatch (~53-115 KB/frame at 1080p, ~0.5 MB worst case at 4K/64). | Folding snapshots into workers needs an extra ready-barrier — significant complexity for a cost that hasn't shown up in a profile. Measure first. |
| TEST-P1 | parked | S | `up_usm_pool_effective_threads`: the mutant `n_threads`→`n_threads_pref` survives the suite — the fields diverge only under a deterministic partial spawn, and the RLIMIT_NPROC test asserts bounds only. | Killing it needs pthread_create fault-injection infra (new `--wrap`) for a diagnostics-only getter; not worth the infra now. |
| SCAL-P1 | parked | L | Static equal-work stripe partition + full per-frame completion barrier (usm_pool.c:530-542, scaler_zimg.c:1167-1195) makes frame latency `max` over workers; on hybrid P/E-core CPUs or decoder-shared cores, fast workers idle at the barrier every frame. | Partition math is balanced (±1-2 rows); the fix is dynamic stripe stealing or heterogeneity-aware sizing — a large change against a deliberately simple, verified-correct design. Revisit with profile evidence on hybrid hardware. |
| SCAL-P2 | parked | M | Per-worker zimg graph + tmp buffer (+ per-tile dst scratch) grows memory and graph-build time linearly with thread count, up to 64 graphs (scaler_zimg.c:732-759). | Independent graphs are what makes the frame path lock-free; bounded by thread caps and stripe floors. Inherent design cost. (The *logging* gap is broken out as OBS-5, which is actionable now.) |
| SCAL-P3 | parked | L | zimg pool and USM pool are separate persistent pools sized from the same budget (autoupscale.c:585, scaler_zimg.c:774): up to 2×64 threads per instance that only ever run sequentially within a frame. Concretely: auto policy on a 32-core box spawns 14+14 = 28 persistent threads, and the USM pass is a second cache-cold full read+write of the luma with its own broadcast/barrier — ~0.1-0.16 ms (~0.6-1%) at 1080p, ~0.3-0.8 ms (~2-5%) at 4K, plus 2× wake/barrier cycles per frame, multiplied per filter instance. | A shared pool (share the gate + worker threads, keep separate run fns) would halve the thread herd even without fusing the passes; full fusion (emit the sharpen from the scaler workers' stripes, as the USM halo-snapshot trick already allows) removes one dispatch per frame and keeps stripes L2-warm. Effort M (share pool) to L (fuse). Large restructuring for a cost that hasn't surfaced in a profile — measure first. |
| SCAL-P4 | parked | M | `src/threading.h:326-350` broadcast wake serializes worker startup through one mutex: glibc `pthread_cond_broadcast` requeues waiters one futex-handoff at a time (~1-2 µs each), so at N=64 the last worker starts ~64-128 µs after the first — longer than the ~20-50 µs per-worker zimg work, and it happens twice per frame (both pools). | At auto thread counts (≤~30) the ramp is ~0.2-0.4% of budget — acceptable; it only bites explicit high `--autoupscale-threads` on small stripes. Fix direction: per-worker atomic-generation check outside the lock (futex/spin-then-wait) or tree wake. Revisit with profile evidence at high thread counts. |

## Audit picks deliberately rejected

Recorded so future passes don't re-pick them:

- **Unifying `worker_copy_in_stripe`/`worker_copy_out_stripe`/`worker_copy_out_tile`** (scaler_zimg.c:373-455) — shapes rhyme but direction and offset math differ; a unified helper needs ~8 params and reads worse.
- **`min_plane_dims` in tests/fuzz_scaler_seam.c re-encoding subsample knowledge** from scaler_zimg_chroma.h — 8 lines, too small to matter.
- **First-frame rows-only fallback passing reduced `n_threads` as budget** (scaler_zimg.c:1255-1256) — verified it cannot change the resulting grid: column tiling engages only when `row_limit < budget` and `min(row_limit, reduced) == row_limit` in every reachable case.
- **`lazy_init_done/failed` as plain bools** — race-free under VLC's documented serial `pf_video_filter` contract; documented in-source. Revisit only if that contract changes.
- **Per-worker 2-boundary-row re-hblur** in the USM pool — deliberate trade (<1% of USM work at 1080p) that buys barrier-free operation.
- **RunProbe fusion beyond PERF-1** / SIMD for probe sweeps — probe is 60-frame bounded and grid-subsampled; not worth kernel complexity.
- **Unifying the stripe-invariant checkers** (`check_stripe`/`check_alignment` vs `check_stripe_step`/`check_stripe_bounds_range` across the two stripe fuzzers) — the invariants each asserts differ (even-alignment vs range/canary), and merging produces exactly the ~10-param mega-helper the guide warns against. Fixing DUP-1 (the shared shim) is the real win.
- **Unifying `run_sse2`/`run_avx2`/`run_avx512`** (test_usm_pool_variants.c:60) — each names distinct per-ISA symbols that only exist in that build; unifying needs a code-gen macro, disfavored by the "prefer explicit" stance.
