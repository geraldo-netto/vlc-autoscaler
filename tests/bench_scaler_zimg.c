// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * bench_scaler_zimg.c — wall-clock throughput for the zimg backend.
 *
 * Usage: bench_scaler_zimg <threads> <chroma> <sw> <sh> <dw> <dh> [frames] [zc]
 *   chroma: i420 | yv12 | i422 | i444   frames default 200   zc default 1
 *
 * Output CSV: threads,chroma,sw x sh,dw x dh,frames,zc,us_per_frame
 *
 * Source is filled once; only process() is timed (resampling is content-
 * independent in cost). This is the vehicle for measuring PERF-1 (parallel
 * copy-in) and the zimg half of SCAL-2.
 */
#define ZIMG_TEST_DEFINE_MODULE_NAME
#include "zimg_test_util.h"

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
    int threads, sw, sh, dw, dh, frames, zc;
    uint32_t chroma;
    const char *chroma_name;
};

static int bargs_valid(const struct bargs *a)
{
    return a->chroma && a->threads >= 1 && a->sw >= 8 && a->sh >= 8
        && a->dw >= 8 && a->dh >= 8 && a->frames >= 1;
}

static int parse(int argc, char **argv, struct bargs *a)
{
    if (argc < 7) {
        fprintf(stderr, "usage: %s <threads> <chroma> <sw> <sh> <dw> <dh> "
                        "[frames] [zc]\n", argv[0]);
        return 2;
    }
    a->threads = atoi(argv[1]);
    a->chroma_name = argv[2];
    a->chroma  = chroma_of(argv[2]);
    a->sw = atoi(argv[3]); a->sh = atoi(argv[4]);
    a->dw = atoi(argv[5]); a->dh = atoi(argv[6]);
    a->frames = (argc >= 8) ? atoi(argv[7]) : 200;
    a->zc     = (argc >= 9) ? atoi(argv[8]) : 1;
    if (!bargs_valid(a)) {
        fprintf(stderr, "bad args (chroma i420|yv12|i422|i444; dims >= 8)\n");
        return 2;
    }
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
    int rc = 1;
    if (ctx.backend->open(&ctx) == 0) {
        rc = 0;
        for (int i = 0; i < 5; i++)
            if (ctx.backend->process(&ctx, &src.pic, &dst.pic) != 0) rc = 1;

        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        for (int i = 0; i < a->frames; i++)
            if (ctx.backend->process(&ctx, &src.pic, &dst.pic) != 0) rc = 1;
        clock_gettime(CLOCK_MONOTONIC, &t1);

        double ns = (t1.tv_sec - t0.tv_sec) * 1.0e9
                  + (t1.tv_nsec - t0.tv_nsec);
        *us_per_frame = (ns / 1000.0) / (double)a->frames;
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
    printf("%d,%s,%dx%d,%dx%d,%d,%d,%.2f\n",
           a.threads, a.chroma_name, a.sw, a.sh, a.dw, a.dh,
           a.frames, a.zc, us);
    return 0;
}
