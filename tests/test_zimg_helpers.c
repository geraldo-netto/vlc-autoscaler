// SPDX-License-Identifier: GPL-2.0-or-later
/*****************************************************************************
 * test_zimg_helpers.c - unit tests for the pure helpers in zimg_helpers.h
 *****************************************************************************/

#include "../src/zimg_helpers.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
#define CHECK_EQ(a, b) do { \
        long _a = (long)(a), _b = (long)(b); \
        if (_a != _b) { \
            printf("    %s:%d: %s (=%ld) != %s (=%ld)\n", \
                   __FILE__, __LINE__, #a, _a, #b, _b); \
            g_cur_fail = 1; \
        } \
    } while (0)

/* ---------- round_up_pitch ---------- */

static void test_round_up_pitch_basic(void)
{
    BEGIN("round_up_pitch: rounds up to multiple of 64");
    CHECK_EQ(up_round_up_pitch(0),    0);
    CHECK_EQ(up_round_up_pitch(1),    64);
    CHECK_EQ(up_round_up_pitch(63),   64);
    CHECK_EQ(up_round_up_pitch(64),   64);
    CHECK_EQ(up_round_up_pitch(65),   128);
    CHECK_EQ(up_round_up_pitch(127),  128);
    CHECK_EQ(up_round_up_pitch(128),  128);
    CHECK_EQ(up_round_up_pitch(854),  896);   /* 480p luma */
    CHECK_EQ(up_round_up_pitch(427),  448);   /* 480p chroma */
    CHECK_EQ(up_round_up_pitch(1920), 1920);  /* already aligned */
    CHECK_EQ(up_round_up_pitch(1921), 1984);
    END();
}

static void test_round_up_pitch_negative(void)
{
    BEGIN("round_up_pitch: negative input clamps to 0");
    CHECK_EQ(up_round_up_pitch(-1),   0);
    CHECK_EQ(up_round_up_pitch(-100), 0);
    END();
}

/* ---------- round_up_lines ---------- */

static void test_round_up_lines_basic(void)
{
    BEGIN("round_up_lines: adds 8 rows of padding");
    CHECK_EQ(up_round_up_lines(0),    0);
    CHECK_EQ(up_round_up_lines(1),    9);
    CHECK_EQ(up_round_up_lines(480),  488);
    CHECK_EQ(up_round_up_lines(1080), 1088);
    END();
}

static void test_round_up_lines_negative(void)
{
    BEGIN("round_up_lines: negative input clamps to 0");
    CHECK_EQ(up_round_up_lines(-1), 0);
    END();
}

static void test_round_up_lines_overflow(void)
{
    BEGIN("round_up_lines: rejects overflowing padding");
    int boundary = INT_MAX - UP_SCRATCH_LINE_PAD;
    CHECK_EQ(up_round_up_lines(boundary), INT_MAX);
    CHECK_EQ(up_round_up_lines(boundary + 1), 0);
    CHECK_EQ(up_round_up_lines(INT_MAX), 0);
    END();
}

/* ---------- chroma_dim ---------- */

static void test_chroma_dim_boundaries(void)
{
    BEGIN("chroma_dim: ceil-divides without overflowing");
    CHECK_EQ(up_chroma_dim(INT_MAX, 0), INT_MAX);
    CHECK_EQ(up_chroma_dim(INT_MAX, 1), 1073741824);
    CHECK_EQ(up_chroma_dim(INT_MAX, 16), 32768);
    CHECK_EQ(up_chroma_dim(INT_MAX, 17), 32768);
    CHECK_EQ(up_chroma_dim(8, 1), 4);
    CHECK_EQ(up_chroma_dim(7, 1), 4);
    CHECK_EQ(up_chroma_dim(0, 1), 0);
    CHECK_EQ(up_chroma_dim(8, -1), 0);
    END();
}

/* ---------- plane_pitch ---------- */

static void test_plane_pitch_no_subsample(void)
{
    BEGIN("plane_pitch: sub_w=0 yields full-width pitch");
    CHECK_EQ(up_plane_pitch(854,  0), 896);
    CHECK_EQ(up_plane_pitch(1920, 0), 1920);
    CHECK_EQ(up_plane_pitch(1280, 0), 1280);
    END();
}

static void test_plane_pitch_h_subsample(void)
{
    BEGIN("plane_pitch: sub_w=1 yields half-width pitch");
    CHECK_EQ(up_plane_pitch(854,  1), 448);   /* ceil(427) padded to 448 */
    CHECK_EQ(up_plane_pitch(1920, 1), 960);
    /* odd-width source: ceil to next int, then pitch-align */
    CHECK_EQ(up_plane_pitch(7, 1), 64);       /* (7+1)/2 = 4 -> 64 */
    END();
}

