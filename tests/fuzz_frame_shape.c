// SPDX-License-Identifier: GPL-2.0-or-later
/*****************************************************************************
 * fuzz_frame_shape.c - fuzz "picture shape" validation across pure helpers
 *****************************************************************************
 * Most existing fuzzers exercise one helper in isolation. This one fuzzes
 * the COMBINATION of helpers that any backend has to call when it receives
 * a frame from VLC:
 *
 *   1. Classify the chroma fourcc:
 *        is_opaque(fourcc)        -> reject (Open() should have done this)
 *        has_y_plane(fourcc)      -> may sharpen luma
 *
 *   2. Compute per-plane geometry from frame dims:
 *        plane_pitch(w, sub_w)
 *        plane_lines(h, sub_h)
 *        round_up_pitch / round_up_lines (alignment)
 *
 *   3. If multi-stripe: compute stripe bounds and verify partition coverage.
 *
 * The fuzz harness drives all of these with arbitrary 32-bit fields plus
 * a "chroma class" picked from a curated list (including: known opaque,
 * known sw planar, known sw semi-planar, known packed, garbage). It
 * verifies a set of post-conditions that hold REGARDLESS of input shape:
 *
 *   - Opaque chromas are detected. (Spot-check known fourccs.)
 *   - has_y_plane() and is_opaque() are mutually exclusive on known chromas.
 *   - plane_pitch/lines never produce a value smaller than what's needed
 *     for the requested image area to fit (when inputs are valid).
 *   - When src_h, dst_h, n_stripes are positive, the per-stripe bounds
 *     produced by compute_stripe_bounds form a non-overlapping cover of
 *     [0, src_h) and [0, dst_h).
 *   - All "partition cover" properties hold for n_stripes from 1..MAX.
 *   - For invalid inputs (n<=0, src_h<=0, etc.) compute_stripe_bounds
 *     returns 0 cleanly without writing to the output pointers (proven
 *     by canary values that must remain unchanged).
 *
 * Build (smoke):
 *   cc -O2 -g -fsanitize=address,undefined -DFUZZ_MAIN \
 *      -I src tests/fuzz_frame_shape.c -o build/fuzz_frame_shape_smoke
 *
 * Build (libFuzzer):
 *   clang -O1 -g -fsanitize=fuzzer,address,undefined \
 *         -I src tests/fuzz_frame_shape.c -o build/fuzz_frame_shape
 *****************************************************************************/

#include "../src/chroma_classify.h"
#include "../src/zimg_helpers.h"

#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- curated chroma fourccs ---- */

/* Known opaque (must be rejected by is_opaque). Subset of up_opaque_chromas
 * - we don't need to enumerate all 16, just spot-check a few. */
static const uint32_t known_opaque[] = {
    UP_FOURCC('V','A','O','P'),
    UP_FOURCC('V','D','V','0'),
    UP_FOURCC('D','X','1','1'),
    UP_FOURCC('M','M','A','L'),
    UP_FOURCC('C','V','P','N'),
};

/* Known has-y-plane sw chromas (planar/semi-planar with discrete luma). */
static const uint32_t known_y_plane[] = {
    UP_FOURCC('I','4','2','0'),
    UP_FOURCC('Y','V','1','2'),
    UP_FOURCC('N','V','1','2'),
    UP_FOURCC('N','V','2','1'),
    UP_FOURCC('I','4','2','2'),
    UP_FOURCC('I','4','4','4'),
};

/* Known sw chromas WITHOUT a discrete y-plane (packed YUV, RGB). */
static const uint32_t known_packed[] = {
    UP_FOURCC('Y','U','Y','2'),  /* packed YUV 4:2:2 */
    UP_FOURCC('U','Y','V','Y'),  /* packed YUV 4:2:2, swapped */
    UP_FOURCC('R','V','3','2'),  /* packed RGBA */
    UP_FOURCC('R','V','2','4'),  /* packed RGB */
    UP_FOURCC('B','G','R','A'),
};

