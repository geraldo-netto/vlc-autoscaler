// SPDX-License-Identifier: GPL-2.0-or-later
/*****************************************************************************
 * test_content_probe.c - unit tests for the content-probe metrics
 *****************************************************************************
 * Tests three things:
 *   1. Squared Laplacian response: zero on flat, high on texture
 *   2. Block-edge strength: zero on smooth plane, high on synthetic blocky
 *   3. Advisory decision: returns 1 only on (very soft AND very blocky)
 *****************************************************************************/

#include "../src/content_probe.h"

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_tests_run = 0;
static int g_tests_failed = 0;
static int g_failed_in_test = 0;

#define BEGIN(name) do { \
    printf("  [....] %s\n", name); \
    g_failed_in_test = 0; \
} while (0)

#define END() do { \
    g_tests_run++; \
    if (g_failed_in_test) { \
        printf("  [FAIL] %s\n", "(see above)"); \
        g_tests_failed++; \
    } \
} while (0)

#define CHECK(cond) do { \
    if (!(cond)) { \
        printf("    %s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
        g_failed_in_test = 1; \
    } \
} while (0)

#define CHECK_EQ(a, b) do { \
    long long _a = (long long)(a); \
    long long _b = (long long)(b); \
    if (_a != _b) { \
        printf("    %s:%d: CHECK_EQ failed: %lld != %lld\n", \
               __FILE__, __LINE__, _a, _b); \
        g_failed_in_test = 1; \
    } \
} while (0)

/* ---------- Squared Laplacian response ---------- */

static void test_laplacian_flat_plane_zero(void)
{
    BEGIN("laplacian energy: flat plane has zero response");
    int w = 64, h = 64;
    uint8_t buf[64 * 64];
    memset(buf, 128, sizeof buf);
    uint64_t n = 0;
    uint64_t sum = up_laplacian_variance(buf, w, w, h, &n);
    CHECK_EQ(sum, 0);
    CHECK(n > 0);  /* did sample, just got zero response */
    END();
}

static void test_laplacian_checkerboard_high(void)
{
    BEGIN("laplacian energy: 1-pixel checkerboard has high response");
    int w = 64, h = 64;
    uint8_t buf[64 * 64];
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            buf[y * w + x] = ((x + y) & 1) ? 255 : 0;
    uint64_t n = 0;
    uint64_t sum = up_laplacian_variance(buf, w, w, h, &n);
    /* Need samples to compute a mean. Failing this would indicate the
     * function bailed out unexpectedly. */
    if (n == 0) {
        CHECK_EQ(1, 0);  /* no samples taken */
        END();
        return;
    }
    /* Each Laplacian response is ±1020; squared = 1,040,400.
     * With many samples, sum/n should be near 1,040,400. */
    uint64_t mean = sum / n;
    CHECK(mean > 100000);
    END();
}

static void test_laplacian_invalid_input(void)
{
    BEGIN("laplacian energy: invalid inputs return 0");
    uint64_t n = 999;
    CHECK_EQ(up_laplacian_variance(NULL, 64, 64, 64, &n), 0);
    CHECK_EQ(n, 0);

    const uint8_t scratch[64] = {0};  /* read-only, values irrelevant - function rejects on shape */
    n = 999;
    CHECK_EQ(up_laplacian_variance(scratch, 0, 64, 64, &n), 0);  /* stride=0 */
    CHECK_EQ(up_laplacian_variance(scratch, 64, 1, 64, &n), 0);  /* w<=2 */
    CHECK_EQ(up_laplacian_variance(scratch, 64, 64, 1, &n), 0);  /* h<=2 */

    /* NULL n_samples_out is allowed (just don't write to it). */
    const uint8_t buf2[64*64] = {0};
    (void)up_laplacian_variance(buf2, 64, 64, 64, NULL);  /* must not crash */
    END();
}

/* ---------- Block-edge strength ---------- */

static void test_block_edge_smooth_low(void)
{
    BEGIN("block_edge: gradient plane has low block-edge intensity");
    int w = 64, h = 64;
    uint8_t buf[64 * 64];
    /* Smooth gradient — values change continuously, no block boundaries. */
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            buf[y * w + x] = (uint8_t)((x + y) & 0xff);
    uint64_t n = 0;
    uint64_t sum = up_block_edge_strength(buf, w, w, h, &n);
    if (n == 0) {
        CHECK_EQ(1, 0);  /* no samples taken */
        END();
        return;
    }
    /* Each block-edge measurement is just |x - (x-1)| = 1 in our
     * gradient. Mean should be small (1-2). */
    uint64_t mean = sum / n;
    CHECK(mean <= 2);
    END();
}

