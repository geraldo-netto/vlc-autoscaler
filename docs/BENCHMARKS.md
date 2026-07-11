# Benchmarking

This project intentionally does not keep numeric throughput snapshots in its
documentation. Results depend on the host, compiler, flags, CPU governor,
thermal state, and background load, so checked-in figures quickly become
misleading.

## Reproduce locally

```sh
make build-bench
make bench
make bench-flatskip
make bench-zimg
scripts/bench_matrix.sh build/bench_usm_pool
```

The matrix script also accepts optional frame-count, amount, and fill-mode
arguments; its usage line and accepted fill values are authoritative.

The benchmark programs fill their source buffers before timing and report
kernel or scaler work separately from decode, encode, display, and frame
generation. The Makefile and benchmark sources are the source of truth for the
workload matrix, defaults, warmup, and repetition policy.

When publishing a result, record:

- the commit and whether the worktree was clean;
- compiler version and complete build flags;
- CPU model, affinity mask, governor, and kernel;
- optional-library versions and selected SIMD variant;
- every raw sample, not only the aggregate;
- whether compared runs used the same host and system load.

## Interpretation

Treat small differences as noise until repeated same-host samples show a stable
separation. Dispatch overhead dominates small stripes, while memory bandwidth
can dominate large frames. More workers are therefore not automatically
faster.

The flat-skip executable is a benchmark-only variant. It may identity-copy
low-activity stripes and is not guaranteed byte-identical to the production
kernel, so its results must not be combined with production-pipeline claims.

## Expected qualitative effects

- The fused USM worker sweep reduces synchronization and avoids rereading the
  luma plane for a separate combine phase.
- Parallel source copy-in moves an explicitly requested copy path off the main
  thread; the production default reads the source directly.
- Flat-skip benefits visually flat content and should have little effect on
  detailed content.
- Wider SIMD can improve the memory-bound kernels, but the size of that change
  is CPU- and compiler-specific.

For SIMD comparisons, use clean build directories for each `MARCH` and
`MULTIVERSION` configuration. Variant byte equivalence is checked by
`tests/test_usm_pool_variants.c`; because that byte-identity would also hide an
accidental collapse of a variant to the baseline, `make check-multiversion-isa`
separately confirms each variant actually emits its target instruction set
(SSE2 / AVX2 / AVX-512). Throughput still must be measured on the deployment host.
