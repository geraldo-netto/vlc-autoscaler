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
static atomic_int g_interrupt_next_sem_wait;
static atomic_int g_interrupt_every_sem_wait;
static atomic_int g_sem_clockwait_seen;
static atomic_int g_sem_clockwait_wrong_clock;
static atomic_int g_fail_sem_post_left;    /* consecutive posts still to fail */
static atomic_int g_suppress_next_broadcast;
static atomic_int g_fail_next_broadcast;
static atomic_int g_fail_next_mutex_lock;
static atomic_int g_fail_next_worker_mutex_lock;
static pthread_t  g_mutex_lock_target;
static pthread_t  g_mutex_lock_excluded;

static inline int barrier_fault_injected_wait_errno(void)
{
    if (atomic_load_explicit(&g_interrupt_every_sem_wait,
                             memory_order_relaxed))
        return EINTR;
    if (atomic_exchange_explicit(&g_interrupt_next_sem_wait, 0,
                                 memory_order_relaxed))
        return EINTR;
    if (atomic_exchange_explicit(&g_fail_next_sem_wait, 0,
                                 memory_order_relaxed))
        return EINVAL;
    return 0;
}

/* Intercept the wait primitive selected by threading.h. EINVAL stands in for
 * a destroyed semaphore; EINTR verifies that the original deadline is reused. */
#if UP_HAVE_SEM_CLOCKWAIT
int __real_sem_clockwait(sem_t *sem, clockid_t clock,
                         const struct timespec *abstime);
int __wrap_sem_clockwait(sem_t *sem, clockid_t clock,
                         const struct timespec *abstime)
{
    atomic_store_explicit(&g_sem_clockwait_seen, 1, memory_order_relaxed);
    if (clock != CLOCK_MONOTONIC)
        atomic_store_explicit(&g_sem_clockwait_wrong_clock, 1,
                              memory_order_relaxed);
    const int injected = barrier_fault_injected_wait_errno();
    if (injected != 0) {
        errno = injected;
        return -1;
    }
    return __real_sem_clockwait(sem, clock, abstime);
}
#else
int __real_sem_trywait(sem_t *sem);
int __wrap_sem_trywait(sem_t *sem)
{
    const int injected = barrier_fault_injected_wait_errno();
    if (injected != 0) {
        errno = injected;
        return -1;
    }
    return __real_sem_trywait(sem);
}
#endif

/* A failed attempt must not create an extra synthetic post: a transient burst
 * eventually posts exactly once on its successful retry, while a persistent
 * burst exercises the bounded no-post recovery path deterministically. */
int __real_sem_post(sem_t *sem);
int __wrap_sem_post(sem_t *sem)
{
    if (atomic_load_explicit(&g_fail_sem_post_left, memory_order_relaxed) > 0) {
        atomic_fetch_sub_explicit(&g_fail_sem_post_left, 1,
                                  memory_order_relaxed);
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
    atomic_store_explicit(&g_fail_sem_post_left, n, memory_order_relaxed);
}

static inline void barrier_fault_inject_next_sem_post(void)
{
    barrier_fault_inject_sem_post_failures(UP_POOL_POST_RETRIES);
}

int __real_pthread_cond_broadcast(pthread_cond_t *cond);
int __wrap_pthread_cond_broadcast(pthread_cond_t *cond)
{
    if (atomic_exchange_explicit(&g_fail_next_broadcast, 0,
                                 memory_order_relaxed))
        return EINVAL;
    if (atomic_exchange_explicit(&g_suppress_next_broadcast, 0,
                                 memory_order_relaxed))
        return 0;
    return __real_pthread_cond_broadcast(cond);
}

int __real_pthread_mutex_lock(pthread_mutex_t *mutex);
int __wrap_pthread_mutex_lock(pthread_mutex_t *mutex)
{
    if (atomic_load_explicit(&g_fail_next_mutex_lock, memory_order_acquire)
        && pthread_equal(pthread_self(), g_mutex_lock_target)
        && atomic_exchange_explicit(&g_fail_next_mutex_lock, 0,
                                    memory_order_relaxed))
        return EINVAL;
    if (atomic_load_explicit(&g_fail_next_worker_mutex_lock,
                             memory_order_acquire)
        && !pthread_equal(pthread_self(), g_mutex_lock_excluded)
        && atomic_exchange_explicit(&g_fail_next_worker_mutex_lock, 0,
                                    memory_order_relaxed))
        return EINVAL;
    return __real_pthread_mutex_lock(mutex);
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
    atomic_store_explicit(&g_fail_next_broadcast, 1, memory_order_relaxed);
}

static inline void barrier_fault_inject_next_mutex_lock(void)
{
    g_mutex_lock_target = pthread_self();
    atomic_store_explicit(&g_fail_next_mutex_lock, 1, memory_order_release);
}

static inline void barrier_fault_inject_next_worker_mutex_lock(void)
{
    g_mutex_lock_excluded = pthread_self();
    atomic_store_explicit(&g_fail_next_worker_mutex_lock, 1,
                          memory_order_release);
}

static inline void barrier_fault_inject_next_sem_wait(void)
{
    atomic_store_explicit(&g_fail_next_sem_wait, 1, memory_order_relaxed);
}

static inline void barrier_fault_interrupt_next_sem_wait(void)
{
    atomic_store_explicit(&g_interrupt_next_sem_wait, 1,
                          memory_order_relaxed);
}

static inline void barrier_fault_interrupt_all_sem_waits(int enabled)
{
    atomic_store_explicit(&g_interrupt_every_sem_wait, enabled,
                          memory_order_relaxed);
}

static inline int barrier_fault_used_monotonic_clock(void)
{
    return atomic_load_explicit(&g_sem_clockwait_seen, memory_order_relaxed)
        && !atomic_load_explicit(&g_sem_clockwait_wrong_clock,
                                 memory_order_relaxed);
}

static inline int barrier_fault_injection_consumed(void)
{
    return atomic_load_explicit(&g_suppress_next_broadcast,
                                memory_order_relaxed) == 0
        && atomic_load_explicit(&g_fail_next_broadcast,
                                memory_order_relaxed) == 0
        && atomic_load_explicit(&g_fail_next_mutex_lock,
                                memory_order_relaxed) == 0
        && atomic_load_explicit(&g_fail_next_worker_mutex_lock,
                                memory_order_relaxed) == 0
        && atomic_load_explicit(&g_fail_next_sem_wait,
                                memory_order_relaxed) == 0
        && atomic_load_explicit(&g_interrupt_next_sem_wait,
                                memory_order_relaxed) == 0
        && atomic_load_explicit(&g_interrupt_every_sem_wait,
                                memory_order_relaxed) == 0;
}

#endif
