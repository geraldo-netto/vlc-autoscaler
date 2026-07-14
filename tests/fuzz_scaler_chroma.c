// SPDX-License-Identifier: GPL-2.0-or-later
/*****************************************************************************
 * fuzz_scaler_chroma.c - fuzz the chroma->zimg mapping pulled from the
 *                       scaler backend's VLC interface boundary.
 *****************************************************************************
 * Many VLC-touching helpers are too tangled with `picture_t` /
 * `filter_t` / `var_InheritInteger` to fuzz without an absurd mocking
 * effort. But the descriptor-to-zimg adapter is pure: it takes a 32-bit
 * fourcc and writes (sub_w, sub_h, yv12_swap) into caller-provided ints,
 * returning 1 on success or 0 on rejection.
 *
 * That makes it a perfect fuzz target. We pulled it out into
 * src/scaler_zimg_chroma.h so the production path and this fuzzer
 * exercise the SAME function. No VLC headers needed.
 *
 * What's checked:
 *
 *   - Known-supported chromas (I420/YV12/I422/I444) map to the
 *     correct (sub_w, sub_h, yv12_swap). Documents the supported
 *     contract.
 *
 *   - Known-unsupported chromas (NV12/NV21/RGB/packed/opaque) are
 *     rejected without changing the outputs. Catches regressions where
 *     the shared descriptor admits a layout zimg cannot consume — that
 *     would crash at runtime on the first semi-planar frame.
 *
 *   - Random fourccs from xorshift never produce inconsistent state:
 *     rc=0 leaves every output untouched; rc=1 sets them all to
 *     documented values.
 *
 *   - NULL output pointers are rejected without crashing.
 *
 *   - Cross-property: every chroma that up_chroma_to_zimg accepts MUST
 *     be a has-y-plane chroma per chroma_classify.h. (If zimg can
 *     scale it, it must have a Y plane that USM can sharpen.) The
 *     reverse isn't true — NV12 has a Y plane but isn't accepted by
 *     zimg. So this is a strict subset relation.
 *
 *   - Cross-property: NO chroma accepted by up_chroma_to_zimg is
 *     opaque. Catches a regression where an opaque hwaccel surface
 *     descriptor accidentally became zimg-compatible.
 *
 * Build (smoke):
 *   cc -O2 -g -fsanitize=address,undefined -DFUZZ_MAIN \
 *      -I src tests/fuzz_scaler_chroma.c -o build/fuzz_scaler_chroma_smoke
 *
 * Build (libFuzzer):
 *   clang -O1 -g -fsanitize=fuzzer,address,undefined \
 *         -I src tests/fuzz_scaler_chroma.c -o build/fuzz_scaler_chroma
 *****************************************************************************/

#include "../src/scaler_zimg_chroma.h"
#include "../src/chroma_classify.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FAIL(fmt, ...) do { \
    fprintf(stderr, "INVARIANT: " fmt "\n", __VA_ARGS__); \
    fflush(stderr); \
    abort(); \
} while (0)

/* ---- known-supported and known-unsupported chromas with expected outputs ---- */

typedef struct {
    uint32_t fourcc;
    unsigned sub_w;
    unsigned sub_h;
    int yv12_swap;
} expected_zimg_t;

static const expected_zimg_t known_supported[] = {
    { UP_FOURCC('I','4','2','0'), 1, 1, 0 },
    { UP_FOURCC('Y','V','1','2'), 1, 1, 1 },
    { UP_FOURCC('I','4','2','2'), 1, 0, 0 },
    { UP_FOURCC('I','4','4','4'), 0, 0, 0 },
};
#define N_SUPPORTED (sizeof known_supported / sizeof *known_supported)

/* zimg explicitly rejects NV12/NV21 (semi-planar — interleaved UV plane).
 * The swscale backend handles those instead. Also reject packed YUV, RGB,
 * and any opaque hwaccel surface. */
static const uint32_t known_unsupported[] = {
    UP_FOURCC('N','V','1','2'),
    UP_FOURCC('N','V','2','1'),
    UP_FOURCC('Y','U','Y','2'),  /* packed YUV */
    UP_FOURCC('U','Y','V','Y'),
    UP_FOURCC('R','V','3','2'),  /* packed RGBA */
    UP_FOURCC('R','V','2','4'),
    UP_FOURCC('B','G','R','A'),
    UP_FOURCC('V','A','O','P'),  /* opaque VAAPI */
    UP_FOURCC('V','D','V','0'),  /* opaque VDPAU */
    UP_FOURCC('D','X','1','1'),  /* opaque D3D11 */
};
#define N_UNSUPPORTED (sizeof known_unsupported / sizeof *known_unsupported)

/* ---- invariant checks ---- */

static bool outputs_untouched(unsigned sw, unsigned sh, int swap)
{
    return sw == 0xDEAD && sh == 0xBEEF && swap == 0xCAFE;
}

