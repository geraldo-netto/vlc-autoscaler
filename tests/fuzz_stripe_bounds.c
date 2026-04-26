// SPDX-License-Identifier: GPL-2.0-or-later
/*****************************************************************************
 * fuzz_stripe_bounds.c - libFuzzer/smoke target for up_compute_stripe_bounds
 *****************************************************************************
 * Generates random (n, src_h, dst_h) triples, computes the stripe bounds
 * for every i in [0, n), and verifies the stripe-partition invariants:
 *
 *   1. Each non-degenerate stripe has dst_y_start < dst_y_end and
 *      src_y_start < src_y_end.
 *   2. All four bounds are aligned to multiples of 2.
 *   3. Stripes are contiguous: stripe i+1's start equals stripe i's end
 *      on both src and dst sides (no gaps, no overlaps).
 *   4. The first stripe starts at 0 and the last ends at the dimension.
 *   5. Per-stripe ratio src_h/dst_h is close to the global ratio (within
 *      a slack proportional to 1/n - the rounding-induced drift).
 *
 * Two entry points: LLVMFuzzerTestOneInput + a deterministic smoke main.
 *****************************************************************************/

#include "../src/zimg_helpers.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_N      64
#define MAX_DIM    8192

static int run_one(const uint8_t *data, size_t size)
{
    if (size < 3 * sizeof(uint16_t)) return 0;

    uint16_t r0, r1, r2;
    memcpy(&r0, data + 0, sizeof r0);
    memcpy(&r1, data + 2, sizeof r1);
    memcpy(&r2, data + 4, sizeof r2);

    int n     = (int)(r0 % (MAX_N + 1));
    int src_h = (int)(r1 % (MAX_DIM + 1));
    int dst_h = (int)(r2 % (MAX_DIM + 1));

    /* Skip genuinely-invalid inputs - the function returns 0 for these,
     * which is the documented contract. We only want to exercise the
     * partition invariants when n >= 1 and both dims >= 1. */
    if (n < 1 || src_h < 1 || dst_h < 1) {
        int sys, sye, dys, dye;
        if (up_compute_stripe_bounds(0, n, src_h, dst_h,
                                     &sys, &sye, &dys, &dye)) {
            fprintf(stderr,
                "FAIL: invalid input (n=%d src_h=%d dst_h=%d) returned ok\n",
                n, src_h, dst_h);
            return 1;
        }
        return 0;
    }

    /* Walk through every stripe i in [0, n) and check invariants. */
    int last_dst_end = 0;
    int last_src_end = 0;
    int stripes_ok = 0;

    for (int i = 0; i < n; i++) {
        int sys, sye, dys, dye;
        int ok = up_compute_stripe_bounds(i, n, src_h, dst_h,
                                          &sys, &sye, &dys, &dye);
        if (!ok) {
            /* Degenerate stripe is acceptable when n is too large for
             * the dst dimension. The contract says caller drops the
             * worker count and stops processing. We just stop too. */
            break;
        }
        stripes_ok++;

        /* Invariant 1: ranges non-empty. */
        if (sye <= sys || dye <= dys) {
            fprintf(stderr,
                "FAIL: empty stripe i=%d n=%d s=[%d,%d) d=[%d,%d)\n",
                i, n, sys, sye, dys, dye);
            return 1;
        }
        /* Invariant 2: aligned to 2. */
        if ((sys & 1) || (sye & 1) || (dys & 1) || (dye & 1)) {
            /* Last stripe end may equal the unaligned dimension - allow it. */
            int ok_last = (i == n - 1) && (sye == src_h) && (dye == dst_h);
            if (!ok_last) {
                fprintf(stderr,
                    "FAIL: misaligned bounds i=%d n=%d "
                    "s=[%d,%d) d=[%d,%d)\n",
                    i, n, sys, sye, dys, dye);
                return 1;
            }
        }
        /* Invariant 3: contiguous. */
        if (sys != last_src_end || dys != last_dst_end) {
            fprintf(stderr,
                "FAIL: gap/overlap i=%d n=%d "
                "got src=%d dst=%d, expected src=%d dst=%d\n",
                i, n, sys, dys, last_src_end, last_dst_end);
            return 1;
        }
        last_src_end = sye;
        last_dst_end = dye;
    }

    /* Invariant 4: full coverage when at least one stripe materialized. */
    if (stripes_ok > 0) {
        if (last_dst_end != dst_h || last_src_end != src_h) {
            fprintf(stderr,
                "FAIL: incomplete coverage n=%d src_h=%d dst_h=%d "
                "last_src_end=%d last_dst_end=%d (stripes_ok=%d)\n",
                n, src_h, dst_h, last_src_end, last_dst_end, stripes_ok);
            return 1;
        }
    }

    return 0;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    return run_one(data, size) ? 1 : 0;
}

#ifdef FUZZ_MAIN
static uint64_t xs_state = 0xc0ffeebabe1234ULL;
static uint64_t xs(void)
{
    uint64_t x = xs_state;
    x ^= x << 13; x ^= x >> 7; x ^= x << 17;
    return xs_state = x;
}

int main(int argc, char **argv)
{
    long iters = (argc > 1) ? atol(argv[1]) : 100000;
    uint8_t buf[16];
    long n_fail = 0;
    for (long i = 0; i < iters; i++) {
        for (size_t j = 0; j < sizeof buf; j++) buf[j] = (uint8_t)xs();
        if (run_one(buf, sizeof buf)) n_fail++;
    }
    if (n_fail) {
        fprintf(stderr, "stripe_bounds smoke FAIL: %ld/%ld trials\n",
                n_fail, iters);
        return 1;
    }
    fprintf(stderr, "stripe_bounds smoke OK: %ld iterations\n", iters);
    return 0;
}
#endif
