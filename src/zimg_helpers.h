// SPDX-License-Identifier: GPL-2.0-or-later
/*****************************************************************************
 * zimg_helpers.h - zimg-specific geometry helpers for scaler_zimg.c
 *****************************************************************************
 * Scratch pitch/line sizing, chroma plane geometry, and the YV12 plane
 * index swap. Header-only with zero VLC, libzimg, or pthread deps so
 * unit tests and fuzzers can include it directly. The backend-neutral
 * plane copy and 1D partition helpers live in plane_utils.h (ARCH-2).
 *****************************************************************************/

#ifndef AUTOUPSCALE_ZIMG_HELPERS_H
#define AUTOUPSCALE_ZIMG_HELPERS_H

#include "plane_utils.h"

#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Pitch alignment: each scratch row begins on a 64-byte boundary. Matches
 * AVX-512 line size and zimg's preferred SIMD alignment. */
#define UP_PITCH_ALIGN          64

/* Line padding: extra rows allocated past the visible image so that
 * zimg's resampling kernel boundary access has headroom. 8 rows is
 * comfortably above any zimg filter's vertical kernel half-width
 * (Spline36 = 6 taps -> 3 rows above + 3 below). */
#define UP_SCRATCH_LINE_PAD     8

/* Minimum destination rows per slice-threaded stripe. This bounds graph count
 * and per-stripe boundary overhead. */
#define UP_STRIPE_MIN_DST_LINES 16

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
 * zimg I/O plan resolver (PAT-1) — the single owner of the grid/zero-copy
 * invariant chain that was previously enforced at three sites in
 * scaler_zimg.c (open, rows-only transition, first-frame re-check):
 *
 *   - column tiling requires source-direct reads: without src zero-copy
 *     the grid resolves rows-only (col_min 0 caps cols at 1);
 *   - a retained tiled grid writes per-tile scratch, so dst zero-copy
 *     is forced off (col_tiled => copy-out).
 *
 * Pure and deterministic: called at open (storage alignment unknown, so
 * the caller passes the raw options) and re-run once on the first frame
 * with alignment folded into the requested flags. Resolving a plan's own
 * flags again returns the identical plan (fixed point), which is what
 * makes the two call sites consistent by construction.
 */
typedef struct {
    int  worker_budget;   /* threads available, >= 1 */
    int  src_w;
    int  src_h;           /* source geometry (bounds the cell count: REL-2) */
    int  dst_w;
    int  dst_h;    /* destination geometry */
    int  stripe_min;      /* min dst rows per stripe */
    int  col_min;         /* min dst cols per tile */
    bool src_zerocopy;    /* requested AND storage-permitted */
    bool dst_zerocopy;
} up_zimg_io_req_t;

typedef struct {
    int  n_rows;
    int  n_cols;
    int  n_threads;       /* == n_rows * n_cols */
    bool col_tiled;       /* == (n_cols > 1) */
    bool src_zerocopy;
    bool dst_zerocopy;
} up_zimg_io_plan_t;

static inline void up_zimg_resolve_io_plan(const up_zimg_io_req_t *req,
                                           up_zimg_io_plan_t *out)
{
    int rows;
    int cols;
    const up_tile_geom_t geom = {
        .src_w = req->src_w,
        .src_h = req->src_h,
        .dst_w = req->dst_w,
        .dst_h = req->dst_h,
    };
    up_decide_tile_grid(req->worker_budget, &geom, req->stripe_min,
                        req->src_zerocopy ? req->col_min : 0,
                        &rows, &cols);
    out->n_rows       = rows;
    out->n_cols       = cols;
    out->n_threads    = rows * cols;
    out->col_tiled    = cols > 1;
    out->src_zerocopy = req->src_zerocopy;
    out->dst_zerocopy = req->dst_zerocopy && cols <= 1;
}

#endif /* AUTOUPSCALE_ZIMG_HELPERS_H */
