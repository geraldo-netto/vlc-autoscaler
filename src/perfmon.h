/*****************************************************************************
 * perfmon.h — pure performance-monitoring logic for AutoUpscale
 *****************************************************************************
 * Per-frame processing-time tracker. Maintains an exponentially-weighted
 * moving average (EWMA) of frame times in nanoseconds and decides whether
 * to fire a "you're not keeping up" warning, exactly once per stream.
 *
 * Header-only with zero VLC/FFmpeg deps so it can be unit-tested with the
 * same discipline as upscale_logic.h and usm.h.
 *
 * Design choices:
 *   - Warmup window of WARMUP_FRAMES samples is dropped (cold caches,
 *     codec startup, sws_scale internal setup).
 *   - EWMA alpha = 1/8 (right-shift 3) — half-life of about 5 samples,
 *     fast enough to react to sustained load, slow enough to ignore
 *     single-frame outliers.
 *   - We fire the warning only when at least MIN_FRAMES_FOR_WARN samples
 *     have been recorded AND the EWMA exceeds the budget. After that
 *     `has_warned` is latched; subsequent records return 0.
 *****************************************************************************/

#ifndef AUTOUPSCALE_PERFMON_H
#define AUTOUPSCALE_PERFMON_H

#include <stddef.h>
#include <stdint.h>

/* Tunables — exposed for tests to verify the contract.
 * Don't make these too large or short clips never get a chance to warn. */
#define UP_PERFMON_WARMUP_FRAMES        10
#define UP_PERFMON_MIN_FRAMES_FOR_WARN  30

/* EWMA shift: ewma <- ewma + (sample - ewma) >> ALPHA_SHIFT
 * 3 means alpha = 1/8 = 0.125. Half-life ~5.5 samples. */
#define UP_PERFMON_ALPHA_SHIFT  3

typedef struct
{
    int64_t budget_ns;       /* per-frame budget (e.g. 16,666,667 for 60fps) */
    int64_t ewma_ns;         /* EWMA of frame times, nanoseconds */
    int     samples_seen;    /* total samples recorded */
    int     has_warned;      /* latched once warning fires */
    int     enabled;         /* 0 if monitoring disabled (e.g. target_fps<=0) */
} up_perfmon_t;

/*
 * Initialize a perfmon. target_fps <= 0 disables monitoring (record_ns
 * always returns 0). Sensible target_fps range is 1..240.
 */
static inline void up_perfmon_init(up_perfmon_t *pm, int target_fps)
{
    if (pm == NULL) return;
    pm->ewma_ns      = 0;
    pm->samples_seen = 0;
    pm->has_warned   = 0;
    if (target_fps <= 0) {
        pm->enabled    = 0;
        pm->budget_ns  = 0;
    } else {
        pm->enabled    = 1;
        /* nanoseconds per frame at target_fps */
        pm->budget_ns  = 1000000000LL / (int64_t)target_fps;
    }
}

/*
 * Record one frame's processing time in nanoseconds.
 *
 * Returns 1 the first time the EWMA crosses the budget (after warmup),
 * meaning the caller should emit a warning. Returns 0 otherwise.
 *
 * Negative or zero frame_ns are ignored (treat as "no measurement").
 * After the first 1 return the perfmon stays latched and always returns 0.
 */
static inline int up_perfmon_record_ns(up_perfmon_t *pm, int64_t frame_ns)
{
    if (pm == NULL || !pm->enabled || pm->has_warned) return 0;
    if (frame_ns <= 0) return 0;

    pm->samples_seen++;

    /* Drop warmup samples — initialise EWMA at the end of warmup. */
    if (pm->samples_seen <= UP_PERFMON_WARMUP_FRAMES) {
        if (pm->samples_seen == UP_PERFMON_WARMUP_FRAMES)
            pm->ewma_ns = frame_ns;
        return 0;
    }

    /* Update EWMA: ewma += (sample - ewma) >> ALPHA_SHIFT.
     * Right-shift on signed values is implementation-defined for
     * negatives in C99 but well-defined in C11/C18 as arithmetic shift
     * on twos-complement, which every target we care about uses. We
     * also take the difference into a wider type so it can't overflow. */
    int64_t diff = frame_ns - pm->ewma_ns;
    pm->ewma_ns += diff >> UP_PERFMON_ALPHA_SHIFT;

    /* Need a minimum number of samples before we trust the EWMA. */
    if (pm->samples_seen < UP_PERFMON_MIN_FRAMES_FOR_WARN) return 0;

    if (pm->ewma_ns > pm->budget_ns) {
        pm->has_warned = 1;
        return 1;
    }
    return 0;
}

/* Convenience accessor: current EWMA in microseconds (for nicer log output).
 * Returns 0 if monitoring is disabled or warmup hasn't completed.
 * Warmup completes on the WARMUP_FRAMES-th sample (when EWMA is seeded). */
static inline int64_t up_perfmon_ewma_us(const up_perfmon_t *pm)
{
    if (pm == NULL || !pm->enabled) return 0;
    if (pm->samples_seen < UP_PERFMON_WARMUP_FRAMES) return 0;
    return pm->ewma_ns / 1000;
}

static inline int64_t up_perfmon_budget_us(const up_perfmon_t *pm)
{
    if (pm == NULL || !pm->enabled) return 0;
    return pm->budget_ns / 1000;
}

#endif /* AUTOUPSCALE_PERFMON_H */
