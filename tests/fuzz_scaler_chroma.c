// SPDX-License-Identifier: GPL-2.0-or-later
/*****************************************************************************
 * fuzz_scaler_chroma.c - fuzz the chroma->zimg mapping pulled from the
 *                       scaler backend's VLC interface boundary.
 *****************************************************************************
 * Many VLC-touching helpers are too tangled with `picture_t` /
 * `filter_t` / `var_InheritInteger` to fuzz without an absurd mocking
 * effort. But the chroma->subsample mapping in scaler_zimg.c is pure:
 * it takes a 32-bit fourcc and writes (sub_w, sub_h, yv12_swap) into
 * caller-provided ints, returning 1 on success or 0 on rejection.
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
 *     rejected. Catches regressions where someone naively adds
 *     "case VLC_CODEC_NV12: *sub_w=1; *sub_h=1;" without realizing
 *     zimg can't consume semi-planar formats — that would crash at
 *     runtime on the first NV12 frame.
 *
 *   - Random fourccs from xorshift never produce inconsistent state:
 *     rc=0 means outputs are NOT promised; rc=1 means outputs are
 *     all set to documented values.
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
 *     fourcc accidentally got added to the supported list.
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
        unsigned sw, sh;
        int swap;
        int rc = up_chroma_to_zimg(known_unsupported[i], &sw, &sh, &swap);
        if (rc != 0) {
            FAIL("known unsupported 0x%08x accepted (rc=%d)",
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
     * Pre-fill outputs with sentinels; for rc=0 we don't assert anything
     * about output values (function may or may not write them); for rc=1
     * we already check ranges in check_cross_properties. */
    unsigned sw = 0xDEAD;
    unsigned sh = 0xBEEF;
    int swap = 0xCAFE;
    int rc = up_chroma_to_zimg(fourcc, &sw, &sh, &swap);
    if (rc != 0 && rc != 1) {
        FAIL("0x%08x produced invalid rc=%d", fourcc, rc);
    }
    /* If rc=1, all three outputs must have been written. We can detect a
     * "wrote some but not all" bug because the sentinels are distinct. */
    if (rc == 1) {
        if (sw == 0xDEAD) FAIL("0x%08x rc=1 but sub_w unwritten", fourcc);
        if (sh == 0xBEEF) FAIL("0x%08x rc=1 but sub_h unwritten", fourcc);
        if (swap == 0xCAFE) FAIL("0x%08x rc=1 but swap unwritten", fourcc);
    }
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
int main(int argc, char **argv)
{
    long n = 100000;
    if (argc > 1) {
        char *end = NULL;
        long v = strtol(argv[1], &end, 10);
        if (end && *end == '\0' && v > 0) n = v;
    }

    uint32_t s = 0xFACEBEEFu;
    uint8_t buf[8];

    for (long i = 0; i < n; i++) {
        for (size_t j = 0; j < sizeof buf; j += 4) {
            s ^= s << 13;
            s ^= s >> 17;
            s ^= s <<  5;
            memcpy(buf + j, &s, 4);
        }
        /* Bias every 4th iteration to a known fourcc, so coverage hits
         * the supported branches explicitly even with chaotic input. */
        if ((i & 3) == 0) {
            uint32_t known[] = {
                UP_FOURCC('I','4','2','0'),
                UP_FOURCC('Y','V','1','2'),
                UP_FOURCC('I','4','2','2'),
                UP_FOURCC('I','4','4','4'),
                UP_FOURCC('N','V','1','2'),
                UP_FOURCC('Y','U','Y','2'),
                UP_FOURCC('V','A','O','P'),
            };
            uint32_t pick = known[s % (sizeof known / sizeof *known)];
            memcpy(buf, &pick, 4);
        }
        run_one(buf, sizeof buf);
    }

    printf("scaler_chroma smoke OK: %ld iterations\n", n);
    return 0;
}
#endif
