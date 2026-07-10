// SPDX-License-Identifier: GPL-2.0-or-later
/*****************************************************************************
 * content_probe.h - pure content-aware metrics for advisory and USM gating
 *****************************************************************************
 * Two no-reference quality proxies used to decide whether an upscale will
 * help on this particular source, computed over a bounded initial luma window
 * at playback start. If both metrics suggest
 * upscaling won't recover useful detail, the filter logs a one-time advisory.
 * VLC 3 cannot renegotiate the output format mid-stream, so scaling continues.
 *
 * The two metrics:
 *
 *   1. Mean squared Laplacian response over a sub-sampled luma plane.
 *      Measures high-frequency energy. Higher value = sharper source =
 *      more detail available to interpolate from. Very low values (heavy
 *      blur or blank frames) indicate the source has nothing for a non-AI
 *      scaler to "uncover". Noise raises this energy metric and is handled
 *      independently by the high-value USM-skip gate.
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
 * row × every 4th column). The grid spans the full visible luma plane, so
 * work scales with frame dimensions.
 *
 * Header-only, no VLC dependencies, fuzzable. The decision logic that
 * combines the metrics into a bypass advisory verdict lives in
 * up_should_bypass_for_content() at the bottom of this file.
 *****************************************************************************/

#ifndef AUTOUPSCALE_CONTENT_PROBE_H
#define AUTOUPSCALE_CONTENT_PROBE_H

#include <stddef.h>
#include <stdint.h>

/* Sub-sample grid step. Smaller = more samples = more accurate but costlier.
 * A value of 4 samples the full visible plane at every fourth row/column. */
#define UP_PROBE_GRID_STEP   4

/* Block size for compression-artifact detection. H.264 and H.265 both
 * use 8x8 transform blocks (H.265 also has 4x4 and 16x16, but the 8x8
 * grid is the most consistently visible artifact boundary). */
#define UP_PROBE_BLOCK_SIZE  8

/*
 * Compute squared Laplacian energy on a sub-sampled grid of the luma plane.
 *
 *   plane        : pointer to luma plane (Y), row-major
 *   stride       : bytes per row (may be > w due to alignment padding)
 *   w, h         : visible plane dimensions in pixels
 *
 * Returns: sum of squared 4-connected Laplacian responses on the sample
 *          grid. 0 if input is invalid
 *          or too small to sample.
 *
 * The Laplacian kernel is 4*center - (top + bottom + left + right).
 * High energy => lots of edges, texture, or noise.
 * Low energy  => smooth or blurry source.
 *
 * Returns the SUM of squared Laplacians (not mean). Callers normalize by
 * sample count: every threshold in this file (SOFT, SHARP) is expressed
 * in those mean-of-squared-Laplacians units and compared linearly.
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
/* Running block-edge accumulator shared by the two samplers below. */
typedef struct {
    uint64_t sum;
    uint64_t n;
} up_edge_acc_t;

/* Vertical block edges: difference between column (k*b - 1) and
 * column (k*b) at every step-th row. Accumulates into acc. */
static inline void up_block_edge_vertical(const uint8_t *plane,
                                          int stride, int w, int h,
                                          int b, int step,
                                          up_edge_acc_t *acc)
{
    for (int y = 0; y < h; y += step) {
        const uint8_t *row = plane + (size_t)y * (size_t)stride;
        for (int x = b; x < w; x += b) {
            int diff = (int)row[x] - (int)row[x - 1];
            if (diff < 0) diff = -diff;
            acc->sum += (uint64_t)diff;
            acc->n++;
        }
    }
}

/* Horizontal block edges: difference between row (k*b - 1) and
 * row (k*b) at every step-th column. Accumulates into acc. */
