// SPDX-License-Identifier: GPL-2.0-or-later
/*****************************************************************************
 * fuzz_smoke.h — shared smoke-fuzz scaffold for #ifdef FUZZ_MAIN (DUP-3)
 *****************************************************************************
 * Every smoke main re-rolled the same xorshift PRNG, iteration loop, arg
 * parsing, and pass/fail report. This header holds one copy; per-fuzzer
 * input BIAS stays local in each fuzzer's iteration callback:
 *
 *   static int iter(long i) {
 *       uint8_t buf[16];
 *       fuzz_smoke_fill(buf, sizeof buf);
 *       ...biasing using i / fuzz_smoke_next()...
 *       return run_one(buf, sizeof buf);   // nonzero = trial failed
 *   }
 *   int main(int argc, char **argv) {
 *       fuzz_smoke_seed(0x5EED...);        // per-fuzzer stream
 *       return fuzz_smoke_main(argc, argv, 50000, "copy_plane", iter);
 *   }
 *****************************************************************************/
#ifndef FUZZ_SMOKE_H
#define FUZZ_SMOKE_H

#include <limits.h>
#include <stdint.h>
#include <stdio.h>

#include "cli_parse.h"

static uint64_t fuzz_smoke_state = UINT64_C(0x9e3779b97f4a7c15);

static inline void fuzz_smoke_seed(uint64_t seed)
{
    if (seed) fuzz_smoke_state = seed;
}

static inline uint64_t fuzz_smoke_next(void)
{
    uint64_t x = fuzz_smoke_state;
    x ^= x << 13; x ^= x >> 7; x ^= x << 17;
    return fuzz_smoke_state = x;
}

static inline void fuzz_smoke_fill(uint8_t *buf, size_t n)
{
    for (size_t i = 0; i < n; i++) buf[i] = (uint8_t)fuzz_smoke_next();
}

/* Runs `iter` for [positive-iterations] (argv[1], default default_iters)
 * trials; nonzero return = failed trial. Exit code: 0 all-pass, 1 any
 * failure, 2 usage error. */
static inline int fuzz_smoke_main(int argc, char **argv, long default_iters,
                                  const char *name, int (*iter)(long i))
{
    long iters = default_iters;
    if (argc > 2 || (argc == 2
            && !up_cli_parse_long(argv[1], 1, LONG_MAX, &iters))) {
        fprintf(stderr, "usage: %s [positive-iterations]\n", argv[0]);
        return 2;
    }
    long n_fail = 0;
    for (long i = 0; i < iters; i++)
        if (iter(i)) n_fail++;
    if (n_fail) {
        fprintf(stderr, "%s smoke FAIL: %ld/%ld trials\n",
                name, n_fail, iters);
        return 1;
    }
    fprintf(stderr, "%s smoke OK: %ld iterations\n", name, iters);
    return 0;
}

#endif /* FUZZ_SMOKE_H */
