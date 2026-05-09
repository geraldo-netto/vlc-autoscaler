// SPDX-License-Identifier: GPL-2.0-or-later
/*****************************************************************************
 * content_probe.h - pure content-aware metrics for upscaling decision
 *****************************************************************************
 * Two no-reference quality proxies used to decide whether an upscale will
 * help on this particular source, computed over a short probe window
 * (first ~60 frames) at the start of playback. If both metrics suggest
 * upscaling won't recover useful detail, the filter sets a bypass flag
 * for the rest of playback and passes frames through unchanged.
 *
 * The two metrics:
 *
 *   1. Laplacian variance over a sub-sampled luma plane.
 *      Measures high-frequency energy. Higher value = sharper source =
 *      more detail available to interpolate from. Very low values (heavy
 *      blur, gaussian-noise-only content, blank frames) indicate the
 *      source has nothing for a non-AI scaler to "uncover".
 *
 *   2. Block-edge intensity along 8-pixel boundaries.
 *      Measures compression-artifact density. H.264/H.265 produce visible
 *      blocking on horizontal/vertical 8-pixel grids when bitrate is too
 *      low. Upscaling amplifies these artifacts — a 1440p output of an
 *      already-blocky 480p stream is more annoying than the original.
 *      Very high values mean upscale will hurt more than help.
 *
 * Both metrics are computed on the LUMA plane only (the visual signal
 * humans care about) and on a deliberately sub-sampled grid (every 4th
 * row × every 4th column) to keep cost under ~50 µs/frame at 480p.
 *
 * Header-only, no VLC dependencies, fuzzable. The decision logic that
 * combines the metrics into a bypass/no-bypass verdict lives in
 * up_should_bypass_for_content() at the bottom of this file.
 *****************************************************************************/

#ifndef AUTOUPSCALE_CONTENT_PROBE_H
#define AUTOUPSCALE_CONTENT_PROBE_H

#include <stddef.h>
#include <stdint.h>

/* Sub-sample grid step. Smaller = more samples = more accurate but
 * costlier. 4 gives ~64x64 sample points on a 256x256 thumbnail of a
 * 1080p frame, which is plenty for variance and edge-density estimation
 * and runs in well under 100 µs even on a slow CPU. */
#define UP_PROBE_GRID_STEP   4

/* Block size for compression-artifact detection. H.264 and H.265 both
 * use 8x8 transform blocks (H.265 also has 4x4 and 16x16, but the 8x8
 * grid is the most consistently visible artifact boundary). */
#define UP_PROBE_BLOCK_SIZE  8

/*
 * Compute Laplacian variance on a sub-sampled grid of the luma plane.
 *
 *   plane        : pointer to luma plane (Y), row-major
 *   stride       : bytes per row (may be > w due to alignment padding)
 *   w, h         : visible plane dimensions in pixels
 *
 * Returns: variance estimate as a uint64_t (sum of squared 4-connected
 *          Laplacian responses on the sample grid). 0 if input is invalid
 *          or too small to sample.
 *
 * The Laplacian kernel is 4*center - (top + bottom + left + right).
 * High variance => lots of edges and texture (sharp source).
 * Low variance  => smooth or blurry source.
 *
 * We accumulate the variance directly (sum of squared responses), not
 * mean-and-variance — for our purposes we just need a relative-magnitude
 * signal, and the un-normalized form is faster and doesn't require a
 * second pass.
 *
 * Returns the SUM of squared Laplacians (not mean). Caller can normalize
 * by sample count if a per-pixel value is needed; for our threshold
 * compare we use the raw sum compared against (n_samples * threshold²).
 */
static inline uint64_t up_laplacian_variance(const uint8_t *plane,
                                             int stride, int w, int h,
                                             uint64_t *n_samples_out)
{
    if (n_samples_out) *n_samples_out = 0;
    if (plane == NULL || stride <= 0 || w <= 2 || h <= 2)
        return 0;
    /* Defensive: the caller's buffer is sized as stride * h. If stride
     * is less than w, our `row[x]` access for x in [step, w-1) would
     * read past the end of the row's actual storage and into either
     * the next row or off the buffer entirely. VLC's frame layout
     * always satisfies i_pitch >= i_visible_pitch, but we don't trust
     * the input — caught by ASan in fuzz_content_probe. */
    if (stride < w)
        return 0;
    /* UP_PROBE_GRID_STEP is a compile-time constant >= 1; no defensive
     * clamp needed. */
    int step = UP_PROBE_GRID_STEP;

    uint64_t sum_sq = 0;
    uint64_t n = 0;

    for (int y = step; y < h - 1; y += step) {
        const uint8_t *row    = plane + (size_t)y * (size_t)stride;
        const uint8_t *row_up = row - stride;
        const uint8_t *row_dn = row + stride;
        for (int x = step; x < w - 1; x += step) {
            int center = row[x];
            int top    = row_up[x];
            int bot    = row_dn[x];
            int left   = row[x - 1];
            int right  = row[x + 1];
            int lap = 4 * center - (top + bot + left + right);
            /* lap is in [-1020, 1020]; squared fits in 32 bits, sum in 64. */
            sum_sq += (uint64_t)(lap * lap);
            n++;
        }
    }
    if (n_samples_out) *n_samples_out = n;
    return sum_sq;
}

