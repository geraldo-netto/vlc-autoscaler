// SPDX-License-Identifier: GPL-2.0-or-later
#include <vlc_common.h>
#include <vlc_picture.h>

#undef msg_Dbg
#define msg_Dbg(obj, ...) ((void)(obj))

#include "../src/scaler_swscale.c"

#include <stdio.h>
#include <string.h>

struct SwsContext { int tag; };

static struct SwsContext g_sws_ctx = { 1 };
static int g_get_fails;
static int g_scale_result;
static int g_free_calls;
static int g_src_w, g_src_h, g_dst_w, g_dst_h, g_flags;
static int g_slice_y, g_slice_h;
static const uint8_t *g_src[4];
static uint8_t *g_dst[4];
static int g_src_stride[4], g_dst_stride[4];

struct SwsContext *sws_getContext(
    int src_w, int src_h, enum AVPixelFormat src_format,
    int dst_w, int dst_h, enum AVPixelFormat dst_format, int flags,
    struct SwsFilter *src_filter, struct SwsFilter *dst_filter,
    const double *param)
{
    (void)src_format; (void)dst_format;
    (void)src_filter; (void)dst_filter; (void)param;
    g_src_w = src_w; g_src_h = src_h;
    g_dst_w = dst_w; g_dst_h = dst_h; g_flags = flags;
    return g_get_fails ? NULL : &g_sws_ctx;
}

int sws_scale(struct SwsContext *ctx, const uint8_t *const src[],
              const int src_stride[], int src_y, int src_h,
              uint8_t *const dst[], const int dst_stride[])
{
    (void)ctx;
    for (int i = 0; i < 4; i++) {
        g_src[i] = src[i]; g_src_stride[i] = src_stride[i];
        g_dst[i] = dst[i]; g_dst_stride[i] = dst_stride[i];
    }
    g_slice_y = src_y; g_slice_h = src_h;
    return g_scale_result;
}

void sws_freeContext(struct SwsContext *ctx)
{
    if (ctx) g_free_calls++;
}

static int g_run, g_fail, g_current_fail;

#define BEGIN(name) do { printf("  [....] %s\n", name); g_current_fail = 0; } while (0)
#define END() do { g_run++; if (g_current_fail) g_fail++; } while (0)
#define CHECK(cond) do { \
    if (!(cond)) { \
        printf("    %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        g_current_fail = 1; \
    } \
} while (0)

static scaler_ctx_t make_ctx(vlc_fourcc_t chroma, int algo)
{
    scaler_ctx_t ctx = {0};
    ctx.src_w = 320; ctx.src_h = 180;
    ctx.dst_w = 640; ctx.dst_h = 360;
    ctx.chroma = chroma; ctx.algo = algo;
    return ctx;
}

static void test_supports(void)
{
    BEGIN("supported chromas map; unknown chroma is rejected");
    const vlc_fourcc_t supported[] = {
        VLC_CODEC_I420, VLC_CODEC_YV12, VLC_CODEC_NV12, VLC_CODEC_NV21,
        VLC_CODEC_I422, VLC_CODEC_I444, VLC_CODEC_RGB24,
        VLC_CODEC_RGBA, VLC_CODEC_BGRA,
    };
    for (size_t i = 0; i < sizeof supported / sizeof supported[0]; i++)
        CHECK(sws_supports(supported[i], UP_ALGO_LANCZOS));
    CHECK(!sws_supports(VLC_FOURCC('B', 'A', 'D', '!'), UP_ALGO_LANCZOS));
    END();
}

static void test_open_close_algorithms(void)
{
    BEGIN("all algorithms open and close through the FFmpeg boundary");
    const int algos[] = { UP_ALGO_FAST_BILINEAR, UP_ALGO_BICUBIC,
                          UP_ALGO_LANCZOS, UP_ALGO_SPLINE36 };
    for (size_t i = 0; i < sizeof algos / sizeof algos[0]; i++) {
        scaler_ctx_t ctx = make_ctx(VLC_CODEC_I420, algos[i]);
        CHECK(sws_open(&ctx) == 0);
        CHECK(ctx.priv != NULL);
        CHECK(g_src_w == 320 && g_src_h == 180);
        CHECK(g_dst_w == 640 && g_dst_h == 360 && g_flags != 0);
        sws_close(&ctx);
        CHECK(ctx.priv == NULL);
    }
    scaler_ctx_t bad = make_ctx(VLC_FOURCC('B', 'A', 'D', '!'), 0);
    CHECK(sws_open(&bad) == -1);
    scaler_ctx_t fail = make_ctx(VLC_CODEC_I420, UP_ALGO_LANCZOS);
    g_get_fails = 1;
    CHECK(sws_open(&fail) == -1 && fail.priv == NULL);
    g_get_fails = 0;
    sws_close(&fail);
    END();
}

static void init_picture(picture_t *pic, uint8_t data[4], int pitch_base)
{
    memset(pic, 0, sizeof *pic);
    pic->i_planes = 4;
    for (int i = 0; i < 4; i++) {
        pic->p[i].p_pixels = &data[i];
        pic->p[i].i_pitch = pitch_base + i;
    }
}

static void test_process_status_and_forwarding(void)
{
    BEGIN("process forwards planes and distinguishes complete/transient/fatal");
    scaler_ctx_t ctx = make_ctx(VLC_CODEC_I420, UP_ALGO_LANCZOS);
    picture_t src, dst;
    uint8_t src_data[4] = {0}, dst_data[4] = {0};
    init_picture(&src, src_data, 100);
    init_picture(&dst, dst_data, 200);
    CHECK(sws_process(&ctx, &src, &dst) == SCALER_PROCESS_FATAL);

    sws_priv_t priv = { .ctx = &g_sws_ctx, .av_fmt = AV_PIX_FMT_YUV420P };
    ctx.priv = &priv;
    g_scale_result = ctx.dst_h;
    CHECK(sws_process(&ctx, &src, &dst) == SCALER_PROCESS_OK);
    CHECK(g_slice_y == 0 && g_slice_h == ctx.src_h);
    for (int i = 0; i < 4; i++) {
        CHECK(g_src[i] == &src_data[i] && g_src_stride[i] == 100 + i);
        CHECK(g_dst[i] == &dst_data[i] && g_dst_stride[i] == 200 + i);
    }
    g_scale_result = ctx.dst_h - 1;
    CHECK(sws_process(&ctx, &src, &dst) == SCALER_PROCESS_TRANSIENT);
    g_scale_result = -1;
    CHECK(sws_process(&ctx, &src, &dst) == SCALER_PROCESS_TRANSIENT);
    END();
}

static void test_close_without_context(void)
{
    BEGIN("close handles empty private state and empty FFmpeg context");
    scaler_ctx_t ctx = {0};
    sws_close(&ctx);
    sws_priv_t *priv = calloc(1, sizeof *priv);
    CHECK(priv != NULL);
    ctx.priv = priv;
    sws_close(&ctx);
    CHECK(ctx.priv == NULL);
    CHECK(g_free_calls == 4);
    END();
}

int main(void)
{
    printf("Running scaler_swscale contract tests...\n");
    test_supports();
    test_open_close_algorithms();
    test_process_status_and_forwarding();
    test_close_without_context();
    printf("\n%d tests run, %d failed\n", g_run, g_fail);
    return g_fail == 0 ? 0 : 1;
}
