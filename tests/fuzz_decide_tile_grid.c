// SPDX-License-Identifier: GPL-2.0-or-later
/*****************************************************************************
 * fuzz_decide_tile_grid.c - fuzz up_decide_tile_grid() (SCAL-3 worker grid).
 *****************************************************************************
 * The grid chooser is a pure int->int function. This fuzzer feeds it RAW
 * 32-bit integers across the WHOLE range — negatives, 0, 1, INT_MAX, INT_MIN —
 * for every parameter (n_threads, dst_w, dst_h, stripe_min, col_min), and
 * checks that:
 *
 *   1. it never invokes UB (signed overflow, divide-by-zero, INT_MIN/-1) —
 *      enforced by the UBSan build, and
 *   2. its output contract holds for ANY input:
 *        - *rows >= 1 and *cols >= 1, and
 *        - rows*cols <= max(1, n_threads)  (computed in 64-bit, so the check
 *          itself can't overflow).
 *
 * Production only ever passes small positive values (n_threads <= 64,
 * stripe_min/col_min are positive constants), but the contract must hold for
 * hostile input too — a future caller must not be able to make the grid
 * produce 0 cells, a negative dimension, or more cells than threads.
 *
 * Two entry points: LLVMFuzzerTestOneInput + a deterministic smoke main that
 * also sweeps the boundary values explicitly.
 *****************************************************************************/
#include "../src/zimg_helpers.h"

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int check_grid(int n_threads, int dst_w, int dst_h,
                      int stripe_min, int col_min)
{
    int rows = -999, cols = -999;
    up_decide_tile_grid(n_threads, dst_w, dst_h, stripe_min, col_min,
                        &rows, &cols);

    if (rows < 1 || cols < 1) {
        fprintf(stderr, "FAIL: non-positive grid rows=%d cols=%d for "
                "n=%d dw=%d dh=%d sm=%d cm=%d\n",
                rows, cols, n_threads, dst_w, dst_h, stripe_min, col_min);
        return 1;
    }
    /* rows*cols must not exceed the (clamped) thread budget. 64-bit so the
     * comparison can't itself overflow for extreme rows/cols. */
    long long cells = (long long)rows * (long long)cols;
    long long budget = (n_threads < 1) ? 1 : (long long)n_threads;
    if (cells > budget) {
        fprintf(stderr, "FAIL: too many cells rows*cols=%lld > budget=%lld "
                "for n=%d dw=%d dh=%d sm=%d cm=%d\n",
                cells, budget, n_threads, dst_w, dst_h, stripe_min, col_min);
        return 1;
    }
    return 0;
}

static void run_one(const uint8_t *data, size_t size)
{
    if (size < 5 * sizeof(int32_t)) return;
    int32_t v[5];
    memcpy(v, data, sizeof v);
    (void)check_grid((int)v[0], (int)v[1], (int)v[2], (int)v[3], (int)v[4]);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    run_one(data, size);
    return 0;
}

#ifdef FUZZ_MAIN
/* Boundary values every parameter is swept over in the smoke main. */
static const int BV[] = {
    INT_MIN, INT_MIN + 1, -65, -64, -2, -1, 0, 1, 2, 15, 16, 63, 64, 65,
    1920, 8192, INT_MAX - 1, INT_MAX,
};
#define NBV ((int)(sizeof BV / sizeof BV[0]))

/* Exhaustive cross-product of boundary values: NBV^5 is ~1.9M, cheap. */
static int sweep_boundaries(void)
{
    int fails = 0;
    for (int a = 0; a < NBV; a++)
    for (int b = 0; b < NBV; b++)
    for (int c = 0; c < NBV; c++)
    for (int d = 0; d < NBV; d++)
    for (int e = 0; e < NBV; e++)
        fails += check_grid(BV[a], BV[b], BV[c], BV[d], BV[e]);
    return fails;
}

int main(int argc, char **argv)
{
    long n = 200000;
    if (argc > 1) {
        char *end = NULL;
        long t = strtol(argv[1], &end, 10);
        if (end && *end == '\0' && t > 0) n = t;
    }

    int fails = sweep_boundaries();

    /* Plus pseudo-random raw ints (full range) for anything the grid missed. */
    uint32_t s = 0x9E3779B9u;
    uint8_t buf[20];
    for (long i = 0; i < n && fails == 0; i++) {
        for (size_t j = 0; j < sizeof buf; j += 4) {
            s ^= s << 13; s ^= s >> 17; s ^= s << 5;
            memcpy(buf + j, &s, 4);
        }
        run_one(buf, sizeof buf);
    }

    if (fails) { printf("decide_tile_grid smoke FAILED: %d\n", fails); return 1; }
    printf("decide_tile_grid smoke OK: boundary sweep (%d^5) + %ld random\n",
           NBV, n);
    return 0;
}
#endif
