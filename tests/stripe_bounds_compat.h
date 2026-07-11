// SPDX-License-Identifier: GPL-2.0-or-later
/*****************************************************************************
 * stripe_bounds_compat.h — shared test-only compat shim (DUP-1)
 *****************************************************************************
 * up_compute_stripe_bounds() took a >7-parameter out-pointer list before the
 * Sonar refactor folded the four outputs into up_stripe_bounds_t. Several
 * tests/fuzzers predate that and were written against the flat call shape.
 * This shim preserves that shape in one place instead of the byte-identical
 * copy each of them used to carry.
 *****************************************************************************/
#ifndef TEST_STRIPE_BOUNDS_COMPAT_H
#define TEST_STRIPE_BOUNDS_COMPAT_H

#include "../src/plane_utils.h"

static inline int stripe_bounds4(int i, int n, int src_h, int dst_h,
                                 int *src_start, int *src_end,
                                 int *dst_start, int *dst_end)
{
    up_stripe_bounds_t b = { 0, 0, 0, 0 };
    int ok = up_compute_stripe_bounds(i, n, src_h, dst_h, &b);
    *src_start = b.src_start; *src_end = b.src_end;
    *dst_start = b.dst_start; *dst_end = b.dst_end;
    return ok;
}

#endif /* TEST_STRIPE_BOUNDS_COMPAT_H */
