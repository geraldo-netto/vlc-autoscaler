// SPDX-License-Identifier: GPL-2.0-or-later
/*****************************************************************************
 * test_threading.c - unit tests for thread-count decision logic
 *****************************************************************************/

#include "../src/thread_policy.h"
#include "../src/pool_gate.h"

#include <errno.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "barrier_fault_inject.h"
#include "test_harness.h"

/* Fault injection (linked with -Wl,--wrap=pthread_cond_init): make the next
 * pthread_cond_init fail once so up_pool_gate_init's mutex-cleanup path runs. */
static atomic_int g_fail_cond_init_after;
static atomic_int g_fail_pthread_create_after;
static atomic_int g_monotonic_cond_init;
static atomic_bool g_fail_monotonic_clock;
static atomic_bool g_fake_monotonic_clock;
static struct timespec g_fake_monotonic_time;
extern int __real_pthread_cond_init(pthread_cond_t *,
                                    const pthread_condattr_t *);
int __wrap_pthread_cond_init(pthread_cond_t *cond,
                             const pthread_condattr_t *attr)
{
    const int remaining = atomic_load(&g_fail_cond_init_after);
    if (remaining > 0
        && atomic_fetch_sub(&g_fail_cond_init_after, 1) == 1)
        return EAGAIN;
    if (attr != NULL) {
        clockid_t clock;
        if (pthread_condattr_getclock(attr, &clock) == 0
            && clock == CLOCK_MONOTONIC)
            atomic_store(&g_monotonic_cond_init, 1);
    }
    return __real_pthread_cond_init(cond, attr);
}

int __real_clock_gettime(clockid_t clock_id, struct timespec *time);
int __wrap_clock_gettime(clockid_t clock_id, struct timespec *time)
{
    if (clock_id == CLOCK_MONOTONIC
        && atomic_exchange_explicit(&g_fail_monotonic_clock, false,
                                    memory_order_acquire)) {
        errno = EIO;
        return -1;
    }
    if (clock_id == CLOCK_MONOTONIC
        && atomic_exchange_explicit(&g_fake_monotonic_clock, false,
                                    memory_order_acquire)) {
        *time = g_fake_monotonic_time;
        return 0;
    }
    return __real_clock_gettime(clock_id, time);
}

int __real_pthread_create(pthread_t *, const pthread_attr_t *,
                          void *(*)(void *), void *);
int __wrap_pthread_create(pthread_t *thread, const pthread_attr_t *attr,
                          void *(*start)(void *), void *arg)
{
    const int remaining = atomic_load_explicit(
        &g_fail_pthread_create_after, memory_order_relaxed);
    if (remaining > 0
        && atomic_fetch_sub_explicit(&g_fail_pthread_create_after, 1,
                                     memory_order_relaxed) == 1)
        return EAGAIN;
    return __real_pthread_create(thread, attr, start, arg);
}

/*
 * Auto policy: cores/2 - 2, clamped to [1, UP_THREADS_AUTO_MAX].
 * Walking through the formula at each interesting core count.
 */
static void test_auto_typical(void)
{
    BEGIN("auto: typical core counts -> cores/2 - 2");
    CHECK_EQ(up_threads_decide(UP_THREADS_AUTO, 64), 12);
    CHECK_EQ(up_threads_decide(UP_THREADS_AUTO, 32), 12);
    CHECK_EQ(up_threads_decide(UP_THREADS_AUTO, 24), 10);  /* 12-2 */
    CHECK_EQ(up_threads_decide(UP_THREADS_AUTO, 16),  6);  /*  8-2 */
    CHECK_EQ(up_threads_decide(UP_THREADS_AUTO, 12),  4);  /*  6-2 */
    CHECK_EQ(up_threads_decide(UP_THREADS_AUTO,  8),  2);  /*  4-2 */
    END();
}

static void test_auto_low_core_count(void)
{
    BEGIN("auto: low core counts clamp at 1 (the formula goes <= 0)");
    /* cores/2 - 2: at 6, gives 1 (3-2); at 4 gives 0 -> 1; at 1-3 gives <0 -> 1. */
    CHECK_EQ(up_threads_decide(UP_THREADS_AUTO, 6), 1);  /*  3-2 = 1 */
    CHECK_EQ(up_threads_decide(UP_THREADS_AUTO, 5), 1);  /*  2-2 = 0 -> 1 */
    CHECK_EQ(up_threads_decide(UP_THREADS_AUTO, 4), 1);  /*  2-2 = 0 -> 1 */
    CHECK_EQ(up_threads_decide(UP_THREADS_AUTO, 3), 1);  /*  1-2 = -1 -> 1 */
    CHECK_EQ(up_threads_decide(UP_THREADS_AUTO, 2), 1);  /*  1-2 = -1 -> 1 */
    CHECK_EQ(up_threads_decide(UP_THREADS_AUTO, 1), 1);  /*  0-2 = -2 -> 1 */
    END();
}

