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
| (none found) | | | All `aligned_alloc` seams verified C11-conformant: sizes are multiples of alignment by construction — `up_round_up_pitch` rounds pitches to `UP_PITCH_ALIGN` (so `lines*pitch` is a multiple), scratch/worker blocks round explicitly or via `_Alignas(64)`, and VLC picture pitches never reach `aligned_alloc`. | |

## performance

| id | status | effort | description | notes |
|----|--------|--------|-------------|-------|

## scalability

| id | status | effort | description | notes |
|----|--------|--------|-------------|-------|
| (none open) | | | SCAL-2 FULLY RESOLVED. Per-frame main-thread dispatch is now O(1) syscalls both ways: WAKE side bumps a shared `generation` under a mutex + one `pthread_cond_broadcast` (was N `sem_post(go)`); DONE side is a counting barrier — workers decrement an atomic `pending`, the last posts a single `all_done` the main waits on once (was N `sem_wait`). `should_exit` is now mutex-protected (set + broadcast in `zimg_wake_all_for_exit`). Validated race-free + byte-identical under the ASan/UBSan and TSan harnesses; bench-zimg shows near-linear 1->16 thread scaling. |

## concurrency

| id | status | effort | description | notes |
|----|--------|--------|-------------|-------|
| (none open) | | | CON-2 RESOLVED by contract doc: the non-atomic `lazy_init_done`/`lazy_init_failed` check-then-act is sound under VLC's serial-per-instance `Filter()` contract, now stated at the field declaration with the pthread_once/`_Atomic` escape hatch if the scaler is ever shared across threads within one instance. | |

## code complexity

| id | status | effort | description | notes |
|----|--------|--------|-------------|-------|
| (none found) | | | `lizard src -C 10` reports no thresholds exceeded; max measured CCN is 10 (`ChromaToAVFmt@scaler_swscale.c:29`). Gate passes clean. | |

## code duplication

| id | status | effort | description | notes |
|----|--------|--------|-------------|-------|
| DUP-3 | keep | S | Worker-pool lifecycle (lazy-init flags, aligned_alloc + memset, sem_init, spawn loop with `constructed`, sticky `lazy_init_failed`) duplicated between `scaler_zimg.c` and `usm_pool.c`; per-worker `_Alignas(64)` struct + rationale comment copy-pasted | DECISION (2026-05-30): keep. A shared scaffold needs a type-erased pool (void* element + per-worker construct/destroy callbacks) since the worker structs and per-worker work differ (per-stripe zimg graphs vs scratch rows). The common part is ~15 lines of alloc+memset+spawn; hiding it behind a callback interface across a module boundary adds indirection while the real work stays divergent — net clarity loss. Per AGENTS.md (SOLID only when it improves clarity). |
| DUP-5 | open | S | Args-valid checks parallel: `usm_pool.c` (`usm_pool_validate_args`) vs `usm.h` (`up_usm__args_valid`) — both null dst/src + stride<width | Marginal: the pool variant also checks its own ptr and uses the stored `p->width`, while `up_usm__args_valid` validates full dims; not cleanly mergeable without threading width/height through. Keep. |

## architecture/modularity/SOLID

| id | status | effort | description | notes |
|----|--------|--------|-------------|-------|
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
| (none open) | | | REL-3 (runtime zimg API major-version probe in `zimg_open`, fails graceful on ABI mismatch) DONE — commit pending. | |

## error handling

| id | status | effort | description | notes |
|----|--------|--------|-------------|-------|
| (none found) | | | | |

## portability/standards conformance

| id | status | effort | description | notes |
|----|--------|--------|-------------|-------|
| PORT-3 | no-action | S | `perfmon.h:96` EWMA update right-shifts a signed `diff` | Well-defined arithmetic shift on every twos-complement target (all real ABIs); already documented in the file. No change unless a non-twos-complement target appears. Kept as a known, accepted item. |
| (none open beyond PORT-3) | | | PORT-4 (`<stdalign.h>` + `alignas` over the `_Alignas` keyword in `usm_pool.c`/`scaler_zimg.c`) DONE — commit pending. | |

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
| BUILD-2 | partial | S | cppcheck excludes `autoupscale.c` and `scaler_zimg.c` (can't parse VLC's macro headers); shipped `.so` analysis gap | Partially addressed: CI builds with `-Werror`, runtime harness exists, and `scan-build` target added for static analysis. `-fanalyzer` remains deferred (noisy). |

## observability

| id | status | effort | description | notes |
|----|--------|--------|-------------|-------|
| OBS-4 | low | M | The serial copy-out path (PERF-5) ships with zero runtime visibility — `log_zimg_open` reports geometry/scratch once at open, nothing per-frame | Deferred: needs a per-frame metric channel through the scaler `process()` API (e.g. `last_copyout_ns` on `scaler_ctx_t`) plus hot-path timing guarded on zerocopy-off. Re-scoped S→M; low value vs. interface change. OBS-3 (periodic frames/dropped/EWMA `msg_Dbg`) DONE — commit pending. |

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
| SCAL-4 | M | Worker thread affinity / core pinning | Parked pending EVIDENCE: no NUMA/high-core throughput bench exists to prove a gain, and pinning short-lived per-frame workers can HARM by fighting VLC's own threads and the OS scheduler's load balancing on the typical desktop. Non-portable (`pthread_setaffinity_np`). Revisit with a real multi-socket bench before adding speculative pinning. |

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
