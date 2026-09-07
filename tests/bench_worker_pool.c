// SPDX-License-Identifier: GPL-2.0-or-later
#define UP_WORKER_POOL_TIMING 1
#include "../src/worker_pool.h"
#include "cli_parse.h"

#include <stdio.h>
#include <time.h>

typedef struct {
    alignas(UP_POOL_CACHELINE) unsigned char count;
    struct timespec finished;
} bench_slot_t;

_Static_assert(sizeof(bench_slot_t) % UP_POOL_CACHELINE == 0,
               "benchmark worker slots must occupy whole cache lines");

typedef struct {
    up_worker_pool_t pool;
} bench_pool_t;

static int construct_slot(void *owner, int index)
{
    (void)owner;
    (void)index;
    return 0;
}

static void run_slot(void *owner, int index)
{
    bench_pool_t *bench = (bench_pool_t *)owner;
    bench_slot_t *slot = up_worker_pool_slot(&bench->pool, index);
    slot->count++;
}

static void record_finish(void *owner, int index)
{
    bench_pool_t *bench = (bench_pool_t *)owner;
    bench_slot_t *slot = up_worker_pool_slot(&bench->pool, index);
    (void)clock_gettime(CLOCK_MONOTONIC, &slot->finished);
}

static const up_worker_pool_ops_t ops = {
    .construct = construct_slot,
    .run = run_slot,
    .after_run = record_finish,
    .all_or_nothing = true,
};

static int elapsed_us(const struct timespec *start, const struct timespec *end,
                      int iterations, double *result)
{
    if (iterations <= 0) return -1;
    double ns = (end->tv_sec - start->tv_sec) * 1.0e9
              + (end->tv_nsec - start->tv_nsec);
    *result = ns / 1000.0 / (double)iterations;
    return 0;
}

static int parse_args(int argc, char **argv, long *workers, long *iterations)
{
    *iterations = 10000;
    if (argc < 2 ||
        !up_cli_parse_long(argv[1], 1, UP_THREADS_MAX, workers) ||
        (argc >= 3 && !up_cli_parse_long(argv[2], 1, INT_MAX, iterations))) {
        fprintf(stderr, "usage: %s <workers> [iterations]\n", argv[0]);
        return 2;
    }
    return 0;
}

static int dispatch_many(up_worker_pool_t *pool, long iterations)
{
    for (long i = 0; i < iterations; i++)
        if (up_worker_pool_dispatch(pool) != 0) return 1;
    return 0;
}

static double completion_skew_us(const bench_pool_t *bench, int workers)
{
    const bench_slot_t *slots = up_worker_pool_slot(&bench->pool, 0);
    const struct timespec *low = &slots[0].finished;
    const struct timespec *high = low;
    for (int i = 1; i < workers; i++) {
        const struct timespec *value = &slots[i].finished;
        if (value->tv_sec < low->tv_sec ||
            (value->tv_sec == low->tv_sec && value->tv_nsec < low->tv_nsec))
            low = value;
        if (value->tv_sec > high->tv_sec ||
            (value->tv_sec == high->tv_sec && value->tv_nsec > high->tv_nsec))
            high = value;
    }
    return (high->tv_sec - low->tv_sec) * 1.0e6
         + (high->tv_nsec - low->tv_nsec) / 1000.0;
}

int main(int argc, char **argv)
{
    long workers = 0;
    long iterations = 0;
    int parse_rc = parse_args(argc, argv, &workers, &iterations);
    if (parse_rc != 0) return parse_rc;

    bench_pool_t bench = {0};
    up_worker_pool_config(&bench.pool, &ops, &bench, (int)workers,
                          sizeof(bench_slot_t));
    if (up_worker_pool_ensure_started(&bench.pool) != 0) return 1;
    if (dispatch_many(&bench.pool, 100) != 0) return 1;

    struct timespec start, end;
    if (clock_gettime(CLOCK_MONOTONIC, &start) != 0) return 1;
    if (dispatch_many(&bench.pool, iterations) != 0) return 1;
    if (clock_gettime(CLOCK_MONOTONIC, &end) != 0) return 1;

    double us = 0.0;
    if (elapsed_us(&start, &end, (int)iterations, &us) != 0) return 1;
    printf("%ld,%ld,%.3f,%.3f\n", workers, iterations, us,
           completion_skew_us(&bench, (int)workers));
    return up_worker_pool_destroy(&bench.pool) == 0 ? 0 : 1;
}