static void test_auto_unknown_cores(void)
{
    BEGIN("auto: unknown core count (0 or negative) returns 1");
    CHECK_EQ(up_threads_decide(UP_THREADS_AUTO,  0), 1);
    CHECK_EQ(up_threads_decide(UP_THREADS_AUTO, -1), 1);
    CHECK_EQ(up_threads_decide(UP_THREADS_AUTO, -999), 1);
    END();
}

static void test_explicit_below_cores(void)
{
    BEGIN("explicit: count below cores is honored exactly");
    CHECK_EQ(up_threads_decide(1,  24), 1);
    CHECK_EQ(up_threads_decide(8,  24), 8);
    CHECK_EQ(up_threads_decide(22, 24), 22);
    END();
}

static void test_explicit_clamped_to_cores(void)
{
    BEGIN("explicit: count above cores is clamped to cores");
    CHECK_EQ(up_threads_decide(50, 8),  8);
    CHECK_EQ(up_threads_decide(100, 4), 4);
    END();
}

static void test_explicit_clamped_to_max(void)
{
    BEGIN("explicit: count above UP_THREADS_MAX is clamped");
    /* On hypothetical big-iron with 200 cores. */
    CHECK_EQ(up_threads_decide(200, 200), UP_THREADS_MAX);
    CHECK_EQ(up_threads_decide(1000, 200), UP_THREADS_MAX);
    CHECK_EQ(up_threads_decide(UP_THREADS_AUTO, 200), UP_THREADS_AUTO_MAX);
    END();
}

static void test_negative_user_pref_means_auto(void)
{
    BEGIN("negative user_pref is treated as auto (new formula)");
    CHECK_EQ(up_threads_decide(-1,  24), 10);  /* same as auto */
    CHECK_EQ(up_threads_decide(-99,  8),  2);
    END();
}

/* Representative high-core host: verify the automatic policy and explicit
 * overrides from the same input topology. */
static void test_32_core_target_machine(void)
{
    BEGIN("32-core target: auto uses measured cap, explicit overrides work");
    CHECK_EQ(up_threads_decide(UP_THREADS_AUTO, 32), UP_THREADS_AUTO_MAX);
    CHECK_EQ(up_threads_decide(1,  32),  1);
    CHECK_EQ(up_threads_decide(16, 32), 16);
    CHECK_EQ(up_threads_decide(32, 32), 32);
    CHECK_EQ(up_threads_decide(64, 32), 32);  /* clamped to cores */
    END();
}

static void test_core_count_clamp(void)
{
    BEGIN("core count clamp: fallback values stay in the supported range");
    CHECK_EQ(up__clamp_core_count(-1), 1);
    CHECK_EQ(up__clamp_core_count(0), 1);
    CHECK_EQ(up__clamp_core_count(1), 1);
    CHECK_EQ(up__clamp_core_count(UP_THREADS_MAX * 4),
             UP_THREADS_MAX * 4);
    CHECK_EQ(up__clamp_core_count(UP_THREADS_MAX * 4L + 1),
             UP_THREADS_MAX * 4);
    END();
}

/* Fallback path shared by both build modes (PORT-1): sysconf-only
 * topology has no pin IDs and clamps degenerate core counts. */
static long g_fb_core_count;
static long fb_sysconf(int name)
{
    (void)name;
    return g_fb_core_count;
}

static void test_topology_sysconf_fallback(void)
{
    BEGIN("sysconf-only fallback: clamped count, no pin IDs");
    up_cpu_topology_t topology = { 0 };

    g_fb_core_count = 12;
    up__topology_from_sysconf(&topology, fb_sysconf);
    CHECK_EQ(topology.allowed_count, 12);
    CHECK_EQ(topology.pin_count, 0);

    g_fb_core_count = 0;
    up__topology_from_sysconf(&topology, fb_sysconf);
    CHECK_EQ(topology.allowed_count, 1);

    g_fb_core_count = LONG_MAX;
    up__topology_from_sysconf(&topology, fb_sysconf);
    CHECK_EQ(topology.allowed_count, UP_CPU_COUNT_MAX);

    up__topology_from_sysconf(&topology, NULL);
    CHECK_EQ(topology.allowed_count, 1);

    /* The public entry point works in both modes; without affinity it
     * must report a sane count and zero pin IDs. */
    up_cpu_topology_t detected;
    up_detect_cpu_topology(&detected);
    CHECK_EQ(detected.allowed_count >= 1, 1);
    CHECK_EQ(detected.allowed_count <= UP_CPU_COUNT_MAX, 1);
#if !UP_HAVE_CPU_AFFINITY
    CHECK_EQ(detected.pin_count, 0);
    up_detect_cpu_topology(NULL);  /* must not crash */
#endif
    END();
}

#if UP_HAVE_CPU_AFFINITY

