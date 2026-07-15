// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef AUTOUPSCALE_USM_REFERENCE_H
#define AUTOUPSCALE_USM_REFERENCE_H

#include "../src/usm.h"

typedef struct {
    uint8_t *dst;
    int dst_stride;
    const uint8_t *src;
    int src_stride;
    int width;
    int height;
} up_usm_plane_io_t;

static inline size_t up_usm_workspace_size(int width, int height)
{
    if (width <= 0 || height <= 0) return 0;
    if (width > 16384 || height > 16384) return 0;
    size_t w = (size_t)width;
    size_t h = (size_t)height;
    if (w > SIZE_MAX / h) return 0;
    return w * h;
}

static inline void up_usm__pass1_hblur(
    uint8_t *workspace, const uint8_t *src, int src_stride,
    int width, int height)
{
    for (int y = 0; y < height; y++) {
        up_usm__hblur_row(workspace + (size_t)y * (size_t)width,
                          src + (size_t)y * (size_t)src_stride, width);
    }
}

static inline void up_usm__pass2_combine(
    const up_usm_plane_io_t *io, const uint8_t *workspace, int amount_q8)
{
    const int width = io->width;
    const int height = io->height;
    for (int y = 0; y < height; y++) {
        int yu = y > 0 ? y - 1 : 0;
        int yd = y < height - 1 ? y + 1 : height - 1;
        up_usm__combine_row(
            io->dst + (size_t)y * (size_t)io->dst_stride,
            io->src + (size_t)y * (size_t)io->src_stride,
            workspace + (size_t)yu * (size_t)width,
            workspace + (size_t)y * (size_t)width,
            workspace + (size_t)yd * (size_t)width,
            width, amount_q8);
    }
}

static inline int up_usm__args_valid(
    const uint8_t *dst, int dst_stride, const uint8_t *src, int src_stride,
    int width, int height)
{
    if (width <= 0 || height <= 0) return 0;
    return up_usm__plane_args_ok(dst, dst_stride, src, src_stride, width);
}

static inline int up_usm_apply_plane(
    const up_usm_plane_io_t *io, int amount_q8, uint8_t *workspace)
{
    if (io == NULL) return 0;
    if (!up_usm__args_valid(io->dst, io->dst_stride, io->src, io->src_stride,
                            io->width, io->height))
        return 0;
    amount_q8 = up_usm__clamp_amount_q8(amount_q8);
    if (amount_q8 == 0) {
        up_usm__apply_identity(io->dst, io->dst_stride, io->src,
                               io->src_stride, io->width, io->height);
        return 1;
    }
    if (workspace == NULL) return 0;
    up_usm__pass1_hblur(workspace, io->src, io->src_stride,
                        io->width, io->height);
    up_usm__pass2_combine(io, workspace, amount_q8);
    return 1;
}

#endif
