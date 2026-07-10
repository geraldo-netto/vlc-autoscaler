// SPDX-License-Identifier: GPL-2.0-or-later
/*****************************************************************************
 * plane_utils.h - backend-neutral plane copy and 1D work partitioning
 *****************************************************************************
 * Helpers shared by more than one backend (zimg tiling, USM pool, tests):
 * a stride-aware plane copy and the even-aligned 1D stripe/tile partition.
 * Header-only with zero VLC, libzimg, or pthread dependencies (ARCH-2:
 * previously these lived in zimg_helpers.h, giving non-zimg consumers a
 * false zimg coupling).
 *****************************************************************************/

#ifndef AUTOUPSCALE_PLANE_UTILS_H
#define AUTOUPSCALE_PLANE_UTILS_H

#include <stdint.h>
#include <string.h>

#define UP_ALIGN_DOWN_2(x)  ((x) & ~1)

#define UP_TILE_THREADS_MAX     64

/*
 * copy_plane: memcpy `rows` rows of `row_bytes` bytes each, from src
 * (stride src_stride) to dst (stride dst_stride). When the strides are
 * equal AND match the row size, falls through to a single memcpy of the
 * whole block. Non-positive sizes and strides smaller than the row width are
 * no-ops; null pointers with otherwise valid geometry are undefined behaviour
 * (caller must validate).
 */
static inline void up_copy_plane(uint8_t *dst, int dst_stride,
                                 const uint8_t *src, int src_stride,
                                 int row_bytes, int rows)
{
    if (rows <= 0 || row_bytes <= 0) return;
    if (dst_stride < row_bytes || src_stride < row_bytes) return;
    if (dst_stride == src_stride && row_bytes == src_stride) {
        memcpy(dst, src, (size_t)rows * (size_t)row_bytes);
        return;
    }
    for (int r = 0; r < rows; r++)
        memcpy(dst + (size_t)r * (size_t)dst_stride,
               src + (size_t)r * (size_t)src_stride,
               (size_t)row_bytes);
}

/*
 * Compute the source and destination y-row ranges for stripe i out of n
 * in a slice-threaded resize. All four bounds are aligned down to even
 * values (chroma subsampling friendliness): the [start..end) intervals
 * partition the destination into contiguous stripes covering exactly
 * [0, dst_h), and similarly for the source covering [0, src_h).
 *
 * Returns 1 if the stripe is non-degenerate (both ranges have at least
 * 1 row), 0 if i, n, src_h, or dst_h have produced an empty stripe (the
 * caller should skip this stripe and reduce its worker count).
 *
 * The src range is computed proportionally to the dst range so that the
 * resize ratio src_h_stripe / dst_h_stripe stays close to the global
 * ratio src_h / dst_h, which is what the per-stripe zimg graph wants.
 *
 */
/* Extracted to keep up_compute_stripe_bounds within the complexity limit. The
 * five OR-ed clauses each count as a branch in the caller; collapsing
 * them into a single call keeps the bound calculation compact without
 * changing behaviour. */
static inline int up__stripe_args_valid(int i, int n, int src_h, int dst_h)
{
    if (n <= 0 || src_h <= 0 || dst_h <= 0) return 0;
    if (i < 0 || i >= n) return 0;
    return 1;
}

/* One stripe's source/destination range along a single axis. The same
 * partition is used for row stripes (heights) and column tiles (widths),
 * so the field names are axis-neutral. */
typedef struct {
    int src_start;
    int src_end;
    int dst_start;
    int dst_end;
} up_stripe_bounds_t;

static inline int up_compute_stripe_bounds(
    int i, int n, int src_h, int dst_h, up_stripe_bounds_t *out)
{
    if (!up__stripe_args_valid(i, n, src_h, dst_h)) return 0;

    out->dst_start = (i == 0) ? 0
        : UP_ALIGN_DOWN_2((int)((int64_t)i * dst_h / n));
    out->dst_end = (i == n - 1) ? dst_h
        : UP_ALIGN_DOWN_2((int)((int64_t)(i + 1) * dst_h / n));
    out->src_start = (i == 0) ? 0
        : UP_ALIGN_DOWN_2((int)((int64_t)src_h * out->dst_start / dst_h));
    out->src_end = (i == n - 1) ? src_h
        : UP_ALIGN_DOWN_2((int)((int64_t)src_h * out->dst_end / dst_h));

    return (out->dst_end > out->dst_start) && (out->src_end > out->src_start);
}

/*
 * SCAL-3: choose a (rows x cols) worker grid for `n_threads` workers given the
 * destination geometry. Rows partition HEIGHT (the existing horizontal
 * stripes); cols partition WIDTH (column tiles). Column tiling engages ONLY
 * when the height cannot host enough row-stripes to use every thread (very
 * wide / short frames) — otherwise cols stays 1 and the result is identical to
 * the row-stripe-only path.
 *
 *   stripe_min: minimum dst rows per stripe (height floor, > 0).
 *   col_min:    minimum dst cols per tile  (width floor,  > 0).
 *
 * up_compute_stripe_bounds() is reused verbatim for the column axis: it is a
 * pure even-aligned 1D partition, and even column boundaries keep chroma
 * subsampling exact. Writes *rows,*cols (each >= 1); rows*cols <= the thread
 * budget. The bounded search maximizes active workers, preferring more rows
 * on ties.
 */
static inline int up__tile_axis_limit(int extent, int minimum,
                                      int fallback, int budget)
{
    int limit = minimum > 0 ? extent / minimum : fallback;
    if (limit < 1) limit = 1;
    if (limit > budget) limit = budget;
    return limit;
}

static inline void up_decide_tile_grid(int n_threads, int dst_w, int dst_h,
                                       int stripe_min, int col_min,
                                       int *rows, int *cols)
{
    int budget = n_threads;
    if (budget < 1) budget = 1;
    if (budget > UP_TILE_THREADS_MAX) budget = UP_TILE_THREADS_MAX;

    int row_limit = up__tile_axis_limit(dst_h, stripe_min, budget, budget);
    int col_limit = up__tile_axis_limit(dst_w, col_min, 1, budget);
    int best_rows = 1;
    int best_cols = 1;
    int best_cells = 1;

    for (int r = 1; r <= row_limit; r++) {
        int c = budget / r;
        if (c > col_limit) c = col_limit;
        int cells = r * c;
        if (cells >= best_cells) {
            best_rows = r;
            best_cols = c;
            best_cells = cells;
        }
    }
    *rows = best_rows;
    *cols = best_cols;
}

#endif /* AUTOUPSCALE_PLANE_UTILS_H */
