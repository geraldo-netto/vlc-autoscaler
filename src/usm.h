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
 * Performance: two passes over the plane, both memory-bound. About 2 ns
 * per pixel on a modern x86 core with `-O2`. A 1080p Y plane (~2.07 MP)
 * costs ~4 ms per frame on one core — well within budget.
 *****************************************************************************/

#ifndef AUTOUPSCALE_USM_H
#define AUTOUPSCALE_USM_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* up_copy_plane: shared stride-aware plane copy, reused by the identity
 * fast path here and by usm_pool.c (DUP-1). Header-only, no extra deps. */
#include "zimg_helpers.h"

/* User-facing amount range: 0..200 (percent of "1.0" sharpening).
 * 0  = off
 * 20 = subtle (default)
 * 100 = strong
 * 200 = very strong */
#define UP_USM_AMOUNT_DEFAULT  20
#define UP_USM_AMOUNT_MAX      200

/* Internal Q8 range: 0..512. Anything above 4096 is rejected by
 * up_usm_apply_plane as nonsensical (16x sharpening). */
#define UP_USM_AMOUNT_Q8_MAX   4096

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

/*
 * Workspace size (in bytes) for one plane of the given dimensions.
 * Returns 0 on invalid input, oversized planes, or multiplication overflow.
 *
 * Hard cap: 16384 x 16384 (256 MB workspace). Anything larger than 8K
 * video isn't a use case this filter targets, and capping here prevents
 * absurd allocations even if a caller passes garbage dimensions.
 */
static inline size_t up_usm_workspace_size(int width, int height)
{
    if (width <= 0 || height <= 0) return 0;
    if (width > 16384 || height > 16384) return 0;
    size_t w = (size_t)width;
    size_t h = (size_t)height;
    if (w > SIZE_MAX / h) return 0;
    return w * h;
}

/* Horizontal 3-tap blur with [1,2,1]/4 kernel and edge replication.
 * `out` and `in` may not overlap. Width must be > 0.
 *
 * The inner pixel loop is the hottest in the entire plugin. With
 * -O2 + restrict, gcc reports "Loop costings not worthwhile" and
 * declines to vectorize; -O3 turns the vectorizer on (~3x at 1080p+),
 * and clang -O2 already emits 16-wide SIMD. We rely on the build
 * compiling the only production caller (usm_pool.c) at -O3 — the
 * Makefile's USM_POOL_CFLAGS substitutes -O2->-O3 for that TU, and the
 * benches do the same — rather than a per-function `#pragma GCC
 * optimize("O3")`, which is brittle across gcc versions and silently
 * no-ops if the function isn't inlined as expected (PERF-4). */
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
 * Apply unsharp mask to a single 8-bit plane.
 *
 *   dst, dst_stride : destination plane (may equal src for in-place)
 *   src, src_stride : source plane (read-only)
 *   width, height   : plane dimensions in pixels
 *   amount_q8       : sharpening amount in Q8 fixed point
 *                       0    = identity (output = input)
 *                       256  = 1.0 (typical)
 *                       512  = 2.0 (strong)
 *                     Negative values are clamped to 0; values above
 *                     UP_USM_AMOUNT_Q8_MAX are clamped down.
 *   workspace       : caller-provided buffer of at least
 *                     up_usm_workspace_size(width, height) bytes.
 *                     Contents on entry don't matter; on exit they're
 *                     a horizontal-blurred copy of src (caller may reuse).
 *
 * Returns 1 on success, 0 on invalid input. On failure the destination
 * plane is left unchanged.
 *
 * In-place is supported (dst == src, same stride). The implementation
 * reads each row's source pixel before writing the corresponding
 * destination pixel within an iteration, and never re-reads it across
 * iterations, so aliasing is safe.
 *
 * NOTE (WIRE-2): production does NOT call this directly — the plugin
 * routes USM through the threaded up_usm_pool_apply(). This single-
 * threaded version is deliberately retained as the BYTE-IDENTITY TEST
 * ORACLE: tests/test_usm_pool.c, tests/stress_usm_pool.c and the variant
 * suites assert the pool's output matches this function bit-for-bit. It
 * is intentionally test-only, not a dead/unwired path.
 */
