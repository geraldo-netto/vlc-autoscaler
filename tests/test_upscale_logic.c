/*****************************************************************************
 * test_upscale_logic.c — unit tests for the pure decision/math logic
 *****************************************************************************
 * Build & run:  make -C tests test
 *
 * No external test framework — tiny assertion macros that print pass/fail
 * and a non-zero exit code on any failure (suitable for CI).
 *****************************************************************************/

#include "../src/upscale_logic.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------- minimal test harness ---------- */

static int g_tests_run = 0;
static int g_tests_failed = 0;
static const char *g_current_test = NULL;
static int g_current_failed = 0;

#define BEGIN(name) do { \
        g_current_test = (name); \
        g_current_failed = 0; \
        g_tests_run++; \
    } while (0)

#define END() do { \
        if (g_current_failed) { \
            g_tests_failed++; \
            printf("  [FAIL] %s\n", g_current_test); \
        } else { \
            printf("  [ ok ] %s\n", g_current_test); \
        } \
    } while (0)

#define CHECK(cond) do { \
        if (!(cond)) { \
            printf("    %s:%d: CHECK failed: %s\n", \
                   __FILE__, __LINE__, #cond); \
            g_current_failed = 1; \
        } \
    } while (0)

#define CHECK_EQ_INT(a, b) do { \
        long _a = (long)(a); \
        long _b = (long)(b); \
        if (_a != _b) { \
            printf("    %s:%d: CHECK_EQ_INT failed: %s (=%ld) != %s (=%ld)\n", \
                   __FILE__, __LINE__, #a, _a, #b, _b); \
            g_current_failed = 1; \
        } \
    } while (0)

/* ---------- decide_target_height ---------- */

static void test_decide_forced_720p(void)
{
    BEGIN("decide_target_height: preset=720 forces 720");
    CHECK_EQ_INT(up_decide_target_height(480, UP_TARGET_720P, 8, 16384), 720);
    CHECK_EQ_INT(up_decide_target_height(360, UP_TARGET_720P, 1,   512), 720);
    CHECK_EQ_INT(up_decide_target_height(540, UP_TARGET_720P, 4,  4096), 720);
    END();
}

static void test_decide_forced_1080p(void)
{
    BEGIN("decide_target_height: preset=1080 forces 1080 within ratio cap");
    CHECK_EQ_INT(up_decide_target_height(540, UP_TARGET_1080P, 8, 16384), 1080);
    CHECK_EQ_INT(up_decide_target_height(720, UP_TARGET_1080P, 8, 16384), 1080);
    /* From 240p, UP_MAX_RATIO=4 caps at 960, NOT 1080. */
    CHECK_EQ_INT(up_decide_target_height(240, UP_TARGET_1080P, 8, 16384), 960);
    END();
}

static void test_decide_auto_strong_hw(void)
{
    BEGIN("decide_target_height: auto picks 1080 with strong hw");
    /* 540p, 8 cores, 8 GB -> 1080p */
    CHECK_EQ_INT(up_decide_target_height(540, UP_TARGET_AUTO, 8, 8192), 1080);
    /* 480p, 4 cores, 4 GB -> 1080p */
    CHECK_EQ_INT(up_decide_target_height(480, UP_TARGET_AUTO, 4, 4096), 1080);
    /* 360p, 8 cores, 8 GB -> 1080p (ratio 3x, within cap) */
    CHECK_EQ_INT(up_decide_target_height(360, UP_TARGET_AUTO, 8, 8192), 1080);
    END();
}

static void test_decide_auto_weak_hw(void)
{
    BEGIN("decide_target_height: auto falls back to 720 on weak hw");
    /* 2 cores -> 720p */
    CHECK_EQ_INT(up_decide_target_height(540, UP_TARGET_AUTO, 2, 8192), 720);
    /* 1 GB RAM -> 720p */
    CHECK_EQ_INT(up_decide_target_height(540, UP_TARGET_AUTO, 8, 1024), 720);
    /* mem=0 (unknown) is treated as sufficient, so this should be 1080 */
    CHECK_EQ_INT(up_decide_target_height(540, UP_TARGET_AUTO, 8, 0), 1080);
    END();
}

