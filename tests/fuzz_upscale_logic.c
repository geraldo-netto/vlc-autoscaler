// SPDX-License-Identifier: GPL-2.0-or-later
/*****************************************************************************
 * fuzz_upscale_logic.c — fuzzer for the pure logic
 *****************************************************************************
 * Two build modes (controlled by the FUZZ_MAIN macro):
 *
 *   - libFuzzer target (default with clang -fsanitize=fuzzer):
 *       Exposes LLVMFuzzerTestOneInput. Build:
 *         clang -fsanitize=fuzzer,address,undefined -I src \
 *             tests/fuzz_upscale_logic.c -o build/fuzz
 *
 *   - Smoke runner (-DFUZZ_MAIN):
 *       Provides a main() that runs N deterministic random-ish inputs.
 *       Useful in CI when libFuzzer isn't available, or as a quick check.
 *
 * The harness verifies the same set of post-conditions in both modes.
 *****************************************************************************/

#include "../src/upscale_logic.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Verify the result of up_plan_upscale matches its documented contract.
 * Aborts (which the fuzzer reports as a finding) on any violation.
 */
static void check_invariants(int src_w, int src_h, int skip_above,
                             int preset, long cores, unsigned long mem_mb,
                             int rc, const up_dims_t *out)
{
    if (rc == 0) {
        /* On bypass, output must be zeroed. */
        if (out->width != 0 || out->height != 0) {
            fprintf(stderr,
                    "INVARIANT: rc=0 but out=(%d,%d) for "
                    "src=(%d,%d) skip=%d preset=%d cores=%ld mem=%lu\n",
                    out->width, out->height, src_w, src_h,
                    skip_above, preset, cores, mem_mb);
            abort();
        }
        return;
    }

    /* rc == 1: an upscale was planned. */
    if (out->width <= 0 || out->height <= 0) abort();
    if (out->width  > UP_MAX_DIM)             abort();
    if (out->height > UP_MAX_DIM)             abort();

    /* Output must be even (chroma alignment). */
    if (out->width  & 1) abort();
    if (out->height & 1) abort();

    /* Result must actually be an upscale, not a downscale. */
    if (out->width  < src_w) abort();
    if (out->height < src_h) abort();

    /* Aspect-ratio preservation within rounding slack:
     *   |src_w*out.height - out.width*src_h|  <=  2*src_h + 4
     * Bound derives from: target_w = src_w*target_h/src_h is floor-divided
     * (off by <= 1 of the true value, so the cross product diff is <= src_h),
     * then rounded down to even (off by another 1, so another src_h). */
    int64_t lhs = (int64_t)src_w * (int64_t)out->height;
    int64_t rhs = (int64_t)out->width * (int64_t)src_h;
    int64_t diff = lhs - rhs;
    if (diff < 0) diff = -diff;
    int64_t slack = 2LL * (int64_t)src_h + 4LL;
    if (diff > slack) abort();

    /* Upscale ratio cap: out.height <= UP_MAX_RATIO * src_h. */
    if (src_h > 0 && src_h <= INT_MAX / UP_MAX_RATIO) {
        if (out->height > src_h * UP_MAX_RATIO) abort();
    }

    /* --- Design-rule invariants for the resolution ladder --- */

    /* AUTO must never produce output above 1080p. Going higher requires
     * explicit user opt-in (target=3..6). This is the design ceiling. */
    if (preset == UP_TARGET_AUTO && out->height > 1080) {
        fprintf(stderr,
                "INVARIANT: AUTO produced height=%d (must be <= 1080) for "
                "src=(%d,%d) cores=%ld mem=%lu\n",
                out->height, src_w, src_h, cores, mem_mb);
        abort();
    }

    /* For valid known presets: if no ratio cap binds, the output height
     * must reach the preset's nominal target. This guards against a
     * regression where a switch case silently falls through to AUTO. */
    int nominal = 0;
    switch (preset) {
        case UP_TARGET_720P:   nominal =  720; break;
        case UP_TARGET_1080P:  nominal = 1080; break;
        case UP_TARGET_1440P:  nominal = 1440; break;
        case UP_TARGET_4K:     nominal = 2160; break;
        case UP_TARGET_5K:     nominal = 2880; break;
        case UP_TARGET_8K:     nominal = 4320; break;
        default: nominal = 0;  /* AUTO or unknown -> no fixed target */
    }
    if (nominal > 0 && src_h > 0 && src_h <= INT_MAX / UP_MAX_RATIO) {
        int cap = src_h * UP_MAX_RATIO;
        int expected = (nominal <= cap) ? nominal : cap;
        if (expected < src_h) expected = src_h;
        /* Even-rounding can shave 1 pixel; allow that slack. */
        if (out->height + 1 < expected) {
            fprintf(stderr,
                    "INVARIANT: preset=%d expected height>=%d (cap=%d) "
                    "got %d for src_h=%d\n",
                    preset, expected, cap, out->height, src_h);
            abort();
        }
    }
}