/*
 * Internal: identity-copy fast path used when amount_q8 == 0. Skips the
 * memcpy entirely if dst aliases src with the same stride. Keeps the
 * dispatch in apply_plane simple. CCN 3.
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
 * single-threaded apply and the threaded pool (DUP-4). */
static inline int up_usm__clamp_amount_q8(int amount_q8)
{
    if (amount_q8 < 0) return 0;
    if (amount_q8 > UP_USM_AMOUNT_Q8_MAX) return UP_USM_AMOUNT_Q8_MAX;
    return amount_q8;
}

/*
 * Internal: pass 1 of the USM. Horizontal-blur every source row into the
 * dense workspace buffer. CCN 2.
 */
static inline void up_usm__pass1_hblur(
    uint8_t *workspace,
    const uint8_t *src, int src_stride,
    int width, int height)
{
    for (int y = 0; y < height; y++) {
        up_usm__hblur_row(
            workspace + (size_t)y * (size_t)width,
            src + (size_t)y * (size_t)src_stride,
            width);
    }
}

/*
 * Internal: combine one row's blur and source values into the sharpened
 * destination row. The triangle blur kernel reads three workspace rows
 * (up_row, mid, dn_row) and the per-pixel detail = src - blur is added
 * back at amount_q8/256 strength, with [0,255] clamping. CCN 4.
 *
 * Hottest pixel loop in the project (called height× per frame). Like
 * up_usm__hblur_row above, it relies on the production TU (usm_pool.c)
 * being compiled at -O3 for gcc's vectorizer rather than a per-function
 * pragma (PERF-4); gcc -O3 and clang -O2 both emit 16-byte SIMD here.
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
        int sharpened = s + ((amount_q8 * hi) >> 8);
        if (sharpened < 0) sharpened = 0;
        else if (sharpened > 255) sharpened = 255;
        dst_row[x] = (uint8_t)sharpened;
    }
}

/*
 * Internal: pass 2 of the USM. For each row y, picks workspace rows
 * y-1, y, y+1 (clamped at boundaries), then combines via the per-row
 * helper. CCN 4.
 */
static inline void up_usm__pass2_combine(
    uint8_t *dst, int dst_stride,
    const uint8_t *src, int src_stride,
    const uint8_t *workspace,
    int width, int height,
    int amount_q8)
{
    for (int y = 0; y < height; y++) {
        int yu = (y > 0) ? (y - 1) : 0;
        int yd = (y < height - 1) ? (y + 1) : (height - 1);
        up_usm__combine_row(
            dst + (size_t)y * (size_t)dst_stride,
            src + (size_t)y * (size_t)src_stride,
            workspace + (size_t)yu * (size_t)width,
            workspace + (size_t)y  * (size_t)width,
            workspace + (size_t)yd * (size_t)width,
            width, amount_q8);
    }
}

/* Extracted to cap the CCN of up_usm_apply_plane at <=10. The six
 * boolean clauses below would otherwise count one branch each in the
 * caller. */
static inline int up_usm__args_valid(
    const uint8_t *dst, int dst_stride,
    const uint8_t *src, int src_stride,
    int width, int height)
{
    if (dst == NULL || src == NULL) return 0;
    if (width <= 0 || height <= 0) return 0;
    if (dst_stride < width || src_stride < width) return 0;
    return 1;
}

static inline int up_usm_apply_plane(
    uint8_t *dst, int dst_stride,
    const uint8_t *src, int src_stride,
    int width, int height,
    int amount_q8,
    uint8_t *workspace)
{
    if (!up_usm__args_valid(dst, dst_stride, src, src_stride, width, height))
        return 0;

    amount_q8 = up_usm__clamp_amount_q8(amount_q8);

    /* Identity fast path. */
    if (amount_q8 == 0) {
        up_usm__apply_identity(dst, dst_stride, src, src_stride,
                               width, height);
        return 1;
    }

    /* Anything else needs workspace. */
    if (workspace == NULL) return 0;

    up_usm__pass1_hblur(workspace, src, src_stride, width, height);
    up_usm__pass2_combine(dst, dst_stride, src, src_stride,
                          workspace, width, height, amount_q8);
    return 1;
}

#endif /* AUTOUPSCALE_USM_H */