static void test_plane_pitch_quarter_subsample(void)
{
    BEGIN("plane_pitch: sub_w=2 yields quarter-width pitch");
    CHECK_EQ(up_plane_pitch(1920, 2), 512);   /* 480, padded to 512 */
    END();
}

static void test_plane_pitch_zero_neg(void)
{
    BEGIN("plane_pitch: zero/negative input is 0");
    CHECK_EQ(up_plane_pitch(0,  0), 0);
    CHECK_EQ(up_plane_pitch(-1, 0), 0);
    CHECK_EQ(up_plane_pitch(1920, -1), 0);
    END();
}

/* ---------- plane_lines ---------- */

static void test_plane_lines_basic(void)
{
    BEGIN("plane_lines: subsample-aware row count + padding");
    CHECK_EQ(up_plane_lines(480,  0), 488);
    CHECK_EQ(up_plane_lines(480,  1), 248);   /* 240 + 8 pad */
    CHECK_EQ(up_plane_lines(1080, 0), 1088);
    CHECK_EQ(up_plane_lines(1080, 1), 548);
    END();
}

static void test_plane_lines_odd_height(void)
{
    BEGIN("plane_lines: odd height ceils to integer chroma rows");
    /* 7-row source with sub_h=1: ceil(7/2)=4 chroma rows + 8 pad = 12 */
    CHECK_EQ(up_plane_lines(7, 1), 12);
    END();
}

static void test_plane_geometry_extreme_height(void)
{
    BEGIN("plane geometry: propagates extreme-dimension rejection");
    CHECK_EQ(up_plane_pitch(INT_MAX, 0), 0);
    CHECK_EQ(up_plane_pitch(INT_MAX, 1), 1073741824);
    CHECK_EQ(up_plane_lines(INT_MAX, 0), 0);
    CHECK_EQ(up_plane_lines(INT_MAX, 1), 1073741832);
    END();
}

/* ---------- zimg_plane_idx ---------- */

static void test_zimg_plane_idx_no_swap(void)
{
    BEGIN("zimg_plane_idx: swap=0 is identity");
    CHECK_EQ(up_zimg_plane_idx(0, 0), 0);
    CHECK_EQ(up_zimg_plane_idx(1, 0), 1);
    CHECK_EQ(up_zimg_plane_idx(2, 0), 2);
    CHECK_EQ(up_zimg_plane_idx(3, 0), 3);  /* untouched outside [0..2] */
    END();
}

static void test_zimg_plane_idx_yv12_swap(void)
{
    BEGIN("zimg_plane_idx: swap=1 swaps U/V (planes 1,2)");
    CHECK_EQ(up_zimg_plane_idx(0, 1), 0);  /* Y unchanged */
    CHECK_EQ(up_zimg_plane_idx(1, 1), 2);  /* U <-> V */
    CHECK_EQ(up_zimg_plane_idx(2, 1), 1);
    CHECK_EQ(up_zimg_plane_idx(3, 1), 3);  /* untouched */
    END();
}

/* ---------- copy_plane ---------- */

static void test_copy_plane_same_stride(void)
{
    BEGIN("copy_plane: stride==row_bytes uses single memcpy fast path");
    uint8_t src[3 * 8] = {0};
    for (int i = 0; i < 24; i++) src[i] = (uint8_t)i;
    uint8_t dst[3 * 8] = {0};
    up_copy_plane(dst, 8, src, 8, 8, 3);
    for (int i = 0; i < 24; i++) CHECK_EQ(dst[i], src[i]);
    END();
}

static void test_copy_plane_different_strides(void)
{
    BEGIN("copy_plane: different strides copies per-row");
    /* src has stride 16 (each row 16 bytes wide, but only first 8 useful) */
    uint8_t src[4 * 16] = {0};
    /* dst has stride 12 */
    uint8_t dst[4 * 12] = {0};
    /* Mark all dst with sentinel. */
    memset(dst, 0xFF, sizeof dst);
    /* Fill src with row-major counter. */
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 8; c++)
            src[r * 16 + c] = (uint8_t)(r * 100 + c);
    /* Copy 4 rows of 8 bytes from stride-16 src to stride-12 dst. */
    up_copy_plane(dst, 12, src, 16, 8, 4);
    /* Check: bytes [0..8) of each dst row should match src row's first 8;
     * bytes [8..12) should still be 0xFF (sentinel). */
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 8; c++)
            CHECK_EQ(dst[r * 12 + c], (uint8_t)(r * 100 + c));
        for (int c = 8; c < 12; c++)
            CHECK_EQ(dst[r * 12 + c], 0xFF);
    }
    END();
}