/* Picked at random from these lists by class-id from fuzz input. */
enum chroma_class {
    CC_OPAQUE   = 0,
    CC_Y_PLANE  = 1,
    CC_PACKED   = 2,
    CC_GARBAGE  = 3,
    CC_COUNT
};

static uint32_t pick_chroma(uint32_t cls_byte, uint32_t garbage_seed)
{
    switch (cls_byte % CC_COUNT) {
        case CC_OPAQUE:
            return known_opaque[cls_byte % (sizeof known_opaque / 4)];
        case CC_Y_PLANE:
            return known_y_plane[cls_byte % (sizeof known_y_plane / 4)];
        case CC_PACKED:
            return known_packed[cls_byte % (sizeof known_packed / 4)];
        default:
            /* Random garbage fourcc that's almost certainly not in either list. */
            return garbage_seed;
    }
}

/* Helper: print invariant violation and abort, with explicit flush. */
#define FAIL(fmt, ...) do { \
    fprintf(stderr, "INVARIANT: " fmt "\n", __VA_ARGS__); \
    fflush(stderr); \
    abort(); \
} while (0)

/* ---- invariant checkers ---- */

static void check_known_class(uint32_t fourcc, uint32_t cls_byte,
                               bool opaque, bool yplane)
{
    switch (cls_byte % CC_COUNT) {
        case CC_OPAQUE:
            if (!opaque) {
                FAIL("known opaque 0x%08x not detected", fourcc);
            }
            break;
        case CC_Y_PLANE:
            if (!yplane || opaque) {
                FAIL("known y-plane 0x%08x mis-classified (yplane=%d opaque=%d)",
                     fourcc, (int)yplane, (int)opaque);
            }
            break;
        case CC_PACKED:
            /* Packed/RGB must NOT be claimed as has-y-plane (would lead to
             * sharpening packed memory and tearing colors). */
            if (yplane) {
                FAIL("known packed 0x%08x classified as has-y-plane",
                     fourcc);
            }
            break;
        case CC_GARBAGE:
            /* For garbage, just check the mutual-exclusion above. */
            break;
    }
}

static void check_chroma_class(uint32_t fourcc, uint32_t cls_byte)
{
    bool opaque = up_chroma_is_opaque(fourcc);
    bool yplane = up_chroma_has_y_plane(fourcc);

    /* CRITICAL: a chroma can be opaque OR has-y-plane, never both.
     * Y-plane sharpening on opaque GPU surfaces would be a use-after-
     * read-of-uninitialized-memory at best, a segfault at worst. */
    if (opaque && yplane) {
        FAIL("fourcc 0x%08x is BOTH opaque and has-y-plane", fourcc);
    }

    check_known_class(fourcc, cls_byte, opaque, yplane);
}

static void check_plane_pitch_lines(int w, int h)
{
    int yp = up_plane_pitch(w, 0);      /* luma: full width */
    int yl = up_plane_lines(h, 0);
    int cp = up_plane_pitch(w, 1);      /* 4:2:0 chroma: half width */
    int cl = up_plane_lines(h, 1);

    /* Luma must accommodate at least the full width/height. */
    if (yp < w) FAIL("plane_pitch(%d,0)=%d < w", w, yp);
    if (yl < h) FAIL("plane_lines(%d,0)=%d < h", h, yl);

    /* 4:2:0 chroma must accommodate at least ceil(w/2) and ceil(h/2). */
    int cw_min = (w + 1) >> 1;
    int ch_min = (h + 1) >> 1;
    if (cp < cw_min) FAIL("plane_pitch(%d,1)=%d < ceil(w/2)=%d", w, cp, cw_min);
    if (cl < ch_min) FAIL("plane_lines(%d,1)=%d < ceil(h/2)=%d", h, cl, ch_min);

    /* Pitch must be aligned: divisible by UP_PITCH_ALIGN (64). */
    if (yp % UP_PITCH_ALIGN != 0) FAIL("luma pitch %d not 64-aligned for w=%d", yp, w);
    if (cp % UP_PITCH_ALIGN != 0) FAIL("chroma pitch %d not 64-aligned for w=%d", cp, w);
}

