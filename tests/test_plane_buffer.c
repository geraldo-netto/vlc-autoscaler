// SPDX-License-Identifier: GPL-2.0-or-later
/*****************************************************************************
 * test_plane_buffer.c - unit tests for the aligned plane-buffer seam
 *****************************************************************************/

#include "../src/plane_buffer.h"

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "test_harness.h"

static void test_alloc_bytes_guards(void)
{
    BEGIN("plane_alloc_bytes: non-positive dimensions yield 0");
    CHECK_EQ(plane_alloc_bytes(0, 64), (size_t)0);
    CHECK_EQ(plane_alloc_bytes(64, 0), (size_t)0);
    CHECK_EQ(plane_alloc_bytes(-1, 64), (size_t)0);
    CHECK_EQ(plane_alloc_bytes(64, -1), (size_t)0);
    CHECK_EQ(plane_alloc_bytes(INT_MIN, INT_MIN), (size_t)0);
    END();
}

static void test_alloc_bytes_products(void)
{
    BEGIN("plane_alloc_bytes: exact products for valid dimensions");
    CHECK_EQ(plane_alloc_bytes(1, 1), (size_t)1);
    CHECK_EQ(plane_alloc_bytes(1088, 1920), (size_t)1088 * 1920);
    CHECK_EQ(plane_alloc_bytes(INT_MAX, 1), (size_t)INT_MAX);
    CHECK_EQ(plane_alloc_bytes(INT_MAX, INT_MAX),
             (size_t)INT_MAX * (size_t)INT_MAX);
    END();
}

/* The layout must agree with the shared sizing helpers, not a private
 * re-derivation: Y is full-resolution, U/V use the subsample exponents. */
static void test_layout_matches_helpers(void)
{
    BEGIN("init_plane_layout: matches up_plane_pitch/lines per plane");
    plane_layout_t layout;
    init_plane_layout(&layout, 1920, 1080, 1, 1);   /* I420 */
    CHECK_EQ(layout.pitch[PLANE_Y], up_plane_pitch(1920, 0));
    CHECK_EQ(layout.lines[PLANE_Y], up_plane_lines(1080, 0));
    for (int p = PLANE_U; p < PLANE_COUNT; p++) {
        CHECK_EQ(layout.pitch[p], up_plane_pitch(1920, 1));
        CHECK_EQ(layout.lines[p], up_plane_lines(1080, 1));
    }
    init_plane_layout(&layout, 854, 480, 0, 0);     /* I444-style */
    CHECK_EQ(layout.pitch[PLANE_U], up_plane_pitch(854, 0));
    CHECK_EQ(layout.lines[PLANE_V], up_plane_lines(480, 0));
    END();
}

static void test_buffer_bytes_sums_planes(void)
{
    BEGIN("plane_buffer_bytes: sums the three per-plane extents");
    plane_buffer_t buffer = { 0 };
    init_plane_layout(&buffer.layout, 1920, 1080, 1, 1);
    size_t expected = 0;
    for (int p = 0; p < PLANE_COUNT; p++)
        expected += plane_alloc_bytes(buffer.layout.lines[p],
                                      buffer.layout.pitch[p]);
    CHECK_EQ(plane_buffer_bytes(&buffer), expected);

    plane_buffer_t empty = { 0 };
    CHECK_EQ(plane_buffer_bytes(&empty), (size_t)0);
    END();
}

static void test_alloc_roundtrip_aligned(void)
{
    BEGIN("alloc_plane_buffer: aligned planes, free nulls the pointers");
    plane_buffer_t buffer = { 0 };
    init_plane_layout(&buffer.layout, 640, 360, 1, 1);
    CHECK_EQ(alloc_plane_buffer(&buffer), 0);
    for (int p = 0; p < PLANE_COUNT; p++) {
        CHECK(buffer.data[p] != NULL);
        CHECK_EQ((uintptr_t)buffer.data[p] % UP_PITCH_ALIGN, (uintptr_t)0);
        /* Touch first and last byte: ASan verifies the extent is real. */
        buffer.data[p][0] = 0xAA;
        buffer.data[p][plane_alloc_bytes(buffer.layout.lines[p],
                                         buffer.layout.pitch[p]) - 1] = 0x55;
    }
    free_plane_buffer(&buffer);
    for (int p = 0; p < PLANE_COUNT; p++)
        CHECK(buffer.data[p] == NULL);
    free_plane_buffer(&buffer);   /* double free must be a no-op */
    END();
}

/* A zero-extent plane must fail the whole allocation; the partial prefix
 * stays owned by the caller and is released by free_plane_buffer (ASan
 * would report the leak otherwise). */
static void test_alloc_rejects_degenerate_layout(void)
{
    BEGIN("alloc_plane_buffer: zero-extent plane fails, prefix reclaimable");
    plane_buffer_t buffer = { 0 };
    init_plane_layout(&buffer.layout, 640, 360, 1, 1);
    buffer.layout.lines[PLANE_V] = 0;
    CHECK_EQ(alloc_plane_buffer(&buffer), -1);
    free_plane_buffer(&buffer);

    plane_buffer_t zero = { 0 };
    CHECK_EQ(alloc_plane_buffer(&zero), -1);
    CHECK(zero.data[PLANE_Y] == NULL);
    END();
}

static void test_view_mirrors_buffer(void)
{
    BEGIN("plane_buffer_view: data and pitch mirror the buffer");
    plane_buffer_t buffer = { 0 };
    init_plane_layout(&buffer.layout, 320, 180, 1, 1);
    CHECK_EQ(alloc_plane_buffer(&buffer), 0);
    const plane_view_t view = plane_buffer_view(&buffer);
    for (int p = 0; p < PLANE_COUNT; p++) {
        CHECK(view.data[p] == buffer.data[p]);
        CHECK_EQ(view.pitch[p], buffer.layout.pitch[p]);
    }
    free_plane_buffer(&buffer);
    END();
}

int main(void)
{
    printf("Running plane_buffer tests...\n");

    test_alloc_bytes_guards();
    test_alloc_bytes_products();
    test_layout_matches_helpers();
    test_buffer_bytes_sums_planes();
    test_alloc_roundtrip_aligned();
    test_alloc_rejects_degenerate_layout();
    test_view_mirrors_buffer();

    return test_harness_report();
}
