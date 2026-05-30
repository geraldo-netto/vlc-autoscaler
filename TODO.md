# TODO

Review findings from a full-project audit (2026-05-30) against the AGENTS.md
review categories. One table per category. Format: `id | status | effort | description | notes`.

Effort: S (small) / M (medium) / L (large). Remove a row once its fix is
implemented + tested + merged (`git log` is the durable record). Keep deferred
items under "Open — parked" with a why-not-now note; keep rejected audit picks
under "Audit picks deliberately rejected".

## security

| id | status | effort | description | notes |
|----|--------|--------|-------------|-------|
| (none open) | | | SEC-1 (32-bit `totalram*mem_unit` overflow) fixed in commit 83f5ee0 (uint64_t + saturate). | |

## undefined behavior

| id | status | effort | description | notes |
|----|--------|--------|-------------|-------|
| UB-3 | open | S | `scaler_zimg.c:238` (`worker_copy_in_stripe`) and `:757` (`copy_planes_to_pic`) shift `1 << w->sub_w` / `1 << sub_w` with an UNCLAMPED `sub_w`; the UB-2 fix clamped `sub_h < 8` but left `sub_w` unguarded | Not reachable today (ChromaToZimg yields sub_w ∈ {0,1}; unsupported chromas rejected at open) — same defensive-consistency gap UB-2 closed for sub_h. Clamp `w->sub_w = (sub_w < 8u) ? sub_w : 0u` at `init_stripe_worker`, and the priv `sub_w` for the copy-out site. |

(UB-1/PORT-1 and UB-2 fixed in commit 8aa9104.)

## memory management

| id | status | effort | description | notes |
|----|--------|--------|-------------|-------|
| (none found) | | | Overflow guards (`plane_alloc_bytes`, `up_usm_workspace_size`, `up_round_up_pitch`) and lazy-init partial-failure cleanup (deferred to `close`/`destroy` via persisted `ctx->priv`/pool ptr) all verified sound. | |

## performance

| id | status | effort | description | notes |
|----|--------|--------|-------------|-------|
| PERF-5 | open | M | `scaler_zimg.c:839` `zimg_copy_out`/`copy_planes_to_pic` — copy-IN was parallelized into the workers (PERF-1) but copy-OUT is still a serial main-thread memcpy of all 3 planes on the zerocopy-OFF path: workers finish, then main serially copies the full dst (~125µs/1080p) while every worker idles | Asymmetric with PERF-1. Fold copy-out into the workers too (per-worker dst stripe, point at VLC dst via a per-frame pointer set like `zimg_zerocopy_point_workers`). Only matters when zerocopy-dst=0 (non-default); validate with the harness + a zc=0 bench (see BUILD-6). |
| PERF-6 | info | S | `scaler_zimg.c:871-874` `zimg_process` walks the worker array TWICE per frame in zerocopy mode (`zimg_point_workers_src` then `zimg_zerocopy_point_workers`), touching each worker cache line twice | Merge into one loop that sets `vlc_src` + dst pointers when `dst_zerocopy` is on. Trivial; removes a redundant n_threads pass on the hot path. Ties to DUP-7. |

(PERF-4 per-function O3 pragma removed DONE — commit 4714938, codegen proven identical 287==287 vector ops.)

PERF-1 (parallel copy-in) DONE — commit b0d327f. PERF-2 (fuse) DONE —
commit cc71221. PERF-3 (probe visible width) DONE — commit 0f92daa.

## scalability

| id | status | effort | description | notes |
|----|--------|--------|-------------|-------|
| SCAL-2 | open | S | `scaler_zimg.c:732` per-frame dispatch issues N `sem_post`/`sem_wait` pairs per phase from the main thread; sync cost grows linearly with thread count | USM path HALVED in cc71221 (fused to one dispatch). zimg dispatch half remains: a counting barrier / futex gate would cut its per-frame syscalls. Parked with PERF-1 (no zimg test harness). |

## concurrency

