# Benchmarks — performance and gains

Per-frame wall-clock for the two hot paths the optimization work this cycle
touched: the threaded USM (unsharp-mask) post-pass and the slice-threaded zimg
resample. "Before" is the pre-optimization source from git; "After" is the
current tree. Both columns use the **same** benchmark harness, so the delta is
the code change, not the harness.

## How to reproduce

```sh
make bench           # USM pool throughput (us/frame) across threads/res
make bench-flatskip  # USM default vs flat-skip on flat/mixed/random content
make bench-zimg      # zimg backend throughput (needs libzimg)
```

Both benches fill the source once and time `apply()` / `process()` only
(resampling cost is content-independent), so they measure the kernel, not
frame generation. Figures below are the **median of 5–9 runs**, 400–2000
frames each, on a 32-core x86-64 host at `-march=native -O3`.

> Caveat: these are microbenchmarks. At high thread counts on small frames
> (e.g. 8 threads at 1080p) per-frame work drops to ~0.2 ms and the result is
> dominated by semaphore-dispatch jitter — treat those rows as "within noise".
> The gains are clearest where memory bandwidth or copy cost dominates (lower
> thread counts, 4K, the flat-skip fast path). Absolute numbers are
> host-specific; the **ratio** is the portable signal.

## Gains

| Path | Optimization | Workload | Before (µs/frame) | After (µs/frame) | Speedup |
|------|--------------|----------|------------------:|-----------------:|:-------:|
| USM pool | PERF-2 fuse 2 passes → 1 | 1 thr · 1080p | 1184 | 1155 | 1.03× |
| USM pool | PERF-2 | 4 thr · 1080p | 324 | 303 | 1.07× |
| USM pool | PERF-2 | 8 thr · 1080p | 217 | 224 | ~1.0× (noise) |
| USM pool | PERF-2 | 8 thr · 4K | 836 | 757 | 1.10× |
| zimg scale | PERF-1 parallel copy-in | 4 thr · 480p→1080p | 1052 | 965 | 1.09× |
| zimg scale | PERF-1 | 8 thr · 480p→1080p | 745 | 661 | 1.13× |
| zimg scale | PERF-1 | 16 thr · 480p→1080p | 566 | 471 | 1.20× |
| zimg scale | PERF-1 | 8 thr · 720p→4K | 2129 | 1778 | 1.20× |
| USM pool | WIRE-1 flat-skip | 8 thr · 1080p, flat content | 199 | 35 | 5.66× |

### Notes per optimization

- **PERF-2 (commit cc71221)** — fused the USM pool's two-phase dispatch
  (hblur-all → barrier → combine-all) into a single per-worker rolling-buffer
  sweep. Halves per-frame semaphore traffic and reads the luma plane once
  instead of twice. Output stays byte-identical (proven by the byte-identity,
  cross-SIMD and ASan/TSan stress suites). Win grows with frame size and
  shrinks into the noise floor when stripes get tiny (8 thr × 1080p).
- **PERF-1 (commit b0d327f)** — moved the per-frame source copy-in off the main
  thread into the workers (each copies its own stripe). The serial copy was a
  fixed per-frame tax; parallelizing it scales with thread count, hence the
  clean 1.09–1.20× that grows with both threads and resolution.
- **WIRE-1 (commit cc71221)** — opt-in per-stripe flat detection (`make
  bench-flatskip`): on visually flat content (letterbox bars, fades) the
  combine pass is skipped for an identity copy. ~5.7× on fully flat frames;
  no effect on detailed content.
- **PERF-4 (commit 4714938)** — removed the per-function `#pragma GCC
  optimize("O3")`. Not a throughput change (production TU is already `-O3`;
  the `-O3` assembly is byte-for-byte identical, 287 == 287 vector ops) — a
  brittleness cleanup with zero measured delta.

These per-path gains compound in the full `Filter()` pipeline (zimg resample
then USM post-pass run back to back per frame).
