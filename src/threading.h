// SPDX-License-Identifier: GPL-2.0-or-later
/*****************************************************************************
 * threading.h - pure thread-count decision logic for AutoUpscale
 *****************************************************************************
 * Header-only with zero VLC/FFmpeg deps. Pure function: maps a user
 * preference + detected core count to a sensible thread count.
 *
 * Policy (for user_pref == UP_THREADS_AUTO):
 *   total_cores - 2, clamped to the range [1, UP_THREADS_MAX].
 *
 * The "minus 2" leaves headroom for the decoder thread and the rest of
 * the system (display compositor, audio, OS). On a 24-core box the
 * default is 22 threads; on a 4-core laptop it is 2.
 *****************************************************************************/

#ifndef AUTOUPSCALE_THREADING_H
#define AUTOUPSCALE_THREADING_H

/* User-visible thread-count preset (do NOT renumber). */
#define UP_THREADS_AUTO   0

/* Hard upper bound. Beyond this, scheduling overhead dominates and we
 * would also need to worry about address-space fragmentation from many
 * tmp buffers. 64 is generous and sane. */
#define UP_THREADS_MAX    64

/*
 * Decide how many worker threads to use given user preference and
 * detected core count.
 *
 *   user_pref  : UP_THREADS_AUTO (0) or an explicit count >= 1
 *   total_cores: number of CPU cores reported by the OS (>=1; 0 or
 *                negative are treated as "unknown", default to 1)
 *
 * Returns: at least 1, at most min(total_cores, UP_THREADS_MAX).
 *
 * Examples:
 *   up_threads_decide(0, 24)    -> 22   (auto: 24-2)
 *   up_threads_decide(0, 8)     -> 6    (auto: 8-2)
 *   up_threads_decide(0, 4)     -> 2    (auto: 4-2)
 *   up_threads_decide(0, 2)     -> 1    (auto, clamp at 1)
 *   up_threads_decide(0, 1)     -> 1
 *   up_threads_decide(0, 0)     -> 1    (unknown -> single-threaded)
 *   up_threads_decide(8, 24)    -> 8    (explicit)
 *   up_threads_decide(100, 24)  -> 24   (clamped to cores)
 *   up_threads_decide(100, 200) -> 64   (clamped to UP_THREADS_MAX)
 */
static inline int up_threads_decide(int user_pref, long total_cores)
{
    if (total_cores <= 0) total_cores = 1;

    long n;
    if (user_pref <= UP_THREADS_AUTO) {
        /* Auto: leave 2 cores for the rest of the system. */
        n = total_cores - 2;
        if (n < 1) n = 1;
    } else {
        n = user_pref;
    }

    /* Clamp to total_cores (no benefit going above). */
    if (n > total_cores) n = total_cores;
    /* Clamp to hard upper bound. */
    if (n > UP_THREADS_MAX) n = UP_THREADS_MAX;
    /* Floor at 1. */
    if (n < 1) n = 1;

    return (int)n;
}

#endif /* AUTOUPSCALE_THREADING_H */