static void test_topology_from_sparse_set(void)
{
    BEGIN("CPU topology: preserves sparse allowed CPU IDs");
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(2, &set);
    CPU_SET(17, &set);
    CPU_SET(63, &set);

    up_cpu_topology_t topology;
    CHECK_EQ(up_cpu_topology_from_set(&topology, &set, sizeof set), 3);
    CHECK_EQ(topology.allowed_count, 3);
    CHECK_EQ(topology.pin_count, 3);
    CHECK_EQ(topology.pin_ids[0], 2);
    CHECK_EQ(topology.pin_ids[1], 17);
    CHECK_EQ(topology.pin_ids[2], 63);
    CHECK_EQ(up_cpu_topology_from_set(NULL, &set, sizeof set), 0);
    CHECK_EQ(up_cpu_topology_from_set(&topology, NULL, sizeof set), 0);
    CHECK_EQ(topology.allowed_count, 0);
    END();
}

static void test_topology_caps_counts(void)
{
    BEGIN("CPU topology: caps pin IDs independently of allowed count");
    cpu_set_t set;
    CPU_ZERO(&set);
    for (int cpu = 0; cpu < UP_CPU_COUNT_MAX + 10; cpu++)
        CPU_SET(cpu, &set);

    up_cpu_topology_t topology;
    up_cpu_topology_from_set(&topology, &set, sizeof set);
    CHECK_EQ(topology.allowed_count, UP_CPU_COUNT_MAX);
    CHECK_EQ(topology.pin_count, UP_THREADS_MAX);
    CHECK_EQ(topology.pin_ids[0], 0);
    CHECK_EQ(topology.pin_ids[UP_THREADS_MAX - 1], UP_THREADS_MAX - 1);
    END();
}

enum mock_affinity_mode {
    MOCK_AFFINITY_SPARSE,
    MOCK_AFFINITY_EMPTY,
    MOCK_AFFINITY_FAIL
};

static enum mock_affinity_mode g_affinity_mode;
static long g_mock_core_count;
static int g_mock_sysconf_calls;

static int mock_getaffinity(pid_t pid, size_t set_size, cpu_set_t *set)
{
    (void)pid;
    CPU_ZERO_S(set_size, set);
    switch (g_affinity_mode) {
        case MOCK_AFFINITY_SPARSE:
            CPU_SET_S(2, set_size, set);
            CPU_SET_S(17, set_size, set);
            CPU_SET_S(4097, set_size, set);
            return 0;
        case MOCK_AFFINITY_EMPTY:
            return 0;
        case MOCK_AFFINITY_FAIL:
            return -1;
    }
    return -1;
}

static long mock_sysconf(int name)
{
    (void)name;
    g_mock_sysconf_calls++;
    return g_mock_core_count;
}

static void test_detect_topology_synthetic(void)
{
    BEGIN("CPU detection: sparse affinity and fallback paths");
    up_cpu_topology_t topology;

    g_affinity_mode = MOCK_AFFINITY_SPARSE;
    g_mock_core_count = 99;
    g_mock_sysconf_calls = 0;
    up_detect_cpu_topology_with(&topology, mock_getaffinity, mock_sysconf);
    CHECK_EQ(topology.allowed_count, 3);
    CHECK_EQ(topology.pin_count, 3);
    CHECK_EQ(topology.pin_ids[0], 2);
    CHECK_EQ(topology.pin_ids[1], 17);
    CHECK_EQ(topology.pin_ids[2], 4097);
    CHECK_EQ(g_mock_sysconf_calls, 0);

    g_affinity_mode = MOCK_AFFINITY_EMPTY;
    g_mock_core_count = 12;
    up_detect_cpu_topology_with(&topology, mock_getaffinity, mock_sysconf);
    CHECK_EQ(topology.allowed_count, 12);
    CHECK_EQ(topology.pin_count, 0);

    g_affinity_mode = MOCK_AFFINITY_FAIL;
    g_mock_core_count = LONG_MAX;
    up_detect_cpu_topology_with(&topology, mock_getaffinity, mock_sysconf);
    CHECK_EQ(topology.allowed_count, UP_CPU_COUNT_MAX);
    CHECK_EQ(topology.pin_count, 0);

    g_mock_core_count = 0;
    up_detect_cpu_topology_with(&topology, NULL, mock_sysconf);
    CHECK_EQ(topology.allowed_count, 1);
    up_detect_cpu_topology_with(&topology, NULL, NULL);
    CHECK_EQ(topology.allowed_count, 1);
    up_detect_cpu_topology_with(NULL, mock_getaffinity, mock_sysconf);
    END();
}