static void check_known_supported(void)
{
    for (size_t i = 0; i < N_SUPPORTED; i++) {
        const expected_zimg_t *e = &known_supported[i];
        unsigned sw = 99, sh = 99;
        int swap = 99;
        int rc = up_chroma_to_zimg(e->fourcc, &sw, &sh, &swap);
        if (rc != 1) {
            FAIL("known supported 0x%08x rejected (rc=%d)", e->fourcc, rc);
        }
        if (sw != e->sub_w) {
            FAIL("0x%08x sub_w=%u expected %u", e->fourcc, sw, e->sub_w);
        }
        if (sh != e->sub_h) {
            FAIL("0x%08x sub_h=%u expected %u", e->fourcc, sh, e->sub_h);
        }
        if (swap != e->yv12_swap) {
            FAIL("0x%08x yv12_swap=%d expected %d",
                 e->fourcc, swap, e->yv12_swap);
        }
    }
}

static void check_known_unsupported(void)
{
    for (size_t i = 0; i < N_UNSUPPORTED; i++) {
        unsigned sw = 0xDEAD, sh = 0xBEEF;
        int swap = 0xCAFE;
        int rc = up_chroma_to_zimg(known_unsupported[i], &sw, &sh, &swap);
        if (rc != 0) {
            FAIL("known unsupported 0x%08x accepted (rc=%d)",
                 known_unsupported[i], rc);
        }
        if (!outputs_untouched(sw, sh, swap)) {
            FAIL("known unsupported 0x%08x changed outputs (rc=%d)",
                 known_unsupported[i], rc);
        }
    }
}

static void check_null_outputs(uint32_t fourcc, uint32_t mask)
{
    /* NULL output pointers must be rejected without crashing.
     * The function returns 0 immediately. We choose which pointer is
     * NULL based on a fuzz-input mask so cppcheck can't dead-code-
     * eliminate the failure branches by inlining a literal NULL. */
    unsigned sw, sh;
    int swap;

    /* Pick a non-NULL pointer for the others, NULL for the chosen one. */
    unsigned *p_sw   = (mask & 1u) ? NULL : &sw;
    unsigned *p_sh   = (mask & 2u) ? NULL : &sh;
    int      *p_swap = (mask & 4u) ? NULL : &swap;

    /* Only check when at least one IS NULL — otherwise this is the same
     * call as check_random_fourcc (no NULL to test). */
    if (p_sw == NULL || p_sh == NULL || p_swap == NULL) {
        int rc = up_chroma_to_zimg(fourcc, p_sw, p_sh, p_swap);
        if (rc != 0) {
            FAIL("NULL output pointer accepted (mask=%u, rc=%d, fourcc=0x%08x)",
                 mask & 7u, rc, fourcc);
        }
    }
}

static void check_cross_properties(uint32_t fourcc)
{
    /* If up_chroma_to_zimg accepts the chroma, it must:
     *   - have a Y plane (USM compatibility)
     *   - NOT be opaque (otherwise we'd crash trying to read GPU memory) */
    unsigned sw, sh;
    int swap;
    int rc = up_chroma_to_zimg(fourcc, &sw, &sh, &swap);
    if (rc == 1) {
        if (!up_chroma_has_y_plane(fourcc)) {
            FAIL("chroma 0x%08x accepted by zimg but has no Y plane",
                 fourcc);
        }
        if (up_chroma_is_opaque(fourcc)) {
            FAIL("chroma 0x%08x accepted by zimg but is opaque", fourcc);
        }
        /* sub_w and sub_h must be in [0, 1] for any 4:2:x or 4:4:4 format. */
        if (sw > 1) FAIL("0x%08x sub_w=%u out of expected range", fourcc, sw);
        if (sh > 1) FAIL("0x%08x sub_h=%u out of expected range", fourcc, sh);
        /* yv12_swap is a boolean. */
        if (swap != 0 && swap != 1) {
            FAIL("0x%08x yv12_swap=%d not a boolean", fourcc, swap);
        }
    }
}

static void check_random_fourcc(uint32_t fourcc)
{
    /* For any fourcc, the function must not crash and must return 0 or 1.
     * Pre-fill outputs with sentinels to verify all-or-nothing writes. */
    unsigned sw = 0xDEAD;
    unsigned sh = 0xBEEF;
    int swap = 0xCAFE;
    int rc = up_chroma_to_zimg(fourcc, &sw, &sh, &swap);
    if (rc != 0 && rc != 1) {
        FAIL("0x%08x produced invalid rc=%d", fourcc, rc);
    }
    if (rc == 0 && !outputs_untouched(sw, sh, swap)) {
        FAIL("0x%08x rejected but outputs were written (rc=%d)", fourcc, rc);
    }
    /* If rc=1, all three outputs must have been written. We can detect a
     * "wrote some but not all" bug because the sentinels are distinct. */
    if (rc == 1) {
        if (sw == 0xDEAD) FAIL("0x%08x rc=1 but sub_w unwritten", fourcc);
        if (sh == 0xBEEF) FAIL("0x%08x rc=1 but sub_h unwritten", fourcc);
        if (swap == 0xCAFE) FAIL("0x%08x rc=1 but swap unwritten", fourcc);
    }
}

