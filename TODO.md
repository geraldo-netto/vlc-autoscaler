# TODO — full-project audit findings

Full rescan of HEAD `9903371` on 2026-07-10 (5 parallel category audits, every src/ file read
in full, tests/ + Makefile + CI inspected; all findings verified against actual code paths).
IDs restart at this rescan — they do not correlate with pre-rescan IDs in git history.
Row format: `id | status | effort | description | notes`.

## security

No open findings. Verified: config ints saturated (`InheritIntSat`) + clamped (`ClampConfig`);
frame geometry validated centrally by `up_picture_view_init` before any pixel access;
allocation-size arithmetic overflow-checked at every seam; all format strings literal.

## undefined behavior

| id | status | effort | description | notes |
|---|---|---|---|---|
| UB-2 | open | S | `tests/test_usm_pool_variants.c:288-298` `check_pattern` mallocs `dst1/dst2/dst3` unchecked and ignores `run_sse2/avx2/avx512` return codes; on OOM or variant pool-create failure `memcmp` reads uninitialized or NULL buffers. | Sibling `fuzz_usm_variants.c:159-182` does this correctly. Add alloc checks + rc checks (fail the test explicitly). |
| UB-3 | open | S | `tests/test_scaler_zimg.c` — `test_full_write`, `test_determinism`, `test_zerocopy_matches_copyout`, `test_src_zerocopy_matches_copy`, `test_pin_cpus_matches` declare `zt_pic_t a, b;` uninitialized and unconditionally `zt_pic_free()` them; when `run_zimg` (tests/test_scaler_zimg.c:94-116) fails its first `zt_pic_alloc` it returns -2 without touching `*out` → `free()` of indeterminate pointers. | OOM-only trigger. `run_zimg_asymmetric_pitch`/`run_zimg_cropped` callers zero-init and are safe. Same fix locus as MEM-1: make `zt_pic_alloc`/`run_zimg` error contract clean (zero-init + free-on-error). |

## memory management

| id | status | effort | description | notes |
|---|---|---|---|---|
| MEM-1 | open | S | `tests/zimg_test_util.h:73-78` `zt_pic_alloc` returns -1 mid-loop leaving planes `[0,k)` allocated; callers `run_zimg` (tests/test_scaler_zimg.c:98) and `resample` (tests/fuzz_scaler_seam.c:106-107) then return without `zt_pic_free(&src)`, leaking the partial picture. | OOM-only. Fix inside `zt_pic_alloc` (free partial planes before returning -1) closes every caller at once; pairs with UB-3. |
| MEM-3 | open | S | `tests/test_scaler_zimg.c:775-778` `test_tiling_matches_untiled` does `CHECK(0); continue;` on reference-run failure without `zt_pic_free(&ref)`; leaks the ref planes when `run_zimg_threads_in` fails after allocating out (process failure, not alloc failure). | Test-only leak under ASan-visible conditions; add the free before `continue`. |

## performance

No open findings (PERF-P1 parked). Verified clean: zero steady-state per-frame allocations; zero-copy defaults mean the
default aligned row-stripe path copies no planes; copy-in/out (when enabled) is parallelized
inside the same dispatch; hot USM kernels have correct `restrict` placement and are
multiversioned at three -march levels with load-time dispatch.

## scalability

No open rows — see "Open — parked" (SCAL-P1..P3): all three verified findings are inherent
costs of the deliberately simple static-partition / independent-graph / dual-pool design.
Verified non-issues: false sharing handled (`alignas(64)` + rounded sizeof + `aligned_alloc`),
partition math balanced to ±1-2 rows, all fixed caps consistent (`_Static_assert`), pinning
cannot double-book a core, no O(n²) growth in the dispatch path.

## concurrency

| id | status | effort | description | notes |
|---|---|---|---|---|
| CON-2 | open | S | `src/usm_pool.c:316-317` and `src/scaler_zimg.c:529-530` — `sem_post(all_done)` return unchecked in the last-finisher path; a failed post would hang the main thread in `up_sem_wait_nointr` forever. Wait-side failure is handled (pool poison + drain + join) but the post side is not symmetric. | `sem_post` on a valid unnamed semaphore can only fail with EOVERFLOW (needs SEM_VALUE_MAX posts) — protocol-symmetry nit, low severity. |

