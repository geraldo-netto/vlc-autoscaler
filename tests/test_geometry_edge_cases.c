// SPDX-License-Identifier: GPL-2.0-or-later
/* Edge-geometry contract checks for up_plan_upscale (REL-12). Every case
 * asserts the exact documented outcome: cores/mem are parameters, so the
 * results are deterministic. */
#include "../src/upscale_logic.h"
#include <stdio.h>

static int g_fail = 0;
#define CHECK(cond) do { \
        if (!(cond)) { \
            printf("    %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            g_fail = 1; \
        } \
    } while (0)

static void test_edge_dimensions(void)
{
    up_dims_t target;

    /* 1x1 source: AUTO wants 720p but the UP_MAX_RATIO cap clamps the
     * target to 4x, so the plan is exactly 4x4 (even, never downscaled). */
    CHECK(up_plan_upscale(1, 1, 720, 0, 8, 4096, &target) == 1);
    CHECK(target.width == UP_MAX_RATIO && target.height == UP_MAX_RATIO);

    /* 1920x1: height is ratio-capped to 4, width follows the aspect. */
    CHECK(up_plan_upscale(1920, 1, 720, 0, 8, 4096, &target) == 1);
    CHECK(target.height == UP_MAX_RATIO);
    CHECK(target.width == 1920 * UP_MAX_RATIO);

    /* 1x1080 with skip-above=720 in AUTO: must bypass and zero the out. */
    CHECK(up_plan_upscale(1, 1080, 720, 0, 8, 4096, &target) == 0);
    CHECK(target.width == 0 && target.height == 0);

    /* skip-above applies to AUTO only: an explicit 4K preset upscales a
     * 1080p source even though src_h >= skip_above. */
    CHECK(up_plan_upscale(1920, 1080, 720, UP_TARGET_4K, 8, 4096,
                          &target) == 1);
    CHECK(target.width == 3840 && target.height == 2160);
}

static void test_zero_hardware(void)
{
    up_dims_t target;

    /* 0 cores / 0 MB (detection failure): cores clamp to 1, mem 0 means
     * "unknown -> sufficient", AUTO stays at the 720p baseline. */
    CHECK(up_plan_upscale(640, 480, 720, 0, 0, 0, &target) == 1);
    CHECK(target.width == 960 && target.height == 720);

    /* Same failure mode must never unlock the 1080p AUTO tier. */
    CHECK(up_plan_upscale(1280, 640, 720, 0, 0, 0, &target) == 1);
    CHECK(target.height == 720);
}

int main(void)
{
    printf("Running geometry edge-case tests...\n");
    test_edge_dimensions();
    test_zero_hardware();
    printf(g_fail ? "FAILED\n" : "OK\n");
    return g_fail;
}
