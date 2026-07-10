#ifndef TEST_STUB_VLC_PICTURE_H
#define TEST_STUB_VLC_PICTURE_H

#include <vlc_common.h>

#include <stdint.h>

typedef struct
{
    uint8_t *p_pixels;
    int i_lines;
    int i_pitch;
    int i_pixel_pitch;
    int i_visible_lines;
    int i_visible_pitch;
} plane_t;

typedef struct
{
    vlc_fourcc_t i_chroma;
    unsigned i_width;
    unsigned i_height;
    unsigned i_visible_width;
    unsigned i_visible_height;
    unsigned i_x_offset;
    unsigned i_y_offset;
} video_format_t;

typedef struct picture_t
{
    video_format_t format;
    int i_planes;
    plane_t p[5];
} picture_t;

#endif