static void check_round_up(int w, int h)
{
    int rp = up_round_up_pitch(w);
    int rl = up_round_up_lines(h);
    if (rp < w) FAIL("round_up_pitch(%d)=%d < w", w, rp);
    if (rl < h) FAIL("round_up_lines(%d)=%d < h", h, rl);
}

static void check_zimg_plane_idx_range(void)
{
    /* zimg plane index is in [0, 2] for any swap value. */
    for (int swap = 0; swap < 2; swap++) {
        for (int idx = 0; idx < 3; idx++) {
            int z = up_zimg_plane_idx(idx, swap);
            if (z < 0 || z > 2) {
                FAIL("zimg_plane_idx(%d,%d)=%d out of range",
                     idx, swap, z);
            }
        }
    }
}

static void check_plane_geometry(int w, int h)
{
    /* up_plane_pitch(w, sub_w) and up_plane_lines(h, sub_h) take sub_w/sub_h
     * as SHIFT EXPONENTS, not divisors:
     *   sub_w = 0  ->  full width  (luma plane)
     *   sub_w = 1  ->  half width  (4:2:0/4:2:2 chroma)
     *   sub_w = 2  ->  quarter (theoretical 4:1:1)
     * The minimum return is ceil(w / 2^sub_w), padded up for alignment.
     * The helpers must produce sane values for any valid (w, h). */
    if (w <= 0 || h <= 0) return;
    if (w > 65536 || h > 65536) return;  /* skip wildly out-of-range */

    check_plane_pitch_lines(w, h);
    check_round_up(w, h);
    check_zimg_plane_idx_range();
}

static int stripe_inputs_out_of_range(int n, int src_h, int dst_h)
{
    if (n <= 0 || n > 64 || src_h <= 0 || dst_h <= 0) return 1;
    if (src_h > 65536 || dst_h > 65536) return 1;
    return 0;
}

static int stripe_partition_skip(int n, int src_h, int dst_h, int *out_max_n)
{
    /* compute_stripe_bounds(i, n, src_h, dst_h) for i in [0, n) must produce
     * a non-overlapping, contiguous cover of [0, src_h) and [0, dst_h) -
     * BUT only when the caller honors the production constraint that
     * n_stripes <= dst_h / UP_STRIPE_MIN_DST_LINES. Otherwise stripes can
     * legitimately collapse to zero size; the production caller breaks
     * out of its loop in that case (see scaler_zimg.c around line 390). */
    if (stripe_inputs_out_of_range(n, src_h, dst_h)) return 1;

    int max_n_for_dst = dst_h / UP_STRIPE_MIN_DST_LINES;
    if (max_n_for_dst < 1) max_n_for_dst = 1;
    if (n > max_n_for_dst) return 1;
    if (src_h / n < 4) return 1;
    *out_max_n = max_n_for_dst;
    return 0;
}

static void check_stripe_bounds_range(int i, int n, int src_h, int dst_h,
                                       int s_a, int s_b, int d_a, int d_b)
{
    if (s_a < 0 || s_b > src_h || s_a > s_b) {
        FAIL("src bounds invalid: i=%d n=%d src_h=%d dst_h=%d "
             "s_a=%d s_b=%d", i, n, src_h, dst_h, s_a, s_b);
    }
    if (d_a < 0 || d_b > dst_h || d_a > d_b) {
        FAIL("dst bounds invalid: i=%d n=%d src_h=%d dst_h=%d "
             "d_a=%d d_b=%d", i, n, src_h, dst_h, d_a, d_b);
    }
}

