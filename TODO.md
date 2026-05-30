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
| SEC-1 | open | S | `autoupscale.c:301` `(unsigned long)info.totalram * info.mem_unit` can overflow `unsigned long` on 32-bit hosts, producing a bogus `mem_mb` that drives the AUTO 720p/1080p decision | Compute in `uint64_t` and saturate before narrowing to `mem_mb`. |

## undefined behavior

| id | status | effort | description | notes |
|----|--------|--------|-------------|-------|
| (none open) | | | UB-1/PORT-1 (`pthread_t != 0`) and UB-2 (unbounded `>> sub_h`) fixed in commit 8aa9104. | |

## memory management

| id | status | effort | description | notes |
|----|--------|--------|-------------|-------|
| (none found) | | | Overflow guards (`plane_alloc_bytes`, `up_usm_workspace_size`, `up_round_up_pitch`) and lazy-init partial-failure cleanup (deferred to `close`/`destroy` via persisted `ctx->priv`/pool ptr) all verified sound. | |

## performance

| id | status | effort | description | notes |
|----|--------|--------|-------------|-------|
| PERF-4 | open | S | `usm.h:86-89,195-198` hot kernels rely on `#pragma GCC optimize("O3")` for autovectorization | Pragma opt-level is brittle across gcc versions and no-ops if inlining differs; compile the TU at -O3 (as usm_pool.o already is) or hand-write intrinsics for `combine_row`. |

PERF-2 (fuse USM passes) DONE — commit cc71221, measured 3-21% faster.
PERF-3 (probe visible width) DONE — commit 0f92daa. PERF-1 parked (below).

## scalability

| id | status | effort | description | notes |
|----|--------|--------|-------------|-------|
| SCAL-1 | open | M | `scaler_zimg.c:635` + `autoupscale.c:401` zimg pool and USM pool each independently call `up_threads_decide()`; with USM on a frame can hold up to 2×(cores/2−2) worker threads | Share one thread budget across scale+USM, or subtract the USM count from the zimg budget. See RES-2. |
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
| DUP-1 | open | M | Identity-copy plane loop triplicated: `usm_pool.c:387` (`usm_pool_identity`), `usm.h:142` (`up_usm__apply_identity`), `zimg_helpers.h:121` (`up_copy_plane`) — same unified-stride-memcpy-else-per-row pattern | Have the first two delegate to `up_copy_plane` (header-only, no VLC deps); collapses 3 copies to 1. Ties to DEAD-4. |
| DUP-2 | open | S | Pass-1 hblur / pass-2 combine row loops duplicated between single-threaded `usm.h` (`up_usm__pass1_hblur:171`, `up_usm__pass2_combine:228`) and threaded `usm_pool.c` (`usm_worker_phase0_hblur:177`, `usm_worker_phase1_combine:226`) | Worker phases could call the `up_usm__pass*` helpers over their own sub-range; single source for kernel orchestration. |
| DUP-3 | open | S | Worker-pool lifecycle (lazy-init flags, aligned_alloc + memset, sem_init, spawn loop with `constructed`, sticky `lazy_init_failed`) duplicated between `scaler_zimg.c:584` and `usm_pool.c:316`; per-worker `_Alignas(64)` struct + rationale comment copy-pasted | Small shared worker-pool scaffold; low priority since payloads differ. |
| DUP-4 | open | S | Amount clamp `[0, UP_USM_AMOUNT_Q8_MAX]` duplicated: `usm_pool.c:452` (`usm_pool_clamp_amount`) and inline `usm.h:273-274` (`up_usm_apply_plane`) | Extract `up_usm__clamp_amount_q8()` inline in usm.h, call from both. |
| DUP-5 | open | S | Args-valid checks parallel: `usm_pool.c:441` (`usm_pool_validate_args`) vs `usm.h:251` (`up_usm__args_valid`) — both null dst/src + stride<width | Pool variant could reuse `up_usm__args_valid` (header-only). Minor. |

## architecture/modularity/SOLID

| id | status | effort | description | notes |
|----|--------|--------|-------------|-------|
| ARCH-1 | open | M | `scaler_zimg.c:608` `zimg_lazy_init` builds a `fake_ctx` (memset + copy 4 geometry fields) only to satisfy `construct_workers`/`try_spawn_one_worker`, which read only src/dst dims | Change the stripe helpers to take a small geometry struct (or `zimg_priv_t` directly), eliminating the fake-ctx and the scaler.h coupling. |
| ARCH-2 | open | S | Constant duplication across header boundaries: `SCALER_PICK_*` (`scaler_pick_logic.h:30`) mirrors `SCALER_BACKEND_*` (`scaler.h:21`, guarded only by `_Static_assert` in `scaler.c:21`); chroma fourcc literals in `chroma_classify.h` mirror VLC's `VLC_CODEC_*` | Accepted/documented pattern but drift risk; add a compile-time cross-check for chroma fourccs vs `VLC_CODEC_*` in a VLC-linked TU. |