/* Both adapters derive their shifts from the shared descriptor, so any chroma
 * zimg accepts must report the same (sub_w, sub_h) through the generic adapter.
 * The generic adapter additionally covers the semi-planar 4:2:0 pair. */
static void check_zimg_agrees_with_subsample(uint32_t fourcc)
{
    unsigned zw, zh, gw = 0, gh = 0;
    int swap;
    if (up_chroma_to_zimg(fourcc, &zw, &zh, &swap) != 1)
        return;
    if (!up_chroma_subsample(fourcc, &gw, &gh))
        FAIL("0x%08x accepted by zimg but has no subsample entry", fourcc);
    if (zw != gw || zh != gh)
        FAIL("0x%08x subsample drift: zimg (%u,%u) vs generic (%u,%u)",
             fourcc, zw, zh, gw, gh);
}

static void check_subsample_contract(uint32_t fourcc)
{
    unsigned sw = 0xDEAD, sh = 0xBEEF;
    if (!up_chroma_subsample(fourcc, &sw, &sh)) {
        /* Rejection must leave the outputs untouched. */
        if (sw != 0xDEAD || sh != 0xBEEF)
            FAIL("0x%08x rejected but outputs were written", fourcc);
        return;
    }
    if (sw > 1) FAIL("0x%08x sub_w=%u out of range", fourcc, sw);
    if (sh > 1) FAIL("0x%08x sub_h=%u out of range", fourcc, sh);
    /* Subsampling is only defined for chromas with a discrete Y plane. */
    if (!up_chroma_has_y_plane(fourcc))
        FAIL("0x%08x has a subsample entry but no Y plane", fourcc);
}

/* Indexed by axis so every invariant is checked one axis at a time — the
 * per-axis helpers stay well inside the project's CCN limit. */
enum { AXIS_X = 0, AXIS_Y = 1, AXIS_COUNT = 2 };

typedef struct {
    int dim[AXIS_COUNT];       /* width, height */
    unsigned off[AXIS_COUNT];  /* x_offset, y_offset */
} crop_window_t;

static crop_window_t crop_window_from(const uint8_t *data, size_t size)
{
    crop_window_t win = { { 1, 1 }, { 0, 0 } };
    if (size >= 12) {
        win.dim[AXIS_X] = (int)(data[5] | ((unsigned)data[6] << 8)) + 1;
        win.dim[AXIS_Y] = (int)(data[7] | ((unsigned)data[8] << 8)) + 1;
        win.off[AXIS_X] = data[9] | ((unsigned)data[10] << 8);
        win.off[AXIS_Y] = data[11];
    }
    return win;
}

static crop_window_t crop_align(uint32_t fourcc, crop_window_t win)
{
    up_chroma_align_crop_even(fourcc, &win.dim[AXIS_X], &win.dim[AXIS_Y],
                              &win.off[AXIS_X], &win.off[AXIS_Y]);
    return win;
}

static bool crop_equal(crop_window_t a, crop_window_t b)
{
    return a.dim[AXIS_X] == b.dim[AXIS_X] && a.dim[AXIS_Y] == b.dim[AXIS_Y]
        && a.off[AXIS_X] == b.off[AXIS_X] && a.off[AXIS_Y] == b.off[AXIS_Y];
}

/* Alignment may only shrink an axis, and by at most one pel — that is what
 * keeps offset+dim inside the coded plane. */
static void check_axis_shrinks(uint32_t fourcc, int a, crop_window_t before,
                               crop_window_t after)
{
    if (after.dim[a] > before.dim[a] || after.off[a] > before.off[a])
        FAIL("0x%08x axis %d align grew the window (%d/%u -> %d/%u)",
             fourcc, a, before.dim[a], before.off[a],
             after.dim[a], after.off[a]);
    if (after.dim[a] < 0)
        FAIL("0x%08x axis %d align produced a negative dim (%d)",
             fourcc, a, after.dim[a]);
    if (before.dim[a] - after.dim[a] > 1 || before.off[a] - after.off[a] > 1)
        FAIL("0x%08x axis %d align shifted by more than one pel", fourcc, a);
}

static void check_axis_parity(uint32_t fourcc, int a, unsigned sub,
                              crop_window_t after)
{
    if (!sub)
        return;
    if ((after.dim[a] & 1) || (after.off[a] & 1u))
        FAIL("0x%08x axis %d is subsampled but dim=%d off=%u still odd",
             fourcc, a, after.dim[a], after.off[a]);
}