/*
 * Compute mean absolute block-edge difference for an 8x8 block grid on
 * the luma plane. For each pixel that sits on an 8-pixel boundary (in
 * either axis), we measure |this_pixel - neighbor_across_boundary|.
 *
 * High-quality sources have similar pixel values across block boundaries
 * (no discontinuity). Heavily-compressed sources have a sudden value
 * change at every block edge — that's what produces visible "blockiness".
 *
 * Returns the SUM of absolute differences across all sampled boundary
 * pixels. Caller can divide by *n_samples_out for a mean. 0 if input is
 * invalid or the plane is too small to sample.
 */
/* Vertical block edges: difference between column (k*b - 1) and
 * column (k*b) at every step-th row. Accumulates into *sum/*n. */
static inline void up_block_edge_vertical(const uint8_t *plane,
                                          int stride, int w, int h,
                                          int b, int step,
                                          uint64_t *sum, uint64_t *n)
{
    for (int y = 0; y < h; y += step) {
        const uint8_t *row = plane + (size_t)y * (size_t)stride;
        for (int x = b; x < w; x += b) {
            int diff = (int)row[x] - (int)row[x - 1];
            if (diff < 0) diff = -diff;
            *sum += (uint64_t)diff;
            (*n)++;
        }
    }
}

/* Horizontal block edges: difference between row (k*b - 1) and
 * row (k*b) at every step-th column. Accumulates into *sum/*n. */
static inline void up_block_edge_horizontal(const uint8_t *plane,
                                            int stride, int w, int h,
                                            int b, int step,
                                            uint64_t *sum, uint64_t *n)
{
    for (int y = b; y < h; y += b) {
        const uint8_t *row    = plane + (size_t)y * (size_t)stride;
        const uint8_t *row_up = row - stride;
        for (int x = 0; x < w; x += step) {
            int diff = (int)row[x] - (int)row_up[x];
            if (diff < 0) diff = -diff;
            *sum += (uint64_t)diff;
            (*n)++;
        }
    }
}

static inline uint64_t up_block_edge_strength(const uint8_t *plane,
                                              int stride, int w, int h,
                                              uint64_t *n_samples_out)
{
    if (n_samples_out) *n_samples_out = 0;
    if (plane == NULL || stride <= 0 || w <= UP_PROBE_BLOCK_SIZE
                                     || h <= UP_PROBE_BLOCK_SIZE)
        return 0;
    /* Same stride defensive check as up_laplacian_variance — see comment
     * there. Reading past the end of the row's storage is UB; bail. */
    if (stride < w)
        return 0;

    uint64_t sum_abs = 0;
    uint64_t n = 0;
    int b = UP_PROBE_BLOCK_SIZE;
    int step = UP_PROBE_GRID_STEP;
    /* UP_PROBE_GRID_STEP and UP_PROBE_BLOCK_SIZE are compile-time
     * constants >= 1; no defensive clamp needed. */

    up_block_edge_vertical(plane, stride, w, h, b, step, &sum_abs, &n);
    up_block_edge_horizontal(plane, stride, w, h, b, step, &sum_abs, &n);

    if (n_samples_out) *n_samples_out = n;
    return sum_abs;
}

/*
 * Probe accumulator. Tracks metric averages over the probe window so a
 * single noisy frame doesn't bias the decision. Initialize to zero and
 * call up_probe_observe() for each frame in the window.
 */
typedef struct {
    uint64_t lap_sum;       /* sum of laplacian variances over frames */
    uint64_t lap_samples;   /* total laplacian samples */
    uint64_t edge_sum;      /* sum of block-edge intensities */
    uint64_t edge_samples;  /* total block-edge samples */
    int      frames;        /* number of frames observed */
} up_probe_accum_t;

