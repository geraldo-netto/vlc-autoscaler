# Performance and benchmarking

This is the canonical performance measurement guide. Measure on the deployment
host: compiler, flags, CPU affinity, governor, temperature, and background load
can outweigh small code differences. This repository therefore does not publish
fixed throughput claims.

## Run

```sh
make build-bench
make bench
make bench-usm-halo
make bench-flatskip
make bench-zimg
scripts/bench_matrix.sh build/bench_usm_pool
```

Run the benchmark binary without arguments for its current interface. The USM
benchmark supports `rand`, `flat`, and `mixed` input fills. The matrix script
accepts optional frame-count, amount, and fill arguments.

`bench-usm-halo` compares out-of-place (`out`) against in-place (`in`) runs at
the same shapes. Their delta includes the serial halo-row snapshots required by
in-place processing and the different cache/write traffic; use it as a trigger
for profiling, not as an isolated snapshot-time measurement.

`bench_usm_pool` emits:

```text
requested_threads,effective_threads,width,height,frames,amount,fill,us_per_frame
```

The matrix emits three validated raw samples and their median:

```text
variant,requested_threads,effective_threads,width,height,frames,amount,fill,row_type,run_index,us_per_frame
```

Filter `row_type=median` for comparisons, but retain the raw rows. Use
`effective_threads`, not the request, because geometry and startup can reduce
the pool size.

## Compare

For each result, record:

- commit and worktree state;
- compiler version and complete flags;
- CPU model, affinity mask, governor, and kernel;
- library versions and selected SIMD variant;
- every raw sample and relevant system load.

Compare the same workload on the same idle host. Repeat runs until the ordering
is stable; treat small differences as noise. Frame time must fit alongside
decode, scaling, display/encode, and other pipeline work.

More workers are not necessarily faster. Dispatch dominates small stripes;
memory bandwidth dominates large frames. Compare compiler and SIMD builds in
clean build directories.

The engagement log reports `simd=default`, `sse2`, `avx2`, or `avx512`. A
multiversion build can verify retained instruction sets with:

```sh
make MARCH=x86-64 MULTIVERSION=1 check-multiversion-isa
```

`bench-flatskip` is experimental and not byte-identical to the production
kernel for all content. Do not combine its numbers with production claims.
Cross-variant production output equivalence is enforced by
`tests/test_usm_pool_variants.c`.
