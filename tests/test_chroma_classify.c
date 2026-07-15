// SPDX-License-Identifier: GPL-2.0-or-later
/*****************************************************************************
 * test_chroma_classify.c - unit tests for chroma_classify.h
 *****************************************************************************
 * Tests the shared software descriptor and the predicates that gate hwaccel
 * rejection and USM application. Their correctness matters:
 *
 *   - A missing entry in up_opaque_chromas means autoupscale accepts a
 *     hardware GPU surface, performs unnecessary worker and scratch setup,
 *     gets torn down by VLC's chain probing, and the user sees the
 *     "Too high level of recursion" cascade.
 *
 *   - A wrong entry in up_opaque_chromas means autoupscale silently
 *     refuses a software chroma the user expected to work.
 *
 *   - up_chroma_has_y_plane wrong on luma-bearing chromas disables USM
 *     where it should run; wrong on packed/RGB enables USM where it
 *     causes color fringing.
 *****************************************************************************/

#include "../src/chroma_classify.h"

#include <stdio.h>
#include <stdlib.h>

#include "test_harness.h"

typedef struct {
    uint32_t fourcc;
    up_chroma_layout_t layout;
    uint8_t plane_count;
    uint8_t sub_w;
    uint8_t sub_h;
    bool uv_planes_swapped;
    uint8_t x_group_pixels[UP_CHROMA_MAX_PLANES];
    uint8_t x_group_bytes[UP_CHROMA_MAX_PLANES];
    uint8_t y_group_pixels[UP_CHROMA_MAX_PLANES];
    uint8_t pixel_pitch[UP_CHROMA_MAX_PLANES];
} expected_descriptor_t;

static const expected_descriptor_t k_descriptors[] = {
    { UP_FOURCC('I','4','2','0'), UP_CHROMA_LAYOUT_YUV420P, 3, 1, 1, false,
      {1, 2, 2, 0}, {1, 1, 1, 0}, {1, 2, 2, 0}, {1, 1, 1, 0} },
    { UP_FOURCC('Y','V','1','2'), UP_CHROMA_LAYOUT_YUV420P, 3, 1, 1, true,
      {1, 2, 2, 0}, {1, 1, 1, 0}, {1, 2, 2, 0}, {1, 1, 1, 0} },
    { UP_FOURCC('I','4','2','2'), UP_CHROMA_LAYOUT_YUV422P, 3, 1, 0, false,
      {1, 2, 2, 0}, {1, 1, 1, 0}, {1, 1, 1, 0}, {1, 1, 1, 0} },
    { UP_FOURCC('I','4','4','4'), UP_CHROMA_LAYOUT_YUV444P, 3, 0, 0, false,
      {1, 1, 1, 0}, {1, 1, 1, 0}, {1, 1, 1, 0}, {1, 1, 1, 0} },
    { UP_FOURCC('N','V','1','2'), UP_CHROMA_LAYOUT_NV12, 2, 1, 1, false,
      {1, 2, 0, 0}, {1, 2, 0, 0}, {1, 2, 0, 0}, {1, 1, 0, 0} },
    { UP_FOURCC('N','V','2','1'), UP_CHROMA_LAYOUT_NV21, 2, 1, 1, false,
      {1, 2, 0, 0}, {1, 2, 0, 0}, {1, 2, 0, 0}, {1, 1, 0, 0} },
    { UP_FOURCC('R','V','2','4'), UP_CHROMA_LAYOUT_RGB24, 1, 0, 0, false,
      {1, 0, 0, 0}, {3, 0, 0, 0}, {1, 0, 0, 0}, {3, 0, 0, 0} },
    { UP_FOURCC('R','G','B','A'), UP_CHROMA_LAYOUT_RGBA, 1, 0, 0, false,
      {1, 0, 0, 0}, {4, 0, 0, 0}, {1, 0, 0, 0}, {4, 0, 0, 0} },
    { UP_FOURCC('B','G','R','A'), UP_CHROMA_LAYOUT_BGRA, 1, 0, 0, false,
      {1, 0, 0, 0}, {4, 0, 0, 0}, {1, 0, 0, 0}, {4, 0, 0, 0} },
};

