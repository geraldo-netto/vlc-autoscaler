// SPDX-License-Identifier: GPL-2.0-or-later
#include <vlc_common.h>
#include <vlc_picture.h>

#undef msg_Dbg
#define msg_Dbg(obj, ...) ((void)(obj))

#include "../src/scaler_swscale.c"

#include <stdio.h>
#include <string.h>

/* cppcheck-suppress unusedStructMember ; keeps the stub struct non-empty */
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

/* cppcheck-suppress constParameterPointer ; mirrors the FFmpeg prototype */
void sws_freeContext(struct SwsContext *ctx)
{
    if (ctx) g_free_calls++;
}

#include "test_harness.h"

static scaler_ctx_t make_ctx(vlc_fourcc_t chroma, int algo)
{
    scaler_ctx_t ctx = {0};
    ctx.src_w = 320; ctx.src_h = 180;
    ctx.src_coded_w = 320; ctx.src_coded_h = 180;
    ctx.dst_w = 640; ctx.dst_h = 360;
    ctx.chroma = chroma; ctx.algo = algo;
    return ctx;
}

static void test_picture_regions(void)
{
    BEGIN("scaler context owns source and destination picture regions");
    scaler_ctx_t ctx = make_ctx(VLC_CODEC_I420, UP_ALGO_LANCZOS);
    ctx.src_coded_w = 352;
    ctx.src_coded_h = 208;
    ctx.src_x_offset = 16;
    ctx.src_y_offset = 8;

    const up_picture_region_t src = up_scaler_src_region(&ctx);
    CHECK(src.coded_width == 352 && src.coded_height == 208);
    CHECK(src.x_offset == 16 && src.y_offset == 8);
    CHECK(src.width == ctx.src_w && src.height == ctx.src_h);

    const up_picture_region_t dst = up_scaler_dst_region(&ctx);
    CHECK(dst.coded_width == (unsigned)ctx.dst_w);
    CHECK(dst.coded_height == (unsigned)ctx.dst_h);
    CHECK(dst.x_offset == 0 && dst.y_offset == 0);
    CHECK(dst.width == ctx.dst_w && dst.height == ctx.dst_h);
    END();
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

static void init_picture(picture_t *pic, uint8_t data[4], int pitch_base,
                         vlc_fourcc_t chroma, int width, int height)
{
    memset(pic, 0, sizeof *pic);
    pic->i_planes = 4;
    pic->format.i_chroma = chroma;
    pic->format.i_width = (unsigned)width;
    pic->format.i_height = (unsigned)height;
    pic->format.i_visible_width = (unsigned)width;
    pic->format.i_visible_height = (unsigned)height;
    for (int i = 0; i < 4; i++) {
        pic->p[i].p_pixels = &data[i];
        pic->p[i].i_pitch = pitch_base + i;
        pic->p[i].i_lines = 400;
        pic->p[i].i_pixel_pitch = 1;
        pic->p[i].i_visible_pitch = pitch_base + i;
        pic->p[i].i_visible_lines = 400;
    }
}

static void test_process_status_and_forwarding(void)
{
    BEGIN("process forwards planes and distinguishes complete/transient/fatal");
    scaler_ctx_t ctx = make_ctx(VLC_CODEC_I420, UP_ALGO_LANCZOS);
    picture_t src, dst;
    uint8_t src_data[4] = {0}, dst_data[4] = {0};
    init_picture(&src, src_data, 400, ctx.chroma, ctx.src_w, ctx.src_h);
    init_picture(&dst, dst_data, 700, ctx.chroma, ctx.dst_w, ctx.dst_h);
    CHECK(sws_process(&ctx, &src, &dst) == SCALER_PROCESS_FATAL);

    sws_priv_t priv = { .ctx = &g_sws_ctx, .av_fmt = AV_PIX_FMT_YUV420P };
    ctx.priv = &priv;
    g_scale_result = ctx.dst_h;
    CHECK(sws_process(&ctx, &src, &dst) == SCALER_PROCESS_OK);
    CHECK(g_slice_y == 0 && g_slice_h == ctx.src_h);
    for (int i = 0; i < 3; i++) {
        CHECK(g_src[i] == &src_data[i] && g_src_stride[i] == 400 + i);
        CHECK(g_dst[i] == &dst_data[i] && g_dst_stride[i] == 700 + i);
    }
    CHECK(g_src[3] == NULL && g_dst[3] == NULL);
    g_scale_result = ctx.dst_h - 1;
    CHECK(sws_process(&ctx, &src, &dst) == SCALER_PROCESS_TRANSIENT);
    g_scale_result = -1;
    CHECK(sws_process(&ctx, &src, &dst) == SCALER_PROCESS_TRANSIENT);
    END();
}

static void test_geometry_rejection_recovers(void)
{
    BEGIN("malformed geometry is transient and the next frame recovers");
    scaler_ctx_t ctx = make_ctx(VLC_CODEC_I420, UP_ALGO_LANCZOS);
    sws_priv_t priv = { .ctx = &g_sws_ctx, .av_fmt = AV_PIX_FMT_YUV420P };
    picture_t src, dst;
    uint8_t src_data[4] = {0}, dst_data[4] = {0};
    init_picture(&src, src_data, 400, ctx.chroma, ctx.src_w, ctx.src_h);
    init_picture(&dst, dst_data, 700, ctx.chroma, ctx.dst_w, ctx.dst_h);
    ctx.priv = &priv;
    g_scale_result = ctx.dst_h;

    uint8_t *src_u = src.p[1].p_pixels;
    src.p[1].p_pixels = NULL;
    CHECK(sws_process(&ctx, &src, &dst) == SCALER_PROCESS_TRANSIENT);
    src.p[1].p_pixels = src_u;

    int dst_lines = dst.p[0].i_lines;
    dst.p[0].i_lines = ctx.dst_h - 1;
    CHECK(sws_process(&ctx, &src, &dst) == SCALER_PROCESS_TRANSIENT);
    dst.p[0].i_lines = dst_lines;

    int src_pitch = src.p[0].i_pitch;
    src.p[0].i_pitch = ctx.src_w - 1;
    CHECK(sws_process(&ctx, &src, &dst) == SCALER_PROCESS_TRANSIENT);
    src.p[0].i_pitch = src_pitch;
    CHECK(sws_process(&ctx, &src, &dst) == SCALER_PROCESS_OK);
    END();
}

#define CROP_STORAGE_BYTES 16384

typedef struct {
    vlc_fourcc_t chroma;
    int planes;
    int pixel_pitch[3];
    int x_group_pixels[3];
    int x_group_bytes[3];
    int logical_to_physical[3];
    int y_group_pixels[3];
} crop_case_t;

static void init_crop_picture(picture_t *pic,
                              uint8_t storage[4][CROP_STORAGE_BYTES],
                              const crop_case_t *c, int width, int height,
                              int x_offset, int y_offset)
{
    memset(pic, 0, sizeof *pic);
    pic->i_planes = c->planes;
    pic->format.i_chroma = c->chroma;
    pic->format.i_width = (unsigned)(width + x_offset + 4);
    pic->format.i_height = (unsigned)(height + y_offset + 4);
    pic->format.i_visible_width = (unsigned)width;
    pic->format.i_visible_height = (unsigned)height;
    for (int i = 0; i < c->planes; i++) {
        pic->p[i].p_pixels = storage[i];
        pic->p[i].i_pitch = 160 + i * 8;
        pic->p[i].i_lines = 64;
        pic->p[i].i_pixel_pitch = c->pixel_pitch[i];
        pic->p[i].i_visible_pitch = pic->p[i].i_pitch;
        pic->p[i].i_visible_lines = pic->p[i].i_lines;
    }
}

static uint8_t *crop_expected_pointer(
    picture_t *pic, const crop_case_t *c, int logical_plane,
    int x_offset, int y_offset)
{
    const int physical = c->logical_to_physical[logical_plane];
    plane_t *plane = &pic->p[physical];
    const int x = x_offset / c->x_group_pixels[logical_plane];
    const int y = y_offset / c->y_group_pixels[logical_plane];
    return plane->p_pixels + (size_t)y * (size_t)plane->i_pitch
         + (size_t)x * (size_t)c->x_group_bytes[logical_plane];
}

static void test_cropped_plane_forwarding(void)
{
    BEGIN("I420/YV12/NV12/RGB crops forward offset physical planes");
    static uint8_t src_storage[4][CROP_STORAGE_BYTES];
    static uint8_t dst_storage[4][CROP_STORAGE_BYTES];
    static const crop_case_t cases[] = {
        { VLC_CODEC_I420, 3, {1, 1, 1}, {1, 2, 2}, {1, 1, 1},
          {0, 1, 2}, {1, 2, 2} },
        { VLC_CODEC_YV12, 3, {1, 1, 1}, {1, 2, 2}, {1, 1, 1},
          {0, 2, 1}, {1, 2, 2} },
        { VLC_CODEC_NV12, 2, {1, 1, 0}, {1, 2, 0}, {1, 2, 0},
          {0, 1, 0}, {1, 2, 0} },
        { VLC_CODEC_RGB24, 1, {3, 0, 0}, {1, 0, 0}, {3, 0, 0},
          {0, 0, 0}, {1, 0, 0} },
    };
    const int x_offset = 3, y_offset = 1;
    for (size_t n = 0; n < sizeof cases / sizeof cases[0]; n++) {
        const crop_case_t *c = &cases[n];
        scaler_ctx_t ctx = make_ctx(c->chroma, UP_ALGO_LANCZOS);
        ctx.src_w = 16; ctx.src_h = 8;
        ctx.dst_w = 32; ctx.dst_h = 16;
        sws_priv_t priv = { .ctx = &g_sws_ctx,
                            .av_fmt = ChromaToAVFmt(c->chroma) };
        picture_t src, dst;
        init_crop_picture(&src, src_storage, c, ctx.src_w, ctx.src_h,
                          x_offset, y_offset);
        init_crop_picture(&dst, dst_storage, c, ctx.dst_w, ctx.dst_h,
                          0, 0);
        ctx.src_coded_w = src.format.i_width;
        ctx.src_coded_h = src.format.i_height;
        ctx.src_x_offset = (unsigned)x_offset;
        ctx.src_y_offset = (unsigned)y_offset;
        ctx.priv = &priv;
        g_scale_result = ctx.dst_h;
        CHECK(sws_process(&ctx, &src, &dst) == SCALER_PROCESS_OK);
        for (int i = 0; i < c->planes; i++) {
            const int physical = c->logical_to_physical[i];
            CHECK(g_src[i] == crop_expected_pointer(
                &src, c, i, x_offset, y_offset));
            CHECK(g_dst[i] == crop_expected_pointer(
                &dst, c, i, 0, 0));
            CHECK(g_src_stride[i] == src.p[physical].i_pitch);
            CHECK(g_dst_stride[i] == dst.p[physical].i_pitch);
        }
    }
    END();
}

static void test_close_without_context(void)
{
    BEGIN("close handles empty private state and empty FFmpeg context");
    const int free_calls_before = g_free_calls;
    scaler_ctx_t ctx = {0};
    sws_close(&ctx);   /* priv NULL: nothing to free */
    sws_priv_t *priv = calloc(1, sizeof *priv);
    CHECK(priv != NULL);
    ctx.priv = priv;
    sws_close(&ctx);   /* priv set, sws NULL: still no context free */
    CHECK(ctx.priv == NULL);
    CHECK(g_free_calls == free_calls_before);
    END();
}

int main(void)
{
    printf("Running scaler_swscale contract tests...\n");
    test_picture_regions();
    test_supports();
    test_open_close_algorithms();
    test_process_status_and_forwarding();
    test_geometry_rejection_recovers();
    test_cropped_plane_forwarding();
    test_close_without_context();
    return test_harness_report();
}