/*
 * Single fuzz iteration. Reads up to 32 bytes from `data` and uses them to
 * synthesise inputs. Short inputs are zero-padded.
 */
static void run_one(const uint8_t *data, size_t size)
{
    uint8_t buf[32] = { 0 };
    size_t n = size < sizeof buf ? size : sizeof buf;
    if (n > 0) memcpy(buf, data, n);

    /* Carve fields out of the buffer. */
    int32_t i_src_w, i_src_h, i_skip, i_preset;
    int32_t i_cores;
    uint32_t u_mem_mb;
    memcpy(&i_src_w,  buf +  0, 4);
    memcpy(&i_src_h,  buf +  4, 4);
    memcpy(&i_skip,   buf +  8, 4);
    memcpy(&i_preset, buf + 12, 4);
    memcpy(&i_cores,  buf + 16, 4);
    memcpy(&u_mem_mb, buf + 20, 4);

    /* Don't artificially constrain - the whole point of a fuzzer is to feed
     * weird values - but do clamp `cores` to something representable as a
     * `long` argument so we're testing the contract, not C type coercion. */
    long cores = (long)i_cores;
    unsigned long mem_mb = (unsigned long)u_mem_mb;

    up_dims_t out = { -1, -1 };
    int rc = up_plan_upscale(i_src_w, i_src_h, i_skip, i_preset,
                             cores, mem_mb, &out);

    check_invariants(i_src_w, i_src_h, i_skip, i_preset,
                     cores, mem_mb, rc, &out);

    /* Also exercise the helpers individually with the same inputs. */
    int dh = up_decide_target_height(i_src_h, i_preset, cores, mem_mb);
    if (i_src_h > 0) {
        if (dh < 0) abort();
        if (dh != 0 && dh < i_src_h) abort();
    } else {
        if (dh != 0) abort();
    }

    up_dims_t d2 = { -1, -1 };
    int rc2 = up_compute_target_dims(i_src_w, i_src_h, i_skip, &d2);
    if (rc2 == 0) {
        if (d2.width != 0 || d2.height != 0) abort();
    } else {
        if ((d2.width & 1) || (d2.height & 1)) abort();
        if (d2.width <= 0 || d2.height <= 0) abort();
        if (d2.width > UP_MAX_DIM || d2.height > UP_MAX_DIM) abort();
    }
}

/* ---------- libFuzzer entry point ---------- */

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    run_one(data, size);
    return 0;
}

/* ---------- standalone smoke main (FUZZ_MAIN) ---------- */

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
    uint32_t s = 0xDEADBEEFu;
    uint8_t buf[32];

    for (long i = 0; i < n; i++) {
        for (size_t j = 0; j < sizeof buf; j += 4) {
            s ^= s << 13;
            s ^= s >> 17;
            s ^= s <<  5;
            memcpy(buf + j, &s, 4);
        }

        /* Bias every 4th iteration to use a *valid* preset value. The raw
         * xorshift bytes give roughly uniform 32-bit ints, which means
         * meaningful presets (0..6) hit only ~1.6e-7 of the time. Without
         * biasing, the new ladder branches would be functionally untested
         * by the smoke runner. With biasing, every preset gets ~25%/7 ≈
         * 3.5% of iterations, which at 100k = 3500 hits per preset. */
        if ((i & 3) == 0) {
            int p = (int)(s % (UP_TARGET_MAX + 1u));  /* 0..UP_TARGET_MAX */
            memcpy(buf + 12, &p, 4);
        }

        /* Bias every 4th-plus-1 iteration to use realistic source heights.
         * Otherwise random int32 src_h is almost always negative or huge. */
        if ((i & 3) == 1) {
            static const int common_heights[] = {
                144, 240, 288, 360, 480, 540, 576, 720, 1080, 1440, 2160, 4320
            };
            int h = common_heights[s % (sizeof common_heights / sizeof *common_heights)];
            int w = (h * 16) / 9;  /* 16:9 default */
            memcpy(buf + 0, &w, 4);
            memcpy(buf + 4, &h, 4);
        }

        run_one(buf, sizeof buf);
    }

    printf("Smoke fuzz OK: %ld iterations\n", n);
    return 0;
}
#endif