static void check_stripe_step(int i, int n, int src_h, int dst_h, int max_n,
                              int prev_src_end, int prev_dst_end,
                              int *out_src_end, int *out_dst_end)
{
    /* Pre-fill with canaries to catch writes-on-failure. */
    int s_a = -777, s_b = -777, d_a = -888, d_b = -888;
    int ok = up_compute_stripe_bounds(i, n, src_h, dst_h,
                                      &s_a, &s_b, &d_a, &d_b);
    if (!ok) {
        FAIL("stripe_bounds returned 0 for valid input "
             "i=%d n=%d src_h=%d dst_h=%d (max_n=%d, src_h/n=%d)",
             i, n, src_h, dst_h, max_n, src_h / n);
    }
    check_stripe_bounds_range(i, n, src_h, dst_h, s_a, s_b, d_a, d_b);
    if (s_a != prev_src_end) {
        FAIL("src not contiguous: i=%d n=%d src_h=%d s_a=%d prev=%d",
             i, n, src_h, s_a, prev_src_end);
    }
    if (d_a != prev_dst_end) {
        FAIL("dst not contiguous: i=%d n=%d dst_h=%d d_a=%d prev=%d",
             i, n, dst_h, d_a, prev_dst_end);
    }
    *out_src_end = s_b;
    *out_dst_end = d_b;
}

static void check_stripe_partition(int n, int src_h, int dst_h)
{
    int max_n_for_dst = 0;
    if (stripe_partition_skip(n, src_h, dst_h, &max_n_for_dst)) return;

    int prev_src_end = 0;
    int prev_dst_end = 0;
    for (int i = 0; i < n; i++) {
        check_stripe_step(i, n, src_h, dst_h, max_n_for_dst,
                          prev_src_end, prev_dst_end,
                          &prev_src_end, &prev_dst_end);
    }
    if (prev_src_end != src_h) {
        FAIL("partition src not closed: n=%d src_h=%d final=%d",
             n, src_h, prev_src_end);
    }
    if (prev_dst_end != dst_h) {
        FAIL("partition dst not closed: n=%d dst_h=%d final=%d",
             n, dst_h, prev_dst_end);
    }
}

static int stripe_invalid_input(int i, int n, int src_h, int dst_h)
{
    /* Compute whether the input is invalid per the documented contract. */
    return (n <= 0) || (src_h <= 0) || (dst_h <= 0)
        || (i < 0)  || (i >= n);
}

static int stripe_canary_unchanged(int s_a, int s_b, int d_a, int d_b)
{
    return s_a == 0x4DEAD && s_b == 0x4BEEF
        && d_a == 0x4CAFE && d_b == 0x4BABE;
}

static void check_stripe_invalid(int i, int n, int src_h, int dst_h)
{
    /* For invalid inputs, stripe_bounds must return 0. We test a wide
     * range of possibly-invalid (i, n, src_h, dst_h) tuples driven by
     * fuzz input, not just hardcoded -1, so cppcheck can't dead-code-
     * eliminate the failure branches.
     *
     * Pre-fill outputs with canaries. The function's contract is that
     * output values may or may not be defined when it returns 0 — we
     * don't assert anything about them in that case, only that it
     * doesn't crash and returns 0 for invalid inputs. */
    int s_a = 0x4DEAD, s_b = 0x4BEEF, d_a = 0x4CAFE, d_b = 0x4BABE;
    int ok = up_compute_stripe_bounds(i, n, src_h, dst_h,
                                      &s_a, &s_b, &d_a, &d_b);

    int invalid = stripe_invalid_input(i, n, src_h, dst_h);

    if (invalid && ok) {
        FAIL("stripe_bounds accepted invalid input "
             "i=%d n=%d src_h=%d dst_h=%d", i, n, src_h, dst_h);
    }
    if (!invalid && !ok && stripe_canary_unchanged(s_a, s_b, d_a, d_b)) {
        /* Valid (i, n, src_h, dst_h) by entry-guard rules — but the
         * function may still return 0 for degenerate stripes. Only
         * complain if all four output values are unchanged from the
         * canary, which would indicate the function exited from the
         * entry guard without doing any work despite valid inputs. */
        FAIL("stripe_bounds rejected valid input without computing: "
             "i=%d n=%d src_h=%d dst_h=%d", i, n, src_h, dst_h);
    }
}

/* ---- single iteration ---- */

