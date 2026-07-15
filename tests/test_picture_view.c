// SPDX-License-Identifier: GPL-2.0-or-later
#include "../src/picture_view.h"

#include "../src/chroma_classify.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

enum { TEST_PITCH = 64, TEST_LINES = 16 };

typedef struct
{
    picture_t pic;
    uint8_t storage[UP_PICTURE_VIEW_MAX_PLANES][TEST_PITCH * TEST_LINES];
} test_picture_t;

static int failures;

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "%s:%d: CHECK(%s) failed\n", \
                __FILE__, __LINE__, #condition); \
        ++failures; \
    } \
} while (0)

static up_picture_region_t test_region(void)
{
    const up_picture_region_t region = {
        .coded_width = 16,
        .coded_height = 12,
        .x_offset = 1,
        .y_offset = 1,
        .width = 5,
        .height = 3,
    };
    return region;
}

static void init_picture(test_picture_t *test, vlc_fourcc_t chroma)
{
    memset(test, 0, sizeof *test);
    const up_picture_format_layout_t *layout = up_picture_format_layout(chroma);
    test->pic.format.i_chroma = chroma;
    test->pic.format.i_width = 16;
    test->pic.format.i_height = 12;
    test->pic.format.i_visible_width = 5;
    test->pic.format.i_visible_height = 3;
    test->pic.i_planes = layout ? layout->plane_count : 0;
    for (int i = 0; layout && i < layout->plane_count; ++i)
    {
        test->pic.p[i].p_pixels = test->storage[i];
        test->pic.p[i].i_pitch = TEST_PITCH;
        test->pic.p[i].i_lines = TEST_LINES;
        test->pic.p[i].i_pixel_pitch = layout->pixel_pitch[i];
    }
}

static size_t ceil_div(size_t value, size_t divisor)
{
    return value / divisor + (value % divisor != 0);
}

static void check_plane(const test_picture_t *test,
                        const up_picture_format_layout_t *layout,
                        const up_picture_region_t *region,
                        const up_picture_view_t *view, int plane_index)
{
    const size_t xf = layout->x_group_pixels[plane_index];
    const size_t yf = layout->y_group_pixels[plane_index];
    const size_t x0 = region->x_offset / xf;
    const size_t y0 = region->y_offset / yf;
    const size_t x1 = ceil_div(region->x_offset + (unsigned)region->width, xf);
    const size_t y1 = ceil_div(region->y_offset + (unsigned)region->height, yf);
    const int group_bytes = layout->x_group_bytes[plane_index];
    const size_t plane_height = y1 - y0;
    const size_t row_bytes = (x1 - x0) * (size_t)group_bytes;
    const up_picture_plane_view_t *plane = &view->plane[plane_index];

    CHECK(plane->pixels == test->storage[plane_index]
                           + y0 * TEST_PITCH + x0 * (size_t)group_bytes);
    CHECK(plane->pitch == TEST_PITCH);
    const uint8_t *last = plane->pixels
                  + (plane_height - 1) * (size_t)plane->pitch
                  + row_bytes - 1;
    CHECK(last >= test->storage[plane_index]);
    CHECK(last < test->storage[plane_index] + sizeof test->storage[plane_index]);
}

static void check_format(vlc_fourcc_t chroma)
{
    test_picture_t test;
    up_picture_view_t view;
    const up_picture_region_t region = test_region();
    init_picture(&test, chroma);
    const up_picture_format_layout_t *layout = up_picture_format_layout(chroma);
    CHECK(layout != NULL);
    CHECK(test.pic.format.i_x_offset == 0 && test.pic.format.i_y_offset == 0);
    CHECK(up_picture_view_init(&view, &test.pic, chroma, &region));
    CHECK(view.plane_count == layout->plane_count);
    for (int i = 0; i < view.plane_count; ++i)
        check_plane(&test, layout, &region, &view, i);
}

static void test_every_supported_format(void)
{
    for (size_t i = 0; i < UP_CHROMA_DESCRIPTOR_COUNT; ++i)
        check_format((vlc_fourcc_t)up_chroma_descriptors[i].chroma);
}

