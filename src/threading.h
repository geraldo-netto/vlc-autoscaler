// SPDX-License-Identifier: GPL-2.0-or-later
/*****************************************************************************
 * threading.h - pure thread-count decision logic for AutoUpscale
 *****************************************************************************
 * Header-only with zero VLC/FFmpeg deps. Two functions:
 *
 *   up_detect_cores(): wraps sysconf(_SC_NPROCESSORS_ONLN), clamps the
 *     result into [1, sane upper bound], returns int. Single source of
 *     truth — both DetectHardware (autoupscale.c) and the zimg backend
 *     call this so they can't drift.
 *
 *   up_threads_decide(): pure logic that maps a user preference + a
 *     detected core count to a worker-count decision. Unit-tested.
 *
 * Auto policy (user_pref == UP_THREADS_AUTO):
 *   total_cores / 2 - 2, clamped to the range [1, UP_THREADS_MAX].
 *
 * The "/ 2" reserves half the machine for everything else (VLC's main
 * thread, decoder, encoder, audio, vout, the OS, plus other libraries
 * VLC may pull in). The "- 2" trims further so the upscaler doesn't
 * exactly match the half-line and leaves a small absolute reserve.
 *
 * On a 32-core box the default is 14 threads; on a 16-core laptop it
 * is 6; on small machines (≤ 8 cores) it floors to 1-2 threads. Users
 * who want maximum parallelism can override with --autoupscale-threads.
 *
 * Trade-off: the older policy (cores - 2) gave more parallelism on
 * desktops but oversubscribed cores when the rest of VLC was busy.
 * The new policy is more conservative; explicit override is the
 * escape hatch for users who measured otherwise.
 *
 * Why int (not long)? UP_THREADS_MAX is 64. Anything larger gets
 * clamped. up_detect_cores narrows sysconf's long return to int after
 * the clamping check. Inside up_threads_decide, plain int is enough
 * and more honest about the actual range.
 *****************************************************************************/

#ifndef AUTOUPSCALE_THREADING_H
#define AUTOUPSCALE_THREADING_H

#include <unistd.h>     /* sysconf */

/* User-visible thread-count preset (do NOT renumber). */
#define UP_THREADS_AUTO   0

/* Hard upper bound. Beyond this, scheduling overhead dominates and we
 * would also need to worry about address-space fragmentation from many
 * tmp buffers. 64 is generous and sane. */
#define UP_THREADS_MAX    64

/*
 * Detect the number of online CPU cores. Returns int in [1, INT_MAX/4];
 * never returns 0 or negative. If sysconf fails (very rare), returns 1.
 *
 * The 4*UP_THREADS_MAX cap defends against absurdly large values that
 * could overflow downstream arithmetic (`/2 - 2`, etc.); 256 is far
 * above any realistic CPU count this decade and well above what we'd
 * ever spawn workers for.
 */
static inline int up_detect_cores(void)
{
    long c = sysconf(_SC_NPROCESSORS_ONLN);
    if (c <= 0) return 1;
    if (c > UP_THREADS_MAX * 4) return UP_THREADS_MAX * 4;
    return (int)c;
}

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
 * Examples (auto policy, total_cores / 2 - 2, clamped):
 *   up_threads_decide(0, 64)    -> 30   (auto: 32-2)
 *   up_threads_decide(0, 32)    -> 14   (auto: 16-2)
 *   up_threads_decide(0, 24)    -> 10   (auto: 12-2)
 *   up_threads_decide(0, 16)    ->  6   (auto: 8-2)
 *   up_threads_decide(0,  8)    ->  2   (auto: 4-2)
 *   up_threads_decide(0,  4)    ->  1   (auto: 0, clamp at 1)
 *   up_threads_decide(0,  2)    ->  1
 *   up_threads_decide(0,  1)    ->  1
 *   up_threads_decide(0,  0)    ->  1   (unknown -> single-threaded)
 *   up_threads_decide(8, 24)    ->  8   (explicit)
 *   up_threads_decide(100, 24)  -> 24   (clamped to cores)
 *   up_threads_decide(100, 200) -> 64   (clamped to UP_THREADS_MAX)
 */
static inline int up_threads_decide(int user_pref, int total_cores)
{
    if (total_cores <= 0) total_cores = 1;

    int n;
    if (user_pref <= UP_THREADS_AUTO) {
        /* Auto: half the cores, then -2 for absolute reserve. The
         * reserved cores cover VLC's main thread, decoder/encoder,
         * audio, vout, the OS, plus any libraries VLC pulls in. */
        n = total_cores / 2 - 2;
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

    return n;
}

#endif /* AUTOUPSCALE_THREADING_H */
