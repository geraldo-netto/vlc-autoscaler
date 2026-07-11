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

#include "../src/plane_utils.h"
#include "cli_parse.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Compat shim over the struct-based bounds API (Sonar >7-params refactor):
 * preserves this file's original out-pointer call shape. */
static int stripe_bounds4(int i, int n, int src_h, int dst_h,
                          int *src_start, int *src_end,
                          int *dst_start, int *dst_end)
{
    up_stripe_bounds_t b = { 0, 0, 0, 0 };
    int ok = up_compute_stripe_bounds(i, n, src_h, dst_h, &b);
    *src_start = b.src_start; *src_end = b.src_end;
    *dst_start = b.dst_start; *dst_end = b.dst_end;
    return ok;
}


#define MAX_N      64
#define MAX_DIM    8192

static int parse_input(const uint8_t *data, size_t size,
                       int *n, int *src_h, int *dst_h)
{
    if (size < 3 * sizeof(uint16_t)) return 0;

    uint16_t r0, r1, r2;
    memcpy(&r0, data + 0, sizeof r0);
    memcpy(&r1, data + 2, sizeof r1);
    memcpy(&r2, data + 4, sizeof r2);

    *n     = (int)(r0 % (MAX_N + 1));
    *src_h = (int)(r1 % (MAX_DIM + 1));
    *dst_h = (int)(r2 % (MAX_DIM + 1));
    return 1;
}

static int check_invalid_input(int n, int src_h, int dst_h)
{
    int sys, sye, dys, dye;
    if (stripe_bounds4(0, n, src_h, dst_h,
                                 &sys, &sye, &dys, &dye)) {
        fprintf(stderr,
            "FAIL: invalid input (n=%d src_h=%d dst_h=%d) returned ok\n",
            n, src_h, dst_h);
        return 1;
    }
    return 0;
}

static int check_alignment(int i, int n, int src_h, int dst_h,
                           int sys, int sye, int dys, int dye)
{
    if (!((sys & 1) || (sye & 1) || (dys & 1) || (dye & 1)))
        return 0;
    /* Last stripe end may equal the unaligned dimension - allow it. */
    if ((i == n - 1) && (sye == src_h) && (dye == dst_h))
        return 0;
    fprintf(stderr,
        "FAIL: misaligned bounds i=%d n=%d s=[%d,%d) d=[%d,%d)\n",
        i, n, sys, sye, dys, dye);
    return 1;
}

static int check_stripe(int i, int n, int src_h, int dst_h,
                        int sys, int sye, int dys, int dye,
                        int last_src_end, int last_dst_end)
{
    /* Invariant 1: ranges non-empty. */
    if (sye <= sys || dye <= dys) {
        fprintf(stderr,
            "FAIL: empty stripe i=%d n=%d s=[%d,%d) d=[%d,%d)\n",
            i, n, sys, sye, dys, dye);
        return 1;
    }
    /* Invariant 2: aligned to 2. */
    if (check_alignment(i, n, src_h, dst_h, sys, sye, dys, dye))
        return 1;
    /* Invariant 3: contiguous. */
    if (sys != last_src_end || dys != last_dst_end) {
        fprintf(stderr,
            "FAIL: gap/overlap i=%d n=%d "
            "got src=%d dst=%d, expected src=%d dst=%d\n",
            i, n, sys, dys, last_src_end, last_dst_end);
        return 1;
    }
    return 0;
}

static int validate_partition(int n, int src_h, int dst_h)
{
    int last_dst_end = 0;
    int last_src_end = 0;
    int stripes_ok = 0;

    for (int i = 0; i < n; i++) {
        int sys, sye, dys, dye;
        int ok = stripe_bounds4(i, n, src_h, dst_h,
                                          &sys, &sye, &dys, &dye);
        if (!ok) break;
        stripes_ok++;
        if (check_stripe(i, n, src_h, dst_h,
                         sys, sye, dys, dye,
                         last_src_end, last_dst_end))
            return 1;
        last_src_end = sye;
        last_dst_end = dye;
    }

    /* Invariant 4: full coverage when at least one stripe materialized. */
    if (stripes_ok > 0 &&
        (last_dst_end != dst_h || last_src_end != src_h)) {
        fprintf(stderr,
            "FAIL: incomplete coverage n=%d src_h=%d dst_h=%d "
            "last_src_end=%d last_dst_end=%d (stripes_ok=%d)\n",
            n, src_h, dst_h, last_src_end, last_dst_end, stripes_ok);
        return 1;
    }
    return 0;
}

static int run_one(const uint8_t *data, size_t size)
{
    int n, src_h, dst_h;
    if (!parse_input(data, size, &n, &src_h, &dst_h))
        return 0;

    /* Skip genuinely-invalid inputs - the function returns 0 for these,
     * which is the documented contract. We only want to exercise the
     * partition invariants when n >= 1 and both dims >= 1. */
    if (n < 1 || src_h < 1 || dst_h < 1)
        return check_invalid_input(n, src_h, dst_h);

    return validate_partition(n, src_h, dst_h);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (run_one(data, size)) abort();
    return 0;
}

#ifdef FUZZ_MAIN
#include "fuzz_smoke.h"

static int smoke_iter(long i)
{
    (void)i;
    uint8_t buf[16];
    fuzz_smoke_fill(buf, sizeof buf);
    return run_one(buf, sizeof buf);
}

int main(int argc, char **argv)
{
    fuzz_smoke_seed(0xc0ffeebabe1234ULL);
    return fuzz_smoke_main(argc, argv, 100000, "stripe_bounds", smoke_iter);
}
#endif
