# Performance reference

This document explains how to measure USM-pool throughput at common
`(threads × resolution)` configurations on the supported Linux x86-64
target. Absolute results are host-, compiler-, and load-specific; measure the
machine that will run AutoUpscale.

Measurements come from `tests/bench_usm_pool.c` driven by
`scripts/bench_matrix.sh` — a standalone harness that times only the
`up_usm_pool_apply` work, with a 5-frame warmup discarded before the
timed loop. They do **not** include scaler (zimg / swscale) cost,
demuxer, decoder, encoder, or vout. Measure those separately for an
end-to-end budget.

## Reproducing

```sh
make build-bench
scripts/bench_matrix.sh build/bench_usm_pool 100 20 rand
```

The script sweeps `threads ∈ {1, 4, 8, 12, 16, 20}` and
`resolution ∈ {720p, 1080p, 1440p}`, then reports the median of three
runs per cell. Keep the host idle and record the commit, compiler, flags,
CPU governor, and kernel when publishing results.

## Interpreting results

- Compare `µs/frame` with the whole-frame budget: 16,666 µs at 60 fps,
  33,333 µs at 30 fps. The harness measures USM only; zimg/swscale,
  decode, encode, and display consume the rest.
- Thread scaling is empirical. Dispatch overhead dominates small stripes;
  memory bandwidth can dominate large frames. More workers are not
  automatically faster.
- The normal production kernel touches the same source and destination bytes
  for `rand`, `mixed`, and `flat` input. Content values alone do not make its
  working set cache-resident.
- `build/bench_usm_pool_flatskip` is a benchmark-only variant. It samples one
  middle row per stripe and may identity-copy low-activity stripes. It is off
  in production and is not guaranteed byte-identical to the reference, so do
  not combine its gains with the default pipeline.
- Compare ISA builds on the same host with the same command. Do not apply a
  fixed SSE2/AVX2/AVX-512 multiplier across different CPUs.

To check which variant your build picks, look for the engagement log
line at `--verbose=2`:

```
AutoUpscale engaged: 854x480 -> 1920x1080 (... simd=avx512)
```

`simd=default` means a single-baseline build; `simd=avx512` /
`avx2` / `sse2` means the runtime dispatcher (`MULTIVERSION=1`)
picked that variant on the running CPU.