static void test_detect_cores_invariants(void)
{
    BEGIN("CPU detection: matches the process affinity mask");
    size_t set_size = CPU_ALLOC_SIZE(UP_CPU_ID_LIMIT);
    cpu_set_t *set = CPU_ALLOC(UP_CPU_ID_LIMIT);
    if (set == NULL) {
        printf("    CPU_ALLOC failed\n");
        g_cur_fail = 1;
        END();
        return;
    }
    CPU_ZERO_S(set_size, set);
    int affinity_rc = sched_getaffinity(0, set_size, set);
    CHECK_EQ(affinity_rc, 0);

    up_cpu_topology_t topology;
    up_detect_cpu_topology(&topology);
    int c = topology.allowed_count;
    if (c < 1) {
        printf("    detect_cores returned %d (< 1)\n", c);
        g_cur_fail = 1;
    }
    if (c > UP_THREADS_MAX * 4) {
        printf("    detect_cores returned %d (> 4*MAX = %d)\n",
               c, UP_THREADS_MAX * 4);
        g_cur_fail = 1;
    }
    if (affinity_rc == 0) {
        int expected = CPU_COUNT_S(set_size, set);
        CHECK_EQ(c, up__clamp_core_count(expected));
        CHECK_EQ(topology.pin_count,
                 expected < UP_THREADS_MAX ? expected : UP_THREADS_MAX);
        for (int i = 0; i < topology.pin_count; i++)
            CHECK_EQ(CPU_ISSET_S((size_t)topology.pin_ids[i],
                                 set_size, set), 1);
    }
    CHECK_EQ(up_detect_cores(), c);
    up_detect_cpu_topology(NULL);
    CPU_FREE(set);
    END();
}

#endif /* UP_HAVE_CPU_AFFINITY */

/* ---------- shared pool gate (DUP-1) ---------- */

typedef struct {
    up_pool_gate_t *gate;
    uint64_t        seen_gen;
    atomic_int      runs;
    pthread_t       thread;
    bool            started;
} gate_worker_t;

static void *gate_worker_main(void *arg)
{
    gate_worker_t *w = (gate_worker_t *)arg;
    while (up_pool_gate_wait_for_go(w->gate, &w->seen_gen) > 0) {
        atomic_fetch_add_explicit(&w->runs, 1, memory_order_relaxed);
        up_pool_gate_worker_done(w->gate);
    }
    return NULL;
}

static int gate_init_required(up_pool_gate_t *gate)
{
    const int rc = up_pool_gate_init(gate);
    CHECK_EQ(rc, 0);
    return rc;
}

static int gate_arm_required(up_pool_gate_t *gate, int pending)
{
    const int lock_rc = up_pool_gate_lock(gate);
    CHECK_EQ(lock_rc, 0);
    if (lock_rc != 0) return -1;

    up_pool_gate_arm_locked(gate, pending);
    const int unlock_rc = up_pool_gate_unlock_broadcast(gate);
    CHECK_EQ(unlock_rc, 0);
    return unlock_rc;
}

static int gate_dispatch_required(up_pool_gate_t *gate, int pending)
{
    if (gate_arm_required(gate, pending) != 0) return -1;
    const int wait_rc = up_pool_gate_wait_all(gate);
    CHECK_EQ(wait_rc, 0);
    return wait_rc;
}

static void gate_worker_prepare(gate_worker_t *w, up_pool_gate_t *gate)
{
    w->gate        = gate;
    w->seen_gen    = gate->generation;
    w->started     = false;
    atomic_init(&w->runs, 0);
}

static int gate_worker_launch(gate_worker_t *w)
{
    const int rc = pthread_create(&w->thread, NULL, gate_worker_main, w);
    if (rc == 0) w->started = true;
    return rc;
}

static int gate_worker_start(gate_worker_t *w, up_pool_gate_t *gate)
{
    gate_worker_prepare(w, gate);
    return gate_worker_launch(w);
}

static int gate_workers_start(gate_worker_t *workers, int count,
                              up_pool_gate_t *gate, int *create_rc)
{
    int started = 0;
    *create_rc = 0;
    while (started < count) {
        *create_rc = gate_worker_start(&workers[started], gate);
        if (*create_rc != 0) break;
        started++;
    }
    return started;
}

static int join_started_thread(pthread_t thread, bool *started)
{
    if (!*started) return 0;
    const int rc = pthread_join(thread, NULL);
    CHECK_EQ(rc, 0);
    if (rc == 0) *started = false;
    return rc;
}

static int cancel_started_thread(pthread_t thread, bool started)
{
    if (!started) return 0;
    const int rc = pthread_cancel(thread);
    CHECK_EQ(rc, 0);
    return rc;
}

static int gate_workers_cancel(gate_worker_t *workers, int count)
{
    int result = 0;
    for (int i = 0; i < count; i++)
        if (cancel_started_thread(workers[i].thread, workers[i].started) != 0)
            result = -1;
    return result;
}

static int gate_workers_join(gate_worker_t *workers, int count)
{
    int result = 0;
    for (int i = 0; i < count; i++)
        if (join_started_thread(workers[i].thread, &workers[i].started) != 0)
            result = -1;
    return result;
}

static int gate_workers_stop(gate_worker_t *workers, int count,
                             up_pool_gate_t *gate)
{
    const int exit_rc = up_pool_gate_request_exit(gate);
    CHECK_EQ(exit_rc, 0);
    if (exit_rc != 0 && gate_workers_cancel(workers, count) != 0) return -1;
    return gate_workers_join(workers, count);
}