Verified sound (checked explicitly): per-frame state publication ordered by go-lock;
acq_rel `fetch_sub` + release-sequence + sem completion barrier publishes worker writes
correctly; SYS-4 in-place halo protocol race-free; exit/drain protocol deadlock-free;
partial-construction teardown joins only started threads; EINTR handled, non-EINTR poisons
the pool; `lazy_init_done/failed` plain bools safe under VLC's documented serial
`pf_video_filter` contract (CON-2 note in-source).

## code complexity

| id | status | effort | description | notes |
|---|---|---|---|---|
| CPLX-1 | open | S | `src/scaler_zimg.c:1143-1161` — `point_workers_planes` selects the target member via raw `offsetof` + `char*` cast through the four `WORKER_*_OFF` macros (scaler_zimg.c:1198-1201), making per-frame data flow opaque at the call site. | Correct and documented; a small `zimg_worker_view_for(worker, side, zerocopy)` accessor expresses the same consolidation without pointer arithmetic. Do only when the file is touched anyway. |

Lizard at HEAD: zero CCN>10 in 684 functions (max is exactly 10 in four functions, one an
exempt flat switch); zero src/ functions over 7 params. The tri-site zero-copy/tiling state
machine complexity is tracked as PAT-1 (the consolidation is the fix).

## code duplication

| id | status | effort | description | notes |
|---|---|---|---|---|
| DUP-1 | open | M | The broadcast-gate + counting-barrier worker-pool machinery is duplicated near-verbatim between `src/scaler_zimg.c` (worker_wait_for_go:468-477, wake_all_for_exit:854-877, dispatch_and_wait:1167-1195, gate fields:159-186, completion:529-530) and `src/usm_pool.c` (:297-306, :597-619, :530-542, :108-125, :316-317) — ~100 lines of correctness-critical synchronization plus its memory-ordering rationale maintained twice. | In-source comments already say "mirrors the zimg pool". Extract a shared `up_pool_gate_t` (init/destroy, wait_for_go, dispatch+wait, wake-for-exit) as `static inline` in threading.h; `USM_VARIANT` triple-compile is no obstacle. Closes ARCH-1 simultaneously. |
| DUP-2 | open | S | 13 test suites re-declare the identical `g_run/g_fail` + `BEGIN/END/CHECK` mini-harness and epilogue with naming drift (`g_cur_fail` vs `g_failed_in_test` vs `g_current_fail`) — e.g. tests/test_usm_pool.c:63-75, tests/test_lifetime.c:37-46, tests/test_zimg_helpers.c:27-45. | A ~30-line `tests/test_harness.h` removes all copies and stops the drift. |
| DUP-3 | open | S | Every `#ifdef FUZZ_MAIN` smoke block re-rolls the same xorshift PRNG + iteration loop + arg parsing (~12 copies; e.g. tests/fuzz_copy_plane.c:171-204, tests/fuzz_perfmon.c:132-171); some use shared `up_cli_parse_long`, others hand-roll `strtol`. | A `tests/fuzz_smoke.h` with the PRNG and a `fuzz_smoke_main(argc, argv, default_iters, run_one)` driver keeps per-fuzzer bias code local while deleting the scaffold copies. |

## architecture/modularity/SOLID

| id | status | effort | description | notes |
|---|---|---|---|---|
| ARCH-1 | open | M | `src/scaler_zimg.c` (1351 lines, 44 functions) carries two responsibilities: a generic persistent worker-pool/dispatch engine and zimg-specific graph/geometry/IO management. | Same root cause as DUP-1; extracting the shared pool gate into threading.h resolves both, leaving scaler_zimg.c owning only zimg concerns. |
| ARCH-2 | open | S | `src/usm.h:29` includes `zimg_helpers.h` solely for `up_copy_plane`; the zimg-named header is a de-facto generic-utilities module (also hosts axis-neutral `up_compute_stripe_bounds`/`up_decide_tile_grid`). | Naming/boundary debt only: move the generic helpers to a neutral `plane_utils.h` or rename the header. |

Otherwise clean: header-heavy pure-logic layout is a deliberate documented testability
convention; `scaler_ctx_t.zimg` sub-struct isolates backend tunables; scaler.c cleanly
separates dispatch from VLC types.

## decoupling

No open rows beyond ABI-2 (the three-way hand-declared variant prototypes — filed under
API/ABI since silent ABI drift is the sharper risk). Verified clean: config knobs read only
in Open()-phase helpers and flow through `scaler_ctx_t`; test reach-ins (`--wrap`,
production-`.c` include in test_scaler_swscale) are deliberate contained fault-injection
idioms; fourcc constants pinned by `_Static_assert`.

