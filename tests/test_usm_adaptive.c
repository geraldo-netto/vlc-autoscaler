// SPDX-License-Identifier: GPL-2.0-or-later
#include <stdlib.h>
#include <time.h>
#include "test_harness.h"

static int fake_clock(clockid_t id, struct timespec *time);
#define clock_gettime fake_clock
#define up_usm_pool_create adaptive_fake_create
#define up_usm_pool_destroy adaptive_fake_destroy
#define up_usm_pool_apply adaptive_fake_apply
#define up_usm_pool_effective_threads adaptive_fake_threads
#include "../src/usm_adaptive.h"
#undef clock_gettime

struct usm_pool_s { int workers, calls; };
static int live, peak, fail_create, fail_apply, shrink, fail_clock;
static long ticks, step;

static int fake_clock(clockid_t id, struct timespec *time)
{
    (void)id;
    if (fail_clock) return -1;
    ticks += step;
    time->tv_sec = ticks / 1000000L;
    time->tv_nsec = ticks % 1000000L * 1000L;
    return 0;
}

usm_pool_t *up_usm_pool_create(int workers, int width, int height, int stripe)
{
    (void)width; (void)height; (void)stripe;
    if (fail_create) return NULL;
    usm_pool_t *pool = calloc(1, sizeof *pool);
    if (pool == NULL) abort();
    pool->workers = workers;
    live++;
    if (peak < live) peak = live;
    return pool;
}

void up_usm_pool_destroy(usm_pool_t *pool)
{
    if (pool == NULL) return;
    live--;
    free(pool);
}

int up_usm_pool_effective_threads(const usm_pool_t *pool)
{
    return shrink ? 1 : pool->workers;
}

int up_usm_pool_apply(usm_pool_t *pool, uint8_t *dst, int dst_stride,
                       const uint8_t *src, int src_stride, int amount)
{
    (void)dst; (void)dst_stride; (void)src; (void)src_stride; (void)amount;
    if (pool == NULL) return UP_USM_APPLY_FAILED_UNCHANGED;
    pool->calls++;
    step = pool->workers == 4 ? 25 : 100;
    const int status = fail_apply;
    fail_apply = 0;
    return status;
}

static usm_pool_t *setup(up_usm_adaptive_t *a)
{
    live = peak = fail_create = fail_apply = shrink = fail_clock = 0;
    ticks = 0; step = 100;
    up_usm_adaptive_init(a, 2, 4, 64, 64, 0);
    return up_usm_pool_create(2, 64, 64, 0);
}

static void cleanup(up_usm_adaptive_t *a, usm_pool_t *best)
{
    up_usm_adaptive_stop(a);
    up_usm_pool_destroy(best);
    CHECK(live == 0);
}

static void test_accept_reject(void)
{
    BEGIN("bounded ownership across rejected and accepted trials");
    up_usm_adaptive_t a;
    usm_pool_t *best = setup(&a);
    for (int i = 0; i < 250; i++)
        CHECK(up_usm_adaptive_apply(&a, &best, NULL, 64, 20) == 0);
    CHECK(a.tuner.best == 4 && best->workers == 4);
    CHECK(a.tuner.phase == UP_TUNER_SETTLED && a.enabled);
    CHECK(a.trial == NULL && live == 1 && peak == 2);
    CHECK(a.tuner.changes == 1);
    cleanup(&a, best);
    END();
}

static void test_trial_failure(int failure)
{
    BEGIN("trial failure retains baseline and never double sharpens");
    up_usm_adaptive_t a;
    usm_pool_t *best = setup(&a);
    a.tuner.current = 4;
    fail_apply = failure;
    const int status = up_usm_adaptive_apply(&a, &best, NULL, 64, 20);
    CHECK(!a.enabled && a.stopped && a.trial == NULL);
    const bool uncertain = failure == UP_USM_APPLY_OUTPUT_UNCERTAIN;
    CHECK(status == (uncertain ? failure : UP_USM_APPLY_OK));
    CHECK(a.retained == uncertain);
    CHECK(best->calls == (uncertain ? 0 : 1));
    CHECK(up_usm_adaptive_apply(&a, &best, NULL, 64, 20) == 0);
    CHECK(!a.retained && live == 1);
    cleanup(&a, best);
    END();
}

static void test_resource_failures(void)
{
    BEGIN("allocation and partial worker startup stop exploration safely");
    up_usm_adaptive_t a;
    usm_pool_t *best = setup(&a);
    a.tuner.current = 4;
    fail_create = 1;
    CHECK(up_usm_adaptive_apply(&a, &best, NULL, 64, 20) == 0);
    CHECK(!a.enabled && best->calls == 1 && live == 1);
    cleanup(&a, best);
    best = setup(&a);
    a.tuner.current = 4;
    shrink = 1;
    CHECK(up_usm_adaptive_apply(&a, &best, NULL, 64, 20) == 0);
    CHECK(!a.enabled && best->calls == 0 && live == 1);
    cleanup(&a, best);
    END();
}

static void test_clock_and_baseline_failure(void)
{
    BEGIN("clock failure and baseline error contract");
    up_usm_adaptive_t a;
    usm_pool_t *best = setup(&a);
    fail_clock = 1;
    CHECK(up_usm_adaptive_apply(&a, &best, NULL, 64, 20) == 0);
    CHECK(!a.enabled && a.stopped);
    fail_apply = UP_USM_APPLY_OUTPUT_UNCERTAIN;
    CHECK(up_usm_adaptive_apply(&a, &best, NULL, 64, 20)
          == UP_USM_APPLY_OUTPUT_UNCERTAIN);
    CHECK(!a.retained);
    cleanup(&a, best);
    END();
}

static void test_pipeline_timing(void)
{
    BEGIN("pipeline score includes upstream processing and excludes idle time");
    up_usm_adaptive_t a;
    usm_pool_t *best = setup(&a);
    a.tuner.warmup = 0;
    ticks += 10000;
    up_usm_adaptive_begin(&a);
    ticks += 250;
    CHECK(up_usm_adaptive_apply(&a, &best, NULL, 64, 20) == 0);
    CHECK(a.tuner.samples[0] == 350.0 && !a.frame_pending);
    fail_clock = 1;
    up_usm_adaptive_begin(&a);
    fail_clock = 0;
    CHECK(up_usm_adaptive_apply(&a, &best, NULL, 64, 20) == 0);
    CHECK(!a.enabled && a.stopped);
    up_usm_adaptive_begin(&a);
    CHECK(!a.frame_pending);
    cleanup(&a, best);
    END();
}

static void test_geometry(void)
{
    BEGIN("height and stripe limits");
    up_usm_adaptive_t a;
    up_usm_adaptive_init(&a, 12, 64, 64, 16, 16);
    CHECK(!a.enabled && a.tuner.best == 1);
    up_usm_adaptive_init(&a, 12, 64, 64, 64, 0);
    CHECK(a.enabled && a.tuner.limit == 8 && a.tuner.best == 8);
    END();
}

int main(void)
{
    test_accept_reject();
    test_trial_failure(UP_USM_APPLY_FAILED_UNCHANGED);
    test_trial_failure(UP_USM_APPLY_OUTPUT_UNCERTAIN);
    test_resource_failures();
    test_clock_and_baseline_failure();
    test_pipeline_timing();
    test_geometry();
    return test_harness_report();
}
