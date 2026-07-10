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
#include <limits.h>
#include <limits.h>

static void chk_bypass(int src_w, int src_h, int skip_above,
                       int preset, int cores, unsigned long mem_mb,
                       const up_dims_t *out)
{
    if (out->width != 0 || out->height != 0) {
        fprintf(stderr,
                "INVARIANT: rc=0 but out=(%d,%d) for "
                "src=(%d,%d) skip=%d preset=%d cores=%d mem=%lu\n",
                out->width, out->height, src_w, src_h,
                skip_above, preset, cores, mem_mb);
        abort();
    }
}

static void chk_dim_bounds(const up_dims_t *out)
{
    if (out->width <= 0 || out->height <= 0) abort();
    if (out->width  > UP_MAX_DIM)             abort();
    if (out->height > UP_MAX_DIM)             abort();
    if (out->width  & 1) abort();
    if (out->height & 1) abort();
}

static void chk_not_downscale(int src_w, int src_h, const up_dims_t *out)
{
    if (out->width  < src_w) abort();
    if (out->height < src_h) abort();
}

static void chk_aspect(int src_w, int src_h, const up_dims_t *out)
{
    /* |src_w*out.height - out.width*src_h|  <=  2*src_h + 4 */
    int64_t lhs = (int64_t)src_w * (int64_t)out->height;
    int64_t rhs = (int64_t)out->width * (int64_t)src_h;
    int64_t diff = lhs - rhs;
    if (diff < 0) diff = -diff;
    int64_t slack = 2LL * (int64_t)src_h + 4LL;
    if (diff > slack) abort();
}

static void chk_ratio_cap(int src_h, const up_dims_t *out)
{
    if (src_h > 0 && src_h <= INT_MAX / UP_MAX_RATIO) {
        if (out->height > src_h * UP_MAX_RATIO) abort();
    }
}

static void chk_auto_ceiling(int src_w, int src_h, int preset,
                             int cores, unsigned long mem_mb,
                             const up_dims_t *out)
{
    if (preset == UP_TARGET_AUTO && out->height > 1080) {
        fprintf(stderr,
                "INVARIANT: AUTO produced height=%d (must be <= 1080) for "
                "src=(%d,%d) cores=%d mem=%lu\n",
                out->height, src_w, src_h, cores, mem_mb);
        abort();
    }
}

static int preset_nominal(int preset)
{
    switch (preset) {
        case UP_TARGET_720P:   return  720;
        case UP_TARGET_1080P:  return 1080;
        case UP_TARGET_1440P:  return 1440;
        case UP_TARGET_4K:     return 2160;
        case UP_TARGET_5K:     return 2880;
        case UP_TARGET_8K:     return 4320;
        default:               return 0;
    }
}

static void chk_preset_reaches_nominal(int src_h, int preset,
                                       const up_dims_t *out)
{
    int nominal = preset_nominal(preset);
    if (nominal <= 0 || src_h <= 0 || src_h > INT_MAX / UP_MAX_RATIO) return;
    int cap = src_h * UP_MAX_RATIO;
    int expected = (nominal <= cap) ? nominal : cap;
    if (expected < src_h) expected = src_h;
    if (out->height + 1 < expected) {
        fprintf(stderr,
                "INVARIANT: preset=%d expected height>=%d (cap=%d) "
                "got %d for src_h=%d\n",
                preset, expected, cap, out->height, src_h);
        abort();
    }
}

/*
 * Verify the result of up_plan_upscale matches its documented contract.
 * Aborts (which the fuzzer reports as a finding) on any violation.
 */
static void check_invariants(int src_w, int src_h, int skip_above,
                             int preset, int cores, unsigned long mem_mb,
                             int rc, const up_dims_t *out)
{
    if (rc == 0) {
        chk_bypass(src_w, src_h, skip_above, preset, cores, mem_mb, out);
        return;
    }
    chk_dim_bounds(out);
    chk_not_downscale(src_w, src_h, out);
    chk_aspect(src_w, src_h, out);
    chk_ratio_cap(src_h, out);
    chk_auto_ceiling(src_w, src_h, preset, cores, mem_mb, out);
    chk_preset_reaches_nominal(src_h, preset, out);
}

typedef struct {
    int32_t  src_w, src_h, skip, preset;
    int      cores;
    unsigned long mem_mb;
} fuzz_inputs_t;

static int clamp_cores(int32_t v)
{
    if (v > INT_MAX) return INT_MAX;
    if (v < INT_MIN) return INT_MIN;
    return (int)v;
}

