// SPDX-License-Identifier: GPL-2.0-or-later
/*****************************************************************************
 * test_perfmon.c — unit tests for the performance-monitoring logic
 *****************************************************************************/

#include "../src/perfmon.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

/* ---------- init / disabled ---------- */

static void test_init_disabled_for_zero_fps(void)
{
    BEGIN("init: target_fps <= 0 disables monitoring");
    up_perfmon_t pm;
    up_perfmon_init(&pm, 0);
    CHECK_EQ(pm.enabled, 0);
    /* Even huge frame times don't warn when disabled. */
    for (int i = 0; i < 200; i++)
        CHECK_EQ(up_perfmon_record_ns(&pm, 1000000000LL), 0);  /* 1s/frame */

    up_perfmon_init(&pm, -50);
    CHECK_EQ(pm.enabled, 0);
    CHECK_EQ(up_perfmon_record_ns(&pm, 999999999LL), 0);
    END();
}

static void test_init_60fps_budget(void)
{
    BEGIN("init: target_fps=60 -> 16,666,666 ns budget");
    up_perfmon_t pm;
    up_perfmon_init(&pm, 60);
    CHECK_EQ(pm.enabled, 1);
    CHECK_EQ(pm.budget_ns, 16666666LL);

    up_perfmon_init(&pm, 30);
    CHECK_EQ(pm.budget_ns, 33333333LL);

    up_perfmon_init(&pm, 240);
    CHECK_EQ(pm.budget_ns, 4166666LL);
    END();
}

/*
 * VLC declares the option range as 0..240. Defensive coverage: values
 * above the VLC max must still produce a sensible budget without
 * overflow / divide-by-zero. up_perfmon_init computes
 * `1e9 / target_fps`, so any positive int yields a valid budget.
 */
static void test_init_target_fps_oob_boundaries(void)
{
    BEGIN("init: target_fps boundaries beyond VLC range "
          "(241, 1000, INT_MAX) and INT_MIN/-1");
    up_perfmon_t pm;

    /* VLC max + 1: enabled, budget shrinks. */
    up_perfmon_init(&pm, 241);
    CHECK_EQ(pm.enabled, 1);
    CHECK_EQ(pm.budget_ns, 1000000000LL / 241);

    /* Pathologically large positive: still enabled, tiny budget. */
    up_perfmon_init(&pm, 1000);
    CHECK_EQ(pm.enabled, 1);
    CHECK_EQ(pm.budget_ns, 1000000LL);

    up_perfmon_init(&pm, INT_MAX);
    CHECK_EQ(pm.enabled, 1);
    CHECK(pm.budget_ns >= 0);

    /* Negative + INT_MIN: disabled (the documented sentinel). */
    up_perfmon_init(&pm, -1);
    CHECK_EQ(pm.enabled, 0);
    CHECK_EQ(pm.budget_ns, 0);

    up_perfmon_init(&pm, INT_MIN);
    CHECK_EQ(pm.enabled, 0);
    CHECK_EQ(pm.budget_ns, 0);
    END();
}

/* ---------- warmup ---------- */

static void test_warmup_drops_first_samples(void)
{
    BEGIN("record: warmup window suppresses warning");
    up_perfmon_t pm;
    up_perfmon_init(&pm, 60);  /* budget = 16.66 ms */

    /* Feed huge values during warmup — must not warn. */
    for (int i = 0; i < UP_PERFMON_WARMUP_FRAMES; i++) {
        CHECK_EQ(up_perfmon_record_ns(&pm, 100000000LL /* 100 ms */), 0);
    }
    /* Just one post-warmup sample — still below MIN_FRAMES_FOR_WARN. */
    CHECK_EQ(up_perfmon_record_ns(&pm, 100000000LL), 0);
    END();
}

/* ---------- detection ---------- */

static void test_warns_after_sustained_overrun(void)
{
    BEGIN("record: warns once after sustained overrun");
    up_perfmon_t pm;
    up_perfmon_init(&pm, 60);  /* budget ~16.67 ms */

    int warns = 0;
    /* Push way past budget (50 ms) for plenty of samples. */
    for (int i = 0; i < 100; i++) {
        if (up_perfmon_record_ns(&pm, 50000000LL)) warns++;
    }
    CHECK_EQ(warns, 1);
    CHECK_EQ(pm.has_warned, 1);

    /* Should never warn again, even if we keep recording overruns. */
    for (int i = 0; i < 100; i++) {
        CHECK_EQ(up_perfmon_record_ns(&pm, 50000000LL), 0);
    }
    END();
}