## business/design patterns/DDD

| id | status | effort | description | notes |
|---|---|---|---|---|
| PAT-1 | open | M | The zero-copy/tiling mode resolution is a distributed state machine with three owners: `zimg_use_rows_only` mutates five fields, `zimg_open` re-adjusts `dst_zerocopy` (src/scaler_zimg.c:1105-1126), and `zimg_prepare_first_frame_io` re-runs the transition on the first frame (:1243-1257). The invariant "col_tiled ⇒ src-direct reads ∧ dst copy-out" is enforced in three places. | Introduce a pure `zimg_resolve_io_plan(options, geometry, alignment) -> {n_rows, n_cols, col_tiled, src_zerocopy, dst_zerocopy}` computed at open and once at first frame, applied atomically. Pure function slots into the existing header-based test/fuzz harness (fuzz_decide_tile_grid already covers the grid half). |

Otherwise the codebase already uses the right patterns (strategy vtables, table-driven
mappings, flat switches); proposing more would violate the "only when it improves clarity" rule.

## reliability/correctness

| id | status | effort | description | notes |
|---|---|---|---|---|
| REL-1 | open | S | `tests/fuzz_usm_variants.c:50-57` gates variants on headline features (`__builtin_cpu_supports("avx2")`, `"avx512f"&&"avx512bw"`) instead of the full-level `up_cpu_supports_v3()/v4()`; the variant objects are compiled at `-march=x86-64-v3/v4` and may emit BMI2/FMA/AVX512VL anywhere → possible SIGILL on partial-feature CPUs. | Internally inconsistent with sibling `tests/test_usm_pool_variants.c` which uses the cpu_level.h probes; switch to the same. |
| REL-2 | open | S | `tests/test_scaler_zimg.c:600-629` `test_construction_pthread_fail`: `RLIMIT_NPROC` is not enforced for privileged processes; as root (common in CI containers) `pthread_create` succeeds and the strict `CHECK(rc1 == SCALER_PROCESS_FATAL)` fails spuriously. | `tests/test_usm_pool.c:580-605` handles the same scenario tolerantly; mirror that (accept OK-as-root or skip under euid 0). |
| REL-3 | open | S | `tests/fuzz_scaler_seam.c:152-154` — `dw` and `threads` decode the same input bytes (`data + 9`), and `dh` (`data + 12`) overlaps `dw`'s last byte, so destination width and thread count are deterministically coupled and cannot be explored independently. | Likely copy-paste offset; re-layout the 16-byte input (or grow it) so each field has its own bytes. Note: existing corpus seeds encode the coupled layout — re-derive or keep both decoders versioned. |
| REL-4 | open | S | `tests/fuzz_decide_tile_grid.c:35-66` — `oracle_grid()` is a near-verbatim reimplementation of `up_decide_tile_grid` (same clamps, loop, tie rule), so the expected-grid comparison is tautological; only the independent contract checks provide signal. | Replace the mirror oracle with property checks only (cells ≤ budget, rows/cols bounds, monotonicity), or an intentionally different brute-force search. |
| REL-5 | open | S | `src/autoupscale.c:1009-1014` — `Filter()` increments `dropped_count` on dead-backend and process-failure paths but not when `filter_NewPicture` fails, so OBS-3 stats and exported variables undercount drops under allocation pressure. | One increment on the NewPicture-fail path. |
| REL-6 | open | M | `src/scaler_zimg.c:1303-1305` + `src/autoupscale.c:1030-1031` — a zero-copy graph built on the first frame returns TRANSIENT for every later frame whose buffer drifts off 32-byte alignment; TRANSIENT never triggers swscale fallback, so a permanent allocator change mid-stream drops all remaining frames, with no zimg-side log (not covered by `preflight_warned`) and only the generic one-shot Filter() warning misattributing the cause. | Add a consecutive-TRANSIENT counter that escalates to FATAL (letting the existing fallback engage) and a one-shot alignment-drift message. In-source comment documents the drop-per-frame tradeoff but not the forever case. |
| REL-7 | open | S | `tests/test_scaler_swscale.c:313-319` `test_close_without_context` asserts `g_free_calls == 4`, a global accumulated by earlier tests; reordering or adding an open/close pair anywhere breaks this unrelated test. | Snapshot the counter at test start and assert the delta. |

