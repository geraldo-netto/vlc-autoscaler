#ifndef TEST_STUB_VLC_PICTURE_H
#define TEST_STUB_VLC_PICTURE_H

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

typedef struct picture_t
{
    int i_planes;
    plane_t p[5];
} picture_t;

#endif
