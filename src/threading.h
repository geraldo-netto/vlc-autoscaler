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
#include <pthread.h>
#include <sched.h>
#include <semaphore.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>
#include <time.h>       /* clock_gettime, timespec */
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
 * CONC-1: the done-barrier's wait is BOUNDED.
 *
 * The final wait runs on VLC's video-output thread. It used to be an untimed
 * sem_wait, which made every "the post never arrived" state a permanent
 * playback hang with no diagnostic: the last finisher posts exactly once, and
 * if that post fails the flag it sets can only be read *after* a wait that
 * never returns. The same is true of any lost wake. A hang is not a recovery —
 * time the wait out instead, so the caller can poison the pool, drop the frame
 * and fail over to a backend that works.
 *
 * The timeout is orders of magnitude above any real dispatch (a worker handles
 * one stripe of one frame: tens of microseconds to a few milliseconds, and
 * even a 64-thread 8K sweep under TSAN stays far below a second), so it can
 * only fire on a genuinely broken barrier.
 *
 * CLOCK_MONOTONIC keeps wall-clock corrections from extending recovery. It
 * deliberately stops during suspend on Linux: sleeping the machine pauses the
 * dispatch budget instead of retiring a healthy backend immediately on resume.
 */
#ifndef UP_POOL_BARRIER_TIMEOUT_MS
#define UP_POOL_BARRIER_TIMEOUT_MS 10000
#endif

#define UP_NSEC_PER_SEC 1000000000L
#define UP_POOL_BARRIER_POLL_NS 100000L

#ifndef UP_HAVE_SEM_CLOCKWAIT
#ifdef __GLIBC_PREREQ
#if __GLIBC_PREREQ(2, 30) && defined(__USE_GNU)
#define UP_HAVE_SEM_CLOCKWAIT 1
#else
#define UP_HAVE_SEM_CLOCKWAIT 0
#endif
#else
#define UP_HAVE_SEM_CLOCKWAIT 0
#endif
#endif

static inline int up__deadline_after_ms(struct timespec *deadline,
                                        long timeout_ms)
{
    if (deadline == NULL || timeout_ms < 0) {
        errno = EINVAL;
        return -1;
    }
    if (clock_gettime(CLOCK_MONOTONIC, deadline) != 0) return -1;

    const long seconds = timeout_ms / 1000;
    const time_t delta = (time_t)seconds;
    if ((long)delta != seconds
        || __builtin_add_overflow(deadline->tv_sec, delta,
                                  &deadline->tv_sec)) {
        errno = EOVERFLOW;
        return -1;
    }

    deadline->tv_nsec += (timeout_ms % 1000) * 1000000L;
    if (deadline->tv_nsec < UP_NSEC_PER_SEC) return 0;
    deadline->tv_nsec -= UP_NSEC_PER_SEC;
    if (__builtin_add_overflow(deadline->tv_sec, (time_t)1,
                               &deadline->tv_sec)) {
        errno = EOVERFLOW;
        return -1;
    }
    return 0;
}

static inline int up__timespec_compare(const struct timespec *left,
                                       const struct timespec *right)
{
    if (left->tv_sec < right->tv_sec) return -1;
    if (left->tv_sec > right->tv_sec) return 1;
    if (left->tv_nsec < right->tv_nsec) return -1;
    return left->tv_nsec > right->tv_nsec;
}

#if !UP_HAVE_SEM_CLOCKWAIT

static inline void up__next_poll_deadline(const struct timespec *now,
                                          const struct timespec *deadline,
                                          struct timespec *next)
{
    *next = *now;
    if (next->tv_nsec <= UP_NSEC_PER_SEC - 1 - UP_POOL_BARRIER_POLL_NS) {
        next->tv_nsec += UP_POOL_BARRIER_POLL_NS;
    } else if (next->tv_sec < deadline->tv_sec) {
        next->tv_sec++;
        next->tv_nsec -= UP_NSEC_PER_SEC - UP_POOL_BARRIER_POLL_NS;
    } else {
        *next = *deadline;
        return;
    }
    if (up__timespec_compare(next, deadline) > 0) *next = *deadline;
}