static void test_software_descriptor_table(void)
{
    BEGIN("descriptor: all supported software layouts are exact and unique");
    const size_t count = sizeof k_descriptors / sizeof k_descriptors[0];
    CHECK(UP_CHROMA_DESCRIPTOR_COUNT == count);
    for (size_t i = 0; i < count; i++) {
        const expected_descriptor_t *expected = &k_descriptors[i];
        const up_chroma_descriptor_t *actual =
            up_chroma_descriptor(expected->fourcc);
        CHECK(actual != NULL);
        if (!actual) continue;
        CHECK(actual->layout == expected->layout);
        CHECK(actual->plane_count == expected->plane_count);
        CHECK(actual->sub_w == expected->sub_w);
        CHECK(actual->sub_h == expected->sub_h);
        CHECK(actual->uv_planes_swapped == expected->uv_planes_swapped);
        for (size_t p = 0; p < UP_CHROMA_MAX_PLANES; p++) {
            CHECK(actual->x_group_pixels[p] == expected->x_group_pixels[p]);
            CHECK(actual->x_group_bytes[p] == expected->x_group_bytes[p]);
            CHECK(actual->y_group_pixels[p] == expected->y_group_pixels[p]);
            CHECK(actual->pixel_pitch[p] == expected->pixel_pitch[p]);
        }
        for (size_t j = i + 1; j < count; j++)
            CHECK(expected->fourcc != k_descriptors[j].fourcc);
    }
    CHECK(up_chroma_descriptor(UP_FOURCC('B','A','D','!')) == NULL);
    END();
}

static void test_physical_plane_index_identity(void)
{
    BEGIN("plane index: unswapped layout preserves every plane");
    CHECK_EQ(up_chroma_physical_plane_index(0, false), 0);
    CHECK_EQ(up_chroma_physical_plane_index(1, false), 1);
    CHECK_EQ(up_chroma_physical_plane_index(2, false), 2);
    CHECK_EQ(up_chroma_physical_plane_index(3, false), 3);
    END();
}

static void test_physical_plane_index_yv12(void)
{
    BEGIN("plane index: YV12 swaps only semantic U/V");
    CHECK_EQ(up_chroma_physical_plane_index(0, true), 0);
    CHECK_EQ(up_chroma_physical_plane_index(1, true), 2);
    CHECK_EQ(up_chroma_physical_plane_index(2, true), 1);
    CHECK_EQ(up_chroma_physical_plane_index(3, true), 3);
    END();
}

/* ---------- up_chroma_is_opaque: opaque chromas detected ---------- */

static void test_opaque_vaapi(void)
{
    BEGIN("opaque: VAAPI fourccs detected (VAOP, VAO0)");
    CHECK(up_chroma_is_opaque(UP_FOURCC('V','A','O','P')));
    CHECK(up_chroma_is_opaque(UP_FOURCC('V','A','O','0')));
    END();
}

static void test_opaque_vdpau(void)
{
    BEGIN("opaque: VDPAU fourccs detected (VDV0, VDV2, VDV4, VDOR)");
    CHECK(up_chroma_is_opaque(UP_FOURCC('V','D','V','0')));
    CHECK(up_chroma_is_opaque(UP_FOURCC('V','D','V','2')));
    CHECK(up_chroma_is_opaque(UP_FOURCC('V','D','V','4')));
    CHECK(up_chroma_is_opaque(UP_FOURCC('V','D','O','R')));
    END();
}

static void test_opaque_direct3d(void)
{
    BEGIN("opaque: Direct3D fourccs detected (DXA9, DXA0, DX11, DX10)");
    CHECK(up_chroma_is_opaque(UP_FOURCC('D','X','A','9')));
    CHECK(up_chroma_is_opaque(UP_FOURCC('D','X','A','0')));
    CHECK(up_chroma_is_opaque(UP_FOURCC('D','X','1','1')));
    CHECK(up_chroma_is_opaque(UP_FOURCC('D','X','1','0')));
    END();
}