static void check_align_noop_when_unsupported(uint32_t fourcc,
                                              crop_window_t before,
                                              crop_window_t after)
{
    unsigned sub[AXIS_COUNT];
    if (up_chroma_subsample(fourcc, &sub[AXIS_X], &sub[AXIS_Y]))
        return;
    if (!crop_equal(before, after))
        FAIL("0x%08x has no subsample entry but the window changed", fourcc);
}

/* Each pointer is independently optional — the two production call sites pass
 * dims only (Open) and offsets only (ConfigureScaler). */
static void check_align_partial_pointers(uint32_t fourcc, crop_window_t before,
                                         crop_window_t after)
{
    int w_only = before.dim[AXIS_X];
    unsigned y_only = before.off[AXIS_Y];
    up_chroma_align_crop_even(fourcc, &w_only, NULL, NULL, NULL);
    up_chroma_align_crop_even(fourcc, NULL, NULL, NULL, &y_only);
    up_chroma_align_crop_even(fourcc, NULL, NULL, NULL, NULL);
    if (w_only != after.dim[AXIS_X] || y_only != after.off[AXIS_Y])
        FAIL("0x%08x partial-pointer align disagrees with the full call",
             fourcc);
}

static void check_crop_align(uint32_t fourcc, const uint8_t *data, size_t size)
{
    const crop_window_t before = crop_window_from(data, size);
    const crop_window_t after = crop_align(fourcc, before);

    unsigned sub[AXIS_COUNT] = { 0, 0 };
    const bool subsampled = up_chroma_subsample(fourcc, &sub[AXIS_X],
                                                &sub[AXIS_Y]);
    for (int a = 0; a < AXIS_COUNT; a++) {
        check_axis_shrinks(fourcc, a, before, after);
        if (subsampled)
            check_axis_parity(fourcc, a, sub[a], after);
    }
    check_align_noop_when_unsupported(fourcc, before, after);
    check_align_partial_pointers(fourcc, before, after);

    if (!crop_equal(crop_align(fourcc, after), after))
        FAIL("0x%08x align is not idempotent", fourcc);
}

/* ---- single iteration ---- */

static void run_one(const uint8_t *data, size_t size)
{
    /* Every iteration runs the deterministic known-list checks so broad
     * mapping regressions fail immediately. */
    check_known_supported();
    check_known_unsupported();

    /* Then drive a random fourcc from the fuzz input. */
    uint32_t fourcc = 0;
    if (size >= 4) memcpy(&fourcc, data, 4);

    check_random_fourcc(fourcc);
    check_cross_properties(fourcc);
    check_zimg_agrees_with_subsample(fourcc);
    check_subsample_contract(fourcc);
    check_crop_align(fourcc, data, size);

    /* Cycle through 7 mask combinations (1..7) so each NULL position
     * gets tested AND combinations with multiple NULLs. The mask is
     * derived from fuzz input bytes so cppcheck can't dead-code-
     * eliminate the failure branches. Iteration index seeds the offset
     * to avoid always testing the same set in sequence. */
    if (size >= 5) {
        for (uint32_t m = 1; m <= 7; m++) {
            uint32_t mask = (uint32_t)data[4] ^ m;
            check_null_outputs(fourcc, mask);
        }
    } else {
        for (uint32_t m = 1; m <= 7; m++) {
            check_null_outputs(fourcc, m);
        }
    }
}

/* ---- entry points ---- */

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    run_one(data, size);
    return 0;
}

#ifdef FUZZ_MAIN
#include "fuzz_smoke.h"

static int smoke_iter(long i)
{
    uint8_t buf[16];  /* >= 12: check_crop_align reads dims/offsets at [5..11] */
    fuzz_smoke_fill(buf, sizeof buf);

    /* Bias every 4th iteration to a known fourcc, so coverage hits
     * the supported branches explicitly even with chaotic input. */
    if ((i & 3) == 0) {
        static const uint32_t known[] = {
            UP_FOURCC('I','4','2','0'),
            UP_FOURCC('Y','V','1','2'),
            UP_FOURCC('I','4','2','2'),
            UP_FOURCC('I','4','4','4'),
            UP_FOURCC('N','V','1','2'),
            UP_FOURCC('Y','U','Y','2'),
            UP_FOURCC('V','A','O','P'),
        };
        uint32_t pick = known[fuzz_smoke_next()
                              % (sizeof known / sizeof *known)];
        memcpy(buf, &pick, 4);
    }
    run_one(buf, sizeof buf);
    return 0;
}

int main(int argc, char **argv)
{
    fuzz_smoke_seed(0xFACEBEEFu);
    return fuzz_smoke_main(argc, argv, 100000, "scaler_chroma", smoke_iter);
}
#endif
