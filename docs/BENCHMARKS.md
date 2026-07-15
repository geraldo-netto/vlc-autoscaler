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
scripts/bench_zimg_pinning.sh build/bench_scaler_zimg
```

Run the benchmark binary without arguments for its current interface. The USM
benchmark supports `rand`, `flat`, and `mixed` input fills. The matrix script
accepts optional frame-count, amount, and fill arguments.

`bench-usm-halo` compares out-of-place (`out`) against in-place (`in`) runs at
the same shapes. Their delta includes the serial halo-row snapshots required by
in-place processing and the different cache/write traffic; use it as a trigger
for profiling, not as an isolated snapshot-time measurement.

To compare ISA variants without letting the runtime dispatcher hide their
individual costs, build the same benchmark three times and run the paired
matrix:

```sh
make BUILD=build_dev/isa-sse2 MARCH=x86-64 build-bench
make BUILD=build_dev/isa-avx2 MARCH=x86-64-v3 build-bench
make BUILD=build_dev/isa-avx512 MARCH=x86-64-v4 build-bench
scripts/bench_usm_isa.sh build_dev/isa-sse2/bench_usm_pool \
  build_dev/isa-avx2/bench_usm_pool build_dev/isa-avx512/bench_usm_pool
```

Set `BENCH_FRAMES` or `BENCH_AMOUNT` to override the matrix defaults. Run each
matrix repeatedly under the same governor and load before changing dispatch.

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

The pinning matrix compares scheduler placement with current first-allowed-CPU
pinning under all-logical-CPU and one-thread-per-core affinity masks. Its
default masks fit the 32-thread/16-core reference host; edit them to match the
measured machine. Repeat the matrix before changing the opt-in pinning default.

The zimg benchmark also reports first-frame lazy initialization in
microseconds and process maximum resident set in KiB before steady-state frame
time. Compare separate process runs by geometry, chroma, zero-copy mode, and
worker count; `ru_maxrss` includes the harness and libraries, so compare deltas
rather than treating it as backend-only allocation.