static void test_opaque_mmal(void)
{
    BEGIN("opaque: MMAL detected (Raspberry Pi)");
    CHECK(up_chroma_is_opaque(UP_FOURCC('M','M','A','L')));
    END();
}

static void test_opaque_corevideo(void)
{
    BEGIN("opaque: CoreVideo fourccs detected (CVPN, CVPY, CVPI, CVPB, CVPP)");
    CHECK(up_chroma_is_opaque(UP_FOURCC('C','V','P','N')));
    CHECK(up_chroma_is_opaque(UP_FOURCC('C','V','P','Y')));
    CHECK(up_chroma_is_opaque(UP_FOURCC('C','V','P','I')));
    CHECK(up_chroma_is_opaque(UP_FOURCC('C','V','P','B')));
    CHECK(up_chroma_is_opaque(UP_FOURCC('C','V','P','P')));
    END();
}

/* ---------- up_chroma_is_opaque: software chromas NOT flagged ---------- */

static void test_software_chromas_not_opaque(void)
{
    BEGIN("opaque: software chromas (I420/YV12/NV12/I422/I444/RGB) "
          "are NOT flagged");
    CHECK(!up_chroma_is_opaque(UP_FOURCC('I','4','2','0')));
    CHECK(!up_chroma_is_opaque(UP_FOURCC('Y','V','1','2')));
    CHECK(!up_chroma_is_opaque(UP_FOURCC('N','V','1','2')));
    CHECK(!up_chroma_is_opaque(UP_FOURCC('N','V','2','1')));
    CHECK(!up_chroma_is_opaque(UP_FOURCC('I','4','2','2')));
    CHECK(!up_chroma_is_opaque(UP_FOURCC('I','4','4','4')));
    CHECK(!up_chroma_is_opaque(UP_FOURCC('Y','U','Y','2')));
    CHECK(!up_chroma_is_opaque(UP_FOURCC('U','Y','V','Y')));
    /* Common RGB packs */
    CHECK(!up_chroma_is_opaque(UP_FOURCC('R','V','2','4')));
    CHECK(!up_chroma_is_opaque(UP_FOURCC('R','V','3','2')));
    /* Garbage fourccs not in any list */
    CHECK(!up_chroma_is_opaque(0));
    CHECK(!up_chroma_is_opaque(0xFFFFFFFFu));
    CHECK(!up_chroma_is_opaque(UP_FOURCC('q','q','q','q')));
    END();
}

static void test_opaque_count_matches_expected(void)
{
    BEGIN("opaque: count matches the documented hwaccel families");
    /* 16 entries: 2 VAAPI + 4 VDPAU + 4 D3D + 1 MMAL + 5 CoreVideo. */
    CHECK(UP_OPAQUE_CHROMA_COUNT == 16);
    END();
}

/* ---------- up_chroma_has_y_plane: positive cases ---------- */

static void test_y_plane_planar_yuv(void)
{
    BEGIN("has_y_plane: planar YUV chromas have Y (I420/YV12/I422/I444)");
    CHECK(up_chroma_has_y_plane(UP_FOURCC('I','4','2','0')));
    CHECK(up_chroma_has_y_plane(UP_FOURCC('Y','V','1','2')));
    CHECK(up_chroma_has_y_plane(UP_FOURCC('I','4','2','2')));
    CHECK(up_chroma_has_y_plane(UP_FOURCC('I','4','4','4')));
    END();
}

static void test_y_plane_semi_planar(void)
{
    BEGIN("has_y_plane: semi-planar (NV12/NV21) have a discrete Y plane");
    CHECK(up_chroma_has_y_plane(UP_FOURCC('N','V','1','2')));
    CHECK(up_chroma_has_y_plane(UP_FOURCC('N','V','2','1')));
    END();
}

