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

## undefined behavior

| id | status | effort | description | notes |
|----|--------|--------|-------------|-------|
| (none open) | | | | |

## memory management

| id | status | effort | description | notes |
|----|--------|--------|-------------|-------|
| MEM-2 | open | S | Ensure `aligned_alloc` size is a multiple of alignment | C11 requirement; check `usm_pool.c` and `scaler_zimg.c` calls to avoid UB. |

## performance

| id | status | effort | description | notes |
|----|--------|--------|-------------|-------|

## scalability

| id | status | effort | description | notes |
|----|--------|--------|-------------|-------|
| SCAL-2 | open | S | `scaler_zimg.c:732` per-frame dispatch issues N `sem_post`/`sem_wait` pairs per phase from the main thread; sync cost grows linearly with thread count | USM path HALVED in cc71221 (fused to one dispatch). zimg dispatch half remains: a counting barrier / futex gate would cut its per-frame syscalls. Parked with PERF-1 (no zimg test harness). |
| SCAL-4 | open | M | Worker thread affinity / core pinning | Reduce cache thrashing and context-switch overhead on high-core-count/NUMA systems. |

## concurrency

| id | status | effort | description | notes |
|----|--------|--------|-------------|-------|
| CON-2 | open | M | `scaler_zimg.c:766` `zimg_lazy_init()` keys off non-atomic `lazy_init_done` on first `zimg_process()` with no guard against concurrent `Filter()` entry (TOCTOU) | Safe while VLC drives one instance serially; add assert/comment or `pthread_once` guard if ever shared across threads. |

## code complexity

| id | status | effort | description | notes |
|----|--------|--------|-------------|-------|
| (none found) | | | `lizard src -C 10` reports no thresholds exceeded; max measured CCN is 10 (`ChromaToAVFmt@scaler_swscale.c:29`). Gate passes clean. | |

## code duplication

| id | status | effort | description | notes |
|----|--------|--------|-------------|-------|
| DUP-3 | open | S | Worker-pool lifecycle (lazy-init flags, aligned_alloc + memset, sem_init, spawn loop with `constructed`, sticky `lazy_init_failed`) duplicated between `scaler_zimg.c:584` and `usm_pool.c:316`; per-worker `_Alignas(64)` struct + rationale comment copy-pasted | Small shared worker-pool scaffold; low priority since the two payloads (zimg graphs vs scratch rows) differ. |
| DUP-5 | open | S | Args-valid checks parallel: `usm_pool.c` (`usm_pool_validate_args`) vs `usm.h` (`up_usm__args_valid`) — both null dst/src + stride<width | Marginal: the pool variant also checks its own ptr and uses the stored `p->width`, while `up_usm__args_valid` validates full dims; not cleanly mergeable without threading width/height through. Keep. |

## architecture/modularity/SOLID

| id | status | effort | description | notes |
|----|--------|--------|-------------|-------|
| ARCH-1 | open | M | `scaler_zimg.c:608` `zimg_lazy_init` builds a `fake_ctx` (memset + copy 4 geometry fields) only to satisfy `construct_workers`/`try_spawn_one_worker`, which read only src/dst dims | Change the stripe helpers to take a small geometry struct (or `zimg_priv_t` directly), eliminating the fake-ctx and the scaler.h coupling. |
| ARCH-4 | low | S | `plane_set_t` on `stripe_worker_t` (`scaler_zimg.c:73,119-129`) carries three roles: scratch geometry (`.lines_*`, priv-level only), the worker's scratch view (`src`/`dst`), and per-frame VLC picture pointers (`vlc_src`). The parallel copy-in widened this overload by adding `vlc_src` | Mild SRP smell. Could split `plane_geom_t` (pitch+lines) vs `plane_ptrs_t` (y/u/v+pitch). Low value unless a third consumer appears. |

## decoupling

| (none open) | | | | |

## business/design patterns/DDD

| id | status | effort | description | notes |
|----|--------|--------|-------------|-------|
| PAT-2 | open | S | `zimg_process` (`scaler_zimg.c:757`) is an implicit pipeline (copy-in → point → dispatch → copy-out) with `if (dst_zerocopy)` scattered across phases | Documenting as a Template Method (fixed skeleton, zerocopy-varying steps) would make the two output paths explicit. Low value; keep unless a third output mode appears. |

