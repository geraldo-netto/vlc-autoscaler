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

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define UP_ALIGN_DOWN_2(x)  ((x) & ~1)

/*
 * round_up_pitch(w): smallest multiple of 64 >= w. Used to size scratch
 * buffer rows so each row begins on a 64-byte boundary (helps SIMD loads
 * and matches zimg's preferred alignment). Caller must guarantee w >= 0;
 * for w == 0 the result is 0 (no row, harmless).
 */
static inline int up_round_up_pitch(int w)
{
    if (w <= 0) return 0;
    return (w + 63) & ~63;
}

/*
 * round_up_lines(h): h plus a small padding allowance for zimg's
 * resampling kernel boundary access. 8 extra lines is comfortably above
 * any zimg filter's vertical kernel half-width (Spline36 = 6 taps, so
 * 3 above + 3 below is the worst case). Negative input clamps to 0.
 */
static inline int up_round_up_lines(int h)
{
    if (h <= 0) return 0;
    return h + 8;
}

/*
 * plane_pitch(w, sub_w): pitch in bytes for a plane whose visible width
 * is `w >> sub_w`, rounded up. sub_w is the chroma horizontal subsample
 * exponent (0 = no subsample, 1 = half, 2 = quarter).
 */
static inline int up_plane_pitch(int w, int sub_w)
{
    if (w <= 0 || sub_w < 0) return 0;
    int sub = (sub_w > 16) ? 16 : sub_w;
    int row = (w + (1 << sub) - 1) >> sub;
    return up_round_up_pitch(row);
}

/*
 * plane_lines(h, sub_h): allocated row count for a plane whose visible
 * height is `h >> sub_h`. Includes the small kernel-boundary padding.
 */
static inline int up_plane_lines(int h, int sub_h)
{
    if (h <= 0 || sub_h < 0) return 0;
    int sub = (sub_h > 16) ? 16 : sub_h;
    int rows = (h + (1 << sub) - 1) >> sub;
    return up_round_up_lines(rows);
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

#endif /* AUTOUPSCALE_ZIMG_HELPERS_H */