ARCH-3 (move zimg-only knobs into a `zimg` sub-struct) DONE — commit 0f92daa.

## decoupling

| id | status | effort | description | notes |
|----|--------|--------|-------------|-------|
| DEC-1 | open | S | `up_usm_pool_variant_name` is a loose `extern const char *` global (`usm_pool_dispatch.c:75` / `usm_pool.c:58` / `usm_pool.h:103`) with two definition sites by build mode | Hide the single/multi split behind an accessor `up_usm_pool_variant_name(void)`. See ABI-1. |
| DEC-2 | open | S | `usm_pool_dispatch.c` hand-maintains 9 extern decls + 3 forwarding shims that must stay in lockstep with `usm_pool.h` (header carries a large "IF YOU ADD/REMOVE" warning) | Generate the shims via an X-macro list so add/remove touches one line. See ABI-2. |

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
| ERR-2 | open | S | `scaler_swscale.c:116-118` `sws_scale` result only checked `rc > 0`; a short return (`0 < rc < ctx->dst_h`) is treated as full success | Validate `rc` against expected output slice height; fail on short writes so a partially-filled frame is never emitted. |

ERR-1 (sem leak on partial spawn) DONE — fixed during the PERF-2 rewrite (commit cc71221) and now covered by the RLIMIT_NPROC test (commit 668650d).

## portability/standards conformance

| id | status | effort | description | notes |
|----|--------|--------|-------------|-------|
| PORT-2 | open | S | `usm_pool_dispatch.c:92-116` dispatcher is x86-only: the `#if defined(__GNUC__)||defined(__clang__)` guards only AVX probes, the unconditional `#else` still wires `_sse2` variant symbols | Guard the whole dispatcher on `defined(__x86_64__)` and provide a plain-`default` link path for non-x86. |
| PORT-3 | open | S | `perfmon.h:96` EWMA update right-shifts a signed `diff` | Implementation-defined for negatives pre-C11 (well-defined on all twos-complement targets). Already documented; no change unless targeting non-twos-complement ABI. |

## resource management

| id | status | effort | description | notes |
|----|--------|--------|-------------|-------|
| RES-1 | open | S | `scaler_swscale.c:118` `sws_scale` result treated as success only when `rc > 0`; a legitimate `0` return drops the frame (no leak — both pictures released — but a robustness gap) | Accept `rc >= 0`, or document why `0` cannot occur for a full-height scale. Overlaps ERR-2. |
| RES-2 | open | M | `scaler_zimg.c` + `usm_pool.c` each filter instance spawns up to `UP_THREADS_MAX` (64) zimg workers + up to 64 USM workers + multi-MB scratch, with no process-wide cap across concurrent AutoUpscale instances | If multiple concurrent instances are possible, add an aggregate thread/memory budget; else document the per-instance bound as intentional. See SCAL-1. |

## API/ABI stability

| id | status | effort | description | notes |
|----|--------|--------|-------------|-------|
| ABI-1 | open | S | `up_usm_pool_variant_name` documented as a "weak alias" in `usm_pool.h:103` but defined *strongly* in both `usm_pool.c:58` and `usm_pool_dispatch.c:75` | No clash today (single-baseline def is `#ifdef USM_VARIANT`-guarded) but the header contract is wrong; a future non-variant link alongside the dispatcher = duplicate-symbol error. Fix comment or make one def `__attribute__((weak))`. See DEC-1. |
| ABI-2 | open | S | USM pool public symbol set hand-mirrored across `usm_pool.h:25-42`, dispatcher externs, and test files with only a comment to keep in sync | Add/remove a public fn → `.so` fails at load (unresolved symbol). Generate shims from one X-macro list or add a CI link check. See DEC-2. |

## build/toolchain hygiene

| id | status | effort | description | notes |
|----|--------|--------|-------------|-------|
| BUILD-1 | open | M | Generic object rule `Makefile:184` omits included headers (`content_probe.h`, `chroma_classify.h`, `zimg_helpers.h`, `scaler_zimg_chroma.h`, `scaler_pick_logic.h`) | Editing those headers does not rebuild dependent TUs → stale incremental builds. Add `-MMD -MP` + `-include $(OBJS:.o=.d)` for auto header deps. |
| BUILD-2 | open | S | cppcheck explicitly excludes `autoupscale.c` and `scaler_zimg.c` (`Makefile:493-499`); shipped `.so` never built under ASan/UBSan or `-fanalyzer` | The two largest pointer-heavy TUs get zero static/dynamic analysis; add a `scan-build`/`-fanalyzer` plugin build or run cppcheck with the VLC include path on them in CI. |
| BUILD-3 | open | S | CI uses `-Wall -Wextra -Wshadow ...` but never `-Werror` (`Makefile:40`) | Warning regressions pass CI silently; add `EXTRA_CFLAGS=-Werror` to build/test jobs. |
| BUILD-4 | open | S | Production `.so` only ever compiled with gcc; clang used solely for fuzz/stress (`Makefile:17-18`) | clang-only warnings and the clang hot-kernel codegen path (which `usm.h` relies on) are untested for the shipped artifact; add a clang plugin build to CI. |
| BUILD-5 | open | S | `MARCH ?= native` + `MULTIVERSION ?= 0` (`Makefile:63,110`) makes the default `.so` non-portable and prone to SIGILL on a different CPU | Acceptable per the build-and-run model, but no runtime guard; a `__builtin_cpu_supports` self-check at Open() (or portable baseline for release artifacts) would fail gracefully. |

