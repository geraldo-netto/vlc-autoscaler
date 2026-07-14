// SPDX-License-Identifier: GPL-2.0-or-later
#include "../src/picture_view.h"
#include "cli_parse.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct
{
    picture_t pic;
    uint8_t *storage[UP_PICTURE_VIEW_MAX_PLANES];
    size_t storage_size[UP_PICTURE_VIEW_MAX_PLANES];
} fuzz_picture_t;

typedef struct
{
    const uint8_t *data;
    size_t size;
    size_t offset;
} fuzz_input_t;

static uint8_t fuzz_take(fuzz_input_t *input)
{
    if (input->offset >= input->size)
        return 0;
    return input->data[input->offset++];
}

static void fuzz_require(bool condition)
{
    if (!condition) abort();
}

static void fuzz_picture_free(fuzz_picture_t *test)
{
    for (int i = 0; i < UP_PICTURE_VIEW_MAX_PLANES; ++i)
    {
        free(test->storage[i]);
        test->storage[i] = NULL;
    }
}

static bool fuzz_plane_alloc(fuzz_picture_t *test,
                             const up_picture_format_layout_t *layout,
                             int plane_index, int coded_w, int coded_h,
                             fuzz_input_t *input)
{
    const up_picture_plane_geom_t geom = {
        .x_offset       = 0,
        .y_offset       = 0,
        .width          = coded_w,
        .height         = coded_h,
        .x_group_pixels = layout->x_group_pixels[plane_index],
        .x_group_bytes  = layout->x_group_bytes[plane_index],
        .y_group_pixels = layout->y_group_pixels[plane_index],
        .pixel_pitch    = layout->pixel_pitch[plane_index],
    };
    const up_picture_plane_extent_t extent = up_picture_plane_extent(&geom);
    const size_t pitch = extent.row_bytes + fuzz_take(input) % 8u;
    const size_t lines = extent.height + fuzz_take(input) % 4u;
    const size_t bytes = pitch * lines;
    uint8_t *storage = malloc(bytes);
    if (!storage)
        return false;
    memset(storage, fuzz_take(input), bytes);
    test->storage[plane_index] = storage;
    test->storage_size[plane_index] = bytes;
    test->pic.p[plane_index].p_pixels = storage;
    test->pic.p[plane_index].i_pitch = (int)pitch;
    test->pic.p[plane_index].i_lines = (int)lines;
    test->pic.p[plane_index].i_pixel_pitch = layout->pixel_pitch[plane_index];
    return true;
}

static bool fuzz_picture_init(fuzz_picture_t *test, vlc_fourcc_t chroma,
                              int coded_w, int coded_h, fuzz_input_t *input)
{
    memset(test, 0, sizeof *test);
    const up_picture_format_layout_t *layout = up_picture_format_layout(chroma);
    if (!layout) return false;
    test->pic.format.i_chroma = chroma;
    test->pic.format.i_width = (unsigned)coded_w;
    test->pic.format.i_height = (unsigned)coded_h;
    test->pic.i_planes = layout->plane_count;
    for (int i = 0; i < layout->plane_count; ++i)
        if (!fuzz_plane_alloc(test, layout, i, coded_w, coded_h, input)) {
            fuzz_picture_free(test); return false;
        }
    return true;
}

static void fuzz_mutate_picture(fuzz_picture_t *test, uint8_t mutation)
{
    if (mutation == 1)
        test->pic.i_planes--;
    if (mutation == 2)
        test->pic.p[0].i_pixel_pitch = 0;
    if (mutation == 3)
        test->pic.p[0].i_pitch = 1;
    if (mutation == 4)
        test->pic.p[0].i_lines = 1;
    if (mutation == 5)
        test->pic.p[0].p_pixels = NULL;
    if (mutation == 6)
        test->pic.format.i_visible_width = 1;
}

static void fuzz_touch_plane(const fuzz_picture_t *test,
                             const up_picture_plane_view_t *plane,
                             int plane_index)
{
    const ptrdiff_t start = plane->pixels - test->storage[plane_index];
    fuzz_require(start >= 0);
    fuzz_require(plane->height > 0 && plane->row_bytes > 0);
    const size_t used = (size_t)(plane->height - 1) * (size_t)plane->pitch
                      + (size_t)plane->row_bytes;
    fuzz_require((size_t)start <= test->storage_size[plane_index]);
    fuzz_require(used <= test->storage_size[plane_index] - (size_t)start);
    for (int row = 0; row < plane->height; ++row)
    {
        uint8_t *pixels = plane->pixels + (size_t)row * (size_t)plane->pitch;
        pixels[0] ^= (uint8_t)row;
        pixels[plane->row_bytes - 1] ^= (uint8_t)plane_index;
    }
}

static void fuzz_touch_view(const fuzz_picture_t *test,
                            const up_picture_view_t *view)
{
    fuzz_require(view->plane_count == test->pic.i_planes);
    for (int i = 0; i < view->plane_count; ++i)
        fuzz_touch_plane(test, &view->plane[i], i);
}

static void fuzz_run_one(const uint8_t *data, size_t size)
{
    fuzz_input_t input = { data, size, 0 };
    const size_t format_index = fuzz_take(&input) % UP_CHROMA_DESCRIPTOR_COUNT;
    const vlc_fourcc_t chroma =
        (vlc_fourcc_t)up_chroma_descriptors[format_index].chroma;
    const int coded_w = 1 + fuzz_take(&input) % 128;
    const int coded_h = 1 + fuzz_take(&input) % 128;
    fuzz_picture_t test;
    if (!fuzz_picture_init(&test, chroma, coded_w, coded_h, &input))
        return;

    const int x_offset = fuzz_take(&input) % coded_w;
    const int y_offset = fuzz_take(&input) % coded_h;
    const int view_w = 1 + fuzz_take(&input) % (coded_w - x_offset);
    const int view_h = 1 + fuzz_take(&input) % (coded_h - y_offset);
    test.pic.format.i_visible_width = (unsigned)view_w;
    test.pic.format.i_visible_height = (unsigned)view_h;
    test.pic.format.i_x_offset = 0;
    test.pic.format.i_y_offset = 0;

    const up_picture_region_t region = {
        .coded_width = (unsigned)coded_w,
        .coded_height = (unsigned)coded_h,
        .x_offset = (unsigned)x_offset,
        .y_offset = (unsigned)y_offset,
        .width = view_w,
        .height = view_h,
    };

    const uint8_t mutation = fuzz_take(&input) % 7u;
    fuzz_mutate_picture(&test, mutation);
    const vlc_fourcc_t expected = fuzz_take(&input) == 0xff
                                ? VLC_FOURCC('B', 'A', 'D', '!') : chroma;
    up_picture_view_t view;
    const bool valid = up_picture_view_init(&view, &test.pic, expected,
                                            &region);
    if (valid)
        fuzz_touch_view(&test, &view);
    fuzz_picture_free(&test);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    fuzz_run_one(data, size);
    return 0;
}

#ifdef FUZZ_MAIN
#include "fuzz_smoke.h"

static int smoke_iter(long i)
{
    uint8_t data[32] = { 0 };
    if (i == 0)
        LLVMFuzzerTestOneInput(data, 0);
    fuzz_smoke_fill(data, sizeof data);
    LLVMFuzzerTestOneInput(data, sizeof data);
    return 0;
}

int main(int argc, char **argv)
{
    return fuzz_smoke_main(argc, argv, 50000, "picture_view", smoke_iter);
}
#endif
