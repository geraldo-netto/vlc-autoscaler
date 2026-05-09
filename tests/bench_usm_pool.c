// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * bench_usm_pool.c — minimal perf bench for the USM pool.
 *
 * Usage: bench_usm_pool <threads> <width> <height> [frames] [amount] [fill]
 *   amount default = 30, frames default = 100
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

int main(int argc, char **argv)
{
    if (argc < 4) {
        fprintf(stderr, "usage: %s <threads> <width> <height> [frames] [amount]\n", argv[0]);
        return 2;
    }
    int  n_threads  = atoi(argv[1]);
    int  width      = atoi(argv[2]);
    int  height     = atoi(argv[3]);
    int  frames     = (argc >= 5) ? atoi(argv[4]) : 100;
    int  amount_pct = (argc >= 6) ? atoi(argv[5]) : 30;
    const char *fill = (argc >= 7) ? argv[6] : "rand";

    if (n_threads < 1 || width < 8 || height < 8 || frames < 1) {
        fprintf(stderr, "bad args\n"); return 2;
    }

    int amount = up_usm_amount_pct_to_q8(amount_pct);
    size_t plane = (size_t)width * (size_t)height;

    uint8_t *src = aligned_alloc(64, plane);
    uint8_t *dst = aligned_alloc(64, plane);
    if (!src || !dst) { fprintf(stderr, "alloc fail\n"); return 1; }

    usm_pool_t *pool = up_usm_pool_create(n_threads, width, height, 0);
    if (!pool) { fprintf(stderr, "pool create fail\n"); return 1; }

    /* Fill src for one frame according to mode. */
    void (*fill_frame)(uint8_t *, size_t, uint32_t, int, int);
    int mode_rand  = !strcmp(fill, "rand");
    int mode_flat  = !strcmp(fill, "flat");
    int mode_mixed = !strcmp(fill, "mixed");
    if (!mode_rand && !mode_flat && !mode_mixed) {
        fprintf(stderr, "unknown fill mode '%s'\n", fill); return 2;
    }
    (void)fill_frame;

    /* Pre-warm: 5 throwaway frames so worker threads + pages settle. */
    for (int i = 0; i < 5; i++) {
        fill_xs(src, plane, 0xC0FFEEu + (uint32_t)i);
        if (up_usm_pool_apply(pool, dst, width, src, width, amount) != 0) {
            fprintf(stderr, "warm apply fail\n"); return 1;
        }
    }

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int i = 0; i < frames; i++) {
        if (mode_rand) {
            fill_xs(src, plane, 0xDEADBEEFu + (uint32_t)i * 2654435761u);
        } else if (mode_flat) {
            memset(src, 128 + (i & 7), plane);  /* near-constant grey */
        } else { /* mixed: top half flat, bottom half random */
            size_t half = (size_t)width * (size_t)(height / 2);
            memset(src, 128 + (i & 7), half);
            fill_xs(src + half, plane - half,
                    0xDEADBEEFu + (uint32_t)i * 2654435761u);
        }
        if (up_usm_pool_apply(pool, dst, width, src, width, amount) != 0) {
            fprintf(stderr, "apply fail at frame %d\n", i); return 1;
        }
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);

    double el_ns = (t1.tv_sec - t0.tv_sec) * 1.0e9 + (t1.tv_nsec - t0.tv_nsec);
    double us_per_frame = (el_ns / 1000.0) / (double)frames;

    printf("%d,%d,%d,%d,%d,%s,%.2f\n",
           n_threads, width, height, frames, amount_pct, fill, us_per_frame);

    up_usm_pool_destroy(pool);
    free(src); free(dst);
    return 0;
}
