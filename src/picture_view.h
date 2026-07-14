// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef AUTOUPSCALE_PICTURE_VIEW_H
#define AUTOUPSCALE_PICTURE_VIEW_H

#include <vlc_common.h>
#include <vlc_picture.h>

#include "chroma_classify.h"

#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define UP_PICTURE_VIEW_MAX_PLANES UP_CHROMA_MAX_PLANES

typedef struct
{
    uint8_t *pixels;
    int pitch;
    int pixel_pitch;
    int width;
    int height;
    int row_bytes;
} up_picture_plane_view_t;

typedef struct
{
    up_picture_plane_view_t plane[UP_PICTURE_VIEW_MAX_PLANES];
    int plane_count;
} up_picture_view_t;

typedef struct
{
    unsigned coded_width;
    unsigned coded_height;
    unsigned x_offset;
    unsigned y_offset;
    int width;
    int height;
} up_picture_region_t;

typedef up_chroma_descriptor_t up_picture_format_layout_t;

typedef struct
{
    size_t x;
    size_t y;
    size_t width;
    size_t height;
    size_t x_bytes;
    size_t row_bytes;
} up_picture_plane_extent_t;

static inline const up_picture_format_layout_t *
up_picture_format_layout(vlc_fourcc_t chroma)
{
    return up_chroma_descriptor((uint32_t)chroma);
}

/* Pixel-space window plus the plane's group/pitch layout — everything
 * needed to derive a plane extent or view. */
typedef struct {
    int      x_offset;
    int      y_offset;
    int      width;
    int      height;
    unsigned x_group_pixels;
    unsigned x_group_bytes;
    unsigned y_group_pixels;
    int      pixel_pitch;
} up_picture_plane_geom_t;

static inline up_picture_plane_extent_t up_picture_plane_extent(
    const up_picture_plane_geom_t *g)
{
    up_picture_plane_extent_t extent = { 0 };
    if (g->x_group_pixels == 0 || g->x_group_bytes == 0
        || g->y_group_pixels == 0 || g->pixel_pitch <= 0
        || g->x_group_bytes % (unsigned)g->pixel_pitch != 0)
        return extent;
    const size_t x_end = (size_t)g->x_offset + (size_t)g->width;
    const size_t y_end = (size_t)g->y_offset + (size_t)g->height;
    extent.x = (size_t)g->x_offset / g->x_group_pixels;
    extent.y = (size_t)g->y_offset / g->y_group_pixels;
    const size_t x_groups = (x_end + g->x_group_pixels - 1)
                          / g->x_group_pixels - extent.x;
    extent.width = x_groups * g->x_group_bytes / (unsigned)g->pixel_pitch;
    extent.height = (y_end + g->y_group_pixels - 1) / g->y_group_pixels
                  - extent.y;
    extent.x_bytes = extent.x * g->x_group_bytes;
    extent.row_bytes = x_groups * g->x_group_bytes;
    return extent;
}

static inline bool up_picture_plane_storage_ok(const plane_t *plane,
                                                int pixel_pitch)
{
    if (!plane->p_pixels || plane->i_pitch <= 0 || plane->i_lines <= 0)
        return false;
    return plane->i_pixel_pitch == pixel_pitch;
}

static inline bool up_picture_plane_extent_ok(
    const plane_t *plane, const up_picture_plane_extent_t *extent)
{
    if (extent->x_bytes > (size_t)plane->i_pitch)
        return false;
    if (extent->row_bytes > (size_t)plane->i_pitch - extent->x_bytes)
        return false;
    if (extent->y > (size_t)plane->i_lines)
        return false;
    if (extent->height > (size_t)plane->i_lines - extent->y)
        return false;
    return extent->width <= INT_MAX && extent->height <= INT_MAX
        && extent->row_bytes <= INT_MAX;
}

static inline bool up_picture_plane_view_init(
    up_picture_plane_view_t *out, const plane_t *plane,
    const up_picture_plane_geom_t *g)
{
    if (!up_picture_plane_storage_ok(plane, g->pixel_pitch))
        return false;
    const up_picture_plane_extent_t extent = up_picture_plane_extent(g);
    if (!up_picture_plane_extent_ok(plane, &extent))
        return false;

    const size_t row_offset = extent.y * (size_t)plane->i_pitch;
    if (row_offset > SIZE_MAX - extent.x_bytes)
        return false;

    out->pixels = plane->p_pixels + row_offset + extent.x_bytes;
    out->pitch = plane->i_pitch;
    out->pixel_pitch = g->pixel_pitch;
    out->width = (int)extent.width;
    out->height = (int)extent.height;
    out->row_bytes = (int)extent.row_bytes;
    return true;
}

static inline bool up_picture_declared_crop_ok(
    const picture_t *pic, const up_picture_region_t *region)
{
    const size_t x_end = (size_t)region->x_offset + (size_t)region->width;
    const size_t y_end = (size_t)region->y_offset + (size_t)region->height;
    if (x_end > region->coded_width || y_end > region->coded_height)
        return false;
    if (pic->format.i_width < region->coded_width
        || pic->format.i_height < region->coded_height)
        return false;
    if (pic->format.i_visible_width != 0
        && (size_t)region->width > pic->format.i_visible_width)
        return false;
    return pic->format.i_visible_height == 0
        || (size_t)region->height <= pic->format.i_visible_height;
}

static inline bool up_picture_view_args_ok(const picture_t *pic,
                                           vlc_fourcc_t chroma,
                                           const up_picture_region_t *region)
{
    if (!pic || !region || region->width <= 0 || region->height <= 0)
        return false;
    if (pic->format.i_chroma != chroma)
        return false;
    if (region->x_offset > INT_MAX || region->y_offset > INT_MAX)
        return false;
    return up_picture_declared_crop_ok(pic, region);
}

static inline const up_picture_format_layout_t *up_picture_view_layout(
    const picture_t *pic, vlc_fourcc_t chroma)
{
    const up_picture_format_layout_t *layout = up_picture_format_layout(chroma);
    if (!layout || pic->i_planes < layout->plane_count)
        return NULL;
    return layout;
}

static inline bool up_picture_view_init(
    up_picture_view_t *out, const picture_t *pic,
    vlc_fourcc_t expected_chroma, const up_picture_region_t *region)
{
    if (!out)
        return false;
    memset(out, 0, sizeof *out);
    if (!up_picture_view_args_ok(pic, expected_chroma, region))
        return false;

    const int x_offset = (int)region->x_offset;
    const int y_offset = (int)region->y_offset;
    const up_picture_format_layout_t *layout =
        up_picture_view_layout(pic, expected_chroma);
    if (!layout)
        return false;

    for (int i = 0; i < layout->plane_count; ++i) {
        const up_picture_plane_geom_t geom = {
            .x_offset       = x_offset,
            .y_offset       = y_offset,
            .width          = region->width,
            .height         = region->height,
            .x_group_pixels = layout->x_group_pixels[i],
            .x_group_bytes  = layout->x_group_bytes[i],
            .y_group_pixels = layout->y_group_pixels[i],
            .pixel_pitch    = layout->pixel_pitch[i],
        };
        if (!up_picture_plane_view_init(&out->plane[i], &pic->p[i], &geom)) {
            memset(out, 0, sizeof *out);
            return false;
        }
    }
    out->plane_count = layout->plane_count;
    return true;
}

#endif
