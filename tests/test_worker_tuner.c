// SPDX-License-Identifier: GPL-2.0-or-later
#include "../src/worker_tuner.h"
#include "test_harness.h"

static double cost(int workers, int optimum)
{
    if (workers == optimum) return 50.0;
    return workers < optimum ? 160.0 - workers : 200.0 + workers;
}

static void converge(up_worker_tuner_t *t, int optimum)
{
    for (int i = 0; i < 2000 && t->phase != UP_TUNER_SETTLED; i++) {
        double elapsed = cost(t->current, optimum);
        if (i % 7 == 0) elapsed *= 100.0;
        up_worker_tuner_observe(t, elapsed);
        CHECK(t->current >= 1 && t->current <= t->limit);
    }
    CHECK(t->phase == UP_TUNER_SETTLED);
    CHECK(t->best == optimum);
}

static void test_search(void)
{
    BEGIN("nonmonotonic timings, outliers and geometry limits");
    const int limits[] = { 1, 2, 3, 8, 12, 16, 23, 32, 48, 64 };
    for (size_t i = 0; i < sizeof limits / sizeof limits[0]; i++) {
        up_worker_tuner_t t;
        up_worker_tuner_init(&t, 12, limits[i]);
        converge(&t, limits[i]);
    }
    up_worker_tuner_t t;
    up_worker_tuner_init(&t, 12, 64);
    converge(&t, 16);
    END();
}

static void window(up_worker_tuner_t *t, double elapsed)
{
    const int frames = t->warmup + UP_TUNER_SAMPLES - t->used;
    for (int i = 0; i < frames; i++) up_worker_tuner_observe(t, elapsed);
}

static void test_confirmation(void)
{
    BEGIN("require five percent gain against both baselines");
    up_worker_tuner_t t;
    up_worker_tuner_init(&t, 2, 4);
    window(&t, 100.0);
    CHECK(t.current == 1);
    window(&t, 80.0);
    CHECK(t.current == 2);
    window(&t, 70.0);
    CHECK(t.best == 2);
    window(&t, 100.0);
    CHECK(t.current == 4);
    window(&t, 96.0);
    window(&t, 100.0);
    CHECK(t.best == 2);
    END();
}

static void test_recheck(void)
{
    BEGIN("sustained drift and periodic exploration");
    up_worker_tuner_t t;
    up_worker_tuner_init(&t, 12, 32);
    converge(&t, 16);
    for (int i = 0; i < UP_TUNER_COOLDOWN_FRAMES; i++)
        up_worker_tuner_observe(&t, 50.0);
    window(&t, 100.0);
    window(&t, 50.0);
    CHECK(t.drift == 0 && t.phase == UP_TUNER_SETTLED);
    for (int i = 0; i < 3; i++) window(&t, 100.0);
    CHECK(t.phase == UP_TUNER_BASE);
    converge(&t, 4);
    const int remaining = t.remaining;
    for (int i = 0; i < remaining; i++)
        up_worker_tuner_observe(&t, 50.0);
    CHECK(t.phase == UP_TUNER_BASE);
    converge(&t, 8);
    END();
}

static void test_bounded_probes(void)
{
    BEGIN("slow probes exit early and load changes respect cooldown");
    up_worker_tuner_t t;
    up_worker_tuner_init(&t, 2, 4);
    window(&t, 100.0);
    for (int i = 0; i < UP_TUNER_WARMUP + 4; i++)
        up_worker_tuner_observe(&t, 200.0);
    CHECK(t.phase == UP_TUNER_CONFIRM && t.current == 2);
    window(&t, 100.0);
    CHECK(t.best == 2);
    up_tuner_settle(&t, 100.0);
    for (int i = 0; i < UP_TUNER_COOLDOWN_FRAMES - 16; i++)
        up_worker_tuner_observe(&t, 200.0);
    CHECK(t.phase == UP_TUNER_SETTLED && t.drift == 0);
    for (int i = 0; i < 3; i++) window(&t, 200.0);
    CHECK(t.phase == UP_TUNER_BASE);
    END();
}

static void test_invalid(void)
{
    BEGIN("invalid samples and initialization bounds");
    up_worker_tuner_t t;
    up_worker_tuner_init(&t, -1, -2);
    CHECK(t.best == 1 && t.limit == 1);
    up_worker_tuner_init(&t, 99, 999);
    CHECK(t.best == 64 && t.limit == 64);
    const double invalid[] = { NAN, INFINITY, -INFINITY, 0.0, -1.0 };
    for (size_t i = 0; i < sizeof invalid / sizeof invalid[0]; i++)
        up_worker_tuner_observe(&t, invalid[i]);
    CHECK(t.used == 0 && t.warmup == UP_TUNER_WARMUP);
    CHECK(up_tuner_next_count(64, 65) == 0);
    END();
}

int main(void)
{
    test_search();
    test_confirmation();
    test_recheck();
    test_bounded_probes();
    test_invalid();
    return test_harness_report();
}
