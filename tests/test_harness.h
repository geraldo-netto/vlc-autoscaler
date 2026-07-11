// SPDX-License-Identifier: GPL-2.0-or-later
/*****************************************************************************
 * test_harness.h — shared mini-harness for the unit-test suites (DUP-2)
 *****************************************************************************
 * Every suite is a standalone binary: BEGIN/END bracket one named test,
 * CHECK/CHECK_EQ record failures without aborting (so one run reports every
 * broken invariant), and main ends with `return test_harness_report();`.
 * State is per-translation-unit statics — one suite per binary by design.
 *****************************************************************************/
#ifndef TEST_HARNESS_H
#define TEST_HARNESS_H

#include <stdio.h>

static int g_run = 0, g_fail = 0, g_cur_fail = 0;
static const char *g_cur = NULL;

#define BEGIN(name) do { g_cur = name; g_cur_fail = 0; g_run++; } while (0)
#define END() do { \
        if (g_cur_fail) { g_fail++; printf("  [FAIL] %s\n", g_cur); } \
        else            { printf("  [ ok ] %s\n", g_cur); } \
    } while (0)
#define CHECK(cond) do { \
        if (!(cond)) { \
            printf("    %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            g_cur_fail = 1; \
        } \
    } while (0)
#define CHECK_EQ(a, b) do { \
        long long _a = (long long)(a), _b = (long long)(b); \
        if (_a != _b) { \
            printf("    %s:%d: %s (=%lld) != %s (=%lld)\n", \
                   __FILE__, __LINE__, #a, _a, #b, _b); \
            g_cur_fail = 1; \
        } \
    } while (0)

static inline int test_harness_report(void)
{
    printf("\n%d tests run, %d failed\n", g_run, g_fail);
    return g_fail == 0 ? 0 : 1;
}

#endif /* TEST_HARNESS_H */