static int mutex_roundtrip(pthread_mutex_t *mutex)
{
    const int lock_rc = pthread_mutex_trylock(mutex);
    CHECK_EQ(lock_rc, 0);
    if (lock_rc != 0) return -1;
    const int unlock_rc = pthread_mutex_unlock(mutex);
    CHECK_EQ(unlock_rc, 0);
    return unlock_rc;
}

static void test_pool_gate_dispatch_cycles(void)
{
    BEGIN("pool gate: N workers x M dispatches, then clean exit");
    enum { N = 4, M = 25 };
    up_pool_gate_t gate = {0};
    if (gate_init_required(&gate) != 0) {
        END();
        return;
    }
    CHECK_EQ(up_pool_gate_ready(&gate), 1);

    gate_worker_t workers[N] = {0};
    int create_rc = 0;
    const int started = gate_workers_start(workers, N, &gate, &create_rc);
    CHECK_EQ(started, N);
    CHECK_EQ(create_rc, 0);
    if (started != N) {
        if (gate_workers_stop(workers, started, &gate) == 0)
            up_pool_gate_destroy(&gate);
        END();
        return;
    }

    for (int gen = 0; gen < M; gen++) {
        if (gate_dispatch_required(&gate, N) != 0) {
            if (gate_workers_stop(workers, N, &gate) == 0)
                up_pool_gate_destroy(&gate);
            END();
            return;
        }
    }

    if (gate_workers_stop(workers, N, &gate) != 0) {
        END();
        return;
    }
    for (int i = 0; i < N; i++)
        CHECK_EQ(atomic_load(&workers[i].runs), M);

    up_pool_gate_destroy(&gate);
    up_pool_gate_destroy(&gate);  /* double destroy must be a no-op */
    CHECK_EQ(up_pool_gate_ready(&gate), 0);
    END();
}

static void test_pool_gate_partial_start_cleanup(void)
{
    BEGIN("pool gate: create failure joins only the successful prefix");
    enum { N = 4, EXPECTED_STARTED = 2 };
    up_pool_gate_t gate = {0};
    if (gate_init_required(&gate) != 0) {
        END();
        return;
    }

    gate_worker_t workers[N] = {0};
    int create_rc = 0;
    atomic_store(&g_fail_pthread_create_after, EXPECTED_STARTED + 1);
    const int started = gate_workers_start(workers, N, &gate, &create_rc);
    CHECK_EQ(started, EXPECTED_STARTED);
    CHECK_EQ(create_rc, EAGAIN);
    CHECK_EQ(atomic_load(&g_fail_pthread_create_after), 0);
    atomic_store(&g_fail_pthread_create_after, 0);

    if (gate_workers_stop(workers, started, &gate) == 0) {
        for (int i = 0; i < N; i++) CHECK_EQ(workers[i].started, false);
        up_pool_gate_destroy(&gate);
    }
    END();
}

static void test_pool_gate_cancel_releases_wait_lock(void)
{
    BEGIN("pool gate: cancelling a waiter releases the gate lock");
    up_pool_gate_t gate = {0};
    if (gate_init_required(&gate) != 0) {
        END();
        return;
    }

    gate_worker_t worker = {0};
    const int create_rc = gate_worker_start(&worker, &gate);
    CHECK_EQ(create_rc, 0);
    if (create_rc != 0) {
        up_pool_gate_destroy(&gate);
        END();
        return;
    }
    const int cancel_rc = cancel_started_thread(worker.thread, worker.started);
    if (cancel_rc != 0) {
        if (gate_workers_stop(&worker, 1, &gate) == 0)
            up_pool_gate_destroy(&gate);
        END();
        return;
    }
    if (join_started_thread(worker.thread, &worker.started) != 0
        || mutex_roundtrip(&gate.lock) != 0) {
        END();
        return;
    }

    up_pool_gate_destroy(&gate);
    END();
}

static void *gate_completion_wait_main(void *arg)
{
    up_pool_gate_t *gate = (up_pool_gate_t *)arg;
    return (void *)(intptr_t)up_pool_gate_wait_all(gate);
}

static void test_pool_gate_cancel_releases_done_lock(void)
{
    BEGIN("pool gate: cancelling completion wait releases its lock");
    up_pool_gate_t gate = {0};
    if (gate_init_required(&gate) != 0) {
        END();
        return;
    }
    if (gate_arm_required(&gate, 1) != 0) {
        END();
        return;
    }

    atomic_store(&g_done_timedwait_seen, 0);
    pthread_t waiter = {0};
    bool waiter_started = false;
    const int create_rc = pthread_create(&waiter, NULL,
                                         gate_completion_wait_main, &gate);
    CHECK_EQ(create_rc, 0);
    if (create_rc != 0) {
        up_pool_gate_destroy(&gate);
        END();
        return;
    }
    waiter_started = true;
    const struct timespec tick = { .tv_sec = 0, .tv_nsec = 1000000 };
    for (int i = 0; i < 1000 && !barrier_fault_used_timed_completion_wait(); i++)
        nanosleep(&tick, NULL);
    CHECK_EQ(barrier_fault_used_timed_completion_wait(), 1);
    (void)cancel_started_thread(waiter, waiter_started);
    if (join_started_thread(waiter, &waiter_started) != 0
        || mutex_roundtrip(&gate.done_lock) != 0) {
        END();
        return;
    }

    up_pool_gate_destroy(&gate);
    END();
}