## observability

| id | status | effort | description | notes |
|----|--------|--------|-------------|-------|
| OBS-1 | open | S | Per-frame process failures drop the frame with no log: `scaler_zimg.c:780`, `autoupscale.c:703-708`, `scaler_swscale.c:118` | A persistently failing scaler yields a black/stalled stream with nothing logged; emit a one-time (rate-limited) `msg_Warn` on first failure. |
| OBS-2 | open | S | `zimg_lazy_init` failure returns -1 silently at first frame (`scaler_zimg.c:766-772`) | The expensive setup (threads/scratch) can fail after a successful cheap Open(); add an explicit `msg_Err` via `log_obj_saved` so users see why frames drop. |
| OBS-3 | open | S | No counters for frames processed/dropped, USM-skipped, or achieved fps; only signal is the one-shot perf advisory (`autoupscale.c:550`) | Add periodic `msg_Dbg` (every N seconds) with processed/dropped counts and current EWMA so long-run behavior is observable. |

## wiring gaps

| id | status | effort | description | notes |
|----|--------|--------|-------------|-------|
| (none open) | | | WIRE-1 (flat-skip wired + exercised via `make bench-flatskip`, commit cc71221) and WIRE-2 (oracle documented, commit 0f92daa) resolved. | |

## unused functions/methods

| id | status | effort | description | notes |
|----|--------|--------|-------------|-------|
| DEAD-1 | open | S | `up_usm_workspace_size` (`usm.h:60`) called only by tests (test_usm.c, fuzz_usm.c); no production caller (pool sizes its workspace inline in `usm_pool_lazy_init`) | keep: small pure helper used by reference-path tests; harmless. Inline into tests if reference path retired. |
| DEAD-2 | open | S | `up_usm__pass1_hblur` (`usm.h:171`) reachable only via `up_usm_apply_plane` (no production caller) | inline/keep with WIRE-2; lives or dies with the single-threaded reference. |
| DEAD-3 | open | S | `up_usm__pass2_combine` (`usm.h:228`) reachable only via `up_usm_apply_plane` | inline/keep with WIRE-2. |
| DEAD-4 | open | S | `up_usm__apply_identity` (`usm.h:142`) reachable only via `up_usm_apply_plane`; usm_pool.c references it only in a comment (has its own `usm_pool_identity`) | keep with WIRE-2; logic duplicated at `usm_pool.c:387` — dedup target (see DUP-1). |
| DEAD-5 | open | S | `up_usm__args_valid` (`usm.h:251`) reachable only via `up_usm_apply_plane` | inline/keep with WIRE-2. |

DEAD-6 (orphaned bench_usm_pool.c) DONE — wired as `make bench` / `make bench-flatskip` and its timing loop fixed (commit cc71221).

## Open — parked

Deferred deliberately; why-not-now recorded so a future pass doesn't treat
them as forgotten.

| id | effort | description | why parked |
|----|--------|-------------|------------|
| PERF-1 | M | Parallelize `zimg_copy_in` (serial source memcpy each frame) across the worker pool | scaler_zimg.c has NO automated test/bench harness (needs libzimg + real VLC pictures), and the file header documents that worker threads reading VLC's pool-managed source buffers is exactly what segfaulted in the field. Changing the copy model blind is unsafe. Needs a zimg correctness + throughput harness first. |
| SCAL-3 | S | 2D/column tiling for very wide/short zimg frames | Column tiling subdivides the WIDTH dimension; horizontal resampling across a column-tile boundary produces visible vertical seams (the horizontal-stripe-only design exists precisely to avoid this). Needs zimg overlap/halo handling + an image-quality harness to validate no seams. Finding itself rates it low priority for typical content. |
| SCAL-2 (zimg half) | S | Replace zimg's per-frame N sem_post/sem_wait with a counting barrier | Same blocker as PERF-1: no zimg test harness. The USM half is already done (single fused dispatch, commit cc71221). |