static void test_copy_plane_zero_rows(void)
{
    BEGIN("copy_plane: rows=0 is no-op");
    uint8_t dst[16];
    memset(dst, 0xAA, 16);
    uint8_t src[16];
    memset(src, 0x55, 16);
    up_copy_plane(dst, 8, src, 8, 8, 0);
    /* dst should be unchanged */
    for (int i = 0; i < 16; i++) CHECK_EQ(dst[i], 0xAA);
    END();
}

static void test_copy_plane_zero_row_bytes(void)
{
    BEGIN("copy_plane: row_bytes=0 is no-op");
    uint8_t dst[16];
    memset(dst, 0xAA, 16);
    uint8_t src[16];
    memset(src, 0x55, 16);
    up_copy_plane(dst, 8, src, 8, 0, 4);
    for (int i = 0; i < 16; i++) CHECK_EQ(dst[i], 0xAA);
    END();
}

static void test_copy_plane_negative_inputs(void)
{
    BEGIN("copy_plane: negative rows/row_bytes is no-op");
    uint8_t dst[16];
    memset(dst, 0xAA, 16);
    uint8_t src[16];
    memset(src, 0x55, 16);
    up_copy_plane(dst, 8, src, 8, -1, 1);
    for (int i = 0; i < 16; i++) CHECK_EQ(dst[i], 0xAA);
    up_copy_plane(dst, 8, src, 8, 8, -1);
    for (int i = 0; i < 16; i++) CHECK_EQ(dst[i], 0xAA);
    END();
}

static void test_copy_plane_realistic_480p_luma(void)
{
    BEGIN("copy_plane: realistic 854x480 luma copy");
    /* src has VLC-style pitch 896, visible 854 cols, 480 rows. */
    int src_pitch = 896, dst_pitch = 854, w = 854, h = 480;
    uint8_t *src = malloc(src_pitch * h);
    uint8_t *dst = malloc(dst_pitch * h);
    /* Fill src with a deterministic pattern. */
    for (int r = 0; r < h; r++)
        for (int c = 0; c < src_pitch; c++)
            src[r * src_pitch + c] = (uint8_t)((r * 7 + c * 13) & 0xff);
    memset(dst, 0, dst_pitch * h);
    up_copy_plane(dst, dst_pitch, src, src_pitch, w, h);
    /* Verify each visible pixel matches. */
    for (int r = 0; r < h; r++) {
        for (int c = 0; c < w; c++) {
            CHECK_EQ(dst[r * dst_pitch + c],
                     src[r * src_pitch + c]);
        }
    }
    free(src);
    free(dst);
    END();
}

/* ---------- compute_stripe_bounds ---------- */

static void test_stripe_bounds_n1_full_coverage(void)
{
    BEGIN("stripe_bounds: N=1 covers the entire image in one stripe");
    int sys, sye, dys, dye;
    int ok = up_compute_stripe_bounds(0, 1, 480, 1080, &sys, &sye, &dys, &dye);
    CHECK(ok);
    CHECK_EQ(sys, 0);   CHECK_EQ(sye, 480);
    CHECK_EQ(dys, 0);   CHECK_EQ(dye, 1080);
    END();
}

static void test_stripe_bounds_n2_clean_split(void)
{
    BEGIN("stripe_bounds: N=2 on 480x1080 splits exactly at midpoint");
    int sys, sye, dys, dye;
    int ok0 = up_compute_stripe_bounds(0, 2, 480, 1080,
                                       &sys, &sye, &dys, &dye);
    CHECK(ok0);
    CHECK_EQ(sys, 0);   CHECK_EQ(sye, 240);
    CHECK_EQ(dys, 0);   CHECK_EQ(dye, 540);
    int ok1 = up_compute_stripe_bounds(1, 2, 480, 1080,
                                       &sys, &sye, &dys, &dye);
    CHECK(ok1);
    CHECK_EQ(sys, 240); CHECK_EQ(sye, 480);
    CHECK_EQ(dys, 540); CHECK_EQ(dye, 1080);
    END();
}

