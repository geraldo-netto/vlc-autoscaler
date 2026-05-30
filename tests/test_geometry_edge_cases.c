// SPDX-License-Identifier: GPL-2.0-or-later
#include "../src/upscale_logic.h"
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>

static void test_edge_dimensions(void)
{
    up_dims_t target;
    
    // REL-4: Test 1x1 source (extreme case)
    // Should either bypass or produce a valid minimum target
    bool ok = up_plan_upscale(1, 1, 720, 0, 8, 4096, &target);
    if (ok) {
        assert(target.width >= 1);
        assert(target.height >= 1);
        printf("  1x1 source -> %dx%d OK\n", target.width, target.height);
    } else {
        printf("  1x1 source bypassed OK\n");
    }

    // Test ultra-wide source (e.g., 1920x1)
    ok = up_plan_upscale(1920, 1, 720, 0, 8, 4096, &target);
    if (ok) {
        assert(target.height >= 1);
        printf("  1920x1 source -> %dx%d OK\n", target.width, target.height);
    }

    // Test ultra-tall source (e.g., 1x1080)
    ok = up_plan_upscale(1, 1080, 720, 0, 8, 4096, &target);
    // Height 1080 should typically be bypassed with skip-above=720
    if (!ok) {
        printf("  1x1080 source bypassed OK\n");
    }
}

static void test_zero_hardware(void)
{
    up_dims_t target;
    // Test with 0 cores/0 RAM (simulated detection failure)
    // Should fail gracefully or default to 720p baseline
    bool ok = up_plan_upscale(640, 480, 720, 0, 0, 0, &target);
    printf("  640x480 with 0 hardware info -> ok=%d target=%dx%d\n", ok, target.width, target.height);
}

int main(void)
{
    test_edge_dimensions();
    test_zero_hardware();
    return 0;
}