/* ---------- up_chroma_has_y_plane: negative cases ---------- */

static void test_y_plane_packed_yuv(void)
{
    BEGIN("has_y_plane: packed YUV (YUY2/UYVY) does NOT count "
          "(no separate plane)");
    CHECK(!up_chroma_has_y_plane(UP_FOURCC('Y','U','Y','2')));
    CHECK(!up_chroma_has_y_plane(UP_FOURCC('U','Y','V','Y')));
    END();
}

static void test_y_plane_rgb(void)
{
    BEGIN("has_y_plane: RGB has no Y plane");
    CHECK(!up_chroma_has_y_plane(UP_FOURCC('R','V','2','4')));
    CHECK(!up_chroma_has_y_plane(UP_FOURCC('R','V','3','2')));
    CHECK(!up_chroma_has_y_plane(UP_FOURCC('R','G','B','A')));
    CHECK(!up_chroma_has_y_plane(UP_FOURCC('B','G','R','A')));
    END();
}

static void test_y_plane_opaque(void)
{
    BEGIN("has_y_plane: hwaccel/opaque chromas don't have a usable Y plane");
    /* Even though VAAPI/VDPAU may be 4:2:0 conceptually, the surface is
     * GPU-only and we cannot read it. has_y_plane gates USM, which needs
     * to read pixels - return false for these. */
    CHECK(!up_chroma_has_y_plane(UP_FOURCC('V','A','O','P')));
    CHECK(!up_chroma_has_y_plane(UP_FOURCC('V','D','V','0')));
    CHECK(!up_chroma_has_y_plane(UP_FOURCC('D','X','1','1')));
    CHECK(!up_chroma_has_y_plane(UP_FOURCC('M','M','A','L')));
    END();
}

static void test_y_plane_garbage(void)
{
    BEGIN("has_y_plane: garbage fourccs return false");
    CHECK(!up_chroma_has_y_plane(0));
    CHECK(!up_chroma_has_y_plane(0xFFFFFFFFu));
    CHECK(!up_chroma_has_y_plane(UP_FOURCC('q','q','q','q')));
    END();
}

/* ---------- consistency invariants between the two predicates ---------- */

static void test_opaque_implies_no_y_plane(void)
{
    BEGIN("invariant: every opaque chroma has has_y_plane==false");
    /* Walk the opaque table and keep it disjoint from software descriptors. */
    for (size_t i = 0; i < UP_OPAQUE_CHROMA_COUNT; i++) {
        if (up_chroma_has_y_plane(up_opaque_chromas[i])) {
            printf("    opaque chroma 0x%08x at index %zu also "
                   "has_y_plane==true (sync error)\n",
                   up_opaque_chromas[i], i);
            g_cur_fail = 1;
        }
    }
    END();
}

/* ---------- up_chroma_subsample ---------- */

typedef struct {
    uint32_t fourcc;
    const char *name;
    unsigned sub_w;
    unsigned sub_h;
} subsample_case_t;

static const subsample_case_t k_subsampled[] = {
    { UP_FOURCC('I','4','2','0'), "I420", 1, 1 },
    { UP_FOURCC('Y','V','1','2'), "YV12", 1, 1 },
    { UP_FOURCC('N','V','1','2'), "NV12", 1, 1 },
    { UP_FOURCC('N','V','2','1'), "NV21", 1, 1 },
    { UP_FOURCC('I','4','2','2'), "I422", 1, 0 },
    { UP_FOURCC('I','4','4','4'), "I444", 0, 0 },
};
#define N_SUBSAMPLED (sizeof k_subsampled / sizeof k_subsampled[0])

