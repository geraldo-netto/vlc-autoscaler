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

## memory management

| id | status | effort | description | notes |
|---|---|---|---|---|

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

Verified sound (checked explicitly): per-frame state publication ordered by go-lock;
acq_rel `fetch_sub` + release-sequence + sem completion barrier publishes worker writes
correctly; SYS-4 in-place halo protocol race-free; exit/drain protocol deadlock-free;
partial-construction teardown joins only started threads; EINTR handled, non-EINTR poisons
the pool; `lazy_init_done/failed` plain bools safe under VLC's documented serial
`pf_video_filter` contract (CON-2 note in-source).

## code complexity

| id | status | effort | description | notes |
|---|---|---|---|---|

Lizard at HEAD: zero CCN>10 in 684 functions (max is exactly 10 in four functions, one an
exempt flat switch); zero src/ functions over 7 params. The tri-site zero-copy/tiling state
machine complexity is tracked as PAT-1 (the consolidation is the fix).

## code duplication

| id | status | effort | description | notes |
|---|---|---|---|---|

## architecture/modularity/SOLID

| id | status | effort | description | notes |
|---|---|---|---|---|

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

No open findings. The codebase already uses the right patterns (strategy vtables,
table-driven mappings, flat switches); proposing more would violate the "only when it
improves clarity" rule.

## reliability/correctness

| id | status | effort | description | notes |
|---|---|---|---|---|
| REL-3 | open | S | `tests/fuzz_scaler_seam.c:152-154` — `dw` and `threads` decode the same input bytes (`data + 9`), and `dh` (`data + 12`) overlaps `dw`'s last byte, so destination width and thread count are deterministically coupled and cannot be explored independently. | Likely copy-paste offset; re-layout the 16-byte input (or grow it) so each field has its own bytes. Note: existing corpus seeds encode the coupled layout — re-derive or keep both decoders versioned. |
| REL-4 | open | S | `tests/fuzz_decide_tile_grid.c:35-66` — `oracle_grid()` is a near-verbatim reimplementation of `up_decide_tile_grid` (same clamps, loop, tie rule), so the expected-grid comparison is tautological; only the independent contract checks provide signal. | Replace the mirror oracle with property checks only (cells ≤ budget, rows/cols bounds, monotonicity), or an intentionally different brute-force search. |

## portability/standards conformance

| id | status | effort | description | notes |
|---|---|---|---|---|

UB-1 (perfmon signed shift) is also a portability item; tracked once under undefined behavior.

## error handling

| id | status | effort | description | notes |
|---|---|---|---|---|
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

## build/toolchain hygiene

| id | status | effort | description | notes |
|---|---|---|---|---|
| BUILD-7 | open | S | `.github/workflows/ci.yml:124` — `actions/upload-artifact@v4` is tag-pinned while checkout and sonarqube-scan-action are SHA-pinned; inconsistent supply-chain pinning. | Pin by SHA. |

## observability

| id | status | effort | description | notes |
|---|---|---|---|---|

No log-spam risks found: all repeated-path messages are one-shot latched; periodic stats
are msg_Dbg on a 5 s tick.

## wiring gaps

No open findings. All 14 `add_integer_with_range` options verified read to a concrete
consumer; both backends reachable through `scaler_pick`; OBS-5 exported variables
created/set/destroyed in pairs. Documented-and-accepted exceptions: `up_usm_apply_plane` +
`up_usm_workspace_size` are test-only byte-identity oracles (WIRE-2 note in usm.h);
`USM_POOL_FLAT_SKIP` is a documented bench-only opt-in (CI compile gap tracked as BUILD-5).

## unused functions/methods

| id | status | effort | description | notes |
|---|---|---|---|---|

No other unused functions/macros: every candidate checked has a verified production or test
consumer.

## Open — parked

| id | status | effort | description | why not now |
|---|---|---|---|---|
| PERF-P1 | parked | M | `src/usm_pool.c:476-516` — in-place USM does 2×n_threads serial main-thread halo-row memcpys per frame before dispatch (~115 KB/frame at 1080p/30 workers, ~0.5 MB worst case at 4K/64). | Folding snapshots into workers needs an extra ready-barrier — significant complexity for a cost that hasn't shown up in a profile. Measure first. |
| TEST-P1 | parked | S | `up_usm_pool_effective_threads` (OBS-1): the mutant `n_threads`→`n_threads_pref` survives the suite — the fields diverge only under a deterministic partial spawn, and the RLIMIT_NPROC test asserts bounds only (root-tolerant by design). | Killing it needs pthread_create fault-injection infra (new `--wrap`) for a diagnostics-only getter; not worth the infra now. |
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