static void test_block_edge_blocky_high(void)
{
    BEGIN("block_edge: synthetic blocky plane has high block-edge intensity");
    int w = 64, h = 64;
    uint8_t buf[64 * 64];
    /* Synthesize a blocky pattern: each 8x8 block has a different mean
     * value with no smooth transition. */
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            int bx = x / 8;
            int by = y / 8;
            buf[y * w + x] = (uint8_t)(((bx + by) * 50) & 0xff);
        }
    }
    uint64_t n = 0;
    uint64_t sum = up_block_edge_strength(buf, w, w, h, &n);
    if (n == 0) {
        CHECK_EQ(1, 0);  /* no samples taken */
        END();
        return;
    }
    uint64_t mean = sum / n;
    /* Adjacent blocks differ by 50; we sample boundary pixels only.
     * Mean should be substantially above the smooth case. */
    CHECK(mean >= 10);
    END();
}

static void test_block_edge_invalid_input(void)
{
    BEGIN("block_edge: invalid inputs return 0");
    uint64_t n = 999;
    CHECK_EQ(up_block_edge_strength(NULL, 64, 64, 64, &n), 0);
    CHECK_EQ(n, 0);

    const uint8_t buf[64*64] = {0};
    CHECK_EQ(up_block_edge_strength(buf, 0, 64, 64, &n), 0);
    /* w/h smaller than block size: must return 0 cleanly. */
    CHECK_EQ(up_block_edge_strength(buf, 4, 4, 64, &n), 0);
    CHECK_EQ(up_block_edge_strength(buf, 64, 64, 4, &n), 0);

    /* NULL n_samples_out OK. */
    (void)up_block_edge_strength(buf, 64, 64, 64, NULL);
    END();
}

/* ---------- Advisory decision ---------- */

static void test_bypass_too_few_frames(void)
{
    BEGIN("advisory: too few frames -> no recommendation");
    up_probe_accum_t a = {0};
    a.frames = UP_PROBE_MIN_FRAMES - 1;
    a.lap_samples  = UP_PROBE_MIN_SAMPLES_PER_KIND * 10;
    a.edge_samples = UP_PROBE_MIN_SAMPLES_PER_KIND * 10;
    /* Even with "very bad" metrics, insufficient frames -> proceed. */
    a.lap_sum = 0;  /* extremely soft */
    a.edge_sum = a.edge_samples * 100;  /* extremely blocky */
    CHECK_EQ(up_should_bypass_for_content(&a), 0);
    END();
}

static void test_bypass_clean_source_no_bypass(void)
{
    BEGIN("advisory: clean source -> no recommendation");
    up_probe_accum_t a = {0};
    a.frames = 60;
    a.lap_samples  = UP_PROBE_MIN_SAMPLES_PER_KIND * 10;
    a.edge_samples = UP_PROBE_MIN_SAMPLES_PER_KIND * 10;
    /* Sharp source: mean stays above the soft cutoff. */
    a.lap_sum = a.lap_samples * 1500ULL;
    /* Smooth: edge mean = 2 */
    a.edge_sum = a.edge_samples * 2;
    CHECK_EQ(up_should_bypass_for_content(&a), 0);
    END();
}

static void test_bypass_soft_only_no_bypass(void)
{
    BEGIN("advisory: soft-only source -> no recommendation");
    up_probe_accum_t a = {0};
    a.frames = 60;
    a.lap_samples  = UP_PROBE_MIN_SAMPLES_PER_KIND * 10;
    a.edge_samples = UP_PROBE_MIN_SAMPLES_PER_KIND * 10;
    /* Very soft: mean stays below the soft cutoff. */
    a.lap_sum = a.lap_samples * 100ULL;
    /* Smooth: edge mean = 2 */
    a.edge_sum = a.edge_samples * 2;
    CHECK_EQ(up_should_bypass_for_content(&a), 0);
    END();
}

static void test_bypass_blocky_only_no_bypass(void)
{
    BEGIN("advisory: blocky-only source -> no recommendation");
    up_probe_accum_t a = {0};
    a.frames = 60;
    a.lap_samples  = UP_PROBE_MIN_SAMPLES_PER_KIND * 10;
    a.edge_samples = UP_PROBE_MIN_SAMPLES_PER_KIND * 10;
    /* Sharp mean. Under the old squared-threshold bug this counted as very
     * soft and advised on blockiness alone. */
    a.lap_sum = a.lap_samples * 1500ULL;
    /* Blocky: edge mean = 12 */
    a.edge_sum = a.edge_samples * 12;
    CHECK_EQ(up_should_bypass_for_content(&a), 0);
    END();
}