static void test_odd_crop_and_physical_order(void)
{
    test_picture_t test;
    up_picture_view_t view;
    const up_picture_region_t region = test_region();

    init_picture(&test, VLC_CODEC_I420);
    test.pic.p[1].i_pitch = 32;
    test.pic.p[2].i_pitch = 48;
    CHECK(up_picture_view_init(&view, &test.pic, VLC_CODEC_I420, &region));
    CHECK(view.plane[1].pixels == test.storage[1]);
    CHECK(view.plane[1].pitch == 32);
    CHECK(view.plane[2].pitch == 48);

    init_picture(&test, VLC_CODEC_YV12);
    CHECK(up_picture_view_init(&view, &test.pic, VLC_CODEC_YV12, &region));
    CHECK(view.plane[1].pixels == test.storage[1]);
    CHECK(view.plane[2].pixels == test.storage[2]);

    init_picture(&test, VLC_CODEC_NV12);
    CHECK(up_picture_view_init(&view, &test.pic, VLC_CODEC_NV12, &region));
    CHECK(view.plane[1].pixels == test.storage[1]);

    init_picture(&test, VLC_CODEC_RGB24);
    CHECK(up_picture_view_init(&view, &test.pic, VLC_CODEC_RGB24, &region));
    CHECK(view.plane[0].pixels == test.storage[0] + TEST_PITCH + 3);
}

static void expect_invalid(const picture_t *pic, vlc_fourcc_t chroma,
                           const up_picture_region_t *region)
{
    up_picture_view_t view;
    memset(&view, 0xa5, sizeof view);
    CHECK(!up_picture_view_init(&view, pic, chroma, region));
    CHECK(view.plane_count == 0);
    CHECK(view.plane[0].pixels == NULL);
}

static void test_invalid_format_contract(void)
{
    test_picture_t test;
    up_picture_view_t view;
    up_picture_region_t region = test_region();
    const up_picture_plane_geom_t zero_group = {
        .x_offset = 0, .y_offset = 0, .width = 1, .height = 1,
        .x_group_pixels = 0, .x_group_bytes = 1, .y_group_pixels = 1,
        .pixel_pitch = 1,
    };
    const up_picture_plane_extent_t empty = up_picture_plane_extent(&zero_group);
    CHECK(empty.row_bytes == 0 && empty.height == 0);
    init_picture(&test, VLC_CODEC_I420);

    CHECK(!up_picture_view_init(NULL, &test.pic, VLC_CODEC_I420, &region));
    expect_invalid(NULL, VLC_CODEC_I420, &region);
    expect_invalid(&test.pic, VLC_CODEC_I420, NULL);
    region.width = 0;
    expect_invalid(&test.pic, VLC_CODEC_I420, &region);
    region = test_region();
    region.height = 0;
    expect_invalid(&test.pic, VLC_CODEC_I420, &region);
    region = test_region();
    expect_invalid(&test.pic, VLC_CODEC_YV12, &region);
    expect_invalid(&test.pic, VLC_FOURCC('B', 'A', 'D', '!'), &region);

    region.x_offset = (unsigned)INT_MAX + 1u;
    expect_invalid(&test.pic, VLC_CODEC_I420, &region);
    region = test_region();
    region.y_offset = (unsigned)INT_MAX + 1u;
    expect_invalid(&test.pic, VLC_CODEC_I420, &region);

    region = test_region();
    region.coded_width = 5;
    expect_invalid(&test.pic, VLC_CODEC_I420, &region);
    region = test_region();
    region.coded_height = 3;
    expect_invalid(&test.pic, VLC_CODEC_I420, &region);
    region = test_region();
    test.pic.format.i_width = 15;
    expect_invalid(&test.pic, VLC_CODEC_I420, &region);
    init_picture(&test, VLC_CODEC_I420);
    test.pic.format.i_height = 11;
    expect_invalid(&test.pic, VLC_CODEC_I420, &region);
    init_picture(&test, VLC_CODEC_I420);
    test.pic.format.i_visible_width = 4;
    expect_invalid(&test.pic, VLC_CODEC_I420, &region);
    init_picture(&test, VLC_CODEC_I420);
    test.pic.format.i_visible_height = 2;
    expect_invalid(&test.pic, VLC_CODEC_I420, &region);

    init_picture(&test, VLC_CODEC_I420);
    test.pic.format.i_visible_width = 0;
    test.pic.format.i_visible_height = 0;
    CHECK(up_picture_view_init(&view, &test.pic, VLC_CODEC_I420, &region));
    test.pic.i_planes = 2;
    expect_invalid(&test.pic, VLC_CODEC_I420, &region);
}

static void test_invalid_plane_storage(void)
{
    test_picture_t test;
    const up_picture_region_t region = test_region();
    init_picture(&test, VLC_CODEC_I420);
    test.pic.p[0].p_pixels = NULL;
    expect_invalid(&test.pic, VLC_CODEC_I420, &region);

    init_picture(&test, VLC_CODEC_I420);
    test.pic.p[0].i_pitch = 0;
    expect_invalid(&test.pic, VLC_CODEC_I420, &region);
    init_picture(&test, VLC_CODEC_I420);
    test.pic.p[0].i_lines = 0;
    expect_invalid(&test.pic, VLC_CODEC_I420, &region);
    init_picture(&test, VLC_CODEC_I420);
    test.pic.p[0].i_pixel_pitch = 0;
    expect_invalid(&test.pic, VLC_CODEC_I420, &region);
    init_picture(&test, VLC_CODEC_RGB24);
    test.pic.p[0].i_pixel_pitch = 4;
    expect_invalid(&test.pic, VLC_CODEC_RGB24, &region);
}