/* The exit contract both pools depend on for fatal-barrier recovery: a
 * worker asked to exit still completes a dispatch it has not yet seen. */
static void test_pool_gate_exit_completes_unseen(void)
{
    BEGIN("pool gate: exit request still completes an unseen dispatch");
    up_pool_gate_t gate = {0};
    if (gate_init_required(&gate) != 0) {
        END();
        return;
    }

    gate_worker_t worker = {0};
    gate_worker_prepare(&worker, &gate);

    /* Arm a dispatch AND request exit before the worker even starts: it
     * must run the unseen generation exactly once, then exit. */
    if (gate_arm_required(&gate, 1) != 0) {
        END();
        return;
    }
    const int exit_rc = up_pool_gate_request_exit(&gate);
    CHECK_EQ(exit_rc, 0);
    if (exit_rc != 0) {
        END();
        return;
    }

    const int create_rc = gate_worker_launch(&worker);
    CHECK_EQ(create_rc, 0);
    if (create_rc != 0) {
        up_pool_gate_destroy(&gate);
        END();
        return;
    }
    const int wait_rc = up_pool_gate_wait_all(&gate);
    CHECK_EQ(wait_rc, 0);
    if (join_started_thread(worker.thread, &worker.started) != 0) {
        END();
        return;
    }
    CHECK_EQ(atomic_load(&worker.runs), 1);

    up_pool_gate_destroy(&gate);
    END();
}

static void test_pool_gate_signal_failure_reported(void)
{
    BEGIN("pool gate: completion-signal failure breaks the dispatch");
    up_pool_gate_t gate = {0};
    if (gate_init_required(&gate) != 0) {
        END();
        return;
    }

    gate_worker_t worker = {0};
    const int create_rc = gate_worker_start(&worker, &gate);
    CHECK_EQ(create_rc, 0);
    if (create_rc != 0) {
        up_pool_gate_destroy(&gate);
        END();
        return;
    }
    barrier_fault_inject_next_done_signal();
    if (gate_arm_required(&gate, 1) != 0) {
        if (gate_workers_stop(&worker, 1, &gate) == 0)
            up_pool_gate_destroy(&gate);
        END();
        return;
    }

    CHECK_EQ(up_pool_gate_wait_all(&gate), -1);
    CHECK_EQ(barrier_fault_injection_consumed(), 1);
    if (gate_workers_stop(&worker, 1, &gate) != 0) {
        END();
        return;
    }
    CHECK_EQ(atomic_load(&worker.runs), 1);
    up_pool_gate_destroy(&gate);
    END();
}

/*
 * Composition: detect + decide should always produce something the
 * worker pools can handle. This isn't testing the formula again — it's
 * sanity-checking the contract between the two functions.
 */
static void test_detect_then_decide(void)
{
    BEGIN("detect_cores -> threads_decide(AUTO) -> result in [1, MAX]");
    int n = up_threads_decide(UP_THREADS_AUTO, up_detect_cores());
    if (n < 1 || n > UP_THREADS_MAX) {
        printf("    decide(auto, detect()) = %d (out of [1, %d])\n",
               n, UP_THREADS_MAX);
        g_cur_fail = 1;
    }
    END();
}

/*
 * Edge case: user_pref of exactly UP_THREADS_MAX is honored when
 * cores allow, demonstrating the boundary doesn't leak.
 */
static void test_explicit_at_max_boundary(void)
{
    BEGIN("explicit user_pref == UP_THREADS_MAX is honored when cores allow");
    CHECK_EQ(up_threads_decide(UP_THREADS_MAX, 100), UP_THREADS_MAX);
    CHECK_EQ(up_threads_decide(UP_THREADS_MAX, UP_THREADS_MAX), UP_THREADS_MAX);
    /* But not when cores are smaller. */
    CHECK_EQ(up_threads_decide(UP_THREADS_MAX, 32), 32);
    END();
}

/* up_pool_gate_init destroys the mutex it just created and returns -1 when
 * pthread_cond_init fails. The fault is one-shot, so a retry succeeds and
 * initializes the completion condition with CLOCK_MONOTONIC. */