static void test_decide_auto_tiny_source(void)
{
    BEGIN("decide_target_height: auto falls back when 1080p needs > 4x");
    /* 240p source: 1080p would be 4.5x — outside ratio cap.
     * Auto mode declines 1080p and picks 720p. */
    CHECK_EQ_INT(up_decide_target_height(240, UP_TARGET_AUTO, 8, 16384), 720);
    /* 144p: 1080p is 7.5x. Auto picks 720p, then ratio cap clamps to 576. */
    CHECK_EQ_INT(up_decide_target_height(144, UP_TARGET_AUTO, 8, 16384), 576);

    /* But forced 1080p with the same tiny source clamps to 4x exactly. */
    CHECK_EQ_INT(up_decide_target_height(240, UP_TARGET_1080P, 8, 16384), 960);
    CHECK_EQ_INT(up_decide_target_height(144, UP_TARGET_1080P, 8, 16384), 576);
    END();
}

static void test_decide_invalid_input(void)
{
    BEGIN("decide_target_height: invalid src_h returns 0");
    CHECK_EQ_INT(up_decide_target_height(0,   UP_TARGET_AUTO, 8, 8192), 0);
    CHECK_EQ_INT(up_decide_target_height(-1,  UP_TARGET_AUTO, 8, 8192), 0);
    CHECK_EQ_INT(up_decide_target_height(-9999, UP_TARGET_720P, 8, 8192), 0);
    END();
}

static void test_decide_no_downscale(void)
{
    BEGIN("decide_target_height: never downscales");
    /* 1440p input with preset=720 -> 1440 (don't downscale). */
    int r = up_decide_target_height(1440, UP_TARGET_720P, 8, 8192);
    CHECK(r >= 1440);
    /* 1080p input with auto -> >= 1080 */
    r = up_decide_target_height(1080, UP_TARGET_AUTO, 8, 8192);
    CHECK(r >= 1080);
    END();
}

/* ---------- compute_target_dims ---------- */

static void test_compute_basic_aspect(void)
{
    BEGIN("compute_target_dims: 16:9 source preserves aspect");
    up_dims_t d = {0};
    /* 854x480 -> target 720 -> 1280x720 */
    CHECK(up_compute_target_dims(854, 480, 720, &d));
    CHECK_EQ_INT(d.width, 1280);
    CHECK_EQ_INT(d.height, 720);

    /* 640x360 -> target 1080 -> 1920x1080 */
    CHECK(up_compute_target_dims(640, 360, 1080, &d));
    CHECK_EQ_INT(d.width, 1920);
    CHECK_EQ_INT(d.height, 1080);
    END();
}

static void test_compute_4_3_aspect(void)
{
    BEGIN("compute_target_dims: 4:3 source preserves aspect");
    up_dims_t d = {0};
    /* 640x480 -> target 720 -> 960x720 */
    CHECK(up_compute_target_dims(640, 480, 720, &d));
    CHECK_EQ_INT(d.width, 960);
    CHECK_EQ_INT(d.height, 720);
    END();
}

static void test_compute_even_rounding(void)
{
    BEGIN("compute_target_dims: dimensions are even");
    up_dims_t d = {0};
    /* Force odd target to verify rounding. */
    CHECK(up_compute_target_dims(720, 405, 721, &d));
    CHECK_EQ_INT(d.height & 1, 0);
    CHECK_EQ_INT(d.width  & 1, 0);

    /* Source with odd-resulting width - 720x405 -> 720 -> 1280x720 */
    CHECK(up_compute_target_dims(720, 405, 720, &d));
    CHECK_EQ_INT(d.width & 1, 0);
    CHECK_EQ_INT(d.height & 1, 0);
    END();
}

