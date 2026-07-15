// SPDX-License-Identifier: GPL-2.0-or-later
#define ZIMG_TEST_DEFINE_MODULE_NAME
#include "zimg_test_util.h"
#include "../src/usm_pool.h"
#include "../src/usm.h"
#include "../src/threading.h"
#include "cli_parse.h"

#include <stdio.h>
#include <sys/resource.h>
#include <time.h>

static int run_frame(scaler_ctx_t *ctx, zt_pic_t *src, zt_pic_t *dst,
                     usm_pool_t *usm, int amount)
{
    if (ctx->backend->process(ctx, &src->pic, &dst->pic)
            != SCALER_PROCESS_OK) return 1;
    plane_t *luma = &dst->pic.p[0];
    return up_usm_pool_apply(usm, luma->p_pixels, luma->i_pitch,
                             luma->p_pixels, luma->i_pitch, amount) != 0;
}

static double elapsed_us(const struct timespec *start,
                         const struct timespec *end, int frames)
{
    double ns = (end->tv_sec - start->tv_sec) * 1.0e9
              + (end->tv_nsec - start->tv_nsec);
    return ns / 1000.0 / (double)frames;
}

typedef struct {
    zt_pic_t src;
    zt_pic_t dst;
    scaler_ctx_t ctx;
    usm_pool_t *usm;
    int amount;
} pipeline_t;

static int pipeline_init(pipeline_t *p, int threads)
{
    *p = (pipeline_t){0};
    if (zt_pic_alloc(&p->src, VLC_CODEC_I420, 640, 360) != 0) return 1;
    if (zt_pic_alloc(&p->dst, VLC_CODEC_I420, 1280, 720) != 0) {
        zt_pic_free(&p->src);
        return 1;
    }
    zt_pic_fill(&p->src, 0x12345678u);
    zt_ctx_init(&p->ctx, VLC_CODEC_I420, 640, 360, 1280, 720, threads, 1);
    p->usm = up_usm_pool_create(threads, 1280, 720, 0);
    p->amount = up_usm_amount_pct_to_q8(20);
    return p->usm == NULL || p->ctx.backend->open(&p->ctx) != 0;
}

static void pipeline_destroy(pipeline_t *p)
{
    if (p->usm != NULL) up_usm_pool_destroy(p->usm);
    if (p->ctx.priv != NULL) p->ctx.backend->close(&p->ctx);
    zt_pic_free(&p->src);
    zt_pic_free(&p->dst);
}

static int pipeline_run_many(pipeline_t *p, int frames)
{
    for (int i = 0; i < frames; i++)
        if (run_frame(&p->ctx, &p->src, &p->dst, p->usm, p->amount)) return 1;
    return 0;
}

static int pipeline_time_many(pipeline_t *p, int frames, double *us)
{
    struct timespec start, end;
    if (clock_gettime(CLOCK_MONOTONIC, &start) != 0) return 1;
    if (pipeline_run_many(p, frames) != 0) return 1;
    if (clock_gettime(CLOCK_MONOTONIC, &end) != 0) return 1;
    *us = elapsed_us(&start, &end, frames);
    return 0;
}

static int time_pipeline(int threads, int frames, double *lazy_us,
                         long *max_rss_kb, double *frame_us)
{
    pipeline_t pipeline;
    int rc = pipeline_init(&pipeline, threads);
    if (!rc) rc = pipeline_time_many(&pipeline, 1, lazy_us);
    if (!rc) rc = pipeline_run_many(&pipeline, 4);
    if (!rc) rc = pipeline_time_many(&pipeline, frames, frame_us);
    struct rusage usage;
    if (!rc && getrusage(RUSAGE_SELF, &usage) == 0)
        *max_rss_kb = usage.ru_maxrss;
    else rc = 1;
    pipeline_destroy(&pipeline);
    return rc;
}

int main(int argc, char **argv)
{
    long threads = 0;
    long frames = 200;
    if (argc < 2 || !up_cli_parse_long(argv[1], 1, UP_THREADS_MAX, &threads) ||
        (argc >= 3 && !up_cli_parse_long(argv[2], 1, INT_MAX, &frames))) {
        fprintf(stderr, "usage: %s <threads> [frames]\n", argv[0]);
        return 2;
    }
    double lazy_us = 0.0;
    double frame_us = 0.0;
    long max_rss_kb = 0;
    if (time_pipeline((int)threads, (int)frames, &lazy_us,
                      &max_rss_kb, &frame_us) != 0) return 1;
    printf("%ld,%ld,%.2f,%ld,%.2f\n", threads, frames, lazy_us,
           max_rss_kb, frame_us);
    return 0;
}