PAT-1 (group dispatch fn-pointers into a usm_pool_ops_t vtable) DONE — commit 0f92daa.

## reliability/correctness

| id | status | effort | description | notes |
|----|--------|--------|-------------|-------|
| REL-3 | open | S | Runtime probe of zimg version and features | Verify library capabilities at Open() to fail gracefully if the environment changes. |

## error handling

| id | status | effort | description | notes |
|----|--------|--------|-------------|-------|
| (none found) | | | | |

## portability/standards conformance

| id | status | effort | description | notes |
|----|--------|--------|-------------|-------|
| PORT-3 | no-action | S | `perfmon.h:96` EWMA update right-shifts a signed `diff` | Well-defined arithmetic shift on every twos-complement target (all real ABIs); already documented in the file. No change unless a non-twos-complement target appears. Kept as a known, accepted item. |
| PORT-4 | open | S | Use `stdalign.h` for `_Alignas` / `alignas` compatibility | Improve portability across C11-compliant compilers. |

## resource management

| id | status | effort | description | notes |
|----|--------|--------|-------------|-------|
| RES-2 | open | M | `scaler_zimg.c` + `usm_pool.c` each filter instance spawns up to `UP_THREADS_MAX` (64) zimg workers + up to 64 USM workers + multi-MB scratch, with no process-wide cap across concurrent AutoUpscale instances | If multiple concurrent instances are possible, add an aggregate thread/memory budget; else document the per-instance bound as intentional. NB the two pools run sequentially per frame (see rejected SCAL-1), so the cost is idle-thread address space, not CPU. |

## API/ABI stability

| id | status | effort | description | notes |
|----|--------|--------|-------------|-------|
| (none open) | | | | |

## build/toolchain hygiene

| id | status | effort | description | notes |
|----|--------|--------|-------------|-------|
| BUILD-2 | partial | S | cppcheck excludes `autoupscale.c` and `scaler_zimg.c` (can't parse VLC's macro headers); shipped `.so` analysis gap | Partially addressed: CI builds with `-Werror`, runtime harness exists, and `scan-build` target added for static analysis. `-fanalyzer` remains deferred (noisy). |

## observability

| id | status | effort | description | notes |
|----|--------|--------|-------------|-------|
| OBS-3 | open | S | No counters for frames processed/dropped, USM-skipped, or achieved fps; only signal is the one-shot perf advisory (`autoupscale.c:550`) | Add periodic `msg_Dbg` (every N seconds) with processed/dropped counts and current EWMA so long-run behavior is observable. |
| OBS-4 | low | S | The serial copy-out path (PERF-5) ships with zero runtime visibility — `log_zimg_open` reports geometry/scratch once at open, nothing per-frame | When OBS-3's periodic counters land, include a copy-out-µs accumulator (zerocopy-off only) so the serial tail is observable. |

## wiring gaps

| id | status | effort | description | notes |
|----|--------|--------|-------------|-------|
| (none open) | | | WIRE-1 (flat-skip wired + exercised via `make bench-flatskip`, commit cc71221) and WIRE-2 (oracle documented, commit 0f92daa) resolved. | |

## unused functions/methods

| id | status | effort | description | notes |
|----|--------|--------|-------------|-------|
| DEAD-1,2,3,5 | keep | S | `up_usm_workspace_size`, `up_usm__pass1_hblur`, `up_usm__pass2_combine`, `up_usm__args_valid` (usm.h) are reachable only via `up_usm_apply_plane` | DECISION: keep. They are the single-threaded byte-identity TEST ORACLE for the threaded pool (documented at up_usm_apply_plane via WIRE-2, commit 0f92daa). Not dead — intentionally test-only. |
| DEAD-7 | keep | S | `scaler_zimg.c` `construct_workers` partial-build retry + `teardown_constructed_workers` are unreachable in practice: `zimg_open` clamps stripes to >=16 dst rows and source-stripe degeneracy is all-or-nothing, so `0 < constructed < n` never occurs | DECISION: keep as a cheap defensive net against future stripe-bounds changes. Not a correctness bug. |

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

Coverage note: pure-logic files are 100% (gated). scaler_zimg.c is exercised
to ~94% by `make coverage-zimg` (was 0%); the rest needs a live VLC logger
(log_zimg_open) or fault injection and is intentionally ungated.
