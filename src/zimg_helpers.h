// SPDX-License-Identifier: GPL-2.0-or-later
/*****************************************************************************
 * zimg_helpers.h - pure helpers extracted from scaler_zimg.c
 *****************************************************************************
 * Geometry math and a stride-aware plane copy. Header-only with zero VLC,
 * libzimg, or pthread dependencies so unit tests and fuzzers can include
 * it directly. The same definitions are used by scaler_zimg.c for the
 * production path.
 *****************************************************************************/

#ifndef AUTOUPSCALE_ZIMG_HELPERS_H
#define AUTOUPSCALE_ZIMG_HELPERS_H

#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define UP_ALIGN_DOWN_2(x)  ((x) & ~1)

/* Pitch alignment: each scratch row begins on a 64-byte boundary. Matches
 * AVX-512 line size and zimg's preferred SIMD alignment. */
#define UP_PITCH_ALIGN          64

/* Line padding: extra rows allocated past the visible image so that
 * zimg's resampling kernel boundary access has headroom. 8 rows is
 * comfortably above any zimg filter's vertical kernel half-width
 * (Spline36 = 6 taps -> 3 rows above + 3 below). */
#define UP_SCRATCH_LINE_PAD     8

/* Minimum destination rows per slice-threaded stripe. Below this, the
 * resampling kernel's boundary handling dominates and adding more
 * stripes hurts quality without adding throughput. */
#define UP_STRIPE_MIN_DST_LINES 16

#define UP_TILE_THREADS_MAX     64

/*
 * Resolve the user-tunable zimg stripe-min-lines value. Pass 0 (or any
 * non-positive sentinel) to fall back to the compile-time default
 * UP_STRIPE_MIN_DST_LINES; any positive value is returned as-is.
 *
 * Pulled out to a helper so both the production zimg backend and the
 * unit tests share the same lookup: any change to the sentinel
 * convention only needs to update one place.
 */
static inline int up_zimg_stripe_min_lines(int user_value)
{
    return user_value > 0 ? user_value : UP_STRIPE_MIN_DST_LINES;
}

/*
 * round_up_pitch(w): smallest multiple of UP_PITCH_ALIGN >= w. Used to
 * size scratch buffer rows so each row begins on an aligned boundary
 * (helps SIMD loads and matches zimg's preferred alignment). Caller
 * must guarantee w >= 0; for w == 0 the result is 0 (no row, harmless).
 */
static inline int up_round_up_pitch(int w)
{
    if (w <= 0) return 0;
    /* Reject inputs that would overflow `w + (UP_PITCH_ALIGN - 1)` as a
     * signed int. With UP_PITCH_ALIGN == 64 the cap is INT_MAX - 63;
     * any plane wider than that is far past anything VLC will hand us. */
    if (w > INT_MAX - (UP_PITCH_ALIGN - 1)) return 0;
    return (w + (UP_PITCH_ALIGN - 1)) & ~(UP_PITCH_ALIGN - 1);
}

/*
 * round_up_lines(h): h plus UP_SCRATCH_LINE_PAD rows of padding for
 * zimg's resampling kernel boundary access. Negative input clamps to 0.
 */
static inline int up_round_up_lines(int h)
{
    if (h <= 0) return 0;
    if (h > INT_MAX - UP_SCRATCH_LINE_PAD) return 0;
    return h + UP_SCRATCH_LINE_PAD;
}

/*
 * chroma_dim(v, sub): the subsampled extent `ceil(v / 2^sub)` for a chroma
 * plane whose luma extent is `v` and subsample exponent is `sub` (0 = none,
 * 1 = half). Single definition of the round-up shared by plane sizing and by
 * the per-stripe copy-in/out in scaler_zimg.c (DUP-6). Returns 0 on
 * non-positive/negative input; clamps `sub` to a sane range.
 */
static inline int up_chroma_dim(int v, int sub)
{
    if (v <= 0 || sub < 0) return 0;
    int s = (sub > 16) ? 16 : sub;
    int divisor = 1 << s;
    return v / divisor + (v % divisor != 0);
}

/*
 * plane_pitch(w, sub_w): pitch in bytes for a plane whose visible width
 * is `w >> sub_w`, rounded up. sub_w is the chroma horizontal subsample
 * exponent (0 = no subsample, 1 = half, 2 = quarter).
 */
static inline int up_plane_pitch(int w, int sub_w)
{
    if (w <= 0 || sub_w < 0) return 0;
    return up_round_up_pitch(up_chroma_dim(w, sub_w));
}

/*
 * plane_lines(h, sub_h): allocated row count for a plane whose visible
 * height is `h >> sub_h`. Includes the small kernel-boundary padding.
 */
static inline int up_plane_lines(int h, int sub_h)
{
    if (h <= 0 || sub_h < 0) return 0;
    return up_round_up_lines(up_chroma_dim(h, sub_h));
}

/*
 * zimg_plane_idx(idx, swap): plane index map. zimg expects YUV order
 * (Y=0, U=1, V=2). VLC's YV12 chroma is stored V before U, so we swap
 * indices 1 and 2 when reading/writing YV12 planes.
 */
static inline int up_zimg_plane_idx(int idx, int swap)
{
    if (!swap) return idx;
    if (idx == 1) return 2;
    if (idx == 2) return 1;
    return idx;
}

/*
 * copy_plane: memcpy `rows` rows of `row_bytes` bytes each, from src
 * (stride src_stride) to dst (stride dst_stride). When the strides are
 * equal AND match the row size, falls through to a single memcpy of the
 * whole block. Negative inputs are no-ops; null pointers with positive
 * sizes are undefined behaviour (caller must validate).
 */
static inline void up_copy_plane(uint8_t *dst, int dst_stride,
                                 const uint8_t *src, int src_stride,
                                 int row_bytes, int rows)
{
    if (rows <= 0 || row_bytes <= 0) return;
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
 * CCN 5.
 */
/* Extracted to cap the CCN of up_compute_stripe_bounds at <=10. The
 * five OR-ed clauses each count as a branch in the caller; collapsing
 * them into a single call reduces the bound calculation's CCN from 11
 * to 6 without changing behaviour. */
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
 * on ties. CCN 6.
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

#endif /* AUTOUPSCALE_ZIMG_HELPERS_H */
