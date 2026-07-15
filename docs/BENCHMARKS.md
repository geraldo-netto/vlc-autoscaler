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
arguments. The benchmark accepts `rand`, `flat`, and `mixed`; its usage output
and source are authoritative for the current interface.

`bench_usm_pool` emits
`requested_threads,effective_threads,width,height,frames,amount,fill,us_per_frame`.
The effective count is queried after warmup, so it includes the pool's worker
cap and any lazy-start reduction. The matrix validates every echoed workload
field, requires the effective count to remain stable across its three samples,
and emits
`variant,requested_threads,effective_threads,width,height,frames,amount,fill,row_type,run_index,us_per_frame`.
Each validated group contains the three raw timings in acquisition order as
`row_type=raw` with run indexes 1 through 3, followed by its derived median as
`row_type=median` with run index 0. Filter on `row_type=median` for aggregate
comparisons; retain the raw rows to reproduce and audit each result. A failed
sample prevents the entire four-row group from being emitted.
Use `effective_threads`, not the request, when interpreting scaling.

The benchmark programs fill their source buffers before timing and report
kernel or scaler work separately from decode, encode, display, and frame
generation. The Makefile and benchmark sources are the source of truth for the
workload matrix, defaults, warmup, and repetition policy.

`bench_usm_pool` specifically times `up_usm_pool_apply` after its source-defined
warmup. Compare its frame time with the budget for the actual playback rate,
remembering that the scaler and the rest of the media pipeline consume the
remaining budget.

Keep the host idle during comparisons. When publishing a result, record:

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
low-activity stripes, is disabled in production, and is not guaranteed
byte-identical to the production kernel, so its results must not be combined
with production-pipeline claims.

The production kernel touches the same source and destination bytes for every
fill mode; content values alone do not make its working set cache-resident.

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
`MULTIVERSION` configuration, and compare the same command on the same host.
Do not apply a fixed multiplier across CPUs. Variant byte equivalence is
checked by `tests/test_usm_pool_variants.c`; because that byte-identity would
also hide an accidental collapse of a variant to the baseline,
`make MULTIVERSION=1 check-multiversion-isa` disassembles the final LTO-linked
plugin and confirms that the retained worker anchors use the target instruction
sets (SSE2 / AVX2 / AVX-512) while the load-time selector contains no AVX/EVEX
vector instructions.

The engagement log identifies the selected implementation: a single-baseline
build reports `simd=default`, while a multiversion build reports its runtime-
selected variant. Record that label with the build flags and raw samples.
Throughput still must be measured on the deployment host.