static void test_subsample_table(void)
{
    BEGIN("subsample: every Y-plane chroma reports its documented shifts");
    for (size_t i = 0; i < N_SUBSAMPLED; i++) {
        const subsample_case_t *c = &k_subsampled[i];
        unsigned sw = 99, sh = 99;
        if (!up_chroma_subsample(c->fourcc, &sw, &sh)
            || sw != c->sub_w || sh != c->sub_h) {
            printf("    %s: got (%u,%u) expected (%u,%u)\n",
                   c->name, sw, sh, c->sub_w, c->sub_h);
            g_cur_fail = 1;
        }
    }
    END();
}

static void test_subsample_rejects_non_planar(void)
{
    BEGIN("subsample: packed YUV / RGB / opaque / garbage are rejected");
    unsigned sw = 7, sh = 7;
    CHECK(!up_chroma_subsample(UP_FOURCC('Y','U','Y','2'), &sw, &sh));
    CHECK(!up_chroma_subsample(UP_FOURCC('R','V','2','4'), &sw, &sh));
    CHECK(!up_chroma_subsample(UP_FOURCC('V','A','O','P'), &sw, &sh));
    CHECK(!up_chroma_subsample(0xFFFFFFFFu, &sw, &sh));
    /* Rejection must leave the outputs untouched. */
    CHECK(sw == 7 && sh == 7);
    END();
}

static void test_subsample_null_outputs(void)
{
    BEGIN("subsample: NULL outputs are rejected without dereferencing");
    unsigned v = 0;
    CHECK(!up_chroma_subsample(UP_FOURCC('I','4','2','0'), NULL, &v));
    CHECK(!up_chroma_subsample(UP_FOURCC('I','4','2','0'), &v, NULL));
    CHECK(!up_chroma_subsample(UP_FOURCC('I','4','2','0'), NULL, NULL));
    END();
}

static void test_subsample_covers_every_y_plane_chroma(void)
{
    BEGIN("invariant: has_y_plane(c) <=> subsample(c) succeeds");
    /* The independent expected cases cover every descriptor with a Y plane. */
    for (size_t i = 0; i < N_SUBSAMPLED; i++)
        CHECK(up_chroma_has_y_plane(k_subsampled[i].fourcc));
    unsigned sw, sh;
    CHECK(!up_chroma_has_y_plane(UP_FOURCC('Y','U','Y','2'))
          && !up_chroma_subsample(UP_FOURCC('Y','U','Y','2'), &sw, &sh));
    END();
}

/* ---------- up_chroma_align_crop_even ---------- */

/* REL-1 regression: NV12/NV21 are 4:2:0 but zimg cannot consume them, so the
 * crop alignment used to be gated on up_chroma_to_zimg() and skipped them
 * entirely — the chroma anchor then floored to x/2 while luma started at the
 * odd byte, displacing chroma half a luma pel for the whole playback. Every
 * hardware decoder hands back NV12, so this was the common path. */
static void test_align_semi_planar_420(void)
{
    BEGIN("align: NV12/NV21 odd crop dims AND offsets are evened (REL-1)");
    for (size_t i = 0; i < 2; i++) {
        const uint32_t c = i ? UP_FOURCC('N','V','2','1')
                             : UP_FOURCC('N','V','1','2');
        int w = 641, h = 361;
        unsigned x = 1, y = 3;
        up_chroma_align_crop_even(c, &w, &h, &x, &y);
        CHECK(w == 640 && h == 360 && x == 0 && y == 2);
    }
    END();
}

static void test_align_planar_420(void)
{
    BEGIN("align: I420/YV12 even both axes");
    int w = 101, h = 51;
    unsigned x = 5, y = 7;
    up_chroma_align_crop_even(UP_FOURCC('I','4','2','0'), &w, &h, &x, &y);
    CHECK(w == 100 && h == 50 && x == 4 && y == 6);
    w = 101; h = 51; x = 5; y = 7;
    up_chroma_align_crop_even(UP_FOURCC('Y','V','1','2'), &w, &h, &x, &y);
    CHECK(w == 100 && h == 50 && x == 4 && y == 6);
    END();
}