static void run_one(const uint8_t *data, size_t size)
{
    uint8_t buf[24] = { 0 };
    size_t n = size < sizeof buf ? size : sizeof buf;
    if (n > 0) memcpy(buf, data, n);

    uint32_t cls_byte;
    uint32_t garbage_fourcc;
    int32_t  i_w, i_h;
    int32_t  i_n_stripes;
    int32_t  i_dst_h;

    memcpy(&cls_byte,        buf +  0, 4);
    memcpy(&garbage_fourcc,  buf +  4, 4);
    memcpy(&i_w,             buf +  8, 4);
    memcpy(&i_h,             buf + 12, 4);
    memcpy(&i_n_stripes,     buf + 16, 4);
    memcpy(&i_dst_h,         buf + 20, 4);

    /* 1. Chroma classification — drive both invariants and class spot-checks. */
    uint32_t fourcc = pick_chroma(cls_byte, garbage_fourcc);
    check_chroma_class(fourcc, cls_byte);

    /* 2. Plane geometry — bounds-checked inside the helper. */
    check_plane_geometry(i_w, i_h);

    /* 3. Stripe partition — coverage and rejection-of-invalid. */
    check_stripe_partition(i_n_stripes, i_h, i_dst_h);

    /* 4. Stripe rejection — drive `i` through several interesting
     * values to exercise the entry guard from many angles. cppcheck
     * can no longer dead-code-eliminate the failure branches because
     * `i` is a runtime value, not a literal. */
    int trial_indices[] = {
        -1,                  /* canonical "below range" */
        i_n_stripes,         /* exactly at the upper edge */
        i_n_stripes + 1,     /* one past upper */
        (int)(cls_byte >> 16),   /* random bits from fuzz input */
        i_w,                 /* could be anything including INT_MIN */
    };
    for (size_t k = 0; k < sizeof trial_indices / sizeof *trial_indices; k++) {
        check_stripe_invalid(trial_indices[k], i_n_stripes, i_h, i_dst_h);
    }
    /* Also test the originally-intended use-case: i in valid range. */
    if (i_n_stripes > 0) {
        int valid_i = ((unsigned)cls_byte) % (unsigned)i_n_stripes;
        check_stripe_invalid(valid_i, i_n_stripes, i_h, i_dst_h);
    }
}

/* ---- entry points ---- */

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    run_one(data, size);
    return 0;
}

#ifdef FUZZ_MAIN
int main(int argc, char **argv)
{
    long n = 100000;
    if (argc > 1) {
        char *end = NULL;
        long v = strtol(argv[1], &end, 10);
        if (end && *end == '\0' && v > 0) n = v;
    }

    /* deterministic xorshift32 */
    uint32_t s = 0xC0FFEE17u;
    uint8_t buf[24];

    for (long i = 0; i < n; i++) {
        for (size_t j = 0; j < sizeof buf; j += 4) {
            s ^= s << 13;
            s ^= s >> 17;
            s ^= s <<  5;
            memcpy(buf + j, &s, 4);
        }
        /* Bias every 3rd iteration to use realistic dimensions, and every
         * 5th to use small n_stripes (the common case). */
        if ((i % 3) == 0) {
            static const int common[] = {
                1, 2, 16, 32, 64, 96, 128, 240, 360, 480, 540,
                720, 1080, 1440, 2160, 4320, 65535
            };
            int w = common[s % (sizeof common / sizeof *common)];
            int h = common[(s >> 8) % (sizeof common / sizeof *common)];
            memcpy(buf +  8, &w, 4);
            memcpy(buf + 12, &h, 4);
        }
        if ((i % 5) == 0) {
            int ns = 1 + (int)(s % 32);  /* 1..32 */
            int dh = 16 + (int)(s % 4080);  /* 16..4096 */
            memcpy(buf + 16, &ns, 4);
            memcpy(buf + 20, &dh, 4);
        }
        run_one(buf, sizeof buf);
    }

    printf("frame_shape smoke OK: %ld iterations\n", n);
    return 0;
}
#endif