static inline int up__clock_nanosleep_until(const struct timespec *deadline)
{
    int rc;
    do {
        rc = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, deadline, NULL);
    } while (rc == EINTR);
    if (rc == 0) return 0;
    errno = rc;
    return -1;
}

static inline int up__sem_poll_until(sem_t *s,
                                     const struct timespec *deadline)
{
    for (;;) {
        if (sem_trywait(s) == 0) return 0;
        const int wait_errno = errno;
        if (wait_errno != EAGAIN && wait_errno != EINTR) return -1;

        struct timespec now;
        if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return -1;
        if (up__timespec_compare(&now, deadline) >= 0) {
            errno = ETIMEDOUT;
            return -1;
        }
        if (wait_errno == EINTR) continue;

        struct timespec next;
        up__next_poll_deadline(&now, deadline, &next);
        if (up__clock_nanosleep_until(&next) != 0) return -1;
    }
}

#endif

/*
 * Monotonic semaphore wait with the standard EINTR retry (CON-3). Both worker
 * pools use a counting done-barrier whose final wait runs on VLC's video
 * thread; libvlc embedders routinely install non-SA_RESTART signal handlers,
 * and an interrupted wait returning early would let Filter() hand a picture
 * downstream while workers are still writing it. Older libcs poll sem_trywait
 * against the same monotonic deadline in short absolute sleep slices.
 *
 * Returns 0 on success. Any other failure — ETIMEDOUT (no post arrived) or
 * EINVAL (destroyed/corrupt sem) — returns -1, and the caller must FAIL the
 * dispatch rather than proceed with the barrier broken.
 */
static inline int up_sem_wait_timeout(sem_t *s, long timeout_ms)
{
    struct timespec deadline;
    if (up__deadline_after_ms(&deadline, timeout_ms) != 0) return -1;
#if UP_HAVE_SEM_CLOCKWAIT
    int rc;
    do {
        rc = sem_clockwait(s, CLOCK_MONOTONIC, &deadline);
    } while (rc == -1 && errno == EINTR);
    return rc;
#else
    return up__sem_poll_until(s, &deadline);
#endif
}

/*
 * Shared worker-pool dispatch gate (DUP-1). One home for the broadcast
 * wake gate + counting done-barrier both pools (usm_pool.c,
 * scaler_zimg.c) previously maintained as parallel copies.
 *
 * Wake side (SCAL-2): the main thread arms `pending` and bumps
 * `generation` once per dispatch under `lock`, then wakes every worker
 * with ONE pthread_cond broadcast; each worker sleeps until the
 * generation advances past its private seen_gen. Per-frame worker state
 * written by the main thread between dispatches needs no extra fences:
 * the unlock releases it and the worker re-acquires the same lock to
 * observe the new generation.
 *
 * Done side: each worker finishes with an acq_rel fetch_sub on
 * `pending`; those RMWs form a release sequence, so the worker that
 * drives it to zero observes every other worker's writes, and its
 * sem_post -> the main thread's sem_wait publishes them all. The main
 * thread waits exactly once per dispatch (EINTR-retried); a non-EINTR
 * wait failure means the barrier is broken and the caller must poison
 * and stop the pool rather than proceed.
 *
 * The lock/arm/unlock split exists so a pool can reset per-worker state
 * (e.g. result codes) inside the critical section: a worker that wakes
 * must never observe stale values.
 */
typedef struct {
    pthread_mutex_t lock;
    pthread_cond_t  cv;
    uint64_t        generation;   /* bumped per dispatch, guarded by lock */
    bool            exit_requested; /* teardown signal, guarded by lock */
    atomic_int      pending;      /* live workers this dispatch */
    sem_t           all_done;     /* posted once when pending hits 0 */
    atomic_bool     post_failed;  /* CON-2: last finisher's post failed */
    atomic_bool     sync_failed;  /* ERR-1: pthread gate operation failed */
    bool            cv_inited;    /* destroy guards for partial init */
    bool            sem_inited;
} up_pool_gate_t;

