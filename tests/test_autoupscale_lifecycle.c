// SPDX-License-Identifier: GPL-2.0-or-later
/* Production lifecycle regression harness. It compiles autoupscale.c against
 * a narrow VLC boundary and drives the real static Open/Filter/Close callbacks.
 */

#include <string.h>

#include "test_harness.h"

#include "../src/autoupscale.c"

typedef struct
{
    const char *name;
    int value;
} lifecycle_config_t;

static const lifecycle_config_t lifecycle_config[] = {
    { "autoupscale-skip-above", 720 },
    { "autoupscale-target", 1 },
    { "autoupscale-algo", UP_ALGO_SPLINE36 },
    { "autoupscale-backend", SCALER_BACKEND_AUTO },
    { "autoupscale-usm", 0 },
    { "autoupscale-target-fps", 0 },
    { "autoupscale-threads", 1 },
    { "autoupscale-pin-threads", 0 },
    { "autoupscale-zerocopy-dst", 1 },
    { "autoupscale-zerocopy-src", 1 },
    { "autoupscale-content-probe", 0 },
    { "autoupscale-usm-stripe-min-rows", 0 },
    { "autoupscale-zimg-stripe-lines", 0 },
    { "autoupscale-usm-sharp-threshold", 0 },
};

static picture_t *g_next_output;
static scaler_process_status_t g_zimg_status;
static scaler_process_status_t g_swscale_status;
static int g_zimg_open_calls;
static int g_swscale_open_calls;
static int g_zimg_process_calls;
static int g_swscale_process_calls;
static int g_zimg_close_calls;
static int g_swscale_close_calls;
static int g_var_create_calls;
static int g_var_destroy_calls;

int64_t lifecycle_var_inherit(const char *name)
{
    for (size_t i = 0; i < sizeof lifecycle_config / sizeof lifecycle_config[0]; i++)
        if (strcmp(name, lifecycle_config[i].name) == 0)
            return lifecycle_config[i].value;
    return 0;
}

int lifecycle_var_create(const char *name)
{
    (void)name;
    g_var_create_calls++;
    return VLC_SUCCESS;
}

void lifecycle_var_destroy(const char *name)
{
    (void)name;
    g_var_destroy_calls++;
}

void lifecycle_var_set_integer(const char *name, int64_t value)
{
    (void)name;
    (void)value;
}

void picture_Release(picture_t *pic)
{
    pic->releases++;
}

void picture_CopyProperties(picture_t *dst, const picture_t *src)
{
    dst->properties_tag = src->properties_tag;
}

static picture_t *lifecycle_buffer_new(filter_t *filter)
{
    (void)filter;
    return g_next_output;
}

static int fake_supports(vlc_fourcc_t chroma, int algo)
{
    (void)chroma;
    (void)algo;
    return 1;
}

static int fake_open(scaler_ctx_t *ctx)
{
    if (ctx->backend->id == SCALER_BACKEND_ZIMG)
        g_zimg_open_calls++;
    else
        g_swscale_open_calls++;
    ctx->priv = ctx;
    return 0;
}

static scaler_process_status_t fake_process(scaler_ctx_t *ctx,
                                             const picture_t *src,
                                             const picture_t *dst)
{
    (void)src;
    (void)dst;
    if (ctx->backend->id == SCALER_BACKEND_ZIMG) {
        g_zimg_process_calls++;
        return g_zimg_status;
    }
    g_swscale_process_calls++;
    return g_swscale_status;
}

static void fake_close(scaler_ctx_t *ctx)
{
    if (ctx->backend->id == SCALER_BACKEND_ZIMG)
        g_zimg_close_calls++;
    else
        g_swscale_close_calls++;
    ctx->priv = NULL;
}

static const scaler_backend_t fake_zimg = {
    .name = "zimg",
    .id = SCALER_BACKEND_ZIMG,
    .supports = fake_supports,
    .open = fake_open,
    .process = fake_process,
    .close = fake_close,
};

static const scaler_backend_t fake_swscale = {
    .name = "swscale",
    .id = SCALER_BACKEND_SWSCALE,
    .supports = fake_supports,
    .open = fake_open,
    .process = fake_process,
    .close = fake_close,
};

const scaler_backend_t *scaler_pick(int pref, vlc_fourcc_t chroma, int algo)
{
    (void)chroma;
    (void)algo;
    if (pref == SCALER_BACKEND_SWSCALE) return &fake_swscale;
    if (pref == SCALER_BACKEND_ZIMG || pref == SCALER_BACKEND_AUTO)
        return &fake_zimg;
    return NULL;
}

usm_pool_t *up_usm_pool_create(int n_threads, int width, int height,
                               int stripe_min_rows)
{
    (void)n_threads;
    (void)width;
    (void)height;
    (void)stripe_min_rows;
    return NULL;
}

int up_usm_pool_apply(usm_pool_t *pool, uint8_t *dst, int dst_stride,
                      const uint8_t *src, int src_stride, int amount_q8)
{
    (void)pool;
    (void)dst;
    (void)dst_stride;
    (void)src;
    (void)src_stride;
    (void)amount_q8;
    return UP_USM_APPLY_FAILED_UNCHANGED;
}

void up_usm_pool_destroy(usm_pool_t *pool)
{
    (void)pool;
}

int up_usm_pool_effective_threads(const usm_pool_t *pool)
{
    (void)pool;
    return 0;
}

