// SPDX-License-Identifier: GPL-2.0-or-later
#define ZIMG_TEST_DEFINE_MODULE_NAME
#include "zimg_test_util.h"
#include "../src/usm_adaptive.h"
#include "cli_parse.h"

typedef struct {
    long workers, frames, width, height, zimg_workers;
} args_t;

typedef struct {
    scaler_ctx_t scaler;
    zt_pic_t src, dst;
    usm_pool_t *usm;
    up_usm_adaptive_t adaptive;
    double total_us, tail_us, zimg_us, usm_us;
    int settled_frame;
} bench_t;

static int parse(int argc, char **argv, args_t *args)
{
    *args = (args_t){ -1, 4096, 1920, 1080, 12 };
    const long minimum[] = { -1, 1024, 64, 64, 1 };
    const long maximum[] = { 64, 1000000, 4096, 2160, 64 };
    long *values[] = { &args->workers, &args->frames, &args->width,
                      &args->height, &args->zimg_workers };
    if (argc < 2 || argc > 6) return 1;
    for (int i = 1; i < argc; i++)
        if (!up_cli_parse_long(argv[i], minimum[i - 1], maximum[i - 1],
                                values[i - 1])) return 1;
    return args->workers == 0 || args->width % 4 || args->height % 4;
}

static void destroy(bench_t *b)
{
    up_usm_adaptive_stop(&b->adaptive);
    up_usm_pool_destroy(b->usm);
    if (b->scaler.priv != NULL) b->scaler.backend->close(&b->scaler);
    zt_pic_free(&b->src);
    zt_pic_free(&b->dst);
}

static int initialize(bench_t *b, const args_t *a)
{
    const int width = (int)a->width, height = (int)a->height;
    const int cores = up_detect_cores();
    const int initial = up_threads_decide(a->workers < 0 ? 0 : (int)a->workers,
                                          cores);
    if (zt_pic_alloc(&b->src, VLC_CODEC_I420, width / 2, height / 2)) return 1;
    if (zt_pic_alloc(&b->dst, VLC_CODEC_I420, width, height)) return 1;
    zt_pic_fill(&b->src, 0x12345678u);
    zt_ctx_init(&b->scaler, VLC_CODEC_I420, width / 2, height / 2,
                width, height, (int)a->zimg_workers, 1);
    b->usm = up_usm_pool_create(initial, width, height, 0);
    if (a->workers < 0)
        up_usm_adaptive_init(&b->adaptive, initial,
            up_threads_decide(64, cores), width, height, 0);
    return b->usm == NULL || b->scaler.backend->open(&b->scaler) != 0;
}

static double difference(const struct timespec *start, const struct timespec *end)
{
    return ((double)end->tv_sec - (double)start->tv_sec) * 1e6
         + ((double)end->tv_nsec - (double)start->tv_nsec) / 1e3;
}

static int frame(bench_t *b, int index, int frames)
{
    struct timespec start, scaled, end;
    if (clock_gettime(CLOCK_MONOTONIC, &start)) return 1;
    up_usm_adaptive_begin(&b->adaptive);
    if (b->scaler.backend->process(&b->scaler, &b->src.pic, &b->dst.pic)
        != SCALER_PROCESS_OK) return 1;
    if (clock_gettime(CLOCK_MONOTONIC, &scaled)) return 1;
    plane_t *luma = &b->dst.pic.p[0];
    if (up_usm_adaptive_apply(&b->adaptive, &b->usm,
        luma->p_pixels, luma->i_pitch, 51) != 0) return 1;
    if (clock_gettime(CLOCK_MONOTONIC, &end)) return 1;
    const double us = difference(&start, &end);
    b->total_us += us;
    b->zimg_us += difference(&start, &scaled);
    b->usm_us += difference(&scaled, &end);
    if (index >= frames - 512) b->tail_us += us;
    if (!b->settled_frame && b->adaptive.tuner.phase == UP_TUNER_SETTLED)
        b->settled_frame = index + 1;
    return 0;
}

int main(int argc, char **argv)
{
    args_t args;
    if (parse(argc, argv, &args)) {
        fprintf(stderr, "usage: %s <USM-workers|-1=adaptive> [frames>=1024] "
            "[width] [height] [zimg-workers]\n", argv[0]);
        return 2;
    }
    bench_t bench = { 0 };
    int rc = initialize(&bench, &args);
    for (int i = 0; !rc && i < args.frames; i++)
        rc = frame(&bench, i, (int)args.frames);
    if (!rc)
        printf("%ld,%ld,%ld,%ld,%ld,%d,%d,%u,%.2f,%.2f,%.2f,%.2f\n",
            args.workers, args.frames, args.width, args.height, args.zimg_workers,
            up_usm_pool_effective_threads(bench.usm), bench.settled_frame,
            bench.adaptive.tuner.changes, bench.total_us / (double)args.frames,
            bench.tail_us / 512.0, bench.zimg_us / (double)args.frames,
            bench.usm_us / (double)args.frames);
    destroy(&bench);
    return rc;
}