static void test_compute_invalid_input(void)
{
    BEGIN("compute_target_dims: invalid input returns 0");
    up_dims_t d;

    /* zero src */
    memset(&d, 0xab, sizeof d);
    CHECK_EQ_INT(up_compute_target_dims(0, 480, 720, &d), 0);
    CHECK_EQ_INT(d.width, 0);
    CHECK_EQ_INT(d.height, 0);

    /* negative */
    memset(&d, 0xab, sizeof d);
    CHECK_EQ_INT(up_compute_target_dims(854, -480, 720, &d), 0);
    CHECK_EQ_INT(d.width, 0);

    /* zero target */
    memset(&d, 0xab, sizeof d);
    CHECK_EQ_INT(up_compute_target_dims(854, 480, 0, &d), 0);
    CHECK_EQ_INT(d.width, 0);

    /* NULL out */
    CHECK_EQ_INT(up_compute_target_dims(854, 480, 720, NULL), 0);
    END();
}

static void test_compute_overflow_safe(void)
{
    BEGIN("compute_target_dims: large values don't overflow");
    up_dims_t d = {0};
    /* 30000-wide source upscaled. Past UP_MAX_DIM, should reject. */
    int rc = up_compute_target_dims(30000, 16000, 32768, &d);
    /* Either rejected, or result is capped under UP_MAX_DIM. */
    if (rc) {
        CHECK(d.width  <= UP_MAX_DIM);
        CHECK(d.height <= UP_MAX_DIM);
    } else {
        CHECK_EQ_INT(d.width, 0);
        CHECK_EQ_INT(d.height, 0);
    }
    END();
}

/* ---------- plan_upscale (the integration entry point) ---------- */

static void test_plan_typical_sd_to_hd(void)
{
    BEGIN("plan_upscale: 480p input -> upscale to 720p (default skip=720)");
    up_dims_t d = {0};
    int rc = up_plan_upscale(854, 480, 720, UP_TARGET_AUTO, 2, 4096, &d);
    CHECK_EQ_INT(rc, 1);
    CHECK_EQ_INT(d.height, 720);
    CHECK_EQ_INT(d.width,  1280);
    END();
}

static void test_plan_already_hd(void)
{
    BEGIN("plan_upscale: 720p input bypasses (skip-above=720)");
    up_dims_t d = {0};
    int rc = up_plan_upscale(1280, 720, 720, UP_TARGET_AUTO, 8, 8192, &d);
    CHECK_EQ_INT(rc, 0);
    CHECK_EQ_INT(d.width, 0);
    CHECK_EQ_INT(d.height, 0);
    END();
}

static void test_plan_skip_above_lowered(void)
{
    BEGIN("plan_upscale: skip_above=1080 lets 720p sources through");
    up_dims_t d = {0};
    int rc = up_plan_upscale(1280, 720, 1080, UP_TARGET_1080P, 8, 8192, &d);
    CHECK_EQ_INT(rc, 1);
    CHECK_EQ_INT(d.height, 1080);
    CHECK_EQ_INT(d.width,  1920);
    END();
}

static void test_plan_invalid(void)
{
    BEGIN("plan_upscale: invalid input -> bypass with zeroed out");
    up_dims_t d;

    memset(&d, 0xab, sizeof d);
    CHECK_EQ_INT(up_plan_upscale(0, 480, 720, UP_TARGET_AUTO, 4, 4096, &d), 0);
    CHECK_EQ_INT(d.width, 0); CHECK_EQ_INT(d.height, 0);

    memset(&d, 0xab, sizeof d);
    CHECK_EQ_INT(up_plan_upscale(854, 0, 720, UP_TARGET_AUTO, 4, 4096, &d), 0);
    CHECK_EQ_INT(d.width, 0); CHECK_EQ_INT(d.height, 0);

    memset(&d, 0xab, sizeof d);
    CHECK_EQ_INT(up_plan_upscale(-1, -1, 720, UP_TARGET_AUTO, 4, 4096, &d), 0);
    CHECK_EQ_INT(d.width, 0); CHECK_EQ_INT(d.height, 0);

    CHECK_EQ_INT(up_plan_upscale(854, 480, 720, UP_TARGET_AUTO, 4, 4096, NULL), 0);
    END();
}

