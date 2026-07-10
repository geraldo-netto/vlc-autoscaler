// SPDX-License-Identifier: GPL-2.0-or-later
/*****************************************************************************
 * test_chroma_classify.c - unit tests for chroma_classify.h
 *****************************************************************************
 * Tests the two predicates that gate hwaccel rejection (up_chroma_is_opaque)
 * and USM application (up_chroma_has_y_plane). These functions are tiny but
 * their correctness matters:
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

static int g_run = 0, g_fail = 0, g_cur_fail = 0;
static const char *g_cur = NULL;

#define BEGIN(name) do { g_cur = name; g_cur_fail = 0; g_run++; } while (0)
#define END() do { \
        if (g_cur_fail) { g_fail++; printf("  [FAIL] %s\n", g_cur); } \
        else            { printf("  [ ok ] %s\n", g_cur); } \
    } while (0)
#define CHECK(cond) do { \
        if (!(cond)) { \
            printf("    %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            g_cur_fail = 1; \
        } \
    } while (0)

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
    /* Walk the opaque table and confirm none claims a usable Y plane.
     * Keeps the two lists in sync if someone edits one without the other. */
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

int main(void)
{
    printf("Running chroma_classify tests...\n");

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

    printf("\n%d tests run, %d failed\n", g_run, g_fail);
    return g_fail == 0 ? 0 : 1;
}