/*
 * Add one frame's metrics to the accumulator. Call this after computing
 * the metrics for a frame's luma plane. Safe to call with zero metrics
 * (e.g. if the plane was too small) — it will just not advance.
 */
static inline void up_probe_observe(up_probe_accum_t *a,
                                    uint64_t lap, uint64_t lap_n,
                                    uint64_t edge, uint64_t edge_n)
{
    if (a == NULL) return;
    a->lap_sum     += lap;
    a->lap_samples += lap_n;
    a->edge_sum    += edge;
    a->edge_samples += edge_n;
    a->frames++;
}

/*
 * Bypass-decision rule based on accumulated probe metrics.
 *
 * Returns:
 *   1 -> upscaling is unlikely to help; recommend bypass (pass-through)
 *   0 -> proceed with upscaling
 *
 * Decision logic:
 *
 *   - If we observed too few samples to be confident, return 0
 *     (don't bypass — give the scaler a chance).
 *
 *   - If the source is very soft (mean Laplacian variance < THRESH_SOFT)
 *     AND it's also very blocky (mean block-edge intensity > THRESH_BLOCKY),
 *     we have a heavily-compressed soft source: upscaling will amplify
 *     the blocking without any detail to recover. Bypass.
 *
 *   - If the source is just very soft (no detail to recover) but NOT
 *     blocky, upscaling may still produce a passable smooth output —
 *     don't bypass on softness alone.
 *
 *   - If the source is just blocky but has detail elsewhere, the user
 *     probably still wants upscaling — don't bypass on blockiness alone.
 *
 * Both conditions in conjunction are the "actively bad to upscale" case.
 *
 * Thresholds are chosen conservatively. A short clip of clean Spline36
 * upscaling on grainy/textured 480p content yields ~lap-mean 800-2000
 * and edge-mean 1-4. Heavily-blocky low-bitrate content yields lap-mean
 * 200-400 and edge-mean 8-15. We bypass only in the second region.
 */
#define UP_PROBE_THRESH_SOFT_LAP_MEAN    400   /* below = "very soft" */
#define UP_PROBE_THRESH_BLOCKY_EDGE_MEAN  6    /* above = "very blocky" */
#define UP_PROBE_MIN_FRAMES               10   /* need this many frames */
#define UP_PROBE_MIN_SAMPLES_PER_KIND  20000   /* need this many samples */

/* Above this lap_mean (sum-of-squared-laplacians per sample), the source
 * is heavily textured/grainy. USM on such content mostly amplifies noise
 * without adding perceived sharpness. We skip USM after probe close.
 * Comment in this file notes clean grainy 480p yields ~800-2000;
 * 3500 is above that band so we trigger only on actively over-detailed
 * sources (film grain, high-noise sensors, etc.). */
#define UP_PROBE_THRESH_SHARP_LAP_MEAN  3500

static inline int up_should_skip_usm_for_sharpness(const up_probe_accum_t *a)
{
    if (a == NULL) return 0;
    if (a->frames < UP_PROBE_MIN_FRAMES) return 0;
    if (a->lap_samples < UP_PROBE_MIN_SAMPLES_PER_KIND) return 0;
    uint64_t lap_mean = a->lap_sum / a->lap_samples;
    return (lap_mean > (uint64_t)UP_PROBE_THRESH_SHARP_LAP_MEAN) ? 1 : 0;
}

static inline int up_should_bypass_for_content(const up_probe_accum_t *a)
{
    if (a == NULL) return 0;
    if (a->frames < UP_PROBE_MIN_FRAMES) return 0;
    if (a->lap_samples  < UP_PROBE_MIN_SAMPLES_PER_KIND) return 0;
    if (a->edge_samples < UP_PROBE_MIN_SAMPLES_PER_KIND) return 0;

    /* Compute means as plain integers (sum / sample-count). The lap_sum
     * is sum of (lap*lap), so we compare the mean against threshold². */
    uint64_t lap_mean  = a->lap_sum  / a->lap_samples;
    uint64_t edge_mean = a->edge_sum / a->edge_samples;

    int very_soft  = (lap_mean  < (uint64_t)UP_PROBE_THRESH_SOFT_LAP_MEAN
                                * (uint64_t)UP_PROBE_THRESH_SOFT_LAP_MEAN);
    int very_blocky = (edge_mean > (uint64_t)UP_PROBE_THRESH_BLOCKY_EDGE_MEAN);

    return (very_soft && very_blocky) ? 1 : 0;
}

#endif /* AUTOUPSCALE_CONTENT_PROBE_H */
