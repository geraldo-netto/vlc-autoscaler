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
 *   2. its output contract holds for ANY input — checked as INDEPENDENT
 *      properties, not against a re-implementation (REL-4: the old mirror
 *      oracle was tautological):
 *        - *rows >= 1 and *cols >= 1, and
 *        - rows*cols <= the thread budget clamped to [1, UP_TILE_THREADS_MAX]
 *          (computed in 64-bit, so the check itself can't overflow), and
 *        - the stripe/tile floors hold: rows > 1 only if every stripe gets
 *          stripe_min dst rows, cols > 1 only if every tile gets col_min dst
 *          columns, and col_min <= 0 disables column tiling outright, and
 *        - monotonicity: one more thread never shrinks rows*cols.
 *      The exact maximize-workers/prefer-rows selection is deliberately NOT
 *      re-derived here; the deterministic suites pin representative grids.
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

static long long clamped_budget(int n_threads)
{
    long long b = n_threads < 1 ? 1 : n_threads;
    return b > UP_TILE_THREADS_MAX ? UP_TILE_THREADS_MAX : b;
}

static int grid_shape_ok(int rows, int cols, long long budget)
{
    /* 64-bit product so the comparison itself can't overflow. */
    return rows >= 1 && cols >= 1 && (long long)rows * cols <= budget;
}

/* Floor semantics, stated directly from the contract: splitting an axis is
 * legal only when every resulting stripe/tile still gets its minimum dst
 * extent; a non-positive col_min disables column tiling outright. */
static int grid_floors_ok(int rows, int cols, int dst_w, int dst_h,
                          int stripe_min, int col_min)
{
    if (stripe_min > 0 && rows > 1 && (long long)rows * stripe_min > dst_h)
        return 0;
    if (col_min <= 0)
        return cols == 1;
    return cols == 1 || (long long)cols * col_min <= dst_w;
}

static long long grid_cells(int n_threads, int dst_w, int dst_h,
                            int stripe_min, int col_min)
{
    int rows, cols;
    up_decide_tile_grid(n_threads, dst_w, dst_h, stripe_min, col_min,
                        &rows, &cols);
    return (long long)rows * cols;
}

static int check_grid(int n_threads, int dst_w, int dst_h,
                      int stripe_min, int col_min)
{
    int rows = -999, cols = -999;
    up_decide_tile_grid(n_threads, dst_w, dst_h, stripe_min, col_min,
                        &rows, &cols);
    long long cells = (long long)rows * cols;

    int bad = !grid_shape_ok(rows, cols, clamped_budget(n_threads))
           || !grid_floors_ok(rows, cols, dst_w, dst_h, stripe_min, col_min)
           /* Monotonicity: one more thread never shrinks the grid. */
           || (n_threads > INT_MIN
               && grid_cells(n_threads - 1, dst_w, dst_h,
                             stripe_min, col_min) > cells);
    if (bad) {
        fprintf(stderr, "FAIL: grid=%dx%d violates contract for "
                "n=%d dw=%d dh=%d sm=%d cm=%d\n",
                rows, cols, n_threads, dst_w, dst_h, stripe_min, col_min);
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
#include "fuzz_smoke.h"

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

/* Pseudo-random raw ints (full range) for anything the sweep missed. */
static int smoke_iter(long i)
{
    (void)i;
    uint8_t buf[20];
    fuzz_smoke_fill(buf, sizeof buf);
    return run_one(buf, sizeof buf);
}

int main(int argc, char **argv)
{
    int fails = sweep_boundaries();
    if (fails) {
        fprintf(stderr, "decide_tile_grid boundary sweep FAILED: %d\n", fails);
        return 1;
    }
    fuzz_smoke_seed(0x9E3779B9u);
    return fuzz_smoke_main(argc, argv, 200000, "decide_tile_grid", smoke_iter);
}
#endif