static void test_align_422_horizontal_only(void)
{
    BEGIN("align: I422 evens the horizontal axis only (sub_h == 0)");
    int w = 101, h = 51;
    unsigned x = 5, y = 7;
    up_chroma_align_crop_even(UP_FOURCC('I','4','2','2'), &w, &h, &x, &y);
    CHECK(w == 100 && h == 51 && x == 4 && y == 7);
    END();
}

static void test_align_444_and_unsupported_untouched(void)
{
    BEGIN("align: I444 (no subsampling) and non-planar chromas are untouched");
    int w = 101, h = 51;
    unsigned x = 5, y = 7;
    up_chroma_align_crop_even(UP_FOURCC('I','4','4','4'), &w, &h, &x, &y);
    CHECK(w == 101 && h == 51 && x == 5 && y == 7);
    up_chroma_align_crop_even(UP_FOURCC('Y','U','Y','2'), &w, &h, &x, &y);
    CHECK(w == 101 && h == 51 && x == 5 && y == 7);
    up_chroma_align_crop_even(0xFFFFFFFFu, &w, &h, &x, &y);
    CHECK(w == 101 && h == 51 && x == 5 && y == 7);
    END();
}

static void test_align_null_arguments(void)
{
    BEGIN("align: each argument is independently optional");
    int w = 101;
    unsigned y = 7;
    /* Open() aligns dims only; ConfigureScaler aligns offsets only. */
    up_chroma_align_crop_even(UP_FOURCC('I','4','2','0'), &w, NULL,
                              NULL, NULL);
    CHECK(w == 100);
    up_chroma_align_crop_even(UP_FOURCC('I','4','2','0'), NULL, NULL,
                              NULL, &y);
    CHECK(y == 6);
    up_chroma_align_crop_even(UP_FOURCC('I','4','2','0'), NULL, NULL,
                              NULL, NULL);
    END();
}

static void test_align_is_idempotent_and_shrinking(void)
{
    BEGIN("invariant: align never grows the window and is idempotent");
    for (size_t i = 0; i < N_SUBSAMPLED; i++) {
        int w = 1919, h = 1079;
        unsigned x = 33, y = 17;
        up_chroma_align_crop_even(k_subsampled[i].fourcc, &w, &h, &x, &y);
        const int w1 = w, h1 = h;
        const unsigned x1 = x, y1 = y;
        CHECK(w1 <= 1919 && h1 <= 1079 && x1 <= 33 && y1 <= 17);
        up_chroma_align_crop_even(k_subsampled[i].fourcc, &w, &h, &x, &y);
        CHECK(w == w1 && h == h1 && x == x1 && y == y1);
    }
    END();
}

int main(void)
{
    printf("Running chroma_classify tests...\n");

    test_software_descriptor_table();
    test_physical_plane_index_identity();
    test_physical_plane_index_yv12();

    /* up_chroma_is_opaque positive cases */
    test_opaque_vaapi();
    test_opaque_vdpau();
    test_opaque_direct3d();
    test_opaque_mmal();
    test_opaque_corevideo();
    test_opaque_count_matches_expected();

    /* up_chroma_is_opaque negative cases */
    test_software_chromas_not_opaque();

    /* up_chroma_has_y_plane positive cases */
    test_y_plane_planar_yuv();
    test_y_plane_semi_planar();

    /* up_chroma_has_y_plane negative cases */
    test_y_plane_packed_yuv();
    test_y_plane_rgb();
    test_y_plane_opaque();
    test_y_plane_garbage();

    /* Cross-predicate invariant */
    test_opaque_implies_no_y_plane();

    /* up_chroma_subsample */
    test_subsample_table();
    test_subsample_rejects_non_planar();
    test_subsample_null_outputs();
    test_subsample_covers_every_y_plane_chroma();

    /* up_chroma_align_crop_even */
    test_align_semi_planar_420();
    test_align_planar_420();
    test_align_422_horizontal_only();
    test_align_444_and_unsupported_untouched();
    test_align_null_arguments();
    test_align_is_idempotent_and_shrinking();

    return test_harness_report();
}