## portability/standards conformance

| id | status | effort | description | notes |
|---|---|---|---|---|

UB-1 (perfmon signed shift) is also a portability item; tracked once under undefined behavior.

## error handling

| id | status | effort | description | notes |
|---|---|---|---|---|
| ERR-1 | open | S | `src/autoupscale.c:706-707` — the two OBS-5 `var_Create` return values are unchecked; on failure the periodic `var_SetInteger` calls (autoupscale.c:934-936) silently operate on a nonexistent variable. | Harmless in VLC 3 but a swallowed failure; check and skip stats export on failure. |
| ERR-2 | open | S | `tests/fuzz_usm_variants.c:126-131` `run_sse2_reference` ignores `up_usm_pool_apply_sse2`'s return; if the reference apply fails while variant applies succeed, the harness compares the 0xCC poison baseline against real output and reports a bogus "variant divergence" instead of the actual failure. | Return/abort on reference failure so the report names the real culprit. |

src/ otherwise clean end-to-end: `scaler_process_status_t` honored by every producer/consumer;
`up_sem_wait_nointr` retries only EINTR; all alloc/sem/pthread init results checked with
guarded cleanup.

## resource management

No open findings in src/. Traced and verified sound: VLC Close pairs everything Open creates;
zimg lazy-init failure at every stage releases exactly once via guard flags; barrier failure
drains + joins before returning (no worker can touch a picture VLC releases); USM pool
mirrors the pattern; swscale open/close paired; allocation growth bounded everywhere.
Test-harness pic leaks are tracked as MEM-1/MEM-3.

## API/ABI stability

| id | status | effort | description | notes |
|---|---|---|---|---|
| ABI-1 | open | S | Plugin `.so` exports every internal symbol with default visibility (verified via `nm`: `up_usm_pool_*`, `scaler_pick`, `scaler_backend_*_impl` all global); generic names can collide in embedders loading with RTLD_GLOBAL. | Add `-fvisibility=hidden` to PLUGIN_CFLAGS (Makefile:69-73); VLC's plugin macros already mark `vlc_entry*` default-visibility. One-flag fix; verify module still loads. |
| ABI-2 | open | M | The nine `up_usm_pool_{create,destroy,apply}_{sse2,avx2,avx512}` extern prototypes are hand-declared in three files (src/usm_pool_dispatch.c:59-81, tests/test_usm_pool_variants.c:40-63, tests/fuzz_usm_variants.c:27-38) with no compile-time cross-check; a prototype change kept in only some copies compiles per-TU and links (same symbol) → silent ABI mismatch/UB at the call boundary. | usm_pool.h:22-39 documents the hazard only as a comment. Single `usm_pool_variants.h` (X-macro over the variant list generating all decls) makes drift a compile error. Also the only decoupling finding. |

## build/toolchain hygiene

| id | status | effort | description | notes |
|---|---|---|---|---|
| BUILD-7 | open | S | `.github/workflows/ci.yml:124` — `actions/upload-artifact@v4` is tag-pinned while checkout and sonarqube-scan-action are SHA-pinned; inconsistent supply-chain pinning. | Pin by SHA. |

## observability

| id | status | effort | description | notes |
|---|---|---|---|---|
| OBS-1 | open | M | The USM pool's effective worker count is unobservable: partial spawn silently shrinks `n_threads` (src/usm_pool.c:422-436) and create-time stripe clamping (usm_pool.c:459-461) shrinks it too, while the engagement log (src/autoupscale.c:712-722) prints `threads=%d` from an independent `up_threads_decide()` computation reflecting neither pool. | The zimg pool logs its real grid (`log_zimg_open`); the USM pool logs nothing — headline log can overstate parallelism. Log actual `n_threads` after lazy init (needs a query hook or deferred log). |
| OBS-2 | open | S | `src/autoupscale.c:964-991` `TryBackendFallback` returns silently when a FATAL zimg failure occurs but fallback is suppressed by forced `--autoupscale-backend=1`; user sees only the generic one-shot "backend failed to process a frame" with no hint that the forced-backend setting suppressed recovery and all remaining frames will drop. | One-shot msg_Err naming the forced-backend suppression. |
| OBS-3 | open | S | `src/autoupscale.c:549-552` — `zerocopy-dst=0` gets a `msg_Info` ("dst zero-copy DISABLED") but `zerocopy-src=0` gets no equivalent (only the conditional grid-change warn in scaler_zimg.c:1113-1117, which fires only when column tiling was planned); asymmetric visibility for symmetric safety knobs. | Mirror the msg_Info. |

