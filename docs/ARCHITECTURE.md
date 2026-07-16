# Architecture

This document describes the runtime contracts needed to maintain AutoUpscale.
For commands and user settings, see [Usage](USAGE.md).

## Frame path

1. VLC calls `Open()` with negotiated input geometry and chroma.
2. `up_plan_upscale()` selects a target or declines the stream.
3. `scaler_pick()` selects zimg or swscale.
4. `Filter()` validates the current picture, allocates output, and invokes the
   active backend.
5. Eligible YUV output optionally receives the luma-only USM pass.
6. Metadata is copied and ownership returns to VLC.

AUTO declines sources at or above `skip-above` and any plan that would
downscale. Explicit targets bypass `skip-above`. Every plan preserves aspect
ratio, produces even dimensions, and caps linear enlargement at 4×.

Opaque VA-API, VDPAU, Direct3D, MMAL, and CoreVideo chromas are rejected before
pixel access. VLC may insert a hardware-to-software converter and retry with a
readable format.

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
`threading.h` provides topology discovery and the generation-based gate.

The main thread publishes frame state while workers are blocked, arms the
completion count, advances the generation under the gate mutex, and broadcasts.
Workers process disjoint regions and decrement completion. The caller waits on
a monotonic deadline. A synchronization failure poisons and retires the pool;
retirement remains synchronous so storage is never freed under a callback.

A one-worker pool runs inline without a thread or barrier. Partial startup is
allowed for USM and repartitions its stripes; zimg uses all-or-nothing startup
because its graph grid is precomputed.

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
| VLC lifecycle | `src/autoupscale.c` |
| Planning | `src/upscale_logic.h` |
| Backend selection | `src/scaler.c`, `src/scaler.h`, `src/scaler_status.h` |
| zimg | `src/scaler_zimg.c`, `src/scaler_zimg_chroma.h` |
| swscale | `src/scaler_swscale.c` |
| Picture validation | `src/picture_view.h`, `src/chroma_classify.h` |
| Worker lifecycle | `src/worker_pool.h`, `src/threading.h` |
| USM | `src/usm.h`, `src/usm_pool.c`, `src/usm_pool.h` |
| SIMD dispatch | `src/usm_pool_dispatch.c`, `src/cpu_level.h` |
| Content analysis | `src/content_probe.h` |

## Verification model

Pure logic is tested without VLC. Contract tests use boundary stubs for VLC,
FFmpeg, allocation, and pthread failures. Cross-variant tests require identical
SSE2/AVX2/AVX-512 output. Deterministic fuzz-smoke, libFuzzer, sanitizer stress,
coverage gates, static analysis, linked-ISA checks, symbol visibility, and
hardening checks cover their respective contracts. The Makefile target output
is authoritative for current scope and thresholds.