static void test_invalid_physical_bounds(void)
{
    test_picture_t test;
    up_picture_region_t region = test_region();
    init_picture(&test, VLC_CODEC_RGB24);
    region.x_offset = 2;
    region.width = 1;
    test.pic.format.i_visible_width = 1;
    test.pic.p[0].i_pitch = 5;
    expect_invalid(&test.pic, VLC_CODEC_RGB24, &region);

    init_picture(&test, VLC_CODEC_RGB24);
    region = test_region();
    test.pic.p[0].i_pitch = 8;
    test.pic.format.i_visible_width = 2;
    region.width = 2;
    expect_invalid(&test.pic, VLC_CODEC_RGB24, &region);

    init_picture(&test, VLC_CODEC_I444);
    region = test_region();
    region.y_offset = 10;
    region.height = 1;
    test.pic.format.i_visible_height = 1;
    test.pic.p[0].i_lines = 5;
    expect_invalid(&test.pic, VLC_CODEC_I444, &region);

    init_picture(&test, VLC_CODEC_I444);
    region = test_region();
    test.pic.p[0].i_lines = 3;
    expect_invalid(&test.pic, VLC_CODEC_I444, &region);

    init_picture(&test, VLC_CODEC_RGBA);
    region = (up_picture_region_t) {
        .coded_width = UINT_MAX,
        .coded_height = 1,
        .width = INT_MAX,
        .height = 1,
    };
    test.pic.format.i_width = UINT_MAX;
    test.pic.format.i_height = 1;
    test.pic.format.i_visible_width = 0;
    test.pic.format.i_visible_height = 0;
    test.pic.p[0].i_pitch = INT_MAX;
    test.pic.p[0].i_lines = 1;
    expect_invalid(&test.pic, VLC_CODEC_RGBA, &region);
}

/* An unsupported fourcc has no software descriptor. The invalid-format
 * contract test can't reach this via
 * up_picture_view_init (its chroma-match arg check rejects a mismatched
 * fourcc first), so exercise the lookup directly and via a picture that
 * declares the unknown chroma so the match check passes. */
static void test_unknown_chroma_layout(void)
{
    CHECK(up_picture_format_layout(VLC_FOURCC('X', 'X', 'X', 'X')) == NULL);

    test_picture_t test;
    up_picture_view_t view;
    up_picture_region_t region = test_region();
    init_picture(&test, VLC_CODEC_I420);
    test.pic.format.i_chroma = VLC_FOURCC('X', 'X', 'X', 'X');
    CHECK(!up_picture_view_init(&view, &test.pic,
                                VLC_FOURCC('X', 'X', 'X', 'X'), &region));
}

/* Each descriptor states both axis shifts and physical plane groups. Pin
 * those fields together: for every Y-plane chroma, plane 1's group size must
 * be exactly 1 << subsample_shift on each axis. */
static void test_layout_matches_subsample_table(void)
{
    for (size_t i = 0; i < UP_CHROMA_DESCRIPTOR_COUNT; ++i)
    {
        const vlc_fourcc_t chroma =
            (vlc_fourcc_t)up_chroma_descriptors[i].chroma;
        if (!up_chroma_has_y_plane(chroma))
            continue;
        const up_picture_format_layout_t *layout =
            up_picture_format_layout(chroma);
        CHECK(layout != NULL);
        if (!layout)
            continue;
        unsigned sub_w = 99;
        unsigned sub_h = 99;
        CHECK(up_chroma_subsample(chroma, &sub_w, &sub_h));
        /* Plane 0 is luma: never subsampled. Plane 1 carries the chroma
         * group size (both chroma planes share it on planar formats). */
        CHECK(layout->x_group_pixels[0] == 1);
        CHECK(layout->y_group_pixels[0] == 1);
        CHECK(layout->x_group_pixels[1] == (1u << sub_w));
        CHECK(layout->y_group_pixels[1] == (1u << sub_h));
    }
}

int main(void)
{
    test_layout_matches_subsample_table();
    test_every_supported_format();
    test_odd_crop_and_physical_order();
    test_invalid_format_contract();
    test_invalid_plane_storage();
    test_invalid_physical_bounds();
    test_unknown_chroma_layout();
    if (failures != 0)
        fprintf(stderr, "picture_view: %d checks failed\n", failures);
    return failures == 0 ? 0 : 1;
}