static void test_plan_anamorphic(void)
{
    BEGIN("plan_upscale: anamorphic (non-16:9) source preserved");
    /* 720x576 PAL DV -> aspect ~1.25, target 720 height -> 900x720 */
    up_dims_t d = {0};
    int rc = up_plan_upscale(720, 576, 720, UP_TARGET_720P, 4, 4096, &d);
    CHECK_EQ_INT(rc, 1);
    CHECK_EQ_INT(d.height, 720);
    CHECK_EQ_INT(d.width,  900);
    END();
}

static void test_plan_pathological_aspect_regression(void)
{
    BEGIN("plan_upscale: regression — pathological aspect bypasses cleanly");
    /* libFuzzer found that src=3x1024 with target_h=1080 computed
     * out=2x1080, i.e. width *shrunk* from 3 to 2 due to even-rounding.
     * Such cases must now bypass the filter, not return a fake upscale. */
    up_dims_t d;

    memset(&d, 0xab, sizeof d);
    int rc = up_plan_upscale(3, 1024, 9999, UP_TARGET_1080P, 8, 8192, &d);
    CHECK_EQ_INT(rc, 0);
    CHECK_EQ_INT(d.width, 0);
    CHECK_EQ_INT(d.height, 0);

    /* Same idea, slightly different ratio. */
    memset(&d, 0xab, sizeof d);
    rc = up_plan_upscale(5, 700, 9999, UP_TARGET_1080P, 8, 8192, &d);
    /* 5*1080/700 = 7.71 -> 7 -> even round -> 6, which IS > 5, so this
     * should succeed with width 6. */
    if (rc) {
        CHECK(d.width >= 5);
        CHECK(d.height >= 700);
    }
    END();
}

/* ---------- ratio invariants for many random-ish inputs ---------- */

static void test_aspect_invariant_smoke(void)
{
    BEGIN("plan_upscale: aspect ratio preserved within rounding (smoke)");
    /* deterministic LCG */
    uint32_t s = 12345;
    int violations = 0;
    for (int i = 0; i < 5000; i++) {
        s = s * 1103515245u + 12345u;
        int w = 16 + (int)(s % 1900);
        s = s * 1103515245u + 12345u;
        int h = 16 + (int)(s % 1060);

        up_dims_t d = {0};
        if (up_plan_upscale(w, h, 720, UP_TARGET_AUTO, 4, 4096, &d)) {
            /* Compare aspect ratios using cross multiplication.
             * After even-rounding target_w = src_w * target_h / src_h,
             * the rounding error in the cross product
             *   |src_w * out.height - out.width * src_h|
             * is bounded by 2 * src_h (1 from floor div, 1 from even-round). */
            int64_t lhs = (int64_t)w * (int64_t)d.height;
            int64_t rhs = (int64_t)d.width * (int64_t)h;
            int64_t diff = lhs - rhs;
            if (diff < 0) diff = -diff;
            int64_t slack = 2LL * h + 4LL;
            if (diff > slack) {
                violations++;
                if (violations < 5) {
                    printf("    aspect violation: %dx%d -> %dx%d "
                           "(diff=%lld, slack=%lld)\n",
                           w, h, d.width, d.height,
                           (long long)diff, (long long)slack);
                }
            }
        }
    }
    CHECK_EQ_INT(violations, 0);
    END();
}

/* ---------- main ---------- */

int main(void)
{
    printf("Running upscale_logic tests...\n");

    test_decide_forced_720p();
    test_decide_forced_1080p();
    test_decide_auto_strong_hw();
    test_decide_auto_weak_hw();
    test_decide_auto_tiny_source();
    test_decide_invalid_input();
    test_decide_no_downscale();

    test_compute_basic_aspect();
    test_compute_4_3_aspect();
    test_compute_even_rounding();
    test_compute_invalid_input();
    test_compute_overflow_safe();

    test_plan_typical_sd_to_hd();
    test_plan_already_hd();
    test_plan_skip_above_lowered();
    test_plan_invalid();
    test_plan_anamorphic();
    test_plan_pathological_aspect_regression();
    test_aspect_invariant_smoke();

    printf("\n%d tests run, %d failed\n", g_tests_run, g_tests_failed);
    return g_tests_failed == 0 ? 0 : 1;
}