static void test_stripe_bounds_full_coverage_n_arbitrary(void)
{
    BEGIN("stripe_bounds: union of stripes covers entire dst for N=1..16");
    for (int n = 1; n <= 16; n++) {
        int last_dst_end = 0;
        int last_src_end = 0;
        for (int i = 0; i < n; i++) {
            int sys, sye, dys, dye;
            int ok = up_compute_stripe_bounds(i, n, 480, 1080,
                                              &sys, &sye, &dys, &dye);
            if (!ok) {
                printf("    N=%d i=%d: empty stripe\n", n, i);
                g_cur_fail = 1;
                break;
            }
            CHECK_EQ(dys, last_dst_end);  /* contiguous */
            CHECK_EQ(sys, last_src_end);
            CHECK_EQ((dys & 1), 0);       /* even */
            CHECK_EQ((sys & 1), 0);
            last_dst_end = dye;
            last_src_end = sye;
        }
        CHECK_EQ(last_dst_end, 1080);   /* covers entire dst */
        CHECK_EQ(last_src_end, 480);    /* covers entire src */
    }
    END();
}

static void test_stripe_bounds_invalid_inputs(void)
{
    BEGIN("stripe_bounds: invalid inputs return 0");
    int sys, sye, dys, dye;
    CHECK(!up_compute_stripe_bounds(0,  0, 480, 1080, &sys, &sye, &dys, &dye));
    CHECK(!up_compute_stripe_bounds(0, -1, 480, 1080, &sys, &sye, &dys, &dye));
    CHECK(!up_compute_stripe_bounds(-1, 4, 480, 1080, &sys, &sye, &dys, &dye));
    CHECK(!up_compute_stripe_bounds(5,  4, 480, 1080, &sys, &sye, &dys, &dye));
    CHECK(!up_compute_stripe_bounds(0,  4,   0, 1080, &sys, &sye, &dys, &dye));
    CHECK(!up_compute_stripe_bounds(0,  4, 480,    0, &sys, &sye, &dys, &dye));
    END();
}

/* ---------- up_zimg_stripe_min_lines ---------- */

static void test_zimg_stripe_min_lines_boundaries(void)
{
    BEGIN("up_zimg_stripe_min_lines: 0/negative -> default 16; "
          "positive returns as-is incl VLC range edges");
    /* 0 + negatives: sentinel -> default. */
    CHECK_EQ(up_zimg_stripe_min_lines(0),       UP_STRIPE_MIN_DST_LINES);
    CHECK_EQ(up_zimg_stripe_min_lines(-1),      UP_STRIPE_MIN_DST_LINES);
    CHECK_EQ(up_zimg_stripe_min_lines(INT_MIN), UP_STRIPE_MIN_DST_LINES);
    /* Positive values returned verbatim across the VLC-declared range
     * (1..128), the documented compile-time default (16), and the upper
     * out-of-range value (would be clamped by VLC; tested defensively). */
    CHECK_EQ(up_zimg_stripe_min_lines(1),       1);
    CHECK_EQ(up_zimg_stripe_min_lines(4),       4);    /* lower nominal */
    CHECK_EQ(up_zimg_stripe_min_lines(16),      16);   /* default */
    CHECK_EQ(up_zimg_stripe_min_lines(128),     128);  /* VLC max */
    CHECK_EQ(up_zimg_stripe_min_lines(129),     129);  /* VLC max + 1 */
    CHECK_EQ(up_zimg_stripe_min_lines(INT_MAX), INT_MAX);
    END();
}

static void test_stripe_bounds_ratio_preservation(void)
{
    BEGIN("stripe_bounds: per-stripe ratio close to global ratio");
    /* Global ratio 480/1080 = 0.4444. Each stripe should be within
     * 1/n of that ratio. */
    int n = 4;
    for (int i = 0; i < n; i++) {
        int sys, sye, dys, dye;
        int ok = up_compute_stripe_bounds(i, n, 480, 1080,
                                          &sys, &sye, &dys, &dye);
        CHECK(ok);
        int dst_stripe_h = dye - dys;
        int src_stripe_h = sye - sys;
        /* Allow a small drift but no degeneration. */
        CHECK(dst_stripe_h > 0);
        CHECK(src_stripe_h > 0);
        /* Ratio should be within 10% of global 480/1080 ~= 0.444. */
        double ratio = (double)src_stripe_h / dst_stripe_h;
        CHECK(ratio > 0.40 && ratio < 0.49);
    }
    END();
}

