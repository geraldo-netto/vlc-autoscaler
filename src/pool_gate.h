// SPDX-License-Identifier: GPL-2.0-or-later
/*****************************************************************************
 * pool_gate.h - shared worker-pool dispatch gate and completion barrier
 *****************************************************************************
 * Header-only with zero VLC/FFmpeg deps. Split from threading.h (ARCH-3):
 * this half is the pthread synchronization gate consumed via worker_pool.h;
 * CPU topology and worker-count policy live in thread_policy.h.
 *****************************************************************************/

#ifndef AUTOUPSCALE_POOL_GATE_H
#define AUTOUPSCALE_POOL_GATE_H

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <time.h>       /* clock_gettime, timespec */

/*
 * CONC-1: the completion-condition wait is BOUNDED.
 *
 * The final wait runs on VLC's video-output thread. It is timed so a lost
 * completion signal cannot leave that thread waiting forever. Failure poisons
 * the pool; the caller never forwards a picture whose workers may still be
 * writing it.
 *
 * CONC-2: this is not an end-to-end dispatch deadline. Recovery requests exit
 * and joins every worker before releasing storage. Cancellation is disabled
 * while an owner callback runs, so a callback still active at the deadline
 * finishes before the join returns. A hard execution bound would require
 * callback-specific cooperative cancellation or process isolation.
 *
 * The timeout is deliberately orders of magnitude above a normal dispatch to
 * avoid retiring a healthy pool during ordinary scheduling delays.
 *
 * CLOCK_MONOTONIC keeps wall-clock corrections from extending the completion
 * wait. It deliberately stops during suspend on Linux: sleeping the machine
 * pauses the dispatch budget instead of retiring a healthy backend immediately
 * on resume.
 */
#ifndef UP_POOL_BARRIER_TIMEOUT_MS
#define UP_POOL_BARRIER_TIMEOUT_MS 10000
#endif

#define UP_NSEC_PER_SEC 1000000000L

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

/*
 * Shared worker-pool dispatch gate (DUP-1). One home for the broadcast
 * wake gate + completion barrier both pools (usm_pool.c,
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
 * drives it to zero observes every other worker's writes, then signals a
 * dedicated completion condition variable. The main thread acquire-loads
 * `pending == 0`, publishing all worker payloads before the caller reads them.
 * Spurious signals recheck the predicate; a wait failure means the barrier is
 * broken and the caller must poison and stop the pool rather than proceed.
 *
 * The lock/arm/unlock split publishes the pending count and generation before
 * any worker can wake for that dispatch.
 */
typedef struct {
    pthread_mutex_t lock;
    pthread_cond_t  cv;
    pthread_mutex_t done_lock;
    pthread_cond_t  done_cv;
    uint64_t        generation;   /* bumped per dispatch, guarded by lock */
    bool            exit_requested; /* teardown signal, guarded by lock */
    atomic_int      pending;      /* live workers this dispatch */
    atomic_bool     sync_failed;  /* ERR-1: pthread gate operation failed */
    bool            cv_inited;    /* destroy guards for partial init */
    bool            done_inited;
} up_pool_gate_t;

static inline int up__pool_gate_init_done(up_pool_gate_t *g)
{
    if (pthread_mutex_init(&g->done_lock, NULL) != 0) return -1;

    pthread_condattr_t attr;
    int rc = pthread_condattr_init(&attr);
    const bool attr_inited = rc == 0;
    if (rc == 0) rc = pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
    if (rc == 0) rc = pthread_cond_init(&g->done_cv, &attr);
    if (attr_inited) (void)pthread_condattr_destroy(&attr);
    if (rc == 0) {
        g->done_inited = true;
        return 0;
    }
    pthread_mutex_destroy(&g->done_lock);
    return -1;
}

static inline int up_pool_gate_init(up_pool_gate_t *g)
{
    g->generation = 0;
    g->exit_requested = false;
    atomic_init(&g->pending, 0);
    atomic_init(&g->sync_failed, false);
    g->cv_inited   = false;
    g->done_inited = false;
    if (pthread_mutex_init(&g->lock, NULL) != 0) return -1;
    if (pthread_cond_init(&g->cv, NULL) != 0) {
        pthread_mutex_destroy(&g->lock);
        return -1;
    }
    g->cv_inited = true;
    if (up__pool_gate_init_done(g) != 0) {
        pthread_cond_destroy(&g->cv);
        pthread_mutex_destroy(&g->lock);
        g->cv_inited = false;
        return -1;
    }
    return 0;
}

