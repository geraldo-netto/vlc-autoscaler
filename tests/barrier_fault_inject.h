// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef AUTOUPSCALE_BARRIER_FAULT_INJECT_H
#define AUTOUPSCALE_BARRIER_FAULT_INJECT_H

#include <errno.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdatomic.h>
#include <time.h>

#include "../src/threading.h"

static atomic_int g_fail_next_sem_wait;
static atomic_int g_fail_sem_post_left;    /* consecutive posts still to fail */
static atomic_int g_sem_post_burst_woke;   /* real post already issued */
static atomic_int g_suppress_next_broadcast;

/* The done barrier waits with a deadline (CONC-1), so that is the call to
 * intercept — EINVAL stands in for a destroyed/corrupt semaphore. */
int __real_sem_timedwait(sem_t *sem, const struct timespec *abstime);
int __wrap_sem_timedwait(sem_t *sem, const struct timespec *abstime)
{
    if (atomic_exchange_explicit(&g_fail_next_sem_wait, 0,
                                 memory_order_relaxed)) {
        errno = EINVAL;
        return -1;
    }
    return __real_sem_timedwait(sem, abstime);
}

/*
 * CON-2: emulate sem_post's only real failure mode, EOVERFLOW — the count is
 * already at SEM_VALUE_MAX, so it stays positive and the waiter still wakes,
 * but no further post can land. The finisher retries the post
 * (UP_POOL_POST_RETRIES), and a real EOVERFLOW fails every one of them: inject
 * a burst, not a single failure, or the retry would paper over the fault the
 * test is asserting on. One real post per burst does the waking.
 */
int __real_sem_post(sem_t *sem);
int __wrap_sem_post(sem_t *sem)
{
    if (atomic_load_explicit(&g_fail_sem_post_left, memory_order_relaxed) > 0) {
        atomic_fetch_sub_explicit(&g_fail_sem_post_left, 1,
                                  memory_order_relaxed);
        if (!atomic_exchange_explicit(&g_sem_post_burst_woke, 1,
                                      memory_order_relaxed))
            (void)__real_sem_post(sem);
        errno = EOVERFLOW;
        return -1;
    }
    return __real_sem_post(sem);
}

/* Fail the next `n` posts. n < UP_POOL_POST_RETRIES models a TRANSIENT
 * failure the finisher's retry recovers from; n >= UP_POOL_POST_RETRIES models
 * a real EOVERFLOW, which no retry can clear. */
static inline void barrier_fault_inject_sem_post_failures(int n)
{
    atomic_store_explicit(&g_sem_post_burst_woke, 0, memory_order_relaxed);
    atomic_store_explicit(&g_fail_sem_post_left, n, memory_order_relaxed);
}

static inline void barrier_fault_inject_next_sem_post(void)
{
    barrier_fault_inject_sem_post_failures(UP_POOL_POST_RETRIES);
}

int __real_pthread_cond_broadcast(pthread_cond_t *cond);
int __wrap_pthread_cond_broadcast(pthread_cond_t *cond)
{
    if (atomic_exchange_explicit(&g_suppress_next_broadcast, 0,
                                 memory_order_relaxed))
        return 0;
    return __real_pthread_cond_broadcast(cond);
}

/* Swallow the next dispatch's wake WITHOUT failing the wait: no worker runs,
 * so no post ever arrives. The bounded wait is the only thing that can end it
 * (CONC-1) — before it, this hung the caller forever. */
static inline void barrier_fault_inject_lose_next_wake(void)
{
    atomic_store_explicit(&g_suppress_next_broadcast, 1,
                          memory_order_relaxed);
}

static inline void barrier_fault_inject_next_dispatch(void)
{
    barrier_fault_inject_lose_next_wake();
    atomic_store_explicit(&g_fail_next_sem_wait, 1, memory_order_relaxed);
}

static inline int barrier_fault_injection_consumed(void)
{
    return atomic_load_explicit(&g_suppress_next_broadcast,
                                memory_order_relaxed) == 0
        && atomic_load_explicit(&g_fail_next_sem_wait,
                                memory_order_relaxed) == 0;
}

#endif