/* SCAL-3 grid: rows*cols <= n, cols>1 only when height-bound. */
static void test_decide_tile_grid(void)
{
    BEGIN("up_decide_tile_grid");
    int r, c;

    /* Tall enough: pure row striping, no columns. */
    up_decide_tile_grid(8, 1920, 1080, 16, 64, &r, &c);
    CHECK_EQ(r, 8); CHECK_EQ(c, 1);

    /* n_threads <= max_rows (1080/16=67): still cols==1. */
    up_decide_tile_grid(16, 1920, 1080, 16, 64, &r, &c);
    CHECK_EQ(r, 16); CHECK_EQ(c, 1);

    /* Wide + short: choose the product that uses all 16 workers. */
    up_decide_tile_grid(16, 1920, 96, 16, 64, &r, &c);
    CHECK_EQ(r, 4); CHECK_EQ(c, 4);
    CHECK(r * c <= 16);

    /* Regression: row-first selection used only 8 of 14 workers. */
    up_decide_tile_grid(14, 1920, 128, 16, 64, &r, &c);
    CHECK_EQ(r, 7); CHECK_EQ(c, 2);

    /* Equal 12-cell products prefer the grid with more row stripes. */
    up_decide_tile_grid(13, 192, 96, 16, 64, &r, &c);
    CHECK_EQ(r, 6); CHECK_EQ(c, 2);

    /* Column cap by width: dst_w=80 -> max_cols=80/64=1, so cols stays 1. */
    up_decide_tile_grid(16, 80, 96, 16, 64, &r, &c);
    CHECK_EQ(r, 6); CHECK_EQ(c, 1);

    /* Degenerate / invalid inputs clamp to >=1 and never overflow the budget.
     * -1, 0, 1, INT_MAX, INT_MIN for each param; the fuzzer sweeps the full
     * cross-product, these pin the contract as a regression guard. */
    up_decide_tile_grid(0, 0, 0, 16, 64, &r, &c);
    CHECK(r == 1 && c == 1);
    up_decide_tile_grid(-1, 1920, 96, 16, 64, &r, &c);   /* n<1 -> 1 cell */
    CHECK(r == 1 && c == 1);
    up_decide_tile_grid(1, 1920, 96, 16, 64, &r, &c);    /* n==1 -> 1 cell */
    CHECK(r == 1 && c == 1);
    up_decide_tile_grid(16, 1920, 96, -1, 64, &r, &c);   /* stripe_min<=0 */
    CHECK(r >= 1 && c >= 1 && (long long)r * c <= 16);
    up_decide_tile_grid(16, 1920, 96, 16, 0, &r, &c);    /* col_min<=0 -> cols 1 */
    CHECK_EQ(r, 6); CHECK_EQ(c, 1);
    up_decide_tile_grid(1000, 8192, 8192, 1, 1, &r, &c);
    CHECK_EQ(r * c, UP_TILE_THREADS_MAX);
    up_decide_tile_grid(INT_MAX, INT_MAX, INT_MAX, 16, 64, &r, &c);
    CHECK(r >= 1 && c >= 1);
    up_decide_tile_grid(INT_MIN, INT_MIN, INT_MIN, INT_MIN, INT_MIN, &r, &c);
    CHECK(r == 1 && c == 1);
    END();
}

int main(void)
{
    printf("Running zimg_helpers tests...\n");

    test_round_up_pitch_basic();
    test_round_up_pitch_negative();
    test_round_up_lines_basic();
    test_round_up_lines_negative();
    test_round_up_lines_overflow();
    test_chroma_dim_boundaries();

    test_plane_pitch_no_subsample();
    test_plane_pitch_h_subsample();
    test_plane_pitch_quarter_subsample();
    test_plane_pitch_zero_neg();

    test_plane_lines_basic();
    test_plane_lines_odd_height();
    test_plane_geometry_extreme_height();

    test_zimg_plane_idx_no_swap();
    test_zimg_plane_idx_yv12_swap();

    test_copy_plane_same_stride();
    test_copy_plane_different_strides();
    test_copy_plane_zero_rows();
    test_copy_plane_zero_row_bytes();
    test_copy_plane_negative_inputs();
    test_copy_plane_realistic_480p_luma();

    test_stripe_bounds_n1_full_coverage();
    test_stripe_bounds_n2_clean_split();
    test_stripe_bounds_full_coverage_n_arbitrary();
    test_stripe_bounds_invalid_inputs();
    test_stripe_bounds_ratio_preservation();

    test_zimg_stripe_min_lines_boundaries();
    test_decide_tile_grid();

    printf("\n%d tests run, %d failed\n", g_run, g_fail);
    return g_fail == 0 ? 0 : 1;
}
