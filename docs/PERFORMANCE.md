# Performance measurement

This document explains how to measure USM-pool throughput on the supported
Linux x86-64 target. Absolute results are host-, compiler-, and load-specific;
measure the machine that will run AutoUpscale.

Measurements come from `tests/bench_usm_pool.c`, optionally driven by
`scripts/bench_matrix.sh`. The harness times `up_usm_pool_apply` after the
fixed warmup defined by its source. It does not include scaler, demuxer,
decoder, encoder, or display cost, so measure those separately for an
end-to-end budget.

## Reproducing

```sh
make build-bench
scripts/bench_matrix.sh build/bench_usm_pool
```

Optional frame-count, amount, and fill-mode arguments are documented by the
script itself. Supported fill modes are `rand`, `flat`, and `mixed`.

The benchmark source and matrix script define the active workloads and sample
policy. Keep the host idle and record the commit, compiler, flags, CPU
governor, affinity, and kernel when publishing results.

## Interpreting results

- Compare the reported frame time with the budget derived from the actual
  playback rate. The harness measures USM only; the rest of the media pipeline
  consumes the remaining budget.
- Thread scaling is empirical. Dispatch overhead dominates small stripes;
  memory bandwidth can dominate large frames. More workers are not
  automatically faster.
- The production kernel touches the same source and destination bytes for each
  fill mode. Content values alone do not make its working set cache-resident.
- The flat-skip executable is benchmark-only. It may identity-copy
  low-activity stripes, is disabled in production, and is not guaranteed
  byte-identical to the reference.
- Compare ISA builds on the same host with the same command. Do not apply a
  fixed multiplier across different CPUs.

The engagement log reports the selected SIMD implementation. A
single-baseline build reports `simd=default`; a multi-versioned build reports
the runtime-selected variant. Treat that label, the build flags, and the raw
samples as part of every performance result.