/* Safe on a zeroed (never-initialized) or partially-initialized gate. */
static inline void up_pool_gate_destroy(up_pool_gate_t *g)
{
    if (g->done_inited) {
        pthread_cond_destroy(&g->done_cv);
        pthread_mutex_destroy(&g->done_lock);
    }
    if (g->cv_inited) {
        pthread_cond_destroy(&g->cv);
        pthread_mutex_destroy(&g->lock);
    }
    g->done_inited = false;
    g->cv_inited   = false;
}

/* True once init succeeded far enough that workers may be blocked on the
 * cv (pools spawn threads only after full gate init). */
static inline bool up_pool_gate_ready(const up_pool_gate_t *g)
{
    return g->cv_inited && g->done_inited;
}

static inline int up_pool_gate_lock(up_pool_gate_t *g)
{
    return pthread_mutex_lock(&g->lock);
}

/* Arm the done-barrier for n workers and open the gate. Caller holds the
 * lock (up_pool_gate_lock). */
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

static inline int up__pool_gate_notify_done(up_pool_gate_t *g)
{
    if (pthread_mutex_lock(&g->done_lock) != 0) return -1;
    const int signal_rc = pthread_cond_signal(&g->done_cv);
    if (signal_rc != 0)
        atomic_store_explicit(&g->sync_failed, true, memory_order_release);
    const int unlock_rc = pthread_mutex_unlock(&g->done_lock);
    return signal_rc == 0 && unlock_rc == 0 ? 0 : -1;
}

static inline void up__pool_gate_report_worker_failure(up_pool_gate_t *g)
{
    atomic_store_explicit(&g->sync_failed, true, memory_order_release);
    (void)up__pool_gate_notify_done(g);
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
    return up_pool_gate_unlock_broadcast(g);
}

/* Worker side: the last finisher signals the completion condition. */
static inline void up_pool_gate_worker_done(up_pool_gate_t *g)
{
    if (atomic_fetch_sub_explicit(&g->pending, 1, memory_order_acq_rel) != 1)
        return;
    if (up__pool_gate_notify_done(g) != 0)
        atomic_store_explicit(&g->sync_failed, true, memory_order_release);
}

static inline int up__pool_gate_wait_done_locked(
    up_pool_gate_t *g, const struct timespec *deadline)
{
    for (;;) {
        if (atomic_load_explicit(&g->sync_failed, memory_order_acquire))
            return EINVAL;
        if (atomic_load_explicit(&g->pending, memory_order_acquire) == 0)
            return 0;
        const int rc = pthread_cond_timedwait(&g->done_cv, &g->done_lock,
                                              deadline);
        if (rc == 0 || rc == EINTR) continue;
        /* CONC-3: POSIX lets a completion signal race the deadline and
         * still report ETIMEDOUT; one final predicate check keeps a
         * finished dispatch from poisoning a healthy pool. */
        if (rc == ETIMEDOUT
            && atomic_load_explicit(&g->pending, memory_order_acquire) == 0)
            return 0;
        return rc;
    }
}

/* Main-thread side: wait for every worker of this dispatch. Returns 0 on
 * success; non-zero means the barrier is broken (poison + stop). */
static inline int up_pool_gate_wait_all(up_pool_gate_t *g)
{
    if (atomic_load_explicit(&g->sync_failed, memory_order_acquire)) return -1;
    struct timespec deadline;
    if (up__deadline_after_ms(&deadline, UP_POOL_BARRIER_TIMEOUT_MS) != 0)
        return -1;
    if (pthread_mutex_lock(&g->done_lock) != 0) return -1;
    int wait_rc = 0;
    pthread_cleanup_push(up__pool_gate_cancel_unlock, &g->done_lock);
    wait_rc = up__pool_gate_wait_done_locked(g, &deadline);
    pthread_cleanup_pop(0);
    const int unlock_rc = pthread_mutex_unlock(&g->done_lock);
    if (wait_rc != 0 || unlock_rc != 0) {
        errno = wait_rc != 0 ? wait_rc : unlock_rc;
        return -1;
    }
    /* Acquire the workers' RMW release sequence before reading payloads. */
    if (atomic_load_explicit(&g->pending, memory_order_acquire) != 0) return -1;
    if (atomic_load_explicit(&g->sync_failed, memory_order_acquire)) return -1;
    return 0;
}

#endif /* AUTOUPSCALE_POOL_GATE_H */
