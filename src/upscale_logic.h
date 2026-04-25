/*****************************************************************************
 * upscale_logic.h — pure decision/math logic for AutoUpscale
 *****************************************************************************
 * This header has zero VLC and FFmpeg dependencies on purpose: the plugin
 * (autoupscale.c) and the standalone test/fuzz harnesses both #include it,
 * so identical code is exercised by the runtime and by the test suite.
 *
 * Everything here is `static inline` and side-effect-free except for writing
 * to caller-provided output structs. No I/O, no global state, no allocation.
 *****************************************************************************/

#ifndef AUTOUPSCALE_UPSCALE_LOGIC_H
#define AUTOUPSCALE_UPSCALE_LOGIC_H

#include <stddef.h>
#include <stdint.h>
#include <limits.h>

/* ---------------------- Public constants ---------------------- */

/* Target presets (user-visible: do NOT renumber). */
#define UP_TARGET_AUTO    0
#define UP_TARGET_720P    1
#define UP_TARGET_1080P   2

/* Algorithm presets (user-visible: do NOT renumber). */
#define UP_ALGO_FAST_BILINEAR  0
#define UP_ALGO_BICUBIC        1
#define UP_ALGO_LANCZOS        2
#define UP_ALGO_SPLINE36       3   /* zimg only; falls back to LANCZOS on swscale */
#define UP_ALGO_MAX            UP_ALGO_SPLINE36

/* Hard upper bound on linear upscale ratio. Going higher than this with any
 * non-AI scaler looks blurry and ringy regardless of algorithm. */
#define UP_MAX_RATIO      4

/* Sanity ceiling for any output dimension. VLC/most decoders break above 8K. */
#define UP_MAX_DIM        32768

/* ---------------------- Public types ---------------------- */

typedef struct {
    int width;
    int height;
} up_dims_t;

/* ---------------------- Helpers (internal) ---------------------- */

static inline int up__clamp_even(int v)
{
    if (v < 0) return 0;
    return v & ~1;
}

/* ---------------------- Public API ---------------------- */

/*
 * Pick a target height given source height, user preset, and detected
 * hardware capacity.
 *
 *   src_h   : source height in pixels (must be > 0)
 *   preset  : UP_TARGET_AUTO | UP_TARGET_720P | UP_TARGET_1080P
 *             (anything else is treated as AUTO)
 *   cores   : number of CPU cores available (>= 1; 0 treated as 1)
 *   mem_mb  : total RAM in MB; 0 means "unknown -> assume sufficient"
 *
 * Returns:
 *   The desired target height. Will be >= src_h. Will not exceed
 *   UP_MAX_RATIO * src_h. Returns 0 if src_h is invalid.
 */
static inline int up_decide_target_height(int src_h, int preset,
                                          long cores, unsigned long mem_mb)
{
    if (src_h <= 0)
        return 0;
    if (cores <= 0)
        cores = 1;

    int target_h;
    if (preset == UP_TARGET_720P) {
        target_h = 720;
    } else if (preset == UP_TARGET_1080P) {
        target_h = 1080;
    } else {
        /* AUTO: 1080p only if we have >= 4 cores AND (>= 2 GB RAM or unknown)
         * AND the upscale ratio to 1080p stays within UP_MAX_RATIO. */
        int ratio_ok_for_1080p = (src_h <= INT_MAX / UP_MAX_RATIO)
                              && (src_h * UP_MAX_RATIO >= 1080);
        int can_1080p = (cores >= 4)
                     && (mem_mb == 0 || mem_mb >= 2048)
                     && ratio_ok_for_1080p;
        target_h = can_1080p ? 1080 : 720;
    }

    /* Clamp to UP_MAX_RATIO * src_h, watching for overflow. */
    if (src_h <= INT_MAX / UP_MAX_RATIO) {
        int cap = src_h * UP_MAX_RATIO;
        if (target_h > cap)
            target_h = cap;
    }

    /* Never downscale. */
    if (target_h < src_h)
        target_h = src_h;

    return target_h;
}

/*
 * Compute aspect-preserving, even-rounded output dimensions.
 *
 *   src_w, src_h : source dimensions, both > 0
 *   target_h     : desired output height, > 0
 *   out          : populated with resulting (width, height); zeroed on failure
 *
 * Returns 1 on success, 0 on invalid input or computed dims out of range.
 */
static inline int up_compute_target_dims(int src_w, int src_h,
                                         int target_h, up_dims_t *out)
{
    if (out == NULL)
        return 0;
    out->width = 0;
    out->height = 0;

    if (src_w <= 0 || src_h <= 0 || target_h <= 0)
        return 0;
    if (src_w > UP_MAX_DIM || src_h > UP_MAX_DIM || target_h > UP_MAX_DIM)
        return 0;

    /* 64-bit math so we don't overflow on huge inputs. */
    int64_t w64 = ((int64_t)src_w * (int64_t)target_h) / (int64_t)src_h;

    /* Even rounding (planar YUV chromas need even dimensions). */
    int target_w = up__clamp_even((int)w64);
    int target_h_even = up__clamp_even(target_h);

    if (target_w <= 0 || target_h_even <= 0)
        return 0;
    if (target_w > UP_MAX_DIM || target_h_even > UP_MAX_DIM)
        return 0;

    out->width  = target_w;
    out->height = target_h_even;
    return 1;
}

/*
 * Top-level: should we upscale, and if so, to what?
 *
 *   src_w, src_h : source dimensions
 *   skip_above   : if src_h >= skip_above, do nothing (typical: 720)
 *   preset       : UP_TARGET_*
 *   cores, mem_mb: hardware capacity (see up_decide_target_height)
 *   out          : populated with (width, height) of upscale target,
 *                  or zeroed when the function returns 0
 *
 * Returns 1 if an upscale should happen, 0 if the filter should bypass.
 */
static inline int up_plan_upscale(int src_w, int src_h, int skip_above,
                                  int preset, long cores,
                                  unsigned long mem_mb, up_dims_t *out)
{
    if (out == NULL)
        return 0;
    out->width = 0;
    out->height = 0;

    if (src_w <= 0 || src_h <= 0)
        return 0;
    if (src_w > UP_MAX_DIM || src_h > UP_MAX_DIM)
        return 0;
    if (skip_above > 0 && src_h >= skip_above)
        return 0;

    int target_h = up_decide_target_height(src_h, preset, cores, mem_mb);
    if (target_h <= src_h)
        return 0;

    if (!up_compute_target_dims(src_w, src_h, target_h, out))
        return 0;

    /* Final guarantee: result must be at least as large as the source in
     * BOTH axes. Even-rounding the width can otherwise produce a smaller
     * value for pathological aspect ratios (e.g. src 3x1024 -> 2x1080).
     * Found by libFuzzer; see tests/fuzz_upscale_logic.c. */
    if (out->width < src_w || out->height < src_h) {
        out->width = 0;
        out->height = 0;
        return 0;
    }
    /* And one of them must be strictly larger, otherwise it's a no-op. */
    if (out->width == src_w && out->height == src_h) {
        out->width = 0;
        out->height = 0;
        return 0;
    }
    return 1;
}

#endif /* AUTOUPSCALE_UPSCALE_LOGIC_H */
