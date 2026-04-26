/*****************************************************************************
 * scaler_zimg_chroma.h - pure chroma->subsample mapping for zimg backend
 *****************************************************************************
 * Header-only mapping from VLC chroma fourccs (passed as uint32_t) to
 * zimg's (sub_w, sub_h, yv12_swap) triple. Pulled out of scaler_zimg.c so
 * the same mapping is exercised by both the production path and a fuzzer
 * that doesn't depend on VLC headers.
 *
 *   sub_w / sub_h: subsample shift exponent (0 = no subsample, 1 = half)
 *                  in the horizontal / vertical axis. zimg uses these
 *                  identically to plane_pitch/plane_lines in zimg_helpers.h.
 *
 *   yv12_swap:     1 for YV12 (planes are Y, V, U), 0 otherwise.
 *
 * Returns 1 on supported chroma, 0 on rejection (NV12, packed, RGB, opaque).
 * The function never reads or writes anything besides the four output ints.
 *
 * The rejection list deliberately excludes NV12/NV21: they're 4:2:0 but
 * semi-planar (interleaved UV), which zimg's plane-array API doesn't
 * accept. The swscale backend handles those formats instead.
 *****************************************************************************/

#ifndef AUTOUPSCALE_SCALER_ZIMG_CHROMA_H
#define AUTOUPSCALE_SCALER_ZIMG_CHROMA_H

#include "chroma_classify.h"  /* UP_FOURCC macro */

#include <stdint.h>

/*
 * Map a VLC chroma fourcc to zimg's (sub_w, sub_h, yv12_swap).
 * Returns 1 on supported chroma, 0 on unsupported (callers should fall
 * through to swscale). All output ints are populated on success; on
 * failure they are left untouched and the caller must not read them.
 */
static inline int up_chroma_to_zimg(uint32_t c,
                                    unsigned *sub_w, unsigned *sub_h,
                                    int *yv12_swap)
{
    if (sub_w == NULL || sub_h == NULL || yv12_swap == NULL)
        return 0;
    switch (c) {
        case UP_FOURCC('I','4','2','0'):
            *sub_w = 1; *sub_h = 1; *yv12_swap = 0; return 1;
        case UP_FOURCC('Y','V','1','2'):
            *sub_w = 1; *sub_h = 1; *yv12_swap = 1; return 1;
        case UP_FOURCC('I','4','2','2'):
            *sub_w = 1; *sub_h = 0; *yv12_swap = 0; return 1;
        case UP_FOURCC('I','4','4','4'):
            *sub_w = 0; *sub_h = 0; *yv12_swap = 0; return 1;
        default:
            return 0;
    }
}

#endif /* AUTOUPSCALE_SCALER_ZIMG_CHROMA_H */
