// SPDX-License-Identifier: GPL-2.0-or-later
/*****************************************************************************
 * scaler_zimg_chroma.h - neutral chroma metadata adapter for zimg
 *****************************************************************************
 * Adapts the shared software-chroma descriptor to zimg's
 * (sub_w, sub_h, yv12_swap) triple. Kept separate so production and the
 * VLC-free fuzzer exercise the same backend boundary.
 *
 *   sub_w / sub_h: subsample shift exponent (0 = no subsample, 1 = half)
 *                  in the horizontal / vertical axis. zimg uses these
 *                  identically to plane_pitch/plane_lines in zimg_helpers.h.
 *
 *   yv12_swap:     1 for YV12 (planes are Y, V, U), 0 otherwise.
 *
 * Returns 1 on supported chroma, 0 on rejection (NV12, packed, RGB, opaque).
 * The function has no side effects and writes outputs only on success.
 *
 * NV12/NV21 descriptors are deliberately rejected: they are semi-planar
 * (interleaved UV), which zimg's plane-array API does not accept. swscale
 * handles those formats instead.
 *****************************************************************************/

#ifndef AUTOUPSCALE_SCALER_ZIMG_CHROMA_H
#define AUTOUPSCALE_SCALER_ZIMG_CHROMA_H

#include "chroma_classify.h"

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
    const up_chroma_descriptor_t *desc = up_chroma_descriptor(c);
    if (desc == NULL || !up_chroma_layout_is_planar(desc->layout))
        return 0;
    *sub_w = desc->sub_w;
    *sub_h = desc->sub_h;
    *yv12_swap = desc->uv_planes_swapped;
    return 1;
}

#endif /* AUTOUPSCALE_SCALER_ZIMG_CHROMA_H */
