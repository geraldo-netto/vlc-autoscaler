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

static void test_auto_typical(void)
{
    BEGIN("auto: typical core counts -> cores - 2");
    CHECK_EQ(up_threads_decide(UP_THREADS_AUTO, 24),  22);
    CHECK_EQ(up_threads_decide(UP_THREADS_AUTO, 16),  14);
    CHECK_EQ(up_threads_decide(UP_THREADS_AUTO,  8),   6);
    CHECK_EQ(up_threads_decide(UP_THREADS_AUTO,  4),   2);
    END();
}

static void test_auto_low_core_count(void)
{
    BEGIN("auto: low core counts clamp at 1");
    CHECK_EQ(up_threads_decide(UP_THREADS_AUTO, 3), 1);
    CHECK_EQ(up_threads_decide(UP_THREADS_AUTO, 2), 1);
    CHECK_EQ(up_threads_decide(UP_THREADS_AUTO, 1), 1);
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
    /* Auto on 200 cores: 198 capped at MAX. */
    CHECK_EQ(up_threads_decide(UP_THREADS_AUTO, 200), UP_THREADS_MAX);
    END();
}

static void test_negative_user_pref_means_auto(void)
{
    BEGIN("negative user_pref is treated as auto");
    CHECK_EQ(up_threads_decide(-1,  24), 22);
    CHECK_EQ(up_threads_decide(-99, 8),  6);
    END();
}

static void test_24_core_request(void)
{
    BEGIN("on a 24-core box, auto uses 22 (the original ask)");
    /* This is the configuration the user explicitly asked about. */
    CHECK_EQ(up_threads_decide(0, 24), 22);
    /* And explicit overrides work. */
    CHECK_EQ(up_threads_decide(1,  24), 1);
    CHECK_EQ(up_threads_decide(12, 24), 12);
    CHECK_EQ(up_threads_decide(24, 24), 24);
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
    test_24_core_request();

    printf("\n%d tests run, %d failed\n", g_run, g_fail);
    return g_fail == 0 ? 0 : 1;
}