static void test_no_warn_below_budget(void)
{
    BEGIN("record: never warns when below budget");
    up_perfmon_t pm;
    up_perfmon_init(&pm, 60);

    /* 5 ms per frame — well under budget. */
    int warns = 0;
    for (int i = 0; i < 200; i++) {
        if (up_perfmon_record_ns(&pm, 5000000LL)) warns++;
    }
    CHECK_EQ(warns, 0);
    CHECK_EQ(pm.has_warned, 0);
    CHECK_EQ(pm.samples_seen, UP_PERFMON_MIN_FRAMES_FOR_WARN);
    END();
}

static void test_samples_seen_saturates(void)
{
    BEGIN("record: samples_seen saturates at trust threshold");
    up_perfmon_t pm;
    up_perfmon_init(&pm, 60);

    for (int i = 0; i < UP_PERFMON_MIN_FRAMES_FOR_WARN + 100; i++)
        CHECK_EQ(up_perfmon_record_ns(&pm, 5000000LL), 0);

    CHECK_EQ(pm.samples_seen, UP_PERFMON_MIN_FRAMES_FOR_WARN);
    CHECK_EQ(pm.has_warned, 0);
    END();
}

static void test_no_warn_for_brief_spike(void)
{
    BEGIN("record: brief spike doesn't trigger warning (EWMA dampens)");
    up_perfmon_t pm;
    up_perfmon_init(&pm, 60);

    /* Mostly fine (5 ms), with one giant outlier in the middle. */
    int warns = 0;
    for (int i = 0; i < 200; i++) {
        int64_t t = (i == 50) ? 80000000LL /* 80 ms spike */ : 5000000LL;
        if (up_perfmon_record_ns(&pm, t)) warns++;
    }
    CHECK_EQ(warns, 0);
    CHECK_EQ(pm.has_warned, 0);
    END();
}

static void test_ewma_tracks_after_warn(void)
{
    BEGIN("record: EWMA keeps tracking after the warn latch (OBS-9)");
    up_perfmon_t pm;
    up_perfmon_init(&pm, 60);

    for (int i = 0; i < 100; i++)
        (void)up_perfmon_record_ns(&pm, 50000000LL);
    CHECK_EQ(pm.has_warned, 1);
    int64_t at_warn_us = up_perfmon_ewma_us(&pm);

    /* Load returns to normal: the exported EWMA must follow it down
     * without ever re-warning. */
    int warns = 0;
    for (int i = 0; i < 200; i++)
        if (up_perfmon_record_ns(&pm, 5000000LL)) warns++;
    CHECK_EQ(warns, 0);
    CHECK_EQ(pm.has_warned, 1);
    CHECK(up_perfmon_ewma_us(&pm) < at_warn_us / 2);
    END();
}

/* ---------- ignore garbage ---------- */

static void test_ignores_non_positive_samples(void)
{
    BEGIN("record: <=0 samples are ignored");
    up_perfmon_t pm;
    up_perfmon_init(&pm, 60);

    /* Negative and zero samples shouldn't advance samples_seen. */
    int before = pm.samples_seen;
    CHECK_EQ(up_perfmon_record_ns(&pm, 0), 0);
    CHECK_EQ(up_perfmon_record_ns(&pm, -123456), 0);
    CHECK_EQ(pm.samples_seen, before);
    END();
}

static void test_null_pm_safe(void)
{
    BEGIN("record / init: NULL pm pointer is safe");
    up_perfmon_init(NULL, 60);  /* must not crash */
    CHECK_EQ(up_perfmon_record_ns(NULL, 5000000LL), 0);
    CHECK_EQ(up_perfmon_ewma_us(NULL), 0);
    CHECK_EQ(up_perfmon_budget_us(NULL), 0);
    END();
}

/* ---------- EWMA convergence ---------- */

