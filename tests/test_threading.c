// SPDX-License-Identifier: GPL-2.0-or-later
/*****************************************************************************
 * test_threading.c - unit tests for thread-count decision logic
 *****************************************************************************/

#include "../src/threading.h"

#include <stdio.h>
#include <stdlib.h>

static int g_run = 0, g_fail = 0, g_cur_fail = 0;
static const char *g_cur = NULL;

#define BEGIN(name) do { g_cur = name; g_cur_fail = 0; g_run++; } while (0)
#define END() do { \
        if (g_cur_fail) { g_fail++; printf("  [FAIL] %s\n", g_cur); } \
        else            { printf("  [ ok ] %s\n", g_cur); } \
    } while (0)
#define CHECK_EQ(a, b) do { \
        long _a = (long)(a), _b = (long)(b); \
        if (_a != _b) { \
            printf("    %s:%d: %s (=%ld) != %s (=%ld)\n", \
                   __FILE__, __LINE__, #a, _a, #b, _b); \
            g_cur_fail = 1; \
        } \
    } while (0)

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

/*
 * 32-core machines are the project's primary deployment target. On
 * the new policy: 32/2 - 2 = 14 worker threads. The remaining 18 cores
 * cover VLC's main thread, decoder, encoder, audio, vout, OS, plus
 * any other libraries VLC pulls in.
 */
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

int main(void)
{
    printf("Running threading tests...\n");

    test_auto_typical();
    test_auto_low_core_count();
    test_auto_unknown_cores();
    test_explicit_below_cores();
    test_explicit_clamped_to_cores();
    test_explicit_clamped_to_max();
    test_negative_user_pref_means_auto();
    test_32_core_target_machine();
    test_core_count_clamp();
    test_topology_from_sparse_set();
    test_topology_caps_counts();
    test_detect_topology_synthetic();
    test_detect_cores_invariants();
    test_detect_then_decide();
    test_explicit_at_max_boundary();

    printf("\n%d tests run, %d failed\n", g_run, g_fail);
    return g_fail == 0 ? 0 : 1;
}
