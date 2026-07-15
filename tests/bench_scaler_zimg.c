// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * bench_scaler_zimg.c — wall-clock throughput for the zimg backend.
 *
 * Usage: bench_scaler_zimg <threads> <chroma> <sw> <sh> <dw> <dh> [frames]
 *                          [zc] [pin]
 *   chroma: i420 | yv12 | i422 | i444   frames default 200   zc default 1
 *
 * Output CSV: threads,chroma,sw x sh,dw x dh,frames,zc,pin,us_per_frame
 *
 * Source is filled once; only process() is timed (resampling is content-
 * independent in cost). This is the vehicle for measuring PERF-1 (parallel
 * copy-in) and the zimg half of SCAL-2.
 */
#define ZIMG_TEST_DEFINE_MODULE_NAME
#include "zimg_test_util.h"
#include "cli_parse.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

static uint32_t chroma_of(const char *s)
{
    if (!strcmp(s, "i420")) return VLC_CODEC_I420;
    if (!strcmp(s, "yv12")) return VLC_CODEC_YV12;
    if (!strcmp(s, "i422")) return VLC_CODEC_I422;
    if (!strcmp(s, "i444")) return VLC_CODEC_I444;
    return 0;
}

struct bargs {
    int threads, sw, sh, dw, dh, frames, zc, pin;
    uint32_t chroma;
    const char *chroma_name;
};

static int parse(int argc, char **argv, struct bargs *a)
{
    if (argc < 7) {
        fprintf(stderr, "usage: %s <threads> <chroma> <sw> <sh> <dw> <dh> "
                        "[frames] [zc] [pin]\n", argv[0]);
        return 2;
    }
    a->chroma_name = argv[2];
    a->chroma  = chroma_of(argv[2]);
    a->frames = 200;
    a->zc = 1;
    a->pin = 0;

    const struct {
        int position;
        long minimum;
        long maximum;
        int *destination;
    } numeric[] = {
        { 1, 1, INT_MAX, &a->threads },
        { 3, 8, UP_MAX_DIM, &a->sw },
        { 4, 8, UP_MAX_DIM, &a->sh },
        { 5, 8, UP_MAX_DIM, &a->dw },
        { 6, 8, UP_MAX_DIM, &a->dh },
        { 7, 1, INT_MAX, &a->frames },
        { 8, 0, 1, &a->zc },
        { 9, 0, 1, &a->pin },
    };
    for (size_t i = 0; i < sizeof numeric / sizeof *numeric; i++) {
        if (numeric[i].position >= argc) continue;
        long value;
        if (!up_cli_parse_long(argv[numeric[i].position], numeric[i].minimum,
                               numeric[i].maximum, &value)) {
            fprintf(stderr, "bad numeric argument '%s'\n",
                    argv[numeric[i].position]);
            return 2;
        }
        *numeric[i].destination = (int)value;
    }
    if (!a->chroma) {
        fprintf(stderr, "bad chroma (expected i420|yv12|i422|i444)\n");
        return 2;
    }
    return 0;
}

static int time_frames(scaler_ctx_t *ctx, zt_pic_t *src, zt_pic_t *dst,
                       int frames, double *us_per_frame)
{
    struct timespec t0, t1;
    int rc = 0;
    if (clock_gettime(CLOCK_MONOTONIC, &t0) != 0) rc = 1;
    for (int i = 0; i < frames; i++)
        if (ctx->backend->process(ctx, &src->pic, &dst->pic)
                != SCALER_PROCESS_OK) rc = 1;
    if (clock_gettime(CLOCK_MONOTONIC, &t1) != 0) rc = 1;
    if (rc != 0) {
        fprintf(stderr, "timing/process failure; no measurement\n");
        return 1;
    }
    double ns = (t1.tv_sec - t0.tv_sec) * 1.0e9 + (t1.tv_nsec - t0.tv_nsec);
    *us_per_frame = (ns / 1000.0) / (double)frames;
    return 0;
}

static int run_timed(const struct bargs *a, double *us_per_frame)
{
    zt_pic_t src, dst;
    if (zt_pic_alloc(&src, a->chroma, a->sw, a->sh) != 0) return 1;
    if (zt_pic_alloc(&dst, a->chroma, a->dw, a->dh) != 0) {
        zt_pic_free(&src);
        return 1;
    }
    zt_pic_fill(&src, 0xABCDEF01u);

    scaler_ctx_t ctx;
    zt_ctx_init(&ctx, a->chroma, a->sw, a->sh, a->dw, a->dh, a->threads, a->zc);
    ctx.pin_cpus = a->pin;
    int rc = 1;
    if (ctx.backend->open(&ctx) == 0) {
        rc = 0;
        for (int i = 0; i < 5; i++)
            if (ctx.backend->process(&ctx, &src.pic, &dst.pic)
                    != SCALER_PROCESS_OK) rc = 1;

        if (rc == 0 && time_frames(&ctx, &src, &dst, a->frames,
                                   us_per_frame) != 0)
            rc = 1;
        ctx.backend->close(&ctx);
    }
    zt_pic_free(&src);
    zt_pic_free(&dst);
    return rc;
}

int main(int argc, char **argv)
{
    struct bargs a;
    int rc = parse(argc, argv, &a);
    if (rc) return rc;

    double us = 0.0;
    if (run_timed(&a, &us) != 0) {
        fprintf(stderr, "bench run failed\n");
        return 1;
    }
    printf("%d,%s,%dx%d,%dx%d,%d,%d,%d,%.2f\n",
           a.threads, a.chroma_name, a.sw, a.sh, a.dw, a.dh,
           a.frames, a.zc, a.pin, us);
    return 0;
}
