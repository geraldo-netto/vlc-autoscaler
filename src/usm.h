// SPDX-License-Identifier: GPL-2.0-or-later
/*****************************************************************************
 * usm.h — unsharp-mask post-pass for AutoUpscale
 *****************************************************************************
 * Single-plane (luma) unsharp mask using a 3x3 separable Gaussian:
 *
 *     blur(x,y) = ([1 2 1]/4) ⊗ ([1 2 1]/4) over src
 *     out(x,y)  = clamp( src(x,y) + amount * (src(x,y) - blur(x,y)) )
 *
 * Edge handling: clamp-to-border (replicate edge pixels).
 *
 * Header-only with zero VLC/FFmpeg deps — same testability story as
 * upscale_logic.h. Apply only to the Y plane of YUV pictures; sharpening
 * RGB or chroma planes causes visible colour fringing on edges.
 *
 * Performance: two memory-bound passes over the luma plane. Throughput
 * depends on frame geometry, compiler, and CPU.
 *****************************************************************************/

#ifndef AUTOUPSCALE_USM_H
#define AUTOUPSCALE_USM_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* up_copy_plane: shared stride-aware plane copy, reused by the identity
 * fast path here and by usm_pool.c (DUP-1). Header-only, no extra deps. */
#include "plane_utils.h"

/* User-facing amount range: 0..200 (percent of "1.0" sharpening).
 * 0  = off
 * 20 = subtle (default)
 * 100 = strong
 * 200 = very strong */
#define UP_USM_AMOUNT_DEFAULT  20
#define UP_USM_AMOUNT_MAX      200

/* Normal Q8 range produced from the user-facing percentage is 0..512.
 * The callable single-threaded and pool APIs defensively accept any int and
 * clamp it to 0..4096, preserving the historical 16x ceiling for internal
 * and test callers without exposing that range as a user option. */
#define UP_USM_AMOUNT_Q8_NORMAL_MAX  512
#define UP_USM_AMOUNT_Q8_MAX         4096

/*
 * Convert a user-facing percentage (0..200) into Q8 fixed-point.
 * Clamps out-of-range values.
 */
static inline int up_usm_amount_pct_to_q8(int amount_pct)
{
    if (amount_pct <= 0) return 0;
    if (amount_pct > UP_USM_AMOUNT_MAX) amount_pct = UP_USM_AMOUNT_MAX;
    /* (pct * 256) / 100; both fit in int. */
    return (amount_pct * 256) / 100;
}

/* Horizontal 3-tap blur with [1,2,1]/4 kernel and edge replication.
 * `out` and `in` may not overlap. Width must be > 0.
 *
 * The inner pixel loop is the hottest in the plugin. GCC's lower
 * optimization level may decline vectorization on cost-model grounds;
 * production therefore compiles usm_pool.c at the Makefile's hot-path
 * optimization level. The policy stays translation-unit-wide rather than
 * relying on compiler-specific function pragmas. */
static inline void up_usm__hblur_row(uint8_t *restrict out,
                                     const uint8_t *restrict in,
                                     int width)
{
    if (width <= 0) return;
    if (width == 1) {
        out[0] = in[0];
        return;
    }
    /* Left edge: replicate in[0] for the missing in[-1]. */
    out[0] = (uint8_t)((in[0] * 3 + (int)in[1] + 2) >> 2);
    for (int x = 1; x < width - 1; x++) {
        out[x] = (uint8_t)(((int)in[x-1] + ((int)in[x] << 1)
                          + (int)in[x+1] + 2) >> 2);
    }
    /* Right edge: replicate in[width-1] for the missing in[width]. */
    out[width-1] = (uint8_t)(((int)in[width-2] + (int)in[width-1] * 3 + 2) >> 2);
}

/*
 * Internal: identity-copy fast path used when amount_q8 == 0. Skips the
 * memcpy entirely if dst aliases src with the same stride. Keeps the
 * dispatch in apply_plane simple.
 */
static inline void up_usm__apply_identity(
    uint8_t *dst, int dst_stride,
    const uint8_t *src, int src_stride,
    int width, int height)
{
    /* Skip the copy entirely when dst aliases src with the same stride. */
    if (dst == src && dst_stride == src_stride) return;
    /* Otherwise reuse the shared stride-aware plane copy (one memcpy when
     * both buffers are contiguous, else row-by-row). See up_copy_plane in
     * zimg_helpers.h. */
    up_copy_plane(dst, dst_stride, src, src_stride, width, height);
}

/* Clamp a Q8 sharpening amount to [0, UP_USM_AMOUNT_Q8_MAX]. Shared by the
 * threaded pool and its test oracle. */
static inline int up_usm__clamp_amount_q8(int amount_q8)
{
    if (amount_q8 < 0) return 0;
    if (amount_q8 > UP_USM_AMOUNT_Q8_MAX) return UP_USM_AMOUNT_Q8_MAX;
    return amount_q8;
}

static inline int up_usm__floor_div_q8(int value)
{
    int quotient = value / 256;
    if (value < 0 && value % 256 != 0) quotient--;
    return quotient;
}

/*
 * Internal: combine one row's blur and source values into the sharpened
 * destination row. The triangle blur kernel reads three workspace rows
 * (up_row, mid, dn_row) and the per-pixel detail = src - blur is added
 * back at amount_q8/256 strength, with [0,255] clamping.
 *
 * Hottest pixel loop in the project (called height× per frame). Like
 * up_usm__hblur_row above, it relies on the production translation unit's
 * optimization level to enable vectorization; no per-function pragma is used.
 *
 * dst_row and src_row may ALIAS (in-place USM: production sharpens the
 * VLC luma plane in place) — each x is read before it is written and
 * never re-read, so element-wise aliasing is safe, but they must NOT
 * carry `restrict`. The three blur rows are private scratch and never
 * alias dst/src; their `restrict` is what the vectorizer needs.
 */
static inline void up_usm__combine_row(
    uint8_t       *dst_row,
    const uint8_t *src_row,
    const uint8_t *restrict up_row,
    const uint8_t *restrict mid,
    const uint8_t *restrict dn_row,
    int width,
    int amount_q8)
{
    for (int x = 0; x < width; x++) {
        int blur = ((int)up_row[x] + ((int)mid[x] << 1)
                  + (int)dn_row[x] + 2) >> 2;
        int s = (int)src_row[x];
        int hi = s - blur;
        int sharpened = s + up_usm__floor_div_q8(amount_q8 * hi);
        if (sharpened < 0) sharpened = 0;
        else if (sharpened > 255) sharpened = 255;
        dst_row[x] = (uint8_t)sharpened;
    }
}

/* Shared plane-argument core: non-null buffers and strides wide enough for
 * `width` pixels. The pool and its test oracle layer their own checks. */
static inline int up_usm__plane_args_ok(
    const uint8_t *dst, int dst_stride,
    const uint8_t *src, int src_stride,
    int width)
{
    if (dst == NULL || src == NULL) return 0;
    if (dst_stride < width || src_stride < width) return 0;
    return 1;
}

#endif /* AUTOUPSCALE_USM_H */