static void parse_inputs(const uint8_t *data, size_t size, fuzz_inputs_t *fi)
{
    uint8_t buf[32] = { 0 };
    size_t n = size < sizeof buf ? size : sizeof buf;
    if (n > 0) memcpy(buf, data, n);

    int32_t i_cores;
    uint32_t u_mem_mb;
    memcpy(&fi->src_w,  buf +  0, 4);
    memcpy(&fi->src_h,  buf +  4, 4);
    memcpy(&fi->skip,   buf +  8, 4);
    memcpy(&fi->preset, buf + 12, 4);
    memcpy(&i_cores,    buf + 16, 4);
    memcpy(&u_mem_mb,   buf + 20, 4);

    fi->cores  = clamp_cores(i_cores);
    fi->mem_mb = (unsigned long)u_mem_mb;
}

static void chk_dh_oor_matches_auto(const fuzz_inputs_t *fi, int dh)
{
    int dh_auto = up_decide_target_height(fi->src_h, UP_TARGET_AUTO,
                                          fi->cores, fi->mem_mb);
    if (dh != dh_auto) {
        fprintf(stderr,
                "INVARIANT: out-of-range preset=%d gave dh=%d but "
                "AUTO gave %d (src_h=%d cores=%d mem=%lu)\n",
                fi->preset, dh, dh_auto, fi->src_h, fi->cores, fi->mem_mb);
        abort();
    }
}

static void check_decide_target_height(const fuzz_inputs_t *fi)
{
    int dh = up_decide_target_height(fi->src_h, fi->preset, fi->cores, fi->mem_mb);
    if (fi->src_h > 0) {
        if (dh < 0) abort();
        if (dh != 0 && dh < fi->src_h) abort();
    } else {
        if (dh != 0) abort();
    }

    /* Out-of-range presets must behave identically to AUTO. */
    if (fi->src_h > 0 &&
        (fi->preset < UP_TARGET_AUTO || fi->preset > UP_TARGET_MAX)) {
        chk_dh_oor_matches_auto(fi, dh);
    }
}

static void chk_ctd_nonzero(const up_dims_t *d2)
{
    if ((d2->width & 1) || (d2->height & 1)) abort();
    if (d2->width <= 0 || d2->height <= 0) abort();
    if (d2->width > UP_MAX_DIM || d2->height > UP_MAX_DIM) abort();
}

static void check_compute_target_dims(const fuzz_inputs_t *fi)
{
    up_dims_t d2 = { -1, -1 };
    int rc2 = up_compute_target_dims(fi->src_w, fi->src_h, fi->skip, &d2);
    if (rc2 == 0) {
        if (d2.width != 0 || d2.height != 0) abort();
    } else {
        chk_ctd_nonzero(&d2);
    }
}

static void check_auto_enum_normalization(const fuzz_inputs_t *fi)
{
    const int target = up_normalize_auto_enum(fi->preset, UP_TARGET_MAX);
    const int expected_target = fi->preset >= 0 && fi->preset <= UP_TARGET_MAX
                              ? fi->preset : UP_TARGET_AUTO;
    if (target != expected_target) abort();

    const int backend = up_normalize_auto_enum(fi->skip, 2);
    const int expected_backend = fi->skip >= 0 && fi->skip <= 2 ? fi->skip : 0;
    if (backend != expected_backend) abort();
}

/*
 * Single fuzz iteration. Reads up to 32 bytes from `data` and uses them to
 * synthesise inputs. Short inputs are zero-padded.
 */
static void run_one(const uint8_t *data, size_t size)
{
    fuzz_inputs_t fi;
    parse_inputs(data, size, &fi);

    up_dims_t out = { -1, -1 };
    int rc = up_plan_upscale(fi.src_w, fi.src_h, fi.skip, fi.preset,
                             fi.cores, fi.mem_mb, &out);

    check_invariants(fi.src_w, fi.src_h, fi.skip, fi.preset,
                     fi.cores, fi.mem_mb, rc, &out);

    check_decide_target_height(&fi);
    check_compute_target_dims(&fi);
    check_auto_enum_normalization(&fi);
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

        /* Regularly bias input toward valid presets; uniform raw integers
         * would almost never exercise the resolution ladder. */
        if ((i & 3) == 0) {
            int p = (int)(s % (UP_TARGET_MAX + 1u));  /* 0..UP_TARGET_MAX */
            memcpy(buf + 12, &p, 4);
        }

        /* Also bias toward realistic source heights; raw src_h values are
         * usually negative or huge. */
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