static inline int up_pool_gate_init(up_pool_gate_t *g)
{
    g->generation = 0;
    g->exit_requested = false;
    atomic_init(&g->pending, 0);
    atomic_init(&g->post_failed, false);
    atomic_init(&g->sync_failed, false);
    g->cv_inited  = false;
    g->sem_inited = false;
    if (pthread_mutex_init(&g->lock, NULL) != 0) return -1;
    if (pthread_cond_init(&g->cv, NULL) != 0) {
        pthread_mutex_destroy(&g->lock);
        return -1;
    }
    g->cv_inited = true;
    if (sem_init(&g->all_done, 0, 0) != 0) return -1;
    g->sem_inited = true;
    return 0;
}

/* Safe on a zeroed (never-initialized) or partially-initialized gate. */
static inline void up_pool_gate_destroy(up_pool_gate_t *g)
{
    if (g->sem_inited) sem_destroy(&g->all_done);
    if (g->cv_inited) {
        pthread_cond_destroy(&g->cv);
        pthread_mutex_destroy(&g->lock);
    }
    g->sem_inited = false;
    g->cv_inited  = false;
}

/* True once init succeeded far enough that workers may be blocked on the
 * cv (pools spawn threads only after full gate init). */
static inline bool up_pool_gate_ready(const up_pool_gate_t *g)
{
    return g->cv_inited;
}

static inline int up_pool_gate_lock(up_pool_gate_t *g)
{
    return pthread_mutex_lock(&g->lock);
}

/* Arm the done-barrier for n workers and open the gate. Caller holds the
 * lock (up_pool_gate_lock) and has reset any per-worker state. */
static inline void up_pool_gate_arm_locked(up_pool_gate_t *g, int n)
{
    atomic_store_explicit(&g->pending, n, memory_order_relaxed);
    g->generation++;
}

static inline int up_pool_gate_unlock_broadcast(up_pool_gate_t *g)
{
    const int broadcast_rc = pthread_cond_broadcast(&g->cv);
    const int unlock_rc = pthread_mutex_unlock(&g->lock);
    if (broadcast_rc == 0 && unlock_rc == 0) return 0;
    atomic_store_explicit(&g->sync_failed, true, memory_order_release);
    return -1;
}

/* pthread_cond_wait reacquires the mutex before acting on cancellation.
 * Recovery can therefore cancel an otherwise unwakeable waiter only when a
 * cleanup handler releases that reacquired lock. */
static inline void up__pool_gate_cancel_unlock(void *arg)
{
    (void)pthread_mutex_unlock((pthread_mutex_t *)arg);
}

static inline void up__pool_gate_report_worker_failure(up_pool_gate_t *g)
{
    atomic_store_explicit(&g->sync_failed, true, memory_order_release);
    (void)sem_post(&g->all_done);
}

static inline int up__pool_gate_wait_locked(up_pool_gate_t *g,
                                            const uint64_t *seen_gen)
{
    while (g->generation == *seen_gen && !g->exit_requested) {
        const int rc = pthread_cond_wait(&g->cv, &g->lock);
        if (rc != 0) return rc;
    }
    return 0;
}

/*
 * Worker side: block until a new dispatch (returns 1), an exit request with
 * no unseen generation (returns 0), or a pthread gate failure (returns -1).
 * An unseen dispatch
 * is completed before exit so fatal-barrier recovery can join without
 * leaving a partly-written frame. The exit flag lives in the gate and is
 * read under the gate lock; up_pool_gate_request_exit sets it under the
 * same lock before broadcasting. Deferred cancellation is enabled only while
 * blocked here and is disabled again before returning, so owner work cannot be
 * interrupted between its acquire/release invariants.
 */