static void test_pool_gate_init_cond_failure(void)
{
    BEGIN("gate init cleans up when either condition init fails");
    for (int nth = 1; nth <= 2; nth++) {
        up_pool_gate_t gate = {0};
        atomic_store(&g_fail_cond_init_after, nth);
        const int failed_rc = up_pool_gate_init(&gate);
        CHECK_EQ(failed_rc, -1);
        if (failed_rc == 0) up_pool_gate_destroy(&gate);
        CHECK_EQ(atomic_load(&g_fail_cond_init_after), 0);
        atomic_store(&g_fail_cond_init_after, 0);

        atomic_store(&g_monotonic_cond_init, 0);
        const int retry_rc = up_pool_gate_init(&gate);
        CHECK_EQ(retry_rc, 0);
        if (retry_rc == 0) {
            CHECK_EQ(atomic_load(&g_monotonic_cond_init), 1);
            up_pool_gate_destroy(&gate);
        }
    }
    END();
}

static time_t test_time_t_max(void)
{
    time_t power = 1;
    time_t doubled;
    while (!__builtin_mul_overflow(power, (time_t)2, &doubled))
        power = doubled;
    return power + (power - 1);
}

static void fake_monotonic_time(time_t seconds, long nanoseconds)
{
    g_fake_monotonic_time.tv_sec = seconds;
    g_fake_monotonic_time.tv_nsec = nanoseconds;
    atomic_store_explicit(&g_fake_monotonic_clock, true,
                          memory_order_release);
}

static void test_deadline_input_and_overflow_checks(void)
{
    BEGIN("monotonic deadline reports clock, input, and overflow failures");
    struct timespec deadline;

    atomic_store_explicit(&g_fail_monotonic_clock, true,
                          memory_order_release);
    errno = 0;
    CHECK_EQ(up__deadline_after_ms(&deadline, 1), -1);
    CHECK_EQ(errno, EIO);
    CHECK_EQ(atomic_load(&g_fail_monotonic_clock), false);
    atomic_store(&g_fail_monotonic_clock, false);

    errno = 0;
    CHECK_EQ(up__deadline_after_ms(NULL, 0), -1);
    CHECK_EQ(errno, EINVAL);
    errno = 0;
    CHECK_EQ(up__deadline_after_ms(&deadline, -1), -1);
    CHECK_EQ(errno, EINVAL);

    const time_t time_max = test_time_t_max();
    fake_monotonic_time(time_max, 0);
    errno = 0;
    CHECK_EQ(up__deadline_after_ms(&deadline, 1000), -1);
    CHECK_EQ(errno, EOVERFLOW);

    fake_monotonic_time(time_max, UP_NSEC_PER_SEC - 1);
    errno = 0;
    CHECK_EQ(up__deadline_after_ms(&deadline, 1), -1);
    CHECK_EQ(errno, EOVERFLOW);
    END();
}

static int monotonic_now_required(struct timespec *time)
{
    const int rc = clock_gettime(CLOCK_MONOTONIC, time);
    CHECK_EQ(rc, 0);
    return rc;
}

static long elapsed_ms(const struct timespec *start,
                       const struct timespec *end)
{
    return (end->tv_sec - start->tv_sec) * 1000
         + (end->tv_nsec - start->tv_nsec) / 1000000;
}

/*
 * REGRESSION (CONC-1): the done-barrier wait is BOUNDED. Arm a dispatch that
 * no worker will ever complete — exactly what a lost wake or a dropped post
 * leaves behind — and the wait must report a broken barrier, not block.
 *
 * The monotonic condition wait must return at the configured deadline even if
 * no completion notification arrives.
 *
 * Built with UP_POOL_BARRIER_TIMEOUT_MS=200 (THREADING_TEST_CFLAGS) so the
 * deadline costs a fifth of a second rather than the shipped 10 s.
 */
static void test_pool_gate_wait_times_out(void)
{
    BEGIN("pool gate: a dispatch nobody completes times out, never hangs");
    up_pool_gate_t gate = {0};
    if (gate_init_required(&gate) != 0) {
        END();
        return;
    }

    struct timespec t0;
    if (monotonic_now_required(&t0) != 0) {
        up_pool_gate_destroy(&gate);
        END();
        return;
    }

    if (gate_arm_required(&gate, 1) != 0) {
        END();
        return;
    }

    atomic_store(&g_done_timedwait_seen, 0);
    CHECK_EQ(up_pool_gate_wait_all(&gate), -1);
    struct timespec t1;
    if (monotonic_now_required(&t1) != 0) {
        up_pool_gate_destroy(&gate);
        END();
        return;
    }
    const long waited = elapsed_ms(&t0, &t1);
    CHECK(waited >= UP_POOL_BARRIER_TIMEOUT_MS / 2);   /* it really waited */
    CHECK(waited < 10 * UP_POOL_BARRIER_TIMEOUT_MS);   /* and it really returned */
    CHECK_EQ(barrier_fault_used_timed_completion_wait(), 1);

    up_pool_gate_destroy(&gate);
    END();
}

/*
 * REGRESSION (CONC-3): a completion signal can race the deadline — the last
 * worker drives pending to 0 and signals in time, yet pthread_cond_timedwait
 * still reports ETIMEDOUT. The wait must observe the finished dispatch and
 * succeed, not poison a healthy pool. The genuine-timeout case (pending
 * still armed) stays a failure — covered by test_pool_gate_wait_times_out.
 */
