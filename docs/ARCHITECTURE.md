# Architecture

This document describes the runtime contracts needed to maintain AutoUpscale.
For commands and user settings, see [Usage](USAGE.md).

## Frame path

1. VLC enters the baseline-compiled `up_autoupscale_open_checked()` callback.
   It verifies the configured CPU level before calling the implementation.
2. `up_autoupscale_open()` receives negotiated input geometry and chroma;
   `up_plan_upscale()` selects a target or declines the stream.
3. `scaler_pick()` selects zimg or swscale, with open-time fallback in AUTO.
4. `Filter()` validates each picture, allocates output, and invokes the
   active backend.
5. Eligible YUV output optionally receives the luma-only USM pass.
6. Metadata is copied and ownership returns to VLC.

AUTO declines sources at or above `skip-above` and any plan that would
downscale. Explicit targets bypass `skip-above`. Every plan preserves aspect
ratio, produces even dimensions, and caps linear enlargement at 4×.

Opaque VA-API, VDPAU, Direct3D, MMAL, and CoreVideo chromas are rejected before
pixel access. VLC may insert a hardware-to-software converter and retry with a
readable format.

## CPU and load contract

`autoupscale_module.c` owns the VLC descriptor and guarded open callback. It is
compiled without LTO at the x86-64 baseline, independently of `MARCH`, so VLC
can enter it safely. Builds whose implementation requires the x86-64-v3 or
x86-64-v4 feature level reject an older CPU before calling
`up_autoupscale_open()`.

All implementation translation units otherwise follow `MARCH`.
`MULTIVERSION=1` changes only the USM pool: it links SSE2, AVX2, and AVX-512
variants plus a baseline dispatcher. It does not make the scaler backends or
the rest of the plugin independent of `MARCH`.

The `plugin` target automatically runs `check-load-safe-isa`, which
disassembles the linked VLC entry points and guarded callback to reject
wide-vector instructions before the CPU check. `check-multiversion-isa`
separately verifies the linked USM variants and dispatcher.

## Backend contract

`scaler.h` defines a small strategy interface: `supports`, `open`, `process`,
and `close`. `scaler.c` owns selection and fallback.

- zimg supports planar YUV and provides Spline36. It uses persistent worker
  graphs arranged as row stripes or a row/column grid.
- swscale covers the broader CPU-readable format set and is single-threaded.
  It maps Spline36 to Lanczos.
- In AUTO, a zimg support/open failure selects swscale. A fatal zimg processing
  failure switches once for later frames. A transient failure drops only the
  current frame. Forced backends never fall back.

Both backends consume validated, crop-aware `up_picture_view_t` data. YV12's
physical Y/V/U order is mapped to semantic Y/U/V only at library boundaries.

## Lazy resources and ownership

zimg `open` records geometry and topology but does not create workers, graphs,
or frame scratch. The first valid frame initializes them. The USM descriptor is
also cheap; its first non-identity apply creates its pool and rolling buffers.
Sticky initialization failures prevent repeated allocation attempts.

VLC owns input and output pictures. Backends borrow their plane memory only for
the current synchronous `process` call. Each backend owns and releases its
private contexts, workers, graphs, and scratch in `close`.

The zimg source and destination paths independently choose direct or scratch
I/O. Direct access requires validated geometry and alignment. Misalignment on
the first frame selects persistent scratch; unsafe later geometry drift drops
the frame. Column cells always use private destination tile scratch. Copy and
direct I/O are byte-identical when they retain the same grid. If source-direct
access enables column tiling that copy-in disables, independently phased graphs
can produce bounded seam differences; that topology change is validated with a
seam criterion rather than byte equality.

## Worker lifecycle

`worker_pool.h` provides the shared lazy lifecycle for zimg and USM.
`thread_policy.h` provides topology discovery and the worker-count policy;
`pool_gate.h` provides the generation-based dispatch gate.

The main thread publishes frame state while workers are blocked, arms the
completion count, advances the generation under the gate mutex, and broadcasts.
Workers process disjoint regions and decrement completion. The caller waits on
a monotonic deadline. A synchronization failure poisons and retires the pool;
retirement remains synchronous so storage is never freed under a callback.

A one-worker pool runs inline without a thread or barrier. Partial startup is
allowed for USM and repartitions its stripes; zimg uses all-or-nothing startup
because its graph grid is precomputed.

With `adaptive-usm=1` and `threads=0`, `worker_tuner.h` scores combined scaling
and sharpening time while testing USM worker counts. `usm_adaptive.h` owns at most one
trial pool beside the working pool; only one dispatches a frame. Pool switches
and retirement occur after dispatch completion. Rejected trials are released;
accepted trials replace the working pool. Allocation or trial initialization
failure stops exploration. An uncertain trial drops its frame while preserving
the baseline for later frames. Close and content-based USM retirement release
both pools. Trials can temporarily keep up to 128 worker threads alive, though
at most 64 execute USM work. The scaler grid and output pixels stay unchanged.

## USM

USM applies a 3×3 separable Gaussian high-pass to luma:

```text
output = source + amount × (source - blur(source))
```

Production uses fixed-point arithmetic and a fused rolling-row sweep. Each
worker owns its destination rows and five row-width buffers. For in-place
operation, the dispatcher snapshots cross-stripe halo rows before waking the
workers. This keeps output byte-identical to the reference implementation.

USM is disabled for RGB and chromas without a readable luma plane. After the
initial content window, a high Laplacian metric can disable USM for grainy
content. The separate soft-and-blocky content advisory is diagnostic only.

## Source map

| Area | Files |
|---|---|
| VLC descriptor and CPU gate | `src/autoupscale_module.c`, `src/autoupscale_module.h` |
| VLC lifecycle implementation | `src/autoupscale.c` |
| Planning | `src/upscale_logic.h` |
| Backend selection | `src/scaler.c`, `src/scaler.h`, `src/scaler_status.h`, `src/scaler_pick_logic.h` |
| zimg backend | `src/scaler_zimg.c`, `src/scaler_zimg_chroma.h` |
| zimg geometry and scratch | `src/zimg_helpers.h`, `src/plane_utils.h`, `src/plane_buffer.h` |
| swscale | `src/scaler_swscale.c` |
| Picture validation | `src/picture_view.h`, `src/chroma_classify.h` |
| Worker lifecycle | `src/worker_pool.h`, `src/pool_gate.h`, `src/thread_policy.h` |
| USM | `src/usm.h`, `src/usm_pool.c`, `src/usm_pool.h` |
| SIMD dispatch | `src/usm_pool_dispatch.c`, `src/usm_pool_variants.h`, `src/cpu_level.h` |
| Content analysis | `src/content_probe.h` |
| Curated mutation runner | `scripts/mutation_test.py` |

## Verification model

Pure logic is tested without VLC. Contract tests use boundary stubs for VLC,
FFmpeg, allocation, and pthread failures. Curated mutation tests compile
isolated faulty copies of target selection, backend dispatch, and content-probe
logic against their owning suites. Only status 1 from a compiled mutant counts
as a kill; survivors, compile failures, abnormal exits, and timeouts fail the
gate. Cross-variant tests require identical SSE2/AVX2/AVX-512 output.
Deterministic fuzz-smoke, libFuzzer, sanitizer stress, coverage gates, static
analysis, lifecycle tests through the real guarded callback, linked-ISA checks,
symbol visibility, and hardening checks cover their respective contracts. The
Makefile target output is authoritative for current scope and thresholds.