const char *up_usm_pool_variant_name = "test";

static void reset_state(void)
{
    g_next_output = NULL;
    g_zimg_status = SCALER_PROCESS_OK;
    g_swscale_status = SCALER_PROCESS_OK;
    g_zimg_open_calls = 0;
    g_swscale_open_calls = 0;
    g_zimg_process_calls = 0;
    g_swscale_process_calls = 0;
    g_zimg_close_calls = 0;
    g_swscale_close_calls = 0;
    g_var_create_calls = 0;
    g_var_destroy_calls = 0;
}

static void init_picture(picture_t *pic, int properties_tag)
{
    memset(pic, 0, sizeof(*pic));
    pic->i_planes = 3;
    pic->properties_tag = properties_tag;
}

static void init_filter(filter_t *filter)
{
    memset(filter, 0, sizeof(*filter));
    filter->fmt_in.video.i_chroma = VLC_CODEC_I420;
    filter->fmt_in.video.i_width = 320;
    filter->fmt_in.video.i_height = 180;
    filter->fmt_in.video.i_visible_width = 320;
    filter->fmt_in.video.i_visible_height = 180;
    filter->owner.video.buffer_new = lifecycle_buffer_new;
}

static void test_success_copies_properties_and_tears_down(void)
{
    BEGIN("Open/Filter/Close success copies properties and tears down stats");
    filter_t filter;
    picture_t input;
    picture_t output;
    reset_state();
    init_filter(&filter);
    init_picture(&input, 42);
    init_picture(&output, 0);
    g_next_output = &output;

    CHECK(Open((vlc_object_t *)&filter) == VLC_SUCCESS);
    CHECK(filter.pf_video_filter == Filter);
    CHECK(Filter(&filter, &input) == &output);
    CHECK(input.releases == 1);
    CHECK(output.releases == 0);
    CHECK(output.properties_tag == 42);
    CHECK(g_zimg_open_calls == 1 && g_zimg_process_calls == 1);
    Close((vlc_object_t *)&filter);
    CHECK(g_zimg_close_calls == 1);
    CHECK(g_var_create_calls == 3 && g_var_destroy_calls == 3);
    END();
}

static void test_output_allocation_failure_releases_input(void)
{
    BEGIN("output allocation failure drops and releases input");
    filter_t filter;
    picture_t input;
    reset_state();
    init_filter(&filter);
    init_picture(&input, 1);

    CHECK(Open((vlc_object_t *)&filter) == VLC_SUCCESS);
    CHECK(Filter(&filter, &input) == NULL);
    CHECK(input.releases == 1);
    CHECK(g_zimg_process_calls == 0);
    Close((vlc_object_t *)&filter);
    END();
}

static void test_transient_failure_keeps_backend(void)
{
    BEGIN("transient failure drops one frame without backend fallback");
    filter_t filter;
    picture_t failed_input;
    picture_t failed_output;
    picture_t retry_input;
    picture_t retry_output;
    reset_state();
    init_filter(&filter);
    init_picture(&failed_input, 1);
    init_picture(&failed_output, 0);
    init_picture(&retry_input, 2);
    init_picture(&retry_output, 0);
    g_zimg_status = SCALER_PROCESS_TRANSIENT;
    g_next_output = &failed_output;

    CHECK(Open((vlc_object_t *)&filter) == VLC_SUCCESS);
    CHECK(Filter(&filter, &failed_input) == NULL);
    CHECK(failed_input.releases == 1 && failed_output.releases == 1);
    CHECK(filter.p_sys->scaler.backend == &fake_zimg);
    g_zimg_status = SCALER_PROCESS_OK;
    g_next_output = &retry_output;
    CHECK(Filter(&filter, &retry_input) == &retry_output);
    CHECK(g_zimg_process_calls == 2 && g_swscale_process_calls == 0);
    Close((vlc_object_t *)&filter);
    END();
}

static void test_fatal_failure_falls_back_once(void)
{
    BEGIN("fatal zimg failure switches once to swscale");
    filter_t filter;
    picture_t failed_input;
    picture_t failed_output;
    picture_t retry_input;
    picture_t retry_output;
    reset_state();
    init_filter(&filter);
    init_picture(&failed_input, 1);
    init_picture(&failed_output, 0);
    init_picture(&retry_input, 2);
    init_picture(&retry_output, 0);
    g_zimg_status = SCALER_PROCESS_FATAL;
    g_next_output = &failed_output;

    CHECK(Open((vlc_object_t *)&filter) == VLC_SUCCESS);
    CHECK(Filter(&filter, &failed_input) == NULL);
    CHECK(failed_input.releases == 1 && failed_output.releases == 1);
    CHECK(g_zimg_close_calls == 1 && g_swscale_open_calls == 1);
    CHECK(filter.p_sys->scaler.backend == &fake_swscale);
    g_next_output = &retry_output;
    CHECK(Filter(&filter, &retry_input) == &retry_output);
    CHECK(g_swscale_process_calls == 1);
    Close((vlc_object_t *)&filter);
    CHECK(g_swscale_close_calls == 1);
    END();
}

int main(void)
{
    test_success_copies_properties_and_tears_down();
    test_output_allocation_failure_releases_input();
    test_transient_failure_keeps_backend();
    test_fatal_failure_falls_back_once();
    return test_harness_report();
}