static void test_pool_gate_timeout_race_completes(void)
{
    BEGIN("pool gate: ETIMEDOUT racing a finished dispatch still succeeds");
    up_pool_gate_t gate = {0};
    if (gate_init_required(&gate) != 0) {
        END();
        return;
    }
    if (gate_arm_required(&gate, 1) != 0) {
        END();
        return;
    }

    barrier_fault_inject_timeout_race(&gate);
    atomic_store(&g_done_timedwait_seen, 0);
    CHECK_EQ(up_pool_gate_wait_all(&gate), 0);
    CHECK_EQ(barrier_fault_injection_consumed(), 1);
    CHECK_EQ(barrier_fault_used_timed_completion_wait(), 1);

    up_pool_gate_destroy(&gate);
    END();
}

static void test_pool_gate_wait_error_is_reported(void)
{
    BEGIN("pool gate: a completion wait error breaks the dispatch");
    up_pool_gate_t gate = {0};
    if (gate_init_required(&gate) != 0) {
        END();
        return;
    }

    if (gate_arm_required(&gate, 1) != 0) {
        END();
        return;
    }
    barrier_fault_inject_next_done_wait();
    atomic_store(&g_done_timedwait_seen, 0);
    CHECK_EQ(up_pool_gate_wait_all(&gate), -1);
    CHECK_EQ(barrier_fault_injection_consumed(), 1);
    CHECK_EQ(barrier_fault_used_timed_completion_wait(), 1);

    up_pool_gate_destroy(&gate);
    END();
}

/* Both pools call request_exit unconditionally from their stop path, which
 * can run before the gate was ever initialized (lazy init failed early) and
 * twice over (poison mid-playback, then Close). Neither may touch the
 * uninitialized mutex or block. */
static void test_pool_gate_request_exit_guards(void)
{
    BEGIN("gate request_exit: no-op on an uninitialized gate, idempotent");
    up_pool_gate_t gate = { 0 };
    CHECK_EQ(up_pool_gate_request_exit(&gate), 0);
    CHECK_EQ(gate.exit_requested, 0);

    if (gate_init_required(&gate) != 0) {
        END();
        return;
    }
    const int first_exit_rc = up_pool_gate_request_exit(&gate);
    CHECK_EQ(first_exit_rc, 0);
    if (first_exit_rc != 0) {
        END();
        return;
    }
    CHECK_EQ(gate.exit_requested, 1);
    const int second_exit_rc = up_pool_gate_request_exit(&gate);
    CHECK_EQ(second_exit_rc, 0);   /* idempotent */
    if (second_exit_rc != 0) {
        END();
        return;
    }
    CHECK_EQ(gate.exit_requested, 1);
    up_pool_gate_destroy(&gate);
    END();
}

/* request_exit shares up_pool_gate_unlock_broadcast (DUP-10): a broadcast
 * failure must report -1 and latch sync_failed so the stop path escalates. */
static void test_pool_gate_request_exit_broadcast_failure(void)
{
    BEGIN("gate request_exit: broadcast failure latches sync_failed");
    up_pool_gate_t gate = { 0 };
    if (gate_init_required(&gate) != 0) {
        END();
        return;
    }
    barrier_fault_inject_next_dispatch();
    CHECK_EQ(up_pool_gate_request_exit(&gate), -1);
    CHECK_EQ(barrier_fault_injection_consumed(), 1);
    CHECK_EQ(atomic_load(&gate.sync_failed), true);
    CHECK_EQ(gate.exit_requested, 1);
    up_pool_gate_destroy(&gate);
    END();
}

int main(void)
{
    printf("Running threading tests...\n");

    test_pool_gate_init_cond_failure();
    test_deadline_input_and_overflow_checks();

    test_auto_typical();
    test_auto_low_core_count();
    test_auto_unknown_cores();
    test_explicit_below_cores();
    test_explicit_clamped_to_cores();
    test_explicit_clamped_to_max();
    test_negative_user_pref_means_auto();
    test_32_core_target_machine();
    test_core_count_clamp();
    test_topology_sysconf_fallback();
#if UP_HAVE_CPU_AFFINITY
    test_topology_from_sparse_set();
    test_topology_caps_counts();
    test_detect_topology_synthetic();
    test_detect_cores_invariants();
#endif
    test_pool_gate_dispatch_cycles();
    test_pool_gate_partial_start_cleanup();
    test_pool_gate_cancel_releases_wait_lock();
    test_pool_gate_cancel_releases_done_lock();
    test_pool_gate_exit_completes_unseen();
    test_pool_gate_signal_failure_reported();
    test_pool_gate_request_exit_guards();
    test_pool_gate_request_exit_broadcast_failure();
    test_pool_gate_wait_times_out();
    test_pool_gate_timeout_race_completes();
    test_pool_gate_wait_error_is_reported();
    test_detect_then_decide();
    test_explicit_at_max_boundary();

    return test_harness_report();
}
