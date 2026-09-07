// SPDX-License-Identifier: GPL-2.0-or-later
/* Production lifecycle regression harness. It compiles the module and
 * lifecycle implementation against a narrow VLC boundary and drives the real
 * guarded Open, Filter, and Close callbacks.
 */

#include <string.h>

#include "test_harness.h"

#include "../src/autoupscale.c"

static int lifecycle_cpu_supported = 1;

static int lifecycle_cpu_supports_v3(void)
{
    return lifecycle_cpu_supported;
}

#define UP_REQUIRED_CPU_LEVEL 3
#define UP_CPU_SUPPORTS_V3() lifecycle_cpu_supports_v3()
#include "../src/autoupscale_module.c"
#undef UP_CPU_SUPPORTS_V3
#undef UP_REQUIRED_CPU_LEVEL

typedef struct
{
    const char *name;
    int value;
} lifecycle_config_t;

static lifecycle_config_t lifecycle_config[] = {
    { "autoupscale-skip-above", 720 },
    { "autoupscale-target", 1 },
    { "autoupscale-algo", UP_ALGO_SPLINE36 },
    { "autoupscale-backend", SCALER_BACKEND_AUTO },
    { "autoupscale-usm", 0 },
    { "autoupscale-threads", 1 },
    { "autoupscale-pin-threads", 0 },
    { "autoupscale-zerocopy-dst", 1 },
    { "autoupscale-zerocopy-src", 1 },
    { "autoupscale-content-probe", 0 },
    { "autoupscale-usm-stripe-min-rows", 0 },
    { "autoupscale-zimg-stripe-lines", 0 },
    { "autoupscale-usm-sharp-threshold", 0 },
    { "autoupscale-adaptive-usm", 0 },
};

