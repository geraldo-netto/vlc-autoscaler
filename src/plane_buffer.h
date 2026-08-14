// SPDX-License-Identifier: GPL-2.0-or-later
/*****************************************************************************
 * plane_buffer.h - aligned three-plane scratch buffers (ARCH-4)
 *****************************************************************************
 * Header-only, zimg/VLC-free. Extracted from scaler_zimg.c so the
 * overflow-checked allocation seam is unit- and fuzz-testable without
 * libzimg. Sizing comes from zimg_helpers.h (up_plane_pitch /
 * up_plane_lines / UP_PITCH_ALIGN); ownership stays with the caller:
 * alloc_plane_buffer's partial state is released by free_plane_buffer.
 *****************************************************************************/

#ifndef AUTOUPSCALE_PLANE_BUFFER_H
#define AUTOUPSCALE_PLANE_BUFFER_H

#include "zimg_helpers.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

enum { PLANE_Y, PLANE_U, PLANE_V, PLANE_COUNT };

typedef struct
{
    uint8_t *data[PLANE_COUNT];
    int      pitch[PLANE_COUNT];
} plane_view_t;

typedef struct
{
    int pitch[PLANE_COUNT];
    int lines[PLANE_COUNT];
} plane_layout_t;

typedef struct
{
    uint8_t       *data[PLANE_COUNT];
    plane_layout_t layout;
} plane_buffer_t;

/*
 * Compute `lines * pitch` as size_t with overflow check. Returns 0 if
 * either input is non-positive or if the multiplication would wrap.
 * In practice both come from up_plane_lines / up_plane_pitch which
 * already cap inputs, but we double-check at the alloc seam so a
 * malformed dimension reaching this code can never produce an
 * undersized buffer that downstream copies write past.
 */
static inline size_t plane_alloc_bytes(int lines, int pitch)
{
    if (lines <= 0 || pitch <= 0) return 0;
    size_t l = (size_t)lines;
    size_t pp = (size_t)pitch;
    if (l > SIZE_MAX / pp) return 0;
    return l * pp;
}

static inline plane_view_t plane_buffer_view(const plane_buffer_t *buffer)
{
    plane_view_t view = {0};
    for (int p = 0; p < PLANE_COUNT; p++) {
        view.data[p] = buffer->data[p];
        view.pitch[p] = buffer->layout.pitch[p];
    }
    return view;
}

static inline void free_plane_buffer(plane_buffer_t *buffer)
{
    for (int p = 0; p < PLANE_COUNT; p++) {
        free(buffer->data[p]);
        buffer->data[p] = NULL;
    }
}

static inline void init_plane_layout(plane_layout_t *layout,
                                     int width, int height,
                                     unsigned sub_w, unsigned sub_h)
{
    layout->pitch[PLANE_Y] = up_plane_pitch(width, 0);
    layout->lines[PLANE_Y] = up_plane_lines(height, 0);
    for (int p = PLANE_U; p < PLANE_COUNT; p++) {
        layout->pitch[p] = up_plane_pitch(width, sub_w);
        layout->lines[p] = up_plane_lines(height, sub_h);
    }
}

/* Saturates to SIZE_MAX instead of wrapping; alloc_plane_buffer treats
 * that as a rejected layout. */
static inline size_t plane_buffer_bytes(const plane_buffer_t *buffer)
{
    size_t bytes = 0;
    for (int p = 0; p < PLANE_COUNT; p++) {
        const size_t plane_bytes = plane_alloc_bytes(buffer->layout.lines[p],
                                                      buffer->layout.pitch[p]);
        if (plane_bytes > SIZE_MAX - bytes) return SIZE_MAX;
        bytes += plane_bytes;
    }
    return bytes;
}

/* Allocate one layout's three plane buffers. Partial state remains owned by
 * the caller and is released by free_plane_buffer. */
static inline int alloc_plane_buffer(plane_buffer_t *buffer)
{
    if (plane_buffer_bytes(buffer) == SIZE_MAX) return -1;
    for (int p = 0; p < PLANE_COUNT; p++) {
        size_t bytes = plane_alloc_bytes(buffer->layout.lines[p],
                                         buffer->layout.pitch[p]);
        if (bytes == 0) return -1;
        buffer->data[p] = aligned_alloc(UP_PITCH_ALIGN, bytes);
        if (!buffer->data[p]) return -1;
    }
    return 0;
}

#endif /* AUTOUPSCALE_PLANE_BUFFER_H */
