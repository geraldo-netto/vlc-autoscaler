// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef AUTOUPSCALE_BARRIER_FAULT_INJECT_H
#define AUTOUPSCALE_BARRIER_FAULT_INJECT_H

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <time.h>

#include "../src/threading.h"

static atomic_int g_fail_next_done_wait;
static atomic_int g_fail_next_done_signal;
static atomic_int g_done_timedwait_seen;
static atomic_int g_suppress_next_broadcast;
static atomic_int g_fail_next_broadcast;
static atomic_int g_fail_next_mutex_lock;
static atomic_int g_fail_next_worker_mutex_lock;
static pthread_t  g_mutex_lock_target;
static pthread_t  g_mutex_lock_excluded;

int __real_pthread_cond_timedwait(pthread_cond_t *, pthread_mutex_t *,
                                  const struct timespec *);
int __wrap_pthread_cond_timedwait(pthread_cond_t *cond,
                                  pthread_mutex_t *mutex,
                                  const struct timespec *deadline)
{
    atomic_store_explicit(&g_done_timedwait_seen, 1, memory_order_relaxed);
    if (atomic_exchange_explicit(&g_fail_next_done_wait, 0,
                                 memory_order_relaxed))
        return EINVAL;
    return __real_pthread_cond_timedwait(cond, mutex, deadline);
}

int __real_pthread_cond_signal(pthread_cond_t *);
int __wrap_pthread_cond_signal(pthread_cond_t *cond)
{
    const int rc = __real_pthread_cond_signal(cond);
    return atomic_exchange_explicit(&g_fail_next_done_signal, 0,
                                    memory_order_relaxed) ? EINVAL : rc;
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

static inline void barrier_fault_inject_next_done_wait(void)
{
    atomic_store_explicit(&g_fail_next_done_wait, 1, memory_order_relaxed);
}

static inline void barrier_fault_inject_next_done_signal(void)
{
    atomic_store_explicit(&g_fail_next_done_signal, 1, memory_order_relaxed);
}

static inline int barrier_fault_used_timed_completion_wait(void)
{
    return atomic_load_explicit(&g_done_timedwait_seen, memory_order_relaxed);
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
        && atomic_load_explicit(&g_fail_next_done_wait,
                                memory_order_relaxed) == 0
        && atomic_load_explicit(&g_fail_next_done_signal,
                                memory_order_relaxed) == 0;
}

#endif