static void test_ewma_converges_to_constant(void)
{
    BEGIN("ewma converges to constant input within ~5 samples post-warmup");
    up_perfmon_t pm;
    up_perfmon_init(&pm, 60);

    const int64_t target = 8000000LL;  /* 8 ms */
    /* Warmup. */
    for (int i = 0; i < UP_PERFMON_WARMUP_FRAMES; i++)
        up_perfmon_record_ns(&pm, target);
    /* After warmup, EWMA should equal target (we initialise to last warmup sample). */
    CHECK_EQ(up_perfmon_ewma_us(&pm), target / 1000);

    /* Feed the same value many times — EWMA stays exactly there. */
    for (int i = 0; i < 100; i++)
        up_perfmon_record_ns(&pm, target);
    CHECK_EQ(up_perfmon_ewma_us(&pm), target / 1000);
    END();
}

/* UB-1 regression: the EWMA step must be floor(diff / 8) — bit-identical
 * to the arithmetic right shift it replaced — for negative diffs too.
 * Truncating division would round toward zero and diverge on negatives. */
static void test_ewma_step_floor_semantics(void)
{
    BEGIN("ewma step is floor division for negative diffs (UB-1)");
    const int64_t cases[] = {
        -17, -16, -15, -9, -8, -7, -1, 0, 1, 7, 8, 9, 15, 16, 17,
        INT64_MIN / 2, INT64_MAX / 2,
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        int64_t d = cases[i];
        int64_t want = d / 8;
        if (d < 0 && d % 8 != 0) want--;   /* reference floor */
        CHECK_EQ(up_perfmon__ewma_step(d), want);
    }
    END();
}

static void test_ewma_converges_downward_exactly(void)
{
    BEGIN("ewma reaches a lower constant exactly (floor step moves on |diff|<8)");
    up_perfmon_t pm;
    up_perfmon_init(&pm, 60);

    for (int i = 0; i < UP_PERFMON_WARMUP_FRAMES; i++)
        up_perfmon_record_ns(&pm, 8000000LL);
    /* Drop by 5 ns: diff = -5 each step; floor gives -1, so the EWMA
     * walks all the way down. Truncation would give 0 and stick. */
    for (int i = 0; i < 10; i++)
        up_perfmon_record_ns(&pm, 8000000LL - 5);
    CHECK_EQ(pm.ewma_ns, 8000000LL - 5);
    END();
}

static void test_ewma_us_zero_during_warmup(void)
{
    BEGIN("ewma_us returns 0 strictly before warmup completes");
    up_perfmon_t pm;
    up_perfmon_init(&pm, 60);
    CHECK_EQ(up_perfmon_ewma_us(&pm), 0);
    /* Feed WARMUP-1 samples — still in warmup. */
    for (int i = 0; i < UP_PERFMON_WARMUP_FRAMES - 1; i++)
        up_perfmon_record_ns(&pm, 5000000LL);
    CHECK_EQ(up_perfmon_ewma_us(&pm), 0);
    /* The WARMUP-th sample seeds the EWMA — accessor non-zero from now on. */
    up_perfmon_record_ns(&pm, 5000000LL);
    CHECK_EQ(up_perfmon_ewma_us(&pm), 5000);
    END();
}

/* ---------- main ---------- */

/* up_perfmon_budget_us: 0 when disabled or NULL, budget_ns/1000 when on. */
static void test_budget_us(void)
{
    BEGIN("budget_us: enabled returns budget_ns/1000; disabled/NULL return 0");
    up_perfmon_t pm;
    up_perfmon_init(&pm, 60);
    CHECK(up_perfmon_budget_us(&pm) > 0);
    CHECK_EQ(up_perfmon_budget_us(&pm), pm.budget_ns / 1000);
    up_perfmon_t off;
    up_perfmon_init(&off, 0);
    CHECK_EQ(up_perfmon_budget_us(&off), 0);
    CHECK_EQ(up_perfmon_budget_us(NULL), 0);
    END();
}

int main(void)
{
    printf("Running perfmon tests...\n");

    test_init_disabled_for_zero_fps();
    test_init_60fps_budget();
    test_init_target_fps_oob_boundaries();
    test_warmup_drops_first_samples();
    test_warns_after_sustained_overrun();
    test_no_warn_below_budget();
    test_samples_seen_saturates();
    test_no_warn_for_brief_spike();
    test_ewma_tracks_after_warn();
    test_ignores_non_positive_samples();
    test_null_pm_safe();
    test_ewma_converges_to_constant();
    test_ewma_step_floor_semantics();
    test_ewma_converges_downward_exactly();
    test_ewma_us_zero_during_warmup();
    test_budget_us();

    printf("\n%d tests run, %d failed\n", g_run, g_fail);
    return g_fail == 0 ? 0 : 1;
}
