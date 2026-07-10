// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * bench_usm_pool.c — minimal perf bench for the USM pool.
 *
 * Usage: bench_usm_pool <threads> <width> <height> [frames] [amount] [fill]
 *   amount default = 20, frames default = 100
 *   fill: rand (default) / flat / mixed (top half flat, bottom random)
 *
 * Output: one CSV line:
 *   threads,width,height,frames,amount,fill,us_per_frame
 */
#include "../src/usm_pool.h"
#include "../src/usm.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static uint32_t xs32(uint32_t *s)
{
    uint32_t v = *s;
    v ^= v << 13; v ^= v >> 17; v ^= v << 5;
    *s = v; return v;
}
static void fill_xs(uint8_t *buf, size_t n, uint32_t seed)
{
    uint32_t s = seed; for (size_t i = 0; i < n; i++) buf[i] = (uint8_t)xs32(&s);
}

struct bench_args {
    int n_threads;
    int width;
    int height;
    int frames;
    int amount_pct;
    const char *fill;
    int mode; /* 0=rand, 1=flat, 2=mixed */
};

static int parse_fill_mode(const char *fill)
{
    if (!strcmp(fill, "rand"))  return 0;
    if (!strcmp(fill, "flat"))  return 1;
    if (!strcmp(fill, "mixed")) return 2;
    return -1;
}

static int args_in_range(const struct bench_args *a)
{
    return a->n_threads >= 1 && a->width >= 8
        && a->height >= 8 && a->frames >= 1;
}

static int parse_args(int argc, char **argv, struct bench_args *a)
{
    if (argc < 4) {
        fprintf(stderr, "usage: %s <threads> <width> <height> [frames] [amount]\n", argv[0]);
        return 2;
    }
    a->n_threads  = atoi(argv[1]);
    a->width      = atoi(argv[2]);
    a->height     = atoi(argv[3]);
    a->frames     = (argc >= 5) ? atoi(argv[4]) : 100;
    a->amount_pct = (argc >= 6) ? atoi(argv[5]) : 20;
    a->fill       = (argc >= 7) ? argv[6] : "rand";

    if (!args_in_range(a)) {
        fprintf(stderr, "bad args\n"); return 2;
    }
    a->mode = parse_fill_mode(a->fill);
    if (a->mode < 0) {
        fprintf(stderr, "unknown fill mode '%s'\n", a->fill); return 2;
    }
    return 0;
}

static void fill_frame(uint8_t *src, int width, int height, int mode, int i)
{
    size_t plane = (size_t)width * (size_t)height;
    if (mode == 0) {
        fill_xs(src, plane, 0xDEADBEEFu + (uint32_t)i * 2654435761u);
    } else if (mode == 1) {
        memset(src, 128 + (i & 7), plane);
    } else {
        size_t half = (size_t)width * (size_t)(height / 2);
        memset(src, 128 + (i & 7), half);
        fill_xs(src + half, plane - half,
                0xDEADBEEFu + (uint32_t)i * 2654435761u);
    }
}

static int run_warmup(usm_pool_t *pool, uint8_t *src, uint8_t *dst,
                      int width, size_t plane, int amount)
{
    for (int i = 0; i < 5; i++) {
        fill_xs(src, plane, 0xC0FFEEu + (uint32_t)i);
        if (up_usm_pool_apply(pool, dst, width, src, width, amount) != 0) {
            fprintf(stderr, "warm apply fail\n"); return 1;
        }
    }
    return 0;
}

static int run_timed(usm_pool_t *pool, uint8_t *src, uint8_t *dst,
                     const struct bench_args *a, int amount, double *us_per_frame)
{
    /* Fill the source ONCE before timing. The USM kernel's work is
     * content-independent (only the opt-in flat-skip path branches on
     * content), so regenerating a random frame each iteration would just
     * fold the serial fill_xs cost into the measurement and swamp the
     * thing we want to measure. Fill once, then time apply() only. */
    fill_frame(src, a->width, a->height, a->mode, 0);

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int i = 0; i < a->frames; i++) {
        if (up_usm_pool_apply(pool, dst, a->width, src, a->width, amount) != 0) {
            fprintf(stderr, "apply fail at frame %d\n", i); return 1;
        }
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);

    double el_ns = (t1.tv_sec - t0.tv_sec) * 1.0e9 + (t1.tv_nsec - t0.tv_nsec);
    *us_per_frame = (el_ns / 1000.0) / (double)a->frames;
    return 0;
}

int main(int argc, char **argv)
{
    struct bench_args a;
    int rc = parse_args(argc, argv, &a);
    if (rc) return rc;

    int amount = up_usm_amount_pct_to_q8(a.amount_pct);
    size_t plane = (size_t)a.width * (size_t)a.height;

    uint8_t *src = aligned_alloc(64, plane);
    uint8_t *dst = aligned_alloc(64, plane);
    if (!src || !dst) { fprintf(stderr, "alloc fail\n"); return 1; }

    usm_pool_t *pool = up_usm_pool_create(a.n_threads, a.width, a.height, 0);
    if (!pool) {
        fprintf(stderr, "pool create fail\n");
        free(src); free(dst);
        return 1;
    }

    int rc_run = 0;
    if (run_warmup(pool, src, dst, a.width, plane, amount) != 0) {
        rc_run = 1;
        goto out;
    }

    double us_per_frame = 0.0;
    if (run_timed(pool, src, dst, &a, amount, &us_per_frame) != 0) {
        rc_run = 1;
        goto out;
    }

    printf("%d,%d,%d,%d,%d,%s,%.2f\n",
           a.n_threads, a.width, a.height, a.frames, a.amount_pct, a.fill, us_per_frame);

out:
    up_usm_pool_destroy(pool);
    free(src); free(dst);
    return rc_run;
}
