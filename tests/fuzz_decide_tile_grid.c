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
 *          itself can't overflow), and
 *        - the grid maximizes active workers within the row/column limits,
 *          preferring more row stripes on ties.
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

static int oracle_axis_limit(int extent, int minimum,
                             int fallback, int budget)
{
    int limit = minimum > 0 ? extent / minimum : fallback;
    if (limit < 1) limit = 1;
    if (limit > budget) limit = budget;
    return limit;
}

static void oracle_grid(int n_threads, int dst_w, int dst_h,
                        int stripe_min, int col_min,
                        int *rows, int *cols)
{
    int budget = n_threads;
    if (budget < 1) budget = 1;
    if (budget > UP_TILE_THREADS_MAX) budget = UP_TILE_THREADS_MAX;
    int row_limit = oracle_axis_limit(dst_h, stripe_min, budget, budget);
    int col_limit = oracle_axis_limit(dst_w, col_min, 1, budget);
    int best_cells = 0;
    *rows = 1;
    *cols = 1;
    for (int r = 1; r <= row_limit; r++) {
        int c = col_limit;
        if (c > budget / r) c = budget / r;
        int cells = r * c;
        if (cells > best_cells || (cells == best_cells && r > *rows)) {
            best_cells = cells;
            *rows = r;
            *cols = c;
        }
    }
}

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
    int expected_rows, expected_cols;
    oracle_grid(n_threads, dst_w, dst_h, stripe_min, col_min,
                &expected_rows, &expected_cols);
    if (rows != expected_rows || cols != expected_cols) {
        fprintf(stderr, "FAIL: grid=%dx%d expected=%dx%d "
                "for n=%d dw=%d dh=%d sm=%d cm=%d\n",
                rows, cols, expected_rows, expected_cols,
                n_threads, dst_w, dst_h, stripe_min, col_min);
        return 1;
    }
    return 0;
}

/* PAT-1 plan invariants: cols collapse without src zero-copy, tiling
 * forces dst copy-out, counts stay consistent, and the resolver is a
 * fixed point of its own output flags. */
static int check_plan(int n_threads, int dst_w, int dst_h,
                      int stripe_min, int col_min)
{
    const up_zimg_io_req_t req = {
        .worker_budget = n_threads, .dst_w = dst_w, .dst_h = dst_h,
        .stripe_min = stripe_min, .col_min = col_min,
        .src_zerocopy = (n_threads & 1) != 0,
        .dst_zerocopy = (dst_w & 1) != 0,
    };
    up_zimg_io_plan_t p1, p2;
    up_zimg_resolve_io_plan(&req, &p1);

    int bad = (p1.n_threads != p1.n_rows * p1.n_cols)
           || (p1.col_tiled != (p1.n_cols > 1))
           || (!req.src_zerocopy && p1.n_cols != 1)
           || (p1.col_tiled && p1.dst_zerocopy)
           || (p1.src_zerocopy != req.src_zerocopy);

    up_zimg_io_req_t again = req;
    again.src_zerocopy = p1.src_zerocopy;
    again.dst_zerocopy = p1.dst_zerocopy;
    up_zimg_resolve_io_plan(&again, &p2);
    bad = bad || memcmp(&p1, &p2, sizeof p1) != 0;

    if (bad) {
        fprintf(stderr, "FAIL: io plan invariant broke for n=%d dw=%d "
                "dh=%d sm=%d cm=%d szc=%d dzc=%d -> %dx%d tiled=%d "
                "szc=%d dzc=%d\n",
                n_threads, dst_w, dst_h, stripe_min, col_min,
                (int)req.src_zerocopy, (int)req.dst_zerocopy,
                p1.n_rows, p1.n_cols, (int)p1.col_tiled,
                (int)p1.src_zerocopy, (int)p1.dst_zerocopy);
        return 1;
    }
    return 0;
}

static int run_one(const uint8_t *data, size_t size)
{
    if (size < 5 * sizeof(int32_t)) return 0;
    int32_t v[5];
    memcpy(v, data, sizeof v);
    return check_grid((int)v[0], (int)v[1], (int)v[2],
                      (int)v[3], (int)v[4])
         + check_plan((int)v[0], (int)v[1], (int)v[2],
                      (int)v[3], (int)v[4]);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (run_one(data, size)) abort();
    return 0;
}

#ifdef FUZZ_MAIN
/* Boundary values every parameter is swept over in the smoke main. */
static const int BV[] = {
    INT_MIN, INT_MIN + 1, -65, -64, -2, -1, 0, 1, 2, 15, 16, 63, 64, 65,
    1920, 8192, INT_MAX - 1, INT_MAX,
};
#define NBV ((int)(sizeof BV / sizeof BV[0]))

/* Exhaustive cross-product of the boundary-value table. */
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
        fails += run_one(buf, sizeof buf);
    }

    if (fails) { printf("decide_tile_grid smoke FAILED: %d\n", fails); return 1; }
    printf("decide_tile_grid smoke OK: boundary sweep (%d^5) + %ld random\n",
           NBV, n);
    return 0;
}
#endif
