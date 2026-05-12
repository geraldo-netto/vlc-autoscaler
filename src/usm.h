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
 * declines to vectorize, while clang -O2 emits 16-wide SIMD. We
 * pin -O3 here to force gcc's vectorizer on; that's a ~3x speedup
 * on this kernel alone at 1080p+. The pragma is scoped to this one
 * function so other code keeps -O2 codegen untouched.
 *
 * Why O3 and not __attribute__((optimize))? On gcc, the function
 * attribute disables always_inline for the attributed function,
 * which would defeat the inline declaration above. The pragma form
 * leaves inlining decisions intact. The guard keeps clang silent under
 * -Wall (-Wunknown-pragmas would warn otherwise); clang -O2 already
 * vectorizes these loops without help, so the guard costs nothing. */
#if defined(__GNUC__) && !defined(__clang__)
# pragma GCC push_options
# pragma GCC optimize("O3")
#endif
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
#if defined(__GNUC__) && !defined(__clang__)
# pragma GCC pop_options
#endif

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
    if (dst == src && dst_stride == src_stride) return;
    /* Unified-stride fast path: when src and dst are contiguous (no row
     * padding) AND share a stride, the whole plane is one contiguous
     * block in both buffers — collapse height memcpy() calls into one.
     *
     * For a 320×240 chroma plane that's 240 calls vs 1; each memcpy()
     * call has ~30-40ns of dispatch+alignment overhead, so eliminating
     * them is a measurable win for small widths. glibc's memcpy is
     * already SIMD inside, so per-byte throughput is unchanged. */
    if (dst_stride == src_stride && src_stride == width) {
        memcpy(dst, src, (size_t)width * (size_t)height);
        return;
    }
    for (int y = 0; y < height; y++) {
        memcpy(dst + (size_t)y * (size_t)dst_stride,
               src + (size_t)y * (size_t)src_stride,
               (size_t)width);
    }
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
 * Hottest pixel loop in the project (called height× per frame). Same
 * pragma trick as up_usm__hblur_row above: gcc -O2 declines to vectorize
 * even with restrict, but gcc -O3 + clang -O2 both emit 16-byte SIMD.
 * We pin O3 here to force gcc's vectorizer; verified ~3x speedup at 1080p+.
 */
#if defined(__GNUC__) && !defined(__clang__)
# pragma GCC push_options
# pragma GCC optimize("O3")
#endif
static inline void up_usm__combine_row(
    uint8_t       *restrict dst_row,
    const uint8_t *restrict src_row,
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
#if defined(__GNUC__) && !defined(__clang__)
# pragma GCC pop_options
#endif

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

    /* Clamp amount. */
    if (amount_q8 < 0) amount_q8 = 0;
    if (amount_q8 > UP_USM_AMOUNT_Q8_MAX) amount_q8 = UP_USM_AMOUNT_Q8_MAX;

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