static inline void up_block_edge_horizontal(const uint8_t *plane,
                                            int stride, int w, int h,
                                            int b, int step,
                                            up_edge_acc_t *acc)
{
    for (int y = b; y < h; y += b) {
        const uint8_t *row    = plane + (size_t)y * (size_t)stride;
        const uint8_t *row_up = row - stride;
        for (int x = 0; x < w; x += step) {
            int diff = (int)row[x] - (int)row_up[x];
            if (diff < 0) diff = -diff;
            acc->sum += (uint64_t)diff;
            acc->n++;
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

    up_edge_acc_t acc = { 0, 0 };
    int b = UP_PROBE_BLOCK_SIZE;
    int step = UP_PROBE_GRID_STEP;
    /* UP_PROBE_GRID_STEP and UP_PROBE_BLOCK_SIZE are compile-time
     * constants >= 1; no defensive clamp needed. */

    up_block_edge_vertical(plane, stride, w, h, b, step, &acc);
    up_block_edge_horizontal(plane, stride, w, h, b, step, &acc);

    if (n_samples_out) *n_samples_out = acc.n;
    return acc.sum;
}

/*
 * Fused single-pass sweep (PERF-1): both metrics in one walk over the
 * sampled rows instead of three separate strided sweeps. Exactly
 * equivalent to up_laplacian_variance + up_block_edge_strength (which
 * remain the reference oracles for tests and fuzzing): the same rows,
 * samples, and 64-bit sums, just interleaved.
 */
typedef struct {
    uint64_t lap_sum;
    uint64_t lap_n;
    uint64_t edge_sum;
    uint64_t edge_n;
} up_probe_metrics_t;

static inline void up_probe__lap_row(const uint8_t *plane, int stride,
                                     int w, int h, int y,
                                     up_probe_metrics_t *m)
{
    if (y < UP_PROBE_GRID_STEP || y + 1 >= h)
        return;
    const uint8_t *row    = plane + (size_t)y * (size_t)stride;
    const uint8_t *row_up = row - stride;
    const uint8_t *row_dn = row + stride;
    for (int x = UP_PROBE_GRID_STEP; x < w - 1; x += UP_PROBE_GRID_STEP) {
        int lap = 4 * row[x] - (row_up[x] + row_dn[x] + row[x - 1] + row[x + 1]);
        m->lap_sum += (uint64_t)(lap * lap);
        m->lap_n++;
    }
}

static inline void up_probe__edge_rows(const uint8_t *plane, int stride,
                                       int w, int h, int y,
                                       up_probe_metrics_t *m)
{
    if (w <= UP_PROBE_BLOCK_SIZE || h <= UP_PROBE_BLOCK_SIZE)
        return;
    const uint8_t *row = plane + (size_t)y * (size_t)stride;
    for (int x = UP_PROBE_BLOCK_SIZE; x < w; x += UP_PROBE_BLOCK_SIZE) {
        int diff = (int)row[x] - (int)row[x - 1];
        m->edge_sum += (uint64_t)(diff < 0 ? -diff : diff);
        m->edge_n++;
    }
    if (y < UP_PROBE_BLOCK_SIZE || y % UP_PROBE_BLOCK_SIZE != 0)
        return;
    const uint8_t *row_up = row - stride;
    for (int x = 0; x < w; x += UP_PROBE_GRID_STEP) {
        int diff = (int)row[x] - (int)row_up[x];
        m->edge_sum += (uint64_t)(diff < 0 ? -diff : diff);
        m->edge_n++;
    }
}

static inline void up_probe_metrics(const uint8_t *plane, int stride,
                                    int w, int h, up_probe_metrics_t *m)
{
    *m = (up_probe_metrics_t){ 0, 0, 0, 0 };
    if (plane == NULL || stride <= 0 || stride < w)
        return;
    for (int y = 0; y < h; y += UP_PROBE_GRID_STEP) {
        up_probe__lap_row(plane, stride, w, h, y, m);
        up_probe__edge_rows(plane, stride, w, h, y, m);
    }
}

/*
 * Probe accumulator. Tracks metric averages over the probe window so a
 * single noisy frame doesn't bias the decision. Initialize to zero and
 * call up_probe_observe() for each frame in the window.
 */
typedef struct {
    uint64_t lap_sum;       /* sum of squared Laplacian responses */
    uint64_t lap_samples;   /* total laplacian samples */
    uint64_t edge_sum;      /* sum of block-edge intensities */
    uint64_t edge_samples;  /* total block-edge samples */
    int      frames;        /* number of frames observed */
} up_probe_accum_t;

/*
 * Add one frame's metrics to the accumulator. Call this after computing
 * the metrics for a frame's luma plane. Zero metrics still advance the
 * observed-frame count; invalid production picture views skip this call.
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
 * Advisory rule based on accumulated probe metrics.
 *
 * Returns:
 *   1 -> upscaling is unlikely to help; recommend disabling it next playback
 *   0 -> no advisory
 *
 * Decision logic:
 *
 *   - If we observed too few samples to be confident, return 0
 *     (do not recommend a change).
 *
 *   - If the source is very soft (mean squared Laplacian response < THRESH_SOFT)
 *     AND it's also very blocky (mean block-edge intensity > THRESH_BLOCKY),
 *     we have a heavily-compressed soft source: upscaling will amplify
 *     the blocking without any detail to recover. Recommend bypassing.
 *
 *   - If the source is just very soft (no detail to recover) but NOT
 *     blocky, upscaling may still produce a passable smooth output —
 *     do not warn on softness alone.
 *
 *   - If the source is just blocky but has detail elsewhere, the user
 *     probably still wants upscaling — do not warn on blockiness alone.
 *
 * Both conditions in conjunction are the "actively bad to upscale" case.
 *
 * The conservative advisory requires both low detail energy and elevated
 * block-edge intensity. Laplacian thresholds use the same
 * `lap_sum / lap_samples` units RunProbe logs and are compared directly.
 */
#define UP_PROBE_THRESH_SOFT_LAP_MEAN    400   /* below = "very soft" */
#define UP_PROBE_THRESH_BLOCKY_EDGE_MEAN  6    /* above = "very blocky" */
#define UP_PROBE_MIN_FRAMES               10   /* need this many frames */
#define UP_PROBE_MIN_SAMPLES_PER_KIND  20000   /* need this many samples */

/* Bounded observation window. Must stay at least UP_PROBE_MIN_FRAMES or the
 * bypass advisory could never fire. */
#define UP_PROBE_WINDOW_FRAMES 60
_Static_assert(UP_PROBE_WINDOW_FRAMES >= UP_PROBE_MIN_FRAMES,
               "probe window shorter than the frames the verdict needs");

/* Default sharpness cutoff (sum-of-squared-laplacians per sample, i.e.
 * the same units as `lap_sum / lap_samples`). Above this value the
 * source is considered heavily textured/grainy and USM is skipped to
 * avoid noise amplification. Exposed as the default of the VLC option
 * `--autoupscale-usm-sharp-threshold`. */
#define UP_PROBE_THRESH_SHARP_LAP_MEAN  3500

/*
 * Decide whether USM should be skipped given accumulated probe state
 * and a runtime threshold. threshold <= 0 means the feature is off
 * (always returns 0). Otherwise: returns 1 iff the probe has gathered
 * at least one sample AND mean squared Laplacian response strictly exceeds
 * threshold. Production invokes this after the probe window closes.
 *
 * Used both in production (autoupscale.c, after probe window closes)
 * and in unit tests for VLC-option boundary coverage.
 */
static inline int up_should_skip_usm_for_sharpness(const up_probe_accum_t *a,
                                                   int threshold)
{
    if (a == NULL) return 0;
    if (threshold <= 0) return 0;            /* feature disabled */
    if (a->lap_samples == 0) return 0;       /* no luma samples */
    uint64_t lap_mean = a->lap_sum / a->lap_samples;
    return (lap_mean > (uint64_t)threshold) ? 1 : 0;
}

static inline int up_should_bypass_for_content(const up_probe_accum_t *a)
{
    if (a == NULL) return 0;
    if (a->frames < UP_PROBE_MIN_FRAMES) return 0;
    if (a->lap_samples  < UP_PROBE_MIN_SAMPLES_PER_KIND) return 0;
    if (a->edge_samples < UP_PROBE_MIN_SAMPLES_PER_KIND) return 0;

    uint64_t lap_mean  = a->lap_sum  / a->lap_samples;
    uint64_t edge_mean = a->edge_sum / a->edge_samples;

    int very_soft  = (lap_mean  < (uint64_t)UP_PROBE_THRESH_SOFT_LAP_MEAN);
    int very_blocky = (edge_mean > (uint64_t)UP_PROBE_THRESH_BLOCKY_EDGE_MEAN);

    return (very_soft && very_blocky) ? 1 : 0;
}

#endif /* AUTOUPSCALE_CONTENT_PROBE_H */