static void test_bypass_soft_and_blocky_yes_bypass(void)
{
    BEGIN("advisory: soft AND blocky -> recommend disabling upscale");
    up_probe_accum_t a = {0};
    a.frames = 60;
    a.lap_samples  = UP_PROBE_MIN_SAMPLES_PER_KIND * 10;
    a.edge_samples = UP_PROBE_MIN_SAMPLES_PER_KIND * 10;
    /* Very soft mean below the configured cutoff. */
    a.lap_sum = a.lap_samples * 200ULL;
    /* Very blocky: edge mean = 12 */
    a.edge_sum = a.edge_samples * 12;
    CHECK_EQ(up_should_bypass_for_content(&a), 1);
    END();
}

static void test_bypass_soft_threshold_boundary(void)
{
    BEGIN("advisory: soft threshold is linear and exclusive at 400");
    up_probe_accum_t a = {0};
    a.frames = 60;
    a.lap_samples  = UP_PROBE_MIN_SAMPLES_PER_KIND * 10;
    a.edge_samples = UP_PROBE_MIN_SAMPLES_PER_KIND * 10;
    a.edge_sum = a.edge_samples * 12;           /* very blocky */

    a.lap_sum = a.lap_samples * (UP_PROBE_THRESH_SOFT_LAP_MEAN - 1);
    CHECK_EQ(up_should_bypass_for_content(&a), 1);   /* 399: soft */

    a.lap_sum = a.lap_samples * UP_PROBE_THRESH_SOFT_LAP_MEAN;
    CHECK_EQ(up_should_bypass_for_content(&a), 0);   /* 400: not soft */
    END();
}

static void test_bypass_null_input(void)
{
    BEGIN("advisory: NULL accumulator -> no recommendation");
    CHECK_EQ(up_should_bypass_for_content(NULL), 0);
    END();
}

/* Helper: build a probe_accum populated with `samples` luma samples
 * whose mean equals `lap_mean` (i.e. lap_sum = samples * lap_mean). */
static up_probe_accum_t make_accum_with_lap_mean(uint64_t lap_mean,
                                                  uint64_t samples)
{
    up_probe_accum_t a = {0};
    a.frames = 60;
    a.lap_samples = samples;
    a.lap_sum     = lap_mean * samples;
    return a;
}

static void test_skip_usm_threshold_disabled_sentinel(void)
{
    BEGIN("skip-usm: threshold <= 0 disables feature regardless of "
          "lap_mean (0/-1/INT_MIN never trip)");
    up_probe_accum_t hot = make_accum_with_lap_mean(
        100000ULL, UP_PROBE_MIN_SAMPLES_PER_KIND * 4);
    /* Even with a wildly textured source, a non-positive threshold
     * means feature off → never skip. */
    CHECK_EQ(up_should_skip_usm_for_sharpness(&hot, 0),       0);
    CHECK_EQ(up_should_skip_usm_for_sharpness(&hot, -1),      0);
    CHECK_EQ(up_should_skip_usm_for_sharpness(&hot, INT_MIN), 0);
    END();
}

static void test_skip_usm_threshold_strict_greater(void)
{
    BEGIN("skip-usm: lap_mean > threshold trips; lap_mean == threshold "
          "does NOT (strict comparison)");
    up_probe_accum_t a = make_accum_with_lap_mean(
        3500ULL, UP_PROBE_MIN_SAMPLES_PER_KIND * 4);
    /* lap_mean = 3500 exactly */
    CHECK_EQ(up_should_skip_usm_for_sharpness(&a, 3499), 1); /* strict greater */
    CHECK_EQ(up_should_skip_usm_for_sharpness(&a, 3500), 0); /* equal: no */
    CHECK_EQ(up_should_skip_usm_for_sharpness(&a, 3501), 0); /* below: no */
    END();
}

