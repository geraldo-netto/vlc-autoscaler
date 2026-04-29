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

/*
 * The detect_cores helper returns sysconf result clamped into a
 * sane range. We can't deterministically test what the OS reports,
 * but we CAN assert it returns something usable: at least 1, no
 * larger than 4 * UP_THREADS_MAX (the documented sanity cap).
 */
static void test_detect_cores_invariants(void)
{
    BEGIN("up_detect_cores: returns a usable int in [1, 4*UP_THREADS_MAX]");
    int c = up_detect_cores();
    if (c < 1) {
        printf("    detect_cores returned %d (< 1)\n", c);
        g_cur_fail = 1;
    }
    if (c > UP_THREADS_MAX * 4) {
        printf("    detect_cores returned %d (> 4*MAX = %d)\n",
               c, UP_THREADS_MAX * 4);
        g_cur_fail = 1;
    }
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
    test_detect_cores_invariants();
    test_detect_then_decide();
    test_explicit_at_max_boundary();

    printf("\n%d tests run, %d failed\n", g_run, g_fail);
    return g_fail == 0 ? 0 : 1;
}