static inline int up_pool_gate_wait_for_go(up_pool_gate_t *g,
                                           uint64_t *seen_gen)
{
    if (pthread_mutex_lock(&g->lock) != 0) {
        up__pool_gate_report_worker_failure(g);
        return -1;
    }

    int wait_rc = 0;
    int result = -1;
    pthread_cleanup_push(up__pool_gate_cancel_unlock, &g->lock);
    wait_rc = pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
    if (wait_rc == 0) wait_rc = up__pool_gate_wait_locked(g, seen_gen);
    const int cancel_rc = pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
    if (wait_rc == 0 && cancel_rc != 0) wait_rc = cancel_rc;
    if (wait_rc == 0) {
        const bool run = g->generation != *seen_gen;
        *seen_gen = g->generation;
        result = run ? 1 : 0;
    } else {
        up__pool_gate_report_worker_failure(g);
    }
    if (pthread_mutex_unlock(&g->lock) != 0) {
        up__pool_gate_report_worker_failure(g);
        result = -1;
    }
    pthread_cleanup_pop(0);
    return result;
}

/*
 * Teardown side: tell every worker on this gate to finish any unseen
 * generation, then exit. The broadcast does NOT advance generation — an
 * idle worker must not run stale per-frame state during teardown.
 *
 * A gate that never initialized has no workers blocked on it (pools spawn
 * threads only after a successful gate init), so this is a no-op there.
 * Idempotent: a pool that poisons itself mid-playback and is closed later
 * calls it twice.
 */
static inline int up_pool_gate_request_exit(up_pool_gate_t *g)
{
    if (!up_pool_gate_ready(g)) return 0;
    if (pthread_mutex_lock(&g->lock) != 0) {
        atomic_store_explicit(&g->sync_failed, true, memory_order_release);
        return -1;
    }
    g->exit_requested = true;
    const int broadcast_rc = pthread_cond_broadcast(&g->cv);
    const int unlock_rc = pthread_mutex_unlock(&g->lock);
    if (broadcast_rc == 0 && unlock_rc == 0) return 0;
    atomic_store_explicit(&g->sync_failed, true, memory_order_release);
    return -1;
}

/* How many times the last finisher retries a failed post before giving up.
 * A post that keeps failing means the semaphore itself is unusable; the
 * bounded wait (up_sem_wait_timeout) is what keeps that from hanging. */
#define UP_POOL_POST_RETRIES 3

/*
 * Worker side: signal completion. The last finisher posts the barrier.
 *
 * CON-2: sem_post on a valid unnamed semaphore can only fail with EOVERFLOW —
 * the count is already at SEM_VALUE_MAX, so the main thread's wait cannot
 * block — but it does mean the barrier accounting is no longer trustworthy.
 * Record it so wait_all reports the dispatch as broken instead of silently
 * succeeding.
 *
 * CONC-1: retry the post rather than dropping it. The main thread is waiting
 * on exactly one post from exactly one finisher, so a single unretried failure
 * used to mean no post reached the semaphore at all. Retrying costs nothing on
 * the path that never fails, and the failure flag is still recorded either way.
 */
static inline void up_pool_gate_worker_done(up_pool_gate_t *g)
{
    if (atomic_fetch_sub_explicit(&g->pending, 1, memory_order_acq_rel) != 1)
        return;

    for (int try = 0; try < UP_POOL_POST_RETRIES; try++)
        if (sem_post(&g->all_done) == 0) return;

    atomic_store_explicit(&g->post_failed, true, memory_order_release);
}

/* Main-thread side: wait for every worker of this dispatch. Returns 0 on
 * success; non-zero means the barrier is broken (poison + stop) — either a
 * recorded post failure, or no post at all within the timeout (CONC-1). */
static inline int up_pool_gate_wait_all(up_pool_gate_t *g)
{
    if (atomic_load_explicit(&g->sync_failed, memory_order_acquire)) return -1;
    if (up_sem_wait_timeout(&g->all_done, UP_POOL_BARRIER_TIMEOUT_MS) != 0)
        return -1;
    if (atomic_load_explicit(&g->sync_failed, memory_order_acquire)) return -1;
    if (atomic_exchange_explicit(&g->post_failed, false,
                                 memory_order_acquire))
        return -1;
    return 0;
}

#endif /* AUTOUPSCALE_THREADING_H */
