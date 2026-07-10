// SPDX-License-Identifier: GPL-2.0-or-later
/*****************************************************************************
 * threading.h - pure thread-count decision logic for AutoUpscale
 *****************************************************************************
 * Header-only with zero VLC/FFmpeg deps. Core helpers:
 *
 *   up_detect_cpu_topology(): reads the process affinity mask and returns
 *     both its usable CPU count and exact CPU IDs for optional pinning.
 *     sysconf(_SC_NPROCESSORS_ONLN) is only a fallback when affinity lookup
 *     fails. up_detect_cores() is the count-only wrapper.
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
 * Users who want a different balance can override with
 * --autoupscale-threads.
 *
 * Trade-off: the older policy (cores - 2) gave more parallelism on
 * desktops but oversubscribed cores when the rest of VLC was busy.
 * The new policy is more conservative; explicit override is the
 * escape hatch for users who measured otherwise.
 *
 * Why int (not long)? UP_THREADS_MAX is 64. Anything larger gets
 * clamped before narrowing. Inside up_threads_decide, plain int is enough
 * and more honest about the actual range.
 *****************************************************************************/

#ifndef AUTOUPSCALE_THREADING_H
#define AUTOUPSCALE_THREADING_H

#include <errno.h>
#include <limits.h>
#include <sched.h>
#include <semaphore.h>
#include <sys/types.h>
#include <unistd.h>     /* sysconf */

/* Affinity-mask topology detection needs the glibc/Linux CPU_*_S macro
 * family (PORT-1). Where <sched.h> doesn't provide it — or when a test
 * forces UP_NO_CPU_AFFINITY — fall back to sysconf-only core counting
 * with no pin IDs, which disables worker pinning downstream. */
#if defined(CPU_ALLOC) && defined(CPU_ALLOC_SIZE) && \
    !defined(UP_NO_CPU_AFFINITY)
#define UP_HAVE_CPU_AFFINITY 1
#else
#define UP_HAVE_CPU_AFFINITY 0
#endif

/* User-visible thread-count preset (do NOT renumber). */
#define UP_THREADS_AUTO   0

/* Hard upper bound on worker, synchronization, and scratch-resource growth. */
#define UP_THREADS_MAX    64
#define UP_CPU_COUNT_MAX  (UP_THREADS_MAX * 4)
#define UP_CPU_ID_LIMIT   8192

typedef struct
{
    int allowed_count;
    int pin_count;
    int pin_ids[UP_THREADS_MAX];
} up_cpu_topology_t;

static inline int up__clamp_core_count(long count)
{
    if (count <= 0) return 1;
    if (count > UP_CPU_COUNT_MAX) return UP_CPU_COUNT_MAX;
    return (int)count;
}

typedef long (*up_sysconf_fn)(int);

/* Shared fallback: count-only topology from sysconf, no pin IDs. */
static inline void up__topology_from_sysconf(up_cpu_topology_t *topology,
                                             up_sysconf_fn getcores)
{
    long fallback = getcores != NULL ? getcores(_SC_NPROCESSORS_ONLN) : 1;
    topology->allowed_count = up__clamp_core_count(fallback);
    topology->pin_count = 0;
}

#if UP_HAVE_CPU_AFFINITY

static inline int up_cpu_topology_from_set(up_cpu_topology_t *topology,
                                            const cpu_set_t *set,
                                            size_t set_size)
{
    if (topology == NULL) return 0;
    *topology = (up_cpu_topology_t){ 0 };
    if (set == NULL) return 0;

    size_t scan_limit = set_size > UP_CPU_ID_LIMIT / CHAR_BIT
        ? UP_CPU_ID_LIMIT : set_size * CHAR_BIT;
    for (size_t cpu = 0; cpu < scan_limit; cpu++) {
        if (!CPU_ISSET_S(cpu, set_size, set)) continue;
        if (topology->pin_count < UP_THREADS_MAX)
            topology->pin_ids[topology->pin_count++] = (int)cpu;
        if (topology->allowed_count < UP_CPU_COUNT_MAX)
            topology->allowed_count++;
    }
    return topology->allowed_count;
}

typedef int (*up_getaffinity_fn)(pid_t, size_t, cpu_set_t *);

/*
 * Detect CPUs available to this process. The allowed count is clamped into
 * [1, 4*UP_THREADS_MAX], and up to UP_THREADS_MAX exact IDs are retained for
 * worker pinning. If sched_getaffinity fails, the count falls back to the
 * host-wide online count and no pin IDs are exposed.
 *
 * The UP_CPU_COUNT_MAX cap defends against absurdly large values that could
 * overflow downstream arithmetic and remains above the worker limit.
 */
static inline void up_detect_cpu_topology_with(up_cpu_topology_t *topology,
                                                up_getaffinity_fn getaffinity,
                                                up_sysconf_fn getcores)
{
    if (topology == NULL) return;
    *topology = (up_cpu_topology_t){ 0 };

    size_t set_size = CPU_ALLOC_SIZE(UP_CPU_ID_LIMIT);
    cpu_set_t *set = CPU_ALLOC(UP_CPU_ID_LIMIT);
    if (set != NULL) {
        CPU_ZERO_S(set_size, set);
        if (getaffinity != NULL &&
            getaffinity(0, set_size, set) == 0 &&
            up_cpu_topology_from_set(topology, set, set_size) > 0) {
            CPU_FREE(set);
            return;
        }
        CPU_FREE(set);
    }

    up__topology_from_sysconf(topology, getcores);
}

static inline void up_detect_cpu_topology(up_cpu_topology_t *topology)
{
    up_detect_cpu_topology_with(topology, sched_getaffinity, sysconf);
}

#else /* !UP_HAVE_CPU_AFFINITY */

static inline void up_detect_cpu_topology(up_cpu_topology_t *topology)
{
    if (topology == NULL) return;
    *topology = (up_cpu_topology_t){ 0 };
    up__topology_from_sysconf(topology, sysconf);
}

#endif /* UP_HAVE_CPU_AFFINITY */

static inline int up_detect_cores(void)
{
    up_cpu_topology_t topology;
    up_detect_cpu_topology(&topology);
    return topology.allowed_count;
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

/*
 * sem_wait with the standard EINTR retry (CON-3). Both worker pools use
 * a counting done-barrier whose final sem_wait runs on VLC's video
 * thread; libvlc embedders routinely install non-SA_RESTART signal
 * handlers, and an EINTR'd wait returning early would let Filter() hand
 * a picture downstream while workers are still writing it.
 *
 * Returns 0 on success. Any non-EINTR failure (EINVAL: destroyed/corrupt
 * sem) returns -1 — the caller must FAIL the dispatch, not proceed with
 * the barrier broken.
 */
static inline int up_sem_wait_nointr(sem_t *s)
{
    int rc;
    do { rc = sem_wait(s); } while (rc == -1 && errno == EINTR);
    return rc;
}

#endif /* AUTOUPSCALE_THREADING_H */
