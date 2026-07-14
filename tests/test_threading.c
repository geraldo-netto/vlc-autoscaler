// SPDX-License-Identifier: GPL-2.0-or-later
/*****************************************************************************
 * test_threading.c - unit tests for thread-count decision logic
 *****************************************************************************/

#include "../src/threading.h"

#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "barrier_fault_inject.h"
#include "test_harness.h"

/* Fault injection (linked with -Wl,--wrap=pthread_cond_init): make the next
 * pthread_cond_init fail once so up_pool_gate_init's mutex-cleanup path runs. */
static atomic_int g_fail_next_cond_init;
extern int __real_pthread_cond_init(pthread_cond_t *,
                                    const pthread_condattr_t *);
int __wrap_pthread_cond_init(pthread_cond_t *cond,
                             const pthread_condattr_t *attr)
{
    if (atomic_exchange(&g_fail_next_cond_init, 0))
        return EAGAIN;
    return __real_pthread_cond_init(cond, attr);
}

/*
 * Auto policy: cores/2 - 2, clamped to [1, UP_THREADS_MAX].
 * Walking through the formula at each interesting core count.
 */
static void test_auto_typical(void)
{
    BEGIN("auto: typical core counts -> cores/2 - 2");
    CHECK_EQ(up_threads_decide(UP_THREADS_AUTO, 64), 30);  /* 32-2 */
    CHECK_EQ(up_threads_decide(UP_THREADS_AUTO, 32), 14);  /* 16-2 */
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
    /* Auto on 200 cores: cores/2-2 = 98, capped at UP_THREADS_MAX (64). */
    CHECK_EQ(up_threads_decide(UP_THREADS_AUTO, 200), UP_THREADS_MAX);
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
    BEGIN("32-core target: auto uses 14, explicit overrides work");
    CHECK_EQ(up_threads_decide(UP_THREADS_AUTO, 32), 14);
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

static void gate_worker_start(gate_worker_t *w, up_pool_gate_t *gate)
{
    w->gate        = gate;
    w->seen_gen    = gate->generation;
    atomic_init(&w->runs, 0);
    CHECK_EQ(pthread_create(&w->thread, NULL, gate_worker_main, w), 0);
}

static void test_pool_gate_dispatch_cycles(void)
{
    BEGIN("pool gate: N workers x M dispatches, then clean exit");
    enum { N = 4, M = 25 };
    up_pool_gate_t gate;
    CHECK_EQ(up_pool_gate_init(&gate), 0);
    CHECK_EQ(up_pool_gate_ready(&gate), 1);

    static gate_worker_t ws[N];
    for (int i = 0; i < N; i++)
        gate_worker_start(&ws[i], &gate);

    for (int gen = 0; gen < M; gen++) {
        CHECK_EQ(up_pool_gate_lock(&gate), 0);
        up_pool_gate_arm_locked(&gate, N);
        CHECK_EQ(up_pool_gate_unlock_broadcast(&gate), 0);
        CHECK_EQ(up_pool_gate_wait_all(&gate), 0);
    }

    CHECK_EQ(up_pool_gate_request_exit(&gate), 0);
    for (int i = 0; i < N; i++) {
        pthread_join(ws[i].thread, NULL);
        CHECK_EQ(atomic_load(&ws[i].runs), M);
    }

    up_pool_gate_destroy(&gate);
    up_pool_gate_destroy(&gate);  /* double destroy must be a no-op */
    CHECK_EQ(up_pool_gate_ready(&gate), 0);
    END();
}

static void test_pool_gate_cancel_releases_wait_lock(void)
{
    BEGIN("pool gate: cancelling a waiter releases the gate lock");
    up_pool_gate_t gate;
    CHECK_EQ(up_pool_gate_init(&gate), 0);

    static gate_worker_t w;
    gate_worker_start(&w, &gate);
    CHECK_EQ(pthread_cancel(w.thread), 0);
    CHECK_EQ(pthread_join(w.thread, NULL), 0);
    CHECK_EQ(pthread_mutex_trylock(&gate.lock), 0);
    CHECK_EQ(pthread_mutex_unlock(&gate.lock), 0);

    up_pool_gate_destroy(&gate);
    END();
}

/* The exit contract both pools depend on for fatal-barrier recovery: a
 * worker asked to exit still completes a dispatch it has not yet seen. */
static void test_pool_gate_exit_completes_unseen(void)
{
    BEGIN("pool gate: exit request still completes an unseen dispatch");
    up_pool_gate_t gate;
    CHECK_EQ(up_pool_gate_init(&gate), 0);

    static gate_worker_t w;
    w.gate     = &gate;
    w.seen_gen = gate.generation;
    atomic_init(&w.runs, 0);

    /* Arm a dispatch AND request exit before the worker even starts: it
     * must run the unseen generation exactly once, then exit. */
    CHECK_EQ(up_pool_gate_lock(&gate), 0);
    up_pool_gate_arm_locked(&gate, 1);
    CHECK_EQ(up_pool_gate_unlock_broadcast(&gate), 0);
    CHECK_EQ(up_pool_gate_request_exit(&gate), 0);

    CHECK_EQ(pthread_create(&w.thread, NULL, gate_worker_main, &w), 0);
    CHECK_EQ(up_pool_gate_wait_all(&gate), 0);
    pthread_join(w.thread, NULL);
    CHECK_EQ(atomic_load(&w.runs), 1);

    up_pool_gate_destroy(&gate);
    END();
}

/* CON-2 plumbing: a recorded post failure breaks exactly one dispatch.
 * (sem_post's only real failure is EOVERFLOW, where the count is already
 * positive — the wake still happens, emulated here by posting first.) */
static void test_pool_gate_post_failure_reported(void)
{
    BEGIN("pool gate: recorded post failure breaks the dispatch (CON-2)");
    up_pool_gate_t gate;
    CHECK_EQ(up_pool_gate_init(&gate), 0);

    atomic_store(&gate.post_failed, true);
    sem_post(&gate.all_done);
    CHECK_EQ(up_pool_gate_wait_all(&gate), -1);

    /* One-shot: a clean dispatch afterwards succeeds again. */
    sem_post(&gate.all_done);
    CHECK_EQ(up_pool_gate_wait_all(&gate), 0);

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
 * pthread_cond_init fails, leaving cv/sem uninitialized. The fault is
 * one-shot, so a retry succeeds and yields a fully usable gate. */
static void test_pool_gate_init_cond_failure(void)
{
    BEGIN("gate init cleans up the mutex when cond init fails");
    up_pool_gate_t gate;
    atomic_store(&g_fail_next_cond_init, 1);
    CHECK_EQ(up_pool_gate_init(&gate), -1);
    CHECK_EQ(up_pool_gate_init(&gate), 0);
    up_pool_gate_destroy(&gate);
    END();
}

/* Elapsed wall-clock milliseconds since `start`. */
static long elapsed_ms(const struct timespec *start)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (now.tv_sec - start->tv_sec) * 1000
         + (now.tv_nsec - start->tv_nsec) / 1000000;
}

/*
 * REGRESSION (CONC-1): the done-barrier wait is BOUNDED. Arm a dispatch that
 * no worker will ever complete — exactly what a lost wake or a dropped post
 * leaves behind — and the wait must report a broken barrier, not block.
 *
 * This used to be an untimed sem_wait, so this state was a permanent stall on
 * VLC's video-output thread with nothing logged: the post-failure flag the
 * code inspects for "recovery" can only be read AFTER the wait returns. Before
 * the fix this test does not terminate.
 *
 * Built with UP_POOL_BARRIER_TIMEOUT_MS=200 (THREADING_TEST_CFLAGS) so the
 * deadline costs a fifth of a second rather than the shipped 10 s.
 */
static void test_pool_gate_wait_times_out(void)
{
    BEGIN("pool gate: a dispatch nobody completes times out, never hangs");
    up_pool_gate_t gate;
    CHECK_EQ(up_pool_gate_init(&gate), 0);

    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    CHECK_EQ(up_pool_gate_lock(&gate), 0);
    up_pool_gate_arm_locked(&gate, 1);      /* one worker owed... */
    CHECK_EQ(up_pool_gate_unlock_broadcast(&gate), 0); /* ...and none exists */

    CHECK_EQ(up_pool_gate_wait_all(&gate), -1);
    const long waited = elapsed_ms(&t0);
    CHECK(waited >= UP_POOL_BARRIER_TIMEOUT_MS / 2);   /* it really waited */
    CHECK(waited < 10 * UP_POOL_BARRIER_TIMEOUT_MS);   /* and it really returned */

    up_pool_gate_destroy(&gate);
    END();
}

static void test_pool_gate_wait_error_is_reported(void)
{
    BEGIN("pool gate: a semaphore wait error breaks the dispatch");
    up_pool_gate_t gate;
    CHECK_EQ(up_pool_gate_init(&gate), 0);

    barrier_fault_inject_next_sem_wait();
    CHECK_EQ(up_pool_gate_wait_all(&gate), -1);
    CHECK_EQ(barrier_fault_injection_consumed(), 1);
#if UP_HAVE_SEM_CLOCKWAIT
    CHECK_EQ(barrier_fault_used_monotonic_clock(), 1);
#endif

    up_pool_gate_destroy(&gate);
    END();
}

static void test_pool_gate_wait_retries_interrupt(void)
{
    BEGIN("pool gate: an interrupted wait keeps the monotonic deadline");
    up_pool_gate_t gate;
    CHECK_EQ(up_pool_gate_init(&gate), 0);

    CHECK_EQ(sem_post(&gate.all_done), 0);
    barrier_fault_interrupt_next_sem_wait();
    CHECK_EQ(up_pool_gate_wait_all(&gate), 0);
    CHECK_EQ(barrier_fault_injection_consumed(), 1);
#if UP_HAVE_SEM_CLOCKWAIT
    CHECK_EQ(barrier_fault_used_monotonic_clock(), 1);
#endif

    up_pool_gate_destroy(&gate);
    END();
}

#if !UP_HAVE_SEM_CLOCKWAIT
static void test_sem_poll_interrupts_still_expire(void)
{
    BEGIN("semaphore polling: sustained interrupts cannot extend timeout");
    sem_t sem;
    CHECK_EQ(sem_init(&sem, 0, 0), 0);

    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    barrier_fault_interrupt_all_sem_waits(1);
    const int rc = up_sem_wait_timeout(&sem, 10);
    const int wait_errno = errno;
    barrier_fault_interrupt_all_sem_waits(0);

    CHECK_EQ(rc, -1);
    CHECK_EQ(wait_errno, ETIMEDOUT);
    CHECK(elapsed_ms(&t0) < 1000);
    CHECK_EQ(barrier_fault_injection_consumed(), 1);
    CHECK_EQ(sem_destroy(&sem), 0);
    END();
}
#endif

/*
 * CONC-1, other half: the last finisher RETRIES its post. A transient failure
 * (fewer than UP_POOL_POST_RETRIES in a row) must be recovered — the dispatch
 * succeeds and nothing is recorded as broken. Only a failure that survives
 * every retry (a real EOVERFLOW) breaks the dispatch, which
 * test_pool_gate_post_failure_reported covers.
 */
static void test_pool_gate_post_retry_recovers(void)
{
    BEGIN("pool gate: the finisher retries a transient post failure (CONC-1)");
    up_pool_gate_t gate;
    CHECK_EQ(up_pool_gate_init(&gate), 0);

    static gate_worker_t w;
    gate_worker_start(&w, &gate);

    barrier_fault_inject_sem_post_failures(UP_POOL_POST_RETRIES - 1);
    CHECK_EQ(up_pool_gate_lock(&gate), 0);
    up_pool_gate_arm_locked(&gate, 1);
    CHECK_EQ(up_pool_gate_unlock_broadcast(&gate), 0);
    CHECK_EQ(up_pool_gate_wait_all(&gate), 0);   /* retry got the post through */
    CHECK_EQ(atomic_load(&w.runs), 1);

    CHECK_EQ(up_pool_gate_request_exit(&gate), 0);
    pthread_join(w.thread, NULL);
    up_pool_gate_destroy(&gate);
    END();
}

/*
 * The other end of the retry: a post failure that survives every retry (a real
 * EOVERFLOW — the count is pinned at SEM_VALUE_MAX) is recorded, and the
 * dispatch it belongs to is reported broken rather than silently succeeding.
 * Driven through a real worker, so the flag is set by the finisher itself.
 */
static void test_pool_gate_post_failure_survives_retries(void)
{
    BEGIN("pool gate: a post failure no retry can clear breaks the dispatch");
    up_pool_gate_t gate;
    CHECK_EQ(up_pool_gate_init(&gate), 0);

    static gate_worker_t w;
    gate_worker_start(&w, &gate);

    barrier_fault_inject_next_sem_post();   /* fails every retry */
    CHECK_EQ(up_pool_gate_lock(&gate), 0);
    up_pool_gate_arm_locked(&gate, 1);
    CHECK_EQ(up_pool_gate_unlock_broadcast(&gate), 0);
    CHECK_EQ(up_pool_gate_wait_all(&gate), -1);
    CHECK_EQ(atomic_load(&w.runs), 1);      /* the worker DID run... */

    CHECK_EQ(up_pool_gate_request_exit(&gate), 0);
    pthread_join(w.thread, NULL);
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

    CHECK_EQ(up_pool_gate_init(&gate), 0);
    CHECK_EQ(up_pool_gate_request_exit(&gate), 0);
    CHECK_EQ(gate.exit_requested, 1);
    CHECK_EQ(up_pool_gate_request_exit(&gate), 0);   /* idempotent */
    CHECK_EQ(gate.exit_requested, 1);
    up_pool_gate_destroy(&gate);
    END();
}

int main(void)
{
    printf("Running threading tests...\n");

    test_pool_gate_init_cond_failure();

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
    test_pool_gate_cancel_releases_wait_lock();
    test_pool_gate_exit_completes_unseen();
    test_pool_gate_post_failure_reported();
    test_pool_gate_request_exit_guards();
    test_pool_gate_post_retry_recovers();
    test_pool_gate_post_failure_survives_retries();
    test_pool_gate_wait_times_out();
    test_pool_gate_wait_error_is_reported();
    test_pool_gate_wait_retries_interrupt();
#if !UP_HAVE_SEM_CLOCKWAIT
    test_sem_poll_interrupts_still_expire();
#endif
    test_detect_then_decide();
    test_explicit_at_max_boundary();

    return test_harness_report();
}