| id | status | effort | description | notes |
|----|--------|--------|-------------|-------|
| CON-2 | open | M | `scaler_zimg.c:766` `zimg_lazy_init()` keys off non-atomic `lazy_init_done` on first `zimg_process()` with no guard against concurrent `Filter()` entry (TOCTOU) | Safe while VLC drives one instance serially; add assert/comment or `pthread_once` guard if ever shared across threads. |

CON-1 (document the sem barrier contract for should_exit/result) DONE — commit 8aa9104.

## code complexity

| id | status | effort | description | notes |
|----|--------|--------|-------------|-------|
| (none found) | | | `lizard src -C 10` reports no thresholds exceeded; max measured CCN is 10 (`ChromaToAVFmt@scaler_swscale.c:29`). Gate passes clean. | |

## code duplication

| id | status | effort | description | notes |
|----|--------|--------|-------------|-------|
| DUP-3 | open | S | Worker-pool lifecycle (lazy-init flags, aligned_alloc + memset, sem_init, spawn loop with `constructed`, sticky `lazy_init_failed`) duplicated between `scaler_zimg.c:584` and `usm_pool.c:316`; per-worker `_Alignas(64)` struct + rationale comment copy-pasted | Small shared worker-pool scaffold; low priority since the two payloads (zimg graphs vs scratch rows) differ. |
| DUP-5 | open | S | Args-valid checks parallel: `usm_pool.c` (`usm_pool_validate_args`) vs `usm.h` (`up_usm__args_valid`) — both null dst/src + stride<width | Marginal: the pool variant also checks its own ptr and uses the stored `p->width`, while `up_usm__args_valid` validates full dims; not cleanly mergeable without threading width/height through. Keep. |
| DUP-6 | open | S | Chroma subsample round-up `(v + (1<<sub) - 1) >> sub` open-coded in 3 sites: `scaler_zimg.c:238` (`worker_copy_in_stripe` cw), `:757` (`copy_planes_to_pic` cw/ch), `tests/zimg_test_util.h:57` (`zt_pic_alloc`). zimg_helpers.h has the same math buried inside `up_plane_pitch`/`up_plane_lines` but exposes no bare helper | Add `static inline int up_chroma_dim(int v, int sub)` to zimg_helpers.h; call from all three sites (and from plane_pitch/lines). One definition for the rounding. |
| DUP-7 | open | S | `scaler_zimg.c:776` `zimg_point_workers_src` and `:800` `zimg_zerocopy_point_workers` are structurally identical — same `swap`→`up_zimg_plane_idx` index map + same per-worker loop assigning `{y,u,v,pitch_y,pitch_c}` from `pic->p[idx]`; only the target sub-struct (`vlc_src` vs `dst`) differs. The new copy-in added the second copy | Extract one `point_workers_planes(p, pic, <which plane_set>)`. Ties to PERF-6 (merge the two per-frame loops). |

DUP-1 (triplicated identity copy) + DUP-4 (amount clamp) DONE — commit 0711d5f.
DUP-2 OBSOLETE: PERF-2 (cc71221) replaced the pool's two-phase loops with a
fused rolling-buffer sweep, so it no longer mirrors usm.h's pass1/pass2 — the
duplication is gone.

## architecture/modularity/SOLID

| id | status | effort | description | notes |
|----|--------|--------|-------------|-------|
| ARCH-1 | open | M | `scaler_zimg.c:608` `zimg_lazy_init` builds a `fake_ctx` (memset + copy 4 geometry fields) only to satisfy `construct_workers`/`try_spawn_one_worker`, which read only src/dst dims | Change the stripe helpers to take a small geometry struct (or `zimg_priv_t` directly), eliminating the fake-ctx and the scaler.h coupling. |
| ARCH-4 | low | S | `plane_set_t` on `stripe_worker_t` (`scaler_zimg.c:73,119-129`) carries three roles: scratch geometry (`.lines_*`, priv-level only), the worker's scratch view (`src`/`dst`), and per-frame VLC picture pointers (`vlc_src`). The parallel copy-in widened this overload by adding `vlc_src` | Mild SRP smell. Could split `plane_geom_t` (pitch+lines) vs `plane_ptrs_t` (y/u/v+pitch). Low value unless a third consumer appears. |