The missing alignment-drift log is folded into REL-6 (same fix). No log-spam risks found:
all repeated-path messages are one-shot latched; periodic stats are msg_Dbg on a 5 s tick.

## wiring gaps

No open findings. All 14 `add_integer_with_range` options verified read to a concrete
consumer; both backends reachable through `scaler_pick`; OBS-5 exported variables
created/set/destroyed in pairs. Documented-and-accepted exceptions: `up_usm_apply_plane` +
`up_usm_workspace_size` are test-only byte-identity oracles (WIRE-2 note in usm.h);
`USM_POOL_FLAT_SKIP` is a documented bench-only opt-in (CI compile gap tracked as BUILD-5).

## unused functions/methods

| id | status | effort | description | notes |
|---|---|---|---|---|
| DEAD-1 | open | S | `src/scaler_zimg.c:201` — `stripe_worker_t.src_x_start` is written in `init_stripe_worker` (:770) but never read anywhere in src or tests; its comment claims "used by active_region" but `build_stripe_graph` takes the column window from `cell_bounds_t` directly. | Whole-repo grep: only the declaration and the assignment. Delete field + stale comment. |

No other unused functions/macros: every candidate checked has a verified production or test
consumer.

## Open — parked

| id | status | effort | description | why not now |
|---|---|---|---|---|
| PERF-P1 | parked | M | `src/usm_pool.c:476-516` — in-place USM does 2×n_threads serial main-thread halo-row memcpys per frame before dispatch (~115 KB/frame at 1080p/30 workers, ~0.5 MB worst case at 4K/64). | Folding snapshots into workers needs an extra ready-barrier — significant complexity for a cost that hasn't shown up in a profile. Measure first. |
| SCAL-P1 | parked | L | Static equal-work stripe partition + full per-frame completion barrier (src/usm_pool.c:530-542, src/scaler_zimg.c:1167-1195) makes frame latency `max` over workers; on hybrid P/E-core CPUs or decoder-shared cores, fast workers idle at the barrier every frame. | Partition math itself is balanced (±1-2 rows); the fix is dynamic stripe stealing or heterogeneity-aware sizing — a large change against a deliberately simple, verified-correct design. Revisit with profile evidence on hybrid hardware. |
| SCAL-P2 | parked | M | Per-worker zimg graph + tmp buffer (+ per-tile dst scratch) grows memory and graph-build time linearly with thread count, up to 64 graphs (src/scaler_zimg.c:732-759). | Independent graphs are what makes the frame path lock-free; bounded by thread caps and stripe floors. Inherent design cost, listed for visibility. |
| SCAL-P3 | parked | L | zimg pool and USM pool are separate persistent pools sized from the same budget (src/autoupscale.c:558-578, src/scaler_zimg.c:1081-1099): up to 2×64 threads per filter instance that only ever run sequentially within a frame. | Idle cv-blocked threads are cheap; lazy init keeps probe-only cycles free. Merging pools is a large restructuring for modest gain (one fewer wake/barrier round-trip). |

## Audit picks deliberately rejected

Recorded so future passes don't re-pick them:

- **Unifying `worker_copy_in_stripe`/`worker_copy_out_stripe`/`worker_copy_out_tile`** (src/scaler_zimg.c:373-455) — shapes rhyme but direction and offset math differ; a unified helper needs ~8 params and reads worse.
- **`min_plane_dims` in tests/fuzz_scaler_seam.c re-encoding subsample knowledge** from scaler_zimg_chroma.h — 8 lines, too small to matter.
- **First-frame rows-only fallback passing reduced `n_threads` as budget** (src/scaler_zimg.c:1255-1256) — verified it cannot change the resulting grid: column tiling engages only when `row_limit < budget` and `min(row_limit, reduced) == row_limit` in every reachable case.
- **`lazy_init_done/failed` as plain bools** — race-free under VLC's documented serial `pf_video_filter` contract; documented in-source. Revisit only if that contract changes.
- **Per-worker 2-boundary-row re-hblur** in the USM pool — deliberate trade (<1% of USM work at 1080p) that buys barrier-free operation.
- **RunProbe fusion beyond PERF-1** / SIMD for probe sweeps — probe is 60-frame bounded and grid-subsampled; not worth kernel complexity.