static void test_skip_usm_threshold_vlc_range_edges(void)
{
    BEGIN("skip-usm: threshold spans VLC range 1..20000 plus default; "
          "INT_MAX never trips on realistic content");
    /* Realistic clean-grainy lap_mean = 1500 (within the 800-2000
     * comment band). Trips at low threshold, doesn't trip at default
     * 3500 or any higher cutoff. */
    up_probe_accum_t clean = make_accum_with_lap_mean(
        1500ULL, UP_PROBE_MIN_SAMPLES_PER_KIND * 4);
    CHECK_EQ(up_should_skip_usm_for_sharpness(&clean, 1),     1);
    CHECK_EQ(up_should_skip_usm_for_sharpness(&clean, 1499),  1);
    CHECK_EQ(up_should_skip_usm_for_sharpness(&clean, 1500),  0);  /* equal */
    CHECK_EQ(up_should_skip_usm_for_sharpness(&clean,
        UP_PROBE_THRESH_SHARP_LAP_MEAN), 0);                       /* default */
    CHECK_EQ(up_should_skip_usm_for_sharpness(&clean, 20000), 0);  /* VLC max */
    CHECK_EQ(up_should_skip_usm_for_sharpness(&clean, INT_MAX), 0);

    /* Pathologically textured: lap_mean = 50000 trips at default 3500
     * but not at INT_MAX. */
    up_probe_accum_t hot = make_accum_with_lap_mean(
        50000ULL, UP_PROBE_MIN_SAMPLES_PER_KIND * 4);
    CHECK_EQ(up_should_skip_usm_for_sharpness(&hot,
        UP_PROBE_THRESH_SHARP_LAP_MEAN), 1);
    CHECK_EQ(up_should_skip_usm_for_sharpness(&hot, 49999), 1);
    CHECK_EQ(up_should_skip_usm_for_sharpness(&hot, 50000), 0);  /* equal */
    CHECK_EQ(up_should_skip_usm_for_sharpness(&hot, INT_MAX), 0);
    END();
}

static void test_skip_usm_safe_defaults(void)
{
    BEGIN("skip-usm: NULL accum + zero-sample accum -> never trip "
          "regardless of threshold");
    CHECK_EQ(up_should_skip_usm_for_sharpness(NULL, 3500), 0);
    CHECK_EQ(up_should_skip_usm_for_sharpness(NULL, 0),    0);
    up_probe_accum_t empty = {0};
    CHECK_EQ(up_should_skip_usm_for_sharpness(&empty, 3500), 0);
    CHECK_EQ(up_should_skip_usm_for_sharpness(&empty, 1),    0);
    END();
}

static void test_observe_accumulates(void)
{
    BEGIN("observe: accumulates across frames");
    up_probe_accum_t a = {0};
    up_probe_observe(&a, 100, 50, 10, 5);
    up_probe_observe(&a, 200, 50, 20, 5);
    up_probe_observe(&a, 300, 50, 30, 5);
    CHECK_EQ(a.frames, 3);
    CHECK_EQ(a.lap_sum, 600);
    CHECK_EQ(a.lap_samples, 150);
    CHECK_EQ(a.edge_sum, 60);
    CHECK_EQ(a.edge_samples, 15);
    /* NULL accumulator must not crash. */
    up_probe_observe(NULL, 1, 2, 3, 4);
    END();
}

/* Defensive stride < width guard in both metric functions: they pass the
 * first validity gate (w/h above the block size, stride > 0) but a stride
 * smaller than the width would read past row storage, so they bail with 0. */
static void test_probe_stride_less_than_width(void)
{
    BEGIN("stride < width rejected by both metric functions");
    static uint8_t buf[64 * 64];
    memset(buf, 128, sizeof buf);
    uint64_t n = 123;
    CHECK(up_laplacian_variance(buf, 20, 40, 40, &n) == 0);
    n = 123;
    CHECK(up_block_edge_strength(buf, 20, 40, 40, &n) == 0);
    END();
}

int main(void)
{
    printf("Running content_probe tests...\n");

    test_laplacian_flat_plane_zero();
    test_laplacian_checkerboard_high();
    test_laplacian_invalid_input();

    test_block_edge_smooth_low();
    test_block_edge_blocky_high();
    test_block_edge_invalid_input();

    test_bypass_too_few_frames();
    test_bypass_clean_source_no_bypass();
    test_bypass_soft_only_no_bypass();
    test_bypass_blocky_only_no_bypass();
    test_bypass_soft_and_blocky_yes_bypass();
    test_bypass_soft_threshold_boundary();
    test_bypass_null_input();

    test_skip_usm_threshold_disabled_sentinel();
    test_skip_usm_threshold_strict_greater();
    test_skip_usm_threshold_vlc_range_edges();
    test_skip_usm_safe_defaults();

    test_observe_accumulates();
    test_probe_stride_less_than_width();

    printf("\n%d tests run, %d failed\n", g_tests_run, g_tests_failed);
    return g_tests_failed == 0 ? 0 : 1;
}