ARCH-2 (compile-time chroma fourcc cross-check vs VLC_CODEC_*) DONE — commit cb2efa9.

ARCH-3 (move zimg-only knobs into a `zimg` sub-struct) DONE — commit 0f92daa.

## decoupling

| id | status | effort | description | notes |
|----|--------|--------|-------------|-------|
| DEC-1 | rejected | S | `up_usm_pool_variant_name` is a loose `extern const char *` global with two definition sites by build mode | Rejected: an accessor `up_usm_pool_variant_name(void)` does NOT remove the two definition sites — there would still be one accessor body per build mode (dispatch.c vs usm_pool.c #else). It only swaps a global for a call, for no real decoupling, while rippling through 5 files incl tests. The misleading doc was the real issue, fixed by ABI-1 (commit ee09ed9). |
| DEC-2 | rejected | S | `usm_pool_dispatch.c` hand-maintains extern decls + forwarding shims that must stay in lockstep with `usm_pool.h` | Rejected with ABI-2: drift fails at link time (loud, not silent), and the variant tests link all three variants + the dispatcher. X-macro shim generation is the "clever shortcut" AGENTS.md steers away from; explicit list + link-time check preferred. |

## business/design patterns/DDD

| id | status | effort | description | notes |
|----|--------|--------|-------------|-------|
| PAT-2 | open | S | `zimg_process` (`scaler_zimg.c:757`) is an implicit pipeline (copy-in → point → dispatch → copy-out) with `if (dst_zerocopy)` scattered across phases | Documenting as a Template Method (fixed skeleton, zerocopy-varying steps) would make the two output paths explicit. Low value; keep unless a third output mode appears. |

PAT-1 (group dispatch fn-pointers into a usm_pool_ops_t vtable) DONE — commit 0f92daa.

## reliability/correctness

| id | status | effort | description | notes |
|----|--------|--------|-------------|-------|
| (none open) | | | REL-1 (probe pixel width) and REL-2 (backend id vs name[0]) fixed in commit 0f92daa. | |

## error handling

| id | status | effort | description | notes |
|----|--------|--------|-------------|-------|
| (none open) | | | ERR-2 / RES-1 (sws_scale short-return) fixed in commit 83f5ee0 (require `rc == ctx->dst_h`). | |

ERR-1 (sem leak on partial spawn) DONE — fixed during the PERF-2 rewrite (commit cc71221) and now covered by the RLIMIT_NPROC test (commit 668650d).

## portability/standards conformance

| id | status | effort | description | notes |
|----|--------|--------|-------------|-------|
| PORT-3 | no-action | S | `perfmon.h:96` EWMA update right-shifts a signed `diff` | Well-defined arithmetic shift on every twos-complement target (all real ABIs); already documented in the file. No change unless a non-twos-complement target appears. Kept as a known, accepted item. |

PORT-2 (dispatcher x86-only) DONE — commit ee09ed9 (#error on !__x86_64__).

## resource management

| id | status | effort | description | notes |
|----|--------|--------|-------------|-------|
| RES-2 | open | M | `scaler_zimg.c` + `usm_pool.c` each filter instance spawns up to `UP_THREADS_MAX` (64) zimg workers + up to 64 USM workers + multi-MB scratch, with no process-wide cap across concurrent AutoUpscale instances | If multiple concurrent instances are possible, add an aggregate thread/memory budget; else document the per-instance bound as intentional. NB the two pools run sequentially per frame (see rejected SCAL-1), so the cost is idle-thread address space, not CPU. |

RES-1 (sws_scale `rc > 0`) DONE — folded into the ERR-2 fix (commit 83f5ee0).

## API/ABI stability

| id | status | effort | description | notes |
|----|--------|--------|-------------|-------|
| ABI-2 | rejected | S | USM pool public symbol set hand-mirrored across `usm_pool.h`, dispatcher externs, and test files with only a comment to keep in sync | Rejected (with DEC-2): drift is ALREADY caught loudly — adding/removing a public fn without updating the dispatcher fails at link (unresolved/orphaned symbol), and the variant tests link all three + the dispatcher. An X-macro to auto-generate the shims is the "clever indirection" AGENTS.md steers away from; the explicit list + link-time safety net is preferred. |

ABI-1 (false "weak alias" comment) DONE — commit ee09ed9 (corrected to describe the two mutually-exclusive strong defs).

## build/toolchain hygiene

| id | status | effort | description | notes |
|----|--------|--------|-------------|-------|
| BUILD-1 | open | M | Generic object rule `Makefile:184` omits included headers (`content_probe.h`, `chroma_classify.h`, `zimg_helpers.h`, `scaler_zimg_chroma.h`, `scaler_pick_logic.h`) | Editing those headers does not rebuild dependent TUs → stale incremental builds. Add `-MMD -MP` + `-include $(OBJS:.o=.d)` for auto header deps. |
| BUILD-1 | open | M | Generic object rule `Makefile:184` omits included headers (`content_probe.h`, `chroma_classify.h`, `zimg_helpers.h`, `scaler_zimg_chroma.h`, `scaler_pick_logic.h`) | Editing those headers does not rebuild dependent TUs → stale incremental builds. Add `-MMD -MP` + `-include $(OBJS:.o=.d)` for auto header deps. (M effort.) |
| BUILD-2 | partial | S | cppcheck excludes `autoupscale.c` and `scaler_zimg.c` (can't parse VLC's macro headers); shipped `.so` analysis gap | Partially addressed: CI now builds the plugin under gcc AND clang `-Werror` and runs scaler_zimg.c through the ASan/UBSan/TSan harness (commit 47d2c40) — stronger than cppcheck for that file. cppcheck-on-VLC-TUs still out (header parsing); `-fanalyzer` deferred (noisy on VLC headers). |
| BUILD-5 | open | S | `MARCH ?= native` + `MULTIVERSION ?= 0` (`Makefile:63,110`) makes the default `.so` non-portable and prone to SIGILL on a different CPU | Acceptable per the build-and-run model, but no runtime guard; a `__builtin_cpu_supports` self-check at Open() (or portable baseline for release artifacts) would fail gracefully. |
| BUILD-6 | open | S | `bench-zimg` (`Makefile`) only ever runs with zc (zerocopy) = 1; never benchmarks zc=0, which is the ONLY path with serial work (copy-out, PERF-5) | The "vehicle for measuring PERF-1" can't see the copy-out cost it should measure. Add a zc=0 row (e.g. `bench_scaler_zimg 8 i420 854 480 1920 1080 200 0`). |
| BUILD-7 | info | S | `bench` / `bench-zimg` binaries are built nowhere in CI (only `test-zimg`/`stress-zimg` are wired); a bench-only compile break (e.g. `zt_ctx_init` signature drift) lands silently | Add a compile-only `make build/bench_scaler_zimg` step to CI, or accept benches as dev-only and document it. (coverage-zimg correctly left out — informational, sub-100%.) |

BUILD-3 (-Werror) + BUILD-4 (clang plugin build) DONE — commit 47d2c40; CI
also now installs libzimg and runs the zimg harness (test-zimg / stress-zimg).

## observability

| id | status | effort | description | notes |
|----|--------|--------|-------------|-------|
| OBS-3 | open | S | No counters for frames processed/dropped, USM-skipped, or achieved fps; only signal is the one-shot perf advisory (`autoupscale.c:550`) | Add periodic `msg_Dbg` (every N seconds) with processed/dropped counts and current EWMA so long-run behavior is observable. |
| OBS-4 | low | S | The serial copy-out path (PERF-5) ships with zero runtime visibility — `log_zimg_open` reports geometry/scratch once at open, nothing per-frame | When OBS-3's periodic counters land, include a copy-out-µs accumulator (zerocopy-off only) so the serial tail is observable. |

OBS-1 (one-shot warn on process failure) + OBS-2 (msg_Err on lazy-init failure)
DONE — commit fe9396f.

## wiring gaps

| id | status | effort | description | notes |
|----|--------|--------|-------------|-------|
| (none open) | | | WIRE-1 (flat-skip wired + exercised via `make bench-flatskip`, commit cc71221) and WIRE-2 (oracle documented, commit 0f92daa) resolved. | |

## unused functions/methods

| id | status | effort | description | notes |
|----|--------|--------|-------------|-------|
| DEAD-1,2,3,5 | keep | S | `up_usm_workspace_size`, `up_usm__pass1_hblur`, `up_usm__pass2_combine`, `up_usm__args_valid` (usm.h) are reachable only via `up_usm_apply_plane` | DECISION: keep. They are the single-threaded byte-identity TEST ORACLE for the threaded pool (documented at up_usm_apply_plane via WIRE-2, commit 0f92daa). Not dead — intentionally test-only. |
| DEAD-7 | keep | S | `scaler_zimg.c` `construct_workers` partial-build retry + `teardown_constructed_workers` are unreachable in practice: `zimg_open` clamps stripes to >=16 dst rows and source-stripe degeneracy is all-or-nothing, so `0 < constructed < n` never occurs | DECISION: keep as a cheap defensive net against future stripe-bounds changes. Not a correctness bug. |

DEAD-4 RESOLVED: after DUP-1 (commit 0711d5f) the threaded pool calls
`up_usm__apply_identity` directly, so it now has a production caller — no
longer dead.

DEAD-6 (orphaned bench_usm_pool.c) DONE — wired as `make bench` / `make bench-flatskip` and its timing loop fixed (commit cc71221).

## Open — parked

Deferred deliberately; why-not-now recorded so a future pass doesn't treat
them as forgotten. The zimg test/bench harness blocker is now REMOVED
(commit 1c53e19: `make test-zimg` / `stress-zimg` / `bench-zimg`), so these
can now be implemented with ASan/UBSan/TSan + throughput validation. Parked
now only on scope, not on lack of a safety net.

| id | effort | description | why parked |
|----|--------|-------------|------------|
| SCAL-3 | S | 2D/column tiling for very wide/short zimg frames | Column tiling subdivides WIDTH; horizontal resampling across a column-tile boundary risks visible vertical seams (horizontal-stripe-only design avoids this). Needs zimg halo/overlap; the harness only checks full-write/determinism/zerocopy, NOT seam-free quality — would need a perceptual/reference check added. Low priority for typical content. |
| SCAL-2 (zimg half) | S | Replace zimg's per-frame N sem_post/sem_wait with a counting barrier | Unblocked by the harness (TSan covers the race surface). USM half already done (commit cc71221). |

PERF-1 (parallel copy-in) DONE — commit b0d327f, measured -12% to -23% at
4-16 threads, validated by the harness under ASan/UBSan/TSan.

CON-1 fully resolved: commit 8aa9104 only documented should_exit/result as
sem-safe; the zimg harness's TSan build caught a real double-write race on
should_exit (close path signalled twice). Fixed properly in commit 3a35a23 by
signalling each worker exactly once (signal-then-reap), so should_exit stays a
plain sem-synchronized int — matching usm_pool's pattern (the interim _Atomic
in 1c53e19 was reverted).

## Audit picks deliberately rejected

Kept here so future passes don't re-pick them.

| id | why rejected |
|----|--------------|
| SCAL-1 | "zimg pool + USM pool double the live thread count and oversubscribe." Premise is wrong: the two pools run SEQUENTIALLY within a frame — `Filter()` runs `scaler.process()` (zimg workers) to completion, THEN `ApplyUsmIfEnabled()` (USM workers). They never execute concurrently, so the idle pool's workers consume zero CPU and negligible RSS (untouched stacks). The suggested "subtract the USM count from the zimg budget" would halve each phase's parallelism for no benefit. A true single shared pool is a large refactor for ~nil gain. The only residual cost (idle-thread address space across many concurrent instances) is tracked under RES-2. Decided 2026-05-30. |

Coverage note: pure-logic files are 100% (gated). scaler_zimg.c is exercised
to ~94% by `make coverage-zimg` (was 0%); the rest needs a live VLC logger
(log_zimg_open) or fault injection and is intentionally ungated.
