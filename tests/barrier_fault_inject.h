// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef AUTOUPSCALE_BARRIER_FAULT_INJECT_H
#define AUTOUPSCALE_BARRIER_FAULT_INJECT_H

#include <errno.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdatomic.h>

static atomic_int g_fail_next_sem_wait;
static atomic_int g_fail_next_sem_post;
static atomic_int g_suppress_next_broadcast;

int __real_sem_wait(sem_t *sem);
int __wrap_sem_wait(sem_t *sem)
{
    if (atomic_exchange_explicit(&g_fail_next_sem_wait, 0,
                                 memory_order_relaxed)) {
        errno = EINVAL;
        return -1;
    }
    return __real_sem_wait(sem);
}

/* CON-2: emulate sem_post's only real failure mode, EOVERFLOW — the
 * count is already positive, so the waiter still wakes. Post for real,
 * then report failure. */
int __real_sem_post(sem_t *sem);
int __wrap_sem_post(sem_t *sem)
{
    if (atomic_exchange_explicit(&g_fail_next_sem_post, 0,
                                 memory_order_relaxed)) {
        (void)__real_sem_post(sem);
        errno = EOVERFLOW;
        return -1;
    }
    return __real_sem_post(sem);
}

static inline void barrier_fault_inject_next_sem_post(void)
{
    atomic_store_explicit(&g_fail_next_sem_post, 1, memory_order_relaxed);
}

int __real_pthread_cond_broadcast(pthread_cond_t *cond);
int __wrap_pthread_cond_broadcast(pthread_cond_t *cond)
{
    if (atomic_exchange_explicit(&g_suppress_next_broadcast, 0,
                                 memory_order_relaxed))
        return 0;
    return __real_pthread_cond_broadcast(cond);
}

static inline void barrier_fault_inject_next_dispatch(void)
{
    atomic_store_explicit(&g_suppress_next_broadcast, 1,
                          memory_order_relaxed);
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