static const int lifecycle_config_defaults[] = {
    720, 1, UP_ALGO_SPLINE36, SCALER_BACKEND_AUTO, 0, 1, 0,
    1, 1, 0, 0, 0, 0, 0,
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
static int g_picker_none;
static int g_zimg_open_result;
static int g_swscale_open_result;
static usm_pool_t *g_usm_pool_create_result;
static int g_usm_apply_status;
static int g_usm_apply_calls;
static int g_usm_destroy_calls;
static int g_usm_effective_threads;
static int g_usm_requested_threads;
static int g_usm_width, g_usm_height;

int64_t lifecycle_var_inherit(const char *name)
{
    for (size_t i = 0; i < sizeof lifecycle_config / sizeof lifecycle_config[0]; i++)
        if (strcmp(name, lifecycle_config[i].name) == 0)
            return lifecycle_config[i].value;
    return 0;
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
    if (ctx->backend->id == SCALER_BACKEND_ZIMG) {
        g_zimg_open_calls++;
        if (g_zimg_open_result != 0) return -1;
    } else {
        g_swscale_open_calls++;
        if (g_swscale_open_result != 0) return -1;
    }
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
    if (g_picker_none) return NULL;
    if (pref == SCALER_BACKEND_SWSCALE) return &fake_swscale;
    if (pref == SCALER_BACKEND_ZIMG || pref == SCALER_BACKEND_AUTO)
        return &fake_zimg;
    return NULL;
}

usm_pool_t *up_usm_pool_create(int n_threads, int width, int height,
                               int stripe_min_rows)
{
    g_usm_requested_threads = n_threads;
    g_usm_width = width;
    g_usm_height = height;
    (void)stripe_min_rows;
    return g_usm_pool_create_result;
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
    g_usm_apply_calls++;
    return g_usm_apply_status;
}

void up_usm_pool_destroy(usm_pool_t *pool)
{
    (void)pool;
    g_usm_destroy_calls++;
}

int up_usm_pool_effective_threads(const usm_pool_t *pool)
{
    (void)pool;
    return g_usm_effective_threads;
}

const char *up_usm_pool_variant_name = "test";

static void reset_state(void)
{
    for (size_t i = 0; i < sizeof lifecycle_config / sizeof lifecycle_config[0]; i++)
        lifecycle_config[i].value = lifecycle_config_defaults[i];
    g_next_output = NULL;
    g_zimg_status = SCALER_PROCESS_OK;
    g_swscale_status = SCALER_PROCESS_OK;
    g_zimg_open_calls = 0;
    g_swscale_open_calls = 0;
    g_zimg_process_calls = 0;
    g_swscale_process_calls = 0;
    g_zimg_close_calls = 0;
    g_swscale_close_calls = 0;
    g_picker_none = 0;
    g_zimg_open_result = 0;
    g_swscale_open_result = 0;
    g_usm_pool_create_result = NULL;
    g_usm_apply_status = UP_USM_APPLY_OK;
    g_usm_apply_calls = 0;
    g_usm_destroy_calls = 0;
    g_usm_effective_threads = 1;
    g_usm_requested_threads = 0;
    g_usm_width = g_usm_height = 0;
    lifecycle_cpu_supported = 1;
}

static void set_config(const char *name, int value)
{
    for (size_t i = 0; i < sizeof lifecycle_config / sizeof lifecycle_config[0]; i++) {
        if (strcmp(name, lifecycle_config[i].name) == 0) {
            lifecycle_config[i].value = value;
            return;
        }
    }
}

static void init_picture(picture_t *pic, int properties_tag)
{
    memset(pic, 0, sizeof(*pic));
    pic->i_planes = 3;
    pic->properties_tag = properties_tag;
}

static void init_i420_picture(picture_t *pic, uint8_t *storage,
                              int width, int height, int properties_tag)
{
    const int chroma_width = (width + 1) / 2;
    const int chroma_height = (height + 1) / 2;
    const size_t y_size = (size_t)width * (size_t)height;
    const size_t chroma_size = (size_t)chroma_width * (size_t)chroma_height;
    init_picture(pic, properties_tag);
    pic->format.i_chroma = VLC_CODEC_I420;
    pic->format.i_width = (unsigned)width;
    pic->format.i_height = (unsigned)height;
    pic->format.i_visible_width = (unsigned)width;
    pic->format.i_visible_height = (unsigned)height;
    pic->p[0] = (plane_t) { storage, height, width, 1, height, width };
    pic->p[1] = (plane_t) { storage + y_size, chroma_height, chroma_width,
                            1, chroma_height, chroma_width };
    pic->p[2] = (plane_t) { storage + y_size + chroma_size, chroma_height,
                            chroma_width, 1, chroma_height, chroma_width };
}

static void fill_checkerboard(uint8_t *pixels, int width, int height)
{
    for (int y = 0; y < height; y++)
        for (int x = 0; x < width; x++)
            pixels[(size_t)y * (size_t)width + (size_t)x] =
                ((x + y) & 1) ? 255 : 0;
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
    BEGIN("Open/Filter/Close success copies properties and tears down");
    filter_t filter;
    picture_t input;
    picture_t output;
    reset_state();
    init_filter(&filter);
    init_picture(&input, 42);
    init_picture(&output, 0);
    g_next_output = &output;

    CHECK(up_autoupscale_open_checked((vlc_object_t *)&filter) == VLC_SUCCESS);
    CHECK(filter.pf_video_filter == Filter);
    CHECK(Filter(&filter, &input) == &output);
    CHECK(input.releases == 1);
    CHECK(output.releases == 0);
    CHECK(output.properties_tag == 42);
    CHECK(g_zimg_open_calls == 1 && g_zimg_process_calls == 1);
    Close((vlc_object_t *)&filter);
    CHECK(g_zimg_close_calls == 1);
    END();
}

static void test_cpu_gate_rejects_before_open(void)
{
    BEGIN("CPU gate rejects before lifecycle implementation");
    filter_t filter;
    reset_state();
    init_filter(&filter);
    lifecycle_cpu_supported = 0;

    CHECK(up_autoupscale_open_checked((vlc_object_t *)&filter)
          == VLC_EGENERIC);
    CHECK(g_zimg_open_calls == 0 && g_swscale_open_calls == 0);
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

    CHECK(up_autoupscale_open_checked((vlc_object_t *)&filter) == VLC_SUCCESS);
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

    CHECK(up_autoupscale_open_checked((vlc_object_t *)&filter) == VLC_SUCCESS);
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

    CHECK(up_autoupscale_open_checked((vlc_object_t *)&filter) == VLC_SUCCESS);
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

static void test_open_rejections_and_open_fallback(void)
{
    BEGIN("Open rejects ineligible inputs and recovers AUTO open failure");
    filter_t filter;
    reset_state();
    init_filter(&filter);
    set_config("autoupscale-target", UP_TARGET_AUTO);
    set_config("autoupscale-skip-above", 180);
    CHECK(up_autoupscale_open_checked((vlc_object_t *)&filter)
          == VLC_EGENERIC);

    reset_state();
    init_filter(&filter);
    filter.fmt_in.video.i_chroma = VLC_FOURCC('V', 'A', 'O', 'P');
    CHECK(up_autoupscale_open_checked((vlc_object_t *)&filter)
          == VLC_EGENERIC);

    reset_state();
    init_filter(&filter);
    g_picker_none = 1;
    CHECK(up_autoupscale_open_checked((vlc_object_t *)&filter)
          == VLC_EGENERIC);

    reset_state();
    init_filter(&filter);
    g_zimg_open_result = -1;
    const int info_before = lifecycle_info_count;
    const int warn_before = lifecycle_warn_count;
    CHECK(up_autoupscale_open_checked((vlc_object_t *)&filter) == VLC_SUCCESS);
    CHECK(g_zimg_open_calls == 1 && g_swscale_open_calls == 1);
    CHECK(filter.p_sys->scaler.backend == &fake_swscale);
    /* OBS-3: the fallback reason must be visible at default verbosity
     * (msg_Info), never demoted to the suppressed msg_Warn. */
    CHECK(lifecycle_info_count > info_before);
    CHECK(lifecycle_warn_count == warn_before);
    Close((vlc_object_t *)&filter);

    reset_state();
    init_filter(&filter);
    g_zimg_open_result = -1;
    g_swscale_open_result = -1;
    CHECK(up_autoupscale_open_checked((vlc_object_t *)&filter)
          == VLC_EGENERIC);
    END();
}

static void test_open_usm_contract(void)
{
    BEGIN("Open keeps scaling when optional USM is available");
    filter_t filter;
    reset_state();
    init_filter(&filter);
    set_config("autoupscale-usm", 20);
    g_usm_pool_create_result = (usm_pool_t *)&filter;
    CHECK(up_autoupscale_open_checked((vlc_object_t *)&filter) == VLC_SUCCESS);
    CHECK(filter.p_sys->usm_pool == (usm_pool_t *)&filter);
    CHECK(filter.p_sys->usm_amount_q8 > 0);
    Close((vlc_object_t *)&filter);
    CHECK(g_usm_destroy_calls == 1);

    END();
}

static void check_open_usm_workers(int preset, int preference, int expected)
{
    filter_t filter;
    reset_state();
    init_filter(&filter);
    filter.fmt_in.video.i_width = filter.fmt_in.video.i_visible_width = 960;
    filter.fmt_in.video.i_height = filter.fmt_in.video.i_visible_height = 540;
    set_config("autoupscale-usm", 20);
    set_config("autoupscale-target", preset);
    set_config("autoupscale-threads", preference);
    g_usm_pool_create_result = (usm_pool_t *)&filter;
    const int rc = up_autoupscale_open_checked((vlc_object_t *)&filter);
    CHECK(rc == VLC_SUCCESS);
    if (rc != VLC_SUCCESS) return;
    CHECK(g_usm_requested_threads == up_threads_decide(expected, up_detect_cores()));
    CHECK(g_usm_height == UP_PRESET_HEIGHTS[preset]);
    CHECK(g_usm_width == g_usm_height * 16 / 9);
    CHECK(filter.p_sys->scaler.threads_pref == preference);
    CHECK(filter.p_sys->scaler.dst_w == g_usm_width);
    CHECK(filter.p_sys->scaler.dst_h == g_usm_height);
    CHECK(!filter.p_sys->usm_adaptive.enabled);
    Close((vlc_object_t *)&filter);
    CHECK(g_usm_destroy_calls == 1);
}

static void test_open_usm_worker_policy(void)
{
    BEGIN("Open sizes USM from output geometry and preserves scaler preference");
    check_open_usm_workers(UP_TARGET_720P, 0, 8);
    check_open_usm_workers(UP_TARGET_1080P, 0, 12);
    check_open_usm_workers(UP_TARGET_4K, 0, 16);
    check_open_usm_workers(UP_TARGET_4K, 4, 4);
    END();
}

static void init_scaler(scaler_ctx_t *scaler, int width, int height)
{
    *scaler = (scaler_ctx_t) {
        .backend = &fake_zimg,
        .src_w = width,
        .src_h = height,
        .src_coded_w = (unsigned)width,
        .src_coded_h = (unsigned)height,
        .dst_w = width,
        .dst_h = height,
        .chroma = VLC_CODEC_I420,
    };
}

static void test_usm_success_failure_and_uncertain_output(void)
{
    BEGIN("USM success, safe failure, and uncertain output ownership");
    uint8_t pixels[384] = { 0 };
    filter_t filter;
    filter_sys_t sys = { 0 };
    picture_t input;
    picture_t output;
    reset_state();
    init_filter(&filter);
    init_scaler(&sys.scaler, 16, 16);
    init_i420_picture(&input, pixels, 16, 16, 7);
    init_i420_picture(&output, pixels, 16, 16, 0);
    sys.usm_pool = (usm_pool_t *)&filter;
    sys.usm_amount_q8 = 20;
    g_usm_effective_threads = 2;
    CHECK(ApplyUsmIfEnabled(&filter, &sys, &output) == UP_USM_APPLY_OK);
    CHECK(g_usm_apply_calls == 1 && sys.usm_workers_logged == 1);

    g_usm_apply_status = UP_USM_APPLY_FAILED_UNCHANGED;
    CHECK(ApplyUsmIfEnabled(&filter, &sys, &output)
          == UP_USM_APPLY_FAILED_UNCHANGED);
    CHECK(sys.usm_pool == NULL && sys.usm_amount_q8 == 0);
    CHECK(g_usm_destroy_calls == 1);

    sys.usm_pool = (usm_pool_t *)&filter;
    sys.usm_amount_q8 = 20;
    g_usm_apply_status = UP_USM_APPLY_OUTPUT_UNCERTAIN;
    CHECK(FinishScaledFrame(&filter, &sys, &input, &output) == NULL);
    CHECK(input.releases == 1 && output.releases == 1);
    CHECK(g_usm_destroy_calls == 2);
    END();
}

static void test_adaptive_configuration(void)
{
    BEGIN("adaptive USM is opt-in and respects explicit workers");
    filter_t filter;
    reset_state();
    init_filter(&filter);
    set_config("autoupscale-usm", 20);
    set_config("autoupscale-adaptive-usm", 1);
    g_usm_pool_create_result = (usm_pool_t *)&filter;
    CHECK(up_autoupscale_open_checked((vlc_object_t *)&filter) == VLC_SUCCESS);
    CHECK(!filter.p_sys->usm_adaptive.enabled);
    Close((vlc_object_t *)&filter);
    set_config("autoupscale-threads", 0);
    CHECK(up_autoupscale_open_checked((vlc_object_t *)&filter) == VLC_SUCCESS);
    CHECK(filter.p_sys->usm_adaptive.tuner.limit >= 1);
    CHECK(filter.p_sys->usm_adaptive.enabled
          == (filter.p_sys->usm_adaptive.tuner.limit > 1));
    CHECK(filter.p_sys->usm_adaptive.tuner.current == g_usm_requested_threads);
    Close((vlc_object_t *)&filter);
    END();
}

static void test_adaptive_trial_drop(void)
{
    BEGIN("uncertain adaptive trial drops one frame and preserves baseline");
    uint8_t pixels[384] = { 0 };
    filter_t filter;
    filter_sys_t sys = { 0 };
    picture_t input, output;
    reset_state();
    init_filter(&filter);
    init_scaler(&sys.scaler, 16, 16);
    init_i420_picture(&input, pixels, 16, 16, 7);
    init_i420_picture(&output, pixels, 16, 16, 0);
    sys.usm_pool = (usm_pool_t *)&filter;
    sys.usm_amount_q8 = 20;
    up_usm_adaptive_init(&sys.usm_adaptive, 1, 2, 16, 16, 8);
    sys.usm_adaptive.tuner.current = 2;
    g_usm_pool_create_result = (usm_pool_t *)&output;
    g_usm_apply_status = UP_USM_APPLY_OUTPUT_UNCERTAIN;
    CHECK(FinishScaledFrame(&filter, &sys, &input, &output) == NULL);
    CHECK(input.releases == 1 && output.releases == 1);
    CHECK(sys.usm_pool == (usm_pool_t *)&filter && sys.usm_amount_q8 == 20);
    CHECK(g_usm_destroy_calls == 1);
    g_usm_apply_status = UP_USM_APPLY_OK;
    CHECK(ApplyUsmIfEnabled(&filter, &sys, &output) == UP_USM_APPLY_OK);
    DisableUsm(&sys);
    CHECK(g_usm_destroy_calls == 2);
    END();
}

static void test_probe_lifecycle(void)
{
    BEGIN("probe validates views, closes its window, and retires grainy USM");
    uint8_t pixels[384] = { 0 };
    filter_t filter;
    filter_sys_t sys = { 0 };
    picture_t valid;
    picture_t invalid;
    reset_state();
    init_filter(&filter);
    init_scaler(&sys.scaler, 16, 16);
    init_i420_picture(&valid, pixels, 16, 16, 0);
    init_picture(&invalid, 0);
    fill_checkerboard(pixels, 16, 16);
    sys.probe.active = 1;
    sys.probe.advice = 1;
    sys.usm_pool = (usm_pool_t *)&filter;
    sys.usm_amount_q8 = 20;
    sys.usm_sharp_threshold = 1;
    RunProbe(&filter, &sys, &invalid);
    CHECK(sys.probe.accum.frames == 0);
    RunProbe(&filter, &sys, &valid);
    CHECK(sys.probe.accum.frames == 1);
    sys.probe.accum.frames = UP_PROBE_WINDOW_FRAMES - 1;
    RunProbe(&filter, &sys, &valid);
    CHECK(sys.probe.active == 0 && sys.usm_skip_sharp == 1);
    CHECK(sys.usm_pool == NULL && g_usm_destroy_calls == 1);

    sys.probe.accum = (up_probe_accum_t) {
        .lap_samples = UP_PROBE_MIN_SAMPLES_PER_KIND,
        .edge_sum = (uint64_t)(UP_PROBE_THRESH_BLOCKY_EDGE_MEAN + 1)
                    * UP_PROBE_MIN_SAMPLES_PER_KIND,
        .edge_samples = UP_PROBE_MIN_SAMPLES_PER_KIND,
        .frames = UP_PROBE_MIN_FRAMES,
    };
    sys.probe.advice_logged = 0;
    LogProbeVerdict(&filter, &sys);
    CHECK(sys.probe.advice_logged == 1);
    LogProbeVerdict(&filter, &sys);

    END();
}

static void test_runtime_fallback(void)
{
    BEGIN("runtime fallback retires failed backends");
    filter_t filter;
    filter_sys_t sys = { 0 };
    reset_state();
    init_filter(&filter);
    init_scaler(&sys.scaler, 16, 16);
    sys.backend_pref = SCALER_BACKEND_AUTO;
    TryBackendFallback(&filter, &sys);
    CHECK(sys.scaler.backend == &fake_swscale);
    CHECK(g_zimg_close_calls == 1 && g_swscale_open_calls == 1);

    reset_state();
    init_filter(&filter);
    memset(&sys, 0, sizeof(sys));
    init_scaler(&sys.scaler, 16, 16);
    sys.backend_pref = SCALER_BACKEND_ZIMG;
    TryBackendFallback(&filter, &sys);
    CHECK(sys.scaler.backend == NULL && g_zimg_close_calls == 1);

    reset_state();
    init_filter(&filter);
    memset(&sys, 0, sizeof(sys));
    init_scaler(&sys.scaler, 16, 16);
    sys.backend_pref = SCALER_BACKEND_AUTO;
    g_swscale_open_result = -1;
    TryBackendFallback(&filter, &sys);
    CHECK(sys.scaler.backend == NULL && g_swscale_open_calls == 1);

    END();
}

int main(void)
{
    test_success_copies_properties_and_tears_down();
    test_cpu_gate_rejects_before_open();
    test_output_allocation_failure_releases_input();
    test_transient_failure_keeps_backend();
    test_fatal_failure_falls_back_once();
    test_open_rejections_and_open_fallback();
    test_open_usm_contract();
    test_open_usm_worker_policy();
    test_usm_success_failure_and_uncertain_output();
    test_adaptive_configuration();
    test_adaptive_trial_drop();
    test_probe_lifecycle();
    test_runtime_fallback();
    return test_harness_report();
}
